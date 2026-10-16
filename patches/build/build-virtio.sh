#!/bin/sh
# Build 2025-01 virtio rump libs for Hurd (test).
export LC_ALL=C
export PATH=$HOME/nbsrc/obj/tooldir/bin:$PATH
R=~/nbsrc25/sys/rump
C=$HOME/nbsrc25/common
S=$HOME/nbsrc25/sys
F="-O2 -fcommon -D_KERNEL -ffreestanding -fno-strict-aliasing -mno-red-zone -mno-mmx -mno-sse -mno-avx -msoft-float -std=gnu99 -Wno-error=implicit-function-declaration -Wno-error=implicit-int -Wno-error=unused-const-variable -Wno-error=unused-variable -D_FILE_OFFSET_BITS=64 -DRUMP_REGISTER_T=int -DRUMPUSER_CONFIG=yes -DNO_PCI_MSI_MSIX=yes -DNUSB_DMA=1 -DPAE -DBUFPAGES=16 -include /home/demo/pri-shim.h -I$R/librump/rumpkern -DCOMPAT_50 -DCOMPAT_60 -DCOMPAT_70 -DCOMPAT_80 -DCOMPAT_90 -DCOMPAT_100 -DCOMPAT_110 -nostdinc -I$R/include -I$R/include/opt -I$S/arch -I$S -I$C/include -DDIAGNOSTIC -DKTRACE -imacros $R/include/opt/opt_rumpkernel.h -imacros /home/demo/unmodule.h"

build_lib() {
    L=$1
    shift
    D=$R/dev/lib/$L
    cd $D || return 1
    cd $D || return 1
    rm -f *.o *.pico *.po .depend* ioconf.c ioconf.h locators.h
    # regenerate ioconf via config
    IONAME=""
    case $L in
        libpci_virtio) IONAME=PCI_VIRTIO.ioconf ;;
        libvirtio_if_vioif) IONAME=VIRTIO_IF_VIOIF.ioconf ;;
    esac
    config -b . -s $S $IONAME >/dev/null 2>&1
    for s in "$@"; do
        b=$(basename $s .c)
        gcc $F -I$D -I. -fPIC -c $s -o $b.pico || { echo "COMPILE FAIL: $s"; return 1; }
    done
    for o in *.pico; do
        nm -go $o | awk '$NF!~/^(rump|RUMP|__|_GLOBAL_OFFSET_TABLE)/ {s=$NF;sub(/^/,"&rumpns_",s); print $NF,s}' | sort | uniq > renametab
        objcopy --redefine-syms renametab $o
    done
    rm -f renametab
    rm -f librumpdev_${L#lib}_pic.a
    ar crs librumpdev_${L#lib}_pic.a *.pico && echo "$L OK: $(ar t librumpdev_${L#lib}_pic.a | tr '\n' ' ')"
}

build_lib libpci_virtio $S/dev/pci/virtio.c $S/dev/pci/virtio_pci.c $R/dev/lib/component_simple.c
build_lib libvirtio_if_vioif $S/dev/pci/if_vioif.c $R/dev/lib/component_simple.c
