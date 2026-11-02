/**
 * @file tests/unit/platform/test_virtualhid_input.cpp
 * @brief Linux input translation and independent-session regression tests.
 */
#ifdef __linux__
  #include "../../tests_common.h"
  #include "src/globals.h"
  #include "src/input.h"
  #include "src/platform/linux/input/virtualhid_common.h"
  #include "src/platform/linux/input/virtualhid_gamepad.h"
  #include "src/platform/linux/input/virtualhid_keyboard.h"
  #include "src/platform/linux/input/virtualhid_mouse.h"

  #include <algorithm>
  #include <array>
  #include <atomic>
  #include <chrono>
  #include <cstdlib>
  #include <fcntl.h>
  #include <filesystem>
  #include <fstream>
  #include <grp.h>
  #include <iostream>
  #include <limits>
  #include <linux/input.h>
  #include <moonlight-common-c/src/Input.h>
  #include <set>
  #include <sys/ioctl.h>
  #include <sys/stat.h>
  #include <sys/sysmacros.h>
  #include <sys/wait.h>
  #include <thread>
  #include <unistd.h>

namespace {
  using namespace std::chrono_literals;

  std::unique_ptr<lvh::Runtime> fake_runtime() {
    lvh::RuntimeOptions options;
    options.backend = lvh::BackendKind::fake;
    return lvh::Runtime::create(options);
  }

  class VirtualHidInputTest: public testing::Test {
  protected:
    config::input_t saved_config = config::input;
    safe::mail_t mailbox = std::make_shared<safe::mail_raw_t>();
    platf::feedback_queue_t feedback = mailbox->queue<platf::gamepad_feedback_msg_t>("feedback");

    void SetUp() override {
      config::input.gamepad = "ds5";
      config::input.linux_gamepad_backend = "native";
      config::input.virtualhid_randomize_mac = false;
      config::input.mouse = true;
      config::input.keyboard = true;
      config::input.controller = true;
      config::input.native_pen_touch = true;
    }

    void TearDown() override {
      config::input = std::move(saved_config);
    }
  };

  TEST_F(VirtualHidInputTest, RuntimeAndDeviceIdentityStayWithinTheirSession) {
    auto one = std::make_unique<platf::input_raw_t>("hermes-kms-1", fake_runtime());
    platf::input_raw_t two {"hermes-kms-12", fake_runtime()};
    platf::virtualhid::client_devices_t pen_one {"hermes-kms-1", fake_runtime()};
    platf::virtualhid::client_devices_t pen_two {"hermes-kms-12", fake_runtime()};
    ASSERT_TRUE(one->keyboard);
    ASSERT_TRUE(two.mouse);
    ASSERT_TRUE(pen_one.touch);
    ASSERT_TRUE(pen_two.pen);
    for (const auto *device : std::array<const lvh::VirtualDevice *, 4> {
           one->keyboard.get(),
           one->mouse.get(),
           pen_one.touch.get(),
           pen_one.pen.get()
         }) {
      EXPECT_EQ(device->profile().linux_phys, "hermes-kms-1");
      EXPECT_EQ(device->profile().vendor_id, VDISPLAY::VIRTUAL_INPUT_SETTINGS_VENDOR_ID);
      EXPECT_EQ(device->profile().product_id, VDISPLAY::VIRTUAL_INPUT_SETTINGS_PRODUCT_ID);
    }
    for (const auto *device : std::array<const lvh::VirtualDevice *, 4> {
           two.keyboard.get(),
           two.mouse.get(),
           pen_two.touch.get(),
           pen_two.pen.get()
         }) {
      EXPECT_EQ(device->profile().linux_phys, "hermes-kms-12");
    }
    EXPECT_EQ(one->keyboard->device_id(), two.keyboard->device_id());
    platf::keyboard::update(one.get(), 0x41, false, 0);
    EXPECT_EQ(one->keyboard->submit_count(), 1U);
    EXPECT_EQ(two.keyboard->submit_count(), 0U);
    one.reset();
    EXPECT_TRUE(two.keyboard->is_open());
    platf::keyboard::update(&two, 0x42, false, 0);
    EXPECT_EQ(two.keyboard->last_submitted_event().key_code, 0x42);
  }

  TEST_F(VirtualHidInputTest, MousePreservesCoordinatesButtonsAndHighResolutionScroll) {
    platf::input_raw_t raw {"hermes-kms-1", fake_runtime()};
    platf::mouse::move(&raw, -17, 23);
    auto event = raw.mouse->last_submitted_event();
    EXPECT_EQ(event.kind, lvh::MouseEventKind::relative_motion);
    EXPECT_EQ(event.x, -17);
    EXPECT_EQ(event.y, 23);
    platf::mouse::move_abs(&raw, {1920, 120, 5760, 2160}, 0.25F, 456.75F);
    event = raw.mouse->last_submitted_event();
    EXPECT_TRUE(event.has_fractional_absolute_coordinates);
    EXPECT_FLOAT_EQ(event.absolute_x, 1920.25F);
    EXPECT_FLOAT_EQ(event.absolute_y, 576.75F);
    EXPECT_EQ(event.width, 5760);
    EXPECT_EQ(event.height, 2160);
    const std::array buttons {lvh::MouseButton::left, lvh::MouseButton::middle, lvh::MouseButton::right, lvh::MouseButton::side, lvh::MouseButton::extra};
    const std::array wire_buttons {BUTTON_LEFT, BUTTON_MIDDLE, BUTTON_RIGHT, BUTTON_X1, BUTTON_X2};
    for (std::size_t index = 0; index < buttons.size(); ++index) {
      platf::mouse::button(&raw, wire_buttons[index], false);
      EXPECT_EQ(raw.mouse->last_submitted_event().button, buttons[index]);
      EXPECT_TRUE(raw.mouse->last_submitted_event().pressed);
      platf::mouse::button(&raw, wire_buttons[index], true);
      EXPECT_FALSE(raw.mouse->last_submitted_event().pressed);
    }
    platf::mouse::scroll(&raw, 37);
    EXPECT_EQ(raw.mouse->last_submitted_event().high_resolution_scroll, 37);
    platf::mouse::hscroll(&raw, -19);
    EXPECT_EQ(raw.mouse->last_submitted_event().kind, lvh::MouseEventKind::horizontal_scroll);
    EXPECT_EQ(raw.mouse->last_submitted_event().high_resolution_scroll, -19);
  }

  TEST_F(VirtualHidInputTest, KeyboardPreservesTransitionsFlagsAndUtf8Text) {
    platf::input_raw_t raw {"hermes-kms-1", fake_runtime()};
    platf::keyboard::update(&raw, 0xA5, false, 0);
    EXPECT_TRUE(raw.keyboard->last_submitted_event().uses_normalized_key_code);
    EXPECT_TRUE(raw.keyboard->last_submitted_event().pressed);
    platf::keyboard::update(&raw, 0xA5, true, SS_KBE_FLAG_NON_NORMALIZED);
    EXPECT_EQ(raw.keyboard->last_submitted_event().key_code, 0xA5);
    EXPECT_FALSE(raw.keyboard->last_submitted_event().uses_normalized_key_code);
    EXPECT_FALSE(raw.keyboard->last_submitted_event().pressed);
    const std::string text = "açã漢字";
    platf::keyboard::unicode(&raw, text.data(), static_cast<int>(text.size()));
    EXPECT_EQ(raw.keyboard->submit_count(), 3U);
  }

  TEST_F(VirtualHidInputTest, GamepadSelectionAndStatePreserveHermesContracts) {
    platf::input_raw_t raw {"hermes-kms-1", fake_runtime()};
    const std::array names {"generic", "x360", "xone", "xseries", "ds4", "ds5", "switch"};
    const std::array kinds {lvh::GamepadProfileKind::generic, lvh::GamepadProfileKind::xbox_360, lvh::GamepadProfileKind::xbox_one, lvh::GamepadProfileKind::xbox_series, lvh::GamepadProfileKind::dualshock4, lvh::GamepadProfileKind::dualsense, lvh::GamepadProfileKind::switch_pro};
    for (std::size_t index = 0; index < names.size(); ++index) {
      config::input.gamepad = names[index];
      ASSERT_EQ(platf::gamepad::alloc(&raw, {0, 3}, {}, feedback), 0);
      const auto &adapter = raw.gamepads[0]->adapter;
      EXPECT_EQ(adapter->gamepad()->profile().gamepad_kind, kinds[index]);
      EXPECT_EQ(adapter->gamepad()->profile().linux_phys, "hermes-kms-1");
      EXPECT_EQ(adapter->gamepad()->metadata().client_relative_index, 3);
      platf::gamepad_state_t input {};
      input.buttonFlags = platf::A | platf::DPAD_UP | platf::RIGHT_BUTTON | platf::HOME;
      input.lsX = std::numeric_limits<std::int16_t>::min();
      input.lsY = std::numeric_limits<std::int16_t>::max();
      input.rsX = 16384;
      input.rsY = -16384;
      input.lt = 255;
      input.rt = 127;
      platf::gamepad::update(&raw, 0, input);
      const auto state = adapter->gamepad()->last_submitted_state();
      EXPECT_TRUE(state.buttons.test(lvh::GamepadButton::a));
      EXPECT_TRUE(state.buttons.test(lvh::GamepadButton::dpad_up));
      EXPECT_TRUE(state.buttons.test(lvh::GamepadButton::right_shoulder));
      EXPECT_TRUE(state.buttons.test(lvh::GamepadButton::guide));
      EXPECT_FLOAT_EQ(state.left_stick.x, -1.0F);
      EXPECT_FLOAT_EQ(state.left_stick.y, 1.0F);
      EXPECT_FLOAT_EQ(state.right_stick.y, -0.5F);
      EXPECT_FLOAT_EQ(state.left_trigger, 1.0F);
      EXPECT_FLOAT_EQ(state.right_trigger, 127.0F / 255.0F);
      platf::gamepad::free(&raw, 0);
      EXPECT_EQ(raw.runtime->active_device_count(), 2U);
    }

    config::input.gamepad = "auto";
    for (const auto [client, kind] : std::array {
           std::pair {LI_CTYPE_XBOX, lvh::GamepadProfileKind::xbox_series},
           std::pair {LI_CTYPE_PS, lvh::GamepadProfileKind::dualsense},
           std::pair {LI_CTYPE_NINTENDO, lvh::GamepadProfileKind::switch_pro}
         }) {
      ASSERT_EQ(platf::gamepad::alloc(&raw, {0, 0}, {.type = static_cast<std::uint8_t>(client)}, feedback), 0);
      EXPECT_EQ(raw.gamepads[0]->adapter->gamepad()->profile().gamepad_kind, kind);
      platf::gamepad::free(&raw, 0);
    }
  }

  TEST_F(VirtualHidInputTest, AutoSelectionUsesUnknownCapabilitiesButManualSelectionWins) {
    platf::input_raw_t raw {"hermes-kms-12", fake_runtime()};
    const auto select = [&](const char *name, std::uint8_t client, std::uint16_t capabilities) {
      config::input.gamepad = name;
      EXPECT_EQ(platf::gamepad::alloc(&raw, {0, 0}, {.type = client, .capabilities = capabilities}, feedback), 0);
      const auto kind = raw.gamepads[0]->adapter->gamepad()->profile().gamepad_kind;
      platf::gamepad::free(&raw, 0);
      return kind;
    };
    config::input.motion_as_ds4 = true;
    config::input.touchpad_as_ds4 = true;
    EXPECT_EQ(select("auto", LI_CTYPE_UNKNOWN, 0), lvh::GamepadProfileKind::xbox_series);
    EXPECT_EQ(select("auto", LI_CTYPE_UNKNOWN, LI_CCAP_GYRO), lvh::GamepadProfileKind::dualsense);
    EXPECT_EQ(select("auto", LI_CTYPE_UNKNOWN, LI_CCAP_TOUCHPAD), lvh::GamepadProfileKind::dualsense);
    EXPECT_EQ(select("auto", LI_CTYPE_XBOX, LI_CCAP_GYRO), lvh::GamepadProfileKind::xbox_series);
    config::input.motion_as_ds4 = false;
    config::input.touchpad_as_ds4 = false;
    EXPECT_EQ(select("auto", LI_CTYPE_UNKNOWN, LI_CCAP_GYRO | LI_CCAP_TOUCHPAD), lvh::GamepadProfileKind::xbox_series);
    EXPECT_EQ(select("xone", LI_CTYPE_PS, LI_CCAP_GYRO), lvh::GamepadProfileKind::xbox_one);
    EXPECT_EQ(select("ds4", LI_CTYPE_XBOX, 0), lvh::GamepadProfileKind::dualshock4);
  }

  TEST_F(VirtualHidInputTest, AutomaticBackendPreservesEventDeviceRumbleAndNativeIsExplicit) {
    config::input.linux_gamepad_backend = "auto";
    for (const auto kind : {lvh::GamepadProfileKind::generic, lvh::GamepadProfileKind::xbox_360, lvh::GamepadProfileKind::xbox_one, lvh::GamepadProfileKind::xbox_series}) {
      EXPECT_TRUE(platf::gamepad::prefer_input_events(kind));
    }
    for (const auto kind : {lvh::GamepadProfileKind::dualshock4, lvh::GamepadProfileKind::dualsense, lvh::GamepadProfileKind::switch_pro}) {
      EXPECT_FALSE(platf::gamepad::prefer_input_events(kind));
    }
    config::input.linux_gamepad_backend = "native";
    EXPECT_FALSE(platf::gamepad::prefer_input_events(lvh::GamepadProfileKind::xbox_series));
    config::input.linux_gamepad_backend = "uinput";
    config::input.gamepad = "auto";
    platf::input_raw_t raw {"hermes-kms-1", fake_runtime()};
    ASSERT_EQ(platf::gamepad::alloc(&raw, {0, 0}, {.type = LI_CTYPE_PS, .capabilities = LI_CCAP_GYRO}, feedback), 0);
    EXPECT_EQ(raw.gamepads[0]->adapter->gamepad()->profile().gamepad_kind, lvh::GamepadProfileKind::xbox_series);
    EXPECT_FALSE(platf::gamepad::supports_controller_extensions());
    platf::gamepad::free(&raw, 0);
    config::input.gamepad = "ds5";
    EXPECT_EQ(platf::gamepad::alloc(&raw, {0, 0}, {}, feedback), -1);
  }

  TEST_F(VirtualHidInputTest, EveryManualProfileIsExposedWithoutLeavingProbeDevices) {
    platf::input_t input {new platf::input_raw_t("hermes-kms-12", fake_runtime())};
    auto *raw = static_cast<platf::input_raw_t *>(input.get());
    const auto options = platf::supported_gamepads(std::addressof(input));
    ASSERT_EQ(options.size(), 8U);
    const std::array names {"auto", "generic", "x360", "xone", "xseries", "ds4", "ds5", "switch"};
    for (std::size_t index = 0; index < names.size(); ++index) {
      EXPECT_EQ(options[index].name, names[index]);
      EXPECT_TRUE(options[index].is_enabled);
    }
    EXPECT_EQ(raw->runtime->active_device_count(), 2U);
  }

  TEST_F(VirtualHidInputTest, AdvertisedCapabilitiesRequireEnabledInputAndAvailableBackend) {
    lvh::BackendCapabilities backend {};
    EXPECT_EQ(platf::virtualhid::input_capabilities(backend), 0);
    backend.supports_touchscreen = true;
    EXPECT_TRUE(platf::virtualhid::input_capabilities(backend) & platf::platform_caps::pen_touch);
    config::input.native_pen_touch = false;
    EXPECT_EQ(platf::virtualhid::input_capabilities(backend), 0);
    config::input.native_pen_touch = true;
    backend.supports_touchscreen = false;
    backend.supports_pen_tablet = true;
    EXPECT_TRUE(platf::virtualhid::input_capabilities(backend) & platf::platform_caps::pen_touch);
    config::input.mouse = false;
    EXPECT_EQ(platf::virtualhid::input_capabilities(backend), 0);
    backend.supports_gamepad = true;
    for (const auto *name : {"auto", "ds4", "ds5", "switch"}) {
      config::input.gamepad = name;
      EXPECT_TRUE(platf::virtualhid::input_capabilities(backend) & platf::platform_caps::controller_touch);
    }
    for (const auto *name : {"generic", "x360", "xone", "xseries"}) {
      config::input.gamepad = name;
      EXPECT_EQ(platf::virtualhid::input_capabilities(backend), 0);
    }
    config::input.gamepad = "ds4";
    config::input.controller = false;
    EXPECT_EQ(platf::virtualhid::input_capabilities(backend), 0);
  }

  TEST_F(VirtualHidInputTest, DisabledNativeInputRejectsPacketsButAllowsCancelCleanup) {
    platf::input_t input {new platf::input_raw_t("hermes-kms-1", fake_runtime())};
    platf::client_input_raw_t client {input, fake_runtime()};
    auto &devices = *client.virtualhid_devices;
    const platf::touch_port_t port {0, 0, 1920, 1080};
    const platf::touch_input_t touch {.eventType = LI_TOUCH_EVENT_DOWN, .pointerId = 1, .x = 0.5F, .y = 0.5F};
    const platf::pen_input_t pen {.eventType = LI_TOUCH_EVENT_DOWN, .toolType = LI_TOOL_TYPE_PEN, .penButtons = LI_PEN_BUTTON_PRIMARY, .x = 0.5F, .y = 0.5F, .pressureOrDistance = 0.8F};
    config::input.native_pen_touch = false;
    platf::touch_update(&client, port, touch);
    platf::pen_update(&client, port, pen);
    EXPECT_EQ(devices.touch->submit_count(), 0U);
    EXPECT_EQ(devices.pen->submit_count(), 0U);
    config::input.native_pen_touch = true;
    platf::touch_update(&client, port, touch);
    platf::pen_update(&client, port, pen);
    EXPECT_EQ(devices.active_contacts.size(), 1U);
    EXPECT_NE(devices.pen_buttons, 0);
    config::input.mouse = false;
    platf::touch_update(&client, port, {.eventType = LI_TOUCH_EVENT_CANCEL_ALL});
    platf::pen_update(&client, port, {.eventType = LI_TOUCH_EVENT_CANCEL_ALL});
    EXPECT_TRUE(devices.active_contacts.empty());
    EXPECT_EQ(devices.pen_buttons, 0);
    EXPECT_EQ(devices.pen->last_submitted_tool().transition, lvh::PointerTransition::cancel);
  }

  TEST_F(VirtualHidInputTest, BackClickPolicyAppliesToBothPlayStationProfiles) {
    platf::input_raw_t raw {"hermes-kms-1", fake_runtime()};
    for (const auto *name : {"ds4", "ds5"}) {
      config::input.gamepad = name;
      ASSERT_EQ(platf::gamepad::alloc(&raw, {0, 0}, {}, feedback), 0);
      config::input.ds4_back_as_touchpad_click = true;
      platf::gamepad::update(&raw, 0, {.buttonFlags = platf::BACK});
      EXPECT_TRUE(raw.gamepads[0]->adapter->state().buttons.test(lvh::GamepadButton::touchpad));
      config::input.ds4_back_as_touchpad_click = false;
      platf::gamepad::update(&raw, 0, {.buttonFlags = platf::BACK});
      EXPECT_FALSE(raw.gamepads[0]->adapter->state().buttons.test(lvh::GamepadButton::touchpad));
      platf::gamepad::free(&raw, 0);
    }
  }

  TEST_F(VirtualHidInputTest, StreamResetReleasesMappedKeysFifthMouseButtonAndOnlyItsOwnDevices) {
    auto platform = std::make_shared<platf::input_t>(new platf::input_raw_t("hermes-kms-1", fake_runtime()));
    platf::input_raw_t other {"hermes-kms-12", fake_runtime()};
    auto *raw = static_cast<platf::input_raw_t *>(platform->get());
    auto client = std::make_unique<platf::client_input_raw_t>(*platform, fake_runtime());
    auto *devices = client->virtualhid_devices.get();
    auto stream = input::alloc_for_testing(mailbox, platform, std::move(client));
    const auto drain = []() {
      while (auto task = task_pool.pop()) {
        (*task)->run();
      }
    };
    auto cleanup = util::fail_guard([&]() {
      if (stream) {
        input::reset(stream);
        drain();
        stream.reset();
      }
      drain();
    });
    const auto send = [&](auto packet, std::uint32_t magic) {
      packet.header.magic = util::endian::little(magic);
      packet.header.size = util::endian::big(static_cast<std::uint32_t>(sizeof(packet) - sizeof(packet.header.size)));
      const auto *bytes = reinterpret_cast<const std::uint8_t *>(&packet);
      input::passthrough(stream, std::vector<std::uint8_t>(bytes, bytes + sizeof(packet)), crypto::PERM::_all_inputs);
      drain();
    };
    config::input.keybindings[0x41] = 0x42;
    NV_KEYBOARD_PACKET key {};
    key.keyCode = util::endian::little<short>(0x41);
    send(key, KEY_DOWN_EVENT_MAGIC);
    EXPECT_EQ(raw->keyboard->last_submitted_event().key_code, 0x42);
    NV_MOUSE_BUTTON_PACKET mouse {};
    mouse.button = BUTTON_X2;
    send(mouse, MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5);
    EXPECT_TRUE(raw->mouse->last_submitted_event().pressed);
    send(SS_CONTROLLER_ARRIVAL_PACKET {}, SS_CONTROLLER_ARRIVAL_MAGIC);
    const auto slot = std::ranges::find_if(raw->gamepads, [](const auto &gamepad) {
      return bool(gamepad);
    });
    ASSERT_NE(slot, raw->gamepads.end());
    const auto index = static_cast<int>(slot - raw->gamepads.begin());
    platf::gamepad::update(raw, index, {.buttonFlags = platf::A, .lt = 255});
    platf::gamepad::touch(raw, {{index, 0}, LI_TOUCH_EVENT_DOWN, 12, 0.3F, 0.4F, 0.8F});
    platf::virtualhid::touch_update(*devices, {}, {.eventType = LI_TOUCH_EVENT_DOWN, .pointerId = 123});
    platf::virtualhid::pen_update(*devices, {}, {.eventType = LI_TOUCH_EVENT_DOWN, .toolType = LI_TOOL_TYPE_PEN, .penButtons = LI_PEN_BUTTON_PRIMARY, .pressureOrDistance = 0.8F});
    const auto keyboard_other = other.keyboard->submit_count();
    const auto mouse_other = other.mouse->submit_count();
    input::reset(stream);
    drain();
    EXPECT_EQ(raw->keyboard->last_submitted_event().key_code, 0x42);
    EXPECT_FALSE(raw->keyboard->last_submitted_event().pressed);
    EXPECT_EQ(raw->mouse->last_submitted_event().button, lvh::MouseButton::extra);
    EXPECT_FALSE(raw->mouse->last_submitted_event().pressed);
    EXPECT_FALSE((*slot)->adapter->state().buttons.test(lvh::GamepadButton::a));
    EXPECT_FLOAT_EQ((*slot)->adapter->state().left_trigger, 0.0F);
    EXPECT_FALSE((*slot)->adapter->state().touchpad_contacts[0].active);
    EXPECT_TRUE(devices->active_contacts.empty());
    EXPECT_EQ(devices->pen_buttons, 0);
    EXPECT_EQ(devices->pen->last_submitted_tool().transition, lvh::PointerTransition::cancel);
    EXPECT_EQ(other.keyboard->submit_count(), keyboard_other);
    EXPECT_EQ(other.mouse->submit_count(), mouse_other);
    stream.reset();
    drain();
    EXPECT_EQ(raw->runtime->active_device_count(), 2U);
  }

  TEST_F(VirtualHidInputTest, DualSenseIdentityIsDistinctAcrossRuntimesAndStableWhenRequested) {
    platf::input_raw_t one {"hermes-kms-1", fake_runtime()};
    platf::input_raw_t two {"hermes-kms-12", fake_runtime()};
    ASSERT_EQ(platf::gamepad::alloc(&one, {0, 0}, {}, feedback), 0);
    ASSERT_EQ(platf::gamepad::alloc(&two, {0, 0}, {}, feedback), 0);
    const auto first = one.gamepads[0]->adapter->gamepad()->metadata().stable_id;
    EXPECT_NE(first, two.gamepads[0]->adapter->gamepad()->metadata().stable_id);
    platf::gamepad::free(&one, 0);
    ASSERT_EQ(platf::gamepad::alloc(&one, {0, 0}, {}, feedback), 0);
    EXPECT_EQ(first, one.gamepads[0]->adapter->gamepad()->metadata().stable_id);
    config::input.virtualhid_randomize_mac = true;
    platf::gamepad::free(&one, 0);
    ASSERT_EQ(platf::gamepad::alloc(&one, {0, 0}, {}, feedback), 0);
    const auto randomized = one.gamepads[0]->adapter->gamepad()->metadata().stable_id;
    EXPECT_NE(first, randomized);
    platf::gamepad::free(&one, 0);
    ASSERT_EQ(platf::gamepad::alloc(&one, {0, 0}, {}, feedback), 0);
    EXPECT_NE(randomized, one.gamepads[0]->adapter->gamepad()->metadata().stable_id);
  }

  TEST_F(VirtualHidInputTest, DualShockIdentityIsScopedAndUsesTheSharedRandomizationPolicy) {
    config::input.gamepad = "ds4";
    platf::input_raw_t one {"hermes-kms-1", fake_runtime()};
    platf::input_raw_t two {"hermes-kms-12", fake_runtime()};
    ASSERT_EQ(platf::gamepad::alloc(&one, {0, 0}, {}, feedback), 0);
    ASSERT_EQ(platf::gamepad::alloc(&two, {0, 0}, {}, feedback), 0);
    const auto first = one.gamepads[0]->adapter->gamepad()->metadata().stable_id;
    EXPECT_NE(first, two.gamepads[0]->adapter->gamepad()->metadata().stable_id);
    platf::gamepad::free(&one, 0);
    ASSERT_EQ(platf::gamepad::alloc(&one, {0, 0}, {}, feedback), 0);
    EXPECT_EQ(first, one.gamepads[0]->adapter->gamepad()->metadata().stable_id);
    config::input.virtualhid_randomize_mac = true;
    platf::gamepad::free(&one, 0);
    ASSERT_EQ(platf::gamepad::alloc(&one, {0, 0}, {}, feedback), 0);
    EXPECT_NE(first, one.gamepads[0]->adapter->gamepad()->metadata().stable_id);
  }

  TEST_F(VirtualHidInputTest, ControllerTouchMotionAndBatterySurviveButtonUpdates) {
    platf::input_raw_t raw {"hermes-kms-1", fake_runtime()};
    const platf::gamepad_id_t id {0, 4};
    ASSERT_EQ(platf::gamepad::alloc(&raw, id, {}, feedback), 0);
    auto &adapter = raw.gamepads[0]->adapter;
    platf::gamepad::touch(&raw, {id, LI_TOUCH_EVENT_DOWN, 1, 0.2F, 0.3F, 0.0F});
    platf::gamepad::touch(&raw, {id, LI_TOUCH_EVENT_DOWN, 257, 0.7F, 0.8F, 0.2F});
    auto state = adapter->state();
    EXPECT_TRUE(state.touchpad_contacts[0].active);
    EXPECT_TRUE(state.touchpad_contacts[1].active);
    EXPECT_NE(state.touchpad_contacts[0].id, state.touchpad_contacts[1].id);
    platf::gamepad::motion(&raw, {id, LI_MOTION_TYPE_ACCEL, 1.0F, 9.81F, -2.0F});
    platf::gamepad::motion(&raw, {id, LI_MOTION_TYPE_GYRO, 90.0F, -180.0F, 45.0F});
    platf::gamepad::battery(&raw, {id, LI_BATTERY_STATE_CHARGING, 73});
    platf::gamepad::update(&raw, 0, {.buttonFlags = platf::A});
    state = adapter->state();
    ASSERT_TRUE(state.acceleration);
    ASSERT_TRUE(state.gyroscope);
    ASSERT_TRUE(state.battery);
    EXPECT_FLOAT_EQ(state.acceleration->y, 9.81F);
    EXPECT_FLOAT_EQ(state.gyroscope->x, 90.0F);
    EXPECT_EQ(state.battery->percentage, 73);
    EXPECT_TRUE(state.touchpad_contacts[1].active);
    platf::gamepad::touch(&raw, {id, LI_TOUCH_EVENT_UP, 1, 0.0F, 0.0F, 1.0F});
    EXPECT_FALSE(adapter->state().touchpad_contacts[0].active);
    EXPECT_TRUE(adapter->state().touchpad_contacts[1].active);
    platf::gamepad::touch(&raw, {id, LI_TOUCH_EVENT_CANCEL_ALL});
    EXPECT_FALSE(adapter->state().touchpad_contacts[1].active);
  }

  TEST_F(VirtualHidInputTest, InvalidTouchpadCoordinatesDoNotConsumeSlotsOrPreventRelease) {
    platf::input_raw_t raw {"hermes-kms-1", fake_runtime()};
    const platf::gamepad_id_t id {0, 0};
    ASSERT_EQ(platf::gamepad::alloc(&raw, id, {}, feedback), 0);
    auto &gamepad = *raw.gamepads[0];
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto inf = std::numeric_limits<float>::infinity();
    for (const float invalid : {nan, inf, -inf}) {
      platf::gamepad::touch(&raw, {id, LI_TOUCH_EVENT_DOWN, 100, invalid, 0.4F});
      platf::gamepad::touch(&raw, {id, LI_TOUCH_EVENT_MOVE, 200, 0.3F, invalid});
    }
    EXPECT_FALSE(gamepad.touch_pointer_ids[0]);
    EXPECT_FALSE(gamepad.touch_pointer_ids[1]);
    EXPECT_EQ(gamepad.next_touch_id, 0);

    platf::gamepad::touch(&raw, {id, LI_TOUCH_EVENT_DOWN, 1, 0.2F, 0.3F});
    platf::gamepad::touch(&raw, {id, LI_TOUCH_EVENT_DOWN, 2, 0.7F, 0.8F});
    const auto previous = gamepad.adapter->state().touchpad_contacts[0];
    platf::gamepad::touch(&raw, {id, LI_TOUCH_EVENT_MOVE, 1, nan, 0.9F});
    EXPECT_FLOAT_EQ(gamepad.adapter->state().touchpad_contacts[0].x, previous.x);
    EXPECT_FLOAT_EQ(gamepad.adapter->state().touchpad_contacts[0].y, previous.y);

    platf::gamepad::touch(&raw, {id, LI_TOUCH_EVENT_UP, 1, nan, inf});
    EXPECT_FALSE(gamepad.adapter->state().touchpad_contacts[0].active);
    EXPECT_FALSE(gamepad.touch_pointer_ids[0]);
    EXPECT_TRUE(gamepad.adapter->state().touchpad_contacts[1].active);
    platf::gamepad::touch(&raw, {id, LI_TOUCH_EVENT_DOWN, 3, -1.0F, 2.0F});
    EXPECT_EQ(gamepad.touch_pointer_ids[0], 3U);
    EXPECT_FLOAT_EQ(gamepad.adapter->state().touchpad_contacts[0].x, 0.0F);
    EXPECT_FLOAT_EQ(gamepad.adapter->state().touchpad_contacts[0].y, 1.0F);
    platf::gamepad::touch(&raw, {id, LI_TOUCH_EVENT_CANCEL, 3, inf, nan});
    EXPECT_FALSE(gamepad.touch_pointer_ids[0]);
  }

  TEST_F(VirtualHidInputTest, TouchpadTrackingIdWrapDoesNotAliasAnActiveFinger) {
    platf::input_raw_t raw {"hermes-kms-1", fake_runtime()};
    const platf::gamepad_id_t id {0, 0};
    ASSERT_EQ(platf::gamepad::alloc(&raw, id, {}, feedback), 0);
    platf::gamepad::touch(&raw, {id, LI_TOUCH_EVENT_DOWN, 1, 0.2F, 0.3F});
    for (int cycle = 0; cycle < 260; ++cycle) {
      platf::gamepad::touch(&raw, {id, LI_TOUCH_EVENT_DOWN, 2, 0.7F, 0.8F});
      const auto state = raw.gamepads[0]->adapter->state();
      ASSERT_TRUE(state.touchpad_contacts[0].active);
      ASSERT_TRUE(state.touchpad_contacts[1].active);
      EXPECT_NE(state.touchpad_contacts[0].id, state.touchpad_contacts[1].id) << cycle;
      platf::gamepad::touch(&raw, {id, LI_TOUCH_EVENT_UP, 2});
    }
  }

  TEST_F(VirtualHidInputTest, FailedTouchpadSubmissionPreservesOwnershipUntilCleanupSucceeds) {
    platf::input_raw_t raw {"hermes-kms-1", fake_runtime()};
    const platf::gamepad_id_t id {0, 0};
    ASSERT_EQ(platf::gamepad::alloc(&raw, id, {}, feedback), 0);
    auto &gamepad = *raw.gamepads[0];
    platf::gamepad::touch(&raw, {id, LI_TOUCH_EVENT_DOWN, 1, 0.2F, 0.3F});
    ASSERT_EQ(gamepad.touch_pointer_ids[0], 1U);
    ASSERT_TRUE(gamepad.adapter->close().ok());
    platf::gamepad::touch(&raw, {id, LI_TOUCH_EVENT_DOWN, 2, 0.7F, 0.8F});
    EXPECT_FALSE(gamepad.touch_pointer_ids[1]);
    EXPECT_FALSE(gamepad.adapter->state().touchpad_contacts[1].active);
    EXPECT_EQ(gamepad.next_touch_id, 1);
    platf::gamepad::touch(&raw, {id, LI_TOUCH_EVENT_UP, 1});
    EXPECT_EQ(gamepad.touch_pointer_ids[0], 1U);
    EXPECT_TRUE(gamepad.adapter->state().touchpad_contacts[0].active);
    platf::gamepad::touch(&raw, {id, LI_TOUCH_EVENT_CANCEL_ALL});
    EXPECT_EQ(gamepad.touch_pointer_ids[0], 1U);
    EXPECT_TRUE(gamepad.adapter->state().touchpad_contacts[0].active);
  }

  TEST_F(VirtualHidInputTest, InvalidControllerMotionDoesNotBlockFollowingButtonUpdates) {
    platf::input_raw_t raw {"hermes-kms-1", fake_runtime()};
    const platf::gamepad_id_t id {0, 0};
    ASSERT_EQ(platf::gamepad::alloc(&raw, id, {}, feedback), 0);
    platf::gamepad::motion(&raw, {id, LI_MOTION_TYPE_ACCEL, 1.0F, 2.0F, 3.0F});
    platf::gamepad::motion(&raw, {id, LI_MOTION_TYPE_ACCEL, std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F});
    platf::gamepad::update(&raw, 0, {.buttonFlags = platf::A});
    const auto &submitted = raw.gamepads[0]->adapter->gamepad()->last_submitted_state();
    EXPECT_TRUE(submitted.buttons.test(lvh::GamepadButton::a));
    ASSERT_TRUE(submitted.acceleration);
    EXPECT_FLOAT_EQ(submitted.acceleration->x, 1.0F);
  }

  TEST_F(VirtualHidInputTest, FeedbackUsesClientIndexDeduplicatesAndDoesNotOwnController) {
    platf::input_raw_t one {"hermes-kms-1", fake_runtime()};
    platf::input_raw_t two {"hermes-kms-12", fake_runtime()};
    auto other_feedback = mailbox->queue<platf::gamepad_feedback_msg_t>("other-feedback");
    ASSERT_EQ(platf::gamepad::alloc(&one, {0, 3}, {}, feedback), 0);
    ASSERT_EQ(platf::gamepad::alloc(&two, {0, 7}, {}, other_feedback), 0);
    feedback->unsafe().clear();
    other_feedback->unsafe().clear();
    auto &adapter = one.gamepads[0]->adapter;
    lvh::GamepadOutput rumble;
    rumble.kind = lvh::GamepadOutputKind::rumble;
    rumble.low_frequency_rumble = 0xABCD;
    rumble.high_frequency_rumble = 0x1234;
    ASSERT_TRUE(adapter->dispatch_output(rumble).ok());
    ASSERT_TRUE(adapter->dispatch_output(rumble).ok());
    ASSERT_EQ(feedback->unsafe().size(), 1U);
    EXPECT_EQ(feedback->unsafe()[0].id, 3);
    EXPECT_EQ(feedback->unsafe()[0].data.rumble.lowfreq, 0xABCD);
    EXPECT_FALSE(other_feedback->peek());
    lvh::GamepadOutput rgb;
    rgb.kind = lvh::GamepadOutputKind::rgb_led;
    rgb.red = 21;
    rgb.green = 43;
    rgb.blue = 65;
    ASSERT_TRUE(adapter->dispatch_output(rgb).ok());
    ASSERT_TRUE(adapter->dispatch_output(rgb).ok());
    ASSERT_EQ(feedback->unsafe().size(), 2U);
    EXPECT_EQ(feedback->unsafe()[1].type, platf::gamepad_feedback_e::set_rgb_led);
    EXPECT_EQ(feedback->unsafe()[1].data.rgb_led.g, 43);
    lvh::GamepadOutput trigger;
    trigger.kind = lvh::GamepadOutputKind::adaptive_triggers;
    trigger.adaptive_trigger_flags = 3;
    trigger.left_trigger_effect_type = 2;
    trigger.left_trigger_effect.fill(42);
    ASSERT_TRUE(adapter->dispatch_output(trigger).ok());
    ASSERT_EQ(feedback->unsafe().size(), 3U);
    EXPECT_EQ(feedback->unsafe()[2].data.adaptive_triggers.left[9], 42);
    const std::weak_ptr<platf::joypad_state> weak = one.gamepads[0];
    platf::gamepad::free(&one, 0);
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(one.runtime->active_device_count(), 2U);
    EXPECT_TRUE(two.gamepads[0]->adapter->gamepad()->is_open());
  }

  TEST_F(VirtualHidInputTest, XboxTriggerRumbleUsesClientIndexAndDeduplicates) {
    config::input.gamepad = "xone";
    platf::input_raw_t raw {"hermes-kms-1", fake_runtime()};
    ASSERT_EQ(platf::gamepad::alloc(&raw, {0, 5}, {}, feedback), 0);
    lvh::GamepadOutput output;
    output.kind = lvh::GamepadOutputKind::trigger_rumble;
    output.left_trigger_rumble = 0x1234;
    output.right_trigger_rumble = 0x5678;
    ASSERT_TRUE(raw.gamepads[0]->adapter->dispatch_output(output).ok());
    ASSERT_TRUE(raw.gamepads[0]->adapter->dispatch_output(output).ok());
    ASSERT_EQ(feedback->unsafe().size(), 1U);
    EXPECT_EQ(feedback->unsafe()[0].id, 5);
    EXPECT_EQ(feedback->unsafe()[0].type, platf::gamepad_feedback_e::rumble_triggers);
    EXPECT_EQ(feedback->unsafe()[0].data.rumble_triggers.left_trigger, 0x1234);
    EXPECT_EQ(feedback->unsafe()[0].data.rumble_triggers.right_trigger, 0x5678);
  }

  TEST_F(VirtualHidInputTest, ControllerDisconnectCanRaceWithInputSubmission) {
    platf::input_raw_t raw {"hermes-kms-1", fake_runtime()};
    ASSERT_EQ(platf::gamepad::alloc(&raw, {0, 0}, {}, feedback), 0);
    std::atomic_bool running {true};
    std::atomic_bool started {false};
    std::thread writer {[&] {
      started = true;
      while (running.load()) {
        platf::gamepad::update(&raw, 0, {.buttonFlags = platf::A});
      }
    }};
    while (!started.load()) {
      std::this_thread::yield();
    }
    for (int cycle = 0; cycle < 100; ++cycle) {
      platf::gamepad::free(&raw, 0);
      EXPECT_EQ(platf::gamepad::alloc(&raw, {0, 0}, {}, feedback), 0);
    }
    running = false;
    writer.join();
    platf::gamepad::free(&raw, 0);
    EXPECT_EQ(raw.runtime->active_device_count(), 2U);
  }

  TEST_F(VirtualHidInputTest, TouchCancelAndPenButtonsStayWithinTheClient) {
    platf::virtualhid::client_devices_t one {"hermes-kms-1", fake_runtime()};
    platf::virtualhid::client_devices_t two {"hermes-kms-12", fake_runtime()};
    const platf::touch_port_t port {0, 0, 1920, 1080};
    platf::touch_input_t finger {.eventType = LI_TOUCH_EVENT_DOWN, .rotation = 120, .pointerId = 123, .x = 0.25F, .y = 0.75F, .pressureOrDistance = 0.7F, .contactAreaMajor = 40.0F, .contactAreaMinor = 20.0F};
    platf::virtualhid::touch_update(one, port, finger);
    const auto contact = one.touch->last_submitted_contact();
    EXPECT_EQ(contact.id, 123);
    EXPECT_FLOAT_EQ(contact.x, 0.25F);
    EXPECT_EQ(contact.orientation, 60);
    EXPECT_FLOAT_EQ(contact.contact_major_axis, 40.0F);
    EXPECT_EQ(contact.viewport.width, 1920);
    finger.pointerId = 456;
    platf::virtualhid::touch_update(one, port, finger);
    finger.eventType = LI_TOUCH_EVENT_CANCEL_ALL;
    platf::virtualhid::touch_update(one, port, finger);
    EXPECT_TRUE(one.active_contacts.empty());
    EXPECT_EQ(two.touch->submit_count(), 0U);

    platf::pen_input_t pen {.eventType = LI_TOUCH_EVENT_DOWN, .toolType = LI_TOOL_TYPE_PEN, .penButtons = 0, .tilt = 30, .rotation = 90, .x = 0.3F, .y = 0.6F, .pressureOrDistance = 0.8F};
    platf::virtualhid::pen_update(one, port, pen);
    EXPECT_FLOAT_EQ(one.pen->last_submitted_tool().pressure, 0.8F);
    const auto count = one.pen->submit_count();
    pen.eventType = LI_TOUCH_EVENT_BUTTON_ONLY;
    pen.penButtons = LI_PEN_BUTTON_PRIMARY;
    platf::virtualhid::pen_update(one, port, pen);
    EXPECT_FLOAT_EQ(one.pen->last_submitted_tool().pressure, 0.8F);
    EXPECT_EQ(one.pen->submit_count(), count + 1);
    platf::virtualhid::pen_update(one, port, pen);
    EXPECT_EQ(one.pen->submit_count(), count + 1);
    EXPECT_EQ(two.pen->submit_count(), 0U);
  }

  TEST_F(VirtualHidInputTest, LegacyMacSettingIsCanonicalizedForTheConfigurationUi) {
    const auto old = config::parse_config("ds5_inputtino_randomize_mac = disabled\n");
    EXPECT_EQ(old.at("virtualhid_randomize_mac"), "disabled");
    EXPECT_FALSE(old.contains("ds5_inputtino_randomize_mac"));
    const auto newer = config::parse_config("ds5_virtualhid_randomize_mac = disabled\n");
    EXPECT_EQ(newer.at("virtualhid_randomize_mac"), "disabled");
    const auto aliases = config::parse_config("ds5_inputtino_randomize_mac = enabled\nds5_virtualhid_randomize_mac = disabled\n");
    EXPECT_EQ(aliases.at("virtualhid_randomize_mac"), "disabled");
    const auto both = config::parse_config("ds5_inputtino_randomize_mac = disabled\nvirtualhid_randomize_mac = enabled\n");
    EXPECT_EQ(both.at("virtualhid_randomize_mac"), "enabled");
  }

  std::vector<lvh::DeviceNode> session_nodes(platf::input_raw_t &input, platf::virtualhid::client_devices_t &client) {
    std::vector<lvh::DeviceNode> nodes;
    const std::array<const lvh::VirtualDevice *, 5> devices {
      input.keyboard.get(),
      input.mouse.get(),
      client.touch.get(),
      client.pen.get(),
      input.gamepads[0]->adapter->gamepad()
    };
    std::set<std::string> seen;
    for (const auto *device : devices) {
      if (!device) {
        continue;
      }
      for (const auto &node : device->device_nodes()) {
        if (node.kind != lvh::DeviceNodeKind::sysfs && seen.insert(node.path).second) {
          nodes.push_back(node);
        }
      }
    }
    return nodes;
  }

  std::string udev_seat(const std::string &device) {
    struct stat status {};
    if (::stat(device.c_str(), &status) != 0) {
      return {};
    }
    const auto database = "/run/udev/data/c" + std::to_string(major(status.st_rdev)) + ":" + std::to_string(minor(status.st_rdev));
    std::ifstream file {database};
    std::string line;
    while (std::getline(file, line)) {
      if (line.starts_with("E:ID_SEAT=")) {
        return line.substr(10);
      }
    }
    return {};
  }

  struct EventReaders {
    std::vector<int> fds;

    ~EventReaders() {
      for (const auto fd : fds) {
        ::close(fd);
      }
    }

    void open(const std::vector<lvh::DeviceNode> &nodes) {
      for (const auto &node : nodes) {
        if (node.kind != lvh::DeviceNodeKind::input_event) {
          continue;
        }
        const auto fd = ::open(node.path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        ASSERT_GE(fd, 0) << node.path;
        fds.push_back(fd);
        ASSERT_EQ(::ioctl(fd, EVIOCGRAB, 1), 0) << node.path;
      }
    }

    std::vector<input_event> drain() {
      std::vector<input_event> captured;
      for (const auto fd : fds) {
        std::array<input_event, 256> events;
        for (;;) {
          const auto bytes = ::read(fd, events.data(), sizeof(events));
          if (bytes <= 0) {
            break;
          }
          captured.insert(captured.end(), events.begin(), events.begin() + bytes / sizeof(input_event));
        }
      }
      return captured;
    }
  };

  class VirtualHidKernelTest: public VirtualHidInputTest, public testing::WithParamInterface<const char *> {
  protected:
    void SetUp() override {
      VirtualHidInputTest::SetUp();
      if (!std::getenv("HERMES_TEST_KERNEL_INPUT")) {
        GTEST_SKIP() << "Run scripts/vm-isolated-input-test.sh in a disposable VM for kernel/udev tests";
      }
    }
  };

  class VirtualHidEventKernelTest: public VirtualHidKernelTest {};

  TEST_P(VirtualHidEventKernelTest, AutomaticXboxRumbleWorksForOnlyTheAuthorizedUidWithoutHidraw) {
  #ifndef HERMES_TEST_HID_CONSUMER
    GTEST_SKIP() << "Build tests with SDL2 >= 2.24 for the unprivileged SDL consumer";
  #else
    ASSERT_EQ(::geteuid(), 0);
    config::input.gamepad = GetParam();
    config::input.linux_gamepad_backend = "auto";
    auto one = platf::input("hermes-kms-91");
    auto two = platf::input("hermes-kms-92");
    auto other_feedback = mailbox->queue<platf::gamepad_feedback_msg_t>("foreign-feedback");
    ASSERT_EQ(platf::alloc_gamepad(one, {0, 3}, {}, feedback), 0);
    ASSERT_EQ(platf::alloc_gamepad(two, {0, 7}, {}, other_feedback), 0);
    ASSERT_EQ(std::system("udevadm settle --timeout=15"), 0);
    auto *first = static_cast<platf::input_raw_t *>(one.get());
    auto *second = static_cast<platf::input_raw_t *>(two.get());
    EXPECT_FALSE(first->gamepads[0]->adapter->support().supports_trigger_rumble);
    EXPECT_FALSE(first->gamepads[0]->adapter->support().supports_battery);
    const auto event_path = [](platf::input_raw_t *raw) {
      std::string path;
      for (int attempt = 0; attempt < 150 && path.empty(); ++attempt) {
        for (const auto &node : raw->gamepads[0]->adapter->gamepad()->device_nodes()) {
          EXPECT_NE(node.kind, lvh::DeviceNodeKind::hidraw);
          if (node.kind == lvh::DeviceNodeKind::input_event) {
            path = node.path;
          }
        }
        if (path.empty()) {
          std::this_thread::sleep_for(20ms);
        }
      }
      return path;
    };
    const auto own = event_path(first);
    const auto foreign = event_path(second);
    ASSERT_FALSE(own.empty());
    ASSERT_FALSE(foreign.empty());
    const auto run = [](std::vector<std::string> arguments) {
      std::vector<char *> raw;
      for (auto &argument : arguments) {
        raw.push_back(argument.data());
      }
      raw.push_back(nullptr);
      const auto child = ::fork();
      if (child == 0) {
        ::execv(raw.front(), raw.data());
        ::_exit(127);
      }
      if (child < 0) {
        return -1;
      }
      int status = 0;
      while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
          return -1;
        }
      }
      return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    };
    ASSERT_EQ(run({"/usr/bin/setfacl", "-m", "u:65534:rw", "--", own}), 0);
    ASSERT_EQ(run({"/usr/bin/setfacl", "-m", "u:65533:rw", "--", foreign}), 0);
    ASSERT_EQ(run({"/usr/bin/setpriv", "--reuid=65534", "--regid=65534", "--clear-groups", HERMES_TEST_HID_CONSUMER, own, foreign}), 0);
    bool observed_rumble = false;
    while (const auto message = feedback->pop(50ms)) {
      if (message->type == platf::gamepad_feedback_e::rumble && message->data.rumble.lowfreq == 0x1234 && message->data.rumble.highfreq == 0x5678) {
        EXPECT_EQ(message->id, 3);
        observed_rumble = true;
      }
    }
    EXPECT_TRUE(observed_rumble);
    EXPECT_FALSE(other_feedback->pop(20ms));
  #endif
  }

  TEST_P(VirtualHidKernelTest, PrivateSeatsRouteEventsAndTearDownIndependently) {
    config::input.gamepad = GetParam();
    if (std::string_view {GetParam()} == "generic") {
      auto initialization = input::init();
      ASSERT_TRUE(initialization);
      EXPECT_FALSE(input::probe_gamepads()) << "startup probing must receive the owning input handle";
    }
    auto one = platf::input("hermes-kms-1");
    auto two = platf::input("hermes-kms-12");
    auto client_one = platf::allocate_client_input_context(one);
    auto client_two = platf::allocate_client_input_context(two);
    auto *raw_one = static_cast<platf::input_raw_t *>(one.get());
    auto *raw_two = static_cast<platf::input_raw_t *>(two.get());
    auto *pen_one = static_cast<platf::client_input_raw_t *>(client_one.get());
    auto *pen_two = static_cast<platf::client_input_raw_t *>(client_two.get());
    ASSERT_EQ(platf::alloc_gamepad(one, {0, 3}, {}, feedback), 0);
    ASSERT_EQ(platf::alloc_gamepad(two, {0, 7}, {}, feedback), 0);
    ASSERT_EQ(std::system("udevadm settle --timeout=15"), 0);
    std::vector<lvh::DeviceNode> nodes_one;
    std::vector<lvh::DeviceNode> nodes_two;
    for (int attempt = 0; attempt < 150; ++attempt) {
      nodes_one = session_nodes(*raw_one, *pen_one->virtualhid_devices);
      nodes_two = session_nodes(*raw_two, *pen_two->virtualhid_devices);
      if (nodes_one.size() >= 6 && nodes_two.size() >= 6 && std::ranges::all_of(nodes_one, [](const auto &node) {
            return udev_seat(node.path) == "hermes-kms-1";
          }) &&
          std::ranges::all_of(nodes_two, [](const auto &node) {
            return udev_seat(node.path) == "hermes-kms-12";
          })) {
        break;
      }
      std::this_thread::sleep_for(20ms);
    }
    ASSERT_GE(nodes_one.size(), 6U);
    ASSERT_GE(nodes_two.size(), 6U);
    const auto *input_group = ::getgrnam("input");
    ASSERT_NE(input_group, nullptr);
    const auto input_gid = input_group->gr_gid;
    for (const auto &[nodes, expected] : std::array {
           std::pair {nodes_one, "hermes-kms-1"},
           std::pair {nodes_two, "hermes-kms-12"}
         }) {
      for (const auto &node : nodes) {
        EXPECT_EQ(udev_seat(node.path), expected) << node.path;
        struct stat status {};
        ASSERT_EQ(::stat(node.path.c_str(), &status), 0);
        EXPECT_EQ(status.st_mode & 0777, 0660) << node.path;
        EXPECT_EQ(status.st_gid, input_gid) << node.path;
      }
    }
    const auto gamepad_nodes = raw_one->gamepads[0]->adapter->gamepad()->device_nodes();
    ASSERT_TRUE(std::ranges::any_of(gamepad_nodes, [](const auto &node) {
      return node.kind == lvh::DeviceNodeKind::input_event;
    }))
      << "UHID controller event children must be discovered even when the driver clears input/phys";
    if (std::string_view {GetParam()} == "ds4" || std::string_view {GetParam()} == "ds5" || std::string_view {GetParam()} == "switch") {
      EXPECT_TRUE(std::ranges::any_of(gamepad_nodes, [](const auto &node) {
        return node.kind == lvh::DeviceNodeKind::hidraw;
      }));
    }
    for (const auto &first : nodes_one) {
      EXPECT_FALSE(std::ranges::any_of(nodes_two, [&](const auto &second) {
        return first.path == second.path;
      }))
        << first.path;
    }
    EventReaders readers_one;
    EventReaders readers_two;
    readers_one.open(nodes_one);
    readers_two.open(nodes_two);
    std::this_thread::sleep_for(50ms);
    readers_one.drain();
    readers_two.drain();
    platf::keyboard_update(one, 0x41, false, 0);
    platf::move_mouse(one, 17, -23);
    const platf::touch_port_t port {0, 0, 1920, 1080};
    platf::touch_update(client_one.get(), port, {.eventType = LI_TOUCH_EVENT_DOWN, .pointerId = 123, .x = 0.25F, .y = 0.75F, .pressureOrDistance = 0.7F});
    platf::pen_update(client_one.get(), port, {.eventType = LI_TOUCH_EVENT_DOWN, .toolType = LI_TOOL_TYPE_PEN, .x = 0.3F, .y = 0.6F, .pressureOrDistance = 0.8F});
    platf::gamepad_update(one, 0, {.buttonFlags = platf::A});
    std::this_thread::sleep_for(100ms);
    const auto first_events = readers_one.drain();
    const auto second_events = readers_two.drain();
    const auto has = [](const auto &events, int type, int code, int value) {
      return std::ranges::any_of(events, [&](const auto &event) {
        return event.type == type && event.code == code && event.value == value;
      });
    };
    EXPECT_TRUE(has(first_events, EV_KEY, KEY_A, 1));
    EXPECT_TRUE(has(first_events, EV_REL, REL_X, 17));
    EXPECT_TRUE(has(first_events, EV_KEY, BTN_TOUCH, 1));
    EXPECT_TRUE(std::ranges::any_of(first_events, [](const auto &event) {
      return event.type == EV_KEY && event.code >= BTN_SOUTH && event.code <= BTN_WEST && event.value == 1;
    }));
    EXPECT_FALSE(has(second_events, EV_KEY, KEY_A, 1));
    EXPECT_FALSE(has(second_events, EV_REL, REL_X, 17));
    EXPECT_FALSE(has(second_events, EV_KEY, BTN_TOUCH, 1));
    EXPECT_FALSE(std::ranges::any_of(second_events, [](const auto &event) {
      return event.type == EV_KEY && event.code >= BTN_SOUTH && event.code <= BTN_WEST && event.value == 1;
    }));

    // Exercise the actual adapter at 1000 batches/second while both private
    // seats remain alive. Every batch contains keyboard, mouse, touchscreen,
    // pen and controller input; drain continuously to detect kernel overflows.
    constexpr int batches = 1000;
    std::vector<double> elapsed;
    elapsed.reserve(batches);
    int key_events = 0;
    int mouse_events = 0;
    int dropped = 0;
    int leaked_keys = 0;
    auto deadline = std::chrono::steady_clock::now();
    for (int batch = 0; batch < batches; ++batch) {
      const auto start = std::chrono::steady_clock::now();
      const float fraction = static_cast<float>(batch % 100) / 100.0F;
      platf::keyboard_update(one, 0x41, batch % 2 == 0, 0);
      platf::move_mouse(one, 17, -23);
      platf::touch_update(client_one.get(), port, {.eventType = LI_TOUCH_EVENT_MOVE, .pointerId = 123, .x = fraction, .y = 1.0F - fraction, .pressureOrDistance = 0.7F});
      platf::pen_update(client_one.get(), port, {.eventType = LI_TOUCH_EVENT_MOVE, .toolType = LI_TOOL_TYPE_PEN, .tilt = 30, .rotation = 90, .x = fraction, .y = fraction, .pressureOrDistance = 0.1F + fraction * 0.8F});
      platf::gamepad_update(one, 0, {.buttonFlags = batch % 2 == 0 ? 0U : platf::A, .lt = static_cast<std::uint8_t>(batch % 256), .lsX = static_cast<std::int16_t>(batch * 17)});
      elapsed.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
      deadline += 1ms;
      std::this_thread::sleep_until(deadline);
      for (const auto &event : readers_one.drain()) {
        if (event.type == EV_KEY && event.code == KEY_A) {
          ++key_events;
        }
        if (event.type == EV_REL && event.code == REL_X && event.value == 17) {
          ++mouse_events;
        }
        if (event.type == EV_SYN && event.code == SYN_DROPPED) {
          ++dropped;
        }
      }
      for (const auto &event : readers_two.drain()) {
        if (event.type == EV_KEY && event.code == KEY_A) {
          ++leaked_keys;
        }
      }
    }
    EXPECT_EQ(key_events, batches);
    EXPECT_EQ(mouse_events, batches);
    EXPECT_EQ(dropped, 0);
    EXPECT_EQ(leaked_keys, 0);
    double sum = 0;
    for (const auto value : elapsed) {
      sum += value;
    }
    std::ranges::sort(elapsed);
    std::cout << "profile=" << GetParam() << " batches=" << batches << " rate=1000Hz"
              << " submit_mean_us=" << sum / batches << " submit_p99_us=" << elapsed[batches * 99 / 100]
              << " dropped=" << dropped << " leaked_keys=" << leaked_keys << '\n';

    client_one.reset();
    one.reset();
    for (int attempt = 0; attempt < 150; ++attempt) {
      if (std::ranges::none_of(nodes_one, [](const auto &node) {
            return std::filesystem::exists(node.path);
          })) {
        break;
      }
      std::this_thread::sleep_for(20ms);
    }
    for (const auto &node : nodes_one) {
      EXPECT_FALSE(std::filesystem::exists(node.path)) << node.path;
    }
    for (const auto &node : nodes_two) {
      EXPECT_TRUE(std::filesystem::exists(node.path)) << node.path;
    }
    platf::keyboard_update(two, 0x42, false, 0);
    std::this_thread::sleep_for(50ms);
    EXPECT_TRUE(has(readers_two.drain(), EV_KEY, KEY_B, 1));
  }

  INSTANTIATE_TEST_SUITE_P(ControllerProfiles, VirtualHidKernelTest, testing::Values("generic", "x360", "xone", "xseries", "ds4", "ds5", "switch"));
  INSTANTIATE_TEST_SUITE_P(XboxEventProfiles, VirtualHidEventKernelTest, testing::Values("xone", "xseries"));
}  // namespace
#endif
