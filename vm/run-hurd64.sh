#!/bin/bash
# Lance Debian Hurd amd64 64-bit sous QEMU - base fork x86_64
# Usage: ./run-hurd64.sh [img] ; snapshot par défaut, ssh sur 2222
set -e
cd "$(dirname "$0")"
IMG=${1:-$(echo debian-hurd*.img | tr ' ' '\n' | head -n1)}
[ -f "$IMG" ] || { echo "image introuvable dans $(pwd)"; exit 1; }
exec qemu-system-x86_64 -m 2048 -M q35 --enable-kvm \
  -drive file="$IMG",cache=writeback \
  -net nic,model=e1000 -net user,hostfwd=tcp:127.0.0.1:2222-:22 \
  "$@"
