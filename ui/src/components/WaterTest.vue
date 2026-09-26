<template>
  <div class="py-6">
    <div class="mx-auto max-w-4xl px-4 sm:px-6 md:px-8 space-y-6">
      <div>
        <h1 class="text-2xl font-semibold text-gray-900">{{ t('water_test.contribute') }}</h1>
        <p class="mt-2 text-gray-600">{{ t('water_test.intro') }}</p>
      </div>

      <div v-if="store.connectionError" class="notice notice-error" role="alert">
        <p>{{ store.connectionError }}</p>
        <p class="mt-1">{{ t('water_test.stale_readings') }}</p>
        <button type="button" class="mt-2 underline" @click="refreshStatus">{{ t('water_test.reconnect') }}</button>
      </div>
      <div v-if="actionError" class="notice notice-error" role="alert">{{ actionError }}</div>
      <div v-if="status?.reason && !status.test_id && !status.active && !status.control_owned" class="notice notice-info" role="status">{{ reasonLabel }}</div>
      <p v-if="!status && !store.connectionError" role="status" class="text-gray-600">{{ t('water_test.connecting') }}</p>

      <section v-if="status && (status.test_id || status.control_owned || status.active)" class="card" aria-labelledby="progress-heading">
        <div class="flex flex-wrap items-start justify-between gap-4">
          <div>
            <h2 id="progress-heading" class="text-lg font-semibold text-gray-900">{{ outcomeTitle }}</h2>
            <p class="mt-1 text-gray-600" aria-live="polite">{{ phaseLabel }}</p>
          </div>
          <button v-if="status.active" type="button" class="button button-stop" :disabled="busy === 'stop' || status.phase === 'stopping'" @click="sendAction('stop')">
            {{ busy === 'stop' || status.phase === 'stopping' ? t('water_test.stopping') : t('water_test.stop') }}
          </button>
        </div>
        <p v-if="status.active" class="mt-3 text-sm text-gray-600">{{ t('water_test.active_help') }}</p>
        <div class="mt-5 grid grid-cols-2 sm:grid-cols-4 gap-4">
          <div v-if="status.control_owned || status.active"><p class="metric-label">{{ t('water_test.beer_probe') }}</p><p class="metric">{{ displayTemperature(status.beer_c) }}</p></div>
          <div v-if="status.moved_chamber_probe && (status.control_owned || status.active)"><p class="metric-label">{{ t('water_test.glycol_bath') }}</p><p class="metric">{{ displayTemperature(status.glycol_c) }}</p></div>
          <div v-if="status.control_owned || status.active"><p class="metric-label">{{ t('water_test.pump_command') }}</p><p class="metric">{{ status.pump_on ? t('water_test.on') : t('water_test.off') }}</p></div>
          <div><p class="metric-label">{{ t('water_test.elapsed') }}</p><p class="metric">{{ displayDuration(status.elapsed_s) }}</p></div>
        </div>
        <p v-if="!status.control_owned && !status.active" class="notice notice-info mt-3">{{ t('water_test.restored') }}</p>
        <p v-if="!status.moved_chamber_probe" class="mt-3 text-sm text-gray-600">
          <template v-if="status.glycol_temperature_source === 'reported_setpoint'">{{ t('water_test.reported_temperature', { temperature: displayTemperature(status.reported_chiller_setpoint_c) }) }}</template>
          <template v-else-if="status.glycol_temperature_source === 'unknown'">{{ t('water_test.unknown_temperature') }}</template>
          <template v-else>{{ t('water_test.unmeasured_temperature') }}</template>
        </p>
        <div v-if="status.active" class="mt-4">
          <label for="test-progress" class="metric-label">{{ t('water_test.pulse_progress', { number: status.pulse_number || 0, duration: displayDuration(status.max_duration_s || 5400) }) }}</label>
          <progress id="test-progress" class="mt-2 w-full accent-indigo-600" :value="status.elapsed_s || 0" :max="status.max_duration_s || 5400"></progress>
        </div>
        <p v-if="status.reason" class="mt-4 text-sm text-gray-700">{{ reasonLabel }}</p>
        <div class="mt-5 border-t border-gray-200 pt-4">
          <h3 class="text-sm font-semibold text-gray-900">{{ t('water_test.submission_status', { status: uploadLabel }) }}</h3>
          <p v-if="['pending', 'uploading', 'error'].includes(status.upload_status)" class="mt-1 text-sm text-gray-600">{{ t('water_test.upload_help') }}</p>
          <p v-if="status.upload_status === 'not_submitted'" class="mt-1 text-sm text-gray-600">{{ t('water_test.not_submitted_help') }}</p>
          <p v-if="status.upload_error" class="mt-2 text-sm text-red-700" role="status">{{ status.upload_error }}</p>
          <a v-if="resultLink && status.upload_status !== 'not_submitted'" :href="resultLink" target="_blank" rel="noopener noreferrer" class="inline-block mt-3 font-medium text-indigo-700 underline">{{ t('water_test.view_results') }}</a>
          <p v-if="status.upload_status === 'submitted'" class="mt-1 text-sm text-gray-600">{{ t('water_test.processing') }}</p>
        </div>
        <div v-if="!status.active && status.control_owned" class="notice notice-info mt-5">
          <p class="font-medium">{{ t('water_test.control_off') }}</p>
          <button type="button" class="button button-primary mt-3" :disabled="busy !== '' || !status.can_resume" @click="sendAction('resume')">{{ busy === 'resume' ? t('water_test.resuming') : t('water_test.resume') }}</button>
        </div>
      </section>

      <form v-if="status && !status.active && !status.control_owned" @submit.prevent="startTest" class="space-y-6">
        <div class="card">
          <h2 class="text-lg font-semibold text-gray-900">{{ t('water_test.prepare_title') }}</h2>
          <ol class="list-decimal pl-5 mt-3 space-y-2 text-sm text-gray-700">
            <i18n-t keypath="water_test.prepare_water" tag="li"><template #water><strong>{{ t('water_test.water') }}</strong></template></i18n-t>
            <li>{{ t('water_test.prepare_circuit') }}</li>
            <li>{{ t('water_test.prepare_nearby') }}</li>
          </ol>
          <p class="mt-3 text-sm text-gray-600">{{ t('water_test.sequence_help') }}</p>
          <p class="mt-3 text-sm text-gray-600">{{ t('water_test.pump_timing') }}</p>
          <p v-if="status.preflight && !status.preflight.beer_configured" class="notice notice-warning mt-4" role="alert">{{ t('water_test.configure_beer_probe') }}</p>
          <p v-if="status.preflight && !status.preflight.cooler_available" class="notice notice-warning mt-4" role="alert">{{ t('water_test.configure_cooling_relay') }}</p>
          <p v-if="(!status.preflight || requiredHardwareAvailable) && !status.can_start" class="notice notice-warning mt-4" role="alert">{{ status.preflight?.reason || t('water_test.cannot_start') }}</p>
        </div>

        <fieldset v-if="requiredHardwareAvailable" class="card space-y-6" :disabled="busy !== '' || !status.can_start || !!store.connectionError">
          <legend class="sr-only">{{ t('water_test.setup') }}</legend>
          <div>
            <h2 class="question">{{ t('water_test.fermenter_question') }}</h2>
            <div class="mt-3 grid sm:grid-cols-2 gap-4">
              <div><label for="fermenter-model" class="field-label">{{ t('water_test.fermenter_model') }}</label><input id="fermenter-model" v-model="form.model" type="text" maxlength="120" class="input" :placeholder="t('water_test.fermenter_placeholder')" /></div>
              <div><label for="volume-unit" class="field-label">{{ t('water_test.volume_units') }}</label><select id="volume-unit" :value="form.volumeUnit" @change="changeVolumeUnit($event.target.value)" class="input"><option value="us_gal">{{ t('water_test.us_gallons') }}</option><option value="l">{{ t('water_test.liters') }}</option></select></div>
              <div><label for="fermenter-capacity" class="field-label">{{ t('water_test.nominal_capacity', { unit: volumeLabel }) }}</label><input id="fermenter-capacity" v-model="form.capacity" type="number" min="0.001" step="any" inputmode="decimal" class="input" /><p class="hint">{{ t('water_test.optional_unknown') }}</p></div>
            </div>
          </div>
          <div>
            <label for="water-volume" class="question block">{{ t('water_test.water_question') }}</label>
            <div class="mt-3 max-w-xs"><label for="water-volume" class="field-label">{{ t('water_test.water_volume', { unit: volumeLabel }) }}</label><input id="water-volume" v-model="form.waterVolume" type="number" min="0.001" step="any" inputmode="decimal" required class="input" /></div>
          </div>
          <div>
            <label for="cooling-type" class="question block">{{ t('water_test.cooling_question') }}</label>
            <select id="cooling-type" v-model="form.coolingType" required class="input mt-3"><option disabled value="">{{ t('water_test.cooling_choose') }}</option><option value="jacket">{{ t('water_test.jacket') }}</option><option value="immersion_coil">{{ t('water_test.immersion_coil') }}</option><option value="other">{{ t('water_test.other_cooling') }}</option><option value="unknown">{{ t('water_test.unknown') }}</option></select>
          </div>
          <div>
            <label for="probe-mounting" class="question block">{{ t('water_test.probe_question') }}</label>
            <select id="probe-mounting" v-model="form.probeMounting" required class="input mt-3"><option disabled value="">{{ t('water_test.probe_choose') }}</option><option value="thermowell">{{ t('water_test.thermowell') }}</option><option value="immersed">{{ t('water_test.immersed') }}</option><option value="outside">{{ t('water_test.outside') }}</option><option value="other">{{ t('water_test.other_probe') }}</option><option value="unknown">{{ t('water_test.unknown') }}</option></select>
          </div>
          <div>
            <h2 class="question">{{ t('water_test.chamber_question') }}</h2>
            <p class="hint">{{ t('water_test.probe_optional') }}</p>
            <label class="check-row mt-3"><input v-model="form.glycolChoice" type="radio" name="glycol-source" value="chamber_probe" :disabled="!status.preflight?.chamber_available" required class="radio" /><span>{{ t('water_test.use_chamber_probe') }}</span></label>
            <p v-if="!status.preflight?.chamber_available" class="hint">{{ t('water_test.chamber_unavailable') }}</p>
            <label class="check-row mt-3"><input v-model="form.glycolChoice" type="radio" name="glycol-source" value="reported_setpoint" required class="radio" /><span>{{ t('water_test.no_chamber_probe') }}</span></label>
            <div v-if="form.glycolChoice === 'reported_setpoint'" class="mt-4">
              <p class="text-sm text-gray-600">{{ form.unknownSetpoint ? t('water_test.unknown_temperature') : t('water_test.reported_setpoint') }}</p>
              <div class="grid grid-cols-2 gap-4 mt-3 max-w-md">
                <div><label for="glycol-setpoint" class="field-label">{{ t('water_test.chiller_setpoint') }}</label><input id="glycol-setpoint" v-model="form.glycolSetpoint" type="number" step="any" inputmode="decimal" :required="!form.unknownSetpoint" :disabled="form.unknownSetpoint" class="input" /></div>
                <div><label for="temperature-unit" class="field-label">{{ t('water_test.temperature_units') }}</label><select id="temperature-unit" :value="form.temperatureUnit" @change="changeTemperatureUnit($event.target.value)" class="input"><option value="F">°F</option><option value="C">°C</option></select></div>
              </div>
              <label class="check-row mt-3"><input v-model="form.unknownSetpoint" type="checkbox" class="checkbox" /><span>{{ t('water_test.unknown_setpoint') }}</span></label>
              <p v-if="form.unknownSetpoint" class="hint">{{ t('water_test.unknown_setpoint_help') }}</p>
            </div>
          </div>
          <div class="border-t border-gray-200 pt-5 space-y-4">
            <label class="check-row"><input v-model="form.waterConfirmed" type="checkbox" required class="checkbox" /><span>{{ t('water_test.water_confirmation') }}</span></label>
            <label class="check-row"><input v-model="form.consent" type="checkbox" required class="checkbox" /><span>{{ t('water_test.consent') }}</span></label>
            <p class="hint">{{ t('water_test.privacy') }}</p>
            <button type="submit" class="button button-primary">{{ busy === 'start' ? t('water_test.starting') : t('water_test.start') }}</button>
          </div>
        </fieldset>
      </form>
      <a v-if="status && !status.test_id && resultLink" :href="resultLink" target="_blank" rel="noopener noreferrer" class="inline-block text-indigo-700 underline">{{ t('water_test.previous_results') }}</a>
    </div>
  </div>
</template>

<script setup>
import { computed, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue';
import { useWaterTestStore, requestWaterTest } from '@/stores/WaterTestStore';
import { useTempControlStore } from '@/stores/TempControlStore';
import { buildWaterTestPayload, convertTemperature, convertVolume, resultsUrl } from '@/mixins/WaterTest';
import { i18n } from '@/i18n';

const { t, te } = i18n.global;
const store = useWaterTestStore();
const tempControl = useTempControlStore();
const status = computed(() => store.status);
const requiredHardwareAvailable = computed(() => status.value?.preflight?.beer_configured === true && status.value?.preflight?.cooler_available === true);
const busy = ref('');
const actionError = ref('');
const form = reactive({ model: '', capacity: '', waterVolume: '', volumeUnit: 'us_gal', coolingType: '', probeMounting: '', glycolChoice: '', glycolSetpoint: '', unknownSetpoint: false, temperatureUnit: 'F', consent: false, waterConfirmed: false });
const volumeLabel = computed(() => t(`water_test.${form.volumeUnit === 'us_gal' ? 'us_gallons' : 'liters'}`));
const resultLink = computed(() => resultsUrl(status.value?.device_guid, status.value?.result_url));

const phaseLabel = computed(() => statusLabel('phases', status.value?.phase, (status.value?.phase || '').replaceAll('_', ' ')));
const outcomeTitle = computed(() => status.value?.active ? t('water_test.in_progress') : statusLabel('outcomes', status.value?.outcome, t('water_test.title')));
const reasonLabel = computed(() => statusLabel('reasons', status.value?.reason));
const uploadLabel = computed(() => statusLabel('uploads', status.value?.upload_status, t('water_test.waiting_status')));

function statusLabel(group, value, fallback = value || '') {
  const key = `water_test.${group}.${value}`;
  return te(key, 'en') ? t(key) : fallback;
}

function displayTemperature(value) {
  if (typeof value !== 'number' || !Number.isFinite(value)) return t('water_test.unavailable');
  return `${convertTemperature(value, 'C', form.temperatureUnit).toFixed(2)} °${form.temperatureUnit}`;
}
function displayDuration(value) {
  if (typeof value !== 'number' || !Number.isFinite(value)) return '—';
  const seconds = Math.max(0, Math.floor(value));
  return t('water_test.duration', { minutes: Math.floor(seconds / 60), seconds: String(seconds % 60).padStart(2, '0') });
}
function changeVolumeUnit(next) {
  form.waterVolume = convertVolume(form.waterVolume, form.volumeUnit, next);
  form.capacity = convertVolume(form.capacity, form.volumeUnit, next);
  form.volumeUnit = next;
}
let temperatureUnitChosen = false;
function changeTemperatureUnit(next) {
  temperatureUnitChosen = true;
  applyTemperatureUnit(next);
}
function applyTemperatureUnit(next) {
  form.glycolSetpoint = convertTemperature(form.glycolSetpoint, form.temperatureUnit, next);
  form.temperatureUnit = next;
}
// The controller preference can arrive after this page mounts. Convert any
// entered setpoint, but preserve an explicit unit choice made on this page.
watch(() => tempControl.tempFormat, (unit) => {
  if (!temperatureUnitChosen && ['C', 'F'].includes(unit)) applyTemperatureUnit(unit);
}, { immediate: true });

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
    await requestWaterTest(`${action}/`, payload);
  } catch (error) {
    actionError.value = error.message || t('water_test.errors.command_unconfirmed');
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
.notice-warning { @apply bg-amber-50 border border-amber-200 text-amber-900; }
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
