#pragma once

#include "OtterWindowBackend.h"

#include <memory>
#include <stdexcept>

#if defined(OTTER_USE_GLFW)
#include "platform/glfw/OtterGlfwWindowBackend.h"
#elif defined(_WIN32)
#include "OtterWindow.h"
#endif

namespace Otter
{
    inline std::unique_ptr<IWindowBackend> create_platform_window(const WindowCreateInfo& info)
    {
#if defined(OTTER_USE_GLFW)
        return std::make_unique<GlfwWindowBackend>(info);
#elif defined(_WIN32)
        return std::make_unique<otterwindow>(info.width, info.height, info.title, info.backend);
#else
        (void)info;
        throw std::runtime_error("No Otter platform window backend is available. Define OTTER_USE_GLFW and link GLFW, or add a native backend for this target.");
#endif
    }
}
