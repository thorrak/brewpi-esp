#!/usr/bin/env python3
"""Test production glycol tuning persistence against a temporary host filesystem."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arduinojson", type=Path)
    args = parser.parse_args()
    candidates = ([args.arduinojson] if args.arduinojson else
                  (ROOT / ".pio/libdeps").glob("*/ArduinoJson/src"))
    json_headers = next((path for path in candidates
                         if (path / "ArduinoJson.h").is_file()), None)
    if json_headers is None:
        parser.error("Build firmware first, or supply --arduinojson PATH")

    with tempfile.TemporaryDirectory(prefix="brewpi-glycol-tuning-") as temporary:
        build = Path(temporary)
        sources = ["GlycolTuning.cpp", "GlycolCoolingController.cpp",
                   "AdaptiveDoseController.cpp", "PredictiveCoastController.cpp"]
        headers = [name.replace(".cpp", ".h") for name in sources]
        headers += ["GlycolCoolingAlgorithm.h", "GlycolCoolingMeasurements.h"]
        for name in sources + headers:
            shutil.copyfile(ROOT / "src" / name, build / name)
        shutil.copyfile(HERE / "shim/ESPEepromAccess.h", build / "ESPEepromAccess.h")
        shutil.copyfile(HERE / "test.cpp", build / "test.cpp")
        executable = build / "glycol-tuning-test"
        subprocess.run(
            [os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-Wall", "-Wextra",
             "-Werror", "-pedantic", "-fno-fast-math", "-ffp-contract=off",
             "-I", str(build), "-I", str(json_headers),
             *[str(build / name) for name in sources], str(build / "test.cpp"),
             "-o", str(executable)], check=True,
        )
        subprocess.run([str(executable)], cwd=build, check=True)


if __name__ == "__main__":
    main()
