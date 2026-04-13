// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QGroupBox>

class QListWidget;
class QListWidgetItem;

class ScriptFavoritesWidget : public QGroupBox
{
  Q_OBJECT

public:
  explicit ScriptFavoritesWidget(QWidget* parent = nullptr);

private:
  void Reload();
  void OnItemChanged(QListWidgetItem* item);
  QString GetDisplayPath(const QString& absolute_path) const;

  QListWidget* m_list;
  bool m_updating = false;
};
