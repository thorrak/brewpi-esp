#!/usr/bin/env python3
"""Compile the actual integration/core/filter sources with deterministic I/O.

The shims only supply Arduino I/O/JSON and the enclosing TempControl facade.
Declarations and constructors/default/reset bodies are extracted from the branch
being tested, not frozen test copies. The real board build covers Arduino links.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def body(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


with tempfile.TemporaryDirectory(prefix='brewpi-integration-') as temp:
    build = Path(temp)
    for file in (HERE / 'shim').iterdir():
        shutil.copyfile(file, build / file.name)
    names = ['AdaptiveDoseController', 'GlycolMode', 'GlycolParams', 'TempSensor',
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
    definitions = '#include "TempControl.h"\n'
    for signature in ['ControlConstants::ControlConstants()', 'void ControlConstants::setDefaults()',
                      'ControlSettings::ControlSettings()', 'void ControlSettings::setDefaults()']:
        definitions += body(constants, signature)
    for signature in ['MinTimes::MinTimes()', 'void MinTimes::setDefaults()', 'void GlycolRuntimeState::reset()']:
        definitions += body(control, signature)
    (build / 'Defaults.cpp').write_text(definitions)
    shutil.copyfile(HERE / 'integration.cpp', build / 'integration.cpp')
    compiler = os.environ.get('CXX', 'c++')
    for cooling_only in [False, True]:
        executable = build / ('test-cooling-only' if cooling_only else 'test-normal')
        command = [compiler, '-std=c++17', '-O2', '-fwrapv', '-fno-fast-math',
                   '-ffp-contract=off', '-I', str(build)]
        if cooling_only:
            command.append('-DBREWPI_CHILLSIM_TEST')
        command += [str(p) for p in build.glob('*.cpp')] + ['-o', str(executable)]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
