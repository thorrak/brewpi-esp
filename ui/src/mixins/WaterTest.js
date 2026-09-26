import { i18n } from '@/i18n';

const LITERS_PER_US_GALLON = 3.785411784;

export function convertVolume(value, from, to) {
    if (value === '' || value === null || value === undefined) return '';
    const liters = Number(value) * (from === 'us_gal' ? LITERS_PER_US_GALLON : 1);
    return liters / (to === 'us_gal' ? LITERS_PER_US_GALLON : 1);
}

export function convertTemperature(value, from, to) {
    if (value === '' || value === null || value === undefined) return '';
    const celsius = from === 'F' ? (Number(value) - 32) / 1.8 : Number(value);
    return to === 'F' ? celsius * 1.8 + 32 : celsius;
}

function requiredNumber(value, messageKey) {
    if (value === null || value === undefined || String(value).trim() === '' || !Number.isFinite(Number(value))) {
        throw new Error(i18n.global.t(`water_test.errors.${messageKey}`));
    }
    return Number(value);
}

export function buildWaterTestPayload(form) {
    if (!form.consent || !form.waterConfirmed) throw new Error(i18n.global.t('water_test.errors.confirm_preparation'));
    if (!['l', 'us_gal'].includes(form.volumeUnit) || !['C', 'F'].includes(form.temperatureUnit)) {
        throw new Error(i18n.global.t('water_test.errors.select_units'));
    }
    const waterVolume = requiredNumber(form.waterVolume, 'enter_water');
    const waterLiters = convertVolume(waterVolume, form.volumeUnit, 'l');
    if (waterLiters < 0.01 || waterLiters > 10000) throw new Error(i18n.global.t('water_test.errors.water_range'));
    const capacity = String(form.capacity ?? '').trim() === '' ? null : requiredNumber(form.capacity, 'enter_capacity');
    const capacityLiters = capacity === null ? null : convertVolume(capacity, form.volumeUnit, 'l');
    if (capacityLiters !== null && (capacityLiters < 0.01 || capacityLiters > 10000)) {
        throw new Error(i18n.global.t('water_test.errors.capacity_range'));
    }
    if (!['jacket', 'immersion_coil', 'other', 'unknown'].includes(form.coolingType)) throw new Error(i18n.global.t('water_test.errors.select_cooling'));
    if (!['thermowell', 'immersed', 'outside', 'other', 'unknown'].includes(form.probeMounting)) throw new Error(i18n.global.t('water_test.errors.select_probe'));
    if (!['chamber_probe', 'reported_setpoint'].includes(form.glycolChoice)) throw new Error(i18n.global.t('water_test.errors.select_chamber'));
    const source = form.glycolChoice === 'chamber_probe' ? 'chamber_probe' : (form.unknownSetpoint ? 'unknown' : 'reported_setpoint');
    const setpoint = source === 'reported_setpoint' ? requiredNumber(form.glycolSetpoint, 'enter_setpoint') : null;
    const setpointC = setpoint === null ? null : convertTemperature(setpoint, form.temperatureUnit, 'C');
    if (setpointC !== null && (setpointC < -60 || setpointC > 100)) throw new Error(i18n.global.t('water_test.errors.setpoint_range'));
    return {
        consent: true,
        water_confirmed: true,
        fermenter_model: String(form.model || '').trim(),
        fermenter_capacity_l: capacityLiters,
        water_volume_l: waterLiters,
        cooling_type: form.coolingType,
        probe_mounting: form.probeMounting,
        glycol_temperature_source: source,
        reported_chiller_setpoint_c: setpointC,
        follow_up: false,
        reported_input: {
            volume_unit: form.volumeUnit,
            water_volume: waterVolume,
            fermenter_capacity: capacity,
            temperature_unit: form.temperatureUnit,
            glycol_setpoint: setpoint,
        },
    };
}

export function resultsUrl(guid, url) {
    return typeof guid === 'string' && /^[0-9A-F]{16}$/.test(guid)
        && url === `http://chill.fermentrack.net/${guid}/` ? url : '';
}
