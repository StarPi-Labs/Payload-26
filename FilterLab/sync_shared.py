#!/usr/bin/env python3
"""
Copy the team's shared Python modules into FilterLab/backend/shared/.

The Filter Lab runs the same code as the other tools (SD-Parser's parser,
Kalman filter and trajectory pipeline, the Dashboard's attitude estimator). It
keeps its own copies so the FilterLab folder is self-contained: scp it to a
server and start it, no repository needed. Run this from a checkout after
changing any of the originals, and commit the refreshed copies with them.

    python FilterLab/sync_shared.py           copy the originals over
    python FilterLab/sync_shared.py --check   only report copies that are out of date
"""
import argparse
import filecmp
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
TARGET = HERE / "backend" / "shared"

ORIGINALS = {
    "frameparser.py": "SD-Parser",
    "pilog_reader.py": "SD-Parser",
    "kalman.py": "SD-Parser",
    "trajectory.py": "SD-Parser",
    "noise_characterization.py": "SD-Parser",
    "imu_calibration.py": "SD-Parser",
    "attitude.py": "Dashboard/Backend",
}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="report differences, change nothing")
    args = ap.parse_args()
    stale = 0
    for name, folder in ORIGINALS.items():
        src = REPO / folder / name
        dst = TARGET / name
        if not src.is_file():
            print(f"[!] {src} not found - run this from a repository checkout")
            return 2
        same = dst.is_file() and filecmp.cmp(src, dst, shallow=False)
        if same:
            print(f"    {name:28s} up to date")
            continue
        stale += 1
        if args.check:
            print(f"[!] {name:28s} differs from {folder}/{name}")
        else:
            TARGET.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
            print(f"[+] {name:28s} copied from {folder}/")
    if args.check and stale:
        print(f"{stale} copy(ies) out of date: run python FilterLab/sync_shared.py")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
