import { inject, type ComputedRef } from 'vue'
import type { VueI18n } from 'vue-i18n'

export type Translator = Pick<VueI18n, 't'>

export function useTranslator(): Translator {
  const translator = inject<Translator>('i18n')
  if (!translator) throw new Error('Platform messages need an initialized locale')
  return translator
}

class PlatformMessageI18n {
  constructor(private readonly platform: string) {}

  getMessageUsingPlatform(key: string, defaultMessage?: string): string {
    const i18n = useTranslator()
    const platformKey = `${key}_${this.platform}`
    const message = i18n.t(platformKey)
    if (message !== platformKey) return message

    const unixKey = `${key}_unix`
    const fallback = i18n.t(unixKey)
    return fallback === unixKey && defaultMessage ? defaultMessage : fallback
  }
}

export function usePlatformI18n(platform = inject<ComputedRef<string>>('platform')?.value): PlatformMessageI18n {
  if (!platform) throw new Error('Platform argument missing')
  return inject('platformMessage', () => new PlatformMessageI18n(platform), true)
}

export function $tp(key: string, defaultMessage?: string): string {
  return usePlatformI18n().getMessageUsingPlatform(key, defaultMessage)
}
