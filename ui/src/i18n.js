import { getBrowserLocales } from './mixins/GetBrowserLocales';
import de from "./locales/de.json";
import es from "./locales/es.json";
import en from "./locales/en.json";
import nl from "./locales/nl.json";
import pt from "./locales/pt.json";

import { createI18n } from "vue-i18n";
export const i18n = createI18n({
    locale: (typeof navigator === 'undefined' ? undefined : getBrowserLocales({ languageCodeOnly: true })?.[0]) || "en",
    fallbackLocale: "en",
    messages: { de, es, en, nl, pt },
});
