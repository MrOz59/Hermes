import {createI18n} from "vue-i18n";
import {compile, registerMessageCompiler} from "@intlify/core-base";

// Import only the fallback language files
import en from './public/assets/locale/en.json'

/**
 * vue-i18n's own compiler, except a malformed message (a bare `@`, an
 * unbalanced brace) is shown as written. In production vue-i18n throws on
 * those, and Vue tears down the whole component that rendered it.
 */
export function safeMessageCompiler(message, context) {
    try {
        return compile(message, context);
    } catch (e) {
        console.error(`Malformed translation ${context.locale}:${context.key}, shown as written`, e);
        return () => message;
    }
}

export default async function() {
    let r = await (await fetch("./api/configLocale", { credentials: 'include' })).json();
    let locale = r.locale ?? "en";
    document.querySelector('html').setAttribute('lang', locale);
    let messages = {
        en
    };
    try {
        if (locale !== 'en') {
            let r = await (await fetch(`./assets/locale/${locale}.json`, { credentials: 'include' })).json();
            messages[locale] = r;
        }
    } catch (e) {
        console.error("Failed to download translations", e);
    }
    // createI18n's legacy mode ignores a messageCompiler option.
    registerMessageCompiler(safeMessageCompiler);
    const i18n = createI18n({
        locale: locale, // set locale
        fallbackLocale: 'en', // set fallback locale
        messages: messages
    })
    return i18n;
}
