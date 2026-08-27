#!/usr/bin/env python3
# Copyright (c) 2026 The Brave Authors. All rights reserved.
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at https://mozilla.org/MPL/2.0/.
"""
Update the auto-generated filter files for flaky upstream tests.

For each upstream test suite that Brave runs, finds tests whose flake
rate in Chromium's LUCI Analysis data exceeds the threshold over the
lookback period and writes them to test/filters/generated/<suite>.filter.
Those files are picked up automatically by `npm run test` (see
build/commands/lib/testUtils.js).

Candidate tests are discovered through failure clusters
(Clusters.QueryClusterSummaries), then each candidate's flake rate is
computed from its full test history (TestHistory.QueryStats). The
cluster API only returns the top 200 clusters per query, so the lookback
period is additionally sliced into weekly windows to widen discovery.

Usage:
    python3 script/update-upstream-flake-filters.py [suite ...] \\
        [--days 30] [--min-flake-rate 1.0]
"""

import argparse
import os
import sys
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timedelta, timezone

from lib.config import BRAVE_CORE_ROOT
from lib.luci_analysis import (
    LuciAnalysisError,
    MIN_MEANINGFUL_VERDICTS,
    analyze_stats,
    get_flakiness_stats,
    query_cluster_failures,
    query_cluster_summaries,
)

# Upstream test suites Brave runs on CI (see chromium_unit_tests in
# BUILD.gn and the existing hand-written files in test/filters/).
DEFAULT_SUITES = [
    "base_unittests",
    "browser_tests",
    "components_unittests",
    "content_unittests",
    "installer_util_unittests",
    "net_unittests",
    "services_unittests",
    "setup_unittests",
    "unit_tests",
]

GENERATED_FILTERS_DIR = os.path.join(BRAVE_CORE_ROOT, "test", "filters",
                                     "generated")

# For parameterized test families (wildcard clusters), only the variants
# with the most recent failures are checked individually.
MAX_VARIANTS_PER_CLUSTER = 20

STATS_WORKERS = 8


def log(message):
    print(message, file=sys.stderr)


def candidate_windows(days):
    """Time windows used for candidate discovery.

    The full lookback window plus weekly slices, to work around the
    200-cluster cap of QueryClusterSummaries.
    """
    now = datetime.now(timezone.utc)
    windows = [(now - timedelta(days=days), now)]
    start = 0
    while start < days:
        end = min(start + 7, days)
        windows.append(
            (now - timedelta(days=end), now - timedelta(days=start)))
        start = end
    return windows


def is_like_pattern(title):
    """Return True if a testname cluster title is a LIKE pattern.

    Titles of clusters that group multiple tests (e.g. parameterized
    variants) are SQL LIKE patterns with literals escaped by backslash.
    Titles of single-test clusters are verbatim test IDs, which never
    contain "\\\\", "\\_" or "%" (only "\\:" from flat test ID encoding).
    """
    return "%" in title or "\\\\" in title or "\\_" in title


def normalize_test_id(test_id):
    """Map WebUI JS sub-result test IDs to their parent test case.

    WebUI browser tests report JS sub-results as separate
    "<case>__<sub_result>" test IDs. Only the parent "<case>" is an
    actual gtest case that can be filtered and has full history stats.
    """
    head, sep, fine = test_id.partition("#")
    if not sep:
        return test_id
    case, slash, param = fine.partition("/")
    case = case.split("__")[0]
    return head + "#" + case + (slash + param if slash else "")


def structured_id_to_gtest_name(test_id):
    """Convert a structured LUCI test ID to a gtest test name.

    Examples:
        "://chrome/test\\:browser_tests!gtest::Suite#Case"
            -> "Suite.Case"
        "://chrome/test\\:browser_tests!gtest::Suite#Case/Inst.Param"
            -> "Inst/Suite.Case/Param"

    Returns:
        The gtest name, or None if the ID is not a gtest test ID.
    """
    _, sep, fine = test_id.partition("!gtest::")
    if not sep:
        return None
    suite, sep, case = fine.partition("#")
    if not sep:
        return None
    case, slash, param = case.partition("/")
    if not slash:
        return f"{suite}.{case}"
    instantiation, dot, value = param.partition(".")
    if not dot:
        # Value-parameterized without instantiation prefix.
        return f"{suite}.{case}/{param}"
    return f"{instantiation}/{suite}.{case}/{value}"


def collect_candidate_test_ids(suite, days):
    """Discover test IDs in a suite with recent upstream failures."""
    # Matches the ":<suite>!gtest" portion of structured test IDs like
    # "://chrome/test\:browser_tests!gtest::Suite#Case".
    suite_marker = f":{suite}!gtest"

    clusters = {}
    for earliest, latest in candidate_windows(days):
        summaries = query_cluster_summaries(f'test_id:"{suite_marker}"',
                                            earliest, latest)
        for summary in summaries:
            cluster = summary["clusterId"]
            if not cluster["algorithm"].startswith("testname"):
                continue
            clusters[(cluster["algorithm"], cluster["id"])] = \
                summary["title"]

    test_ids = set()
    for (algorithm, cluster_id), title in sorted(clusters.items()):
        if not is_like_pattern(title):
            # The title is the verbatim test ID.
            if suite_marker in title:
                test_ids.add(normalize_test_id(title))
            continue

        # The cluster groups multiple test IDs (e.g. a parameterized
        # test family). Enumerate its recent failures to get exact IDs.
        failure_counts = Counter()
        for failure in query_cluster_failures(algorithm, cluster_id):
            test_id = failure.get("testId", "")
            if suite_marker in test_id:
                failure_counts[normalize_test_id(test_id)] += int(
                    failure.get("count", 1))
        top = failure_counts.most_common(MAX_VARIANTS_PER_CLUSTER)
        dropped = len(failure_counts) - len(top)
        if dropped > 0:
            log(f"  Note: cluster '{title[:80]}' has "
                f"{len(failure_counts)} recently failing variants; only "
                f"checking the top {len(top)} by failure count.")
        test_ids.update(test_id for test_id, _ in top)

    return sorted(test_ids)


def analyze_candidates(test_ids, days):
    """Compute flakiness stats for each candidate test ID."""

    def analyze_one(test_id):
        return test_id, analyze_stats(get_flakiness_stats(test_id, days))

    with ThreadPoolExecutor(max_workers=STATS_WORKERS) as executor:
        return dict(executor.map(analyze_one, test_ids))


def build_filter_content(suite, entries, days, min_flake_rate):
    """Build the content of a generated filter file.

    Args:
        suite: Test suite name.
        entries: List of (gtest_name, analysis_dict) tuples.
        days: Lookback window in days.
        min_flake_rate: Flake rate threshold (fraction).

    Returns:
        The filter file content string.
    """
    lines = [
        "## AUTO-GENERATED FILE -- DO NOT EDIT.",
        "##",
        f"## Upstream {suite} tests with a flake rate >="
        f" {min_flake_rate:.1%} over the",
        f"## past {days} days per Chromium LUCI Analysis. Regenerate with:",
        "##   python3 script/update-upstream-flake-filters.py",
    ]
    for gtest_name, analysis in sorted(entries):
        lines.append("")
        lines.append(f"# {analysis['flake_rate']:.1%} flake rate over"
                     f" {days} days per LUCI Analysis"
                     f" ({analysis['passed']} passed,"
                     f" {analysis['failed']} failed,"
                     f" {analysis['flaky']} flaky).")
        lines.append(f"-{gtest_name}")
    return "\n".join(lines) + "\n"


def update_suite_filter(suite, days, min_flake_rate):
    """Regenerate the filter file for one suite."""
    log(f"[{suite}] Discovering candidate flaky tests...")
    test_ids = collect_candidate_test_ids(suite, days)
    log(f"[{suite}] Checking flake rate of {len(test_ids)} candidates...")
    analyses = analyze_candidates(test_ids, days)

    entries = []
    for test_id, analysis in analyses.items():
        if analysis["meaningful_verdicts"] < MIN_MEANINGFUL_VERDICTS:
            continue
        if analysis["flake_rate"] < min_flake_rate:
            continue
        gtest_name = structured_id_to_gtest_name(test_id)
        if gtest_name:
            entries.append((gtest_name, analysis))

    path = os.path.join(GENERATED_FILTERS_DIR, f"{suite}.filter")
    os.makedirs(GENERATED_FILTERS_DIR, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(build_filter_content(suite, entries, days, min_flake_rate))
    log(f"[{suite}] Wrote {len(entries)} entries to"
        f" {os.path.relpath(path, BRAVE_CORE_ROOT)}")


def main():
    parser = argparse.ArgumentParser(
        description=("Update test/filters/generated/*.filter with upstream"
                     " tests that are flaky per Chromium LUCI Analysis."))
    parser.add_argument(
        "suites",
        nargs="*",
        default=DEFAULT_SUITES,
        help=f"Test suites to update (default: {' '.join(DEFAULT_SUITES)})",
    )
    parser.add_argument(
        "--days",
        type=int,
        default=30,
        help="Number of days to look back (default: 30, max: 90)",
    )
    parser.add_argument(
        "--min-flake-rate",
        type=float,
        default=1.0,
        help="Flake rate threshold in percent (default: 1.0)",
    )
    args = parser.parse_args()

    if args.days < 1 or args.days > 90:
        print("Error: --days must be between 1 and 90.", file=sys.stderr)
        sys.exit(1)

    for suite in args.suites:
        update_suite_filter(suite, args.days, args.min_flake_rate / 100.0)


if __name__ == "__main__":
    try:
        main()
    except LuciAnalysisError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
