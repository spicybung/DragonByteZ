#include "dragonbytez/rom.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace dragonbytez {

namespace {
void require_range(std::size_t size, std::size_t offset, std::size_t count) {
    if (offset > size || count > size - offset) {
        throw std::out_of_range("ROM read is outside the file");
    }
}

const GameProfile europe_rev0 = [] {
    GameProfile profile;
    profile.name = "Europe Rev 0";
    profile.game_code = "ALFP";
    profile.expected_size = 8 * 1024 * 1024;
    profile.map_entry_count = 327;
    profile.level_tileset_table = 0x00124308;
    profile.level_tileset_count = 168;
    profile.area_name_table = 0x006C9F1C;
    profile.area_name_count = 70;
    profile.area_name_languages = 5;
    profile.level_record_tileset_pointer_offset = 0x54;
    profile.character_display_table = 0x0002A4E8;
    profile.character_display_count = 22;
    profile.title_image = 0x0069C570;
    profile.title_palette = 0x006A85F8;
    profile.default_bg_palette = 0x006A83F8;
    profile.default_obj_palette = 0x006A87F8;
    profile.rejected_indexed_graphics_table = 0x006A981C;
    profile.rejected_indexed_graphics_count = 107;
    profile.music_sample_table = 0x0037A524;
    profile.music_sample_count = 101;
    profile.sfx_sample_table = 0x004B8658;
    profile.sfx_sample_count = 64;
    profile.bgm_table = 0x004047AC;
    profile.bgm_count = 44;
    profile.title_bgm_record = 0x004048D8;
    return profile;
}();

const GameProfile usa_rev0 = [] {
    GameProfile profile;
    profile.name = "USA Rev 0";
    profile.game_code = "ALFE";
    profile.expected_size = 8 * 1024 * 1024;
    profile.map_entry_count = 327;
    profile.level_tileset_table = 0x004DF574;
    profile.level_tileset_count = 168;
    profile.area_name_table = 0x000E2B38;
    profile.area_name_count = 70;
    profile.area_name_languages = 1;
    profile.level_record_tileset_pointer_offset = 0x54;
    profile.character_display_table = 0x001D967C;
    profile.character_display_count = 22;
    profile.title_image = 0x00029294;
    profile.title_palette = 0x001D4F50;
    profile.default_bg_palette = 0x001DA4C8;
    profile.default_obj_palette = 0x001DA6C8;
    profile.rejected_indexed_graphics_table = 0x003EC9F4;
    profile.rejected_indexed_graphics_count = 107;
    profile.music_sample_table = 0x0014A8A4;
    profile.music_sample_count = 101;
    profile.sfx_sample_table = 0x000E15B0;
    profile.sfx_sample_count = 64;
    profile.bgm_table = 0x001D4B2C;
    profile.bgm_count = 44;
    profile.title_bgm_record = 0x001D4C58;
    return profile;
}();

const GameProfile buus_fury_usa_rev0 = [] {
    GameProfile profile;
    profile.name = "USA Rev 0";
    profile.game_code = "BG3E";
    profile.expected_size = 8 * 1024 * 1024;
    profile.map_entry_table = 0x0008E2E0;
    profile.map_entry_count = 452;
    profile.level_tileset_table = 0x00179764;
    profile.level_tileset_count = 187;
    profile.level_record_tileset_pointer_offset = 0x4C;
    profile.default_bg_palette = 0x0005632C;
    profile.default_obj_palette = 0x0005652C;
    profile.bgm_table = 0x003BB78C;
    profile.bgm_count = 53;
    profile.title_bgm_record = 0x003BBB10;
    return profile;
}();
}

Rom::Rom(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open ROM: " + path.string());
    }
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    if (length < 0) {
        throw std::runtime_error("cannot determine ROM size");
    }
    input.seekg(0, std::ios::beg);
    bytes_.resize(static_cast<std::size_t>(length));
    input.read(reinterpret_cast<char*>(bytes_.data()), length);
    if (!input) {
        throw std::runtime_error("cannot read complete ROM");
    }
}

std::uint8_t Rom::u8(std::size_t offset) const {
    require_range(size(), offset, 1);
    return bytes_[offset];
}

std::uint16_t Rom::u16(std::size_t offset) const {
    require_range(size(), offset, 2);
    return static_cast<std::uint16_t>(bytes_[offset]) |
           (static_cast<std::uint16_t>(bytes_[offset + 1]) << 8);
}

std::uint32_t Rom::u32(std::size_t offset) const {
    require_range(size(), offset, 4);
    return static_cast<std::uint32_t>(bytes_[offset]) |
           (static_cast<std::uint32_t>(bytes_[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes_[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes_[offset + 3]) << 24);
}

std::vector<std::uint8_t> Rom::slice(std::size_t offset, std::size_t count) const {
    require_range(size(), offset, count);
    return {bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset + count)};
}

bool Rom::is_rom_pointer(std::uint32_t value) const noexcept {
    return value >= 0x08000000U &&
           static_cast<std::uint64_t>(value - 0x08000000U) < size();
}

std::size_t Rom::pointer_to_offset(std::uint32_t value) const {
    if (!is_rom_pointer(value)) {
        throw std::out_of_range("value is not a pointer into this ROM");
    }
    return value - 0x08000000U;
}

std::string Rom::title() const {
    require_range(size(), 0xA0, 12);
    std::string value(bytes_.begin() + 0xA0, bytes_.begin() + 0xAC);
    value.erase(std::find(value.begin(), value.end(), '\0'), value.end());
    while (!value.empty() && value.back() == ' ') value.pop_back();
    return value;
}

std::string Rom::game_code() const {
    require_range(size(), 0xAC, 4);
    return std::string(bytes_.begin() + 0xAC, bytes_.begin() + 0xB0);
}

std::uint8_t Rom::revision() const {
    return u8(0xBC);
}

bool matches_europe_rev0(const Rom& rom) {
    return rom.size() == europe_rev0.expected_size &&
           rom.game_code() == europe_rev0.game_code &&
           rom.revision() == 0;
}

bool matches_usa_rev0(const Rom& rom) {
    return rom.size() == usa_rev0.expected_size &&
           rom.game_code() == usa_rev0.game_code &&
           rom.revision() == 0;
}

bool matches_log1_europe_rev0(const Rom& rom) {
    return rom.size() == 8 * 1024 * 1024 &&
           rom.game_code() == "ALGP" &&
           rom.revision() == 0;
}

bool matches_buus_fury_usa_rev0(const Rom& rom) {
    return rom.size() == buus_fury_usa_rev0.expected_size &&
           rom.game_code() == buus_fury_usa_rev0.game_code &&
           rom.revision() == 0;
}

bool is_log1_rom(const Rom& rom) {
    return matches_log1_europe_rev0(rom);
}

bool is_log2_rom(const Rom& rom) {
    return matches_europe_rev0(rom) || matches_usa_rev0(rom);
}

bool is_buus_fury_rom(const Rom& rom) {
    return matches_buus_fury_usa_rev0(rom);
}

bool is_supported_rom(const Rom& rom) {
    return is_log1_rom(rom) || is_log2_rom(rom) || is_buus_fury_rom(rom);
}

GameFamily game_family(const Rom& rom) {
    if (is_log1_rom(rom)) return GameFamily::legacy_of_goku;
    if (is_log2_rom(rom)) return GameFamily::legacy_of_goku_ii;
    if (is_buus_fury_rom(rom)) return GameFamily::buus_fury;
    return GameFamily::unknown;
}

const char* game_family_name(GameFamily family) noexcept {
    switch (family) {
    case GameFamily::legacy_of_goku:
        return "Dragon Ball Z: The Legacy of Goku";
    case GameFamily::legacy_of_goku_ii:
        return "Dragon Ball Z: The Legacy of Goku II";
    case GameFamily::buus_fury:
        return "Dragon Ball Z: Buu's Fury";
    case GameFamily::unknown:
    default:
        return "Unknown";
    }
}

const GameProfile& profile_for(const Rom& rom) {
    if (matches_europe_rev0(rom)) return europe_rev0;
    if (matches_usa_rev0(rom)) return usa_rev0;
    if (matches_buus_fury_usa_rev0(rom)) return buus_fury_usa_rev0;
    throw std::runtime_error(
        "this command requires LOG2 ALFP/ALFE or Buu's Fury BG3E");
}

GameProfile music_profile_for(const Rom& rom) {
    if (is_log2_rom(rom) || is_buus_fury_rom(rom)) {
        return profile_for(rom);
    }
    if (!is_log1_rom(rom)) {
        throw std::runtime_error(
            "no Webfoot music profile is available for this ROM");
    }

    const auto valid_record = [&rom](std::size_t offset) {
        if (offset + 20 > rom.size()) return false;
        const std::uint32_t song_end = rom.u32(offset);
        const std::uint32_t song_header = rom.u32(offset + 4);
        const std::uint32_t instrument_table = rom.u32(offset + 8);
        const std::uint32_t shared = rom.u32(offset + 12);
        if (!rom.is_rom_pointer(song_end) ||
            !rom.is_rom_pointer(song_header) ||
            !rom.is_rom_pointer(instrument_table) ||
            !rom.is_rom_pointer(shared) ||
            song_end <= song_header) {
            return false;
        }
        const std::uint32_t sequence_size = song_end - song_header;
        return sequence_size >= 8 && sequence_size <= 0x20000U;
    };

    std::size_t best_offset = 0;
    std::size_t best_count = 0;
    for (std::size_t phase = 0; phase < 5; ++phase) {
        std::size_t run_offset = 0;
        std::size_t run_count = 0;
        for (std::size_t offset = phase * 4;
             offset + 20 <= rom.size(); offset += 20) {
            if (valid_record(offset)) {
                if (run_count == 0) run_offset = offset;
                ++run_count;
            } else {
                if (run_count > best_count) {
                    best_offset = run_offset;
                    best_count = run_count;
                }
                run_count = 0;
            }
        }
        if (run_count > best_count) {
            best_offset = run_offset;
            best_count = run_count;
        }
    }
    if (best_count < 4) {
        throw std::runtime_error(
            "could not locate a contiguous Webfoot music table");
    }

    GameProfile profile;
    profile.name = "Europe Rev 0 experimental music scan";
    profile.game_code = "ALGP";
    profile.expected_size = rom.size();
    profile.bgm_table = best_offset;
    profile.bgm_count = best_count;
    const std::size_t title_index = std::min<std::size_t>(15, best_count - 1);
    profile.title_bgm_record = best_offset + title_index * 20;
    return profile;
}

} // namespace dragonbytez
