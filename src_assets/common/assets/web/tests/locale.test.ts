/**
 * @file src_assets/common/assets/web/tests/locale.test.ts
 * @brief Every translation compiles, and one that does not costs only itself.
 *
 * vue-i18n compiles a message the first time it is shown and, in production,
 * throws on a syntax error. A bare `@` (linked-message syntax) in the isolated
 * sessions warning did that twice, and each time Vue tore down the whole
 * Audio/Video tab, leaving only the Save button.
 */
import { readdirSync, readFileSync } from 'node:fs'
import { join } from 'node:path'

import { compile, createMessageContext } from '@intlify/core-base'
import { describe, expect, it } from 'bun:test'

import { safeMessageCompiler } from '../locale.ts'

const localeDir = join(import.meta.dirname, '../public/assets/locale')

/** [key, message] for every string under a locale's tree. */
function* messages(tree: object, prefix = ''): Generator<[string, string]> {
  for (const [key, value] of Object.entries(tree)) {
    const path = prefix ? `${prefix}.${key}` : key
    if (typeof value === 'string') yield [path, value]
    else if (value && typeof value === 'object') yield* messages(value, path)
  }
}

describe('locale files', () => {
  for (const file of readdirSync(localeDir).filter(f => f.endsWith('.json'))) {
    it(`${file} compiles`, () => {
      const broken: string[] = []
      for (const [key, message] of messages(JSON.parse(readFileSync(join(localeDir, file), 'utf8')))) {
        compile(message, { locale: file.slice(0, -5), key, onError: error => { broken.push(`${key}: ${error.message} in ${JSON.stringify(message)}`) } })
      }
      expect(broken).toEqual([])
    })
  }
})

describe('safeMessageCompiler', () => {
  it('shows a malformed message as written instead of throwing', () => {
    const message = 'run hermes-kms-seatd@N.service'
    const onError = (error: Error): never => { throw error }
    const original = console.error
    console.error = () => {}
    try {
      const msg = safeMessageCompiler(message, { locale: 'en', key: 'k', onError })
      expect(msg(createMessageContext())).toBe(message)
    } finally {
      console.error = original
    }
  })
})
