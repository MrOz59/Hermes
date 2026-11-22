import { realpathSync } from 'node:fs'
import { resolve } from 'node:path'

import { codecovVitePlugin } from '@codecov/vite-plugin'
import vue from '@vitejs/plugin-vue'
import { defineConfig } from 'vite'
import { ViteEjsPlugin } from 'vite-plugin-ejs'

const pages = ['apps', 'config', 'index', 'password', 'pin', 'troubleshooting', 'welcome', 'login']

function resolveOverride(override: string | undefined, relativePath: string, fallback: string): string {
  // Canonicalize packaging paths before Rollup uses them as emitted asset names.
  return override ? resolve(realpathSync(override), relativePath) : resolve(fallback)
}

export default defineConfig(async () => {
  const source = resolveOverride(
    Bun.env.HERMES_SOURCE_ASSETS_DIR ?? Bun.env.SUNSHINE_SOURCE_ASSETS_DIR,
    'common/assets/web',
    'src_assets/common/assets/web',
  )
  const output = resolveOverride(
    Bun.env.HERMES_ASSETS_DIR ?? Bun.env.SUNSHINE_ASSETS_DIR,
    'assets/web',
    'target/web-assets/assets/web',
  )
  const header = await Bun.file(resolve(source, 'template_header.html')).text()
  const token = Bun.env.CODECOV_TOKEN

  return {
    root: source,
    base: './',
    resolve: {
      alias: {
        vue: 'vue/dist/vue.esm-bundler.js',
      },
    },
    plugins: [
      vue(),
      ViteEjsPlugin({ header }),
      // Bundle analysis is opt-in and follows the page and Vue transforms.
      ...(token ? [codecovVitePlugin({
        enableBundleAnalysis: true,
        bundleName: 'hermes',
        uploadToken: token,
      })] : []),
    ],
    build: {
      outDir: output,
      emptyOutDir: true,
      rollupOptions: {
        input: Object.fromEntries(pages.map(page => [page, resolve(source, `${page}.html`)])),
      },
    },
  }
})
