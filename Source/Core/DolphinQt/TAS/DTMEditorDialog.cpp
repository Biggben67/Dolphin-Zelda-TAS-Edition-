#include "DolphinQt/TAS/DTMEditorDialog.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <new>
#include <optional>
#include <ranges>
#include <tuple>
#include <variant>
#include <vector>

#include <QAction>
#include <QAbstractItemView>
#include <QAbstractTableModel>
#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QColor>
#include <QCursor>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QKeySequence>
#include <QMessageBox>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QRubberBand>
#include <QScrollArea>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>
#include <QStringList>

#include "Common/CommonTypes.h"
#include "Core/Core.h"
#include "Core/HotkeyManager.h"
#include "Core/HW/WiimoteEmu/Extension/Classic.h"
#include "Core/HW/WiimoteEmu/Extension/Nunchuk.h"
#include "Core/HW/WiimoteEmu/MotionPlus.h"
#include "Core/System.h"
#include "DolphinQt/Host.h"
#include "InputCommon/GCPadStatus.h"

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
constexpr int k_drag_hold_ms = 750;
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
  std::array<bool, 4> active_gba_controllers{};
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

std::array<bool, 4> GetGBAControllersFromHeader(const Movie::DTMHeader& header)
{
  std::array<bool, 4> controllers{};
  for (int i = 0; i < 4; ++i)
    controllers[i] = (header.GBAControllers & (1 << i)) != 0;
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

bool IsVisibleIRPoint(const WiimoteEmu::CameraPoint& point)
{
  return point.position.x != 0xffff && point.position.y != 0xffff;
}

bool HasValidWiiSerializedState(const Movie::WiiRuntimeInputRow& row)
{
  return row.serialized_state.length <= row.serialized_state.data.size();
}

bool WiiRowsEqual(const Movie::WiiRuntimeInputRow& lhs, const Movie::WiiRuntimeInputRow& rhs)
{
  if (!HasValidWiiSerializedState(lhs) || !HasValidWiiSerializedState(rhs))
    return false;

  return lhs.wiimote == rhs.wiimote && lhs.is_reset == rhs.is_reset &&
         lhs.serialized_state.length == rhs.serialized_state.length &&
         std::memcmp(lhs.serialized_state.data.data(), rhs.serialized_state.data.data(),
                     lhs.serialized_state.length) == 0;
}

void ClearClassicInput(WiimoteEmu::Classic::DataFormat* classic)
{
  classic->SetButtons(0);
  classic->SetLeftStick(
      {WiimoteEmu::Classic::LEFT_STICK_CENTER, WiimoteEmu::Classic::LEFT_STICK_CENTER});
  classic->SetRightStick(
      {WiimoteEmu::Classic::RIGHT_STICK_CENTER, WiimoteEmu::Classic::RIGHT_STICK_CENTER});
  classic->SetLeftTrigger(0);
  classic->SetRightTrigger(0);
}

Movie::ControllerState MakeNeutralGCStateLike(const Movie::ControllerState& like)
{
  Movie::ControllerState neutral = like;
  neutral.Start = false;
  neutral.A = false;
  neutral.B = false;
  neutral.X = false;
  neutral.Y = false;
  neutral.Z = false;
  neutral.DPadUp = false;
  neutral.DPadDown = false;
  neutral.DPadLeft = false;
  neutral.DPadRight = false;
  neutral.L = false;
  neutral.R = false;
  neutral.disc = false;
  neutral.reset = false;
  neutral.get_origin = false;
  neutral.TriggerL = 0;
  neutral.TriggerR = 0;
  neutral.AnalogStickX = GCPadStatus::MAIN_STICK_CENTER_X;
  neutral.AnalogStickY = GCPadStatus::MAIN_STICK_CENTER_Y;
  neutral.CStickX = GCPadStatus::C_STICK_CENTER_X;
  neutral.CStickY = GCPadStatus::C_STICK_CENTER_Y;
  return neutral;
}

Movie::WiiRuntimeInputRow MakeNeutralWiiRowLike(const Movie::WiiRuntimeInputRow& like)
{
  Movie::WiiRuntimeInputRow neutral = like;
  neutral.is_reset = false;

  WiimoteEmu::DesiredWiimoteState state;
  if (!like.is_reset &&
      (!HasValidWiiSerializedState(like) ||
       !WiimoteEmu::DeserializeDesiredState(&state, like.serialized_state)))
  {
    return neutral;
  }

  state.buttons.hex = 0;
  state.acceleration = WiimoteEmu::DesiredWiimoteState::DEFAULT_ACCELERATION;
  state.camera_points = WiimoteEmu::DesiredWiimoteState::DEFAULT_CAMERA;
  state.battery = 100;

  if (state.motion_plus.has_value())
  {
    state.motion_plus->gyro.value.x = WiimoteEmu::MotionPlus::ZERO_VALUE;
    state.motion_plus->gyro.value.y = WiimoteEmu::MotionPlus::ZERO_VALUE;
    state.motion_plus->gyro.value.z = WiimoteEmu::MotionPlus::ZERO_VALUE;
    state.motion_plus->is_slow = {};
  }

  if (std::holds_alternative<WiimoteEmu::Nunchuk::DataFormat>(state.extension.data))
  {
    auto nunchuk = std::get<WiimoteEmu::Nunchuk::DataFormat>(state.extension.data);
    nunchuk.jx = WiimoteEmu::Nunchuk::STICK_CENTER;
    nunchuk.jy = WiimoteEmu::Nunchuk::STICK_CENTER;
    nunchuk.SetButtons(0);
    nunchuk.SetAccel(WiimoteEmu::Nunchuk::DataFormat::AccelType(k_nunchuk_accel_zero,
                                                                k_nunchuk_accel_zero,
                                                                k_nunchuk_accel_one));
    state.extension.data = nunchuk;
  }
  else if (std::holds_alternative<WiimoteEmu::Classic::DataFormat>(state.extension.data))
  {
    auto classic = std::get<WiimoteEmu::Classic::DataFormat>(state.extension.data);
    ClearClassicInput(&classic);
    state.extension.data = classic;
  }

  neutral.serialized_state = WiimoteEmu::SerializeDesiredState(state);
  return neutral;
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
  if (state.L || state.TriggerL > 0)
    parts << QStringLiteral("L%1").arg(state.TriggerL);
  if (state.R || state.TriggerR > 0)
    parts << QStringLiteral("R%1").arg(state.TriggerR);
  if (state.AnalogStickX != GCPadStatus::MAIN_STICK_CENTER_X ||
      state.AnalogStickY != GCPadStatus::MAIN_STICK_CENTER_Y)
  {
    parts << QStringLiteral("LS(%1,%2)").arg(state.AnalogStickX).arg(state.AnalogStickY);
  }
  if (state.CStickX != GCPadStatus::C_STICK_CENTER_X ||
      state.CStickY != GCPadStatus::C_STICK_CENTER_Y)
  {
    parts << QStringLiteral("CS(%1,%2)").arg(state.CStickX).arg(state.CStickY);
  }
  return parts.join(QStringLiteral(" "));
}

QString FormatWiiState(const Movie::WiiRuntimeInputRow& row, bool hide_remote_inputs)
{
  if (row.is_reset)
    return QStringLiteral("[RESET]");
  if (!HasValidWiiSerializedState(row))
    return QStringLiteral("[invalid]");

  WiimoteEmu::DesiredWiimoteState state;
  if (!WiimoteEmu::DeserializeDesiredState(&state, row.serialized_state))
    return QStringLiteral("[invalid]");

  QStringList parts;
  if (!hide_remote_inputs)
  {
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

    if (state.acceleration.value.x !=
            WiimoteEmu::DesiredWiimoteState::DEFAULT_ACCELERATION.value.x ||
        state.acceleration.value.y !=
            WiimoteEmu::DesiredWiimoteState::DEFAULT_ACCELERATION.value.y ||
        state.acceleration.value.z !=
            WiimoteEmu::DesiredWiimoteState::DEFAULT_ACCELERATION.value.z)
    {
      parts << QStringLiteral("Accel(%1,%2,%3)")
                   .arg(state.acceleration.value.x)
                   .arg(state.acceleration.value.y)
                   .arg(state.acceleration.value.z);
    }

    QStringList ir_parts;
    for (size_t i = 0; i < state.camera_points.size(); ++i)
    {
      if (!IsVisibleIRPoint(state.camera_points[i]))
        continue;
      ir_parts << QStringLiteral("P%1(%2,%3,%4)")
                      .arg(i + 1)
                      .arg(state.camera_points[i].position.x)
                      .arg(state.camera_points[i].position.y)
                      .arg(state.camera_points[i].size);
    }
    if (!ir_parts.isEmpty())
      parts << QStringLiteral("IR[%1]").arg(ir_parts.join(QStringLiteral(" ")));

    if (state.motion_plus.has_value())
    {
      const auto& gyro = state.motion_plus->gyro.value;
      if (gyro.x != WiimoteEmu::MotionPlus::ZERO_VALUE ||
          gyro.y != WiimoteEmu::MotionPlus::ZERO_VALUE ||
          gyro.z != WiimoteEmu::MotionPlus::ZERO_VALUE || state.motion_plus->is_slow.x ||
          state.motion_plus->is_slow.y || state.motion_plus->is_slow.z)
      {
        parts << QStringLiteral("Gyro(%1,%2,%3)")
                     .arg(GyroRawToTasValue(gyro.x))
                     .arg(GyroRawToTasValue(gyro.y))
                     .arg(GyroRawToTasValue(gyro.z));
      }
    }

    if (state.battery.has_value() && *state.battery != 100)
      parts << QStringLiteral("Bat(%1%)").arg(*state.battery);
  }

  if (!hide_remote_inputs &&
      std::holds_alternative<WiimoteEmu::Nunchuk::DataFormat>(state.extension.data))
  {
    const auto& nunchuk = std::get<WiimoteEmu::Nunchuk::DataFormat>(state.extension.data);
    const u8 buttons = nunchuk.GetButtons();
    QStringList ext;
    if (buttons & WiimoteEmu::Nunchuk::BUTTON_C)
      ext << QStringLiteral("C");
    if (buttons & WiimoteEmu::Nunchuk::BUTTON_Z)
      ext << QStringLiteral("Z");
    if (nunchuk.GetStick().value.x != WiimoteEmu::Nunchuk::STICK_CENTER ||
        nunchuk.GetStick().value.y != WiimoteEmu::Nunchuk::STICK_CENTER)
    {
      ext << QStringLiteral("Stick(%1,%2)")
                 .arg(nunchuk.GetStick().value.x)
                 .arg(nunchuk.GetStick().value.y);
    }
    if (nunchuk.GetAccel().value.x != k_nunchuk_accel_zero ||
        nunchuk.GetAccel().value.y != k_nunchuk_accel_zero ||
        nunchuk.GetAccel().value.z != k_nunchuk_accel_one)
    {
      ext << QStringLiteral("NAccel(%1,%2,%3)")
                 .arg(nunchuk.GetAccel().value.x)
                 .arg(nunchuk.GetAccel().value.y)
                 .arg(nunchuk.GetAccel().value.z);
    }
    if (!ext.isEmpty())
      parts << ext.join(QStringLiteral(" "));
  }
  else if (std::holds_alternative<WiimoteEmu::Classic::DataFormat>(state.extension.data))
  {
    const auto& classic = std::get<WiimoteEmu::Classic::DataFormat>(state.extension.data);
    const u16 buttons = classic.GetButtons();
    QStringList ext;
    if (buttons & WiimoteEmu::Classic::BUTTON_A)
      ext << QStringLiteral("A");
    if (buttons & WiimoteEmu::Classic::BUTTON_B)
      ext << QStringLiteral("B");
    if (buttons & WiimoteEmu::Classic::BUTTON_X)
      ext << QStringLiteral("X");
    if (buttons & WiimoteEmu::Classic::BUTTON_Y)
      ext << QStringLiteral("Y");
    if (buttons & WiimoteEmu::Classic::BUTTON_ZL)
      ext << QStringLiteral("ZL");
    if (buttons & WiimoteEmu::Classic::BUTTON_ZR)
      ext << QStringLiteral("ZR");
    if (buttons & WiimoteEmu::Classic::TRIGGER_L)
      ext << QStringLiteral("L");
    if (buttons & WiimoteEmu::Classic::TRIGGER_R)
      ext << QStringLiteral("R");
    if (buttons & WiimoteEmu::Classic::BUTTON_MINUS)
      ext << QStringLiteral("-");
    if (buttons & WiimoteEmu::Classic::BUTTON_PLUS)
      ext << QStringLiteral("+");
    if (buttons & WiimoteEmu::Classic::BUTTON_HOME)
      ext << QStringLiteral("HOME");
    if (buttons & WiimoteEmu::Classic::PAD_UP)
      ext << QStringLiteral("Up");
    if (buttons & WiimoteEmu::Classic::PAD_DOWN)
      ext << QStringLiteral("Down");
    if (buttons & WiimoteEmu::Classic::PAD_LEFT)
      ext << QStringLiteral("Left");
    if (buttons & WiimoteEmu::Classic::PAD_RIGHT)
      ext << QStringLiteral("Right");

    const auto left_stick = classic.GetLeftStick().value;
    const auto right_stick = classic.GetRightStick().value;
    const u8 left_trigger = classic.GetLeftTrigger().value;
    const u8 right_trigger = classic.GetRightTrigger().value;
    if (left_stick.x != WiimoteEmu::Classic::LEFT_STICK_CENTER ||
        left_stick.y != WiimoteEmu::Classic::LEFT_STICK_CENTER)
    {
      ext << QStringLiteral("CLS(%1,%2)").arg(left_stick.x).arg(left_stick.y);
    }
    if (right_stick.x != WiimoteEmu::Classic::RIGHT_STICK_CENTER ||
        right_stick.y != WiimoteEmu::Classic::RIGHT_STICK_CENTER)
    {
      ext << QStringLiteral("CRS(%1,%2)").arg(right_stick.x).arg(right_stick.y);
    }
    if (left_trigger != 0)
      ext << QStringLiteral("CLT%1").arg(left_trigger);
    if (right_trigger != 0)
      ext << QStringLiteral("CRT%1").arg(right_trigger);
    if (!ext.isEmpty())
      parts << (hide_remote_inputs ? ext.join(QStringLiteral(" ")) :
                                      QStringLiteral("Classic[%1]").arg(ext.join(QStringLiteral(" "))));
  }

  return parts.join(QStringLiteral(" "));
}

Movie::WiiRuntimeInputRow MakeNeutralClassicWiiRowLike(const Movie::WiiRuntimeInputRow& like)
{
  Movie::WiiRuntimeInputRow neutral = like;
  if (neutral.is_reset)
    return neutral;
  if (!HasValidWiiSerializedState(neutral))
    return neutral;

  WiimoteEmu::DesiredWiimoteState state;
  if (!WiimoteEmu::DeserializeDesiredState(&state, neutral.serialized_state) ||
      !std::holds_alternative<WiimoteEmu::Classic::DataFormat>(state.extension.data))
  {
    return neutral;
  }

  auto classic = std::get<WiimoteEmu::Classic::DataFormat>(state.extension.data);
  ClearClassicInput(&classic);
  state.extension.data = classic;
  neutral.serialized_state = WiimoteEmu::SerializeDesiredState(state);
  return neutral;
}

std::optional<Movie::WiiRuntimeInputRow> CopyClassicWiiInputOnly(
    const Movie::WiiRuntimeInputRow& destination, const Movie::WiiRuntimeInputRow& source)
{
  if (destination.is_reset || source.is_reset)
    return std::nullopt;
  if (!HasValidWiiSerializedState(destination) || !HasValidWiiSerializedState(source))
    return std::nullopt;

  WiimoteEmu::DesiredWiimoteState destination_state;
  WiimoteEmu::DesiredWiimoteState source_state;
  if (!WiimoteEmu::DeserializeDesiredState(&destination_state, destination.serialized_state) ||
      !WiimoteEmu::DeserializeDesiredState(&source_state, source.serialized_state) ||
      !std::holds_alternative<WiimoteEmu::Classic::DataFormat>(destination_state.extension.data) ||
      !std::holds_alternative<WiimoteEmu::Classic::DataFormat>(source_state.extension.data))
  {
    return std::nullopt;
  }

  Movie::WiiRuntimeInputRow updated = destination;
  destination_state.extension.data =
      std::get<WiimoteEmu::Classic::DataFormat>(source_state.extension.data);
  updated.serialized_state = WiimoteEmu::SerializeDesiredState(destination_state);
  return updated;
}

std::optional<ParsedGCDTMData> ParseGCDTMFile(const QByteArray& bytes)
{
  if (bytes.size() < static_cast<int>(sizeof(Movie::DTMHeader)))
    return std::nullopt;

  ParsedGCDTMData parsed;
  std::memcpy(&parsed.header, bytes.constData(), sizeof(parsed.header));
  parsed.active_controllers = GetGCControllersFromHeader(parsed.header);
  parsed.active_gba_controllers = GetGBAControllersFromHeader(parsed.header);
  const auto active_wiimotes = GetWiimotesFromHeader(parsed.header);
  if (std::ranges::any_of(active_wiimotes, [](bool active) { return active; }))
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
    Movie::WiiRuntimeInputRow row;
    if (length > row.serialized_state.data.size() || offset + length > payload.size())
      return std::nullopt;

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
        return QString::number(index.row());

      if (m_kind == ModelMovieKind::GC)
      {
        int controller = -1;
        if (IsGCControllerColumn(index.column(), &controller))
          return FormatControllerState(m_gc_rows[index.row()][controller]);
      }
      else if (m_kind == ModelMovieKind::Wii && index.column() == 1)
      {
        return FormatWiiState(m_wii_rows[index.row()], m_hide_wii_remote_inputs);
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
      return QStringLiteral("Input Frame");
    if (m_kind == ModelMovieKind::GC)
    {
      int visual = 1;
      for (int i = 0; i < 4; ++i)
      {
        if (!m_active_gc[i])
          continue;
        if (visual == section)
          return GetGCColumnLabel(i);
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
    m_active_gba = {};
    m_active_wii = {};
    m_active_gc_count = 0;
    m_gc_rows.clear();
    m_wii_rows.clear();
    m_highlight_row = -1;
    endResetModel();
  }

  void SetGCMovieData(const std::array<bool, 4>& active_controllers,
                      const std::array<bool, 4>& active_gba_controllers,
                      const std::vector<std::array<Movie::ControllerState, 4>>& rows)
  {
    beginResetModel();
    m_kind = ModelMovieKind::GC;
    m_active_gc = active_controllers;
    m_active_gba = active_gba_controllers;
    m_active_gc_count = 0;
    for (bool active : active_controllers)
    {
      if (active)
        ++m_active_gc_count;
    }
    m_gc_rows = rows;
    m_wii_rows.clear();
    endResetModel();
  }

  void SetWiiMovieData(const std::array<bool, 4>& active_wiimotes,
                       const std::vector<Movie::WiiRuntimeInputRow>& rows)
  {
    beginResetModel();
    m_kind = ModelMovieKind::Wii;
    m_active_wii = active_wiimotes;
    m_gc_rows.clear();
    m_wii_rows = rows;
    endResetModel();
  }

  bool HasGCLayout(const std::array<bool, 4>& active_controllers,
                   const std::array<bool, 4>& active_gba_controllers) const
  {
    return m_kind == ModelMovieKind::GC && m_active_gc == active_controllers &&
           m_active_gba == active_gba_controllers;
  }

  bool HasWiiLayout(const std::array<bool, 4>& active_wiimotes) const
  {
    return m_kind == ModelMovieKind::Wii && m_active_wii == active_wiimotes;
  }

  void SetHideWiiRemoteInputs(bool hide)
  {
    if (m_hide_wii_remote_inputs == hide)
      return;
    m_hide_wii_remote_inputs = hide;
    if (m_kind == ModelMovieKind::Wii && rowCount() > 0)
      emit dataChanged(index(0, 1), index(rowCount() - 1, 1));
  }

  void AppendGCRows(const std::vector<std::array<Movie::ControllerState, 4>>& rows)
  {
    const int old_count = rowCount();
    const int new_count = static_cast<int>(rows.size());
    if (new_count <= old_count)
      return;

    beginInsertRows({}, old_count, new_count - 1);
    m_gc_rows.resize(rows.size());
    std::copy(rows.begin() + old_count, rows.end(), m_gc_rows.begin() + old_count);
    endInsertRows();
  }

  void ReplaceGCRow(int row, const std::array<Movie::ControllerState, 4>& data)
  {
    if (row < 0 || row >= rowCount())
      return;

    auto& existing = m_gc_rows[static_cast<size_t>(row)];
    if (std::memcmp(&existing, &data, sizeof(existing)) == 0)
      return;

    existing = data;
    emit dataChanged(index(row, 0), index(row, std::max(0, columnCount() - 1)));
  }

  void ReplaceWiiRow(int row, const Movie::WiiRuntimeInputRow& data)
  {
    if (row < 0 || row >= rowCount())
      return;

    auto& existing = m_wii_rows[static_cast<size_t>(row)];
    if (WiiRowsEqual(existing, data))
      return;

    existing = data;
    emit dataChanged(index(row, 0), index(row, std::max(0, columnCount() - 1)));
  }

  void AppendWiiRows(const std::vector<Movie::WiiRuntimeInputRow>& rows)
  {
    const int old_count = rowCount();
    const int new_count = static_cast<int>(rows.size());
    if (new_count <= old_count)
      return;

    beginInsertRows({}, old_count, new_count - 1);
    m_wii_rows.resize(rows.size());
    std::copy(rows.begin() + old_count, rows.end(), m_wii_rows.begin() + old_count);
    endInsertRows();
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

  bool IsGCControllerActive(int controller) const
  {
    return controller >= 0 && controller < 4 && m_active_gc[controller];
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

  QString GetRowGameFrameLabel(int row) const
  {
    if (row < 0 || row >= rowCount())
      return {};
    return QString::number(row);
  }

  QString GetGCColumnLabel(int controller) const
  {
    if (controller < 0 || controller >= 4)
      return {};

    const int display_controller = GetGCDisplayController(controller);
    const bool is_gba = m_active_gba[display_controller];
    const int player = display_controller + 1;

    return is_gba ? QStringLiteral("GBA P%1").arg(player) :
                    QStringLiteral("GCN P%1").arg(player);
  }

  int GetGCDisplayController(int controller) const
  {
    std::vector<int> active_gc;
    std::vector<int> active_gba;
    for (int i = 0; i < 4; ++i)
    {
      if (!m_active_gc[i])
        continue;

      if (m_active_gba[i])
        active_gba.push_back(i);
      else
        active_gc.push_back(i);
    }

    if (active_gc.size() == 1 && active_gba.size() == 1)
    {
      if (controller == active_gc.front())
        return active_gba.front();
      if (controller == active_gba.front())
        return active_gc.front();
    }

    return controller;
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
  std::array<bool, 4> m_active_gba{};
  std::array<bool, 4> m_active_wii{};
  int m_active_gc_count = 0;
  std::vector<std::array<Movie::ControllerState, 4>> m_gc_rows;
  std::vector<Movie::WiiRuntimeInputRow> m_wii_rows;
  int m_highlight_row = -1;
  bool m_hide_wii_remote_inputs = false;
};

DTMEditorDialog::DTMEditorDialog(QWidget* parent) : QDialog(parent)
{
  setWindowTitle(tr("DTM Editor"));
  resize(1100, 700);
  setAttribute(Qt::WA_QuitOnClose, false);

  CreateWidgets();
  Refresh();

  m_refresh_timer = new QTimer(this);
  m_refresh_timer->setInterval(100);
  connect(m_refresh_timer, &QTimer::timeout, this, &DTMEditorDialog::Refresh);

  m_drag_hold_timer = new QTimer(this);
  m_drag_hold_timer->setSingleShot(true);
  m_drag_hold_timer->setInterval(k_drag_hold_ms);
  connect(m_drag_hold_timer, &QTimer::timeout, this, &DTMEditorDialog::BeginPendingDrag);
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
  m_table->setSelectionBehavior(QAbstractItemView::SelectItems);
  m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_table->setAlternatingRowColors(true);
  m_table->setSortingEnabled(false);
  m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  m_table->setMouseTracking(true);
  m_table->setContextMenuPolicy(Qt::DefaultContextMenu);
  m_table->verticalHeader()->setDefaultSectionSize(6);
  m_table->verticalHeader()->setVisible(false);
  m_table->horizontalHeader()->setStretchLastSection(true);
  m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  m_table->setStyleSheet(
      QStringLiteral("QTableView::item:selected { background-color: rgba(120, 200, 120, 96); }"));
  m_table->viewport()->installEventFilter(this);
  m_drag_preview = new QRubberBand(QRubberBand::Rectangle, m_table->viewport());
  m_drag_preview->setAttribute(Qt::WA_TransparentForMouseEvents);
  m_drag_preview->setStyleSheet(
      QStringLiteral("border: 2px solid rgba(120, 200, 120, 220); "
                     "background-color: rgba(120, 200, 120, 36);"));
  m_drag_preview->hide();
  connect(m_table->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
          [this](const QModelIndex&, const QModelIndex&) {
            UpdateStatusLabel();
            PopulateEditor();
          });

  auto* copy_action = new QAction(tr("Copy"), this);
  copy_action->setShortcut(QKeySequence::Copy);
  copy_action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  connect(copy_action, &QAction::triggered, this, &DTMEditorDialog::CopySelectedInputs);
  m_table->addAction(copy_action);

  auto* paste_action = new QAction(tr("Paste"), this);
  paste_action->setShortcut(QKeySequence::Paste);
  paste_action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  connect(paste_action, &QAction::triggered, this, [this] {
    const QModelIndex current = m_table->currentIndex();
    if (!current.isValid())
      return;
    PasteCopiedInputs(current.row());
  });
  m_table->addAction(paste_action);

  m_open_button = new QPushButton(tr("Open DTM"), this);
  m_save_button = new QPushButton(tr("Save"), this);
  m_select_buttons = new QCheckBox(tr("Select Buttons"), this);
  m_wii_hide_remote_inputs = new QCheckBox(tr("Hide Remote Inputs"), this);
  connect(m_open_button, &QPushButton::clicked, this, &DTMEditorDialog::PromptOpen);
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
  button_layout->addWidget(m_save_button);
  button_layout->addWidget(m_select_buttons);
  button_layout->addWidget(m_wii_hide_remote_inputs);
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
    box->setKeyboardTracking(false);
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
  m_wii_battery_label = new QLabel(tr("Battery (%):"), wii_page);
  info_layout->addWidget(m_wii_battery_label, 2, 0);
  m_wii_battery = new QSpinBox(wii_page);
  m_wii_battery->setRange(0, 100);
  m_wii_battery->setKeyboardTracking(false);
  info_layout->addWidget(m_wii_battery, 2, 1);
  info_layout->addWidget(new QLabel(tr("Reset Marker:"), wii_page), 3, 0);
  info_layout->addWidget(m_wii_reset, 3, 1);
  wii_layout->addLayout(info_layout);

  m_wii_remote_button_group = new QGroupBox(tr("Wii Remote Buttons"), wii_page);
  auto* wii_button_layout = new QGridLayout(m_wii_remote_button_group);
  m_wii_a = new QCheckBox(tr("A"), m_wii_remote_button_group);
  m_wii_b = new QCheckBox(tr("B"), m_wii_remote_button_group);
  m_wii_one = new QCheckBox(tr("1"), m_wii_remote_button_group);
  m_wii_two = new QCheckBox(tr("2"), m_wii_remote_button_group);
  m_wii_plus = new QCheckBox(tr("+"), m_wii_remote_button_group);
  m_wii_minus = new QCheckBox(tr("-"), m_wii_remote_button_group);
  m_wii_home = new QCheckBox(tr("HOME"), m_wii_remote_button_group);
  m_wii_up = new QCheckBox(tr("Up"), m_wii_remote_button_group);
  m_wii_down = new QCheckBox(tr("Down"), m_wii_remote_button_group);
  m_wii_left = new QCheckBox(tr("Left"), m_wii_remote_button_group);
  m_wii_right = new QCheckBox(tr("Right"), m_wii_remote_button_group);
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
  wii_layout->addWidget(m_wii_remote_button_group);

  m_wii_accel_group = new QGroupBox(tr("Wii Remote Accelerometer"), wii_page);
  auto* accel_layout = new QGridLayout(m_wii_accel_group);
  m_wii_accel_x = new QSpinBox(m_wii_accel_group);
  m_wii_accel_y = new QSpinBox(m_wii_accel_group);
  m_wii_accel_z = new QSpinBox(m_wii_accel_group);
  for (QSpinBox* box : {m_wii_accel_x, m_wii_accel_y, m_wii_accel_z})
  {
    box->setRange(0, 1023);
    box->setKeyboardTracking(false);
  }
  accel_layout->addWidget(new QLabel(tr("X"), m_wii_accel_group), 0, 0);
  accel_layout->addWidget(m_wii_accel_x, 0, 1);
  accel_layout->addWidget(new QLabel(tr("Y"), m_wii_accel_group), 1, 0);
  accel_layout->addWidget(m_wii_accel_y, 1, 1);
  accel_layout->addWidget(new QLabel(tr("Z"), m_wii_accel_group), 2, 0);
  accel_layout->addWidget(m_wii_accel_z, 2, 1);
  wii_layout->addWidget(m_wii_accel_group);

  m_wii_ir_group = new QGroupBox(tr("IR Camera"), wii_page);
  auto* ir_layout = new QGridLayout(m_wii_ir_group);
  ir_layout->addWidget(new QLabel(tr("Visible"), m_wii_ir_group), 0, 0);
  ir_layout->addWidget(new QLabel(tr("X"), m_wii_ir_group), 0, 1);
  ir_layout->addWidget(new QLabel(tr("Y"), m_wii_ir_group), 0, 2);
  ir_layout->addWidget(new QLabel(tr("Size"), m_wii_ir_group), 0, 3);
  for (int i = 0; i < 4; ++i)
  {
    m_wii_ir_visible[i] = new QCheckBox(QStringLiteral("P%1").arg(i + 1), m_wii_ir_group);
    m_wii_ir_x[i] = new QSpinBox(m_wii_ir_group);
    m_wii_ir_y[i] = new QSpinBox(m_wii_ir_group);
    m_wii_ir_size[i] = new QSpinBox(m_wii_ir_group);
    m_wii_ir_x[i]->setRange(0, 1023);
    m_wii_ir_y[i]->setRange(0, 767);
    m_wii_ir_size[i]->setRange(0, 15);
    m_wii_ir_x[i]->setKeyboardTracking(false);
    m_wii_ir_y[i]->setKeyboardTracking(false);
    m_wii_ir_size[i]->setKeyboardTracking(false);
    ir_layout->addWidget(m_wii_ir_visible[i], i + 1, 0);
    ir_layout->addWidget(m_wii_ir_x[i], i + 1, 1);
    ir_layout->addWidget(m_wii_ir_y[i], i + 1, 2);
    ir_layout->addWidget(m_wii_ir_size[i], i + 1, 3);
    connect(m_wii_ir_visible[i], &QCheckBox::toggled, this, [this] { ApplyEditorChanges(); });
    connect(m_wii_ir_x[i], qOverload<int>(&QSpinBox::valueChanged), this,
            [this] { ApplyEditorChanges(); });
    connect(m_wii_ir_y[i], qOverload<int>(&QSpinBox::valueChanged), this,
            [this] { ApplyEditorChanges(); });
    connect(m_wii_ir_size[i], qOverload<int>(&QSpinBox::valueChanged), this,
            [this] { ApplyEditorChanges(); });
  }
  wii_layout->addWidget(m_wii_ir_group);

  m_wii_gyro_group = new QGroupBox(tr("MotionPlus Gyroscope"), wii_page);
  auto* gyro_layout = new QGridLayout(m_wii_gyro_group);
  m_wii_gyro_x = new QSpinBox(m_wii_gyro_group);
  m_wii_gyro_y = new QSpinBox(m_wii_gyro_group);
  m_wii_gyro_z = new QSpinBox(m_wii_gyro_group);
  m_wii_gyro_slow_x = new QCheckBox(tr("Slow X"), m_wii_gyro_group);
  m_wii_gyro_slow_y = new QCheckBox(tr("Slow Y"), m_wii_gyro_group);
  m_wii_gyro_slow_z = new QCheckBox(tr("Slow Z"), m_wii_gyro_group);
  for (QSpinBox* box : {m_wii_gyro_x, m_wii_gyro_y, m_wii_gyro_z})
  {
    box->setRange(k_gyro_min, k_gyro_max);
    box->setKeyboardTracking(false);
  }
  gyro_layout->addWidget(new QLabel(tr("X"), m_wii_gyro_group), 0, 0);
  gyro_layout->addWidget(m_wii_gyro_x, 0, 1);
  gyro_layout->addWidget(m_wii_gyro_slow_x, 0, 2);
  gyro_layout->addWidget(new QLabel(tr("Y"), m_wii_gyro_group), 1, 0);
  gyro_layout->addWidget(m_wii_gyro_y, 1, 1);
  gyro_layout->addWidget(m_wii_gyro_slow_y, 1, 2);
  gyro_layout->addWidget(new QLabel(tr("Z"), m_wii_gyro_group), 2, 0);
  gyro_layout->addWidget(m_wii_gyro_z, 2, 1);
  gyro_layout->addWidget(m_wii_gyro_slow_z, 2, 2);
  wii_layout->addWidget(m_wii_gyro_group);

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
  m_wii_nunchuk_x->setKeyboardTracking(false);
  m_wii_nunchuk_y->setKeyboardTracking(false);
  for (QSpinBox* box : {m_wii_nunchuk_accel_x, m_wii_nunchuk_accel_y, m_wii_nunchuk_accel_z})
  {
    box->setRange(0, 1023);
    box->setKeyboardTracking(false);
  }
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

  auto* classic_group = new QGroupBox(tr("Classic Controller / WiiVC N64"), wii_page);
  auto* classic_layout = new QGridLayout(classic_group);
  m_wii_classic_a = new QCheckBox(tr("A"), classic_group);
  m_wii_classic_b = new QCheckBox(tr("B"), classic_group);
  m_wii_classic_x = new QCheckBox(tr("X"), classic_group);
  m_wii_classic_y = new QCheckBox(tr("Y"), classic_group);
  m_wii_classic_zl = new QCheckBox(tr("ZL"), classic_group);
  m_wii_classic_zr = new QCheckBox(tr("ZR"), classic_group);
  m_wii_classic_l = new QCheckBox(tr("L"), classic_group);
  m_wii_classic_r = new QCheckBox(tr("R"), classic_group);
  m_wii_classic_minus = new QCheckBox(tr("-"), classic_group);
  m_wii_classic_plus = new QCheckBox(tr("+"), classic_group);
  m_wii_classic_home = new QCheckBox(tr("HOME"), classic_group);
  m_wii_classic_up = new QCheckBox(tr("Up"), classic_group);
  m_wii_classic_down = new QCheckBox(tr("Down"), classic_group);
  m_wii_classic_left = new QCheckBox(tr("Left"), classic_group);
  m_wii_classic_right = new QCheckBox(tr("Right"), classic_group);
  const std::array<QCheckBox*, 15> classic_checks = {
      m_wii_classic_a,     m_wii_classic_b,    m_wii_classic_x,     m_wii_classic_y,
      m_wii_classic_zl,    m_wii_classic_zr,   m_wii_classic_l,     m_wii_classic_r,
      m_wii_classic_minus, m_wii_classic_plus, m_wii_classic_home,  m_wii_classic_up,
      m_wii_classic_down,  m_wii_classic_left, m_wii_classic_right};
  int classic_row = 0;
  int classic_col = 0;
  for (QCheckBox* box : classic_checks)
  {
    classic_layout->addWidget(box, classic_row, classic_col);
    if (++classic_col == 4)
    {
      classic_col = 0;
      ++classic_row;
    }
  }
  ++classic_row;
  m_wii_classic_left_x = new QSpinBox(classic_group);
  m_wii_classic_left_y = new QSpinBox(classic_group);
  m_wii_classic_right_x = new QSpinBox(classic_group);
  m_wii_classic_right_y = new QSpinBox(classic_group);
  m_wii_classic_l_trigger = new QSpinBox(classic_group);
  m_wii_classic_r_trigger = new QSpinBox(classic_group);
  for (QSpinBox* box : {m_wii_classic_left_x, m_wii_classic_left_y})
  {
    box->setRange(0, WiimoteEmu::Classic::LEFT_STICK_RANGE);
    box->setKeyboardTracking(false);
  }
  for (QSpinBox* box : {m_wii_classic_right_x, m_wii_classic_right_y})
  {
    box->setRange(0, WiimoteEmu::Classic::RIGHT_STICK_RANGE);
    box->setKeyboardTracking(false);
  }
  for (QSpinBox* box : {m_wii_classic_l_trigger, m_wii_classic_r_trigger})
  {
    box->setRange(0, WiimoteEmu::Classic::TRIGGER_RANGE);
    box->setKeyboardTracking(false);
  }
  classic_layout->addWidget(new QLabel(tr("Left Stick X"), classic_group), classic_row, 0);
  classic_layout->addWidget(m_wii_classic_left_x, classic_row, 1);
  classic_layout->addWidget(new QLabel(tr("Left Stick Y"), classic_group), classic_row, 2);
  classic_layout->addWidget(m_wii_classic_left_y, classic_row, 3);
  ++classic_row;
  classic_layout->addWidget(new QLabel(tr("Right Stick X"), classic_group), classic_row, 0);
  classic_layout->addWidget(m_wii_classic_right_x, classic_row, 1);
  classic_layout->addWidget(new QLabel(tr("Right Stick Y"), classic_group), classic_row, 2);
  classic_layout->addWidget(m_wii_classic_right_y, classic_row, 3);
  ++classic_row;
  classic_layout->addWidget(new QLabel(tr("L Trigger"), classic_group), classic_row, 0);
  classic_layout->addWidget(m_wii_classic_l_trigger, classic_row, 1);
  classic_layout->addWidget(new QLabel(tr("R Trigger"), classic_group), classic_row, 2);
  classic_layout->addWidget(m_wii_classic_r_trigger, classic_row, 3);
  wii_layout->addWidget(classic_group);
  wii_layout->addStretch();

  for (QCheckBox* box : {m_wii_reset, m_wii_c, m_wii_z, m_wii_gyro_slow_x, m_wii_gyro_slow_y,
                         m_wii_gyro_slow_z, m_wii_classic_a, m_wii_classic_b, m_wii_classic_x,
                         m_wii_classic_y, m_wii_classic_zl, m_wii_classic_zr, m_wii_classic_l,
                         m_wii_classic_r, m_wii_classic_minus, m_wii_classic_plus,
                         m_wii_classic_home, m_wii_classic_up, m_wii_classic_down,
                         m_wii_classic_left, m_wii_classic_right})
    connect(box, &QCheckBox::toggled, this, [this] { ApplyEditorChanges(); });
  for (QSpinBox* box : {m_wii_accel_x, m_wii_accel_y, m_wii_accel_z, m_wii_battery, m_wii_gyro_x,
                        m_wii_gyro_y, m_wii_gyro_z, m_wii_nunchuk_x, m_wii_nunchuk_y,
                        m_wii_nunchuk_accel_x, m_wii_nunchuk_accel_y, m_wii_nunchuk_accel_z,
                        m_wii_classic_left_x, m_wii_classic_left_y, m_wii_classic_right_x,
                        m_wii_classic_right_y, m_wii_classic_l_trigger,
                        m_wii_classic_r_trigger})
  {
    connect(box, qOverload<int>(&QSpinBox::valueChanged), this, [this] { ApplyEditorChanges(); });
  }
  connect(m_select_buttons, &QCheckBox::toggled, this,
          [this] { UpdateInputFieldSelectionStyles(); });
  RegisterInputField(m_connected, InputField::GCConnected);
  RegisterInputField(m_start, InputField::GCStart);
  RegisterInputField(m_a, InputField::GCA);
  RegisterInputField(m_b, InputField::GCB);
  RegisterInputField(m_x, InputField::GCX);
  RegisterInputField(m_y, InputField::GCY);
  RegisterInputField(m_z, InputField::GCZ);
  RegisterInputField(m_l, InputField::GCL);
  RegisterInputField(m_r, InputField::GCR);
  RegisterInputField(m_up, InputField::GCUp);
  RegisterInputField(m_down, InputField::GCDown);
  RegisterInputField(m_left, InputField::GCLeft);
  RegisterInputField(m_right, InputField::GCRight);
  RegisterInputField(m_disc, InputField::GCDisc);
  RegisterInputField(m_reset, InputField::GCReset);
  RegisterInputField(m_get_origin, InputField::GCGetOrigin);
  RegisterInputField(m_trigger_l, InputField::GCTriggerL);
  RegisterInputField(m_trigger_r, InputField::GCTriggerR);
  RegisterInputField(m_stick_x, InputField::GCStickX);
  RegisterInputField(m_stick_y, InputField::GCStickY);
  RegisterInputField(m_cstick_x, InputField::GCCStickX);
  RegisterInputField(m_cstick_y, InputField::GCCStickY);
  RegisterInputField(m_wii_reset, InputField::WiiReset);
  RegisterInputField(m_wii_a, InputField::WiiA);
  RegisterInputField(m_wii_b, InputField::WiiB);
  RegisterInputField(m_wii_one, InputField::WiiOne);
  RegisterInputField(m_wii_two, InputField::WiiTwo);
  RegisterInputField(m_wii_plus, InputField::WiiPlus);
  RegisterInputField(m_wii_minus, InputField::WiiMinus);
  RegisterInputField(m_wii_home, InputField::WiiHome);
  RegisterInputField(m_wii_up, InputField::WiiUp);
  RegisterInputField(m_wii_down, InputField::WiiDown);
  RegisterInputField(m_wii_left, InputField::WiiLeft);
  RegisterInputField(m_wii_right, InputField::WiiRight);
  RegisterInputField(m_wii_accel_x, InputField::WiiAccelX);
  RegisterInputField(m_wii_accel_y, InputField::WiiAccelY);
  RegisterInputField(m_wii_accel_z, InputField::WiiAccelZ);
  RegisterInputField(m_wii_battery, InputField::WiiBattery);
  const std::array<std::array<InputField, 4>, 4> ir_fields = {{
      {InputField::WiiIR1Visible, InputField::WiiIR1X, InputField::WiiIR1Y,
       InputField::WiiIR1Size},
      {InputField::WiiIR2Visible, InputField::WiiIR2X, InputField::WiiIR2Y,
       InputField::WiiIR2Size},
      {InputField::WiiIR3Visible, InputField::WiiIR3X, InputField::WiiIR3Y,
       InputField::WiiIR3Size},
      {InputField::WiiIR4Visible, InputField::WiiIR4X, InputField::WiiIR4Y,
       InputField::WiiIR4Size},
  }};
  for (size_t i = 0; i < m_wii_ir_visible.size(); ++i)
  {
    RegisterInputField(m_wii_ir_visible[i], ir_fields[i][0]);
    RegisterInputField(m_wii_ir_x[i], ir_fields[i][1]);
    RegisterInputField(m_wii_ir_y[i], ir_fields[i][2]);
    RegisterInputField(m_wii_ir_size[i], ir_fields[i][3]);
  }
  RegisterInputField(m_wii_gyro_x, InputField::WiiGyroX);
  RegisterInputField(m_wii_gyro_y, InputField::WiiGyroY);
  RegisterInputField(m_wii_gyro_z, InputField::WiiGyroZ);
  RegisterInputField(m_wii_gyro_slow_x, InputField::WiiGyroSlowX);
  RegisterInputField(m_wii_gyro_slow_y, InputField::WiiGyroSlowY);
  RegisterInputField(m_wii_gyro_slow_z, InputField::WiiGyroSlowZ);
  RegisterInputField(m_wii_c, InputField::WiiNunchukC);
  RegisterInputField(m_wii_z, InputField::WiiNunchukZ);
  RegisterInputField(m_wii_nunchuk_x, InputField::WiiNunchukStickX);
  RegisterInputField(m_wii_nunchuk_y, InputField::WiiNunchukStickY);
  RegisterInputField(m_wii_nunchuk_accel_x, InputField::WiiNunchukAccelX);
  RegisterInputField(m_wii_nunchuk_accel_y, InputField::WiiNunchukAccelY);
  RegisterInputField(m_wii_nunchuk_accel_z, InputField::WiiNunchukAccelZ);
  RegisterInputField(m_wii_classic_a, InputField::WiiClassicA);
  RegisterInputField(m_wii_classic_b, InputField::WiiClassicB);
  RegisterInputField(m_wii_classic_x, InputField::WiiClassicX);
  RegisterInputField(m_wii_classic_y, InputField::WiiClassicY);
  RegisterInputField(m_wii_classic_zl, InputField::WiiClassicZL);
  RegisterInputField(m_wii_classic_zr, InputField::WiiClassicZR);
  RegisterInputField(m_wii_classic_l, InputField::WiiClassicL);
  RegisterInputField(m_wii_classic_r, InputField::WiiClassicR);
  RegisterInputField(m_wii_classic_minus, InputField::WiiClassicMinus);
  RegisterInputField(m_wii_classic_plus, InputField::WiiClassicPlus);
  RegisterInputField(m_wii_classic_home, InputField::WiiClassicHome);
  RegisterInputField(m_wii_classic_up, InputField::WiiClassicUp);
  RegisterInputField(m_wii_classic_down, InputField::WiiClassicDown);
  RegisterInputField(m_wii_classic_left, InputField::WiiClassicLeft);
  RegisterInputField(m_wii_classic_right, InputField::WiiClassicRight);
  RegisterInputField(m_wii_classic_left_x, InputField::WiiClassicLeftX);
  RegisterInputField(m_wii_classic_left_y, InputField::WiiClassicLeftY);
  RegisterInputField(m_wii_classic_right_x, InputField::WiiClassicRightX);
  RegisterInputField(m_wii_classic_right_y, InputField::WiiClassicRightY);
  RegisterInputField(m_wii_classic_l_trigger, InputField::WiiClassicTriggerL);
  RegisterInputField(m_wii_classic_r_trigger, InputField::WiiClassicTriggerR);
  connect(m_wii_hide_remote_inputs, &QCheckBox::toggled, this, [this](bool hide) {
    m_wii_battery_label->setVisible(!hide);
    m_wii_battery->setVisible(!hide);
    m_wii_remote_button_group->setVisible(!hide);
    m_wii_accel_group->setVisible(!hide);
    m_wii_ir_group->setVisible(!hide);
    m_wii_gyro_group->setVisible(!hide);
    m_model->SetHideWiiRemoteInputs(hide);
  });
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
  if (!isVisible())
    return;

  if (m_drag_pending || m_drag_active)
    return;

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
    m_last_runtime_data_generation = 0;
    if (m_file_movie_kind == EditorMovieKind::None)
      m_model->Clear();
    else
      m_model->SetHighlightedRow(-1);
  }

  UpdateStatusLabel();
  PopulateEditor();
}

void DTMEditorDialog::showEvent(QShowEvent* event)
{
  QDialog::showEvent(event);
  if (m_refresh_timer && !m_refresh_timer->isActive())
    m_refresh_timer->start();
  UpdateHotkeyFocusState();
  Refresh();
}

void DTMEditorDialog::hideEvent(QHideEvent* event)
{
  ClearPendingDrag();
  if (m_refresh_timer)
    m_refresh_timer->stop();
  QDialog::hideEvent(event);
  UpdateHotkeyFocusState();
}

void DTMEditorDialog::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::ActivationChange)
    UpdateHotkeyFocusState();

  QDialog::changeEvent(event);
}

bool DTMEditorDialog::RefreshGCRuntimeMovie()
{
  auto& movie = Core::System::GetInstance().GetMovie();
  const auto metadata = movie.GetGCRuntimeFrameMetadata();
  if (!metadata.has_value())
    return false;

  const Core::State core_state = Core::GetState(Core::System::GetInstance());
  const bool paused = core_state == Core::State::Paused;
  const bool frame_changed =
      !m_has_last_runtime_frame || metadata->current_frame != m_last_runtime_frame;
  const bool keep_manual_view =
      paused && !frame_changed && isActiveWindow() && m_using_runtime_movie &&
      m_runtime_movie_kind == EditorMovieKind::GC;
  const bool should_follow = !paused || frame_changed || !isActiveWindow();
  const bool runtime_switched = !m_using_runtime_movie || m_runtime_movie_kind != EditorMovieKind::GC;
  const bool data_generation_changed =
      metadata->data_generation != m_last_runtime_data_generation;
  const bool layout_changed =
      !m_model->HasGCLayout(metadata->active_controllers, metadata->active_gba_controllers);
  const bool row_count_changed =
      metadata->row_count != static_cast<u64>(m_model->rowCount());
  const bool live_row_changed =
      static_cast<int>(metadata->current_input_row) != m_last_runtime_row;
  const std::vector<InputCell> previously_selected_cells =
      (!keep_manual_view && !should_follow) ? GetSelectedInputCells() : std::vector<InputCell>{};
  const QModelIndex current = m_table->currentIndex();
  const int previous_column =
      current.isValid() ? current.column() : m_model->GetFirstGCControllerColumn();
  const int previous_row = current.isValid() ? current.row() : -1;

  if (!keep_manual_view)
  {
    if (runtime_switched || data_generation_changed || layout_changed || row_count_changed)
    {
      const auto snapshot = movie.GetGCRuntimeFrameSnapshot();
      if (!snapshot.has_value())
        return false;

      if (runtime_switched || data_generation_changed || layout_changed ||
          snapshot->rows.size() < static_cast<size_t>(m_model->rowCount()))
      {
        m_model->SetGCMovieData(snapshot->active_controllers, snapshot->active_gba_controllers,
                                snapshot->rows);
      }
      else if (snapshot->rows.size() > static_cast<size_t>(m_model->rowCount()))
      {
        m_model->AppendGCRows(snapshot->rows);
      }
    }
  }

  bool refreshed_selected_editor = false;
  if (metadata->row_count > 0 && metadata->current_input_row < metadata->row_count &&
      metadata->current_input_row < static_cast<u64>(m_model->rowCount()))
  {
    const int live_data_row = static_cast<int>(metadata->current_input_row);
    if (const auto row_data = movie.GetGCRuntimeFrameRow(metadata->current_input_row))
    {
      m_model->ReplaceGCRow(live_data_row, *row_data);
      if (current.isValid() && current.row() == live_data_row)
      {
        PopulateEditor();
        refreshed_selected_editor = true;
      }
    }
  }

  m_model->SetHighlightedRow(static_cast<int>(metadata->current_input_row));
  m_using_runtime_movie = true;
  m_runtime_movie_kind = EditorMovieKind::GC;

  const bool selection_needs_update =
      !keep_manual_view &&
      (runtime_switched || data_generation_changed || layout_changed || row_count_changed ||
       (should_follow && live_row_changed));

  if (selection_needs_update)
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
            std::clamp(static_cast<int>(metadata->current_input_row), 0, m_model->rowCount() - 1);
        const QModelIndex idx = m_model->index(live_row, target_column);
        selection_model->clearSelection();
        selection_model->setCurrentIndex(idx, QItemSelectionModel::NoUpdate);
        selection_model->select(idx, QItemSelectionModel::Select);
        const QRect visual_rect = m_table->visualRect(idx);
        if (!m_table->viewport()->rect().contains(visual_rect.center()))
          m_table->scrollTo(idx, QAbstractItemView::EnsureVisible);
      }
      else
      {
        const int restored_row =
            previous_row >= 0 ? std::clamp(previous_row, 0, m_model->rowCount() - 1) : 0;
        const QModelIndex current_idx = m_model->index(restored_row, target_column);
        selection_model->clearSelection();
        selection_model->setCurrentIndex(current_idx, QItemSelectionModel::NoUpdate);
        for (const InputCell& cell : previously_selected_cells)
        {
          if (cell.row < 0 || cell.row >= m_model->rowCount() ||
              !m_model->IsGCControllerColumn(cell.column))
            continue;
          selection_model->select(m_model->index(cell.row, cell.column), QItemSelectionModel::Select);
        }
      }
    }
  }

  m_last_runtime_row = static_cast<int>(metadata->current_input_row);
  m_last_runtime_frame = metadata->current_frame;
  m_last_runtime_data_generation = metadata->data_generation;
  m_has_last_runtime_frame = true;
  if (runtime_switched)
    UpdateStatusLabel();
  if (!refreshed_selected_editor &&
      (selection_needs_update || runtime_switched || data_generation_changed || layout_changed))
    PopulateEditor();
  return true;
}

bool DTMEditorDialog::RefreshWiiRuntimeMovie()
{
  auto& movie = Core::System::GetInstance().GetMovie();
  const auto metadata = movie.GetWiiRuntimeFrameMetadata();
  if (!metadata.has_value())
    return false;

  const Core::State core_state = Core::GetState(Core::System::GetInstance());
  const bool paused = core_state == Core::State::Paused;
  const bool frame_changed =
      !m_has_last_runtime_frame || metadata->current_frame != m_last_runtime_frame;
  const bool keep_manual_view =
      paused && !frame_changed && isActiveWindow() && m_using_runtime_movie &&
      m_runtime_movie_kind == EditorMovieKind::Wii;
  const bool should_follow = !paused || frame_changed || !isActiveWindow();
  const bool runtime_switched =
      !m_using_runtime_movie || m_runtime_movie_kind != EditorMovieKind::Wii;
  const bool data_generation_changed =
      metadata->data_generation != m_last_runtime_data_generation;
  const bool layout_changed = !m_model->HasWiiLayout(metadata->active_wiimotes);
  const bool row_count_changed =
      metadata->row_count != static_cast<u64>(m_model->rowCount());
  const bool live_row_changed =
      static_cast<int>(metadata->current_input_row) != m_last_runtime_row;
  const std::vector<int> previously_selected_rows =
      (!keep_manual_view && !should_follow) ? GetSelectedRows() : std::vector<int>{};
  const QModelIndex current = m_table->currentIndex();
  const int previous_row = current.isValid() ? current.row() : -1;
  if (!keep_manual_view)
  {
    if (runtime_switched || data_generation_changed || layout_changed || row_count_changed)
    {
      const auto snapshot = movie.GetWiiRuntimeFrameSnapshot();
      if (!snapshot.has_value())
        return false;

      if (runtime_switched || data_generation_changed || layout_changed ||
          snapshot->rows.size() < static_cast<size_t>(m_model->rowCount()))
      {
        m_model->SetWiiMovieData(snapshot->active_wiimotes, snapshot->rows);
      }
      else if (snapshot->rows.size() > static_cast<size_t>(m_model->rowCount()))
      {
        m_model->AppendWiiRows(snapshot->rows);
      }
    }
  }

  bool refreshed_selected_editor = false;
  if (metadata->row_count > 0 && metadata->current_input_row < metadata->row_count &&
      metadata->current_input_row < static_cast<u64>(m_model->rowCount()))
  {
    const int live_data_row = static_cast<int>(metadata->current_input_row);
    if (const auto row_data = movie.GetWiiRuntimeFrameRow(metadata->current_input_row))
    {
      m_model->ReplaceWiiRow(live_data_row, *row_data);
      if (current.isValid() && current.row() == live_data_row)
      {
        PopulateEditor();
        refreshed_selected_editor = true;
      }
    }
  }

  m_model->SetHighlightedRow(static_cast<int>(metadata->current_input_row));
  m_using_runtime_movie = true;
  m_runtime_movie_kind = EditorMovieKind::Wii;

  const bool selection_needs_update =
      !keep_manual_view &&
      (runtime_switched || data_generation_changed || layout_changed || row_count_changed ||
       (should_follow && live_row_changed));

  if (selection_needs_update && m_model->rowCount() > 0)
  {
    QItemSelectionModel* selection_model = m_table->selectionModel();
    QSignalBlocker blocker(selection_model);

    if (should_follow)
    {
      const int live_row =
          std::clamp(static_cast<int>(metadata->current_input_row), 0, m_model->rowCount() - 1);
      const QModelIndex idx = m_model->index(live_row, 1);
      selection_model->clearSelection();
      selection_model->setCurrentIndex(idx, QItemSelectionModel::NoUpdate);
      selection_model->select(idx, QItemSelectionModel::Select);
      const QRect visual_rect = m_table->visualRect(idx);
      if (!m_table->viewport()->rect().contains(visual_rect.center()))
        m_table->scrollTo(idx, QAbstractItemView::EnsureVisible);
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
        selection_model->select(m_model->index(row, 1), QItemSelectionModel::Select);
      }
    }
  }

  m_last_runtime_row = static_cast<int>(metadata->current_input_row);
  m_last_runtime_frame = metadata->current_frame;
  m_last_runtime_data_generation = metadata->data_generation;
  m_has_last_runtime_frame = true;
  if (runtime_switched)
    UpdateStatusLabel();
  if (!refreshed_selected_editor &&
      (selection_needs_update || runtime_switched || data_generation_changed || layout_changed))
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
  const QString game_frame_label = m_model->GetRowGameFrameLabel(row);
  const QString input_label = m_model->GetGCColumnLabel(controller);
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
  m_editor_box->setTitle(!game_frame_label.isEmpty() ?
                             tr("Input Frame %1, %2").arg(game_frame_label, input_label) :
                             tr("Input Row %1, %2").arg(row).arg(input_label));
  SetEditorEnabled(true);
}

void DTMEditorDialog::PopulateWiiEditor(int row)
{
  m_current_wii_row = m_model->GetWiiRow(row);
  m_current_wii_state_valid = false;
  const QString game_frame_label = m_model->GetRowGameFrameLabel(row);
  const auto set_classic_controls_enabled = [this](bool enabled) {
    for (QWidget* widget : {static_cast<QWidget*>(m_wii_classic_a),
                            static_cast<QWidget*>(m_wii_classic_b),
                            static_cast<QWidget*>(m_wii_classic_x),
                            static_cast<QWidget*>(m_wii_classic_y),
                            static_cast<QWidget*>(m_wii_classic_zl),
                            static_cast<QWidget*>(m_wii_classic_zr),
                            static_cast<QWidget*>(m_wii_classic_l),
                            static_cast<QWidget*>(m_wii_classic_r),
                            static_cast<QWidget*>(m_wii_classic_minus),
                            static_cast<QWidget*>(m_wii_classic_plus),
                            static_cast<QWidget*>(m_wii_classic_home),
                            static_cast<QWidget*>(m_wii_classic_up),
                            static_cast<QWidget*>(m_wii_classic_down),
                            static_cast<QWidget*>(m_wii_classic_left),
                            static_cast<QWidget*>(m_wii_classic_right),
                            static_cast<QWidget*>(m_wii_classic_left_x),
                            static_cast<QWidget*>(m_wii_classic_left_y),
                            static_cast<QWidget*>(m_wii_classic_right_x),
                            static_cast<QWidget*>(m_wii_classic_right_y),
                            static_cast<QWidget*>(m_wii_classic_l_trigger),
                            static_cast<QWidget*>(m_wii_classic_r_trigger)})
    {
      widget->setEnabled(enabled);
    }
  };
  const auto clear_classic_controls = [this] {
    for (QCheckBox* box :
         {m_wii_classic_a, m_wii_classic_b, m_wii_classic_x, m_wii_classic_y,
          m_wii_classic_zl, m_wii_classic_zr, m_wii_classic_l, m_wii_classic_r,
          m_wii_classic_minus, m_wii_classic_plus, m_wii_classic_home, m_wii_classic_up,
          m_wii_classic_down, m_wii_classic_left, m_wii_classic_right})
    {
      box->setChecked(false);
    }
    m_wii_classic_left_x->setValue(WiimoteEmu::Classic::LEFT_STICK_CENTER);
    m_wii_classic_left_y->setValue(WiimoteEmu::Classic::LEFT_STICK_CENTER);
    m_wii_classic_right_x->setValue(WiimoteEmu::Classic::RIGHT_STICK_CENTER);
    m_wii_classic_right_y->setValue(WiimoteEmu::Classic::RIGHT_STICK_CENTER);
    m_wii_classic_l_trigger->setValue(0);
    m_wii_classic_r_trigger->setValue(0);
  };
  const auto populate_classic_controls = [this](const WiimoteEmu::Classic::DataFormat& classic) {
    const u16 buttons = classic.GetButtons();
    m_wii_classic_a->setChecked((buttons & WiimoteEmu::Classic::BUTTON_A) != 0);
    m_wii_classic_b->setChecked((buttons & WiimoteEmu::Classic::BUTTON_B) != 0);
    m_wii_classic_x->setChecked((buttons & WiimoteEmu::Classic::BUTTON_X) != 0);
    m_wii_classic_y->setChecked((buttons & WiimoteEmu::Classic::BUTTON_Y) != 0);
    m_wii_classic_zl->setChecked((buttons & WiimoteEmu::Classic::BUTTON_ZL) != 0);
    m_wii_classic_zr->setChecked((buttons & WiimoteEmu::Classic::BUTTON_ZR) != 0);
    m_wii_classic_l->setChecked((buttons & WiimoteEmu::Classic::TRIGGER_L) != 0);
    m_wii_classic_r->setChecked((buttons & WiimoteEmu::Classic::TRIGGER_R) != 0);
    m_wii_classic_minus->setChecked((buttons & WiimoteEmu::Classic::BUTTON_MINUS) != 0);
    m_wii_classic_plus->setChecked((buttons & WiimoteEmu::Classic::BUTTON_PLUS) != 0);
    m_wii_classic_home->setChecked((buttons & WiimoteEmu::Classic::BUTTON_HOME) != 0);
    m_wii_classic_up->setChecked((buttons & WiimoteEmu::Classic::PAD_UP) != 0);
    m_wii_classic_down->setChecked((buttons & WiimoteEmu::Classic::PAD_DOWN) != 0);
    m_wii_classic_left->setChecked((buttons & WiimoteEmu::Classic::PAD_LEFT) != 0);
    m_wii_classic_right->setChecked((buttons & WiimoteEmu::Classic::PAD_RIGHT) != 0);
    const auto left_stick = classic.GetLeftStick().value;
    const auto right_stick = classic.GetRightStick().value;
    m_wii_classic_left_x->setValue(left_stick.x);
    m_wii_classic_left_y->setValue(left_stick.y);
    m_wii_classic_right_x->setValue(right_stick.x);
    m_wii_classic_right_y->setValue(right_stick.y);
    m_wii_classic_l_trigger->setValue(classic.GetLeftTrigger().value);
    m_wii_classic_r_trigger->setValue(classic.GetRightTrigger().value);
  };

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
    else if (std::holds_alternative<WiimoteEmu::Classic::DataFormat>(
                 m_current_wii_state.extension.data))
    {
      auto classic = std::get<WiimoteEmu::Classic::DataFormat>(m_current_wii_state.extension.data);
      classic.SetButtons(0);
      classic.SetLeftStick({WiimoteEmu::Classic::LEFT_STICK_CENTER,
                            WiimoteEmu::Classic::LEFT_STICK_CENTER});
      classic.SetRightStick({WiimoteEmu::Classic::RIGHT_STICK_CENTER,
                             WiimoteEmu::Classic::RIGHT_STICK_CENTER});
      classic.SetLeftTrigger(0);
      classic.SetRightTrigger(0);
      m_current_wii_state.extension.data = classic;
    }

    m_current_wii_state_valid = true;
    const bool has_nunchuk = std::holds_alternative<WiimoteEmu::Nunchuk::DataFormat>(
        m_current_wii_state.extension.data);
    const bool has_classic = std::holds_alternative<WiimoteEmu::Classic::DataFormat>(
        m_current_wii_state.extension.data);
    m_wii_extension_label->setText(has_nunchuk ? tr("Nunchuk") :
                                   has_classic ? tr("Classic Controller / WiiVC N64") :
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
    for (size_t i = 0; i < m_wii_ir_visible.size(); ++i)
    {
      m_wii_ir_visible[i]->setChecked(false);
      m_wii_ir_x[i]->setValue(0);
      m_wii_ir_y[i]->setValue(0);
      m_wii_ir_size[i]->setValue(0);
    }
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
    clear_classic_controls();
    m_updating_editor = false;
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
    set_classic_controls_enabled(has_classic);
    m_editor_box->setTitle(!game_frame_label.isEmpty() ?
                               tr("Wii Input Frame %1 (Reset Marker)").arg(game_frame_label) :
                               tr("Wii Input Row %1 (Reset Marker)").arg(row));
    SetEditorEnabled(true);
    return;
  }

  if (!WiimoteEmu::DeserializeDesiredState(&m_current_wii_state, m_current_wii_row.serialized_state))
  {
    m_wii_extension_label->setText(tr("Invalid"));
    m_wii_motion_plus_label->setText(tr("Invalid"));
    m_updating_editor = false;
    m_editor_box->setTitle(!game_frame_label.isEmpty() ?
                               tr("Wii Input Frame %1 (Invalid)").arg(game_frame_label) :
                               tr("Wii Input Row %1 (Invalid)").arg(row));
    SetEditorEnabled(false);
    return;
  }

  m_current_wii_state_valid = true;
  const bool has_nunchuk = std::holds_alternative<WiimoteEmu::Nunchuk::DataFormat>(m_current_wii_state.extension.data);
  const bool has_classic = std::holds_alternative<WiimoteEmu::Classic::DataFormat>(m_current_wii_state.extension.data);
  const bool has_motion_plus = m_current_wii_state.motion_plus.has_value();
  m_wii_extension_label->setText(has_nunchuk ? tr("Nunchuk") :
                                 has_classic ? tr("Classic Controller / WiiVC N64") :
                                               tr("None / Other"));
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
  for (size_t i = 0; i < m_wii_ir_visible.size(); ++i)
  {
    const auto& point = m_current_wii_state.camera_points[i];
    const bool visible = IsVisibleIRPoint(point);
    m_wii_ir_visible[i]->setChecked(visible);
    m_wii_ir_x[i]->setValue(visible ? point.position.x : 0);
    m_wii_ir_y[i]->setValue(visible ? point.position.y : 0);
    m_wii_ir_size[i]->setValue(visible ? point.size : 0);
  }

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
  if (has_classic)
  {
    populate_classic_controls(
        std::get<WiimoteEmu::Classic::DataFormat>(m_current_wii_state.extension.data));
  }
  else
  {
    clear_classic_controls();
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
  set_classic_controls_enabled(has_classic);

  m_editor_box->setTitle(!game_frame_label.isEmpty() ? tr("Wii Input Frame %1").arg(game_frame_label) :
                                                    tr("Wii Input Row %1").arg(row));
  SetEditorEnabled(true);
}

std::vector<DTMEditorDialog::InputCell> DTMEditorDialog::GetSelectedInputCells() const
{
  std::vector<InputCell> cells;
  if (const auto* selection_model = m_table->selectionModel())
  {
    const auto selected = selection_model->selectedIndexes();
    cells.reserve(selected.size());
    for (const QModelIndex& index : selected)
    {
      if (m_model->GetKind() == ModelMovieKind::GC)
      {
        if (!m_model->IsGCControllerColumn(index.column()))
          continue;
      }
      else if (m_model->GetKind() == ModelMovieKind::Wii)
      {
        if (index.column() != 1)
          continue;
      }
      else
      {
        continue;
      }

      cells.push_back({index.row(), index.column()});
    }
  }

  if (cells.empty())
  {
    if (const QModelIndex current = m_table->currentIndex(); current.isValid())
    {
      if ((m_model->GetKind() == ModelMovieKind::GC &&
           m_model->IsGCControllerColumn(current.column())) ||
          (m_model->GetKind() == ModelMovieKind::Wii && current.column() == 1))
      {
        cells.push_back({current.row(), current.column()});
      }
    }
  }

  std::sort(cells.begin(), cells.end(), [](const InputCell& lhs, const InputCell& rhs) {
    return std::tie(lhs.row, lhs.column) < std::tie(rhs.row, rhs.column);
  });
  cells.erase(std::unique(cells.begin(), cells.end(), [](const InputCell& lhs, const InputCell& rhs) {
                return lhs.row == rhs.row && lhs.column == rhs.column;
              }),
              cells.end());
  return cells;
}

std::vector<int> DTMEditorDialog::GetSelectedRows() const
{
  const std::vector<InputCell> cells = GetSelectedInputCells();
  std::vector<int> rows;
  rows.reserve(cells.size());
  for (const InputCell& cell : cells)
    rows.push_back(cell.row);

  std::sort(rows.begin(), rows.end());
  rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
  return rows;
}

bool DTMEditorDialog::IsInputCellSelected(const std::vector<InputCell>& cells, int row,
                                          int column) const
{
  return std::ranges::any_of(cells, [row, column](const InputCell& cell) {
    return cell.row == row && cell.column == column;
  });
}

int DTMEditorDialog::GetPrimaryDataColumn() const
{
  return m_model->GetKind() == ModelMovieKind::Wii ? 1 :
         std::max(1, m_model->GetFirstGCControllerColumn());
}

bool DTMEditorDialog::HasCopiedInputs() const
{
  return (m_copied_movie_kind == EditorMovieKind::GC && !m_copied_gc_rows.empty()) ||
         (m_copied_movie_kind == EditorMovieKind::Wii && !m_copied_wii_rows.empty());
}

bool DTMEditorDialog::HasCompatibleCopiedInputs() const
{
  if (!HasCopiedInputs())
    return false;

  return (m_copied_movie_kind == EditorMovieKind::GC && m_model->GetKind() == ModelMovieKind::GC) ||
         (m_copied_movie_kind == EditorMovieKind::Wii && m_model->GetKind() == ModelMovieKind::Wii);
}

void DTMEditorDialog::SelectRows(const std::vector<int>& rows, int current_row)
{
  if (!m_table || !m_model || m_model->rowCount() == 0)
    return;

  std::vector<int> valid_rows = rows;
  valid_rows.erase(std::remove_if(valid_rows.begin(), valid_rows.end(), [this](int row) {
                   return row < 0 || row >= m_model->rowCount();
                 }),
                 valid_rows.end());
  std::sort(valid_rows.begin(), valid_rows.end());
  valid_rows.erase(std::unique(valid_rows.begin(), valid_rows.end()), valid_rows.end());

  if (valid_rows.empty())
    return;

  const int current = std::clamp(current_row >= 0 ? current_row : valid_rows.front(), 0,
                                 m_model->rowCount() - 1);
  if (QItemSelectionModel* selection_model = m_table->selectionModel())
  {
    QSignalBlocker blocker(selection_model);
    selection_model->clearSelection();
    for (const int row : valid_rows)
    {
      selection_model->select(m_model->index(row, 0),
                              QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }
    selection_model->setCurrentIndex(m_model->index(current, GetPrimaryDataColumn()),
                                     QItemSelectionModel::NoUpdate);
  }

  m_table->setFocus(Qt::OtherFocusReason);
  m_table->scrollTo(m_model->index(current, GetPrimaryDataColumn()), QAbstractItemView::EnsureVisible);
  m_table->viewport()->update();
  m_table->update();
}

void DTMEditorDialog::SelectCells(const std::vector<InputCell>& cells, int current_row,
                                  int current_column)
{
  if (!m_table || !m_model || m_model->rowCount() == 0)
    return;

  std::vector<InputCell> valid_cells = cells;
  valid_cells.erase(std::remove_if(valid_cells.begin(), valid_cells.end(), [this](const InputCell& cell) {
                      if (cell.row < 0 || cell.row >= m_model->rowCount())
                        return true;
                      if (m_model->GetKind() == ModelMovieKind::GC)
                        return !m_model->IsGCControllerColumn(cell.column);
                      return m_model->GetKind() != ModelMovieKind::Wii || cell.column != 1;
                    }),
                    valid_cells.end());
  std::sort(valid_cells.begin(), valid_cells.end(), [](const InputCell& lhs, const InputCell& rhs) {
    return std::tie(lhs.row, lhs.column) < std::tie(rhs.row, rhs.column);
  });
  valid_cells.erase(std::unique(valid_cells.begin(), valid_cells.end(),
                                [](const InputCell& lhs, const InputCell& rhs) {
                                  return lhs.row == rhs.row && lhs.column == rhs.column;
                                }),
                    valid_cells.end());

  if (valid_cells.empty())
    return;

  int current = current_row >= 0 ? current_row : valid_cells.front().row;
  current = std::clamp(current, 0, m_model->rowCount() - 1);
  int current_col = current_column;
  if ((m_model->GetKind() == ModelMovieKind::GC &&
       !m_model->IsGCControllerColumn(current_col)) ||
      (m_model->GetKind() == ModelMovieKind::Wii && current_col != 1))
  {
    current_col = valid_cells.front().column;
  }

  if (QItemSelectionModel* selection_model = m_table->selectionModel())
  {
    QSignalBlocker blocker(selection_model);
    selection_model->clearSelection();
    for (const InputCell& cell : valid_cells)
      selection_model->select(m_model->index(cell.row, cell.column), QItemSelectionModel::Select);
    selection_model->setCurrentIndex(m_model->index(current, current_col),
                                     QItemSelectionModel::NoUpdate);
  }

  m_table->setFocus(Qt::OtherFocusReason);
  m_table->scrollTo(m_model->index(current, current_col), QAbstractItemView::EnsureVisible);
  m_table->viewport()->update();
  m_table->update();
}

void DTMEditorDialog::RefreshEditedRows(const std::vector<int>& rows)
{
  if (!m_table || !m_model)
    return;

  std::vector<int> valid_rows = rows;
  valid_rows.erase(std::remove_if(valid_rows.begin(), valid_rows.end(), [this](const int row) {
                     return row < 0 || row >= m_model->rowCount();
                   }),
                   valid_rows.end());
  std::sort(valid_rows.begin(), valid_rows.end());
  valid_rows.erase(std::unique(valid_rows.begin(), valid_rows.end()), valid_rows.end());

  if (!m_using_runtime_movie || valid_rows.empty())
  {
    m_table->viewport()->update();
    m_table->update();
    return;
  }

  auto& movie = Core::System::GetInstance().GetMovie();
  if (m_runtime_movie_kind == EditorMovieKind::GC && m_model->GetKind() == ModelMovieKind::GC)
  {
    for (const int row : valid_rows)
    {
      if (const auto row_data = movie.GetGCRuntimeFrameRow(static_cast<u64>(row)))
        m_model->ReplaceGCRow(row, *row_data);
    }

    if (const auto metadata = movie.GetGCRuntimeFrameMetadata())
    {
      m_model->SetHighlightedRow(static_cast<int>(metadata->current_input_row));
      m_last_runtime_row = static_cast<int>(metadata->current_input_row);
      m_last_runtime_frame = metadata->current_frame;
      m_last_runtime_data_generation = metadata->data_generation;
      m_has_last_runtime_frame = true;
    }
  }
  else if (m_runtime_movie_kind == EditorMovieKind::Wii && m_model->GetKind() == ModelMovieKind::Wii)
  {
    const int current_row = m_table->currentIndex().isValid() ? m_table->currentIndex().row() : -1;
    for (const int row : valid_rows)
    {
      if (const auto row_data = movie.GetWiiRuntimeFrameRow(static_cast<u64>(row)))
      {
        m_model->ReplaceWiiRow(row, *row_data);
        if (row == current_row)
          m_current_wii_row = *row_data;
      }
    }

    if (const auto metadata = movie.GetWiiRuntimeFrameMetadata())
    {
      m_model->SetHighlightedRow(static_cast<int>(metadata->current_input_row));
      m_last_runtime_row = static_cast<int>(metadata->current_input_row);
      m_last_runtime_frame = metadata->current_frame;
      m_last_runtime_data_generation = metadata->data_generation;
      m_has_last_runtime_frame = true;
    }
  }

  m_table->viewport()->update();
  m_table->update();
}

void DTMEditorDialog::RegisterInputField(QWidget* widget, const InputField field)
{
  if (!widget)
    return;

  const int value = static_cast<int>(field);
  widget->setProperty("dtm_input_field", value);
  widget->installEventFilter(this);
  m_input_field_widgets.push_back(widget);
  for (QWidget* child : widget->findChildren<QWidget*>())
  {
    child->setProperty("dtm_input_field", value);
    child->installEventFilter(this);
  }
}

void DTMEditorDialog::ToggleSelectedInputField(const InputField field)
{
  if (m_selected_input_fields.contains(field))
    m_selected_input_fields.erase(field);
  else
    m_selected_input_fields.insert(field);
  UpdateInputFieldSelectionStyles();
  UpdateStatusLabel();
}

bool DTMEditorDialog::IsInputFieldFilterActive() const
{
  return m_select_buttons && m_select_buttons->isChecked() && !m_selected_input_fields.empty();
}

std::set<DTMEditorDialog::InputField> DTMEditorDialog::ActiveInputFieldFilter() const
{
  return IsInputFieldFilterActive() ? m_selected_input_fields : std::set<InputField>{};
}

void DTMEditorDialog::UpdateInputFieldSelectionStyles()
{
  const bool selecting = m_select_buttons && m_select_buttons->isChecked();
  for (QWidget* widget : m_input_field_widgets)
  {
    if (!widget)
      continue;

    if (!selecting)
    {
      widget->setStyleSheet(QString());
      continue;
    }

    const QVariant field_value = widget->property("dtm_input_field");
    const bool selected =
        field_value.isValid() &&
        m_selected_input_fields.contains(static_cast<InputField>(field_value.toInt()));
    widget->setStyleSheet(selected ? QStringLiteral("background-color: rgba(80, 160, 255, 70);") :
                                     QStringLiteral("color: palette(mid);"));
  }
}

Movie::ControllerState DTMEditorDialog::CopyGCInputFields(
    const Movie::ControllerState& destination, const Movie::ControllerState& source,
    const std::set<InputField>& fields) const
{
  if (fields.empty())
    return source;

  Movie::ControllerState updated = destination;
  const auto has = [&fields](InputField field) { return fields.contains(field); };
  if (has(InputField::GCConnected))
    updated.is_connected = source.is_connected;
  if (has(InputField::GCStart))
    updated.Start = source.Start;
  if (has(InputField::GCA))
    updated.A = source.A;
  if (has(InputField::GCB))
    updated.B = source.B;
  if (has(InputField::GCX))
    updated.X = source.X;
  if (has(InputField::GCY))
    updated.Y = source.Y;
  if (has(InputField::GCZ))
    updated.Z = source.Z;
  if (has(InputField::GCL))
    updated.L = source.L;
  if (has(InputField::GCR))
    updated.R = source.R;
  if (has(InputField::GCUp))
    updated.DPadUp = source.DPadUp;
  if (has(InputField::GCDown))
    updated.DPadDown = source.DPadDown;
  if (has(InputField::GCLeft))
    updated.DPadLeft = source.DPadLeft;
  if (has(InputField::GCRight))
    updated.DPadRight = source.DPadRight;
  if (has(InputField::GCDisc))
    updated.disc = source.disc;
  if (has(InputField::GCReset))
    updated.reset = source.reset;
  if (has(InputField::GCGetOrigin))
    updated.get_origin = source.get_origin;
  if (has(InputField::GCTriggerL))
    updated.TriggerL = source.TriggerL;
  if (has(InputField::GCTriggerR))
    updated.TriggerR = source.TriggerR;
  if (has(InputField::GCStickX))
    updated.AnalogStickX = source.AnalogStickX;
  if (has(InputField::GCStickY))
    updated.AnalogStickY = source.AnalogStickY;
  if (has(InputField::GCCStickX))
    updated.CStickX = source.CStickX;
  if (has(InputField::GCCStickY))
    updated.CStickY = source.CStickY;
  return updated;
}

Movie::ControllerState DTMEditorDialog::NeutralizeGCInputFields(
    const Movie::ControllerState& state, const std::set<InputField>& fields) const
{
  return CopyGCInputFields(state, MakeNeutralGCStateLike(state), fields);
}

Movie::WiiRuntimeInputRow DTMEditorDialog::CopyWiiInputFields(
    const Movie::WiiRuntimeInputRow& destination, const Movie::WiiRuntimeInputRow& source,
    const std::set<InputField>& fields) const
{
  if (fields.empty())
    return source;

  const auto has = [&fields](InputField field) { return fields.contains(field); };
  Movie::WiiRuntimeInputRow updated = destination;
  if (has(InputField::WiiReset))
  {
    updated.is_reset = source.is_reset;
    if (source.is_reset)
    {
      updated.serialized_state = {};
      return updated;
    }
    if (!HasValidWiiSerializedState(updated))
      updated.serialized_state = source.serialized_state;
  }

  if (updated.is_reset || source.is_reset || !HasValidWiiSerializedState(updated) ||
      !HasValidWiiSerializedState(source))
  {
    return updated;
  }

  WiimoteEmu::DesiredWiimoteState destination_state;
  WiimoteEmu::DesiredWiimoteState source_state;
  if (!WiimoteEmu::DeserializeDesiredState(&destination_state, updated.serialized_state) ||
      !WiimoteEmu::DeserializeDesiredState(&source_state, source.serialized_state))
  {
    return destination;
  }

  if (has(InputField::WiiA))
    destination_state.buttons.a = source_state.buttons.a;
  if (has(InputField::WiiB))
    destination_state.buttons.b = source_state.buttons.b;
  if (has(InputField::WiiOne))
    destination_state.buttons.one = source_state.buttons.one;
  if (has(InputField::WiiTwo))
    destination_state.buttons.two = source_state.buttons.two;
  if (has(InputField::WiiPlus))
    destination_state.buttons.plus = source_state.buttons.plus;
  if (has(InputField::WiiMinus))
    destination_state.buttons.minus = source_state.buttons.minus;
  if (has(InputField::WiiHome))
    destination_state.buttons.home = source_state.buttons.home;
  if (has(InputField::WiiUp))
    destination_state.buttons.up = source_state.buttons.up;
  if (has(InputField::WiiDown))
    destination_state.buttons.down = source_state.buttons.down;
  if (has(InputField::WiiLeft))
    destination_state.buttons.left = source_state.buttons.left;
  if (has(InputField::WiiRight))
    destination_state.buttons.right = source_state.buttons.right;
  if (has(InputField::WiiAccelX))
    destination_state.acceleration.value.x = source_state.acceleration.value.x;
  if (has(InputField::WiiAccelY))
    destination_state.acceleration.value.y = source_state.acceleration.value.y;
  if (has(InputField::WiiAccelZ))
    destination_state.acceleration.value.z = source_state.acceleration.value.z;
  if (has(InputField::WiiBattery))
    destination_state.battery = source_state.battery;

  const std::array<std::array<InputField, 4>, 4> ir_fields = {{
      {InputField::WiiIR1Visible, InputField::WiiIR1X, InputField::WiiIR1Y,
       InputField::WiiIR1Size},
      {InputField::WiiIR2Visible, InputField::WiiIR2X, InputField::WiiIR2Y,
       InputField::WiiIR2Size},
      {InputField::WiiIR3Visible, InputField::WiiIR3X, InputField::WiiIR3Y,
       InputField::WiiIR3Size},
      {InputField::WiiIR4Visible, InputField::WiiIR4X, InputField::WiiIR4Y,
       InputField::WiiIR4Size},
  }};
  for (size_t i = 0; i < destination_state.camera_points.size(); ++i)
  {
    WiimoteEmu::CameraPoint point = destination_state.camera_points[i];
    if (has(ir_fields[i][0]))
    {
      point = source_state.camera_points[i];
      destination_state.camera_points[i] = point;
      continue;
    }

    if (IsVisibleIRPoint(point))
    {
      if (has(ir_fields[i][1]))
        point.position.x = source_state.camera_points[i].position.x;
      if (has(ir_fields[i][2]))
        point.position.y = source_state.camera_points[i].position.y;
      if (has(ir_fields[i][3]))
        point.size = source_state.camera_points[i].size;
    }
    destination_state.camera_points[i] = point;
  }

  if (destination_state.motion_plus.has_value() && source_state.motion_plus.has_value())
  {
    if (has(InputField::WiiGyroX))
      destination_state.motion_plus->gyro.value.x = source_state.motion_plus->gyro.value.x;
    if (has(InputField::WiiGyroY))
      destination_state.motion_plus->gyro.value.y = source_state.motion_plus->gyro.value.y;
    if (has(InputField::WiiGyroZ))
      destination_state.motion_plus->gyro.value.z = source_state.motion_plus->gyro.value.z;
    if (has(InputField::WiiGyroSlowX))
      destination_state.motion_plus->is_slow.x = source_state.motion_plus->is_slow.x;
    if (has(InputField::WiiGyroSlowY))
      destination_state.motion_plus->is_slow.y = source_state.motion_plus->is_slow.y;
    if (has(InputField::WiiGyroSlowZ))
      destination_state.motion_plus->is_slow.z = source_state.motion_plus->is_slow.z;
  }

  if (std::holds_alternative<WiimoteEmu::Nunchuk::DataFormat>(destination_state.extension.data) &&
      std::holds_alternative<WiimoteEmu::Nunchuk::DataFormat>(source_state.extension.data))
  {
    auto destination_nunchuk =
        std::get<WiimoteEmu::Nunchuk::DataFormat>(destination_state.extension.data);
    const auto& source_nunchuk =
        std::get<WiimoteEmu::Nunchuk::DataFormat>(source_state.extension.data);
    u8 buttons = destination_nunchuk.GetButtons();
    if (has(InputField::WiiNunchukC))
      buttons = source_nunchuk.GetButtons() & WiimoteEmu::Nunchuk::BUTTON_C ?
                    buttons | WiimoteEmu::Nunchuk::BUTTON_C :
                    buttons & ~WiimoteEmu::Nunchuk::BUTTON_C;
    if (has(InputField::WiiNunchukZ))
      buttons = source_nunchuk.GetButtons() & WiimoteEmu::Nunchuk::BUTTON_Z ?
                    buttons | WiimoteEmu::Nunchuk::BUTTON_Z :
                    buttons & ~WiimoteEmu::Nunchuk::BUTTON_Z;
    destination_nunchuk.SetButtons(buttons);
    if (has(InputField::WiiNunchukStickX))
      destination_nunchuk.jx = source_nunchuk.GetStick().value.x;
    if (has(InputField::WiiNunchukStickY))
      destination_nunchuk.jy = source_nunchuk.GetStick().value.y;
    if (has(InputField::WiiNunchukAccelX))
      destination_nunchuk.SetAccelX(source_nunchuk.GetAccel().value.x);
    if (has(InputField::WiiNunchukAccelY))
      destination_nunchuk.SetAccelY(source_nunchuk.GetAccel().value.y);
    if (has(InputField::WiiNunchukAccelZ))
      destination_nunchuk.SetAccelZ(source_nunchuk.GetAccel().value.z);
    destination_state.extension.data = destination_nunchuk;
  }
  else if (std::holds_alternative<WiimoteEmu::Classic::DataFormat>(
               destination_state.extension.data) &&
           std::holds_alternative<WiimoteEmu::Classic::DataFormat>(source_state.extension.data))
  {
    auto destination_classic =
        std::get<WiimoteEmu::Classic::DataFormat>(destination_state.extension.data);
    const auto& source_classic =
        std::get<WiimoteEmu::Classic::DataFormat>(source_state.extension.data);
    u16 buttons = destination_classic.GetButtons();
    const auto copy_classic_button = [&](InputField field, u16 bit) {
      if (!has(field))
        return;
      buttons = source_classic.GetButtons() & bit ? buttons | bit : buttons & ~bit;
    };
    copy_classic_button(InputField::WiiClassicA, WiimoteEmu::Classic::BUTTON_A);
    copy_classic_button(InputField::WiiClassicB, WiimoteEmu::Classic::BUTTON_B);
    copy_classic_button(InputField::WiiClassicX, WiimoteEmu::Classic::BUTTON_X);
    copy_classic_button(InputField::WiiClassicY, WiimoteEmu::Classic::BUTTON_Y);
    copy_classic_button(InputField::WiiClassicZL, WiimoteEmu::Classic::BUTTON_ZL);
    copy_classic_button(InputField::WiiClassicZR, WiimoteEmu::Classic::BUTTON_ZR);
    copy_classic_button(InputField::WiiClassicL, WiimoteEmu::Classic::TRIGGER_L);
    copy_classic_button(InputField::WiiClassicR, WiimoteEmu::Classic::TRIGGER_R);
    copy_classic_button(InputField::WiiClassicMinus, WiimoteEmu::Classic::BUTTON_MINUS);
    copy_classic_button(InputField::WiiClassicPlus, WiimoteEmu::Classic::BUTTON_PLUS);
    copy_classic_button(InputField::WiiClassicHome, WiimoteEmu::Classic::BUTTON_HOME);
    copy_classic_button(InputField::WiiClassicUp, WiimoteEmu::Classic::PAD_UP);
    copy_classic_button(InputField::WiiClassicDown, WiimoteEmu::Classic::PAD_DOWN);
    copy_classic_button(InputField::WiiClassicLeft, WiimoteEmu::Classic::PAD_LEFT);
    copy_classic_button(InputField::WiiClassicRight, WiimoteEmu::Classic::PAD_RIGHT);
    destination_classic.SetButtons(buttons);
    auto left_stick = destination_classic.GetLeftStick().value;
    auto right_stick = destination_classic.GetRightStick().value;
    if (has(InputField::WiiClassicLeftX))
      left_stick.x = source_classic.GetLeftStick().value.x;
    if (has(InputField::WiiClassicLeftY))
      left_stick.y = source_classic.GetLeftStick().value.y;
    if (has(InputField::WiiClassicRightX))
      right_stick.x = source_classic.GetRightStick().value.x;
    if (has(InputField::WiiClassicRightY))
      right_stick.y = source_classic.GetRightStick().value.y;
    destination_classic.SetLeftStick(left_stick);
    destination_classic.SetRightStick(right_stick);
    if (has(InputField::WiiClassicTriggerL))
      destination_classic.SetLeftTrigger(source_classic.GetLeftTrigger().value);
    if (has(InputField::WiiClassicTriggerR))
      destination_classic.SetRightTrigger(source_classic.GetRightTrigger().value);
    destination_state.extension.data = destination_classic;
  }

  updated.serialized_state = WiimoteEmu::SerializeDesiredState(destination_state);
  return updated;
}

Movie::WiiRuntimeInputRow DTMEditorDialog::NeutralizeWiiInputFields(
    const Movie::WiiRuntimeInputRow& row, const std::set<InputField>& fields) const
{
  return CopyWiiInputFields(row, MakeNeutralWiiRowLike(row), fields);
}

void DTMEditorDialog::CopySelectedInputs()
{
  if (m_select_buttons && m_select_buttons->isChecked() && m_selected_input_fields.empty())
    return;

  const std::vector<InputCell> cells = GetSelectedInputCells();
  if (cells.empty())
    return;

  m_copied_input_fields = ActiveInputFieldFilter();

  if (m_model->GetKind() == ModelMovieKind::GC)
  {
    std::vector<int> rows;
    for (const InputCell& cell : cells)
      rows.push_back(cell.row);
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());

    m_copied_gc_rows.clear();
    m_copied_gc_cells.clear();
    m_copied_gc_controllers = {};
    m_copied_gc_rows.reserve(rows.size());
    m_copied_gc_cells.reserve(rows.size());
    for (const int row : rows)
    {
      std::array<Movie::ControllerState, 4> copied_row{};
      std::array<bool, 4> copied_cells{};
      for (int controller = 0; controller < 4; ++controller)
      {
        if (!m_model->IsGCControllerActive(controller))
          continue;

        int column = -1;
        for (int candidate = 1; candidate < m_model->columnCount(); ++candidate)
        {
          int mapped = -1;
          if (m_model->IsGCControllerColumn(candidate, &mapped) && mapped == controller)
          {
            column = candidate;
            break;
          }
        }
        if (column < 0 || !IsInputCellSelected(cells, row, column))
          continue;

        copied_row[static_cast<size_t>(controller)] = m_model->GetGCState(row, controller);
        copied_cells[static_cast<size_t>(controller)] = true;
        m_copied_gc_controllers[static_cast<size_t>(controller)] = true;
      }
      m_copied_gc_rows.push_back(copied_row);
      m_copied_gc_cells.push_back(copied_cells);
    }
    m_copied_wii_rows.clear();
    m_copied_wii_classic_only = false;
    m_copied_movie_kind = EditorMovieKind::GC;
  }
  else if (m_model->GetKind() == ModelMovieKind::Wii)
  {
    const std::vector<int> rows = GetSelectedRows();
    m_copied_wii_rows.clear();
    m_copied_wii_rows.reserve(rows.size());
    for (const int row : rows)
      m_copied_wii_rows.push_back(m_model->GetWiiRow(row));
    m_copied_gc_rows.clear();
    m_copied_gc_cells.clear();
    m_copied_gc_controllers = {};
    m_copied_wii_classic_only = m_copied_input_fields.empty() && m_wii_hide_remote_inputs &&
                                m_wii_hide_remote_inputs->isChecked();
    m_copied_movie_kind = EditorMovieKind::Wii;
  }
  else
  {
    return;
  }

  UpdateStatusLabel();
}

bool DTMEditorDialog::PasteCopiedInputs(int start_row)
{
  if (!HasCompatibleCopiedInputs() || start_row < 0 || start_row >= m_model->rowCount())
    return false;

  auto& movie = Core::System::GetInstance().GetMovie();
  std::vector<int> pasted_rows;
  std::vector<InputCell> pasted_cells;
  if (m_model->GetKind() == ModelMovieKind::GC)
  {
    std::vector<int> source_controllers;
    for (int controller = 0; controller < 4; ++controller)
    {
      if (m_copied_gc_controllers[static_cast<size_t>(controller)])
        source_controllers.push_back(controller);
    }
    if (source_controllers.empty())
      return false;

    std::vector<int> destination_controllers;
    const std::vector<InputCell> selected_destination_cells = GetSelectedInputCells();
    for (const InputCell& cell : selected_destination_cells)
    {
      int controller = -1;
      if (m_model->IsGCControllerColumn(cell.column, &controller) &&
          std::find(destination_controllers.begin(), destination_controllers.end(), controller) ==
              destination_controllers.end())
      {
        destination_controllers.push_back(controller);
      }
    }
    if (destination_controllers.empty())
    {
      int controller = -1;
      if (const QModelIndex current = m_table->currentIndex();
          current.isValid() && m_model->IsGCControllerColumn(current.column(), &controller))
      {
        destination_controllers.push_back(controller);
      }
    }
    if (destination_controllers.empty())
      destination_controllers = source_controllers;
    else if (static_cast<int>(destination_controllers.size()) < static_cast<int>(source_controllers.size()))
      destination_controllers = source_controllers;
    std::sort(destination_controllers.begin(), destination_controllers.end());

    const int rows_to_paste =
        std::min(static_cast<int>(m_copied_gc_rows.size()), m_model->rowCount() - start_row);
    for (int i = 0; i < rows_to_paste; ++i)
    {
      const int row = start_row + i;
      const auto& copied_row = m_copied_gc_rows[static_cast<size_t>(i)];
      const auto& copied_cells = m_copied_gc_cells[static_cast<size_t>(i)];
      for (size_t mapping = 0; mapping < source_controllers.size() &&
                             mapping < destination_controllers.size();
           ++mapping)
      {
        const int source_controller = source_controllers[mapping];
        const int controller = destination_controllers[mapping];
        if (!m_model->IsGCControllerActive(controller) ||
            !copied_cells[static_cast<size_t>(source_controller)])
          continue;

        if (m_using_runtime_movie && m_runtime_movie_kind == EditorMovieKind::GC &&
            !movie.SetGCRuntimeFrameState(
                row, controller,
                CopyGCInputFields(m_model->GetGCState(row, controller),
                                  copied_row[static_cast<size_t>(source_controller)],
                                  m_copied_input_fields)))
        {
          continue;
        }

        m_model->SetGCState(
            row, controller,
            CopyGCInputFields(m_model->GetGCState(row, controller),
                              copied_row[static_cast<size_t>(source_controller)],
                              m_copied_input_fields));
        for (int candidate = 1; candidate < m_model->columnCount(); ++candidate)
        {
          int mapped = -1;
          if (m_model->IsGCControllerColumn(candidate, &mapped) && mapped == controller)
          {
            pasted_cells.push_back({row, candidate});
            break;
          }
        }
      }
      pasted_rows.push_back(row);
    }
  }
  else if (m_model->GetKind() == ModelMovieKind::Wii)
  {
    const int rows_to_paste =
        std::min(static_cast<int>(m_copied_wii_rows.size()), m_model->rowCount() - start_row);
    const bool classic_only =
        m_copied_wii_classic_only ||
        (m_wii_hide_remote_inputs && m_wii_hide_remote_inputs->isChecked());
    for (int i = 0; i < rows_to_paste; ++i)
    {
      const int row = start_row + i;
      const auto& copied_row = m_copied_wii_rows[static_cast<size_t>(i)];
      const Movie::WiiRuntimeInputRow current_row = m_model->GetWiiRow(row);
      const Movie::WiiRuntimeInputRow pasted_row =
          !m_copied_input_fields.empty() ?
              CopyWiiInputFields(current_row, copied_row, m_copied_input_fields) :
          classic_only ? CopyClassicWiiInputOnly(current_row, copied_row).value_or(current_row) :
                         copied_row;
      if (classic_only && WiiRowsEqual(pasted_row, current_row))
        continue;

      if (m_using_runtime_movie && m_runtime_movie_kind == EditorMovieKind::Wii &&
          !movie.SetWiiRuntimeFrameState(
              row, pasted_row.is_reset ? WiimoteEmu::SerializedWiimoteState{} :
                                         pasted_row.serialized_state))
      {
        continue;
      }

      m_model->SetWiiRow(row, pasted_row);
      if (row == m_table->currentIndex().row())
        m_current_wii_row = pasted_row;
      pasted_rows.push_back(row);
    }
  }
  else
  {
    return false;
  }

  if (pasted_rows.empty())
    return false;

  m_dirty = !m_using_runtime_movie;
  if (!pasted_cells.empty())
    SelectCells(pasted_cells, pasted_cells.front().row, pasted_cells.front().column);
  else
    SelectRows(pasted_rows, pasted_rows.front());
  RefreshEditedRows(pasted_rows);
  UpdateStatusLabel();
  PopulateEditor();
  return true;
}

void DTMEditorDialog::ShowTableContextMenu(const QPoint& pos)
{
  QMenu menu(this);
  QAction* copy_action = menu.addAction(tr("Copy"));
  QAction* paste_action = menu.addAction(tr("Paste"));
  copy_action->setEnabled(!GetSelectedRows().empty());
  paste_action->setEnabled(HasCompatibleCopiedInputs() && m_table->currentIndex().isValid());

  QAction* chosen = menu.exec(m_table->viewport()->mapToGlobal(pos));
  if (chosen == copy_action)
    CopySelectedInputs();
  else if (chosen == paste_action)
    PasteCopiedInputs(m_table->currentIndex().row());
}

void DTMEditorDialog::UpdateHotkeyFocusState()
{
  const bool active = isVisible() && qobject_cast<DTMEditorDialog*>(QApplication::activeWindow()) == this;
  Host::GetInstance()->SetTASInputFocus(active);
  HotkeyManagerEmu::SetStateHotkeysBlocked(active);
}

void DTMEditorDialog::ClearPendingDrag()
{
  if (m_drag_hold_timer)
    m_drag_hold_timer->stop();

  m_drag_pending = false;
  m_drag_active = false;
  m_drag_cells.clear();
  m_drag_press_row = -1;
  m_drag_press_column = -1;
  m_drag_hover_row = -1;
  if (m_drag_preview)
    m_drag_preview->hide();
  m_table->viewport()->unsetCursor();
  m_table->unsetCursor();
}

void DTMEditorDialog::BeginPendingDrag()
{
  if (!m_drag_pending || m_drag_cells.empty() || !(QApplication::mouseButtons() & Qt::LeftButton))
    return;

  m_drag_active = true;
  const QPoint viewport_pos = m_table->viewport()->mapFromGlobal(QCursor::pos());
  const int row = m_table->rowAt(viewport_pos.y());
  if (row >= 0)
    m_drag_hover_row = row;
  m_table->viewport()->setCursor(Qt::ClosedHandCursor);
  m_table->setCursor(Qt::ClosedHandCursor);
  UpdateDragPreview();
}

void DTMEditorDialog::FinishPendingDrag(const bool apply_drag)
{
  const std::vector<InputCell> dragged_cells = m_drag_cells;
  const int hover_row = m_drag_hover_row;
  ClearPendingDrag();

  if (!apply_drag || dragged_cells.empty() || hover_row < 0)
    return;

  MoveSelectedInputs(dragged_cells, hover_row);
}

void DTMEditorDialog::UpdateDragPreview()
{
  if (!m_drag_preview || !m_drag_active || !m_model || m_model->rowCount() == 0 ||
      m_drag_cells.empty())
  {
    if (m_drag_preview)
      m_drag_preview->hide();
    return;
  }

  std::vector<InputCell> cells = m_drag_cells;
  cells.erase(std::remove_if(cells.begin(), cells.end(), [this](const InputCell& cell) {
                return cell.row < 0 || cell.row >= m_model->rowCount() || cell.column < 0 ||
                       cell.column >= m_model->columnCount();
              }),
              cells.end());
  std::vector<int> rows;
  std::vector<int> columns;
  for (const InputCell& cell : cells)
  {
    rows.push_back(cell.row);
    columns.push_back(cell.column);
  }
  std::sort(rows.begin(), rows.end());
  rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
  std::sort(columns.begin(), columns.end());
  columns.erase(std::unique(columns.begin(), columns.end()), columns.end());
  if (rows.empty() || m_drag_hover_row < 0)
  {
    m_drag_preview->hide();
    return;
  }

  const int pivot_row = rows.front();
  const int max_offset = rows.back() - pivot_row;
  const int start_row =
      std::clamp(m_drag_hover_row, 0, std::max(0, m_model->rowCount() - 1 - max_offset));
  const int end_row = std::min(m_model->rowCount() - 1, start_row + max_offset);
  const int first_column = columns.empty() ? 0 : columns.front();
  const int last_column = columns.empty() ? std::max(0, m_model->columnCount() - 1) : columns.back();

  const QModelIndex top_index = m_model->index(start_row, first_column);
  const QModelIndex bottom_index = m_model->index(end_row, last_column);
  const QRect top_rect = m_table->visualRect(top_index);
  const QRect bottom_rect = m_table->visualRect(bottom_index);
  QRect preview_rect = top_rect.united(bottom_rect);
  if (!preview_rect.isValid() || preview_rect.height() <= 0)
  {
    m_drag_preview->hide();
    return;
  }

  preview_rect.adjust(0, 0, -1, -1);
  m_drag_preview->setGeometry(preview_rect);
  m_drag_preview->raise();
  m_drag_preview->show();
}

bool DTMEditorDialog::eventFilter(QObject* watched, QEvent* event)
{
  if (m_select_buttons && m_select_buttons->isChecked())
  {
    const QVariant field_value = watched->property("dtm_input_field");
    if (field_value.isValid())
    {
      if (event->type() == QEvent::MouseButtonPress)
      {
        auto* mouse_event = static_cast<QMouseEvent*>(event);
        if (mouse_event->button() == Qt::LeftButton)
          ToggleSelectedInputField(static_cast<InputField>(field_value.toInt()));
        return true;
      }

      if (event->type() == QEvent::MouseButtonRelease ||
          event->type() == QEvent::MouseButtonDblClick)
      {
        return true;
      }
    }
  }

  if (watched == m_table->viewport())
  {
    switch (event->type())
    {
    case QEvent::MouseButtonPress:
    {
      auto* mouse_event = static_cast<QMouseEvent*>(event);
      if (mouse_event->button() == Qt::RightButton)
      {
        ClearPendingDrag();

        const QModelIndex clicked = m_table->indexAt(mouse_event->position().toPoint());
        if (clicked.isValid())
        {
          const std::vector<InputCell> selected_cells = GetSelectedInputCells();
          const bool cell_selected =
              IsInputCellSelected(selected_cells, clicked.row(), clicked.column());
          if (QItemSelectionModel* selection_model = m_table->selectionModel())
          {
            QSignalBlocker blocker(selection_model);
            if (!cell_selected)
            {
              selection_model->clearSelection();
              if ((m_model->GetKind() == ModelMovieKind::GC &&
                   m_model->IsGCControllerColumn(clicked.column())) ||
                  (m_model->GetKind() == ModelMovieKind::Wii && clicked.column() == 1))
              {
                selection_model->select(clicked, QItemSelectionModel::Select);
              }
            }
            selection_model->setCurrentIndex(clicked, QItemSelectionModel::NoUpdate);
          }
        }
        break;
      }

      if (mouse_event->button() != Qt::LeftButton || mouse_event->modifiers() != Qt::NoModifier)
        break;

      const QModelIndex clicked = m_table->indexAt(mouse_event->position().toPoint());
      if (!clicked.isValid())
        break;

      const std::vector<InputCell> selected_cells = GetSelectedInputCells();
      if (selected_cells.empty() ||
          !IsInputCellSelected(selected_cells, clicked.row(), clicked.column()))
        break;

      m_drag_cells = selected_cells;
      m_drag_press_row = clicked.row();
      m_drag_press_column = clicked.column();
      m_drag_hover_row = clicked.row();
      m_drag_pending = true;
      m_drag_active = false;
      m_drag_hold_timer->start();
      return true;
    }
    case QEvent::MouseMove:
    {
      if (!m_drag_pending)
        break;

      auto* mouse_event = static_cast<QMouseEvent*>(event);
      const int row = m_table->rowAt(static_cast<int>(mouse_event->position().y()));
      if (row >= 0)
      {
        m_drag_hover_row = row;
        if (m_drag_active)
        {
          m_table->scrollTo(m_model->index(row, 0), QAbstractItemView::EnsureVisible);
          UpdateDragPreview();
        }
      }
      return m_drag_active;
    }
    case QEvent::ContextMenu:
    {
      auto* context_event = static_cast<QContextMenuEvent*>(event);
      ShowTableContextMenu(context_event->pos());
      return true;
    }
    case QEvent::MouseButtonRelease:
    {
      auto* mouse_event = static_cast<QMouseEvent*>(event);
      if (!m_drag_pending || mouse_event->button() != Qt::LeftButton)
        break;

      if (!m_drag_active && m_drag_press_row >= 0)
      {
        if (QItemSelectionModel* selection_model = m_table->selectionModel())
        {
          selection_model->setCurrentIndex(m_model->index(m_drag_press_row, m_drag_press_column),
                                           QItemSelectionModel::NoUpdate);
        }
      }

      FinishPendingDrag(m_drag_active);
      return true;
    }
    default:
      break;
    }
  }

  return QDialog::eventFilter(watched, event);
}

bool DTMEditorDialog::MoveSelectedInputs(const std::vector<InputCell>& selected_cells, int dest_start)
{
  if (selected_cells.empty() || m_model->GetKind() == ModelMovieKind::None)
    return false;
  if (m_select_buttons && m_select_buttons->isChecked() && m_selected_input_fields.empty())
    return false;
  const std::set<InputField> moved_fields = ActiveInputFieldFilter();

  std::vector<InputCell> cells = selected_cells;
  cells.erase(std::remove_if(cells.begin(), cells.end(), [this](const InputCell& cell) {
                if (cell.row < 0 || cell.row >= m_model->rowCount())
                  return true;
                if (m_model->GetKind() == ModelMovieKind::GC)
                  return !m_model->IsGCControllerColumn(cell.column);
                return m_model->GetKind() != ModelMovieKind::Wii || cell.column != 1;
              }),
              cells.end());
  std::sort(cells.begin(), cells.end(), [](const InputCell& lhs, const InputCell& rhs) {
    return std::tie(lhs.row, lhs.column) < std::tie(rhs.row, rhs.column);
  });
  cells.erase(std::unique(cells.begin(), cells.end(), [](const InputCell& lhs, const InputCell& rhs) {
                return lhs.row == rhs.row && lhs.column == rhs.column;
              }),
              cells.end());
  if (cells.empty())
    return false;

  std::vector<int> rows;
  rows.reserve(cells.size());
  for (const InputCell& cell : cells)
    rows.push_back(cell.row);
  std::sort(rows.begin(), rows.end());
  rows.erase(std::unique(rows.begin(), rows.end()), rows.end());

  const int row_count = m_model->rowCount();
  const int pivot_row = rows.front();
  const int max_offset = rows.back() - pivot_row;
  const int clamped_dest_start = std::clamp(dest_start, 0, std::max(0, row_count - 1 - max_offset));

  std::vector<int> dest_rows;
  dest_rows.reserve(rows.size());
  for (const int row : rows)
    dest_rows.push_back(clamped_dest_start + (row - pivot_row));

  std::vector<InputCell> dest_cells;
  dest_cells.reserve(cells.size());
  for (const InputCell& cell : cells)
    dest_cells.push_back({clamped_dest_start + (cell.row - pivot_row), cell.column});

  const bool cells_unchanged =
      cells.size() == dest_cells.size() &&
      std::equal(cells.begin(), cells.end(), dest_cells.begin(), [](const InputCell& lhs,
                                                                    const InputCell& rhs) {
        return lhs.row == rhs.row && lhs.column == rhs.column;
      });
  if (cells_unchanged)
  {
    SelectCells(dest_cells, clamped_dest_start, dest_cells.front().column);
    return true;
  }

  std::vector<int> changed_rows = rows;
  changed_rows.insert(changed_rows.end(), dest_rows.begin(), dest_rows.end());
  std::sort(changed_rows.begin(), changed_rows.end());
  changed_rows.erase(std::unique(changed_rows.begin(), changed_rows.end()), changed_rows.end());

  const auto changed_index = [&changed_rows](const int row) {
    return static_cast<size_t>(
        std::lower_bound(changed_rows.begin(), changed_rows.end(), row) - changed_rows.begin());
  };
  const auto is_dest_row = [&dest_rows](const int row) {
    return std::binary_search(dest_rows.begin(), dest_rows.end(), row);
  };

  auto& movie = Core::System::GetInstance().GetMovie();
  if (m_model->GetKind() == ModelMovieKind::GC)
  {
    struct GCCellMove
    {
      int source_row = -1;
      int dest_row = -1;
      int controller = -1;
      int column = -1;
    };
    std::vector<GCCellMove> cell_moves;
    cell_moves.reserve(cells.size());
    for (const InputCell& cell : cells)
    {
      int controller = -1;
      if (!m_model->IsGCControllerColumn(cell.column, &controller))
        continue;
      cell_moves.push_back({cell.row, clamped_dest_start + (cell.row - pivot_row), controller,
                            cell.column});
    }
    const auto is_dest_cell = [&cell_moves](const int row, const int controller) {
      return std::ranges::any_of(cell_moves, [row, controller](const GCCellMove& move) {
        return move.dest_row == row && move.controller == controller;
      });
    };

    // Only materialize rows that actually change, large DTMs should not copy the whole movie.
    std::vector<std::array<Movie::ControllerState, 4>> original;
    original.reserve(changed_rows.size());
    for (const int row : changed_rows)
    {
      std::array<Movie::ControllerState, 4> row_data{};
      for (int controller = 0; controller < 4; ++controller)
        row_data[static_cast<size_t>(controller)] = m_model->GetGCState(row, controller);
      original.push_back(row_data);
    }
    auto updated = original;

    for (const GCCellMove& move : cell_moves)
    {
      if (is_dest_cell(move.source_row, move.controller))
        continue;

      auto& updated_row = updated[changed_index(move.source_row)];
      updated_row[static_cast<size_t>(move.controller)] =
          moved_fields.empty() ?
              MakeNeutralGCStateLike(
                  original[changed_index(move.source_row)][static_cast<size_t>(move.controller)]) :
              NeutralizeGCInputFields(
                  original[changed_index(move.source_row)][static_cast<size_t>(move.controller)],
                  moved_fields);
    }

    for (const GCCellMove& move : cell_moves)
    {
      updated[changed_index(move.dest_row)][static_cast<size_t>(move.controller)] =
          CopyGCInputFields(
              updated[changed_index(move.dest_row)][static_cast<size_t>(move.controller)],
              original[changed_index(move.source_row)][static_cast<size_t>(move.controller)],
              moved_fields);
    }

    for (size_t i = 0; i < changed_rows.size(); ++i)
    {
      const int row = changed_rows[i];
      for (int controller = 0; controller < 4; ++controller)
      {
        if (!m_model->IsGCControllerActive(controller))
          continue;

        if (m_using_runtime_movie && m_runtime_movie_kind == EditorMovieKind::GC &&
            !movie.SetGCRuntimeFrameState(row, controller,
                                          updated[i][static_cast<size_t>(controller)]))
        {
          continue;
        }

        m_model->SetGCState(row, controller, updated[i][static_cast<size_t>(controller)]);
      }
    }
  }
  else if (m_model->GetKind() == ModelMovieKind::Wii)
  {
    std::vector<Movie::WiiRuntimeInputRow> original;
    original.reserve(changed_rows.size());
    for (const int row : changed_rows)
      original.push_back(m_model->GetWiiRow(row));
    auto updated = original;
    const bool classic_only =
        moved_fields.empty() && m_wii_hide_remote_inputs && m_wii_hide_remote_inputs->isChecked();

    for (const int row : rows)
    {
      if (is_dest_row(row))
        continue;
      updated[changed_index(row)] =
          !moved_fields.empty() ? NeutralizeWiiInputFields(original[changed_index(row)], moved_fields) :
          classic_only          ? MakeNeutralClassicWiiRowLike(original[changed_index(row)]) :
                                  MakeNeutralWiiRowLike(original[changed_index(row)]);
    }

    for (size_t i = 0; i < rows.size(); ++i)
    {
      updated[changed_index(dest_rows[i])] =
          !moved_fields.empty() ?
              CopyWiiInputFields(updated[changed_index(dest_rows[i])],
                                 original[changed_index(rows[i])], moved_fields) :
          classic_only ? CopyClassicWiiInputOnly(updated[changed_index(dest_rows[i])],
                                                 original[changed_index(rows[i])])
                             .value_or(updated[changed_index(dest_rows[i])]) :
                         original[changed_index(rows[i])];
    }

    for (size_t i = 0; i < changed_rows.size(); ++i)
    {
      const int row = changed_rows[i];
      if (WiiRowsEqual(updated[i], original[i]))
        continue;

      if (m_using_runtime_movie && m_runtime_movie_kind == EditorMovieKind::Wii &&
          !movie.SetWiiRuntimeFrameState(row, updated[i].is_reset ? WiimoteEmu::SerializedWiimoteState{} :
                                                              updated[i].serialized_state))
      {
        continue;
      }

      m_model->SetWiiRow(row, updated[i]);
    }
  }
  else
  {
    return false;
  }

  m_dirty = !m_using_runtime_movie;
  if (m_model->GetKind() == ModelMovieKind::GC)
    SelectCells(dest_cells, clamped_dest_start, dest_cells.front().column);
  else
    SelectRows(dest_rows, clamped_dest_start);
  RefreshEditedRows(changed_rows);
  UpdateStatusLabel();
  PopulateEditor();
  return true;
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
  else
  {
    for (size_t i = 0; i < m_wii_ir_visible.size(); ++i)
    {
      if (source != m_wii_ir_visible[i] && source != m_wii_ir_x[i] && source != m_wii_ir_y[i] &&
          source != m_wii_ir_size[i])
      {
        continue;
      }

      if (m_wii_ir_visible[i]->isChecked())
      {
        state.camera_points[i] =
            WiimoteEmu::CameraPoint({static_cast<u16>(m_wii_ir_x[i]->value()),
                                     static_cast<u16>(m_wii_ir_y[i]->value())},
                                    static_cast<u8>(m_wii_ir_size[i]->value()));
      }
      else
      {
        state.camera_points[i] = WiimoteEmu::CameraPoint();
      }

      row->serialized_state = WiimoteEmu::SerializeDesiredState(state);
      return;
    }
  }

  if (state.motion_plus.has_value() &&
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
  else if (std::holds_alternative<WiimoteEmu::Classic::DataFormat>(state.extension.data))
  {
    auto classic = std::get<WiimoteEmu::Classic::DataFormat>(state.extension.data);
    u16 buttons = 0;
    if (m_wii_classic_a->isChecked())
      buttons |= WiimoteEmu::Classic::BUTTON_A;
    if (m_wii_classic_b->isChecked())
      buttons |= WiimoteEmu::Classic::BUTTON_B;
    if (m_wii_classic_x->isChecked())
      buttons |= WiimoteEmu::Classic::BUTTON_X;
    if (m_wii_classic_y->isChecked())
      buttons |= WiimoteEmu::Classic::BUTTON_Y;
    if (m_wii_classic_zl->isChecked())
      buttons |= WiimoteEmu::Classic::BUTTON_ZL;
    if (m_wii_classic_zr->isChecked())
      buttons |= WiimoteEmu::Classic::BUTTON_ZR;
    if (m_wii_classic_l->isChecked())
      buttons |= WiimoteEmu::Classic::TRIGGER_L;
    if (m_wii_classic_r->isChecked())
      buttons |= WiimoteEmu::Classic::TRIGGER_R;
    if (m_wii_classic_minus->isChecked())
      buttons |= WiimoteEmu::Classic::BUTTON_MINUS;
    if (m_wii_classic_plus->isChecked())
      buttons |= WiimoteEmu::Classic::BUTTON_PLUS;
    if (m_wii_classic_home->isChecked())
      buttons |= WiimoteEmu::Classic::BUTTON_HOME;
    if (m_wii_classic_up->isChecked())
      buttons |= WiimoteEmu::Classic::PAD_UP;
    if (m_wii_classic_down->isChecked())
      buttons |= WiimoteEmu::Classic::PAD_DOWN;
    if (m_wii_classic_left->isChecked())
      buttons |= WiimoteEmu::Classic::PAD_LEFT;
    if (m_wii_classic_right->isChecked())
      buttons |= WiimoteEmu::Classic::PAD_RIGHT;
    classic.SetButtons(buttons);
    classic.SetLeftStick({static_cast<u8>(m_wii_classic_left_x->value()),
                          static_cast<u8>(m_wii_classic_left_y->value())});
    classic.SetRightStick({static_cast<u8>(m_wii_classic_right_x->value()),
                           static_cast<u8>(m_wii_classic_right_y->value())});
    classic.SetLeftTrigger(static_cast<u8>(m_wii_classic_l_trigger->value()));
    classic.SetRightTrigger(static_cast<u8>(m_wii_classic_r_trigger->value()));
    state.extension.data = classic;
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
  const std::vector<InputCell> selected_cells = GetSelectedInputCells();
  if (selected_cells.empty())
    return;

  if (m_model->GetKind() == ModelMovieKind::GC)
  {
    for (const InputCell& cell : selected_cells)
    {
      int controller = -1;
      if (!m_model->IsGCControllerColumn(cell.column, &controller))
        continue;

      Movie::ControllerState state = m_model->GetGCState(cell.row, controller);
      ApplyGCEditorChange(&state, source);
      if (m_using_runtime_movie && m_runtime_movie_kind == EditorMovieKind::GC &&
          !Core::System::GetInstance().GetMovie().SetGCRuntimeFrameState(cell.row, controller,
                                                                         state))
      {
        continue;
      }
      m_model->SetGCState(cell.row, controller, state);
    }
    m_dirty = !m_using_runtime_movie;
  }
  else if (m_model->GetKind() == ModelMovieKind::Wii)
  {
    if (!m_current_wii_state_valid && !m_current_wii_row.is_reset)
      return;

    for (const InputCell& cell : selected_cells)
    {
      const int row_index = cell.row;
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
  UpdateInputFieldSelectionStyles();
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
  for (size_t i = 0; i < m_wii_ir_visible.size(); ++i)
  {
    if (m_wii_ir_visible[i]->isChecked())
    {
      state.camera_points[i] =
          WiimoteEmu::CameraPoint({static_cast<u16>(m_wii_ir_x[i]->value()),
                                   static_cast<u16>(m_wii_ir_y[i]->value())},
                                  static_cast<u8>(m_wii_ir_size[i]->value()));
    }
    else
    {
      state.camera_points[i] = WiimoteEmu::CameraPoint();
    }
  }

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
  else if (std::holds_alternative<WiimoteEmu::Classic::DataFormat>(state.extension.data))
  {
    auto classic = std::get<WiimoteEmu::Classic::DataFormat>(state.extension.data);
    u16 buttons = 0;
    if (m_wii_classic_a->isChecked())
      buttons |= WiimoteEmu::Classic::BUTTON_A;
    if (m_wii_classic_b->isChecked())
      buttons |= WiimoteEmu::Classic::BUTTON_B;
    if (m_wii_classic_x->isChecked())
      buttons |= WiimoteEmu::Classic::BUTTON_X;
    if (m_wii_classic_y->isChecked())
      buttons |= WiimoteEmu::Classic::BUTTON_Y;
    if (m_wii_classic_zl->isChecked())
      buttons |= WiimoteEmu::Classic::BUTTON_ZL;
    if (m_wii_classic_zr->isChecked())
      buttons |= WiimoteEmu::Classic::BUTTON_ZR;
    if (m_wii_classic_l->isChecked())
      buttons |= WiimoteEmu::Classic::TRIGGER_L;
    if (m_wii_classic_r->isChecked())
      buttons |= WiimoteEmu::Classic::TRIGGER_R;
    if (m_wii_classic_minus->isChecked())
      buttons |= WiimoteEmu::Classic::BUTTON_MINUS;
    if (m_wii_classic_plus->isChecked())
      buttons |= WiimoteEmu::Classic::BUTTON_PLUS;
    if (m_wii_classic_home->isChecked())
      buttons |= WiimoteEmu::Classic::BUTTON_HOME;
    if (m_wii_classic_up->isChecked())
      buttons |= WiimoteEmu::Classic::PAD_UP;
    if (m_wii_classic_down->isChecked())
      buttons |= WiimoteEmu::Classic::PAD_DOWN;
    if (m_wii_classic_left->isChecked())
      buttons |= WiimoteEmu::Classic::PAD_LEFT;
    if (m_wii_classic_right->isChecked())
      buttons |= WiimoteEmu::Classic::PAD_RIGHT;
    classic.SetButtons(buttons);
    classic.SetLeftStick({static_cast<u8>(m_wii_classic_left_x->value()),
                          static_cast<u8>(m_wii_classic_left_y->value())});
    classic.SetRightStick({static_cast<u8>(m_wii_classic_right_x->value()),
                           static_cast<u8>(m_wii_classic_right_y->value())});
    classic.SetLeftTrigger(static_cast<u8>(m_wii_classic_l_trigger->value()));
    classic.SetRightTrigger(static_cast<u8>(m_wii_classic_r_trigger->value()));
    state.extension.data = classic;
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
    m_model->SetGCMovieData(gc->active_controllers, gc->active_gba_controllers, gc->rows);
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
    m_model->SetWiiMovieData(wii->active_wiimotes, wii->rows);
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
  m_save_button->setEnabled(m_model->GetKind() != ModelMovieKind::None);
  m_save_button->setText(m_using_runtime_movie ? tr("Export DTM") : tr("Save"));
}
