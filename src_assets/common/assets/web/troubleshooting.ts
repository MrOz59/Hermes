import { createApp, defineComponent } from 'vue'
import { initApp } from './init'
import Navbar from './Navbar.vue'

interface Metrics {
  encoder?: { name: string; hardware: boolean; codecs: string[] };
  sessions?: { active: number; streaming: boolean };
  pipeline?: {
    fps: number;
    bitrate_kbps: number;
    encode_ms: number;
    capture_to_encode_ms: number;
    frames_dropped: number;
  };
}

const app = createApp(defineComponent({
  components: {
    Navbar
  },
  data() {
    return {
      clients: [],
      closeAppPressed: false,
      closeAppStatus: null as boolean | null,
      logs: 'Loading...',
      logFilter: null as string | null,
      logInterval: undefined as number | undefined,
      serverRestarting: false,
      serverQuitting: false,
      serverQuit: false,
      platform: "",
      metrics: null as Metrics | null,
      metricsInterval: undefined as number | undefined,
    };
  },
  computed: {
    actualLogs() {
      if (!this.logFilter) return this.logs;
      let lines = this.logs.split("\n");
      const filter = this.logFilter;
      lines = lines.filter((line: string) => line.includes(filter));
      return lines.join("\n");
    }
  },
  created() {
    fetch("/api/config")
      .then((r) => r.json())
      .then((r) => {
        this.platform = r.platform;
      });

    this.logInterval = window.setInterval(() => {
      this.refreshLogs();
    }, 5000);
    this.refreshLogs();

    this.metricsInterval = window.setInterval(() => {
      this.refreshMetrics();
    }, 1000);
    this.refreshMetrics();
  },
  beforeUnmount() {
    clearInterval(this.logInterval);
    clearInterval(this.metricsInterval);
  },
  methods: {
    refreshMetrics() {
      fetch("./api/metrics", { credentials: 'include' })
        .then(r => r.ok ? r.json() : null)
        .then(r => { this.metrics = r; })
        .catch(() => { /* keep last known metrics on transient errors */ });
    },
    refreshLogs() {
      fetch("./api/logs", {
        credentials: 'include'
      })
        .then(response => {
          // Retrieve the Content-Type header
          const contentType = response.headers.get("Content-Type") || "";
          // Attempt to extract charset from the header
          const charsetMatch = contentType.match(/charset=([^;]+)/i);
          const charset = charsetMatch ? charsetMatch[1].trim() : "utf-8";

          // Read response as an ArrayBuffer and decode it with the correct charset
          return response.arrayBuffer().then(buffer => {
            const decoder = new TextDecoder(charset);
            return decoder.decode(buffer);
          });
        })
        .then(text => {
          this.logs = text;
        })
        .catch(error => console.error("Error fetching logs:", error));
    },
    closeApp() {
      this.closeAppPressed = true;
      fetch("./api/apps/close", {
        credentials: 'include',
        method: 'POST',
        headers: {
          'Content-Type': 'application/json'
        }
      })
        .then((r) => r.json())
        .then((r) => {
          this.closeAppPressed = false;
          this.closeAppStatus = r.status;
          setTimeout(() => {
            this.closeAppStatus = null;
          }, 5000);
        });
    },
    copyLogs() {
      navigator.clipboard.writeText(this.actualLogs);
    },
    restart() {
      this.serverRestarting = true;
      setTimeout(() => {
        this.serverRestarting = false;
      }, 5000);
      fetch("./api/restart", {
        credentials: 'include',
        method: 'POST',
        headers: {
          'Content-Type': 'application/json'
        }
      })
      .then((resp) => {
        if (resp.status !== 200) {
          setTimeout(() => {
            location.reload();
          }, 1000);
          return;
        }
      })
      .catch((e) => {
        this.serverRestarting = false
        console.error(e);
        setTimeout(() => {
          location.reload();
        }, 1000);
        return;
      });
    },
    quit() {
      if (window.confirm(this.$t('troubleshooting.quit_hermes_confirm'))) {
        this.serverQuitting = true;
        fetch("./api/quit", {
          credentials: 'include',
          method: 'POST',
          headers: {
            'Content-Type': 'application/json'
          }
        })
        .then(() => {
          this.serverQuitting = false;
          this.serverQuit = false;
          alert("Exit failed!");
        })
        .catch(() => {
          this.serverQuitting = false;
          this.serverQuit = true;
        });
      }
    },
  },
}));

initApp(app);
