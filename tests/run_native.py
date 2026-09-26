#!/usr/bin/env python3
"""Run the glycol and water-test host regressions without hardware or a network.

Build one firmware target first to install ArduinoJson. The full portal contract
and saved Chillsim trace replay remain separate cross-repository checks.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(*args):
    subprocess.run([str(arg) for arg in args], cwd=ROOT, check=True)


def main():
    json_headers = next((path for path in (ROOT / ".pio/libdeps").glob("*/ArduinoJson/src")
                         if (path / "ArduinoJson.h").is_file()), None)
    if json_headers is None:
        sys.exit("ArduinoJson headers missing; build a firmware target first.")
    controller_sources = ["src/AdaptiveDoseController.cpp", "src/PredictiveCoastController.cpp",
                          "src/GlycolCoolingController.cpp"]
    suites = {
        "adaptive_dose_core": ["src/AdaptiveDoseController.cpp", "tests/adaptive_dose_core/core_test.cpp"],
        "predictive_coast_core": ["src/PredictiveCoastController.cpp", "tests/predictive_coast_core/core_test.cpp"],
        "glycol_cooling_selector": controller_sources + ["tests/glycol_cooling_selector/core_test.cpp"],
        "water_test_core": ["tests/water_test_core/test.cpp"],
        "water_test_protocol": ["tests/water_test_protocol/test.cpp"],
    }
    with tempfile.TemporaryDirectory(prefix="brewpi-native-") as temp:
        for name, sources in suites.items():
            print(f"Running {name}", flush=True)
            binary = Path(temp) / name
            run(os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-Wall", "-Wextra",
                "-Werror", "-pedantic", "-fno-fast-math", "-ffp-contract=off", "-Isrc",
                "-I" + str(json_headers), *sources, "-o", binary)
            run(binary)
    for name in ["cooling_selector_settings", "predictive_coast_integration",
                 "water_test_backend", "static_file_serving"]:
        print(f"Running {name}", flush=True)
        run(sys.executable, ROOT / "tests" / name / "run.py")
    run(sys.executable, ROOT / "tests/water_test_contract/run.py", "--firmware-only")
    print("All native regressions passed.")


if __name__ == "__main__":
    main()
