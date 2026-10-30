#!/bin/sh
# Build and test the same executable that goes into the release archive.
set -eu
: "${RELEASE_TARGET:?missing release target}"
: "${RELEASE_OS:?missing release OS}"
: "${GITHUB_SHA:?missing source revision}"
version=$(sed -n 's/^#define POSIRC_VERSION "\([^"]*\)"/\1/p' include/app.h)
case "$version" in *-beta.[0-9]*) ;; *) echo 'Expected a beta version' >&2; exit 1 ;; esac
name="posirc-$version-$RELEASE_TARGET"
stage="dist/$name"
mkdir -p "$stage/licenses"
flags='-std=c99 -O2 -Wall -Wextra -Wpedantic -Werror'

case "$RELEASE_OS" in
linux)
    test "$(apk --print-arch)" = "$RELEASE_ARCH"
    apk add --no-cache build-base pkgconf ncurses-dev ncurses-static ncurses-terminfo \
        openssl-dev openssl-libs-static ca-certificates curl file
    make clean
    make all test ui-test integration-test CFLAGS="$flags" LDFLAGS=-static \
        CURSES_LIBS="$(pkg-config --static --libs ncursesw)" \
        TLS_LIBS="$(pkg-config --static --libs openssl)"
    # A release advertised as static must have neither a loader nor DT_NEEDED.
    if readelf -l posirc | grep -q INTERP; then exit 1; fi
    if readelf -d posirc | grep -q NEEDED; then exit 1; fi
    strip posirc
    musl_version=$(apk info -v musl | sed -n 's/^musl-\(.*\)-r[0-9]*$/\1/p' | head -1)
    curl --fail --location --retry 3 \
        "https://git.musl-libc.org/cgit/musl/plain/COPYRIGHT?h=v$musl_version" \
        -o "$stage/licenses/musl-COPYRIGHT"
    gcc_version=$(gcc -dumpfullversion)
    for license in COPYING3 COPYING.RUNTIME; do
        curl --fail --location --retry 3 \
            "https://raw.githubusercontent.com/gcc-mirror/gcc/releases/gcc-$gcc_version/$license" \
            -o "$stage/licenses/gcc-$license"
    done
    apk info -v > "$stage/packages.txt"
    ;;
macos)
    test "$(uname -m)" = "$RELEASE_ARCH"
    ssl=$(brew --prefix openssl@3)
    curses=$(brew --prefix ncurses)
    export PKG_CONFIG_PATH="$ssl/lib/pkgconfig:$curses/lib/pkgconfig"
    export MACOSX_DEPLOYMENT_TARGET=15.0
    curses_libs="$curses/lib/libncursesw.a"
    if [ -f "$curses/lib/libtinfow.a" ]; then curses_libs="$curses_libs $curses/lib/libtinfow.a"; fi
    make clean
    make all test ui-test integration-test CC=clang CFLAGS="$flags" \
        CPPFLAGS="-Iinclude -I$ssl/include -I$curses/include" \
        CURSES_LIBS="$curses_libs" TLS_LIBS="$ssl/lib/libssl.a $ssl/lib/libcrypto.a -pthread"
    # Disallow a binary that only runs on a machine with this Homebrew prefix.
    if otool -L posirc | tail -n +2 | grep -vE '^[[:space:]]*(/usr/lib/|/System/Library/)'; then
        echo 'Unexpected non-system dynamic dependency' >&2
        exit 1
    fi
    strip posirc
    codesign --force --sign - posirc
    codesign --verify --strict posirc
    brew list --versions openssl@3 ncurses > "$stage/packages.txt"
    ;;
*) exit 1 ;;
esac

test "$(./posirc --version)" = "POSIRC $version"
# Copy the notices for the exact installed upstream library versions.
ssl_version=$(pkg-config --modversion openssl)
ncurses_version=$(pkg-config --modversion ncursesw)
ncurses_tag="v$(printf '%s' "$ncurses_version" | tr . _)"
curl --fail --location --retry 3 \
    "https://raw.githubusercontent.com/openssl/openssl/openssl-$ssl_version/LICENSE.txt" \
    -o "$stage/licenses/OpenSSL-LICENSE.txt"
curl --fail --location --retry 3 \
    "https://raw.githubusercontent.com/ThomasDickey/ncurses-snapshots/$ncurses_tag/COPYING" \
    -o "$stage/licenses/ncurses-COPYING"
cp posirc README.md "$stage/"
cp docs/releases/INSTALL.txt "$stage/INSTALL.txt"
{
    printf 'POSIRC version: %s\nSource commit: %s\nTarget: %s\n' "$version" "$GITHUB_SHA" "$RELEASE_TARGET"
    printf 'Compiler flags: %s\nOpenSSL: %s\nncurses: %s\n' "$flags" "$ssl_version" "$ncurses_version"
    printf 'Tests: unit, curses, local TLS integration, real CLI pipe scenarios\n'
    printf 'Execution: %s\n' "$RELEASE_EXECUTION"
    uname -a
    cc --version
    file posirc
    if [ "$RELEASE_OS" = linux ]; then readelf -h posirc; else otool -L posirc; fi
} > "$stage/BUILD-INFO.txt"
tar -czf "dist/$name.tar.gz" -C dist "$name"
