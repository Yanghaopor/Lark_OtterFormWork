#pragma once
#ifndef _WIN32
#error "OtterRenderer.h is the Direct2D renderer backend and is only available on Windows. Use a platform renderer implementation on non-Windows platforms."
#endif
// ============================================================
//  OtterRenderer.h
//  水獭图形框架 —— Direct2D 渲染后端
//
//  实现 OtterLayer.h 中定义的 RenderContext 纯虚接口，
//  将 PaintChain 的绘制指令翻译为 Direct2D API 调用。
//
//  核心职责：
//    1. 管理 D2D 工厂（ID2D1Factory）和渲染目标（HwndRenderTarget）
//    2. 维护路径构建状态机（PathGeometry + GeometrySink）
//    3. 维护变换矩阵栈（支持图层嵌套变换）
//    4. 将 Otter 颜色/样式转换为 D2D 对应类型
//
//  路径状态机说明：
//    D2D 的路径是"不可变几何体"：必须先用 GeometrySink 构建，
//    Close() 后才能用于绘制，绘制后释放。
//    因此内部维护 current_geometry_ / current_sink_ 来积累路径，
//    在 fill() / stroke() 时一次性提交。
//
//  阴影实现：
//    D2D 1.0（HwndRenderTarget）不支持原生模糊滤镜，
//    此处采用"偏移叠影"近似实现（多层半透明 offset 绘制）。
//    如需真正高斯模糊阴影，需升级至 D2D 1.1 (ID2D1DeviceContext)，
//    在后续版本中可替换 apply_shadow_* 函数实现。
//
//  命名空间：Otter
//  C++ 标准：C++20
//  依赖库：d2d1.lib（已通过 #pragma comment 自动链接）
// ============================================================

#include "OtterLayer.h"

// Windows / Direct2D 头文件
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d2d1.h>
#include <d2d1helper.h>      // D2D1::ColorF, D2D1::Point2F 等便捷函数

// DirectWrite —— 文字排版与渲染
#include <dwrite.h>          // DWrite 1.0（基础文字渲染）
#include <dwrite_1.h>        // DWrite 1.1（字符间距等扩展）

// Windows Imaging Component —— 图片解码（PNG / JPG / BMP 等）
#include <wincodec.h>
#pragma comment(lib, "windowscodecs")

// WinHTTP —— HTTP 图片下载
#include <winhttp.h>
#pragma comment(lib, "winhttp")

// 自动链接库，无需在项目设置中手动添加
#pragma comment(lib, "d2d1")
#pragma comment(lib, "dwrite")

#include <cmath>
#include <vector>
#include <optional>
#include <unordered_map>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <queue>
#include <set>

// ─── 数学常量（MSVC 默认不定义 M_PI）────────────────────────
#ifndef OTTER_PI
#define OTTER_PI 3.14159265358979323846f
#endif

namespace Otter
{
    // ============================================================
    //  辅助：将 Otter::Color 转换为 D2D1_COLOR_F
    // ============================================================
    inline D2D1_COLOR_F to_d2d_color(const Color& c, float opacity_multiplier = 1.f)
    {
        return D2D1::ColorF(c.r, c.g, c.b, c.a * opacity_multiplier);
    }


    // ============================================================
    //  D2DRenderContext
    //
    //  实现 Otter::RenderContext 接口的 Direct2D 渲染上下文。
    //  由 otterwindow 创建并持有，每帧在 begin_frame() / end_frame()
    //  之间被 Layer::flush() 调用。
    // ============================================================
    class D2DRenderContext : public RenderContext
    {
    public:
        D2DRenderContext()  = default;
        ~D2DRenderContext() noexcept { release_all(); }

        // 不可拷贝（持有 COM 资源）
        D2DRenderContext(const D2DRenderContext&)            = delete;
        D2DRenderContext& operator=(const D2DRenderContext&) = delete;

        // ── 颜色键模式 ─────────────────────────────────────────────
        void set_color_key_mode(bool enable) { color_key_mode_ = enable; }
        bool is_color_key_mode() const noexcept { return color_key_mode_; }

        // 转换颜色（颜色键模式下纯黑转近黑）
        Color convert_color(const Color& c) const
        {
            if (color_key_mode_ && c.is_pure_black())
                return c.to_near_black();
            return c;
        }

        // ── 初始化 / 销毁 ────────────────────────────────────────

        void initialize_native(void* hwnd, unsigned int width, unsigned int height) override
        {
            initialize(static_cast<HWND>(hwnd), width, height);
        }

        // 在 HWND 创建后调用，初始化 D2D 工厂和渲染目标
        // 渲染目标固定 96 DPI：1 DIP = 1 物理像素，坐标直接用 px，无任何缩放
        void initialize(HWND hwnd, UINT width, UINT height)
        {
            CoInitialize(nullptr);  // 初始化 COM（WIC 需要，可重复调用无副作用）

            HRESULT hr;

            // 创建 D2D 工厂（单线程模式，主线程渲染）
            hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory_);
            check_hr(hr, "D2D1CreateFactory 失败");

            // 固定 96 DPI：D2D 坐标单位 = 物理像素，无 DPI 缩放干扰
            D2D1_RENDER_TARGET_PROPERTIES rt_props =
                D2D1::RenderTargetProperties(
                    D2D1_RENDER_TARGET_TYPE_DEFAULT,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                      D2D1_ALPHA_MODE_PREMULTIPLIED),
                    96.f, 96.f);

            D2D1_HWND_RENDER_TARGET_PROPERTIES hwnd_props =
                D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(width, height));

            hr = factory_->CreateHwndRenderTarget(rt_props, hwnd_props, &render_target_);
            check_hr(hr, "CreateHwndRenderTarget 失败");

            // 矢量图形：亚像素抗锯齿
            render_target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

            // ── 文字默认使用 ClearType ──────────────────────────
            // 比默认的 DEFAULT 模式更清晰，是静态水平文字的最佳选择。
            // 动态/旋转文字可通过 TextStyle::render_mode 切换为 GrayScale。
            render_target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

            // 初始化变换栈
            transform_stack_.push_back(D2D1::Matrix3x2F::Identity());

            // ── 初始化 DirectWrite 工厂 ──────────────────────────
            hr = DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(&dwrite_factory_));
            check_hr(hr, "DWriteCreateFactory 失败");

            // 升级到 DWrite 1.1（字符间距等扩展，Win8+ 可用）
            dwrite_factory_->QueryInterface(__uuidof(IDWriteFactory1),
                                             reinterpret_cast<void**>(&dwrite_factory1_));

            // ── 配置高质量文字渲染参数 ──────────────────────────
            // 使用 NATURAL_SYMMETRIC 渲染模式：
            //   · 比默认的 ALIASED / DEFAULT 更清晰
            //   · 对称处理提升小字号可读性
            //   · 是 Chrome / WPF / VS 等使用的模式
            setup_rendering_params();

            // ── 启动异步 HTTP 图片下载线程 ────────────────────────
            download_hwnd_ = hwnd;
            download_running_ = true;
            download_thread_ = std::thread([this]() { download_worker(); });
        }

        // 窗口尺寸变化时调用（WM_SIZE 消息处理后）
        void resize(UINT width, UINT height) override
        {
            if (render_target_)
            {
                render_target_->Resize(D2D1::SizeU(width, height));

                // 不在这里清除，让调用方在 WM_SIZE 后立即调用 begin_frame/end_frame
                // 这样可以确保使用正确的清除颜色
            }
        }

        // 设置 resize 时的临时清除颜色（应与主循环的 clear_color 一致）
        void set_resize_clear_color(const Color& c) override
        {
            resize_clear_color_ = c;
        }

        // ── 帧生命周期 ────────────────────────────────────────────

        // 每帧开始时调用，清除背景色，进入绘制状态
        void begin_frame(Color clear_color = Color{ 0.08f, 0.08f, 0.1f, 1.f }) override
        {
            render_target_->BeginDraw();
            render_target_->Clear(to_d2d_color(clear_color));

            // 重置为单位矩阵。
            // 渲染目标已用实际 DPI 创建，DirectWrite 自动以该 DPI 输出清晰文字。
            // 所有坐标使用物理像素，不需要额外 Scale 变换。
            render_target_->SetTransform(D2D1::Matrix3x2F::Identity());
            transform_stack_[0] = D2D1::Matrix3x2F::Identity();
        }

        // 每帧结束时调用，提交绘制结果到屏幕
        // 返回 false 表示设备丢失（需重新初始化，如窗口最小化恢复）
        bool end_frame() override
        {
            HRESULT hr = render_target_->EndDraw();
            if (hr == D2DERR_RECREATE_TARGET)
            {
                // 设备丢失，释放资源（下帧重建）
                release_render_resources();
                return false;
            }
            return SUCCEEDED(hr);
        }

        // ── RenderContext 接口实现 ───────────────────────────────

        // 压入变换矩阵：平移(tx,ty) × 缩放(sx,sy) × 旋转(rot 弧度)
        // 与当前变换相乘，形成嵌套变换
        // tx/ty 已经是物理像素坐标（用户代码用物理像素），不需要缩放
        void push_transform(float tx, float ty,
                            float sx, float sy,
                            float rot) override
        {
            // 构建本层的局部变换矩阵（旋转 → 缩放 → 平移）
            float cos_r = cosf(rot), sin_r = sinf(rot);

            D2D1::Matrix3x2F local =
                D2D1::Matrix3x2F(
                    sx * cos_r,  sx * sin_r,   // M11, M12
                    sy * -sin_r, sy * cos_r,   // M21, M22
                    tx,          ty);           // Dx,  Dy

            // 与当前累积变换相乘
            D2D1::Matrix3x2F combined = local * transform_stack_.back();

            transform_stack_.push_back(combined);
            render_target_->SetTransform(combined);
        }

        // 弹出变换矩阵，恢复到父层变换
        void pop_transform() override
        {
            if (transform_stack_.size() > 1)
                transform_stack_.pop_back();

            render_target_->SetTransform(transform_stack_.back());
        }

        // 压入图层样式（透明度、混合模式）
        void push_style(const LayerStyle& style) override
        {
            float parent_opacity = style_stack_.empty() ? 1.f
                                                        : style_stack_.back().effective_opacity;
            StyleEntry entry;
            entry.style            = style;
            entry.effective_opacity = parent_opacity * style.opacity;  // 透明度继承
            style_stack_.push_back(entry);
        }

        // 弹出图层样式
        void pop_style() override
        {
            if (!style_stack_.empty())
                style_stack_.pop_back();
        }

        // ── 路径构建指令 ─────────────────────────────────────────

        // 移动画笔到 (x,y)，不绘制线条
        // 若当前有未关闭的 figure，先以 open 方式结束它，再开新 figure
        void cmd_move_to(float x, float y) override
        {
            ensure_geometry();

            if (figure_open_)
            {
                // 结束当前 figure（不闭合，即 open 端点）
                current_sink_->EndFigure(D2D1_FIGURE_END_OPEN);
                figure_open_ = false;
            }

            // 在此位置开始新 figure（FILLED = 可以填充内部）
            current_sink_->BeginFigure(
                D2D1::Point2F(x, y),
                D2D1_FIGURE_BEGIN_FILLED);
            figure_open_ = true;
            last_point_  = D2D1::Point2F(x, y);
        }

        // 从当前点画直线到 (x,y)
        void cmd_line_to(float x, float y) override
        {
            if (!figure_open_) return;  // 必须先 move_to
            current_sink_->AddLine(D2D1::Point2F(x, y));
            last_point_ = D2D1::Point2F(x, y);
        }

        // 三次贝塞尔曲线（两控制点 + 终点）
        void cmd_bezier_to(float cx1, float cy1,
                           float cx2, float cy2,
                           float ex,  float ey) override
        {
            if (!figure_open_) return;
            current_sink_->AddBezier(
                D2D1::BezierSegment(
                    D2D1::Point2F(cx1, cy1),
                    D2D1::Point2F(cx2, cy2),
                    D2D1::Point2F(ex, ey)));
            last_point_ = D2D1::Point2F(ex, ey);
        }

        // 圆弧（中心 + 半径 + 起止角，角度单位：弧度）
        // D2D 弧段从当前点画到弧的终点，因此先移动到弧的起点
        void cmd_arc(float cx, float cy, float radius,
                     float start_angle, float end_angle) override
        {
            ensure_geometry();

            // 计算弧的起点和终点（世界坐标）
            float sx = cx + radius * cosf(start_angle);
            float sy = cy + radius * sinf(start_angle);
            float ex = cx + radius * cosf(end_angle);
            float ey = cy + radius * sinf(end_angle);

            if (!figure_open_)
            {
                // 没有当前路径，从弧起点开始新 figure
                current_sink_->BeginFigure(D2D1::Point2F(sx, sy),
                                           D2D1_FIGURE_BEGIN_FILLED);
                figure_open_ = true;
            }
            else
            {
                // 已有路径，先画直线连到弧起点（与 Canvas API 行为一致）
                current_sink_->AddLine(D2D1::Point2F(sx, sy));
            }

            // 计算扫过角度，决定方向和大弧/小弧
            float sweep = end_angle - start_angle;
            D2D1_SWEEP_DIRECTION dir = (sweep >= 0.f)
                ? D2D1_SWEEP_DIRECTION_CLOCKWISE
                : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE;

            D2D1_ARC_SIZE arc_size = (fabsf(sweep) > OTTER_PI)
                ? D2D1_ARC_SIZE_LARGE
                : D2D1_ARC_SIZE_SMALL;

            current_sink_->AddArc(
                D2D1::ArcSegment(
                    D2D1::Point2F(ex, ey),        // 弧终点
                    D2D1::SizeF(radius, radius),  // X/Y 半径（圆形取相等值）
                    0.f,                          // 旋转角（椭圆弧用，圆形为0）
                    dir,
                    arc_size));

            last_point_ = D2D1::Point2F(ex, ey);
        }

        // 闭合当前路径（连回当前 figure 起点）
        void cmd_close_path() override
        {
            if (!figure_open_) return;
            current_sink_->EndFigure(D2D1_FIGURE_END_CLOSED);
            figure_open_ = false;
        }

        // ── 绘制指令 ─────────────────────────────────────────────

        // 填充当前路径
        void cmd_fill(const FillStyle& style) override
        {
            ID2D1PathGeometry* geom = commit_geometry();
            if (!geom) return;

            float opacity = effective_opacity();

            // 若有待处理阴影，先绘制阴影
            if (pending_shadow_)
                apply_shadow_fill(geom, *pending_shadow_, opacity);

            // 根据类型创建画笔
            ID2D1Brush* brush = nullptr;

            switch (style.type)
            {
            case FillStyle::Type::Solid:
                {
                    ID2D1SolidColorBrush* solid = make_brush(style.color, opacity);
                    brush = solid;
                }
                break;

            case FillStyle::Type::Linear:
                if (style.linear)
                    brush = make_linear_gradient_brush(*style.linear, opacity);
                break;

            case FillStyle::Type::Radial:
                if (style.radial)
                    brush = make_radial_gradient_brush(*style.radial, opacity);
                break;

            case FillStyle::Type::Conic:
                if (style.conic) {
                    // 锥形渐变：三角扇形近似，使用路径几何体裁切
                    draw_conic_gradient(*style.conic, opacity, geom);
                    geom->Release();
                    pending_shadow_.reset();
                    return;  // 已直接绘制，跳过下面的 brush 路径
                }
                break;
            }

            if (brush)
            {
                render_target_->FillGeometry(geom, brush);
                brush->Release();
            }

            geom->Release();
            pending_shadow_.reset(); // 阴影只作用于下一次 fill/stroke
        }

        // 描边当前路径
        void cmd_stroke(const StrokeStyle& style) override
        {
            ID2D1PathGeometry* geom = commit_geometry();
            if (!geom) return;

            float opacity = effective_opacity();

            // 若有待处理阴影，先绘制阴影
            if (pending_shadow_)
                apply_shadow_stroke(geom, style, *pending_shadow_, opacity);

            // 创建描边画笔并绘制
            ID2D1SolidColorBrush* brush = make_brush(style.color, opacity);
            if (brush)
            {
                render_target_->DrawGeometry(geom, brush, style.width);
                brush->Release();
            }

            geom->Release();
            pending_shadow_.reset();
        }

        // 设置阴影（在下一次 fill/stroke 之前调用生效）
        void cmd_shadow(const ShadowStyle& style) override
        {
            pending_shadow_ = style;
        }

        // ── CSS 效果指令实现 ──────────────────────────────────────

        // 填充圆角矩形（radius=0 降级为普通矩形，性能更好）
        void cmd_fill_round_rect(float x, float y, float w, float h,
                                  float radius, const FillStyle& style) override
        {
            if (!render_target_) return;
            float op = effective_opacity();

            // 根据类型创建画笔
            ID2D1Brush* brush = nullptr;
            switch (style.type)
            {
            case FillStyle::Type::Solid:
                brush = make_brush(style.color, op);
                break;
            case FillStyle::Type::Linear:
                if (style.linear)
                    brush = make_linear_gradient_brush(*style.linear, op);
                break;
            case FillStyle::Type::Radial:
                if (style.radial)
                    brush = make_radial_gradient_brush(*style.radial, op);
                break;
            case FillStyle::Type::Conic:
                if (style.conic) {
                    // 锥形渐变填充圆角矩形：先建立裁切几何体再画扇形
                    if (radius > 0.f) {
                        ID2D1RoundedRectangleGeometry* clip_geom = nullptr;
                        if (SUCCEEDED(factory_->CreateRoundedRectangleGeometry(
                                D2D1::RoundedRect(D2D1::RectF(x, y, x+w, y+h), radius, radius),
                                &clip_geom)) && clip_geom) {
                            draw_conic_gradient(*style.conic, op, clip_geom);
                            clip_geom->Release();
                        }
                    } else {
                        // 无圆角：用 PushAxisAlignedClip 裁切
                        render_target_->PushAxisAlignedClip(
                            D2D1::RectF(x, y, x+w, y+h),
                            D2D1_ANTIALIAS_MODE_ALIASED);
                        draw_conic_gradient(*style.conic, op, nullptr);
                        render_target_->PopAxisAlignedClip();
                    }
                    return;
                }
                break;
            }
            if (!brush) return;

            if (radius > 0.f)
            {
                ID2D1RoundedRectangleGeometry* geom = nullptr;
                HRESULT hr = factory_->CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(D2D1::RectF(x, y, x+w, y+h), radius, radius),
                    &geom);
                if (SUCCEEDED(hr) && geom)
                {
                    render_target_->FillGeometry(geom, brush);
                    geom->Release();
                }
            }
            else
            {
                render_target_->FillRectangle(D2D1::RectF(x, y, x+w, y+h), brush);
            }
            brush->Release();
        }

        // 描边圆角矩形
        void cmd_stroke_round_rect(float x, float y, float w, float h,
                                    float radius,
                                    const StrokeStyle& style) override
        {
            if (!render_target_) return;
            float op = effective_opacity();
            ID2D1SolidColorBrush* brush = make_brush(style.color, op);
            if (!brush) return;

            if (radius > 0.f)
            {
                ID2D1RoundedRectangleGeometry* geom = nullptr;
                HRESULT hr = factory_->CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(D2D1::RectF(x, y, x+w, y+h), radius, radius),
                    &geom);
                if (SUCCEEDED(hr) && geom)
                {
                    render_target_->DrawGeometry(geom, brush, style.width);
                    geom->Release();
                }
            }
            else
            {
                render_target_->DrawRectangle(D2D1::RectF(x, y, x+w, y+h),
                                               brush, style.width);
            }
            brush->Release();
        }

        // 压入圆角裁切区域
        // radius=0 → 轴对齐矩形裁切（高效）
        // radius>0 → 几何体 PushLayer 裁切（支持圆角，稍慢）
        void cmd_push_round_clip(float x, float y, float w, float h,
                                  float radius) override
        {
            if (!render_target_) return;

            if (radius <= 0.f)
            {
                // 普通矩形裁切
                render_target_->PushAxisAlignedClip(
                    D2D1::RectF(x, y, x+w, y+h),
                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                clip_stack_.push_back(ClipEntry{ nullptr, false });
            }
            else
            {
                // 圆角几何体裁切（通过 PushLayer + 几何遮罩实现）
                ID2D1RoundedRectangleGeometry* geom = nullptr;
                HRESULT hr = factory_->CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(D2D1::RectF(x, y, x+w, y+h), radius, radius),
                    &geom);
                if (FAILED(hr) || !geom)
                {
                    // 几何体创建失败，降级为轴对齐裁切
                    render_target_->PushAxisAlignedClip(
                        D2D1::RectF(x, y, x+w, y+h),
                        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    clip_stack_.push_back(ClipEntry{ nullptr, false });
                    return;
                }

                ID2D1Layer* layer = nullptr;
                hr = render_target_->CreateLayer(&layer);
                if (FAILED(hr) || !layer)
                {
                    geom->Release();
                    // 降级
                    render_target_->PushAxisAlignedClip(
                        D2D1::RectF(x, y, x+w, y+h),
                        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    clip_stack_.push_back(ClipEntry{ nullptr, false });
                    return;
                }

                render_target_->PushLayer(
                    D2D1::LayerParameters(
                        D2D1::InfiniteRect(),   // 内容边界（无限大，几何体已限制）
                        geom,                   // 几何遮罩
                        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                        D2D1::IdentityMatrix(),
                        1.0f,
                        nullptr,
                        D2D1_LAYER_OPTIONS_NONE),
                    layer);

                geom->Release();  // PushLayer 已持有引用
                clip_stack_.push_back(ClipEntry{ layer, true });
            }
        }

        // 弹出最近一次压入的裁切区域
        void cmd_pop_clip() override
        {
            if (!render_target_ || clip_stack_.empty()) return;

            ClipEntry& entry = clip_stack_.back();
            if (entry.is_layer && entry.layer)
            {
                render_target_->PopLayer();
                entry.layer->Release();
            }
            else
            {
                render_target_->PopAxisAlignedClip();
            }
            clip_stack_.pop_back();
        }

        // 近似高斯模糊（D2D 1.0：在指定区域绘制多层扩散半透明矩形叠加）
        // 注：真正的 Gaussian blur 需 D2D 1.1 ID2D1Effect，此为降级近似。
        // 效果：在内容之上覆盖多层从边缘向内逐渐透明的白色/灰色矩形，
        //       产生"磨砂/柔化"视觉效果（而非真正的像素模糊）。
        void cmd_blur_rect(float x, float y, float w, float h,
                            float radius) override
        {
            if (!render_target_ || radius <= 0.f) return;

            float op = effective_opacity();
            constexpr int LAYERS = 5;

            for (int i = 0; i < LAYERS; ++i) {
                float t = (float)i / (LAYERS - 1);      // 0 → 1
                float alpha = (1.f - t) * 0.12f * op;   // 边缘最不透明，中心渐淡
                float inset = radius * t * 0.5f;         // 从边缘向内收缩

                float fx = x + inset, fy = y + inset;
                float fw = w - inset * 2.f, fh = h - inset * 2.f;
                if (fw <= 0.f || fh <= 0.f) break;

                // 半透明白色覆盖层（模拟光散射/柔化）
                Color overlay{ 1.f, 1.f, 1.f, alpha };
                ID2D1SolidColorBrush* brush = make_brush(overlay, 1.f);
                if (!brush) continue;
                render_target_->FillRectangle(D2D1::RectF(fx, fy, fx+fw, fy+fh), brush);
                brush->Release();
            }
        }

        // 边缘羽化：在矩形四周绘制渐变透明边框（模拟软遮罩）
        void cmd_feather_rect(float x, float y, float w, float h,
                               float radius, float feather) override
        {
            if (!render_target_ || feather <= 0.f) return;

            // 用多层半透明圆角矩形，从外到内逐渐透明 → 不透明
            constexpr int STEPS = 6;
            float op = effective_opacity();

            for (int i = 0; i < STEPS; ++i)
            {
                float t    = (float)i / (float)(STEPS - 1); // 0.0 ~ 1.0
                float inset = feather * (1.f - t);           // 从外向内收缩
                float alpha = t * op * 0.9f;                 // 逐渐不透明

                float fx = x + inset, fy = y + inset;
                float fw = w - inset * 2.f, fh = h - inset * 2.f;
                if (fw <= 0 || fh <= 0) break;

                // 反向：用透明矩形"擦除"边缘（近似羽化）
                Color fade_col{ 0.f, 0.f, 0.f, (1.f - t) * 0.6f * op };
                ID2D1SolidColorBrush* brush = make_brush(fade_col, 1.f);
                if (!brush) continue;

                if (radius > 0.f)
                {
                    ID2D1RoundedRectangleGeometry* geom = nullptr;
                    if (SUCCEEDED(factory_->CreateRoundedRectangleGeometry(
                        D2D1::RoundedRect(D2D1::RectF(fx, fy, fx+fw, fy+fh),
                                           radius, radius), &geom)) && geom)
                    {
                        render_target_->FillGeometry(geom, brush);
                        geom->Release();
                    }
                }
                else
                {
                    render_target_->FillRectangle(
                        D2D1::RectF(fx, fy, fx+fw, fy+fh), brush);
                }
                brush->Release();
            }
        }

        // ── 文字渲染（DirectWrite）公开接口 ─────────────────────
        // cmd_text / cmd_measure_text 实现了 RenderContext 的纯虚函数，
        // 必须放在 public 区（覆盖基类的 public 虚函数）。

        // 绘制文本（渲染顺序：阴影 → 描边 → 填充）
        //
        // 坐标体系说明：
        //   渲染目标固定 96 DPI，1 DIP = 1 物理像素
        //   x/y 是局部物理像素坐标，transform_stack_ 也是物理像素变换
        //   字号单位 = 物理像素（px），传多少渲染多少，无任何 DPI 缩放
        void cmd_text(const std::wstring& content,
                       float x, float y,
                       float max_width, float max_height,
                       const TextStyle& style) override
        {
            if (!render_target_ || !dwrite_factory_ || content.empty()) return;

            // 1. 获取或创建 TextFormat
            IDWriteTextFormat* fmt = get_text_format(style);
            if (!fmt) return;

            float w = max_width  > 0.f ? max_width  : 100000.f;
            float h = max_height > 0.f ? max_height : 100000.f;

            // 2. 创建 TextLayout
            IDWriteTextLayout* layout = nullptr;
            HRESULT hr = dwrite_factory_->CreateTextLayout(
                content.c_str(), (UINT32)content.size(),
                fmt, w, h, &layout);
            if (FAILED(hr) || !layout) return;

            DWRITE_TEXT_RANGE all{ 0, (UINT32)content.size() };

            // 3. 对齐
            layout->SetTextAlignment(to_dwrite_h_align(style.h_align));
            layout->SetParagraphAlignment(to_dwrite_v_align(style.v_align));
            layout->SetWordWrapping(style.word_wrap
                ? DWRITE_WORD_WRAPPING_WRAP
                : DWRITE_WORD_WRAPPING_NO_WRAP);

            // 4. 装饰
            if (style.underline)     layout->SetUnderline(TRUE,  all);
            if (style.strikethrough) layout->SetStrikethrough(TRUE, all);

            // 5. 字符间距
            if (style.letter_spacing != 0.f)
            {
                IDWriteTextLayout1* layout1 = nullptr;
                if (SUCCEEDED(layout->QueryInterface(__uuidof(IDWriteTextLayout1),
                                                     reinterpret_cast<void**>(&layout1)))
                    && layout1)
                {
                    layout1->SetCharacterSpacing(0.f, style.letter_spacing, 0.f, all);
                    layout1->Release();
                }
            }

            // 6. 行间距
            if (style.line_spacing > 0.f)
            {
                layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
                                       style.line_spacing, style.line_spacing * 0.8f);
            }

            // 7. 抗锯齿模式
            apply_render_mode(style.render_mode);

            float op = effective_opacity();

            // 变换矩阵本身就是物理像素空间，直接用当前变换绘制即可
            // 不需要切换到 Identity，也不需要手动换算世界坐标
            // 8. 阴影
            if (style.shadow.has_value())
            {
                const ShadowStyle& sh = *style.shadow;
                constexpr int SH_STEPS = 3;
                for (int i = SH_STEPS; i >= 1; --i)
                {
                    float t = (float)i / SH_STEPS;
                    float spread = sh.blur * (1.f - t) * 0.5f;
                    Color sh_col{ sh.color.r, sh.color.g, sh.color.b, sh.color.a * t * op };
                    ID2D1SolidColorBrush* sh_brush = make_brush(sh_col, 1.f);
                    if (sh_brush)
                    {
                        render_target_->DrawTextLayout(
                            D2D1::Point2F(x + sh.offset_x + spread,
                                          y + sh.offset_y + spread),
                            layout, sh_brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
                        sh_brush->Release();
                    }
                }
            }

            // 9. 描边
            if (style.stroke_width > 0.f && style.stroke_color.a > 0.f)
            {
                float sw = style.stroke_width;
                ID2D1SolidColorBrush* sk_brush = make_brush(style.stroke_color, op);
                if (sk_brush)
                {
                    const float dirs[8][2] = {
                        {-sw,0},{sw,0},{0,-sw},{0,sw},
                        {-sw,-sw},{sw,-sw},{-sw,sw},{sw,sw}
                    };
                    for (auto& d : dirs)
                    {
                        render_target_->DrawTextLayout(
                            D2D1::Point2F(x + d[0], y + d[1]),
                            layout, sk_brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
                    }
                    sk_brush->Release();
                }
            }

            // 10. 文字本体
            ID2D1SolidColorBrush* brush = make_brush(style.color, op);
            if (brush)
            {
                render_target_->DrawTextLayout(
                    D2D1::Point2F(x, y), layout, brush,
                    D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                brush->Release();
            }

            layout->Release();
            // 注意：不在这里重置抗锯齿模式，让用户设置的样式保持有效
            // 如果需要重置，应该在更高层级处理
        }

        // 测量文本尺寸（不绘制任何内容）
        bool cmd_measure_text(const std::wstring& content,
                               float max_width, float max_height,
                               const TextStyle& style,
                               TextMetrics& out) override
        {
            if (!dwrite_factory_ || content.empty()) return false;

            // 字号和尺寸都是 DIP，直接用，DWrite 自动按渲染目标 DPI 处理
            IDWriteTextFormat* fmt = get_text_format(style);
            if (!fmt) return false;

            float w = max_width  > 0.f ? max_width  : 100000.f;
            float h = max_height > 0.f ? max_height : 100000.f;

            IDWriteTextLayout* layout = nullptr;
            HRESULT hr = dwrite_factory_->CreateTextLayout(
                content.c_str(), (UINT32)content.size(), fmt, w, h, &layout);
            if (FAILED(hr) || !layout) return false;

            layout->SetWordWrapping(style.word_wrap
                ? DWRITE_WORD_WRAPPING_WRAP
                : DWRITE_WORD_WRAPPING_NO_WRAP);

            DWRITE_TEXT_METRICS metrics{};
            hr = layout->GetMetrics(&metrics);

            DWRITE_LINE_METRICS line_metrics{};
            UINT32 line_count = 0;
            layout->GetLineMetrics(&line_metrics, 1, &line_count);

            layout->Release();

            if (FAILED(hr)) return false;

            out.width       = metrics.widthIncludingTrailingWhitespace;
            out.height      = metrics.height;
            out.line_height = (line_count > 0) ? line_metrics.height : metrics.height;
            out.line_count  = (int)metrics.lineCount;
            out.truncated   = (max_width  > 0 && out.width  > max_width )
                           || (max_height > 0 && out.height > max_height);
            return true;
        }

        // 绘制图片（path=文件路径, x/y/w/h=目标矩形, opacity=不透明度, radius=圆角）
        void cmd_draw_bitmap(const std::wstring& path,
                             float x, float y, float w, float h,
                             float opacity, float radius) override
        {
            if (!render_target_) return;
            ID2D1Bitmap* bmp = load_bitmap(path);
            if (!bmp) return;

            float op = effective_opacity() * opacity;
            D2D1_RECT_F dst = D2D1::RectF(x, y, x + w, y + h);

            if (radius > 0.f)
            {
                cmd_push_round_clip(x, y, w, h, radius);
                render_target_->DrawBitmap(bmp, dst, op);
                cmd_pop_clip();
            }
            else
            {
                render_target_->DrawBitmap(bmp, dst, op);
            }
        }

        void cmd_draw_bitmap_bgra(const uint8_t* pixels,
                                  int width, int height, int stride,
                                  float x, float y, float w, float h,
                                  float opacity, float radius) override
        {
            if (!render_target_ || !pixels || width <= 0 || height <= 0 || stride < width * 4)
                return;

            D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.f,
                96.f);
            ID2D1Bitmap* bmp = nullptr;
            HRESULT hr = render_target_->CreateBitmap(
                D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(height)),
                pixels,
                static_cast<UINT32>(stride),
                props,
                &bmp);
            if (FAILED(hr) || !bmp)
                return;

            float op = effective_opacity() * std::clamp(opacity, 0.f, 1.f);
            D2D1_RECT_F dst = D2D1::RectF(x, y, x + w, y + h);

            if (radius > 0.f)
            {
                cmd_push_round_clip(x, y, w, h, radius);
                render_target_->DrawBitmap(bmp, dst, op);
                cmd_pop_clip();
            }
            else
            {
                render_target_->DrawBitmap(bmp, dst, op);
            }
            bmp->Release();
        }

    private:
        // ── D2D / DWrite 资源成员 ─────────────────────────────────
        ID2D1Factory*          factory_       = nullptr;
        ID2D1HwndRenderTarget* render_target_ = nullptr;
        bool                   color_key_mode_ = false;  // 颜色键透明模式
        Color                  resize_clear_color_ = Color{0.98f, 0.98f, 0.99f, 1.f};  // resize 时的清除颜色

        // ── DirectWrite 资源 ──────────────────────────────────────
        IDWriteFactory*  dwrite_factory_  = nullptr; // DWrite 工厂（进程共享）
        IDWriteFactory1* dwrite_factory1_ = nullptr; // DWrite 1.1（可能为 nullptr）

        // IDWriteTextFormat 缓存（按字体族+字号+字重+字形索引）
        struct TFKey
        {
            std::wstring family;
            float size; uint16_t weight; uint8_t fstyle;
            bool operator==(const TFKey& o) const noexcept
            { return family==o.family && size==o.size && weight==o.weight && fstyle==o.fstyle; }
        };
        struct TFHash {
            std::size_t operator()(const TFKey& k) const noexcept {
                std::size_t h = std::hash<std::wstring>{}(k.family);
                auto mix = [&](std::size_t v){ h ^= v+0x9e3779b9ull+(h<<6)+(h>>2); };
                mix(std::hash<float>{}(k.size));
                mix((std::size_t)k.weight | ((std::size_t)k.fstyle<<16));
                return h;
            }
        };
        std::unordered_map<TFKey, IDWriteTextFormat*, TFHash> fmt_cache_;

        // ── 路径构建状态机 ────────────────────────────────────────
        ID2D1PathGeometry* current_geometry_ = nullptr; // 当前正在构建的路径几何体
        ID2D1GeometrySink* current_sink_     = nullptr; // 几何体的写入句柄
        bool               figure_open_      = false;   // 当前是否有未关闭的 figure
        D2D1_POINT_2F      last_point_       = {};      // 上一次路径点（备用）

        // ── 变换栈 ────────────────────────────────────────────────
        // 每次 push_transform 压入新的累积变换，pop_transform 弹出并恢复
        std::vector<D2D1::Matrix3x2F> transform_stack_;

        // ── 样式栈 ────────────────────────────────────────────────
        struct StyleEntry
        {
            LayerStyle style;
            float      effective_opacity; // 继承父层后的实际不透明度
        };
        std::vector<StyleEntry> style_stack_;

        // ── 待处理阴影（可选，每次 fill/stroke 后重置）─────────────
        std::optional<ShadowStyle> pending_shadow_;

        // ── 圆角裁切栈 ────────────────────────────────────────────
        // 每次 cmd_push_round_clip 压入一个 ClipEntry：
        //   is_layer=false → 用 PushAxisAlignedClip（pop = PopAxisAlignedClip）
        //   is_layer=true  → 用 PushLayer+几何遮罩（pop = PopLayer + Release）
        struct ClipEntry
        {
            ID2D1Layer* layer    = nullptr; // nullptr = 轴对齐裁切
            bool        is_layer = false;
        };
        std::vector<ClipEntry> clip_stack_;

        // ── 图片缓存（按文件路径索引，渲染目标重建时清空）────────
        std::unordered_map<std::wstring, ID2D1Bitmap*> bitmap_cache_;

        // ── 异步 HTTP 图片下载 ────────────────────────────────────
        std::thread                     download_thread_;
        std::atomic<bool>               download_running_{false};
        std::mutex                      download_mutex_;
        std::condition_variable         download_cv_;
        std::queue<std::wstring>        download_queue_;          // 待下载URL队列
        std::set<std::wstring>          download_pending_;        // 正在下载中的URL
        std::unordered_map<std::wstring, std::vector<char>> download_results_; // 下载完成的数据
        std::set<std::wstring>          download_failed_;         // 下载失败的URL
        HWND                            download_hwnd_ = nullptr; // 用于PostMessage通知

        // ── 私有辅助函数 ─────────────────────────────────────────

        // 获取当前有效的累积不透明度
        float effective_opacity() const
        {
            return style_stack_.empty() ? 1.f
                                        : style_stack_.back().effective_opacity;
        }

        // 确保路径几何体和 Sink 已创建并开放
        // 若已存在则直接返回，实现懒创建
        void ensure_geometry()
        {
            if (current_geometry_) return;

            HRESULT hr = factory_->CreatePathGeometry(&current_geometry_);
            if (FAILED(hr)) return;

            hr = current_geometry_->Open(&current_sink_);
            if (FAILED(hr))
            {
                current_geometry_->Release();
                current_geometry_ = nullptr;
                return;
            }

            // 使用 Winding 填充规则（更直观，复杂路径不会有空洞）
            current_sink_->SetFillMode(D2D1_FILL_MODE_WINDING);
        }

        // 关闭当前 Sink，返回可用于绘制的几何体（调用者负责 Release()）
        // 返回 nullptr 表示没有路径可提交
        ID2D1PathGeometry* commit_geometry()
        {
            if (!current_geometry_) return nullptr;

            // 若还有未关闭的 figure，以 open 方式关闭
            if (figure_open_)
            {
                current_sink_->EndFigure(D2D1_FIGURE_END_OPEN);
                figure_open_ = false;
            }

            current_sink_->Close();
            current_sink_->Release();
            current_sink_ = nullptr;

            // 转移所有权给调用者
            ID2D1PathGeometry* result = current_geometry_;
            current_geometry_        = nullptr;
            return result;
        }

        // 创建纯色画笔（调用者负责 Release()）
        ID2D1SolidColorBrush* make_brush(const Color& color, float opacity_mul)
        {
            ID2D1SolidColorBrush* brush = nullptr;
            Color c = convert_color(color);  // 颜色键模式下纯黑转近黑
            render_target_->CreateSolidColorBrush(
                to_d2d_color(c, opacity_mul),
                &brush);
            return brush;
        }

        // ── 渐变画笔创建 ───────────────────────────────────────────

        // 创建渐变停止点集合
        ID2D1GradientStopCollection* make_gradient_stops(
            const std::vector<GradientStop>& stops, float opacity_mul)
        {
            if (stops.empty()) return nullptr;

            // 优化：预分配精确大小，避免动态扩容
            std::vector<D2D1_GRADIENT_STOP> d2d_stops;
            d2d_stops.reserve(stops.size());
            for (const auto& s : stops)
            {
                Color c = convert_color(s.color);
                d2d_stops.push_back({
                    s.position,
                    to_d2d_color(c, opacity_mul)
                });
            }

            ID2D1GradientStopCollection* collection = nullptr;
            render_target_->CreateGradientStopCollection(
                d2d_stops.data(),
                (UINT32)d2d_stops.size(),
                D2D1_GAMMA_2_2,
                D2D1_EXTEND_MODE_CLAMP,
                &collection);

            return collection;
        }

        // 创建线性渐变画笔
        ID2D1LinearGradientBrush* make_linear_gradient_brush(
            const LinearGradient& grad, float opacity_mul)
        {
            ID2D1GradientStopCollection* collection = make_gradient_stops(grad.stops, opacity_mul);
            if (!collection) return nullptr;

            D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES props = {
                D2D1::Point2F(grad.x1, grad.y1),
                D2D1::Point2F(grad.x2, grad.y2)
            };

            ID2D1LinearGradientBrush* brush = nullptr;
            render_target_->CreateLinearGradientBrush(&props, nullptr, collection, &brush);

            collection->Release();
            return brush;
        }

        // 创建径向渐变画笔
        ID2D1RadialGradientBrush* make_radial_gradient_brush(
            const RadialGradient& grad, float opacity_mul)
        {
            ID2D1GradientStopCollection* collection = make_gradient_stops(grad.stops, opacity_mul);
            if (!collection) return nullptr;

            D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES props = {
                D2D1::Point2F(grad.cx, grad.cy),             // Center（椭圆中心）
                D2D1::Point2F(grad.ox, grad.oy),             // GradientOriginOffset（焦点相对中心偏移）
                grad.rx, grad.ry                              // 半径
            };

            ID2D1RadialGradientBrush* brush = nullptr;
            render_target_->CreateRadialGradientBrush(&props, nullptr, collection, &brush);

            collection->Release();
            return brush;
        }

        // ── 锥形渐变（扫描渐变）近似实现 ─────────────────────────
        // D2D 1.0 无原生锥形渐变，通过三角形扇形逼近：
        // 将整圆切分为 N 个细扇形，每个扇形用对应角度处颜色的纯色实心三角形填充。
        // 裁切由调用方 (push_round_clip / PathGeometry) 控制。
        // 参数：grad — 锥形渐变描述；opacity_mul — 透明度乘数；
        //       clip_geom — 若非 null，用此几何体裁切扇形（适用于路径 fill）
        void draw_conic_gradient(const ConicGradient& grad, float opacity_mul,
                                 ID2D1Geometry* clip_geom = nullptr)
        {
            if (!render_target_ || grad.stops.empty()) return;
            if (grad.stops.size() == 1) {
                // 退化为纯色
                Color c = grad.stops[0].color;
                c.a *= opacity_mul;
                if (clip_geom) {
                    ID2D1SolidColorBrush* b = make_brush(c, 1.f);
                    if (b) { render_target_->FillGeometry(clip_geom, b); b->Release(); }
                }
                return;
            }

            // 分扇形数量：越多越细腻，60 段视觉效果已相当平滑
            constexpr int SEGMENTS = 72;
            const float TWO_PI = 6.28318530718f;
            const float seg_angle = TWO_PI / SEGMENTS;

            for (int i = 0; i < SEGMENTS; ++i) {
                float a0 = grad.start_angle + seg_angle * i;
                float a1 = a0 + seg_angle;
                float t0 = (float)i / SEGMENTS;
                float t1 = (float)(i + 1) / SEGMENTS;

                // 在 stops 中插值颜色
                Color c0 = sample_conic_stops(grad.stops, t0);
                Color c1 = sample_conic_stops(grad.stops, t1);
                Color mid = Color::lerp(c0, c1, 0.5f);
                mid.a *= opacity_mul;

                // 三角形顶点（使用足够大的半径覆盖绘制区域）
                // 优化：使用更大的半径确保覆盖大屏幕（4K+ 分辨率）
                constexpr float R = 16000.f;  // 覆盖 8K 及以上分辨率
                float x0 = grad.cx + std::cos(a0) * R;
                float y0 = grad.cy + std::sin(a0) * R;
                float x1 = grad.cx + std::cos(a1) * R;
                float y1 = grad.cy + std::sin(a1) * R;

                // 构建三角形路径
                ID2D1PathGeometry* tri = nullptr;
                if (FAILED(factory_->CreatePathGeometry(&tri)) || !tri) continue;

                ID2D1GeometrySink* sink = nullptr;
                if (FAILED(tri->Open(&sink)) || !sink) { tri->Release(); continue; }

                sink->BeginFigure(D2D1::Point2F(grad.cx, grad.cy), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(x0, y0));
                sink->AddLine(D2D1::Point2F(x1, y1));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                sink->Release();

                // 若有裁切几何体，计算交集再填充
                ID2D1Geometry* fill_geom = tri;
                ID2D1PathGeometry* intersect = nullptr;
                if (clip_geom) {
                    if (SUCCEEDED(factory_->CreatePathGeometry(&intersect)) && intersect) {
                        ID2D1GeometrySink* isink = nullptr;
                        if (SUCCEEDED(intersect->Open(&isink)) && isink) {
                            tri->CombineWithGeometry(clip_geom, D2D1_COMBINE_MODE_INTERSECT,
                                                      nullptr, 0.25f, isink);
                            isink->Close();
                            isink->Release();
                            fill_geom = intersect;
                        }
                    }
                }

                ID2D1SolidColorBrush* b = make_brush(mid, 1.f);
                if (b) {
                    render_target_->FillGeometry(fill_geom, b);
                    b->Release();
                }

                if (intersect) intersect->Release();
                tri->Release();
            }
        }

        // 在锥形渐变停止点数组中，按 t∈[0,1] 采样颜色（线性插值）
        static Color sample_conic_stops(const std::vector<GradientStop>& stops, float t)
        {
            if (stops.empty()) return {};
            if (t <= stops.front().position) return stops.front().color;
            if (t >= stops.back().position)  return stops.back().color;
            for (size_t i = 1; i < stops.size(); ++i) {
                if (t <= stops[i].position) {
                    float range = stops[i].position - stops[i-1].position;
                    float u = (range > 0.f) ? (t - stops[i-1].position) / range : 0.f;
                    return Color::lerp(stops[i-1].color, stops[i].color, u);
                }
            }
            return stops.back().color;
        }

        // 近似阴影（填充版）：在 shadow 偏移位置绘制同形状的阴影色
        // 注：D2D 1.0 不支持原生高斯模糊，此处用多层半透明叠加模拟扩散感
        void apply_shadow_fill(ID2D1PathGeometry* geom,
                               const ShadowStyle& sh,
                               float              opacity_mul)
        {
            // 保存当前变换
            D2D1::Matrix3x2F saved = transform_stack_.back();

            // 用 blur 步数模拟扩散（步数越多越平滑，性能消耗越大）
            constexpr int SHADOW_STEPS = 3;
            for (int i = 1; i <= SHADOW_STEPS; ++i)
            {
                float t       = (float)i / SHADOW_STEPS;
                float alpha   = sh.color.a * (1.f - t * 0.6f) * opacity_mul;
                float spread  = sh.blur * t * 0.4f;

                // 稍微放大形状以模拟扩散（通过平移近似）
                D2D1::Matrix3x2F offset_mat =
                    D2D1::Matrix3x2F::Translation(
                        sh.offset_x + spread,
                        sh.offset_y + spread) * saved;

                render_target_->SetTransform(offset_mat);

                Color shadow_color = { sh.color.r, sh.color.g, sh.color.b, alpha };
                ID2D1SolidColorBrush* brush = make_brush(shadow_color, 1.f);
                if (brush)
                {
                    render_target_->FillGeometry(geom, brush);
                    brush->Release();
                }
            }

            // 恢复原变换
            render_target_->SetTransform(saved);
        }

        // 近似阴影（描边版）
        void apply_shadow_stroke(ID2D1PathGeometry* geom,
                                 const StrokeStyle& stroke_style,
                                 const ShadowStyle& sh,
                                 float              opacity_mul)
        {
            D2D1::Matrix3x2F saved = transform_stack_.back();

            constexpr int SHADOW_STEPS = 3;
            for (int i = 1; i <= SHADOW_STEPS; ++i)
            {
                float t     = (float)i / SHADOW_STEPS;
                float alpha = sh.color.a * (1.f - t * 0.6f) * opacity_mul;

                D2D1::Matrix3x2F offset_mat =
                    D2D1::Matrix3x2F::Translation(
                        sh.offset_x,
                        sh.offset_y) * saved;

                render_target_->SetTransform(offset_mat);

                Color shadow_color = { sh.color.r, sh.color.g, sh.color.b, alpha };
                ID2D1SolidColorBrush* brush = make_brush(shadow_color, 1.f);
                if (brush)
                {
                    render_target_->DrawGeometry(geom, brush, stroke_style.width + sh.blur * 0.3f * t);
                    brush->Release();
                }
            }

            render_target_->SetTransform(saved);
        }

        // 释放渲染目标（设备丢失时重建）
        void release_render_resources()
        {
            // 先释放路径相关资源
            if (current_sink_)     { current_sink_->Release();     current_sink_     = nullptr; }
            if (current_geometry_) { current_geometry_->Release(); current_geometry_ = nullptr; }
            figure_open_ = false;

            // 释放未弹出的裁切层（异常路径保护）
            // 先正确 Pop 所有裁切，再 Release Layer 对象
            if (render_target_)
            {
                for (auto it = clip_stack_.rbegin(); it != clip_stack_.rend(); ++it)
                {
                    if (it->is_layer)
                        render_target_->PopLayer();
                    else
                        render_target_->PopAxisAlignedClip();
                }
            }
            for (auto& entry : clip_stack_)
                if (entry.is_layer && entry.layer) entry.layer->Release();
            clip_stack_.clear();

            // 释放图片缓存（渲染目标失效后 Bitmap 也需重建）
            for (auto& [k, bmp] : bitmap_cache_)
                if (bmp) bmp->Release();
            bitmap_cache_.clear();

            // 清理下载相关数据
            {
                std::unique_lock<std::mutex> lock(download_mutex_);
                download_results_.clear();
                download_failed_.clear();
                download_pending_.clear();
                while (!download_queue_.empty()) download_queue_.pop();
            }

            if (render_target_)    { render_target_->Release();    render_target_    = nullptr; }
        }

        // 释放所有 D2D / DWrite 资源（析构时调用）
        void release_all()
        {
            // 停止异步下载线程
            download_running_ = false;
            download_cv_.notify_all();
            if (download_thread_.joinable())
                download_thread_.join();

            release_render_resources();

            // 释放 TextFormat 缓存
            for (auto& [k, fmt] : fmt_cache_)
                if (fmt) fmt->Release();
            fmt_cache_.clear();

            if (dwrite_factory1_) { dwrite_factory1_->Release(); dwrite_factory1_ = nullptr; }
            if (dwrite_factory_)  { dwrite_factory_->Release();  dwrite_factory_  = nullptr; }
            if (factory_)         { factory_->Release();          factory_          = nullptr; }
        }

        // ── DWrite 辅助函数 ───────────────────────────────────────

        // 获取或创建 IDWriteTextFormat（带缓存）
        IDWriteTextFormat* get_text_format(const TextStyle& style)
        {
            if (!dwrite_factory_) return nullptr;

            TFKey key{
                style.font_family,
                style.font_size,
                (uint16_t)style.weight,
                (uint8_t)style.font_style
            };

            auto it = fmt_cache_.find(key);
            if (it != fmt_cache_.end()) return it->second;

            // 将枚举转换为 DWRITE_ 常量
            DWRITE_FONT_WEIGHT dw_weight = (DWRITE_FONT_WEIGHT)(uint16_t)style.weight;
            DWRITE_FONT_STYLE  dw_style  =
                style.font_style == TextStyle::FontStyle::Italic  ? DWRITE_FONT_STYLE_ITALIC  :
                style.font_style == TextStyle::FontStyle::Oblique ? DWRITE_FONT_STYLE_OBLIQUE :
                                                                     DWRITE_FONT_STYLE_NORMAL;

            IDWriteTextFormat* fmt = nullptr;
            HRESULT hr = dwrite_factory_->CreateTextFormat(
                style.font_family.c_str(),   // 字体族
                nullptr,                      // 字体集合（nullptr = 系统字体）
                dw_weight,
                dw_style,
                DWRITE_FONT_STRETCH_NORMAL,
                style.font_size,
                L"",                          // 语言区域（空串 = 系统区域）
                &fmt);

            if (FAILED(hr) || !fmt)
            {
                // 字体不存在时降级为系统默认字体
                dwrite_factory_->CreateTextFormat(
                    L"Segoe UI", nullptr,
                    DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, style.font_size, L"", &fmt);
            }

            if (fmt) fmt_cache_[key] = fmt;
            return fmt;
        }

        // 将 TextStyle::HAlign 转换为 DWRITE_TEXT_ALIGNMENT
        static DWRITE_TEXT_ALIGNMENT to_dwrite_h_align(TextStyle::HAlign h)
        {
            switch (h)
            {
            case TextStyle::HAlign::Center:    return DWRITE_TEXT_ALIGNMENT_CENTER;
            case TextStyle::HAlign::Right:     return DWRITE_TEXT_ALIGNMENT_TRAILING;
            case TextStyle::HAlign::Justified: return DWRITE_TEXT_ALIGNMENT_JUSTIFIED;
            default:                           return DWRITE_TEXT_ALIGNMENT_LEADING;
            }
        }

        // 将 TextStyle::VAlign 转换为 DWRITE_PARAGRAPH_ALIGNMENT
        static DWRITE_PARAGRAPH_ALIGNMENT to_dwrite_v_align(TextStyle::VAlign v)
        {
            switch (v)
            {
            case TextStyle::VAlign::Middle: return DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
            case TextStyle::VAlign::Bottom: return DWRITE_PARAGRAPH_ALIGNMENT_FAR;
            default:                        return DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
            }
        }

        // 根据 TextStyle::RenderMode 设置文字抗锯齿模式
        // 每次绘制文本前调用，绘制后重置为 ClearType（默认最高质量）
        void apply_render_mode(TextStyle::RenderMode mode)
        {
            if (!render_target_) return;
            switch (mode)
            {
            case TextStyle::RenderMode::Aliased:
                render_target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_ALIASED);
                break;
            case TextStyle::RenderMode::GrayScale:
                render_target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
                break;
            case TextStyle::RenderMode::ClearType:
            case TextStyle::RenderMode::Default:
            default:
                render_target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
                break;
            }
        }

        // 配置高质量文字渲染参数（初始化时调用一次）
        // 使用 NATURAL_SYMMETRIC 渲染模式 + 正确的 ClearType 参数，
        // 与 Chrome、WPF、Visual Studio 等主流应用的文字渲染质量一致。
        void setup_rendering_params()
        {
            if (!dwrite_factory_ || !render_target_) return;

            // 1. 获取系统默认渲染参数（包含 Gamma、ClearType 级别等系统设置）
            IDWriteRenderingParams* default_params = nullptr;
            HRESULT hr = dwrite_factory_->CreateRenderingParams(&default_params);
            if (FAILED(hr) || !default_params) return;

            // 2. 用系统参数作基础，覆盖渲染模式为 NATURAL_SYMMETRIC
            //    NATURAL_SYMMETRIC: 字形轮廓自然平滑 + 左右对称，
            //    是现代高质量 UI 文字的推荐模式（比 ALIASED 清晰，比 OUTLINE 快）
            IDWriteRenderingParams* quality_params = nullptr;
            hr = dwrite_factory_->CreateCustomRenderingParams(
                default_params->GetGamma(),              // 保留系统 Gamma 值
                default_params->GetEnhancedContrast(),   // 保留系统增强对比度
                default_params->GetClearTypeLevel(),     // 保留系统 ClearType 级别
                default_params->GetPixelGeometry(),      // 保留像素几何（RGB/BGR）
                DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, // ← 关键：高质量对称渲染
                &quality_params);

            default_params->Release();

            if (SUCCEEDED(hr) && quality_params)
            {
                render_target_->SetTextRenderingParams(quality_params);
                quality_params->Release();
            }
        }

        // ── WIC 图片加载（按路径缓存 D2D Bitmap）────────────────────

        // 判断是否为 HTTP/HTTPS URL
        static bool is_http_url(const std::wstring& path)
        {
            return path.find(L"http://") == 0 || path.find(L"https://") == 0;
        }

        // ── 后台下载线程工作函数 ────────────────────────────────
        void download_worker()
        {
            CoInitialize(nullptr);  // COM初始化（WIC需要）

            while (download_running_)
            {
                std::wstring url;
                {
                    std::unique_lock<std::mutex> lock(download_mutex_);
                    download_cv_.wait(lock, [this]() {
                        return !download_queue_.empty() || !download_running_;
                    });

                    if (!download_running_) break;

                    if (download_queue_.empty()) continue;

                    url = download_queue_.front();
                    download_queue_.pop();
                }

                // 执行下载
                std::vector<char> data;
                bool success = download_http_image_sync(url, data);

                {
                    std::unique_lock<std::mutex> lock(download_mutex_);
                    download_pending_.erase(url);

                    if (success)
                        download_results_[url] = std::move(data);
                    else
                        download_failed_.insert(url);
                }

                // 通知主线程刷新（触发重绘）
                if (download_hwnd_)
                    PostMessage(download_hwnd_, WM_PAINT, 0, 0);
            }

            CoUninitialize();
        }

        // 同步下载（在后台线程中调用）
        static bool download_http_image_sync(const std::wstring& url, std::vector<char>& out_data)
        {
            // 解析 URL
            std::wstring host, path_part;
            bool secure = false;

            size_t pos = url.find(L"://");
            if (pos == std::wstring::npos) return false;

            std::wstring rest = url.substr(pos + 3);
            secure = (url.substr(0, pos) == L"https");

            size_t slash_pos = rest.find(L'/');
            if (slash_pos == std::wstring::npos)
            {
                host = rest;
                path_part = L"/";
            }
            else
            {
                host = rest.substr(0, slash_pos);
                path_part = rest.substr(slash_pos);
            }

            HINTERNET session = WinHttpOpen(L"OtterCreat/1.0",
                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (!session) return false;

            INTERNET_PORT port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
            HINTERNET connect = WinHttpConnect(session, host.c_str(), port, 0);
            if (!connect) { WinHttpCloseHandle(session); return false; }

            DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
            HINTERNET request = WinHttpOpenRequest(connect, L"GET", path_part.c_str(),
                nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
            if (!request) { WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return false; }

            BOOL sent = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
            if (!sent) { WinHttpCloseHandle(request); WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return false; }

            BOOL received = WinHttpReceiveResponse(request, nullptr);
            if (!received) { WinHttpCloseHandle(request); WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return false; }

            DWORD status = 0;
            DWORD status_size = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);

            if (status != 200) { WinHttpCloseHandle(request); WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return false; }

            out_data.clear();
            DWORD bytes_avail = 0;
            DWORD bytes_read = 0;

            do
            {
                bytes_avail = 0;
                if (!WinHttpQueryDataAvailable(request, &bytes_avail) || bytes_avail == 0)
                    break;

                std::vector<char> buffer(bytes_avail);
                if (!WinHttpReadData(request, buffer.data(), bytes_avail, &bytes_read))
                    break;

                if (bytes_read > 0)
                    out_data.insert(out_data.end(), buffer.begin(), buffer.begin() + bytes_read);
            } while (bytes_avail > 0);

            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);

            return !out_data.empty();
        }

        // 从内存数据创建 WIC Bitmap（主线程调用）
        ID2D1Bitmap* load_bitmap_from_memory(const std::vector<char>& data)
        {
            IWICImagingFactory* wic = get_wic_factory();
            if (!wic || !render_target_ || data.empty()) return nullptr;

            IWICStream* stream = nullptr;
            if (FAILED(wic->CreateStream(&stream)) || !stream) return nullptr;

            HRESULT hr = stream->InitializeFromMemory(
                reinterpret_cast<BYTE*>(const_cast<char*>(data.data())),
                static_cast<DWORD>(data.size()));
            if (FAILED(hr)) { stream->Release(); return nullptr; }

            IWICBitmapDecoder* decoder = nullptr;
            hr = wic->CreateDecoderFromStream(stream, nullptr,
                WICDecodeMetadataCacheOnLoad, &decoder);
            stream->Release();
            if (FAILED(hr) || !decoder) return nullptr;

            IWICBitmapFrameDecode* frame = nullptr;
            decoder->GetFrame(0, &frame);
            decoder->Release();
            if (!frame) return nullptr;

            IWICFormatConverter* converter = nullptr;
            wic->CreateFormatConverter(&converter);
            if (!converter) { frame->Release(); return nullptr; }

            converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom);
            frame->Release();

            ID2D1Bitmap* bmp = nullptr;
            render_target_->CreateBitmapFromWicBitmap(converter, nullptr, &bmp);
            converter->Release();

            return bmp;
        }

        // 加载图片（本地文件同步，HTTP URL异步）
        ID2D1Bitmap* load_bitmap(const std::wstring& path)
        {
            // 1. 检查已完成的缓存
            auto it = bitmap_cache_.find(path);
            if (it != bitmap_cache_.end()) return it->second;

            IWICImagingFactory* wic = get_wic_factory();
            if (!wic || !render_target_) return nullptr;

            // 2. 本地文件：同步加载
            if (!is_http_url(path))
            {
                IWICBitmapDecoder* decoder = nullptr;
                if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr,
                    GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) || !decoder)
                    return nullptr;

                IWICBitmapFrameDecode* frame = nullptr;
                decoder->GetFrame(0, &frame);
                decoder->Release();
                if (!frame) return nullptr;

                IWICFormatConverter* converter = nullptr;
                wic->CreateFormatConverter(&converter);
                if (!converter) { frame->Release(); return nullptr; }

                converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom);
                frame->Release();

                ID2D1Bitmap* bmp = nullptr;
                render_target_->CreateBitmapFromWicBitmap(converter, nullptr, &bmp);
                converter->Release();

                if (bmp) bitmap_cache_[path] = bmp;
                return bmp;
            }

            // 3. HTTP URL：异步加载
            {
                std::unique_lock<std::mutex> lock(download_mutex_);

                // 检查是否下载完成
                auto rit = download_results_.find(path);
                if (rit != download_results_.end())
                {
                    // 下载完成，创建bitmap并缓存
                    ID2D1Bitmap* bmp = load_bitmap_from_memory(rit->second);
                    if (bmp)
                    {
                        bitmap_cache_[path] = bmp;
                        download_results_.erase(rit);
                        return bmp;
                    }
                    // 创建失败，标记为失败
                    download_failed_.insert(path);
                    download_results_.erase(rit);
                    return nullptr;
                }

                // 检查是否正在下载
                if (download_pending_.count(path) > 0)
                    return nullptr;  // 还在下载中，下一帧再检查

                // 检查是否已失败
                if (download_failed_.count(path) > 0)
                    return nullptr;  // 下载失败，不重试

                // 加入下载队列
                download_queue_.push(path);
                download_pending_.insert(path);
                download_cv_.notify_one();
            }

            return nullptr;  // 首次请求，返回空，等下载完成
        }

        // 获取（懒创建）进程级 WIC 工厂
        static IWICImagingFactory* get_wic_factory()
        {
            static IWICImagingFactory* factory = nullptr;
            if (!factory)
                CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
            return factory;
        }

        // 检查 HRESULT，失败时抛出带描述的异常
        static void check_hr(HRESULT hr, const char* message)
        {
            if (FAILED(hr))
                throw std::runtime_error(std::string(message) +
                    " (HRESULT=0x" + std::to_string(hr) + ")");
        }
    };

} // namespace Otter
