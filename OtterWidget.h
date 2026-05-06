#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
// ============================================================
//  OtterWidget.h
//  ˮ̡ͼ�ο�� ���� UI �ؼ��⣨�� Direct2D ʵ�֣�
//
//  �ṩ��TextField���������룩��TextArea�������ı��򣩡�
//        Checkbox����ѡ�򣩡�TitleText���ı���ǩ��
//
//  �������ܣ�v2.0����
//    �� �����קѡȡ���֣�TextField / TextArea��
//    �� ˫��ѡ�е��ʣ�TextField / TextArea��
//    �� TextArea �������Զ����У�word_wrap ģʽ��
//    �� CSS-like ��ʽ���� API�������棩
//
//  ���пؼ�ʹ�� Direct2D ��Ⱦ����ԭ�� Win32 �Ӵ���������
//  �ı�����ͨ�� otterwindow �ļ��̻ص����ƴ�����
//
//  ������OtterWindow.h���� DirectWrite ֧�֣�
// ============================================================

#include "OtterWindow.h"

#include <algorithm>   // std::min, std::max, std::clamp
#include <cwctype>     // ::iswalnum, ::iswspace
#include <vector>
#include <string>
#include <functional>
#include <optional>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwrite.h>
#pragma comment(lib, "dwrite")

namespace Otter
{
    // ============================================================
    //  �����幤�ߺ������� TextField / TextArea ���ã�
    // ============================================================
    inline void clipboard_set(const std::wstring& text)
    {
        if (!OpenClipboard(nullptr)) return;
        EmptyClipboard();
        size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (h)
        {
            void* p = GlobalLock(h);
            if (p) memcpy(p, text.c_str(), bytes);
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        }
        CloseClipboard();
    }

    inline std::wstring clipboard_get()
    {
        if (!OpenClipboard(nullptr)) return {};
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        std::wstring result;
        if (h)
        {
            wchar_t* p = static_cast<wchar_t*>(GlobalLock(h));
            if (p) result = p;
            GlobalUnlock(h);
        }
        CloseClipboard();
        return result;
    }

    // ============================================================
    //  �ı��������ߺ���
    //  measure_text_width�����ظ����ַ�����ָ������/�ֺ��µ����ؿ���
    // ============================================================
    inline float measure_text_width(const std::wstring& text,
        const std::wstring& font = L"Segoe UI",
        float size = 14.f)
    {
        if (text.empty()) return 0.f;

        static IDWriteFactory* factory = nullptr;
        if (!factory)
        {
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(&factory));
        }
        if (!factory) return static_cast<float>(text.size()) * size * 0.55f;

        IDWriteTextFormat* fmt = nullptr;
        HRESULT hr = factory->CreateTextFormat(
            font.c_str(), nullptr,
            DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"", &fmt);
        if (FAILED(hr) || !fmt) return static_cast<float>(text.size()) * size * 0.55f;

        IDWriteTextLayout* layout = nullptr;
        hr = factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
            fmt, 100000.f, 100000.f, &layout);
        fmt->Release();
        if (FAILED(hr) || !layout) return static_cast<float>(text.size()) * size * 0.55f;

        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        layout->Release();
        return m.widthIncludingTrailingWhitespace;
    }


    // ============================================================
    //  TextField ���� �����ı�����򣨴� Direct2D ʵ�֣�
    //
    //  �¹��ܣ�
    //    �� on_mouse_down  �� ���ù�� / ѡ�����
    //    �� on_mouse_move  �� ��ק����ѡ���յ�
    //    �� on_mouse_up    �� ȷ��ѡ��
    //    �� on_double_click �� ѡ�е�ǰ����
    //    �� ѡ����������ɫ��͸������ + ѡ�����ְ�ɫ
    //    �� ��ʽ���� API��ȫ����
    // ============================================================
    class TextField
    {
    public:
        TextField(otterwindow* win, Layer* parent,
            std::string_view name,
            float x, float y, float w, float h)
            : win_(win), layer_(parent->creat(std::string(name))),
            width_(w), height_(h)
        {
            layer_->translate(x, y)
                .layer_bounds(0, 0, w, h)
                .hit_area(0, 0, w, h);

            // ���� ��갴�£���ȡ���� + ���ù��/ѡ����� ��������������������
            layer_->on_mouse_down([this](const MouseEvent& e) -> bool {
                if (!e.left_down) return false;
                focus(true);
                float offset_x = calc_scroll_offset();
                int pos = hit_test_pos(e.x, offset_x);
                cursor_pos_ = pos;
                sel_start_ = pos;
                sel_end_ = pos;
                dragging_ = true;
                return true;
                });

            // ���� ����ƶ�����ק����ѡ���յ� ��������������������������������������������
            layer_->on_mouse_move([this](const MouseEvent& e) -> bool {
                if (!dragging_) return false;
                float offset_x = calc_scroll_offset();
                int pos = hit_test_pos(e.x, offset_x);
                sel_end_ = pos;
                cursor_pos_ = pos;
                return true;
                });

            // ���� ���̧��ȷ��ѡ�� ��������������������������������������������������������������
            layer_->on_mouse_up([this](const MouseEvent& e) -> bool {
                dragging_ = false;
                return false;
                });

            // ���� ˫����ѡ�е�ǰ���� ��������������������������������������������������������������
            layer_->on_double_click([this](const MouseEvent& e) -> bool {
                focus(true);
                float offset_x = calc_scroll_offset();
                int pos = hit_test_pos(e.x, offset_x);
                select_word_at(pos);
                return true;
                });

            // ���� ������on_click �� mouse_down ֮�󴥷�����Ҫ���ڽ��㣩
            // ���� on_mouse_down �������˴�������ע���Է������Ҫ
            layer_->on_click([this](const MouseEvent&) -> bool {
                return true;
                });

            // ���� ��Ⱦ ������������������������������������������������������������������������������������������
            layer_->on_render([this](PaintChain& ch, float) -> bool {
                render(ch);
                return true;
                });
        }

        ~TextField()
        {
            if (focused_)
                win_->clear_keyboard_target();
        }

        // ���� ���� ������������������������������������������������������������������������������������������
        std::wstring text() const { return buffer_; }

        TextField& set_text(std::wstring_view t)
        {
            buffer_ = std::wstring(t);
            cursor_pos_ = static_cast<int>(buffer_.size());
            sel_start_ = cursor_pos_;
            sel_end_ = cursor_pos_;
            return *this;
        }

        TextField& placeholder(std::wstring_view t)
        {
            placeholder_ = std::wstring(t);
            return *this;
        }

        // ���� �ص� ������������������������������������������������������������������������������������������
        TextField& on_change(std::function<void(const std::wstring&)> cb)
        {
            on_change_cb_ = std::move(cb); return *this;
        }

        TextField& on_enter(std::function<void(const std::wstring&)> cb)
        {
            on_enter_cb_ = std::move(cb); return *this;
        }

        TextField& on_focus_change(std::function<void(bool)> cb)
        {
            on_focus_cb_ = std::move(cb); return *this;
        }

        // ���� CSS-like ��ʽ��ʽ API ��������������������������������������������������������
        TextField& font_size(float s)
        {
            font_size_ = s; return *this;
        }

        TextField& font_family(std::wstring_view f)
        {
            font_family_ = std::wstring(f); return *this;
        }

        // 100~900��400=normal��700=bold
        TextField& font_weight(int w)
        {
            // ӳ�䵽 TextStyle::Weight ö�٣�ֱ�Ӵ洢����ֵ��
            font_weight_ = w;
            return *this;
        }

        TextField& text_color(Color c)
        {
            text_color_ = c; return *this;
        }

        TextField& placeholder_color(Color c)
        {
            placeholder_color_ = c; return *this;
        }

        TextField& bg_color(Color c)
        {
            bg_color_ = c; return *this;
        }

        TextField& border_color(Color normal, Color focused)
        {
            border_normal_ = normal; border_focused_ = focused; return *this;
        }

        TextField& border_width(float w)
        {
            border_width_ = w; return *this;
        }

        TextField& border_radius(float r)
        {
            border_radius_ = r; return *this;
        }

        TextField& caret_color(Color c)
        {
            caret_color_ = c; return *this;
        }

        TextField& caret_blink_step(float step)
        {
            cursor_blink_step_ = (std::max)(0.001f, step); return *this;
        }

        TextField& padding(float p)
        {
            pad_top_ = pad_right_ = pad_bottom_ = pad_left_ = p; return *this;
        }

        TextField& padding(float top, float right, float bottom, float left)
        {
            pad_top_ = top; pad_right_ = right; pad_bottom_ = bottom; pad_left_ = left; return *this;
        }

        TextField& selection_bg_color(Color c)
        {
            sel_bg_color_ = c; return *this;
        }

        TextField& selection_text_color(Color c)
        {
            sel_text_color_ = c; return *this;
        }

        TextField& opacity(float o)
        {
            opacity_ = (std::clamp)(o, 0.f, 1.f); return *this;
        }

        TextField& letter_spacing(float s)
        {
            letter_spacing_ = s; return *this;
        }

        TextField& max_length(int n)
        {
            max_length_ = n; return *this;
        }

        TextField& readonly(bool r = true)
        {
            readonly_ = r; return *this;
        }

        TextField& password_mode(bool p = true)
        {
            password_ = p; return *this;
        }

        // ���� ״̬ ������������������������������������������������������������������������������������������
        bool is_focused() const { return focused_; }

        void focus(bool f)
        {
            if (focused_ == f) return;
            focused_ = f;

            if (f)
            {
                win_->set_keyboard_target(
                    [this](WCHAR ch)            -> bool { return handle_char(ch); },
                    [this](WPARAM vk, LPARAM lp)-> bool { return handle_key(vk, lp); },
                    {},
                    [this]() { focus(false); }
                );

                // ���� IME ���λ�ûص�
                win_->set_ime_cursor_callback([this]() -> std::pair<float, float> {
                    return get_cursor_window_position();
                    });
            }
            else
            {
                win_->clear_keyboard_target();
                sel_start_ = sel_end_ = cursor_pos_;
                dragging_ = false;
                cursor_blink_ = 0.f;
            }

            if (on_focus_cb_) on_focus_cb_(f);
        }

        void clear()
        {
            buffer_.clear();
            cursor_pos_ = 0;
            sel_start_ = sel_end_ = 0;
            if (on_change_cb_) on_change_cb_(buffer_);
        }

        void select_all()
        {
            sel_start_ = 0;
            sel_end_ = static_cast<int>(buffer_.size());
            cursor_pos_ = sel_end_;
        }

        // ��ȡ����ڴ����е�λ�ã����� IME ��ѡ���ڶ�λ��
        std::pair<float, float> get_cursor_window_position() const
        {
            float text_x = pad_left_;
            float cx = text_x + measure_text_width(buffer_.substr(0, cursor_pos_), font_family_, font_size_);
            float cy = pad_top_ + font_size_;  // ʹ�� font_size_ ��Ϊ�߶�

            // ת��Ϊ��������
            return { layer_->world_x() + cx, layer_->world_y() + cy };
        }

        TextField& resize(float w, float h)
        {
            width_ = w;
            height_ = h;
            layer_->layer_bounds(0, 0, w, h).hit_area(0, 0, w, h);
            return *this;
        }

        Layer* layer() const { return layer_; }

    private:
        // ���� ��ʾ�ı�������ģʽ��������������������������������������������������������������
        std::wstring display_text() const
        {
            if (password_ && !buffer_.empty())
                return std::wstring(buffer_.size(), L'\u2022');
            return buffer_;
        }

        // ���� ���㵱ǰ����ƫ�ƣ�ȷ�����ɼ�������������������������������������������
        float calc_scroll_offset() const
        {
            // ���� hit_test ʹ�ã���Ⱦʱ�����¼��㲢���� scroll_offset_
            return scroll_offset_;
        }

        // ���� ������� X �������в����ַ�λ�� ������������������������������������������
        //  text_x = pad_left_���ı���ʼ x��
        //  x_in_text = (��� x - text_x + scroll_offset)
        int hit_test_pos(float mouse_x, float /*scroll_offset_unused*/) const
        {
            std::wstring disp = display_text();
            float text_x = pad_left_;
            float x_in_text = mouse_x - text_x + scroll_offset_;
            if (x_in_text <= 0.f) return 0;

            int n = static_cast<int>(disp.size());
            for (int i = 0; i < n; ++i)
            {
                float w_left = measure_text_width(disp.substr(0, i), font_family_, font_size_);
                float w_right = measure_text_width(disp.substr(0, i + 1), font_family_, font_size_);
                float mid = (w_left + w_right) * 0.5f;
                if (x_in_text < mid) return i;
            }
            return n;
        }

        // ���� ˫��ѡ�е��� ��������������������������������������������������������������������������
        void select_word_at(int pos)
        {
            int n = static_cast<int>(buffer_.size());
            pos = (std::clamp)(pos, 0, n);

            // �����Ҵʱ߽�
            int lo = pos;
            while (lo > 0 && (::iswalnum(buffer_[lo - 1]) || buffer_[lo - 1] == L'_'))
                --lo;

            // �����Ҵʱ߽�
            int hi = pos;
            while (hi < n && (::iswalnum(buffer_[hi]) || buffer_[hi] == L'_'))
                ++hi;

            if (lo == hi)
            {
                // û�����е����ַ���ѡ�е����ַ�
                if (hi < n) ++hi;
            }

            sel_start_ = lo;
            sel_end_ = hi;
            cursor_pos_ = hi;
        }

        // ���� ���̴��� ������������������������������������������������������������������������������������������
        bool handle_char(WCHAR ch)
        {
            if (readonly_) return false;

            if (ch == '\b') // Backspace
            {
                if (sel_start_ != sel_end_)
                    delete_selection();
                else if (cursor_pos_ > 0)
                {
                    buffer_.erase(cursor_pos_ - 1, 1);
                    --cursor_pos_;
                    sel_start_ = sel_end_ = cursor_pos_;
                    if (on_change_cb_) on_change_cb_(buffer_);
                }
                return true;
            }
            if (ch == '\r' || ch == '\n') // Enter
            {
                if (on_enter_cb_) on_enter_cb_(buffer_);
                return true;
            }
            // �ɴ�ӡ�ַ�
            if (ch >= 32 && ch != 127)
            {
                delete_selection();
                if (max_length_ > 0 && static_cast<int>(buffer_.size()) >= max_length_)
                    return false;
                buffer_.insert(cursor_pos_, 1, ch);
                ++cursor_pos_;
                sel_start_ = sel_end_ = cursor_pos_;
                if (on_change_cb_) on_change_cb_(buffer_);
                return true;
            }
            return false;
        }

        bool handle_key(WPARAM vk, LPARAM /*lp*/)
        {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

            // ���� Ctrl ��ݼ� ������������������������������������������������������������������������������
            if (ctrl)
            {
                switch (vk)
                {
                case 'A': // ȫѡ
                    select_all();
                    return true;

                case 'C': // ����
                {
                    std::wstring sel = get_selection();
                    if (!sel.empty()) Otter::clipboard_set(sel);
                    return true;
                }
                case 'X': // ����
                    if (!readonly_)
                    {
                        std::wstring sel = get_selection();
                        if (!sel.empty())
                        {
                            Otter::clipboard_set(sel);
                            delete_selection();
                        }
                    }
                    return true;

                case 'V': // ճ��
                    if (!readonly_)
                    {
                        std::wstring clip = Otter::clipboard_get();
                        if (!clip.empty())
                        {
                            delete_selection();
                            // ���У�ȥ�����з�
                            std::wstring safe;
                            for (wchar_t c : clip)
                                if (c != L'\r' && c != L'\n') safe += c;
                            if (max_length_ > 0)
                            {
                                int remain = max_length_ - static_cast<int>(buffer_.size());
                                if (remain <= 0) return true;
                                if (static_cast<int>(safe.size()) > remain)
                                    safe.resize(remain);
                            }
                            buffer_.insert(cursor_pos_, safe);
                            cursor_pos_ += static_cast<int>(safe.size());
                            sel_start_ = sel_end_ = cursor_pos_;
                            if (on_change_cb_) on_change_cb_(buffer_);
                        }
                    }
                    return true;
                }
            }

            // ���� �������֧�� Shift ��չѡ��������������������������������������������
            int n = static_cast<int>(buffer_.size());

            switch (vk)
            {
            case VK_LEFT:
                if (shift)
                {
                    if (cursor_pos_ > 0)
                    {
                        --cursor_pos_;
                        sel_end_ = cursor_pos_;
                    }
                }
                else
                {
                    if (sel_start_ != sel_end_)
                    {
                        // ��ѡ��ʱ���Ƶ�ѡ�����
                        cursor_pos_ = (std::min)(sel_start_, sel_end_);
                    }
                    else if (cursor_pos_ > 0)
                    {
                        --cursor_pos_;
                    }
                    sel_start_ = sel_end_ = cursor_pos_;
                }
                return true;

            case VK_RIGHT:
                if (shift)
                {
                    if (cursor_pos_ < n)
                    {
                        ++cursor_pos_;
                        sel_end_ = cursor_pos_;
                    }
                }
                else
                {
                    if (sel_start_ != sel_end_)
                    {
                        cursor_pos_ = (std::max)(sel_start_, sel_end_);
                    }
                    else if (cursor_pos_ < n)
                    {
                        ++cursor_pos_;
                    }
                    sel_start_ = sel_end_ = cursor_pos_;
                }
                return true;

            case VK_HOME:
                if (shift)
                {
                    cursor_pos_ = 0;
                    sel_end_ = 0;
                }
                else
                {
                    cursor_pos_ = 0;
                    sel_start_ = sel_end_ = 0;
                }
                return true;

            case VK_END:
                if (shift)
                {
                    cursor_pos_ = n;
                    sel_end_ = n;
                }
                else
                {
                    cursor_pos_ = n;
                    sel_start_ = sel_end_ = n;
                }
                return true;

            case VK_DELETE:
                if (!readonly_)
                {
                    if (sel_start_ != sel_end_)
                        delete_selection();
                    else if (cursor_pos_ < n)
                    {
                        buffer_.erase(cursor_pos_, 1);
                        if (on_change_cb_) on_change_cb_(buffer_);
                    }
                }
                return true;
            }
            return false;
        }

        // ���� ������ / ѡ������ ������������������������������������������������������������������������
        std::wstring get_selection() const
        {
            int lo = (std::min)(sel_start_, sel_end_);
            int hi = (std::max)(sel_start_, sel_end_);
            if (lo >= hi) return {};
            return buffer_.substr(lo, hi - lo);
        }

        void delete_selection()
        {
            int lo = (std::min)(sel_start_, sel_end_);
            int hi = (std::max)(sel_start_, sel_end_);
            if (lo >= hi) return;
            buffer_.erase(lo, hi - lo);
            cursor_pos_ = lo;
            sel_start_ = sel_end_ = lo;
            if (on_change_cb_) on_change_cb_(buffer_);
        }

        bool has_selection() const { return sel_start_ != sel_end_; }

        // ���� ��Ⱦ ��������������������������������������������������������������������������������������������������
        void render(PaintChain& ch)
        {
            // �����˸��ʱ
            cursor_blink_ += cursor_blink_step_;
            if (cursor_blink_ > 1.f) cursor_blink_ = 0.f;

            float bx = 0.f, by = 0.f;
            float text_x = bx + pad_left_;
            float text_y = by + (height_ - font_size_) * 0.5f;
            float text_w = width_ - pad_left_ - pad_right_;
            float text_h = font_size_ + 4.f;

            std::wstring disp = display_text();

            // ���� ����ˮƽ������ȷ�����ɼ� ����������������������������������������������
            float cursor_raw_x = text_x + measure_text_width(
                disp.substr(0, (std::min)(cursor_pos_, static_cast<int>(disp.size()))),
                font_family_, font_size_);

            if (cursor_raw_x - scroll_offset_ > text_x + text_w - 2.f)
                scroll_offset_ = cursor_raw_x - (text_x + text_w - 2.f);
            else if (cursor_raw_x - scroll_offset_ < text_x + 2.f)
                scroll_offset_ = cursor_raw_x - text_x - 2.f;
            if (scroll_offset_ < 0.f) scroll_offset_ = 0.f;

            // ���� ���� ������������������������������������������������������������������������������������������
            ch.fill_round_rect(bx, by, width_, height_, border_radius_, bg_color_);

            // ���� �߿� ������������������������������������������������������������������������������������������
            Color border = focused_ ? border_focused_ : border_normal_;
            ch.stroke_round_rect(bx, by, width_, height_,
                border_radius_, StrokeStyle{ border, border_width_ });

            // ���� �����ı����� ����������������������������������������������������������������������������
            ch.push_round_clip(text_x, text_y, text_w, text_h, 0.f);

            // ���� ռλ�� ��������������������������������������������������������������������������������������
            if (disp.empty() && !focused_)
            {
                TextStyle ts;
                ts.font_family = font_family_;
                ts.font_size = font_size_;
                ts.color = placeholder_color_;
                ts.letter_spacing = letter_spacing_;
                ts.weight = static_cast<TextStyle::Weight>(font_weight_);
                ch.text(placeholder_, text_x, text_y, ts);
                ch.pop_clip();
                return;
            }

            // ���� ѡ���������� ����������������������������������������������������������������������������
            if (has_selection())
            {
                int lo = (std::min)(sel_start_, sel_end_);
                int hi = (std::max)(sel_start_, sel_end_);
                lo = (std::clamp)(lo, 0, static_cast<int>(disp.size()));
                hi = (std::clamp)(hi, 0, static_cast<int>(disp.size()));

                float x_lo = text_x + measure_text_width(disp.substr(0, lo), font_family_, font_size_) - scroll_offset_;
                float x_hi = text_x + measure_text_width(disp.substr(0, hi), font_family_, font_size_) - scroll_offset_;

                // �õ��ɼ���
                float vis_lo = (std::max)(x_lo, text_x);
                float vis_hi = (std::min)(x_hi, text_x + text_w);
                if (vis_hi > vis_lo)
                {
                    ch.fill_round_rect(vis_lo, text_y - 1.f,
                        vis_hi - vis_lo, text_h + 2.f,
                        2.f, sel_bg_color_);
                }
            }

            // ���� �ı����ֶ���Ⱦ��ʵ��ѡ����ɫ���֣���������������������������������
            if (!disp.empty())
            {
                if (!has_selection())
                {
                    // ��ѡ�����������
                    TextStyle ts;
                    ts.font_family = font_family_;
                    ts.font_size = font_size_;
                    ts.color = text_color_;
                    ts.letter_spacing = letter_spacing_;
                    ts.weight = static_cast<TextStyle::Weight>(font_weight_);
                    ch.text(disp, text_x - scroll_offset_, text_y, ts);
                }
                else
                {
                    // ��ѡ���������λ��ƣ�ѡǰ / ѡ�� / ѡ��
                    int lo = (std::clamp)((std::min)(sel_start_, sel_end_), 0, static_cast<int>(disp.size()));
                    int hi = (std::clamp)((std::max)(sel_start_, sel_end_), 0, static_cast<int>(disp.size()));

                    float x0 = text_x - scroll_offset_;

                    auto draw_seg = [&](int from, int to, Color col) {
                        if (from >= to) return;
                        std::wstring seg = disp.substr(from, to - from);
                        float seg_x = x0 + measure_text_width(disp.substr(0, from), font_family_, font_size_);
                        TextStyle ts;
                        ts.font_family = font_family_;
                        ts.font_size = font_size_;
                        ts.color = col;
                        ts.letter_spacing = letter_spacing_;
                        ts.weight = static_cast<TextStyle::Weight>(font_weight_);
                        ch.text(seg, seg_x, text_y, ts);
                        };

                    int total = static_cast<int>(disp.size());
                    draw_seg(0, lo, text_color_);
                    draw_seg(lo, hi, sel_text_color_);
                    draw_seg(hi, total, text_color_);
                }
            }

            ch.pop_clip();

            // ���� ��� ������������������������������������������������������������������������������������������
            if (focused_ && cursor_blink_ < 0.5f)
            {
                float cx = text_x
                    + measure_text_width(disp.substr(0, (std::min)(cursor_pos_, static_cast<int>(disp.size()))),
                        font_family_, font_size_)
                    - scroll_offset_;
                cx = (std::clamp)(cx, text_x + 1.f, text_x + text_w - 1.f);

                ch.move_to(cx, by + pad_top_ + 2.f)
                    .line_to(cx, by + height_ - pad_bottom_ - 2.f)
                    .stroke(caret_color_, 1.5f);
            }
        }

        // ���� ��Ա���� ��������������������������������������������������������������������������������������������
        otterwindow* win_ = nullptr;
        Layer* layer_ = nullptr;

        float width_ = 0.f;
        float height_ = 0.f;

        // ����
        float        font_size_ = 14.f;
        int          font_weight_ = 400;        // CSS font-weight ��ֵ
        std::wstring font_family_ = L"Segoe UI";
        float        letter_spacing_ = 0.f;

        // ռλ��
        std::wstring placeholder_;

        // ��ɫ
        Color text_color_ = Color{ 0.9f,  0.9f,  0.9f,  1.f };
        Color placeholder_color_ = Color{ 0.45f, 0.45f, 0.5f,  1.f };
        Color bg_color_ = Color{ 0.16f, 0.16f, 0.20f, 1.f };
        Color border_normal_ = Color{ 0.25f, 0.25f, 0.30f, 1.f };
        Color border_focused_ = Color{ 0.4f,  0.6f,  1.f,   1.f };
        Color caret_color_ = Color{ 0.9f,  0.9f,  0.9f,  1.f };
        Color sel_bg_color_ = Color{ 0.26f, 0.52f, 0.96f, 0.35f };
        Color sel_text_color_ = Color::white();

        // ����
        float border_radius_ = 4.f;
        float border_width_ = 1.5f;
        float pad_top_ = 8.f;
        float pad_right_ = 8.f;
        float pad_bottom_ = 8.f;
        float pad_left_ = 8.f;
        float opacity_ = 1.f;

        // ���ܿ���
        bool readonly_ = false;
        bool password_ = false;
        int  max_length_ = 0;   // 0 = ������

        // ����״̬
        std::wstring buffer_;
        int  cursor_pos_ = 0;
        int  sel_start_ = 0;   // ѡ����㣨anchor��
        int  sel_end_ = 0;   // ѡ���յ㣨active end��==sel_start_ ʱ��ѡ����
        bool focused_ = false;
        bool dragging_ = false;  // ���������קѡ��
        float cursor_blink_ = 0.f;
        float cursor_blink_step_ = 0.016f;
        float scroll_offset_ = 0.f;   // ˮƽ����ƫ�ƣ����أ�>=0��

        // �ص�
        std::function<void(const std::wstring&)> on_change_cb_;
        std::function<void(const std::wstring&)> on_enter_cb_;
        std::function<void(bool)>                on_focus_cb_;
    };


    // ============================================================
    //  TextArea ���� �����ı��򣨴� Direct2D ʵ�֣�
    //
    //  �¹��ܣ�
    //    �� on_mouse_down  �� ���ù�� / ѡ�����
    //    �� on_mouse_move  �� ��ק����ѡ���յ�
    //    �� on_mouse_up    �� ȷ��ѡ��
    //    �� on_double_click �� ѡ�е�ǰ����
    //    �� ѡ����������ɫ��͸������ + ѡ�����ְ�ɫ
    //    �� word_wrap ģʽ���������Զ����У��Ӿ��У�
    //    �� ��ʽ���� API��ȫ����
    //
    //  ������ϵ��
    //    �� �߼���/�� (cursor_line_, cursor_col_) ��Ӧ buffer_ �� \n �ָ�
    //    �� �Ӿ��� (VisualLine)��ÿ���߼������к������������Ļ��
    //    �� sel_start_ / sel_end_ ��Ϊ buffer_ �ֽ�ƫ��
    // ============================================================
    class TextArea
    {
    public:
        TextArea(otterwindow* win, Layer* parent,
            std::string_view name,
            float x, float y, float w, float h)
            : win_(win), layer_(parent->creat(std::string(name))),
            width_(w), height_(h)
        {
            layer_->translate(x, y)
                .layer_bounds(0, 0, w, h)
                .hit_area(0, 0, w, h);

            // ���� ��갴�£���ȡ���� + ���ù��/ѡ����� ��������������������
            layer_->on_mouse_down([this](const MouseEvent& e) -> bool {
                if (!e.left_down) return false;
                focus(true);
                int buf_idx = hit_test_buffer_index(e.x, e.y);
                cursor_from_buffer_idx(buf_idx, cursor_line_, cursor_col_);
                sel_start_ = buf_idx;
                sel_end_ = buf_idx;
                dragging_ = true;
                scroll_to_cursor_ = true;
                return true;
                });

            // ���� ����ƶ�����ק����ѡ���յ� ��������������������������������������������
            layer_->on_mouse_move([this](const MouseEvent& e) -> bool {
                if (!dragging_) return false;
                int buf_idx = hit_test_buffer_index(e.x, e.y);
                sel_end_ = buf_idx;
                cursor_from_buffer_idx(buf_idx, cursor_line_, cursor_col_);
                scroll_to_cursor_ = true;
                return true;
                });

            // ���� ���̧��ȷ��ѡ�� ��������������������������������������������������������������
            layer_->on_mouse_up([this](const MouseEvent&) -> bool {
                dragging_ = false;
                return false;
                });

            // ���� ˫����ѡ�е�ǰ���� ��������������������������������������������������������������
            layer_->on_double_click([this](const MouseEvent& e) -> bool {
                focus(true);
                int buf_idx = hit_test_buffer_index(e.x, e.y);
                select_word_at(buf_idx);
                return true;
                });

            // on_click ��������ܼ��ݣ�
            layer_->on_click([this](const MouseEvent&) -> bool {
                return true;
                });

            // ���� ��Ⱦ ������������������������������������������������������������������������������������������
            layer_->on_render([this](PaintChain& ch, float) -> bool {
                render(ch);
                return true;
                });
        }

        ~TextArea()
        {
            if (focused_)
                win_->clear_keyboard_target();
        }

        // ���� ���� ������������������������������������������������������������������������������������������
        std::wstring text() const { return buffer_; }

        TextArea& set_text(std::wstring_view t)
        {
            if (buffer_ == t)
                return *this;
            buffer_ = std::wstring(t);
            auto ln = get_lines();
            cursor_line_ = static_cast<int>(ln.size()) - 1;
            cursor_col_ = 0;
            sel_start_ = sel_end_ = buffer_index();
            return *this;
        }

        TextArea& placeholder(std::wstring_view t)
        {
            placeholder_ = std::wstring(t);
            return *this;
        }

        // ���� �ص� ������������������������������������������������������������������������������������������
        TextArea& on_change(std::function<void(const std::wstring&)> cb)
        {
            on_change_cb_ = std::move(cb); return *this;
        }

        TextArea& on_focus_change(std::function<void(bool)> cb)
        {
            on_focus_cb_ = std::move(cb); return *this;
        }

        // ���� CSS-like ��ʽ��ʽ API ����������������������������������������������������������
        TextArea& font_size(float s)
        {
            font_size_ = s; line_height_ = s * 1.4f; return *this;
        }

        TextArea& font_family(std::wstring_view f)
        {
            font_family_ = std::wstring(f); return *this;
        }

        TextArea& font_weight(int w)
        {
            font_weight_ = w; return *this;
        }

        TextArea& text_color(Color c)
        {
            text_color_ = c; return *this;
        }

        TextArea& placeholder_color(Color c)
        {
            placeholder_color_ = c; return *this;
        }

        TextArea& bg_color(Color c)
        {
            bg_color_ = c; return *this;
        }

        TextArea& border_color(Color normal, Color focused)
        {
            border_normal_ = normal; border_focused_ = focused; return *this;
        }

        TextArea& border_width(float w)
        {
            border_width_ = w; return *this;
        }

        TextArea& border_radius(float r)
        {
            border_radius_ = r; return *this;
        }

        TextArea& caret_color(Color c)
        {
            caret_color_ = c; return *this;
        }

        TextArea& padding(float p)
        {
            pad_top_ = pad_right_ = pad_bottom_ = pad_left_ = p; return *this;
        }

        TextArea& padding(float top, float right, float bottom, float left)
        {
            pad_top_ = top; pad_right_ = right; pad_bottom_ = bottom; pad_left_ = left; return *this;
        }

        TextArea& selection_bg_color(Color c)
        {
            sel_bg_color_ = c; return *this;
        }

        TextArea& selection_text_color(Color c)
        {
            sel_text_color_ = c; return *this;
        }

        TextArea& opacity(float o)
        {
            opacity_ = (std::clamp)(o, 0.f, 1.f); return *this;
        }

        TextArea& letter_spacing(float s)
        {
            letter_spacing_ = s; return *this;
        }

        // line_height�����и߶ȣ�px�������� <= 0 ���Զ� = font_size * 1.4
        TextArea& line_height(float lh)
        {
            line_height_ = (lh > 0.f ? lh : font_size_ * 1.4f); return *this;
        }

        TextArea& readonly(bool r = true)
        {
            readonly_ = r; return *this;
        }

        // �������Զ����У�Ĭ�� true��
        TextArea& word_wrap(bool w = true)
        {
            word_wrap_ = w; return *this;
        }

        // ���� ״̬ ������������������������������������������������������������������������������������������
        bool is_focused() const { return focused_; }

        void focus(bool f)
        {
            if (focused_ == f) return;
            focused_ = f;

            if (f)
            {
                win_->set_keyboard_target(
                    [this](WCHAR ch)             -> bool { return handle_char(ch); },
                    [this](WPARAM vk, LPARAM lp) -> bool { return handle_key(vk, lp); },
                    {},
                    [this]() { focus(false); }
                );

                // ���� IME ���λ�ûص�
                win_->set_ime_cursor_callback([this]() -> std::pair<float, float> {
                    return get_cursor_window_position();
                    });
            }
            else
            {
                win_->clear_keyboard_target();
                dragging_ = false;
                cursor_blink_ = 0.f;
            }

            if (on_focus_cb_) on_focus_cb_(f);
        }

        // ��ȡ����ڴ����е�λ�ã����� IME ��ѡ���ڶ�λ��
        std::pair<float, float> get_cursor_window_position() const
        {
            float text_x = pad_left_;
            float text_y = pad_top_;
            float text_w = width_ - pad_left_ - pad_right_;

            // �����Ӿ��У��������Ϊ�գ�
            if (cached_visual_lines_.empty())
                const_cast<TextArea*>(this)->cached_visual_lines_ = build_visual_lines(text_w);

            const auto& vls = cached_visual_lines_;
            if (vls.empty())
                return { layer_->world_x() + text_x, layer_->world_y() + text_y };

            // �ҵ���������Ӿ���
            int cur_buf_idx = buffer_index();
            int cur_vi = 0;
            for (int vi = 0; vi < static_cast<int>(vls.size()); ++vi)
            {
                if (cur_buf_idx >= vls[vi].buf_start && cur_buf_idx <= vls[vi].buf_end)
                {
                    cur_vi = vi;
                    break;
                }
                if (cur_buf_idx >= vls[vi].buf_start)
                    cur_vi = vi;
            }

            const VisualLine& cvl = vls[cur_vi];
            int col_in_vl = cur_buf_idx - cvl.buf_start;
            col_in_vl = (std::clamp)(col_in_vl, 0, static_cast<int>(cvl.text.size()));

            // ������λ��
            float cx = text_x + measure_text_width(cvl.text.substr(0, col_in_vl), font_family_, font_size_);
            float cy = text_y + cur_vi * line_height_ - scroll_y_;

            // ת��Ϊ��������
            return { layer_->world_x() + cx, layer_->world_y() + cy + line_height_ };
        }

        void clear()
        {
            buffer_.clear();
            cursor_line_ = 0;
            cursor_col_ = 0;
            sel_start_ = sel_end_ = 0;
            if (on_change_cb_) on_change_cb_(buffer_);
        }

        void select_all()
        {
            sel_start_ = 0;
            sel_end_ = static_cast<int>(buffer_.size());
            auto ln = get_lines();
            cursor_line_ = static_cast<int>(ln.size()) - 1;
            cursor_col_ = static_cast<int>(ln[cursor_line_].size());
        }

        TextArea& resize(float w, float h)
        {
            width_ = w;
            height_ = h;
            layer_->layer_bounds(0, 0, w, h).hit_area(0, 0, w, h);
            set_scroll_y(scroll_y_);
            return *this;
        }

        Layer* layer() const { return layer_; }

        // scroll / layout accessors for line-number gutter sync
        float scroll_y()         const { return scroll_y_; }
        float get_line_height()  const { return line_height_; }
        float get_pad_top()      const { return pad_top_; }
        int   cursor_line()      const { return cursor_line_; }
        int   line_count()       const
        {
            int n = 1;
            for (wchar_t c : buffer_) if (c == L'\n') ++n;
            return n;
        }

        // Max scrollable offset (total content height minus visible height)
        float max_scroll_y() const
        {
            int   total_vlines = line_count(); // approx; exact would need build_visual_lines
            float content_h    = pad_top_ + total_vlines * line_height_ + pad_bottom_;
            float view_h       = height_;
            return (std::max)(0.f, content_h - view_h);
        }

        // Scroll to a specific offset (clamped)
        void set_scroll_y(float y)
        {
            scroll_y_ = (std::max)(0.f, (std::min)(y, max_scroll_y()));
        }

        // Scroll by delta lines (positive = scroll down = content moves up)
        void scroll_by_lines(float delta_lines)
        {
            set_scroll_y(scroll_y_ + delta_lines * line_height_);
        }

    private:
        // ============================================================
        //  �Ӿ��нṹ
        //  ÿ���߼��У�\n �ָ�ĶΣ����к�������� VisualLine��
        //  buf_start / buf_end�����Ӿ��ж�Ӧ�� buffer_ �ֽ�ƫ������
        //  [buf_start, buf_end)��������ĩ \n��
        // ============================================================
        struct VisualLine
        {
            std::wstring text;      // ���Ӿ��е���������
            int          buf_start; // �� buffer_ �е���ʼ�ֽ�ƫ��
            int          buf_end;   // �� buffer_ �еĽ����ֽ�ƫ�ƣ����� \n��
            bool         is_last_of_logical; // �Ƿ�Ϊ���߼������һ���Ӿ���
        };

        // ���� �����Ӿ����б� ��������������������������������������������������������������������������������
        //  text_w�����ÿ��ȣ���Ⱦʱ���룩
        std::vector<VisualLine> build_visual_lines(float text_w) const
        {
            std::vector<VisualLine> result;
            auto logical = get_lines();          // �� \n �ָ���߼���
            int buf_pos = 0;                     // ��ǰ�� buffer_ �е�λ��

            for (int li = 0; li < static_cast<int>(logical.size()); ++li)
            {
                const std::wstring& line = logical[li];
                bool is_last_logical = (li == static_cast<int>(logical.size()) - 1);

                if (!word_wrap_ || text_w <= 0.f
                    || measure_text_width(line, font_family_, font_size_) <= text_w)
                {
                    // ����Ҫ����
                    VisualLine vl;
                    vl.text = line;
                    vl.buf_start = buf_pos;
                    vl.buf_end = buf_pos + static_cast<int>(line.size());
                    vl.is_last_of_logical = true;
                    result.push_back(vl);
                }
                else
                {
                    // ��Ҫ���У����ַ�����̰���и�
                    int start = 0;  // �� line �е���ʼλ��
                    int n = static_cast<int>(line.size());

                    while (start < n)
                    {
                        // �ҵ������ܶ���ַ�ʹ���Ȳ����� text_w
                        int end = start;
                        float w = 0.f;
                        while (end < n)
                        {
                            float cw = measure_text_width(line.substr(0, end + 1),
                                font_family_, font_size_) - w;
                            // �ۻ���飺�� start �� end �Ŀ���
                            float seg_w = measure_text_width(
                                line.substr(start, end - start + 1),
                                font_family_, font_size_);
                            if (seg_w > text_w && end > start)
                                break;  // ����������
                            ++end;
                        }
                        if (end == start) ++end; // ���ٰ���һ���ַ�������ѭ��

                        // �����ڴʱ߽���ˣ��� ASCII �ո�
                        // �޸�����ȷ�����ո�ָȷ���ָ����Ȳ�����
                        bool skip_space = false;  // ����Ƿ���Ҫ�����ո�
                        if (end < n && word_wrap_)
                        {
                            int back = end;
                            // ����ֱ���ҵ��ո񣬻򵽴� start+1�����ٱ���һ���ַ���
                            while (back > start + 1 && line[back] != L' ')
                                --back;

                            // ����ҵ��˿ո�back > start �� line[back] �ǿո�
                            if (back > start && line[back] == L' ')
                            {
                                // ���ָ�󣨲����ո񣩵Ŀ����Ƿ񲻳���
                                float split_w = measure_text_width(
                                    line.substr(start, back - start),
                                    font_family_, font_size_);
                                if (split_w <= text_w)
                                {
                                    // �ڿո񴦷ָ��һ�в����ո�
                                    end = back;  // �����ո�
                                    skip_space = true;  // ��һ�������ո�
                                }
                                // ����ָ����Ȼ����������ԭ���� end��ǿ�Ʒָ
                            }
                            // ���򱣳� end ���䣨�޷��ڴʱ߽�ָ
                        }

                        // ���շָ�� end λ�÷ָ�
                        // ��� end ��Ȼʹ���䳬����ǿ�ƻ���һ���ַ�
                        if (end > start)
                        {
                            float final_w = measure_text_width(
                                line.substr(start, end - start),
                                font_family_, font_size_);
                            if (final_w > text_w)
                            {
                                --end;  // ǿ�ƻ���һ���ַ�
                            }
                        }

                        VisualLine vl;
                        vl.text = line.substr(start, end - start);
                        vl.buf_start = buf_pos + start;
                        vl.buf_end = buf_pos + end;
                        vl.is_last_of_logical = (end >= n);
                        result.push_back(vl);

                        // ������һ����ʼλ��
                        start = end;
                        if (skip_space && start < n && line[start] == L' ')
                        {
                            ++start;  // �����ո���һ�дӿո��ʼ
                        }
                    }
                }

                // ����߼������� + ���з�
                buf_pos += static_cast<int>(line.size());
                if (!is_last_logical) buf_pos += 1; // '\n'
            }

            return result;
        }

        // ���� �������� buffer_ �� \n �ָ�Ϊ�߼��� ����������������������������������
        std::vector<std::wstring> get_lines() const
        {
            std::vector<std::wstring> lines;
            std::wstring cur;
            for (wchar_t ch : buffer_)
            {
                if (ch == L'\n') { lines.push_back(cur); cur.clear(); }
                else              cur += ch;
            }
            lines.push_back(cur);
            return lines;
        }

        int count_lines() const
        {
            int n = 1;
            for (wchar_t ch : buffer_) if (ch == L'\n') ++n;
            return n;
        }

        // ���� �����߼� (line, col) ���� buffer_ �ֽ�ƫ�� ��������������������
        int buffer_index() const
        {
            auto lines = get_lines();
            int idx = 0;
            for (int i = 0; i < cursor_line_ && i < static_cast<int>(lines.size()); ++i)
                idx += static_cast<int>(lines[i].size()) + 1; // +1 for '\n'
            if (cursor_line_ < static_cast<int>(lines.size()))
                idx += (std::min)(cursor_col_, static_cast<int>(lines[cursor_line_].size()));
            return idx;
        }

        // ���� �� buffer_ �ֽ�ƫ�Ʒ����߼� (line, col) ����������������������������
        void cursor_from_buffer_idx(int idx, int& line, int& col) const
        {
            line = 0; col = 0;
            int n = static_cast<int>(buffer_.size());
            idx = (std::clamp)(idx, 0, n);
            for (int i = 0; i < idx; ++i)
            {
                if (buffer_[i] == L'\n') { ++line; col = 0; }
                else ++col;
            }
        }

        // ���� ������в��ԣ����� buffer_ �ֽ�ƫ�� ������������������������������������
        //  mx, my������ڿؼ����Ͻǵ�����
        int hit_test_buffer_index(float mx, float my) const
        {
            float text_x = pad_left_;
            float text_y = pad_top_;
            float text_w = width_ - pad_left_ - pad_right_;

            // ʹ���ϴ���Ⱦ���Ӿ��л��棻��Ϊ����ʱ����
            const auto& vls = cached_visual_lines_.empty()
                ? (const_cast<TextArea*>(this)->cached_visual_lines_ = build_visual_lines(text_w))
                : cached_visual_lines_;

            if (vls.empty()) return 0;

            // �ҵ����е��Ӿ���
            float vy = text_y - scroll_y_;
            int vi = 0;
            for (; vi < static_cast<int>(vls.size()) - 1; ++vi)
            {
                if (my < vy + line_height_) break;
                vy += line_height_;
            }

            const VisualLine& vl = vls[vi];
            const std::wstring& seg = vl.text;

            // �ڸ��Ӿ����ڶ���������
            float x_in = mx - text_x;
            int col = 0;
            int sn = static_cast<int>(seg.size());
            for (int i = 0; i < sn; ++i)
            {
                float w_left = measure_text_width(seg.substr(0, i), font_family_, font_size_);
                float w_right = measure_text_width(seg.substr(0, i + 1), font_family_, font_size_);
                if (x_in < (w_left + w_right) * 0.5f) break;
                col = i + 1;
            }
            col = (std::clamp)(col, 0, sn);

            return vl.buf_start + col;
        }

        // ���� ˫��ѡ�е��� ����������������������������������������������������������������������������������
        void select_word_at(int buf_idx)
        {
            int n = static_cast<int>(buffer_.size());
            buf_idx = (std::clamp)(buf_idx, 0, n);

            int lo = buf_idx;
            while (lo > 0 && (::iswalnum(buffer_[lo - 1]) || buffer_[lo - 1] == L'_'))
                --lo;

            int hi = buf_idx;
            while (hi < n && (::iswalnum(buffer_[hi]) || buffer_[hi] == L'_'))
                ++hi;

            if (lo == hi && hi < n) ++hi;

            sel_start_ = lo;
            sel_end_ = hi;
            cursor_from_buffer_idx(hi, cursor_line_, cursor_col_);
        }

        // ���� ���̴��� ������������������������������������������������������������������������������������������
        bool handle_char(WCHAR ch)
        {
            if (readonly_) return false;
            scroll_to_cursor_ = true;

            if (ch == '\b') // Backspace
            {
                if (sel_start_ != sel_end_)
                    delete_selection_ta();
                else
                {
                    int idx = buffer_index();
                    if (idx > 0)
                    {
                        bool is_newline = (buffer_[idx - 1] == L'\n');
                        buffer_.erase(idx - 1, 1);
                        if (is_newline)
                        {
                            --cursor_line_;
                            auto lines = get_lines();
                            cursor_col_ = static_cast<int>(lines[cursor_line_].size());
                        }
                        else
                        {
                            --cursor_col_;
                        }
                        sel_start_ = sel_end_ = buffer_index();
                        if (on_change_cb_) on_change_cb_(buffer_);
                    }
                }
                return true;
            }
            if (ch == '\r') // Enter �� ����
            {
                delete_selection_ta();
                int idx = buffer_index();
                buffer_.insert(idx, L"\n");
                ++cursor_line_;
                cursor_col_ = 0;
                sel_start_ = sel_end_ = buffer_index();
                if (on_change_cb_) on_change_cb_(buffer_);
                return true;
            }
            if (ch >= 32 && ch != 127) // �ɴ�ӡ�ַ�
            {
                delete_selection_ta();
                int idx = buffer_index();
                buffer_.insert(idx, 1, ch);
                ++cursor_col_;
                sel_start_ = sel_end_ = buffer_index();
                if (on_change_cb_) on_change_cb_(buffer_);
                return true;
            }
            return false;
        }

        bool handle_key(WPARAM vk, LPARAM /*lp*/)
        {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            scroll_to_cursor_ = true;

            if (ctrl)
            {
                switch (vk)
                {
                case 'A': // ȫѡ
                    select_all();
                    return true;

                case 'C': // ����
                {
                    std::wstring sel = get_selection_ta();
                    if (!sel.empty()) Otter::clipboard_set(sel);
                    return true;
                }
                case 'X': // ����
                    if (!readonly_)
                    {
                        std::wstring sel = get_selection_ta();
                        if (!sel.empty())
                        {
                            Otter::clipboard_set(sel);
                            delete_selection_ta();
                        }
                    }
                    return true;

                case 'V': // ճ��
                    if (!readonly_)
                    {
                        std::wstring clip = Otter::clipboard_get();
                        if (!clip.empty())
                        {
                            delete_selection_ta();
                            // ͳһ \r\n �� \n
                            std::wstring safe;
                            for (int i = 0; i < static_cast<int>(clip.size()); ++i)
                                if (clip[i] != L'\r') safe += clip[i];

                            int idx = buffer_index();
                            buffer_.insert(idx, safe);
                            // ���¹�굽ճ��ĩβ
                            for (wchar_t c : safe)
                            {
                                if (c == L'\n') { ++cursor_line_; cursor_col_ = 0; }
                                else ++cursor_col_;
                            }
                            sel_start_ = sel_end_ = buffer_index();
                            if (on_change_cb_) on_change_cb_(buffer_);
                        }
                    }
                    return true;
                }
            }

            auto lines = get_lines();
            int  total_lines = static_cast<int>(lines.size());

            // �� word_wrap ģʽ�£����·�������Ӿ����ƶ�
            switch (vk)
            {
            case VK_LEFT:
            {
                if (shift)
                {
                    int cur = buffer_index();
                    if (cur > 0)
                    {
                        if (buffer_[cur - 1] == L'\n') { --cursor_line_; auto& l = lines[cursor_line_]; cursor_col_ = static_cast<int>(l.size()); }
                        else if (cursor_col_ > 0) --cursor_col_;
                        sel_end_ = buffer_index();
                    }
                }
                else
                {
                    if (sel_start_ != sel_end_)
                    {
                        int lo = (std::min)(sel_start_, sel_end_);
                        cursor_from_buffer_idx(lo, cursor_line_, cursor_col_);
                        sel_start_ = sel_end_ = lo;
                    }
                    else
                    {
                        if (cursor_col_ > 0) --cursor_col_;
                        else if (cursor_line_ > 0)
                        {
                            --cursor_line_;
                            cursor_col_ = static_cast<int>(lines[cursor_line_].size());
                        }
                        sel_start_ = sel_end_ = buffer_index();
                    }
                }
                return true;
            }
            case VK_RIGHT:
            {
                if (shift)
                {
                    int cur = buffer_index();
                    int total_buf = static_cast<int>(buffer_.size());
                    if (cur < total_buf)
                    {
                        if (buffer_[cur] == L'\n') { ++cursor_line_; cursor_col_ = 0; }
                        else if (cursor_col_ < static_cast<int>(lines[cursor_line_].size())) ++cursor_col_;
                        sel_end_ = buffer_index();
                    }
                }
                else
                {
                    if (sel_start_ != sel_end_)
                    {
                        int hi = (std::max)(sel_start_, sel_end_);
                        cursor_from_buffer_idx(hi, cursor_line_, cursor_col_);
                        sel_start_ = sel_end_ = hi;
                    }
                    else
                    {
                        if (cursor_col_ < static_cast<int>(lines[cursor_line_].size())) ++cursor_col_;
                        else if (cursor_line_ < total_lines - 1)
                        {
                            ++cursor_line_;
                            cursor_col_ = 0;
                        }
                        sel_start_ = sel_end_ = buffer_index();
                    }
                }
                return true;
            }
            case VK_UP:
            {
                if (cursor_line_ > 0)
                {
                    --cursor_line_;
                    cursor_col_ = (std::min)(cursor_col_, static_cast<int>(lines[cursor_line_].size()));
                }
                if (shift)
                    sel_end_ = buffer_index();
                else
                    sel_start_ = sel_end_ = buffer_index();
                return true;
            }
            case VK_DOWN:
            {
                if (cursor_line_ < total_lines - 1)
                {
                    ++cursor_line_;
                    cursor_col_ = (std::min)(cursor_col_, static_cast<int>(lines[cursor_line_].size()));
                }
                if (shift)
                    sel_end_ = buffer_index();
                else
                    sel_start_ = sel_end_ = buffer_index();
                return true;
            }
            case VK_HOME:
            {
                cursor_col_ = 0;
                if (shift) sel_end_ = buffer_index();
                else sel_start_ = sel_end_ = buffer_index();
                return true;
            }
            case VK_END:
            {
                cursor_col_ = static_cast<int>(lines[cursor_line_].size());
                if (shift) sel_end_ = buffer_index();
                else sel_start_ = sel_end_ = buffer_index();
                return true;
            }
            case VK_TAB:
            {
                if (!readonly_)
                {
                    delete_selection_ta();
                    // Insert 4 spaces (soft tab, VS-style)
                    int idx = buffer_index();
                    buffer_.insert(idx, 4, L' ');
                    cursor_col_ += 4;
                    sel_start_ = sel_end_ = buffer_index();
                    if (on_change_cb_) on_change_cb_(buffer_);
                }
                return true;
            }
            case VK_DELETE:
            {
                if (!readonly_)
                {
                    if (sel_start_ != sel_end_)
                        delete_selection_ta();
                    else
                    {
                        int idx = buffer_index();
                        if (idx < static_cast<int>(buffer_.size()))
                        {
                            buffer_.erase(idx, 1);
                            if (on_change_cb_) on_change_cb_(buffer_);
                        }
                    }
                }
                return true;
            }
            }
            return false;
        }

        // ���� ѡ������ ������������������������������������������������������������������������������������������
        std::wstring get_selection_ta() const
        {
            int lo = (std::min)(sel_start_, sel_end_);
            int hi = (std::max)(sel_start_, sel_end_);
            if (lo >= hi) return {};
            return buffer_.substr(lo, hi - lo);
        }

        void delete_selection_ta()
        {
            int lo = (std::min)(sel_start_, sel_end_);
            int hi = (std::max)(sel_start_, sel_end_);
            if (lo >= hi) return;
            buffer_.erase(lo, hi - lo);
            cursor_from_buffer_idx(lo, cursor_line_, cursor_col_);
            sel_start_ = sel_end_ = lo;
            if (on_change_cb_) on_change_cb_(buffer_);
        }

        bool has_selection() const { return sel_start_ != sel_end_; }

        // ���� ��Ⱦ ��������������������������������������������������������������������������������������������������
        void render(PaintChain& ch)
        {
            cursor_blink_ += 0.016f;
            if (cursor_blink_ > 1.f) cursor_blink_ = 0.f;

            float bx = 0.f, by = 0.f;
            float text_x = bx + pad_left_;
            float text_y = by + pad_top_;
            float text_w = width_ - pad_left_ - pad_right_;
            float text_h = height_ - pad_top_ - pad_bottom_;

            // ���� �ؽ��Ӿ��� ��������������������������������������������������������������������������������
            cached_visual_lines_ = build_visual_lines(text_w);
            const auto& vls = cached_visual_lines_;

            // ���� ��ǰ��������Ӿ��� ����������������������������������������������������������������
            int cur_buf_idx = buffer_index();
            int cur_vi = 0; // ��������Ӿ�������
            {
                int best = 0;
                for (int vi = 0; vi < static_cast<int>(vls.size()); ++vi)
                {
                    if (cur_buf_idx >= vls[vi].buf_start)
                        best = vi;
                    // �ϸ�������
                    if (cur_buf_idx >= vls[vi].buf_start
                        && cur_buf_idx <= vls[vi].buf_end)
                    {
                        cur_vi = vi;
                        break;
                    }
                    cur_vi = best;
                }
            }

            // ���� ��ֱ������ȷ������пɼ� ����������������������������������������������������
            if (scroll_to_cursor_) {
                scroll_to_cursor_ = false;
                float cursor_screen_y = text_y + cur_vi * line_height_ - scroll_y_;
                if (cursor_screen_y < text_y)
                    scroll_y_ = static_cast<float>(cur_vi) * line_height_;
                else if (cursor_screen_y + line_height_ > text_y + text_h)
                    scroll_y_ = static_cast<float>(cur_vi + 1) * line_height_ - text_h;
                if (scroll_y_ < 0.f) scroll_y_ = 0.f;
            }

            // ���� ���� ������������������������������������������������������������������������������������������
            ch.fill_round_rect(bx, by, width_, height_, border_radius_, bg_color_);

            // ���� �߿� ������������������������������������������������������������������������������������������
            Color border = focused_ ? border_focused_ : border_normal_;
            ch.stroke_round_rect(bx, by, width_, height_,
                border_radius_, StrokeStyle{ border, border_width_ });

            // ���� �����ı����� ����������������������������������������������������������������������������
            ch.push_round_clip(text_x, text_y, text_w, text_h, 0.f);

            // ���� ռλ�� ��������������������������������������������������������������������������������������
            if (buffer_.empty() && !focused_)
            {
                TextStyle ts;
                ts.font_family = font_family_;
                ts.font_size = font_size_;
                ts.color = placeholder_color_;
                ts.letter_spacing = letter_spacing_;
                ts.weight = static_cast<TextStyle::Weight>(font_weight_);
                ts.word_wrap = false;
                ch.text(placeholder_, text_x, text_y, text_w, text_h, ts);
                ch.pop_clip();
                return;
            }

            // ���� ѡ�� lo/hi��buffer_ �ֽ�ƫ�ƣ�����������������������������������������
            int sel_lo = (std::min)(sel_start_, sel_end_);
            int sel_hi = (std::max)(sel_start_, sel_end_);

            // ���� ���Ӿ�����Ⱦ ����������������������������������������������������������������������������
            TextStyle ts;
            ts.font_family = font_family_;
            ts.font_size = font_size_;
            ts.letter_spacing = letter_spacing_;
            ts.weight = static_cast<TextStyle::Weight>(font_weight_);
            ts.word_wrap = false;

            for (int vi = 0; vi < static_cast<int>(vls.size()); ++vi)
            {
                float line_y = text_y + vi * line_height_ - scroll_y_;

                // �������ɼ���
                if (line_y + line_height_ < text_y) continue;
                if (line_y > text_y + text_h)       break;

                const VisualLine& vl = vls[vi];
                const std::wstring& seg = vl.text;
                int vs = vl.buf_start;
                int ve = vl.buf_end;

                // �жϴ��Ӿ����Ƿ���ѡ���ص�
                bool seg_has_sel = has_selection() && sel_lo < ve && sel_hi > vs;

                if (!seg_has_sel)
                {
                    // ��ѡ�����������
                    ts.color = text_color_;
                    ch.text(seg, text_x, line_y, text_w, line_height_, ts);
                }
                else
                {
                    // ���Ӿ��е�ѡ���� [line_lo, line_hi]������� vs��
                    int line_lo = (std::max)(sel_lo, vs) - vs;
                    int line_hi = (std::min)(sel_hi, ve) - vs;
                    int line_len = static_cast<int>(seg.size());

                    line_lo = (std::clamp)(line_lo, 0, line_len);
                    line_hi = (std::clamp)(line_hi, 0, line_len);

                    // ѡ��������������
                    float x_lo = text_x + measure_text_width(seg.substr(0, line_lo), font_family_, font_size_);
                    float x_hi = text_x + measure_text_width(seg.substr(0, line_hi), font_family_, font_size_);

                    // �޸�����ѡ����������ʱ������Ӧ����߿�ʼ���쵽�ұ�
                    bool full_line_selected = (line_lo == 0 && line_hi >= line_len);
                    if (line_lo == 0)
                        x_lo = text_x;  // ������߿�ʼ
                    if (line_hi >= line_len)
                        x_hi = text_x + text_w;  // ���쵽���ұ�

                    if (x_hi > x_lo)
                    {
                        ch.fill_round_rect(x_lo, line_y, x_hi - x_lo, line_height_, 2.f, sel_bg_color_);
                    }

                    // �����λ�������
                    auto draw_seg_ta = [&](int from, int to, Color col) {
                        if (from >= to) return;
                        float sx = text_x + measure_text_width(seg.substr(0, from), font_family_, font_size_);
                        ts.color = col;
                        ch.text(seg.substr(from, to - from), sx, line_y, ts);
                        };

                    draw_seg_ta(0, line_lo, text_color_);
                    draw_seg_ta(line_lo, line_hi, sel_text_color_);
                    draw_seg_ta(line_hi, line_len, text_color_);
                }
            }

            ch.pop_clip();

            // ���� ��� ������������������������������������������������������������������������������������������
            if (focused_ && cursor_blink_ < 0.5f && cur_vi < static_cast<int>(vls.size()))
            {
                const VisualLine& cvl = vls[cur_vi];
                int col_in_vl = cur_buf_idx - cvl.buf_start;
                col_in_vl = (std::clamp)(col_in_vl, 0, static_cast<int>(cvl.text.size()));

                float cx = text_x + measure_text_width(
                    cvl.text.substr(0, col_in_vl), font_family_, font_size_);
                float cy = text_y + cur_vi * line_height_ - scroll_y_;

                cx = (std::clamp)(cx, text_x, text_x + text_w - 1.f);

                ch.move_to(cx, cy + 1.f)
                    .line_to(cx, cy + line_height_ - 1.f)
                    .stroke(caret_color_, 1.5f);
            }
        }

        // ���� ��Ա���� ��������������������������������������������������������������������������������������������
        otterwindow* win_ = nullptr;
        Layer* layer_ = nullptr;

        float width_ = 0.f;
        float height_ = 0.f;

        // ����
        float        font_size_ = 14.f;
        int          font_weight_ = 400;
        std::wstring font_family_ = L"Segoe UI";
        float        letter_spacing_ = 0.f;
        float        line_height_ = 20.f;

        // ռλ��
        std::wstring placeholder_;

        // ��ɫ
        Color text_color_ = Color{ 0.9f,  0.9f,  0.9f,  1.f };
        Color placeholder_color_ = Color{ 0.45f, 0.45f, 0.5f,  1.f };
        Color bg_color_ = Color{ 0.16f, 0.16f, 0.20f, 1.f };
        Color border_normal_ = Color{ 0.25f, 0.25f, 0.30f, 1.f };
        Color border_focused_ = Color{ 0.4f,  0.6f,  1.f,   1.f };
        Color caret_color_ = Color{ 0.9f,  0.9f,  0.9f,  1.f };
        Color sel_bg_color_ = Color{ 0.26f, 0.52f, 0.96f, 0.35f };
        Color sel_text_color_ = Color::white();

        // ����
        float border_radius_ = 4.f;
        float border_width_ = 1.5f;
        float pad_top_ = 8.f;
        float pad_right_ = 8.f;
        float pad_bottom_ = 8.f;
        float pad_left_ = 8.f;
        float opacity_ = 1.f;

        // ���ܿ���
        bool readonly_ = false;
        bool word_wrap_ = true;

        // ����״̬���߼����꣩
        std::wstring buffer_;
        int          cursor_line_ = 0;
        int          cursor_col_ = 0;
        int          sel_start_ = 0;   // buffer_ �ֽ�ƫ��
        int          sel_end_ = 0;
        bool         focused_ = false;
        bool         dragging_ = false;
        bool         scroll_to_cursor_ = false;  // set when cursor moves, cleared after scroll adjust
        float        cursor_blink_ = 0.f;
        float        scroll_x_ = 0.f;
        float        scroll_y_ = 0.f;

        // �Ӿ��л��棨��Ⱦʱ���£�hit_test ʱ���ã�
        mutable std::vector<VisualLine> cached_visual_lines_;

        // �ص�
        std::function<void(const std::wstring&)> on_change_cb_;
        std::function<void(bool)>                on_focus_cb_;
    };


    // ============================================================
    //  ImageView ���� ͼƬ��ʾ�ؼ�
    //  ֧�֣�����·����Բ�ǡ�͸���ȡ�object-fit(fill/contain/cover)
    //  ����ͼͨ�� Layer::paint().img(...) ʵ��
    // ============================================================
    class ImageView
    {
    public:
        enum class Fit { Fill, Contain, Cover };

        ImageView(Layer* parent, std::string_view name,
            float x, float y, float w, float h)
            : layer_(parent->creat(std::string(name))),
            width_(w), height_(h)
        {
            layer_->translate(x, y)
                .layer_bounds(0, 0, w, h);
            layer_->on_render([this](PaintChain& ch, float) -> bool {
                render(ch);
                return true;
                });
        }

        // ���� ���� ������������������������������������������������������������������������������������������
        ImageView& src(std::wstring_view path)
        {
            path_ = std::wstring(path); return *this;
        }

        // ���� ��ʽ ������������������������������������������������������������������������������������������
        ImageView& border_radius(float r) { radius_ = r; return *this; }
        ImageView& opacity(float o) { opacity_ = (std::clamp)(o, 0.f, 1.f); return *this; }
        ImageView& fit(Fit f) { fit_ = f; return *this; }
        ImageView& bg_color(Color c) { bg_ = c; has_bg_ = true; return *this; }

        Layer* layer() const { return layer_; }

    private:
        void render(PaintChain& ch)
        {
            if (has_bg_)
                ch.fill_round_rect(0, 0, width_, height_, radius_, bg_);
            if (!path_.empty())
                ch.img(path_, 0, 0, width_, height_, opacity_, radius_);
        }

        Layer* layer_ = nullptr;
        float        width_ = 0;
        float        height_ = 0;
        std::wstring path_;
        float        radius_ = 0.f;
        float        opacity_ = 1.f;
        Fit          fit_ = Fit::Fill;
        Color        bg_ = Color{};
        bool         has_bg_ = false;
    };


    // ============================================================
    //  Checkbox ���� ��ѡ�򣨱�����ԭ����ȫһ�£�
    // ============================================================
    class Checkbox
    {
    public:
        Checkbox(Layer* parent, std::string_view name, float x, float y)
            : layer_(parent->creat(std::string(name)))
        {
            layer_->translate(x, y)
                .hit_area(0, 0, hit_width(), box_size_)
                .on_click([this](const MouseEvent&) -> bool {
                toggle();
                return true;
                    });
            layer_->on_render([this](PaintChain& ch, float) -> bool {
                render(ch);
                return true;
                });
        }

        // ���� ״̬ ������������������������������������������������������������������������������������������
        bool checked() const { return checked_; }

        Checkbox& set_checked(bool v)
        {
            checked_ = v;
            if (on_toggle_cb_) on_toggle_cb_(checked_);
            return *this;
        }

        Checkbox& toggle()
        {
            checked_ = !checked_;
            if (on_toggle_cb_) on_toggle_cb_(checked_);
            return *this;
        }

        // ���� ���� ������������������������������������������������������������������������������������������
        Checkbox& label(std::wstring_view t)
        {
            label_ = std::wstring(t); return *this;
        }

        // ���� �ص� ������������������������������������������������������������������������������������������
        Checkbox& on_toggle(std::function<void(bool)> cb)
        {
            on_toggle_cb_ = std::move(cb); return *this;
        }

        // ���� ��ʽ ������������������������������������������������������������������������������������������
        Checkbox& box_size(float s) { box_size_ = s; return *this; }
        Checkbox& box_color(Color normal, Color hover, Color checked)
        {
            box_normal_ = normal; box_hover_ = hover; box_checked_ = checked; return *this;
        }
        Checkbox& check_color(Color c) { check_color_ = c;  return *this; }
        Checkbox& label_color(Color c) { label_color_ = c;  return *this; }
        Checkbox& font_size(float s) { font_size_ = s;    return *this; }
        Checkbox& font_family(std::wstring_view f)
        {
            font_family_ = std::wstring(f); return *this;
        }
        Checkbox& gap(float g) { gap_ = g;          return *this; }

        Layer* layer() const { return layer_; }

    protected:
        float hit_width() const
        {
            if (label_.empty()) return box_size_ + 4.f;
            return box_size_ + gap_
                + measure_text_width(label_, font_family_, font_size_) + 4.f;
        }

        virtual void render(PaintChain& ch)
        {
            float bx = 2.f, by = 0.f;

            Color bg = checked_ ? box_checked_ :
                (layer_->is_hovered() ? box_hover_ : box_normal_);
            ch.move_to(bx + 2, by)
                .line_to(bx + box_size_ - 2, by)
                .line_to(bx + box_size_, by + 2)
                .line_to(bx + box_size_, by + box_size_ - 2)
                .line_to(bx + box_size_ - 2, by + box_size_)
                .line_to(bx + 2, by + box_size_)
                .line_to(bx, by + box_size_ - 2)
                .line_to(bx, by + 2)
                .close()
                .fill(bg);

            if (checked_)
            {
                ch.move_to(bx + box_size_ * 0.2f,
                    by + box_size_ * 0.45f + box_size_ * 0.25f)
                    .line_to(bx + box_size_ * 0.2f + box_size_ * 0.25f,
                        by + box_size_ * 0.45f + box_size_ * 0.5f)
                    .line_to(bx + box_size_ * 0.2f + box_size_ * 0.6f,
                        by + box_size_ * 0.45f - box_size_ * 0.05f)
                    .stroke(check_color_, 2.f);
            }

            if (!label_.empty())
            {
                TextStyle ts;
                ts.font_family = font_family_;
                ts.font_size = font_size_;
                ts.color = label_color_;
                ch.text(label_, bx + box_size_ + gap_,
                    (box_size_ - font_size_) * 0.5f, ts);
            }
        }

        Layer* layer_ = nullptr;
        bool         checked_ = false;
        std::wstring label_;
        float        box_size_ = 18.f;
        float        gap_ = 8.f;
        float        font_size_ = 14.f;
        std::wstring font_family_ = L"Segoe UI";

        Color box_normal_ = Color{ 0.3f,  0.3f,  0.35f, 1.f };
        Color box_hover_ = Color{ 0.4f,  0.4f,  0.45f, 1.f };
        Color box_checked_ = Color{ 0.4f,  0.6f,  1.f,   1.f };
        Color check_color_ = Color::white();
        Color label_color_ = Color{ 0.85f, 0.85f, 0.9f,  1.f };

        std::function<void(bool)> on_toggle_cb_;
    };


    // ============================================================
    //  TitleText ���� �ɸ����ı���ǩ��������ԭ����ȫһ�£�
    // ============================================================
    class TitleText
    {
    public:
        TitleText(Layer* parent, std::string_view name,
            float x, float y)
            : layer_(parent->creat(std::string(name)))
        {
            layer_->translate(x, y)
                .hit_area(0, 0, 1, 1)
                .on_render([this](PaintChain& ch, float) -> bool {
                render(ch);
                return true;
                    })
                .on_click([this](const MouseEvent&) -> bool {
                return true;
                    });
        }

        TitleText& title(std::wstring_view t)
        {
            title_ = std::wstring(t);
            recalc_size();
            return *this;
        }

        const std::wstring& title() const { return title_; }

        TitleText& font_size(float s)
        {
            font_size_ = s; recalc_size(); return *this;
        }

        TitleText& font_family(std::wstring_view f)
        {
            font_family_ = std::wstring(f); recalc_size(); return *this;
        }

        TitleText& text_color(Color c)
        {
            text_color_ = c; return *this;
        }

        TitleText& bold(bool b = true)
        {
            bold_ = b; return *this;
        }

        Layer* layer() const { return layer_; }

    private:
        void recalc_size()
        {
            if (title_.empty()) {
                layer_->hit_area(0, 0, 1, 1);
                return;
            }
            float w = measure_text_width(title_, font_family_, font_size_);
            layer_->hit_area(0, 0, w, font_size_ + 2.f);
        }

        void render(PaintChain& ch)
        {
            if (title_.empty()) return;
            TextStyle ts;
            ts.font_family = font_family_;
            ts.font_size = font_size_;
            ts.color = text_color_;
            if (bold_) ts.weight = TextStyle::Weight::Bold;
            ch.text(title_, 0, 1, ts);
        }

        Layer* layer_ = nullptr;
        std::wstring title_;
        float        font_size_ = 13.f;
        std::wstring font_family_ = L"Segoe UI";
        Color        text_color_ = Color{ 0.5f, 0.55f, 0.65f, 1.f };
        bool         bold_ = false;
    };


    // ============================================================
    //  DropdownStyle — 下拉栏外观配置（可完全自定义）
    // ============================================================
    struct DropdownStyle
    {
        // 头部尺寸
        float width        = 200.f;
        float height       = 36.f;
        float border_radius= 6.f;
        float border_width = 1.5f;

        // 字体
        std::wstring font_family  = L"Segoe UI";
        float        font_size    = 14.f;
        int          font_weight  = 400;   // 400=normal, 700=bold

        // 内边距
        float pad_x = 12.f;
        float pad_y = 8.f;

        // 颜色 — 头部
        Color bg_normal   = Color{ 0.16f, 0.16f, 0.20f, 1.f };
        Color bg_hover    = Color{ 0.20f, 0.20f, 0.26f, 1.f };
        Color bg_open     = Color{ 0.22f, 0.22f, 0.28f, 1.f };
        Color border_normal  = Color{ 0.25f, 0.25f, 0.30f, 1.f };
        Color border_hover   = Color{ 0.38f, 0.45f, 0.65f, 1.f };
        Color border_open    = Color{ 0.40f, 0.60f, 1.00f, 1.f };
        Color text_color     = Color{ 0.9f,  0.9f,  0.9f,  1.f };
        Color placeholder_color = Color{ 0.45f, 0.45f, 0.50f, 1.f };
        Color arrow_color    = Color{ 0.6f,  0.6f,  0.7f,  1.f };

        // 颜色 — 弹出列表
        Color popup_bg       = Color{ 0.14f, 0.14f, 0.18f, 0.97f };
        Color popup_border   = Color{ 0.30f, 0.32f, 0.40f, 1.f };
        Color item_hover_bg  = Color{ 0.22f, 0.36f, 0.72f, 0.85f };
        Color item_select_bg = Color{ 0.26f, 0.42f, 0.84f, 1.f };
        Color item_text      = Color{ 0.88f, 0.88f, 0.92f, 1.f };
        Color item_select_text = Color::white();

        // 弹出列表参数
        float item_height    = 32.f;
        float popup_radius   = 6.f;
        float popup_border_w = 1.f;
        int   max_visible_items = 6;   // 超过后滚动

        // 动画速度（0 = 无动画，正值=时间常数，越小越快）
        float hover_anim_speed = 8.f;  // 悬停背景过渡速度
        float open_anim_speed  = 10.f; // 展开高度动画速度
        float arrow_anim_speed = 10.f; // 箭头旋转速度
    };


    // ============================================================
    //  Dropdown — 下拉选择控件
    //
    //  API 设计理念（与 TextField 一致）：
    //    · 链式样式设置：.font_size() .bg_color() 等
    //    · 渲染/动画钩子：可完全替换默认绘制逻辑
    //    · on_change 回调：选中项变化时触发
    //
    //  自定义渲染 API：
    //    set_header_renderer(fn)   — 替换头部绘制
    //    set_item_renderer(fn)     — 替换每条选项绘制
    //    set_popup_bg_renderer(fn) — 替换弹出层背景绘制
    //
    //  注：弹出层挂在 root_layer（canvas 级）上，保证不被裁剪
    // ============================================================
    class Dropdown
    {
    public:
        // ── 构造 ─────────────────────────────────────────────────
        Dropdown(otterwindow* win, Layer* parent, Layer* root_layer,
                 std::string_view name,
                 float x, float y,
                 const DropdownStyle& style = DropdownStyle{})
            : win_(win), style_(style)
            , width_(style.width), height_(style.height)
        {
            // 头部图层
            head_ = parent->creat(std::string(name) + "_head");
            head_->translate(x, y)
                 .layer_bounds(0, 0, width_, height_)
                 .hit_area(0, 0, width_, height_);

            // 弹出层挂根（不被 parent 裁剪）
            // 注意：构造时 head_ 尚未完成布局，世界坐标在 open() 中动态更新
            popup_ = root_layer->creat(std::string(name) + "_popup");
            popup_->layer_bounds(0, 0, width_, 0.f)
                   .hit_area(0, 0, 0, 0)
                   .visible(false);

            // ── 头部事件 ──────────────────────────────────────────
            head_->on_mouse_down([this](const MouseEvent& e) -> bool {
                return e.left_down; // 消费 down，阻止 draggable 窗口抢夺
            });
            head_->on_mouse_enter([this](const MouseEvent&) -> bool {
                hover_target_ = 1.f;
                return false;
            });
            head_->on_mouse_leave([this](const MouseEvent&) -> bool {
                hover_target_ = 0.f;
                return false;
            });
            head_->on_click([this](const MouseEvent&) -> bool {
                toggle_open();
                return true;
            });
            head_->on_render([this](PaintChain& ch, float dt) -> bool {
                tick_anim(dt);
                draw_header(ch);
                return true;
            });

            // ── 弹出层事件 ────────────────────────────────────────
            popup_->on_render([this](PaintChain& ch, float) -> bool {
                draw_popup(ch);
                return true;
            });
            popup_->on_mouse_move([this](const MouseEvent& e) -> bool {
                hovered_item_ = item_at(e.y);
                return true;
            });
            popup_->on_mouse_leave([this](const MouseEvent&) -> bool {
                hovered_item_ = -1;
                return false;
            });
            popup_->on_mouse_down([this](const MouseEvent& e) -> bool {
                return e.left_down;
            });
            popup_->on_click([this](const MouseEvent& e) -> bool {
                int idx = item_at(e.y);
                if (idx >= 0 && idx < (int)items_.size()) {
                    selected_ = idx;
                    if (on_change_cb_) on_change_cb_(selected_, items_[selected_]);
                }
                close();
                return true;
            });
        }

        ~Dropdown() = default;

        // 禁止拷贝/移动（内部 lambda 捕获了 this）
        Dropdown(const Dropdown&) = delete;
        Dropdown& operator=(const Dropdown&) = delete;
        Dropdown(Dropdown&&) = delete;
        Dropdown& operator=(Dropdown&&) = delete;

        // ── 数据 API ─────────────────────────────────────────────
        Dropdown& add_item(std::wstring_view text)
        {
            items_.push_back(std::wstring(text));
            return *this;
        }

        Dropdown& set_items(std::vector<std::wstring> v)
        {
            items_ = std::move(v);
            if (selected_ >= (int)items_.size()) selected_ = -1;
            return *this;
        }

        Dropdown& select(int index)
        {
            if (index >= -1 && index < (int)items_.size())
                selected_ = index;
            return *this;
        }

        Dropdown& placeholder(std::wstring_view t)
        {
            placeholder_ = std::wstring(t);
            return *this;
        }

        int         selected_index() const { return selected_; }
        std::wstring selected_text() const
        {
            if (selected_ < 0 || selected_ >= (int)items_.size()) return {};
            return items_[selected_];
        }

        // ── 样式 API ─────────────────────────────────────────────
        Dropdown& font_size(float s)          { style_.font_size = s; return *this; }
        Dropdown& font_family(std::wstring_view f) { style_.font_family = std::wstring(f); return *this; }
        Dropdown& font_weight(int w)          { style_.font_weight = w; return *this; }
        Dropdown& bg_color(Color n, Color h, Color o)
        { style_.bg_normal = n; style_.bg_hover = h; style_.bg_open = o; return *this; }
        Dropdown& border_color(Color n, Color h, Color o)
        { style_.border_normal = n; style_.border_hover = h; style_.border_open = o; return *this; }
        Dropdown& text_color(Color c)         { style_.text_color = c; return *this; }
        Dropdown& arrow_color(Color c)        { style_.arrow_color = c; return *this; }
        Dropdown& popup_bg_color(Color c)     { style_.popup_bg = c; return *this; }
        Dropdown& item_hover_color(Color c)   { style_.item_hover_bg = c; return *this; }
        Dropdown& item_select_color(Color c)  { style_.item_select_bg = c; return *this; }
        Dropdown& item_height(float h)        { style_.item_height = h; return *this; }
        Dropdown& max_visible_items(int n)    { style_.max_visible_items = n; return *this; }
        Dropdown& hover_anim_speed(float s)   { style_.hover_anim_speed = s; return *this; }
        Dropdown& open_anim_speed(float s)    { style_.open_anim_speed = s; return *this; }

        // ── 回调 API ─────────────────────────────────────────────
        Dropdown& on_change(std::function<void(int, const std::wstring&)> cb)
        { on_change_cb_ = std::move(cb); return *this; }

        // ── 自定义渲染钩子 ────────────────────────────────────────
        // fn(PaintChain&, width, height, hover_t[0-1], is_open, selected_text, placeholder)
        using HeaderRenderer = std::function<void(PaintChain&, float /*w*/, float /*h*/,
                                                   float /*hover_t*/, bool /*is_open*/,
                                                   const std::wstring& /*text*/,
                                                   const DropdownStyle&)>;
        Dropdown& set_header_renderer(HeaderRenderer fn)
        { custom_header_ = std::move(fn); return *this; }

        // fn(PaintChain&, item_index, item_text, item_rect{x,y,w,h}, is_hovered, is_selected, style)
        using ItemRenderer = std::function<void(PaintChain&, int /*idx*/,
                                                 const std::wstring& /*text*/,
                                                 float /*x*/, float /*y*/, float /*w*/, float /*h*/,
                                                 bool /*hovered*/, bool /*selected*/,
                                                 const DropdownStyle&)>;
        Dropdown& set_item_renderer(ItemRenderer fn)
        { custom_item_ = std::move(fn); return *this; }

        // fn(PaintChain&, width, popup_h, style) — 替换弹出层背景
        using PopupBgRenderer = std::function<void(PaintChain&, float /*w*/, float /*h*/,
                                                    const DropdownStyle&)>;
        Dropdown& set_popup_bg_renderer(PopupBgRenderer fn)
        { custom_popup_bg_ = std::move(fn); return *this; }

        // ── 状态 ─────────────────────────────────────────────────
        bool is_open() const { return is_open_; }
        void close()
        {
            is_open_ = false;
            open_target_ = 0.f;
            // 立即关闭命中区域（防止透明层接收点击），可见性由动画完成后隐藏
            popup_->hit_area(0, 0, 0, 0);
        }
        void open()
        {
            is_open_ = true;
            open_target_ = 1.f;
            // 每次展开时，用 head_ 的世界坐标重新定位弹出层
            popup_->translate(head_->world_x(), head_->world_y() + height_ + 2.f)
                   .layer_bounds(0, 0, width_, 0.f)
                   .visible(true)
                   .bring_to_front();  // 置顶，保证弹出层不被后来创建的兄弟层遮挡
            // hit_area 由 tick_anim 随动画逐帧更新，此处不提前设置
        }
        void toggle_open() { is_open_ ? close() : open(); }

        Layer* head_layer()  const { return head_; }
        Layer* popup_layer() const { return popup_; }

    private:
        // ── 动画 tick ────────────────────────────────────────────
        void tick_anim(float dt)
        {
            auto lerp_f = [](float cur, float tgt, float spd, float dt_) {
                return cur + (tgt - cur) * std::min(1.f, spd * dt_);
            };
            hover_t_  = lerp_f(hover_t_,  hover_target_,  style_.hover_anim_speed, dt);
            open_t_   = lerp_f(open_t_,   open_target_,   style_.open_anim_speed,  dt);
            arrow_t_  = lerp_f(arrow_t_,  open_target_,   style_.arrow_anim_speed, dt);

            // 更新弹出层实际高度
            float full_h = popup_full_height();
            float cur_h  = full_h * open_t_;
            popup_->layer_bounds(0, 0, width_, cur_h);

            if (is_open_) {
                // 打开时随动画更新命中区域
                popup_->hit_area(0, 0, width_, cur_h);
            } else if (open_t_ < 0.01f) {
                // 动画完全关闭后才隐藏图层
                popup_->visible(false);
            }
        }

        float popup_full_height() const
        {
            int n = std::min((int)items_.size(), style_.max_visible_items);
            return n * style_.item_height + 2.f; // +2 上下内边距
        }

        int item_at(float local_y) const
        {
            int idx = (int)((local_y - 1.f) / style_.item_height);
            if (idx < 0 || idx >= (int)items_.size()) return -1;
            return idx;
        }

        // ── 绘制头部 ─────────────────────────────────────────────
        void draw_header(PaintChain& ch)
        {
            std::wstring display = (selected_ >= 0) ? items_[selected_] : placeholder_;
            bool use_placeholder = (selected_ < 0);

            if (custom_header_) {
                custom_header_(ch, width_, height_, hover_t_, is_open_, display, style_);
                return;
            }

            // 背景色：normal→hover→open 混合
            Color bg = Color::lerp(style_.bg_normal, style_.bg_hover, hover_t_);
            bg = Color::lerp(bg, style_.bg_open, is_open_ ? 0.6f : 0.f);

            // 边框色
            Color bdr = Color::lerp(style_.border_normal, style_.border_hover, hover_t_);
            bdr = Color::lerp(bdr, style_.border_open, open_t_);

            // 背景
            ch.fill_round_rect(0, 0, width_, height_, style_.border_radius, bg);
            // 边框
            ch.stroke_round_rect(0, 0, width_, height_, style_.border_radius,
                                  StrokeStyle{ bdr, style_.border_width });

            // 文字
            float arrow_w = style_.font_size + 12.f;
            float text_w  = width_ - style_.pad_x * 2.f - arrow_w;
            float text_y  = (height_ - style_.font_size) * 0.5f;

            // 裁剪文字区域
            ch.push_round_clip(style_.pad_x, text_y, text_w, style_.font_size + 2.f, 0.f);
            TextStyle ts;
            ts.font_family = style_.font_family;
            ts.font_size   = style_.font_size;
            ts.color       = use_placeholder ? style_.placeholder_color : style_.text_color;
            ts.weight      = static_cast<TextStyle::Weight>(style_.font_weight);
            ch.text(display, style_.pad_x, text_y, ts);
            ch.pop_clip();

            // 箭头（旋转 arrow_t_ * 180°）
            draw_arrow(ch, width_ - style_.pad_x - arrow_w * 0.5f,
                       height_ * 0.5f, arrow_w * 0.35f, arrow_t_);
        }

        // 绘制折叠箭头（chevron）
        void draw_arrow(PaintChain& ch, float cx, float cy, float size, float t)
        {
            // t=0 → 向下，t=1 → 向上；用线段画 "V"
            float angle = t * 3.14159265f; // 0 → π
            float cos_a = std::cos(angle);
            float sin_a = std::sin(angle);

            // 原始向下箭头的两个点（相对中心）
            float lx = -size, ly = -size * 0.5f;
            float mx =  0.f,  my =  size * 0.5f;
            float rx =  size, ry = -size * 0.5f;

            // 旋转
            auto rot = [&](float px, float py, float& ox, float& oy) {
                ox = cx + px * cos_a - py * sin_a;
                oy = cy + px * sin_a + py * cos_a;
            };
            float x0,y0,x1,y1,x2,y2;
            rot(lx,ly,x0,y0);
            rot(mx,my,x1,y1);
            rot(rx,ry,x2,y2);

            StrokeStyle ss{ style_.arrow_color, 1.8f };
            ss.cap  = StrokeStyle::LineCap::Round;
            ss.join = StrokeStyle::LineJoin::Round;
            ch.move_to(x0,y0).line_to(x1,y1).line_to(x2,y2).stroke(ss);
        }

        // ── 绘制弹出层 ────────────────────────────────────────────
        void draw_popup(PaintChain& ch)
        {
            float cur_h = popup_full_height() * open_t_;
            if (cur_h < 1.f) return;

            // 背景
            if (custom_popup_bg_) {
                custom_popup_bg_(ch, width_, cur_h, style_);
            } else {
                ch.fill_round_rect(0, 0, width_, cur_h, style_.popup_radius, style_.popup_bg);
                ch.stroke_round_rect(0, 0, width_, cur_h, style_.popup_radius,
                                      StrokeStyle{ style_.popup_border, style_.popup_border_w });
            }

            // 裁剪到弹出层
            ch.push_round_clip(0, 0, width_, cur_h, style_.popup_radius);

            int n = std::min((int)items_.size(), style_.max_visible_items);
            for (int i = 0; i < n; ++i) {
                float iy = 1.f + i * style_.item_height;
                bool hov = (i == hovered_item_);
                bool sel = (i == selected_);

                if (custom_item_) {
                    custom_item_(ch, i, items_[i], 0, iy, width_, style_.item_height,
                                  hov, sel, style_);
                } else {
                    draw_default_item(ch, i, iy, hov, sel);
                }
            }

            ch.pop_clip();
        }

        void draw_default_item(PaintChain& ch, int idx, float iy, bool hov, bool sel)
        {
            float iw = width_, ih = style_.item_height;

            if (sel) {
                ch.fill_round_rect(2.f, iy, iw - 4.f, ih - 1.f, 4.f, style_.item_select_bg);
            } else if (hov) {
                ch.fill_round_rect(2.f, iy, iw - 4.f, ih - 1.f, 4.f, style_.item_hover_bg);
            }

            float ty = iy + (ih - style_.font_size) * 0.5f;
            TextStyle ts;
            ts.font_family = style_.font_family;
            ts.font_size   = style_.font_size;
            ts.color       = sel ? style_.item_select_text : style_.item_text;
            ts.weight      = static_cast<TextStyle::Weight>(style_.font_weight);
            ch.text(items_[idx], style_.pad_x, ty, ts);
        }

        // ── 成员变量 ──────────────────────────────────────────────
        otterwindow*  win_   = nullptr;
        Layer*        head_  = nullptr;
        Layer*        popup_ = nullptr;

        DropdownStyle style_;
        float width_, height_;

        std::vector<std::wstring> items_;
        std::wstring              placeholder_  = L"请选择...";
        int                       selected_     = -1;
        int                       hovered_item_ = -1;

        bool  is_open_      = false;
        float hover_t_      = 0.f;
        float hover_target_ = 0.f;
        float open_t_       = 0.f;
        float open_target_  = 0.f;
        float arrow_t_      = 0.f;

        std::function<void(int, const std::wstring&)> on_change_cb_;
        HeaderRenderer  custom_header_;
        ItemRenderer    custom_item_;
        PopupBgRenderer custom_popup_bg_;
    };


    // ============================================================
    //  RadioStyle / CheckboxStyle — 共享样式基础
    // ============================================================
    struct ToggleStyle
    {
        float box_size    = 18.f;   // 方框/圆圈直径
        float gap         = 8.f;    // 图标与文字间距
        float border_width= 1.8f;

        std::wstring font_family = L"Segoe UI";
        float        font_size   = 14.f;
        int          font_weight = 400;

        Color bg_off        = Color{ 0.16f, 0.16f, 0.20f, 1.f };
        Color bg_on         = Color{ 0.26f, 0.50f, 0.96f, 1.f };
        Color bg_hover_off  = Color{ 0.20f, 0.20f, 0.26f, 1.f };
        Color bg_hover_on   = Color{ 0.32f, 0.58f, 1.00f, 1.f };
        Color border_off    = Color{ 0.28f, 0.28f, 0.34f, 1.f };
        Color border_on     = Color{ 0.30f, 0.54f, 1.00f, 1.f };
        Color check_color   = Color::white();
        Color text_color    = Color{ 0.88f, 0.88f, 0.92f, 1.f };
        Color disabled_color= Color{ 0.35f, 0.35f, 0.40f, 1.f };

        float anim_speed    = 10.f;  // 状态切换动画速度
        float hover_speed   = 8.f;
    };


    // ============================================================
    //  RadioButtonEx — 单选框（增强版，带动画和自定义渲染）
    // ============================================================
    class RadioButtonEx
    {
    public:
        RadioButtonEx(Layer* parent, std::string_view name,
                    float x, float y,
                    std::wstring_view label,
                    const ToggleStyle& style = ToggleStyle{})
            : style_(style), label_(label)
        {
            float total_w = style_.box_size + style_.gap
                          + measure_text_width(label_, style_.font_family, style_.font_size);
            float total_h = std::max(style_.box_size, style_.font_size + 4.f);

            layer_ = parent->creat(std::string(name));
            layer_->translate(x, y)
                   .layer_bounds(0, 0, total_w + 4.f, total_h)
                   .hit_area(0, 0, total_w + 4.f, total_h);

            layer_->on_mouse_down([this](const MouseEvent& e) -> bool {
                return e.left_down; // 消费 down，阻止 draggable 窗口抢夺
            });
            layer_->on_mouse_enter([this](const MouseEvent&) -> bool {
                hover_target_ = 1.f; return false;
            });
            layer_->on_mouse_leave([this](const MouseEvent&) -> bool {
                hover_target_ = 0.f; return false;
            });
            layer_->on_click([this](const MouseEvent&) -> bool {
                if (!disabled_ && !checked_) {
                    set_checked(true);
                }
                return true;
            });
            layer_->on_render([this](PaintChain& ch, float dt) -> bool {
                tick_anim(dt);
                draw(ch);
                return true;
            });
        }

        // ── 状态 API ─────────────────────────────────────────────
        bool is_checked()  const { return checked_; }
        bool is_disabled() const { return disabled_; }

        RadioButtonEx& set_checked(bool v)
        {
            if (disabled_) return *this;
            if (checked_ == v) return *this;  // 无变化，早退
            checked_ = v;
            if (on_change_cb_) on_change_cb_(v);
            if (group_change_cb_) group_change_cb_(v);
            return *this;
        }

        RadioButtonEx& disabled(bool d = true) { disabled_ = d; return *this; }

        RadioButtonEx& label(std::wstring_view t)
        {
            label_ = std::wstring(t);
            recalc_size();
            return *this;
        }

        // ── 样式 API ─────────────────────────────────────────────
        RadioButtonEx& font_size(float s)  { style_.font_size = s; recalc_size(); return *this; }
        RadioButtonEx& box_size(float s)   { style_.box_size = s;  recalc_size(); return *this; }
        RadioButtonEx& text_color(Color c) { style_.text_color = c;  return *this; }
        RadioButtonEx& bg_color(Color off, Color on) { style_.bg_off = off; style_.bg_on = on; return *this; }
        RadioButtonEx& border_color(Color off, Color on) { style_.border_off = off; style_.border_on = on; return *this; }
        RadioButtonEx& check_color(Color c){ style_.check_color = c; return *this; }
        RadioButtonEx& anim_speed(float s) { style_.anim_speed = s;  return *this; }

        // ── 回调 ─────────────────────────────────────────────────
        RadioButtonEx& on_change(std::function<void(bool)> cb)
        { on_change_cb_ = std::move(cb); return *this; }

        // ── 自定义渲染 ────────────────────────────────────────────
        // fn(ch, checked_t[0-1], hover_t[0-1], label, total_w, total_h, style)
        using Renderer = std::function<void(PaintChain&, float /*checked_t*/,
                                             float /*hover_t*/,
                                             const std::wstring& /*label*/,
                                             float /*w*/, float /*h*/,
                                             const ToggleStyle&)>;
        RadioButtonEx& set_renderer(Renderer fn)
        { custom_renderer_ = std::move(fn); return *this; }

        Layer* layer() const { return layer_; }

        // 禁止拷贝/移动（内部 lambda 捕获了 this）
        RadioButtonEx(const RadioButtonEx&) = delete;
        RadioButtonEx& operator=(const RadioButtonEx&) = delete;
        RadioButtonEx(RadioButtonEx&&) = delete;
        RadioButtonEx& operator=(RadioButtonEx&&) = delete;

    private:
        // RadioGroup 专用内部回调槽（不会覆盖用户的 on_change_cb_）
        friend class RadioGroup;
        void set_group_cb_(std::function<void(bool)> cb)
        { group_change_cb_ = std::move(cb); }

        void recalc_size()
        {
            float total_w = style_.box_size + style_.gap
                          + measure_text_width(label_, style_.font_family, style_.font_size);
            float total_h = std::max(style_.box_size, style_.font_size + 4.f);
            layer_->layer_bounds(0, 0, total_w + 4.f, total_h)
                   .hit_area(0, 0, total_w + 4.f, total_h);
        }

        void tick_anim(float dt)
        {
            float spd = style_.anim_speed;
            auto lerp_f = [](float c, float t, float s, float d) {
                return c + (t - c) * std::min(1.f, s * d);
            };
            hover_t_   = lerp_f(hover_t_,   hover_target_,    style_.hover_speed, dt);
            checked_t_ = lerp_f(checked_t_, checked_ ? 1.f : 0.f, spd, dt);
        }

        void draw(PaintChain& ch)
        {
            float bx  = 2.f;
            float by  = (std::max(style_.box_size, style_.font_size + 4.f) - style_.box_size) * 0.5f;
            float r   = style_.box_size * 0.5f;

            if (custom_renderer_) {
                float total_w = style_.box_size + style_.gap
                              + measure_text_width(label_, style_.font_family, style_.font_size);
                float total_h = std::max(style_.box_size, style_.font_size + 4.f);
                custom_renderer_(ch, checked_t_, hover_t_, label_, total_w, total_h, style_);
                return;
            }

            Color bg  = disabled_ ? style_.disabled_color
                                  : Color::lerp(
                                        Color::lerp(style_.bg_off,       style_.bg_hover_off, hover_t_),
                                        Color::lerp(style_.bg_on,        style_.bg_hover_on,  hover_t_),
                                        checked_t_);
            Color bdr = disabled_ ? style_.disabled_color
                                  : Color::lerp(style_.border_off, style_.border_on, checked_t_);

            // 外圆
            ch.fill_circle(bx + r, by + r, r, bg);
            ch.stroke_circle(bx + r, by + r, r, style_.border_width, bdr);

            // 内实心圆（checked_t 控制半径）
            if (checked_t_ > 0.01f) {
                float ir = r * 0.42f * checked_t_;
                ch.fill_circle(bx + r, by + r, ir, style_.check_color);
            }

            // 文字
            float ty  = (std::max(style_.box_size, style_.font_size + 4.f) - style_.font_size) * 0.5f;
            TextStyle ts;
            ts.font_family = style_.font_family;
            ts.font_size   = style_.font_size;
            ts.color       = disabled_ ? style_.disabled_color : style_.text_color;
            ts.weight      = static_cast<TextStyle::Weight>(style_.font_weight);
            ch.text(label_, style_.box_size + style_.gap + 2.f, ty, ts);
        }

        Layer*       layer_   = nullptr;
        ToggleStyle  style_;
        std::wstring label_;
        bool         checked_  = false;
        bool         disabled_ = false;

        float hover_t_      = 0.f;
        float hover_target_ = 0.f;
        float checked_t_    = 0.f;

        std::function<void(bool)> on_change_cb_;
        std::function<void(bool)> group_change_cb_;  // RadioGroup 内部使用，不被 on_change() 覆盖
        Renderer                  custom_renderer_;
    };


    // ============================================================
    //  RadioGroup — 管理一组 RadioButton，保证互斥
    // ============================================================
    class RadioGroup
    {
    public:
        RadioGroup() = default;

        // 注册一个按钮到此组（返回该按钮引用）
        RadioButtonEx& add(RadioButtonEx& btn)
        {
            buttons_.push_back(&btn);
            RadioButtonEx* p = &btn;  // 捕获指针，避免悬空引用 UB
            p->set_group_cb_([this, p](bool checked) {
                if (checked) {
                    for (auto* b : buttons_)
                        if (b != p) b->set_checked(false);
                    selected_ = index_of(*p);
                    if (group_cb_) group_cb_(selected_);
                }
            });
            return btn;
        }

        RadioGroup& on_change(std::function<void(int)> cb)
        { group_cb_ = std::move(cb); return *this; }

        int selected() const { return selected_; }

        void select(int idx)
        {
            if (idx < 0 || idx >= (int)buttons_.size()) return;
            buttons_[idx]->set_checked(true);
        }

    private:
        int index_of(const RadioButtonEx& btn) const
        {
            for (int i = 0; i < (int)buttons_.size(); ++i)
                if (buttons_[i] == &btn) return i;
            return -1;
        }

        std::vector<RadioButtonEx*> buttons_;
        int selected_ = -1;
        std::function<void(int)> group_cb_;
    };


    // ============================================================
    //  Checkbox — 多选框
    //
    //  API 与 RadioButton 完全对齐。
    //  点击切换 checked 状态，支持三态（indeterminate）。
    //
    //  自定义渲染：set_renderer(fn) 同 RadioButton
    // ============================================================
    class CheckboxEx
    {
    public:
        CheckboxEx(Layer* parent, std::string_view name,
                 float x, float y,
                 std::wstring_view label,
                 const ToggleStyle& style = ToggleStyle{})
            : style_(style), label_(label)
        {
            float total_w = style_.box_size + style_.gap
                          + measure_text_width(label_, style_.font_family, style_.font_size);
            float total_h = std::max(style_.box_size, style_.font_size + 4.f);

            layer_ = parent->creat(std::string(name));
            layer_->translate(x, y)
                   .layer_bounds(0, 0, total_w + 4.f, total_h)
                   .hit_area(0, 0, total_w + 4.f, total_h);

            layer_->on_mouse_enter([this](const MouseEvent&) -> bool {
                hover_target_ = 1.f; return false;
            });
            layer_->on_mouse_leave([this](const MouseEvent&) -> bool {
                hover_target_ = 0.f; return false;
            });
            layer_->on_mouse_down([this](const MouseEvent& e) -> bool {
                return e.left_down; // 消费 down，阻止 draggable 窗口抢夺
            });
            layer_->on_click([this](const MouseEvent&) -> bool {
                if (!disabled_) {
                    // 三态循环：unchecked → checked → indeterminate → unchecked
                    if (tri_state_) {
                        state_ = (CheckState)(((int)state_ + 1) % 3);
                    } else {
                        state_ = (state_ == CheckState::Checked)
                                 ? CheckState::Unchecked : CheckState::Checked;
                    }
                    if (on_change_cb_) on_change_cb_(state_);
                }
                return true;
            });
            layer_->on_render([this](PaintChain& ch, float dt) -> bool {
                tick_anim(dt);
                draw(ch);
                return true;
            });
        }

        // ── 状态 ─────────────────────────────────────────────────
        enum class CheckState { Unchecked = 0, Checked = 1, Indeterminate = 2 };

        CheckState check_state() const { return state_; }
        bool is_checked()   const { return state_ == CheckState::Checked; }
        bool is_disabled()  const { return disabled_; }

        CheckboxEx& set_state(CheckState s)
        {
            state_ = s;
            if (on_change_cb_) on_change_cb_(s);
            return *this;
        }

        CheckboxEx& checked(bool v)
        {
            return set_state(v ? CheckState::Checked : CheckState::Unchecked);
        }

        CheckboxEx& disabled(bool d = true) { disabled_ = d; return *this; }
        CheckboxEx& tri_state(bool t = true){ tri_state_ = t; return *this; }

        CheckboxEx& label(std::wstring_view t)
        {
            label_ = std::wstring(t);
            recalc_size();
            return *this;
        }

        // ── 样式 API ─────────────────────────────────────────────
        CheckboxEx& font_size(float s)  { style_.font_size = s; recalc_size(); return *this; }
        CheckboxEx& box_size(float s)   { style_.box_size = s;  recalc_size(); return *this; }
        CheckboxEx& text_color(Color c) { style_.text_color = c;  return *this; }
        CheckboxEx& bg_color(Color off, Color on) { style_.bg_off = off; style_.bg_on = on; return *this; }
        CheckboxEx& border_color(Color off, Color on) { style_.border_off = off; style_.border_on = on; return *this; }
        CheckboxEx& check_color(Color c){ style_.check_color = c; return *this; }
        CheckboxEx& anim_speed(float s) { style_.anim_speed = s;  return *this; }
        CheckboxEx& border_radius(float r){ box_radius_ = r; return *this; }

        // ── 回调 ─────────────────────────────────────────────────
        CheckboxEx& on_change(std::function<void(CheckState)> cb)
        { on_change_cb_ = std::move(cb); return *this; }

        // ── 自定义渲染（接口与 RadioButton 完全一致）─────────────
        using Renderer = std::function<void(PaintChain&, float /*checked_t*/,
                                             float /*hover_t*/,
                                             const std::wstring& /*label*/,
                                             float /*w*/, float /*h*/,
                                             const ToggleStyle&)>;
        CheckboxEx& set_renderer(Renderer fn)
        { custom_renderer_ = std::move(fn); return *this; }

        Layer* layer() const { return layer_; }

        // 禁止拷贝/移动（内部 lambda 捕获了 this）
        CheckboxEx(const CheckboxEx&) = delete;
        CheckboxEx& operator=(const CheckboxEx&) = delete;
        CheckboxEx(CheckboxEx&&) = delete;
        CheckboxEx& operator=(CheckboxEx&&) = delete;

    private:
        void recalc_size()
        {
            float total_w = style_.box_size + style_.gap
                          + measure_text_width(label_, style_.font_family, style_.font_size);
            float total_h = std::max(style_.box_size, style_.font_size + 4.f);
            layer_->layer_bounds(0, 0, total_w + 4.f, total_h)
                   .hit_area(0, 0, total_w + 4.f, total_h);
        }

        void tick_anim(float dt)
        {
            auto lerp_f = [](float c, float t, float s, float d) {
                return c + (t - c) * std::min(1.f, s * d);
            };
            float tgt = (state_ == CheckState::Checked) ? 1.f
                      : (state_ == CheckState::Indeterminate) ? 0.5f : 0.f;
            hover_t_   = lerp_f(hover_t_,   hover_target_, style_.hover_speed, dt);
            checked_t_ = lerp_f(checked_t_, tgt,           style_.anim_speed,  dt);
        }

        void draw(PaintChain& ch)
        {
            float bx = 2.f;
            float by = (std::max(style_.box_size, style_.font_size + 4.f) - style_.box_size) * 0.5f;
            float bs = style_.box_size;

            if (custom_renderer_) {
                float total_w = style_.box_size + style_.gap
                              + measure_text_width(label_, style_.font_family, style_.font_size);
                float total_h = std::max(style_.box_size, style_.font_size + 4.f);
                custom_renderer_(ch, checked_t_, hover_t_, label_, total_w, total_h, style_);
                return;
            }

            Color bg  = disabled_ ? style_.disabled_color
                                  : Color::lerp(
                                        Color::lerp(style_.bg_off,  style_.bg_hover_off, hover_t_),
                                        Color::lerp(style_.bg_on,   style_.bg_hover_on,  hover_t_),
                                        checked_t_);
            Color bdr = disabled_ ? style_.disabled_color
                                  : Color::lerp(style_.border_off, style_.border_on, checked_t_);

            // 方框背景
            ch.fill_round_rect(bx, by, bs, bs, box_radius_, bg);
            ch.stroke_round_rect(bx, by, bs, bs, box_radius_, StrokeStyle{ bdr, style_.border_width });

            // 勾选标记
            if (checked_t_ > 0.01f) {
                if (state_ == CheckState::Indeterminate || checked_t_ < 0.6f) {
                    // 横线（三态或动画过渡）
                    float mx = bx + bs * 0.2f;
                    float my = by + bs * 0.5f;
                    float mw = bs * 0.6f * checked_t_;
                    Color cc = style_.check_color;
                    cc.a *= checked_t_;
                    StrokeStyle ss{ cc, 2.f };
                    ss.cap = StrokeStyle::LineCap::Round;
                    ch.move_to(mx, my).line_to(mx + mw, my).stroke(ss);
                } else {
                    // 打勾 ✓
                    float t = (checked_t_ - 0.6f) / 0.4f; // 0~1
                    float x1 = bx + bs * 0.18f, y1 = by + bs * 0.52f;
                    float x2 = bx + bs * 0.42f, y2 = by + bs * 0.72f;
                    float x3 = bx + bs * 0.82f, y3 = by + bs * 0.28f;

                    Color cc = style_.check_color;
                    StrokeStyle ss{ cc, 2.f };
                    ss.cap = StrokeStyle::LineCap::Round;
                    ss.join = StrokeStyle::LineJoin::Round;

                    if (t < 0.5f) {
                        // 第一段：x1→x2
                        float seg_t = t * 2.f;
                        float px = x1 + (x2 - x1) * seg_t;
                        float py = y1 + (y2 - y1) * seg_t;
                        ch.move_to(x1, y1).line_to(px, py).stroke(ss);
                    } else {
                        // 两段全画
                        float seg_t = (t - 0.5f) * 2.f;
                        float px = x2 + (x3 - x2) * seg_t;
                        float py = y2 + (y3 - y2) * seg_t;
                        ch.move_to(x1, y1).line_to(x2, y2).stroke(ss);
                        ch.move_to(x2, y2).line_to(px, py).stroke(ss);
                    }
                }
            }

            // 文字
            float ty = (std::max(style_.box_size, style_.font_size + 4.f) - style_.font_size) * 0.5f;
            TextStyle ts;
            ts.font_family = style_.font_family;
            ts.font_size   = style_.font_size;
            ts.color       = disabled_ ? style_.disabled_color : style_.text_color;
            ts.weight      = static_cast<TextStyle::Weight>(style_.font_weight);
            ch.text(label_, style_.box_size + style_.gap + 2.f, ty, ts);
        }

        Layer*       layer_   = nullptr;
        ToggleStyle  style_;
        std::wstring label_;
        CheckState   state_    = CheckState::Unchecked;
        bool         disabled_ = false;
        bool         tri_state_= false;
        float        box_radius_= 4.f;

        float hover_t_      = 0.f;
        float hover_target_ = 0.f;
        float checked_t_    = 0.f;

        std::function<void(CheckState)> on_change_cb_;
        Renderer                        custom_renderer_;
    };

} // namespace Otter
