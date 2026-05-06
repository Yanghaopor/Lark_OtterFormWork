#pragma once

#ifndef _WIN32
#error "OtterWebView2Node is only available on Windows because WebView2 is a Windows platform component."
#endif

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "OtterBrowserBackend.h"

namespace Otter
{
    class Layer;
    struct MouseEvent;

    class OtterWebView2Node : public IBrowserNode
    {
    public:
        using QueryHandler = std::function<void(const std::wstring&)>;

        static int execute_subprocess(HINSTANCE instance);
        static bool initialize(HINSTANCE instance);
        static void shutdown();
        static void use_shared_texture(bool enable);

        explicit OtterWebView2Node(HWND parent);
        ~OtterWebView2Node();

        OtterWebView2Node(const OtterWebView2Node&) = delete;
        OtterWebView2Node& operator=(const OtterWebView2Node&) = delete;

        void set_query_handler(QueryHandler handler);
        void show(bool visible);
        void set_bounds(int x, int y, int width, int height);
        void load_url(const std::wstring& url);
        void load_file(const std::wstring& path);
        void load_html(const std::wstring& html, const std::wstring& virtual_url = L"https://otter.local/");
        void evaluate_script(const std::wstring& script, const std::wstring& url = L"https://otter.local/script");
        void set_json_state(const std::wstring& json);
        void focus(bool focused);

    private:
        class Impl;
        Impl* impl_ = nullptr;
    };

    class OtterWebView2Layer : public IBrowserLayer
    {
    public:
        using QueryHandler = OtterWebView2Node::QueryHandler;
        using RegionHitTest = std::function<bool(float local_x, float local_y)>;

        explicit OtterWebView2Layer(Layer* layer, void* parent_window = nullptr);
        ~OtterWebView2Layer();

        OtterWebView2Layer(const OtterWebView2Layer&) = delete;
        OtterWebView2Layer& operator=(const OtterWebView2Layer&) = delete;

        static OtterWebView2Layer* attach(Layer* layer, int width, int height, void* parent_window = nullptr);

        void resize(int width, int height);
        void set_query_handler(QueryHandler handler);
        void load_url(const std::wstring& url);
        void load_file(const std::wstring& path);
        void load_html(const std::wstring& html, const std::wstring& virtual_url = L"https://otter.local/");
        void evaluate_script(const std::wstring& script, const std::wstring& url = L"https://otter.local/script");
        void set_json_state(const std::wstring& json);
        void transparent_hit_test(bool enable, uint8_t alpha_threshold = 8);
        void set_region_hit_test(RegionHitTest hit_test);
        void focus(bool focused);

        bool hit_test(float local_x, float local_y) const;
        bool send_mouse_move(const MouseEvent& e);
        bool send_mouse_down(const MouseEvent& e);
        bool send_mouse_up(const MouseEvent& e);
        bool send_wheel(const MouseEvent& e);
        void send_key_down(WPARAM key, LPARAM native);
        void send_key_char(WPARAM ch, LPARAM native);
        void send_key_up(WPARAM key, LPARAM native);

        const uint8_t* pixels() const;
        int width() const;
        int height() const;
        int stride() const;

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };
}
