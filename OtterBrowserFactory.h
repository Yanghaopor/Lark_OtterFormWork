#pragma once

#include "OtterBrowserBackend.h"
#include "OtterChromeNode.h"
#include "OtterWebView2Node.h"

namespace Otter
{
    inline int browser_execute_subprocess(BrowserBackend backend, HINSTANCE instance)
    {
        switch (resolve_browser_backend(backend))
        {
        case BrowserBackend::WebView2:
            return OtterWebView2Node::execute_subprocess(instance);
        case BrowserBackend::Cef:
        default:
            return OtterChromeNode::execute_subprocess(instance);
        }
    }

    inline bool browser_initialize(BrowserBackend backend, HINSTANCE instance)
    {
        switch (resolve_browser_backend(backend))
        {
        case BrowserBackend::WebView2:
            return OtterWebView2Node::initialize(instance);
        case BrowserBackend::Cef:
        default:
            return OtterChromeNode::initialize(instance);
        }
    }

    inline void browser_shutdown(BrowserBackend backend)
    {
        switch (resolve_browser_backend(backend))
        {
        case BrowserBackend::WebView2:
            OtterWebView2Node::shutdown();
            return;
        case BrowserBackend::Cef:
        default:
            OtterChromeNode::shutdown();
            return;
        }
    }

    inline void browser_use_shared_texture(BrowserBackend backend, bool enable)
    {
        switch (resolve_browser_backend(backend))
        {
        case BrowserBackend::WebView2:
            OtterWebView2Node::use_shared_texture(enable);
            return;
        case BrowserBackend::Cef:
        default:
            OtterChromeNode::use_shared_texture(enable);
            return;
        }
    }

    inline std::unique_ptr<IBrowserNode> create_browser_node(BrowserBackend backend, HWND parent)
    {
        switch (resolve_browser_backend(backend))
        {
        case BrowserBackend::WebView2:
            return std::make_unique<OtterWebView2Node>(parent);
        case BrowserBackend::Cef:
        default:
            return std::make_unique<OtterChromeNode>(parent);
        }
    }

    inline IBrowserLayer* attach_browser_layer(BrowserBackend backend, Layer* layer, int width, int height, void* parent_window)
    {
        switch (resolve_browser_backend(backend))
        {
        case BrowserBackend::WebView2:
            return OtterWebView2Layer::attach(layer, width, height, parent_window);
        case BrowserBackend::Cef:
        default:
            return OtterChromeLayer::attach(layer, width, height, parent_window);
        }
    }
}
