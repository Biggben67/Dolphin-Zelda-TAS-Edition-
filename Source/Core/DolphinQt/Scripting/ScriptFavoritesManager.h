// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

class ScriptFavoritesManager : public QObject
{
  Q_OBJECT

public:
  static ScriptFavoritesManager& Get();

  QStringList GetFavorites() const;
  bool IsFavorite(const QString& path) const;
  void SetFavorite(const QString& path, bool favorite);
  void ToggleFavorite(const QString& path);

  bool IsScriptEnabled(const QString& path) const;
  void SetScriptEnabled(const QString& path, bool enabled);
  void RestartScript(const QString& path);

signals:
  void FavoritesChanged();
  void ScriptStatesChanged();

private:
  ScriptFavoritesManager();

  void LoadFavorites();
  void SaveFavorites() const;
  static QString NormalizePath(const QString& path);

  QHash<QString, bool> m_favorites;
};
