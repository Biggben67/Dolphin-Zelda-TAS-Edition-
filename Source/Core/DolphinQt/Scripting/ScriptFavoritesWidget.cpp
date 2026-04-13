// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Scripting/ScriptFavoritesWidget.h"

#include <QDir>
#include <QFileInfo>
#include <QListWidget>
#include <QListWidgetItem>
#include <QVBoxLayout>

#include "Common/FileUtil.h"
#include "DolphinQt/Scripting/ScriptFavoritesManager.h"

namespace
{
constexpr int FAVORITES_WIDTH = 230;
constexpr int PATH_ROLE = Qt::UserRole;
}

ScriptFavoritesWidget::ScriptFavoritesWidget(QWidget* parent) : QGroupBox(tr("Favorite Scripts"), parent)
{
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
  setMinimumWidth(FAVORITES_WIDTH);
  setMaximumWidth(FAVORITES_WIDTH);

  m_list = new QListWidget(this);
  m_list->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_list->setWordWrap(true);
  m_list->setSelectionMode(QAbstractItemView::NoSelection);

  auto* layout = new QVBoxLayout;
  layout->addWidget(m_list);
  setLayout(layout);

  connect(m_list, &QListWidget::itemChanged, this, &ScriptFavoritesWidget::OnItemChanged);

  auto& manager = ScriptFavoritesManager::Get();
  connect(&manager, &ScriptFavoritesManager::FavoritesChanged, this, &ScriptFavoritesWidget::Reload);
  connect(&manager, &ScriptFavoritesManager::ScriptStatesChanged, this, &ScriptFavoritesWidget::Reload);

  Reload();
}

void ScriptFavoritesWidget::Reload()
{
  m_updating = true;
  m_list->clear();

  const QStringList favorites = ScriptFavoritesManager::Get().GetFavorites();
  if (favorites.empty())
  {
    auto* item = new QListWidgetItem(tr("No favorite scripts"));
    item->setFlags(Qt::NoItemFlags);
    m_list->addItem(item);
    m_updating = false;
    return;
  }

  for (const QString& favorite : favorites)
  {
    QFileInfo info(favorite);
    if (!info.exists() || !info.isFile())
      continue;

    auto* item = new QListWidgetItem(GetDisplayPath(favorite), m_list);
    item->setData(PATH_ROLE, favorite);
    item->setToolTip(favorite);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
    item->setCheckState(ScriptFavoritesManager::Get().IsScriptEnabled(favorite) ? Qt::Checked :
                                                                                    Qt::Unchecked);
  }

  if (m_list->count() == 0)
  {
    auto* item = new QListWidgetItem(tr("No favorite scripts"));
    item->setFlags(Qt::NoItemFlags);
    m_list->addItem(item);
  }

  m_updating = false;
}

void ScriptFavoritesWidget::OnItemChanged(QListWidgetItem* item)
{
  if (m_updating || item == nullptr)
    return;

  const QString path = item->data(PATH_ROLE).toString();
  if (path.isEmpty())
    return;

  ScriptFavoritesManager::Get().SetScriptEnabled(path, item->checkState() == Qt::Checked);
}

QString ScriptFavoritesWidget::GetDisplayPath(const QString& absolute_path) const
{
  const QDir scripts_dir(QString::fromStdString(File::GetUserPath(D_SCRIPTS_IDX)));
  const QString relative = scripts_dir.relativeFilePath(absolute_path);
  return relative.startsWith(QStringLiteral("..")) ? QFileInfo(absolute_path).fileName() : relative;
}
