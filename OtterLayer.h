#pragma once
// ============================================================
//  OtterLayer.h
//  水獭图形框架 —— 图层 / 画布核心定义
//
//  设计理念：
//    第一个创建的图层即为"画布（Canvas）"，所有后续图层
//    均挂载在其下方，形成一棵图层树。
//
//    图层内部维护一条"绘画链（PaintChain）"，每个绘制操作
//    都是链上的一个节点，按顺序执行，模拟真实绘画流程。
//
//    事件回调（on_update / on_render）返回 bool：
//      true  → 持续触发（每帧）
//      false → 自动注销（一次性动画 / 条件终止）
//
//  命名空间：Otter
//  C++ 标准：C++20
// ============================================================

#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <stdexcept>
#include <cassert>
#include <algorithm>
#include <optional>
#include <cstdint>
#include <cmath>
#include <type_traits>

#ifndef OTTER_PI
#define OTTER_PI 3.14159265358979323846f
#endif

namespace Otter
{
    // ============================================================
    //  前向声明
    // ============================================================
    class Layer;
    class PaintChain;
    struct LayerRef;


    // ============================================================
    //  矩形区域（Rect）
    //  用于图层的鼠标交互命中测试（hit_area）。
    //  未设置 hit_area 的图层：全窗口范围均可响应鼠标事件。
    //  设置后：仅在矩形范围内的鼠标事件才会被分发到该图层。
    // ============================================================
    struct Rect
    {
        float x = 0, y = 0, width = 0, height = 0;

        // 判断点 (px, py) 是否在矩形内（含边界）
        constexpr bool contains(float px, float py) const noexcept
        {
            return px >= x && px <= x + width
                && py >= y && py <= y + height;
        }
    };

    struct BitmapUpdateRect
    {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
    };


    // ============================================================
    //  鼠标事件数据（MouseEvent）
    //  每次鼠标事件触发时，框架将此结构传递给回调函数。
    //
    //  坐标系：窗口客户区左上角为原点，向右/下为正方向（像素）。
    // ============================================================
    struct MouseEvent
    {
        // ── 鼠标位置 ─────────────────────────────────────────────
        float x         = 0;   // 当前鼠标 X 坐标（客户区像素）
        float y         = 0;   // 当前鼠标 Y 坐标（客户区像素）
        float delta_x   = 0;   // 本次移动的 X 增量（仅 on_mouse_move 有效）
        float delta_y   = 0;   // 本次移动的 Y 增量（仅 on_mouse_move 有效）

        // ── 滚轮 ─────────────────────────────────────────────────
        float wheel_delta = 0; // 滚轮滚动量（正=向上滚，负=向下滚）

        // ── 按键状态（事件发生时刻的快照）──────────────────────
        bool left_down   = false;  // 左键是否处于按下状态
        bool right_down  = false;  // 右键是否处于按下状态
        bool middle_down = false;  // 中键是否处于按下状态

        // ── 修饰键 ───────────────────────────────────────────────
        bool ctrl_down   = false;  // Ctrl 键是否按下
        bool shift_down  = false;  // Shift 键是否按下
        bool alt_down    = false;  // Alt 键是否按下

        // ── 点击相关 ─────────────────────────────────────────────
        int  click_count = 1;      // 点击次数（1=单击，2=双击）
    };


    // ============================================================
    //  鼠标回调类型（MouseCallback）
    //  返回值语义（与 HTML addEventListener 的 stopPropagation 对应）：
    //    true  → 事件已处理，停止向下层传播（consume）
    //    false → 事件未处理，继续传播到更底层图层（pass through）
    //
    //  注意：鼠标回调不会自动注销（不同于 on_update / on_render）。
    //        若需移除，使用 remove_mouse_callbacks() 清空该类型回调。
    // ============================================================
    using MouseCallback = std::function<bool(const MouseEvent&)>;



    //  控制当前图层与下方图层的像素合成方式，
    //  对应 Photoshop 中的图层混合模式概念。
    // ============================================================
    enum class BlendMode : uint8_t
    {
        Normal,       // 正常：直接覆盖，受透明度影响
        Multiply,     // 正片叠底：颜色相乘，结果变暗
        Screen,       // 滤色：颜色互补相乘，结果变亮
        Overlay,      // 叠加：综合 Multiply 和 Screen
        Add,          // 线性减淡：颜色相加，高光增强
        SoftLight,    // 柔光：温和的对比度调整
        HardLight,    // 强光：强烈的对比度调整
        Difference,   // 差值：取颜色差的绝对值
        Erase,        // 擦除：用当前层的形状擦除下层内容
    };


    // ============================================================
    //  图层样式（LayerStyle）
    //  附加在图层上的全局属性，应用于整个图层的输出结果。
    // ============================================================
    struct LayerStyle
    {
        float     opacity  = 1.0f;              // 图层不透明度 [0.0, 1.0]
        BlendMode blend    = BlendMode::Normal;  // 混合模式
        bool      visible  = true;              // 是否参与渲染
        // 后续可扩展：滤镜（Filter）、遮罩（Mask）等
    };


    // ============================================================
    //  颜色（Color）
    //  使用 RGBA 浮点表示，[0.0, 1.0] 范围。
    //  提供常用构造方式。
    // ============================================================
    struct Color
    {
        float r = 0.f, g = 0.f, b = 0.f, a = 1.f;

        // 默认：纯黑色
        constexpr Color() = default;

        // 从 RGBA 浮点构造
        constexpr Color(float r, float g, float b, float a = 1.f)
            : r(r), g(g), b(b), a(a) {}

        // 从 0xRRGGBB 十六进制整数构造（alpha 默认 1.0）
        static constexpr Color from_rgb_hex(uint32_t hex, float alpha = 1.f)
        {
            return Color{
                ((hex >> 16) & 0xFF) / 255.f,
                ((hex >>  8) & 0xFF) / 255.f,
                ((hex      ) & 0xFF) / 255.f,
                alpha
            };
        }

        // 从 0xRRGGBBAA 十六进制整数构造
        static constexpr Color from_rgba_hex(uint32_t hex)
        {
            return Color{
                ((hex >> 24) & 0xFF) / 255.f,
                ((hex >> 16) & 0xFF) / 255.f,
                ((hex >>  8) & 0xFF) / 255.f,
                ((hex      ) & 0xFF) / 255.f,
            };
        }

        // 预置常用颜色
        static constexpr Color white()           { return {1,1,1,1}; }
        static constexpr Color black()           { return {0,0,0,1}; }
        static constexpr Color transparent()     { return {0,0,0,0}; }  // alpha=0 真透明
        static constexpr Color transparent_key() { return {0,0,0,1}; }  // 颜色键透明（纯黑）
        static constexpr Color red()             { return {1,0,0,1}; }
        static constexpr Color green()           { return {0,1,0,1}; }
        static constexpr Color blue()            { return {0,0,1,1}; }

        // 检查是否是纯黑色（颜色键）
        constexpr bool is_pure_black() const noexcept
        {
            return r == 0.f && g == 0.f && b == 0.f && a > 0.f;
        }

        // 转换为近黑色（避免被颜色键透明）
        constexpr Color to_near_black() const
        {
            if (is_pure_black())
                return Color{ 1.f/255.f, 1.f/255.f, 1.f/255.f, a };
            return *this;
        }

        // 颜色插值
        static constexpr Color lerp(const Color& a, const Color& b, float t)
        {
            t = std::clamp(t, 0.f, 1.f);
            return Color{
                a.r + (b.r - a.r) * t,
                a.g + (b.g - a.g) * t,
                a.b + (b.b - a.b) * t,
                a.a + (b.a - a.a) * t
            };
        }
    };


    // ============================================================
    //  描边样式（StrokeStyle）
    //  用于 PaintChain::stroke() 操作节点的参数。
    // ============================================================
    struct StrokeStyle
    {
        Color  color      = Color::black(); // 描边颜色
        float  width      = 1.0f;           // 描边宽度（像素）
        float  miter      = 4.0f;           // 斜接限制（尖角处理）

        // 线段端点形状
        enum class LineCap : uint8_t { Butt, Round, Square } cap = LineCap::Butt;

        // 线段连接形状
        enum class LineJoin : uint8_t { Miter, Round, Bevel } join = LineJoin::Miter;
    };


    // ============================================================
    //  填充样式（FillStyle）
    //  用于 PaintChain::fill() 操作节点的参数。
    // ============================================================
    // ============================================================
    //  渐变停止点（GradientStop）
    //  定义渐变中某个位置的颜色。
    // ============================================================
    struct GradientStop
    {
        float position = 0.f;  // 位置 [0.0, 1.0]
        Color color;           // 该位置的颜色

        GradientStop() = default;
        GradientStop(float pos, Color c) : position(pos), color(c) {}
    };


    // ============================================================
    //  线性渐变样式（LinearGradient）
    //  从起点到终点的线性颜色过渡。
    // ============================================================
    struct LinearGradient
    {
        float x1 = 0.f, y1 = 0.f;  // 起点
        float x2 = 100.f, y2 = 0.f; // 终点
        std::vector<GradientStop> stops;

        LinearGradient() = default;

        // 便捷构造：两点 + 起止颜色
        LinearGradient(float x1, float y1, float x2, float y2, Color start_color, Color end_color)
            : x1(x1), y1(y1), x2(x2), y2(y2)
        {
            stops.emplace_back(0.f, start_color);
            stops.emplace_back(1.f, end_color);
        }
    };


    // ============================================================
    //  径向渐变样式（RadialGradient）
    //  从中心向外的圆形/椭圆形颜色过渡。
    // ============================================================
    struct RadialGradient
    {
        float cx = 0.f, cy = 0.f;   // 中心点
        float rx = 50.f, ry = 50.f; // 半径（可椭圆）
        float ox = 0.f, oy = 0.f;   // 渐变焦点相对中心的偏移（GradientOriginOffset）
        std::vector<GradientStop> stops;

        RadialGradient() = default;

        // 便捷构造：中心 + 半径 + 起止颜色
        RadialGradient(float cx, float cy, float radius, Color inner_color, Color outer_color)
            : cx(cx), cy(cy), rx(radius), ry(radius)
        {
            stops.emplace_back(0.f, inner_color);
            stops.emplace_back(1.f, outer_color);
        }

        // 椭圆版本
        RadialGradient(float cx, float cy, float rx, float ry, Color inner_color, Color outer_color)
            : cx(cx), cy(cy), rx(rx), ry(ry)
        {
            stops.emplace_back(0.f, inner_color);
            stops.emplace_back(1.f, outer_color);
        }
    };


    // ============================================================
    //  锥形渐变（ConicGradient / 扫描渐变）
    //  围绕中心点按角度扫描的颜色过渡。
    //  D2D 1.0 无原生支持，通过三角形扇形近似实现。
    // ============================================================
    struct ConicGradient
    {
        float cx = 0.f, cy = 0.f;      // 圆心
        float start_angle = 0.f;       // 起始角度（弧度，0 = 正右方）
        std::vector<GradientStop> stops;

        ConicGradient() = default;

        // 便捷构造：整圆，从 start_color 到 end_color
        ConicGradient(float cx, float cy, Color start_color, Color end_color,
                      float start_angle = 0.f)
            : cx(cx), cy(cy), start_angle(start_angle)
        {
            stops.emplace_back(0.f, start_color);
            stops.emplace_back(1.f, end_color);
        }

        // 多停止点版本
        ConicGradient(float cx, float cy, std::vector<GradientStop> s,
                      float start_angle = 0.f)
            : cx(cx), cy(cy), start_angle(start_angle), stops(std::move(s)) {}
    };


    // ============================================================
    //  填充样式（FillStyle）
    //  支持：纯色、线性渐变、径向渐变、锥形渐变
    // ============================================================
    struct FillStyle
    {
        enum class Type : uint8_t { Solid, Linear, Radial, Conic } type = Type::Solid;

        Color color = Color::white();  // 纯色填充
        std::optional<LinearGradient> linear;   // 线性渐变
        std::optional<RadialGradient> radial;   // 径向渐变
        std::optional<ConicGradient>  conic;    // 锥形渐变

        FillStyle() = default;
        FillStyle(Color c) : type(Type::Solid), color(c) {}
        FillStyle(LinearGradient lg) : type(Type::Linear), linear(std::move(lg)) {}
        FillStyle(RadialGradient rg) : type(Type::Radial), radial(std::move(rg)) {}
        FillStyle(ConicGradient cg)  : type(Type::Conic),  conic(std::move(cg))  {}
    };


    // ============================================================
    //  阴影样式（ShadowStyle）
    //  用于 PaintChain::shadow() 操作节点的参数。
    // ============================================================
    struct ShadowStyle
    {
        Color  color    = Color{0,0,0,0.5f}; // 阴影颜色
        float  blur     = 8.0f;              // 模糊半径（像素）
        float  offset_x = 2.0f;             // X 方向偏移
        float  offset_y = 2.0f;             // Y 方向偏移
    };


    // ============================================================
    //  缓动函数类型（Easing）
    //  定义动画的加速/减速曲线
    // ============================================================
    enum class Easing : uint8_t
    {
        Linear,
        EaseInQuad,    EaseOutQuad,    EaseInOutQuad,
        EaseInCubic,   EaseOutCubic,   EaseInOutCubic,
        EaseInQuart,   EaseOutQuart,   EaseInOutQuart,
        EaseInSine,    EaseOutSine,    EaseInOutSine,
        EaseInExpo,    EaseOutExpo,    EaseInOutExpo,
        EaseInBack,    EaseOutBack,    EaseInOutBack
    };

    // 缓动函数计算
    inline float apply_easing(Easing type, float t)
    {
        switch (type)
        {
        case Easing::Linear:
            return t;

        case Easing::EaseInQuad:
            return t * t;
        case Easing::EaseOutQuad:
            return t * (2.f - t);
        case Easing::EaseInOutQuad:
            return t < 0.5f ? 2.f * t * t : -1.f + (4.f - 2.f * t) * t;

        case Easing::EaseInCubic:
            return t * t * t;
        case Easing::EaseOutCubic:
            { float t1 = t - 1.f; return t1 * t1 * t1 + 1.f; }
        case Easing::EaseInOutCubic:
            return t < 0.5f ? 4.f * t * t * t : (t - 1.f) * (2.f * t - 2.f) * (2.f * t - 2.f) + 1.f;

        case Easing::EaseInQuart:
            return t * t * t * t;
        case Easing::EaseOutQuart:
            { float t1 = t - 1.f; return 1.f - t1 * t1 * t1 * t1; }
        case Easing::EaseInOutQuart:
            return t < 0.5f ? 8.f * t * t * t * t : 1.f - 8.f * (t - 1.f) * (t - 1.f) * (t - 1.f) * (t - 1.f);

        case Easing::EaseInSine:
            return 1.f - std::cos(t * OTTER_PI / 2.f);
        case Easing::EaseOutSine:
            return std::sin(t * OTTER_PI / 2.f);
        case Easing::EaseInOutSine:
            return -(std::cos(OTTER_PI * t) - 1.f) / 2.f;

        case Easing::EaseInExpo:
            return t == 0.f ? 0.f : std::pow(2.f, 10.f * (t - 1.f));
        case Easing::EaseOutExpo:
            return t == 1.f ? 1.f : 1.f - std::pow(2.f, -10.f * t);
        case Easing::EaseInOutExpo:
            if (t == 0.f) return 0.f;
            if (t == 1.f) return 1.f;
            return t < 0.5f ? std::pow(2.f, 20.f * t - 10.f) / 2.f
                            : (2.f - std::pow(2.f, -20.f * t + 10.f)) / 2.f;

        case Easing::EaseInBack:
            { float c = 1.70158f; return (c + 1.f) * t * t * t - c * t * t; }
        case Easing::EaseOutBack:
            { float c = 1.70158f; float t1 = t - 1.f; return 1.f + (c + 1.f) * t1 * t1 * t1 + c * t1 * t1; }
        case Easing::EaseInOutBack:
            { float c = 1.70158f * 1.525f;
              return t < 0.5f ? (std::pow(2.f * t, 2.f) * ((c + 1.f) * 2.f * t - c)) / 2.f
                              : (std::pow(2.f * t - 2.f, 2.f) * ((c + 1.f) * (t * 2.f - 2.f) + c) + 2.f) / 2.f; }
        }
        return t;
    }


    // ============================================================
    //  动画轨道（AnimTrack）
    //  定义单个属性的动画
    // ============================================================
    struct AnimTrack
    {
        enum Target : uint8_t { TranslateX, TranslateY, Scale, Opacity, Rotate } target;
        float from;
        float to;
        float duration;      // 秒
        float elapsed = 0.f; // 已流逝时间
        Easing easing = Easing::Linear;
        bool  active  = true;

        AnimTrack(Target t, float f, float t2, float dur, Easing e = Easing::Linear)
            : target(t), from(f), to(t2), duration(dur), easing(e) {}
    };


    // ============================================================
    //  动画实例（Animation）
    //  可包含多个并行动画轨道
    // ============================================================
    struct Animation
    {
        uint64_t id;
        Layer*   layer;
        std::vector<AnimTrack> tracks;
        std::function<void()>  on_complete;
    };


    // ============================================================
    //  动画管理器（AnimManager）
    //  管理所有动画实例的创建、更新、销毁
    // ============================================================
    class AnimManager
    {
    public:
        // 创建动画
        uint64_t create(Layer* layer, std::vector<AnimTrack> tracks,
                        std::function<void()> on_complete = {})
        {
            uint64_t id = next_id_++;
            Animation anim;
            anim.id         = id;
            anim.layer      = layer;
            anim.tracks     = std::move(tracks);
            anim.on_complete = std::move(on_complete);
            animations_[id] = std::move(anim);
            return id;
        }

        // 更新所有动画
        void tick(float dt)
        {
            std::vector<uint64_t> to_remove;
            std::vector<std::function<void()>> pending_callbacks;

            for (auto& [id, anim] : animations_)
            {
                if (!anim.layer) continue;

                bool all_done = true;

                for (auto& track : anim.tracks)
                {
                    if (!track.active) continue;

                    track.elapsed += dt;
                    float t = std::min(track.elapsed / track.duration, 1.f);
                    t = apply_easing(track.easing, t);

                    float value = track.from + (track.to - track.from) * t;

                    // 应用到图层
                    apply_to_layer(anim.layer, track.target, value);

                    if (track.elapsed < track.duration)
                        all_done = false;
                    else
                        track.active = false;
                }

                if (all_done)
                {
                    if (anim.on_complete)
                        pending_callbacks.push_back(std::move(anim.on_complete));
                    to_remove.push_back(id);
                }
            }

            // 先删除动画，再执行回调（避免回调中调用cancel导致迭代器失效）
            for (uint64_t id : to_remove)
                animations_.erase(id);

            for (auto& cb : pending_callbacks)
                if (cb) cb();
        }

        // 取消动画
        void cancel(uint64_t id)
        {
            animations_.erase(id);
        }

        // 取消图层所有动画
        void cancel_all(Layer* layer)
        {
            std::vector<uint64_t> to_remove;
            for (auto& [id, anim] : animations_)
            {
                if (anim.layer == layer)
                    to_remove.push_back(id);
            }
            for (uint64_t id : to_remove)
                animations_.erase(id);
        }

        // 查询动画是否完成
        bool is_done(uint64_t id) const
        {
            return animations_.find(id) == animations_.end();
        }

    private:
        void apply_to_layer(Layer* layer, AnimTrack::Target target, float value);

        std::unordered_map<uint64_t, Animation> animations_;
        uint64_t next_id_ = 1;
    };


    // ============================================================
    //  文字样式（TextStyle）
    //
    //  控制文本的字体、大小、颜色、间距、对齐、装饰等所有属性。
    //  与 CSS font / text 属性集对应，通过 PaintChain::text() 使用。
    //  注意：依赖 ShadowStyle，必须在其之后声明。
    //
    //  坐标单位：DIP（Device Independent Pixel，设备无关像素）
    //    1 DIP = 1/96 英寸；96 DPI 下 1 DIP = 1 物理像素。
    // ============================================================
    struct TextStyle
    {
        // ── 字体 ─────────────────────────────────────────────────
        std::wstring font_family = L"Segoe UI"; // 字体族名（系统字体）
        float        font_size   = 14.f;        // 字号（DIP）

        // 字重（数值与 CSS font-weight / DWRITE_FONT_WEIGHT 对应）
        enum class Weight : uint16_t
        {
            Thin       = 100, ExtraLight = 200, Light    = 300,
            Regular    = 400, Medium     = 500, SemiBold = 600,
            Bold       = 700, ExtraBold  = 800, Black    = 900,
        } weight = Weight::Regular;

        // 字形（正体 / 斜体 / 仿斜体）
        enum class FontStyle : uint8_t { Normal, Italic, Oblique }
            font_style = FontStyle::Normal;

        // ── 颜色与基础装饰 ───────────────────────────────────────
        Color color         = Color::white();
        bool  underline     = false;
        bool  strikethrough = false;

        // ── 文字阴影（无值 = 不启用）─────────────────────────────
        std::optional<ShadowStyle> shadow;

        // ── 描边（外框，多层偏移模拟）────────────────────────────
        float stroke_width  = 0.f;
        Color stroke_color  = Color::transparent();

        // ── 间距 ─────────────────────────────────────────────────
        float letter_spacing = 0.f;  // 字符间距增量（DIP）
        float line_spacing   = 0.f;  // 行间距（DIP，0 = 字体自动）

        // ── 对齐 ─────────────────────────────────────────────────
        enum class HAlign : uint8_t { Left, Center, Right, Justified }
            h_align = HAlign::Left;
        enum class VAlign : uint8_t { Top, Middle, Bottom }
            v_align = VAlign::Top;

        // 是否自动换行（需在 text() 中传入 max_width）
        bool word_wrap = false;

        // ── 渲染模式（抗锯齿）────────────────────────────────────
        enum class RenderMode : uint8_t
        {
            Default,    // 系统默认（通常 ClearType）
            Aliased,    // 无抗锯齿
            GrayScale,  // 灰度抗锯齿（动画/旋转文字推荐）
            ClearType,  // 子像素 ClearType（静态水平文字最清晰）
        } render_mode = RenderMode::Default;
    };


    // ============================================================
    //  滚动条样式（ScrollBarStyle）
    //  控制单根滚动条（垂直或水平）的外观与行为。
    // ============================================================
    struct ScrollBarStyle
    {
        enum class Visibility : uint8_t
        {
            Auto,   // 内容超出时自动显示
            Always, // 始终显示
            Never,  // 始终隐藏
        } visibility = Visibility::Auto;

        float width     = 6.f;    // 滚动条宽度（px）
        float radius    = 3.f;    // 滑块圆角（px）
        float min_thumb = 20.f;   // 滑块最小长度（px）
        float margin    = 4.f;    // 距视口边缘距离（px）

        Color track       = Color{1,1,1, 0.05f}; // 轨道背景色
        Color thumb       = Color{1,1,1, 0.25f}; // 滑块正常色
        Color thumb_hover = Color{1,1,1, 0.50f}; // 滑块悬浮色
        Color thumb_press = Color{1,1,1, 0.80f}; // 滑块按下色
    };


    // ============================================================
    //  滚动配置（ScrollConfig）
    //  设置在图层上，使该图层具备滚动能力。
    //
    //  content_height / content_width：可滚动内容的总尺寸（px）。
    //    需由用户指定；框架不自动测量。
    //    0 = 与视口同尺寸（该方向不可滚动）。
    //
    //  使用方式：
    //    layer->enable_scroll(1200.f)       // 仅垂直滚动，内容高1200px
    //    layer->enable_scroll(config)       // 完整配置
    //    layer->scroll_to(0, 300)           // 跳转到指定位置
    //    layer->scroll_offset_y()           // 读取当前滚动量
    // ============================================================
    struct ScrollConfig
    {
        float content_width  = 0.f;  // 内容总宽（0 = 不横向滚动）
        float content_height = 0.f;  // 内容总高（0 = 不纵向滚动）
        float wheel_step     = 60.f; // 每格滚轮滚动量（px）

        ScrollBarStyle v_bar;        // 垂直滚动条样式
        ScrollBarStyle h_bar;        // 水平滚动条样式
    };


    // ============================================================
    //  文本度量结果（TextMetrics）
    //  由 RenderContext::cmd_measure_text() 填充。
    // ============================================================
    struct TextMetrics
    {
        float width       = 0.f;
        float height      = 0.f;
        float line_height = 0.f;
        int   line_count  = 1;
        bool  truncated   = false;
    };


    // ============================================================
    //  图层变换（LayerTransform）
    //  描述图层相对于父层的平移、缩放、旋转。
    //  由 Layer::translate() / scale() / rotate() 设置。
    //  与 LayoutPos 冲突时以最后调用的为准（PosSource 标志控制）。
    // ============================================================
    struct LayerTransform
    {
        float tx  = 0.f;   // X 方向平移（像素）
        float ty  = 0.f;   // Y 方向平移（像素）
        float sx  = 1.f;   // X 方向缩放（1.0 = 不缩放）
        float sy  = 1.f;   // Y 方向缩放（1.0 = 不缩放）
        float rot = 0.f;   // 旋转角度（弧度，顺时针）

        bool is_identity() const noexcept
        {
            return tx == 0.f && ty == 0.f
                && sx == 1.f && sy == 1.f
                && rot == 0.f;
        }
    };


    // ============================================================
    //  网格布局配置（LayoutConfig）
    //  设置在某图层上，使该层成为"布局容器"。
    //  子层可以通过 LayoutPos() 按网格定位。
    //
    //  坐标体系：所有值均为像素，相对于窗口客户区左上角。
    //
    //  网格结构示意（cols=3, rows=2）：
    //    origin_x/y
    //      │← pad_x →│ cell │← gap_x →│ cell │← gap_x →│ cell │← pad_x →│
    // ============================================================
    struct LayoutConfig
    {
        int   cols     = 1;    // 列数
        int   rows     = 1;    // 行数
        float ref_w    = 0.f;  // 布局区域总宽度（像素，通常 = 窗口宽）
        float ref_h    = 0.f;  // 布局区域总高度（像素，通常 = 窗口高）
        float origin_x = 0.f;  // 网格区域起始 X（像素）
        float origin_y = 0.f;  // 网格区域起始 Y（像素）
        float pad_x    = 0.f;  // 左右内边距（像素）
        float pad_y    = 0.f;  // 上下内边距（像素）
        float gap_x    = 0.f;  // 列间距（像素）
        float gap_y    = 0.f;  // 行间距（像素）

        // 单元格宽度（考虑内边距和间距后每列的可用宽度）
        float cell_w() const noexcept
        {
            if (cols <= 0 || ref_w <= 0.f) return 0.f;
            return (ref_w - pad_x * 2.f - gap_x * (float)(cols - 1)) / (float)cols;
        }

        // 单元格高度
        float cell_h() const noexcept
        {
            if (rows <= 0 || ref_h <= 0.f) return 0.f;
            return (ref_h - pad_y * 2.f - gap_y * (float)(rows - 1)) / (float)rows;
        }

        // 第 col 列的左边缘 X 坐标（像素，世界坐标）
        float pos_x(int col) const noexcept
        {
            return origin_x + pad_x + (float)col * (cell_w() + gap_x);
        }

        // 第 row 行的上边缘 Y 坐标（像素，世界坐标）
        float pos_y(int row) const noexcept
        {
            return origin_y + pad_y + (float)row * (cell_h() + gap_y);
        }

        // 跨 col_span 列的总宽度（含中间间距）
        float span_w(int col_span) const noexcept
        {
            if (col_span <= 0) return 0.f;
            return (float)col_span * cell_w() + (float)(col_span - 1) * gap_x;
        }

        // 跨 row_span 行的总高度（含中间间距）
        float span_h(int row_span) const noexcept
        {
            if (row_span <= 0) return 0.f;
            return (float)row_span * cell_h() + (float)(row_span - 1) * gap_y;
        }

        // 是否有效（ref_w/ref_h 必须 > 0 才能计算）
        bool is_valid() const noexcept
        {
            return cols > 0 && rows > 0 && ref_w > 0.f && ref_h > 0.f;
        }
    };


    // ============================================================
    //  网格位置（LayoutPosition）
    //  设置在子图层上，表示该层在父层网格中的位置和跨度。
    //  由 Layer::LayoutPos() 设置。
    // ============================================================
    struct LayoutPosition
    {
        int col      = 0;   // 列索引（从 0 开始）
        int row      = 0;   // 行索引（从 0 开始）
        int col_span = 1;   // 跨越的列数（默认 1 列）
        int row_span = 1;   // 跨越的行数（默认 1 行）
    };


    // ============================================================
    //  图层 CSS 效果（LayerEffect）
    //
    //  通过 Layer 的链式方法设置，在 flush() 时由框架自动渲染，
    //  无需在绘画链中手动绘制背景/边框等重复代码。
    //
    //  所有效果依赖 resolved_bounds_（图层有效边界）：
    //    - 使用 LayoutPos 时，边界自动等于格子矩形
    //    - 否则须手动调用 layer_bounds() 指定
    //    - 未设置边界时，effects 静默跳过（不报错）
    //
    //  渲染顺序：背景 → 内容(裁切中) → 边框 → 模糊后处理
    // ============================================================
    struct LayerEffect
    {
        // ── 形状 ─────────────────────────────────────────────────
        float border_radius  = 0.f;    // 圆角半径（px，0 = 直角）
        bool  clip_to_bounds = false;  // 内容是否被裁切到 border_radius 内

        // ── 背景 ─────────────────────────────────────────────────
        std::optional<Color> background;  // 背景填充色（无值 = 透明）

        // ── 边框 ─────────────────────────────────────────────────
        float border_width = 0.f;                      // 边框宽度（0 = 无边框）
        Color border_color = Color::transparent();      // 边框颜色

        // ── 模糊 / 羽化（D2D 1.0 近似实现）──────────────────────
        float blur_radius = 0.f;  // 高斯模糊半径（px）
        float feather     = 0.f;  // 边缘羽化宽度（px，边界渐变透明）
        unsigned int shader_id = 0; // 后端 shader/program id；非 OpenGL 后端会跳过

        // ── 查询 ─────────────────────────────────────────────────
        bool has_background()    const noexcept { return background.has_value(); }
        bool has_border()        const noexcept { return border_width > 0.f && border_color.a > 0.f; }
        bool has_clip()          const noexcept { return clip_to_bounds; }
        bool has_blur()          const noexcept { return blur_radius > 0.f; }
        bool has_feather()       const noexcept { return feather > 0.f; }
        bool has_shader()        const noexcept { return shader_id != 0; }
        bool needs_bounds()      const noexcept
        {
            return has_background() || has_border() || has_clip()
                || has_blur() || has_feather() || has_shader();
        }
    };


    // ============================================================
    //  渲染上下文（RenderContext）
    //  绘画链执行时的运行环境，由后端（Backend）实现。
    //  此处为接口抽象，具体渲染能力在后续头文件中实现。
    // ============================================================
    class RenderContext
    {
    public:
        virtual ~RenderContext() = default;

        // ── 后端生命周期 ────────────────────────────────────────
        virtual void initialize_native(void* hwnd, unsigned int width, unsigned int height) = 0;
        virtual void resize(unsigned int width, unsigned int height) = 0;
        virtual void set_resize_clear_color(const Color& color) = 0;
        virtual void set_color_key_mode(bool enable) = 0;
        virtual void begin_frame(Color clear_color = Color{ 0.08f, 0.08f, 0.1f, 1.f }) = 0;
        virtual bool end_frame() = 0;

        // ── 变换栈 ──────────────────────────────────────────────
        // 压入/弹出变换矩阵，用于图层嵌套时的坐标变换
        virtual void push_transform(float tx, float ty,
                                    float sx = 1.f, float sy = 1.f,
                                    float rot = 0.f) = 0;
        virtual void pop_transform() = 0;

        // ── 样式栈 ──────────────────────────────────────────────
        // 压入/弹出图层样式（透明度、混合模式等）
        virtual void push_style(const LayerStyle& style) = 0;
        virtual void pop_style() = 0;

        // ── 图层 shader 后处理 ───────────────────────────────────
        // 默认实现为不支持：D2D 等后端直接返回 false，Layer 会按普通路径渲染。
        virtual bool supports_layer_shader() const { return false; }
        virtual unsigned int create_shader(std::string_view fragment_source,
                                           std::string_view vertex_source = {})
        {
            (void)fragment_source;
            (void)vertex_source;
            return 0;
        }
        virtual void destroy_shader(unsigned int shader_id)
        {
            (void)shader_id;
        }
        virtual bool begin_layer_shader(unsigned int shader_id, float width, float height)
        {
            (void)shader_id;
            (void)width;
            (void)height;
            return false;
        }
        virtual void end_layer_shader(unsigned int shader_id)
        {
            (void)shader_id;
        }

        // ── 路径操作（由 PaintOp 节点调用）─────────────────────
        virtual void cmd_move_to   (float x, float y) = 0;
        virtual void cmd_line_to   (float x, float y) = 0;
        virtual void cmd_bezier_to (float cx1, float cy1,
                                    float cx2, float cy2,
                                    float ex,  float ey)  = 0;
        virtual void cmd_arc       (float cx, float cy,
                                    float radius,
                                    float start_angle,
                                    float end_angle) = 0;
        virtual void cmd_close_path() = 0;

        // ── 绘制指令 ────────────────────────────────────────────
        virtual void cmd_fill   (const FillStyle&   style) = 0;
        virtual void cmd_stroke (const StrokeStyle& style) = 0;
        virtual void cmd_shadow (const ShadowStyle& style) = 0;

        // ── CSS 效果指令（LayerEffect 使用）─────────────────────

        // 填充圆角矩形（radius=0 时退化为普通矩形）
        virtual void cmd_fill_round_rect(float x, float y, float w, float h,
                                         float radius, const FillStyle& style) = 0;

        // 描边圆角矩形
        virtual void cmd_stroke_round_rect(float x, float y, float w, float h,
                                            float radius,
                                            const StrokeStyle& style) = 0;

        // 压入圆角裁切区域（后续绘制内容被裁切在此矩形内）
        // radius=0 时使用轴对齐矩形裁切（性能更好）
        virtual void cmd_push_round_clip(float x, float y, float w, float h,
                                          float radius) = 0;

        // 弹出最近一次压入的裁切区域（与 cmd_push_round_clip 配对）
        virtual void cmd_pop_clip() = 0;

        // 近似高斯模糊（D2D 1.0 多层半透明叠加，D2D 1.1 可用真正的 Effect）
        // 在指定矩形区域内对已绘制内容施加模糊
        virtual void cmd_blur_rect(float x, float y, float w, float h,
                                    float radius) = 0;

        // 羽化（边缘渐变透明，feather = 渐变宽度 px）
        virtual void cmd_feather_rect(float x, float y, float w, float h,
                                       float radius, float feather) = 0;

        // ── 文字指令 ─────────────────────────────────────────────

        // 绘制文本
        // content    : UTF-16 文本内容
        // x, y       : 文本框左上角坐标（DIP）
        // max_width  : 文本框最大宽度（0 = 不限制，单行）
        // max_height : 文本框最大高度（0 = 不限制）
        // style      : 字体、颜色、对齐等所有样式
        virtual void cmd_text(const std::wstring& content,
                               float x, float y,
                               float max_width, float max_height,
                               const TextStyle& style) = 0;

        // 测量文本尺寸（不进行任何绘制，仅返回排版结果）
        // 用于在绘制前预算文本框大小、实现自适应布局。
        // 返回 true = 测量成功；false = 字体不存在或系统错误
        virtual bool cmd_measure_text(const std::wstring& content,
                                       float max_width, float max_height,
                                       const TextStyle& style,
                                       TextMetrics& out) = 0;

        // 绘制图片（path=文件路径, x/y/w/h=目标矩形, opacity=不透明度[0-1], radius=圆角）
        virtual void cmd_draw_bitmap(const std::wstring& path,
                                      float x, float y, float w, float h,
                                      float opacity, float radius) = 0;

        // 绘制内存 BGRA 位图。pixels 为 top-down 32bpp BGRA，stride 为每行字节数。
        virtual void cmd_draw_bitmap_bgra(const uint8_t* pixels,
                                          int width, int height, int stride,
                                          float x, float y, float w, float h,
                                          float opacity, float radius)
        {
            (void)pixels; (void)width; (void)height; (void)stride;
            (void)x; (void)y; (void)w; (void)h; (void)opacity; (void)radius;
        }

        virtual void cmd_draw_bitmap_bgra_cached(uint64_t cache_key, uint64_t revision,
                                                 const uint8_t* pixels,
                                                 int width, int height, int stride,
                                                 const std::vector<BitmapUpdateRect>& update_rects,
                                                 float x, float y, float w, float h,
                                                 float opacity, float radius)
        {
            (void)cache_key; (void)revision; (void)update_rects;
            cmd_draw_bitmap_bgra(pixels, width, height, stride, x, y, w, h, opacity, radius);
        }
    };


    // ============================================================
    //  绘制操作节点基类（PaintOp）
    //
    //  绘画链由 PaintOp 节点组成的单链表构成：
    //
    //    [MoveToOp] → [LineToOp] → [StrokeOp] → nullptr
    //
    //  每个节点持有 next 指针（unique_ptr 管理生命周期）。
    //  执行时从 head 开始顺序调用 execute()。
    // ============================================================
    struct PaintOp
    {
        std::unique_ptr<PaintOp> next = nullptr; // 链表下一个节点

        virtual ~PaintOp() = default;

        // 每个节点实现自己的渲染指令
        virtual void execute(RenderContext& ctx) const = 0;
    };


    // ──────────────────────────────────────────────────────────────
    //  具体操作节点（内置类型）
    //  用户也可以继承 PaintOp 自定义节点，实现扩展绘制能力。
    // ──────────────────────────────────────────────────────────────

    // 移动画笔到指定坐标（不画线）
    struct MoveToOp final : PaintOp
    {
        float x, y;
        MoveToOp(float x, float y) : x(x), y(y) {}
        void execute(RenderContext& ctx) const override { ctx.cmd_move_to(x, y); }
    };

    // 从当前位置画直线到指定坐标
    struct LineToOp final : PaintOp
    {
        float x, y;
        LineToOp(float x, float y) : x(x), y(y) {}
        void execute(RenderContext& ctx) const override { ctx.cmd_line_to(x, y); }
    };

    // 三次贝塞尔曲线：两个控制点 + 终点
    struct BezierToOp final : PaintOp
    {
        float cx1, cy1, cx2, cy2, ex, ey;
        BezierToOp(float cx1, float cy1,
                   float cx2, float cy2,
                   float ex,  float ey)
            : cx1(cx1), cy1(cy1), cx2(cx2), cy2(cy2), ex(ex), ey(ey) {}
        void execute(RenderContext& ctx) const override
        { ctx.cmd_bezier_to(cx1, cy1, cx2, cy2, ex, ey); }
    };

    // 圆弧（顺时针，角度单位：弧度）
    struct ArcOp final : PaintOp
    {
        float cx, cy, radius, start_angle, end_angle;
        ArcOp(float cx, float cy, float r, float sa, float ea)
            : cx(cx), cy(cy), radius(r), start_angle(sa), end_angle(ea) {}
        void execute(RenderContext& ctx) const override
        { ctx.cmd_arc(cx, cy, radius, start_angle, end_angle); }
    };

    // 闭合当前路径（连回起点）
    struct ClosePathOp final : PaintOp
    {
        void execute(RenderContext& ctx) const override { ctx.cmd_close_path(); }
    };

    // 填充当前路径
    struct FillOp final : PaintOp
    {
        FillStyle style;
        explicit FillOp(FillStyle s) : style(std::move(s)) {}
        void execute(RenderContext& ctx) const override { ctx.cmd_fill(style); }
    };

    // 描边当前路径
    struct StrokeOp final : PaintOp
    {
        StrokeStyle style;
        explicit StrokeOp(StrokeStyle s) : style(std::move(s)) {}
        void execute(RenderContext& ctx) const override { ctx.cmd_stroke(style); }
    };

    // 为当前路径添加阴影（需在 fill/stroke 之前调用）
    struct ShadowOp final : PaintOp
    {
        ShadowStyle style;
        explicit ShadowOp(ShadowStyle s) : style(std::move(s)) {}
        void execute(RenderContext& ctx) const override { ctx.cmd_shadow(style); }
    };

    // ============================================================
    //  圆角矩形绘制节点
    // ============================================================

    // 填充圆角矩形
    struct FillRoundRectOp final : PaintOp
    {
        float x, y, w, h, radius;
        FillStyle style;
        FillRoundRectOp(float x, float y, float w, float h, float r, FillStyle s)
            : x(x), y(y), w(w), h(h), radius(r), style(std::move(s)) {}
        void execute(RenderContext& ctx) const override
        { ctx.cmd_fill_round_rect(x, y, w, h, radius, style); }
    };

    // 描边圆角矩形
    struct StrokeRoundRectOp final : PaintOp
    {
        float x, y, w, h, radius;
        StrokeStyle style;
        StrokeRoundRectOp(float x, float y, float w, float h, float r, StrokeStyle s)
            : x(x), y(y), w(w), h(h), radius(r), style(std::move(s)) {}
        void execute(RenderContext& ctx) const override
        { ctx.cmd_stroke_round_rect(x, y, w, h, radius, style); }
    };

    // 压入圆角裁切区域
    struct PushRoundClipOp final : PaintOp
    {
        float x, y, w, h, radius;
        PushRoundClipOp(float x, float y, float w, float h, float r)
            : x(x), y(y), w(w), h(h), radius(r) {}
        void execute(RenderContext& ctx) const override
        { ctx.cmd_push_round_clip(x, y, w, h, radius); }
    };

    // 弹出裁切区域
    struct PopClipOp final : PaintOp
    {
        void execute(RenderContext& ctx) const override { ctx.cmd_pop_clip(); }
    };


    // ============================================================
    //  图片绘制节点（DrawBitmapOp）
    //  通过 PaintChain::img() 创建；调用 RenderContext::cmd_draw_bitmap。
    // ============================================================
    struct DrawBitmapOp final : PaintOp
    {
        std::wstring path;
        float x, y, w, h, opacity, radius;

        DrawBitmapOp(std::wstring p, float x, float y, float w, float h,
                     float op, float r)
            : path(std::move(p)), x(x), y(y), w(w), h(h), opacity(op), radius(r) {}

        void execute(RenderContext& ctx) const override
        { ctx.cmd_draw_bitmap(path, x, y, w, h, opacity, radius); }
    };

    struct DrawBitmapBgraOp final : PaintOp
    {
        const uint8_t* pixels = nullptr;
        int width = 0, height = 0, stride = 0;
        float x = 0.f, y = 0.f, w = 0.f, h = 0.f, opacity = 1.f, radius = 0.f;

        DrawBitmapBgraOp(const uint8_t* pixels, int width, int height, int stride,
                         float x, float y, float w, float h, float opacity, float radius)
            : pixels(pixels), width(width), height(height), stride(stride),
              x(x), y(y), w(w), h(h), opacity(opacity), radius(radius)
        {}

        void execute(RenderContext& ctx) const override
        { ctx.cmd_draw_bitmap_bgra(pixels, width, height, stride, x, y, w, h, opacity, radius); }
    };


    // ============================================================
    //  文本绘制节点（TextOp）
    //
    //  文本内容使用 UTF-16（std::wstring）以直接对接 DirectWrite API。
    //  通过 PaintChain::text() 创建，按顺序渲染：
    //    阴影层 → 描边层 → 填充层
    //
    //  坐标 (x, y) 为文本框左上角。
    //  max_width / max_height = 0 表示不限制（单行自动宽度）。
    // ============================================================
    struct TextOp final : PaintOp
    {
        std::wstring content;    // 文本内容（UTF-16）
        float        x, y;       // 文本框左上角坐标（DIP）
        float        max_width;  // 文本框最大宽度（0 = 不限）
        float        max_height; // 文本框最大高度（0 = 不限）
        TextStyle    style;

        TextOp(std::wstring c, float x, float y,
               float mw, float mh, TextStyle s)
            : content(std::move(c)), x(x), y(y)
            , max_width(mw), max_height(mh)
            , style(std::move(s))
        {}

        void execute(RenderContext& ctx) const override
        {
            ctx.cmd_text(content, x, y, max_width, max_height, style);
        }
    };


    // ============================================================
    //  绘画链（PaintChain）
    //
    //  图层内部维护的操作链，提供流式（链式）API。
    //  内部使用单链表，head_ 是链首，tail_ 是链尾（O(1) 追加）。
    //
    //  使用方式：
    //    layer->paint()
    //          .move_to(0, 0)
    //          .line_to(100, 100)
    //          .stroke({.color=Color::red(), .width=2});
    //
    //  注意：PaintChain 不可拷贝，只可移动（链表所有权唯一）。
    // ============================================================
    class PaintChain
    {
    public:
        PaintChain()                             = default;
        PaintChain(PaintChain&&)                 = default;
        PaintChain& operator=(PaintChain&&)      = default;
        PaintChain(const PaintChain&)            = delete;
        PaintChain& operator=(const PaintChain&) = delete;

        // ── 路径操作 ─────────────────────────────────────────────
        PaintChain& move_to(float x, float y)
        { return append(std::make_unique<MoveToOp>(x, y)); }

        PaintChain& line_to(float x, float y)
        { return append(std::make_unique<LineToOp>(x, y)); }

        PaintChain& bezier_to(float cx1, float cy1,
                               float cx2, float cy2, float ex, float ey)
        { return append(std::make_unique<BezierToOp>(cx1, cy1, cx2, cy2, ex, ey)); }

        PaintChain& arc(float cx, float cy, float radius,
                        float start_angle, float end_angle)
        { return append(std::make_unique<ArcOp>(cx, cy, radius, start_angle, end_angle)); }

        PaintChain& close()
        { return append(std::make_unique<ClosePathOp>()); }

        // ── 绘制操作 ─────────────────────────────────────────────
        PaintChain& fill(Color color)
        { return append(std::make_unique<FillOp>(FillStyle{color})); }

        PaintChain& fill(FillStyle style)
        { return append(std::make_unique<FillOp>(std::move(style))); }

        // 渐变填充
        PaintChain& fill(LinearGradient gradient)
        { return append(std::make_unique<FillOp>(FillStyle(std::move(gradient)))); }

        PaintChain& fill(RadialGradient gradient)
        { return append(std::make_unique<FillOp>(FillStyle(std::move(gradient)))); }

        PaintChain& fill(ConicGradient gradient)
        { return append(std::make_unique<FillOp>(FillStyle(std::move(gradient)))); }

        PaintChain& stroke(Color color, float width = 1.f)
        { StrokeStyle s; s.color=color; s.width=width;
          return append(std::make_unique<StrokeOp>(s)); }

        PaintChain& stroke(StrokeStyle style)
        { return append(std::make_unique<StrokeOp>(std::move(style))); }

        PaintChain& shadow(ShadowStyle style)
        { return append(std::make_unique<ShadowOp>(std::move(style))); }

        // ── 圆角矩形绘制 ───────────────────────────────────────────
        PaintChain& fill_round_rect(float x, float y, float w, float h,
                                     float radius, Color color)
        { return append(std::make_unique<FillRoundRectOp>(x, y, w, h, radius, FillStyle(color))); }

        PaintChain& fill_round_rect(float x, float y, float w, float h,
                                     float radius, FillStyle style)
        { return append(std::make_unique<FillRoundRectOp>(x, y, w, h, radius, std::move(style))); }

        PaintChain& fill_round_rect(float x, float y, float w, float h,
                                     float radius, LinearGradient gradient)
        { return append(std::make_unique<FillRoundRectOp>(x, y, w, h, radius, FillStyle(std::move(gradient)))); }

        PaintChain& fill_round_rect(float x, float y, float w, float h,
                                     float radius, RadialGradient gradient)
        { return append(std::make_unique<FillRoundRectOp>(x, y, w, h, radius, FillStyle(std::move(gradient)))); }

        PaintChain& fill_round_rect(float x, float y, float w, float h,
                                     float radius, ConicGradient gradient)
        { return append(std::make_unique<FillRoundRectOp>(x, y, w, h, radius, FillStyle(std::move(gradient)))); }

        PaintChain& stroke_round_rect(float x, float y, float w, float h,
                                       float radius, StrokeStyle style)
        { return append(std::make_unique<StrokeRoundRectOp>(x, y, w, h, radius, std::move(style))); }

        PaintChain& stroke_round_rect(float x, float y, float w, float h,
                                       float radius, Color color, float width = 1.f)
        {
            StrokeStyle s;
            s.color = color;
            s.width = width;
            return append(std::make_unique<StrokeRoundRectOp>(x, y, w, h, radius, s));
        }

        // ── 裁切区域 ───────────────────────────────────────────────
        PaintChain& push_round_clip(float x, float y, float w, float h, float radius)
        { return append(std::make_unique<PushRoundClipOp>(x, y, w, h, radius)); }

        PaintChain& push_clip_rect(float x, float y, float w, float h)
        { return append(std::make_unique<PushRoundClipOp>(x, y, w, h, 0.f)); }

        PaintChain& pop_clip()
        { return append(std::make_unique<PopClipOp>()); }

        // ── 辅助绘制方法 ───────────────────────────────────────────────
        // 绘制线条
        PaintChain& stroke_line(float x1, float y1, float x2, float y2, float width, Color color)
        { return move_to(x1, y1).line_to(x2, y2).stroke(color, width); }

        // 绘制/填充矩形
        PaintChain& fill_rect(float x, float y, float w, float h, Color color)
        { return move_to(x, y).line_to(x + w, y).line_to(x + w, y + h).line_to(x, y + h).close().fill(color); }

        PaintChain& stroke_rect(float x, float y, float w, float h, float width, Color color)
        { return move_to(x, y).line_to(x + w, y).line_to(x + w, y + h).line_to(x, y + h).close().stroke(color, width); }

        // 绘制/填充圆形
        PaintChain& fill_circle(float cx, float cy, float radius, Color color)
        { return arc(cx, cy, radius, 0.f, 6.28318f).close().fill(color); }

        PaintChain& fill_circle(float cx, float cy, float radius, FillStyle style)
        { return arc(cx, cy, radius, 0.f, 6.28318f).close().fill(std::move(style)); }

        PaintChain& stroke_circle(float cx, float cy, float radius, float width, Color color)
        { return arc(cx, cy, radius, 0.f, 6.28318f).close().stroke(color, width); }

        // ── 文字绘制 ─────────────────────────────────────────────

        // 宽字符（UTF-16），单行
        PaintChain& text(std::wstring_view content, float x, float y,
                          const TextStyle& style = {})
        { return append(std::make_unique<TextOp>(
              std::wstring(content), x, y, 0.f, 0.f, style)); }

        // 宽字符，文本框（含换行）
        PaintChain& text(std::wstring_view content,
                          float x, float y, float max_w, float max_h,
                          const TextStyle& style = {})
        { return append(std::make_unique<TextOp>(
              std::wstring(content), x, y, max_w, max_h, style)); }

        // UTF-8，单行
        PaintChain& text(std::string_view content_utf8, float x, float y,
                          const TextStyle& style = {})
        { return append(std::make_unique<TextOp>(
              s_utf8to16(content_utf8), x, y, 0.f, 0.f, style)); }

        // UTF-8，文本框
        PaintChain& text(std::string_view content_utf8,
                          float x, float y, float max_w, float max_h,
                          const TextStyle& style = {})
        { return append(std::make_unique<TextOp>(
              s_utf8to16(content_utf8), x, y, max_w, max_h, style)); }

        // ── 图片绘制 ─────────────────────────────────────────────
        // path=文件路径, x/y/w/h=目标矩形（像素）,
        // opacity=不透明度[0-1], radius=圆角半径（0=直角）
        PaintChain& img(const std::wstring& path,
                        float x, float y, float w, float h,
                        float opacity = 1.f, float radius = 0.f)
        { return append(std::make_unique<DrawBitmapOp>(path, x, y, w, h, opacity, radius)); }

        PaintChain& bitmap_bgra(const uint8_t* pixels, int width, int height, int stride,
                                float x, float y, float w, float h,
                                float opacity = 1.f, float radius = 0.f)
        { return append(std::make_unique<DrawBitmapBgraOp>(pixels, width, height, stride, x, y, w, h, opacity, radius)); }

        // ── 自定义节点 ───────────────────────────────────────────
        PaintChain& custom(std::unique_ptr<PaintOp> op)
        { return append(std::move(op)); }

        // ── 执行与管理（public，供 Layer::flush / tick 调用）─────
        void execute(RenderContext& ctx) const
        {
            for (const PaintOp* op = head_.get(); op; op = op->next.get())
                op->execute(ctx);
        }

        void clear()
        {
            head_.reset();
            tail_ = nullptr;
        }

        bool empty() const { return head_ == nullptr; }

        // ── 内存池支持（可选优化）────────────────────────────────
        // 启用后，PaintOp 节点从预分配池中获取，减少堆分配
        // 适用于高频动态绘制场景
        static void enable_pool(bool enable, size_t pool_size = 1024);
        static void* pool_alloc(size_t size);
        static void pool_dealloc(void* ptr, size_t size);

    private:
        std::unique_ptr<PaintOp> head_ = nullptr;
        PaintOp*                 tail_ = nullptr;

        // UTF-8 → UTF-16（仅在 Windows 上使用系统 API，其他平台 ASCII 直通）
        static std::wstring s_utf8to16(std::string_view s)
        {
            if (s.empty()) return {};
#ifdef _WIN32
            // 前向声明 WinAPI，避免 #include <windows.h>
            extern int __stdcall MultiByteToWideChar(
                unsigned int, unsigned long, const char*, int, wchar_t*, int);
            int n = MultiByteToWideChar(65001u, 0ul,
                                         s.data(), (int)s.size(), nullptr, 0);
            if (n <= 0) return {};
            std::wstring out(n, L'\0');
            MultiByteToWideChar(65001u, 0ul, s.data(), (int)s.size(),
                                  out.data(), n);
            return out;
#else
            return std::wstring(s.begin(), s.end());
#endif
        }

        PaintChain& append(std::unique_ptr<PaintOp> op)
        {
            PaintOp* raw = op.get();
            if (!tail_) head_ = std::move(op);
            else        tail_->next = std::move(op);
            tail_ = raw;
            return *this;
        }
    };


    // ============================================================
    //  图层（Layer）
    //
    //  框架的核心对象，具有以下职责：
    //    1. 持有一条 PaintChain（静态绘制内容）
    //    2. 持有事件回调列表（动态绘制 / 逻辑更新 / 鼠标交互）
    //    3. 管理子图层（形成图层树）
    //    4. 每帧由 Window 驱动 tick() + flush()
    //    5. 处理鼠标命中测试与事件分发（dispatch_mouse_*）
    //
    //  鼠标事件分发顺序（类似 HTML 捕获/冒泡）：
    //    子层优先（视觉上层的子层先收到事件），
    //    任意回调返回 true 则停止继续传播。
    //
    //  生命周期：Layer 由 Window 通过 unique_ptr 独占管理，
    //  外部持有的是 Layer*（裸指针）或 LayerRef（代理句柄），
    //  不应自行 delete。
    // ============================================================
    class Layer
    {
    public:
        // ── 回调类型定义 ─────────────────────────────────────────

        // 逻辑更新回调：dt 为帧间隔时间（秒）
        // 返回 true 继续触发，返回 false 自动注销
        using UpdateCallback = std::function<bool(float dt)>;

        // 渲染回调：可在回调内操作绘画链，实现每帧动态绘制
        // 返回 true 继续触发，返回 false 自动注销
        using RenderCallback = std::function<bool(PaintChain& chain, float dt)>;

        // ── 构造 ─────────────────────────────────────────────────

        // name：图层名称（在兄弟层中唯一）
        // is_canvas：是否为画布根层（由 Window 创建时标记）
        explicit Layer(std::string name, bool is_canvas = false)
            : name_(std::move(name))
            , is_canvas_(is_canvas)
        {}

        ~Layer()
        {
            if (native_component_destroy_)
                native_component_destroy_(native_component_);
        }

        // 不可拷贝（图层树结构唯一）
        Layer(const Layer&)            = delete;
        Layer& operator=(const Layer&) = delete;

        // ── 图层属性 ─────────────────────────────────────────────

        // 图层名称（只读）
        const std::string& name()      const { return name_; }

        // 是否为画布根层
        bool               is_canvas() const { return is_canvas_; }

        template <typename T>
        T* native_component() const noexcept
        {
            return static_cast<T*>(native_component_);
        }

        Layer& set_native_component(void* component, void(*destroy)(void*) = nullptr)
        {
            if (native_component_destroy_ && native_component_ && native_component_ != component)
                native_component_destroy_(native_component_);
            native_component_ = component;
            native_component_destroy_ = destroy;
            return *this;
        }

        // 设置不透明度 [0.0, 1.0]，返回自身支持链式
        Layer& opacity(float value)
        {
            style_.opacity = std::clamp(value, 0.f, 1.f);
            return *this;
        }

        // 设置混合模式
        Layer& blend(BlendMode mode)
        {
            style_.blend = mode;
            return *this;
        }

        // 设置可见性（不可见的图层同时也不响应鼠标事件）
        Layer& visible(bool v)
        {
            style_.visible = v;
            return *this;
        }

        // 读取当前样式（只读）
        const LayerStyle& style() const { return style_; }

        // ── 鼠标命中区域 ─────────────────────────────────────────

        // 设置图层的鼠标响应矩形区域（像素，相对于窗口客户区）
        // 调用后 hit_area_mode = Manual，覆盖 auto_hit_area()（若之前有设置）
        Layer& hit_area(float x, float y, float w, float h)
        {
            hit_area_      = Rect{ x, y, w, h };
            hit_area_mode_ = HitAreaMode::Manual;
            return *this;
        }

        using HitTestCallback = std::function<bool(float local_x, float local_y)>;

        Layer& hit_test(HitTestCallback cb)
        {
            hit_test_cb_ = std::move(cb);
            return *this;
        }

        Layer& clear_hit_test()
        {
            hit_test_cb_ = {};
            return *this;
        }

        // 清除命中区域限制（恢复全窗口响应）
        Layer& clear_hit_area()
        {
            hit_area_.reset();
            hit_area_mode_ = HitAreaMode::None;
            return *this;
        }

        // 查询鼠标当前是否悬浮在本图层上
        bool is_hovered() const { return is_hovered_; }

        // ── 绘画链访问 ───────────────────────────────────────────

        // 访问静态绘画链（适合一次性固定内容）
        PaintChain& paint() { return static_chain_; }

        // ── 帧事件回调注册 ───────────────────────────────────────

        // 注册逻辑更新回调（不涉及绘制，只更新数据）
        Layer& on_update(UpdateCallback cb)
        {
            update_callbacks_.emplace_back(std::move(cb));
            return *this;
        }

        // 注册渲染回调（每帧清空绘画链后调用，适合动态内容）
        Layer& on_render(RenderCallback cb)
        {
            render_callbacks_.emplace_back(std::move(cb));
            return *this;
        }

        // ── 鼠标事件回调注册 ─────────────────────────────────────
        // 所有鼠标回调：返回 true = 消费事件（阻止传播），false = 继续传播

        // 鼠标左键点击（按下后抬起，未移动）
        Layer& on_click(MouseCallback cb)
        {
            mouse_click_.emplace_back(std::move(cb));
            return *this;
        }

        // 鼠标右键点击
        Layer& on_right_click(MouseCallback cb)
        {
            mouse_right_click_.emplace_back(std::move(cb));
            return *this;
        }

        // 左键双击（需窗口注册 CS_DBLCLKS 样式，Window 已处理）
        Layer& on_double_click(MouseCallback cb)
        {
            mouse_double_click_.emplace_back(std::move(cb));
            return *this;
        }

        // 任意鼠标键按下（left/right/middle 通过 MouseEvent 区分）
        Layer& on_mouse_down(MouseCallback cb)
        {
            mouse_down_.emplace_back(std::move(cb));
            return *this;
        }

        // 任意鼠标键抬起
        Layer& on_mouse_up(MouseCallback cb)
        {
            mouse_up_.emplace_back(std::move(cb));
            return *this;
        }

        // 鼠标在窗口内移动（坐标通过 MouseEvent.x/y 获取）
        Layer& on_mouse_move(MouseCallback cb)
        {
            mouse_move_.emplace_back(std::move(cb));
            return *this;
        }

        // 鼠标首次进入命中区域（从外部移入时触发一次）
        Layer& on_mouse_enter(MouseCallback cb)
        {
            mouse_enter_.emplace_back(std::move(cb));
            return *this;
        }

        // 鼠标离开命中区域（移出时触发一次）
        Layer& on_mouse_leave(MouseCallback cb)
        {
            mouse_leave_.emplace_back(std::move(cb));
            return *this;
        }

        // 鼠标悬浮期间持续触发（鼠标在命中区域内移动时每次移动都触发）
        Layer& on_mouse_hover(MouseCallback cb)
        {
            mouse_hover_.emplace_back(std::move(cb));
            return *this;
        }

        // 鼠标滚轮滚动（wheel_delta > 0 = 向上，< 0 = 向下）
        Layer& on_wheel(MouseCallback cb)
        {
            mouse_wheel_.emplace_back(std::move(cb));
            return *this;
        }

        // ── 文件拖放回调 ───────────────────────────────────────────
        using DropCallback = std::function<bool(const std::vector<std::wstring>& files, const MouseEvent& e)>;

        Layer& on_drop_files(DropCallback cb)
        {
            drop_files_.emplace_back(std::move(cb));
            return *this;
        }

        // 清空指定类型的所有鼠标回调（用于动态移除监听）
        enum class MouseEventType : uint8_t
        {
            Click, RightClick, DoubleClick,
            Down, Up, Move,
            Enter, Leave, Hover,
            Wheel
        };

        Layer& remove_mouse_callbacks(MouseEventType type)
        {
            switch (type)
            {
            case MouseEventType::Click:       mouse_click_.clear();        break;
            case MouseEventType::RightClick:  mouse_right_click_.clear();  break;
            case MouseEventType::DoubleClick: mouse_double_click_.clear(); break;
            case MouseEventType::Down:        mouse_down_.clear();         break;
            case MouseEventType::Up:          mouse_up_.clear();           break;
            case MouseEventType::Move:        mouse_move_.clear();         break;
            case MouseEventType::Enter:       mouse_enter_.clear();        break;
            case MouseEventType::Leave:       mouse_leave_.clear();        break;
            case MouseEventType::Hover:       mouse_hover_.clear();        break;
            case MouseEventType::Wheel:       mouse_wheel_.clear();        break;
            }
            return *this;
        }

        // ── 变换（显式坐标，与 LayoutPos 二选一，最后调用的生效）──

        // 设置图层的平移（像素）
        // 调用后 pos_source = Manual，覆盖 LayoutPos（若之前有设置）
        Layer& translate(float tx, float ty)
        {
            transform_.tx = tx;
            transform_.ty = ty;
            pos_source_   = PosSource::Manual;
            return *this;
        }

        // 设置图层的缩放（独立于平移/布局，不影响 pos_source）
        Layer& scale(float sx, float sy = 0.f)
        {
            transform_.sx = sx;
            transform_.sy = (sy == 0.f) ? sx : sy;  // sy=0 时等比缩放
            return *this;
        }

        // 设置图层的旋转（弧度，独立于平移/布局）
        Layer& rotate(float radians)
        {
            transform_.rot = radians;
            return *this;
        }

        // ── 动画支持（内部方法）────────────────────────────────────

        // 供 AnimManager 调用的 setter
        void set_anim_translate_x(float x) { transform_.tx = x; pos_source_ = PosSource::Manual; }
        void set_anim_translate_y(float y) { transform_.ty = y; pos_source_ = PosSource::Manual; }
        void set_anim_scale(float s) { transform_.sx = transform_.sy = s; }

        // ── 动画便捷方法 ───────────────────────────────────────────

        // 平移动画
        Layer& animate_translate(float to_x, float to_y, float duration,
                                 Easing easing = Easing::Linear,
                                 std::function<void()> on_complete = {})
        {
            if (anim_manager_)
            {
                std::vector<AnimTrack> tracks;
                tracks.emplace_back(AnimTrack::TranslateX, transform_.tx, to_x, duration, easing);
                tracks.emplace_back(AnimTrack::TranslateY, transform_.ty, to_y, duration, easing);
                anim_manager_->create(this, std::move(tracks), std::move(on_complete));
            }
            return *this;
        }

        // X方向平移动画
        Layer& animate_translate_x(float to_x, float duration,
                                   Easing easing = Easing::Linear,
                                   std::function<void()> on_complete = {})
        {
            if (anim_manager_)
                anim_manager_->create(this, { AnimTrack(AnimTrack::TranslateX, transform_.tx, to_x, duration, easing) }, std::move(on_complete));
            return *this;
        }

        // Y方向平移动画
        Layer& animate_translate_y(float to_y, float duration,
                                   Easing easing = Easing::Linear,
                                   std::function<void()> on_complete = {})
        {
            if (anim_manager_)
                anim_manager_->create(this, { AnimTrack(AnimTrack::TranslateY, transform_.ty, to_y, duration, easing) }, std::move(on_complete));
            return *this;
        }

        // 缩放动画
        Layer& animate_scale(float to_scale, float duration,
                             Easing easing = Easing::Linear,
                             std::function<void()> on_complete = {})
        {
            if (anim_manager_)
                anim_manager_->create(this, { AnimTrack(AnimTrack::Scale, transform_.sx, to_scale, duration, easing) }, std::move(on_complete));
            return *this;
        }

        // 透明度动画
        Layer& animate_opacity(float to_opacity, float duration,
                               Easing easing = Easing::Linear,
                               std::function<void()> on_complete = {})
        {
            if (anim_manager_)
                anim_manager_->create(this, { AnimTrack(AnimTrack::Opacity, style_.opacity, to_opacity, duration, easing) }, std::move(on_complete));
            return *this;
        }

        // 旋转动画
        Layer& animate_rotate(float to_radians, float duration,
                              Easing easing = Easing::Linear,
                              std::function<void()> on_complete = {})
        {
            if (anim_manager_)
                anim_manager_->create(this, { AnimTrack(AnimTrack::Rotate, transform_.rot, to_radians, duration, easing) }, std::move(on_complete));
            return *this;
        }

        // 组合动画
        Layer& animate(std::vector<AnimTrack> tracks, std::function<void()> on_complete = {})
        {
            if (anim_manager_)
                anim_manager_->create(this, std::move(tracks), std::move(on_complete));
            return *this;
        }

        // 设置动画管理器（由 otterwindow 调用）
        void set_anim_manager(AnimManager* mgr) { anim_manager_ = mgr; }

        // 取消所有动画
        void cancel_animations()
        {
            if (anim_manager_) anim_manager_->cancel_all(this);
        }

        // ── 布局容器（设置在父层上，为子层提供网格参考）──────────

        // 将本层设置为网格布局容器
        // cols, rows  : 列数、行数
        // ref_w, ref_h: 布局参考尺寸（像素，0 = 使用窗口尺寸，由 otterwindow 填入）
        // origin_x/y  : 网格起始坐标（像素）
        // pad_x/y     : 内边距（像素）
        // gap_x/y     : 间距（像素）
        Layer& Layout(int   cols,     int   rows,
                      float ref_w    = 0.f, float ref_h    = 0.f,
                      float origin_x = 0.f, float origin_y = 0.f,
                      float pad_x    = 0.f, float pad_y    = 0.f,
                      float gap_x    = 0.f, float gap_y    = 0.f)
        {
            LayoutConfig cfg;
            cfg.cols     = cols;
            cfg.rows     = rows;
            cfg.ref_w    = ref_w;
            cfg.ref_h    = ref_h;
            cfg.origin_x = origin_x;
            cfg.origin_y = origin_y;
            cfg.pad_x    = pad_x;
            cfg.pad_y    = pad_y;
            cfg.gap_x    = gap_x;
            cfg.gap_y    = gap_y;
            layout_config_ = cfg;
            return *this;
        }

        // 内部：由 otterwindow::Layout() 调用，填入窗口尺寸后整体更新
        void set_layout_config(const LayoutConfig& cfg)
        {
            layout_config_ = cfg;
        }

        // 获取本层的布局配置（子层 flush 时需要）
        const LayoutConfig* get_layout_config() const noexcept
        {
            return layout_config_.has_value() ? &(*layout_config_) : nullptr;
        }

        // ── 布局位置（设置在子层上，按父层网格定位）────────────────

        // 设置本层在父层网格中的位置
        // 调用后 pos_source = Layout，覆盖 translate()（若之前有设置）
        // col, row     : 网格坐标（0开始）
        // col_span, row_span : 跨越的格数（默认 1）
        Layer& LayoutPos(int col, int row, int col_span = 1, int row_span = 1)
        {
            layout_pos_ = LayoutPosition{ col, row, col_span, row_span };
            pos_source_ = PosSource::Layout;
            return *this;
        }

        // 是否使用布局自动命中区域
        // true  = 命中区域自动等于 LayoutPos 对应的网格单元格矩形
        //         （在 flush 时计算，dispatch 时使用，无需手动设置 hit_area）
        // false = 使用手动 hit_area()，或无限制（默认）
        // 注意：调用 hit_area() 会隐式关闭此选项（hit_area_mode 切换）
        Layer& auto_hit_area(bool enable = true)
        {
            hit_area_mode_ = enable ? HitAreaMode::Auto : HitAreaMode::None;
            return *this;
        }

        // 查询本层当前布局配置（用于外部工具/调试）
        std::optional<LayoutConfig>  current_layout()   const { return layout_config_; }
        std::optional<LayoutPosition> current_pos()     const { return layout_pos_; }
        LayerTransform                current_transform() const { return transform_; }

        // ── 图层边界（CSS 效果的参考矩形）────────────────────────

        // 手动指定图层的可视边界（背景/边框/裁切的参考矩形）。
        // 使用 LayoutPos 时，边界会自动从网格格子推算，无需手动设置。
        Layer& layer_bounds(float x, float y, float w, float h)
        {
            bounds_ = Rect{ x, y, w, h };
            resolved_bounds_ = bounds_;  // 同步，使 dispatch 可立即使用
            return *this;
        }

        Layer& clear_bounds()
        {
            bounds_.reset();
            return *this;
        }

        const Rect* resolved_bounds() const noexcept
        {
            return resolved_bounds_.has_value() ? &(*resolved_bounds_) : nullptr;
        }

        // 本帧在窗口客户区中的世界坐标（flush 之后有效）
        float world_x() const noexcept { return world_tx_; }
        float world_y() const noexcept { return world_ty_; }

        // ── 滚动（Scroll View）───────────────────────────────────

        // 启用垂直滚动（最简接口：仅指定内容总高度，其余使用默认样式）
        Layer& enable_scroll(float content_height,
                              float content_width = 0.f)
        {
            ScrollConfig sc;
            sc.content_height = content_height;
            sc.content_width  = content_width;
            scroll_cfg_       = sc;
            return *this;
        }

        // 启用滚动（完整配置版）
        Layer& enable_scroll(ScrollConfig cfg)
        {
            scroll_cfg_ = std::move(cfg);
            return *this;
        }

        // 禁用滚动
        Layer& disable_scroll()
        {
            scroll_cfg_.reset();
            return *this;
        }

        // 跳转到指定滚动位置（像素）
        Layer& scroll_to(float x, float y)
        {
            scroll_x_ = x;
            scroll_y_ = y;
            return *this;
        }

        // 相对滚动
        Layer& scroll_by(float dx, float dy)
        {
            scroll_x_ += dx;
            scroll_y_ += dy;
            return *this;
        }

        // 读取当前滚动量
        float scroll_offset_x() const noexcept { return scroll_x_; }
        float scroll_offset_y() const noexcept { return scroll_y_; }

        // 读取/修改滚动配置（运行时动态调整内容高度等）
        ScrollConfig*       scroll_config()       { return scroll_cfg_.has_value() ? &*scroll_cfg_ : nullptr; }
        const ScrollConfig* scroll_config() const { return scroll_cfg_.has_value() ? &*scroll_cfg_ : nullptr; }

        // ── CSS 风格效果（均返回 *this，支持链式调用）───────────

        // 设置背景填充色（渲染在内容之前，受 border_radius 影响）
        Layer& background(Color c)     { effect_.background = c;                         return *this; }
        Layer& no_background()         { effect_.background.reset();                      return *this; }

        // 圆角半径（px），影响背景/边框/clip_content 的裁切形状
        Layer& border_radius(float r)  { effect_.border_radius = (r<0?0:r);               return *this; }

        // 边框（渲染在内容之后，覆盖在内容上方）
        Layer& border(float w, Color c){ effect_.border_width=(w<0?0:w); effect_.border_color=c; return *this; }
        Layer& no_border()             { effect_.border_width = 0.f;                      return *this; }

        // 内容裁切（类似 CSS overflow:hidden + border-radius）
        Layer& clip_content(bool e=true){ effect_.clip_to_bounds = e;                     return *this; }

        // 高斯模糊（D2D 1.0 近似，radius=px）
        Layer& blur(float r)           { effect_.blur_radius = (r<0?0:r);                 return *this; }

        // 边缘羽化（边界渐变透明，width=px）
        Layer& feather(float w)        { effect_.feather = (w<0?0:w);                     return *this; }

        // OpenGL 图层后处理 shader。Direct2D 后端会自动跳过，不报错。
        Layer& shader(unsigned int shader_id) { effect_.shader_id = shader_id;             return *this; }
        Layer& no_shader()             { effect_.shader_id = 0;                            return *this; }

        // 移除所有 CSS 效果
        Layer& clear_effects()         { effect_ = LayerEffect{};                         return *this; }

        // 读取当前效果配置（只读）
        const LayerEffect& effect() const noexcept { return effect_; }

        // 背景图片便捷接口（渲染时图片铺满 layer_bounds 区域）
        // path=文件路径, opacity=不透明度[0-1], radius=圆角（与 border_radius 保持一致）
        Layer& bg_img(const std::wstring& path, float opacity = 1.f, float radius = 0.f)
        {
            // 记录参数，在 on_render 里通过 img() 绘制
            bg_img_path_    = path;
            bg_img_opacity_ = opacity;
            bg_img_radius_  = radius;
            // 注册渲染回调（幂等：每次调用都更新参数，回调只注册一次由标志控制）
            if (!bg_img_registered_)
            {
                bg_img_registered_ = true;
                on_render([this](PaintChain& ch, float) -> bool {
                    if (bg_img_path_.empty()) return true;
                    float w = 0.f, h = 0.f;
                    if (resolved_bounds_.has_value())
                    { w = resolved_bounds_->width; h = resolved_bounds_->height; }
                    else if (bounds_.has_value())
                    { w = bounds_->width; h = bounds_->height; }
                    if (w > 0.f && h > 0.f)
                        ch.img(bg_img_path_, 0, 0, w, h, bg_img_opacity_, bg_img_radius_);
                    return true;
                });
            }
            return *this;
        }

        // ── 子图层管理 ───────────────────────────────────────────

        // 创建一个子图层并返回其裸指针
        // 若同名子层已存在则直接返回已有层（幂等）
        Layer* creat(std::string_view child_name)
        {
            // 使用 heterogeneous lookup，直接用 string_view 查找
            auto it = name_map_.find(child_name);
            if (it != name_map_.end())
                return it->second;

            // 只有在需要创建新图层时才创建 string
            auto   child   = std::make_unique<Layer>(std::string(child_name));
            Layer* raw_ptr = child.get();

            // 继承父层的动画管理器
            if (anim_manager_)
                raw_ptr->set_anim_manager(anim_manager_);

            raw_ptr->parent_ = this;  // 记录父层

            name_map_.emplace(child->name_, raw_ptr);
            children_.emplace_back(std::move(child));

            return raw_ptr;
        }

        // 将指定直接子层移动到 children_ 末尾（渲染时在最上面）
        // 若该层不是本层的直接子层则忽略
        void bring_child_to_front(Layer* child)
        {
            auto it = std::find_if(children_.begin(), children_.end(),
                [child](const std::unique_ptr<Layer>& p){ return p.get() == child; });
            if (it == children_.end() || it == children_.end() - 1) return;
            std::rotate(it, it + 1, children_.end());
        }

        // 将本层在其父层中移到最前（渲染时在最上面）
        // 如果本层是根层（无父层）则忽略
        Layer& bring_to_front()
        {
            if (parent_) parent_->bring_child_to_front(this);
            return *this;
        }

        // 将本层在其父层中移到最后（渲染时在最下面）
        Layer& send_to_back()
        {
            if (!parent_) return *this;
            auto& ch = parent_->children_;
            auto it = std::find_if(ch.begin(), ch.end(),
                [this](const std::unique_ptr<Layer>& p){ return p.get() == this; });
            if (it == ch.end() || it == ch.begin()) return *this;
            std::rotate(ch.begin(), it, it + 1);
            return *this;
        }

        // 按名称查找直接子图层（不递归），未找到返回 nullptr
        Layer* get_child(std::string_view child_name) const
        {
            // 使用 heterogeneous lookup，直接用 string_view 查找，无需创建临时 string
            auto it = name_map_.find(child_name);
            return (it != name_map_.end()) ? it->second : nullptr;
        }

        // 按名称递归查找整个子树，未找到返回 nullptr
        Layer* find(std::string_view target_name) const
        {
            if (name_ == target_name) return const_cast<Layer*>(this);
            if (auto* p = get_child(target_name)) return p;
            for (const auto& child : children_)
                if (auto* p = child->find(target_name)) return p;
            return nullptr;
        }

        // 递归计算所有后代图层的边界（用于自动滚动检测）
        struct ContentBounds
        {
            float min_x = 0, min_y = 0, max_x = 0, max_y = 0;
        };

        ContentBounds content_bounds() const
        {
            ContentBounds b;
            s_collect_bounds(this, 0.f, 0.f, b);
            return b;
        }

        // ── 帧驱动（由 Window/Canvas 调用）─────────────────────

        // tick：每帧逻辑步进
        void tick(float dt)
        {
            // 采用快照遍历，避免回调在执行期间增删回调或子层导致容器失效
            const auto update_snapshot = update_callbacks_;
            const auto render_snapshot = render_callbacks_;

            // 只有当有回调返回 false 时才需要修改原列表
            size_t write_idx = 0;
            for (const auto& cb : update_snapshot)
            {
                if (cb && cb(dt))
                {
                    if (write_idx < update_callbacks_.size())
                        update_callbacks_[write_idx] = cb;
                    ++write_idx;
                }
            }
            update_callbacks_.resize(write_idx);

            dynamic_chain_.clear();
            write_idx = 0;
            for (const auto& cb : render_snapshot)
            {
                if (cb && cb(dynamic_chain_, dt))
                {
                    if (write_idx < render_callbacks_.size())
                        render_callbacks_[write_idx] = cb;
                    ++write_idx;
                }
            }
            render_callbacks_.resize(write_idx);

            std::vector<Layer*> child_snapshot;
            child_snapshot.reserve(children_.size());
            for (auto& child : children_)
                if (child) child_snapshot.push_back(child.get());

            for (auto* child : child_snapshot)
                child->tick(dt);
        }

        // flush：将本图层（及子层）的绘画内容提交到渲染上下文
        //
        // parent_layout : 父层的网格配置（nullptr = 父层无网格）
        // parent_wtx/wty : 父层的世界坐标偏移（用于正确计算子层的 world_tx_/world_ty_）
        void flush(RenderContext& ctx, const LayoutConfig* parent_layout = nullptr,
                   float parent_wtx = 0.f, float parent_wty = 0.f) const
        {
            if (!style_.visible) return;

            // ── 1. 解析有效变换 ───────────────────────────────────
            float eff_tx = 0.f, eff_ty = 0.f;
            float eff_sx = transform_.sx;
            float eff_sy = transform_.sy;
            float eff_rot = transform_.rot;
            bool  has_pos = false;

            if (pos_source_ == PosSource::Layout
                && parent_layout != nullptr
                && parent_layout->is_valid()
                && layout_pos_.has_value())
            {
                eff_tx  = parent_layout->pos_x(layout_pos_->col);
                eff_ty  = parent_layout->pos_y(layout_pos_->row);
                has_pos = true;
            }
            else if (pos_source_ == PosSource::Manual)
            {
                eff_tx  = transform_.tx;
                eff_ty  = transform_.ty;
                has_pos = true;
            }

            // 累加父层的世界偏移
            world_tx_ = eff_tx + parent_wtx;
            world_ty_ = eff_ty + parent_wty;

            // ── 2. 解析有效边界（背景/边框/裁切 使用）───────────
            if (parent_layout != nullptr
                && parent_layout->is_valid()
                && layout_pos_.has_value())
            {
                resolved_bounds_ = Rect{
                    parent_layout->pos_x(layout_pos_->col),
                    parent_layout->pos_y(layout_pos_->row),
                    parent_layout->span_w(layout_pos_->col_span),
                    parent_layout->span_h(layout_pos_->row_span)
                };
            }
            else if (bounds_.has_value())
            {
                resolved_bounds_ = bounds_;
            }
            else
            {
                resolved_bounds_.reset();
            }

            // ── 3. 自动命中区域（同步 resolved_bounds_）──────────
            if (hit_area_mode_ == HitAreaMode::Auto)
                resolved_hit_area_ = resolved_bounds_;
            else
                resolved_hit_area_.reset();

            // ── 4. 推入变换 ───────────────────────────────────────
            bool needs_transform = has_pos
                || eff_sx != 1.f || eff_sy != 1.f || eff_rot != 0.f;

            ctx.push_style(style_);
            if (needs_transform)
                ctx.push_transform(eff_tx, eff_ty, eff_sx, eff_sy, eff_rot);

            // ── 5. 应用 CSS 效果（需要 resolved_bounds_）─────────
            const bool has_fx    = effect_.needs_bounds();
            const bool has_bounds = resolved_bounds_.has_value();
            const bool shader_active = has_fx && has_bounds
                && effect_.has_shader()
                && ctx.begin_layer_shader(effect_.shader_id,
                                          resolved_bounds_->width,
                                          resolved_bounds_->height);

            if (has_fx && has_bounds)
            {
                const Rect& rb = *resolved_bounds_;
                const float rr = effect_.border_radius;

                // 5a. 模糊后处理（先于内容，近似实现）—— 局部坐标
                if (effect_.has_blur())
                    ctx.cmd_blur_rect(0, 0, rb.width, rb.height, effect_.blur_radius);

                // 5b. 压入圆角裁切（内容在裁切范围内渲染）—— 局部坐标
                if (effect_.has_clip())
                    ctx.cmd_push_round_clip(0, 0, rb.width, rb.height, rr);

                // 5c. 背景（在裁切内，位于内容之下）—— 局部坐标
                if (effect_.has_background())
                    ctx.cmd_fill_round_rect(0, 0, rb.width, rb.height,
                                            rr, *effect_.background);
            }

            // ── 6. 执行绘画链 + 子层（含滚动支持）───────────────
            const LayoutConfig* child_layout = layout_config_.has_value()
                                               ? &(*layout_config_) : nullptr;

            if (scroll_cfg_.has_value() && resolved_bounds_.has_value())
            {
                // ── 滚动视口模式 ──────────────────────────────────
                flush_scroll_view(ctx, *resolved_bounds_, child_layout, world_tx_, world_ty_);
            }
            else
            {
                // ── 普通模式 ──────────────────────────────────────
                static_chain_.execute(ctx);
                dynamic_chain_.execute(ctx);
                for (const auto& child : children_)
                    if (child) child->flush(ctx, child_layout, world_tx_, world_ty_);
            }

            if (shader_active)
                ctx.end_layer_shader(effect_.shader_id);

            // ── 7. 弹出裁切 + 绘制边框（在内容上方）—— 局部坐标 ──
            if (has_fx && has_bounds)
            {
                const Rect& rb = *resolved_bounds_;
                const float rr = effect_.border_radius;

                if (effect_.has_clip())
                    ctx.cmd_pop_clip();

                if (effect_.has_feather())
                    ctx.cmd_feather_rect(0, 0, rb.width, rb.height,
                                          rr, effect_.feather);

                if (effect_.has_border())
                {
                    StrokeStyle bs;
                    bs.color = effect_.border_color;
                    bs.width = effect_.border_width;
                    ctx.cmd_stroke_round_rect(0, 0, rb.width, rb.height, rr, bs);
                }
            }

            if (needs_transform) ctx.pop_transform();
            ctx.pop_style();
        }

    private:
        // ── 滚动视口渲染（flush 的内部实现）─────────────────────
        void flush_scroll_view(RenderContext& ctx,
                                const Rect& vp,
                                const LayoutConfig* child_layout,
                                float parent_wtx, float parent_wty) const
        {
            const ScrollConfig& sc = *scroll_cfg_;

            // 内容尺寸（0 = 与视口同尺寸）
            float cw = sc.content_width  > 0.f ? sc.content_width  : vp.width;
            float ch = sc.content_height > 0.f ? sc.content_height : vp.height;

            // 可滚动最大量，并钳位当前滚动位置
            float max_sx = std::max(0.f, cw - vp.width);
            float max_sy = std::max(0.f, ch - vp.height);
            scroll_x_ = std::clamp(scroll_x_, 0.f, max_sx);
            scroll_y_ = std::clamp(scroll_y_, 0.f, max_sy);

            // ── 计算滚动条几何（复用 dispatch 中的同一逻辑）──
            compute_sb_states();

            // ── 内容裁切 + 滚动变换 —— 局部坐标 ──────────────
            ctx.cmd_push_round_clip(0, 0, vp.width, vp.height, 0.f);

            bool has_scroll = scroll_x_ != 0.f || scroll_y_ != 0.f;
            if (has_scroll)
                ctx.push_transform(-scroll_x_, -scroll_y_, 1.f, 1.f, 0.f);

            static_chain_.execute(ctx);
            dynamic_chain_.execute(ctx);
            for (const auto& child : children_)
                if (child) child->flush(ctx, child_layout, parent_wtx, parent_wty);

            if (has_scroll) ctx.pop_transform();
            ctx.cmd_pop_clip();

            // ── 绘制垂直滚动条（在裁切外，始终可见）—— 局部坐标 ─
            if (v_sb_.visible)
            {
                const ScrollBarStyle& vs = sc.v_bar;
                float bx = vp.width - vs.width - vs.margin;
                float by = vs.margin;

                // 轨道
                ctx.cmd_fill_round_rect(bx, by, vs.width, v_sb_.track_len,
                                         vs.radius, vs.track);
                // 滑块
                Color thumb_col = vs.thumb;
                if (drag_target_ == DragTarget::VThumb)
                    thumb_col = vs.thumb_press;
                else if (is_hovered_)
                    thumb_col = vs.thumb_hover;
                ctx.cmd_fill_round_rect(bx, v_sb_.thumb_pos, vs.width,
                                         v_sb_.thumb_size, vs.radius, thumb_col);
            }

            // ── 绘制水平滚动条 —— 局部坐标 ────────────────────
            if (h_sb_.visible)
            {
                const ScrollBarStyle& hs = sc.h_bar;
                float bx = hs.margin;
                float by = vp.height - hs.width - hs.margin;

                ctx.cmd_fill_round_rect(bx, by, h_sb_.track_len, hs.width,
                                         hs.radius, hs.track);
                Color thumb_col = hs.thumb;
                if (drag_target_ == DragTarget::HThumb)
                    thumb_col = hs.thumb_press;
                else if (is_hovered_)
                    thumb_col = hs.thumb_hover;
                ctx.cmd_fill_round_rect(h_sb_.thumb_pos, by,
                                         h_sb_.thumb_size, hs.width,
                                         hs.radius, thumb_col);
            }
        }

    public:

        // ── 鼠标事件分发（由 otterwindow 内部调用）─────────────
        // 分发策略：子层优先（渲染顺序逆序 = 视觉上层优先），
        //           任意回调返回 true 则停止传播。
        // 返回值：true = 事件已被本层或子层消费

        // 移动事件（同时处理 enter / leave / hover 状态）
        bool dispatch_mouse_move(const MouseEvent& e)
        {
            if (!style_.visible) return false;

            // ── 滚动条拖拽中 —— 局部坐标 ─────────────────────────
            if (drag_target_ != DragTarget::None)
            {
                compute_sb_states();
                if (scroll_cfg_.has_value() && resolved_bounds_.has_value())
                {
                    const ScrollConfig& sc = *scroll_cfg_;
                    const Rect& vp = *resolved_bounds_;

                    if (drag_target_ == DragTarget::VThumb && v_sb_.visible)
                    {
                        float my = (e.y - vp.y) - v_sb_.track_start - drag_offset_;
                        float max_t = v_sb_.track_len - v_sb_.thumb_size;
                        float t = max_t > 0.f ? my / max_t : 0.f;
                        scroll_y_ = t * (sc.content_height - vp.height);
                        scroll_y_ = std::clamp(scroll_y_, 0.f, std::max(0.f, sc.content_height - vp.height));
                    }
                    else if (drag_target_ == DragTarget::HThumb && h_sb_.visible)
                    {
                        float mx = (e.x - vp.x) - h_sb_.track_start - drag_offset_;
                        float max_t = h_sb_.track_len - h_sb_.thumb_size;
                        float t = max_t > 0.f ? mx / max_t : 0.f;
                        scroll_x_ = t * (sc.content_width - vp.width);
                        scroll_x_ = std::clamp(scroll_x_, 0.f, std::max(0.f, sc.content_width - vp.width));
                    }
                }
                return true;
            }

            bool consumed = false;
            for (auto it = children_.rbegin(); it != children_.rend(); ++it)
            {
                if (!(*it)) continue;
                if ((*it)->dispatch_mouse_move(e)) { consumed = true; break; }
            }

            MouseEvent le = e;
            le.x -= world_tx_;
            le.y -= world_ty_;
            bool in_area = is_in_hit_area(le.x, le.y);
            if (in_area && !is_hovered_) { is_hovered_ = true;  fire_mouse_cbs(mouse_enter_, le); }
            else if (!in_area && is_hovered_) { is_hovered_ = false; fire_mouse_cbs(mouse_leave_, le); }
            if (in_area && !consumed)
            {
                fire_mouse_cbs(mouse_hover_, le);
                if (fire_mouse_cbs(mouse_move_, le)) consumed = true;
            }
            return consumed;
        }

        // 按下事件
        bool dispatch_mouse_down(const MouseEvent& e)
        {
            if (!style_.visible) return false;

            // ── 滚动条拖拽优先 ─────────────────────────────────
            if (scroll_cfg_.has_value() && resolved_bounds_.has_value())
            {
                compute_sb_states();
                const ScrollConfig& sc = *scroll_cfg_;
                const Rect& vp = *resolved_bounds_;
                auto hit = hit_scrollbar(e.x, e.y);
                if (hit.is_thumb)
                {
                    // 开始拖拽滑块 —— 局部坐标
                    if (hit.is_vertical)
                    {
                        drag_target_ = DragTarget::VThumb;
                        float rel = (e.y - vp.y) - v_sb_.thumb_pos;
                        drag_offset_ = (rel >= 0.f && rel <= v_sb_.thumb_size) ? rel : v_sb_.thumb_size / 2.f;
                    }
                    else
                    {
                        drag_target_ = DragTarget::HThumb;
                        float rel = (e.x - vp.x) - h_sb_.thumb_pos;
                        drag_offset_ = (rel >= 0.f && rel <= h_sb_.thumb_size) ? rel : h_sb_.thumb_size / 2.f;
                    }
                    return true;
                }
                if (hit.is_track)
                {
                    // 点击轨道 → 跳转到该位置 —— 局部坐标
                    if (hit.is_vertical)
                    {
                        float t = ((e.y - vp.y) - v_sb_.thumb_pos) / v_sb_.track_len;
                        scroll_y_ += t * (sc.content_height - vp.height) * 0.5f;
                        scroll_y_ = std::clamp(scroll_y_, 0.f, std::max(0.f, sc.content_height - vp.height));
                    }
                    else
                    {
                        float t = ((e.x - vp.x) - h_sb_.thumb_pos) / h_sb_.track_len;
                        scroll_x_ += t * (sc.content_width - vp.width) * 0.5f;
                        scroll_x_ = std::clamp(scroll_x_, 0.f, std::max(0.f, sc.content_width - vp.width));
                    }
                    return true;
                }
            }

            for (auto it = children_.rbegin(); it != children_.rend(); ++it)
            {
                if (!(*it)) continue;
                if ((*it)->dispatch_mouse_down(e)) return true;
            }
            {
                MouseEvent le = e;
                le.x -= world_tx_;
                le.y -= world_ty_;
                if (!is_in_hit_area(le.x, le.y)) return false;
                return fire_mouse_cbs(mouse_down_, le);
            }
        }

        // 抬起 + 点击事件
        bool dispatch_mouse_up(const MouseEvent& e, bool is_click, bool is_right)
        {
            // ── 释放滚动条拖拽 ─────────────────────────────────
            if (drag_target_ != DragTarget::None)
            {
                drag_target_ = DragTarget::None;
                return true;
            }

            if (!style_.visible) return false;
            for (auto it = children_.rbegin(); it != children_.rend(); ++it)
            {
                if (!(*it)) continue;
                if ((*it)->dispatch_mouse_up(e, is_click, is_right)) return true;
            }
            {
                MouseEvent le = e;
                le.x -= world_tx_;
                le.y -= world_ty_;
                if (!is_in_hit_area(le.x, le.y)) return false;
                bool consumed = fire_mouse_cbs(mouse_up_, le);
                if (is_click)
                    consumed |= is_right ? fire_mouse_cbs(mouse_right_click_, le)
                                         : fire_mouse_cbs(mouse_click_, le);
                return consumed;
            }
        }

        // 双击事件
        bool dispatch_double_click(const MouseEvent& e)
        {
            if (!style_.visible) return false;
            for (auto it = children_.rbegin(); it != children_.rend(); ++it)
            {
                if (!(*it)) continue;
                if ((*it)->dispatch_double_click(e)) return true;
            }
            {
                MouseEvent le = e;
                le.x -= world_tx_;
                le.y -= world_ty_;
                if (!is_in_hit_area(le.x, le.y)) return false;
                return fire_mouse_cbs(mouse_double_click_, le);
            }
        }

        // 滚轮事件
        bool dispatch_wheel(const MouseEvent& e)
        {
            if (!style_.visible) return false;

            MouseEvent le = e;
            le.x -= world_tx_;
            le.y -= world_ty_;

            // ── 滚轮驱动滚动（命中视口时消费事件）──────────────
            if (scroll_cfg_.has_value() && resolved_bounds_.has_value()
                && resolved_bounds_->contains(le.x, le.y))
            {
                float step = scroll_cfg_->wheel_step;
                if (e.ctrl_down)
                {
                    // Ctrl + 滚轮：横向滚动
                    scroll_x_ -= e.wheel_delta * step;
                    float cw = scroll_cfg_->content_width;
                    float vw = resolved_bounds_->width;
                    scroll_x_ = std::clamp(scroll_x_, 0.f, std::max(0.f, cw - vw));
                }
                else
                {
                    // 普通滚轮：纵向滚动
                    scroll_y_ -= e.wheel_delta * step;
                    float ch = scroll_cfg_->content_height;
                    float vh = resolved_bounds_->height;
                    scroll_y_ = std::clamp(scroll_y_, 0.f, std::max(0.f, ch - vh));
                }
                // 先传给子层（如嵌套滚动），若子层不消费则本层消费
                for (auto it = children_.rbegin(); it != children_.rend(); ++it)
                    if (*it && (*it)->dispatch_wheel(e)) return true;
                return true;
            }

            for (auto it = children_.rbegin(); it != children_.rend(); ++it)
            {
                if (!(*it)) continue;
                if ((*it)->dispatch_wheel(e)) return true;
            }
            if (!is_in_hit_area(le.x, le.y)) return false;
            return fire_mouse_cbs(mouse_wheel_, le);
        }

        // 文件拖放事件分发
        bool dispatch_drop_files(const std::vector<std::wstring>& files, const MouseEvent& e)
        {
            if (!style_.visible) return false;

            // 子层优先
            for (auto it = children_.rbegin(); it != children_.rend(); ++it)
            {
                if (!(*it)) continue;
                if ((*it)->dispatch_drop_files(files, e)) return true;
            }

            // 转换为局部坐标
            MouseEvent le = e;
            le.x -= world_tx_;
            le.y -= world_ty_;

            if (!is_in_hit_area(le.x, le.y)) return false;

            // 触发回调
            for (const auto& cb : drop_files_)
            {
                if (cb(files, le)) return true;
            }
            return false;
        }

        // 鼠标离开窗口时，递归重置所有图层的 hover 状态
        void reset_hover_recursive()
        {
            if (is_hovered_) { is_hovered_ = false; MouseEvent e{}; fire_mouse_cbs(mouse_leave_, e); }
            drag_target_ = DragTarget::None;  // 释放滚动条拖拽
            for (auto& child : children_) if (child) child->reset_hover_recursive();
        }

    private:
        // ── 基本属性 ─────────────────────────────────────────────
        std::string       name_;
        bool              is_canvas_  = false;
        LayerStyle        style_;
        Layer*            parent_     = nullptr;  // 父层指针（canvas 根层为 nullptr）

        // ── 命中区域 ─────────────────────────────────────────────
        // hit_area_mode_ 决定以哪种方式做命中测试（最后调用的设置生效）
        enum class HitAreaMode : uint8_t { None, Manual, Auto };
        HitAreaMode             hit_area_mode_      = HitAreaMode::None;
        std::optional<Rect>     hit_area_;           // Manual 模式下的矩形
        mutable std::optional<Rect> resolved_hit_area_; // Auto 模式，flush 时计算
        bool                    is_hovered_         = false;
        HitTestCallback         hit_test_cb_;

        // ── 图层变换 ─────────────────────────────────────────────
        // PosSource 控制 translate() 与 LayoutPos() 哪个生效（最后调用的为准）
        enum class PosSource : uint8_t { None, Manual, Layout };
        PosSource               pos_source_         = PosSource::None;
        LayerTransform          transform_;          // 显式变换（translate/scale/rotate）

        // ── 布局配置 ─────────────────────────────────────────────
        std::optional<LayoutConfig>   layout_config_; // 本层作为容器时的网格配置
        std::optional<LayoutPosition> layout_pos_;    // 本层在父层网格中的位置

        // ── CSS 效果 ─────────────────────────────────────────────
        LayerEffect             effect_;
        std::optional<Rect>     bounds_;
        mutable std::optional<Rect> resolved_bounds_;
        mutable float           world_tx_ = 0.f;  // 本帧世界偏移（用于 dispatch 坐标转换）
        mutable float           world_ty_ = 0.f;

        // ── 背景图片 ─────────────────────────────────────────────
        std::wstring            bg_img_path_;
        float                   bg_img_opacity_    = 1.f;
        float                   bg_img_radius_     = 0.f;
        bool                    bg_img_registered_ = false;

        // ── 滚动状态 ─────────────────────────────────────────────
        std::optional<ScrollConfig> scroll_cfg_;
        // 注意：scroll_x_ 和 scroll_y_ 被声明为 mutable 以支持在 const 方法中修改
        // 这是设计决策：flush() 是 const 方法但需要更新滚动位置钳制
        // 在多线程环境下，应确保同一 Layer 不会被多个线程同时 flush
        mutable float scroll_x_ = 0.f;
        mutable float scroll_y_ = 0.f;

        struct SBState {
            mutable float thumb_pos  = 0.f;  // 滑块起始坐标（flush 时计算）
            mutable float thumb_size = 0.f;  // 滑块尺寸
            mutable bool  visible    = false; // 本帧是否可见

            // 计算轨道长度（内部使用）
            float track_len  = 0.f;
            float track_start = 0.f;
            float bar_x = 0.f;  // 滚动条 X（垂直）或 Y（水平）
        };
        mutable SBState v_sb_, h_sb_;

        // ── 滚动条拖拽状态 ──────────────────────────────────────
        enum class DragTarget : uint8_t { None, VThumb, HThumb };
        mutable DragTarget drag_target_ = DragTarget::None;
        mutable float      drag_offset_ = 0.f;  // 拖拽起始时鼠标在滑块内的偏移

        // ── 绘画链 ───────────────────────────────────────────────
        PaintChain   static_chain_;
        PaintChain   dynamic_chain_;

        // ── 帧回调 ───────────────────────────────────────────────
        std::vector<UpdateCallback> update_callbacks_;
        std::vector<RenderCallback> render_callbacks_;

        // ── 鼠标回调 ─────────────────────────────────────────────
        std::vector<MouseCallback> mouse_click_;
        std::vector<MouseCallback> mouse_right_click_;
        std::vector<MouseCallback> mouse_double_click_;
        std::vector<MouseCallback> mouse_down_;
        std::vector<MouseCallback> mouse_up_;
        std::vector<MouseCallback> mouse_move_;
        std::vector<MouseCallback> mouse_enter_;
        std::vector<MouseCallback> mouse_leave_;
        std::vector<MouseCallback> mouse_hover_;
        std::vector<MouseCallback> mouse_wheel_;

        // ── 文件拖放回调 ─────────────────────────────────────────
        std::vector<DropCallback> drop_files_;
        void* native_component_ = nullptr;
        void (*native_component_destroy_)(void*) = nullptr;

        // ── 子层 ─────────────────────────────────────────────────
        std::vector<std::unique_ptr<Layer>>    children_;

        // ── Heterogeneous lookup 支持的 string_map ────────────────
        // 使用 C++20 特性，支持用 string_view 查找，避免创建临时 string
        struct StringHash {
            using is_transparent = void;  // 启用 heterogeneous lookup
            std::size_t operator()(std::string_view sv) const noexcept {
                // FNV-1a hash，高效且分布均匀
                std::size_t h = 14695981039346656037ull;
                for (char c : sv) {
                    h ^= static_cast<std::size_t>(c);
                    h *= 1099511628211ull;
                }
                return h;
            }
        };
        struct StringEq {
            using is_transparent = void;  // 启用 heterogeneous lookup
            bool operator()(std::string_view a, std::string_view b) const noexcept {
                return a == b;
            }
        };
        std::unordered_map<std::string, Layer*, StringHash, StringEq> name_map_;

        // ── 动画管理器指针（由 otterwindow 设置）──────────────────
        AnimManager* anim_manager_ = nullptr;

        // ── 私有辅助 ─────────────────────────────────────────────

        // 递归收集所有后代的边界
        static void s_collect_bounds(const Layer* layer,
                                     float tx, float ty,
                                     ContentBounds& b)
        {
            if (auto* rb = layer->resolved_bounds())
            {
                b.min_x = std::fmin(b.min_x, tx + rb->x);
                b.min_y = std::fmin(b.min_y, ty + rb->y);
                b.max_x = std::fmax(b.max_x, tx + rb->x + rb->width);
                b.max_y = std::fmax(b.max_y, ty + rb->y + rb->height);
            }
            for (const auto& child : layer->children_)
                if (child) s_collect_bounds(child.get(), tx, ty, b);
        }

        // 计算滚动条几何（与 flush_scroll_view 中相同的数学）
        void compute_sb_states() const
        {
            if (!scroll_cfg_.has_value() || !resolved_bounds_.has_value()) {
                v_sb_.visible = false; h_sb_.visible = false;
                return;
            }
            const ScrollConfig& sc = *scroll_cfg_;
            const Rect& vp = *resolved_bounds_;
            float cw = sc.content_width  > 0.f ? sc.content_width  : vp.width;
            float ch = sc.content_height > 0.f ? sc.content_height : vp.height;
            float max_sy = std::max(0.f, ch - vp.height);
            float max_sx = std::max(0.f, cw - vp.width);

            auto compute_bar = [](SBState& sb, const ScrollBarStyle& style,
                                  float total, float view, float scroll,
                                  float track_len, float track_start) {
                bool should_show =
                    style.visibility == ScrollBarStyle::Visibility::Always
                    || (style.visibility == ScrollBarStyle::Visibility::Auto && total > view + 0.5f);
                sb.visible = should_show;
                sb.track_len = track_len;
                sb.track_start = track_start;
                if (should_show && total > 0.f) {
                    float ratio = view / total;
                    sb.thumb_size = std::max(style.min_thumb, ratio * track_len);
                    float t = total > view ? scroll / (total - view) : 0.f;
                    sb.thumb_pos = track_start + t * (track_len - sb.thumb_size);
                }
            };

            float v_track_len = vp.height - sc.v_bar.margin * 2.f;
            compute_bar(v_sb_, sc.v_bar, ch, vp.height, scroll_y_,
                         v_track_len, sc.v_bar.margin);

            float h_track_len = vp.width - sc.h_bar.margin * 2.f
                                - (v_sb_.visible ? sc.v_bar.width + sc.v_bar.margin : 0.f);
            compute_bar(h_sb_, sc.h_bar, cw, vp.width, scroll_x_,
                         h_track_len, sc.h_bar.margin);
        }

        // 滚动条命中测试：返回 Hit {is_thumb, is_track, is_vertical}
        struct SBHit { bool is_thumb, is_track, is_vertical; };
        SBHit hit_scrollbar(float mx, float my) const
        {
            if (!scroll_cfg_.has_value() || !resolved_bounds_.has_value()) return {};
            const ScrollConfig& sc = *scroll_cfg_;
            const Rect& vp = *resolved_bounds_;

            // 转为局部坐标
            float lmx = mx - vp.x;
            float lmy = my - vp.y;

            // 垂直滚动条
            if (v_sb_.visible) {
                float bx = vp.width - sc.v_bar.width - sc.v_bar.margin;
                float by = sc.v_bar.margin;
                float bw = sc.v_bar.width;
                float bh = v_sb_.track_len;
                if (lmx >= bx && lmx <= bx + bw && lmy >= by && lmy <= by + bh) {
                    if (lmy >= v_sb_.thumb_pos && lmy <= v_sb_.thumb_pos + v_sb_.thumb_size)
                        return { true, false, true };
                    return { false, true, true };
                }
            }
            // 水平滚动条
            if (h_sb_.visible) {
                float bx = sc.h_bar.margin;
                float by = vp.height - sc.h_bar.width - sc.h_bar.margin;
                float bw = h_sb_.track_len;
                float bh = sc.h_bar.width;
                if (lmx >= bx && lmx <= bx + bw && lmy >= by && lmy <= by + bh) {
                    if (lmx >= h_sb_.thumb_pos && lmx <= h_sb_.thumb_pos + h_sb_.thumb_size)
                        return { true, false, false };
                    return { false, true, false };
                }
            }
            return {};
        }

        // 判断点是否在命中区域内
        // 优先级：Auto(resolved) > Manual(hit_area_) > None(全通过)
        bool is_in_hit_area(float px, float py) const noexcept
        {
            bool base = true;
            switch (hit_area_mode_)
            {
            case HitAreaMode::Auto:
                // Auto 模式：使用 flush 时计算好的 resolved_hit_area_
                base = resolved_hit_area_.has_value()
                    ? resolved_hit_area_->contains(px, py)
                    : true;  // 尚未 flush 过，暂时全通过
                break;
            case HitAreaMode::Manual:
                base = hit_area_.has_value()
                    ? hit_area_->contains(px, py)
                    : true;
                break;
            case HitAreaMode::None:
            default:
                base = true;  // 无限制，全窗口响应
                break;
            }
            if (base && hit_test_cb_)
                return hit_test_cb_(px, py);
            return base;
        }

        // 顺序触发回调列表，任一返回 true 则返回 true（consume）
        // 使用快照防止回调中修改列表导致迭代器失效
        static bool fire_mouse_cbs(const std::vector<MouseCallback>& cbs,
                                   const MouseEvent&                 e)
        {
            // 优化：先检查是否有空回调需要清理，避免每次都完整复制
            // 只有在回调可能修改列表时才需要快照（当前实现中回调不会修改列表）
            // 直接遍历，减少内存分配
            for (const auto& cb : cbs)
                if (cb && cb(e)) return true;
            return false;
        }
    };


    // ============================================================
    //  AnimManager::apply_to_layer 实现（必须在 Layer 定义之后）
    // ============================================================
    inline void AnimManager::apply_to_layer(Layer* layer, AnimTrack::Target target, float value)
    {
        switch (target)
        {
        case AnimTrack::TranslateX:
            layer->set_anim_translate_x(value);
            break;
        case AnimTrack::TranslateY:
            layer->set_anim_translate_y(value);
            break;
        case AnimTrack::Scale:
            layer->set_anim_scale(value);
            break;
        case AnimTrack::Opacity:
            layer->opacity(value);
            break;
        case AnimTrack::Rotate:
            layer->rotate(value);
            break;
        }
    }


    // ============================================================
    //  图层代理（LayerRef）
    //
    //  包装 Layer* 的轻量代理，提供 .creat["name"] 语法。
    //  不拥有 Layer 的生命周期（不要用它 delete）。
    //
    //  用法：
    //    win.creat["bg"].creat["sky"].creat["cloud"];
    //    auto* sky = win.get["sky"];
    // ============================================================
    struct LayerRef
    {
        Layer* ptr = nullptr;

        // 透明访问底层 Layer
        Layer* operator->() const
        {
            assert(ptr && "LayerRef: 访问空图层指针");
            return ptr;
        }

        Layer& operator*() const
        {
            assert(ptr && "LayerRef: 解引用空图层指针");
            return *ptr;
        }

        // 隐式转换为 Layer*，方便直接赋值给裸指针变量
        operator Layer*() const { return ptr; }

        // ── creat 代理 ───────────────────────────────────────────
        // 实现 layerRef.creat["child_name"] 语法
        // 在当前层下创建子层，并返回子层的 LayerRef（可继续链式）
        struct CreatProxy
        {
            Layer* parent;

            // operator[] 在 parent 下创建子层，返回子层的 LayerRef
            LayerRef operator[](std::string_view name) const
            {
                assert(parent && "CreatProxy: 父层指针为空");
                return LayerRef{ parent->creat(name) };
            }
        };

        // ── get 代理 ─────────────────────────────────────────────
        // 实现 layerRef.get["name"] 语法，在子树中搜索图层
        struct GetProxy
        {
            Layer* parent;

            // operator[] 在 parent 的子树中查找图层，返回裸指针
            Layer* operator[](std::string_view name) const
            {
                assert(parent && "GetProxy: 父层指针为空");
                return parent->find(name);
            }
        };

        // creat 和 get 作为成员，允许 ref.creat["x"] / ref.get["x"]
        // 注意：它们不是函数，是代理对象，通过 operator[] 工作
        CreatProxy creat{ ptr };
        GetProxy   get  { ptr };

        // ── 构造与赋值 ───────────────────────────────────────────
        LayerRef() = default;

        explicit LayerRef(Layer* p)
            : ptr(p), creat{p}, get{p}
        {}

        LayerRef& operator=(Layer* p)
        {
            ptr   = p;
            creat = CreatProxy{p};
            get   = GetProxy{p};
            return *this;
        }
    };

    // ============================================================
    //  PaintChain 内存池实现（可选优化）
    // ============================================================
    namespace detail
    {
        // 简单的线性分配内存池
        class PaintOpPool
        {
        public:
            static PaintOpPool& instance()
            {
                static PaintOpPool pool;
                return pool;
            }

            void enable(bool e, size_t size = 1024 * 1024)  // 默认 1MB
            {
                enabled_ = e;
                if (e && buffer_.empty())
                {
                    buffer_.resize(size);
                    offset_ = 0;
                }
            }

            void* alloc(size_t size)
            {
                if (!enabled_) return nullptr;
                if (offset_ + size > buffer_.size()) return nullptr;  // 池已满

                void* ptr = buffer_.data() + offset_;
                offset_ += size;
                // 对齐到 8 字节
                offset_ = (offset_ + 7) & ~size_t(7);
                return ptr;
            }

            void reset() { offset_ = 0; }

            bool is_enabled() const { return enabled_; }

        private:
            PaintOpPool() = default;
            std::vector<char> buffer_;
            size_t offset_ = 0;
            bool enabled_ = false;
        };
    }

    inline void PaintChain::enable_pool(bool enable, size_t pool_size)
    {
        detail::PaintOpPool::instance().enable(enable, pool_size);
    }

    inline void* PaintChain::pool_alloc(size_t size)
    {
        return detail::PaintOpPool::instance().alloc(size);
    }

    inline void PaintChain::pool_dealloc(void* ptr, size_t size)
    {
        // 线性池不支持单独释放，只能整体 reset
        (void)ptr; (void)size;
    }

} // namespace Otter
