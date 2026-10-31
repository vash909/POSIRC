#!/bin/bash
# Cross-build static Linux binaries, then execute ALL client tests via binfmt/QEMU.
set -euo pipefail
: "${RELEASE_ARCH:?missing CPU}"
: "${GITHUB_SHA:?missing revision}"
root=$PWD
read -r url checksum bits cpu libc libc_version gcc_version apt_triple extra_cflags < <(python3 - "$RELEASE_ARCH" <<'PY'
import json,sys
r=next(x for x in json.load(open('scripts/release-linux-toolchains.json')) if x['arch']==sys.argv[1])
libc=r.get('libc','musl')
print(r['url'],r['sha256'],r['bits'],r['qemu_cpu'] or '-',libc,r['components'][libc],r['components']['gcc'],r.get('apt_triple','-'),r.get('extra_cflags') or '-')
PY
)
if [ "$cpu" != - ]; then export QEMU_CPU="$cpu"; fi
if [ "$extra_cflags" = - ]; then extra_cflags=; fi
work="$root/cross/$RELEASE_ARCH"
prefix="$root/cross/prefix/$RELEASE_ARCH"
mkdir -p "$work" "$prefix/licenses"
fetch() {
    curl -fsSL --retry 3 "$1" -o "$2"
    printf '%s  %s\n' "$3" "$2" | sha256sum -c -
}
if [ "$url" = apt ]; then
    sudo apt-get install -y "gcc-$apt_triple" "libc6-dev-${RELEASE_ARCH}-cross"
    compiler="/usr/bin/$apt_triple-gcc"
else
    fetch "$url" "$work/toolchain.tar.xz" "$checksum"
    mkdir -p "$work/toolchain"
    tar -xf "$work/toolchain.tar.xz" -C "$work/toolchain" --strip-components=1
    if [ -f "$work/toolchain/relocate-sdk.sh" ]; then (cd "$work/toolchain" && ./relocate-sdk.sh); fi
    compiler=$(find "$work/toolchain/bin" -maxdepth 1 -name '*-gcc' | head -1)
fi
test -n "$compiler"
cross_prefix=${compiler%gcc}
triple=$($compiler -dumpmachine)
linkflags=-static
atomic_libs=-latomic
if [ "$libc" = glibc ]; then
    export QEMU_LD_PREFIX="/usr/$triple"
    linkflags=-static-libgcc
    atomic_libs='-Wl,-Bstatic -latomic -Wl,-Bdynamic'
fi
if [ ! -f "$prefix/complete" ]; then
    fetch https://www.openssl.org/source/openssl-3.5.9.tar.gz "$work/openssl.tar.gz" \
        603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
    fetch https://ftp.gnu.org/gnu/ncurses/ncurses-6.6.tar.gz "$work/ncurses.tar.gz" \
        355b4cbbed880b0381a04c46617b7656e362585d52e9cf84a67e2009b749ff11
    mkdir -p "$work/openssl" "$work/ncurses"
    tar -xf "$work/openssl.tar.gz" -C "$work/openssl" --strip-components=1
    tar -xf "$work/ncurses.tar.gz" -C "$work/ncurses" --strip-components=1
    (
        cd "$work/openssl"
        perl ./Configure "linux-generic$bits" no-shared no-tests no-asm no-dso no-module \
            --cross-compile-prefix="$cross_prefix" --prefix="$prefix" --libdir=lib --openssldir=/etc/ssl -latomic $extra_cflags
        make -j2
        make install_sw
        cp LICENSE.txt "$prefix/licenses/OpenSSL-LICENSE.txt"
    )
    (
        cd "$work/ncurses"
        CFLAGS="$extra_cflags -O2" CC="$compiler" AR="${cross_prefix}ar" RANLIB="${cross_prefix}ranlib" \
            ./configure --host="$triple" --build="$(./config.guess)" --prefix="$prefix" \
            --enable-widec --without-shared --with-normal --without-debug --without-ada \
            --without-cxx --without-cxx-binding --without-progs --without-tests \
            --disable-db-install --with-terminfo-dirs=/etc/terminfo:/lib/terminfo:/usr/share/terminfo
        make -j2
        make install.libs install.includes
        cp COPYING "$prefix/licenses/ncurses-COPYING"
    )
    case "$libc" in
    musl)
        fetch_url="https://git.musl-libc.org/cgit/musl/plain/COPYRIGHT?h=v$libc_version"
        ;;
    glibc)
        fetch_url="https://raw.githubusercontent.com/bminor/glibc/glibc-$libc_version/COPYING.LIB"
        ;;
    uclibc)
        fetch_url="https://raw.githubusercontent.com/wbx-github/uclibc-ng/v$libc_version/COPYING.LIB"
        ;;
    esac
    curl -fsSL --retry 3 "$fetch_url" -o "$prefix/licenses/$libc-LICENSE"
    for license in COPYING3 COPYING.RUNTIME; do
        curl -fsSL --retry 3 "https://raw.githubusercontent.com/gcc-mirror/gcc/releases/gcc-$gcc_version/$license" \
            -o "$prefix/licenses/gcc-$license"
    done
    touch "$prefix/complete"
fi
flags="-std=c99 -O2 -Wall -Wextra -Wpedantic -Werror $extra_cflags"
make clean
make all test ui-test integration-test CC="$compiler" CFLAGS="$flags" \
    CPPFLAGS="-Iinclude -I$prefix/include -I$prefix/include/ncursesw" LDFLAGS="$linkflags" \
    CURSES_LIBS="$prefix/lib/libncursesw.a" TLS_LIBS="$prefix/lib/libssl.a $prefix/lib/libcrypto.a" \
    LDLIBS="-pthread -ldl $atomic_libs"
version=$(sed -n 's/^#define POSIRC_VERSION "\([^"]*\)"/\1/p' include/app.h)
test "$(./posirc --version)" = "POSIRC $version"
if [ "$libc" != glibc ]; then
    if readelf -l posirc | grep INTERP; then exit 1; fi
    if readelf -d posirc | grep NEEDED; then exit 1; fi
fi
"${cross_prefix}strip" posirc
name="posirc-$version-linux-$RELEASE_ARCH-$libc"
stage="dist/$name"
mkdir -p "$stage"
cp posirc README.md LICENSE "$stage/"
cp -r "$prefix/licenses" "$stage/"
cp docs/releases/INSTALL.txt "$stage/INSTALL.txt"
{
    printf 'POSIRC version: %s\nSource commit: %s\nTarget: linux-%s-%s\n' "$version" "$GITHUB_SHA" "$RELEASE_ARCH" "$libc"
    printf 'Execution: QEMU user-mode via binfmt\nQEMU CPU: %s\nCompiler flags: %s\n' "$cpu" "$flags"
    printf 'Tests: unit, config, curses, local TLS integration, real CLI pipe scenarios\n'
    printf 'OpenSSL: 3.5.9 (no assembler)\nncurses: 6.6\n%s: %s\n' "$libc" "$libc_version"
    "$compiler" --version
    file posirc
    readelf -h posirc
    cat scripts/release-linux-toolchains.json
} > "$stage/BUILD-INFO.txt"
tar -czf "dist/$name.tar.gz" -C dist "$name"
