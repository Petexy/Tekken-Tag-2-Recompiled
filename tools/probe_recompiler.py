#!/usr/bin/env python3
"""Run a bounded generation probe and summarize the first failure per block."""

import argparse
from collections import Counter
import json
from pathlib import Path
import re
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rpx", type=Path, default=ROOT / "local/wua/000500001010f800_v16/code/Tekken.rpx")
    parser.add_argument("--config", type=Path, default=ROOT / "configs/ttt2-eu-v16.toml")
    parser.add_argument("--recompiler", type=Path, default=ROOT / "build/nwiiu/nWiiURecomp/nwiiu-recompile")
    parser.add_argument("--output", type=Path, default=ROOT / "local/generated/nwiiu")
    parser.add_argument("--report-prefix", type=Path, default=ROOT / "analysis/latest-recompile")
    parser.add_argument("--timeout", type=float, default=120)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("timeout must be positive")
    command = [str(args.recompiler.resolve()), "--config", str(args.config.resolve()),
               str(args.rpx.resolve()), str(args.output.resolve())]
    args.report_prefix.parent.mkdir(parents=True, exist_ok=True)
    logfile = args.report_prefix.with_suffix(".log")
    start = time.monotonic()
    timed_out = False
    with logfile.open("w") as log:
        try:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout)
            code = result.returncode
        except subprocess.TimeoutExpired:
            timed_out, code = True, 124
        except OSError as error:
            parser.exit(1, f"Could not run recompiler: {error}\n")
    failures = re.findall(r"instruction (0x[0-9A-Fa-f]+) \((0x[0-9A-Fa-f]+)\): (.+)", logfile.read_text())
    report = dict(command=command, returncode=code, timed_out=timed_out,
                  seconds=round(time.monotonic() - start, 3),
                  unsupported_block_count=len(failures),
                  first_failures_by_primary_opcode=dict(sorted(Counter(str(int(word, 16) >> 26) for _, word, _ in failures).items())),
                  project_generation_succeeded=code == 0,
                  meaning="First rejection per recovered block. Generation success alone does not prove a buildable or working native port.")
    args.report_prefix.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    raise SystemExit(0 if code == 0 else 1)


if __name__ == "__main__":
    main()
