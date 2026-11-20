<script setup>
import { ref } from 'vue'
import Checkbox from "../../../Checkbox.vue";

const props = defineProps([
  'platform',
  'config',
])

const config = ref(props.config)
</script>

<template>
  <div id="vaapi-encoder" class="config-page">
    <!-- Strict RC Buffer -->
    <Checkbox class="mb-3"
              id="vaapi_strict_rc_buffer"
              locale-prefix="config"
              v-model="config.vaapi_strict_rc_buffer"
              default="false"
    ></Checkbox>
    <!-- Rolling intra refresh -->
    <Checkbox class="mb-3"
              id="vaapi_intra_refresh"
              locale-prefix="config"
              v-model="config.vaapi_intra_refresh"
              default="true"
    ></Checkbox>
  </div>
</template>

<style scoped>

</style>
