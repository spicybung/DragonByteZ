#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dragonbytez {

class Rom {
public:
    explicit Rom(const std::filesystem::path& path);

    const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
    std::size_t size() const noexcept { return bytes_.size(); }
    std::uint8_t u8(std::size_t offset) const;
    std::uint16_t u16(std::size_t offset) const;
    std::uint32_t u32(std::size_t offset) const;
    std::vector<std::uint8_t> slice(std::size_t offset, std::size_t count) const;
    bool is_rom_pointer(std::uint32_t value) const noexcept;
    std::size_t pointer_to_offset(std::uint32_t value) const;

    std::string title() const;
    std::string game_code() const;
    std::uint8_t revision() const;

private:
    std::vector<std::uint8_t> bytes_;
};

enum class GameFamily {
    unknown,
    legacy_of_goku,
    legacy_of_goku_ii,
    buus_fury
};

struct GameProfile {
    const char* name = "";
    const char* game_code = "";
    std::size_t expected_size = 0;
    std::size_t map_entry_table = 0;
    std::size_t map_entry_count = 0;
    std::size_t level_tileset_table = 0;
    std::size_t level_tileset_count = 0;
    std::size_t area_name_table = 0;
    std::size_t area_name_count = 0;
    std::size_t area_name_languages = 0;
    std::size_t level_record_tileset_pointer_offset = 0;
    std::size_t character_display_table = 0;
    std::size_t character_display_count = 0;
    std::size_t title_image = 0;
    std::size_t title_palette = 0;
    std::size_t default_bg_palette = 0;
    std::size_t default_obj_palette = 0;
    std::size_t rejected_indexed_graphics_table = 0;
    std::size_t rejected_indexed_graphics_count = 0;
    std::size_t music_sample_table = 0;
    std::size_t music_sample_count = 0;
    std::size_t sfx_sample_table = 0;
    std::size_t sfx_sample_count = 0;
    std::size_t bgm_table = 0;
    std::size_t bgm_count = 0;
    std::size_t title_bgm_record = 0;
};

bool matches_europe_rev0(const Rom& rom);
bool matches_usa_rev0(const Rom& rom);
bool matches_log1_europe_rev0(const Rom& rom);
bool matches_buus_fury_usa_rev0(const Rom& rom);
bool is_log1_rom(const Rom& rom);
bool is_log2_rom(const Rom& rom);
bool is_buus_fury_rom(const Rom& rom);
bool is_supported_rom(const Rom& rom);
GameFamily game_family(const Rom& rom);
const char* game_family_name(GameFamily family) noexcept;
const GameProfile& profile_for(const Rom& rom);
GameProfile music_profile_for(const Rom& rom);

} // namespace dragonbytez
