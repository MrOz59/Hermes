<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue'
import {
  DEFAULT_FEED_URL,
  fetchFeed,
  loadDismissed,
  saveDismissed,
  selectMessages,
  type FeedMessage,
} from './feed'

const LEVEL_ICONS = {
  danger: 'fa-circle-exclamation',
  warning: 'fa-triangle-exclamation',
  info: 'fa-circle-info',
  success: 'fa-circle-check',
};

/**
 * Announcements published by the Hermes maintainers, shown at the top of the
 * home page. This is how a known issue reaches people who are hitting it,
 * before they go and file it again.
 *
 * The parent decides whether to render this component at all, so mounting is
 * itself the consent to make the outbound request.
 */
const { version = null, feedUrl = DEFAULT_FEED_URL } = defineProps<{
  version?: string | null;
  feedUrl?: string;
}>();
const messages = ref<FeedMessage[]>([]);
const dismissed = ref(loadDismissed());
const request = new AbortController();
const visible = computed(() => selectMessages(messages.value, {
  version,
  dismissed: dismissed.value,
}));

onMounted(async () => {
  try {
    messages.value = await fetchFeed(feedUrl, { signal: request.signal });
  } catch (error) {
    if (!request.signal.aborted) console.warn('Announcement feed unavailable', error);
  }
});
onUnmounted(() => request.abort());

function iconFor(level: FeedMessage['level']): string {
  return LEVEL_ICONS[level] ?? LEVEL_ICONS.info;
}

function dismiss(message: FeedMessage): void {
  dismissed.value = { ...dismissed.value, [message.id]: message.revision };
  saveDismissed(dismissed.value);
}
</script>

<template>
  <div v-if="visible.length" class="my-4">
    <div v-for="message in visible"
         :key="message.id"
         class="alert d-flex align-items-start gap-3 mb-2"
         :class="`alert-${message.level}`">
      <i class="fa-solid fa-fw mt-1" :class="iconFor(message.level)"></i>
      <div class="flex-grow-1">
        <div class="fw-semibold">{{ message.title }}</div>
        <p v-if="message.body" class="mb-0 mt-1 announcement-body">{{ message.body }}</p>
        <a v-if="message.link"
           class="btn btn-sm btn-outline-dark mt-2"
           :href="message.link.url"
           target="_blank"
           rel="noopener noreferrer">
          {{ message.link.label || $t('feed.learn_more') }}
        </a>
      </div>
      <button v-if="message.dismissible"
              type="button"
              class="btn-close"
              :aria-label="$t('_common.dismiss')"
              @click="dismiss(message)"></button>
    </div>
  </div>
</template>

<style scoped>
/* Announcement bodies are plain text; keep the author's line breaks without
   letting the feed inject markup. */
.announcement-body {
  white-space: pre-line;
}
</style>
