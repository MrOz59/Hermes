import { defineConfig } from 'vitest/config'

/**
 * Vitest runs on its own config rather than vite.config.js: that one is driven
 * by the CMake environment (SUNSHINE_SOURCE_ASSETS_DIR and friends) and builds
 * the pages through ejs, none of which a unit test needs or should depend on.
 */
export default defineConfig({
  test: {
    include: ['tests/web/**/*.test.js'],
    // The logic under test is pure - config in, banners out - so there is
    // nothing to render and no DOM to stand up.
    environment: 'node',
  },
})
