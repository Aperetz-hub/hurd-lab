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

## pfinet sur vioif0 — test live (17/10/2026)

- Translator rumpnet (`rumpnet-vioif-test`) + devnode sur `/dev/vioif0` :
  `settrans -ac /dev/rumpnet /home/demo/rumpnet-vioif.sh` puis
  `settrans -c /dev/vioif0 /hurd/devnode -M /dev/rumpnet vioif0`.
  `open-dev /dev/vioif0 eth` → `device_open("eth") -> err=0` (pfinet ouvre le
  nœud puis `device_open("eth")`, cf. `pfinet/ethernet.c`).
- **Bascule sans redémarrer pfinet** (méthode retenue) : `ifconfig` (Hurd)
  ajoute une interface à l'instance pfinet vivante (pfinet gère N interfaces).
  Séquence validée (`scripts/net-switch-vioif.sh`) :
  1. `pkill -f dhclient` (sinon dhclient remet `eth0` up) ;
  2. `ifconfig /dev/eth0 down` ;
  3. `ifconfig /dev/vioif0 10.0.2.15 netmask 255.255.255.0 up`.
  Résultat : **ping gw 10.0.2.2 + DNS 10.0.2.3 OK**, et **SSH via hostfwd
  hôte 2223 OK** (TX+RX+TCP+résolution, single-interface) → driver virtio-net
  pleinement fonctionnel. Rollback auto (watchdog `KEEP`) validé.
- **Limite** : la route par défaut reste liée à `eth0`. `fsysopts
  /servers/socket/2 --gateway 10.0.2.2` → `EBUSY` (interfaces même
  sous-réseau) ; l'accès externe (hors 10.0.2.0/24) échoue donc après bascule.
- **Piège** : redémarrer pfinet (`settrans -g` puis `settrans -ac` sur
  `/servers/socket/2`) **fige** : `ifconfig /dev/vioif0` bloque indéfiniment
  (l'ancien pfinet garde le device ouvert). Ne pas redémarrer pfinet à chaud ;
  utiliser `ifconfig` sur l'instance vivante, ou configurer vioif dès le boot.
  Récupération : reboot (mais `/etc/init.d/networking` stop → `dhclient -r`
  peut figer le shutdown sans réseau → `system_reset` + fsck).
## virtio-net (vioif) : intégration au boot (17/10/2026) — FAIT, validé

- Fichiers (dans `scripts/`) :
  - `start-rumpnet.sh` → installé `/usr/local/sbin/start-rumpnet.sh` : démarre
    le translator rumpnet + le devnode `/dev/vioif0` (idempotent, log
    `/tmp/start-rumpnet.log`).
  - `interfaces.vioif` → `/etc/network/interfaces` : `auto /dev/vioif0` avec
    `pre-up /usr/local/sbin/start-rumpnet.sh` ; `eth0` conservé en secours
    (non auto) ; sauvegarde `/etc/network/interfaces.eth0.bak`.
- **Résultat après reboot** : boot propre (pas de hang au shutdown car `eth0`
  n'est plus `auto` → plus de `dhclient -r` sur un réseau mort). L'interface
  unique est `/dev/vioif0` (MAC `52:54:00:12:34:58`) et `pfinet` a bien
  `--gateway=10.0.2.2` (au boot pfinet est neuf → **pas de `EBUSY`**).
  Vérifié : `ping 10.0.2.2`, `ping 8.8.8.8`, DNS (`getent ahostsv4`),
  HTTPS (`wget https://example.com` rc=0), SSH via hostfwd hôte 2223.
- **Caveat** : ne pas `ifup /dev/eth0` sur un système vivant en vioif : la
  méthode `dhcp`/`fsysopts` de ifup reconstruit les options pfinet et **perd
  `--gateway`** (plus d'accès externe) en plus d'ajouter une 2e interface.
  En cas de besoin, rebooter (l'état de boot est propre) ou refixer la route.
- Récupération si échec : console graphique X (xdm) → terminal
  (`Ctrl+Alt+T`) → `sudo cp /etc/network/interfaces.eth0.bak
  /etc/network/interfaces && sudo ifup /dev/eth0 && sudo reboot`.

## Reprise `-smp 2` (17/10/2026) — nouvelle observation

- Le noyau par défaut de la VM dev (entrée GRUB 0) est déjà
  `gnumach-smp-test.gz` : `-smp 1` = système fonctionnel (vioif + XFCE).
  Le test `-smp 2` = relancer QEMU avec `-smp 2` (même disque, même noyau).
- **Observation révisée** : en `-smp 2` le boot va **plus loin** que l'ancien
  « figé après `ahcisata0: 64-bit DMA` » (ce texte restait juste le dernier
  de la console avant que X prenne l'écran). Le boot **atteint xdm/X**, mais :
  - le greeter est dessiné **incomplet** (pas de champs login) ;
  - **le réseau ne monte pas** (ni 2222 ni 2223, `pfinet` inopérant) ;
  - **CPU0 reste bloqué en espace utilisateur** : `RIP=0x5225ac` (`CPL=3`)
    constant sur plusieurs échantillons → spin utilisateur (libpthread/glibc
    ou IPC noyau), et **CPU1 est idle** (`machine_idle`, `HLT=1`).
  ⇒ c'est un **soft-wedge** (spin), pas une boucle noyau unique. Cohérent avec
    les races SMP amont (verrous fins manquants). Pas de quick-fix.
- **Diagnostic sans gdb** (absent de l'hôte) : HMP
  `cpu N` + `info registers` pour échantillonner `RIP` par CPU. Symboles :
  `addr2line -e /home/ben/hurd-fork/patches/gnumach-smp-test -f -p 0x...`
  (ELF avec `debug_info`). `-s` (gdbserver tcp:1234) démarre la VM *paused* →
  faire `cont` avant d'observer.
- **Récupération** : le guest SMP wedgé ne s'éteint pas proprement → kill QEMU
  → le fs devient sale → au reboot, prompt maintenance → `Enter` →
  `fsck -y /dev/wd0s2` → `Ctrl-D` (le boot reprend).
- Rappel : `eth0`/netdde reste un secours ; en `-smp 1` la VM fonctionne
  (réseau vioif + internet + XFCE).

## Option boot `maxcpus=N` / `nosmp` (17/10/2026) — échec

- But : pouvoir démarrer un noyau compilé `--enable-ncpus=2` sur une machine
  `-smp 2` en n'utilisant que le BSP (contournement des races SMP amont).
- Implémentation : `i386/i386/smp.c::smp_data_init()` plafonne
  `apic_get_numcpus()` d'après `kernel_cmdline` (`maxcpus=N`, `nosmp`).
  Commit `a4237652` (branche `fork/x86_64`).
- **Résultat : ne marche pas.** Le noyau plafonné *ainsi que* le noyau
  d'origine échouent en `-smp 2`. Symptômes non déterministes observés :
  panic `EX_NOT_EXECUTABLE` au bootstrap, blocage aux scripts de boot,
  `machine_idle` après « hurd server bootstrap ». L'AP est bien arrêté
  (CPU1 `HLT=1`), donc ce n'est pas un simple problème de démarrage d'AP.
  ⇒ l'option est un cul-de-sac ; `-smp 2` reste inutilisable.

## INCIDENT (17/10/2026) — perte du disque de la VM dev

- **Cause racine** : ré-extraction de `vm/debian-hurd-amd64-20260314.img`
  *alors que* `hurd-dev.qcow2` (overlay dont cette image est le **backing
  file**) était utilisé. Le tarball ré-extrait contenait une version
  différente → le backing a changé → vue disque incohérente → corruption fs.
- **Symptôme** : panic au bootstrap
  `user_bootstrap: Cannot load user executable module (error code 6000): exec`
  = `EX_NOT_EXECUTABLE`. En cause : `/lib/x86_64-gnu/ld-x86-64.so.1`
  (le module `exec` de GRUB est en fait `ld.so`) corrompu — son contenu était
  devenu une archive `ar`.
- **Tentatives** : `e2fsck -y` (2 passes, fs redevenu « clean ») +
  restauration de `ld.so` depuis l'image pristine → plus de panic, mais
  blocage définitif au bootstrap (serveur ext2fs/exec ne démarre pas).
  ⇒ disque irrécupérable. Conservé : `hurd-dev.qcow2.corrupt`,
  `hurd-dev.qcow2.fixed-corrupt`.
- **Leçon** : ne jamais re-extraire/écraser l'image de base d'un overlay
  pendant que l'overlay existe ; préférer des copies **autonomes**
  (`qemu-img convert`) sans backing file.

## Reconstruction de la VM dev (17/10/2026)

- Disque neuf **autonome** : `qemu-img convert debian-hurd-amd64-20260314.img
  → hurd-dev.qcow2` (plus de backing file).
- Réseau : `netdde`/e1000 (`10.0.2.15`), SSH par **clé** (port 2222) ;
  `demo` (dans `sudo`), mot de passe `demo`. `apt` OK.
- Artefacts rump récupérés depuis le disque corrompu (debugfs) :
  binaire `.../rumpnet/rumpnet-vioif-test`, libs `/usr/lib/librump*.so*`
  (114 fichiers), scripts. **glibc** doit être ≥ **2.43** (les binaires rump
  exigent `GLIBC_2.43`) → `libc0.3` mis à jour `2.42 → 2.43-7`.
- **vioif** : le driver démarre et détecte `vioif0`
  (MAC `52:54:00:12:34:58`) **seulement avec 4 Go de RAM**. En 2 Go :
  `virtio0: virtqueue 0 for rx0 allocation failed, error code 1` (le driver
  ne s'attache pas). En 4 Go : `virtio0: interrupting at pausebreak`,
  IRQ enregistrée, process `rumpnet-vioif-test` vivant.
- **Reste à faire** : le câblage du devnode
  `settrans -ac /dev/vioif0 /hurd/devnode -M /dev/rumpnet vioif0` fige la
  machine/le réseau (à débugger). Tant que ce n'est pas réglé, la VM reste
  fonctionnelle via `eth0`/netdde (`iface /dev/eth0 inet dhcp`).
- XFCE **non réinstallé** après reconstruction.

## Références

- FOSDEM 2026 Thibault : x86_64 essentiellement complet (MIG RPC), ~75% Debian.
- Guix blog 01/03/2026 : 64-bit Hurd, GCC 14, `hurd64-qcow2`, `kernel-arguments ("noide")`.
- `~/hurd-fork/vm/README-64bit:99-116` pour réseau/disque.
