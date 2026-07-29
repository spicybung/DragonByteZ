#include "dragonbytez/log1_runtime.hpp"

#include "dragonbytez/gsf_player.hpp"
#include "dragonbytez/png.hpp"
#include "dragonbytez/rom.hpp"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "vbam/gba/GBA.h"
#include "vbam/gba/Sound.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace dragonbytez {

namespace {

constexpr unsigned screen_width = 240;
constexpr unsigned screen_height = 160;
constexpr std::uint32_t audio_sample_rate = 44100;
constexpr std::size_t log1_map_count = 42;
constexpr std::size_t log1_sprite_count = 109;
constexpr std::size_t log1_music_count = 18;

constexpr std::uint16_t key_a = 1U << 0U;
constexpr std::uint16_t key_b = 1U << 1U;
constexpr std::uint16_t key_start = 1U << 3U;
constexpr std::uint16_t key_right = 1U << 4U;
constexpr std::uint16_t key_up = 1U << 6U;
constexpr std::uint16_t key_down = 1U << 7U;
constexpr std::uint16_t key_r = 1U << 8U;
constexpr std::uint16_t key_l = 1U << 9U;

class RuntimeAudioOutput final : public GBASoundOut {
public:
    void write(const void* samples, unsigned long bytes) override {
        const auto* source = static_cast<const std::int16_t*>(samples);
        samples_.insert(
            samples_.end(),
            source,
            source + bytes / sizeof(std::int16_t));
    }

    void clear() {
        samples_.clear();
    }

    const std::vector<std::int16_t>& samples() const noexcept {
        return samples_;
    }

private:
    std::vector<std::int16_t> samples_;
};

class Log1RuntimeSession {
public:
    explicit Log1RuntimeSession(const Rom& rom) {
        system_.cpuIsMultiBoot = false;
        system_.soundSampleRate = audio_sample_rate;
        system_.soundDeclicking = true;
        system_.soundInterpolation = true;
        if (CPULoadRom(
                &system_,
                rom.bytes().data(),
                static_cast<std::uint32_t>(rom.size())) == 0) {
            throw std::runtime_error("embedded GBA runtime could not load LOG1");
        }
        loaded_ = true;
        try {
            if (!soundInit(&system_, &audio_)) {
                throw std::runtime_error("embedded GBA runtime could not initialize audio");
            }
            soundReset(&system_);
            CPUInit(&system_);
            CPUReset(&system_);
            soundResume(&system_);
        } catch (...) {
            CPUCleanUp(&system_);
            loaded_ = false;
            throw;
        }
    }

    ~Log1RuntimeSession() {
        if (loaded_) CPUCleanUp(&system_);
    }

    Log1RuntimeSession(const Log1RuntimeSession&) = delete;
    Log1RuntimeSession& operator=(const Log1RuntimeSession&) = delete;

    void run_frames(unsigned count, std::uint16_t keys = 0) {
        for (unsigned frame = 0; frame < count; ++frame) {
            set_keys(keys);
            const int target = system_.frameCount + 1;
            unsigned stalled_iterations = 0;
            int previous_frame_count = system_.frameCount;
            while (system_.frameCount < target) {
                CPULoop(&system_, 200000);
                if (system_.frameCount == previous_frame_count) {
                    ++stalled_iterations;
                    if (stalled_iterations >= 2000U) {
                        throw std::runtime_error(
                            "the LOG1 runtime stopped advancing frames");
                    }
                } else {
                    previous_frame_count = system_.frameCount;
                    stalled_iterations = 0;
                }
            }
        }
    }

    void run_until(unsigned absolute_frame) {
        if (system_.frameCount >= static_cast<int>(absolute_frame)) return;
        run_frames(absolute_frame - static_cast<unsigned>(system_.frameCount));
    }

    void press(
        std::uint16_t keys,
        unsigned held_frames = 6,
        unsigned released_frames = 10) {
        run_frames(held_frames, keys);
        run_frames(released_frames, 0);
    }

    void boot_to_debug_menu() {
        run_until(320);
        run_frames(6, key_a);
        run_until(3400);
        run_frames(100, key_start);
        run_until(3650);
        run_frames(10, key_down);
        run_until(3680);
        run_frames(10, key_down);
        run_until(3710);
        run_frames(10, key_a);
        run_until(3860);
        run_frames(20, key_l | key_r | key_up | key_a);
        run_until(3920);
    }

    GBASystem& system() noexcept {
        return system_;
    }

    RuntimeAudioOutput& audio() noexcept {
        return audio_;
    }

private:
    void set_keys(std::uint16_t pressed) {
        system_.P1 = static_cast<std::uint16_t>(0x03FFU & ~pressed);
        system_.ioMem[0x130] = static_cast<std::uint8_t>(system_.P1);
        system_.ioMem[0x131] = static_cast<std::uint8_t>(system_.P1 >> 8U);
    }

    GBASystem system_;
    RuntimeAudioOutput audio_;
    bool loaded_ = false;
};

std::uint16_t read_u16(const std::uint8_t* bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(bytes[offset + 1]) << 8U;
}

Rgba decode_rgb555(std::uint16_t value) {
    const auto expand = [](unsigned part) {
        return static_cast<std::uint8_t>((part << 3U) | (part >> 2U));
    };
    return {
        expand(value & 31U),
        expand((value >> 5U) & 31U),
        expand((value >> 10U) & 31U),
        255};
}

struct CompositedPixel {
    Rgba colour{0, 0, 0, 255};
    int priority = 99;
    int order = 99;
};

void place_pixel(
    std::vector<CompositedPixel>& pixels,
    unsigned x,
    unsigned y,
    const Rgba& colour,
    int priority,
    int order) {
    if (x >= screen_width || y >= screen_height) return;
    auto& destination = pixels[static_cast<std::size_t>(y) * screen_width + x];
    if (priority < destination.priority ||
        (priority == destination.priority && order < destination.order)) {
        destination.colour = colour;
        destination.priority = priority;
        destination.order = order;
    }
}

void render_objects(
    const GBASystem& system,
    const std::array<Rgba, 512>& palette,
    std::vector<CompositedPixel>& pixels) {
    const bool one_dimensional = (system.DISPCNT & 0x40U) != 0;
    const std::array<std::array<std::array<unsigned, 2>, 4>, 3> dimensions = {{
        {{{8, 8}, {16, 16}, {32, 32}, {64, 64}}},
        {{{16, 8}, {32, 8}, {32, 16}, {64, 32}}},
        {{{8, 16}, {8, 32}, {16, 32}, {32, 64}}}
    }};

    for (int object = 127; object >= 0; --object) {
        const std::size_t oam_offset = static_cast<std::size_t>(object) * 8U;
        const std::uint16_t attribute0 = read_u16(system.oam, oam_offset);
        const std::uint16_t attribute1 = read_u16(system.oam, oam_offset + 2U);
        const std::uint16_t attribute2 = read_u16(system.oam, oam_offset + 4U);

        const bool affine = (attribute0 & 0x0100U) != 0;
        const bool double_size = affine && (attribute0 & 0x0200U) != 0;
        if (!affine && (attribute0 & 0x0200U) != 0) continue;
        const unsigned object_mode = (attribute0 >> 10U) & 3U;
        if (object_mode == 2U) continue;
        const bool colour_256 = (attribute0 & 0x2000U) != 0;
        const unsigned shape = (attribute0 >> 14U) & 3U;
        if (shape >= 3U) continue;
        const unsigned size = (attribute1 >> 14U) & 3U;
        const unsigned width = dimensions[shape][size][0];
        const unsigned height = dimensions[shape][size][1];

        int object_x = attribute1 & 0x1FFU;
        int object_y = attribute0 & 0xFFU;
        if (object_x >= 240) object_x -= 512;
        if (object_y >= 160) object_y -= 256;
        if (double_size) {
            object_x += static_cast<int>(width) / 2;
            object_y += static_cast<int>(height) / 2;
        }

        const bool horizontal_flip = !affine && (attribute1 & 0x1000U) != 0;
        const bool vertical_flip = !affine && (attribute1 & 0x2000U) != 0;
        const unsigned first_tile = attribute2 & 0x3FFU;
        const int priority = (attribute2 >> 10U) & 3U;
        const unsigned bank = (attribute2 >> 12U) & 15U;

        for (unsigned destination_y = 0; destination_y < height; ++destination_y) {
            const int screen_y = object_y + static_cast<int>(destination_y);
            if (screen_y < 0 || screen_y >= static_cast<int>(screen_height)) continue;
            const unsigned source_y = vertical_flip
                ? height - 1U - destination_y
                : destination_y;
            for (unsigned destination_x = 0; destination_x < width; ++destination_x) {
                const int screen_x = object_x + static_cast<int>(destination_x);
                if (screen_x < 0 || screen_x >= static_cast<int>(screen_width)) continue;
                const unsigned source_x = horizontal_flip
                    ? width - 1U - destination_x
                    : destination_x;
                const unsigned tile_x = source_x / 8U;
                const unsigned tile_y = source_y / 8U;
                unsigned tile = first_tile;
                if (one_dimensional) {
                    const unsigned stride = width / 8U * (colour_256 ? 2U : 1U);
                    tile += tile_y * stride + tile_x * (colour_256 ? 2U : 1U);
                } else {
                    tile += tile_y * 32U + tile_x * (colour_256 ? 2U : 1U);
                }

                const unsigned pixel_x = source_x & 7U;
                const unsigned pixel_y = source_y & 7U;
                unsigned colour_index = 0;
                if (colour_256) {
                    const std::size_t offset = 0x10000U + tile * 32U +
                        pixel_y * 8U + pixel_x;
                    if (offset >= 0x20000U) continue;
                    colour_index = system.vram[offset];
                    if (colour_index == 0) continue;
                    colour_index += 256U;
                } else {
                    const std::size_t offset = 0x10000U + tile * 32U +
                        pixel_y * 4U + pixel_x / 2U;
                    if (offset >= 0x20000U) continue;
                    const std::uint8_t packed = system.vram[offset];
                    colour_index = pixel_x & 1U ? packed >> 4U : packed & 15U;
                    if (colour_index == 0) continue;
                    colour_index += 256U + bank * 16U;
                }
                place_pixel(
                    pixels,
                    static_cast<unsigned>(screen_x),
                    static_cast<unsigned>(screen_y),
                    palette[colour_index],
                    priority,
                    -1);
            }
        }
    }
}


struct RuntimeSpriteFrame {
    unsigned width = 0;
    unsigned height = 0;
    std::vector<Rgba> pixels;

    bool empty() const noexcept {
        return width == 0U || height == 0U || pixels.empty();
    }
};

std::vector<Rgba> render_objects_only(const GBASystem& system) {
    std::array<Rgba, 512> palette{};
    for (std::size_t index = 0; index < palette.size(); ++index) {
        palette[index] = decode_rgb555(read_u16(system.paletteRAM, index * 2U));
    }

    std::vector<CompositedPixel> composited(
        static_cast<std::size_t>(screen_width) * screen_height);
    for (CompositedPixel& pixel : composited) {
        pixel.colour = {0, 0, 0, 0};
        pixel.priority = 99;
        pixel.order = 99;
    }
    if ((system.DISPCNT & 0x1000U) != 0U) {
        render_objects(system, palette, composited);
    }

    std::vector<Rgba> output(composited.size());
    std::transform(
        composited.begin(),
        composited.end(),
        output.begin(),
        [](const CompositedPixel& pixel) { return pixel.colour; });
    return output;
}

RuntimeSpriteFrame crop_sprite_preview(const std::vector<Rgba>& screen) {
    constexpr unsigned preview_left = 154U;
    constexpr unsigned preview_top = 48U;
    constexpr unsigned preview_right = screen_width;
    constexpr unsigned preview_bottom = screen_height;

    unsigned minimum_x = preview_right;
    unsigned minimum_y = preview_bottom;
    unsigned maximum_x = preview_left;
    unsigned maximum_y = preview_top;
    bool found = false;
    for (unsigned y = preview_top; y < preview_bottom; ++y) {
        for (unsigned x = preview_left; x < preview_right; ++x) {
            const Rgba& pixel = screen[static_cast<std::size_t>(y) * screen_width + x];
            if (pixel.a == 0U) continue;
            minimum_x = std::min(minimum_x, x);
            minimum_y = std::min(minimum_y, y);
            maximum_x = std::max(maximum_x, x);
            maximum_y = std::max(maximum_y, y);
            found = true;
        }
    }
    if (!found) return {};

    RuntimeSpriteFrame frame;
    frame.width = maximum_x - minimum_x + 1U;
    frame.height = maximum_y - minimum_y + 1U;
    frame.pixels.assign(
        static_cast<std::size_t>(frame.width) * frame.height,
        Rgba{0, 0, 0, 0});
    for (unsigned y = 0; y < frame.height; ++y) {
        for (unsigned x = 0; x < frame.width; ++x) {
            frame.pixels[static_cast<std::size_t>(y) * frame.width + x] =
                screen[static_cast<std::size_t>(minimum_y + y) * screen_width +
                       minimum_x + x];
        }
    }
    return frame;
}

std::string runtime_frame_signature(const RuntimeSpriteFrame& frame) {
    std::string signature;
    signature.reserve(8U + frame.pixels.size() * 4U);
    const auto append_u32 = [&](std::uint32_t value) {
        signature.push_back(static_cast<char>(value));
        signature.push_back(static_cast<char>(value >> 8U));
        signature.push_back(static_cast<char>(value >> 16U));
        signature.push_back(static_cast<char>(value >> 24U));
    };
    append_u32(frame.width);
    append_u32(frame.height);
    for (const Rgba& pixel : frame.pixels) {
        signature.push_back(static_cast<char>(pixel.r));
        signature.push_back(static_cast<char>(pixel.g));
        signature.push_back(static_cast<char>(pixel.b));
        signature.push_back(static_cast<char>(pixel.a));
    }
    return signature;
}

RuntimeSpriteFrame make_runtime_sprite_sheet(
    const std::vector<RuntimeSpriteFrame>& frames) {
    RuntimeSpriteFrame sheet;
    if (frames.empty()) return sheet;
    unsigned maximum_width = 1U;
    unsigned maximum_height = 1U;
    for (const RuntimeSpriteFrame& frame : frames) {
        maximum_width = std::max(maximum_width, frame.width);
        maximum_height = std::max(maximum_height, frame.height);
    }
    constexpr unsigned padding = 6U;
    const unsigned columns = std::min<unsigned>(
        8U,
        std::max(1U, static_cast<unsigned>(std::ceil(std::sqrt(
            static_cast<double>(frames.size()))))));
    const unsigned rows = static_cast<unsigned>(
        (frames.size() + columns - 1U) / columns);
    const unsigned cell_width = maximum_width + padding;
    const unsigned cell_height = maximum_height + padding;
    sheet.width = columns * cell_width;
    sheet.height = rows * cell_height;
    sheet.pixels.assign(
        static_cast<std::size_t>(sheet.width) * sheet.height,
        Rgba{0, 0, 0, 0});

    for (std::size_t index = 0; index < frames.size(); ++index) {
        const RuntimeSpriteFrame& frame = frames[index];
        const unsigned column = static_cast<unsigned>(index) % columns;
        const unsigned row = static_cast<unsigned>(index) / columns;
        const unsigned destination_x = column * cell_width +
            (maximum_width - frame.width) / 2U;
        const unsigned destination_y = row * cell_height +
            (maximum_height - frame.height) / 2U;
        for (unsigned y = 0; y < frame.height; ++y) {
            for (unsigned x = 0; x < frame.width; ++x) {
                const Rgba& source =
                    frame.pixels[static_cast<std::size_t>(y) * frame.width + x];
                if (source.a == 0U) continue;
                sheet.pixels[
                    static_cast<std::size_t>(destination_y + y) * sheet.width +
                    destination_x + x] = source;
            }
        }
    }
    return sheet;
}

std::string numbered_filename(
    const std::string& prefix,
    std::size_t index,
    const std::string& extension,
    unsigned digits = 3) {
    std::ostringstream name;
    name << prefix << std::setw(static_cast<int>(digits)) << std::setfill('0')
         << index << extension;
    return name.str();
}

void write_gallery(
    const std::filesystem::path& output,
    const std::string& title,
    const std::string& folder,
    const std::string& prefix,
    std::size_t count,
    unsigned digits,
    std::size_t first_value = 0) {
    std::ofstream html(output / (folder + "_gallery.html"));
    html << "<!doctype html><meta charset=\"utf-8\"><title>" << title
         << "</title><style>body{background:#171b20;color:#eef3f8;font:14px Segoe UI,Arial;margin:0}"
            "header{position:sticky;top:0;background:#11151a;padding:16px;border-bottom:3px solid #f47d1f;z-index:2}"
            "main{display:grid;grid-template-columns:repeat(auto-fill,minmax(260px,1fr));gap:14px;padding:16px}"
            "article{background:#272d34;border:1px solid #59636e;border-radius:12px;padding:10px}"
            "img{display:block;width:240px;max-width:100%;height:auto;margin:auto;image-rendering:pixelated;background:#0c0f12}"
            "b{display:block;margin-bottom:8px}</style><header><h1>"
         << title << "</h1><p>Transparent OBJ/OAM animation frames only. Menu backgrounds, borders and character artwork are excluded.</p></header><main>";
    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t value = index + first_value;
        const std::string filename = numbered_filename(prefix, value, ".png", digits);
        html << "<article><b>" << value << "</b><a href=\"" << folder << '/'
             << filename << "\"><img loading=\"lazy\" src=\"" << folder << '/'
             << filename << "\"></a></article>";
    }
    html << "</main>";
}

void write_u16(std::ofstream& output, std::uint16_t value) {
    const char bytes[] = {
        static_cast<char>(value),
        static_cast<char>(value >> 8U)};
    output.write(bytes, 2);
}

void write_u32(std::ofstream& output, std::uint32_t value) {
    const char bytes[] = {
        static_cast<char>(value),
        static_cast<char>(value >> 8U),
        static_cast<char>(value >> 16U),
        static_cast<char>(value >> 24U)};
    output.write(bytes, 4);
}

void write_stereo_wav(
    const std::filesystem::path& path,
    std::vector<std::int16_t> samples) {
    const std::size_t frame_count = samples.size() / 2U;
    const std::size_t fade_in = std::min<std::size_t>(audio_sample_rate / 50U, frame_count);
    const std::size_t fade_out = std::min<std::size_t>(audio_sample_rate / 2U, frame_count);
    for (std::size_t frame = 0; frame < fade_in; ++frame) {
        const double gain = static_cast<double>(frame) / std::max<std::size_t>(1, fade_in);
        samples[frame * 2U] = static_cast<std::int16_t>(std::lround(samples[frame * 2U] * gain));
        samples[frame * 2U + 1U] = static_cast<std::int16_t>(std::lround(samples[frame * 2U + 1U] * gain));
    }
    for (std::size_t frame = 0; frame < fade_out; ++frame) {
        const double gain = static_cast<double>(fade_out - frame - 1U) /
            std::max<std::size_t>(1, fade_out);
        const std::size_t destination = frame_count - fade_out + frame;
        samples[destination * 2U] = static_cast<std::int16_t>(
            std::lround(samples[destination * 2U] * gain));
        samples[destination * 2U + 1U] = static_cast<std::int16_t>(
            std::lround(samples[destination * 2U + 1U] * gain));
    }

    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("cannot create LOG1 track WAV");
    const std::uint32_t data_size = static_cast<std::uint32_t>(
        samples.size() * sizeof(std::int16_t));
    output.write("RIFF", 4);
    write_u32(output, 36U + data_size);
    output.write("WAVEfmt ", 8);
    write_u32(output, 16);
    write_u16(output, 1);
    write_u16(output, 2);
    write_u32(output, audio_sample_rate);
    write_u32(output, audio_sample_rate * 4U);
    write_u16(output, 4);
    write_u16(output, 16);
    output.write("data", 4);
    write_u32(output, data_size);
    output.write(
        reinterpret_cast<const char*>(samples.data()),
        static_cast<std::streamsize>(data_size));
    if (!output) throw std::runtime_error("cannot finish LOG1 track WAV");
}


struct RuntimeTilesetRecord {
    std::size_t map_index = 0;
    unsigned background = 0;
    unsigned mode = 0;
    bool colour_256 = false;
    unsigned character_base = 0;
    unsigned screen_base = 0;
    unsigned tile_count = 0;
    unsigned referenced_tiles = 0;
    std::string filename;
};

std::pair<unsigned, unsigned> text_background_dimensions(std::uint16_t control) {
    switch ((control >> 14U) & 3U) {
    case 0U: return {32U, 32U};
    case 1U: return {64U, 32U};
    case 2U: return {32U, 64U};
    default: return {64U, 64U};
    }
}

std::size_t text_screen_entry_offset(
    unsigned screen_base,
    unsigned width_tiles,
    unsigned x,
    unsigned y) {
    const unsigned block_x = x / 32U;
    const unsigned block_y = y / 32U;
    const unsigned blocks_per_row = width_tiles / 32U;
    const unsigned block = block_y * blocks_per_row + block_x;
    return static_cast<std::size_t>(screen_base) +
        static_cast<std::size_t>(block) * 0x800U +
        static_cast<std::size_t>((y & 31U) * 32U + (x & 31U)) * 2U;
}

bool is_text_background(unsigned mode, unsigned background) {
    if (mode == 0U) return background < 4U;
    if (mode == 1U) return background < 2U;
    return false;
}

bool is_affine_background(unsigned mode, unsigned background) {
    if (mode == 1U) return background == 2U;
    if (mode == 2U) return background == 2U || background == 3U;
    return false;
}

std::string tileset_signature(
    const std::vector<Rgba>& pixels,
    unsigned width,
    unsigned height) {
    std::string signature;
    signature.reserve(8U + pixels.size() * 4U);
    const auto append_u32 = [&](std::uint32_t value) {
        signature.push_back(static_cast<char>(value));
        signature.push_back(static_cast<char>(value >> 8U));
        signature.push_back(static_cast<char>(value >> 16U));
        signature.push_back(static_cast<char>(value >> 24U));
    };
    append_u32(width);
    append_u32(height);
    for (const Rgba& pixel : pixels) {
        signature.push_back(static_cast<char>(pixel.r));
        signature.push_back(static_cast<char>(pixel.g));
        signature.push_back(static_cast<char>(pixel.b));
        signature.push_back(static_cast<char>(pixel.a));
    }
    return signature;
}

bool capture_runtime_background_tileset(
    const GBASystem& system,
    std::size_t map_index,
    unsigned background,
    const std::filesystem::path& folder,
    RuntimeTilesetRecord& record,
    std::vector<Rgba>& pixels,
    unsigned& atlas_width,
    unsigned& atlas_height) {
    const unsigned mode = system.DISPCNT & 7U;
    if ((system.DISPCNT & (0x0100U << background)) == 0U) return false;
    const bool text_background = is_text_background(mode, background);
    const bool affine_background = is_affine_background(mode, background);
    if (!text_background && !affine_background) return false;

    const std::uint16_t control = read_u16(system.ioMem, 0x008U + background * 2U);
    const unsigned character_base = ((control >> 2U) & 3U) * 0x4000U;
    const unsigned screen_base = ((control >> 8U) & 31U) * 0x800U;
    const bool colour_256 = affine_background || (control & 0x0080U) != 0U;
    const unsigned bytes_per_tile = colour_256 ? 64U : 32U;
    if (character_base >= 0x10000U) return false;
    const unsigned maximum_by_vram = (0x10000U - character_base) / bytes_per_tile;
    const unsigned tile_count = std::min(1024U, maximum_by_vram);
    if (tile_count == 0U) return false;

    std::array<Rgba, 256> palette{};
    for (std::size_t index = 0; index < palette.size(); ++index) {
        palette[index] = decode_rgb555(read_u16(system.paletteRAM, index * 2U));
    }
    palette[0].a = 0U;

    std::vector<std::array<unsigned, 16>> palette_usage(tile_count);
    std::vector<unsigned> tile_usage(tile_count, 0U);
    if (text_background) {
        const auto dimensions = text_background_dimensions(control);
        for (unsigned y = 0; y < dimensions.second; ++y) {
            for (unsigned x = 0; x < dimensions.first; ++x) {
                const std::size_t entry_offset = text_screen_entry_offset(
                    screen_base, dimensions.first, x, y);
                if (entry_offset + 2U > 0x10000U) continue;
                const std::uint16_t entry = read_u16(system.vram, entry_offset);
                const unsigned tile = entry & 0x03FFU;
                if (tile >= tile_count) continue;
                ++tile_usage[tile];
                ++palette_usage[tile][(entry >> 12U) & 15U];
            }
        }
    } else {
        const unsigned dimension_tiles = 16U << ((control >> 14U) & 3U);
        const std::size_t entry_count = static_cast<std::size_t>(dimension_tiles) * dimension_tiles;
        for (std::size_t entry_index = 0; entry_index < entry_count; ++entry_index) {
            const std::size_t entry_offset = static_cast<std::size_t>(screen_base) + entry_index;
            if (entry_offset >= 0x10000U) break;
            const unsigned tile = system.vram[entry_offset];
            if (tile < tile_count) ++tile_usage[tile];
        }
    }

    std::vector<unsigned> exported_tile_indices;
    exported_tile_indices.reserve(tile_count);
    for (unsigned tile = 0; tile < tile_count; ++tile) {
        if (tile_usage[tile] != 0U) exported_tile_indices.push_back(tile);
    }
    if (exported_tile_indices.empty()) exported_tile_indices.push_back(0U);
    const unsigned referenced_tiles = static_cast<unsigned>(
        exported_tile_indices.size());
    const unsigned exported_tiles = referenced_tiles;
    const unsigned columns = std::min(32U, std::max(1U, exported_tiles));
    const unsigned rows = (exported_tiles + columns - 1U) / columns;
    atlas_width = columns * 8U;
    atlas_height = rows * 8U;
    pixels.assign(
        static_cast<std::size_t>(atlas_width) * atlas_height,
        Rgba{0, 0, 0, 0});

    for (unsigned destination_tile = 0; destination_tile < exported_tiles;
         ++destination_tile) {
        const unsigned tile = exported_tile_indices[destination_tile];
        unsigned bank = 0U;
        if (!colour_256) {
            for (unsigned candidate = 1U; candidate < 16U; ++candidate) {
                if (palette_usage[tile][candidate] > palette_usage[tile][bank]) {
                    bank = candidate;
                }
            }
        }
        const unsigned destination_tile_x = destination_tile % columns;
        const unsigned destination_tile_y = destination_tile / columns;
        const std::size_t tile_offset = static_cast<std::size_t>(character_base) +
            static_cast<std::size_t>(tile) * bytes_per_tile;
        for (unsigned y = 0; y < 8U; ++y) {
            for (unsigned x = 0; x < 8U; ++x) {
                unsigned colour = 0U;
                if (colour_256) {
                    colour = system.vram[tile_offset + y * 8U + x];
                } else {
                    const std::uint8_t packed =
                        system.vram[tile_offset + y * 4U + x / 2U];
                    const unsigned nibble = (x & 1U) != 0U
                        ? packed >> 4U
                        : packed & 15U;
                    colour = bank * 16U + nibble;
                }
                Rgba pixel = palette[colour & 255U];
                if ((colour_256 && colour == 0U) ||
                    (!colour_256 && (colour & 15U) == 0U)) {
                    pixel.a = 0U;
                }
                const unsigned destination_x = destination_tile_x * 8U + x;
                const unsigned destination_y = destination_tile_y * 8U + y;
                pixels[static_cast<std::size_t>(destination_y) * atlas_width + destination_x] = pixel;
            }
        }
    }

    record.map_index = map_index;
    record.background = background;
    record.mode = mode;
    record.colour_256 = colour_256;
    record.character_base = character_base;
    record.screen_base = screen_base;
    record.tile_count = exported_tiles;
    record.referenced_tiles = referenced_tiles;
    std::ostringstream filename;
    filename << "map_" << std::setw(2) << std::setfill('0') << map_index
             << "_bg" << background << "_tileset.png";
    record.filename = filename.str();
    (void)folder;
    return true;
}

void write_runtime_tileset_gallery(
    const std::filesystem::path& output,
    const std::vector<RuntimeTilesetRecord>& records) {
    std::ofstream html(output / "tileset_gallery.html");
    html << R"HTML(<!doctype html><meta charset="utf-8"><title>Legacy of Goku runtime tilesets</title>
<style>body{background:#171b20;color:#eef3f8;font:14px Segoe UI,Arial;margin:0}header{position:sticky;top:0;background:#11151a;padding:16px;border-bottom:3px solid #f47d1f;z-index:2}main{display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));gap:14px;padding:16px}article{background:#272d34;border:1px solid #59636e;border-radius:12px;padding:10px}img{display:block;width:256px;max-width:100%;height:auto;margin:auto;image-rendering:pixelated;background:repeating-conic-gradient(#20262d 0 25%,#151a1f 0 50%) 50%/16px 16px}.meta{color:#b9c4cf;line-height:1.45}</style><header><h1>Legacy of Goku decoded runtime tilesets</h1><p>Background character data read directly from GBA VRAM with the active RGB555 palette and map palette-bank usage. These are tile atlases, not emulator screenshots.</p></header><main>)HTML";
    for (const RuntimeTilesetRecord& record : records) {
        html << "<article><b>Map " << record.map_index << " / BG"
             << record.background << "</b><a href=\"runtime_tilesets/"
             << record.filename << "\"><img loading=\"lazy\" src=\"runtime_tilesets/"
             << record.filename << "\"></a><div class=\"meta\">mode "
             << record.mode << " &middot; "
             << (record.colour_256 ? "8bpp" : "4bpp")
             << " &middot; char base 0x" << std::hex << std::uppercase
             << record.character_base << " &middot; screen base 0x"
             << record.screen_base << std::dec << "<br>" << record.tile_count
             << " exported tiles; " << record.referenced_tiles
             << " referenced by the active map</div></article>";
    }
    html << "</main>";
}

void export_log1_runtime_tilesets(
    const Rom& rom,
    const std::filesystem::path& output) {
    const std::filesystem::path folder = output / "runtime_tilesets";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    std::ofstream csv(output / "runtime_tilesets.csv");
    csv << "map,background,video_mode,bpp,character_base,screen_base,exported_tiles,referenced_tiles,png,status\n";

    std::vector<RuntimeTilesetRecord> records;
    std::set<std::string> seen_tilesets;

    for (std::size_t map_index = 0; map_index < log1_map_count; ++map_index) {
        Log1RuntimeSession map_session(rom);
        map_session.boot_to_debug_menu();
        for (std::size_t selection = 0; selection < map_index; ++selection) {
            map_session.press(key_right, 3, 5);
        }
        map_session.press(key_a, 8, 30);
        map_session.run_frames(180U);

        bool captured_any = false;
        for (unsigned background = 0; background < 4U; ++background) {
            RuntimeTilesetRecord record;
            std::vector<Rgba> pixels;
            unsigned width = 0U;
            unsigned height = 0U;
            if (!capture_runtime_background_tileset(
                    map_session.system(), map_index, background, folder,
                    record, pixels, width, height)) {
                continue;
            }
            captured_any = true;
            const std::string signature = tileset_signature(pixels, width, height);
            if (!seen_tilesets.insert(signature).second) {
                csv << map_index << ',' << background << ',' << record.mode << ','
                    << (record.colour_256 ? 8 : 4) << ',' << record.character_base << ','
                    << record.screen_base << ',' << record.tile_count << ','
                    << record.referenced_tiles << ",,duplicate tileset\n";
                continue;
            }
            write_png_rgba(folder / record.filename, width, height, pixels);
            csv << map_index << ',' << background << ',' << record.mode << ','
                << (record.colour_256 ? 8 : 4) << ',' << record.character_base << ','
                << record.screen_base << ',' << record.tile_count << ','
                << record.referenced_tiles << ',' << record.filename << ",decoded from VRAM\n";
            records.push_back(std::move(record));
        }
        if (!captured_any) {
            csv << map_index << ",,,,,,,,no active text/affine backgrounds after map load\n";
        }
    }
    write_runtime_tileset_gallery(output, records);
}

std::vector<std::int16_t> fixed_duration_audio(
    const std::vector<std::int16_t>& captured,
    unsigned seconds) {
    const std::size_t requested = static_cast<std::size_t>(audio_sample_rate) *
        2U * seconds;
    if (captured.size() < requested) {
        throw std::runtime_error("LOG1 sound test did not produce enough audio");
    }
    return {
        captured.begin(),
        captured.begin() + static_cast<std::ptrdiff_t>(requested)};
}

} // namespace

std::string log1_runtime_summary() {
    std::ostringstream summary;
    summary << "Original-engine LOG1 extraction: "
            << log1_sprite_count << " transparent OBJ animation sheets and "
            << log1_music_count << " Play Music selections. Runtime viewport "
               "captures are no longer presented as complete map layers.";
    return summary.str();
}

void export_log1_runtime_graphics(
    const Rom& rom,
    const std::filesystem::path& output) {
    if (!is_log1_rom(rom)) {
        throw std::runtime_error("LOG1 runtime graphics requires ALGP Europe Rev 0");
    }
    std::filesystem::remove_all(output / "sprites");
    std::filesystem::remove(output / "sprites.csv");
    std::filesystem::remove(output / "sprites_gallery.html");
    std::filesystem::create_directories(output / "sprites");

    Log1RuntimeSession sprite_session(rom);
    sprite_session.boot_to_debug_menu();
    sprite_session.press(key_down, 8, 12);
    sprite_session.press(key_a, 8, 30);
    std::ofstream sprites_csv(output / "sprites.csv");
    sprites_csv << "sprite,unique_animation_frames,png\n";
    for (std::size_t sprite = 0; sprite < log1_sprite_count; ++sprite) {
        sprite_session.run_frames(12);
        std::vector<RuntimeSpriteFrame> frames;
        std::set<std::string> seen;
        for (unsigned sample = 0; sample < 40U; ++sample) {
            sprite_session.run_frames(3U);
            RuntimeSpriteFrame frame = crop_sprite_preview(
                render_objects_only(sprite_session.system()));
            if (frame.empty()) continue;
            std::string signature = runtime_frame_signature(frame);
            if (seen.insert(std::move(signature)).second) {
                frames.push_back(std::move(frame));
            }
        }

        RuntimeSpriteFrame sheet = make_runtime_sprite_sheet(frames);
        if (sheet.empty()) {
            sheet.width = 1U;
            sheet.height = 1U;
            sheet.pixels.assign(1U, Rgba{0, 0, 0, 0});
        }
        const std::string filename = numbered_filename("sprite_", sprite, ".png", 3);
        write_png_rgba(
            output / "sprites" / filename,
            sheet.width,
            sheet.height,
            sheet.pixels);
        sprites_csv << sprite << ',' << frames.size() << ',' << filename << '\n';
        if (sprite + 1U < log1_sprite_count) {
            sprite_session.press(key_down, 4, 10);
        }
    }

    write_gallery(
        output,
        "Legacy of Goku OBJ animation sheets",
        "sprites",
        "sprite_",
        log1_sprite_count,
        3);

    export_log1_runtime_tilesets(rom, output);

    std::ofstream level_status(output / "LOG1_level_structure_status.txt");
    level_status
        << "Legacy of Goku level extraction status\n"
        << "========================================\n\n"
        << "The former 240x160 Sprite Viewer and Go To Map captures have been "
           "removed. Sprite sheets contain only transparent GBA OBJ/OAM pixels. "
           "runtime_tilesets contains background character data decoded directly "
           "from GBA VRAM with active RGB555 palettes and per-map palette-bank "
           "usage. These PNGs are actual tile atlases, not viewport screenshots. "
           "Complete LOG1 map records and full layer grids remain under "
           "reconstruction.\n";

    std::ofstream note(output / "README.txt");
    note << "Legacy of Goku OBJ sprite export\n"
         << "=================================\n\n"
         << "Each sprite PNG is a transparent sheet of unique animated OBJ "
            "frames from the hidden Sprite Viewer preview area. "
            "tileset_gallery.html displays background tile atlases read from "
            "VRAM after loading each Go To Map selection; no menu or viewport "
            "screenshots are used.\n";
}

void render_log1_runtime_track_preview_wav(
    const Rom& rom,
    std::size_t track,
    const std::filesystem::path& output_path,
    unsigned seconds) {
    if (!is_log1_rom(rom)) {
        throw std::runtime_error(
            "LOG1 runtime soundtrack requires ALGP Europe Rev 0");
    }
    if (track >= log1_music_count) {
        throw std::out_of_range("LOG1 track index is out of range");
    }
    if (seconds == 0U || seconds > 300U) {
        throw std::out_of_range(
            "LOG1 preview duration must be between 1 and 300 seconds");
    }

    Log1RuntimeSession session(rom);
    session.boot_to_debug_menu();
    session.press(key_down, 6, 8);
    session.press(key_down, 6, 8);
    session.press(key_down, 6, 8);
    for (std::size_t index = 0; index < track; ++index) {
        session.press(key_right, 3, 5);
    }
    session.press(key_a, 6, 10);
    session.audio().clear();
    session.run_frames((seconds + 1U) * 60U);
    write_stereo_wav(
        output_path,
        fixed_duration_audio(session.audio().samples(), seconds));
}

BgmRenderResult render_log1_runtime_track_full_wav(
    const Rom& rom,
    std::size_t track,
    const std::filesystem::path& output_path,
    unsigned maximum_seconds,
    unsigned loop_count,
    unsigned fade_seconds) {
    if (!is_log1_rom(rom)) {
        throw std::runtime_error(
            "LOG1 runtime soundtrack requires ALGP Europe Rev 0");
    }
    if (track >= log1_music_count) {
        throw std::out_of_range("LOG1 track index is out of range");
    }

    Log1RuntimeSession session(rom);
    session.boot_to_debug_menu();
    session.press(key_down, 6, 8);
    session.press(key_down, 6, 8);
    session.press(key_down, 6, 8);
    for (std::size_t index = 0; index < track; ++index) {
        session.press(key_right, 3, 5);
    }
    session.press(key_a, 6, 10);
    session.audio().clear();
    session.run_frames((maximum_seconds + 1U) * 60U);
    return write_full_stereo_wav_from_capture(
        session.audio().samples(),
        audio_sample_rate,
        output_path,
        maximum_seconds,
        loop_count,
        fade_seconds);
}

void write_runtime_audio_progress(
    const std::filesystem::path& soundtrack_directory,
    const std::string& message) {
    std::filesystem::create_directories(soundtrack_directory);
    std::ofstream progress(soundtrack_directory / ".audio_progress.txt");
    progress << message << '\n';
}

void export_log1_runtime_soundtrack(
    const Rom& rom,
    const std::filesystem::path& output,
    unsigned maximum_seconds_per_track) {
    if (!is_log1_rom(rom)) {
        throw std::runtime_error(
            "LOG1 runtime soundtrack requires ALGP Europe Rev 0");
    }
    if (maximum_seconds_per_track < 30U ||
        maximum_seconds_per_track > 900U) {
        throw std::out_of_range(
            "LOG1 full-track maximum must be between 30 and 900 seconds");
    }

    constexpr const char* cache_version =
        "DragonByteZ-0.7.6-LOG1-declicked-interpolated-full-WAV";
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

    std::ofstream csv(output / "tracks.csv");
    csv << "track,duration_seconds,loop_detected,loop_start_seconds,"
           "loop_length_seconds,natural_end,wav,status\n";
    std::ofstream playlist(output / "DragonByteZ_LOG1.m3u");

    std::unique_ptr<Log1RuntimeSession> session;
    auto ensure_sound_test = [&]() -> Log1RuntimeSession& {
        if (!session) {
            session = std::make_unique<Log1RuntimeSession>(rom);
            session->boot_to_debug_menu();
            session->press(key_down, 6, 8);
            session->press(key_down, 6, 8);
            session->press(key_down, 6, 8);
        }
        return *session;
    };

    std::size_t selected_track = 0;
    const std::filesystem::path progress_directory = output.parent_path();
    for (std::size_t track = 0; track < log1_music_count; ++track) {
        write_runtime_audio_progress(
            progress_directory,
            "Rendering full track " + std::to_string(track + 1U) + " of " +
                std::to_string(log1_music_count) + "...");

        const std::string filename =
            numbered_filename("track_", track, ".wav", 2);
        const std::filesystem::path wav_path = output / "tracks" / filename;
        bool reused = false;
        bool rendered = false;
        BgmRenderResult render_result;
        std::error_code error;
        std::string failure;

        if (std::filesystem::is_regular_file(wav_path, error) &&
            !error && std::filesystem::file_size(wav_path, error) > 44U &&
            !error) {
            reused = true;
        } else {
            try {
                Log1RuntimeSession& active = ensure_sound_test();
                while (selected_track < track) {
                    active.press(key_right, 3, 5);
                    ++selected_track;
                }
                active.press(key_a, 6, 10);
                active.audio().clear();
                active.run_frames((maximum_seconds_per_track + 1U) * 60U);
                render_result = write_full_stereo_wav_from_capture(
                    active.audio().samples(),
                    audio_sample_rate,
                    wav_path,
                    maximum_seconds_per_track,
                    2,
                    5);
                rendered = true;
            } catch (const std::exception& render_error) {
                failure = render_error.what();
                std::filesystem::remove(wav_path, error);
                session.reset();
                selected_track = 0;
            }
        }

        csv << track << ',';
        if (reused) {
            csv << "cached,cached,cached,cached,cached,";
        } else if (rendered) {
            csv << render_result.duration_seconds << ','
                << (render_result.loop_detected ? 1 : 0) << ','
                << render_result.loop_start_seconds << ','
                << render_result.loop_length_seconds << ','
                << (render_result.natural_end_detected ? 1 : 0) << ',';
        } else {
            csv << "failed,failed,failed,failed,failed,";
        }

        csv << filename << ',';
        if (reused) {
            csv << "cached";
        } else if (rendered) {
            csv << "rendered";
        } else {
            csv << "failed: " << failure;
        }
        csv << '\n';

        if (reused || rendered) {
            playlist << "tracks/" << filename << '\n';
        }
    }

    write_runtime_audio_progress(
        progress_directory,
        "Full track rendering complete.");

    std::ofstream note(output / "README.txt");
    note << "Legacy of Goku original-engine full music export\n"
         << "================================================\n\n"
         << "All " << log1_music_count
         << " entries are selected through the hidden Play Music sound test "
            "and rendered by the embedded GBA engine as 44.1 kHz stereo WAV "
            "files. DragonByteZ writes the complete non-looping track or, for "
            "looping music, the introduction plus two detected loops followed "
            "by a five-second fade. The maximum fallback is "
         << maximum_seconds_per_track
         << " seconds when a loop or natural end cannot be identified.\n";
}


} // namespace dragonbytez
