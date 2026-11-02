#!/usr/bin/env python3
# nanoalloc - memory-efficient, high-performance C++ allocation
# SPDX-License-Identifier: MIT
"""Compare nanoalloc_bench executables built from different library versions.

Each executable must be built from the same benchmark sources (benchmarks/ of
this tree) so that all of them replay identical traces with identical harness
code; only the linked library differs. The script runs every executable
`--runs` times, rotating the order in which they run (run r starts with
executable r mod n), collects their CSV output, and reports for every workload
the median over runs of each run's median, the spread across runs, and the
ratio to the first executable.

    python3 compare_builds.py --runs 6 \\
        buddy=../baseline/build/release/benchmarks/nanoalloc_bench \\
        current=build/release/benchmarks/nanoalloc_bench \\
        -- --allocators nanoalloc --iterations 9

Arguments after "--" are passed to every executable. Uses only the Python
standard library.
"""

import argparse
import csv
import os
import statistics
import subprocess
import sys
import tempfile

METRICS = [
    ("combined_mops", "combined Mops/s", True),
    ("alloc_mops", "alloc Mops/s", True),
    ("free_mops", "free Mops/s", True),
]


def parse_build(text):
    if "=" not in text:
        raise argparse.ArgumentTypeError("expected LABEL=PATH, got " + text)
    label, path = text.split("=", 1)
    if not os.path.isfile(path):
        raise argparse.ArgumentTypeError("no such executable: " + path)
    return label, path


def run_once(path, extra_args, csv_path):
    command = [path] + extra_args + ["--csv", csv_path]
    completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               universal_newlines=True)
    if completed.returncode != 0:
        sys.stderr.write(completed.stdout + completed.stderr)
        raise SystemExit("benchmark failed: " + " ".join(command))
    with open(csv_path, newline="") as handle:
        return list(csv.DictReader(handle))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--runs", type=int, default=6, help="runs per executable (default 6)")
    parser.add_argument("--allocator", default="nanoalloc",
                        help="allocator column to compare (default nanoalloc)")
    parser.add_argument("--csv", help="write the aggregated comparison to this file")
    parser.add_argument("builds", nargs="+", type=parse_build, metavar="LABEL=PATH")
    argv = sys.argv[1:]
    extra = []
    if "--" in argv:
        split = argv.index("--")
        argv, extra = argv[:split], argv[split + 1:]
    args = parser.parse_args(argv)
    if args.runs < 1:
        parser.error("--runs must be positive")

    labels = [label for label, _ in args.builds]
    # samples[label][workload][metric] -> list of per-run medians
    samples = {label: {} for label in labels}
    order = []
    with tempfile.TemporaryDirectory() as scratch:
        for run in range(args.runs):
            n = len(args.builds)
            for k in range(n):
                label, path = args.builds[(k + run) % n]
                rows = run_once(path, extra, os.path.join(scratch, "run.csv"))
                for row in rows:
                    if row["allocator"] != args.allocator:
                        continue
                    workload = row["workload"]
                    if workload not in order:
                        order.append(workload)
                    per_workload = samples[label].setdefault(workload, {})
                    for metric, _, _ in METRICS:
                        per_workload.setdefault(metric, []).append(float(row[metric]))
            sys.stderr.write("run %d/%d done\n" % (run + 1, args.runs))

    base = labels[0]
    header = ["workload"]
    for _, title, _ in METRICS:
        for label in labels:
            header.append("%s %s" % (title, label))
        for label in labels[1:]:
            header.append("%s %s/%s" % (title, label, base))
    header += ["spread %% %s" % label for label in labels]
    table = []
    for workload in order:
        line = [workload]
        for metric, _, _ in METRICS:
            medians = {label: statistics.median(samples[label][workload][metric])
                       for label in labels}
            line += ["%.2f" % medians[label] for label in labels]
            line += ["%.2f" % (medians[label] / medians[base]) for label in labels[1:]]
        for label in labels:
            values = samples[label][workload]["combined_mops"]
            center = statistics.median(values)
            line.append("%.1f" % (100.0 * (max(values) - min(values)) / center))
        table.append(line)

    widths = [max(len(row[i]) for row in [header] + table) for i in range(len(header))]
    print("Median over %d runs of each run's median (allocator %s); spread = (max - min) / "
          "median of combined Mops/s across runs" % (args.runs, args.allocator))
    for row in [header] + table:
        print("  ".join(cell.rjust(width) for cell, width in zip(row, widths)))
    if args.csv:
        with open(args.csv, "w", newline="") as handle:
            writer = csv.writer(handle)
            writer.writerow(header)
            writer.writerows(table)


if __name__ == "__main__":
    main()
