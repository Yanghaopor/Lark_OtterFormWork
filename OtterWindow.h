#pragma once
#ifndef _WIN32
#error "OtterWindow.h currently exposes the Win32 window backend. Add a platform window backend (SDL/GLFW/Cocoa/X11/Wayland) and include it through a platform-neutral window facade on non-Windows platforms."
#endif
// ============================================================
//  OtterWindow.h
//  水獭图形框架 —— Win32 窗口 + 主循环 + 鼠标事件分发
//
//  坐标系策略：纯物理像素（Physical Pixels）
//    · 窗口 width/height       = 物理像素
//    · 鼠标坐标                = 物理像素，不做任何换算
//    · 图层/按钮/滚动条坐标    = 物理像素
//    · D2D 渲染目标固定 96 DPI（1 DIP = 1 物理像素），无 DPI 缩放
//    · 字体大小单位             = 物理像素（px），传多少渲染多少
//
//  命名空间：Otter
//  C++ 标准：C++20
//  依赖：OtterRenderer.h
// ============================================================

#ifndef OTTER_DISABLE_D2D
#include "OtterRenderer.h"
#endif
#include "OtterOpenGLRenderer.h"
#include "OtterWindowBackend.h"
#include "OtterDebug.h"

#include <string>
#include <string_view>
#include <stdexcept>
#include <memory>
#include <profileapi.h>   // QueryPerformanceCounter
#include <windowsx.h>     // GET_X_LPARAM / GET_Y_LPARAM / GET_WHEEL_DELTA_WPARAM
#include <commctrl.h>     // EM_SETCUEBANNER
#include <imm.h>          // IME 输入法
#include <shellapi.h>     // DragAcceptFiles, DragQueryFile
#ifndef OTTER_DISABLE_D2D
#include <dwmapi.h>       // DWM 合成透明
#pragma comment(lib, "dwmapi")
#endif

// Windows 8+ 标志，旧版 SDK 可能未定义
#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00000020
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_COLOR_NONE
#define DWMWA_COLOR_NONE 0xFFFFFFFE
#endif

#pragma comment(lib, "imm32")
namespace Otter
{
    class otterwindow : public IWindowBackend
    {
    public:
        // ── 构造 / 析构 ──────────────────────────────────────────

        // width/height：物理像素（客户区尺寸）
        otterwindow(
            int width,
            int height,
            std::wstring_view title,
            RenderBackend backend = default_render_backend())
            : canvas_("__otter_canvas__", true)
            , creat{ &canvas_ }
            , get  { &canvas_ }
            , width_(width)
            , height_(height)
            , backend_(backend)
        {
            register_window_class();

            RECT rc = { 0, 0, width, height };
            AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);

            int sw = GetSystemMetrics(SM_CXSCREEN);
            int sh = GetSystemMetrics(SM_CYSCREEN);
            int wx = (sw - (rc.right  - rc.left)) / 2;
            int wy = (sh - (rc.bottom - rc.top )) / 2;
            if (wx < 0) wx = 0;
            if (wy < 0) wy = 0;

            hwnd_ = CreateWindowExW(
                0, OTTER_WND_CLASS, title.data(),
                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                wx, wy,
                rc.right - rc.left,
                rc.bottom - rc.top,
                nullptr, nullptr,
                GetModuleHandleW(nullptr),
                this);

            if (!hwnd_)
            {
                OTTER_LOG_ERROR("window", "CreateWindowExW failed");
                throw std::runtime_error("CreateWindowExW 失败");
            }

            default_ime_context_ = ImmAssociateContext(hwnd_, nullptr);
            initialize_renderer();
            OTTER_LOG_INFO("window", "window created");

            // 同步清除颜色，确保 resize 时使用正确的颜色
            renderer().set_resize_clear_color(clear_color_);

            // 设置动画管理器（让图层可以访问）
            canvas_.set_anim_manager(&anim_manager_);

            ShowWindow(hwnd_, SW_SHOWDEFAULT);
            UpdateWindow(hwnd_);
        }

        otterwindow(const otterwindow&)            = delete;
        otterwindow& operator=(const otterwindow&) = delete;

        // ── 图层树操作代理 ───────────────────────────────────────

        struct CreatProxy
        {
            Layer* parent;
            LayerRef operator[](std::string_view name) const
            {
                assert(parent);
                return LayerRef{ parent->creat(name) };
            }
        };

        struct GetProxy
        {
            Layer* parent;
            Layer* operator[](std::string_view name) const
            {
                assert(parent);
                return parent->find(name);
            }
        };

        CreatProxy creat;
        GetProxy   get;

        // 获取顶层覆盖图层（专用于弹出框、tooltip 等需要遮挡一切的内容）
        // 该层总是挂在 canvas 的最顶部，每次调用会自动 bring_to_front。
        // 用法：Dropdown(&win, parent, win.overlay(), "dd", x, y)
        Layer* overlay()
        {
            Layer* ov = canvas_.get_child("__otter_overlay__");
            if (!ov) ov = canvas_.creat("__otter_overlay__");
            ov->bring_to_front();
            return ov;
        }

        // ── 窗口属性配置 ────────────────────────────────────────

        otterwindow& borderless(bool enable = true)
        {
            borderless_ = enable;
            if (!hwnd_) return *this;

            if (enable)
            {
                // 无边框模式：窗口尺寸 = 客户区尺寸
                // 需要调整窗口大小以匹配原来的客户区尺寸
                RECT rc = { 0, 0, width_, height_ };
                LONG_PTR style = WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
                SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
#ifndef OTTER_DISABLE_D2D
                COLORREF noBorder = DWMWA_COLOR_NONE;
                DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &noBorder, sizeof(noBorder));
#endif
                SetWindowPos(hwnd_, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                    SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
            }
            else
            {
                LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
                style = (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
                SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
                SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
            }
            return *this;
        }

        otterwindow& draggable(bool enable = true)
        {
            draggable_ = enable;
            return *this;
        }

        otterwindow& layered(float alpha = 1.0f)
        {
            window_alpha_ = std::clamp(alpha, 0.f, 1.f);
            if (!hwnd_) return *this;
            LONG_PTR ex_style = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
            SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex_style | WS_EX_LAYERED);
            SetLayeredWindowAttributes(hwnd_, 0,
                static_cast<BYTE>(window_alpha_ * 255.f), LWA_ALPHA);
            return *this;
        }

        otterwindow& topmost(bool enable = true)
        {
            topmost_ = enable;
            if (!hwnd_) return *this;
            SetWindowPos(hwnd_,
                enable ? HWND_TOPMOST : HWND_NOTOPMOST,
                0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            return *this;
        }

        // ── 透明区域鼠标穿透（区域级）──────────────────────────────────
        // 设置一个矩形区域作为窗口的有效区域，其他区域完全穿透
        // 使用 SetWindowRgn 实现真正的形状窗口
        otterwindow& set_hit_rect(float x, float y, float w, float h)
        {
            if (!hwnd_) return *this;
            HRGN rgn = CreateRectRgn((int)x, (int)y, (int)(x + w), (int)(y + h));
            SetWindowRgn(hwnd_, rgn, TRUE);
            return *this;
        }

        // 设置自定义形状区域
        otterwindow& set_hit_callback(std::function<bool(float x, float y)> cb, int sample_step = 4)
        {
            if (!hwnd_ || !cb) return *this;

            HRGN total_rgn = CreateRectRgn(0, 0, 0, 0);
            for (int y = 0; y < height_; y += sample_step)
            {
                for (int x = 0; x < width_; x += sample_step)
                {
                    if (cb((float)x, (float)y))
                    {
                        HRGN cell = CreateRectRgn(x, y, x + sample_step, y + sample_step);
                        CombineRgn(total_rgn, total_rgn, cell, RGN_OR);
                        DeleteObject(cell);
                    }
                }
            }
            SetWindowRgn(hwnd_, total_rgn, TRUE);
            return *this;
        }

        // ── 颜色键透明模式 ─────────────────────────────────────────
        // 纯黑色 RGB(0,0,0) 作为透明色，透明区域自动穿透鼠标
        // 注意：启用后，用户绘制的纯黑色会自动转为近黑色 (1,1,1)
        otterwindow& color_key_transparent()
        {
            color_key_mode_ = true;
            if (!hwnd_) return *this;

            LONG_PTR ex_style = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
            ex_style |= WS_EX_LAYERED;
            SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex_style);

            SetLayeredWindowAttributes(hwnd_, RGB(0, 0, 0), 255, LWA_COLORKEY);
            clear_color_ = Color::transparent_key();
            renderer().set_color_key_mode(true);

            return *this;
        }

        // ── DWM 透明模式 ───────────────────────────────────────────
        // 使用 DWM 合成透明，窗口整体透明但透明区域不穿透鼠标
        // 颜色正常处理，纯黑色就是纯黑色
        otterwindow& dwm_transparent(bool enable = true)
        {
#ifdef OTTER_DISABLE_D2D
            (void)enable;
            return *this;
#else
            color_key_mode_ = false;
            dwm_transparent_ = enable;
            if (!hwnd_) return *this;

            if (enable)
            {
                MARGINS margins = { -1, -1, -1, -1 };
                DwmExtendFrameIntoClientArea(hwnd_, &margins);

                LONG_PTR ex_style = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
                ex_style |= WS_EX_LAYERED;
                SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex_style);

                clear_color_ = Color::transparent();
                renderer().set_color_key_mode(false);
            }
            else
            {
                MARGINS margins = { 0, 0, 0, 0 };
                DwmExtendFrameIntoClientArea(hwnd_, &margins);
                clear_color_ = Color{ 0.08f, 0.08f, 0.10f, 1.f };
            }
            return *this;
#endif
        }

        // 检查是否启用颜色键模式
        bool is_color_key_mode() const noexcept { return color_key_mode_; }

        // ── 文件拖放支持 ───────────────────────────────────────────
        otterwindow& enable_drop_files(bool enable = true)
        {
            if (!hwnd_) return *this;
            DragAcceptFiles(hwnd_, enable);
            drop_files_enabled_ = enable;
            return *this;
        }

        bool is_drop_files_enabled() const noexcept { return drop_files_enabled_; }

        // ── 窗口尺寸策略 ────────────────────────────────────────

        // 是否允许拉伸调整大小（仅控制 WS_SIZEBOX）
        otterwindow& resizable(bool enable = true)
        {
            resizable_ = enable;
            if (!hwnd_) return *this;
            LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
            if (enable)
                style |=  WS_SIZEBOX;
            else
                style &= ~WS_SIZEBOX;
            SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
            SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
            return *this;
        }

        // 限制窗口最小尺寸（客户区像素）
        otterwindow& min_size(int w, int h)
        {
            min_width_  = w;
            min_height_ = h;
            return *this;
        }

        // 限制窗口最大尺寸（客户区像素，0 = 不限制）
        otterwindow& max_size(int w, int h)
        {
            max_width_  = w;
            max_height_ = h;
            return *this;
        }

        // 是否允许最大化（仅控制 WS_MAXIMIZEBOX）
        otterwindow& maximizable(bool enable = true)
        {
            maximizable_ = enable;
            if (!hwnd_) return *this;
            LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
            if (enable)
                style |=  WS_MAXIMIZEBOX;
            else
                style &= ~WS_MAXIMIZEBOX;
            SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
            SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
            return *this;
        }

        // ── 自动滚动条 ───────────────────────────────────────────
        // 启用后，每帧自动检测画布子图层是否溢出视口：
        //   · 溢出 → 启用滚动（滚动条出现）
        //   · 未溢出 → 禁用滚动（滚动条消失）

        otterwindow& auto_scroll(bool enable = true)
        {
            auto_scroll_ = enable;
            if (!enable)
                canvas_.disable_scroll();
            return *this;
        }

        otterwindow& auto_scroll(bool enable, ScrollConfig cfg)
        {
            auto_scroll_ = enable;
            auto_scroll_cfg_ = enable ? std::make_optional(std::move(cfg)) : std::nullopt;
            if (!enable)
                canvas_.disable_scroll();
            return *this;
        }

        otterwindow& Layout(int cols, int rows,
                            float pad_x = 0.f, float pad_y = 0.f,
                            float gap_x = 0.f, float gap_y = 0.f,
                            float origin_x = 0.f, float origin_y = 0.f)
        {
            Otter::LayoutConfig cfg;
            cfg.cols = cols;    cfg.rows = rows;
            cfg.ref_w = (float)width_;
            cfg.ref_h = (float)height_;
            cfg.origin_x = origin_x;  cfg.origin_y = origin_y;
            cfg.pad_x = pad_x;  cfg.pad_y = pad_y;
            cfg.gap_x = gap_x;  cfg.gap_y = gap_y;
            canvas_.set_layout_config(cfg);
            return *this;
        }

        Otter::Rect layout_rect(int col, int row,
                                int col_span = 1, int row_span = 1) const
        {
            const Otter::LayoutConfig* cfg = canvas_.get_layout_config();
            if (!cfg || !cfg->is_valid()) return Otter::Rect{};
            return Otter::Rect{
                cfg->pos_x(col), cfg->pos_y(row),
                cfg->span_w(col_span), cfg->span_h(row_span)
            };
        }

        otterwindow& continue_on_blur(bool enable = true)
        { continue_on_blur_ = enable; return *this; }

        otterwindow& run_during_drag(bool enable = true)
        { run_during_drag_ = enable; return *this; }

        otterwindow& set_clear_color(Color color)
        { 
            clear_color_ = color; 
            // 同步更新 renderer 的 resize 清除颜色，避免闪烁
            renderer().set_resize_clear_color(color);
            return *this; 
        }

        otterwindow& render_backend(RenderBackend backend)
        {
            if (backend_ == backend)
                return *this;
            backend_ = backend;
            if (hwnd_)
                initialize_renderer();
            return *this;
        }

        // ── 窗口生命周期回调 ───────────────────────────────────────
        // ready: 窗口准备就绪时调用（类似 Unity Start）
        //        在 run() 首次进入主循环前触发
        otterwindow& on_ready(std::function<void()> cb)
        { ready_cb_ = std::move(cb); return *this; }

        // close: 窗口关闭前调用（可做清理工作）
        otterwindow& on_close(std::function<void()> cb)
        { close_cb_ = std::move(cb); return *this; }

        // 手动触发 ready 回调（通常由系统自动调用）
        void trigger_ready()
        {
            if (!ready_called_ && ready_cb_)
            {
                ready_called_ = true;
                ready_cb_();
            }
        }

        // 手动触发 close 回调
        void trigger_close()
        {
            if (close_cb_) close_cb_();
        }

        // ── 窗口级画布滚动 ───────────────────────────────────────
        otterwindow& enable_scroll(float content_height,
                                    float content_width = 0.f)
        {
            canvas_.layer_bounds(0.f, 0.f, (float)width_, (float)height_);
            Otter::ScrollConfig sc;
            sc.content_height = content_height;
            sc.content_width  = content_width;
            canvas_.enable_scroll(sc);
            return *this;
        }

        otterwindow& enable_scroll(Otter::ScrollConfig cfg)
        {
            canvas_.layer_bounds(0.f, 0.f, (float)width_, (float)height_);
            canvas_.enable_scroll(std::move(cfg));
            return *this;
        }

        otterwindow& disable_scroll()
        {
            canvas_.disable_scroll();
            canvas_.clear_bounds();
            return *this;
        }

        // ── 帧率控制 ────────────────────────────────────────────────
        // 设置目标帧率（0 = 不限制，默认 0）
        // 适用于高刷新率显示器，避免过度消耗 CPU/GPU
        otterwindow& target_fps(int fps)
        {
            target_fps_ = fps > 0 ? fps : 0;
            if (target_fps_ > 0)
                frame_time_ns_ = 1000000000LL / target_fps_;
            return *this;
        }

        // 获取当前帧率
        int current_fps() const { return current_fps_; }

        // 获取当前帧时间（毫秒）
        float frame_time_ms() const { return frame_time_ms_; }

        // 启用/禁用 VSync 模式（等待垂直同步）
        otterwindow& vsync(bool enable)
        {
            vsync_ = enable;
            return *this;
        }

        // ── 主循环 ────────────────────────────────────────────────
        void run() override
        {
            OTTER_LOG_INFO("window", "main loop started");
            LARGE_INTEGER freq, t0, t1, frame_start;
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&t0);

            running_ = true;

            // 触发 ready 回调（窗口准备就绪）
            trigger_ready();

            MSG msg  = {};

            // 帧率统计
            int frame_count = 0;
            LARGE_INTEGER fps_timer;
            QueryPerformanceCounter(&fps_timer);

            while (running_)
            {
                // 帧开始时间
                QueryPerformanceCounter(&frame_start);

                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                {
                    if (msg.message == WM_QUIT) { running_ = false; break; }
                    TranslateMessage(&msg);
                    dispatch_message_safely(msg);
                }
                if (!running_) break;

                if (!is_focused_ && !continue_on_blur_)
                { WaitMessage(); continue; }

                QueryPerformanceCounter(&t1);
                float dt = static_cast<float>(t1.QuadPart - t0.QuadPart)
                         / static_cast<float>(freq.QuadPart);
                t0 = t1;
                dt = std::fmin(dt, 0.1f);

                // ── 自动滚动：检测子图层溢出视口 ──────────────
                if (auto_scroll_)
                    update_auto_scroll();

                anim_manager_.tick(dt);  // 更新动画
                canvas_.tick(dt);
                renderer().begin_frame(clear_color_);
                canvas_.flush(renderer());
                renderer().end_frame();

                // ── 帧率控制 ──────────────────────────────────────
                if (target_fps_ > 0)
                {
                    QueryPerformanceCounter(&t1);
                    long long elapsed_ns = (t1.QuadPart - frame_start.QuadPart)
                                         * 1000000000LL / freq.QuadPart;

                    if (elapsed_ns < frame_time_ns_)
                    {
                        // 精确睡眠：先 Sleep 大部分时间，再忙等待剩余时间
                        long long sleep_ns = frame_time_ns_ - elapsed_ns;
                        if (sleep_ns > 2000000LL)  // > 2ms，使用 Sleep
                        {
                            DWORD sleep_ms = static_cast<DWORD>((sleep_ns - 1000000LL) / 1000000LL);
                            Sleep(sleep_ms);
                        }
                        // 忙等待剩余时间（精确控制）
                        while (true)
                        {
                            QueryPerformanceCounter(&t1);
                            long long now_ns = (t1.QuadPart - frame_start.QuadPart)
                                             * 1000000000LL / freq.QuadPart;
                            if (now_ns >= frame_time_ns_) break;
                        }
                    }
                }

                // ── FPS 统计 ───────────────────────────────────────
                frame_count++;
                QueryPerformanceCounter(&t1);
                long long fps_elapsed = t1.QuadPart - fps_timer.QuadPart;
                if (fps_elapsed >= freq.QuadPart)  // 每秒更新一次
                {
                    current_fps_ = static_cast<int>(frame_count * freq.QuadPart / fps_elapsed);
                    frame_time_ms_ = 1000.f / (current_fps_ > 0 ? current_fps_ : 1);
                    frame_count = 0;
                    fps_timer = t1;
                }
            }
            OTTER_LOG_INFO("window", "main loop stopped");
        }

        HWND              hwnd()     const { return hwnd_; }
        void* native_handle() const override { return hwnd_; }
        RenderContext& renderer() override  { return *renderer_; }
        int               width()    const override { return width_; }
        int               height()   const override { return height_; }

        void close() override
        {
            running_ = false;
            if (hwnd_)
                DestroyWindow(hwnd_);
        }

        // ── 键盘焦点（供 Widget 使用）─────────────────────────
        void set_keyboard_target(std::function<bool(WCHAR)> char_cb,
                                 std::function<bool(WPARAM,LPARAM)> key_cb = {},
                                 std::function<bool(WPARAM,LPARAM)> key_up_cb = {},
                                 std::function<void()> defocus_cb = {})
        {
            if (keyboard_defocus_cb_) keyboard_defocus_cb_();
            keyboard_defocus_cb_ = std::move(defocus_cb);
            keyboard_char_cb_ = std::move(char_cb);
            keyboard_key_cb_  = std::move(key_cb);
            keyboard_key_up_cb_ = std::move(key_up_cb);
            ImmAssociateContext(hwnd_, default_ime_context_);
        }

        void set_keyboard_target(std::function<bool(wchar_t)> char_cb,
                                 std::function<bool(Key)> key_cb,
                                 std::function<void()> defocus_cb = {}) override
        {
            set_keyboard_target(
                [cb = std::move(char_cb)](WCHAR ch) mutable {
                    return cb ? cb(static_cast<wchar_t>(ch)) : false;
                },
                [cb = std::move(key_cb)](WPARAM key, LPARAM) mutable {
                    return cb ? cb(key_from_native(static_cast<uintptr_t>(key))) : false;
                },
                {},
                std::move(defocus_cb));
        }

        // 设置 IME 光标位置回调（返回光标在窗口中的 x, y 坐标）
        void set_ime_cursor_callback(std::function<std::pair<float,float>()> cb)
        {
            ime_cursor_cb_ = std::move(cb);
        }

        // 注册原始鼠标移动回调（在图层事件分发之前触发，适合跨图层拖拽）
        // cb(x, y, left_button_down)
        void on_raw_mouse_move(std::function<void(float,float,bool)> cb)
        {
            raw_mouse_move_cb_ = std::move(cb);
        }

        void clear_keyboard_target()
        {
            if (keyboard_defocus_cb_) keyboard_defocus_cb_();
            keyboard_defocus_cb_ = {};
            keyboard_char_cb_ = {};
            keyboard_key_cb_  = {};
            keyboard_key_up_cb_ = {};
            ime_cursor_cb_    = {};
            ImmAssociateContext(hwnd_, nullptr);
        }

        // ── 原生 Edit 控件回调（WM_CTLCOLOREDIT 和 WM_COMMAND 发给父窗口）
        //    用 SetPropW 挂在 Edit HWND 上，避免头文件循环依赖
        using EditProcCb = LRESULT(*)(HWND, UINT, WPARAM, LPARAM, HDC);
        void register_edit_cb(HWND h, EditProcCb cb)
        {
            SetPropW(h, L"OtterEditCb", (HANDLE)cb);
        }
        HBRUSH invoke_edit_color(HWND h, HDC dc)
        {
            auto cb = (EditProcCb)GetPropW(h, L"OtterEditCb");
            if (cb) return (HBRUSH)cb(h, WM_CTLCOLOREDIT, 0, 0, dc);
            return nullptr;
        }
        void invoke_edit_notify(HWND h, UINT code)
        {
            auto cb = (EditProcCb)GetPropW(h, L"OtterEditCb");
            if (cb) cb(h, WM_COMMAND, code, 0, nullptr);
        }

    private:
        void initialize_renderer()
        {
            switch (backend_)
            {
#ifndef OTTER_DISABLE_D2D
            case RenderBackend::Direct2D:
                renderer_ = std::make_unique<D2DRenderContext>();
                OTTER_LOG_INFO("renderer", "selected Direct2D backend");
                break;
#endif
            case RenderBackend::OpenGL:
                renderer_ = std::make_unique<OpenGLRenderContext>();
                OTTER_LOG_INFO("renderer", "selected OpenGL backend");
                break;
            }
            renderer_->initialize_native(hwnd_, static_cast<unsigned int>(width_), static_cast<unsigned int>(height_));
            renderer_->set_resize_clear_color(clear_color_);
            renderer_->set_color_key_mode(color_key_mode_);
        }

        void update_auto_scroll()
        {
            auto b = canvas_.content_bounds();

            float cw = std::fmax((float)width_,  b.max_x);
            float ch = std::fmax((float)height_, b.max_y);

            bool overflow = cw > (float)width_ + 1.f || ch > (float)height_ + 1.f;

            if (overflow)
            {
                ScrollConfig sc = auto_scroll_cfg_.value_or(ScrollConfig{});
                sc.content_width  = cw;
                sc.content_height = ch;
                canvas_.layer_bounds(0.f, 0.f, (float)width_, (float)height_);
                canvas_.enable_scroll(sc);
            }
            else
            {
                if (canvas_.scroll_config())
                    canvas_.disable_scroll();
            }
        }

        // ── 私有成员变量 ────────────────────────────────────────
        HWND             hwnd_             = nullptr;
        bool             running_          = false;
        bool             is_focused_       = true;
        int              width_            = 0;   // 物理像素
        int              height_           = 0;   // 物理像素

        bool             borderless_       = false;
        bool             draggable_        = false;
        bool             topmost_          = false;
        bool             dwm_transparent_  = false;
        bool             color_key_mode_   = false;  // 颜色键透明模式
        bool             drop_files_enabled_ = false;  // 文件拖放启用
        bool             resizable_        = true;
        bool             maximizable_      = true;
        bool             continue_on_blur_ = true;
        bool             run_during_drag_  = true;
        int              min_width_        = 0;
        int              min_height_       = 0;
        int              max_width_        = 0;
        int              max_height_       = 0;

        bool             auto_scroll_      = false;
        std::optional<ScrollConfig> auto_scroll_cfg_;

        std::function<bool(WCHAR)> keyboard_char_cb_;
        std::function<bool(WPARAM,LPARAM)> keyboard_key_cb_;
        std::function<bool(WPARAM,LPARAM)> keyboard_key_up_cb_;
        std::function<void()> keyboard_defocus_cb_;
        std::function<std::pair<float,float>()> ime_cursor_cb_;  // IME 光标位置回调
        std::function<void(float,float,bool)> raw_mouse_move_cb_; // 原始鼠标移动回调(x,y,left_down)
        HIMC             default_ime_context_ = nullptr;

        // 窗口生命周期回调
        std::function<void()> ready_cb_;    // 窗口准备就绪（首次 run 时调用）
        std::function<void()> close_cb_;    // 窗口关闭前调用
        bool             ready_called_  = false;  // 避免重复调用

        bool             is_dragging_      = false;
        LARGE_INTEGER    drag_last_time_   = {};
        LARGE_INTEGER    drag_freq_        = {};
        float            window_alpha_     = 1.f;

        Color            clear_color_      = Color{ 0.08f, 0.08f, 0.10f, 1.f };

        Layer            canvas_;
        RenderBackend    backend_ = default_render_backend();
        std::unique_ptr<RenderContext> renderer_;
        AnimManager      anim_manager_;  // 动画管理器

        // ── 帧率控制 ────────────────────────────────────────────────
        int              target_fps_       = 0;     // 目标帧率（0 = 不限制）
        long long        frame_time_ns_    = 0;     // 每帧时间（纳秒）
        int              current_fps_      = 0;     // 当前帧率
        float            frame_time_ms_    = 0.f;   // 当前帧时间（毫秒）
        bool             vsync_            = false; // VSync 模式

        float mouse_x_      = 0;
        float mouse_y_      = 0;
        bool  left_pressed_ = false;
        bool  right_pressed_= false;
        bool  mid_pressed_  = false;
        float ldown_x_ = 0, ldown_y_ = 0;
        float rdown_x_ = 0, rdown_y_ = 0;

        static constexpr const wchar_t* OTTER_WND_CLASS = L"OtterWindow_v4";

        // ── 辅助：构建 MouseEvent（全部物理像素，不做任何换算）
        MouseEvent make_mouse_event(LPARAM lp, WPARAM wp,
                                    float dx = 0, float dy = 0,
                                    float wheel = 0) const
        {
            MouseEvent e;
            e.x           = static_cast<float>(GET_X_LPARAM(lp));
            e.y           = static_cast<float>(GET_Y_LPARAM(lp));
            e.delta_x     = dx;
            e.delta_y     = dy;
            e.wheel_delta = wheel;
            e.left_down   = (wp & MK_LBUTTON)  != 0;
            e.right_down  = (wp & MK_RBUTTON)  != 0;
            e.middle_down = (wp & MK_MBUTTON)  != 0;
            e.ctrl_down   = (wp & MK_CONTROL)  != 0;
            e.shift_down  = (wp & MK_SHIFT)    != 0;
            e.alt_down    = (GetKeyState(VK_MENU) & 0x8000) != 0;
            return e;
        }

        static constexpr float CLICK_TOLERANCE = 5.f;
        static bool is_click(float dx, float dy, float ux, float uy)
        {
            float ddx = ux - dx, ddy = uy - dy;
            return ddx*ddx + ddy*ddy <= CLICK_TOLERANCE * CLICK_TOLERANCE;
        }

    private:
        // ── Win32 消息处理 ────────────────────────────────────────
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg,
                                        WPARAM wp, LPARAM lp)
        {
            otterwindow* self = nullptr;
            if (msg == WM_NCCREATE)
            {
                auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
                self = reinterpret_cast<otterwindow*>(cs->lpCreateParams);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                                  reinterpret_cast<LONG_PTR>(self));
                self->hwnd_ = hwnd;
            }
            else
            {
                self = reinterpret_cast<otterwindow*>(
                    GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            }
            if (self) return self->handle_message(msg, wp, lp);
            return DefWindowProcW(hwnd, msg, wp, lp);
        }

        static void dispatch_message_safely(const MSG& msg)
        {
#if defined(_MSC_VER)
            __try
            {
                DispatchMessageW(&msg);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                // Windows text input framework hidden-window dispatch can raise
                // SEH exceptions on some systems. Keep Otter's frame loop alive.
            }
#else
            DispatchMessageW(&msg);
#endif
        }

        bool is_own_message(const MSG& msg) const
        {
            if (!msg.hwnd)
                return true;
            if (msg.hwnd == hwnd_)
                return true;
            return IsChild(hwnd_, msg.hwnd) == TRUE;
        }

        LRESULT handle_message(UINT msg, WPARAM wp, LPARAM lp)
        {
            switch (msg)
            {
            // 消除非客户区（顶部细线），让客户区完全覆盖整个窗口
            case WM_NCCALCSIZE:
                if (wp && borderless_) return 0;
                return DefWindowProcW(hwnd_, msg, wp, lp);

            // 无边框可拉伸：手动处理命中测试，提供边框热区
            // 有边框模式：直接返回 DefWindowProcW 结果（系统自动处理边框拉伸）
            case WM_NCHITTEST:
            {
                if (!borderless_)
                    return DefWindowProcW(hwnd_, msg, wp, lp);
                LRESULT hit = DefWindowProcW(hwnd_, msg, wp, lp);
                if (hit == HTCLIENT && resizable_)
                {
                    POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                    ScreenToClient(hwnd_, &pt);
                    const int B = 6; // 边框热区宽度（px）
                    bool left   = pt.x < B;
                    bool right  = pt.x >= width_  - B;
                    bool top    = pt.y < B;
                    bool bottom = pt.y >= height_ - B;
                    if (top    && left)  return HTTOPLEFT;
                    if (top    && right) return HTTOPRIGHT;
                    if (bottom && left)  return HTBOTTOMLEFT;
                    if (bottom && right) return HTBOTTOMRIGHT;
                    if (top)    return HTTOP;
                    if (bottom) return HTBOTTOM;
                    if (left)   return HTLEFT;
                    if (right)  return HTRIGHT;
                }
                return hit;
            }

            // 阻止系统擦除背景，避免闪烁
            case WM_ERASEBKGND:
                return 1;

            case WM_SIZE:
            {
                UINT w = LOWORD(lp), h = HIWORD(lp);
                if (w > 0 && h > 0)
                {
                    width_  = (int)w;
                    height_ = (int)h;
                    renderer().resize(w, h);

                    // 更新布局参考尺寸
                    const Otter::LayoutConfig* cfg = canvas_.get_layout_config();
                    if (cfg)
                    {
                        Otter::LayoutConfig updated = *cfg;
                        updated.ref_w = (float)w;
                        updated.ref_h = (float)h;
                        canvas_.set_layout_config(updated);
                    }

                    if (canvas_.scroll_config())
                        canvas_.layer_bounds(0.f, 0.f, (float)w, (float)h);

                    // 自动滚动：窗口尺寸变化后重新检测
                    if (auto_scroll_)
                        update_auto_scroll();

                    // 立即重绘一帧，避免任何颜色闪烁
                    // 使用正确的 clear_color_ 而非默认颜色
                    renderer().begin_frame(clear_color_);
                    canvas_.flush(renderer());
                    renderer().end_frame();
                }
                return 0;
            }

            // DPI 变化时：窗口跟随系统建议位置即可，渲染目标不变（始终96 DPI物理像素）
            case WM_DPICHANGED:
            {
                const RECT* suggested = reinterpret_cast<const RECT*>(lp);
                SetWindowPos(hwnd_, nullptr,
                    suggested->left, suggested->top,
                    suggested->right  - suggested->left,
                    suggested->bottom - suggested->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }

            case WM_SETFOCUS:  is_focused_ = true;  return 0;
            case WM_KILLFOCUS:
                is_focused_ = false;
                canvas_.reset_hover_recursive();
                return 0;

            case WM_ENTERSIZEMOVE:
                is_dragging_ = true;
                QueryPerformanceFrequency(&drag_freq_);
                QueryPerformanceCounter(&drag_last_time_);
                // 启动定时器，16ms ≈ 60fps，确保长按时动画继续
                SetTimer(hwnd_, 1, 16, nullptr);
                // 启用双缓冲，减少调整大小时的闪烁
                // 注意：WS_EX_COMPOSITED 在某些情况下可能导致问题
                // 改用更稳定的方式：设置 SWP_NOCOPYBITS 避免内容复制闪烁
                return 0;

            case WM_EXITSIZEMOVE:
                is_dragging_ = false;
                KillTimer(hwnd_, 1);
                // 立即重绘一帧，确保内容正确
                renderer().begin_frame(clear_color_);
                canvas_.flush(renderer());
                renderer().end_frame();
                return 0;

            case WM_TIMER:
                if (wp == 1 && is_dragging_)
                {
                    LARGE_INTEGER now;
                    QueryPerformanceCounter(&now);
                    float dt = (drag_freq_.QuadPart > 0)
                        ? static_cast<float>(now.QuadPart - drag_last_time_.QuadPart)
                          / static_cast<float>(drag_freq_.QuadPart)
                        : 0.016f;
                    drag_last_time_ = now;
                    dt = std::fmin(dt, 0.1f);
                    anim_manager_.tick(dt);
                    canvas_.tick(dt);
                    renderer().begin_frame(clear_color_);
                    canvas_.flush(renderer());
                    renderer().end_frame();
                }
                return 0;

            case WM_MOVING:
            {
                if (run_during_drag_ && is_dragging_)
                {
                    LARGE_INTEGER now;
                    QueryPerformanceCounter(&now);
                    float dt = (drag_freq_.QuadPart > 0)
                        ? static_cast<float>(now.QuadPart - drag_last_time_.QuadPart)
                          / static_cast<float>(drag_freq_.QuadPart)
                        : 0.016f;
                    drag_last_time_ = now;
                    dt = std::fmin(dt, 0.1f);
                    anim_manager_.tick(dt);  // 更新动画
                    canvas_.tick(dt);
                    renderer().begin_frame(clear_color_);
                    canvas_.flush(renderer());
                    renderer().end_frame();
                }
                return DefWindowProcW(hwnd_, WM_MOVING, wp, lp);
            }

            case WM_SIZING:
            {
                // 从建议窗口矩形计算客户区尺寸
                // 无边框窗口（WS_POPUP）：窗口尺寸 = 客户区尺寸，直接使用窗口矩形
                const RECT* prc = reinterpret_cast<const RECT*>(lp);
                int cw = prc->right - prc->left;
                int ch = prc->bottom - prc->top;

                if (cw > 0 && ch > 0)
                {
                    width_  = cw;
                    height_ = ch;
                    renderer().resize(cw, ch);

                    // 更新布局参考尺寸
                    const Otter::LayoutConfig* cfg = canvas_.get_layout_config();
                    if (cfg)
                    {
                        Otter::LayoutConfig updated = *cfg;
                        updated.ref_w = (float)cw;
                        updated.ref_h = (float)ch;
                        canvas_.set_layout_config(updated);
                    }

                    if (canvas_.scroll_config())
                        canvas_.layer_bounds(0.f, 0.f, (float)cw, (float)ch);

                    if (auto_scroll_)
                        update_auto_scroll();

                    // 立即渲染更新后的内容（resize 期间也需要实时更新）
                    anim_manager_.tick(0.016f);
                    canvas_.tick(0.016f);
                    renderer().begin_frame(clear_color_);
                    canvas_.flush(renderer());
                    renderer().end_frame();
                }
                return DefWindowProcW(hwnd_, WM_SIZING, wp, lp);
            }

            case WM_MOUSEMOVE:
            {
                float nx = static_cast<float>(GET_X_LPARAM(lp));
                float ny = static_cast<float>(GET_Y_LPARAM(lp));
                float dx = nx - mouse_x_;
                float dy = ny - mouse_y_;
                mouse_x_ = nx; mouse_y_ = ny;
                if (raw_mouse_move_cb_)
                    raw_mouse_move_cb_(nx, ny, left_pressed_);
                MouseEvent e = make_mouse_event(lp, wp);
                e.delta_x = dx;
                e.delta_y = dy;
                canvas_.dispatch_mouse_move(e);
                return 0;
            }

            case WM_MOUSELEAVE:
                canvas_.reset_hover_recursive();
                left_pressed_ = false;
                right_pressed_ = false;
                mid_pressed_ = false;
                return 0;

            case WM_CAPTURECHANGED:
                left_pressed_ = false;
                right_pressed_ = false;
                mid_pressed_ = false;
                return 0;

            case WM_LBUTTONDOWN:
            {
                left_pressed_ = true;
                ldown_x_ = static_cast<float>(GET_X_LPARAM(lp));
                ldown_y_ = static_cast<float>(GET_Y_LPARAM(lp));
                bool consumed = canvas_.dispatch_mouse_down(make_mouse_event(lp, wp));
                if (!consumed && draggable_)
                    SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, lp);
                else
                    SetCapture(hwnd_);
                return 0;
            }

            case WM_LBUTTONUP:
            {
                ReleaseCapture();
                left_pressed_ = false;
                float ux = static_cast<float>(GET_X_LPARAM(lp));
                float uy = static_cast<float>(GET_Y_LPARAM(lp));
                bool click = is_click(ldown_x_, ldown_y_, ux, uy);
                canvas_.dispatch_mouse_up(make_mouse_event(lp, wp), click, false);
                return 0;
            }

            case WM_LBUTTONDBLCLK:
            {
                MouseEvent e = make_mouse_event(lp, wp);
                e.click_count = 2;
                canvas_.dispatch_double_click(e);
                return 0;
            }

            case WM_RBUTTONDOWN:
            {
                right_pressed_ = true;
                rdown_x_ = static_cast<float>(GET_X_LPARAM(lp));
                rdown_y_ = static_cast<float>(GET_Y_LPARAM(lp));
                canvas_.dispatch_mouse_down(make_mouse_event(lp, wp));
                return 0;
            }

            case WM_RBUTTONUP:
            {
                right_pressed_ = false;
                float ux = static_cast<float>(GET_X_LPARAM(lp));
                float uy = static_cast<float>(GET_Y_LPARAM(lp));
                bool click = is_click(rdown_x_, rdown_y_, ux, uy);
                canvas_.dispatch_mouse_up(make_mouse_event(lp, wp), click, true);
                return 0;
            }

            case WM_MBUTTONDOWN:
                mid_pressed_ = true;
                canvas_.dispatch_mouse_down(make_mouse_event(lp, wp));
                return 0;

            case WM_MBUTTONUP:
                mid_pressed_ = false;
                canvas_.dispatch_mouse_up(make_mouse_event(lp, wp), false, false);
                return 0;

            case WM_MOUSEWHEEL:
            {
                POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                ScreenToClient(hwnd_, &pt);
                float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp))
                            / static_cast<float>(WHEEL_DELTA);
                MouseEvent e{};
                e.x           = (float)pt.x;
                e.y           = (float)pt.y;
                e.wheel_delta = delta;
                e.left_down   = left_pressed_;
                e.right_down  = right_pressed_;
                e.middle_down = mid_pressed_;
                e.ctrl_down   = (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) != 0;
                e.shift_down  = (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT)   != 0;
                canvas_.dispatch_wheel(e);
                return 0;
            }

            // ── 原生 Edit 控件颜色（发给父窗口）
            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORSTATIC:
            {
                HWND hEdit = (HWND)lp;
                HDC hdc    = (HDC)wp;
                // 先尝试回调机制
                HBRUSH br = invoke_edit_color(hEdit, hdc);
                if (br) return (LRESULT)br;
                // 再尝试直接读取属性
                auto* brush = (HBRUSH)GetPropW(hEdit, L"OtterEditBgBrush");
                if (brush)
                {
                    auto txt_col = (COLORREF)(UINT_PTR)GetPropW(hEdit, L"OtterEditTextCol");
                    auto bg_col  = (COLORREF)(UINT_PTR)GetPropW(hEdit, L"OtterEditBgCol");
                    SetTextColor(hdc, txt_col);
                    SetBkColor(hdc, bg_col);
                    return (LRESULT)brush;
                }
                return DefWindowProcW(hwnd_, msg, wp, lp);
            }

            // ── 原生 Edit 控件通知（发给父窗口）
            case WM_COMMAND:
            {
                HWND hEdit = (HWND)lp;
                UINT code  = HIWORD(wp);
                if (code == EN_CHANGE || code == EN_KILLFOCUS || code == EN_SETFOCUS)
                {
                    invoke_edit_notify(hEdit, code);
                    return 0;
                }
                return DefWindowProcW(hwnd_, msg, wp, lp);
            }

            case WM_KEYDOWN:
                if (wp == VK_ESCAPE) { running_ = false; DestroyWindow(hwnd_); }
                if (keyboard_key_cb_ && keyboard_key_cb_(wp, lp)) return 0;
                // Let DefWindowProcW handle WM_KEYDOWN so TranslateMessage can generate WM_CHAR
                return DefWindowProcW(hwnd_, msg, wp, lp);

            case WM_SYSKEYDOWN:
                if (keyboard_key_cb_ && keyboard_key_cb_(wp, lp)) return 0;
                return DefWindowProcW(hwnd_, msg, wp, lp);

            case WM_KEYUP:
                if (keyboard_key_up_cb_ && keyboard_key_up_cb_(wp, lp)) return 0;
                return DefWindowProcW(hwnd_, msg, wp, lp);

            case WM_SYSKEYUP:
                if (keyboard_key_up_cb_ && keyboard_key_up_cb_(wp, lp)) return 0;
                return DefWindowProcW(hwnd_, msg, wp, lp);

            case WM_CHAR:
                if (keyboard_char_cb_) keyboard_char_cb_((WCHAR)wp);
                return 0;

            case WM_SYSCHAR:
                if (keyboard_char_cb_ && keyboard_char_cb_((WCHAR)wp)) return 0;
                return DefWindowProcW(hwnd_, msg, wp, lp);

            case WM_IME_STARTCOMPOSITION:
            {
                // 设置输入法候选窗口位置
                if (ime_cursor_cb_)
                {
                    auto [cx, cy] = ime_cursor_cb_();
                    HIMC himc = ImmGetContext(hwnd_);
                    if (himc)
                    {
                        COMPOSITIONFORM cf;
                        cf.dwStyle = CFS_POINT;
                        cf.ptCurrentPos.x = static_cast<LONG>(cx);
                        cf.ptCurrentPos.y = static_cast<LONG>(cy);
                        ImmSetCompositionWindow(himc, &cf);
                        ImmReleaseContext(hwnd_, himc);
                    }
                }
                return 0;
            }

            case WM_GETMINMAXINFO:
            {
                MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lp);
                if (min_width_  > 0) mmi->ptMinTrackSize.x = min_width_;
                if (min_height_ > 0) mmi->ptMinTrackSize.y = min_height_;
                if (max_width_  > 0) mmi->ptMaxTrackSize.x = max_width_;
                if (max_height_ > 0) mmi->ptMaxTrackSize.y = max_height_;
                return 0;
            }

            case WM_DESTROY:
                trigger_close();  // 窗口关闭前回调
                OTTER_LOG_INFO("window", "WM_DESTROY received");
                running_ = false;
                PostQuitMessage(0);
                return 0;

            // ── 文件拖放 ───────────────────────────────────────────
            case WM_DROPFILES:
            {
                HDROP hDrop = (HDROP)wp;
                UINT  nFiles = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);

                std::vector<std::wstring> files;
                files.reserve(nFiles);
                for (UINT i = 0; i < nFiles; ++i)
                {
                    WCHAR buf[MAX_PATH];
                    DragQueryFileW(hDrop, i, buf, MAX_PATH);
                    files.push_back(buf);
                }

                POINT pt;
                DragQueryPoint(hDrop, &pt);
                DragFinish(hDrop);

                // 构建事件并分发到图层树
                MouseEvent e;
                e.x = (float)pt.x;
                e.y = (float)pt.y;

                canvas_.dispatch_drop_files(files, e);
                return 0;
            }

            default:
                return DefWindowProcW(hwnd_, msg, wp, lp);
            }
        }

        static void register_window_class()
        {
            static bool done = false;
            if (done) return;

            // ── DPI 声明：必须在创建任何窗口之前调用 ────────────────
            // vcxproj 里的 <DPIAwareness> 放在 <Link> 节点下，VS 不写进
            // manifest，实际无效。用代码调用是唯一可靠的方式。
            // V2：每显示器独立 DPI，系统不做任何位图拉伸，我们拿到原始物理像素。
            // 若进程已声明过（manifest 或其他代码），此调用返回 FALSE 但无害。
            using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
            HMODULE user32 = GetModuleHandleW(L"user32.dll");
            auto set_dpi_awareness_context = user32
                ? reinterpret_cast<SetProcessDpiAwarenessContextFn>(
                    GetProcAddress(user32, "SetProcessDpiAwarenessContext"))
                : nullptr;
            if (set_dpi_awareness_context)
                set_dpi_awareness_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            WNDCLASSEXW wc  = {};
            wc.cbSize       = sizeof(wc);
            wc.style        = CS_DBLCLKS | CS_OWNDC;  // OpenGL/WGL requires a stable window DC.
            wc.lpfnWndProc  = WndProc;
            wc.hInstance    = GetModuleHandleW(nullptr);
            wc.hCursor      = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
            wc.hbrBackground= nullptr;
            wc.lpszClassName= OTTER_WND_CLASS;
            wc.hIcon        = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
            wc.hIconSm      = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
            if (!RegisterClassExW(&wc))
                throw std::runtime_error("RegisterClassExW 失败");
            done = true;
        }
    };

} // namespace Otter
