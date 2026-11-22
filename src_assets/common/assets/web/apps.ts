import { createApp } from 'vue'
import AppsPage from './AppsPage.vue'
import { initApp } from './init'

initApp(createApp(AppsPage))
