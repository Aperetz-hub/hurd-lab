# hurd-lab — fork GNU/Hurd x86_64 : SMP + rumpnet virtio

Fork expérimental de GNU/Hurd (64-bit) avec deux objectifs :

1. **gnumach** : ne plus pendre le boot quand les AP ne démarrent pas
   (dégradation propre vers mono-CPU au lieu de `while (1)`).
2. **rumpnet** : ajouter le support réseau **virtio-net** (`vioif`) — pilotes
   `rumpdev_virtio_if_vioif` / `rumpdev_pci_virtio`, absents des paquets
   Debian — puis en faire l'interface réseau par défaut.

## État (17/10/2026)

> **Reconstruction en cours.** Le disque de la VM dev a été corrompu le
> 17/10 (voir `docs/suivi.md` § *INCIDENT* : ré-extraction de l'image de base
> d'un overlay alors qu'il l'utilisait). Une VM neuve a été reconstruite depuis
> l'image pristine ; `eth0`/netdde + SSH refonctionnent, mais XFCE n'est
> **pas encore** réinstallé et l'intégration vioif est en cours de revalidation.

- **virtio-net (`vioif`)** : le pilote rump compile et s'attache au device.
  - Le crash `curlwp`/`%gs` est corrigé (interposition de `<machine/cpu.h>`
    rump + `-D_RUMPKERNEL`, voir `patches/build/`).
  - Sur la VM reconstruite, le translator démarre et voit `vioif0`
    (MAC `52:54:00:12:34:58`) **uniquement avec 4 Go de RAM** ; en 2 Go :
    `virtio0: virtqueue 0 for rx0 allocation failed, error code 1`.
  - Intégration boot : `scripts/start-rumpnet.sh` + `scripts/interfaces.vioif`
    (+ `scripts/net-switch-vioif.sh`). **En cours de revalidation** : le
    câblage du devnode `/dev/vioif0` (`devnode -M /dev/rumpnet vioif0`) fige
    encore la machine. `eth0`/netdde sert de secours.
  - `glibc ≥ 2.43` requis par les binaires rump (l'image pristine est en 2.42).
- **SMP : non fonctionnel.** `-smp 1` = système complet. `-smp 2` = échecs non
  déterministes (soft-wedge *userspace*, panic `EX_NOT_EXECUTABLE` au
  bootstrap, `machine_idle`). L'option de boot `maxcpus=N`/`nosmp` (commit
  `a4237652`, branche `fork/x86_64`) **ne contourne pas** le problème
  → cul-de-sac.

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
