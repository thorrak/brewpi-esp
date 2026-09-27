#!/usr/bin/env python3
"""Execute the production WaterTest.cpp against a deterministic ESP/RTOS facade.

Only platform include directives are replaced; every lifecycle, journal, reboot,
HTTP request/acknowledgement, and scheduling function comes from the real source.
Run from any directory after PlatformIO has installed ArduinoJson.
"""
from pathlib import Path
import subprocess
import json
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'scripts'))
from controller_identity import controller_identity
implementation_id = controller_identity(root)
headers = next((path for path in (root / '.pio/libdeps').glob('*/ArduinoJson/src')
                if (path / 'ArduinoJson.h').is_file()), None)
if headers is None:
    raise SystemExit('Build a firmware target first to install ArduinoJson.')
allowed = {'#include "WaterTest.h"', '#include "WaterTestCore.h"', '#include "WaterTestProtocol.h"',
           '#include "WaterTestStorage.h"', '#include "WaterTestTransport.h"',
           '#include "WaterTestControllerSnapshot.h"'}
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
                    '-I' + str(headers), '-DCOOLING_IMPLEMENTATION_ID=' + json.dumps(implementation_id),
                    str(translation), str(root / 'src/GlycolCoolingController.cpp'),
                    str(root / 'src/PredictiveCoastController.cpp'), str(root / 'src/AdaptiveDoseController.cpp'),
                    '-o', str(binary)], check=True)
    for name in ['flow_validation', 'flow_retention', 'normal_stop', 'baseline_stop', 'minimum_stop', 'full_pulse_stop',
                 'transient_sensor_errors', 'transient_bad_start', 'prolonged_invalid', 'silent_sensor',
                 'minimum_water_limit', 'bath_fault', 'status_freshness',
                 'fsync_failure', 'edge_fsync_failure', 'start_failure', 'queue_overflow', 'unexpected_output',
                 'upload_retry', 'upload_allocation_failure', 'upload_pacing',
                 'upload_pacing_http_retry', 'upload_pacing_ack_retry', 'upload_pacing_preparation_retry',
                 'all_pulses', 'completed',
                 'failure_after_full_pulse', 'stale_stop_after_full_pulse',
                 'slow_sensor_fault', 'slow_temperature_limit', 'slow_deadline', 'slow_on_edge',
                 'slow_phase', 'slow_storage_repair',
                 'queue_snapshot', 'slow_queue_overflow', 'queued_unexpected_output', 'cleanup_failure',
                 'resume_pending_upload', 'independent_hardware_availability', 'configured_glycol_probe',
                 'offline_12h_budget', 'finish_persistence_failure', 'receipt_persistence_failure',
                 'reserve_persistence_failure', 'controller_interrupted_status', 'controller_continuous_pulse_limit',
                 'controller_fast_loop_predictive', 'controller_fast_loop_dose',
                 'controller_snapshot_predictive', 'controller_snapshot_dose']:
        subprocess.run([str(binary), name, str(build / name)], check=True)
    for before, after in [('active', 'recovered_active'), ('stopped', 'recovered_ended'),
                          ('pending', 'recovered_ended'), ('partial_upload', 'recovered_ended'),
                          ('resumed_pending', 'recovered_resumed'), ('submitted', 'idle'),
                          ('corrupt', 'blocked'), ('controller_snapshot', 'recovered_snapshot'),
                          ('flow', 'recovered_flow')]:
        reboot = build / before
        subprocess.run([str(binary), before + '_before_reboot', str(reboot)], check=True)
        subprocess.run([str(binary), after + '_after_reboot', str(reboot)], check=True)
    reboot = build / 'double_reboot'
    for scenario in ['active_before_reboot', 'recover_only_after_reboot', 'recovered_active_after_reboot']:
        subprocess.run([str(binary), scenario, str(reboot)], check=True)
