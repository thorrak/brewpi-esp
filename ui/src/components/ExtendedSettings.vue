<template>
  <div class="py-6">
    <div class="mx-auto max-w-7xl px-4 sm:px-6 md:px-8">
      <h1 class="text-2xl font-semibold text-gray-900">{{ $t("extended_settings.controller_settings") }}</h1>
    </div>
    <div class="mx-auto max-w-7xl px-4 sm:px-6 md:px-8">
      <div class="py-4">
        <div>
          <form class="space-y-8 divide-y divide-gray-200" @submit.prevent="submitForm">
            <div class="space-y-8 divide-y divide-gray-200">


              <div v-if="ExtendedSettingsStore.hasExtendedSettings">
                <div>
                  <h3 class="text-lg font-medium leading-6 text-gray-900">{{ $t("extended_settings.controller_settings") }}</h3>
                  <p class="mt-1 text-sm text-gray-500">{{ $t("extended_settings.controller_settings_msg") }}</p>
                </div>

<!--                &lt;!&ndash; LargeTFT &ndash;&gt;-->
<!--                &lt;!&ndash; TODO - hide this if using an IIC display &ndash;&gt;-->
<!--                <SwitchGroup as="div" class="flex items-center my-3">-->
<!--                  <Switch v-model="largeTFT" :class="[largeTFT ? 'bg-indigo-600' : 'bg-gray-200', 'relative inline-flex h-6 w-11 flex-shrink-0 cursor-pointer rounded-full border-2 border-transparent transition-colors duration-200 ease-in-out focus:outline-none focus:ring-2 focus:ring-indigo-500 focus:ring-offset-2']">-->
<!--                    <span aria-hidden="true" :class="[largeTFT ? 'translate-x-5' : 'translate-x-0', 'pointer-events-none inline-block h-5 w-5 transform rounded-full bg-white shadow ring-0 transition duration-200 ease-in-out']" />-->
<!--                  </Switch>-->
<!--                  <SwitchLabel as="span" class="ml-3">-->
<!--                    <span class="text-sm font-medium text-gray-900">Large TFT Mode</span>-->
<!--                    <span class="text-sm text-gray-500 mx-1">Expands size of the TFT (for 240x320 TFTs)</span>-->
<!--                  </SwitchLabel>-->
<!--                </SwitchGroup>-->

                <SwitchGroup as="div" class="flex items-center my-3">
                  <Switch v-model="glycol" :class="[glycol ? 'bg-indigo-600' : 'bg-gray-200', 'relative inline-flex h-6 w-11 flex-shrink-0 cursor-pointer rounded-full border-2 border-transparent transition-colors duration-200 ease-in-out focus:outline-none focus:ring-2 focus:ring-indigo-500 focus:ring-offset-2']">
                    <span aria-hidden="true" :class="[glycol ? 'translate-x-5' : 'translate-x-0', 'pointer-events-none inline-block h-5 w-5 transform rounded-full bg-white shadow ring-0 transition duration-200 ease-in-out']" />
                  </Switch>
                  <SwitchLabel as="span" class="ml-3">
                    <span class="text-sm font-medium text-gray-900">{{ $t("extended_settings.glycol_mode") }}</span>
                    <span class="text-sm text-gray-500 mx-1">{{ $t("extended_settings.glycol_mode_desc") }}</span>
                  </SwitchLabel>
                </SwitchGroup>

                <div class="rounded-md bg-yellow-50 p-4 my-3" v-if="glycol && !ExtendedSettingsStore.glycol">
                  <div class="flex">
                    <div class="flex-shrink-0">
                      <ExclamationTriangleIcon class="h-5 w-5 text-yellow-400" aria-hidden="true" />
                    </div>
                    <div class="ml-3">
                      <h3 class="text-sm font-medium text-yellow-800">{{ $t("sitewide.warning") }}</h3>
                      <div class="mt-2 text-sm text-yellow-700">
                        <p>{{ $t("extended_settings.glycol_mode_warning") }}</p>
                      </div>
                    </div>
                  </div>
                </div>

                <div v-if="glycol && ExtendedSettingsStore.hasGlycolCoolingAlgorithm" class="my-4">
                  <label for="glycol-cooling-algorithm" class="block text-sm font-medium leading-6 text-gray-900">{{ $t("extended_settings.glycol_cooling_algorithm") }}</label>
                  <select id="glycol-cooling-algorithm" v-model="glycolCoolingAlgorithm" name="glycol-cooling-algorithm"
                          class="mt-2 block w-full rounded-md border-gray-300 py-2 text-gray-900 shadow-sm focus:border-indigo-500 focus:ring-indigo-500 sm:max-w-md sm:text-sm"
                          aria-describedby="glycol-cooling-algorithm-description glycol-cooling-algorithm-switch">
                    <option value="predictive_coast">{{ $t("extended_settings.predictive_coast") }}</option>
                    <option value="pulse_dose">{{ $t("extended_settings.pulse_dose") }}</option>
                  </select>
                  <p id="glycol-cooling-algorithm-description" class="mt-2 text-sm text-gray-500">{{ $t("extended_settings.glycol_cooling_algorithm_desc") }}</p>
                  <p id="glycol-cooling-algorithm-switch" class="mt-2 text-sm text-gray-500">{{ $t("extended_settings.glycol_cooling_algorithm_switch") }}</p>
                </div>

                <!-- InvertTFT -->
                <!-- TODO - hide this if using an IIC display -->
                <SwitchGroup as="div" class="flex items-center my-3">
                  <Switch v-model="invertTFT" :class="[invertTFT ? 'bg-indigo-600' : 'bg-gray-200', 'relative inline-flex h-6 w-11 flex-shrink-0 cursor-pointer rounded-full border-2 border-transparent transition-colors duration-200 ease-in-out focus:outline-none focus:ring-2 focus:ring-indigo-500 focus:ring-offset-2']">
                    <span aria-hidden="true" :class="[invertTFT ? 'translate-x-5' : 'translate-x-0', 'pointer-events-none inline-block h-5 w-5 transform rounded-full bg-white shadow ring-0 transition duration-200 ease-in-out']" />
                  </Switch>
                  <SwitchLabel as="span" class="ml-3">
                    <span class="text-sm font-medium text-gray-900">{{ $t("extended_settings.rotate_tft") }}</span>
                    <span class="text-sm text-gray-500 mx-1">{{ $t("extended_settings.rotate_tft_desc") }}</span>
                  </SwitchLabel>
                </SwitchGroup>

                <!-- ResetScreenOnPin -->
                <SwitchGroup as="div" class="flex items-center my-3">
                  <Switch v-model="resetScreenOnPin" :class="[resetScreenOnPin ? 'bg-indigo-600' : 'bg-gray-200', 'relative inline-flex h-6 w-11 flex-shrink-0 cursor-pointer rounded-full border-2 border-transparent transition-colors duration-200 ease-in-out focus:outline-none focus:ring-2 focus:ring-indigo-500 focus:ring-offset-2']">
                    <span aria-hidden="true" :class="[resetScreenOnPin ? 'translate-x-5' : 'translate-x-0', 'pointer-events-none inline-block h-5 w-5 transform rounded-full bg-white shadow ring-0 transition duration-200 ease-in-out']" />
                  </Switch>
                  <SwitchLabel as="span" class="ml-3">
                    <span class="text-sm font-medium text-gray-900">{{ $t("extended_settings.reset_screen_on_pin") }}</span>
                    <span class="text-sm text-gray-500 mx-1">{{ $t("extended_settings.reset_screen_on_pin_desc") }}</span>
                  </SwitchLabel>
                </SwitchGroup>


              </div>

              <div>
                <div>
                  <h3 class="text-lg font-medium leading-6 text-gray-900 mt-4">{{ $t("extended_settings.min_times_header") }}</h3>
                  <p class="mt-1 text-sm text-gray-500">{{ $t("extended_settings.min_times_desc") }}</p>
                </div>

                <Listbox as="div" v-model="selectedSettingSet">
                  <ListboxLabel class="sr-only">{{ $t("extended_settings.min_times_set") }}</ListboxLabel>
                  <div class="relative">
                    <div class="inline-flex divide-x divide-indigo-700 rounded-md shadow-sm">
                      <div class="inline-flex items-center gap-x-1.5 rounded-l-md bg-indigo-600 py-2 px-3 text-white shadow-sm">
                        <CheckIcon class="-ml-0.5 h-5 w-5" aria-hidden="true" />
                        <p class="text-sm font-semibold">{{ selectedSettingSet.title }}</p>
                      </div>
                      <ListboxButton class="inline-flex items-center rounded-l-none rounded-r-md bg-indigo-600 p-2 hover:bg-indigo-700 focus:outline-none focus:ring-2 focus:ring-indigo-600 focus:ring-offset-2 focus:ring-offset-gray-50">
                        <span class="sr-only">{{ $t("extended_settings.change_time_set_button") }}</span>
                        <ChevronDownIcon class="h-5 w-5 text-white" aria-hidden="true" />
                      </ListboxButton>
                    </div>

                    <transition leave-active-class="transition ease-in duration-100" leave-from-class="opacity-100" leave-to-class="opacity-0">
                      <ListboxOptions class="absolute z-10 mt-2 w-72 origin-top-right divide-y divide-gray-200 overflow-hidden rounded-md bg-white shadow-lg ring-1 ring-black ring-opacity-5 focus:outline-none">
                        <ListboxOption as="template" v-for="option in minimumTimesSets" :key="option.title" :value="option" v-slot="{ active, selected }">
                          <li :class="[active ? 'bg-indigo-600 text-white' : 'text-gray-900', 'cursor-default select-none p-4 text-sm']">
                            <div class="flex flex-col">
                              <div class="flex justify-between">
                                <p :class="selected ? 'font-semibold' : 'font-normal'">{{ option.title }}</p>
                                <span v-if="selected" :class="active ? 'text-white' : 'text-indigo-600'">
                                  <CheckIcon class="h-5 w-5" aria-hidden="true" />
                                </span>
                              </div>
                              <p :class="[active ? 'text-indigo-200' : 'text-gray-500', 'mt-2']">{{ option.description }}</p>
                            </div>
                          </li>
                        </ListboxOption>
                      </ListboxOptions>
                    </transition>

                  </div>
                </Listbox>

                <div class="rounded-md bg-yellow-50 p-4" v-if="selectedSettingSet.value === 1">
                  <div class="flex">
                    <div class="flex-shrink-0">
                      <ExclamationTriangleIcon class="h-5 w-5 text-yellow-400" aria-hidden="true" />
                    </div>
                    <div class="ml-3">
                      <h3 class="text-sm font-medium text-yellow-800">{{ $t("sitewide.warning") }}</h3>
                      <div class="mt-2 text-sm text-yellow-700">
                        <p>{{ $t("extended_settings.low_delay_warning") }}</p>
                      </div>
                    </div>
                  </div>
                </div>
                <div class="rounded-md bg-yellow-50 p-4" v-else-if="selectedSettingSet.value === 2">
                  <div class="flex">
                    <div class="flex-shrink-0">
                      <ExclamationTriangleIcon class="h-5 w-5 text-yellow-400" aria-hidden="true" />
                    </div>
                    <div class="ml-3">
                      <h3 class="text-sm font-medium text-yellow-800">{{ $t("sitewide.warning") }}</h3>
                      <div class="mt-2 text-sm text-yellow-700">
                        <p>{{ $t("extended_settings.custom_times_warning") }}</p>
                      </div>
                    </div>
                  </div>
                </div>


              </div>
            </div>

            <div v-if="selectedSettingSet.value === 2">
              <!-- MIN_COOL_OFF_TIME -->
              <div class="mb-3">
                <label for="min-cool-off" class="block text-sm font-medium leading-6 text-gray-900">{{ $t("extended_settings.min_cool_off_time") }}</label>
                <div class="mt-2">
                  <input type="number" name="min-cool-off" id="min-cool-off" class="block w-full rounded-md border-0 py-1.5 text-gray-900 shadow-sm ring-1 ring-inset ring-gray-300 placeholder:text-gray-400 focus:ring-2 focus:ring-inset focus:ring-indigo-600 sm:text-sm sm:leading-6"
                         v-model="MIN_COOL_OFF_TIME" aria-describedby="min-cool-off-description" />
                </div>
                <p class="mt-2 text-sm text-gray-500" id="min-cool-off-description">{{ $t("extended_settings.min_cool_off_time_desc") }}</p>
              </div>

              <!-- MIN_HEAT_OFF_TIME -->
              <div class="mb-3">
                <label for="min-heat-off" class="block text-sm font-medium leading-6 text-gray-900">{{ $t("extended_settings.min_heat_off_time") }}</label>
                <div class="mt-2">
                  <input type="number" name="min-heat-off" id="min-heat-off" class="block w-full rounded-md border-0 py-1.5 text-gray-900 shadow-sm ring-1 ring-inset ring-gray-300 placeholder:text-gray-400 focus:ring-2 focus:ring-inset focus:ring-indigo-600 sm:text-sm sm:leading-6"
                         v-model="MIN_HEAT_OFF_TIME" aria-describedby="min-heat-off-description" />
                </div>
                <p class="mt-2 text-sm text-gray-500" id="min-heat-off-description">{{ $t("extended_settings.min_heat_off_time_desc") }}</p>
              </div>

              <!-- MIN_COOL_ON_TIME -->
              <div class="mb-3">
                <label for="min-cool-on" class="block text-sm font-medium leading-6 text-gray-900">{{ $t("extended_settings.min_cool_on_time") }}</label>
                <div class="mt-2">
                  <input type="number" name="min-cool-on" id="min-cool-on" class="block w-full rounded-md border-0 py-1.5 text-gray-900 shadow-sm ring-1 ring-inset ring-gray-300 placeholder:text-gray-400 focus:ring-2 focus:ring-inset focus:ring-indigo-600 sm:text-sm sm:leading-6"
                         v-model="MIN_COOL_ON_TIME" aria-describedby="min-cool-on-description" />
                </div>
                <p class="mt-2 text-sm text-gray-500" id="min-cool-on-description">{{ $t("extended_settings.min_cool_on_time_desc") }}</p>
              </div>

              <!-- MIN_HEAT_ON_TIME -->
              <div class="mb-3">
                <label for="min-heat-on" class="block text-sm font-medium leading-6 text-gray-900">{{ $t("extended_settings.min_heat_on_time") }}</label>
                <div class="mt-2">
                  <input type="number" name="min-heat-on" id="min-heat-on" class="block w-full rounded-md border-0 py-1.5 text-gray-900 shadow-sm ring-1 ring-inset ring-gray-300 placeholder:text-gray-400 focus:ring-2 focus:ring-inset focus:ring-indigo-600 sm:text-sm sm:leading-6"
                         v-model="MIN_HEAT_ON_TIME" aria-describedby="min-heat-on-description" />
                </div>
                <p class="mt-2 text-sm text-gray-500" id="min-heat-on-description">{{ $t("extended_settings.min_heat_on_time_desc") }}</p>
              </div>

              <!-- MIN_COOL_OFF_TIME_FRIDGE_CONSTANT -->
              <div class="mb-3">
                <label for="min-cool-off-fridge-constant" class="block text-sm font-medium leading-6 text-gray-900">{{ $t("extended_settings.min_cool_off_time_fridge_constant") }}</label>
                <div class="mt-2">
                  <input type="number" name="min-cool-off-fridge-constant" id="min-cool-off-fridge-constant" class="block w-full rounded-md border-0 py-1.5 text-gray-900 shadow-sm ring-1 ring-inset ring-gray-300 placeholder:text-gray-400 focus:ring-2 focus:ring-inset focus:ring-indigo-600 sm:text-sm sm:leading-6"
                         v-model="MIN_COOL_OFF_TIME_FRIDGE_CONSTANT" aria-describedby="min-cool-off-fridge-constant-description" />
                </div>
                <p class="mt-2 text-sm text-gray-500" id="min-cool-off-fridge-constant-description">{{ $t("extended_settings.min_cool_off_time_fridge_constant_desc") }}</p>
              </div>

              <!-- MIN_SWITCH_TIME -->
              <div class="mb-3">
                <label for="min-switch-time" class="block text-sm font-medium leading-6 text-gray-900">{{ $t("extended_settings.min_switch_time") }}</label>
                <div class="mt-2">
                  <input type="number" name="min-switch-time" id="min-switch-time" class="block w-full rounded-md border-0 py-1.5 text-gray-900 shadow-sm ring-1 ring-inset ring-gray-300 placeholder:text-gray-400 focus:ring-2 focus:ring-inset focus:ring-indigo-600 sm:text-sm sm:leading-6"
                         v-model="MIN_SWITCH_TIME" aria-describedby="min-switch-time-description" />
                </div>
                <p class="mt-2 text-sm text-gray-500" id="min-switch-time-description">{{ $t("extended_settings.min_switch_time_desc") }}</p>
              </div>

              <!-- COOL_PEAK_DETECT_TIME -->
              <div class="mb-3">
                <label for="cool-peak-detect-time" class="block text-sm font-medium leading-6 text-gray-900">{{ $t("extended_settings.cool_peak_detect_time") }}</label>
                <div class="mt-2">
                  <input type="number" name="cool-peak-detect-time" id="cool-peak-detect-time" class="block w-full rounded-md border-0 py-1.5 text-gray-900 shadow-sm ring-1 ring-inset ring-gray-300 placeholder:text-gray-400 focus:ring-2 focus:ring-inset focus:ring-indigo-600 sm:text-sm sm:leading-6"
                         v-model="COOL_PEAK_DETECT_TIME" aria-describedby="cool-peak-detect-time-description" />
                </div>
                <p class="mt-2 text-sm text-gray-500" id="cool-peak-detect-time-description">{{ $t("extended_settings.cool_peak_detect_time_desc") }}</p>
              </div>

              <!-- HEAT_PEAK_DETECT_TIME -->
              <div class="mb-3">
                <label for="heat-peak-detect-time" class="block text-sm font-medium leading-6 text-gray-900">{{ $t("extended_settings.heat_peak_detect_time") }}</label>
                <div class="mt-2">
                  <input type="number" name="heat-peak-detect-time" id="heat-peak-detect-time" class="block w-full rounded-md border-0 py-1.5 text-gray-900 shadow-sm ring-1 ring-inset ring-gray-300 placeholder:text-gray-400 focus:ring-2 focus:ring-inset focus:ring-indigo-600 sm:text-sm sm:leading-6"
                         v-model="HEAT_PEAK_DETECT_TIME" aria-describedby="heat-peak-detect-time-description" />
                </div>
                <p class="mt-2 text-sm text-gray-500" id="heat-peak-detect-time-description">{{ $t("extended_settings.heat_peak_detect_time_desc") }}</p>
              </div>


            </div>

            <div class="pt-4">
              <div class="pb-3">
                <h3 class="text-lg font-medium leading-6 text-gray-900">{{ $t("extended_settings.temperature_units") }}</h3>
                <p class="mt-1 text-sm text-gray-500">{{ $t("extended_settings.temperature_units_msg") }}</p>
              </div>


              <span class="text-sm font-medium text-gray-900">{{ $t("extended_settings.current_temperature_units") }}:
                <span class="pl-2"><TempUnitsChangeModal /></span>
              </span>
            </div>


            <div class="pt-5">
              <p v-if="ExtendedSettingsStore.extendedSettingsUpdateError" role="alert" class="mb-3 text-sm text-red-700">{{ $t("extended_settings.save_failed") }}</p>
              <p v-else-if="settingsSaved" role="status" class="mb-3 text-sm text-green-700">{{ $t("extended_settings.saved") }}</p>
              <div class="flex justify-end">
                <!--          <button type="button" class="rounded-md border border-gray-300 bg-white py-2 px-4 text-sm font-medium text-gray-700 shadow-sm hover:bg-gray-50 focus:outline-none focus:ring-2 focus:ring-indigo-500 focus:ring-offset-2">Cancel</button>-->
                <button type="submit" :disabled="saving" class="ml-3 inline-flex justify-center rounded-md border border-transparent bg-indigo-600 py-2 px-4 text-sm font-medium text-white shadow-sm hover:bg-indigo-700 focus:outline-none focus:ring-2 focus:ring-indigo-500 focus:ring-offset-2 disabled:opacity-50">{{ $t("sitewide.save") }}</button>
              </div>
            </div>

          </form>
        </div>
      </div>
    </div>

  </div>
</template>

<script setup>
import { useExtendedSettingsStore } from "@/stores/ExtendedSettingsStore";
import { Switch, SwitchGroup, SwitchLabel, Listbox, ListboxButton, ListboxLabel, ListboxOption, ListboxOptions } from "@headlessui/vue";
import { CheckIcon, ChevronDownIcon, ExclamationTriangleIcon } from '@heroicons/vue/20/solid'
import { i18n } from "@/main.js";
import { onMounted, ref } from "vue";
import { useLoading } from "vue-loading-overlay";
import { useControlConstantsStore } from "@/stores/ControlConstantsStore";
import TempUnitsChangeModal from "@/components/dashboard/TempUnitsChangeModal.vue";

const minimumTimesSets = [
  { title: i18n.global.t('extended_settings.set_defaults_title'), description: i18n.global.t('extended_settings.set_defaults_desc'), value: 0 },
  { title: i18n.global.t('extended_settings.set_low_delay_title'), description: i18n.global.t('extended_settings.set_low_delay_desc'), value: 1 },
  { title: i18n.global.t('extended_settings.set_custom_times_title'), description: i18n.global.t('extended_settings.set_custom_times_desc'), value: 2 },
]

const $loading = useLoading({});

let ExtendedSettingsStore = useExtendedSettingsStore();  // Updated in ExtendedSettings.vue
let ControlConstantsStore = useControlConstantsStore();  // Updated in ExtendedSettings.vue

let largeTFT = ref(false);
let glycol = ref(false);
let glycolCoolingAlgorithm = ref(null);
let settingsSaved = ref(false);
let saving = ref(false);
let invertTFT = ref(false);
let resetScreenOnPin = ref(false);
let SETTINGS_CHOICE = ref(0);
let MIN_COOL_OFF_TIME = ref(0);
let MIN_HEAT_OFF_TIME = ref(0);
let MIN_COOL_ON_TIME = ref(0);
let MIN_HEAT_ON_TIME = ref(0);
let MIN_COOL_OFF_TIME_FRIDGE_CONSTANT = ref(0);
let MIN_SWITCH_TIME = ref(0);
let COOL_PEAK_DETECT_TIME = ref(0);
let HEAT_PEAK_DETECT_TIME = ref(0);
let selectedSettingSet = ref(minimumTimesSets[0]);

let tempUnits = ref("");

onMounted(() => {
  // Retrieve initial data
  ExtendedSettingsStore.getExtendedSettings().then(() => {
    updateCachedSettings();
  });
  ControlConstantsStore.getControlConstants().then(() => {
    tempUnits.value = ControlConstantsStore.tempFormat;
  })
});

async function submitForm() {
  if (saving.value) return;
  saving.value = true;
  settingsSaved.value = false;
  const loader = $loading.show({});
  try {
    settingsSaved.value = await ExtendedSettingsStore.setExtendedSettings(glycol.value, largeTFT.value, invertTFT.value, resetScreenOnPin.value, selectedSettingSet.value.value, MIN_COOL_OFF_TIME.value, MIN_HEAT_OFF_TIME.value, MIN_COOL_ON_TIME.value, MIN_HEAT_ON_TIME.value,
        MIN_COOL_OFF_TIME_FRIDGE_CONSTANT.value, MIN_SWITCH_TIME.value, COOL_PEAK_DETECT_TIME.value, HEAT_PEAK_DETECT_TIME.value,
        ExtendedSettingsStore.hasGlycolCoolingAlgorithm ? glycolCoolingAlgorithm.value : undefined);
    if (settingsSaved.value) {
      await ExtendedSettingsStore.getExtendedSettings();
      if (!ExtendedSettingsStore.extendedSettingsError) updateCachedSettings();
    }
  } finally {
    saving.value = false;
    loader.hide();
  }
}

function updateCachedSettings() {
  largeTFT.value = ExtendedSettingsStore.largeTFT;
  invertTFT.value = ExtendedSettingsStore.invertTFT;
  resetScreenOnPin.value = ExtendedSettingsStore.resetScreenOnPin;
  glycol.value = ExtendedSettingsStore.glycol;
  glycolCoolingAlgorithm.value = ExtendedSettingsStore.glycolCoolingAlgorithm;

  SETTINGS_CHOICE.value = ExtendedSettingsStore.SETTINGS_CHOICE;
  MIN_COOL_OFF_TIME.value = ExtendedSettingsStore.MIN_COOL_OFF_TIME;
  MIN_HEAT_OFF_TIME.value = ExtendedSettingsStore.MIN_HEAT_OFF_TIME;
  MIN_COOL_ON_TIME.value = ExtendedSettingsStore.MIN_COOL_ON_TIME;
  MIN_HEAT_ON_TIME.value = ExtendedSettingsStore.MIN_HEAT_ON_TIME;
  MIN_COOL_OFF_TIME_FRIDGE_CONSTANT.value = ExtendedSettingsStore.MIN_COOL_OFF_TIME_FRIDGE_CONSTANT;
  MIN_SWITCH_TIME.value = ExtendedSettingsStore.MIN_SWITCH_TIME;
  COOL_PEAK_DETECT_TIME.value = ExtendedSettingsStore.COOL_PEAK_DETECT_TIME;
  HEAT_PEAK_DETECT_TIME.value = ExtendedSettingsStore.HEAT_PEAK_DETECT_TIME;
  selectedSettingSet.value = minimumTimesSets[ExtendedSettingsStore.SETTINGS_CHOICE];
}

</script>

<style scoped>

</style>
