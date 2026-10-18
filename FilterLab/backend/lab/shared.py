"""
The team's existing Python code, made importable for the lab.

Every block that converts, filters or estimates something calls the same code
the other tools use - SD-Parser's frameparser / pilog_reader / kalman /
trajectory / noise_characterization / imu_calibration and the Dashboard's
attitude.py - so a recipe reproduces exactly what the Dashboard and the
command-line tools compute.

In the repository the modules are found next to this project; the Docker image
copies them to /app/shared. PILOG_SHARED_PATH (os.pathsep-separated) overrides
both. They are APPENDED to sys.path, so a lab module can never be shadowed.
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.abspath(os.path.join(_HERE, "..", "..", ".."))


def _paths():
    env = os.environ.get("PILOG_SHARED_PATH")
    if env:
        return [p for p in env.split(os.pathsep) if p]
    return [os.path.join(_REPO, "SD-Parser"),
            os.path.join(_REPO, "Dashboard", "Backend"),
            "/app/shared"]


for _p in _paths():
    if os.path.isdir(_p) and _p not in sys.path:
        sys.path.append(_p)

import attitude  # noqa: E402,F401  (Dashboard/Backend)
import frameparser  # noqa: E402,F401
import imu_calibration  # noqa: E402,F401
import kalman  # noqa: E402,F401
import noise_characterization  # noqa: E402,F401
import pilog_reader  # noqa: E402,F401
import trajectory  # noqa: E402,F401
