#include "meshcentral/cursors.h"

#include <windows.h>

namespace meshcentral::cursors {

namespace {

std::uint16_t idc(LPCWSTR id) { return static_cast<std::uint16_t>(reinterpret_cast<std::uintptr_t>(id)); }

/// Reads a bitmap as 32-bit pixels, top row first.
bool read_bits(HDC dc, HBITMAP bitmap, int width, int height, std::vector<std::uint32_t>& out) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    out.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
    return GetDIBits(dc, bitmap, 0, static_cast<UINT>(height), out.data(), &bi, DIB_RGB_COLORS) == height;
}

}  // namespace

Kind for_index(std::uint8_t index, bool windows_agent) {
    // The order of KVM_MouseCursors in MeshAgent's KVM code; MeshCentral's web
    // client maps the same numbers to CSS cursors.
    if (index == 4 && !windows_agent) return {Shape::arrow};  // "can't name it"
    switch (index) {
        case 0: return {Shape::arrow};
        case 1: return {Shape::system, idc(IDC_APPSTARTING)};
        case 2: return {Shape::system, idc(IDC_CROSS)};
        case 3: return {Shape::system, idc(IDC_HAND)};
        case 4: return {Shape::system, idc(IDC_HELP)};
        case 5: return {Shape::system, idc(IDC_IBEAM)};
        case 6: return {Shape::system, idc(IDC_NO)};
        case 7: return {Shape::system, idc(IDC_SIZEALL)};
        case 8: return {Shape::system, idc(IDC_SIZENESW)};
        case 9: return {Shape::system, idc(IDC_SIZENS)};
        case 10: return {Shape::system, idc(IDC_SIZENWSE)};
        case 11: return {Shape::system, idc(IDC_SIZEWE)};
        case 12: return {Shape::system, idc(IDC_UPARROW)};
        case 13: return {Shape::system, idc(IDC_WAIT)};
        case 14: return {Shape::hidden};
        case 15: return {Shape::system, idc(IDC_NO)};      // not allowed
        case 16: return {Shape::system, idc(IDC_SIZEWE)};  // column resize
        case 17: return {Shape::system, idc(IDC_SIZENS)};  // row resize
        default: return {Shape::arrow};
    }
}

std::optional<Picture> render(std::uint16_t id) {
    HCURSOR cursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(id));
    if (!cursor) return std::nullopt;
    ICONINFO info{};
    if (!GetIconInfo(cursor, &info)) return std::nullopt;
    BITMAP mask{};
    GetObjectW(info.hbmMask, sizeof(mask), &mask);
    int width = mask.bmWidth;
    // Without a colour bitmap the mask holds the AND mask above the XOR mask.
    int height = info.hbmColor ? mask.bmHeight : mask.bmHeight / 2;
    std::optional<Picture> result;
    HDC dc = CreateCompatibleDC(nullptr);
    std::vector<std::uint32_t> masks, colours;
    if (width > 0 && height > 0 && dc && read_bits(dc, info.hbmMask, width, mask.bmHeight, masks) &&
        (!info.hbmColor || read_bits(dc, info.hbmColor, width, height, colours))) {
        Picture p;
        p.width = width;
        p.height = height;
        p.hot_x = static_cast<int>(info.xHotspot);
        p.hot_y = static_cast<int>(info.yHotspot);
        const auto w = static_cast<std::size_t>(width), h = static_cast<std::size_t>(height);
        p.rgba.assign(w * h * 4, 0);
        std::vector<bool> inverted(w * h, false);
        bool alpha = false;
        for (std::uint32_t c : colours) alpha = alpha || (c >> 24) != 0;
        for (std::size_t i = 0; i < inverted.size(); ++i) {
            std::uint8_t* out = &p.rgba[i * 4];
            bool and_bit = (masks[i] & 0xFFFFFF) != 0;
            std::uint32_t c;
            std::uint8_t a;
            if (!colours.empty()) {
                c = colours[i];
                if (alpha) a = static_cast<std::uint8_t>(c >> 24);
                else a = (!and_bit || (c & 0xFFFFFF) != 0) ? 255 : 0;
            } else {
                bool xor_bit = (masks[i + inverted.size()] & 0xFFFFFF) != 0;
                if (and_bit && xor_bit) inverted[i] = true;
                c = (!and_bit && xor_bit) ? 0xFFFFFF : 0;
                a = (and_bit && !xor_bit) ? 0 : 255;
            }
            out[0] = static_cast<std::uint8_t>(c >> 16);  // DIB pixels are BGRX
            out[1] = static_cast<std::uint8_t>(c >> 8);
            out[2] = static_cast<std::uint8_t>(c);
            out[3] = a;
        }
        // Pixels that invert the screen (the I-beam) can't be shown as such;
        // they are black, with a white edge so they stay visible on dark
        // backgrounds.
        for (std::size_t y = 0; y < h; ++y)
            for (std::size_t x = 0; x < w; ++x) {
                std::size_t i = y * w + x;
                if (p.rgba[i * 4 + 3] != 0) continue;
                bool edge = (x > 0 && inverted[i - 1]) || (x + 1 < w && inverted[i + 1]) ||
                            (y > 0 && inverted[i - w]) || (y + 1 < h && inverted[i + w]);
                if (edge) p.rgba[i * 4] = p.rgba[i * 4 + 1] = p.rgba[i * 4 + 2] = p.rgba[i * 4 + 3] = 255;
            }
        result = std::move(p);
    }
    if (dc) DeleteDC(dc);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    if (info.hbmColor) DeleteObject(info.hbmColor);
    return result;
}

casement::json message(const Kind& kind) {
    if (kind.shape == Shape::hidden) {
        std::uint8_t clear[4] = {0, 0, 0, 0};
        return casement::cursor_message(1, 1, 0, 0, clear);
    }
    if (kind.shape == Shape::system) {
        if (auto p = render(kind.idc)) return casement::cursor_message(p->width, p->height, p->hot_x, p->hot_y, p->rgba);
    }
    return casement::default_cursor_message();
}

}  // namespace meshcentral::cursors
