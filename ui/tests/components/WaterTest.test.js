import { createPinia, setActivePinia } from 'pinia';
import { createRenderer, nextTick } from 'vue';
import { jest } from '@jest/globals';
import WaterTest from '@/components/WaterTest.vue';
import { useWaterTestStore } from '@/stores/WaterTestStore';
import { useTempControlStore } from '@/stores/TempControlStore';
import fixture from '../stores/fixtures/api.water-test.json';

// No DOM is needed to exercise the real setup script, lifecycle hooks, and
// reactive stores. In particular, mounting must happen before preferences load.
const renderer = createRenderer({
    createComment: () => ({}),
    insert: () => {},
    remove: () => {},
    parentNode: () => null,
    nextSibling: () => null,
});
const flush = async () => {
    for (let i = 0; i < 8; i++) await Promise.resolve();
    await nextTick();
};

describe('water-test page lifecycle', () => {
    const originalWindow = global.window;
    let app;
    let waterTest;
    let tempControl;
    let refresh;

    function mount() {
        app = renderer.createApp({ ...WaterTest, render: () => null });
        app.mount({});
        return app._instance.setupState;
    }

    beforeEach(() => {
        jest.useFakeTimers();
        global.window = global;
        setActivePinia(createPinia());
        waterTest = useWaterTestStore();
        tempControl = useTempControlStore();
        refresh = jest.spyOn(waterTest, 'refresh').mockResolvedValue(fixture);
    });
    afterEach(() => {
        if (app) app.unmount();
        app = null;
        global.window = originalWindow;
        jest.useRealTimers();
    });

    it('updates live test temperatures when the controller preference arrives after mounting', async () => {
        waterTest.status = { ...fixture, active: true, control_owned: true, can_start: false };
        const page = mount();
        expect(page.displayTemperature(20)).toBe('68.00 °F');
        tempControl.tempFormat = 'C';
        await nextTick();
        expect(page.form.temperatureUnit).toBe('C');
        expect(page.displayTemperature(20)).toBe('20.00 °C');
        await flush();
    });

    it('uses an already loaded preference on mount and ignores temporarily unavailable preferences', async () => {
        tempControl.tempFormat = 'C';
        const page = mount();
        expect(page.form.temperatureUnit).toBe('C');
        tempControl.tempFormat = 'X';
        await nextTick();
        expect(page.form.temperatureUnit).toBe('C');
        await flush();
    });

    it('converts an entered setpoint when applying a late preference', async () => {
        const page = mount();
        page.form.glycolSetpoint = 41;
        tempControl.tempFormat = 'C';
        await nextTick();
        expect(page.form.glycolSetpoint).toBeCloseTo(5);
        await flush();
    });

    it('preserves an explicit selection even when it matches the initial default', async () => {
        const page = mount();
        page.form.glycolSetpoint = 41;
        page.changeTemperatureUnit('F');
        tempControl.tempFormat = 'C';
        await nextTick();
        expect(page.form.temperatureUnit).toBe('F');
        expect(page.form.glycolSetpoint).toBeCloseTo(41);
        page.changeTemperatureUnit('C');
        expect(page.form.glycolSetpoint).toBeCloseTo(5);
        tempControl.tempFormat = 'F';
        await nextTick();
        expect(page.form.temperatureUnit).toBe('C');
        await flush();
    });

    it('polls after each response and clears the timer on navigation away', async () => {
        mount();
        expect(refresh).toHaveBeenCalledTimes(1);
        await flush();
        expect(jest.getTimerCount()).toBe(1);
        await jest.advanceTimersByTimeAsync(2000);
        expect(refresh).toHaveBeenCalledTimes(2);
        app.unmount();
        app = null;
        expect(jest.getTimerCount()).toBe(0);
        await jest.advanceTimersByTimeAsync(10000);
        expect(refresh).toHaveBeenCalledTimes(2);
    });

    it('shares an in-flight refresh and never rearms polling after unmounting during a request', async () => {
        let resolve;
        refresh.mockReturnValue(new Promise(done => { resolve = done; }));
        const page = mount();
        page.refreshStatus();
        expect(refresh).toHaveBeenCalledTimes(1);
        app.unmount();
        app = null;
        resolve(fixture);
        await flush();
        expect(jest.getTimerCount()).toBe(0);
        await jest.advanceTimersByTimeAsync(10000);
        expect(refresh).toHaveBeenCalledTimes(1);
    });

    it('continues polling after a temporary connection failure', async () => {
        refresh.mockRejectedValueOnce(new Error('offline'));
        mount();
        await flush();
        expect(jest.getTimerCount()).toBe(1);
        await jest.advanceTimersByTimeAsync(2000);
        expect(refresh).toHaveBeenCalledTimes(2);
    });
});
