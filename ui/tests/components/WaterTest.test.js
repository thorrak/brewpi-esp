import { createPinia, setActivePinia } from 'pinia';
import { createRenderer, createSSRApp, nextTick } from 'vue';
import { renderToString } from '@vue/server-renderer';
import { jest } from '@jest/globals';
import WaterTest from '@/components/WaterTest.vue';
import { useWaterTestStore } from '@/stores/WaterTestStore';
import { useTempControlStore } from '@/stores/TempControlStore';
import fixture from '../stores/fixtures/api.water-test.json';
import { i18n } from '@/i18n';

// No DOM is needed to exercise the real setup script, lifecycle hooks, and
// reactive stores. In particular, mounting must happen before preferences load.
const renderer = createRenderer({
    createComment: () => ({}),
    insert: () => {},
    remove: () => {},
    parentNode: () => null,
    nextSibling: () => null,
});

describe('water-test setup and resume UI', () => {
    let pinia;
    let waterTest;

    beforeEach(() => {
        pinia = createPinia();
        setActivePinia(pinia);
        waterTest = useWaterTestStore();
        waterTest.status = JSON.parse(JSON.stringify(fixture));
    });

    function render(glycolChoice = '') {
        const page = {
            ...WaterTest,
            setup(props, context) {
                const state = WaterTest.setup(props, context);
                state.form.glycolChoice = glycolChoice;
                return state;
            },
        };
        return renderToString(createSSRApp(page).use(pinia).use(i18n));
    }

    it.each([
        [false, true], [true, false], [false, false],
    ])('hides the survey with beer probe configured=%s and cooling relay available=%s', async (beer, cooler) => {
        waterTest.status.preflight.beer_configured = beer;
        waterTest.status.preflight.beer_available = beer;
        waterTest.status.preflight.cooler_available = cooler;
        waterTest.status.can_start = false;
        const html = await render();
        expect(html).toContain('Prepare once, then let the test run');
        expect(html).not.toContain('<fieldset');
        expect(html).not.toContain('id="water-volume"');
        expect(html).toContain('notice-warning');
        expect(html).toContain('role="alert"');
        expect(html.includes('Configure a DS18B20 beer probe before starting.')).toBe(!beer);
        expect(html.includes('Configure a Cooling Switch/Relay before starting.')).toBe(!cooler);
    });

    it('shows the survey and decimal inputs once both required devices are available', async () => {
        const html = await render();
        expect(html).toContain('<fieldset');
        expect(html).toContain('id="fermenter-capacity"');
        expect(html).toContain('id="water-volume"');
        expect(html.match(/step="any"/g)).toHaveLength(2);
        expect(html).not.toContain('notice-warning');
        expect(html).toContain('10 seconds');
        expect(html).toContain('5–60 seconds');
    });

    it('keeps the survey visible but disabled for other preflight restrictions', async () => {
        waterTest.status.can_start = false;
        waterTest.status.preflight.reason = 'Waiting for normal outputs to turn off.';
        const html = await render();
        expect(html).toMatch(/<fieldset[^>]*disabled/);
        expect(html).toContain(waterTest.status.preflight.reason);
    });

    it('shows the reading warning for a configured but stale beer probe', async () => {
        waterTest.status.preflight.beer_available = false;
        waterTest.status.can_start = false;
        waterTest.status.preflight.reason = 'Waiting for a fresh valid beer probe reading.';
        const html = await render();
        expect(html).toMatch(/<fieldset[^>]*disabled/);
        expect(html).toContain(waterTest.status.preflight.reason);
        expect(html).not.toContain('Configure a DS18B20 beer probe before starting.');
    });

    it('selects the configured glycol probe without a placement prompt', async () => {
        const html = await render('chamber_probe');
        expect(html).toContain('Yes, use my glycol probe to measure the bath');
        expect(html).not.toContain('Move the chamber probe');
        expect(html).not.toContain('probe is now immersed');
        expect(html.match(/type="checkbox"/g)).toHaveLength(2);
    });

    it.each([
        ['failed', 1], ['inconclusive', 3], ['stopped', 0],
    ])('explains why %s tests are not submitted without linking to a current result', async (outcome, completedPulses) => {
        Object.assign(waterTest.status, {
            test_id: 'test-not-submitted', phase: 'finished', outcome,
            completed_pulses: completedPulses, upload_status: 'not_submitted',
            control_owned: true, can_resume: true, can_start: false,
        });
        const html = await render();
        expect(html).toContain('Data submission: Not submitted');
        expect(html).toContain('Failed or inconclusive tests and tests stopped before a full pump run are not submitted.');
        expect(html).not.toContain(`href="${fixture.result_url}"`);
        expect(html).not.toContain('It will retry automatically');
    });

    it.each(['completed', 'stopped'])('keeps automatic upload information and result links for eligible %s tests', async (outcome) => {
        Object.assign(waterTest.status, {
            test_id: 'test-eligible', phase: 'finished', outcome, completed_pulses: 1,
            upload_status: 'pending', control_owned: true, can_resume: true, can_start: false,
        });
        const pending = await render();
        expect(pending).toContain('Data submission: Upload pending');
        expect(pending).toContain('It will retry automatically');
        expect(pending).toContain(`href="${fixture.result_url}"`);
        waterTest.status.upload_status = 'submitted';
        const submitted = await render();
        expect(submitted).toContain('Data submission: Test submitted');
        expect(submitted).toContain('Processing may take a little longer.');
        expect(submitted).not.toContain('A stopped or inconclusive test can still be submitted successfully.');
    });

    it('allows a new survey after resuming an unsubmitted test without suggesting a nonexistent result', async () => {
        Object.assign(waterTest.status, {
            test_id: 'test-not-submitted', phase: 'finished', outcome: 'stopped',
            completed_pulses: 0, upload_status: 'not_submitted',
            control_owned: false, can_resume: false, can_start: true,
        });
        const html = await render();
        expect(html).toContain('Normal control has been restored using the saved mode and setpoints.');
        expect(html).toMatch(/<fieldset(?![^>]*disabled)[^>]*>/);
        expect(html).not.toContain(`href="${fixture.result_url}"`);
        expect(html).not.toContain('Use the submission link below');
    });

    it.each(['pending', 'submitted', 'not_submitted'])('allows resume without a probe-return prompt when upload is %s', async (upload) => {
        waterTest.status.control_owned = true;
        waterTest.status.can_resume = true;
        waterTest.status.can_start = false;
        waterTest.status.moved_chamber_probe = true;
        waterTest.status.upload_status = upload;
        const html = await render();
        expect(html).toMatch(/<button(?![^>]*disabled)[^>]*>Resume saved temperature control<\/button>/);
        expect(html).not.toContain('type="checkbox"');
        expect(html).not.toContain('returned the chamber probe');
    });
});
const flush = async () => {
    for (let i = 0; i < 8; i++) await Promise.resolve();
    await nextTick();
};

describe('water-test page lifecycle', () => {
    const originalWindow = global.window;
    const originalFetch = global.fetch;
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
        global.fetch = originalFetch;
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

    it('starts using the configured glycol probe without a placement confirmation', async () => {
        global.fetch = jest.fn().mockResolvedValue({ ok: true, json: async () => ({ status: true }) });
        const page = mount();
        await flush();
        Object.assign(page.form, {
            capacity: 7.25, waterVolume: 5.5, coolingType: 'immersion_coil',
            probeMounting: 'thermowell', glycolChoice: 'chamber_probe',
            consent: true, waterConfirmed: true,
        });
        await page.startTest();
        expect(global.fetch).toHaveBeenCalledTimes(1);
        const [url, options] = global.fetch.mock.calls[0];
        expect(url).toBe('/api/water-test/start/');
        const body = JSON.parse(options.body);
        expect(body.glycol_temperature_source).toBe('chamber_probe');
        expect(body).not.toHaveProperty('bath_placement_confirmed');
        expect(page.actionError).toBe('');
        expect(page.busy).toBe('');
        expect(refresh).toHaveBeenCalledTimes(2);
    });

    it('resumes control without a probe-return confirmation', async () => {
        global.fetch = jest.fn().mockResolvedValue({ ok: true, json: async () => ({ status: true }) });
        const page = mount();
        await flush();
        await page.sendAction('resume');
        const [url, options] = global.fetch.mock.calls[0];
        expect(url).toBe('/api/water-test/resume/');
        expect(JSON.parse(options.body)).toEqual({});
        expect(page.actionError).toBe('');
        expect(refresh).toHaveBeenCalledTimes(2);
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
