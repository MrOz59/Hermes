import { createApp, defineComponent, reactive, ref } from "vue"
import { loadAutoTheme } from './theme'
import { initApp } from './init'
import { apiFetch, logPageContext } from './diagnostics'

logPageContext('login')

const app = createApp(defineComponent({
  setup() {
    loadAutoTheme()

    const error = ref<string | null>('');
    const success = ref(false);
    const loading = ref(false);
    const savePassword = ref(false);

    const data = {
      error,
      success,
      loading,
      savePassword,
      passwordData: reactive({
        username: '',
        password: ''
      })
    }

    const savedPasswordStr = localStorage.getItem('login')
    if (savedPasswordStr) {
      try {
        const { username, password }: { username: unknown; password: unknown } = JSON.parse(savedPasswordStr);
        if (typeof username === 'string' && typeof password === 'string') {
          savePassword.value = true;
          Object.assign(data.passwordData, { username, password });
        }
      } catch (e) {
        console.error('Reading saved password failed!', e);
      }
    }

    return data
  },
  methods: {
    login() {
      this.error = null;
      this.loading = true;
      if (!this.savePassword) {
        localStorage.removeItem('login')
      }
      apiFetch("./api/login", {
        headers: {
          'Content-Type': 'application/json'
        },
        method: 'POST',
        body: JSON.stringify(this.passwordData),
      }, "login").then((res) => {
        this.loading = false;
        if (res.status === 200) {
          this.success = true;
          if (this.savePassword) {
            localStorage.setItem('login', JSON.stringify(this.passwordData));
          }
          const url = new URL(window.location.href);
          const redirectUrl = url.searchParams.get('redir');
          const hash = url.hash;
          if (redirectUrl && redirectUrl.startsWith('.')) {
            location.href = redirectUrl + hash;
          } else {
            location.href = './' + hash;
          }
        } else {
          if (res.status === 401) {
            throw new Error('Please check your username and password')
          } else {
            throw new Error(`Server returned ${res.status}`);
          }
        }
      }).catch((e) => {
        this.error = `Login failed: ${e.message}`;
      });
    },
  },
}));

initApp(app);
