import { createPinia, setActivePinia } from 'pinia';
import { jest } from '@jest/globals';
import { requestWaterTest, useWaterTestStore } from '@/stores/WaterTestStore';

const status = { active: true, can_start: false, control_owned: true, phase: 'observe', device_guid: '0123456789ABCDEF' };
const reply = (payload, options = {}) => ({ ok: true, status: 200, json: async () => payload, ...options });

describe('water-test communication', () => {
    const originalFetch = global.fetch;
    beforeEach(() => {
        setActivePinia(createPinia());
        global.fetch = jest.fn();
    });
    afterEach(() => { global.fetch = originalFetch; });

    it('keeps an active test available after a failed poll so Stop does not disappear', async () => {
        global.fetch.mockResolvedValueOnce(reply(status)).mockRejectedValueOnce(new Error('Network unavailable'));
        const store = useWaterTestStore();
        await store.refresh();
        await expect(store.refresh()).rejects.toThrow('Network unavailable');
        expect(store.status.active).toBe(true);
        expect(store.connectionError).toBe('Network unavailable');
        global.fetch.mockResolvedValueOnce(reply({ ...status, active: false }));
        await store.refresh();
        expect(store.connectionError).toBe('');
        expect(store.status.active).toBe(false);
    });
    it('treats malformed successful responses as unknown state', async () => {
        global.fetch.mockResolvedValueOnce(reply({}));
        const store = useWaterTestStore();
        await expect(store.refresh()).rejects.toThrow('unreadable');
        expect(store.status).toBeNull();
    });
    it('posts commands to the device rather than scheduling outputs in the browser', async () => {
        global.fetch.mockResolvedValueOnce(reply({ status: true }, { status: 202 }));
        await requestWaterTest('stop/', {});
        expect(global.fetch).toHaveBeenCalledWith('/api/water-test/stop/', expect.objectContaining({ method: 'POST', body: '{}', cache: 'no-store' }));
    });
    it('surfaces backend preflight / ownership failures', async () => {
        global.fetch.mockResolvedValueOnce(reply({ status: false, error: 'Beer probe is not fresh.' }, { ok: false, status: 409 }));
        await expect(requestWaterTest('start/', { consent: true })).rejects.toThrow('Beer probe is not fresh.');
    });
    it('does not claim a timed-out command failed to execute', async () => {
        global.fetch.mockRejectedValueOnce(Object.assign(new Error('Aborted'), { name: 'AbortError' }));
        await expect(requestWaterTest('start/', {})).rejects.toThrow('test may still be running');
    });
});
