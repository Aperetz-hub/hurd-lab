#!/bin/sh
# Build 2025-01 virtio rump libs for Hurd (libpci_virtio, libvirtio_if_vioif).
#
# Recipe: stock component_simple.c (WITH ioconf.c inside, tables defined
# once) + driver .c files compiled with _MODULE undefined (so they do NOT
# re-include ioconf.c and their modcmd is stubbed; registration happens
# through the RUMP_COMPONENT init).  Mirrors how working drivers (wm) link.
#
# FIX (curlwp/curcpu): rump kernel code MUST use the rump <machine/cpu.h>
# (curlwp == rump_curlwp_fast(), curcpu() == curlwp->l_cpu), NOT the arch
# <machine/cpu.h> (curlwp == x86_curlwp() via %gs, which rump never sets).
# Upstream sys/rump/Makefile.rump puts -I${RUMPTOP}/include FIRST for this
# reason, and defines -D_RUMPKERNEL.  But the arch PCI headers (pcivar.h ->
# <machine/pci_machdep.h> -> <x86/pci_machdep.h>) need intr_handle_t, which
# only the arch <machine/intr.h> provides.  So we interpose an override dir
# ($HOME/ovr) that maps machine/cpu.h -> rump and machine/intr.h -> arch,
# placed first in the include path.
#
# Symptom before the fix: vioif0 attaches, then SIGSEGV in
# vioif_set_rx_filter -> vioif_ctrl_acquire -> curlwp, faulting on %gs:0xac0
# (gs base 0, since rump never sets %gs on Hurd).  Backtrace obtained by
# running the rumpnet translator under gdb.
export LC_ALL=C
T=$HOME/nbsrc/obj/tooldir/bin
export PATH=$T:$PATH
R=$HOME/nbsrc25/sys/rump
C=$HOME/nbsrc25/common
S=$HOME/nbsrc25/sys
OVR=$HOME/ovr

mkdir -p $OVR/machine
ln -sf $R/include/machine/cpu.h $OVR/machine/cpu.h
ln -sf $S/arch/amd64/include/intr.h $OVR/machine/intr.h

# Non-include flags only (include dirs are added per-lib in build_lib).
FLAGS="-O2 -g -fcommon -D_KERNEL -D_RUMPKERNEL -ffreestanding -fno-strict-aliasing -mno-red-zone -mno-mmx -mno-sse -mno-avx -msoft-float -std=gnu99 -Wno-error -D_FILE_OFFSET_BITS=64 -DRUMP_REGISTER_T=int -DRUMPUSER_CONFIG=yes -DNO_PCI_MSI_MSIX=yes -DNUSB_DMA=1 -DPAE -DBUFPAGES=16 -include $HOME/pri-shim.h -DCOMPAT_50 -DCOMPAT_60 -DCOMPAT_70 -DCOMPAT_80 -DCOMPAT_90 -DCOMPAT_100 -DCOMPAT_110 -nostdinc -DDIAGNOSTIC -DKTRACE -imacros $R/include/opt/opt_rumpkernel.h -imacros $HOME/unmodule.h"

build_lib() {
    L=$1
    shift
    D=$R/dev/lib/$L
    INCS="-I$OVR -I$R/include -I$D -I. -I$R/librump/rumpkern -I$C/include -I$R/include/opt -I$S/arch -I$S"
    cd $D || return 1
    rm -f *.o *.pico *.d .depend* ioconf.c ioconf.h locators.h cs_nolocal* glue* virtio_pci_glue*
    case $L in
        libpci_virtio) IONAME=PCI_VIRTIO.ioconf ;;
        libvirtio_if_vioif) IONAME=VIRTIO_IF_VIOIF.ioconf ;;
    esac
    config -b . -s $S $IONAME >/dev/null 2>&1 || { echo "CONFIG FAIL $L"; return 1; }
    for s in "$@"; do
        b=$(basename $s .c)
        gcc $FLAGS $INCS -fPIC -c $s -o $b.pico || { echo "COMPILE FAIL: $s"; return 1; }
    done
    for o in *.pico; do
        nm -go $o | awk '$NF !~ /^(rump|RUMP|__|_GLOBAL_OFFSET_TABLE)/ {s=$NF;sub(/^/,"&rumpns_",s); print $NF,s}' | sort | uniq > renametab
        objcopy --redefine-syms renametab $o
    done
    rm -f renametab librumpdev_${L#lib}_pic.a
    ar crs librumpdev_${L#lib}_pic.a *.pico && echo "OK $L: $(ar t librumpdev_${L#lib}_pic.a | tr '\n' ' ')"
}

build_lib libpci_virtio $S/dev/pci/virtio.c $S/dev/pci/virtio_pci.c $R/dev/lib/component_simple.c
build_lib libvirtio_if_vioif $S/dev/pci/if_vioif.c $R/dev/lib/component_simple.c
cp $R/dev/lib/libpci_virtio/librumpdev_pci_virtio_pic.a $R/dev/lib/libvirtio_if_vioif/librumpdev_virtio_if_vioif_pic.a ~/rumplib/ && echo LIBS_OK
