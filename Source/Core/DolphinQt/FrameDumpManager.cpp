// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/FrameDumpManager.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include <QApplication>
#include <QAbstractItemView>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFormLayout>
#include <QGraphicsObject>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsView>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSplitter>
#include <QSpinBox>
#include <QStyle>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "AudioCommon/AudioCommon.h"
#include "AudioCommon/Mixer.h"
#include "AudioCommon/SoundStream.h"
#include "Core/Core.h"
#include "Core/Config/MainSettings.h"
#include "Core/CoreTiming.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/System.h"

#include "DolphinQt/GBAWidget.h"
#include "DolphinQt/RenderWidget.h"
#include "DolphinQt/Scripting/InputDisplayWidget.h"

#include "VideoCommon/FrameDumper.h"
#include "VideoCommon/OnScreenDisplay.h"

#if defined(HAVE_FFMPEG)
#include "VideoCommon/FrameDumpFFMpeg.h"
#endif

namespace
{
constexpr int PREVIEW_INTERVAL_MS = 33;
constexpr qreal ITEM_MINIMUM_WIDTH = 80.0;
constexpr qreal ITEM_MINIMUM_HEIGHT = 45.0;
constexpr qreal RESIZE_HANDLE_SIZE = 34.0;
constexpr qreal CANVAS_MARGIN = 32.0;
constexpr qreal SNAP_DISTANCE_PIXELS = 12.0;
constexpr int GAME_SOURCE_UPSCALE = 1;
constexpr int DEFAULT_WINDOW_UPSCALE = 4;
constexpr int DEFAULT_GBA_UPSCALE = 6;
constexpr int MAX_SOURCE_UPSCALE = 12;

class CompositionView final : public QGraphicsView
{
public:
  explicit CompositionView(QGraphicsScene* scene, QWidget* parent = nullptr)
      : QGraphicsView(scene, parent)
  {
    setAlignment(Qt::AlignCenter);
    setFrameShape(QFrame::NoFrame);
    setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
  }

  void SetPreviewEnabled(bool enabled)
  {
    if (m_preview_enabled == enabled)
      return;
    m_preview_enabled = enabled;
    viewport()->update();
  }

  void FitCanvas()
  {
    if (!scene() || scene()->sceneRect().isEmpty())
      return;
    fitInView(scene()->sceneRect().adjusted(-CANVAS_MARGIN, -CANVAS_MARGIN, CANVAS_MARGIN,
                                            CANVAS_MARGIN),
              Qt::KeepAspectRatio);
  }

protected:
  void resizeEvent(QResizeEvent* event) override
  {
    QGraphicsView::resizeEvent(event);
    FitCanvas();
  }

  void drawBackground(QPainter* painter, const QRectF& rect) override
  {
    painter->fillRect(rect, QColor(24, 27, 32));
    if (!scene())
      return;

    const QRectF canvas = scene()->sceneRect();
    painter->fillRect(canvas, m_preview_enabled ? Qt::black : QColor(58, 61, 68));

    if (!m_preview_enabled)
      return;

    const qreal spacing = std::max<qreal>(32.0, canvas.height() / 18.0);
    painter->setPen(QPen(QColor(255, 255, 255, 24), 0));
    for (qreal x = canvas.left() + spacing; x < canvas.right(); x += spacing)
      painter->drawLine(QPointF(x, canvas.top()), QPointF(x, canvas.bottom()));
    for (qreal y = canvas.top() + spacing; y < canvas.bottom(); y += spacing)
      painter->drawLine(QPointF(canvas.left(), y), QPointF(canvas.right(), y));
  }

private:
  bool m_preview_enabled = true;
};

class LayerListWidget final : public QListWidget
{
public:
  explicit LayerListWidget(QWidget* parent = nullptr) : QListWidget(parent)
  {
    setSelectionMode(QAbstractItemView::SingleSelection);
    setDragDropMode(QAbstractItemView::NoDragDrop);
    setDragEnabled(false);
    setAcceptDrops(false);
    setStyleSheet(QStringLiteral(
        "QListWidget::item:selected { border: 2px solid #3599ff; background: rgba(53, 153, 255, "
        "70); }"));
  }

  void SetOrderChangedCallback(std::function<void()> callback)
  {
    m_order_changed = std::move(callback);
  }

protected:
  void mousePressEvent(QMouseEvent* event) override
  {
    if (event->button() == Qt::LeftButton)
    {
      m_drag_start_position = event->pos();
      m_drag_start_row = row(itemAt(event->pos()));
      m_drop_row = m_drag_start_row;
      m_dragging = false;
    }
    QListWidget::mousePressEvent(event);
  }

  void mouseMoveEvent(QMouseEvent* event) override
  {
    if ((event->buttons() & Qt::LeftButton) && m_drag_start_row >= 0)
    {
      if (!m_dragging && (event->pos() - m_drag_start_position).manhattanLength() >=
                             QApplication::startDragDistance())
      {
        m_dragging = true;
      }

      if (m_dragging)
      {
        const int row = InsertionRow(event->pos());
        if (row != m_drop_row)
        {
          m_drop_row = row;
          viewport()->update();
        }
        event->accept();
        return;
      }
    }
    QListWidget::mouseMoveEvent(event);
  }

  void mouseReleaseEvent(QMouseEvent* event) override
  {
    if (event->button() == Qt::LeftButton && m_dragging)
    {
      MoveRow(m_drag_start_row, InsertionRow(event->pos()));
      m_dragging = false;
      m_drag_start_row = -1;
      m_drop_row = -1;
      viewport()->update();
      event->accept();
      return;
    }

    m_dragging = false;
    m_drag_start_row = -1;
    m_drop_row = -1;
    QListWidget::mouseReleaseEvent(event);
  }

  void paintEvent(QPaintEvent* event) override
  {
    QListWidget::paintEvent(event);
    if (!m_dragging || m_drop_row < 0)
      return;

    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QPen(QColor(53, 153, 255), 3.0));
    const int y = DropY(m_drop_row);
    painter.drawLine(QPoint(4, y), QPoint(viewport()->width() - 4, y));
  }

private:
  int InsertionRow(const QPoint& position) const
  {
    if (count() == 0)
      return 0;

    QListWidgetItem* item = itemAt(position);
    if (!item)
      return position.y() < 0 ? 0 : count();

    const QRect rect = visualItemRect(item);
    const int item_row = row(item);
    return std::clamp(item_row + (position.y() > rect.center().y() ? 1 : 0), 0, count());
  }

  int DropY(int row) const
  {
    if (count() == 0)
      return 0;
    if (row <= 0)
      return visualItemRect(item(0)).top();
    if (row >= count())
      return visualItemRect(item(count() - 1)).bottom();
    return visualItemRect(item(row)).top();
  }

  void MoveRow(int from, int to)
  {
    if (from < 0 || from >= count())
      return;
    if (to > from)
      --to;
    to = std::clamp(to, 0, count() - 1);
    if (from == to)
      return;

    QListWidgetItem* moved = takeItem(from);
    insertItem(to, moved);
    setCurrentItem(moved);
    if (m_order_changed)
      m_order_changed();
  }

  std::function<void()> m_order_changed;
  QPoint m_drag_start_position;
  int m_drag_start_row = -1;
  int m_drop_row = -1;
  bool m_dragging = false;
};

class CompositionItem final : public QGraphicsObject
{
public:
  CompositionItem(QString name, std::function<void()> geometry_changed)
      : m_name(std::move(name)), m_geometry_changed(std::move(geometry_changed))
  {
    setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
    setAcceptHoverEvents(true);
    setCursor(Qt::OpenHandCursor);
    m_size = QSizeF(480.0, 270.0);
  }

  QRectF boundingRect() const override
  {
    const qreal margin = RESIZE_HANDLE_SIZE / 2.0 + 2.0;
    return QRectF(-margin, -margin, m_size.width() + margin * 2.0,
                  m_size.height() + margin * 2.0);
  }

  QRectF LayoutRect() const { return QRectF(pos(), m_size); }

  void SetPreviewEnabled(bool enabled)
  {
    if (m_preview_enabled == enabled)
      return;
    m_preview_enabled = enabled;
    update();
  }

  void SetSnappingEnabled(bool enabled) { m_snapping_enabled = enabled; }

  void ApplyLayout(const QRectF& rect)
  {
    m_applying_layout = true;
    prepareGeometryChange();
    m_size = QSizeF(std::max(rect.width(), ITEM_MINIMUM_WIDTH),
                    std::max(rect.height(), ITEM_MINIMUM_HEIGHT));
    setPos(ClampPosition(rect.topLeft()));
    m_applying_layout = false;
    update();
  }

  void SetPixmap(QPixmap pixmap)
  {
    m_pixmap = std::move(pixmap);
    update();
  }

  const QPixmap& GetPixmap() const { return m_pixmap; }

protected:
  void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override
  {
    const QRectF bounds(QPointF{}, m_size);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
    if (m_preview_enabled && !m_pixmap.isNull())
    {
      painter->drawPixmap(bounds, m_pixmap, m_pixmap.rect());
    }
    else
    {
      painter->fillRect(bounds, QColor(40, 44, 50));
      painter->setPen(QColor(190, 194, 201));
      painter->drawText(bounds, Qt::AlignCenter, m_name);
    }

    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(isSelected() ? QColor(53, 153, 255) : QColor(255, 255, 255, 90),
                         isSelected() ? 4.0 : 2.0));
    painter->drawRect(bounds.adjusted(1.0, 1.0, -1.0, -1.0));

    if (isSelected())
    {
      painter->setPen(QPen(Qt::white, 2.0));
      painter->setBrush(QColor(53, 153, 255));
      for (const ResizeCorner corner : {ResizeCorner::TopLeft, ResizeCorner::TopRight,
                                        ResizeCorner::BottomLeft, ResizeCorner::BottomRight,
                                        ResizeCorner::Top, ResizeCorner::Right,
                                        ResizeCorner::Bottom, ResizeCorner::Left})
      {
        painter->drawRect(ResizeHandleRect(corner));
      }
    }
  }

  void hoverMoveEvent(QGraphicsSceneHoverEvent* event) override
  {
    const ResizeCorner corner = ResizeCornerAt(event->pos());
    setCursor(CursorForCorner(corner));
    QGraphicsObject::hoverMoveEvent(event);
  }

  void mousePressEvent(QGraphicsSceneMouseEvent* event) override
  {
    const ResizeCorner corner = ResizeCornerAt(event->pos());
    if (event->button() == Qt::LeftButton && corner != ResizeCorner::None)
    {
      m_resizing = true;
      m_resize_corner = corner;
      m_resize_start_rect = LayoutRect();
      setCursor(CursorForCorner(corner));
      event->accept();
      return;
    }
    setCursor(Qt::ClosedHandCursor);
    QGraphicsObject::mousePressEvent(event);
  }

  void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override
  {
    if (!m_resizing)
    {
      QGraphicsObject::mouseMoveEvent(event);
      return;
    }

    const QRectF resized = CalculateResizeRect(event->scenePos());
    m_applying_layout = true;
    prepareGeometryChange();
    m_size = resized.size();
    setPos(resized.topLeft());
    m_applying_layout = false;
    update();
    if (m_geometry_changed)
      m_geometry_changed();
    event->accept();
  }

  void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override
  {
    if (m_resizing)
    {
      m_resizing = false;
      setCursor(CursorForCorner(m_resize_corner));
      m_resize_corner = ResizeCorner::None;
      event->accept();
      return;
    }
    setCursor(Qt::OpenHandCursor);
    QGraphicsObject::mouseReleaseEvent(event);
  }

  QVariant itemChange(GraphicsItemChange change, const QVariant& value) override
  {
    if (change == ItemPositionChange && scene())
    {
      const QPointF clamped = ClampPosition(value.toPointF());
      return m_applying_layout || m_resizing || !m_snapping_enabled ? clamped :
                                                                    SnapMovePosition(clamped);
    }
    if (change == ItemPositionHasChanged && m_geometry_changed)
      m_geometry_changed();
    return QGraphicsObject::itemChange(change, value);
  }

private:
  enum class ResizeCorner
  {
    None,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    Top,
    Right,
    Bottom,
    Left,
  };

  QRectF ResizeHandleRect(ResizeCorner corner) const
  {
    QPointF center;
    switch (corner)
    {
    case ResizeCorner::TopLeft:
      center = QPointF(0.0, 0.0);
      break;
    case ResizeCorner::TopRight:
      center = QPointF(m_size.width(), 0.0);
      break;
    case ResizeCorner::BottomLeft:
      center = QPointF(0.0, m_size.height());
      break;
    case ResizeCorner::BottomRight:
      center = QPointF(m_size.width(), m_size.height());
      break;
    case ResizeCorner::Top:
      center = QPointF(m_size.width() / 2.0, 0.0);
      break;
    case ResizeCorner::Right:
      center = QPointF(m_size.width(), m_size.height() / 2.0);
      break;
    case ResizeCorner::Bottom:
      center = QPointF(m_size.width() / 2.0, m_size.height());
      break;
    case ResizeCorner::Left:
      center = QPointF(0.0, m_size.height() / 2.0);
      break;
    case ResizeCorner::None:
      return {};
    }
    const qreal half = RESIZE_HANDLE_SIZE / 2.0;
    return QRectF(center.x() - half, center.y() - half, RESIZE_HANDLE_SIZE,
                  RESIZE_HANDLE_SIZE);
  }

  ResizeCorner ResizeCornerAt(const QPointF& point) const
  {
    for (const ResizeCorner corner : {ResizeCorner::TopLeft, ResizeCorner::TopRight,
                                      ResizeCorner::BottomLeft, ResizeCorner::BottomRight,
                                      ResizeCorner::Top, ResizeCorner::Right,
                                      ResizeCorner::Bottom, ResizeCorner::Left})
    {
      if (ResizeHandleRect(corner).contains(point))
        return corner;
    }
    return ResizeCorner::None;
  }

  static Qt::CursorShape CursorForCorner(ResizeCorner corner)
  {
    switch (corner)
    {
    case ResizeCorner::TopLeft:
    case ResizeCorner::BottomRight:
      return Qt::SizeFDiagCursor;
    case ResizeCorner::TopRight:
    case ResizeCorner::BottomLeft:
      return Qt::SizeBDiagCursor;
    case ResizeCorner::Top:
    case ResizeCorner::Bottom:
      return Qt::SizeVerCursor;
    case ResizeCorner::Left:
    case ResizeCorner::Right:
      return Qt::SizeHorCursor;
    case ResizeCorner::None:
      return Qt::OpenHandCursor;
    }
    return Qt::OpenHandCursor;
  }

  qreal SnapDistance() const
  {
    if (!scene() || scene()->views().isEmpty())
      return SNAP_DISTANCE_PIXELS;
    const qreal scale = scene()->views().front()->transform().m11();
    return scale > 0.0 ? SNAP_DISTANCE_PIXELS / scale : SNAP_DISTANCE_PIXELS;
  }

  std::vector<const CompositionItem*> OtherItems() const
  {
    std::vector<const CompositionItem*> items;
    if (!scene())
      return items;
    for (QGraphicsItem* graphics_item : scene()->items())
    {
      const auto* item = dynamic_cast<const CompositionItem*>(graphics_item);
      if (item && item != this && item->isVisible())
        items.push_back(item);
    }
    return items;
  }

  QPointF SnapMovePosition(const QPointF& requested) const
  {
    if (!scene())
      return requested;

    QRectF proposed(requested, m_size);
    const QRectF canvas = scene()->sceneRect();
    const qreal threshold = SnapDistance();
    qreal best_dx = threshold + 1.0;
    qreal best_dy = threshold + 1.0;

    const auto consider_x = [&](qreal delta) {
      if (std::abs(delta) <= threshold && std::abs(delta) < std::abs(best_dx))
        best_dx = delta;
    };
    const auto consider_y = [&](qreal delta) {
      if (std::abs(delta) <= threshold && std::abs(delta) < std::abs(best_dy))
        best_dy = delta;
    };

    consider_x(canvas.left() - proposed.left());
    consider_x(canvas.right() - proposed.right());
    consider_y(canvas.top() - proposed.top());
    consider_y(canvas.bottom() - proposed.bottom());

    const std::vector<const CompositionItem*> others = OtherItems();
    for (const CompositionItem* other : others)
    {
      const QRectF target = other->LayoutRect();
      consider_x(target.left() - proposed.left());
      consider_x(target.right() - proposed.right());
      consider_x(target.left() - proposed.right());
      consider_x(target.right() - proposed.left());
      consider_y(target.top() - proposed.top());
      consider_y(target.bottom() - proposed.bottom());
      consider_y(target.top() - proposed.bottom());
      consider_y(target.bottom() - proposed.top());
    }

    if (std::abs(best_dx) <= threshold)
      proposed.translate(best_dx, 0.0);
    if (std::abs(best_dy) <= threshold)
      proposed.translate(0.0, best_dy);

    // Treat overlap as a collision and place the moving item flush against the nearest edge.
    for (const CompositionItem* other : others)
    {
      const QRectF target = other->LayoutRect();
      if (!proposed.intersects(target))
        continue;

      const qreal move_left = target.left() - proposed.right();
      const qreal move_right = target.right() - proposed.left();
      const qreal move_up = target.top() - proposed.bottom();
      const qreal move_down = target.bottom() - proposed.top();
      const qreal horizontal = std::abs(move_left) < std::abs(move_right) ? move_left : move_right;
      const qreal vertical = std::abs(move_up) < std::abs(move_down) ? move_up : move_down;
      if (std::abs(horizontal) < std::abs(vertical))
        proposed.translate(horizontal, 0.0);
      else
        proposed.translate(0.0, vertical);
    }

    return ClampPosition(proposed.topLeft());
  }

  QRectF ResizeRectForWidth(qreal width, qreal aspect) const
  {
    const qreal height = width / aspect;
    const QPointF center = m_resize_start_rect.center();
    const bool left = m_resize_corner == ResizeCorner::TopLeft ||
                      m_resize_corner == ResizeCorner::BottomLeft ||
                      m_resize_corner == ResizeCorner::Left;
    const bool top = m_resize_corner == ResizeCorner::TopLeft ||
                     m_resize_corner == ResizeCorner::TopRight ||
                     m_resize_corner == ResizeCorner::Top;
    const bool horizontal_edge =
        m_resize_corner == ResizeCorner::Left || m_resize_corner == ResizeCorner::Right;
    const bool vertical_edge =
        m_resize_corner == ResizeCorner::Top || m_resize_corner == ResizeCorner::Bottom;

    if (horizontal_edge)
    {
      return QRectF(left ? m_resize_start_rect.right() - width : m_resize_start_rect.left(),
                    center.y() - height / 2.0, width, height);
    }
    if (vertical_edge)
    {
      return QRectF(center.x() - width / 2.0,
                    top ? m_resize_start_rect.bottom() - height : m_resize_start_rect.top(),
                    width, height);
    }

    return QRectF(left ? m_resize_start_rect.right() - width : m_resize_start_rect.left(),
                  top ? m_resize_start_rect.bottom() - height : m_resize_start_rect.top(), width,
                  height);
  }

  QRectF SnapResizeRect(const QRectF& requested, qreal aspect, qreal minimum_width) const
  {
    if (!m_snapping_enabled || !scene())
      return requested;

    const QRectF canvas = scene()->sceneRect();
    const qreal threshold = SnapDistance();
    qreal best_width = requested.width();
    qreal best_delta = threshold + 1.0;

    const bool left = m_resize_corner == ResizeCorner::TopLeft ||
                      m_resize_corner == ResizeCorner::BottomLeft ||
                      m_resize_corner == ResizeCorner::Left;
    const bool top = m_resize_corner == ResizeCorner::TopLeft ||
                     m_resize_corner == ResizeCorner::TopRight ||
                     m_resize_corner == ResizeCorner::Top;
    const bool horizontal_edge =
        m_resize_corner == ResizeCorner::Left || m_resize_corner == ResizeCorner::Right;
    const bool vertical_edge =
        m_resize_corner == ResizeCorner::Top || m_resize_corner == ResizeCorner::Bottom;
    const bool resizes_x = !vertical_edge;
    const bool resizes_y = !horizontal_edge;

    const auto consider_width = [&](qreal width, qreal delta) {
      if (std::abs(delta) > threshold || std::abs(delta) >= std::abs(best_delta) ||
          width < minimum_width)
      {
        return;
      }

      const QRectF snapped = ResizeRectForWidth(width, aspect);
      if (!canvas.adjusted(-0.5, -0.5, 0.5, 0.5).contains(snapped))
        return;

      best_width = width;
      best_delta = delta;
    };

    const auto consider_x = [&](qreal target) {
      if (!resizes_x)
        return;
      if (left)
        consider_width(m_resize_start_rect.right() - target, target - requested.left());
      else
        consider_width(target - m_resize_start_rect.left(), target - requested.right());
    };
    const auto consider_y = [&](qreal target) {
      if (!resizes_y)
        return;
      if (top)
        consider_width((m_resize_start_rect.bottom() - target) * aspect,
                       target - requested.top());
      else
        consider_width((target - m_resize_start_rect.top()) * aspect,
                       target - requested.bottom());
    };

    consider_x(canvas.left());
    consider_x(canvas.right());
    consider_y(canvas.top());
    consider_y(canvas.bottom());
    for (const CompositionItem* other : OtherItems())
    {
      const QRectF target = other->LayoutRect();
      consider_x(target.left());
      consider_x(target.right());
      consider_y(target.top());
      consider_y(target.bottom());
    }

    return std::abs(best_delta) <= threshold ? ResizeRectForWidth(best_width, aspect) : requested;
  }

  QRectF CalculateResizeRect(const QPointF& scene_point) const
  {
    const qreal aspect = !m_pixmap.isNull() && m_pixmap.height() > 0 ?
                             static_cast<qreal>(m_pixmap.width()) / m_pixmap.height() :
                             m_resize_start_rect.width() / m_resize_start_rect.height();
    const qreal minimum_width = std::max(ITEM_MINIMUM_WIDTH, ITEM_MINIMUM_HEIGHT * aspect);
    const QPointF center = m_resize_start_rect.center();

    const bool left = m_resize_corner == ResizeCorner::TopLeft ||
                      m_resize_corner == ResizeCorner::BottomLeft ||
                      m_resize_corner == ResizeCorner::Left;
    const bool top = m_resize_corner == ResizeCorner::TopLeft ||
                     m_resize_corner == ResizeCorner::TopRight ||
                     m_resize_corner == ResizeCorner::Top;
    const bool horizontal_edge =
        m_resize_corner == ResizeCorner::Left || m_resize_corner == ResizeCorner::Right;
    const bool vertical_edge =
        m_resize_corner == ResizeCorner::Top || m_resize_corner == ResizeCorner::Bottom;

    qreal width = m_resize_start_rect.width();
    if (horizontal_edge)
      width = std::abs(scene_point.x() - (left ? m_resize_start_rect.right() :
                                                 m_resize_start_rect.left()));
    else if (vertical_edge)
      width = std::abs(scene_point.y() - (top ? m_resize_start_rect.bottom() :
                                               m_resize_start_rect.top())) *
              aspect;
    else
    {
      const QPointF fixed(left ? m_resize_start_rect.right() : m_resize_start_rect.left(),
                          top ? m_resize_start_rect.bottom() : m_resize_start_rect.top());
      width = std::max(std::abs(scene_point.x() - fixed.x()),
                       std::abs(scene_point.y() - fixed.y()) * aspect);
    }
    width = std::max(width, minimum_width);

    if (scene())
    {
      const QRectF canvas = scene()->sceneRect();
      qreal max_width = canvas.width();
      if (horizontal_edge)
      {
        max_width = left ? m_resize_start_rect.right() - canvas.left() :
                           canvas.right() - m_resize_start_rect.left();
        const qreal max_height_from_center =
            2.0 * std::min(center.y() - canvas.top(), canvas.bottom() - center.y());
        max_width = std::min(max_width, max_height_from_center * aspect);
      }
      else if (vertical_edge)
      {
        const qreal max_height = top ? m_resize_start_rect.bottom() - canvas.top() :
                                       canvas.bottom() - m_resize_start_rect.top();
        max_width = max_height * aspect;
        const qreal max_width_from_center =
            2.0 * std::min(center.x() - canvas.left(), canvas.right() - center.x());
        max_width = std::min(max_width, max_width_from_center);
      }
      else
      {
        const QPointF fixed(left ? m_resize_start_rect.right() : m_resize_start_rect.left(),
                            top ? m_resize_start_rect.bottom() : m_resize_start_rect.top());
        const qreal horizontal_limit = left ? fixed.x() - canvas.left() : canvas.right() - fixed.x();
        const qreal vertical_limit =
            (top ? fixed.y() - canvas.top() : canvas.bottom() - fixed.y()) * aspect;
        max_width = std::min(horizontal_limit, vertical_limit);
      }
      width = std::min(width, std::max(minimum_width, max_width));
    }

    return SnapResizeRect(ResizeRectForWidth(width, aspect), aspect, minimum_width);
  }

  QPointF ClampPosition(const QPointF& requested) const
  {
    if (!scene())
      return requested;
    const QRectF available = scene()->sceneRect();
    return QPointF(std::clamp(requested.x(), available.left(),
                              std::max(available.left(), available.right() - m_size.width())),
                   std::clamp(requested.y(), available.top(),
                              std::max(available.top(), available.bottom() - m_size.height())));
  }

  QString m_name;
  QPixmap m_pixmap;
  QSizeF m_size;
  std::function<void()> m_geometry_changed;
  bool m_resizing = false;
  bool m_applying_layout = false;
  bool m_preview_enabled = true;
  bool m_snapping_enabled = true;
  ResizeCorner m_resize_corner = ResizeCorner::None;
  QRectF m_resize_start_rect;
};

bool IsGameWidget(QWidget* widget)
{
  return qobject_cast<RenderWidget*>(widget) != nullptr;
}

bool IsGBAWidget(QWidget* widget)
{
#if defined(HAS_LIBMGBA)
  return qobject_cast<GBAWidget*>(widget) != nullptr;
#else
  return false;
#endif
}

int DefaultUpscaleForWidget(QWidget* widget)
{
  if (IsGameWidget(widget))
    return GAME_SOURCE_UPSCALE;
  if (IsGBAWidget(widget))
    return DEFAULT_GBA_UPSCALE;
  return DEFAULT_WINDOW_UPSCALE;
}

std::string SanitizeDumpName(const QString& name)
{
  QString sanitized;
  sanitized.reserve(name.size());
  for (const QChar ch : name)
  {
    if (ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('-'))
      sanitized.append(ch);
    else if (ch.isSpace() || ch == QLatin1Char('.') || ch == QLatin1Char('|') ||
             ch == QLatin1Char(':'))
      sanitized.append(QLatin1Char('_'));
  }

  sanitized = sanitized.simplified();
  sanitized.replace(QLatin1Char(' '), QLatin1Char('_'));
  while (sanitized.contains(QStringLiteral("__")))
    sanitized.replace(QStringLiteral("__"), QStringLiteral("_"));
  sanitized = sanitized.trimmed();
  if (sanitized.isEmpty())
    sanitized = QStringLiteral("Source");
  return sanitized.left(80).toStdString();
}

QImage UpscaleNearest(const QImage& image, int scale)
{
  if (image.isNull() || scale <= 1)
    return image;

  return image.scaled(image.width() * scale, image.height() * scale, Qt::IgnoreAspectRatio,
                      Qt::FastTransformation);
}

QImage FlattenOnBlack(const QImage& image)
{
  if (image.isNull())
    return {};

  QImage flattened(image.size(), QImage::Format_RGBA8888);
  flattened.fill(Qt::black);
  QPainter painter(&flattened);
  painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
  painter.drawImage(0, 0, image);
  return flattened;
}

QRect RoundedCanvasRect(const QRectF& rect, const QSize& canvas_size)
{
  QRect rounded(qRound(rect.x()), qRound(rect.y()), qRound(rect.width()), qRound(rect.height()));
  rounded = rounded.normalized();
  rounded.setLeft(std::clamp(rounded.left(), 0, canvas_size.width()));
  rounded.setTop(std::clamp(rounded.top(), 0, canvas_size.height()));
  rounded.setRight(std::clamp(rounded.right(), 0, canvas_size.width() - 1));
  rounded.setBottom(std::clamp(rounded.bottom(), 0, canvas_size.height() - 1));
  return rounded;
}

QImage CaptureWidgetOffscreen(QWidget* widget, const QImage& game_frame, int upscale_factor)
{
  if (!widget || !widget->isVisible() || widget->width() <= 0 || widget->height() <= 0)
    return {};

  if (IsGameWidget(widget))
    return game_frame;

#if defined(HAS_LIBMGBA)
  if (const auto* gba = qobject_cast<GBAWidget*>(widget))
  {
    std::vector<u32> video_buffer;
    u32 width = 0;
    u32 height = 0;
    QImage frame;
    if (HW::GBA::Core::GetLatestVideoFrame(gba->GetDeviceNumber(), &video_buffer, &width,
                                           &height))
    {
      frame = QImage(reinterpret_cast<const uchar*>(video_buffer.data()), static_cast<int>(width),
                     static_cast<int>(height), QImage::Format_ARGB32)
                  .convertToFormat(QImage::Format_RGB32)
                  .rgbSwapped();
    }
    if (frame.isNull())
      frame = GBAWidget::GetLatestFrameForDevice(gba->GetDeviceNumber());
    if (frame.isNull())
      frame = gba->GetCurrentFrame();
    return UpscaleNearest(frame, upscale_factor);
  }
#endif

  if (auto* input_display = qobject_cast<InputDisplayWidget*>(widget))
    input_display->RefreshState();

  const qreal render_scale =
      widget->devicePixelRatioF() * static_cast<qreal>(std::max(1, upscale_factor));
  QImage image((QSizeF(widget->size()) * render_scale).toSize(),
               QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  painter.scale(render_scale, render_scale);
  widget->render(&painter);
  return image;
}
}  // namespace

class FrameDumpManager::Impl
{
public:
  explicit Impl(FrameDumpManager* owner) : m_owner(owner)
  {
    BuildUi();
    LoadSettings();
    RefreshSources();
    RefreshPreviews();

    QObject::connect(&m_preview_timer, &QTimer::timeout, m_owner, [this] {
      if (m_dumping.load() || m_preview_enabled.load())
        RegisterFrameConsumer();
      else
        UnregisterFrameConsumer();
      if (++m_discovery_counter >= 10)
      {
        m_discovery_counter = 0;
        RefreshSources();
      }
      RefreshPreviews();
    });

#if !defined(HAVE_FFMPEG)
    m_dump_button->setEnabled(false);
    m_dump_button->setToolTip(
        FrameDumpManager::tr("This build does not include FFmpeg frame dumping support."));
#endif
  }

  ~Impl()
  {
    m_preview_timer.stop();
    StopDump();
    UnregisterFrameConsumer();
    SaveSettings();
  }

  void OnShown()
  {
    if (m_show_preview->isChecked())
      RegisterFrameConsumer();
    RefreshSources();
    RefreshPreviews();
    m_preview_timer.start(PREVIEW_INTERVAL_MS);
    m_view->FitCanvas();
  }

  void OnClosed()
  {
    m_preview_timer.stop();
    StopDump();
    UnregisterFrameConsumer();
    SaveSettings();
  }

private:
  struct SavedSource
  {
    QRectF rect;
    bool visible = true;
    qreal z = 0.0;
    int upscale_factor = 0;
  };

  struct SourceEntry
  {
    QPointer<QWidget> widget;
    QString label;
    QListWidgetItem* list_item = nullptr;
    CompositionItem* composition_item = nullptr;
    int upscale_factor = DEFAULT_WINDOW_UPSCALE;
    bool automatically_sized = false;
  };

#if defined(HAVE_FFMPEG)
  struct SourceDump
  {
    std::unique_ptr<FFMpegFrameDump> dump;
    u64 frame_count = 0;
    u64 start_ticks = 0;
    bool failed = false;
  };
#endif

  void BuildUi()
  {
    m_owner->setWindowTitle(FrameDumpManager::tr("Frame Dump Manager"));
    m_owner->setMinimumSize(860, 560);
    m_owner->resize(1120, 720);

    auto* root = new QVBoxLayout(m_owner);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    auto* toolbar = new QHBoxLayout;
    toolbar->setSpacing(6);
    toolbar->addWidget(new QLabel(FrameDumpManager::tr("Resolution"), m_owner));

    m_resolution_combo = new QComboBox(m_owner);
    m_resolution_combo->addItem(FrameDumpManager::tr("720p"), 720);
    m_resolution_combo->addItem(FrameDumpManager::tr("1080p"), 1080);
    m_resolution_combo->addItem(FrameDumpManager::tr("1440p"), 1440);
    m_resolution_combo->addItem(FrameDumpManager::tr("2160p"), 2160);
    toolbar->addWidget(m_resolution_combo);

    auto* aspect_group = new QButtonGroup(m_owner);
    m_aspect_4_3 = new QToolButton(m_owner);
    m_aspect_4_3->setText(QStringLiteral("4:3"));
    m_aspect_4_3->setCheckable(true);
    m_aspect_16_9 = new QToolButton(m_owner);
    m_aspect_16_9->setText(QStringLiteral("16:9"));
    m_aspect_16_9->setCheckable(true);
    aspect_group->setExclusive(true);
    aspect_group->addButton(m_aspect_4_3);
    aspect_group->addButton(m_aspect_16_9);
    toolbar->addWidget(m_aspect_4_3);
    toolbar->addWidget(m_aspect_16_9);

    m_lock_aspect_ratio = new QCheckBox(FrameDumpManager::tr("Lock Aspect Ratio"), m_owner);
    m_lock_aspect_ratio->setToolTip(FrameDumpManager::tr(
        "Use the selected source window's current aspect ratio for the output canvas."));
    toolbar->addWidget(m_lock_aspect_ratio);

    m_output_label = new QLabel(m_owner);
    toolbar->addWidget(m_output_label);
    toolbar->addStretch();

    m_snap_to_edges = new QCheckBox(FrameDumpManager::tr("Snap"), m_owner);
    m_snap_to_edges->setToolTip(
        FrameDumpManager::tr("Snap sources to canvas and edges while moving."));
    toolbar->addWidget(m_snap_to_edges);

    auto* reset_button = new QPushButton(FrameDumpManager::tr("Reset Layout"), m_owner);
    reset_button->setIcon(m_owner->style()->standardIcon(QStyle::SP_DialogResetButton));
    toolbar->addWidget(reset_button);

    m_dump_audio = new QCheckBox(FrameDumpManager::tr("Dump Audio"), m_owner);
    m_dump_audio->setToolTip(FrameDumpManager::tr(
        "Dump DSP, DTK, and connected GBA audio as separate WAV files."));
    toolbar->addWidget(m_dump_audio);

    m_dump_selected_windows =
        new QCheckBox(FrameDumpManager::tr("Dump Windows Separately"), m_owner);
    m_dump_selected_windows->setToolTip(FrameDumpManager::tr(
        "Dump each visible source window as a separate video at its captured source resolution."));
    toolbar->addWidget(m_dump_selected_windows);

    m_show_preview = new QCheckBox(FrameDumpManager::tr("Show Preview"), m_owner);
    m_show_preview->setToolTip(FrameDumpManager::tr(
        "Show the live editor preview. Turn this off while dumping to reduce lag."));
    toolbar->addWidget(m_show_preview);

    m_dump_button = new QPushButton(FrameDumpManager::tr("Start Dump"), m_owner);
    m_dump_button->setIcon(m_owner->style()->standardIcon(QStyle::SP_MediaPlay));
    toolbar->addWidget(m_dump_button);
    root->addLayout(toolbar);

    auto* splitter = new QSplitter(Qt::Horizontal, m_owner);
    auto* source_panel = new QWidget(splitter);
    source_panel->setMinimumWidth(210);
    auto* source_layout = new QVBoxLayout(source_panel);
    source_layout->setContentsMargins(0, 0, 0, 0);
    auto* tabs = new QTabWidget(source_panel);
    auto* windows_tab = new QWidget(tabs);
    auto* windows_layout = new QVBoxLayout(windows_tab);
    windows_layout->setContentsMargins(0, 0, 0, 0);
    windows_layout->addWidget(new QLabel(FrameDumpManager::tr("Visible Windows"), windows_tab));
    m_source_list = new LayerListWidget(windows_tab);
    windows_layout->addWidget(m_source_list);
    tabs->addTab(windows_tab, FrameDumpManager::tr("Windows"));

    auto* settings_tab = new QWidget(tabs);
    auto* settings_layout = new QFormLayout(settings_tab);
    settings_layout->setContentsMargins(8, 8, 8, 8);
    settings_layout->setSpacing(8);
    m_settings_source_combo = new QComboBox(settings_tab);
    settings_layout->addRow(FrameDumpManager::tr("Source"), m_settings_source_combo);
    m_upscale_spin = new QSpinBox(settings_tab);
    m_upscale_spin->setRange(1, MAX_SOURCE_UPSCALE);
    m_upscale_spin->setSuffix(FrameDumpManager::tr("x"));
    m_upscale_spin->setToolTip(FrameDumpManager::tr(
        "Nearest-neighbor pre-upscale for GBA and other window sources before compositing."));
    settings_layout->addRow(FrameDumpManager::tr("Upscale"), m_upscale_spin);
    auto* scale_note = new QLabel(
        FrameDumpManager::tr("The game source uses Dolphin's frame-dump resolution."), settings_tab);
    scale_note->setWordWrap(true);
    settings_layout->addRow(scale_note);
    settings_layout->addItem(
        new QSpacerItem(1, 1, QSizePolicy::Minimum, QSizePolicy::Expanding));
    tabs->addTab(settings_tab, FrameDumpManager::tr("Settings"));
    source_layout->addWidget(tabs);

    m_scene = new QGraphicsScene(m_owner);
    m_view = new CompositionView(m_scene, splitter);
    splitter->addWidget(source_panel);
    splitter->addWidget(m_view);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({230, 850});
    root->addWidget(splitter, 1);

    m_status_label = new QLabel(FrameDumpManager::tr("No windows detected"), m_owner);
    root->addWidget(m_status_label);

    QObject::connect(m_resolution_combo, &QComboBox::currentIndexChanged, m_owner,
                     [this] { UpdateOutputSize(true); });
    QObject::connect(m_aspect_4_3, &QToolButton::toggled, m_owner,
                     [this](bool checked) {
                       if (checked)
                         UpdateOutputSize(true);
                     });
    QObject::connect(m_aspect_16_9, &QToolButton::toggled, m_owner,
                     [this](bool checked) {
                       if (checked)
                         UpdateOutputSize(true);
                     });
    QObject::connect(m_lock_aspect_ratio, &QCheckBox::toggled, m_owner, [this](bool checked) {
      if (checked)
      {
        if (!LockAspectRatioToSelectedSource())
        {
          const QSignalBlocker blocker(m_lock_aspect_ratio);
          m_lock_aspect_ratio->setChecked(false);
          m_locked_aspect_ratio.reset();
          return;
        }
      }
      else
      {
        m_locked_aspect_ratio.reset();
        UpdateOutputSize(true);
      }
      SaveSettings();
    });
    QObject::connect(reset_button, &QPushButton::clicked, m_owner, [this] { ResetLayout(); });
    QObject::connect(m_snap_to_edges, &QCheckBox::toggled, m_owner,
                     [this](bool enabled) {
                       SetSnappingEnabled(enabled);
                       SaveSettings();
                     });
    QObject::connect(m_dump_button, &QPushButton::clicked, m_owner, [this] {
      if (m_dumping.load())
        StopDump();
      else
        StartDump();
    });
    QObject::connect(m_source_list, &QListWidget::itemChanged, m_owner,
                     [this](QListWidgetItem* item) {
                       const QString key = item->data(Qt::UserRole).toString();
                       auto it = m_sources.find(key);
                       if (it == m_sources.end())
                         return;
                       const bool visible = item->checkState() == Qt::Checked;
                       it->second.composition_item->setVisible(visible);
                       SaveSourceState(key);
                       UpdateGBAFrameConsumer();
                       UpdateStatus();
                     });
    QObject::connect(m_settings_source_combo, &QComboBox::currentIndexChanged, m_owner,
                     [this] { UpdateSettingsForSelectedSource(); });
    QObject::connect(m_upscale_spin, &QSpinBox::valueChanged, m_owner, [this](int value) {
      const QString key = SelectedSettingsKey();
      auto it = m_sources.find(key);
      if (it == m_sources.end() || IsGameWidget(it->second.widget))
        return;
      it->second.upscale_factor = value;
      SaveSourceState(key);
      RefreshPreviews();
    });
    QObject::connect(m_show_preview, &QCheckBox::toggled, m_owner,
                     [this](bool visible) { SetPreviewVisible(visible); });
    QObject::connect(m_source_list, &QListWidget::itemClicked, m_owner,
                     [this](QListWidgetItem* item) {
                       const QString key = item->data(Qt::UserRole).toString();
                       auto it = m_sources.find(key);
                       if (it == m_sources.end())
                         return;
                       m_scene->clearSelection();
                       it->second.composition_item->setSelected(true);
                       SetSettingsSourceKey(key);
                       m_view->ensureVisible(it->second.composition_item);
                     });
    m_source_list->SetOrderChangedCallback([this] { ApplyLayerOrderFromList(); });
    QObject::connect(m_scene, &QGraphicsScene::selectionChanged, m_owner, [this] {
      const auto selected = m_scene->selectedItems();
      if (selected.empty())
        return;
      for (const auto& [key, source] : m_sources)
      {
        if (source.composition_item == selected.front())
        {
          const QSignalBlocker blocker(m_source_list);
          m_source_list->setCurrentItem(source.list_item);
          SetSettingsSourceKey(key);
          break;
        }
      }
    });
  }

  QSize SelectedOutputSize() const
  {
    const int height = m_resolution_combo->currentData().toInt();
    qreal aspect = m_aspect_16_9->isChecked() ? 16.0 / 9.0 : 4.0 / 3.0;
    if (m_lock_aspect_ratio && m_lock_aspect_ratio->isChecked() && m_locked_aspect_ratio &&
        *m_locked_aspect_ratio > 0.0)
    {
      aspect = *m_locked_aspect_ratio;
    }

    int width = qRound(height * aspect);
    const int remainder = width % 4;
    if (remainder != 0)
      width += 4 - remainder;
    return QSize(std::max(4, width), height);
  }

  std::optional<qreal> SelectedSourceAspectRatio() const
  {
    QString key = SelectedSettingsKey();
    if (key.isEmpty() && m_source_list && m_source_list->currentItem())
      key = m_source_list->currentItem()->data(Qt::UserRole).toString();
    const auto it = m_sources.find(key);
    if (it == m_sources.end())
      return std::nullopt;

    if (it->second.composition_item && !it->second.composition_item->GetPixmap().isNull())
    {
      const QSize size = it->second.composition_item->GetPixmap().size();
      if (size.width() > 0 && size.height() > 0)
        return static_cast<qreal>(size.width()) / size.height();
    }

    if (it->second.widget && it->second.widget->width() > 0 && it->second.widget->height() > 0)
      return static_cast<qreal>(it->second.widget->width()) / it->second.widget->height();

    if (it->second.composition_item && it->second.composition_item->LayoutRect().height() > 0.0)
      return it->second.composition_item->LayoutRect().width() /
             it->second.composition_item->LayoutRect().height();

    return std::nullopt;
  }

  bool LockAspectRatioToSelectedSource()
  {
    const std::optional<qreal> aspect = SelectedSourceAspectRatio();
    if (!aspect || !std::isfinite(*aspect) || *aspect <= 0.0)
    {
      QMessageBox::information(
          m_owner, FrameDumpManager::tr("Frame Dump Manager"),
          FrameDumpManager::tr("Select a visible source window before locking the aspect ratio."));
      return false;
    }

    m_locked_aspect_ratio = *aspect;
    UpdateOutputSize(true);
    return true;
  }

  void UpdateOutputSize(bool scale_layout)
  {
    const QSize new_size = SelectedOutputSize();
    if (!new_size.isValid())
      return;

    const QSize old_size = m_output_size;
    m_output_size = new_size;
    m_scene->setSceneRect(QRectF(QPointF{}, QSizeF(new_size)));
    m_output_label->setText(QStringLiteral("%1 x %2").arg(new_size.width()).arg(new_size.height()));

    if (scale_layout && old_size.isValid() && old_size != new_size)
    {
      const qreal scale_x = static_cast<qreal>(new_size.width()) / old_size.width();
      const qreal scale_y = static_cast<qreal>(new_size.height()) / old_size.height();
      for (auto& [key, source] : m_sources)
      {
        const QRectF old_rect = source.composition_item->LayoutRect();
        source.composition_item->ApplyLayout(
            QRectF(old_rect.x() * scale_x, old_rect.y() * scale_y,
                   old_rect.width() * scale_y, old_rect.height() * scale_y));
        SaveSourceState(key);
      }
    }
    m_view->FitCanvas();
    UpdateStatus();
  }

  void LoadSettings()
  {
    QSettings settings;
    settings.beginGroup(QStringLiteral("FrameDumpManager"));
    const int height = settings.value(QStringLiteral("height"), 1080).toInt();
    const int resolution_index = std::max(0, m_resolution_combo->findData(height));
    m_resolution_combo->setCurrentIndex(resolution_index);
    const bool widescreen = settings.value(QStringLiteral("widescreen"), true).toBool();
    m_aspect_16_9->setChecked(widescreen);
    m_aspect_4_3->setChecked(!widescreen);
    m_locked_aspect_ratio = settings.value(QStringLiteral("lockedAspectRatio"), 0.0).toDouble();
    if (m_locked_aspect_ratio && *m_locked_aspect_ratio <= 0.0)
      m_locked_aspect_ratio.reset();
    {
      const QSignalBlocker blocker(m_lock_aspect_ratio);
      m_lock_aspect_ratio->setChecked(
          settings.value(QStringLiteral("lockAspectRatio"), false).toBool() &&
          m_locked_aspect_ratio.has_value());
    }
    m_dump_audio->setChecked(settings.value(QStringLiteral("dumpAudio"), false).toBool());
    m_dump_selected_windows->setChecked(
        settings
            .value(QStringLiteral("dumpWindowsSeparately"),
                   settings.value(QStringLiteral("dumpSelectedWindows"), false))
            .toBool());
    m_show_preview->setChecked(settings.value(QStringLiteral("showPreview"), true).toBool());
    m_preview_enabled.store(m_show_preview->isChecked());
    m_view->SetPreviewEnabled(m_preview_enabled.load());
    {
      const QSignalBlocker blocker(m_snap_to_edges);
      m_snap_to_edges->setChecked(settings.value(QStringLiteral("snap"), true).toBool());
    }
    SetSnappingEnabled(m_snap_to_edges->isChecked());
    if (settings.contains(QStringLiteral("geometry")))
      m_owner->restoreGeometry(settings.value(QStringLiteral("geometry")).toByteArray());

    const QJsonDocument document =
        QJsonDocument::fromJson(settings.value(QStringLiteral("sources")).toByteArray());
    for (const QJsonValue& value : document.array())
    {
      const QJsonObject object = value.toObject();
      const QString key = object.value(QStringLiteral("key")).toString();
      if (key.isEmpty())
        continue;
      m_saved_sources[key] =
          {QRectF(object.value(QStringLiteral("x")).toDouble(),
                  object.value(QStringLiteral("y")).toDouble(),
                  object.value(QStringLiteral("width")).toDouble(),
                  object.value(QStringLiteral("height")).toDouble()),
           object.value(QStringLiteral("visible")).toBool(true),
           object.value(QStringLiteral("z")).toDouble(),
           object.value(QStringLiteral("upscale")).toInt(0)};
    }
    settings.endGroup();
    UpdateOutputSize(false);
  }

  void SaveSettings()
  {
    for (const auto& [key, source] : m_sources)
      SaveSourceState(key);

    QJsonArray array;
    for (const auto& [key, source] : m_saved_sources)
    {
      QJsonObject object;
      object.insert(QStringLiteral("key"), key);
      object.insert(QStringLiteral("x"), source.rect.x());
      object.insert(QStringLiteral("y"), source.rect.y());
      object.insert(QStringLiteral("width"), source.rect.width());
      object.insert(QStringLiteral("height"), source.rect.height());
      object.insert(QStringLiteral("visible"), source.visible);
      object.insert(QStringLiteral("z"), source.z);
      object.insert(QStringLiteral("upscale"), source.upscale_factor);
      array.push_back(object);
    }

    QSettings settings;
    settings.beginGroup(QStringLiteral("FrameDumpManager"));
    settings.setValue(QStringLiteral("height"), m_resolution_combo->currentData().toInt());
    settings.setValue(QStringLiteral("widescreen"), m_aspect_16_9->isChecked());
    settings.setValue(QStringLiteral("lockAspectRatio"), m_lock_aspect_ratio->isChecked());
    settings.setValue(QStringLiteral("lockedAspectRatio"), m_locked_aspect_ratio.value_or(0.0));
    settings.setValue(QStringLiteral("dumpAudio"), m_dump_audio->isChecked());
    settings.setValue(QStringLiteral("dumpWindowsSeparately"),
                      m_dump_selected_windows->isChecked());
    settings.setValue(QStringLiteral("showPreview"), m_show_preview->isChecked());
    settings.setValue(QStringLiteral("snap"), m_snap_to_edges->isChecked());
    settings.setValue(QStringLiteral("geometry"), m_owner->saveGeometry());
    settings.setValue(QStringLiteral("sources"), QJsonDocument(array).toJson(QJsonDocument::Compact));
    settings.endGroup();
  }

  QString SelectedSettingsKey() const
  {
    if (!m_settings_source_combo)
      return {};
    return m_settings_source_combo->currentData().toString();
  }

  void SetSettingsSourceKey(const QString& key)
  {
    if (!m_settings_source_combo)
      return;
    const int index = m_settings_source_combo->findData(key);
    if (index < 0 || index == m_settings_source_combo->currentIndex())
    {
      if (index >= 0)
        UpdateSettingsForSelectedSource();
      return;
    }
    m_settings_source_combo->setCurrentIndex(index);
  }

  void RebuildSettingsSourceCombo()
  {
    if (!m_settings_source_combo)
      return;

    const QString previous_key = SelectedSettingsKey();
    const QSignalBlocker blocker(m_settings_source_combo);
    m_settings_source_combo->clear();
    for (const auto& [key, source] : m_sources)
      m_settings_source_combo->addItem(source.label, key);

    int index = m_settings_source_combo->findData(previous_key);
    if (index < 0 && m_source_list && m_source_list->currentItem())
      index = m_settings_source_combo->findData(m_source_list->currentItem()->data(Qt::UserRole));
    if (index < 0 && m_settings_source_combo->count() > 0)
      index = 0;
    if (index >= 0)
      m_settings_source_combo->setCurrentIndex(index);
    UpdateSettingsForSelectedSource();
  }

  void ApplyLayerOrderFromList()
  {
    if (!m_source_list)
      return;

    const int count = m_source_list->count();
    for (int row = 0; row < count; ++row)
    {
      QListWidgetItem* item = m_source_list->item(row);
      if (!item)
        continue;
      const QString key = item->data(Qt::UserRole).toString();
      auto it = m_sources.find(key);
      if (it == m_sources.end() || !it->second.composition_item)
        continue;

      it->second.composition_item->setZValue(count - row);
      SaveSourceState(key);
    }
  }

  void RebuildSourceListLayerOrder()
  {
    if (!m_source_list)
      return;

    std::vector<QListWidgetItem*> items;
    items.reserve(m_source_list->count());
    const QSignalBlocker blocker(m_source_list);
    while (m_source_list->count() > 0)
      items.push_back(m_source_list->takeItem(0));

    std::ranges::sort(items, [this](const QListWidgetItem* a, const QListWidgetItem* b) {
      const QString key_a = a->data(Qt::UserRole).toString();
      const QString key_b = b->data(Qt::UserRole).toString();
      const auto it_a = m_sources.find(key_a);
      const auto it_b = m_sources.find(key_b);
      const qreal z_a = it_a != m_sources.end() && it_a->second.composition_item ?
                            it_a->second.composition_item->zValue() :
                            0.0;
      const qreal z_b = it_b != m_sources.end() && it_b->second.composition_item ?
                            it_b->second.composition_item->zValue() :
                            0.0;
      return z_a > z_b;
    });

    for (QListWidgetItem* item : items)
      m_source_list->addItem(item);
    ApplyLayerOrderFromList();
  }

  void UpdateSettingsForSelectedSource()
  {
    if (!m_settings_source_combo || !m_upscale_spin)
      return;

    const QString key = SelectedSettingsKey();
    const auto it = m_sources.find(key);
    const bool has_source = it != m_sources.end();
    const bool is_game_source = has_source && IsGameWidget(it->second.widget);
    const QSignalBlocker blocker(m_upscale_spin);
    m_upscale_spin->setEnabled(has_source && !is_game_source);
    m_upscale_spin->setValue(has_source ? it->second.upscale_factor : DEFAULT_WINDOW_UPSCALE);
  }

  void SetPreviewVisible(bool visible)
  {
    m_preview_enabled.store(visible);
    m_view->SetPreviewEnabled(visible);
    for (auto& [key, source] : m_sources)
    {
      if (source.composition_item)
        source.composition_item->SetPreviewEnabled(visible);
    }
    if (visible)
    {
      RegisterFrameConsumer();
      RefreshPreviews();
      m_view->FitCanvas();
    }
    else if (!m_dumping.load())
    {
      UnregisterFrameConsumer();
    }
    SaveSettings();
  }

  void SetSnappingEnabled(bool enabled)
  {
    for (auto& [key, source] : m_sources)
    {
      if (source.composition_item)
        source.composition_item->SetSnappingEnabled(enabled);
    }
  }

  QString SourceKey(QWidget* widget, const QString& title, int duplicate) const
  {
    if (qobject_cast<RenderWidget*>(widget))
      return QStringLiteral("RenderWidget");
    QString key = QStringLiteral("%1|%2")
                      .arg(QString::fromLatin1(widget->metaObject()->className()), title);
    if (duplicate > 0)
      key += QStringLiteral("|%1").arg(duplicate);
    return key;
  }

  void RefreshSources()
  {
    struct Candidate
    {
      QWidget* widget;
      QString title;
    };
    std::vector<Candidate> candidates;

    for (QWidget* widget : QApplication::allWidgets())
    {
      if (qobject_cast<RenderWidget*>(widget) && widget->isVisible())
      {
        candidates.push_back({widget, FrameDumpManager::tr("Game")});
        break;
      }
    }

    for (QWidget* widget : QApplication::topLevelWidgets())
    {
      if (!widget || !widget->isVisible() || widget == m_owner || widget == m_owner->parentWidget() ||
          qobject_cast<QMenu*>(widget) || qobject_cast<RenderWidget*>(widget))
      {
        continue;
      }
      const Qt::WindowType type = static_cast<Qt::WindowType>(widget->windowType());
      if (type == Qt::Popup || type == Qt::ToolTip || type == Qt::SplashScreen)
        continue;
      QString title = widget->windowTitle().trimmed();
      if (title.isEmpty())
        title = QString::fromLatin1(widget->metaObject()->className());
      candidates.push_back({widget, title});
    }

    std::map<QString, int> duplicate_counts;
    std::map<QString, QPointer<QWidget>> discovered;
    std::map<QString, QString> labels;
    for (const Candidate& candidate : candidates)
    {
      const QString base = SourceKey(candidate.widget, candidate.title, 0);
      const int duplicate = duplicate_counts[base]++;
      const QString key = SourceKey(candidate.widget, candidate.title, duplicate);
      discovered[key] = candidate.widget;
      labels[key] = candidate.title;
    }

    for (auto it = m_sources.begin(); it != m_sources.end();)
    {
      if (discovered.contains(it->first))
      {
        it->second.widget = discovered[it->first];
        ++it;
        continue;
      }
      SaveSourceState(it->first);
      delete it->second.composition_item;
      delete m_source_list->takeItem(m_source_list->row(it->second.list_item));
      it = m_sources.erase(it);
    }

    for (const auto& [key, widget] : discovered)
    {
      if (m_sources.contains(key))
        continue;

      SourceEntry source;
      source.widget = widget;
      source.label = labels[key];
      source.upscale_factor = DefaultUpscaleForWidget(widget);
      source.list_item = new QListWidgetItem(labels[key], m_source_list);
      source.list_item->setFlags(source.list_item->flags() | Qt::ItemIsUserCheckable);
      source.list_item->setData(Qt::UserRole, key);

      source.composition_item = new CompositionItem(labels[key], [this, key] {
        if (m_sources.contains(key))
          SaveSourceState(key);
      });
      source.composition_item->SetSnappingEnabled(m_snap_to_edges->isChecked());
      source.composition_item->SetPreviewEnabled(m_preview_enabled.load());
      m_scene->addItem(source.composition_item);

      if (const auto saved = m_saved_sources.find(key); saved != m_saved_sources.end())
      {
        if (saved->second.upscale_factor > 0)
          source.upscale_factor = saved->second.upscale_factor;
        if (IsGameWidget(widget))
          source.upscale_factor = GAME_SOURCE_UPSCALE;
        source.composition_item->ApplyLayout(saved->second.rect);
        source.composition_item->setVisible(saved->second.visible);
        source.composition_item->setZValue(saved->second.z);
        source.list_item->setCheckState(saved->second.visible ? Qt::Checked : Qt::Unchecked);
        // Normalize layouts saved by older builds once the sources real aspect ratio is known.
        source.automatically_sized = false;
      }
      else
      {
        const int index = static_cast<int>(m_sources.size());
        const qreal width = m_output_size.width() * 0.42;
        const qreal height = width * 9.0 / 16.0;
        source.composition_item->ApplyLayout(
            QRectF(32.0 + (index % 2) * (width + 32.0),
                   32.0 + (index / 2) * (height + 32.0), width, height));
        source.composition_item->setZValue(-index);
        source.list_item->setCheckState(Qt::Checked);
      }
      m_sources.emplace(key, source);
    }
    RebuildSourceListLayerOrder();
    RebuildSettingsSourceCombo();
    UpdateGBAFrameConsumer();
    UpdateStatus();
  }

  void RefreshPreviews()
  {
    if (!m_preview_enabled.load())
      return;

    for (auto& [key, source] : m_sources)
    {
      if (!source.widget || !source.composition_item->isVisible())
        continue;
      QImage image =
          CaptureWidgetOffscreen(source.widget, m_latest_game_frame, source.upscale_factor);
      if (image.isNull())
        continue;
      source.composition_item->SetPixmap(QPixmap::fromImage(std::move(image)));
      if (!source.automatically_sized)
      {
        source.automatically_sized = true;
        const QRectF current = source.composition_item->LayoutRect();
        const QPixmap& current_pixmap = source.composition_item->GetPixmap();
        const qreal aspect = static_cast<qreal>(current_pixmap.width()) / current_pixmap.height();
        source.composition_item->ApplyLayout(
            QRectF(current.topLeft(), QSizeF(current.width(), current.width() / aspect)));
        SaveSourceState(key);
      }
    }
  }

  void ResetLayout()
  {
    std::vector<std::pair<QString, SourceEntry*>> visible;
    for (auto& [key, source] : m_sources)
    {
      if (source.composition_item->isVisible())
        visible.emplace_back(key, &source);
    }
    if (visible.empty())
      return;

    const int columns = visible.size() == 1 ? 1 : 2;
    const int rows = (static_cast<int>(visible.size()) + columns - 1) / columns;
    const qreal cell_width = m_output_size.width() / static_cast<qreal>(columns);
    const qreal cell_height = m_output_size.height() / static_cast<qreal>(rows);
    for (int i = 0; i < static_cast<int>(visible.size()); ++i)
    {
      SourceEntry& source = *visible[i].second;
      const QPixmap& pixmap = source.composition_item->GetPixmap();
      const qreal aspect = !pixmap.isNull() && pixmap.height() > 0 ?
                               static_cast<qreal>(pixmap.width()) / pixmap.height() :
                               16.0 / 9.0;
      qreal width = cell_width - 32.0;
      qreal height = width / aspect;
      if (height > cell_height - 32.0)
      {
        height = cell_height - 32.0;
        width = height * aspect;
      }
      const int column = i % columns;
      const int row = i / columns;
      source.composition_item->ApplyLayout(
          QRectF(column * cell_width + (cell_width - width) / 2.0,
                 row * cell_height + (cell_height - height) / 2.0, width, height));
      source.composition_item->setZValue(static_cast<int>(visible.size()) - i);
      SaveSourceState(visible[i].first);
    }
    RebuildSourceListLayerOrder();
    m_view->FitCanvas();
  }

  void SaveSourceState(const QString& key)
  {
    const auto it = m_sources.find(key);
    if (it == m_sources.end() || !it->second.composition_item)
      return;
    m_saved_sources[key] = {it->second.composition_item->LayoutRect(),
                            it->second.composition_item->isVisible(),
                            it->second.composition_item->zValue(),
                            it->second.upscale_factor};
  }

  void UpdateStatus()
  {
    int visible = 0;
    for (const auto& [key, source] : m_sources)
      visible += source.composition_item->isVisible() ? 1 : 0;

    if (m_dumping.load())
    {
      if (IsSelectedWindowDumpMode())
      {
#if defined(HAVE_FFMPEG)
        int active_dumps = 0;
        int failed_dumps = 0;
        for (const auto& [key, source_dump] : m_source_dumps)
        {
          if (source_dump.failed)
            ++failed_dumps;
          else if (source_dump.dump && source_dump.dump->IsStarted())
            ++active_dumps;
        }

        m_status_label->setText(
            FrameDumpManager::tr("Dumping windows separately | %1 sources | %2 active | %3 failed")
                .arg(visible)
                .arg(active_dumps)
                .arg(failed_dumps));
#endif
      }
      else
      {
        m_status_label->setText(
            FrameDumpManager::tr("Dumping %1 x %2 | %3 sources | %4 frames")
                .arg(m_output_size.width())
                .arg(m_output_size.height())
                .arg(visible)
                .arg(m_frame_count));
      }
    }
    else
    {
      m_status_label->setText(
          FrameDumpManager::tr("%1 x %2 | %3 of %4 sources visible")
              .arg(m_output_size.width())
              .arg(m_output_size.height())
              .arg(visible)
              .arg(m_sources.size()));
    }
  }

  void StartDump()
  {
#if defined(HAVE_FFMPEG)
    if (Core::GetState(Core::System::GetInstance()) == Core::State::Uninitialized)
    {
      QMessageBox::information(m_owner, FrameDumpManager::tr("Frame Dump Manager"),
                               FrameDumpManager::tr("Start a game before beginning a frame dump."));
      return;
    }
    if (std::ranges::none_of(m_sources, [](const auto& pair) {
          return pair.second.composition_item->isVisible();
        }))
    {
      QMessageBox::information(m_owner, FrameDumpManager::tr("Frame Dump Manager"),
                               FrameDumpManager::tr("Enable at least one source window first."));
      return;
    }

    RefreshSources();
    RefreshPreviews();
    RegisterFrameConsumer();
    if (m_frame_callback_id == 0)
    {
      QMessageBox::warning(m_owner, FrameDumpManager::tr("Frame Dump Manager"),
                           FrameDumpManager::tr("The game frame source is not available."));
      return;
    }
    constexpr u64 start_ticks = 0;
    m_dump_start_ticks = start_ticks;
    m_frame_count = 0;
    StopSourceDumps();

    const bool dump_selected_windows = m_dump_selected_windows->isChecked();
    if (!dump_selected_windows &&
        !m_dump.Start(m_output_size.width(), m_output_size.height(), start_ticks, "Composite"))
    {
      if (!m_preview_enabled.load())
        UnregisterFrameConsumer();
      QMessageBox::warning(m_owner, FrameDumpManager::tr("Frame Dump Manager"),
                           FrameDumpManager::tr("The composite frame dump could not be started."));
      return;
    }
    m_dumping.store(true);
    UpdateGBAFrameConsumer();
    StartAudioDump();
    m_resolution_combo->setEnabled(false);
    m_aspect_4_3->setEnabled(false);
    m_aspect_16_9->setEnabled(false);
    m_lock_aspect_ratio->setEnabled(false);
    m_dump_audio->setEnabled(false);
    m_dump_selected_windows->setEnabled(false);
    m_dump_button->setText(FrameDumpManager::tr("Stop Dump"));
    m_dump_button->setIcon(m_owner->style()->standardIcon(QStyle::SP_MediaStop));
    UpdateStatus();
#endif
  }

  void StopDump()
  {
#if defined(HAVE_FFMPEG)
    m_dumping.store(false);
    UpdateGBAFrameConsumer();
    if (m_dump.IsStarted())
      m_dump.Stop();
    StopSourceDumps();
#endif
    StopAudioDump();
    if (!m_dump_button)
      return;
    m_resolution_combo->setEnabled(true);
    m_aspect_4_3->setEnabled(true);
    m_aspect_16_9->setEnabled(true);
    m_lock_aspect_ratio->setEnabled(true);
    m_dump_audio->setEnabled(true);
    m_dump_selected_windows->setEnabled(true);
    if (!m_preview_enabled.load())
      UnregisterFrameConsumer();
    m_dump_button->setText(FrameDumpManager::tr("Start Dump"));
    m_dump_button->setIcon(m_owner->style()->standardIcon(QStyle::SP_MediaPlay));
    UpdateStatus();
  }

  bool IsSelectedWindowDumpMode() const
  {
#if defined(HAVE_FFMPEG)
    return m_dump_selected_windows && m_dump_selected_windows->isChecked();
#else
    return false;
#endif
  }

  void StopSourceDumps()
  {
#if defined(HAVE_FFMPEG)
    for (auto& [key, source_dump] : m_source_dumps)
    {
      if (source_dump.dump && source_dump.dump->IsStarted())
        source_dump.dump->Stop();
    }
    m_source_dumps.clear();
#endif
  }

  u64 SyntheticTicksForFrame(u64 start_ticks, u64 frame_count, u64 fallback_ticks) const
  {
    Core::System& system = Core::System::GetInstance();
    const u64 ticks_per_second = system.GetSystemTimers().GetTicksPerSecond();
    const u64 refresh_num = system.GetVideoInterface().GetTargetRefreshRateNumerator();
    const u64 refresh_den = system.GetVideoInterface().GetTargetRefreshRateDenominator();
    if (ticks_per_second == 0 || refresh_num == 0 || refresh_den == 0)
      return fallback_ticks;

    return start_ticks +
           (frame_count * ticks_per_second * refresh_den + refresh_num / 2) / refresh_num;
  }

  void CaptureSelectedSourceFrames(const QImage& game_frame, const FrameState& source_state,
                                   std::optional<int> gba_device)
  {
#if defined(HAVE_FFMPEG)
    if (!m_dumping.load() || !IsSelectedWindowDumpMode())
      return;

    for (const auto& [key, source] : m_sources)
    {
      if (!source.widget || !source.composition_item->isVisible())
        continue;

#if defined(HAS_LIBMGBA)
      const auto* gba = qobject_cast<GBAWidget*>(source.widget);
      if (gba_device)
      {
        if (!gba || gba->GetDeviceNumber() != *gba_device)
          continue;
      }
      else if (gba)
      {
        continue;
      }
#else
      if (gba_device)
        continue;
#endif

      const QImage source_image =
          CaptureWidgetOffscreen(source.widget, game_frame, source.upscale_factor);
      if (!source_image.isNull())
      {
        const auto* input_display = qobject_cast<InputDisplayWidget*>(source.widget);
        const bool force_black_background =
            input_display && input_display->IsBackgroundRemoved();
        DumpSourceFrame(key, source.label, source_image, source_state, force_black_background);
      }
    }
#endif
  }

  void DumpSourceFrame(const QString& key, const QString& label, const QImage& image,
                       const FrameState& source_state, bool force_black_background)
  {
#if defined(HAVE_FFMPEG)
    if (image.isNull())
      return;

    QImage rgba = force_black_background ? FlattenOnBlack(image) :
                                           image.convertToFormat(QImage::Format_RGBA8888);
    if (rgba.isNull())
      return;

    SourceDump& source_dump = m_source_dumps[key];
    if (source_dump.failed)
      return;

    if (!source_dump.dump)
      source_dump.dump = std::make_unique<FFMpegFrameDump>();

    if (!source_dump.dump->IsStarted())
    {
      source_dump.start_ticks = 0;
      source_dump.frame_count = 0;
      const std::string prefix =
          std::string("FrameDumpManager/") + SanitizeDumpName(label + QStringLiteral("_") + key);
      if (!source_dump.dump->Start(rgba.width(), rgba.height(), source_dump.start_ticks, prefix))
      {
        source_dump.failed = true;
        OSD::AddMessage("Frame Dump Manager: failed to start separate dump for " +
                        label.toStdString(),
                        OSD::Duration::NORMAL, OSD::Color::RED);
        UpdateStatus();
        return;
      }
    }

    const u64 ticks =
        SyntheticTicksForFrame(source_dump.start_ticks, source_dump.frame_count, source_state.ticks);

    FrameData frame;
    frame.data = rgba.constBits();
    frame.width = rgba.width();
    frame.height = rgba.height();
    frame.stride = static_cast<int>(rgba.bytesPerLine());
    frame.state = source_dump.dump->FetchState(ticks, static_cast<int>(source_dump.frame_count));
    source_dump.dump->AddFrame(frame);
    if (!source_dump.dump->IsStarted())
    {
      source_dump.failed = true;
      OSD::AddMessage("Frame Dump Manager: separate dump stopped for " + label.toStdString(),
                      OSD::Duration::VERY_LONG, OSD::Color::RED);
      UpdateStatus();
      return;
    }

    ++source_dump.frame_count;
    if (source_dump.frame_count % 30 == 0)
      UpdateStatus();
#endif
  }

  std::optional<int> VisibleGBAPacerDevice() const
  {
#if defined(HAS_LIBMGBA)
    std::optional<int> device;
    for (const auto& [key, source] : m_sources)
    {
      if (!source.widget || !source.composition_item->isVisible())
        continue;
      const auto* gba = qobject_cast<GBAWidget*>(source.widget);
      if (!gba)
        continue;
      const int source_device = gba->GetDeviceNumber();
      if (!device || source_device < *device)
        device = source_device;
    }
    return device;
#else
    return std::nullopt;
#endif
  }

  u64 SyntheticDumpTicksForNextFrame(u64 fallback_ticks) const
  {
    return SyntheticTicksForFrame(m_dump_start_ticks, m_frame_count, fallback_ticks);
  }

  void UpdateGBAFrameConsumer()
  {
#if defined(HAS_LIBMGBA)
    const std::optional<int> pacer_device = m_dumping.load() ? VisibleGBAPacerDevice() :
                                                              std::nullopt;
    m_gba_pacer_device.store(pacer_device.value_or(-1));
    m_gba_frame_pacer_active.store(pacer_device.has_value());

    if (pacer_device && m_gba_frame_callback_id == 0)
    {
      QPointer<FrameDumpManager> owner = m_owner;
      m_gba_frame_callback_id =
          HW::GBA::Core::AddVideoFrameCallback([owner](int device_number, u64 ticks) {
            if (!owner || !owner->m_impl)
              return;

            const auto invoke = [owner, device_number, ticks] {
              if (!owner || !owner->m_impl)
                return;
              owner->m_impl->CaptureGBAClockedFrame(device_number, ticks);
            };

            if (QThread::currentThread() == owner->thread())
              invoke();
            else
              QMetaObject::invokeMethod(owner, invoke, Qt::BlockingQueuedConnection);
          });
    }
    else if (!pacer_device && m_gba_frame_callback_id != 0)
    {
      HW::GBA::Core::RemoveVideoFrameCallback(m_gba_frame_callback_id);
      m_gba_frame_callback_id = 0;
    }
#endif
  }

  void CaptureGBAClockedFrame(int device_number, u64 ticks)
  {
#if defined(HAVE_FFMPEG)
    if (!m_dumping.load())
      return;

    FrameState state;
    state.ticks = SyntheticDumpTicksForNextFrame(ticks);
    if (IsSelectedWindowDumpMode())
      CaptureSelectedSourceFrames(m_latest_game_frame, state, device_number);
    else if (m_dump.IsStarted() && m_gba_frame_pacer_active.load() &&
             m_gba_pacer_device.load() == device_number)
    {
      CaptureDumpFrame(m_latest_game_frame, state);
    }
#endif
  }

  void CaptureDumpFrame(const QImage& game_frame, const FrameState& source_state)
  {
#if defined(HAVE_FFMPEG)
    if (!m_dumping.load() || !m_dump.IsStarted())
      return;

    if (m_composite_frame.size() != m_output_size ||
        m_composite_frame.format() != QImage::Format_RGBA8888)
    {
      m_composite_frame = QImage(m_output_size, QImage::Format_RGBA8888);
    }
    m_composite_frame.fill(Qt::black);

    QPainter painter(&m_composite_frame);
    painter.setRenderHint(QPainter::Antialiasing, false);
    std::vector<SourceEntry*> ordered_sources;
    ordered_sources.reserve(m_sources.size());
    for (auto& [key, source] : m_sources)
    {
      if (source.widget && source.composition_item->isVisible())
        ordered_sources.push_back(&source);
    }
    std::ranges::sort(ordered_sources, {}, [](const SourceEntry* source) {
      return source->composition_item->zValue();
    });
    for (const SourceEntry* source : ordered_sources)
    {
      const QImage source_image =
          CaptureWidgetOffscreen(source->widget, game_frame, source->upscale_factor);
      if (!source_image.isNull())
      {
        const QRect target_rect =
            RoundedCanvasRect(source->composition_item->LayoutRect(), m_output_size);
        if (!target_rect.isValid() || target_rect.isEmpty())
          continue;

        painter.setRenderHint(QPainter::SmoothPixmapTransform, IsGameWidget(source->widget));
        painter.drawImage(target_rect, source_image, source_image.rect());
      }
    }
    painter.end();

    FrameData frame;
    frame.data = m_composite_frame.constBits();
    frame.width = m_composite_frame.width();
    frame.height = m_composite_frame.height();
    frame.stride = static_cast<int>(m_composite_frame.bytesPerLine());
    const u64 ticks = SyntheticDumpTicksForNextFrame(source_state.ticks);
    frame.state = m_dump.FetchState(ticks, static_cast<int>(m_frame_count));
    m_dump.AddFrame(frame);
    ++m_frame_count;
    if (m_frame_count % 30 == 0)
      UpdateStatus();
#endif
  }

  void RegisterFrameConsumer()
  {
    if (!g_frame_dumper)
      return;
    if (m_frame_callback_id != 0 && m_registered_frame_dumper == g_frame_dumper.get() &&
        g_frame_dumper->HasFrameDataCallback(m_frame_callback_id))
      return;

    m_frame_callback_id = 0;
    m_registered_frame_dumper = g_frame_dumper.get();

    QPointer<FrameDumpManager> owner = m_owner;
    m_frame_callback_id = g_frame_dumper->AddFrameDataCallback([owner](const FrameData& frame) {
      if (!owner || !owner->m_impl)
        return;

      Impl* impl = owner->m_impl.get();
      const bool dumping = impl->m_dumping.load();
      const bool preview = impl->m_preview_enabled.load();
      if (!dumping && !preview)
        return;
      if (!dumping && impl->m_preview_frame_queued.exchange(true))
        return;

      const QImage view(frame.data, frame.width, frame.height, frame.stride,
                        QImage::Format_RGBA8888);
      if (dumping)
      {
        const FrameState state = frame.state;
        QMetaObject::invokeMethod(
            owner,
            [owner, view, state, preview] {
              if (!owner || !owner->m_impl)
                return;
              Impl* current = owner->m_impl.get();
              current->m_preview_frame_queued.store(false);
              const bool gba_paced = current->m_gba_frame_pacer_active.load();
              const bool selected_window_dump = current->IsSelectedWindowDumpMode();
              if (preview || gba_paced || selected_window_dump)
                current->m_latest_game_frame = view.copy();
              if (selected_window_dump)
                current->CaptureSelectedSourceFrames(view, state, std::nullopt);
              else if (!gba_paced && current->m_dumping.load())
                current->CaptureDumpFrame(view, state);
            },
            Qt::BlockingQueuedConnection);
        return;
      }

      QImage copy = view.copy();
      QMetaObject::invokeMethod(
          owner,
          [owner, copy = std::move(copy), preview]() mutable {
            if (!owner || !owner->m_impl)
              return;
            Impl* current = owner->m_impl.get();
            current->m_preview_frame_queued.store(false);
            if (preview)
              current->m_latest_game_frame = copy;
          },
          Qt::QueuedConnection);
    });
  }

  void UnregisterFrameConsumer()
  {
    if (m_frame_callback_id == 0)
      return;
    if (g_frame_dumper && m_registered_frame_dumper == g_frame_dumper.get())
      g_frame_dumper->RemoveFrameDataCallback(m_frame_callback_id);
    m_frame_callback_id = 0;
    m_registered_frame_dumper = nullptr;
    m_preview_frame_queued.store(false);
  }

  void StartAudioDump()
  {
    if (!m_dump_audio->isChecked() || m_audio_settings_overridden)
      return;

    Core::System& system = Core::System::GetInstance();
    m_previous_main_audio_dump = Config::Get(Config::MAIN_DUMP_AUDIO);
    m_previous_gba_audio_dump = Config::Get(Config::MAIN_GBA_DUMP_AUDIO);
    m_started_main_audio_dump = !system.IsAudioDumpStarted();
    Config::SetBaseOrCurrent(Config::MAIN_DUMP_AUDIO, true);
    Config::SetBaseOrCurrent(Config::MAIN_GBA_DUMP_AUDIO, true);
    if (system.GetSoundStream() && system.GetSoundStream()->GetMixer())
    {
      Mixer* mixer = system.GetSoundStream()->GetMixer();
      mixer->RefreshConfig();
      if (!m_previous_gba_audio_dump)
      {
        for (std::size_t i = 0; i < 4; ++i)
          mixer->StartLogGBAAudio(i);
      }
    }
    if (m_started_main_audio_dump && system.GetSoundStream())
      AudioCommon::StartAudioDump(system);
    m_audio_settings_overridden = true;
  }

  void StopAudioDump()
  {
    if (!m_audio_settings_overridden)
      return;

    Core::System& system = Core::System::GetInstance();
    if (m_started_main_audio_dump && system.IsAudioDumpStarted() && system.GetSoundStream())
      AudioCommon::StopAudioDump(system);
    Config::SetBaseOrCurrent(Config::MAIN_DUMP_AUDIO, m_previous_main_audio_dump);
    Config::SetBaseOrCurrent(Config::MAIN_GBA_DUMP_AUDIO, m_previous_gba_audio_dump);
    if (system.GetSoundStream() && system.GetSoundStream()->GetMixer())
    {
      Mixer* mixer = system.GetSoundStream()->GetMixer();
      mixer->RefreshConfig();
      if (!m_previous_gba_audio_dump)
      {
        for (std::size_t i = 0; i < 4; ++i)
          mixer->StopLogGBAAudio(i);
      }
    }
    m_audio_settings_overridden = false;
    m_started_main_audio_dump = false;
  }

  FrameDumpManager* m_owner;
  QComboBox* m_resolution_combo = nullptr;
  QToolButton* m_aspect_4_3 = nullptr;
  QToolButton* m_aspect_16_9 = nullptr;
  QCheckBox* m_lock_aspect_ratio = nullptr;
  QCheckBox* m_dump_audio = nullptr;
  QCheckBox* m_dump_selected_windows = nullptr;
  QCheckBox* m_show_preview = nullptr;
  QCheckBox* m_snap_to_edges = nullptr;
  QLabel* m_output_label = nullptr;
  LayerListWidget* m_source_list = nullptr;
  QComboBox* m_settings_source_combo = nullptr;
  QSpinBox* m_upscale_spin = nullptr;
  QGraphicsScene* m_scene = nullptr;
  CompositionView* m_view = nullptr;
  QPushButton* m_dump_button = nullptr;
  QLabel* m_status_label = nullptr;
  QTimer m_preview_timer;
  QSize m_output_size;
  std::optional<qreal> m_locked_aspect_ratio;
  int m_discovery_counter = 0;
  u64 m_frame_count = 0;
  u64 m_dump_start_ticks = 0;
  std::map<QString, SourceEntry> m_sources;
  std::map<QString, SavedSource> m_saved_sources;
#if defined(HAVE_FFMPEG)
  std::map<QString, SourceDump> m_source_dumps;
#endif
  std::atomic<bool> m_dumping{false};
  std::atomic<bool> m_preview_enabled{true};
  std::atomic<bool> m_preview_frame_queued{false};
  std::atomic<bool> m_gba_frame_pacer_active{false};
  std::atomic<int> m_gba_pacer_device{-1};
  QImage m_latest_game_frame;
  QImage m_composite_frame;
  u64 m_frame_callback_id = 0;
  u64 m_gba_frame_callback_id = 0;
  FrameDumper* m_registered_frame_dumper = nullptr;
  bool m_audio_settings_overridden = false;
  bool m_started_main_audio_dump = false;
  bool m_previous_main_audio_dump = false;
  bool m_previous_gba_audio_dump = false;

#if defined(HAVE_FFMPEG)
  FFMpegFrameDump m_dump;
#endif
};

FrameDumpManager::FrameDumpManager(QWidget* parent)
    : QDialog(parent, Qt::Window), m_impl(std::make_unique<Impl>(this))
{
}

FrameDumpManager::~FrameDumpManager() = default;

void FrameDumpManager::closeEvent(QCloseEvent* event)
{
  m_impl->OnClosed();
  QDialog::closeEvent(event);
}

void FrameDumpManager::showEvent(QShowEvent* event)
{
  QDialog::showEvent(event);
  m_impl->OnShown();
}
