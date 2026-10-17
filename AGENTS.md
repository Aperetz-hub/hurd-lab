# AGENTS.md — Project Rules (GNU/Hurd x86_64 fork lab)

Mandatory rules for every contributor or automated agent working in this
repository (`hurd-lab`). Nothing in this file overrides upstream project
policies; it governs only this fork-lab workspace.

## 1. Language: English only

- All documentation (`README.md`, `docs/*`, any other Markdown) **MUST** be
  written strictly in English — clear, idiomatic technical English.
- All code comments, script headers (shebang blocks), diagnostic/`echo`
  messages, and commit messages **MUST** be in English.
- Historical notes may keep verbatim quoted log lines, boot messages, and
  identifiers as they appeared (those are data, not prose).
- Existing French material is legacy: whenever you touch a file, translate its
  French prose into English in the same change.
- Keep file names as they are (e.g. do not rename `docs/suivi.md`, which is
  referenced from scripts and other documents); add an English alias/symlink
  instead if needed.

## 2. Headless console workflow only

- Pure text console + SSH. This project **NEVER** runs X11 in the guest:
  no Xorg, no `xdm`, no XFCE, no display managers. Do not install
  `xfce4`/`xdm`/Xorg packages on the VM.
- Boot logs are captured through the QEMU monitor (HMP), the VGA text buffer
  (`xp/4000cb 0xb8000`, see `vm/vgacap.py` / `vm/vgaprobe.py`), or
  `-serial file:`; there is no Mach serial console in normal use.

## 3. Strict VM disk safety

- **Never** boot or write to the base image `debian-hurd-amd64-*.img`
  (read-only reference image).
- **Never** re-extract or overwrite an image while it is referenced as the
  backing file of an existing overlay (this corrupted the dev disk once,
  see `docs/suivi.md` § *INCIDENT*).
- All risky experiments (`gnumach` SMP trials, `rumpnet`/`rumpdisk` tests,
  network switching) run on **disposable overlays** (`hurd-*-test.qcow2`,
  recreated at each launch), never on the dev disk `hurd-dev.qcow2` itself.
- Keep a known-good snapshot copy before touching anything
  (e.g. `hurd-dev.qcow2.good-YYYYMMDD`), taken with the VM cleanly shut down.
- QEMU launchers in `vm/` enforce these rules by construction
  (`-m 4G`, autonomous working disk, disposable overlays). Do not weaken them.

## 4. Multi-agent coexistence

- Other agents may be running VM tests at the same time: **do not** launch
  QEMU, GDB, or the `vm/run-*.sh` scripts, and do not touch any `.qcow2` or
  `.img` file unless your task explicitly requires it and you hold the VM.
- Documentation/comment edits must not alter shell logic, variable names,
  file paths, or functional code.
- Inside `patches/*.patch` / `*.diff`, only the commit message/header
  description at the top may be edited. Never modify anything inside the
  `@@ ... @@` hunks (would corrupt line offsets and break `git apply`).
- Before pushing: `git pull --rebase` first in case another agent pushed new
  findings.
