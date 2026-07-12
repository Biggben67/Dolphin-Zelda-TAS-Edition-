// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QPointer>
#include <QWidget>

class ConfigBool;
class ConfigChoice;
class ConfigText;
class FreeLookKeyframeEditor;
class QCheckBox;
class QPushButton;
class QString;
class ToolTipCheckBox;

class FreeLookWidget final : public QWidget
{
  Q_OBJECT
public:
  explicit FreeLookWidget(QWidget* parent);

private:
  void CreateLayout();
  void ConnectWidgets();

  void OnFreeLookControllerConfigured();
  void OnKeyframeEditorOpened();
  void LoadSettings();
  void RefreshFocusTargetSettings();
  void UpdateFocusTargetControls();

  ConfigBool* m_enable_freelook;
  ConfigChoice* m_freelook_control_type;
  QPushButton* m_freelook_controller_configure_button;
  QCheckBox* m_freelook_background_input;

  QPushButton* m_keyframe_editor_button;
  QPointer<FreeLookKeyframeEditor> m_keyframe_editor;

  ConfigBool* m_focus_target_enabled;
  ConfigText* m_focus_target_x;
  ConfigText* m_focus_target_y;
  ConfigText* m_focus_target_z;
  ConfigBool* m_focus_target_custom_coords_enabled;
  ConfigText* m_focus_target_custom_x;
  ConfigText* m_focus_target_custom_y;
  ConfigText* m_focus_target_custom_z;
  ConfigText* m_focus_target_offset_x;
  ConfigText* m_focus_target_offset_y;
  ConfigText* m_focus_target_offset_z;
  ConfigBool* m_focus_target_fixed_rotation;
  ConfigBool* m_track_target_enabled;
  ConfigText* m_track_target_x;
  ConfigText* m_track_target_y;
  ConfigText* m_track_target_z;
  ConfigBool* m_track_target_custom_coords_enabled;
  ConfigText* m_track_target_custom_x;
  ConfigText* m_track_target_custom_y;
  ConfigText* m_track_target_custom_z;
  ConfigText* m_track_target_offset_x;
  ConfigText* m_track_target_offset_y;
  ConfigText* m_track_target_offset_z;
};
