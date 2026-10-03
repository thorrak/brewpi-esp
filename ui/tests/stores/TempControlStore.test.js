import { setActivePinia, createPinia } from 'pinia'
import { useTempControlStore } from '@/stores/TempControlStore.js';
import { mande } from 'mande';
import { jest } from '@jest/globals';
import fs from 'fs';
import path from 'path';

jest.mock('mande');

describe('TempControlStore', () => {
    beforeEach(() => {
        jest.clearAllMocks();
        setActivePinia(createPinia());
    });

    it('has correct initial state', () => {
        const store = useTempControlStore();
        expect(store.hasTempInfo).toBe(false);
        expect(store.tempInfoError).toBe(false);
        expect(store.fridgeTemp).toBe(0);
        expect(store.beerTemp).toBe(0);
        expect(store.roomTemp).toBe(0);
        expect(store.tempFormat).toBe("X");
        expect(store.controlMode).toBe("o");
        expect(store.controlState).toBe(0);
        expect(store.setModeError).toBe(false);
        // Add additional initial state checks as needed
    });

    it('clears the temperature info correctly', async () => {
        const store = useTempControlStore();
        store.hasTempInfo = true;
        store.fridgeTemp = 10;
        store.beerTemp = 10;
        store.roomTemp = 10;
        store.tempFormat = "F";
        store.controlMode = "f";
        store.controlState = 1;

        await store.clearTempInfo();

        expect(store.hasTempInfo).toBe(false);
        expect(store.fridgeTemp).toBe(0);
        expect(store.beerTemp).toBe(0);
        expect(store.roomTemp).toBe(0);
        expect(store.tempFormat).toBe("X");
        expect(store.controlMode).toBe("o");
        expect(store.controlState).toBe(0);
        // Add additional cleared state checks as needed
    });

    it('gets temperature info correctly', async () => {
        const fixtureData = JSON.parse(fs.readFileSync(path.resolve(__dirname, './fixtures/api.all_temp_control.json'), 'utf-8'));

        const mockGet = jest.fn().mockResolvedValue(fixtureData);
        mande.mockImplementation(() => {
            return {
                get: mockGet,
            };
        });

        const store = useTempControlStore();

        await store.getTempInfo();

        expect(store.hasTempInfo).toBe(true);
        expect(store.tempInfoError).toBe(false);
        expect(store.fridgeTemp).toBe(fixtureData.temp.FridgeTemp);
        expect(store.beerTemp).toBe(fixtureData.temp.BeerTemp);
        expect(store.roomTemp).toBe(fixtureData.temp.RoomTemp);
        expect(store.tempFormat).toBe(fixtureData.cc.tempFormat);
        expect(store.controlState).toBe(fixtureData.temp.State);
        expect(store.controlMode).toBe(fixtureData.cs.mode);
        // Add additional checks for the state properties
    });

    it('sets mode correctly', async () => {
        const fixtureData = JSON.parse(fs.readFileSync(path.resolve(__dirname, './fixtures/api.all_temp_control.json'), 'utf-8'));
        fixtureData.cs.mode = 'f';
        fixtureData.cs.fridgeSet = 10;
        fixtureData.temp.FridgeSet = 10;
        const mockPut = jest.fn().mockResolvedValue({ status: 'ok' });
        const mockGet = jest.fn().mockResolvedValue(fixtureData);
        mande.mockImplementation(() => {
            return {
                put: mockPut,
                get: mockGet,
            };
        });

        const store = useTempControlStore();
        store.setModeError = true;

        await store.setMode('f', 10);

        expect(store.setModeError).toBe(false);
        expect(mockPut).toHaveBeenCalledTimes(1);
        expect(mockPut).toHaveBeenCalledWith({
            newMode: 'f',
            setPoint: 10
        });
        expect(mande.mock.calls.map(([url]) => url)).toEqual([
            '/api/mode/', '/api/all_temp_control/',
        ]);
        expect(mockGet).toHaveBeenCalledTimes(1);
        expect(store.hasTempInfo).toBe(true);
        expect(store.tempInfoError).toBe(false);
        expect(store.controlMode).toBe('f');
        expect(store.cs.fridgeSet).toBe(10);
        expect(store.tempInfo.FridgeSet).toBe(10);
    });

    it('rejects an error status and still refreshes the actual control mode', async () => {
        const fixtureData = JSON.parse(fs.readFileSync(path.resolve(__dirname, './fixtures/api.all_temp_control.json'), 'utf-8'));
        const mockPut = jest.fn().mockResolvedValue({ status: 'error' });
        const mockGet = jest.fn().mockResolvedValue(fixtureData);
        mande.mockReturnValue({ put: mockPut, get: mockGet });
        const store = useTempControlStore();

        await store.setMode('f', 10);

        expect(store.setModeError).toBe(true);
        expect(mockPut).toHaveBeenCalledWith({ newMode: 'f', setPoint: 10 });
        expect(mockGet).toHaveBeenCalledTimes(1);
        expect(store.hasTempInfo).toBe(true);
        expect(store.tempInfoError).toBe(false);
        expect(store.controlMode).toBe(fixtureData.cs.mode);
        expect(store.cs.fridgeSet).toBe(fixtureData.cs.fridgeSet);
    });
});
