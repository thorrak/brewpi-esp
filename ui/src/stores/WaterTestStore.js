import { defineStore } from 'pinia';
import { ref } from 'vue';
import { genCSRFOptions } from './CSRF';

// This timeout only bounds a browser request. All test and relay timing belongs
// to the device, including when this page is closed or loses its connection.
export async function requestWaterTest(path = '', body) {
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 8000);
    try {
        const response = await fetch(`/api/water-test/${path}`, {
            method: body === undefined ? 'GET' : 'POST',
            headers: { ...genCSRFOptions().headers, 'Content-Type': 'application/json' },
            cache: 'no-store',
            signal: controller.signal,
            ...(body === undefined ? {} : { body: JSON.stringify(body) }),
        });
        const payload = await response.json();
        if (!response.ok || payload.status === false) {
            throw new Error(payload.error || payload.reason || `Device request failed (${response.status}).`);
        }
        return payload;
    } catch (error) {
        if (error.name === 'AbortError') {
            throw new Error('The device did not reply. The test may still be running; reconnect to check its state.');
        }
        throw error;
    } finally {
        clearTimeout(timeout);
    }
}

export const useWaterTestStore = defineStore('WaterTestStore', () => {
    const status = ref(null);
    const connectionError = ref('');
    const lastUpdated = ref(null);

    async function refresh() {
        try {
            const response = await requestWaterTest();
            if (!response || typeof response.active !== 'boolean' || typeof response.can_start !== 'boolean') {
                throw new Error('The device returned an unreadable water-test status.');
            }
            status.value = response;
            connectionError.value = '';
            lastUpdated.value = Date.now();
            return response;
        } catch (error) {
            // Preserve the last known run so Stop remains accessible after a
            // temporary connection loss. Never display stale values as live.
            connectionError.value = error.message || 'Unable to reach the device.';
            throw error;
        }
    }

    return { status, connectionError, lastUpdated, refresh };
});
