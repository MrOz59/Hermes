<script setup>
import {ref, computed, inject, watch} from 'vue'
import {$tp} from '../../platform-i18n'
import PlatformLayout from '../../PlatformLayout.vue'
import AdapterNameSelector from './audiovideo/AdapterNameSelector.vue'
import DisplayOutputSelector from './audiovideo/DisplayOutputSelector.vue'
import DisplayDeviceOptions from "./audiovideo/DisplayDeviceOptions.vue";
import DisplayModesSettings from "./audiovideo/DisplayModesSettings.vue";
import Checkbox from "../../Checkbox.vue";

const $t = inject('i18n').t;

const props = defineProps([
  'platform',
  'config',
  'vdisplay',
  'evdiSetupRequired',
  'evdiDiagnostic',
  'evdiInfo',
  'min_fps_factor',
])

const sudovdaStatus = {
  '1': 'Unknown',
  '0': 'Ready',
  '-1': 'Uninitialized',
  '-2': 'Version Incompatible',
  '-3': 'Watchdog Failed'
}

const currentDriverStatus = computed(() => sudovdaStatus[props.vdisplay])

const evdiMessage = ref('')
const evdiInstallError = ref(false)
const evdiInstalling = ref(false)
const evdiDiagnostic = ref(props.evdiDiagnostic)
const evdiInfo = ref(props.evdiInfo || {})
const evdiInfoLoading = ref(false)
const clipboardInfo = ref({})
const clipboardLoading = ref(false)
const clipboardInstalling = ref(false)
const clipboardMessage = ref('')
const clipboardInstallError = ref(false)

watch(() => props.evdiDiagnostic, (diagnostic) => {
  evdiDiagnostic.value = diagnostic
})

watch(() => props.evdiInfo, (info) => {
  evdiInfo.value = info || {}
})

const refreshEvdiInfo = async () => {
  evdiInfoLoading.value = true
  try {
    const response = await fetch('./api/evdi/status', {credentials: 'include'})
    const result = await response.json()
    if (result.status) {
      evdiInfo.value = result.evdiInfo || {}
      evdiDiagnostic.value = evdiInfo.value.diagnostic || evdiDiagnostic.value
    }
  } finally {
    evdiInfoLoading.value = false
  }
}

const evdiState = computed(() => {
  if (props.evdiSetupRequired) return 'setup_required'
  return evdiDiagnostic.value || (props.vdisplay !== 0 ? 'module_not_loaded' : 'ready')
})

const evdiGuide = computed(() => {
  switch (evdiState.value) {
    case 'dkms_build_failed':
      return {
        title: 'config.evdi_dkms_build_failed_title',
        description: 'config.evdi_dkms_build_failed_desc',
        steps: ['config.evdi_dkms_update_step', 'config.evdi_dkms_rebuild_step', 'config.evdi_restart_step'],
        commands: ['sudo dkms autoinstall -k "$(uname -r)"', 'sudo modprobe evdi'],
      }
    case 'module_not_installed':
      return {
        title: 'config.evdi_module_not_installed_title',
        description: 'config.evdi_module_not_installed_desc',
        steps: ['config.evdi_install_step', 'config.evdi_restart_step'],
        commands: ['Arch/CachyOS: sudo pacman -S --needed evdi', 'Debian/Ubuntu: sudo apt install evdi-dkms libevdi1', 'sudo modprobe evdi initial_device_count=1'],
      }
    case 'module_not_loaded':
      return {
        title: 'config.evdi_module_not_loaded_title',
        description: 'config.evdi_module_not_loaded_desc',
        steps: ['config.evdi_module_not_loaded_step', 'config.evdi_restart_step'],
        commands: ['sudo modprobe evdi initial_device_count=1'],
      }
    case 'library_missing':
      return {
        title: 'config.evdi_library_missing_title',
        description: 'config.evdi_library_missing_desc',
        steps: ['config.evdi_install_step', 'config.evdi_restart_step'],
        commands: ['Arch/CachyOS: sudo pacman -S --needed evdi', 'Debian/Ubuntu: sudo apt install evdi-dkms libevdi1'],
      }
    default:
      return {
        title: 'config.evdi_setup_title',
        description: 'config.evdi_setup_desc',
        steps: ['config.evdi_restart_step'],
        commands: ['sudo modprobe evdi initial_device_count=1'],
      }
  }
})

const evdiActionLabel = computed(() => evdiState.value === 'setup_required'
  ? $t('config.evdi_setup_button')
  : evdiState.value === 'dkms_build_failed'
    ? $t('config.evdi_retry_button')
    : $t('config.evdi_install_button'))

const checkEvdiInstall = async () => {
  try {
    const response = await fetch('./api/evdi/install/status', {credentials: 'include'})
    const result = await response.json()
    evdiDiagnostic.value = result.evdiDiagnostic || evdiDiagnostic.value
    evdiInfo.value = result.evdiInfo || evdiInfo.value
    if (result.installStatus === 1) {
      evdiInstalling.value = true
      evdiMessage.value = $t('config.evdi_install_running')
      window.setTimeout(checkEvdiInstall, 1500)
    } else if (result.installStatus === 2) {
      evdiInstalling.value = false
      evdiInstallError.value = false
      evdiMessage.value = $t('config.evdi_install_succeeded')
    } else if (result.installStatus === 3) {
      evdiInstalling.value = false
      evdiInstallError.value = true
      evdiMessage.value = $t('config.evdi_install_failed_status')
    }
  } catch (_) {
    evdiInstalling.value = false
    evdiInstallError.value = true
    evdiMessage.value = $t('config.evdi_install_failed')
  }
}

const installEvdi = async () => {
  if (!window.confirm($t('config.evdi_install_confirm'))) return

  evdiMessage.value = ''
  evdiInstallError.value = false
  try {
    const response = await fetch('./api/evdi/install', {
      method: 'POST',
      credentials: 'include',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({confirm: true}),
    })
    const result = await response.json()
    evdiInstallError.value = !result.status
    if (result.status && result.installStatus === 'running') {
      evdiInstalling.value = true
      evdiMessage.value = $t('config.evdi_install_running')
      window.setTimeout(checkEvdiInstall, 1500)
    } else {
      evdiMessage.value = result.message || result.error || $t('config.evdi_install_failed')
    }
  } catch (_) {
    evdiInstallError.value = true
    evdiMessage.value = $t('config.evdi_install_failed')
  }
}

const refreshClipboardInfo = async () => {
  clipboardLoading.value = true
  try {
    const response = await fetch('./api/clipboard/status', {credentials: 'include'})
    const result = await response.json()
    if (result.status) {
      clipboardInfo.value = result.clipboardInfo || {}
      if (result.installStatus === 1) {
        clipboardInstalling.value = true
        window.setTimeout(refreshClipboardInfo, 1500)
      } else if (result.installStatus === 2) {
        clipboardInstalling.value = false
        clipboardInstallError.value = false
        clipboardMessage.value = 'Installation completed. Refresh this status after starting Hermes in your Wayland session.'
      } else if (result.installStatus === 3) {
        clipboardInstalling.value = false
        clipboardInstallError.value = true
        clipboardMessage.value = 'Installation failed. Use the manual commands below.'
      }
    }
  } catch (_) {
    clipboardInstallError.value = true
    clipboardMessage.value = 'Unable to read clipboard support status.'
  } finally {
    clipboardLoading.value = false
  }
}

const installClipboard = async () => {
  if (!window.confirm('Install Wayland and X11 clipboard support now? Hermes will ask your system for administrator permission.')) return

  clipboardMessage.value = ''
  clipboardInstallError.value = false
  try {
    const response = await fetch('./api/clipboard/install', {
      method: 'POST',
      credentials: 'include',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({confirm: true}),
    })
    const result = await response.json()
    if (result.status && result.installStatus === 'running') {
      clipboardInstalling.value = true
      clipboardMessage.value = 'Installation is running. Complete the system permission prompt if it appears.'
      window.setTimeout(refreshClipboardInfo, 1500)
    } else {
      clipboardInstallError.value = true
      clipboardMessage.value = result.error || result.message || 'Unable to start installation.'
    }
  } catch (_) {
    clipboardInstallError.value = true
    clipboardMessage.value = 'Unable to start installation.'
  }
}

if (props.platform === 'linux') {
  refreshClipboardInfo()
}

const config = ref(props.config)

const validateFallbackMode = (event) => {
  const value = event.target.value;
  if (!value.match(/^\d+x\d+x\d+(\.\d+)?$/)) {
    event.target.setCustomValidity($t('config.fallback_mode_error'));
  } else {
    event.target.setCustomValidity('');
  }

  event.target.reportValidity();
}
</script>

<template>
  <div id="audio-video" class="config-page">
    <!-- Audio Sink -->
    <div class="mb-3">
      <label for="audio_sink" class="form-label">{{ $t('config.audio_sink') }}</label>
      <input type="text" class="form-control" id="audio_sink"
             :placeholder="$tp('config.audio_sink_placeholder', 'alsa_output.pci-0000_09_00.3.analog-stereo')"
             v-model="config.audio_sink" />
      <div class="form-text pre-wrap">
        {{ $tp('config.audio_sink_desc') }}<br>
        <PlatformLayout :platform="platform">
          <template #windows>
            <pre>tools\audio-info.exe</pre>
          </template>
          <template #linux>
            <pre>pacmd list-sinks | grep "name:"</pre>
            <pre>pactl info | grep Source</pre>
          </template>
          <template #macos>
            <a href="https://github.com/mattingalls/Soundflower" target="_blank">Soundflower</a><br>
            <a href="https://github.com/ExistentialAudio/BlackHole" target="_blank">BlackHole</a>.
          </template>
        </PlatformLayout>
      </div>
    </div>

    <PlatformLayout :platform="platform">
      <template #windows>
        <!-- Virtual Sink -->
        <div class="mb-3">
          <label for="virtual_sink" class="form-label">{{ $t('config.virtual_sink') }}</label>
          <input type="text" class="form-control" id="virtual_sink" :placeholder="$t('config.virtual_sink_placeholder')"
                 v-model="config.virtual_sink" />
          <div class="form-text pre-wrap">{{ $t('config.virtual_sink_desc') }}</div>
        </div>
        <!-- Install Steam Audio Drivers -->
        <Checkbox class="mb-3"
                  id="install_steam_audio_drivers"
                  locale-prefix="config"
                  v-model="config.install_steam_audio_drivers"
                  default="true"
        ></Checkbox>

        <Checkbox class="mb-3"
                  id="keep_sink_default"
                  locale-prefix="config"
                  v-model="config.keep_sink_default"
                  default="true"
        ></Checkbox>

        <Checkbox class="mb-3"
                  id="auto_capture_sink"
                  locale-prefix="config"
                  v-model="config.auto_capture_sink"
                  default="true"
        ></Checkbox>
      </template>
    </PlatformLayout>

    <!-- Disable Audio -->
    <Checkbox class="mb-3"
              id="stream_audio"
              locale-prefix="config"
              v-model="config.stream_audio"
              default="true"
    ></Checkbox>

    <AdapterNameSelector
        :platform="platform"
        :config="config"
    />

    <DisplayOutputSelector
      :platform="platform"
      :config="config"
    />

    <div class="mb-3" v-if="platform === 'linux'">
      <label for="virtual_display_backend" class="form-label">{{ $t('config.virtual_display_backend') }}</label>
      <select id="virtual_display_backend" class="form-select" v-model="config.virtual_display_backend">
        <option value="evdi">{{ $t('config.virtual_display_backend_evdi') }}</option>
        <option value="hermes_kms">{{ $t('config.virtual_display_backend_hermes_kms') }}</option>
      </select>
      <div class="form-text">{{ $t('config.virtual_display_backend_desc') }}</div>
    </div>

    <DisplayDeviceOptions
      :platform="platform"
      :config="config"
    />

    <!-- Display Modes -->
    <DisplayModesSettings
        :platform="platform"
        :config="config"
    />

    <!-- Fallback Display Mode -->
    <div class="mb-3">
      <label for="fallback_mode" class="form-label">{{ $t('config.fallback_mode') }}</label>
      <input
        type="text"
        class="form-control"
        id="fallback_mode"
        v-model="config.fallback_mode"
        placeholder="1920x1080x60"
        @input="validateFallbackMode"
      />
      <div class="form-text">{{ $t('config.fallback_mode_desc') }}</div>
    </div>

    <!-- Headless Mode -->
    <Checkbox class="mb-3"
              id="headless_mode"
              locale-prefix="config"
              v-model="config.headless_mode"
              default="false"
              v-if="platform === 'windows'"
    ></Checkbox>

    <!-- Double Refreshrate -->
    <Checkbox class="mb-3"
              id="double_refreshrate"
              locale-prefix="config"
              v-model="config.double_refreshrate"
              default="false"
              v-if="platform === 'windows'"
    ></Checkbox>

    <!-- Isolated Virtual Display -->
    <Checkbox class="mb-3"
              id="isolated_virtual_display_option"
              locale-prefix="config"
              v-model="config.isolated_virtual_display_option"
              default="false"
              v-if="platform === 'windows' || platform === 'linux'"
    ></Checkbox>

    <!-- SudoVDA Driver Status -->
    <div class="alert" :class="[vdisplay ? 'alert-warning' : 'alert-success']" v-if="platform === 'windows'">
      <i class="fa-solid fa-xl fa-circle-info"></i> SudoVDA Driver status: {{currentDriverStatus}}
    </div>
    <div class="form-text" v-if="platform === 'windows' && vdisplay">Please ensure SudoVDA driver is installed to the latest version and enabled properly.</div>

    <section class="border-top pt-3 mt-4" v-if="platform === 'linux' && config.virtual_display_backend !== 'hermes_kms'">
      <div class="d-flex align-items-center justify-content-between mb-3">
        <h3 class="h6 mb-0">{{ $t('config.evdi_status_title') }}</h3>
        <button type="button" class="btn btn-outline-secondary btn-sm" :disabled="evdiInfoLoading" @click="refreshEvdiInfo" :title="$t('config.evdi_refresh')">
          <i class="fa-solid fa-arrows-rotate" :class="{'fa-spin': evdiInfoLoading}"></i>
        </button>
      </div>
      <dl class="row mb-0 small">
        <dt class="col-sm-4">{{ $t('config.evdi_status_diagnostic') }}</dt>
        <dd class="col-sm-8"><code>{{ evdiInfo.diagnostic || 'n/a' }}</code></dd>
        <dt class="col-sm-4">{{ $t('config.evdi_status_library') }}</dt>
        <dd class="col-sm-8">{{ evdiInfo.libraryInstalled ? $t('config.evdi_status_present') : $t('config.evdi_status_absent') }}<span v-if="evdiInfo.libraryVersion"> ({{ evdiInfo.libraryVersion }})</span></dd>
        <dt class="col-sm-4">{{ $t('config.evdi_status_kernel') }}</dt>
        <dd class="col-sm-8"><code>{{ evdiInfo.runningKernel || 'n/a' }}</code></dd>
        <dt class="col-sm-4">{{ $t('config.evdi_status_session') }}</dt>
        <dd class="col-sm-8"><code>{{ evdiInfo.sessionType || 'n/a' }}</code></dd>
        <dt class="col-sm-4">{{ $t('config.evdi_status_layout_backend') }}</dt>
        <dd class="col-sm-8"><code>{{ evdiInfo.outputLayoutBackend || 'n/a' }}</code></dd>
        <dt class="col-sm-4">{{ $t('config.evdi_status_module') }}</dt>
        <dd class="col-sm-8">{{ evdiInfo.moduleLoaded ? $t('config.evdi_status_loaded') : $t('config.evdi_status_not_loaded') }}</dd>
        <dt class="col-sm-4">{{ $t('config.evdi_status_devices') }}</dt>
        <dd class="col-sm-8">{{ evdiInfo.deviceCount ?? 'n/a' }}</dd>
        <dt class="col-sm-4">{{ $t('config.evdi_status_dkms') }}</dt>
        <dd class="col-sm-8">
          <span v-if="evdiInfo.dkmsKernels?.length">{{ evdiInfo.dkmsKernels.join(', ') }}</span>
          <span v-else>{{ $t('config.evdi_status_none') }}</span>
        </dd>
      </dl>
      <div class="mt-3">
        <div class="small fw-semibold mb-1">{{ $t('config.evdi_status_displays') }}</div>
        <div v-if="!evdiInfo.activeDisplays?.length" class="small text-body-secondary">{{ $t('config.evdi_status_none') }}</div>
        <ul v-else class="small mb-0 ps-3">
          <li v-for="display in evdiInfo.activeDisplays" :key="display.name">
            <code>{{ display.name }}</code> - {{ $t('config.evdi_status_device') }} {{ display.deviceIndex }}, card{{ display.drmCardIndex }}, {{ display.width }}x{{ display.height }}@{{ display.fps }}Hz, {{ $t('config.evdi_status_frames') }} {{ display.frameUpdates }}
            <span v-if="display.capturePath">, {{ display.capturePath }}</span>
          </li>
        </ul>
        <div v-if="evdiInfo.activeDisplays?.some(display => display.frameUpdates === 0)" class="alert alert-warning small mt-2 mb-0">
          {{ $t('config.evdi_no_frames_warning') }}
        </div>
        <div v-if="evdiInfo.activeDisplays?.some(display => display.zeroCopyCapture === false)" class="alert alert-warning small mt-2 mb-0">
          Hermes can still use hardware encoding, but EVDI exposes a CPU framebuffer rather than a GPU render node. Frames are copied through system memory before VAAPI encoding, which can increase latency compared with direct physical-display capture.
        </div>
      </div>
      <div v-if="evdiInfo.captureFallbackActive" class="alert alert-warning small mt-3 mb-0">
        {{ $t('config.evdi_capture_fallback_active') }}
      </div>
    </section>

    <section class="border-top pt-3 mt-4" v-if="platform === 'linux' && config.virtual_display_backend === 'hermes_kms'">
      <h3 class="h6 mb-2">Hermes-KMS</h3>
      <div class="alert alert-info small mb-0">
        Hermes-KMS is experimental. Hermes will ask the driver to connect <code>HERMES-1</code> for the stream session and keep the owner fd open until the session ends. EVDI setup checks are bypassed for this backend.
      </div>
    </section>

    <section class="border-top pt-3 mt-4" v-if="platform === 'linux'">
      <div class="d-flex align-items-center justify-content-between mb-3">
        <h3 class="h6 mb-0">Hermes Clipboard Support</h3>
        <button type="button" class="btn btn-outline-secondary btn-sm" :disabled="clipboardLoading" @click="refreshClipboardInfo" title="Refresh clipboard support status">
          <i class="fa-solid fa-arrows-rotate" :class="{'fa-spin': clipboardLoading}"></i>
        </button>
      </div>
      <dl class="row mb-0 small">
        <dt class="col-sm-4">Status</dt>
        <dd class="col-sm-8">{{ clipboardInfo.available ? 'Ready' : 'Unavailable' }}</dd>
        <dt class="col-sm-4">Diagnostic</dt>
        <dd class="col-sm-8"><code>{{ clipboardInfo.diagnostic || 'checking' }}</code></dd>
      </dl>
      <div v-if="clipboardInfo.diagnostic && !clipboardInfo.available" class="alert alert-warning small mt-3 mb-0">
        <p class="mb-2">Text clipboard sync uses <code>wl-copy</code>/<code>wl-paste</code> on Wayland or <code>xclip</code> on X11.</p>
        <div class="d-flex flex-wrap gap-2 mb-2">
          <button type="button" class="btn btn-warning btn-sm" :disabled="clipboardInstalling" @click="installClipboard">
            <i class="fa-solid fa-download"></i> Install clipboard support
          </button>
        </div>
        <div class="form-text">The installer is fixed to the detected package manager and always asks for system permission. You can instead install it manually:</div>
        <pre class="mb-0 mt-2"><code>{{ clipboardInfo.manualInstall }}</code></pre>
      </div>
      <div v-if="clipboardMessage" class="small mt-2" :class="clipboardInstallError ? 'text-danger' : 'text-success'">{{ clipboardMessage }}</div>
    </section>

    <div v-if="platform === 'linux' && config.virtual_display_backend !== 'hermes_kms' && config.isolated_virtual_display_option && !evdiInfo.exclusiveLayoutSupported" class="alert alert-warning mt-3">
      <i class="fa-solid fa-triangle-exclamation me-2"></i>{{ $t('config.evdi_exclusive_layout_unavailable') }}
    </div>

    <div class="alert alert-warning" v-if="platform === 'linux' && config.virtual_display_backend !== 'hermes_kms' && evdiState !== 'ready'">
      <div class="d-flex gap-2 align-items-center mb-2">
        <i class="fa-solid fa-xl fa-triangle-exclamation"></i>
        <strong>{{ $t(evdiGuide.title) }}</strong>
      </div>
      <p class="mb-2">{{ $t(evdiGuide.description) }}</p>
      <ol class="mb-3 ps-3">
        <li v-for="step in evdiGuide.steps" :key="step">{{ $t(step) }}</li>
        <li v-for="command in evdiGuide.commands" :key="command"><code>{{ command }}</code></li>
      </ol>
      <div class="d-flex flex-wrap gap-2">
        <button type="button" class="btn btn-warning" :disabled="evdiInstalling" @click="installEvdi">
          <i class="fa-solid fa-download"></i> {{ evdiActionLabel }}
        </button>
        <a class="btn btn-outline-secondary" href="https://github.com/DisplayLink/evdi" target="_blank" rel="noopener noreferrer">
          {{ $t('config.evdi_guide_button') }}
        </a>
      </div>
      <div class="form-text mt-2">{{ $t('config.evdi_permission_note') }}</div>
      <div v-if="evdiMessage" class="mt-2" :class="evdiInstallError ? 'text-danger' : 'text-success'">{{ evdiMessage }}</div>
    </div>

  </div>
</template>

<style scoped>
</style>
