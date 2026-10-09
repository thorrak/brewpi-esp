import { BrewPiSensor } from '@/mixins/BrewPiSensor.js';

describe('BrewPiSensor', () => {
    it('can convert from BrewPi device spec', () => {
        const sensor = new BrewPiSensor();
        const device_spec = {
            c: 1,
            b: 2,
            f: 3,
            h: 4,
            p: 5,
            x: true,
            d: true,
            i: 6,
            a: "address",
            r: "alias",
            n: "child_id",
            j: 7,
        };
        sensor.convertFromBrewPi(device_spec);

        expect(sensor).toMatchObject({
            chamber: 1,
            beer: 2,
            device_function_int: 3,
            hardware_int: 4,
            pin: 5,
            invert: true,
            deactivated: true,
            index: 6,
            address: 'address',
            device_alias: 'alias',
            child_id: 'child_id',
            calibrate_adjust: 7,
        });
    });

    it('can convert to BrewPi device spec', () => {
        const sensor = new BrewPiSensor();
        sensor.chamber = 1;
        sensor.beer = 1;
        sensor.device_function_int = 2;
        sensor.hardware_int = 3;
        sensor.pin = 4;
        sensor.invert = true;
        sensor.deactivated = true;
        sensor.address = "TestAddress";
        sensor.device_alias = "TestAlias";
        sensor.child_id = "TestChildID";
        sensor.calibrate_adjust = 5;
        sensor.index = 6;

        const device_spec = sensor.convertToBrewPi();

        expect(device_spec).toEqual({
            c: 1,
            b: 1,
            f: 2,
            h: 3,
            p: 4,
            x: true,
            d: true,
            a: 'TestAddress',
            i: 6,
        });
    });

    it.each(['01', ''])('includes the TP-Link outlet identifier %j for hardware 7', childID => {
        const sensor = new BrewPiSensor();
        sensor.hardware_int = 7;
        sensor.address = 'AA:BB:CC:DD:EE:FF';
        sensor.child_id = childID;

        expect(sensor.convertToBrewPi()).toMatchObject({
            h: 7,
            a: 'AA:BB:CC:DD:EE:FF',
            n: childID,
        });
    });

    it.each([2, 5, 6])('includes a non-zero calibration adjustment for hardware %i', hardware => {
        const sensor = new BrewPiSensor();
        sensor.hardware_int = hardware;
        sensor.calibrate_adjust = 5;

        const device_spec = sensor.convertToBrewPi();

        expect(device_spec.j).toBe(sensor.calibrate_adjust);
    });

    it('does not include calibrate_adjust in device_spec if it is zero', () => {
        const sensor = new BrewPiSensor();
        sensor.hardware_int = 2;
        sensor.calibrate_adjust = 0;

        const device_spec = sensor.convertToBrewPi();

        expect(device_spec.j).toBeUndefined();
    });

    it('does not include calibrate_adjust in device_spec if hardware_int is not 2, 5, or 6', () => {
        const sensor = new BrewPiSensor();
        sensor.hardware_int = 1;
        sensor.calibrate_adjust = 5;

        const device_spec = sensor.convertToBrewPi();

        expect(device_spec.j).toBeUndefined();
    });


    it('returns the correct device function', () => {
        const sensor = new BrewPiSensor();
        sensor.device_function_int = 1;

        const device_function = sensor.device_function;

        expect(device_function).toBe('chamber_door');
        // TODO - add similar checks for other values
    });

    it('returns the correct device hardware', () => {
        const sensor = new BrewPiSensor();
        sensor.hardware_int = 1;

        const device_hardware = sensor.device_hardware;

        expect(device_hardware).toBe('pin');
        // TODO - add similar checks for other values
    });

    it('returns the correct valid functions for hardware type', () => {
        const sensor = new BrewPiSensor();
        sensor.hardware_int = 1;  // for pin

        const valid_functions = sensor.valid_functions();

        // TODO - add checks for each function
        expect(valid_functions).toContainEqual({id: 0, function_name: 'none'});
        expect(valid_functions).toContainEqual({id: 1, function_name: 'chamber_door'});
        // ...
    });
});
