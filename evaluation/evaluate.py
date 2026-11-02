#!/usr/bin/env python3
# nanoalloc evaluation package - single entry point.
# SPDX-License-Identifier: MIT
"""Evaluate a nanoalloc implementation against the frozen buddy baseline.

Steps:
  1. Check that --baseline is the frozen reference (baseline_manifest.json).
  2. Build the baseline, an identical copy of it and the candidate, each
     independently, with the same compiler, build type and flags, together with
     the trusted drivers of this package (driver/).
  3. Correctness gate: build the candidate with its own CMake project and run
     its own tests (--candidate-tests); run the conformance driver on the
     baseline (for reference) and on the candidate (gate).
  4. A/A calibration: baseline against its identical copy.
  5. Measurement: baseline against candidate.
     In each round (--processes rounds), every workload runs in one timing
     process per implementation, the two back to back, the order alternating
     from pair to pair. A process replays its workload --warmup + --iterations
     times and validates every replay's checksum.
  6. Memory footprint of each implementation (smallest capacity that
     completes each workload).
  7. Scores, verdict and reports (report.md, results.json, results.csv).

Score per workload: 100 * baseline median time / candidate median time, where
the median time of an implementation is the median over its processes of each
process's median iteration time. Overall: geometric mean of the nine scores.

Exit status: 0 PASS (or measured, when no --threshold is given), 1 FAIL,
2 INCONCLUSIVE, 3 evaluation error (invalid input, baseline unusable, tools
missing).

Requires Python 3.9+, CMake 3.20+ and a C++20 compiler. No network access and
no Git metadata are needed.
"""

import argparse
import csv
import datetime
import hashlib
import json
import math
import os
import platform
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

EVALUATOR_VERSION = "1.0.0"
HERE = Path(__file__).resolve().parent
DRIVER_DIR = HERE / "driver"
MANIFEST = HERE / "baseline_manifest.json"
WORKLOADS = ["small", "medium", "mixed", "lifo", "random", "bursts", "long_short",
             "fragmentation", "reuse"]
GATED_CATEGORIES = ["api", "integrity", "reuse", "budget"]

EXIT_PASS, EXIT_FAIL, EXIT_INCONCLUSIVE, EXIT_ERROR = 0, 1, 2, 3


class EvaluationError(Exception):
    """The evaluation itself cannot be carried out (exit status 3)."""


# ---------------------------------------------------------------------------
# Logging and processes
# ---------------------------------------------------------------------------

class Log:
    def __init__(self):
        self.file = None

    def open(self, path):
        self.file = open(path, "w", encoding="utf-8")

    def __call__(self, message=""):
        stamp = datetime.datetime.now().strftime("%H:%M:%S")
        line = f"[{stamp}] {message}" if message else ""
        print(line, flush=True)
        if self.file:
            self.file.write(line + "\n")
            self.file.flush()


log = Log()


def run(command, log_path, cwd=None, timeout=None, pin_cpu=None, env=None):
    """Runs a command, writing its combined output to log_path."""
    preexec = None
    if pin_cpu is not None and hasattr(os, "sched_setaffinity"):
        preexec = lambda: os.sched_setaffinity(0, {pin_cpu})  # noqa: E731
    with open(log_path, "w", encoding="utf-8") as out:
        out.write("$ " + " ".join(str(part) for part in command) + "\n")
        out.flush()
        try:
            result = subprocess.run([str(part) for part in command], cwd=cwd, stdout=out,
                                    stderr=subprocess.STDOUT, timeout=timeout,
                                    preexec_fn=preexec, env=env)
        except subprocess.TimeoutExpired:
            out.write(f"\nTIMEOUT after {timeout} s\n")
            return 124
    return result.returncode


def run_json(command, log_path, timeout, pin_cpu=None):
    """Runs a driver that prints one JSON document on stdout."""
    preexec = None
    if pin_cpu is not None and hasattr(os, "sched_setaffinity"):
        preexec = lambda: os.sched_setaffinity(0, {pin_cpu})  # noqa: E731
    with open(log_path, "w", encoding="utf-8") as err:
        err.write("$ " + " ".join(str(part) for part in command) + "\n")
        err.flush()
        try:
            result = subprocess.run([str(part) for part in command], stdout=subprocess.PIPE,
                                    stderr=err, timeout=timeout, preexec_fn=preexec)
        except subprocess.TimeoutExpired:
            return None, f"timed out after {timeout} s"
    if result.returncode < 0:
        return None, f"terminated by signal {-result.returncode}"
    try:
        document = json.loads(result.stdout.decode("utf-8", "replace"))
    except ValueError:
        return None, f"exit status {result.returncode}, no valid JSON output"
    if result.returncode != 0:
        return document, document.get("error", f"exit status {result.returncode}")
    return document, None


# ---------------------------------------------------------------------------
# Inputs
# ---------------------------------------------------------------------------

def tree_files(root):
    """Relative path -> SHA-256 of every file under include/ and src/."""
    files = {}
    for sub in ("include", "src"):
        base = Path(root) / sub
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.is_file():
                files[path.relative_to(root).as_posix()] = hashlib.sha256(
                    path.read_bytes()).hexdigest()
    return files


def tree_digest(files):
    text = "".join(f"{name}\0{digest}\n" for name, digest in sorted(files.items()))
    return hashlib.sha256(text.encode()).hexdigest()


def verify_baseline(baseline, allow_unverified):
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    actual = tree_files(baseline)
    expected = manifest["files"]
    missing = sorted(set(expected) - set(actual))
    extra = sorted(set(actual) - set(expected))
    changed = sorted(name for name in set(expected) & set(actual)
                     if expected[name] != actual[name])
    verified = not (missing or extra or changed)
    result = {"verified": verified, "reference": manifest["reference"],
              "digest": tree_digest(actual), "expected_digest": manifest["digest"],
              "missing": missing, "extra": extra, "changed": changed}
    if not verified:
        detail = f"missing {missing}, extra {extra}, changed {changed}"
        if not allow_unverified:
            raise EvaluationError(
                "--baseline does not match the frozen reference " +
                manifest["reference"] + f" ({detail}); use the unmodified snapshot or "
                "--allow-unverified-baseline")
        log(f"WARNING: baseline does not match the frozen reference: {detail}")
    return result


def stage_tree(source, destination, full):
    """Copies an implementation tree: include/ and src/ only, or (full) every
    file except VCS metadata and build directories."""
    if destination.exists():
        shutil.rmtree(destination)
    if full:
        ignore = shutil.ignore_patterns(".git", ".hg", ".svn", "build", "build-*", "_build",
                                        "cmake-build-*", "__pycache__")
        shutil.copytree(source, destination, ignore=ignore, symlinks=False)
    else:
        destination.mkdir(parents=True)
        for sub in ("include", "src"):
            if not (Path(source) / sub).is_dir():
                raise FileNotFoundError(f"{source} has no {sub}/ directory")
            shutil.copytree(Path(source) / sub, destination / sub, symlinks=False)


# ---------------------------------------------------------------------------
# Environment
# ---------------------------------------------------------------------------

def read_text(path):
    try:
        return Path(path).read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return None


def first_line(command):
    try:
        output = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=60).stdout.decode("utf-8", "replace")
        return output.strip().splitlines()[0] if output.strip() else None
    except (OSError, subprocess.SubprocessError):
        return None


def environment(args):
    cpu_model = None
    cpuinfo = read_text("/proc/cpuinfo") or ""
    for line in cpuinfo.splitlines():
        if line.lower().startswith(("model name", "hardware", "cpu model")):
            cpu_model = line.split(":", 1)[1].strip()
            break
    os_release = None
    for line in (read_text("/etc/os-release") or "").splitlines():
        if line.startswith("PRETTY_NAME="):
            os_release = line.split("=", 1)[1].strip('"')
    memory = None
    for line in (read_text("/proc/meminfo") or "").splitlines():
        if line.startswith("MemTotal:"):
            memory = line.split(":", 1)[1].strip()
    cgroup = read_text("/proc/1/cgroup") or ""
    container = {
        "dockerenv": Path("/.dockerenv").exists(),
        "containerenv": Path("/run/.containerenv").exists(),
        "cgroup_mentions_container": any(word in cgroup for word in ("docker", "containerd",
                                                                     "kubepods", "libpod")),
        "image": os.environ.get("NANOEVAL_IMAGE"),
    }
    container["in_container"] = any(value for key, value in container.items() if key != "image")
    affinity = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None
    try:
        load = os.getloadavg()
    except OSError:
        load = None
    return {
        "evaluator_version": EVALUATOR_VERSION,
        "date_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "hostname": platform.node(),
        "os": os_release,
        "kernel": platform.release(),
        "machine": platform.machine(),
        "cpu_model": cpu_model,
        "logical_cpus": os.cpu_count(),
        "cpu_affinity": affinity,
        "cpu_quota": read_text("/sys/fs/cgroup/cpu.max"),
        "cpu_governor": read_text("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"),
        "memory": memory,
        "load_average_at_start": load,
        "container": container,
        "python": platform.python_version(),
        "cmake": first_line([args.cmake, "--version"]),
        "compiler": first_line([args.cxx, "--version"]),
        "generator": args.generator,
        "pinned_cpu": args.pin_cpu,
    }


# ---------------------------------------------------------------------------
# Builds
# ---------------------------------------------------------------------------

def configure_and_build(source, build, logs, name, args, extra):
    if build.exists():
        shutil.rmtree(build)
    configure = [args.cmake, "-S", source, "-B", build, "-G", args.generator,
                 "-DCMAKE_BUILD_TYPE=" + args.build_type,
                 "-DCMAKE_CXX_COMPILER=" + args.cxx] + extra
    if args.cxx_flags:
        configure.append("-DCMAKE_CXX_FLAGS=" + args.cxx_flags)
    if run(configure, logs / f"{name}.configure.log") != 0:
        return False, f"configuration failed (see logs/{name}.configure.log)"
    if run([args.cmake, "--build", build, "--parallel", str(args.jobs)],
           logs / f"{name}.build.log") != 0:
        return False, f"build failed (see logs/{name}.build.log)"
    return True, None


def build_info(build, staged):
    """Compiler and flags actually used for the implementation's sources."""
    cache = {}
    for line in (read_text(build / "CMakeCache.txt") or "").splitlines():
        if ":" in line and "=" in line and not line.startswith(("//", "#")):
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    compiler_version = None
    for path in (build / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"):
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.startswith("set(CMAKE_CXX_COMPILER_VERSION"):
                compiler_version = line.split('"')[1]
            if line.startswith("set(CMAKE_CXX_COMPILER_ID "):
                cache["CMAKE_CXX_COMPILER_ID"] = line.split('"')[1]
    flags = None
    commands_path = build / "compile_commands.json"
    if commands_path.exists():
        for entry in json.loads(commands_path.read_text(encoding="utf-8")):
            if Path(entry["file"]).resolve().is_relative_to(Path(staged).resolve() / "src"):
                parts = entry.get("command", "").split()
                # Keep options, drop paths (they differ between trees).
                flags = [part for part in parts[1:] if part.startswith("-")
                         and not part.startswith(("-I", "-o", "-c", "-MF", "-MT", "-MD",
                                                  "-isystem"))]
                break
    return {
        "compiler_path": cache.get("CMAKE_CXX_COMPILER"),
        "compiler_id": cache.get("CMAKE_CXX_COMPILER_ID"),
        "compiler_version": compiler_version,
        "build_type": cache.get("CMAKE_BUILD_TYPE"),
        "cmake_cxx_flags": cache.get("CMAKE_CXX_FLAGS"),
        "cmake_cxx_flags_build_type": cache.get(
            "CMAKE_CXX_FLAGS_" + (cache.get("CMAKE_BUILD_TYPE") or "").upper()),
        "implementation_compile_flags": flags,
    }


def candidate_self_tests(source, work, logs, args):
    """Builds the candidate with its own CMake project and runs its tests."""
    if args.candidate_tests == "off":
        return {"status": "skipped", "detail": "--candidate-tests off"}
    if not (source / "CMakeLists.txt").exists():
        status = "failed" if args.candidate_tests == "on" else "not_available"
        return {"status": status, "detail": "no CMakeLists.txt at the top of the candidate"}
    build = work / "build" / "candidate-own"
    ok, detail = configure_and_build(source, build, logs, "candidate-own", args,
                                     ["-DBUILD_TESTING=ON"])
    if not ok:
        return {"status": "failed", "detail": detail}
    log_path = logs / "candidate-own.ctest.log"
    cmake_path = shutil.which(args.cmake)
    ctest = Path(cmake_path).with_name("ctest") if cmake_path else Path("ctest")
    code = run([ctest if ctest.exists() else "ctest", "--test-dir", build, "--output-on-failure", "--timeout",
                str(args.test_timeout), "-j", str(args.jobs)], log_path,
               timeout=args.test_timeout * 4)
    text = read_text(log_path) or ""
    summary = next((line.strip() for line in text.splitlines() if "tests passed" in line), None)
    if "No tests were found" in text:
        status = "failed" if args.candidate_tests == "on" else "not_available"
        return {"status": status, "detail": "the candidate defines no tests"}
    return {"status": "passed" if code == 0 else "failed",
            "detail": summary or f"ctest exit status {code} (see logs/candidate-own.ctest.log)"}


# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------

def geometric_mean(values):
    return math.exp(sum(math.log(value) for value in values) / len(values))


def median_absolute_deviation(values):
    center = statistics.median(values)
    return statistics.median(abs(value - center) for value in values)


def implementation_summary(runs):
    """Per workload: per-process medians, overall median and variability.
    `runs` maps each workload to its process results in round order."""
    summary = {}
    for workload in WORKLOADS:
        per_process = []
        within = []
        for entry in runs[workload]:
            samples = entry["iterations_ns"]
            per_process.append(statistics.median(samples))
            within.append(100.0 * median_absolute_deviation(samples) / statistics.median(samples))
        center = statistics.median(per_process)
        summary[workload] = {
            "process_medians_ns": per_process,
            "median_ns": center,
            "process_spread_pct": 100.0 * (max(per_process) - min(per_process)) / center,
            "process_cv_pct": (100.0 * statistics.stdev(per_process) / statistics.mean(per_process)
                               if len(per_process) > 1 else 0.0),
            "within_process_mad_pct": statistics.median(within),
        }
    return summary


def compare(reference_runs, other_runs):
    """Scores of `other` relative to `reference` (100 = equal speed)."""
    reference = implementation_summary(reference_runs)
    other = implementation_summary(other_runs)
    rounds = len(reference_runs[WORKLOADS[0]])
    workloads = {}
    for workload in WORKLOADS:
        round_scores = [100.0 * reference[workload]["process_medians_ns"][r] /
                        other[workload]["process_medians_ns"][r] for r in range(rounds)]
        workloads[workload] = {
            "reference_median_ns": reference[workload]["median_ns"],
            "other_median_ns": other[workload]["median_ns"],
            "speed_percent": 100.0 * reference[workload]["median_ns"] / other[workload]["median_ns"],
            "round_scores": round_scores,
            "round_min": min(round_scores),
            "round_max": max(round_scores),
            "reference_variability": reference[workload],
            "other_variability": other[workload],
        }
    round_overall = [geometric_mean([workloads[w]["round_scores"][r] for w in WORKLOADS])
                     for r in range(rounds)]
    overall = geometric_mean([workloads[w]["speed_percent"] for w in WORKLOADS])
    return {
        "workloads": workloads,
        "overall_percent": overall,
        "round_overall": round_overall,
        "median_round_percent": statistics.median(round_overall),
        # [min, max] of the per-round scores: the probability that the median
        # round lies outside it is 2 * 0.5^n for n independent rounds. It is
        # extended to include the overall score (a ratio of medians, which can
        # fall just outside the paired rounds).
        "interval": [min(min(round_overall), overall), max(max(round_overall), overall)],
        "interval_confidence": 1.0 - 2.0 * 0.5 ** rounds,
    }


# ---------------------------------------------------------------------------
# Measurement
# ---------------------------------------------------------------------------

def timing_rounds(first, second, label, args, logs):
    """Alternating timing processes of two implementations.

    Every (round, workload) pair runs one process of each implementation
    back to back, so that the two measurements being compared are close in
    time; the order alternates from pair to pair. Returns (first_runs,
    second_runs, failure): runs map each workload to process results in
    round order; failure names the implementation whose process failed."""
    runs = {first["name"]: {w: [] for w in WORKLOADS}, second["name"]: {w: [] for w in WORKLOADS}}
    for round_index in range(args.processes):
        for workload_index, workload in enumerate(WORKLOADS):
            pair = (first, second) if (round_index + workload_index) % 2 == 0 else (second, first)
            for impl in pair:
                command = [impl["timing"], "--workloads", workload, "--warmup", str(args.warmup),
                           "--iterations", str(args.iterations), "--scale", repr(args.scale)]
                log_path = logs / f"{label}.r{round_index + 1}.{workload}.{impl['name']}.log"
                document, error = run_json(command, log_path, args.process_timeout, args.pin_cpu)
                if error:
                    return runs[first["name"]], runs[second["name"]], {
                        "implementation": impl["name"], "round": round_index + 1,
                        "workload": workload, "error": error, "log": f"logs/{log_path.name}"}
                runs[impl["name"]][workload].append(document["workloads"][0])
        log(f"  {label}: round {round_index + 1}/{args.processes} done")
    return runs[first["name"]], runs[second["name"]], None


def checksums_agree(runs_a, runs_b):
    def table(runs):
        return {(w, entry["checksum"]) for w in WORKLOADS for entry in runs[w]}
    return table(runs_a) == table(runs_b)


# ---------------------------------------------------------------------------
# Reports
# ---------------------------------------------------------------------------

def format_ms(ns):
    return f"{ns / 1e6:.3f} ms"


def conformance_summary(document):
    if document is None:
        return None
    categories = {}
    for check in document["checks"]:
        categories.setdefault(check["category"], True)
        categories[check["category"]] &= check["passed"]
    return categories


def write_reports(out, results):
    (out / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    main = results.get("measurement")
    aa = results.get("calibration")
    footprint = results.get("footprint", {})
    with open(out / "results.csv", "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["comparison", "workload", "baseline_median_ns", "candidate_median_ns",
                         "speed_percent", "round_min_percent", "round_max_percent",
                         "baseline_process_spread_pct", "candidate_process_spread_pct",
                         "baseline_capacity", "candidate_capacity",
                         "baseline_minimum_capacity", "candidate_minimum_capacity"])
        for name, comparison in (("calibration", aa), ("measurement", main)):
            if not comparison:
                continue
            for workload in WORKLOADS:
                item = comparison["workloads"][workload]
                writer.writerow([
                    name, workload, f"{item['reference_median_ns']:.0f}",
                    f"{item['other_median_ns']:.0f}", f"{item['speed_percent']:.2f}",
                    f"{item['round_min']:.2f}", f"{item['round_max']:.2f}",
                    f"{item['reference_variability']['process_spread_pct']:.2f}",
                    f"{item['other_variability']['process_spread_pct']:.2f}",
                    *([results.get("capacities", {}).get(side, {}).get(workload, "")
                       for side in ("baseline", "candidate")] +
                      [footprint.get(side, {}).get(workload, "")
                       for side in ("baseline", "candidate")]
                      if name == "measurement" else ["", "", "", ""])])
            writer.writerow([name, "OVERALL", "", "", f"{comparison['overall_percent']:.2f}",
                             f"{comparison['interval'][0]:.2f}", f"{comparison['interval'][1]:.2f}",
                             "", "", "", "", "", ""])

    lines = [f"# nanoalloc evaluation: {results['inputs']['candidate']['label']}", ""]
    verdict = results["verdict"]
    lines += [f"**Verdict: {verdict['result']}** - {verdict['reason']}", ""]
    if main:
        lines += [f"Overall performance: **{main['overall_percent']:.1f}%** of the buddy baseline "
                  f"(per-round range {main['interval'][0]:.1f}-{main['interval'][1]:.1f}%, "
                  f"{100 * main['interval_confidence']:.1f}% confidence interval of the median "
                  "round).", ""]
        lines += ["| Workload | Buddy time | Candidate time | Performance % | Round range % | "
                  "Buddy spread % | Candidate spread % | A/A % |",
                  "|---|---:|---:|---:|---:|---:|---:|---:|"]
        for workload in WORKLOADS:
            item = main["workloads"][workload]
            aa_item = aa["workloads"][workload] if aa else None
            lines.append(
                f"| {workload} | {format_ms(item['reference_median_ns'])} | "
                f"{format_ms(item['other_median_ns'])} | {item['speed_percent']:.1f} | "
                f"{item['round_min']:.1f}-{item['round_max']:.1f} | "
                f"{item['reference_variability']['process_spread_pct']:.1f} | "
                f"{item['other_variability']['process_spread_pct']:.1f} | "
                f"{aa_item['speed_percent']:.1f} |" if aa_item else "| |")
        lines.append(f"| OVERALL | | | **{main['overall_percent']:.1f}** | "
                     f"{main['interval'][0]:.1f}-{main['interval'][1]:.1f} | | | "
                     f"{aa['overall_percent']:.1f} |" if aa else "| OVERALL | | | | | | | |")
        lines.append("")
        lines += ["Times are medians over processes of each process's median replay time "
                  "(allocations and deallocations of the whole trace). Spread: (max - min) / "
                  "median of the per-process medians. A/A: the baseline against an identical "
                  "copy of itself, measured the same way.", ""]
    if aa:
        lines += ["## A/A calibration", "",
                  f"Overall {aa['overall_percent']:.2f}% (per-round range "
                  f"{aa['interval'][0]:.2f}-{aa['interval'][1]:.2f}%); bias "
                  f"{results['reliability']['aa_bias_pct']:.2f} points, limit "
                  f"{results['reliability']['max_aa_bias_pct']:.2f}; reliable: "
                  f"{results['reliability']['reliable']}.", ""]
    gate = results["correctness"]
    lines += ["## Correctness gate", "",
              f"Result: **{'passed' if gate['passed'] else 'FAILED'}**"
              + (f" - {'; '.join(gate['failures'])}" if gate["failures"] else ""), "",
              f"- Candidate builds with the evaluation driver: {gate['candidate_build']}",
              f"- Candidate's own tests: {gate['candidate_tests']['status']} "
              f"({gate['candidate_tests']['detail']})",
              f"- Packing requirement: {results['settings']['packing']}", ""]
    for who in ("candidate", "baseline"):
        document = results.get("conformance", {}).get(who)
        if not document:
            continue
        lines += [f"Conformance, {who}:", "", "| Check | Category | Result | Cases | Violations | "
                  "First violation |", "|---|---|---|---:|---:|---|"]
        for check in document["checks"]:
            example = check["examples"][0] if check["examples"] else ""
            lines.append(f"| {check['name']} | {check['category']} | "
                         f"{'pass' if check['passed'] else 'FAIL'} | {check['cases']} | "
                         f"{check['violations']} | {example} |")
        lines += ["", f"Largest payload slack: {document['max_payload_slack']} bytes", ""]
    consumption = results.get("conformance", {})
    if consumption.get("candidate") and consumption.get("baseline"):
        lines += ["## Actual consumption", "",
                  "Arena bytes per allocation when a 4 MiB pool is filled with one size until "
                  "it is full (capacity / number of allocations; independent of usable_size()). "
                  "Bookkeeping kept outside the arena is not included.", "",
                  "| Size | Buddy bytes/alloc | Candidate bytes/alloc |", "|---:|---:|---:|"]
        for base, cand in zip(consumption["baseline"]["consumption"],
                              consumption["candidate"]["consumption"]):
            lines.append(f"| {base['size']} | {base['bytes_per_allocation']:.1f} | "
                         f"{cand['bytes_per_allocation']:.1f} |")
        lines.append("")
    if footprint:
        lines += ["## Memory footprint per workload", "",
                  "Smallest pool capacity that completes the workload (bisection), and the "
                  "capacity used for timing (each implementation gets the capacity it needs).",
                  "", "| Workload | Peak live requested | Buddy minimum | Candidate minimum | "
                  "Buddy timing capacity | Candidate timing capacity |",
                  "|---|---:|---:|---:|---:|---:|"]
        peaks = results.get("peak_live_requested", {})
        for workload in WORKLOADS:
            def mib(value):
                return f"{value / 1048576:.2f} MiB" if isinstance(value, (int, float)) else "-"
            lines.append(
                f"| {workload} | {mib(peaks.get(workload))} | "
                f"{mib(footprint.get('baseline', {}).get(workload))} | "
                f"{mib(footprint.get('candidate', {}).get(workload))} | "
                f"{mib(results.get('capacities', {}).get('baseline', {}).get(workload))} | "
                f"{mib(results.get('capacities', {}).get('candidate', {}).get(workload))} |")
        lines.append("")
    env = results["environment"]
    builds = results.get("builds", {})
    lines += ["## Environment", "",
              f"- CPU: {env['cpu_model']} ({env['logical_cpus']} logical CPUs; affinity "
              f"{env['cpu_affinity']}; quota {env['cpu_quota']}; governor {env['cpu_governor']})",
              f"- OS: {env['os']}, kernel {env['kernel']}, {env['machine']}; memory {env['memory']}",
              f"- Container: {env['container']}",
              f"- Compiler: {env['compiler']}; CMake: {env['cmake']}; generator {env['generator']}",
              f"- Build type: {results['settings']['build_type']}; extra flags: "
              f"'{results['settings']['cxx_flags']}'",
              f"- Identical implementation compile flags in all builds: "
              f"{results.get('identical_build_flags')}"]
    if builds.get("baseline"):
        lines.append(f"- Implementation compile flags: "
                     f"{' '.join(builds['baseline']['implementation_compile_flags'] or [])}")
    settings = results["settings"]
    lines += [f"- Method: {settings['processes']} rounds; in each round every workload runs "
              f"in one process per implementation, back to back in alternating order, each with "
              f"{settings['warmup']} warmup + {settings['iterations']} measured replays; scale "
              f"{settings['scale']}"
              + ("" if settings["scale"] == 1.0 else " (NOT the standard scale)"),
              f"- Evaluator {EVALUATOR_VERSION}; baseline reference "
              f"{results['inputs']['baseline']['reference']}, verified: "
              f"{results['inputs']['baseline']['verified']}",
              f"- Candidate tree digest (include/, src/): "
              f"{results['inputs']['candidate']['digest']}", ""]
    (out / "report.md").write_text("\n".join(lines), encoding="utf-8")


def print_table(results):
    main = results.get("measurement")
    print()
    if main:
        print(f"{'Workload':<14} {'Buddy time':>14} {'Candidate time':>15} {'Performance %':>14}")
        for workload in WORKLOADS:
            item = main["workloads"][workload]
            print(f"{workload:<14} {format_ms(item['reference_median_ns']):>14} "
                  f"{format_ms(item['other_median_ns']):>15} {item['speed_percent']:>14.1f}")
        print(f"{'OVERALL':<14} {'':>14} {'':>15} {main['overall_percent']:>14.1f}")
        print(f"  per-round range {main['interval'][0]:.1f}-{main['interval'][1]:.1f}%; "
              f"A/A {results['calibration']['overall_percent']:.1f}%")
    verdict = results["verdict"]
    print(f"\nVERDICT: {verdict['result']} - {verdict['reason']}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Measure a nanoalloc implementation as a percentage of the buddy baseline.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument("--baseline", required=True, type=Path,
                        help="source tree of the frozen buddy baseline (no .git needed)")
    parser.add_argument("--candidate", required=True, type=Path,
                        help="source tree of the implementation under evaluation")
    parser.add_argument("--label", default=None, help="name of the candidate in reports")
    parser.add_argument("--out", type=Path, default=Path("nanoeval-results"),
                        help="output directory (reports, logs, work files)")
    parser.add_argument("--threshold", type=float, default=None,
                        help="minimum overall performance percentage for PASS (optional)")
    parser.add_argument("--processes", type=int, default=7,
                        help="rounds: timing processes per implementation and workload, for "
                             "the A/A calibration and for the measurement (>= 3)")
    parser.add_argument("--iterations", type=int, default=9,
                        help="measured replays per workload and process (>= 7)")
    parser.add_argument("--warmup", type=int, default=2,
                        help="unmeasured replays per workload and process (>= 1)")
    parser.add_argument("--scale", type=float, default=1.0,
                        help="workload scale; 1.0 is the standard (other values are reported "
                             "as non-standard)")
    parser.add_argument("--packing", choices=["enforce", "report"], default="enforce",
                        help="whether exact 16-byte rounding is part of the correctness gate")
    parser.add_argument("--candidate-tests", choices=["auto", "on", "off"], default="auto",
                        help="build the candidate with its own CMake project and run its tests "
                             "(auto: if it has any)")
    parser.add_argument("--max-aa-bias", type=float, default=3.0,
                        help="largest acceptable A/A deviation from 100%% (percentage points)")
    parser.add_argument("--max-aa-range", type=float, default=30.0,
                        help="largest acceptable width of the A/A per-round range (points)")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"), help="C++ compiler")
    parser.add_argument("--cmake", default="cmake", help="CMake executable")
    parser.add_argument("--generator", default="Ninja" if shutil.which("ninja") else "Unix Makefiles",
                        help="CMake generator")
    parser.add_argument("--build-type", default="Release", help="CMake build type")
    parser.add_argument("--cxx-flags", default="",
                        help="extra compiler flags, applied identically to every build")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2, help="build parallelism")
    parser.add_argument("--pin-cpu", type=int, default=None,
                        help="run timing processes on this CPU only (Linux)")
    parser.add_argument("--process-timeout", type=int, default=3600,
                        help="seconds allowed per timing or conformance process")
    parser.add_argument("--test-timeout", type=int, default=900,
                        help="seconds allowed per candidate test")
    parser.add_argument("--allow-unverified-baseline", action="store_true",
                        help="accept a baseline that differs from the frozen reference")
    args = parser.parse_args(argv)
    if args.processes < 3 or args.iterations < 7 or args.warmup < 1:
        parser.error("--processes must be >= 3, --iterations >= 7 and --warmup >= 1")
    if not (0 < args.scale <= 100):
        parser.error("--scale must be in (0, 100]")
    if args.threshold is not None and not (0 < args.threshold < 10000):
        parser.error("--threshold must be a positive percentage")
    return args


def evaluate(args):
    started = time.monotonic()
    out = args.out.resolve()
    work = out / "work"
    logs = out / "logs"
    for directory in (out, work, logs):
        directory.mkdir(parents=True, exist_ok=True)
    log.open(out / "evaluation.log")
    baseline_source = args.baseline.resolve()
    candidate_source = args.candidate.resolve()
    label = args.label or candidate_source.name
    log(f"nanoalloc evaluation {EVALUATOR_VERSION}: candidate '{label}'")
    for tool in (args.cmake, args.cxx):
        if shutil.which(tool) is None:
            raise EvaluationError(f"tool not found: {tool}")
    for source, what in ((baseline_source, "--baseline"), (candidate_source, "--candidate")):
        if not source.is_dir():
            raise EvaluationError(f"{what} is not a directory: {source}")

    results = {
        "evaluator_version": EVALUATOR_VERSION,
        "settings": {key: (str(value) if isinstance(value, Path) else value)
                     for key, value in vars(args).items()},
        "environment": environment(args),
    }
    log("verifying the baseline against the frozen reference")
    baseline_check = verify_baseline(baseline_source, args.allow_unverified_baseline)
    candidate_files = tree_files(candidate_source)
    results["inputs"] = {
        "baseline": dict(baseline_check, path=str(baseline_source)),
        "candidate": {"path": str(candidate_source), "label": label,
                      "digest": tree_digest(candidate_files), "files": len(candidate_files)},
    }

    # --- builds -----------------------------------------------------------------------
    log("staging sources and building (identical toolchain and flags)")
    trees = work / "trees"
    implementations = {}
    for name, source in (("baseline", baseline_source), ("baseline_copy", baseline_source),
                         ("candidate", candidate_source)):
        staged = trees / name
        try:
            stage_tree(source, staged, full=False)
        except (OSError, FileNotFoundError) as error:
            if name == "candidate":
                implementations[name] = {"name": name, "error": str(error)}
                continue
            raise EvaluationError(f"cannot stage the baseline: {error}")
        build = work / "build" / name
        ok, detail = configure_and_build(DRIVER_DIR, build, logs, name, args,
                                         ["-DNANOEVAL_IMPL_DIR=" + str(staged)])
        if not ok and name != "candidate":
            raise EvaluationError(f"the baseline does not build: {detail}")
        implementations[name] = {
            "name": name, "error": None if ok else detail,
            "timing": build / "nanoeval_timing", "conformance": build / "nanoeval_conformance",
            "build": build_info(build, staged) if ok else None,
        }
        log(f"  {name}: {'built' if ok else detail}")
    results["builds"] = {name: impl.get("build") for name, impl in implementations.items()}
    flag_sets = {json.dumps(impl["build"]["implementation_compile_flags"])
                 for impl in implementations.values() if impl.get("build")}
    results["identical_build_flags"] = len(flag_sets) == 1
    candidate = implementations["candidate"]

    # --- correctness gate -------------------------------------------------------------------
    failures = []
    gate = {"candidate_build": "ok" if not candidate.get("error") else candidate["error"]}
    if candidate.get("error"):
        failures.append("candidate does not build with the evaluation driver: " + candidate["error"])
    log("building the candidate with its own project and running its tests")
    try:
        stage_tree(candidate_source, trees / "candidate-own", full=True)
        gate["candidate_tests"] = candidate_self_tests(trees / "candidate-own", work, logs, args)
    except OSError as error:
        gate["candidate_tests"] = {"status": "failed", "detail": f"cannot copy the tree: {error}"}
    log(f"  candidate's own tests: {gate['candidate_tests']['status']} "
        f"({gate['candidate_tests']['detail']})")
    if gate["candidate_tests"]["status"] == "failed":
        failures.append("candidate's own tests: " + gate["candidate_tests"]["detail"])

    log("running the conformance driver")
    conformance = {}
    for name in ("baseline", "candidate"):
        impl = implementations[name]
        if impl.get("error"):
            continue
        log_path = logs / f"conformance.{name}.log"
        document, error = run_json([impl["conformance"]], log_path, args.process_timeout)
        if error or document is None:
            running = [line.split("running ", 1)[1] for line in
                       (read_text(log_path) or "").splitlines()
                       if line.startswith("nanoeval_conformance: running ")]
            if running:
                error += f" during check '{running[-1]}'"
            if name == "baseline":
                raise EvaluationError(f"conformance driver failed on the baseline: {error}")
            failures.append(f"conformance driver failed on the candidate: {error}")
            continue
        conformance[name] = document
        summary = conformance_summary(document)
        log(f"  {name}: " + ", ".join(f"{cat} {'pass' if ok else 'FAIL'}"
                                      for cat, ok in summary.items()))
    results["conformance"] = conformance
    if not conformance_summary(conformance["baseline"]).get("api", False):
        raise EvaluationError("the baseline fails the API conformance checks")
    packing_failed = False
    if "candidate" in conformance:
        summary = conformance_summary(conformance["candidate"])
        for category in GATED_CATEGORIES:
            if not summary.get(category, False):
                failures.append(f"conformance category '{category}' failed")
        if not summary.get("packing", False):
            packing_failed = True
            if args.packing == "enforce":
                failures.append("packing requirement (exact 16-byte rounding, at most 15 bytes "
                                "of slack) not met")
    gate["packing_met"] = not packing_failed
    # Only a failure of the packing requirement still allows a measurement.
    blocking = [failure for failure in failures if not failure.startswith("packing")]

    # --- measurement -------------------------------------------------------------------------
    calibration = measurement = None
    warnings = []
    reliability = {"reliable": False, "aa_bias_pct": None, "max_aa_bias_pct": args.max_aa_bias,
                   "aa_range_pct": None, "max_aa_range_pct": args.max_aa_range}
    if not blocking:
        log(f"A/A calibration: baseline against its copy, {args.processes} processes each")
        aa_base, aa_copy, failure = timing_rounds(implementations["baseline"],
                                                  implementations["baseline_copy"],
                                                  "calibration", args, logs)
        if failure:
            raise EvaluationError(f"baseline timing process failed: {failure}")
        calibration = compare(aa_base, aa_copy)
        log(f"  A/A overall {calibration['overall_percent']:.2f}% (rounds "
            f"{calibration['interval'][0]:.2f}-{calibration['interval'][1]:.2f}%)")
        log(f"measurement: baseline against candidate, {args.processes} processes each")
        base_runs, cand_runs, failure = timing_rounds(implementations["baseline"], candidate,
                                                      "measurement", args, logs)
        if failure:
            if failure["implementation"] != "candidate":
                raise EvaluationError(f"baseline timing process failed: {failure}")
            failures.append(f"candidate replay failed (round {failure['round']}): "
                            f"{failure['error']}")
            results["replay_failure"] = failure
        elif not checksums_agree(base_runs, cand_runs):
            failures.append("workload checksums differ between the drivers")
        else:
            measurement = compare(base_runs, cand_runs)
            log(f"  overall {measurement['overall_percent']:.2f}% (rounds "
                f"{measurement['interval'][0]:.2f}-{measurement['interval'][1]:.2f}%)")
            results["capacities"] = {
                "baseline": {w: base_runs[w][0]["capacity"] for w in WORKLOADS},
                "candidate": {w: cand_runs[w][0]["capacity"] for w in WORKLOADS}}
            results["peak_live_requested"] = {w: base_runs[w][0]["peak_live_requested"]
                                              for w in WORKLOADS}
            results["self_reported_stats"] = {
                "baseline": {w: base_runs[w][0]["self_reported"] for w in WORKLOADS},
                "candidate": {w: cand_runs[w][0]["self_reported"] for w in WORKLOADS}}
            def raw(runs):  # workload -> per process: measured iteration times (ns)
                return {w: [e["iterations_ns"] for e in runs[w]] for w in WORKLOADS}
            results["raw_timing_ns"] = {
                "calibration": {"baseline": raw(aa_base), "baseline_copy": raw(aa_copy)},
                "measurement": {"baseline": raw(base_runs), "candidate": raw(cand_runs)}}
            log("memory footprint (smallest capacity per workload)")
            results["footprint"] = {}
            for name in ("baseline", "candidate"):
                document, error = run_json(
                    [implementations[name]["timing"], "--footprint", "--warmup", "1",
                     "--iterations", "1", "--scale", repr(args.scale)],
                    logs / f"footprint.{name}.log", args.process_timeout)
                if error:
                    if name == "baseline":
                        raise EvaluationError(f"footprint run failed for the baseline: {error}")
                    failures.append(f"footprint run failed for the candidate: {error}")
                    continue
                results["footprint"][name] = {e["name"]: e["minimum_capacity"]
                                              for e in document["workloads"]}
                for entry in document["workloads"]:
                    if entry["at_lower_bound"]:
                        warnings.append(f"{name} completed {entry['name']} with a capacity equal "
                                        "to its peak live requested bytes (check that its "
                                        "payload comes from the pool)")
        aa_bias = abs(calibration["overall_percent"] - 100.0)
        aa_range = calibration["interval"][1] - calibration["interval"][0]
        reliability.update({
            "aa_bias_pct": aa_bias, "aa_range_pct": aa_range,
            "reliable": aa_bias <= args.max_aa_bias and aa_range <= args.max_aa_range})
    results["calibration"] = calibration
    results["measurement"] = measurement
    results["reliability"] = reliability
    results["warnings"] = warnings
    gate["passed"] = not failures
    gate["failures"] = failures
    results["correctness"] = gate

    # --- verdict -------------------------------------------------------------------------------
    decision = None
    if measurement:
        margin = (reliability["aa_bias_pct"] or 0.0) / 100.0
        decision = [measurement["interval"][0] * (1 - margin),
                    measurement["interval"][1] * (1 + margin)]
    if failures:
        verdict = {"result": "FAIL", "reason": "correctness gate: " + "; ".join(failures)}
    elif args.threshold is None:
        verdict = {"result": "MEASURED",
                   "reason": f"{measurement['overall_percent']:.1f}% of the buddy baseline; no "
                             "threshold given" + ("" if reliability["reliable"] else
                                                   " (A/A calibration outside its limits)")}
    elif not reliability["reliable"]:
        problems = []
        if reliability["aa_bias_pct"] > args.max_aa_bias:
            problems.append(f"bias {reliability['aa_bias_pct']:.2f} > {args.max_aa_bias} points")
        if reliability["aa_range_pct"] > args.max_aa_range:
            problems.append(f"per-round range {reliability['aa_range_pct']:.2f} > "
                            f"{args.max_aa_range} points")
        verdict = {"result": "INCONCLUSIVE",
                   "reason": "measurement noise too high: A/A calibration " + ", ".join(problems)}
    elif decision[0] >= args.threshold:
        verdict = {"result": "PASS",
                   "reason": f"{measurement['overall_percent']:.1f}% >= {args.threshold}% "
                             f"(decision interval {decision[0]:.1f}-{decision[1]:.1f}%)"}
    elif decision[1] < args.threshold:
        verdict = {"result": "FAIL",
                   "reason": f"{measurement['overall_percent']:.1f}% < {args.threshold}% "
                             f"(decision interval {decision[0]:.1f}-{decision[1]:.1f}%)"}
    else:
        verdict = {"result": "INCONCLUSIVE",
                   "reason": f"{measurement['overall_percent']:.1f}%, but the decision interval "
                             f"{decision[0]:.1f}-{decision[1]:.1f}% contains the threshold "
                             f"{args.threshold}%"}
    verdict["threshold"] = args.threshold
    verdict["decision_interval"] = decision
    results["verdict"] = verdict
    results["elapsed_seconds"] = round(time.monotonic() - started, 1)
    write_reports(out, results)
    print_table(results)
    log(f"reports: {out / 'report.md'}, {out / 'results.json'}, {out / 'results.csv'}")
    return {"PASS": EXIT_PASS, "MEASURED": EXIT_PASS, "FAIL": EXIT_FAIL,
            "INCONCLUSIVE": EXIT_INCONCLUSIVE}[verdict["result"]]


def main(argv=None):
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        return evaluate(args)
    except EvaluationError as error:
        log(f"EVALUATION ERROR: {error}")
        return EXIT_ERROR


if __name__ == "__main__":
    sys.exit(main())
