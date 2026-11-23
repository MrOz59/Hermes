import { createApp } from 'vue'
import WelcomePage from './WelcomePage.vue'
import { initApp } from './init'

initApp(createApp(WelcomePage))
