// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Scripting/ControlPipe.h"

#ifdef _WIN32

#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "Common/Logging/LogManager.h"
#include "Core/API/Events.h"
#include "Core/Core.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/GCPadEmu.h"
#include "Core/Movie.h"
#include "Core/State.h"
#include "Core/System.h"
#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"
#include "InputCommon/ControllerEmu/StickGate.h"
#include "InputCommon/GCPadStatus.h"
#include "InputCommon/InputConfig.h"
#include "Scripting/ScriptList.h"

namespace Scripting
{

namespace
{
constexpr DWORD PIPE_BUF = 4096;

// Per-process pipe name so multiple Dolphin instances each get their own channel.
const wchar_t* PipeName()
{
  static const std::wstring name =
      L"\\\\.\\pipe\\DolphinControl-" + std::to_wstring(GetCurrentProcessId());
  return name.c_str();
}

Core::System* s_system = nullptr;
std::atomic<bool> s_running{false};
std::thread s_thread;
HostHooks s_hooks;

// Captures recent log lines (already filtered by the per-channel/verbosity config) so the pipe
// can serve them. Lives for LogManager's lifetime once registered.
class RingLogListener : public Common::Log::LogListener
{
public:
  struct Entry
  {
    Common::Log::LogLevel level;
    std::string text;
  };

  void Log(Common::Log::LogLevel level, const char* msg) override
  {
    std::lock_guard lock(m_mutex);
    m_entries[m_head] = Entry{level, msg};
    m_head = (m_head + 1) % CAP;
    if (m_count < CAP)
      m_count++;
  }

  std::vector<Entry> Snapshot()
  {
    std::lock_guard lock(m_mutex);
    std::vector<Entry> out;
    out.reserve(m_count);
    const size_t start = (m_head + CAP - m_count) % CAP;
    for (size_t i = 0; i < m_count; ++i)
      out.push_back(m_entries[(start + i) % CAP]);
    return out;
  }

private:
  static constexpr size_t CAP = 1024;
  std::mutex m_mutex;
  std::array<Entry, CAP> m_entries;
  size_t m_head = 0;
  size_t m_count = 0;
};

RingLogListener* s_log_listener = nullptr;

const char* StateToString(Core::State state)
{
  switch (state)
  {
  case Core::State::Running:
    return "running";
  case Core::State::Paused:
    return "paused";
  default:
    return "other";
  }
}

bool WriteRaw(HANDLE pipe, const char* buf, int len)
{
  DWORD written = 0;
  return WriteFile(pipe, buf, static_cast<DWORD>(len), &written, nullptr) &&
         written == static_cast<DWORD>(len);
}

bool WriteResponse(HANDLE pipe, bool ok, const char* state)
{
  char buf[128];
  int len = snprintf(buf, sizeof(buf), "{\"ok\":%s,\"state\":\"%s\"}\n",
                     ok ? "true" : "false", state);
  return WriteRaw(pipe, buf, len);
}

bool WriteResponseWithFrame(HANDLE pipe, bool ok, const char* state, u64 frame)
{
  char buf[160];
  int len = snprintf(buf, sizeof(buf), "{\"ok\":%s,\"state\":\"%s\",\"frame\":%" PRIu64 "}\n",
                     ok ? "true" : "false", state, frame);
  return WriteRaw(pipe, buf, len);
}

bool WriteString(HANDLE pipe, const std::string& s)
{
  return WriteRaw(pipe, s.c_str(), static_cast<int>(s.size()));
}

// Append s to out as a JSON string body (no surrounding quotes), escaping the minimum needed.
void AppendJsonEscaped(std::string& out, const std::string& s)
{
  for (char c : s)
  {
    switch (c)
    {
    case '"':  out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n";  break;
    case '\r': out += "\\r";  break;
    case '\t': out += "\\t";  break;
    default:
      if (static_cast<unsigned char>(c) < 0x20)
      {
        char u[8];
        snprintf(u, sizeof(u), "\\u%04x", static_cast<unsigned char>(c));
        out += u;
      }
      else
      {
        out += c;
      }
    }
  }
}

// Parse a JSON integer field "key": N from buf. Returns -1 if not found.
long long ParseIntField(const char* buf, const char* key)
{
  const char* p = strstr(buf, key);
  if (!p)
    return -1;
  p += strlen(key);
  while (*p == ' ' || *p == ':' || *p == '"')
    p++;
  return strtoll(p, nullptr, 10);
}

// Parse a JSON string field "key": "value" from buf into out (max outlen).
bool ParseStringField(const char* buf, const char* key, char* out, int outlen)
{
  const char* p = strstr(buf, key);
  if (!p)
    return false;
  p += strlen(key);
  while (*p && *p != ':')
    p++;  // key-value colon
  if (!*p)
    return false;
  while (*p && *p != '"')
    p++;  // opening quote of value
  if (!*p)
    return false;
  p++;  // skip opening quote
  int i = 0;
  // Copy raw until the closing quote; any ':' inside (a drive letter) is part of the value.
  while (*p && *p != '"' && i < outlen - 1)
    out[i++] = *p++;
  out[i] = '\0';
  return i > 0;
}

struct PadOverride
{
  bool active = false;
  u16 buttons = 0;
  u8 stickX = 128, stickY = 128;
  u8 substickX = 128, substickY = 128;
  u8 triggerLeft = 0, triggerRight = 0;
};

// Normalized 0-255 byte → ControlState (-1.0 to 1.0, center=128→0.0)
static double ByteToControl(u8 v) { return (static_cast<double>(v) - 128.0) / 128.0; }
static double ByteToTrigger(u8 v) { return static_cast<double>(v) / 255.0; }

// Install/clear the GCPad override. Not GCManip: it no-ops during movie playback.
// Must run on the CPU thread — the emu thread reads this at poll time (HANDOFF.md pt 9).
static void ApplyPadOverride(int port, const PadOverride& ov)
{
  auto* controller = Pad::GetConfig()->GetController(port);
  if (!controller)
    return;
  if (!ov.active)
  {
    controller->ClearInputOverrideFunction();
    return;
  }
  const u16 buttons = ov.buttons;
  const double sx = ByteToControl(ov.stickX);
  const double sy = ByteToControl(ov.stickY);
  const double cx = ByteToControl(ov.substickX);
  const double cy = ByteToControl(ov.substickY);
  const double tl = ByteToTrigger(ov.triggerLeft);
  const double tr = ByteToTrigger(ov.triggerRight);
  controller->SetInputOverrideFunction(
      [buttons, sx, sy, cx, cy, tl, tr](std::string_view group, std::string_view control,
                                        ControlState) -> std::optional<ControlState> {
        if (group == GCPad::MAIN_STICK_GROUP)
        {
          if (control == ControllerEmu::ReshapableInput::X_INPUT_OVERRIDE) return sx;
          if (control == ControllerEmu::ReshapableInput::Y_INPUT_OVERRIDE) return sy;
        }
        if (group == GCPad::C_STICK_GROUP)
        {
          if (control == ControllerEmu::ReshapableInput::X_INPUT_OVERRIDE) return cx;
          if (control == ControllerEmu::ReshapableInput::Y_INPUT_OVERRIDE) return cy;
        }
        if (group == GCPad::TRIGGERS_GROUP)
        {
          if (control == GCPad::L_ANALOG)  return tl;
          if (control == GCPad::R_ANALOG)  return tr;
        }
        if (group == GCPad::BUTTONS_GROUP)
        {
          if (control == GCPad::A_BUTTON)     return (buttons & PAD_BUTTON_A)     ? 1.0 : 0.0;
          if (control == GCPad::B_BUTTON)     return (buttons & PAD_BUTTON_B)     ? 1.0 : 0.0;
          if (control == GCPad::X_BUTTON)     return (buttons & PAD_BUTTON_X)     ? 1.0 : 0.0;
          if (control == GCPad::Y_BUTTON)     return (buttons & PAD_BUTTON_Y)     ? 1.0 : 0.0;
          if (control == GCPad::Z_BUTTON)     return (buttons & PAD_TRIGGER_Z)    ? 1.0 : 0.0;
          if (control == GCPad::START_BUTTON) return (buttons & PAD_BUTTON_START) ? 1.0 : 0.0;
        }
        return std::nullopt;
      });
}

// Apply on the CPU thread (the pad-polling thread) and wait, so the value is latched
// before the caller frame-steps.
static void ApplyPadOverrideOnCPU(Core::System* system, int port, const PadOverride& ov)
{
  Core::RunOnCPUThread(
      *system, [port, ov]() { ApplyPadOverride(port, ov); }, true);
}

// Step the requested number of input frames (controller polls), skipping lag VI frames so
// "frames=N" means N inputs the game actually read, not N display frames.
static u64 StepInputFrames(Core::System* system, long long n)
{
  auto& movie = system->GetMovie();
  for (long long i = 0; i < n; ++i)
  {
    const u64 polls_before = movie.GetPollCount();
    // Cap the VI steps spent reaching one poll so a non-polling state can't hang the pipe.
    for (int step = 0; step < 64 && movie.GetPollCount() == polls_before; ++step)
    {
      Core::DoFrameStep(*system);
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (std::chrono::steady_clock::now() < deadline)
      {
        if (Core::GetState(*system) == Core::State::Paused)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  }
  return movie.GetCurrentFrame();
}

struct SeqElement
{
  PadOverride ov;
  long long frames = 1;  // input frames to hold this element
};

// Drive a whole sequence in one call. A CPU-thread FrameAdvance listener re-applies the
// current element before each SI poll, so the override is only written on the polling thread.
static u64 RunInputSequence(Core::System* system, int port, const std::vector<SeqElement>& seq)
{
  auto& movie = system->GetMovie();
  if (seq.empty())
    return movie.GetCurrentFrame();

  struct Shared
  {
    const std::vector<SeqElement>* seq;
    int port;
    std::atomic<size_t> index;
  };
  auto shared = std::make_shared<Shared>();
  shared->seq = &seq;
  shared->port = port;
  shared->index.store(0);

  auto listener = [shared](const API::Events::FrameAdvance&) {
    const size_t i = shared->index.load();
    if (i < shared->seq->size())
      ApplyPadOverride(shared->port, (*shared->seq)[i].ov);
  };
  auto id = API::GetEventHub().ListenEvent<API::Events::FrameAdvance>(listener);

  u64 landed = movie.GetCurrentFrame();
  for (size_t i = 0; i < seq.size(); ++i)
  {
    shared->index.store(i);
    landed = StepInputFrames(system, seq[i].frames > 0 ? seq[i].frames : 1);
  }

  API::GetEventHub().UnlistenEvent(id);
  return landed;
}

static PadOverride ParsePadOverride(const char* buf)
{
  PadOverride ov;
  ov.active = true;
  ov.buttons      = static_cast<u16>(ParseIntField(buf, "\"buttons\"") & 0xFFFF);
  long long sx    = ParseIntField(buf, "\"stickX\"");
  long long sy    = ParseIntField(buf, "\"stickY\"");
  long long cx    = ParseIntField(buf, "\"substickX\"");
  long long cy    = ParseIntField(buf, "\"substickY\"");
  long long tl    = ParseIntField(buf, "\"triggerL\"");
  long long tr    = ParseIntField(buf, "\"triggerR\"");
  ov.stickX       = static_cast<u8>(sx < 0 ? 128 : sx);
  ov.stickY       = static_cast<u8>(sy < 0 ? 128 : sy);
  ov.substickX    = static_cast<u8>(cx < 0 ? 128 : cx);
  ov.substickY    = static_cast<u8>(cy < 0 ? 128 : cy);
  ov.triggerLeft  = static_cast<u8>(tl < 0 ? 0   : tl);
  ov.triggerRight = static_cast<u8>(tr < 0 ? 0   : tr);
  return ov;
}

// Parse a flat "seq":[{...},{...}] array. Elements are flat objects (no nested braces), each
// parsed with ParsePadOverride + a "frames" field. Returns false if no elements were found.
static bool ParseSequence(const char* buf, std::vector<SeqElement>& out)
{
  const char* p = strstr(buf, "\"seq\"");
  if (!p)
    return false;
  p = strchr(p, '[');
  if (!p)
    return false;

  while (true)
  {
    const char* obj = strchr(p, '{');
    const char* close = strchr(p, ']');
    if (!obj || (close && close < obj))
      break;
    const char* end = strchr(obj, '}');
    if (!end)
      break;
    const std::string elem(obj, end - obj + 1);
    SeqElement e;
    e.ov = ParsePadOverride(elem.c_str());
    const long long f = ParseIntField(elem.c_str(), "\"frames\"");
    e.frames = (f > 0 ? f : 1);
    out.push_back(e);
    p = end + 1;
  }
  return !out.empty();
}

void HandleRequest(HANDLE pipe, const char* buf, Core::System* system)
{
  Core::State state = Core::GetState(*system);

  if (strstr(buf, "\"ping\""))
  {
    const char* resp = "{\"ok\":true,\"result\":\"pong\"}\n";
    WriteRaw(pipe, resp, static_cast<int>(strlen(resp)));
  }
  else if (strstr(buf, "\"status\""))
  {
    auto& movie = system->GetMovie();
    char resp[256];
    int len = snprintf(
        resp, sizeof(resp),
        "{\"ok\":true,\"state\":\"%s\",\"frame\":%" PRIu64 ",\"recording\":%s,\"playing\":%s}\n",
        StateToString(state), movie.GetCurrentFrame(), movie.IsRecordingInput() ? "true" : "false",
        movie.IsPlayingInput() ? "true" : "false");
    WriteRaw(pipe, resp, len);
  }
  else if (strstr(buf, "\"pause\""))
  {
    Core::SetState(*system, Core::State::Paused);
    WriteResponse(pipe, true, "paused");
  }
  else if (strstr(buf, "\"resume\""))
  {
    Core::SetState(*system, Core::State::Running);
    WriteResponse(pipe, true, "running");
  }
  else if (strstr(buf, "\"toggle\""))
  {
    Core::State next =
        (state == Core::State::Paused) ? Core::State::Running : Core::State::Paused;
    Core::SetState(*system, next);
    WriteResponse(pipe, true, StateToString(next));
  }
  else if (strstr(buf, "\"frame\""))
  {
    u64 frame = system->GetMovie().GetCurrentFrame();
    char resp[64];
    int len = snprintf(resp, sizeof(resp), "{\"ok\":true,\"frame\":%" PRIu64 "}\n", frame);
    WriteRaw(pipe, resp, len);
  }
  else if (strstr(buf, "\"advance\""))
  {
    long long n = ParseIntField(buf, "\"frames\"");
    if (n <= 0)
      n = 1;

    u64 landed = StepInputFrames(system, n);
    WriteResponseWithFrame(pipe, true, "paused", landed);
  }
  else if (strstr(buf, "\"advanceseq\""))
  {
    long long port = ParseIntField(buf, "\"port\"");
    int ctrl = static_cast<int>(port < 0 ? 0 : port);
    std::vector<SeqElement> seq;
    if (!ParseSequence(buf, seq))
    {
      WriteResponse(pipe, false, StateToString(state));
    }
    else
    {
      u64 landed = RunInputSequence(system, ctrl, seq);
      WriteResponseWithFrame(pipe, true, "paused", landed);
    }
  }
  else if (strstr(buf, "\"advancewith\""))
  {
    long long n    = ParseIntField(buf, "\"frames\"");
    long long port = ParseIntField(buf, "\"port\"");
    if (n <= 0) n = 1;
    int ctrl = static_cast<int>(port < 0 ? 0 : port);

    // Route through the sequence path so the input is applied on the CPU thread (race-free),
    // even for a single call in a dense per-frame replay loop.
    std::vector<SeqElement> seq{SeqElement{ParsePadOverride(buf), n}};
    u64 landed = RunInputSequence(system, ctrl, seq);
    WriteResponseWithFrame(pipe, true, "paused", landed);
  }
  else if (strstr(buf, "\"setinput\""))
  {
    long long port = ParseIntField(buf, "\"port\"");
    int ctrl = static_cast<int>(port < 0 ? 0 : port);
    PadOverride ov = ParsePadOverride(buf);
    ApplyPadOverrideOnCPU(system, ctrl, ov);
    WriteResponse(pipe, true, StateToString(state));
  }
  else if (strstr(buf, "\"clearinput\""))
  {
    long long port = ParseIntField(buf, "\"port\"");
    int ctrl = static_cast<int>(port < 0 ? 0 : port);
    PadOverride clear_ov;
    ApplyPadOverrideOnCPU(system, ctrl, clear_ov);
    WriteResponse(pipe, true, StateToString(state));
  }
  else if (strstr(buf, "\"savestate\""))
  {
    long long slot = ParseIntField(buf, "\"slot\"");
    char path[512] = {};
    bool has_path = ParseStringField(buf, "\"path\"", path, sizeof(path));

    if (strstr(buf, "\"save\""))
    {
      if (has_path)
        State::SaveAs(*system, std::string(path), true);
      else if (slot >= 0)
        State::Save(*system, static_cast<int>(slot), true);
      else
        State::Save(*system, 1, true);
      WriteResponse(pipe, true, StateToString(Core::GetState(*system)));
    }
    else if (strstr(buf, "\"load\""))
    {
      if (has_path)
        State::LoadAs(*system, std::string(path));
      else if (slot >= 0)
        State::Load(*system, static_cast<int>(slot));
      else
        State::Load(*system, 1);
      WriteResponse(pipe, true, StateToString(Core::GetState(*system)));
    }
    else
    {
      WriteResponse(pipe, false, StateToString(state));
    }
  }
  else if (strstr(buf, "\"enablescript\"") || strstr(buf, "\"disablescript\""))
  {
    const bool enable = strstr(buf, "\"enablescript\"") != nullptr;
    char path[512] = {};
    if (!ParseStringField(buf, "\"path\"", path, sizeof(path)))
      WriteResponse(pipe, false, StateToString(state));
    else
      WriteResponse(pipe, Scripts::SetEnabled(path, enable), StateToString(state));
  }
  else if (strstr(buf, "\"restartscript\""))
  {
    char path[512] = {};
    if (!ParseStringField(buf, "\"path\"", path, sizeof(path)))
      WriteResponse(pipe, false, StateToString(state));
    else
      WriteResponse(pipe, Scripts::Restart(path), StateToString(state));
  }
  else if (strstr(buf, "\"listscripts\""))
  {
    std::string resp = "{\"ok\":true,\"dir\":\"";
    AppendJsonEscaped(resp, Scripts::ScriptsDir());
    resp += "\",\"scripts\":[";
    bool first = true;
    for (const auto& s : Scripts::List())
    {
      if (!first)
        resp += ',';
      first = false;
      resp += "{\"path\":\"";
      AppendJsonEscaped(resp, s.path);
      resp += "\",\"enabled\":";
      resp += s.enabled ? "true" : "false";
      resp += ",\"running\":";
      resp += s.running ? "true" : "false";
      resp += '}';
    }
    resp += "]}\n";
    WriteString(pipe, resp);
  }
  else if (strstr(buf, "\"boot\""))
  {
    char path[512] = {};
    if (!s_hooks.boot || !ParseStringField(buf, "\"path\"", path, sizeof(path)))
      WriteResponse(pipe, false, StateToString(state));
    else
    {
      s_hooks.boot(path);
      // Boot is async (render widget + core spin-up); the client polls "status" for "running".
      WriteResponse(pipe, true, "booting");
    }
  }
  else if (strstr(buf, "\"stop\""))
  {
    if (!s_hooks.stop)
      WriteResponse(pipe, false, StateToString(state));
    else
    {
      s_hooks.stop();
      WriteResponse(pipe, true, "stopping");
    }
  }
  else if (strstr(buf, "\"reset\""))
  {
    if (!s_hooks.reset)
      WriteResponse(pipe, false, StateToString(state));
    else
    {
      s_hooks.reset();
      WriteResponse(pipe, true, StateToString(state));
    }
  }
  else if (strstr(buf, "\"listgames\""))
  {
    if (!s_hooks.list_games)
    {
      WriteResponse(pipe, false, StateToString(state));
    }
    else
    {
      std::string resp = "{\"ok\":true,\"games\":[";
      bool first = true;
      for (const auto& g : s_hooks.list_games())
      {
        if (!first)
          resp += ',';
        first = false;
        resp += "{\"id\":\"";
        AppendJsonEscaped(resp, g.id);
        resp += "\",\"name\":\"";
        AppendJsonEscaped(resp, g.name);
        resp += "\",\"path\":\"";
        AppendJsonEscaped(resp, g.path);
        resp += "\",\"platform\":\"";
        AppendJsonEscaped(resp, g.platform);
        resp += "\",\"region\":\"";
        AppendJsonEscaped(resp, g.region);
        resp += "\"}";
      }
      resp += "]}\n";
      WriteString(pipe, resp);
    }
  }
  else if (strstr(buf, "\"recordstart\""))
  {
    char path[512] = {};
    ParseStringField(buf, "\"path\"", path, sizeof(path));  // optional
    if (!s_hooks.record_start)
      WriteResponse(pipe, false, StateToString(state));
    else
    {
      s_hooks.record_start(path);
      WriteResponse(pipe, true, StateToString(state));
    }
  }
  else if (strstr(buf, "\"recordstop\""))
  {
    char path[512] = {};
    ParseStringField(buf, "\"path\"", path, sizeof(path));  // optional; saved if given
    if (!s_hooks.record_stop)
      WriteResponse(pipe, false, StateToString(state));
    else
    {
      s_hooks.record_stop(path);
      WriteResponse(pipe, true, StateToString(state));
    }
  }
  else if (strstr(buf, "\"playmovie\""))
  {
    char dtm[512] = {};
    char game[512] = {};
    if (!s_hooks.play_movie || !ParseStringField(buf, "\"path\"", dtm, sizeof(dtm)) ||
        !ParseStringField(buf, "\"game\"", game, sizeof(game)))
    {
      WriteResponse(pipe, false, StateToString(state));
    }
    else
    {
      s_hooks.play_movie(dtm, game);
      WriteResponse(pipe, true, "booting");
    }
  }
  else if (strstr(buf, "\"logconfig\""))
  {
    auto* lm = Common::Log::LogManager::GetInstance();
    std::string resp = "{\"ok\":true,\"verbosity\":";
    resp += std::to_string(lm ? static_cast<int>(lm->GetEffectiveLogLevel()) : 0);
    resp += ",\"channels\":[";
    if (lm)
    {
      bool first = true;
      for (const auto& c : lm->GetLogTypes())
      {
        if (!first)
          resp += ',';
        first = false;
        resp += "{\"name\":\"";
        AppendJsonEscaped(resp, c.m_short_name);
        resp += "\",\"full\":\"";
        AppendJsonEscaped(resp, c.m_full_name);
        resp += "\",\"enabled\":";
        resp += c.m_enable ? "true" : "false";
        resp += '}';
      }
    }
    resp += "]}\n";
    WriteString(pipe, resp);
  }
  else if (strstr(buf, "\"logenable\""))
  {
    char channel[64] = {};
    const long long en = ParseIntField(buf, "\"enable\"");
    auto* lm = Common::Log::LogManager::GetInstance();
    bool ok = false;
    if (lm && ParseStringField(buf, "\"channel\"", channel, sizeof(channel)))
    {
      const auto types = lm->GetLogTypes();
      for (size_t i = 0; i < types.size(); ++i)
      {
        if (_stricmp(types[i].m_short_name, channel) == 0)
        {
          lm->SetEnable(static_cast<Common::Log::LogType>(i), en != 0);
          ok = true;
          break;
        }
      }
    }
    WriteResponse(pipe, ok, StateToString(state));
  }
  else if (strstr(buf, "\"logverbosity\""))
  {
    const long long lvl = ParseIntField(buf, "\"level\"");
    auto* lm = Common::Log::LogManager::GetInstance();
    if (lm && lvl >= 1 && lvl <= 5)
    {
      lm->SetConfigLogLevel(static_cast<Common::Log::LogLevel>(lvl));
      WriteResponse(pipe, true, StateToString(state));
    }
    else
    {
      WriteResponse(pipe, false, StateToString(state));
    }
  }
  else if (strstr(buf, "\"log\""))
  {
    long long count = ParseIntField(buf, "\"count\"");
    if (count <= 0)
      count = 200;  // default tail; the buffer holds up to 1024 lines
    char channel[64] = {};
    const bool has_chan = ParseStringField(buf, "\"channel\"", channel, sizeof(channel));
    const std::string filter = has_chan ? std::string("[") + channel + "]" : std::string();

    std::vector<RingLogListener::Entry> entries;
    if (s_log_listener)
      entries = s_log_listener->Snapshot();

    std::vector<const RingLogListener::Entry*> sel;
    for (const auto& e : entries)
    {
      if (filter.empty() || e.text.find(filter) != std::string::npos)
        sel.push_back(&e);
    }
    size_t begin = 0;
    if (sel.size() > static_cast<size_t>(count))
      begin = sel.size() - static_cast<size_t>(count);

    std::string resp = "{\"ok\":true,\"lines\":[";
    for (size_t i = begin; i < sel.size(); ++i)
    {
      if (i != begin)
        resp += ',';
      std::string line = sel[i]->text;
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
      resp += '"';
      AppendJsonEscaped(resp, line);
      resp += '"';
    }
    resp += "]}\n";
    WriteString(pipe, resp);
  }
  else
  {
    WriteResponse(pipe, false, StateToString(state));
  }
}

void PipeServerThread(Core::System* system)
{
  while (s_running.load())
  {
    HANDLE pipe = CreateNamedPipeW(
        PipeName(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        PIPE_BUF,
        PIPE_BUF,
        0,
        nullptr);

    if (pipe == INVALID_HANDLE_VALUE)
      break;

    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
    {
      CloseHandle(pipe);
      continue;
    }

    if (!s_running.load())
    {
      DisconnectNamedPipe(pipe);
      CloseHandle(pipe);
      break;
    }

    // Accumulate one newline-terminated request. Read in chunks so large advanceseq payloads
    // (hundreds of input frames) aren't capped at the pipe buffer size.
    std::string request;
    bool got_newline = false;
    char chunk[PIPE_BUF];
    while (request.size() < (4u << 20))  // 4 MB guard against a runaway sender
    {
      DWORD nread = 0;
      if (!ReadFile(pipe, chunk, sizeof(chunk), &nread, nullptr) || nread == 0)
        break;
      request.append(chunk, nread);
      if (memchr(chunk, '\n', nread))
      {
        got_newline = true;
        break;
      }
    }

    if (got_newline)
    {
      const auto nl = request.find('\n');
      if (nl != std::string::npos)
        request.resize(nl);
      HandleRequest(pipe, request.c_str(), system);
    }

    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
  }
}
}  // namespace

void StartControlPipe(Core::System& system, HostHooks hooks)
{
  s_system = &system;
  s_hooks = std::move(hooks);

  // Tap the log stream once; LogManager owns the listener for the rest of the run.
  if (!s_log_listener)
  {
    if (auto* lm = Common::Log::LogManager::GetInstance())
    {
      auto listener = std::make_unique<RingLogListener>();
      s_log_listener = listener.get();
      lm->RegisterListener(Common::Log::LogListener::PIPE_LISTENER, std::move(listener));
      lm->EnableListener(Common::Log::LogListener::PIPE_LISTENER, true);
    }
  }

  s_running.store(true);
  s_thread = std::thread(PipeServerThread, &system);
}

void StopControlPipe()
{
  if (!s_running.exchange(false))
    return;

  HANDLE h = CreateFileW(PipeName(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h != INVALID_HANDLE_VALUE)
    CloseHandle(h);

  if (s_thread.joinable())
    s_thread.join();

  s_hooks = {};
  s_system = nullptr;
}

}  // namespace Scripting

#else  // !_WIN32

namespace Scripting
{
void StartControlPipe(Core::System&, HostHooks) {}
void StopControlPipe() {}
}  // namespace Scripting

#endif
