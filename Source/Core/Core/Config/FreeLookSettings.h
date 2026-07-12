// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/Config/ConfigInfo.h"

namespace FreeLook
{
enum class ControlType : int
{
  SixAxis,
  FPS,
  Orbital
};
}

namespace Config
{
// Configuration Information

extern const Info<bool> FREE_LOOK_ENABLED;
extern const Info<bool> FREE_LOOK_BACKGROUND_INPUT;

// FreeLook.Controller1
extern const Info<FreeLook::ControlType> FL1_CONTROL_TYPE;
extern const Info<int> FL1_KEYFRAME_DEFAULT_FRAME_STEP;
extern const Info<bool> FL1_KEYFRAME_SYNC_TO_TAS;
extern const Info<std::string> FL1_KEYFRAME_PATH_FILE;
extern const Info<bool> FL1_FOCUS_TARGET_ENABLED;
extern const Info<std::string> FL1_FOCUS_TARGET_ADDRESS_X;
extern const Info<std::string> FL1_FOCUS_TARGET_ADDRESS_Y;
extern const Info<std::string> FL1_FOCUS_TARGET_ADDRESS_Z;
extern const Info<bool> FL1_FOCUS_TARGET_CUSTOM_COORDS_ENABLED;
extern const Info<std::string> FL1_FOCUS_TARGET_CUSTOM_X;
extern const Info<std::string> FL1_FOCUS_TARGET_CUSTOM_Y;
extern const Info<std::string> FL1_FOCUS_TARGET_CUSTOM_Z;
extern const Info<std::string> FL1_FOCUS_TARGET_OFFSET_X;
extern const Info<std::string> FL1_FOCUS_TARGET_OFFSET_Y;
extern const Info<std::string> FL1_FOCUS_TARGET_OFFSET_Z;
extern const Info<bool> FL1_FOCUS_TARGET_FIXED_ROTATION;
extern const Info<bool> FL1_TRACK_TARGET_ENABLED;
extern const Info<std::string> FL1_TRACK_TARGET_ADDRESS_X;
extern const Info<std::string> FL1_TRACK_TARGET_ADDRESS_Y;
extern const Info<std::string> FL1_TRACK_TARGET_ADDRESS_Z;
extern const Info<bool> FL1_TRACK_TARGET_CUSTOM_COORDS_ENABLED;
extern const Info<std::string> FL1_TRACK_TARGET_CUSTOM_X;
extern const Info<std::string> FL1_TRACK_TARGET_CUSTOM_Y;
extern const Info<std::string> FL1_TRACK_TARGET_CUSTOM_Z;
extern const Info<std::string> FL1_TRACK_TARGET_OFFSET_X;
extern const Info<std::string> FL1_TRACK_TARGET_OFFSET_Y;
extern const Info<std::string> FL1_TRACK_TARGET_OFFSET_Z;

}  // namespace Config
