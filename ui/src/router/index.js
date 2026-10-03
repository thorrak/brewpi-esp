import { createWebHistory, createRouter } from "vue-router";
import Dashboard from "@/components/Dashboard.vue";
import UpstreamSettings from "@/components/UpstreamSettings.vue";
import ConfigSensorsActuators from "@/components/ConfigSensorsActuators.vue";
import About from "@/components/About.vue";
import ExtendedSettings from "@/components/ExtendedSettings.vue";
import WaterTest from "@/components/WaterTest.vue";

// NOTE - The below paths MUST also be set up in the relevant firmware!
const routes = [
    {
        path: "/",
        name: "Home",
        component: Dashboard,
    },
    {
        path: "/upstream",
        name: "UpstreamSettings",
        component: UpstreamSettings,
    },
    {
        path: "/devices",
        name: "ConfigSensorsActuators",
        component: ConfigSensorsActuators,
    },
    {
        path: "/about",
        name: "About",
        component: About,
    },
    {
        path: "/settings",
        name: "ExtendedSettings",
        component: ExtendedSettings,
    },
    {
        path: "/water-test",
        name: "WaterTest",
        component: WaterTest,
    },
];

const router = createRouter({
    history: createWebHistory(),
    routes,
});

export default router;
