import { defineStore } from 'pinia';
import { mande } from 'mande';
import { genCSRFOptions } from './CSRF';
import { ref } from 'vue';

export const useExtendedSettingsStore = defineStore("ExtendedSettingsStore", () => {
    const hasExtendedSettings = ref(false);
    const extendedSettingsError = ref(false);
    const extendedSettingsUpdateError = ref(false);

    const glycol = ref(false);
    const hasGlycolCoolingAlgorithm = ref(false);
    const glycolCoolingAlgorithm = ref(null);
    const largeTFT = ref(false);
    const invertTFT = ref(false);
    const resetScreenOnPin = ref(false);

    const SETTINGS_CHOICE = ref(0);
    const MIN_COOL_OFF_TIME = ref(0);
    const MIN_HEAT_OFF_TIME = ref(0);
    const MIN_COOL_ON_TIME = ref(0);
    const MIN_HEAT_ON_TIME = ref(0);
    const MIN_COOL_OFF_TIME_FRIDGE_CONSTANT = ref(0);
    const MIN_SWITCH_TIME = ref(0);
    const COOL_PEAK_DETECT_TIME = ref(0);
    const HEAT_PEAK_DETECT_TIME = ref(0);

    async function getExtendedSettings() {
        try {
            const remote_api = mande("/api/extended/", genCSRFOptions());
            const response = await remote_api.get();
            if (response && response.extendedSettings) {
                hasExtendedSettings.value = true;
                extendedSettingsError.value = false;

                glycol.value = response.extendedSettings.glycol;
                const algorithm = response.extendedSettings.glycolCoolingAlgorithm;
                hasGlycolCoolingAlgorithm.value = isCoolingAlgorithm(algorithm);
                if (hasGlycolCoolingAlgorithm.value) {
                    glycolCoolingAlgorithm.value = algorithm;
                }
                largeTFT.value = response.extendedSettings.largeTFT;
                invertTFT.value = response.extendedSettings.invertTFT;
                resetScreenOnPin.value = response.extendedSettings.resetScreenOnPin;

                SETTINGS_CHOICE.value = response.minTimes.SETTINGS_CHOICE;
                MIN_COOL_OFF_TIME.value = response.minTimes.MIN_COOL_OFF_TIME;
                MIN_HEAT_OFF_TIME.value = response.minTimes.MIN_HEAT_OFF_TIME;
                MIN_COOL_ON_TIME.value = response.minTimes.MIN_COOL_ON_TIME;
                MIN_HEAT_ON_TIME.value = response.minTimes.MIN_HEAT_ON_TIME;
                MIN_COOL_OFF_TIME_FRIDGE_CONSTANT.value = response.minTimes.MIN_COOL_OFF_TIME_FRIDGE_CONSTANT;
                MIN_SWITCH_TIME.value = response.minTimes.MIN_SWITCH_TIME;
                COOL_PEAK_DETECT_TIME.value = response.minTimes.COOL_PEAK_DETECT_TIME;
                HEAT_PEAK_DETECT_TIME.value = response.minTimes.HEAT_PEAK_DETECT_TIME;
            } else {
                await clearExtendedSettings();
                extendedSettingsError.value = true;
            }
        } catch (error) {
            await clearExtendedSettings();
            extendedSettingsError.value = true;
        }
    }

    async function clearExtendedSettings() {
        hasExtendedSettings.value = false;
        glycol.value = false;
        hasGlycolCoolingAlgorithm.value = false;
        glycolCoolingAlgorithm.value = null;
        largeTFT.value = false;
        invertTFT.value = false;
        resetScreenOnPin.value = false;

        SETTINGS_CHOICE.value = 0;
        MIN_COOL_OFF_TIME.value = 0;
        MIN_HEAT_OFF_TIME.value = 0;
        MIN_COOL_ON_TIME.value = 0;
        MIN_HEAT_ON_TIME.value = 0;
        MIN_COOL_OFF_TIME_FRIDGE_CONSTANT.value = 0;
        MIN_SWITCH_TIME.value = 0;
        COOL_PEAK_DETECT_TIME.value = 0;
        HEAT_PEAK_DETECT_TIME.value = 0;
    }

    function isCoolingAlgorithm(value) {
        return value === 'predictive_coast' || value === 'pulse_dose';
    }

    async function setExtendedSettings(input) {
        try {
            const algorithm = input.glycolCoolingAlgorithm === undefined ? glycolCoolingAlgorithm.value : input.glycolCoolingAlgorithm;
            if ((hasGlycolCoolingAlgorithm.value && !isCoolingAlgorithm(algorithm)) ||
                (!hasGlycolCoolingAlgorithm.value && input.glycolCoolingAlgorithm !== undefined)) {
                extendedSettingsUpdateError.value = true;
                return false;
            }
            const remote_api = mande("/api/extended/", genCSRFOptions());
            const settings = {
                glycol: input.glycol,
                largeTFT: input.largeTFT,
                invertTFT: input.invertTFT,
                resetScreenOnPin: input.resetScreenOnPin,
                SETTINGS_CHOICE: input.SETTINGS_CHOICE,
                MIN_COOL_OFF_TIME: input.MIN_COOL_OFF_TIME,
                MIN_HEAT_OFF_TIME: input.MIN_HEAT_OFF_TIME,
                MIN_COOL_ON_TIME: input.MIN_COOL_ON_TIME,
                MIN_HEAT_ON_TIME: input.MIN_HEAT_ON_TIME,
                MIN_COOL_OFF_TIME_FRIDGE_CONSTANT: input.MIN_COOL_OFF_TIME_FRIDGE_CONSTANT,
                MIN_SWITCH_TIME: input.MIN_SWITCH_TIME,
                COOL_PEAK_DETECT_TIME: input.COOL_PEAK_DETECT_TIME,
                HEAT_PEAK_DETECT_TIME: input.HEAT_PEAK_DETECT_TIME,
            };
            if (hasGlycolCoolingAlgorithm.value) {
                settings.glycolCoolingAlgorithm = algorithm;
            }
            const response = await remote_api.put(settings);
            if (response && response.status === 'ok') {
                glycol.value = settings.glycol;
                if (hasGlycolCoolingAlgorithm.value) {
                    glycolCoolingAlgorithm.value = algorithm;
                }
                largeTFT.value = settings.largeTFT;
                invertTFT.value = settings.invertTFT;
                resetScreenOnPin.value = settings.resetScreenOnPin;
                SETTINGS_CHOICE.value = settings.SETTINGS_CHOICE;
                MIN_COOL_OFF_TIME.value = settings.MIN_COOL_OFF_TIME;
                MIN_HEAT_OFF_TIME.value = settings.MIN_HEAT_OFF_TIME;
                MIN_COOL_ON_TIME.value = settings.MIN_COOL_ON_TIME;
                MIN_HEAT_ON_TIME.value = settings.MIN_HEAT_ON_TIME;
                MIN_COOL_OFF_TIME_FRIDGE_CONSTANT.value = settings.MIN_COOL_OFF_TIME_FRIDGE_CONSTANT;
                MIN_SWITCH_TIME.value = settings.MIN_SWITCH_TIME;
                COOL_PEAK_DETECT_TIME.value = settings.COOL_PEAK_DETECT_TIME;
                HEAT_PEAK_DETECT_TIME.value = settings.HEAT_PEAK_DETECT_TIME;
                extendedSettingsUpdateError.value = false;
                return true;
            } else {
                extendedSettingsUpdateError.value = true;
                return false;
            }
        } catch (error) {
            extendedSettingsUpdateError.value = true;
            return false;
        }
    }

    return { hasExtendedSettings, extendedSettingsError, extendedSettingsUpdateError, glycol, hasGlycolCoolingAlgorithm, glycolCoolingAlgorithm, largeTFT, invertTFT, resetScreenOnPin, SETTINGS_CHOICE, MIN_COOL_OFF_TIME, MIN_HEAT_OFF_TIME, MIN_COOL_ON_TIME, MIN_HEAT_ON_TIME, MIN_COOL_OFF_TIME_FRIDGE_CONSTANT, MIN_SWITCH_TIME, COOL_PEAK_DETECT_TIME, HEAT_PEAK_DETECT_TIME, getExtendedSettings, clearExtendedSettings, setExtendedSettings };
});
