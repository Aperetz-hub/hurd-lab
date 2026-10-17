# hurd-lab — fork GNU/Hurd x86_64 : SMP + rumpnet virtio

Fork expérimental de GNU/Hurd (64-bit) avec deux objectifs :

1. **gnumach** : ne plus pendre le boot quand les AP ne démarrent pas
   (dégradation propre vers mono-CPU au lieu de `while (1)`).
2. **rumpnet** : ajouter le support réseau **virtio-net** (`vioif`) — pilotes
   `rumpdev_virtio_if_vioif` / `rumpdev_pci_virtio`, absents des paquets
   Debian — puis en faire l'interface réseau par défaut.

## État (17/10/2026)

> **Mode console pure / headless — aucun X11.** Ce projet ne tourne **jamais**
> avec Xorg, `xdm` ou XFCE : tout se fait en console texte + SSH. Ne pas
> installer `xfce4`/`xdm`/Xorg sur la VM.

- **virtio-net (`vioif`) : fonctionnel au boot.**
  - Crash `curlwp`/`%gs` corrigé (interposition de `<machine/cpu.h>` rump +
    `-D_RUMPKERNEL`, voir `patches/build/`).
  - La VM démarre sur `/dev/vioif0` (MAC `52:54:00:12:34:58`) : `ping` gateway
    + `8.8.8.8`, DNS, HTTPS (`wget`), SSH (hostfwd hôte `2223`) — OK.
  - Intégration **au boot** : `scripts/start-rumpnet.sh` (translator rumpnet +
    devnode `/dev/vioif0`, appelé en `pre-up`) + `scripts/interfaces.vioif` ;
    `eth0`/netdde en secours (non-auto).
  - **`-m 4G` obligatoire** : en 2 Go le pilote ne s'attache pas
    (`virtio0: virtqueue 0 for rx0 allocation failed, error code 1`).
  - **Ne pas basculer à chaud** : `ifconfig /dev/eth0 down` avec rumpnet actif
    fige la machine (IRQ 11 non acquittée, réseau + console morts, reboot
    requis) → `scripts/net-switch-vioif.sh` est **verrouillé**.
  - `glibc ≥ 2.43` requis par les binaires rump.
- **SMP : non fonctionnel.** `-smp 1` = système complet. `-smp 2` = échecs non
  déterministes (soft-wedge *userspace*, panic `EX_NOT_EXECUTABLE` au
  bootstrap, `machine_idle`). L'option de boot `maxcpus=N`/`nosmp` (commit
  `a4237652`, branche `fork/x86_64`) **ne contourne pas** le problème
  → cul-de-sac.

> **Historique.** Le disque de la VM dev a été corrompu le 17/10
> (ré-extraction de l'image de base d'un overlay en service) puis reconstruit
> en copie autonome — voir `docs/suivi.md` § *INCIDENT*.

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
# image Debian hurd-amd64 officielle (base, JAMAIS modifiée en place) :
# https://cdimage.debian.org/cdimage/ports/latest/hurd-amd64/debian-hurd.img.tar.gz

# 1) créer une copie de travail AUTONOME (sans backing file) :
qemu-img convert -O qcow2 debian-hurd-amd64-*.img hurd-dev.qcow2

# 2) lancer (console pure / headless, -m 4G, ssh localhost:2222) :
./vm/run-hurd64.sh            # VM dev (disque autonome hurd-dev.qcow2)
./vm/run-hurd64-smp.sh        # test -smp 2 sur overlay jetable — attendu : échec
./vm/run-hurd64-vioif.sh      # test vioif (bascule réseau) sur overlay jetable
```

> **Sécurité disque.** Ne jamais démarrer l'image de base `debian-hurd-amd64-*.img`
> en écriture, et ne jamais la ré-extraire/écraser pendant qu'un overlay la
> référence comme *backing file* (c'est ce qui a corrompu le disque le 17/10).
> Pour tout essai risqué (bascule réseau, SMP), passer par un overlay jetable
> (`hurd-*-test.qcow2`), ou `qemu-img snapshot` avant.

## Images VM

Volontairement **exclues** du dépôt (plusieurs Go) : image de base Debian
`debian-hurd-amd64-*.img` (lecture seule, ne pas modifier) + copie de travail
autonome `hurd-dev.qcow2` (voir `.gitignore`).
