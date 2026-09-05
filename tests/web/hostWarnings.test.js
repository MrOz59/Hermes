/**
 * @file tests/web/hostWarnings.test.js
 * @brief The home page's host-readiness banners.
 *
 * The first tests in this repository that run on the Web UI at all. The
 * reported bug they exist for did not corrupt a banner: it blanked the entire
 * page. `hostWarnings` is a computed feeding the root render with no error
 * boundary, so a value it could not cope with left `hostWarnings` undefined,
 * the render dereferenced `.length`, Vue tore the tree down, and `v-cloak`
 * hid what was left. A bad field must cost one banner, never the page.
 */
import { describe, expect, it } from 'vitest'

import { hostWarnings } from '../../src_assets/common/assets/web/hostWarnings.js'

/** A host with nothing wrong with it, as /api/config describes one. */
const healthyHost = {
  platform: 'linux',
  virtual_display_backend: 'hermes_kms',
  hermesKmsInfo: { diagnostic: 'ready' },
  evdiInfo: { diagnostic: 'ready', activeDisplays: [] },
  clipboardInfo: { diagnostic: 'ready', available: true },
  streamPorts: [
    { name: 'Video', port: 47998, available: true },
    { name: 'Control', port: 47999, available: true },
  ],
}

describe('hostWarnings', () => {
  it('says nothing about a healthy host', () => {
    expect(hostWarnings(healthyHost)).toEqual([])
  })

  it('survives a streamPorts that came back through the config file as a string', () => {
    // The reported bug, exactly: a read-only field round-tripped through
    // hermes.conf and returned as the string "[]". It is truthy, so the old
    // `|| []` guard did not fire and `.filter` was not a function.
    const warnings = hostWarnings({ ...healthyHost, streamPorts: '[]' })
    expect(Array.isArray(warnings)).toBe(true)
    expect(warnings.find(w => w.id === 'stream-port-in-use')).toBeUndefined()
  })

  it.each([
    ['null', null],
    ['undefined', undefined],
    ['a string', 'not a config at all'],
    ['a number', 0],
    ['an array', []],
    ['an empty object', {}],
  ])('returns an array of warnings when the config is %s', (_label, config) => {
    expect(Array.isArray(hostWarnings(config))).toBe(true)
  })

  it('survives every structured field arriving as a string', () => {
    // Any of these can reach the response the same way streamPorts did, and
    // the parser only ever produces strings. None of them may throw.
    const mangled = {
      platform: 'linux',
      virtual_display_backend: 'evdi',
      evdiInfo: '{}',
      hermesKmsInfo: '{}',
      clipboardInfo: '{}',
      streamPorts: '[]',
      isolated_virtual_display_option: 'true',
    }
    expect(() => hostWarnings(mangled)).not.toThrow()
    expect(Array.isArray(hostWarnings(mangled))).toBe(true)
  })

  it('still reports a port that cannot be bound', () => {
    // Hardening the input must not cost the warning itself: the banner is the
    // only place a taken port is visible before a client tries to connect.
    const warnings = hostWarnings({
      ...healthyHost,
      streamPorts: [
        { name: 'Video', port: 47998, available: false },
        { name: 'Control', port: 47999, available: true },
      ],
    })
    const busy = warnings.find(w => w.id === 'stream-port-in-use')
    expect(busy).toBeDefined()
    expect(busy.level).toBe('danger')
    expect(busy.message).toContain('Video (47998)')
    expect(busy.message).not.toContain('Control')
  })

  it('ignores a malformed entry among well-formed ports', () => {
    const warnings = hostWarnings({
      ...healthyHost,
      streamPorts: [null, 'Video', { name: 'Audio', port: 48000, available: false }],
    })
    const busy = warnings.find(w => w.id === 'stream-port-in-use')
    expect(busy).toBeDefined()
    expect(busy.message).toContain('Audio (48000)')
  })

  it('warns about the backend that is actually selected, and only that one', () => {
    // A host that wants no virtual-display device at all was being told to go
    // install EVDI, and every remedy it was offered was a no-op there.
    const evdiHost = {
      ...healthyHost,
      virtual_display_backend: 'evdi',
      evdiInfo: { diagnostic: 'library-missing' },
      hermesKmsInfo: { diagnostic: 'module-missing' },
    }
    expect(hostWarnings(evdiHost).map(w => w.id)).toEqual(['evdi-library-missing'])

    const noneHost = { ...evdiHost, virtual_display_backend: 'none' }
    expect(hostWarnings(noneHost)).toEqual([])
  })

  it('does not warn a non-Linux host about Linux-only devices', () => {
    const windowsHost = {
      ...healthyHost,
      platform: 'windows',
      virtual_display_backend: 'evdi',
      evdiInfo: { diagnostic: 'library-missing' },
      clipboardInfo: { diagnostic: 'missing', available: false },
    }
    expect(hostWarnings(windowsHost)).toEqual([])
  })

  it('reports a capture path that fell back to a physical display', () => {
    const warnings = hostWarnings({
      ...healthyHost,
      evdiInfo: { diagnostic: 'ready', captureFallbackActive: true },
    })
    expect(warnings.map(w => w.id)).toContain('evdi-capture-fallback')
    expect(warnings.find(w => w.id === 'evdi-capture-fallback').level).toBe('danger')
  })

  it('reports a virtual display that is copying frames through system memory', () => {
    const warnings = hostWarnings({
      ...healthyHost,
      evdiInfo: { diagnostic: 'ready', activeDisplays: [{ zeroCopyCapture: false }] },
    })
    expect(warnings.map(w => w.id)).toContain('evdi-cpu-buffer')
  })

  it('gives every warning the fields the template renders', () => {
    const warnings = hostWarnings({
      ...healthyHost,
      virtual_display_backend: 'evdi',
      evdiInfo: { diagnostic: 'device-unconfigured', captureFallbackActive: true },
      clipboardInfo: { diagnostic: 'missing', available: false },
      streamPorts: [{ name: 'Video', port: 47998, available: false }],
    })
    expect(warnings.length).toBeGreaterThan(1)
    for (const warning of warnings) {
      expect(warning).toMatchObject({
        id: expect.any(String),
        level: expect.stringMatching(/^(warning|danger)$/),
        title: expect.any(String),
        message: expect.any(String),
        href: expect.any(String),
        action: expect.any(String),
      })
    }
    // The id keys the v-for; duplicates make Vue reuse the wrong node.
    const ids = warnings.map(w => w.id)
    expect(new Set(ids).size).toBe(ids.length)
  })
})
