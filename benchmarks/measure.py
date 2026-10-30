#!/usr/bin/env python3
"""Linux end-to-end benchmark: real client, PTY, verified local TLS; stdlib only."""
import argparse
import csv
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import pty
import socket
import ssl
import struct
import subprocess
import tempfile
import termios
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
TICKS = os.sysconf('SC_CLK_TCK')


def snapshot(pid):
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    memory = {}
    for line in Path(f'/proc/{pid}/smaps_rollup').read_text().splitlines():
        if ':' in line:
            key, value = line.split(':', 1)
            memory[key] = int(value.split()[0])
    status = dict(line.split(':', 1) for line in Path(f'/proc/{pid}/status').read_text().splitlines() if ':' in line)
    io = dict(line.split(':', 1) for line in Path(f'/proc/{pid}/io').read_text().splitlines())
    return dict(rss_mib=memory['Rss'] / 1024, pss_mib=memory['Pss'] / 1024,
                uss_mib=(memory['Private_Clean'] + memory['Private_Dirty']) / 1024,
                anonymous_mib=memory['Anonymous'] / 1024,
                cpu_seconds=(int(fields[11]) + int(fields[12])) / TICKS,
                minor_faults=int(fields[7]), major_faults=int(fields[9]),
                voluntary_switches=int(status['voluntary_ctxt_switches']),
                involuntary_switches=int(status['nonvoluntary_ctxt_switches']),
                read_calls=int(io['syscr']), write_calls=int(io['syscw']),
                read_bytes=int(io['rchar']), write_bytes=int(io['wchar']))



def drain(fd):
    try:
        while os.read(fd, 65536):
            pass
    except OSError:
        pass


def run(number, certificate, key, samples, summaries, binary):
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen(1)
    listener.settimeout(15)
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 40, 120, 0, 0))
    started = time.monotonic()
    client = subprocess.Popen([str(binary), '-n', 'bench', '-c', str(certificate)],
                              stdin=slave, stdout=slave, stderr=slave,
                              env={**os.environ, 'TERM': 'xterm-256color', 'LC_ALL': 'C.UTF-8'})
    os.close(slave)
    reader = threading.Thread(target=drain, args=(master,), daemon=True)
    reader.start()
    conn = None
    pending = b''

    def until(fragment):
        nonlocal pending
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            while b'\n' in pending:
                line, pending = pending.split(b'\n', 1)
                if fragment in line:
                    return
            data = conn.recv(65536)
            if not data:
                raise RuntimeError('Client disconnected before acknowledgement')
            pending += data
        raise TimeoutError(fragment)

    def barrier():
        start = time.monotonic()
        conn.sendall(b'PING :benchmark-barrier\r\n')
        until(b'PONG :benchmark-barrier')
        return (time.monotonic() - start) * 1000

    def phase(name, duration, channels=0):
        start = time.monotonic()
        before = snapshot(client.pid)
        previous, previous_time = before, start
        rows = []
        sent = 0
        for tick in range(round(duration * 10)):
            if channels:
                frames = []
                for i in range(100):
                    channel = (sent + i) % channels
                    frames.append(f':u000!u@local PRIVMSG #bench{channel} :{sent+i:08d} ' + 'x' * 391 + '\r\n')
                conn.sendall(''.join(frames).encode())
                sent += 100
            time.sleep(max(0, start + (tick + 1) / 10 - time.monotonic()))
            now = time.monotonic()
            current = snapshot(client.pid)
            row = dict(run=number, phase=name, elapsed_s=now-started,
                       rss_mib=current['rss_mib'], pss_mib=current['pss_mib'], uss_mib=current['uss_mib'],
                       cpu_pct=100*(current['cpu_seconds']-previous['cpu_seconds'])/(now-previous_time))
            rows.append(row)
            samples.append(row)
            previous, previous_time = current, now
        barrier_ms = barrier() if conn else None  # All preceding frames reached the parser.
        end = time.monotonic()
        after = snapshot(client.pid)
        summaries.append(dict(run=number, phase=name, duration_s=end-start, messages=sent,
                              messages_per_s=sent/(end-start),
                              rss_mib=sum(r['rss_mib'] for r in rows)/len(rows),
                              pss_mib=sum(r['pss_mib'] for r in rows)/len(rows),
                              peak_rss_mib=max(r['rss_mib'] for r in rows),
                              cpu_pct=100*(after['cpu_seconds']-before['cpu_seconds'])/(end-start)))
        summary = summaries[-1]
        summary['uss_mib'] = sum(r['uss_mib'] for r in rows)/len(rows)
        summary['barrier_ms'] = barrier_ms
        for key in ('cpu_seconds', 'minor_faults', 'major_faults', 'voluntary_switches',
                    'involuntary_switches', 'read_calls', 'write_calls', 'read_bytes', 'write_bytes'):
            summary[key] = after[key] - before[key]
        print(number, name, f"RSS={summary['rss_mib']:.2f} MiB CPU={summary['cpu_pct']:.2f}%", flush=True)

    try:
        time.sleep(1)  # Warm-up, excluded from phase averages.
        phase('Offline idle', 5)
        os.write(master, f'/connect 127.0.0.1 {listener.getsockname()[1]}\n'.encode())
        raw, _ = listener.accept()
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate, key)
        conn = context.wrap_socket(raw, server_side=True)
        conn.settimeout(15)
        until(b'USER ')
        conn.sendall(b':local 001 bench :Welcome\r\n:bench!u@local JOIN #bench0\r\n')
        barrier()
        time.sleep(1)
        phase('TLS idle', 5)
        phase('1 channel / 1000 msg/s', 10, 1)
        # Populate 20 channels, 200 names and a full 256-line history in each.
        for channel in range(20):
            conn.sendall(f':bench!u@local JOIN #bench{channel}\r\n'.encode())
            for first in range(0, 200, 40):
                names = ' '.join(f'u{i:03}' for i in range(first, first+40))
                conn.sendall(f':local 353 bench = #bench{channel} :{names}\r\n'.encode())
            conn.sendall(f':local 366 bench #bench{channel} :End\r\n'.encode())
            for i in range(256):
                conn.sendall((f':u000!u@local PRIVMSG #bench{channel} :' + 'x'*400 + '\r\n').encode())
        barrier()
        phase('20 channels / 1000 msg/s', 10, 20)
        phase('20 channels / idle', 5)
        os.write(master, b'/quit\n')
        # Allow orderly QUIT handling; the client has a bounded drain timeout.
        client.wait(timeout=5)
        if client.returncode != 0:
            raise RuntimeError(f'Client exited with {client.returncode}')
    finally:
        if client.poll() is None:
            client.terminate()
            try:
                client.wait(timeout=5)
            except subprocess.TimeoutExpired:
                client.kill()
                client.wait()
        if conn:
            conn.close()
        listener.close()
        os.close(master)
        reader.join(timeout=1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'posirc')
    parser.add_argument('--label', default='optimized')
    parser.add_argument('--source-dir', type=Path, default=ROOT)
    parser.add_argument('--runs', type=int, default=3)
    parser.add_argument('--output', type=Path, default=ROOT / 'docs/benchmarks')
    args = parser.parse_args()
    if args.runs < 1:
        parser.error('--runs must be positive')
    args.output.mkdir(parents=True, exist_ok=True)
    samples, summaries = [], []
    with tempfile.TemporaryDirectory(prefix='posirc-benchmark-') as directory:
        certificate, key = Path(directory)/'cert.pem', Path(directory)/'key.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                        '-keyout', str(key), '-out', str(certificate), '-days', '1',
                        '-subj', '/CN=127.0.0.1', '-addext', 'subjectAltName=IP:127.0.0.1'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for number in range(1, args.runs+1):
            run(number, certificate, key, samples, summaries, args.binary.resolve())
    with (args.output/'samples.csv').open('w') as file:
        writer = csv.DictWriter(file, fieldnames=list(samples[0]), lineterminator='\n')
        writer.writeheader()
        writer.writerows(samples)
    source_files = sorted([*args.source_dir.glob('src/*.c'), *args.source_dir.glob('include/*.h'), args.source_dir/'Makefile'])
    source_hashes = {str(p.relative_to(args.source_dir)): hashlib.sha256(p.read_bytes()).hexdigest() for p in source_files}
    metadata = dict(label=args.label, source_sha256=source_hashes, build_flags='cc -O2, dynamic linking, no sanitizers',
                    page_bytes=os.sysconf('SC_PAGE_SIZE'),
                    cache={p.name: {f: (p/f).read_text().strip() for f in ('level', 'type', 'size', 'coherency_line_size')} for p in Path('/sys/devices/system/cpu/cpu0/cache').glob('index*')},
                    date=time.strftime('%Y-%m-%d'), platform=platform.platform(),
                    cpu=next(line.split(':', 1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines() if line.startswith('model name')),
                    compiler=subprocess.check_output(['cc', '--version'], text=True).splitlines()[0],
                    openssl=subprocess.check_output(['openssl', 'version'], text=True).strip(),
                    source_revision=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                    binary_bytes=args.binary.stat().st_size,
                    binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                    clock_ticks_per_second=TICKS, terminal='xterm-256color, 120x40, C.UTF-8',
                    summaries=summaries)
    (args.output/'results.json').write_text(json.dumps(metadata, indent=2)+'\n')


if __name__ == '__main__':
    main()
