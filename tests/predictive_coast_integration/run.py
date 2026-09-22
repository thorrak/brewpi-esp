#!/usr/bin/env python3
"""Compile the actual integration/core/filter sources with deterministic I/O.

The shims only supply Arduino I/O and the enclosing TempControl facade.
Declarations, defaults, reset and JSON diagnostics are extracted from the branch
being tested, not frozen test copies. ArduinoJson is the project's real library;
run a PlatformIO build first, or pass its src directory with --arduinojson.
The real board build covers Arduino links.
"""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--arduinojson', type=Path)
args = parser.parse_args()
json_paths = ([args.arduinojson] if args.arduinojson else
              list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src')))
json_path = next((path for path in json_paths if (path / 'ArduinoJson.h').is_file()), None)
if json_path is None:
    parser.error('ArduinoJson headers missing; build a firmware target or use --arduinojson PATH')


def body(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    # These definitions close at column zero. Counting raw braces would treat
    # mutually exclusive #ifdef/#else opening braces as two simultaneous scopes.
    end = source.index('\n}', opening) + 2
    return source[start:end] + '\n'


with tempfile.TemporaryDirectory(prefix='brewpi-integration-') as temp:
    build = Path(temp)
    for file in (HERE / 'shim').iterdir():
        shutil.copyfile(file, build / file.name)
    names = ['PredictiveCoastController', 'GlycolMode', 'GlycolParams', 'TempSensor',
             'FilterFixed', 'FilterCascaded', 'TemperatureFormats']
    for name in names:
        for suffix in ['.cpp', '.h']:
            shutil.copyfile(ROOT / 'src' / (name + suffix), build / (name + suffix))
    for name in ['ControlContext.h', 'TempSensorBasic.h', 'Actuator.h', 'GlycolLog.h', 'JsonKeys.h']:
        shutil.copyfile(ROOT / 'src' / name, build / name)
    header = (ROOT / 'src/TempControl.h').read_text()
    (build / 'ControlTypes.h').write_text('#pragma once\n' + header[
        header.index('enum GlycolState'):header.index('#define TC_STATE_MASK')])
    constants = (ROOT / 'src/EepromStructs.cpp').read_text()
    control = (ROOT / 'src/TempControl.cpp').read_text()
    definitions = '#include "TempControl.h"\n#include "Ticks.h"\n#include <cmath>\n'
    for signature in ['ControlConstants::ControlConstants()', 'void ControlConstants::setDefaults()',
                      'ControlSettings::ControlSettings()', 'void ControlSettings::setDefaults()']:
        definitions += body(constants, signature)
    for signature in ['MinTimes::MinTimes()', 'void MinTimes::setDefaults()', 'void GlycolRuntimeState::reset()']:
        definitions += body(control, signature)
    for signature in ['void TempControl::getControlVariablesDoc(JsonDocument& doc)',
                      'void TempControl::getControlConstantsDoc(JsonDocument& doc)']:
        definitions += body(control, signature)
    (build / 'Defaults.cpp').write_text(definitions)
    shutil.copyfile(HERE / 'integration.cpp', build / 'integration.cpp')
    compiler = os.environ.get('CXX', 'c++')
    for cooling_only in [False, True]:
        executable = build / ('test-cooling-only' if cooling_only else 'test-normal')
        command = [compiler, '-std=c++17', '-O2', '-fwrapv', '-fno-fast-math',
                   '-ffp-contract=off', '-I', str(build), '-I', str(json_path)]
        if cooling_only:
            command.append('-DBREWPI_CHILLSIM_TEST')
        command += [str(p) for p in build.glob('*.cpp')] + ['-o', str(executable)]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
