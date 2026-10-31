#!/bin/bash
# Recent, static QEMU userspace interpreters; unpack without changing host packages.
set -euo pipefail
mkdir -p cross/qemu
curl -4 -fsSL --connect-timeout 20 --retry 3 \
    https://deb.debian.org/debian/pool/main/q/qemu/qemu-user_11.1.3+ds-1_amd64.deb \
    -o cross/qemu/package.deb
printf '%s  %s\n' 5fa915e759f314400cf7d64a2745747e6baa50a180c938b3e8d281ce57ede452 cross/qemu/package.deb | sha256sum -c -
dpkg-deb -x cross/qemu/package.deb cross/qemu
if [ ! -e /proc/sys/fs/binfmt_misc/register ]; then
    sudo mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
fi
sudo env RELEASE_ARCH="$RELEASE_ARCH" POSIRC_QEMU_DIR="$PWD/cross/qemu/usr/bin" \
    python3 scripts/release-binfmt.py
