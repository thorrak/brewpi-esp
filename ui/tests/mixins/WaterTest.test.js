import { buildWaterTestPayload, convertTemperature, convertVolume, resultsUrl } from '@/mixins/WaterTest';

const form = (changes = {}) => ({
    model: 'Example fermenter', capacity: '7', waterVolume: '5', volumeUnit: 'us_gal',
    coolingType: 'immersion_coil', probeMounting: 'thermowell', glycolChoice: 'reported_setpoint',
    glycolSetpoint: '45', unknownSetpoint: false, temperatureUnit: 'F',
    consent: true, waterConfirmed: true, ...changes,
});

describe('water-test questionnaire', () => {
    it('normalizes explicit units while preserving the participant entries', () => {
        const payload = buildWaterTestPayload(form());
        expect(payload.water_volume_l).toBeCloseTo(18.92705892, 8);
        expect(payload.fermenter_capacity_l).toBeCloseTo(26.497882488, 8);
        expect(payload.reported_chiller_setpoint_c).toBeCloseTo(7.222222222);
        expect(payload.glycol_temperature_source).toBe('reported_setpoint');
        expect(payload.reported_input).toEqual({ volume_unit: 'us_gal', water_volume: 5, fermenter_capacity: 7, temperature_unit: 'F', glycol_setpoint: 45 });
    });
    it('preserves unknown input as null rather than zero or a default', () => {
        const payload = buildWaterTestPayload(form({ unknownSetpoint: true, capacity: '' }));
        expect(payload.glycol_temperature_source).toBe('unknown');
        expect(payload.reported_chiller_setpoint_c).toBeNull();
        expect(payload.reported_input.glycol_setpoint).toBeNull();
        expect(payload.fermenter_capacity_l).toBeNull();
    });
    it('uses the configured glycol probe and excludes static-setpoint inputs in measured mode', () => {
        const payload = buildWaterTestPayload(form({ glycolChoice: 'chamber_probe' }));
        expect(payload.glycol_temperature_source).toBe('chamber_probe');
        expect(payload.reported_chiller_setpoint_c).toBeNull();
        expect(payload.reported_input.glycol_setpoint).toBeNull();
    });
    it.each(['pump_rating', 'measured_at_fermenter'])('retains the source and entered decimal flow for %s in every unit', (flowSource) => {
        for (const flowUnit of ['us_gph', 'us_gpm', 'lph', 'lpm']) {
            const payload = JSON.parse(JSON.stringify(buildWaterTestPayload(form({
                flowSource, flowValue: '12.375', flowUnit,
            }))));
            expect(payload.glycol_flow_source).toBe(flowSource);
            expect(payload.glycol_flow_value).toBe(12.375);
            expect(payload.glycol_flow_unit).toBe(flowUnit);
        }
    });
    it('discards stale flow entries when the participant chooses No', () => {
        const payload = buildWaterTestPayload(form({ flowSource: 'unknown', flowValue: 'not a number', flowUnit: 'old-unit' }));
        expect(payload.glycol_flow_source).toBe('unknown');
        expect(payload.glycol_flow_value).toBeNull();
        expect(payload.glycol_flow_unit).toBeNull();
        expect(buildWaterTestPayload(form()).glycol_flow_source).toBe('unknown');
    });
    it.each([
        ['l', 'us_gph'], ['l', 'us_gpm'], ['us_gal', 'lph'], ['us_gal', 'lpm'],
    ])('accepts accurately reported mixed units: fermenter %s and flow %s', (volumeUnit, flowUnit) => {
        expect(buildWaterTestPayload(form({ volumeUnit, flowUnit, flowValue: '2.75', flowSource: 'pump_rating' })).glycol_flow_value).toBe(2.75);
    });
    it.each(['', ' ', null, undefined, 'bad', Infinity, NaN, -1, 0, true, false, [], [5], {}])('rejects invalid known flow %p', (flowValue) => {
        expect(() => buildWaterTestPayload(form({ flowSource: 'pump_rating', flowUnit: 'lpm', flowValue }))).toThrow();
    });
    it.each(['', 'gph', 'gpm', 'imperial_gpm', 'rpm', 'toString', null, undefined, ['lpm'], {}])('requires an explicit supported flow unit instead of %p', (flowUnit) => {
        expect(() => buildWaterTestPayload(form({ flowSource: 'measured_at_fermenter', flowUnit, flowValue: 2 }))).toThrow();
    });
    it.each(['', null, 'yes', 'rpm'])('rejects unsupported flow source %p', (flowSource) => {
        expect(() => buildWaterTestPayload(form({ flowSource }))).toThrow();
    });
    it('rejects overflow and underflow when interpreting a flow rate', () => {
        expect(() => buildWaterTestPayload(form({ flowSource: 'pump_rating', flowUnit: 'us_gpm', flowValue: Number.MAX_VALUE }))).toThrow();
        expect(() => buildWaterTestPayload(form({ flowSource: 'pump_rating', flowUnit: 'lph', flowValue: Number.MIN_VALUE }))).toThrow();
    });
    it.each(['l', 'us_gal'])('preserves decimal volumes entered in %s', (volumeUnit) => {
        const payload = JSON.parse(JSON.stringify(buildWaterTestPayload(form({
            capacity: '7.25', waterVolume: '5.5', volumeUnit,
        }))));
        const litersPerUnit = volumeUnit === 'us_gal' ? 3.785411784 : 1;
        expect(payload.fermenter_capacity_l).toBeCloseTo(7.25 * litersPerUnit, 8);
        expect(payload.water_volume_l).toBeCloseTo(5.5 * litersPerUnit, 8);
        expect(payload.reported_input.fermenter_capacity).toBe(7.25);
        expect(payload.reported_input.water_volume).toBe(5.5);
    });
    it.each(['', ' ', null, undefined, 'bad', Infinity, -1, 0])('rejects invalid water volume %p', (waterVolume) => {
        expect(() => buildWaterTestPayload(form({ waterVolume }))).toThrow();
    });
    it('requires both consent and water preparation', () => {
        expect(() => buildWaterTestPayload(form({ consent: false }))).toThrow('consent');
        expect(() => buildWaterTestPayload(form({ waterConfirmed: false }))).toThrow('water-only');
    });
    it('requires a setpoint unless explicitly unknown and accepts a real zero Celsius', () => {
        expect(() => buildWaterTestPayload(form({ glycolSetpoint: '' }))).toThrow('setpoint');
        expect(buildWaterTestPayload(form({ glycolSetpoint: '0', temperatureUnit: 'C' })).reported_chiller_setpoint_c).toBe(0);
    });
    it('converts existing entries when changing units without inventing absent values', () => {
        expect(convertVolume('', 'l', 'us_gal')).toBe('');
        expect(convertTemperature('', 'F', 'C')).toBe('');
        expect(convertVolume(convertVolume(5, 'us_gal', 'l'), 'l', 'us_gal')).toBeCloseTo(5);
        expect(convertTemperature(convertTemperature(45, 'F', 'C'), 'C', 'F')).toBeCloseTo(45);
    });
    it('uses the returned HTTP collection link only when it matches the device GUID', () => {
        expect(resultsUrl('0123456789ABCDEF', 'http://chill.fermentrack.net/0123456789ABCDEF/')).toBe('http://chill.fermentrack.net/0123456789ABCDEF/');
        expect(resultsUrl('0123456789ABCDEF', 'https://chill.fermentrack.net/0123456789ABCDEF/')).toBe('');
        expect(resultsUrl('0123456789ABCDEF', 'http://chill.fermentrack.net/0000000000000000/')).toBe('');
        expect(resultsUrl('../elsewhere')).toBe('');
        expect(resultsUrl(undefined)).toBe('');
    });
});
