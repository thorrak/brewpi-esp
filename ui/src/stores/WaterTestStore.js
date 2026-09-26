import { i18n } from '@/i18n';

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
            throw new Error(payload.error || payload.reason || i18n.global.t('water_test.errors.request_failed', { status: response.status }));
        }
        return payload;
    } catch (error) {
        if (error.name === 'AbortError') {
            throw new Error(i18n.global.t('water_test.errors.timeout'));
        }
        throw error;
    } finally {
        clearTimeout(timeout);
    }
}

export const useWaterTestStore = defineStore('WaterTestStore', () => {
    const status = ref(null);
    const connectionError = ref('');

    async function refresh() {
        try {
            const response = await requestWaterTest();
            if (!response || typeof response.active !== 'boolean' || typeof response.can_start !== 'boolean') {
                throw new Error(i18n.global.t('water_test.errors.unreadable'));
            }
            status.value = response;
            connectionError.value = '';
            return response;
        } catch (error) {
            // Preserve the last known run so Stop remains accessible after a
            // temporary connection loss. Never display stale values as live.
            connectionError.value = error.message || i18n.global.t('water_test.errors.unreachable');
            throw error;
        }
    }

    return { status, connectionError, refresh };
});
