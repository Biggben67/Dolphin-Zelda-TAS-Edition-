// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/HW/WiimoteEmu/DesiredWiimoteState.h"
#include "InputCommon/GCPadStatus.h"
struct BootParameters;
class PointerWrap;

namespace Core
{
class System;
}

namespace ExpansionInterface
{
enum class Slot : int;
}

namespace WiimoteCommon
{
class DataReportBuilder;
}

namespace WiimoteEmu
{
class EncryptionKey;
enum ExtensionNumber : u8;
}  // namespace WiimoteEmu

// Per-(video )Movie actions

namespace Movie
{
// Enumerations and structs
enum class ControllerType
{
  None = 0,
  GC,
  GBA,
};
using ControllerTypeArray = std::array<ControllerType, 4>;
using WiimoteEnabledArray = std::array<bool, 4>;

// GameCube Controller State
#pragma pack(push, 1)
struct ControllerState
{
  bool Start : 1, A : 1, B : 1, X : 1, Y : 1, Z : 1;  // Binary buttons, 6 bits
  bool DPadUp : 1, DPadDown : 1,                      // Binary D-Pad buttons, 4 bits
      DPadLeft : 1, DPadRight : 1;
  bool L : 1, R : 1;      // Binary triggers, 2 bits
  bool disc : 1;          // Checks for disc being changed
  bool reset : 1;         // Console reset button
  bool is_connected : 1;  // Should controller be treated as connected
  bool get_origin : 1;    // Special bit to indicate analog origin reset

  u8 TriggerL, TriggerR;          // Triggers, 16 bits
  u8 AnalogStickX, AnalogStickY;  // Main Stick, 16 bits
  u8 CStickX, CStickY;            // Sub-Stick, 16 bits
};
static_assert(sizeof(ControllerState) == 8, "ControllerState should be 8 bytes");
#pragma pack(pop)

// When making changes to the DTM format, keep in mind that there are programs other
// than Dolphin that parse DTM files. The format is expected to be relatively stable.
#pragma pack(push, 1)
struct DTMHeader
{
  std::string_view GetGameID() const
  {
    return {gameID.data(), strnlen(gameID.data(), gameID.size())};
  }

  std::array<u8, 4> filetype;  // Unique Identifier (always "DTM"0x1A)

  std::array<char, 6> gameID;  // The Game ID
  bool bWii;                   // Wii game

  u8 controllers;  // Controllers plugged in (from least to most significant,
                   // the bits are GC controllers 1-4 and Wiimotes 1-4)

  bool
      bFromSaveState;  // false indicates that the recording started from bootup, true for savestate
  u64 frameCount;      // Number of frames in the recording
  u64 inputCount;      // Number of input frames in recording
  u64 lagCount;        // Number of lag frames in the recording
  u64 uniqueID;        // (not implemented) A Unique ID comprised of: md5(time + Game ID)
  u32 numRerecords;    // Number of rerecords/'cuts' of this TAS
  std::array<char, 32> author;  // Author's name (encoded in UTF-8)

  std::array<char, 16> videoBackend;   // UTF-8 representation of the video backend
  std::array<char, 16> audioEmulator;  // UTF-8 representation of the audio emulator
  std::array<u8, 16> md5;              // MD5 of game iso

  u64 recordingStartTime;  // seconds since 1970 that recording started (used for RTC)

  bool bSaveConfig;  // Loads the settings below on startup if true
  bool bSkipIdle;
  bool bDualCore;
  bool bProgressive;
  bool bDSPHLE;
  bool bFastDiscSpeed;
  u8 CPUCore;  // Uses the values of PowerPC::CPUCore
  bool bEFBAccessEnable;
  bool bEFBCopyEnable;
  bool bSkipEFBCopyToRam;
  bool bEFBCopyCacheEnable;
  bool bEFBEmulateFormatChanges;
  bool bImmediateXFB;
  bool bSkipXFBCopyToRam;
  u8 memcards;      // Memcards inserted (from least to most significant, the bits are slot A and B)
  bool bClearSave;  // Create a new memory card when playing back a movie if true
  u8 bongos;        // Bongos plugged in (from least to most significant, the bits are ports 1-4)
  bool bSyncGPU;
  bool bNetPlay;
  bool bPAL60;
  u8 language;
  u8 reserved3;
  bool bFollowBranch;
  bool bUseFMA;
  u8 GBAControllers;                // GBA Controllers plugged in (the bits are ports 1-4)
  bool bWidescreen;                 // true indicates SYSCONF aspect ratio is 16:9, false for 4:3
  u8 countryCode;                   // SYSCONF country code
  std::array<u8, 5> reserved;       // Padding for any new config options
  std::array<char, 40> discChange;  // Name of iso file to switch to, for two disc games.
  std::array<u8, 20> revision;      // Git hash
  u32 DSPiromHash;
  u32 DSPcoefHash;
  u64 tickCount;                 // Number of ticks in the recording
  std::array<u8, 11> reserved2;  // Make heading 256 bytes, just because we can
};
static_assert(sizeof(DTMHeader) == 256, "DTMHeader should be 256 bytes");

#pragma pack(pop)

struct GCRuntimeFrameSnapshot
{
  std::array<bool, 4> active_controllers{};
  std::array<bool, 4> active_gba_controllers{};
  std::vector<std::array<ControllerState, 4>> rows;
  std::vector<std::optional<u64>> row_game_frames;
  u64 current_frame = 0;
  u64 current_input_row = 0;
  bool is_recording = false;
  bool is_playing = false;
  bool is_read_only = true;
};

struct GCRuntimeFrameMetadata
{
  std::array<bool, 4> active_controllers{};
  std::array<bool, 4> active_gba_controllers{};
  u64 row_count = 0;
  u64 current_frame = 0;
  u64 current_input_row = 0;
  u64 data_generation = 0;
  bool is_recording = false;
  bool is_playing = false;
  bool is_read_only = true;
};

struct WiiRuntimeInputRow
{
  int wiimote = -1;
  bool is_reset = false;
  WiimoteEmu::SerializedWiimoteState serialized_state{};
};

struct WiiRuntimeFrameSnapshot
{
  std::array<bool, 4> active_wiimotes{};
  std::vector<WiiRuntimeInputRow> rows;
  std::vector<std::optional<u64>> row_game_frames;
  u64 current_frame = 0;
  u64 current_input_row = 0;
  bool is_recording = false;
  bool is_playing = false;
  bool is_read_only = true;
};

struct WiiRuntimeFrameMetadata
{
  std::array<bool, 4> active_wiimotes{};
  u64 row_count = 0;
  u64 current_frame = 0;
  u64 current_input_row = 0;
  u64 data_generation = 0;
  bool is_recording = false;
  bool is_playing = false;
  bool is_read_only = true;
};

enum class PlayMode
{
  None = 0,
  Recording,
  Playing,
};

class MovieManager
{
public:
  explicit MovieManager(Core::System& system);
  MovieManager(const MovieManager& other) = delete;
  MovieManager(MovieManager&& other) = delete;
  MovieManager& operator=(const MovieManager& other) = delete;
  MovieManager& operator=(MovieManager&& other) = delete;
  ~MovieManager();

  void FrameUpdate();
  void InputUpdate();
  void Init(const BootParameters& boot);

  void SetPolledDevice();

  bool IsRecordingInput() const;
  bool IsRecordingInputFromSaveState() const;
  bool IsJustStartingRecordingInputFromSaveState() const;
  bool IsJustStartingPlayingInputFromSaveState() const;
  bool IsPlayingInput() const;
  bool IsMovieActive() const;
  bool IsReadOnly() const;
  u64 GetRecordingStartTime() const;

  u64 GetCurrentFrame() const;
  u64 GetTotalFrames() const;
  u64 GetCurrentInputCount() const;
  u64 GetTotalInputCount() const;
  u64 GetCurrentLagCount() const;
  u64 GetTotalLagCount() const;
  // Monotonic count of controller polls; lets host-thread tools step by input frame.
  u64 GetPollCount() const;

  void SetClearSave(bool enabled);
  void SignalDiscChange(const std::string& new_path);
  void SetReset(bool reset);

  bool IsConfigSaved() const;
  bool IsStartingFromClearSave() const;
  bool IsUsingMemcard(ExpansionInterface::Slot slot) const;
  bool IsNetPlayRecording() const;

  bool IsUsingPad(int controller) const;
  bool IsUsingWiimote(int wiimote) const;
  bool IsUsingBongo(int controller) const;
  bool IsUsingGBA(int controller) const;
  void ChangePads();
  void ChangeWiiPads(bool instantly = false);
  void ApplyWiimoteBatteryIfPending();

  void SetReadOnly(bool bEnabled);

  bool BeginRecordingInput(const ControllerTypeArray& controllers,
                           const WiimoteEnabledArray& wiimotes);
  void RecordInput(const GCPadStatus* PadStatus, int controllerID);
  void RecordWiimote(int wiimote, const WiimoteEmu::SerializedWiimoteState& serialized_state);

  bool PlayInput(const std::string& movie_path, std::optional<std::string>* savestate_path);
  void LoadInput(const std::string& movie_path);
  void ReadHeader();
  void PlayController(GCPadStatus* PadStatus, int controllerID);
  bool PlayWiimote(int wiimote, WiimoteEmu::DesiredWiimoteState* desired_state);
  void EndPlayInput(bool cont);
  void SaveRecording(const std::string& filename);
  void DoState(PointerWrap& p);
  void Shutdown();
  void CheckPadStatus(const GCPadStatus* PadStatus, int controllerID);
  void CheckWiimoteStatus(int wiimote, const WiimoteEmu::DesiredWiimoteState& desired_state);

  std::string GetInputDisplay();
  std::string GetInfoDisplay();
  std::string GetRTCDisplay() const;
  std::string GetRerecords() const;
  std::optional<GCRuntimeFrameMetadata> GetGCRuntimeFrameMetadata() const;
  std::optional<GCRuntimeFrameSnapshot> GetGCRuntimeFrameSnapshot() const;
  std::optional<std::array<ControllerState, 4>> GetGCRuntimeFrameRow(u64 row) const;
  bool SetGCRuntimeFrameState(u64 frame, int controller, const ControllerState& state);
  std::optional<WiiRuntimeFrameMetadata> GetWiiRuntimeFrameMetadata() const;
  std::optional<WiiRuntimeFrameSnapshot> GetWiiRuntimeFrameSnapshot() const;
  std::optional<WiiRuntimeInputRow> GetWiiRuntimeFrameRow(u64 row) const;
  bool SetWiiRuntimeFrameState(u64 row, const WiimoteEmu::SerializedWiimoteState& serialized_state);
  std::optional<GCPadStatus> GetDisplayedPadStatus(int controller) const;
  std::optional<WiimoteEmu::DesiredWiimoteState> GetDisplayedWiimoteState(int wiimote) const;
  void OnAfterStateLoad();

private:
  void GetSettings();
  void CheckInputEnd();
  void SaveWiimoteBatteryToHeader(DTMHeader* header) const;
  void ApplyWiimoteBatteryFromHeader(const DTMHeader& header);
  void SaveGBADisconnectToHeader(DTMHeader* header) const;
  void ApplyGBADisconnectFromHeader(const DTMHeader& header);
  void ApplyGBADisconnectIfPending();
  void UpdateDisplayedPadStatus(int controller, const GCPadStatus& status);
  void UpdateDisplayedWiimoteState(int wiimote,
                                   const WiimoteEmu::DesiredWiimoteState& desired_state);

  void CheckMD5();
  void GetMD5();

  bool m_read_only = true;
  u32 m_rerecords = 0;
  PlayMode m_play_mode = PlayMode::None;

  std::array<ControllerType, 4> m_controllers{};
  std::array<bool, 4> m_wiimotes{};
  ControllerState m_pad_state{};
  DTMHeader m_temp_header{};
  std::vector<u8> m_temp_input;
  u64 m_current_byte = 0;
  u64 m_current_frame = 0;
  u64 m_total_frames = 0;  // VI
  u64 m_current_lag_count = 0;
  u64 m_total_lag_count = 0;
  u64 m_current_input_count = 0;
  u64 m_total_input_count = 0;
  u64 m_total_tick_count = 0;
  u64 m_tick_count_at_last_input = 0;
  u64 m_recording_start_time = 0;  // seconds since 1970 that recording started
  bool m_save_config = false;
  bool m_net_play = false;
  bool m_clear_save = false;
  bool m_has_disc_change = false;
  bool m_reset = false;
  std::string m_author;
  std::string m_disc_change_filename;
  std::array<u8, 16> m_md5{};
  u8 m_bongos = 0;
  u8 m_memcards = 0;
  std::array<u8, 20> m_revision{};
  u32 m_dsp_irom_hash = 0;
  u32 m_dsp_coef_hash = 0;
  bool m_has_dtm_wiimote_battery = false;
  std::array<u8, 4> m_dtm_wiimote_battery{};
  bool m_has_dtm_gba_disconnect = false;
  u8 m_dtm_gba_disconnect_mask = 0;

  bool m_recording_from_save_state = false;
  bool m_polled = false;
  std::atomic<u64> m_poll_count{0};

  std::string m_current_file_name;
  std::string m_movie_path;

  // m_input_display is used by both CPU and GPU (is mutable).
  std::mutex m_input_display_lock;
  std::array<std::string, 8> m_input_display;
  mutable std::mutex m_live_input_lock;
  std::array<std::optional<GCPadStatus>, 4> m_displayed_pad_status{};
  std::array<std::optional<WiimoteEmu::SerializedWiimoteState>, 4> m_displayed_wiimote_state{};
  mutable std::mutex m_temp_input_lock;
  std::vector<std::optional<u64>> m_gc_runtime_row_frames;
  std::vector<std::optional<u64>> m_wii_runtime_row_frames;
  std::atomic<u64> m_runtime_data_generation{0};

  Core::System& m_system;
};

}  // namespace Movie
