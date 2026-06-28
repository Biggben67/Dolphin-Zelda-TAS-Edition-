// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace Core
{
class System;
}

namespace Scripting
{

// One selectable game, as surfaced to pipe clients by the list_games hook.
struct GameEntry
{
  std::string id;
  std::string name;
  std::string path;
  std::string platform;
  std::string region;
};

// Operations the pipe can't perform itself because they live in DolphinQt (GUI thread, render
// widget). MainWindow injects these at startup; each must marshal onto the GUI thread internally.
struct HostHooks
{
  std::function<void(const std::string& path)> boot;
  std::function<void()> stop;
  std::function<void()> reset;
  std::function<std::vector<GameEntry>()> list_games;
  // DTM movies. record_start/stop take an optional save path; play takes the DTM and the game
  // to boot it on (caller supplies the game path; no game-id lookup).
  std::function<void(const std::string& path)> record_start;
  std::function<void(const std::string& path)> record_stop;
  std::function<void(const std::string& dtm, const std::string& game)> play_movie;
};

void StartControlPipe(Core::System& system, HostHooks hooks = {});
void StopControlPipe();

}  // namespace Scripting
