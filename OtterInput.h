#pragma once

#ifndef OTTER_USE_RAYLIB
#include "OtterPlatform.h"
#endif

#include <cstdint>

namespace Otter
{
    enum class Key : uint16_t
    {
        Unknown = 0,
        Backspace = 8,
        Tab = 9,
        Enter = 13,
        Escape = 27,
        Space = 32,
        Left = 1000,
        Up,
        Right,
        Down,
        MouseLeft,
        MouseRight,
        MouseMiddle,
    };

    inline Key key_from_ascii(char ch)
    {
        if (ch >= 'a' && ch <= 'z')
            ch = static_cast<char>(ch - 'a' + 'A');
        if (ch >= 32 && ch <= 126)
            return static_cast<Key>(static_cast<uint16_t>(ch));
        return Key::Unknown;
    }

    inline uint16_t key_code(Key key)
    {
        return static_cast<uint16_t>(key);
    }

#if defined(OTTER_PLATFORM_WINDOWS) && !defined(OTTER_USE_RAYLIB)
    inline Key key_from_native(uintptr_t native_key)
    {
        switch (native_key)
        {
        case VK_BACK: return Key::Backspace;
        case VK_TAB: return Key::Tab;
        case VK_RETURN: return Key::Enter;
        case VK_ESCAPE: return Key::Escape;
        case VK_SPACE: return Key::Space;
        case VK_LEFT: return Key::Left;
        case VK_UP: return Key::Up;
        case VK_RIGHT: return Key::Right;
        case VK_DOWN: return Key::Down;
        case VK_LBUTTON: return Key::MouseLeft;
        case VK_RBUTTON: return Key::MouseRight;
        case VK_MBUTTON: return Key::MouseMiddle;
        default:
            if (native_key >= 'a' && native_key <= 'z')
                native_key = native_key - 'a' + 'A';
            if (native_key >= 32 && native_key <= 126)
                return static_cast<Key>(static_cast<uint16_t>(native_key));
            return Key::Unknown;
        }
    }

    inline uintptr_t native_key_from_key(Key key)
    {
        switch (key)
        {
        case Key::Backspace: return VK_BACK;
        case Key::Tab: return VK_TAB;
        case Key::Enter: return VK_RETURN;
        case Key::Escape: return VK_ESCAPE;
        case Key::Space: return VK_SPACE;
        case Key::Left: return VK_LEFT;
        case Key::Up: return VK_UP;
        case Key::Right: return VK_RIGHT;
        case Key::Down: return VK_DOWN;
        case Key::MouseLeft: return VK_LBUTTON;
        case Key::MouseRight: return VK_RBUTTON;
        case Key::MouseMiddle: return VK_MBUTTON;
        default:
            return key_code(key);
        }
    }

    inline bool is_key_down(Key key)
    {
        return (GetAsyncKeyState(static_cast<int>(native_key_from_key(key))) & 0x8000) != 0;
    }
#else
    inline Key key_from_native(uintptr_t native_key)
    {
        if (native_key >= 32 && native_key <= 126)
            return static_cast<Key>(static_cast<uint16_t>(native_key));
        return Key::Unknown;
    }

    inline uintptr_t native_key_from_key(Key key)
    {
        return key_code(key);
    }

    inline bool is_key_down(Key)
    {
        return false;
    }
#endif
}
