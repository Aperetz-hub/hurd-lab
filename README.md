# hurd-lab — fork GNU/Hurd x86_64 : SMP + rumpnet virtio

Fork expérimental de GNU/Hurd (64-bit) avec deux objectifs :

1. **gnumach** : ne plus pendre le boot quand les AP ne démarrent pas
   (dégradation propre vers mono-CPU au lieu de `while (1)`).
2. **rumpnet** : ajouter le support réseau **virtio-net** (`vioif`) — pilotes
   `rumpdev_virtio_if_vioif` / `rumpdev_pci_virtio`, absents des paquets
   Debian — puis en faire l'interface réseau par défaut.

## État (17/10/2026)

- **virtio-net (`vioif`) : fonctionnel et intégré au boot.**
  - Le crash `curlwp`/`%gs` est corrigé (interposition de `<machine/cpu.h>`
    rump + `-D_RUMPKERNEL`, voir `patches/build/`).
  - La VM boote sur `/dev/vioif0` (MAC `52:54:00:12:34:58`) : ping gateway et
    `8.8.8.8`, DNS, HTTPS (`wget`) et SSH (hostfwd hôte `2223`) OK.
  - Intégration : `scripts/start-rumpnet.sh` (démarre le translator rumpnet +
    le devnode `/dev/vioif0`, appelé par `pre-up`) et
    `scripts/interfaces.vioif`. `eth0`/netdde conservé en secours.
- **SMP : toujours non fonctionnel.** `-smp 1` = système complet (réseau vioif
  + XFCE). `-smp 2` atteint X/xdm mais le greeter est incomplet, le réseau ne
  monte pas, et un processus **userspace spin** (CPU0 en `CPL=3`), CPU1 idle
  → **soft-wedge** (races SMP amont, verrous fins manquants). Pas de quick-fix.

Détails, méthodes de test et diagnostics : `docs/suivi.md` et les messages de
commit.

## Contenu

- `src/` — *non versionné ici* : clones complets de
  [`hurd/gnumach`](https://git.savannah.gnu.org/cgit/hurd/gnumach.git),
  [`hurd/hurd`](https://git.savannah.gnu.org/cgit/hurd/hurd.git) et
  [`hurd/mig`](https://git.savannah.gnu.org/cgit/hurd/mig.git).
  Travail sur branches `fork/x86_64` (poussées telles quelles sur GitHub,
  voir ci-dessous).
- `vm/` — scripts QEMU (`-M q35` ; e1000 + pcnet + virtio-net ; SSH
  localhost:2222 e1000 / :2223 virtio).
- `patches/` — patchs consolidés + outillage de build rump (virtio) et noyau
  de test SMP (`gnumach-smp-test`, ELF avec `debug_info`).
- `scripts/` — intégration boot vioif : `start-rumpnet.sh`,
  `interfaces.vioif`, `net-switch-vioif.sh`.
- `docs/` — notes de suivi (`suivi.md`).

## Branches publiées

- `main` — ce projet (docs, scripts, patchs).
- `fork/gnumach-x86_64` — historique gnumach + commits SMP.
- `fork/hurd-x86_64` — historique hurd + commits (bpf_hdr, login utmp, pcn…).

## Démarrage rapide (hôte Debian)

```
sudo apt install qemu-system-x86 qemu-utils
# image Debian hurd-amd64 officielle :
# https://cdimage.debian.org/cdimage/ports/latest/hurd-amd64/debian-hurd.img.tar.gz
./vm/run-xfce.sh          # bureau XFCE (login demo/demo)
./vm/run-hurd64.sh        # console + ssh -p 2222
./vm/run-hurd64-smp.sh    # test -smp 2 (ssh -p 2223) — attendu : soft-wedge
```

## Images VM

Volontairement **exclues** du dépôt (plusieurs Go) : image de base Debian
`debian-hurd-amd64-*.img` + overlay qcow2 de dev (voir `.gitignore`).
