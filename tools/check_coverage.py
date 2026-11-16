#!/usr/bin/env python3
# Copyright (C) 2026 Christian Mayer and the CometVisu contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
"""Coverage gate: every executable line and every branch arm needs a test.

Reads the gcov counters produced by a ``-DENABLE_COVERAGE=ON`` build and fails
when any line in ``src/`` was never executed or any branch arm was never taken
(including "the other side of an if").

Lines that cannot be exercised in-process are marked in the source:

    initialize_logging();   // GCOV_EXCL_LINE      line is never required
    if (fd < 0) { ... }     // GCOV_EXCL_BR_LINE   branches of this line optional
    // GCOV_EXCL_START
    ...fork()-only code...
    // GCOV_EXCL_STOP

Markers are only acceptable for code that a test process genuinely cannot
reach: fork() children that exec/exit, signal handlers, and defensive paths
for OS failures that cannot be injected.  Everything else needs a test.

Usage:
    tools/check_coverage.py                    # check, fail on any gap
    tools/check_coverage.py --report           # also print every file
    tools/check_coverage.py --list-uncovered   # show line numbers / branches
    tools/check_coverage.py --min-branch 90    # looser gate while closing gaps
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class FileCoverage:
    path: Path
    lines_total: int = 0
    lines_hit: int = 0
    branches_total: int = 0
    branches_hit: int = 0
    uncovered_lines: list[int] = field(default_factory=list)
    uncovered_branches: list[int] = field(default_factory=list)

    @property
    def line_pct(self) -> float:
        return 100.0 * self.lines_hit / self.lines_total if self.lines_total else 100.0

    @property
    def branch_pct(self) -> float:
        return 100.0 * self.branches_hit / self.branches_total if self.branches_total else 100.0


EXCL_LINE = re.compile(r"GCOV_EXCL_LINE\b(?!_)")
EXCL_BR_LINE = re.compile(r"GCOV_EXCL_BR_LINE")
EXCL_START = re.compile(r"GCOV_EXCL_START")
EXCL_STOP = re.compile(r"GCOV_EXCL_STOP")
# A line that holds nothing but closing braces carries no statement.  gcc still
# emits a line record for the closing brace of some functions (the function
# exit block), which can never be reached after a ``return``.  Such lines are
# not counted at all — no test can execute them and they express no logic.
BRACE_ONLY = re.compile(r"^\s*[}\)]+\s*;?\s*$")


def marker_maps(source: Path) -> tuple[set[int], set[int], set[int]]:
    """Return (excluded, branch_optional, non_statement) line numbers."""
    excluded: set[int] = set()
    branch_optional: set[int] = set()
    non_statement: set[int] = set()
    inside = False
    try:
        text = source.read_text(errors="replace").splitlines()
    except OSError:
        return excluded, branch_optional, non_statement
    for number, line in enumerate(text, start=1):
        if BRACE_ONLY.match(line):
            non_statement.add(number)
        if EXCL_START.search(line):
            inside = True
            excluded.add(number)
            continue
        if EXCL_STOP.search(line):
            excluded.add(number)
            inside = False
            continue
        if inside:
            excluded.add(number)
            continue
        if EXCL_LINE.search(line):
            excluded.add(number)
        if EXCL_BR_LINE.search(line):
            branch_optional.add(number)
    return excluded, branch_optional, non_statement


def gcov_json(gcno: Path) -> dict:
    result = subprocess.run(
        [  # noqa: S603 - fixed tool, no shell
            "gcov",
            "--json-format",
            "--branch-probabilities",
            "--stdout",
            "--object-directory",
            str(gcno.parent),
            str(gcno),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0 or not result.stdout.strip():
        raise RuntimeError(f"gcov failed for {gcno}: {result.stderr.strip()}")
    return json.loads(result.stdout)


def collect(build_dir: Path, source_root: Path) -> dict[Path, FileCoverage]:
    """Aggregate the gcov counters of every object file.

    A header is compiled into several translation units, so the same line
    appears in several JSON documents.  Two rules keep the result honest:

    * a line counts as covered if *any* translation unit executed it;
    * for a line with branch arms, the record with the most arms wins (an
      inlined copy in another TU can report fewer arms).  Summing copies would
      inflate the totals and report covered code as partially dead.
    """
    gcno_files = sorted(build_dir.rglob("*.gcno"))
    if not gcno_files:
        raise SystemExit(
            f"no .gcno files under {build_dir} — configure with -DENABLE_COVERAGE=ON, "
            "build and run the test suite first"
        )

    line_counts: dict[Path, dict[int, int]] = {}
    # best branch record per line: (arms, taken)
    arm_choice: dict[Path, dict[int, tuple[int, int]]] = {}

    for gcno in gcno_files:
        data = gcov_json(gcno)
        for entry in data.get("files", []):
            path = Path(entry["file"]).resolve()
            try:
                relative = path.relative_to(source_root)
            except ValueError:
                continue  # system headers, gtest, ...
            if relative.parts[0] != "src" or path.suffix not in (".cpp", ".h"):
                continue  # only production code is gated

            lines = line_counts.setdefault(path, {})
            arms = arm_choice.setdefault(path, {})
            for line in entry.get("lines", []):
                number = line["line_number"]
                lines[number] = max(lines.get(number, 0), line.get("count", 0))

                candidates = [b for b in line.get("branches", []) if not b.get("throw")]
                if not candidates:
                    continue
                total = len(candidates)
                taken = sum(1 for b in candidates if b.get("count", 0) > 0)
                best = arms.get(number)
                if best is None or total > best[0] or (total == best[0] and taken > best[1]):
                    arms[number] = (total, taken)

    coverages: dict[Path, FileCoverage] = {}
    for path, lines in line_counts.items():
        excluded, branch_optional, non_statement = marker_maps(path)
        coverage = FileCoverage(path=path)
        for number, count in sorted(lines.items()):
            if number in excluded or number in non_statement:
                continue
            coverage.lines_total += 1
            if count > 0:
                coverage.lines_hit += 1
            else:
                coverage.uncovered_lines.append(number)

        for number, (total, taken) in sorted(arm_choice.get(path, {}).items()):
            if number in excluded or number in branch_optional or number in non_statement:
                continue
            coverage.branches_total += total
            coverage.branches_hit += taken
            if taken < total:
                coverage.uncovered_branches.append(number)
        coverages[path] = coverage
    return coverages


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build-cov")
    parser.add_argument("--source-root", default=".")
    parser.add_argument(
        "--tolerance",
        type=float,
        default=1.0,
        help="allowed drop against a baseline entry, in percentage points "
        "(different gcc versions instrument different branch arm counts); "
        "files that are not baselined are always held to 100%%",
    )
    parser.add_argument("--min-line", type=float, default=100.0)
    parser.add_argument("--min-branch", type=float, default=100.0)
    parser.add_argument("--only", default="", help="restrict to paths containing this text")
    parser.add_argument(
        "--baseline",
        default="",
        help="JSON with per-file line/branch percentages; files not listed must be at 100%%, "
        "listed files must not regress below their recorded value",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="ignore the baseline: every file must reach --min-line/--min-branch",
    )
    parser.add_argument(
        "--write-baseline",
        default="",
        help="record the current numbers into this JSON file and exit 0",
    )
    parser.add_argument("--report", action="store_true", help="print every file, not only failures")
    parser.add_argument(
        "--list-uncovered", action="store_true", help="print uncovered line numbers and branches"
    )
    args = parser.parse_args()

    if shutil.which("gcov") is None:
        print("gcov not found — install gcc", file=sys.stderr)
        return 2

    source_root = Path(args.source_root).resolve()
    coverages = collect(Path(args.build_dir).resolve(), source_root)
    if not coverages:
        print("no coverage data for src/ — did the tests run?", file=sys.stderr)
        return 2

    baseline: dict[str, dict[str, float]] = {}
    if args.baseline and not args.strict:
        baseline = json.loads(Path(args.baseline).read_text())

    if args.write_baseline:
        recorded = {
            str(path.relative_to(source_root)): {
                "line": round(coverage.line_pct, 4),
                "branch": round(coverage.branch_pct, 4),
            }
            for path, coverage in sorted(coverages.items())
        }
        Path(args.write_baseline).write_text(json.dumps(recorded, indent=2, sort_keys=True) + "\n")
        print(f"recorded {len(recorded)} file(s) into {args.write_baseline}")
        return 0

    failures: list[FileCoverage] = []
    rows = []
    for path, coverage in sorted(coverages.items()):
        relative = path.relative_to(source_root)
        if args.only and args.only not in str(relative):
            continue
        # A file recorded in the baseline may not regress; every other file
        # (i.e. anything new) has to be covered completely.
        entry = baseline.get(str(relative))
        min_line = entry["line"] if entry else args.min_line
        min_branch = entry["branch"] if entry else args.min_branch
        if entry is None:
            # New code: no tolerance at all.
            ok = (
                coverage.line_pct + 1e-3 >= min_line
                and coverage.branch_pct + 1e-3 >= min_branch
                and not coverage.uncovered_lines
                and not coverage.uncovered_branches
            )
        else:
            # Legacy code: never regress beyond the recorded value.  A small
            # tolerance absorbs toolchain differences (branch arm counts vary
            # between gcc releases and between "-O0 with and without inlining").
            ok = (
                coverage.line_pct + 1e-3 + args.tolerance >= min_line
                and coverage.branch_pct + 1e-3 + args.tolerance >= min_branch
            )
        rows.append((relative, coverage, ok))
        if not ok:
            failures.append(coverage)

    width = max(len(str(row[0])) for row in rows) if rows else 10
    print(f"{'file':<{width}}  {'lines':>13}  {'branches':>13}")
    print(f"{'-' * width}  {'-' * 13}  {'-' * 13}")
    for relative, coverage, ok in rows:
        if not ok and not args.report:
            continue
        print(
            f"{str(relative):<{width}}  "
            f"{coverage.lines_hit:>5}/{coverage.lines_total:<5} "
            f"{coverage.line_pct:5.1f}%  "
            f"{coverage.branches_hit:>5}/{coverage.branches_total:<5} "
            f"{coverage.branch_pct:5.1f}%"
        )

    if failures and args.list_uncovered:
        print("\nuncovered code:")
        for coverage in failures:
            relative = coverage.path.relative_to(source_root)
            if coverage.uncovered_lines:
                numbers = " ".join(str(n) for n in sorted(set(coverage.uncovered_lines)))
                print(f"  {relative}: lines {numbers}")
            if coverage.uncovered_branches:
                numbers = " ".join(str(n) for n in sorted(set(coverage.uncovered_branches)))
                print(f"  {relative}: branch arms at lines {numbers}")

    if failures:
        print(f"\nFAIL: {len(failures)} file(s) below the required coverage")
        if baseline:
            print(f"  gate: not below tools/coverage-baseline.json minus {args.tolerance:g} pp,")
            print(f"        {args.min_line:.0f}% lines / {args.min_branch:.0f}% branches for new files")
        else:
            print(
                f"  gate: {args.min_line:.0f}% lines, {args.min_branch:.0f}% branches"
            )
        for coverage in failures:
            print(f"  {coverage.path.relative_to(source_root)}")
        return 1

    print(f"\nOK: {len(rows)} file(s) at or above the required coverage")
    return 0


if __name__ == "__main__":
    sys.exit(main())
