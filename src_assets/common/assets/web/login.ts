import { createApp } from 'vue'
import LoginPage from './LoginPage.vue'
import { initApp } from './init'

initApp(createApp(LoginPage))
