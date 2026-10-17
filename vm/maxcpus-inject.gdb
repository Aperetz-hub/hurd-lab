set pagination off
set confirm off
# Injects `maxcpus=1` into the kernel command line WITHOUT touching GRUB:
# we write into the string pointed to by `kernel_cmdline` (upstream, no
# fixed location) during the execution of `smp_init` (the kernel reads the
# line later, in `smp_data_init`).
#
# Prerequisites: start QEMU with GDB=1 STOP=1 GDBPORT=<port>, then
#   gdb -q -batch -x vm/maxcpus-inject.gdb <ELF-of-the-kernel-under-test>
# and adjust `target remote localhost:1234` if GDBPORT != 1234.
#
# ALWAYS end with `detach`: a gdb script that aborts without detach leaves
# the VM paused (`info status` = paused) and invalidates any measurement.
target remote localhost:1234
hbreak smp_init
continue
printf "=== AT smp_init, cmdline before : "
p (char*)kernel_cmdline
set {char}(((char*)kernel_cmdline)+0) = 'r'
set {char}(((char*)kernel_cmdline)+1) = 'o'
set {char}(((char*)kernel_cmdline)+2) = 'o'
set {char}(((char*)kernel_cmdline)+3) = 't'
set {char}(((char*)kernel_cmdline)+4) = '='
set {char}(((char*)kernel_cmdline)+5) = 'p'
set {char}(((char*)kernel_cmdline)+6) = 'a'
set {char}(((char*)kernel_cmdline)+7) = 'r'
set {char}(((char*)kernel_cmdline)+8) = 't'
set {char}(((char*)kernel_cmdline)+9) = ':'
set {char}(((char*)kernel_cmdline)+10) = '2'
set {char}(((char*)kernel_cmdline)+11) = ':'
set {char}(((char*)kernel_cmdline)+12) = 'd'
set {char}(((char*)kernel_cmdline)+13) = 'e'
set {char}(((char*)kernel_cmdline)+14) = 'v'
set {char}(((char*)kernel_cmdline)+15) = 'i'
set {char}(((char*)kernel_cmdline)+16) = 'c'
set {char}(((char*)kernel_cmdline)+17) = 'e'
set {char}(((char*)kernel_cmdline)+18) = ':'
set {char}(((char*)kernel_cmdline)+19) = 'w'
set {char}(((char*)kernel_cmdline)+20) = 'd'
set {char}(((char*)kernel_cmdline)+21) = '0'
set {char}(((char*)kernel_cmdline)+22) = ' '
set {char}(((char*)kernel_cmdline)+23) = 'm'
set {char}(((char*)kernel_cmdline)+24) = 'a'
set {char}(((char*)kernel_cmdline)+25) = 'x'
set {char}(((char*)kernel_cmdline)+26) = 'c'
set {char}(((char*)kernel_cmdline)+27) = 'p'
set {char}(((char*)kernel_cmdline)+28) = 'u'
set {char}(((char*)kernel_cmdline)+29) = 's'
set {char}(((char*)kernel_cmdline)+30) = '='
set {char}(((char*)kernel_cmdline)+31) = '1'
set {char}(((char*)kernel_cmdline)+32) = 0
printf "cmdline after : "
p (char*)kernel_cmdline
delete
detach
