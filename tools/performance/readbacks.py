"""Summarize bounded readback samples; no third-party dependencies."""
import argparse
import collections
import csv
import json
import statistics

SOURCES = {0: 'unknown', 1: 'cpu_read', 2: 'cpu_write', 3: 'gpu_read',
           4: 'gpu_write', 5: 'metadata', 6: 'gc'}
PHASES = ('dispatch_ns', 'prepare_ns', 'gpu_wait_ns', 'publish_wait_ns', 'total_ns')


def summarize(rows):
    rows = sorted(rows, key=lambda row: row['elapsed_ns'])
    by_source = {}
    for source in sorted({row['source'] for row in rows}):
        selected = [row for row in rows if row['source'] == source]
        by_source[SOURCES[source]] = {
            'samples': len(selected),
            'no_copy_samples': sum(row['copies'] == 0 for row in selected),
            'payload_bytes': sum(row['payload_bytes'] for row in selected),
            'packed_bytes': sum(row['packed_bytes'] for row in selected),
            'copies': sum(row['copies'] for row in selected),
            'phase_ms': {phase: {
                'sum': sum(row[phase] for row in selected) / 1e6,
                'median': statistics.median(row[phase] for row in selected) / 1e6,
            } for phase in PHASES},
        }
    groups = collections.defaultdict(list)
    for row in rows:
        groups[(row['source'], row['buffer_address'], row['window_address'])].append(row)
    windows = []
    for (source, buffer, window), selected in groups.items():
        if not window:
            continue
        consecutive = list(zip(selected, selected[1:]))
        windows.append({
            'source': SOURCES[source], 'buffer_address': hex(buffer), 'window_address': hex(window),
            'samples': len(selected),
            'unique_request_pages': len({row['address'] // 4096 for row in selected}),
            'payload_bytes': sum(row['payload_bytes'] for row in selected),
            'total_ms': sum(row['total_ns'] for row in selected) / 1e6,
            'same_tick_begin_samples': sum(a['tick_begin'] == b['tick_begin'] for a, b in consecutive),
            'next_begin_equals_previous_end': sum(a['tick_end'] == b['tick_begin'] for a, b in consecutive),
            'median_gap_ms': statistics.median((b['elapsed_ns'] - a['elapsed_ns']) / 1e6
                                             for a, b in consecutive) if consecutive else None,
        })
    return {
        'samples': len(rows), 'by_source': by_source,
        'top_windows': sorted(windows, key=lambda row: row['samples'], reverse=True)[:15],
        'note': 'One in 64 requests per host thread; at most 20000 rows per process. '
                'Sample sums are not full-run totals and threads with few requests are overrepresented. '
                'Ticks describe the download command scheduler, not the last writer or guest frame. '
                'Repeated window/tick does not prove reusable data. Host phases can overlap across threads.',
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('csv')
    parser.add_argument('--start-seconds', type=float, default=0)
    parser.add_argument('--end-seconds', type=float, default=float('inf'))
    parser.add_argument('--output')
    args = parser.parse_args()
    with open(args.csv, newline='', encoding='utf-8') as stream:
        rows = [{key: int(value) for key, value in row.items()} for row in csv.DictReader(stream)]
    selected = [row for row in rows
                if args.start_seconds <= row['elapsed_ns'] / 1e9 < args.end_seconds]
    report = summarize(selected)
    report['source'] = args.csv
    rendered = json.dumps(report, indent=2)
    if args.output:
        with open(args.output, 'w', encoding='utf-8') as stream:
            stream.write(rendered + '\n')
    print(rendered)


if __name__ == '__main__':
    main()
