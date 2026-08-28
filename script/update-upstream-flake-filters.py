#!/usr/bin/env python3
# Copyright (c) 2026 The Brave Authors. All rights reserved.
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at https://mozilla.org/MPL/2.0/.
"""
Update the auto-generated filter files for flaky upstream tests.

For each upstream test suite that Brave runs, finds tests whose flake
rate in Chromium's LUCI Analysis data exceeds the threshold over the
lookback period and writes them to
test/filters/generated/<suite>-<platform>.filter. Flake rates are
computed per platform from the matching upstream bots, so a test only
flaky on e.g. Linux is only filtered there. Those files are picked up
automatically by `npm run test` (see build/commands/lib/testUtils.js).

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
    get_test_variants,
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

# Platforms Brave runs upstream test suites on, mapped to the "os"
# prefixes of the corresponding upstream bots. Bots for other platforms
# (e.g. ChromeOS, Android) are ignored. The platform names must match
# those used by getApplicableFilters in build/commands/lib/testUtils.js.
PLATFORM_OS_PREFIXES = {
    "linux": ("Ubuntu", "Linux"),
    "macos": ("Mac", ),
    "windows": ("Windows", ),
}

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


def fetch_candidate_stats(test_ids, days):
    """Fetch raw per-variant flakiness stats for each candidate test ID."""

    def fetch_one(test_id):
        return test_id, get_flakiness_stats(test_id, days)

    with ThreadPoolExecutor(max_workers=STATS_WORKERS) as executor:
        return dict(executor.map(fetch_one, test_ids))


def platform_for_os(os_name):
    """Map an upstream bot "os" variant value to a Brave platform.

    Returns None for platforms Brave doesn't run (e.g. ChromeOS).
    """
    for platform, prefixes in PLATFORM_OS_PREFIXES.items():
        if os_name.startswith(prefixes):
            return platform
    return None


def resolve_variant_platforms(stats_by_test_id):
    """Map each variant hash seen in the stats to a Brave platform.

    Variant hashes are shared between tests that run on the same bot
    config, so one QueryVariants call typically resolves the hashes of
    most tests in a suite; further calls are only made for tests whose
    stats contain still-unknown hashes.
    """
    platform_by_hash = {}
    for test_id, groups in stats_by_test_id.items():
        if all(g.get("variantHash") in platform_by_hash for g in groups):
            continue
        for entry in get_test_variants(test_id):
            os_name = entry.get("variant", {}).get("def", {}).get("os", "")
            platform_by_hash[entry["variantHash"]] = platform_for_os(os_name)
        # Don't re-query for hashes QueryVariants didn't return.
        for group in groups:
            platform_by_hash.setdefault(group.get("variantHash"), None)
    return platform_by_hash


def analyze_per_platform(groups, platform_by_hash):
    """Compute per-platform flakiness analyses from raw stats groups."""
    analyses = {}
    for platform in PLATFORM_OS_PREFIXES:
        platform_groups = [
            g for g in groups
            if platform_by_hash.get(g.get("variantHash")) == platform
        ]
        analyses[platform] = analyze_stats(platform_groups)
    return analyses


def build_filter_content(suite, platform, entries, days, min_flake_rate):
    """Build the content of a generated filter file.

    Args:
        suite: Test suite name.
        platform: Brave platform name (e.g. "linux").
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
        f" {min_flake_rate:.1%} on",
        f"## {platform} bots over the past {days} days per Chromium LUCI"
        " Analysis.",
        "## Regenerate with:",
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


def update_suite_filters(suite, days, min_flake_rate):
    """Regenerate the per-platform filter files for one suite."""
    log(f"[{suite}] Discovering candidate flaky tests...")
    test_ids = collect_candidate_test_ids(suite, days)
    log(f"[{suite}] Checking flake rate of {len(test_ids)} candidates...")
    stats_by_test_id = fetch_candidate_stats(test_ids, days)
    platform_by_hash = resolve_variant_platforms(stats_by_test_id)

    entries_by_platform = {platform: [] for platform in PLATFORM_OS_PREFIXES}
    for test_id, groups in stats_by_test_id.items():
        gtest_name = structured_id_to_gtest_name(test_id)
        if not gtest_name:
            continue
        analyses = analyze_per_platform(groups, platform_by_hash)
        for platform, analysis in analyses.items():
            if analysis["meaningful_verdicts"] < MIN_MEANINGFUL_VERDICTS:
                continue
            if analysis["flake_rate"] < min_flake_rate:
                continue
            entries_by_platform[platform].append((gtest_name, analysis))

    os.makedirs(GENERATED_FILTERS_DIR, exist_ok=True)
    for platform, entries in entries_by_platform.items():
        path = os.path.join(GENERATED_FILTERS_DIR,
                            f"{suite}-{platform}.filter")
        with open(path, "w", encoding="utf-8") as f:
            f.write(
                build_filter_content(suite, platform, entries, days,
                                     min_flake_rate))
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
        update_suite_filters(suite, args.days, args.min_flake_rate / 100.0)


if __name__ == "__main__":
    try:
        main()
    except LuciAnalysisError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
