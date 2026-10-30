#!/usr/bin/env python3
"""Compare C text/history kernels against a source checkout (Linux/GNU linker)."""
import argparse
import csv
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--before', type=Path, required=True)
parser.add_argument('--runs', type=int, default=5)
parser.add_argument('--output', type=Path, default=ROOT/'docs/benchmarks/core.csv')
args = parser.parse_args()
if args.runs < 1:
    parser.error('--runs must be positive')
rows = []
with tempfile.TemporaryDirectory(prefix='posirc-core-') as directory:
    binaries = []
    for label, source in [('before', args.before), ('after', ROOT)]:
        binary = Path(directory)/label
        subprocess.run(['cc', '-O2', '-I'+str(source/'include'), str(ROOT/'benchmarks/core.c'),
                        *[str(source/'src'/name) for name in ('buffer.c', 'text.c', 'irc.c')],
                        '-Wl,--wrap=malloc,--wrap=realloc,--wrap=free', '-o', str(binary)], check=True)
        binaries.append((label, binary))
    for run in range(1, args.runs+1):
        for label, binary in binaries:
            output = subprocess.check_output([str(binary)], text=True)
            for line in output.splitlines():
                workload, metric, value = line.split(',')
                rows.append(dict(run=run, build=label, workload=workload, metric=metric, value=value))
args.output.parent.mkdir(parents=True, exist_ok=True)
with args.output.open('w') as file:
    writer = csv.DictWriter(file, fieldnames=list(rows[0]), lineterminator='\n')
    writer.writeheader()
    writer.writerows(rows)
