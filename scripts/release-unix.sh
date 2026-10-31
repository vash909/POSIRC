#!/bin/sh
# Build the complete client and execute every suite inside an additional VM.
set -eu
: "${RELEASE_OS:?missing OS}"
: "${RELEASE_ARCH:?missing architecture}"
: "${RELEASE_TARGET:?missing target}"
: "${GITHUB_SHA:?missing revision}"
version=$(sed -n 's/^#define POSIRC_VERSION "\([^"]*\)"/\1/p' include/app.h)
name="posirc-$version-$RELEASE_TARGET"
stage="dist/$name"
mkdir -p "$stage"
uname -a
flags='-std=c99 -O2 -Wall -Wextra -Wpedantic -Werror'
cppflags=-Iinclude
ldflags=
libs=
cc=cc
make_command=make
curses_libs=-lncursesw
tls_libs='-lssl -lcrypto'
case "$RELEASE_OS" in
openeuler)
    dnf install -y gcc make ncurses-devel openssl-devel ca-certificates glibc-langpack-en
    ;;
hurd)
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y build-essential libncurses-dev libssl-dev ca-certificates locales
    ;;
haiku)
    pkgman install -y ncurses6_devel openssl3_devel make gcc
    cppflags="$cppflags -I/boot/system/develop/headers/ncurses"
    libs=-lnetwork
    ;;
solaris|omnios|openindiana)
    PATH="/usr/gnu/bin:/opt/gcc-14/bin:/opt/gcc-13/bin:/opt/gcc-12/bin:$PATH"; export PATH
    pkg_status=0
    pkg install --accept library/ncurses library/security/openssl || pkg_status=$?
    case "$pkg_status" in 0|4) ;; *) exit "$pkg_status" ;; esac
    cppflags="$cppflags -I/usr/include/ncursesw -I/usr/include/ncurses"
    cc=gcc
    flags="$flags -m64 -D__EXTENSIONS__"
    ldflags='-m64 -L/usr/lib/amd64 -R/usr/lib/amd64'
    libs='-lsocket -lnsl -lrt'
    command -v gmake >/dev/null 2>&1 && make_command=gmake
    ;;
tribblix)
    PATH="/usr/gnu/bin:/opt/gcc14/bin:/opt/gcc13/bin:/opt/gcc12/bin:$PATH"; export PATH
    zap install-overlay develop
    cc=gcc
    flags="$flags -m64 -D__EXTENSIONS__"
    ldflags='-m64 -L/usr/lib/amd64 -R/usr/lib/amd64'
    libs='-lsocket -lnsl -lrt'
    command -v gmake >/dev/null 2>&1 && make_command=gmake
    ;;
dragonflybsd|midnightbsd|ghostbsd|hardenedbsd|opnsense|nextbsd)
    if ! command -v make >/dev/null 2>&1; then
        pkg install -y gmake
        make_command=gmake
    fi
    if ! command -v cc >/dev/null 2>&1; then
        pkg install -y gcc
        cc=gcc
    fi
    ;;
*) exit 1 ;;
esac
"$cc" --version
openssl version -a
# Libraries installed by these OS package managers remain runtime dependencies.
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists ncursesw openssl; then
    cppflags="$cppflags $(pkg-config --cflags ncursesw openssl)"
    curses_libs=$(pkg-config --libs ncursesw)
    tls_libs=$(pkg-config --libs openssl)
fi
"$make_command" clean
"$make_command" all test ui-test integration-test CC="$cc" CFLAGS="$flags" \
    CPPFLAGS="$cppflags" LDFLAGS="$ldflags" LDLIBS="$libs" \
    CURSES_LIBS="$curses_libs" TLS_LIBS="$tls_libs"
strip posirc
test "$(./posirc --version)" = "POSIRC $version"
cp posirc README.md LICENSE "$stage/"
cp docs/releases/INSTALL.txt "$stage/INSTALL.txt"
{ ldd posirc || true; } > "$stage/dependencies.txt" 2>&1
{
    printf 'POSIRC version: %s\nSource commit: %s\nTarget: %s\n' "$version" "$GITHUB_SHA" "$RELEASE_TARGET"
    printf 'Compiler flags: %s\nExecution: QEMU full-system VM\n' "$flags"
    printf 'Tests: unit, config, curses, local TLS integration, real CLI pipe scenarios\n'
    printf 'Libraries: OS packages; see dependencies.txt and installed packages below\n'
    uname -a
    "$cc" --version
    openssl version -a
    cat "$stage/dependencies.txt"
    case "$RELEASE_OS" in
    openeuler) rpm -q glibc openssl-libs ncurses-libs ;;
    hurd) dpkg-query -W libc6 libssl3 libncursesw6 2>/dev/null || dpkg-query -W 'libssl*' 'libncurses*' ;;
    haiku) pkgman list ;;
    solaris|omnios|openindiana) pkg list ;;
    tribblix) pkginfo ;;
    *) pkg info 2>/dev/null || true ;;
    esac
} > "$stage/BUILD-INFO.txt"
tar -czf "dist/$name.tar.gz" -C dist "$name"
