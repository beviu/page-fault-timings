#!/usr/bin/env python3
import csv
import itertools
import os
import sys

from collections import Counter

def array_differences(array):
    return list(map(lambda pair: pair[1] - pair[0], zip(itertools.chain([0], array), array)))

MIN_DURATIONS = None

def get_order_of_tracepoints(f):
    reader = csv.reader(f)
    _columns = next(reader)
    for row in reader:
        hit = [(i, int(cell)) for i, cell in enumerate(row) if cell]
        hit.sort(key=lambda pair: pair[1])
        yield tuple((i for i, timestamp in hit))

def reorder(array, order):
    last = 0
    for i in order:
        value = int(array[i])
        yield value - last
        last = value

def print_stacked_chart(f, order):
    reader = csv.reader(f)
    columns = next(reader)
    columns = [columns[i] for i in order]
    print(",".join(columns))
    for row in reader:
        durations = map(str, reorder(row, order))
        print(",".join(durations))

def main():
    with open(sys.argv[1]) as f:
        counter = Counter(get_order_of_tracepoints(f))
        most_common_order = counter.most_common(1)[0][0]

        # Prepare the file for reading again.
        f.seek(0, os.SEEK_SET)

        print_stacked_chart(f, most_common_order)

if __name__ == "__main__":
    main()
