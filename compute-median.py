#!/usr/bin/env python3
import argparse
import pandas as pd

def main(filename):
    df = pd.read_csv(filename)
    medians = df.median()
    medians['total'] = df.sum(axis=1).median()
    print(medians.to_string())

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('filename')

    args = parser.parse_args()
    main(args.filename)
