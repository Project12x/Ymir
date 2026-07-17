#include <catch2/catch_test_macros.hpp>

#include <video_capture.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

TEST_CASE("Video capture converts numeric XBGR8888 to canonical RGBA8888", "[video-capture]") {
    const ymir::debug::CapturedVideoFrame frame{
        .width = 2,
        .height = 1,
        .sequence = 7,
        .xbgr8888Pixels = {0xFF332211, 0x80445566},
    };

    CHECK(ymir::debug::ConvertToRGBA8888(frame) ==
          std::vector<uint8_t>{0x11, 0x22, 0x33, 0xFF, 0x66, 0x55, 0x44, 0x80});
}

TEST_CASE("Video capture emits deterministic hashes and in-memory PNGs", "[video-capture]") {
    const std::vector<uint8_t> rgba8888{
        0x11, 0x22, 0x33, 0xFF, 0x66, 0x55, 0x44, 0x80,
    };

    const std::string hash = ymir::debug::HashRGBA8888(rgba8888);
    CHECK(hash.size() == 32);
    CHECK(hash == ymir::debug::HashRGBA8888(rgba8888));

    const auto png = ymir::debug::EncodePNG(rgba8888, 2, 1);
    constexpr std::array<uint8_t, 8> kPNGSignature{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    REQUIRE(png.size() > kPNGSignature.size());
    CHECK(std::equal(kPNGSignature.begin(), kPNGSignature.end(), png.begin()));
    CHECK(ymir::debug::EncodeBase64(png).starts_with("iVBORw0KGgo"));
}

TEST_CASE("Video capture uses padded RFC 4648 base64", "[video-capture]") {
    const auto encode = [](std::string input) {
        return ymir::debug::EncodeBase64(
            std::span<const uint8_t>{reinterpret_cast<const uint8_t *>(input.data()), input.size()});
    };

    CHECK(encode("") == "");
    CHECK(encode("f") == "Zg==");
    CHECK(encode("fo") == "Zm8=");
    CHECK(encode("foo") == "Zm9v");
    CHECK(encode("foobar") == "Zm9vYmFy");
}

TEST_CASE("Video capture rejects inconsistent dimensions", "[video-capture]") {
    const ymir::debug::CapturedVideoFrame frame{
        .width = 2,
        .height = 2,
        .xbgr8888Pixels = {0xFF000000},
    };
    CHECK_THROWS_AS(ymir::debug::ConvertToRGBA8888(frame), std::invalid_argument);

    const std::vector<uint8_t> shortBuffer{0x00, 0x00, 0x00, 0xFF};
    CHECK_THROWS_AS(ymir::debug::EncodePNG(shortBuffer, 2, 2), std::invalid_argument);
}
