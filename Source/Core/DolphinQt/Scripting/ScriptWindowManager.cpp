// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "DolphinQt/Scripting/ScriptWindowManager.h"

#include <algorithm>
#include <optional>

#include <QCheckBox>
#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegion>
#include <QScrollArea>
#include <QStackedLayout>
#include <QSlider>
#include <QVBoxLayout>
#include <QWindow>

#include "Core/API/Events.h"
#include "Core/API/Gui.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "Scripting/ScriptList.h"
#include "DolphinQt/Scripting/ScriptHardwareMeshWidget.h"

static constexpr int POLL_INTERVAL_MS = 16;
static constexpr int HOST_UPDATE_INTERVAL_MS = 33;

static bool IsTpCollisionViewer(const std::string& title)
{
  return title == "TP Collision + Trigger Viewer";
}

static bool IsTpTriggerControl(const std::string& label)
{
  return label == "Load zones" || label == "Event areas" || label == "Switch areas" ||
         label == "Restart changes" || label == "Other Triggers" || label == "Filled triggers" ||
         label == "On Top Triggers";
}

static bool IsTpColliderControl(const std::string& label)
{
  return label == "Push colliders" || label == "Attack colliders" ||
         label == "Coordinate Dot";
}

// ARGB colors become rgba() QSS fragments; the raw style is appended last so it wins on conflict.
static QString BuildStyleSheet(const std::optional<u32>& text_color,
                               const std::optional<u32>& bg_color, const std::string& style)
{
  QString qss;
  auto rgba = [](u32 argb) {
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg((argb >> 16) & 0xFF)
        .arg((argb >> 8) & 0xFF)
        .arg(argb & 0xFF)
        .arg(((argb >> 24) & 0xFF) / 255.0);
  };
  if (text_color)
    qss += QStringLiteral("color: %1;").arg(rgba(*text_color));
  if (bg_color)
    qss += QStringLiteral("background-color: %1;").arg(rgba(*bg_color));
  qss += QString::fromStdString(style);
  return qss;
}

ScriptWindowManager::ScriptWindowManager(QObject* parent) : QObject(parent)
{
  auto* gui_app = static_cast<QGuiApplication*>(QCoreApplication::instance());
  connect(gui_app, &QGuiApplication::focusWindowChanged, this,
          [this](QWindow* focused_window) {
            const bool script_window_focused =
                std::any_of(m_windows.begin(), m_windows.end(), [focused_window](const auto& entry) {
                  return entry.second.window && entry.second.window->windowHandle() == focused_window;
                });
            API::GetGui().SetDetachedScriptWindowFocused(script_window_focused);
          });
  connect(&m_timer, &QTimer::timeout, this, &ScriptWindowManager::Sync);
  m_timer.start(POLL_INTERVAL_MS);

  // Let canvases process camera input while emulation is paused.  RunOnCPUThread
  // performs the required pause lock and executes Python on its owning CPU
  // thread; calling the event hub from Qt was the source of the prior crashes.
  connect(&m_host_update_timer, &QTimer::timeout, this, [pending = m_host_update_pending] {
    if (Scripts::IsConstructing())
      return;

    Core::System& system = Core::System::GetInstance();
    if (Core::GetState(system) != Core::State::Paused)
      return;

    if (pending->exchange(true))
      return;

    Core::RunOnCPUThread(system,
                         [pending] {
                           if (!Scripts::IsConstructing())
                             API::GetEventHub().EmitEvent(API::Events::HostUpdate{});
                           pending->store(false);
                         },
                         false);
  });
  m_host_update_timer.start(HOST_UPDATE_INTERVAL_MS);
}

ScriptWindowManager::~ScriptWindowManager()
{
  API::GetGui().SetDetachedScriptWindowFocused(false);
  API::GetGui().SetDetachedScriptWindowsPresent(false);
  for (auto& [id, mw] : m_windows)
    delete mw.window;
}

void ScriptWindowManager::Sync()
{
  // Materializing/snapshotting a window mid-construction races the ctor building the widget tree.
  if (Scripts::IsConstructing())
    return;

  API::Gui& gui = API::GetGui();
  std::string clipboard_text;
  if (gui.TakeClipboardText(clipboard_text))
    QGuiApplication::clipboard()->setText(QString::fromStdString(clipboard_text));

  const std::vector<API::Gui::WindowInfo> snapshots = gui.SnapshotDetachedWindows();
  std::optional<API::Gui::WidgetId> tp_viewer_id;
  for (const auto& snapshot : snapshots)
  {
    if (IsTpCollisionViewer(snapshot.title) && !tp_viewer_id)
      tp_viewer_id = snapshot.id;
  }
  const auto is_duplicate_tp_viewer = [&](API::Gui::WidgetId id) {
    if (!tp_viewer_id || id == *tp_viewer_id)
      return false;
    const auto it = std::find_if(snapshots.begin(), snapshots.end(),
                                 [id](const API::Gui::WindowInfo& snapshot) {
                                   return snapshot.id == id;
                                 });
    return it != snapshots.end() && IsTpCollisionViewer(it->title);
  };

  // Remove Qt windows whose tree node is gone.
  std::erase_if(m_windows, [&](auto& kv) {
    bool gone = std::none_of(snapshots.begin(), snapshots.end(),
                             [id = kv.first](const API::Gui::WindowInfo& s) { return s.id == id; });
    if (gone || is_duplicate_tp_viewer(kv.first))
      delete kv.second.window;
    return gone || is_duplicate_tp_viewer(kv.first);
  });

  for (const auto& snap : snapshots)
  {
    if (is_duplicate_tp_viewer(snap.id))
      continue;
    auto it = m_windows.find(snap.id);

    // Overlay canvas: a frameless stays-on-top top-level surface with no form children.
    if (snap.overlay)
    {
      if (it == m_windows.end())
      {
        auto* cw = new ScriptCanvasWidget(snap.canvas_w, snap.canvas_h, true, snap.id);
        connect(cw, &ScriptCanvasWidget::closed, this, &ScriptWindowManager::OverlayClosed);
        cw->setWindowTitle(QString::fromStdString(snap.title));
        cw->show();
        m_windows[snap.id] = ManagedWindow{snap.id, cw, {}, cw, nullptr, nullptr};
        it = m_windows.find(snap.id);
      }
      const u64 generation = gui.CanvasGeneration(snap.id);
      if (!it->second.canvas_generation_set || it->second.canvas_generation != generation)
      {
        it->second.canvas->SetPrimitives(gui.SnapshotCanvas(snap.id));
        it->second.canvas_generation = generation;
        it->second.canvas_generation_set = true;
      }
      continue;
    }

    if (it == m_windows.end())
    {
      // New window — a container that can hold a canvas surface and/or form widgets.
      QWidget* win = new QWidget(nullptr, Qt::Window);
      win->setWindowTitle(QString::fromStdString(snap.title));
      auto* root = new QVBoxLayout(win);
      root->setContentsMargins(8, 8, 8, 8);
      root->setSpacing(6);
      const bool tp_collision_layout = IsTpCollisionViewer(snap.title);
      auto* scroll = new QScrollArea(win);
      scroll->setWidgetResizable(true);
      scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
      scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
      if (tp_collision_layout)
      {
        scroll->setMinimumHeight(410);
        scroll->setMaximumHeight(455);
      }
      auto* controls = new QWidget(scroll);
      QVBoxLayout* controls_layout = nullptr;
      QVBoxLayout* trigger_layout = nullptr;
      QVBoxLayout* collider_layout = nullptr;
      if (tp_collision_layout)
      {
        auto* columns = new QHBoxLayout(controls);
        columns->setContentsMargins(4, 4, 4, 4);
        columns->setSpacing(8);
        auto* viewer_group = new QGroupBox(QStringLiteral("Collision and View"), controls);
        auto* side_column = new QWidget(controls);
        auto* side_layout = new QVBoxLayout(side_column);
        auto* trigger_group = new QGroupBox(QStringLiteral("Triggers"), controls);
        auto* collider_group = new QGroupBox(QStringLiteral("Colliders"), controls);
        controls_layout = new QVBoxLayout(viewer_group);
        trigger_layout = new QVBoxLayout(trigger_group);
        collider_layout = new QVBoxLayout(collider_group);
        controls_layout->setAlignment(Qt::AlignTop);
        trigger_layout->setAlignment(Qt::AlignTop);
        collider_layout->setAlignment(Qt::AlignTop);
        side_layout->setContentsMargins(0, 0, 0, 0);
        side_layout->setSpacing(8);
        side_layout->addWidget(trigger_group);
        side_layout->addWidget(collider_group);
        side_layout->addStretch();
        columns->addWidget(viewer_group, 3);
        columns->addWidget(side_column, 2);
      }
      else
      {
        controls_layout = new QVBoxLayout(controls);
        controls_layout->setAlignment(Qt::AlignTop);
      }
      scroll->setWidget(controls);
      // The TP viewport is the primary surface. The controls retain a usable
      // fixed-height, scrollable panel beneath it instead of consuming every
      // extra pixel when the detached window is enlarged.
      root->addWidget(scroll, 0);
      if (snap.text_color || snap.bg_color || !snap.style.empty())
        win->setStyleSheet(BuildStyleSheet(snap.text_color, snap.bg_color, snap.style));
      win->resize(tp_collision_layout ? 1200 : 720, tp_collision_layout ? 1180 : 640);
      win->show();
      m_windows[snap.id] = ManagedWindow{snap.id, win, {}, nullptr, nullptr, nullptr, root,
                                          controls_layout, trigger_layout, collider_layout, controls,
                                          scroll, tp_collision_layout};
      it = m_windows.find(snap.id);
    }

    ManagedWindow& mw = it->second;

    // The TP viewer owns a native D3D child. Keep the host alive independently
    // of the CPU fallback canvas: GPU mode must not leave a second native
    // painting surface beneath it.
    if (snap.canvas && !mw.canvas_host)
    {
      auto* canvas_host = new QWidget(mw.window);
      // The TP renderer is a native D3D child. Make its immediate host native
      // and opaque as well so Qt never briefly repaints a backing surface over
      // the viewport while processing a resize or a stale script snapshot.
      canvas_host->setAttribute(Qt::WA_NativeWindow);
      canvas_host->setAttribute(Qt::WA_OpaquePaintEvent);
      canvas_host->setAttribute(Qt::WA_NoSystemBackground);
      auto* canvas_layout = new QStackedLayout(canvas_host);
      canvas_layout->setContentsMargins(0, 0, 0, 0);
      canvas_layout->setStackingMode(QStackedLayout::StackAll);
      if (mw.tp_collision_layout)
      {
        auto* hardware = new ScriptHardwareMeshWidget(snap.id, canvas_host);
        canvas_layout->addWidget(hardware);
        mw.hardware_mesh = hardware;
      }
      mw.root_layout->insertWidget(0, canvas_host, mw.tp_collision_layout ? 1 : 0);
      mw.canvas_host = canvas_host;
    }

    const auto hardware_snapshot =
        mw.hardware_mesh ? gui.SnapshotHardwareMesh(snap.id) : API::Gui::HardwareSnapshot{};
    // The TP collision viewer is deliberately GPU-only. A one-tick false
    // state while a save state or room transition is being observed must not
    // resurrect the CPU canvas beneath the D3D child, which was the remaining
    // source of visible viewport/options flicker.
    const bool use_hardware = mw.hardware_mesh &&
                              (hardware_snapshot.state.enabled || mw.tp_collision_layout);
    if (mw.tp_collision_layout && mw.controls_scroll &&
        mw.viewer_fullscreen != hardware_snapshot.state.fullscreen)
    {
      mw.controls_scroll->setVisible(!hardware_snapshot.state.fullscreen);
      mw.viewer_fullscreen = hardware_snapshot.state.fullscreen;
    }

    // Construct the CPU canvas only for the explicit non-GPU fallback.  It is
    // deleted as soon as D3D is enabled, rather than hidden/lowered, because a
    // hidden native QWidget can still be promoted by a parent repaint on Win32.
    if (snap.canvas && !mw.canvas && !use_hardware)
    {
      auto* cw = new ScriptCanvasWidget(snap.canvas_w, snap.canvas_h, false, snap.id, mw.canvas_host);
      auto* canvas_layout = qobject_cast<QStackedLayout*>(mw.canvas_host->layout());
      canvas_layout->addWidget(cw);
      mw.canvas = cw;
    }
    if (mw.canvas && use_hardware)
    {
      delete mw.canvas;
      mw.canvas = nullptr;
      mw.canvas_generation_set = false;
    }
    if (mw.canvas)
    {
      const u64 generation = gui.CanvasGeneration(snap.id);
      if (!mw.canvas_generation_set || mw.canvas_generation != generation)
      {
        mw.canvas->SetPrimitives(gui.SnapshotCanvas(snap.id));
        mw.canvas_generation = generation;
        mw.canvas_generation_set = true;
      }
    }
    if (mw.hardware_mesh)
    {
      mw.hardware_mesh->SetSnapshot(hardware_snapshot);
      // Two native child windows cannot alpha-compose reliably on Windows.
      // Normal GPU mode therefore puts the D3D surface on top; it forwards
      // freecam/picking input into the shared script canvas state. CPU vertex
      // mode puts the canvas on top so its detailed selection UI remains visible.
      if (use_hardware)
      {
        if (!mw.hardware_visibility_initialized || !mw.hardware_active)
        {
          mw.hardware_mesh->show();
          mw.hardware_mesh->raise();
          mw.hardware_active = true;
          mw.hardware_visibility_initialized = true;
        }
      }
      else
      {
        if (!mw.hardware_visibility_initialized || mw.hardware_active)
        {
          mw.hardware_mesh->hide();
          mw.hardware_active = false;
          mw.hardware_visibility_initialized = true;
        }
        if (mw.canvas)
        {
          mw.canvas->clearMask();
          mw.canvas->show();
          mw.canvas->raise();
        }
      }
    }

    // Add any child widgets not yet created.
    for (const auto& child : snap.children)
    {
      if (mw.children.contains(child.id))
        continue;

      QWidget* w = nullptr;
      QWidget* caption = nullptr;
      QVBoxLayout* destination = mw.controls_layout;
      if (mw.tp_collision_layout && IsTpTriggerControl(child.label))
        destination = mw.trigger_layout;
      else if (mw.tp_collision_layout && IsTpColliderControl(child.label))
        destination = mw.collider_layout;
      switch (child.kind)
      {
      case API::Gui::WidgetKind::Button:
      {
        auto* btn = new QPushButton(QString::fromStdString(child.label), mw.controls_host);
        const API::Gui::WidgetId cid = child.id;
        connect(btn, &QPushButton::clicked, this,
                [cid] { API::GetGui().SetClicked(cid); });
        w = btn;
        break;
      }
      case API::Gui::WidgetKind::SliderFloat:
      {
        // QSlider is integer; map float range to 0–1000 steps.
        caption = new QLabel(QString::fromStdString(child.label), mw.controls_host);
        auto* slider = new QSlider(Qt::Horizontal, mw.controls_host);
        slider->setRange(0, 1000);
        const API::Gui::WidgetId cid = child.id;
        const float smin = child.min, smax = child.max;
        connect(slider, &QSlider::valueChanged, this, [cid, smin, smax](int v) {
          API::GetGui().SetValue(cid, smin + (smax - smin) * (v / 1000.0f));
        });
        // Init the handle from the model value (QSlider otherwise pins to its minimum) so a
        // script's starting value -- e.g. a bipolar slider centered at 0 -- is honored on load.
        if (smax > smin)
        {
          int iv = int((child.value - smin) / (smax - smin) * 1000.0f + 0.5f);
          iv = iv < 0 ? 0 : (iv > 1000 ? 1000 : iv);
          slider->setValue(iv);
        }
        // Right-click recenters the slider to the midpoint of its range (e.g. 0 on a
        // bipolar −x..+x slider); valueChanged then pushes the new value to the model.
        slider->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(slider, &QSlider::customContextMenuRequested, this,
                [slider](const QPoint&) { slider->setValue(500); });
        w = slider;
        break;
      }
      case API::Gui::WidgetKind::Text:
        w = new QLabel(QString::fromStdString(child.label), mw.controls_host);
        break;
      case API::Gui::WidgetKind::Checkbox:
      {
        auto* box = new QCheckBox(QString::fromStdString(child.label), mw.controls_host);
        box->setChecked(child.checked);
        const API::Gui::WidgetId cid = child.id;
        connect(box, &QCheckBox::toggled, this,
                [cid](bool on) { API::GetGui().SetChecked(cid, on); });
        w = box;
        break;
      }
      case API::Gui::WidgetKind::InputText:
      {
        caption = new QLabel(QString::fromStdString(child.label), mw.controls_host);
        auto* edit = new QLineEdit(QString::fromStdString(child.text_value), mw.controls_host);
        const API::Gui::WidgetId cid = child.id;
        connect(edit, &QLineEdit::textEdited, this,
                [cid](const QString& t) { API::GetGui().SetInputText(cid, t.toStdString()); });
        w = edit;
        break;
      }
      default:
        break;
      }
      if (w)
      {
        if (caption)
          destination->addWidget(caption);
        if (child.text_color || child.bg_color || !child.style.empty())
          w->setStyleSheet(BuildStyleSheet(child.text_color, child.bg_color, child.style));
        destination->addWidget(w);
        mw.children[child.id] = {w, caption};
      }
    }

    // Sync live text labels and per-widget visibility.
    for (const auto& child : snap.children)
    {
      auto cit = mw.children.find(child.id);
      if (cit == mw.children.end())
        continue;
      if (child.kind == API::Gui::WidgetKind::Text)
        if (auto* lbl = qobject_cast<QLabel*>(cit->second.control))
          lbl->setText(QString::fromStdString(child.label));
      // Reflect model->widget (the signal path is one-way widget->model) so a script-driven
      // change -- e.g. forcing a checkbox off for mutual exclusion -- shows; block to avoid echo.
      if (child.kind == API::Gui::WidgetKind::Checkbox)
        if (auto* box = qobject_cast<QCheckBox*>(cit->second.control))
          if (box->isChecked() != child.checked)
          {
            box->blockSignals(true);
            box->setChecked(child.checked);
            box->blockSignals(false);
          }
      cit->second.control->setVisible(child.visible);
      if (cit->second.caption)
        cit->second.caption->setVisible(child.visible);
    }
  }
  API::GetGui().SetDetachedScriptWindowsPresent(!m_windows.empty());
}
