import { createApp } from 'vue'
import ConfigurationPage from './ConfigurationPage.vue'
import { initApp } from './init'

initApp(createApp(ConfigurationPage))
