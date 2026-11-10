#!/usr/bin/env python3
"""Summarise kernel fuzzing and sanitizer logs.

  fuzzlog.py fuzz <syz-manager.log>     execution and coverage timeline
  fuzzlog.py scan <dmesg-or-serial.log> classify sanitizer and kernel reports
  fuzzlog.py kcsan <dmesg.log>          data-race reports, split by whether
                                        the racing access is in a project driver
"""
from __future__ import annotations

import json
import re
import sys
from collections import Counter
from dataclasses import asdict, dataclass
from datetime import datetime

DRIVERS = ("sgpu", "mychar")

STAT_RE = re.compile(
    r"^(\d{4}/\d\d/\d\d \d\d:\d\d:\d\d) .*?corpus=(\d+) coverage=(\d+) exec total=(\d+) \(\d+/(?:sec|min|hour)\)"
)
TS_RE = re.compile(r"^\[\s*\d+\.\d+\]\s?")
ACCESS_RE = re.compile(r"\bby (?:task \d+|interrupt) on cpu \d+:|\bby task \d+")
REPORT_RES = [
    ("kasan", re.compile(r"BUG: KASAN: ([\w-]+) in (\S+)")),
    ("kcsan", re.compile(r"BUG: KCSAN: ([\w-]+) in (\S+)")),
    ("ubsan", re.compile(r"UBSAN: ([\w-]+) in (\S+)")),
    ("lockdep", re.compile(r"WARNING: (possible circular locking dependency detected|"
                           r"inconsistent lock state|possible recursive locking detected|"
                           r"suspicious RCU usage)()")),
    ("warning", re.compile(r"WARNING: CPU: \d+ PID: \d+ at (\S+)()")),
    ("bug", re.compile(r"(?:kernel )?BUG (?:at|: )(.+?)()$")),
]


@dataclass(frozen=True)
class FuzzSummary:
    samples: int
    duration_s: int
    executions: int
    average_exec_per_s: float
    final_corpus: int
    final_coverage: int
    time_to_90pct_coverage_s: int


@dataclass(frozen=True)
class Report:
    kind: str
    detail: str
    location: str


def parse_fuzz(lines) -> FuzzSummary | None:
    points = []
    for line in lines:
        m = STAT_RE.match(line)
        if m:
            t = datetime.strptime(m[1], "%Y/%m/%d %H:%M:%S")
            points.append((t, int(m[2]), int(m[3]), int(m[4])))
    if not points:
        return None
    start = points[0][0]
    duration = int((points[-1][0] - start).total_seconds())
    final = points[-1]
    target = final[2] * 0.9
    reach = next(p for p in points if p[2] >= target)
    return FuzzSummary(
        samples=len(points),
        duration_s=duration,
        executions=final[3],
        average_exec_per_s=round(final[3] / duration, 1) if duration else 0.0,
        final_corpus=final[1],
        final_coverage=final[2],
        time_to_90pct_coverage_s=int((reach[0] - start).total_seconds()),
    )


def parse_reports(lines) -> list[Report]:
    reports = []
    for raw in lines:
        line = TS_RE.sub("", raw.rstrip("\n"))
        for kind, pattern in REPORT_RES:
            m = pattern.search(line)
            if m:
                groups = [g for g in m.groups() if g]
                detail = groups[0] if groups else ""
                location = groups[1] if len(groups) > 1 else ""
                reports.append(Report(kind, detail, location))
                break
    return reports


def split_kcsan(lines) -> tuple[list[str], list[str]]:
    """Return (titles with a racing access in a project driver, the rest).

    Only the frame directly under each access line is the racing code. A driver
    further down a stack is merely a caller, for example of mutex_lock().
    """
    ours, other = [], []
    current, body = None, []

    def racing_frames():
        return [body[i + 1] for i in range(len(body) - 1) if ACCESS_RE.search(body[i])]

    def flush():
        if current is None:
            return
        hit = any(d + "_" in current for d in DRIVERS) or any(
            d in frame for frame in racing_frames() for d in DRIVERS
        )
        (ours if hit else other).append(current)

    for raw in lines:
        line = TS_RE.sub("", raw.rstrip("\n"))
        if "BUG: KCSAN:" in line:
            flush()
            current, body = line[line.index("BUG: KCSAN:"):], [line]
        elif current is not None:
            body.append(line)
    flush()
    return ours, other


def main(argv) -> int:
    if len(argv) != 3 or argv[1] not in ("fuzz", "scan", "kcsan"):
        print(__doc__)
        return 2
    with open(argv[2], encoding="utf-8", errors="replace") as f:
        lines = f.readlines()
    if argv[1] == "fuzz":
        summary = parse_fuzz(lines)
        print(json.dumps(asdict(summary), indent=2) if summary else "no statistics found")
        return 0 if summary else 1
    if argv[1] == "scan":
        reports = parse_reports(lines)
        print(f"reports: {len(reports)}")
        for (kind, detail, location), n in Counter(
            (r.kind, r.detail, r.location) for r in reports
        ).most_common():
            print(f"{n:5d}  {kind:8s} {detail} {location}")
        return 0
    ours, other = split_kcsan(lines)
    print(f"kcsan_reports_total={len(ours) + len(other)} racing_in_project_drivers={len(ours)}")
    for title, n in Counter(ours).most_common():
        print(f"{n:5d}  driver  {title}")
    for title, n in Counter(other).most_common():
        print(f"{n:5d}  other   {title}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
