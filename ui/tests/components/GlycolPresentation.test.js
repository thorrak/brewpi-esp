import { createPinia, setActivePinia } from 'pinia';
import { createRenderer, nextTick } from 'vue';
import { jest } from '@jest/globals';
import App from '@/App.vue';
import ConfigSensorsActuators from '@/components/ConfigSensorsActuators.vue';
import AssignSensorModal from '@/components/sensors/AssignSensorModal.vue';
import { BrewPiSensor } from '@/mixins/BrewPiSensor';
import { useExtendedSettingsStore } from '@/stores/ExtendedSettingsStore';
import { useBrewPiSensorStore } from '@/stores/BrewPiSensorStore';
import { useTempControlStore } from '@/stores/TempControlStore';
import { i18n } from '@/i18n';

// Render the real page templates, replacing only browser-specific Headless UI
// focus/transition machinery so reactive changes can be exercised in Node.
jest.mock('@headlessui/vue', () => {
    const { h } = require('vue');
    const plain = { setup: (_, { slots }) => () => h('div', slots.default?.()) };
    const transition = {
        props: ['show'],
        setup: (props, { slots }) => () => props.show === false ? null : h('div', slots.default?.()),
    };
    return Object.fromEntries([
        'Dialog', 'DialogPanel', 'DialogOverlay', 'DialogTitle', 'Listbox', 'ListboxButton',
        'ListboxLabel', 'ListboxOption', 'ListboxOptions', 'TransitionChild', 'Switch', 'SwitchGroup', 'SwitchLabel',
    ].map(name => [name, plain]).concat([['TransitionRoot', transition]]));
});

function node(tag, text = '') {
    return {
        tag, tagName: tag.toUpperCase(), text, children: [], parent: null, props: {},
        addEventListener() {}, removeEventListener() {},
        get options() { return this.children.filter(child => child.tag === 'option'); },
    };
}
const renderer = createRenderer({
    createElement: tag => node(tag), createText: text => node('#text', text), createComment: () => node('#comment'),
    setText: (element, text) => { element.text = text; },
    setElementText: (element, text) => { element.text = text; element.children = []; },
    patchProp: (element, key, previous, value) => { element.props[key] = value; element[key] = value; },
    insert(element, parent, anchor) {
        if (element.parent) element.parent.children.splice(element.parent.children.indexOf(element), 1);
        element.parent = parent;
        const index = anchor ? parent.children.indexOf(anchor) : -1;
        if (index < 0) parent.children.push(element);
        else parent.children.splice(index, 0, element);
    },
    remove(element) { if (element.parent) element.parent.children.splice(element.parent.children.indexOf(element), 1); },
    parentNode: element => element.parent,
    nextSibling: element => element.parent?.children[element.parent.children.indexOf(element) + 1],
});
const text = element => element.text + element.children.map(text).join('');
const findAll = (element, tag) => (element.tag === tag ? [element] : []).concat(element.children.flatMap(child => findAll(child, tag)));
const flush = async () => { for (let n = 0; n < 5; n++) await Promise.resolve(); await nextTick(); };
function chamberSensor() {
    const sensor = new BrewPiSensor();
    sensor.convertFromBrewPi({ c: 1, b: 0, f: 5, h: 2, i: 0, a: '28123456789ABCDE' });
    return sensor;
}

describe('glycol-specific sensor names and navigation', () => {
    const originalWindow = global.window;
    const originalDocument = global.document;
    let app, settings, sensors;
    function mount(component, props) {
        const root = node('root');
        app = renderer.createApp(component, props);
        app.use(i18n);
        app.component('RouterView', { inheritAttrs: false, render: () => null });
        app.component('RouterLink', {
            props: ['to', 'custom'],
            setup: (props, { slots }) => () => slots.default({ href: props.to.name, isActive: false, navigate() {} }),
        });
        return { root, vm: app.mount(root) };
    }
    beforeEach(() => {
        jest.useFakeTimers();
        global.window = global;
        global.document = { activeElement: null };
        setActivePinia(createPinia());
        settings = useExtendedSettingsStore();
        sensors = useBrewPiSensorStore();
        jest.spyOn(settings, 'getExtendedSettings').mockResolvedValue();
        jest.spyOn(sensors, 'getDevices').mockResolvedValue();
        jest.spyOn(useTempControlStore(), 'getTempInfo').mockResolvedValue();
    });
    afterEach(() => {
        app?.unmount(); app = null;
        global.window = originalWindow;
        global.document = originalDocument;
        jest.useRealTimers();
    });
    it('shows the chill-test link in both sidebars only after glycol settings arrive, and removes it when disabled', async () => {
        const { root } = mount(App);
        app._instance.setupState.sidebarOpen = true;
        await flush();
        const testLinks = () => findAll(root, 'a').filter(link => link.props.href === 'WaterTest');
        expect(settings.getExtendedSettings).toHaveBeenCalledTimes(1);
        expect(testLinks()).toHaveLength(0);
        settings.glycol = true;
        await nextTick();
        expect(testLinks()).toHaveLength(2);
        settings.glycol = false;
        await nextTick();
        expect(testLinks()).toHaveLength(0);
    });
    it('renames the assigned chamber probe reactively without changing its stored role', async () => {
        const sensor = chamberSensor();
        sensors.devices = [sensor]; sensors.loaded = true;
        const { root } = mount({
            ...ConfigSensorsActuators,
            components: { ...ConfigSensorsActuators.components, AssignSensorModal: { render: () => null } },
        });
        await flush();
        expect(text(root)).toContain('Chamber Temp');
        settings.glycol = true;
        await nextTick();
        expect(text(root)).toContain('Glycol Temp');
        expect(text(root)).not.toContain('Chamber Temp');
        expect(sensor.convertToBrewPi().f).toBe(5);
        settings.glycol = false;
        await nextTick();
        expect(text(root)).toContain('Chamber Temp');
    });
    it('updates an open assignment choice while retaining its numeric role and selection', async () => {
        const { root, vm } = mount(AssignSensorModal, { sensor: chamberSensor() });
        vm.popModal();
        await nextTick();
        const chamberOption = () => findAll(root, 'option').find(option => option.props.value === 5);
        expect(text(chamberOption())).toBe('Chamber Temp');
        settings.glycol = true;
        await nextTick();
        expect(text(chamberOption())).toBe('Glycol Temp');
        expect(vm.new_function).toBe(5);
        expect(text(findAll(root, 'option').find(option => option.props.value === 9))).toBe('Beer Temp');
        settings.glycol = false;
        await nextTick();
        expect(text(chamberOption())).toBe('Chamber Temp');
        expect(vm.new_function).toBe(5);
    });
});
