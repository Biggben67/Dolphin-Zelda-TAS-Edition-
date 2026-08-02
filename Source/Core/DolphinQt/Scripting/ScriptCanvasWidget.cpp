// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "DolphinQt/Scripting/ScriptCanvasWidget.h"

#include <QCloseEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

static QColor ArgbToColor(u32 argb)
{
  return QColor((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, (argb >> 24) & 0xFF);
}

static QPointF Pt(const Vec2f& p)
{
  return QPointF(p.x, p.y);
}

static QRgb BlendPremultiplied(QRgb dst, QRgb src)
{
  const int inverse_alpha = 255 - qAlpha(src);
  return qRgba(qRed(src) + (qRed(dst) * inverse_alpha + 127) / 255,
               qGreen(src) + (qGreen(dst) * inverse_alpha + 127) / 255,
               qBlue(src) + (qBlue(dst) * inverse_alpha + 127) / 255,
               qAlpha(src) + (qAlpha(dst) * inverse_alpha + 127) / 255);
}

static void DrawDepthLine(QImage& image, const std::vector<float>& inv_depth, float ax, float ay,
                          float inv_z0, float bx, float by, float inv_z1, QRgb color,
                          float thickness)
{
  const int dx = static_cast<int>(std::round(bx - ax));
  const int dy = static_cast<int>(std::round(by - ay));
  const int steps = std::max(std::abs(dx), std::abs(dy));
  const int radius = std::max(0, static_cast<int>(std::ceil(thickness * 0.5f)) - 1);
  const int width = image.width();
  const int height = image.height();
  auto* pixels = reinterpret_cast<QRgb*>(image.bits());

  for (int index = 0; index <= std::max(1, steps); ++index)
  {
    const float t = steps == 0 ? 0.0f : static_cast<float>(index) / steps;
    const int x = static_cast<int>(std::round(ax + (bx - ax) * t));
    const int y = static_cast<int>(std::round(ay + (by - ay) * t));
    const float z = inv_z0 + (inv_z1 - inv_z0) * t;
    for (int offset_y = -radius; offset_y <= radius; ++offset_y)
    {
      for (int offset_x = -radius; offset_x <= radius; ++offset_x)
      {
        const int pixel_x = x + offset_x;
        const int pixel_y = y + offset_y;
        if (pixel_x < 0 || pixel_x >= width || pixel_y < 0 || pixel_y >= height)
          continue;
        const size_t offset = static_cast<size_t>(pixel_y) * width + pixel_x;
        // Edges belong to their own triangle, so the small epsilon keeps them
        // visible while still rejecting any surface strictly in front of them.
        if (z + 0.00001f >= inv_depth[offset])
          pixels[offset] = BlendPremultiplied(pixels[offset], color);
      }
    }
  }
}

// QPainter has no depth buffer. Script canvases use this small software pass for
// 3D debug geometry, so translucent front faces still correctly hide geometry
// behind them instead of relying on an unreliable whole-triangle paint order.
static void DrawDepthTriangles(QPainter& painter,
                               const std::vector<API::Gui::CanvasPrimitive>& primitives,
                               const QSize& size)
{
  using Type = API::Gui::CanvasPrimitive::Type;
  if (size.width() <= 0 || size.height() <= 0)
    return;

  const size_t depth_primitive_count =
      std::count_if(primitives.begin(), primitives.end(), [](const auto& primitive) {
        return primitive.type == Type::DepthTriangleFilled || primitive.type == Type::DepthTriangleWire;
      });
  // A full-radius TP scene can contain hundreds of thousands of triangles.
  // Rasterizing that many at the window's native resolution monopolizes Qt's
  // main thread, which also dispatches Dolphin's frame-step jobs.  Keep normal
  // views exact and lower only very dense depth passes before scaling them back.
  const int downsample = depth_primitive_count > 80000 ? 4 :
                         depth_primitive_count > 20000 ? 2 : 1;
  const QSize raster_size((size.width() + downsample - 1) / downsample,
                         (size.height() + downsample - 1) / downsample);
  const float raster_scale = 1.0f / static_cast<float>(downsample);
  QImage image(raster_size, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  std::vector<float> inv_depth(static_cast<size_t>(raster_size.width()) * raster_size.height(),
                               -std::numeric_limits<float>::infinity());
  auto* pixels = reinterpret_cast<QRgb*>(image.bits());

  for (const auto& primitive : primitives)
  {
    if (primitive.type != Type::DepthTriangleFilled || primitive.z0 <= 0.0f ||
        primitive.z1 <= 0.0f || primitive.z2 <= 0.0f)
    {
      continue;
    }

    const float ax = primitive.p0.x * raster_scale, ay = primitive.p0.y * raster_scale;
    const float bx = primitive.p1.x * raster_scale, by = primitive.p1.y * raster_scale;
    const float cx = primitive.p2.x * raster_scale, cy = primitive.p2.y * raster_scale;
    const float area = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
    if (std::abs(area) < 0.00001f)
      continue;

    const int min_x = std::max(0, static_cast<int>(std::floor(std::min({ax, bx, cx}))));
    const int max_x = std::min(raster_size.width() - 1,
                               static_cast<int>(std::ceil(std::max({ax, bx, cx}))));
    const int min_y = std::max(0, static_cast<int>(std::floor(std::min({ay, by, cy}))));
    const int max_y = std::min(raster_size.height() - 1,
                               static_cast<int>(std::ceil(std::max({ay, by, cy}))));
    if (min_x > max_x || min_y > max_y)
      continue;

    const float inv_z0 = 1.0f / primitive.z0;
    const float inv_z1 = 1.0f / primitive.z1;
    const float inv_z2 = 1.0f / primitive.z2;
    const QRgb color = qPremultiply(ArgbToColor(primitive.color).rgba());
    for (int y = min_y; y <= max_y; ++y)
    {
      const float py = static_cast<float>(y) + 0.5f;
      for (int x = min_x; x <= max_x; ++x)
      {
        const float px = static_cast<float>(x) + 0.5f;
        const float u = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / area;
        const float v = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / area;
        const float w = 1.0f - u - v;
        if (u < 0.0f || v < 0.0f || w < 0.0f)
          continue;
        const float z = u * inv_z0 + v * inv_z1 + w * inv_z2;
        const size_t offset = static_cast<size_t>(y) * raster_size.width() + x;
        if (z > inv_depth[offset])
        {
          inv_depth[offset] = z;
          // Alpha-zero triangles are depth-only occluders. This lets a wireframe
          // view stay correctly hidden even when filled collision is disabled.
          if (qAlpha(color) != 0)
            pixels[offset] = color;
        }
      }
    }
  }

  for (const auto& primitive : primitives)
  {
    if (primitive.type != Type::DepthTriangleWire || primitive.z0 <= 0.0f ||
        primitive.z1 <= 0.0f || primitive.z2 <= 0.0f)
    {
      continue;
    }
    const QRgb color = qPremultiply(ArgbToColor(primitive.color).rgba());
    if (qAlpha(color) == 0)
      continue;
    DrawDepthLine(image, inv_depth, primitive.p0.x * raster_scale, primitive.p0.y * raster_scale,
                  1.0f / primitive.z0, primitive.p1.x * raster_scale,
                  primitive.p1.y * raster_scale, 1.0f / primitive.z1, color,
                  primitive.thickness);
    DrawDepthLine(image, inv_depth, primitive.p1.x * raster_scale, primitive.p1.y * raster_scale,
                  1.0f / primitive.z1, primitive.p2.x * raster_scale,
                  primitive.p2.y * raster_scale, 1.0f / primitive.z2, color,
                  primitive.thickness);
    DrawDepthLine(image, inv_depth, primitive.p2.x * raster_scale, primitive.p2.y * raster_scale,
                  1.0f / primitive.z2, primitive.p0.x * raster_scale,
                  primitive.p0.y * raster_scale, 1.0f / primitive.z0, color,
                  primitive.thickness);
  }
  painter.drawImage(QRect(QPoint(0, 0), size), image);
}

static u32 CanvasKeyBit(int key)
{
  switch (key)
  {
  case Qt::Key_W:
    return API::Gui::CanvasKey_W;
  case Qt::Key_A:
    return API::Gui::CanvasKey_A;
  case Qt::Key_S:
    return API::Gui::CanvasKey_S;
  case Qt::Key_D:
    return API::Gui::CanvasKey_D;
  case Qt::Key_Space:
    return API::Gui::CanvasKey_Space;
  case Qt::Key_Shift:
    return API::Gui::CanvasKey_Shift;
  default:
    return 0;
  }
}

const QPixmap& ScriptCanvasWidget::LoadPixmap(const QString& path)
{
  auto it = m_pixmaps.find(path);
  if (it == m_pixmaps.end())
    it = m_pixmaps.insert(path, QPixmap(path));  // null pixmap if the file is missing
  return *it;
}

// Multiply-tints a texture (like LOVE's setColor): black stays black, white becomes the tint,
// so a black-body/white-ring sprite yields a black body with a colored ring. Alpha is preserved.
const QPixmap& ScriptCanvasWidget::TintedPixmap(const QString& path, u32 argb)
{
  const QString key = path + QLatin1Char('|') + QString::number(argb, 16);
  auto it = m_tinted.find(key);
  if (it != m_tinted.end())
    return *it;

  QPixmap tinted = LoadPixmap(path);
  if (!tinted.isNull())
  {
    QPainter p(&tinted);
    p.setCompositionMode(QPainter::CompositionMode_Multiply);
    p.fillRect(tinted.rect(), ArgbToColor(argb | 0xFF000000u));
    // Multiply floods transparent texels with the fill; re-mask to the original alpha.
    p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    p.drawPixmap(0, 0, LoadPixmap(path));
  }
  return *m_tinted.insert(key, tinted);
}

ScriptCanvasWidget::ScriptCanvasWidget(int width, int height, bool overlay, API::Gui::WidgetId id,
                                       QWidget* parent)
    : QWidget(parent, overlay ? (Qt::Window | Qt::WindowStaysOnTopHint) :
                                 parent ? Qt::Widget : Qt::Window),
      m_requested_size(width, height), m_overlay(overlay), m_id(id)
{
  resize(width, height);
  // Honor the requested size as the floor so a sibling form layout can't compress the canvas
  // below it on initial show; Expanding still lets the user grow the window past it.
  setMinimumSize(width, height);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  setFocusPolicy(Qt::StrongFocus);
  // Track motion even with no button held so scripts get live hover coordinates.
  setMouseTracking(true);
}

void ScriptCanvasWidget::SetTransparentBackground(bool transparent)
{
  m_transparent_background = transparent;
  setAttribute(Qt::WA_TranslucentBackground, transparent);
  setAutoFillBackground(!transparent);
  update();
}

void ScriptCanvasWidget::mousePressEvent(QMouseEvent* event)
{
  setFocus(Qt::MouseFocusReason);
  const QPointF p = event->position();
  if (event->button() == Qt::LeftButton)
  {
    API::GetGui().CanvasReportClick(m_id, static_cast<float>(p.x()), static_cast<float>(p.y()));
    API::GetGui().CanvasReportLeftDown(m_id, true);
  }
  else if (event->button() == Qt::RightButton)
  {
    API::GetGui().CanvasReportRightClick(m_id, static_cast<float>(p.x()), static_cast<float>(p.y()));
    API::GetGui().CanvasReportRightDown(m_id, true);
  }
  API::GetGui().CanvasReportMouse(m_id, static_cast<float>(p.x()), static_cast<float>(p.y()), true);
}

void ScriptCanvasWidget::mouseReleaseEvent(QMouseEvent* event)
{
  const QPointF p = event->position();
  if (event->button() == Qt::LeftButton)
    API::GetGui().CanvasReportLeftDown(m_id, false);
  else if (event->button() == Qt::RightButton)
    API::GetGui().CanvasReportRightDown(m_id, false);
  API::GetGui().CanvasReportMouse(m_id, static_cast<float>(p.x()), static_cast<float>(p.y()), true);
}

void ScriptCanvasWidget::mouseMoveEvent(QMouseEvent* event)
{
  const QPointF p = event->position();
  API::GetGui().CanvasReportMouse(m_id, static_cast<float>(p.x()), static_cast<float>(p.y()), true);
}

void ScriptCanvasWidget::keyPressEvent(QKeyEvent* event)
{
  const u32 bit = CanvasKeyBit(event->key());
  if (bit)
  {
    m_key_mask |= bit;
    API::GetGui().CanvasReportKeyMask(m_id, m_key_mask);
    // Keep the script's held-key state while allowing Dolphin shortcuts such
    // as frame advance to propagate beyond the focused canvas.
    event->ignore();
    QWidget::keyPressEvent(event);
    return;
  }
  QWidget::keyPressEvent(event);
}

void ScriptCanvasWidget::keyReleaseEvent(QKeyEvent* event)
{
  const u32 bit = CanvasKeyBit(event->key());
  if (bit)
  {
    m_key_mask &= ~bit;
    API::GetGui().CanvasReportKeyMask(m_id, m_key_mask);
    event->ignore();
    QWidget::keyReleaseEvent(event);
    return;
  }
  QWidget::keyReleaseEvent(event);
}

void ScriptCanvasWidget::focusOutEvent(QFocusEvent* event)
{
  m_key_mask = 0;
  API::GetGui().CanvasReportKeyMask(m_id, 0);
  API::GetGui().CanvasReportLeftDown(m_id, false);
  API::GetGui().CanvasReportRightDown(m_id, false);
  QWidget::focusOutEvent(event);
}

void ScriptCanvasWidget::wheelEvent(QWheelEvent* event)
{
  // angleDelta is in eighths of a degree; one mouse notch is 120 → 1.0 notch.
  API::GetGui().CanvasReportWheel(m_id, event->angleDelta().y() / 120.0f);
  event->accept();
}

void ScriptCanvasWidget::leaveEvent(QEvent*)
{
  // Mark the cursor as outside; keep the last position so a script can still read it.
  bool inside = false;
  const Vec2f last = API::GetGui().CanvasMousePos(m_id, inside);
  API::GetGui().CanvasReportMouse(m_id, last.x, last.y, false);
}

void ScriptCanvasWidget::resizeEvent(QResizeEvent* event)
{
  QWidget::resizeEvent(event);
  API::GetGui().CanvasReportSize(m_id, event->size().width(), event->size().height());
}

void ScriptCanvasWidget::closeEvent(QCloseEvent* event)
{
  // Let the menu toggle drive teardown so the two stay in sync.
  if (m_overlay)
    emit closed();
  QWidget::closeEvent(event);
}

void ScriptCanvasWidget::SetPrimitives(std::vector<API::Gui::CanvasPrimitive> prims)
{
  m_prims = std::move(prims);
  update();
}

void ScriptCanvasWidget::paintEvent(QPaintEvent*)
{
  QPainter painter(this);
  if (m_transparent_background)
  {
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
  }
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setRenderHint(QPainter::TextAntialiasing, true);
  painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

  using Type = API::Gui::CanvasPrimitive::Type;
  bool depth_pass_drawn = false;
  for (const auto& p : m_prims)
  {
    if (p.type == Type::DepthTriangleFilled || p.type == Type::DepthTriangleWire)
    {
      if (!depth_pass_drawn)
      {
        DrawDepthTriangles(painter, m_prims, size());
        depth_pass_drawn = true;
      }
      continue;
    }
    const QColor color = ArgbToColor(p.color);
    const QPen pen(color, p.thickness);
    switch (p.type)
    {
    case Type::Line:
      painter.setPen(pen);
      painter.drawLine(Pt(p.p0), Pt(p.p1));
      break;
    case Type::Rect:
      painter.setPen(pen);
      painter.setBrush(Qt::NoBrush);
      painter.drawRoundedRect(QRectF(Pt(p.p0), Pt(p.p1)), p.rounding, p.rounding);
      break;
    case Type::RectFilled:
      painter.setPen(Qt::NoPen);
      painter.setBrush(color);
      painter.drawRoundedRect(QRectF(Pt(p.p0), Pt(p.p1)), p.rounding, p.rounding);
      break;
    case Type::Circle:
      painter.setPen(pen);
      painter.setBrush(Qt::NoBrush);
      painter.drawEllipse(Pt(p.p0), p.radius, p.radius);
      break;
    case Type::CircleFilled:
      painter.setPen(Qt::NoPen);
      painter.setBrush(color);
      painter.drawEllipse(Pt(p.p0), p.radius, p.radius);
      break;
    case Type::Triangle:
    case Type::TriangleFilled:
    {
      QPolygonF tri;
      tri << Pt(p.p0) << Pt(p.p1) << Pt(p.p2);
      if (p.type == Type::TriangleFilled)
      {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
      }
      else
      {
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
      }
      painter.drawPolygon(tri);
      break;
    }
    case Type::DepthTriangleFilled:
    case Type::DepthTriangleWire:
      break;
    case Type::Text:
    {
      painter.save();
      QFont font = painter.font();
      if (!p.font_family.empty())
        font.setFamily(QString::fromStdString(p.font_family));
      font.setPixelSize(std::max(1, static_cast<int>(p.text_size)));
      font.setBold(p.text_bold);
      painter.setFont(font);
      painter.setPen(color);
      painter.drawText(Pt(p.p0), QString::fromStdString(p.text));
      painter.restore();
      break;
    }
    case Type::Image:
    {
      const QString path = QString::fromStdString(p.image);
      // color==0 means draw untinted; otherwise its RGB tints and its alpha scales opacity.
      const QPixmap& pix = p.color == 0 ? LoadPixmap(path) : TintedPixmap(path, p.color);
      if (pix.isNull())
        break;
      const QRectF dest(Pt(p.p0), Pt(p.p1));
      const QRectF src(p.src_min.x * pix.width(), p.src_min.y * pix.height(),
                       (p.src_max.x - p.src_min.x) * pix.width(),
                       (p.src_max.y - p.src_min.y) * pix.height());
      const qreal prev = painter.opacity();
      if (p.color != 0)
        painter.setOpacity(prev * (((p.color >> 24) & 0xFF) / 255.0));
      painter.drawPixmap(dest, pix, src);
      painter.setOpacity(prev);
      break;
    }
    }
  }
}
