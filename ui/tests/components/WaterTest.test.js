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

    function render(glycolChoice = '', formValues = {}) {
        const page = {
            ...WaterTest,
            setup(props, context) {
                const state = WaterTest.setup(props, context);
                state.form.glycolChoice = glycolChoice;
                Object.assign(state.form, formValues);
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
        expect(html).toContain(i18n.global.t('water_test.prepare_title'));
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
        expect(html.match(/step="any"/g)).toHaveLength(3);
        expect(html).not.toContain('notice-warning');
        expect(html).toContain('10 seconds');
        expect(html).toContain('up to 30 minutes');
        expect(html).toContain('12-hour limit');
        expect(html).toContain('both BrewPi cooling algorithms in sequence');
        expect(html).toContain('three hours');
        expect(html).toContain('30 minutes of pump-off observation');
        expect(html).toContain('3 + 5/9 °C (6.4 °F)');
    });

    it.each([
        [1, 'predictive_coast', 'Predictive coast'],
        [2, 'pulse_dose', 'Pulse-dose'],
    ])('preserves legacy episode progress for controller run %s', async (run, algorithm, label) => {
        Object.assign(waterTest.status, {
            test_id: 'dual-test', active: true, phase: 'controller', controller_run: run,
            controller_run_count: 2, controller_algorithm: algorithm, controller_target_c: 20,
            controller_episodes_completed: 1, controller_episodes_required: 3,
            controller_waiting_for_rewarming: true, controller_remaining_s: 3661,
        });
        const html = await render();
        expect(html).toContain(`Controller run ${run} of 2: ${label}`);
        expect(html).toContain('Settled cooling episodes: 1 of 3 · target 68.00 °F');
        expect(html).toContain('waiting for natural warming');
        expect(html).toContain('Time remaining for this algorithm: 1h 1m 01s');
    });

    it.each([0, 3, 5])('shows fixed duration and observation coverage without a quality verdict (%s)', async (completed) => {
        Object.assign(waterTest.status, {
            test_id: 'v3-test', active: true, phase: 'controller', controller_run: 1,
            controller_run_count: 2, controller_algorithm: 'predictive_coast', controller_target_c: 20,
            controller_completion_policy: 'fixed_duration_v1', controller_elapsed_s: 3540,
            controller_duration_s: 10800, controller_remaining_s: 7260,
            controller_observations: { completed, goal: 3, rate_qualified: completed, time_limited: 0,
                rate_unqualified: 0, interrupted: 1 },
        });
        const html = await render();
        expect(html).toContain('Run time: 59m 00s of 3h 0m 00s · target 68.00 °F');
        expect(html).toContain(`Completed controller observations: ${completed} · coverage goal: 3`);
        expect(html).toContain('1 interrupted');
        expect(html).toContain('temperature-control quality is evaluated separately.');
        expect(html).not.toContain('Settled cooling episodes');
        expect(html).not.toContain('waiting for natural warming');
    });

    it('uses the reported duration when reading an older fixed-duration plan', async () => {
        Object.assign(waterTest.status, {
            test_id: 'v3-two-hour-test', active: true, phase: 'controller', controller_run: 1,
            controller_run_count: 2, controller_algorithm: 'predictive_coast', controller_target_c: 20,
            controller_completion_policy: 'fixed_duration_v1', controller_elapsed_s: 3540,
            controller_duration_s: 7200, controller_remaining_s: 3660,
            controller_observations: { completed: 1, goal: 3 },
        });
        const html = await render();
        expect(html).toContain('Run time: 59m 00s of 2h 0m 00s');
        expect(html).not.toContain('Scheduled duration: three hours.');
    });

    it('shows the current plan transition and its separate time allowance', async () => {
        Object.assign(waterTest.status, {
            test_id: 'dual-test', active: true, phase: 'controller_transition', controller_run: 1,
            controller_run_count: 2, controller_algorithm: 'pulse_dose', controller_target_c: 20,
            controller_completion_policy: 'fixed_duration_v1', controller_elapsed_s: 10800,
            controller_duration_s: 10800, controller_remaining_s: 1200,
            controller_observations: { completed: 2, goal: 3 },
        });
        const html = await render();
        expect(html).toContain('Pump off: waiting for cooling to settle before the second algorithm');
        expect(html).toContain('Controller run 1 of 2: Pulse-dose');
        expect(html).toContain('Time remaining to observe recovery: 20m 00s');
        expect(html).not.toContain('waiting for natural warming');
        expect(html).not.toContain('Time remaining for this algorithm');
    });

    it('shows final OFF recording without extending the algorithm countdown', async () => {
        Object.assign(waterTest.status, {
            test_id: 'dual-test', active: true, phase: 'controller_final_observe', controller_run: 2,
            controller_run_count: 2, controller_algorithm: 'pulse_dose', controller_target_c: 20,
            controller_completion_policy: 'fixed_duration_v1', controller_elapsed_s: 10800,
            controller_duration_s: 10800, controller_remaining_s: 160,
            controller_observations: { completed: 1, goal: 3 },
            controller_final_observation_remaining_s: 70,
            controller_final_observation_valid_samples: 4, controller_final_observation_required_samples: 6,
        });
        const html = await render();
        expect(html).toContain('Pump off: saving final temperature readings');
        expect(html).toContain('Final recording: at least 1m 10s remaining · 4 of 6 fresh readings required');
        expect(html).toContain('The algorithm has stopped and the pump stays off.');
        expect(html).not.toContain('waiting for natural warming');
        expect(html).not.toContain('Time remaining for this algorithm');
    });

    it('keeps legacy controller status readable without inventing a run number', async () => {
        Object.assign(waterTest.status, { test_id: 'old-test', active: true, phase: 'controller' });
        const html = await render();
        expect(html).toContain('Testing a cooling algorithm');
        expect(html).not.toContain('Controller run');
    });

    it.each([
        ['controller_transition_timeout', 'The second run was skipped; the first run is saved.'],
        ['controller_headroom', 'Recorded measurements are saved.'],
        ['controller_final_observation_timeout', 'The controller results and available readings are saved.'],
    ])('explains %s while preserving the result link', async (reason, message) => {
        Object.assign(waterTest.status, {
            test_id: 'dual-test', active: false, phase: 'finished', outcome: 'inconclusive', reason,
            upload_status: 'pending', control_owned: true,
        });
        const html = await render();
        expect(html).toContain(message);
        expect(html).toContain(`href="${fixture.result_url}"`);
    });

    it('keeps the survey visible but disabled for other preflight restrictions', async () => {
        waterTest.status.can_start = false;
        waterTest.status.preflight.reason = 'Waiting for normal outputs to turn off.';
        const html = await render();
        expect(html).toMatch(/<fieldset[^>]*disabled/);
        expect(html).toContain(waterTest.status.preflight.reason);
    });

    it.each([true, false])('offers interrupted-start setup with live preflight ready=%s while showing held-off control', async (ready) => {
        Object.assign(waterTest.status, {
            test_id: null, active: false, control_owned: true, startup_interrupted: true,
            can_start: ready, can_resume: false, reason: 'Setup was interrupted before the test began. Start a new test when ready.',
        });
        waterTest.status.preflight.reason = ready ? '' : 'Waiting for a fresh valid beer probe reading.';
        const html = await render();
        expect(html).toContain('id="progress-heading"');
        expect(html).toContain(waterTest.status.reason);
        expect(html).toContain(i18n.global.t('water_test.control_off'));
        expect(html).toContain(i18n.global.t('water_test.prepare_title'));
        expect(html).toContain('id="water-volume"');
        expect(/<fieldset[^>]*disabled/.test(html)).toBe(!ready);
        expect(html).not.toContain(i18n.global.t('water_test.resume'));
        expect(html).not.toContain(i18n.global.t('water_test.restored'));
        if (!ready) expect(html).toContain(waterTest.status.preflight.reason);
    });

    it('offers three flow choices after the bath question, with pump rating selected by default', async () => {
        const html = await render();
        expect(html.indexOf('id="flow-question"')).toBeGreaterThan(html.indexOf('name="glycol-source"'));
        expect(html.match(/name="flow-source"/g)).toHaveLength(3);
        expect(html).toMatch(/name="flow-source"[^>]*value="pump_rating"[^>]*checked/);
        expect(html).toContain('id="glycol-flow"');
        expect(html).not.toContain('id="flow-unit-warning"');
        const unknown = await render('', { flowSource: 'unknown' });
        expect(unknown).not.toContain('id="glycol-flow"');
        expect(unknown).not.toContain('id="flow-unit"');
    });

    it.each([
        ['pump_rating', 'Rated pump flow'],
        ['measured_at_fermenter', 'Measured flow at the fermenter'],
    ])('shows the source-specific decimal input and all four units for %s', async (flowSource, label) => {
        const html = await render('', { flowSource });
        expect(html).toContain(label);
        expect(html).toMatch(/id="glycol-flow"[^>]*type="number"[^>]*step="any"[^>]*required/);
        expect(html).toContain('US GPH (gal/hour)');
        expect(html).toContain('US GPM (gal/min)');
        expect(html).toContain('LPH (L/hour)');
        expect(html).toContain('LPM (L/min)');
        expect(html).toContain('grid-cols-1 sm:grid-cols-2');
    });

    it.each([
        ['l', 'us_gph'], ['l', 'us_gpm'], ['us_gal', 'lph'], ['us_gal', 'lpm'],
    ])('warns without disabling submission for %s volume and %s flow', async (volumeUnit, flowUnit) => {
        const html = await render('', { flowSource: 'pump_rating', volumeUnit, flowUnit });
        expect(html).toContain('id="flow-unit-warning"');
        expect(html).toContain('Mixed units are fine if intentional; you can continue.');
        expect(html).toMatch(/<fieldset(?![^>]*disabled)[^>]*>/);
        expect(html).toMatch(/<button[^>]*type="submit"(?![^>]*disabled)[^>]*>/);
        expect(html.match(/type="checkbox"/g)).toHaveLength(2);
    });

    it.each([
        ['pump_rating', 'l', 'lph'], ['measured_at_fermenter', 'l', 'lpm'],
        ['pump_rating', 'us_gal', 'us_gph'], ['measured_at_fermenter', 'us_gal', 'us_gpm'],
        ['unknown', 'l', 'us_gpm'], ['unknown', 'us_gal', 'lpm'],
    ])('does not warn for %s with %s volume and %s flow', async (flowSource, volumeUnit, flowUnit) => {
        const html = await render('', { flowSource, volumeUnit, flowUnit });
        expect(html).not.toContain('id="flow-unit-warning"');
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

    it('waits for the whole test to finish before describing an upload as pending', async () => {
        Object.assign(waterTest.status, {
            test_id: 'test-running', active: true, phase: 'pulse_2',
            upload_status: 'pending', control_owned: true, can_start: false,
        });
        const html = await render();
        expect(html).toContain('>Stop Test</button>');
        expect(html).toContain('&#39;Stop Test&#39; may wait for the relay minimum ON time.');
        expect(html).toContain('Data submission: Waiting for test to complete');
        expect(html).toContain('will upload the recording after the test ends');
        expect(html).not.toContain('Data submission: Upload pending');
        expect(html).not.toContain('It retries automatically');
    });

    it.each(['completed', 'stopped', 'failed', 'inconclusive', 'interrupted'])('keeps automatic upload information and result links for eligible %s tests', async (outcome) => {
        Object.assign(waterTest.status, {
            test_id: 'test-eligible', phase: 'finished', outcome, completed_pulses: 1,
            upload_status: 'pending', control_owned: true, can_resume: true, can_start: false,
        });
        const pending = await render();
        expect(pending).toContain('Data submission: Upload pending');
        expect(pending).toContain('It retries automatically');
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
            flowSource: 'unknown', consent: true, waterConfirmed: true,
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

    it('matches flow units to volume units while the current flow entry is blank, including after deletion', async () => {
        const page = mount();
        expect(page.form.flowSource).toBe('pump_rating');
        expect(page.form.flowValue).toBe('');
        expect(page.form.flowUnit).toBe('us_gpm');
        page.changeVolumeUnit('l');
        expect(page.form.flowUnit).toBe('lpm');
        page.form.flowUnit = 'lph';
        page.changeVolumeUnit('us_gal');
        expect(page.form.flowUnit).toBe('us_gpm');
        page.form.flowValue = 1.25;
        page.form.flowUnit = 'us_gph';
        page.changeVolumeUnit('l');
        expect(page.form.flowUnit).toBe('us_gph');
        page.form.flowValue = '';
        page.changeVolumeUnit('us_gal');
        expect(page.form.flowUnit).toBe('us_gpm');
        page.changeVolumeUnit('l');
        expect(page.form.flowUnit).toBe('lpm');
        await flush();
    });

    it.each([1.25, 0])('preserves an entered flow value of %s and its units when volume units change', async (flowValue) => {
        const page = mount();
        expect(page.form.flowUnit).toBe('us_gpm');
        page.form.flowValue = flowValue;
        page.form.flowSource = 'pump_rating';
        await nextTick();
        page.changeVolumeUnit('l');
        expect(page.form.flowValue).toBe(flowValue);
        expect(page.form.flowUnit).toBe('us_gpm');
        expect(page.flowUnitsMixed).toBe(true);
        page.form.flowUnit = 'lph';
        expect(page.flowUnitsMixed).toBe(false);
        page.changeVolumeUnit('us_gal');
        expect(page.form.flowValue).toBe(flowValue);
        expect(page.form.flowUnit).toBe('lph');
        expect(page.flowUnitsMixed).toBe(true);
        await flush();
    });

    it.each([
        ['pump_rating', 'measured_at_fermenter'], ['measured_at_fermenter', 'pump_rating'],
        ['pump_rating', 'unknown'], ['measured_at_fermenter', 'unknown'],
        ['unknown', 'pump_rating'], ['unknown', 'measured_at_fermenter'],
    ])('clears the entered flow when its source changes from %s to %s', async (previous, next) => {
        const page = mount();
        page.form.flowSource = previous;
        await nextTick();
        Object.assign(page.form, { flowValue: 2.5, flowUnit: 'lpm' });
        page.form.flowSource = next;
        await nextTick();
        expect(page.form.flowValue).toBe('');
        expect(page.flowUnitsMixed).toBe(next !== 'unknown');
        expect(page.form.flowUnit).toBe('lpm');
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
