#pragma once

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

#if defined(_MSC_VER) && !defined(OTTER_CHROME_NO_AUTOLINK)
#  if defined(_DEBUG)
#    pragma comment(lib, "OtterChromeNode_d.lib")
#    pragma comment(lib, "libcef_d.lib")
#    pragma comment(lib, "libcef_dll_wrapper_d.lib")
#  else
#    pragma comment(lib, "OtterChromeNode.lib")
#    pragma comment(lib, "libcef.lib")
#    pragma comment(lib, "libcef_dll_wrapper.lib")
#  endif
#  pragma comment(lib, "d3d11.lib")
#  pragma comment(lib, "dxgi.lib")
#  pragma comment(lib, "user32.lib")
#  pragma comment(lib, "ole32.lib")
#  pragma comment(lib, "oleaut32.lib")
#  pragma comment(lib, "shell32.lib")
#endif

namespace Otter
{
    class Layer;
    struct MouseEvent;

    class OtterChromeNode : public IBrowserNode
    {
    public:
        using QueryHandler = std::function<void(const std::wstring&)>;

        static int execute_subprocess(HINSTANCE instance);
        static bool initialize(HINSTANCE instance);
        static void shutdown();
        static void use_shared_texture(bool enable);

        explicit OtterChromeNode(HWND parent);
        ~OtterChromeNode();

        OtterChromeNode(const OtterChromeNode&) = delete;
        OtterChromeNode& operator=(const OtterChromeNode&) = delete;

        void set_query_handler(QueryHandler handler);
        void show(bool visible);
        void set_bounds(int x, int y, int width, int height);
        void load_url(const std::wstring& url);
        void load_file(const std::wstring& path);
        void load_html(const std::wstring& html, const std::wstring& virtual_url = L"https://otter.local/");
        void evaluate_script(const std::wstring& script, const std::wstring& url = L"https://otter.local/script");
        void set_json_state(const std::wstring& json);

    private:
        class Impl;
        Impl* impl_ = nullptr;
    };

    class OtterChromeLayer : public IBrowserLayer
    {
    public:
        using QueryHandler = OtterChromeNode::QueryHandler;
        using RegionHitTest = std::function<bool(float local_x, float local_y)>;

        explicit OtterChromeLayer(Layer* layer, void* parent_window = nullptr);
        ~OtterChromeLayer();

        OtterChromeLayer(const OtterChromeLayer&) = delete;
        OtterChromeLayer& operator=(const OtterChromeLayer&) = delete;

        static OtterChromeLayer* attach(Layer* layer, int width, int height, void* parent_window = nullptr);

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
