// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/FreeLookKeyframeEditor.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <numbers>
#include <utility>

#include <QAbstractItemView>
#include <QAction>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QKeySequence>
#include <QShortcut>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/Matrix.h"
#include "Core/Config/FreeLookSettings.h"
#include "VideoCommon/FreeLookCamera.h"

namespace
{
QString ToText(float value)
{
  return QString::number(value, 'f', 4);
}

CameraControllerInput* GetActiveKeyframeController()
{
  CameraController* controller = g_freelook_camera.GetController();
  if (!controller || !controller->SupportsInput())
    return nullptr;

  auto* input = static_cast<CameraControllerInput*>(controller);
  return input->SupportsKeyframeAnimation() ? input : nullptr;
}

void SaveFocusSettings(Common::IniFile::Section* meta)
{
  if (!meta)
    return;

  meta->Set("FocusTargetEnabled", Config::Get(Config::FL1_FOCUS_TARGET_ENABLED));
  meta->Set("FocusTargetAddressX", Config::Get(Config::FL1_FOCUS_TARGET_ADDRESS_X));
  meta->Set("FocusTargetAddressY", Config::Get(Config::FL1_FOCUS_TARGET_ADDRESS_Y));
  meta->Set("FocusTargetAddressZ", Config::Get(Config::FL1_FOCUS_TARGET_ADDRESS_Z));
  meta->Set("FocusTargetCustomCoordsEnabled",
            Config::Get(Config::FL1_FOCUS_TARGET_CUSTOM_COORDS_ENABLED));
  meta->Set("FocusTargetCustomX", Config::Get(Config::FL1_FOCUS_TARGET_CUSTOM_X));
  meta->Set("FocusTargetCustomY", Config::Get(Config::FL1_FOCUS_TARGET_CUSTOM_Y));
  meta->Set("FocusTargetCustomZ", Config::Get(Config::FL1_FOCUS_TARGET_CUSTOM_Z));
  meta->Set("FocusTargetOffsetX", Config::Get(Config::FL1_FOCUS_TARGET_OFFSET_X));
  meta->Set("FocusTargetOffsetY", Config::Get(Config::FL1_FOCUS_TARGET_OFFSET_Y));
  meta->Set("FocusTargetOffsetZ", Config::Get(Config::FL1_FOCUS_TARGET_OFFSET_Z));
  meta->Set("FocusTargetFixedRotation", Config::Get(Config::FL1_FOCUS_TARGET_FIXED_ROTATION));
  meta->Set("TrackTargetEnabled", Config::Get(Config::FL1_TRACK_TARGET_ENABLED));
  meta->Set("TrackTargetAddressX", Config::Get(Config::FL1_TRACK_TARGET_ADDRESS_X));
  meta->Set("TrackTargetAddressY", Config::Get(Config::FL1_TRACK_TARGET_ADDRESS_Y));
  meta->Set("TrackTargetAddressZ", Config::Get(Config::FL1_TRACK_TARGET_ADDRESS_Z));
  meta->Set("TrackTargetCustomCoordsEnabled",
            Config::Get(Config::FL1_TRACK_TARGET_CUSTOM_COORDS_ENABLED));
  meta->Set("TrackTargetCustomX", Config::Get(Config::FL1_TRACK_TARGET_CUSTOM_X));
  meta->Set("TrackTargetCustomY", Config::Get(Config::FL1_TRACK_TARGET_CUSTOM_Y));
  meta->Set("TrackTargetCustomZ", Config::Get(Config::FL1_TRACK_TARGET_CUSTOM_Z));
  meta->Set("TrackTargetOffsetX", Config::Get(Config::FL1_TRACK_TARGET_OFFSET_X));
  meta->Set("TrackTargetOffsetY", Config::Get(Config::FL1_TRACK_TARGET_OFFSET_Y));
  meta->Set("TrackTargetOffsetZ", Config::Get(Config::FL1_TRACK_TARGET_OFFSET_Z));
}

void LoadFocusSettings(const Common::IniFile::Section& meta)
{
  bool bool_value = false;
  std::string string_value;

  if (meta.Get("FocusTargetEnabled", &bool_value, Config::Get(Config::FL1_FOCUS_TARGET_ENABLED)))
    Config::SetBaseOrCurrent(Config::FL1_FOCUS_TARGET_ENABLED, bool_value);
  if (meta.Get("FocusTargetAddressX", &string_value,
               Config::Get(Config::FL1_FOCUS_TARGET_ADDRESS_X)))
    Config::SetBaseOrCurrent(Config::FL1_FOCUS_TARGET_ADDRESS_X, string_value);
  if (meta.Get("FocusTargetAddressY", &string_value,
               Config::Get(Config::FL1_FOCUS_TARGET_ADDRESS_Y)))
    Config::SetBaseOrCurrent(Config::FL1_FOCUS_TARGET_ADDRESS_Y, string_value);
  if (meta.Get("FocusTargetAddressZ", &string_value,
               Config::Get(Config::FL1_FOCUS_TARGET_ADDRESS_Z)))
    Config::SetBaseOrCurrent(Config::FL1_FOCUS_TARGET_ADDRESS_Z, string_value);
  if (meta.Get("FocusTargetCustomCoordsEnabled", &bool_value,
               Config::Get(Config::FL1_FOCUS_TARGET_CUSTOM_COORDS_ENABLED)))
    Config::SetBaseOrCurrent(Config::FL1_FOCUS_TARGET_CUSTOM_COORDS_ENABLED, bool_value);
  if (meta.Get("FocusTargetCustomX", &string_value, Config::Get(Config::FL1_FOCUS_TARGET_CUSTOM_X)))
    Config::SetBaseOrCurrent(Config::FL1_FOCUS_TARGET_CUSTOM_X, string_value);
  if (meta.Get("FocusTargetCustomY", &string_value, Config::Get(Config::FL1_FOCUS_TARGET_CUSTOM_Y)))
    Config::SetBaseOrCurrent(Config::FL1_FOCUS_TARGET_CUSTOM_Y, string_value);
  if (meta.Get("FocusTargetCustomZ", &string_value, Config::Get(Config::FL1_FOCUS_TARGET_CUSTOM_Z)))
    Config::SetBaseOrCurrent(Config::FL1_FOCUS_TARGET_CUSTOM_Z, string_value);
  if (meta.Get("FocusTargetOffsetX", &string_value, Config::Get(Config::FL1_FOCUS_TARGET_OFFSET_X)))
    Config::SetBaseOrCurrent(Config::FL1_FOCUS_TARGET_OFFSET_X, string_value);
  if (meta.Get("FocusTargetOffsetY", &string_value, Config::Get(Config::FL1_FOCUS_TARGET_OFFSET_Y)))
    Config::SetBaseOrCurrent(Config::FL1_FOCUS_TARGET_OFFSET_Y, string_value);
  if (meta.Get("FocusTargetOffsetZ", &string_value, Config::Get(Config::FL1_FOCUS_TARGET_OFFSET_Z)))
    Config::SetBaseOrCurrent(Config::FL1_FOCUS_TARGET_OFFSET_Z, string_value);
  if (meta.Get("FocusTargetFixedRotation", &bool_value,
               Config::Get(Config::FL1_FOCUS_TARGET_FIXED_ROTATION)))
    Config::SetBaseOrCurrent(Config::FL1_FOCUS_TARGET_FIXED_ROTATION, bool_value);
  if (meta.Get("TrackTargetEnabled", &bool_value, Config::Get(Config::FL1_TRACK_TARGET_ENABLED)))
    Config::SetBaseOrCurrent(Config::FL1_TRACK_TARGET_ENABLED, bool_value);
  if (meta.Get("TrackTargetAddressX", &string_value,
               Config::Get(Config::FL1_TRACK_TARGET_ADDRESS_X)))
    Config::SetBaseOrCurrent(Config::FL1_TRACK_TARGET_ADDRESS_X, string_value);
  if (meta.Get("TrackTargetAddressY", &string_value,
               Config::Get(Config::FL1_TRACK_TARGET_ADDRESS_Y)))
    Config::SetBaseOrCurrent(Config::FL1_TRACK_TARGET_ADDRESS_Y, string_value);
  if (meta.Get("TrackTargetAddressZ", &string_value,
               Config::Get(Config::FL1_TRACK_TARGET_ADDRESS_Z)))
    Config::SetBaseOrCurrent(Config::FL1_TRACK_TARGET_ADDRESS_Z, string_value);
  if (meta.Get("TrackTargetCustomCoordsEnabled", &bool_value,
               Config::Get(Config::FL1_TRACK_TARGET_CUSTOM_COORDS_ENABLED)))
    Config::SetBaseOrCurrent(Config::FL1_TRACK_TARGET_CUSTOM_COORDS_ENABLED, bool_value);
  if (meta.Get("TrackTargetCustomX", &string_value, Config::Get(Config::FL1_TRACK_TARGET_CUSTOM_X)))
    Config::SetBaseOrCurrent(Config::FL1_TRACK_TARGET_CUSTOM_X, string_value);
  if (meta.Get("TrackTargetCustomY", &string_value, Config::Get(Config::FL1_TRACK_TARGET_CUSTOM_Y)))
    Config::SetBaseOrCurrent(Config::FL1_TRACK_TARGET_CUSTOM_Y, string_value);
  if (meta.Get("TrackTargetCustomZ", &string_value, Config::Get(Config::FL1_TRACK_TARGET_CUSTOM_Z)))
    Config::SetBaseOrCurrent(Config::FL1_TRACK_TARGET_CUSTOM_Z, string_value);
  if (meta.Get("TrackTargetOffsetX", &string_value, Config::Get(Config::FL1_TRACK_TARGET_OFFSET_X)))
    Config::SetBaseOrCurrent(Config::FL1_TRACK_TARGET_OFFSET_X, string_value);
  if (meta.Get("TrackTargetOffsetY", &string_value, Config::Get(Config::FL1_TRACK_TARGET_OFFSET_Y)))
    Config::SetBaseOrCurrent(Config::FL1_TRACK_TARGET_OFFSET_Y, string_value);
  if (meta.Get("TrackTargetOffsetZ", &string_value, Config::Get(Config::FL1_TRACK_TARGET_OFFSET_Z)))
    Config::SetBaseOrCurrent(Config::FL1_TRACK_TARGET_OFFSET_Z, string_value);
}
}  // namespace

FreeLookKeyframeEditor::FreeLookKeyframeEditor(QWidget* parent) : QDialog(parent)
{
  CreateLayout();
  ConnectWidgets();

  setWindowTitle(tr("Free Look Keyframe Editor"));
  resize(980, 620);

  LoadFromDraftFile();
  RefreshCurrentCamera();
  RefreshKeyframeTable();

  m_draft_refresh_timer = new QTimer(this);
  m_draft_refresh_timer->setInterval(500);
  connect(m_draft_refresh_timer, &QTimer::timeout, this,
          &FreeLookKeyframeEditor::MaybeReloadDraftFromDisk);
  m_draft_refresh_timer->start();
}

void FreeLookKeyframeEditor::ReloadFromDraft()
{
  LoadFromDraftFile();
  RefreshCurrentCamera();
  RefreshKeyframeTable();
  ApplyEditorStateToActiveCamera();
}

void FreeLookKeyframeEditor::CreateLayout()
{
  auto* main_layout = new QVBoxLayout();

  auto* interpolation_group = new QGroupBox(tr("Interpolation"));
  auto* interpolation_layout = new QFormLayout();
  m_interpolation_mode_combo = new QComboBox();
  m_interpolation_mode_combo->addItem(tr("Linear"));
  m_interpolation_mode_combo->addItem(tr("Catmull-Rom"));
  m_interpolation_mode_combo->addItem(tr("Smooth Step"));
  interpolation_layout->addRow(tr("Mode"), m_interpolation_mode_combo);
  interpolation_group->setLayout(interpolation_layout);

  auto* current_group = new QGroupBox(tr("Current Camera"));
  auto* current_layout = new QFormLayout();
  m_current_position = new QLabel();
  m_current_rotation = new QLabel();
  current_layout->addRow(tr("Position"), m_current_position);
  current_layout->addRow(tr("Angle (deg)"), m_current_rotation);
  current_group->setLayout(current_layout);

  auto* top_row = new QHBoxLayout();
  top_row->addWidget(interpolation_group, 1);
  top_row->addWidget(current_group, 2);

  m_table = new QTableWidget();
  m_table->setColumnCount(9);
  m_table->setHorizontalHeaderLabels(
      {tr("Frame"), tr("Pos X"), tr("Pos Y"), tr("Pos Z"), tr("Rot X"), tr("Rot Y"),
       tr("Rot Z"), tr("FOV X"), tr("FOV Y")});
  m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  m_table->horizontalHeader()->setSectionResizeMode(8, QHeaderView::Stretch);
  m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_table->setContextMenuPolicy(Qt::CustomContextMenu);

  auto* edit_group = new QGroupBox(tr("Selected Keyframe"));
  auto* edit_layout = new QGridLayout();

  m_frame = new QSpinBox();
  m_frame->setRange(0, std::numeric_limits<int>::max());

  auto make_spin = [] {
    auto* spin = new QDoubleSpinBox();
    spin->setDecimals(4);
    spin->setSingleStep(0.1);
    spin->setRange(-1000000.0, 1000000.0);
    return spin;
  };

  m_pos_x = make_spin();
  m_pos_y = make_spin();
  m_pos_z = make_spin();

  m_rot_x = make_spin();
  m_rot_y = make_spin();
  m_rot_z = make_spin();
  m_rot_x->setRange(-36000.0, 36000.0);
  m_rot_y->setRange(-36000.0, 36000.0);
  m_rot_z->setRange(-36000.0, 36000.0);

  m_fov_x = new QDoubleSpinBox();
  m_fov_y = new QDoubleSpinBox();
  for (QDoubleSpinBox* spin : {m_fov_x, m_fov_y})
  {
    spin->setDecimals(4);
    spin->setSingleStep(0.01);
    spin->setRange(0.025, 32.0);
  }

  edit_layout->addWidget(new QLabel(tr("Frame")), 0, 0);
  edit_layout->addWidget(m_frame, 0, 1);
  edit_layout->addWidget(new QLabel(tr("Pos X")), 1, 0);
  edit_layout->addWidget(m_pos_x, 1, 1);
  edit_layout->addWidget(new QLabel(tr("Pos Y")), 1, 2);
  edit_layout->addWidget(m_pos_y, 1, 3);
  edit_layout->addWidget(new QLabel(tr("Pos Z")), 1, 4);
  edit_layout->addWidget(m_pos_z, 1, 5);

  edit_layout->addWidget(new QLabel(tr("Rot X (deg)")), 2, 0);
  edit_layout->addWidget(m_rot_x, 2, 1);
  edit_layout->addWidget(new QLabel(tr("Rot Y (deg)")), 2, 2);
  edit_layout->addWidget(m_rot_y, 2, 3);
  edit_layout->addWidget(new QLabel(tr("Rot Z (deg)")), 2, 4);
  edit_layout->addWidget(m_rot_z, 2, 5);

  edit_layout->addWidget(new QLabel(tr("FOV X")), 3, 0);
  edit_layout->addWidget(m_fov_x, 3, 1);
  edit_layout->addWidget(new QLabel(tr("FOV Y")), 3, 2);
  edit_layout->addWidget(m_fov_y, 3, 3);

  m_refresh_button = new QPushButton(tr("Reload Draft"));
  m_load_file_button = new QPushButton(tr("Load File..."));
  m_save_as_button = new QPushButton(tr("Save As..."));
  m_add_current_button = new QPushButton(tr("Add Current Camera"));
  m_apply_button = new QPushButton(tr("Apply Changes"));
  m_delete_button = new QPushButton(tr("Delete Keyframe"));

  auto* button_row = new QHBoxLayout();
  button_row->addWidget(m_refresh_button);
  button_row->addWidget(m_load_file_button);
  button_row->addWidget(m_save_as_button);
  button_row->addWidget(m_add_current_button);
  button_row->addStretch();
  button_row->addWidget(m_apply_button);
  button_row->addWidget(m_delete_button);

  auto* edit_outer = new QVBoxLayout();
  edit_outer->addLayout(edit_layout);
  edit_outer->addLayout(button_row);
  edit_group->setLayout(edit_outer);

  main_layout->addLayout(top_row);
  main_layout->addWidget(m_table, 1);
  main_layout->addWidget(edit_group);

  setLayout(main_layout);
}

void FreeLookKeyframeEditor::ConnectWidgets()
{
  connect(m_table, &QTableWidget::itemSelectionChanged, this,
          &FreeLookKeyframeEditor::RefreshEditorSelection);
  connect(m_refresh_button, &QPushButton::clicked, this, &FreeLookKeyframeEditor::ReloadFromDraft);
  connect(m_load_file_button, &QPushButton::clicked, this,
          &FreeLookKeyframeEditor::LoadPathFromDialog);
  connect(m_save_as_button, &QPushButton::clicked, this, &FreeLookKeyframeEditor::SavePathAs);
  connect(m_add_current_button, &QPushButton::clicked, this,
          &FreeLookKeyframeEditor::AddCurrentCamera);
  connect(m_apply_button, &QPushButton::clicked, this, &FreeLookKeyframeEditor::ApplySelectionEdits);
  connect(m_delete_button, &QPushButton::clicked, this, &FreeLookKeyframeEditor::DeleteSelection);
  connect(m_interpolation_mode_combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
          &FreeLookKeyframeEditor::OnInterpolationModeChanged);
  connect(m_table, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
    const int row = m_table->rowAt(pos.y());
    if (row >= 0 && !m_table->selectionModel()->isRowSelected(row, QModelIndex{}))
      m_table->selectRow(row);

    QMenu menu(this);
    QAction* copy_action = menu.addAction(tr("Copy"));
    QAction* paste_action = menu.addAction(tr("Paste"));
    copy_action->setEnabled(!GetSelectedRows().empty());
    paste_action->setEnabled(!m_copied_keyframes.empty());
    connect(copy_action, &QAction::triggered, this, &FreeLookKeyframeEditor::CopySelection);
    connect(paste_action, &QAction::triggered, this, &FreeLookKeyframeEditor::PasteCopiedKeyframes);
    menu.exec(m_table->viewport()->mapToGlobal(pos));
  });

  auto* copy_shortcut = new QShortcut(QKeySequence::Copy, this);
  connect(copy_shortcut, &QShortcut::activated, this, &FreeLookKeyframeEditor::CopySelection);
  auto* paste_shortcut = new QShortcut(QKeySequence::Paste, this);
  connect(paste_shortcut, &QShortcut::activated, this, &FreeLookKeyframeEditor::PasteCopiedKeyframes);

  connect(m_rot_x, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
          [this](double) {
            if (!m_updating_selection)
              m_rot_x_dirty = true;
          });
  connect(m_rot_y, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
          [this](double) {
            if (!m_updating_selection)
              m_rot_y_dirty = true;
          });
  connect(m_rot_z, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
          [this](double) {
            if (!m_updating_selection)
              m_rot_z_dirty = true;
          });
}

void FreeLookKeyframeEditor::RefreshCurrentCamera()
{
  CameraControllerInput* controller = GetActiveKeyframeController();
  if (!controller)
  {
    m_current_position->setText(tr("Unavailable"));
    m_current_rotation->setText(tr("Unavailable"));
    m_add_current_button->setEnabled(false);
    return;
  }

  const auto current = controller->GetCurrentCameraTransform();
  m_current_position->setText(tr("X %1 | Y %2 | Z %3")
                                  .arg(ToText(current.position.x), ToText(current.position.y),
                                       ToText(current.position.z)));
  m_current_rotation->setText(tr("X %1 | Y %2 | Z %3")
                                  .arg(ToText(current.rotation.x * 180.0f /
                                              std::numbers::pi_v<float>),
                                       ToText(current.rotation.y * 180.0f /
                                              std::numbers::pi_v<float>),
                                       ToText(current.rotation.z * 180.0f /
                                              std::numbers::pi_v<float>)));
  m_add_current_button->setEnabled(true);
}

void FreeLookKeyframeEditor::RefreshKeyframeTable()
{
  RefreshKeyframeTableWithSelection(nullptr);
}

void FreeLookKeyframeEditor::RefreshKeyframeTableWithSelection(const KeyframeRow* preferred_selection)
{
  const int old_row = m_table->currentRow();

  m_interpolation_mode_combo->blockSignals(true);
  m_interpolation_mode_combo->setCurrentIndex(std::clamp(m_interpolation_mode, 0, 2));
  m_interpolation_mode_combo->blockSignals(false);

  m_table->clearContents();
  m_table->setRowCount(static_cast<int>(m_keyframes.size()));

  for (int row = 0; row < static_cast<int>(m_keyframes.size()); ++row)
  {
    const auto& keyframe = m_keyframes[row];
    const std::array<QString, 9> cells = {
        QString::number(keyframe.frame), ToText(keyframe.pos_x), ToText(keyframe.pos_y),
        ToText(keyframe.pos_z),          ToText(keyframe.rot_x_deg), ToText(keyframe.rot_y_deg),
        ToText(keyframe.rot_z_deg),      ToText(keyframe.fov_x),     ToText(keyframe.fov_y)};

    for (int col = 0; col < static_cast<int>(cells.size()); ++col)
      m_table->setItem(row, col, new QTableWidgetItem(cells[col]));
  }

  int selected_row = -1;
  if (preferred_selection)
    selected_row = FindMatchingRow(*preferred_selection);

  if (selected_row < 0 && !m_keyframes.empty())
    selected_row = std::clamp(old_row, 0, static_cast<int>(m_keyframes.size()) - 1);

  SelectRow(selected_row);
  RefreshEditorSelection();
}

int FreeLookKeyframeEditor::FindMatchingRow(const KeyframeRow& keyframe) const
{
  if (keyframe.editor_id == 0)
    return -1;

  for (int i = 0; i < static_cast<int>(m_keyframes.size()); ++i)
  {
    if (m_keyframes[i].editor_id == keyframe.editor_id)
      return i;
  }

  return -1;
}

void FreeLookKeyframeEditor::SelectRow(int row)
{
  m_table->blockSignals(true);
  if (row >= 0 && row < m_table->rowCount())
    m_table->selectRow(row);
  else
    m_table->clearSelection();
  m_table->blockSignals(false);
}

void FreeLookKeyframeEditor::RefreshEditorSelection()
{
  const int row = m_table->currentRow();
  if (row < 0 || row >= static_cast<int>(m_keyframes.size()))
  {
    m_apply_button->setEnabled(false);
    m_delete_button->setEnabled(false);
    return;
  }

  m_apply_button->setEnabled(true);
  m_delete_button->setEnabled(true);

  const auto& keyframe = m_keyframes[row];
  m_updating_selection = true;
  m_frame->setValue(
      static_cast<int>(std::min<uint64_t>(keyframe.frame, std::numeric_limits<int>::max())));
  m_pos_x->setValue(keyframe.pos_x);
  m_pos_y->setValue(keyframe.pos_y);
  m_pos_z->setValue(keyframe.pos_z);
  m_rot_x->setValue(keyframe.rot_x_deg);
  m_rot_y->setValue(keyframe.rot_y_deg);
  m_rot_z->setValue(keyframe.rot_z_deg);
  m_fov_x->setValue(keyframe.fov_x);
  m_fov_y->setValue(keyframe.fov_y);
  m_updating_selection = false;

  m_rot_x_dirty = false;
  m_rot_y_dirty = false;
  m_rot_z_dirty = false;
}

void FreeLookKeyframeEditor::ApplySelectionEdits()
{
  const int row = m_table->currentRow();
  if (row < 0 || row >= static_cast<int>(m_keyframes.size()))
    return;

  auto& keyframe = m_keyframes[row];
  keyframe.frame = static_cast<uint64_t>(m_frame->value());
  keyframe.pos_x = static_cast<float>(m_pos_x->value());
  keyframe.pos_y = static_cast<float>(m_pos_y->value());
  keyframe.pos_z = static_cast<float>(m_pos_z->value());
  keyframe.fov_x = static_cast<float>(m_fov_x->value());
  keyframe.fov_y = static_cast<float>(m_fov_y->value());

  if (m_rot_x_dirty || m_rot_y_dirty || m_rot_z_dirty)
  {
    keyframe.rot_x_deg = static_cast<float>(m_rot_x->value());
    keyframe.rot_y_deg = static_cast<float>(m_rot_y->value());
    keyframe.rot_z_deg = static_cast<float>(m_rot_z->value());

    const Common::Vec3 euler{keyframe.rot_x_deg * std::numbers::pi_v<float> / 180.0f,
                             keyframe.rot_y_deg * std::numbers::pi_v<float> / 180.0f,
                             keyframe.rot_z_deg * std::numbers::pi_v<float> / 180.0f};
    const Common::Quaternion quat = Common::Quaternion::RotateXYZ(euler).Normalized();
    keyframe.rot_qx = quat.data.x;
    keyframe.rot_qy = quat.data.y;
    keyframe.rot_qz = quat.data.z;
    keyframe.rot_qw = quat.data.w;
  }

  const KeyframeRow updated_keyframe = keyframe;

  std::sort(m_keyframes.begin(), m_keyframes.end(),
            [](const KeyframeRow& lhs, const KeyframeRow& rhs) { return lhs.frame < rhs.frame; });

  ApplyEditorStateToActiveCamera();
  SaveDraftFile();
  RefreshKeyframeTableWithSelection(&updated_keyframe);
}

void FreeLookKeyframeEditor::AddCurrentCamera()
{
  CameraControllerInput* controller = GetActiveKeyframeController();
  if (!controller)
    return;

  const auto current = controller->GetCurrentCameraTransform();
  KeyframeRow row;
  row.editor_id = m_next_editor_id++;
  row.frame = current.frame;
  if (row.frame == 0 && !m_keyframes.empty())
  {
    row.frame = m_keyframes.back().frame +
                static_cast<uint64_t>(
                    std::max(1, Config::Get(Config::FL1_KEYFRAME_DEFAULT_FRAME_STEP)));
  }

  row.pos_x = current.position.x;
  row.pos_y = current.position.y;
  row.pos_z = current.position.z;
  row.rot_x_deg = current.rotation.x * 180.0f / std::numbers::pi_v<float>;
  row.rot_y_deg = current.rotation.y * 180.0f / std::numbers::pi_v<float>;
  row.rot_z_deg = current.rotation.z * 180.0f / std::numbers::pi_v<float>;
  const Common::Quaternion quat = Common::Quaternion::RotateXYZ(current.rotation).Normalized();
  row.rot_qx = quat.data.x;
  row.rot_qy = quat.data.y;
  row.rot_qz = quat.data.z;
  row.rot_qw = quat.data.w;
  row.fov_x = current.fov.x;
  row.fov_y = current.fov.y;

  m_focus_relative_keyframes = Config::Get(Config::FL1_FOCUS_TARGET_ENABLED);
  m_keyframes.push_back(row);
  std::sort(m_keyframes.begin(), m_keyframes.end(),
            [](const KeyframeRow& lhs, const KeyframeRow& rhs) { return lhs.frame < rhs.frame; });

  ApplyEditorStateToActiveCamera();
  SaveDraftFile();
  RefreshCurrentCamera();
  RefreshKeyframeTableWithSelection(&row);
}

void FreeLookKeyframeEditor::DeleteSelection()
{
  const int row = m_table->currentRow();
  if (row < 0 || row >= static_cast<int>(m_keyframes.size()))
    return;

  m_keyframes.erase(m_keyframes.begin() + row);

  KeyframeRow preferred;
  const bool has_preferred = !m_keyframes.empty();
  if (has_preferred)
    preferred = m_keyframes[std::min(row, static_cast<int>(m_keyframes.size()) - 1)];

  ApplyEditorStateToActiveCamera();
  SaveDraftFile();
  if (has_preferred)
    RefreshKeyframeTableWithSelection(&preferred);
  else
    RefreshKeyframeTable();
}

std::vector<int> FreeLookKeyframeEditor::GetSelectedRows() const
{
  std::vector<int> rows;
  for (const QModelIndex& index : m_table->selectionModel()->selectedRows())
    rows.push_back(index.row());

  std::ranges::sort(rows);
  rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
  return rows;
}

void FreeLookKeyframeEditor::CopySelection()
{
  m_copied_keyframes.clear();
  for (const int row : GetSelectedRows())
  {
    if (row >= 0 && row < static_cast<int>(m_keyframes.size()))
      m_copied_keyframes.push_back(m_keyframes[row]);
  }
}

void FreeLookKeyframeEditor::PasteCopiedKeyframes()
{
  if (m_copied_keyframes.empty())
    return;

  KeyframeRow first_pasted;
  bool has_first_pasted = false;
  for (KeyframeRow keyframe : m_copied_keyframes)
  {
    keyframe.editor_id = m_next_editor_id++;
    if (!has_first_pasted)
    {
      first_pasted = keyframe;
      has_first_pasted = true;
    }
    m_keyframes.push_back(keyframe);
  }

  std::sort(m_keyframes.begin(), m_keyframes.end(),
            [](const KeyframeRow& lhs, const KeyframeRow& rhs) { return lhs.frame < rhs.frame; });

  ApplyEditorStateToActiveCamera();
  SaveDraftFile();
  RefreshKeyframeTableWithSelection(has_first_pasted ? &first_pasted : nullptr);
}

void FreeLookKeyframeEditor::LoadPathFromDialog()
{
  QString path =
      QFileDialog::getOpenFileName(this, tr("Load Free Look Keyframe Path"), GetKeyframePath(),
                                   tr("Free Look Keyframe Paths (*.fkf);;All Files (*)"));
  if (path.isEmpty())
    return;

  path = NormalizeKeyframePath(path);
  Config::SetBaseOrCurrent(Config::FL1_KEYFRAME_PATH_FILE, path.toStdString());
  LoadFromFile();
  RefreshKeyframeTable();
  ApplyEditorStateToActiveCamera();
  SaveDraftFile();
}

void FreeLookKeyframeEditor::SavePathAs()
{
  QString path =
      QFileDialog::getSaveFileName(this, tr("Save Free Look Keyframe Path"), GetKeyframePath(),
                                   tr("Free Look Keyframe Paths (*.fkf);;All Files (*)"));
  if (path.isEmpty())
    return;

  path = NormalizeKeyframePath(path);
  if (SaveToFile(path))
    Config::SetBaseOrCurrent(Config::FL1_KEYFRAME_PATH_FILE, path.toStdString());
}

void FreeLookKeyframeEditor::OnInterpolationModeChanged(int index)
{
  m_interpolation_mode = std::clamp(index, 0, 2);
  ApplyEditorStateToActiveCamera();
  SaveDraftFile();
}

bool FreeLookKeyframeEditor::LoadFromFile()
{
  return LoadFromFilePath(GetKeyframePath());
}

bool FreeLookKeyframeEditor::LoadFromDraftFile()
{
  if (LoadFromFilePath(GetDraftKeyframePath()))
  {
    UpdateDraftTimestamp();
    return true;
  }

  return LoadFromFilePath(GetKeyframePath());
}

bool FreeLookKeyframeEditor::LoadFromFilePath(const QString& path)
{
  Common::IniFile ini;
  if (!ini.Load(path.toStdString()))
    return false;

  const auto* meta = ini.GetSection("Path");
  if (!meta)
    return false;

  m_keyframes.clear();
  m_next_editor_id = 1;

  int count = 0;
  meta->Get("Count", &count, 0);
  meta->Get("InterpolationMode", &m_interpolation_mode, m_interpolation_mode);
  m_interpolation_mode = std::clamp(m_interpolation_mode, 0, 2);
  meta->Get("FocusRelative", &m_focus_relative_keyframes, false);
  LoadFocusSettings(*meta);

  int frame_step = Config::Get(Config::FL1_KEYFRAME_DEFAULT_FRAME_STEP);
  bool sync_to_tas = Config::Get(Config::FL1_KEYFRAME_SYNC_TO_TAS);
  meta->Get("DefaultFrameStep", &frame_step, frame_step);
  meta->Get("SyncToMoviePlayback", &sync_to_tas, sync_to_tas);
  Config::SetBaseOrCurrent(Config::FL1_KEYFRAME_DEFAULT_FRAME_STEP,
                           std::clamp(frame_step, 1, 600));
  Config::SetBaseOrCurrent(Config::FL1_KEYFRAME_SYNC_TO_TAS, sync_to_tas);

  m_keyframes.reserve(std::max(0, count));

  for (int i = 0; i < count; ++i)
  {
    const auto* section = ini.GetSection(fmt::format("Keyframe{}", i));
    if (!section)
      continue;

    KeyframeRow row;

    section->Get("EditorId", &row.editor_id, uint64_t{0});
    if (row.editor_id == 0)
      row.editor_id = m_next_editor_id++;
    else
      m_next_editor_id = std::max(m_next_editor_id, row.editor_id + 1);

    section->Get("Frame", &row.frame, uint64_t{0});
    section->Get("PositionX", &row.pos_x, 0.0f);
    section->Get("PositionY", &row.pos_y, 0.0f);
    section->Get("PositionZ", &row.pos_z, 0.0f);
    section->Get("RotationX", &row.rot_qx, 0.0f);
    section->Get("RotationY", &row.rot_qy, 0.0f);
    section->Get("RotationZ", &row.rot_qz, 0.0f);
    section->Get("RotationW", &row.rot_qw, 1.0f);
    section->Get("FovX", &row.fov_x, 1.0f);
    section->Get("FovY", &row.fov_y, 1.0f);

    const bool has_euler_x = section->Get("EulerRotationX", &row.rot_x_deg, 0.0f);
    const bool has_euler_y = section->Get("EulerRotationY", &row.rot_y_deg, 0.0f);
    const bool has_euler_z = section->Get("EulerRotationZ", &row.rot_z_deg, 0.0f);
    if (!has_euler_x || !has_euler_y || !has_euler_z)
    {
      const Common::Quaternion quat(row.rot_qw, row.rot_qx, row.rot_qy, row.rot_qz);
      const Common::Vec3 euler = Common::FromQuaternionToEuler(quat.Normalized());
      row.rot_x_deg = euler.x * 180.0f / std::numbers::pi_v<float>;
      row.rot_y_deg = euler.y * 180.0f / std::numbers::pi_v<float>;
      row.rot_z_deg = euler.z * 180.0f / std::numbers::pi_v<float>;
    }

    m_keyframes.push_back(row);
  }

  std::sort(m_keyframes.begin(), m_keyframes.end(),
            [](const KeyframeRow& lhs, const KeyframeRow& rhs) { return lhs.frame < rhs.frame; });
  return true;
}

bool FreeLookKeyframeEditor::SaveToFile(const QString& path) const
{
  const std::string path_str = path.toStdString();
  if (!File::CreateFullPath(path_str))
    return false;

  Common::IniFile ini;
  auto* meta = ini.GetOrCreateSection("Path");
  meta->Set("Version", 2);
  meta->Set("Count", static_cast<int>(m_keyframes.size()));
  meta->Set("InterpolationMode", m_interpolation_mode);
  meta->Set("DefaultFrameStep", Config::Get(Config::FL1_KEYFRAME_DEFAULT_FRAME_STEP));
  meta->Set("SyncToMoviePlayback", Config::Get(Config::FL1_KEYFRAME_SYNC_TO_TAS));
  meta->Set("FocusRelative", m_focus_relative_keyframes);
  SaveFocusSettings(meta);

  for (std::size_t i = 0; i < m_keyframes.size(); ++i)
  {
    const auto& keyframe = m_keyframes[i];
    auto* section = ini.GetOrCreateSection(fmt::format("Keyframe{}", i));

    section->Set("EditorId", keyframe.editor_id);
    section->Set("Frame", keyframe.frame);
    section->Set("PositionX", keyframe.pos_x);
    section->Set("PositionY", keyframe.pos_y);
    section->Set("PositionZ", keyframe.pos_z);
    section->Set("RotationX", keyframe.rot_qx);
    section->Set("RotationY", keyframe.rot_qy);
    section->Set("RotationZ", keyframe.rot_qz);
    section->Set("RotationW", keyframe.rot_qw);
    section->Set("EulerRotationX", keyframe.rot_x_deg);
    section->Set("EulerRotationY", keyframe.rot_y_deg);
    section->Set("EulerRotationZ", keyframe.rot_z_deg);
    section->Set("FovX", std::max(keyframe.fov_x, 0.025f));
    section->Set("FovY", std::max(keyframe.fov_y, 0.025f));
  }

  return ini.Save(path_str);
}

bool FreeLookKeyframeEditor::SaveDraftFile()
{
  if (!SaveToFile(GetDraftKeyframePath()))
    return false;

  UpdateDraftTimestamp();
  return true;
}

void FreeLookKeyframeEditor::MaybeReloadDraftFromDisk()
{
  if (isActiveWindow())
    return;

  std::filesystem::file_time_type timestamp{};
  if (!GetDraftTimestamp(&timestamp))
    return;

  if (m_has_last_draft_timestamp && timestamp == m_last_draft_timestamp)
    return;

  LoadFromDraftFile();
  RefreshCurrentCamera();
  RefreshKeyframeTable();
  ApplyEditorStateToActiveCamera();
}

void FreeLookKeyframeEditor::UpdateDraftTimestamp()
{
  std::filesystem::file_time_type timestamp{};
  if (!GetDraftTimestamp(&timestamp))
  {
    m_has_last_draft_timestamp = false;
    return;
  }

  m_last_draft_timestamp = timestamp;
  m_has_last_draft_timestamp = true;
}

bool FreeLookKeyframeEditor::GetDraftTimestamp(std::filesystem::file_time_type* timestamp) const
{
  std::error_code ec;
  const auto value = std::filesystem::last_write_time(GetDraftKeyframePath().toStdString(), ec);
  if (ec)
    return false;

  if (timestamp)
    *timestamp = value;
  return true;
}

bool FreeLookKeyframeEditor::ApplyEditorStateToActiveCamera() const
{
  CameraControllerInput* controller = GetActiveKeyframeController();
  if (!controller)
    return false;

  std::vector<CameraControllerInput::KeyframeData> keyframes;
  keyframes.reserve(m_keyframes.size());
  for (const KeyframeRow& row : m_keyframes)
  {
    CameraControllerInput::KeyframeData keyframe_data;
    keyframe_data.frame = row.frame;
    keyframe_data.position = Common::Vec3{row.pos_x, row.pos_y, row.pos_z};
    keyframe_data.rotation = Common::Vec3{row.rot_x_deg * std::numbers::pi_v<float> / 180.0f,
                                          row.rot_y_deg * std::numbers::pi_v<float> / 180.0f,
                                          row.rot_z_deg * std::numbers::pi_v<float> / 180.0f};
    keyframe_data.fov = Common::Vec2{std::max(row.fov_x, 0.025f), std::max(row.fov_y, 0.025f)};
    keyframes.push_back(keyframe_data);
  }

  return controller->ReplaceKeyframes(
      std::move(keyframes),
      static_cast<CameraControllerInput::KeyframeInterpolationMode>(m_interpolation_mode),
      m_focus_relative_keyframes);
}

QString FreeLookKeyframeEditor::GetKeyframePath()
{
  std::string path = Config::Get(Config::FL1_KEYFRAME_PATH_FILE);
  if (path.empty())
    path = "freelook_camera_path.fkf";

  std::filesystem::path configured_path(path);
  if (configured_path.is_relative())
  {
    configured_path =
        std::filesystem::path(File::GetUserPath(D_CONFIG_IDX)) / "FreeLook" / configured_path;
  }

  return QString::fromStdString(configured_path.string());
}

QString FreeLookKeyframeEditor::GetDraftKeyframePath()
{
  const std::filesystem::path draft_path =
      std::filesystem::path(File::GetUserPath(D_CONFIG_IDX)) / "FreeLook" /
      "freelook_camera_draft.fkf";
  return QString::fromStdString(draft_path.string());
}

QString FreeLookKeyframeEditor::NormalizeKeyframePath(QString path)
{
  if (!path.endsWith(QStringLiteral(".fkf"), Qt::CaseInsensitive))
    path += QStringLiteral(".fkf");
  return path;
}
