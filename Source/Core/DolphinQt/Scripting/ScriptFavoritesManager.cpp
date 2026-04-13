// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Scripting/ScriptFavoritesManager.h"

#include <QDir>
#include <QFileInfo>

#include "DolphinQt/Settings.h"

#include "Scripting/ScriptList.h"
#include "Scripting/ScriptingEngine.h"

namespace
{
constexpr const char* SETTINGS_KEY = "scripting/favorites";
}

ScriptFavoritesManager& ScriptFavoritesManager::Get()
{
  static ScriptFavoritesManager manager;
  return manager;
}

ScriptFavoritesManager::ScriptFavoritesManager()
{
  LoadFavorites();
}

QStringList ScriptFavoritesManager::GetFavorites() const
{
  QStringList favorites = m_favorites.keys();
  favorites.sort(Qt::CaseInsensitive);
  return favorites;
}

bool ScriptFavoritesManager::IsFavorite(const QString& path) const
{
  return m_favorites.contains(NormalizePath(path));
}

void ScriptFavoritesManager::SetFavorite(const QString& path, bool favorite)
{
  const QString normalized = NormalizePath(path);
  if (normalized.isEmpty())
    return;

  const bool changed =
      favorite ? !m_favorites.contains(normalized) : m_favorites.remove(normalized) != 0;
  if (!changed)
    return;

  if (favorite)
    m_favorites.insert(normalized, true);

  SaveFavorites();
  emit FavoritesChanged();
}

void ScriptFavoritesManager::ToggleFavorite(const QString& path)
{
  SetFavorite(path, !IsFavorite(path));
}

bool ScriptFavoritesManager::IsScriptEnabled(const QString& path) const
{
  return Scripts::g_scripts.find(NormalizePath(path).toStdString()) != Scripts::g_scripts.end();
}

void ScriptFavoritesManager::SetScriptEnabled(const QString& path, bool enabled)
{
  const QString normalized = NormalizePath(path);
  if (normalized.isEmpty())
    return;

  const std::string file_path = normalized.toStdString();
  const auto it = Scripts::g_scripts.find(file_path);

  if (enabled)
  {
    if (it == Scripts::g_scripts.end())
    {
      Scripting::ScriptingBackend* backend = nullptr;
      if (Scripts::g_scripts_started)
        backend = new Scripting::ScriptingBackend(file_path);
      Scripts::g_scripts[file_path] = backend;
      emit ScriptStatesChanged();
    }
    return;
  }

  if (it != Scripts::g_scripts.end())
  {
    delete it->second;
    Scripts::g_scripts.erase(it);
    emit ScriptStatesChanged();
  }
}

void ScriptFavoritesManager::RestartScript(const QString& path)
{
  const QString normalized = NormalizePath(path);
  if (normalized.isEmpty())
    return;

  auto it = Scripts::g_scripts.find(normalized.toStdString());
  if (it != Scripts::g_scripts.end() && it->second != nullptr)
  {
    delete it->second;
    it->second = new Scripting::ScriptingBackend(normalized.toStdString());
    emit ScriptStatesChanged();
  }
}

void ScriptFavoritesManager::LoadFavorites()
{
  const QStringList favorite_paths = Settings::GetQSettings().value(QLatin1String(SETTINGS_KEY))
                                         .toStringList();
  for (const QString& favorite_path : favorite_paths)
  {
    const QString normalized = NormalizePath(favorite_path);
    if (!normalized.isEmpty())
      m_favorites.insert(normalized, true);
  }
}

void ScriptFavoritesManager::SaveFavorites() const
{
  Settings::GetQSettings().setValue(QLatin1String(SETTINGS_KEY), GetFavorites());
}

QString ScriptFavoritesManager::NormalizePath(const QString& path)
{
  if (path.isEmpty())
    return {};

  const QFileInfo info(path);
  const QString absolute = info.exists() ? info.canonicalFilePath() : info.absoluteFilePath();
  return QDir::cleanPath(absolute);
}
