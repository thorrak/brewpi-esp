<template>
  <div class="py-6">
    <div class="mx-auto max-w-7xl px-4 sm:px-6 md:px-8">
      <h1 class="text-2xl font-semibold text-gray-900">{{ $t("sitewide.sidebar_options.fermentrack_settings") }}</h1>
    </div>
    <div class="mx-auto max-w-7xl px-4 sm:px-6 md:px-8">
      <div class="py-4">

        <div>
          <form class="space-y-8 divide-y divide-gray-200" @submit.prevent="submitForm" data-form-type="other">

            <div class="space-y-8 divide-y divide-gray-200">

              <div>
                <div>
                  <h3 class="text-lg font-medium leading-6 text-gray-900 mt-4">{{ $t('upstream_settings.fermentrack_settings') }}</h3>
                  <p class="mt-1 text-sm text-gray-500">{{ $t('upstream_settings.fermentrack_settings_desc') }}</p>
                </div>

                <Listbox as="div" v-model="selectedSettingSet" :disabled="isRegistered">
                  <ListboxLabel class="sr-only">{{ $t('upstream_settings.fermentrack_settings') }}</ListboxLabel>
                  <div class="relative">
                    <div class="inline-flex divide-x rounded-md shadow-sm" :class="isRegistered ? 'divide-gray-500' : 'divide-indigo-700'">
                      <div class="inline-flex items-center gap-x-1.5 rounded-l-md py-2 px-3 text-white shadow-sm" :class="isRegistered ? 'bg-gray-400' : 'bg-indigo-600'">
                        <CheckIcon class="-ml-0.5 h-5 w-5" aria-hidden="true" />
                        <p class="text-sm font-semibold">{{ selectedSettingSet.title }}</p>
                      </div>
                      <ListboxButton class="inline-flex items-center rounded-l-none rounded-r-md p-2 focus:outline-none focus:ring-2 focus:ring-offset-2 focus:ring-offset-gray-50" :class="isRegistered ? 'bg-gray-400 cursor-not-allowed focus:ring-gray-400' : 'bg-indigo-600 hover:bg-indigo-700 focus:ring-indigo-600'" :disabled="isRegistered">
                        <span class="sr-only">{{ $t('upstream_settings.upstream_settings_sr_text') }}</span>
                        <ChevronDownIcon class="h-5 w-5 text-white" aria-hidden="true" />
                      </ListboxButton>
                    </div>

                    <transition leave-active-class="transition ease-in duration-100" leave-from-class="opacity-100" leave-to-class="opacity-0">
                      <ListboxOptions class="absolute z-10 mt-2 w-72 origin-top-right divide-y divide-gray-200 overflow-hidden rounded-md bg-white shadow-lg ring-1 ring-black ring-opacity-5 focus:outline-none">
                        <ListboxOption as="template" v-for="option in upstreamSettingsSets" :key="option.title" :value="option" v-slot="{ active, selected }">
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

              </div>
            </div>

            <div class="space-y-8 divide-y divide-gray-200" v-if="selectedSettingSet.value !== 0">
              <div>
                <!-- Display errors from the upstream registration process -->
                <div v-if="UpstreamSettingsStore.loadedUpstreamSettingsFromDevice">
                  <div class="border-l-4 border-red-400 bg-red-50 p-4 mb-4 mt-4" v-if="UpstreamSettingsStore.upstreamRegistrationError === 1 || UpstreamSettingsStore.upstreamRegistrationError === 5 || UpstreamSettingsStore.upstreamRegistrationError === 6">
                    <!-- Error 1 (missing GUID), Error 5 (missing hardware), Error 6 (missing firmware version) are all internal -->
                    <div class="flex">
                      <div class="flex-shrink-0">
                        <ExclamationTriangleIcon class="h-5 w-5 text-red-400" aria-hidden="true" />
                      </div>
                      <div class="ml-3">
                        <p class="text-sm text-red-700">
                          {{ $t("upstream_settings.missing_data_error") }}
                        </p>
                      </div>
                    </div>
                  </div>
                  <div class="border-l-4 border-red-400 bg-red-50 p-4 mb-4 mt-4" v-else-if="UpstreamSettingsStore.upstreamRegistrationError === 3">
                    <!-- Error 3 - User not found -->
                    <div class="flex">
                      <div class="flex-shrink-0">
                        <ExclamationTriangleIcon class="h-5 w-5 text-red-400" aria-hidden="true" />
                      </div>
                      <div class="ml-3">
                        <p class="text-sm text-red-700">
                          {{ $t("upstream_settings.invalid_username_error") }}
                        </p>
                      </div>
                    </div>
                  </div>
                  <div class="border-l-4 border-red-400 bg-red-50 p-4 mb-4 mt-4" v-else-if="UpstreamSettingsStore.upstreamRegistrationError === 7">
                    <!-- Error 7 - API Key is not associated with a brewhouse -->
                    <div class="flex">
                      <div class="flex-shrink-0">
                        <ExclamationTriangleIcon class="h-5 w-5 text-red-400" aria-hidden="true" />
                      </div>
                      <div class="ml-3">
                        <p class="text-sm text-red-700">
                          {{ $t("upstream_settings.bad_api_key_error") }}
                        </p>
                      </div>
                    </div>
                  </div>
                  <div class="border-l-4 border-red-400 bg-red-50 p-4 mb-4 mt-4" v-else-if="UpstreamSettingsStore.upstreamRegistrationError === 4">
                    <!-- Error 4 - User does not have a brewhouse -->
                    <div class="flex">
                      <div class="flex-shrink-0">
                        <ExclamationTriangleIcon class="h-5 w-5 text-red-400" aria-hidden="true" />
                      </div>
                      <div class="ml-3">
                        <p class="text-sm text-red-700">
                          {{ $t("upstream_settings.missing_brewhouse_error") }}
                        </p>
                      </div>
                    </div>
                  </div>
                  <div class="border-l-4 border-red-400 bg-red-50 p-4 mb-4 mt-4" v-else-if="UpstreamSettingsStore.upstreamRegistrationError === 2 || (UpstreamSettingsStore.username === '' && UpstreamSettingsStore.apiKey === '')">
                    <!-- Error 2 - Missing username or API key -->
                    <div class="flex">
                      <div class="flex-shrink-0">
                        <ExclamationTriangleIcon class="h-5 w-5 text-red-400" aria-hidden="true" />
                      </div>
                      <div class="ml-3">
                        <p class="text-sm text-red-700">
                          {{ $t("upstream_settings.no_username_error") }}
                        </p>
                      </div>
                    </div>
                  </div>
                  <div class="border-l-4 border-yellow-400 bg-yellow-50 p-4 mt-4 mb-4" v-else-if="UpstreamSettingsStore.upstreamRegistrationError === 8">
                    <!-- Error 2 - Missing username or API key -->
                    <div class="flex">
                      <div class="flex-shrink-0">
                        <ExclamationTriangleIcon class="h-5 w-5 text-yellow-400" aria-hidden="true" />
                      </div>
                      <div class="ml-3">
                        <p class="text-sm text-yellow-700">
                          {{ $t("upstream_settings.waiting_to_register_error") }}
                        </p>
                      </div>
                    </div>
                  </div>
                  <div class="border-l-4 border-red-400 bg-red-50 p-4 mb-4 mt-4" v-else-if="UpstreamSettingsStore.upstreamRegistrationError === 9">
                    <!-- Error 9 - Registration endpoint error (unreachable, or credentials/config rejected) -->
                    <div class="flex">
                      <div class="flex-shrink-0">
                        <ExclamationTriangleIcon class="h-5 w-5 text-red-400" aria-hidden="true" />
                      </div>
                      <div class="ml-3">
                        <p class="text-sm text-red-700">
                          {{ $t("upstream_settings.registration_failed_error") }}
                        </p>
                      </div>
                    </div>
                  </div>

                  <div v-else-if="UpstreamSettingsStore.upstreamRegistrationError === 0">
                    <div class="border-l-4 border-green-400 bg-green-50 p-4 mb-4 mt-4">
                      <!-- Error 0 - No error -->
                      <div class="flex">
                        <div class="flex-shrink-0">
                          <InformationCircleIcon class="h-5 w-5 text-green-400" aria-hidden="true" />
                        </div>
                        <div class="ml-3">
                          <p class="text-sm text-green-700">
                            {{ $t("upstream_settings.successful_registration_msg") }}
                          </p>
                        </div>
                      </div>
                    </div>

                    <div class="border-l-4 border-indigo-400 bg-indigo-50 p-4 mb-4 mt-4">
                      <!-- Note about telnet being disabled -->
                      <div class="flex">
                        <div class="flex-shrink-0">
                          <InformationCircleIcon class="h-5 w-5 text-indigo-400" aria-hidden="true" />
                        </div>
                        <div class="ml-3">
                          <p class="text-sm text-indigo-700">
                            {{ $t("upstream_settings.telnet_interface_unavailable") }}
                          </p>
                        </div>
                      </div>
                    </div>
                  </div>

                  <div class="border-l-4 border-red-400 bg-red-50 p-4 mb-4 mt-4" v-else>
                    <!-- Error code not captured. Means that Fermentrack is probably more recent than this firmware -->
                    <div class="flex">
                      <div class="flex-shrink-0">
                        <ExclamationTriangleIcon class="h-5 w-5 text-red-400" aria-hidden="true" />
                      </div>
                      <div class="ml-3">
                        <p class="text-sm text-red-700">
                          {{ $t("upstream_settings.unknown_error") }}
                        </p>
                      </div>
                    </div>
                  </div>
                </div>

                <div class="mt-6 grid grid-cols-1 gap-y-6 gap-x-4 sm:grid-cols-6">
                  <!-- Hide the below unless the user is using a custom upstream -->
                  <div class="sm:col-span-4">
                    <label for="hostname" class="block text-sm font-medium text-gray-700">{{ $t("upstream_settings.hostname") }}</label>
                    <div class="mt-1">
                      <!-- Note - There is no actual differentiation between HTTPS and HTTP as the controller currently only can use HTTP for outgoing connections -->
                      <div v-if="UpstreamSettingsStore.deviceID === '' && !UpstreamSettingsStore.awaitingRegistration && selectedSettingSet.value !== 1" class="flex rounded-md shadow-sm">
                        <span class="inline-flex items-center rounded-l-md border border-r-0 border-gray-300 bg-gray-50 px-3 text-gray-500 sm:text-sm" v-if="UpstreamSettingsStore.upstreamPort === 443">http://</span>
                        <span class="inline-flex items-center rounded-l-md border border-r-0 border-gray-300 bg-gray-50 px-3 text-gray-500 sm:text-sm" v-else>http://</span>
                        <input type="text" name="hostname" v-model="UpstreamSettingsStore.upstreamHost"  id="hostname" class="block w-full min-w-0 flex-1 rounded-none rounded-r-md border-gray-300 focus:border-indigo-500 focus:ring-indigo-500 sm:text-sm" />
                      </div>
                      <span v-else-if="selectedSettingSet.value === 1">
                        <!-- Technically, this is currently incorrect, but if I convert to port 443 I can presume this is then valid -->
                        <span v-if="ft_net_port === 443">https://</span><span v-else>http://</span>{{ ft_net_host }}
                      </span>
                      <span v-else>
                        <!-- Effectively only shows when selectedSettingSet.value === 2 -->
                        <span v-if="UpstreamSettingsStore.upstreamPort === 443">https://</span><span v-else>http://</span>{{ UpstreamSettingsStore.upstreamHost }}
                      </span>
                    </div>
                  </div>

                  <!-- Hide the below unless the user is using a custom upstream -->
                  <div class="sm:col-span-4">
                    <label for="port" class="block text-sm font-medium text-gray-700">{{ $t("upstream_settings.port") }}</label>
                    <div class="mt-1">
                      <input id="port" name="port" v-model="UpstreamSettingsStore.upstreamPort" v-if="UpstreamSettingsStore.deviceID === '' && !UpstreamSettingsStore.awaitingRegistration && selectedSettingSet.value !== 1" type="text" class="block w-full rounded-md border-gray-300 shadow-sm focus:border-indigo-500 focus:ring-indigo-500 sm:text-sm" />
                      <span v-else-if="selectedSettingSet.value === 1">{{ ft_net_port }}</span>
                      <span v-else>{{ UpstreamSettingsStore.upstreamPort }}</span>
                    </div>
                  </div>

                  <!-- Hide the below when the user is either already registered, or is using an upstream that doesn't require a username -->
                  <div class="sm:col-span-4" v-if="UpstreamSettingsStore.deviceID === '' && !UpstreamSettingsStore.awaitingRegistration">
                    <label for="username" class="block text-sm font-medium text-gray-700">
                      <span v-if="selectedSettingSet.value === 1">{{ $t("upstream_settings.ft_net_username") }}</span>
                      <span v-else>{{ $t("upstream_settings.username") }}</span>
                    </label>
                    <div class="mt-1">
                      <input type="text" name="username" v-model="UpstreamSettingsStore.username" id="username" data-form-type="other" class="block w-full rounded-md border-gray-300 shadow-sm focus:border-indigo-500 focus:ring-indigo-500 sm:text-sm" />
                    </div>
                  </div>


                  <div class="sm:col-span-4" v-if="UpstreamSettingsStore.hasUpstreamSettings">
                    <label for="deviceid" class="block text-sm font-medium text-gray-700">{{ $t("upstream_settings.device_id") }}</label>
                    <div class="mt-1">
                      <span v-if="UpstreamSettingsStore.deviceID.length <= 0">{{ $t("upstream_settings.not_yet_registered") }}</span>
                      <span v-else><a :href="device_uri" class="text-indigo-600">{{ UpstreamSettingsStore.deviceID }}</a></span>
                    </div>
                  </div>

                  <!-- Always display the Hardware GUID, as it is part of what is registered with Fermentrack -->
                  <div class="sm:col-span-4">
                    <label for="deviceid" class="block text-sm font-medium text-gray-700">{{ $t("upstream_settings.hardware_guid") }}</label>
                    <div class="mt-1">
                      <span>{{ UpstreamSettingsStore.guid }}</span>
                    </div>
                  </div>

                </div>
              </div>
            </div>

            <div class="pt-5">
              <div class="flex justify-end">
                <!--          <button type="button" class="rounded-md border border-gray-300 bg-white py-2 px-4 text-sm font-medium text-gray-700 shadow-sm hover:bg-gray-50 focus:outline-none focus:ring-2 focus:ring-indigo-500 focus:ring-offset-2">Cancel</button>-->
                <button v-if="UpstreamSettingsStore.deviceID === ''" type="submit" class="ml-3 inline-flex justify-center rounded-md border border-transparent bg-indigo-600 py-2 px-4 text-sm font-medium text-white shadow-sm hover:bg-indigo-700 focus:outline-none focus:ring-2 focus:ring-indigo-500 focus:ring-offset-2">{{ $t("sitewide.save") }}</button>
                <button v-else type="button" class="ml-3 inline-flex justify-center rounded-md border border-transparent bg-red-600 py-2 px-4 text-sm font-medium text-white shadow-sm hover:bg-red-700 focus:outline-none focus:ring-2 focus:ring-red-500 focus:ring-offset-2" @click="clearRegistration()">{{ $t("upstream_settings.reset_registration") }}</button>

              </div>
            </div>
          </form>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { useUpstreamSettingsStore, ft_net_host, ft_net_port } from "@/stores/UpstreamSettingsStore";
import { ExclamationTriangleIcon, InformationCircleIcon } from '@heroicons/vue/24/outline'
import { useLoading } from "vue-loading-overlay";
const $loading = useLoading({});
import {onMounted, ref, computed} from "vue";
import { Listbox, ListboxButton, ListboxLabel, ListboxOption, ListboxOptions } from "@headlessui/vue";
import {CheckIcon, ChevronDownIcon} from "@heroicons/vue/20/solid";
import {i18n} from "@/i18n";

// selectedSettingSet is one of three values:
//   0 - "Legacy Fermentack" (or no REST API target)
//   1 - Fermentrack.net
//   2 - Local Fermentrack 2 (FT2, but not FT.net)
// The determination of which is currently in use is made in the onMounted hook by comparing the value to the defined
// FT.net values
const upstreamSettingsSets = [
  { title: i18n.global.t('upstream_settings.upstream_presets.legacy_fermentrack'), description: i18n.global.t('upstream_settings.upstream_presets.legacy_fermentrack_desc'), value: 0 },
  { title: i18n.global.t('upstream_settings.upstream_presets.fermentrack_net'), description: i18n.global.t('upstream_settings.upstream_presets.fermentrack_net_desc'), value: 1 },
  { title: i18n.global.t('upstream_settings.upstream_presets.fermentrack_local'), description: i18n.global.t('upstream_settings.upstream_presets.fermentrack_local_desc'), value: 2 },
]

let selectedSettingSet = ref(upstreamSettingsSets[0]);

const UpstreamSettingsStore = useUpstreamSettingsStore();  // Updated in UpstreamSettings.vue

let loader;


onMounted(() => {
  // Retrieve initial data
  loader = $loading.show({});
  UpstreamSettingsStore.getUpstreamSettings().then(() => {
    loader.hide();

    if(UpstreamSettingsStore.upstreamHost === ft_net_host && UpstreamSettingsStore.upstreamPort === ft_net_port) {
      selectedSettingSet.value = upstreamSettingsSets[1];
    } else if(UpstreamSettingsStore.upstreamHost !== "") {
      selectedSettingSet.value = upstreamSettingsSets[2];
    } else {
      selectedSettingSet.value = upstreamSettingsSets[0];
    }

  }).catch(() => {
    loader.hide();
  });
});



async function submitForm() {
  // Validate the information in the form

  if(selectedSettingSet.value.value === 1) {
    // If the user has selected FT.net, use the FT.net host/port
    UpstreamSettingsStore.upstreamHost = ft_net_host;
    UpstreamSettingsStore.upstreamPort = ft_net_port;
  } else if(selectedSettingSet.value.value === 0) {
    // If the user has selected FT.net, use the FT.net host/port
    UpstreamSettingsStore.upstreamHost = "";
    UpstreamSettingsStore.upstreamPort = 80;
  }

  if(UpstreamSettingsStore.upstreamHost.length >= 127) {
    this.form_error_message = this.$t("upstream_settings.hostname_too_long_error");
    return;
  }

  if(parseInt(UpstreamSettingsStore.upstreamPort) >= 65535 || parseInt(UpstreamSettingsStore.upstreamPort) <= 10) {
    this.form_error_message = this.$t("upstream_settings.port_out_of_range_error");
    return;
  }

  if(UpstreamSettingsStore.username.length >= 127) {
    // TODO - Add feedback here
    // this.form_error_message = "Username is invalid. Must be less than 127 characters.";
    return;
  }

  loader = $loading.show({});
  UpstreamSettingsStore.setUpstreamSettings(UpstreamSettingsStore.upstreamHost, UpstreamSettingsStore.upstreamPort, UpstreamSettingsStore.username, UpstreamSettingsStore.apiKey).then(() => {
    UpstreamSettingsStore.getUpstreamSettings().finally(() => {
      loader.hide();
    }); // Once we've set the upstream settings, let's retrieve them to make sure they saved
  }).catch(() => {
    loader.hide();
  });
  // TODO - Add some kind of error checking/handling here
}

async function clearRegistration() {
  loader = $loading.show({});
  // TODO - Convert this from await to a promise
  await UpstreamSettingsStore.setUpstreamSettings("", 80, "", "");
  await UpstreamSettingsStore.getUpstreamSettings(); // Once we've set the upstream settings, let's retrieve them to make sure they saved
  loader.hide();
  // TODO - Add some kind of error checking/handling here

  // this.updateSuccessful = res.ok;
  // this.alertOpen = true;
  // this.UpstreamSettingsStore.saveUpstreamSettings();
}

const isRegistered = computed(() =>
  UpstreamSettingsStore.loadedUpstreamSettingsFromDevice
  && UpstreamSettingsStore.upstreamRegistrationError === 0
);

const device_uri = computed(() => {
  let http_protocol = "http://";
  if(UpstreamSettingsStore.upstreamPort == 443)
    http_protocol = "https://";
  return http_protocol + UpstreamSettingsStore.upstreamHost + ":" + UpstreamSettingsStore.upstreamPort + "/device/tc/" + UpstreamSettingsStore.deviceID + "/";
})


</script>

<style scoped>

</style>