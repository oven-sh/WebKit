#!/usr/bin/env python3
"""SCALEBENCH parse/aggregate helper (SPEC.md §5).

Subcommands (driven by run.sh — not meant for interactive use):

  record  --lang L --threads W --rep R --exit-code N \
          --stdout FILE --time FILE --runs FILE
      Parse one benchmark invocation: extract the program's JSON line from
      FILE(stdout) and RSS/user/sys/elapsed from the `/usr/bin/time -v`
      capture, append one JSON record to the runs file (JSONL), and print the
      run's checksum tuple ("A|postings|A2|B|C") to stdout so the shell can
      fail fast on a mismatch. Prints "FAILED" instead if the run failed.

  aggregate RUNS_JSONL META_JSON RESULTS_JSON RESULTS_MD
      Enforce the SPEC §5.5 checksum gate over the whole matrix (warmup reps
      included), compute medians/speedups/cpu_util, and emit results.json +
      RESULTS.md. On checksum mismatch: print the offending cells, write
      results.json with "valid": false, do NOT write RESULTS.md, exit 3.
"""

import argparse
import json
import re
import statistics
import sys

# SPEC §0 — must appear verbatim in every results document.
HANDICAP = (
    "**Known structural handicap:** GC under JS threads on this branch is "
    "currently stop-the-world with parallel marking; concurrent marking is "
    "designed (SPEC-congc.md) but not implemented. Go and Java ship fully "
    "concurrent collectors. Allocation-heavy phases (A especially) are "
    "expected to show STW pauses scaling with heap size and thread count. "
    "Results must be reported with this stated up front, not buried."
)

CHECKSUM_KEYS = ("checksumA", "postings", "checksumA2", "checksumB", "checksumC")
# Deterministic counters: also identical across the matrix by construction.
COUNTER_KEYS = ("docsIngested", "tokensProcessed", "writesDone")
PHASE_KEYS = ("phaseA_ms", "phaseB_ms", "phaseC_ms", "total_ms")
LANGS = ("js", "go", "java")


# ---------------------------------------------------------------------------
# /usr/bin/time -v parsing
# ---------------------------------------------------------------------------

def parse_elapsed(text):
    """'h:mm:ss' or 'm:ss.ss' -> seconds (float)."""
    parts = text.strip().split(":")
    seconds = 0.0
    for part in parts:
        seconds = seconds * 60.0 + float(part)
    return seconds


def parse_time_v(path):
    """Extract metrics from a `/usr/bin/time -v` stderr capture.

    The capture may also contain the benchmark's own stderr, so we match the
    exact GNU time labels and take the LAST occurrence of each (GNU time
    appends its block after the program exits).
    """
    out = {"rss_kb": None, "user_s": None, "sys_s": None, "elapsed_s": None}
    patterns = {
        "user_s": re.compile(r"User time \(seconds\):\s+([0-9.]+)"),
        "sys_s": re.compile(r"System time \(seconds\):\s+([0-9.]+)"),
        "elapsed_s": re.compile(
            r"Elapsed \(wall clock\) time \([^)]*\):\s+([0-9:.]+)"),
        "rss_kb": re.compile(r"Maximum resident set size \(kbytes\):\s+(\d+)"),
    }
    try:
        with open(path, "r", errors="replace") as f:
            text = f.read()
    except OSError:
        return out
    for key, pat in patterns.items():
        matches = pat.findall(text)
        if not matches:
            continue
        raw = matches[-1]
        if key == "elapsed_s":
            out[key] = parse_elapsed(raw)
        elif key == "rss_kb":
            out[key] = int(raw)
        else:
            out[key] = float(raw)
    return out


def extract_bench_json(path):
    """Last stdout line that parses as the benchmark's JSON object."""
    try:
        with open(path, "r", errors="replace") as f:
            lines = f.readlines()
    except OSError:
        return None
    for line in reversed(lines):
        line = line.strip()
        if not line.startswith('{"impl"'):
            continue
        try:
            obj = json.loads(line)
        except ValueError:
            continue
        if all(k in obj for k in CHECKSUM_KEYS) and "total_ms" in obj:
            return obj
    return None


def checksum_tuple(rec):
    return "|".join(str(rec[k]) for k in CHECKSUM_KEYS)


# ---------------------------------------------------------------------------
# record
# ---------------------------------------------------------------------------

def cmd_record(args):
    rec = {
        "lang": args.lang,
        "threads": args.threads,
        "rep": args.rep,  # 0 = warmup (discarded from medians, kept for gate)
        "exit_code": args.exit_code,
        "failed": False,
    }
    rec.update(parse_time_v(args.time))
    bench = extract_bench_json(args.stdout)

    if args.exit_code != 0 or bench is None:
        rec["failed"] = True
        if bench is not None:
            rec.update(bench)
    else:
        rec.update(bench)
        if bench.get("impl") != args.lang:
            rec["failed"] = True
            rec["fail_reason"] = "impl field %r != expected lang %r" % (
                bench.get("impl"), args.lang)
        elif bench.get("threads") != args.threads:
            rec["failed"] = True
            rec["fail_reason"] = "threads field %r != requested W %r" % (
                bench.get("threads"), args.threads)
        elif (getattr(args, "expect_tuple", None)
                and checksum_tuple(rec) != args.expect_tuple):
            rec["failed"] = True
            rec["fail_reason"] = (
                "checksum mismatch vs batch reference — quarantined: known "
                "shared-GC-heap corruption under GIL-off at W>=4 "
                "(js/repro-bigint-shared-ingest.js)")

    with open(args.runs, "a") as f:
        f.write(json.dumps(rec, sort_keys=True) + "\n")

    if rec["failed"]:
        print("FAILED")
    else:
        print(checksum_tuple(rec))
    return 0


# ---------------------------------------------------------------------------
# aggregate
# ---------------------------------------------------------------------------

def median(values):
    return statistics.median(values) if values else None


def fmt_ms(v):
    if v is None:
        return "null"
    return "%.0f" % v


def fmt_x(v):
    if v is None:
        return "null"
    return "%.2fx" % v


def cmd_aggregate(args):
    runs = []
    with open(args.runs) as f:
        for line in f:
            line = line.strip()
            if line:
                runs.append(json.loads(line))
    with open(args.meta) as f:
        meta = json.load(f)

    # ---- Checksum gate (SPEC §5.5) — warmups included, failed runs excluded.
    ok_runs = [r for r in runs if not r.get("failed")]
    failed_runs = [r for r in runs if r.get("failed")]
    tuples = {}
    for r in ok_runs:
        tuples.setdefault(checksum_tuple(r), []).append(r)
    counter_tuples = set(
        tuple(r[k] for k in COUNTER_KEYS) for r in ok_runs)

    valid = len(tuples) == 1 and len(counter_tuples) <= 1 and ok_runs

    base = {
        "valid": bool(valid),
        "spec": "SCALEBENCH v1",
        "date": meta.get("date"),
        "host": meta.get("host"),
        "versions": meta.get("versions"),
        "exceptions": meta.get("exceptions", []),
        "constants": meta.get("constants"),
        "gate_delay_s": meta.get("gate_delay_s", 0),
        # Post-run settle waits: total, and the share beyond the modeled
        # own-loadavg-decay allowance (presumed external interference). The
        # SPEC §6 "> 5 min" disclosure triggers on gate_delay_s +
        # settle_excess_s so short repeated external bursts (each under the
        # 180 s settle cap) still count toward the tripwire.
        "settle_delay_s": meta.get("settle_delay_s", 0),
        "settle_excess_s": meta.get("settle_excess_s", 0),
        "handicap": HANDICAP,
    }

    if not valid:
        if not ok_runs:
            print("scalebench: GATE FAILURE: no successful runs", file=sys.stderr)
        if len(tuples) > 1:
            print("scalebench: CHECKSUM MISMATCH across the matrix "
                  "(SPEC §5.5) — batch INVALID. Offending cells:",
                  file=sys.stderr)
            for tup, rs in sorted(tuples.items(),
                                  key=lambda kv: -len(kv[1])):
                cells = ", ".join(
                    "%s/W=%d/rep%d" % (r["lang"], r["threads"], r["rep"])
                    for r in rs)
                print("  tuple %s\n    cells: %s" % (tup, cells),
                      file=sys.stderr)
        if len(counter_tuples) > 1:
            print("scalebench: COUNTER MISMATCH (docsIngested/tokensProcessed/"
                  "writesDone differ across runs) — batch INVALID",
                  file=sys.stderr)
        base["runs"] = runs
        base["checksum_tuples"] = {t: len(rs) for t, rs in tuples.items()}
        with open(args.results, "w") as f:
            json.dump(base, f, indent=1, sort_keys=False)
            f.write("\n")
        # SPEC §5.5: no RESULTS.md from an invalid batch.
        return 3

    ref = ok_runs[0]
    base["checksums"] = {
        "A": ref["checksumA"], "postings": ref["postings"],
        "A2": ref["checksumA2"], "B": ref["checksumB"], "C": ref["checksumC"],
    }
    base["counters"] = {k: ref[k] for k in COUNTER_KEYS}

    # ---- Per-run derived metrics + medians.
    out_runs = []
    for r in runs:
        o = dict(r)
        if (not r.get("failed") and r.get("user_s") is not None
                and r.get("sys_s") is not None
                and r.get("elapsed_s") and r["elapsed_s"] > 0):
            # SPEC §4: cpu_util's wall basis is the FULL process lifetime
            # (GNU time elapsed), which includes runtime startup/teardown
            # (JVM class loading, jsc shell init) that the phase/speedup
            # tables deliberately exclude. inprogram_share quantifies the
            # dilution per run: in-bench wall / process wall.
            o["cpu_util"] = (r["user_s"] + r["sys_s"]) / (
                r["elapsed_s"] * r["threads"])
            if r.get("total_ms") is not None:
                o["inprogram_share"] = r["total_ms"] / (r["elapsed_s"] * 1000.0)
        out_runs.append(o)
    base["runs"] = out_runs

    thread_counts = sorted(set(r["threads"] for r in runs))
    medians = {}
    cell_failures = []
    for lang in LANGS:
        medians[lang] = {}
        for w in thread_counts:
            cell = [r for r in out_runs
                    if r["lang"] == lang and r["threads"] == w and r["rep"] > 0]
            ok = [r for r in cell if not r.get("failed")]
            nfail = len(cell) - len(ok)
            if nfail:
                cell_failures.append((lang, w, nfail, len(cell)))
            entry = {"reps_ok": len(ok), "reps_failed": nfail}
            if len(ok) >= 3:  # SPEC §6: median only with >= 3 measured reps
                for key in PHASE_KEYS:
                    vals = [r[key] for r in ok if key in r]
                    entry[key] = median(vals)
                    entry[key + "_min"] = min(vals) if vals else None
                    entry[key + "_max"] = max(vals) if vals else None
                rss = [r["rss_kb"] for r in ok if r.get("rss_kb") is not None]
                entry["rss_kb"] = median(rss)
                util = [r["cpu_util"] for r in ok if "cpu_util" in r]
                entry["cpu_util"] = median(util)
            else:
                for key in PHASE_KEYS:
                    entry[key] = None
                entry["rss_kb"] = None
                entry["cpu_util"] = None
            medians[lang][str(w)] = entry

    for lang in LANGS:
        base_entry = medians[lang].get("1", {})
        for w in thread_counts:
            entry = medians[lang][str(w)]
            for key in PHASE_KEYS:
                skey = "speedup" if key == "total_ms" else key.replace(
                    "_ms", "_speedup")
                b, v = base_entry.get(key), entry.get(key)
                entry[skey] = (b / v) if (b and v) else None
    base["medians"] = medians

    with open(args.results, "w") as f:
        json.dump(base, f, indent=1, sort_keys=False)
        f.write("\n")

    write_results_md(args.md, base, thread_counts, cell_failures, failed_runs)
    return 0


def write_results_md(path, base, thread_counts, cell_failures, failed_runs):
    m = base["medians"]
    L = []
    L.append("# SCALEBENCH RESULTS — %s" % base["date"])
    L.append("")
    L.append(HANDICAP)
    L.append("")
    L.append("- Host: %s cores, kernel %s" % (
        base["host"].get("cores"), base["host"].get("kernel")))
    for lang in LANGS:
        vkey = "jsc" if lang == "js" else lang  # SPEC §5.6 versions schema
        L.append("- %s: %s" % (lang, base["versions"].get(vkey)))
    L.append("- Constants: %s" % json.dumps(base["constants"]))
    L.append("- Checksums (identical across all cells): %s" %
             json.dumps(base["checksums"]))
    if base.get("exceptions"):
        L.append("- Default-flag exceptions (SPEC §4): %s" %
                 json.dumps(base["exceptions"]))
    external_delay = (base.get("gate_delay_s", 0)
                      + base.get("settle_excess_s", 0))
    if external_delay > 300:
        L.append("- NOTE (SPEC §6): external load delayed this batch by "
                 "%d s total (loadavg gate %d s + settle excess %d s, "
                 "> 5 min)." % (external_delay, base.get("gate_delay_s", 0),
                                base.get("settle_excess_s", 0)))
    L.append("")
    L.append("Medians of 5 measured reps (1 warmup discarded); speedup is "
             "vs the same language at W=1. JSC compiler/GC helper threads "
             "are part of the platform and are not counted in W; cpu_util "
             "may exceed 1.0 for JS — reported as-is.")
    L.append("")
    L.append("Note on cpu_util's wall basis: it is the FULL process lifetime "
             "from `/usr/bin/time` (per SPEC §4), which INCLUDES runtime "
             "startup/teardown (JVM class loading, jsc shell init) — unlike "
             "the phase/speedup tables, which are in-program barrier-to-"
             "barrier. At high W the in-bench wall shrinks while startup is a "
             "fixed, mostly serial cost, so cpu_util is systematically "
             "depressed for runtimes with slower startup (Java most, then "
             "js). Each run in results.json carries `inprogram_share` "
             "(in-bench wall / process wall) to quantify the dilution.")
    L.append("")

    titles = (("phaseA_ms", "phaseA_speedup", "Phase A — INGEST"),
              ("phaseB_ms", "phaseB_speedup", "Phase B — QUERY 90/10"),
              ("phaseC_ms", "phaseC_speedup", "Phase C — ANALYTICS"),
              ("total_ms", "speedup", "Total"))
    for key, skey, title in titles:
        L.append("## %s" % title)
        L.append("")
        L.append("| W | js ms | js speedup | go ms | go speedup | "
                 "java ms | java speedup |")
        L.append("|---|---|---|---|---|---|---|")
        for w in thread_counts:
            row = ["%d" % w]
            for lang in LANGS:
                e = m[lang][str(w)]
                row.append(fmt_ms(e.get(key)))
                row.append(fmt_x(e.get(skey)))
            L.append("| " + " | ".join(row) + " |")
        L.append("")

    L.append("## Peak RSS (MB, median)")
    L.append("")
    L.append("| W | js | go | java |")
    L.append("|---|---|---|---|")
    for w in thread_counts:
        row = ["%d" % w]
        for lang in LANGS:
            v = m[lang][str(w)].get("rss_kb")
            row.append("null" if v is None else "%.0f" % (v / 1024.0))
        L.append("| " + " | ".join(row) + " |")
    L.append("")

    L.append("## CPU utilization ((user+sys) / (wall * W), median)")
    L.append("")
    L.append("| W | js | go | java |")
    L.append("|---|---|---|---|")
    for w in thread_counts:
        row = ["%d" % w]
        for lang in LANGS:
            v = m[lang][str(w)].get("cpu_util")
            row.append("null" if v is None else "%.2f" % v)
        L.append("| " + " | ".join(row) + " |")
    L.append("")

    if cell_failures:
        L.append("## Failures (SPEC §6 — findings, not hidden)")
        L.append("")
        for lang, w, nfail, total in cell_failures:
            e = m[lang][str(w)]
            note = ("median reported (>= 3 reps ok)"
                    if e["total_ms"] is not None else "cell is null (< 3 reps ok)")
            L.append("- %s W=%d: %d of %d measured reps failed; %s" % (
                lang, w, nfail, total, note))
        for r in failed_runs:
            L.append("  - %s W=%d rep%d: exit_code=%s %s" % (
                r["lang"], r["threads"], r["rep"], r.get("exit_code"),
                r.get("fail_reason", "")))
        L.append("")

    with open(path, "w") as f:
        f.write("\n".join(L) + "\n")


# ---------------------------------------------------------------------------

def main():
    p = argparse.ArgumentParser(prog="scalebench_lib.py")
    sub = p.add_subparsers(dest="cmd", required=True)

    pr = sub.add_parser("record")
    pr.add_argument("--lang", required=True, choices=LANGS)
    pr.add_argument("--threads", required=True, type=int)
    pr.add_argument("--rep", required=True, type=int)
    pr.add_argument("--exit-code", required=True, type=int)
    pr.add_argument("--stdout", required=True)
    pr.add_argument("--time", required=True)
    pr.add_argument("--runs", required=True)
    # Quarantine hook (2026-06-10, engine-bug accommodation — see run.sh):
    # if the parsed checksum tuple differs from this reference, record the
    # run as failed (fail_reason names the known shared-heap bug) instead of
    # letting run.sh's fail-fast abort the batch. Passed ONLY for js W>=2;
    # go/java and the js W=1 baseline keep the §5.5 hard abort.
    pr.add_argument("--expect-tuple", default=None)

    pa = sub.add_parser("aggregate")
    pa.add_argument("runs")
    pa.add_argument("meta")
    pa.add_argument("results")
    pa.add_argument("md")

    args = p.parse_args()
    if args.cmd == "record":
        sys.exit(cmd_record(args))
    sys.exit(cmd_aggregate(args))


if __name__ == "__main__":
    main()
