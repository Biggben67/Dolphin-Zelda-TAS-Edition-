// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "DolphinQt/Config/Mapping/MappingWidget.h"

class QPushButton;

class FreeLookKeyframes final : public MappingWidget
{
  Q_OBJECT
public:
  explicit FreeLookKeyframes(MappingWindow* window);

  InputConfig* GetConfig() override;
  void LoadSettings() override;
  void SaveSettings() override;

private:
  void CreateMainLayout();
  void OpenEditor();

  QPushButton* m_open_editor_button;
};
