import { defineStore } from 'pinia';
import { mande } from 'mande'
import { genCSRFOptions } from './CSRF';
import { ref } from 'vue';

export const ft_net_host = "www.fermentrack.net";
export const ft_net_port = 80;

export const useUpstreamSettingsStore = defineStore("UpstreamSettingsStore", () => {
    const hasUpstreamSettings = ref(false);
    const upstreamSettingsError = ref(false);
    const loadedUpstreamSettingsFromDevice = ref(false);
    const awaitingRegistration = ref(false);
    const upstreamHost = ref("");
    const upstreamPort = ref(0);
    const username = ref("");
    const apiKey = ref("");
    const guid = ref("");
    const upstreamRegistrationError = ref(7);
    const deviceID = ref("");

    async function getUpstreamSettings() {
        try {
            const remote_api = mande("/api/upstream/", genCSRFOptions());
            const response = await remote_api.get();
            if (response) {
                hasUpstreamSettings.value = true;
                upstreamSettingsError.value = false;
                upstreamHost.value = response.upstreamHost;
                upstreamPort.value = response.upstreamPort;
                deviceID.value = response.deviceID;
                username.value = response.username;
                apiKey.value = response.apiKey;
                guid.value = response.guid;
                upstreamRegistrationError.value = response.upstreamRegistrationError;

                // Since the form edits the store directly, cache if we loaded the settings from the device (so we
                // know if there are settings only because the user is typing them in)
                loadedUpstreamSettingsFromDevice.value = response.upstreamHost.length > 0;

            } else {
                await clearUpstreamSettings();
                upstreamSettingsError.value = true;
            }
        } catch (error) {
            await clearUpstreamSettings();
            upstreamSettingsError.value = true;
        }
    }

    async function clearUpstreamSettings() {
        hasUpstreamSettings.value = false;
        awaitingRegistration.value = false;
        upstreamHost.value = "";
        upstreamPort.value = 0;
        deviceID.value = "";
        username.value = "";
        apiKey.value = "";
        guid.value = "";
        upstreamRegistrationError.value = 7;
    }

    async function setUpstreamSettings(upstreamHostParam, upstreamPortParam, usernameParam, apiKeyParam) {
        try {
            const remote_api = mande("/api/upstream/", genCSRFOptions());
            const response = await remote_api.put({
                upstreamHost: upstreamHostParam,  // String
                upstreamPort: Number(upstreamPortParam),  // Integer
                username: usernameParam, // String
                // apiKey: apiKeyParam, // String
                // deviceID: deviceID, // Not processed in the firmware currently
            });
            if (response && response.status === 'ok') {
                upstreamSettingsError.value = false;
                // On success, update the store
                this.hasUpstreamSettings = true;
                this.upstreamHost = upstreamHostParam;
                this.upstreamPort = Number(upstreamPortParam);
                this.username = usernameParam;
                this.apiKey = apiKeyParam;

                // If we were unsetting the host or username, we aren't waiting for registration
                awaitingRegistration.value = (this.upstreamHost.length > 0 && this.username.length > 0);

            } else {
                await clearUpstreamSettings();
                upstreamSettingsError.value = true;
            }
        } catch (error) {
            await clearUpstreamSettings();
            upstreamSettingsError.value = true;
        }
    }

    return {
        hasUpstreamSettings,
        upstreamSettingsError,
        awaitingRegistration,
        loadedUpstreamSettingsFromDevice,
        upstreamHost,
        upstreamPort,
        username,
        apiKey,
        guid,
        upstreamRegistrationError,
        deviceID,

        getUpstreamSettings,
        clearUpstreamSettings,
        setUpstreamSettings };
});
