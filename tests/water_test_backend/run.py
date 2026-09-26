#!/usr/bin/env python3
"""Execute the production WaterTest.cpp against a deterministic ESP/RTOS facade.

Only platform include directives are replaced; every lifecycle, journal, recovery,
HTTP request/acknowledgement, and scheduling function comes from the real source.
Run from any directory after PlatformIO has installed ArduinoJson.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
headers = next((path for path in (root / '.pio/libdeps').glob('*/ArduinoJson/src')
                if (path / 'ArduinoJson.h').is_file()), None)
if headers is None:
    raise SystemExit('Build a firmware target first to install ArduinoJson.')
allowed = {'#include "WaterTest.h"', '#include "WaterTestCore.h"', '#include "WaterTestProtocol.h"',
           '#include "WaterTestStorage.h"', '#include "WaterTestTransport.h"'}
source = '\n'.join(line for name in ('WaterTestStorage.cpp', 'WaterTestTransport.cpp', 'WaterTest.cpp')
                   for line in (root / 'src' / name).read_text().splitlines()
                   if not line.startswith('#include') or line in allowed)
with tempfile.TemporaryDirectory(prefix='water-test-backend-') as temp:
    build = Path(temp)
    translation = build / 'backend.cpp'
    translation.write_text('#include "NativeEnvironment.h"\n' + source + '\n' +
                           (root / 'tests/water_test_backend/test.inc.cpp').read_text())
    binary = build / 'test'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'src'), '-I' + str(root / 'tests/water_test_backend'),
                    '-I' + str(headers),
                    str(translation), '-o', str(binary)], check=True)
    for name in ['normal_stop', 'minimum_stop', 'sensor_fault', 'bath_fault', 'status_freshness',
                 'fsync_failure', 'edge_fsync_failure', 'start_failure', 'queue_overflow', 'unexpected_output',
                 'upload_retry', 'all_pulses', 'slow_sensor_fault', 'slow_temperature_limit', 'slow_deadline', 'slow_on_edge',
                 'slow_phase', 'slow_storage_repair',
                 'queue_snapshot', 'slow_queue_overflow', 'queued_unexpected_output', 'cleanup_ack_failure',
                 'cleanup_resume_failure', 'cleanup_manifest_failure']:
        subprocess.run([str(binary), name, str(build / name)], check=True)
    recovery = build / 'reboot'
    subprocess.run([str(binary), 'active_before_reboot', str(recovery)], check=True)
    subprocess.run([str(binary), 'after_reboot', str(recovery)], check=True)

    upload_recovery = build / 'upload-reboot'
    subprocess.run([str(binary), 'partial_upload_before_reboot', str(upload_recovery)], check=True)
    subprocess.run([str(binary), 'after_partial_upload_reboot', str(upload_recovery)], check=True)

    for before, after in [('foreign_markers', 'foreign_markers'), ('foreign_finish', 'foreign_finish'),
                          ('foreign_boots', 'invalid_boots'), ('untagged_boots', 'invalid_boots'),
                          ('submitted', 'submitted'), ('legacy_submitted', 'legacy_submitted'),
                          ('legacy_active', 'legacy_active'), ('legacy_pending', 'legacy_pending'),
                          ('cleanup_ack', 'cleanup'), ('cleanup_resume', 'cleanup'), ('cleanup_manifest', 'cleanup')]:
        recovery = build / before
        subprocess.run([str(binary), before + '_before_reboot', str(recovery)], check=True)
        subprocess.run([str(binary), after + '_after_reboot', str(recovery)], check=True)
