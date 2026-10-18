// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Scripting/ScriptFavoritesWidget.h"

#include <QDir>
#include <QFileInfo>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMouseEvent>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <utility>

#include "Common/FileUtil.h"
#include "DolphinQt/Scripting/ScriptFavoritesManager.h"

namespace
{
constexpr int FAVORITES_WIDTH = 230;
constexpr int PATH_ROLE = Qt::UserRole;
}

class FavoriteScriptsList final : public QListWidget
{
public:
  explicit FavoriteScriptsList(QWidget* parent) : QListWidget(parent)
  {
    m_hold_timer.setSingleShot(true);
    m_hold_timer.setInterval(1000);
    connect(&m_hold_timer, &QTimer::timeout, this, [this] {
      if (!m_rearrange_enabled || !m_holding || !m_pressed_item)
        return;
      m_dragging = true;
      viewport()->setCursor(Qt::ClosedHandCursor);
    });
    setProperty("tas_favorite_reorder", true);
    viewport()->setProperty("tas_favorite_reorder", true);
  }

  void SetRearrangeEnabled(bool enabled)
  {
    m_rearrange_enabled = enabled;
    m_hold_timer.stop();
    m_holding = false;
    m_dragging = false;
    m_order_dirty = false;
    m_pressed_item = nullptr;
    viewport()->unsetCursor();
    setDragEnabled(false);
    setAcceptDrops(false);
    setDropIndicatorShown(false);
    setDragDropMode(QAbstractItemView::NoDragDrop);
    setSelectionMode(enabled ? QAbstractItemView::SingleSelection :
                               QAbstractItemView::NoSelection);
    clearSelection();
  }

  void SetOrderChangedCallback(std::function<void()> callback)
  {
    m_order_changed = std::move(callback);
  }

protected:
  void mousePressEvent(QMouseEvent* event) override
  {
    if (!m_rearrange_enabled || event->button() != Qt::LeftButton)
    {
      QListWidget::mousePressEvent(event);
      return;
    }
    m_pressed_item = itemAt(event->position().toPoint());
    if (!m_pressed_item || m_pressed_item->data(PATH_ROLE).toString().isEmpty())
      return;
    setCurrentItem(m_pressed_item);
    m_pressed_item->setSelected(true);
    m_holding = true;
    m_order_dirty = false;
    m_hold_timer.start();
    event->accept();
  }

  void mouseMoveEvent(QMouseEvent* event) override
  {
    if (!m_rearrange_enabled)
    {
      QListWidget::mouseMoveEvent(event);
      return;
    }
    if (!m_dragging || !(event->buttons() & Qt::LeftButton) || !m_pressed_item)
    {
      event->accept();
      return;
    }

    const QPoint position = event->position().toPoint();
    QListWidgetItem* target = itemAt(position);
    int destination = count();
    if (target)
    {
      destination = row(target);
      if (position.y() > visualItemRect(target).center().y())
        ++destination;
    }
    else if (position.y() < 0)
    {
      destination = 0;
    }

    const int source = row(m_pressed_item);
    if (source >= 0)
    {
      if (source < destination)
        --destination;
      destination = std::clamp(destination, 0, count() - 1);
      if (source != destination)
      {
        QListWidgetItem* moved = takeItem(source);
        insertItem(destination, moved);
        setCurrentItem(moved);
        moved->setSelected(true);
        scrollToItem(moved);
        m_order_dirty = true;
      }
    }
    event->accept();
  }

  void mouseReleaseEvent(QMouseEvent* event) override
  {
    if (!m_rearrange_enabled)
    {
      QListWidget::mouseReleaseEvent(event);
      return;
    }
    m_hold_timer.stop();
    const bool order_changed = m_dragging && m_order_dirty;
    viewport()->unsetCursor();
    m_holding = false;
    m_dragging = false;
    m_order_dirty = false;
    m_pressed_item = nullptr;
    event->accept();
    if (order_changed && m_order_changed)
      m_order_changed();
  }

private:
  QTimer m_hold_timer;
  QListWidgetItem* m_pressed_item = nullptr;
  std::function<void()> m_order_changed;
  bool m_rearrange_enabled = false;
  bool m_holding = false;
  bool m_dragging = false;
  bool m_order_dirty = false;
};

ScriptFavoritesWidget::ScriptFavoritesWidget(QWidget* parent) : QGroupBox(tr("Favorite Scripts"), parent)
{
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
  setMinimumWidth(FAVORITES_WIDTH);
  setMaximumWidth(FAVORITES_WIDTH);

  m_list = new FavoriteScriptsList(this);
  m_list->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_list->setWordWrap(true);
  m_list->setSelectionMode(QAbstractItemView::NoSelection);

  auto* layout = new QVBoxLayout;
  layout->addWidget(m_list);
  setLayout(layout);

  connect(m_list, &QListWidget::itemChanged, this, &ScriptFavoritesWidget::OnItemChanged);
  m_list->SetOrderChangedCallback([this] {
    QStringList paths;
    for (int row = 0; row < m_list->count(); ++row)
    {
      if (QListWidgetItem* item = m_list->item(row))
      {
        const QString path = item->data(PATH_ROLE).toString();
        if (!path.isEmpty())
          paths.append(path);
      }
    }
    ScriptFavoritesManager::Get().SetFavoritesOrder(paths);
  });

  auto& manager = ScriptFavoritesManager::Get();
  connect(&manager, &ScriptFavoritesManager::FavoritesChanged, this, &ScriptFavoritesWidget::Reload);
  connect(&manager, &ScriptFavoritesManager::ScriptStatesChanged, this, &ScriptFavoritesWidget::Reload);

  Reload();
}

void ScriptFavoritesWidget::SetRearrangeEnabled(bool enabled)
{
  m_list->SetRearrangeEnabled(enabled);
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
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
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
  return QFileInfo(absolute_path).fileName();
}
