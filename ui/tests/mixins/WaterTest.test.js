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
