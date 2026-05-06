#include "OtterChromeNode.h"

#if !defined(OTTER_DISABLE_CEF)

#include "../OtterLayer.h"

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/cef_display_handler.h"
#include "include/cef_drag_handler.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_keyboard_handler.h"
#include "include/cef_render_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_request_handler.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_helpers.h"
#include "include/wrapper/cef_message_router.h"

#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shellapi.h>
#include <string>
#include <cstring>
#include <utility>
#include <vector>

namespace Otter
{
namespace
{
    std::atomic_bool g_initialized = false;
    std::atomic_bool g_ole_initialized = false;
    std::atomic_bool g_use_shared_texture = true;
    CefRefPtr<CefApp> g_cef_app;
    std::atomic_uint64_t g_dynamic_bitmap_key = 1;

    template <typename T>
    void safe_release(T*& value)
    {
        if (value)
        {
            value->Release();
            value = nullptr;
        }
    }

    struct ChromeFrameSnapshot
    {
        std::vector<uint8_t> pixels;
        std::vector<BitmapUpdateRect> dirty_rects;
        int width = 0;
        int height = 0;
        int stride = 0;
        uint64_t revision = 0;
    };

    void chrome_trace(const char* message)
    {
        std::ofstream out("otter_cef_trace.log", std::ios::out | std::ios::app);
        if (out.is_open())
            out << message << "\n";
    }

    void chrome_tracef(const char* prefix, HRESULT hr)
    {
        char buffer[160]{};
        std::snprintf(buffer, sizeof(buffer), "%s hr=0x%08X", prefix, static_cast<unsigned int>(hr));
        chrome_trace(buffer);
    }

    class NativeDropTarget final : public IDropTarget
    {
    public:
        explicit NativeDropTarget(OtterChromeNode::QueryHandler handler) : handler_(std::move(handler)) {}

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
        {
            if (!object) return E_POINTER;
            if (riid == IID_IUnknown || riid == IID_IDropTarget) {
                *object = static_cast<IDropTarget*>(this);
                AddRef();
                return S_OK;
            }
            *object = nullptr;
            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
        ULONG STDMETHODCALLTYPE Release() override
        {
            ULONG refs = (ULONG)InterlockedDecrement(&refs_);
            if (!refs) delete this;
            return refs;
        }

        HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data_object, DWORD, POINTL, DWORD* effect) override
        {
            accepts_files_ = has_files(data_object);
            if (effect) *effect = accepts_files_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override
        {
            if (effect) *effect = accepts_files_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }

        HRESULT STDMETHODCALLTYPE Drop(IDataObject* data_object, DWORD, POINTL, DWORD* effect) override
        {
            if (effect) *effect = DROPEFFECT_NONE;
            std::vector<std::wstring> paths = read_paths(data_object);
            send_paths(paths);
            if (effect) *effect = DROPEFFECT_COPY;
            return S_OK;
        }

        void set_handler(OtterChromeNode::QueryHandler handler) { handler_ = std::move(handler); }

        void handle_hdrop(HDROP drop)
        {
            if (!drop) return;
            std::vector<std::wstring> paths;
            UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            paths.reserve(count);
            for (UINT index = 0; index < count; ++index) {
                UINT length = DragQueryFileW(drop, index, nullptr, 0);
                if (!length) continue;
                std::wstring path(length + 1, L'\0');
                DragQueryFileW(drop, index, path.data(), length + 1);
                path.resize(length);
                paths.push_back(std::move(path));
            }
            send_paths(paths);
        }

    private:
        void send_paths(const std::vector<std::wstring>& paths)
        {
            if (paths.empty()) return;
            std::wstring request = L"files";
            for (const auto& path : paths) {
                if (!path.empty()) request += L"\n" + path;
            }
            if (handler_ && request.size() > 5) handler_(request);
        }

        static bool has_files(IDataObject* data_object)
        {
            if (!data_object) return false;
            FORMATETC format = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
            return data_object->QueryGetData(&format) == S_OK;
        }

        static std::vector<std::wstring> read_paths(IDataObject* data_object)
        {
            std::vector<std::wstring> paths;
            if (!data_object) return paths;
            FORMATETC format = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
            STGMEDIUM medium = {};
            if (FAILED(data_object->GetData(&format, &medium))) return paths;
            HDROP drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
            if (drop) {
                UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                paths.reserve(count);
                for (UINT index = 0; index < count; ++index) {
                    UINT length = DragQueryFileW(drop, index, nullptr, 0);
                    if (!length) continue;
                    std::wstring path(length + 1, L'\0');
                    DragQueryFileW(drop, index, path.data(), length + 1);
                    path.resize(length);
                    paths.push_back(std::move(path));
                }
                GlobalUnlock(medium.hGlobal);
            }
            ReleaseStgMedium(&medium);
            return paths;
        }

        LONG refs_ = 1;
        bool accepts_files_ = false;
        OtterChromeNode::QueryHandler handler_;
    };

    const wchar_t* kDropTargetProp = L"OtterChromeDropTarget";
    const wchar_t* kDropOldProcProp = L"OtterChromeOldWndProc";

    struct RegisteredDropWindow
    {
        HWND hwnd = nullptr;
        WNDPROC old_proc = nullptr;
        bool ole_registered = false;
    };

    LRESULT CALLBACK chrome_drop_wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_DROPFILES) {
            auto* target = reinterpret_cast<NativeDropTarget*>(GetPropW(hwnd, kDropTargetProp));
            if (target) target->handle_hdrop(reinterpret_cast<HDROP>(wparam));
            DragFinish(reinterpret_cast<HDROP>(wparam));
            return 0;
        }

        auto old_proc = reinterpret_cast<WNDPROC>(GetPropW(hwnd, kDropOldProcProp));
        return old_proc ? CallWindowProcW(old_proc, hwnd, message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    std::wstring html_shell()
    {
        return L""; // Not used anymore
    }

    std::string utf8(std::wstring_view text)
    {
        if (text.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
        std::string out(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), n, nullptr, nullptr);
        return out;
    }

    class D3D11SharedTextureReader
    {
    public:
        ~D3D11SharedTextureReader()
        {
            safe_release(staging_);
            safe_release(context_);
            safe_release(device_);
        }

        bool read(HANDLE shared_handle, int fallback_width, int fallback_height, std::vector<uint8_t>& pixels, int& width, int& height, int& stride)
        {
            if (!shared_handle || !ensure_device())
                return false;

            ID3D11Texture2D* shared_texture = nullptr;
            HRESULT hr = open_shared_texture(shared_handle, &shared_texture);
            if (FAILED(hr) || !shared_texture)
                hr = reopen_on_matching_adapter(shared_handle, &shared_texture);
            if (FAILED(hr) || !shared_texture)
            {
                static std::atomic_int open_logs = 0;
                if (open_logs.fetch_add(1) < 8)
                    chrome_tracef("osr shared texture open failed", hr);
                return false;
            }

            D3D11_TEXTURE2D_DESC desc{};
            shared_texture->GetDesc(&desc);
            width = desc.Width > 0 ? static_cast<int>(desc.Width) : fallback_width;
            height = desc.Height > 0 ? static_cast<int>(desc.Height) : fallback_height;
            if (width <= 0 || height <= 0)
            {
                shared_texture->Release();
                return false;
            }

            ensure_staging(desc);
            if (!staging_)
            {
                shared_texture->Release();
                return false;
            }

            context_->CopyResource(staging_, shared_texture);
            shared_texture->Release();

            D3D11_MAPPED_SUBRESOURCE mapped{};
            hr = context_->Map(staging_, 0, D3D11_MAP_READ, 0, &mapped);
            if (FAILED(hr))
            {
                static std::atomic_int map_logs = 0;
                if (map_logs.fetch_add(1) < 8)
                    chrome_tracef("osr shared texture map failed", hr);
                return false;
            }

            stride = width * 4;
            pixels.resize(static_cast<size_t>(stride) * static_cast<size_t>(height));
            const auto* src = static_cast<const uint8_t*>(mapped.pData);
            for (int y = 0; y < height; ++y)
            {
                std::memcpy(pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(stride),
                            src + static_cast<size_t>(y) * static_cast<size_t>(mapped.RowPitch),
                            static_cast<size_t>(stride));
            }
            context_->Unmap(staging_, 0);
            return true;
        }

    private:
        bool ensure_device()
        {
            if (device_ && context_)
                return true;
            D3D_FEATURE_LEVEL levels[] = {
                D3D_FEATURE_LEVEL_11_1,
                D3D_FEATURE_LEVEL_11_0,
                D3D_FEATURE_LEVEL_10_1,
                D3D_FEATURE_LEVEL_10_0
            };
            D3D_FEATURE_LEVEL created_level{};
            UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
            HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                           levels, static_cast<UINT>(std::size(levels)),
                                           D3D11_SDK_VERSION, &device_, &created_level, &context_);
            (void)created_level;
            return SUCCEEDED(hr) && device_ && context_;
        }

        HRESULT open_shared_texture(HANDLE shared_handle, ID3D11Texture2D** texture)
        {
            if (!device_ || !texture)
                return E_POINTER;

            HRESULT hr = device_->OpenSharedResource(shared_handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture));
            if (SUCCEEDED(hr) && *texture)
                return hr;

            ID3D11Device1* device1 = nullptr;
            if (SUCCEEDED(device_->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1))) && device1)
            {
                hr = device1->OpenSharedResource1(shared_handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture));
                device1->Release();
            }
            return hr;
        }

        HRESULT reopen_on_matching_adapter(HANDLE shared_handle, ID3D11Texture2D** texture)
        {
            IDXGIFactory1* factory = nullptr;
            HRESULT last_hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
            if (FAILED(last_hr) || !factory)
                return last_hr;

            for (UINT index = 0; ; ++index)
            {
                IDXGIAdapter1* adapter = nullptr;
                HRESULT enum_hr = factory->EnumAdapters1(index, &adapter);
                if (enum_hr == DXGI_ERROR_NOT_FOUND)
                    break;
                if (FAILED(enum_hr) || !adapter)
                {
                    last_hr = enum_hr;
                    continue;
                }

                ID3D11Device* test_device = nullptr;
                ID3D11DeviceContext* test_context = nullptr;
                if (create_device(adapter, &test_device, &test_context))
                {
                    safe_release(device_);
                    safe_release(context_);
                    safe_release(staging_);
                    device_ = test_device;
                    context_ = test_context;
                    last_hr = open_shared_texture(shared_handle, texture);
                    if (SUCCEEDED(last_hr) && *texture)
                    {
                        adapter->Release();
                        factory->Release();
                        return last_hr;
                    }
                }
                adapter->Release();
            }

            factory->Release();
            return last_hr;
        }

        static bool create_device(IDXGIAdapter* adapter, ID3D11Device** device, ID3D11DeviceContext** context)
        {
            D3D_FEATURE_LEVEL levels[] = {
                D3D_FEATURE_LEVEL_11_1,
                D3D_FEATURE_LEVEL_11_0,
                D3D_FEATURE_LEVEL_10_1,
                D3D_FEATURE_LEVEL_10_0
            };
            D3D_FEATURE_LEVEL created_level{};
            const D3D_DRIVER_TYPE type = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
            HRESULT hr = D3D11CreateDevice(adapter, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                           levels, static_cast<UINT>(std::size(levels)),
                                           D3D11_SDK_VERSION, device, &created_level, context);
            (void)created_level;
            return SUCCEEDED(hr) && *device && *context;
        }

        void ensure_staging(const D3D11_TEXTURE2D_DESC& src_desc)
        {
            if (staging_ &&
                staging_desc_.Width == src_desc.Width &&
                staging_desc_.Height == src_desc.Height &&
                staging_desc_.Format == src_desc.Format)
                return;

            safe_release(staging_);
            staging_desc_ = src_desc;
            staging_desc_.MipLevels = 1;
            staging_desc_.ArraySize = 1;
            staging_desc_.SampleDesc.Count = 1;
            staging_desc_.SampleDesc.Quality = 0;
            staging_desc_.Usage = D3D11_USAGE_STAGING;
            staging_desc_.BindFlags = 0;
            staging_desc_.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging_desc_.MiscFlags = 0;
            device_->CreateTexture2D(&staging_desc_, nullptr, &staging_);
        }

        ID3D11Device* device_ = nullptr;
        ID3D11DeviceContext* context_ = nullptr;
        ID3D11Texture2D* staging_ = nullptr;
        D3D11_TEXTURE2D_DESC staging_desc_{};
    };

    std::wstring file_url(const std::wstring& path)
    {
        std::wstring normalized = path;
        std::replace(normalized.begin(), normalized.end(), L'\\', L'/');
        std::string bytes = utf8(normalized);
        static const wchar_t* hex = L"0123456789ABCDEF";
        std::wstring out = L"file:///";
        for (unsigned char ch : bytes) {
            bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/' || ch == ':';
            if (safe) {
                out.push_back(static_cast<wchar_t>(ch));
            } else {
                out.push_back(L'%');
                out.push_back(hex[ch >> 4]);
                out.push_back(hex[ch & 15]);
            }
        }
        return out;
    }

    class ClosureTask final : public CefTask
    {
    public:
        explicit ClosureTask(std::function<void()> fn) : fn_(std::move(fn)) {}
        void Execute() override { if (fn_) fn_(); }
    private:
        std::function<void()> fn_;
        IMPLEMENT_REFCOUNTING(ClosureTask);
    };

    void post_cef_ui(std::function<void()> fn)
    {
        if (CefCurrentlyOn(TID_UI))
            fn();
        else
            CefPostTask(TID_UI, new ClosureTask(std::move(fn)));
    }

    class ChromeApp : public CefApp
    {
    public:
        void OnBeforeCommandLineProcessing(const CefString&, CefRefPtr<CefCommandLine> command_line) override
        {
            command_line->AppendSwitch("allow-file-access-from-files");
            command_line->AppendSwitch("autoplay-policy=no-user-gesture-required");
            command_line->AppendSwitch("disable-web-security");
            command_line->AppendSwitch("enable-media-stream");
            command_line->AppendSwitch("enable-system-flash=false");
            command_line->AppendSwitch("enable-widevine-cdm");
            command_line->AppendSwitch("use-fake-ui-for-media-stream");
            command_line->AppendSwitchWithValue("disable-features",
                "BlockInsecurePrivateNetworkRequests,IsolateOrigins,site-per-process");
            command_line->AppendSwitchWithValue("enable-features",
                "MediaFoundationVideoCapture,VaapiVideoDecoder,PlatformHEVCDecoderSupport");
            command_line->AppendSwitchWithValue("user-agent",
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                "(KHTML, like Gecko) Chrome/146.0.7680.179 Safari/537.36");
        }

    private:
        IMPLEMENT_REFCOUNTING(ChromeApp);
    };

    class ChromeClient final : public CefClient,
                               public CefLifeSpanHandler,
                               public CefLoadHandler,
                               public CefDisplayHandler,
                               public CefDragHandler,
                               public CefRequestHandler
    {
    public:
        ChromeClient(OtterChromeNode::QueryHandler handler, RECT bounds, bool visible, std::wstring initial_url)
            : handler_(std::move(handler)), bounds_(bounds), visible_(visible), current_url_(std::move(initial_url))
        {}

        CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
        CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
        CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
        CefRefPtr<CefDragHandler> GetDragHandler() override { return this; }
        CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

        bool OnBeforePopup(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame>,
                           int,
                           const CefString& target_url,
                           const CefString&,
                           cef_window_open_disposition_t,
                           bool,
                           const CefPopupFeatures&,
                           CefWindowInfo&,
                           CefRefPtr<CefClient>&,
                           CefBrowserSettings&,
                           CefRefPtr<CefDictionaryValue>&,
                           bool*) override
        {
            chrome_trace("native OnBeforePopup");
            std::wstring url = target_url.ToWString();
            if (!url.empty() && browser)
                browser->GetMainFrame()->LoadURL(url);
            return true;
        }

        bool OnBeforeBrowse(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest> request, bool, bool) override
        {
            if (!request) return false;
            std::wstring url = request->GetURL().ToWString();
            constexpr std::wstring_view prefix = L"https://otter.local/query/";
            if (url.rfind(prefix.data(), 0) == 0)
            {
                chrome_trace("osr query");
                if (handler_) handler_(url.substr(prefix.size()));
                return true;
            }
            return false;
        }

        void OnAfterCreated(CefRefPtr<CefBrowser> browser) override
        {
            CEF_REQUIRE_UI_THREAD();
            browser_ = browser;
            apply_window_state();
            install_drop_target();
            if (!current_url_.empty())
                browser->GetMainFrame()->LoadURL(current_url_);
        }

        bool DoClose(CefRefPtr<CefBrowser>) override { return false; }
        void OnBeforeClose(CefRefPtr<CefBrowser> browser) override
        {
            (void)browser;
            uninstall_drop_target();
            browser_ = nullptr;
        }
        void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int) override
        {
            install_drop_target();
            flush_state();
            if (frame && frame->IsMain())
            {
                frame->ExecuteJavaScript(
                    L"(function(){try{"
                    L"var v=document.createElement('video');"
                    L"console.log('[otter media] mp4=' + v.canPlayType('video/mp4; codecs=\"avc1.42E01E, mp4a.40.2\"') + "
                    L"', webm=' + v.canPlayType('video/webm; codecs=\"vp9, opus\"') + "
                    L"', mse=' + (!!window.MediaSource) + "
                    L"', mse_mp4=' + (!!window.MediaSource && MediaSource.isTypeSupported('video/mp4; codecs=\"avc1.42E01E, mp4a.40.2\"')) + "
                    L"', mse_webm=' + (!!window.MediaSource && MediaSource.isTypeSupported('video/webm; codecs=\"vp9, opus\"')));"
                    L"}catch(e){console.log('[otter media] probe failed ' + e);}})();",
                    frame->GetURL(),
                    0);
            }
        }

        bool OnConsoleMessage(CefRefPtr<CefBrowser>,
                              cef_log_severity_t,
                              const CefString& message,
                              const CefString& source,
                              int line) override
        {
            std::string entry = "console: " + source.ToString() + ":" + std::to_string(line) + " " + message.ToString();
            chrome_trace(entry.c_str());
            return false;
        }

        bool OnDragEnter(CefRefPtr<CefBrowser>, CefRefPtr<CefDragData> dragData, DragOperationsMask) override
        {
            if (!dragData || !dragData->IsFile()) return false;
            std::vector<CefString> paths;
            if (!dragData->GetFilePaths(paths) || paths.empty()) dragData->GetFileNames(paths);
            if (paths.empty()) return true;
            std::wstring request = L"files";
            for (const auto& path : paths) {
                std::wstring value = path.ToWString();
                if (!value.empty()) request += L"\n" + value;
            }
            if (handler_ && request.size() > 5) handler_(request);
            return true;
        }

        void set_handler(OtterChromeNode::QueryHandler handler)
        {
            handler_ = std::move(handler);
            if (drop_target_) drop_target_->set_handler(handler_);
        }

        void set_state(std::wstring json)
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (state_json_ == json) return;
                state_json_ = std::move(json);
            }
            flush_state();
        }

        void load_url(std::wstring url)
        {
            current_url_ = std::move(url);
            if (browser_ && !current_url_.empty())
                browser_->GetMainFrame()->LoadURL(current_url_);
        }

        void load_html(const std::wstring& html, const std::wstring& virtual_url)
        {
            current_url_ = virtual_url;
            if (browser_)
            {
                std::wstring data_url = L"data:text/html;charset=utf-8,";
                std::string bytes = utf8(html);
                static const wchar_t* hex = L"0123456789ABCDEF";
                for (unsigned char ch : bytes) {
                    bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~';
                    if (safe) {
                        data_url.push_back(static_cast<wchar_t>(ch));
                    } else {
                        data_url.push_back(L'%');
                        data_url.push_back(hex[ch >> 4]);
                        data_url.push_back(hex[ch & 15]);
                    }
                }
                browser_->GetMainFrame()->LoadURL(data_url);
            }
        }

        void evaluate_script(const std::wstring& script, const std::wstring& url)
        {
            if (browser_ && !script.empty())
                browser_->GetMainFrame()->ExecuteJavaScript(script, url, 0);
        }

        void set_window_state(RECT bounds, bool visible)
        {
            bounds_ = bounds;
            visible_ = visible;
            apply_window_state();
        }

        CefBrowser* browser() const { return browser_.get(); }

        void close_browser()
        {
            if (browser_)
                browser_->GetHost()->CloseBrowser(true);
        }

    private:
        void apply_window_state()
        {
            if (!browser_) return;
            HWND hwnd = browser_->GetHost()->GetWindowHandle();
            if (!hwnd) return;
            install_drop_target();
            int width = bounds_.right - bounds_.left;
            int height = bounds_.bottom - bounds_.top;
            browser_->GetHost()->WasHidden(!visible_);
            SetWindowPos(hwnd, HWND_TOP, bounds_.left, bounds_.top, width, height, SWP_NOACTIVATE | (visible_ ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
            browser_->GetHost()->WasResized();
            InvalidateRect(hwnd, nullptr, FALSE);
            ShowWindow(hwnd, visible_ ? SW_SHOW : SW_HIDE);
        }

        void install_drop_target()
        {
            if (!browser_) return;
            HWND hwnd = browser_->GetHost()->GetWindowHandle();
            if (!hwnd) return;
            OleInitialize(nullptr);
            if (!drop_target_) drop_target_ = new NativeDropTarget(handler_);
            drop_target_->set_handler(handler_);
            remove_dead_drop_windows();
            register_drop_window(hwnd);
            EnumChildWindows(hwnd, [](HWND child, LPARAM param) -> BOOL {
                reinterpret_cast<ChromeClient*>(param)->register_drop_window(child);
                return TRUE;
            }, reinterpret_cast<LPARAM>(this));
        }

        void uninstall_drop_target()
        {
            for (auto& window : drop_windows_) {
                if (!IsWindow(window.hwnd)) continue;
                if (window.ole_registered) RevokeDragDrop(window.hwnd);
                DragAcceptFiles(window.hwnd, FALSE);
                auto current_proc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window.hwnd, GWLP_WNDPROC));
                if (current_proc == chrome_drop_wnd_proc && window.old_proc) {
                    SetWindowLongPtrW(window.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(window.old_proc));
                }
                RemovePropW(window.hwnd, kDropTargetProp);
                RemovePropW(window.hwnd, kDropOldProcProp);
            }
            drop_windows_.clear();
            if (drop_target_) {
                drop_target_->Release();
                drop_target_ = nullptr;
            }
        }

        void register_drop_window(HWND hwnd)
        {
            if (!hwnd) return;
            auto it = std::find_if(drop_windows_.begin(), drop_windows_.end(), [hwnd](const RegisteredDropWindow& item) {
                return item.hwnd == hwnd;
            });
            if (it != drop_windows_.end()) return;
            WNDPROC old_proc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
            if (!old_proc || old_proc == chrome_drop_wnd_proc) return;
            DragAcceptFiles(hwnd, TRUE);
            SetPropW(hwnd, kDropTargetProp, drop_target_);
            SetPropW(hwnd, kDropOldProcProp, reinterpret_cast<HANDLE>(old_proc));
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(chrome_drop_wnd_proc));
            RevokeDragDrop(hwnd);
            HRESULT hr = RegisterDragDrop(hwnd, drop_target_);
            drop_windows_.push_back({ hwnd, old_proc, SUCCEEDED(hr) });
        }

        void remove_dead_drop_windows()
        {
            drop_windows_.erase(std::remove_if(drop_windows_.begin(), drop_windows_.end(), [](RegisteredDropWindow& window) {
                if (IsWindow(window.hwnd)) return false;
                window.hwnd = nullptr;
                window.old_proc = nullptr;
                window.ole_registered = false;
                return true;
            }), drop_windows_.end());
        }

        void flush_state()
        {
            if (!browser_) return;
            std::wstring json;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                json = state_json_;
            }
            if (!json.empty()) browser_->GetMainFrame()->ExecuteJavaScript(L"setOtterState(" + json + L");", "https://otter.local/state", 0);
        }

        std::mutex mutex_;
        std::wstring state_json_ = L"{}";
        std::wstring current_url_;
        OtterChromeNode::QueryHandler handler_;
        RECT bounds_ = { 0, 0, 1, 1 };
        bool visible_ = true;
        CefRefPtr<CefBrowser> browser_;
        std::vector<RegisteredDropWindow> drop_windows_;
        NativeDropTarget* drop_target_ = nullptr;
        IMPLEMENT_REFCOUNTING(ChromeClient);
    };

    class OffscreenChromeClient final : public CefClient,
                                        public CefLifeSpanHandler,
                                        public CefLoadHandler,
                                        public CefKeyboardHandler,
                                        public CefContextMenuHandler,
                                        public CefDisplayHandler,
                                        public CefRenderHandler,
                                        public CefRequestHandler
    {
    public:
        using FrameCallback = std::function<void(int, int, const void*, int, const CefRenderHandler::RectList&)>;

        OffscreenChromeClient(OtterChromeNode::QueryHandler handler,
                              int width,
                              int height,
                              FrameCallback frame_cb)
            : handler_(std::move(handler))
            , width_(std::max(1, width))
            , height_(std::max(1, height))
            , frame_cb_(std::move(frame_cb))
        {}

        CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
        CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
        CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }
        CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
        CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
        CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
        CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

        bool OnBeforePopup(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame>,
                           int,
                           const CefString& target_url,
                           const CefString&,
                           cef_window_open_disposition_t,
                           bool,
                           const CefPopupFeatures&,
                           CefWindowInfo&,
                           CefRefPtr<CefClient>&,
                           CefBrowserSettings&,
                           CefRefPtr<CefDictionaryValue>&,
                           bool*) override
        {
            chrome_trace("osr OnBeforePopup");
            std::wstring url = target_url.ToWString();
            if (!url.empty() && browser)
                browser->GetMainFrame()->LoadURL(url);
            return true;
        }

        bool OnBeforeBrowse(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest> request, bool, bool) override
        {
            if (!request) return false;
            std::wstring url = request->GetURL().ToWString();
            constexpr std::wstring_view prefix = L"https://otter.local/query/";
            if (url.rfind(prefix.data(), 0) == 0)
            {
                if (handler_) handler_(url.substr(prefix.size()));
                return true;
            }
            return false;
        }

        void OnAfterCreated(CefRefPtr<CefBrowser> browser) override
        {
            CEF_REQUIRE_UI_THREAD();
            chrome_trace("osr OnAfterCreated");
            browser_ = browser;
            browser_->GetHost()->SetWindowlessFrameRate(90);
            if (ready_cb_) ready_cb_();
            if (!current_url_.empty())
            {
                chrome_trace("osr OnAfterCreated load_url");
                browser_->GetMainFrame()->LoadURL(current_url_);
            }
        }

        bool DoClose(CefRefPtr<CefBrowser>) override { return false; }
        void OnBeforeClose(CefRefPtr<CefBrowser> browser) override
        {
            chrome_trace("osr OnBeforeClose");
            (void)browser;
            browser_ = nullptr;
        }
        void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int) override { chrome_trace("osr OnLoadEnd"); }

        void OnBeforeContextMenu(CefRefPtr<CefBrowser>,
                                 CefRefPtr<CefFrame>,
                                 CefRefPtr<CefContextMenuParams>,
                                 CefRefPtr<CefMenuModel> model) override
        {
            if (model) model->Clear();
        }

        bool OnPreKeyEvent(CefRefPtr<CefBrowser>,
                           const CefKeyEvent&,
                           CefEventHandle,
                           bool* is_keyboard_shortcut) override
        {
            if (is_keyboard_shortcut) *is_keyboard_shortcut = false;
            return false;
        }

        bool OnKeyEvent(CefRefPtr<CefBrowser>, const CefKeyEvent&, CefEventHandle) override
        {
            return false;
        }

        bool OnConsoleMessage(CefRefPtr<CefBrowser>,
                              cef_log_severity_t,
                              const CefString& message,
                              const CefString& source,
                              int line) override
        {
            std::string entry = "console: " + source.ToString() + ":" + std::to_string(line) + " " + message.ToString();
            chrome_trace(entry.c_str());
            return false;
        }

        void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override
        {
            rect = CefRect(0, 0, width_, height_);
        }

        void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type,
                     const RectList& dirty_rects, const void* buffer, int width, int height) override
        {
            static std::atomic_int paint_logs = 0;
            if (paint_logs.fetch_add(1) < 6)
                chrome_trace("osr OnPaint");
            if (type != PET_VIEW || !buffer || width <= 0 || height <= 0)
                return;
            if (frame_cb_)
                frame_cb_(width, height, buffer, width * 4, dirty_rects);
        }

        void OnAcceleratedPaint(CefRefPtr<CefBrowser>,
                                PaintElementType type,
                                const RectList& dirty_rects,
                                const CefAcceleratedPaintInfo& info) override
        {
            static std::atomic_int accelerated_logs = 0;
            if (accelerated_logs.fetch_add(1) < 6)
                chrome_trace("osr OnAcceleratedPaint");
            if (type != PET_VIEW || !frame_cb_)
                return;

            std::vector<uint8_t> pixels;
            int frame_width = info.extra.coded_size.width > 0 ? info.extra.coded_size.width : width_;
            int frame_height = info.extra.coded_size.height > 0 ? info.extra.coded_size.height : height_;
            int stride = 0;
            if (!shared_reader_.read(info.shared_texture_handle, frame_width, frame_height, pixels, frame_width, frame_height, stride))
            {
                chrome_trace("osr shared texture read failed");
                return;
            }
            frame_cb_(frame_width, frame_height, pixels.data(), stride, dirty_rects);
        }

        void set_handler(OtterChromeNode::QueryHandler handler) { handler_ = std::move(handler); }

        void resize(int width, int height)
        {
            width_ = std::max(1, width);
            height_ = std::max(1, height);
            if (browser_)
                browser_->GetHost()->WasResized();
        }

        void set_state(std::wstring json)
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (state_json_ == json) return;
                state_json_ = std::move(json);
            }
            flush_state();
        }

        void load_url(std::wstring url)
        {
            chrome_trace("osr client load_url");
            current_url_ = std::move(url);
            if (browser_ && !current_url_.empty())
            {
                chrome_trace("osr client LoadURL now");
                browser_->GetMainFrame()->LoadURL(current_url_);
            }
        }

        void load_html(const std::wstring& html, const std::wstring&)
        {
            chrome_trace("osr client load_html");
            std::wstring data_url = L"data:text/html;charset=utf-8,";
            std::string bytes = utf8(html);
            static const wchar_t* hex = L"0123456789ABCDEF";
            for (unsigned char ch : bytes) {
                bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~';
                if (safe) {
                    data_url.push_back(static_cast<wchar_t>(ch));
                } else {
                    data_url.push_back(L'%');
                    data_url.push_back(hex[ch >> 4]);
                    data_url.push_back(hex[ch & 15]);
                }
            }
            load_url(data_url);
        }

        void evaluate_script(const std::wstring& script, const std::wstring& url)
        {
            if (browser_ && !script.empty())
                browser_->GetMainFrame()->ExecuteJavaScript(script, url, 0);
        }

        void set_focus(bool focused)
        {
            if (browser_) browser_->GetHost()->SetFocus(focused);
        }

        void send_key_event(const CefKeyEvent& event)
        {
            if (!browser_) return;
            browser_->GetHost()->SendKeyEvent(event);
        }

        void send_mouse_move(int x, int y, int modifiers)
        {
            if (!browser_) return;
            CefMouseEvent event;
            event.x = x;
            event.y = y;
            event.modifiers = modifiers;
            browser_->GetHost()->SendMouseMoveEvent(event, false);
        }

        void send_mouse_click(int x, int y, CefBrowserHost::MouseButtonType button,
                              bool mouse_up, int click_count, int modifiers)
        {
            if (!browser_) return;
            CefMouseEvent event;
            event.x = x;
            event.y = y;
            event.modifiers = modifiers;
            browser_->GetHost()->SendMouseClickEvent(event, button, mouse_up, click_count);
        }

        void send_wheel(int x, int y, int delta_x, int delta_y, int modifiers)
        {
            if (!browser_) return;
            CefMouseEvent event;
            event.x = x;
            event.y = y;
            event.modifiers = modifiers;
            browser_->GetHost()->SendMouseWheelEvent(event, delta_x, delta_y);
        }

        CefBrowser* browser() const { return browser_.get(); }

        void close_browser()
        {
            if (browser_)
                browser_->GetHost()->CloseBrowser(true);
        }

        void set_ready_callback(std::function<void()> cb) { ready_cb_ = std::move(cb); }

    private:
        void flush_state()
        {
            if (!browser_) return;
            std::wstring json;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                json = state_json_;
            }
            if (!json.empty()) browser_->GetMainFrame()->ExecuteJavaScript(L"setOtterState(" + json + L");", "https://otter.local/state", 0);
        }

        std::mutex mutex_;
        std::wstring state_json_ = L"{}";
        std::wstring current_url_;
        OtterChromeNode::QueryHandler handler_;
        int width_ = 1;
        int height_ = 1;
        FrameCallback frame_cb_;
        std::function<void()> ready_cb_;
        CefRefPtr<CefBrowser> browser_;
        D3D11SharedTextureReader shared_reader_;
        IMPLEMENT_REFCOUNTING(OffscreenChromeClient);
    };
}

class OtterChromeNode::Impl
{
public:
    explicit Impl(HWND parent) : parent_(parent) {}

    ~Impl()
    {
        state_->alive.store(false);
        auto state = state_;
        post_cef_ui([state]() {
            if (state->client)
            {
                state->client->close_browser();
                state->client = nullptr;
            }
        });
    }

    void set_query_handler(QueryHandler handler)
    {
        handler_ = std::move(handler);
        create_if_needed();
        auto state = state_;
        auto handler_copy = handler_;
        post_cef_ui([state, handler_copy]() mutable {
            if (state->alive.load() && state->client)
                state->client->set_handler(std::move(handler_copy));
        });
    }

    void create_if_needed()
    {
        if (creating_ || !g_initialized) return;
        creating_ = true;
        HWND parent = parent_;
        RECT bounds = bounds_;
        bool visible = visible_;
        std::wstring current_url = current_url_;
        auto handler = handler_;
        auto state = state_;
        if (!state->alive.load() || state->client || !g_initialized) return;
        chrome_trace("native CreateBrowser");
        CefWindowInfo info;
        CefRect rc(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top);
        info.SetAsChild(parent, rc);
        info.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
        CefBrowserSettings settings;
        settings.background_color = CefColorSetARGB(0xFF, 0xFF, 0xFD, 0xF8);
        state->client = new ChromeClient(handler, bounds, visible, current_url);
        CefBrowserHost::CreateBrowser(info, CefRefPtr<CefClient>(state->client), "about:blank", settings, nullptr, nullptr);
        chrome_trace("native CreateBrowser returned");
    }

    void set_bounds(int x, int y, int width, int height)
    {
        RECT next_bounds = { x, y, x + width, y + height };
        if (next_bounds.left == bounds_.left && next_bounds.top == bounds_.top && next_bounds.right == bounds_.right && next_bounds.bottom == bounds_.bottom) return;
        bounds_ = next_bounds;
        create_if_needed();
        auto state = state_;
        bool visible = visible_;
        post_cef_ui([state, next_bounds, visible]() {
            if (state->alive.load() && state->client)
                state->client->set_window_state(next_bounds, visible);
        });
    }

    void show(bool visible)
    {
        if (visible_ == visible) return;
        visible_ = visible;
        create_if_needed();
        auto state = state_;
        RECT bounds = bounds_;
        post_cef_ui([state, bounds, visible]() {
            if (state->alive.load() && state->client)
                state->client->set_window_state(bounds, visible);
        });
    }

    void load_url(const std::wstring& url)
    {
        current_url_ = url;
        create_if_needed();
        auto state = state_;
        auto url_copy = current_url_;
        post_cef_ui([state, url_copy]() {
            if (state->alive.load() && state->client)
                state->client->load_url(url_copy);
        });
    }

    void load_file(const std::wstring& path)
    {
        load_url(file_url(path));
    }

    void load_html(const std::wstring& html, const std::wstring& virtual_url)
    {
        create_if_needed();
        auto state = state_;
        post_cef_ui([state, html, virtual_url]() {
            if (state->alive.load() && state->client)
                state->client->load_html(html, virtual_url);
        });
    }

    void evaluate_script(const std::wstring& script, const std::wstring& url)
    {
        create_if_needed();
        auto state = state_;
        post_cef_ui([state, script, url]() {
            if (state->alive.load() && state->client)
                state->client->evaluate_script(script, url);
        });
    }

    void set_json_state(const std::wstring& json)
    {
        create_if_needed();
        auto state = state_;
        post_cef_ui([state, json]() {
            if (state->alive.load() && state->client)
                state->client->set_state(json);
        });
    }

private:
    struct NativeState
    {
        std::atomic_bool alive = true;
        CefRefPtr<ChromeClient> client;
    };

    HWND parent_ = nullptr;
    std::shared_ptr<NativeState> state_ = std::make_shared<NativeState>();
    RECT bounds_ = { 0, 0, 1, 1 };
    bool visible_ = true;
    std::wstring current_url_;
    QueryHandler handler_;
    bool creating_ = false;
};

class OtterChromeLayer::Impl
{
public:
    explicit Impl(Layer* layer, void* parent_window)
        : layer_(layer), parent_window_(parent_window), cache_key_(g_dynamic_bitmap_key.fetch_add(1))
    {}

    ~Impl()
    {
        state_->alive.store(false);
        auto state = state_;
        post_cef_ui([state]() {
            if (state->client)
            {
                state->client->close_browser();
                state->client = nullptr;
            }
        });
    }

    void resize(int width, int height)
    {
        width_ = std::max(1, width);
        height_ = std::max(1, height);
        if (layer_)
        {
            layer_->layer_bounds(0.f, 0.f, static_cast<float>(width_), static_cast<float>(height_));
            layer_->hit_area(0.f, 0.f, static_cast<float>(width_), static_cast<float>(height_));
        }
        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            frame_snapshot_.reset();
        }
        auto state = state_;
        const int w = width_;
        const int h = height_;
        post_cef_ui([state, w, h]() {
            if (state->alive.load() && state->client)
                state->client->resize(w, h);
        });
    }

    void create_if_needed()
    {
        if (creating_ || !g_initialized) return;
        chrome_trace("osr impl create_if_needed");
        creating_ = true;
        auto* self = this;
        auto state = state_;
        auto handler = handler_;
        auto initial_url = current_url_;
        const int width = width_;
        const int height = height_;
        void* parent_window = parent_window_;
        post_cef_ui([self, state, handler, initial_url, width, height, parent_window]() {
            chrome_trace("osr ui create task");
            if (!state->alive.load() || state->client) return;
            CefWindowInfo info;
            info.SetAsWindowless(static_cast<HWND>(parent_window));
            info.shared_texture_enabled = g_use_shared_texture.load();
            info.external_begin_frame_enabled = false;
            CefBrowserSettings settings;
            settings.background_color = CefColorSetARGB(0, 0, 0, 0);
            settings.windowless_frame_rate = 90;
            state->client = new OffscreenChromeClient(handler, width, height,
                [self, state](int frame_width, int frame_height, const void* buffer, int stride, const CefRenderHandler::RectList& dirty_rects) {
                    if (state->alive.load())
                        self->update_frame(frame_width, frame_height, buffer, stride, dirty_rects);
                });
            if (!initial_url.empty())
                state->client->load_url(initial_url);
            chrome_trace("osr CreateBrowser");
            CefBrowserHost::CreateBrowser(info, CefRefPtr<CefClient>(state->client), "about:blank", settings, nullptr, nullptr);
            chrome_trace("osr CreateBrowser returned");
        });
    }

    void set_query_handler(OtterChromeNode::QueryHandler handler)
    {
        handler_ = std::move(handler);
        auto state = state_;
        auto handler_copy = handler_;
        post_cef_ui([state, handler_copy]() mutable {
            if (state->alive.load() && state->client)
                state->client->set_handler(std::move(handler_copy));
        });
    }

    void load_url(const std::wstring& url)
    {
        current_url_ = url;
        create_if_needed();
        auto state = state_;
        auto url_copy = current_url_;
        post_cef_ui([state, url_copy]() {
            if (state->alive.load() && state->client)
                state->client->load_url(url_copy);
        });
    }

    void load_file(const std::wstring& path)
    {
        load_url(file_url(path));
    }

    void load_html(const std::wstring& html, const std::wstring& virtual_url)
    {
        (void)virtual_url;
        chrome_trace("osr impl load_html");
        std::wstring data_url = L"data:text/html;charset=utf-8,";
        std::string bytes = utf8(html);
        static const wchar_t* hex = L"0123456789ABCDEF";
        for (unsigned char ch : bytes) {
            bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~';
            if (safe) {
                data_url.push_back(static_cast<wchar_t>(ch));
            } else {
                data_url.push_back(L'%');
                data_url.push_back(hex[ch >> 4]);
                data_url.push_back(hex[ch & 15]);
            }
        }
        load_url(data_url);
    }

    void evaluate_script(const std::wstring& script, const std::wstring& url)
    {
        create_if_needed();
        auto state = state_;
        post_cef_ui([state, script, url]() {
            if (state->alive.load() && state->client)
                state->client->evaluate_script(script, url);
        });
    }

    void set_json_state(const std::wstring& json)
    {
        create_if_needed();
        auto state = state_;
        post_cef_ui([state, json]() {
            if (state->alive.load() && state->client)
                state->client->set_state(json);
        });
    }

    void transparent_hit_test(bool enable, uint8_t alpha_threshold)
    {
        transparent_hit_test_ = enable;
        alpha_threshold_ = alpha_threshold;
    }

    void set_region_hit_test(OtterChromeLayer::RegionHitTest hit_test)
    {
        region_hit_test_ = std::move(hit_test);
    }

    void focus(bool focused)
    {
        create_if_needed();
        auto state = state_;
        post_cef_ui([state, focused]() {
            if (state->alive.load() && state->client)
                state->client->set_focus(focused);
        });
    }

    void send_key_down(WPARAM wp, LPARAM lp)
    {
        create_if_needed();
        auto state = state_;
        const int mods = key_modifiers();
        post_cef_ui([state, wp, lp, mods]() {
            if (!state->alive.load() || !state->client) return;
            state->client->set_focus(true);
            CefKeyEvent raw = make_key_event(KEYEVENT_RAWKEYDOWN, wp, lp, mods);
            state->client->send_key_event(raw);
            CefKeyEvent down = make_key_event(KEYEVENT_KEYDOWN, wp, lp, mods);
            state->client->send_key_event(down);
        });
    }

    void send_key_char(WPARAM wp, LPARAM lp)
    {
        create_if_needed();
        auto state = state_;
        const int mods = key_modifiers();
        post_cef_ui([state, wp, lp, mods]() {
            if (!state->alive.load() || !state->client) return;
            CefKeyEvent event = make_key_event(KEYEVENT_CHAR, wp, lp, mods);
            event.character = static_cast<char16_t>(wp);
            event.unmodified_character = static_cast<char16_t>(wp);
            state->client->send_key_event(event);
        });
    }

    void send_key_up(WPARAM wp, LPARAM lp)
    {
        create_if_needed();
        auto state = state_;
        const int mods = key_modifiers();
        post_cef_ui([state, wp, lp, mods]() {
            if (!state->alive.load() || !state->client) return;
            CefKeyEvent event = make_key_event(KEYEVENT_KEYUP, wp, lp, mods);
            state->client->send_key_event(event);
        });
    }

    bool hit_test(float local_x, float local_y) const
    {
        if (local_x < 0.f || local_y < 0.f || local_x >= static_cast<float>(width_) || local_y >= static_cast<float>(height_))
            return false;
        if (region_hit_test_ && !region_hit_test_(local_x, local_y))
            return false;
        if (!transparent_hit_test_)
            return true;
        auto frame = frame_snapshot();
        if (!frame || frame->pixels.empty()) return true;
        int x = std::clamp(static_cast<int>(local_x), 0, frame->width - 1);
        int y = std::clamp(static_cast<int>(local_y), 0, frame->height - 1);
        uint8_t alpha = frame->pixels[static_cast<size_t>(y) * static_cast<size_t>(frame->stride) + static_cast<size_t>(x) * 4u + 3u];
        return alpha > alpha_threshold_;
    }

    bool send_mouse_move(const MouseEvent& e)
    {
        if (!hit_test(e.x, e.y)) return false;
        create_if_needed();
        auto state = state_;
        const int x = static_cast<int>(e.x);
        const int y = static_cast<int>(e.y);
        const int mods = modifiers(e);
        post_cef_ui([state, x, y, mods]() {
            if (state->alive.load() && state->client)
                state->client->send_mouse_move(x, y, mods);
        });
        return true;
    }

    bool send_mouse_down(const MouseEvent& e)
    {
        if (!hit_test(e.x, e.y)) return false;
        create_if_needed();
        last_button_ = button(e);
        auto state = state_;
        const int x = static_cast<int>(e.x);
        const int y = static_cast<int>(e.y);
        const auto btn = last_button_;
        const int clicks = std::max(1, e.click_count);
        const int mods = modifiers(e);
        post_cef_ui([state, x, y, btn, clicks, mods]() {
            if (state->alive.load() && state->client)
            {
                state->client->set_focus(true);
                state->client->send_mouse_click(x, y, btn, false, clicks, mods);
            }
        });
        return true;
    }

    bool send_mouse_up(const MouseEvent& e)
    {
        if (!hit_test(e.x, e.y)) return false;
        create_if_needed();
        auto state = state_;
        const int x = static_cast<int>(e.x);
        const int y = static_cast<int>(e.y);
        const auto btn = last_button_;
        const int clicks = std::max(1, e.click_count);
        const int mods = modifiers(e);
        post_cef_ui([state, x, y, btn, clicks, mods]() {
            if (state->alive.load() && state->client)
                state->client->send_mouse_click(x, y, btn, true, clicks, mods);
        });
        return true;
    }

    bool send_wheel(const MouseEvent& e)
    {
        if (!hit_test(e.x, e.y)) return false;
        create_if_needed();
        auto state = state_;
        const int x = static_cast<int>(e.x);
        const int y = static_cast<int>(e.y);
        const int delta = static_cast<int>(e.wheel_delta * WHEEL_DELTA);
        const int mods = modifiers(e);
        post_cef_ui([state, x, y, delta, mods]() {
            if (state->alive.load() && state->client)
                state->client->send_wheel(x, y, 0, delta, mods);
        });
        return true;
    }

    std::shared_ptr<const ChromeFrameSnapshot> frame_snapshot() const
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        return frame_snapshot_;
    }

    const uint8_t* pixels() const
    {
        auto frame = frame_snapshot();
        return (!frame || frame->pixels.empty()) ? nullptr : frame->pixels.data();
    }
    int width() const { return width_; }
    int height() const { return height_; }
    int stride() const { return stride_; }
    uint64_t cache_key() const { return cache_key_; }

private:
    void update_frame(int width, int height, const void* buffer, int stride, const CefRenderHandler::RectList& dirty_rects)
    {
        static std::atomic_int frame_logs = 0;
        if (frame_logs.fetch_add(1) < 6)
            chrome_trace("osr update_frame");
        if (!buffer || width <= 0 || height <= 0 || stride < width * 4) return;
        auto previous = frame_snapshot();
        auto frame = std::make_shared<ChromeFrameSnapshot>();
        frame->width = width;
        frame->height = height;
        frame->stride = width * 4;
        frame->pixels.resize(static_cast<size_t>(frame->stride) * static_cast<size_t>(frame->height));
        const auto* src = static_cast<const uint8_t*>(buffer);
        if (previous && previous->width == width && previous->height == height && previous->stride == frame->stride)
            frame->pixels = previous->pixels;

        std::vector<BitmapUpdateRect> updates;
        updates.reserve(dirty_rects.size());
        for (const auto& rect : dirty_rects)
        {
            int x0 = std::clamp(rect.x, 0, width);
            int y0 = std::clamp(rect.y, 0, height);
            int x1 = std::clamp(rect.x + rect.width, 0, width);
            int y1 = std::clamp(rect.y + rect.height, 0, height);
            if (x1 <= x0 || y1 <= y0) continue;
            const int copy_width = (x1 - x0) * 4;
            for (int y = y0; y < y1; ++y)
            {
                std::memcpy(frame->pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(frame->stride) + static_cast<size_t>(x0) * 4u,
                            src + static_cast<size_t>(y) * static_cast<size_t>(stride) + static_cast<size_t>(x0) * 4u,
                            static_cast<size_t>(copy_width));
            }
            updates.push_back(BitmapUpdateRect{ x0, y0, x1 - x0, y1 - y0 });
        }
        if (updates.empty())
        {
            for (int y = 0; y < frame->height; ++y)
            {
                std::memcpy(frame->pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(frame->stride),
                            src + static_cast<size_t>(y) * static_cast<size_t>(stride),
                            static_cast<size_t>(frame->stride));
            }
            updates.push_back(BitmapUpdateRect{ 0, 0, width, height });
        }
        frame->dirty_rects = std::move(updates);
        std::lock_guard<std::mutex> lock(frame_mutex_);
        width_ = width;
        height_ = height;
        stride_ = width * 4;
        frame->revision = ++frame_revision_;
        frame_snapshot_ = std::move(frame);
    }

    static int modifiers(const MouseEvent& e)
    {
        int mods = 0;
        if (e.left_down) mods |= EVENTFLAG_LEFT_MOUSE_BUTTON;
        if (e.right_down) mods |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
        if (e.middle_down) mods |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
        if (e.ctrl_down) mods |= EVENTFLAG_CONTROL_DOWN;
        if (e.shift_down) mods |= EVENTFLAG_SHIFT_DOWN;
        if (e.alt_down) mods |= EVENTFLAG_ALT_DOWN;
        return mods;
    }

    static int key_modifiers()
    {
        int mods = 0;
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) mods |= EVENTFLAG_CONTROL_DOWN;
        if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) mods |= EVENTFLAG_SHIFT_DOWN;
        if ((GetKeyState(VK_MENU) & 0x8000) != 0) mods |= EVENTFLAG_ALT_DOWN;
        return mods;
    }

    static CefKeyEvent make_key_event(cef_key_event_type_t type, WPARAM wp, LPARAM lp, int mods)
    {
        CefKeyEvent event{};
        event.size = sizeof(CefKeyEvent);
        event.type = type;
        event.modifiers = mods;
        event.windows_key_code = static_cast<int>(wp);
        event.native_key_code = static_cast<int>(lp);
        event.is_system_key = ((mods & EVENTFLAG_ALT_DOWN) != 0) ? 1 : 0;
        event.focus_on_editable_field = 1;
        return event;
    }

    static CefBrowserHost::MouseButtonType button(const MouseEvent& e)
    {
        if (e.right_down) return MBT_RIGHT;
        if (e.middle_down) return MBT_MIDDLE;
        return MBT_LEFT;
    }

    struct CefState
    {
        std::atomic_bool alive = true;
        OffscreenChromeClient* client = nullptr;
    };

    Layer* layer_ = nullptr;
    void* parent_window_ = nullptr;
    std::shared_ptr<CefState> state_ = std::make_shared<CefState>();
    uint64_t cache_key_ = 0;
    int width_ = 1;
    int height_ = 1;
    int stride_ = 4;
    uint64_t frame_revision_ = 0;
    mutable std::mutex frame_mutex_;
    std::shared_ptr<const ChromeFrameSnapshot> frame_snapshot_;
    bool transparent_hit_test_ = true;
    uint8_t alpha_threshold_ = 8;
    OtterChromeLayer::RegionHitTest region_hit_test_;
    std::wstring current_url_;
    OtterChromeNode::QueryHandler handler_;
    CefBrowserHost::MouseButtonType last_button_ = MBT_LEFT;
    bool creating_ = false;
};

int OtterChromeNode::execute_subprocess(HINSTANCE instance)
{
    CefMainArgs args(instance);
    chrome_trace("execute_subprocess enter");
    if (!g_cef_app)
        g_cef_app = new ChromeApp();
    const int exit_code = CefExecuteProcess(args, g_cef_app, nullptr);
    chrome_trace(exit_code >= 0 ? "execute_subprocess handled" : "execute_subprocess browser");
    return exit_code;
}

bool OtterChromeNode::initialize(HINSTANCE instance)
{
    if (g_initialized) return true;
    chrome_trace("initialize enter");
    CefMainArgs args(instance);
    CefSettings settings;
    settings.no_sandbox = true;
    settings.multi_threaded_message_loop = true;
    settings.windowless_rendering_enabled = true;
    settings.log_severity = LOGSEVERITY_WARNING;
    const std::filesystem::path cef_root = std::filesystem::absolute("cef_cache");
    const std::filesystem::path cef_cache = cef_root / "default";
    const std::filesystem::path cef_log = std::filesystem::absolute("cef_debug.log");
    std::filesystem::create_directories(cef_cache);
    CefString(&settings.root_cache_path).FromWString(cef_root.wstring());
    CefString(&settings.cache_path).FromWString(cef_cache.wstring());
    CefString(&settings.log_file).FromWString(cef_log.wstring());
    settings.background_color = CefColorSetARGB(0, 0xFF, 0xFD, 0xF8);
    if (!g_cef_app)
        g_cef_app = new ChromeApp();
    bool ok = CefInitialize(args, settings, g_cef_app, nullptr);
    g_initialized = ok;
    chrome_trace(ok ? "initialize ok" : "initialize failed");
    return ok;
}

void OtterChromeNode::shutdown()
{
    if (!g_initialized) return;
    chrome_trace("shutdown enter");
    g_initialized = false;
    CefShutdown();
    g_cef_app = nullptr;
    chrome_trace("shutdown exit");
}

void OtterChromeNode::use_shared_texture(bool enable)
{
    g_use_shared_texture = enable;
}

OtterChromeNode::OtterChromeNode(HWND parent) : impl_(new Impl(parent)) {}
OtterChromeNode::~OtterChromeNode() { delete impl_; }
void OtterChromeNode::set_query_handler(QueryHandler handler) { impl_->set_query_handler(std::move(handler)); }
void OtterChromeNode::show(bool visible) { impl_->show(visible); }
void OtterChromeNode::set_bounds(int x, int y, int width, int height) { impl_->set_bounds(x, y, width, height); }
void OtterChromeNode::load_url(const std::wstring& url) { impl_->load_url(url); }
void OtterChromeNode::load_file(const std::wstring& path) { impl_->load_file(path); }
void OtterChromeNode::load_html(const std::wstring& html, const std::wstring& virtual_url) { impl_->load_html(html, virtual_url); }
void OtterChromeNode::evaluate_script(const std::wstring& script, const std::wstring& url) { impl_->evaluate_script(script, url); }
void OtterChromeNode::set_json_state(const std::wstring& json) { impl_->set_json_state(json); }

namespace
{
    struct SharedBgraOp final : PaintOp
    {
        std::shared_ptr<const ChromeFrameSnapshot> frame;
        uint64_t cache_key = 0;
        float opacity = 1.f;

        SharedBgraOp(std::shared_ptr<const ChromeFrameSnapshot> frame, uint64_t cache_key, float opacity)
            : frame(std::move(frame)), cache_key(cache_key), opacity(opacity)
        {}

        void execute(RenderContext& ctx) const override
        {
            if (!frame || frame->pixels.empty()) return;
            ctx.cmd_draw_bitmap_bgra_cached(cache_key, frame->revision,
                                            frame->pixels.data(), frame->width, frame->height, frame->stride,
                                            frame->dirty_rects,
                                            0.f, 0.f, static_cast<float>(frame->width), static_cast<float>(frame->height),
                                            opacity, 0.f);
        }
    };

    void destroy_chrome_layer_component(void* value)
    {
        delete static_cast<OtterChromeLayer*>(value);
    }
}

OtterChromeLayer::OtterChromeLayer(Layer* layer, void* parent_window)
    : impl_(std::make_unique<Impl>(layer, parent_window))
{}

OtterChromeLayer::~OtterChromeLayer() = default;

OtterChromeLayer* OtterChromeLayer::attach(Layer* layer, int width, int height, void* parent_window)
{
    if (!layer) return nullptr;
    auto* existing = layer->native_component<OtterChromeLayer>();
    if (existing)
    {
        existing->resize(width, height);
        return existing;
    }

    auto* chrome = new OtterChromeLayer(layer, parent_window);
    chrome->resize(width, height);
    layer->set_native_component(chrome, destroy_chrome_layer_component)
         .layer_bounds(0.f, 0.f, static_cast<float>(width), static_cast<float>(height))
         .hit_area(0.f, 0.f, static_cast<float>(width), static_cast<float>(height))
         .hit_test([chrome](float x, float y) { return chrome->hit_test(x, y); })
         .on_render([chrome](PaintChain& chain, float) {
             auto frame = chrome->impl_->frame_snapshot();
             if (frame && !frame->pixels.empty())
                 chain.custom(std::make_unique<SharedBgraOp>(std::move(frame), chrome->impl_->cache_key(), 1.f));
             return true;
         })
         .on_mouse_move([chrome](const MouseEvent& e) { return chrome->send_mouse_move(e); })
         .on_mouse_down([chrome](const MouseEvent& e) { return chrome->send_mouse_down(e); })
         .on_mouse_up([chrome](const MouseEvent& e) { return chrome->send_mouse_up(e); })
         .on_wheel([chrome](const MouseEvent& e) { return chrome->send_wheel(e); });
    return chrome;
}

void OtterChromeLayer::resize(int width, int height) { impl_->resize(width, height); }
void OtterChromeLayer::set_query_handler(QueryHandler handler) { impl_->set_query_handler(std::move(handler)); }
void OtterChromeLayer::load_url(const std::wstring& url) { impl_->load_url(url); }
void OtterChromeLayer::load_file(const std::wstring& path) { impl_->load_file(path); }
void OtterChromeLayer::load_html(const std::wstring& html, const std::wstring& virtual_url) { impl_->load_html(html, virtual_url); }
void OtterChromeLayer::evaluate_script(const std::wstring& script, const std::wstring& url) { impl_->evaluate_script(script, url); }
void OtterChromeLayer::set_json_state(const std::wstring& json) { impl_->set_json_state(json); }
void OtterChromeLayer::transparent_hit_test(bool enable, uint8_t alpha_threshold) { impl_->transparent_hit_test(enable, alpha_threshold); }
void OtterChromeLayer::set_region_hit_test(RegionHitTest hit_test) { impl_->set_region_hit_test(std::move(hit_test)); }
void OtterChromeLayer::focus(bool focused) { impl_->focus(focused); }
bool OtterChromeLayer::hit_test(float local_x, float local_y) const { return impl_->hit_test(local_x, local_y); }
bool OtterChromeLayer::send_mouse_move(const MouseEvent& e) { return impl_->send_mouse_move(e); }
bool OtterChromeLayer::send_mouse_down(const MouseEvent& e) { return impl_->send_mouse_down(e); }
bool OtterChromeLayer::send_mouse_up(const MouseEvent& e) { return impl_->send_mouse_up(e); }
bool OtterChromeLayer::send_wheel(const MouseEvent& e) { return impl_->send_wheel(e); }
void OtterChromeLayer::send_key_down(WPARAM key, LPARAM native) { impl_->send_key_down(key, native); }
void OtterChromeLayer::send_key_char(WPARAM ch, LPARAM native) { impl_->send_key_char(ch, native); }
void OtterChromeLayer::send_key_up(WPARAM key, LPARAM native) { impl_->send_key_up(key, native); }
const uint8_t* OtterChromeLayer::pixels() const { return impl_->pixels(); }
int OtterChromeLayer::width() const { return impl_->width(); }
int OtterChromeLayer::height() const { return impl_->height(); }
int OtterChromeLayer::stride() const { return impl_->stride(); }
}

#endif // !defined(OTTER_DISABLE_CEF)
