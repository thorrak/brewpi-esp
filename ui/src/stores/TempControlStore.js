import { defineStore } from 'pinia';
import { mande } from 'mande'
import { genCSRFOptions } from './CSRF';
import { ref } from 'vue';

export const useTempControlStore = defineStore("TempControlStore", () => {
    const hasTempInfo = ref(false);
    const cc = ref(null);
    const cv = ref(null);
    const cs = ref(null);
    const tempInfo = ref(null);
    const tempInfoError = ref(false);
    const fridgeTemp = ref(0);
    const beerTemp = ref(0);
    const roomTemp = ref(0);
    const tempFormat = ref("X");
    const controlMode = ref("o");
    const controlState = ref(0);
    const setModeError = ref(false);

    async function getTempInfo({ signal } = {}) {
        try {
            const remote_api = mande("/api/all_temp_control/", genCSRFOptions());
            const response = await remote_api.get({ signal });
            if (response && response.cc) {
                hasTempInfo.value = true;
                tempInfoError.value = false;
                cc.value = response.cc;
                cv.value = response.cv;
                cs.value = response.cs;
                tempInfo.value = response.temp;
                fridgeTemp.value = response.temp.FridgeTemp;
                beerTemp.value = response.temp.BeerTemp;
                roomTemp.value = response.temp.RoomTemp;
                tempFormat.value = response.cc.tempFormat;
                controlState.value = response.temp.State;
                controlMode.value = response.cs.mode;
            } else {
                await clearTempInfo();
                tempInfoError.value = true;
            }
        } catch (error) {
            await clearTempInfo();
            tempInfoError.value = true;
        }
    }

    async function clearTempInfo() {
        hasTempInfo.value = false;
        cc.value = null;
        cv.value = null;
        cs.value = null;
        tempInfo.value = null;
        fridgeTemp.value = 0;
        beerTemp.value = 0;
        roomTemp.value = 0;
        tempFormat.value = "X";
        controlState.value = 0;
        controlMode.value = "o";
    }

    async function setMode(new_mode, new_setpoint) {
        try {
            const remote_api = mande("/api/mode/", genCSRFOptions());
            const response = await remote_api.put({
                newMode: new_mode,  // Char (String)
                setPoint: new_setpoint,  // Double
            });
            if (response && response.status === 'ok') {
                setModeError.value = false;
            } else {
                await clearTempInfo();
                setModeError.value = true;
            }
        } catch (error) {
            // TODO - Raise error, or otherwise process
            await clearTempInfo();
            setModeError.value = true;
        }
        await getTempInfo();
    }

    return { hasTempInfo, cc, cv, cs, tempInfo, tempInfoError, fridgeTemp, beerTemp, roomTemp, tempFormat, controlMode, controlState, setModeError, getTempInfo, clearTempInfo, setMode };
});
