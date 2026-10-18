# Build/run tooling for the bare-metal rv64gc kernel skeleton.
#
# The kernel is freestanding C plus one assembly trampoline, built with the
# GNU cross toolchain, exactly like GNU Mach itself: gcc, gas, ld, make.
# There is no other build system involved.
#
#   make check      verify the toolchain is present (fails early, readably)
#   make build      release ELF into build/
#   make run        boot it under qemu-system-riscv64 (headless, serial on stdio)
#   make verify     boot at -smp 1..4 and check the milestone banner appears
#   make debug      boot it stopped, so a debugger can attach first
#   make disasm     dump the image layout and the first instructions
#   make clean

CROSS      ?= riscv64-linux-gnu-
CC         := $(CROSS)gcc
OBJCOPY    := $(CROSS)objcopy
OBJDUMP    := $(CROSS)objdump
READELF    := $(CROSS)readelf
QEMU       ?= qemu-system-riscv64
MEM        ?= 256M

BUILD      := build
BIN        := $(BUILD)/hurd-riscv64-boot.elf
LD_SCRIPT  := linker/riscv64-virt.ld

# readelf localises its output ("Point d'entrée" here), so the "entry point"
# grep in `disasm` only matches under a fixed locale.
export LC_ALL = C

# rv64gc, doubles and long doubles enabled, so the ABI matches the string the
# banner prints. medany because the image lives at 0x8020_0000, above the
# signed 2 GiB reach of the medlow default.
ARCHFLAGS := -march=rv64gc -mabi=lp64d -mcmodel=medany

# Freestanding: no libc, no start files, no builtins the compiler would expect
# to be able to call, no PIC (the image is linked at a fixed address), and no
# unwind tables (there is no unwinder to consume them).
CFLAGS := $(ARCHFLAGS) \
	-std=gnu11 -O2 -g \
	-ffreestanding -nostdlib -nostartfiles \
	-fno-builtin -fno-common -fno-pic -fno-pie -no-pie \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-stack-protector -fno-strict-aliasing \
	-Wall -Wextra \
	-Iinclude

ASFLAGS := $(ARCHFLAGS) -g -Iinclude

# The linker script replaces the default layout, which assumes an OS loader.
# --gc-sections drops everything the entry point does not reach.
LDFLAGS := $(ARCHFLAGS) -nostdlib -no-pie -static \
	-T $(LD_SCRIPT) -Wl,--gc-sections \
	-Wl,-Map,$(BUILD)/hurd-riscv64-boot.map

SRCS_C := src/main.c src/console.c src/sbi.c src/uart.c
SRCS_S := src/boot.S
OBJS   := $(SRCS_C:%.c=$(BUILD)/%.o) $(SRCS_S:%.S=$(BUILD)/%.o)

# The banner the milestone is defined by; `make verify` greps for it.
BANNER := hurd-riscv64: milestone 1 reached, halting

.PHONY: all check build run verify debug disasm clean

all: build

check:
	@echo "== toolchain =="
	@command -v $(CC) >/dev/null || { echo "MISSING: $(CC) -> install gcc-riscv64-linux-gnu"; exit 1; }
	@command -v $(QEMU) >/dev/null || { echo "MISSING: $(QEMU) -> install qemu-system-misc"; exit 1; }
	@command -v $(OBJDUMP) >/dev/null || { echo "MISSING: $(OBJDUMP)"; exit 1; }
	@command -v $(READELF) >/dev/null || { echo "MISSING: $(READELF)"; exit 1; }
	@echo "cc      : $$($(CC) --version | head -1)"
	@echo "as/ld   : $$($(CROSS)ld --version | head -1)"
	@echo "qemu    : $$($(QEMU) --version | head -1)"
	@echo "target  : $$($(CC) -dumpmachine)"
	@echo "OK"

$(BUILD)/%.o: %.c | $(BUILD)/.stamp
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.S | $(BUILD)/.stamp
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c -o $@ $<

# The phony target is also called `build`, so the directory is created through a
# stamp file rather than a `build:` rule, which make would treat as circular.
$(BUILD)/.stamp:
	@mkdir -p $(BUILD)
	@touch $@

build: $(BIN)

$(BIN): $(OBJS) $(LD_SCRIPT)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) -lgcc
	@echo "built: $@"

# Headless by design, matching AGENTS.md rule 2 (no GUI anywhere in this lab).
# -nographic multiplexes the UART onto stdio; -bios default is QEMU's bundled
# OpenSBI; -kernel loads our ELF at 0x80200000 (the address in the linker
# script).
run: build
	$(QEMU) -machine virt -m $(MEM) -nographic -no-reboot \
		-bios default \
		-kernel $(BIN)

# Boot the milestone at every hart count the board supports and require the
# banner exactly once per run: more than once would mean a second hart ran
# the kernel. Each count is booted several times, because the boot hart that
# the firmware picks is not always the same one, and a single run per count
# does not exercise that.
ROUNDS ?= 3

verify: build
	@fail=0; \
	for n in 1 2 3 4; do \
		ok=0; \
		for i in $$(seq 1 $(ROUNDS)); do \
			out=$$(timeout 30 $(QEMU) -machine virt -m $(MEM) -nographic \
				-no-reboot -smp $$n -bios default -kernel $(BIN) 2>&1); \
			got=$$(printf '%s\n' "$$out" | grep -c '$(BANNER)'); \
			if [ "$$got" = 1 ]; then ok=$$((ok+1)); else \
				printf '  -smp %s : FAIL on round %s, banner x%s\n' \
					"$$n" "$$i" "$$got"; \
			fi; \
		done; \
		if [ "$$ok" = $(ROUNDS) ]; then \
			printf '  -smp %s : BOOT, banner x1 on %s/%s runs\n' \
				"$$n" "$$ok" "$(ROUNDS)"; \
		else \
			printf '  -smp %s : FAIL, %s/%s runs\n' "$$n" "$$ok" "$(ROUNDS)"; \
			fail=1; \
		fi; \
	done; \
	exit $$fail

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
	rm -rf $(BUILD)