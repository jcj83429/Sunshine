/**
 * @file src/platform/windows/input.cpp
 * @brief todo
 */
#include <windows.h>

#include <cmath>

#include <ViGEm/Client.h>

#include "misc.h"
#include "src/config.h"
#include "src/main.h"
#include "src/platform/common.h"

namespace platf {
  using namespace std::literals;

  thread_local HDESK _lastKnownInputDesktop = nullptr;

  constexpr touch_port_t target_touch_port {
    0, 0,
    65535, 65535
  };

  using client_t = util::safe_ptr<_VIGEM_CLIENT_T, vigem_free>;
  using target_t = util::safe_ptr<_VIGEM_TARGET_T, vigem_target_free>;

  void CALLBACK
  x360_notify(
    client_t::pointer client,
    target_t::pointer target,
    std::uint8_t largeMotor, std::uint8_t smallMotor,
    std::uint8_t /* led_number */,
    void *userdata);

  void CALLBACK
  ds4_notify(
    client_t::pointer client,
    target_t::pointer target,
    std::uint8_t largeMotor, std::uint8_t smallMotor,
    DS4_LIGHTBAR_COLOR /* led_color */,
    void *userdata);

  struct gamepad_context_t {
    target_t gp;
    rumble_queue_t rumble_queue;

    union {
      XUSB_REPORT x360;
      DS4_REPORT_EX ds4;
    } report;
  };

  constexpr float EARTH_G = 9.80665f;

#define MPS2_TO_DS4_ACCEL(x) (int32_t)(((x) / EARTH_G) * 8192)
#define DPS_TO_DS4_GYRO(x) (int32_t)((x) * (1024 / 64))

#define APPLY_CALIBRATION(val, bias, scale) (int32_t)(((float) (val) + (bias)) / (scale))

  constexpr DS4_TOUCH ds4_touch_unused = {
    .bPacketCounter = 0,
    .bIsUpTrackingNum1 = 0x80,
    .bTouchData1 = { 0x00, 0x00, 0x00 },
    .bIsUpTrackingNum2 = 0x80,
    .bTouchData2 = { 0x00, 0x00, 0x00 },
  };

  // See https://github.com/ViGEm/ViGEmBus/blob/22835473d17fbf0c4d4bb2f2d42fd692b6e44df4/sys/Ds4Pdo.cpp#L153-L164
  constexpr DS4_REPORT_EX ds4_report_init_ex = {
    { { .bThumbLX = 0x80,
      .bThumbLY = 0x80,
      .bThumbRX = 0x80,
      .bThumbRY = 0x80,
      .wButtons = DS4_BUTTON_DPAD_NONE,
      .bSpecial = 0,
      .bTriggerL = 0,
      .bTriggerR = 0,
      .wTimestamp = 0,
      .bBatteryLvl = 99,
      .wGyroX = 0,
      .wGyroY = 0,
      .wGyroZ = 0,
      .wAccelX = 0,
      .wAccelY = 0,
      .wAccelZ = 0,
      ._bUnknown1 = { 0x00, 0x00, 0x00, 0x00, 0x00 },
      .bBatteryLvlSpecial = 0x10,  // Wired
      ._bUnknown2 = { 0x00, 0x00 },
      .bTouchPacketsN = 0,
      .sCurrentTouch = ds4_touch_unused,
      .sPreviousTouch = { ds4_touch_unused, ds4_touch_unused } } }
  };

  /**
   * @brief Updates the DS4 input report with the provided motion data.
   * @details Acceleration is in m/s^2 and gyro is in deg/s.
   * @param gamepad The gamepad to update.
   * @param motion_type The type of motion data.
   * @param x X component of motion.
   * @param y Y component of motion.
   * @param z Z component of motion.
   */
  static void
  ds4_update_motion(gamepad_context_t &gamepad, uint8_t motion_type, float x, float y, float z) {
    auto &report = gamepad.report.ds4.Report;

    // Use int32 to process this data, so we can clamp if needed.
    int32_t intX, intY, intZ;

    switch (motion_type) {
      case LI_MOTION_TYPE_ACCEL:
        // Convert to the DS4's accelerometer scale
        intX = MPS2_TO_DS4_ACCEL(x);
        intY = MPS2_TO_DS4_ACCEL(y);
        intZ = MPS2_TO_DS4_ACCEL(z);

        // Apply the inverse of ViGEmBus's calibration data
        intX = APPLY_CALIBRATION(intX, -297, 1.010796f);
        intY = APPLY_CALIBRATION(intY, -42, 1.014614f);
        intZ = APPLY_CALIBRATION(intZ, -512, 1.024768f);
        break;
      case LI_MOTION_TYPE_GYRO:
        // Convert to the DS4's gyro scale
        intX = DPS_TO_DS4_GYRO(x);
        intY = DPS_TO_DS4_GYRO(y);
        intZ = DPS_TO_DS4_GYRO(z);

        // Apply the inverse of ViGEmBus's calibration data
        intX = APPLY_CALIBRATION(intX, 1, 0.977596f);
        intY = APPLY_CALIBRATION(intY, 0, 0.972370f);
        intZ = APPLY_CALIBRATION(intZ, 0, 0.971550f);
        break;
      default:
        return;
    }

    // Clamp the values to the range of the data type
    intX = std::clamp(intX, INT16_MIN, INT16_MAX);
    intY = std::clamp(intY, INT16_MIN, INT16_MAX);
    intZ = std::clamp(intZ, INT16_MIN, INT16_MAX);

    // Populate the report
    switch (motion_type) {
      case LI_MOTION_TYPE_ACCEL:
        report.wAccelX = (int16_t) intX;
        report.wAccelY = (int16_t) intY;
        report.wAccelZ = (int16_t) intZ;
        break;
      case LI_MOTION_TYPE_GYRO:
        report.wGyroX = (int16_t) intX;
        report.wGyroY = (int16_t) intY;
        report.wGyroZ = (int16_t) intZ;
        break;
      default:
        return;
    }
  }

  class vigem_t {
  public:
    int
    init() {
      VIGEM_ERROR status;

      client.reset(vigem_alloc());

      status = vigem_connect(client.get());
      if (!VIGEM_SUCCESS(status)) {
        BOOST_LOG(warning) << "Couldn't setup connection to ViGEm for gamepad support ["sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      gamepads.resize(MAX_GAMEPADS);

      return 0;
    }

    /**
     * @brief Attaches a new gamepad.
     * @param nr The gamepad index.
     * @param rumble_queue The queue to publish rumble packets.
     * @param gp_type The type of gamepad.
     * @return 0 on success.
     */
    int
    alloc_gamepad_internal(int nr, rumble_queue_t &rumble_queue, VIGEM_TARGET_TYPE gp_type) {
      auto &gamepad = gamepads[nr];
      assert(!gamepad.gp);

      if (gp_type == Xbox360Wired) {
        gamepad.gp.reset(vigem_target_x360_alloc());
        XUSB_REPORT_INIT(&gamepad.report.x360);
      }
      else {
        gamepad.gp.reset(vigem_target_ds4_alloc());

        // There is no equivalent DS4_REPORT_EX_INIT()
        gamepad.report.ds4 = ds4_report_init_ex;

        // Set initial accelerometer and gyro state
        ds4_update_motion(gamepad, LI_MOTION_TYPE_ACCEL, 0.0f, EARTH_G, 0.0f);
        ds4_update_motion(gamepad, LI_MOTION_TYPE_GYRO, 0.0f, 0.0f, 0.0f);
      }

      auto status = vigem_target_add(client.get(), gamepad.gp.get());
      if (!VIGEM_SUCCESS(status)) {
        BOOST_LOG(error) << "Couldn't add Gamepad to ViGEm connection ["sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      gamepad.rumble_queue = std::move(rumble_queue);

      if (gp_type == Xbox360Wired) {
        status = vigem_target_x360_register_notification(client.get(), gamepad.gp.get(), x360_notify, this);
      }
      else {
        status = vigem_target_ds4_register_notification(client.get(), gamepad.gp.get(), ds4_notify, this);
      }

      if (!VIGEM_SUCCESS(status)) {
        BOOST_LOG(warning) << "Couldn't register notifications for rumble support ["sv << util::hex(status).to_string_view() << ']';
      }

      return 0;
    }

    /**
     * @brief Detaches the specified gamepad
     * @param nr The gamepad.
     */
    void
    free_target(int nr) {
      auto &gamepad = gamepads[nr];

      if (gamepad.gp && vigem_target_is_attached(gamepad.gp.get())) {
        auto status = vigem_target_remove(client.get(), gamepad.gp.get());
        if (!VIGEM_SUCCESS(status)) {
          BOOST_LOG(warning) << "Couldn't detach gamepad from ViGEm ["sv << util::hex(status).to_string_view() << ']';
        }
      }

      gamepad.gp.reset();
    }

    /**
     * @brief Pass rumble data back to the host.
     * @param target The gamepad.
     * @param smallMotor The small motor.
     * @param largeMotor The large motor.
     */
    void
    rumble(target_t::pointer target, std::uint8_t smallMotor, std::uint8_t largeMotor) {
      for (int x = 0; x < gamepads.size(); ++x) {
        auto &gamepad = gamepads[x];

        if (gamepad.gp.get() == target) {
          gamepad.rumble_queue->raise(x, ((std::uint16_t) smallMotor) << 8, ((std::uint16_t) largeMotor) << 8);

          return;
        }
      }
    }

    /**
     * @brief vigem_t destructor.
     */
    ~vigem_t() {
      if (client) {
        for (auto &gamepad : gamepads) {
          if (gamepad.gp && vigem_target_is_attached(gamepad.gp.get())) {
            auto status = vigem_target_remove(client.get(), gamepad.gp.get());
            if (!VIGEM_SUCCESS(status)) {
              BOOST_LOG(warning) << "Couldn't detach gamepad from ViGEm ["sv << util::hex(status).to_string_view() << ']';
            }
          }
        }

        vigem_disconnect(client.get());
      }
    }

    std::vector<gamepad_context_t> gamepads;

    client_t client;
  };

  void CALLBACK
  x360_notify(
    client_t::pointer client,
    target_t::pointer target,
    std::uint8_t largeMotor, std::uint8_t smallMotor,
    std::uint8_t /* led_number */,
    void *userdata) {
    BOOST_LOG(debug)
      << "largeMotor: "sv << (int) largeMotor << std::endl
      << "smallMotor: "sv << (int) smallMotor;

    task_pool.push(&vigem_t::rumble, (vigem_t *) userdata, target, smallMotor, largeMotor);
  }

  void CALLBACK
  ds4_notify(
    client_t::pointer client,
    target_t::pointer target,
    std::uint8_t largeMotor, std::uint8_t smallMotor,
    DS4_LIGHTBAR_COLOR /* led_color */,
    void *userdata) {
    BOOST_LOG(debug)
      << "largeMotor: "sv << (int) largeMotor << std::endl
      << "smallMotor: "sv << (int) smallMotor;

    task_pool.push(&vigem_t::rumble, (vigem_t *) userdata, target, smallMotor, largeMotor);
  }

  struct input_raw_t {
    ~input_raw_t() {
      delete vigem;
    }

    vigem_t *vigem;
    HKL keyboard_layout;
    HKL active_layout;
  };

  input_t
  input() {
    input_t result { new input_raw_t {} };
    auto &raw = *(input_raw_t *) result.get();

    raw.vigem = new vigem_t {};
    if (raw.vigem->init()) {
      delete raw.vigem;
      raw.vigem = nullptr;
    }

    // Moonlight currently sends keys normalized to the US English layout.
    // We need to use that layout when converting to scancodes.
    raw.keyboard_layout = LoadKeyboardLayoutA("00000409", 0);
    if (!raw.keyboard_layout || LOWORD(raw.keyboard_layout) != 0x409) {
      BOOST_LOG(warning) << "Unable to load US English keyboard layout for scancode translation. Keyboard input may not work in games."sv;
      raw.keyboard_layout = NULL;
    }

    // Activate layout for current process only
    raw.active_layout = ActivateKeyboardLayout(raw.keyboard_layout, KLF_SETFORPROCESS);
    if (!raw.active_layout) {
      BOOST_LOG(warning) << "Unable to activate US English keyboard layout for scancode translation. Keyboard input may not work in games."sv;
      raw.keyboard_layout = NULL;
    }

    return result;
  }

  void
  send_input(INPUT &i) {
  retry:
    auto send = SendInput(1, &i, sizeof(INPUT));
    if (send != 1) {
      auto hDesk = syncThreadDesktop();
      if (_lastKnownInputDesktop != hDesk) {
        _lastKnownInputDesktop = hDesk;
        goto retry;
      }
      BOOST_LOG(error) << "Couldn't send input"sv;
    }
  }

  void
  abs_mouse(input_t &input, const touch_port_t &touch_port, float x, float y) {
    INPUT i {};

    i.type = INPUT_MOUSE;
    auto &mi = i.mi;

    mi.dwFlags =
      MOUSEEVENTF_MOVE |
      MOUSEEVENTF_ABSOLUTE |

      // MOUSEEVENTF_VIRTUALDESK maps to the entirety of the desktop rather than the primary desktop
      MOUSEEVENTF_VIRTUALDESK;

    auto scaled_x = std::lround((x + touch_port.offset_x) * ((float) target_touch_port.width / (float) touch_port.width));
    auto scaled_y = std::lround((y + touch_port.offset_y) * ((float) target_touch_port.height / (float) touch_port.height));

    mi.dx = scaled_x;
    mi.dy = scaled_y;

    send_input(i);
  }

  void
  move_mouse(input_t &input, int deltaX, int deltaY) {
    INPUT i {};

    i.type = INPUT_MOUSE;
    auto &mi = i.mi;

    mi.dwFlags = MOUSEEVENTF_MOVE;
    mi.dx = deltaX;
    mi.dy = deltaY;

    send_input(i);
  }

  void
  button_mouse(input_t &input, int button, bool release) {
    constexpr auto KEY_STATE_DOWN = (SHORT) 0x8000;

    INPUT i {};

    i.type = INPUT_MOUSE;
    auto &mi = i.mi;

    int mouse_button;
    if (button == 1) {
      mi.dwFlags = release ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_LEFTDOWN;
      mouse_button = VK_LBUTTON;
    }
    else if (button == 2) {
      mi.dwFlags = release ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_MIDDLEDOWN;
      mouse_button = VK_MBUTTON;
    }
    else if (button == 3) {
      mi.dwFlags = release ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_RIGHTDOWN;
      mouse_button = VK_RBUTTON;
    }
    else if (button == 4) {
      mi.dwFlags = release ? MOUSEEVENTF_XUP : MOUSEEVENTF_XDOWN;
      mi.mouseData = XBUTTON1;
      mouse_button = VK_XBUTTON1;
    }
    else {
      mi.dwFlags = release ? MOUSEEVENTF_XUP : MOUSEEVENTF_XDOWN;
      mi.mouseData = XBUTTON2;
      mouse_button = VK_XBUTTON2;
    }

    auto key_state = GetAsyncKeyState(mouse_button);
    bool key_state_down = (key_state & KEY_STATE_DOWN) != 0;
    if (key_state_down != release) {
      BOOST_LOG(warning) << "Button state of mouse_button ["sv << button << "] does not match the desired state"sv;

      return;
    }

    send_input(i);
  }

  void
  scroll(input_t &input, int distance) {
    INPUT i {};

    i.type = INPUT_MOUSE;
    auto &mi = i.mi;

    mi.dwFlags = MOUSEEVENTF_WHEEL;
    mi.mouseData = distance;

    send_input(i);
  }

  void
  hscroll(input_t &input, int distance) {
    INPUT i {};

    i.type = INPUT_MOUSE;
    auto &mi = i.mi;

    mi.dwFlags = MOUSEEVENTF_HWHEEL;
    mi.mouseData = distance;

    send_input(i);
  }

  void
  keyboard(input_t &input, uint16_t modcode, bool release, uint8_t flags) {
    auto raw = (input_raw_t *) input.get();

    INPUT i {};
    i.type = INPUT_KEYBOARD;
    auto &ki = i.ki;

    // If the client did not normalize this VK code to a US English layout, we can't accurately convert it to a scancode.
    bool send_scancode = !(flags & SS_KBE_FLAG_NON_NORMALIZED) || config::input.always_send_scancodes;

    if (send_scancode && modcode != VK_LWIN && modcode != VK_RWIN && modcode != VK_PAUSE && raw->keyboard_layout != NULL) {
      // For some reason, MapVirtualKey(VK_LWIN, MAPVK_VK_TO_VSC) doesn't seem to work :/
      ki.wScan = MapVirtualKeyEx(modcode, MAPVK_VK_TO_VSC, raw->keyboard_layout);
    }

    // If we can map this to a scancode, send it as a scancode for maximum game compatibility.
    if (ki.wScan) {
      ki.dwFlags = KEYEVENTF_SCANCODE;
    }
    else {
      // If there is no scancode mapping or it's non-normalized, send it as a regular VK event.
      ki.wVk = modcode;
    }

    // https://docs.microsoft.com/en-us/windows/win32/inputdev/about-keyboard-input#keystroke-message-flags
    switch (modcode) {
      case VK_RMENU:
      case VK_RCONTROL:
      case VK_INSERT:
      case VK_DELETE:
      case VK_HOME:
      case VK_END:
      case VK_PRIOR:
      case VK_NEXT:
      case VK_UP:
      case VK_DOWN:
      case VK_LEFT:
      case VK_RIGHT:
      case VK_DIVIDE:
      case VK_APPS:
        ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        break;
      default:
        break;
    }

    if (release) {
      ki.dwFlags |= KEYEVENTF_KEYUP;
    }

    send_input(i);
  }

  void
  unicode(input_t &input, char *utf8, int size) {
    // We can do no worse than one UTF-16 character per byte of UTF-8
    WCHAR wide[size];

    int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, size, wide, size);
    if (chars <= 0) {
      return;
    }

    // Send all key down events
    for (int i = 0; i < chars; i++) {
      INPUT input {};
      input.type = INPUT_KEYBOARD;
      input.ki.wScan = wide[i];
      input.ki.dwFlags = KEYEVENTF_UNICODE;
      send_input(input);
    }

    // Send all key up events
    for (int i = 0; i < chars; i++) {
      INPUT input {};
      input.type = INPUT_KEYBOARD;
      input.ki.wScan = wide[i];
      input.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
      send_input(input);
    }
  }

  /**
   * @brief Creates a new virtual gamepad.
   * @param input The input context.
   * @param nr The assigned controller number.
   * @param metadata Controller metadata from client (empty if none provided).
   * @param rumble_queue The queue for posting rumble messages to the client.
   * @return 0 on success.
   */
  int
  alloc_gamepad(input_t &input, int nr, const gamepad_arrival_t &metadata, rumble_queue_t rumble_queue) {
    auto raw = (input_raw_t *) input.get();

    if (!raw->vigem) {
      return 0;
    }

    VIGEM_TARGET_TYPE selectedGamepadType;

    if (config::input.gamepad == "x360"sv) {
      BOOST_LOG(info) << "Gamepad " << nr << " will be Xbox 360 controller (manual selection)"sv;
      selectedGamepadType = Xbox360Wired;
    }
    else if (config::input.gamepad == "ps4"sv || config::input.gamepad == "ds4"sv) {
      BOOST_LOG(info) << "Gamepad " << nr << " will be DualShock 4 controller (manual selection)"sv;
      selectedGamepadType = DualShock4Wired;
    }
    else if (metadata.type == LI_CTYPE_PS) {
      BOOST_LOG(info) << "Gamepad " << nr << " will be DualShock 4 controller (auto-selected by client-reported type)"sv;
      selectedGamepadType = DualShock4Wired;
    }
    else if (metadata.type == LI_CTYPE_XBOX) {
      BOOST_LOG(info) << "Gamepad " << nr << " will be Xbox 360 controller (auto-selected by client-reported type)"sv;
      selectedGamepadType = Xbox360Wired;
    }
    else if (metadata.capabilities & (LI_CCAP_ACCEL | LI_CCAP_GYRO)) {
      BOOST_LOG(info) << "Gamepad " << nr << " will be DualShock 4 controller (auto-selected by motion sensor presence)"sv;
      selectedGamepadType = DualShock4Wired;
    }
    else if (metadata.capabilities & LI_CCAP_TOUCHPAD) {
      BOOST_LOG(info) << "Gamepad " << nr << " will be DualShock 4 controller (auto-selected by touchpad presence)"sv;
      selectedGamepadType = DualShock4Wired;
    }
    else {
      BOOST_LOG(info) << "Gamepad " << nr << " will be Xbox 360 controller (default)"sv;
      selectedGamepadType = Xbox360Wired;
    }

    return raw->vigem->alloc_gamepad_internal(nr, rumble_queue, selectedGamepadType);
  }

  void
  free_gamepad(input_t &input, int nr) {
    auto raw = (input_raw_t *) input.get();

    if (!raw->vigem) {
      return;
    }

    raw->vigem->free_target(nr);
  }

  /**
   * @brief Converts the standard button flags into X360 format.
   * @param gamepad_state The gamepad button/axis state sent from the client.
   * @return XUSB_BUTTON flags.
   */
  static XUSB_BUTTON
  x360_buttons(const gamepad_state_t &gamepad_state) {
    int buttons {};

    auto flags = gamepad_state.buttonFlags;
    // clang-format off
    if(flags & DPAD_UP)      buttons |= XUSB_GAMEPAD_DPAD_UP;
    if(flags & DPAD_DOWN)    buttons |= XUSB_GAMEPAD_DPAD_DOWN;
    if(flags & DPAD_LEFT)    buttons |= XUSB_GAMEPAD_DPAD_LEFT;
    if(flags & DPAD_RIGHT)   buttons |= XUSB_GAMEPAD_DPAD_RIGHT;
    if(flags & START)        buttons |= XUSB_GAMEPAD_START;
    if(flags & BACK)         buttons |= XUSB_GAMEPAD_BACK;
    if(flags & LEFT_STICK)   buttons |= XUSB_GAMEPAD_LEFT_THUMB;
    if(flags & RIGHT_STICK)  buttons |= XUSB_GAMEPAD_RIGHT_THUMB;
    if(flags & LEFT_BUTTON)  buttons |= XUSB_GAMEPAD_LEFT_SHOULDER;
    if(flags & RIGHT_BUTTON) buttons |= XUSB_GAMEPAD_RIGHT_SHOULDER;
    if(flags & HOME)         buttons |= XUSB_GAMEPAD_GUIDE;
    if(flags & A)            buttons |= XUSB_GAMEPAD_A;
    if(flags & B)            buttons |= XUSB_GAMEPAD_B;
    if(flags & X)            buttons |= XUSB_GAMEPAD_X;
    if(flags & Y)            buttons |= XUSB_GAMEPAD_Y;
    // clang-format on

    return (XUSB_BUTTON) buttons;
  }

  /**
   * @brief Updates the X360 input report with the provided gamepad state.
   * @param gamepad The gamepad to update.
   * @param gamepad_state The gamepad button/axis state sent from the client.
   */
  static void
  x360_update_state(gamepad_context_t &gamepad, const gamepad_state_t &gamepad_state) {
    auto &report = gamepad.report.x360;

    report.wButtons = x360_buttons(gamepad_state);
    report.bLeftTrigger = gamepad_state.lt;
    report.bRightTrigger = gamepad_state.rt;
    report.sThumbLX = gamepad_state.lsX;
    report.sThumbLY = gamepad_state.lsY;
    report.sThumbRX = gamepad_state.rsX;
    report.sThumbRY = gamepad_state.rsY;
  }

  static DS4_DPAD_DIRECTIONS
  ds4_dpad(const gamepad_state_t &gamepad_state) {
    auto flags = gamepad_state.buttonFlags;
    if (flags & DPAD_UP) {
      if (flags & DPAD_RIGHT) {
        return DS4_BUTTON_DPAD_NORTHEAST;
      }
      else if (flags & DPAD_LEFT) {
        return DS4_BUTTON_DPAD_NORTHWEST;
      }
      else {
        return DS4_BUTTON_DPAD_NORTH;
      }
    }

    else if (flags & DPAD_DOWN) {
      if (flags & DPAD_RIGHT) {
        return DS4_BUTTON_DPAD_SOUTHEAST;
      }
      else if (flags & DPAD_LEFT) {
        return DS4_BUTTON_DPAD_SOUTHWEST;
      }
      else {
        return DS4_BUTTON_DPAD_SOUTH;
      }
    }

    else if (flags & DPAD_RIGHT) {
      return DS4_BUTTON_DPAD_EAST;
    }

    else if (flags & DPAD_LEFT) {
      return DS4_BUTTON_DPAD_WEST;
    }

    return DS4_BUTTON_DPAD_NONE;
  }

  /**
   * @brief Converts the standard button flags into DS4 format.
   * @param gamepad_state The gamepad button/axis state sent from the client.
   * @return DS4_BUTTONS flags.
   */
  static DS4_BUTTONS
  ds4_buttons(const gamepad_state_t &gamepad_state) {
    int buttons {};

    auto flags = gamepad_state.buttonFlags;
    // clang-format off
    if(flags & LEFT_STICK)   buttons |= DS4_BUTTON_THUMB_LEFT;
    if(flags & RIGHT_STICK)  buttons |= DS4_BUTTON_THUMB_RIGHT;
    if(flags & LEFT_BUTTON)  buttons |= DS4_BUTTON_SHOULDER_LEFT;
    if(flags & RIGHT_BUTTON) buttons |= DS4_BUTTON_SHOULDER_RIGHT;
    if(flags & START)        buttons |= DS4_BUTTON_OPTIONS;
    if(flags & BACK)         buttons |= DS4_BUTTON_SHARE;
    if(flags & A)            buttons |= DS4_BUTTON_CROSS;
    if(flags & B)            buttons |= DS4_BUTTON_CIRCLE;
    if(flags & X)            buttons |= DS4_BUTTON_SQUARE;
    if(flags & Y)            buttons |= DS4_BUTTON_TRIANGLE;

    if(gamepad_state.lt > 0) buttons |= DS4_BUTTON_TRIGGER_LEFT;
    if(gamepad_state.rt > 0) buttons |= DS4_BUTTON_TRIGGER_RIGHT;
    // clang-format on

    return (DS4_BUTTONS) buttons;
  }

  static DS4_SPECIAL_BUTTONS
  ds4_special_buttons(const gamepad_state_t &gamepad_state) {
    int buttons {};

    if (gamepad_state.buttonFlags & HOME) buttons |= DS4_SPECIAL_BUTTON_PS;

    // Allow either PS4/PS5 clickpad button or Xbox Series X share button to activate DS4 clickpad
    if (gamepad_state.buttonFlags & (TOUCHPAD_BUTTON | MISC_BUTTON)) buttons |= DS4_SPECIAL_BUTTON_TOUCHPAD;

    return (DS4_SPECIAL_BUTTONS) buttons;
  }

  static std::uint8_t
  to_ds4_triggerX(std::int16_t v) {
    return (v + std::numeric_limits<std::uint16_t>::max() / 2 + 1) / 257;
  }

  static std::uint8_t
  to_ds4_triggerY(std::int16_t v) {
    auto new_v = -((std::numeric_limits<std::uint16_t>::max() / 2 + v - 1)) / 257;

    return new_v == 0 ? 0xFF : (std::uint8_t) new_v;
  }

  /**
   * @brief Updates the DS4 input report with the provided gamepad state.
   * @param gamepad The gamepad to update.
   * @param gamepad_state The gamepad button/axis state sent from the client.
   */
  static void
  ds4_update_state(gamepad_context_t &gamepad, const gamepad_state_t &gamepad_state) {
    auto &report = gamepad.report.ds4.Report;

    report.wButtons = ds4_buttons(gamepad_state) | ds4_dpad(gamepad_state);
    report.bSpecial = ds4_special_buttons(gamepad_state);

    report.bTriggerL = gamepad_state.lt;
    report.bTriggerR = gamepad_state.rt;

    report.bThumbLX = to_ds4_triggerX(gamepad_state.lsX);
    report.bThumbLY = to_ds4_triggerY(gamepad_state.lsY);

    report.bThumbRX = to_ds4_triggerX(gamepad_state.rsX);
    report.bThumbRY = to_ds4_triggerY(gamepad_state.rsY);
  }

  /**
   * @brief Updates virtual gamepad with the provided gamepad state.
   * @param input The input context.
   * @param nr The gamepad index to update.
   * @param gamepad_state The gamepad button/axis state sent from the client.
   */
  void
  gamepad(input_t &input, int nr, const gamepad_state_t &gamepad_state) {
    auto vigem = ((input_raw_t *) input.get())->vigem;

    // If there is no gamepad support
    if (!vigem) {
      return;
    }

    auto &gamepad = vigem->gamepads[nr];

    VIGEM_ERROR status;

    if (vigem_target_get_type(gamepad.gp.get()) == Xbox360Wired) {
      x360_update_state(gamepad, gamepad_state);
      status = vigem_target_x360_update(vigem->client.get(), gamepad.gp.get(), gamepad.report.x360);
    }
    else {
      ds4_update_state(gamepad, gamepad_state);
      status = vigem_target_ds4_update_ex(vigem->client.get(), gamepad.gp.get(), gamepad.report.ds4);
    }

    if (!VIGEM_SUCCESS(status)) {
      BOOST_LOG(warning) << "Couldn't send gamepad input to ViGEm ["sv << util::hex(status).to_string_view() << ']';
    }
  }

  void
  freeInput(void *p) {
    auto input = (input_raw_t *) p;

    delete input;
  }

  /**
   * @brief Gets the supported gamepads for this platform backend.
   * @return Vector of gamepad type strings.
   */
  std::vector<std::string_view> &
  supported_gamepads() {
    // ds4 == ps4
    static std::vector<std::string_view> gps {
      "auto"sv, "x360"sv, "ds4"sv, "ps4"sv
    };

    return gps;
  }
}  // namespace platf
