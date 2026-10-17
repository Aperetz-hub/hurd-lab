#!/bin/sh
# Relink rumpnet with the freshly built virtio rump libs.
# Non-PIE so crashes symbolize correctly with addr2line.
set -e
export LC_ALL=C
cd "$HOME/hurd-build/rumpnet"
gcc -L. -L../libfshelp/ -L../libhurdbugaddr/ -L../libihash/ -L../libiohelp/ \
    -L../libirqhelp/ -L../libmachdev/ -L../libports/ -L../libshouldbeinlibc/ \
    -L../libtrivfs/ \
    -Wl,-rpath-link=../libfshelp/ -Wl,-rpath-link=../libhurdbugaddr/ \
    -Wl,-rpath-link=../libihash/ -Wl,-rpath-link=../libiohelp/ \
    -Wl,-rpath-link=../libirqhelp/ -Wl,-rpath-link=../libmachdev/ \
    -Wl,-rpath-link=../libports/ -Wl,-rpath-link=../libshouldbeinlibc/ \
    -Wl,-rpath-link=../libtrivfs/ \
    -Wno-unused-function -Wno-unused-variable -std=gnu99 -Wall \
    -g -O0 -fno-strict-aliasing -no-pie \
    -L"$HOME/rumplib" -Wl,--whole-archive \
      "$HOME/rumplib/librumpdev_virtio_if_vioif_pic.a" \
      "$HOME/rumplib/librumpdev_pci_virtio_pic.a" \
    -Wl,--no-whole-archive \
    -uargp_program_bug_address -o rumpnet-vioif-test \
    main.o net-rump.o \
    '-Wl,-(' ../libhurdbugaddr/libhurdbugaddr.so ../libmachdev/libmachdev.so \
      ../libports/libports.so ../libtrivfs/libtrivfs.so \
      ../libshouldbeinlibc/libshouldbeinlibc.so ../libiohelp/libiohelp.so \
      ../libihash/libihash.so ../libfshelp/libfshelp.so \
      ../libirqhelp/libirqhelp.so \
    -Wl,--no-as-needed -lrump -lrumpuser -lrumpdev -lrumpdev_miiphy \
      -lrumpdev_pci -lrumpvfs -lrumpvfs_nofifofs -lrumpnet -lrumpnet_net \
      -lrumpnet_netinet -lrumpnet_local -lrumpdev_bpf \
    -Wl,--as-needed -lpthread -lpciaccess -ldl -lz \
    '-Wl,-)'
echo "LINK OK: $(ls -la rumpnet-vioif-test)"
file rumpnet-vioif-test
