# Portability and release validation

The release manifest in `scripts/release-targets.json` is the exact binary asset
set. All of its targets must pass before publication; a successful cross-link
alone is insufficient. This release expands the original nineteen packages to
sixty-two, with the same application functionality throughout.

## What runs on every target

The CI executes `make all test ui-test integration-test` using the target compiler,
C library, curses implementation and TLS library. It packages that executable,
then checks `posirc --version`. Tests include:

- IRC framing, limits and case mapping; channel/query routing, NAMES/MODE races,
  unread state, settings, WHOIS, LIST, ACTION and message histories.
- UTF-8 editing, truncation, nickname limits, configuration loading/atomic saving
  and the single-entry input recall.
- Actual curses cell inspection for `xterm-256color` and `vt100`, including
  horizontal input scrolling, wrapped chats, clipped server replies, combining
  marks, CJK, emoji, colors and sidebars. No visual features are disabled.
- Real loopback TLS 1.2 and 1.3 sessions, hostname/IP/CA/expiry rejection,
  registration, PING/PONG, joins, message delivery, bounded queues and errors.
- Fork/exec of the actual CLI with redirected descriptors: fragmented stdin,
  literal text, Unicode, EOF, output streaming and closed downstream pipes.

The integration fixture uses three distinct, public RSA-2048 test keys. They
avoid repeated prime generation on slow emulators; certificates are freshly
constructed and include valid, expired, wrong-host and untrusted-issuer cases.
Those keys are only compiled into the test program, never into POSIRC.

Linux cross tests use binfmt so a test's child `exec("./posirc", ...)` also runs
the target client. Additional CPU jobs pin Debian's static QEMU 11.1.3 package
and its SHA-256, rather than relying on a container's partial interpreter list.
The ELF class, endianness and machine ID select the interpreter. QEMU_CPU selects
a particular CPU model where the SDK requires one.

## CPU and ABI details

The eight original Linux packages use Alpine 3.22's architecture baselines.
`x86` means i686 with CMOV/SSE2, not an original 80386; `armhf` means ARMv6
hard-float, and `armv7` means ARMv7 hard-float.

Additional musl packages use Bootlin's stable-2026.08-1 SDKs, GCC 15.3.0 and
musl 1.2.6, except Hexagon. Dependency sources are checksum-pinned OpenSSL 3.5.9
and wide ncurses 6.6. OpenSSL's portable C code is selected with `no-asm`;
verified TLS, certificate checks and both tested protocol versions remain enabled.

| Archive architecture | Data model / byte order / baseline |
| --- | --- |
| `aarch64_be` | ARM64, 64-bit pointers, big-endian |
| `armebv7` | ARMv7 EABI hard-float, big-endian; tested as Cortex-A15 |
| `riscv32` | RV32 ILP32D: 32-bit pointers, double-precision floating-point ABI |
| `powerpc` | PowerPC32 big-endian, e300c3 baseline |
| `powerpc64` | PowerPC64 big-endian, POWER8 baseline; distinct from `ppc64le` |
| `mips`, `mipsel` | MIPS32 O32, big-endian and little-endian |
| `mips64n32`, `mips64eln32` | MIPS64 instruction set, N32 ABI, 32-bit pointers, BE/LE |
| `mips64`, `mips64el` | MIPS64 N64 ABI, 64-bit pointers, BE/LE; glibc packages |
| `microblaze`, `microblazeel` | MicroBlaze Linux, big-endian and little-endian |
| `or1k` | OpenRISC Linux, big-endian |
| `sh4`, `sh4eb` | SuperH: SH4 little-endian / SH4A big-endian SDKs; QEMU sh7750r / sh7785 |
| `m68k` | Linux m68k, tested on QEMU m68040; not certification for every 68000 |
| `alpha` | Alpha Linux, little-endian, 64-bit pointers; glibc package |
| `hppa` | PA-RISC Linux, big-endian, 32-bit pointers; glibc package |
| `sparc64` | SPARC64 Linux, big-endian, 64-bit pointers; glibc package |
| `sparc` | SPARC V8 Linux, big-endian, 32-bit pointers; uClibc-ng |
| `sparc32plus` | SPARC Linux with V9 instructions and a 32-bit ABI; uClibc-ng |
| `xtensa` | Xtensa dc233c little-endian; configured core ISA, not generic ESP hardware |
| `xtensaeb` | Xtensa stock FSF big-endian Linux; SDK built from pinned Buildroot sources |
| `hexagon` | Hexagon Linux/musl, 32-bit; Qualcomm LLVM 22.1.8 SDK |
| `loongarch64` | openEuler 24.03 LTS SP4's native LoongArch64 ABI and shared libraries |

The glibc builds use Ubuntu 24.04 cross packages and require glibc 2.39+.
OpenSSL/ncurses and GCC runtime helpers are embedded; the C library stays dynamic.
The CI generates UTF-8 locale data in the target's byte order: host x86 locale
files cannot simply be reused on big-endian glibc targets.
SPARC32 and both Xtensa SDKs use uClibc-ng 1.0.59. The FSF big-endian SDK is
built from the pinned Buildroot tree in CI; other Bootlin SDKs are prebuilt. Hexagon uses the SDK's
`hexagon-v1.2.4-apr-2026` musl fork and LLVM compiler runtime.
The release also supplies a companion uClibc source archive with the matching
Buildroot recipes/patches and per-target configurations, alongside POSIRC's
complete source. See [relinking instructions](RELINK.txt).
Exact toolchain URLs/checksums are in `scripts/release-linux-toolchains.json`.

`BUILD-INFO.txt` records the source commit, compiler, flags, dependency versions,
ELF headers/attributes and test method. For example, an ELF64 MIPS binary with
N64 has a different loader/data layout from an ELF32 N32 executable despite both
running a MIPS64 ISA. A CPU family label alone does not make binaries compatible.

## Other Unix systems

FreeBSD 14.4, OpenBSD 7.9 and NetBSD 10.1 use base-system curses/TLS libraries;
NetBSD RISC-V needs 11.0. OpenBSD uses LibreSSL. Their archives reject unexpected
`/usr/local` or `/usr/pkg` dependencies during packaging.

Additional VMs cover DragonFly, MidnightBSD, GhostBSD, HardenedBSD, OPNsense,
NextBSD x86-64/ARM64, Solaris, three illumos distributions, GNU Hurd x86-64/i386,
Haiku x86-64 and openEuler LoongArch64. These are OS-specific binaries, with
installed package versions and loader dependencies recorded in each archive.
They are not interchangeable merely because they share a CPU or kernel ancestry.
See `INSTALL.txt` for the corresponding runtime packages.

macOS 15+ builds embed OpenSSL/ncurses, link Apple's system libraries and receive
an ad-hoc signature. Tests execute on native Apple Silicon and Intel runners.

## Research boundaries

Every OS/CPU combination and both byte orders in the
[project's portability list](https://xzgroup.net/posirc.html) is covered by the
release matrix. A release requires a working userspace, compatible compiler/
sysroot, an emulator CPU configuration and a complete passing suite.

The original MIPS N64 musl SDK host is unreachable from GitHub runners.
N64 packages instead use tested glibc cross toolchains; both byte orders
remain covered, with all client features enabled.

A bare-metal Hexagon or Xtensa compiler cannot supply POSIX sockets, curses,
TLS trust-store access and a Unix terminal. The published Hexagon and Xtensa
packages explicitly target Linux userspace. They are not firmware for arbitrary
DSP boards or microcontrollers.

Emulated tests do not certify every physical CPU, kernel version, terminal,
libc patch level, CA-store layout or derivative distribution. Download the
matching OS/CPU/ABI, check the metadata and verify the archive checksum.
