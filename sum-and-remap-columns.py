#!/usr/bin/env python3
import argparse
import csv

def parse_column(s, source_columns):
    new_name, columns_to_sum = s.split('=', 2)
    columns_to_sum = columns_to_sum.split('+')
    columns_to_sum = list(map(source_columns.index, columns_to_sum))
    return new_name, columns_to_sum

def transform_row(row, new_columns):
    return (sum(row[i] for i in columns_to_sum) for _name, columns_to_sum in new_columns)

def main(filename, new_columns):
    with open(filename) as f:
        reader = csv.reader(f)
        source_columns = next(reader)
        new_columns = [parse_column(s, source_columns) for s in new_columns]
        new_column_names = (name for name, columns_to_sum in new_columns)
        print(",".join(new_column_names))
        for row in reader:
            row = list(map(int, row))
            print(",".join(map(str, transform_row(row, new_columns))))

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('filename')

    args, new_columns = parser.parse_known_args()
    main(args.filename, new_columns)
