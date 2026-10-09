import { createPinia, setActivePinia } from 'pinia';
import { createRenderer, nextTick } from 'vue';
import { jest } from '@jest/globals';
import App from '@/App.vue';
import { useTempControlStore } from '@/stores/TempControlStore';
import { useExtendedSettingsStore } from '@/stores/ExtendedSettingsStore';
import fixture from '../stores/fixtures/api.all_temp_control.json';

const renderer = createRenderer({
    createComment: () => ({}),
    insert: () => {},
    remove: () => {},
    parentNode: () => null,
    nextSibling: () => null,
});
const reply = () => ({ status: 200, json: async () => fixture });
const flush = async () => {
    for (let i = 0; i < 12; i++) await Promise.resolve();
    await nextTick();
};

describe('application temperature polling', () => {
    const originalFetch = global.fetch;
    let app;
    let store;

    function mount() {
        app = renderer.createApp({ ...App, render: () => null });
        app.mount({});
    }

    beforeEach(() => {
        jest.useFakeTimers();
        setActivePinia(createPinia());
        store = useTempControlStore();
        jest.spyOn(useExtendedSettingsStore(), 'getExtendedSettings').mockResolvedValue();
        global.fetch = jest.fn().mockResolvedValue(reply());
    });

    afterEach(async () => {
        if (app) app.unmount();
        app = null;
        await flush();
        global.fetch = originalFetch;
        jest.restoreAllMocks();
        jest.useRealTimers();
    });

    it('loads immediately and waits seven seconds after each response', async () => {
        let resolve;
        global.fetch.mockImplementationOnce(() => new Promise(done => { resolve = done; }));
        mount();
        expect(global.fetch).toHaveBeenCalledTimes(1);
        expect(global.fetch).toHaveBeenCalledWith('/api/all_temp_control/', expect.objectContaining({
            method: 'GET', signal: expect.any(AbortSignal),
        }));
        await jest.advanceTimersByTimeAsync(7000);
        expect(global.fetch).toHaveBeenCalledTimes(1);
        resolve(reply());
        await flush();
        expect(store.hasTempInfo).toBe(true);
        await jest.advanceTimersByTimeAsync(6999);
        expect(global.fetch).toHaveBeenCalledTimes(1);
        await jest.advanceTimersByTimeAsync(1);
        expect(global.fetch).toHaveBeenCalledTimes(2);
    });

    it('aborts a stalled request after eight seconds and retries without overlap', async () => {
        let active = 0;
        let peak = 0;
        global.fetch.mockImplementation((url, { signal }) => new Promise((resolve, reject) => {
            active++;
            peak = Math.max(peak, active);
            signal.addEventListener('abort', () => {
                active--;
                reject(Object.assign(new Error('Aborted'), { name: 'AbortError' }));
            }, { once: true });
        }));
        mount();
        const firstSignal = global.fetch.mock.calls[0][1].signal;
        await jest.advanceTimersByTimeAsync(7999);
        expect(firstSignal.aborted).toBe(false);
        expect(global.fetch).toHaveBeenCalledTimes(1);
        await jest.advanceTimersByTimeAsync(1);
        expect(firstSignal.aborted).toBe(true);
        expect(store.tempInfoError).toBe(true);
        expect(active).toBe(0);
        await jest.advanceTimersByTimeAsync(7000);
        expect(global.fetch).toHaveBeenCalledTimes(2);
        expect(peak).toBe(1);
    });

    it('cancels an in-flight request and every timer when unmounted', async () => {
        global.fetch.mockImplementation((url, { signal }) => new Promise((resolve, reject) => {
            signal.addEventListener('abort', () => {
                reject(Object.assign(new Error('Aborted'), { name: 'AbortError' }));
            }, { once: true });
        }));
        mount();
        const signal = global.fetch.mock.calls[0][1].signal;
        app.unmount();
        app = null;
        expect(signal.aborted).toBe(true);
        await flush();
        expect(jest.getTimerCount()).toBe(0);
        await jest.advanceTimersByTimeAsync(30000);
        expect(global.fetch).toHaveBeenCalledTimes(1);
    });

    it('clears a scheduled refresh when unmounted', async () => {
        mount();
        await flush();
        expect(jest.getTimerCount()).toBe(1);
        app.unmount();
        app = null;
        expect(jest.getTimerCount()).toBe(0);
        await jest.advanceTimersByTimeAsync(30000);
        expect(global.fetch).toHaveBeenCalledTimes(1);
    });

    it('continues polling after a network failure', async () => {
        global.fetch.mockRejectedValueOnce(new Error('Offline'));
        mount();
        await flush();
        expect(store.tempInfoError).toBe(true);
        await jest.advanceTimersByTimeAsync(7000);
        expect(global.fetch).toHaveBeenCalledTimes(2);
        expect(store.hasTempInfo).toBe(true);
        expect(store.tempInfoError).toBe(false);
    });

    it('keeps an explicit foreground refresh immediate between polls', async () => {
        mount();
        await flush();
        await store.getTempInfo();
        expect(global.fetch).toHaveBeenCalledTimes(2);
        expect(store.hasTempInfo).toBe(true);
        await jest.advanceTimersByTimeAsync(7000);
        expect(global.fetch).toHaveBeenCalledTimes(3);
    });
});
