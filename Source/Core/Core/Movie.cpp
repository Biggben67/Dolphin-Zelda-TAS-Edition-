// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Movie.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <cmath>
#include <iterator>
#include <locale>
#include <mbedtls/md.h>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/IniFile.h"
#include "Common/IOFile.h"
#include "Common/MsgHandler.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"
#include "Common/Timer.h"
#include "Common/VariantUtil.h"
#include "Common/Version.h"

#include "Core/AchievementManager.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/AchievementSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/ConfigLoaders/MovieConfigLoader.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/DSP/DSPCore.h"
#include "Core/HW/CPU.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_DeviceIPL.h"
#include "Core/HW/EXI/EXI_DeviceMemoryCard.h"
#include "Core/HW/GBACore.h"
#include "Core/HW/GBAPad.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#ifdef HAS_LIBMGBA
#include "Core/HW/SI/SI_DeviceGBAEmu.h"
#endif
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteCommon/WiimoteReport.h"
#include "Core/HW/WiimoteEmu/Extension/Classic.h"
#include "Core/HW/WiimoteEmu/Extension/Nunchuk.h"
#include "Core/HW/WiimoteEmu/ExtensionPort.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/IOS/USB/Bluetooth/BTEmu.h"
#include "Core/IOS/USB/Bluetooth/WiimoteDevice.h"
#include "Core/NetPlayProto.h"
#include "Core/State.h"
#include "Core/System.h"
#include "Core/WiiUtils.h"

#include "InputCommon/GCPadStatus.h"
#include "InputCommon/InputConfig.h"

#include "VideoCommon/VideoBackendBase.h"

namespace Movie
{
using namespace WiimoteCommon;
using namespace WiimoteEmu;

namespace
{
constexpr u8 DTM_BATTERY_MAGIC = 0xB1;
constexpr u8 DTM_BATTERY_UNSET = 0xFF;
constexpr u8 DTM_GBA_DISCONNECT_MAGIC = 0xD6;

std::vector<int> GetActiveGCControllerOrder(const Movie::ControllerTypeArray& controllers)
{
  std::vector<int> active_controllers;
  for (int controller = 0; controller < 4; ++controller)
  {
    if (controllers[controller] == Movie::ControllerType::GC ||
        controllers[controller] == Movie::ControllerType::GBA)
      active_controllers.push_back(controller);
  }
  return active_controllers;
}

bool UsesOnlyGCPads(const Movie::ControllerTypeArray& controllers,
                    const Movie::WiimoteEnabledArray& wiimotes)
{
  if (std::ranges::any_of(wiimotes, [](bool enabled) { return enabled; }))
    return false;

  return std::ranges::none_of(controllers, [](Movie::ControllerType type) {
    return type != Movie::ControllerType::None && type != Movie::ControllerType::GC &&
           type != Movie::ControllerType::GBA;
  });
}

std::optional<int> GetSingleActiveWiimote(const Movie::ControllerTypeArray& controllers,
                                          const Movie::WiimoteEnabledArray& wiimotes)
{
  if (std::ranges::any_of(controllers, [](Movie::ControllerType type) {
        return type != Movie::ControllerType::None;
      }))
  {
    return std::nullopt;
  }

  int wiimote = -1;
  for (int i = 0; i < static_cast<int>(wiimotes.size()); ++i)
  {
    if (!wiimotes[i])
      continue;

    if (wiimote != -1)
      return std::nullopt;

    wiimote = i;
  }

  return wiimote >= 0 ? std::optional<int>(wiimote) : std::nullopt;
}

std::optional<std::vector<Movie::WiiRuntimeInputRow>> ParseSingleWiimoteRows(
    const std::vector<u8>& raw_input, int wiimote)
{
  std::vector<Movie::WiiRuntimeInputRow> rows;
  size_t offset = 0;
  while (offset < raw_input.size())
  {
    const u8 length = raw_input[offset++];
    Movie::WiiRuntimeInputRow row;
    row.wiimote = wiimote;
    row.is_reset = length == 0;
    row.serialized_state.length = length;

    if (length > row.serialized_state.data.size())
      return std::nullopt;

    if (offset + length > raw_input.size())
      return std::nullopt;

    if (length > 0)
      std::copy_n(raw_input.data() + offset, length, row.serialized_state.data.data());

    offset += length;
    rows.push_back(row);
  }

  return rows;
}
}  // namespace

static bool IsMovieHeader(const std::array<u8, 4>& magic)
{
  return magic[0] == 'D' && magic[1] == 'T' && magic[2] == 'M' && magic[3] == 0x1A;
}

static std::array<u8, 20> ConvertGitRevisionToBytes(const std::string& revision)
{
  std::array<u8, 20> revision_bytes{};

  if (revision.size() % 2 == 0 && std::ranges::all_of(revision, Common::IsXDigit))
  {
    // The revision string normally contains a git commit hash,
    // which is 40 hexadecimal digits long. In DTM files, each pair of
    // hexadecimal digits is stored as one byte, for a total of 20 bytes.
    size_t bytes_to_write = std::min(revision.size() / 2, revision_bytes.size());
    unsigned int temp;
    for (size_t i = 0; i < bytes_to_write; ++i)
    {
      sscanf(&revision[2 * i], "%02x", &temp);
      revision_bytes[i] = temp;
    }
  }
  else
  {
    // If the revision string for some reason doesn't only contain hexadecimal digit
    // pairs, we instead copy the string with no conversion. This probably doesn't match
    // the intended design of the DTM format, but it's the most sensible fallback.
    size_t bytes_to_write = std::min(revision.size(), revision_bytes.size());
    std::copy_n(std::begin(revision), bytes_to_write, std::begin(revision_bytes));
  }

  return revision_bytes;
}

MovieManager::MovieManager(Core::System& system) : m_system(system)
{
  m_dtm_wiimote_battery.fill(DTM_BATTERY_UNSET);
}

MovieManager::~MovieManager() = default;

// NOTE: GPU Thread
std::string MovieManager::GetInputDisplay()
{
  if (!IsMovieActive())
  {
    m_controllers = {};
    m_wiimotes = {};

    const auto& si = m_system.GetSerialInterface();
    for (int i = 0; i < 4; ++i)
    {
      if (si.GetDeviceType(i) == SerialInterface::SIDEVICE_GC_GBA_EMULATED)
        m_controllers[i] = ControllerType::GBA;
      else if (si.GetDeviceType(i) != SerialInterface::SIDEVICE_NONE)
        m_controllers[i] = ControllerType::GC;
      else
        m_controllers[i] = ControllerType::None;
      m_wiimotes[i] = Config::Get(Config::GetInfoForWiimoteSource(i)) != WiimoteSource::None;
    }
  }

  std::string input_display;
  {
    std::lock_guard guard(m_input_display_lock);
    for (int i = 0; i < 4; ++i)
    {
      if (IsUsingPad(i))
        input_display += m_input_display[i] + '\n';
    }
    for (int i = 0; i < 4; ++i)
    {
      if (IsUsingWiimote(i))
        input_display += m_input_display[i + 4] + '\n';
    }
  }
  return input_display;
}

// NOTE: GPU Thread
std::string MovieManager::GetInfoDisplay()
{
  std::string input_display;

  Memory::MemoryManager& memory = m_system.GetMemory();
  const std::string game_id = SConfig::GetInstance().m_debugger_game_id;
  std::string ini_content;
  const bool success =
      File::ReadFileToString(File::GetExeDirectory() + "\\InfoDisplay\\" + game_id + ".ini",
                             ini_content);

  if (!success)
    return GetInputDisplay();

  int line_counter = 0;
  bool in_progress = true;
  input_display.push_back('\n');

  while (in_progress)
  {
    ++line_counter;

    const std::string line_name = StringFromFormat("Line%i", line_counter);
    const std::string::size_type loc = ini_content.find(line_name, 0);
    if (loc == std::string::npos)
      break;

    ini_content = ini_content.substr(loc);
    ini_content = ini_content.substr(ini_content.find('"', 0) + 1);

    const std::string line = ini_content.substr(0, ini_content.find('"', 0));
    const std::string block_content = ini_content.substr(0, ini_content.find("End Line", 0));

    std::string::size_type loc_next = line.find('%', 0);
    std::string sub_line = line;
    int arg_counter = 0;

    while (loc_next != std::string::npos)
    {
      ++arg_counter;

      const std::string curr_section_output = sub_line.substr(0, loc_next);
      sub_line = sub_line.substr(loc_next + 1);

      int num_bytes = 0;
      std::string curr_identifier;

      if (sub_line.size() >= 3 && sub_line[2] == 'b')
      {
        curr_identifier = sub_line.substr(0, 3);
        num_bytes = atoi(curr_identifier.substr(1, 1).c_str());
        sub_line = sub_line.substr(3);
      }
      else
      {
        curr_identifier = sub_line.substr(0, 4);
        num_bytes = atoi(curr_identifier.substr(1, 2).c_str());
        sub_line = sub_line.substr(4);
      }

      const std::string identifier = "%" + curr_identifier.substr(0, 1);
      u32 read_address = 0;

      const std::string next_arg_name = StringFromFormat("Arg%i", arg_counter);
      const std::string::size_type loc_next_arg = block_content.find(next_arg_name, 0);
      if (loc_next_arg == std::string::npos)
        break;

      std::string arg_string = block_content.substr(loc_next_arg);
      arg_string = arg_string.substr(arg_string.find('=', 0) + 1);
      arg_string = arg_string.substr(0, arg_string.find(';', 0));

      const std::string::size_type loc_plus = arg_string.find('+', 0);
      std::string::size_type loc_hint = arg_string.find(">>", 0);
      std::string curr_hint;

      if (loc_hint != std::string::npos)
        curr_hint = arg_string.substr(loc_hint + 3);

      if (loc_plus == std::string::npos)
      {
        std::string argu_string =
            loc_hint != std::string::npos ? arg_string.substr(0, loc_hint - 1) : arg_string;
        read_address = strtoul(argu_string.c_str(), nullptr, 16);
      }
      else
      {
        const u32 pointer_address = strtoul(arg_string.substr(0, loc_plus - 1).c_str(), nullptr, 16);
        std::string argu_string = arg_string.substr(loc_plus + 2);

        if (loc_hint != std::string::npos)
        {
          loc_hint = argu_string.find(">>", 0);
          argu_string = argu_string.substr(0, loc_hint - 1);
        }

        const u32 offset = strtoul(argu_string.c_str(), nullptr, 16);
        u32 pointer = memory.Read_U32(pointer_address);

        if (pointer > 0x80000000)
        {
          pointer -= 0x80000000;
          read_address = pointer + offset;
        }
        else
        {
          input_display.append(curr_section_output + "N/A");
          loc_next = sub_line.find('%', 0);
          continue;
        }
      }

      std::string final_output;
      if (identifier == "%s")
      {
        const std::string output_string = memory.GetString(read_address, static_cast<size_t>(num_bytes));
        final_output = StringFromFormat(identifier.c_str(), output_string.c_str());
      }
      else if (identifier == "%f")
      {
        final_output =
            StringFromFormat(identifier.c_str(), std::bit_cast<float>(memory.Read_U32(read_address)));
      }
      else if (num_bytes == 4)
      {
        final_output = StringFromFormat(identifier.c_str(), memory.Read_U32(read_address));
      }
      else if (num_bytes == 2)
      {
        const u16 output = memory.Read_U16(read_address);
        if (curr_hint == "Degrees")
        {
          int final_degrees = static_cast<int>((static_cast<double>(output) / 182.04) + 0.5);
          if (final_degrees >= 360)
            final_degrees -= 360;
          final_output = StringFromFormat((identifier + " (%i DEG)").c_str(), output, final_degrees);
        }
        else if (curr_hint == "Time")
        {
          const int hours = output / 256;
          const int minutes = static_cast<int>((static_cast<float>(output) / 256.0f - hours) * 256.0f);
          std::stringstream ss;
          ss << minutes;
          std::string minute_string = ss.str();
          if (minutes < 10)
            minute_string = "0" + minute_string;
          final_output = StringFromFormat((identifier + ":%s").c_str(), hours, minute_string.c_str());
        }
        else
        {
          final_output = StringFromFormat(identifier.c_str(), output);
        }
      }
      else if (num_bytes == 1)
      {
        final_output = StringFromFormat(identifier.c_str(), memory.Read_U8(read_address));
      }

      if (final_output.empty())
      {
        final_output = "N/A";
      }
      else if (loc_hint == std::string::npos && (identifier == "%X" || identifier == "%x") &&
               final_output.length() < 2)
      {
        final_output = "0" + final_output;
      }

      input_display.append(curr_section_output + final_output);
      loc_next = sub_line.find('%', 0);
    }

    input_display.push_back('\n');
  }

  return input_display;
}

// NOTE: GPU Thread
std::string MovieManager::GetRTCDisplay() const
{
  using ExpansionInterface::CEXIIPL;

  // Cosmetic OSD path: in the brief window where determinism is set but the movie isn't active yet
  // (movie boot/end), GetEmulatedTime would assert, so skip the read for those frames.
  if (Core::WantsDeterminism() && !IsMovieActive())
    return "Date/Time: --";

  const time_t current_time = CEXIIPL::GetEmulatedTime(m_system, CEXIIPL::UNIX_EPOCH);
  const tm gm_time = fmt::gmtime(current_time);

  // Use current locale for formatting time, as fmt is locale-agnostic by default.
  return fmt::format(std::locale{""}, "Date/Time: {:%c}", gm_time);
}

// NOTE: GPU Thread
std::string MovieManager::GetRerecords() const
{
  if (IsMovieActive())
    return fmt::format("Rerecords: {}", m_rerecords);

  return "Rerecords: N/A";
}

std::optional<GCRuntimeFrameSnapshot> MovieManager::GetGCRuntimeFrameSnapshot() const
{
  if (!IsMovieActive() || !UsesOnlyGCPads(m_controllers, m_wiimotes))
    return std::nullopt;

  const std::vector<int> active_controllers = GetActiveGCControllerOrder(m_controllers);
  if (active_controllers.empty())
    return std::nullopt;

  GCRuntimeFrameSnapshot snapshot;
  for (const int controller : active_controllers)
  {
    snapshot.active_controllers[controller] = true;
    snapshot.active_gba_controllers[controller] = m_controllers[controller] == ControllerType::GBA;
  }

  snapshot.current_frame = m_current_frame;
  snapshot.is_recording = IsRecordingInput();
  snapshot.is_playing = IsPlayingInput();
  snapshot.is_read_only = m_read_only;

  const size_t stride = active_controllers.size() * sizeof(ControllerState);

  std::lock_guard guard(m_temp_input_lock);
  if (m_temp_input.empty())
    return snapshot;

  const size_t row_count = (m_temp_input.size() + stride - 1) / stride;
  snapshot.rows.resize(row_count);
  snapshot.row_game_frames = m_gc_runtime_row_frames;
  snapshot.row_game_frames.resize(row_count);
  if (stride > 0 && m_current_byte >= stride)
    snapshot.current_input_row = (m_current_byte / stride) - 1;
  if (snapshot.current_input_row >= row_count && row_count > 0)
    snapshot.current_input_row = row_count - 1;

  size_t offset = 0;
  for (size_t row = 0; row < row_count && offset < m_temp_input.size(); ++row)
  {
    for (const int controller : active_controllers)
    {
      if (offset + sizeof(ControllerState) > m_temp_input.size())
        break;

      std::memcpy(&snapshot.rows[row][controller], m_temp_input.data() + offset,
                  sizeof(ControllerState));
      offset += sizeof(ControllerState);
    }
  }

  return snapshot;
}

std::optional<GCRuntimeFrameMetadata> MovieManager::GetGCRuntimeFrameMetadata() const
{
  if (!IsMovieActive() || !UsesOnlyGCPads(m_controllers, m_wiimotes))
    return std::nullopt;

  const std::vector<int> active_controllers = GetActiveGCControllerOrder(m_controllers);
  if (active_controllers.empty())
    return std::nullopt;

  GCRuntimeFrameMetadata metadata;
  for (const int controller : active_controllers)
  {
    metadata.active_controllers[controller] = true;
    metadata.active_gba_controllers[controller] =
        m_controllers[controller] == ControllerType::GBA;
  }

  metadata.current_frame = m_current_frame;
  metadata.is_recording = IsRecordingInput();
  metadata.is_playing = IsPlayingInput();
  metadata.is_read_only = m_read_only;
  metadata.data_generation = m_runtime_data_generation.load();

  const size_t stride = active_controllers.size() * sizeof(ControllerState);

  std::lock_guard guard(m_temp_input_lock);
  if (m_temp_input.empty())
    return metadata;

  metadata.row_count = (m_temp_input.size() + stride - 1) / stride;
  if (stride > 0 && m_current_byte >= stride)
    metadata.current_input_row = (m_current_byte / stride) - 1;
  if (metadata.current_input_row >= metadata.row_count && metadata.row_count > 0)
    metadata.current_input_row = metadata.row_count - 1;

  return metadata;
}

std::optional<std::array<ControllerState, 4>> MovieManager::GetGCRuntimeFrameRow(const u64 row) const
{
  if (!IsMovieActive() || !UsesOnlyGCPads(m_controllers, m_wiimotes))
    return std::nullopt;

  const std::vector<int> active_controllers = GetActiveGCControllerOrder(m_controllers);
  if (active_controllers.empty())
    return std::nullopt;

  const size_t stride = active_controllers.size() * sizeof(ControllerState);
  if (stride == 0)
    return std::nullopt;

  std::array<ControllerState, 4> row_data{};

  std::lock_guard guard(m_temp_input_lock);
  const size_t row_offset = static_cast<size_t>(row) * stride;
  if (row_offset >= m_temp_input.size())
    return std::nullopt;

  size_t offset = row_offset;
  for (const int controller : active_controllers)
  {
    if (offset + sizeof(ControllerState) > m_temp_input.size())
      break;

    std::memcpy(&row_data[static_cast<size_t>(controller)], m_temp_input.data() + offset,
                sizeof(ControllerState));
    offset += sizeof(ControllerState);
  }

  return row_data;
}

bool MovieManager::SetGCRuntimeFrameState(u64 frame, int controller, const ControllerState& state)
{
  if (!IsMovieActive() || !UsesOnlyGCPads(m_controllers, m_wiimotes) || controller < 0 ||
      controller >= 4 ||
      (m_controllers[controller] != ControllerType::GC &&
       m_controllers[controller] != ControllerType::GBA))
  {
    return false;
  }

  const std::vector<int> active_controllers = GetActiveGCControllerOrder(m_controllers);
  const auto it = std::ranges::find(active_controllers, controller);
  if (it == active_controllers.end())
    return false;

  const size_t active_index = static_cast<size_t>(std::distance(active_controllers.begin(), it));
  const size_t stride = active_controllers.size() * sizeof(ControllerState);
  const size_t offset = static_cast<size_t>(frame) * stride + active_index * sizeof(ControllerState);

  std::lock_guard guard(m_temp_input_lock);
  if (offset + sizeof(ControllerState) > m_temp_input.size())
    return false;

  std::memcpy(m_temp_input.data() + offset, &state, sizeof(ControllerState));
  ++m_runtime_data_generation;
  return true;
}

std::optional<WiiRuntimeFrameSnapshot> MovieManager::GetWiiRuntimeFrameSnapshot() const
{
  const auto active_wiimote = GetSingleActiveWiimote(m_controllers, m_wiimotes);
  if (!IsMovieActive() || !active_wiimote.has_value())
    return std::nullopt;

  WiiRuntimeFrameSnapshot snapshot;
  snapshot.active_wiimotes[*active_wiimote] = true;
  snapshot.current_frame = m_current_frame;
  snapshot.is_recording = IsRecordingInput();
  snapshot.is_playing = IsPlayingInput();
  snapshot.is_read_only = m_read_only;

  std::lock_guard guard(m_temp_input_lock);
  if (m_temp_input.empty())
    return snapshot;

  const auto parsed_rows = ParseSingleWiimoteRows(m_temp_input, *active_wiimote);
  if (!parsed_rows.has_value())
    return std::nullopt;

  snapshot.rows = *parsed_rows;
  snapshot.row_game_frames = m_wii_runtime_row_frames;
  snapshot.row_game_frames.resize(snapshot.rows.size());

  size_t offset = 0;
  for (size_t row_index = 0; row_index < snapshot.rows.size(); ++row_index)
  {
    offset += 1 + snapshot.rows[row_index].serialized_state.length;
    if (m_current_byte >= offset)
      snapshot.current_input_row = row_index;
  }

  return snapshot;
}

std::optional<WiiRuntimeFrameMetadata> MovieManager::GetWiiRuntimeFrameMetadata() const
{
  const auto active_wiimote = GetSingleActiveWiimote(m_controllers, m_wiimotes);
  if (!IsMovieActive() || !active_wiimote.has_value())
    return std::nullopt;

  WiiRuntimeFrameMetadata metadata;
  metadata.active_wiimotes[*active_wiimote] = true;
  metadata.current_frame = m_current_frame;
  metadata.is_recording = IsRecordingInput();
  metadata.is_playing = IsPlayingInput();
  metadata.is_read_only = m_read_only;
  metadata.data_generation = m_runtime_data_generation.load();

  std::lock_guard guard(m_temp_input_lock);
  if (m_temp_input.empty())
    return metadata;

  size_t offset = 0;
  u64 row_index = 0;
  while (offset < m_temp_input.size())
  {
    const u8 length = m_temp_input[offset++];
    if (length > WiimoteEmu::SerializedWiimoteState{}.data.size() ||
        offset + length > m_temp_input.size())
    {
      break;
    }

    offset += length;
    if (m_current_byte >= offset)
      metadata.current_input_row = row_index;
    ++row_index;
  }

  metadata.row_count = row_index;
  if (metadata.current_input_row >= metadata.row_count && metadata.row_count > 0)
    metadata.current_input_row = metadata.row_count - 1;

  return metadata;
}

std::optional<WiiRuntimeInputRow> MovieManager::GetWiiRuntimeFrameRow(const u64 row) const
{
  const auto active_wiimote = GetSingleActiveWiimote(m_controllers, m_wiimotes);
  if (!IsMovieActive() || !active_wiimote.has_value())
    return std::nullopt;

  std::lock_guard guard(m_temp_input_lock);
  size_t offset = 0;
  u64 row_index = 0;
  while (offset < m_temp_input.size())
  {
    const u8 length = m_temp_input[offset++];
    if (length > WiimoteEmu::SerializedWiimoteState{}.data.size() ||
        offset + length > m_temp_input.size())
    {
      return std::nullopt;
    }

    if (row_index == row)
    {
      WiiRuntimeInputRow row_data;
      row_data.wiimote = *active_wiimote;
      row_data.is_reset = length == 0;
      row_data.serialized_state.length = length;
      if (length > 0)
      {
        std::copy_n(m_temp_input.data() + offset, length, row_data.serialized_state.data.data());
      }
      return row_data;
    }

    offset += length;
    ++row_index;
  }

  return std::nullopt;
}

bool MovieManager::SetWiiRuntimeFrameState(u64 row,
                                           const WiimoteEmu::SerializedWiimoteState& serialized_state)
{
  const auto active_wiimote = GetSingleActiveWiimote(m_controllers, m_wiimotes);
  if (!IsMovieActive() || !active_wiimote.has_value() ||
      serialized_state.length > serialized_state.data.size())
  {
    return false;
  }

  std::lock_guard guard(m_temp_input_lock);
  const auto parsed_rows = ParseSingleWiimoteRows(m_temp_input, *active_wiimote);
  if (!parsed_rows.has_value() || row >= parsed_rows->size())
    return false;

  auto rows = *parsed_rows;
  size_t current_row_index = 0;
  size_t offset = 0;
  for (size_t row_index = 0; row_index < rows.size(); ++row_index)
  {
    offset += 1 + rows[row_index].serialized_state.length;
    if (m_current_byte >= offset)
    {
      current_row_index = row_index;
    }
  }

  rows[static_cast<size_t>(row)].serialized_state = serialized_state;
  rows[static_cast<size_t>(row)].is_reset = serialized_state.length == 0;

  std::vector<u8> rebuilt_input;
  rebuilt_input.reserve(m_temp_input.size());
  for (const WiiRuntimeInputRow& input_row : rows)
  {
    rebuilt_input.push_back(input_row.is_reset ? 0 : input_row.serialized_state.length);
    if (!input_row.is_reset && input_row.serialized_state.length > 0)
    {
      rebuilt_input.insert(rebuilt_input.end(), input_row.serialized_state.data.begin(),
                           input_row.serialized_state.data.begin() + input_row.serialized_state.length);
    }
  }

  m_temp_input = std::move(rebuilt_input);
  ++m_runtime_data_generation;

  if (!rows.empty() && row <= current_row_index)
  {
    size_t new_row_end_offset = 0;
    const size_t clamped_row = std::min(current_row_index, rows.size() - 1);
    for (size_t row_index = 0; row_index <= clamped_row; ++row_index)
      new_row_end_offset += 1 + rows[row_index].serialized_state.length;

    m_current_byte = static_cast<u64>(new_row_end_offset);
  }
  else if (m_current_byte > m_temp_input.size())
  {
    m_current_byte = static_cast<u64>(m_temp_input.size());
  }

  return true;
}

std::optional<GCPadStatus> MovieManager::GetDisplayedPadStatus(int controller) const
{
  if (controller < 0 || controller >= static_cast<int>(m_displayed_pad_status.size()))
    return std::nullopt;

  std::lock_guard guard(m_live_input_lock);
  return m_displayed_pad_status[controller];
}

std::optional<WiimoteEmu::DesiredWiimoteState> MovieManager::GetDisplayedWiimoteState(
    int wiimote) const
{
  if (wiimote < 0 || wiimote >= static_cast<int>(m_displayed_wiimote_state.size()))
    return std::nullopt;

  std::lock_guard guard(m_live_input_lock);
  if (!m_displayed_wiimote_state[wiimote].has_value())
    return std::nullopt;

  WiimoteEmu::DesiredWiimoteState desired_state;
  if (!WiimoteEmu::DeserializeDesiredState(&desired_state, *m_displayed_wiimote_state[wiimote]))
    return std::nullopt;

  return desired_state;
}

void MovieManager::UpdateDisplayedPadStatus(int controller, const GCPadStatus& status)
{
  if (controller < 0 || controller >= static_cast<int>(m_displayed_pad_status.size()))
    return;

  std::lock_guard guard(m_live_input_lock);
  m_displayed_pad_status[controller] = status;
}

void MovieManager::UpdateDisplayedWiimoteState(int wiimote,
                                               const DesiredWiimoteState& desired_state)
{
  if (wiimote < 0 || wiimote >= static_cast<int>(m_displayed_wiimote_state.size()))
    return;

  std::lock_guard guard(m_live_input_lock);
  m_displayed_wiimote_state[wiimote] = WiimoteEmu::SerializeDesiredState(desired_state);
}

void MovieManager::FrameUpdate()
{
  m_current_frame++;
  if (!m_polled)
    m_current_lag_count++;

  if (IsRecordingInput())
  {
    m_total_frames = m_current_frame;
    m_total_lag_count = m_current_lag_count;
  }

  m_polled = false;
}

// called when game is booting up, even if no movie is active,
// but potentially after BeginRecordingInput or PlayInput has been called.
// NOTE: EmuThread
void MovieManager::Init(const BootParameters& boot)
{
  if (std::holds_alternative<BootParameters::Disc>(boot.parameters))
    m_current_file_name = std::get<BootParameters::Disc>(boot.parameters).path;
  else
    m_current_file_name.clear();

  m_polled = false;
  m_save_config = false;
  if (IsPlayingInput())
  {
    ReadHeader();
    std::thread md5thread(&MovieManager::CheckMD5, this);
    md5thread.detach();
    if (strncmp(m_temp_header.gameID.data(), SConfig::GetInstance().GetGameID().c_str(), 6))
    {
      PanicAlertFmtT("The recorded game ({0}) is not the same as the selected game ({1})",
                     m_temp_header.GetGameID(), SConfig::GetInstance().GetGameID());
      EndPlayInput(false);
    }
  }

  if (IsRecordingInput())
  {
    GetSettings();
    std::thread md5thread(&MovieManager::GetMD5, this);
    md5thread.detach();
    m_tick_count_at_last_input = 0;
  }

  memset(&m_pad_state, 0, sizeof(m_pad_state));

  for (auto& disp : m_input_display)
    disp.clear();

  {
    std::lock_guard guard(m_live_input_lock);
    m_displayed_pad_status.fill(std::nullopt);
    m_displayed_wiimote_state.fill(std::nullopt);
  }

  if (!IsMovieActive())
  {
    m_recording_from_save_state = false;
    m_rerecords = 0;
    m_current_byte = 0;
    m_current_frame = 0;
    m_current_lag_count = 0;
    m_current_input_count = 0;
    m_gc_runtime_row_frames.clear();
    m_wii_runtime_row_frames.clear();
  }
}

// NOTE: CPU Thread
void MovieManager::InputUpdate()
{
  m_current_input_count++;

  if (!IsRecordingInput())
    return;

  const auto& core_timing = m_system.GetCoreTiming();
  m_total_input_count = m_current_input_count;
  m_total_tick_count += core_timing.GetTicks() - m_tick_count_at_last_input;
  m_tick_count_at_last_input = core_timing.GetTicks();
}

// NOTE: CPU Thread
void MovieManager::SetPolledDevice()
{
  m_polled = true;
  m_poll_count.fetch_add(1, std::memory_order_relaxed);
}

// NOTE: Host Thread
void MovieManager::SetReadOnly(bool bEnabled)
{
  if (m_read_only != bEnabled)
    Core::DisplayMessage(bEnabled ? "Read-only mode." : "Read+Write mode.", 1000);

  m_read_only = bEnabled;
}

bool MovieManager::IsRecordingInput() const
{
  return (m_play_mode == PlayMode::Recording);
}

bool MovieManager::IsRecordingInputFromSaveState() const
{
  return m_recording_from_save_state;
}

bool MovieManager::IsJustStartingRecordingInputFromSaveState() const
{
  return IsRecordingInputFromSaveState() && m_current_frame == 0;
}

bool MovieManager::IsJustStartingPlayingInputFromSaveState() const
{
  return IsRecordingInputFromSaveState() && m_current_frame == 1 && IsPlayingInput();
}

bool MovieManager::IsPlayingInput() const
{
  return (m_play_mode == PlayMode::Playing);
}

bool MovieManager::IsMovieActive() const
{
  return m_play_mode != PlayMode::None;
}

bool MovieManager::IsReadOnly() const
{
  return m_read_only;
}

u64 MovieManager::GetRecordingStartTime() const
{
  return m_recording_start_time;
}

u64 MovieManager::GetCurrentFrame() const
{
  return m_current_frame;
}

u64 MovieManager::GetPollCount() const
{
  return m_poll_count.load(std::memory_order_relaxed);
}

u64 MovieManager::GetTotalFrames() const
{
  return m_total_frames;
}

u64 MovieManager::GetCurrentInputCount() const
{
  return m_current_input_count;
}

u64 MovieManager::GetTotalInputCount() const
{
  return m_total_input_count;
}

u64 MovieManager::GetCurrentLagCount() const
{
  return m_current_lag_count;
}

u64 MovieManager::GetTotalLagCount() const
{
  return m_total_lag_count;
}

void MovieManager::SetClearSave(bool enabled)
{
  m_clear_save = enabled;
}

void MovieManager::SignalDiscChange(const std::string& new_path)
{
  if (IsRecordingInput())
  {
    size_t size_of_path_without_filename = new_path.find_last_of("/\\") + 1;
    std::string filename = new_path.substr(size_of_path_without_filename);
    constexpr size_t maximum_length = sizeof(DTMHeader::discChange);
    if (filename.length() > maximum_length)
    {
      PanicAlertFmtT("The disc change to \"{0}\" could not be saved in the .dtm file.\n"
                     "The filename of the disc image must not be longer than 40 characters.",
                     filename);
    }
    m_disc_change_filename = filename;
    m_has_disc_change = true;
  }
}

void MovieManager::SetReset(bool reset)
{
  m_reset = reset;
}

bool MovieManager::IsUsingPad(int controller) const
{
  return m_controllers[controller] != ControllerType::None;
}

bool MovieManager::IsUsingBongo(int controller) const
{
  return ((m_bongos & (1 << controller)) != 0);
}

bool MovieManager::IsUsingGBA(int controller) const
{
  return m_controllers[controller] == ControllerType::GBA;
}

bool MovieManager::IsUsingWiimote(int wiimote) const
{
  return m_wiimotes[wiimote];
}

bool MovieManager::IsConfigSaved() const
{
  return m_save_config;
}

bool MovieManager::IsStartingFromClearSave() const
{
  return m_clear_save;
}

bool MovieManager::IsUsingMemcard(ExpansionInterface::Slot slot) const
{
  switch (slot)
  {
  case ExpansionInterface::Slot::A:
    return (m_memcards & 1) != 0;
  case ExpansionInterface::Slot::B:
    return (m_memcards & 2) != 0;
  default:
    return false;
  }
}

bool MovieManager::IsNetPlayRecording() const
{
  return m_net_play;
}

// NOTE: Host Thread
void MovieManager::ChangePads()
{
  if (!Core::IsRunning(m_system))
    return;

  ControllerTypeArray controllers{};

  for (int i = 0; i < SerialInterface::MAX_SI_CHANNELS; ++i)
  {
    const SerialInterface::SIDevices si_device = Config::Get(Config::GetInfoForSIDevice(i));
    if (si_device == SerialInterface::SIDEVICE_GC_GBA_EMULATED)
      controllers[i] = ControllerType::GBA;
    else if (SerialInterface::SIDevice_IsGCController(si_device))
      controllers[i] = ControllerType::GC;
    else
      controllers[i] = ControllerType::None;
  }

  if (m_controllers == controllers)
  {
    ApplyGBADisconnectIfPending();
    return;
  }

  auto& si = m_system.GetSerialInterface();
  for (int i = 0; i < SerialInterface::MAX_SI_CHANNELS; ++i)
  {
    SerialInterface::SIDevices device = SerialInterface::SIDEVICE_NONE;
    if (IsUsingGBA(i))
    {
      device = SerialInterface::SIDEVICE_GC_GBA_EMULATED;
    }
    else if (IsUsingPad(i))
    {
      const SerialInterface::SIDevices si_device = Config::Get(Config::GetInfoForSIDevice(i));
      if (SerialInterface::SIDevice_IsGCController(si_device))
      {
        device = si_device;
      }
      else
      {
        device = IsUsingBongo(i) ? SerialInterface::SIDEVICE_GC_TARUKONGA :
                                   SerialInterface::SIDEVICE_GC_CONTROLLER;
      }
    }

    si.ChangeDevice(device, i);
  }

  ApplyGBADisconnectIfPending();
}

// NOTE: Host / Emu Threads
void MovieManager::ChangeWiiPads(bool instantly)
{
  WiimoteEnabledArray wiimotes{};

  for (int i = 0; i < MAX_WIIMOTES; ++i)
  {
    wiimotes[i] = Config::Get(Config::GetInfoForWiimoteSource(i)) != WiimoteSource::None;
  }

  // This is important for Wiimotes, because they can desync easily if they get re-activated
  if (instantly && m_wiimotes == wiimotes)
    return;

  const auto bt = WiiUtils::GetBluetoothEmuDevice();
  for (int i = 0; i < MAX_WIIMOTES; ++i)
  {
    const bool is_using_wiimote = IsUsingWiimote(i);

    Config::SetCurrent(Config::GetInfoForWiimoteSource(i),
                       is_using_wiimote ? WiimoteSource::Emulated : WiimoteSource::None);
    if (bt != nullptr)
      bt->AccessWiimoteByIndex(i)->Activate(is_using_wiimote);
  }
}

// NOTE: Host Thread
bool MovieManager::BeginRecordingInput(const ControllerTypeArray& controllers,
                                       const WiimoteEnabledArray& wiimotes)
{
  if (m_play_mode != PlayMode::None ||
      (controllers == ControllerTypeArray{} && wiimotes == WiimoteEnabledArray{}))
    return false;

  Core::DisplayMessage("Starting movie recording", 2000);

  Core::CPUThreadGuard cpu_thread_guard{m_system};

  m_controllers = controllers;
  m_wiimotes = wiimotes;
  m_current_frame = m_total_frames = 0;
  m_current_lag_count = m_total_lag_count = 0;
  m_current_input_count = m_total_input_count = 0;
  m_total_tick_count = m_tick_count_at_last_input = 0;
  m_bongos = 0;
  m_memcards = 0;
  if (NetPlay::IsNetPlayRunning())
  {
    m_net_play = true;
    m_recording_start_time = ExpansionInterface::CEXIIPL::NetPlay_GetEmulatedTime();
  }
  else if (Config::Get(Config::MAIN_CUSTOM_RTC_ENABLE))
  {
    m_recording_start_time = Config::Get(Config::MAIN_CUSTOM_RTC_VALUE);
  }
  else
  {
    m_recording_start_time = Common::Timer::GetLocalTimeSinceJan1970();
  }

  m_rerecords = 0;

  for (int i = 0; i < SerialInterface::MAX_SI_CHANNELS; ++i)
  {
    const SerialInterface::SIDevices si_device = Config::Get(Config::GetInfoForSIDevice(i));
    if (si_device == SerialInterface::SIDEVICE_GC_TARUKONGA)
      m_bongos |= (1 << i);
  }

  if (Core::IsRunning(m_system))
  {
    const std::string save_path = File::GetUserPath(D_STATESAVES_IDX) + "dtm.sav";
    if (File::Exists(save_path))
      File::Delete(save_path);

    State::SaveAs(m_system, save_path);
    m_recording_from_save_state = true;

    std::thread md5thread(&MovieManager::GetMD5, this);
    md5thread.detach();
    GetSettings();
  }

  // Wiimotes cause desync issues if they're not reset before launching the game
  if (!Core::IsRunning(m_system))
  {
    // This will also reset the Wiimotes for GameCube games, but that shouldn't do anything
    ::Wiimote::ResetAllWiimotes();
  }

  m_play_mode = PlayMode::Recording;
  m_author = Config::Get(Config::MAIN_MOVIE_MOVIE_AUTHOR);
  {
    std::lock_guard guard(m_temp_input_lock);
    m_temp_input.clear();
    m_gc_runtime_row_frames.clear();
    m_current_byte = 0;
  }

  // This is a bit of a hack, SYSCONF movie code expects the movie layer active for both recording
  // and playback. That layer is really only designed for playback, not recording. Also, we can't
  // know if we're using a Wii at this point. So, we'll assume a Wii is used here. In practice,
  // this shouldn't affect anything for GC (as its only unique setting is language, which will be
  // taken from base settings as expected)
  static DTMHeader header = {.bWii = true};
  ConfigLoaders::SaveToDTM(&header);
  Config::AddLayer(ConfigLoaders::GenerateMovieConfigLoader(&header));

  if (Core::IsRunning(m_system))
    Core::UpdateWantDeterminism(m_system);

  return true;
}

static std::string Analog2DToString(u32 x, u32 y, const std::string& prefix, u32 range = 255)
{
  const u32 center = range / 2 + 1;

  if ((x <= 1 || x == center || x >= range) && (y <= 1 || y == center || y >= range))
  {
    if (x != center || y != center)
    {
      if (x != center && y != center)
      {
        return fmt::format("{}:{},{}", prefix, x < center ? "LEFT" : "RIGHT",
                           y < center ? "DOWN" : "UP");
      }

      if (x != center)
      {
        return fmt::format("{}:{}", prefix, x < center ? "LEFT" : "RIGHT");
      }

      return fmt::format("{}:{}", prefix, y < center ? "DOWN" : "UP");
    }

    return "";
  }

  return fmt::format("{}:{},{}", prefix, x, y);
}

static std::string Analog1DToString(u32 v, const std::string& prefix, u32 range = 255)
{
  if (v == 0)
    return "";

  if (v == range)
    return prefix;

  return fmt::format("{}:{}", prefix, v);
}

// NOTE: CPU Thread
static std::string GenerateInputDisplayString(ControllerState padState, int controllerID)
{
  std::string display_str = fmt::format("P{}:", controllerID + 1);

  if (padState.is_connected)
  {
    if (padState.A)
      display_str += " A";
    if (padState.B)
      display_str += " B";
    if (padState.X)
      display_str += " X";
    if (padState.Y)
      display_str += " Y";
    if (padState.Z)
      display_str += " Z";
    if (padState.Start)
      display_str += " START";

    if (padState.DPadUp)
      display_str += " UP";
    if (padState.DPadDown)
      display_str += " DOWN";
    if (padState.DPadLeft)
      display_str += " LEFT";
    if (padState.DPadRight)
      display_str += " RIGHT";
    if (padState.reset)
      display_str += " RESET";

    if (padState.TriggerL == 255 || padState.L)
      display_str += " L";
    else
      display_str += Analog1DToString(padState.TriggerL, " L");

    if (padState.TriggerR == 255 || padState.R)
      display_str += " R";
    else
      display_str += Analog1DToString(padState.TriggerR, " R");

    display_str += Analog2DToString(padState.AnalogStickX, padState.AnalogStickY, " ANA");
    display_str += Analog2DToString(padState.CStickX, padState.CStickY, " C");
  }
  else
  {
    display_str += " DISCONNECTED";
  }

  return display_str;
}

// NOTE: CPU Thread
static std::string GenerateWiiInputDisplayString(int index, const DesiredWiimoteState& state)
{
  std::string display_str = fmt::format("R{}:", index + 1);

  const auto& buttons = state.buttons;
  if (buttons.hex & WiimoteCommon::ButtonData::BUTTON_MASK)
  {
    if (buttons.left)
      display_str += " LEFT";
    if (buttons.right)
      display_str += " RIGHT";
    if (buttons.down)
      display_str += " DOWN";
    if (buttons.up)
      display_str += " UP";
    if (buttons.a)
      display_str += " A";
    if (buttons.b)
      display_str += " B";
    if (buttons.plus)
      display_str += " +";
    if (buttons.minus)
      display_str += " -";
    if (buttons.one)
      display_str += " 1";
    if (buttons.two)
      display_str += " 2";
    if (buttons.home)
      display_str += " HOME";
  }

  if (state.acceleration != state.DEFAULT_ACCELERATION)
  {
    const AccelData& accel_data = state.acceleration;
    display_str +=
        fmt::format(" ACC:{},{},{}", accel_data.value.x, accel_data.value.y, accel_data.value.z);
  }

  if (state.camera_points != state.DEFAULT_CAMERA)
  {
    display_str += " IR:";
    for (auto& point : state.camera_points)
    {
      if (point.size == 0xff)
        display_str += "_,";
      else
        display_str += fmt::format("{},{},", point.position.x, point.position.y);
    }
    display_str.pop_back();
  }

  if (state.extension.data.index() != ExtensionNumber::NONE)
  {
    const auto ext_visitor = overloaded{
        [&](const Nunchuk::DesiredState& nunchuk) {
          const auto bt = nunchuk.GetButtons();
          if (bt & Nunchuk::BUTTON_C)
            display_str += " C";
          if (bt & Nunchuk::BUTTON_Z)
            display_str += " Z";
          display_str += fmt::format(" N-ACC:{},{},{}", nunchuk.GetAccelX(), nunchuk.GetAccelY(),
                                     nunchuk.GetAccelZ());
          display_str += Analog2DToString(nunchuk.jx, nunchuk.jy, " ANA");
        },
        [&](const Classic::DesiredState& cc) {
          const auto bt = cc.GetButtons();
          constexpr std::pair<u16, const char*> named_buttons[] = {
              {Classic::PAD_LEFT, "LEFT"},    {Classic::PAD_RIGHT, "RIGHT"},
              {Classic::PAD_DOWN, "DOWN"},    {Classic::PAD_UP, "UP"},
              {Classic::BUTTON_A, "A"},       {Classic::BUTTON_B, "B"},
              {Classic::BUTTON_X, "X"},       {Classic::BUTTON_Y, "Y"},
              {Classic::BUTTON_ZL, "ZL"},     {Classic::BUTTON_ZR, "ZR"},
              {Classic::BUTTON_PLUS, "+"},    {Classic::BUTTON_MINUS, "-"},
              {Classic::BUTTON_HOME, "HOME"},
          };
          for (auto& [value, name] : named_buttons)
          {
            if (bt & value)
            {
              display_str += ' ';
              display_str += name;
            }
          }
          constexpr auto trigger_max = (1 << Classic::TRIGGER_BITS) - 1;
          display_str += Analog1DToString(cc.GetLeftTrigger().value, " L", trigger_max);
          display_str += Analog1DToString(cc.GetRightTrigger().value, " R", trigger_max);

          constexpr auto lstick_max = (1 << Classic::LEFT_STICK_BITS) - 1;
          const auto left_stick = cc.GetLeftStick().value;
          display_str += Analog2DToString(left_stick.x, left_stick.y, " ANA", lstick_max);

          constexpr auto rstick_max = (1 << Classic::RIGHT_STICK_BITS) - 1;
          const auto right_stick = cc.GetRightStick().value;
          display_str += Analog2DToString(right_stick.x, right_stick.y, " R-ANA", rstick_max);
        },
        [&](const Guitar::DesiredState&) { display_str += " Guitar"; },
        [&](const Drums::DesiredState&) { display_str += " Drums"; },
        [&](const Turntable::DesiredState&) { display_str += " Turntable"; },
        [&](const UDrawTablet::DesiredState&) { display_str += " UDraw"; },
        [&](const DrawsomeTablet::DesiredState&) { display_str += " Drawsome"; },
        [&](const TaTaCon::DesiredState&) { display_str += " TaTaCon"; },
        [&](const Shinkansen::DesiredState&) { display_str += " Shinkansen"; },
        [](const auto& arg) {
          static_assert(std::is_same_v<std::monostate, std::decay_t<decltype(arg)>>,
                        "unimplemented extension");
        },
    };
    std::visit(ext_visitor, state.extension.data);
  }

  return display_str;
}

// NOTE: CPU Thread
void MovieManager::CheckPadStatus(const GCPadStatus* PadStatus, int controllerID)
{
  m_pad_state.A = ((PadStatus->button & PAD_BUTTON_A) != 0);
  m_pad_state.B = ((PadStatus->button & PAD_BUTTON_B) != 0);
  m_pad_state.X = ((PadStatus->button & PAD_BUTTON_X) != 0);
  m_pad_state.Y = ((PadStatus->button & PAD_BUTTON_Y) != 0);
  m_pad_state.Z = ((PadStatus->button & PAD_TRIGGER_Z) != 0);
  m_pad_state.Start = ((PadStatus->button & PAD_BUTTON_START) != 0);

  m_pad_state.DPadUp = ((PadStatus->button & PAD_BUTTON_UP) != 0);
  m_pad_state.DPadDown = ((PadStatus->button & PAD_BUTTON_DOWN) != 0);
  m_pad_state.DPadLeft = ((PadStatus->button & PAD_BUTTON_LEFT) != 0);
  m_pad_state.DPadRight = ((PadStatus->button & PAD_BUTTON_RIGHT) != 0);

  m_pad_state.L = ((PadStatus->button & PAD_TRIGGER_L) != 0);
  m_pad_state.R = ((PadStatus->button & PAD_TRIGGER_R) != 0);
  m_pad_state.TriggerL = PadStatus->triggerLeft;
  m_pad_state.TriggerR = PadStatus->triggerRight;

  m_pad_state.AnalogStickX = PadStatus->stickX;
  m_pad_state.AnalogStickY = PadStatus->stickY;

  m_pad_state.CStickX = PadStatus->substickX;
  m_pad_state.CStickY = PadStatus->substickY;

  m_pad_state.is_connected = PadStatus->isConnected;

  m_pad_state.get_origin = (PadStatus->button & PAD_GET_ORIGIN) != 0;

  m_pad_state.disc = m_has_disc_change;
  m_has_disc_change = false;
  m_pad_state.reset = m_reset;
  m_reset = false;

  {
    std::string display_str = GenerateInputDisplayString(m_pad_state, controllerID);

    std::lock_guard guard(m_input_display_lock);
    m_input_display[controllerID] = std::move(display_str);
  }

  UpdateDisplayedPadStatus(controllerID, *PadStatus);
}

// NOTE: CPU Thread
void MovieManager::RecordInput(const GCPadStatus* PadStatus, int controllerID)
{
  if (!IsRecordingInput() || !IsUsingPad(controllerID))
    return;

  CheckPadStatus(PadStatus, controllerID);

  std::lock_guard guard(m_temp_input_lock);
  const auto active_controllers = GetActiveGCControllerOrder(m_controllers);
  const size_t stride = active_controllers.size() * sizeof(ControllerState);
  if (stride > 0)
  {
    const size_t row_index = static_cast<size_t>(m_current_byte / stride);
    if (m_gc_runtime_row_frames.size() <= row_index)
      m_gc_runtime_row_frames.resize(row_index + 1);
    m_gc_runtime_row_frames[row_index] = m_current_frame;
  }
  m_temp_input.resize(m_current_byte + sizeof(ControllerState));
  memcpy(&m_temp_input[m_current_byte], &m_pad_state, sizeof(ControllerState));
  m_current_byte += sizeof(ControllerState);
}

// NOTE: CPU Thread
void MovieManager::CheckWiimoteStatus(int wiimote, const DesiredWiimoteState& desired_state)
{
  SetPolledDevice();

  {
    std::string display_str = GenerateWiiInputDisplayString(wiimote, desired_state);

    std::lock_guard guard(m_input_display_lock);
    m_input_display[wiimote + 4] = std::move(display_str);
  }

  UpdateDisplayedWiimoteState(wiimote, desired_state);

  if (IsRecordingInput())
    RecordWiimote(wiimote, SerializeDesiredState(desired_state));
}

void MovieManager::RecordWiimote(int wiimote, const SerializedWiimoteState& serialized_state)
{
  if (!IsRecordingInput() || !IsUsingWiimote(wiimote))
    return;

  InputUpdate();

  std::lock_guard guard(m_temp_input_lock);
  if (m_reset)
  {
    const size_t row_index = m_wii_runtime_row_frames.size();
    m_wii_runtime_row_frames.resize(row_index + 1);
    m_wii_runtime_row_frames[row_index] = m_current_frame;
    // Use a zero-length marker to record console resets in Wiimote movies.
    m_temp_input.resize(m_current_byte + 1);
    m_temp_input[m_current_byte++] = 0;
    m_reset = false;
  }

  const size_t row_index = m_wii_runtime_row_frames.size();
  m_wii_runtime_row_frames.resize(row_index + 1);
  m_wii_runtime_row_frames[row_index] = m_current_frame;
  const u8 size = serialized_state.length;
  m_temp_input.resize(m_current_byte + size + 1);
  m_temp_input[m_current_byte++] = size;
  std::copy_n(serialized_state.data.data(), size, m_temp_input.data() + m_current_byte);
  m_current_byte += size;
}

// NOTE: EmuThread / Host Thread
void MovieManager::ReadHeader()
{
  m_temp_header.controllers |= m_temp_header.GBAControllers;

  for (int i = 0; i < 4; ++i)
  {
    if (m_temp_header.GBAControllers & (1 << i))
      m_controllers[i] = ControllerType::GBA;
    else if (m_temp_header.controllers & (1 << i))
      m_controllers[i] = ControllerType::GC;
    else
      m_controllers[i] = ControllerType::None;
    m_wiimotes[i] = (m_temp_header.controllers & (1 << (i + 4))) != 0;
  }
  m_recording_start_time = m_temp_header.recordingStartTime;
  if (m_rerecords < m_temp_header.numRerecords)
    m_rerecords = m_temp_header.numRerecords;

  if (m_temp_header.bSaveConfig)
  {
    m_save_config = true;
    Config::AddLayer(ConfigLoaders::GenerateMovieConfigLoader(&m_temp_header));
    m_clear_save = m_temp_header.bClearSave;
    m_memcards = m_temp_header.memcards;
    m_bongos = m_temp_header.bongos;
    m_net_play = m_temp_header.bNetPlay;
    m_revision = m_temp_header.revision;
  }
  else
  {
    GetSettings();
  }

  m_disc_change_filename = {m_temp_header.discChange.begin(), m_temp_header.discChange.end()};
  m_author = {m_temp_header.author.begin(), m_temp_header.author.end()};
  m_md5 = m_temp_header.md5;
  m_dsp_irom_hash = m_temp_header.DSPiromHash;
  m_dsp_coef_hash = m_temp_header.DSPcoefHash;
  ApplyWiimoteBatteryFromHeader(m_temp_header);
  ApplyGBADisconnectFromHeader(m_temp_header);
}

// NOTE: Host Thread
bool MovieManager::PlayInput(const std::string& movie_path,
                             std::optional<std::string>* savestate_path)
{
  if (m_play_mode != PlayMode::None)
    return false;

  File::IOFile recording_file(movie_path, "rb");
  if (!recording_file.ReadArray(&m_temp_header, 1))
    return false;

  if (!IsMovieHeader(m_temp_header.filetype))
  {
    PanicAlertFmtT("Invalid recording file");
    return false;
  }

  ReadHeader();

  if (AchievementManager::GetInstance().IsHardcoreModeActive())
    return false;

  m_total_frames = m_temp_header.frameCount;
  m_total_lag_count = m_temp_header.lagCount;
  m_total_input_count = m_temp_header.inputCount;
  m_total_tick_count = m_temp_header.tickCount;
  m_current_frame = 0;
  m_current_lag_count = 0;
  m_current_input_count = 0;

  m_play_mode = PlayMode::Playing;

  // Wiimotes cause desync issues if they're not reset before launching the game
  ::Wiimote::ResetAllWiimotes();

  Core::UpdateWantDeterminism(m_system);

  {
    std::lock_guard guard(m_temp_input_lock);
    m_temp_input.resize(recording_file.GetSize() - 256);
    m_gc_runtime_row_frames.clear();
    m_wii_runtime_row_frames.clear();
    recording_file.ReadBytes(m_temp_input.data(), m_temp_input.size());
    m_current_byte = 0;
  }
  recording_file.Close();
  m_movie_path = movie_path;

  // Load savestate (and skip to frame data)
  if (m_temp_header.bFromSaveState && savestate_path)
  {
    const std::string savestate_path_temp = movie_path + ".sav";
    if (File::Exists(savestate_path_temp))
    {
      *savestate_path = savestate_path_temp;
    }
    else
    {
      PanicAlertFmtT("Movie {0} indicates that it starts from a savestate, but {1} doesn't exist. "
                     "The movie will likely not sync!",
                     movie_path, savestate_path_temp);
    }

    m_recording_from_save_state = true;

#ifdef USE_RETRO_ACHIEVEMENTS
    // On the chance someone tries to re-enable before the TAS can start
    Config::SetBase(Config::RA_HARDCORE_ENABLED, false);
#endif  // USE_RETRO_ACHIEVEMENTS

    LoadInput(movie_path);
  }

  return true;
}

void MovieManager::DoState(PointerWrap& p)
{
  // many of these could be useful to save even when no movie is active,
  // and the data is tiny, so let's just save it regardless of movie state.
  p.Do(m_current_frame);
  p.Do(m_current_byte);
  p.Do(m_current_lag_count);
  p.Do(m_current_input_count);
  p.Do(m_polled);
  p.Do(m_tick_count_at_last_input);
  // other variables (such as s_totalBytes and m_total_frames) are set in LoadInput
}

void MovieManager::OnAfterStateLoad()
{
  {
    std::lock_guard guard(m_temp_input_lock);
    m_gc_runtime_row_frames.clear();
    m_wii_runtime_row_frames.clear();
  }
  ++m_runtime_data_generation;

  {
    std::lock_guard guard(m_input_display_lock);
    for (auto& display : m_input_display)
      display.clear();
  }

  {
    std::lock_guard guard(m_live_input_lock);
    m_displayed_pad_status.fill(std::nullopt);
    m_displayed_wiimote_state.fill(std::nullopt);
  }

  ApplyWiimoteBatteryIfPending();
  ApplyGBADisconnectIfPending();
}

// NOTE: Host Thread
void MovieManager::LoadInput(const std::string& movie_path)
{
  File::IOFile t_record;
  if (!t_record.Open(movie_path, "r+b"))
  {
    PanicAlertFmtT("Failed to read {0}", movie_path);
    EndPlayInput(false);
    return;
  }

  t_record.ReadArray(&m_temp_header, 1);

  if (!IsMovieHeader(m_temp_header.filetype))
  {
    PanicAlertFmtT("Savestate movie {0} is corrupted, movie recording stopping...", movie_path);
    EndPlayInput(false);
    return;
  }
  ReadHeader();
  if (!m_read_only)
  {
    m_rerecords++;
    m_temp_header.numRerecords = m_rerecords;
    t_record.Seek(0, File::SeekOrigin::Begin);
    t_record.WriteArray(&m_temp_header, 1);
  }

  ChangePads();
  if (m_system.IsWii())
    ChangeWiiPads(true);

  u64 totalSavedBytes = t_record.GetSize() - 256;

  bool afterEnd = false;
  // This can only happen if the user manually deletes data from the dtm.
  if (m_current_byte > totalSavedBytes)
  {
    PanicAlertFmtT(
        "Warning: You loaded a save whose movie ends before the current frame in the save "
        "(byte {0} < {1}) (frame {2} < {3}). You should load another save before continuing.",
        totalSavedBytes + 256, m_current_byte + 256, m_temp_header.frameCount, m_current_frame);
    afterEnd = true;
  }

  u64 current_byte_snapshot = 0;
  size_t temp_input_size_snapshot = 0;
  std::vector<u8> temp_input_snapshot;
  bool should_reload_movie = false;
  {
    std::lock_guard guard(m_temp_input_lock);
    current_byte_snapshot = m_current_byte;
    temp_input_size_snapshot = m_temp_input.size();
    should_reload_movie = !m_read_only || m_temp_input.empty();
    if (!should_reload_movie && m_current_byte > 0 && !m_temp_input.empty())
      temp_input_snapshot = m_temp_input;
  }

  if (should_reload_movie)
  {
    std::vector<u8> loaded_input(static_cast<size_t>(totalSavedBytes));
    t_record.ReadBytes(loaded_input.data(), loaded_input.size());

    m_total_frames = m_temp_header.frameCount;
    m_total_lag_count = m_temp_header.lagCount;
    m_total_input_count = m_temp_header.inputCount;
    m_total_tick_count = m_tick_count_at_last_input = m_temp_header.tickCount;

    std::lock_guard guard(m_temp_input_lock);
    m_temp_input = std::move(loaded_input);
    m_gc_runtime_row_frames.clear();
    m_wii_runtime_row_frames.clear();
    ++m_runtime_data_generation;
  }
  else if (current_byte_snapshot > 0)
  {
    if (current_byte_snapshot > totalSavedBytes)
    {
    }
    else if (current_byte_snapshot > temp_input_size_snapshot)
    {
      afterEnd = true;
      PanicAlertFmtT(
          "Warning: You loaded a save that's after the end of the current movie. (byte {0} "
          "> {1}) (input {2} > {3}). You should load another save before continuing, or load "
          "this state with read-only mode off.",
          current_byte_snapshot + 256, temp_input_size_snapshot + 256, m_current_input_count,
          m_total_input_count);
    }
    else if (!temp_input_snapshot.empty())
    {
      std::vector<u8> movInput(static_cast<size_t>(current_byte_snapshot));
      t_record.ReadArray(movInput.data(), movInput.size());

      const auto mismatch_result = std::ranges::mismatch(movInput, temp_input_snapshot);

      if (mismatch_result.in1 != movInput.end())
      {
        const ptrdiff_t mismatch_index = std::distance(movInput.begin(), mismatch_result.in1);

        if (IsUsingWiimote(0))
        {
          const size_t byte_offset = static_cast<size_t>(mismatch_index) + sizeof(DTMHeader);

          PanicAlertFmtT(
              "Warning: You loaded a save whose movie mismatches on byte {0} ({1:#x}). "
              "You should load another save before continuing, or load this state with "
              "read-only mode off. Otherwise you'll probably get a desync.",
              byte_offset, byte_offset);

          std::lock_guard guard(m_temp_input_lock);
          if (movInput.size() <= m_temp_input.size())
            std::ranges::copy(movInput, m_temp_input.begin());
          ++m_runtime_data_generation;
        }
        else
        {
          const ptrdiff_t frame = mismatch_index / sizeof(ControllerState);
          ControllerState curPadState;
          memcpy(&curPadState, &temp_input_snapshot[frame * sizeof(ControllerState)],
                 sizeof(ControllerState));
          ControllerState movPadState;
          memcpy(&movPadState, &movInput[frame * sizeof(ControllerState)],
                 sizeof(ControllerState));
          PanicAlertFmtT(
              "Warning: You loaded a save whose movie mismatches on frame {0}. You should load "
              "another save before continuing, or load this state with read-only mode off. "
              "Otherwise you'll probably get a desync.\n\n"
              "More information: The current movie is {1} frames long and the savestate's movie "
              "is {2} frames long.\n\n"
              "On frame {3}, the current movie presses:\n"
              "Start={4}, A={5}, B={6}, X={7}, Y={8}, Z={9}, DUp={10}, DDown={11}, DLeft={12}, "
              "DRight={13}, L={14}, R={15}, LT={16}, RT={17}, AnalogX={18}, AnalogY={19}, "
              "CX={20}, CY={21}, Connected={22}"
              "\n\n"
              "On frame {23}, the savestate's movie presses:\n"
              "Start={24}, A={25}, B={26}, X={27}, Y={28}, Z={29}, DUp={30}, DDown={31}, "
              "DLeft={32}, DRight={33}, L={34}, R={35}, LT={36}, RT={37}, AnalogX={38}, "
              "AnalogY={39}, CX={40}, CY={41}, Connected={42}",
              frame, m_total_frames, m_temp_header.frameCount, frame, curPadState.Start,
              curPadState.A, curPadState.B, curPadState.X, curPadState.Y, curPadState.Z,
              curPadState.DPadUp, curPadState.DPadDown, curPadState.DPadLeft,
              curPadState.DPadRight, curPadState.L, curPadState.R, curPadState.TriggerL,
              curPadState.TriggerR, curPadState.AnalogStickX, curPadState.AnalogStickY,
              curPadState.CStickX, curPadState.CStickY, curPadState.is_connected, frame,
              movPadState.Start, movPadState.A, movPadState.B, movPadState.X, movPadState.Y,
              movPadState.Z, movPadState.DPadUp, movPadState.DPadDown, movPadState.DPadLeft,
              movPadState.DPadRight, movPadState.L, movPadState.R, movPadState.TriggerL,
              movPadState.TriggerR, movPadState.AnalogStickX, movPadState.AnalogStickY,
              movPadState.CStickX, movPadState.CStickY, curPadState.is_connected);
        }
      }
    }
  }
  t_record.Close();

  m_save_config = m_temp_header.bSaveConfig;

  if (!afterEnd)
  {
    if (m_read_only)
    {
      if (m_play_mode != PlayMode::Playing)
      {
        m_play_mode = PlayMode::Playing;
        Core::UpdateWantDeterminism(m_system);
        Core::DisplayMessage("Switched to playback", 2000);
      }
    }
    else
    {
      if (m_play_mode != PlayMode::Recording)
      {
        m_play_mode = PlayMode::Recording;
        Core::UpdateWantDeterminism(m_system);
        Core::DisplayMessage("Switched to recording", 2000);
      }
    }
  }
  else
  {
    EndPlayInput(false);
  }
}

// NOTE: CPU Thread
void MovieManager::CheckInputEnd()
{
  bool should_end = false;
  {
    std::lock_guard guard(m_temp_input_lock);
    should_end = m_current_byte >= m_temp_input.size();
  }

  if (should_end || (m_system.GetCoreTiming().GetTicks() > m_total_tick_count &&
                     !IsRecordingInputFromSaveState()))
    EndPlayInput(!m_read_only);
}

// NOTE: CPU Thread
void MovieManager::PlayController(GCPadStatus* PadStatus, int controllerID)
{
  // Correct playback is entirely dependent on the emulator polling the controllers
  // in the same order done during recording
  if (!IsPlayingInput() || !IsUsingPad(controllerID))
    return;

  bool premature_end = false;
  {
    std::lock_guard guard(m_temp_input_lock);
    if (m_temp_input.empty())
      return;

    if (m_current_byte + sizeof(ControllerState) > m_temp_input.size())
    {
      PanicAlertFmtT("Premature movie end in PlayController. {0} + {1} > {2}", m_current_byte,
                     sizeof(ControllerState), m_temp_input.size());
      premature_end = true;
    }
    else
    {
      const auto active_controllers = GetActiveGCControllerOrder(m_controllers);
      const size_t stride = active_controllers.size() * sizeof(ControllerState);
      if (stride > 0)
      {
        const size_t row_index = static_cast<size_t>(m_current_byte / stride);
        if (m_gc_runtime_row_frames.size() <= row_index)
          m_gc_runtime_row_frames.resize(row_index + 1);
        m_gc_runtime_row_frames[row_index] = m_current_frame;
      }

      memcpy(&m_pad_state, &m_temp_input[m_current_byte], sizeof(ControllerState));
      m_current_byte += sizeof(ControllerState);
    }
  }

  if (premature_end)
  {
    EndPlayInput(!m_read_only);
    return;
  }

  PadStatus->isConnected = m_pad_state.is_connected;

  PadStatus->triggerLeft = m_pad_state.TriggerL;
  PadStatus->triggerRight = m_pad_state.TriggerR;

  PadStatus->stickX = m_pad_state.AnalogStickX;
  PadStatus->stickY = m_pad_state.AnalogStickY;

  PadStatus->substickX = m_pad_state.CStickX;
  PadStatus->substickY = m_pad_state.CStickY;

  PadStatus->button = 0;
  PadStatus->button |= PAD_USE_ORIGIN;

  if (m_pad_state.A)
  {
    PadStatus->button |= PAD_BUTTON_A;
    PadStatus->analogA = 0xFF;
  }
  if (m_pad_state.B)
  {
    PadStatus->button |= PAD_BUTTON_B;
    PadStatus->analogB = 0xFF;
  }
  if (m_pad_state.X)
    PadStatus->button |= PAD_BUTTON_X;
  if (m_pad_state.Y)
    PadStatus->button |= PAD_BUTTON_Y;
  if (m_pad_state.Z)
    PadStatus->button |= PAD_TRIGGER_Z;
  if (m_pad_state.Start)
    PadStatus->button |= PAD_BUTTON_START;

  if (m_pad_state.DPadUp)
    PadStatus->button |= PAD_BUTTON_UP;
  if (m_pad_state.DPadDown)
    PadStatus->button |= PAD_BUTTON_DOWN;
  if (m_pad_state.DPadLeft)
    PadStatus->button |= PAD_BUTTON_LEFT;
  if (m_pad_state.DPadRight)
    PadStatus->button |= PAD_BUTTON_RIGHT;

  if (m_pad_state.L)
    PadStatus->button |= PAD_TRIGGER_L;
  if (m_pad_state.R)
    PadStatus->button |= PAD_TRIGGER_R;

  if (m_pad_state.get_origin)
    PadStatus->button |= PAD_GET_ORIGIN;

  if (m_pad_state.disc)
  {
    const Core::CPUThreadGuard guard(m_system);
    if (!m_system.GetDVDInterface().AutoChangeDisc(guard))
    {
      m_system.GetCPU().Break();
      PanicAlertFmtT("Change the disc to {0}", m_disc_change_filename);
    }
  }

  if (m_pad_state.reset)
    m_system.GetProcessorInterface().ResetButton_Tap();

  {
    std::string display_str = GenerateInputDisplayString(m_pad_state, controllerID);

    std::lock_guard guard(m_input_display_lock);
    m_input_display[controllerID] = std::move(display_str);
  }

  UpdateDisplayedPadStatus(controllerID, *PadStatus);

  CheckInputEnd();
}

// NOTE: CPU Thread
bool MovieManager::PlayWiimote(int wiimote, DesiredWiimoteState* desired_state)
{
  if (!IsPlayingInput() || !IsUsingWiimote(wiimote))
    return false;

  SerializedWiimoteState serialized;
  bool consumed_row = false;
  while (true)
  {
    bool premature_end = false;
    bool invalid_length = false;
    {
      std::lock_guard guard(m_temp_input_lock);
      if (m_temp_input.empty())
        return false;

      if (m_current_byte + sizeof(u8) > m_temp_input.size())
      {
        PanicAlertFmtT("Premature movie end in PlayWiimote. {0} + 1 > {1}", m_current_byte,
                       m_temp_input.size());
        premature_end = true;
      }
      else
      {
        serialized.length = m_temp_input[m_current_byte];
        ++m_current_byte;

        if (serialized.length == 0)
        {
          const size_t row_index = m_wii_runtime_row_frames.size();
          m_wii_runtime_row_frames.resize(row_index + 1);
          m_wii_runtime_row_frames[row_index] = m_current_frame;
          // Zero-length marker indicates a console reset for Wiimote movies.
          m_system.GetProcessorInterface().ResetButton_Tap();
          continue;
        }

        if (serialized.length > serialized.data.size())
        {
          PanicAlertFmtT("Invalid serialized length:{0} in PlayWiimote. byte:{1}",
                         int(serialized.length), m_current_byte - 1);
          invalid_length = true;
        }
        else if (m_current_byte + serialized.length > m_temp_input.size())
        {
          PanicAlertFmtT("Premature movie end in PlayWiimote. {0} + {1} > {2}", m_current_byte,
                         int(serialized.length), m_temp_input.size());
          premature_end = true;
        }
        else
        {
          std::copy_n(m_temp_input.data() + m_current_byte, serialized.length,
                      serialized.data.data());
          m_current_byte += serialized.length;
        }
      }
    }

    if (premature_end || invalid_length)
    {
      EndPlayInput(!m_read_only);
      return false;
    }

    if (!WiimoteEmu::DeserializeDesiredState(desired_state, serialized))
    {
      PanicAlertFmtT("Aborting playback. Error in DeserializeDesiredState. byte:{0}{1}",
                     m_current_byte,
                     (m_controllers == ControllerTypeArray{}) ?
                         " Try re-creating the recording with all GameCube controllers "
                         "disabled (in Configure > GameCube > Device Settings)." :
                         "");
      EndPlayInput(!m_read_only);
      return false;
    }

    consumed_row = true;
    ++m_current_input_count;
    break;
  }

  if (consumed_row)
  {
    std::lock_guard guard(m_temp_input_lock);
    const size_t row_index = m_wii_runtime_row_frames.size();
    if (m_wii_runtime_row_frames.size() <= row_index)
      m_wii_runtime_row_frames.resize(row_index + 1);
    m_wii_runtime_row_frames[row_index] = m_current_frame;
  }

  CheckInputEnd();
  return true;
}

// NOTE: Host / EmuThread / CPU Thread
void MovieManager::EndPlayInput(bool cont)
{
  if (cont)
  {
    // If !IsMovieActive(), changing m_play_mode requires calling UpdateWantDeterminism
    ASSERT(IsMovieActive());

    m_play_mode = PlayMode::Recording;
    Core::DisplayMessage("Reached movie end. Resuming recording.", 2000);
  }
  else if (m_play_mode != PlayMode::None)
  {
    // We can be called by EmuThread during boot (CPU::State::PowerDown)
    auto& cpu = m_system.GetCPU();
    const bool was_running = Core::IsRunning(m_system) && !cpu.IsStepping();
    if (was_running && Config::Get(Config::MAIN_MOVIE_PAUSE_MOVIE))
      cpu.Break();
    m_rerecords = 0;
    m_current_byte = 0;
    m_play_mode = PlayMode::None;
    Core::DisplayMessage("Movie End.", 2000);
    m_recording_from_save_state = false;
    Config::RemoveLayer(Config::LayerType::Movie);
    // we don't clear these things because otherwise we can't resume playback if we load a movie
    // state later
    // m_total_frames = s_totalBytes = 0;
    // delete tmpInput;
    // tmpInput = nullptr;

    Core::QueueHostJob([](Core::System& system) { Core::UpdateWantDeterminism(system); });
  }
}


void MovieManager::SaveWiimoteBatteryToHeader(DTMHeader* header) const
{
  if (!header->bWii)
    return;

  header->reserved2[10] = DTM_BATTERY_MAGIC;

  const int controller_count = ::Wiimote::GetConfig()->GetControllerCount();
  for (int i = 0; i < 4; ++i)
  {
    u8 value = DTM_BATTERY_UNSET;
    if (controller_count > i)
    {
      auto* wiimote =
          static_cast<WiimoteEmu::Wiimote*>(::Wiimote::GetConfig()->GetController(i));
      const int battery = static_cast<int>(std::lround(wiimote->GetBatterySettingValue()));
      value = static_cast<u8>(std::clamp(battery, 0, 100));
    }
    header->reserved2[i] = value;
  }
}

void MovieManager::ApplyWiimoteBatteryFromHeader(const DTMHeader& header)
{
  m_has_dtm_wiimote_battery = false;

  if (!header.bWii)
    return;

  if (header.reserved2[10] != DTM_BATTERY_MAGIC)
    return;

  m_has_dtm_wiimote_battery = true;
  for (int i = 0; i < 4; ++i)
    m_dtm_wiimote_battery[i] = header.reserved2[i];

  ApplyWiimoteBatteryIfPending();
}

void MovieManager::ApplyWiimoteBatteryIfPending()
{
  if (!m_has_dtm_wiimote_battery)
    return;

  const int controller_count = ::Wiimote::GetConfig()->GetControllerCount();
  if (controller_count <= 0)
    return;

  for (int i = 0; i < 4; ++i)
  {
    if (controller_count <= i)
      continue;
    const u8 value = m_dtm_wiimote_battery[i];
    if (value == DTM_BATTERY_UNSET)
      continue;
    auto* wiimote =
        static_cast<WiimoteEmu::Wiimote*>(::Wiimote::GetConfig()->GetController(i));
    wiimote->SetBatterySettingValue(value);
  }

  m_has_dtm_wiimote_battery = false;
}

void MovieManager::SaveGBADisconnectToHeader(DTMHeader* header) const
{
#ifdef HAS_LIBMGBA
  u8 disconnect_mask = 0;
  const auto& si = m_system.GetSerialInterface();
  for (int i = 0; i < 4; ++i)
  {
    if (!IsUsingGBA(i))
      continue;

    auto* device = si.GetDevice(i);
    if (!device || device->GetDeviceType() != SerialInterface::SIDEVICE_GC_GBA_EMULATED)
      continue;

    auto* gba_device = static_cast<SerialInterface::CSIDevice_GBAEmu*>(device);
    const auto core = gba_device->GetCore();
    if (core && core->IsForceDisconnect())
      disconnect_mask |= static_cast<u8>(1 << i);
  }

  header->reserved2[4] = disconnect_mask;
  header->reserved2[9] = DTM_GBA_DISCONNECT_MAGIC;
#else
  header->reserved2[4] = 0;
#endif
}

void MovieManager::ApplyGBADisconnectFromHeader(const DTMHeader& header)
{
  m_has_dtm_gba_disconnect = false;
  m_dtm_gba_disconnect_mask = 0;

  if (header.reserved2[9] != DTM_GBA_DISCONNECT_MAGIC)
    return;

  m_has_dtm_gba_disconnect = true;
  m_dtm_gba_disconnect_mask = header.reserved2[4];
  ApplyGBADisconnectIfPending();
}

void MovieManager::ApplyGBADisconnectIfPending()
{
#ifdef HAS_LIBMGBA
  if (!m_has_dtm_gba_disconnect)
    return;

  const auto& si = m_system.GetSerialInterface();
  for (int i = 0; i < 4; ++i)
  {
    auto* device = si.GetDevice(i);
    if (!device || device->GetDeviceType() != SerialInterface::SIDEVICE_GC_GBA_EMULATED)
      continue;

    auto* gba_device = static_cast<SerialInterface::CSIDevice_GBAEmu*>(device);
    const auto core = gba_device->GetCore();
    if (!core)
      continue;

    core->SetForceDisconnect((m_dtm_gba_disconnect_mask & (1 << i)) != 0);
  }
#endif
}

// NOTE: Save State + Host Thread
void MovieManager::SaveRecording(const std::string& filename)
{
  File::IOFile save_record(filename, "wb");
  // Create the real header now and write it
  DTMHeader header;
  memset(&header, 0, sizeof(DTMHeader));

  header.filetype[0] = 'D';
  header.filetype[1] = 'T';
  header.filetype[2] = 'M';
  header.filetype[3] = 0x1A;
  strncpy(header.gameID.data(), SConfig::GetInstance().GetGameID().c_str(), 6);
  header.bWii = m_system.IsWii();
  header.controllers = 0;
  header.GBAControllers = 0;
  for (int i = 0; i < 4; ++i)
  {
    if (IsUsingGBA(i))
      header.GBAControllers |= 1 << i;
    if (IsUsingPad(i))
      header.controllers |= 1 << i;
    if (IsUsingWiimote(i) && m_system.IsWii())
      header.controllers |= 1 << (i + 4);
  }

  header.bFromSaveState = m_recording_from_save_state;
  header.frameCount = m_total_frames;
  header.lagCount = m_total_lag_count;
  header.inputCount = m_total_input_count;
  header.numRerecords = m_rerecords;
  header.recordingStartTime = m_recording_start_time;

  header.bSaveConfig = true;
  ConfigLoaders::SaveToDTM(&header);
  header.memcards = m_memcards;
  header.bClearSave = m_clear_save;
  header.bNetPlay = m_net_play;
  strncpy(header.discChange.data(), m_disc_change_filename.c_str(), header.discChange.size());
  strncpy(header.author.data(), m_author.c_str(), header.author.size());
  header.md5 = m_md5;
  header.bongos = m_bongos;
  header.revision = m_revision;
  header.DSPiromHash = m_dsp_irom_hash;
  header.DSPcoefHash = m_dsp_coef_hash;
  header.tickCount = m_total_tick_count;
  SaveWiimoteBatteryToHeader(&header);
  SaveGBADisconnectToHeader(&header);

  // TODO
  header.uniqueID = 0;
  // header.audioEmulator;

  save_record.WriteArray(&header, 1);

  std::vector<u8> temp_input_copy;
  {
    std::lock_guard guard(m_temp_input_lock);
    temp_input_copy = m_temp_input;
  }

  bool success = save_record.WriteBytes(temp_input_copy.data(), temp_input_copy.size());

  if (success && m_recording_from_save_state)
  {
    std::string stateFilename = filename + ".sav";
    success = File::CopyRegularFile(File::GetUserPath(D_STATESAVES_IDX) + "dtm.sav", stateFilename);
  }

  if (success)
    Core::DisplayMessage(fmt::format("DTM {} saved", filename), 2000);
  else
    Core::DisplayMessage(fmt::format("Failed to save {}", filename), 2000);
}

// NOTE: EmuThread / Host Thread
void MovieManager::GetSettings()
{
  using ExpansionInterface::EXIDeviceType;
  const EXIDeviceType slot_a_type = Config::Get(Config::MAIN_SLOT_A);
  const EXIDeviceType slot_b_type = Config::Get(Config::MAIN_SLOT_B);
  const bool slot_a_has_raw_memcard = slot_a_type == EXIDeviceType::MemoryCard;
  const bool slot_a_has_gci_folder = slot_a_type == EXIDeviceType::MemoryCardFolder;
  const bool slot_b_has_raw_memcard = slot_b_type == EXIDeviceType::MemoryCard;
  const bool slot_b_has_gci_folder = slot_b_type == EXIDeviceType::MemoryCardFolder;

  m_save_config = true;
  m_net_play = NetPlay::IsNetPlayRunning();
  if (m_system.IsWii())
  {
    u64 title_id = SConfig::GetInstance().GetTitleID();
    m_clear_save = !File::Exists(
        Common::GetTitleDataPath(title_id, Common::FromWhichRoot::Session) + "/banner.bin");
  }
  else
  {
    const auto raw_memcard_exists = [](ExpansionInterface::Slot card_slot) {
      return File::Exists(Config::GetMemcardPath(card_slot, SConfig::GetInstance().m_region));
    };
    const auto gci_folder_has_saves = [this](ExpansionInterface::Slot card_slot) {
      const auto [path, migrate] = ExpansionInterface::CEXIMemoryCard::GetGCIFolderPath(
          card_slot, ExpansionInterface::AllowMovieFolder::No, *this);
      const u64 number_of_saves = File::ScanDirectoryTree(path, false).size;
      return number_of_saves > 0;
    };

    m_clear_save = !(slot_a_has_raw_memcard && raw_memcard_exists(ExpansionInterface::Slot::A)) &&
                   !(slot_b_has_raw_memcard && raw_memcard_exists(ExpansionInterface::Slot::B)) &&
                   !(slot_a_has_gci_folder && gci_folder_has_saves(ExpansionInterface::Slot::A)) &&
                   !(slot_b_has_gci_folder && gci_folder_has_saves(ExpansionInterface::Slot::B));
  }
  m_memcards |= (slot_a_has_raw_memcard || slot_a_has_gci_folder) << 0;
  m_memcards |= (slot_b_has_raw_memcard || slot_b_has_gci_folder) << 1;

  m_revision = ConvertGitRevisionToBytes(Common::GetScmRevGitStr());

  if (!Config::Get(Config::MAIN_DSP_HLE))
  {
    std::string irom_file = File::GetUserPath(D_GCUSER_IDX) + DSP_IROM;
    std::string coef_file = File::GetUserPath(D_GCUSER_IDX) + DSP_COEF;

    if (!File::Exists(irom_file))
      irom_file = File::GetSysDirectory() + GC_SYS_DIR DIR_SEP DSP_IROM;
    if (!File::Exists(coef_file))
      coef_file = File::GetSysDirectory() + GC_SYS_DIR DIR_SEP DSP_COEF;
    std::vector<u16> irom(DSP::DSP_IROM_SIZE);
    File::IOFile file_irom(irom_file, "rb");

    file_irom.ReadArray(irom.data(), irom.size());
    file_irom.Close();
    for (u16& entry : irom)
      entry = Common::swap16(entry);

    std::vector<u16> coef(DSP::DSP_COEF_SIZE);
    File::IOFile file_coef(coef_file, "rb");

    file_coef.ReadArray(coef.data(), coef.size());
    file_coef.Close();
    for (u16& entry : coef)
      entry = Common::swap16(entry);
    m_dsp_irom_hash =
        Common::HashAdler32(reinterpret_cast<u8*>(irom.data()), DSP::DSP_IROM_BYTE_SIZE);
    m_dsp_coef_hash =
        Common::HashAdler32(reinterpret_cast<u8*>(coef.data()), DSP::DSP_COEF_BYTE_SIZE);
  }
  else
  {
    m_dsp_irom_hash = 0;
    m_dsp_coef_hash = 0;
  }
}

// NOTE: Entrypoint for own thread
void MovieManager::CheckMD5()
{
  if (m_current_file_name.empty())
    return;

  // The MD5 hash was introduced in 3.0-846-gca650d4435.
  // Before that, these header bytes were set to zero.
  if (m_temp_header.md5 == std::array<u8, 16>{})
    return;

  Core::DisplayMessage("Verifying checksum...", 2000);

  std::array<u8, 16> game_md5;
  mbedtls_md_file(mbedtls_md_info_from_type(MBEDTLS_MD_MD5), m_current_file_name.c_str(),
                  game_md5.data());

  if (game_md5 == m_md5)
    Core::DisplayMessage("Checksum of current game matches the recorded game.", 2000);
  else
    Core::DisplayMessage("Checksum of current game does not match the recorded game!", 3000);
}

// NOTE: Entrypoint for own thread
void MovieManager::GetMD5()
{
  if (m_current_file_name.empty())
    return;

  Core::DisplayMessage("Calculating checksum of game file...", 2000);
  mbedtls_md_file(mbedtls_md_info_from_type(MBEDTLS_MD_MD5), m_current_file_name.c_str(),
                  m_md5.data());
  Core::DisplayMessage("Finished calculating checksum.", 2000);
}

// NOTE: EmuThread
void MovieManager::Shutdown()
{
  m_current_input_count = m_total_input_count = m_total_frames = m_tick_count_at_last_input = 0;
  {
    std::lock_guard guard(m_temp_input_lock);
    m_temp_input.clear();
    m_gc_runtime_row_frames.clear();
  }
}
}  // namespace Movie
