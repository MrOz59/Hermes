/**
 * @file tests/web/locale.test.js
 * @brief Every translation compiles.
 *
 * vue-i18n compiles a message the first time it is shown and, in production,
 * throws on a syntax error. A bare `@` (linked-message syntax) in the isolated
 * sessions warning did that twice, and each time Vue tore down the whole
 * Audio/Video tab, leaving only the Save button.
 */
import { readdirSync, readFileSync } from 'node:fs'
import { join } from 'node:path'

import { compile } from '@intlify/core-base'
import { describe, expect, it } from 'vitest'

const localeDir = join(import.meta.dirname, '../../src_assets/common/assets/web/public/assets/locale')

/** [key, message] for every string under a locale's tree. */
function* messages(tree, prefix = '') {
  for (const [key, value] of Object.entries(tree)) {
    const path = prefix ? `${prefix}.${key}` : key
    if (typeof value === 'string') yield [path, value]
    else if (value && typeof value === 'object') yield* messages(value, path)
  }
}

describe('locale files', () => {
  for (const file of readdirSync(localeDir).filter(f => f.endsWith('.json'))) {
    it(`${file} compiles`, () => {
      const broken = []
      for (const [key, message] of messages(JSON.parse(readFileSync(join(localeDir, file), 'utf8')))) {
        compile(message, { key, onError: e => broken.push(`${key}: ${e.message} in ${JSON.stringify(message)}`) })
      }
      expect(broken).toEqual([])
    })
  }
})
