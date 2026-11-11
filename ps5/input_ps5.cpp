// reEye: the DualSense of the user who started the app, through libScePad, as the 360's controller 1. The pad data
// prefix and button bits follow reKing's ps5/input_ps5.cpp (from reUndr's input_ps5.c, MIT, bystritskiy).
// ReXGlue's default input system takes it in place of SDL's gamepads on the PS5 (patches/rexglue-ps5.patch).
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>

#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/logging.h>

extern "C" {
int scePadInit(void);
int scePadOpen(int user, int type, int index, const void* param);
int scePadGetHandle(int user, int type, int index);
int scePadReadState(int handle, void* data);
int scePadSetVibration(int handle, const void* motors);
int scePadSetVibrationMode(int handle, int mode);
int sceUserServiceInitialize(const void* param);
int sceUserServiceGetInitialUser(int* user);
}

namespace rex::input::ps5 {

namespace {
// the public prefix of libScePad's state, padded to well past its real size
struct PadData {
  uint32_t buttons;
  uint8_t lx, ly, rx, ry, l2, r2;
  uint8_t reserved[66];
  uint8_t connected;
  uint8_t tail[179];
};
static_assert(offsetof(PadData, connected) == 76, "libScePad connected ABI");

constexpr uint32_t kL3 = 0x2, kR3 = 0x4, kOptions = 0x8, kUp = 0x10, kRight = 0x20, kDown = 0x40, kLeft = 0x80,
                   kL1 = 0x400, kR1 = 0x800, kTriangle = 0x1000, kCircle = 0x2000, kCross = 0x4000,
                   kSquare = 0x8000, kTouchpad = 0x100000;

// DualSense button → Xbox 360 button (Cross is A, Circle B, Square X, Triangle Y; Options is Start, the touchpad
// Back)
constexpr struct {
  uint32_t ps5;
  uint16_t xbox;
} kButtons[] = {
    {kUp, X_INPUT_GAMEPAD_DPAD_UP},       {kDown, X_INPUT_GAMEPAD_DPAD_DOWN},
    {kLeft, X_INPUT_GAMEPAD_DPAD_LEFT},   {kRight, X_INPUT_GAMEPAD_DPAD_RIGHT},
    {kOptions, X_INPUT_GAMEPAD_START},    {kTouchpad, X_INPUT_GAMEPAD_BACK},
    {kL3, X_INPUT_GAMEPAD_LEFT_THUMB},    {kR3, X_INPUT_GAMEPAD_RIGHT_THUMB},
    {kL1, X_INPUT_GAMEPAD_LEFT_SHOULDER}, {kR1, X_INPUT_GAMEPAD_RIGHT_SHOULDER},
    {kCross, X_INPUT_GAMEPAD_A},          {kCircle, X_INPUT_GAMEPAD_B},
    {kSquare, X_INPUT_GAMEPAD_X},         {kTriangle, X_INPUT_GAMEPAD_Y},
};

// a libScePad stick (0-255, 128 centred, y down) as XInput's: -32768..32767, y up when `flip`
int16_t Axis(uint8_t v, bool flip) {
  int d = int(v) - 128;
  int a = d >= 0 ? d * 32767 / 127 : d * 256;
  if (flip) a = a == -32768 ? 32767 : -a;
  return int16_t(a);
}
}  // namespace

class Ps5InputDriver final : public InputDriver {
 public:
  Ps5InputDriver() : InputDriver(nullptr, 0) {}

  X_STATUS Setup() override {
    sceUserServiceInitialize(nullptr);  // already done is fine
    int rc = scePadInit();
    if (rc < 0) REXLOG_WARN("input: scePadInit {:08X}", uint32_t(rc));
    int user = -1;
    sceUserServiceGetInitialUser(&user);
    handle_ = scePadOpen(user, 0, 0, nullptr);
    if (handle_ < 0) handle_ = scePadGetHandle(user, 0, 0);  // already open (the system's own handle)
    if (handle_ < 0) {
      REXLOG_ERROR("input: no controller for user {:X} ({:08X})", uint32_t(user), uint32_t(handle_));
      return X_STATUS_UNSUCCESSFUL;
    }
    scePadSetVibrationMode(handle_, 2);
    REXLOG_INFO("input: DualSense of user {:X} as controller 1", uint32_t(user));
    return X_STATUS_SUCCESS;
  }

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t, X_INPUT_CAPABILITIES* out_caps) override {
    if (user_index != 0 || handle_ < 0) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out_caps) {
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
      out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
      out_caps->gamepad.buttons = 0xFFFF;
      out_caps->gamepad.left_trigger = 0xFF;
      out_caps->gamepad.right_trigger = 0xFF;
      out_caps->gamepad.thumb_lx = int16_t(0x7FFF);
      out_caps->gamepad.thumb_ly = int16_t(0x7FFF);
      out_caps->gamepad.thumb_rx = int16_t(0x7FFF);
      out_caps->gamepad.thumb_ry = int16_t(0x7FFF);
      out_caps->vibration.left_motor_speed = 0xFFFF;
      out_caps->vibration.right_motor_speed = 0xFFFF;
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override {
    if (user_index != 0 || handle_ < 0) return X_ERROR_DEVICE_NOT_CONNECTED;
    PadData d;
    std::memset(&d, 0, sizeof d);
    {
      std::lock_guard<std::mutex> lock(lock_);
      if (scePadReadState(handle_, &d) < 0 || !d.connected) return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    uint16_t buttons = 0;
    for (const auto& b : kButtons) {
      if (d.buttons & b.ps5) buttons |= b.xbox;
    }
    if (out_state) {
      const bool changed = std::memcmp(&d, &last_, offsetof(PadData, reserved)) != 0;
      if (changed) {
        std::memcpy(&last_, &d, offsetof(PadData, reserved));
        packet_++;
      }
      out_state->packet_number = packet_;
      out_state->gamepad.buttons = buttons;
      out_state->gamepad.left_trigger = d.l2;
      out_state->gamepad.right_trigger = d.r2;
      out_state->gamepad.thumb_lx = Axis(d.lx, false);
      out_state->gamepad.thumb_ly = Axis(d.ly, true);
      out_state->gamepad.thumb_rx = Axis(d.rx, false);
      out_state->gamepad.thumb_ry = Axis(d.ry, true);
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override {
    if (user_index != 0 || handle_ < 0) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (vibration) {
      const uint8_t motors[2] = {uint8_t(uint16_t(vibration->left_motor_speed) >> 8),
                                 uint8_t(uint16_t(vibration->right_motor_speed) >> 8)};
      std::lock_guard<std::mutex> lock(lock_);
      scePadSetVibration(handle_, motors);
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetKeystroke(uint32_t user_index, uint32_t, X_INPUT_KEYSTROKE*) override {
    return user_index == 0 && handle_ >= 0 ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  int handle_ = -1;
  std::mutex lock_;
  PadData last_{};
  uint32_t packet_ = 0;
};

// called by ReXGlue's CreateDefaultInputSystem on the PS5
std::unique_ptr<InputDriver> CreatePs5InputDriver() {
  return std::make_unique<Ps5InputDriver>();
}

}  // namespace rex::input::ps5
