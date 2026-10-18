// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <optional>

#include <QObject>
#include <QByteArray>
#include <QRect>
#include <QTimer>
#include <QString>
#include <QWidget>

class QVBoxLayout;
class QScrollArea;
class ScriptHardwareMeshWidget;

#include "Core/API/Gui.h"
#include "DolphinQt/Scripting/ScriptCanvasWidget.h"

// Manages free-floating OS windows for non-embedded script GUI windows.
// Polls API::Gui's widget tree on a timer; Qt signals write results back.
class ScriptWindowManager : public QObject
{
  Q_OBJECT
public:
  explicit ScriptWindowManager(QObject* parent = nullptr);
  ~ScriptWindowManager();

signals:
  // A user closed an overlay canvas window (X / Alt+F4); its menu toggle should follow.
  void OverlayClosed();

private:
  struct ManagedWindow;
  void Sync();
  bool eventFilter(QObject* watched, QEvent* event) override;
  void ConfigureGeometryPersistence(ManagedWindow& window, const std::string& path);
  void SaveGeometry(const ManagedWindow& window) const;

  struct ManagedChild
  {
    QWidget* control;
    QWidget* caption = nullptr;  // separate caption label for slider/input_text, hidden as a unit
  };

  struct ManagedWindow
  {
    API::Gui::WidgetId id;
    QWidget* window;
    std::map<API::Gui::WidgetId, ManagedChild> children;
    ScriptCanvasWidget* canvas = nullptr;  // non-null for freeform canvas windows
    ScriptHardwareMeshWidget* hardware_mesh = nullptr;
    QWidget* canvas_host = nullptr;
    QVBoxLayout* root_layout = nullptr;
    QVBoxLayout* controls_layout = nullptr;
    std::map<std::string, QVBoxLayout*> group_layouts;
    QWidget* controls_host = nullptr;
    QScrollArea* controls_scroll = nullptr;
    bool grouped_layout = false;
    bool viewer_fullscreen = false;
    bool hardware_active = false;
    bool hardware_visibility_initialized = false;
    u64 canvas_generation = 0;
    bool canvas_generation_set = false;
    QString geometry_path;
    QByteArray observed_geometry;
    QTimer* geometry_save_timer = nullptr;
    bool applying_saved_geometry = false;
    std::optional<QRect> requested_geometry;
    int geometry_restore_cycles = 0;
  };

  std::map<API::Gui::WidgetId, ManagedWindow> m_windows;
  QTimer m_timer;
  // Dispatches paused-script UI work onto the emulation CPU thread.  Python
  // must never be invoked directly by a Qt timer.
  QTimer m_host_update_timer;
  // Coalesce heartbeat updates so background UI work cannot queue ahead of
  // emulation/frame-step jobs.
  std::shared_ptr<std::atomic_bool> m_host_update_pending = std::make_shared<std::atomic_bool>(false);
};
