#include "video_capture.hpp"

#include <xxh3.h>

#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace ymir::debug {

namespace {

    constexpr std::string_view kBase64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    constexpr std::string_view kHexAlphabet = "0123456789abcdef";

    struct PNGWriteContext {
        std::vector<uint8_t> bytes;
        bool failed{};
    };

    void WritePNGBytes(void *context, void *data, int size) noexcept {
        auto &writeContext = *static_cast<PNGWriteContext *>(context);
        if (writeContext.failed || size <= 0) {
            return;
        }

        try {
            const auto *first = static_cast<const uint8_t *>(data);
            writeContext.bytes.insert(writeContext.bytes.end(), first, first + size);
        } catch (...) {
            writeContext.failed = true;
        }
    }

} // namespace

std::vector<uint8_t> ConvertToRGBA8888(const CapturedVideoFrame &frame) {
    const size_t pixelCount = static_cast<size_t>(frame.width) * frame.height;
    if (frame.width == 0 || frame.height == 0 || frame.xbgr8888Pixels.size() != pixelCount) {
        throw std::invalid_argument("captured frame dimensions do not match its pixel buffer");
    }

    std::vector<uint8_t> rgba8888(pixelCount * 4);
    for (size_t i = 0; i < pixelCount; ++i) {
        const uint32_t pixel = frame.xbgr8888Pixels[i];
        rgba8888[i * 4 + 0] = static_cast<uint8_t>(pixel >> 0);
        rgba8888[i * 4 + 1] = static_cast<uint8_t>(pixel >> 8);
        rgba8888[i * 4 + 2] = static_cast<uint8_t>(pixel >> 16);
        rgba8888[i * 4 + 3] = static_cast<uint8_t>(pixel >> 24);
    }
    return rgba8888;
}

std::string HashRGBA8888(std::span<const uint8_t> rgba8888) {
    const XXH128_hash_t hash = XXH3_128bits(rgba8888.data(), rgba8888.size());
    XXH128_canonical_t canonicalHash{};
    XXH128_canonicalFromHash(&canonicalHash, hash);

    std::string result(sizeof(canonicalHash.digest) * 2, '\0');
    for (size_t i = 0; i < sizeof(canonicalHash.digest); ++i) {
        result[i * 2 + 0] = kHexAlphabet[canonicalHash.digest[i] >> 4];
        result[i * 2 + 1] = kHexAlphabet[canonicalHash.digest[i] & 0x0F];
    }
    return result;
}

std::vector<uint8_t> EncodePNG(std::span<const uint8_t> rgba8888, uint32_t width, uint32_t height) {
    const size_t pixelCount = static_cast<size_t>(width) * height;
    if (width == 0 || height == 0 || rgba8888.size() != pixelCount * 4 ||
        width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        height > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        width > static_cast<uint32_t>(std::numeric_limits<int>::max() / 4)) {
        throw std::invalid_argument("RGBA8888 buffer does not match the requested PNG dimensions");
    }

    PNGWriteContext context;
    context.bytes.reserve(rgba8888.size() / 2);
    const int encoded =
        stbi_write_png_to_func(&WritePNGBytes, &context, static_cast<int>(width), static_cast<int>(height), 4,
                               rgba8888.data(), static_cast<int>(width * 4));
    if (encoded == 0 || context.failed) {
        throw std::runtime_error("failed to encode captured frame as PNG");
    }
    return context.bytes;
}

std::string EncodeBase64(std::span<const uint8_t> bytes) {
    std::string result(((bytes.size() + 2) / 3) * 4, '=');
    size_t inputOffset = 0;
    size_t outputOffset = 0;

    while (inputOffset + 3 <= bytes.size()) {
        const uint32_t block = (static_cast<uint32_t>(bytes[inputOffset + 0]) << 16) |
                               (static_cast<uint32_t>(bytes[inputOffset + 1]) << 8) |
                               static_cast<uint32_t>(bytes[inputOffset + 2]);
        result[outputOffset + 0] = kBase64Alphabet[(block >> 18) & 0x3F];
        result[outputOffset + 1] = kBase64Alphabet[(block >> 12) & 0x3F];
        result[outputOffset + 2] = kBase64Alphabet[(block >> 6) & 0x3F];
        result[outputOffset + 3] = kBase64Alphabet[block & 0x3F];
        inputOffset += 3;
        outputOffset += 4;
    }

    const size_t remaining = bytes.size() - inputOffset;
    if (remaining != 0) {
        uint32_t block = static_cast<uint32_t>(bytes[inputOffset]) << 16;
        if (remaining == 2) {
            block |= static_cast<uint32_t>(bytes[inputOffset + 1]) << 8;
        }
        result[outputOffset + 0] = kBase64Alphabet[(block >> 18) & 0x3F];
        result[outputOffset + 1] = kBase64Alphabet[(block >> 12) & 0x3F];
        if (remaining == 2) {
            result[outputOffset + 2] = kBase64Alphabet[(block >> 6) & 0x3F];
        }
    }
    return result;
}

} // namespace ymir::debug
