// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/FreeLookWidget.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <string>

#include "Core/AchievementManager.h"
#include "Core/Config/FreeLookSettings.h"

#include "VideoCommon/FreeLookCamera.h"

#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Config/ConfigControls/ConfigChoice.h"
#include "DolphinQt/Config/ConfigControls/ConfigText.h"
#include "DolphinQt/Config/FreeLookKeyframeEditor.h"
#include "DolphinQt/Config/Mapping/MappingWindow.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/Settings.h"

FreeLookWidget::FreeLookWidget(QWidget* parent) : QWidget(parent)
{
  CreateLayout();
  LoadSettings();
  ConnectWidgets();
}

void FreeLookWidget::CreateLayout()
{
  auto* layout = new QVBoxLayout();

  m_enable_freelook = new ConfigBool(tr("Enable"), Config::FREE_LOOK_ENABLED);
  m_enable_freelook->SetDescription(
      tr("Allows manipulation of the in-game camera.<br><br><dolphin_emphasis>If unsure, "
         "leave this unchecked.</dolphin_emphasis>"));
#ifdef USE_RETRO_ACHIEVEMENTS
  const bool hardcore = AchievementManager::GetInstance().IsHardcoreModeActive();
  m_enable_freelook->setEnabled(!hardcore);
#endif  // USE_RETRO_ACHIEVEMENTS
  m_freelook_controller_configure_button = new NonDefaultQPushButton(tr("Configure Controller"));

  m_freelook_control_type = new ConfigChoice({tr("Six Axis"), tr("First Person"), tr("Orbital")},
                                             Config::FL1_CONTROL_TYPE);
  m_freelook_control_type->SetTitle(tr("Free Look Control Type"));
  m_freelook_control_type->SetDescription(tr(
      "Changes the in-game camera type during Free Look.<br><br>"
      "Six Axis: Offers full camera control on all axes, akin to moving a spacecraft in zero "
      "gravity. This is the most powerful Free Look option but is the most challenging to use.<br> "
      "<br>"
      "First Person: Controls the free camera similarly to a first person video game. The camera "
      "can rotate and travel, but roll is impossible. Easy to use, but limiting.<br><br>"
      "Orbital: Rotates the free camera around the original camera. Has no lateral movement, only "
      "rotation and you may zoom up to the camera's origin point."));

  auto* description =
      new QLabel(tr("Free Look allows for manipulation of the in-game camera. "
                    "Different camera types are available from the dropdown.<br><br>"
                    "For detailed instructions, "
                    "<a href=\"https://wiki.dolphin-emu.org/index.php?title=Free_Look\">"
                    "refer to this page</a>."));
  description->setTextFormat(Qt::RichText);
  description->setWordWrap(true);
  description->setTextInteractionFlags(Qt::TextBrowserInteraction);
  description->setOpenExternalLinks(true);

  m_freelook_background_input =
      new ConfigBool(tr("Background Input"), Config::FREE_LOOK_BACKGROUND_INPUT);

  m_keyframe_editor_button = new NonDefaultQPushButton(tr("Edit Keyframes"));
  m_focus_target_enabled =
      new ConfigBool(tr("Follow XYZ Memory Target"), Config::FL1_FOCUS_TARGET_ENABLED);
  m_focus_target_x = new ConfigText(Config::FL1_FOCUS_TARGET_ADDRESS_X);
  m_focus_target_y = new ConfigText(Config::FL1_FOCUS_TARGET_ADDRESS_Y);
  m_focus_target_z = new ConfigText(Config::FL1_FOCUS_TARGET_ADDRESS_Z);
  m_focus_target_custom_coords_enabled = new ConfigBool(
      tr("Use Custom XYZ Coordinates"), Config::FL1_FOCUS_TARGET_CUSTOM_COORDS_ENABLED);
  m_focus_target_custom_x = new ConfigText(Config::FL1_FOCUS_TARGET_CUSTOM_X);
  m_focus_target_custom_y = new ConfigText(Config::FL1_FOCUS_TARGET_CUSTOM_Y);
  m_focus_target_custom_z = new ConfigText(Config::FL1_FOCUS_TARGET_CUSTOM_Z);
  m_focus_target_offset_x = new ConfigText(Config::FL1_FOCUS_TARGET_OFFSET_X);
  m_focus_target_offset_y = new ConfigText(Config::FL1_FOCUS_TARGET_OFFSET_Y);
  m_focus_target_offset_z = new ConfigText(Config::FL1_FOCUS_TARGET_OFFSET_Z);
  m_focus_target_fixed_rotation =
      new ConfigBool(tr("Fixed Relative Rotation"), Config::FL1_FOCUS_TARGET_FIXED_ROTATION);
  m_track_target_enabled = new ConfigBool(tr("Track Object"), Config::FL1_TRACK_TARGET_ENABLED);
  m_track_target_x = new ConfigText(Config::FL1_TRACK_TARGET_ADDRESS_X);
  m_track_target_y = new ConfigText(Config::FL1_TRACK_TARGET_ADDRESS_Y);
  m_track_target_z = new ConfigText(Config::FL1_TRACK_TARGET_ADDRESS_Z);
  m_track_target_custom_coords_enabled = new ConfigBool(
      tr("Use Custom XYZ Coordinates"), Config::FL1_TRACK_TARGET_CUSTOM_COORDS_ENABLED);
  m_track_target_custom_x = new ConfigText(Config::FL1_TRACK_TARGET_CUSTOM_X);
  m_track_target_custom_y = new ConfigText(Config::FL1_TRACK_TARGET_CUSTOM_Y);
  m_track_target_custom_z = new ConfigText(Config::FL1_TRACK_TARGET_CUSTOM_Z);
  m_track_target_offset_x = new ConfigText(Config::FL1_TRACK_TARGET_OFFSET_X);
  m_track_target_offset_y = new ConfigText(Config::FL1_TRACK_TARGET_OFFSET_Y);
  m_track_target_offset_z = new ConfigText(Config::FL1_TRACK_TARGET_OFFSET_Z);
  m_focus_target_x->setPlaceholderText(tr("0x803D78FC or [0x80400000] + 0x34E4"));
  m_focus_target_y->setPlaceholderText(tr("0x803D7900 or [0x80400000] + 0x34E8"));
  m_focus_target_z->setPlaceholderText(tr("0x803D7904 or [0x80400000] + 0x34EC"));
  m_focus_target_custom_x->setPlaceholderText(tr("0.0"));
  m_focus_target_custom_y->setPlaceholderText(tr("0.0"));
  m_focus_target_custom_z->setPlaceholderText(tr("0.0"));
  m_focus_target_offset_x->setPlaceholderText(tr("0.0"));
  m_focus_target_offset_y->setPlaceholderText(tr("150.0"));
  m_focus_target_offset_z->setPlaceholderText(tr("0.0"));
  m_focus_target_offset_x->setFixedWidth(72);
  m_focus_target_offset_y->setFixedWidth(72);
  m_focus_target_offset_z->setFixedWidth(72);
  m_track_target_x->setPlaceholderText(tr("0x803D78FC or [0x80400000] + 0x34E4"));
  m_track_target_y->setPlaceholderText(tr("0x803D7900 or [0x80400000] + 0x34E8"));
  m_track_target_z->setPlaceholderText(tr("0x803D7904 or [0x80400000] + 0x34EC"));
  m_track_target_custom_x->setPlaceholderText(tr("0.0"));
  m_track_target_custom_y->setPlaceholderText(tr("0.0"));
  m_track_target_custom_z->setPlaceholderText(tr("0.0"));
  m_track_target_offset_x->setPlaceholderText(tr("0.0"));
  m_track_target_offset_y->setPlaceholderText(tr("150.0"));
  m_track_target_offset_z->setPlaceholderText(tr("0.0"));
  m_track_target_offset_x->setFixedWidth(72);
  m_track_target_offset_y->setFixedWidth(72);
  m_track_target_offset_z->setFixedWidth(72);

  const auto update_focus_text_live = [](ConfigText* field) {
    QObject::connect(field, &QLineEdit::textChanged, field, [field] { field->Update(); });
  };
  update_focus_text_live(m_focus_target_x);
  update_focus_text_live(m_focus_target_y);
  update_focus_text_live(m_focus_target_z);
  update_focus_text_live(m_focus_target_custom_x);
  update_focus_text_live(m_focus_target_custom_y);
  update_focus_text_live(m_focus_target_custom_z);
  update_focus_text_live(m_focus_target_offset_x);
  update_focus_text_live(m_focus_target_offset_y);
  update_focus_text_live(m_focus_target_offset_z);
  update_focus_text_live(m_track_target_x);
  update_focus_text_live(m_track_target_y);
  update_focus_text_live(m_track_target_z);
  update_focus_text_live(m_track_target_custom_x);
  update_focus_text_live(m_track_target_custom_y);
  update_focus_text_live(m_track_target_custom_z);
  update_focus_text_live(m_track_target_offset_x);
  update_focus_text_live(m_track_target_offset_y);
  update_focus_text_live(m_track_target_offset_z);

  auto* path_layout = new QHBoxLayout();
  path_layout->setContentsMargins(0, 0, 0, 0);
  path_layout->addWidget(m_keyframe_editor_button);
  path_layout->addStretch();

  auto* keyframe_group = new QGroupBox(tr("Keyframe Path Settings"));
  keyframe_group->setLayout(path_layout);

  auto* focus_options = new QFormLayout();
  const auto create_value_offset_row = [](ConfigText* value, ConfigText* offset) {
    auto* row = new QWidget;
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(0, 0, 0, 0);
    row_layout->setSpacing(6);
    row_layout->addWidget(value, 1);
    row_layout->addWidget(offset, 0);
    return row;
  };

  focus_options->addRow(QString(), m_focus_target_enabled);
  focus_options->addRow(tr("X Address"), create_value_offset_row(m_focus_target_x,
                                                                  m_focus_target_offset_x));
  focus_options->addRow(tr("Y Address"), create_value_offset_row(m_focus_target_y,
                                                                  m_focus_target_offset_y));
  focus_options->addRow(tr("Z Address"), create_value_offset_row(m_focus_target_z,
                                                                  m_focus_target_offset_z));
  focus_options->addRow(QString(), m_focus_target_custom_coords_enabled);
  focus_options->addRow(tr("Custom X"), m_focus_target_custom_x);
  focus_options->addRow(tr("Custom Y"), m_focus_target_custom_y);
  focus_options->addRow(tr("Custom Z"), m_focus_target_custom_z);
  focus_options->addRow(QString(), m_focus_target_fixed_rotation);

  auto* focus_group = new QGroupBox(tr("Focus Target"));
  focus_group->setLayout(focus_options);

  auto* track_options = new QFormLayout();
  track_options->addRow(QString(), m_track_target_enabled);
  track_options->addRow(tr("X Address"), create_value_offset_row(m_track_target_x,
                                                                  m_track_target_offset_x));
  track_options->addRow(tr("Y Address"), create_value_offset_row(m_track_target_y,
                                                                  m_track_target_offset_y));
  track_options->addRow(tr("Z Address"), create_value_offset_row(m_track_target_z,
                                                                  m_track_target_offset_z));
  track_options->addRow(QString(), m_track_target_custom_coords_enabled);
  track_options->addRow(tr("Custom X"), m_track_target_custom_x);
  track_options->addRow(tr("Custom Y"), m_track_target_custom_y);
  track_options->addRow(tr("Custom Z"), m_track_target_custom_z);

  auto* track_group = new QGroupBox(tr("Track Object"));
  track_group->setLayout(track_options);

  auto* hlayout = new QHBoxLayout();
  hlayout->addWidget(new QLabel(tr("Camera 1")));
  hlayout->addWidget(m_freelook_control_type);
  hlayout->addWidget(m_freelook_controller_configure_button);

  layout->addWidget(m_enable_freelook);
  layout->addLayout(hlayout);
  layout->addWidget(m_freelook_background_input);
  layout->addWidget(keyframe_group);
  layout->addWidget(focus_group);
  layout->addWidget(track_group);
  layout->addWidget(description);

  setLayout(layout);
}

void FreeLookWidget::ConnectWidgets()
{
  connect(m_freelook_controller_configure_button, &QPushButton::clicked, this,
          &FreeLookWidget::OnFreeLookControllerConfigured);
  connect(m_keyframe_editor_button, &QPushButton::clicked, this,
          &FreeLookWidget::OnKeyframeEditorOpened);
  connect(m_focus_target_enabled, &QCheckBox::toggled, this,
          &FreeLookWidget::UpdateFocusTargetControls);
  connect(m_focus_target_custom_coords_enabled, &QCheckBox::toggled, this,
          &FreeLookWidget::UpdateFocusTargetControls);
  connect(m_track_target_enabled, &QCheckBox::toggled, this,
          &FreeLookWidget::UpdateFocusTargetControls);
  connect(m_track_target_custom_coords_enabled, &QCheckBox::toggled, this,
          &FreeLookWidget::UpdateFocusTargetControls);
  connect(&Settings::Instance(), &Settings::ConfigChanged, this, [this] {
    const QSignalBlocker blocker(this);
    LoadSettings();
  });
}

void FreeLookWidget::OnFreeLookControllerConfigured()
{
  if (m_freelook_controller_configure_button != QObject::sender())
    return;
  const int index = 0;
  MappingWindow* window = new MappingWindow(this, MappingWindow::Type::MAPPING_FREELOOK, index);
  window->setAttribute(Qt::WA_DeleteOnClose, true);
  window->setWindowModality(Qt::WindowModality::WindowModal);
  window->show();
}

void FreeLookWidget::OnKeyframeEditorOpened()
{
  if (m_keyframe_editor_button != QObject::sender())
    return;

  if (!m_keyframe_editor)
  {
    m_keyframe_editor = new FreeLookKeyframeEditor(this);
    m_keyframe_editor->setAttribute(Qt::WA_DeleteOnClose, true);
    m_keyframe_editor->setWindowModality(Qt::WindowModality::WindowModal);
  }
  else
  {
    m_keyframe_editor->ReloadFromDraft();
  }

  m_keyframe_editor->show();
  m_keyframe_editor->raise();
  m_keyframe_editor->activateWindow();
}

void FreeLookWidget::LoadSettings()
{
  const bool checked = Config::Get(Config::FREE_LOOK_ENABLED);
#ifdef USE_RETRO_ACHIEVEMENTS
  const bool hardcore = AchievementManager::GetInstance().IsHardcoreModeActive();
  m_enable_freelook->setEnabled(!hardcore);
#endif  // USE_RETRO_ACHIEVEMENTS
  m_freelook_control_type->setEnabled(checked);
  m_freelook_controller_configure_button->setEnabled(checked);
  m_freelook_background_input->setEnabled(checked);
  m_keyframe_editor_button->setEnabled(checked);
  RefreshFocusTargetSettings();
}

void FreeLookWidget::RefreshFocusTargetSettings()
{
  const QSignalBlocker focus_enabled_blocker(m_focus_target_enabled);
  const QSignalBlocker focus_x_blocker(m_focus_target_x);
  const QSignalBlocker focus_y_blocker(m_focus_target_y);
  const QSignalBlocker focus_z_blocker(m_focus_target_z);
  const QSignalBlocker focus_custom_enabled_blocker(m_focus_target_custom_coords_enabled);
  const QSignalBlocker focus_custom_x_blocker(m_focus_target_custom_x);
  const QSignalBlocker focus_custom_y_blocker(m_focus_target_custom_y);
  const QSignalBlocker focus_custom_z_blocker(m_focus_target_custom_z);
  const QSignalBlocker focus_offset_x_blocker(m_focus_target_offset_x);
  const QSignalBlocker focus_offset_y_blocker(m_focus_target_offset_y);
  const QSignalBlocker focus_offset_z_blocker(m_focus_target_offset_z);
  const QSignalBlocker focus_rotation_blocker(m_focus_target_fixed_rotation);
  const QSignalBlocker track_enabled_blocker(m_track_target_enabled);
  const QSignalBlocker track_x_blocker(m_track_target_x);
  const QSignalBlocker track_y_blocker(m_track_target_y);
  const QSignalBlocker track_z_blocker(m_track_target_z);
  const QSignalBlocker track_custom_enabled_blocker(m_track_target_custom_coords_enabled);
  const QSignalBlocker track_custom_x_blocker(m_track_target_custom_x);
  const QSignalBlocker track_custom_y_blocker(m_track_target_custom_y);
  const QSignalBlocker track_custom_z_blocker(m_track_target_custom_z);
  const QSignalBlocker track_offset_x_blocker(m_track_target_offset_x);
  const QSignalBlocker track_offset_y_blocker(m_track_target_offset_y);
  const QSignalBlocker track_offset_z_blocker(m_track_target_offset_z);

  m_focus_target_enabled->setChecked(Config::Get(Config::FL1_FOCUS_TARGET_ENABLED));
  m_focus_target_x->setText(QString::fromStdString(Config::Get(Config::FL1_FOCUS_TARGET_ADDRESS_X)));
  m_focus_target_y->setText(QString::fromStdString(Config::Get(Config::FL1_FOCUS_TARGET_ADDRESS_Y)));
  m_focus_target_z->setText(QString::fromStdString(Config::Get(Config::FL1_FOCUS_TARGET_ADDRESS_Z)));
  m_focus_target_custom_coords_enabled->setChecked(
      Config::Get(Config::FL1_FOCUS_TARGET_CUSTOM_COORDS_ENABLED));
  m_focus_target_custom_x->setText(
      QString::fromStdString(Config::Get(Config::FL1_FOCUS_TARGET_CUSTOM_X)));
  m_focus_target_custom_y->setText(
      QString::fromStdString(Config::Get(Config::FL1_FOCUS_TARGET_CUSTOM_Y)));
  m_focus_target_custom_z->setText(
      QString::fromStdString(Config::Get(Config::FL1_FOCUS_TARGET_CUSTOM_Z)));
  m_focus_target_offset_x->setText(
      QString::fromStdString(Config::Get(Config::FL1_FOCUS_TARGET_OFFSET_X)));
  m_focus_target_offset_y->setText(
      QString::fromStdString(Config::Get(Config::FL1_FOCUS_TARGET_OFFSET_Y)));
  m_focus_target_offset_z->setText(
      QString::fromStdString(Config::Get(Config::FL1_FOCUS_TARGET_OFFSET_Z)));
  m_focus_target_fixed_rotation->setChecked(
      Config::Get(Config::FL1_FOCUS_TARGET_FIXED_ROTATION));
  m_track_target_enabled->setChecked(Config::Get(Config::FL1_TRACK_TARGET_ENABLED));
  m_track_target_x->setText(QString::fromStdString(Config::Get(Config::FL1_TRACK_TARGET_ADDRESS_X)));
  m_track_target_y->setText(QString::fromStdString(Config::Get(Config::FL1_TRACK_TARGET_ADDRESS_Y)));
  m_track_target_z->setText(QString::fromStdString(Config::Get(Config::FL1_TRACK_TARGET_ADDRESS_Z)));
  m_track_target_custom_coords_enabled->setChecked(
      Config::Get(Config::FL1_TRACK_TARGET_CUSTOM_COORDS_ENABLED));
  m_track_target_custom_x->setText(
      QString::fromStdString(Config::Get(Config::FL1_TRACK_TARGET_CUSTOM_X)));
  m_track_target_custom_y->setText(
      QString::fromStdString(Config::Get(Config::FL1_TRACK_TARGET_CUSTOM_Y)));
  m_track_target_custom_z->setText(
      QString::fromStdString(Config::Get(Config::FL1_TRACK_TARGET_CUSTOM_Z)));
  m_track_target_offset_x->setText(
      QString::fromStdString(Config::Get(Config::FL1_TRACK_TARGET_OFFSET_X)));
  m_track_target_offset_y->setText(
      QString::fromStdString(Config::Get(Config::FL1_TRACK_TARGET_OFFSET_Y)));
  m_track_target_offset_z->setText(
      QString::fromStdString(Config::Get(Config::FL1_TRACK_TARGET_OFFSET_Z)));

  UpdateFocusTargetControls();
}

void FreeLookWidget::UpdateFocusTargetControls()
{
  const bool free_look_enabled = Config::Get(Config::FREE_LOOK_ENABLED);
  const bool focus_supported =
      Config::Get(Config::FL1_CONTROL_TYPE) != FreeLook::ControlType::Orbital;
  const bool focus_enabled = m_focus_target_enabled->isChecked();
  const bool custom_coords_enabled = m_focus_target_custom_coords_enabled->isChecked();
  const bool track_enabled = m_track_target_enabled->isChecked();
  const bool track_custom_coords_enabled = m_track_target_custom_coords_enabled->isChecked();
  const bool controls_enabled = free_look_enabled && focus_supported;

  m_focus_target_enabled->setEnabled(controls_enabled);
  m_focus_target_x->setEnabled(controls_enabled && focus_enabled && !custom_coords_enabled);
  m_focus_target_y->setEnabled(controls_enabled && focus_enabled && !custom_coords_enabled);
  m_focus_target_z->setEnabled(controls_enabled && focus_enabled && !custom_coords_enabled);
  m_focus_target_custom_coords_enabled->setEnabled(controls_enabled && focus_enabled);
  m_focus_target_custom_x->setEnabled(controls_enabled && focus_enabled && custom_coords_enabled);
  m_focus_target_custom_y->setEnabled(controls_enabled && focus_enabled && custom_coords_enabled);
  m_focus_target_custom_z->setEnabled(controls_enabled && focus_enabled && custom_coords_enabled);
  m_focus_target_offset_x->setEnabled(controls_enabled && focus_enabled);
  m_focus_target_offset_y->setEnabled(controls_enabled && focus_enabled);
  m_focus_target_offset_z->setEnabled(controls_enabled && focus_enabled);
  m_focus_target_fixed_rotation->setEnabled(controls_enabled && focus_enabled);
  m_track_target_enabled->setEnabled(controls_enabled && focus_enabled);
  m_track_target_x->setEnabled(controls_enabled && focus_enabled && track_enabled &&
                               !track_custom_coords_enabled);
  m_track_target_y->setEnabled(controls_enabled && focus_enabled && track_enabled &&
                               !track_custom_coords_enabled);
  m_track_target_z->setEnabled(controls_enabled && focus_enabled && track_enabled &&
                               !track_custom_coords_enabled);
  m_track_target_custom_coords_enabled->setEnabled(controls_enabled && focus_enabled &&
                                                   track_enabled);
  m_track_target_custom_x->setEnabled(controls_enabled && focus_enabled && track_enabled &&
                                      track_custom_coords_enabled);
  m_track_target_custom_y->setEnabled(controls_enabled && focus_enabled && track_enabled &&
                                      track_custom_coords_enabled);
  m_track_target_custom_z->setEnabled(controls_enabled && focus_enabled && track_enabled &&
                                      track_custom_coords_enabled);
  m_track_target_offset_x->setEnabled(controls_enabled && focus_enabled && track_enabled);
  m_track_target_offset_y->setEnabled(controls_enabled && focus_enabled && track_enabled);
  m_track_target_offset_z->setEnabled(controls_enabled && focus_enabled && track_enabled);
}
