#include "dragonbytez/png.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>

namespace dragonbytez {

namespace {

void append_be32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 24));
    output.push_back(static_cast<std::uint8_t>(value >> 16));
    output.push_back(static_cast<std::uint8_t>(value >> 8));
    output.push_back(static_cast<std::uint8_t>(value));
}

std::uint32_t crc32_bytes(
    const std::uint8_t* data,
    std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (unsigned bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask =
                0U - static_cast<std::uint32_t>(crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

std::uint32_t adler32_bytes(const std::vector<std::uint8_t>& data) {
    constexpr std::uint32_t modulus = 65521;
    std::uint32_t first = 1;
    std::uint32_t second = 0;
    for (const std::uint8_t value : data) {
        first = (first + value) % modulus;
        second = (second + first) % modulus;
    }
    return (second << 16U) | first;
}

std::vector<std::uint8_t> make_zlib_stored_stream(
    const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> output;
    output.reserve(data.size() + data.size() / 65535 * 5 + 11);

    output.push_back(0x78);
    output.push_back(0x01);

    std::size_t position = 0;
    do {
        const std::size_t remaining = data.size() - position;
        const std::size_t block_size = std::min<std::size_t>(65535, remaining);
        const bool final_block = position + block_size == data.size();
        const std::uint16_t length = static_cast<std::uint16_t>(block_size);
        const std::uint16_t inverse_length =
            static_cast<std::uint16_t>(~length);

        output.push_back(final_block ? 0x01 : 0x00);
        output.push_back(static_cast<std::uint8_t>(length));
        output.push_back(static_cast<std::uint8_t>(length >> 8U));
        output.push_back(static_cast<std::uint8_t>(inverse_length));
        output.push_back(static_cast<std::uint8_t>(inverse_length >> 8U));
        output.insert(
            output.end(),
            data.begin() + static_cast<std::ptrdiff_t>(position),
            data.begin() + static_cast<std::ptrdiff_t>(position + block_size));
        position += block_size;
    } while (position < data.size());

    append_be32(output, adler32_bytes(data));
    return output;
}

void append_chunk(
    std::vector<std::uint8_t>& output,
    const char type[4],
    const std::vector<std::uint8_t>& data) {
    append_be32(output, static_cast<std::uint32_t>(data.size()));
    const std::size_t type_start = output.size();
    output.insert(output.end(), type, type + 4);
    output.insert(output.end(), data.begin(), data.end());
    const std::uint32_t crc =
        crc32_bytes(output.data() + type_start, 4 + data.size());
    append_be32(output, crc);
}

double hue_component(double p, double q, double t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6.0) return p + (q - p) * 6 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6;
    return p;
}

Rgba hsl(double h, double s, double l) {
    const double q = l < 0.5 ? l * (1 + s) : l + s - l * s;
    const double p = 2 * l - q;
    return {
        static_cast<std::uint8_t>(std::lround(255 * hue_component(p, q, h + 1.0 / 3.0))),
        static_cast<std::uint8_t>(std::lround(255 * hue_component(p, q, h))),
        static_cast<std::uint8_t>(std::lround(255 * hue_component(p, q, h - 1.0 / 3.0))),
        255};
}

} // namespace

void write_png_rgba(
    const std::filesystem::path& path,
    unsigned width,
    unsigned height,
    const std::vector<Rgba>& pixels) {
    if (width == 0 || height == 0 ||
        pixels.size() != static_cast<std::size_t>(width) * height) {
        throw std::invalid_argument("invalid PNG dimensions or pixel count");
    }
    std::vector<std::uint8_t> scanlines;
    scanlines.reserve((static_cast<std::size_t>(width) * 4 + 1) * height);
    for (unsigned y = 0; y < height; ++y) {
        scanlines.push_back(0);
        for (unsigned x = 0; x < width; ++x) {
            const auto& pixel = pixels[static_cast<std::size_t>(y) * width + x];
            scanlines.insert(scanlines.end(), {pixel.r, pixel.g, pixel.b, pixel.a});
        }
    }
    const std::vector<std::uint8_t> compressed =
        make_zlib_stored_stream(scanlines);

    std::vector<std::uint8_t> png = {137, 80, 78, 71, 13, 10, 26, 10};
    std::vector<std::uint8_t> ihdr;
    append_be32(ihdr, width);
    append_be32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    append_chunk(png, "IHDR", ihdr);
    append_chunk(png, "IDAT", compressed);
    append_chunk(png, "IEND", {});

    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(png.data()),
                 static_cast<std::streamsize>(png.size()));
    if (!output) throw std::runtime_error("cannot write PNG: " + path.string());
}

std::vector<Rgba> diagnostic_palette() {
    std::vector<Rgba> result(256);
    result[0] = {0, 0, 0, 0};
    for (unsigned value = 1; value < 256; ++value) {
        const double hue = std::fmod(value * 0.618033988749895, 1.0);
        const double saturation = 0.55 + ((value * 17) % 30) / 100.0;
        const double lightness = 0.48 + ((value * 29) % 18) / 100.0;
        result[value] = hsl(hue, saturation, lightness);
    }
    return result;
}

std::vector<Rgba> gba_rgb555_palette(const std::vector<std::uint8_t>& raw) {
    std::vector<Rgba> result(256, {0, 0, 0, 255});
    const std::size_t count = std::min<std::size_t>(256, raw.size() / 2);
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint16_t value = raw[index * 2] |
            (static_cast<std::uint16_t>(raw[index * 2 + 1]) << 8);
        const auto expand = [](unsigned part) {
            return static_cast<std::uint8_t>((part << 3) | (part >> 2));
        };
        result[index] = {
            expand(value & 31), expand((value >> 5) & 31),
            expand((value >> 10) & 31), 255};
    }
    return result;
}

std::vector<std::uint8_t> untile_8bpp(
    const std::vector<std::uint8_t>& raw,
    unsigned width,
    unsigned height) {
    if (width == 0 || height == 0 || width % 8 || height % 8 ||
        raw.size() < static_cast<std::size_t>(width) * height) {
        throw std::invalid_argument("invalid 8bpp tiled image dimensions");
    }
    std::vector<std::uint8_t> result(static_cast<std::size_t>(width) * height);
    std::size_t source = 0;
    for (unsigned tile_y = 0; tile_y < height; tile_y += 8) {
        for (unsigned tile_x = 0; tile_x < width; tile_x += 8) {
            for (unsigned y = 0; y < 8; ++y) {
                for (unsigned x = 0; x < 8; ++x) {
                    result[static_cast<std::size_t>(tile_y + y) * width +
                           tile_x + x] = raw[source++];
                }
            }
        }
    }
    return result;
}

std::vector<std::uint8_t> untile_4bpp(
    const std::vector<std::uint8_t>& raw,
    unsigned width,
    unsigned height) {
    if (width == 0 || height == 0 || width % 8 || height % 8 ||
        raw.size() < static_cast<std::size_t>(width) * height / 2) {
        throw std::invalid_argument("invalid 4bpp tiled image dimensions");
    }
    std::vector<std::uint8_t> result(static_cast<std::size_t>(width) * height);
    std::size_t source = 0;
    for (unsigned tile_y = 0; tile_y < height; tile_y += 8) {
        for (unsigned tile_x = 0; tile_x < width; tile_x += 8) {
            for (unsigned y = 0; y < 8; ++y) {
                for (unsigned x = 0; x < 8; x += 2) {
                    const std::uint8_t packed = raw[source++];
                    const std::size_t destination =
                        static_cast<std::size_t>(tile_y + y) * width +
                        tile_x + x;
                    result[destination] = packed & 0x0FU;
                    result[destination + 1] = packed >> 4U;
                }
            }
        }
    }
    return result;
}

std::vector<Rgba> colorize(
    const std::vector<std::uint8_t>& indices,
    const std::vector<Rgba>& palette) {
    if (palette.size() < 256) throw std::invalid_argument("palette is too small");
    std::vector<Rgba> result;
    result.reserve(indices.size());
    for (std::uint8_t index : indices) result.push_back(palette[index]);
    return result;
}

} // namespace dragonbytez
