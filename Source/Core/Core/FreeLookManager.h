// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <optional>

#include "InputCommon/ControllerEmu/ControllerEmu.h"

class CameraControllerInput;
class InputConfig;

namespace ControllerEmu
{
class ControlGroup;
class Buttons;
class IMUGyroscope;
}  // namespace ControllerEmu

enum class FreeLookGroup
{
  Move,
  Speed,
  FieldOfView,
  Other,
  Rotation,
  Keyframe,
};

namespace FreeLook
{
void Shutdown();
void Initialize();
void LoadInputConfig();
bool IsInitialized();
void UpdateInput();

InputConfig* GetInputConfig();
ControllerEmu::ControlGroup* GetInputGroup(int pad_num, FreeLookGroup group);

}  // namespace FreeLook

class FreeLookController final : public ControllerEmu::EmulatedController
{
public:
  explicit FreeLookController(unsigned int index);

  std::string GetName() const override;
  InputConfig* GetConfig() const override;
  void LoadDefaults(const ControllerInterface& ciface) override;

  ControllerEmu::ControlGroup* GetGroup(FreeLookGroup group) const;
  void Update();

private:
  void UpdateInput(CameraControllerInput* camera_controller);
  ControllerEmu::Buttons* m_move_buttons;
  ControllerEmu::Buttons* m_speed_buttons;
  ControllerEmu::Buttons* m_fov_buttons;
  ControllerEmu::Buttons* m_other_buttons;
  ControllerEmu::IMUGyroscope* m_rotation_gyro;
  ControllerEmu::Buttons* m_keyframe_buttons;

  const unsigned int m_index;
  std::optional<std::chrono::steady_clock::time_point> m_last_free_look_rotate_time;
  std::array<bool, 2> m_last_keyframe_button_states{};
};
