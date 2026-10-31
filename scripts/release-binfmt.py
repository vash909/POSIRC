#!/usr/bin/env python3
"""Register the selected Linux ELF interpreter on an ephemeral CI runner.

Some distribution/binfmt container presets omit working QEMU user targets.
Register the target explicitly so fork/exec tests also run the actual client.
This script is CI setup, never part of the installed client.
"""
import os
import pathlib
import struct

arch = os.environ['RELEASE_ARCH']
# ELF class, byte order, machine, QEMU executable suffix.
targets = {
    'armebv7': (1, 2, 40, 'armeb'),
    'riscv32': (1, 1, 243, 'riscv32'),
    'powerpc': (1, 2, 20, 'ppc'),
    'powerpc64': (2, 2, 21, 'ppc64'),
    'mips': (1, 2, 8, 'mips'),
    'mipsel': (1, 1, 8, 'mipsel'),
    'mips64n32': (1, 2, 8, 'mipsn32'),
    'mips64eln32': (1, 1, 8, 'mipsn32el'),
    'mips64': (2, 2, 8, 'mips64'),
    'mips64el': (2, 1, 8, 'mips64el'),
    'sh4': (1, 1, 42, 'sh4'),
    'sh4eb': (1, 2, 42, 'sh4eb'),
    'm68k': (1, 2, 4, 'm68k'),
    'aarch64_be': (2, 2, 183, 'aarch64_be'),
    'microblaze': (1, 2, 189, 'microblaze'),
    'microblazeel': (1, 1, 189, 'microblazeel'),
    'or1k': (1, 2, 92, 'or1k'),
    'hexagon': (1, 1, 164, 'hexagon'),
    'alpha': (2, 1, 0x9026, 'alpha'),
    'hppa': (1, 2, 15, 'hppa'),
    'sparc': (1, 2, 2, 'sparc'),
    'sparc32plus': (1, 2, 18, 'sparc32plus'),
    'sparc64': (2, 2, 43, 'sparc64'),
    'xtensa': (1, 1, 94, 'xtensa'),
    'xtensaeb': (1, 2, 94, 'xtensaeb'),
}
if arch in targets:
    elf_class, byte_order, machine, emulator = targets[arch]
    interpreter = pathlib.Path(os.environ.get('POSIRC_QEMU_DIR', '/usr/bin')) / ('qemu-' + emulator)
    if not interpreter.is_file():
        raise SystemExit('Missing interpreter: ' + str(interpreter))
    magic = bytearray(b'\x7fELF' + bytes([elf_class, byte_order, 1]) + bytes(9))
    magic += struct.pack('<HH' if byte_order == 1 else '>HH', 2, machine)
    mask = bytearray(b'\xff' * 7 + bytes(9) + b'\xff' * 4)
    mask[16 if byte_order == 1 else 17] = 0xfe  # ET_EXEC or ET_DYN
    escape = lambda b: ''.join('\\x%02x' % v for v in b)
    registration = ':posirc-qemu-%s:M::%s:%s:%s:F\n' % (
        arch, escape(magic), escape(mask), interpreter)
    previous = pathlib.Path('/proc/sys/fs/binfmt_misc/posirc-qemu-' + arch)
    if previous.exists():
        previous.write_text('-1\n')
    pathlib.Path('/proc/sys/fs/binfmt_misc/register').write_text(registration)
    print(pathlib.Path('/proc/sys/fs/binfmt_misc/posirc-qemu-' + arch).read_text())
