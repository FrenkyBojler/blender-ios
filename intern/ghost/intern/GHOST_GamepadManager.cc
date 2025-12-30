/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <optional>

#include "GHOST_EventGamepad.hh"
#include "GHOST_GamepadManager.hh"
#include "GHOST_System.hh"
#include "GHOST_WindowManager.hh"

#include "SDL.h"
#include "SDL_gamecontroller.h"

struct GHOST_Gamepad {
  SDL_GameController *controller = nullptr;
  GHOST_GamepadState state = {};
  constexpr GHOST_Gamepad(SDL_GameController *controller) : controller{controller} {}
};

GHOST_GamepadManager::GHOST_GamepadManager(GHOST_System &sys) : system_(sys), dead_zone_(0.2)
{
  if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) < 0) {
    printf("SDL_INIT_GAMECONTROLLER subsystem init error.");
  }
}

GHOST_GamepadManager::~GHOST_GamepadManager()
{
  if (gamepad_) {
    SDL_GameControllerClose(gamepad_->controller);
  }
  SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}

void GHOST_GamepadManager::send_gamepad_events(float dt)
{
  if (!system_.getWindowManager()->getActiveWindow()) {
    return;
  }
  GHOST_GamepadState state = gamepad_ ? gamepad_->state : GHOST_GamepadState{};
  GHOST_GamepadState old_state = state;

  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    switch (event.type) {
      case SDL_CONTROLLERDEVICEADDED: {
        if (!gamepad_) {
          printf("Gamepad connected.");
          gamepad_ = std::make_unique<GHOST_Gamepad>(SDL_GameControllerOpen(event.cdevice.which));
        }
        break;
      }
      case SDL_CONTROLLERDEVICEREMOVED: {
        if (gamepad_ &&
            event.cdevice.which ==
                SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gamepad_->controller)))
        {
          printf("Gamepad disconnected.");
          SDL_GameControllerClose(gamepad_->controller);
          gamepad_.reset();
        }
        break;
      }
      default:
        break;
    }
    if (!gamepad_) {
      return;
    }
    switch (event.type) {
      case SDL_CONTROLLERAXISMOTION: {
        switch (event.caxis.axis) {
          case SDL_CONTROLLER_AXIS_LEFTX:
            state.left_thumb[0] = float(event.caxis.value) / 32767.0f;
            break;
          case SDL_CONTROLLER_AXIS_LEFTY:
            state.left_thumb[1] = float(event.caxis.value) / 32767.0f;
            break;
          case SDL_CONTROLLER_AXIS_RIGHTX:
            state.right_thumb[0] = float(event.caxis.value) / 32767.0f;
            break;
          case SDL_CONTROLLER_AXIS_RIGHTY:
            state.right_thumb[1] = float(event.caxis.value) / 32767.0f;
            break;
          case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
            state.left_trigger = float(event.caxis.value) / 32767.0f;
            break;
          case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
            state.right_trigger = float(event.caxis.value) / 32767.0f;
            break;
          default:
            break;
        }

        break;
      }
      case SDL_CONTROLLERBUTTONDOWN:
      case SDL_CONTROLLERBUTTONUP: {
#define button_case(button, mask) \
  case button: { \
    return mask; \
  };
        std::optional<GamepadButtonMask> mask =
            [](SDL_GameControllerButton button) -> std::optional<GamepadButtonMask> {
          switch (button) {
            button_case(SDL_CONTROLLER_BUTTON_A, GamepadButtonMask::A);
            button_case(SDL_CONTROLLER_BUTTON_B, GamepadButtonMask::B);
            button_case(SDL_CONTROLLER_BUTTON_X, GamepadButtonMask::X);
            button_case(SDL_CONTROLLER_BUTTON_Y, GamepadButtonMask::Y);
            // button_case(SDL_CONTROLLER_BUTTON_BACK, GamepadButtonMask::??);
            button_case(SDL_CONTROLLER_BUTTON_GUIDE, GamepadButtonMask::View);
            button_case(SDL_CONTROLLER_BUTTON_START, GamepadButtonMask::Menu);
            button_case(SDL_CONTROLLER_BUTTON_LEFTSTICK, GamepadButtonMask::LeftThumb);
            button_case(SDL_CONTROLLER_BUTTON_RIGHTSTICK, GamepadButtonMask::RightThumb);
            button_case(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, GamepadButtonMask::LeftShoulder);
            button_case(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, GamepadButtonMask::RightShoulder);
            button_case(SDL_CONTROLLER_BUTTON_DPAD_UP, GamepadButtonMask::DPadUp);
            button_case(SDL_CONTROLLER_BUTTON_DPAD_DOWN, GamepadButtonMask::DPadDown);
            button_case(SDL_CONTROLLER_BUTTON_DPAD_LEFT, GamepadButtonMask::DPadLeft);
            button_case(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, GamepadButtonMask::DPadRight);
            default:
              break;
          }
          return std::nullopt;
        }(SDL_GameControllerButton(event.cbutton.button));
        if (!mask.has_value()) {
          break;
        }
        state.button_depressed[int(*mask)] = event.cbutton.state == SDL_PRESSED;
        break;
      }
      default:
        break;
    }
  }
  this->send_gamepad_events(state, old_state, dt);
  if (gamepad_) {
    gamepad_->state = state;
  }
}

void GHOST_GamepadManager::send_gamepad_events(GHOST_GamepadState &new_state,
                                               GHOST_GamepadState &old_state,
                                               float delta_time)
{
  GHOST_IWindow *window = system_.getWindowManager()->getActiveWindow();

  const uint64_t now = system_.getMilliSeconds();

  const auto is_zero_input = [](const float (&val)[2]) {
    return val[0] == 0.0f && val[1] == 0.0f;
  };

  const auto send_thumb_event = [&, this](const float (&old_vals)[2],
                                          float (&new_vals)[2],
                                          GHOST_TGamepadThumb thumb) -> void {
    if (std::abs(new_vals[0] * new_vals[0] + new_vals[1] * new_vals[1]) <
        (dead_zone_ * dead_zone_))
    {
      new_vals[0] = new_vals[1] = 0.0f;
    }
    /* Send only thumb events if there is non-zero reading or the thumb has just been released.
     */
    if (is_zero_input(old_vals) && is_zero_input(new_vals)) {
      return;
    }
    std::unique_ptr<GHOST_EventGamepadThumb> event = std::make_unique<GHOST_EventGamepadThumb>(
        now, window);
    GHOST_TEventGamepadThumbData *data = (GHOST_TEventGamepadThumbData *)event->getData();
    data->value[0] = new_vals[0];
    data->value[1] = new_vals[1];
    data->thumb = thumb;
    data->action = !is_zero_input(new_vals) ? GHOST_kPress : GHOST_kRelease;
    data->dt = delta_time;
    system_.pushEvent(std::move(event));
  };

  send_thumb_event(old_state.left_thumb, new_state.left_thumb, GHOST_kGamepadLeftThumb);
  send_thumb_event(old_state.right_thumb, new_state.right_thumb, GHOST_kGamepadRightThumb);

  const auto send_trigger_event =
      [&, this](const float old_val, float new_val, GHOST_TGamepadTrigger trigger) -> void {
    new_val = std::abs(new_val) < dead_zone_ ? 0 : new_val;
    /* Send only triggers events if there is non-zero reading or the triggers has just been
     * released. */
    if (old_val == 0.0f && new_val == 0.0f) {
      return;
    }
    std::unique_ptr<GHOST_EventGamepadTrigger> event = std::make_unique<GHOST_EventGamepadTrigger>(
        now, window);
    GHOST_TEventGamepadTriggerData *data = (GHOST_TEventGamepadTriggerData *)event->getData();
    data->value = new_val;
    data->trigger = trigger;
    data->action = new_val ? GHOST_kPress : GHOST_kRelease;
    data->dt = delta_time;
    system_.pushEvent(std::move(event));
  };

  send_trigger_event(old_state.left_trigger, new_state.left_trigger, GHOST_kGamepadLeftTrigger);
  send_trigger_event(old_state.right_trigger, new_state.right_trigger, GHOST_kGamepadRightTrigger);

  struct ButtonMap {
    GamepadButtonMask mask;
    GHOST_TGamepadButton event_button;
  };
  constexpr ButtonMap buttons_map[]{
      {GamepadButtonMask::A, GHOST_kGamepadButtonA},
      {GamepadButtonMask::B, GHOST_kGamepadButtonB},
      {GamepadButtonMask::X, GHOST_kGamepadButtonX},
      {GamepadButtonMask::Y, GHOST_kGamepadButtonY},

      {GamepadButtonMask::LeftShoulder, GHOST_kGamepadButtonLeftShoulder},
      {GamepadButtonMask::RightShoulder, GHOST_kGamepadButtonRightShoulder},

      {GamepadButtonMask::View, GHOST_kGamepadButtonView},
      {GamepadButtonMask::Menu, GHOST_kGamepadButtonMenu},

      {GamepadButtonMask::LeftThumb, GHOST_kGamepadButtonLeftThumb},
      {GamepadButtonMask::RightThumb, GHOST_kGamepadButtonRightThumb},

      {GamepadButtonMask::DPadUp, GHOST_kGamepadButtonDPadUp},
      {GamepadButtonMask::DPadDown, GHOST_kGamepadButtonDPadDown},
      {GamepadButtonMask::DPadLeft, GHOST_kGamepadButtonDPadLeft},
      {GamepadButtonMask::DPadRight, GHOST_kGamepadButtonDPadRight},
  };

  for (const ButtonMap &button_map : buttons_map) {
    const bool was_depressed = old_state.button_depressed[int(button_map.mask)];
    const bool is_depressed = new_state.button_depressed[int(button_map.mask)];
    if (was_depressed != is_depressed || is_depressed) {

      std::unique_ptr<GHOST_EventGamepadButton> event = std::make_unique<GHOST_EventGamepadButton>(
          now, window);
      GHOST_TEventGamepadButtonData *data = (GHOST_TEventGamepadButtonData *)event->getData();

      data->action = is_depressed ? GHOST_kPress : GHOST_kRelease;
      data->button = button_map.event_button;

      system_.pushEvent(std::move(event));
    }
  }
}

void GHOST_GamepadManager::set_dead_zone(const float dz)
{
  dead_zone_ = std::clamp<float>(dz, 0.0f, 1.0f);
}
