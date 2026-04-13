// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Controller.h"

#include <algorithm>
#include <cmath>

#include "Common/BitUtils.h"
#include "Core/Config/MainSettings.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/Camera.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/Movie.h"
#include "Core/System.h"
#include "InputCommon/GCAdapter.h"
#include "InputCommon/InputConfig.h"

namespace
{
bool IsMovieInputPlaybackActive()
{
  return Core::System::GetInstance().GetMovie().IsPlayingInput();
}

constexpr WiimoteCommon::AccelData k_default_nunchuck_acceleration = WiimoteCommon::AccelData(
    {WiimoteEmu::Nunchuk::ACCEL_ZERO_G << 2, WiimoteEmu::Nunchuk::ACCEL_ZERO_G << 2,
     WiimoteEmu::Nunchuk::ACCEL_ONE_G << 2});

WiimoteEmu::Wiimote* GetEmulatedWiimote(int controller_id)
{
  auto* const config = ::Wiimote::GetConfig();
  if (config == nullptr)
    return nullptr;

  auto* const controller = config->GetController(controller_id);
  return static_cast<WiimoteEmu::Wiimote*>(controller);
}

std::array<WiimoteEmu::CameraPoint, WiimoteEmu::CameraLogic::NUM_POINTS>
GetCameraPointsForTransform(const API::IRCameraTransform& ircamera_transform)
{
  using namespace Common;
  using WiimoteEmu::CameraLogic;

  const auto face_forward = Matrix33::RotateX(static_cast<float>(MathUtil::TAU) / -4);
  const auto transform =
      Matrix44::FromMatrix33(face_forward) *
      Matrix44::FromQuaternion(Quaternion::RotateXYZ(ircamera_transform.pitch_yaw_roll)) *
      Matrix44::Translate(ircamera_transform.position);
  const Vec2 field_of_view = {CameraLogic::CAMERA_FOV_X, CameraLogic::CAMERA_FOV_Y};
  return CameraLogic::GetCameraPoints(transform, field_of_view);
}

void WriteIRDataForPoints(WiimoteCommon::DataReportBuilder& rpt,
                          const std::array<WiimoteEmu::CameraPoint,
                                           WiimoteEmu::CameraLogic::NUM_POINTS>& camera_points)
{
  using WiimoteCommon::IRReportFormat;
  using WiimoteEmu::CameraLogic;

  u8* const ir_data = rpt.GetIRDataPtr();
  if (ir_data == nullptr)
    return;

  std::fill_n(ir_data, rpt.GetIRDataSize(), u8(0xff));

  switch (rpt.GetIRReportFormat())
  {
  case IRReportFormat::Basic:
    for (size_t i = 0; i != camera_points.size() / 2; ++i)
    {
      WiimoteEmu::IRBasic ir = {};
      ir.SetObject1(camera_points[i * 2].position);
      ir.SetObject2(camera_points[i * 2 + 1].position);
      Common::BitCastPtr<WiimoteEmu::IRBasic>(&ir_data[i * sizeof(WiimoteEmu::IRBasic)]) = ir;
    }
    break;
  case IRReportFormat::Extended:
    for (size_t i = 0; i != camera_points.size(); ++i)
    {
      const auto& point = camera_points[i];
      if (point.position.x >= CameraLogic::CAMERA_RES_X)
        continue;

      WiimoteEmu::IRExtended ir = {};
      ir.SetPosition(point.position);
      ir.size = point.size;
      Common::BitCastPtr<WiimoteEmu::IRExtended>(&ir_data[i * sizeof(WiimoteEmu::IRExtended)]) =
          ir;
    }
    break;
  case IRReportFormat::Full1:
  case IRReportFormat::Full2:
  {
    const size_t start_index = rpt.GetIRReportFormat() == IRReportFormat::Full2 ? 2 : 0;
    for (size_t i = 0; i != 2; ++i)
    {
      const auto& point = camera_points[start_index + i];
      if (point.position.x >= CameraLogic::CAMERA_RES_X)
        continue;

      WiimoteEmu::IRFull ir = {};
      ir.SetPosition(point.position);
      ir.size = point.size;
      ir.xmin = std::max(static_cast<int>(point.position.x) - static_cast<int>(point.size), 0);
      ir.ymin = std::max(static_cast<int>(point.position.y) - static_cast<int>(point.size), 0);
      ir.xmax = std::min(static_cast<int>(point.position.x) + static_cast<int>(point.size),
                         CameraLogic::CAMERA_RES_X);
      ir.ymax = std::min(static_cast<int>(point.position.y) + static_cast<int>(point.size),
                         CameraLogic::CAMERA_RES_Y);

      constexpr int subpixel_resolution = 8;
      constexpr long max_intensity = 0xff;
      const auto intensity =
          std::lround((ir.xmax - ir.xmin) * (ir.ymax - ir.ymin) / subpixel_resolution /
                      subpixel_resolution * MathUtil::TAU / 8);
      ir.intensity = static_cast<u8>(std::min(max_intensity, intensity));

      Common::BitCastPtr<WiimoteEmu::IRFull>(&ir_data[i * sizeof(WiimoteEmu::IRFull)]) = ir;
    }
    break;
  }
  case IRReportFormat::None:
    break;
  }
}
}  // namespace

namespace API
{

GCPadStatus GCManip::Get(int controller_id)
{
  auto iter = m_overrides.find(controller_id);
  if (iter != m_overrides.end())
    return iter->second.pad_status;
  if (Config::Get(Config::GetInfoForSIDevice(controller_id)) == SerialInterface::SIDEVICE_WIIU_ADAPTER)
    return GCAdapter::Input(controller_id);
  else
    return Pad::GetStatus(controller_id);
}

void GCManip::Set(GCPadStatus pad_status, int controller_id, ClearOn clear_on)
{
  if (IsMovieInputPlaybackActive())
  {
    m_overrides.erase(controller_id);
    return;
  }

  m_overrides[controller_id] = {pad_status, clear_on, /* used: */ false};
}

void GCManip::PerformInputManip(GCPadStatus* pad_status, int controller_id)
{
  if (IsMovieInputPlaybackActive())
  {
    m_overrides.erase(controller_id);
    return;
  }

  auto iter = m_overrides.find(controller_id);
  if (iter == m_overrides.end())
  {
    return;
  }
  GCInputOverride& input_override = iter->second;
  *pad_status = input_override.pad_status;
  if (input_override.clear_on == ClearOn::NextPoll)
  {
    m_overrides.erase(controller_id);
    return;
  }
  input_override.used = true;
}

WiimoteCommon::ButtonData WiiButtonsManip::Get(int controller_id)
{
  auto iter = m_overrides.find(controller_id);
  if (iter != m_overrides.end())
    return iter->second.button_data;

  if (auto* const wiimote = GetEmulatedWiimote(controller_id))
    return wiimote->GetCurrentlyPressedButtons();

  return {};
}

bool WiiButtonsManip::TryGetOverride(int controller_id, WiimoteCommon::ButtonData* button_data) const
{
  if (IsMovieInputPlaybackActive())
    return false;

  const auto iter = m_overrides.find(controller_id);
  if (iter == m_overrides.end())
    return false;

  *button_data = iter->second.button_data;
  return true;
}

void WiiButtonsManip::Set(WiimoteCommon::ButtonData button_data, int controller_id,
                          ClearOn clear_on)
{
  if (IsMovieInputPlaybackActive())
  {
    m_overrides.erase(controller_id);
    return;
  }

  m_overrides[controller_id] = {button_data, clear_on, /* used: */ false};
}

void WiiButtonsManip::PerformInputManip(WiimoteCommon::DataReportBuilder& rpt, int controller_id)
{
  if (IsMovieInputPlaybackActive())
  {
    m_overrides.erase(controller_id);
    return;
  }

  if (!rpt.HasCore())
  {
    return;
  }
  auto iter = m_overrides.find(controller_id);
  if (iter == m_overrides.end())
  {
    return;
  }
  WiiInputButtonsOverride& input_override = iter->second;

  WiimoteCommon::DataReportBuilder::CoreData core;
  rpt.GetCoreData(&core);
  core.hex = input_override.button_data.hex;
  rpt.SetCoreData(core);
  if (input_override.clear_on == ClearOn::NextPoll)
  {
    m_overrides.erase(controller_id);
    return;
  }
  input_override.used = true;
}

void WiiIRManip::Set(IRCameraTransform ircamera_transform, int controller_id, ClearOn clear_on)
{
  if (IsMovieInputPlaybackActive())
  {
    m_overrides.erase(controller_id);
    return;
  }

  m_overrides[controller_id] = {ircamera_transform, clear_on, /* used: */ false};
}

void WiiIRManip::PerformInputManip(WiimoteCommon::DataReportBuilder& rpt, int controller_id)
{
  if (IsMovieInputPlaybackActive())
  {
    m_overrides.erase(controller_id);
    return;
  }

  if (!rpt.HasIR())
  {
    return;
  }
  const auto iter = m_overrides.find(controller_id);
  if (iter == m_overrides.end())
  {
    return;
  }
  WiiInputIROverride& input_override = iter->second;

  WriteIRDataForPoints(rpt, GetCameraPointsForTransform(input_override.ircamera_transform));
  if (input_override.clear_on == ClearOn::NextPoll)
  {
    m_overrides.erase(controller_id);
    return;
  }
  input_override.used = true;
}

WiimoteCommon::AccelData WiiAccelManip::Get(int controller_id) const
{
  const auto iter = m_overrides.find(controller_id);
  if (iter != m_overrides.end())
    return iter->second.accel_data;

  const auto raw_iter = m_accel_state.find(controller_id);
  return raw_iter != m_accel_state.end() ? raw_iter->second :
                                           WiimoteEmu::DesiredWiimoteState::DEFAULT_ACCELERATION;
}

void WiiAccelManip::SetRaw(int controller_id, WiimoteCommon::AccelData accel_data)
{
  m_accel_state[controller_id] = accel_data;
}

bool WiiAccelManip::TryGetOverride(int controller_id, WiimoteCommon::AccelData* accel_data) const
{
  if (IsMovieInputPlaybackActive())
    return false;

  const auto iter = m_overrides.find(controller_id);
  if (iter == m_overrides.end())
    return false;

  *accel_data = iter->second.accel_data;
  return true;
}

void WiiAccelManip::Set(WiimoteCommon::AccelData accel_data, int controller_id, ClearOn clear_on)
{
  if (IsMovieInputPlaybackActive())
  {
    m_overrides.erase(controller_id);
    return;
  }

  m_overrides[controller_id] = {accel_data, clear_on, /* used: */ false};
}

std::optional<WiimoteEmu::MotionPlus::DataFormat::Data>
WiiMotionPlusManip::Get(int controller_id) const
{
  const auto iter = m_overrides.find(controller_id);
  if (iter != m_overrides.end())
    return iter->second.motion_plus_data;

  const auto raw_iter = m_motion_plus_state.find(controller_id);
  if (raw_iter == m_motion_plus_state.end())
    return std::nullopt;

  return raw_iter->second;
}

void WiiMotionPlusManip::SetRaw(
    int controller_id, std::optional<WiimoteEmu::MotionPlus::DataFormat::Data> motion_plus_data)
{
  if (motion_plus_data.has_value())
    m_motion_plus_state[controller_id] = *motion_plus_data;
  else
    m_motion_plus_state.erase(controller_id);
}

bool WiiMotionPlusManip::TryGetOverride(
    int controller_id, WiimoteEmu::MotionPlus::DataFormat::Data* motion_plus_data) const
{
  if (IsMovieInputPlaybackActive())
    return false;

  const auto iter = m_overrides.find(controller_id);
  if (iter == m_overrides.end())
    return false;

  *motion_plus_data = iter->second.motion_plus_data;
  return true;
}

void WiiMotionPlusManip::Set(WiimoteEmu::MotionPlus::DataFormat::Data motion_plus_data,
                             int controller_id, ClearOn clear_on)
{
  if (IsMovieInputPlaybackActive())
  {
    m_overrides.erase(controller_id);
    return;
  }

  m_overrides[controller_id] = {motion_plus_data, clear_on, /* used: */ false};
}

WiimoteEmu::Nunchuk::DataFormat NunchuckButtonsManip::Get(int controller_id)
{
  auto iter = m_overrides.find(controller_id);
  if (iter != m_overrides.end())
    return iter->second.button_data;
  return m_nunchuk_state[controller_id];
}

WiimoteEmu::Nunchuk::DataFormat NunchuckButtonsManip::GetRaw(int controller_id) const
{
  const auto iter = m_raw_nunchuk_state.find(controller_id);
  if (iter != m_raw_nunchuk_state.end())
    return iter->second;
  return {};
}

void NunchuckButtonsManip::SetRaw(int controller_id, WiimoteEmu::Nunchuk::DataFormat button_data)
{
  m_raw_nunchuk_state[controller_id] = button_data;
}

bool NunchuckButtonsManip::TryGetOverride(int controller_id,
                                          WiimoteEmu::Nunchuk::DataFormat* button_data) const
{
  if (IsMovieInputPlaybackActive())
    return false;

  const auto iter = m_overrides.find(controller_id);
  if (iter == m_overrides.end())
    return false;

  *button_data = iter->second.button_data;
  return true;
}

void NunchuckButtonsManip::Set(WiimoteEmu::Nunchuk::DataFormat button_data, int controller_id,
                               ClearOn clear_on)
{
  if (IsMovieInputPlaybackActive())
  {
    m_overrides.erase(controller_id);
    return;
  }

  m_overrides[controller_id] = {button_data, clear_on, /* used: */ false};
}

void NunchuckButtonsManip::PerformInputManip(WiimoteCommon::DataReportBuilder& rpt,
                                             int controller_id, WiimoteEmu::EncryptionKey key)
{
  if (IsMovieInputPlaybackActive())
  {
    m_overrides.erase(controller_id);
    return;
  }

  if (!rpt.HasExt())
  {
    return;
  }
  auto iter = m_overrides.find(controller_id);
  if (iter == m_overrides.end())
  {
    return;
  }
  NunchuckButtonsOverride& input_overrides = iter->second;

  auto nunchuk = reinterpret_cast<WiimoteEmu::Nunchuk::DataFormat*>(rpt.GetExtDataPtr());
  key.Decrypt((u8*)nunchuk, 0, sizeof(*nunchuk));
  *nunchuk = input_overrides.button_data;
  key.Encrypt((u8*)nunchuk, 0, sizeof(*nunchuk));
  if (input_overrides.clear_on == ClearOn::NextPoll)
  {
    m_overrides.erase(controller_id);
    return;
  }
  input_overrides.used = true;
}

// There doesn't seem to be an easy way to grab the state of the Nunchuck at a later point
// For now, let's save the state of the nunchuck inside the manip class.
// This is only relevant for get_nunchuck_buttons API.
void NunchuckButtonsManip::SaveNunchuckState(WiimoteCommon::DataReportBuilder& rpt,
                                             int controller_id, WiimoteEmu::EncryptionKey key)
{
  if (!rpt.HasExt())
    return;

  auto nunchuk = reinterpret_cast<WiimoteEmu::Nunchuk::DataFormat*>(rpt.GetExtDataPtr());
  key.Decrypt((u8*)nunchuk, 0, sizeof(*nunchuk));
  m_nunchuk_state[controller_id] = *nunchuk;
  key.Encrypt((u8*)nunchuk, 0, sizeof(*nunchuk));
}

WiimoteCommon::AccelData NunchuckAccelManip::Get(int controller_id) const
{
  const auto iter = m_overrides.find(controller_id);
  if (iter != m_overrides.end())
    return iter->second.accel_data;

  const auto raw_iter = m_accel_state.find(controller_id);
  return raw_iter != m_accel_state.end() ? raw_iter->second : k_default_nunchuck_acceleration;
}

void NunchuckAccelManip::SetRaw(int controller_id, WiimoteCommon::AccelData accel_data)
{
  m_accel_state[controller_id] = accel_data;
}

bool NunchuckAccelManip::TryGetOverride(int controller_id,
                                        WiimoteCommon::AccelData* accel_data) const
{
  if (IsMovieInputPlaybackActive())
    return false;

  const auto iter = m_overrides.find(controller_id);
  if (iter == m_overrides.end())
    return false;

  *accel_data = iter->second.accel_data;
  return true;
}

void NunchuckAccelManip::Set(WiimoteCommon::AccelData accel_data, int controller_id,
                             ClearOn clear_on)
{
  if (IsMovieInputPlaybackActive())
  {
    m_overrides.erase(controller_id);
    return;
  }

  m_overrides[controller_id] = {accel_data, clear_on, /* used: */ false};
}

GCManip& GetGCManip()
{
  static GCManip manip(GetEventHub());
  return manip;
}

WiiButtonsManip& GetWiiButtonsManip()
{
  static WiiButtonsManip manip(GetEventHub());
  return manip;
}

WiiIRManip& GetWiiIRManip()
{
  static WiiIRManip manip(GetEventHub());
  return manip;
}

WiiAccelManip& GetWiiAccelManip()
{
  static WiiAccelManip manip(GetEventHub());
  return manip;
}

WiiMotionPlusManip& GetWiiMotionPlusManip()
{
  static WiiMotionPlusManip manip(GetEventHub());
  return manip;
}

NunchuckButtonsManip& GetNunchuckButtonsManip()
{
  static NunchuckButtonsManip manip(GetEventHub());
  return manip;
}

NunchuckAccelManip& GetNunchuckAccelManip()
{
  static NunchuckAccelManip manip(GetEventHub());
  return manip;
}

void ApplyManipToDesiredWiimoteState(int controller_id, WiimoteEmu::DesiredWiimoteState* state)
{
  if (IsMovieInputPlaybackActive())
    return;

  GetWiiAccelManip().SetRaw(controller_id, state->acceleration);
  GetWiiMotionPlusManip().SetRaw(controller_id, state->motion_plus);

  if (std::holds_alternative<WiimoteEmu::Nunchuk::DataFormat>(state->extension.data))
  {
    GetNunchuckAccelManip().SetRaw(
        controller_id, std::get<WiimoteEmu::Nunchuk::DataFormat>(state->extension.data).GetAccel());
    GetNunchuckButtonsManip().SetRaw(
        controller_id, std::get<WiimoteEmu::Nunchuk::DataFormat>(state->extension.data));
  }

  WiimoteCommon::ButtonData button_override;
  if (GetWiiButtonsManip().TryGetOverride(controller_id, &button_override))
    state->buttons = button_override;

  WiimoteCommon::AccelData accel_override;
  if (GetWiiAccelManip().TryGetOverride(controller_id, &accel_override))
    state->acceleration = accel_override;

  WiimoteEmu::MotionPlus::DataFormat::Data motion_plus_override;
  if (GetWiiMotionPlusManip().TryGetOverride(controller_id, &motion_plus_override))
    state->motion_plus = motion_plus_override;

  WiimoteEmu::Nunchuk::DataFormat nunchuk_override;
  if (GetNunchuckButtonsManip().TryGetOverride(controller_id, &nunchuk_override))
  {
    if (std::holds_alternative<WiimoteEmu::Nunchuk::DataFormat>(state->extension.data))
    {
      const auto current_nunchuk = std::get<WiimoteEmu::Nunchuk::DataFormat>(state->extension.data);
      nunchuk_override.SetAccel(current_nunchuk.GetAccel().value);
    }
    state->extension.data = nunchuk_override;
  }

  WiimoteCommon::AccelData nunchuk_accel_override;
  if (GetNunchuckAccelManip().TryGetOverride(controller_id, &nunchuk_accel_override) &&
      std::holds_alternative<WiimoteEmu::Nunchuk::DataFormat>(state->extension.data))
  {
    auto nunchuk_state = std::get<WiimoteEmu::Nunchuk::DataFormat>(state->extension.data);
    nunchuk_state.SetAccel(nunchuk_accel_override.value);
    state->extension.data = nunchuk_state;
  }
}

}  // namespace API
