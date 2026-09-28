#!/usr/bin/env python3
"""Exercise real Chill Test controller allocation failures and memory release."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from controller_identity import controller_identity


def main():
    headers = next((path for path in (ROOT / ".pio/libdeps").glob("*/ArduinoJson/src")
                    if (path / "ArduinoJson.h").is_file()), None)
    if headers is None:
        raise SystemExit("Build a firmware target first to install ArduinoJson.")
    allowed = {f'#include "{name}"' for name in (
        "WaterTest.h", "WaterTestCore.h", "WaterTestProtocol.h", "WaterTestStorage.h",
        "WaterTestTransport.h", "WaterTestControllerSnapshot.h")}
    source = "\n".join(
        line for name in ("WaterTestStorage.cpp", "WaterTestTransport.cpp", "WaterTest.cpp")
        for line in (ROOT / "src" / name).read_text().splitlines()
        if not line.startswith("#include") or line in allowed)
    allocation = "std::malloc(sizeof(GlycolCooling::Controller))"
    assert source.count(allocation) == 1
    # Inject failure only at the controller allocator boundary; the actual
    # constructor, null handling, lifecycle, and persistence code remain intact.
    source = source.replace(allocation, "ControllerMemoryTest::allocate(sizeof(GlycolCooling::Controller))")
    # Reuse the shared hardware/survey fixture without changing its test driver.
    helpers = (ROOT / "tests/water_test_backend/test.inc.cpp").read_text().split("int main(")[0]
    with tempfile.TemporaryDirectory(prefix="brewpi-controller-memory-") as temporary:
        build = Path(temporary)
        cpp = build / "controller_memory.cpp"
        allocation_hook = '''
namespace ControllerMemoryTest {
bool failAllocation = false;
unsigned attempts = 0;
void *allocate(std::size_t size) {
  ++attempts;
  return failAllocation ? nullptr : std::malloc(size);
}
}
'''
        cpp.write_text('#include "NativeEnvironment.h"\n' + allocation_hook + source + "\n" + helpers + "\n" +
                       (HERE / "test.inc.cpp").read_text())
        binary = build / "test"
        subprocess.run([
            os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-I" + str(ROOT / "src"), "-I" + str(ROOT / "tests/water_test_backend"),
            "-I" + str(headers),
            "-DCOOLING_IMPLEMENTATION_ID=" + json.dumps(controller_identity(ROOT)), str(cpp),
            str(ROOT / "src/GlycolCoolingController.cpp"),
            str(ROOT / "src/PredictiveCoastController.cpp"),
            str(ROOT / "src/AdaptiveDoseController.cpp"), "-o", str(binary),
        ], check=True)
        for scenario in ("snapshot_failure", "held_retry_failure", "phase_failure", "release_predictive", "release_dose"):
            subprocess.run([str(binary), scenario, str(build / scenario)], check=True)
    print("Controller memory regressions passed.")


if __name__ == "__main__":
    main()
