<template>
  <div class="py-6">
    <div class="mx-auto max-w-7xl px-4 sm:px-6 md:px-8">
      <h1 class="text-2xl font-semibold text-gray-900">{{ $t("sensors.config_sensors_header") }}</h1>
    </div>
    <div class="mx-auto max-w-7xl px-4 sm:px-6 md:px-8">
      <div class="py-4" v-if="BrewPiSensorStore.loaded">


        <div class="px-6 lg:px-8">
          <div class="sm:flex sm:items-center">
            <div class="sm:flex-auto">
              <h1 class="text-xl font-semibold text-gray-900">{{ $t("sensors.detected_devices") }}</h1>
              <p class="mt-2 text-sm text-gray-700">{{ $t("sensors.detected_devices_desc") }}</p>
            </div>
<!--            <div class="mt-4 sm:mt-0 sm:ml-16 sm:flex-none">-->
<!--              <button type="button" class="block rounded-md bg-indigo-600 py-1.5 px-3 text-center text-sm font-semibold leading-6 text-white shadow-sm hover:bg-indigo-500 focus-visible:outline focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-indigo-600">Add user</button>-->
<!--            </div>-->
          </div>

          <!-- Glycol mode: Beer sensor is required, fridge sensor is not used -->
          <div class="border-l-4 border-red-400 bg-red-50 p-4 mt-8" v-if="ExtendedSettingsStore.glycol && !hasBeerSensor">
            <div class="flex">
              <div class="flex-shrink-0">
                <ExclamationTriangleIcon class="h-5 w-5 text-red-400" aria-hidden="true" />
              </div>
              <div class="ml-3">
                <p class="text-sm text-red-700">
                  {{ $t("sensors.no_beer_warning_glycol") }}
                </p>
              </div>
            </div>
          </div>
          <!-- Compressor mode: Fridge sensor is required -->
          <div class="border-l-4 border-red-400 bg-red-50 p-4 mt-8" v-else-if="!ExtendedSettingsStore.glycol && !hasFridgeSensor">
            <div class="flex">
              <div class="flex-shrink-0">
                <ExclamationTriangleIcon class="h-5 w-5 text-red-400" aria-hidden="true" />
              </div>
              <div class="ml-3">
                <p class="text-sm text-red-700">
                  {{ $t("sensors.no_fridge_warning") }}
                </p>
              </div>
            </div>
          </div>
          <!-- Compressor mode: Beer sensor is optional but recommended -->
          <div class="border-l-4 border-yellow-400 bg-yellow-50 p-4 mt-8" v-else-if="!ExtendedSettingsStore.glycol && !hasBeerSensor">
            <div class="flex">
              <div class="flex-shrink-0">
                <ExclamationTriangleIcon class="h-5 w-5 text-yellow-400" aria-hidden="true" />
              </div>
              <div class="ml-3">
                <p class="text-sm text-yellow-700">
                  {{ $t("sensors.no_beer_warning") }}
                </p>
              </div>
            </div>
          </div>

          <div class="border-l-4 border-red-400 bg-red-50 p-4 mt-8" v-if="!hasHeatActuator && !hasCoolActuator">
            <!-- If there is neither a heat or cool relay configured, show this warning -->
            <div class="flex">
              <div class="flex-shrink-0">
                <ExclamationTriangleIcon class="h-5 w-5 text-red-400" aria-hidden="true" />
              </div>
              <div class="ml-3">
                <p class="text-sm text-red-700">
                  {{ $t("sensors.no_relay_warning") }}
                </p>
              </div>
            </div>
          </div>
          <div class="border-l-4 border-blue-400 bg-blue-50 p-4 mt-8" v-else-if="!hasHeatActuator || !hasCoolActuator">
            <!-- If there is neither a heat or cool relay configured, show this warning -->
            <div class="flex">
              <div class="flex-shrink-0">
                <InformationCircleIcon class="h-5 w-5 text-blue-400" aria-hidden="true" />
              </div>
              <div class="ml-3">
                <p class="text-sm text-blue-700">
                  <span v-if="!hasHeatActuator">{{ $t("sensors.no_heat_warning") }}</span>
                  <span v-else>{{ $t("sensors.no_cool_warning") }}</span>
                </p>
              </div>
            </div>
          </div>


          <div class="mt-8 flow-root">
            <div class="-my-2 -mx-6 overflow-x-auto lg:-mx-8">
              <div class="inline-block min-w-full py-2 align-middle sm:px-6 lg:px-8">
                <div class="overflow-hidden shadow ring-1 ring-black ring-opacity-5 sm:rounded-lg">
                  <table class="min-w-full divide-y divide-gray-300">
                    <thead class="bg-gray-50">
                    <tr>
                      <th scope="col" class="py-3.5 pl-6 pr-3 text-left text-sm font-semibold text-gray-900">{{ $t("sensors.device_type_header") }}</th>
                      <th scope="col" class="px-3 py-3.5 text-left text-sm font-semibold text-gray-900 hidden md:table-cell">{{ $t("sensors.device_identifier_header") }}</th>
                      <th scope="col" class="px-3 py-3.5 text-left text-sm font-semibold text-gray-900">{{ $t("sensors.device_function_header") }}</th>
                      <th scope="col" class="relative py-3.5 pl-3 pr-6 text-right">
                        <a href="#" type="button" class="inline-flex items-center rounded-full border border-transparent bg-indigo-600 px-3 py-1.5 text-xs font-medium text-white shadow-sm hover:bg-indigo-700 focus:outline-none focus:ring-2 focus:ring-indigo-500 focus:ring-offset-2" @click="refreshDevices">
                          <span class="hidden md:flex">{{ $t("sensors.refresh_devices_long") }}</span>
                          <span class="md:hidden">{{ $t("sensors.refresh_devices_short") }}</span>
                        </a>
                        <span class="sr-only">{{ $t("sitewide.edit") }}</span>
                      </th>
                    </tr>
                    </thead>
                    <tbody class="divide-y divide-gray-200 bg-white">
                    <!-- TODO - Add some kind of v-bind key directive here -->
                    <tr v-for="sensor in BrewPiSensorStore.devices" >
                      <td class="whitespace-nowrap py-4 pl-6 pr-3 text-sm font-medium text-gray-900">
                        {{ $t("sitewide.brewpi_hardware_types." + sensor.device_hardware) }}
                        <div class="md:hidden">
                          <div class="text-gray-500" v-if="sensor.device_hardware === 'pin'">{{ $t('sensors.pin_device_number', { pin_number: sensor.pin}) }}</div>
                          <div class="text-gray-400" v-if="sensor.device_hardware === 'pin' && sensor.device_alias.length > 0"><!-- TODO - Figure out how to internationalize alias here -->({{ sensor.device_alias }})</div>
                          <div class="text-gray-500" v-if="sensor.device_hardware === 'onewire_temp' || sensor.device_hardware === 'onewire_2413'">{{ sensor.address }}</div>
                          <div class="text-gray-500" v-if="sensor.device_hardware === 'inkbird_bluetooth'">
                            <span v-if="BrewPiSensorStore.lowerMacAddressInDevices(sensor.address)">{{BrewPiSensorStore.getLowerMacAddress(sensor.address)}}</span>
                            <span v-else>{{ sensor.address }}</span>
                          </div>
                          <div class="text-gray-400" v-if="sensor.device_hardware === 'inkbird_bluetooth' && BrewPiSensorStore.lowerMacAddressInDevices(sensor.address)">
                            ({{ $t('sensors.inkbird_external_probe') }})
                          </div>

                          <div class="text-gray-500" v-if="sensor.device_hardware === 'tplink_switch' || sensor.device_hardware === 'tilt'"><!-- TODO - Figure out how to internationalize alias here -->{{ sensor.device_alias }}</div>
                          <div class="text-gray-400" v-if="sensor.device_hardware === 'tplink_switch'">{{ sensor.address }}</div>
                        </div>
                      </td>
                      <td class="whitespace-nowrap px-3 py-4 text-sm text-gray-500 hidden md:table-cell">
                        <div class="text-gray-900" v-if="sensor.device_hardware === 'pin'">{{ $t('sensors.pin_device_number', { pin_number: sensor.pin}) }}</div>
                        <div class="text-gray-500" v-if="sensor.device_hardware === 'pin' && sensor.device_alias.length > 0"><!-- TODO - Figure out how to internationalize alias here -->({{ sensor.device_alias }})</div>
                        <div class="text-gray-900" v-if="sensor.device_hardware === 'onewire_temp' || sensor.device_hardware === 'onewire_2413'">{{ sensor.address }}</div>
                        <div class="text-gray-900" v-if="sensor.device_hardware === 'inkbird_bluetooth'">
                          <span v-if="BrewPiSensorStore.lowerMacAddressInDevices(sensor.address)">{{BrewPiSensorStore.getLowerMacAddress(sensor.address)}}</span>
                          <span v-else>{{ sensor.address }}</span>
                        </div>
                        <div class="text-gray-500" v-if="sensor.device_hardware === 'inkbird_bluetooth' && BrewPiSensorStore.lowerMacAddressInDevices(sensor.address)">
                          ({{ $t('sensors.inkbird_external_probe') }})
                        </div>
                        <div class="text-gray-900" v-if="sensor.device_hardware === 'tplink_switch' || sensor.device_hardware === 'tilt'"><!-- TODO - Figure out how to internationalize alias here -->{{ sensor.device_alias }}</div>
                        <div class="text-gray-500" v-if="sensor.device_hardware === 'tplink_switch'">{{ sensor.address }}</div>
                      </td>
                      <td class="whitespace-nowrap px-3 py-4 text-sm text-gray-500">
                        {{ $t("sitewide.brewpi_device_functions." + deviceFunctionLabel(sensor.device_function, ExtendedSettingsStore.glycol)) }}
                      </td>
                      <td class="relative whitespace-nowrap py-4 pl-3 pr-6 text-right text-sm font-medium">
                        <AssignSensorModal :sensor="sensor" v-on:device-updated="delayedRefreshDevices" />
                      </td>
                    </tr>
                    </tbody>
                  </table>
                </div>
              </div>
            </div>
          </div>
        </div>


      </div>
    </div>
  </div>
</template>

<script>
import { useBrewPiSensorStore } from "@/stores/BrewPiSensorStore";
import { useExtendedSettingsStore } from "@/stores/ExtendedSettingsStore";
import AssignSensorModal from "@/components/sensors/AssignSensorModal.vue";
import { ExclamationTriangleIcon, InformationCircleIcon } from '@heroicons/vue/24/outline'
import { ref } from "vue";
import { deviceFunctionLabel } from "@/mixins/BrewPiSensor";

export default {
  name: "ConfigSensorsActuators",
  components: {
    AssignSensorModal,
    ExclamationTriangleIcon,
    InformationCircleIcon,
  },
  mounted() {
    // Retrieve initial data -- extended settings first so we don't display erroneous warnings
    this.ExtendedSettingsStore.getExtendedSettings().then((response) => {
      this.BrewPiSensorStore.getDevices().then((response) => {
        this.updateDeviceFunctions();
      });
    });
  },
  setup() {
    const hasFridgeSensor = ref(false);
    const hasBeerSensor = ref(false);
    const hasHeatActuator = ref(false);
    const hasCoolActuator = ref(false);

    return {
      BrewPiSensorStore: useBrewPiSensorStore(),  // Updated in ConfigSensorsActuators.vue
      ExtendedSettingsStore: useExtendedSettingsStore(),
      deviceFunctionLabel,
      hasFridgeSensor,
      hasBeerSensor,
      hasHeatActuator,
      hasCoolActuator,
    }
  },
  methods: {
    delayedRefreshDevices: async function() {
      let loader = this.$loading.show({});

      // Delay 2 seconds before executing the below to let the device store update on the controller
      // (this is in addition to the 3 second delay in displaying the "success" message)
      await new Promise(resolve => setTimeout(resolve, 2000));
      await this.BrewPiSensorStore.clearDevices();
      await this.BrewPiSensorStore.getDevices();
      this.updateDeviceFunctions();
      loader.hide();

    },

    refreshDevices: async function() {
      let loader = this.$loading.show({});
      await this.BrewPiSensorStore.clearDevices();
      await this.BrewPiSensorStore.getDevices();
      this.updateDeviceFunctions();
      loader.hide();
    },
    updateDeviceFunctions: function() {
      this.hasBeerSensor = this.BrewPiSensorStore.hasDeviceWithFunction(9);
      this.hasFridgeSensor = this.BrewPiSensorStore.hasDeviceWithFunction(5);
      this.hasHeatActuator = this.BrewPiSensorStore.hasDeviceWithFunction(2);
      this.hasCoolActuator = this.BrewPiSensorStore.hasDeviceWithFunction(3);
    }
  }
}
</script>

<style scoped>

</style>