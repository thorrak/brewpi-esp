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

function requiredNumber(value, label) {
    if (value === null || value === undefined || String(value).trim() === '' || !Number.isFinite(Number(value))) {
        throw new Error(`Enter ${label}.`);
    }
    return Number(value);
}

export function buildWaterTestPayload(form) {
    if (!form.consent || !form.waterConfirmed) throw new Error('Confirm the water-only preparation and consent to submit this test.');
    if (!['l', 'us_gal'].includes(form.volumeUnit) || !['C', 'F'].includes(form.temperatureUnit)) {
        throw new Error('Select volume and temperature units.');
    }
    const waterVolume = requiredNumber(form.waterVolume, 'the amount of water in the fermenter');
    const waterLiters = convertVolume(waterVolume, form.volumeUnit, 'l');
    if (waterLiters < 0.01 || waterLiters > 10000) throw new Error('Water volume must be between 0.01 and 10,000 liters.');
    const capacity = String(form.capacity ?? '').trim() === '' ? null : requiredNumber(form.capacity, 'a valid fermenter capacity');
    const capacityLiters = capacity === null ? null : convertVolume(capacity, form.volumeUnit, 'l');
    if (capacityLiters !== null && (capacityLiters < 0.01 || capacityLiters > 10000)) {
        throw new Error('Fermenter capacity must be between 0.01 and 10,000 liters.');
    }
    if (!['jacket', 'immersion_coil', 'other', 'unknown'].includes(form.coolingType)) throw new Error('Select how the fermenter is cooled.');
    if (!['thermowell', 'immersed', 'outside', 'other', 'unknown'].includes(form.probeMounting)) throw new Error('Select how the beer probe is installed.');
    if (!['chamber_probe', 'reported_setpoint'].includes(form.glycolChoice)) throw new Error('Select whether you can place a chamber probe in the glycol bath.');
    if (form.glycolChoice === 'chamber_probe' && !form.bathPlacementConfirmed) throw new Error('Confirm that the chamber probe is now in the glycol bath.');
    const source = form.glycolChoice === 'chamber_probe' ? 'chamber_probe' : (form.unknownSetpoint ? 'unknown' : 'reported_setpoint');
    const setpoint = source === 'reported_setpoint' ? requiredNumber(form.glycolSetpoint, "the chiller's setpoint, or select unknown") : null;
    const setpointC = setpoint === null ? null : convertTemperature(setpoint, form.temperatureUnit, 'C');
    if (setpointC !== null && (setpointC < -60 || setpointC > 100)) throw new Error('Check the chiller setpoint and its units.');
    return {
        consent: true,
        water_confirmed: true,
        fermenter_model: String(form.model || '').trim(),
        fermenter_capacity_l: capacityLiters,
        water_volume_l: waterLiters,
        cooling_type: form.coolingType,
        probe_mounting: form.probeMounting,
        glycol_temperature_source: source,
        bath_placement_confirmed: source === 'chamber_probe' && form.bathPlacementConfirmed === true,
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
