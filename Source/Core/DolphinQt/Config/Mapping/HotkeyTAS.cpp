// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/Mapping/HotkeyTAS.h"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

#include "Common/FileUtil.h"
#include "Common/IniFile.h"

#include "Core/HotkeyManager.h"

namespace
{
constexpr std::array<std::pair<int, int>, 9> k_default_main_ess = {
    std::pair{111, 145},  // Up-left
    {128, 146},           // Up
    {145, 145},           // Up-right
    {110, 128},           // Left
    {128, 128},           // Center
    {146, 128},           // Right
    {111, 111},           // Down-left
    {128, 110},           // Down
    {145, 111},           // Down-right
};

constexpr std::array<std::pair<int, int>, 9> k_default_nunchuk_ess = {
    std::pair{111, 145},  // Up-left
    {128, 146},           // Up
    {145, 145},           // Up-right
    {110, 128},           // Left
    {128, 128},           // Center
    {146, 128},           // Right
    {111, 111},           // Down-left
    {128, 110},           // Down
    {145, 111},           // Down-right
};

constexpr const char* k_ess_labels[] = {
    "Up-Left",
    "Up",
    "Up-Right",
    "Left",
    "Center",
    "Right",
    "Down-Left",
    "Down",
    "Down-Right",
};
}  // namespace

HotkeyTAS::HotkeyTAS(MappingWindow* window) : MappingWidget(window)
{
  CreateMainLayout();
}

void HotkeyTAS::CreateMainLayout()
{
  m_main_layout = new QHBoxLayout();

  m_main_layout->addWidget(
      CreateGroupBox(tr("Frame Advance"), HotkeyManagerEmu::GetHotkeyGroup(HKGP_FRAME_ADVANCE)));
  m_main_layout->addWidget(
      CreateGroupBox(tr("Movie"), HotkeyManagerEmu::GetHotkeyGroup(HKGP_MOVIE)));
  m_main_layout->addWidget(
      CreateGroupBox(tr("Main Stick ESS"),
                     HotkeyManagerEmu::GetHotkeyGroup(HKGP_TAS_MAIN_STICK_ESS)));
  m_main_layout->addWidget(
      CreateGroupBox(tr("Nunchuk Stick ESS"),
                     HotkeyManagerEmu::GetHotkeyGroup(HKGP_TAS_NUNCHUK_STICK_ESS)));

  auto* ess_box = new QGroupBox(tr("ESS Presets"));
  auto* ess_layout = new QVBoxLayout;
  ess_layout->addWidget(CreateEssTable(tr("Main Stick Presets"), "MainStickEss",
                                      k_default_main_ess));
  ess_layout->addWidget(CreateEssTable(tr("Nunchuk Stick Presets"), "NunchukEss",
                                      k_default_nunchuk_ess));
  ess_box->setLayout(ess_layout);
  m_main_layout->addWidget(ess_box);

  setLayout(m_main_layout);
}

InputConfig* HotkeyTAS::GetConfig()
{
  return HotkeyManagerEmu::GetConfig();
}

void HotkeyTAS::LoadSettings()
{
  HotkeyManagerEmu::LoadConfig();
}

void HotkeyTAS::SaveSettings()
{
  HotkeyManagerEmu::GetConfig()->SaveConfig();
}

QGroupBox* HotkeyTAS::CreateEssTable(const QString& title, const char* key_prefix,
                                     const std::array<std::pair<int, int>, 9>& defaults)
{
  auto* box = new QGroupBox(title);
  auto* layout = new QGridLayout;
  layout->addWidget(new QLabel(tr("Preset")), 0, 0);
  layout->addWidget(new QLabel(tr("X")), 0, 1);
  layout->addWidget(new QLabel(tr("Y")), 0, 2);

  Common::IniFile ini;
  const std::string ini_path = File::GetUserPath(D_CONFIG_IDX) + "Dolphin.ini";
  ini.Load(ini_path);

  for (int i = 0; i < static_cast<int>(defaults.size()); ++i)
  {
    int x = defaults[i].first;
    int y = defaults[i].second;
    ini.GetIfExists("TAS", std::string(key_prefix) + std::to_string(i) + "X", &x);
    ini.GetIfExists("TAS", std::string(key_prefix) + std::to_string(i) + "Y", &y);
    x = std::clamp(x, 0, 255);
    y = std::clamp(y, 0, 255);

    auto* label = new QLabel(tr(k_ess_labels[i]));
    auto* x_value = new QSpinBox;
    auto* y_value = new QSpinBox;
    x_value->setRange(0, 255);
    y_value->setRange(0, 255);
    x_value->setValue(x);
    y_value->setValue(y);

    connect(x_value, &QSpinBox::valueChanged, this, [this, key_prefix, i, x_value, y_value](int) {
      SaveEssPreset(key_prefix, i, x_value->value(), y_value->value());
    });
    connect(y_value, &QSpinBox::valueChanged, this, [this, key_prefix, i, x_value, y_value](int) {
      SaveEssPreset(key_prefix, i, x_value->value(), y_value->value());
    });

    layout->addWidget(label, i + 1, 0);
    layout->addWidget(x_value, i + 1, 1);
    layout->addWidget(y_value, i + 1, 2);
  }

  box->setLayout(layout);
  return box;
}

void HotkeyTAS::SaveEssPreset(const char* key_prefix, int index, int x, int y)
{
  Common::IniFile ini;
  const std::string ini_path = File::GetUserPath(D_CONFIG_IDX) + "Dolphin.ini";
  ini.Load(ini_path);
  auto* section = ini.GetOrCreateSection("TAS");
  section->Set(std::string(key_prefix) + std::to_string(index) + "X", x);
  section->Set(std::string(key_prefix) + std::to_string(index) + "Y", y);
  ini.Save(ini_path);
}
