#!/usr/bin/env python3
"""Exercise actual selector persistence, HTTP, and Telnet settings methods.

Only platform I/O is replaced. Declarations and methods are extracted from the
current sources and compiled against the project's real ArduinoJson library.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--arduinojson", type=Path)
args = parser.parse_args()
json_paths = (
    [args.arduinojson]
    if args.arduinojson
    else list((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src"))
)
json_path = next((p for p in json_paths if (p / "ArduinoJson.h").is_file()), None)
if json_path is None:
    parser.error("Build firmware first, or supply --arduinojson PATH")


def definition(source, signature):
    start = source.index(signature)
    end = source.index("\n}", source.index("{", start)) + 2
    return source[start:end] + "\n"


with tempfile.TemporaryDirectory(prefix="brewpi-selector-settings-") as temp:
    build = Path(temp)
    for name in ["EepromStructs.h", "TemperatureFormats.h", "CoolingAlgorithm.h", "JsonKeys.h"]:
        shutil.copyfile(ROOT / "src" / name, build / name)
    control_header = (ROOT / "src/TempControl.h").read_text()
    modes_end = control_header.index("\n};", control_header.index("namespace Modes")) + 3
    (build / "MinTimesTypes.h").write_text(
        control_header[control_header.index("enum MinTimesSettingsChoice"):modes_end]
    )
    constants = (ROOT / "src/EepromStructs.cpp").read_text()
    control = (ROOT / "src/TempControl.cpp").read_text()
    http = (ROOT / "src/http_server.cpp").read_text()
    commands = (ROOT / "src/CommandProcessor.cpp").read_text()
    source = '#include "shim.h"\n'
    for signature in [
        "void JSONSaveable::writeJsonToFile",
        "ArduinoJson::JsonDocument JSONSaveable::readJsonFromFile",
        "ExtendedSettings::ExtendedSettings()",
        "void ExtendedSettings::setDefaults()",
        "void ExtendedSettings::toJson",
        "void ExtendedSettings::storeToFilesystem()",
        "void ExtendedSettings::loadFromFilesystem()",
        "bool ExtendedSettings::validateSettingsJson",
        "void ExtendedSettings::processSettingKeypair",
        "void ExtendedSettings::setGlycol(",
        "bool ExtendedSettings::setGlycolCoolingAlgorithm",
        "void ExtendedSettings::setLargeTFT",
        "void ExtendedSettings::setInvertTFT",
        "void ExtendedSettings::setResetScreenOnPin",
    ]:
        source += definition(constants, signature)
    for signature in [
        "MinTimes::MinTimes()",
        "void MinTimes::setDefaults()",
        "void MinTimes::storeToFilesystem()",
        "void MinTimes::toJson",
    ]:
        source += definition(control, signature)
    source += definition(http, "bool processExtendedSettingsJson(")
    source += definition(http, "void serveExtendedSettings(")
    source += definition(commands, "void CommandProcessor::processExtendedSettingsJson()")
    source += definition(commands, "void CommandProcessor::sendExtendedSettings()")
    (build / "settings.cpp").write_text(source)
    for name in ["shim.h", "settings_test.cpp"]:
        shutil.copyfile(HERE / name, build / name)
    executable = build / "settings-test"
    subprocess.run(
        [os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-Wall", "-Wextra",
         "-Wno-unused-parameter", "-I", str(build), "-I", str(json_path),
         str(build / "settings.cpp"), str(build / "settings_test.cpp"),
         "-o", str(executable)],
        check=True,
    )
    subprocess.run([str(executable), str(build)], check=True)
