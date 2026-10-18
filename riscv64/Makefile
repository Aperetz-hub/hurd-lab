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
#   make selftest   exercise the fault paths and the PLIC interrupt on purpose
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
PYTHON     ?= python3

# Host-side helper for the PLIC self test: it injects a byte into the guest's
# serial receive register, which is what makes the UART assert its interrupt.
VMTOOLS    := ../vm

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

SRCS_C := src/main.c src/console.c src/sbi.c src/uart.c src/trap.c src/timer.c \
           src/pmm.c src/vm.c src/hsm.c src/plic.c
SRCS_S := src/boot.S src/trap.S
# C and assembly objects go to different directories: src/trap.c and
# src/trap.S would otherwise share one object name.
OBJS   := $(SRCS_C:%.c=$(BUILD)/c/%.o) $(SRCS_S:%.S=$(BUILD)/s/%.o)

# The banners the milestone is defined by; `make verify` greps for them.
BANNER := hurd-riscv64: milestone 4 reached, halting
BANNER1 := hurd-riscv64: milestone 3 reached

.PHONY: all check build run verify selftest selftest-plic selftest-build debug disasm clean

all: build

check:
	@echo "== toolchain =="
	@command -v $(CC) >/dev/null || { echo "MISSING: $(CC) -> install gcc-riscv64-linux-gnu"; exit 1; }
	@command -v $(QEMU) >/dev/null || { echo "MISSING: $(QEMU) -> install qemu-system-misc"; exit 1; }
	@command -v $(OBJDUMP) >/dev/null || { echo "MISSING: $(OBJDUMP)"; exit 1; }
	@command -v $(READELF) >/dev/null || { echo "MISSING: $(READELF)"; exit 1; }
	@command -v $(PYTHON) >/dev/null || { echo "MISSING: $(PYTHON) -> install python3"; exit 1; }
	@test -f $(VMTOOLS)/riscv-uart-inject.py || { echo "MISSING: $(VMTOOLS)/riscv-uart-inject.py"; exit 1; }
	@echo "cc      : $$($(CC) --version | head -1)"
	@echo "as/ld   : $$($(CROSS)ld --version | head -1)"
	@echo "qemu    : $$($(QEMU) --version | head -1)"
	@echo "target  : $$($(CC) -dumpmachine)"
	@echo "OK"

$(BUILD)/c/%.o: %.c | $(BUILD)/.stamp
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/s/%.o: %.S | $(BUILD)/.stamp
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

# Boot the milestone at every hart count the board supports, and require:
#
#   - the final banner exactly once per run: more than once would mean a second
#     hart reached kmain;
#   - exactly N harts reported running at -smp N, so every hart of the board
#     really came up with the MMU live, not just the one that printed;
#   - no hart reported an error.
#
# Each count is booted several times, because the boot hart the firmware picks
# is not always the same one, and a single run per count does not exercise that.
ROUNDS ?= 3

verify: build
	@fail=0; \
	for n in 1 2 3 4; do \
		ok=0; \
		for i in $$(seq 1 $(ROUNDS)); do \
			out=$$(timeout 30 $(QEMU) -machine virt -m $(MEM) -nographic \
				-no-reboot -smp $$n -bios default -kernel $(BIN) 2>&1); \
			banners=$$(printf '%s\n' "$$out" | grep -c '$(BANNER)'); \
			up=$$(printf '%s\n' "$$out" | grep -c '^hsm: hart .* running'); \
			bad=$$(printf '%s\n' "$$out" | grep -c 'NOT running'); \
			if [ "$$banners" = 1 ] && [ "$$up" = "$$n" ] && [ "$$bad" = 0 ]; then \
				ok=$$((ok+1)); \
			else \
				printf '  -smp %s : FAIL on round %s, banner x%s, %s/%s harts up, %s errors\n' \
					"$$n" "$$i" "$$banners" "$$up" "$$n" "$$bad"; \
				printf '%s\n' "$$out" | grep -E '^hsm:|^trap:|^plic:|^vm: NOT' \
					| sed 's/^/      /'; \
			fi; \
		done; \
		if [ "$$ok" = $(ROUNDS) ]; then \
			printf '  -smp %s : OK on %s/%s runs, %s harts up each time\n' \
				"$$n" "$$ok" "$(ROUNDS)" "$$n"; \
		else \
			printf '  -smp %s : FAIL, %s/%s runs\n' "$$n" "$$ok" "$(ROUNDS)"; \
			fail=1; \
		fi; \
	done; \
	exit $$fail

# Exercise the fault paths and the interrupt path on purpose.
#
#   exception  raises a deliberate illegal instruction.  The kernel stops on the
#              first fault it does not expect, so this one gets a build of its
#              own.
#   rodata     stores to the read-only data, which under Sv39 must raise a
#              store/AMO page fault (cause 15).  Also stops there.
#   plic       lets a host-injected byte arrive in the UART, which must produce a
#              supervisor external interrupt that is claimed and completed
#              through the PLIC.  Unlike the two above it stops on no fault at
#              all, so it runs the whole bring-up and then parks.
#
# All three are built from the same sources, with -DSELFTEST_... selecting which.
SELFTEST_LDFLAGS = $(BUILD)/hurd-riscv64-selftest.elf

selftest: build
	@fail=0; \
	for mode in exception rodata; do \
		$(MAKE) --no-print-directory selftest-build SELFTEST_MODE=$$mode; \
		out=$$(timeout 30 $(QEMU) -machine virt -m $(MEM) -nographic \
			-no-reboot -bios default -kernel $(SELFTEST_LDFLAGS) 2>&1); \
		if printf '%s\n' "$$out" | grep -q 'selftest: PASS'; then \
			echo "  selftest $$mode: PASS"; \
			printf '%s\n' "$$out" | grep -E 'selftest|trap: ' | sed 's/^/    /'; \
		else \
			echo "  selftest $$mode: FAIL"; \
			printf '%s\n' "$$out" | tail -3 | sed 's/^/    /'; \
			fail=1; \
		fi; \
	done; \
	$(MAKE) --no-print-directory selftest-plic || fail=1; \
	exit $$fail

# The PLIC self test needs a byte to arrive in the guest's serial line, and the
# guest can only ever write to that line, so the byte has to come from the host.
# QEMU is therefore given a unix socket as its serial chardev and the injector
# connects to it as the client, which puts the guest's console output in the log
# file instead of on stdio.
#
# The injector is started first and waits for QEMU to create the socket, so the
# two do not race.  The kernel prints a trigger line and then waits for the
# interrupt, so the byte cannot be sent before the guest is ready for it: the
# exchange is driven by the guest, not by a timeout on this side.
#
# One --send pair per trigger, not one per trial.  A pair fires every time its
# trigger appears, and the PLIC self test prints PLIC-TRIAL once per candidate
# context it tries; the search cannot know in advance how many that will be,
# which is why the pair is repeatable rather than one-shot.
selftest-plic: build
	@$(MAKE) --no-print-directory selftest-build SELFTEST_MODE=plic; \
	log=$(BUILD)/plic-selftest.log; \
	sock=$(BUILD)/plic-selftest.sock; \
	rm -f $$log $$sock; \
	$(PYTHON) $(VMTOOLS)/riscv-uart-inject.py --socket $$sock --log $$log \
		--send "PLIC-INJECT-1=Z" --send "PLIC-TRIAL=Y" & \
	inj=$$!; \
	timeout 30 $(QEMU) -machine virt -m $(MEM) -monitor none -display none \
		-no-reboot -bios default \
		-serial unix:$$sock,server=on,wait=off \
		-kernel $(SELFTEST_LDFLAGS) >/dev/null 2>&1; \
	rc=$$?; wait $$inj; \
	if grep -q 'selftest: PASS' $$log 2>/dev/null; then \
		echo "  selftest plic: PASS"; \
		grep -E 'selftest|plic:|injected' $$log | sed 's/^/    /'; \
		exit 0; \
	fi; \
	echo "  selftest plic: FAIL (qemu exit $$rc)"; \
	tail -5 $$log 2>/dev/null | sed 's/^/    /'; \
	exit 1

selftest-build:
	@mkdir -p $(BUILD)/selftest
ifeq ($(SELFTEST_MODE),rodata)
	$(CC) $(CFLAGS) -DSELFTEST_RODATA_FAULT -c -o $(BUILD)/selftest/main.o src/main.c
else ifeq ($(SELFTEST_MODE),plic)
	$(CC) $(CFLAGS) -DSELFTEST_PLIC -c -o $(BUILD)/selftest/main.o src/main.c
else
	$(CC) $(CFLAGS) -DSELFTEST_ILLEGAL_INSTRUCTION -c -o $(BUILD)/selftest/main.o src/main.c
endif
	$(CC) $(LDFLAGS) -o $(SELFTEST_LDFLAGS) \
		$(BUILD)/selftest/main.o $(filter-out $(BUILD)/c/src/main.o,$(OBJS)) -lgcc

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