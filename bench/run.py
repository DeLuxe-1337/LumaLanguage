#!/usr/bin/env python3
"""Builds every bench/*.luma with the given luma flags and reports the best
wall-clock time of N runs, plus the program's output (which must not change
between optimization levels).

usage: bench/run.py [--runs N] [--compare] [luma flags...]
  --compare   build each program at -O0 and at the default level and print both
"""
import glob
import os
import subprocess
import sys
import time

LUMA = os.environ.get("LUMA", "build/luma")
OUT = "build/bench"


def build(src, tag, flags):
    exe = os.path.join(OUT, "%s%s" % (os.path.basename(src)[:-5], tag))
    subprocess.run([LUMA, src, "-o", exe] + flags, check=True)
    return exe


def best_time(exe, runs):
    best, out = None, None
    for _ in range(runs):
        t0 = time.perf_counter()
        r = subprocess.run([exe], capture_output=True, check=True)
        dt = time.perf_counter() - t0
        best = dt if best is None or dt < best else best
        out = r.stdout.decode().strip().replace("\n", " ")
    return best, out


def main():
    args = sys.argv[1:]
    runs, compare = 3, False
    while args and args[0].startswith("--"):
        if args[0] == "--runs":
            runs = int(args[1])
            args = args[2:]
        elif args[0] == "--compare":
            compare = True
            args = args[1:]
        else:
            break
    os.makedirs(OUT, exist_ok=True)
    srcs = sorted(glob.glob("bench/*.luma"))
    if compare:
        print("%-11s %9s %9s %8s   %s" % ("benchmark", "-O0", "default", "speedup", "output"))
        for src in srcs:
            t0, o0 = best_time(build(src, ".O0", ["-O0"] + args), runs)
            t2, o2 = best_time(build(src, ".opt", args), runs)
            if o0 != o2:
                print("%s: OUTPUT DIFFERS: %r vs %r" % (src, o0, o2))
                sys.exit(1)
            print("%-11s %8.3fs %8.3fs %7.1fx   %s" % (os.path.basename(src)[:-5], t0, t2, t0 / t2, o2[:40]))
    else:
        for src in srcs:
            t, o = best_time(build(src, "", args), runs)
            print("%-11s %8.3fs   %s" % (os.path.basename(src)[:-5], t, o[:40]))


if __name__ == "__main__":
    main()
