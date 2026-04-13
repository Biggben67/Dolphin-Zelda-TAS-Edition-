#include "DolphinQt/TAS/DTMEditorDialog.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <new>
#include <optional>
#include <ranges>
#include <variant>
#include <vector>

#include <QAbstractItemView>
#include <QAbstractTableModel>
#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QStringList>

#include "Common/CommonTypes.h"
#include "Core/Core.h"
#include "Core/HW/WiimoteEmu/Extension/Nunchuk.h"
#include "Core/HW/WiimoteEmu/MotionPlus.h"
#include "Core/System.h"

namespace
{
enum class ModelMovieKind
{
  None,
  GC,
  Wii,
};

constexpr int k_empty_page = 0;
constexpr int k_gc_page = 1;
constexpr int k_wii_page = 2;
constexpr double k_gyro_stretch =
    static_cast<double>(WiimoteEmu::MotionPlus::CALIBRATION_FAST_SCALE_DEGREES) /
    WiimoteEmu::MotionPlus::CALIBRATION_SLOW_SCALE_DEGREES;
constexpr int k_gyro_min = 0;
const int k_gyro_max =
    static_cast<int>(std::lround(WiimoteEmu::MotionPlus::MAX_VALUE * k_gyro_stretch));
const int k_gyro_center =
    static_cast<int>(std::lround(WiimoteEmu::MotionPlus::ZERO_VALUE * k_gyro_stretch));
constexpr int k_nunchuk_accel_zero = WiimoteEmu::Nunchuk::ACCEL_ZERO_G << 2;
constexpr int k_nunchuk_accel_one = WiimoteEmu::Nunchuk::ACCEL_ONE_G << 2;

struct ParsedGCDTMData
{
  Movie::DTMHeader header{};
  std::array<bool, 4> active_controllers{};
  std::vector<std::array<Movie::ControllerState, 4>> rows;
};

struct ParsedWiiDTMData
{
  Movie::DTMHeader header{};
  std::array<bool, 4> active_wiimotes{};
  std::vector<Movie::WiiRuntimeInputRow> rows;
};

int GyroRawToTasValue(u16 raw_value)
{
  return static_cast<int>(std::lround(raw_value * k_gyro_stretch));
}

u16 TasGyroToRawValue(int tas_value)
{
  return static_cast<u16>(std::clamp<int>(static_cast<int>(std::lround(tas_value / k_gyro_stretch)),
                                          0, WiimoteEmu::MotionPlus::MAX_VALUE));
}

std::array<bool, 4> GetGCControllersFromHeader(const Movie::DTMHeader& header)
{
  std::array<bool, 4> controllers{};
  for (int i = 0; i < 4; ++i)
    controllers[i] = (header.controllers & (1 << i)) != 0 || (header.GBAControllers & (1 << i)) != 0;
  return controllers;
}

std::array<bool, 4> GetWiimotesFromHeader(const Movie::DTMHeader& header)
{
  std::array<bool, 4> wiimotes{};
  for (int i = 0; i < 4; ++i)
    wiimotes[i] = (header.controllers & (1 << (4 + i))) != 0;
  return wiimotes;
}

std::optional<int> GetSingleActiveWiimoteFromHeader(const Movie::DTMHeader& header)
{
  if (std::ranges::any_of(GetGCControllersFromHeader(header), [](bool active) { return active; }) ||
      header.GBAControllers != 0)
  {
    return std::nullopt;
  }

  int active = -1;
  const auto wiimotes = GetWiimotesFromHeader(header);
  for (int i = 0; i < 4; ++i)
  {
    if (!wiimotes[i])
      continue;
    if (active != -1)
      return std::nullopt;
    active = i;
  }

  return active >= 0 ? std::optional<int>(active) : std::nullopt;
}

QString FormatGameFrame(const std::optional<u64>& frame)
{
  return frame.has_value() ? QString::number(*frame) : QString();
}

std::vector<std::optional<u64>> PopulateMissingGameFrames(std::vector<std::optional<u64>> row_game_frames,
                                                          const size_t row_count,
                                                          const u64 current_frame,
                                                          const u64 current_input_row)
{
  row_game_frames.resize(row_count);

  const s64 frame_base =
      static_cast<s64>(current_frame) - static_cast<s64>(current_input_row);

  for (size_t row = 0; row < row_count; ++row)
  {
    if (row_game_frames[row].has_value())
      continue;

    const s64 derived = frame_base + static_cast<s64>(row);
    row_game_frames[row] = static_cast<u64>(std::max<s64>(0, derived));
  }

  return row_game_frames;
}

QString FormatControllerState(const Movie::ControllerState& state)
{
  if (!state.is_connected)
    return QStringLiteral("[disconnected]");

  QStringList parts;
  if (state.Start)
    parts << QStringLiteral("Start");
  if (state.A)
    parts << QStringLiteral("A");
  if (state.B)
    parts << QStringLiteral("B");
  if (state.X)
    parts << QStringLiteral("X");
  if (state.Y)
    parts << QStringLiteral("Y");
  if (state.Z)
    parts << QStringLiteral("Z");
  if (state.L)
    parts << QStringLiteral("L");
  if (state.R)
    parts << QStringLiteral("R");
  if (state.DPadUp)
    parts << QStringLiteral("Up");
  if (state.DPadDown)
    parts << QStringLiteral("Down");
  if (state.DPadLeft)
    parts << QStringLiteral("Left");
  if (state.DPadRight)
    parts << QStringLiteral("Right");
  if (state.reset)
    parts << QStringLiteral("Reset");
  if (state.disc)
    parts << QStringLiteral("Disc");
  parts << QStringLiteral("LS(%1,%2)").arg(state.AnalogStickX).arg(state.AnalogStickY);
  parts << QStringLiteral("CS(%1,%2)").arg(state.CStickX).arg(state.CStickY);
  parts << QStringLiteral("LT%1").arg(state.TriggerL);
  parts << QStringLiteral("RT%1").arg(state.TriggerR);
  return parts.join(QStringLiteral(" "));
}

QString FormatWiiState(const Movie::WiiRuntimeInputRow& row)
{
  if (row.is_reset)
    return QStringLiteral("[RESET]");

  WiimoteEmu::DesiredWiimoteState state;
  if (!WiimoteEmu::DeserializeDesiredState(&state, row.serialized_state))
    return QStringLiteral("[invalid]");

  QStringList parts;
  if (state.buttons.a)
    parts << QStringLiteral("A");
  if (state.buttons.b)
    parts << QStringLiteral("B");
  if (state.buttons.one)
    parts << QStringLiteral("1");
  if (state.buttons.two)
    parts << QStringLiteral("2");
  if (state.buttons.plus)
    parts << QStringLiteral("+");
  if (state.buttons.minus)
    parts << QStringLiteral("-");
  if (state.buttons.home)
    parts << QStringLiteral("HOME");
  if (state.buttons.up)
    parts << QStringLiteral("Up");
  if (state.buttons.down)
    parts << QStringLiteral("Down");
  if (state.buttons.left)
    parts << QStringLiteral("Left");
  if (state.buttons.right)
    parts << QStringLiteral("Right");

  parts << QStringLiteral("Accel(%1,%2,%3)")
               .arg(state.acceleration.value.x)
               .arg(state.acceleration.value.y)
               .arg(state.acceleration.value.z);

  if (state.motion_plus.has_value())
  {
    const auto& gyro = state.motion_plus->gyro.value;
    parts << QStringLiteral("Gyro(%1,%2,%3)")
                 .arg(GyroRawToTasValue(gyro.x))
                 .arg(GyroRawToTasValue(gyro.y))
                 .arg(GyroRawToTasValue(gyro.z));
  }

  if (state.battery.has_value())
    parts << QStringLiteral("Bat(%1%)").arg(*state.battery);

  if (std::holds_alternative<WiimoteEmu::Nunchuk::DataFormat>(state.extension.data))
  {
    const auto& nunchuk = std::get<WiimoteEmu::Nunchuk::DataFormat>(state.extension.data);
    const u8 buttons = nunchuk.GetButtons();
    QStringList ext;
    if (buttons & WiimoteEmu::Nunchuk::BUTTON_C)
      ext << QStringLiteral("C");
    if (buttons & WiimoteEmu::Nunchuk::BUTTON_Z)
      ext << QStringLiteral("Z");
    ext << QStringLiteral("Stick(%1,%2)").arg(nunchuk.GetStick().value.x).arg(nunchuk.GetStick().value.y);
    ext << QStringLiteral("NAccel(%1,%2,%3)")
               .arg(nunchuk.GetAccel().value.x)
               .arg(nunchuk.GetAccel().value.y)
               .arg(nunchuk.GetAccel().value.z);
    parts << ext.join(QStringLiteral(" "));
  }

  return parts.join(QStringLiteral(" "));
}

std::optional<ParsedGCDTMData> ParseGCDTMFile(const QByteArray& bytes)
{
  if (bytes.size() < static_cast<int>(sizeof(Movie::DTMHeader)))
    return std::nullopt;

  ParsedGCDTMData parsed;
  std::memcpy(&parsed.header, bytes.constData(), sizeof(parsed.header));
  parsed.active_controllers = GetGCControllersFromHeader(parsed.header);
  const auto active_wiimotes = GetWiimotesFromHeader(parsed.header);
  if (parsed.header.bWii || std::ranges::any_of(active_wiimotes, [](bool active) { return active; }))
    return std::nullopt;

  size_t row_size = 0;
  for (bool active : parsed.active_controllers)
  {
    if (active)
      row_size += sizeof(Movie::ControllerState);
  }
  if (row_size == 0)
    return std::nullopt;

  const QByteArray payload = bytes.mid(sizeof(Movie::DTMHeader));
  if (payload.size() % static_cast<int>(row_size) != 0)
    return std::nullopt;

  for (int offset = 0; offset < payload.size(); offset += static_cast<int>(row_size))
  {
    std::array<Movie::ControllerState, 4> row{};
    int cursor = offset;
    for (int i = 0; i < 4; ++i)
    {
      if (!parsed.active_controllers[i])
        continue;
      std::memcpy(&row[i], payload.constData() + cursor, sizeof(Movie::ControllerState));
      cursor += sizeof(Movie::ControllerState);
    }
    parsed.rows.push_back(row);
  }

  return parsed;
}

std::optional<ParsedWiiDTMData> ParseWiiDTMFile(const QByteArray& bytes)
{
  if (bytes.size() < static_cast<int>(sizeof(Movie::DTMHeader)))
    return std::nullopt;

  ParsedWiiDTMData parsed;
  std::memcpy(&parsed.header, bytes.constData(), sizeof(parsed.header));
  parsed.active_wiimotes = GetWiimotesFromHeader(parsed.header);
  const auto wiimote = GetSingleActiveWiimoteFromHeader(parsed.header);
  if (!parsed.header.bWii || !wiimote.has_value())
    return std::nullopt;

  const QByteArray payload = bytes.mid(sizeof(Movie::DTMHeader));
  int offset = 0;
  while (offset < payload.size())
  {
    const u8 length = static_cast<u8>(payload[offset++]);
    if (offset + length > payload.size())
      return std::nullopt;

    Movie::WiiRuntimeInputRow row;
    row.wiimote = *wiimote;
    row.is_reset = length == 0;
    row.serialized_state.length = length;
    if (length > 0)
      std::copy_n(reinterpret_cast<const u8*>(payload.constData()) + offset, length,
                  row.serialized_state.data.begin());
    offset += length;
    parsed.rows.push_back(row);
  }

  return parsed;
}

}  // namespace

class DTMEditorModel final : public QAbstractTableModel
{
public:
  explicit DTMEditorModel(QObject* parent = nullptr) : QAbstractTableModel(parent) {}

  int rowCount(const QModelIndex& parent = {}) const override
  {
    if (parent.isValid())
      return 0;
    return m_kind == ModelMovieKind::GC ? static_cast<int>(m_gc_rows.size()) :
           m_kind == ModelMovieKind::Wii ? static_cast<int>(m_wii_rows.size()) : 0;
  }

  int columnCount(const QModelIndex& parent = {}) const override
  {
    if (parent.isValid())
      return 0;
    if (m_kind == ModelMovieKind::GC)
      return 1 + m_active_gc_count;
    if (m_kind == ModelMovieKind::Wii)
      return 2;
    return 1;
  }

  QVariant data(const QModelIndex& index, int role) const override
  {
    if (!index.isValid())
      return {};

    if (role == Qt::DisplayRole)
    {
      if (index.column() == 0)
        return FormatGameFrame(index.row() < static_cast<int>(m_row_game_frames.size()) ?
                                   m_row_game_frames[index.row()] :
                                   std::optional<u64>{});

      if (m_kind == ModelMovieKind::GC)
      {
        int controller = -1;
        if (IsGCControllerColumn(index.column(), &controller))
          return FormatControllerState(m_gc_rows[index.row()][controller]);
      }
      else if (m_kind == ModelMovieKind::Wii && index.column() == 1)
      {
        return FormatWiiState(m_wii_rows[index.row()]);
      }
    }
    else if (role == Qt::TextAlignmentRole)
    {
      return QVariant::fromValue(index.column() == 0 ? (Qt::AlignRight | Qt::AlignVCenter) :
                                                     (Qt::AlignLeft | Qt::AlignVCenter));
    }
    else if (role == Qt::BackgroundRole && index.row() == m_highlight_row)
    {
      return QBrush(QColor(120, 200, 120, 96));
    }

    return {};
  }

  QVariant headerData(int section, Qt::Orientation orientation, int role) const override
  {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
      return QAbstractTableModel::headerData(section, orientation, role);

    if (section == 0)
      return QStringLiteral("Game Frame");
    if (m_kind == ModelMovieKind::GC)
    {
      int visual = 1;
      for (int i = 0; i < 4; ++i)
      {
        if (!m_active_gc[i])
          continue;
        if (visual == section)
          return QStringLiteral("P%1").arg(i + 1);
        ++visual;
      }
    }
    else if (m_kind == ModelMovieKind::Wii && section == 1)
    {
      for (int i = 0; i < 4; ++i)
      {
        if (m_active_wii[i])
          return QStringLiteral("W%1").arg(i + 1);
      }
      return QStringLiteral("Wiimote");
    }

    return {};
  }

  Qt::ItemFlags flags(const QModelIndex& index) const override
  {
    if (!index.isValid())
      return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
  }

  void Clear()
  {
    beginResetModel();
    m_kind = ModelMovieKind::None;
    m_active_gc = {};
    m_active_wii = {};
    m_active_gc_count = 0;
    m_gc_rows.clear();
    m_wii_rows.clear();
    m_row_game_frames.clear();
    m_highlight_row = -1;
    endResetModel();
  }

  void SetGCMovieData(const std::array<bool, 4>& active_controllers,
                      const std::vector<std::array<Movie::ControllerState, 4>>& rows,
                      const std::vector<std::optional<u64>>& row_game_frames)
  {
    beginResetModel();
    m_kind = ModelMovieKind::GC;
    m_active_gc = active_controllers;
    m_active_gc_count = 0;
    for (bool active : active_controllers)
    {
      if (active)
        ++m_active_gc_count;
    }
    m_gc_rows = rows;
    m_wii_rows.clear();
    m_row_game_frames = row_game_frames;
    m_row_game_frames.resize(m_gc_rows.size());
    endResetModel();
  }

  void SetWiiMovieData(const std::array<bool, 4>& active_wiimotes,
                       const std::vector<Movie::WiiRuntimeInputRow>& rows,
                       const std::vector<std::optional<u64>>& row_game_frames)
  {
    beginResetModel();
    m_kind = ModelMovieKind::Wii;
    m_active_wii = active_wiimotes;
    m_gc_rows.clear();
    m_wii_rows = rows;
    m_row_game_frames = row_game_frames;
    m_row_game_frames.resize(m_wii_rows.size());
    endResetModel();
  }

  ModelMovieKind GetKind() const { return m_kind; }
  int GetFirstGCControllerColumn() const { return m_active_gc_count > 0 ? 1 : -1; }
  void SetHighlightedRow(int row)
  {
    if (m_highlight_row == row)
      return;
    const int previous = m_highlight_row;
    m_highlight_row = row;
    if (previous >= 0 && previous < rowCount())
      emit dataChanged(index(previous, 0), index(previous, std::max(0, columnCount() - 1)));
    if (m_highlight_row >= 0 && m_highlight_row < rowCount())
      emit dataChanged(index(m_highlight_row, 0),
                       index(m_highlight_row, std::max(0, columnCount() - 1)));
  }

  bool IsGCControllerColumn(int column, int* controller_out = nullptr) const
  {
    if (m_kind != ModelMovieKind::GC || column <= 0)
      return false;

    int visual = 1;
    for (int i = 0; i < 4; ++i)
    {
      if (!m_active_gc[i])
        continue;
      if (visual == column)
      {
        if (controller_out)
          *controller_out = i;
        return true;
      }
      ++visual;
    }
    return false;
  }

  const Movie::ControllerState& GetGCState(int row, int controller) const
  {
    return m_gc_rows.at(static_cast<size_t>(row)).at(static_cast<size_t>(controller));
  }

  void SetGCState(int row, int controller, const Movie::ControllerState& state)
  {
    m_gc_rows.at(static_cast<size_t>(row)).at(static_cast<size_t>(controller)) = state;
    const QModelIndex idx = index(row, ControllerToColumn(controller));
    emit dataChanged(idx, idx);
  }

  Movie::WiiRuntimeInputRow GetWiiRow(int row) const
  {
    return m_wii_rows.at(static_cast<size_t>(row));
  }

  void SetWiiRow(int row, const Movie::WiiRuntimeInputRow& data)
  {
    m_wii_rows.at(static_cast<size_t>(row)) = data;
    const QModelIndex idx = index(row, 1);
    emit dataChanged(idx, idx);
  }

private:
  int ControllerToColumn(int controller) const
  {
    int visual = 1;
    for (int i = 0; i < 4; ++i)
    {
      if (!m_active_gc[i])
        continue;
      if (i == controller)
        return visual;
      ++visual;
    }
    return -1;
  }

  ModelMovieKind m_kind = ModelMovieKind::None;
  std::array<bool, 4> m_active_gc{};
  std::array<bool, 4> m_active_wii{};
  int m_active_gc_count = 0;
  std::vector<std::array<Movie::ControllerState, 4>> m_gc_rows;
  std::vector<Movie::WiiRuntimeInputRow> m_wii_rows;
  std::vector<std::optional<u64>> m_row_game_frames;
  int m_highlight_row = -1;
};

DTMEditorDialog::DTMEditorDialog(QWidget* parent) : QDialog(parent)
{
  setWindowTitle(tr("DTM Editor"));
  resize(1100, 700);

  CreateWidgets();
  Refresh();

  m_refresh_timer = new QTimer(this);
  m_refresh_timer->setInterval(100);
  connect(m_refresh_timer, &QTimer::timeout, this, &DTMEditorDialog::Refresh);
  m_refresh_timer->start();
}

bool DTMEditorDialog::HasMovieLoaded() const
{
  return m_model && m_model->GetKind() != ModelMovieKind::None;
}

void DTMEditorDialog::PromptOpen()
{
  const QString path = QFileDialog::getOpenFileName(this, tr("Open DTM"), QString(),
                                                    tr("Dolphin TAS Movies (*.dtm)"));
  if (!path.isEmpty())
    LoadFile(path);
}

void DTMEditorDialog::CreateWidgets()
{
  m_model = new DTMEditorModel(this);
  m_table = new QTableView(this);
  m_table->setModel(m_model);
  m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_table->setAlternatingRowColors(true);
  m_table->setSortingEnabled(false);
  m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  m_table->verticalHeader()->setDefaultSectionSize(6);
  m_table->verticalHeader()->setVisible(false);
  m_table->horizontalHeader()->setStretchLastSection(true);
  m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  m_table->setStyleSheet(
      QStringLiteral("QTableView::item:selected { background-color: rgba(120, 200, 120, 96); }"));
  connect(m_table->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
          [this](const QModelIndex&, const QModelIndex&) { PopulateEditor(); });

  m_open_button = new QPushButton(tr("Open DTM"), this);
  m_paste_button = new QPushButton(tr("Paste Inputs"), this);
  m_save_button = new QPushButton(tr("Save"), this);
  connect(m_open_button, &QPushButton::clicked, this, &DTMEditorDialog::PromptOpen);
  connect(m_paste_button, &QPushButton::clicked, this, &DTMEditorDialog::PasteInputs);
  connect(m_save_button, &QPushButton::clicked, this, [this] {
    if (m_using_runtime_movie)
    {
      const QString path = QFileDialog::getSaveFileName(this, tr("Save DTM"), QString(),
                                                        tr("Dolphin TAS Movies (*.dtm)"));
      if (path.isEmpty())
        return;
      Core::System::GetInstance().GetMovie().SaveRecording(path.toStdString());
      return;
    }

    if (m_file_path.isEmpty())
    {
      const QString path = QFileDialog::getSaveFileName(this, tr("Save DTM"), QString(),
                                                        tr("Dolphin TAS Movies (*.dtm)"));
      if (path.isEmpty())
        return;
      m_file_path = path;
    }
    SaveFile(m_file_path);
  });

  auto* button_layout = new QHBoxLayout;
  button_layout->addWidget(m_open_button);
  button_layout->addWidget(m_paste_button);
  button_layout->addWidget(m_save_button);
  button_layout->addStretch();

  m_editor_box = new QGroupBox(tr("Selected Frame"), this);
  m_editor_stack = new QStackedWidget(m_editor_box);

  auto* empty_page = new QWidget(m_editor_stack);
  auto* empty_layout = new QVBoxLayout(empty_page);
  empty_layout->addWidget(new QLabel(tr("No movie loaded."), empty_page));
  empty_layout->addStretch();
  m_editor_stack->addWidget(empty_page);

  auto* gc_page = new QWidget(m_editor_stack);
  auto* gc_layout = new QGridLayout(gc_page);
  m_connected = new QCheckBox(tr("Connected"), gc_page);
  m_start = new QCheckBox(tr("Start"), gc_page);
  m_a = new QCheckBox(tr("A"), gc_page);
  m_b = new QCheckBox(tr("B"), gc_page);
  m_x = new QCheckBox(tr("X"), gc_page);
  m_y = new QCheckBox(tr("Y"), gc_page);
  m_z = new QCheckBox(tr("Z"), gc_page);
  m_l = new QCheckBox(tr("L"), gc_page);
  m_r = new QCheckBox(tr("R"), gc_page);
  m_up = new QCheckBox(tr("Up"), gc_page);
  m_down = new QCheckBox(tr("Down"), gc_page);
  m_left = new QCheckBox(tr("Left"), gc_page);
  m_right = new QCheckBox(tr("Right"), gc_page);
  m_disc = new QCheckBox(tr("Disc Change"), gc_page);
  m_reset = new QCheckBox(tr("Reset"), gc_page);
  m_get_origin = new QCheckBox(tr("Get Origin"), gc_page);
  m_trigger_l = new QSpinBox(gc_page);
  m_trigger_r = new QSpinBox(gc_page);
  m_stick_x = new QSpinBox(gc_page);
  m_stick_y = new QSpinBox(gc_page);
  m_cstick_x = new QSpinBox(gc_page);
  m_cstick_y = new QSpinBox(gc_page);
  for (QSpinBox* box : {m_trigger_l, m_trigger_r, m_stick_x, m_stick_y, m_cstick_x, m_cstick_y})
  {
    box->setRange(0, 255);
  }

  const std::array<QCheckBox*, 16> gc_checks = {m_connected, m_start, m_a,        m_b,
                                                m_x,         m_y,     m_z,        m_l,
                                                m_r,         m_up,    m_down,     m_left,
                                                m_right,     m_disc,  m_reset,    m_get_origin};
  int gc_row = 0;
  int gc_col = 0;
  for (QCheckBox* box : gc_checks)
  {
    gc_layout->addWidget(box, gc_row, gc_col);
    if (++gc_col == 4)
    {
      gc_col = 0;
      ++gc_row;
    }
    connect(box, &QCheckBox::toggled, this, [this] { ApplyEditorChanges(); });
  }

  gc_layout->addWidget(new QLabel(tr("Trigger L"), gc_page), gc_row, 0);
  gc_layout->addWidget(m_trigger_l, gc_row, 1);
  gc_layout->addWidget(new QLabel(tr("Trigger R"), gc_page), gc_row, 2);
  gc_layout->addWidget(m_trigger_r, gc_row, 3);
  ++gc_row;
  gc_layout->addWidget(new QLabel(tr("Stick X"), gc_page), gc_row, 0);
  gc_layout->addWidget(m_stick_x, gc_row, 1);
  gc_layout->addWidget(new QLabel(tr("Stick Y"), gc_page), gc_row, 2);
  gc_layout->addWidget(m_stick_y, gc_row, 3);
  ++gc_row;
  gc_layout->addWidget(new QLabel(tr("C-Stick X"), gc_page), gc_row, 0);
  gc_layout->addWidget(m_cstick_x, gc_row, 1);
  gc_layout->addWidget(new QLabel(tr("C-Stick Y"), gc_page), gc_row, 2);
  gc_layout->addWidget(m_cstick_y, gc_row, 3);
  gc_layout->setRowStretch(gc_row + 1, 1);
  for (QSpinBox* box : {m_trigger_l, m_trigger_r, m_stick_x, m_stick_y, m_cstick_x, m_cstick_y})
    connect(box, qOverload<int>(&QSpinBox::valueChanged), this, [this] { ApplyEditorChanges(); });
  m_editor_stack->addWidget(gc_page);

  auto* wii_page = new QWidget(m_editor_stack);
  auto* wii_layout = new QVBoxLayout(wii_page);
  auto* info_layout = new QGridLayout;
  m_wii_extension_label = new QLabel(wii_page);
  m_wii_motion_plus_label = new QLabel(wii_page);
  m_wii_reset = new QCheckBox(tr("Reset"), wii_page);
  info_layout->addWidget(new QLabel(tr("Extension:"), wii_page), 0, 0);
  info_layout->addWidget(m_wii_extension_label, 0, 1);
  info_layout->addWidget(new QLabel(tr("MotionPlus:"), wii_page), 1, 0);
  info_layout->addWidget(m_wii_motion_plus_label, 1, 1);
  info_layout->addWidget(new QLabel(tr("Battery (%):"), wii_page), 2, 0);
  m_wii_battery = new QSpinBox(wii_page);
  m_wii_battery->setRange(0, 100);
  info_layout->addWidget(m_wii_battery, 2, 1);
  info_layout->addWidget(new QLabel(tr("Reset Marker:"), wii_page), 3, 0);
  info_layout->addWidget(m_wii_reset, 3, 1);
  wii_layout->addLayout(info_layout);

  auto* wii_button_group = new QGroupBox(tr("Wii Remote Buttons"), wii_page);
  auto* wii_button_layout = new QGridLayout(wii_button_group);
  m_wii_a = new QCheckBox(tr("A"), wii_button_group);
  m_wii_b = new QCheckBox(tr("B"), wii_button_group);
  m_wii_one = new QCheckBox(tr("1"), wii_button_group);
  m_wii_two = new QCheckBox(tr("2"), wii_button_group);
  m_wii_plus = new QCheckBox(tr("+"), wii_button_group);
  m_wii_minus = new QCheckBox(tr("-"), wii_button_group);
  m_wii_home = new QCheckBox(tr("HOME"), wii_button_group);
  m_wii_up = new QCheckBox(tr("Up"), wii_button_group);
  m_wii_down = new QCheckBox(tr("Down"), wii_button_group);
  m_wii_left = new QCheckBox(tr("Left"), wii_button_group);
  m_wii_right = new QCheckBox(tr("Right"), wii_button_group);
  const std::array<QCheckBox*, 11> wii_checks = {m_wii_a,    m_wii_b,   m_wii_one, m_wii_two,
                                                 m_wii_plus, m_wii_minus, m_wii_home, m_wii_up,
                                                 m_wii_down, m_wii_left, m_wii_right};
  int wii_button_row = 0;
  int wii_button_col = 0;
  for (QCheckBox* box : wii_checks)
  {
    wii_button_layout->addWidget(box, wii_button_row, wii_button_col);
    if (++wii_button_col == 4)
    {
      wii_button_col = 0;
      ++wii_button_row;
    }
    connect(box, &QCheckBox::toggled, this, [this] { ApplyEditorChanges(); });
  }
  wii_layout->addWidget(wii_button_group);

  auto* accel_group = new QGroupBox(tr("Wii Remote Accelerometer"), wii_page);
  auto* accel_layout = new QGridLayout(accel_group);
  m_wii_accel_x = new QSpinBox(accel_group);
  m_wii_accel_y = new QSpinBox(accel_group);
  m_wii_accel_z = new QSpinBox(accel_group);
  for (QSpinBox* box : {m_wii_accel_x, m_wii_accel_y, m_wii_accel_z})
    box->setRange(0, 1023);
  accel_layout->addWidget(new QLabel(tr("X"), accel_group), 0, 0);
  accel_layout->addWidget(m_wii_accel_x, 0, 1);
  accel_layout->addWidget(new QLabel(tr("Y"), accel_group), 1, 0);
  accel_layout->addWidget(m_wii_accel_y, 1, 1);
  accel_layout->addWidget(new QLabel(tr("Z"), accel_group), 2, 0);
  accel_layout->addWidget(m_wii_accel_z, 2, 1);
  wii_layout->addWidget(accel_group);

  auto* gyro_group = new QGroupBox(tr("MotionPlus Gyroscope"), wii_page);
  auto* gyro_layout = new QGridLayout(gyro_group);
  m_wii_gyro_x = new QSpinBox(gyro_group);
  m_wii_gyro_y = new QSpinBox(gyro_group);
  m_wii_gyro_z = new QSpinBox(gyro_group);
  m_wii_gyro_slow_x = new QCheckBox(tr("Slow X"), gyro_group);
  m_wii_gyro_slow_y = new QCheckBox(tr("Slow Y"), gyro_group);
  m_wii_gyro_slow_z = new QCheckBox(tr("Slow Z"), gyro_group);
  for (QSpinBox* box : {m_wii_gyro_x, m_wii_gyro_y, m_wii_gyro_z})
    box->setRange(k_gyro_min, k_gyro_max);
  gyro_layout->addWidget(new QLabel(tr("X"), gyro_group), 0, 0);
  gyro_layout->addWidget(m_wii_gyro_x, 0, 1);
  gyro_layout->addWidget(m_wii_gyro_slow_x, 0, 2);
  gyro_layout->addWidget(new QLabel(tr("Y"), gyro_group), 1, 0);
  gyro_layout->addWidget(m_wii_gyro_y, 1, 1);
  gyro_layout->addWidget(m_wii_gyro_slow_y, 1, 2);
  gyro_layout->addWidget(new QLabel(tr("Z"), gyro_group), 2, 0);
  gyro_layout->addWidget(m_wii_gyro_z, 2, 1);
  gyro_layout->addWidget(m_wii_gyro_slow_z, 2, 2);
  wii_layout->addWidget(gyro_group);

  auto* nunchuk_group = new QGroupBox(tr("Nunchuk"), wii_page);
  auto* nunchuk_layout = new QGridLayout(nunchuk_group);
  m_wii_c = new QCheckBox(tr("C"), nunchuk_group);
  m_wii_z = new QCheckBox(tr("Z"), nunchuk_group);
  m_wii_nunchuk_x = new QSpinBox(nunchuk_group);
  m_wii_nunchuk_y = new QSpinBox(nunchuk_group);
  m_wii_nunchuk_accel_x = new QSpinBox(nunchuk_group);
  m_wii_nunchuk_accel_y = new QSpinBox(nunchuk_group);
  m_wii_nunchuk_accel_z = new QSpinBox(nunchuk_group);
  m_wii_nunchuk_x->setRange(0, 255);
  m_wii_nunchuk_y->setRange(0, 255);
  for (QSpinBox* box : {m_wii_nunchuk_accel_x, m_wii_nunchuk_accel_y, m_wii_nunchuk_accel_z})
    box->setRange(0, 1023);
  nunchuk_layout->addWidget(m_wii_c, 0, 0);
  nunchuk_layout->addWidget(m_wii_z, 0, 1);
  nunchuk_layout->addWidget(new QLabel(tr("Stick X"), nunchuk_group), 1, 0);
  nunchuk_layout->addWidget(m_wii_nunchuk_x, 1, 1);
  nunchuk_layout->addWidget(new QLabel(tr("Stick Y"), nunchuk_group), 1, 2);
  nunchuk_layout->addWidget(m_wii_nunchuk_y, 1, 3);
  nunchuk_layout->addWidget(new QLabel(tr("Accel X"), nunchuk_group), 2, 0);
  nunchuk_layout->addWidget(m_wii_nunchuk_accel_x, 2, 1);
  nunchuk_layout->addWidget(new QLabel(tr("Accel Y"), nunchuk_group), 2, 2);
  nunchuk_layout->addWidget(m_wii_nunchuk_accel_y, 2, 3);
  nunchuk_layout->addWidget(new QLabel(tr("Accel Z"), nunchuk_group), 3, 0);
  nunchuk_layout->addWidget(m_wii_nunchuk_accel_z, 3, 1);
  wii_layout->addWidget(nunchuk_group);
  wii_layout->addStretch();

  for (QCheckBox* box : {m_wii_reset, m_wii_c, m_wii_z, m_wii_gyro_slow_x, m_wii_gyro_slow_y,
                         m_wii_gyro_slow_z})
    connect(box, &QCheckBox::toggled, this, [this] { ApplyEditorChanges(); });
  for (QSpinBox* box : {m_wii_accel_x, m_wii_accel_y, m_wii_accel_z, m_wii_battery, m_wii_gyro_x,
                        m_wii_gyro_y, m_wii_gyro_z, m_wii_nunchuk_x, m_wii_nunchuk_y,
                        m_wii_nunchuk_accel_x, m_wii_nunchuk_accel_y, m_wii_nunchuk_accel_z})
  {
    connect(box, qOverload<int>(&QSpinBox::valueChanged), this, [this] { ApplyEditorChanges(); });
  }
  m_editor_stack->addWidget(wii_page);

  auto* editor_layout = new QVBoxLayout(m_editor_box);
  auto* editor_scroll = new QScrollArea(m_editor_box);
  editor_scroll->setWidgetResizable(true);
  editor_scroll->setFrameShape(QFrame::NoFrame);
  editor_scroll->setWidget(m_editor_stack);
  editor_layout->addWidget(editor_scroll);

  m_status_label = new QLabel(this);

  m_editor_box->setMinimumWidth(460);
  m_editor_box->setMaximumWidth(560);

  auto* center_layout = new QHBoxLayout;
  center_layout->addWidget(m_table, 3);
  center_layout->addWidget(m_editor_box, 2);

  auto* main_layout = new QVBoxLayout(this);
  main_layout->addLayout(button_layout);
  main_layout->addLayout(center_layout, 1);
  main_layout->addWidget(m_status_label);
  setLayout(main_layout);

  SetEditorEnabled(false);
  UpdateStatusLabel();
}

void DTMEditorDialog::Refresh()
{
  if (m_refresh_in_progress)
    return;

  QWidget* modal = QApplication::activeModalWidget();
  if (modal && modal != this && !isAncestorOf(modal))
    return;

  const QScopedValueRollback refresh_guard(m_refresh_in_progress, true);

  if (RefreshGCRuntimeMovie())
    return;
  if (RefreshWiiRuntimeMovie())
    return;

  if (m_using_runtime_movie)
  {
    m_using_runtime_movie = false;
    m_runtime_movie_kind = EditorMovieKind::None;
    m_last_runtime_row = -1;
    m_has_last_runtime_frame = false;
    if (m_file_movie_kind == EditorMovieKind::None)
      m_model->Clear();
    else
      m_model->SetHighlightedRow(-1);
  }

  UpdateStatusLabel();
  PopulateEditor();
}

bool DTMEditorDialog::RefreshGCRuntimeMovie()
{
  auto& movie = Core::System::GetInstance().GetMovie();
  const auto snapshot = movie.GetGCRuntimeFrameSnapshot();
  if (!snapshot.has_value())
    return false;

  const Core::State core_state = Core::GetState(Core::System::GetInstance());
  const bool paused = core_state == Core::State::Paused;
  const bool frame_changed =
      !m_has_last_runtime_frame || snapshot->current_frame != m_last_runtime_frame;
  const bool keep_manual_view =
      paused && !frame_changed && isActiveWindow() && m_using_runtime_movie &&
      m_runtime_movie_kind == EditorMovieKind::GC;
  const bool should_follow = !paused || frame_changed || !isActiveWindow();
  const std::vector<int> previously_selected_rows = GetSelectedRows();
  const QModelIndex current = m_table->currentIndex();
  const int previous_column = current.isValid() ? current.column() : m_model->GetFirstGCControllerColumn();
  const int previous_row = current.isValid() ? current.row() : -1;

  const auto row_game_frames = PopulateMissingGameFrames(snapshot->row_game_frames, snapshot->rows.size(),
                                                         snapshot->current_frame,
                                                         snapshot->current_input_row);
  if (!keep_manual_view)
    m_model->SetGCMovieData(snapshot->active_controllers, snapshot->rows, row_game_frames);
  m_model->SetHighlightedRow(static_cast<int>(snapshot->current_input_row));
  m_using_runtime_movie = true;
  m_runtime_movie_kind = EditorMovieKind::GC;

  if (!keep_manual_view)
  {
    int target_column = previous_column;
    int controller = -1;
    if (!m_model->IsGCControllerColumn(target_column, &controller))
      target_column = m_model->GetFirstGCControllerColumn();

    if (m_model->rowCount() > 0 && target_column >= 0)
    {
      QItemSelectionModel* selection_model = m_table->selectionModel();
      QSignalBlocker blocker(selection_model);

      if (should_follow)
      {
        const int live_row =
            std::clamp(static_cast<int>(snapshot->current_input_row), 0, m_model->rowCount() - 1);
        const QModelIndex idx = m_model->index(live_row, target_column);
        selection_model->clearSelection();
        selection_model->setCurrentIndex(idx, QItemSelectionModel::NoUpdate);
        selection_model->select(idx, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        m_table->scrollTo(idx, QAbstractItemView::PositionAtCenter);
      }
      else
      {
        const int restored_row =
            previous_row >= 0 ? std::clamp(previous_row, 0, m_model->rowCount() - 1) : 0;
        const QModelIndex current_idx = m_model->index(restored_row, target_column);
        selection_model->clearSelection();
        selection_model->setCurrentIndex(current_idx, QItemSelectionModel::NoUpdate);
        for (const int row : previously_selected_rows)
        {
          if (row < 0 || row >= m_model->rowCount())
            continue;
          selection_model->select(m_model->index(row, 0),
                                  QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
      }
    }
  }

  m_last_runtime_row = static_cast<int>(snapshot->current_input_row);
  m_last_runtime_frame = snapshot->current_frame;
  m_has_last_runtime_frame = true;
  UpdateStatusLabel();
  PopulateEditor();
  return true;
}

bool DTMEditorDialog::RefreshWiiRuntimeMovie()
{
  auto& movie = Core::System::GetInstance().GetMovie();
  const auto snapshot = movie.GetWiiRuntimeFrameSnapshot();
  if (!snapshot.has_value())
    return false;

  const Core::State core_state = Core::GetState(Core::System::GetInstance());
  const bool paused = core_state == Core::State::Paused;
  const bool frame_changed =
      !m_has_last_runtime_frame || snapshot->current_frame != m_last_runtime_frame;
  const bool keep_manual_view =
      paused && !frame_changed && isActiveWindow() && m_using_runtime_movie &&
      m_runtime_movie_kind == EditorMovieKind::Wii;
  const bool should_follow = !paused || frame_changed || !isActiveWindow();
  const std::vector<int> previously_selected_rows = GetSelectedRows();
  const QModelIndex current = m_table->currentIndex();
  const int previous_row = current.isValid() ? current.row() : -1;
  const auto row_game_frames = PopulateMissingGameFrames(snapshot->row_game_frames, snapshot->rows.size(),
                                                         snapshot->current_frame,
                                                         snapshot->current_input_row);
  if (!keep_manual_view)
    m_model->SetWiiMovieData(snapshot->active_wiimotes, snapshot->rows, row_game_frames);
  m_model->SetHighlightedRow(static_cast<int>(snapshot->current_input_row));
  m_using_runtime_movie = true;
  m_runtime_movie_kind = EditorMovieKind::Wii;

  if (!keep_manual_view && m_model->rowCount() > 0)
  {
    QItemSelectionModel* selection_model = m_table->selectionModel();
    QSignalBlocker blocker(selection_model);

    if (should_follow)
    {
      const int live_row =
          std::clamp(static_cast<int>(snapshot->current_input_row), 0, m_model->rowCount() - 1);
      const QModelIndex idx = m_model->index(live_row, 1);
      selection_model->clearSelection();
      selection_model->setCurrentIndex(idx, QItemSelectionModel::NoUpdate);
      selection_model->select(idx, QItemSelectionModel::Select | QItemSelectionModel::Rows);
      m_table->scrollTo(idx, QAbstractItemView::PositionAtCenter);
    }
    else
    {
      const int restored_row = previous_row >= 0 ? std::clamp(previous_row, 0, m_model->rowCount() - 1) : 0;
      const QModelIndex current_idx = m_model->index(restored_row, 1);
      selection_model->clearSelection();
      selection_model->setCurrentIndex(current_idx, QItemSelectionModel::NoUpdate);
      for (const int row : previously_selected_rows)
      {
        if (row < 0 || row >= m_model->rowCount())
          continue;
        selection_model->select(m_model->index(row, 0),
                                QItemSelectionModel::Select | QItemSelectionModel::Rows);
      }
    }
  }

  m_last_runtime_row = static_cast<int>(snapshot->current_input_row);
  m_last_runtime_frame = snapshot->current_frame;
  m_has_last_runtime_frame = true;
  UpdateStatusLabel();
  PopulateEditor();
  return true;
}

void DTMEditorDialog::PopulateEditor()
{
  const QModelIndex current = m_table->currentIndex();
  if (!current.isValid() || m_model->GetKind() == ModelMovieKind::None)
  {
    m_editor_stack->setCurrentIndex(k_empty_page);
    m_editor_box->setTitle(tr("Selected Frame"));
    SetEditorEnabled(false);
    return;
  }

  if (m_model->GetKind() == ModelMovieKind::GC)
  {
    int controller = -1;
    if (!m_model->IsGCControllerColumn(current.column(), &controller))
    {
      controller = 0;
      while (controller < 4)
      {
        int dummy = -1;
        if (m_model->IsGCControllerColumn(m_model->GetFirstGCControllerColumn(), &dummy))
        {
          controller = dummy;
          break;
        }
        ++controller;
      }
    }
    PopulateGCEditor(current.row(), controller);
    return;
  }

  if (m_model->GetKind() == ModelMovieKind::Wii)
  {
    PopulateWiiEditor(current.row());
    return;
  }

  m_editor_stack->setCurrentIndex(k_empty_page);
  SetEditorEnabled(false);
}

void DTMEditorDialog::PopulateGCEditor(int row, int controller)
{
  m_updating_editor = true;
  const auto& state = m_model->GetGCState(row, controller);
  m_connected->setChecked(state.is_connected);
  m_start->setChecked(state.Start);
  m_a->setChecked(state.A);
  m_b->setChecked(state.B);
  m_x->setChecked(state.X);
  m_y->setChecked(state.Y);
  m_z->setChecked(state.Z);
  m_l->setChecked(state.L);
  m_r->setChecked(state.R);
  m_up->setChecked(state.DPadUp);
  m_down->setChecked(state.DPadDown);
  m_left->setChecked(state.DPadLeft);
  m_right->setChecked(state.DPadRight);
  m_disc->setChecked(state.disc);
  m_reset->setChecked(state.reset);
  m_get_origin->setChecked(state.get_origin);
  m_trigger_l->setValue(state.TriggerL);
  m_trigger_r->setValue(state.TriggerR);
  m_stick_x->setValue(state.AnalogStickX);
  m_stick_y->setValue(state.AnalogStickY);
  m_cstick_x->setValue(state.CStickX);
  m_cstick_y->setValue(state.CStickY);
  m_updating_editor = false;

  m_editor_stack->setCurrentIndex(k_gc_page);
  m_editor_box->setTitle(tr("GameCube Frame %1, Port %2").arg(row).arg(controller + 1));
  SetEditorEnabled(true);
}

void DTMEditorDialog::PopulateWiiEditor(int row)
{
  m_current_wii_row = m_model->GetWiiRow(row);
  m_current_wii_state_valid = false;

  m_editor_stack->setCurrentIndex(k_wii_page);
  m_updating_editor = true;
  m_wii_reset->setChecked(m_current_wii_row.is_reset);
  if (m_current_wii_row.is_reset)
  {
    std::optional<Movie::WiiRuntimeInputRow> template_row;
    for (int offset = 1; offset < m_model->rowCount(); ++offset)
    {
      for (const int candidate : {row - offset, row + offset})
      {
        if (candidate < 0 || candidate >= m_model->rowCount())
          continue;

        const Movie::WiiRuntimeInputRow candidate_row = m_model->GetWiiRow(candidate);
        if (candidate_row.is_reset)
          continue;

        if (WiimoteEmu::DeserializeDesiredState(&m_current_wii_state, candidate_row.serialized_state))
        {
          template_row = candidate_row;
          break;
        }
      }

      if (template_row.has_value())
        break;
    }

    if (!template_row.has_value())
    {
      m_current_wii_state.~DesiredWiimoteState();
      new (&m_current_wii_state) WiimoteEmu::DesiredWiimoteState();
    }
    m_current_wii_state.buttons.hex = 0;
    m_current_wii_state.acceleration.value.x = 512;
    m_current_wii_state.acceleration.value.y = 512;
    m_current_wii_state.acceleration.value.z = 616;
    m_current_wii_state.battery = 100;
    if (m_current_wii_state.motion_plus.has_value())
    {
      m_current_wii_state.motion_plus->gyro.value.x = WiimoteEmu::MotionPlus::ZERO_VALUE;
      m_current_wii_state.motion_plus->gyro.value.y = WiimoteEmu::MotionPlus::ZERO_VALUE;
      m_current_wii_state.motion_plus->gyro.value.z = WiimoteEmu::MotionPlus::ZERO_VALUE;
      m_current_wii_state.motion_plus->is_slow = {};
    }
    if (std::holds_alternative<WiimoteEmu::Nunchuk::DataFormat>(m_current_wii_state.extension.data))
    {
      auto nunchuk = std::get<WiimoteEmu::Nunchuk::DataFormat>(m_current_wii_state.extension.data);
      nunchuk.jx = WiimoteEmu::Nunchuk::STICK_CENTER;
      nunchuk.jy = WiimoteEmu::Nunchuk::STICK_CENTER;
      nunchuk.SetButtons(0);
      nunchuk.SetAccel(WiimoteEmu::Nunchuk::DataFormat::AccelType(k_nunchuk_accel_zero,
                                                                  k_nunchuk_accel_zero,
                                                                  k_nunchuk_accel_one));
      m_current_wii_state.extension.data = nunchuk;
    }

    m_current_wii_state_valid = true;
    m_wii_extension_label->setText(std::holds_alternative<WiimoteEmu::Nunchuk::DataFormat>(
                                       m_current_wii_state.extension.data) ?
                                       tr("Nunchuk") :
                                       tr("None / Other"));
    m_wii_motion_plus_label->setText(m_current_wii_state.motion_plus.has_value() ? tr("Yes") :
                                                                          tr("No"));
    m_wii_a->setChecked(false);
    m_wii_b->setChecked(false);
    m_wii_one->setChecked(false);
    m_wii_two->setChecked(false);
    m_wii_plus->setChecked(false);
    m_wii_minus->setChecked(false);
    m_wii_home->setChecked(false);
    m_wii_up->setChecked(false);
    m_wii_down->setChecked(false);
    m_wii_left->setChecked(false);
    m_wii_right->setChecked(false);
    m_wii_accel_x->setValue(m_current_wii_state.acceleration.value.x);
    m_wii_accel_y->setValue(m_current_wii_state.acceleration.value.y);
    m_wii_accel_z->setValue(m_current_wii_state.acceleration.value.z);
    m_wii_battery->setValue(m_current_wii_state.battery.value_or(100));
    m_wii_gyro_x->setValue(k_gyro_center);
    m_wii_gyro_y->setValue(k_gyro_center);
    m_wii_gyro_z->setValue(k_gyro_center);
    m_wii_gyro_slow_x->setChecked(false);
    m_wii_gyro_slow_y->setChecked(false);
    m_wii_gyro_slow_z->setChecked(false);
    m_wii_c->setChecked(false);
    m_wii_z->setChecked(false);
    m_wii_nunchuk_x->setValue(WiimoteEmu::Nunchuk::STICK_CENTER);
    m_wii_nunchuk_y->setValue(WiimoteEmu::Nunchuk::STICK_CENTER);
    m_wii_nunchuk_accel_x->setValue(k_nunchuk_accel_zero);
    m_wii_nunchuk_accel_y->setValue(k_nunchuk_accel_zero);
    m_wii_nunchuk_accel_z->setValue(k_nunchuk_accel_one);
    m_updating_editor = false;
    const bool has_nunchuk = std::holds_alternative<WiimoteEmu::Nunchuk::DataFormat>(
        m_current_wii_state.extension.data);
    const bool has_motion_plus = m_current_wii_state.motion_plus.has_value();
    for (QWidget* widget : {static_cast<QWidget*>(m_wii_gyro_x), static_cast<QWidget*>(m_wii_gyro_y),
                            static_cast<QWidget*>(m_wii_gyro_z),
                            static_cast<QWidget*>(m_wii_gyro_slow_x),
                            static_cast<QWidget*>(m_wii_gyro_slow_y),
                            static_cast<QWidget*>(m_wii_gyro_slow_z)})
    {
      widget->setEnabled(has_motion_plus);
    }
    for (QWidget* widget : {static_cast<QWidget*>(m_wii_c), static_cast<QWidget*>(m_wii_z),
                            static_cast<QWidget*>(m_wii_nunchuk_x),
                            static_cast<QWidget*>(m_wii_nunchuk_y),
                            static_cast<QWidget*>(m_wii_nunchuk_accel_x),
                            static_cast<QWidget*>(m_wii_nunchuk_accel_y),
                            static_cast<QWidget*>(m_wii_nunchuk_accel_z)})
    {
      widget->setEnabled(has_nunchuk);
    }
    m_editor_box->setTitle(tr("Wii Frame %1 (Reset Marker)").arg(row));
    SetEditorEnabled(true);
    return;
  }

  if (!WiimoteEmu::DeserializeDesiredState(&m_current_wii_state, m_current_wii_row.serialized_state))
  {
    m_wii_extension_label->setText(tr("Invalid"));
    m_wii_motion_plus_label->setText(tr("Invalid"));
    m_updating_editor = false;
    m_editor_box->setTitle(tr("Wii Frame %1 (Invalid)").arg(row));
    SetEditorEnabled(false);
    return;
  }

  m_current_wii_state_valid = true;
  const bool has_nunchuk = std::holds_alternative<WiimoteEmu::Nunchuk::DataFormat>(m_current_wii_state.extension.data);
  const bool has_motion_plus = m_current_wii_state.motion_plus.has_value();
  m_wii_extension_label->setText(has_nunchuk ? tr("Nunchuk") : tr("None / Other"));
  m_wii_motion_plus_label->setText(has_motion_plus ? tr("Yes") : tr("No"));

  m_updating_editor = true;
  m_wii_a->setChecked(m_current_wii_state.buttons.a);
  m_wii_b->setChecked(m_current_wii_state.buttons.b);
  m_wii_one->setChecked(m_current_wii_state.buttons.one);
  m_wii_two->setChecked(m_current_wii_state.buttons.two);
  m_wii_plus->setChecked(m_current_wii_state.buttons.plus);
  m_wii_minus->setChecked(m_current_wii_state.buttons.minus);
  m_wii_home->setChecked(m_current_wii_state.buttons.home);
  m_wii_up->setChecked(m_current_wii_state.buttons.up);
  m_wii_down->setChecked(m_current_wii_state.buttons.down);
  m_wii_left->setChecked(m_current_wii_state.buttons.left);
  m_wii_right->setChecked(m_current_wii_state.buttons.right);
  m_wii_accel_x->setValue(m_current_wii_state.acceleration.value.x);
  m_wii_accel_y->setValue(m_current_wii_state.acceleration.value.y);
  m_wii_accel_z->setValue(m_current_wii_state.acceleration.value.z);
  m_wii_battery->setValue(m_current_wii_state.battery.value_or(100));

  if (has_motion_plus)
  {
    const auto& gyro = *m_current_wii_state.motion_plus;
    m_wii_gyro_x->setValue(GyroRawToTasValue(gyro.gyro.value.x));
    m_wii_gyro_y->setValue(GyroRawToTasValue(gyro.gyro.value.y));
    m_wii_gyro_z->setValue(GyroRawToTasValue(gyro.gyro.value.z));
    m_wii_gyro_slow_x->setChecked(gyro.is_slow.x);
    m_wii_gyro_slow_y->setChecked(gyro.is_slow.y);
    m_wii_gyro_slow_z->setChecked(gyro.is_slow.z);
  }
  else
  {
    m_wii_gyro_x->setValue(k_gyro_center);
    m_wii_gyro_y->setValue(k_gyro_center);
    m_wii_gyro_z->setValue(k_gyro_center);
    m_wii_gyro_slow_x->setChecked(false);
    m_wii_gyro_slow_y->setChecked(false);
    m_wii_gyro_slow_z->setChecked(false);
  }

  if (has_nunchuk)
  {
    const auto& nunchuk = std::get<WiimoteEmu::Nunchuk::DataFormat>(m_current_wii_state.extension.data);
    const u8 buttons = nunchuk.GetButtons();
    m_wii_c->setChecked((buttons & WiimoteEmu::Nunchuk::BUTTON_C) != 0);
    m_wii_z->setChecked((buttons & WiimoteEmu::Nunchuk::BUTTON_Z) != 0);
    m_wii_nunchuk_x->setValue(nunchuk.GetStick().value.x);
    m_wii_nunchuk_y->setValue(nunchuk.GetStick().value.y);
    m_wii_nunchuk_accel_x->setValue(nunchuk.GetAccel().value.x);
    m_wii_nunchuk_accel_y->setValue(nunchuk.GetAccel().value.y);
    m_wii_nunchuk_accel_z->setValue(nunchuk.GetAccel().value.z);
  }
  else
  {
    m_wii_c->setChecked(false);
    m_wii_z->setChecked(false);
    m_wii_nunchuk_x->setValue(WiimoteEmu::Nunchuk::STICK_CENTER);
    m_wii_nunchuk_y->setValue(WiimoteEmu::Nunchuk::STICK_CENTER);
    m_wii_nunchuk_accel_x->setValue(k_nunchuk_accel_zero);
    m_wii_nunchuk_accel_y->setValue(k_nunchuk_accel_zero);
    m_wii_nunchuk_accel_z->setValue(k_nunchuk_accel_one);
  }
  m_updating_editor = false;

  for (QWidget* widget : {static_cast<QWidget*>(m_wii_gyro_x), static_cast<QWidget*>(m_wii_gyro_y),
                          static_cast<QWidget*>(m_wii_gyro_z), static_cast<QWidget*>(m_wii_gyro_slow_x),
                          static_cast<QWidget*>(m_wii_gyro_slow_y), static_cast<QWidget*>(m_wii_gyro_slow_z)})
  {
    widget->setEnabled(has_motion_plus);
  }
  for (QWidget* widget : {static_cast<QWidget*>(m_wii_c), static_cast<QWidget*>(m_wii_z),
                          static_cast<QWidget*>(m_wii_nunchuk_x), static_cast<QWidget*>(m_wii_nunchuk_y),
                          static_cast<QWidget*>(m_wii_nunchuk_accel_x),
                          static_cast<QWidget*>(m_wii_nunchuk_accel_y),
                          static_cast<QWidget*>(m_wii_nunchuk_accel_z)})
  {
    widget->setEnabled(has_nunchuk);
  }

  m_editor_box->setTitle(tr("Wii Frame %1").arg(row));
  SetEditorEnabled(true);
}

std::vector<int> DTMEditorDialog::GetSelectedRows() const
{
  std::vector<int> rows;
  if (const auto* selection_model = m_table->selectionModel())
  {
    const auto selected = selection_model->selectedRows(0);
    rows.reserve(selected.size());
    for (const QModelIndex& index : selected)
      rows.push_back(index.row());
  }

  if (const QModelIndex current = m_table->currentIndex(); current.isValid())
    rows.push_back(current.row());

  std::sort(rows.begin(), rows.end());
  rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
  return rows;
}

void DTMEditorDialog::ApplyGCEditorChange(Movie::ControllerState* state, const QObject* source) const
{
  if (!source)
  {
    *state = BuildGCStateFromEditor();
    return;
  }

  if (source == m_connected)
    state->is_connected = m_connected->isChecked();
  else if (source == m_start)
    state->Start = m_start->isChecked();
  else if (source == m_a)
    state->A = m_a->isChecked();
  else if (source == m_b)
    state->B = m_b->isChecked();
  else if (source == m_x)
    state->X = m_x->isChecked();
  else if (source == m_y)
    state->Y = m_y->isChecked();
  else if (source == m_z)
    state->Z = m_z->isChecked();
  else if (source == m_l)
    state->L = m_l->isChecked();
  else if (source == m_r)
    state->R = m_r->isChecked();
  else if (source == m_up)
    state->DPadUp = m_up->isChecked();
  else if (source == m_down)
    state->DPadDown = m_down->isChecked();
  else if (source == m_left)
    state->DPadLeft = m_left->isChecked();
  else if (source == m_right)
    state->DPadRight = m_right->isChecked();
  else if (source == m_disc)
    state->disc = m_disc->isChecked();
  else if (source == m_reset)
    state->reset = m_reset->isChecked();
  else if (source == m_get_origin)
    state->get_origin = m_get_origin->isChecked();
  else if (source == m_trigger_l)
    state->TriggerL = static_cast<u8>(m_trigger_l->value());
  else if (source == m_trigger_r)
    state->TriggerR = static_cast<u8>(m_trigger_r->value());
  else if (source == m_stick_x)
    state->AnalogStickX = static_cast<u8>(m_stick_x->value());
  else if (source == m_stick_y)
    state->AnalogStickY = static_cast<u8>(m_stick_y->value());
  else if (source == m_cstick_x)
    state->CStickX = static_cast<u8>(m_cstick_x->value());
  else if (source == m_cstick_y)
    state->CStickY = static_cast<u8>(m_cstick_y->value());
}

void DTMEditorDialog::ApplyWiiEditorChange(Movie::WiiRuntimeInputRow* row, const QObject* source) const
{
  if (source == m_wii_reset)
  {
    row->is_reset = m_wii_reset->isChecked();
    if (row->is_reset)
    {
      row->serialized_state = {};
    }
    else
    {
      row->serialized_state = BuildWiiSerializedStateFromEditor();
    }
    return;
  }

  if (row->is_reset)
    return;

  WiimoteEmu::DesiredWiimoteState state;
  if (!WiimoteEmu::DeserializeDesiredState(&state, row->serialized_state))
    return;

  if (!source)
  {
    row->serialized_state = BuildWiiSerializedStateFromEditor();
    return;
  }

  if (source == m_wii_a)
    state.buttons.a = m_wii_a->isChecked();
  else if (source == m_wii_b)
    state.buttons.b = m_wii_b->isChecked();
  else if (source == m_wii_one)
    state.buttons.one = m_wii_one->isChecked();
  else if (source == m_wii_two)
    state.buttons.two = m_wii_two->isChecked();
  else if (source == m_wii_plus)
    state.buttons.plus = m_wii_plus->isChecked();
  else if (source == m_wii_minus)
    state.buttons.minus = m_wii_minus->isChecked();
  else if (source == m_wii_home)
    state.buttons.home = m_wii_home->isChecked();
  else if (source == m_wii_up)
    state.buttons.up = m_wii_up->isChecked();
  else if (source == m_wii_down)
    state.buttons.down = m_wii_down->isChecked();
  else if (source == m_wii_left)
    state.buttons.left = m_wii_left->isChecked();
  else if (source == m_wii_right)
    state.buttons.right = m_wii_right->isChecked();
  else if (source == m_wii_accel_x)
    state.acceleration.value.x = static_cast<u16>(m_wii_accel_x->value());
  else if (source == m_wii_accel_y)
    state.acceleration.value.y = static_cast<u16>(m_wii_accel_y->value());
  else if (source == m_wii_accel_z)
    state.acceleration.value.z = static_cast<u16>(m_wii_accel_z->value());
  else if (source == m_wii_battery)
    state.battery = static_cast<u8>(m_wii_battery->value());
  else if (state.motion_plus.has_value() &&
           (source == m_wii_gyro_x || source == m_wii_gyro_y || source == m_wii_gyro_z ||
            source == m_wii_gyro_slow_x || source == m_wii_gyro_slow_y || source == m_wii_gyro_slow_z))
  {
    state.motion_plus = WiimoteEmu::MotionPlus::DataFormat::Data{
        WiimoteEmu::MotionPlus::DataFormat::GyroRawValue{
            WiimoteEmu::MotionPlus::DataFormat::GyroType(
                source == m_wii_gyro_x ? TasGyroToRawValue(m_wii_gyro_x->value())
                                       : state.motion_plus->gyro.value.x,
                source == m_wii_gyro_y ? TasGyroToRawValue(m_wii_gyro_y->value())
                                       : state.motion_plus->gyro.value.y,
                source == m_wii_gyro_z ? TasGyroToRawValue(m_wii_gyro_z->value())
                                       : state.motion_plus->gyro.value.z)},
        WiimoteEmu::MotionPlus::DataFormat::SlowType(
            source == m_wii_gyro_slow_x ? m_wii_gyro_slow_x->isChecked()
                                        : state.motion_plus->is_slow.x,
            source == m_wii_gyro_slow_y ? m_wii_gyro_slow_y->isChecked()
                                        : state.motion_plus->is_slow.y,
            source == m_wii_gyro_slow_z ? m_wii_gyro_slow_z->isChecked()
                                        : state.motion_plus->is_slow.z)};
  }
  else if (std::holds_alternative<WiimoteEmu::Nunchuk::DataFormat>(state.extension.data))
  {
    auto nunchuk = std::get<WiimoteEmu::Nunchuk::DataFormat>(state.extension.data);
    u8 buttons = nunchuk.GetButtons();

    if (source == m_wii_c)
    {
      if (m_wii_c->isChecked())
        buttons |= WiimoteEmu::Nunchuk::BUTTON_C;
      else
        buttons &= ~WiimoteEmu::Nunchuk::BUTTON_C;
      nunchuk.SetButtons(buttons);
    }
    else if (source == m_wii_z)
    {
      if (m_wii_z->isChecked())
        buttons |= WiimoteEmu::Nunchuk::BUTTON_Z;
      else
        buttons &= ~WiimoteEmu::Nunchuk::BUTTON_Z;
      nunchuk.SetButtons(buttons);
    }
    else if (source == m_wii_nunchuk_x)
    {
      nunchuk.jx = static_cast<u8>(m_wii_nunchuk_x->value());
    }
    else if (source == m_wii_nunchuk_y)
    {
      nunchuk.jy = static_cast<u8>(m_wii_nunchuk_y->value());
    }
    else if (source == m_wii_nunchuk_accel_x)
    {
      nunchuk.SetAccelX(static_cast<u16>(m_wii_nunchuk_accel_x->value()));
    }
    else if (source == m_wii_nunchuk_accel_y)
    {
      nunchuk.SetAccelY(static_cast<u16>(m_wii_nunchuk_accel_y->value()));
    }
    else if (source == m_wii_nunchuk_accel_z)
    {
      nunchuk.SetAccelZ(static_cast<u16>(m_wii_nunchuk_accel_z->value()));
    }

    state.extension.data = nunchuk;
  }

  row->serialized_state = WiimoteEmu::SerializeDesiredState(state);
}

void DTMEditorDialog::ApplyEditorChanges()
{
  if (m_updating_editor)
    return;

  const QModelIndex current = m_table->currentIndex();
  if (!current.isValid())
    return;

  const QObject* source = sender();
  const std::vector<int> selected_rows = GetSelectedRows();
  if (selected_rows.empty())
    return;

  if (m_model->GetKind() == ModelMovieKind::GC)
  {
    int controller = -1;
    if (!m_model->IsGCControllerColumn(current.column(), &controller))
      return;

    for (const int row_index : selected_rows)
    {
      Movie::ControllerState state = m_model->GetGCState(row_index, controller);
      ApplyGCEditorChange(&state, source);
      if (m_using_runtime_movie && m_runtime_movie_kind == EditorMovieKind::GC &&
          !Core::System::GetInstance().GetMovie().SetGCRuntimeFrameState(row_index, controller, state))
      {
        continue;
      }
      m_model->SetGCState(row_index, controller, state);
    }
    m_dirty = !m_using_runtime_movie;
  }
  else if (m_model->GetKind() == ModelMovieKind::Wii)
  {
    if (!m_current_wii_state_valid && !m_current_wii_row.is_reset)
      return;

    for (const int row_index : selected_rows)
    {
      Movie::WiiRuntimeInputRow row = m_model->GetWiiRow(row_index);
      ApplyWiiEditorChange(&row, source);
      if (m_using_runtime_movie && m_runtime_movie_kind == EditorMovieKind::Wii &&
          !Core::System::GetInstance().GetMovie().SetWiiRuntimeFrameState(
              row_index, row.is_reset ? WiimoteEmu::SerializedWiimoteState{} : row.serialized_state))
      {
        continue;
      }
      m_model->SetWiiRow(row_index, row);
      if (row_index == current.row())
        m_current_wii_row = row;
    }
    m_dirty = !m_using_runtime_movie;
  }

  UpdateStatusLabel();
  PopulateEditor();
}

void DTMEditorDialog::SetEditorEnabled(bool enabled)
{
  m_editor_box->setEnabled(enabled);
}

Movie::ControllerState DTMEditorDialog::BuildGCStateFromEditor() const
{
  Movie::ControllerState state{};
  state.is_connected = m_connected->isChecked();
  state.Start = m_start->isChecked();
  state.A = m_a->isChecked();
  state.B = m_b->isChecked();
  state.X = m_x->isChecked();
  state.Y = m_y->isChecked();
  state.Z = m_z->isChecked();
  state.L = m_l->isChecked();
  state.R = m_r->isChecked();
  state.DPadUp = m_up->isChecked();
  state.DPadDown = m_down->isChecked();
  state.DPadLeft = m_left->isChecked();
  state.DPadRight = m_right->isChecked();
  state.disc = m_disc->isChecked();
  state.reset = m_reset->isChecked();
  state.get_origin = m_get_origin->isChecked();
  state.TriggerL = static_cast<u8>(m_trigger_l->value());
  state.TriggerR = static_cast<u8>(m_trigger_r->value());
  state.AnalogStickX = static_cast<u8>(m_stick_x->value());
  state.AnalogStickY = static_cast<u8>(m_stick_y->value());
  state.CStickX = static_cast<u8>(m_cstick_x->value());
  state.CStickY = static_cast<u8>(m_cstick_y->value());
  return state;
}

WiimoteEmu::SerializedWiimoteState DTMEditorDialog::BuildWiiSerializedStateFromEditor() const
{
  WiimoteEmu::DesiredWiimoteState state = m_current_wii_state;
  state.buttons.hex = 0;
  state.buttons.a = m_wii_a->isChecked();
  state.buttons.b = m_wii_b->isChecked();
  state.buttons.one = m_wii_one->isChecked();
  state.buttons.two = m_wii_two->isChecked();
  state.buttons.plus = m_wii_plus->isChecked();
  state.buttons.minus = m_wii_minus->isChecked();
  state.buttons.home = m_wii_home->isChecked();
  state.buttons.up = m_wii_up->isChecked();
  state.buttons.down = m_wii_down->isChecked();
  state.buttons.left = m_wii_left->isChecked();
  state.buttons.right = m_wii_right->isChecked();
  state.acceleration.value.x = static_cast<u16>(m_wii_accel_x->value());
  state.acceleration.value.y = static_cast<u16>(m_wii_accel_y->value());
  state.acceleration.value.z = static_cast<u16>(m_wii_accel_z->value());
  state.battery = static_cast<u8>(m_wii_battery->value());

  if (state.motion_plus.has_value())
  {
    state.motion_plus = WiimoteEmu::MotionPlus::DataFormat::Data{
        WiimoteEmu::MotionPlus::DataFormat::GyroRawValue{
            WiimoteEmu::MotionPlus::DataFormat::GyroType(
                TasGyroToRawValue(m_wii_gyro_x->value()), TasGyroToRawValue(m_wii_gyro_y->value()),
                TasGyroToRawValue(m_wii_gyro_z->value()))},
        WiimoteEmu::MotionPlus::DataFormat::SlowType(m_wii_gyro_slow_x->isChecked(),
                                                     m_wii_gyro_slow_y->isChecked(),
                                                     m_wii_gyro_slow_z->isChecked())};
  }

  if (std::holds_alternative<WiimoteEmu::Nunchuk::DataFormat>(state.extension.data))
  {
    auto nunchuk = std::get<WiimoteEmu::Nunchuk::DataFormat>(state.extension.data);
    nunchuk.jx = static_cast<u8>(m_wii_nunchuk_x->value());
    nunchuk.jy = static_cast<u8>(m_wii_nunchuk_y->value());
    nunchuk.SetAccel(WiimoteEmu::Nunchuk::DataFormat::AccelType(
        static_cast<u16>(m_wii_nunchuk_accel_x->value()),
        static_cast<u16>(m_wii_nunchuk_accel_y->value()),
        static_cast<u16>(m_wii_nunchuk_accel_z->value())));
    u8 buttons = 0;
    if (m_wii_c->isChecked())
      buttons |= WiimoteEmu::Nunchuk::BUTTON_C;
    if (m_wii_z->isChecked())
      buttons |= WiimoteEmu::Nunchuk::BUTTON_Z;
    nunchuk.SetButtons(buttons);
    state.extension.data = nunchuk;
  }

  return WiimoteEmu::SerializeDesiredState(state);
}

bool DTMEditorDialog::LoadFile(const QString& path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
  {
    QMessageBox::warning(this, tr("DTM Editor"), tr("Failed to open DTM file."));
    return false;
  }

  const QByteArray bytes = file.readAll();
  if (const auto gc = ParseGCDTMFile(bytes))
  {
    m_header = gc->header;
    m_file_path = path;
    m_file_movie_kind = EditorMovieKind::GC;
    m_using_runtime_movie = false;
    m_runtime_movie_kind = EditorMovieKind::None;
    std::vector<std::optional<u64>> row_game_frames(gc->rows.size());
    for (size_t i = 0; i < row_game_frames.size(); ++i)
      row_game_frames[i] = static_cast<u64>(i);
    m_model->SetGCMovieData(gc->active_controllers, gc->rows, row_game_frames);
    m_model->SetHighlightedRow(-1);
    m_dirty = false;
    UpdateStatusLabel();
    PopulateEditor();
    return true;
  }

  if (const auto wii = ParseWiiDTMFile(bytes))
  {
    m_header = wii->header;
    m_file_path = path;
    m_file_movie_kind = EditorMovieKind::Wii;
    m_using_runtime_movie = false;
    m_runtime_movie_kind = EditorMovieKind::None;
    std::vector<std::optional<u64>> row_game_frames(wii->rows.size());
    for (size_t i = 0; i < row_game_frames.size(); ++i)
      row_game_frames[i] = static_cast<u64>(i);
    m_model->SetWiiMovieData(wii->active_wiimotes, wii->rows, row_game_frames);
    m_model->SetHighlightedRow(-1);
    m_dirty = false;
    UpdateStatusLabel();
    PopulateEditor();
    return true;
  }

  QMessageBox::warning(this, tr("DTM Editor"),
                       tr("This DTM format is not supported by the editor."));
  return false;
}

bool DTMEditorDialog::SaveFile(const QString& path)
{
  if (m_using_runtime_movie || m_file_movie_kind == EditorMovieKind::None)
    return false;

  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    QMessageBox::warning(this, tr("DTM Editor"), tr("Failed to save DTM file."));
    return false;
  }

  QByteArray bytes;
  bytes.append(reinterpret_cast<const char*>(&m_header), sizeof(m_header));

  if (m_file_movie_kind == EditorMovieKind::GC)
  {
    for (int row = 0; row < m_model->rowCount(); ++row)
    {
      for (int i = 0; i < 4; ++i)
      {
        int column = -1;
        for (int try_col = 1; try_col < m_model->columnCount(); ++try_col)
        {
          int mapped = -1;
          if (m_model->IsGCControllerColumn(try_col, &mapped) && mapped == i)
          {
            column = try_col;
            break;
          }
        }
        if (column < 0)
          continue;
        const auto state = m_model->GetGCState(row, i);
        bytes.append(reinterpret_cast<const char*>(&state), sizeof(state));
      }
    }
  }
  else if (m_file_movie_kind == EditorMovieKind::Wii)
  {
    for (int row = 0; row < m_model->rowCount(); ++row)
    {
      const auto state = m_model->GetWiiRow(row);
      bytes.append(static_cast<char>(state.serialized_state.length));
      bytes.append(reinterpret_cast<const char*>(state.serialized_state.data.data()),
                   state.serialized_state.length);
    }
  }

  if (file.write(bytes) != bytes.size())
  {
    QMessageBox::warning(this, tr("DTM Editor"), tr("Failed to write DTM file."));
    return false;
  }

  m_dirty = false;
  UpdateStatusLabel();
  return true;
}

void DTMEditorDialog::PasteInputs()
{
  if (m_model->GetKind() != ModelMovieKind::GC)
  {
    QMessageBox::information(this, tr("DTM Editor"),
                             tr("Paste Inputs currently supports GameCube DTM data only."));
    return;
  }

  const QString path = QFileDialog::getOpenFileName(this, tr("Paste Inputs From DTM"), QString(),
                                                    tr("Dolphin TAS Movies (*.dtm)"));
  if (path.isEmpty())
    return;

  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return;
  const auto parsed = ParseGCDTMFile(file.readAll());
  if (!parsed)
  {
    QMessageBox::warning(this, tr("DTM Editor"), tr("Failed to parse source DTM file."));
    return;
  }

  bool ok = false;
  const int src_start = QInputDialog::getInt(this, tr("Paste Inputs"), tr("Source start row"), 0, 0,
                                             std::max(0, static_cast<int>(parsed->rows.size()) - 1), 1, &ok);
  if (!ok)
    return;
  const int src_end = QInputDialog::getInt(this, tr("Paste Inputs"), tr("Source end row"), src_start,
                                           src_start,
                                           std::max(src_start, static_cast<int>(parsed->rows.size()) - 1), 1,
                                           &ok);
  if (!ok)
    return;
  const int dst_start = QInputDialog::getInt(this, tr("Paste Inputs"), tr("Destination start row"),
                                             std::max(0, m_table->currentIndex().row()), 0,
                                             std::max(0, m_model->rowCount() - 1), 1, &ok);
  if (!ok)
    return;

  const int rows_to_copy = src_end - src_start + 1;
  if (dst_start + rows_to_copy > m_model->rowCount())
  {
    QMessageBox::warning(this, tr("DTM Editor"), tr("Destination range exceeds current movie length."));
    return;
  }

  for (int i = 0; i < rows_to_copy; ++i)
  {
    const auto& src_row = parsed->rows[static_cast<size_t>(src_start + i)];
    for (int controller = 0; controller < 4; ++controller)
    {
      int dest_col = -1;
      for (int col = 1; col < m_model->columnCount(); ++col)
      {
        int mapped = -1;
        if (m_model->IsGCControllerColumn(col, &mapped) && mapped == controller)
          dest_col = col;
      }
      if (!parsed->active_controllers[controller] || dest_col < 0)
        continue;

      if (m_using_runtime_movie && m_runtime_movie_kind == EditorMovieKind::GC)
        Core::System::GetInstance().GetMovie().SetGCRuntimeFrameState(dst_start + i, controller,
                                                                      src_row[controller]);
      m_model->SetGCState(dst_start + i, controller, src_row[controller]);
    }
  }

  m_dirty = !m_using_runtime_movie;
  UpdateStatusLabel();
  PopulateEditor();
}

void DTMEditorDialog::UpdateStatusLabel()
{
  QString status;
  if (m_using_runtime_movie)
  {
    if (m_runtime_movie_kind == EditorMovieKind::GC)
      status = tr("Live GameCube movie");
    else if (m_runtime_movie_kind == EditorMovieKind::Wii)
      status = tr("Live Wii movie");
  }
  else if (!m_file_path.isEmpty())
  {
    status = tr("File: %1").arg(m_file_path);
  }
  else
  {
    status = tr("No movie loaded");
  }

  if (m_dirty)
    status += tr(" [modified]");

  m_status_label->setText(status);
  m_paste_button->setEnabled(m_model->GetKind() == ModelMovieKind::GC);
  m_save_button->setEnabled(m_model->GetKind() != ModelMovieKind::None);
  m_save_button->setText(m_using_runtime_movie ? tr("Export DTM") : tr("Save"));
}
