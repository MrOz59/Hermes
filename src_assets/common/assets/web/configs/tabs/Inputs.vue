<script setup>
import { ref } from 'vue'
import PlatformLayout from '../../PlatformLayout.vue'
import Checkbox from "../../Checkbox.vue";

const props = defineProps([
  'platform',
  'config'
])

const config = ref(props.config)
</script>

<template>
  <div id="input" class="config-page">
    <!-- Enable Gamepad Input -->
    <Checkbox class="mb-3"
              id="controller"
              locale-prefix="config"
              v-model="config.controller"
              default="true"
    ></Checkbox>

    <!-- Emulated Gamepad Type -->
    <div class="mb-3" v-if="config.controller === 'enabled' && platform !== 'macos'">
      <label for="gamepad" class="form-label">{{ $t('config.gamepad') }}</label>
      <select id="gamepad" class="form-select" v-model="config.gamepad">
        <option value="auto">{{ $t('_common.auto') }}</option>

        <PlatformLayout :platform="platform">
          <template #linux>
            <option value="generic">{{ $t("config.gamepad_generic") }}</option>
            <option value="x360">{{ $t("config.gamepad_x360") }}</option>
            <option value="xseries">{{ $t("config.gamepad_xseries") }}</option>
            <option value="ds4">{{ $t("config.gamepad_ds4") }}</option>
            <option value="ds5">{{ $t("config.gamepad_ds5") }}</option>
            <option value="switch">{{ $t("config.gamepad_switch") }}</option>
            <option value="xone">{{ $t("config.gamepad_xone") }}</option>
          </template>

          <template #windows>
            <option value="ds4">{{ $t('config.gamepad_ds4') }}</option>
            <option value="x360">{{ $t('config.gamepad_x360') }}</option>
          </template>
        </PlatformLayout>
      </select>
      <div class="form-text">{{ $t('config.gamepad_desc') }}</div>
    </div>

    <div class="mb-3" v-if="platform === 'linux' && config.controller === 'enabled'">
      <label for="linux_gamepad_backend" class="form-label">{{ $t('config.linux_gamepad_backend') }}</label>
      <select id="linux_gamepad_backend" class="form-select" v-model="config.linux_gamepad_backend">
        <option value="auto">{{ $t('_common.auto') }}</option>
        <option value="native">{{ $t('config.linux_gamepad_backend_native') }}</option>
        <option value="uinput">{{ $t('config.linux_gamepad_backend_uinput') }}</option>
      </select>
      <div class="form-text">{{ $t('config.linux_gamepad_backend_desc') }}</div>
      <div class="form-text text-warning" v-if="config.linux_gamepad_backend === 'uinput' && ['ds4', 'ds5', 'switch'].includes(config.gamepad)">
        {{ $t('config.linux_gamepad_backend_conflict') }}
      </div>
    </div>

    <!-- Additional options based on gamepad type -->
    <template v-if="config.controller === 'enabled'">
      <template v-if="config.gamepad === 'ds4' || config.gamepad === 'ds5' || (config.gamepad === 'auto' && platform !== 'macos')">
        <div class="mb-3 accordion">
          <div class="accordion-item">
            <h2 class="accordion-header">
              <button class="accordion-button" type="button" data-bs-toggle="collapse"
                      data-bs-target="#panelsStayOpen-collapseOne">
                {{ $t(config.gamepad === 'ds4' ? 'config.gamepad_ds4_manual' : (config.gamepad === 'ds5' ? 'config.gamepad_ds5_manual' : 'config.gamepad_auto')) }}
              </button>
            </h2>
            <div id="panelsStayOpen-collapseOne" class="accordion-collapse collapse show"
                 aria-labelledby="panelsStayOpen-headingOne">
              <div class="accordion-body">
                <!-- Automatic detection options (for Windows and Linux) -->
                <template v-if="config.gamepad === 'auto' && (platform === 'windows' || platform === 'linux')">
                  <!-- Gamepad with motion-capability as DS4(Windows)/DS5(Linux) -->
                  <Checkbox class="mb-3"
                            id="motion_as_ds4"
                            locale-prefix="config"
                            v-model="config.motion_as_ds4"
                            default="true"
                  ></Checkbox>
                  <!-- Gamepad with touch-capability as DS4(Windows)/DS5(Linux) -->
                  <Checkbox class="mb-3"
                            id="touchpad_as_ds4"
                            locale-prefix="config"
                            v-model="config.touchpad_as_ds4"
                            default="true"
                  ></Checkbox>
                </template>
                <!-- Back/Select can click any PlayStation-style virtual touchpad. -->
                <template v-if="config.gamepad === 'ds4' || (platform === 'linux' && config.gamepad === 'ds5') || (config.gamepad === 'auto' && (platform === 'windows' || platform === 'linux'))">
                  <Checkbox class="mb-3"
                            id="ds4_back_as_touchpad_click"
                            locale-prefix="config"
                            v-model="config.ds4_back_as_touchpad_click"
                            default="true"
                  ></Checkbox>
                </template>
                <!-- Linux PlayStation profiles share the MAC identity policy. -->
                <template v-if="platform === 'linux' && (config.gamepad === 'ds4' || config.gamepad === 'ds5' || config.gamepad === 'auto')">
                  <Checkbox class="mb-3"
                            id="virtualhid_randomize_mac"
                            locale-prefix="config"
                            v-model="config.virtualhid_randomize_mac"
                            default="true"
                  ></Checkbox>
                </template>
              </div>
            </div>
          </div>
        </div>
      </template>
    </template>

    <!-- Home/Guide Button Emulation Timeout -->
    <div class="mb-3" v-if="config.controller === 'enabled'">
      <label for="back_button_timeout" class="form-label">{{ $t('config.back_button_timeout') }}</label>
      <input type="text" class="form-control" id="back_button_timeout" placeholder="-1"
             v-model="config.back_button_timeout" />
      <div class="form-text">{{ $t('config.back_button_timeout_desc') }}</div>
    </div>

    <!-- Enable Keyboard Input -->
    <hr>
    <Checkbox class="mb-3"
              id="keyboard"
              locale-prefix="config"
              v-model="config.keyboard"
              default="true"
    ></Checkbox>

    <!-- Key Repeat Delay-->
    <div class="mb-3" v-if="config.keyboard === 'enabled' && platform === 'windows'">
      <label for="key_repeat_delay" class="form-label">{{ $t('config.key_repeat_delay') }}</label>
      <input type="text" class="form-control" id="key_repeat_delay" placeholder="500"
             v-model="config.key_repeat_delay" />
      <div class="form-text">{{ $t('config.key_repeat_delay_desc') }}</div>
    </div>

    <!-- Key Repeat Frequency-->
    <div class="mb-3" v-if="config.keyboard === 'enabled' && platform === 'windows'">
      <label for="key_repeat_frequency" class="form-label">{{ $t('config.key_repeat_frequency') }}</label>
      <input type="text" class="form-control" id="key_repeat_frequency" placeholder="24.9"
             v-model="config.key_repeat_frequency" />
      <div class="form-text">{{ $t('config.key_repeat_frequency_desc') }}</div>
    </div>

    <!-- Always send scancodes -->
    <Checkbox v-if="config.keyboard === 'enabled' && platform === 'windows'"
              class="mb-3"
              id="always_send_scancodes"
              locale-prefix="config"
              v-model="config.always_send_scancodes"
              default="true"
    ></Checkbox>

    <!-- Mapping Key AltRight to Key Windows -->
    <Checkbox v-if="config.keyboard === 'enabled'"
              class="mb-3"
              id="key_rightalt_to_key_win"
              locale-prefix="config"
              v-model="config.key_rightalt_to_key_win"
              default="false"
    ></Checkbox>

    <!-- Enable Mouse Input -->
    <hr>
    <Checkbox class="mb-3"
              id="mouse"
              locale-prefix="config"
              v-model="config.mouse"
              default="true"
    ></Checkbox>

    <!-- High resolution scrolling support -->
    <Checkbox v-if="config.mouse === 'enabled'"
              class="mb-3"
              id="high_resolution_scrolling"
              locale-prefix="config"
              v-model="config.high_resolution_scrolling"
              default="true"
    ></Checkbox>

    <!-- Native pen/touch support -->
    <Checkbox v-if="config.mouse === 'enabled'"
              class="mb-3"
              id="native_pen_touch"
              locale-prefix="config"
              v-model="config.native_pen_touch"
              default="true"
    ></Checkbox>

    <!-- Ignore the client's connect-time mouse wiggle in isolated sessions -->
    <Checkbox v-if="config.mouse === 'enabled'"
              class="mb-3"
              id="isolated_ignore_mouse_wiggle"
              locale-prefix="config"
              v-model="config.isolated_ignore_mouse_wiggle"
              default="true"
    ></Checkbox>

    <!-- Enable Input Only Mode -->
    <hr>
    <Checkbox class="mb-3"
              id="enable_input_only_mode"
              locale-prefix="config"
              v-model="config.enable_input_only_mode"
              default="false"
    ></Checkbox>

    <!-- Enable Rumble Messages to Controllers -->
    <hr>
    <Checkbox v-if="platform === 'windows'"
              class="mb-3"
              id="forward_rumble"
              locale-prefix="config"
              v-model="config.forward_rumble"
              default="true"
    ></Checkbox>

  </div>
</template>

<style scoped>

</style>
