// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "DolphinQt/TAS/TASInputWindow.h"

class QGroupBox;
class QHideEvent;
class QShowEvent;
class QSpinBox;
class TASCheckBox;

class GCTASInputWindow : public TASInputWindow
{
  Q_OBJECT
public:
  explicit GCTASInputWindow(QWidget* parent, int controller_id);
  ~GCTASInputWindow();
  void hideEvent(QHideEvent* event) override;
  void showEvent(QShowEvent* event) override;
  static GCTASInputWindow* GetInstanceForController(int controller_id);
  void ApplyEssPreset(int preset_index);

private:
  void CreateTriggersBox();
  void CreateButtonsBox();

  // main stick hotkeys
  void SetupMainStickHotkeys();
  void ApplyMainStickPreset(int preset_index);

  int m_controller_id;

  InputOverrider m_overrider;

  TASCheckBox* m_a_button;
  TASCheckBox* m_b_button;
  TASCheckBox* m_x_button;
  TASCheckBox* m_y_button;
  TASCheckBox* m_z_button;
  TASCheckBox* m_l_button;
  TASCheckBox* m_r_button;
  TASCheckBox* m_start_button;
  TASCheckBox* m_left_button;
  TASCheckBox* m_up_button;
  TASCheckBox* m_down_button;
  TASCheckBox* m_right_button;
  TASSpinBox* m_l_trigger_value;
  TASSpinBox* m_r_trigger_value;
  TASStickBox* m_main_stick_box;
  TASStickBox* m_c_stick_box;
  QGroupBox* m_triggers_box;
  QGroupBox* m_buttons_box;
};
