// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "ScriptsListModel.h"

#include <QDirIterator>

#include "Common/FileUtil.h"
#include "Core/Core.h"

#include "DolphinQt/Scripting/ScriptFavoritesManager.h"
#include "Scripting/ScriptList.h"

ScriptsFileSystemModel::ScriptsFileSystemModel(QObject* parent /* = nullptr */)
    : QFileSystemModel(parent)
{
  QStringList filters;
  filters.append(QString::fromStdString("*.py"));
  filters.append(QString::fromStdString("*.py3"));
  setNameFilters(filters);
  setNameFilterDisables(false);
  setFilter(QDir::Files | QDir::AllDirs | QDir::NoDotAndDotDot);

  AutoRunScripts();
}

void ScriptsFileSystemModel::AutoRunScripts()
{
  // In scripts dir, look for all files that start with an underscore
  // This is apparently very hard to do via the QFileSystemModel class,
  // because it does not compute subfolder files unless you expand the folder first.
  QString dir = QString::fromStdString(File::GetUserPath(D_SCRIPTS_IDX));
  QStringList nameFilter{QStringLiteral("_*.py"), QStringLiteral("_*.py3")};
  QDirIterator it = QDirIterator(dir, nameFilter, QDir::NoFilter, QDirIterator::Subdirectories);

  while (it.hasNext())
  {
    QFileInfo file = it.nextFileInfo();

    // Ignore __init__.py files, as these are used to make subfolder modules visible
    if (file.fileName() == QStringLiteral("__init__.py"))
      continue;

    Scripts::g_scripts[file.absoluteFilePath().toStdString()] = nullptr;
  }
}

Qt::ItemFlags ScriptsFileSystemModel::flags(const QModelIndex& index) const
{
  Qt::ItemFlags result = QAbstractItemModel::flags(index);
  if (index.column() == 0)
    result |= Qt::ItemIsUserCheckable;

  return result;
}

QVariant ScriptsFileSystemModel::data(const QModelIndex& index, int role) const
{
  if (!index.isValid())
    return QVariant();

  const bool is_file = !hasChildren(index);

  if (index.column() == 1)
  {
    if (!is_file)
      return QVariant();

    const QString path = filePath(index);
    if (role == Qt::DisplayRole)
      return ScriptFavoritesManager::Get().IsFavorite(path) ? QStringLiteral("★") :
                                                              QStringLiteral("☆");
    if (role == Qt::TextAlignmentRole)
      return Qt::AlignCenter;
    if (role == Qt::ToolTipRole)
      return ScriptFavoritesManager::Get().IsFavorite(path) ? tr("Remove favorite") :
                                                              tr("Add favorite");
    return QVariant();
  }

  // Hide filetype icons except for folders
  if (role == Qt::DecorationRole && is_file)
    return QVariant();

  if (role == Qt::CheckStateRole)
  {
    if (!is_file)
      return QVariant();

    return ScriptFavoritesManager::Get().IsScriptEnabled(filePath(index)) ? Qt::Checked :
                                                                            Qt::Unchecked;
  }

  return QFileSystemModel::data(index, role);
}

bool ScriptsFileSystemModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
  if (!index.isValid() || index.column() > 0)
    return false;

  switch (role)
  {
  case Qt::CheckStateRole:
  {
    ScriptFavoritesManager::Get().SetScriptEnabled(filePath(index),
                                                   static_cast<Qt::CheckState>(value.toUInt()) ==
                                                       Qt::Checked);

    emit ScriptsFileSystemModel::dataChanged(index, index, QList<int>{role});

    return true;
  }
  default:
    return false;
  }
}

void ScriptsFileSystemModel::Restart(const QModelIndex& index)
{
  if (!index.isValid())
    return;

  ScriptFavoritesManager::Get().RestartScript(filePath(index));
}
