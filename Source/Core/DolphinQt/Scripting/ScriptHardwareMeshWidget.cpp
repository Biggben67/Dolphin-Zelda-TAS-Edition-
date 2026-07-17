// Copyright 2026 Dolphin Emulator Project
// Licensed under GPLv2+

#include "DolphinQt/Scripting/ScriptHardwareMeshWidget.h"

#include <QPaintEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QString>
#include <QWheelEvent>

#ifdef _WIN32
#include <d2d1.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dwrite.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <array>
#include <cstring>
#include <numeric>
#include <string>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;

namespace
{
constexpr float NEAR_PLANE = 5.0f;
constexpr float FAR_PLANE = 100000.0f;

struct Constants
{
  float eye_radius[4];
  float right_focal[4];
  float up_fill[4];
  float forward_wire[4];
  float viewport_flags[4];
};

constexpr char VERTEX_SHADER[] = R"(
cbuffer Camera : register(b0) {
  float4 eye_radius;
  float4 right_focal;
  float4 up_fill;
  float4 forward_wire;
  float4 viewport_flags;
};
struct VSIn { float3 position : POSITION; float4 color : COLOR; };
struct VSOut { float4 position : SV_POSITION; float3 world : WORLD; float4 color : COLOR; float depth : TEXCOORD0; float3 bary : BARY; };
VSOut main(VSIn input, uint vertex_id : SV_VertexID) {
  VSOut output;
  float3 relative = input.position - eye_radius.xyz;
  float x = dot(relative, right_focal.xyz);
  float y = dot(relative, up_fill.xyz);
  float z = dot(relative, forward_wire.xyz);
  float perspective_z = z * (100000.0 / 99995.0) - (500000.0 / 99995.0);
  output.position = float4(right_focal.w * 2.0 * x / viewport_flags.x,
                           right_focal.w * 2.0 * y / viewport_flags.y,
                           perspective_z, z);
  output.world = input.position;
  output.color = input.color;
  output.depth = z;
  uint corner = vertex_id % 3;
  output.bary = corner == 0 ? float3(1, 0, 0) : (corner == 1 ? float3(0, 1, 0) : float3(0, 0, 1));
  return output;
}
)";

constexpr char PIXEL_SHADER[] = R"(
cbuffer Camera : register(b0) {
  float4 eye_radius;
  float4 right_focal;
  float4 up_fill;
  float4 forward_wire;
  float4 viewport_flags;
};
struct PSIn { float4 position : SV_POSITION; float3 world : WORLD; float4 color : COLOR; float depth : TEXCOORD0; float3 bary : BARY; };
float4 main(PSIn input) : SV_TARGET {
  clip(input.depth - 5.0);
  if (eye_radius.w > 0.0 && distance(input.world, eye_radius.xyz) > eye_radius.w)
    discard;
  float alpha = input.color.a * viewport_flags.z;
  if (viewport_flags.w > 1.5)
    return float4(input.color.rgb, alpha);
  if (viewport_flags.w > 0.5) {
    float edge_distance = min(input.bary.x, min(input.bary.y, input.bary.z));
    float edge_width = max(fwidth(edge_distance) * 2.5, 0.00001);
    alpha *= 1.0 - smoothstep(0.0, edge_width, edge_distance);
    clip(alpha - 0.003);
    return float4(1.0, 1.0, 1.0, alpha);
  }
  return float4(input.color.rgb, alpha);
}
)";

constexpr char LINE_GEOMETRY_SHADER[] = R"(
cbuffer Camera : register(b0) {
  float4 eye_radius;
  float4 right_focal;
  float4 up_fill;
  float4 forward_wire;
  float4 viewport_flags;
};
struct VSOut { float4 position : SV_POSITION; float3 world : WORLD; float4 color : COLOR; float depth : TEXCOORD0; float3 bary : BARY; };
struct GSOut { float4 position : SV_POSITION; float3 world : WORLD; float4 color : COLOR; float depth : TEXCOORD0; float3 bary : BARY; };
[maxvertexcount(4)]
void main(line VSOut input[2], inout TriangleStream<GSOut> stream) {
  float2 p0 = input[0].position.xy / input[0].position.w;
  float2 p1 = input[1].position.xy / input[1].position.w;
  float2 delta = (p1 - p0) * float2(viewport_flags.x, viewport_flags.y);
  float length_pixels = max(length(delta), 0.001);
  // Expand every line consistently in screen space. Mesh groups deliberately
  // carry no tool-specific color or feature semantics.
  const float half_width = 2.0;
  const float2 offset = float2(-delta.y, delta.x) / length_pixels * half_width /
                        float2(viewport_flags.x, viewport_flags.y);
  GSOut output;
  output.bary = float3(0, 0, 0);
  output.world = input[0].world; output.color = input[0].color; output.depth = input[0].depth;
  output.position = float4((p0 + offset) * input[0].position.w, input[0].position.z, input[0].position.w);
  stream.Append(output);
  output.position = float4((p0 - offset) * input[0].position.w, input[0].position.z, input[0].position.w);
  stream.Append(output);
  output.world = input[1].world; output.color = input[1].color; output.depth = input[1].depth;
  output.position = float4((p1 + offset) * input[1].position.w, input[1].position.z, input[1].position.w);
  stream.Append(output);
  output.position = float4((p1 - offset) * input[1].position.w, input[1].position.z, input[1].position.w);
  stream.Append(output);
}
)";
}  // namespace

struct ScriptHardwareMeshWidget::Resources
{
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<IDXGISwapChain> swap_chain;
  ComPtr<ID3D11Texture2D> backbuffer;
  ComPtr<ID3D11Texture2D> capture_texture;
  QSize capture_size;
  ComPtr<ID3D11RenderTargetView> target;
  ComPtr<ID3D11Texture2D> depth_texture;
  ComPtr<ID3D11DepthStencilView> depth;
  // Render into this multisampled target when the GPU supports it, then
  // resolve once into the swap-chain backbuffer. This retains the fast bulk
  // path while substantially improving diagonal collision/wire edges.
  ComPtr<ID3D11Texture2D> msaa_color;
  ComPtr<ID3D11RenderTargetView> msaa_target;
  ComPtr<ID3D11Texture2D> msaa_depth_texture;
  ComPtr<ID3D11DepthStencilView> msaa_depth;
  ComPtr<ID3D11VertexShader> vertex_shader;
  ComPtr<ID3D11PixelShader> pixel_shader;
  ComPtr<ID3D11GeometryShader> line_geometry_shader;
  ComPtr<ID3D11InputLayout> input_layout;
  ComPtr<ID3D11Buffer> constants;
  std::array<ComPtr<ID3D11Buffer>, 8> vertex_buffers;
  std::array<UINT, 8> vertex_counts{};
  ComPtr<ID3D11RasterizerState> solid_rasterizer;
  ComPtr<ID3D11RasterizerState> wire_rasterizer;
  ComPtr<ID3D11DepthStencilState> depth_write;
  ComPtr<ID3D11DepthStencilState> depth_read;
  ComPtr<ID3D11DepthStencilState> depth_off;
  ComPtr<ID3D11BlendState> blend;
  ComPtr<ID3D11BlendState> no_color_blend;
  ComPtr<ID2D1Factory> d2d_factory;
  ComPtr<IDWriteFactory> dwrite_factory;
  ComPtr<IDWriteTextFormat> hud_format;
  ComPtr<ID2D1RenderTarget> hud_target;
  ComPtr<ID2D1SolidColorBrush> hud_brush;
};

namespace
{
u32 CanvasKeyBit(int key)
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
}  // namespace

ScriptHardwareMeshWidget::ScriptHardwareMeshWidget(API::Gui::WidgetId id, QWidget* parent)
    : QWidget(parent), m_id(id)
{
  setAttribute(Qt::WA_NativeWindow);
  // This widget presents a D3D swap chain directly to its native HWND. Do
  // not let Qt's backing-store paint path clear that HWND during a pause,
  // focus, or expose transition between script frame callbacks.
  setAttribute(Qt::WA_PaintOnScreen);
  setAttribute(Qt::WA_OpaquePaintEvent);
  setAttribute(Qt::WA_NoSystemBackground);
  setUpdatesEnabled(true);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
}

ScriptHardwareMeshWidget::~ScriptHardwareMeshWidget()
{
  ReleaseResources();
}

void ScriptHardwareMeshWidget::SetSnapshot(API::Gui::HardwareSnapshot snapshot)
{
  if (m_snapshot.generation == snapshot.generation)
    return;
  for (size_t i = 0; i < m_uploaded_groups.size(); ++i)
  {
    if (m_snapshot.groups[i] != snapshot.groups[i])
      m_mesh_dirty = true;
  }
  m_snapshot = std::move(snapshot);
  update();
}

QPaintEngine* ScriptHardwareMeshWidget::paintEngine() const
{
  return nullptr;
}

void ScriptHardwareMeshWidget::paintEvent(QPaintEvent*)
{
  Render();
}

void ScriptHardwareMeshWidget::resizeEvent(QResizeEvent* event)
{
  QWidget::resizeEvent(event);
  m_targets_dirty = true;
  API::GetGui().CanvasReportSize(m_id, event->size().width(), event->size().height());
  update();
}

void ScriptHardwareMeshWidget::mousePressEvent(QMouseEvent* event)
{
  setFocus(Qt::MouseFocusReason);
  const QPointF point = event->position();
  if (event->button() == Qt::LeftButton)
  {
    API::GetGui().CanvasReportClick(m_id, point.x(), point.y());
    API::GetGui().CanvasReportLeftDown(m_id, true);
  }
  else if (event->button() == Qt::RightButton)
  {
    API::GetGui().CanvasReportRightClick(m_id, point.x(), point.y());
    API::GetGui().CanvasReportRightDown(m_id, true);
  }
  API::GetGui().CanvasReportMouse(m_id, point.x(), point.y(), true);
  event->accept();
}

void ScriptHardwareMeshWidget::mouseReleaseEvent(QMouseEvent* event)
{
  const QPointF point = event->position();
  if (event->button() == Qt::LeftButton)
    API::GetGui().CanvasReportLeftDown(m_id, false);
  else if (event->button() == Qt::RightButton)
    API::GetGui().CanvasReportRightDown(m_id, false);
  API::GetGui().CanvasReportMouse(m_id, point.x(), point.y(), true);
  event->accept();
}

void ScriptHardwareMeshWidget::mouseMoveEvent(QMouseEvent* event)
{
  const QPointF point = event->position();
  API::GetGui().CanvasReportMouse(m_id, point.x(), point.y(), true);
  event->accept();
}

void ScriptHardwareMeshWidget::keyPressEvent(QKeyEvent* event)
{
  if (event->key() == Qt::Key_H && !event->isAutoRepeat())
  {
    API::GetGui().CanvasReportCaptureToggle(m_id);
    event->accept();
    return;
  }
  if (const u32 bit = CanvasKeyBit(event->key()))
  {
    m_key_mask |= bit;
    API::GetGui().CanvasReportKeyMask(m_id, m_key_mask);
  }
  // Do not consume unrecognized keys; Dolphin's global shortcuts can still see them.
  event->ignore();
}

void ScriptHardwareMeshWidget::keyReleaseEvent(QKeyEvent* event)
{
  if (const u32 bit = CanvasKeyBit(event->key()))
  {
    m_key_mask &= ~bit;
    API::GetGui().CanvasReportKeyMask(m_id, m_key_mask);
  }
  event->ignore();
}

void ScriptHardwareMeshWidget::focusOutEvent(QFocusEvent* event)
{
  m_key_mask = 0;
  API::GetGui().CanvasReportKeyMask(m_id, 0);
  API::GetGui().CanvasReportLeftDown(m_id, false);
  API::GetGui().CanvasReportRightDown(m_id, false);
  QWidget::focusOutEvent(event);
}

void ScriptHardwareMeshWidget::wheelEvent(QWheelEvent* event)
{
  API::GetGui().CanvasReportWheel(m_id, event->angleDelta().y() / 120.0f);
  event->accept();
}

bool ScriptHardwareMeshWidget::EnsureResources()
{
  if (m_ready)
    return true;

  m_resources = std::make_unique<Resources>();
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  D3D_FEATURE_LEVEL level{};
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, requested,
                               std::size(requested), D3D11_SDK_VERSION, &m_resources->device,
                               &level, &m_resources->context)))
  {
    ReleaseResources();
    return false;
  }

  ComPtr<IDXGIDevice> dxgi_device;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIFactory> factory;
  if (FAILED(m_resources->device.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter)) ||
      FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
  {
    ReleaseResources();
    return false;
  }

  DXGI_SWAP_CHAIN_DESC swap_desc{};
  swap_desc.BufferCount = 1;
  swap_desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap_desc.OutputWindow = reinterpret_cast<HWND>(winId());
  swap_desc.SampleDesc.Count = 1;
  swap_desc.Windowed = TRUE;
  // DISCARD can expose a cleared child HWND between Qt paint events. The
  // preserved sequential surface keeps the last completed GPU frame visible.
  swap_desc.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;
  if (FAILED(factory->CreateSwapChain(m_resources->device.Get(), &swap_desc,
                                      &m_resources->swap_chain)))
  {
    ReleaseResources();
    return false;
  }

  ComPtr<ID3DBlob> vs, ps, gs, errors;
  if (FAILED(D3DCompile(VERTEX_SHADER, sizeof(VERTEX_SHADER) - 1, nullptr, nullptr, nullptr, "main",
                        "vs_4_0", 0, 0, &vs, &errors)) ||
      FAILED(D3DCompile(PIXEL_SHADER, sizeof(PIXEL_SHADER) - 1, nullptr, nullptr, nullptr, "main",
                        "ps_4_0", 0, 0, &ps, &errors)) ||
      FAILED(D3DCompile(LINE_GEOMETRY_SHADER, sizeof(LINE_GEOMETRY_SHADER) - 1, nullptr, nullptr,
                        nullptr, "main", "gs_4_0", 0, 0, &gs, &errors)) ||
      FAILED(m_resources->device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(),
                                                      nullptr, &m_resources->vertex_shader)) ||
      FAILED(m_resources->device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(),
                                                     nullptr, &m_resources->pixel_shader)) ||
      FAILED(m_resources->device->CreateGeometryShader(gs->GetBufferPointer(), gs->GetBufferSize(),
                                                        nullptr, &m_resources->line_geometry_shader)))
  {
    ReleaseResources();
    return false;
  }
  const D3D11_INPUT_ELEMENT_DESC input[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  if (FAILED(m_resources->device->CreateInputLayout(input, std::size(input), vs->GetBufferPointer(),
                                                     vs->GetBufferSize(), &m_resources->input_layout)))
  {
    ReleaseResources();
    return false;
  }
  D3D11_BUFFER_DESC constant_desc{};
  constant_desc.ByteWidth = sizeof(Constants);
  constant_desc.Usage = D3D11_USAGE_DEFAULT;
  constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  if (FAILED(m_resources->device->CreateBuffer(&constant_desc, nullptr, &m_resources->constants)))
  {
    ReleaseResources();
    return false;
  }
  // The script HUD is drawn directly onto the D3D backbuffer by
  // Direct2D. This keeps screen space UI above the mesh without a second Qt
  // child window or renderer specific panel code.
  D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr,
                    reinterpret_cast<void**>(m_resources->d2d_factory.GetAddressOf()));
  if (m_resources->d2d_factory &&
      SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                    reinterpret_cast<IUnknown**>(m_resources->dwrite_factory.GetAddressOf()))))
  {
    m_resources->dwrite_factory->CreateTextFormat(
        L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &m_resources->hud_format);
  }
  D3D11_RASTERIZER_DESC raster{};
  raster.FillMode = D3D11_FILL_SOLID;
  raster.CullMode = D3D11_CULL_NONE;
  raster.DepthClipEnable = TRUE;
  raster.MultisampleEnable = TRUE;
  raster.AntialiasedLineEnable = TRUE;
  if (FAILED(m_resources->device->CreateRasterizerState(&raster, &m_resources->solid_rasterizer)))
  {
    ReleaseResources();
    return false;
  }
  raster.FillMode = D3D11_FILL_WIREFRAME;
  if (FAILED(m_resources->device->CreateRasterizerState(&raster, &m_resources->wire_rasterizer)))
  {
    ReleaseResources();
    return false;
  }
  D3D11_DEPTH_STENCIL_DESC depth{};
  depth.DepthEnable = TRUE;
  depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
  if (FAILED(m_resources->device->CreateDepthStencilState(&depth, &m_resources->depth_write)))
  {
    ReleaseResources();
    return false;
  }
  depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  if (FAILED(m_resources->device->CreateDepthStencilState(&depth, &m_resources->depth_read)))
  {
    ReleaseResources();
    return false;
  }
  depth.DepthEnable = FALSE;
  if (FAILED(m_resources->device->CreateDepthStencilState(&depth, &m_resources->depth_off)))
  {
    ReleaseResources();
    return false;
  }
  D3D11_BLEND_DESC blend{};
  blend.RenderTarget[0].BlendEnable = TRUE;
  blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  if (FAILED(m_resources->device->CreateBlendState(&blend, &m_resources->blend)))
  {
    ReleaseResources();
    return false;
  }
  D3D11_BLEND_DESC no_color_desc{};
  no_color_desc.RenderTarget[0].RenderTargetWriteMask = 0;
  if (FAILED(m_resources->device->CreateBlendState(&no_color_desc, &m_resources->no_color_blend)))
  {
    ReleaseResources();
    return false;
  }
  m_targets_dirty = true;
  m_ready = true;
  return true;
}

void ScriptHardwareMeshWidget::RecreateTargets()
{
  if (!m_resources || !m_targets_dirty || width() <= 0 || height() <= 0)
    return;
  auto& r = *m_resources;
  r.context->OMSetRenderTargets(0, nullptr, nullptr);
  r.backbuffer.Reset();
  r.target.Reset();
  r.depth.Reset();
  r.depth_texture.Reset();
  r.msaa_target.Reset();
  r.msaa_color.Reset();
  r.msaa_depth.Reset();
  r.msaa_depth_texture.Reset();
  r.hud_brush.Reset();
  r.hud_target.Reset();
  if (FAILED(r.swap_chain->ResizeBuffers(0, width(), height(), DXGI_FORMAT_UNKNOWN, 0)))
    return;
  if (FAILED(r.swap_chain->GetBuffer(0, IID_PPV_ARGS(&r.backbuffer))) ||
      FAILED(r.device->CreateRenderTargetView(r.backbuffer.Get(), nullptr, &r.target)))
    return;
  D3D11_TEXTURE2D_DESC color_desc{};
  r.backbuffer->GetDesc(&color_desc);
  D3D11_TEXTURE2D_DESC depth_desc{};
  depth_desc.Width = width(); depth_desc.Height = height(); depth_desc.MipLevels = 1;
  depth_desc.ArraySize = 1; depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  depth_desc.SampleDesc.Count = 1; depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

  UINT color_quality = 0;
  UINT depth_quality = 0;
  r.device->CheckMultisampleQualityLevels(color_desc.Format, 4, &color_quality);
  r.device->CheckMultisampleQualityLevels(depth_desc.Format, 4, &depth_quality);
  if (color_quality != 0 && depth_quality != 0)
  {
    D3D11_TEXTURE2D_DESC msaa_color_desc = color_desc;
    msaa_color_desc.SampleDesc.Count = 4;
    msaa_color_desc.SampleDesc.Quality = 0;
    msaa_color_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    D3D11_TEXTURE2D_DESC msaa_depth_desc = depth_desc;
    msaa_depth_desc.SampleDesc.Count = 4;
    msaa_depth_desc.SampleDesc.Quality = 0;
    if (FAILED(r.device->CreateTexture2D(&msaa_color_desc, nullptr, &r.msaa_color)) ||
        FAILED(r.device->CreateRenderTargetView(r.msaa_color.Get(), nullptr, &r.msaa_target)) ||
        FAILED(r.device->CreateTexture2D(&msaa_depth_desc, nullptr, &r.msaa_depth_texture)) ||
        FAILED(r.device->CreateDepthStencilView(r.msaa_depth_texture.Get(), nullptr, &r.msaa_depth)))
    {
      r.msaa_target.Reset();
      r.msaa_color.Reset();
      r.msaa_depth.Reset();
      r.msaa_depth_texture.Reset();
    }
  }
  if (FAILED(r.device->CreateTexture2D(&depth_desc, nullptr, &r.depth_texture)) ||
      FAILED(r.device->CreateDepthStencilView(r.depth_texture.Get(), nullptr, &r.depth)))
    return;
  if (r.d2d_factory && r.hud_format)
  {
    ComPtr<IDXGISurface> surface;
    const D2D1_RENDER_TARGET_PROPERTIES properties =
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                     D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                       D2D1_ALPHA_MODE_IGNORE));
    if (SUCCEEDED(r.backbuffer.As(&surface)) &&
        SUCCEEDED(r.d2d_factory->CreateDxgiSurfaceRenderTarget(surface.Get(), &properties,
                                                                 &r.hud_target)))
    {
      r.hud_target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f), &r.hud_brush);
    }
  }
  m_targets_dirty = false;
}

void ScriptHardwareMeshWidget::UploadMeshes()
{
  if (!m_resources || !m_mesh_dirty)
    return;
  for (size_t i = 0; i < m_uploaded_groups.size(); ++i)
  {
    if (m_uploaded_groups[i] == m_snapshot.groups[i])
      continue;
    m_resources->vertex_buffers[i].Reset();
    m_resources->vertex_counts[i] = 0;
    m_uploaded_groups[i] = m_snapshot.groups[i];
    if (!m_uploaded_groups[i] || m_uploaded_groups[i]->empty())
      continue;
    const auto& vertices = *m_uploaded_groups[i];
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(vertices[0]));
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA source_data{};
    source_data.pSysMem = vertices.data();
    if (SUCCEEDED(m_resources->device->CreateBuffer(&desc, &source_data,
                                                    &m_resources->vertex_buffers[i])))
      m_resources->vertex_counts[i] = static_cast<UINT>(vertices.size());
  }
  m_mesh_dirty = false;
}

void ScriptHardwareMeshWidget::Present()
{
  if (!m_resources || SUCCEEDED(m_resources->swap_chain->Present(0, 0)))
    return;

  // A driver reset otherwise leaves the native child black until an unrelated
  // script-state change happens. Recreate the device and reupload retained
  // mesh groups on the next Qt paint event.
  ReleaseResources();
  m_mesh_dirty = true;
  m_targets_dirty = true;
  update();
}

QImage ScriptHardwareMeshWidget::CaptureFrame()
{
  if (!EnsureResources())
    return {};

  // Render first so frame-dump callbacks capture the same retained scene that
  // is being presented to the detached script window.
  Render();
  if (!m_resources || !m_resources->backbuffer)
    return {};

  auto& r = *m_resources;
  D3D11_TEXTURE2D_DESC source_desc{};
  r.backbuffer->GetDesc(&source_desc);
  const QSize source_size(static_cast<int>(source_desc.Width), static_cast<int>(source_desc.Height));
  if (source_size.isEmpty())
    return {};

  if (!r.capture_texture || r.capture_size != source_size)
  {
    D3D11_TEXTURE2D_DESC staging_desc = source_desc;
    staging_desc.BindFlags = 0;
    staging_desc.MiscFlags = 0;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    r.capture_texture.Reset();
    if (FAILED(r.device->CreateTexture2D(&staging_desc, nullptr, &r.capture_texture)))
      return {};
    r.capture_size = source_size;
  }

  // The backbuffer may remain bound after a no-overlay render. Unbind it
  // before making it a copy source, then let the next Render() restore state.
  r.context->OMSetRenderTargets(0, nullptr, nullptr);
  r.context->CopyResource(r.capture_texture.Get(), r.backbuffer.Get());
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(r.context->Map(r.capture_texture.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
    return {};

  QImage image(source_size, QImage::Format_ARGB32);
  const size_t row_bytes = static_cast<size_t>(source_size.width()) * 4;
  for (int y = 0; y < source_size.height(); ++y)
  {
    std::memcpy(image.scanLine(y), static_cast<const u8*>(mapped.pData) +
                                      static_cast<size_t>(y) * mapped.RowPitch,
                row_bytes);
  }
  r.context->Unmap(r.capture_texture.Get(), 0);
  return image;
}

void ScriptHardwareMeshWidget::Render()
{
  if (!EnsureResources())
    return;
  RecreateTargets();
  if (!m_resources->target || !m_resources->depth)
    return;
  UploadMeshes();
  auto& r = *m_resources;
  // A native QWidget paint event may have already exposed its cleared
  // background. Never throttle by returning from here: that caused D3D and
  // the Direct2D vertex panel to disappear intermittently between frames.
  constexpr float background[] = {0.063f, 0.075f, 0.094f, 1.0f};
  ID3D11RenderTargetView* render_target = r.msaa_target ? r.msaa_target.Get() : r.target.Get();
  ID3D11DepthStencilView* render_depth = r.msaa_depth ? r.msaa_depth.Get() : r.depth.Get();
  r.context->ClearRenderTargetView(render_target, background);
  r.context->ClearDepthStencilView(render_depth, D3D11_CLEAR_DEPTH, 1.0f, 0);
  if (!m_snapshot.state.enabled)
  {
    Present();
    return;
  }
  const auto& s = m_snapshot.state;
  Constants constants{};
  std::memcpy(constants.eye_radius, s.eye.data(), sizeof(s.eye));
  std::memcpy(constants.right_focal, s.right.data(), sizeof(s.right));
  std::memcpy(constants.up_fill, s.up.data(), sizeof(s.up));
  std::memcpy(constants.forward_wire, s.forward.data(), sizeof(s.forward));
  constants.eye_radius[3] = s.radius;
  constants.right_focal[3] = s.focal;
  constants.up_fill[3] = s.fill_opacity;
  constants.forward_wire[3] = s.wire_opacity;
  float viewport_x = 0.0f;
  float viewport_y = 0.0f;
  float viewport_width = static_cast<float>(width());
  float viewport_height = static_cast<float>(height());
  if (s.clean_capture)
  {
    constexpr float capture_aspect = 4.0f / 3.0f;
    if (viewport_width / viewport_height > capture_aspect)
    {
      const float content_width = viewport_height * capture_aspect;
      viewport_x = (viewport_width - content_width) * 0.5f;
      viewport_width = content_width;
    }
    else
    {
      const float content_height = viewport_width / capture_aspect;
      viewport_y = (viewport_height - content_height) * 0.5f;
      viewport_height = content_height;
    }
  }
  constants.viewport_flags[0] = viewport_width;
  constants.viewport_flags[1] = viewport_height;
  constants.viewport_flags[2] = 1.0f;
  r.context->UpdateSubresource(r.constants.Get(), 0, nullptr, &constants, 0, 0);
  const D3D11_VIEWPORT viewport{viewport_x, viewport_y, viewport_width, viewport_height, 0.0f, 1.0f};
  r.context->OMSetRenderTargets(1, &render_target, render_depth);
  r.context->RSSetViewports(1, &viewport);
  r.context->IASetInputLayout(r.input_layout.Get());
  r.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  r.context->VSSetShader(r.vertex_shader.Get(), nullptr, 0);
  r.context->PSSetShader(r.pixel_shader.Get(), nullptr, 0);
  r.context->GSSetShader(nullptr, nullptr, 0);
  r.context->VSSetConstantBuffers(0, 1, r.constants.GetAddressOf());
  r.context->PSSetConstantBuffers(0, 1, r.constants.GetAddressOf());
  r.context->OMSetBlendState(r.blend.Get(), nullptr, 0xFFFFFFFF);
  UINT stride = sizeof(API::Gui::HardwareVertex), offset = 0;
  auto draw_triangles = [&](size_t first, size_t last) {
    r.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (size_t i = first; i < last; ++i)
    {
      if (!r.vertex_buffers[i])
        continue;
      r.context->IASetVertexBuffers(0, 1, r.vertex_buffers[i].GetAddressOf(), &stride, &offset);
      r.context->Draw(r.vertex_counts[i], 0);
    }
  };
  // Prepass all collision geometry together so moving BG correctly
  // occludes/is occluded by KCL. X-ray deliberately bypasses this depth map.
  if (!s.xray)
  {
    r.context->OMSetDepthStencilState(r.depth_write.Get(), 0);
    r.context->RSSetState(r.solid_rasterizer.Get());
    r.context->OMSetBlendState(r.no_color_blend.Get(), nullptr, 0xFFFFFFFF);
    draw_triangles(0, 5);
  }
  r.context->OMSetBlendState(r.blend.Get(), nullptr, 0xFFFFFFFF);
  r.context->OMSetDepthStencilState(s.xray ? r.depth_off.Get() : r.depth_read.Get(), 0);
  if (s.filled && s.fill_opacity > 0.0f)
  {
    constants.viewport_flags[2] = s.fill_opacity;
    constants.viewport_flags[3] = 0.0f;
    r.context->UpdateSubresource(r.constants.Get(), 0, nullptr, &constants, 0, 0);
    r.context->RSSetState(r.solid_rasterizer.Get());
    draw_triangles(0, 5);
  }
  // Group 7 is a retained fill-only overlay layer. Draw it before the scene
  // wire pass and write its depth in normal mode: its explicitly submitted
  // line group remains visible, while unrelated mesh wire edges cannot show
  // through triangulated faces. This is useful for any script drawn volume,
  // not just collision viewers.
  if (r.vertex_buffers[7] && r.vertex_counts[7] != 0 && s.fill_opacity > 0.0f &&
      !(s.debug_on_top || s.xray))
  {
    constants.viewport_flags[2] = s.fill_opacity;
    constants.viewport_flags[3] = 0.0f;
    r.context->UpdateSubresource(r.constants.Get(), 0, nullptr, &constants, 0, 0);
    r.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    r.context->RSSetState(r.solid_rasterizer.Get());
    r.context->OMSetDepthStencilState(r.depth_write.Get(), 0);
    r.context->IASetVertexBuffers(0, 1, r.vertex_buffers[7].GetAddressOf(), &stride, &offset);
    r.context->Draw(r.vertex_counts[7], 0);
  }
  if (s.wireframe && s.wire_opacity > 0.0f)
  {
    constants.viewport_flags[2] = s.wire_opacity;
    constants.viewport_flags[3] = 1.0f;
    r.context->UpdateSubresource(r.constants.Get(), 0, nullptr, &constants, 0, 0);
    r.context->RSSetState(r.solid_rasterizer.Get());
    draw_triangles(0, 5);
  }
  // On top/X-ray overlay fills are intentionally rendered after the scene.
  if (r.vertex_buffers[7] && r.vertex_counts[7] != 0 && s.fill_opacity > 0.0f &&
      (s.debug_on_top || s.xray))
  {
    constants.viewport_flags[2] = s.fill_opacity;
    constants.viewport_flags[3] = 0.0f;
    r.context->UpdateSubresource(r.constants.Get(), 0, nullptr, &constants, 0, 0);
    r.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    r.context->RSSetState(r.solid_rasterizer.Get());
    r.context->OMSetDepthStencilState((s.debug_on_top || s.xray) ? r.depth_off.Get() : r.depth_read.Get(), 0);
    r.context->IASetVertexBuffers(0, 1, r.vertex_buffers[7].GetAddressOf(), &stride, &offset);
    r.context->Draw(r.vertex_counts[7], 0);
  }
  // Group 5 is the retained line-list layer. It shares this scene's depth
  // buffer instead of requiring a second native window.
  if (r.vertex_buffers[5] && r.vertex_counts[5] != 0)
  {
    constants.viewport_flags[2] = s.wire_opacity;
    constants.viewport_flags[3] = 2.0f;
    r.context->UpdateSubresource(r.constants.Get(), 0, nullptr, &constants, 0, 0);
    r.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    r.context->RSSetState(r.solid_rasterizer.Get());
    // The generic on-top mode leaves line data visible through the main mesh.
    r.context->OMSetDepthStencilState((s.debug_on_top || s.xray) ? r.depth_off.Get() : r.depth_read.Get(), 0);
    r.context->GSSetConstantBuffers(0, 1, r.constants.GetAddressOf());
    r.context->GSSetShader(r.line_geometry_shader.Get(), nullptr, 0);
    r.context->IASetVertexBuffers(0, 1, r.vertex_buffers[5].GetAddressOf(), &stride, &offset);
    r.context->Draw(r.vertex_counts[5], 0);
    r.context->GSSetShader(nullptr, nullptr, 0);
  }
  if (r.vertex_buffers[6] && r.vertex_counts[6] != 0)
  {
    constants.viewport_flags[2] = 1.0f;
    constants.viewport_flags[3] = 0.0f;
    r.context->UpdateSubresource(r.constants.Get(), 0, nullptr, &constants, 0, 0);
    r.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    r.context->RSSetState(r.solid_rasterizer.Get());
    r.context->OMSetDepthStencilState(r.depth_off.Get(), 0);
    r.context->IASetVertexBuffers(0, 1, r.vertex_buffers[6].GetAddressOf(), &stride, &offset);
    r.context->Draw(r.vertex_counts[6], 0);
  }
  if (r.msaa_color)
    r.context->ResolveSubresource(r.backbuffer.Get(), 0, r.msaa_color.Get(), 0,
                                  DXGI_FORMAT_B8G8R8A8_UNORM);
  if (r.hud_target && r.hud_brush && m_snapshot.hud && !s.clean_capture)
  {
    r.context->OMSetRenderTargets(0, nullptr, nullptr);
    r.context->Flush();
    const auto set_color = [&](u32 color) {
      r.hud_brush->SetColor(D2D1::ColorF(((color >> 16) & 0xFF) / 255.0f,
                                          ((color >> 8) & 0xFF) / 255.0f,
                                          (color & 0xFF) / 255.0f,
                                          ((color >> 24) & 0xFF) / 255.0f));
    };
    const auto draw_triangle = [&](const API::Gui::CanvasPrimitive& p, bool filled) {
      ComPtr<ID2D1PathGeometry> geometry;
      ComPtr<ID2D1GeometrySink> sink;
      if (FAILED(r.d2d_factory->CreatePathGeometry(geometry.GetAddressOf())) ||
          FAILED(geometry->Open(sink.GetAddressOf())))
        return;
      sink->BeginFigure(D2D1::Point2F(p.p0.x, p.p0.y),
                        filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
      const D2D1_POINT_2F points[] = {D2D1::Point2F(p.p1.x, p.p1.y), D2D1::Point2F(p.p2.x, p.p2.y)};
      sink->AddLines(points, std::size(points));
      sink->EndFigure(D2D1_FIGURE_END_CLOSED);
      if (SUCCEEDED(sink->Close()))
      {
        if (filled)
          r.hud_target->FillGeometry(geometry.Get(), r.hud_brush.Get());
        else
          r.hud_target->DrawGeometry(geometry.Get(), r.hud_brush.Get(), p.thickness);
      }
    };
    r.hud_target->BeginDraw();
    for (const API::Gui::CanvasPrimitive& p : *m_snapshot.hud)
    {
      set_color(p.color);
      switch (p.type)
      {
      case API::Gui::CanvasPrimitive::Type::Line:
        r.hud_target->DrawLine(D2D1::Point2F(p.p0.x, p.p0.y), D2D1::Point2F(p.p1.x, p.p1.y),
                               r.hud_brush.Get(), p.thickness);
        break;
      case API::Gui::CanvasPrimitive::Type::Rect:
        r.hud_target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(p.p0.x, p.p0.y, p.p1.x, p.p1.y),
                                                              p.rounding, p.rounding), r.hud_brush.Get(), p.thickness);
        break;
      case API::Gui::CanvasPrimitive::Type::RectFilled:
        r.hud_target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(p.p0.x, p.p0.y, p.p1.x, p.p1.y),
                                                              p.rounding, p.rounding), r.hud_brush.Get());
        break;
      case API::Gui::CanvasPrimitive::Type::Circle:
        r.hud_target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(p.p0.x, p.p0.y), p.radius, p.radius),
                                  r.hud_brush.Get(), p.thickness);
        break;
      case API::Gui::CanvasPrimitive::Type::CircleFilled:
        r.hud_target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(p.p0.x, p.p0.y), p.radius, p.radius),
                                  r.hud_brush.Get());
        break;
      case API::Gui::CanvasPrimitive::Type::Triangle:
        draw_triangle(p, false);
        break;
      case API::Gui::CanvasPrimitive::Type::TriangleFilled:
        draw_triangle(p, true);
        break;
      case API::Gui::CanvasPrimitive::Type::Text:
      {
        const std::wstring text = QString::fromUtf8(p.text.c_str()).toStdWString();
        r.hud_target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), r.hud_format.Get(),
                                D2D1::RectF(p.p0.x, p.p0.y, static_cast<float>(width()), p.p0.y + 22.0f),
                                r.hud_brush.Get());
        break;
      }
      default:
        break;
      }
    }
    if (r.hud_target->EndDraw() == D2DERR_RECREATE_TARGET)
    {
      r.hud_brush.Reset();
      r.hud_target.Reset();
      m_targets_dirty = true;
    }
  }
  Present();
}

void ScriptHardwareMeshWidget::ReleaseResources()
{
  m_ready = false;
  m_resources.reset();
}

#else

#include <QBoxLayout>
#include <QEvent>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QFontMetricsF>
#include <QPainter>
#include <QPolygonF>
#include <QSurfaceFormat>
#include <QWindow>

#include <array>
#include <cstring>
#include <string>

namespace
{
u32 CanvasKeyBit(int key)
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

constexpr char TRIANGLE_VERTEX_SHADER[] = R"(
#version 330 core
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_color;
uniform vec4 eye_radius;
uniform vec4 right_focal;
uniform vec4 up_fill;
uniform vec4 forward_wire;
uniform vec2 viewport_size;
out vec3 world;
out vec4 color;
out float depth;
out vec3 bary;
void main()
{
  vec3 relative = in_position - eye_radius.xyz;
  float x = dot(relative, right_focal.xyz);
  float y = dot(relative, up_fill.xyz);
  float z = dot(relative, forward_wire.xyz);
  float d3d_z = z * (100000.0 / 99995.0) - (500000.0 / 99995.0);
  gl_Position = vec4(right_focal.w * 2.0 * x / viewport_size.x,
                     right_focal.w * 2.0 * y / viewport_size.y,
                     d3d_z * 2.0 - z, z);
  world = in_position;
  color = vec4(in_color.bgr, in_color.a);
  depth = z;
  uint corner = uint(gl_VertexID) % 3u;
  bary = corner == 0u ? vec3(1.0, 0.0, 0.0) :
         (corner == 1u ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0));
}
)";

constexpr char MESH_FRAGMENT_SHADER[] = R"(
#version 330 core
uniform vec4 eye_radius;
uniform float alpha_scale;
uniform int mode;
in vec3 world;
in vec4 color;
in float depth;
in vec3 bary;
out vec4 out_color;
void main()
{
  if (depth < 5.0)
    discard;
  if (eye_radius.w > 0.0 && distance(world, eye_radius.xyz) > eye_radius.w)
    discard;
  float alpha = color.a * alpha_scale;
  if (mode == 1)
  {
    float edge_distance = min(bary.x, min(bary.y, bary.z));
    float edge_width = max(fwidth(edge_distance) * 2.5, 0.00001);
    alpha *= 1.0 - smoothstep(0.0, edge_width, edge_distance);
    if (alpha < 0.003)
      discard;
    out_color = vec4(1.0, 1.0, 1.0, alpha);
    return;
  }
  out_color = vec4(color.rgb, alpha);
}
)";

constexpr char LINE_GEOMETRY_SHADER[] = R"(
#version 330 core
layout(lines) in;
layout(triangle_strip, max_vertices = 4) out;
uniform vec2 viewport_size;
in vec3 world[];
in vec4 color[];
in float depth[];
in vec3 bary[];
out vec3 g_world;
out vec4 g_color;
out float g_depth;
out vec3 g_bary;
void emit_vertex(vec4 position, int index)
{
  gl_Position = position;
  g_world = world[index];
  g_color = color[index];
  g_depth = depth[index];
  g_bary = vec3(0.0);
  EmitVertex();
}
void main()
{
  vec2 p0 = gl_in[0].gl_Position.xy / gl_in[0].gl_Position.w;
  vec2 p1 = gl_in[1].gl_Position.xy / gl_in[1].gl_Position.w;
  vec2 delta = (p1 - p0) * viewport_size;
  float length_pixels = max(length(delta), 0.001);
  float half_width = (color[0].r > 0.9 && color[0].g < 0.3 && color[0].b < 0.3) ? 4.0 : 2.0;
  vec2 offset = vec2(-delta.y, delta.x) / length_pixels * half_width / viewport_size;
  emit_vertex(vec4((p0 + offset) * gl_in[0].gl_Position.w,
                   gl_in[0].gl_Position.z, gl_in[0].gl_Position.w), 0);
  emit_vertex(vec4((p0 - offset) * gl_in[0].gl_Position.w,
                   gl_in[0].gl_Position.z, gl_in[0].gl_Position.w), 0);
  emit_vertex(vec4((p1 + offset) * gl_in[1].gl_Position.w,
                   gl_in[1].gl_Position.z, gl_in[1].gl_Position.w), 1);
  emit_vertex(vec4((p1 - offset) * gl_in[1].gl_Position.w,
                   gl_in[1].gl_Position.z, gl_in[1].gl_Position.w), 1);
  EndPrimitive();
}
)";

constexpr char LINE_FRAGMENT_SHADER[] = R"(
#version 330 core
uniform vec4 eye_radius;
uniform float alpha_scale;
in vec3 g_world;
in vec4 g_color;
in float g_depth;
in vec3 g_bary;
out vec4 out_color;
void main()
{
  if (g_depth < 5.0)
    discard;
  if (eye_radius.w > 0.0 && distance(g_world, eye_radius.xyz) > eye_radius.w)
    discard;
  out_color = vec4(g_color.rgb, g_color.a * alpha_scale);
}
)";

constexpr char OVERLAY_VERTEX_SHADER[] = R"(
#version 330 core
layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;
out vec2 uv;
void main()
{
  gl_Position = vec4(in_position, 0.0, 1.0);
  uv = in_uv;
}
)";

constexpr char OVERLAY_FRAGMENT_SHADER[] = R"(
#version 330 core
uniform sampler2D overlay_texture;
in vec2 uv;
out vec4 out_color;
void main()
{
  out_color = texture(overlay_texture, uv);
}
)";

GLuint CompileShader(QOpenGLExtraFunctions* gl, GLenum type, const char* source)
{
  const GLuint shader = gl->glCreateShader(type);
  gl->glShaderSource(shader, 1, &source, nullptr);
  gl->glCompileShader(shader);
  GLint compiled = GL_FALSE;
  gl->glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled == GL_TRUE)
    return shader;
  gl->glDeleteShader(shader);
  return 0;
}

GLuint CreateProgram(QOpenGLExtraFunctions* gl, const char* vertex, const char* fragment,
                     const char* geometry = nullptr)
{
  const GLuint vs = CompileShader(gl, GL_VERTEX_SHADER, vertex);
  const GLuint fs = CompileShader(gl, GL_FRAGMENT_SHADER, fragment);
  const GLuint gs = geometry ? CompileShader(gl, GL_GEOMETRY_SHADER, geometry) : 0;
  if (!vs || !fs || (geometry && !gs))
  {
    if (vs)
      gl->glDeleteShader(vs);
    if (fs)
      gl->glDeleteShader(fs);
    if (gs)
      gl->glDeleteShader(gs);
    return 0;
  }
  const GLuint program = gl->glCreateProgram();
  gl->glAttachShader(program, vs);
  gl->glAttachShader(program, fs);
  if (gs)
    gl->glAttachShader(program, gs);
  gl->glLinkProgram(program);
  gl->glDeleteShader(vs);
  gl->glDeleteShader(fs);
  if (gs)
    gl->glDeleteShader(gs);
  GLint linked = GL_FALSE;
  gl->glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked == GL_TRUE)
    return program;
  gl->glDeleteProgram(program);
  return 0;
}
}  // namespace

class ScriptHardwareMeshWindow final : public QWindow
{
public:
  explicit ScriptHardwareMeshWindow(ScriptHardwareMeshWidget* owner) : m_owner(owner)
  {
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    format.setSamples(4);
    setSurfaceType(QSurface::OpenGLSurface);
    setFormat(format);
  }

protected:
  bool event(QEvent* event) override
  {
    if (event->type() == QEvent::UpdateRequest)
      m_owner->Render();
    return QWindow::event(event);
  }

  void exposeEvent(QExposeEvent*) override
  {
    if (isExposed())
      m_owner->Render();
  }

  void mousePressEvent(QMouseEvent* event) override
  {
    const QPointF point = event->position();
    if (event->button() == Qt::LeftButton)
    {
      API::GetGui().CanvasReportClick(m_owner->m_id, point.x(), point.y());
      API::GetGui().CanvasReportLeftDown(m_owner->m_id, true);
    }
    else if (event->button() == Qt::RightButton)
    {
      API::GetGui().CanvasReportRightClick(m_owner->m_id, point.x(), point.y());
      API::GetGui().CanvasReportRightDown(m_owner->m_id, true);
    }
    API::GetGui().CanvasReportMouse(m_owner->m_id, point.x(), point.y(), true);
    event->accept();
  }

  void mouseReleaseEvent(QMouseEvent* event) override
  {
    const QPointF point = event->position();
    if (event->button() == Qt::LeftButton)
      API::GetGui().CanvasReportLeftDown(m_owner->m_id, false);
    else if (event->button() == Qt::RightButton)
      API::GetGui().CanvasReportRightDown(m_owner->m_id, false);
    API::GetGui().CanvasReportMouse(m_owner->m_id, point.x(), point.y(), true);
    event->accept();
  }

  void mouseMoveEvent(QMouseEvent* event) override
  {
    const QPointF point = event->position();
    API::GetGui().CanvasReportMouse(m_owner->m_id, point.x(), point.y(), true);
    event->accept();
  }

  void keyPressEvent(QKeyEvent* event) override
  {
    if (event->key() == Qt::Key_H && !event->isAutoRepeat())
    {
      API::GetGui().CanvasReportCaptureToggle(m_owner->m_id);
      event->accept();
      return;
    }
    if (const u32 bit = CanvasKeyBit(event->key()))
    {
      m_owner->m_key_mask |= bit;
      API::GetGui().CanvasReportKeyMask(m_owner->m_id, m_owner->m_key_mask);
    }
    event->ignore();
  }

  void keyReleaseEvent(QKeyEvent* event) override
  {
    if (const u32 bit = CanvasKeyBit(event->key()))
    {
      m_owner->m_key_mask &= ~bit;
      API::GetGui().CanvasReportKeyMask(m_owner->m_id, m_owner->m_key_mask);
    }
    event->ignore();
  }

  void focusOutEvent(QFocusEvent* event) override
  {
    m_owner->m_key_mask = 0;
    API::GetGui().CanvasReportKeyMask(m_owner->m_id, 0);
    API::GetGui().CanvasReportLeftDown(m_owner->m_id, false);
    API::GetGui().CanvasReportRightDown(m_owner->m_id, false);
    QWindow::focusOutEvent(event);
  }

  void wheelEvent(QWheelEvent* event) override
  {
    API::GetGui().CanvasReportWheel(m_owner->m_id, event->angleDelta().y() / 120.0f);
    event->accept();
  }

private:
  ScriptHardwareMeshWidget* m_owner;
};

struct ScriptHardwareMeshWidget::Resources
{
  std::unique_ptr<QOpenGLContext> context;
  QOpenGLExtraFunctions* gl = nullptr;
  GLuint triangle_program = 0;
  GLuint line_program = 0;
  GLuint overlay_program = 0;
  GLuint mesh_vao = 0;
  GLuint overlay_vao = 0;
  GLuint overlay_vbo = 0;
  GLuint overlay_texture = 0;
  std::array<GLuint, 8> vertex_buffers{};
  std::array<GLsizei, 8> vertex_counts{};
  QSize overlay_size;
  std::shared_ptr<const std::vector<API::Gui::CanvasPrimitive>> overlay_hud;
  bool overlay_valid = false;
};

ScriptHardwareMeshWidget::ScriptHardwareMeshWidget(API::Gui::WidgetId id, QWidget* parent)
    : QWidget(parent), m_id(id)
{
  setAttribute(Qt::WA_NativeWindow);
  setAttribute(Qt::WA_OpaquePaintEvent);
  setAttribute(Qt::WA_NoSystemBackground);
  setFocusPolicy(Qt::StrongFocus);
  m_gl_window = new ScriptHardwareMeshWindow(this);
  m_gl_container = QWidget::createWindowContainer(m_gl_window, this);
  m_gl_container->setFocusPolicy(Qt::StrongFocus);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(m_gl_container);
}

ScriptHardwareMeshWidget::~ScriptHardwareMeshWidget()
{
  ReleaseResources();
}

void ScriptHardwareMeshWidget::SetSnapshot(API::Gui::HardwareSnapshot snapshot)
{
  if (m_snapshot.generation == snapshot.generation)
    return;
  for (size_t i = 0; i < m_uploaded_groups.size(); ++i)
  {
    if (m_snapshot.groups[i] != snapshot.groups[i])
      m_mesh_dirty = true;
  }
  m_snapshot = std::move(snapshot);
  if (m_gl_window)
    m_gl_window->requestUpdate();
}

QImage ScriptHardwareMeshWidget::CaptureFrame()
{
  Render();
  if (!m_resources || !m_resources->context || !m_gl_window || width() <= 0 || height() <= 0 ||
      !m_resources->context->makeCurrent(m_gl_window))
    return {};
  auto& r = *m_resources;
  const QSize size = m_gl_window->size() * m_gl_window->devicePixelRatio();
  QImage image(size, QImage::Format_RGBA8888);
  if (!image.isNull())
  {
    r.gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    r.gl->glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_UNSIGNED_BYTE, image.bits());
    image = image.mirrored();
  }
  m_resources->context->doneCurrent();
  return image;
}

QPaintEngine* ScriptHardwareMeshWidget::paintEngine() const
{
  return nullptr;
}

void ScriptHardwareMeshWidget::paintEvent(QPaintEvent*)
{
  if (m_gl_window)
    m_gl_window->requestUpdate();
}

void ScriptHardwareMeshWidget::resizeEvent(QResizeEvent* event)
{
  QWidget::resizeEvent(event);
  m_targets_dirty = true;
  API::GetGui().CanvasReportSize(m_id, event->size().width(), event->size().height());
  if (m_gl_window)
    m_gl_window->requestUpdate();
}

void ScriptHardwareMeshWidget::mousePressEvent(QMouseEvent* event) { event->ignore(); }
void ScriptHardwareMeshWidget::mouseReleaseEvent(QMouseEvent* event) { event->ignore(); }
void ScriptHardwareMeshWidget::mouseMoveEvent(QMouseEvent* event) { event->ignore(); }
void ScriptHardwareMeshWidget::keyPressEvent(QKeyEvent* event) { event->ignore(); }
void ScriptHardwareMeshWidget::keyReleaseEvent(QKeyEvent* event) { event->ignore(); }

void ScriptHardwareMeshWidget::focusOutEvent(QFocusEvent* event)
{
  m_key_mask = 0;
  API::GetGui().CanvasReportKeyMask(m_id, 0);
  API::GetGui().CanvasReportLeftDown(m_id, false);
  API::GetGui().CanvasReportRightDown(m_id, false);
  QWidget::focusOutEvent(event);
}

void ScriptHardwareMeshWidget::wheelEvent(QWheelEvent* event) { event->ignore(); }

bool ScriptHardwareMeshWidget::EnsureResources()
{
  if (m_ready)
    return true;
  if (!m_gl_window)
    return false;

  m_resources = std::make_unique<Resources>();
  m_resources->context = std::make_unique<QOpenGLContext>();
  m_resources->context->setFormat(m_gl_window->requestedFormat());
  if (!m_resources->context->create() || !m_resources->context->makeCurrent(m_gl_window))
  {
    ReleaseResources();
    return false;
  }
  auto& r = *m_resources;
  r.gl = r.context->extraFunctions();
  r.gl->initializeOpenGLFunctions();
  r.triangle_program = CreateProgram(r.gl, TRIANGLE_VERTEX_SHADER, MESH_FRAGMENT_SHADER);
  r.line_program = CreateProgram(r.gl, TRIANGLE_VERTEX_SHADER, LINE_FRAGMENT_SHADER,
                                 LINE_GEOMETRY_SHADER);
  r.overlay_program = CreateProgram(r.gl, OVERLAY_VERTEX_SHADER, OVERLAY_FRAGMENT_SHADER);
  if (!r.triangle_program || !r.line_program || !r.overlay_program)
  {
    r.context->doneCurrent();
    ReleaseResources();
    return false;
  }
  r.gl->glGenVertexArrays(1, &r.mesh_vao);
  r.gl->glBindVertexArray(r.mesh_vao);
  r.gl->glEnableVertexAttribArray(0);
  r.gl->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(API::Gui::HardwareVertex), nullptr);
  r.gl->glEnableVertexAttribArray(1);
  r.gl->glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(API::Gui::HardwareVertex),
                              reinterpret_cast<void*>(offsetof(API::Gui::HardwareVertex, color)));
  r.gl->glGenVertexArrays(1, &r.overlay_vao);
  r.gl->glGenBuffers(1, &r.overlay_vbo);
  r.gl->glBindVertexArray(r.overlay_vao);
  r.gl->glBindBuffer(GL_ARRAY_BUFFER, r.overlay_vbo);
  r.gl->glEnableVertexAttribArray(0);
  r.gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, nullptr);
  r.gl->glEnableVertexAttribArray(1);
  r.gl->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4,
                              reinterpret_cast<void*>(sizeof(float) * 2));
  r.gl->glGenTextures(1, &r.overlay_texture);
  r.gl->glBindTexture(GL_TEXTURE_2D, r.overlay_texture);
  r.gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  r.gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  r.gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  r.gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  r.context->doneCurrent();
  m_targets_dirty = true;
  m_ready = true;
  return true;
}

void ScriptHardwareMeshWidget::RecreateTargets()
{
  m_targets_dirty = false;
}

void ScriptHardwareMeshWidget::UploadMeshes()
{
  if (!m_resources || !m_mesh_dirty)
    return;
  auto& r = *m_resources;
  for (size_t i = 0; i < m_uploaded_groups.size(); ++i)
  {
    if (m_uploaded_groups[i] == m_snapshot.groups[i])
      continue;
    if (r.vertex_buffers[i])
      r.gl->glDeleteBuffers(1, &r.vertex_buffers[i]);
    r.vertex_buffers[i] = 0;
    r.vertex_counts[i] = 0;
    m_uploaded_groups[i] = m_snapshot.groups[i];
    if (!m_uploaded_groups[i] || m_uploaded_groups[i]->empty())
      continue;
    const auto& vertices = *m_uploaded_groups[i];
    r.gl->glGenBuffers(1, &r.vertex_buffers[i]);
    r.gl->glBindBuffer(GL_ARRAY_BUFFER, r.vertex_buffers[i]);
    r.gl->glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(vertices[0])),
                       vertices.data(), GL_STATIC_DRAW);
    r.vertex_counts[i] = static_cast<GLsizei>(vertices.size());
  }
  m_mesh_dirty = false;
}

void ScriptHardwareMeshWidget::Present()
{
  if (m_resources && m_resources->context && m_gl_window)
    m_resources->context->swapBuffers(m_gl_window);
}

void ScriptHardwareMeshWidget::Render()
{
  if (!m_gl_window || !m_gl_window->isExposed() || !EnsureResources() ||
      !m_resources->context->makeCurrent(m_gl_window))
    return;
  RecreateTargets();
  UploadMeshes();
  auto& r = *m_resources;
  const qreal pixel_ratio = m_gl_window->devicePixelRatio();
  const int pixel_width = static_cast<int>(m_gl_window->width() * pixel_ratio);
  const int pixel_height = static_cast<int>(m_gl_window->height() * pixel_ratio);
  if (pixel_width <= 0 || pixel_height <= 0)
  {
    r.context->doneCurrent();
    return;
  }

  r.gl->glViewport(0, 0, pixel_width, pixel_height);
  r.gl->glClearColor(0.063f, 0.075f, 0.094f, 1.0f);
  r.gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  if (m_snapshot.state.enabled)
  {
    const auto& s = m_snapshot.state;
    float viewport_x = 0.0f;
    float viewport_y = 0.0f;
    float viewport_width = static_cast<float>(pixel_width);
    float viewport_height = static_cast<float>(pixel_height);
    if (s.clean_capture)
    {
      constexpr float capture_aspect = 4.0f / 3.0f;
      if (viewport_width / viewport_height > capture_aspect)
      {
        const float content_width = viewport_height * capture_aspect;
        viewport_x = (viewport_width - content_width) * 0.5f;
        viewport_width = content_width;
      }
      else
      {
        const float content_height = viewport_width / capture_aspect;
        viewport_y = (viewport_height - content_height) * 0.5f;
        viewport_height = content_height;
      }
    }
    r.gl->glViewport(static_cast<GLint>(viewport_x), static_cast<GLint>(viewport_y),
                     static_cast<GLsizei>(viewport_width), static_cast<GLsizei>(viewport_height));
    r.gl->glDisable(GL_CULL_FACE);
    r.gl->glEnable(GL_BLEND);
    r.gl->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    r.gl->glUseProgram(r.triangle_program);
    const auto set_uniforms = [&](GLuint program, float alpha, int mode) {
      r.gl->glUniform4f(r.gl->glGetUniformLocation(program, "eye_radius"), s.eye[0], s.eye[1],
                        s.eye[2], s.radius);
      r.gl->glUniform4f(r.gl->glGetUniformLocation(program, "right_focal"), s.right[0], s.right[1],
                        s.right[2], s.focal);
      r.gl->glUniform4f(r.gl->glGetUniformLocation(program, "up_fill"), s.up[0], s.up[1], s.up[2],
                        s.fill_opacity);
      r.gl->glUniform4f(r.gl->glGetUniformLocation(program, "forward_wire"), s.forward[0],
                        s.forward[1], s.forward[2], s.wire_opacity);
      r.gl->glUniform2f(r.gl->glGetUniformLocation(program, "viewport_size"), viewport_width,
                        viewport_height);
      r.gl->glUniform1f(r.gl->glGetUniformLocation(program, "alpha_scale"), alpha);
      const GLint mode_location = r.gl->glGetUniformLocation(program, "mode");
      if (mode_location >= 0)
        r.gl->glUniform1i(mode_location, mode);
    };
    const auto draw_triangles = [&](size_t first, size_t last) {
      r.gl->glBindVertexArray(r.mesh_vao);
      for (size_t i = first; i < last; ++i)
      {
        if (!r.vertex_buffers[i] || r.vertex_counts[i] == 0)
          continue;
        r.gl->glBindBuffer(GL_ARRAY_BUFFER, r.vertex_buffers[i]);
        r.gl->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(API::Gui::HardwareVertex), nullptr);
        r.gl->glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                                    sizeof(API::Gui::HardwareVertex),
                                    reinterpret_cast<void*>(offsetof(API::Gui::HardwareVertex, color)));
        r.gl->glDrawArrays(GL_TRIANGLES, 0, r.vertex_counts[i]);
      }
    };
    if (!s.xray)
    {
      r.gl->glEnable(GL_DEPTH_TEST);
      r.gl->glDepthFunc(GL_LEQUAL);
      r.gl->glDepthMask(GL_TRUE);
      r.gl->glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
      set_uniforms(r.triangle_program, 1.0f, 0);
      draw_triangles(0, 5);
      r.gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }
    if (s.xray)
      r.gl->glDisable(GL_DEPTH_TEST);
    else
    {
      r.gl->glEnable(GL_DEPTH_TEST);
      r.gl->glDepthMask(GL_FALSE);
    }
    if (s.filled && s.fill_opacity > 0.0f)
    {
      r.gl->glUseProgram(r.triangle_program);
      set_uniforms(r.triangle_program, s.fill_opacity, 0);
      draw_triangles(0, 5);
    }
    // Group 7 is a depth-writing fill-only overlay in normal mode. Rendering
    // it before scene wire prevents unrelated triangle edges appearing inside
    // translucent scripted volumes; scripts submit their desired outer edges
    // separately through group 5.
    if (r.vertex_buffers[7] && r.vertex_counts[7] != 0 && s.fill_opacity > 0.0f &&
        !(s.debug_on_top || s.xray))
    {
      r.gl->glEnable(GL_DEPTH_TEST);
      r.gl->glDepthFunc(GL_LEQUAL);
      r.gl->glDepthMask(GL_TRUE);
      r.gl->glUseProgram(r.triangle_program);
      set_uniforms(r.triangle_program, s.fill_opacity, 0);
      draw_triangles(7, 8);
      r.gl->glDepthMask(GL_FALSE);
    }
    if (s.wireframe && s.wire_opacity > 0.0f)
    {
      r.gl->glUseProgram(r.triangle_program);
      set_uniforms(r.triangle_program, s.wire_opacity, 1);
      draw_triangles(0, 5);
    }
    if (r.vertex_buffers[7] && r.vertex_counts[7] != 0 && s.fill_opacity > 0.0f &&
        (s.debug_on_top || s.xray))
    {
      if (s.debug_on_top || s.xray)
        r.gl->glDisable(GL_DEPTH_TEST);
      r.gl->glUseProgram(r.triangle_program);
      set_uniforms(r.triangle_program, s.fill_opacity, 0);
      draw_triangles(7, 8);
    }
    if (r.vertex_buffers[5] && r.vertex_counts[5] != 0)
    {
      if (s.debug_on_top || s.xray)
        r.gl->glDisable(GL_DEPTH_TEST);
      r.gl->glUseProgram(r.line_program);
      set_uniforms(r.line_program, s.wire_opacity, 2);
      r.gl->glBindVertexArray(r.mesh_vao);
      r.gl->glBindBuffer(GL_ARRAY_BUFFER, r.vertex_buffers[5]);
      r.gl->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(API::Gui::HardwareVertex), nullptr);
      r.gl->glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(API::Gui::HardwareVertex),
                                  reinterpret_cast<void*>(offsetof(API::Gui::HardwareVertex, color)));
      r.gl->glDrawArrays(GL_LINES, 0, r.vertex_counts[5]);
    }
    if (r.vertex_buffers[6] && r.vertex_counts[6] != 0)
    {
      r.gl->glDisable(GL_DEPTH_TEST);
      r.gl->glUseProgram(r.triangle_program);
      set_uniforms(r.triangle_program, 1.0f, 0);
      draw_triangles(6, 7);
    }

    if (!s.clean_capture && m_snapshot.hud)
    {
      const bool overlay_changed = !r.overlay_valid || r.overlay_size != QSize(pixel_width, pixel_height) ||
                                   r.overlay_hud != m_snapshot.hud;
      if (overlay_changed)
      {
        QImage overlay(QSize(pixel_width, pixel_height), QImage::Format_RGBA8888);
        overlay.fill(Qt::transparent);
        QPainter painter(&overlay);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);
        painter.setFont(QFont(QStringLiteral("Consolas"), 14));
        const qreal dpr = devicePixelRatioF();
        painter.scale(dpr, dpr);
        const auto color = [](u32 argb) {
          return QColor((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, (argb >> 24) & 0xFF);
        };
        for (const API::Gui::CanvasPrimitive& p : *m_snapshot.hud)
        {
          const QColor c = color(p.color);
          switch (p.type)
          {
          case API::Gui::CanvasPrimitive::Type::Line:
            painter.setPen(QPen(c, p.thickness)); painter.drawLine(QPointF(p.p0.x, p.p0.y), QPointF(p.p1.x, p.p1.y)); break;
          case API::Gui::CanvasPrimitive::Type::Rect:
            painter.setPen(QPen(c, p.thickness)); painter.setBrush(Qt::NoBrush); painter.drawRoundedRect(QRectF(p.p0.x, p.p0.y, p.p1.x - p.p0.x, p.p1.y - p.p0.y), p.rounding, p.rounding); break;
          case API::Gui::CanvasPrimitive::Type::RectFilled:
            painter.setPen(Qt::NoPen); painter.setBrush(c); painter.drawRoundedRect(QRectF(p.p0.x, p.p0.y, p.p1.x - p.p0.x, p.p1.y - p.p0.y), p.rounding, p.rounding); break;
          case API::Gui::CanvasPrimitive::Type::Circle:
            painter.setPen(QPen(c, p.thickness)); painter.setBrush(Qt::NoBrush); painter.drawEllipse(QPointF(p.p0.x, p.p0.y), p.radius, p.radius); break;
          case API::Gui::CanvasPrimitive::Type::CircleFilled:
            painter.setPen(Qt::NoPen); painter.setBrush(c); painter.drawEllipse(QPointF(p.p0.x, p.p0.y), p.radius, p.radius); break;
          case API::Gui::CanvasPrimitive::Type::Triangle:
            painter.setPen(QPen(c, p.thickness)); painter.setBrush(Qt::NoBrush); painter.drawPolygon(QPolygonF{QPointF(p.p0.x, p.p0.y), QPointF(p.p1.x, p.p1.y), QPointF(p.p2.x, p.p2.y)}); break;
          case API::Gui::CanvasPrimitive::Type::TriangleFilled:
            painter.setPen(Qt::NoPen); painter.setBrush(c); painter.drawPolygon(QPolygonF{QPointF(p.p0.x, p.p0.y), QPointF(p.p1.x, p.p1.y), QPointF(p.p2.x, p.p2.y)}); break;
          case API::Gui::CanvasPrimitive::Type::Text:
            painter.setPen(c);
            painter.drawText(QPointF(p.p0.x, p.p0.y + QFontMetricsF(painter.font()).ascent()),
                             QString::fromUtf8(p.text.c_str()));
            break;
          default:
            break;
          }
        }
        painter.end();
        r.gl->glBindTexture(GL_TEXTURE_2D, r.overlay_texture);
        r.gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        r.gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pixel_width, pixel_height, 0, GL_RGBA,
                           GL_UNSIGNED_BYTE, overlay.constBits());
        r.overlay_size = QSize(pixel_width, pixel_height);
        r.overlay_hud = m_snapshot.hud;
        r.overlay_valid = true;
      }
      // QImage stores its first row at the top while OpenGL texture row zero
      // is the bottom. Flip V here without changing world-space projection.
      const float quad[] = {-1.0f, -1.0f, 0.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f,
                            -1.0f, 1.0f, 0.0f, 0.0f,  1.0f, 1.0f, 1.0f, 0.0f};
      r.gl->glViewport(0, 0, pixel_width, pixel_height);
      r.gl->glDisable(GL_DEPTH_TEST);
      r.gl->glUseProgram(r.overlay_program);
      r.gl->glActiveTexture(GL_TEXTURE0);
      r.gl->glBindTexture(GL_TEXTURE_2D, r.overlay_texture);
      r.gl->glUniform1i(r.gl->glGetUniformLocation(r.overlay_program, "overlay_texture"), 0);
      r.gl->glBindVertexArray(r.overlay_vao);
      r.gl->glBindBuffer(GL_ARRAY_BUFFER, r.overlay_vbo);
      r.gl->glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STREAM_DRAW);
      r.gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
  }
  Present();
  r.context->doneCurrent();
}

void ScriptHardwareMeshWidget::ReleaseResources()
{
  m_ready = false;
  if (!m_resources)
    return;
  if (m_resources->context && m_gl_window && m_resources->context->makeCurrent(m_gl_window))
  {
    auto& r = *m_resources;
    for (GLuint& buffer : r.vertex_buffers)
    {
      if (buffer)
        r.gl->glDeleteBuffers(1, &buffer);
    }
    if (r.mesh_vao)
      r.gl->glDeleteVertexArrays(1, &r.mesh_vao);
    if (r.overlay_vbo)
      r.gl->glDeleteBuffers(1, &r.overlay_vbo);
    if (r.overlay_vao)
      r.gl->glDeleteVertexArrays(1, &r.overlay_vao);
    if (r.overlay_texture)
      r.gl->glDeleteTextures(1, &r.overlay_texture);
    if (r.triangle_program)
      r.gl->glDeleteProgram(r.triangle_program);
    if (r.line_program)
      r.gl->glDeleteProgram(r.line_program);
    if (r.overlay_program)
      r.gl->glDeleteProgram(r.overlay_program);
    r.context->doneCurrent();
  }
  m_resources.reset();
}

#endif
