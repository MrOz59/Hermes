import type { App } from 'vue'
import createLocale from './locale'

// Register Bootstrap's data API for navigation and dropdowns.
import 'bootstrap'
import 'bootstrap/dist/css/bootstrap.min.css'
import '@fortawesome/fontawesome-free/css/all.min.css'

export function initApp(app: App): void {
  // Wait for locale initialization before Vue renders the page.
  void createLocale().then(i18n => {
    app.use(i18n)
    app.provide('i18n', i18n.global)
    app.mount('#app')
  })
}
