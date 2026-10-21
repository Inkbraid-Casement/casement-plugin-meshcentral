// Screen tiles: the agent sends changed parts of the screen as images
// (JPEG unless asked otherwise). They are decoded with the Windows Imaging
// Component and copied into the shadow picture at their position.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

struct IWICImagingFactory;

namespace meshcentral {

/// A decoded image: BGRX, 4 bytes per pixel, rows top first, no padding.
struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
};

/// A rectangle in screen pixels.
struct Rect {
    std::uint32_t x = 0, y = 0, width = 0, height = 0;
    bool empty() const { return width == 0 || height == 0; }
};

/// Decodes tiles. Uses COM: the thread must have called CoInitializeEx.
/// One decoder per thread.
class TileDecoder {
public:
    TileDecoder();
    ~TileDecoder();
    TileDecoder(const TileDecoder&) = delete;
    TileDecoder& operator=(const TileDecoder&) = delete;

    /// Whether WIC is available; `error` says why not.
    bool ok() const { return factory_ != nullptr; }
    const std::string& error() const { return error_; }

    /// Decodes an image file in memory (JPEG, PNG, BMP, TIFF, …).
    bool decode(std::span<const std::uint8_t> data, Image& out, std::string& error);

private:
    IWICImagingFactory* factory_ = nullptr;
    std::string error_;
};

/// Copies `image` into a BGRX picture of `width` x `height` pixels with
/// `stride` bytes per row, its top left corner at (x, y). Parts outside the
/// picture are left out. Returns the area written.
Rect blit(const Image& image, std::uint8_t* picture, std::size_t stride, std::uint32_t width,
          std::uint32_t height, std::uint32_t x, std::uint32_t y);

}  // namespace meshcentral
