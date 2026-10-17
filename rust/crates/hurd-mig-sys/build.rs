/*
 * Build script for hurd-mig-sys: run bindgen over include/mach_ffi.h and emit a
 * no_std-friendly bindings.rs into OUT_DIR.
 *
 * Design goals, in order of importance:
 *
 *  1. ALWAYS produce bindings. A `*-sys` crate that fails to build when the
 *     Hurd headers are missing is unusable for a first `cargo check`, in CI, or
 *     on a machine that only wants to read the code. include/mach_ffi.h
 *     therefore carries a self-contained fallback, so bindgen always has
 *     something to parse.
 *
 *  2. Use the REAL headers when they are available, so struct layouts and
 *     enumerations come from the authoritative source instead of from our
 *     summary. Discovery order, first hit wins:
 *       - $HURD_INCLUDE         (explicit, colon-separated list)
 *       - $HURD_SRC/hurd        (a source checkout: .../hurd/gnumach/...)
 *       - /usr/include/i386-gnu/hurd          (Debian cross headers)
 *       - /usr/local/include/i386-gnu/hurd
 *
 *  3. no_std. Every one of `use_core()`, `ctypes_prefix()` and the allow-list
 *     below exists purely to stop bindgen emitting `::std::` paths. Forgetting
 *     `use_core()` is the single most common reason a generated crate fails to
 *     compile in a #![no_std] crate, with an error about `std::os::raw` that
 *     looks unrelated to what you just did.
 *
 *  4. No shelling out to the bindgen CLI. Using the library means the build
 *     does not depend on a separately installed `bindgen-cli` being the same
 *     version; `cargo install bindgen-cli` is only needed for the interactive
 *     workflow described in README.md.
 *
 * Environment variables understood here:
 *   HURD_INCLUDE    colon-separated list of include roots
 *   HURD_SRC        path to the hurd source checkout
 *   MIG_DEFS        directory holding MIG-generated headers (optional)
 *   BINDGEN_EXTRA   extra `-- clang` arguments, space separated
 */

use std::env;
use std::path::{Path, PathBuf};

/// Root of this crate.
fn manifest_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
}

/// Candidate include roots, in discovery order.
fn candidate_includes() -> Vec<PathBuf> {
    let mut out: Vec<PathBuf> = Vec::new();

    if let Ok(list) = env::var("HURD_INCLUDE") {
        for entry in list.split(':') {
            if !entry.is_empty() {
                out.push(PathBuf::from(entry));
            }
        }
    }

    if let Ok(src) = env::var("HURD_SRC") {
        // hurd/hurd/gnumach holds <mach/*.h>; the parent of hurd/hurd also
        // holds mach/machine/*.h through the same directory.
        out.push(PathBuf::from(&src).join("hurd/gnumach"));
        out.push(PathBuf::from(&src).join("hurd"));
    }

    out.push(PathBuf::from("/usr/include/i386-gnu/hurd"));
    out.push(PathBuf::from("/usr/local/include/i386-gnu/hurd"));

    out
}

/// A directory counts as a Mach include root if it contains <mach/mach.h>.
fn is_mach_include(dir: &Path) -> bool {
    dir.join("mach").join("mach.h").is_file()
}

/// True if `dir` looks like it holds MIG-generated headers (<mach/server.h>).
fn is_mig_include(dir: &Path) -> bool {
    dir.join("mach").join("server.h").is_file()
}

fn main() {
    let manifest = manifest_dir();

    println!("cargo:rerun-if-changed=include/mach_ffi.h");
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=HURD_INCLUDE");
    println!("cargo:rerun-if-env-changed=HURD_SRC");
    println!("cargo:rerun-if-env-changed=MIG_DEFS");
    println!("cargo:rerun-if-env-changed=BINDGEN_EXTRA");

    // ---------------------------------------------------------------
    // Locate the headers.
    // ---------------------------------------------------------------
    let candidates = candidate_includes();
    let mut includes: Vec<PathBuf> = Vec::new();
    for dir in &candidates {
        if is_mach_include(dir) {
            println!("cargo:rustc-env=HURD_MACH_INCLUDE={}", dir.display());
            println!("cargo:rerun-if-changed={}", dir.display());
            includes.push(dir.clone());
        }
    }

    let have_hurd = !includes.is_empty();

    // MIG-generated headers are a separate, optional lookup: they are not
    // installed by the distro packages and usually only exist in a build tree.
    let mut have_mig = false;
    if let Ok(defs) = env::var("MIG_DEFS") {
        let dir = PathBuf::from(defs);
        if is_mig_include(&dir) {
            println!("cargo:rustc-env=HURD_MIG_INCLUDE={}", dir.display());
            includes.push(dir.clone());
            have_mig = true;
        } else {
            println!(
                "cargo:warning=MIG_DEFS={} does not contain mach/server.h; \
                 continuing without MIG-generated declarations",
                dir.display()
            );
        }
    }

    if !have_hurd {
        println!(
            "cargo:warning=no GNU Mach headers found; building against the \
             self-contained fallback in include/mach_ffi.h. Set HURD_INCLUDE \
             (e.g. to /usr/include/i386-gnu/hurd) or HURD_SRC (a hurd source \
             checkout) to generate bindings from the real headers."
        );
    }

    // ---------------------------------------------------------------
    // Configure bindgen.
    // ---------------------------------------------------------------
    let header = manifest.join("include").join("mach_ffi.h");

    let mut builder = bindgen::Builder::default()
        .header(header.to_string_lossy())
        // Tell the umbrella header which branch of its #if to take.
        .clang_arg(if have_hurd {
            "-DHAVE_HURD_HEADERS"
        } else {
            "-UHAVE_HURD_HEADERS"
        })
        .clang_arg(if have_mig {
            "-DHAVE_MIG_HEADERS"
        } else {
            "-UHAVE_MIG_HEADERS"
        });

    for dir in &includes {
        builder = builder.clang_arg(format!("-I{}", dir.display()));
    }

    if let Ok(extra) = env::var("BINDGEN_EXTRA") {
        for arg in extra.split_whitespace() {
            builder = builder.clang_arg(arg);
        }
    }

    // -- no_std plumbing (see goal 3 above) --------------------------
    builder = builder
        // Emit `::core::...` instead of `::std::...`.
        .use_core()
        // The type used for c_char/c_int/... must exist in core, not std.
        .ctypes_prefix("::core::ffi")
        // The generated file is a module body, not its own crate root.
        .raw_line("#![allow(non_upper_case_globals)]")
        .raw_line("#![allow(non_camel_case_types)]")
        .raw_line("#![allow(non_snake_case)]")
        .raw_line("#![allow(dead_code)]")
        .raw_line("#![allow(clippy::all)]")
        // Keep the file small and diff-friendly: comments and c_naming noise
        // add nothing here, since the header is the readable source of truth.
        .generate_comments(false)
        .prepend_enum_name(false)
        // Mach is full of anonymous unions/structs; keep the MSRV low by not
        // demanding Rust 1.49+ layout-test features.
        .layout_tests(true);

    let bindings = builder
        .generate()
        .expect("bindgen failed; is clang/libclang installed? (apt install clang libclang-dev)");

    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR is always set by cargo"));
    let out_file = out_dir.join("bindings.rs");
    bindings
        .write_to_file(&out_file)
        .expect("failed to write bindings.rs");

    println!(
        "cargo:warning=hurd-mig-sys: generated {} from {} include root(s) ({})",
        out_file.display(),
        includes.len(),
        if have_hurd { "real Mach headers" } else { "fallback" }
    );
}