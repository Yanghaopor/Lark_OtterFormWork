#pragma once

#include "OtterLayer.h"

#if defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <algorithm>
#include <cmath>
#include <vector>

namespace Otter
{
    class PortableOpenGLRenderContext : public RenderContext
    {
    public:
        void initialize_native(void*, unsigned int width, unsigned int height) override
        {
            resize(width, height);
        }

        void resize(unsigned int width, unsigned int height) override
        {
            width_ = std::max(1u, width);
            height_ = std::max(1u, height);
            glViewport(0, 0, static_cast<GLsizei>(width_), static_cast<GLsizei>(height_));
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glOrtho(0.0, static_cast<double>(width_), static_cast<double>(height_), 0.0, -1.0, 1.0);
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();
            glDisable(GL_DEPTH_TEST);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }

        void set_resize_clear_color(const Color& color) override { resize_clear_color_ = color; }
        void set_color_key_mode(bool enable) override { color_key_mode_ = enable; }

        void begin_frame(Color clear_color = Color{ 0.08f, 0.08f, 0.1f, 1.f }) override
        {
            glClearColor(clear_color.r, clear_color.g, clear_color.b, color_key_mode_ ? 0.f : clear_color.a);
            glClear(GL_COLOR_BUFFER_BIT);
            path_.clear();
            path_closed_ = false;
        }

        bool end_frame() override
        {
            glFlush();
            return true;
        }

        void push_transform(float tx, float ty, float sx = 1.f, float sy = 1.f, float rot = 0.f) override
        {
            glPushMatrix();
            glTranslatef(tx, ty, 0.f);
            if (rot != 0.f)
                glRotatef(rot * 57.2957795f, 0.f, 0.f, 1.f);
            glScalef(sx, sy, 1.f);
        }

        void pop_transform() override { glPopMatrix(); }
        void push_style(const LayerStyle& style) override { opacity_stack_.push_back(style.opacity); }
        void pop_style() override { if (!opacity_stack_.empty()) opacity_stack_.pop_back(); }

        void cmd_move_to(float x, float y) override
        {
            path_.clear();
            path_.push_back({ x, y });
            path_closed_ = false;
        }

        void cmd_line_to(float x, float y) override { path_.push_back({ x, y }); }

        void cmd_bezier_to(float cx1, float cy1, float cx2, float cy2, float ex, float ey) override
        {
            if (path_.empty())
            {
                path_.push_back({ ex, ey });
                return;
            }
            const Point start = path_.back();
            for (int i = 1; i <= 18; ++i)
            {
                const float t = static_cast<float>(i) / 18.f;
                const float u = 1.f - t;
                const float x = u*u*u*start.x + 3.f*u*u*t*cx1 + 3.f*u*t*t*cx2 + t*t*t*ex;
                const float y = u*u*u*start.y + 3.f*u*u*t*cy1 + 3.f*u*t*t*cy2 + t*t*t*ey;
                path_.push_back({ x, y });
            }
        }

        void cmd_arc(float cx, float cy, float radius, float start_angle, float end_angle) override
        {
            const int steps = std::max(8, static_cast<int>(std::abs(end_angle - start_angle) * radius / 8.f));
            for (int i = 0; i <= steps; ++i)
            {
                const float t = static_cast<float>(i) / static_cast<float>(steps);
                const float a = start_angle + (end_angle - start_angle) * t;
                path_.push_back({ cx + std::cos(a) * radius, cy + std::sin(a) * radius });
            }
        }

        void cmd_close_path() override { path_closed_ = true; }

        void cmd_fill(const FillStyle& style) override
        {
            if (path_.size() < 3) return;
            set_color(style.color);
            glBegin(GL_TRIANGLE_FAN);
            for (const Point& p : path_) glVertex2f(p.x, p.y);
            glEnd();
        }

        void cmd_stroke(const StrokeStyle& style) override
        {
            if (path_.size() < 2) return;
            set_color(style.color);
            glLineWidth(std::max(1.f, style.width));
            glBegin(path_closed_ ? GL_LINE_LOOP : GL_LINE_STRIP);
            for (const Point& p : path_) glVertex2f(p.x, p.y);
            glEnd();
            glLineWidth(1.f);
        }

        void cmd_shadow(const ShadowStyle&) override {}

        void cmd_fill_round_rect(float x, float y, float w, float h, float radius, const FillStyle& style) override
        {
            build_round_rect(x, y, w, h, radius);
            cmd_fill(style);
        }

        void cmd_stroke_round_rect(float x, float y, float w, float h, float radius, const StrokeStyle& style) override
        {
            build_round_rect(x, y, w, h, radius);
            path_closed_ = true;
            cmd_stroke(style);
        }

        void cmd_push_round_clip(float, float, float, float, float) override {}
        void cmd_pop_clip() override {}
        void cmd_blur_rect(float, float, float, float, float) override {}
        void cmd_feather_rect(float, float, float, float, float, float) override {}

        void cmd_text(const std::wstring&, float, float, float, float, const TextStyle&) override {}

        bool cmd_measure_text(const std::wstring& content, float, float, const TextStyle& style, TextMetrics& out) override
        {
            out.width = static_cast<float>(content.size()) * style.font_size * 0.55f;
            out.height = style.font_size * 1.25f;
            out.line_count = content.empty() ? 0u : 1u;
            return true;
        }

        void cmd_draw_bitmap(const std::wstring&, float, float, float, float, float, float) override {}
        void cmd_draw_bitmap_bgra(const uint8_t*, int, int, int, float, float, float, float, float, float) override {}

    private:
        void set_color(Color color)
        {
            const float opacity = opacity_stack_.empty() ? 1.f : opacity_stack_.back();
            glColor4f(color.r, color.g, color.b, color.a * opacity);
        }

        void build_round_rect(float x, float y, float w, float h, float radius)
        {
            path_.clear();
            radius = std::clamp(radius, 0.f, std::min(w, h) * .5f);
            if (radius <= 0.f)
            {
                path_ = { {x,y}, {x+w,y}, {x+w,y+h}, {x,y+h} };
                path_closed_ = true;
                return;
            }
            append_arc(x + w - radius, y + radius, radius, -1.5707963f, 0.f);
            append_arc(x + w - radius, y + h - radius, radius, 0.f, 1.5707963f);
            append_arc(x + radius, y + h - radius, radius, 1.5707963f, 3.1415926f);
            append_arc(x + radius, y + radius, radius, 3.1415926f, 4.712389f);
            path_closed_ = true;
        }

        void append_arc(float cx, float cy, float radius, float a0, float a1)
        {
            for (int i = 0; i <= 8; ++i)
            {
                const float t = static_cast<float>(i) / 8.f;
                const float a = a0 + (a1 - a0) * t;
                path_.push_back({ cx + std::cos(a) * radius, cy + std::sin(a) * radius });
            }
        }

        unsigned int width_ = 1;
        unsigned int height_ = 1;
        bool color_key_mode_ = false;
        Color resize_clear_color_{ 0.08f, 0.08f, 0.1f, 1.f };
        std::vector<Point> path_;
        std::vector<float> opacity_stack_;
        bool path_closed_ = false;
    };
}
