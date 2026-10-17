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
- Récupération si échec : accès console (moniteur QEMU → `screendump`/
  `sendkey`) → `sudo cp /etc/network/interfaces.eth0.bak
  /etc/network/interfaces && sudo ifup /dev/eth0 && sudo reboot`.

## Reprise `-smp 2` (17/10/2026) — nouvelle observation

> ⚠️ **Nuance (voir § *Autopsie du gel `-smp 2`*)** : les mesures « CPU0 spin
> utilisateur `RIP=0x5225ac` » ci-dessous ont été prises **avec le stub gdb
> (`-s`) sans `cont`** → elles sont faussées par la mise en pause automatique
> de QEMU. Sur overlay neuf, le blocage réel se produit **plus tôt**, dans
> l'attache AHCI de `rumpdisk` (`ahcisata0: 64-bit DMA`).

- Le noyau par défaut de la VM dev (entrée GRUB 0) est déjà
  `gnumach-smp-test.gz` : `-smp 1` = système fonctionnel (réseau vioif +
  internet). Le test `-smp 2` = relancer QEMU avec `-smp 2` (même disque,
  même noyau) — de préférence sur un overlay jetable (cf. README).
- **Observation révisée** : en `-smp 2` le boot va **plus loin** que l'ancien
  « figé après `ahcisata0: 64-bit DMA` ». (Mesure faite à l'époque avec X
  actif ; le projet est passé depuis en **console pure, sans X**.) Le boot
  progresse jusqu'au login mais :
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
  (réseau vioif + internet).

## Option de boot `maxcpus=N` / `nosmp` (17/10/2026) — conservée

- **Rôle** : option de ligne de commande noyau (multiboot) qui **plafonne le
  nombre de CPU utilisés** au démarrage :
  - `nosmp` → au plus 1 CPU ;
  - `maxcpus=N` → au plus N CPU.
  Implémentée dans `i386/i386/smp.c::smp_data_init()` (commit `a4237652`,
  branche git `fork/x86_64`), elle lit `kernel_cmdline`. Elle permet donc de
  **forcer le mode mono-CPU sur une VM multi-cœurs** (`-smp N`), sans
  recompiler le noyau ni changer le `-smp` de QEMU.
- **Usage** : ajouter `nosmp` (ou `maxcpus=1`) à la ligne
  `multiboot /boot/gnumach… root=part:2:device:wd0` de GRUB. Confirmation au
  boot : `SMP: maxcpus=N, limite le noyau a N CPU sur M`.
- **Ce que ça ne fait PAS** : ça ne répare **pas** les races SMP. Un noyau
  `--enable-ncpus≥2` lancé sur `-smp 2` **avec** tous les CPU continue de
  geler (cf. § *Autopsie du gel `-smp 2`*). L'option sert uniquement à
  *éviter* le SMP (mode mono fiable), pas à le rendre fonctionnel.
- **Statut** : **conservée**. Arbre de travail propre, commit poussé sur
  `remotes/github/fork/x86_64`. Aucun revert.

## Autopsie du gel `-smp 2` (17/10/2026) — GDB sur QEMU

Méthode : `vm/run-hurd64-smp.sh` accepte désormais `GDB=1` (stub QEMU
`-gdb tcp::1234`), `STOP=1` (`-S`), `FRESH=0` (réutiliser l'overlay). gdb est
absent de l'hôte et `sudo` est bloqué → gdb récupéré **sans root** via
`apt-get download gdb` + `dpkg-deb -x` (idem ses libs : libbabeltrace, libipt,
libsource-highlight, libdebuginfod) puis
`LD_LIBRARY_PATH=/tmp/gdbroot/usr/lib/x86_64-linux-gnu /tmp/gdbroot/usr/bin/gdb
/tmp/gnumach-new` (noyau fork, ELF avec `debug_info`).

**Piège méthodologique majeur** : avec `-gdb`, QEMU **met la VM en pause dès
qu'elle devient oisive** (~20 s) — `info status` → `VM status: paused`. Toute
mesure faite sans `cont` est donc faussée (HPET « figé », SSH qui expire,
`CPL=3`). **Toujours faire `cont` après un `detach` gdb et vérifier
`info status`.**

### Résultat (reproductible)

- Overlay **neuf** (`FRESH=1`, fs propre) + noyau fork installé en
  `/boot/gnumach-1.8-amd64-up.gz` :
  - `-smp 1` → boot complet, réseau vioif monté, SSH OK ;
  - `-smp 2` → le boot **se bloque toujours** sur la même ligne console :
    ```
    ahcisata0 at pci0 dev 31 function 2: vendor 8086 product 2922 (rev. 0x02)
    ahcisata0: 64-bit DMA
    ```
    (constaté 4 fois, dont > 5 min sans progression ; `cont` + `sendkey` ne
    débloquent pas).
- Cette sortie vient du **traducteur utilisateur `rumpdisk`**
  (`/hurd/rumpdisk.static`) : la chaîne `ahcisata` **n'est pas** dans le noyau
  (`strings gnumach | grep -c ahcisata` = 0) et `rump` n'est pas compilé dans
  gnumach. C'est le rump kernel embarqué dans `rumpdisk` qui se bloque en
  pleine attache AHCI, **avant tout montage de fs**.
- État noyau au blocage (gdb, symboles `/tmp/gnumach-new`) :
  - **CPU0** : `rip 0xffffffff810168e4` = `hpclock_read_counter+20`
    (`i386/i386/apic.c:495`) ; backtrace
    `clock_interrupt (usec=10000) ← kern/mach_clock.c:341 ← interrupt()`
    (`x86_64/interrupt.S:128`). ⇒ CPU0 ne fait que l'IT horloge 100 Hz.
  - **CPU1** : `rip 0xffffffff8103403d` = `machine_idle+13`
    (`i386/i386at/model_dep.c:257`), `HLT=1`.
  - `percpu_array[0..1].active_thread->task->name` = `"gnumach"` (tâche oisive)
    ⇒ **aucune tâche utilisateur n'est runnable** : `rumpdisk` est **bloqué
    dans le noyau**, il ne fait pas de spin.
  - Le compteur HPET avance (`xp/2xw 0xfed000f0` change) ⇒ l'horloge **n'est
    pas** gelée ; ce n'est pas un problème de timer.
- Interprétation : sous `-smp 2` l'attache AHCI de `rumpdisk` n'aboutit jamais
  (attente d'un événement qui n'arrive pas : IRQ PCI non délivrée / réveil
  perdu). Le reste du noyau est sain. C'est bien une **course SMP** : en
  `-smp 1` le *même* binaire `rumpdisk` passe.
- Piste côté gnumach : `device/intr.c` — un IRQ utilisateur est **masqué**
  jusqu'à l'`irq_acknowledge()` de l'utilisateur (`queue_intr()` →
  `__disable_irq()`), et le nettoyage des ports morts imprime
  `release a dead delivery port` / `still N unacked irqs` (messages déjà vus
  lors du gel de bascule réseau). Un IRQ non délivré/acquitté en SMP laisse le
  pilote bloqué définitivement. **Aucun correctif isolé identifié à ce stade.**
- Limite : la marche de `default_pset.tasks` via gdb renvoie des entrées
  invalides, et le boot bloqué avant le shell interdit `ps` ; le PC utilisateur
  exact de `rumpdisk` n'a donc pas pu être relevé. Le **binaire utilisateur
  fautif est néanmoins identifié sans ambiguïté** (`rumpdisk`) et le **point de
  blocage** est l'attache AHCI (`ahcisata`, après « 64-bit DMA »).

⇒ Chantier **non isolé** (rump/Hurd, pas un simple verrou gnumach) :
`-smp 2` reste **inutilisable** ; `-smp 1` reste la référence fonctionnelle.

## Virtio stockage (`virtio-blk` / `ld_virtio`) (17/10/2026) — recette prête, non exécutable ici

Objectif : étendre la recette rump virtio (déjà OK pour
`librumpdev_pci_if_vioif`) au **stockage** (`librumpdev_virtio_ld`), puis voir
si `rumpdisk` détecte un disque `ld0`.

### État de l'outillage

- `patches/build/build-virtio.sh` ne construit que le **réseau** :
  `libpci_virtio` (`dev/pci/virtio.c` + `virtio_pci.c`) et
  `libvirtio_if_vioif` (`dev/pci/if_vioif.c`). **Aucun** `ld_virtio`.
- La recette `ld_virtio` **serait la même** : `build_lib libvirtio_ld
  $S/dev/pci/ld_virtio.c $R/dev/lib/component_simple.c` avec
  `IONAME=LD_VIRTIO.ioconf`, plus le même correctif (`<machine/cpu.h>` → rump
  + `-D_RUMPKERNEL`) : le blocage `curlwp`/`%gs` est identique (code PCI rump).
  Le front-end PCI (`libpci_virtio` + `virtio_pci_glue.c`) est déjà là et
  suffit pour attacher `ld_virtio` au bus `virtio`.
- **MAIS** la construction exige l'arbre source **NetBSD 2025-01**
  (`$HOME/nbsrc25` = `sys` + `common` + `rump`) **et** le `tooldir`
  (`config(1)` + gcc cross). Cet arbre est **absent** de l'hôte
  (`/home/ben/nbsrc25`, `~/rumplib`, `~/ovr` : inexistants) **et** du guest
  (`/home/demo/nbsrc25` : inexistant ; seul
  `~/hurd-build/rumpnet/rumpnet-vioif-test` subsiste).
  ⇒ **`ld_virtio` ne peut pas être compilé dans cet environnement.**

### Côté paquets (guest)

- Debian fournit les rump **runtime** `0~20250111-7` :
  `librumpdev_ahcisata.so`, `librumpdev_ata.so`, `librumpdev_disk.so`,
  `librumpdev_pci.so`… mais **aucun `librumpdev_virtio*`** (Debian a désactivé
  virtio côté rump : « Fix virtio later »). `apt-cache search rump` le
  confirme : aucun paquet `*virtio*` (ni dev ni runtime). D'où le besoin de
  notre build maison (vioif).

### Test disque virtio secondaire

- `vm/run-hurd64-vioif.sh` accepte maintenant `BLK=1` (+ `BLKIMG`, `BLKSIZE`) :
  ajoute `-drive file=$BLKIMG,if=none,id=vd0,format=raw -device
  virtio-blk-pci,drive=vd0`.
- Boot `-smp 1`, disque 64 Mo attaché :
  - QEMU voit le contrôleur : `info pci` →
    `SCSI controller: PCI device 1af4:1001` (vendor `1af4` = virtio,
    `0x1001` = virtio-blk) ;
  - côté guest : **pas de `/dev/ld0`** (`ls /dev/ld*` → absent) ; `rumpdisk`
    (Debian, sans virtio) ignore le périphérique.
- Conclusion : le disque est présent au niveau PCI mais **aucun pilote ne le
  prend**. Pour obtenir `ld0` il faudrait **recompiler `rumpdisk`** (source
  Hurd disponible sur l'hôte) **en y liant** un `librumpdev_virtio_ld` construit
  avec l'arbre NetBSD — bloqué par l'absence de cet arbre.

⇒ ÉTAPE 3 : recette identifiée et prête, **non exécutable ici** (dépendance
source NetBSD manquante) ; test QEMU `ld0` négatif (attendu).

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
- **vioif** : démarre et s'attache **seulement avec 4 Go de RAM**
  (cf. § « Résolution vioif » ci-dessous). En 2 Go :
  `virtio0: virtqueue 0 for rx0 allocation failed, error code 1`.
- **Console pure / headless** : plus de X11/XFCE (ne pas installer
  `xfce4`/`xdm`/Xorg).

## Résolution vioif / bascule réseau (17/10/2026)

- **Attache AU BOOT = OK (chemin retenu).** `/etc/network/interfaces` =
  `scripts/interfaces.vioif` : `auto /dev/vioif0` +
  `pre-up /usr/local/sbin/start-rumpnet.sh` ; `eth0` en secours non-auto.
  Validé après reboot sur la VM dev : `ifconfig /dev/vioif0` = `10.0.2.15`,
  `ping` gateway + `8.8.8.8`, DNS, HTTPS (`wget` rc=0), SSH hôte `2223`.
  `start-rumpnet.sh` lance bien le translator `rumpnet-vioif-test` puis le
  devnode `/dev/vioif0` (log `/tmp/start-rumpnet.log`).
- **4 Go obligatoires.** En 2 Go le pilote ne s'attache pas :
  `virtio0: virtqueue 0 for rx0 allocation failed, error code 1`.
  En 4 Go : `virtio0: interrupting at pausebreak`, IRQ OK, process vivant.
  D'où `-m 4G` dans `vm/run-*.sh`.
- **La bascule À CHAUD rend la machine inutilisable.** Faire
  `ifconfig /dev/eth0 down` sur un système vivant où rumpnet tourne →
  messages noyau `irq handler [11]: release a dead delivery port …` puis
  `still 1 unacked irqs in entry …` ; réseau mort **et** console VGA qui
  n'écho plus ; noyau en `machine_idle` (`RIP ffffffff8100c7fd`) →
  **reboot obligatoire**. Reproduit sur un overlay jetable.
  ⇒ `scripts/net-switch-vioif.sh` est **verrouillé** (refus par défaut ;
  cf. en-tête du script) : ne plus basculer à chaud, rebooter.
- **Nuance / diagnostic corrigé** : un simple `ifconfig /dev/eth0 down`
  *sans* rumpnet actif **ne fait pas** crasher `pfinet` — il reste vivant
  (vérifié sur la console via `ps`). Les diagnostics « pfinet a crashé »
  faits depuis SSH étaient biaisés : couper `eth0` coupe aussi la session
  SSH (qui passe par `eth0`).
- **Piste A (binaires Hurd du fork) : écartée.** L'ancien disque
  (`hurd-dev.qcow2.fixed-corrupt`) contenait bien **tout le Hurd du fork**
  installé (`/hurd/*` + `/lib/x86_64-gnu` en date du 2026-10-08), face au
  paquet Debian de base `hurd 1:0.9.git20251029-7` (binaires du 2026-03-07) ;
  `netdde` est identique sur les deux disques. Mais un test A/B (graft du
  `pfinet` du fork + `libtrivfs/libfshelp/libports/libshouldbeinlibc` du
  fork) **ne change rien** au blocage → ce n'est pas un bug `pfinet` mais un
  problème de démontage IRQ à chaud. Les binaires du fork sont conservés
  hors dépôt dans `~/hurd-fork/recovered/hurd-fork-20261008/` (voir son
  `README.md`) mais **non nécessaires** — l'attache au boot marche avec les
  binaires Debian de base.

## Sécurité disque (consignes permanentes)

- Ne **jamais** démarrer `debian-hurd-amd64-*.img` en écriture.
- Ne **jamais** ré-extraire/réécraser une image qui sert de *backing file*.
- `vm/run-*.sh` n'utilisent qu'une copie **autonome** (`hurd-dev.qcow2`,
  `-m 4G`) ; `run-hurd64-smp.sh` et `run-hurd64-vioif.sh` démarrent en plus
  sur un **overlay jetable** (`hurd-*-test.qcow2`, recréé à chaque lancement)
  pour les essais risqués → le disque de dev n'est jamais touché.
- Sauvegarde d'un état qui marche : `hurd-dev.qcow2.good-20261017`
  (copie faite VM éteinte proprement).

## Références

- FOSDEM 2026 Thibault : x86_64 essentiellement complet (MIG RPC), ~75% Debian.
- Guix blog 01/03/2026 : 64-bit Hurd, GCC 14, `hurd64-qcow2`, `kernel-arguments ("noide")`.
- `~/hurd-fork/vm/README-64bit:99-116` pour réseau/disque.
