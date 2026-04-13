// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <utility>

#include "DolphinQt/Config/Mapping/MappingWidget.h"

class QHBoxLayout;
class QGroupBox;
class QSpinBox;

class HotkeyTAS final : public MappingWidget
{
  Q_OBJECT
public:
  explicit HotkeyTAS(MappingWindow* window);

  InputConfig* GetConfig() override;

private:
  void LoadSettings() override;
  void SaveSettings() override;
  void CreateMainLayout();
  QGroupBox* CreateEssTable(const QString& title, const char* key_prefix,
                            const std::array<std::pair<int, int>, 9>& defaults);
  void SaveEssPreset(const char* key_prefix, int index, int x, int y);

  // Main
  QHBoxLayout* m_main_layout;
};
