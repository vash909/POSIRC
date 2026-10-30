#!/usr/bin/env python3
"""Render committed measurements; requires matplotlib only for plotting."""
import argparse
import csv
import json
from pathlib import Path
import statistics

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('directory', nargs='?', type=Path,
                    default=Path(__file__).resolve().parents[1] / 'docs/benchmarks')
args = parser.parse_args()
results = json.loads((args.directory / 'results.json').read_text())
rows = results['summaries']
phases = list(dict.fromkeys(r['phase'] for r in rows))
labels = ['Offline\nidle', 'TLS\nidle', '1 channel\n1,000 msg/s', '20 channels\n1,000 msg/s', '20 channels\nidle']
plt.rcParams.update({'font.family': 'DejaVu Sans', 'font.size': 10,
                     'axes.spines.top': False, 'axes.spines.right': False,
                     'svg.hashsalt': 'posirc-benchmark'})
fig, (memory, cpu) = plt.subplots(2, 1, figsize=(10, 7), layout='constrained')
for metric, color, offset, label in [('rss_mib', '#2563eb', -.18, 'RSS'),
                                    ('pss_mib', '#0d9488', .18, 'PSS')]:
    values = [[r[metric] for r in rows if r['phase'] == phase] for phase in phases]
    medians = [statistics.median(v) for v in values]
    memory.bar([i+offset for i in range(len(phases))], medians, width=.34,
               color=color, label=label,
               yerr=[[m-min(v) for m,v in zip(medians, values)],
                     [max(v)-m for m,v in zip(medians, values)]], capsize=3)
    for i, value in enumerate(medians):
        memory.text(i+offset, value+.25, f'{value:.2f}', ha='center', fontsize=9)
values = [[r['cpu_pct'] for r in rows if r['phase'] == phase] for phase in phases]
medians = [statistics.median(v) for v in values]
cpu.bar(range(len(phases)), medians, width=.5, color='#7c3aed',
        yerr=[[m-min(v) for m,v in zip(medians, values)],
              [max(v)-m for m,v in zip(medians, values)]], capsize=3)
for i, value in enumerate(medians):
    cpu.text(i, max(values[i])+.03, f'{value:.2f}%', ha='center')
for ax in (memory, cpu):
    ax.set_xticks(range(len(phases)), labels)
    ax.grid(axis='y', alpha=.18)
    ax.set_axisbelow(True)
    ax.set_ylim(0, ax.get_ylim()[1]*1.15)
memory.set_ylabel('Resident memory (MiB)')
memory.legend(loc='upper left', ncols=2)
cpu.set_ylabel('CPU (% of one logical core)')
fig.suptitle(f"POSIRC · real client with curses + verified TLS\n{results['date']} · median of {len({r['run'] for r in rows})} runs; whiskers show min–max", fontsize=14)
fig.savefig(args.directory/'footprint.svg', metadata={'Date': None})
plt.close(fig)

with (args.directory/'samples.csv').open() as file:
    samples = list(csv.DictReader(file))
fig, axes = plt.subplots(2, 1, figsize=(10, 6), sharex=True, layout='constrained')
for run in sorted({r['run'] for r in samples}):
    selected = [r for r in samples if r['run'] == run]
    # Average CPU over groups of 10 samples within each phase (~1 second).
    for phase in phases:
        section = [r for r in selected if r['phase'] == phase]
        axes[0].plot([float(r['elapsed_s']) for r in section],
                     [float(r['rss_mib']) for r in section], color=f'C{int(run)-1}',
                     label=f'Run {run}' if phase == phases[0] else None)
        blocks = [section[i:i+10] for i in range(0, len(section), 10)]
        axes[1].plot([statistics.mean(float(r['elapsed_s']) for r in block) for block in blocks],
                     [statistics.mean(float(r['cpu_pct']) for r in block) for block in blocks],
                     color=f'C{int(run)-1}')
first = [r for r in samples if r['run'] == '1']
for i, phase in enumerate(phases):
    section = [float(r['elapsed_s']) for r in first if r['phase'] == phase]
    for ax in axes:
        ax.axvspan(min(section), max(section), color='grey', alpha=.05 if i%2 == 0 else .13)
    axes[0].text(statistics.mean(section), 1.02, labels[i].replace('\n', '\n'),
                 transform=axes[0].get_xaxis_transform(), ha='center', va='bottom', fontsize=9)
axes[0].set_ylabel('RSS (MiB)')
axes[0].legend(loc='lower right', ncols=3)
axes[1].set_ylabel('CPU (% of one logical core)')
axes[1].set_xlabel('Seconds since process launch · gaps: connection / channel population')
for ax in axes:
    ax.set_ylim(bottom=0)
    ax.grid(alpha=.2)
fig.savefig(args.directory/'load.svg', metadata={'Date': None})
