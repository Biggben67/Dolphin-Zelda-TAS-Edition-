// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "guimodule.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "Common/Logging/Log.h"
#include "Core/API/Gui.h"
#include "Scripting/Python/PyScriptingBackend.h"
#include "Scripting/Python/Utils/module.h"

namespace PyScripting
{

struct GuiModuleState
{
  API::Gui* gui;
};

static void add_osd_message(PyObject* self, const char* message, u32 duration_ms, u32 color_argb)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->AddOSDMessage(std::string(message), duration_ms, color_argb);
}

static void clear_osd_messages(PyObject* self)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->ClearOSDMessages();
}

static void set_clipboard(PyObject* self, const char* text)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->SetClipboardText(std::string(text));
}

static PyObject* get_display_size(PyObject* self, PyObject* args)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  auto size = state->gui->GetDisplaySize();
  return Py_BuildValue("(ff)", size.x, size.y);
}

static void draw_line(PyObject* self, float ax, float ay, float bx, float by, u32 color, float thickness = 1.0f)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->DrawLine({ax, ay}, {bx, by}, color, thickness);
}

static void draw_rect(PyObject* self, float ax, float ay, float bx, float by, u32 color,
               float rounding = 0.0f, float thickness = 1.0f)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->DrawRect({ax, ay}, {bx, by}, color, rounding, thickness);
}

static void draw_rect_filled(PyObject* self, float ax, float ay, float bx, float by, u32 color,
                      float rounding = 0.0f)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->DrawRectFilled({ax, ay}, {bx, by}, color, rounding);
}

static void draw_quad(PyObject* self, float ax, float ay, float bx, float by, float cx, float cy, float dx,
               float dy, u32 color, float thickness = 1.0f)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->DrawQuad({ax, ay}, {bx, by}, {cx, cy}, {dx, dy}, color, thickness);
}

static void draw_quad_filled(PyObject* self, float ax, float ay, float bx, float by, float cx, float cy,
                      float dx, float dy, u32 color)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->DrawQuadFilled({ax, ay}, {bx, by}, {cx, cy}, {dx, dy}, color);
}

static void draw_triangle(PyObject* self, float ax, float ay, float bx, float by, float cx, float cy,
                   u32 color, float thickness = 1.0f)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->DrawTriangle({ax, ay}, {bx, by}, {cx, cy}, color, thickness);
}

static void draw_triangle_filled(PyObject* self, float ax, float ay, float bx, float by, float cx,
                          float cy, u32 color)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->DrawTriangleFilled({ax, ay}, {bx, by}, {cx, cy}, color);
}

static void draw_circle(PyObject* self, float centerX, float centerY, float radius, u32 color,
                 int num_segments = 12, float thickness = 1.0f)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->DrawCircle({centerX, centerY}, radius, color, num_segments, thickness);
}

static void draw_circle_filled(PyObject* self, float centerX, float centerY, float radius, u32 color,
                        int num_segments = 12)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->DrawCircleFilled({centerX, centerY}, radius, color, num_segments);
}

static void draw_text(PyObject* self, float posX, float posY, u32 color, const char* text)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->DrawText({posX, posY}, color, std::string(text));
}

static PyObject* draw_polyline(PyObject* self, PyObject* args)
{
  PyObject* points_list_obj;
  u32 color;
  bool closed;
  float thickness;
  if (!PyArg_ParseTuple(args, "O!Ipf", &PyList_Type, &points_list_obj, &color, &closed,
                        &thickness))
    return nullptr;
  int num_points = PyList_Size(points_list_obj);
  if (num_points < 0)
    return nullptr;
  std::vector<Vec2f> points_collecting;
  for (int i = 0; i < num_points; ++i)
  {
    PyObject* item = PyList_GetItem(points_list_obj, i);
    float x, y;
    if (!PyArg_ParseTuple(item, "ff", &x, &y))
      return nullptr;
    points_collecting.push_back({x, y});
  }
  const std::vector<Vec2f> points = points_collecting;
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->DrawPolyline(points, color, closed, thickness);
  Py_RETURN_NONE;
}

static PyObject* draw_convex_poly_filled(PyObject* self, PyObject* args)
{
  PyObject* points_list_obj;
  u32 color;
  if (!PyArg_ParseTuple(args, "O!I", &PyList_Type, &points_list_obj, &color))
    return nullptr;
  int num_points = PyList_Size(points_list_obj);
  if (num_points < 0)
    return nullptr;
  std::vector<Vec2f> points;
  for (int i = 0; i < num_points; ++i)
  {
    PyObject* item = PyList_GetItem(points_list_obj, i);
    float x, y;
    if (!PyArg_ParseTuple(item, "ff", &x, &y))
      return nullptr;
    points.push_back({x, y});
  }
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->DrawConvexPolyFilled(points, color);
  Py_RETURN_NONE;
}

// Retained-mode widgets. The owner is the current backend so a script's windows
// are pruned together when it stops; ids are opaque handles into the Gui tree.
static void* CurrentOwner()
{
  return PyScripting::PyScriptingBackend::GetCurrent();
}

static u64 widget_window(PyObject* self, const char* title, int embedded)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  return state->gui->GetOrCreateWindow(CurrentOwner(), std::string(title), embedded != 0);
}

static void widget_enable_canvas(PyObject* self, u64 id, int width, int height)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->EnableCanvas(id, width, height);
}

static void widget_enable_hardware_canvas(PyObject* self, u64 id)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->EnableHardwareCanvas(id);
}

static void widget_set_group(PyObject* self, u64 id, const char* group)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->SetChildGroup(id, std::string(group));
}

static u64 widget_button(PyObject* self, u64 parent, const char* label)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  return state->gui->AddChild(parent, API::Gui::WidgetKind::Button, std::string(label));
}

static u64 widget_slider_float(PyObject* self, u64 parent, const char* label, float min, float max)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  u64 id = state->gui->AddChild(parent, API::Gui::WidgetKind::SliderFloat, std::string(label));
  state->gui->SetSliderRange(id, min, max);
  return id;
}

static u64 widget_text(PyObject* self, u64 parent, const char* text)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  return state->gui->AddChild(parent, API::Gui::WidgetKind::Text, std::string(text));
}

static u64 widget_checkbox(PyObject* self, u64 parent, const char* label, int checked)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  u64 id = state->gui->AddChild(parent, API::Gui::WidgetKind::Checkbox, std::string(label));
  state->gui->SetChecked(id, checked != 0);
  return id;
}

static u64 widget_input_text(PyObject* self, u64 parent, const char* label, const char* initial)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  u64 id = state->gui->AddChild(parent, API::Gui::WidgetKind::InputText, std::string(label));
  state->gui->SetInputText(id, std::string(initial));
  return id;
}

static bool widget_get_checked(PyObject* self, u64 id)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  return state->gui->GetChecked(id);
}

static void widget_set_checked(PyObject* self, u64 id, int checked)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->SetChecked(id, checked != 0);
}

static bool widget_get_visible(PyObject* self, u64 id)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  return state->gui->GetVisible(id);
}

static void widget_set_visible(PyObject* self, u64 id, int visible)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->SetVisible(id, visible != 0);
}

static PyObject* widget_get_input_text(PyObject* self, PyObject* args)
{
  unsigned long long id;
  if (!PyArg_ParseTuple(args, "K", &id))
    return nullptr;
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  return PyUnicode_FromString(state->gui->GetInputText(id).c_str());
}

static void widget_set_input_text(PyObject* self, u64 id, const char* text)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->SetInputText(id, std::string(text));
}

static bool widget_take_clicked(PyObject* self, u64 id)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  return state->gui->TakeClicked(id);
}

static float widget_get_value(PyObject* self, u64 id)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  return state->gui->GetValue(id);
}

static void widget_set_value(PyObject* self, u64 id, float value)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->SetValue(id, value);
}

static void widget_set_text(PyObject* self, u64 id, const char* text)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->SetText(id, std::string(text));
}

static void widget_set_text_color(PyObject* self, u64 id, u32 color)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->SetTextColor(id, color);
}

static void widget_set_bg_color(PyObject* self, u64 id, u32 color)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->SetBgColor(id, color);
}

static void widget_set_style(PyObject* self, u64 id, const char* qss)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->SetStyle(id, std::string(qss));
}

static u64 canvas_window(PyObject* self, const char* title, int width, int height, int embedded,
                         int overlay)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  return state->gui->GetOrCreateCanvas(CurrentOwner(), std::string(title), width, height,
                                       embedded != 0, overlay != 0);
}

static void canvas_clear(PyObject* self, u64 id)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->CanvasClear(id);
}

static void canvas_commit(PyObject* self, u64 id)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->CanvasCommit(id);
}

static void canvas_line(PyObject* self, u64 id, float ax, float ay, float bx, float by, u32 color,
                        float thickness)
{
  using P = API::Gui::CanvasPrimitive;
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->CanvasAdd(id, P{P::Type::Line, {ax, ay}, {bx, by}, {}, 0.0f, thickness, 0.0f, color});
}

static void canvas_rect(PyObject* self, u64 id, float ax, float ay, float bx, float by, u32 color,
                        int filled, float rounding, float thickness)
{
  using P = API::Gui::CanvasPrimitive;
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  const auto type = filled ? P::Type::RectFilled : P::Type::Rect;
  state->gui->CanvasAdd(id, P{type, {ax, ay}, {bx, by}, {}, 0.0f, thickness, rounding, color});
}

static void canvas_circle(PyObject* self, u64 id, float cx, float cy, float radius, u32 color,
                          int filled, float thickness)
{
  using P = API::Gui::CanvasPrimitive;
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  const auto type = filled ? P::Type::CircleFilled : P::Type::Circle;
  state->gui->CanvasAdd(id, P{type, {cx, cy}, {}, {}, radius, thickness, 0.0f, color});
}

static void canvas_triangle(PyObject* self, u64 id, float ax, float ay, float bx, float by,
                            float cx, float cy, u32 color, int filled, float thickness)
{
  using P = API::Gui::CanvasPrimitive;
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  const auto type = filled ? P::Type::TriangleFilled : P::Type::Triangle;
  state->gui->CanvasAdd(id,
                        P{type, {ax, ay}, {bx, by}, {cx, cy}, 0.0f, thickness, 0.0f, color});
}

static void canvas_depth_triangle_filled(PyObject* self, u64 id, float ax, float ay, float az,
                                         float bx, float by, float bz, float cx, float cy, float cz,
                                         u32 color)
{
  using P = API::Gui::CanvasPrimitive;
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  P primitive{P::Type::DepthTriangleFilled, {ax, ay}, {bx, by}, {cx, cy}, 0.0f, 1.0f, 0.0f,
              color};
  primitive.z0 = az;
  primitive.z1 = bz;
  primitive.z2 = cz;
  state->gui->CanvasAdd(id, primitive);
}

static void canvas_depth_triangle_wire(PyObject* self, u64 id, float ax, float ay, float az,
                                       float bx, float by, float bz, float cx, float cy, float cz,
                                       u32 color, float thickness)
{
  using P = API::Gui::CanvasPrimitive;
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  P primitive{P::Type::DepthTriangleWire, {ax, ay}, {bx, by}, {cx, cy}, 0.0f, thickness, 0.0f,
              color};
  primitive.z0 = az;
  primitive.z1 = bz;
  primitive.z2 = cz;
  state->gui->CanvasAdd(id, primitive);
}

static PyObject* canvas_hardware_mesh(PyObject* self, PyObject* args)
{
  u64 id;
  int group;
  PyObject *positions_obj, *colors_obj;
  if (!PyArg_ParseTuple(args, "KiOO", &id, &group, &positions_obj, &colors_obj))
    return nullptr;
  Py_buffer positions{}, colors{};
  if (PyObject_GetBuffer(positions_obj, &positions, PyBUF_CONTIG_RO) != 0)
    return nullptr;
  if (PyObject_GetBuffer(colors_obj, &colors, PyBUF_CONTIG_RO) != 0)
  {
    PyBuffer_Release(&positions);
    return nullptr;
  }
  constexpr Py_ssize_t MAX_HARDWARE_VERTICES = 4'000'000;
  const bool valid = group >= 0 && group < static_cast<int>(API::Gui::HARDWARE_MESH_GROUP_COUNT) &&
                     positions.len % 12 == 0 &&
                     positions.len / 12 <= MAX_HARDWARE_VERTICES &&
                     colors.len == positions.len / 3;
  if (!valid)
  {
    PyBuffer_Release(&positions);
    PyBuffer_Release(&colors);
    PyErr_SetString(PyExc_ValueError,
                    "hardware mesh requires at most 4M xyz float vertices and one u32 color per vertex");
    return nullptr;
  }
  const size_t count = static_cast<size_t>(positions.len) / 12;
  std::vector<API::Gui::HardwareVertex> vertices(count);
  const auto* pos = static_cast<const u8*>(positions.buf);
  const auto* col = static_cast<const u8*>(colors.buf);
  for (size_t i = 0; i < count; ++i)
  {
    std::memcpy(&vertices[i].x, pos + i * 12, 12);
    std::memcpy(&vertices[i].color, col + i * 4, 4);
  }
  PyBuffer_Release(&positions);
  PyBuffer_Release(&colors);
  Py::GetState<GuiModuleState>(self)->gui->SetHardwareMesh(id, static_cast<size_t>(group),
                                                            std::move(vertices));
  Py_RETURN_NONE;
}

static bool GetHudFloat(PyObject* command, Py_ssize_t index, float* value)
{
  const double parsed = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(command, index));
  if (PyErr_Occurred() || !std::isfinite(parsed) ||
      std::abs(parsed) > std::numeric_limits<float>::max())
  {
    if (!PyErr_Occurred())
      PyErr_SetString(PyExc_ValueError, "hardware HUD coordinates must be finite numbers");
    return false;
  }
  *value = static_cast<float>(parsed);
  return true;
}

static bool GetHudNonNegativeFloat(PyObject* command, Py_ssize_t index, float* value)
{
  if (!GetHudFloat(command, index, value))
    return false;
  if (*value < 0.0f)
  {
    PyErr_SetString(PyExc_ValueError, "hardware HUD radii, rounding, and thickness must be non-negative");
    return false;
  }
  return true;
}

static bool GetHudColor(PyObject* command, Py_ssize_t index, u32* color)
{
  const unsigned long parsed = PyLong_AsUnsignedLong(PySequence_Fast_GET_ITEM(command, index));
  if (PyErr_Occurred())
    return false;
  if (parsed > std::numeric_limits<u32>::max())
  {
    PyErr_SetString(PyExc_ValueError, "hardware HUD colors must be ARGB u32 values");
    return false;
  }
  *color = static_cast<u32>(parsed);
  return true;
}

static PyObject* canvas_hardware_hud(PyObject* self, PyObject* args)
{
  u64 id;
  PyObject* sequence;
  if (!PyArg_ParseTuple(args, "KO", &id, &sequence))
    return nullptr;
  PyObject* commands = PySequence_Fast(sequence, "hardware HUD requires a sequence of commands");
  if (!commands)
    return nullptr;

  constexpr Py_ssize_t MAX_HUD_COMMANDS = 16'384;
  const Py_ssize_t count = PySequence_Fast_GET_SIZE(commands);
  if (count > MAX_HUD_COMMANDS)
  {
    Py_DECREF(commands);
    PyErr_SetString(PyExc_ValueError, "hardware HUD supports at most 16384 commands");
    return nullptr;
  }

  using P = API::Gui::CanvasPrimitive;
  std::vector<P> primitives;
  primitives.reserve(static_cast<size_t>(count));
  for (Py_ssize_t i = 0; i < count; ++i)
  {
    PyObject* command = PySequence_Fast(PySequence_Fast_GET_ITEM(commands, i),
                                        "each hardware HUD command must be a sequence");
    if (!command)
      break;
    const Py_ssize_t size = PySequence_Fast_GET_SIZE(command);
    const char* kind = size > 0 ? PyUnicode_AsUTF8(PySequence_Fast_GET_ITEM(command, 0)) : nullptr;
    P primitive{};
    bool valid = kind != nullptr;
    const auto point = [&](Py_ssize_t index, Vec2f* out) {
      return GetHudFloat(command, index, &out->x) && GetHudFloat(command, index + 1, &out->y);
    };
    if (valid && std::strcmp(kind, "line") == 0 && size == 7)
    {
      primitive.type = P::Type::Line;
      valid = point(1, &primitive.p0) && point(3, &primitive.p1) && GetHudColor(command, 5, &primitive.color) &&
              GetHudNonNegativeFloat(command, 6, &primitive.thickness);
    }
    else if (valid && std::strcmp(kind, "rect") == 0 && size == 8)
    {
      primitive.type = P::Type::Rect;
      valid = point(1, &primitive.p0) && point(3, &primitive.p1) && GetHudColor(command, 5, &primitive.color) &&
              GetHudNonNegativeFloat(command, 6, &primitive.rounding) &&
              GetHudNonNegativeFloat(command, 7, &primitive.thickness);
    }
    else if (valid && std::strcmp(kind, "rect_filled") == 0 && size == 7)
    {
      primitive.type = P::Type::RectFilled;
      valid = point(1, &primitive.p0) && point(3, &primitive.p1) && GetHudColor(command, 5, &primitive.color) &&
              GetHudNonNegativeFloat(command, 6, &primitive.rounding);
    }
    else if (valid && std::strcmp(kind, "circle") == 0 && size == 6)
    {
      primitive.type = P::Type::Circle;
      valid = point(1, &primitive.p0) && GetHudNonNegativeFloat(command, 3, &primitive.radius) &&
              GetHudColor(command, 4, &primitive.color) &&
              GetHudNonNegativeFloat(command, 5, &primitive.thickness);
    }
    else if (valid && std::strcmp(kind, "circle_filled") == 0 && size == 5)
    {
      primitive.type = P::Type::CircleFilled;
      valid = point(1, &primitive.p0) && GetHudNonNegativeFloat(command, 3, &primitive.radius) &&
              GetHudColor(command, 4, &primitive.color);
    }
    else if (valid && std::strcmp(kind, "triangle") == 0 && size == 9)
    {
      primitive.type = P::Type::Triangle;
      valid = point(1, &primitive.p0) && point(3, &primitive.p1) && point(5, &primitive.p2) &&
              GetHudColor(command, 7, &primitive.color) &&
              GetHudNonNegativeFloat(command, 8, &primitive.thickness);
    }
    else if (valid && std::strcmp(kind, "triangle_filled") == 0 && size == 8)
    {
      primitive.type = P::Type::TriangleFilled;
      valid = point(1, &primitive.p0) && point(3, &primitive.p1) && point(5, &primitive.p2) &&
              GetHudColor(command, 7, &primitive.color);
    }
    else if (valid && std::strcmp(kind, "text") == 0 && size == 5)
    {
      primitive.type = P::Type::Text;
      const char* text = PyUnicode_AsUTF8(PySequence_Fast_GET_ITEM(command, 4));
      valid = point(1, &primitive.p0) && GetHudColor(command, 3, &primitive.color) && text != nullptr;
      if (valid)
      {
        constexpr size_t MAX_HUD_TEXT_BYTES = 16'384;
        if (std::strlen(text) > MAX_HUD_TEXT_BYTES)
        {
          PyErr_SetString(PyExc_ValueError, "hardware HUD text is limited to 16384 UTF-8 bytes per command");
          valid = false;
        }
        primitive.text = text;
      }
    }
    else
    {
      valid = false;
      PyErr_Format(PyExc_ValueError, "invalid hardware HUD command at index %zd", i);
    }
    Py_DECREF(command);
    if (!valid)
      break;
    primitives.push_back(std::move(primitive));
  }
  Py_DECREF(commands);
  if (PyErr_Occurred())
    return nullptr;
  Py::GetState<GuiModuleState>(self)->gui->SetHardwareHud(id, std::move(primitives));
  Py_RETURN_NONE;
}

static void canvas_hardware_state(PyObject* self, u64 id, float ex, float ey, float ez, float rx,
                                  float ry, float rz, float ux, float uy, float uz, float fx,
                                  float fy, float fz, float focal, float radius, float fill,
                                  float wire, int filled, int wireframe, int enabled, int xray,
                                  int debug_on_top, int fullscreen, int clean_capture, int hud_visible)
{
  API::Gui::HardwareState state{};
  state.enabled = enabled != 0;
  state.eye = {ex, ey, ez}; state.right = {rx, ry, rz}; state.up = {ux, uy, uz};
  state.forward = {fx, fy, fz}; state.focal = focal; state.radius = radius;
  state.fill_opacity = fill; state.wire_opacity = wire;
  state.filled = filled != 0; state.wireframe = wireframe != 0;
  state.xray = xray != 0; state.debug_on_top = debug_on_top != 0; state.fullscreen = fullscreen != 0;
  state.clean_capture = clean_capture != 0;
  state.hud_visible = hud_visible != 0;
  Py::GetState<GuiModuleState>(self)->gui->SetHardwareState(id, state);
}

static void canvas_text(PyObject* self, u64 id, float x, float y, u32 color, const char* text)
{
  using P = API::Gui::CanvasPrimitive;
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->CanvasAdd(id, P{P::Type::Text, {x, y}, {}, {}, 0.0f, 1.0f, 0.0f, color,
                              std::string(text)});
}

static void canvas_image(PyObject* self, u64 id, const char* path, float x, float y, float w,
                         float h, u32 tint, float sx0, float sy0, float sx1, float sy1)
{
  using P = API::Gui::CanvasPrimitive;
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  state->gui->CanvasAdd(id, P{P::Type::Image, {x, y}, {x + w, y + h}, {}, 0.0f, 1.0f, 0.0f, tint,
                              std::string{}, std::string(path), {sx0, sy0}, {sx1, sy1}});
}

// Returns (x, y, inside) for the canvas cursor; coords are canvas pixels.
static PyObject* canvas_mouse_pos(PyObject* self, PyObject* args)
{
  u64 id;
  if (!PyArg_ParseTuple(args, "K", &id))
    return nullptr;
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  bool inside = false;
  const auto pos = state->gui->CanvasMousePos(id, inside);
  return Py_BuildValue("(ffO)", pos.x, pos.y, inside ? Py_True : Py_False);
}

// Returns (x, y) of the last unconsumed left-click, or None. Consumes it.
static PyObject* canvas_take_click(PyObject* self, PyObject* args)
{
  u64 id;
  if (!PyArg_ParseTuple(args, "K", &id))
    return nullptr;
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  Vec2f pos;
  if (!state->gui->CanvasTakeClick(id, pos))
    Py_RETURN_NONE;
  return Py_BuildValue("(ff)", pos.x, pos.y);
}

// Returns (x, y) of the last unconsumed right-click, or None. Consumes it.
static PyObject* canvas_take_right_click(PyObject* self, PyObject* args)
{
  u64 id;
  if (!PyArg_ParseTuple(args, "K", &id))
    return nullptr;
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  Vec2f pos;
  if (!state->gui->CanvasTakeRightClick(id, pos))
    Py_RETURN_NONE;
  return Py_BuildValue("(ff)", pos.x, pos.y);
}

static PyObject* canvas_take_capture_toggle(PyObject* self, PyObject* args)
{
  u64 id;
  if (!PyArg_ParseTuple(args, "K", &id))
    return nullptr;
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  return PyBool_FromLong(state->gui->CanvasTakeCaptureToggle(id));
}

static PyObject* canvas_right_down(PyObject* self, PyObject* args)
{
  u64 id;
  if (!PyArg_ParseTuple(args, "K", &id))
    return nullptr;
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  return PyBool_FromLong(state->gui->CanvasRightDown(id));
}

static PyObject* canvas_left_down(PyObject* self, PyObject* args)
{
  u64 id;
  if (!PyArg_ParseTuple(args, "K", &id))
    return nullptr;
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  return PyBool_FromLong(state->gui->CanvasLeftDown(id));
}

static PyObject* canvas_key_mask(PyObject* self, PyObject* args)
{
  u64 id;
  if (!PyArg_ParseTuple(args, "K", &id))
    return nullptr;
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  return PyLong_FromUnsignedLong(state->gui->CanvasKeyMask(id));
}

// Returns current canvas size as (width, height) in pixels.
static PyObject* canvas_size(PyObject* self, PyObject* args)
{
  u64 id;
  if (!PyArg_ParseTuple(args, "K", &id))
    return nullptr;
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  const auto [w, h] = state->gui->CanvasSize(id);
  return Py_BuildValue("(ii)", w, h);
}

// Returns accumulated wheel notches since last call (positive = scroll up). Consumes it.
static float canvas_take_wheel(PyObject* self, u64 id)
{
  GuiModuleState* state = Py::GetState<GuiModuleState>(self);
  return state->gui->CanvasTakeWheel(id);
}

static void SetupGuiModule(PyObject* module, GuiModuleState* state)
{
  static const char pycode[] = R"(
def add_osd_message(message: str, duration_ms: int = 2000, color_argb: int = 0xFFFFFF30):
    return _add_osd_message(message, duration_ms, color_argb)

def set_clipboard(text: str):
    _set_clipboard(text)

def draw_line(a, b, color, thickness = 1):
    _draw_line(a[0], a[1], b[0], b[1], color, thickness)

def draw_rect(a, b, color, rounding = 0, thickness = 1):
    _draw_rect(a[0], a[1], b[0], b[1], color, rounding, thickness)

def draw_rect_filled(a, b, color, rounding= 0):
    _draw_rect_filled(a[0], a[1], b[0], b[1], color, rounding)

def draw_quad(a, b, c, d, color, thickness = 1):
    _draw_quad(a[0], a[1], b[0], b[1], c[0], c[1], d[0], d[1], color, thickness)

def draw_quad_filled(a, b, c, d, color):
    _draw_quad_filled(a[0], a[1], b[0], b[1], c[0], c[1], d[0], d[1], color)

def draw_triangle(a, b, c, color, thickness = 1):
    _draw_triangle(a[0], a[1], b[0], b[1], c[0], c[1], color, thickness)

def draw_triangle_filled(a, b, c, color):
    _draw_triangle_filled(a[0], a[1], b[0], b[1], c[0], c[1], color)

def draw_circle(center, radius, color, num_segments = None, thickness = 1):
    if num_segments is None:
        num_segments = 8 + int(radius // 50)
    _draw_circle(center[0], center[1], radius, color, num_segments, thickness)

def draw_circle_filled(center, radius, color, num_segments = None):
    if num_segments is None:
        num_segments = 8 + int(radius // 50)
    _draw_circle_filled(center[0], center[1], radius, color, num_segments)

def draw_text(pos, color, text):
    _draw_text(pos[0], pos[1], color, text)

def draw_polyline(points, color, closed = False, thickness = 1):
    _draw_polyline(points, color, closed, thickness)

def draw_convex_poly_filled(points, color):
    _draw_convex_poly_filled(points, color)

class _Widget:
    def __init__(self, id):
        self._id = id
    @property
    def visible(self):
        return _widget_get_visible(self._id)
    @visible.setter
    def visible(self, v):
        _widget_set_visible(self._id, int(v))

class Button(_Widget):
    @property
    def clicked(self):
        return _widget_take_clicked(self._id)

class SliderFloat(_Widget):
    @property
    def value(self):
        return _widget_get_value(self._id)
    @value.setter
    def value(self, v):
        _widget_set_value(self._id, v)

class Text(_Widget):
    def set(self, text):
        _widget_set_text(self._id, text)

class Checkbox(_Widget):
    @property
    def checked(self):
        return _widget_get_checked(self._id)
    @checked.setter
    def checked(self, v):
        _widget_set_checked(self._id, int(v))

class InputText(_Widget):
    @property
    def value(self):
        return _widget_get_input_text(self._id)
    @value.setter
    def value(self, v):
        _widget_set_input_text(self._id, v)

class _BaseWindow:
    def __init__(self, wid):
        self._id = wid
    def canvas(self, width, height):
        # Attach a drawing surface to this window; form widgets added here sit below it.
        _widget_enable_canvas(self._id, width, height)
        return Canvas(self._id, width, height)
    def enable_hardware_canvas(self):
        # Request the native hardware canvas. CPU canvas windows remain the default.
        _widget_enable_hardware_canvas(self._id)
    def _child(self, wid, style, text_color, bg_color, group=None):
        if text_color is not None:
            _widget_set_text_color(wid, text_color)
        if bg_color is not None:
            _widget_set_bg_color(wid, bg_color)
        if style is not None:
            _widget_set_style(wid, style)
        if group is not None:
            _widget_set_group(wid, group)
        return wid

class Overlay(_BaseWindow):
    def button(self, label, *, text_color=None, bg_color=None):
        return Button(self._child(_widget_button(self._id, label), None, text_color, bg_color))
    def slider_float(self, label, min = 0.0, max = 1.0, *, text_color=None, bg_color=None):
        return SliderFloat(self._child(_widget_slider_float(self._id, label, min, max),
                                       None, text_color, bg_color))
    def text(self, text = "", *, text_color=None, bg_color=None):
        return Text(self._child(_widget_text(self._id, text), None, text_color, bg_color))
    def checkbox(self, label, checked = False, *, text_color=None, bg_color=None):
        return Checkbox(self._child(_widget_checkbox(self._id, label, int(checked)),
                                    None, text_color, bg_color))
    def input_text(self, label, initial = "", *, text_color=None, bg_color=None):
        return InputText(self._child(_widget_input_text(self._id, label, initial),
                                     None, text_color, bg_color))

class Window(_BaseWindow):
    def button(self, label, *, style=None, text_color=None, bg_color=None, group=None):
        return Button(self._child(_widget_button(self._id, label), style, text_color, bg_color, group))
    def slider_float(self, label, min = 0.0, max = 1.0, *, style=None, text_color=None, bg_color=None, group=None):
        return SliderFloat(self._child(_widget_slider_float(self._id, label, min, max),
                                       style, text_color, bg_color, group))
    def text(self, text = "", *, style=None, text_color=None, bg_color=None, group=None):
        return Text(self._child(_widget_text(self._id, text), style, text_color, bg_color, group))
    def checkbox(self, label, checked = False, *, style=None, text_color=None, bg_color=None, group=None):
        return Checkbox(self._child(_widget_checkbox(self._id, label, int(checked)),
                                    style, text_color, bg_color, group))
    def input_text(self, label, initial = "", *, style=None, text_color=None, bg_color=None, group=None):
        return InputText(self._child(_widget_input_text(self._id, label, initial),
                                     style, text_color, bg_color, group))

class HardwareHud:
    """Retained screen-space draw list for a hardware canvas.

    HUD coordinates are logical canvas pixels. Commands are composited after
    the 3D mesh and preserve their Python order, which makes the class useful
    for custom panels as well as simple annotations.
    """
    def __init__(self, canvas_id):
        self._id = canvas_id
        self._commands = []
    def line(self, a, b, color, thickness = 1):
        self._commands.append(("line", a[0], a[1], b[0], b[1], color, thickness)); return self
    def rect(self, a, b, color, rounding = 0, thickness = 1):
        self._commands.append(("rect", a[0], a[1], b[0], b[1], color, rounding, thickness)); return self
    def rect_filled(self, a, b, color, rounding = 0):
        self._commands.append(("rect_filled", a[0], a[1], b[0], b[1], color, rounding)); return self
    def circle(self, center, radius, color, thickness = 1):
        self._commands.append(("circle", center[0], center[1], radius, color, thickness)); return self
    def circle_filled(self, center, radius, color):
        self._commands.append(("circle_filled", center[0], center[1], radius, color)); return self
    def triangle(self, a, b, c, color, thickness = 1):
        self._commands.append(("triangle", a[0], a[1], b[0], b[1], c[0], c[1], color, thickness)); return self
    def triangle_filled(self, a, b, c, color):
        self._commands.append(("triangle_filled", a[0], a[1], b[0], b[1], c[0], c[1], color)); return self
    def text(self, pos, color, text):
        self._commands.append(("text", pos[0], pos[1], color, str(text))); return self
    def commit(self):
        _canvas_hardware_hud(self._id, tuple(self._commands))

class Canvas:
    def __init__(self, id, width, height):
        self._id = id
    @property
    def width(self):
        return _canvas_size(self._id)[0]
    @property
    def height(self):
        return _canvas_size(self._id)[1]
    def clear(self):
        _canvas_clear(self._id)
    def commit(self):
        _canvas_commit(self._id)
    def line(self, a, b, color, thickness = 1):
        _canvas_line(self._id, a[0], a[1], b[0], b[1], color, thickness)
    def rect(self, a, b, color, rounding = 0, thickness = 1):
        _canvas_rect(self._id, a[0], a[1], b[0], b[1], color, 0, rounding, thickness)
    def rect_filled(self, a, b, color, rounding = 0):
        _canvas_rect(self._id, a[0], a[1], b[0], b[1], color, 1, rounding, 1)
    def circle(self, center, radius, color, thickness = 1):
        _canvas_circle(self._id, center[0], center[1], radius, color, 0, thickness)
    def circle_filled(self, center, radius, color):
        _canvas_circle(self._id, center[0], center[1], radius, color, 1, 1)
    def triangle(self, a, b, c, color, thickness = 1):
        _canvas_triangle(self._id, a[0], a[1], b[0], b[1], c[0], c[1], color, 0, thickness)
    def triangle_filled(self, a, b, c, color):
        _canvas_triangle(self._id, a[0], a[1], b[0], b[1], c[0], c[1], color, 1, 1)
    def depth_triangle_filled(self, a, b, c, depths, color):
        _canvas_depth_triangle_filled(self._id, a[0], a[1], depths[0], b[0], b[1], depths[1],
                                      c[0], c[1], depths[2], color)
    def depth_triangle_wire(self, a, b, c, depths, color, thickness = 1):
        _canvas_depth_triangle_wire(self._id, a[0], a[1], depths[0], b[0], b[1], depths[1],
                                    c[0], c[1], depths[2], color, thickness)
    def hardware_mesh(self, group, positions, colors):
        # positions: packed xyz float32 bytes; colors: packed ARGB u32 bytes.
        _canvas_hardware_mesh(self._id, group, positions, colors)
    def hud(self):
        return HardwareHud(self._id)
    def hardware_hud(self, commands):
        """Commit a sequence produced by HardwareHud, or compatible commands."""
        _canvas_hardware_hud(self._id, tuple(commands))
    def hardware_overlay(self, lines):
        # Compatibility shim for older scripts. New scripts should own their
        # layout through hud(), rather than relying on a renderer-made panel.
        hud = self.hud()
        for i, line in enumerate(lines):
            hud.text((12, 12 + i * 18), 0xFFE3E8F0, line)
        hud.commit()
    def hardware_state(self, eye, right, up, forward, focal, radius, fill_opacity, wire_opacity,
                       filled=True, wireframe=True, enabled=True, xray=False, debug_on_top=False,
                       fullscreen=False, clean_capture=False, hud_visible=None):
        if hud_visible is None:
            hud_visible = not clean_capture
        _canvas_hardware_state(self._id, eye[0], eye[1], eye[2], right[0], right[1], right[2],
                               up[0], up[1], up[2], forward[0], forward[1], forward[2], focal,
                               radius, fill_opacity, wire_opacity, int(filled), int(wireframe),
                               int(enabled), int(xray), int(debug_on_top), int(fullscreen),
                               int(clean_capture), int(hud_visible))
    def text(self, pos, color, text):
        _canvas_text(self._id, pos[0], pos[1], color, text)
    def image(self, path, pos, size, *, tint=0, src=(0.0, 0.0, 1.0, 1.0)):
        # tint=0 draws the texture as-is; otherwise RGB tints it and alpha scales opacity.
        # src is a normalized (x0, y0, x1, y1) crop for partial draws (e.g. analog fills).
        _canvas_image(self._id, path, pos[0], pos[1], size[0], size[1], tint,
                      src[0], src[1], src[2], src[3])
    def mouse_pos(self):
        # (x, y, inside): last cursor position in canvas pixels; inside is False off-widget.
        return _canvas_mouse_pos(self._id)
    def take_click(self):
        # (x, y) of the last unconsumed left-click in canvas pixels, or None. Consumes it.
        return _canvas_take_click(self._id)
    def take_right_click(self):
        # (x, y) of the last unconsumed right-click in canvas pixels, or None. Consumes it.
        return _canvas_take_right_click(self._id)
    def take_capture_toggle(self):
        return _canvas_take_capture_toggle(self._id)
    def right_down(self):
        # True while the canvas holds the right mouse button.
        return _canvas_right_down(self._id)
    def left_down(self):
        # True while the canvas holds the left mouse button.
        return _canvas_left_down(self._id)
    def key_mask(self):
        # Held W/A/S/D/Space/Shift bits for focused script canvases.
        return _canvas_key_mask(self._id)
    def take_wheel(self):
        # Accumulated wheel notches since the last call (positive = scroll up). Consumes it.
        return _canvas_take_wheel(self._id)

def canvas(title, width, height, *, embedded=False, overlay=False):
    return Canvas(_canvas_window(title, width, height, int(embedded), int(overlay)), width, height)

def overlay(title, *, bg_color=None, text_color=None):
    w = Overlay(_widget_window(title, 1))
    if bg_color is not None:
        _widget_set_bg_color(w._id, bg_color)
    if text_color is not None:
        _widget_set_text_color(w._id, text_color)
    return w

def window(title, *, style=None, bg_color=None, text_color=None):
    w = Window(_widget_window(title, 0))
    if bg_color is not None:
        _widget_set_bg_color(w._id, bg_color)
    if text_color is not None:
        _widget_set_text_color(w._id, text_color)
    if style is not None:
        _widget_set_style(w._id, style)
    return w
)";
  Py::Object result = Py::LoadPyCodeIntoModule(module, pycode);
  if (result.IsNull())
  {
    ERROR_LOG_FMT(SCRIPTING, "Failed to load embedded python code into gui module");
  }
  API::Gui* gui = PyScripting::PyScriptingBackend::GetCurrent()->GetGui();
  state->gui = gui;
}

PyMODINIT_FUNC PyInit_gui()
{
  static PyMethodDef methods[] = {
      {"_add_osd_message", Py::as_py_func<add_osd_message>, METH_VARARGS, ""},
      {"clear_osd_messages", Py::as_py_func<clear_osd_messages>, METH_VARARGS, ""},
      {"_set_clipboard", Py::as_py_func<set_clipboard>, METH_VARARGS, ""},
      {"get_display_size", get_display_size, METH_NOARGS, ""},
      {"_draw_line", Py::as_py_func<draw_line>, METH_VARARGS, ""},
      {"_draw_rect", Py::as_py_func<draw_rect>, METH_VARARGS, ""},
      {"_draw_rect_filled", Py::as_py_func<draw_rect_filled>, METH_VARARGS, ""},
      {"_draw_quad", Py::as_py_func<draw_quad>, METH_VARARGS, ""},
      {"_draw_quad_filled", Py::as_py_func<draw_quad_filled>, METH_VARARGS, ""},
      {"_draw_triangle", Py::as_py_func<draw_triangle>, METH_VARARGS, ""},
      {"_draw_triangle_filled", Py::as_py_func<draw_triangle_filled>, METH_VARARGS, ""},
      {"_draw_circle", Py::as_py_func<draw_circle>, METH_VARARGS, ""},
      {"_draw_circle_filled", Py::as_py_func<draw_circle_filled>, METH_VARARGS, ""},
      {"_draw_text", Py::as_py_func<draw_text>, METH_VARARGS, ""},
      {"_draw_polyline", draw_polyline, METH_VARARGS, ""},
      {"_draw_convex_poly_filled", draw_convex_poly_filled, METH_VARARGS, ""},
      {"_widget_window", Py::as_py_func<widget_window>, METH_VARARGS, ""},
      {"_widget_enable_canvas", Py::as_py_func<widget_enable_canvas>, METH_VARARGS, ""},
      {"_widget_enable_hardware_canvas", Py::as_py_func<widget_enable_hardware_canvas>, METH_VARARGS, ""},
      {"_widget_set_group", Py::as_py_func<widget_set_group>, METH_VARARGS, ""},
      {"_widget_button", Py::as_py_func<widget_button>, METH_VARARGS, ""},
      {"_widget_slider_float", Py::as_py_func<widget_slider_float>, METH_VARARGS, ""},
      {"_widget_text", Py::as_py_func<widget_text>, METH_VARARGS, ""},
      {"_widget_checkbox", Py::as_py_func<widget_checkbox>, METH_VARARGS, ""},
      {"_widget_input_text", Py::as_py_func<widget_input_text>, METH_VARARGS, ""},
      {"_widget_get_checked", Py::as_py_func<widget_get_checked>, METH_VARARGS, ""},
      {"_widget_set_checked", Py::as_py_func<widget_set_checked>, METH_VARARGS, ""},
      {"_widget_get_visible", Py::as_py_func<widget_get_visible>, METH_VARARGS, ""},
      {"_widget_set_visible", Py::as_py_func<widget_set_visible>, METH_VARARGS, ""},
      {"_widget_get_input_text", widget_get_input_text, METH_VARARGS, ""},
      {"_widget_set_input_text", Py::as_py_func<widget_set_input_text>, METH_VARARGS, ""},
      {"_widget_take_clicked", Py::as_py_func<widget_take_clicked>, METH_VARARGS, ""},
      {"_widget_get_value", Py::as_py_func<widget_get_value>, METH_VARARGS, ""},
      {"_widget_set_value", Py::as_py_func<widget_set_value>, METH_VARARGS, ""},
      {"_widget_set_text", Py::as_py_func<widget_set_text>, METH_VARARGS, ""},
      {"_widget_set_text_color", Py::as_py_func<widget_set_text_color>, METH_VARARGS, ""},
      {"_widget_set_bg_color", Py::as_py_func<widget_set_bg_color>, METH_VARARGS, ""},
      {"_widget_set_style", Py::as_py_func<widget_set_style>, METH_VARARGS, ""},
      {"_canvas_window", Py::as_py_func<canvas_window>, METH_VARARGS, ""},
      {"_canvas_clear", Py::as_py_func<canvas_clear>, METH_VARARGS, ""},
      {"_canvas_commit", Py::as_py_func<canvas_commit>, METH_VARARGS, ""},
      {"_canvas_line", Py::as_py_func<canvas_line>, METH_VARARGS, ""},
      {"_canvas_rect", Py::as_py_func<canvas_rect>, METH_VARARGS, ""},
      {"_canvas_circle", Py::as_py_func<canvas_circle>, METH_VARARGS, ""},
      {"_canvas_triangle", Py::as_py_func<canvas_triangle>, METH_VARARGS, ""},
      {"_canvas_depth_triangle_filled", Py::as_py_func<canvas_depth_triangle_filled>, METH_VARARGS,
       ""},
      {"_canvas_depth_triangle_wire", Py::as_py_func<canvas_depth_triangle_wire>, METH_VARARGS,
       ""},
      {"_canvas_hardware_mesh", canvas_hardware_mesh, METH_VARARGS, ""},
      {"_canvas_hardware_hud", canvas_hardware_hud, METH_VARARGS, ""},
      {"_canvas_hardware_state", Py::as_py_func<canvas_hardware_state>, METH_VARARGS, ""},
      {"_canvas_text", Py::as_py_func<canvas_text>, METH_VARARGS, ""},
      {"_canvas_image", Py::as_py_func<canvas_image>, METH_VARARGS, ""},
      {"_canvas_size", canvas_size, METH_VARARGS, ""},
      {"_canvas_mouse_pos", canvas_mouse_pos, METH_VARARGS, ""},
      {"_canvas_take_click", canvas_take_click, METH_VARARGS, ""},
      {"_canvas_take_right_click", canvas_take_right_click, METH_VARARGS, ""},
      {"_canvas_take_capture_toggle", canvas_take_capture_toggle, METH_VARARGS, ""},
      {"_canvas_right_down", canvas_right_down, METH_VARARGS, ""},
      {"_canvas_left_down", canvas_left_down, METH_VARARGS, ""},
      {"_canvas_key_mask", canvas_key_mask, METH_VARARGS, ""},
      {"_canvas_take_wheel", Py::as_py_func<canvas_take_wheel>, METH_VARARGS, ""},

      {nullptr, nullptr, 0, nullptr}  // Sentinel
  };
  static PyModuleDef module_def =
      Py::MakeStatefulModuleDef<GuiModuleState, SetupGuiModule>("gui", methods);
  PyObject* def_obj = PyModuleDef_Init(&module_def);
  return def_obj;
}

}  // namespace PyScripting
