<script lang="ts">
import { defineComponent } from "vue"
import ResourceCard from './ResourceCard.vue'
import { apiFetch, logPageContext } from './diagnostics'

logPageContext('welcome')

export default defineComponent({
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
});
</script>

<template>
  <main role="main" style="max-width: 1200px; margin: 1em auto">
    <div class="d-flex gap-4">
      <div class="card p-2">
        <header>
          <h1 class="mb-0">
            <img src="/images/logo-hermes-45.png" height="45" alt="">
            {{ $t('welcome.greeting') }}
          </h1>
        </header>
        <p class="my-2 align-self-start">{{ $t('welcome.create_creds') }}</p>
        <div class="alert alert-warning">
          {{ $t('welcome.create_creds_alert') }}
        </div>
        <form @submit.prevent="save">
          <div class="mb-2">
            <label for="usernameInput" class="form-label">{{ $t('_common.username') }}</label>
            <input type="text" class="form-control" id="usernameInput" autocomplete="username"
              v-model="passwordData.newUsername" />
          </div>
          <div class="mb-2">
            <label for="passwordInput" class="form-label">{{ $t('_common.password') }}</label>
            <input type="password" class="form-control" id="passwordInput" autocomplete="new-password"
              v-model="passwordData.newPassword" required />
          </div>
          <div class="mb-2">
            <label for="confirmPasswordInput" class="form-label">{{ $t('welcome.confirm_password') }}</label>
            <input type="password" class="form-control" id="confirmPasswordInput" autocomplete="new-password"
              v-model="passwordData.confirmNewPassword" required />
          </div>
          <button type="submit" class="btn btn-primary w-100 mb-2" v-bind:disabled="loading">
            {{ $t('welcome.login') }}
          </button>
          <div class="alert alert-danger" v-if="error"><b>{{ $t('_common.error') }}</b> {{error}}</div>
          <div class="alert alert-success" v-if="success">
            <b>{{ $t('_common.success') }}</b> {{ $t('welcome.welcome_success') }}
          </div>
        </form>
      </div>
      <div>
        <Resource-Card></Resource-Card>
      </div>
    </div>
  </main>
</template>
