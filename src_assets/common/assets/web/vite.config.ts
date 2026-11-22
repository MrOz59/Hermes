import { realpathSync } from 'node:fs'
import { resolve } from 'node:path'

import vue from '@vitejs/plugin-vue'
import { defineConfig } from 'vite'

const pages = ['apps', 'config', 'index', 'password', 'pin', 'troubleshooting', 'welcome', 'login']

function resolveAssetsPath(override: string | undefined, relativePath: string, fallback: string): string {
  // Canonicalize overrides so mcpp can stage assets through symlinked paths.
  return override ? resolve(realpathSync(override), relativePath) : resolve(fallback)
}

const source = import.meta.dirname
const output = resolveAssetsPath(
  Bun.env.HERMES_ASSETS_DIR,
  'assets/web',
  resolve(source, '../../../../target/web-assets/assets/web'),
)

export default defineConfig({
  root: source,
  base: './',
  plugins: [vue()],
  build: {
    outDir: output,
    emptyOutDir: true,
    rolldownOptions: {
      input: Object.fromEntries(pages.map(page => [page, resolve(source, `${page}.html`)])),
    },
  },
})
