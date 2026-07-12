// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDialog>

#include <cstdint>
#include <filesystem>
#include <vector>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QString;
class QSpinBox;
class QTableWidget;
class QTimer;

class FreeLookKeyframeEditor final : public QDialog
{
  Q_OBJECT

public:
  explicit FreeLookKeyframeEditor(QWidget* parent);
  void ReloadFromDraft();

private:
  struct KeyframeRow
  {
    uint64_t editor_id = 0;
    uint64_t frame = 0;
    float pos_x = 0.0f;
    float pos_y = 0.0f;
    float pos_z = 0.0f;
    float rot_x_deg = 0.0f;
    float rot_y_deg = 0.0f;
    float rot_z_deg = 0.0f;
    float rot_qx = 0.0f;
    float rot_qy = 0.0f;
    float rot_qz = 0.0f;
    float rot_qw = 1.0f;
    float fov_x = 1.0f;
    float fov_y = 1.0f;
  };

  void CreateLayout();
  void ConnectWidgets();

  void RefreshCurrentCamera();
  void RefreshKeyframeTable();
  void RefreshKeyframeTableWithSelection(const KeyframeRow* preferred_selection);
  void RefreshEditorSelection();
  int FindMatchingRow(const KeyframeRow& keyframe) const;
  void SelectRow(int row);

  void ApplySelectionEdits();
  void AddCurrentCamera();
  void DeleteSelection();
  void CopySelection();
  void PasteCopiedKeyframes();
  std::vector<int> GetSelectedRows() const;
  void LoadPathFromDialog();
  void SavePathAs();
  void OnInterpolationModeChanged(int index);
  void MaybeReloadDraftFromDisk();
  void UpdateDraftTimestamp();
  bool GetDraftTimestamp(std::filesystem::file_time_type* timestamp) const;

  bool LoadFromFile();
  bool LoadFromDraftFile();
  bool LoadFromFilePath(const QString& path);
  bool SaveToFile(const QString& path) const;
  bool SaveDraftFile();
  bool ApplyEditorStateToActiveCamera() const;

  static QString GetKeyframePath();
  static QString GetDraftKeyframePath();
  static QString NormalizeKeyframePath(QString path);

  std::vector<KeyframeRow> m_keyframes;
  std::vector<KeyframeRow> m_copied_keyframes;
  uint64_t m_next_editor_id = 1;
  int m_interpolation_mode = 1;
  bool m_focus_relative_keyframes = false;

  bool m_updating_selection = false;
  bool m_rot_x_dirty = false;
  bool m_rot_y_dirty = false;
  bool m_rot_z_dirty = false;

  QComboBox* m_interpolation_mode_combo;
  QLabel* m_current_position;
  QLabel* m_current_rotation;
  QTimer* m_draft_refresh_timer;

  QTableWidget* m_table;

  QSpinBox* m_frame;
  QDoubleSpinBox* m_pos_x;
  QDoubleSpinBox* m_pos_y;
  QDoubleSpinBox* m_pos_z;
  QDoubleSpinBox* m_rot_x;
  QDoubleSpinBox* m_rot_y;
  QDoubleSpinBox* m_rot_z;
  QDoubleSpinBox* m_fov_x;
  QDoubleSpinBox* m_fov_y;

  QPushButton* m_refresh_button;
  QPushButton* m_load_file_button;
  QPushButton* m_save_as_button;
  QPushButton* m_add_current_button;
  QPushButton* m_apply_button;
  QPushButton* m_delete_button;

  bool m_has_last_draft_timestamp = false;
  std::filesystem::file_time_type m_last_draft_timestamp{};
};
