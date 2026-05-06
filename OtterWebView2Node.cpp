#include "OtterWebView2Node.h"

#include "../OtterLayer.h"

#include <WebView2.h>
#include <wrl.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace
{
    bool g_com_initialized = false;

    void webview2_log(const char* message)
    {
        std::ofstream out("otter_webview2.log", std::ios::out | std::ios::app);
        if (out.is_open())
            out << message << "\n";
    }

    void webview2_log_hr(const char* prefix, HRESULT hr)
    {
        char buffer[160]{};
        std::snprintf(buffer, sizeof(buffer), "%s hr=0x%08X", prefix, static_cast<unsigned int>(hr));
        webview2_log(buffer);
    }

    std::wstring normalize_url(std::wstring url)
    {
        if (url.find(L"://") != std::wstring::npos || url.rfind(L"about:", 0) == 0)
            return url;
        return L"https://" + url;
    }

    std::wstring file_url(const std::wstring& path)
    {
        std::wstring result = L"file:///";
        std::wstring full = std::filesystem::absolute(path).wstring();
        for (wchar_t ch : full)
        {
            if (ch == L'\\')
                result.push_back(L'/');
            else if (ch == L' ')
                result += L"%20";
            else
                result.push_back(ch);
        }
        return result;
    }

    std::wstring escape_js_string(std::wstring_view value)
    {
        std::wstring out;
        out.reserve(value.size() + 16);
        for (wchar_t ch : value)
        {
            switch (ch)
            {
            case L'\\': out += L"\\\\"; break;
            case L'\'': out += L"\\'"; break;
            case L'\r': out += L"\\r"; break;
            case L'\n': out += L"\\n"; break;
            case L'\t': out += L"\\t"; break;
            default: out.push_back(ch); break;
            }
        }
        return out;
    }

    void destroy_webview2_layer_component(void* value)
    {
        delete static_cast<Otter::OtterWebView2Layer*>(value);
    }
}

namespace Otter
{
    class OtterWebView2Node::Impl
    {
    public:
        explicit Impl(HWND parent)
            : parent_(parent)
        {
            if (parent_)
                SetWindowLongPtrW(parent_, GWL_STYLE, GetWindowLongPtrW(parent_, GWL_STYLE) | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
            create();
        }

        ~Impl()
        {
            if (controller_)
                controller_->Close();
            if (host_)
            {
                DestroyWindow(host_);
                host_ = nullptr;
            }
        }

        void set_query_handler(QueryHandler handler)
        {
            handler_ = std::move(handler);
        }

        void show(bool visible)
        {
            visible_ = visible;
            if (host_)
                ShowWindow(host_, visible ? SW_SHOW : SW_HIDE);
            if (controller_)
                controller_->put_IsVisible(visible ? TRUE : FALSE);
        }

        void set_bounds(int x, int y, int width, int height)
        {
            bounds_.left = x;
            bounds_.top = y;
            bounds_.right = x + width;
            bounds_.bottom = y + height;
            if (host_)
            {
                MoveWindow(host_, x, y, width, height, TRUE);
                SetWindowPos(host_, HWND_TOP, x, y, width, height, SWP_SHOWWINDOW);
            }
            if (controller_)
            {
                RECT child_bounds{ 0, 0, width, height };
                controller_->put_Bounds(child_bounds);
            }
        }

        void load_url(const std::wstring& url)
        {
            pending_html_.clear();
            pending_url_ = normalize_url(url);
            if (webview_)
                webview_->Navigate(pending_url_.c_str());
        }

        void load_file(const std::wstring& path)
        {
            load_url(file_url(path));
        }

        void load_html(const std::wstring& html, const std::wstring&)
        {
            pending_url_.clear();
            pending_html_ = html;
            if (webview_)
                webview_->NavigateToString(pending_html_.c_str());
        }

        void evaluate_script(const std::wstring& script, const std::wstring&)
        {
            if (webview_)
                webview_->ExecuteScript(script.c_str(), nullptr);
        }

        void set_json_state(const std::wstring& json)
        {
            std::wstring script = L"window.__otterState = " + json +
                L"; window.dispatchEvent(new CustomEvent('otter-state', { detail: window.__otterState }));";
            evaluate_script(script, L"");
        }

        void focus(bool focused)
        {
            if (focused && controller_)
                controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        }

    private:
        void create()
        {
            if (!parent_)
            {
                webview2_log("create skipped: parent hwnd is null");
                return;
            }

            if (!IsWindow(parent_))
            {
                webview2_log("create skipped: parent hwnd is invalid");
                return;
            }

            create_host_window();
            if (!host_)
                return;

            std::filesystem::path user_data = std::filesystem::absolute("webview2_cache");
            std::filesystem::create_directories(user_data);
            webview2_log("CreateCoreWebView2EnvironmentWithOptions enter");

            HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
                nullptr,
                user_data.c_str(),
                nullptr,
                Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                    [this](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT
                    {
                        webview2_log_hr("environment callback", result);
                        if (FAILED(result) || !environment)
                            return result;
                        environment_ = environment;
                        return environment_->CreateCoreWebView2Controller(
                            host_,
                            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                [this](HRESULT controller_result, ICoreWebView2Controller* controller) -> HRESULT
                                {
                                    webview2_log_hr("controller callback", controller_result);
                                    if (FAILED(controller_result) || !controller)
                                        return controller_result;

                                    controller_ = controller;
                                    controller_->get_CoreWebView2(&webview_);
                                    RECT child_bounds{
                                        0,
                                        0,
                                        std::max<LONG>(1, bounds_.right - bounds_.left),
                                        std::max<LONG>(1, bounds_.bottom - bounds_.top)
                                    };
                                    controller_->put_Bounds(child_bounds);
                                    controller_->put_IsVisible(visible_ ? TRUE : FALSE);

                                    ComPtr<ICoreWebView2Settings> settings;
                                    if (webview_ && SUCCEEDED(webview_->get_Settings(&settings)) && settings)
                                    {
                                        settings->put_IsScriptEnabled(TRUE);
                                        settings->put_AreDefaultScriptDialogsEnabled(TRUE);
                                        settings->put_IsWebMessageEnabled(TRUE);
                                    }

                                    if (webview_)
                                    {
                                        EventRegistrationToken token{};
                                        webview_->add_WebMessageReceived(
                                            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                                [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT
                                                {
                                                    if (!handler_ || !args)
                                                        return S_OK;
                                                    LPWSTR json = nullptr;
                                                    if (SUCCEEDED(args->get_WebMessageAsJson(&json)) && json)
                                                    {
                                                        handler_(json);
                                                        CoTaskMemFree(json);
                                                    }
                                                    return S_OK;
                                                }).Get(),
                                            &token);

                                        webview_->add_NewWindowRequested(
                                            Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                                [this](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT
                                                {
                                                    if (!args)
                                                        return S_OK;

                                                    LPWSTR uri = nullptr;
                                                    if (SUCCEEDED(args->get_Uri(&uri)) && uri)
                                                    {
                                                        webview2_log("NewWindowRequested -> navigate current view");
                                                        args->put_Handled(TRUE);
                                                        webview_->Navigate(uri);
                                                        CoTaskMemFree(uri);
                                                        return S_OK;
                                                    }

                                                    webview2_log("NewWindowRequested -> handled without uri");
                                                    args->put_Handled(TRUE);
                                                    return S_OK;
                                                }).Get(),
                                            &token);

                                        if (!pending_html_.empty())
                                        {
                                            webview2_log("NavigateToString pending html");
                                            webview_->NavigateToString(pending_html_.c_str());
                                        }
                                        else if (!pending_url_.empty())
                                        {
                                            webview2_log("Navigate pending url");
                                            webview_->Navigate(pending_url_.c_str());
                                        }
                                    }
                                    webview2_log("controller ready");
                                    return S_OK;
                                }).Get());
                    }).Get());

            webview2_log_hr("CreateCoreWebView2EnvironmentWithOptions returned", hr);
            (void)hr;
        }

        void create_host_window()
        {
            if (host_)
                return;

            host_ = CreateWindowExW(
                0,
                L"STATIC",
                nullptr,
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                bounds_.left,
                bounds_.top,
                std::max<LONG>(1, bounds_.right - bounds_.left),
                std::max<LONG>(1, bounds_.bottom - bounds_.top),
                parent_,
                nullptr,
                GetModuleHandleW(nullptr),
                nullptr);
            if (!host_)
            {
                char buffer[160]{};
                std::snprintf(buffer, sizeof(buffer), "CreateWindowExW host failed gle=%lu", GetLastError());
                webview2_log(buffer);
                return;
            }
            SetWindowPos(host_, HWND_TOP, bounds_.left, bounds_.top,
                         std::max<LONG>(1, bounds_.right - bounds_.left),
                         std::max<LONG>(1, bounds_.bottom - bounds_.top),
                         SWP_SHOWWINDOW);
            webview2_log("host window created");
        }

        HWND parent_ = nullptr;
        HWND host_ = nullptr;
        RECT bounds_{ 0, 0, 1, 1 };
        bool visible_ = true;
        QueryHandler handler_;
        std::wstring pending_url_;
        std::wstring pending_html_;
        ComPtr<ICoreWebView2Environment> environment_;
        ComPtr<ICoreWebView2Controller> controller_;
        ComPtr<ICoreWebView2> webview_;
    };

    int OtterWebView2Node::execute_subprocess(HINSTANCE) { return -1; }

    bool OtterWebView2Node::initialize(HINSTANCE)
    {
        HRESULT hr = OleInitialize(nullptr);
        if (SUCCEEDED(hr))
            g_com_initialized = true;
        return SUCCEEDED(hr) || hr == S_FALSE || hr == RPC_E_CHANGED_MODE;
    }

    void OtterWebView2Node::shutdown()
    {
        if (g_com_initialized)
        {
            OleUninitialize();
            g_com_initialized = false;
        }
    }

    void OtterWebView2Node::use_shared_texture(bool) {}

    OtterWebView2Node::OtterWebView2Node(HWND parent) : impl_(new Impl(parent)) {}
    OtterWebView2Node::~OtterWebView2Node() { delete impl_; }
    void OtterWebView2Node::set_query_handler(QueryHandler handler) { impl_->set_query_handler(std::move(handler)); }
    void OtterWebView2Node::show(bool visible) { impl_->show(visible); }
    void OtterWebView2Node::set_bounds(int x, int y, int width, int height) { impl_->set_bounds(x, y, width, height); }
    void OtterWebView2Node::load_url(const std::wstring& url) { impl_->load_url(url); }
    void OtterWebView2Node::load_file(const std::wstring& path) { impl_->load_file(path); }
    void OtterWebView2Node::load_html(const std::wstring& html, const std::wstring& virtual_url) { impl_->load_html(html, virtual_url); }
    void OtterWebView2Node::evaluate_script(const std::wstring& script, const std::wstring& url) { impl_->evaluate_script(script, url); }
    void OtterWebView2Node::set_json_state(const std::wstring& json) { impl_->set_json_state(json); }
    void OtterWebView2Node::focus(bool focused) { impl_->focus(focused); }

    class OtterWebView2Layer::Impl
    {
    public:
        explicit Impl(Layer*, void* parent_window)
            : node_(static_cast<HWND>(parent_window))
        {}

        void resize(int width, int height)
        {
            width_ = width;
            height_ = height;
            node_.set_bounds(0, 0, width, height);
        }

        void set_query_handler(QueryHandler handler) { node_.set_query_handler(std::move(handler)); }
        void load_url(const std::wstring& url) { node_.load_url(url); }
        void load_file(const std::wstring& path) { node_.load_file(path); }
        void load_html(const std::wstring& html, const std::wstring& virtual_url) { node_.load_html(html, virtual_url); }
        void evaluate_script(const std::wstring& script, const std::wstring& url) { node_.evaluate_script(script, url); }
        void set_json_state(const std::wstring& json) { node_.set_json_state(json); }
        void focus(bool focused) { node_.focus(focused); }
        int width() const { return width_; }
        int height() const { return height_; }

    private:
        OtterWebView2Node node_;
        int width_ = 1;
        int height_ = 1;
    };

    OtterWebView2Layer::OtterWebView2Layer(Layer* layer, void* parent_window)
        : impl_(std::make_unique<Impl>(layer, parent_window))
    {}

    OtterWebView2Layer::~OtterWebView2Layer() = default;

    OtterWebView2Layer* OtterWebView2Layer::attach(Layer* layer, int width, int height, void* parent_window)
    {
        if (!layer) return nullptr;
        auto* existing = layer->native_component<OtterWebView2Layer>();
        if (existing)
        {
            existing->resize(width, height);
            return existing;
        }

        auto* webview = new OtterWebView2Layer(layer, parent_window);
        webview->resize(width, height);
        layer->set_native_component(webview, destroy_webview2_layer_component)
             .layer_bounds(0.f, 0.f, static_cast<float>(width), static_cast<float>(height))
             .hit_area(0.f, 0.f, static_cast<float>(width), static_cast<float>(height))
             .hit_test([webview](float x, float y) { return webview->hit_test(x, y); });
        return webview;
    }

    void OtterWebView2Layer::resize(int width, int height) { impl_->resize(width, height); }
    void OtterWebView2Layer::set_query_handler(QueryHandler handler) { impl_->set_query_handler(std::move(handler)); }
    void OtterWebView2Layer::load_url(const std::wstring& url) { impl_->load_url(url); }
    void OtterWebView2Layer::load_file(const std::wstring& path) { impl_->load_file(path); }
    void OtterWebView2Layer::load_html(const std::wstring& html, const std::wstring& virtual_url) { impl_->load_html(html, virtual_url); }
    void OtterWebView2Layer::evaluate_script(const std::wstring& script, const std::wstring& url) { impl_->evaluate_script(script, url); }
    void OtterWebView2Layer::set_json_state(const std::wstring& json) { impl_->set_json_state(json); }
    void OtterWebView2Layer::transparent_hit_test(bool, uint8_t) {}
    void OtterWebView2Layer::set_region_hit_test(RegionHitTest) {}
    void OtterWebView2Layer::focus(bool focused) { impl_->focus(focused); }
    bool OtterWebView2Layer::hit_test(float, float) const { return true; }
    bool OtterWebView2Layer::send_mouse_move(const MouseEvent&) { return false; }
    bool OtterWebView2Layer::send_mouse_down(const MouseEvent&) { return false; }
    bool OtterWebView2Layer::send_mouse_up(const MouseEvent&) { return false; }
    bool OtterWebView2Layer::send_wheel(const MouseEvent&) { return false; }
    void OtterWebView2Layer::send_key_down(WPARAM, LPARAM) { impl_->focus(true); }
    void OtterWebView2Layer::send_key_char(WPARAM, LPARAM) { impl_->focus(true); }
    void OtterWebView2Layer::send_key_up(WPARAM, LPARAM) { impl_->focus(true); }
    const uint8_t* OtterWebView2Layer::pixels() const { return nullptr; }
    int OtterWebView2Layer::width() const { return impl_->width(); }
    int OtterWebView2Layer::height() const { return impl_->height(); }
    int OtterWebView2Layer::stride() const { return 0; }
}
