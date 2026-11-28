#!/usr/bin/env python3
"""Coverage report for the host tests (run by `make coverage`; stdlib only).

gcov JSON for every .gcda in out/, merged per source file (a source compiled into several test
binaries counts a line as covered if any of them ran it). Firmware sources are core/, src/ and
board/; the ones no host test compiles are listed as untested. Third-party code (external/) and
the tests themselves are not counted.

Outputs: table on stdout, out/coverage/summary.md, out/coverage/coverage.json.
"""
import argparse
import glob
import gzip
import json
import os
import subprocess
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
BUILD = os.path.join(HERE, "out")  # Makefile OUT: test binaries and gcov data
OUT = os.path.join(BUILD, "coverage")
FIRMWARE_DIRECTORIES = ("core/", "src/", "board/")
FIRMWARE_PATTERNS = ("core/*.c", "src/*.c", "board/bsp/*.c")


def relative_to_root(path, working_directory):
    absolute_path = os.path.normpath(os.path.join(working_directory, path))
    return os.path.relpath(absolute_path, ROOT)


def firmware_source(relative_path):
    return relative_path.startswith(FIRMWARE_DIRECTORIES) and relative_path.endswith(".c")


def c_coverage():
    line_counts = defaultdict(dict)
    branch_counts = defaultdict(dict)
    function_counts = defaultdict(dict)
    gcda_files = sorted(glob.glob(os.path.join(BUILD, "*.gcda")))
    if not gcda_files:
        return {}
    gcov_directory = os.path.join(OUT, "gcov")
    os.makedirs(gcov_directory, exist_ok=True)
    for old_report in glob.glob(os.path.join(gcov_directory, "*")):
        os.remove(old_report)
    subprocess.run(["gcov", "-b", "-j", "-o", BUILD] + gcda_files, cwd=gcov_directory, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for report_path in glob.glob(os.path.join(gcov_directory, "*.gcov.json.gz")):
        with gzip.open(report_path) as report_file:
            report = json.load(report_file)
        working_directory = report.get("current_working_directory", HERE)
        for source in report["files"]:
            source_path = relative_to_root(source["file"], working_directory)
            if not firmware_source(source_path):
                continue
            for line in source["lines"]:
                line_number = line["line_number"]
                line_counts[source_path][line_number] = line_counts[source_path].get(line_number, 0) + line["count"]
                for branch_index, branch in enumerate(line.get("branches", [])):
                    key = (line_number, branch_index)
                    branch_counts[source_path][key] = branch_counts[source_path].get(key, 0) + branch["count"]
            for function in source.get("functions", []):
                name = function.get("demangled_name", function["name"])
                function_counts[source_path][name] = function_counts[source_path].get(name, 0) + \
                    function["execution_count"]
    results = {}
    for source_path, lines in line_counts.items():
        branches, functions = branch_counts[source_path], function_counts[source_path]
        results[source_path] = {
            "lines": [sum(count > 0 for count in lines.values()), len(lines)],
            "branches": [sum(count > 0 for count in branches.values()), len(branches)],
            "functions": [sum(count > 0 for count in functions.values()), len(functions)],
            "uncovered_lines": sorted(number for number, count in lines.items() if count == 0),
            "uncalled": sorted(name for name, count in functions.items() if count == 0),
        }
    return results


def untested_sources(tested):
    found = []
    for pattern in FIRMWARE_PATTERNS:
        for path in glob.glob(os.path.join(ROOT, pattern)):
            relative_path = os.path.relpath(path, ROOT)
            if relative_path not in tested:
                found.append(relative_path)
    return sorted(found)


def percent(covered, total):
    return 100.0 * covered / total if total else 100.0


def ranges(numbers):
    parts, start, previous = [], None, None
    for number in numbers:
        if start is None:
            start = previous = number
        elif number <= previous + 2:
            previous = number
        else:
            parts.append(f"{start}-{previous}" if start != previous else str(start))
            start = previous = number
    if start is not None:
        parts.append(f"{start}-{previous}" if start != previous else str(start))
    return ",".join(parts)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--fail-under", type=float, help="minimum total line coverage (%%)")
    parser.add_argument("-v", "--verbose", action="store_true", help="list uncovered lines")
    arguments = parser.parse_args()
    os.makedirs(OUT, exist_ok=True)

    results = c_coverage()
    untested = untested_sources(results)
    totals = {"lines": [0, 0], "branches": [0, 0]}
    for file_coverage in results.values():
        for key in totals:
            totals[key] = [total + count for total, count in zip(totals[key], file_coverage[key])]

    def cell(counts):
        return f"{percent(*counts):.1f}% ({counts[0]}/{counts[1]})" if counts else "-"

    name_width = max([len(path) for path in results] + [5])
    print(f"{'file':{name_width}}  {'lines':>18}  {'branches':>18}  {'functions':>18}")
    for path in sorted(results):
        file_coverage = results[path]
        print(f"{path:{name_width}}  {cell(file_coverage['lines']):>18}  {cell(file_coverage['branches']):>18}  "
              f"{cell(file_coverage['functions']):>18}")
        if arguments.verbose:
            if file_coverage["uncovered_lines"]:
                print(f"    uncovered: {ranges(file_coverage['uncovered_lines'])}")
            if file_coverage["uncalled"]:
                print(f"    uncalled:  {', '.join(file_coverage['uncalled'])}")
    print(f"{'TOTAL':{name_width}}  {cell(totals['lines']):>18}  {cell(totals['branches']):>18}")
    if untested:
        print("\nNo host test compiles: " + ", ".join(untested))

    markdown_rows = ["| File | Lines | Branches | Functions |", "|---|---:|---:|---:|"]
    for path in sorted(results):
        file_coverage = results[path]
        markdown_rows.append(f"| `{path}` | {cell(file_coverage['lines'])} | {cell(file_coverage['branches'])} | "
                             f"{cell(file_coverage['functions'])} |")
    markdown_rows.append(f"| **Total** | **{cell(totals['lines'])}** | **{cell(totals['branches'])}** | |")
    if untested:
        markdown_rows.append("\nNot compiled by any host test: " + ", ".join(f"`{path}`" for path in untested))
    with open(os.path.join(OUT, "summary.md"), "w") as output_file:
        output_file.write("\n".join(markdown_rows) + "\n")
    with open(os.path.join(OUT, "coverage.json"), "w") as output_file:
        json.dump({"c": results, "untested": untested, "totals": totals}, output_file, indent=1)

    if arguments.fail_under is not None and percent(*totals["lines"]) < arguments.fail_under:
        print(f"\nFAIL: total line coverage {percent(*totals['lines']):.1f}% < {arguments.fail_under}%")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
