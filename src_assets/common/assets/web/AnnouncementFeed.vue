<script>
import {
  DEFAULT_FEED_URL,
  fetchFeed,
  loadDismissed,
  saveDismissed,
  selectMessages,
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
export default {
  name: 'AnnouncementFeed',
  props: {
    /** Installed version, used to match each message's `affects` range. */
    version: {
      type: String,
      default: null,
    },
    feedUrl: {
      type: String,
      default: DEFAULT_FEED_URL,
    },
  },
  data() {
    return {
      messages: [],
      dismissed: loadDismissed(),
    };
  },
  async created() {
    try {
      this.messages = await fetchFeed(this.feedUrl);
    } catch (e) {
      // An unreachable feed is not a problem the user can act on, and an error
      // box for it would be exactly the noise this feature exists to reduce.
      console.warn('Announcement feed unavailable', e);
    }
  },
  computed: {
    visible() {
      return selectMessages(this.messages, {
        version: this.version,
        dismissed: this.dismissed,
      });
    },
  },
  methods: {
    iconFor(level) {
      return LEVEL_ICONS[level] ?? LEVEL_ICONS.info;
    },
    dismiss(message) {
      // Replace rather than mutate so the computed list re-evaluates.
      this.dismissed = { ...this.dismissed, [message.id]: message.revision };
      saveDismissed(this.dismissed);
    },
  },
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
