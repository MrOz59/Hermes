import { createApp, defineComponent } from 'vue'
import { initApp } from './init'
import Navbar from './Navbar.vue'
import ClientCard from './ClientCard.vue'
import ResourceCard from './ResourceCard.vue'
import AnnouncementFeed from './AnnouncementFeed.vue'
import HermesVersion, { type GithubRelease } from './hermes_version'
import type { ConfigurationResponse } from './config-types'
import { hostWarnings as computeHostWarnings } from './hostWarnings'

console.log("Hello, Hermes!")
const hermesApi = "https://api.github.com/repos/MrOz59/Hermes";
async function fetchJson<T>(url: string): Promise<T> {
  const response = await fetch(url, {
    headers: {
      "Accept": "application/vnd.github+json",
    },
  });
  if (!response.ok) {
    throw new Error(`GitHub API request failed (${response.status})`);
  }
  return response.json();
}

const app = createApp(defineComponent({
  components: {
    Navbar,
    ClientCard,
    ResourceCard,
    AnnouncementFeed
  },
  data() {
    return {
      config: null as ConfigurationResponse | null,
      version: null as HermesVersion | null,
      stableVersion: null as HermesVersion | null,
      nightlyVersion: null as HermesVersion | null,
      nightlyCommit: null as string | null,
      loading: true,
      updateLoading: true,
      updateError: false,
      logs: null as string | null,
    }
  },
  async created() {
    await this.refresh();
  },
  methods: {
    async refresh() {
      await this.loadStatus();
      await this.loadUpdates();
    },
    async loadStatus() {
      this.loading = true;
      try {
        this.config = await fetch("./api/config").then((r) => r.json());
      } catch (e) {
        console.error(e);
      }
      try {
        this.logs = (await fetch("./api/logs").then(r => r.text()))
      } catch (e) {
        console.error(e);
      }
      this.loading = false;
    },
    async loadUpdates() {
      this.updateLoading = true;
      this.updateError = false;
      this.stableVersion = null;
      this.nightlyVersion = null;
      this.nightlyCommit = null;
      this.version = this.config?.version ? new HermesVersion(null, this.config.version) : null;

      if (!this.version) {
        this.updateError = true;
        this.updateLoading = false;
        return;
      }

      try {
        const stableRelease = await fetchJson<GithubRelease>(`${hermesApi}/releases/latest`);
        this.stableVersion = new HermesVersion(stableRelease, null);
      } catch (e) {
        console.error("Stable update check failed", e);
        this.updateError = true;
      }

      if (this.notifyNightlyUpdates) {
        try {
          const [nightlyRelease, nightlyCommit] = await Promise.all([
            fetchJson<GithubRelease>(`${hermesApi}/releases/tags/nightly`),
            fetchJson<{ sha: string }>(`${hermesApi}/commits/nightly`),
          ]);
          this.nightlyVersion = new HermesVersion(nightlyRelease, null);
          this.nightlyCommit = nightlyCommit.sha;
        } catch (e) {
          console.error("Nightly update check failed", e);
          this.updateError = true;
        }
      }

      this.updateLoading = false;
    },
  },
  computed: {
    announcementsEnabled() {
      const value = this.config?.announcements;
      if (value === undefined || value === null || value === "") {
        // Absent from the config file: on by default. This is how the
        // maintainers reach users about a known issue, so it has to be opt
        // *out*, not opt in.
        return true;
      }
      return !["false", "0", "disabled", "disable", "no", "off"].includes(`${value}`.toLowerCase().trim());
    },
    notifyNightlyUpdates() {
      const value = this.config?.notify_pre_releases;
      if (value === true || value === 1) {
        return true;
      }
      return ["true", "1", "enabled", "enable", "yes", "on"].includes(`${value ?? ""}`.toLowerCase().trim());
    },
    installedVersionNotStable() {
      return !!this.version && !!this.stableVersion && this.version.isGreater(this.stableVersion);
    },
    stableBuildAvailable() {
      return !!this.version && !!this.stableVersion && this.stableVersion.isGreater(this.version);
    },
    nightlyBuildAvailable() {
      if (!this.notifyNightlyUpdates || !this.version || !this.nightlyVersion || !this.nightlyCommit || this.buildVersionIsDirty) {
        return false;
      }
      if (this.nightlyVersion.isGreater(this.version)) {
        return true;
      }
      if (!this.nightlyVersion.isSame(this.version)) {
        return false;
      }

      const installedCommit = this.config?.version_commit ?? "";
      if (!/^[0-9a-f]{7,40}$/i.test(installedCommit)) {
        // Older Hermes packages did not expose their build commit. Offer the
        // current nightly once; builds produced after this change can compare
        // exact commits and will not repeat the notification.
        return true;
      }
      return !this.nightlyCommit.startsWith(installedCommit) && !installedCommit.startsWith(this.nightlyCommit);
    },
    buildVersionIsDirty() {
      return this.config?.version?.includes("dirty") ?? false;
    },
    hostWarnings() {
      // The body lives in ./hostWarnings so it can be tested without a
      // browser: it feeds the root render, and a value it could not cope
      // with used to blank the whole page rather than drop one banner.
      return computeHostWarnings(this.config);
    },
    /** Parse the text errors, calculating the text, the timestamp and the level */
    fancyLogs() {
      if (!this.logs) return [];
      let regex = /(\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}]):\s/g;
      let rawLogLines = (this.logs.split(regex)).splice(1);
      let logLines = []
      for (let i = 0; i < rawLogLines.length; i += 2) {
        logLines.push({ timestamp: rawLogLines[i], level: rawLogLines[i + 1].split(":")[0], value: rawLogLines[i + 1] });
      }
      return logLines;
    }
  }
}));

initApp(app);
