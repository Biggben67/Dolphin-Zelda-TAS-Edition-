// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <array>
#include <functional>
#include <imgui.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

#ifdef DrawText
#undef DrawText
#endif

using Vec2f = ImVec2;

namespace API
{
class Gui
{
public:
  // All colors are ARGB

  void AddOSDMessage(std::string message, u32 duration_ms = 2000, u32 color = 0xffffff30);
  void ClearOSDMessages();

  void Render();

  Vec2f GetDisplaySize();
  void DrawLine(const Vec2f a, const Vec2f b, u32 color, float thickness = 1.0f);
  void DrawRect(const Vec2f a, const Vec2f b, u32 color, float rounding = 0.0f,
                float thickness = 1.0f);
  void DrawRectFilled(const Vec2f a, const Vec2f b, u32 color, float rounding = 0.0f);
  void DrawQuad(const Vec2f a, const Vec2f b, const Vec2f c, const Vec2f d, u32 color,
                float thickness = 1.0f);
  void DrawQuadFilled(const Vec2f a, const Vec2f b, const Vec2f c, const Vec2f d, u32 color);
  void DrawTriangle(const Vec2f a, const Vec2f b, const Vec2f c, u32 color, float thickness = 1.0f);
  void DrawTriangleFilled(const Vec2f a, const Vec2f b, const Vec2f c, u32 color);
  void DrawCircle(const Vec2f center, float radius, u32 color, int num_segments = 12,
                  float thickness = 1.0f);
  void DrawCircleFilled(const Vec2f center, float radius, u32 color, int num_segments = 12);
  void DrawText(const Vec2f pos, u32 color, std::string text);
  void CreateThickOutline(const Vec2f& pos, u32 color, std::string& text);
  void CreateThinOutline(const Vec2f& pos, u32 color, std::string& text);
  void DrawPolyline(const std::vector<Vec2f> points, u32 color, bool closed, float thickness);
  void DrawConvexPolyFilled(const std::vector<Vec2f> points, u32 color);

  // Retained-mode widgets: scripts mutate this tree, the video thread renders it
  // in RenderWidgets() and writes results back, so reads lag input by one frame.
  using WidgetId = u64;
  enum class WidgetKind
  {
    Window,
    Button,
    SliderFloat,
    Text,
    Checkbox,
    InputText,
  };
  struct Widget
  {
    WidgetKind kind;
    void* owner;
    std::string label;
    std::vector<WidgetId> children;  // Window only
    bool embedded = true;            // Window only: false = detached Qt window
    bool clicked = false;            // Button: latched until read
    bool checked = false;            // Checkbox
    float value = 0.0f;              // SliderFloat
    float min = 0.0f;
    float max = 1.0f;
    std::string text_value;          // InputText: editable contents
    std::optional<u32> text_color;   // ARGB
    std::optional<u32> bg_color;     // ARGB
    std::string style;               // QSS, detached only
    std::string group;               // Detached-window control section
    bool canvas = false;             // Window only: freeform QPainter canvas
    bool hardware_canvas = false;    // Window only: native GPU canvas requested by the script
    int canvas_w = 0, canvas_h = 0;  // Window only
    bool overlay = false;            // Window only: frameless, stays-on-top, not user-closable
    bool visible = true;             // child widgets: hidden ones aren't rendered
  };

  // Freeform canvas: a window backed by a replayable primitive list instead of widgets.
  // Scripts push primitives, call CanvasCommit, and Qt replays them via QPainter.
  struct CanvasPrimitive
  {
    enum class Type : u8
    {
      Line,
      Rect,
      RectFilled,
      Circle,
      CircleFilled,
      Triangle,
      TriangleFilled,
      DepthTriangleFilled,
      DepthTriangleWire,
      Text,
      Image,
    };
    Type type;
    Vec2f p0, p1, p2;
    float radius = 0.0f;
    float thickness = 1.0f;
    float rounding = 0.0f;
    u32 color = 0;  // ARGB
    std::string text;
    // Image only: path + dest rect (p0..p1) + normalized [0,1] src crop. color==0 draws untinted;
    // else RGB tints the texture and alpha scales opacity.
    std::string image;
    Vec2f src_min{0.0f, 0.0f};
    Vec2f src_max{1.0f, 1.0f};
    // Depth triangle primitives: camera-space depth at p0/p1/p2.  Kept at
    // the end so existing aggregate construction stays source-compatible.
    float z0 = 0.0f, z1 = 0.0f, z2 = 0.0f;
  };

  // Retained world-space data for a hardware-backed detached canvas. Meshes
  // are uploaded only when their source collision changes; camera state is
  // updated independently each frame.
  struct HardwareVertex
  {
    float x, y, z;
    u32 color;
  };
  struct HardwareState
  {
    bool enabled = false;
    std::array<float, 3> eye{}, right{}, up{}, forward{};
    float focal = 1.0f;
    float radius = 0.0f;
    float fill_opacity = 1.0f;
    float wire_opacity = 1.0f;
    bool filled = true;
    bool wireframe = true;
    bool xray = false;
    bool debug_on_top = false;
    bool fullscreen = false;
    bool clean_capture = false;
  };
  struct HardwareSnapshot
  {
    // Eight script-addressable mesh slots. The renderer uses slots 0-4 as
    // depth-writing triangles, slot 5 as a line list, slot 6 as triangles
    // drawn above the depth buffer, and slot 7 as depth-tested overlay fills.
    // This is a rendering contract, not a game or tool identity.
    std::array<std::shared_ptr<const std::vector<HardwareVertex>>, 8> groups;
    // Script-owned screen-space HUD. The native renderer composites these
    // retained primitives after the world mesh, so no script needs a second
    // QWidget or a renderer-specific C++ overlay to draw an interface.
    std::shared_ptr<const std::vector<CanvasPrimitive>> hud;
    HardwareState state;
    u64 generation = 0;
  };


  WidgetId GetOrCreateCanvas(void* owner, const std::string& title, int width, int height,
                             bool embedded = false, bool overlay = false);
  void CanvasClear(WidgetId id);
  void CanvasAdd(WidgetId id, const CanvasPrimitive& prim);
  void CanvasCommit(WidgetId id);
  std::vector<CanvasPrimitive> SnapshotCanvas(WidgetId id);
  u64 CanvasGeneration(WidgetId id);
  void SetHardwareMesh(WidgetId id, size_t group, std::vector<HardwareVertex> vertices);
  void SetHardwareState(WidgetId id, const HardwareState& state);
  void SetHardwareHud(WidgetId id, std::vector<CanvasPrimitive> primitives);
  HardwareSnapshot SnapshotHardwareMesh(WidgetId id);

  // Canvas pointer input: Qt widget reports (main thread), scripts consume (emu thread).
  // Click and wheel latch until read so a once-per-frame poll never misses one.
  struct CanvasInput
  {
    float mouse_x = 0.0f, mouse_y = 0.0f;
    bool inside = false;
    bool clicked = false;       // latched until CanvasTakeClick
    float click_x = 0.0f, click_y = 0.0f;
    bool right_clicked = false;  // latched until CanvasTakeRightClick
    float right_click_x = 0.0f, right_click_y = 0.0f;
    bool left_down = false;
    bool right_down = false;
    bool capture_toggle = false;  // latched H press from the focused hardware canvas
    u32 key_mask = 0;
    float wheel_accum = 0.0f;   // accumulated wheel notches until CanvasTakeWheel
  };
  enum CanvasKey : u32
  {
    CanvasKey_W = 1 << 0,
    CanvasKey_A = 1 << 1,
    CanvasKey_S = 1 << 2,
    CanvasKey_D = 1 << 3,
    CanvasKey_Space = 1 << 4,
    CanvasKey_Shift = 1 << 5,
  };
  void CanvasReportMouse(WidgetId id, float x, float y, bool inside);  // Qt thread
  void CanvasReportClick(WidgetId id, float x, float y);               // Qt thread
  void CanvasReportRightClick(WidgetId id, float x, float y);          // Qt thread
  void CanvasReportLeftDown(WidgetId id, bool down);                    // Qt thread
  void CanvasReportRightDown(WidgetId id, bool down);                   // Qt thread
  void CanvasReportKeyMask(WidgetId id, u32 mask);                      // Qt thread
  void CanvasReportCaptureToggle(WidgetId id);                           // Qt thread
  void CanvasReportWheel(WidgetId id, float delta);                    // Qt thread
  void CanvasReportSize(WidgetId id, int w, int h);                    // Qt thread
  std::pair<int, int> CanvasSize(WidgetId id);                        // script thread
  Vec2f CanvasMousePos(WidgetId id, bool& inside);                     // script thread
  bool CanvasTakeClick(WidgetId id, Vec2f& pos);                       // script thread, consumes
  bool CanvasTakeRightClick(WidgetId id, Vec2f& pos);                  // script thread, consumes
  bool CanvasLeftDown(WidgetId id);                                     // script thread
  bool CanvasRightDown(WidgetId id);                                    // script thread
  u32 CanvasKeyMask(WidgetId id);                                       // script thread
  bool CanvasTakeCaptureToggle(WidgetId id);                            // script thread, consumes
  float CanvasTakeWheel(WidgetId id);                                  // script thread, consumes

  WidgetId GetOrCreateWindow(void* owner, const std::string& title, bool embedded = true);
  // Attaches a freeform canvas region to an existing window so it hosts a canvas and child widgets.
  void EnableCanvas(WidgetId window, int width, int height);
  // Opt a detached canvas window into the native hardware mesh renderer.
  // This is a script capability, not a per-tool or per-game special case.
  void EnableHardwareCanvas(WidgetId window);
  WidgetId AddChild(WidgetId parent, WidgetKind kind, const std::string& label);
  void SetChildGroup(WidgetId id, const std::string& group);
  bool TakeClicked(WidgetId id);
  float GetValue(WidgetId id);
  void SetValue(WidgetId id, float value);
  void SetSliderRange(WidgetId id, float min, float max);
  void SetText(WidgetId id, const std::string& text);
  void SetClicked(WidgetId id);
  bool GetChecked(WidgetId id);
  void SetChecked(WidgetId id, bool checked);
  bool GetVisible(WidgetId id);
  void SetVisible(WidgetId id, bool visible);
  std::string GetInputText(WidgetId id);
  void SetInputText(WidgetId id, const std::string& text);
  void SetTextColor(WidgetId id, u32 color);
  void SetBgColor(WidgetId id, u32 color);
  void SetStyle(WidgetId id, const std::string& style);
  void SetClipboardText(std::string text);
  bool TakeClipboardText(std::string& text);  // Qt thread, consumes
  void RemoveWidgetsForOwner(void* owner);

  // Snapshot for the Qt detached-window manager; called from the Qt main thread.
  struct ChildInfo
  {
    WidgetId id;
    WidgetKind kind;
    std::string label;
    float value;
    float min;
    float max;
    bool checked;
    std::string text_value;
    std::optional<u32> text_color;
    std::optional<u32> bg_color;
    std::string style;
    std::string group;
    bool visible;
  };
  struct WindowInfo
  {
    WidgetId id;
    std::string title;
    std::vector<ChildInfo> children;
    std::optional<u32> text_color;
    std::optional<u32> bg_color;
    std::string style;
    bool canvas = false;
    bool hardware_canvas = false;
    int canvas_w = 0, canvas_h = 0;
    bool overlay = false;
  };
  std::vector<WindowInfo> SnapshotDetachedWindows();

  // Script GUI windows are transparent to Dolphin's input gate. Detached Qt
  // windows report their focus independently of embedded ImGui windows.
  bool IsScriptWindowFocused() const
  {
    return m_script_window_focused.load() || m_detached_script_window_focused.load();
  }
  bool IsDetachedScriptWindowFocused() const { return m_detached_script_window_focused.load(); }
  void SetDetachedScriptWindowFocused(bool focused) { m_detached_script_window_focused.store(focused); }
  // Detached script tools should not make Dolphin's normal hotkeys unusable.
  bool HasDetachedScriptWindows() const { return m_detached_script_windows_present.load(); }
  void SetDetachedScriptWindowsPresent(bool present)
  {
    m_detached_script_windows_present.store(present);
  }

private:
  void RenderWidgets();
  // Replays a committed canvas list at the current cursor; caller is already inside ImGui::Begin.
  void RenderEmbeddedCanvas(WidgetId id, int width, int height);

private:
  // Pushing all draw calls onto a vector of functions is probably not
  // the most efficient thing in the world, but it seems to be negligible.
  // Since we want script code to be able to draw stuff whenever it wants
  // without having to worry about when exactly to draw, just deferring the
  // execution of draw calls like this is an easy solution.
  std::vector<std::function<void(ImDrawList*)>> m_draw_calls;

  std::mutex m_widget_mutex;
  std::map<WidgetId, Widget> m_widgets;
  std::vector<WidgetId> m_windows;  // top-level, in creation order
  // Canvas primitive lists keyed by window id; building is script-side, committed is Qt-visible.
  std::map<WidgetId, std::vector<CanvasPrimitive>> m_canvas_building;
  std::map<WidgetId, std::vector<CanvasPrimitive>> m_canvas_committed;
  std::map<WidgetId, u64> m_canvas_generations;
  std::map<WidgetId, HardwareSnapshot> m_hardware_meshes;
  // Pointer input per canvas, written by Qt, drained by scripts.
  std::map<WidgetId, CanvasInput> m_canvas_input;
  std::optional<std::string> m_pending_clipboard_text;
  WidgetId m_next_widget_id = 1;
  std::atomic<bool> m_script_window_focused{false};  // set by video thread in RenderWidgets
  std::atomic<bool> m_detached_script_window_focused{false};  // set by ScriptWindowManager
  std::atomic<bool> m_detached_script_windows_present{false};  // set by ScriptWindowManager
};

// global instance
Gui& GetGui();

}  // namespace API
