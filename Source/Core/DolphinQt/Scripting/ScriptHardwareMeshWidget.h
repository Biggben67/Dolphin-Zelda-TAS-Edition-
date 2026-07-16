// Copyright 2026 Dolphin Emulator Project
// Licensed under GPLv2+

#pragma once

#include <array>
#include <memory>

#include <QImage>
#include <QWidget>

#include "Core/API/Gui.h"

class QFocusEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEngine;
class QPaintEvent;
class QResizeEvent;
class QWheelEvent;
class ScriptHardwareMeshWindow;

// Native GPU surface used only by the TP collision viewer. On Windows this
// uses the existing Direct3D surface; other platforms use a QtGui OpenGL
// surface. Both paths retain collision vertices on the GPU.
class ScriptHardwareMeshWidget final : public QWidget
{
  Q_OBJECT
public:
  explicit ScriptHardwareMeshWidget(API::Gui::WidgetId id, QWidget* parent = nullptr);
  ~ScriptHardwareMeshWidget() override;

  void SetSnapshot(API::Gui::HardwareSnapshot snapshot);
  bool IsReady() const { return m_ready; }
  // The frame dump manager cannot use QWidget::render() on this native D3D
  // child. Return a CPU image from the resolved swap-chain backbuffer instead.
  QImage CaptureFrame();

protected:
  QPaintEngine* paintEngine() const override;
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void keyReleaseEvent(QKeyEvent* event) override;
  void focusOutEvent(QFocusEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;

private:
  bool EnsureResources();
  void RecreateTargets();
  void UploadMeshes();
  void Render();
  void Present();
  void ReleaseResources();

  API::Gui::HardwareSnapshot m_snapshot;
  std::array<std::shared_ptr<const std::vector<API::Gui::HardwareVertex>>, 8> m_uploaded_groups;
  bool m_mesh_dirty = true;
  bool m_targets_dirty = true;
  bool m_ready = false;
  API::Gui::WidgetId m_id;
  u32 m_key_mask = 0;

  struct Resources;
  std::unique_ptr<Resources> m_resources;

#ifndef _WIN32
  friend class ScriptHardwareMeshWindow;
  ScriptHardwareMeshWindow* m_gl_window = nullptr;
  QWidget* m_gl_container = nullptr;
#endif
};
