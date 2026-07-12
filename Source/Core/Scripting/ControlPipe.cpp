// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Scripting/ControlPipe.h"

#ifdef _WIN32

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <windows.h>

#include "Common/Event.h"
#include "Common/Logging/LogManager.h"
#include "Core/API/Events.h"
#include "Core/Core.h"
#include "Core/HW/CPU.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/GCPadEmu.h"
#include "Core/Movie.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/Expression.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
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

bool WriteResponseWithPC(HANDLE pipe, const char* state, u32 pc)
{
  char buf[96];
  int len = snprintf(buf, sizeof(buf), "{\"ok\":true,\"state\":\"%s\",\"pc\":%u}\n", state, pc);
  return WriteRaw(pipe, buf, len);
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

bool ParseCommandName(const char* buf, char* out, int outlen)
{
  return ParseStringField(buf, "\"cmd\"", out, outlen) ||
         ParseStringField(buf, "\"command\"", out, outlen);
}

bool CommandMatches(const char* buf, const char* parsed_command, const char* command)
{
  if (parsed_command && parsed_command[0] != '\0')
    return strcmp(parsed_command, command) == 0;

  char legacy_token[64];
  snprintf(legacy_token, sizeof(legacy_token), "\"%s\"", command);
  return strstr(buf, legacy_token) != nullptr;
}

// Advance past "key" and its value separator, returning the start of the value (or nullptr).
const char* ValueAfter(const char* buf, const char* key)
{
  const char* p = strstr(buf, key);
  if (!p)
    return nullptr;
  p += strlen(key);
  while (*p == ' ' || *p == ':' || *p == '"')
    p++;
  return p;
}

// Parse an unsigned field, accepting hex (0x...) or decimal. Addresses arrive either way.
bool ParseU64Field(const char* buf, const char* key, u64& out)
{
  const char* p = ValueAfter(buf, key);
  if (!p)
    return false;
  out = strtoull(p, nullptr, 0);
  return true;
}

// Parse a bool field, accepting JSON true/false or 0/1. Missing key yields def.
bool ParseBoolField(const char* buf, const char* key, bool def)
{
  const char* p = ValueAfter(buf, key);
  if (!p)
    return def;
  if (strncmp(p, "true", 4) == 0)
    return true;
  if (strncmp(p, "false", 5) == 0)
    return false;
  return strtoll(p, nullptr, 10) != 0;
}

bool ParseDoubleField(const char* buf, const char* key, double& out)
{
  const char* p = ValueAfter(buf, key);
  if (!p)
    return false;
  out = strtod(p, nullptr);
  return true;
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

// --- Debugger: registers, breakpoints, stepping ---

enum class RegKind
{
  GPR,
  FPR,
  SPR,
  PC,
  NPC,
  LR,
  CTR,
  MSR,
  CR,
  XER,
  FPSCR,
  SRR0,
  SRR1,
};

struct RegRef
{
  RegKind kind;
  u32 index = 0;
};

// Resolve a register name (case-insensitive) to a kind + index. Accepts pc/npc/lr/ctr/msr/cr/
// xer/fpscr/srr0/srr1, gprN or rN (sp=r1), fprN or fN, sprN.
bool ParseRegName(const char* name, RegRef& out)
{
  std::string n;
  for (const char* c = name; *c; ++c)
    n += static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));

  // Parse the digits following a prefix; returns false unless the whole tail is digits.
  auto tail_index = [&n](size_t pfx, u32 limit, u32& idx) -> bool {
    if (n.size() <= pfx)
      return false;
    for (size_t i = pfx; i < n.size(); ++i)
      if (!std::isdigit(static_cast<unsigned char>(n[i])))
        return false;
    idx = static_cast<u32>(strtoul(n.c_str() + pfx, nullptr, 10));
    return idx < limit;
  };

  u32 idx = 0;
  if (n == "pc") { out = {RegKind::PC}; return true; }
  if (n == "npc") { out = {RegKind::NPC}; return true; }
  if (n == "lr") { out = {RegKind::LR}; return true; }
  if (n == "ctr") { out = {RegKind::CTR}; return true; }
  if (n == "msr") { out = {RegKind::MSR}; return true; }
  if (n == "cr") { out = {RegKind::CR}; return true; }
  if (n == "xer") { out = {RegKind::XER}; return true; }
  if (n == "fpscr") { out = {RegKind::FPSCR}; return true; }
  if (n == "srr0") { out = {RegKind::SRR0}; return true; }
  if (n == "srr1") { out = {RegKind::SRR1}; return true; }
  if (n == "sp") { out = {RegKind::GPR, 1}; return true; }
  if (n.rfind("gpr", 0) == 0 && tail_index(3, 32, idx)) { out = {RegKind::GPR, idx}; return true; }
  if (n[0] == 'r' && tail_index(1, 32, idx)) { out = {RegKind::GPR, idx}; return true; }
  if (n.rfind("fpr", 0) == 0 && tail_index(3, 32, idx)) { out = {RegKind::FPR, idx}; return true; }
  if (n[0] == 'f' && tail_index(1, 32, idx)) { out = {RegKind::FPR, idx}; return true; }
  if (n.rfind("spr", 0) == 0 && tail_index(3, 1024, idx)) { out = {RegKind::SPR, idx}; return true; }
  return false;
}

// Read a register. Must run on the CPU thread. Sets is_fpr + dbl for float registers.
bool ReadReg(PowerPC::PowerPCState& st, const RegRef& r, u64& raw, double& dbl, bool& is_fpr)
{
  is_fpr = false;
  switch (r.kind)
  {
  case RegKind::GPR:   raw = st.gpr[r.index]; return true;
  case RegKind::FPR:   raw = st.ps[r.index].PS0AsU64(); dbl = st.ps[r.index].PS0AsDouble();
                       is_fpr = true; return true;
  case RegKind::SPR:   raw = st.spr[r.index]; return true;
  case RegKind::PC:    raw = st.pc; return true;
  case RegKind::NPC:   raw = st.npc; return true;
  case RegKind::LR:    raw = LR(st); return true;
  case RegKind::CTR:   raw = CTR(st); return true;
  case RegKind::MSR:   raw = st.msr.Hex; return true;
  case RegKind::CR:    raw = st.cr.Get(); return true;
  case RegKind::XER:   raw = st.GetXER().Hex; return true;
  case RegKind::FPSCR: raw = st.fpscr.Hex; return true;
  case RegKind::SRR0:  raw = SRR0(st); return true;
  case RegKind::SRR1:  raw = SRR1(st); return true;
  }
  return false;
}

// Write a register. Must run on the CPU thread. For FPRs, has_double picks SetPS0(double) vs raw bits.
bool WriteReg(PowerPC::PowerPCState& st, const RegRef& r, u64 val, bool has_double, double dval)
{
  const auto v32 = static_cast<u32>(val);
  switch (r.kind)
  {
  case RegKind::GPR:   st.gpr[r.index] = v32; return true;
  case RegKind::FPR:   if (has_double) st.ps[r.index].SetPS0(dval); else st.ps[r.index].SetPS0(val);
                       return true;
  case RegKind::SPR:   st.spr[r.index] = v32; return true;
  case RegKind::PC:    st.pc = v32; st.npc = v32; return true;  // redirect flow: keep npc in step
  case RegKind::NPC:   st.npc = v32; return true;
  case RegKind::LR:    LR(st) = v32; return true;
  case RegKind::CTR:   CTR(st) = v32; return true;
  case RegKind::MSR:   st.msr.Hex = v32; return true;
  case RegKind::CR:    st.cr.Set(v32); return true;
  case RegKind::XER:   st.SetXER(UReg_XER{v32}); return true;
  case RegKind::FPSCR: st.fpscr.Hex = v32; return true;
  case RegKind::SRR0:  SRR0(st) = v32; return true;
  case RegKind::SRR1:  SRR1(st) = v32; return true;
  }
  return false;
}

// True on a rfi, blr, or a bclr that would return (ported from CodeWidget::WillInstructionReturn).
bool WillInstructionReturn(Core::System& system, UGeckoInstruction inst)
{
  if (inst.hex == 0x4C000064u)
    return true;
  const auto& ppc_state = system.GetPPCState();
  const bool counter =
      (inst.BO_2 >> 2 & 1) != 0 || (CTR(ppc_state) != 0) != ((inst.BO_2 >> 1 & 1) != 0);
  const bool condition = inst.BO_2 >> 4 != 0 || ppc_state.cr.GetBit(inst.BI_2) == (inst.BO_2 >> 3 & 1);
  const bool is_bclr = inst.OPCD_7 == 0b010011 && inst.XO == 16;
  return is_bclr && counter && condition && !inst.LK_3;
}

// Single-step one instruction (interpreter). Requires the core to be stepping; breaks first if not.
// Mirrors CodeWidget::Step — runs on the pipe thread, signaling the parked CPU thread.
void DoStepInto(Core::System* system)
{
  auto& cpu = system->GetCPU();
  if (!cpu.IsStepping())
    cpu.SetStepping(true);

  auto& power_pc = system->GetPowerPC();
  const PowerPC::CoreMode old_mode = power_pc.GetMode();
  power_pc.SetMode(PowerPC::CoreMode::Interpreter);
  Common::Event sync_event;
  cpu.StepOpcode(&sync_event);
  sync_event.WaitFor(std::chrono::milliseconds(200));
  power_pc.SetMode(old_mode);
}

// Read PC after a synchronous step. The core is parked in stepping, so a direct read is race-free.
u32 CurrentPC(Core::System* system)
{
  return system->GetPPCState().pc;
}

// Handles the debugger commands; returns true if buf was one. Declared here, defined below.
bool HandleDebugRequest(HANDLE pipe, const char* buf, Core::System* system, Core::State state);

void HandleRequest(HANDLE pipe, const char* buf, Core::System* system)
{
  char command[64] = {};
  ParseCommandName(buf, command, sizeof(command));
  Core::State state = Core::GetState(*system);

  // Dispatched first: a breakpoint's free-text condition could otherwise contain another
  // command's name and mis-route through the substring matching below.
  if (HandleDebugRequest(pipe, buf, system, state))
    return;

  if (CommandMatches(buf, command, "ping"))
  {
    const char* resp = "{\"ok\":true,\"result\":\"pong\"}\n";
    WriteRaw(pipe, resp, static_cast<int>(strlen(resp)));
  }
  else if (CommandMatches(buf, command, "status"))
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
  else if (CommandMatches(buf, command, "pause"))
  {
    Core::SetState(*system, Core::State::Paused);
    WriteResponse(pipe, true, "paused");
  }
  else if (CommandMatches(buf, command, "resume"))
  {
    Core::SetState(*system, Core::State::Running);
    WriteResponse(pipe, true, "running");
  }
  else if (CommandMatches(buf, command, "toggle"))
  {
    Core::State next =
        (state == Core::State::Paused) ? Core::State::Running : Core::State::Paused;
    Core::SetState(*system, next);
    WriteResponse(pipe, true, StateToString(next));
  }
  else if (CommandMatches(buf, command, "frame"))
  {
    u64 frame = system->GetMovie().GetCurrentFrame();
    char resp[64];
    int len = snprintf(resp, sizeof(resp), "{\"ok\":true,\"frame\":%" PRIu64 "}\n", frame);
    WriteRaw(pipe, resp, len);
  }
  else if (CommandMatches(buf, command, "advance"))
  {
    long long n = ParseIntField(buf, "\"frames\"");
    if (n <= 0)
      n = 1;

    u64 landed = StepInputFrames(system, n);
    WriteResponseWithFrame(pipe, true, "paused", landed);
  }
  else if (CommandMatches(buf, command, "advanceseq"))
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
  else if (CommandMatches(buf, command, "advancewith"))
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
  else if (CommandMatches(buf, command, "setinput"))
  {
    long long port = ParseIntField(buf, "\"port\"");
    int ctrl = static_cast<int>(port < 0 ? 0 : port);
    PadOverride ov = ParsePadOverride(buf);
    ApplyPadOverrideOnCPU(system, ctrl, ov);
    WriteResponse(pipe, true, StateToString(state));
  }
  else if (CommandMatches(buf, command, "clearinput"))
  {
    long long port = ParseIntField(buf, "\"port\"");
    int ctrl = static_cast<int>(port < 0 ? 0 : port);
    PadOverride clear_ov;
    ApplyPadOverrideOnCPU(system, ctrl, clear_ov);
    WriteResponse(pipe, true, StateToString(state));
  }
  else if (CommandMatches(buf, command, "savestate"))
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
  else if (CommandMatches(buf, command, "enablescript") || CommandMatches(buf, command, "disablescript"))
  {
    const bool enable = CommandMatches(buf, command, "enablescript");
    char path[512] = {};
    if (!ParseStringField(buf, "\"path\"", path, sizeof(path)))
      WriteResponse(pipe, false, StateToString(state));
    else
      WriteResponse(pipe, Scripts::SetEnabled(path, enable), StateToString(state));
  }
  else if (CommandMatches(buf, command, "restartscript"))
  {
    char path[512] = {};
    if (!ParseStringField(buf, "\"path\"", path, sizeof(path)))
      WriteResponse(pipe, false, StateToString(state));
    else
      WriteResponse(pipe, Scripts::Restart(path), StateToString(state));
  }
  else if (CommandMatches(buf, command, "listscripts"))
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
  else if (CommandMatches(buf, command, "boot"))
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
  else if (CommandMatches(buf, command, "stop"))
  {
    if (!s_hooks.stop)
      WriteResponse(pipe, false, StateToString(state));
    else
    {
      s_hooks.stop();
      WriteResponse(pipe, true, "stopping");
    }
  }
  else if (CommandMatches(buf, command, "reset"))
  {
    if (!s_hooks.reset)
      WriteResponse(pipe, false, StateToString(state));
    else
    {
      s_hooks.reset();
      WriteResponse(pipe, true, StateToString(state));
    }
  }
  else if (CommandMatches(buf, command, "listgames"))
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
  else if (CommandMatches(buf, command, "recordstart"))
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
  else if (CommandMatches(buf, command, "recordstop"))
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
  else if (CommandMatches(buf, command, "playmovie"))
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
  else if (CommandMatches(buf, command, "logconfig"))
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
  else if (CommandMatches(buf, command, "logenable"))
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
  else if (CommandMatches(buf, command, "logverbosity"))
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
  else if (CommandMatches(buf, command, "log"))
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

bool HandleDebugRequest(HANDLE pipe, const char* buf, Core::System* system, Core::State state)
{
  char command[64] = {};
  ParseCommandName(buf, command, sizeof(command));
  if (CommandMatches(buf, command, "setbp"))
  {
    u64 addr = 0;
    if (state == Core::State::Uninitialized || !ParseU64Field(buf, "\"addr\"", addr))
    {
      WriteResponse(pipe, false, StateToString(state));
    }
    else
    {
      const bool brk = ParseBoolField(buf, "\"break_on_hit\"", true);
      const bool log = ParseBoolField(buf, "\"log_on_hit\"", false);
      char cond[256] = {};
      std::optional<Expression> expr;
      if (ParseStringField(buf, "\"condition\"", cond, sizeof(cond)))
        expr = Expression::TryParse(std::string_view(cond));
      Core::RunOnCPUThread(
          *system,
          [&] {
            system->GetPowerPC().GetBreakPoints().Add(static_cast<u32>(addr), brk, log,
                                                      std::move(expr));
          },
          true);
      WriteResponse(pipe, true, StateToString(state));
    }
  }
  else if (CommandMatches(buf, command, "removebp"))
  {
    u64 addr = 0;
    bool ok = false;
    if (state != Core::State::Uninitialized && ParseU64Field(buf, "\"addr\"", addr))
    {
      Core::RunOnCPUThread(
          *system,
          [&] { ok = system->GetPowerPC().GetBreakPoints().Remove(static_cast<u32>(addr)); }, true);
    }
    WriteResponse(pipe, ok, StateToString(state));
  }
  else if (CommandMatches(buf, command, "clearbp"))
  {
    if (state == Core::State::Uninitialized)
    {
      WriteResponse(pipe, false, StateToString(state));
    }
    else
    {
      Core::RunOnCPUThread(
          *system, [&] { system->GetPowerPC().GetBreakPoints().Clear(); }, true);
      WriteResponse(pipe, true, StateToString(state));
    }
  }
  else if (CommandMatches(buf, command, "listbp"))
  {
    struct BpInfo
    {
      u32 addr;
      bool enabled, brk, log, cond;
    };
    std::vector<BpInfo> infos;
    if (state != Core::State::Uninitialized)
    {
      Core::RunOnCPUThread(
          *system,
          [&] {
            for (const auto& bp : system->GetPowerPC().GetBreakPoints().GetBreakPoints())
              infos.push_back({bp.address, bp.is_enabled, bp.break_on_hit, bp.log_on_hit,
                               bp.condition.has_value()});
          },
          true);
    }
    std::string resp = "{\"ok\":true,\"breakpoints\":[";
    bool first = true;
    for (const auto& b : infos)
    {
      if (!first)
        resp += ',';
      first = false;
      char item[160];
      snprintf(item, sizeof(item),
               "{\"addr\":%u,\"enabled\":%s,\"break\":%s,\"log\":%s,\"condition\":%s}", b.addr,
               b.enabled ? "true" : "false", b.brk ? "true" : "false", b.log ? "true" : "false",
               b.cond ? "true" : "false");
      resp += item;
    }
    resp += "]}\n";
    WriteString(pipe, resp);
  }
  else if (CommandMatches(buf, command, "setmbp"))
  {
    u64 at = 0, start = 0, end = 0;
    const bool has_at = ParseU64Field(buf, "\"at\"", at);
    const bool has_range = ParseU64Field(buf, "\"start\"", start) &&
                           ParseU64Field(buf, "\"end\"", end);
    if (state == Core::State::Uninitialized || (!has_at && !has_range))
    {
      WriteResponse(pipe, false, StateToString(state));
    }
    else
    {
      TMemCheck check;
      if (has_at)
      {
        check.start_address = check.end_address = static_cast<u32>(at);
        check.is_ranged = false;
      }
      else
      {
        check.start_address = static_cast<u32>(start);
        check.end_address = static_cast<u32>(end);
        check.is_ranged = true;
      }
      check.is_break_on_read = ParseBoolField(buf, "\"read\"", true);
      check.is_break_on_write = ParseBoolField(buf, "\"write\"", true);
      check.log_on_hit = ParseBoolField(buf, "\"log_on_hit\"", false);
      check.break_on_hit = ParseBoolField(buf, "\"break_on_hit\"", true);
      check.is_enabled = true;
      char cond[256] = {};
      if (ParseStringField(buf, "\"condition\"", cond, sizeof(cond)))
        check.condition = Expression::TryParse(std::string_view(cond));
      Core::RunOnCPUThread(
          *system, [&] { system->GetPowerPC().GetMemChecks().Add(std::move(check)); }, true);
      WriteResponse(pipe, true, StateToString(state));
    }
  }
  else if (CommandMatches(buf, command, "removembp"))
  {
    u64 addr = 0;
    if (state == Core::State::Uninitialized || !ParseU64Field(buf, "\"addr\"", addr))
    {
      WriteResponse(pipe, false, StateToString(state));
    }
    else
    {
      Core::RunOnCPUThread(
          *system, [&] { system->GetPowerPC().GetMemChecks().Remove(static_cast<u32>(addr)); },
          true);
      WriteResponse(pipe, true, StateToString(state));
    }
  }
  else if (CommandMatches(buf, command, "clearmbp"))
  {
    if (state == Core::State::Uninitialized)
    {
      WriteResponse(pipe, false, StateToString(state));
    }
    else
    {
      Core::RunOnCPUThread(
          *system, [&] { system->GetPowerPC().GetMemChecks().Clear(); }, true);
      WriteResponse(pipe, true, StateToString(state));
    }
  }
  else if (CommandMatches(buf, command, "listmbp"))
  {
    struct McInfo
    {
      u32 start, end, hits;
      bool ranged, read, write, log, brk, enabled, cond;
    };
    std::vector<McInfo> infos;
    if (state != Core::State::Uninitialized)
    {
      Core::RunOnCPUThread(
          *system,
          [&] {
            for (const auto& mc : system->GetPowerPC().GetMemChecks().GetMemChecks())
              infos.push_back({mc.start_address, mc.end_address, mc.num_hits, mc.is_ranged,
                               mc.is_break_on_read, mc.is_break_on_write, mc.log_on_hit,
                               mc.break_on_hit, mc.is_enabled, mc.condition.has_value()});
          },
          true);
    }
    std::string resp = "{\"ok\":true,\"membreakpoints\":[";
    bool first = true;
    for (const auto& m : infos)
    {
      if (!first)
        resp += ',';
      first = false;
      char item[256];
      snprintf(item, sizeof(item),
               "{\"start\":%u,\"end\":%u,\"ranged\":%s,\"read\":%s,\"write\":%s,\"break\":%s,"
               "\"log\":%s,\"enabled\":%s,\"condition\":%s,\"hits\":%u}",
               m.start, m.end, m.ranged ? "true" : "false", m.read ? "true" : "false",
               m.write ? "true" : "false", m.brk ? "true" : "false", m.log ? "true" : "false",
               m.enabled ? "true" : "false", m.cond ? "true" : "false", m.hits);
      resp += item;
    }
    resp += "]}\n";
    WriteString(pipe, resp);
  }
  else if (CommandMatches(buf, command, "stepover"))
  {
    auto& cpu = system->GetCPU();
    if (state == Core::State::Uninitialized)
    {
      WriteResponse(pipe, false, StateToString(state));
    }
    else
    {
      if (!cpu.IsStepping())
        cpu.SetStepping(true);
      const u32 pc = system->GetPPCState().pc;
      UGeckoInstruction inst;
      {
        Core::CPUThreadGuard guard(*system);
        inst = PowerPC::MMU::HostRead_Instruction(guard, pc);
      }
      if (inst.LK)
      {
        // A call: break at the return address and resume; the client polls status for "paused".
        system->GetPowerPC().GetBreakPoints().SetTemporary(pc + 4);
        cpu.SetStepping(false);
        char resp[96];
        int len = snprintf(resp, sizeof(resp),
                           "{\"ok\":true,\"state\":\"running\",\"stepping_over\":true,\"target\":%u}\n",
                           pc + 4);
        WriteRaw(pipe, resp, len);
      }
      else
      {
        DoStepInto(system);
        WriteResponseWithPC(pipe, "paused", CurrentPC(system));
      }
    }
  }
  else if (CommandMatches(buf, command, "stepout"))
  {
    if (state == Core::State::Uninitialized)
    {
      WriteResponse(pipe, false, StateToString(state));
    }
    else
    {
      auto& cpu = system->GetCPU();
      if (!cpu.IsStepping())
        cpu.SetStepping(true);

      using clock = std::chrono::steady_clock;
      const clock::time_point timeout = clock::now() + std::chrono::seconds(5);
      auto& power_pc = system->GetPowerPC();
      {
        auto& ppc_state = power_pc.GetPPCState();
        Core::CPUThreadGuard guard(*system);
        const PowerPC::CoreMode old_mode = power_pc.GetMode();
        power_pc.SetMode(PowerPC::CoreMode::Interpreter);

        UGeckoInstruction inst = PowerPC::MMU::HostRead_Instruction(guard, ppc_state.pc);
        do
        {
          if (WillInstructionReturn(*system, inst))
          {
            power_pc.SingleStep();
            break;
          }
          if (inst.LK)
          {
            const u32 next_pc = ppc_state.pc + 4;
            do
            {
              power_pc.SingleStep();
            } while (ppc_state.pc != next_pc && clock::now() < timeout &&
                     !power_pc.CheckBreakPoints());
          }
          else
          {
            power_pc.SingleStep();
          }
          inst = PowerPC::MMU::HostRead_Instruction(guard, ppc_state.pc);
        } while (clock::now() < timeout && !power_pc.CheckBreakPoints());

        power_pc.SetMode(old_mode);
      }
      WriteResponseWithPC(pipe, "paused", CurrentPC(system));
    }
  }
  else if (CommandMatches(buf, command, "stepin"))
  {
    if (state == Core::State::Uninitialized)
    {
      WriteResponse(pipe, false, StateToString(state));
    }
    else
    {
      DoStepInto(system);
      WriteResponseWithPC(pipe, "paused", CurrentPC(system));
    }
  }
  else if (CommandMatches(buf, command, "readreg"))
  {
    char name[32] = {};
    RegRef ref;
    if (state == Core::State::Uninitialized ||
        !ParseStringField(buf, "\"reg\"", name, sizeof(name)) || !ParseRegName(name, ref))
    {
      WriteResponse(pipe, false, StateToString(state));
    }
    else
    {
      u64 raw = 0;
      double dbl = 0.0;
      bool is_fpr = false;
      Core::RunOnCPUThread(
          *system, [&] { ReadReg(system->GetPPCState(), ref, raw, dbl, is_fpr); }, true);
      char resp[192];
      if (is_fpr)
        snprintf(resp, sizeof(resp),
                 "{\"ok\":true,\"reg\":\"%s\",\"bits\":%" PRIu64 ",\"double\":%.17g}\n", name, raw,
                 dbl);
      else
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"reg\":\"%s\",\"value\":%" PRIu64 "}\n", name,
                 raw);
      WriteString(pipe, resp);
    }
  }
  else if (CommandMatches(buf, command, "writereg"))
  {
    char name[32] = {};
    RegRef ref;
    u64 val = 0;
    double dval = 0.0;
    const bool has_double = ParseDoubleField(buf, "\"double\"", dval);
    ParseU64Field(buf, "\"value\"", val);
    bool ok = false;
    if (state != Core::State::Uninitialized &&
        ParseStringField(buf, "\"reg\"", name, sizeof(name)) && ParseRegName(name, ref))
    {
      Core::RunOnCPUThread(
          *system, [&] { ok = WriteReg(system->GetPPCState(), ref, val, has_double, dval); }, true);
    }
    WriteResponse(pipe, ok, StateToString(state));
  }
  else if (CommandMatches(buf, command, "regs"))
  {
    if (state == Core::State::Uninitialized)
    {
      WriteResponse(pipe, false, StateToString(state));
    }
    else
    {
      const bool want_fpr = strstr(buf, "\"fpr\"") != nullptr;
      struct Dump
      {
        u32 pc, npc, lr, ctr, cr, xer, msr, fpscr;
        u32 gpr[32];
        u64 ps0[32];
        double psd[32];
      } d{};
      Core::RunOnCPUThread(
          *system,
          [&] {
            auto& st = system->GetPPCState();
            d.pc = st.pc;
            d.npc = st.npc;
            d.lr = LR(st);
            d.ctr = CTR(st);
            d.cr = st.cr.Get();
            d.xer = st.GetXER().Hex;
            d.msr = st.msr.Hex;
            d.fpscr = st.fpscr.Hex;
            for (int i = 0; i < 32; ++i)
            {
              d.gpr[i] = st.gpr[i];
              if (want_fpr)
              {
                d.ps0[i] = st.ps[i].PS0AsU64();
                d.psd[i] = st.ps[i].PS0AsDouble();
              }
            }
          },
          true);
      std::string resp = "{\"ok\":true";
      char hdr[256];
      snprintf(hdr, sizeof(hdr),
               ",\"pc\":%u,\"npc\":%u,\"lr\":%u,\"ctr\":%u,\"cr\":%u,\"xer\":%u,\"msr\":%u,"
               "\"fpscr\":%u,\"gpr\":[",
               d.pc, d.npc, d.lr, d.ctr, d.cr, d.xer, d.msr, d.fpscr);
      resp += hdr;
      for (int i = 0; i < 32; ++i)
      {
        if (i)
          resp += ',';
        resp += std::to_string(d.gpr[i]);
      }
      resp += ']';
      if (want_fpr)
      {
        resp += ",\"fpr\":[";
        for (int i = 0; i < 32; ++i)
        {
          if (i)
            resp += ',';
          char f[64];
          snprintf(f, sizeof(f), "{\"bits\":%" PRIu64 ",\"double\":%.17g}", d.ps0[i], d.psd[i]);
          resp += f;
        }
        resp += ']';
      }
      resp += "}\n";
      WriteString(pipe, resp);
    }
  }
  else
  {
    return false;
  }
  return true;
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
  if (s_running.exchange(true))
  {
    s_system = &system;
    s_hooks = std::move(hooks);
    return;
  }

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

  // Drop any override a client left held so disabling the pipe hands input back to the player.
  if (s_system)
  {
    const PadOverride clear;
    for (int port = 0; port < 4; ++port)
      ApplyPadOverrideOnCPU(s_system, port, clear);
  }

  s_hooks = {};
  s_system = nullptr;
}

bool IsControlPipeRunning()
{
  return s_running.load();
}

}  // namespace Scripting

#else  // !_WIN32

namespace Scripting
{
void StartControlPipe(Core::System&, HostHooks) {}
void StopControlPipe() {}
bool IsControlPipeRunning() { return false; }
}  // namespace Scripting

#endif
