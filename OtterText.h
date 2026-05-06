#pragma once
// ============================================================
//  OtterText.h
//  水獭图形框架 —— 字体加载与文本工具
//
//  本文件提供 OtterLayer.h 核心文字功能的扩展工具：
//    · FontLibrary  : 从文件 / 内存 / 资源加载自定义字体
//    · TextBuilder  : 多样式富文本构建器（链式 API）
//    · text_measure : 便捷测量函数（无需持有 RenderContext）
//
//  依赖：OtterWindow.h（已包含 OtterRenderer.h → OtterLayer.h）
//
//  用法：
//    #include "OtterWindow.h"
//    #include "OtterText.h"
//
//  命名空间：Otter
//  C++ 标准：C++20
// ============================================================

#include "OtterWindow.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>

namespace Otter
{
    // ============================================================
    //  FontLibrary —— 自定义字体加载
    //
    //  支持三种字体来源：
    //    1. 系统字体 : 直接在 TextStyle::font_family 填入字体名即可，
    //                 无需 FontLibrary 介入。
    //    2. 文件加载 : FontLibrary::load_file("C:/fonts/my.ttf")
    //    3. 内存加载 : FontLibrary::load_memory(data_ptr, data_size)
    //
    //  实现原理（Windows）：
    //    使用 AddFontMemResourceEx / AddFontResourceExW 将字体注册到
    //    当前进程，DirectWrite 随后可通过字体族名找到它。
    //    进程退出时字体自动注销；也可调用 unload() 提前注销。
    //
    //  注意：加载后需用字体文件中实际的字体族名（可用 FontLibrary::family_name
    //        查询），而非文件名。
    // ============================================================
    class FontLibrary
    {
    public:
        FontLibrary()  = default;
        ~FontLibrary() { unload_all(); }

        // 不可拷贝（持有字体句柄）
        FontLibrary(const FontLibrary&)            = delete;
        FontLibrary& operator=(const FontLibrary&) = delete;

        // ── 从文件路径加载 ───────────────────────────────────────

        // 加载 TTF / OTF / TTC 字体文件
        // path : 字体文件绝对路径（宽字符）
        // 返回 true = 成功，随后可在 TextStyle::font_family 中使用字体名
        bool load_file(std::wstring_view path)
        {
            // AddFontResourceExW 返回 int（添加的字体数量），0 = 失败
            // FR_PRIVATE：仅对当前进程可见，不影响其他应用
            int n = AddFontResourceExW(path.data(), FR_PRIVATE, nullptr);
            if (n <= 0) return false;

            // 通知所有窗口字体集合已变化（部分旧版 DWrite 需要）
            PostMessageW(HWND_BROADCAST, WM_FONTCHANGE, 0, 0);

            FontEntry e;
            e.path           = std::wstring(path);
            e.file_added     = true;
            e.mem_handle     = nullptr;
            e.mem_font_count = 0;
            font_handles_.push_back(std::move(e));
            return true;
        }

        // UTF-8 路径版本
        bool load_file(std::string_view path_utf8)
        {
            return load_file(utf8_to_utf16(path_utf8));
        }

        // ── 从内存加载 ───────────────────────────────────────────

        // 从内存缓冲区加载字体（适用于嵌入资源的字体数据）
        // data    : 字体文件原始字节（TTF/OTF/TTC 格式）
        // size    : 数据大小（字节）
        // 注意：AddFontMemResourceEx 会复制数据，调用后原始缓冲区可释放
        bool load_memory(const void* data, DWORD size)
        {
            if (!data || size == 0) return false;

            DWORD num_fonts = 0;
            HANDLE h = AddFontMemResourceEx(
                const_cast<void*>(data), size,
                nullptr, &num_fonts);

            if (!h) return false;
            PostMessageW(HWND_BROADCAST, WM_FONTCHANGE, 0, 0);

            FontEntry e;
            e.file_added     = false;
            e.mem_handle     = h;
            e.mem_font_count = num_fonts;
            font_handles_.push_back(std::move(e));
            return true;
        }

        // ── 从 EXE/DLL 嵌入资源加载 ─────────────────────────────

        // 从 Win32 资源节（RCDATA 类型）加载字体
        // resource_name : 资源名称（字符串或整数 ID）
        // hmodule       : 含资源的模块（nullptr = 当前 EXE）
        bool load_resource(LPCWSTR resource_name, HMODULE hmodule = nullptr)
        {
            HRSRC   rsrc = FindResourceW(hmodule, resource_name, RT_RCDATA);
            if (!rsrc) return false;

            HGLOBAL hglobal = LoadResource(hmodule, rsrc);
            if (!hglobal) return false;

            void*  data = LockResource(hglobal);
            DWORD  size = SizeofResource(hmodule, rsrc);

            return load_memory(data, size);
        }

        // ── 注销 ─────────────────────────────────────────────────

        // 注销所有已加载字体（通常无需手动调用，析构时自动处理）
        
        void unload_all()
        {
            for (auto& e : font_handles_)
            {
                if (e.file_added)
                {
                    // AddFontResourceExW 用 RemoveFontResourceExW 注销
                    RemoveFontResourceExW(e.path.c_str(), FR_PRIVATE, nullptr);
                }
                else if (e.mem_handle)
                {
                    // AddFontMemResourceEx 用 RemoveFontMemResourceEx 注销
                    RemoveFontMemResourceEx(e.mem_handle);
                    e.mem_handle = nullptr;
                }
            }
            font_handles_.clear();
            PostMessageW(HWND_BROADCAST, WM_FONTCHANGE, 0, 0);
        }

        // 已加载字体的数量
        int count() const noexcept { return (int)font_handles_.size(); }

    private:
        // FontEntry 区分两种加载方式：
        //   文件字体  : file_added=true，用路径注销（RemoveFontResourceExW）
        //   内存字体  : file_added=false，mem_handle 非空，用句柄注销
        struct FontEntry
        {
            std::wstring path;           // 文件字体路径（文件方式使用）
            bool         file_added     = false;  // true = 通过文件加载
            HANDLE       mem_handle     = nullptr; // 内存字体句柄
            DWORD        mem_font_count = 0;       // 内存字体中的字体数量
        };
        std::vector<FontEntry> font_handles_;

        static std::wstring utf8_to_utf16(std::string_view s)
        {
            if (s.empty()) return {};
            int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
            if (n <= 0) return {};
            std::wstring out(n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
            return out;
        }
    };


    // ============================================================
    //  TextBuilder —— 富文本链式构建器
    //
    //  用于快速构建带有统一基准样式的文本，
    //  然后一次性追加到 PaintChain。
    //
    //  用法：
    //    TextBuilder(win.renderer())
    //        .font(L"微软雅黑", 18.f)
    //        .color(Color::white())
    //        .shadow(ShadowStyle{...})
    //        .align(TextStyle::HAlign::Center)
    //        .build(L"Hello, Otter!", 100, 50)
    //        .apply_to(layer->paint());
    // ============================================================
    class TextBuilder
    {
    public:
        explicit TextBuilder() = default;

        // ── 样式配置（均返回 *this，支持链式调用）───────────────

        TextBuilder& font(std::wstring family, float size = 14.f)
        {
            style_.font_family = std::move(family);
            style_.font_size   = size;
            return *this;
        }

        TextBuilder& size(float s)   { style_.font_size = s;              return *this; }
        TextBuilder& bold()          { style_.weight = TextStyle::Weight::Bold;   return *this; }
        TextBuilder& italic()        { style_.font_style = TextStyle::FontStyle::Italic; return *this; }
        TextBuilder& color(Color c)  { style_.color = c;                  return *this; }
        TextBuilder& underline()     { style_.underline = true;           return *this; }
        TextBuilder& strikethrough() { style_.strikethrough = true;       return *this; }
        TextBuilder& weight(TextStyle::Weight w) { style_.weight = w;     return *this; }

        TextBuilder& shadow(ShadowStyle s)
        { style_.shadow = std::move(s); return *this; }

        TextBuilder& stroke(float w, Color c)
        { style_.stroke_width = w; style_.stroke_color = c; return *this; }

        TextBuilder& align(TextStyle::HAlign h, TextStyle::VAlign v = TextStyle::VAlign::Top)
        { style_.h_align = h; style_.v_align = v; return *this; }

        TextBuilder& letter_spacing(float ls) { style_.letter_spacing = ls; return *this; }
        TextBuilder& word_wrap(bool ww = true) { style_.word_wrap = ww; return *this; }

        TextBuilder& render_mode(TextStyle::RenderMode m)
        { style_.render_mode = m; return *this; }

        // 读取当前样式（可用于 PaintChain::text() 直接传入）
        const TextStyle& get_style() const noexcept { return style_; }

        // ── 生成 PaintChain 调用 ─────────────────────────────────

        // 将文本追加到 PaintChain（返回 chain 自身以继续链式）
        PaintChain& apply(PaintChain& chain,
                           std::wstring_view content, float x, float y,
                           float max_w = 0.f, float max_h = 0.f) const
        {
            return chain.text(content, x, y, max_w, max_h, style_);
        }

        PaintChain& apply(PaintChain& chain,
                           std::string_view content_utf8, float x, float y,
                           float max_w = 0.f, float max_h = 0.f) const
        {
            return chain.text(content_utf8, x, y, max_w, max_h, style_);
        }

    private:
        TextStyle style_;
    };


    // ============================================================
    //  便捷函数：测量文本尺寸
    //  需要传入 D2DRenderContext 引用（由 win.renderer() 获取）。
    //
    //  用法：
    //    TextMetrics m;
    //    if (measure_text(win.renderer(), L"Hello", style, m))
    //        do_something_with(m.width, m.height);
    // ============================================================
    inline bool measure_text(D2DRenderContext& ctx,
                              const std::wstring& content,
                              const TextStyle& style,
                              TextMetrics& out,
                              float max_width  = 0.f,
                              float max_height = 0.f)
    {
        return ctx.cmd_measure_text(content, max_width, max_height, style, out);
    }

    // UTF-8 版本
    inline bool measure_text(D2DRenderContext& ctx,
                              std::string_view content_utf8,
                              const TextStyle& style,
                              TextMetrics& out,
                              float max_width  = 0.f,
                              float max_height = 0.f)
    {
        // 转换 UTF-8 → UTF-16
        std::wstring wide;
        if (!content_utf8.empty())
        {
            int n = MultiByteToWideChar(CP_UTF8, 0,
                                         content_utf8.data(),
                                         (int)content_utf8.size(), nullptr, 0);
            if (n > 0)
            {
                wide.resize(n);
                MultiByteToWideChar(CP_UTF8, 0,
                                     content_utf8.data(), (int)content_utf8.size(),
                                     wide.data(), n);
            }
        }
        return ctx.cmd_measure_text(wide, max_width, max_height, style, out);
    }

} // namespace Otter
