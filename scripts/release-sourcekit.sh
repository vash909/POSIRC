#!/bin/bash
# Accompany static LGPL C-library binaries with matching sources and build recipes.
set -euo pipefail
version=$(sed -n 's/^#define POSIRC_VERSION "\([^"]*\)"/\1/p' include/app.h)
name="posirc-$version-uclibc-source"
stage="dist/$name"
mkdir -p "$stage"
fetch() {
    curl -4 -fsSL --connect-timeout 20 --retry 3 "$1" -o "$2"
    printf '%s  %s\n' "$3" "$2" | sha256sum -c -
}
fetch https://toolchains.bootlin.com/downloads/releases/sources/uclibc-1.0.59/uClibc-ng-1.0.59.tar.xz \
    "$stage/uClibc-ng-1.0.59.tar.xz" \
    86c6f15971bfb14156850f2e36906dac5a69df7b3e688216d1aea09d2db54174
fetch https://codeload.github.com/bootlin/buildroot-toolchains/tar.gz/48bfc2c656979dbbb24277ffc6746ea7a6af4a50 \
    "$stage/buildroot-toolchains-48bfc2c656979dbbb24277ffc6746ea7a6af4a50.tar.gz" \
    189155cfd0f82b7b7938b306ca2926b68af177b68adcb8a786a737f7bf40c0bd
fetch https://raw.githubusercontent.com/qemu/qemu/84f07211cc5b4fc6a371559bf8a5de4fb068e648/target/xtensa/core-test_kc705_be/xtensa-modules.c.inc \
    "$stage/xtensaeb-binutils-isa-module.c" \
    953cda24820aaaf6bc9e9154f6c36b8d1d3190ad5c83abaa3ea30a113d5d11bd
sed -i '1i#include "ansidecl.h"' "$stage/xtensaeb-binutils-isa-module.c"
for archive in dist/posirc-"$version"-linux-*-uclibc.tar.gz; do
    target=${archive#dist/posirc-$version-linux-}
    target=${target%-uclibc.tar.gz}
    tar -xOzf "$archive" --wildcards '*/licenses/toolchain-buildroot.config' > "$stage/$target.buildroot.config"
    tar -xOzf "$archive" --wildcards '*/licenses/uClibc_config.h' > "$stage/$target.uClibc_config.h"
done
cp docs/releases/RELINK.txt "$stage/"
tar -xOf "$stage/uClibc-ng-1.0.59.tar.xz" uClibc-ng-1.0.59/COPYING.LIB > "$stage/COPYING.LIB"
tar -czf "dist/$name.tar.gz" -C dist "$name"
