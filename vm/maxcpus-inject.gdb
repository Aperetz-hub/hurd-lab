set pagination off
set confirm off
# Injecte `maxcpus=1` dans la ligne de commande noyau SANS toucher a GRUB :
# on ecrit dans la chaine pointee par `kernel_cmdline` (upstream, aucun
# endroit fixe) pendant l'execution de `smp_init` (le noyau lit la ligne plus
# tard, dans `smp_data_init`).
#
# Prerequis : lancer QEMU avec GDB=1 STOP=1 GDBPORT=<port>, puis
#   gdb -q -batch -x vm/maxcpus-inject.gdb <ELF-du-noyau-teste>
# et modifier `target remote localhost:1234` si GDBPORT != 1234.
#
# TOUJOURS finir par `detach` : un script gdb qui s'interrompt sans detach
# laisse la VM en pause (`info status` = paused) et invalide toute mesure.
target remote localhost:1234
hbreak smp_init
continue
printf "=== AT smp_init, cmdline avant : "
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
printf "cmdline apres : "
p (char*)kernel_cmdline
delete
detach
