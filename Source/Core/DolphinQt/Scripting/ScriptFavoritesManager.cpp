// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Scripting/ScriptFavoritesManager.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>

#include <utility>

#include "DolphinQt/QtUtils/QueueOnObject.h"
#include "DolphinQt/Settings.h"

#include "Scripting/ScriptList.h"

namespace
{
constexpr const char* SETTINGS_KEY = "scripting/favorites";

int IndexOfCaseInsensitive(const QStringList& values, const QString& value)
{
  for (int i = 0; i < values.size(); ++i)
  {
    if (values[i].compare(value, Qt::CaseInsensitive) == 0)
      return i;
  }
  return -1;
}
}

ScriptFavoritesManager& ScriptFavoritesManager::Get()
{
  static ScriptFavoritesManager manager;
  return manager;
}

ScriptFavoritesManager::ScriptFavoritesManager()
{
  LoadFavorites();

  // Refresh the Scripts panel when the script map changes, including from the control pipe.
  Scripts::SetChangeCallback(
      [this] { QueueOnObject(this, [this] { emit ScriptStatesChanged(); }); });
}

QStringList ScriptFavoritesManager::GetFavorites() const
{
  return m_favorites;
}

bool ScriptFavoritesManager::IsFavorite(const QString& path) const
{
  return m_favorites.contains(NormalizePath(path), Qt::CaseInsensitive);
}

void ScriptFavoritesManager::SetFavorite(const QString& path, bool favorite)
{
  const QString normalized = NormalizePath(path);
  if (normalized.isEmpty())
    return;

  const int existing = IndexOfCaseInsensitive(m_favorites, normalized);
  const bool changed = favorite ? existing < 0 : existing >= 0;
  if (!changed)
    return;

  if (favorite)
    m_favorites.append(normalized);
  else
    m_favorites.removeAt(existing);

  SaveFavorites();
  emit FavoritesChanged();
}

void ScriptFavoritesManager::SetFavoritesOrder(const QStringList& paths)
{
  QStringList ordered;
  ordered.reserve(m_favorites.size());
  for (const QString& path : paths)
  {
    const QString normalized = NormalizePath(path);
    if (IsFavorite(normalized) && !ordered.contains(normalized, Qt::CaseInsensitive))
      ordered.append(normalized);
  }
  for (const QString& favorite : m_favorites)
  {
    if (!ordered.contains(favorite, Qt::CaseInsensitive))
      ordered.append(favorite);
  }
  if (ordered == m_favorites)
    return;
  m_favorites = std::move(ordered);
  SaveFavorites();
  emit FavoritesChanged();
}

void ScriptFavoritesManager::ClearFavorites()
{
  if (m_favorites.isEmpty())
    return;
  m_favorites.clear();
  SaveFavorites();
  emit FavoritesChanged();
}

void ScriptFavoritesManager::ToggleFavorite(const QString& path)
{
  SetFavorite(path, !IsFavorite(path));
}

bool ScriptFavoritesManager::IsScriptEnabled(const QString& path) const
{
  return Scripts::IsEnabled(NormalizePath(path).toStdString());
}

void ScriptFavoritesManager::SetScriptEnabled(const QString& path, bool enabled)
{
  const QString normalized = NormalizePath(path);
  if (normalized.isEmpty())
    return;

  Scripts::SetEnabled(normalized.toStdString(), enabled);
}

void ScriptFavoritesManager::RestartScript(const QString& path)
{
  const QString normalized = NormalizePath(path);
  if (normalized.isEmpty())
    return;

  Scripts::Restart(normalized.toStdString());
}

void ScriptFavoritesManager::LoadFavorites()
{
  const QStringList favorite_paths = Settings::GetQSettings().value(QLatin1String(SETTINGS_KEY))
                                         .toStringList();
  for (const QString& favorite_path : favorite_paths)
  {
    const QString normalized = NormalizePath(favorite_path);
    if (!normalized.isEmpty())
    {
      if (!m_favorites.contains(normalized, Qt::CaseInsensitive))
        m_favorites.append(normalized);
    }
  }
}

void ScriptFavoritesManager::SaveFavorites() const
{
  QSettings& settings = Settings::GetQSettings();
  settings.setValue(QLatin1String(SETTINGS_KEY), GetFavorites());
  settings.sync();
}

QString ScriptFavoritesManager::NormalizePath(const QString& path)
{
  if (path.isEmpty())
    return {};

  const QFileInfo info(path);
  const QString absolute = info.exists() ? info.canonicalFilePath() : info.absoluteFilePath();
  return QDir::cleanPath(absolute);
}
