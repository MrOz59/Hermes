<script setup lang="ts">
import type { Configuration } from '../../config-types'
import { ref } from 'vue'
import NvidiaNvencEncoder from './encoders/NvidiaNvencEncoder.vue'
import SoftwareEncoder from './encoders/SoftwareEncoder.vue'
import VAAPIEncoder from './encoders/VAAPIEncoder.vue'

const props = defineProps<{
  platform: string;
  config: Configuration;
  currentTab: string;
}>()

const config = ref(props.config)
</script>

<template>

  <!-- NVIDIA NVENC Encoder Tab -->
  <NvidiaNvencEncoder
      v-if="currentTab === 'nv'"
      :platform="platform"
      :config="config"
  />

  <!-- VAAPI Encoder Tab -->
  <VAAPIEncoder
      v-if="currentTab === 'vaapi'"
      :platform="platform"
      :config="config"
  />

  <!-- Software Encoder Tab -->
  <SoftwareEncoder
      v-if="currentTab === 'sw'"
      :platform="platform"
      :config="config"
  />

</template>

<style scoped>
</style>
