fn main() {
    // Expose the build target so the binary can print which ABI it was built
    // for. Cargo provides TARGET to build scripts, not to the crate itself.
    println!(
        "cargo:rustc-env=HURD_EXAMPLE_TARGET={}",
        std::env::var("TARGET").unwrap_or_else(|_| "unknown".to_string())
    );
    println!("cargo:rerun-if-changed=build.rs");
}
