# Fork GNU Hurd x86_64 — base locale

Branches locales `fork/x86_64` créées sur `mig`, `gnumach`, `hurd` (upstream = savannah, master du 16/10/2026).

## État constaté (audit 16/10/2026)

- x86_64 boot sous QEMU q35 + e1000 + rumpdisk, `noide`. Image testée : `~/hurd-fork/vm/debian-hurd-amd64-*.img`.
- `gnumach/x86_64/` = shim sur `i386/i386/` (symlink `x86_64/x86_64 -> ../i386/i386`).
  `Makefrag.am` 64-bit réutilise `i386/i386at/*`. Flags : `-mcmodel=kernel -mno-red-zone`, `KERNEL_MAP_BASE=0xffffffff80000000`.
- SMP : code présent (`i386/i386/smp.c`, `apic.c`, `cpuboot.S` x2, `acpi_parse_apic.c`, `ioapic.c`) mais déclaré cassé (`configfrag-first.ac` : `Multiprocessor support is still broken`, défaut `NCPUS=1`).
- MIG : générique, `cpu.sym` gère LP64 via `sizeof(long/uintptr_t`. Pas de dir arch.
- `hurd/` : ~37 `#ifdef __x86_64__` seulement. Points 64-bit restants : `exec/elfcore.c:102`, `rumpnet/net-rump.c:128 bpf_hdr`, `utils/login.c:168 utmp`, `procfs/cpuid`.
- rump : `rumpdisk` (ahcisata/piixide/ata + usb) et `rumpnet` (wm Intel PRO/1000 uniquement, pas de virtio-net). `RUMP_NCPU=1` forcé dans les deux `main.c`.
- `noide` : aucune occurrence dans le code, c'est un arg boot + garde `sd/hd/ud` dans `rumpdisk/block-rump.c`.

## Scripts QEMU

- `~/hurd-fork/vm/run-hurd64.sh` : boot standard 2G q35 e1000, ssh 2222.
- `~/hurd-fork/vm/run-hurd64-smp.sh` : test `-smp 2`, ssh 2223, snapshot on.

## Roadmap fork proposée

1. P0 — Harnais : scripts QEMU + `ssh -p 2222 root@localhost`, doc test translator (`settrans`, `fsysopts`, `showtrans`, `rpctrace`).
2. P1 — Correctness x86_64 : `elfcore`, `bpf_hdr`, `login utmp`, test `qemu-system-x86_64` en CI.
3. P2 — rumpnet : ajouter `virtio-net` à `rumpnet/Makefile`, bench e1000 vs virtio.
4. P3 — SMP : timeout boot AP (`smp.c`), tests `qemu -smp 2`, rendre `ipc/vm` MP-safe. Ne pas activer `NCPUS>1` par défaut avant.
5. P4 — rumpdisk : option `--no-ide-probe`, clarifier garde sd/hd.

## Suivi fork (16/10/2026)

- `hurd/fork/x86_64` : `5636dd8` bpf_hdr sec/usec, `b9bbcf5` login utmp saturant
  (tests layout/time_t vérifiés OK, scripts temporaires nettoyés).
- `gnumach/fork/x86_64` : `cfe04a2` SMP timeout, boot dégrade BSP-only au lieu de
  `while(1)`. À tester : `run-hurd64-smp.sh` avec noyau rebuildé en VM.
- `mig` : build hôte OK (cpu.h LP64 correct, répertoire temporaire nettoyé).
  `make check` 7 FAIL attendus sans `gnumach-dev` (stubs veulent `<mach/boolean.h>`).

## Tests noyau (16/10/2026, VM debian-hurd-amd64, q35/e1000)

- Noyau `cfe04a2` (`--enable-ncpus=2`, 3.5M) : **boote + SSH OK en `-smp 1`**
  (userland complet). Preuve : le patch ne casse pas le chemin UP.
- Même noyau en `-smp 2` : **bloqué après phase rump/ahcisata** (`ahcisata0:
  64-bit DMA` figé, pas de SSH). Cause probable : AP démarrés mais SMP
  globalement bancal (races connues), ou IPI vers AP mort plus tard
  (pmap/sched utilisent `smp_get_numcpus()` global).
- Fixup `12bd028` : `smp_set_numcpus()` propagé (dégradation globale).
  Testé en `-smp 2` : **même blocage après `ahcisata0: 64-bit DMA`**.
  Capture VGA à 1s (`capture VGA (non conservée)`) : `Sending IPIs from BSP APIC ID 0...`
  puis `AP=(1) paging/mpdesc/gdt/... done` + `LAPIC timer configured on
  cpu 1` → **SIPIs OK, l'AP démarre, notre chemin de dégradation ne se
  déclenche pas**. Le hang vient des races SMP en aval (locking incomplet,
  cf. `#warning SMP support missing` dans `io_perm.c`, `pcb.c`), pas de
  notre patch. Vrai SMP = chantier amont (verrous fins), hors portée d'un
  quick-fix.
- Leçons VM : `/tmp` vidé à chaque reboot → travailler dans `~` ;
  `snapshot=on` jette tout à l'arrêt QEMU ; image de base persistante OK ;
  `screendump` via monitor HMP pour le boot (pas de console série Mach).

## Réseau : pcn (16/10/2026, VM dev overlay, QEMU pcnet ajouté)

- `7b5ab52` lie `rumpdev_pci_if_pcn` (déjà packagé) à côté de `wm`. Build OK
  dans la VM, les deux `.so` liés. Aucun nom en dur dans `net-rump.c`.
- Test live : le driver **s'attache** (`pcn0 at pci0 dev 3: AMD PCnet-PCI`,
  syslog) mais lit **MAC `ff:ff:ff:ff:ff:ff`** → lectures PIO (ports I/O)
  qui rendent `0xFF`, interface inutilisable.
- Hypothèse : le chemin PIO de rump PCI (`libpciaccess` → `pci-arbiter`)
  est cassé sous Hurd ; `wm` (MMIO) et `rumpdisk` AHCI (MMIO) marchent.
- Leçon test : ne jamais lancer une instance rumpnet avec `wm` lié quand
  `e1000` est actif (`netdde`) — le probe wm réclame le NIC et tue le réseau
  (récupéré par `system_reset`). Tester avec variante pcn-only.
- Conséquence : **prioriser `vioif`/virtio-net (MMIO)** plutôt que pcn.
  `libvirtio_if_vioif` + `libpci_virtio` existent dans `rumpkernel/src-netbsd`
  (branche `kernel-src`) mais ne sont pas packagés par Debian → builder les
  libs rump, les lier dans `RUMPLIBS`, tester avec `-device virtio-net-pci`.

## virtio-net (`vioif`) rump sur Hurd (17/10/2026) — crash corrigé

- Contexte : `libvirtio_if_vioif` + `libpci_virtio` ne sont pas packagés par
  Debian → buildés à partir de `nbsrc25` (NetBSD 2025-01, commit `4a1a4e8`).
  Recette : `patches/build/build-virtio.sh` ; relink :
  `patches/build/link-vioif.sh` (binaire non-PIE pour symboliser les crashes).
- **Bug** : après un attachement correct de `vioif0` (MAC `52:54:00:12:34:58`,
  virtqueues rx/tx/ctrl, IRQ 20), SIGSEGV au `device_open` (chemin
  `up_interface` → `rump_sys_ioctl(SIOCSIFFLAGS)` → `vioif_init` →
  `vioif_rx_filter` → `vioif_set_rx_filter` → `vioif_ctrl_acquire` →
  `curlwp`) sur `mov %gs:0xac0,%rax` (base `%gs` = 0, sous-code 0xAC0).
- **Cause** : nos objets étaient compilés avec le `<machine/cpu.h>` de l'arch,
  où `curlwp == x86_curlwp()` (accès `%gs`). Or sous Hurd, rump n'utilise
  **pas** `%gs` (glibc réserve `%fs` pour le TLS) : il faut
  `curlwp == rump_curlwp_fast()` (mécanisme *hypercall*,
  `rump_lwproc_curlwp_hypercall()`), tel que défini par le
  `sys/rump/include/machine/cpu.h`. L'ordre d'include upstream
  (`sys/rump/Makefile.rump`) met `-I${RUMPTOP}/include` **en premier** ; notre
  recette mettait `-I$D` (build dir, `machine -> arch`) en premier → mauvais
  `cpu.h`. (`-I$D` d'abord avait été choisi pour que `<machine/intr.h>` =
  arch, car `x86/pci_machdep.h` exige `intr_handle_t`.)
- **Fix** : interposer `$HOME/ovr/machine/cpu.h -> rump` et
  `$HOME/ovr/machine/intr.h -> arch`, `ovr` en tête du `-I`, +
  `-D_RUMPKERNEL`. Vérification : `if_vioif.pico` n'a plus aucune référence
  `%gs:` ni `x86_curlwp`, et appelle `rump_lwproc_curlwp_hypercall`.
- **Résultat** : `device_open("vioif0") -> err=0` via le translator rumpnet,
  attache stable, plus aucun crash (backtrace sous gdb). Étape suivante :
  `pfinet` sur le device / démarrage au boot.
- Méthode : lancer le translator sous gdb (`settrans -ac NODE wrapper-gdb`).
  **Attention** : `settrans` sans `-a` = translator *passif* (démarre
  seulement à l'ouverture du nœud). Nettoyer les nœuds translator laissés :
  `find /` les ouvre et peut figer la VM.

## Références

- FOSDEM 2026 Thibault : x86_64 essentiellement complet (MIG RPC), ~75% Debian.
- Guix blog 01/03/2026 : 64-bit Hurd, GCC 14, `hurd64-qcow2`, `kernel-arguments ("noide")`.
- `~/hurd-fork/vm/README-64bit:99-116` pour réseau/disque.
