# hurd-lab — fork GNU/Hurd x86_64 : SMP sans hang + rumpnet virtio

Fork expérimental de GNU/Hurd (64-bit) avec deux objectifs :
1. **gnumach** : ne plus pendre le boot quand les AP ne démarrent pas
   (dégradation propre vers mono-CPU au lieu de `while (1)`).
2. **rumpnet** : ajouter le support réseau **virtio-net** (pilotes
   `rumpdev_virtio_if_vioif` / `rumpdev_pci_virtio`, absents des paquets Debian).

État : noyau patché validé en QEMU (`-smp 1` boot + SSH OK, AP détecté en
`-smp 2` mais races SMP amont ensuite) ; attachement `vioif0` obtenu avec le
bon MAC QEMU, crash restant dans `vioif_set_rx_filter` en cours d'analyse.
Voir `docs/` et les messages de commit pour les détails.

## Contenu

- `src/` — *non versionné ici* : clones complets de
  [`hurd/gnumach`](https://git.savannah.gnu.org/cgit/hurd/gnumach.git),
  [`hurd/hurd`](https://git.savannah.gnu.org/cgit/hurd/hurd.git) et
  [`hurd/mig`](https://git.savannah.gnu.org/cgit/hurd/mig.git).
  Travail sur branches `fork/x86_64` (poussées telles quelles sur GitHub,
  voir ci-dessous).
- `vm/` — scripts QEMU (`-M q35`, `e1000`, ssh sur localhost:2222).
- `patches/` — patchs consolidés + outillage de build rump.
- `docs/` — notes de suivi.

## Branches publiées

- `main` — ce projet (docs, scripts, patchs).
- `fork/gnumach-x86_64` — historique gnumach + 2 commits SMP.
- `fork/hurd-x86_64` — historique hurd + 3 commits (bpf_hdr, login utmp, pcn).

## Démarrage rapide (hôte Debian)

```
sudo apt install qemu-system-x86 qemu-utils
# image Debian hurd-amd64 officielle :
# https://cdimage.debian.org/cdimage/ports/latest/hurd-amd64/debian-hurd.img.tar.gz
./vm/run-xfce.sh        # bureau XFCE (login demo/demo)
./vm/run-hurd64.sh      # console + ssh -p 2222
```

## Images VM

Volontairement **exclues** du dépôt (plusieurs Go) : image de base Debian
`debian-hurd-amd64-*.img` + overlay qcow2 de dev (voir `.gitignore`).
