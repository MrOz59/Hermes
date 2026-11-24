<script lang="ts">
import { computed, defineComponent } from 'vue'
import Navbar from './Navbar.vue'
import General from './configs/tabs/General.vue'
import Inputs from './configs/tabs/Inputs.vue'
import Network from './configs/tabs/Network.vue'
import Files from './configs/tabs/Files.vue'
import Advanced from './configs/tabs/Advanced.vue'
import AudioVideo from './configs/tabs/AudioVideo.vue'
import ContainerEncoders from './configs/tabs/ContainerEncoders.vue'
import {$tp, usePlatformI18n} from './platform-i18n'

import type { ClipboardStatus, Configuration, ConfigurationResponse, EvdiStatus, HermesKmsStatus, PreparationCommand, ServerCommand } from './config-types'

interface ConfigurationTab {
  id: string;
  name: string;
  options: Configuration;
}

let fallbackDisplayModeCache = "";

export default defineComponent({
  components: {
    Navbar,
    General,
    Inputs,
    Network,
    Files,
    Advanced,
    // They will be accessible via audio-video, container-encoders only.
    AudioVideo,
    ContainerEncoders,
  },
  data() {
    return {
      platform: "",
      saved: false,
      restarted: false,
      config: null as Configuration | null,
      currentTab: "general",
      evdiSetupRequired: false,
      evdiDiagnostic: "",
      evdiInfo: null as EvdiStatus | null,
      hermesKmsDiagnostic: "",
      hermesKmsInfo: null as HermesKmsStatus | null,
      clipboardInfo: null as ClipboardStatus | null,
      global_prep_cmd: [] as PreparationCommand[],
      global_state_cmd: [] as PreparationCommand[],
      server_cmd: [] as ServerCommand[],
      tabs: [ // TODO: Move the options to each Component instead, encapsulate.
        {
          id: "general",
          name: "General",
          options: {
            "locale": "en",
            "sunshine_name": "",
            "min_log_level": 2,
            "global_prep_cmd": [],
            "global_state_cmd": [],
            "server_cmd": [],
            "notify_pre_releases": "disabled",
            "announcements": "enabled",
            "system_tray": "enabled",
            "hide_tray_controls": "disabled",
            "enable_pairing": "enabled",
            "enable_discovery": "enabled",
            "inhibit_screen_lock": "disabled",
          },
        },
        {
          id: "input",
          name: "Input",
          options: {
            "controller": "enabled",
            "gamepad": "auto",
            "linux_gamepad_backend": "auto",
            "ds4_back_as_touchpad_click": "enabled",
            "motion_as_ds4": "enabled",
            "touchpad_as_ds4": "enabled",
            "virtualhid_randomize_mac": "enabled",
            "virtualhid_stable_mac": "enabled",
            "back_button_timeout": -1,
            "keyboard": "enabled",
            "key_repeat_delay": 500,
            "key_repeat_frequency": 24.9,
            "key_rightalt_to_key_win": "disabled",
            "mouse": "enabled",
            "high_resolution_scrolling": "enabled",
            "native_pen_touch": "enabled",
            "isolated_ignore_mouse_wiggle": "enabled",
            "isolated_hide_hid_gamepads": "disabled",
            "enable_input_only_mode": "disabled",
            "forward_rumble": "enabled",
            "keybindings": "[0x10,0xA0,0x11,0xA2,0x12,0xA4]",  // todo: add this to UI
          },
        },
        {
          id: "av",
          name: "Audio/Video",
          options: {
            "audio_sink": "",
            "stream_audio": "enabled",
            "stream_mic": "disabled",
            "keep_sink_default": "enabled",
            "auto_capture_sink": "enabled",
            "adapter_name": "",
            "output_name": "",
            "virtual_display_backend": "evdi",
            "hermes_kms_multi_output": "disabled",
            "hermes_kms_isolated_sessions": "disabled",
            "gamescope_backend": "auto",
            "hermes_kms_session_compositor": "plasma",
            "isolation_method": "gamescope",
            "fallback_mode": "",
            "default_scale_factor": 100,
            "dd_configuration_option": "disabled",
            "dd_resolution_option": "auto",
            "dd_manual_resolution": "",
            "dd_refresh_rate_option": "auto",
            "dd_manual_refresh_rate": "",
            "dd_hdr_option": "auto",
            "dd_wa_hdr_toggle_delay": 0,
            "dd_config_revert_delay": 3000,
            "dd_config_revert_on_disconnect": "disabled",
            "dd_mode_remapping": {"mixed": [], "resolution_only": [], "refresh_rate_only": []},
            "dd_wa_hdr_toggle": "disabled",
            "headless_mode": "disabled",
            "double_refreshrate": "disabled",
            "max_bitrate": 0,
            "minimum_fps_target": 0,
            "isolated_virtual_display_option": "disabled",
          },
        },
        {
          id: "network",
          name: "Network",
          options: {
            "upnp": "disabled",
            // Must match the server default in config.cpp. The save handler
            // drops any value equal to the default here, so a mismatch makes
            // the option unreachable: picking the server's real default
            // writes nothing, and the page shows a setting the host is not
            // using.
            "address_family": "both",
            "port": 47989,
            "origin_web_ui_allowed": "lan",
            "external_ip": "",
            "lan_encryption_mode": 0,
            "wan_encryption_mode": 1,
            "ping_timeout": 10000,
          },
        },
        {
          id: "files",
          name: "Config Files",
          options: {
            "file_apps": "",
            "credentials_file": "",
            "log_path": "",
            "pkey": "",
            "cert": "",
            "file_state": "",
          },
        },
        {
          id: "advanced",
          name: "Advanced",
          options: {
            "fec_percentage": 20,
            "adaptive_fec": "enabled",
            "adaptive_fec_max_percentage": 50,
            "qp": 28,
            "min_threads": 2,
            "limit_framerate": "enabled",
            "envvar_compatibility_mode": "disabled",
            "legacy_ordering": "disabled",
            "ignore_encoder_probe_failure": "disabled",
            "pyrowave": "disabled",
            "hevc_mode": 0,
            "av1_mode": 0,
            "capture": "",
            "encoder": "",
          },
        },
        {
          id: "nv",
          name: "NVIDIA NVENC Encoder",
          options: {
            "nvenc_preset": 1,
            "nvenc_twopass": "quarter_res",
            "nvenc_spatial_aq": "disabled",
            "nvenc_vbv_increase": 0,
            "nvenc_h264_cavlc": "disabled",
            "nvenc_intra_refresh": "disabled"
          },
        },
        {
          id: "vaapi",
          name: "VA-API Encoder",
          options: {
            "vaapi_strict_rc_buffer": "disabled",
            "vaapi_quality": -1,
            "vaapi_intra_refresh": "enabled",
          },
        },
        {
          id: "sw",
          name: "Software Encoder",
          options: {
            "sw_preset": "superfast",
            "sw_tune": "zerolatency",
          },
        },
      ] satisfies ConfigurationTab[],
    };
  },
  provide() {
     return {
       platform: computed(() => this.platform)
     }
  },
  created() {
    fetch("./api/config", {
      credentials: 'include'
    })
      .then((r) => r.json())
      .then((r: ConfigurationResponse) => {
        const { platform, evdiSetupRequired, evdiDiagnostic, evdiInfo, hermesKmsDiagnostic, hermesKmsInfo, clipboardInfo, ...editable } = r;
        this.platform = platform;
        this.evdiSetupRequired = evdiSetupRequired ?? false;
        this.evdiDiagnostic = evdiDiagnostic ?? "";
        this.evdiInfo = evdiInfo ?? null;
        this.hermesKmsDiagnostic = hermesKmsDiagnostic ?? "";
        this.hermesKmsInfo = hermesKmsInfo ?? null;
        this.clipboardInfo = clipboardInfo ?? null;

        // These fields are computed by the server and must not be saved.
        for (const field of ['status', 'version', 'version_commit', 'vdisplayStatus', 'preloginInfo']) {
          delete editable[field];
        }
        this.config = editable;

        fallbackDisplayModeCache = editable.fallback_mode || "";

        // TODO: let each tab's Component handle it's own data instead of doing it here

        // Parse the special options before population if available
        const specialOptions = ["dd_mode_remapping", "global_prep_cmd", "global_state_cmd", "server_cmd"]
        for (const optionKey of specialOptions) {
          if (typeof editable[optionKey] === "string") {
            editable[optionKey] = JSON.parse(editable[optionKey]);
          } else {
            editable[optionKey] = null;
          }
        }

        editable.dd_mode_remapping ??= {mixed: [], resolution_only: [], refresh_rate_only: []};
        editable.global_prep_cmd ??= [];
        editable.global_state_cmd ??= [];
        editable.server_cmd ??= [];

        // Populate default values from tabs options
        this.tabs.forEach((tab: ConfigurationTab) => {
          Object.keys(tab.options).forEach(optionKey => {
            if (editable[optionKey] === undefined) {
              // Make sure to copy by value
              editable[optionKey] = JSON.parse(JSON.stringify(tab.options[optionKey]));
            }
          });
        });

        this.global_prep_cmd = editable.global_prep_cmd as PreparationCommand[];
        this.global_state_cmd = editable.global_state_cmd as PreparationCommand[];
        this.server_cmd = editable.server_cmd as ServerCommand[];

      });
  },
  methods: {
    forceUpdate() {
      this.$forceUpdate()
    },
    serialize(): Configuration | null {
      if (!this.config) return null;
      // Validate fallback mode
      if (this.config.fallback_mode && !this.config.fallback_mode.match(/^\d+x\d+x\d+$/)) {
        this.config.fallback_mode = fallbackDisplayModeCache;
      } else {
        fallbackDisplayModeCache = this.config.fallback_mode ?? "";
      }
      const config: Configuration = JSON.parse(JSON.stringify(this.config));
      config.global_prep_cmd = this.global_prep_cmd.filter((cmd: PreparationCommand) => (cmd.do && cmd.do.trim()) || (cmd.undo && cmd.undo.trim()));
      config.global_state_cmd = this.global_state_cmd.filter((cmd: PreparationCommand) => (cmd.do && cmd.do.trim()) || (cmd.undo && cmd.undo.trim()));
      config.server_cmd = this.server_cmd.filter((cmd: ServerCommand) => (cmd.name && cmd.cmd && cmd.name.trim() && cmd.cmd.trim()));
      return config;
    },
    save(): Promise<boolean> {
      this.saved = false;
      this.restarted = false;

      // create a temp copy of this.config to use for the post request
      const config = this.serialize();
      if (!config) return Promise.resolve(false);

      // delete default values from this.config
      this.tabs.forEach((tab: ConfigurationTab) => {
        Object.keys(tab.options).forEach(optionKey => {
          let delete_value = false

          // todo: add proper type checking
          if (JSON.stringify(config[optionKey]) === JSON.stringify(tab.options[optionKey])) {
            delete_value = true
          }

          if (delete_value) {
            delete config[optionKey]
          }
        });
      });

      return fetch("./api/config", {
        credentials: 'include',
        headers: {
          'Content-Type': 'application/json'
        },
        method: 'POST',
        body: JSON.stringify(config),
      }).then((r) => {
        if (r.status === 200) {
          this.saved = true
          return this.saved
        }
        else {
          return false
        }
      });
    },
    apply() {
      this.saved = this.restarted = false;
      let saved = this.save();

      saved.then((result: boolean) => {
        if (result === true) {
          this.restarted = true;
          setTimeout(() => {
            this.saved = this.restarted = false;
          }, 5000);
          fetch("./api/restart", {
            method: 'POST',
            headers: {
              'Content-Type': 'application/json'
            }
          })
          .then((resp) => {
            if (resp.status !== 200) {
              location.reload();
              return
            }
          })
          .catch((e) => {
            console.error(e);
            setTimeout(() => {
              location.reload();
            }, 1000);
            return;
          });
        }
      });
    },
  },
  mounted() {
    // Handle hashchange events
    const handleHash = () => {
      let hash = window.location.hash;
      if (hash) {
        // remove the # from the hash
        let stripped_hash = hash.substring(1);

        this.tabs.forEach((tab: ConfigurationTab) => {
          Object.keys(tab.options).forEach(key => {
            if (tab.id === stripped_hash || key === stripped_hash) {
              this.currentTab = tab.id;
            }
            if (key === stripped_hash) {
              // sleep for 2 seconds to allow the page to load
              setTimeout(() => {
                let element = document.getElementById(stripped_hash);
                if (element) {
                  window.location.hash = hash;
                }
              }, 2000);
            }

            if (this.currentTab === tab.id) {
              // stop looping
              return true;
            }
          });
        });
      }
    };

    // Call handleHash for the initial load
    handleHash();

    // Add hashchange event listener
    window.addEventListener("hashchange", handleHash);
  },
});
</script>

<template>
  <Navbar></Navbar>
  <div class="container">
    <h1 class="my-4">{{ $t('config.configuration') }}</h1>
    <div class="form" v-if="config">
      <!-- Header -->
      <ul class="nav nav-tabs">
        <li class="nav-item" v-for="tab in tabs" :key="tab.id">
          <a class="nav-link" :class="{'active': tab.id === currentTab}" href="#"
            @click="currentTab = tab.id">{{tab.name}}</a>
        </li>
      </ul>

      <!-- General Tab -->
      <general
        v-if="currentTab === 'general'"
        :config="config"
        :global-prep-cmd="global_prep_cmd"
        :global-state-cmd="global_state_cmd"
        :server-cmd="server_cmd"
        :platform="platform">
      </general>

      <!-- Input Tab -->
      <inputs
        v-if="currentTab === 'input'"
        :config="config"
        :platform="platform">
      </inputs>

      <!-- Audio/Video Tab -->
      <audio-video
        v-if="currentTab === 'av'"
        :config="config"
        :platform="platform"
        :evdi-setup-required="evdiSetupRequired"
        :evdi-diagnostic="evdiDiagnostic"
        :evdi-info="evdiInfo"
        :hermes-kms-diagnostic="hermesKmsDiagnostic"
        :hermes-kms-info="hermesKmsInfo"
      >
      </audio-video>

      <!-- Network Tab -->
      <network
        v-if="currentTab === 'network'"
        :config="config"
        :platform="platform">
      </network>

      <!-- Files Tab -->
      <files
        v-if="currentTab === 'files'"
        :config="config"
        :platform="platform">
      </files>

      <!-- Advanced Tab -->
      <advanced
        v-if="currentTab === 'advanced'"
        :config="config"
        :platform="platform">
      </advanced>

      <container-encoders
        :current-tab="currentTab"
        :config="config"
        :platform="platform">
      </container-encoders>
    </div>

    <!-- Save and Apply buttons -->
    <div class="alert alert-success my-4" v-if="saved && !restarted">
      <b>{{ $t('_common.success') }}</b> {{ $t('config.apply_note') }}
    </div>
    <div class="alert alert-success my-4" v-if="restarted">
      <b>{{ $t('_common.success') }}</b> {{ $t('config.restart_note') }}
    </div>
    <div class="mb-3 buttons">
      <button class="btn btn-primary" @click="save">{{ $t('_common.save') }}</button>
      <button class="btn btn-success mx-2" @click="apply" v-if="saved && !restarted">{{ $t('_common.apply') }}</button>
    </div>
  </div>
</template>

<style>
.config-page {
        padding: 1em;
        border: 1px solid #dee2e6;
        border-top: none;
      }

      .pre-wrap {
        white-space: pre-wrap;
      }

      .buttons {
        padding: 1em 0;
      }
</style>
