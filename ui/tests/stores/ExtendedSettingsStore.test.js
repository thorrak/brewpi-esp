import { setActivePinia, createPinia } from 'pinia';
import { useExtendedSettingsStore } from '@/stores/ExtendedSettingsStore.js';
import { mande } from 'mande';
import { jest } from '@jest/globals';
import fixture from './fixtures/api.extended.json';

jest.mock('mande');

const expectedSettings = {
    glycol: true,
    largeTFT: false,
    invertTFT: true,
    resetScreenOnPin: false,
    SETTINGS_CHOICE: 2,
    MIN_COOL_OFF_TIME: 100,
    MIN_HEAT_OFF_TIME: 200,
    MIN_COOL_ON_TIME: 300,
    MIN_HEAT_ON_TIME: 400,
    MIN_COOL_OFF_TIME_FRIDGE_CONSTANT: 500,
    MIN_SWITCH_TIME: 600,
    COOL_PEAK_DETECT_TIME: 700,
    HEAT_PEAK_DETECT_TIME: 800,
};

function responseWithAlgorithm(algorithm) {
    return {
        ...fixture,
        extendedSettings: { ...fixture.extendedSettings, resetScreenOnPin: false, glycolCoolingAlgorithm: algorithm },
    };
}

describe('ExtendedSettingsStore', () => {
    let store;
    let mockGet;
    let mockPut;

    beforeEach(() => {
        jest.clearAllMocks();
        setActivePinia(createPinia());
        store = useExtendedSettingsStore();
        mockGet = jest.fn().mockResolvedValue(responseWithAlgorithm('predictive_coast'));
        mockPut = jest.fn().mockResolvedValue({ status: 'ok' });
        mande.mockImplementation(() => ({ get: mockGet, put: mockPut }));
    });

    it('does not advertise a selectable algorithm before loading capabilities', () => {
        expect(store.hasExtendedSettings).toBe(false);
        expect(store.hasGlycolCoolingAlgorithm).toBe(false);
        expect(store.glycolCoolingAlgorithm).toBeNull();
        expect(store.extendedSettingsError).toBe(false);
        expect(store.extendedSettingsUpdateError).toBe(false);
        expect(store.glycol).toBe(false);
    });

    it.each(['predictive_coast', 'pulse_dose'])('loads the saved %s selection and existing settings', async (algorithm) => {
        mockGet.mockResolvedValue(responseWithAlgorithm(algorithm));
        await store.getExtendedSettings();
        expect(store.hasExtendedSettings).toBe(true);
        expect(store.hasGlycolCoolingAlgorithm).toBe(true);
        expect(store.glycolCoolingAlgorithm).toBe(algorithm);
        for (const [key, value] of Object.entries(fixture.minTimes)) expect(store[key]).toBe(value);
        expect(store.glycol).toBe(fixture.extendedSettings.glycol);
    });

    it.each([undefined, 'future_algorithm'])('hides unsupported selection (%s) without inventing a default', async (algorithm) => {
        mockGet.mockResolvedValue(responseWithAlgorithm(algorithm));
        await store.getExtendedSettings();
        expect(store.hasExtendedSettings).toBe(true);
        expect(store.hasGlycolCoolingAlgorithm).toBe(false);
        expect(store.glycolCoolingAlgorithm).toBeNull();
        await store.setExtendedSettings(expectedSettings);
        expect(mockPut).toHaveBeenCalledWith(expectedSettings);
    });

    it('does not overwrite the last choice when an older response omits the field', async () => {
        mockGet.mockResolvedValueOnce(responseWithAlgorithm('pulse_dose')).mockResolvedValueOnce(fixture);
        await store.getExtendedSettings();
        await store.getExtendedSettings();
        expect(store.glycolCoolingAlgorithm).toBe('pulse_dose');
        expect(store.hasGlycolCoolingAlgorithm).toBe(false);
        await store.setExtendedSettings(expectedSettings);
        expect(mockPut).toHaveBeenCalledWith(expectedSettings);
    });

    it('clears capabilities and settings on reset', async () => {
        await store.getExtendedSettings();
        await store.clearExtendedSettings();
        expect(store.hasExtendedSettings).toBe(false);
        expect(store.hasGlycolCoolingAlgorithm).toBe(false);
        expect(store.glycolCoolingAlgorithm).toBeNull();
        expect(store.glycol).toBe(false);
        expect(store.MIN_COOL_OFF_TIME).toBe(0);
    });

    it('handles an unreadable response', async () => {
        mockGet.mockResolvedValue({});
        await store.getExtendedSettings();
        expect(store.hasExtendedSettings).toBe(false);
        expect(store.extendedSettingsError).toBe(true);
        expect(store.hasGlycolCoolingAlgorithm).toBe(false);
    });

    it('handles a failed GET', async () => {
        mockGet.mockRejectedValue(new Error('network error'));
        await store.getExtendedSettings();
        expect(store.extendedSettingsError).toBe(true);
        expect(store.hasGlycolCoolingAlgorithm).toBe(false);
    });

    it.each(['predictive_coast', 'pulse_dose'])('saves %s with named settings', async (algorithm) => {
        await store.getExtendedSettings();
        const result = await store.setExtendedSettings({ ...expectedSettings, glycolCoolingAlgorithm: algorithm });
        expect(result).toBe(true);
        expect(mockPut).toHaveBeenCalledWith({ ...expectedSettings, glycolCoolingAlgorithm: algorithm });
        for (const [key, value] of Object.entries(expectedSettings)) expect(store[key]).toBe(value);
        expect(store.glycolCoolingAlgorithm).toBe(algorithm);
        expect(store.extendedSettingsUpdateError).toBe(false);
    });

    it('preserves pulse-dose on unrelated saves when no algorithm is specified', async () => {
        mockGet.mockResolvedValue(responseWithAlgorithm('pulse_dose'));
        await store.getExtendedSettings();
        await store.setExtendedSettings(expectedSettings);
        expect(mockPut).toHaveBeenCalledWith({ ...expectedSettings, glycolCoolingAlgorithm: 'pulse_dose' });
    });

    it.each(['unsupported', '', null])('rejects an invalid algorithm (%s) before sending settings', async (algorithm) => {
        await store.getExtendedSettings();
        expect(await store.setExtendedSettings({ ...expectedSettings, glycolCoolingAlgorithm: algorithm })).toBe(false);
        expect(mockPut).not.toHaveBeenCalled();
        expect(store.glycolCoolingAlgorithm).toBe('predictive_coast');
        expect(store.extendedSettingsUpdateError).toBe(true);
    });

    it('rejects explicit selection when firmware has not advertised support', async () => {
        expect(await store.setExtendedSettings({ ...expectedSettings, glycolCoolingAlgorithm: 'pulse_dose' })).toBe(false);
        expect(mockPut).not.toHaveBeenCalled();
    });

    it.each([{ status: 'failed' }, { status: true }, {}, null])('does not accept a failed or malformed PUT response: %j', async (response) => {
        await store.getExtendedSettings();
        mockPut.mockResolvedValue(response);
        expect(await store.setExtendedSettings({ ...expectedSettings, glycolCoolingAlgorithm: 'pulse_dose' })).toBe(false);
        expect(store.glycolCoolingAlgorithm).toBe('predictive_coast');
        expect(store.hasExtendedSettings).toBe(true);
        expect(store.glycol).toBe(fixture.extendedSettings.glycol);
        expect(store.extendedSettingsUpdateError).toBe(true);
    });

    it('keeps confirmed settings after network failure and allows retry', async () => {
        await store.getExtendedSettings();
        mockPut.mockRejectedValueOnce(new Error('network error'));
        expect(await store.setExtendedSettings({ ...expectedSettings, glycolCoolingAlgorithm: 'pulse_dose' })).toBe(false);
        expect(store.glycolCoolingAlgorithm).toBe('predictive_coast');
        expect(store.hasExtendedSettings).toBe(true);
        expect(store.extendedSettingsUpdateError).toBe(true);
        expect(await store.setExtendedSettings({ ...expectedSettings, glycolCoolingAlgorithm: 'pulse_dose' })).toBe(true);
        expect(store.glycolCoolingAlgorithm).toBe('pulse_dose');
        expect(store.extendedSettingsUpdateError).toBe(false);
    });
});
