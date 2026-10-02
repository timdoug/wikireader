#!/usr/bin/env python3
"""Audit complete inventories and preserve raw and follow-up suite results."""
import argparse
from collections import Counter
import importlib.util
import json
from pathlib import Path
import re

spec = importlib.util.spec_from_file_location("upstream_run", Path(__file__).with_name("run.py"))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def records(directory):
    final = json.loads((directory / "results.json").read_text())
    observed = {x["name"]: x for x in final["results"]}
    # Reparse raw output too: old runs omitted assertions on a timeout and
    # called a reproducible emulator crash MISSING. Keep originals untouched.
    trials = sorted(directory.glob("batch-*/results.json")) + sorted(directory.glob("retry-*/results.json"))
    for path in trials:
        report = json.loads(path.read_text())
        raw = path.with_name("boot.log").read_text(errors="replace")
        text = re.sub(r" *\[[a-z][^\]\n]*\]\n?", "", raw.replace("\r", "\n"))
        for case in report["results"]:
            name = case["name"]
            start = re.search(r"^UPSTREAM-BEGIN " + re.escape(name) + r"\s*$", text, re.M)
            end = re.search(r"^UPSTREAM-RESULT " + re.escape(name) + r" (\d+)\s*$", text, re.M)
            segment = text[start.end():end.start() if end else None] if start else ""
            result = runner.verdict(report["suite"], case, case["exit_status"], segment)
            if result["status"] == "MISSING" and runner.crashed(raw, name):
                result["status"] = "CRASH"
            observed[name] = {"name": name, "exit_status": case["exit_status"], **result,
                              "source_report": str(path), "boot_log_sha256": runner.sha(path.with_name("boot.log"))}
    return final, observed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binaries", type=Path, default=runner.ROOT / "linux/artifacts/upstream-suites")
    parser.add_argument("--busybox", type=Path, required=True)
    parser.add_argument("--uclibc", type=Path, required=True)
    parser.add_argument("--hush", type=Path, action="append", default=[])
    parser.add_argument("--libc-followup", type=Path, action="append", default=[])
    parser.add_argument("--busybox-followup", type=Path, action="append", default=[])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    manifest = json.loads((args.binaries / "manifest.json").read_text())
    manifest_hash = runner.sha(args.binaries / "manifest.json")
    summary = {"manifest_sha256": manifest_hash, "uclibc_upstream_disabled": manifest["uclibc_upstream_disabled"]}
    full = {}
    for suite in ("busybox", "uclibc"):
        directory = getattr(args, suite).resolve()
        final, observed = records(directory)
        if final["manifest_sha256"] != manifest_hash or set(observed) != {x["name"] for x in manifest[suite]}:
            raise SystemExit(f"{suite}: full inventory/manifest mismatch")
        counts = dict(Counter(x["status"] for x in observed.values()))
        summary[suite] = {"raw_counts": final["counts"], "audited_counts": counts,
                          "cases": len(observed), "results": list(observed.values()),
                          "raw_report": str(directory / "results.json")}
        full[suite] = observed
    hush = {}
    pattern = r"^(?:-n )?(hush-[^:\n]+):[ \n]*(ok|skip[^\n]*|fail[^\n]*)$"
    for path in sorted(args.busybox.glob("batch-*/boot.log")):
        text = path.read_text(errors="replace").replace("\r", "\n")
        for name, status in re.findall(pattern, runner.hush_output(text), re.M):
            hush["hush/" + name] = {"name": "hush/" + name,
                                   "status": "PASS" if status == "ok" else "SKIPPED" if status.startswith("skip") else "FAIL",
                                   "boot_log": str(path.resolve()), "boot_log_sha256": runner.sha(path)}
    for directory in args.hush:
        final, observed = records(directory.resolve())
        if final["manifest_sha256"] != manifest_hash:
            raise SystemExit("Hush follow-up manifest mismatch")
        hush.update(observed)
    expected = {x["name"] for x in runner.hush_cases(args.binaries / "suite.tar.gz")}
    if set(hush) - expected:
        raise SystemExit("Unrecognized Hush case in raw output")
    for name in expected - set(hush):
        hush[name] = {"name": name, "status": "MISSING"}
    summary["hush_individual"] = {"cases": len(hush), "counts": dict(Counter(x["status"] for x in hush.values())),
                                  "results": sorted(hush.values(), key=lambda x: x["name"])}
    libc = dict(full["uclibc"])
    followups = []
    for directory in args.libc_followup:
        final, observed = records(directory.resolve())
        if final["manifest_sha256"] != manifest_hash or set(observed) - set(libc):
            raise SystemExit("Libc follow-up inventory mismatch")
        followups.append({"report": str(directory.resolve() / "results.json"),
                          "direct_mode": final.get("direct_mode", False), "results": list(observed.values())})
        for name, result in observed.items():
            libc[name] = {**result, "direct_mode": final.get("direct_mode", False)}
    summary["uclibc_followups"] = followups
    summary["uclibc_after_followups"] = {"counts": dict(Counter(x["status"] for x in libc.values())),
                                         "results": list(libc.values())}
    applets = dict(full["busybox"])
    followups = []
    for directory in args.busybox_followup:
        final, observed = records(directory.resolve())
        if final["manifest_sha256"] != manifest_hash or set(observed) - set(applets):
            raise SystemExit("BusyBox follow-up inventory mismatch")
        followups.append({"report": str(directory.resolve() / "results.json"), "results": list(observed.values())})
        applets.update(observed)
    summary["busybox_followups"] = followups
    summary["busybox_after_followups"] = {"counts": dict(Counter(x["status"] for x in applets.values())),
                                          "results": list(applets.values())}
    disabled = args.binaries / "busybox-disabled-suites.json"
    if disabled.exists():
        summary["busybox_disabled_suites"] = json.loads(disabled.read_text())
    host = args.binaries / "busybox-sourcecode-host.log"
    if host.exists():
        assertions = Counter(re.findall(r"^(PASS|FAIL|SKIPPED):", host.read_text(), re.M))
        summary["busybox_host_source_checks"] = {"assertions": dict(assertions), "log": str(host.resolve()),
                                                "sha256": runner.sha(host)}
    summary["accounted"] = all(x["status"] != "MISSING" for x in [*full["busybox"].values(), *libc.values(), *hush.values()])
    args.output.write_text(json.dumps(summary, indent=2) + "\n")
    for key in ("busybox", "uclibc", "hush_individual", "uclibc_after_followups", "busybox_after_followups"):
        print(key, summary[key].get("audited_counts", summary[key].get("counts")))
    print("accounted:", summary["accounted"])


if __name__ == "__main__":
    main()
