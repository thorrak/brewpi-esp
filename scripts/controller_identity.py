"""Identify the exact controller sources used by firmware and offline replay.

The simulator uses the same five paths and byte-level recipe. Changing any of
these sources requires a matching simulator bundle before learning comparisons
can run; a Git revision alone does not identify locally modified firmware.
"""

import hashlib
from pathlib import Path


CONTROLLER_SOURCES = (
    "src/AdaptiveDoseController.cpp",
    "src/AdaptiveDoseController.h",
    "src/GlycolCoolingMeasurements.h",
    "src/PredictiveCoastController.cpp",
    "src/PredictiveCoastController.h",
)


def controller_identity(root=None):
    """Return sha256:<hex> over sorted path\0content\0 pairs, without fallback."""
    root = Path(root) if root is not None else Path(__file__).resolve().parents[1]
    digest = hashlib.sha256()
    for relative_path in sorted(CONTROLLER_SOURCES):
        digest.update(relative_path.encode("utf-8"))
        digest.update(b"\0")
        digest.update((root / relative_path).read_bytes())
        digest.update(b"\0")
    return "sha256:" + digest.hexdigest()


if __name__ == "__main__":
    print(controller_identity())
