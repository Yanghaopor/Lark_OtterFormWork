#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace Otter
{
    class Layer;
    struct MouseEvent;

    enum class BrowserBackend
    {
        Auto,
        Cef,
        WebView2,
    };

    inline constexpr BrowserBackend default_browser_backend()
    {
        return BrowserBackend::Auto;
    }

    inline constexpr const char* browser_backend_name(BrowserBackend backend)
    {
        switch (backend)
        {
        case BrowserBackend::Cef:
            return "cef";
        case BrowserBackend::WebView2:
            return "webview2";
        case BrowserBackend::Auto:
        default:
            return "auto";
        }
    }

    inline constexpr BrowserBackend resolve_browser_backend(BrowserBackend backend)
    {
        if (backend == BrowserBackend::Auto)
            return BrowserBackend::Cef;
        return backend;
    }

    inline constexpr bool browser_backend_supported(BrowserBackend backend)
    {
#if defined(_WIN32)
        return backend == BrowserBackend::Auto || backend == BrowserBackend::Cef || backend == BrowserBackend::WebView2;
#else
        return backend == BrowserBackend::Auto || backend == BrowserBackend::Cef;
#endif
    }

    class IBrowserNode
    {
    public:
        virtual ~IBrowserNode() = default;

        virtual void set_query_handler(std::function<void(const std::wstring&)> handler) = 0;
        virtual void show(bool visible) = 0;
        virtual void set_bounds(int x, int y, int width, int height) = 0;
        virtual void load_url(const std::wstring& url) = 0;
        virtual void load_file(const std::wstring& path) = 0;
        virtual void load_html(const std::wstring& html, const std::wstring& virtual_url = L"https://otter.local/") = 0;
        virtual void evaluate_script(const std::wstring& script, const std::wstring& url = L"https://otter.local/script") = 0;
        virtual void set_json_state(const std::wstring& json) = 0;
    };

    class IBrowserLayer
    {
    public:
        using QueryHandler = std::function<void(const std::wstring&)>;
        using RegionHitTest = std::function<bool(float local_x, float local_y)>;

        virtual ~IBrowserLayer() = default;

        virtual void resize(int width, int height) = 0;
        virtual void set_query_handler(QueryHandler handler) = 0;
        virtual void load_url(const std::wstring& url) = 0;
        virtual void load_file(const std::wstring& path) = 0;
        virtual void load_html(const std::wstring& html, const std::wstring& virtual_url = L"https://otter.local/") = 0;
        virtual void evaluate_script(const std::wstring& script, const std::wstring& url = L"https://otter.local/script") = 0;
        virtual void set_json_state(const std::wstring& json) = 0;
        virtual void transparent_hit_test(bool enable, uint8_t alpha_threshold = 8) = 0;
        virtual void set_region_hit_test(RegionHitTest hit_test) = 0;
        virtual void focus(bool focused) = 0;

        virtual bool hit_test(float local_x, float local_y) const = 0;
        virtual bool send_mouse_move(const MouseEvent& e) = 0;
        virtual bool send_mouse_down(const MouseEvent& e) = 0;
        virtual bool send_mouse_up(const MouseEvent& e) = 0;
        virtual bool send_wheel(const MouseEvent& e) = 0;
        virtual void send_key_down(WPARAM key, LPARAM native) = 0;
        virtual void send_key_char(WPARAM ch, LPARAM native) = 0;
        virtual void send_key_up(WPARAM key, LPARAM native) = 0;

        virtual const uint8_t* pixels() const = 0;
        virtual int width() const = 0;
        virtual int height() const = 0;
        virtual int stride() const = 0;
    };

    int browser_execute_subprocess(BrowserBackend backend, HINSTANCE instance);
    bool browser_initialize(BrowserBackend backend, HINSTANCE instance);
    void browser_shutdown(BrowserBackend backend);
    void browser_use_shared_texture(BrowserBackend backend, bool enable);

    std::unique_ptr<IBrowserNode> create_browser_node(BrowserBackend backend, HWND parent);
    IBrowserLayer* attach_browser_layer(BrowserBackend backend, Layer* layer, int width, int height, void* parent_window = nullptr);
}
