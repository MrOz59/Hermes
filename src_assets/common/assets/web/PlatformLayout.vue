<script setup>
const props = defineProps({
  platform: {
    type: String,
    required: true
  }
})
</script>

<template>
  <slot name="linux" v-if="$slots.linux && platform === 'linux'"></slot>
</template>


<style scoped>

</style>
