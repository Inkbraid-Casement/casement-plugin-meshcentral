#include "meshcentral/tiles.h"

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cstring>
#include <format>

#include "casement/text.h"

namespace meshcentral {

namespace {

/// Holds a COM interface pointer and releases it.
template <class T>
class Ref {
public:
    Ref() = default;
    ~Ref() {
        if (p_) p_->Release();
    }
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    T** put() { return &p_; }
    T* operator->() const { return p_; }
    T* get() const { return p_; }

private:
    T* p_ = nullptr;
};

std::string hresult_text(HRESULT hr) {
    std::string text = casement::system_message(static_cast<unsigned long>(hr));
    if (text.starts_with("error ")) return std::format("error {:#010x}", static_cast<unsigned long>(hr));
    return text;
}

/// Tiles are parts of one screen; anything larger is not a tile.
constexpr std::uint32_t max_side = 65535;
constexpr std::uint64_t max_pixels = std::uint64_t{16384} * 16384;

}  // namespace

TileDecoder::TileDecoder() {
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                  reinterpret_cast<void**>(&factory_));
    if (FAILED(hr)) {
        factory_ = nullptr;
        error_ = "the Windows Imaging Component isn't available: " + hresult_text(hr);
    }
}

TileDecoder::~TileDecoder() {
    if (factory_) factory_->Release();
}

bool TileDecoder::decode(std::span<const std::uint8_t> data, Image& out, std::string& error) {
    if (!factory_) {
        error = error_;
        return false;
    }
    if (data.empty() || data.size() > 0xFFFFFFFFu) {
        error = "empty or oversized image";
        return false;
    }
    auto fail = [&](const char* step, HRESULT hr) {
        error = std::format("{}: {}", step, hresult_text(hr));
        return false;
    };
    Ref<IWICStream> stream;
    HRESULT hr = factory_->CreateStream(stream.put());
    if (FAILED(hr)) return fail("CreateStream", hr);
    // WIC only reads the memory; the cast is for the API's signature.
    hr = stream->InitializeFromMemory(const_cast<BYTE*>(data.data()), static_cast<DWORD>(data.size()));
    if (FAILED(hr)) return fail("InitializeFromMemory", hr);
    Ref<IWICBitmapDecoder> decoder;
    hr = factory_->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put());
    if (FAILED(hr)) return fail("no decoder for the image", hr);
    Ref<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.put());
    if (FAILED(hr)) return fail("GetFrame", hr);
    UINT width = 0, height = 0;
    hr = frame->GetSize(&width, &height);
    if (FAILED(hr)) return fail("GetSize", hr);
    if (width == 0 || height == 0 || width > max_side || height > max_side ||
        std::uint64_t{width} * height > max_pixels) {
        error = std::format("unexpected image size {}x{}", width, height);
        return false;
    }
    Ref<IWICFormatConverter> converter;
    hr = factory_->CreateFormatConverter(converter.put());
    if (FAILED(hr)) return fail("CreateFormatConverter", hr);
    hr = converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return fail("converting to BGRX", hr);
    out.width = width;
    out.height = height;
    std::size_t stride = std::size_t{width} * 4;
    out.pixels.resize(stride * height);
    hr = converter->CopyPixels(nullptr, static_cast<UINT>(stride), static_cast<UINT>(out.pixels.size()),
                               out.pixels.data());
    if (FAILED(hr)) return fail("CopyPixels", hr);
    return true;
}

Rect blit(const Image& image, std::uint8_t* picture, std::size_t stride, std::uint32_t width,
          std::uint32_t height, std::uint32_t x, std::uint32_t y) {
    if (x >= width || y >= height || image.width == 0 || image.height == 0) return {};
    std::uint32_t w = std::min(image.width, width - x);
    std::uint32_t h = std::min(image.height, height - y);
    const std::uint8_t* src = image.pixels.data();
    std::size_t src_stride = std::size_t{image.width} * 4;
    for (std::uint32_t row = 0; row < h; ++row)
        std::memcpy(picture + std::size_t{y + row} * stride + std::size_t{x} * 4, src + row * src_stride,
                    std::size_t{w} * 4);
    return Rect{x, y, w, h};
}

}  // namespace meshcentral
