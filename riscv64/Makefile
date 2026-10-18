# Build/run tooling for the bare-metal rv64gc kernel skeleton.
#
# Everything is driven by cargo; this Makefile only wraps the commands that are
# awkward to type, so `make` and `cargo build` never disagree about flags (those
# live in .cargo/config.toml and rust-toolchain.toml).
#
#   make check    verify the toolchain is present (fails early, readably)
#   make build    release ELF into target/
#   make build-std  same, but rebuild core from source (-Z build-std)
#   make run      boot it under qemu-system-riscv64 (headless, serial on stdio)
#   make debug    boot it stopped on the OpenSBI debug server (:3333)
#   make disasm   dump the image layout and the entry point
#   make clean

TARGET      := riscv64gc-unknown-none-elf
PROFILE     ?= release
BIN         := target/$(TARGET)/$(PROFILE)/hurd-riscv64-boot
QEMU        ?= qemu-system-riscv64
MEM         ?= 256M
CARGO       ?= cargo

# The host binutils cannot disassemble RISC-V ("cannot disassemble for
# architecture UNKNOWN"), and readelf localises its output, so "grep -i
# 'entry point'" only matches under LC_ALL=C. Prefer the cross binutils and
# fall back to the host ones; LC_ALL=C keeps the grep locale-independent.
READELF     ?= $(shell command -v riscv64-linux-gnu-readelf 2>/dev/null || command -v readelf)
OBJDUMP     ?= $(shell command -v riscv64-linux-gnu-objdump 2>/dev/null || command -v objdump)
export LC_ALL = C

.PHONY: all check build build-std run debug disasm clean

all: build

check:
	@echo "== toolchain =="
	@command -v rustup   >/dev/null || { echo "MISSING: rustup (https://rustup.rs)"; exit 1; }
	@command -v rustc    >/dev/null || { echo "MISSING: rustc"; exit 1; }
	@command -v $(CARGO) >/dev/null || { echo "MISSING: cargo"; exit 1; }
	@command -v $(QEMU)  >/dev/null || { \
		echo "MISSING: $(QEMU)  ->  sudo apt install qemu-system-misc"; exit 1; }
	@echo "rustc  : $$(rustc --version)"
	@echo "cargo  : $$($(CARGO) --version)"
	@echo "target : $$(rustc --print target-list | grep -c '^$(TARGET)$$') installed? (see make build)"
	@echo "qemu   : $$($(QEMU) --version | head -1)"
	@echo "OK"

build:
	$(CARGO) build --profile $(PROFILE)
	@echo "built: $(BIN)"

# Rebuild core/compiler_builtins from source. Only needed on a toolchain that
# ships no prebuilt core for this target, or when hacking on core itself.
build-std:
	$(CARGO) +nightly build -Z build-std=core,compiler_builtins \
		-Z build-std-features=compiler-builtins-mem --profile $(PROFILE)
	@echo "built: $(BIN)"

# Headless by design, matching AGENTS.md rule 2 (no GUI anywhere in this lab).
# -nographic multiplexes the UART onto stdio; -bios default is QEMU's bundled
# OpenSBI; -kernel loads our ELF at 0x80200000 (the address in the linker
# script).
run: build
	$(QEMU) -machine virt -m $(MEM) -nographic -no-reboot \
		-bios default \
		-kernel $(BIN)

# Start stopped so a debugger can attach before the first instruction.
# OpenSBI exposes the debug spec on TCP 3333; gdb-multiarch speaks the same
# RISC-V remote protocol.
debug: build
	$(QEMU) -machine virt -m $(MEM) -nographic -no-reboot \
		-bios default -S \
		-kernel $(BIN)

disasm: build
	@echo "== program headers =="
	@$(READELF) -lW $(BIN) | sed -n '1,25p'
	@echo
	@echo "== entry point =="
	@$(READELF) -hW $(BIN) | grep -i "entry point"
	@echo
	@echo "== first instructions =="
	@$(OBJDUMP) -d $(BIN) | sed -n '/<_start>:/,/^$$/p' | head -40

clean:
	$(CARGO) clean