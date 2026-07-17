#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ymir::debug {

/// @brief A copy of the latest completed software-renderer frame.
struct CapturedVideoFrame {
    uint32_t width{};
    uint32_t height{};
    uint64_t sequence{};
    std::vector<uint32_t> xbgr8888Pixels;
};

/// @brief Converts Ymir's numeric XBGR8888 pixels to canonical RGBA8888 bytes.
/// @param[in] frame the captured software-renderer frame
/// @return tightly packed RGBA8888 pixels in top-to-bottom row order
[[nodiscard]] std::vector<uint8_t> ConvertToRGBA8888(const CapturedVideoFrame &frame);

/// @brief Computes a platform-independent xxh3-128 hash of RGBA8888 pixel bytes.
/// @param[in] rgba8888 tightly packed canonical pixel bytes
/// @return the lowercase 32-character canonical hexadecimal hash
[[nodiscard]] std::string HashRGBA8888(std::span<const uint8_t> rgba8888);

/// @brief Encodes canonical RGBA8888 pixels as a PNG held entirely in memory.
/// @param[in] rgba8888 tightly packed canonical pixel bytes
/// @param[in] width frame width in pixels
/// @param[in] height frame height in pixels
/// @return encoded PNG bytes
[[nodiscard]] std::vector<uint8_t> EncodePNG(std::span<const uint8_t> rgba8888, uint32_t width, uint32_t height);

/// @brief Encodes bytes using padded RFC 4648 base64 without line breaks.
/// @param[in] bytes binary input
/// @return base64 text
[[nodiscard]] std::string EncodeBase64(std::span<const uint8_t> bytes);

} // namespace ymir::debug
