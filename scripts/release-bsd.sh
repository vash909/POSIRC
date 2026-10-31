#!/bin/sh
# Build inside the target BSD VM, using that OS's base-system libraries.
set -eu
: "${RELEASE_OS:?missing OS}"
: "${RELEASE_ARCH:?missing CPU}"
: "${RELEASE_TARGET:?missing target}"
: "${GITHUB_SHA:?missing revision}"
version=$(sed -n 's/^#define POSIRC_VERSION "\([^"]*\)"/\1/p' include/app.h)
case "$version" in *-beta.[0-9]*) ;; *) exit 1 ;; esac
name="posirc-$version-$RELEASE_TARGET"
stage="dist/$name"
mkdir -p "$stage"
flags='-std=c99 -O2 -Wall -Wextra -Wpedantic -Werror'
case "$RELEASE_OS" in
freebsd) test "$(uname -s)" = FreeBSD; curses_libs=-lncursesw ;;
openbsd) test "$(uname -s)" = OpenBSD; curses_libs=-lcurses ;;
netbsd) test "$(uname -s)" = NetBSD; curses_libs=-lcurses ;;
*) exit 1 ;;
esac
make clean
make all test ui-test integration-test CFLAGS="$flags" CURSES_LIBS="$curses_libs"
strip posirc
test "$(./posirc --version)" = "POSIRC $version"
ldd posirc > "$stage/dependencies.txt"
# These packages must run without pkg/pkgsrc libraries being installed.
if grep -E '/usr/local/|/usr/pkg/|not found' "$stage/dependencies.txt"; then
    echo 'Unexpected external runtime dependency' >&2
    exit 1
fi
cp posirc README.md LICENSE "$stage/"
cp docs/releases/INSTALL.txt "$stage/INSTALL.txt"
{
    printf 'POSIRC version: %s\nSource commit: %s\nTarget: %s\n' "$version" "$GITHUB_SHA" "$RELEASE_TARGET"
    printf 'Compiler flags: %s\nExecution: QEMU full-system VM\n' "$flags"
    printf 'Tests: unit, curses, local TLS integration, real CLI pipe scenarios\n'
    printf 'Libraries: dynamically linked base-system curses and TLS; see dependencies.txt\n'
    uname -a
    cc --version
    openssl version -a
    file posirc
    cat "$stage/dependencies.txt"
} > "$stage/BUILD-INFO.txt"
tar -czf "dist/$name.tar.gz" -C dist "$name"
