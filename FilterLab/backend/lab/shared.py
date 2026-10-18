"""
The team's existing Python code, as the lab uses it.

Every block that converts, filters or estimates something calls the same code
the other tools use - SD-Parser's frameparser / pilog_reader / kalman /
trajectory / noise_characterization / imu_calibration and the Dashboard's
attitude.py - so a recipe reproduces exactly what the Dashboard and the
command-line tools compute.

The lab carries copies of them in backend/shared/, which keeps the FilterLab
folder self-contained: scp it to a server and run it. FilterLab/sync_shared.py
refreshes the copies from a repository checkout. PILOG_SHARED_PATH
(os.pathsep-separated) points somewhere else instead, e.g. at the originals
while developing them.
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_OWN = os.path.abspath(os.path.join(_HERE, "..", "shared"))

for _p in (os.environ.get("PILOG_SHARED_PATH") or _OWN).split(os.pathsep):
    # Appended, so a lab module can never be shadowed by one of these.
    if _p and os.path.isdir(_p) and _p not in sys.path:
        sys.path.append(_p)

import attitude  # noqa: E402,F401
import frameparser  # noqa: E402,F401
import imu_calibration  # noqa: E402,F401
import kalman  # noqa: E402,F401
import noise_characterization  # noqa: E402,F401
import pilog_reader  # noqa: E402,F401
import trajectory  # noqa: E402,F401
