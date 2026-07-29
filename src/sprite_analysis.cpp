#include "dragonbytez/sprite_analysis.hpp"

#include "dragonbytez/compression.hpp"
#include "dragonbytez/png.hpp"
#include "dragonbytez/rom.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace dragonbytez {

namespace {

constexpr std::size_t log2_europe_character_table = 0x00692E68U;
constexpr std::size_t log2_character_count = 150U;
constexpr std::size_t log2_first_sprite_id = 7U;
constexpr std::size_t buus_fury_character_table = 0x006B6BDCU;
constexpr std::size_t buus_fury_character_count = 297U;

std::string hex_value(std::uint64_t value, unsigned digits = 8) {
    std::ostringstream output;
    output << "0x" << std::uppercase << std::hex << std::setw(static_cast<int>(digits))
           << std::setfill('0') << value;
    return output.str();
}

std::string numbered_name(
    const std::string& prefix,
    std::size_t value,
    unsigned digits,
    const std::string& suffix) {
    std::ostringstream output;
    output << prefix << std::setw(static_cast<int>(digits)) << std::setfill('0')
           << value << suffix;
    return output.str();
}

std::string html_escape(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text) {
        switch (character) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        case '\'': escaped += "&#39;"; break;
        default: escaped.push_back(character); break;
        }
    }
    return escaped;
}

std::int32_t signed_u32(std::uint32_t value) {
    return static_cast<std::int32_t>(value);
}

bool valid_container_header(
    const Rom& rom,
    std::size_t offset,
    std::size_t minimum_size = 1,
    std::size_t maximum_size = 4U * 1024U * 1024U) {
    if (offset > rom.size() || rom.size() - offset < 8U) return false;
    const std::uint32_t kind = rom.u32(offset);
    const std::uint32_t declared = rom.u32(offset + 4U);
    return kind <= 2U && declared >= minimum_size && declared <= maximum_size;
}

std::vector<Rgba> object_palette(
    const Rom& rom,
    std::size_t offset,
    std::size_t byte_count = 512U) {
    if (offset == 0U || offset >= rom.size()) return diagnostic_palette();
    byte_count = std::min(byte_count, rom.size() - offset);
    byte_count &= ~std::size_t(1U);
    if (byte_count < 2U) return diagnostic_palette();
    auto palette = gba_rgb555_palette(rom.slice(offset, byte_count));
    if (palette.empty()) return diagnostic_palette();
    palette[0].a = 0;
    while (palette.size() < 256U) {
        palette.push_back({255, 0, 255, 255});
    }
    return palette;
}

struct ImageFrame {
    unsigned width = 0;
    unsigned height = 0;
    int origin_x = 0;
    int origin_y = 0;
    std::vector<Rgba> pixels;

    bool empty() const noexcept {
        return width == 0U || height == 0U || pixels.empty();
    }
};

void copy_frame(
    std::vector<Rgba>& destination,
    unsigned destination_width,
    unsigned destination_height,
    int destination_x,
    int destination_y,
    const ImageFrame& source) {
    if (source.empty()) return;
    for (unsigned y = 0; y < source.height; ++y) {
        const int target_y = destination_y + static_cast<int>(y);
        if (target_y < 0 || target_y >= static_cast<int>(destination_height)) continue;
        for (unsigned x = 0; x < source.width; ++x) {
            const int target_x = destination_x + static_cast<int>(x);
            if (target_x < 0 || target_x >= static_cast<int>(destination_width)) continue;
            const Rgba& source_pixel =
                source.pixels[static_cast<std::size_t>(y) * source.width + x];
            if (source_pixel.a == 0U) continue;
            destination[static_cast<std::size_t>(target_y) * destination_width +
                        static_cast<unsigned>(target_x)] = source_pixel;
        }
    }
}

void copy_tile_8bpp(
    const std::vector<std::uint8_t>& graphics,
    std::size_t tile_index,
    bool horizontal_flip,
    bool vertical_flip,
    const std::vector<Rgba>& palette,
    std::vector<Rgba>& pixels,
    unsigned width,
    unsigned height,
    unsigned destination_x,
    unsigned destination_y) {
    const std::size_t tile_offset = tile_index * 64U;
    if (tile_offset > graphics.size() || graphics.size() - tile_offset < 64U) return;
    for (unsigned y = 0; y < 8U; ++y) {
        const unsigned source_y = vertical_flip ? 7U - y : y;
        const unsigned target_y = destination_y + y;
        if (target_y >= height) continue;
        for (unsigned x = 0; x < 8U; ++x) {
            const unsigned source_x = horizontal_flip ? 7U - x : x;
            const unsigned target_x = destination_x + x;
            if (target_x >= width) continue;
            const std::uint8_t colour =
                graphics[tile_offset + source_y * 8U + source_x];
            if (colour == 0U || colour >= palette.size()) continue;
            pixels[static_cast<std::size_t>(target_y) * width + target_x] =
                palette[colour];
        }
    }
}

void copy_tile_4bpp(
    const std::vector<std::uint8_t>& graphics,
    std::size_t tile_index,
    bool horizontal_flip,
    bool vertical_flip,
    unsigned palette_bank,
    const std::vector<Rgba>& palette,
    std::vector<Rgba>& pixels,
    unsigned width,
    unsigned height,
    unsigned destination_x,
    unsigned destination_y) {
    const std::size_t tile_offset = tile_index * 32U;
    if (tile_offset > graphics.size() || graphics.size() - tile_offset < 32U) return;
    for (unsigned y = 0; y < 8U; ++y) {
        const unsigned source_y = vertical_flip ? 7U - y : y;
        const unsigned target_y = destination_y + y;
        if (target_y >= height) continue;
        for (unsigned x = 0; x < 8U; ++x) {
            const unsigned source_x = horizontal_flip ? 7U - x : x;
            const unsigned target_x = destination_x + x;
            if (target_x >= width) continue;
            const std::uint8_t packed =
                graphics[tile_offset + source_y * 4U + source_x / 2U];
            const unsigned local_colour =
                (source_x & 1U) != 0U ? packed >> 4U : packed & 15U;
            if (local_colour == 0U) continue;
            const unsigned colour = palette_bank * 16U + local_colour;
            if (colour >= palette.size()) continue;
            pixels[static_cast<std::size_t>(target_y) * width + target_x] =
                palette[colour];
        }
    }
}

struct Log2FrameRecord {
    std::size_t pointer = 0;
    int x = 0;
    int y = 0;
    unsigned width = 0;
    unsigned height = 0;
    bool horizontal_flip = false;
    bool vertical_flip = false;
    std::size_t graphics_container = 0;
};

struct Log2Animation {
    std::size_t index = 0;
    std::array<std::size_t, 4> sequence_pointers{};
    std::array<std::vector<ImageFrame>, 4> directions;

    unsigned frame_count() const noexcept {
        unsigned count = 0;
        for (const auto& direction : directions) {
            count = std::max(count, static_cast<unsigned>(direction.size()));
        }
        return count;
    }
};

struct Log2CharacterStructure {
    std::size_t sprite_id = 0;
    std::size_t pointer = 0;
    unsigned animation_count = 0;
    std::vector<std::uint32_t> sequence_pointer_values;
};

bool plausible_log2_frame_record(const Rom& rom, std::size_t offset) {
    if (offset > rom.size() || rom.size() - offset < 12U) return false;
    const unsigned width = rom.u8(offset + 2U);
    const unsigned height = rom.u8(offset + 3U);
    const auto valid_dimension = [](unsigned value) {
        return value == 8U || value == 16U || value == 32U || value == 64U;
    };
    if (!valid_dimension(width) || !valid_dimension(height)) return false;

    const std::uint32_t attributes = rom.u32(offset + 4U);
    const std::uint16_t attribute0 = attributes & 0xFFFFU;
    if ((attribute0 & 0x2000U) == 0U) return false;

    const std::uint32_t graphics_pointer = rom.u32(offset + 8U);
    if (!rom.is_rom_pointer(graphics_pointer)) return false;
    const std::size_t graphics = rom.pointer_to_offset(graphics_pointer);
    if (!valid_container_header(
            rom,
            graphics,
            static_cast<std::size_t>(width) * height,
            static_cast<std::size_t>(width) * height)) {
        return false;
    }
    return rom.u32(graphics + 4U) ==
        static_cast<std::uint32_t>(width * height);
}

bool plausible_log2_character_structure(const Rom& rom, std::size_t offset) {
    if (offset > rom.size() || rom.size() - offset < 0x50U) return false;
    const unsigned structure_flags = rom.u8(offset);
    const unsigned direction_mode = rom.u8(offset + 1U);
    const unsigned animation_count = rom.u8(offset + 2U);
    const unsigned record_mode = rom.u8(offset + 3U);
    if (structure_flags != 0U && structure_flags != 0x40U) return false;
    if (direction_mode != 0U && direction_mode != 1U && direction_mode != 2U &&
        direction_mode != 4U && direction_mode != 16U) {
        return false;
    }
    if (animation_count == 0U || animation_count > 32U) return false;
    if (record_mode != 8U && record_mode != 16U && record_mode != 24U &&
        record_mode != 32U && record_mode != 48U) {
        return false;
    }
    const std::uint32_t alignment = rom.u32(offset + 4U);
    if (alignment != 8U && alignment != 16U && alignment != 24U) return false;
    const std::uint32_t inherited = rom.u32(offset + 8U);
    if (inherited != 0U && !rom.is_rom_pointer(inherited)) return false;

    for (unsigned direction = 0; direction < 4U; ++direction) {
        const std::size_t rectangle = offset + 0x0CU + direction * 16U;
        const std::int32_t left = signed_u32(rom.u32(rectangle));
        const std::int32_t top = signed_u32(rom.u32(rectangle + 4U));
        const std::int32_t right = signed_u32(rom.u32(rectangle + 8U));
        const std::int32_t bottom = signed_u32(rom.u32(rectangle + 12U));
        if (left < -256 || left > 256 || top < -256 || top > 256 ||
            right < -256 || right > 256 || bottom < -256 || bottom > 256 ||
            right <= left || bottom <= top) {
            return false;
        }
    }

    const std::size_t pointer_table = offset + 0x4CU;
    const std::size_t pointer_count = static_cast<std::size_t>(animation_count) * 4U;
    if (pointer_table > rom.size() || pointer_count * 4U > rom.size() - pointer_table) {
        return false;
    }
    bool found_frame_sequence = false;
    for (std::size_t index = 0; index < pointer_count; ++index) {
        const std::uint32_t pointer = rom.u32(pointer_table + index * 4U);
        if (pointer == 0U) continue;
        if (!rom.is_rom_pointer(pointer)) return false;
        if (!found_frame_sequence &&
            plausible_log2_frame_record(rom, rom.pointer_to_offset(pointer))) {
            found_frame_sequence = true;
        }
    }
    return found_frame_sequence;
}

std::pair<std::size_t, std::size_t> locate_log2_character_table(const Rom& rom) {
    if (rom.game_code() == "ALFP" &&
        log2_europe_character_table + log2_character_count * 4U <= rom.size()) {
        unsigned valid = 0;
        for (std::size_t index = 0; index < log2_character_count; ++index) {
            const std::uint32_t pointer =
                rom.u32(log2_europe_character_table + index * 4U);
            if (rom.is_rom_pointer(pointer) &&
                plausible_log2_character_structure(
                    rom, rom.pointer_to_offset(pointer))) {
                ++valid;
            }
        }
        if (valid == log2_character_count) {
            return {log2_europe_character_table, log2_character_count};
        }
    }

    std::size_t best_table = 0U;
    unsigned best_score = 0U;
    std::size_t offset = 0U;
    while (offset + 4U <= rom.size()) {
        if (!rom.is_rom_pointer(rom.u32(offset))) {
            offset += 4U;
            continue;
        }
        const std::size_t run_start = offset;
        std::size_t run_count = 0U;
        while (offset + 4U <= rom.size() &&
               rom.is_rom_pointer(rom.u32(offset))) {
            ++run_count;
            offset += 4U;
        }
        if (run_count < log2_character_count) continue;
        for (std::size_t window = 0;
             window + log2_character_count <= run_count;
             ++window) {
            const std::size_t table = run_start + window * 4U;
            unsigned score = 0U;
            for (std::size_t index = 0; index < log2_character_count; ++index) {
                const std::uint32_t pointer = rom.u32(table + index * 4U);
                if (plausible_log2_character_structure(
                        rom, rom.pointer_to_offset(pointer))) {
                    ++score;
                }
            }
            if (score > best_score) {
                best_score = score;
                best_table = table;
            }
        }
    }
    if (best_table == 0U || best_score < 140U) {
        throw std::runtime_error(
            "could not locate the 150-entry LOG2 sprite-ID table");
    }
    return {best_table, log2_character_count};
}

Log2CharacterStructure read_log2_character_structure(
    const Rom& rom,
    std::size_t sprite_id,
    std::size_t pointer) {
    if (!plausible_log2_character_structure(rom, pointer)) {
        throw std::runtime_error("invalid LOG2 character sprite structure");
    }
    Log2CharacterStructure structure;
    structure.sprite_id = sprite_id;
    structure.pointer = pointer;
    structure.animation_count = rom.u8(pointer + 2U);
    structure.sequence_pointer_values.reserve(
        static_cast<std::size_t>(structure.animation_count) * 4U);
    for (std::size_t index = 0;
         index < static_cast<std::size_t>(structure.animation_count) * 4U;
         ++index) {
        structure.sequence_pointer_values.push_back(
            rom.u32(pointer + 0x4CU + index * 4U));
    }
    return structure;
}

ImageFrame decode_log2_frame_record(
    const Rom& rom,
    std::size_t pointer,
    const std::vector<Rgba>& palette) {
    if (!plausible_log2_frame_record(rom, pointer)) return {};
    Log2FrameRecord record;
    record.pointer = pointer;
    record.x = static_cast<std::int8_t>(rom.u8(pointer));
    record.y = static_cast<std::int8_t>(rom.u8(pointer + 1U));
    record.width = rom.u8(pointer + 2U);
    record.height = rom.u8(pointer + 3U);
    const std::uint32_t attributes = rom.u32(pointer + 4U);
    const std::uint16_t attribute1 = attributes >> 16U;
    record.horizontal_flip = (attribute1 & 0x1000U) != 0U;
    record.vertical_flip = (attribute1 & 0x2000U) != 0U;
    record.graphics_container =
        rom.pointer_to_offset(rom.u32(pointer + 8U));

    std::vector<std::uint8_t> graphics;
    try {
        graphics = decompress_container(
            rom,
            record.graphics_container,
            static_cast<std::size_t>(record.width) * record.height).data;
    } catch (const std::exception&) {
        return {};
    }
    if (graphics.size() != static_cast<std::size_t>(record.width) * record.height) {
        return {};
    }

    ImageFrame frame;
    frame.width = record.width;
    frame.height = record.height;
    frame.origin_x = record.x;
    frame.origin_y = record.y;
    frame.pixels.assign(
        static_cast<std::size_t>(frame.width) * frame.height,
        Rgba{0, 0, 0, 0});
    const unsigned tile_columns = frame.width / 8U;
    const unsigned tile_rows = frame.height / 8U;
    for (unsigned tile_y = 0; tile_y < tile_rows; ++tile_y) {
        for (unsigned tile_x = 0; tile_x < tile_columns; ++tile_x) {
            const unsigned source_tile_x = record.horizontal_flip
                ? tile_columns - 1U - tile_x
                : tile_x;
            const unsigned source_tile_y = record.vertical_flip
                ? tile_rows - 1U - tile_y
                : tile_y;
            const std::size_t tile_index =
                static_cast<std::size_t>(source_tile_y) * tile_columns +
                source_tile_x;
            copy_tile_8bpp(
                graphics,
                tile_index,
                record.horizontal_flip,
                record.vertical_flip,
                palette,
                frame.pixels,
                frame.width,
                frame.height,
                tile_x * 8U,
                tile_y * 8U);
        }
    }
    return frame;
}

std::vector<ImageFrame> decode_log2_sequence(
    const Rom& rom,
    std::size_t pointer,
    std::size_t boundary,
    const std::vector<Rgba>& palette) {
    std::vector<ImageFrame> frames;
    if (pointer >= boundary || pointer >= rom.size()) return frames;
    std::size_t cursor = pointer;
    while (frames.size() < 64U && cursor + 12U <= boundary &&
           plausible_log2_frame_record(rom, cursor)) {
        ImageFrame frame = decode_log2_frame_record(rom, cursor, palette);
        if (frame.empty()) break;
        frames.push_back(std::move(frame));
        cursor += 12U;
    }
    return frames;
}

std::vector<Log2Animation> decode_log2_animations(
    const Rom& rom,
    const Log2CharacterStructure& structure,
    const std::vector<Rgba>& palette) {
    std::vector<std::size_t> starts;
    for (const std::uint32_t value : structure.sequence_pointer_values) {
        if (value != 0U && rom.is_rom_pointer(value)) {
            starts.push_back(rom.pointer_to_offset(value));
        }
    }
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());

    std::map<std::size_t, std::vector<ImageFrame>> decoded_sequences;
    for (std::size_t index = 0; index < starts.size(); ++index) {
        const std::size_t boundary = index + 1U < starts.size()
            ? starts[index + 1U]
            : rom.size();
        decoded_sequences.emplace(
            starts[index],
            decode_log2_sequence(rom, starts[index], boundary, palette));
    }

    std::vector<Log2Animation> animations;
    animations.reserve(structure.animation_count);
    for (unsigned animation_index = 0;
         animation_index < structure.animation_count;
         ++animation_index) {
        Log2Animation animation;
        animation.index = animation_index;
        bool has_frames = false;
        for (unsigned direction = 0; direction < 4U; ++direction) {
            const std::uint32_t value = structure.sequence_pointer_values[
                static_cast<std::size_t>(animation_index) * 4U + direction];
            if (value == 0U || !rom.is_rom_pointer(value)) continue;
            const std::size_t pointer = rom.pointer_to_offset(value);
            animation.sequence_pointers[direction] = pointer;
            const auto found = decoded_sequences.find(pointer);
            if (found == decoded_sequences.end() || found->second.empty()) continue;
            animation.directions[direction] = found->second;
            has_frames = true;
        }
        if (has_frames) animations.push_back(std::move(animation));
    }
    return animations;
}

ImageFrame make_log2_animation_sheet(const Log2Animation& animation) {
    ImageFrame sheet;
    unsigned maximum_width = 1U;
    unsigned maximum_height = 1U;
    for (const auto& direction : animation.directions) {
        for (const auto& frame : direction) {
            maximum_width = std::max(maximum_width, frame.width);
            maximum_height = std::max(maximum_height, frame.height);
        }
    }
    constexpr unsigned padding = 8U;
    const unsigned cell_width = maximum_width + padding;
    const unsigned cell_height = maximum_height + padding;
    const unsigned frame_count = std::max(1U, animation.frame_count());
    sheet.width = frame_count * cell_width;
    sheet.height = 4U * cell_height;
    sheet.pixels.assign(
        static_cast<std::size_t>(sheet.width) * sheet.height,
        Rgba{0, 0, 0, 0});
    for (unsigned direction = 0; direction < 4U; ++direction) {
        for (std::size_t frame_index = 0;
             frame_index < animation.directions[direction].size();
             ++frame_index) {
            const ImageFrame& frame = animation.directions[direction][frame_index];
            const int x = static_cast<int>(frame_index * cell_width) +
                static_cast<int>((maximum_width - frame.width) / 2U);
            const int y = static_cast<int>(direction * cell_height) +
                static_cast<int>((maximum_height - frame.height) / 2U);
            copy_frame(sheet.pixels, sheet.width, sheet.height, x, y, frame);
        }
    }
    return sheet;
}

ImageFrame make_log2_character_sheet(
    const std::vector<Log2Animation>& animations) {
    ImageFrame sheet;
    if (animations.empty()) return sheet;
    std::vector<ImageFrame> rows;
    unsigned maximum_width = 1U;
    unsigned total_height = 0U;
    for (const Log2Animation& animation : animations) {
        ImageFrame row = make_log2_animation_sheet(animation);
        maximum_width = std::max(maximum_width, row.width);
        total_height += row.height + 8U;
        rows.push_back(std::move(row));
    }
    sheet.width = maximum_width;
    sheet.height = std::max(1U, total_height - 8U);
    if (static_cast<std::uint64_t>(sheet.width) * sheet.height >
        256U * 1024U * 1024U) {
        return {};
    }
    sheet.pixels.assign(
        static_cast<std::size_t>(sheet.width) * sheet.height,
        Rgba{0, 0, 0, 0});
    unsigned y = 0U;
    for (const ImageFrame& row : rows) {
        copy_frame(
            sheet.pixels,
            sheet.width,
            sheet.height,
            static_cast<int>((sheet.width - row.width) / 2U),
            static_cast<int>(y),
            row);
        y += row.height + 8U;
    }
    return sheet;
}

void write_log2_gallery(
    const std::filesystem::path& output,
    const std::vector<std::tuple<std::size_t, std::size_t, std::size_t, std::string>>& rows,
    std::size_t table_offset) {
    std::ofstream html(output / "character_sprite_gallery.html");
    html << "<!doctype html><html><head><meta charset=\"utf-8\"><title>"
            "DragonByteZ LOG2 character sprites</title><style>"
            "body{margin:0;background:#15191e;color:#edf2f7;font:14px Segoe UI,Arial}"
            "header{position:sticky;top:0;z-index:2;background:#101419;border-bottom:3px solid #f47d1f;padding:16px}"
            "input{width:min(620px,80vw);padding:9px;background:#252b32;color:white;border:1px solid #66717d;border-radius:8px}"
            "main{display:grid;grid-template-columns:repeat(auto-fill,minmax(330px,1fr));gap:14px;padding:16px}"
            "article{background:#262c33;border:1px solid #59636e;border-radius:12px;overflow:hidden}"
            "img{display:block;width:100%;height:300px;object-fit:contain;background:#0c0f12;image-rendering:pixelated}"
            ".meta{padding:10px;line-height:1.5;color:#b9c3cd}b{color:white}</style></head><body><header>"
            "<h1>Legacy of Goku II sprite-ID animation sheets</h1><p>"
            "All 150 authoritative sprite structures, IDs 7 through 156, are read from the game&apos;s "
            "sprite lookup table. Each animation is split into its four direction sequences and every "
            "frame is decompressed from its own Webfoot graphics container. No emulator screenshots "
            "or arbitrary tile-grid candidates are used.</p><p>Sprite table: "
         << hex_value(table_offset, 7) << "</p><input id=\"q\" placeholder=\"Search sprite ID, structure or animation count\"> "
            "<span id=\"count\"></span></header><main>";
    for (const auto& row : rows) {
        const std::size_t sprite_id = std::get<0>(row);
        const std::size_t structure = std::get<1>(row);
        const std::size_t animations = std::get<2>(row);
        const std::string& png = std::get<3>(row);
        std::ostringstream search;
        search << "sprite " << sprite_id << ' ' << hex_value(structure, 7)
               << " animations " << animations;
        html << "<article data-search=\"" << html_escape(search.str())
             << "\"><a href=\"" << html_escape(png) << "\"><img loading=\"lazy\" src=\""
             << html_escape(png) << "\"></a><div class=\"meta\"><b>Sprite ID "
             << sprite_id << "</b><br>Structure " << hex_value(structure, 7)
             << "<br>Decoded animations " << animations
             << "</div></article>";
    }
    html << "</main><script>const q=document.getElementById('q'),cards=[...document.querySelectorAll('article')],c=document.getElementById('count');"
            "function f(){let n=0,s=q.value.toLowerCase();for(const x of cards){const v=!s||x.dataset.search.toLowerCase().includes(s);x.hidden=!v;if(v)n++;}c.textContent=n+' of '+cards.length;}q.oninput=f;f();</script></body></html>";
}

struct BuuPiece {
    int x = 0;
    int y = 0;
    unsigned width = 0;
    unsigned height = 0;
    bool colour_256 = true;
    bool horizontal_flip = false;
    bool vertical_flip = false;
    unsigned palette_bank = 0;
    std::size_t graphics_container = 0;
};

const std::array<std::array<std::array<unsigned, 2>, 4>, 3>& object_dimensions() {
    static const std::array<std::array<std::array<unsigned, 2>, 4>, 3> values = {{
        {{{8, 8}, {16, 16}, {32, 32}, {64, 64}}},
        {{{16, 8}, {32, 8}, {32, 16}, {64, 32}}},
        {{{8, 16}, {8, 32}, {16, 32}, {32, 64}}}
    }};
    return values;
}

bool plausible_buu_frame(const Rom& rom, std::size_t offset) {
    if (offset > rom.size() || rom.size() - offset < 20U) return false;
    const std::uint32_t piece_count = rom.u32(offset);
    if (piece_count == 0U || piece_count > 16U) return false;
    if (piece_count * 16U > rom.size() - (offset + 4U)) return false;
    unsigned valid_pieces = 0;
    for (unsigned piece = 0; piece < piece_count; ++piece) {
        const std::size_t record = offset + 4U + piece * 16U;
        const std::uint32_t attributes = rom.u32(record + 8U);
        const std::uint16_t attribute0 = attributes & 0xFFFFU;
        const std::uint16_t attribute1 = attributes >> 16U;
        const unsigned shape = (attribute0 >> 14U) & 3U;
        const unsigned size = (attribute1 >> 14U) & 3U;
        if (shape >= 3U || size >= 4U) continue;
        const std::uint32_t graphics_pointer = rom.u32(record + 12U);
        if (!rom.is_rom_pointer(graphics_pointer)) continue;
        if (valid_container_header(
                rom, rom.pointer_to_offset(graphics_pointer), 32U, 64U * 1024U)) {
            ++valid_pieces;
        }
    }
    return valid_pieces == piece_count;
}

bool plausible_buu_animation(const Rom& rom, std::size_t offset) {
    if (offset > rom.size() || rom.size() - offset < 20U) return false;
    const std::uint32_t frame_count = rom.u32(offset);
    if (frame_count == 0U || frame_count > 64U) return false;
    const std::size_t pointer_count = static_cast<std::size_t>(frame_count) * 4U;
    if (pointer_count * 4U > rom.size() - (offset + 4U)) return false;
    unsigned valid = 0;
    for (std::size_t index = 0; index < std::min<std::size_t>(pointer_count, 12U);
         ++index) {
        const std::uint32_t frame_pointer = rom.u32(offset + 4U + index * 4U);
        if (frame_pointer == 0U) continue;
        if (rom.is_rom_pointer(frame_pointer) &&
            plausible_buu_frame(rom, rom.pointer_to_offset(frame_pointer))) {
            ++valid;
        }
    }
    return valid != 0U;
}

ImageFrame decode_buu_frame(
    const Rom& rom,
    std::size_t offset,
    const std::vector<Rgba>& palette) {
    if (!plausible_buu_frame(rom, offset)) return {};
    const unsigned piece_count = rom.u32(offset);
    std::vector<BuuPiece> pieces;
    int minimum_x = std::numeric_limits<int>::max();
    int minimum_y = std::numeric_limits<int>::max();
    int maximum_x = std::numeric_limits<int>::min();
    int maximum_y = std::numeric_limits<int>::min();
    for (unsigned piece_index = 0; piece_index < piece_count; ++piece_index) {
        const std::size_t record = offset + 4U + piece_index * 16U;
        BuuPiece piece;
        piece.x = static_cast<std::int8_t>(rom.u8(record));
        piece.y = static_cast<std::int8_t>(rom.u8(record + 1U));
        const std::uint32_t attributes = rom.u32(record + 8U);
        const std::uint16_t attribute0 = attributes & 0xFFFFU;
        const std::uint16_t attribute1 = attributes >> 16U;
        const unsigned shape = (attribute0 >> 14U) & 3U;
        const unsigned size = (attribute1 >> 14U) & 3U;
        if (shape >= 3U) continue;
        piece.width = object_dimensions()[shape][size][0];
        piece.height = object_dimensions()[shape][size][1];
        piece.colour_256 = (attribute0 & 0x2000U) != 0U;
        piece.horizontal_flip = (attribute1 & 0x1000U) != 0U;
        piece.vertical_flip = (attribute1 & 0x2000U) != 0U;
        piece.palette_bank = 0U;
        piece.graphics_container =
            rom.pointer_to_offset(rom.u32(record + 12U));
        minimum_x = std::min(minimum_x, piece.x);
        minimum_y = std::min(minimum_y, piece.y);
        maximum_x = std::max(maximum_x, piece.x + static_cast<int>(piece.width));
        maximum_y = std::max(maximum_y, piece.y + static_cast<int>(piece.height));
        pieces.push_back(piece);
    }
    if (pieces.empty() || maximum_x <= minimum_x || maximum_y <= minimum_y) return {};

    ImageFrame frame;
    frame.width = static_cast<unsigned>(maximum_x - minimum_x);
    frame.height = static_cast<unsigned>(maximum_y - minimum_y);
    frame.origin_x = minimum_x;
    frame.origin_y = minimum_y;
    frame.pixels.assign(
        static_cast<std::size_t>(frame.width) * frame.height,
        Rgba{0, 0, 0, 0});

    for (const BuuPiece& piece : pieces) {
        std::vector<std::uint8_t> graphics;
        try {
            graphics = decompress_container(
                rom, piece.graphics_container, 64U * 1024U).data;
        } catch (const std::exception&) {
            continue;
        }
        const unsigned destination_x = static_cast<unsigned>(piece.x - minimum_x);
        const unsigned destination_y = static_cast<unsigned>(piece.y - minimum_y);
        const unsigned tile_columns = piece.width / 8U;
        const unsigned tile_rows = piece.height / 8U;
        for (unsigned tile_y = 0; tile_y < tile_rows; ++tile_y) {
            for (unsigned tile_x = 0; tile_x < tile_columns; ++tile_x) {
                const unsigned source_tile_x = piece.horizontal_flip
                    ? tile_columns - 1U - tile_x
                    : tile_x;
                const unsigned source_tile_y = piece.vertical_flip
                    ? tile_rows - 1U - tile_y
                    : tile_y;
                const std::size_t tile_index =
                    static_cast<std::size_t>(source_tile_y) * tile_columns +
                    source_tile_x;
                if (piece.colour_256) {
                    copy_tile_8bpp(
                        graphics,
                        tile_index,
                        piece.horizontal_flip,
                        piece.vertical_flip,
                        palette,
                        frame.pixels,
                        frame.width,
                        frame.height,
                        destination_x + tile_x * 8U,
                        destination_y + tile_y * 8U);
                } else {
                    copy_tile_4bpp(
                        graphics,
                        tile_index,
                        piece.horizontal_flip,
                        piece.vertical_flip,
                        piece.palette_bank,
                        palette,
                        frame.pixels,
                        frame.width,
                        frame.height,
                        destination_x + tile_x * 8U,
                        destination_y + tile_y * 8U);
                }
            }
        }
    }
    return frame;
}

struct BuuAnimation {
    std::size_t pointer = 0;
    unsigned frame_count = 0;
    std::array<std::vector<ImageFrame>, 4> directions;
};

BuuAnimation decode_buu_animation(
    const Rom& rom,
    std::size_t pointer,
    const std::vector<Rgba>& palette) {
    BuuAnimation animation;
    animation.pointer = pointer;
    if (!plausible_buu_animation(rom, pointer)) return animation;
    animation.frame_count = rom.u32(pointer);
    for (unsigned frame = 0; frame < animation.frame_count; ++frame) {
        for (unsigned direction = 0; direction < 4U; ++direction) {
            const std::size_t pointer_index =
                static_cast<std::size_t>(frame) * 4U + direction;
            const std::uint32_t frame_pointer =
                rom.u32(pointer + 4U + pointer_index * 4U);
            if (frame_pointer == 0U || !rom.is_rom_pointer(frame_pointer)) {
                animation.directions[direction].push_back({});
                continue;
            }
            animation.directions[direction].push_back(decode_buu_frame(
                rom, rom.pointer_to_offset(frame_pointer), palette));
        }
    }
    return animation;
}

ImageFrame make_buu_animation_sheet(const BuuAnimation& animation) {
    ImageFrame sheet;
    unsigned maximum_width = 1U;
    unsigned maximum_height = 1U;
    for (const auto& direction : animation.directions) {
        for (const auto& frame : direction) {
            maximum_width = std::max(maximum_width, frame.width);
            maximum_height = std::max(maximum_height, frame.height);
        }
    }
    constexpr unsigned padding = 8U;
    const unsigned cell_width = maximum_width + padding;
    const unsigned cell_height = maximum_height + padding;
    sheet.width = std::max(1U, animation.frame_count * cell_width);
    sheet.height = 4U * cell_height;
    sheet.pixels.assign(
        static_cast<std::size_t>(sheet.width) * sheet.height,
        Rgba{0, 0, 0, 0});
    for (unsigned direction = 0; direction < 4U; ++direction) {
        for (unsigned frame_index = 0; frame_index < animation.frame_count;
             ++frame_index) {
            const ImageFrame& frame = animation.directions[direction][frame_index];
            const int x = static_cast<int>(frame_index * cell_width) +
                static_cast<int>((maximum_width - frame.width) / 2U);
            const int y = static_cast<int>(direction * cell_height) +
                static_cast<int>((maximum_height - frame.height) / 2U);
            copy_frame(sheet.pixels, sheet.width, sheet.height, x, y, frame);
        }
    }
    return sheet;
}

ImageFrame make_buu_character_sheet(const std::vector<BuuAnimation>& animations) {
    ImageFrame sheet;
    if (animations.empty()) return sheet;
    std::vector<ImageFrame> rows;
    unsigned maximum_width = 1U;
    unsigned total_height = 0U;
    for (const auto& animation : animations) {
        ImageFrame row = make_buu_animation_sheet(animation);
        maximum_width = std::max(maximum_width, row.width);
        total_height += row.height + 8U;
        rows.push_back(std::move(row));
    }
    sheet.width = maximum_width;
    sheet.height = std::max(1U, total_height - 8U);
    if (static_cast<std::uint64_t>(sheet.width) * sheet.height >
        256U * 1024U * 1024U) {
        return {};
    }
    sheet.pixels.assign(
        static_cast<std::size_t>(sheet.width) * sheet.height,
        Rgba{0, 0, 0, 0});
    unsigned y = 0U;
    for (const auto& row : rows) {
        copy_frame(
            sheet.pixels,
            sheet.width,
            sheet.height,
            static_cast<int>((sheet.width - row.width) / 2U),
            static_cast<int>(y),
            row);
        y += row.height + 8U;
    }
    return sheet;
}

void write_buu_gallery(
    const std::filesystem::path& output,
    const std::vector<std::tuple<std::size_t, std::size_t, std::size_t, std::string>>& rows) {
    std::ofstream html(output / "character_sprite_gallery.html");
    html << "<!doctype html><html><head><meta charset=\"utf-8\"><title>"
            "DragonByteZ Buu&apos;s Fury character sprites</title><style>"
            "body{margin:0;background:#15191e;color:#edf2f7;font:14px Segoe UI,Arial}"
            "header{position:sticky;top:0;z-index:2;background:#101419;border-bottom:3px solid #f47d1f;padding:16px}"
            "input{width:min(620px,80vw);padding:9px;background:#252b32;color:white;border:1px solid #66717d;border-radius:8px}"
            "main{display:grid;grid-template-columns:repeat(auto-fill,minmax(330px,1fr));gap:14px;padding:16px}"
            "article{background:#262c33;border:1px solid #59636e;border-radius:12px;overflow:hidden}"
            "img{display:block;width:100%;height:280px;object-fit:contain;background:#0c0f12;image-rendering:pixelated}"
            ".meta{padding:10px;line-height:1.5;color:#b9c3cd}b{color:white}</style></head><body><header>"
            "<h1>Buu&apos;s Fury decoded character animation sheets</h1><p>"
            "Character structures, animation pointer arrays, four-direction frame tables, "
            "piece bounds, GBA OBJ attributes and compressed graphics are decoded directly "
            "from the ROM. These are not emulator screenshots.</p>"
            "<input id=\"q\" placeholder=\"Search character index, struct or animation count\"> "
            "<span id=\"count\"></span></header><main>";
    for (const auto& row : rows) {
        const std::size_t index = std::get<0>(row);
        const std::size_t structure = std::get<1>(row);
        const std::size_t animations = std::get<2>(row);
        const std::string& png = std::get<3>(row);
        std::ostringstream search;
        search << "character " << index << ' ' << hex_value(structure, 7)
               << " animations " << animations;
        html << "<article data-search=\"" << html_escape(search.str())
             << "\"><a href=\"" << html_escape(png) << "\"><img loading=\"lazy\" src=\""
             << html_escape(png) << "\"></a><div class=\"meta\"><b>Character "
             << index << "</b><br>Structure " << hex_value(structure, 7)
             << "<br>Animations " << animations
             << "</div></article>";
    }
    html << "</main><script>const q=document.getElementById('q'),cards=[...document.querySelectorAll('article')],c=document.getElementById('count');"
            "function f(){let n=0,s=q.value.toLowerCase();for(const x of cards){const v=!s||x.dataset.search.toLowerCase().includes(s);x.hidden=!v;if(v)n++;}c.textContent=n+' of '+cards.length;}q.oninput=f;f();</script></body></html>";
}

} // namespace

SpriteExportSummary export_log2_character_sprites(
    const Rom& rom,
    const std::filesystem::path& output) {
    if (!is_log2_rom(rom)) {
        throw std::runtime_error("LOG2 sprite export requires an ALFP or ALFE ROM");
    }
    const auto table = locate_log2_character_table(rom);
    const auto palette = object_palette(rom, profile_for(rom).default_obj_palette);

    std::filesystem::remove_all(output / "character_sprites");
    std::filesystem::create_directories(output / "character_sprites");
    std::ofstream csv(output / "character_sprites.csv");
    csv << "sprite_id,structure,animation,direction_0,direction_1,direction_2,"
           "direction_3,maximum_frames,png\n";

    SpriteExportSummary summary;
    std::vector<std::tuple<std::size_t, std::size_t, std::size_t, std::string>> gallery_rows;
    for (std::size_t table_index = 0; table_index < table.second; ++table_index) {
        const std::size_t sprite_id = log2_first_sprite_id + table_index;
        const std::uint32_t structure_pointer =
            rom.u32(table.first + table_index * 4U);
        if (!rom.is_rom_pointer(structure_pointer)) continue;
        const std::size_t structure_offset =
            rom.pointer_to_offset(structure_pointer);
        if (!plausible_log2_character_structure(rom, structure_offset)) continue;

        const Log2CharacterStructure structure = read_log2_character_structure(
            rom, sprite_id, structure_offset);
        std::vector<Log2Animation> animations = decode_log2_animations(
            rom, structure, palette);
        if (animations.empty()) continue;

        const std::string sprite_folder_name =
            numbered_name("sprite_", sprite_id, 3, "");
        const std::filesystem::path sprite_folder =
            output / "character_sprites" / sprite_folder_name;
        std::filesystem::create_directories(sprite_folder);

        for (const Log2Animation& animation : animations) {
            ImageFrame animation_sheet = make_log2_animation_sheet(animation);
            if (animation_sheet.empty()) continue;
            const std::string animation_png =
                numbered_name("animation_", animation.index, 3, ".png");
            write_png_rgba(
                sprite_folder / animation_png,
                animation_sheet.width,
                animation_sheet.height,
                animation_sheet.pixels);
            const std::filesystem::path relative =
                std::filesystem::path("character_sprites") /
                sprite_folder_name / animation_png;
            csv << sprite_id << ',' << hex_value(structure_offset, 7) << ','
                << animation.index;
            for (unsigned direction = 0; direction < 4U; ++direction) {
                csv << ',';
                if (animation.sequence_pointers[direction] != 0U) {
                    csv << hex_value(animation.sequence_pointers[direction], 7);
                }
            }
            csv << ',' << animation.frame_count() << ','
                << relative.generic_string() << '\n';
            summary.animation_count += 1U;
            for (const auto& direction : animation.directions) {
                summary.frame_count += direction.size();
            }
        }

        ImageFrame sprite_sheet = make_log2_character_sheet(animations);
        if (sprite_sheet.empty()) {
            std::filesystem::remove_all(sprite_folder);
            continue;
        }
        const std::string sheet_name =
            sprite_folder_name + "_sprite_sheet.png";
        write_png_rgba(
            output / "character_sprites" / sheet_name,
            sprite_sheet.width,
            sprite_sheet.height,
            sprite_sheet.pixels);
        gallery_rows.emplace_back(
            sprite_id,
            structure_offset,
            animations.size(),
            (std::filesystem::path("character_sprites") / sheet_name).generic_string());
        summary.resource_count += 1U;
    }

    write_log2_gallery(output, gallery_rows, table.first);
    std::ofstream note(output / "sprite_extraction_status.txt");
    note << "Legacy of Goku II direct sprite extraction\n"
            "=============================================\n\n"
         << "Authoritative sprite-ID table: " << hex_value(table.first, 7) << "\n"
         << "Sprite ID range: 7 through 156\n"
         << "Decoded sprite structures: " << summary.resource_count << "\n"
         << "Decoded animations: " << summary.animation_count << "\n"
         << "Decoded directional frames: " << summary.frame_count << "\n\n"
            "The old generic compressed-block gallery stopped after an arbitrary "
            "candidate limit and cut graphics into guessed tile grids. It has been "
            "replaced by the game's actual sprite lookup structures, four-direction "
            "animation pointers, 12-byte frame records, OBJ flip attributes and "
            "per-frame Webfoot graphics containers.\n";
    std::ostringstream description;
    description << summary.resource_count << " LOG2 sprite IDs, "
                << summary.animation_count << " animations and "
                << summary.frame_count << " directional frames";
    summary.description = description.str();
    return summary;
}

SpriteExportSummary export_buus_fury_character_sprites(
    const Rom& rom,
    const std::filesystem::path& output) {
    if (!is_buus_fury_rom(rom)) {
        throw std::runtime_error("Buu's Fury sprite export requires the BG3E ROM");
    }
    if (buus_fury_character_table + buus_fury_character_count * 4U > rom.size()) {
        throw std::runtime_error("Buu's Fury character pointer table is outside the ROM");
    }
    const auto palette = object_palette(rom, profile_for(rom).default_obj_palette);
    std::filesystem::remove_all(output / "character_sprites");
    std::filesystem::create_directories(output / "character_sprites");
    std::ofstream csv(output / "character_sprites.csv");
    csv << "character,structure,animation,animation_pointer,frames,directions,png\n";

    std::vector<std::size_t> structures;
    structures.reserve(buus_fury_character_count);
    for (std::size_t index = 0; index < buus_fury_character_count; ++index) {
        const std::uint32_t pointer =
            rom.u32(buus_fury_character_table + index * 4U);
        if (!rom.is_rom_pointer(pointer)) {
            structures.push_back(0U);
        } else {
            structures.push_back(rom.pointer_to_offset(pointer));
        }
    }

    SpriteExportSummary summary;
    std::vector<std::tuple<std::size_t, std::size_t, std::size_t, std::string>> gallery_rows;
    for (std::size_t character = 0; character < structures.size(); ++character) {
        const std::size_t structure = structures[character];
        if (structure == 0U || structure + 0x54U > rom.size()) continue;
        const std::size_t structure_end = character + 1U < structures.size()
            ? structures[character + 1U]
            : buus_fury_character_table;
        if (structure_end <= structure + 0x54U || structure_end > rom.size()) continue;

        std::vector<std::size_t> animation_pointers;
        std::set<std::size_t> seen;
        for (std::size_t cursor = structure + 0x54U;
             cursor + 4U <= structure_end;
             cursor += 4U) {
            const std::uint32_t pointer = rom.u32(cursor);
            if (!rom.is_rom_pointer(pointer)) continue;
            const std::size_t animation = rom.pointer_to_offset(pointer);
            if (!plausible_buu_animation(rom, animation)) continue;
            if (seen.insert(animation).second) {
                animation_pointers.push_back(animation);
            }
        }
        if (animation_pointers.empty()) continue;

        const std::string character_folder_name =
            numbered_name("character_", character, 3, "");
        const std::filesystem::path character_folder =
            output / "character_sprites" / character_folder_name;
        std::filesystem::create_directories(character_folder);
        std::vector<BuuAnimation> animations;
        for (std::size_t animation_index = 0;
             animation_index < animation_pointers.size(); ++animation_index) {
            BuuAnimation animation = decode_buu_animation(
                rom, animation_pointers[animation_index], palette);
            if (animation.frame_count == 0U) continue;
            ImageFrame animation_sheet = make_buu_animation_sheet(animation);
            const std::string animation_png =
                numbered_name("animation_", animation_index, 3, ".png");
            write_png_rgba(
                character_folder / animation_png,
                animation_sheet.width,
                animation_sheet.height,
                animation_sheet.pixels);
            const std::filesystem::path relative =
                std::filesystem::path("character_sprites") /
                character_folder_name / animation_png;
            csv << character << ',' << hex_value(structure, 7) << ','
                << animation_index << ',' << hex_value(animation.pointer, 7)
                << ',' << animation.frame_count << ",4,"
                << relative.generic_string() << '\n';
            summary.animation_count += 1U;
            summary.frame_count += static_cast<std::size_t>(animation.frame_count) * 4U;
            animations.push_back(std::move(animation));
        }
        if (animations.empty()) {
            std::filesystem::remove_all(character_folder);
            continue;
        }
        ImageFrame character_sheet = make_buu_character_sheet(animations);
        if (!character_sheet.empty()) {
            const std::string sheet_name =
                character_folder_name + "_sprite_sheet.png";
            write_png_rgba(
                output / "character_sprites" / sheet_name,
                character_sheet.width,
                character_sheet.height,
                character_sheet.pixels);
            gallery_rows.emplace_back(
                character,
                structure,
                animations.size(),
                (std::filesystem::path("character_sprites") / sheet_name).generic_string());
        }
        summary.resource_count += 1U;
    }
    write_buu_gallery(output, gallery_rows);
    std::ofstream note(output / "sprite_extraction_status.txt");
    note << "Buu's Fury character sprite extraction\n"
            "=========================================\n\n"
         << "Character pointer table: " << hex_value(buus_fury_character_table, 7)
         << "\nDecoded character structures: " << summary.resource_count
         << "\nDecoded animations: " << summary.animation_count
         << "\nDecoded directional frames: " << summary.frame_count << "\n\n"
            "Frames are assembled from character structures, animation pointers, "
            "four-direction frame tables and compressed OBJ pieces.\n";
    std::ostringstream description;
    description << summary.resource_count << " Buu's Fury characters, "
                << summary.animation_count << " animations and "
                << summary.frame_count << " directional frames";
    summary.description = description.str();
    return summary;
}

} // namespace dragonbytez
