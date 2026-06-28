// Copyright 2023 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "Scripting/ScriptingEngine.h"

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Scripts
{

void StartPendingScripts();
void StopAllScripts();

// Thread-safe; paths matched tolerantly. SetEnabled queues a pending script when emulation
// hasn't started, like the GUI checkbox. Each returns true if the script map changed.
bool SetEnabled(const std::string& path, bool enabled);
bool Restart(const std::string& path);
bool IsEnabled(const std::string& path);

struct ScriptInfo
{
  std::string path;
  bool enabled;  // present in the script map (running or queued)
  bool running;  // backend constructed (false while only pending)
};

// Scripts available in the configured directory (ScriptsPath), overlaid with live state.
std::vector<ScriptInfo> List();

// The configured scripts directory (ScriptsPath / D_SCRIPTS_IDX), forward-slash form.
std::string ScriptsDir();

// Registered once at startup so non-GUI callers (the pipe) can refresh the Scripts panel.
void SetChangeCallback(std::function<void()> callback);

// extern so that different translation units can access a global instance of these vars
// i.e. DolphinLib needs to access these variables even though they're housed in the Scripting unit
extern std::unordered_map<std::string, Scripting::ScriptingBackend*> g_scripts;
extern bool g_scripts_started;
// Guards all g_scripts/g_scripts_started access now that the control pipe is a third mutator.
extern std::mutex g_scripts_mutex;
}
