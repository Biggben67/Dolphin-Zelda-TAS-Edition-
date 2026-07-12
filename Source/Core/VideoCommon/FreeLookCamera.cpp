// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/FreeLookCamera.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fmt/format.h>

#include "Common/ChunkFile.h"
#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/StringUtil.h"
#include "Core/Core.h"
#include "Core/ConfigManager.h"
#include "Core/Movie.h"
#include "Core/PowerPC/MMU.h"
#include "Core/System.h"

FreeLookCamera g_freelook_camera;

namespace
{
struct FocusTargetSample
{
  Common::Vec3 target;
  std::optional<Common::Vec3> track_target;
  uint64_t sequence = 0;
  std::optional<Common::Matrix44> game_view_matrix;
  std::optional<Common::Vec3> camera_eye;
  std::optional<Common::Vec3> camera_center;
  std::optional<Common::Vec3> camera_up;
};

enum class FocusTargetStatus
{
  Inactive,
  NoTarget,
  TargetOnly,
  TargetAndCamera,
};

std::mutex s_focus_target_mutex;
std::optional<FocusTargetSample> s_focus_target;
FocusTargetStatus s_focus_target_status = FocusTargetStatus::Inactive;
std::atomic<uint64_t> s_focus_target_sequence = 0;
std::optional<uint64_t> s_last_focus_movie_frame;

std::string to_string(FreeLook::ControlType type)
{
  switch (type)
  {
  case FreeLook::ControlType::SixAxis:
    return "Six Axis";
  case FreeLook::ControlType::FPS:
    return "First Person";
  case FreeLook::ControlType::Orbital:
    return "Orbital";
  }

  return "";
}

float Clamp01(float value)
{
  return std::clamp(value, 0.0f, 1.0f);
}
float SmoothStep(float t)
{
  const float clamped_t = Clamp01(t);
  return clamped_t * clamped_t * (3.0f - 2.0f * clamped_t);
}

Common::Vec2 Lerp(const Common::Vec2& a, const Common::Vec2& b, float t)
{
  return a + (b - a) * Clamp01(t);
}

Common::Quaternion QuaternionFromMatrix(const Common::Matrix44& matrix)
{
  const float m00 = matrix.data[0];
  const float m01 = matrix.data[1];
  const float m02 = matrix.data[2];
  const float m10 = matrix.data[4];
  const float m11 = matrix.data[5];
  const float m12 = matrix.data[6];
  const float m20 = matrix.data[8];
  const float m21 = matrix.data[9];
  const float m22 = matrix.data[10];

  const float trace = m00 + m11 + m22;
  if (trace > 0.0f)
  {
    const float s = std::sqrt(trace + 1.0f) * 2.0f;
    return Common::Quaternion(0.25f * s, (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s)
        .Normalized();
  }

  if (m00 > m11 && m00 > m22)
  {
    const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
    return Common::Quaternion((m21 - m12) / s, 0.25f * s, (m01 + m10) / s,
                              (m02 + m20) / s)
        .Normalized();
  }

  if (m11 > m22)
  {
    const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
    return Common::Quaternion((m02 - m20) / s, (m01 + m10) / s, 0.25f * s,
                              (m12 + m21) / s)
        .Normalized();
  }

  const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
  return Common::Quaternion((m10 - m01) / s, (m02 + m20) / s, (m12 + m21) / s, 0.25f * s)
      .Normalized();
}

Common::Matrix44 MatrixFromKeyframeTransform(const Common::Vec3& position,
                                             const Common::Quaternion& rotation)
{
  Common::Matrix44 matrix = Common::Matrix44::FromQuaternion(rotation);
  matrix.data[3] = position.x;
  matrix.data[7] = position.y;
  matrix.data[11] = position.z;
  return matrix;
}

Common::Quaternion NLerp(const Common::Quaternion& a, const Common::Quaternion& b, float t)
{
  Common::Quaternion rhs = b;
  if (a.data.Dot(b.data) < 0.0f)
    rhs.data *= -1.0f;

  const float clamped_t = Clamp01(t);
  Common::Quaternion result;
  result.data.x = a.data.x * (1.0f - clamped_t) + rhs.data.x * clamped_t;
  result.data.y = a.data.y * (1.0f - clamped_t) + rhs.data.y * clamped_t;
  result.data.z = a.data.z * (1.0f - clamped_t) + rhs.data.z * clamped_t;
  result.data.w = a.data.w * (1.0f - clamped_t) + rhs.data.w * clamped_t;
  return result.Normalized();
}

std::string_view Trim(std::string_view text)
{
  const std::size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos)
    return {};

  const std::size_t last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

std::optional<u32> ParseHexAddressTerm(std::string_view text)
{
  text = Trim(text);
  if (text.empty())
    return std::nullopt;

  if (text.front() == '$')
    text.remove_prefix(1);
  else if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    text.remove_prefix(2);

  if (text.empty())
    return std::nullopt;

  uint64_t value = 0;
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value, 16);
  if (ec != std::errc{} || ptr != end || value > UINT32_MAX)
    return std::nullopt;

  return static_cast<u32>(value);
}

std::optional<std::vector<std::string_view>> SplitAddressExpression(std::string_view text)
{
  text = Trim(text);
  if (text.empty())
    return std::nullopt;

  std::vector<std::string_view> terms;
  int bracket_depth = 0;
  std::size_t term_start = 0;
  for (std::size_t i = 0; i < text.size(); ++i)
  {
    if (text[i] == '[')
    {
      ++bracket_depth;
    }
    else if (text[i] == ']')
    {
      --bracket_depth;
      if (bracket_depth < 0)
        return std::nullopt;
    }
    else if (text[i] == '+' && bracket_depth == 0)
    {
      std::string_view term = Trim(text.substr(term_start, i - term_start));
      if (term.empty())
        return std::nullopt;

      terms.push_back(term);
      term_start = i + 1;
    }
  }

  if (bracket_depth != 0)
    return std::nullopt;

  std::string_view term = Trim(text.substr(term_start));
  if (term.empty())
    return std::nullopt;

  terms.push_back(term);
  return terms;
}

bool IsSingleBracketedTerm(std::string_view text)
{
  text = Trim(text);
  if (text.size() < 3 || text.front() != '[' || text.back() != ']')
    return false;

  int bracket_depth = 0;
  for (std::size_t i = 0; i < text.size(); ++i)
  {
    if (text[i] == '[')
    {
      ++bracket_depth;
    }
    else if (text[i] == ']')
    {
      --bracket_depth;
      if (bracket_depth == 0 && i != text.size() - 1)
        return false;

      if (bracket_depth < 0)
        return false;
    }
  }

  return bracket_depth == 0;
}

std::optional<u32> ReadU32FromMemory(const Core::CPUThreadGuard& guard, u32 address)
{
  const std::optional<PowerPC::ReadResult<u32>> value =
      PowerPC::MMU::HostTryRead<u32>(guard, address);
  if (!value)
    return std::nullopt;

  return value->value;
}

std::optional<float> ReadFloatFromMemory(const Core::CPUThreadGuard& guard, u32 address)
{
  const std::optional<PowerPC::ReadResult<u32>> value =
      PowerPC::MMU::HostTryRead<u32>(guard, address);
  if (!value)
    return std::nullopt;

  return std::bit_cast<float>(value->value);
}

std::optional<u32> ResolveAddressExpression(const Core::CPUThreadGuard& guard,
                                            std::string_view text, int depth);

std::optional<Common::Vec3> ReadVec3FromMemoryExpressions(const Core::CPUThreadGuard& guard,
                                                          const std::string& x_expression,
                                                          const std::string& y_expression,
                                                          const std::string& z_expression)
{
  const std::optional<u32> x_address = ResolveAddressExpression(guard, x_expression, 0);
  const std::optional<u32> y_address = ResolveAddressExpression(guard, y_expression, 0);
  const std::optional<u32> z_address = ResolveAddressExpression(guard, z_expression, 0);
  if (!x_address || !y_address || !z_address)
    return std::nullopt;

  const std::optional<float> x = ReadFloatFromMemory(guard, *x_address);
  const std::optional<float> y = ReadFloatFromMemory(guard, *y_address);
  const std::optional<float> z = ReadFloatFromMemory(guard, *z_address);
  if (!x || !y || !z)
    return std::nullopt;

  const Common::Vec3 value{*x, *y, *z};
  if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z))
    return std::nullopt;

  return value;
}

std::optional<Common::Vec3> ReadVec3FromConfigStrings(const std::string& x_text,
                                                      const std::string& y_text,
                                                      const std::string& z_text)
{
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  if (!TryParse(x_text, &x) || !TryParse(y_text, &y) || !TryParse(z_text, &z))
    return std::nullopt;

  const Common::Vec3 value{x, y, z};
  if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z))
    return std::nullopt;

  return value;
}

std::optional<Common::Vec3> ReadVec3FromMemoryAddress(const Core::CPUThreadGuard& guard,
                                                      u32 base_address)
{
  const std::optional<float> x = ReadFloatFromMemory(guard, base_address);
  const std::optional<float> y = ReadFloatFromMemory(guard, base_address + sizeof(float));
  const std::optional<float> z = ReadFloatFromMemory(guard, base_address + sizeof(float) * 2);
  if (!x || !y || !z)
    return std::nullopt;

  const Common::Vec3 value{*x, *y, *z};
  if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z))
    return std::nullopt;

  return value;
}

std::optional<Common::Matrix44> ReadViewMatrixFromMemoryAddress(
    const Core::CPUThreadGuard& guard, u32 base_address)
{
  Common::Matrix44 matrix = Common::Matrix44::Identity();
  for (u32 row = 0; row < 3; ++row)
  {
    for (u32 column = 0; column < 4; ++column)
    {
      const std::optional<float> value =
          ReadFloatFromMemory(guard, base_address + (row * 4 + column) * sizeof(float));
      if (!value || !std::isfinite(*value))
        return std::nullopt;

      matrix.data[row * 4 + column] = *value;
    }
  }

  matrix.data[12] = 0.0f;
  matrix.data[13] = 0.0f;
  matrix.data[14] = 0.0f;
  matrix.data[15] = 1.0f;
  return matrix;
}

std::optional<Common::Vec3> GetEyeFromViewMatrix(const Common::Matrix44& view_matrix);
std::optional<Common::Vec3> GetUpFromViewMatrix(const Common::Matrix44& view_matrix);

std::optional<Common::Vec3> ReadConfiguredVec3Offset(const Config::Info<std::string>& x_config,
                                                     const Config::Info<std::string>& y_config,
                                                     const Config::Info<std::string>& z_config)
{
  std::optional<Common::Vec3> offset =
      ReadVec3FromConfigStrings(Config::Get(x_config), Config::Get(y_config),
                                Config::Get(z_config));
  return offset.value_or(Common::Vec3{0.0f, 0.0f, 0.0f});
}

bool IsLikelyViewMatrix(const Common::Matrix44& matrix)
{
  const Common::Vec3 right{matrix.data[0], matrix.data[1], matrix.data[2]};
  const Common::Vec3 up{matrix.data[4], matrix.data[5], matrix.data[6]};
  const Common::Vec3 look{matrix.data[8], matrix.data[9], matrix.data[10]};
  if (!std::isfinite(right.x) || !std::isfinite(right.y) || !std::isfinite(right.z) ||
      !std::isfinite(up.x) || !std::isfinite(up.y) || !std::isfinite(up.z) ||
      !std::isfinite(look.x) || !std::isfinite(look.y) || !std::isfinite(look.z))
  {
    return false;
  }

  const float right_length = right.Length();
  const float up_length = up.Length();
  const float look_length = look.Length();
  if (right_length < 0.5f || right_length > 1.5f || up_length < 0.5f || up_length > 1.5f ||
      look_length < 0.5f || look_length > 1.5f)
  {
    return false;
  }

  const Common::Vec3 normalized_right = right / right_length;
  const Common::Vec3 normalized_up = up / up_length;
  const Common::Vec3 normalized_look = look / look_length;
  return std::abs(normalized_right.Dot(normalized_up)) < 0.25f &&
         std::abs(normalized_right.Dot(normalized_look)) < 0.25f &&
         std::abs(normalized_up.Dot(normalized_look)) < 0.25f;
}

bool ReadSkywardSwordCameraFromScnRootPointer(const Core::CPUThreadGuard& guard,
                                             u32 scn_root_pointer_address,
                                             FocusTargetSample* sample)
{
  static constexpr u32 CAMERA_MATRIX_OFFSET = 0xF8;
  const std::optional<u32> scn_root_address =
      ReadU32FromMemory(guard, scn_root_pointer_address);
  if (!scn_root_address || *scn_root_address < 0x80000000 || *scn_root_address > 0x90000000)
    return false;

  const std::optional<Common::Matrix44> view_matrix =
      ReadViewMatrixFromMemoryAddress(guard, *scn_root_address + CAMERA_MATRIX_OFFSET);
  if (!view_matrix || !IsLikelyViewMatrix(*view_matrix))
    return false;

  const std::optional<Common::Vec3> eye = GetEyeFromViewMatrix(*view_matrix);
  const std::optional<Common::Vec3> up = GetUpFromViewMatrix(*view_matrix);
  Common::Vec3 look{view_matrix->data[8], view_matrix->data[9], view_matrix->data[10]};
  if (!eye || !up || look.Length() < 0.0001f)
    return false;

  look = look.Normalized();
  sample->game_view_matrix = view_matrix;
  sample->camera_eye = eye;
  sample->camera_center = *eye - look;
  sample->camera_up = up;
  return true;
}

bool ReadSkywardSwordCamera(const Core::CPUThreadGuard& guard, std::string_view game_id,
                            FocusTargetSample* sample)
{
  struct SupportedScnRoot
  {
    std::string_view game_id;
    u32 scn_root_pointer_address;
  };

  static constexpr SupportedScnRoot supported_scn_roots[] = {
      {"SOUE01", 0x80575BD4},
      {"SOUJ01", 0x80578E34},
  };

  const auto it = std::ranges::find_if(supported_scn_roots, [game_id](const auto& entry) {
    return game_id == entry.game_id;
  });
  if (it != std::end(supported_scn_roots) &&
      ReadSkywardSwordCameraFromScnRootPointer(guard, it->scn_root_pointer_address, sample))
  {
    return true;
  }

  if (!game_id.starts_with("SOU"))
    return false;

  static std::string s_cached_game_id;
  static std::optional<u32> s_cached_scn_root_pointer_address;
  if (s_cached_game_id == game_id && s_cached_scn_root_pointer_address &&
      ReadSkywardSwordCameraFromScnRootPointer(guard, *s_cached_scn_root_pointer_address, sample))
  {
    return true;
  }

  s_cached_game_id = std::string(game_id);
  s_cached_scn_root_pointer_address.reset();

  // PAL symbols are not available in the local SS decomp. Restrict the fallback scan to the small
  // data range where l_scnRoot_p lives in the verified US/JP builds, then cache the first valid hit.
  for (u32 address = 0x80570000; address <= 0x80590000; address += sizeof(u32))
  {
    if (ReadSkywardSwordCameraFromScnRootPointer(guard, address, sample))
    {
      s_cached_scn_root_pointer_address = address;
      return true;
    }
  }

  return false;
}

void ReadSupportedGameCamera(const Core::CPUThreadGuard& guard, FocusTargetSample* sample)
{
  if (!sample)
    return;

  struct SupportedView
  {
    std::string_view game_id;
    u32 game_info_address;
    u32 play_offset;
    u32 camera_info_camera_offset;
    u32 current_view_offset;
  };

  // Zelda titles using dComIfG expose the active view_class* through gameInfo.play.mCurrentView.
  // The view matrix is view_class::viewMtx at +0x140, so focus targets only need XYZ watches.
  static constexpr SupportedView supported_views[] = {
      // Twilight Princess
      {"GZ2E01", 0x804061C0, 0x0F38, 0x4E3C, 0x5010},
      {"GZ2P01", 0x80408160, 0x0F38, 0x4E3C, 0x5010},
      {"GZ2J01", 0x80400300, 0x0F38, 0x4E3C, 0x5010},
      // The Wind Waker
      {"GZLE01", 0x803C4C08, 0x12A0, 0x4870, 0x4A58},
      {"GZLP01", 0x803CC530, 0x12A0, 0x4870, 0x4A58},
      {"GZLJ01", 0x803B8108, 0x12A0, 0x4864, 0x4A4C},
  };

  const std::string game_id = SConfig::GetInstance().GetGameID();
  const auto it = std::ranges::find_if(supported_views, [&game_id](const SupportedView& entry) {
    return game_id == entry.game_id;
  });
  if (it == std::end(supported_views))
  {
    ReadSkywardSwordCamera(guard, game_id, sample);
    return;
  }

  static constexpr u32 LOOKAT_OFFSET = 0x0D8;
  static constexpr u32 LOOKAT_EYE_OFFSET = LOOKAT_OFFSET;
  static constexpr u32 LOOKAT_CENTER_OFFSET = LOOKAT_OFFSET + 0x0C;
  static constexpr u32 LOOKAT_UP_OFFSET = LOOKAT_OFFSET + 0x18;
  static constexpr u32 VIEW_MATRIX_OFFSET = 0x140;

  const u32 play_address = it->game_info_address + it->play_offset;
  std::optional<u32> view_address =
      ReadU32FromMemory(guard, play_address + it->current_view_offset);
  if (!view_address || *view_address == 0)
    view_address = ReadU32FromMemory(guard, play_address + it->camera_info_camera_offset);

  if (!view_address || *view_address == 0)
    return;

  sample->camera_eye = ReadVec3FromMemoryAddress(guard, *view_address + LOOKAT_EYE_OFFSET);
  sample->camera_center = ReadVec3FromMemoryAddress(guard, *view_address + LOOKAT_CENTER_OFFSET);
  sample->camera_up = ReadVec3FromMemoryAddress(guard, *view_address + LOOKAT_UP_OFFSET);
  sample->game_view_matrix =
      ReadViewMatrixFromMemoryAddress(guard, *view_address + VIEW_MATRIX_OFFSET);
}

std::optional<u32> ResolveAddressTerm(const Core::CPUThreadGuard& guard, std::string_view text,
                                      int depth);

std::optional<u32> ResolveAddressExpression(const Core::CPUThreadGuard& guard,
                                            std::string_view text, int depth)
{
  if (depth > 8)
    return std::nullopt;

  const std::optional<std::vector<std::string_view>> terms = SplitAddressExpression(text);
  if (!terms)
    return std::nullopt;

  uint64_t result = 0;
  for (std::string_view term : *terms)
  {
    const std::optional<u32> value = ResolveAddressTerm(guard, term, depth + 1);
    if (!value || result > UINT32_MAX - *value)
      return std::nullopt;

    result += *value;
  }

  return static_cast<u32>(result);
}

std::optional<u32> ResolveAddressTerm(const Core::CPUThreadGuard& guard, std::string_view text,
                                      int depth)
{
  text = Trim(text);
  if (text.empty() || depth > 8)
    return std::nullopt;

  if (text.front() == '*')
  {
    const std::optional<u32> pointer_address =
        ResolveAddressExpression(guard, text.substr(1), depth + 1);
    return pointer_address ? ReadU32FromMemory(guard, *pointer_address) : std::nullopt;
  }

  if (IsSingleBracketedTerm(text))
  {
    const std::string_view inner = text.substr(1, text.size() - 2);
    const std::optional<u32> pointer_address = ResolveAddressExpression(guard, inner, depth + 1);
    return pointer_address ? ReadU32FromMemory(guard, *pointer_address) : std::nullopt;
  }

  return ParseHexAddressTerm(text);
}

std::optional<FocusTargetSample> ReadFocusTarget(const Core::CPUThreadGuard& guard)
{
  if (!Config::Get(Config::FL1_FOCUS_TARGET_ENABLED))
    return std::nullopt;

  std::optional<Common::Vec3> target;
  if (Config::Get(Config::FL1_FOCUS_TARGET_CUSTOM_COORDS_ENABLED))
  {
    target = ReadVec3FromConfigStrings(Config::Get(Config::FL1_FOCUS_TARGET_CUSTOM_X),
                                       Config::Get(Config::FL1_FOCUS_TARGET_CUSTOM_Y),
                                       Config::Get(Config::FL1_FOCUS_TARGET_CUSTOM_Z));
  }
  else
  {
    target = ReadVec3FromMemoryExpressions(guard, Config::Get(Config::FL1_FOCUS_TARGET_ADDRESS_X),
                                           Config::Get(Config::FL1_FOCUS_TARGET_ADDRESS_Y),
                                           Config::Get(Config::FL1_FOCUS_TARGET_ADDRESS_Z));
  }

  if (!target)
    return std::nullopt;

  const Common::Vec3 target_offset =
      *ReadConfiguredVec3Offset(Config::FL1_FOCUS_TARGET_OFFSET_X,
                                Config::FL1_FOCUS_TARGET_OFFSET_Y,
                                Config::FL1_FOCUS_TARGET_OFFSET_Z);
  FocusTargetSample sample{*target + target_offset};
  if (Config::Get(Config::FL1_TRACK_TARGET_ENABLED))
  {
    if (Config::Get(Config::FL1_TRACK_TARGET_CUSTOM_COORDS_ENABLED))
    {
      sample.track_target =
          ReadVec3FromConfigStrings(Config::Get(Config::FL1_TRACK_TARGET_CUSTOM_X),
                                    Config::Get(Config::FL1_TRACK_TARGET_CUSTOM_Y),
                                    Config::Get(Config::FL1_TRACK_TARGET_CUSTOM_Z));
    }
    else
    {
      sample.track_target =
          ReadVec3FromMemoryExpressions(guard, Config::Get(Config::FL1_TRACK_TARGET_ADDRESS_X),
                                        Config::Get(Config::FL1_TRACK_TARGET_ADDRESS_Y),
                                        Config::Get(Config::FL1_TRACK_TARGET_ADDRESS_Z));
    }

    if (sample.track_target)
    {
      const Common::Vec3 track_offset =
          *ReadConfiguredVec3Offset(Config::FL1_TRACK_TARGET_OFFSET_X,
                                    Config::FL1_TRACK_TARGET_OFFSET_Y,
                                    Config::FL1_TRACK_TARGET_OFFSET_Z);
      sample.track_target = *sample.track_target + track_offset;
    }
  }

  ReadSupportedGameCamera(guard, &sample);

  return sample;
}

std::optional<FocusTargetSample> GetCachedFocusTarget()
{
  const std::lock_guard<std::mutex> lock(s_focus_target_mutex);
  return s_focus_target;
}

void SetCachedFocusTarget(std::optional<FocusTargetSample> target)
{
  const std::lock_guard<std::mutex> lock(s_focus_target_mutex);
  s_focus_target = target;
}

void SetFocusTargetStatus(FocusTargetStatus status)
{
  bool changed = false;
  {
    const std::lock_guard<std::mutex> lock(s_focus_target_mutex);
    changed = s_focus_target_status != status;
    s_focus_target_status = status;
  }

  if (!changed)
    return;

  switch (status)
  {
  case FocusTargetStatus::Inactive:
    break;
  case FocusTargetStatus::NoTarget:
    Core::DisplayMessage("Free Look focus target: address read failed", 2500);
    break;
  case FocusTargetStatus::TargetOnly:
    Core::DisplayMessage("Free Look focus target: camera data unavailable", 2500);
    break;
  case FocusTargetStatus::TargetAndCamera:
    Core::DisplayMessage("Free Look focus target: camera lock active", 2500);
    break;
  }
}

std::optional<Common::Matrix44> BuildLookAtMatrix(const Common::Vec3& eye,
                                                  const Common::Vec3& center,
                                                  const Common::Vec3& up)
{
  Common::Vec3 look = eye - center;
  if (look.Length() < 0.0001f)
    return std::nullopt;
  look = look.Normalized();

  Common::Vec3 right = up.Cross(look);
  if (right.Length() < 0.0001f)
  {
    right = Common::Vec3{0.0f, 1.0f, 0.0f}.Cross(look);
    if (right.Length() < 0.0001f)
      right = Common::Vec3{1.0f, 0.0f, 0.0f}.Cross(look);
  }
  if (right.Length() < 0.0001f)
    return std::nullopt;

  right = right.Normalized();
  const Common::Vec3 corrected_up = look.Cross(right).Normalized();

  Common::Matrix44 matrix = Common::Matrix44::Identity();
  matrix.data[0] = right.x;
  matrix.data[1] = right.y;
  matrix.data[2] = right.z;
  matrix.data[3] = -right.Dot(eye);
  matrix.data[4] = corrected_up.x;
  matrix.data[5] = corrected_up.y;
  matrix.data[6] = corrected_up.z;
  matrix.data[7] = -corrected_up.Dot(eye);
  matrix.data[8] = look.x;
  matrix.data[9] = look.y;
  matrix.data[10] = look.z;
  matrix.data[11] = -look.Dot(eye);
  return matrix;
}

std::optional<Common::Vec3> GetEyeFromViewMatrix(const Common::Matrix44& view_matrix)
{
  const float determinant = view_matrix.Determinant();
  if (!std::isfinite(determinant) || std::abs(determinant) < 0.0001f)
    return std::nullopt;

  const Common::Vec3 eye = view_matrix.Inverted().Transform(Common::Vec3{}, 1.0f);
  if (!std::isfinite(eye.x) || !std::isfinite(eye.y) || !std::isfinite(eye.z))
    return std::nullopt;

  return eye;
}

std::optional<Common::Vec3> GetUpFromViewMatrix(const Common::Matrix44& view_matrix)
{
  Common::Vec3 up{view_matrix.data[4], view_matrix.data[5], view_matrix.data[6]};
  if (!std::isfinite(up.x) || !std::isfinite(up.y) || !std::isfinite(up.z) ||
      up.Length() < 0.0001f)
  {
    return std::nullopt;
  }

  return up.Normalized();
}

bool IsUsableMatrix(const Common::Matrix44& matrix)
{
  for (float value : matrix.data)
  {
    if (!std::isfinite(value))
      return false;
  }

  const float determinant = matrix.Determinant();
  return std::isfinite(determinant) && std::abs(determinant) >= 0.0001f;
}

std::optional<Common::Matrix44> ApplyViewDelta(const Common::Matrix44& current_matrix,
                                               const Common::Matrix44& previous_matrix,
                                               const Common::Matrix44& target_view)
{
  if (!IsUsableMatrix(current_matrix) || !IsUsableMatrix(previous_matrix) ||
      !IsUsableMatrix(target_view))
  {
    return std::nullopt;
  }

  Common::Matrix44 adjusted_view = current_matrix * previous_matrix.Inverted() * target_view;
  if (!IsUsableMatrix(adjusted_view))
    return std::nullopt;

  return adjusted_view;
}

std::optional<Common::Matrix44> GetFocusTargetMatrix(const FocusTargetSample& sample,
                                                     const Common::Vec3& orbit_offset,
                                                     const Common::Vec3& up)
{
  if (!sample.game_view_matrix || !sample.camera_eye || !sample.camera_center)
    return std::nullopt;

  if (orbit_offset.Length() < 0.0001f)
    return std::nullopt;

  const Common::Vec3 desired_eye = sample.target + orbit_offset;
  const Common::Vec3 desired_center = sample.track_target.value_or(sample.target);
  const std::optional<Common::Matrix44> desired_view =
      BuildLookAtMatrix(desired_eye, desired_center, up);
  if (!desired_view)
    return std::nullopt;

  const float determinant = sample.game_view_matrix->Determinant();
  if (!std::isfinite(determinant) || std::abs(determinant) < 0.0001f)
    return std::nullopt;

  Common::Matrix44 focus_matrix = *desired_view * sample.game_view_matrix->Inverted();
  for (float value : focus_matrix.data)
  {
    if (!std::isfinite(value))
      return std::nullopt;
  }

  return focus_matrix;
}

std::optional<Common::Matrix44> BuildViewFromBasisAndEye(const Common::Matrix44& reference_view,
                                                         const Common::Vec3& eye)
{
  Common::Vec3 right{reference_view.data[0], reference_view.data[1], reference_view.data[2]};
  Common::Vec3 up{reference_view.data[4], reference_view.data[5], reference_view.data[6]};
  Common::Vec3 look{reference_view.data[8], reference_view.data[9], reference_view.data[10]};
  if (!std::isfinite(right.x) || !std::isfinite(right.y) || !std::isfinite(right.z) ||
      !std::isfinite(up.x) || !std::isfinite(up.y) || !std::isfinite(up.z) ||
      !std::isfinite(look.x) || !std::isfinite(look.y) || !std::isfinite(look.z) ||
      right.Length() < 0.0001f || up.Length() < 0.0001f || look.Length() < 0.0001f)
  {
    return std::nullopt;
  }

  right = right.Normalized();
  look = look.Normalized();
  up = look.Cross(right);
  if (up.Length() < 0.0001f)
    return std::nullopt;

  up = up.Normalized();
  right = up.Cross(look);
  if (right.Length() < 0.0001f)
    return std::nullopt;

  right = right.Normalized();

  Common::Matrix44 view = Common::Matrix44::Identity();
  view.data[0] = right.x;
  view.data[1] = right.y;
  view.data[2] = right.z;
  view.data[3] = -right.Dot(eye);
  view.data[4] = up.x;
  view.data[5] = up.y;
  view.data[6] = up.z;
  view.data[7] = -up.Dot(eye);
  view.data[8] = look.x;
  view.data[9] = look.y;
  view.data[10] = look.z;
  view.data[11] = -look.Dot(eye);
  return view;
}

std::optional<Common::Matrix44> GetFixedRotationFocusMatrix(
    const FocusTargetSample& sample, const Common::Matrix44& reference_view,
    const Common::Vec3& target_eye_offset)
{
  if (!sample.game_view_matrix)
    return std::nullopt;

  if (target_eye_offset.Length() < 0.0001f)
    return std::nullopt;

  const Common::Vec3 desired_eye = sample.target + target_eye_offset;
  const std::optional<Common::Matrix44> desired_view =
      BuildViewFromBasisAndEye(reference_view, desired_eye);
  if (!desired_view)
    return std::nullopt;

  const float determinant = sample.game_view_matrix->Determinant();
  if (!std::isfinite(determinant) || std::abs(determinant) < 0.0001f)
    return std::nullopt;

  Common::Matrix44 focus_matrix = *desired_view * sample.game_view_matrix->Inverted();
  for (float value : focus_matrix.data)
  {
    if (!std::isfinite(value))
      return std::nullopt;
  }

  return focus_matrix;
}

Common::Vec3 CatmullRom(const Common::Vec3& p0, const Common::Vec3& p1, const Common::Vec3& p2,
                        const Common::Vec3& p3, float t)
{
  const float tt = t * t;
  const float ttt = tt * t;
  return ((p1 * 2.0f) +
          (p2 - p0) * t +
          (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * tt +
          (-p0 + p1 * 3.0f - p2 * 3.0f + p3) * ttt) *
         0.5f;
}

std::string GetKeyframePath()
{
  std::string path = Config::Get(Config::FL1_KEYFRAME_PATH_FILE);
  if (path.empty())
    path = "freelook_camera_path.fkf";

  std::filesystem::path configured_path(path);
  if (configured_path.is_relative())
  {
    configured_path = std::filesystem::path(File::GetUserPath(D_CONFIG_IDX)) / "FreeLook" /
                      configured_path;
  }

  return configured_path.string();
}

std::string GetKeyframeDraftPath()
{
  const std::filesystem::path draft_path =
      std::filesystem::path(File::GetUserPath(D_CONFIG_IDX)) / "FreeLook" /
      "freelook_camera_draft.fkf";
  return draft_path.string();
}

bool IsMoviePlaybackActive()
{
  auto& system = Core::System::GetInstance();
  return !Core::IsUninitialized(system) && system.GetMovie().IsPlayingInput();
}

uint64_t GetCurrentMovieFrame()
{
  auto& system = Core::System::GetInstance();
  return system.GetMovie().GetCurrentFrame();
}

void SaveFocusSettings(Common::IniFile::Section* meta)
{
  if (!meta)
    return;

  meta->Set("FocusTargetEnabled", Config::Get(Config::FL1_FOCUS_TARGET_ENABLED));
  meta->Set("FocusTargetAddressX", Config::Get(Config::FL1_FOCUS_TARGET_ADDRESS_X));
  meta->Set("FocusTargetAddressY", Config::Get(Config::FL1_FOCUS_TARGET_ADDRESS_Y));
  meta->Set("FocusTargetAddressZ", Config::Get(Config::FL1_FOCUS_TARGET_ADDRESS_Z));
  meta->Set("FocusTargetCustomCoordsEnabled",
            Config::Get(Config::FL1_FOCUS_TARGET_CUSTOM_COORDS_ENABLED));
  meta->Set("FocusTargetCustomX", Config::Get(Config::FL1_FOCUS_TARGET_CUSTOM_X));
  meta->Set("FocusTargetCustomY", Config::Get(Config::FL1_FOCUS_TARGET_CUSTOM_Y));
  meta->Set("FocusTargetCustomZ", Config::Get(Config::FL1_FOCUS_TARGET_CUSTOM_Z));
  meta->Set("FocusTargetOffsetX", Config::Get(Config::FL1_FOCUS_TARGET_OFFSET_X));
  meta->Set("FocusTargetOffsetY", Config::Get(Config::FL1_FOCUS_TARGET_OFFSET_Y));
  meta->Set("FocusTargetOffsetZ", Config::Get(Config::FL1_FOCUS_TARGET_OFFSET_Z));
  meta->Set("FocusTargetFixedRotation", Config::Get(Config::FL1_FOCUS_TARGET_FIXED_ROTATION));
  meta->Set("TrackTargetEnabled", Config::Get(Config::FL1_TRACK_TARGET_ENABLED));
  meta->Set("TrackTargetAddressX", Config::Get(Config::FL1_TRACK_TARGET_ADDRESS_X));
  meta->Set("TrackTargetAddressY", Config::Get(Config::FL1_TRACK_TARGET_ADDRESS_Y));
  meta->Set("TrackTargetAddressZ", Config::Get(Config::FL1_TRACK_TARGET_ADDRESS_Z));
  meta->Set("TrackTargetCustomCoordsEnabled",
            Config::Get(Config::FL1_TRACK_TARGET_CUSTOM_COORDS_ENABLED));
  meta->Set("TrackTargetCustomX", Config::Get(Config::FL1_TRACK_TARGET_CUSTOM_X));
  meta->Set("TrackTargetCustomY", Config::Get(Config::FL1_TRACK_TARGET_CUSTOM_Y));
  meta->Set("TrackTargetCustomZ", Config::Get(Config::FL1_TRACK_TARGET_CUSTOM_Z));
  meta->Set("TrackTargetOffsetX", Config::Get(Config::FL1_TRACK_TARGET_OFFSET_X));
  meta->Set("TrackTargetOffsetY", Config::Get(Config::FL1_TRACK_TARGET_OFFSET_Y));
  meta->Set("TrackTargetOffsetZ", Config::Get(Config::FL1_TRACK_TARGET_OFFSET_Z));
}

void LoadFocusSettings(const Common::IniFile::Section& meta)
{
  bool bool_value = false;
  std::string string_value;

  if (meta.Get("FocusTargetEnabled", &bool_value, Config::Get(Config::FL1_FOCUS_TARGET_ENABLED)))
    Config::SetBaseOrCurrent(Config::FL1_FOCUS_TARGET_ENABLED, bool_value);
  if (meta.Get("FocusTargetAddressX", &string_value, Config::Get(Config::FL1_FOCUS_TARGET_ADDRESS_X)))
    Config::SetBaseOrCurrent(Config::FL1_FOCUS_TARGET_ADDRESS_X, string_value);
  if (meta.Get("FocusTargetAddressY", &string_value, Config::Get(Config::FL1_FOCUS_TARGET_ADDRESS_Y)))
    Config::SetBaseOrCurrent(Config::FL1_FOCUS_TARGET_ADDRESS_Y, string_value);
  if (meta.Get("FocusTargetAddressZ", &string_value, Config::Get(Config::FL1_FOCUS_TARGET_ADDRESS_Z)))
    Config::SetBaseOrCurrent(Config::FL1_FOCUS_TARGET_ADDRESS_Z, string_value);
  if (meta.Get("FocusTargetCustomCoordsEnabled", &bool_value,
               Config::Get(Config::FL1_FOCUS_TARGET_CUSTOM_COORDS_ENABLED)))
    Config::SetBaseOrCurrent(Config::FL1_FOCUS_TARGET_CUSTOM_COORDS_ENABLED, bool_value);
  if (meta.Get("FocusTargetCustomX", &string_value, Config::Get(Config::FL1_FOCUS_TARGET_CUSTOM_X)))
    Config::SetBaseOrCurrent(Config::FL1_FOCUS_TARGET_CUSTOM_X, string_value);
  if (meta.Get("FocusTargetCustomY", &string_value, Config::Get(Config::FL1_FOCUS_TARGET_CUSTOM_Y)))
    Config::SetBaseOrCurrent(Config::FL1_FOCUS_TARGET_CUSTOM_Y, string_value);
  if (meta.Get("FocusTargetCustomZ", &string_value, Config::Get(Config::FL1_FOCUS_TARGET_CUSTOM_Z)))
    Config::SetBaseOrCurrent(Config::FL1_FOCUS_TARGET_CUSTOM_Z, string_value);
  if (meta.Get("FocusTargetOffsetX", &string_value, Config::Get(Config::FL1_FOCUS_TARGET_OFFSET_X)))
    Config::SetBaseOrCurrent(Config::FL1_FOCUS_TARGET_OFFSET_X, string_value);
  if (meta.Get("FocusTargetOffsetY", &string_value, Config::Get(Config::FL1_FOCUS_TARGET_OFFSET_Y)))
    Config::SetBaseOrCurrent(Config::FL1_FOCUS_TARGET_OFFSET_Y, string_value);
  if (meta.Get("FocusTargetOffsetZ", &string_value, Config::Get(Config::FL1_FOCUS_TARGET_OFFSET_Z)))
    Config::SetBaseOrCurrent(Config::FL1_FOCUS_TARGET_OFFSET_Z, string_value);
  if (meta.Get("FocusTargetFixedRotation", &bool_value,
               Config::Get(Config::FL1_FOCUS_TARGET_FIXED_ROTATION)))
    Config::SetBaseOrCurrent(Config::FL1_FOCUS_TARGET_FIXED_ROTATION, bool_value);
  if (meta.Get("TrackTargetEnabled", &bool_value, Config::Get(Config::FL1_TRACK_TARGET_ENABLED)))
    Config::SetBaseOrCurrent(Config::FL1_TRACK_TARGET_ENABLED, bool_value);
  if (meta.Get("TrackTargetAddressX", &string_value, Config::Get(Config::FL1_TRACK_TARGET_ADDRESS_X)))
    Config::SetBaseOrCurrent(Config::FL1_TRACK_TARGET_ADDRESS_X, string_value);
  if (meta.Get("TrackTargetAddressY", &string_value, Config::Get(Config::FL1_TRACK_TARGET_ADDRESS_Y)))
    Config::SetBaseOrCurrent(Config::FL1_TRACK_TARGET_ADDRESS_Y, string_value);
  if (meta.Get("TrackTargetAddressZ", &string_value, Config::Get(Config::FL1_TRACK_TARGET_ADDRESS_Z)))
    Config::SetBaseOrCurrent(Config::FL1_TRACK_TARGET_ADDRESS_Z, string_value);
  if (meta.Get("TrackTargetCustomCoordsEnabled", &bool_value,
               Config::Get(Config::FL1_TRACK_TARGET_CUSTOM_COORDS_ENABLED)))
    Config::SetBaseOrCurrent(Config::FL1_TRACK_TARGET_CUSTOM_COORDS_ENABLED, bool_value);
  if (meta.Get("TrackTargetCustomX", &string_value, Config::Get(Config::FL1_TRACK_TARGET_CUSTOM_X)))
    Config::SetBaseOrCurrent(Config::FL1_TRACK_TARGET_CUSTOM_X, string_value);
  if (meta.Get("TrackTargetCustomY", &string_value, Config::Get(Config::FL1_TRACK_TARGET_CUSTOM_Y)))
    Config::SetBaseOrCurrent(Config::FL1_TRACK_TARGET_CUSTOM_Y, string_value);
  if (meta.Get("TrackTargetCustomZ", &string_value, Config::Get(Config::FL1_TRACK_TARGET_CUSTOM_Z)))
    Config::SetBaseOrCurrent(Config::FL1_TRACK_TARGET_CUSTOM_Z, string_value);
  if (meta.Get("TrackTargetOffsetX", &string_value, Config::Get(Config::FL1_TRACK_TARGET_OFFSET_X)))
    Config::SetBaseOrCurrent(Config::FL1_TRACK_TARGET_OFFSET_X, string_value);
  if (meta.Get("TrackTargetOffsetY", &string_value, Config::Get(Config::FL1_TRACK_TARGET_OFFSET_Y)))
    Config::SetBaseOrCurrent(Config::FL1_TRACK_TARGET_OFFSET_Y, string_value);
  if (meta.Get("TrackTargetOffsetZ", &string_value, Config::Get(Config::FL1_TRACK_TARGET_OFFSET_Z)))
    Config::SetBaseOrCurrent(Config::FL1_TRACK_TARGET_OFFSET_Z, string_value);
}

class KeyframePathController : public CameraControllerInput
{
public:
  bool SupportsKeyframeAnimation() const override { return true; }
  std::size_t GetKeyframeCount() const override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_keyframes.size();
  }
  bool IsKeyframePlaybackActive() const override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_keyframe_playback;
  }

  void AddKeyframe() override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_mutex);
    Keyframe keyframe = CaptureCurrentKeyframe();
    if (const std::optional<Keyframe> focus_keyframe = CaptureCurrentFocusKeyframe())
      keyframe = *focus_keyframe;

    const int frame_step = std::max(1, Config::Get(Config::FL1_KEYFRAME_DEFAULT_FRAME_STEP));
    if (Config::Get(Config::FL1_KEYFRAME_SYNC_TO_TAS) && IsMoviePlaybackActive())
      keyframe.frame = GetCurrentMovieFrame();
    else if (!m_keyframes.empty())
      keyframe.frame = m_keyframes.back().frame + static_cast<uint64_t>(frame_step);
    else
      keyframe.frame = 0;

    m_keyframes.push_back(keyframe);
    SortKeyframes();
    MarkDirty();
  }

  void DeleteLastKeyframe() override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_keyframes.empty())
      return;

    m_keyframes.pop_back();

    if (m_keyframes.size() < 2)
    {
      m_keyframe_playback = false;
      m_playback_frame = 0.0f;
    }

    MarkDirty();
  }

  void ClearKeyframes() override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_keyframes.clear();
    StopKeyframePlayback();
  }

  void ToggleKeyframePlayback() override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (m_keyframes.size() < 2)
    {
      if ((!LoadKeyframesFromDraftFile() && !LoadKeyframesFromFile()) || m_keyframes.size() < 2)
        return;
    }

    m_keyframe_playback = !m_keyframe_playback;
    m_playback_frame = static_cast<float>(m_keyframes.front().frame);
    MarkDirty();
  }

  bool SaveKeyframesToFile() override
  {
    return SaveKeyframesToPath(GetKeyframePath());
  }

  bool LoadKeyframesFromFile() override
  {
    return LoadKeyframesFromPath(GetKeyframePath());
  }

  bool SaveKeyframesToDraftFile() override
  {
    return SaveKeyframesToPath(GetKeyframeDraftPath());
  }

  bool LoadKeyframesFromDraftFile() override
  {
    return LoadKeyframesFromPath(GetKeyframeDraftPath());
  }

  void AdvanceKeyframePlayback(float dt) override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (!m_keyframe_playback || m_keyframes.size() < 2)
    {
      m_last_applied_keyframe_frame.reset();
      return;
    }

    const bool sync_to_tas =
        Config::Get(Config::FL1_KEYFRAME_SYNC_TO_TAS) && IsMoviePlaybackActive();
    float frame = m_playback_frame;
    if (sync_to_tas)
    {
      frame = static_cast<float>(GetCurrentMovieFrame());
    }
    else
    {
      m_playback_frame += std::max(dt, 0.0f) * 60.0f;
      frame = m_playback_frame;
    }

    if (m_last_applied_keyframe_frame &&
        std::abs(frame - *m_last_applied_keyframe_frame) <= 0.001f)
    {
      return;
    }

    ApplyAtFrame(frame);
    m_focus_keyframe_transform_changed = true;
    m_last_applied_keyframe_frame = frame;
  }

protected:
  bool SaveKeyframesToPath(const std::string& path)
  {
    const std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (path.empty())
      return false;

    if (!File::CreateFullPath(path))
      return false;

    Common::IniFile ini;
    auto* meta = ini.GetOrCreateSection("Path");
    meta->Set("Version", 2);
    meta->Set("Count", static_cast<int>(m_keyframes.size()));
    meta->Set("InterpolationMode", static_cast<int>(m_interpolation_mode));
    meta->Set("DefaultFrameStep", Config::Get(Config::FL1_KEYFRAME_DEFAULT_FRAME_STEP));
    meta->Set("SyncToMoviePlayback", Config::Get(Config::FL1_KEYFRAME_SYNC_TO_TAS));
    meta->Set("FocusRelative", Config::Get(Config::FL1_FOCUS_TARGET_ENABLED));
    SaveFocusSettings(meta);

    for (std::size_t i = 0; i < m_keyframes.size(); ++i)
    {
      const auto& keyframe = m_keyframes[i];
      auto* section = ini.GetOrCreateSection(fmt::format("Keyframe{}", i));
      section->Set("Frame", keyframe.frame);
      section->Set("PositionX", keyframe.position.x);
      section->Set("PositionY", keyframe.position.y);
      section->Set("PositionZ", keyframe.position.z);
      section->Set("RotationX", keyframe.rotation.data.x);
      section->Set("RotationY", keyframe.rotation.data.y);
      section->Set("RotationZ", keyframe.rotation.data.z);
      section->Set("RotationW", keyframe.rotation.data.w);
      section->Set("EulerRotationX",
                   keyframe.euler_rotation.x * 180.0f / std::numbers::pi_v<float>);
      section->Set("EulerRotationY",
                   keyframe.euler_rotation.y * 180.0f / std::numbers::pi_v<float>);
      section->Set("EulerRotationZ",
                   keyframe.euler_rotation.z * 180.0f / std::numbers::pi_v<float>);
      section->Set("FovX", keyframe.fov.x);
      section->Set("FovY", keyframe.fov.y);
    }

    const bool saved = ini.Save(path);
    if (saved)
      UpdateKeyframeFileTimestamp();
    return saved;
  }

  bool LoadKeyframesFromPath(const std::string& path)
  {
    const std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!File::Exists(path))
      return false;

    Common::IniFile ini;
    if (!ini.Load(path))
      return false;

    const auto* meta = ini.GetSection("Path");
    if (!meta)
      return false;

    int count = 0;
    int interpolation_mode = static_cast<int>(m_interpolation_mode);
    meta->Get("Count", &count, 0);
    meta->Get("InterpolationMode", &interpolation_mode, interpolation_mode);
    m_interpolation_mode =
        static_cast<KeyframeInterpolationMode>(std::clamp(interpolation_mode, 0, 2));
    meta->Get("FocusRelative", &m_focus_relative_keyframes, false);
    LoadFocusSettings(*meta);

    int frame_step = Config::Get(Config::FL1_KEYFRAME_DEFAULT_FRAME_STEP);
    bool sync_to_tas = Config::Get(Config::FL1_KEYFRAME_SYNC_TO_TAS);
    meta->Get("DefaultFrameStep", &frame_step, frame_step);
    meta->Get("SyncToMoviePlayback", &sync_to_tas, sync_to_tas);
    Config::SetBaseOrCurrent(Config::FL1_KEYFRAME_DEFAULT_FRAME_STEP,
                             std::clamp(frame_step, 1, 600));
    Config::SetBaseOrCurrent(Config::FL1_KEYFRAME_SYNC_TO_TAS, sync_to_tas);

    std::vector<Keyframe> loaded;
    loaded.reserve(std::max(0, count));
    for (int i = 0; i < count; ++i)
    {
      const auto* section = ini.GetSection(fmt::format("Keyframe{}", i));
      if (!section)
        continue;

      Keyframe keyframe{};
      float qx = 0.0f;
      float qy = 0.0f;
      float qz = 0.0f;
      float qw = 1.0f;
      section->Get("Frame", &keyframe.frame, uint64_t{0});
      section->Get("PositionX", &keyframe.position.x, 0.0f);
      section->Get("PositionY", &keyframe.position.y, 0.0f);
      section->Get("PositionZ", &keyframe.position.z, 0.0f);
      section->Get("RotationX", &qx, 0.0f);
      section->Get("RotationY", &qy, 0.0f);
      section->Get("RotationZ", &qz, 0.0f);
      section->Get("RotationW", &qw, 1.0f);
      section->Get("FovX", &keyframe.fov.x, 1.0f);
      section->Get("FovY", &keyframe.fov.y, 1.0f);

      keyframe.rotation = Common::Quaternion(qw, qx, qy, qz).Normalized();
      float euler_x_deg = 0.0f;
      float euler_y_deg = 0.0f;
      float euler_z_deg = 0.0f;
      const bool has_euler_x = section->Get("EulerRotationX", &euler_x_deg, 0.0f);
      const bool has_euler_y = section->Get("EulerRotationY", &euler_y_deg, 0.0f);
      const bool has_euler_z = section->Get("EulerRotationZ", &euler_z_deg, 0.0f);
      if (has_euler_x && has_euler_y && has_euler_z)
      {
        keyframe.euler_rotation =
            Common::Vec3{euler_x_deg * std::numbers::pi_v<float> / 180.0f,
                         euler_y_deg * std::numbers::pi_v<float> / 180.0f,
                         euler_z_deg * std::numbers::pi_v<float> / 180.0f};
      }
      else
      {
        keyframe.euler_rotation = Common::FromQuaternionToEuler(keyframe.rotation);
      }
      loaded.push_back(keyframe);
    }

    if (loaded.empty())
    {
      m_keyframes.clear();
      StopKeyframePlayback();
      UpdateKeyframeFileTimestamp();
      return true;
    }

    const bool was_playing = m_keyframe_playback;
    const float old_playback_frame = m_playback_frame;

    m_keyframes = std::move(loaded);
    SortKeyframes();

    if (was_playing && m_keyframes.size() >= 2)
    {
      m_keyframe_playback = true;
      const float first_frame = static_cast<float>(m_keyframes.front().frame);
      const float last_frame = static_cast<float>(m_keyframes.back().frame);
      m_playback_frame = std::clamp(old_playback_frame, first_frame, last_frame);
      ApplyAtFrame(m_playback_frame);
    }
    else
    {
      m_keyframe_playback = false;
      m_playback_frame = 0.0f;
    }

    UpdateKeyframeFileTimestamp();
    MarkDirty();
    return true;
  }

  void UpdateFocusTarget() override
  {
    std::optional<FocusTargetSample> sample = GetCachedFocusTarget();
    if (!sample)
      return;

    const std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!sample->game_view_matrix || !sample->camera_eye || !sample->camera_center ||
        !sample->camera_up)
    {
      m_focus_orbit_offset.reset();
      m_focus_up_vector.reset();
      m_focus_fixed_final_view.reset();
      m_focus_last_applied_matrix.reset();
      m_focus_manual_transform_changed = false;
      return;
    }

    const bool fixed_rotation = Config::Get(Config::FL1_FOCUS_TARGET_FIXED_ROTATION);
    const bool fixed_rotation_changed =
        !m_focus_last_fixed_rotation || *m_focus_last_fixed_rotation != fixed_rotation;
    m_focus_last_fixed_rotation = fixed_rotation;

    const bool new_sample = sample->sequence != m_last_focus_sample_sequence;
    const bool manual_transform_changed = m_focus_manual_transform_changed.exchange(false);
    const bool keyframe_transform_changed = m_focus_keyframe_transform_changed.exchange(false);
    const std::optional<Keyframe> pending_focus_keyframe = m_pending_focus_keyframe;
    m_pending_focus_keyframe.reset();
    if (!new_sample && !manual_transform_changed && !keyframe_transform_changed &&
        !fixed_rotation_changed && !pending_focus_keyframe && m_focus_has_applied)
    {
      return;
    }

    m_last_focus_sample_sequence = sample->sequence;

    const Common::Matrix44 current_matrix = GetView();

    if (pending_focus_keyframe)
    {
      m_focus_up_vector.reset();
      const Common::Vec3 desired_eye = sample->target + pending_focus_keyframe->position;
      const std::optional<Common::Matrix44> desired_view =
          !fixed_rotation && sample->track_target ?
              BuildLookAtMatrix(desired_eye, *sample->track_target,
                                Common::Vec3{0.0f, 1.0f, 0.0f}) :
              BuildViewFromBasisAndEye(
                  MatrixFromKeyframeTransform(Common::Vec3{}, pending_focus_keyframe->rotation),
                  desired_eye);
      if (!desired_view)
        return;

      m_focus_fixed_final_view = *desired_view;
      m_focus_orbit_offset = pending_focus_keyframe->position;
    }
    else if (fixed_rotation)
    {
      m_focus_up_vector.reset();
      const bool initialize_locked_pose =
          fixed_rotation_changed || !m_focus_fixed_final_view || !m_focus_orbit_offset;
      if (initialize_locked_pose)
      {
        const Common::Matrix44 composed_view = current_matrix * *sample->game_view_matrix;
        const std::optional<Common::Vec3> current_eye = GetEyeFromViewMatrix(composed_view);
        if (!current_eye)
          return;

        m_focus_fixed_final_view = composed_view;
        m_focus_orbit_offset = *current_eye - sample->target;
      }
      else if (manual_transform_changed && m_focus_last_applied_matrix)
      {
        const std::optional<Common::Matrix44> adjusted_view =
            ApplyViewDelta(current_matrix, *m_focus_last_applied_matrix, *m_focus_fixed_final_view);
        if (adjusted_view)
        {
          const std::optional<Common::Vec3> current_eye = GetEyeFromViewMatrix(*adjusted_view);
          if (!current_eye)
            return;

          m_focus_fixed_final_view = *adjusted_view;
          m_focus_orbit_offset = *current_eye - sample->target;
        }
      }
      else if (keyframe_transform_changed)
      {
        const Common::Matrix44 composed_view = current_matrix * *sample->game_view_matrix;
        const std::optional<Common::Vec3> current_eye = GetEyeFromViewMatrix(composed_view);
        if (!current_eye)
          return;

        m_focus_fixed_final_view = composed_view;
        m_focus_orbit_offset = *current_eye - sample->target;
      }
    }
    else
    {
      m_focus_fixed_final_view.reset();

      const bool refresh_orbit = !m_focus_orbit_offset || manual_transform_changed ||
                                 keyframe_transform_changed || fixed_rotation_changed;
      if (refresh_orbit)
      {
        const std::optional<Common::Matrix44> current_locked_view =
            m_focus_orbit_offset && m_focus_up_vector ?
                BuildLookAtMatrix(sample->target + *m_focus_orbit_offset, sample->target,
                                  *m_focus_up_vector) :
                std::nullopt;
        const std::optional<Common::Matrix44> adjusted_view =
            manual_transform_changed && m_focus_last_applied_matrix && current_locked_view ?
                ApplyViewDelta(current_matrix, *m_focus_last_applied_matrix,
                               *current_locked_view) :
                std::nullopt;
        const Common::Matrix44 current_view =
            adjusted_view.value_or(current_matrix * *sample->game_view_matrix);
        const std::optional<Common::Vec3> current_eye = GetEyeFromViewMatrix(current_view);
        m_focus_orbit_offset = current_eye ? *current_eye - sample->target :
                                             *sample->camera_eye - *sample->camera_center;
        m_focus_up_vector = GetUpFromViewMatrix(current_view).value_or(*sample->camera_up);
      }
    }

    const std::optional<Common::Matrix44> focus_matrix =
        (fixed_rotation || pending_focus_keyframe) && m_focus_fixed_final_view &&
                m_focus_orbit_offset ?
            GetFixedRotationFocusMatrix(*sample, *m_focus_fixed_final_view,
                                        *m_focus_orbit_offset) :
            (m_focus_orbit_offset ?
                 GetFocusTargetMatrix(*sample, *m_focus_orbit_offset,
                                      m_focus_up_vector.value_or(*sample->camera_up)) :
                 std::nullopt);
    if (!focus_matrix)
      return;

    ApplyFocusMatrix(*focus_matrix);
    m_focus_last_applied_matrix = *focus_matrix;
    m_focus_has_applied = true;
    MarkDirty();
  }

  std::vector<KeyframeData> GetKeyframes() const override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<KeyframeData> keyframes;
    keyframes.reserve(m_keyframes.size());

    for (const Keyframe& keyframe : m_keyframes)
    {
      KeyframeData data;
      data.frame = keyframe.frame;
      data.position = keyframe.position;
      data.rotation = keyframe.euler_rotation;
      data.fov = keyframe.fov;
      keyframes.push_back(data);
    }

    return keyframes;
  }

  bool GetKeyframe(std::size_t index, KeyframeData* out_keyframe) const override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!out_keyframe || index >= m_keyframes.size())
      return false;

    out_keyframe->frame = m_keyframes[index].frame;
    out_keyframe->position = m_keyframes[index].position;
    out_keyframe->rotation = m_keyframes[index].euler_rotation;
    out_keyframe->fov = m_keyframes[index].fov;
    return true;
  }

  bool UpdateKeyframe(std::size_t index, const KeyframeData& keyframe) override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (index >= m_keyframes.size())
      return false;

    m_keyframes[index].frame = keyframe.frame;
    m_keyframes[index].position = keyframe.position;
    m_keyframes[index].euler_rotation = keyframe.rotation;
    m_keyframes[index].rotation = Common::Quaternion::RotateXYZ(keyframe.rotation).Normalized();
    m_keyframes[index].fov = Common::Vec2{std::max(keyframe.fov.x, 0.025f),
                                          std::max(keyframe.fov.y, 0.025f)};

    SortKeyframes();
    MarkDirty();
    return true;
  }

  bool DeleteKeyframe(std::size_t index) override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (index >= m_keyframes.size())
      return false;

    m_keyframes.erase(m_keyframes.begin() + static_cast<std::ptrdiff_t>(index));
    if (m_keyframes.size() < 2)
    {
      m_keyframe_playback = false;
      m_playback_frame = 0.0f;
    }

    MarkDirty();
    return true;
  }

  bool ReplaceKeyframes(std::vector<KeyframeData> keyframes,
                        KeyframeInterpolationMode interpolation_mode,
                        bool focus_relative) override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_keyframes.clear();
    m_keyframes.reserve(keyframes.size());

    for (const KeyframeData& data : keyframes)
    {
      Keyframe keyframe;
      keyframe.frame = data.frame;
      keyframe.position = data.position;
      keyframe.euler_rotation = data.rotation;
      keyframe.rotation = Common::Quaternion::RotateXYZ(data.rotation).Normalized();
      keyframe.fov = Common::Vec2{std::max(data.fov.x, 0.025f), std::max(data.fov.y, 0.025f)};
      m_keyframes.push_back(keyframe);
    }

    m_interpolation_mode = interpolation_mode;
    m_focus_relative_keyframes = focus_relative;
    SortKeyframes();

    if (m_keyframes.size() < 2)
      StopKeyframePlayback();
    else if (m_keyframe_playback)
      m_playback_frame = std::clamp(m_playback_frame, static_cast<float>(m_keyframes.front().frame),
                                    static_cast<float>(m_keyframes.back().frame));

    m_last_applied_keyframe_frame.reset();
    MarkDirty();
    return true;
  }

  KeyframeData GetCurrentCameraTransform() const override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_mutex);
    KeyframeData current = ToKeyframeData(CaptureCurrentFocusKeyframe().value_or(CaptureCurrentKeyframe()));
    if (Config::Get(Config::FL1_KEYFRAME_SYNC_TO_TAS) && IsMoviePlaybackActive())
      current.frame = GetCurrentMovieFrame();
    return current;
  }

  KeyframeInterpolationMode GetKeyframeInterpolationMode() const override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_interpolation_mode;
  }

  void SetKeyframeInterpolationMode(KeyframeInterpolationMode mode) override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_interpolation_mode = mode;
    MarkDirty();
  }

  void DoState(PointerWrap& p) override
  {
    CameraControllerInput::DoState(p);
  }

protected:
  struct Keyframe
  {
    uint64_t frame = 0;
    Common::Vec3 position = Common::Vec3{};
    Common::Quaternion rotation = Common::Quaternion::Identity();
    Common::Vec3 euler_rotation = Common::Vec3{};
    Common::Vec2 fov = Common::Vec2{1.0f, 1.0f};
  };

  void StopKeyframePlayback()
  {
    m_keyframe_playback = false;
    m_playback_frame = 0.0f;
    m_last_applied_keyframe_frame.reset();
    m_pending_focus_keyframe.reset();
    MarkDirty();
  }

  void MarkFocusManualTransformChanged() { m_focus_manual_transform_changed = true; }

  void ResetFocusOrbit()
  {
    m_focus_orbit_offset.reset();
    m_focus_up_vector.reset();
    m_focus_fixed_final_view.reset();
    m_focus_last_applied_matrix.reset();
    m_focus_last_fixed_rotation.reset();
    m_last_focus_sample_sequence = 0;
    m_focus_has_applied = false;
    m_focus_manual_transform_changed = false;
    m_focus_keyframe_transform_changed = false;
  }

private:
  virtual Keyframe CaptureCurrentKeyframe() const = 0;
  virtual void ApplyKeyframeTransform(const Keyframe& keyframe) = 0;

  std::optional<Keyframe> CaptureCurrentFocusKeyframe() const
  {
    const std::optional<FocusTargetSample> sample = GetCachedFocusTarget();
    if (!sample || !sample->game_view_matrix || !Config::Get(Config::FL1_FOCUS_TARGET_ENABLED))
      return std::nullopt;

    const Common::Matrix44 current_view = GetView() * *sample->game_view_matrix;
    const std::optional<Common::Vec3> current_eye = GetEyeFromViewMatrix(current_view);
    if (!current_eye)
      return std::nullopt;

    Keyframe keyframe;
    keyframe.position = *current_eye - sample->target;
    keyframe.rotation = QuaternionFromMatrix(current_view);
    keyframe.euler_rotation = Common::FromQuaternionToEuler(keyframe.rotation);
    keyframe.fov = GetFieldOfViewMultiplier();
    return keyframe;
  }

  virtual void ApplyFocusMatrix(const Common::Matrix44& matrix)
  {
    Keyframe keyframe;
    keyframe.position = Common::Vec3{matrix.data[3], matrix.data[7], matrix.data[11]};
    keyframe.rotation = QuaternionFromMatrix(matrix);
    keyframe.euler_rotation = Common::FromQuaternionToEuler(keyframe.rotation);
    keyframe.fov = GetFieldOfViewMultiplier();
    ApplyKeyframeTransform(keyframe);
  }

  void UpdateKeyframeFileTimestamp()
  {
    const std::string path = GetKeyframePath();
    std::error_code ec;
    const auto timestamp = std::filesystem::last_write_time(path, ec);
    if (ec)
      return;

    m_last_loaded_timestamp = timestamp;
    m_has_loaded_timestamp = true;
  }

  KeyframeData ToKeyframeData(const Keyframe& keyframe) const
  {
    KeyframeData data;
    data.frame = keyframe.frame;
    data.position = keyframe.position;
    data.rotation = keyframe.euler_rotation;
    data.fov = keyframe.fov;
    return data;
  }

  void SortKeyframes()
  {
    std::sort(m_keyframes.begin(), m_keyframes.end(),
              [](const Keyframe& lhs, const Keyframe& rhs) { return lhs.frame < rhs.frame; });
  }

  void ApplyKeyframe(const Keyframe& keyframe)
  {
    if (m_focus_relative_keyframes && Config::Get(Config::FL1_FOCUS_TARGET_ENABLED))
    {
      m_pending_focus_keyframe = keyframe;
      SetFieldOfViewMultiplier(keyframe.fov);
      MarkDirty();
      return;
    }

    ApplyKeyframeTransform(keyframe);
    SetFieldOfViewMultiplier(keyframe.fov);
    MarkDirty();
  }

  void ApplyAtFrame(float frame)
  {
    if (m_keyframes.empty())
      return;

    if (m_keyframes.size() == 1)
    {
      ApplyKeyframe(m_keyframes.front());
      return;
    }

    const float first_frame = static_cast<float>(m_keyframes.front().frame);
    const float last_frame = static_cast<float>(m_keyframes.back().frame);

    if (frame <= first_frame)
    {
      ApplyKeyframe(m_keyframes.front());
      return;
    }

    if (frame >= last_frame)
    {
      ApplyKeyframe(m_keyframes.back());
      return;
    }

    std::size_t segment_index = 0;
    for (std::size_t i = 0; i + 1 < m_keyframes.size(); ++i)
    {
      if (frame >= static_cast<float>(m_keyframes[i].frame) &&
          frame <= static_cast<float>(m_keyframes[i + 1].frame))
      {
        segment_index = i;
        break;
      }
    }

    const Keyframe& p0 = m_keyframes[segment_index == 0 ? 0 : segment_index - 1];
    const Keyframe& p1 = m_keyframes[segment_index];
    const Keyframe& p2 = m_keyframes[segment_index + 1];
    const Keyframe& p3 =
        m_keyframes[(segment_index + 2 < m_keyframes.size()) ? (segment_index + 2)
                                                              : (m_keyframes.size() - 1)];

    const float start = static_cast<float>(p1.frame);
    const float end = static_cast<float>(p2.frame);
    const float span = std::max(1.0f, end - start);
    const float t = Clamp01((frame - start) / span);

    Keyframe interpolated = p1;
    float interp_t = t;
    switch (m_interpolation_mode)
    {
    case KeyframeInterpolationMode::Linear:
      interpolated.position = p1.position + (p2.position - p1.position) * t;
      break;
    case KeyframeInterpolationMode::CatmullRom:
      interpolated.position = CatmullRom(p0.position, p1.position, p2.position, p3.position, t);
      break;
    case KeyframeInterpolationMode::SmoothStep:
      interp_t = SmoothStep(t);
      interpolated.position = p1.position + (p2.position - p1.position) * interp_t;
      break;
    }

    if (m_interpolation_mode == KeyframeInterpolationMode::SmoothStep)
      interp_t = SmoothStep(t);

    interpolated.rotation = NLerp(p1.rotation, p2.rotation, interp_t);
    interpolated.euler_rotation = Common::FromQuaternionToEuler(interpolated.rotation);
    interpolated.fov = Lerp(p1.fov, p2.fov, interp_t);
    ApplyKeyframe(interpolated);
  }

  std::vector<Keyframe> m_keyframes;
  bool m_keyframe_playback = false;
  bool m_focus_relative_keyframes = false;
  KeyframeInterpolationMode m_interpolation_mode = KeyframeInterpolationMode::CatmullRom;
  float m_playback_frame = 0.0f;
  bool m_has_loaded_timestamp = false;
  std::filesystem::file_time_type m_last_loaded_timestamp{};
  std::optional<Common::Vec3> m_focus_orbit_offset;
  std::optional<Common::Vec3> m_focus_up_vector;
  std::optional<Common::Matrix44> m_focus_fixed_final_view;
  std::optional<Common::Matrix44> m_focus_last_applied_matrix;
  std::optional<Keyframe> m_pending_focus_keyframe;
  std::optional<bool> m_focus_last_fixed_rotation;
  std::optional<float> m_last_applied_keyframe_frame;
  uint64_t m_last_focus_sample_sequence = 0;
  bool m_focus_has_applied = false;
  std::atomic_bool m_focus_manual_transform_changed = false;
  std::atomic_bool m_focus_keyframe_transform_changed = false;
  mutable std::recursive_mutex m_mutex;
};

class SixAxisController final : public KeyframePathController
{
public:
  SixAxisController() = default;

  Common::Matrix44 GetView() const override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_transform_mutex);
    return m_mat;
  }

  void MoveVertical(float amt) override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_transform_mutex);
    m_mat = Common::Matrix44::Translate(Common::Vec3{0, amt, 0}) * m_mat;
    MarkFocusManualTransformChanged();
    MarkDirty();
  }

  void MoveHorizontal(float amt) override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_transform_mutex);
    m_mat = Common::Matrix44::Translate(Common::Vec3{amt, 0, 0}) * m_mat;
    MarkFocusManualTransformChanged();
    MarkDirty();
  }

  void MoveForward(float amt) override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_transform_mutex);
    m_mat = Common::Matrix44::Translate(Common::Vec3{0, 0, amt}) * m_mat;
    MarkFocusManualTransformChanged();
    MarkDirty();
  }

  void Rotate(const Common::Vec3& amt) override { Rotate(Common::Quaternion::RotateXYZ(amt)); }

  void Rotate(const Common::Quaternion& quat) override
  {
    if (std::abs(quat.data.x) < 0.000001f && std::abs(quat.data.y) < 0.000001f &&
        std::abs(quat.data.z) < 0.000001f && std::abs(quat.data.w - 1.0f) < 0.000001f)
    {
      return;
    }

    const std::lock_guard<std::recursive_mutex> lock(m_transform_mutex);
    m_mat = Common::Matrix44::FromQuaternion(quat) * m_mat;
    MarkFocusManualTransformChanged();
    MarkDirty();
  }

  void Reset() override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_transform_mutex);
    CameraControllerInput::Reset();
    StopKeyframePlayback();
    ResetFocusOrbit();
    m_mat = Common::Matrix44::Identity();
  }

  void DoState(PointerWrap& p) override
  {
    KeyframePathController::DoState(p);
    p.Do(m_mat);
  }

private:
  Keyframe CaptureCurrentKeyframe() const override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_transform_mutex);
    Keyframe keyframe;
    keyframe.position = Common::Vec3{m_mat.data[3], m_mat.data[7], m_mat.data[11]};
    keyframe.rotation = QuaternionFromMatrix(m_mat);
    keyframe.euler_rotation = Common::FromQuaternionToEuler(keyframe.rotation);
    keyframe.fov = GetFieldOfViewMultiplier();
    return keyframe;
  }

  void ApplyKeyframeTransform(const Keyframe& keyframe) override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_transform_mutex);
    m_mat = MatrixFromKeyframeTransform(keyframe.position, keyframe.rotation);
  }

  void ApplyFocusMatrix(const Common::Matrix44& matrix) override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_transform_mutex);
    m_mat = matrix;
  }

  Common::Matrix44 m_mat = Common::Matrix44::Identity();
  mutable std::recursive_mutex m_transform_mutex;
};

class FPSController final : public KeyframePathController
{
public:
  FPSController() = default;

  Common::Matrix44 GetView() const override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_transform_mutex);
    return Common::Matrix44::FromQuaternion(m_rotate_quat) *
           Common::Matrix44::Translate(m_position);
  }

  void MoveVertical(float amt) override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_transform_mutex);
    const Common::Vec3 up = m_rotate_quat.Conjugate() * Common::Vec3{0, 1, 0};
    m_position += up * amt;
    MarkFocusManualTransformChanged();
    MarkDirty();
  }

  void MoveHorizontal(float amt) override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_transform_mutex);
    const Common::Vec3 right = m_rotate_quat.Conjugate() * Common::Vec3{1, 0, 0};
    m_position += right * amt;
    MarkFocusManualTransformChanged();
    MarkDirty();
  }

  void MoveForward(float amt) override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_transform_mutex);
    const Common::Vec3 forward = m_rotate_quat.Conjugate() * Common::Vec3{0, 0, 1};
    m_position += forward * amt;
    MarkFocusManualTransformChanged();
    MarkDirty();
  }

  void Rotate(const Common::Vec3& amt) override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_transform_mutex);
    if (amt.Length() == 0)
      return;

    m_rotation += amt;

    using Common::Quaternion;
    m_rotate_quat =
        (Quaternion::RotateX(m_rotation.x) * Quaternion::RotateY(m_rotation.y)).Normalized();
    MarkFocusManualTransformChanged();
    MarkDirty();
  }

  void Rotate(const Common::Quaternion& quat) override
  {
    Rotate(Common::FromQuaternionToEuler(quat));
  }

  void Reset() override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_transform_mutex);
    CameraControllerInput::Reset();
    StopKeyframePlayback();
    ResetFocusOrbit();
    m_position = Common::Vec3{};
    m_rotation = Common::Vec3{};
    m_rotate_quat = Common::Quaternion::Identity();
  }

  void DoState(PointerWrap& p) override
  {
    KeyframePathController::DoState(p);
    p.Do(m_rotation);
    p.Do(m_rotate_quat);
    p.Do(m_position);
  }

private:
  Keyframe CaptureCurrentKeyframe() const override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_transform_mutex);
    Keyframe keyframe;
    keyframe.position = m_position;
    keyframe.rotation = m_rotate_quat;
    keyframe.euler_rotation = m_rotation;
    keyframe.fov = GetFieldOfViewMultiplier();
    return keyframe;
  }

  void ApplyKeyframeTransform(const Keyframe& keyframe) override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_transform_mutex);
    m_position = keyframe.position;
    m_rotate_quat = keyframe.rotation;
    m_rotation = keyframe.euler_rotation;
  }

  void ApplyFocusMatrix(const Common::Matrix44& matrix) override
  {
    const std::lock_guard<std::recursive_mutex> lock(m_transform_mutex);
    m_rotate_quat = QuaternionFromMatrix(matrix);
    m_rotation = Common::FromQuaternionToEuler(m_rotate_quat);
    const Common::Vec3 translation{matrix.data[3], matrix.data[7], matrix.data[11]};
    m_position = m_rotate_quat.Conjugate() * translation;
  }

  Common::Vec3 m_rotation = Common::Vec3{};
  Common::Quaternion m_rotate_quat = Common::Quaternion::Identity();
  Common::Vec3 m_position = Common::Vec3{};
  mutable std::recursive_mutex m_transform_mutex;
};

class OrbitalController final : public CameraControllerInput
{
public:
  Common::Matrix44 GetView() const override
  {
    return Common::Matrix44::Translate(Common::Vec3{0, 0, -m_distance}) *
           Common::Matrix44::FromQuaternion(m_rotate_quat);
  }

  void MoveVertical(float) override {}

  void MoveHorizontal(float) override {}

  void MoveForward(float amt) override
  {
    m_distance += -1 * amt;
    m_distance = std::max(m_distance, MIN_DISTANCE);
    MarkDirty();
  }

  void Rotate(const Common::Vec3& amt) override
  {
    if (amt.Length() == 0)
      return;

    m_rotation += amt;

    using Common::Quaternion;
    m_rotate_quat =
        (Quaternion::RotateX(m_rotation.x) * Quaternion::RotateY(m_rotation.y)).Normalized();
    MarkDirty();
  }

  void Rotate(const Common::Quaternion& quat) override
  {
    Rotate(Common::FromQuaternionToEuler(quat));
  }

  void Reset() override
  {
    CameraControllerInput::Reset();
    m_rotation = Common::Vec3{};
    m_rotate_quat = Common::Quaternion::Identity();
    m_distance = MIN_DISTANCE;
  }

  void DoState(PointerWrap& p) override
  {
    CameraControllerInput::DoState(p);
    p.Do(m_rotation);
    p.Do(m_rotate_quat);
    p.Do(m_distance);
  }

private:
  static constexpr float MIN_DISTANCE = 0.0f;
  float m_distance = MIN_DISTANCE;
  Common::Vec3 m_rotation = Common::Vec3{};
  Common::Quaternion m_rotate_quat = Common::Quaternion::Identity();
};
}  // namespace

Common::Vec2 CameraControllerInput::GetFieldOfViewMultiplier() const
{
  return Common::Vec2{m_fov_x_multiplier, m_fov_y_multiplier};
}

void CameraControllerInput::DoState(PointerWrap& p)
{
  p.Do(m_speed);
  p.Do(m_fov_x_multiplier);
  p.Do(m_fov_y_multiplier);
}

bool CameraControllerInput::SupportsKeyframeAnimation() const
{
  return false;
}

std::size_t CameraControllerInput::GetKeyframeCount() const
{
  return 0;
}

bool CameraControllerInput::IsKeyframePlaybackActive() const
{
  return false;
}

void CameraControllerInput::AddKeyframe() {}

void CameraControllerInput::DeleteLastKeyframe() {}

void CameraControllerInput::ClearKeyframes() {}

void CameraControllerInput::ToggleKeyframePlayback() {}

bool CameraControllerInput::SaveKeyframesToFile()
{
  return false;
}

bool CameraControllerInput::LoadKeyframesFromFile()
{
  return false;
}

bool CameraControllerInput::SaveKeyframesToDraftFile()
{
  return false;
}

bool CameraControllerInput::LoadKeyframesFromDraftFile()
{
  return false;
}

void CameraControllerInput::AdvanceKeyframePlayback(float) {}

void CameraControllerInput::UpdateFocusTarget() {}

std::vector<CameraControllerInput::KeyframeData> CameraControllerInput::GetKeyframes() const
{
  return {};
}

bool CameraControllerInput::GetKeyframe(std::size_t, KeyframeData*) const
{
  return false;
}

bool CameraControllerInput::UpdateKeyframe(std::size_t, const KeyframeData&)
{
  return false;
}

bool CameraControllerInput::DeleteKeyframe(std::size_t)
{
  return false;
}

bool CameraControllerInput::ReplaceKeyframes(std::vector<KeyframeData>,
                                             KeyframeInterpolationMode, bool)
{
  return false;
}

CameraControllerInput::KeyframeData CameraControllerInput::GetCurrentCameraTransform() const
{
  return {};
}

CameraControllerInput::KeyframeInterpolationMode
CameraControllerInput::GetKeyframeInterpolationMode() const
{
  return KeyframeInterpolationMode::Linear;
}

void CameraControllerInput::SetKeyframeInterpolationMode(KeyframeInterpolationMode)
{
}

void CameraControllerInput::IncreaseFovX(float fov)
{
  m_fov_x_multiplier += fov;
  m_fov_x_multiplier = std::max(m_fov_x_multiplier, MIN_FOV_MULTIPLIER);
  MarkDirty();
}

void CameraControllerInput::IncreaseFovY(float fov)
{
  m_fov_y_multiplier += fov;
  m_fov_y_multiplier = std::max(m_fov_y_multiplier, MIN_FOV_MULTIPLIER);
  MarkDirty();
}

float CameraControllerInput::GetFovStepSize() const
{
  return 1.5f;
}

void CameraControllerInput::Reset()
{
  m_fov_x_multiplier = DEFAULT_FOV_MULTIPLIER;
  m_fov_y_multiplier = DEFAULT_FOV_MULTIPLIER;
  m_dirty = true;
}

void CameraControllerInput::ModifySpeed(float amt)
{
  m_speed += amt;
  m_speed = std::max(m_speed, 0.0f);
}

void CameraControllerInput::ResetSpeed()
{
  m_speed = DEFAULT_SPEED;
}

float CameraControllerInput::GetSpeed() const
{
  return m_speed;
}

void CameraControllerInput::MarkDirty()
{
  m_dirty = true;
}

void CameraControllerInput::SetFieldOfViewMultiplier(const Common::Vec2& multiplier)
{
  m_fov_x_multiplier = std::max(multiplier.x, MIN_FOV_MULTIPLIER);
  m_fov_y_multiplier = std::max(multiplier.y, MIN_FOV_MULTIPLIER);
  MarkDirty();
}

FreeLookCamera::FreeLookCamera()
{
  RefreshConfig();
}

void FreeLookCamera::RefreshConfig()
{
  m_is_enabled = Config::Get(Config::FREE_LOOK_ENABLED);
  const auto type = Config::Get(Config::FL1_CONTROL_TYPE);

  if (m_current_type == type)
    return;

  if (type == FreeLook::ControlType::Orbital)
  {
    m_camera_controller = std::make_unique<OrbitalController>();
  }
  else if (type == FreeLook::ControlType::FPS)
  {
    m_camera_controller = std::make_unique<FPSController>();
  }
  else
  {
    m_camera_controller = std::make_unique<SixAxisController>();
  }

  m_current_type = type;
}

Common::Matrix44 FreeLookCamera::GetView() const
{
  return m_camera_controller->GetView();
}

Common::Vec2 FreeLookCamera::GetFieldOfViewMultiplier() const
{
  return m_camera_controller->GetFieldOfViewMultiplier();
}

void FreeLookCamera::DoState(PointerWrap& p)
{
  if (p.IsWriteMode() || p.IsMeasureMode())
  {
    p.Do(m_current_type);
    if (m_camera_controller)
    {
      m_camera_controller->DoState(p);
    }
  }
  else
  {
    const auto old_type = m_current_type;
    p.Do(m_current_type);
    if (old_type == m_current_type)
    {
      m_camera_controller->DoState(p);
    }
    else if (p.IsReadMode())
    {
      const std::string old_type_name = old_type ? to_string(*old_type) : "";
      const std::string loaded_type_name = m_current_type ? to_string(*m_current_type) : "";
      const std::string message =
          fmt::format("State needs same free look camera type. Settings value '{}', loaded value "
                      "'{}'.  Aborting load state",
                      old_type_name, loaded_type_name);
      Core::DisplayMessage(message, 5000);
      p.SetVerifyMode();
    }
  }
}

bool FreeLookCamera::IsActive() const
{
  return m_is_enabled;
}

CameraController* FreeLookCamera::GetController() const
{
  return m_camera_controller.get();
}

void FreeLookCamera::UpdateFocusTargetFromMemory(Core::System& system)
{
  if (!m_is_enabled || !Config::Get(Config::FL1_FOCUS_TARGET_ENABLED) ||
      Config::Get(Config::FL1_CONTROL_TYPE) == FreeLook::ControlType::Orbital ||
      !Core::IsRunning(system) || !Core::IsCPUThread())
  {
    SetCachedFocusTarget(std::nullopt);
    SetFocusTargetStatus(FocusTargetStatus::Inactive);
    s_last_focus_movie_frame.reset();
    return;
  }

  if (system.GetMovie().IsPlayingInput())
  {
    const uint64_t movie_frame = system.GetMovie().GetCurrentFrame();
    if (s_last_focus_movie_frame && *s_last_focus_movie_frame == movie_frame)
      return;

    s_last_focus_movie_frame = movie_frame;
  }
  else
  {
    s_last_focus_movie_frame.reset();
  }

  const Core::CPUThreadGuard guard(system);
  std::optional<FocusTargetSample> sample = ReadFocusTarget(guard);
  if (sample)
    sample->sequence = ++s_focus_target_sequence;
  SetCachedFocusTarget(sample);

  if (!sample)
  {
    SetFocusTargetStatus(FocusTargetStatus::NoTarget);
  }
  else if (sample->game_view_matrix && sample->camera_eye && sample->camera_center &&
           sample->camera_up)
  {
    SetFocusTargetStatus(FocusTargetStatus::TargetAndCamera);
  }
  else
  {
    SetFocusTargetStatus(FocusTargetStatus::TargetOnly);
  }

  CameraController* controller = GetController();
  if (controller && controller->SupportsInput())
  {
    auto* input_controller = static_cast<CameraControllerInput*>(controller);
    if (input_controller->IsKeyframePlaybackActive() &&
        Config::Get(Config::FL1_KEYFRAME_SYNC_TO_TAS))
    {
      input_controller->AdvanceKeyframePlayback(0.0f);
    }

    input_controller->UpdateFocusTarget();
  }
}
