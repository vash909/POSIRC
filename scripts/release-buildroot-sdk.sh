#!/bin/bash
# Build the stock big-endian Xtensa FSF Linux SDK; no vendor core overlay needed.
set -euo pipefail
root=$PWD
archive="$root/cross/xtensaeb-sdk.tar.gz"
if [ -f "$archive" ]; then exit 0; fi
work="$root/cross/xtensaeb-buildroot"
mkdir -p "$work"
curl -4 -fsSL --connect-timeout 20 --retry 3 \
    https://codeload.github.com/bootlin/buildroot-toolchains/tar.gz/48bfc2c656979dbbb24277ffc6746ea7a6af4a50 \
    -o "$work/source.tar.gz"
printf '%s  %s\n' 189155cfd0f82b7b7938b306ca2926b68af177b68adcb8a786a737f7bf40c0bd "$work/source.tar.gz" | sha256sum -c -
mkdir -p "$work/source"
tar -xf "$work/source.tar.gz" -C "$work/source" --strip-components=1
cp scripts/release-xtensaeb.config "$work/source/.config"
make -C "$work/source" olddefconfig
# Stock binutils uses a little-endian generated ISA module even when its
# default xtensa-config.h/GCC configuration declares big-endian. Use the
# matching, generated ISA module from the exact QEMU core tested below.
make -C "$work/source" host-binutils-extract
curl -4 -fsSL --connect-timeout 20 --retry 3 \
    https://raw.githubusercontent.com/qemu/qemu/84f07211cc5b4fc6a371559bf8a5de4fb068e648/target/xtensa/core-test_kc705_be/xtensa-modules.c.inc \
    -o "$work/xtensa-modules.c"
printf '%s  %s\n' 953cda24820aaaf6bc9e9154f6c36b8d1d3190ad5c83abaa3ea30a113d5d11bd "$work/xtensa-modules.c" | sha256sum -c -
isa_module=$(find "$work/source/output/build" -path '*/bfd/xtensa-modules.c' | head -1)
test -n "$isa_module"
cp "$work/xtensa-modules.c" "$isa_module"
make -C "$work/source" -j2 sdk
sdk=$(find "$work/source/output/images" -name '*_sdk-buildroot.tar.gz' | head -1)
test -n "$sdk"
mkdir -p "$work/stage"
tar -xf "$sdk" -C "$work/stage" --strip-components=1
cp "$work/source/.config" "$work/stage/buildroot.config"
printf '%s\n' 'Xtensa FSF big-endian Linux SDK, built from Bootlin Buildroot 48bfc2c656979dbbb24277ffc6746ea7a6af4a50.' \
    'Big-endian binutils ISA module: QEMU 84f07211cc5b4fc6a371559bf8a5de4fb068e648 core-test_kc705_be.' \
    'Sources, patches and configuration are included in the release uClibc source kit.' > "$work/stage/README.txt"
tar -czf "$archive" -C "$work/stage" .
sha256sum "$archive"
