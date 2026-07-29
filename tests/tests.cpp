#include "dragonbytez/compression.hpp"
#include "dragonbytez/gba_bios.hpp"
#include "dragonbytez/png.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    std::vector<std::uint8_t> raw(64);
    for (unsigned index = 0; index < raw.size(); ++index) {
        raw[index] = static_cast<std::uint8_t>(index);
    }
    const auto linear = dragonbytez::untile_8bpp(raw, 8, 8);
    assert(linear == raw);

    const std::vector<std::uint8_t> packed_4bpp(32, 0xA3);
    const auto linear_4bpp = dragonbytez::untile_4bpp(packed_4bpp, 8, 8);
    assert(linear_4bpp.size() == 64);
    for (std::size_t index = 0; index < linear_4bpp.size(); index += 2) {
        assert(linear_4bpp[index] == 3);
        assert(linear_4bpp[index + 1] == 10);
    }

    const auto palette = dragonbytez::diagnostic_palette();
    assert(palette.size() == 256);
    assert(palette[0].a == 0);

    const std::vector<std::uint8_t> rgb555 = {0x1F, 0x00, 0xE0, 0x03, 0x00, 0x7C};
    const auto decoded = dragonbytez::gba_rgb555_palette(rgb555);
    assert(decoded[0].r == 255 && decoded[0].g == 0 && decoded[0].b == 0);
    assert(decoded[1].r == 0 && decoded[1].g == 255 && decoded[1].b == 0);
    assert(decoded[2].r == 0 && decoded[2].g == 0 && decoded[2].b == 255);
    assert(decoded[255].a == 255);


    const std::vector<std::uint8_t> gba_lz77 = {
        0x10, 0x09, 0x00, 0x00,
        0x10, 'A', 'B', 'C', 0x30, 0x02};
    const auto gba_lz77_result =
        dragonbytez::decompress_gba_bios(gba_lz77, 0);
    assert(std::string(
        gba_lz77_result.data.begin(), gba_lz77_result.data.end()) ==
        "ABCABCABC");

    const std::vector<std::uint8_t> gba_rle = {
        0x30, 0x06, 0x00, 0x00,
        0x82, 'X', 0x00, 'Y'};
    const auto gba_rle_result =
        dragonbytez::decompress_gba_bios(gba_rle, 0);
    assert(std::string(
        gba_rle_result.data.begin(), gba_rle_result.data.end()) ==
        "XXXXXY");

    const std::vector<std::uint8_t> gba_huffman_8 = {
        0x28, 0x04, 0x00, 0x00,
        0x01, 0xC0, 'A', 'B',
        0x00, 0x00, 0x00, 0x60};
    const auto gba_huffman_8_result =
        dragonbytez::decompress_gba_bios(gba_huffman_8, 0);
    assert(std::string(
        gba_huffman_8_result.data.begin(),
        gba_huffman_8_result.data.end()) == "ABBA");

    const std::vector<std::uint8_t> gba_huffman_4 = {
        0x24, 0x02, 0x00, 0x00,
        0x01, 0xC0, 0x01, 0x02,
        0x00, 0x00, 0x00, 0x60};
    const auto gba_huffman_4_result =
        dragonbytez::decompress_gba_bios(gba_huffman_4, 0);
    assert(gba_huffman_4_result.data ==
           std::vector<std::uint8_t>({0x21, 0x12}));

    std::cout << "DragonByteZ tests passed\n";
}
