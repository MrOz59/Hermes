import { createApp, defineComponent } from 'vue'
import { initApp } from './init'
import { apiFetch, logPageContext } from './diagnostics'
import Navbar from './Navbar.vue'

logPageContext('change password')

const app = createApp(defineComponent({
  components: {
    Navbar
  },
  data() {
    return {
      error: null as string | null,
      success: false,
      passwordData: {
        currentUsername: "",
        currentPassword: "",
        newUsername: "",
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
      apiFetch("./api/password", {
        credentials: 'include',
        headers: {
          'Content-Type': 'application/json'
        },
        method: 'POST',
        body: JSON.stringify(this.passwordData),
      }, "change password").then((r) => {
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
        // A request that never reaches the server otherwise leaves this form
        // silent: no success, no error, nothing in the host log to go on.
        this.error = `Could not reach Hermes: ${e.message}`;
      });
    },
  },
}));

initApp(app);
