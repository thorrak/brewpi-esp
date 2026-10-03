import { setActivePinia, createPinia } from 'pinia'
import { useUpstreamSettingsStore } from '@/stores/UpstreamSettingsStore.js';
import { mande } from 'mande';
import { jest } from '@jest/globals';
import fs from 'fs';
import path from 'path';

jest.mock('mande');

describe('UpstreamSettingsStore', () => {
    beforeEach(() => {
        jest.clearAllMocks();
        setActivePinia(createPinia());
    });

    it('has correct initial state', () => {
        const store = useUpstreamSettingsStore();
        expect(store.hasUpstreamSettings).toBe(false);
        expect(store.upstreamSettingsError).toBe(false);
        expect(store.upstreamHost).toBe("");
        expect(store.upstreamPort).toBe(0);
        expect(store.username).toBe("");
        expect(store.apiKey).toBe("");
        expect(store.upstreamRegistrationError).toBe(7);
        expect(store.deviceID).toBe("");
    });

    it('clears the upstream settings correctly', async () => {
        const store = useUpstreamSettingsStore();
        store.hasUpstreamSettings = true;
        store.upstreamHost = "localhost";
        store.upstreamPort = 8080;
        store.username = "user";
        store.apiKey = "key";
        store.upstreamRegistrationError = 0;
        store.deviceID = "id";

        await store.clearUpstreamSettings();

        expect(store.hasUpstreamSettings).toBe(false);
        expect(store.upstreamHost).toBe("");
        expect(store.upstreamPort).toBe(0);
        expect(store.username).toBe("");
        expect(store.apiKey).toBe("");
        expect(store.upstreamRegistrationError).toBe(7);
        expect(store.deviceID).toBe("");
    });

    it('gets upstream settings correctly', async () => {
        const fixtureData = JSON.parse(fs.readFileSync(path.resolve(__dirname, './fixtures/api.upstream.json'), 'utf-8'));

        const mockGet = jest.fn().mockResolvedValue(fixtureData);
        mande.mockImplementation(() => {
            return {
                get: mockGet,
            };
        });

        const store = useUpstreamSettingsStore();

        await store.getUpstreamSettings();

        expect(store.hasUpstreamSettings).toBe(true);
        expect(store.upstreamSettingsError).toBe(false);
        expect(store.upstreamHost).toBe(fixtureData.upstreamHost);
        expect(store.upstreamPort).toBe(fixtureData.upstreamPort);
        expect(store.username).toBe(fixtureData.username);
        expect(store.apiKey).toBe(fixtureData.apiKey);
        // TODO - Assert other state properties here as needed
    });

    it('sets upstream settings correctly', async () => {
        const fixtureData = JSON.parse(fs.readFileSync(path.resolve(__dirname, './fixtures/api.upstream.json'), 'utf-8'));

        const mockPut = jest.fn().mockResolvedValue({ status: 'ok' });
        mande.mockImplementation(() => {
            return {
                put: mockPut,
            };
        });

        const store = useUpstreamSettingsStore();

        await store.setUpstreamSettings(
            fixtureData.upstreamHost,
            String(fixtureData.upstreamPort),
            fixtureData.username,
            fixtureData.apiKey
        );

        expect(mande).toHaveBeenCalledWith('/api/upstream/', expect.any(Object));
        expect(mockPut).toHaveBeenCalledTimes(1);
        expect(mockPut).toHaveBeenCalledWith({
            upstreamHost: fixtureData.upstreamHost,
            upstreamPort: fixtureData.upstreamPort,
            username: fixtureData.username,
        });
        expect(store.hasUpstreamSettings).toBe(true);
        expect(store.upstreamSettingsError).toBe(false);
        expect(store.upstreamHost).toBe(fixtureData.upstreamHost);
        expect(store.upstreamPort).toBe(fixtureData.upstreamPort);
        expect(store.username).toBe(fixtureData.username);
        expect(store.apiKey).toBe(fixtureData.apiKey);
        expect(store.awaitingRegistration).toBe(true);
    });

    it('rejects an error status and clears it only after an acknowledged retry', async () => {
        const mockPut = jest.fn().mockResolvedValue({ status: 'error' });
        mande.mockReturnValue({ put: mockPut });
        const store = useUpstreamSettingsStore();
        store.hasUpstreamSettings = true;
        store.upstreamHost = 'previous.example';
        store.username = 'previous-user';
        store.apiKey = 'previous-key';
        store.awaitingRegistration = true;

        await store.setUpstreamSettings('new.example', 80, 'new-user', 'new-key');

        expect(mockPut).toHaveBeenCalledTimes(1);
        expect(store.upstreamSettingsError).toBe(true);
        expect(store.hasUpstreamSettings).toBe(false);
        expect(store.upstreamHost).toBe('');
        expect(store.upstreamPort).toBe(0);
        expect(store.username).toBe('');
        expect(store.apiKey).toBe('');
        expect(store.awaitingRegistration).toBe(false);

        mockPut.mockResolvedValueOnce({ status: 'ok' });
        await store.setUpstreamSettings('new.example', 80, 'new-user', 'new-key');
        expect(store.upstreamSettingsError).toBe(false);
        expect(store.hasUpstreamSettings).toBe(true);
        expect(store.upstreamHost).toBe('new.example');
        expect(store.awaitingRegistration).toBe(true);
    });

});
