// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "Common/CommonTypes.h"
#include "Core/API/Events.h"
#include "Core/HW/WiimoteCommon/DataReport.h"
#include "Core/HW/WiimoteEmu/DesiredWiimoteState.h"
#include "Core/HW/WiimoteEmu/Encryption.h"
#include "Core/HW/WiimoteEmu/Extension/Nunchuk.h"
#include "InputCommon/GCPadStatus.h"

namespace API
{
enum class ClearOn
{
  NextPoll = 0,
  NextFrame = 1,
  NextOverride = 2,
};

template <typename T>
class BaseManip
{
public:
  BaseManip(API::EventHub& event_hub) : m_event_hub(event_hub)
  {
    m_frame_advanced_listener = m_event_hub.ListenEvent<API::Events::FrameAdvance>(
      [&](const API::Events::FrameAdvance&) { NotifyFrameAdvanced(); });
  }
  ~BaseManip() { m_event_hub.UnlistenEvent(m_frame_advanced_listener); }
  void Clear() { m_overrides.clear(); }
  void NotifyFrameAdvanced()
  {
    // std::erase_if back-ported to C++17
    for (auto i = m_overrides.begin(), last = m_overrides.end(); i != last; )
    {
      auto kvp = *i;
      if (kvp.second.clear_on == ClearOn::NextFrame && kvp.second.used)
        i = m_overrides.erase(i);
      else
        ++i;
    }
  }

protected:
  std::map<int, T> m_overrides;

private:
  API::EventHub& m_event_hub;
  API::ListenerID<API::Events::FrameAdvance> m_frame_advanced_listener;
};

struct GCInputOverride
{
  GCPadStatus pad_status;
  ClearOn clear_on;
  bool used;
};

struct WiiInputButtonsOverride
{
  WiimoteCommon::ButtonData button_data;
  ClearOn clear_on;
  bool used;
};

struct IRCameraTransform
{
  Common::Vec3 position;
  Common::Vec3 pitch_yaw_roll;
};

struct WiiInputIROverride
{
  IRCameraTransform ircamera_transform;
  ClearOn clear_on;
  bool used;
};

struct WiiAccelOverride
{
  WiimoteCommon::AccelData accel_data;
  ClearOn clear_on;
  bool used;
};

struct WiiMotionPlusOverride
{
  WiimoteEmu::MotionPlus::DataFormat::Data motion_plus_data;
  ClearOn clear_on;
  bool used;
};

struct NunchuckButtonsOverride
{
  WiimoteEmu::Nunchuk::DataFormat button_data;
  ClearOn clear_on;
  bool used;
};

struct NunchuckAccelOverride
{
  WiimoteCommon::AccelData accel_data;
  ClearOn clear_on;
  bool used;
};

class GCManip : public BaseManip<GCInputOverride>
{
public:
  using BaseManip::BaseManip;
  GCPadStatus Get(int controller_id);
  void Set(GCPadStatus pad_status, int controller_id, ClearOn clear_on);
  void PerformInputManip(GCPadStatus* pad_status, int controller_id);
};

class WiiButtonsManip : public BaseManip<WiiInputButtonsOverride>
{
public:
  using BaseManip::BaseManip;
  WiimoteCommon::ButtonData Get(int controller_id);
  bool TryGetOverride(int controller_id, WiimoteCommon::ButtonData* button_data) const;
  void Set(WiimoteCommon::ButtonData button_data, int controller_id, ClearOn clear_on);
  void PerformInputManip(WiimoteCommon::DataReportBuilder& rpt, int controller_id);
};

class WiiIRManip : public BaseManip<WiiInputIROverride>
{
public:
  using BaseManip::BaseManip;
  void Set(IRCameraTransform ircamera_transform, int controller_id, ClearOn clear_on);
  void PerformInputManip(WiimoteCommon::DataReportBuilder& rpt, int controller_id);
};

class WiiAccelManip : public BaseManip<WiiAccelOverride>
{
public:
  using BaseManip::BaseManip;
  WiimoteCommon::AccelData Get(int controller_id) const;
  void SetRaw(int controller_id, WiimoteCommon::AccelData accel_data);
  bool TryGetOverride(int controller_id, WiimoteCommon::AccelData* accel_data) const;
  void Set(WiimoteCommon::AccelData accel_data, int controller_id, ClearOn clear_on);

private:
  std::map<int, WiimoteCommon::AccelData> m_accel_state;
};

class WiiMotionPlusManip : public BaseManip<WiiMotionPlusOverride>
{
public:
  using BaseManip::BaseManip;
  std::optional<WiimoteEmu::MotionPlus::DataFormat::Data> Get(int controller_id) const;
  void SetRaw(int controller_id,
              std::optional<WiimoteEmu::MotionPlus::DataFormat::Data> motion_plus_data);
  bool TryGetOverride(int controller_id,
                      WiimoteEmu::MotionPlus::DataFormat::Data* motion_plus_data) const;
  void Set(WiimoteEmu::MotionPlus::DataFormat::Data motion_plus_data, int controller_id,
           ClearOn clear_on);

private:
  std::map<int, WiimoteEmu::MotionPlus::DataFormat::Data> m_motion_plus_state;
};

class NunchuckButtonsManip : public BaseManip<NunchuckButtonsOverride>
{
public:
  using BaseManip::BaseManip;
  WiimoteEmu::Nunchuk::DataFormat Get(int controller_id);
  WiimoteEmu::Nunchuk::DataFormat GetRaw(int controller_id) const;
  void SetRaw(int controller_id, WiimoteEmu::Nunchuk::DataFormat button_data);
  bool TryGetOverride(int controller_id, WiimoteEmu::Nunchuk::DataFormat* button_data) const;
  void Set(WiimoteEmu::Nunchuk::DataFormat button_data, int controller_id, ClearOn clear_on);
  void PerformInputManip(WiimoteCommon::DataReportBuilder& rpt, int controller_id,
                         WiimoteEmu::EncryptionKey key);
  void SaveNunchuckState(WiimoteCommon::DataReportBuilder& rpt,
                         int controller_id, WiimoteEmu::EncryptionKey key);

private:
  std::map<int, WiimoteEmu::Nunchuk::DataFormat> m_nunchuk_state;
  std::map<int, WiimoteEmu::Nunchuk::DataFormat> m_raw_nunchuk_state;
};

class NunchuckAccelManip : public BaseManip<NunchuckAccelOverride>
{
public:
  using BaseManip::BaseManip;
  WiimoteCommon::AccelData Get(int controller_id) const;
  void SetRaw(int controller_id, WiimoteCommon::AccelData accel_data);
  bool TryGetOverride(int controller_id, WiimoteCommon::AccelData* accel_data) const;
  void Set(WiimoteCommon::AccelData accel_data, int controller_id, ClearOn clear_on);

private:
  std::map<int, WiimoteCommon::AccelData> m_accel_state;
};

// global instances
GCManip& GetGCManip();
WiiButtonsManip& GetWiiButtonsManip();
WiiIRManip& GetWiiIRManip();
WiiAccelManip& GetWiiAccelManip();
WiiMotionPlusManip& GetWiiMotionPlusManip();
NunchuckButtonsManip& GetNunchuckButtonsManip();
NunchuckAccelManip& GetNunchuckAccelManip();
void ApplyManipToDesiredWiimoteState(int controller_id, WiimoteEmu::DesiredWiimoteState* state);

}  // namespace API
