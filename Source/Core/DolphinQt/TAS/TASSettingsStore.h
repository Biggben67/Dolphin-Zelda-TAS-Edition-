// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <string>
#include <utility>

#include <QSettings>
#include <QString>

#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "DolphinQt/Settings.h"

namespace TASSettingsStore
{
inline QString EssKey(const char* prefix, int index, char axis)
{
  return QStringLiteral("tas/ess/%1/%2/%3")
      .arg(QString::fromLatin1(prefix))
      .arg(index)
      .arg(QChar::fromLatin1(axis));
}

inline std::pair<int, int> ReadEssPreset(const char* prefix, int index, int default_x,
                                         int default_y)
{
  QSettings& settings = Settings::GetQSettings();
  const QString x_key = EssKey(prefix, index, 'x');
  const QString y_key = EssKey(prefix, index, 'y');

  int x = default_x;
  int y = default_y;
  if (settings.contains(x_key) || settings.contains(y_key))
  {
    x = settings.value(x_key, default_x).toInt();
    y = settings.value(y_key, default_y).toInt();
  }
  else
  {
    Common::IniFile ini;
    ini.Load(File::GetUserPath(D_CONFIG_IDX) + "Dolphin.ini");
    ini.GetIfExists("TAS", std::string(prefix) + std::to_string(index) + "X", &x);
    ini.GetIfExists("TAS", std::string(prefix) + std::to_string(index) + "Y", &y);
    settings.setValue(x_key, x);
    settings.setValue(y_key, y);
    settings.sync();
  }

  return {std::clamp(x, 0, 255), std::clamp(y, 0, 255)};
}

inline void WriteEssPreset(const char* prefix, int index, int x, int y)
{
  QSettings& settings = Settings::GetQSettings();
  settings.setValue(EssKey(prefix, index, 'x'), std::clamp(x, 0, 255));
  settings.setValue(EssKey(prefix, index, 'y'), std::clamp(y, 0, 255));
  settings.sync();
}
}  // namespace TASSettingsStore
