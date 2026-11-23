"""Validate one native BOOM SFENCE.VMA fixture's report and Spike trace."""

import argparse
import json
import re
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def check_verdict(report, profile):
    require(report["platform"] == "boom" and report["verdict"] == "Pass",
            "expected a BOOM Pass")
    harts = {hart["hart"]: hart for hart in report["harts"]}
    require(report["hart_count"] == len(report["harts"]) == 4 and set(harts) == set(range(4)),
            "expected four distinct harts")
    fences, stores, min_loads = {
        "bare": ([1, 1, 1, 1], [2, 1, 1, 1], 1),
        "sv39": ([3, 2, 2, 2], [9, 1, 1, 1], 2),
    }[profile]
    for hart, expected_fences, expected_stores in zip(range(4), fences, stores):
        stats = harts[hart]
        require(stats["sfence_vma_compared"] == expected_fences,
                f"hart {hart} has unexpected SFENCE.VMA coverage")
        require(stats["accepted_load_operations"] == stats["load_compares"]
                == stats["ppo_load_checks"] >= min_loads,
                f"hart {hart} has missing load or load PPO comparisons")
        require(stats["accepted_store_operations"] == stats["store_audits"]
                == stats["verified_store_operations"] == stats["ppo_store_checks"]
                == expected_stores, f"hart {hart} has missing store audits or PPO comparisons")

    cycle = report["completed_cycle"]
    producers = report["producer_closure"]
    require(cycle > 0 and len(producers) == len({p["producer_id"] for p in producers}) == 8,
            "expected eight distinct producers and a completed cycle")
    require(all(p["physically_drained"] is True and p["closed_through"] is not None
                and p["delivered_through"] is not None and p["closed_through"] >= cycle
                and p["delivered_through"] >= cycle for p in producers),
            "not all producer streams drained and closed through completion")
    require(report["pending"] == {
        "semantic_events": 0, "harts_without_completion": 0, "producers_without_drain": 0,
    }, "pending work did not drain")


def check_remap_trace(log, symbols):
    # The PTE update and old/new loads must come from independent S-mode Spike steps.
    pte = symbols["alias_pte"]
    descriptor = ((symbols["new_page"] >> 12) << 10) | symbols["LEAF_FLAGS"]
    update = re.search(
        rf"^core +0: 1 0x{symbols['update_pte']:016x} .* mem 0x{pte:016x} 0x{descriptor:016x}\b",
        log, re.M,
    )
    require(update is not None, "missing Spike store trace for the new leaf PTE")
    va = symbols["ALIAS_VA"]
    for hart in range(4):
        for site, value in [("old_load", 0x31), ("new_load", 0x72)]:
            load = re.search(
                rf"^core +{hart}: 1 0x{symbols[site]:016x} .* x[0-9]+ +0x{value:016x} "
                rf"mem 0x{va:016x}\b", log, re.M,
            )
            require(load is not None, f"hart {hart} did not read the expected value at {site}")
            require((load.start() < update.start()) == (site == "old_load"),
                    f"hart {hart} {site} was on the wrong side of the PTE update")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", choices=("bare", "sv39"))
    parser.add_argument("log", type=Path)
    parser.add_argument("symbols", type=Path)
    args = parser.parse_args()
    try:
        log = args.log.read_text(encoding="utf-8")
        reports = re.findall(r"^DIFFTEST_S_FINAL (.*)$", log, re.M)
        require(len(reports) == 1, "expected one DIFFTEST_S_FINAL report")
        report = json.loads(reports[0])
        check_verdict(report, args.profile)
        if args.profile == "sv39":
            symbols = {fields[0]: int(fields[2], 16) for fields in
                       (line.split() for line in args.symbols.read_text().splitlines())}
            check_remap_trace(log, symbols)
    except (KeyError, OSError, TypeError, ValueError) as error:
        parser.exit(1, f"{args.log}: {error}\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
