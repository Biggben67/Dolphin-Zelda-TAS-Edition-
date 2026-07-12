// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <set>
#include <vector>

#include <QDialog>

#include "Core/HW/WiimoteEmu/DesiredWiimoteState.h"
#include "Core/Movie.h"

class QCheckBox;
class QGroupBox;
class QLabel;
class QObject;
class QPushButton;
class QEvent;
class QPoint;
class QRubberBand;
class QShowEvent;
class QSpinBox;
class QStackedWidget;
class QTableView;
class QTimer;
class QHideEvent;
class QWidget;
class DTMEditorModel;

class DTMEditorDialog final : public QDialog
{
  Q_OBJECT

public:
  explicit DTMEditorDialog(QWidget* parent = nullptr);

  bool HasMovieLoaded() const;
  void PromptOpen();

private:
  struct InputCell
  {
    int row = -1;
    int column = -1;
  };

  enum class InputField
  {
    GCConnected,
    GCStart,
    GCA,
    GCB,
    GCX,
    GCY,
    GCZ,
    GCL,
    GCR,
    GCUp,
    GCDown,
    GCLeft,
    GCRight,
    GCDisc,
    GCReset,
    GCGetOrigin,
    GCTriggerL,
    GCTriggerR,
    GCStickX,
    GCStickY,
    GCCStickX,
    GCCStickY,
    WiiReset,
    WiiA,
    WiiB,
    WiiOne,
    WiiTwo,
    WiiPlus,
    WiiMinus,
    WiiHome,
    WiiUp,
    WiiDown,
    WiiLeft,
    WiiRight,
    WiiAccelX,
    WiiAccelY,
    WiiAccelZ,
    WiiBattery,
    WiiIR1Visible,
    WiiIR1X,
    WiiIR1Y,
    WiiIR1Size,
    WiiIR2Visible,
    WiiIR2X,
    WiiIR2Y,
    WiiIR2Size,
    WiiIR3Visible,
    WiiIR3X,
    WiiIR3Y,
    WiiIR3Size,
    WiiIR4Visible,
    WiiIR4X,
    WiiIR4Y,
    WiiIR4Size,
    WiiGyroX,
    WiiGyroY,
    WiiGyroZ,
    WiiGyroSlowX,
    WiiGyroSlowY,
    WiiGyroSlowZ,
    WiiNunchukC,
    WiiNunchukZ,
    WiiNunchukStickX,
    WiiNunchukStickY,
    WiiNunchukAccelX,
    WiiNunchukAccelY,
    WiiNunchukAccelZ,
    WiiClassicA,
    WiiClassicB,
    WiiClassicX,
    WiiClassicY,
    WiiClassicZL,
    WiiClassicZR,
    WiiClassicL,
    WiiClassicR,
    WiiClassicMinus,
    WiiClassicPlus,
    WiiClassicHome,
    WiiClassicUp,
    WiiClassicDown,
    WiiClassicLeft,
    WiiClassicRight,
    WiiClassicLeftX,
    WiiClassicLeftY,
    WiiClassicRightX,
    WiiClassicRightY,
    WiiClassicTriggerL,
    WiiClassicTriggerR,
  };

  enum class EditorMovieKind
  {
    None,
    GC,
    Wii,
  };

  void CreateWidgets();
  void Refresh();
  bool RefreshGCRuntimeMovie();
  bool RefreshWiiRuntimeMovie();
  void CopySelectedInputs();
  bool PasteCopiedInputs(int start_row);
  void PopulateEditor();
  void PopulateGCEditor(int row, int controller);
  void PopulateWiiEditor(int row);
  void ApplyEditorChanges();
  void SetEditorEnabled(bool enabled);
  bool LoadFile(const QString& path);
  bool SaveFile(const QString& path);
  void UpdateStatusLabel();
  void UpdateHotkeyFocusState();
  std::vector<InputCell> GetSelectedInputCells() const;
  std::vector<int> GetSelectedRows() const;
  bool IsInputCellSelected(const std::vector<InputCell>& cells, int row, int column) const;
  bool MoveSelectedInputs(const std::vector<InputCell>& selected_cells, int dest_start);
  void SelectCells(const std::vector<InputCell>& cells, int current_row, int current_column);
  void SelectRows(const std::vector<int>& rows, int current_row);
  void RefreshEditedRows(const std::vector<int>& rows);
  bool HasCopiedInputs() const;
  bool HasCompatibleCopiedInputs() const;
  void ShowTableContextMenu(const QPoint& pos);
  void RegisterInputField(QWidget* widget, InputField field);
  void ToggleSelectedInputField(InputField field);
  bool IsInputFieldFilterActive() const;
  std::set<InputField> ActiveInputFieldFilter() const;
  void UpdateInputFieldSelectionStyles();
  Movie::ControllerState CopyGCInputFields(const Movie::ControllerState& destination,
                                           const Movie::ControllerState& source,
                                           const std::set<InputField>& fields) const;
  Movie::ControllerState NeutralizeGCInputFields(const Movie::ControllerState& state,
                                                 const std::set<InputField>& fields) const;
  Movie::WiiRuntimeInputRow CopyWiiInputFields(const Movie::WiiRuntimeInputRow& destination,
                                               const Movie::WiiRuntimeInputRow& source,
                                               const std::set<InputField>& fields) const;
  Movie::WiiRuntimeInputRow NeutralizeWiiInputFields(const Movie::WiiRuntimeInputRow& row,
                                                     const std::set<InputField>& fields) const;
  void ClearPendingDrag();
  void BeginPendingDrag();
  void FinishPendingDrag(bool apply_drag);
  void UpdateDragPreview();
  int GetPrimaryDataColumn() const;
  void ApplyGCEditorChange(Movie::ControllerState* state, const QObject* source) const;
  void ApplyWiiEditorChange(Movie::WiiRuntimeInputRow* row, const QObject* source) const;
  Movie::ControllerState BuildGCStateFromEditor() const;
  WiimoteEmu::SerializedWiimoteState BuildWiiSerializedStateFromEditor() const;
  bool eventFilter(QObject* watched, QEvent* event) override;
  void changeEvent(QEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

  DTMEditorModel* m_model = nullptr;
  QTableView* m_table = nullptr;
  QLabel* m_status_label = nullptr;
  QGroupBox* m_editor_box = nullptr;
  QStackedWidget* m_editor_stack = nullptr;
  QPushButton* m_open_button = nullptr;
  QPushButton* m_save_button = nullptr;
  QCheckBox* m_select_buttons = nullptr;
  QRubberBand* m_drag_preview = nullptr;
  QTimer* m_refresh_timer = nullptr;
  QTimer* m_drag_hold_timer = nullptr;

  QCheckBox* m_connected = nullptr;
  QCheckBox* m_start = nullptr;
  QCheckBox* m_a = nullptr;
  QCheckBox* m_b = nullptr;
  QCheckBox* m_x = nullptr;
  QCheckBox* m_y = nullptr;
  QCheckBox* m_z = nullptr;
  QCheckBox* m_l = nullptr;
  QCheckBox* m_r = nullptr;
  QCheckBox* m_up = nullptr;
  QCheckBox* m_down = nullptr;
  QCheckBox* m_left = nullptr;
  QCheckBox* m_right = nullptr;
  QCheckBox* m_disc = nullptr;
  QCheckBox* m_reset = nullptr;
  QCheckBox* m_get_origin = nullptr;
  QSpinBox* m_trigger_l = nullptr;
  QSpinBox* m_trigger_r = nullptr;
  QSpinBox* m_stick_x = nullptr;
  QSpinBox* m_stick_y = nullptr;
  QSpinBox* m_cstick_x = nullptr;
  QSpinBox* m_cstick_y = nullptr;

  QLabel* m_wii_extension_label = nullptr;
  QLabel* m_wii_motion_plus_label = nullptr;
  QLabel* m_wii_battery_label = nullptr;
  QCheckBox* m_wii_hide_remote_inputs = nullptr;
  QGroupBox* m_wii_remote_button_group = nullptr;
  QGroupBox* m_wii_accel_group = nullptr;
  QGroupBox* m_wii_ir_group = nullptr;
  QGroupBox* m_wii_gyro_group = nullptr;
  QCheckBox* m_wii_reset = nullptr;
  QCheckBox* m_wii_a = nullptr;
  QCheckBox* m_wii_b = nullptr;
  QCheckBox* m_wii_one = nullptr;
  QCheckBox* m_wii_two = nullptr;
  QCheckBox* m_wii_plus = nullptr;
  QCheckBox* m_wii_minus = nullptr;
  QCheckBox* m_wii_home = nullptr;
  QCheckBox* m_wii_up = nullptr;
  QCheckBox* m_wii_down = nullptr;
  QCheckBox* m_wii_left = nullptr;
  QCheckBox* m_wii_right = nullptr;
  QCheckBox* m_wii_c = nullptr;
  QCheckBox* m_wii_z = nullptr;
  QCheckBox* m_wii_gyro_slow_x = nullptr;
  QCheckBox* m_wii_gyro_slow_y = nullptr;
  QCheckBox* m_wii_gyro_slow_z = nullptr;
  QSpinBox* m_wii_accel_x = nullptr;
  QSpinBox* m_wii_accel_y = nullptr;
  QSpinBox* m_wii_accel_z = nullptr;
  QSpinBox* m_wii_battery = nullptr;
  std::array<QCheckBox*, 4> m_wii_ir_visible{};
  std::array<QSpinBox*, 4> m_wii_ir_x{};
  std::array<QSpinBox*, 4> m_wii_ir_y{};
  std::array<QSpinBox*, 4> m_wii_ir_size{};
  QSpinBox* m_wii_gyro_x = nullptr;
  QSpinBox* m_wii_gyro_y = nullptr;
  QSpinBox* m_wii_gyro_z = nullptr;
  QSpinBox* m_wii_nunchuk_x = nullptr;
  QSpinBox* m_wii_nunchuk_y = nullptr;
  QSpinBox* m_wii_nunchuk_accel_x = nullptr;
  QSpinBox* m_wii_nunchuk_accel_y = nullptr;
  QSpinBox* m_wii_nunchuk_accel_z = nullptr;
  QCheckBox* m_wii_classic_a = nullptr;
  QCheckBox* m_wii_classic_b = nullptr;
  QCheckBox* m_wii_classic_x = nullptr;
  QCheckBox* m_wii_classic_y = nullptr;
  QCheckBox* m_wii_classic_zl = nullptr;
  QCheckBox* m_wii_classic_zr = nullptr;
  QCheckBox* m_wii_classic_l = nullptr;
  QCheckBox* m_wii_classic_r = nullptr;
  QCheckBox* m_wii_classic_minus = nullptr;
  QCheckBox* m_wii_classic_plus = nullptr;
  QCheckBox* m_wii_classic_home = nullptr;
  QCheckBox* m_wii_classic_up = nullptr;
  QCheckBox* m_wii_classic_down = nullptr;
  QCheckBox* m_wii_classic_left = nullptr;
  QCheckBox* m_wii_classic_right = nullptr;
  QSpinBox* m_wii_classic_left_x = nullptr;
  QSpinBox* m_wii_classic_left_y = nullptr;
  QSpinBox* m_wii_classic_right_x = nullptr;
  QSpinBox* m_wii_classic_right_y = nullptr;
  QSpinBox* m_wii_classic_l_trigger = nullptr;
  QSpinBox* m_wii_classic_r_trigger = nullptr;

  QString m_file_path;
  Movie::DTMHeader m_header{};
  EditorMovieKind m_file_movie_kind = EditorMovieKind::None;
  EditorMovieKind m_runtime_movie_kind = EditorMovieKind::None;
  bool m_using_runtime_movie = false;
  int m_last_runtime_row = -1;
  u64 m_last_runtime_frame = 0;
  u64 m_last_runtime_data_generation = 0;
  bool m_has_last_runtime_frame = false;
  bool m_dirty = false;
  bool m_updating_editor = false;
  bool m_refresh_in_progress = false;
  bool m_drag_pending = false;
  bool m_drag_active = false;
  std::vector<InputCell> m_drag_cells;
  int m_drag_press_row = -1;
  int m_drag_press_column = -1;
  int m_drag_hover_row = -1;
  EditorMovieKind m_copied_movie_kind = EditorMovieKind::None;
  std::vector<std::array<Movie::ControllerState, 4>> m_copied_gc_rows;
  std::vector<std::array<bool, 4>> m_copied_gc_cells;
  std::array<bool, 4> m_copied_gc_controllers{};
  std::vector<Movie::WiiRuntimeInputRow> m_copied_wii_rows;
  bool m_copied_wii_classic_only = false;
  std::set<InputField> m_copied_input_fields;
  std::set<InputField> m_selected_input_fields;
  std::vector<QWidget*> m_input_field_widgets;

  Movie::WiiRuntimeInputRow m_current_wii_row{};
  WiimoteEmu::DesiredWiimoteState m_current_wii_state{};
  bool m_current_wii_state_valid = false;
};
