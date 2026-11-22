import { createApp, defineComponent } from 'vue'
import { initApp } from './init'
import Navbar from './Navbar.vue'
import Checkbox from './Checkbox.vue'

interface HostAddress {
  hostAddr: string;
  hostPort: string | number;
}

interface ClientCommand {
  cmd: string;
  elevated: string | boolean;
}

interface SavedClient {
  name: string;
  uuid: string;
  display_mode: string;
  perm: number | string;
  connected: boolean;
  do?: ClientCommand[];
  undo?: ClientCommand[];
  allow_client_commands: boolean;
  always_use_virtual_display: boolean;
  enable_legacy_ordering: boolean;
}

interface Client extends Omit<SavedClient, 'perm'> {
  perm: number;
  editing: boolean;
  editPerm: number;
  editName: string;
  editDisplayMode: string;
  editAllowClientCommands: boolean;
  editAlwaysUseVirtualDisplay: boolean;
  editEnableLegacyOrdering: boolean;
  edit_do: ClientCommand[];
  edit_undo: ClientCommand[];
}

interface RunningSession {
  id: number;
  uuid: string;
  client_name: string;
  app_name: string;
  running: boolean;
  streaming: boolean;
  profile: string;
  compositor: string;
  seat: string;
  account_user?: string;
  account_uid?: number;
  uptime_seconds: number;
}

declare const QRCode: new (container: HTMLElement) => {
  clear(): void;
  makeCode(url: string): void;
};

let resetOTPTimeout: number | undefined;
const qrContainer = document.createElement('div');
qrContainer.className = "mb-2 p-2 bg-white"
const qrCode = new QRCode(qrContainer);

const updateQR = (url: string) => {
  qrCode.clear()
  qrCode.makeCode(url)

  const refContainer = document.querySelector('#qrRef');
  if (refContainer) refContainer.appendChild(qrContainer);
}

function readHostCache(): HostAddress | null {
  const saved = sessionStorage.getItem('hostInfo');
  if (!saved) return null;
  try {
    const parsed: Partial<HostAddress> | null = JSON.parse(saved);
    if (typeof parsed?.hostAddr === 'string' && (typeof parsed.hostPort === 'string' || typeof parsed.hostPort === 'number')) {
      return { hostAddr: parsed.hostAddr, hostPort: parsed.hostPort };
    }
  } catch (error) {
    console.warn('Could not read the saved host address', error);
  }
  return null;
}

let hostInfoCache = readHostCache();
let hostManuallySet = false;

if (hostInfoCache) hostManuallySet = true;

const saveHostCache = ({hostAddr, hostPort}: HostAddress, manual = false) => {
  hostInfoCache = {hostAddr, hostPort}
  if (manual) {
    sessionStorage.setItem('hostInfo', JSON.stringify(hostInfoCache))
    hostManuallySet = true;
  }
}

/**
 * Permissions:
 enum class PERM: uint32_t {
    _reserved        = 1,

    _input           = _reserved << 8,   // Input permission group
    input_controller = _input << 0,      // Allow controller input
    input_touch      = _input << 1,      // Allow touch input
    input_pen        = _input << 2,      // Allow pen input
    input_mouse      = _input << 3,      // Allow mouse input
    input_kbd        = _input << 4,      // Allow keyboard input
    _all_inputs      = input_controller | input_touch | input_pen | input_mouse | input_kbd,

    _operation       = _input << 8,      // Operation permission group
    clipboard_set    = _operation << 0,  // Allow set clipboard from client
    clipboard_read   = _operation << 1,  // Allow read clipboard from host
    file_upload      = _operation << 2,  // Allow upload files to host
    file_dwnload     = _operation << 3,  // Allow download files from host
    server_cmd       = _operation << 4,  // Allow execute server cmd
    _all_opeiations  = clipboard_set | clipboard_read | file_upload | file_dwnload | server_cmd,

    _action          = _operation << 8,  // Action permission group
    list             = _action << 0,     // Allow list apps
    view             = _action << 1,     // Allow view streams
    launch           = _action << 2,     // Allow launch apps
    _allow_view      = view | launch,    // Launch contains view permission
    _all_actions     = list | view | launch,

    _default         = view | list,      // Default permissions for new clients
    _no              = 0,                // No permissions are granted
    _all             = _all_inputs | _all_opeiations | _all_actions, // All current permissions
  };
*/

const permissionMapping = {
  // Input permission group
  input_controller: 0x00000100,
  input_touch: 0x00000200,
  input_pen: 0x00000400,
  input_mouse: 0x00000800,
  input_kbd: 0x00001000,
  _all_inputs: 0x00001F00,

  // Operation permission group
  clipboard_set: 0x00010000,
  clipboard_read: 0x00020000,
  file_upload: 0x00040000,
  file_dwnload: 0x00080000,
  server_cmd: 0x00100000,
  _all_operations: 0x001F0000,

  // Action permission group
  list: 0x01000000,
  view: 0x02000000,
  launch: 0x04000000,
  _allow_view: 0x06000000,
  _all_actions: 0x07000000,

  // Special permissions
  _default: 0x03000000,
  _no: 0x00000000,
  _all: 0x071F1F00
};

type Permission = keyof typeof permissionMapping;

const permissionGroups = [
  { name: 'Action', permissions: [
    {
      name: 'list',
      suppressed_by: ['view', 'launch']
    }, {
      name: 'view',
      suppressed_by: ['launch']
    }, {
      name: 'launch',
      suppressed_by: []
    }
  ] },
  { name: 'Operation', permissions: [
    {
      name: 'clipboard_set',
      suppressed_by: []
    },
    {
      name: 'clipboard_read',
      suppressed_by: []
    },
    {
      name: 'server_cmd',
      suppressed_by: []
    }
  ] },
  { name: 'Input', permissions: [
    {
      name: 'input_controller',
      suppressed_by: []
    }, {
      name: 'input_touch',
      suppressed_by: []
    }, {
      name: 'input_pen',
      suppressed_by: []
    }, {
      name: 'input_mouse',
      suppressed_by: []
    }, {
      name: 'input_kbd',
      suppressed_by: []
    }
  ] },
] satisfies { name: string; permissions: { name: Permission; suppressed_by: Permission[] }[] }[];

let currentEditingClient: Client | null = null;

const data = () => {
  return {
    platform: '',
    editingHost: false,
    currentTab: location.hash || '#OTP',
    pin: '',
    pinName: '',
    pairStatus: null as 'pending' | 'failure' | null,
    otp: '',
    passphrase: '',
    otpMessage: '',
    otpStatus: 'warning',
    deviceName: '',
    hostAddr: '',
    hostPort: '' as HostAddress['hostPort'],
    hostName: '',
    permissionGroups,
    clients: [] as Client[],
    sessions: [] as RunningSession[],
    sessionsSupported: false,
    endingSession: null as string | null,
    sessionTimer: undefined as number | undefined,
    showApplyMessage: false,
    unpairAllPressed: false,
    unpairAllStatus: null as boolean | null
  }
}

const cmdTpl: ClientCommand = {
  cmd: '',
  elevated: 'false'
}

const app = createApp(defineComponent({
  components: {
    Navbar,
    Checkbox
  },
  data,
  computed: {
    deepLink() {
      return encodeURI(`art://${this.hostAddr}:${this.hostPort}?pin=${this.otp}&passphrase=${this.passphrase}&name=${this.hostName}`);
    },
    canSaveHost() {
      return !!(this.hostAddr && this.hostPort);
    }
  },
  created() {
    this.refreshClients();
    this.refreshSessions();
  },
  mounted() {
    // A session's state and its uptime both move without anybody touching
    // this page, so the list refreshes itself rather than going stale behind
    // a reload the reader has no reason to think they need.
    this.sessionTimer = window.setInterval(() => this.refreshSessions(), 5000);
  },
  beforeUnmount() {
    clearInterval(this.sessionTimer);
  },
  methods: {
    switchTab(currentTab: string) {
      location.hash = currentTab;
      const clients = this.clients;
      Object.assign(this, data(), { clients });
      hostInfoCache = null;
      clearTimeout(resetOTPTimeout);
    },
    editHost() {
      this.editingHost = !this.editingHost;
      Object.assign(this, hostInfoCache);
    },
    saveHost() {
      if (!this.canSaveHost) return;
      updateQR(this.deepLink);
      this.editingHost = false;
      saveHostCache(this, true);
    },
    async registerDevice(): Promise<void> {
      this.pairStatus = null;
      try {
        const response = await fetch("./api/pin", {
          credentials: 'include',
          headers: { 'Content-Type': 'application/json' },
          method: 'POST',
          body: JSON.stringify({ pin: this.pin, name: this.pinName }),
        });
        const result: { status?: boolean } = await response.json();
        if (result.status !== true) {
          this.pairStatus = 'failure';
          return;
        }
        // Status is delivery to the paired handshake, not confirmation that
        // the client completed its cryptographic challenge.
        this.pairStatus = 'pending';
        this.pin = '';
        this.pinName = '';
        window.setTimeout(() => this.refreshClients(), 1000);
      } catch {
        this.pairStatus = 'failure';
      }
    },
    requestOTP() {
      if (this.editingHost) return;

      fetch("./api/otp", {
        credentials: 'include',
        headers: {
          'Content-Type': 'application/json'
        },
        method: 'POST',
        body: JSON.stringify({ passphrase: this.passphrase, deviceName: this.deviceName })
      })
      .then(resp => resp.json())
      .then(resp => {
        if (!resp.status) {
          this.otpMessage = resp.message
          this.otpStatus = 'danger'
          return
        }

        this.otp = resp.otp
        this.hostName = resp.name
        this.otpStatus = 'success'
        this.otpMessage = this.$t('pin.otp_success')

        const isLocalHost = ['localhost', '127.0.0.1', '[::1]'].indexOf(location.hostname) >= 0

        if (hostManuallySet) {
          Object.assign(this, hostInfoCache);
        } else {
          this.hostAddr = resp.ip
          this.hostPort = parseInt(location.port, 10) - 1

          if (!isLocalHost) {
            this.hostAddr = location.hostname
          }

          saveHostCache(this);
        }

        if (this.hostAddr) {
          updateQR(this.deepLink);

          clearTimeout(resetOTPTimeout)
          resetOTPTimeout = window.setTimeout(() => {
            Object.assign(this, data(), {
              otp: this.$t('pin.otp_expired'),
              otpMessage: this.$t('pin.otp_expired_msg')
            })
            resetOTPTimeout = undefined
          }, 3 * 60 * 1000)

          if (!isLocalHost) {
            setTimeout(() => {
              if (window.confirm(this.$t('pin.otp_pair_now'))) {
                window.open(this.deepLink);
              }
            }, 0)
          }
        }
      })
    },
    clickedApplyBanner() {
      this.showApplyMessage = false;
    },
    addCmd(arr: ClientCommand[], idx: number) {
      const newCmd = Object.assign({}, cmdTpl);
      if (idx < 0) {
        arr.push(newCmd);
      } else {
        arr.splice(idx + 1, 0, newCmd);
      }
    },
    removeCmd(arr: ClientCommand[], idx: number) {
      arr.splice(idx, 1);
    },
    editClient(client: Client) {
      if (currentEditingClient) {
        this.cancelEdit(currentEditingClient);
      }
      client.editing = true;
      client.editPerm = client.perm;
      client.editName = client.name;
      client.editAllowClientCommands = client.allow_client_commands;
      client.editEnableLegacyOrdering = client.enable_legacy_ordering;
      client.editAlwaysUseVirtualDisplay = client.always_use_virtual_display;
      client.editDisplayMode = client.display_mode;
      client.edit_do = JSON.parse(JSON.stringify(client.do || []));
      client.edit_undo = JSON.parse(JSON.stringify(client.undo || []));
      currentEditingClient = client;

      console.log(client.do, client.undo)
    },
    cancelEdit(client: Client) {
      currentEditingClient = null;
      client.editing = false;
      client.editPerm = client.perm;
      client.editName = client.name;
      client.editDisplayMode = client.display_mode;
      client.editAllowClientCommands = client.allow_client_commands;
      client.editEnableLegacyOrdering = client.enable_legacy_ordering;
      client.editAlwaysUseVirtualDisplay = client.always_use_virtual_display;
    },
    saveClient(client: Client) {
      client.editing = false;
      currentEditingClient = null;
      const editedClient = {
        uuid: client.uuid,
        name: client.editName,
        display_mode: client.editDisplayMode.trim(),
        allow_client_commands: client.editAllowClientCommands,
        enable_legacy_ordering: client.editEnableLegacyOrdering,
        always_use_virtual_display: client.editAlwaysUseVirtualDisplay,
        perm: client.editPerm & permissionMapping._all,
        do: client.edit_do.reduce<ClientCommand[]>((filtered, {cmd: _cmd, elevated}) => {
          const cmd = _cmd.trim()
          if (cmd) {
            filtered.push({
              cmd,
              elevated
            })
          }
          return filtered
        }, []),
        undo: client.edit_undo.reduce<ClientCommand[]>((filtered, {cmd: _cmd, elevated}) => {
          const cmd = _cmd.trim()
          if (cmd) {
            filtered.push({
              cmd,
              elevated
            })
          }
          return filtered
        }, [])
      }
      fetch("./api/clients/update", {
        credentials: 'include',
        headers: {
          'Content-Type': 'application/json'
        },
        method: 'POST',
        body: JSON.stringify(editedClient)
      })
      .then(resp => resp.json())
      .then(resp => {
        if (resp.status) {
          this.refreshClients();
        } else {
          throw new Error(resp.message);
        }
      })
      .catch(err => {
        alert(this.$t('pin.save_client_error') + err);
      })
      .finally(() => {
        setTimeout(() => {
          this.refreshClients();
        }, 1000);
      });
    },
    permToStr(perm: number) {
      const permSegments = [];
      permSegments.push((perm >> 24) & 0xFF);
      permSegments.push((perm >> 16) & 0xFF);
      permSegments.push((perm >> 8) & 0xFF);
      return permSegments.map(seg => seg.toString(16).toUpperCase().padStart(2, '0')).join(' ');
    },
    checkPermission(perm: number, permission: Permission) {
      return (perm & permissionMapping[permission]) !== 0;
    },
    isSuppressed(perm: number, _permission: Permission, suppressed_by: Permission[]) {
      for (const suppressed of suppressed_by) {
        if (this.checkPermission(perm, suppressed)) {
          return true;
        }
      }
      return false;
    },
    togglePermission(client: Client, permission: Permission) {
      client.editPerm ^= permissionMapping[permission];
    },
    validateModeOverride(event: Event) {
      const input = event.target;
      if (!(input instanceof HTMLInputElement)) return;
      const value = input.value.trim();
      if (value && !value.match(/^\d+x\d+x\d+(\.\d+)?$/)) {
        input.setCustomValidity(this.$t('pin.display_mode_override_error'));
      } else {
        input.setCustomValidity('');
      }

      input.reportValidity();
    },
    disconnectClient(uuid: string) {
      fetch("./api/clients/disconnect", {
        credentials: 'include',
        headers: {
          'Content-Type': 'application/json'
        },
        method: 'POST',
        body: JSON.stringify({ uuid })
      }).finally(() => {
        setTimeout(() => {
          this.refreshClients();
        }, 1000);
      });
    },
    unpairAll() {
      this.unpairAllPressed = true;
      fetch("./api/clients/unpair-all", {
        credentials: 'include',
        method: 'POST',
        headers: {
          'Content-Type': 'application/json'
        },
      })
        .then((r) => r.json())
        .then((r) => {
          this.unpairAllPressed = false;
          this.unpairAllStatus = r.status;
          setTimeout(() => {
            this.unpairAllStatus = null;
          }, 5000);
          this.refreshClients();
        });
    },
    unpairSingle(uuid: string) {
      fetch("./api/clients/unpair", {
        credentials: 'include',
        headers: {
          'Content-Type': 'application/json'
        },
        method: 'POST',
        body: JSON.stringify({ uuid })
      }).then(() => {
        this.showApplyMessage = true;
        this.refreshClients();
      });
    },
    formatUptime(seconds: number) {
      const total = Math.max(0, Math.floor(seconds || 0));
      const hours = Math.floor(total / 3600);
      const minutes = Math.floor((total % 3600) / 60);
      if (hours > 0) return `${hours}h ${minutes}m`;
      if (minutes > 0) return `${minutes}m`;
      return `${total}s`;
    },
    refreshSessions() {
      fetch("./api/sessions/list", { credentials: 'include' })
        .then((response) => response.json())
        .then((response) => {
          this.sessionsSupported = !!response.supported;
          this.sessions = response.sessions || [];
        })
        .catch((e) => {
          console.error(e);
        });
    },
    endSession(session: RunningSession) {
      const name = session.client_name || this.$t('pin.unpair_single_unknown');
      // Confirmed because this is not recoverable from the client's side: it
      // ends somebody's desktop, and anything unsaved in it goes with it.
      if (!confirm(this.$t('pin.sessions_end_confirm', { name }))) {
        return;
      }
      this.endingSession = session.uuid;
      fetch("./api/sessions/terminate", {
        credentials: 'include',
        headers: {
          'Content-Type': 'application/json'
        },
        method: 'POST',
        body: JSON.stringify({ uuid: session.uuid })
      })
        .then((r) => r.json())
        .then((r) => {
          if (!r.status) {
            alert(this.$t('pin.sessions_end_error') + (r.error || ''));
          }
        })
        .catch((e) => {
          alert(this.$t('pin.sessions_end_error') + e);
        })
        .finally(() => {
          this.endingSession = null;
          setTimeout(() => {
            this.refreshSessions();
            this.refreshClients();
          }, 1000);
        });
    },
    refreshClients() {
      if (currentEditingClient) {
        this.cancelEdit(currentEditingClient);
      }
      fetch("./api/clients/list", { credentials: 'include' })
        .then((response) => response.json())
        .then((response: { status: boolean; named_certs?: SavedClient[]; platform: string }) => {
          if (response.status && response.named_certs && response.named_certs.length) {
            this.platform = response.platform
            this.clients = response.named_certs.map(({
              name,
              uuid,
              display_mode,
              perm,
              connected,
              do: _do,
              undo,
              allow_client_commands,
              always_use_virtual_display,
              enable_legacy_ordering
            }) => {
              const permInt = Number.parseInt(String(perm), 10);
              return {
                name,
                uuid,
                display_mode,
                perm: permInt,
                connected,
                editing: false,
                editPerm: permInt,
                editName: name,
                editDisplayMode: display_mode,
                editAllowClientCommands: allow_client_commands,
                editAlwaysUseVirtualDisplay: always_use_virtual_display,
                editEnableLegacyOrdering: enable_legacy_ordering,
                edit_do: [],
                edit_undo: [],
                do: _do,
                undo,
                allow_client_commands,
                enable_legacy_ordering,
                always_use_virtual_display
              }
            })
            currentEditingClient = null;
          } else {
            this.clients = [];
          }
        })
        .catch(e => {
          console.error(e)
          location.reload();
        });
    },
  }
}));

initApp(app);
