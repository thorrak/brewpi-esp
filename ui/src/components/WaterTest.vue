<template>
  <div class="py-6">
    <div class="mx-auto max-w-4xl px-4 sm:px-6 md:px-8 space-y-6">
      <div>
        <h1 class="text-2xl font-semibold text-gray-900">Contribute a water test</h1>
        <p class="mt-2 text-gray-600">Help improve BrewPi's glycol simulator using your own fermenter and cooling system.</p>
      </div>

      <div v-if="store.connectionError" class="notice notice-error" role="alert">
        <p>{{ store.connectionError }}</p>
        <p class="mt-1">Displayed values are the last known state, not live readings. Closing this page or losing WiFi does not stop a test.</p>
        <button type="button" class="mt-2 underline" @click="refreshStatus">Reconnect now</button>
      </div>
      <div v-if="actionError" class="notice notice-error" role="alert">{{ actionError }}</div>
      <div v-if="status?.reason && !status.test_id && !status.active && !status.control_owned" class="notice notice-info" role="status">{{ reasonLabel }}</div>
      <p v-if="!status && !store.connectionError" role="status" class="text-gray-600">Connecting to your BrewPi…</p>

      <section v-if="status && (status.test_id || status.control_owned || status.active)" class="card" aria-labelledby="progress-heading">
        <div class="flex flex-wrap items-start justify-between gap-4">
          <div>
            <h2 id="progress-heading" class="text-lg font-semibold text-gray-900">{{ outcomeTitle }}</h2>
            <p class="mt-1 text-gray-600" aria-live="polite">{{ phaseLabel }}</p>
          </div>
          <button v-if="status.active" type="button" class="button button-stop" :disabled="busy === 'stop' || status.phase === 'stopping'" @click="sendAction('stop')">
            {{ busy === 'stop' || status.phase === 'stopping' ? 'Stopping…' : 'Stop test' }}
          </button>
        </div>
        <p v-if="status.active" class="mt-3 text-sm text-gray-600">The device runs the entire sequence. You may close this page. Stop may wait for the relay's minimum ON time before switching the pump off.</p>
        <div class="mt-5 grid grid-cols-2 sm:grid-cols-4 gap-4">
          <div v-if="status.control_owned || status.active"><p class="metric-label">Water / beer probe</p><p class="metric">{{ displayTemperature(status.beer_c) }}</p></div>
          <div v-if="status.moved_chamber_probe && (status.control_owned || status.active)"><p class="metric-label">Glycol bath</p><p class="metric">{{ displayTemperature(status.glycol_c) }}</p></div>
          <div v-if="status.control_owned || status.active"><p class="metric-label">Pump command</p><p class="metric">{{ status.pump_on ? 'ON' : 'OFF' }}</p></div>
          <div><p class="metric-label">Elapsed</p><p class="metric">{{ displayDuration(status.elapsed_s) }}</p></div>
        </div>
        <p v-if="!status.control_owned && !status.active" class="notice notice-info mt-3">Normal control has been restored using the saved mode and setpoints. Use the submission link below to view the recorded test temperatures.</p>
        <p v-if="!status.moved_chamber_probe" class="mt-3 text-sm text-gray-600">
          <template v-if="status.glycol_temperature_source === 'reported_setpoint'">Glycol temperature not measured — using reported setpoint {{ displayTemperature(status.reported_chiller_setpoint_c) }}.</template>
          <template v-else-if="status.glycol_temperature_source === 'unknown'">Glycol temperature not measured — input unknown.</template>
          <template v-else>Glycol temperature is not being measured during this test.</template>
        </p>
        <div v-if="status.active" class="mt-4">
          <label for="test-progress" class="metric-label">Pulse {{ status.pulse_number || 0 }} of 3 · maximum {{ displayDuration(status.max_duration_s || 5400) }}</label>
          <progress id="test-progress" class="mt-2 w-full accent-indigo-600" :value="status.elapsed_s || 0" :max="status.max_duration_s || 5400"></progress>
        </div>
        <p v-if="status.reason" class="mt-4 text-sm text-gray-700">{{ reasonLabel }}</p>
        <div class="mt-5 border-t border-gray-200 pt-4">
          <h3 class="text-sm font-semibold text-gray-900">Data submission: {{ uploadLabel }}</h3>
          <p v-if="['pending', 'uploading', 'error'].includes(status.upload_status)" class="mt-1 text-sm text-gray-600">Keep BrewPi powered and connected to WiFi. It will retry automatically; you can close this page. Test completion and data delivery are separate.</p>
          <p v-if="status.upload_error" class="mt-2 text-sm text-red-700" role="status">{{ status.upload_error }}</p>
          <a v-if="resultLink" :href="resultLink" target="_blank" rel="noopener noreferrer" class="inline-block mt-3 font-medium text-indigo-700 underline">View this device's submissions and simulator comparisons</a>
          <p v-if="status.upload_status === 'submitted'" class="mt-1 text-sm text-gray-600">Processing may take a little longer. A stopped or inconclusive test can still be submitted successfully.</p>
        </div>
        <div v-if="!status.active && status.control_owned" class="notice notice-info mt-5">
          <p class="font-medium">Normal temperature control is OFF.</p>
          <p class="mt-1">Restore your usual setup before resuming the saved mode and setpoints. No heater is used in this test.</p>
          <label v-if="status.moved_chamber_probe" class="check-row mt-3">
            <input v-model="probeReturned" type="checkbox" class="checkbox" />
            <span>I have returned the chamber probe to its normal location.</span>
          </label>
          <button type="button" class="button button-primary mt-3" :disabled="busy !== '' || !status.can_resume || (status.moved_chamber_probe && !probeReturned)" @click="sendAction('resume')">{{ busy === 'resume' ? 'Resuming…' : 'Resume saved temperature control' }}</button>
        </div>
      </section>

      <div v-if="status && !status.active && !status.control_owned && !status.can_start" class="notice notice-info">
        {{ status.preflight?.reason || 'A new test is not available yet. Any pending submission is retained on the device.' }}
      </div>

      <form v-if="status && !status.active && !status.control_owned" @submit.prevent="startTest" class="space-y-6">
        <div class="card">
          <h2 class="text-lg font-semibold text-gray-900">Prepare once, then let the test run</h2>
          <ol class="list-decimal pl-5 mt-3 space-y-2 text-sm text-gray-700">
            <li>Fill the fermenter with <strong>water</strong> at your usual batch volume. Keep the beer probe in its usual position.</li>
            <li>Connect the normal glycol circuit and run your chiller at its usual setpoint. Leave the heater off.</li>
            <li>Remain nearby for the first pump pulse so you can stop the test if the pump or circulation is not working.</li>
          </ol>
          <p class="mt-3 text-sm text-gray-600">BrewPi records a baseline, runs up to three bounded cooling pulses, and observes the response between pulses. Allow about 60–90 minutes; no reheating or temperature resets are needed. Control stays OFF when the test ends until you choose to resume it.</p>
          <p v-if="status.preflight?.reason" class="notice notice-info mt-4">{{ status.preflight.reason }}</p>
        </div>

        <fieldset class="card space-y-6" :disabled="busy !== '' || !status.can_start || !!store.connectionError">
          <legend class="sr-only">Your test setup</legend>
          <div>
            <h2 class="question">1. What fermenter are you using?</h2>
            <div class="mt-3 grid sm:grid-cols-2 gap-4">
              <div><label for="fermenter-model" class="field-label">Make / model (or a short description)</label><input id="fermenter-model" v-model="form.model" type="text" maxlength="120" class="input" placeholder="e.g. BrewBuilt X3" /></div>
              <div><label for="volume-unit" class="field-label">Volume units</label><select id="volume-unit" :value="form.volumeUnit" @change="changeVolumeUnit($event.target.value)" class="input"><option value="us_gal">US gallons</option><option value="l">Liters</option></select></div>
              <div><label for="fermenter-capacity" class="field-label">Nominal capacity ({{ volumeLabel }})</label><input id="fermenter-capacity" v-model="form.capacity" type="number" min="0.001" step="any" inputmode="decimal" class="input" /><p class="hint">Leave blank if unknown.</p></div>
            </div>
          </div>
          <div>
            <label for="water-volume" class="question block">2. How much water is in it for this test?</label>
            <div class="mt-3 max-w-xs"><label for="water-volume" class="field-label">Water volume ({{ volumeLabel }})</label><input id="water-volume" v-model="form.waterVolume" type="number" min="0.001" step="any" inputmode="decimal" required class="input" /></div>
          </div>
          <div>
            <label for="cooling-type" class="question block">3. How is it cooled?</label>
            <select id="cooling-type" v-model="form.coolingType" required class="input mt-3"><option disabled value="">Choose the cooling configuration</option><option value="jacket">Glycol jacket</option><option value="immersion_coil">Immersion coil</option><option value="other">Another configuration</option><option value="unknown">Unknown</option></select>
          </div>
          <div>
            <label for="probe-mounting" class="question block">4. How is the beer probe installed?</label>
            <select id="probe-mounting" v-model="form.probeMounting" required class="input mt-3"><option disabled value="">Choose the probe position</option><option value="thermowell">Inside a thermowell</option><option value="immersed">Directly immersed</option><option value="outside">Attached outside the fermenter</option><option value="other">Another position</option><option value="unknown">Unknown</option></select>
          </div>
          <div>
            <h2 class="question">5. Can you place your existing chamber probe in the glycol bath?</h2>
            <p class="hint">Optional — you do not need to add a probe.</p>
            <label class="check-row mt-3"><input v-model="form.glycolChoice" type="radio" name="glycol-source" value="chamber_probe" :disabled="!status.preflight?.chamber_available" required class="radio" /><span>Yes, use my chamber probe to measure the bath</span></label>
            <p v-if="!status.preflight?.chamber_available" class="hint">No usable chamber probe is currently available. You can use a reported setpoint below.</p>
            <label class="check-row mt-3"><input v-model="form.glycolChoice" type="radio" name="glycol-source" value="reported_setpoint" required class="radio" /><span>No / I do not have a chamber probe</span></label>
            <div v-if="form.glycolChoice === 'chamber_probe'" class="notice notice-info mt-4">
              <p>Move the chamber probe into the glycol bath now. Its normal assignment will be preserved, and these readings will be recorded as glycol bath temperature during this test. Return it to its normal location before resuming control.</p>
              <label class="check-row mt-3"><input v-model="form.bathPlacementConfirmed" type="checkbox" required class="checkbox" /><span>The chamber probe is now immersed in the glycol bath, separate from the beer probe.</span></label>
            </div>
            <div v-else-if="form.glycolChoice === 'reported_setpoint'" class="mt-4">
              <p class="text-sm text-gray-600">{{ form.unknownSetpoint ? 'Glycol temperature not measured — input unknown.' : 'Glycol temperature not measured — using reported setpoint.' }}</p>
              <div class="grid grid-cols-2 gap-4 mt-3 max-w-md">
                <div><label for="glycol-setpoint" class="field-label">Chiller setpoint</label><input id="glycol-setpoint" v-model="form.glycolSetpoint" type="number" step="any" inputmode="decimal" :required="!form.unknownSetpoint" :disabled="form.unknownSetpoint" class="input" /></div>
                <div><label for="temperature-unit" class="field-label">Temperature units</label><select id="temperature-unit" :value="form.temperatureUnit" @change="changeTemperatureUnit($event.target.value)" class="input"><option value="F">°F</option><option value="C">°C</option></select></div>
              </div>
              <label class="check-row mt-3"><input v-model="form.unknownSetpoint" type="checkbox" class="checkbox" /><span>I do not know the setpoint</span></label>
              <p v-if="form.unknownSetpoint" class="hint">The glycol input will be marked unknown. This may limit the simulator comparison.</p>
            </div>
          </div>
          <div class="border-t border-gray-200 pt-5 space-y-4">
            <label class="check-row"><input v-model="form.waterConfirmed" type="checkbox" required class="checkbox" /><span>My vessel contains water for this test, the heater is off, and the cooling circuit is ready.</span></label>
            <label class="check-row"><input v-model="form.consent" type="checkbox" required class="checkbox" /><span>I agree to submit this test's setup answers, temperature readings, relay events, firmware details, and device GUID to chill.fermentrack.net to improve BrewPi's simulator. This records only this test, not ongoing brewing. The submission and setup answers can be viewed by anyone with the device's results link.</span></label>
            <p class="hint">No account, additional logger, or manual upload is needed. WiFi passwords and upstream API keys are not part of the test data.</p>
            <button type="submit" class="button button-primary">{{ busy === 'start' ? 'Starting…' : 'Start water test' }}</button>
          </div>
        </fieldset>
      </form>
      <a v-if="status && !status.test_id && resultLink" :href="resultLink" target="_blank" rel="noopener noreferrer" class="inline-block text-indigo-700 underline">View previous submissions for this device</a>
    </div>
  </div>
</template>

<script setup>
import { computed, onBeforeUnmount, onMounted, reactive, ref } from 'vue';
import { useWaterTestStore, requestWaterTest } from '@/stores/WaterTestStore';
import { useTempControlStore } from '@/stores/TempControlStore';
import { buildWaterTestPayload, convertTemperature, convertVolume, resultsUrl } from '@/mixins/WaterTest';

const store = useWaterTestStore();
const tempControl = useTempControlStore();
const status = computed(() => store.status);
const busy = ref('');
const actionError = ref('');
const probeReturned = ref(false);
const form = reactive({ model: '', capacity: '', waterVolume: '', volumeUnit: 'us_gal', coolingType: '', probeMounting: '', glycolChoice: '', bathPlacementConfirmed: false, glycolSetpoint: '', unknownSetpoint: false, temperatureUnit: 'F', consent: false, waterConfirmed: false });
const volumeLabel = computed(() => form.volumeUnit === 'us_gal' ? 'US gallons' : 'liters');
const resultLink = computed(() => resultsUrl(status.value?.device_guid, status.value?.result_url));
const phases = { idle: 'Ready for a water test', preflight: 'Checking sensors and recording space', baseline: 'Recording the starting temperature and drift', pilot: 'Pilot cooling pulse', pulse: 'Cooling pulse', observe: 'Observing the delayed cooling response', observation: 'Observing the delayed cooling response', stopping: 'Stopping after the relay minimum ON time', finished: 'Test ended', complete: 'Test ended' };
const phaseLabel = computed(() => phases[status.value?.phase] || (status.value?.phase || '').replaceAll('_', ' '));
const outcomeTitle = computed(() => status.value?.active ? 'Water test in progress' : ({ completed: 'Test completed', stopped: 'Test stopped', interrupted: 'Test interrupted', inconclusive: 'Response inconclusive', failed: 'Test ended with a problem' }[status.value?.outcome] || 'Water test'));
const reasonLabels = {
  program_complete: 'The cooling sequence completed.',
  no_measurable_response: 'The test finished without a clear cooling response.',
  user_stop: 'You stopped the test.',
  beer_sensor_fault: 'The beer probe reported an error. The pump was switched off.',
  beer_sensor_stale: 'The beer probe stopped providing fresh readings. The pump was switched off.',
  temperature_limit: 'The water reached this test’s temperature-change limit.',
  runtime_limit: 'The test reached its maximum running time.',
  recording_failure: 'The device could not continue saving readings. Available data was retained.',
  sample_queue_overflow: 'The device could not keep up with recording. The test was stopped.',
  reboot_interrupted: 'A device restart interrupted the test. The pump sequence was not resumed.',
  recording_gap: 'Part of the recording is missing. The available data was retained.',
  relay_minimum_limit: 'The relay minimum times exceed this test’s pulse limits.',
  pump_time_limit: 'The test reached its total pump-time limit.',
  unexpected_output: 'An unexpected output state ended the test.',
};
const reasonLabel = computed(() => reasonLabels[status.value?.reason] || status.value?.reason || '');
const uploadLabel = computed(() => ({ idle: 'Not started', pending: 'Upload pending', uploading: 'Uploading', submitted: 'Test submitted', error: 'Upload pending — will retry' }[status.value?.upload_status] || 'Waiting for device status'));

function displayTemperature(value) {
  if (typeof value !== 'number' || !Number.isFinite(value)) return 'Unavailable';
  return `${convertTemperature(value, 'C', form.temperatureUnit).toFixed(2)} °${form.temperatureUnit}`;
}
function displayDuration(value) {
  if (typeof value !== 'number' || !Number.isFinite(value)) return '—';
  const seconds = Math.max(0, Math.floor(value));
  return `${Math.floor(seconds / 60)}m ${String(seconds % 60).padStart(2, '0')}s`;
}
function changeVolumeUnit(next) {
  form.waterVolume = convertVolume(form.waterVolume, form.volumeUnit, next);
  form.capacity = convertVolume(form.capacity, form.volumeUnit, next);
  form.volumeUnit = next;
}
function changeTemperatureUnit(next) {
  form.glycolSetpoint = convertTemperature(form.glycolSetpoint, form.temperatureUnit, next);
  form.temperatureUnit = next;
}
let refreshInFlight;
async function refreshStatus() {
  if (!refreshInFlight) refreshInFlight = store.refresh().catch(() => {}).finally(() => { refreshInFlight = null; });
  return refreshInFlight;
}
async function sendAction(action, payload = {}) {
  if (busy.value) return;
  busy.value = action;
  actionError.value = '';
  try {
    await requestWaterTest(`${action}/`, action === 'resume' ? { probe_returned: probeReturned.value } : payload);
    if (action === 'start') probeReturned.value = false;
  } catch (error) {
    actionError.value = error.message || 'Unable to confirm the command. Check the device status before trying again.';
  } finally {
    await refreshStatus();
    busy.value = '';
  }
}
async function startTest() {
  try {
    await sendAction('start', buildWaterTestPayload(form));
  } catch (error) {
    actionError.value = error.message;
  }
}
let pollTimer;
let mounted = false;
async function poll() {
  await refreshStatus();
  if (mounted) pollTimer = window.setTimeout(poll, 2000);
}
onMounted(() => {
  mounted = true;
  if (tempControl.tempFormat === 'C') form.temperatureUnit = 'C';
  poll();
});
onBeforeUnmount(() => {
  mounted = false;
  window.clearTimeout(pollTimer);
});
</script>

<style scoped>
.card { @apply rounded-lg border border-gray-200 bg-white p-5 shadow-sm; }
.notice { @apply rounded-md p-4 text-sm; }
.notice-error { @apply bg-red-50 border border-red-200 text-red-800; }
.notice-info { @apply bg-indigo-50 border border-indigo-100 text-indigo-900; }
.question { @apply text-base font-semibold text-gray-900; }
.field-label { @apply block text-sm font-medium text-gray-700; }
.input { @apply mt-1 block w-full rounded-md border-gray-300 shadow-sm focus:border-indigo-500 focus:ring-indigo-500 sm:text-sm disabled:bg-gray-100; }
.hint { @apply mt-1 text-sm text-gray-500; }
.check-row { @apply flex items-start gap-3 text-sm text-gray-700; }
.checkbox { @apply mt-0.5 h-4 w-4 shrink-0 rounded border-gray-300 text-indigo-600 focus:ring-indigo-500; }
.radio { @apply mt-0.5 h-4 w-4 shrink-0 border-gray-300 text-indigo-600 focus:ring-indigo-500; }
.button { @apply inline-flex justify-center rounded-md px-4 py-2 text-sm font-medium shadow-sm focus:outline-none focus:ring-2 focus:ring-offset-2 disabled:cursor-not-allowed disabled:opacity-50; }
.button-primary { @apply bg-indigo-600 text-white hover:bg-indigo-700 focus:ring-indigo-500; }
.button-stop { @apply bg-red-600 text-white hover:bg-red-700 focus:ring-red-500; }
.metric-label { @apply text-xs font-medium text-gray-500; }
.metric { @apply mt-1 text-xl font-semibold text-gray-900; }
</style>
