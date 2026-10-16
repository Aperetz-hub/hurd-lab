#!/bin/bash
# Test SMP 2 CPU - attendu: boot mono OK, AP en cours (SMP cassé upstream)
# Usage: ./run-hurd64-smp.sh [img]
set -e
cd "$(dirname "$0")"
IMG=${1:-$(echo debian-hurd*.img | tr ' ' '\n' | head -n1)}
exec qemu-system-x86_64 -m 2048 -M q35 --enable-kvm -smp 2 \
  -drive file="$IMG",cache=writeback,snapshot=on \
  -net nic,model=e1000 -net user,hostfwd=tcp:127.0.0.1:2223-:22 \
  -serial stdio "$@"
