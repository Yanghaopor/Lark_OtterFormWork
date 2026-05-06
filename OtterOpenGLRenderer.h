#pragma once

#ifndef _WIN32
#error "OtterOpenGLRenderer.h currently uses the Win32 WGL/GDI OpenGL backend. Add a GL context provider for the target platform before including this backend."
#endif

#include "OtterLayer.h"
#include "OtterDebug.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <gl/GL.h>
#include <wincodec.h>
#include <winhttp.h>

#pragma comment(lib, "opengl32")
#pragma comment(lib, "gdi32")
#pragma comment(lib, "windowscodecs")
#pragma comment(lib, "winhttp")

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef OTTER_PI
#define OTTER_PI 3.14159265358979323846f
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_TEXTURE_BINDING_2D
#define GL_TEXTURE_BINDING_2D 0x8069
#endif

namespace Otter
{
    class OpenGLRenderContext : public RenderContext
    {
    public:
        OpenGLRenderContext() = default;
        ~OpenGLRenderContext() noexcept { release_all(); }

        OpenGLRenderContext(const OpenGLRenderContext&) = delete;
        OpenGLRenderContext& operator=(const OpenGLRenderContext&) = delete;

        void initialize_native(void* hwnd, unsigned int width, unsigned int height) override
        {
            initialize(static_cast<HWND>(hwnd), width, height);
        }

        void initialize(HWND hwnd, UINT width, UINT height)
        {
            hwnd_ = hwnd;
            width_ = width;
            height_ = height;
            SetEnvironmentVariableW(L"__GL_THREADED_OPTIMIZATIONS", L"0");
            dc_ = GetDC(hwnd_);
            if (!dc_)
                throw std::runtime_error("OpenGL GetDC failed");

            PIXELFORMATDESCRIPTOR pfd{};
            pfd.nSize = sizeof(pfd);
            pfd.nVersion = 1;
            pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
            pfd.iPixelType = PFD_TYPE_RGBA;
            pfd.cColorBits = 32;
            pfd.cDepthBits = 0;
            pfd.cStencilBits = 8;
            pfd.iLayerType = PFD_MAIN_PLANE;

            const int pixel_format = ChoosePixelFormat(dc_, &pfd);
            if (pixel_format == 0 || !SetPixelFormat(dc_, pixel_format, &pfd))
                throw std::runtime_error("OpenGL SetPixelFormat failed");

            glrc_ = wglCreateContext(dc_);
            if (!glrc_ || !wglMakeCurrent(dc_, glrc_))
                throw std::runtime_error("OpenGL context creation failed");

            const HRESULT com_hr = CoInitialize(nullptr);
            if (com_hr == S_OK || com_hr == S_FALSE)
                com_initialized_ = true;
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glEnable(GL_LINE_SMOOTH);
            glDisable(GL_POLYGON_SMOOTH);
            glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
            glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            load_shader_entry_points();
            output_driver_info();
            resize(width_, height_);
        }

        void release_all()
        {
            if (dc_ && glrc_)
            {
                make_current();
                release_shader_layers();
                for (auto& item : shader_programs_)
                {
                    if (item.second)
                        glDeleteProgram_(item.second);
                }
                shader_programs_.clear();
                for (auto& item : bitmap_cache_)
                {
                    if (item.second.texture)
                        glDeleteTextures(1, &item.second.texture);
                }
                bitmap_cache_.clear();
                for (auto& item : text_cache_)
                {
                    if (item.second.texture)
                        glDeleteTextures(1, &item.second.texture);
                }
                text_cache_.clear();
                for (auto& item : dynamic_bgra_cache_)
                {
                    if (item.second.texture.texture)
                        glDeleteTextures(1, &item.second.texture.texture);
                }
                dynamic_bgra_cache_.clear();
            }

            if (glrc_)
            {
                wglMakeCurrent(nullptr, nullptr);
                wglDeleteContext(glrc_);
                glrc_ = nullptr;
            }
            if (dc_ && hwnd_)
            {
                ReleaseDC(hwnd_, dc_);
                dc_ = nullptr;
            }
            if (wic_factory_)
            {
                wic_factory_->Release();
                wic_factory_ = nullptr;
            }
            if (com_initialized_)
            {
                CoUninitialize();
                com_initialized_ = false;
            }
            hwnd_ = nullptr;
        }

        void resize(UINT width, UINT height) override
        {
            width_ = (std::max)(1u, width);
            height_ = (std::max)(1u, height);
            make_current();
            glViewport(0, 0, static_cast<GLsizei>(width_), static_cast<GLsizei>(height_));
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glOrtho(0.0, static_cast<double>(width_), static_cast<double>(height_), 0.0, -1.0, 1.0);
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();
            release_shader_layers();
        }

        void set_resize_clear_color(const Color& color) override
        {
            resize_clear_color_ = color;
        }

        void set_color_key_mode(bool enable) override
        {
            color_key_mode_ = enable;
        }

        bool supports_layer_shader() const override
        {
            return shader_api_available();
        }

        unsigned int create_shader(std::string_view fragment_source,
                                   std::string_view vertex_source = {}) override
        {
            if (!shader_api_available() || fragment_source.empty())
                return 0;

            static constexpr const char* kDefaultVertex =
                "#version 120\n"
                "varying vec2 vTexCoord;\n"
                "void main() {\n"
                "    vTexCoord = gl_MultiTexCoord0.xy;\n"
                "    gl_Position = ftransform();\n"
                "}\n";

            const GLuint vertex = compile_shader(
                GL_VERTEX_SHADER,
                vertex_source.empty() ? std::string_view(kDefaultVertex) : vertex_source);
            if (!vertex)
                return 0;

            const GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
            if (!fragment)
            {
                glDeleteShader_(vertex);
                return 0;
            }

            const GLuint program = glCreateProgram_();
            glAttachShader_(program, vertex);
            glAttachShader_(program, fragment);
            glLinkProgram_(program);
            GLint linked = 0;
            glGetProgramiv_(program, GL_LINK_STATUS, &linked);
            glDeleteShader_(vertex);
            glDeleteShader_(fragment);

            if (!linked)
            {
                OTTER_LOG_ERROR("shader", "OpenGL shader program link failed");
                glDeleteProgram_(program);
                return 0;
            }

            const unsigned int id = next_shader_id_++;
            shader_programs_[id] = program;
            return id;
        }

        void destroy_shader(unsigned int shader_id) override
        {
            auto it = shader_programs_.find(shader_id);
            if (it == shader_programs_.end())
                return;
            if (it->second && glDeleteProgram_)
                glDeleteProgram_(it->second);
            shader_programs_.erase(it);
        }

        bool begin_layer_shader(unsigned int shader_id, float width, float height) override
        {
            if (!shader_api_available() || shader_id == 0 || width <= 0.f || height <= 0.f)
                return false;
            if (shader_programs_.find(shader_id) == shader_programs_.end())
                return false;
            ShaderLayer* layer = acquire_shader_layer(
                static_cast<int>(std::ceil(width)),
                static_cast<int>(std::ceil(height)));
            if (!layer || !layer->valid())
                return false;

            GLint viewport[4]{};
            glGetIntegerv(GL_VIEWPORT, viewport);
            shader_stack_.push_back({ shader_id, layer, viewport[0], viewport[1], viewport[2], viewport[3] });

            glBindFramebuffer_(GL_FRAMEBUFFER, layer->framebuffer);
            glViewport(0, 0, layer->width, layer->height);
            glMatrixMode(GL_PROJECTION);
            glPushMatrix();
            glLoadIdentity();
            glOrtho(0.0, static_cast<double>(layer->width), static_cast<double>(layer->height), 0.0, -1.0, 1.0);
            glMatrixMode(GL_MODELVIEW);
            glPushMatrix();
            glLoadIdentity();
            glClearColor(0.f, 0.f, 0.f, 0.f);
            glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            return true;
        }

        void end_layer_shader(unsigned int shader_id) override
        {
            if (shader_stack_.empty())
                return;
            ShaderInvocation invocation = shader_stack_.back();
            shader_stack_.pop_back();
            if (shader_id != invocation.shader_id || !invocation.layer)
                return;

            glMatrixMode(GL_MODELVIEW);
            glPopMatrix();
            glMatrixMode(GL_PROJECTION);
            glPopMatrix();
            glBindFramebuffer_(GL_FRAMEBUFFER, shader_stack_.empty() ? 0u : shader_stack_.back().layer->framebuffer);
            glViewport(invocation.viewport_x, invocation.viewport_y, invocation.viewport_w, invocation.viewport_h);

            auto program = shader_programs_.find(shader_id);
            if (program == shader_programs_.end() || !program->second)
                return;

            glMatrixMode(GL_PROJECTION);
            glPushMatrix();
            glLoadIdentity();
            const int target_w = shader_stack_.empty() ? static_cast<int>(width_) : shader_stack_.back().layer->width;
            const int target_h = shader_stack_.empty() ? static_cast<int>(height_) : shader_stack_.back().layer->height;
            glOrtho(0.0, static_cast<double>(target_w), static_cast<double>(target_h), 0.0, -1.0, 1.0);
            glMatrixMode(GL_MODELVIEW);
            glPushMatrix();
            glMatrixMode(GL_TEXTURE);
            glPushMatrix();
            glLoadIdentity();
            glMatrixMode(GL_MODELVIEW);

            glDisable(GL_STENCIL_TEST);
            glEnable(GL_TEXTURE_2D);
            glActiveTexture_(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, invocation.layer->texture);
            glUseProgram_(program->second);
            set_shader_uniforms(program->second, invocation.layer->width, invocation.layer->height);
            glColor4f(1.f, 1.f, 1.f, 1.f);
            draw_textured_quad_flipped_y(0.f, 0.f, static_cast<float>(invocation.layer->width), static_cast<float>(invocation.layer->height), 1.f, 1.f);
            glUseProgram_(0);
            glBindTexture(GL_TEXTURE_2D, 0);
            glDisable(GL_TEXTURE_2D);

            glMatrixMode(GL_MODELVIEW);
            glPopMatrix();
            glMatrixMode(GL_TEXTURE);
            glPopMatrix();
            glMatrixMode(GL_PROJECTION);
            glPopMatrix();
            glMatrixMode(GL_MODELVIEW);
            rebuild_stencil_clip();
        }

        void begin_frame(Color clear_color = Color{ 0.08f, 0.08f, 0.1f, 1.f }) override
        {
            make_current();
            resize(width_, height_);
            const Color c = convert_color(clear_color);
            glClearColor(c.r, c.g, c.b, c.a);
            glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            glDisable(GL_TEXTURE_2D);
            glDisable(GL_STENCIL_TEST);
            transform_stack_depth_ = 0;
            opacity_stack_.clear();
            opacity_stack_.push_back(1.f);
            blend_stack_.clear();
            blend_stack_.push_back(BlendMode::Normal);
            clip_stack_.clear();
            path_.clear();
            current_path_closed_ = false;
        }

        bool end_frame() override
        {
#if defined(_MSC_VER)
            __try
            {
                glFlush();
                return SwapBuffers(dc_) == TRUE;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
#else
            glFlush();
            return SwapBuffers(dc_) == TRUE;
#endif
        }

        void push_transform(float tx, float ty, float sx = 1.f, float sy = 1.f, float rot = 0.f) override
        {
            glPushMatrix();
            ++transform_stack_depth_;
            glTranslatef(tx, ty, 0.f);
            if (rot != 0.f)
                glRotatef(rot * 57.2957795f, 0.f, 0.f, 1.f);
            glScalef(sx, sy, 1.f);
        }

        void pop_transform() override
        {
            if (transform_stack_depth_ > 0)
            {
                glPopMatrix();
                --transform_stack_depth_;
            }
        }

        void push_style(const LayerStyle& style) override
        {
            opacity_stack_.push_back(current_opacity() * std::clamp(style.opacity, 0.f, 1.f));
            blend_stack_.push_back(style.blend);
            apply_blend(style.blend);
        }

        void pop_style() override
        {
            if (opacity_stack_.size() > 1)
                opacity_stack_.pop_back();
            if (blend_stack_.size() > 1)
                blend_stack_.pop_back();
            apply_blend(blend_stack_.empty() ? BlendMode::Normal : blend_stack_.back());
        }

        void cmd_move_to(float x, float y) override
        {
            path_.clear();
            push_path_point({ x, y });
            current_path_closed_ = false;
        }

        void cmd_line_to(float x, float y) override
        {
            push_path_point({ x, y });
        }

        void cmd_bezier_to(float cx1, float cy1, float cx2, float cy2, float ex, float ey) override
        {
            if (path_.empty())
            {
                push_path_point({ ex, ey });
                return;
            }
            const Point p0 = path_.back();
            for (int i = 1; i <= 28; ++i)
            {
                const float t = static_cast<float>(i) / 28.f;
                const float mt = 1.f - t;
                push_path_point({
                    mt * mt * mt * p0.x + 3.f * mt * mt * t * cx1 + 3.f * mt * t * t * cx2 + t * t * t * ex,
                    mt * mt * mt * p0.y + 3.f * mt * mt * t * cy1 + 3.f * mt * t * t * cy2 + t * t * t * ey
                });
            }
        }

        void cmd_arc(float cx, float cy, float radius, float start_angle, float end_angle) override
        {
            if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(radius)
                || !std::isfinite(start_angle) || !std::isfinite(end_angle)
                || radius <= 0.f)
            {
                path_.clear();
                current_path_closed_ = false;
                return;
            }
            const float sweep = end_angle - start_angle;
            const int segments = std::clamp(
                static_cast<int>(std::fabs(sweep) / (OTTER_PI / 64.f)),
                32,
                256);
            path_.clear();
            for (int i = 0; i <= segments; ++i)
            {
                const float t = static_cast<float>(i) / static_cast<float>(segments);
                const float a = start_angle + sweep * t;
                push_path_point({ cx + std::cos(a) * radius, cy + std::sin(a) * radius });
            }
            current_path_closed_ = std::fabs(std::fabs(sweep) - 2.f * OTTER_PI) < 0.01f;
        }

        void cmd_close_path() override
        {
            current_path_closed_ = true;
        }

        void cmd_fill(const FillStyle& style) override
        {
            if (path_.size() < 2)
                return;
            draw_polygon(path_, style);
        }

        void cmd_stroke(const StrokeStyle& style) override
        {
            if (path_.size() < 2 || style.width <= 0.f)
                return;
            glDisable(GL_TEXTURE_2D);
            set_gl_color(style.color);
            glLineWidth(style.width);
            glBegin(current_path_closed_ ? GL_LINE_LOOP : GL_LINE_STRIP);
            for (const Point& p : path_)
                glVertex2f(p.x, p.y);
            glEnd();
            glLineWidth(1.f);
        }

        void cmd_shadow(const ShadowStyle& style) override
        {
            pending_shadow_ = style;
        }

        void cmd_fill_round_rect(float x, float y, float w, float h, float radius, const FillStyle& style) override
        {
            const std::vector<Point> points = rounded_rect_points(x, y, w, h, radius);
            if (points.empty())
                return;
            if (pending_shadow_)
            {
                ShadowStyle shadow = *pending_shadow_;
                pending_shadow_.reset();
                FillStyle shadow_fill{ shadow.color };
                draw_polygon(
                    offset_points(points, shadow.offset_x, shadow.offset_y),
                    shadow_fill,
                    std::nullopt,
                    shadow.color.a);
            }
            draw_polygon(points, style, Point{ x + w * 0.5f, y + h * 0.5f });
        }

        void cmd_stroke_round_rect(float x, float y, float w, float h, float radius, const StrokeStyle& style) override
        {
            if (style.width <= 0.f)
                return;
            const std::vector<Point> points = rounded_rect_points(x, y, w, h, radius);
            if (points.empty())
                return;
            glDisable(GL_TEXTURE_2D);
            set_gl_color(style.color);
            glLineWidth(style.width);
            glBegin(GL_LINE_LOOP);
            for (const Point& p : points)
                glVertex2f(p.x, p.y);
            glEnd();
            glLineWidth(1.f);
        }

        void cmd_push_round_clip(float x, float y, float w, float h, float radius) override
        {
            std::vector<Point> points = rounded_rect_points(x, y, w, h, radius);
            for (Point& point : points)
                point = transform_point(point);
            clip_stack_.push_back({ std::move(points) });
            rebuild_stencil_clip();
        }

        void cmd_pop_clip() override
        {
            if (!clip_stack_.empty())
                clip_stack_.pop_back();
            rebuild_stencil_clip();
        }

        void cmd_blur_rect(float x, float y, float w, float h, float radius) override
        {
            if (radius <= 0.f)
                return;
            constexpr int layers = 5;
            for (int i = 0; i < layers; ++i)
            {
                const float t = static_cast<float>(i) / static_cast<float>(layers - 1);
                const float spread = radius * (0.25f + t * 0.75f);
                Color overlay = Color::from_rgb_hex(0xFFFFFF, 0.035f * (1.f - t));
                FillStyle fill{ overlay };
                cmd_fill_round_rect(
                    x - spread,
                    y - spread,
                    w + spread * 2.f,
                    h + spread * 2.f,
                    radius + spread,
                    fill);
            }
        }

        void cmd_feather_rect(float x, float y, float w, float h, float radius, float feather) override
        {
            if (feather <= 0.f)
                return;
            constexpr int steps = 6;
            for (int i = 0; i < steps; ++i)
            {
                const float t = static_cast<float>(i) / static_cast<float>(steps);
                StrokeStyle stroke;
                stroke.width = feather / static_cast<float>(steps);
                stroke.color = Color::from_rgb_hex(0xFFFFFF, 0.10f * (1.f - t));
                cmd_stroke_round_rect(
                    x + feather * t * 0.5f,
                    y + feather * t * 0.5f,
                    w - feather * t,
                    h - feather * t,
                    (std::max)(0.f, radius - feather * t * 0.5f),
                    stroke);
            }
        }

        void cmd_text(const std::wstring& content, float x, float y, float max_width, float max_height, const TextStyle& style) override
        {
            if (content.empty())
                return;

            TextMetrics metrics{};
            cmd_measure_text(content, max_width, max_height, style, metrics);
            float draw_x = x;
            float draw_y = y;
            if (max_width > 0.f)
            {
                if (style.h_align == TextStyle::HAlign::Center)
                    draw_x += (max_width - metrics.width) * 0.5f;
                else if (style.h_align == TextStyle::HAlign::Right)
                    draw_x += max_width - metrics.width;
            }
            if (max_height > 0.f)
            {
                if (style.v_align == TextStyle::VAlign::Middle)
                    draw_y += (max_height - metrics.height) * 0.5f;
                else if (style.v_align == TextStyle::VAlign::Bottom)
                    draw_y += max_height - metrics.height;
            }

            if (style.shadow)
            {
                TextStyle shadow_style = style;
                shadow_style.color = style.shadow->color;
                shadow_style.shadow.reset();
                draw_text_texture(content, draw_x + style.shadow->offset_x, draw_y + style.shadow->offset_y, metrics, max_width, max_height, shadow_style);
            }
            if (style.stroke_width > 0.f && style.stroke_color.a > 0.f)
            {
                TextStyle stroke_style = style;
                stroke_style.color = style.stroke_color;
                stroke_style.shadow.reset();
                const int spread = (std::max)(1, static_cast<int>(std::ceil(style.stroke_width)));
                for (int dy = -spread; dy <= spread; ++dy)
                    for (int dx = -spread; dx <= spread; ++dx)
                        if (dx != 0 || dy != 0)
                            draw_text_texture(content, draw_x + static_cast<float>(dx), draw_y + static_cast<float>(dy), metrics, max_width, max_height, stroke_style);
            }
            draw_text_texture(content, draw_x, draw_y, metrics, max_width, max_height, style);
        }

        bool cmd_measure_text(const std::wstring& content, float max_width, float max_height, const TextStyle& style, TextMetrics& out) override
        {
            if (content.empty())
            {
                out = {};
                return true;
            }

            HFONT font = create_font(style);
            HFONT old_font = static_cast<HFONT>(SelectObject(dc_, font));
            RECT rect{
                0,
                0,
                static_cast<LONG>(max_width > 0.f ? max_width : 100000.f),
                static_cast<LONG>(max_height > 0.f ? max_height : 100000.f)
            };
            DrawTextW(dc_, content.c_str(), static_cast<int>(content.size()), &rect,
                DT_CALCRECT | DT_NOPREFIX | (style.word_wrap ? DT_WORDBREAK : DT_SINGLELINE));
            TEXTMETRICW tm{};
            GetTextMetricsW(dc_, &tm);
            SelectObject(dc_, old_font);
            DeleteObject(font);

            out.width = static_cast<float>(rect.right - rect.left);
            out.height = static_cast<float>(rect.bottom - rect.top);
            out.line_height = style.line_spacing > 0.f ? style.line_spacing : static_cast<float>(tm.tmHeight);
            out.line_count = (std::max)(1, static_cast<int>(std::round(out.height / (std::max)(1.f, out.line_height))));
            out.truncated = max_height > 0.f && out.height > max_height;
            return true;
        }

        void cmd_draw_bitmap(const std::wstring& path, float x, float y, float w, float h, float opacity, float radius) override
        {
            Texture texture = load_bitmap_texture(path);
            if (!texture.valid())
                return;

            if (radius > 0.f)
                cmd_push_round_clip(x, y, w, h, radius);

            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, texture.texture);
            glColor4f(1.f, 1.f, 1.f, current_opacity() * std::clamp(opacity, 0.f, 1.f));
            draw_textured_quad(x, y, w, h, texture.u, texture.v);
            glDisable(GL_TEXTURE_2D);

            if (radius > 0.f)
                cmd_pop_clip();
        }

        void cmd_draw_bitmap_bgra(const uint8_t* pixels, int width, int height, int stride,
                                  float x, float y, float w, float h,
                                  float opacity, float radius) override
        {
            Texture texture = upload_texture_bgra(pixels, width, height, stride);
            if (!texture.valid())
                return;

            if (radius > 0.f)
                cmd_push_round_clip(x, y, w, h, radius);

            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, texture.texture);
            glColor4f(1.f, 1.f, 1.f, current_opacity() * std::clamp(opacity, 0.f, 1.f));
            draw_textured_quad(x, y, w, h, texture.u, texture.v);
            glBindTexture(GL_TEXTURE_2D, 0);
            glDisable(GL_TEXTURE_2D);
            glDeleteTextures(1, &texture.texture);

            if (radius > 0.f)
                cmd_pop_clip();
        }

        void cmd_draw_bitmap_bgra_cached(uint64_t cache_key, uint64_t revision,
                                         const uint8_t* pixels, int width, int height, int stride,
                                         const std::vector<BitmapUpdateRect>& update_rects,
                                         float x, float y, float w, float h,
                                         float opacity, float radius) override
        {
            Texture texture = update_dynamic_bgra_texture(cache_key, revision, pixels, width, height, stride, update_rects);
            if (!texture.valid())
                return;

            if (radius > 0.f)
                cmd_push_round_clip(x, y, w, h, radius);

            glDisable(GL_POLYGON_SMOOTH);
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, texture.texture);
            glColor4f(1.f, 1.f, 1.f, current_opacity() * std::clamp(opacity, 0.f, 1.f));
            draw_textured_quad(x, y, w, h, texture.u, texture.v);
            glBindTexture(GL_TEXTURE_2D, 0);
            glDisable(GL_TEXTURE_2D);

            if (radius > 0.f)
                cmd_pop_clip();
        }

    private:
        struct Point
        {
            float x = 0.f;
            float y = 0.f;
        };

        struct ClipEntry
        {
            std::vector<Point> points;
        };

        struct Texture
        {
            GLuint texture = 0;
            int width = 0;
            int height = 0;
            int tex_width = 0;
            int tex_height = 0;
            float u = 1.f;
            float v = 1.f;
            bool valid() const { return texture != 0 && width > 0 && height > 0; }
        };

        struct DynamicBgraTexture
        {
            Texture texture;
            uint64_t revision = 0;
        };

        struct ShaderLayer
        {
            GLuint framebuffer = 0;
            GLuint texture = 0;
            int width = 0;
            int height = 0;
            bool valid() const { return framebuffer != 0 && texture != 0 && width > 0 && height > 0; }
        };

        struct ShaderInvocation
        {
            unsigned int shader_id = 0;
            ShaderLayer* layer = nullptr;
            GLint viewport_x = 0;
            GLint viewport_y = 0;
            GLint viewport_w = 0;
            GLint viewport_h = 0;
        };

        using GlCreateShaderProc = GLuint(APIENTRY*)(GLenum);
        using GlShaderSourceProc = void(APIENTRY*)(GLuint, GLsizei, const char* const*, const GLint*);
        using GlCompileShaderProc = void(APIENTRY*)(GLuint);
        using GlGetShaderivProc = void(APIENTRY*)(GLuint, GLenum, GLint*);
        using GlDeleteShaderProc = void(APIENTRY*)(GLuint);
        using GlCreateProgramProc = GLuint(APIENTRY*)();
        using GlAttachShaderProc = void(APIENTRY*)(GLuint, GLuint);
        using GlLinkProgramProc = void(APIENTRY*)(GLuint);
        using GlGetProgramivProc = void(APIENTRY*)(GLuint, GLenum, GLint*);
        using GlUseProgramProc = void(APIENTRY*)(GLuint);
        using GlDeleteProgramProc = void(APIENTRY*)(GLuint);
        using GlGetUniformLocationProc = GLint(APIENTRY*)(GLuint, const char*);
        using GlUniform1iProc = void(APIENTRY*)(GLint, GLint);
        using GlUniform1fProc = void(APIENTRY*)(GLint, GLfloat);
        using GlUniform2fProc = void(APIENTRY*)(GLint, GLfloat, GLfloat);
        using GlActiveTextureProc = void(APIENTRY*)(GLenum);
        using GlGenFramebuffersProc = void(APIENTRY*)(GLsizei, GLuint*);
        using GlBindFramebufferProc = void(APIENTRY*)(GLenum, GLuint);
        using GlFramebufferTexture2DProc = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
        using GlCheckFramebufferStatusProc = GLenum(APIENTRY*)(GLenum);
        using GlDeleteFramebuffersProc = void(APIENTRY*)(GLsizei, const GLuint*);

        struct TextKey
        {
            std::wstring text;
            std::wstring font;
            int size = 0;
            int weight = 400;
            bool italic = false;
            bool underline = false;
            bool strike = false;
            bool wrap = false;
            int letter_spacing = 0;
            int line_spacing = 0;
            int max_w = 0;
            int max_h = 0;

            bool operator==(const TextKey& rhs) const
            {
                return text == rhs.text
                    && font == rhs.font
                    && size == rhs.size
                    && weight == rhs.weight
                    && italic == rhs.italic
                    && underline == rhs.underline
                    && strike == rhs.strike
                    && wrap == rhs.wrap
                    && letter_spacing == rhs.letter_spacing
                    && line_spacing == rhs.line_spacing
                    && max_w == rhs.max_w
                    && max_h == rhs.max_h;
            }
        };

        struct TextKeyHash
        {
            size_t operator()(const TextKey& key) const
            {
                size_t h = std::hash<std::wstring>{}(key.text);
                h ^= std::hash<std::wstring>{}(key.font) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<int>{}(key.size) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<int>{}(key.weight) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<int>{}(key.max_w) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<int>{}(key.max_h) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= static_cast<size_t>(key.italic) << 1;
                h ^= static_cast<size_t>(key.underline) << 2;
                h ^= static_cast<size_t>(key.strike) << 3;
                h ^= static_cast<size_t>(key.wrap) << 4;
                h ^= std::hash<int>{}(key.letter_spacing) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<int>{}(key.line_spacing) + 0x9e3779b9 + (h << 6) + (h >> 2);
                return h;
            }
        };

        HWND hwnd_ = nullptr;
        HDC dc_ = nullptr;
        HGLRC glrc_ = nullptr;
        IWICImagingFactory* wic_factory_ = nullptr;
        UINT width_ = 1;
        UINT height_ = 1;
        Color resize_clear_color_{ 0.08f, 0.08f, 0.1f, 1.f };
        bool color_key_mode_ = false;
        int transform_stack_depth_ = 0;
        std::vector<float> opacity_stack_{ 1.f };
        std::vector<BlendMode> blend_stack_{ BlendMode::Normal };
        std::vector<ClipEntry> clip_stack_;
        std::vector<Point> path_;
        bool current_path_closed_ = false;
        std::optional<ShadowStyle> pending_shadow_;
        std::unordered_map<std::wstring, Texture> bitmap_cache_;
        std::unordered_map<TextKey, Texture, TextKeyHash> text_cache_;
        std::unordered_map<uint64_t, DynamicBgraTexture> dynamic_bgra_cache_;
        std::unordered_map<unsigned int, GLuint> shader_programs_;
        std::vector<ShaderLayer> shader_layers_;
        std::vector<ShaderInvocation> shader_stack_;
        unsigned int next_shader_id_ = 1;
        GlCreateShaderProc glCreateShader_ = nullptr;
        GlShaderSourceProc glShaderSource_ = nullptr;
        GlCompileShaderProc glCompileShader_ = nullptr;
        GlGetShaderivProc glGetShaderiv_ = nullptr;
        GlDeleteShaderProc glDeleteShader_ = nullptr;
        GlCreateProgramProc glCreateProgram_ = nullptr;
        GlAttachShaderProc glAttachShader_ = nullptr;
        GlLinkProgramProc glLinkProgram_ = nullptr;
        GlGetProgramivProc glGetProgramiv_ = nullptr;
        GlUseProgramProc glUseProgram_ = nullptr;
        GlDeleteProgramProc glDeleteProgram_ = nullptr;
        GlGetUniformLocationProc glGetUniformLocation_ = nullptr;
        GlUniform1iProc glUniform1i_ = nullptr;
        GlUniform1fProc glUniform1f_ = nullptr;
        GlUniform2fProc glUniform2f_ = nullptr;
        GlActiveTextureProc glActiveTexture_ = nullptr;
        GlGenFramebuffersProc glGenFramebuffers_ = nullptr;
        GlBindFramebufferProc glBindFramebuffer_ = nullptr;
        GlFramebufferTexture2DProc glFramebufferTexture2D_ = nullptr;
        GlCheckFramebufferStatusProc glCheckFramebufferStatus_ = nullptr;
        GlDeleteFramebuffersProc glDeleteFramebuffers_ = nullptr;
        bool com_initialized_ = false;

        void make_current()
        {
            if (dc_ && glrc_)
                wglMakeCurrent(dc_, glrc_);
        }

        void output_driver_info() const
        {
            const GLubyte* vendor = glGetString(GL_VENDOR);
            const GLubyte* renderer = glGetString(GL_RENDERER);
            const GLubyte* version = glGetString(GL_VERSION);
            std::wstring text = L"Otter OpenGL: vendor=";
            text += widen_gl_string(vendor);
            text += L", renderer=";
            text += widen_gl_string(renderer);
            text += L", version=";
            text += widen_gl_string(version);
            text += L"\n";
            OutputDebugStringW(text.c_str());
        }

        template <typename Proc>
        Proc load_gl_proc(const char* name) const
        {
            return reinterpret_cast<Proc>(wglGetProcAddress(name));
        }

        void load_shader_entry_points()
        {
            glCreateShader_ = load_gl_proc<GlCreateShaderProc>("glCreateShader");
            glShaderSource_ = load_gl_proc<GlShaderSourceProc>("glShaderSource");
            glCompileShader_ = load_gl_proc<GlCompileShaderProc>("glCompileShader");
            glGetShaderiv_ = load_gl_proc<GlGetShaderivProc>("glGetShaderiv");
            glDeleteShader_ = load_gl_proc<GlDeleteShaderProc>("glDeleteShader");
            glCreateProgram_ = load_gl_proc<GlCreateProgramProc>("glCreateProgram");
            glAttachShader_ = load_gl_proc<GlAttachShaderProc>("glAttachShader");
            glLinkProgram_ = load_gl_proc<GlLinkProgramProc>("glLinkProgram");
            glGetProgramiv_ = load_gl_proc<GlGetProgramivProc>("glGetProgramiv");
            glUseProgram_ = load_gl_proc<GlUseProgramProc>("glUseProgram");
            glDeleteProgram_ = load_gl_proc<GlDeleteProgramProc>("glDeleteProgram");
            glGetUniformLocation_ = load_gl_proc<GlGetUniformLocationProc>("glGetUniformLocation");
            glUniform1i_ = load_gl_proc<GlUniform1iProc>("glUniform1i");
            glUniform1f_ = load_gl_proc<GlUniform1fProc>("glUniform1f");
            glUniform2f_ = load_gl_proc<GlUniform2fProc>("glUniform2f");
            glActiveTexture_ = load_gl_proc<GlActiveTextureProc>("glActiveTexture");
            glGenFramebuffers_ = load_gl_proc<GlGenFramebuffersProc>("glGenFramebuffers");
            glBindFramebuffer_ = load_gl_proc<GlBindFramebufferProc>("glBindFramebuffer");
            glFramebufferTexture2D_ = load_gl_proc<GlFramebufferTexture2DProc>("glFramebufferTexture2D");
            glCheckFramebufferStatus_ = load_gl_proc<GlCheckFramebufferStatusProc>("glCheckFramebufferStatus");
            glDeleteFramebuffers_ = load_gl_proc<GlDeleteFramebuffersProc>("glDeleteFramebuffers");
        }

        bool shader_api_available() const
        {
            return glCreateShader_ && glShaderSource_ && glCompileShader_ && glGetShaderiv_
                && glDeleteShader_ && glCreateProgram_ && glAttachShader_ && glLinkProgram_
                && glGetProgramiv_ && glUseProgram_ && glDeleteProgram_
                && glGetUniformLocation_ && glUniform1i_ && glUniform1f_ && glUniform2f_
                && glActiveTexture_ && glGenFramebuffers_ && glBindFramebuffer_
                && glFramebufferTexture2D_ && glCheckFramebufferStatus_ && glDeleteFramebuffers_;
        }

        GLuint compile_shader(GLenum type, std::string_view source)
        {
            if (source.empty())
                return 0;
            GLuint shader = glCreateShader_(type);
            const char* data = source.data();
            const GLint length = static_cast<GLint>(source.size());
            glShaderSource_(shader, 1, &data, &length);
            glCompileShader_(shader);
            GLint compiled = 0;
            glGetShaderiv_(shader, GL_COMPILE_STATUS, &compiled);
            if (!compiled)
            {
                OTTER_LOG_ERROR("shader", "OpenGL shader compilation failed");
                glDeleteShader_(shader);
                return 0;
            }
            return shader;
        }

        ShaderLayer* acquire_shader_layer(int width, int height)
        {
            width = (std::max)(1, width);
            height = (std::max)(1, height);
            ShaderLayer* free_layer = nullptr;
            for (ShaderLayer& layer : shader_layers_)
            {
                const bool in_use = std::any_of(
                    shader_stack_.begin(),
                    shader_stack_.end(),
                    [&layer](const ShaderInvocation& invocation) { return invocation.layer == &layer; });
                if (in_use)
                    continue;
                if (layer.width == width && layer.height == height && layer.valid())
                    return &layer;
                if (!free_layer)
                    free_layer = &layer;
            }
            if (!free_layer)
            {
                shader_layers_.push_back({});
                free_layer = &shader_layers_.back();
            }
            const GLint previous_texture = current_bound_texture_2d();
            resize_shader_layer(*free_layer, width, height);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture));
            return free_layer->valid() ? free_layer : nullptr;
        }

        void resize_shader_layer(ShaderLayer& layer, int width, int height)
        {
            delete_shader_layer(layer);
            glGenTextures(1, &layer.texture);
            glBindTexture(GL_TEXTURE_2D, layer.texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

            glGenFramebuffers_(1, &layer.framebuffer);
            GLint previous_framebuffer = 0;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_framebuffer);
            glBindFramebuffer_(GL_FRAMEBUFFER, layer.framebuffer);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, layer.texture, 0);
            const GLenum status = glCheckFramebufferStatus_(GL_FRAMEBUFFER);
            glBindFramebuffer_(GL_FRAMEBUFFER, static_cast<GLuint>(previous_framebuffer));
            if (status != GL_FRAMEBUFFER_COMPLETE)
            {
                delete_shader_layer(layer);
                return;
            }
            layer.width = width;
            layer.height = height;
        }

        void delete_shader_layer(ShaderLayer& layer)
        {
            if (layer.framebuffer && glDeleteFramebuffers_)
                glDeleteFramebuffers_(1, &layer.framebuffer);
            if (layer.texture)
                glDeleteTextures(1, &layer.texture);
            layer = {};
        }

        void release_shader_layers()
        {
            shader_stack_.clear();
            for (ShaderLayer& layer : shader_layers_)
                delete_shader_layer(layer);
            shader_layers_.clear();
        }

        static GLint current_bound_texture_2d()
        {
            GLint texture = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
            return texture;
        }

        void set_shader_uniforms(GLuint program, int width, int height)
        {
            const GLint tex = glGetUniformLocation_(program, "u_texture");
            if (tex >= 0)
                glUniform1i_(tex, 0);
            const GLint tex0 = glGetUniformLocation_(program, "texture0");
            if (tex0 >= 0)
                glUniform1i_(tex0, 0);
            const GLint resolution = glGetUniformLocation_(program, "u_resolution");
            if (resolution >= 0)
                glUniform2f_(resolution, static_cast<GLfloat>(width), static_cast<GLfloat>(height));
            const GLint opacity = glGetUniformLocation_(program, "u_opacity");
            if (opacity >= 0)
                glUniform1f_(opacity, current_opacity());
            const GLint time = glGetUniformLocation_(program, "u_time");
            if (time >= 0)
            {
                using namespace std::chrono;
                const auto now = steady_clock::now().time_since_epoch();
                glUniform1f_(time, duration_cast<duration<float>>(now).count());
            }
        }

        static std::wstring widen_gl_string(const GLubyte* value)
        {
            if (!value)
                return L"(null)";
            std::wstring result;
            for (const unsigned char* p = value; *p; ++p)
                result.push_back(static_cast<wchar_t>(*p));
            return result;
        }

        Color convert_color(const Color& color) const
        {
            if (color_key_mode_ && color.is_pure_black())
                return color.to_near_black();
            return color;
        }

        float current_opacity() const
        {
            return opacity_stack_.empty() ? 1.f : opacity_stack_.back();
        }

        Color with_opacity(Color color, float opacity_multiplier = 1.f) const
        {
            color = convert_color(color);
            color.a *= current_opacity() * opacity_multiplier;
            return color;
        }

        void set_gl_color(Color color, float opacity_multiplier = 1.f)
        {
            color = with_opacity(color, opacity_multiplier);
            glColor4f(color.r, color.g, color.b, color.a);
        }

        void apply_blend(BlendMode blend)
        {
            glEnable(GL_BLEND);
            switch (blend)
            {
            case BlendMode::Add:
                glBlendFunc(GL_SRC_ALPHA, GL_ONE);
                break;
            case BlendMode::Multiply:
                glBlendFunc(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA);
                break;
            case BlendMode::Screen:
                glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR);
                break;
            case BlendMode::Difference:
                glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_COLOR);
                break;
            case BlendMode::Erase:
                glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
                break;
            case BlendMode::Overlay:
            case BlendMode::SoftLight:
            case BlendMode::HardLight:
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                break;
            default:
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                break;
            }
        }

        static int next_power_of_two(int value)
        {
            int result = 1;
            while (result < value)
                result <<= 1;
            return result;
        }

        static Color lerp_color(Color a, Color b, float t)
        {
            t = std::clamp(t, 0.f, 1.f);
            return Color{
                a.r + (b.r - a.r) * t,
                a.g + (b.g - a.g) * t,
                a.b + (b.b - a.b) * t,
                a.a + (b.a - a.a) * t
            };
        }

        static Color sample_stops(const std::vector<GradientStop>& stops, float t)
        {
            if (stops.empty())
                return Color::transparent();
            if (stops.size() == 1 || t <= stops.front().position)
                return stops.front().color;
            for (size_t i = 1; i < stops.size(); ++i)
            {
                if (t <= stops[i].position)
                {
                    const float span = (std::max)(0.0001f, stops[i].position - stops[i - 1].position);
                    return lerp_color(stops[i - 1].color, stops[i].color, (t - stops[i - 1].position) / span);
                }
            }
            return stops.back().color;
        }

        Color color_for_point(const FillStyle& style, Point p, Point center) const
        {
            switch (style.type)
            {
            case FillStyle::Type::Solid:
                return style.color;
            case FillStyle::Type::Linear:
                if (!style.linear)
                    return Color::transparent();
                {
                    const LinearGradient& g = *style.linear;
                    const float dx = g.x2 - g.x1;
                    const float dy = g.y2 - g.y1;
                    const float len2 = dx * dx + dy * dy;
                    const float t = len2 <= 0.0001f ? 0.f : ((p.x - g.x1) * dx + (p.y - g.y1) * dy) / len2;
                    return sample_stops(g.stops, t);
                }
            case FillStyle::Type::Radial:
                if (!style.radial)
                    return Color::transparent();
                {
                    const RadialGradient& g = *style.radial;
                    const float rx = (std::max)(1.f, g.rx);
                    const float ry = (std::max)(1.f, g.ry);
                    const float nx = (p.x - g.cx) / rx;
                    const float ny = (p.y - g.cy) / ry;
                    return sample_stops(g.stops, std::sqrt(nx * nx + ny * ny));
                }
            case FillStyle::Type::Conic:
                if (!style.conic)
                    return Color::transparent();
                {
                    const ConicGradient& g = *style.conic;
                    float a = std::atan2(p.y - g.cy, p.x - g.cx) - g.start_angle;
                    while (a < 0.f) a += 2.f * OTTER_PI;
                    while (a > 2.f * OTTER_PI) a -= 2.f * OTTER_PI;
                    return sample_stops(g.stops, a / (2.f * OTTER_PI));
                }
            default:
                return Color::transparent();
            }
        }

        void push_path_point(Point point)
        {
            if (path_.size() >= 4096 || !std::isfinite(point.x) || !std::isfinite(point.y))
                return;
            if (!path_.empty() && nearly_same(path_.back(), point))
                return;
            path_.push_back(point);
        }

        void draw_polygon(const std::vector<Point>& points, const FillStyle& style, std::optional<Point> center_override = std::nullopt, float opacity_multiplier = 1.f)
        {
            std::vector<Point> clean_points = normalize_polygon(points);
            if (clean_points.size() < 2)
                return;
            glDisable(GL_TEXTURE_2D);
            Point center = center_override.value_or(polygon_center(clean_points));
            if (pending_shadow_)
            {
                ShadowStyle shadow = *pending_shadow_;
                pending_shadow_.reset();
                FillStyle shadow_fill{ shadow.color };
                draw_polygon(offset_points(clean_points, shadow.offset_x, shadow.offset_y), shadow_fill, std::nullopt, shadow.color.a);
            }

            glBegin(GL_TRIANGLE_FAN);
            set_gl_color(color_for_point(style, center, center), opacity_multiplier);
            glVertex2f(center.x, center.y);
            for (const Point& p : clean_points)
            {
                set_gl_color(color_for_point(style, p, center), opacity_multiplier);
                glVertex2f(p.x, p.y);
            }
            set_gl_color(color_for_point(style, clean_points.front(), center), opacity_multiplier);
            glVertex2f(clean_points.front().x, clean_points.front().y);
            glEnd();
        }

        void draw_polygon_solid_raw(const std::vector<Point>& points)
        {
            std::vector<Point> clean_points = normalize_polygon(points);
            if (clean_points.size() < 3)
                return;
            glBegin(GL_TRIANGLE_FAN);
            for (const Point& p : clean_points)
                glVertex2f(p.x, p.y);
            glEnd();
        }

        static bool nearly_same(Point a, Point b)
        {
            constexpr float epsilon = 0.01f;
            return std::fabs(a.x - b.x) <= epsilon && std::fabs(a.y - b.y) <= epsilon;
        }

        static std::vector<Point> normalize_polygon(const std::vector<Point>& points)
        {
            std::vector<Point> result;
            result.reserve((std::min)(points.size(), static_cast<size_t>(4096)));
            for (const Point& point : points)
            {
                if (!std::isfinite(point.x) || !std::isfinite(point.y))
                    continue;
                if (!result.empty() && nearly_same(result.back(), point))
                    continue;
                result.push_back(point);
                if (result.size() >= 4096)
                    break;
            }
            while (result.size() > 1 && nearly_same(result.front(), result.back()))
                result.pop_back();
            return result;
        }

        static Point polygon_center(const std::vector<Point>& points)
        {
            Point center{};
            for (const Point& p : points)
            {
                center.x += p.x;
                center.y += p.y;
            }
            const float n = (std::max)(1.f, static_cast<float>(points.size()));
            center.x /= n;
            center.y /= n;
            return center;
        }

        static std::vector<Point> offset_points(const std::vector<Point>& points, float dx, float dy)
        {
            std::vector<Point> result;
            result.reserve(points.size());
            for (Point p : points)
                result.push_back({ p.x + dx, p.y + dy });
            return result;
        }

        static std::vector<Point> rounded_rect_points(float x, float y, float w, float h, float radius)
        {
            std::vector<Point> points;
            if (w <= 0.f || h <= 0.f)
                return points;

            radius = std::clamp(radius, 0.f, (std::min)(w, h) * 0.5f);
            if (radius <= 0.01f)
            {
                points.push_back({ x, y });
                points.push_back({ x + w, y });
                points.push_back({ x + w, y + h });
                points.push_back({ x, y + h });
                return points;
            }

            constexpr int segments = 24;
            auto add_arc = [&](float cx, float cy, float start, float end) {
                for (int i = 0; i <= segments; ++i)
                {
                    const float t = static_cast<float>(i) / static_cast<float>(segments);
                    const float a = start + (end - start) * t;
                    points.push_back({ cx + std::cos(a) * radius, cy + std::sin(a) * radius });
                }
            };
            add_arc(x + w - radius, y + radius, -OTTER_PI * 0.5f, 0.f);
            add_arc(x + w - radius, y + h - radius, 0.f, OTTER_PI * 0.5f);
            add_arc(x + radius, y + h - radius, OTTER_PI * 0.5f, OTTER_PI);
            add_arc(x + radius, y + radius, OTTER_PI, OTTER_PI * 1.5f);
            return points;
        }

        Point transform_point(Point point) const
        {
            GLfloat m[16]{};
            glGetFloatv(GL_MODELVIEW_MATRIX, m);
            return {
                m[0] * point.x + m[4] * point.y + m[12],
                m[1] * point.x + m[5] * point.y + m[13]
            };
        }

        void rebuild_stencil_clip()
        {
            if (clip_stack_.empty())
            {
                glDisable(GL_STENCIL_TEST);
                return;
            }

            glEnable(GL_STENCIL_TEST);
            glClear(GL_STENCIL_BUFFER_BIT);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glDisable(GL_TEXTURE_2D);

            glMatrixMode(GL_MODELVIEW);
            glPushMatrix();
            glLoadIdentity();

            for (size_t i = 0; i < clip_stack_.size(); ++i)
            {
                glStencilFunc(i == 0 ? GL_ALWAYS : GL_EQUAL, static_cast<GLint>(i), 0xFF);
                glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
                draw_polygon_solid_raw(clip_stack_[i].points);
            }

            glPopMatrix();
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glStencilFunc(GL_EQUAL, static_cast<GLint>(clip_stack_.size()), 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        }

        HFONT create_font(const TextStyle& style) const
        {
            LOGFONTW lf{};
            lf.lfHeight = -static_cast<LONG>((std::max)(1.f, style.font_size));
            lf.lfWeight = static_cast<LONG>(style.weight);
            lf.lfItalic = style.font_style == TextStyle::FontStyle::Normal ? FALSE : TRUE;
            lf.lfUnderline = style.underline ? TRUE : FALSE;
            lf.lfStrikeOut = style.strikethrough ? TRUE : FALSE;
            lf.lfCharSet = DEFAULT_CHARSET;
            lf.lfQuality = CLEARTYPE_QUALITY;
            wcsncpy_s(lf.lfFaceName, style.font_family.c_str(), _TRUNCATE);
            return CreateFontIndirectW(&lf);
        }

        TextKey make_text_key(const std::wstring& content, float max_width, float max_height, const TextStyle& style) const
        {
            TextKey key;
            key.text = content;
            key.font = style.font_family;
            key.size = static_cast<int>(std::round(style.font_size));
            key.weight = static_cast<int>(style.weight);
            key.italic = style.font_style != TextStyle::FontStyle::Normal;
            key.underline = style.underline;
            key.strike = style.strikethrough;
            key.wrap = style.word_wrap;
            key.letter_spacing = static_cast<int>(std::round(style.letter_spacing * 100.f));
            key.line_spacing = static_cast<int>(std::round(style.line_spacing * 100.f));
            key.max_w = static_cast<int>(std::round(max_width));
            key.max_h = static_cast<int>(std::round(max_height));
            return key;
        }

        void draw_text_texture(const std::wstring& content, float x, float y, const TextMetrics& metrics, float max_width, float max_height, const TextStyle& style)
        {
            Texture texture = load_text_texture(content, metrics, max_width, max_height, style);
            if (!texture.valid())
                return;
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, texture.texture);
            set_gl_color(style.color);
            draw_textured_quad(x, y, static_cast<float>(texture.width), static_cast<float>(texture.height), texture.u, texture.v);
            glDisable(GL_TEXTURE_2D);
        }

        Texture load_text_texture(const std::wstring& content, const TextMetrics& metrics, float max_width, float max_height, const TextStyle& style)
        {
            const TextKey key = make_text_key(content, max_width, max_height, style);
            auto it = text_cache_.find(key);
            if (it != text_cache_.end())
                return it->second;

            constexpr int text_scale = 2;
            const int text_w = (std::max)(1, static_cast<int>(std::ceil(metrics.width + 4.f)));
            const int text_h = (std::max)(1, static_cast<int>(std::ceil(metrics.height + 4.f)));
            const int raster_w = text_w * text_scale;
            const int raster_h = text_h * text_scale;
            const int tex_w = next_power_of_two(raster_w);
            const int tex_h = next_power_of_two(raster_h);

            HDC mem_dc = CreateCompatibleDC(dc_);
            if (!mem_dc)
                return {};
            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = tex_w;
            bmi.bmiHeader.biHeight = -tex_h;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            void* pixels = nullptr;
            HBITMAP bitmap = CreateDIBSection(mem_dc, &bmi, DIB_RGB_COLORS, &pixels, nullptr, 0);
            if (!bitmap || !pixels)
            {
                if (bitmap) DeleteObject(bitmap);
                DeleteDC(mem_dc);
                return {};
            }

            HGDIOBJ old_bitmap = SelectObject(mem_dc, bitmap);
            TextStyle raster_style = style;
            raster_style.font_size *= static_cast<float>(text_scale);
            raster_style.letter_spacing *= static_cast<float>(text_scale);
            raster_style.line_spacing *= static_cast<float>(text_scale);
            HFONT font = create_font(raster_style);
            HFONT old_font = static_cast<HFONT>(SelectObject(mem_dc, font));
            RECT clear{ 0, 0, tex_w, tex_h };
            HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(mem_dc, &clear, black);
            DeleteObject(black);
            SetBkMode(mem_dc, TRANSPARENT);
            SetTextColor(mem_dc, RGB(255, 255, 255));
            draw_text_to_dc(mem_dc, content, raster_w, raster_h, raster_style);

            std::vector<uint8_t> rgba(static_cast<size_t>(tex_w) * static_cast<size_t>(tex_h) * 4u);
            const uint8_t* src = static_cast<const uint8_t*>(pixels);
            for (int i = 0; i < tex_w * tex_h; ++i)
            {
                const uint8_t blue = src[i * 4u + 0u];
                const uint8_t green = src[i * 4u + 1u];
                const uint8_t red = src[i * 4u + 2u];
                const uint8_t alpha = (std::max)(red, (std::max)(green, blue));
                rgba[i * 4u + 0u] = 255;
                rgba[i * 4u + 1u] = 255;
                rgba[i * 4u + 2u] = 255;
                rgba[i * 4u + 3u] = alpha;
            }

            SelectObject(mem_dc, old_font);
            SelectObject(mem_dc, old_bitmap);
            DeleteObject(font);
            DeleteObject(bitmap);
            DeleteDC(mem_dc);

            Texture texture = upload_texture(rgba.data(), tex_w, tex_h, tex_w, tex_h);
            texture.width = text_w;
            texture.height = text_h;
            texture.u = static_cast<float>(raster_w) / static_cast<float>(tex_w);
            texture.v = static_cast<float>(raster_h) / static_cast<float>(tex_h);
            text_cache_[key] = texture;
            return texture;
        }

        static void draw_text_to_dc(HDC dc, const std::wstring& content, int width, int height, const TextStyle& style)
        {
            if (content.empty())
                return;

            if (style.letter_spacing == 0.f && style.line_spacing == 0.f)
            {
                RECT text_rect{ 0, 0, width, height };
                DrawTextW(dc, content.c_str(), static_cast<int>(content.size()), &text_rect,
                    DT_LEFT | DT_TOP | DT_NOPREFIX | (style.word_wrap ? DT_WORDBREAK : DT_SINGLELINE));
                return;
            }

            TEXTMETRICW tm{};
            GetTextMetricsW(dc, &tm);
            const int line_height = static_cast<int>(std::round(style.line_spacing > 0.f ? style.line_spacing : static_cast<float>(tm.tmHeight)));
            int y = 0;
            size_t line_start = 0;
            while (line_start <= content.size() && y < height)
            {
                size_t line_end = content.find(L'\n', line_start);
                if (line_end == std::wstring::npos)
                    line_end = content.size();
                int x = 0;
                for (size_t i = line_start; i < line_end && x < width; ++i)
                {
                    const wchar_t ch = content[i];
                    TextOutW(dc, x, y, &ch, 1);
                    SIZE size{};
                    GetTextExtentPoint32W(dc, &ch, 1, &size);
                    x += size.cx + static_cast<int>(std::round(style.letter_spacing));
                }
                if (line_end == content.size())
                    break;
                line_start = line_end + 1;
                y += line_height;
            }
        }

        Texture upload_texture(const uint8_t* rgba, int width, int height, int tex_width = 0, int tex_height = 0)
        {
            if (!rgba || width <= 0 || height <= 0)
                return {};
            tex_width = tex_width > 0 ? tex_width : next_power_of_two(width);
            tex_height = tex_height > 0 ? tex_height : next_power_of_two(height);

            std::vector<uint8_t> storage;
            const uint8_t* upload_data = rgba;
            if (tex_width != width || tex_height != height)
            {
                storage.assign(static_cast<size_t>(tex_width) * static_cast<size_t>(tex_height) * 4u, 0u);
                for (int row = 0; row < height; ++row)
                {
                    memcpy(
                        storage.data() + static_cast<size_t>(row) * static_cast<size_t>(tex_width) * 4u,
                        rgba + static_cast<size_t>(row) * static_cast<size_t>(width) * 4u,
                        static_cast<size_t>(width) * 4u);
                }
                upload_data = storage.data();
            }

            GLuint id = 0;
            glGenTextures(1, &id);
            glBindTexture(GL_TEXTURE_2D, id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex_width, tex_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, upload_data);

            Texture texture;
            texture.texture = id;
            texture.width = width;
            texture.height = height;
            texture.tex_width = tex_width;
            texture.tex_height = tex_height;
            texture.u = static_cast<float>(width) / static_cast<float>(tex_width);
            texture.v = static_cast<float>(height) / static_cast<float>(tex_height);
            return texture;
        }

        Texture upload_texture_bgra(const uint8_t* bgra, int width, int height, int stride)
        {
            if (!bgra || width <= 0 || height <= 0 || stride < width * 4)
                return {};
            std::vector<uint8_t> packed;
            const uint8_t* upload_data = bgra;
            if (stride != width * 4)
            {
                packed.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
                for (int row = 0; row < height; ++row)
                    std::memcpy(packed.data() + static_cast<size_t>(row) * static_cast<size_t>(width) * 4u,
                                bgra + static_cast<size_t>(row) * static_cast<size_t>(stride),
                                static_cast<size_t>(width) * 4u);
                upload_data = packed.data();
            }

            GLuint id = 0;
            glGenTextures(1, &id);
            glBindTexture(GL_TEXTURE_2D, id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, upload_data);

            Texture texture;
            texture.texture = id;
            texture.width = width;
            texture.height = height;
            texture.tex_width = width;
            texture.tex_height = height;
            texture.u = 1.f;
            texture.v = 1.f;
            return texture;
        }

        Texture update_dynamic_bgra_texture(uint64_t cache_key, uint64_t revision,
                                            const uint8_t* bgra, int width, int height, int stride,
                                            const std::vector<BitmapUpdateRect>& update_rects)
        {
            if (cache_key == 0 || !bgra || width <= 0 || height <= 0 || stride < width * 4)
                return {};

            DynamicBgraTexture& entry = dynamic_bgra_cache_[cache_key];
            const bool recreate = !entry.texture.valid() ||
                                  entry.texture.width != width ||
                                  entry.texture.height != height;
            if (recreate)
            {
                if (entry.texture.texture)
                    glDeleteTextures(1, &entry.texture.texture);

                GLuint id = 0;
                glGenTextures(1, &id);
                glBindTexture(GL_TEXTURE_2D, id);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, nullptr);

                entry.texture.texture = id;
                entry.texture.width = width;
                entry.texture.height = height;
                entry.texture.tex_width = width;
                entry.texture.tex_height = height;
                entry.texture.u = 1.f;
                entry.texture.v = 1.f;
                entry.revision = 0;
            }

            if (entry.revision != revision)
            {
                glBindTexture(GL_TEXTURE_2D, entry.texture.texture);
                if (recreate || update_rects.empty())
                {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, bgra);
                }
                else
                {
                    for (const auto& rect : update_rects)
                    {
                        if (rect.width <= 0 || rect.height <= 0) continue;
                        const uint8_t* rect_data = bgra + static_cast<size_t>(rect.y) * static_cast<size_t>(stride) +
                                                   static_cast<size_t>(rect.x) * 4u;
                        glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 4);
                        glTexSubImage2D(GL_TEXTURE_2D, 0, rect.x, rect.y, rect.width, rect.height,
                                        GL_BGRA_EXT, GL_UNSIGNED_BYTE, rect_data);
                        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                    }
                }
                entry.revision = revision;
            }

            return entry.texture;
        }

        Texture load_bitmap_texture(const std::wstring& path)
        {
            auto found = bitmap_cache_.find(path);
            if (found != bitmap_cache_.end())
                return found->second;

            IWICImagingFactory* wic = get_wic_factory();
            if (!wic)
                return {};

            Texture texture;
            if (starts_with_http(path))
            {
                std::vector<uint8_t> bytes = download_url(path);
                if (bytes.empty())
                    return {};
                texture = load_bitmap_texture_from_memory(bytes);
            }
            else
            {
                texture = load_bitmap_texture_from_file(path);
            }

            if (texture.valid())
                bitmap_cache_[path] = texture;
            return texture;
        }

        Texture load_bitmap_texture_from_file(const std::wstring& path)
        {
            IWICImagingFactory* wic = get_wic_factory();
            if (!wic)
                return {};

            IWICBitmapDecoder* decoder = nullptr;
            HRESULT hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
            if (FAILED(hr) || !decoder)
                return {};
            Texture texture = load_bitmap_texture_from_decoder(decoder);
            decoder->Release();
            return texture;
        }

        Texture load_bitmap_texture_from_memory(const std::vector<uint8_t>& bytes)
        {
            IWICImagingFactory* wic = get_wic_factory();
            if (!wic || bytes.empty())
                return {};

            IWICStream* stream = nullptr;
            HRESULT hr = wic->CreateStream(&stream);
            if (FAILED(hr) || !stream)
                return {};

            hr = stream->InitializeFromMemory(
                const_cast<BYTE*>(bytes.data()),
                static_cast<DWORD>(bytes.size()));
            if (FAILED(hr))
            {
                stream->Release();
                return {};
            }

            IWICBitmapDecoder* decoder = nullptr;
            hr = wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
            stream->Release();
            if (FAILED(hr) || !decoder)
                return {};

            Texture texture = load_bitmap_texture_from_decoder(decoder);
            decoder->Release();
            return texture;
        }

        Texture load_bitmap_texture_from_decoder(IWICBitmapDecoder* decoder)
        {
            if (!decoder)
                return {};

            IWICImagingFactory* wic = get_wic_factory();
            if (!wic)
                return {};

            IWICBitmapFrameDecode* frame = nullptr;
            HRESULT hr = decoder->GetFrame(0, &frame);
            if (FAILED(hr) || !frame)
            {
                return {};
            }

            IWICFormatConverter* converter = nullptr;
            hr = wic->CreateFormatConverter(&converter);
            if (SUCCEEDED(hr) && converter)
            {
                hr = converter->Initialize(
                    frame,
                    GUID_WICPixelFormat32bppRGBA,
                    WICBitmapDitherTypeNone,
                    nullptr,
                    0.f,
                    WICBitmapPaletteTypeCustom);
            }

            UINT width = 0;
            UINT height = 0;
            if (SUCCEEDED(hr))
                hr = converter->GetSize(&width, &height);

            std::vector<uint8_t> pixels;
            if (SUCCEEDED(hr) && width > 0 && height > 0)
            {
                const uint64_t byte_count =
                    static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4ull;
                if (byte_count > static_cast<uint64_t>((std::numeric_limits<UINT>::max)()))
                {
                    hr = E_OUTOFMEMORY;
                }
                else
                {
                    pixels.resize(static_cast<size_t>(byte_count));
                    hr = converter->CopyPixels(
                        nullptr,
                        width * 4u,
                        static_cast<UINT>(pixels.size()),
                        pixels.data());
                }
            }

            if (converter) converter->Release();
            frame->Release();

            if (FAILED(hr) || pixels.empty())
                return {};

            return upload_texture(pixels.data(), static_cast<int>(width), static_cast<int>(height));
        }

        static bool starts_with_http(const std::wstring& path)
        {
            return path.rfind(L"http://", 0) == 0 || path.rfind(L"https://", 0) == 0;
        }

        static std::vector<uint8_t> download_url(const std::wstring& url)
        {
            const bool secure = url.rfind(L"https://", 0) == 0;
            const size_t scheme_len = secure ? 8u : 7u;
            const size_t slash = url.find(L'/', scheme_len);
            const std::wstring host_port = slash == std::wstring::npos
                ? url.substr(scheme_len)
                : url.substr(scheme_len, slash - scheme_len);
            const std::wstring path = slash == std::wstring::npos
                ? L"/"
                : url.substr(slash);
            if (host_port.empty())
                return {};

            std::wstring host = host_port;
            INTERNET_PORT port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
            const size_t colon = host_port.rfind(L':');
            if (colon != std::wstring::npos)
            {
                host = host_port.substr(0, colon);
                port = static_cast<INTERNET_PORT>(std::wcstoul(host_port.substr(colon + 1).c_str(), nullptr, 10));
            }

            HINTERNET session = WinHttpOpen(
                L"OtterOpenGLRenderer/1.0",
                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS,
                0);
            if (!session)
                return {};

            HINTERNET connection = WinHttpConnect(session, host.c_str(), port, 0);
            if (!connection)
            {
                WinHttpCloseHandle(session);
                return {};
            }

            HINTERNET request = WinHttpOpenRequest(
                connection,
                L"GET",
                path.c_str(),
                nullptr,
                WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES,
                secure ? WINHTTP_FLAG_SECURE : 0);
            if (!request)
            {
                WinHttpCloseHandle(connection);
                WinHttpCloseHandle(session);
                return {};
            }

            std::vector<uint8_t> data;
            BOOL ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
            if (ok)
                ok = WinHttpReceiveResponse(request, nullptr);
            if (ok)
            {
                DWORD status = 0;
                DWORD status_size = sizeof(status);
                WinHttpQueryHeaders(
                    request,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX,
                    &status,
                    &status_size,
                    WINHTTP_NO_HEADER_INDEX);
                if (status == 200)
                {
                    DWORD available = 0;
                    while (WinHttpQueryDataAvailable(request, &available) && available > 0)
                    {
                        const size_t old_size = data.size();
                        if (available > 16u * 1024u * 1024u
                            || old_size > 64u * 1024u * 1024u
                            || old_size + available > 64u * 1024u * 1024u)
                        {
                            data.clear();
                            break;
                        }
                        data.resize(old_size + available);
                        DWORD read = 0;
                        if (!WinHttpReadData(request, data.data() + old_size, available, &read))
                            break;
                        data.resize(old_size + read);
                    }
                }
            }

            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return data;
        }

        IWICImagingFactory* get_wic_factory()
        {
            if (wic_factory_)
                return wic_factory_;
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_factory_));
            return wic_factory_;
        }

        static void draw_textured_quad(float x, float y, float w, float h, float u, float v)
        {
            glBegin(GL_QUADS);
            glTexCoord2f(0.f, 0.f); glVertex2f(x, y);
            glTexCoord2f(u, 0.f); glVertex2f(x + w, y);
            glTexCoord2f(u, v); glVertex2f(x + w, y + h);
            glTexCoord2f(0.f, v); glVertex2f(x, y + h);
            glEnd();
        }

        static void draw_textured_quad_flipped_y(float x, float y, float w, float h, float u, float v)
        {
            glBegin(GL_QUADS);
            glTexCoord2f(0.f, v); glVertex2f(x, y);
            glTexCoord2f(u, v); glVertex2f(x + w, y);
            glTexCoord2f(u, 0.f); glVertex2f(x + w, y + h);
            glTexCoord2f(0.f, 0.f); glVertex2f(x, y + h);
            glEnd();
        }
    };
}
