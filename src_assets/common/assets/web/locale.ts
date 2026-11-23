import { createI18n } from 'vue-i18n'
import { compile, registerMessageCompiler, type LocaleMessage, type ResourceNode } from '@intlify/core-base'
import en from './public/assets/locale/en.json'

/**
 * Preserve the original text if a malformed translation cannot be compiled.
 * Otherwise, a single bad translation can interrupt Vue rendering.
 */
export function safeMessageCompiler(
  message: string | ResourceNode,
  context: Parameters<typeof compile>[1],
) {
  try {
    return compile(message, context)
  } catch (error) {
    console.error(`Malformed translation ${context.locale}:${context.key}, shown as written`, error)
    return () => typeof message === 'string' ? message : (message.loc?.source ?? '')
  }
}

export default async function createLocale() {
  const settings: { locale?: string } = await (await fetch('./api/configLocale', { credentials: 'include' })).json()
  const locale = settings.locale ?? 'en'
  document.documentElement.lang = locale
  const messages: Record<string, LocaleMessage> = { en }

  try {
    if (locale !== 'en') {
      messages[locale] = await (await fetch(`./assets/locale/${locale}.json`, { credentials: 'include' })).json()
    }
  } catch (error) {
    console.error('Failed to download translations', error)
  }

  // vue-i18n's legacy mode ignores a messageCompiler option.
  registerMessageCompiler(safeMessageCompiler)
  return createI18n({ locale, fallbackLocale: 'en', messages })
}
