#include "dragonbytez/analysis.hpp"
#include "dragonbytez/compression.hpp"
#include "dragonbytez/gsf_player.hpp"
#include "dragonbytez/log1_runtime.hpp"
#include "dragonbytez/png.hpp"
#include "dragonbytez/rom.hpp"
#include "dragonbytez/sprite_analysis.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace dragonbytez {

void analyze_webfoot_graphics(
    const Rom& rom,
    const GameProfile& profile,
    const std::filesystem::path& output);

namespace {

std::string hex(std::uint64_t value, unsigned digits = 8) {
    std::ostringstream text;
    text << "0x" << std::uppercase << std::hex << std::setw(digits)
         << std::setfill('0') << value;
    return text.str();
}

void require_log2_profile(const Rom& rom) {
    if (!is_log2_rom(rom)) {
        throw std::runtime_error(
            "this command requires LOG2 Europe Rev 0 (ALFP) or "
            "USA Rev 0 (ALFE)");
    }
}

void write_wav(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& signed_pcm,
    std::uint32_t sample_rate) {
    std::vector<std::uint8_t> wave_pcm(signed_pcm.size());
    std::transform(
        signed_pcm.begin(), signed_pcm.end(), wave_pcm.begin(),
        [](std::uint8_t value) {
            return static_cast<std::uint8_t>(value ^ 0x80U);
        });

    const auto write_u16 = [](std::ofstream& output, std::uint16_t value) {
        const char bytes[] = {
            static_cast<char>(value),
            static_cast<char>(value >> 8U)};
        output.write(bytes, 2);
    };
    const auto write_u32 = [](std::ofstream& output, std::uint32_t value) {
        const char bytes[] = {
            static_cast<char>(value),
            static_cast<char>(value >> 8U),
            static_cast<char>(value >> 16U),
            static_cast<char>(value >> 24U)};
        output.write(bytes, 4);
    };

    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write("RIFF", 4);
    write_u32(output, static_cast<std::uint32_t>(36 + wave_pcm.size()));
    output.write("WAVEfmt ", 8);
    write_u32(output, 16);
    write_u16(output, 1);
    write_u16(output, 1);
    write_u32(output, sample_rate);
    write_u32(output, sample_rate);
    write_u16(output, 1);
    write_u16(output, 8);
    output.write("data", 4);
    write_u32(output, static_cast<std::uint32_t>(wave_pcm.size()));
    output.write(
        reinterpret_cast<const char*>(wave_pcm.data()),
        static_cast<std::streamsize>(wave_pcm.size()));
    if (!output) {
        throw std::runtime_error("cannot write WAV: " + path.string());
    }
}

void write_character_display_csv(
    const Rom& rom,
    const GameProfile& profile,
    const std::filesystem::path& path) {
    std::ofstream out(path);
    out << "record,file_offset,flags,sprite_index,byte_02,byte_03,byte_04,"
           "byte_05,byte_06,byte_07,word_08,word_0C,word_10\n";
    for (std::size_t index = 0;
         index < profile.character_display_count; ++index) {
        const std::size_t offset =
            profile.character_display_table + index * 0x14;
        out << index << ',' << hex(offset, 7) << ','
            << static_cast<unsigned>(rom.u8(offset)) << ','
            << static_cast<unsigned>(rom.u8(offset + 1));
        for (unsigned byte = 2; byte < 8; ++byte) {
            out << ',' << static_cast<unsigned>(rom.u8(offset + byte));
        }
        out << ',' << hex(rom.u32(offset + 8))
            << ',' << hex(rom.u32(offset + 12))
            << ',' << hex(rom.u32(offset + 16)) << '\n';
    }
}

void write_palette_csv(
    const Rom& rom,
    std::size_t offset,
    const std::filesystem::path& path) {
    std::ofstream out(path);
    out << "index,rgb555,red,green,blue,hex_rgb\n";
    for (std::size_t index = 0; index < 256; ++index) {
        const std::uint16_t value = rom.u16(offset + index * 2);
        const auto expand = [](unsigned part) {
            return static_cast<unsigned>((part << 3) | (part >> 2));
        };
        const unsigned red = expand(value & 31);
        const unsigned green = expand((value >> 5) & 31);
        const unsigned blue = expand((value >> 10) & 31);
        std::ostringstream colour;
        colour << '#' << std::uppercase << std::hex << std::setw(2)
               << std::setfill('0') << red << std::setw(2) << green
               << std::setw(2) << blue;
        out << index << ',' << hex(value, 4) << ',' << std::dec << red << ','
            << green << ',' << blue << ',' << colour.str() << '\n';
    }
}

std::vector<Rgba> make_palette_swatch(const std::vector<Rgba>& palette) {
    constexpr unsigned columns = 16;
    constexpr unsigned swatch = 8;
    std::vector<Rgba> image(columns * swatch * columns * swatch);
    for (unsigned index = 0; index < 256; ++index) {
        const unsigned cell_x = (index % columns) * swatch;
        const unsigned cell_y = (index / columns) * swatch;
        for (unsigned y = 0; y < swatch; ++y) {
            for (unsigned x = 0; x < swatch; ++x) {
                image[static_cast<std::size_t>(cell_y + y) *
                          (columns * swatch) +
                      cell_x + x] = palette[index];
            }
        }
    }
    return image;
}

std::vector<Rgba> make_contact_sheet(
    const std::vector<std::vector<Rgba>>& images,
    unsigned image_width,
    unsigned image_height,
    unsigned columns,
    unsigned padding) {
    if (images.empty() || !columns) return {};
    const unsigned rows =
        static_cast<unsigned>((images.size() + columns - 1) / columns);
    const unsigned cell_width = image_width + padding * 2;
    const unsigned cell_height = image_height + padding * 2;
    std::vector<Rgba> output(
        static_cast<std::size_t>(columns) * cell_width * rows * cell_height,
        {20, 22, 28, 255});
    const unsigned width = columns * cell_width;
    for (std::size_t index = 0; index < images.size(); ++index) {
        const unsigned column = static_cast<unsigned>(index % columns);
        const unsigned row = static_cast<unsigned>(index / columns);
        const auto& image = images[index];
        if (image.size() !=
            static_cast<std::size_t>(image_width) * image_height) {
            throw std::runtime_error("contact-sheet image size mismatch");
        }
        for (unsigned y = 0; y < image_height; ++y) {
            for (unsigned x = 0; x < image_width; ++x) {
                const unsigned dx = column * cell_width + padding + x;
                const unsigned dy = row * cell_height + padding + y;
                output[static_cast<std::size_t>(dy) * width + dx] =
                    image[static_cast<std::size_t>(y) * image_width + x];
            }
        }
    }
    return output;
}

std::vector<Rgba> scale_nearest(
    const std::vector<Rgba>& source,
    unsigned source_width,
    unsigned source_height,
    unsigned output_width,
    unsigned output_height) {
    if (source.size() !=
            static_cast<std::size_t>(source_width) * source_height ||
        !source_width || !source_height || !output_width || !output_height) {
        throw std::invalid_argument("invalid nearest-neighbour scale request");
    }
    std::vector<Rgba> output(
        static_cast<std::size_t>(output_width) * output_height);
    for (unsigned y = 0; y < output_height; ++y) {
        const unsigned source_y =
            static_cast<unsigned>(
                static_cast<std::uint64_t>(y) * source_height / output_height);
        for (unsigned x = 0; x < output_width; ++x) {
            const unsigned source_x =
                static_cast<unsigned>(
                    static_cast<std::uint64_t>(x) * source_width / output_width);
            output[static_cast<std::size_t>(y) * output_width + x] =
                source[static_cast<std::size_t>(source_y) * source_width +
                       source_x];
        }
    }
    return output;
}

std::string numbered_name(
    const std::string& prefix,
    std::size_t index,
    const std::string& suffix) {
    std::ostringstream name;
    name << prefix << std::setw(2) << std::setfill('0') << index << suffix;
    return name.str();
}

void append_le32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value >> 16U));
    output.push_back(static_cast<std::uint8_t>(value >> 24U));
}

std::uint32_t adler32(const std::vector<std::uint8_t>& data) {
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const std::uint8_t byte : data) {
        a = (a + byte) % 65521U;
        b = (b + a) % 65521U;
    }
    return (b << 16U) | a;
}

std::uint32_t crc32(const std::vector<std::uint8_t>& data) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const std::uint8_t byte : data) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1U) ^
                (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

std::vector<std::uint8_t> zlib_store(
    const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> output;
    output.reserve(data.size() + data.size() / 65535U * 5U + 16U);
    output.push_back(0x78);
    output.push_back(0x01);
    std::size_t position = 0;
    do {
        const std::size_t remaining = data.size() - position;
        const std::uint16_t block_size = static_cast<std::uint16_t>(
            std::min<std::size_t>(remaining, 65535U));
        const bool final = position + block_size == data.size();
        output.push_back(final ? 1 : 0);
        output.push_back(static_cast<std::uint8_t>(block_size));
        output.push_back(static_cast<std::uint8_t>(block_size >> 8U));
        const std::uint16_t inverse =
            static_cast<std::uint16_t>(~block_size);
        output.push_back(static_cast<std::uint8_t>(inverse));
        output.push_back(static_cast<std::uint8_t>(inverse >> 8U));
        output.insert(
            output.end(),
            data.begin() + static_cast<std::ptrdiff_t>(position),
            data.begin() + static_cast<std::ptrdiff_t>(
                position + block_size));
        position += block_size;
    } while (position < data.size());
    const std::uint32_t checksum = adler32(data);
    output.push_back(static_cast<std::uint8_t>(checksum >> 24U));
    output.push_back(static_cast<std::uint8_t>(checksum >> 16U));
    output.push_back(static_cast<std::uint8_t>(checksum >> 8U));
    output.push_back(static_cast<std::uint8_t>(checksum));
    return output;
}

[[maybe_unused]] void write_psf22(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& program,
    const std::string& tags = {}) {
    const auto compressed = zlib_store(program);
    std::vector<std::uint8_t> file = {'P', 'S', 'F', 0x22};
    append_le32(file, 0);
    append_le32(
        file, static_cast<std::uint32_t>(compressed.size()));
    append_le32(file, crc32(compressed));
    file.insert(file.end(), compressed.begin(), compressed.end());
    file.insert(file.end(), tags.begin(), tags.end());
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(
        reinterpret_cast<const char*>(file.data()),
        static_cast<std::streamsize>(file.size()));
    if (!output) {
        throw std::runtime_error("cannot write GSF: " + path.string());
    }
}

void write_binary(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& data) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(
        reinterpret_cast<const char*>(data.data()),
        static_cast<std::streamsize>(data.size()));
    if (!output) {
        throw std::runtime_error("cannot write binary file: " + path.string());
    }
}

void write_level_music(
    const Rom& rom,
    const GameProfile& profile,
    const std::filesystem::path& output) {
    constexpr unsigned track_seconds = 12;
    constexpr const char* cache_version = "DragonByteZ-0.6.18-12-second-WAV-preview";

    std::filesystem::create_directories(output);
    const std::filesystem::path marker = output / ".music_cache_version";
    bool cache_matches = false;
    {
        std::ifstream input(marker);
        std::string value;
        if (input && std::getline(input, value)) {
            cache_matches = value == cache_version;
        }
    }
    if (!cache_matches) {
        std::filesystem::remove_all(output / "tracks");
    }
    std::filesystem::create_directories(output / "tracks");
    {
        std::ofstream marker_output(marker);
        marker_output << cache_version << '\n';
    }

    std::ofstream csv(output / "level_music.csv");
    csv << "track,record_offset,duration_seconds,wav,status\n";
    std::ofstream playlist(output / "DragonByteZ_tracks.m3u");
    for (std::size_t index = 0; index < profile.bgm_count; ++index) {
        const std::size_t record = profile.bgm_table + index * 20U;
        const std::string wav_name = numbered_name("track_", index, ".wav");
        const std::filesystem::path wav_path = output / "tracks" / wav_name;
        bool reused = false;
        std::error_code error;
        if (std::filesystem::is_regular_file(wav_path, error) &&
            !error && std::filesystem::file_size(wav_path, error) > 44U && !error) {
            reused = true;
        } else {
            render_bgm_preview_wav(rom, index, wav_path, track_seconds);
        }
        csv << index << ',' << hex(record, 7) << ',' << track_seconds << ','
            << wav_name << ',' << (reused ? "cached" : "rendered") << '\n';
        playlist << "tracks/" << wav_name << '\n';
    }

    std::ofstream note(output / "README.txt");
    note << "DragonByteZ Webfoot music export\n"
         << "================================\n\n"
         << "Rendered " << profile.bgm_count
         << " distinct original-engine selections as immediately playable "
            "44.1 kHz stereo WAV previews. Batch analysis uses twelve seconds "
            "per track so analyzing a ROM does not spend many minutes emulating "
            "hours of audio. Existing valid WAV previews are reused on later "
            "runs. No BIN, GSF, miniGSF or sequence files are written.\n";
}

struct MapLayer {
    std::vector<Rgba> pixels;
    unsigned chunks = 0;
    unsigned highest_tile = 0;
    unsigned priority = 0;
};

struct MapEntry {
    std::size_t index = 0;
    std::size_t offset = 0;
    std::uint8_t output = 0;
    std::uint8_t zone = 0;
    std::uint8_t area = 0;
    std::uint8_t variation = 0;
    std::uint8_t trigger_count = 0;
    std::uint8_t script_count = 0;
    std::uint8_t item_count = 0;
    std::uint8_t object_count = 0;
    std::uint8_t npc_count = 0;
    std::uint8_t flags = 0;
    std::uint8_t unknown_09 = 0;
    std::uint8_t unknown_0A = 0;
    std::uint32_t unknown_0B = 0;
    std::uint32_t triggers_pointer = 0;
    std::uint32_t scripts_pointer = 0;
    std::uint32_t items_pointer = 0;
    std::uint32_t objects_pointer = 0;
    std::uint32_t npcs_pointer = 0;
    std::uint32_t music_id = 0;
    std::uint32_t variation_script_pointer = 0;
    std::uint32_t variation_array_pointer = 0;
    std::uint32_t entry_script_pointer = 0;
    std::uint32_t exit_script_pointer = 0;
    std::uint32_t graphics_pointer = 0;
    std::size_t level_record_index =
        std::numeric_limits<std::size_t>::max();
};

struct LevelRecord {
    std::size_t offset = 0;
    std::uint32_t pointer = 0;
    std::array<std::uint32_t, 4> descriptors{};
    std::uint32_t bg_controls = 0;
    std::uint32_t flags = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t animated_sequence_count = 0;
    std::uint32_t animated_sequence_table = 0;
    std::uint32_t static_tileset = 0;
    std::uint32_t tile_atlas_table = 0;
    std::vector<std::size_t> map_entries;
    std::vector<std::string> aliases;
};

struct LevelIndex {
    std::size_t map_entry_table = 0;
    std::vector<MapEntry> entries;
    std::vector<LevelRecord> records;
};

std::string map_label(const MapEntry& entry) {
    return "Z" + std::to_string(entry.output) + "A" +
           std::to_string(entry.zone);
}

std::string map_file_label(const MapEntry& entry) {
    std::ostringstream label;
    label << 'Z' << std::setw(2) << std::setfill('0')
          << static_cast<unsigned>(entry.output)
          << 'A' << std::setw(3) << std::setfill('0')
          << static_cast<unsigned>(entry.zone);
    return label.str();
}

std::string join_aliases(const std::vector<std::string>& aliases) {
    std::ostringstream joined;
    for (std::size_t index = 0; index < aliases.size(); ++index) {
        if (index != 0) joined << " | ";
        joined << aliases[index];
    }
    return joined.str();
}

bool matches_map_entry_signature(const Rom& rom, std::size_t offset) {
    static constexpr std::array<std::uint8_t, 12> first = {
        1, 1, 1, 1, 1, 3, 0, 0, 1, 0, 0, 0};
    static constexpr std::array<std::uint8_t, 12> second = {
        1, 2, 1, 1, 4, 0, 0, 0, 1, 0, 0, 0};
    static constexpr std::array<std::uint8_t, 12> third = {
        1, 3, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    constexpr std::size_t stride = 0x38;
    if (offset + stride * 2 + 0x10 > rom.size()) return false;
    for (std::size_t index = 0; index < first.size(); ++index) {
        if (rom.u8(offset + index) != first[index] ||
            rom.u8(offset + stride + index) != second[index] ||
            rom.u8(offset + stride * 2 + index) != third[index]) {
            return false;
        }
    }
    return rom.u32(offset + 0x0C) == 0xE3U &&
           rom.u32(offset + stride + 0x0C) == 0xE5U &&
           rom.u32(offset + stride * 2 + 0x0C) == 0xDDU;
}

std::size_t find_map_entry_table(
    const Rom& rom,
    const GameProfile& profile) {
    constexpr std::size_t stride = 0x38;
    if (profile.map_entry_table != 0) {
        const std::size_t required = stride * profile.map_entry_count;
        if (profile.map_entry_table > rom.size() ||
            required > rom.size() - profile.map_entry_table) {
            throw std::runtime_error("configured map-entry table is outside the ROM");
        }
        return profile.map_entry_table;
    }
    const std::size_t required = stride * profile.map_entry_count;
    for (std::size_t offset = 0; offset + required <= rom.size(); offset += 4) {
        if (matches_map_entry_signature(rom, offset)) return offset;
    }
    throw std::runtime_error("could not locate the configured map-entry table");
}

std::vector<MapEntry> read_map_entries(
    const Rom& rom,
    std::size_t table,
    std::size_t count) {
    constexpr std::size_t stride = 0x38;
    std::vector<MapEntry> entries;
    entries.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t offset = table + index * stride;
        MapEntry entry;
        entry.index = index;
        entry.offset = offset;
        entry.output = rom.u8(offset + 0x00);
        entry.zone = rom.u8(offset + 0x01);
        entry.area = rom.u8(offset + 0x02);
        entry.variation = rom.u8(offset + 0x03);
        entry.trigger_count = rom.u8(offset + 0x04);
        entry.script_count = rom.u8(offset + 0x05);
        entry.item_count = rom.u8(offset + 0x06);
        entry.object_count = rom.u8(offset + 0x07);
        entry.npc_count = rom.u8(offset + 0x08);
        entry.flags = rom.u8(offset + 0x09);
        entry.unknown_09 = rom.u8(offset + 0x0A);
        entry.unknown_0A = rom.u8(offset + 0x0B);
        entry.unknown_0B = rom.u32(offset + 0x0C);
        entry.triggers_pointer = rom.u32(offset + 0x10);
        entry.scripts_pointer = rom.u32(offset + 0x14);
        entry.items_pointer = rom.u32(offset + 0x18);
        entry.objects_pointer = rom.u32(offset + 0x1C);
        entry.npcs_pointer = rom.u32(offset + 0x20);
        entry.music_id = rom.u32(offset + 0x24);
        entry.variation_script_pointer = rom.u32(offset + 0x28);
        entry.variation_array_pointer = rom.u32(offset + 0x2C);
        entry.entry_script_pointer = rom.u32(offset + 0x30);
        entry.exit_script_pointer = rom.u32(offset + 0x34);
        if (rom.is_rom_pointer(entry.variation_array_pointer)) {
            const std::size_t variation_array =
                rom.pointer_to_offset(entry.variation_array_pointer);
            if (variation_array + 4 <= rom.size()) {
                entry.graphics_pointer = rom.u32(variation_array);
            }
        }
        entries.push_back(entry);
    }
    return entries;
}

bool valid_level_descriptor(const Rom& rom, std::uint32_t pointer) {
    if (!rom.is_rom_pointer(pointer)) return false;
    const std::size_t offset = rom.pointer_to_offset(pointer);
    if (offset + 0x18 > rom.size()) return false;
    const unsigned columns = rom.u8(offset + 0x14);
    const unsigned rows = rom.u8(offset + 0x15);
    const std::size_t pointer_count =
        static_cast<std::size_t>(columns) * rows;
    return pointer_count <= 256 &&
           offset + 0x18 + pointer_count * 4 <= rom.size();
}

bool parse_level_record(
    const Rom& rom,
    const GameProfile& profile,
    std::uint32_t pointer,
    LevelRecord& record) {
    if (!rom.is_rom_pointer(pointer)) return false;
    const std::size_t offset = rom.pointer_to_offset(pointer);
    if (offset + 0x5C > rom.size()) return false;

    record = {};
    record.offset = offset;
    record.pointer = pointer;
    record.bg_controls = rom.u32(offset + 0x00);
    record.flags = rom.u32(offset + 0x04);
    record.width = rom.u16(offset + 0x08) + 240U;
    record.height = rom.u16(offset + 0x0A) + 160U;
    record.animated_sequence_count = rom.u32(offset + 0x0C);
    record.animated_sequence_table = rom.u32(offset + 0x10);
    record.static_tileset = rom.u32(offset + 0x48);
    record.tile_atlas_table = rom.u32(offset + 0x4C);

    if (record.width < 240 || record.height < 160 ||
        record.width > 8192 || record.height > 8192 ||
        record.animated_sequence_count > 128 ||
        !rom.is_rom_pointer(record.static_tileset) ||
        !rom.is_rom_pointer(record.tile_atlas_table)) {
        return false;
    }
    const std::uint32_t expected_atlas_table =
        0x08000000U +
        static_cast<std::uint32_t>(profile.level_tileset_table);
    if (record.tile_atlas_table != expected_atlas_table) return false;
    if (record.animated_sequence_count != 0 &&
        !rom.is_rom_pointer(record.animated_sequence_table)) {
        return false;
    }

    bool has_descriptor = false;
    for (std::size_t layer = 0; layer < record.descriptors.size(); ++layer) {
        const std::uint32_t descriptor = rom.u32(offset + 0x14 + layer * 4);
        if (descriptor == 0 || !valid_level_descriptor(rom, descriptor)) {
            record.descriptors[layer] = 0;
            continue;
        }
        record.descriptors[layer] = descriptor;
        has_descriptor = true;
    }
    return has_descriptor;
}

LevelIndex build_level_index(const Rom& rom, const GameProfile& profile) {
    LevelIndex result;
    result.map_entry_table = find_map_entry_table(rom, profile);
    result.entries = read_map_entries(
        rom, result.map_entry_table, profile.map_entry_count);
    std::map<std::uint32_t, std::size_t> record_by_pointer;

    for (auto& entry : result.entries) {
        LevelRecord parsed;
        if (!parse_level_record(
                rom, profile, entry.graphics_pointer, parsed)) {
            continue;
        }
        auto existing = record_by_pointer.find(entry.graphics_pointer);
        if (existing == record_by_pointer.end()) {
            entry.level_record_index = result.records.size();
            parsed.map_entries.push_back(entry.index);
            parsed.aliases.push_back(map_label(entry));
            record_by_pointer.emplace(
                entry.graphics_pointer, entry.level_record_index);
            result.records.push_back(std::move(parsed));
        } else {
            entry.level_record_index = existing->second;
            auto& record = result.records[entry.level_record_index];
            record.map_entries.push_back(entry.index);
            const std::string alias = map_label(entry);
            if (std::find(record.aliases.begin(), record.aliases.end(), alias) ==
                record.aliases.end()) {
                record.aliases.push_back(alias);
            }
        }
    }
    return result;
}

std::vector<LevelRecord> find_level_records(
    const Rom& rom,
    const GameProfile& profile) {
    return build_level_index(rom, profile).records;
}

using TileIndices = std::array<std::uint8_t, 64>;

struct MapTileset {
    std::vector<TileIndices> tiles;
    std::vector<bool> present;
    std::size_t static_tiles = 0;
    std::size_t animated_tiles = 0;
    std::size_t invalid_global_tiles = 0;
};

TileIndices global_tile(
    const std::vector<std::vector<std::uint8_t>>& atlases,
    std::size_t tile_id) {
    constexpr std::size_t tiles_per_atlas = 256;
    constexpr std::size_t bytes_per_tile = 64;
    const std::size_t atlas_index = tile_id / tiles_per_atlas;
    const std::size_t local_tile = tile_id % tiles_per_atlas;
    if (atlas_index >= atlases.size() ||
        atlases[atlas_index].size() <
            (local_tile + 1) * bytes_per_tile) {
        throw std::runtime_error("map references a tile outside the atlas");
    }
    TileIndices result{};
    std::copy_n(
        atlases[atlas_index].begin() +
            static_cast<std::ptrdiff_t>(local_tile * bytes_per_tile),
        bytes_per_tile, result.begin());
    return result;
}

void set_map_tile(
    MapTileset& tileset,
    std::size_t tile_id,
    const TileIndices& tile) {
    if (tile_id >= 1024) return;
    if (tileset.tiles.size() <= tile_id) {
        tileset.tiles.resize(tile_id + 1);
        tileset.present.resize(tile_id + 1, false);
    }
    tileset.tiles[tile_id] = tile;
    tileset.present[tile_id] = true;
}

MapTileset build_map_tileset(
    const Rom& rom,
    const LevelRecord& record,
    const std::vector<std::vector<std::uint8_t>>& atlases) {
    MapTileset result;
    const auto static_data = decompress_container(
        rom, rom.pointer_to_offset(record.static_tileset)).data;
    if (static_data.empty() || (static_data.size() & 1U) != 0) {
        throw std::runtime_error(
            "map static-tileset delta stream has an invalid size");
    }

    std::size_t global_tile_id = 0;
    for (std::size_t position = 0;
         position < static_data.size(); position += 2) {
        const std::uint16_t delta =
            static_data[position] |
            (static_cast<std::uint16_t>(static_data[position + 1]) << 8U);
        global_tile_id += delta;
        const std::size_t available_global_tiles = atlases.size() * 256U;
        if (global_tile_id < available_global_tiles) {
            set_map_tile(
                result, result.static_tiles,
                global_tile(atlases, global_tile_id));
        } else {
            ++result.invalid_global_tiles;
        }
        ++result.static_tiles;
        ++global_tile_id;
    }

    if (record.animated_sequence_count == 0) return result;
    const std::size_t sequence_table =
        rom.pointer_to_offset(record.animated_sequence_table);
    for (std::size_t sequence = 0;
         sequence < record.animated_sequence_count; ++sequence) {
        const std::uint32_t sequence_pointer =
            rom.u32(sequence_table + sequence * 4);
        if (!rom.is_rom_pointer(sequence_pointer)) continue;
        const std::size_t sequence_offset =
            rom.pointer_to_offset(sequence_pointer);
        const unsigned animation_frame_count = rom.u8(sequence_offset);
        const unsigned tile_count = rom.u8(sequence_offset + 1);
        const unsigned vram_offset = rom.u16(sequence_offset + 2);
        if (animation_frame_count == 0 || tile_count == 0 ||
            animation_frame_count > 32 || tile_count > 128) {
            continue;
        }
        const std::uint32_t first_frame_pointer = rom.u32(sequence_offset + 4);
        if (!rom.is_rom_pointer(first_frame_pointer)) continue;
        const auto first_frame = decompress_unknown_header(
            rom, rom.pointer_to_offset(first_frame_pointer),
            static_cast<std::size_t>(tile_count) * 64 + 4096).data;
        if (first_frame.size() < static_cast<std::size_t>(tile_count) * 64) {
            continue;
        }
        for (unsigned tile = 0; tile < tile_count; ++tile) {
            TileIndices tile_pixels{};
            std::copy_n(
                first_frame.begin() + static_cast<std::ptrdiff_t>(tile * 64),
                64, tile_pixels.begin());
            set_map_tile(result, vram_offset + tile, tile_pixels);
            ++result.animated_tiles;
        }
    }
    return result;
}

std::string read_latin1_string(
    const Rom& rom, std::uint32_t pointer, std::size_t maximum = 256) {
    if (!rom.is_rom_pointer(pointer)) return {};
    const std::size_t offset = rom.pointer_to_offset(pointer);
    std::string result;
    result.reserve(32);
    for (std::size_t index = 0;
         index < maximum && offset + index < rom.size(); ++index) {
        const std::uint8_t value = rom.u8(offset + index);
        if (value == 0) return result;
        if (value < 0x80) {
            result.push_back(static_cast<char>(value));
        } else {
            result.push_back(static_cast<char>(0xC0U | (value >> 6U)));
            result.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
        }
    }
    return result;
}

std::string csv_text(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size() + 2);
    escaped.push_back('"');
    for (const char character : text) {
        if (character == '"') escaped.push_back('"');
        escaped.push_back(character);
    }
    escaped.push_back('"');
    return escaped;
}

void write_area_names(
    const Rom& rom,
    const GameProfile& profile,
    const std::filesystem::path& path) {
    static constexpr std::array<const char*, 5> languages = {
        "english", "french", "german", "italian", "spanish"};
    std::ofstream output(path);
    output << "area_id";
    for (std::size_t language = 0;
         language < profile.area_name_languages; ++language) {
        const char* name = languages[language];
        output << ',' << name << "_pointer," << name;
    }
    output << '\n';
    for (std::size_t area = 0;
         area < profile.area_name_count; ++area) {
        output << area;
        for (std::size_t language = 0;
             language < profile.area_name_languages; ++language) {
            const std::uint32_t pointer = rom.u32(
                profile.area_name_table +
                (area * profile.area_name_languages + language) * 4);
            output << ',' << hex(pointer) << ','
                   << csv_text(read_latin1_string(rom, pointer));
        }
        output << '\n';
    }
}

void write_map_entries(
    const Rom& rom,
    const LevelIndex& level_index,
    const std::filesystem::path& path) {
    std::ofstream output(path);
    output << "map_entry,label,map_entry_offset,output,zone,area,variation,"
              "trigger_count,script_count,item_count,object_count,npc_count,"
              "flags,unknown_09,unknown_0A,unknown_0B,triggers_pointer,"
              "scripts_pointer,items_pointer,objects_pointer,npcs_pointer,"
              "music_id,variation_script_pointer,variation_array_pointer,"
              "entry_script_pointer,exit_script_pointer,graphics_pointer,"
              "graphics_file_offset,level_record,status\n";
    for (const auto& entry : level_index.entries) {
        output << entry.index << ',' << csv_text(map_label(entry)) << ','
               << hex(entry.offset, 7) << ','
               << static_cast<unsigned>(entry.output) << ','
               << static_cast<unsigned>(entry.zone) << ','
               << static_cast<unsigned>(entry.area) << ','
               << static_cast<unsigned>(entry.variation) << ','
               << static_cast<unsigned>(entry.trigger_count) << ','
               << static_cast<unsigned>(entry.script_count) << ','
               << static_cast<unsigned>(entry.item_count) << ','
               << static_cast<unsigned>(entry.object_count) << ','
               << static_cast<unsigned>(entry.npc_count) << ','
               << static_cast<unsigned>(entry.flags) << ','
               << static_cast<unsigned>(entry.unknown_09) << ','
               << static_cast<unsigned>(entry.unknown_0A) << ','
               << hex(entry.unknown_0B) << ','
               << (entry.triggers_pointer ? hex(entry.triggers_pointer) : "")
               << ','
               << (entry.scripts_pointer ? hex(entry.scripts_pointer) : "")
               << ','
               << (entry.items_pointer ? hex(entry.items_pointer) : "")
               << ','
               << (entry.objects_pointer ? hex(entry.objects_pointer) : "")
               << ','
               << (entry.npcs_pointer ? hex(entry.npcs_pointer) : "") << ','
               << hex(entry.music_id) << ','
               << (entry.variation_script_pointer
                       ? hex(entry.variation_script_pointer)
                       : "")
               << ','
               << (entry.variation_array_pointer
                       ? hex(entry.variation_array_pointer)
                       : "")
               << ','
               << (entry.entry_script_pointer
                       ? hex(entry.entry_script_pointer)
                       : "")
               << ','
               << (entry.exit_script_pointer
                       ? hex(entry.exit_script_pointer)
                       : "")
               << ','
               << (entry.graphics_pointer ? hex(entry.graphics_pointer) : "")
               << ',';
        if (rom.is_rom_pointer(entry.graphics_pointer)) {
            output << hex(rom.pointer_to_offset(entry.graphics_pointer), 7);
        }
        output << ',';
        if (entry.level_record_index !=
            std::numeric_limits<std::size_t>::max()) {
            output << entry.level_record_index
                   << ",linked through variation_array[0]\n";
        } else {
            output << ",special or unresolved graphics record\n";
        }
    }
}

void write_level_records(
    const std::vector<LevelRecord>& records,
    const std::filesystem::path& path) {
    std::ofstream output(path);
    output << "record,labels,graphics_pointer,record_offset,width,height,"
              "active_layers,bg_controls,flags,descriptor_0,descriptor_1,"
              "descriptor_2,descriptor_3,animated_sequence_count,"
              "animated_sequence_table,static_tileset_pointer,"
              "tile_atlas_table,map_entry_count\n";
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto& record = records[index];
        unsigned active_layers = 0;
        for (const std::uint32_t pointer : record.descriptors) {
            if (pointer != 0) ++active_layers;
        }
        output << index << ',' << csv_text(join_aliases(record.aliases)) << ','
               << hex(record.pointer) << ',' << hex(record.offset, 7) << ','
               << record.width << ',' << record.height << ','
               << active_layers << ',' << hex(record.bg_controls) << ','
               << hex(record.flags);
        for (const std::uint32_t pointer : record.descriptors) {
            output << ',' << (pointer ? hex(pointer) : "");
        }
        output << ',' << record.animated_sequence_count << ','
               << (record.animated_sequence_table
                       ? hex(record.animated_sequence_table)
                       : "")
               << ',' << hex(record.static_tileset) << ','
               << hex(record.tile_atlas_table) << ','
               << record.map_entries.size() << '\n';
    }
}

MapLayer render_map_layer(
    const Rom& rom,
    std::size_t descriptor,
    unsigned map_width,
    unsigned map_height,
    const MapTileset& tileset,
    const std::vector<Rgba>& source_palette) {
    constexpr unsigned chunk_tiles = 32;
    constexpr unsigned tile_size = 8;
    constexpr unsigned chunk_pixels = chunk_tiles * tile_size;
    if (descriptor + 0x18 > rom.size()) {
        throw std::runtime_error("map layer descriptor is truncated");
    }

    MapLayer result;
    result.pixels.assign(
        static_cast<std::size_t>(map_width) * map_height, {0, 0, 0, 0});
    auto palette = source_palette;
    palette[0].a = 0;

    const unsigned offset_x_raw = rom.u16(descriptor + 0x0D);
    const unsigned offset_y_raw = rom.u16(descriptor + 0x11);
    int offset_x = static_cast<int>(offset_x_raw / 4U);
    int offset_y = static_cast<int>((offset_y_raw & 0xFFU) / 4U);
    if (offset_x >= 0x3F00) offset_x = -(offset_x - 0x3F00);
    if (offset_y >= 0x3F00) offset_y = -(offset_y - 0x3F00);
    result.priority = (offset_y_raw >> 8U) & 0xFFU;

    const unsigned columns = rom.u8(descriptor + 0x14);
    const unsigned rows = rom.u8(descriptor + 0x15);
    const std::size_t chunk_count =
        static_cast<std::size_t>(columns) * rows;
    if (chunk_count > 256 ||
        descriptor + 0x18 + chunk_count * 4 > rom.size()) {
        throw std::runtime_error("map layer chunk table is invalid");
    }

    for (unsigned row = 0; row < rows; ++row) {
        for (unsigned column = 0; column < columns; ++column) {
            const unsigned chunk_index = row * columns + column;
            const std::uint32_t pointer =
                rom.u32(descriptor + 0x18 + chunk_index * 4);
            if (!rom.is_rom_pointer(pointer)) continue;
            const auto decoded = decompress_container(
                rom, rom.pointer_to_offset(pointer)).data;
            if (decoded.size() != 32U * 32U * 2U) continue;
            ++result.chunks;

            const int block_x =
                static_cast<int>(column * chunk_pixels) - offset_x;
            const int block_y =
                static_cast<int>(row * chunk_pixels) - offset_y;
            for (unsigned entry_index = 0; entry_index < 32U * 32U;
                 ++entry_index) {
                const std::uint16_t entry =
                    decoded[entry_index * 2] |
                    (static_cast<std::uint16_t>(
                         decoded[entry_index * 2 + 1])
                     << 8U);
                const unsigned tile = entry & 0x03FFU;
                result.highest_tile = std::max(result.highest_tile, tile);
                const bool horizontal_flip = (entry & 0x0400U) != 0;
                const bool vertical_flip = (entry & 0x0800U) != 0;
                if (tile == 0 || tile >= tileset.tiles.size() ||
                    !tileset.present[tile]) {
                    continue;
                }

                const int tile_x =
                    block_x + static_cast<int>((entry_index % 32) * 8);
                const int tile_y =
                    block_y + static_cast<int>((entry_index / 32) * 8);
                for (unsigned y = 0; y < 8; ++y) {
                    for (unsigned x = 0; x < 8; ++x) {
                        const unsigned source_x = horizontal_flip ? 7 - x : x;
                        const unsigned source_y = vertical_flip ? 7 - y : y;
                        const std::uint8_t palette_index =
                            tileset.tiles[tile][source_y * 8 + source_x];
                        const int destination_x =
                            tile_x + static_cast<int>(x);
                        const int destination_y =
                            tile_y + static_cast<int>(y);
                        if (destination_x < 0 || destination_y < 0 ||
                            destination_x >= static_cast<int>(map_width) ||
                            destination_y >= static_cast<int>(map_height)) {
                            continue;
                        }
                        result.pixels[
                            static_cast<std::size_t>(destination_y) * map_width +
                            static_cast<unsigned>(destination_x)] =
                            palette[palette_index];
                    }
                }
            }
        }
    }
    return result;
}

std::pair<unsigned, unsigned> fit_dimensions(
    unsigned width,
    unsigned height,
    unsigned maximum_dimension) {
    if (!width || !height || !maximum_dimension) return {1, 1};
    if (width <= maximum_dimension && height <= maximum_dimension) {
        return {width, height};
    }
    if (width >= height) {
        const unsigned scaled_height = std::max(
            1U,
            static_cast<unsigned>(
                static_cast<std::uint64_t>(height) * maximum_dimension /
                width));
        return {maximum_dimension, scaled_height};
    }
    const unsigned scaled_width = std::max(
        1U,
        static_cast<unsigned>(
            static_cast<std::uint64_t>(width) * maximum_dimension / height));
    return {scaled_width, maximum_dimension};
}

std::vector<Rgba> make_local_tileset_image(
    const MapTileset& tileset,
    const std::vector<Rgba>& palette,
    unsigned& width,
    unsigned& height) {
    constexpr unsigned columns = 32;
    constexpr unsigned tile_size = 8;
    const unsigned rows = std::max(
        1U,
        static_cast<unsigned>(
            (tileset.tiles.size() + columns - 1) / columns));
    width = columns * tile_size;
    height = rows * tile_size;
    std::vector<Rgba> pixels(
        static_cast<std::size_t>(width) * height, {0, 0, 0, 0});
    for (std::size_t tile = 0; tile < tileset.tiles.size(); ++tile) {
        if (!tileset.present[tile]) continue;
        const unsigned origin_x =
            static_cast<unsigned>(tile % columns) * tile_size;
        const unsigned origin_y =
            static_cast<unsigned>(tile / columns) * tile_size;
        for (unsigned y = 0; y < tile_size; ++y) {
            for (unsigned x = 0; x < tile_size; ++x) {
                const std::uint8_t palette_index =
                    tileset.tiles[tile][y * tile_size + x];
                pixels[
                    static_cast<std::size_t>(origin_y + y) * width +
                    origin_x + x] = palette[palette_index];
            }
        }
    }
    return pixels;
}

std::vector<Rgba> composite_map_layers(
    const std::vector<MapLayer>& source_layers,
    unsigned width,
    unsigned height) {
    std::vector<std::size_t> order(source_layers.size());
    for (std::size_t index = 0; index < order.size(); ++index) {
        order[index] = index;
    }
    std::stable_sort(
        order.begin(), order.end(),
        [&source_layers](std::size_t left, std::size_t right) {
            return source_layers[left].priority >
                   source_layers[right].priority;
        });

    std::vector<Rgba> composite(
        static_cast<std::size_t>(width) * height, {0, 0, 0, 0});
    for (const std::size_t layer_index : order) {
        const auto& layer = source_layers[layer_index];
        for (std::size_t pixel = 0; pixel < composite.size(); ++pixel) {
            if (layer.pixels[pixel].a != 0) {
                composite[pixel] = layer.pixels[pixel];
            }
        }
    }
    return composite;
}

std::string level_preview_stem(
    const LevelIndex& level_index,
    const LevelRecord& record,
    std::size_t record_index) {
    std::ostringstream stem;
    stem << "record_" << std::setw(3) << std::setfill('0') << record_index;
    if (!record.map_entries.empty()) {
        stem << '_' << map_file_label(
            level_index.entries[record.map_entries.front()]);
    }
    return stem.str();
}

std::size_t opening_record_index(const LevelIndex& level_index) {
    for (const auto& entry : level_index.entries) {
        if (entry.output == 1 && entry.zone == 1 &&
            entry.level_record_index !=
                std::numeric_limits<std::size_t>::max()) {
            return entry.level_record_index;
        }
    }
    return std::numeric_limits<std::size_t>::max();
}


std::string html_text(const std::string& text) {
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

std::vector<std::string> primary_area_names(
    const Rom& rom,
    const GameProfile& profile) {
    std::vector<std::string> names;
    names.reserve(profile.area_name_count);
    for (std::size_t area = 0; area < profile.area_name_count; ++area) {
        const std::uint32_t pointer = rom.u32(
            profile.area_name_table +
            area * profile.area_name_languages * 4);
        names.push_back(read_latin1_string(rom, pointer));
    }
    return names;
}

std::vector<unsigned> map_entry_area_candidates(
    const MapEntry& entry,
    std::size_t area_count) {
    const std::array<unsigned, 4> raw = {
        entry.output, entry.zone, entry.area, entry.variation};
    std::vector<unsigned> result;
    for (const unsigned value : raw) {
        if (value >= area_count ||
            std::find(result.begin(), result.end(), value) != result.end()) {
            continue;
        }
        result.push_back(value);
    }
    return result;
}

void write_named_level_candidates(
    const Rom& rom,
    const GameProfile& profile,
    const LevelIndex& level_index,
    const std::filesystem::path& path) {
    const auto names = primary_area_names(rom, profile);
    std::ofstream output(path);
    output << "area_id,area_name,map_entry,label,matched_field,raw_value,"
              "level_record,preview\n";
    for (const auto& entry : level_index.entries) {
        if (entry.level_record_index ==
            std::numeric_limits<std::size_t>::max()) {
            continue;
        }
        const std::array<std::pair<const char*, unsigned>, 4> fields = {{
            {"output", entry.output},
            {"zone", entry.zone},
            {"area", entry.area},
            {"variation", entry.variation}}};
        for (const auto& field : fields) {
            if (field.second >= names.size()) continue;
            const auto& record =
                level_index.records[entry.level_record_index];
            const std::string preview =
                level_preview_stem(
                    level_index, record, entry.level_record_index) +
                "_composite.png";
            output << field.second << ',' << csv_text(names[field.second])
                   << ',' << entry.index << ',' << csv_text(map_label(entry))
                   << ',' << field.first << ',' << field.second << ','
                   << entry.level_record_index << ',' << preview << '\n';
        }
    }
}

void write_level_gallery(
    const Rom& rom,
    const GameProfile& profile,
    const LevelIndex& level_index,
    const std::filesystem::path& output) {
    const auto names = primary_area_names(rom, profile);
    std::ofstream html(output / "level_gallery.html");
    if (!html) {
        throw std::runtime_error("cannot write level_gallery.html");
    }
    html << R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DragonByteZ Level Gallery</title>
<style>
:root{font-family:"Segoe UI",Arial,sans-serif;color:#17334a;background:#dff4ff}
*{box-sizing:border-box}body{margin:0;min-height:100vh;background:radial-gradient(circle at 20% 10%,#fff 0 8%,transparent 25%),linear-gradient(#bdeaff,#f5fcff 55%,#d9f4ff)}
header{position:sticky;top:0;z-index:10;padding:18px 24px;background:rgba(255,255,255,.92);border-bottom:1px solid #8bcbe8;box-shadow:0 8px 24px #5ba7c633;backdrop-filter:blur(10px)}
h1{margin:0;color:#ec6c18;font-size:30px;text-shadow:0 2px #fff}.sub{margin:4px 0 14px;color:#41677d}
.controls{display:flex;flex-wrap:wrap;gap:10px}input,select,button{font:inherit;border:1px solid #77bad8;border-radius:20px;padding:9px 14px;background:white;color:#17334a;box-shadow:inset 0 1px #fff,0 3px 8px #4b9fbe22}
input{min-width:260px;flex:1}button{cursor:pointer;font-weight:700;background:linear-gradient(#fff,#cfefff)}button:hover{background:linear-gradient(#fff4d2,#ffd393);border-color:#ed8b2d}
.notice{margin:18px 24px;padding:14px 18px;border-radius:22px;background:#fff9df;border:1px solid #f0bf5d;box-shadow:0 6px 20px #8a661522;line-height:1.45}
#count{font-weight:700;color:#d85b10}.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(310px,1fr));gap:18px;padding:0 24px 32px}.card{overflow:hidden;border:1px solid #86cbe9;border-radius:28px;background:rgba(255,255,255,.94);box-shadow:0 10px 28px #3986a52e}.card[hidden]{display:none}.imagebox{display:flex;align-items:center;justify-content:center;min-height:210px;background:linear-gradient(135deg,#bbebff,#f8fdff);border-bottom:1px solid #acd9ec}.imagebox img{display:block;max-width:100%;max-height:360px;image-rendering:pixelated}.body{padding:15px 17px 18px}.body h2{font-size:18px;margin:0 0 8px;color:#d85b10}.meta{font-size:13px;line-height:1.5;color:#35576b}.names{margin-top:9px;padding:8px 10px;border-radius:14px;background:#e9f8ff;color:#245775;font-size:13px}.empty{display:none;margin:30px;text-align:center;font-size:20px;color:#50778c}
</style>
</head>
<body>
<header><h1>DragonByteZ Level Gallery</h1><div class="sub">All renderable Webfoot map graphics records, searchable by map IDs, record numbers, and recovered names.</div><div class="controls">
<input id="search" type="search" placeholder="Search Z8A19, Haunted Swamp, record 221...">
<select id="area"><option value="">All localized area names</option>
)HTML";
    for (std::size_t area = 0; area < names.size(); ++area) {
        html << "<option value=\"" << area << "\">" << area << " - "
             << html_text(names[area]) << "</option>\n";
    }
    html << "</select>";
    if (!names.empty()) {
        html << "<button id=\"haunted\" type=\"button\">"
                "Haunted Swamp candidates</button>";
    }
    html << R"HTML(<button id="clear" type="button">Show all</button>
</div></header>
)HTML";
    if (!names.empty()) {
        html << R"HTML(<div class="notice"><span id="count"></span> The localized name table is verified, but its exact runtime link to a MapEntry field is still under reconstruction. Name filtering therefore shows every linked map whose raw <b>output</b>, <b>zone</b>, <b>area</b>, or <b>variation</b> value matches the selected localized-name ID. This exposes Haunted Swamp candidates without inventing a false exact map assignment.</div>
)HTML";
    } else {
        html << R"HTML(<div class="notice"><span id="count"></span> Map records and graphics links are decoded. A game-specific localized area-name table has not yet been assigned, so this gallery searches recovered map labels, record numbers, pointers, dimensions, and raw entry fields.</div>
)HTML";
    }
    html << R"HTML(<main class="grid" id="grid">
)HTML";

    for (std::size_t record_index = 0;
         record_index < level_index.records.size(); ++record_index) {
        const auto& record = level_index.records[record_index];
        const std::string stem =
            level_preview_stem(level_index, record, record_index);
        const std::filesystem::path preview_path =
            output / "level_previews" / "maps" /
            (stem + "_composite.png");
        if (!std::filesystem::is_regular_file(preview_path)) continue;

        std::vector<unsigned> area_ids;
        std::ostringstream raw_entries;
        for (const std::size_t entry_index : record.map_entries) {
            const auto& entry = level_index.entries[entry_index];
            if (raw_entries.tellp() > 0) raw_entries << " | ";
            raw_entries << map_label(entry)
                        << " [entry " << entry.index
                        << ", out " << static_cast<unsigned>(entry.output)
                        << ", zone " << static_cast<unsigned>(entry.zone)
                        << ", area " << static_cast<unsigned>(entry.area)
                        << ", var " << static_cast<unsigned>(entry.variation)
                        << ", music " << hex(entry.music_id) << ']';
            for (const unsigned candidate :
                 map_entry_area_candidates(entry, names.size())) {
                if (std::find(area_ids.begin(), area_ids.end(), candidate) ==
                    area_ids.end()) {
                    area_ids.push_back(candidate);
                }
            }
        }
        std::sort(area_ids.begin(), area_ids.end());
        std::ostringstream data_areas;
        std::ostringstream candidate_names;
        std::ostringstream search;
        search << "record " << record_index << ' ' << join_aliases(record.aliases)
               << ' ' << raw_entries.str();
        for (const unsigned area_id : area_ids) {
            data_areas << ',' << area_id;
            if (candidate_names.tellp() > 0) candidate_names << " | ";
            candidate_names << area_id << " - " << names[area_id];
            search << ' ' << names[area_id];
        }
        data_areas << ',';

        html << "<article class=\"card\" data-areas=\""
             << data_areas.str() << "\" data-search=\""
             << html_text(search.str()) << "\">"
             << "<a class=\"imagebox\" href=\"level_previews/maps/"
             << stem << "_composite.png\"><img loading=\"lazy\" src=\""
             << "level_previews/maps/" << stem
             << "_composite.png\" alt=\"Level record " << record_index
             << "\"></a><div class=\"body\"><h2>Record "
             << record_index << " - " << html_text(join_aliases(record.aliases))
             << "</h2><div class=\"meta\">Graphics " << hex(record.pointer)
             << " &middot; " << record.width << "&times;" << record.height
             << "<br>" << html_text(raw_entries.str())
             << "</div><div class=\"names\"><b>Name candidates:</b> "
             << html_text(candidate_names.str().empty()
                    ? std::string("not mapped yet")
                    : candidate_names.str())
             << "</div></div></article>\n";
    }

    html << R"HTML(</main><div class="empty" id="empty">No level previews match this filter.</div>
<script>
const cards=[...document.querySelectorAll('.card')];
const search=document.getElementById('search');
const area=document.getElementById('area');
const count=document.getElementById('count');
const empty=document.getElementById('empty');
function filter(){const q=search.value.trim().toLowerCase();const id=area.value;let shown=0;for(const card of cards){const text=card.dataset.search.toLowerCase();const areaMatch=!id||card.dataset.areas.includes(','+id+',');const show=areaMatch&&(!q||text.includes(q));card.hidden=!show;if(show)shown++;}count.textContent=shown+' of '+cards.length+' previews shown.';empty.style.display=shown?'none':'block';}
search.addEventListener('input',filter);area.addEventListener('change',filter);
const haunted=document.getElementById('haunted');if(haunted){haunted.addEventListener('click',()=>{area.value='19';search.value='';filter();});}
document.getElementById('clear').addEventListener('click',()=>{area.value='';search.value='';filter();});
filter();
</script></body></html>)HTML";
}

struct GbaLz77Data {
    std::vector<std::uint8_t> data;
    std::size_t input_end = 0;
};

GbaLz77Data decompress_gba_lz77(
    const Rom& rom,
    std::size_t offset,
    std::size_t maximum_output = 4U * 1024U * 1024U) {
    if (offset + 4 > rom.size() || rom.u8(offset) != 0x10) {
        throw std::runtime_error("not a GBA BIOS LZ77 stream");
    }
    const std::size_t output_size =
        static_cast<std::size_t>(rom.u8(offset + 1)) |
        (static_cast<std::size_t>(rom.u8(offset + 2)) << 8U) |
        (static_cast<std::size_t>(rom.u8(offset + 3)) << 16U);
    if (output_size == 0 || output_size > maximum_output) {
        throw std::runtime_error("invalid GBA LZ77 output size");
    }

    GbaLz77Data result;
    result.data.reserve(output_size);
    std::size_t input = offset + 4;
    while (result.data.size() < output_size) {
        if (input >= rom.size()) {
            throw std::runtime_error("truncated GBA LZ77 flags");
        }
        const std::uint8_t flags = rom.u8(input++);
        for (unsigned bit = 0; bit < 8 && result.data.size() < output_size;
             ++bit) {
            if ((flags & (0x80U >> bit)) == 0) {
                if (input >= rom.size()) {
                    throw std::runtime_error("truncated GBA LZ77 literal");
                }
                result.data.push_back(rom.u8(input++));
                continue;
            }
            if (input + 2 > rom.size()) {
                throw std::runtime_error("truncated GBA LZ77 match");
            }
            const std::uint16_t packed =
                static_cast<std::uint16_t>(rom.u8(input) << 8U) |
                rom.u8(input + 1);
            input += 2;
            const std::size_t length = (packed >> 12U) + 3U;
            const std::size_t distance = (packed & 0x0FFFU) + 1U;
            if (distance > result.data.size()) {
                throw std::runtime_error("invalid GBA LZ77 back-reference");
            }
            for (std::size_t copy = 0;
                 copy < length && result.data.size() < output_size; ++copy) {
                result.data.push_back(
                    result.data[result.data.size() - distance]);
            }
        }
    }
    result.input_end = input;
    return result;
}

[[maybe_unused]] std::map<std::size_t, std::vector<std::size_t>> scan_rom_pointers(
    const Rom& rom) {
    std::map<std::size_t, std::vector<std::size_t>> references;
    for (std::size_t offset = 0; offset + 4 <= rom.size(); offset += 4) {
        const std::uint32_t pointer = rom.u32(offset);
        if (!rom.is_rom_pointer(pointer)) continue;
        references[rom.pointer_to_offset(pointer)].push_back(offset);
    }
    return references;
}

[[maybe_unused]] void write_pointer_index(
    const std::map<std::size_t, std::vector<std::size_t>>& references,
    const std::filesystem::path& path) {
    std::ofstream output(path);
    output << "target_file_offset,target_pointer,reference_count,"
              "reference_offsets\n";
    for (const auto& item : references) {
        output << hex(item.first, 7) << ','
               << hex(0x08000000U + item.first) << ','
               << item.second.size() << ',' << csv_text([&item]() {
                      std::ostringstream offsets;
                      for (std::size_t index = 0;
                           index < item.second.size(); ++index) {
                          if (index != 0) offsets << " | ";
                          offsets << hex(item.second[index], 7);
                      }
                      return offsets.str();
                  }()) << '\n';
    }
}

[[maybe_unused]] void write_ascii_strings(const Rom& rom, const std::filesystem::path& path) {
    std::ofstream output(path);
    output << "file_offset,length,text\n";
    std::size_t offset = 0;
    while (offset < rom.size()) {
        const std::size_t start = offset;
        while (offset < rom.size()) {
            const std::uint8_t value = rom.u8(offset);
            if (value < 0x20 || value > 0x7E) break;
            ++offset;
        }
        if (offset - start >= 4) {
            std::string text;
            text.reserve(offset - start);
            for (std::size_t index = start; index < offset; ++index) {
                text.push_back(static_cast<char>(rom.u8(index)));
            }
            output << hex(start, 7) << ',' << text.size() << ','
                   << csv_text(text) << '\n';
        }
        offset = std::max(offset + 1, start + 1);
    }
}

void write_palette_bytes_csv(
    const std::vector<std::uint8_t>& bytes,
    const std::filesystem::path& path) {
    if (bytes.size() < 512) return;
    std::ofstream output(path);
    output << "index,rgb555,red,green,blue,hex_rgb\n";
    for (std::size_t index = 0; index < 256; ++index) {
        const std::uint16_t value =
            static_cast<std::uint16_t>(bytes[index * 2]) |
            (static_cast<std::uint16_t>(bytes[index * 2 + 1]) << 8U);
        const auto expand = [](unsigned part) {
            return static_cast<unsigned>((part << 3U) | (part >> 2U));
        };
        const unsigned red = expand(value & 31U);
        const unsigned green = expand((value >> 5U) & 31U);
        const unsigned blue = expand((value >> 10U) & 31U);
        std::ostringstream colour;
        colour << '#' << std::uppercase << std::hex << std::setw(2)
               << std::setfill('0') << red << std::setw(2) << green
               << std::setw(2) << blue;
        output << index << ',' << hex(value, 4) << ',' << std::dec << red
               << ',' << green << ',' << blue << ',' << colour.str() << '\n';
    }
}

[[maybe_unused]] void write_log1_lz77_candidates(
    const Rom& rom,
    const std::map<std::size_t, std::vector<std::size_t>>& references,
    const std::filesystem::path& output) {
    std::filesystem::remove_all(output / "compressed_candidates");
    std::filesystem::create_directories(output / "compressed_candidates");
    std::ofstream csv(output / "gba_lz77_candidates.csv");
    csv << "candidate,file_offset,pointer,reference_count,compressed_size,"
           "decompressed_size,binary,palette_png,diagnostic_8bpp,"
           "diagnostic_4bpp\n";
    std::size_t candidate_index = 0;
    for (const auto& item : references) {
        const std::size_t offset = item.first;
        if (offset + 4 > rom.size() || rom.u8(offset) != 0x10) continue;
        GbaLz77Data decoded;
        try {
            decoded = decompress_gba_lz77(rom, offset);
        } catch (const std::exception&) {
            continue;
        }
        if (decoded.data.size() < 32) continue;

        std::ostringstream base;
        base << "candidate_" << std::setw(3) << std::setfill('0')
             << candidate_index << "_" << hex(offset, 7).substr(2);
        const std::string binary_name = base.str() + ".bin";
        write_binary(
            output / "compressed_candidates" / binary_name, decoded.data);

        std::string palette_name;
        std::string diagnostic_8bpp_name;
        std::string diagnostic_4bpp_name;
        if (decoded.data.size() == 512) {
            palette_name = base.str() + "_palette.png";
            const auto palette = gba_rgb555_palette(decoded.data);
            write_png_rgba(
                output / "compressed_candidates" / palette_name,
                128, 128, make_palette_swatch(palette));
            write_palette_bytes_csv(
                decoded.data,
                output / "compressed_candidates" /
                    (base.str() + "_palette.csv"));
        }
        if (decoded.data.size() >= 1024) {
            const std::size_t tiles_8bpp = decoded.data.size() / 64U;
            if (tiles_8bpp != 0) {
                constexpr unsigned columns = 16;
                const unsigned rows = static_cast<unsigned>(
                    (tiles_8bpp + columns - 1) / columns);
                std::vector<std::uint8_t> padded(
                    static_cast<std::size_t>(columns) * rows * 64U, 0);
                std::copy_n(
                    decoded.data.begin(), tiles_8bpp * 64U, padded.begin());
                diagnostic_8bpp_name = base.str() + "_8bpp.png";
                write_png_rgba(
                    output / "compressed_candidates" /
                        diagnostic_8bpp_name,
                    columns * 8U, rows * 8U,
                    colorize(
                        untile_8bpp(padded, columns * 8U, rows * 8U),
                        diagnostic_palette()));
            }
            const std::size_t tiles_4bpp = decoded.data.size() / 32U;
            if (tiles_4bpp != 0) {
                constexpr unsigned columns = 16;
                const unsigned rows = static_cast<unsigned>(
                    (tiles_4bpp + columns - 1) / columns);
                std::vector<std::uint8_t> padded(
                    static_cast<std::size_t>(columns) * rows * 32U, 0);
                std::copy_n(
                    decoded.data.begin(), tiles_4bpp * 32U, padded.begin());
                diagnostic_4bpp_name = base.str() + "_4bpp.png";
                write_png_rgba(
                    output / "compressed_candidates" /
                        diagnostic_4bpp_name,
                    columns * 8U, rows * 8U,
                    colorize(
                        untile_4bpp(padded, columns * 8U, rows * 8U),
                        diagnostic_palette()));
            }
        }

        csv << candidate_index << ',' << hex(offset, 7) << ','
            << hex(0x08000000U + offset) << ',' << item.second.size() << ','
            << decoded.input_end - offset << ',' << decoded.data.size() << ','
            << binary_name << ',' << palette_name << ','
            << diagnostic_8bpp_name << ',' << diagnostic_4bpp_name << '\n';
        ++candidate_index;
    }
}

struct Log1AudioRecord {
    std::size_t offset = 0;
    std::uint32_t pointer = 0;
    std::uint16_t flags = 0;
    std::uint16_t loop_start = 0;
    std::uint16_t raw_size = 0;
    std::uint16_t sample_rate = 0;
};

bool read_log1_audio_record(
    const Rom& rom,
    std::size_t offset,
    Log1AudioRecord& record) {
    if (offset + 12 > rom.size()) return false;
    record.offset = offset;
    record.pointer = rom.u32(offset);
    record.flags = rom.u16(offset + 4);
    record.loop_start = rom.u16(offset + 6);
    record.raw_size = rom.u16(offset + 8);
    record.sample_rate = rom.u16(offset + 10);
    if (!rom.is_rom_pointer(record.pointer) || record.flags > 3 ||
        record.raw_size < 16 || record.loop_start > record.raw_size ||
        record.sample_rate < 3000 || record.sample_rate > 32000) {
        return false;
    }
    const std::size_t sample_offset = rom.pointer_to_offset(record.pointer);
    return sample_offset <= rom.size() &&
           record.raw_size <= rom.size() - sample_offset;
}

[[maybe_unused]] void write_log1_audio_candidates(
    const Rom& rom,
    const std::filesystem::path& output) {
    std::filesystem::remove_all(output / "candidate_samples");
    std::filesystem::create_directories(output / "candidate_samples");
    std::ofstream runs_csv(output / "audio_candidate_runs.csv");
    runs_csv << "run,table_offset,record_count,first_sample_pointer,"
                "last_sample_pointer\n";
    std::ofstream samples_csv(output / "audio_candidate_samples.csv");
    samples_csv << "run,record,record_offset,pointer,file_offset,flags,"
                   "loop_start,raw_size,sample_rate,wav\n";

    std::map<std::tuple<std::uint32_t, std::uint16_t, std::uint16_t>,
             std::string> exported;
    std::size_t run_index = 0;
    for (std::size_t offset = 0; offset + 12 <= rom.size(); offset += 4) {
        Log1AudioRecord first;
        if (!read_log1_audio_record(rom, offset, first)) continue;
        if (offset >= 12) {
            Log1AudioRecord previous;
            if (read_log1_audio_record(rom, offset - 12, previous)) continue;
        }

        std::vector<Log1AudioRecord> run;
        for (std::size_t record_offset = offset;
             record_offset + 12 <= rom.size(); record_offset += 12) {
            Log1AudioRecord record;
            if (!read_log1_audio_record(rom, record_offset, record)) break;
            run.push_back(record);
        }
        if (run.size() < 3) continue;

        runs_csv << run_index << ',' << hex(offset, 7) << ',' << run.size()
                 << ',' << hex(run.front().pointer) << ','
                 << hex(run.back().pointer) << '\n';
        for (std::size_t record_index = 0;
             record_index < run.size(); ++record_index) {
            const auto& record = run[record_index];
            const auto key = std::make_tuple(
                record.pointer, record.raw_size, record.sample_rate);
            auto exported_item = exported.find(key);
            if (exported_item == exported.end()) {
                std::ostringstream filename;
                filename << "sample_" << std::setw(3) << std::setfill('0')
                         << exported.size() << '_' << record.sample_rate
                         << "hz.wav";
                const std::string name = filename.str();
                write_wav(
                    output / "candidate_samples" / name,
                    rom.slice(
                        rom.pointer_to_offset(record.pointer),
                        record.raw_size),
                    record.sample_rate);
                exported_item = exported.emplace(key, name).first;
            }
            samples_csv << run_index << ',' << record_index << ','
                        << hex(record.offset, 7) << ','
                        << hex(record.pointer) << ','
                        << hex(rom.pointer_to_offset(record.pointer), 7) << ','
                        << record.flags << ',' << record.loop_start << ','
                        << record.raw_size << ',' << record.sample_rate << ','
                        << exported_item->second << '\n';
        }
        ++run_index;
        offset += run.size() * 12U - 4U;
    }

    std::ofstream note(output / "LOG1_soundtrack_notes.txt");
    note << "DragonByteZ LOG1 experimental soundtrack scan\n"
         << "=============================================\n\n"
         << "The Legacy of Goku uses a different data layout from LOG2. "
            "DragonByteZ does not reuse LOG2's hard-coded sample or BGM "
            "tables. Instead, it locates contiguous candidate runs of "
            "12-byte records containing a ROM pointer, flags, loop point, "
            "sample byte length and plausible sample rate. Only runs with "
            "at least three consecutive valid records are exported.\n"
         << "The WAV files are candidate instrument/effect samples, not "
            "claims that complete songs are raw PCM. Full LOG1 sequence and "
            "song-table semantics remain under reconstruction.\n";
}


bool valid_optional_rom_pointer(const Rom& rom, std::uint32_t value) {
    return value == 0 || rom.is_rom_pointer(value);
}

bool plausible_webfoot_map_entry(const Rom& rom, std::size_t offset) {
    constexpr std::size_t stride = 0x38;
    if (offset + stride > rom.size()) return false;

    const unsigned output = rom.u8(offset + 0x00);
    const unsigned zone = rom.u8(offset + 0x01);
    const unsigned area = rom.u8(offset + 0x02);
    const unsigned variation = rom.u8(offset + 0x03);
    if (output > 127 || zone > 127 || area > 127 || variation > 127) {
        return false;
    }
    for (std::size_t field = 0x04; field <= 0x08; ++field) {
        if (rom.u8(offset + field) > 96) return false;
    }

    for (const std::size_t field : {
             std::size_t(0x10), std::size_t(0x14), std::size_t(0x18),
             std::size_t(0x1C), std::size_t(0x20), std::size_t(0x28),
             std::size_t(0x2C), std::size_t(0x30), std::size_t(0x34)}) {
        if (!valid_optional_rom_pointer(rom, rom.u32(offset + field))) {
            return false;
        }
    }

    const std::uint32_t variation_array = rom.u32(offset + 0x2C);
    if (!rom.is_rom_pointer(variation_array)) return false;
    const std::size_t array_offset = rom.pointer_to_offset(variation_array);
    if (array_offset + 4 > rom.size()) return false;
    const std::uint32_t graphics_pointer = rom.u32(array_offset);
    return graphics_pointer == 0 || rom.is_rom_pointer(graphics_pointer);
}

std::size_t count_decompressible_tile_atlases(
    const Rom& rom,
    std::size_t table,
    std::size_t maximum_count = 1024) {
    std::size_t count = 0;
    for (; count < maximum_count && table + (count + 1) * 4 <= rom.size();
         ++count) {
        const std::uint32_t pointer = rom.u32(table + count * 4);
        if (!rom.is_rom_pointer(pointer)) break;
        try {
            const auto decoded =
                decompress_container(rom, rom.pointer_to_offset(pointer));
            if (decoded.data.size() != 16384 && decoded.data.size() != 8192) {
                break;
            }
        } catch (const std::exception&) {
            break;
        }
    }
    return count;
}

[[maybe_unused]] GameProfile discover_log1_webfoot_profile(const Rom& rom) {
    if (!is_log1_rom(rom)) {
        throw std::runtime_error(
            "LOG1 Webfoot layout discovery requires ALGP Europe Rev 0");
    }

    constexpr std::size_t stride = 0x38;
    std::size_t best_table = 0;
    std::size_t best_count = 0;
    for (std::size_t phase = 0; phase < stride / 4; ++phase) {
        std::size_t run_table = 0;
        std::size_t run_count = 0;
        for (std::size_t offset = phase * 4;
             offset + stride <= rom.size(); offset += stride) {
            if (plausible_webfoot_map_entry(rom, offset)) {
                if (run_count == 0) run_table = offset;
                ++run_count;
            } else {
                if (run_count > best_count) {
                    best_table = run_table;
                    best_count = run_count;
                }
                run_count = 0;
            }
        }
        if (run_count > best_count) {
            best_table = run_table;
            best_count = run_count;
        }
    }
    if (best_count < 8) {
        throw std::runtime_error(
            "could not locate a contiguous LOG1 Webfoot map-entry table");
    }

    std::map<std::uint32_t, std::size_t> atlas_usage;
    std::size_t linked_graphics_records = 0;
    for (std::size_t index = 0; index < best_count; ++index) {
        const std::size_t entry = best_table + index * stride;
        const std::uint32_t variation_array = rom.u32(entry + 0x2C);
        if (!rom.is_rom_pointer(variation_array)) continue;
        const std::size_t array_offset = rom.pointer_to_offset(variation_array);
        if (array_offset + 4 > rom.size()) continue;
        const std::uint32_t graphics_pointer = rom.u32(array_offset);
        if (!rom.is_rom_pointer(graphics_pointer)) continue;
        const std::size_t graphics_offset =
            rom.pointer_to_offset(graphics_pointer);
        if (graphics_offset + 0x50 > rom.size()) continue;
        const std::uint32_t static_tileset = rom.u32(graphics_offset + 0x48);
        const std::uint32_t atlas_pointer = rom.u32(graphics_offset + 0x4C);
        if (!rom.is_rom_pointer(static_tileset) ||
            !rom.is_rom_pointer(atlas_pointer)) {
            continue;
        }
        bool has_descriptor = false;
        for (std::size_t layer = 0; layer < 4; ++layer) {
            if (valid_level_descriptor(
                    rom, rom.u32(graphics_offset + 0x14 + layer * 4))) {
                has_descriptor = true;
                break;
            }
        }
        if (!has_descriptor) continue;
        ++atlas_usage[atlas_pointer];
        ++linked_graphics_records;
    }
    if (linked_graphics_records < 4 || atlas_usage.empty()) {
        throw std::runtime_error(
            "LOG1 map records were found, but their graphics links did not "
            "validate");
    }

    std::uint32_t best_atlas_pointer = 0;
    std::size_t best_usage = 0;
    std::size_t best_atlas_count = 0;
    for (const auto& [pointer, usage] : atlas_usage) {
        const std::size_t table = rom.pointer_to_offset(pointer);
        const std::size_t atlas_count =
            count_decompressible_tile_atlases(rom, table);
        if (atlas_count >= 4 &&
            (usage > best_usage ||
             (usage == best_usage && atlas_count > best_atlas_count))) {
            best_atlas_pointer = pointer;
            best_usage = usage;
            best_atlas_count = atlas_count;
        }
    }
    if (best_atlas_pointer == 0) {
        throw std::runtime_error(
            "LOG1 linked graphics records did not expose a valid compressed "
            "tile-atlas table");
    }

    GameProfile profile;
    profile.name = "Europe Rev 0 discovered Webfoot map profile";
    profile.game_code = "ALGP";
    profile.expected_size = rom.size();
    profile.map_entry_table = best_table;
    profile.map_entry_count = best_count;
    profile.level_tileset_table =
        rom.pointer_to_offset(best_atlas_pointer);
    profile.level_tileset_count = best_atlas_count;
    profile.level_record_tileset_pointer_offset = 0x4C;
    return profile;
}

std::string log1_level_summary(const Rom& rom) {
    if (!is_log1_rom(rom)) {
        throw std::runtime_error(
            "this command requires LOG1 Europe Rev 0 (ALGP)");
    }
    std::ostringstream summary;
    summary << "Legacy of Goku Europe Rev 0 (ALGP) detected.\n"
            << log1_runtime_summary();
    return summary.str();
}

void analyze_log1_graphics(
    const Rom& rom,
    const std::filesystem::path& output) {
    export_log1_runtime_graphics(rom, output);
}

void analyze_log1_soundtrack(
    const Rom& rom,
    const std::filesystem::path& output) {
    export_log1_runtime_soundtrack(rom, output, 12);
}

void analyze_log1_all(
    const Rom& rom,
    const std::filesystem::path& output) {
    std::filesystem::create_directories(output);
    analyze_log1_graphics(rom, output / "graphics");
    analyze_log1_soundtrack(rom, output / "soundtrack");
    std::ofstream report(output / "DragonByteZ_report.txt");
    report << "DragonByteZ 0.6.23 Legacy of Goku analysis report\n"
           << "================================================\n\n"
           << "ROM title: " << rom.title() << '\n'
           << "Game code: " << rom.game_code() << '\n'
           << "Revision: " << static_cast<unsigned>(rom.revision()) << '\n'
           << "Size: " << rom.size() << " bytes\n\n"
           << log1_runtime_summary() << "\n\n"
           << "Character graphics are assembled through the original game's "
              "hidden Sprite Viewer. The old Go To Map viewport screenshots "
              "were removed because they were not complete level layers. "
              "Music is selected through the hidden Play Music sound test and "
              "written only as stereo WAV previews.\n";
}

} // namespace

std::string log2_level_summary(const Rom& rom) {
    require_log2_profile(rom);
    const auto& profile = profile_for(rom);
    const auto level_index = build_level_index(rom, profile);
    if (level_index.records.empty()) return "No verified level records found.";

    const std::size_t opening_index = opening_record_index(level_index);
    std::ostringstream summary;
    summary << "Map-entry table: " << hex(level_index.map_entry_table, 7)
            << "   Entries: " << level_index.entries.size() << '\n'
            << "Unique graphics records: " << level_index.records.size()
            << "   Localized area names: " << profile.area_name_count
            << "   8bpp tile atlases: " << profile.level_tileset_count;
    if (opening_index != std::numeric_limits<std::size_t>::max()) {
        const auto& opening = level_index.records[opening_index];
        unsigned active_layers = 0;
        for (const std::uint32_t pointer : opening.descriptors) {
            if (pointer != 0) ++active_layers;
        }
        summary << '\n'
                << "Opening map Z1A1: " << hex(opening.pointer)
                << " (file " << hex(opening.offset, 7) << ")"
                << "   Size: " << opening.width << 'x' << opening.height
                << "   Layers: " << active_layers;
    }
    return summary.str();
}

void write_header_report(const Rom& rom) {
    std::string profile = "unknown";
    if (is_log2_rom(rom)) {
        profile = std::string("Legacy of Goku II ") + profile_for(rom).name;
    } else if (is_log1_rom(rom)) {
        profile = "Legacy of Goku Europe Rev 0 (discovered Webfoot tables)";
    } else if (is_buus_fury_rom(rom)) {
        profile = std::string("Buu's Fury ") + profile_for(rom).name;
    }
    std::cout << "Title: " << rom.title() << '\n'
              << "Game code: " << rom.game_code() << '\n'
              << "Revision: " << static_cast<unsigned>(rom.revision()) << '\n'
              << "ROM size: " << rom.size() << " bytes\n"
              << "Game family: " << game_family_name(game_family(rom)) << '\n'
              << "Profile: " << profile << '\n';
}

void remove_obsolete_sprite_outputs(
    const std::filesystem::path& output) {
    std::filesystem::remove_all(output / "character_sprite_candidates");
    std::filesystem::remove_all(output / "character_sprites");
    std::filesystem::remove(output / "character_sprites.csv");
    std::filesystem::remove(output / "character_sprite_candidates.csv");
    std::filesystem::remove(output / "character_sprite_gallery.html");
    std::filesystem::remove(output / "asset_gallery.html");
    std::filesystem::remove(output / "bios_compressed_assets.csv");
    std::filesystem::remove_all(output / "compressed_assets");
    std::filesystem::remove_all(output / "fullscreen_art");
    std::filesystem::remove_all(output / "level_visuals");
    std::filesystem::remove_all(output / "palettes");
    std::filesystem::remove(output / "palettes.csv");
    std::filesystem::remove_all(output / "data");
}

void analyze_webfoot_graphics(
    const Rom& rom,
    const GameProfile& profile,
    const std::filesystem::path& output) {
    const bool verified_log2 = is_log2_rom(rom);
    const bool buus_fury = is_buus_fury_rom(rom);
    std::filesystem::create_directories(output);
    remove_obsolete_sprite_outputs(output);
    std::filesystem::remove_all(output / "level_tile_groups");
    std::filesystem::remove_all(output / "level_tilesets");
    std::filesystem::remove_all(output / "level_previews");
    std::filesystem::remove_all(output / "character_art");
    std::filesystem::remove(output / "level_tile_group_contact_sheet.png");
    std::filesystem::remove(output / "level_tile_groups.csv");
    std::filesystem::remove(output / "level_tilesets.csv");
    std::filesystem::remove(output / "raw_tile_atlas_diagnostic.png");
    std::filesystem::remove(output / "level_tileset_contact_sheet.png");
    std::filesystem::create_directories(output / "level_tilesets");
    std::filesystem::create_directories(output / "level_previews" / "maps");
    std::filesystem::create_directories(
        output / "level_previews" / "local_tilesets");
    std::filesystem::create_directories(output / "level_previews" / "opening");
    std::filesystem::create_directories(output / "character_art");

    if (profile.title_image != 0 && profile.title_palette != 0) {
        const auto title_indices =
            decompress_container(rom, profile.title_image).data;
        const auto title_palette_raw = rom.slice(profile.title_palette, 512);
        const auto title_pixels =
            colorize(title_indices, gba_rgb555_palette(title_palette_raw));
        write_png_rgba(
            output / "character_art" / "title_character_art.png",
            240, 160, title_pixels);
    }

    if (profile.character_display_table != 0 &&
        profile.character_display_count != 0) {
        write_character_display_csv(
            rom, profile, output / "character_display_records.csv");
    }

    std::vector<Rgba> background_palette;
    if (profile.default_bg_palette != 0) {
        const auto background_palette_raw =
            rom.slice(profile.default_bg_palette, 512);
        background_palette = gba_rgb555_palette(background_palette_raw);
        write_palette_csv(
            rom, profile.default_bg_palette,
            output / "default_bg_palette.csv");
        write_png_rgba(
            output / "default_bg_palette.png", 128, 128,
            make_palette_swatch(background_palette));
    } else {
        background_palette = diagnostic_palette();
        write_png_rgba(
            output / "diagnostic_map_palette.png", 128, 128,
            make_palette_swatch(background_palette));
    }

    if (profile.default_obj_palette != 0) {
        const auto object_palette_raw =
            rom.slice(profile.default_obj_palette, 512);
        const auto object_palette = gba_rgb555_palette(object_palette_raw);
        write_palette_csv(
            rom, profile.default_obj_palette,
            output / "default_obj_palette.csv");
        write_png_rgba(
            output / "default_obj_palette.png", 128, 128,
            make_palette_swatch(object_palette));
    }

    const auto level_index = build_level_index(rom, profile);
    if (level_index.records.empty()) {
        throw std::runtime_error("no map-linked level graphics records found");
    }
    if (profile.area_name_table != 0 && profile.area_name_count != 0) {
        write_area_names(rom, profile, output / "area_names.csv");
    }
    write_map_entries(rom, level_index, output / "map_entries.csv");
    write_level_records(level_index.records, output / "level_records.csv");

    constexpr std::size_t atlas_size = 128 * 128;
    std::ofstream tiles_csv(output / "level_tilesets.csv");
    tiles_csv << "index,pointer,file_offset,kind,raw_size,packed_size,"
                 "palette,png,status\n";
    std::vector<std::vector<Rgba>> contact_images;
    std::vector<std::vector<std::uint8_t>> atlases;
    atlases.reserve(profile.level_tileset_count);
    auto map_palette = background_palette;
    map_palette[0].a = 0;
    for (std::size_t index = 0;
         index < profile.level_tileset_count; ++index) {
        const std::uint32_t pointer =
            rom.u32(profile.level_tileset_table + index * 4);
        if (!rom.is_rom_pointer(pointer)) {
            throw std::runtime_error(
                "invalid tile-atlas pointer " + std::to_string(index));
        }
        const auto decoded = decompress_container(
            rom, rom.pointer_to_offset(pointer));
        if (decoded.data.size() > atlas_size) {
            throw std::runtime_error(
                "tile atlas " + std::to_string(index) +
                " expanded beyond 16 KiB");
        }
        const bool four_bpp = decoded.data.size() == atlas_size / 2;
        std::vector<std::uint8_t> atlas;
        if (four_bpp) {
            atlas.reserve(atlas_size);
            for (const std::uint8_t packed : decoded.data) {
                atlas.push_back(static_cast<std::uint8_t>(packed & 0x0FU));
                atlas.push_back(static_cast<std::uint8_t>(packed >> 4U));
            }
        } else {
            atlas = decoded.data;
        }
        const bool padded = atlas.size() < atlas_size;
        atlas.resize(atlas_size, 0);
        atlases.push_back(atlas);
        const auto pixels = colorize(
            untile_8bpp(atlas, 128, 128), map_palette);
        const std::string filename =
            numbered_name("tileset_", index, ".png");
        write_png_rgba(
            output / "level_tilesets" / filename, 128, 128, pixels);
        contact_images.push_back(pixels);
        tiles_csv << index << ',' << hex(pointer) << ','
                  << hex(rom.pointer_to_offset(pointer), 7) << ','
                  << decoded.kind << ',' << decoded.data.size() << ','
                  << decoded.packed_size() << ','
                  << (profile.default_bg_palette != 0
                          ? hex(profile.default_bg_palette, 7)
                          : "diagnostic") << ','
                  << filename << ',';
        if (four_bpp) {
            tiles_csv << "4bpp GBA tile order; unpacked to 8-bit indices";
        } else if (padded) {
            tiles_csv << "8bpp GBA tile order; padded partial atlas";
        } else {
            tiles_csv << "8bpp GBA tile order; complete 256-tile atlas";
        }
        tiles_csv << '\n';
    }

    constexpr unsigned atlas_columns = 12;
    constexpr unsigned atlas_width = 128;
    constexpr unsigned atlas_height = 128;
    constexpr unsigned atlas_padding = 4;
    const auto contact = make_contact_sheet(
        contact_images, atlas_width, atlas_height,
        atlas_columns, atlas_padding);
    const unsigned atlas_rows = static_cast<unsigned>(
        (contact_images.size() + atlas_columns - 1) / atlas_columns);
    write_png_rgba(
        output / "level_tileset_contact_sheet.png",
        atlas_columns * (atlas_width + atlas_padding * 2),
        atlas_rows * (atlas_height + atlas_padding * 2),
        contact);

    const std::size_t opening_index = opening_record_index(level_index);
    std::ofstream map_csv(output / "level_preview_layers.csv");
    map_csv << "record,labels,level_record,layer,descriptor,priority,chunks,"
               "highest_tile,static_tiles,animated_tiles,composite_preview,"
               "status\n";
    std::ofstream preview_csv(output / "level_preview_index.csv");
    preview_csv << "record,labels,width,height,static_tiles,animated_tiles,"
                   "local_tileset,composite_preview,status\n";

    std::size_t rendered_records = 0;
    std::size_t failed_records = 0;
    for (std::size_t record_index = 0;
         record_index < level_index.records.size(); ++record_index) {
        const auto& record = level_index.records[record_index];
        const std::string stem =
            level_preview_stem(level_index, record, record_index);
        const std::string labels = join_aliases(record.aliases);
        try {
            const std::uint64_t pixel_count =
                static_cast<std::uint64_t>(record.width) * record.height;
            if (pixel_count > 16U * 1024U * 1024U) {
                throw std::runtime_error(
                    "map dimensions exceed the 16-megapixel preview limit");
            }

            const auto tileset = build_map_tileset(rom, record, atlases);
            unsigned tileset_width = 0;
            unsigned tileset_height = 0;
            const auto tileset_pixels = make_local_tileset_image(
                tileset, map_palette, tileset_width, tileset_height);
            const std::string tileset_name = stem + "_tileset.png";
            write_png_rgba(
                output / "level_previews" / "local_tilesets" / tileset_name,
                tileset_width, tileset_height, tileset_pixels);

            std::vector<MapLayer> rendered_layers;
            for (std::size_t layer = 0;
                 layer < record.descriptors.size(); ++layer) {
                if (!record.descriptors[layer]) continue;
                const std::size_t descriptor =
                    rom.pointer_to_offset(record.descriptors[layer]);
                try {
                    auto rendered = render_map_layer(
                        rom, descriptor, record.width, record.height,
                        tileset, map_palette);
                    map_csv << record_index << ',' << csv_text(labels) << ','
                            << hex(record.pointer) << ',' << layer << ','
                            << hex(record.descriptors[layer]) << ','
                            << rendered.priority << ',' << rendered.chunks
                            << ',' << rendered.highest_tile << ','
                            << tileset.static_tiles << ','
                            << tileset.animated_tiles << ',' << stem
                            << "_composite.png,rendered\n";
                    if (record_index == opening_index) {
                        const std::string layer_name =
                            "Z1A1_layer_" + std::to_string(layer) + ".png";
                        write_png_rgba(
                            output / "level_previews" / "opening" /
                                layer_name,
                            record.width, record.height, rendered.pixels);
                    }
                    rendered_layers.push_back(std::move(rendered));
                } catch (const std::exception& error) {
                    map_csv << record_index << ',' << csv_text(labels) << ','
                            << hex(record.pointer) << ',' << layer << ','
                            << hex(record.descriptors[layer])
                            << ",,,,,," << stem << "_composite.png,"
                            << csv_text(error.what()) << '\n';
                }
            }
            if (rendered_layers.empty()) {
                throw std::runtime_error("record has no renderable map layers");
            }

            const auto composite = composite_map_layers(
                rendered_layers, record.width, record.height);
            const auto preview_size = fit_dimensions(
                record.width, record.height, 512);
            const auto preview_pixels = scale_nearest(
                composite, record.width, record.height,
                preview_size.first, preview_size.second);
            const std::string composite_name = stem + "_composite.png";
            write_png_rgba(
                output / "level_previews" / "maps" / composite_name,
                preview_size.first, preview_size.second, preview_pixels);

            if (record_index == opening_index) {
                write_png_rgba(
                    output / "level_previews" / "opening" /
                        "Z1A1_composite.png",
                    record.width, record.height, composite);
                std::vector<std::vector<Rgba>> opening_contact;
                for (const auto& layer : rendered_layers) {
                    opening_contact.push_back(scale_nearest(
                        layer.pixels, record.width, record.height, 512, 512));
                }
                const unsigned contact_columns = 2;
                const unsigned contact_rows = static_cast<unsigned>(
                    (opening_contact.size() + contact_columns - 1) /
                    contact_columns);
                write_png_rgba(
                    output / "level_previews" / "opening" /
                        "Z1A1_layers_contact_sheet.png",
                    contact_columns * 520,
                    contact_rows * 520,
                    make_contact_sheet(
                        opening_contact, 512, 512,
                        contact_columns, 4));
            }

            preview_csv << record_index << ',' << csv_text(labels) << ','
                        << record.width << ',' << record.height << ','
                        << tileset.static_tiles << ','
                        << tileset.animated_tiles << ',' << tileset_name << ','
                        << composite_name << ',';
            if (tileset.invalid_global_tiles == 0) {
                preview_csv << "rendered\n";
            } else {
                preview_csv << csv_text(
                    "rendered with " +
                    std::to_string(tileset.invalid_global_tiles) +
                    " out-of-range global tile references left transparent")
                            << '\n';
            }
            ++rendered_records;
        } catch (const std::exception& error) {
            preview_csv << record_index << ',' << csv_text(labels) << ','
                        << record.width << ',' << record.height
                        << ",,,,," << csv_text(error.what()) << '\n';
            ++failed_records;
        }
    }

    if (profile.area_name_table != 0 && profile.area_name_count != 0) {
        write_named_level_candidates(
            rom, profile, level_index,
            output / "named_level_candidates.csv");
    } else {
        std::filesystem::remove(output / "named_level_candidates.csv");
    }
    write_level_gallery(rom, profile, level_index, output);

    std::ofstream level_note(output / "level_information.txt");
    level_note << game_family_name(game_family(rom)) << '\n'
               << "Map-entry table: " << hex(level_index.map_entry_table, 7)
               << "   Entries: " << level_index.entries.size() << '\n'
               << "Unique linked graphics records: "
               << level_index.records.size() << '\n'
               << "Global tile atlases: "
               << profile.level_tileset_count << " at "
               << hex(profile.level_tileset_table, 7) << "\n\n"
               << "DragonByteZ follows variation_array[0] to the graphics "
                  "record instead of assuming ROM order. map_entries.csv "
                  "preserves every raw entry field and pointer.\n"
               << "level_preview_index.csv indexes " << rendered_records
               << " rendered composite previews and " << failed_records
               << " special or unresolved graphics records.\n";
    if (profile.area_name_table != 0 && profile.area_name_count != 0) {
        level_note
            << "area_names.csv and named_level_candidates.csv preserve the "
               "localized-name evidence without inventing an exact runtime "
               "binding.\n";
    } else {
        level_note
            << "A game-specific localized area-name table has not yet been "
               "assigned. The gallery still exposes map labels, dimensions, "
               "pointers, music fields, and raw entry values.\n";
    }
    if (profile.default_bg_palette == 0) {
        level_note
            << "Map pixels currently use a diagnostic 256-colour palette. "
               "Tile IDs, flips, layer placement, local-tile rebuilding, and "
               "map geometry are decoded independently of that palette.\n";
    } else {
        level_note << "Background palette: "
                   << hex(profile.default_bg_palette, 7) << "\n";
    }

    std::ofstream note(output / "graphics_notes.txt");
    note << game_family_name(game_family(rom)) << " graphics findings\n"
         << "============================================\n\n"
         << "* " << hex(profile.level_tileset_table, 7) << " contains "
         << profile.level_tileset_count
         << " compressed GBA tile atlases. Complete tables may contain "
            "8 KiB 4bpp atlases or 16 KiB 8bpp atlases; 4bpp data is unpacked "
            "to 8-bit indices before rendering.\n"
         << "* The map-entry table contains " << profile.map_entry_count
         << " records at " << hex(level_index.map_entry_table, 7) << ".\n"
         << "* variation_array[0] supplies the linked graphics record.\n"
         << "* Map chunk entries use a 10-bit local tile ID plus horizontal "
            "and vertical flip bits.\n"
         << "* Delta-coded global-atlas selections rebuild each static local "
            "tileset. The first complete animated frame is placed at its "
            "recorded local/VRAM tile offset.\n"
         << "* level_previews/maps contains bounded composites for every "
            "renderable linked graphics record, and level_gallery.html is "
            "the searchable visual browser.\n";
    if (verified_log2) {
        note << "* LOG2 uses its verified recovered RGB555 background and "
                "object palettes, title art, character-display table, and "
                "localized area-name table.\n";
    } else if (buus_fury) {
        note << "* Buu's Fury uses its own recovered 256-colour background "
                "palette at " << hex(profile.default_bg_palette, 7)
             << " and object palette at "
             << hex(profile.default_obj_palette, 7) << ".\n";
    }

    SpriteExportSummary sprite_summary;
    if (verified_log2) {
        sprite_summary = export_log2_character_sprites(rom, output);
    } else if (buus_fury) {
        sprite_summary = export_buus_fury_character_sprites(rom, output);
    }
    if (!sprite_summary.description.empty()) {
        note << "* Character sprites: " << sprite_summary.description << ".\n";
    }
}

void analyze_log2_graphics(
    const Rom& rom,
    const std::filesystem::path& output) {
    require_log2_profile(rom);
    analyze_webfoot_graphics(rom, profile_for(rom), output);
}

void analyze_buus_fury_graphics(
    const Rom& rom,
    const std::filesystem::path& output) {
    if (!is_buus_fury_rom(rom)) {
        throw std::runtime_error(
            "this command requires Buu's Fury USA Rev 0 (BG3E)");
    }
    analyze_webfoot_graphics(rom, profile_for(rom), output);
}

void analyze_log2_soundtrack(const Rom& rom, const std::filesystem::path& output) {
    require_log2_profile(rom);
    const auto& profile = profile_for(rom);
    std::filesystem::remove_all(output / "instrument_samples");
    std::filesystem::remove_all(output / "sfx_samples");
    std::filesystem::create_directories(output);
    std::filesystem::create_directories(output / "instrument_samples");
    std::filesystem::create_directories(output / "sfx_samples");

    std::ofstream instruments(output / "instrument_samples.csv");
    instruments << "index,pointer,file_offset,flags,loop_start,raw_size,"
                   "sample_rate,wav\n";
    for (std::size_t index = 0;
         index < profile.music_sample_count; ++index) {
        const std::size_t record =
            profile.music_sample_table + index * 12;
        const std::uint32_t pointer = rom.u32(record);
        const std::uint8_t flags = rom.u8(record + 4);
        const std::uint8_t padding = rom.u8(record + 5);
        const std::uint16_t loop_start = rom.u16(record + 6);
        const std::uint16_t raw_size = rom.u16(record + 8);
        const std::uint16_t sample_rate = rom.u16(record + 10);
        if (!rom.is_rom_pointer(pointer) || padding != 0 || raw_size == 0 ||
            sample_rate < 3000 || sample_rate > 30000 ||
            loop_start > raw_size) {
            throw std::runtime_error(
                "invalid music sample record " +
                std::to_string(index));
        }
        const std::size_t file_offset = rom.pointer_to_offset(pointer);
        const auto pcm = rom.slice(file_offset, raw_size);
        const std::string filename =
            numbered_name("instrument_", index, "_" +
                std::to_string(sample_rate) + "hz.wav");
        write_wav(output / "instrument_samples" / filename, pcm, sample_rate);
        instruments << index << ',' << hex(pointer) << ','
                    << hex(file_offset, 7) << ','
                    << static_cast<unsigned>(flags) << ',' << loop_start << ','
                    << raw_size << ',' << sample_rate << ',' << filename
                    << '\n';
    }

    std::ofstream sfx(output / "sfx_samples.csv");
    sfx << "index,pointer,file_offset,loop_start,flags,raw_size,sample_rate,wav\n";
    for (std::size_t index = 0;
         index < profile.sfx_sample_count; ++index) {
        const std::size_t record =
            profile.sfx_sample_table + index * 12;
        const std::uint32_t pointer = rom.u32(record);
        const std::uint32_t loop_field = rom.u32(record + 4);
        const std::uint32_t packed = rom.u32(record + 8);
        const std::uint16_t loop_start =
            static_cast<std::uint16_t>(loop_field >> 16U);
        const std::uint16_t flags =
            static_cast<std::uint16_t>(loop_field & 0xFFFFU);
        const std::uint16_t raw_size =
            static_cast<std::uint16_t>(packed & 0xFFFFU);
        const std::uint16_t sample_rate =
            static_cast<std::uint16_t>(packed >> 16U);
        if (!rom.is_rom_pointer(pointer) || flags > 3 || raw_size == 0 ||
            loop_start > raw_size ||
            sample_rate < 3000 || sample_rate > 30000) {
            throw std::runtime_error(
                "invalid SFX sample record " +
                std::to_string(index));
        }
        const std::size_t file_offset = rom.pointer_to_offset(pointer);
        const auto pcm = rom.slice(file_offset, raw_size);
        const std::string filename =
            numbered_name(
                "sfx_", index, "_" + std::to_string(sample_rate) + "hz.wav");
        write_wav(output / "sfx_samples" / filename, pcm, sample_rate);
        sfx << index << ',' << hex(pointer) << ','
            << hex(file_offset, 7) << ',' << loop_start << ',' << flags << ','
            << raw_size << ',' << sample_rate << ',' << filename << '\n';
    }

    std::ofstream csv(output / "rejected_false_positive.csv");
    csv << "index,pointer,file_offset,kind,raw_size,packed_size,"
           "distinct_indices,classification,reason\n";

    for (std::size_t index = 0;
         index < profile.rejected_indexed_graphics_count; ++index) {
        const std::uint32_t pointer =
            rom.u32(
                profile.rejected_indexed_graphics_table + index * 4);
        const std::size_t offset = rom.pointer_to_offset(pointer);
        const auto decoded = decompress_container(rom, offset);
        std::array<bool, 256> seen{};
        unsigned distinct = 0;
        for (const std::uint8_t byte : decoded.data) {
            if (!seen[byte]) {
                seen[byte] = true;
                ++distinct;
            }
        }
        csv << index << ',' << hex(pointer) << ',' << hex(offset, 7) << ','
            << decoded.kind << ',' << decoded.data.size() << ','
            << decoded.packed_size() << ',' << distinct
            << ",64x64 indexed graphic,"
               "\"4096 index bytes with repeated row/border patterns; not "
               "signed PCM\"\n";
    }

    write_level_music(rom, profile, output / "level_music");

    std::ofstream research(output / "soundtrack_research.csv");
    research << "item,value,region,confidence,status,evidence\n"
             << "engine_family,custom Webfoot/non-Sappy," << profile.name
             << ",high,verified,"
                "\"Playback code does not use the standard Nintendo Sappy "
                "driver\"\n"
             << "instrument_sample_table,"
             << hex(profile.music_sample_table, 7) << ',' << profile.name
             << ",high,verified,"
                "\"101 packed 12-byte records with ROM pointer, flags, loop "
                "start, byte length and per-sample rate\"\n"
             << "sfx_sample_table," << hex(profile.sfx_sample_table, 7)
             << ',' << profile.name << ",high,verified,"
                "\"64 raw signed 8-bit PCM effects at their packed rates\"\n"
             << "bgm_table," << hex(profile.bgm_table, 7) << ','
             << profile.name << ",high,verified,"
                "\"44 custom Webfoot records rendered as cached WAV previews\"\n"
             << "expected_instrumentation,electric guitar-heavy score,"
             << profile.name << ','
             << "medium,search signature,"
                "\"Use guitar-like instrument banks and note/event streams as "
                "validation clues; do not assume raw PCM\"\n"
             << "current_song_runtime_lead,0x030026DC," << profile.name
             << ",medium,"
                "needs control-flow confirmation,"
                "\"Matches a published US runtime field and the corresponding "
                "EU global-state layout\"\n"
             << "audio_export,209 WAV files,"
             << profile.name << ",high,enabled,"
                "\"101 instrument samples, 64 SFX samples and 44 cached BGM WAV previews\"\n"
             << "rejected_table,"
             << hex(profile.rejected_indexed_graphics_table, 7) << ','
             << profile.name << ",high,reclassified,"
                "\"107 blocks of 4096 indexed pixels; see "
                "rejected_false_positive.csv\"\n";

    std::ofstream note(output / "soundtrack_notes.txt");
    note << "DragonByteZ soundtrack analysis\n"
         << "================================\n\n"
         << "* LOG2 uses Webfoot's custom music engine, not Nintendo Sappy.\n"
         << "* The score's Bruce Faulconer-influenced, guitar-heavy sound is a "
            "useful validation clue. The likely target is a sequenced custom "
            "engine with instrument data, not a folder of whole-song PCM "
            "recordings.\n"
         << "* The former PCM claim was wrong. The rejected table has "
            "107 compressed blocks of 4096 index bytes. Their 64x64 structure, "
            "repeated borders and low-valued indices identify indexed "
            "graphics, not signed audio samples.\n"
         << "* The verified music-sample table at "
         << hex(profile.music_sample_table, 7) << " contains "
            "101 records. Each provides the raw signed 8-bit PCM pointer, "
            "flags, loop start, byte length and sample rate. All 101 samples "
            "are exported as mono WAV files at their recorded rates.\n"
         << "* The verified SFX table at " << hex(profile.sfx_sample_table, 7)
         << " contains 64 raw "
            "signed 8-bit PCM effects. They are exported as mono WAV files at "
            "the packed 8000 or 16000 Hz rate.\n"
         << "* The unrelated indexed-graphics table remains "
            "rejected and is never turned into WAV data.\n"
         << "* The verified 44-record BGM table at "
         << hex(profile.bgm_table, 7)
         << " is rendered as cached twelve-second stereo WAV previews. "
            "No sequence BIN, GSF or miniGSF files are written.\n"
         << "* 0x030026DC is retained only as a medium-confidence runtime "
            "current-song lead. It still needs European control-flow proof.\n"
         << "* Region-specific BGM, sample and effect offsets are selected "
            "from the verified ALFP/ALFE ROM profile; one region's pointers "
            "are never silently reused for the other.\n";
}

void analyze_buus_fury_soundtrack(
    const Rom& rom,
    const std::filesystem::path& output) {
    if (!is_buus_fury_rom(rom)) {
        throw std::runtime_error(
            "this command requires Buu's Fury USA Rev 0 (BG3E)");
    }
    std::filesystem::create_directories(output);
    const auto& profile = profile_for(rom);
    write_level_music(rom, profile, output / "level_music");

    std::ofstream note(output / "BUUS_FURY_soundtrack_notes.txt");
    note << "DragonByteZ Buu's Fury soundtrack analysis\n"
         << "=========================================\n\n"
         << "Recovered BGM table: " << hex(profile.bgm_table, 7) << '\n'
         << "Track records: " << profile.bgm_count << '\n'
         << "Title-screen patch record: "
         << hex(profile.title_bgm_record, 7) << "\n\n"
         << "level_music contains all recovered BGM entries as distinct, "
            "cached twelve-second stereo WAV previews. The startup patch uses "
            "the verified title record at 0x003BBB10. No sequence BIN, GSF, "
            "miniGSF or candidate sample files are written.\n";
}

void analyze_buus_fury_all(
    const Rom& rom,
    const std::filesystem::path& output) {
    std::filesystem::create_directories(output);
    analyze_buus_fury_graphics(rom, output / "graphics");
    analyze_buus_fury_soundtrack(rom, output / "soundtrack");
    const auto& profile = profile_for(rom);
    const auto level_index = build_level_index(rom, profile);
    std::ofstream report(output / "DragonByteZ_report.txt");
    report << "DragonByteZ 0.6.23 Buu's Fury analysis report\n"
           << "===========================================\n\n"
           << "ROM title: " << rom.title() << '\n'
           << "Game code: " << rom.game_code() << '\n'
           << "Revision: " << static_cast<unsigned>(rom.revision()) << '\n'
           << "Size: " << rom.size() << " bytes\n\n"
           << "Recovered structures:\n"
           << "- " << profile.map_entry_count << " map entries at "
           << hex(profile.map_entry_table, 7) << '\n'
           << "- " << level_index.records.size()
           << " unique linked graphics records\n"
           << "- " << profile.level_tileset_count
           << " compressed 4bpp/8bpp global tile atlases at "
           << hex(profile.level_tileset_table, 7) << '\n'
           << "- searchable level gallery and bounded composite previews\n"
           << "- recovered background palette at "
           << hex(profile.default_bg_palette, 7) << " and object palette at "
           << hex(profile.default_obj_palette, 7) << '\n'
           << "- " << profile.bgm_count
           << " distinct cached Webfoot BGM WAV previews\n\n"
           << "Open research:\n"
           << "- assign localized area names to map entries\n"
           << "- identify character and sprite animation tables\n";
}

void analyze_log2_all(const Rom& rom, const std::filesystem::path& output) {
    require_log2_profile(rom);
    const auto& profile = profile_for(rom);
    std::filesystem::create_directories(output);
    analyze_log2_graphics(rom, output / "graphics");
    analyze_log2_soundtrack(rom, output / "soundtrack");
    std::ofstream report(output / "DragonByteZ_report.txt");
    report << "DragonByteZ 0.6.23 LOG2 analysis report\n"
           << "=================================\n\n"
           << "ROM title: " << rom.title() << '\n'
           << "Game code: " << rom.game_code() << '\n'
           << "Revision: " << static_cast<unsigned>(rom.revision()) << '\n'
           << "Size: " << rom.size() << " bytes\n\n"
           << "Completed extractors:\n"
           << "- Webfoot type 0/1/2 decompression\n"
           << "- title character art with verified RGB555 palette\n"
           << "- default BG/OBJ RGB555 palettes traced from DMA setup\n"
           << "- 168 individual 256-tile 8bpp level atlases using the "
              "region-correct 256-colour BG palette\n"
           << "- authoritative 327-entry MapEntry table with output, zone, "
              "entity counts, scripts, music IDs and graphics links\n"
           << "- " << find_level_records(rom, profile).size()
           << " unique map-linked graphics records with bounded previews\n"
           << "- complete animated tile frames placed at recorded VRAM slots\n"
           << "- 70 localized area names, including Haunted Swamp\n"
           << "- 22 character-display metadata records\n"
           << "- 101 music instrument WAV samples at recorded sample rates\n"
           << "- 64 sound-effect WAV samples at their recorded rates\n"
           << "- 44 distinct cached level-music WAV previews; no BIN, GSF or miniGSF output\n"
           << "- false 0x006A981C PCM table rejected as indexed graphics\n\n"
           << "Open research:\n"
           << "- confirm layer priority and scene-specific palette effects\n"
           << "- assemble individual overworld character animation frames\n"
           << "- assign descriptive names to the 44 verified BGM records\n";
}

std::string level_summary(const Rom& rom) {
    if (is_log2_rom(rom)) return log2_level_summary(rom);
    if (is_log1_rom(rom)) return log1_level_summary(rom);
    if (is_buus_fury_rom(rom)) {
        const auto& profile = profile_for(rom);
        const auto index = build_level_index(rom, profile);
        std::ostringstream summary;
        summary << "Buu's Fury USA Rev 0 (BG3E) detected.\n"
                << "Map-entry table: " << hex(index.map_entry_table, 7)
                << "   Entries: " << index.entries.size() << '\n'
                << "Unique linked graphics records: "
                << index.records.size()
                << "   4bpp/8bpp tile atlases: "
                << profile.level_tileset_count << '\n'
                << "Webfoot BGM records: " << profile.bgm_count
                << " at " << hex(profile.bgm_table, 7) << ".";
        return summary.str();
    }
    throw std::runtime_error(
        "supported ROMs are LOG1 ALGP, LOG2 ALFP/ALFE, and "
        "Buu's Fury BG3E");
}

void analyze_graphics(
    const Rom& rom,
    const std::filesystem::path& output) {
    if (is_log2_rom(rom)) {
        analyze_log2_graphics(rom, output);
        return;
    }
    if (is_log1_rom(rom)) {
        analyze_log1_graphics(rom, output);
        return;
    }
    if (is_buus_fury_rom(rom)) {
        analyze_buus_fury_graphics(rom, output);
        return;
    }
    throw std::runtime_error(
        "supported ROMs are LOG1 ALGP, LOG2 ALFP/ALFE, and "
        "Buu's Fury BG3E");
}

void analyze_soundtrack(
    const Rom& rom,
    const std::filesystem::path& output) {
    if (is_log2_rom(rom)) {
        analyze_log2_soundtrack(rom, output);
        return;
    }
    if (is_log1_rom(rom)) {
        analyze_log1_soundtrack(rom, output);
        return;
    }
    if (is_buus_fury_rom(rom)) {
        analyze_buus_fury_soundtrack(rom, output);
        return;
    }
    throw std::runtime_error(
        "supported ROMs are LOG1 ALGP, LOG2 ALFP/ALFE, and "
        "Buu's Fury BG3E");
}

void analyze_all(const Rom& rom, const std::filesystem::path& output) {
    if (is_log2_rom(rom)) {
        analyze_log2_all(rom, output);
        return;
    }
    if (is_log1_rom(rom)) {
        analyze_log1_all(rom, output);
        return;
    }
    if (is_buus_fury_rom(rom)) {
        analyze_buus_fury_all(rom, output);
        return;
    }
    throw std::runtime_error(
        "supported ROMs are LOG1 ALGP, LOG2 ALFP/ALFE, and "
        "Buu's Fury BG3E");
}

} // namespace dragonbytez
