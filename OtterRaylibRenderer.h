

// ============================================================
//  RaylibRenderContext
//  仅在定义 OTTER_USE_RAYLIB 时编译。
//  使用方式：在包含本头文件之前 #define OTTER_USE_RAYLIB
//
//  Shader 支持：
//    图层通过 layer->shader(shd.id) 指定一个 Raylib Shader。
//    flush() 渲染该图层内容时，会在 RenderTexture2D 上先绘制，
//    再以该 Shader 将纹理绘制到屏幕，实现逐图层后处理效果。
//
//  注意：
//    - Raylib 窗口由 RaylibRenderContext::initialize() 创建，
//      不再使用 Win32 HWND（与 D2D 后端互斥）。
//    - 文字渲染使用 Raylib DrawTextEx，字体需提前加载。
//    - 图片加载使用 Raylib LoadTexture，结果缓存在 texture_cache_。
// ============================================================
#ifdef OTTER_USE_RAYLIB

#include "OtterLayer.h"

// ── Win32 與 Raylib 符號衝突修復 ─────────────────────────────
// windows.h 的宏衝突：先 undef 再讓 raylib.h 重新定義
#ifdef DrawText
#  undef DrawText
#endif
#ifdef DrawTextEx
#  undef DrawTextEx
#endif
#ifdef LoadImage
#  undef LoadImage
#endif
#ifdef ShowCursor
#  undef ShowCursor
#endif
#ifdef CloseWindow
#  undef CloseWindow
#endif
// GDI 的 Rectangle 是函數聲明，與 Raylib 的 Rectangle typedef 衝突。
// 用 #pragma push_macro 保存後 undef，讓 Raylib typedef 優先。
#pragma push_macro("Rectangle")
#undef Rectangle

#include "raylib.h"
#include "rlgl.h"

#pragma pop_macro("Rectangle")  // 恢復 GDI Rectangle（若有需要）

#include <unordered_map>
#include <string>
#include <string_view>
#include <vector>
#include <stack>
#include <cmath>

namespace Otter
{
    // ============================================================
    //  RaylibRenderContext
    //  实现 Otter::RenderContext 接口的 Raylib/OpenGL 渲染上下文。
    // ============================================================
    class RaylibRenderContext : public RenderContext
    {
    public:
        RaylibRenderContext()  = default;
        ~RaylibRenderContext() noexcept { release_all(); }

        RaylibRenderContext(const RaylibRenderContext&)            = delete;
        RaylibRenderContext& operator=(const RaylibRenderContext&) = delete;

        // ── 初始化 / 销毁 ────────────────────────────────────────

        void initialize(int width, int height, const char* title)
        {
            width_  = width;
            height_ = height;
            SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
            InitWindow(width, height, title);
            SetTargetFPS(60);

            // 创建离屏纹理（用于 shader 图层）
            layer_rt_ = LoadRenderTexture(width, height);
        }

        void initialize_native(void*, unsigned int width, unsigned int height) override
        {
            if (!IsWindowReady())
                initialize(static_cast<int>(width), static_cast<int>(height), "Otter Raylib");
            else
                resize(width, height);
        }

        void release_all()
        {
            for (auto& [k, tex] : texture_cache_)
                UnloadTexture(tex);
            texture_cache_.clear();
            for (auto& [id, shader] : shaders_)
                UnloadShader(shader);
            shaders_.clear();

            if (layer_rt_.id != 0)
            {
                UnloadRenderTexture(layer_rt_);
                layer_rt_ = {};
            }

            if (IsWindowReady()) ::CloseWindow();
        }

        void resize(unsigned int width, unsigned int height) override
        {
            width_  = static_cast<int>(width);
            height_ = static_cast<int>(height);
            if (layer_rt_.id != 0) UnloadRenderTexture(layer_rt_);
            layer_rt_ = LoadRenderTexture(width_, height_);
        }

        void set_resize_clear_color(const Color& color) override { resize_clear_color_ = color; }
        void set_color_key_mode(bool enable) override { color_key_mode_ = enable; }

        bool should_close() const { return WindowShouldClose(); }

        // ── 帧生命周期 ───────────────────────────────────────────

        void begin_frame(Color clear_col = Color{0,0,0,1}) override
        {
            BeginDrawing();
            ClearBackground({ (unsigned char)(clear_col.r * 255),
                              (unsigned char)(clear_col.g * 255),
                              (unsigned char)(clear_col.b * 255),
                              (unsigned char)(clear_col.a * 255) });
            // 重置路径缓冲
            path_points_.clear();
            path_started_ = false;
        }

        bool end_frame() override
        {
            EndDrawing();
            return true;
        }

        // ── 变换栈 ───────────────────────────────────────────────

        void push_transform(float tx, float ty,
                            float sx = 1.f, float sy = 1.f,
                            float rot = 0.f) override
        {
            Transform2D t;
            t.tx = tx; t.ty = ty;
            t.sx = sx; t.sy = sy;
            t.rot = rot;
            // 累加父层偏移
            if (!transform_stack_.empty())
            {
                const Transform2D& p = transform_stack_.back();
                t.world_tx = p.world_tx + tx * p.sx;
                t.world_ty = p.world_ty + ty * p.sy;
            }
            else
            {
                t.world_tx = tx;
                t.world_ty = ty;
            }
            transform_stack_.push_back(t);
        }

        void pop_transform() override
        {
            if (!transform_stack_.empty())
                transform_stack_.pop_back();
        }

        // ── 样式栈 ───────────────────────────────────────────────

        void push_style(const LayerStyle& style) override
        {
            float parent_op = opacity_stack_.empty() ? 1.f : opacity_stack_.back();
            opacity_stack_.push_back(parent_op * style.opacity);

            // 记录当前图层的 shader（从 LayerEffect 传入）
            // 注意：LayerEffect 的 shader_id 通过 push_layer_shader() 单独传入
        }

        void pop_style() override
        {
            if (!opacity_stack_.empty()) opacity_stack_.pop_back();
        }

        // ── 路径操作 ─────────────────────────────────────────────

        void cmd_move_to(float x, float y) override
        {
            // 开始新路径段
            if (!path_points_.empty() && path_started_)
                path_segments_.push_back(path_points_);
            path_points_.clear();
            path_points_.push_back({ apply_tx(x), apply_ty(y) });
            path_started_ = true;
        }

        void cmd_line_to(float x, float y) override
        {
            path_points_.push_back({ apply_tx(x), apply_ty(y) });
        }

        void cmd_bezier_to(float cx1, float cy1,
                           float cx2, float cy2,
                           float ex,  float ey) override
        {
            // 将贝塞尔曲线细分为折线（20段）
            if (path_points_.empty()) return;
            Vector2 p0 = path_points_.back();
            float ax = apply_tx(cx1), ay = apply_ty(cy1);
            float bx = apply_tx(cx2), by = apply_ty(cy2);
            float endx = apply_tx(ex), endy = apply_ty(ey);
            for (int i = 1; i <= 20; ++i)
            {
                float t  = (float)i / 20.f;
                float mt = 1.f - t;
                float px = mt*mt*mt*p0.x + 3*mt*mt*t*ax + 3*mt*t*t*bx + t*t*t*endx;
                float py = mt*mt*mt*p0.y + 3*mt*mt*t*ay + 3*mt*t*t*by + t*t*t*endy;
                path_points_.push_back({ px, py });
            }
        }

        void cmd_arc(float cx, float cy, float radius,
                     float start_angle, float end_angle) override
        {
            float sweep = end_angle - start_angle;
            int   segs  = std::max(8, (int)(fabsf(sweep) / (OTTER_PI / 18.f)));
            float wcx = apply_tx(cx), wcy = apply_ty(cy);

            // cmd_move_to 会先放一个起点进来，arc 直接覆盖它，
            // 用弧线采样点替换（避免多余顶点破坏三角扇填充）
            path_points_.clear();
            for (int i = 0; i <= segs; ++i)
            {
                float a = start_angle + sweep * ((float)i / (float)segs);
                path_points_.push_back({ wcx + radius * cosf(a),
                                         wcy + radius * sinf(a) });
            }
        }

        void cmd_close_path() override
        {
            if (path_points_.size() >= 2)
                path_points_.push_back(path_points_.front());  // 闭合
        }

        // ── 绘制指令 ─────────────────────────────────────────────

        void cmd_fill(const FillStyle& style) override
        {
            flush_segments();
            if (path_points_.size() < 3) { reset_path(); return; }

            ::Color c = to_rl_color(style.color, effective_opacity());
            // Raylib 无内置多边形填充 API，使用 rlgl 三角扇
            rlBegin(RL_TRIANGLES);
            rlColor4ub(c.r, c.g, c.b, c.a);
            Vector2 center = centroid(path_points_);
            for (size_t i = 0; i + 1 < path_points_.size(); ++i)
            {
                rlVertex2f(center.x, center.y);
                rlVertex2f(path_points_[i].x, path_points_[i].y);
                rlVertex2f(path_points_[i+1].x, path_points_[i+1].y);
            }
            rlEnd();
            reset_path();
            pending_shadow_.reset();
        }

        void cmd_stroke(const StrokeStyle& style) override
        {
            flush_segments();
            if (path_points_.size() < 2) { reset_path(); return; }

            ::Color c = to_rl_color(style.color, effective_opacity());
            for (size_t i = 0; i + 1 < path_points_.size(); ++i)
                DrawLineEx(path_points_[i], path_points_[i+1], style.width, c);
            reset_path();
            pending_shadow_.reset();
        }

        void cmd_shadow(const ShadowStyle& style) override
        {
            pending_shadow_ = style;
        }

        void cmd_fill_round_rect(float x, float y, float w, float h,
                                  float radius, const FillStyle& style) override
        {
            float wx = apply_tx(x), wy = apply_ty(y);
            ::Color c = to_rl_color(style.color, effective_opacity());
            if (radius <= 0.f)
                DrawRectangleRec({ wx, wy, w, h }, c);
            else
                DrawRectangleRounded({ wx, wy, w, h }, radius / (std::min(w, h) * 0.5f), 8, c);
        }

        void cmd_stroke_round_rect(float x, float y, float w, float h,
                                    float radius, const StrokeStyle& style) override
        {
            float wx = apply_tx(x), wy = apply_ty(y);
            ::Color c = to_rl_color(style.color, effective_opacity());
            if (radius <= 0.f)
                DrawRectangleLinesEx({ wx, wy, w, h }, style.width, c);
            else
                DrawRectangleRoundedLinesEx({ wx, wy, w, h },
                    radius / (std::min(w, h) * 0.5f), 8, style.width, c);
        }

        void cmd_push_round_clip(float x, float y, float w, float h,
                                  float /*radius*/) override
        {
            // Raylib 无原生裁切，使用 scissor 矩形（忽略圆角）
            float wx = apply_tx(x), wy = apply_ty(y);
            BeginScissorMode((int)wx, (int)wy, (int)w, (int)h);
            scissor_depth_++;
        }

        void cmd_pop_clip() override
        {
            if (scissor_depth_ > 0)
            {
                EndScissorMode();
                scissor_depth_--;
            }
        }

        void cmd_blur_rect(float /*x*/, float /*y*/, float /*w*/, float /*h*/,
                            float /*radius*/) override
        {
            // Raylib 后端：模糊需通过 shader 实现，此处留空
            // 用户可通过 layer->shader(blur_shader_id) 实现
        }

        void cmd_feather_rect(float x, float y, float w, float h,
                               float /*radius*/, float feather) override
        {
            // 简单近似：在边缘绘制渐变透明矩形
            float wx = apply_tx(x), wy = apply_ty(y);
            float op = effective_opacity();
            // 上边缘
            DrawRectangleGradientV((int)wx, (int)wy, (int)w, (int)feather,
                { 0,0,0,0 }, { 0,0,0,(unsigned char)(op*255) });
            // 下边缘
            DrawRectangleGradientV((int)wx, (int)(wy+h-feather), (int)w, (int)feather,
                { 0,0,0,(unsigned char)(op*255) }, { 0,0,0,0 });
        }

        void cmd_text(const std::wstring& content,
                      float x, float y, float max_width, float max_height,
                      const TextStyle& style) override
        {
            // 将 wstring 转为 UTF-8
            std::string utf8 = wstring_to_utf8(content);
            float wx = apply_tx(x), wy = apply_ty(y);
            ::Color c = to_rl_color(style.color, effective_opacity());

            Font font = GetFontDefault();
            float font_size = style.font_size > 0.f ? style.font_size : 16.f;
            float spacing   = style.letter_spacing;

            // 简单换行处理
            if (style.word_wrap && max_width > 0.f)
                DrawTextBoxed(font, utf8.c_str(),
                    { wx, wy, max_width, max_height > 0.f ? max_height : 9999.f },
                    font_size, spacing, true, c);
            else
                DrawTextEx(font, utf8.c_str(), { wx, wy }, font_size, spacing, c);
        }

        void cmd_draw_bitmap(const std::wstring& path,
                             float x, float y, float w, float h,
                             float opacity, float radius) override
        {
            std::string utf8 = wstring_to_utf8(path);
            Texture2D tex = get_or_load_texture(utf8);
            if (tex.id == 0) return;

            float wx = apply_tx(x), wy = apply_ty(y);
            float op = effective_opacity() * opacity;
            ::Color tint = { 255, 255, 255, (unsigned char)(op * 255) };

            if (radius > 0.f)
            {
                // 圆角图片：用 scissor 近似（完整圆角需 shader）
                BeginScissorMode((int)wx, (int)wy, (int)w, (int)h);
                DrawTexturePro(tex,
                    { 0, 0, (float)tex.width, (float)tex.height },
                    { wx, wy, w, h }, { 0, 0 }, 0.f, tint);
                EndScissorMode();
            }
            else
            {
                DrawTexturePro(tex,
                    { 0, 0, (float)tex.width, (float)tex.height },
                    { wx, wy, w, h }, { 0, 0 }, 0.f, tint);
            }
        }

        void cmd_draw_bitmap_bgra(const uint8_t* pixels,
                                  int width, int height, int stride,
                                  float x, float y, float w, float h,
                                  float opacity, float radius) override
        {
            if (!pixels || width <= 0 || height <= 0 || stride < width * 4)
                return;
            std::vector<unsigned char> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
            for (int row = 0; row < height; ++row)
            {
                const uint8_t* src = pixels + static_cast<size_t>(row) * static_cast<size_t>(stride);
                unsigned char* dst = rgba.data() + static_cast<size_t>(row) * static_cast<size_t>(width) * 4u;
                for (int col = 0; col < width; ++col)
                {
                    dst[col * 4 + 0] = src[col * 4 + 2];
                    dst[col * 4 + 1] = src[col * 4 + 1];
                    dst[col * 4 + 2] = src[col * 4 + 0];
                    dst[col * 4 + 3] = src[col * 4 + 3];
                }
            }
            Image image{};
            image.data = rgba.data();
            image.width = width;
            image.height = height;
            image.mipmaps = 1;
            image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            Texture2D tex = LoadTextureFromImage(image);
            if (tex.id == 0) return;

            float wx = apply_tx(x), wy = apply_ty(y);
            ::Color tint = { 255, 255, 255, static_cast<unsigned char>(effective_opacity() * std::clamp(opacity, 0.f, 1.f) * 255.f) };
            if (radius > 0.f)
                BeginScissorMode(static_cast<int>(wx), static_cast<int>(wy), static_cast<int>(w), static_cast<int>(h));
            DrawTexturePro(tex, { 0, 0, static_cast<float>(width), static_cast<float>(height) },
                           { wx, wy, w, h }, { 0, 0 }, 0.f, tint);
            if (radius > 0.f)
                EndScissorMode();
            UnloadTexture(tex);
        }

        bool cmd_measure_text(const std::wstring& content,
                               float max_width, float /*max_height*/,
                               const TextStyle& style,
                               TextMetrics& out) override
        {
            std::string utf8 = wstring_to_utf8(content);
            Font font = GetFontDefault();
            float font_size = style.font_size > 0.f ? style.font_size : 16.f;
            float spacing   = style.letter_spacing;

            Vector2 sz = MeasureTextEx(font, utf8.c_str(), font_size, spacing);
            out.width       = sz.x;
            out.height      = sz.y;
            out.line_height = font_size;
            out.line_count  = 1;
            out.truncated   = (max_width > 0.f && sz.x > max_width);
            return true;
        }

        bool supports_layer_shader() const override
        {
            return true;
        }

        unsigned int create_shader(std::string_view fragment_source,
                                   std::string_view vertex_source = {}) override
        {
            if (fragment_source.empty()) return 0;
            std::string fs(fragment_source);
            std::string vs(vertex_source);
            Shader shader = LoadShaderFromMemory(vs.empty() ? nullptr : vs.c_str(), fs.c_str());
            if (shader.id == 0) return 0;
            const unsigned int id = next_shader_id_++;
            shaders_[id] = shader;
            return id;
        }

        void destroy_shader(unsigned int shader_id) override
        {
            auto it = shaders_.find(shader_id);
            if (it == shaders_.end()) return;
            UnloadShader(it->second);
            shaders_.erase(it);
        }

        bool begin_layer_shader(unsigned int shader_id, float width, float height) override
        {
            if (shader_id == 0 || width <= 0.f || height <= 0.f) return false;
            if (shaders_.find(shader_id) == shaders_.end()) return false;
            active_shader_id_ = shader_id;
            BeginTextureMode(layer_rt_);
            ClearBackground(::BLANK);
            return true;
        }

        void end_layer_shader(unsigned int shader_id) override
        {
            if (shader_id == 0 || active_shader_id_ == 0) return;
            EndTextureMode();

            auto it = shaders_.find(shader_id);
            if (it == shaders_.end())
            {
                active_shader_id_ = 0;
                return;
            }

            Shader& shd = it->second;
            const int resolution_loc = GetShaderLocation(shd, "u_resolution");
            if (resolution_loc >= 0)
            {
                const float resolution[2] = { static_cast<float>(width_), static_cast<float>(height_) };
                SetShaderValue(shd, resolution_loc, resolution, SHADER_UNIFORM_VEC2);
            }
            const int time_loc = GetShaderLocation(shd, "u_time");
            if (time_loc >= 0)
            {
                const float time = static_cast<float>(GetTime());
                SetShaderValue(shd, time_loc, &time, SHADER_UNIFORM_FLOAT);
            }

            BeginShaderMode(shd);
            DrawTextureRec(layer_rt_.texture,
                { 0, 0, (float)layer_rt_.texture.width, -(float)layer_rt_.texture.height },
                { 0, 0 }, ::WHITE);
            EndShaderMode();

            active_shader_id_ = 0;
        }

        // ── 窗口尺寸 ─────────────────────────────────────────────
        int width()  const { return width_; }
        int height() const { return height_; }

    private:
        // ── 内部变换辅助 ─────────────────────────────────────────
        struct Transform2D
        {
            float tx = 0, ty = 0, sx = 1, sy = 1, rot = 0;
            float world_tx = 0, world_ty = 0;
        };

        float apply_tx(float x) const
        {
            if (transform_stack_.empty()) return x;
            return transform_stack_.back().world_tx + x;
        }
        float apply_ty(float y) const
        {
            if (transform_stack_.empty()) return y;
            return transform_stack_.back().world_ty + y;
        }

        float effective_opacity() const
        {
            return opacity_stack_.empty() ? 1.f : opacity_stack_.back();
        }

        // ── 路径辅助 ─────────────────────────────────────────────
        void flush_segments()
        {
            for (auto& seg : path_segments_)
                for (auto& pt : seg)
                    path_points_.push_back(pt);
            path_segments_.clear();
        }

        void reset_path()
        {
            path_points_.clear();
            path_segments_.clear();
            path_started_ = false;
        }

        static Vector2 centroid(const std::vector<Vector2>& pts)
        {
            float cx = 0, cy = 0;
            for (auto& p : pts) { cx += p.x; cy += p.y; }
            float n = (float)pts.size();
            return { cx / n, cy / n };
        }

        // ── 颜色转换 ─────────────────────────────────────────────
        static ::Color to_rl_color(const Otter::Color& c, float opacity = 1.f)
        {
            return {
                (unsigned char)(c.r * 255),
                (unsigned char)(c.g * 255),
                (unsigned char)(c.b * 255),
                (unsigned char)(c.a * opacity * 255)
            };
        }

        // ── 纹理缓存 ─────────────────────────────────────────────
        Texture2D get_or_load_texture(const std::string& path)
        {
            auto it = texture_cache_.find(path);
            if (it != texture_cache_.end()) return it->second;
            Texture2D tex = LoadTexture(path.c_str());
            if (tex.id != 0) texture_cache_[path] = tex;
            return tex;
        }

        // ── wstring → UTF-8 ──────────────────────────────────────
        static std::string wstring_to_utf8(const std::wstring& ws)
        {
            if (ws.empty()) return {};
            std::string out;
            out.reserve(ws.size());
            for (wchar_t ch : ws)
                out.push_back(ch >= 32 && ch <= 126 ? static_cast<char>(ch) : '?');
            return out;
        }

        // DrawTextBoxed 辅助（Raylib 无内置，简单实现）
        static void DrawTextBoxed(Font font, const char* text, ::Rectangle rec,
                                   float fontSize, float spacing, bool wordWrap, ::Color tint)
        {
            DrawTextEx(font, text, { rec.x, rec.y }, fontSize, spacing, tint);
        }

        // ── 成员变量 ─────────────────────────────────────────────
        int width_  = 800;
        int height_ = 600;
        Color resize_clear_color_{ 0.f, 0.f, 0.f, 1.f };
        bool color_key_mode_ = false;

        std::vector<Transform2D>  transform_stack_;
        std::vector<float>        opacity_stack_;

        // 路径缓冲
        std::vector<Vector2>              path_points_;
        std::vector<std::vector<Vector2>> path_segments_;
        bool                              path_started_ = false;

        std::optional<ShadowStyle> pending_shadow_;

        // 离屏纹理（shader 图层用）
        RenderTexture2D layer_rt_ = {};
        unsigned int    active_shader_id_ = 0;
        unsigned int    next_shader_id_ = 1;

        int scissor_depth_ = 0;

        std::unordered_map<std::string, Texture2D> texture_cache_;
        std::unordered_map<unsigned int, Shader> shaders_;
    };

} // namespace Otter (Raylib)

#endif // OTTER_USE_RAYLIB
