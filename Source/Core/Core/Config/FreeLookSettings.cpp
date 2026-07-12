// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Config/FreeLookSettings.h"

namespace Config
{
// Configuration Information
const Info<bool> FREE_LOOK_ENABLED{{System::FreeLook, "General", "Enabled"}, false};
const Info<bool> FREE_LOOK_BACKGROUND_INPUT{{System::FreeLook, "General", "BackgroundInput"},
                                            false};

// FreeLook.Controller1
const Info<FreeLook::ControlType> FL1_CONTROL_TYPE{{System::FreeLook, "Camera1", "ControlType"},
                                                   FreeLook::ControlType::SixAxis};
const Info<int> FL1_KEYFRAME_DEFAULT_FRAME_STEP{{System::FreeLook, "Camera1", "KeyframeFrameStep"},
                                                60};
const Info<bool> FL1_KEYFRAME_SYNC_TO_TAS{{System::FreeLook, "Camera1", "KeyframeSyncToTAS"},
                                          true};
const Info<std::string> FL1_KEYFRAME_PATH_FILE{{System::FreeLook, "Camera1", "KeyframePathFile"},
                                                "freelook_camera_path.fkf"};
const Info<bool> FL1_FOCUS_TARGET_ENABLED{{System::FreeLook, "Camera1", "FocusTargetEnabled"},
                                          false};
const Info<std::string> FL1_FOCUS_TARGET_ADDRESS_X{{System::FreeLook, "Camera1",
                                                    "FocusTargetAddressX"},
                                                   ""};
const Info<std::string> FL1_FOCUS_TARGET_ADDRESS_Y{{System::FreeLook, "Camera1",
                                                    "FocusTargetAddressY"},
                                                   ""};
const Info<std::string> FL1_FOCUS_TARGET_ADDRESS_Z{{System::FreeLook, "Camera1",
                                                    "FocusTargetAddressZ"},
                                                   ""};
const Info<bool> FL1_FOCUS_TARGET_CUSTOM_COORDS_ENABLED{
    {System::FreeLook, "Camera1", "FocusTargetCustomCoordsEnabled"}, false};
const Info<std::string> FL1_FOCUS_TARGET_CUSTOM_X{{System::FreeLook, "Camera1",
                                                   "FocusTargetCustomX"},
                                                  "0"};
const Info<std::string> FL1_FOCUS_TARGET_CUSTOM_Y{{System::FreeLook, "Camera1",
                                                   "FocusTargetCustomY"},
                                                  "0"};
const Info<std::string> FL1_FOCUS_TARGET_CUSTOM_Z{{System::FreeLook, "Camera1",
                                                   "FocusTargetCustomZ"},
                                                  "0"};
const Info<std::string> FL1_FOCUS_TARGET_OFFSET_X{{System::FreeLook, "Camera1",
                                                   "FocusTargetOffsetX"},
                                                  "0"};
const Info<std::string> FL1_FOCUS_TARGET_OFFSET_Y{{System::FreeLook, "Camera1",
                                                   "FocusTargetOffsetY"},
                                                  "0"};
const Info<std::string> FL1_FOCUS_TARGET_OFFSET_Z{{System::FreeLook, "Camera1",
                                                   "FocusTargetOffsetZ"},
                                                  "0"};
const Info<bool> FL1_FOCUS_TARGET_FIXED_ROTATION{
    {System::FreeLook, "Camera1", "FocusTargetFixedRotation"}, false};
const Info<bool> FL1_TRACK_TARGET_ENABLED{{System::FreeLook, "Camera1", "TrackTargetEnabled"},
                                          false};
const Info<std::string> FL1_TRACK_TARGET_ADDRESS_X{{System::FreeLook, "Camera1",
                                                    "TrackTargetAddressX"},
                                                   ""};
const Info<std::string> FL1_TRACK_TARGET_ADDRESS_Y{{System::FreeLook, "Camera1",
                                                    "TrackTargetAddressY"},
                                                   ""};
const Info<std::string> FL1_TRACK_TARGET_ADDRESS_Z{{System::FreeLook, "Camera1",
                                                    "TrackTargetAddressZ"},
                                                   ""};
const Info<bool> FL1_TRACK_TARGET_CUSTOM_COORDS_ENABLED{
    {System::FreeLook, "Camera1", "TrackTargetCustomCoordsEnabled"}, false};
const Info<std::string> FL1_TRACK_TARGET_CUSTOM_X{{System::FreeLook, "Camera1",
                                                   "TrackTargetCustomX"},
                                                  "0"};
const Info<std::string> FL1_TRACK_TARGET_CUSTOM_Y{{System::FreeLook, "Camera1",
                                                   "TrackTargetCustomY"},
                                                  "0"};
const Info<std::string> FL1_TRACK_TARGET_CUSTOM_Z{{System::FreeLook, "Camera1",
                                                   "TrackTargetCustomZ"},
                                                  "0"};
const Info<std::string> FL1_TRACK_TARGET_OFFSET_X{{System::FreeLook, "Camera1",
                                                   "TrackTargetOffsetX"},
                                                  "0"};
const Info<std::string> FL1_TRACK_TARGET_OFFSET_Y{{System::FreeLook, "Camera1",
                                                   "TrackTargetOffsetY"},
                                                  "0"};
const Info<std::string> FL1_TRACK_TARGET_OFFSET_Z{{System::FreeLook, "Camera1",
                                                   "TrackTargetOffsetZ"},
                                                  "0"};

}  // namespace Config
