<template>
  <a href="#" @click="popModal()" class="text-indigo-600 hover:text-indigo-900">
    <span>{{ $t("dashboard.mode_change.change") }}</span>
    <span class="sr-only">{{ $t("dashboard.mode_change.mode") }}</span>
  </a>
  <TransitionRoot as="template" :show="isOpen">
    <Dialog as="div" class="fixed z-10 inset-0 overflow-y-auto" @close="isOpen = false">
      <div class="flex items-end justify-center min-h-screen pt-4 px-4 pb-20 text-center sm:block sm:p-0">
        <TransitionChild as="template" enter="ease-out duration-300" enter-from="opacity-0" enter-to="opacity-100" leave="ease-in duration-200" leave-from="opacity-100" leave-to="opacity-0">
          <DialogOverlay class="fixed inset-0 bg-gray-500 bg-opacity-75 transition-opacity" />
        </TransitionChild>

        <!-- This element is to trick the browser into centering the modal contents. -->
        <span class="hidden sm:inline-block sm:align-middle sm:h-screen" aria-hidden="true">&#8203;</span>
        <TransitionChild as="template" enter="ease-out duration-300" enter-from="opacity-0 translate-y-4 sm:translate-y-0 sm:scale-95" enter-to="opacity-100 translate-y-0 sm:scale-100" leave="ease-in duration-200" leave-from="opacity-100 translate-y-0 sm:scale-100" leave-to="opacity-0 translate-y-4 sm:translate-y-0 sm:scale-95">
          <div class="inline-block align-bottom bg-white rounded-lg text-left overflow-hidden shadow-xl transform transition-all sm:my-8 sm:align-middle sm:max-w-lg sm:w-full">
            <form @submit.prevent="submitForm">

              <div class="bg-white px-4 pt-5 pb-4 sm:p-6 sm:pb-4">
                <div class="sm:flex sm:items-start">
                  <div class="mx-auto flex-shrink-0 flex items-center justify-center h-12 w-12 rounded-full bg-blue-100 sm:mx-0 sm:h-10 sm:w-10">
                    <CogIcon class="h-6 w-6 text-blue-600" aria-hidden="true" />
                  </div>
                  <div class="mt-3 text-center sm:mt-0 sm:ml-4 sm:text-left">
                    <DialogTitle as="h3" class="text-lg leading-6 font-medium text-gray-900">
                      {{ $t("dashboard.mode_change.change_mode_header") }}
                    </DialogTitle>

                    <!-- Not using the listbox, as it gets hidden by the modal -->
                    <!--                    <Listbox as="div" v-model="new_function">-->
                    <!--                      <ListboxLabel class="block text-sm font-medium text-gray-700">Device Function</ListboxLabel>-->
                    <!--                      <div class="relative mt-1">-->
                    <!--                        <ListboxButton class="relative w-full cursor-default rounded-md border border-gray-300 bg-white py-2 pl-3 pr-10 text-left shadow-sm focus:border-indigo-500 focus:outline-none focus:ring-1 focus:ring-indigo-500 sm:text-sm">-->
                    <!--                          <span class="block truncate">{{ DeviceFunctions[new_function] }}</span>-->
                    <!--                          <span class="pointer-events-none absolute inset-y-0 right-0 flex items-center pr-2">-->
                    <!--                            <ChevronUpDownIcon class="h-5 w-5 text-gray-400" aria-hidden="true" />-->
                    <!--                          </span>-->
                    <!--                        </ListboxButton>-->

                    <!--                        <transition leave-active-class="transition ease-in duration-100" leave-from-class="opacity-100" leave-to-class="opacity-0">-->
                    <!--                          <ListboxOptions class="absolute z-10 mt-1 max-h-60 w-full overflow-auto rounded-md bg-white py-1 text-base shadow-lg ring-1 ring-black ring-opacity-5 focus:outline-none sm:text-sm">-->
                    <!--                            <ListboxOption as="template" v-for="valid_function in sensor.valid_functions()" :key="valid_function.id" :value="valid_function.id" v-slot="{ active, selected }">-->
                    <!--                              <li :class="[active ? 'text-white bg-indigo-600' : 'text-gray-900', 'relative cursor-default select-none py-2 pl-3 pr-9']">-->
                    <!--                                <span :class="[selected ? 'font-semibold' : 'font-normal', 'block truncate']">{{ valid_function.function_name }}</span>-->

                    <!--                                <span v-if="selected" :class="[active ? 'text-white' : 'text-indigo-600', 'absolute inset-y-0 right-0 flex items-center pr-4']">-->
                    <!--                                  <CheckIcon class="h-5 w-5" aria-hidden="true" />-->
                    <!--                                </span>-->
                    <!--                              </li>-->
                    <!--                            </ListboxOption>-->
                    <!--                          </ListboxOptions>-->
                    <!--                        </transition>-->
                    <!--                      </div>-->
                    <!--                    </Listbox>-->

                    <div>
                      <label for="device_mode" class="block text-sm font-medium text-gray-700 sr-only">{{ $t("dashboard.mode_change.mode") }}</label>
                      <select id="device_mode" name="device_mode" v-model="new_mode" class="mt-1 block w-full rounded-md border-gray-300 py-2 pl-3 pr-10 text-base focus:border-indigo-500 focus:outline-none focus:ring-indigo-500 sm:text-sm">
                        <option v-for="mode in modes" :key="mode.value" :value="mode.value">{{ mode.name }}</option>
                        <option value="p" v-if="old_mode === 'p'">{{ $t("sitewide.brewpi_modes.beer_profile") }}</option>
                      </select>
                    </div>

                    <div class="border-l-4 border-yellow-400 bg-yellow-50 p-4 mt-2 mb-2" v-if="old_mode === 'p' && new_mode !== 'p'">
                      <!-- If an existing device will get uninstalled here if the user continues, alert him/her -->
                      <div class="flex columns-2">
                        <div class="flex flex-shrink-0 align-middle justify-center items-center">
                          <ExclamationTriangleIcon class="h-5 w-5 text-yellow-400" aria-hidden="true" />
                        </div>
                        <div class="ml-3">
                          <p class="text-sm text-yellow-700 text-left">
                            {{ $t("dashboard.mode_change.beer_profile_switch_warning") }}
                          </p>
                        </div>
                      </div>
                    </div>


                    <!-- Set Point -->
                    <div class="relative border border-gray-300 rounded-md px-3 py-2 my-3 shadow-sm focus-within:ring-1 focus-within:ring-indigo-600 focus-within:border-indigo-600"
                         v-if="new_mode === 'b' || new_mode === 'f'">
                      <label for="set_point_ele" class="absolute -top-2 left-2 -mt-px inline-block px-1 bg-white text-xs font-medium text-gray-900">{{ $t("dashboard.mode_change.set_point_header") }}</label>
                      <input type="text" ref="set_point_ele" v-model="set_point" id="set_point_ele" class="border-0 p-0 text-gray-900 placeholder-gray-500 focus:ring-0 sm:text-sm flex-1" placeholder="62.0" />
                      <span class="flex-none w-auto min-w-max max-w-max">&deg; {{ TempControlStore.cc.tempFormat }}</span>
                    </div>

                  </div>
                </div>

                <div v-if="form_error_message" class="mt-3">
                  <!-- TODO - Make sure the below gets translated -->
                  <FormErrorMsg :form_error_message="form_error_message" />
                </div>

              </div>

              <div class="bg-gray-50 px-4 py-3 sm:px-6 sm:flex sm:flex-row-reverse">
                <button type="submit" class="w-full inline-flex justify-center rounded-md border border-transparent shadow-sm px-4 py-2 bg-blue-600 text-base font-medium text-white hover:bg-blue-700 focus:outline-none focus:ring-2 focus:ring-offset-2 focus:ring-blue-500 sm:ml-3 sm:w-auto sm:text-sm">
                  {{ $t("dashboard.mode_change.set_mode") }}
                </button>
                <button type="button" class="mt-3 w-full inline-flex justify-center rounded-md border border-gray-300 shadow-sm px-4 py-2 bg-white text-base font-medium text-gray-700 hover:bg-gray-50 focus:outline-none focus:ring-2 focus:ring-offset-2 focus:ring-indigo-500 sm:mt-0 sm:ml-3 sm:w-auto sm:text-sm" @click="isOpen = false" ref="cancelButtonRef">
                  {{ $t("sitewide.cancel") }}
                </button>
              </div>
            </form>
          </div>
        </TransitionChild>
      </div>
    </Dialog>
  </TransitionRoot>

  <TransitionRoot as="template" :show="alertOpen">
    <Dialog as="div" class="fixed z-10 inset-0 overflow-y-auto" @close="closeAlert()">
      <div class="flex items-end justify-center min-h-screen pt-4 px-4 pb-20 text-center sm:block sm:p-0">
        <TransitionChild as="template" enter="ease-out duration-300" enter-from="opacity-0" enter-to="opacity-100" leave="ease-in duration-200" leave-from="opacity-100" leave-to="opacity-0">
          <DialogOverlay class="fixed inset-0 bg-gray-500 bg-opacity-75 transition-opacity" />
        </TransitionChild>

        <!-- This element is to trick the browser into centering the modal contents. -->
        <span class="hidden sm:inline-block sm:align-middle sm:h-screen" aria-hidden="true">&#8203;</span>
        <TransitionChild as="template" enter="ease-out duration-300" enter-from="opacity-0 translate-y-4 sm:translate-y-0 sm:scale-95" enter-to="opacity-100 translate-y-0 sm:scale-100" leave="ease-in duration-200" leave-from="opacity-100 translate-y-0 sm:scale-100" leave-to="opacity-0 translate-y-4 sm:translate-y-0 sm:scale-95">
          <div class="inline-block align-bottom bg-white rounded-lg px-4 pt-5 pb-4 text-left overflow-hidden shadow-xl transform transition-all sm:my-8 sm:align-middle sm:max-w-sm sm:w-full sm:p-6">
            <div v-if="TempControlStore.setModeError">
              <div class="mx-auto flex items-center justify-center h-12 w-12 rounded-full bg-red-100">
                <NoSymbolIcon class="h-6 w-6 text-red-600" aria-hidden="true" />
              </div>
              <div class="mt-3 text-center sm:mt-5">
                <DialogTitle as="h3" class="text-lg leading-6 font-medium text-gray-900">
                  {{ $t("dashboard.mode_change.update_failed") }}
                </DialogTitle>
                <div class="mt-2">
                  <p class="text-sm text-gray-500">
                    {{ $t("dashboard.mode_change.update_failed_msg") }}
                  </p>
                </div>
              </div>
            </div>
            <div v-else>
              <div class="mx-auto flex items-center justify-center h-12 w-12 rounded-full bg-green-100">
                <CheckIcon class="h-6 w-6 text-green-600" aria-hidden="true" />
              </div>
              <div class="mt-3 text-center sm:mt-5">
                <DialogTitle as="h3" class="text-lg leading-6 font-medium text-gray-900">
                  {{ $t("dashboard.mode_change.update_succeeded") }}
                </DialogTitle>
                <div class="mt-2">
                  <p class="text-sm text-gray-500">
                    {{ $t("dashboard.mode_change.update_succeeded_msg") }}
                  </p>
                </div>
              </div>
            </div>

            <div class="mt-5 sm:mt-6">
              <button type="button" class="inline-flex justify-center w-full rounded-md border border-transparent shadow-sm px-4 py-2 bg-indigo-600 text-base font-medium text-white hover:bg-indigo-700 focus:outline-none focus:ring-2 focus:ring-offset-2 focus:ring-indigo-500 sm:text-sm" @click="closeAlert()">
                {{ $t("sitewide.close") }}
              </button>
            </div>
          </div>
        </TransitionChild>
      </div>
    </Dialog>
  </TransitionRoot>

</template>

<script setup>
import { ref, computed } from 'vue'
import { i18n } from "@/i18n";
import {
  Dialog,
  DialogOverlay,
  DialogTitle,
  TransitionChild,
  TransitionRoot,
} from '@headlessui/vue'
import {
  CheckIcon,
  NoSymbolIcon,
  CogIcon,
  ExclamationTriangleIcon,
} from '@heroicons/vue/24/outline'
import FormErrorMsg from "@/components/generic/FormErrorMsg.vue";
import { useTempControlStore } from "@/stores/TempControlStore";
import { useExtendedSettingsStore } from "@/stores/ExtendedSettingsStore";
import {useLoading} from "vue-loading-overlay";

const ExtendedSettingsStore = useExtendedSettingsStore();

const modes = computed(() => {
  const availableModes = [
    { name: i18n.global.t('sitewide.brewpi_modes.off'), value: 'o' },
    { name: i18n.global.t('sitewide.brewpi_modes.beer_constant'), value: 'b' },
  ];
  // Fridge Constant is not available in glycol mode
  if (!ExtendedSettingsStore.glycol) {
    availableModes.push({ name: i18n.global.t('sitewide.brewpi_modes.fridge_constant'), value: 'f' });
  }
  return availableModes;
})

const isOpen = ref(false);
const alertOpen = ref(false);
const updateSuccessful = ref(false);
const TempControlStore = useTempControlStore();  // Updated in App.vue
let new_mode =  ref('o');
let old_mode = ref('o');
let set_point = ref(0.0);
let form_error_message = ref("");

const $loading = useLoading({});


// submit the form to our backend api
async function submitForm() {
  let setpoint_sendable = parseFloat(set_point.value);

  // Validate the information in the form
  form_error_message.value = "";

  if(isNaN(setpoint_sendable)) {
    form_error_message.value = `Setpoint must be a number`;
    return;
  }

  if (new_mode.value !== 'b' && new_mode.value !== 'p' && new_mode.value !== 'f') {
    setpoint_sendable = 0.0;
  }

  isOpen.value = false;
  let loader = $loading.show({});
  await TempControlStore.setMode(new_mode.value, setpoint_sendable);
  loader.hide();
  updateSuccessful.value = !TempControlStore.setModeError;
  alertOpen.value = true;
}

function popModal() {
  // The prop keeps getting updated in the background - this prevents the data from changing after the form is
  // displayed
  // device.value = StructuredClone(this.$props.sensor);
  new_mode.value = TempControlStore.controlMode;
  old_mode.value = TempControlStore.controlMode;
  if (TempControlStore.controlMode === 'f')
    set_point.value = TempControlStore.tempInfo.FridgeSet;
  else if (TempControlStore.controlMode === 'b')
    set_point.value = TempControlStore.tempInfo.BeerSet;
  form_error_message.value = "";
  isOpen.value = true;
}

function closeAlert() {
  // There's almost certainly a better way to refactor all this, but for now, this works
  // When the user closes the "alert" popup, we need to close BOTH the alert (alertOpen) and the modal (isOpen)
  alertOpen.value = false;
}

</script>

<style scoped>

</style>