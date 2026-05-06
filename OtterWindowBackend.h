#pragma once

#include "OtterInput.h"
#include "OtterLayer.h"

#include <functional>
#include <memory>
#include <string_view>

namespace Otter
{
    enum class RenderBackend
    {
#ifndef OTTER_DISABLE_D2D
        Direct2D,
#endif
        OpenGL,
    };

    inline constexpr RenderBackend default_render_backend()
    {
#ifndef OTTER_DISABLE_D2D
        return RenderBackend::Direct2D;
#else
        return RenderBackend::OpenGL;
#endif
    }

    struct WindowCreateInfo
    {
        int width = 0;
        int height = 0;
        std::wstring_view title;
        RenderBackend backend = default_render_backend();
    };

    class IWindowBackend
    {
    public:
        virtual ~IWindowBackend() = default;
        virtual void* native_handle() const = 0;
        virtual int width() const = 0;
        virtual int height() const = 0;
        virtual RenderContext& renderer() = 0;
        virtual void run() = 0;
        virtual void close() = 0;
        virtual void set_keyboard_target(std::function<bool(wchar_t)> char_cb,
                                         std::function<bool(Key)> key_cb = {},
                                         std::function<void()> defocus_cb = {}) = 0;
    };
}
