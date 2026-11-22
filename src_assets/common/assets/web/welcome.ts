import { createApp, defineComponent } from "vue"
import ResourceCard from './ResourceCard.vue'
import { initApp } from './init'
import { apiFetch, logPageContext } from './diagnostics'

logPageContext('welcome')

let app = createApp(defineComponent({
  components: {
    ResourceCard
  },
  data() {
    return {
      error: null as string | null,
      success: false,
      loading: false,
      passwordData: {
        newUsername: "hermes",
        newPassword: "",
        confirmNewPassword: "",
      },
    };
  },
  methods: {
    save() {
      if (this.passwordData.newPassword !== this.passwordData.confirmNewPassword) {
        this.error = "Password mismatch";
        return;
      };
      this.error = null;
      this.loading = true;
      apiFetch("./api/password", {
        headers: {
          'Content-Type': 'application/json'
        },
        method: 'POST',
        body: JSON.stringify(this.passwordData),
      }, "create password").then((r) => {
        this.loading = false;
        if (r.status === 200) {
          r.json().then((rj: { status: boolean; error?: string }) => {
            this.success = rj.status;
            if (this.success === true) {
              setTimeout(() => {
                document.location.reload();
              }, 5000);
            } else {
              this.error = rj.error ?? null;
            }
          });
        } else {
          this.error = `Server returned ${r.status}`;
        }
      }).catch((e) => {
        // Without this the button stays disabled forever and nothing is shown.
        // loading was only cleared on the reply path, so a request that never
        // reaches the server left the page looking frozen, with no explanation
        // anywhere and nothing in the host log either.
        this.loading = false;
        this.error = `Could not reach Hermes: ${e.message}`;
      });
    },
  },
}));

initApp(app);
