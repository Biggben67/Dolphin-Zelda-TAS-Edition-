// Copyright 2023 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Scripting/ScriptList.h"

#include <atomic>
#include <filesystem>
#include <map>
#include <system_error>

#include "Common/FileUtil.h"

namespace Scripts
{
namespace
{
std::function<void()> s_on_change;

// Nonzero while a backend ctor runs; the GUI-thread enable path pumps the Qt event loop from inside
// the ctor wait, so GUI pollers must not re-enter a half-built subinterpreter.
std::atomic<int> s_constructing{0};

<<<<<<< Updated upstream
Scripting::ScriptingBackend* NewBackend(const std::string& key)
{
  ++s_constructing;
  auto* backend = new Scripting::ScriptingBackend(key);
  --s_constructing;
  return backend;
=======
class ConstructingScope
{
public:
  ConstructingScope() { s_constructing.fetch_add(1, std::memory_order_relaxed); }
  ~ConstructingScope() { s_constructing.fetch_sub(1, std::memory_order_relaxed); }
  ConstructingScope(const ConstructingScope&) = delete;
  ConstructingScope& operator=(const ConstructingScope&) = delete;
};

Scripting::ScriptingBackend* NewBackend(const std::string& key)
{
  ConstructingScope constructing;
  return new Scripting::ScriptingBackend(key);
>>>>>>> Stashed changes
}

// Destruction also marshals onto the CPU thread and, from the GUI, pumps the event loop while it
// waits -- guard it like construction so the window Sync and HostUpdate tick skip a tearing-down script.
void DeleteBackend(Scripting::ScriptingBackend* backend)
{
<<<<<<< Updated upstream
  ++s_constructing;
  delete backend;
  --s_constructing;
=======
  ConstructingScope constructing;
  delete backend;
>>>>>>> Stashed changes
}

// Fire the registered refresh callback off-lock (it may marshal onto the GUI thread).
void NotifyChanged()
{
  if (s_on_change)
    s_on_change();
}

// Forward-slash absolute form used as the canonical map key and List() output.
std::string NormalizeKey(const std::string& path)
{
  std::error_code ec;
  std::filesystem::path p = std::filesystem::weakly_canonical(path, ec);
  if (ec)
    p = std::filesystem::absolute(path, ec);
  return ec ? path : p.generic_string();
}

// Find an existing entry for path, tolerating separator/case differences vs the stored key.
// Caller must hold g_scripts_mutex.
std::unordered_map<std::string, Scripting::ScriptingBackend*>::iterator FindEntry(
    const std::string& path)
{
  auto it = g_scripts.find(path);
  if (it != g_scripts.end())
    return it;
  it = g_scripts.find(NormalizeKey(path));
  if (it != g_scripts.end())
    return it;
  std::error_code ec;
  for (auto i = g_scripts.begin(); i != g_scripts.end(); ++i)
  {
    if (std::filesystem::equivalent(i->first, path, ec) && !ec)
      return i;
  }
  return g_scripts.end();
}
}  // namespace

void StartPendingScripts()
{
  std::lock_guard lock(g_scripts_mutex);
  // NOTE: This may enable scripts in an arbitrary order
  for (auto it = g_scripts.begin(); it != g_scripts.end(); it++)
  {
    if (!it->second)
      it->second = NewBackend(it->first);
  }
  g_scripts_started = true;
}

// called on game close
// maintain enabled flag, but delete backends
void StopAllScripts()
{
  std::lock_guard lock(g_scripts_mutex);
  for (auto it = g_scripts.begin(); it != g_scripts.end(); it++)
  {
    if (it->second)
    {
      DeleteBackend(it->second);
      it->second = nullptr;
    }
  }
  g_scripts_started = false;
}

bool SetEnabled(const std::string& path, bool enabled)
{
  bool changed = false;
  {
    std::lock_guard lock(g_scripts_mutex);
    auto it = FindEntry(path);

    if (enabled)
    {
      if (it == g_scripts.end())
      {
        // Construct under the lock: the backend ctor marshals onto the CPU thread, which never
        // takes this mutex, so there is no deadlock and the map stays consistent.
        Scripting::ScriptingBackend* backend =
            g_scripts_started ? NewBackend(NormalizeKey(path)) : nullptr;
        g_scripts[NormalizeKey(path)] = backend;
        changed = true;
      }
    }
    else if (it != g_scripts.end())
    {
      DeleteBackend(it->second);
      g_scripts.erase(it);
      changed = true;
    }
  }
  if (changed)
    NotifyChanged();
  return changed;
}

bool Restart(const std::string& path)
{
  bool changed = false;
  {
    std::lock_guard lock(g_scripts_mutex);
    auto it = FindEntry(path);
    if (it != g_scripts.end() && it->second != nullptr)
    {
      const std::string key = it->first;
      DeleteBackend(it->second);
      it->second = NewBackend(key);
      changed = true;
    }
  }
  if (changed)
    NotifyChanged();
  return changed;
}

bool IsEnabled(const std::string& path)
{
  std::lock_guard lock(g_scripts_mutex);
  return FindEntry(path) != g_scripts.end();
}

std::string ScriptsDir()
{
  return std::filesystem::path(File::GetUserPath(D_SCRIPTS_IDX)).generic_string();
}

std::vector<ScriptInfo> List()
{
  std::map<std::string, ScriptInfo> by_path;

  // Available scripts in the configured directory (ScriptsPath), recursing into game subfolders.
  std::error_code ec;
  const std::string dir = File::GetUserPath(D_SCRIPTS_IDX);
  auto it = std::filesystem::recursive_directory_iterator(
      dir, std::filesystem::directory_options::skip_permission_denied, ec);
  for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
  {
    const std::string name = it->path().filename().string();
    if (it->is_directory(ec))
    {
      // Skip hidden dirs (.git) and the Modules import dir.
      if (!name.empty() && (name[0] == '.' || name == "Modules"))
        it.disable_recursion_pending();
      continue;
    }
    const std::string ext = it->path().extension().string();
    if ((ext != ".py" && ext != ".py3") || name == "__init__.py")
      continue;
    const std::string key = NormalizeKey(it->path().string());
    by_path[key] = ScriptInfo{key, false, false};
  }

  // Overlay live enabled/running state; normalize keys so auto-run entries dedupe with disk paths.
  {
    std::lock_guard lock(g_scripts_mutex);
    for (const auto& [path, backend] : g_scripts)
    {
      const std::string key = NormalizeKey(path);
      by_path[key] = ScriptInfo{key, true, backend != nullptr};
    }
  }

  std::vector<ScriptInfo> out;
  out.reserve(by_path.size());
  for (auto& [key, info] : by_path)
    out.push_back(std::move(info));
  return out;
}

void SetChangeCallback(std::function<void()> callback)
{
  s_on_change = std::move(callback);
}

bool IsConstructing()
{
  return s_constructing.load(std::memory_order_relaxed) != 0;
}

std::unordered_map<std::string, Scripting::ScriptingBackend*> g_scripts = {};
bool g_scripts_started = false;
std::recursive_mutex g_scripts_mutex;
}  // namespace Scripts
