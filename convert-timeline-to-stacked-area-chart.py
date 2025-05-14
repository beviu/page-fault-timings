#!/usr/bin/env python3
import argparse
import csv
import os

from collections import Counter

def get_order_of_tracepoints(f):
    reader = csv.reader(f)
    _columns = next(reader)
    for row in reader:
        hit_tracepoints = [(i, int(timestamp)) for i, timestamp in enumerate(row) if timestamp != ""]
        hit_tracepoints.sort(key=lambda pair: pair[1])
        yield tuple((i for i, _timestamp in hit_tracepoints))

def reorder_timestamps_and_convert_to_delays(timestamps, order):
    previous_timestamp = 0
    reordered = []
    for i in order:
        timestamp = timestamps[i]
        if timestamp == "":
            return None
        timestamp = int(timestamp)
        if timestamp < previous_timestamp:
            return None
        reordered.append(timestamp - previous_timestamp)
        previous_timestamp = timestamp
    return reordered

def print_stacked_chart(f, order, max_lines):
    reader = csv.reader(f)
    columns = next(reader)
    columns = [columns[i] for i in order]
    print(",".join(columns))
    lines = 0
    for row in reader:
        reordered = reorder_timestamps_and_convert_to_delays(row, order)
        if reordered is None:
            continue
        print(",".join(map(str, reordered)))
        lines += 1
        if max_lines is not None and lines == max_lines:
            break

def main(filename, max_lines):
    with open(filename) as f:
        counter = Counter(get_order_of_tracepoints(f))
        most_common_order = counter.most_common(1)[0][0]

        # Prepare the file for reading again.
        f.seek(0, os.SEEK_SET)

        print_stacked_chart(f, most_common_order, max_lines)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('filename')
    parser.add_argument('-n', '--lines', type=int)

    args = parser.parse_args()
    main(args.filename, args.lines)
