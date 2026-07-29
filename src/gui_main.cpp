#ifdef _WIN32

#include "dragonbytez/analysis.hpp"
#include "dragonbytez/gsf_player.hpp"
#include "dragonbytez/rom.hpp"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objidl.h>
#include <propidl.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t window_class_name[] = L"DragonByteZMainWindow";
constexpr wchar_t splash_class_name[] = L"DragonByteZSplashWindow";
constexpr COLORREF splash_transparency_key = RGB(1, 0, 1);
constexpr int application_icon_id = 101;
constexpr int intro_audio_id = 102;
constexpr wchar_t application_title[] = L"DragonByteZ 0.6.23";
constexpr UINT message_analysis_finished = WM_APP + 1;
constexpr UINT message_analysis_failed = WM_APP + 2;
constexpr UINT_PTR dragon_ball_timer_id = 2;
constexpr unsigned bgm_preview_seconds = 180;
constexpr int header_height = 78;
constexpr int content_margin = 18;
constexpr int setup_panel_top = 96;
constexpr int setup_panel_height = 274;
constexpr int workspace_top = 412;
constexpr int footer_height = 72;

enum ControlId {
    control_rom_path = 1001,
    control_browse_rom,
    control_output_path,
    control_browse_output,
    control_rom_information,
    control_graphics,
    control_soundtrack,
    control_analyze,
    control_open_output,
    control_progress,
    control_status,
    control_results,
    control_open_selected,
    control_open_contact_sheet,
    control_open_sprite_gallery,
    control_preview_image,
    control_preview_text,
    control_preview_selected,
    control_level_information,
    control_open_repository
};

struct ApplicationState {
    HWND window = nullptr;
    HWND rom_path = nullptr;
    HWND output_path = nullptr;
    HWND rom_information = nullptr;
    HWND level_information = nullptr;
    HWND graphics = nullptr;
    HWND soundtrack = nullptr;
    HWND analyze = nullptr;
    HWND open_output = nullptr;
    HWND progress = nullptr;
    HWND status = nullptr;
    HWND results = nullptr;
    HWND open_selected = nullptr;
    HWND open_contact_sheet = nullptr;
    HWND open_sprite_gallery = nullptr;
    HWND preview_image = nullptr;
    HWND preview_text = nullptr;
    HWND preview_selected = nullptr;
    std::unique_ptr<Gdiplus::Image> preview_bitmap;
    HFONT normal_font = nullptr;
    HFONT title_font = nullptr;
    HFONT subtitle_font = nullptr;
    HFONT section_font = nullptr;
    HBRUSH background_brush = nullptr;
    HBRUSH panel_brush = nullptr;
    HBRUSH header_brush = nullptr;
    HBRUSH accent_brush = nullptr;
    HBRUSH accent_pressed_brush = nullptr;
    HBRUSH border_brush = nullptr;
    bool running = false;
    dragonbytez::GameFamily current_game_family =
        dragonbytez::GameFamily::unknown;
};

ApplicationState application;
ULONG_PTR gdiplus_token = 0;
unsigned dragon_ball_tick = 0;

HICON load_dragonbytez_icon(HINSTANCE instance, int width, int height);

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int required = MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) return L"Unknown error";
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        result.data(), required);
    return result;
}

std::wstring get_window_text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring result(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) GetWindowTextW(control, result.data(), length + 1);
    result.resize(static_cast<std::size_t>(length));
    return result;
}

void set_control_font(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND create_control(
    DWORD extended_style,
    const wchar_t* class_name,
    const wchar_t* text,
    DWORD style,
    int id) {
    HWND control = CreateWindowExW(
        extended_style,
        class_name,
        text,
        style | WS_CHILD | WS_VISIBLE,
        0,
        0,
        1,
        1,
        application.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr),
        nullptr);
    if (!control) throw std::runtime_error("Unable to create a GUI control");
    set_control_font(control, application.normal_font);
    return control;
}

std::filesystem::path selected_output_directory() {
    return std::filesystem::path(get_window_text(application.output_path));
}

void show_error(const std::wstring& text) {
    MessageBoxW(
        application.window, text.c_str(), application_title,
        MB_OK | MB_ICONERROR);
}

void open_in_shell(const std::filesystem::path& path) {
    if (path.empty() || !std::filesystem::exists(path)) {
        show_error(L"The selected output does not exist yet.");
        return;
    }
    const HINSTANCE result = ShellExecuteW(
        application.window, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        show_error(L"Windows could not open the selected file or directory.");
    }
}

void open_repository() {
    constexpr wchar_t repository_url[] =
        L"https://github.com/spicybung/DragonByteZ";
    const HINSTANCE result = ShellExecuteW(
        application.window, L"open", repository_url,
        nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        show_error(L"Windows could not open the DragonByteZ repository.");
    }
}

std::filesystem::path open_rom_dialog() {
    std::vector<wchar_t> filename(32768, L'\0');
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = application.window;
    dialog.lpstrFilter =
        L"Game Boy Advance ROM (*.gba)\0*.gba\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = filename.data();
    dialog.nMaxFile = static_cast<DWORD>(filename.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                   OFN_EXPLORER | OFN_NOCHANGEDIR;
    dialog.lpstrDefExt = L"gba";
    if (!GetOpenFileNameW(&dialog)) return {};
    return std::filesystem::path(filename.data());
}

std::filesystem::path select_folder_dialog(
    const std::filesystem::path& initial_directory) {
    IFileDialog* raw_dialog = nullptr;
    const HRESULT created = CoCreateInstance(
        CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&raw_dialog));
    if (FAILED(created)) return {};
    std::unique_ptr<IFileDialog, void (*)(IFileDialog*)> dialog(
        raw_dialog, [](IFileDialog* value) { value->Release(); });

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Select DragonByteZ output directory");

    if (!initial_directory.empty() && std::filesystem::exists(initial_directory)) {
        IShellItem* raw_item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                initial_directory.c_str(), nullptr, IID_PPV_ARGS(&raw_item)))) {
            dialog->SetFolder(raw_item);
            raw_item->Release();
        }
    }

    if (FAILED(dialog->Show(application.window))) return {};
    IShellItem* raw_result = nullptr;
    if (FAILED(dialog->GetResult(&raw_result))) return {};
    std::unique_ptr<IShellItem, void (*)(IShellItem*)> result(
        raw_result, [](IShellItem* value) { value->Release(); });

    PWSTR value = nullptr;
    if (FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &value))) return {};
    const std::filesystem::path directory(value);
    CoTaskMemFree(value);
    return directory;
}

void set_running(bool running) {
    application.running = running;
    EnableWindow(application.rom_path, !running);
    EnableWindow(GetDlgItem(application.window, control_browse_rom), !running);
    EnableWindow(application.output_path, !running);
    EnableWindow(GetDlgItem(application.window, control_browse_output), !running);
    EnableWindow(application.graphics, !running);
    EnableWindow(application.soundtrack, !running);
    EnableWindow(application.analyze, !running);
    ShowWindow(application.progress, running ? SW_SHOW : SW_HIDE);
    SendMessageW(application.progress, PBM_SETMARQUEE, running ? TRUE : FALSE, 30);
}

void populate_results(const std::filesystem::path& output_directory) {
    SendMessageW(application.results, LB_RESETCONTENT, 0, 0);
    SendMessageW(application.results, LB_SETHORIZONTALEXTENT, 0, 0);
    if (!std::filesystem::exists(output_directory)) return;

    std::vector<std::filesystem::path> files;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(output_directory)) {
        if (!entry.is_regular_file()) continue;
        const auto extension = entry.path().extension().wstring();
        if (extension == L".png" || extension == L".wav" ||
            extension == L".csv" || extension == L".txt" ||
            extension == L".xlsx" || extension == L".minigsf" ||
            extension == L".gsflib" || extension == L".m3u" ||
            extension == L".bin" || extension == L".html") {
            files.push_back(std::filesystem::relative(
                entry.path(), output_directory));
        }
    }
    std::sort(files.begin(), files.end());

    int horizontal_extent = 0;
    HDC device = GetDC(application.results);
    HGDIOBJ previous_font = nullptr;
    if (device) {
        previous_font = SelectObject(device, application.normal_font);
    }
    for (const auto& file : files) {
        const std::wstring text = file.wstring();
        SendMessageW(
            application.results, LB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(text.c_str()));
        if (device) {
            SIZE size{};
            if (GetTextExtentPoint32W(
                    device, text.c_str(), static_cast<int>(text.size()), &size)) {
                horizontal_extent = std::max(
                    horizontal_extent,
                    static_cast<int>(size.cx) + 24);
            }
        }
    }
    if (device) {
        if (previous_font) SelectObject(device, previous_font);
        ReleaseDC(application.results, device);
    }
    SendMessageW(
        application.results, LB_SETHORIZONTALEXTENT,
        static_cast<WPARAM>(horizontal_extent), 0);
}

std::filesystem::path selected_result_path() {
    const LRESULT selection =
        SendMessageW(application.results, LB_GETCURSEL, 0, 0);
    if (selection == LB_ERR) return {};
    const LRESULT length =
        SendMessageW(application.results, LB_GETTEXTLEN, selection, 0);
    if (length == LB_ERR) return {};
    std::wstring relative(static_cast<std::size_t>(length) + 1, L'\0');
    SendMessageW(
        application.results, LB_GETTEXT, selection,
        reinterpret_cast<LPARAM>(relative.data()));
    relative.resize(static_cast<std::size_t>(length));
    return selected_output_directory() / relative;
}

void clear_preview() {
    PlaySoundW(nullptr, nullptr, 0);
    application.preview_bitmap.reset();
    SetWindowTextW(
        application.preview_text,
        L"Select an image, report, sample, sequence, or GSF track to "
        L"preview it here.");
    ShowWindow(application.preview_image, SW_HIDE);
    ShowWindow(application.preview_text, SW_SHOW);
    InvalidateRect(application.preview_image, nullptr, TRUE);
}

bool track_index_from_path(
    const std::filesystem::path& path,
    std::size_t& track) {
    const std::wstring filename = path.filename().wstring();
    constexpr wchar_t prefix[] = L"track_";
    if (filename.rfind(prefix, 0) != 0) return false;
    std::size_t position = 6;
    std::size_t value = 0;
    bool has_digit = false;
    while (position < filename.size() &&
           std::iswdigit(filename[position])) {
        has_digit = true;
        value = value * 10U +
            static_cast<std::size_t>(filename[position] - L'0');
        ++position;
    }
    if (!has_digit || value >= 1000) return false;
    track = value;
    return true;
}

void play_internal_bgm(std::size_t track) {
    const std::filesystem::path rom_path(
        get_window_text(application.rom_path));
    if (!std::filesystem::is_regular_file(rom_path)) {
        show_error(L"Select the matching ROM before playing level music.");
        return;
    }

    const std::filesystem::path preview_directory =
        selected_output_directory() / L"soundtrack" /
        L"level_music" / L"preview_cache";
    SetWindowTextW(
        application.status,
        L"Rendering track with DragonByteZ's embedded GBA audio engine...");
    EnableWindow(application.preview_selected, FALSE);
    const HCURSOR previous_cursor =
        SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    try {
        const dragonbytez::Rom rom(rom_path);
        std::wostringstream filename;
        filename << L"track_" << std::setw(2) << std::setfill(L'0')
                 << track << L"_" << utf8_to_wide(rom.game_code())
                 << L"_v0618_180s.wav";
        const std::filesystem::path preview_path =
            preview_directory / filename.str();
        if (!std::filesystem::is_regular_file(preview_path)) {
            dragonbytez::render_bgm_preview_wav(
                rom, track, preview_path, bgm_preview_seconds);
        }
        if (!PlaySoundW(
                preview_path.c_str(), nullptr,
                SND_FILENAME | SND_ASYNC | SND_NODEFAULT)) {
            throw std::runtime_error(
                "Windows could not play the rendered BGM preview");
        }
        std::wostringstream description;
        description
            << L"Webfoot level music\r\n\r\nTrack "
            << std::setw(2) << std::setfill(L'0') << track
            << L"\r\n\r\nPlaying a three-minute stereo render created "
               L"on demand inside DragonByteZ by its embedded GBA audio "
               L"engine.\r\n\r\n"
            << L"Cached WAV:\r\n" << preview_path.wstring();
        SetWindowTextW(
            application.preview_text, description.str().c_str());
        SetWindowTextW(
            application.status,
            L"Playing internally rendered level music.");
    } catch (const std::exception& error) {
        SetWindowTextW(
            application.status, L"Music preview failed.");
        show_error(utf8_to_wide(error.what()));
    }
    SetCursor(previous_cursor);
    EnableWindow(application.preview_selected, TRUE);
}

void show_selected_preview(bool play_audio) {
    const auto path = selected_result_path();
    if (path.empty() || !std::filesystem::is_regular_file(path)) {
        clear_preview();
        return;
    }

    std::wstring extension = path.extension().wstring();
    std::transform(
        extension.begin(), extension.end(), extension.begin(),
        [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });

    if (extension == L".png") {
        auto image = std::make_unique<Gdiplus::Image>(path.c_str());
        if (image->GetLastStatus() != Gdiplus::Ok) {
            SetWindowTextW(application.preview_text, L"Unable to decode this PNG.");
            ShowWindow(application.preview_image, SW_HIDE);
            ShowWindow(application.preview_text, SW_SHOW);
            return;
        }
        application.preview_bitmap = std::move(image);
        ShowWindow(application.preview_text, SW_HIDE);
        ShowWindow(application.preview_image, SW_SHOW);
        InvalidateRect(application.preview_image, nullptr, TRUE);
        return;
    }

    application.preview_bitmap.reset();
    ShowWindow(application.preview_image, SW_HIDE);
    ShowWindow(application.preview_text, SW_SHOW);

    if (extension == L".wav") {
        std::wostringstream description;
        description << L"GBA PCM sample\r\n\r\n" << path.filename().wstring()
                    << L"\r\n\r\nUse Preview / Play to hear it.";
        SetWindowTextW(application.preview_text, description.str().c_str());
        if (play_audio &&
            !PlaySoundW(
                path.c_str(), nullptr,
                SND_FILENAME | SND_ASYNC | SND_NODEFAULT)) {
            show_error(L"Windows could not play this WAV sample.");
        }
        return;
    }

    if (extension == L".minigsf" || extension == L".gsflib" ||
        extension == L".m3u") {
        std::size_t track = 0;
        const bool has_track =
            track_index_from_path(path, track);
        std::wostringstream description;
        description
            << L"LOG2 level music\r\n\r\n"
            << path.filename().wstring()
            << L"\r\n\r\nDragonByteZ extracted this track for the original "
               L"Webfoot GBA sound engine.\r\n";
        if (has_track) {
            description
                << L"Use Preview / Play to render and play it directly "
                   L"inside DragonByteZ.";
        } else {
            description
                << L"Select one of the track_XX.minigsf entries to play "
                   L"a track inside DragonByteZ.";
        }
        SetWindowTextW(application.preview_text, description.str().c_str());
        if (play_audio && has_track) play_internal_bgm(track);
        return;
    }

    if (extension == L".bin") {
        std::ifstream input(path, std::ios::binary);
        std::vector<unsigned char> bytes{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        const bool is_sequence =
            path.filename().wstring().find(L"_sequence") !=
            std::wstring::npos;
        std::wostringstream description;
        description << (is_sequence
                            ? L"Webfoot native music sequence\r\n\r\n"
                            : L"Extracted binary data\r\n\r\n")
                    << path.filename().wstring() << L"\r\n"
                    << bytes.size() << L" bytes\r\n\r\nFirst bytes:\r\n";
        const std::size_t preview_count =
            std::min<std::size_t>(bytes.size(), 96);
        description << std::uppercase << std::hex << std::setfill(L'0');
        for (std::size_t index = 0; index < preview_count; ++index) {
            if (index && index % 16 == 0) description << L"\r\n";
            description << std::setw(2)
                        << static_cast<unsigned>(bytes[index]) << L' ';
        }
        SetWindowTextW(application.preview_text, description.str().c_str());
        std::size_t track = 0;
        if (play_audio && track_index_from_path(path, track)) {
            play_internal_bgm(track);
        }
        return;
    }

    if (extension == L".html" || extension == L".htm") {
        std::wostringstream description;
        description << L"Generated visual gallery\r\n\r\n"
                    << path.filename().wstring()
                    << L"\r\n\r\nUse Open Selected to view the interactive "
                       L"gallery in your browser.";
        SetWindowTextW(application.preview_text, description.str().c_str());
        return;
    }

    if (extension == L".csv" || extension == L".txt") {
        std::ifstream input(path, std::ios::binary);
        std::string bytes{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        constexpr std::size_t preview_limit = 128 * 1024;
        if (bytes.size() > preview_limit) {
            bytes.resize(preview_limit);
            bytes += "\n\n[Preview truncated at 128 KiB]";
        }
        SetWindowTextW(application.preview_text, utf8_to_wide(bytes).c_str());
        return;
    }

    SetWindowTextW(
        application.preview_text,
        L"This workbook uses Excel's native format. Use Open Selected to view it.");
}

void inspect_rom(const std::filesystem::path& path) {
    try {
        const dragonbytez::Rom rom(path);
        application.current_game_family = dragonbytez::game_family(rom);
        std::wstring profile = L"Unknown or unsupported";
        if (dragonbytez::is_log2_rom(rom)) {
            profile = L"Legacy of Goku II " +
                utf8_to_wide(dragonbytez::profile_for(rom).name) +
                L" (supported)";
        } else if (dragonbytez::is_log1_rom(rom)) {
            profile = L"Legacy of Goku Europe Rev 0 ALGP (BIOS asset support)";
        } else if (dragonbytez::is_buus_fury_rom(rom)) {
            profile = L"Dragon Ball Z: Buu's Fury BG3E (verified map and music structures)";
        }
        std::wostringstream information;
        information << L"Internal title: " << utf8_to_wide(rom.title()) << L"\r\n"
                    << L"Game code: " << utf8_to_wide(rom.game_code()) << L"\r\n"
                    << L"Revision: "
                    << static_cast<unsigned>(rom.revision()) << L"\r\n"
                    << L"ROM size: " << rom.size() << L" bytes\r\n"
                    << L"Profile: " << profile;
        SetWindowTextW(application.rom_information, information.str().c_str());
        if (dragonbytez::is_supported_rom(rom)) {
            const std::wstring levels =
                utf8_to_wide(dragonbytez::level_summary(rom));
            std::wstring windows_levels;
            windows_levels.reserve(levels.size() + 16);
            for (const wchar_t character : levels) {
                if (character == L'\n') windows_levels += L"\r\n";
                else windows_levels.push_back(character);
            }
            SetWindowTextW(
                application.level_information, windows_levels.c_str());
        } else {
            SetWindowTextW(
                application.level_information,
                L"Analysis information is available for supported LOG1, LOG2, and Buu\'s Fury ROMs.");
        }
        SetWindowTextW(application.rom_path, path.c_str());

        const std::filesystem::path default_output =
            path.parent_path() / (path.stem().wstring() + L"_DragonByteZ");
        SetWindowTextW(application.output_path, default_output.c_str());

        const bool has_verified_sprite_gallery =
            application.current_game_family == dragonbytez::GameFamily::legacy_of_goku ||
            application.current_game_family == dragonbytez::GameFamily::legacy_of_goku_ii ||
            application.current_game_family == dragonbytez::GameFamily::buus_fury;
        EnableWindow(
            application.open_sprite_gallery,
            has_verified_sprite_gallery ? TRUE : FALSE);
        SetWindowTextW(
            application.open_sprite_gallery,
            has_verified_sprite_gallery
                ? L"Sprite Gallery"
                : L"Sprites not decoded");
        InvalidateRect(application.open_sprite_gallery, nullptr, TRUE);

        SetWindowTextW(
            application.status,
            dragonbytez::is_supported_rom(rom)
                ? L"ROM verified. Select Analyze ROM to extract its assets."
                : L"Supported: LOG1 ALGP; LOG2 ALFP / ALFE; Buu\'s Fury BG3E.");
    } catch (const std::exception& error) {
        application.current_game_family = dragonbytez::GameFamily::unknown;
        EnableWindow(application.open_sprite_gallery, FALSE);
        SetWindowTextW(application.open_sprite_gallery, L"Sprites not decoded");
        InvalidateRect(application.open_sprite_gallery, nullptr, TRUE);
        SetWindowTextW(application.rom_information, L"No valid ROM loaded.");
        SetWindowTextW(
            application.level_information, L"No level information loaded.");
        show_error(utf8_to_wide(error.what()));
    }
}

void choose_rom() {
    const auto path = open_rom_dialog();
    if (!path.empty()) inspect_rom(path);
}

void choose_output_directory() {
    const auto selected = select_folder_dialog(selected_output_directory());
    if (!selected.empty()) SetWindowTextW(application.output_path, selected.c_str());
}

void run_analysis() {
    const std::filesystem::path rom_path(get_window_text(application.rom_path));
    const std::filesystem::path output_path = selected_output_directory();
    const bool graphics =
        SendMessageW(application.graphics, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool soundtrack =
        SendMessageW(application.soundtrack, BM_GETCHECK, 0, 0) == BST_CHECKED;

    if (rom_path.empty() || !std::filesystem::is_regular_file(rom_path)) {
        show_error(L"Select a valid .gba ROM first.");
        return;
    }
    if (output_path.empty()) {
        show_error(L"Select an output directory first.");
        return;
    }
    if (!graphics && !soundtrack) {
        show_error(L"Enable Maps / tiles, Audio / music, or both.");
        return;
    }

    SendMessageW(application.results, LB_RESETCONTENT, 0, 0);
    clear_preview();
    SetWindowTextW(
        application.status,
        L"Analyzing the ROM and writing fresh output files...");
    set_running(true);

    const HWND target_window = application.window;
    std::thread([target_window, rom_path, output_path, graphics, soundtrack]() {
        try {
            const dragonbytez::Rom rom(rom_path);
            if (graphics && soundtrack) {
                dragonbytez::analyze_all(rom, output_path);
            } else if (graphics) {
                std::filesystem::create_directories(output_path);
                dragonbytez::analyze_graphics(rom, output_path / "graphics");
            } else {
                std::filesystem::create_directories(output_path);
                dragonbytez::analyze_soundtrack(rom, output_path / "soundtrack");
            }
            auto* result = new std::wstring(output_path.wstring());
            PostMessageW(
                target_window, message_analysis_finished, 0,
                reinterpret_cast<LPARAM>(result));
        } catch (const std::exception& error) {
            auto* result = new std::wstring(utf8_to_wide(error.what()));
            PostMessageW(
                target_window, message_analysis_failed, 0,
                reinterpret_cast<LPARAM>(result));
        }
    }).detach();
}

void open_selected_result() {
    const auto path = selected_result_path();
    if (path.empty()) {
        show_error(L"Select an extracted file first.");
        return;
    }
    std::wstring extension = path.extension().wstring();
    std::transform(
        extension.begin(), extension.end(), extension.begin(),
        [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    if (extension == L".minigsf" || extension == L".bin") {
        std::size_t track = 0;
        if (track_index_from_path(path, track)) {
            play_internal_bgm(track);
            return;
        }
    }
    if (extension == L".gsflib" || extension == L".m3u") {
        show_error(
            L"Select a track_XX.minigsf entry to play it inside DragonByteZ.");
        return;
    }
    open_in_shell(path);
}

void open_contact_sheet() {
    const std::filesystem::path gallery =
        selected_output_directory() / L"graphics" / L"level_gallery.html";
    if (std::filesystem::is_regular_file(gallery)) {
        open_in_shell(gallery);
        return;
    }
    const std::filesystem::path asset_gallery =
        selected_output_directory() / L"graphics" / L"asset_gallery.html";
    if (std::filesystem::is_regular_file(asset_gallery)) {
        open_in_shell(asset_gallery);
        return;
    }
    const std::filesystem::path character_gallery =
        selected_output_directory() / L"graphics" /
        L"character_sprite_gallery.html";
    if (std::filesystem::is_regular_file(character_gallery)) {
        open_in_shell(character_gallery);
        return;
    }
    const std::filesystem::path opening =
        selected_output_directory() / L"graphics" / L"level_previews" /
        L"opening" / L"Z1A1_layers_contact_sheet.png";
    if (std::filesystem::is_regular_file(opening)) {
        open_in_shell(opening);
        return;
    }
    show_error(
        L"Analyze a supported ROM with Maps / tiles enabled before opening "
        L"the level and sprite gallery.");
}


void open_sprite_gallery() {
    std::filesystem::path gallery;
    if (application.current_game_family ==
        dragonbytez::GameFamily::legacy_of_goku) {
        gallery = selected_output_directory() / L"graphics" /
            L"sprites_gallery.html";
    } else if (application.current_game_family ==
                   dragonbytez::GameFamily::legacy_of_goku_ii ||
               application.current_game_family ==
                   dragonbytez::GameFamily::buus_fury) {
        gallery = selected_output_directory() / L"graphics" /
            L"character_sprite_gallery.html";
    } else {
        show_error(L"Load and analyze a supported ROM first.");
        return;
    }

    if (std::filesystem::is_regular_file(gallery)) {
        open_in_shell(gallery);
        return;
    }
    show_error(
        L"Analyze the selected ROM with Graphics / levels enabled before "
        L"opening its decoded sprite gallery.");
}

void create_menu(HWND window) {
    HMENU menu = CreateMenu();
    HMENU file_menu = CreatePopupMenu();
    AppendMenuW(file_menu, MF_STRING, control_browse_rom, L"&Open ROM...");
    AppendMenuW(
        file_menu, MF_STRING, control_browse_output,
        L"Select &Output Directory...");
    AppendMenuW(file_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file_menu, MF_STRING, IDCANCEL, L"E&xit");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file_menu), L"&File");

    HMENU help_menu = CreatePopupMenu();
    AppendMenuW(
        help_menu, MF_STRING, control_open_repository,
        L"Open &GitHub Repository");
    AppendMenuW(help_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(help_menu, MF_STRING, IDHELP, L"&About DragonByteZ");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help_menu), L"&Help");
    SetMenu(window, menu);
}

void layout_controls(int width, int height) {
    constexpr int label_width = 88;
    constexpr int browse_width = 108;
    constexpr int gap = 10;

    const int content_width = std::max(720, width - content_margin * 2);
    const int field_x = content_margin + label_width + gap;
    const int field_width =
        std::max(240, content_width - label_width - browse_width - gap * 2);

    MoveWindow(GetDlgItem(application.window, 2001), 24, 9,
               235, 34, TRUE);
    MoveWindow(GetDlgItem(application.window, 2006), 25, 42,
               std::max(320, width - 50), 24, TRUE);

    MoveWindow(GetDlgItem(application.window, 2002), content_margin + 14, 108,
               label_width - 4, 30, TRUE);
    MoveWindow(application.rom_path, field_x + 14, 105,
               field_width - 28, 32, TRUE);
    MoveWindow(GetDlgItem(application.window, control_browse_rom),
               width - content_margin - browse_width - 14, 105,
               browse_width, 32, TRUE);

    MoveWindow(GetDlgItem(application.window, 2003), content_margin + 14, 150,
               label_width - 4, 30, TRUE);
    MoveWindow(application.output_path, field_x + 14, 147,
               field_width - 28, 32, TRUE);
    MoveWindow(GetDlgItem(application.window, control_browse_output),
               width - content_margin - browse_width - 14, 147,
               browse_width, 32, TRUE);

    const int information_width = content_width * 48 / 100;
    const int action_x = content_margin + information_width + 30;
    const int action_width =
        std::max(300, width - content_margin - 14 - action_x);
    MoveWindow(application.rom_information, content_margin + 14, 188,
               information_width - 20, 86, TRUE);
    MoveWindow(application.graphics, action_x, 194, 128, 26, TRUE);
    MoveWindow(application.soundtrack, action_x + 144, 194, 140, 26, TRUE);
    MoveWindow(application.analyze, action_x, 228, 142, 36, TRUE);
    MoveWindow(application.open_output, action_x + 152, 228, 128, 36, TRUE);
    MoveWindow(application.progress, action_x + 292, 237,
               std::max(80, action_width - 292), 18, TRUE);
    MoveWindow(application.level_information, content_margin + 14, 290,
               content_width - 28, 70, TRUE);

    const int results_width = std::max(320, content_width * 42 / 100);
    const int preview_x = content_margin + results_width + 14;
    const int preview_width =
        std::max(360, width - content_margin - preview_x);
    const int workspace_bottom = std::max(
        workspace_top + 180, height - footer_height);
    const int workspace_height = workspace_bottom - workspace_top;

    MoveWindow(GetDlgItem(application.window, 2004), content_margin,
               workspace_top - 28, results_width, 24, TRUE);
    MoveWindow(GetDlgItem(application.window, 2005), preview_x,
               workspace_top - 28, preview_width, 24, TRUE);
    MoveWindow(application.results, content_margin, workspace_top,
               results_width, workspace_height, TRUE);
    MoveWindow(application.preview_image, preview_x, workspace_top,
               preview_width, workspace_height, TRUE);
    MoveWindow(application.preview_text, preview_x, workspace_top,
               preview_width, workspace_height, TRUE);

    const int button_y = workspace_bottom + 10;
    MoveWindow(application.open_selected, content_margin, button_y,
               106, 34, TRUE);
    MoveWindow(application.open_contact_sheet, content_margin + 114, button_y,
               116, 34, TRUE);
    MoveWindow(application.open_sprite_gallery, content_margin + 238, button_y,
               116, 34, TRUE);
    MoveWindow(application.preview_selected, preview_x, button_y,
               132, 34, TRUE);
    MoveWindow(application.status, preview_x + 146, button_y,
               std::max(180, width - content_margin - preview_x - 146),
               34, TRUE);
}

void create_controls() {
    HDC device = GetDC(application.window);
    const int dpi = device ? GetDeviceCaps(device, LOGPIXELSY) : 96;
    if (device) ReleaseDC(application.window, device);

    application.normal_font = CreateFontW(
        -MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    application.title_font = CreateFontW(
        -MulDiv(22, dpi, 72), 0, 0, 0, FW_BOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Semibold");
    application.subtitle_font = CreateFontW(
        -MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    application.section_font = CreateFontW(
        -MulDiv(10, dpi, 72), 0, 0, 0, FW_SEMIBOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    application.background_brush = CreateSolidBrush(RGB(25, 28, 33));
    application.panel_brush = CreateSolidBrush(RGB(39, 44, 51));
    application.header_brush = CreateSolidBrush(RGB(18, 21, 25));
    application.accent_brush = CreateSolidBrush(RGB(244, 125, 31));
    application.accent_pressed_brush = CreateSolidBrush(RGB(196, 78, 18));
    application.border_brush = CreateSolidBrush(RGB(83, 94, 106));

    HWND title = create_control(
        0, L"STATIC", L"DragonByteZ",
        SS_LEFT | SS_CENTERIMAGE, 2001);
    set_control_font(title, application.title_font);
    HWND subtitle = create_control(
        0, L"STATIC", L"Legacy of Goku I / II and Buu\'s Fury asset workspace",
        SS_LEFT | SS_CENTERIMAGE, 2006);
    set_control_font(subtitle, application.subtitle_font);

    HWND rom_label = create_control(
        0, L"STATIC", L"ROM file", SS_LEFT | SS_CENTERIMAGE, 2002);
    set_control_font(rom_label, application.section_font);
    application.rom_path = create_control(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        ES_AUTOHSCROLL | WS_TABSTOP, control_rom_path);
    create_control(
        0, L"BUTTON", L"Browse...",
        BS_OWNERDRAW | WS_TABSTOP, control_browse_rom);

    HWND output_label = create_control(
        0, L"STATIC", L"Output folder", SS_LEFT | SS_CENTERIMAGE, 2003);
    set_control_font(output_label, application.section_font);
    application.output_path = create_control(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        ES_AUTOHSCROLL | WS_TABSTOP, control_output_path);
    create_control(
        0, L"BUTTON", L"Browse...",
        BS_OWNERDRAW | WS_TABSTOP, control_browse_output);

    application.rom_information = create_control(
        WS_EX_CLIENTEDGE, L"EDIT", L"No ROM loaded.",
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        control_rom_information);
    application.level_information = create_control(
        WS_EX_CLIENTEDGE, L"EDIT", L"No level information loaded.",
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        control_level_information);

    application.graphics = create_control(
        0, L"BUTTON", L"Graphics / levels",
        BS_AUTOCHECKBOX | WS_TABSTOP, control_graphics);
    application.soundtrack = create_control(
        0, L"BUTTON", L"WAV track previews (slower)",
        BS_AUTOCHECKBOX | WS_TABSTOP, control_soundtrack);
    SendMessageW(application.graphics, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(application.soundtrack, BM_SETCHECK, BST_UNCHECKED, 0);
    SetWindowTheme(application.graphics, L"", L"");
    SetWindowTheme(application.soundtrack, L"", L"");

    application.analyze = create_control(
        0, L"BUTTON", L"Analyze ROM",
        BS_OWNERDRAW | WS_TABSTOP, control_analyze);
    application.open_output = create_control(
        0, L"BUTTON", L"Open Output",
        BS_OWNERDRAW | WS_TABSTOP, control_open_output);
    application.progress = create_control(
        0, PROGRESS_CLASSW, L"",
        PBS_MARQUEE | PBS_SMOOTH, control_progress);
    ShowWindow(application.progress, SW_HIDE);

    HWND results_label = create_control(
        0, L"STATIC", L"Extracted assets",
        SS_LEFT | SS_CENTERIMAGE, 2004);
    set_control_font(results_label, application.section_font);
    HWND preview_label = create_control(
        0, L"STATIC", L"In-app preview",
        SS_LEFT | SS_CENTERIMAGE, 2005);
    set_control_font(preview_label, application.section_font);
    application.results = create_control(
        WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
            WS_VSCROLL | WS_HSCROLL | WS_TABSTOP,
        control_results);
    application.open_selected = create_control(
        0, L"BUTTON", L"Open Selected",
        BS_OWNERDRAW | WS_TABSTOP, control_open_selected);
    application.open_contact_sheet = create_control(
        0, L"BUTTON", L"Level Gallery",
        BS_OWNERDRAW | WS_TABSTOP, control_open_contact_sheet);
    application.open_sprite_gallery = create_control(
        0, L"BUTTON", L"Sprite Gallery",
        BS_OWNERDRAW | WS_TABSTOP, control_open_sprite_gallery);
    EnableWindow(application.open_sprite_gallery, FALSE);
    application.preview_image = create_control(
        WS_EX_CLIENTEDGE, L"STATIC", L"",
        SS_OWNERDRAW, control_preview_image);
    application.preview_text = create_control(
        WS_EX_CLIENTEDGE, L"EDIT",
        L"Select an image, report, sample, sequence, or GSF track to "
        L"preview it here.",
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
        control_preview_text);
    application.preview_selected = create_control(
        0, L"BUTTON", L"Preview / Play",
        BS_OWNERDRAW | WS_TABSTOP, control_preview_selected);
    ShowWindow(application.preview_image, SW_HIDE);
    application.status = create_control(
        0, L"STATIC", L"Select LOG1, LOG2, or Buu\'s Fury .gba ROM.",
        SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, control_status);

    SendMessageW(
        application.rom_path, EM_SETCUEBANNER, TRUE,
        reinterpret_cast<LPARAM>(L"Select ALGP, ALFP, ALFE, or Buu\'s Fury BG3E"));
    SendMessageW(
        application.output_path, EM_SETCUEBANNER, TRUE,
        reinterpret_cast<LPARAM>(L"Choose where analysis data will be stored"));
    for (HWND edit : {
             application.rom_path, application.output_path,
             application.rom_information, application.level_information,
             application.preview_text}) {
        SendMessageW(
            edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
            MAKELPARAM(8, 8));
        SetWindowTheme(edit, L"Explorer", nullptr);
    }
    SetWindowTheme(application.results, L"Explorer", nullptr);

    for (int id : {
             control_browse_rom, control_browse_output, control_analyze,
             control_open_output, control_open_selected,
             control_open_contact_sheet, control_open_sprite_gallery,
             control_preview_selected}) {
        SetWindowTheme(GetDlgItem(application.window, id), L"Explorer", nullptr);
    }
}

LRESULT handle_command(WPARAM wparam) {
    const int id = LOWORD(wparam);
    const int notification = HIWORD(wparam);
    switch (id) {
    case control_browse_rom:
        choose_rom();
        return 0;
    case control_browse_output:
        choose_output_directory();
        return 0;
    case control_analyze:
        run_analysis();
        return 0;
    case control_open_output:
        open_in_shell(selected_output_directory());
        return 0;
    case control_open_selected:
        open_selected_result();
        return 0;
    case control_open_contact_sheet:
        open_contact_sheet();
        return 0;
    case control_open_sprite_gallery:
        open_sprite_gallery();
        return 0;
    case control_preview_selected:
        show_selected_preview(true);
        return 0;
    case control_open_repository:
        open_repository();
        return 0;
    case control_results:
        if (notification == LBN_SELCHANGE) show_selected_preview(false);
        if (notification == LBN_DBLCLK) open_selected_result();
        return 0;
    case IDCANCEL:
        SendMessageW(application.window, WM_CLOSE, 0, 0);
        return 0;
    case IDHELP:
        MessageBoxW(
            application.window,
            L"DragonByteZ 0.6.23\r\n\r\n"
            L"Reverse-engineering and asset analysis for "
            L"Dragon Ball Z: The Legacy of Goku I, II, and Buu\'s Fury.\r\n\r\n"
            L"Special Thanks to Zeke Luna\r\n\r\n"
            L"https://github.com/spicybung/DragonByteZ\r\n\r\n"
            L"The program does not contain or distribute ROM data.",
            L"About DragonByteZ", MB_OK | MB_ICONINFORMATION);
        return 0;
    default:
        return 1;
    }
}

bool is_ds_button(int identifier) {
    switch (identifier) {
    case control_browse_rom:
    case control_browse_output:
    case control_analyze:
    case control_open_output:
    case control_open_selected:
    case control_open_contact_sheet:
    case control_open_sprite_gallery:
    case control_preview_selected:
        return true;
    default:
        return false;
    }
}

void draw_ds_button(const DRAWITEMSTRUCT& item) {
    RECT bounds = item.rcItem;
    const bool primary = item.CtlID == control_analyze;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const COLORREF fill = disabled
        ? RGB(54, 59, 65)
        : primary
            ? (pressed ? RGB(196, 78, 18) : RGB(244, 125, 31))
            : (pressed ? RGB(52, 59, 67) : RGB(69, 77, 87));
    const COLORREF border = primary
        ? RGB(255, 186, 92)
        : RGB(67, 192, 230);
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ old_brush = SelectObject(item.hDC, brush);
    HGDIOBJ old_pen = SelectObject(item.hDC, pen);
    RoundRect(
        item.hDC, bounds.left, bounds.top,
        bounds.right, bounds.bottom, 14, 14);

    if (!disabled) {
        RECT shine = bounds;
        shine.left += 7;
        shine.right -= 7;
        shine.top += 4;
        shine.bottom = shine.top + 2;
        HBRUSH shine_brush = CreateSolidBrush(
            primary ? RGB(255, 211, 145) : RGB(126, 205, 230));
        FillRect(item.hDC, &shine, shine_brush);
        DeleteObject(shine_brush);
    }

    SelectObject(item.hDC, old_pen);
    SelectObject(item.hDC, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);

    wchar_t label[96]{};
    GetWindowTextW(item.hwndItem, label, 96);
    HGDIOBJ old_font = SelectObject(item.hDC, application.section_font);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(
        item.hDC,
        disabled ? RGB(135, 142, 150) : RGB(247, 250, 252));
    if (pressed) OffsetRect(&bounds, 0, 1);
    DrawTextW(
        item.hDC, label, -1, &bounds,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(item.hDC, old_font);
    if ((item.itemState & ODS_FOCUS) != 0) {
        InflateRect(&bounds, -4, -4);
        DrawFocusRect(item.hDC, &bounds);
    }
}

void add_star_path(
    Gdiplus::GraphicsPath& path,
    float center_x,
    float center_y,
    float outer_radius,
    float inner_radius) {
    constexpr float pi = 3.14159265358979323846f;
    std::array<Gdiplus::PointF, 10> points{};
    for (std::size_t point = 0; point < points.size(); ++point) {
        const float radius = (point % 2U) == 0U
            ? outer_radius
            : inner_radius;
        const float angle =
            -pi * 0.5f + static_cast<float>(point) * pi / 5.0f;
        points[point] = Gdiplus::PointF(
            center_x + std::cos(angle) * radius,
            center_y + std::sin(angle) * radius);
    }
    path.AddPolygon(points.data(), static_cast<INT>(points.size()));
}

std::vector<Gdiplus::PointF> dragon_ball_star_positions(
    unsigned star_count,
    float center_x,
    float center_y,
    float spacing) {
    std::vector<Gdiplus::PointF> positions;
    positions.reserve(star_count);
    if (star_count == 1) {
        positions.emplace_back(center_x, center_y);
    } else if (star_count == 2) {
        positions.emplace_back(center_x - spacing, center_y);
        positions.emplace_back(center_x + spacing, center_y);
    } else if (star_count == 3) {
        positions.emplace_back(center_x, center_y - spacing);
        positions.emplace_back(center_x - spacing, center_y + spacing * 0.7f);
        positions.emplace_back(center_x + spacing, center_y + spacing * 0.7f);
    } else if (star_count == 4) {
        positions.emplace_back(center_x - spacing, center_y - spacing);
        positions.emplace_back(center_x + spacing, center_y - spacing);
        positions.emplace_back(center_x - spacing, center_y + spacing);
        positions.emplace_back(center_x + spacing, center_y + spacing);
    } else if (star_count == 5) {
        positions.emplace_back(center_x, center_y);
        positions.emplace_back(center_x - spacing, center_y - spacing);
        positions.emplace_back(center_x + spacing, center_y - spacing);
        positions.emplace_back(center_x - spacing, center_y + spacing);
        positions.emplace_back(center_x + spacing, center_y + spacing);
    } else if (star_count == 6) {
        for (int row = -1; row <= 1; row += 2) {
            for (int column = -1; column <= 1; ++column) {
                positions.emplace_back(
                    center_x + static_cast<float>(column) * spacing,
                    center_y + static_cast<float>(row) * spacing * 0.75f);
            }
        }
    } else {
        positions.emplace_back(center_x, center_y);
        constexpr float pi = 3.14159265358979323846f;
        for (unsigned index = 0; index < 6; ++index) {
            const float angle = static_cast<float>(index) * pi / 3.0f;
            positions.emplace_back(
                center_x + std::cos(angle) * spacing * 1.35f,
                center_y + std::sin(angle) * spacing * 1.35f);
        }
    }
    return positions;
}

void draw_dragon_balls(HDC device, const RECT&) {
    Gdiplus::Graphics graphics(device);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);

    constexpr float first_center_x = 282.0f;
    constexpr float center_y = 27.0f;
    constexpr float spacing_x = 39.0f;
    constexpr float radius = 15.5f;
    constexpr unsigned ticks_per_ball = 4;

    for (unsigned ball = 0; ball < 7; ++ball) {
        const int age = static_cast<int>(dragon_ball_tick) -
            static_cast<int>(ball * ticks_per_ball);
        if (age < 0) continue;

        float scale = 1.0f;
        if (age < 5) {
            const float amount = static_cast<float>(age + 1) / 5.0f;
            const float smooth = amount * amount * (3.0f - 2.0f * amount);
            scale = 0.20f + smooth * 0.92f;
            if (age >= 3) scale += 0.08f * (5.0f - static_cast<float>(age)) / 2.0f;
        }
        const float bob = std::sin(
            (static_cast<float>(dragon_ball_tick) + ball * 2.1f) * 0.16f) * 1.2f;
        const float center_x = first_center_x + ball * spacing_x;
        const float ball_y = center_y + bob;
        const float draw_radius = radius * scale;

        Gdiplus::SolidBrush glow(Gdiplus::Color(42, 255, 115, 15));
        graphics.FillEllipse(
            &glow,
            Gdiplus::RectF(
                center_x - draw_radius * 1.35f,
                ball_y - draw_radius * 1.35f,
                draw_radius * 2.7f,
                draw_radius * 2.7f));

        const Gdiplus::RectF outer_rect(
            center_x - draw_radius,
            ball_y - draw_radius,
            draw_radius * 2.0f,
            draw_radius * 2.0f);
        Gdiplus::SolidBrush outer(Gdiplus::Color(255, 225, 72, 7));
        graphics.FillEllipse(&outer, outer_rect);

        const Gdiplus::RectF inner_rect(
            center_x - draw_radius * 0.84f,
            ball_y - draw_radius * 0.84f,
            draw_radius * 1.68f,
            draw_radius * 1.68f);
        Gdiplus::SolidBrush inner(Gdiplus::Color(255, 255, 158, 23));
        graphics.FillEllipse(&inner, inner_rect);

        Gdiplus::SolidBrush shine(Gdiplus::Color(205, 255, 237, 178));
        graphics.FillEllipse(
            &shine,
            Gdiplus::RectF(
                center_x - draw_radius * 0.48f,
                ball_y - draw_radius * 0.59f,
                draw_radius * 0.55f,
                draw_radius * 0.33f));

        Gdiplus::Pen edge(Gdiplus::Color(255, 255, 205, 76), 1.2f);
        graphics.DrawEllipse(&edge, outer_rect);

        const float star_spacing = draw_radius * 0.30f;
        const float star_outer = std::max(1.2f, draw_radius * 0.105f);
        Gdiplus::SolidBrush star_brush(Gdiplus::Color(255, 198, 25, 10));
        for (const auto& star : dragon_ball_star_positions(
                 ball + 1U, center_x, ball_y, star_spacing)) {
            Gdiplus::GraphicsPath path;
            add_star_path(
                path, star.X, star.Y, star_outer, star_outer * 0.44f);
            graphics.FillPath(&star_brush, &path);
        }
    }
}

void draw_screen_bezel(HDC device, HWND control) {
    if (!control || !IsWindowVisible(control)) return;
    RECT bounds{};
    GetWindowRect(control, &bounds);
    MapWindowPoints(
        nullptr, application.window,
        reinterpret_cast<POINT*>(&bounds), 2);
    InflateRect(&bounds, 7, 7);
    HBRUSH bezel = CreateSolidBrush(RGB(12, 14, 17));
    HPEN edge = CreatePen(PS_SOLID, 2, RGB(77, 88, 100));
    HGDIOBJ old_brush = SelectObject(device, bezel);
    HGDIOBJ old_pen = SelectObject(device, edge);
    RoundRect(
        device, bounds.left, bounds.top,
        bounds.right, bounds.bottom, 18, 18);
    SelectObject(device, old_pen);
    SelectObject(device, old_brush);
    DeleteObject(edge);
    DeleteObject(bezel);
}

LRESULT CALLBACK window_procedure(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        application.window = window;
        create_menu(window);
        create_controls();
        DragAcceptFiles(window, TRUE);
        SetTimer(window, dragon_ball_timer_id, 100, nullptr);
        return 0;
    case WM_SIZE:
        layout_controls(LOWORD(lparam), HIWORD(lparam));
        return 0;
    case WM_TIMER:
        if (wparam == dragon_ball_timer_id) {
            ++dragon_ball_tick;
            RECT client{};
            GetClientRect(window, &client);
            RECT header = {0, 0, client.right, header_height};
            InvalidateRect(window, &header, FALSE);
            return 0;
        }
        break;
    case WM_COMMAND:
        if (handle_command(wparam) == 0) return 0;
        break;
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (item && is_ds_button(static_cast<int>(item->CtlID))) {
            draw_ds_button(*item);
            return TRUE;
        }
        if (item && item->CtlID == control_preview_image) {
            FillRect(item->hDC, &item->rcItem, application.panel_brush);
            if (application.preview_bitmap) {
                const UINT image_width = application.preview_bitmap->GetWidth();
                const UINT image_height = application.preview_bitmap->GetHeight();
                const int box_width = item->rcItem.right - item->rcItem.left;
                const int box_height = item->rcItem.bottom - item->rcItem.top;
                if (image_width > 0 && image_height > 0 &&
                    box_width > 0 && box_height > 0) {
                    const double scale = std::min(
                        static_cast<double>(box_width) / image_width,
                        static_cast<double>(box_height) / image_height);
                    const int draw_width =
                        std::max(1, static_cast<int>(image_width * scale));
                    const int draw_height =
                        std::max(1, static_cast<int>(image_height * scale));
                    const int x = item->rcItem.left + (box_width - draw_width) / 2;
                    const int y = item->rcItem.top + (box_height - draw_height) / 2;
                    Gdiplus::Graphics graphics(item->hDC);
                    graphics.SetInterpolationMode(
                        Gdiplus::InterpolationModeNearestNeighbor);
                    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
                    graphics.DrawImage(
                        application.preview_bitmap.get(), x, y,
                        draw_width, draw_height);
                }
            }
            return TRUE;
        }
        break;
    }
    case WM_DROPFILES: {
        const HDROP drop = reinterpret_cast<HDROP>(wparam);
        std::vector<wchar_t> path(32768, L'\0');
        if (DragQueryFileW(
                drop, 0, path.data(), static_cast<UINT>(path.size())) > 0) {
            inspect_rom(std::filesystem::path(path.data()));
        }
        DragFinish(drop);
        return 0;
    }
    case message_analysis_finished: {
        std::unique_ptr<std::wstring> output(
            reinterpret_cast<std::wstring*>(lparam));
        set_running(false);
        populate_results(*output);
        if (std::filesystem::is_regular_file(
                std::filesystem::path(*output) / L"graphics" /
                L"level_gallery.html") ||
            std::filesystem::is_regular_file(
                std::filesystem::path(*output) / L"graphics" /
                L"asset_gallery.html")) {
            open_contact_sheet();
        } else if (SendMessageW(application.results, LB_GETCOUNT, 0, 0) > 0) {
            SendMessageW(application.results, LB_SETCURSEL, 0, 0);
            show_selected_preview(false);
        }
        SetWindowTextW(
            application.status,
            L"Analysis finished. Select an asset to preview it here.");
        return 0;
    }
    case message_analysis_failed: {
        std::unique_ptr<std::wstring> error(
            reinterpret_cast<std::wstring*>(lparam));
        set_running(false);
        SetWindowTextW(application.status, L"Analysis failed.");
        show_error(*error);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* information = reinterpret_cast<MINMAXINFO*>(lparam);
        information->ptMinTrackSize.x = 900;
        information->ptMinTrackSize.y = 720;
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC device = reinterpret_cast<HDC>(wparam);
        const HWND control = reinterpret_cast<HWND>(lparam);
        const int identifier = GetDlgCtrlID(control);
        SetBkMode(device, TRANSPARENT);
        if (identifier == 2001 || identifier == 2006) {
            SetTextColor(
                device,
                identifier == 2001
                    ? RGB(255, 143, 44)
                    : RGB(83, 204, 247));
            return reinterpret_cast<LRESULT>(application.header_brush);
        }
        SetTextColor(device, RGB(226, 232, 238));
        if (control == application.rom_information ||
            control == application.level_information ||
            control == application.preview_text) {
            SetBkMode(device, OPAQUE);
            SetBkColor(device, RGB(39, 44, 51));
            return reinterpret_cast<LRESULT>(application.panel_brush);
        }
        if (identifier == 2002 || identifier == 2003) {
            return reinterpret_cast<LRESULT>(application.panel_brush);
        }
        return reinterpret_cast<LRESULT>(application.background_brush);
    }
    case WM_CTLCOLOREDIT: {
        HDC device = reinterpret_cast<HDC>(wparam);
        SetTextColor(device, RGB(226, 232, 238));
        SetBkColor(device, RGB(39, 44, 51));
        return reinterpret_cast<LRESULT>(application.panel_brush);
    }
    case WM_CTLCOLORLISTBOX: {
        HDC device = reinterpret_cast<HDC>(wparam);
        SetTextColor(device, RGB(226, 232, 238));
        SetBkColor(device, RGB(39, 44, 51));
        return reinterpret_cast<LRESULT>(application.panel_brush);
    }
    case WM_CTLCOLORBTN: {
        const HWND control = reinterpret_cast<HWND>(lparam);
        const int identifier = GetDlgCtrlID(control);
        HDC device = reinterpret_cast<HDC>(wparam);
        SetTextColor(device, RGB(226, 232, 238));
        SetBkMode(device, TRANSPARENT);
        if (identifier == control_graphics ||
            identifier == control_soundtrack) {
            return reinterpret_cast<LRESULT>(application.panel_brush);
        }
        return reinterpret_cast<LRESULT>(application.background_brush);
    }
    case WM_ERASEBKGND: {
        RECT client{};
        GetClientRect(window, &client);
        HDC device = reinterpret_cast<HDC>(wparam);
        FillRect(device, &client, application.background_brush);

        RECT header = {0, 0, client.right, header_height};
        FillRect(device, &header, application.header_brush);
        draw_dragon_balls(device, client);
        RECT accent = {0, header_height - 4, client.right, header_height - 1};
        FillRect(device, &accent, application.accent_brush);
        RECT blue_undertone = {
            0, header_height - 1, client.right, header_height};
        HBRUSH blue_brush = CreateSolidBrush(RGB(36, 183, 232));
        FillRect(device, &blue_undertone, blue_brush);
        DeleteObject(blue_brush);

        RECT setup_panel = {
            content_margin,
            setup_panel_top,
            std::max(
                content_margin + 1,
                static_cast<int>(client.right) - content_margin),
            setup_panel_top + setup_panel_height};
        HPEN panel_pen = CreatePen(PS_SOLID, 1, RGB(83, 94, 106));
        HGDIOBJ old_pen = SelectObject(device, panel_pen);
        HGDIOBJ old_brush = SelectObject(device, application.panel_brush);
        RoundRect(
            device, setup_panel.left, setup_panel.top,
            setup_panel.right, setup_panel.bottom, 18, 18);
        SelectObject(device, old_brush);
        SelectObject(device, old_pen);
        DeleteObject(panel_pen);
        draw_screen_bezel(device, application.results);
        draw_screen_bezel(
            device,
            IsWindowVisible(application.preview_image)
                ? application.preview_image
                : application.preview_text);
        return 1;
    }
    case WM_CLOSE:
        if (application.running) {
            const int answer = MessageBoxW(
                window,
                L"An analysis is still running. Exit DragonByteZ anyway?",
                application_title, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (answer != IDYES) return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, dragon_ball_timer_id);
        PlaySoundW(nullptr, nullptr, 0);
        application.preview_bitmap.reset();
        DeleteObject(application.normal_font);
        DeleteObject(application.title_font);
        DeleteObject(application.subtitle_font);
        DeleteObject(application.section_font);
        DeleteObject(application.background_brush);
        DeleteObject(application.panel_brush);
        DeleteObject(application.header_brush);
        DeleteObject(application.accent_brush);
        DeleteObject(application.accent_pressed_brush);
        DeleteObject(application.border_brush);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}


struct DragonByteZSplashDrop {
    float head_y = 0.0f;
    float speed = 7.0f;
    int trail = 10;
    uint32_t seed = 0;
};

std::vector<DragonByteZSplashDrop> splash_drops;
int splash_tick = 0;
int splash_cell = 20;
bool splash_skip_requested = false;
HFONT splash_rain_font = nullptr;
HFONT splash_logo_font = nullptr;
int splash_logo_font_width = 0;
int splash_logo_font_height = 0;
RECT splash_final_rectangle{};

float splash_palette_progress(float phase = 0.0f) {
    float value = std::clamp((float(splash_tick) - 72.0f) / 42.0f + phase, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
}

COLORREF splash_blend(COLORREF blue, COLORREF orange, float amount) {
    auto channel = [&](BYTE a, BYTE b) {
        return BYTE(std::clamp(
            int(std::lround(float(a) + (float(b) - float(a)) * amount)),
            0,
            255));
    };
    return RGB(
        channel(GetRValue(blue), GetRValue(orange)),
        channel(GetGValue(blue), GetGValue(orange)),
        channel(GetBValue(blue), GetBValue(orange)));
}

COLORREF splash_scale(COLORREF colour, float scale) {
    return RGB(
        BYTE(std::clamp(int(GetRValue(colour) * scale), 0, 255)),
        BYTE(std::clamp(int(GetGValue(colour) * scale), 0, 255)),
        BYTE(std::clamp(int(GetBValue(colour) * scale), 0, 255)));
}

uint32_t splash_hash(uint32_t value) {
    value ^= value >> 16;
    value *= 0x7FEB352Du;
    value ^= value >> 15;
    value *= 0x846CA68Bu;
    value ^= value >> 16;
    return value;
}

void reset_splash_drops(int width, int height) {
    const int columns = std::max(1, (width + splash_cell - 1) / splash_cell);
    splash_drops.resize(static_cast<std::size_t>(columns));
    for (int column = 0; column < columns; ++column) {
        const uint32_t random = splash_hash(
            0x4452425Au + uint32_t(column) * 0x9E3779B9u);
        DragonByteZSplashDrop& drop = splash_drops[static_cast<std::size_t>(column)];
        drop.seed = random;
        drop.head_y = -float(random % uint32_t(std::max(1, height + 320)));
        drop.speed = 5.0f + float((random >> 9) % 8u);
        drop.trail = 6 + int((random >> 17) % 9u);
    }
}

void draw_splash_matrix_rain(HDC device, const RECT& client) {
    static const wchar_t glyphs[] =
        L"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ<>[]{}+=*#:/\\|"
        L"ｱｲｳｴｵｶｷｸｹｺｻｼｽｾｿﾀﾁﾂﾃﾄﾅﾆﾇﾈﾉﾊﾋﾌﾍﾎﾏﾐﾑﾒﾓ";
    constexpr std::size_t glyph_count =
        (sizeof(glyphs) / sizeof(glyphs[0])) - 1u;

    if (!splash_rain_font) {
        splash_rain_font = CreateFontW(
            -17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    }
    HGDIOBJ old_font = SelectObject(device, splash_rain_font);
    SetBkMode(device, TRANSPARENT);

    const int glyph_tick = splash_tick / 2;
    for (std::size_t column = 0; column < splash_drops.size(); ++column) {
        const DragonByteZSplashDrop& drop = splash_drops[column];
        const float column_phase = splash_drops.size() > 1
            ? (float(column) / float(splash_drops.size() - 1u) - 0.5f) * 0.34f
            : 0.0f;
        const float palette = splash_palette_progress(column_phase);
        const COLORREF bright = splash_blend(
            RGB(214, 238, 255), RGB(255, 232, 192), palette);
        const COLORREF body = splash_blend(
            RGB(30, 132, 255), RGB(255, 118, 24), palette);
        const int x = static_cast<int>(column) * splash_cell;
        for (int trail_index = drop.trail; trail_index >= 0; --trail_index) {
            const int y = static_cast<int>(drop.head_y) - trail_index * splash_cell;
            if (y < -splash_cell || y >= client.bottom) continue;
            const uint32_t random = splash_hash(
                drop.seed ^ static_cast<uint32_t>(glyph_tick * 37 - trail_index * 101));
            const wchar_t glyph = glyphs[random % glyph_count];
            if (trail_index == 0) {
                SetTextColor(device, bright);
            } else {
                const int strength = 30 +
                    (drop.trail - trail_index) * 150 / std::max(1, drop.trail);
                SetTextColor(
                    device,
                    splash_scale(body, static_cast<float>(strength) / 210.0f));
            }
            TextOutW(device, x, y, &glyph, 1);
        }
    }

    SelectObject(device, old_font);
}

float splash_smooth(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
}

void add_rounded_rectangle(
    Gdiplus::GraphicsPath& path,
    const Gdiplus::RectF& rectangle,
    float radius) {
    const float diameter = radius * 2.0f;
    path.AddArc(rectangle.X, rectangle.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(
        rectangle.GetRight() - diameter,
        rectangle.Y,
        diameter,
        diameter,
        270.0f,
        90.0f);
    path.AddArc(
        rectangle.GetRight() - diameter,
        rectangle.GetBottom() - diameter,
        diameter,
        diameter,
        0.0f,
        90.0f);
    path.AddArc(
        rectangle.X,
        rectangle.GetBottom() - diameter,
        diameter,
        diameter,
        90.0f,
        90.0f);
    path.CloseFigure();
}

void draw_capsule_skill_intro(HDC device, const RECT& client) {
    if (splash_tick >= 56) return;

    Gdiplus::Graphics graphics(device);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    const float center_x = static_cast<float>(client.right) * 0.5f;
    const float center_y = static_cast<float>(client.bottom) * 0.44f;
    const float appear = splash_smooth(static_cast<float>(splash_tick) / 18.0f);
    const float move_y = static_cast<float>(client.bottom) + 84.0f +
        (center_y - static_cast<float>(client.bottom) - 84.0f) * appear;
    const float bounce = splash_tick >= 14 && splash_tick <= 24
        ? std::sin((static_cast<float>(splash_tick) - 14.0f) * 0.9f) * 11.0f
        : 0.0f;
    const float spin_progress = splash_smooth(static_cast<float>(splash_tick) / 24.0f);
    const float icon_scale = 0.18f + 1.18f * spin_progress;
    const float angle = (1.0f - spin_progress) * (1.0f - spin_progress) * 1080.0f +
        std::sin(static_cast<float>(splash_tick) * 0.32f) * 7.0f;
    const float icon_fade = 1.0f - splash_smooth((static_cast<float>(splash_tick) - 22.0f) / 7.0f);

    static std::unique_ptr<Gdiplus::Bitmap> capsule_bitmap;
    if (!capsule_bitmap) {
        if (HICON icon = load_dragonbytez_icon(GetModuleHandleW(nullptr), 128, 128)) {
            capsule_bitmap.reset(Gdiplus::Bitmap::FromHICON(icon));
        }
    }

    if (capsule_bitmap && icon_fade > 0.02f) {
        const float draw_size = 128.0f;
        Gdiplus::ImageAttributes attributes;
        Gdiplus::ColorMatrix matrix = {
            1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, icon_fade, 0.0f,
            0.0f, 0.0f, 0.0f, 0.0f, 1.0f
        };
        attributes.SetColorMatrix(&matrix);

        Gdiplus::Matrix original;
        graphics.GetTransform(&original);
        graphics.TranslateTransform(center_x, move_y + bounce);
        graphics.RotateTransform(angle);
        graphics.ScaleTransform(icon_scale, icon_scale);
        graphics.DrawImage(
            capsule_bitmap.get(),
            Gdiplus::RectF(-draw_size * 0.5f, -draw_size * 0.5f, draw_size, draw_size),
            0.0f,
            0.0f,
            static_cast<Gdiplus::REAL>(capsule_bitmap->GetWidth()),
            static_cast<Gdiplus::REAL>(capsule_bitmap->GetHeight()),
            Gdiplus::UnitPixel,
            &attributes);
        graphics.SetTransform(&original);
    }

    const float burst = splash_smooth((static_cast<float>(splash_tick) - 18.0f) / 4.0f);
    const float burst_fade = 1.0f - splash_smooth((static_cast<float>(splash_tick) - 33.0f) / 12.0f);
    if (burst > 0.0f && burst_fade > 0.01f) {
        const BYTE alpha = static_cast<BYTE>(
            std::clamp(static_cast<int>(burst_fade * 205.0f), 0, 205));
        for (int puff = 0; puff < 28; ++puff) {
            const uint32_t random = splash_hash(0xCA5E0000u + static_cast<uint32_t>(puff));
            const float angle_radians =
                (static_cast<float>(puff) / 28.0f) * 6.28318530718f +
                (static_cast<float>(random & 255u) / 255.0f) * 0.33f;
            const float radial = (38.0f + static_cast<float>((random >> 8) & 63u)) * burst;
            const float px = center_x + std::cos(angle_radians) * radial;
            const float py = center_y + std::sin(angle_radians) * radial * 0.58f;
            const float size =
                (18.0f + static_cast<float>((random >> 16) & 31u)) * (0.45f + burst * 0.90f);
            Gdiplus::SolidBrush cloud(
                Gdiplus::Color(alpha, 232, 238, 246));
            graphics.FillEllipse(
                &cloud,
                Gdiplus::RectF(px - size, py - size, size * 2.0f, size * 2.0f));
            Gdiplus::SolidBrush core(
                Gdiplus::Color(static_cast<BYTE>(alpha * 0.65f), 255, 255, 255));
            graphics.FillEllipse(
                &core,
                Gdiplus::RectF(px - size * 0.54f, py - size * 0.54f, size * 1.08f, size * 1.08f));
        }

        const float flash = 1.0f - splash_smooth((static_cast<float>(splash_tick) - 20.0f) / 6.0f);
        if (flash > 0.02f) {
            Gdiplus::SolidBrush glow(
                Gdiplus::Color(static_cast<BYTE>(flash * 145.0f), 255, 252, 244));
            graphics.FillEllipse(
                &glow,
                Gdiplus::RectF(center_x - 112.0f, center_y - 84.0f, 224.0f, 168.0f));
        }
    }
}

void draw_splash_panel(HDC device, const RECT& client) {
    if (splash_tick < 20) return;

    constexpr int initial_width = 96;
    constexpr int initial_height = 56;
    const float expansion = splash_smooth((float(splash_tick) - 20.0f) / 10.0f);
    const int final_width = std::max(1, int(client.right - client.left));
    const int final_height = std::max(1, int(client.bottom - client.top));
    const int current_width = std::max(
        initial_width,
        int(std::lround(
            float(initial_width) +
            (float(final_width) - float(initial_width)) * expansion)));
    const int current_height = std::max(
        initial_height,
        int(std::lround(
            float(initial_height) +
            (float(final_height) - float(initial_height)) * expansion)));
    const int center_x = (client.left + client.right) / 2;
    const int center_y = (client.top + client.bottom) / 2;
    const RECT panel{
        center_x - current_width / 2,
        center_y - current_height / 2,
        center_x + (current_width + 1) / 2,
        center_y + (current_height + 1) / 2};

    const float palette = splash_palette_progress();
    HBRUSH background = CreateSolidBrush(splash_blend(
        RGB(4, 8, 14), RGB(18, 9, 3), palette));
    HPEN border = CreatePen(
        PS_SOLID,
        2,
        splash_blend(RGB(32, 112, 224), RGB(255, 128, 32), palette));
    HGDIOBJ old_brush = SelectObject(device, background);
    HGDIOBJ old_pen = SelectObject(device, border);
    RoundRect(
        device,
        panel.left,
        panel.top,
        panel.right,
        panel.bottom,
        24,
        24);
    SelectObject(device, old_pen);
    SelectObject(device, old_brush);
    DeleteObject(border);
    DeleteObject(background);
}

void draw_splash_skip_hint(HDC device, const RECT& client) {
    HFONT font = CreateFontW(
        -15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ old_font = SelectObject(device, font);
    SetBkMode(device, TRANSPARENT);
    SetTextColor(device, RGB(150, 165, 181));
    RECT hint{12, client.bottom - 34, client.right - 12, client.bottom - 10};
    DrawTextW(
        device,
        L"Click or press any key to skip",
        -1,
        &hint,
        DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(device, old_font);
    DeleteObject(font);
}

const wchar_t* reigns_studios_block_logo() {
    return
        L" ▄▄▄▄▄▄▄       ▄▄▄▄  ▄▄▄     ▄▄▄▄▄    ▄▄▄   ▄▄▄    ▄▄▄▄▄▄▄        ▄▄▄▄▄▄▄  ▄▄▄▄▄▄▄▄▄  ▄▄▄   ▄▄▄ ▄▄▄▄▄▄▄     ▄▄▄     ▄▄▄       ▄▄▄▄▄▄▄\r\n"
        L"▐███▓▓▓▓▀▄  ▄██████▌▐███▌ ▄███████▌  ▐███▄ ▐███▌ ▄████████▌     ▄████████▌▐█████████▌▐███▌ ▐███▌▐████████▄ ▐███▌ ▄███████▄  ▄████████▌\r\n"
        L"▐░░░▌ ▐░░▐ ▐░░░█▀   ▐░░░▌▐░░░█▀      ▐░░░░▌▐░░░▌▐░░░▌ ▐░░░▌    ▐░░░▌ ▐░░░▌   ▐░░░▌   ▐░░░▌ ▐░░░▌▐░░░▌ ▐░░░▌▐░░░▌▐░░░▌ ▐░░░▌▐░░░▌ ▐░░░▌\r\n"
        L"▐▒▒▒▌  ▒▒▐ ▐▒▒▒▌    ▐▒▒▒▌▐▒▒▒▌       ▐▒▒▒▌▌▐▒▒▒▌▐▒▒▒▌ ▐▒▒▒▌    ▐▒▒▒▌ ▐▒▒▒▌   ▐▒▒▒▌   ▐▒▒▒▌ ▐▒▒▒▌▐▒▒▒▌ ▐▒▒▒▌▐▒▒▒▌▐▒▒▒▌ ▐▒▒▒▌▐▒▒▒▌ ▐▒▒▒▌\r\n"
        L"▐▓▓▓▌ ▐▓▓▌ ▐▓▓▓▓▓▓  ▐▓▓▓▌▐▓▓▓▌██████▌▐▓▓▓▌▐▐▓▓▓▌▐▓▓▓▌  ▀▀▀     ▐▓▓▓▌  ▀▀▀    ▐▓▓▓▌   ▐▓▓▓▌ ▐▓▓▓▌▐▓▓▓▌ ▐▓▓▓▌▐▓▓▓▌▐▓▓▓▌ ▐▓▓▓▌▐▓▓▓▌  ▀▀▀\r\n"
        L"▐███▌ ███  ▐████▀▀  ▐███▌▐███▌ ▐███▌ ▐███▌▐▌███▌ ▀███████▄      ▀███████▄    ▐███▌   ▐███▌ ▐███▌▐███▌ ▐███▌▐███▌▐███▌ ▐███▌ ▀███████▄\r\n"
        L"▐░░░▌▐░░▌  ▐░░░▌    ▐░░░▌▐░░░▌ ▐░░░▌ ▐░░░▌ ▌░░░▌ ▄▄▄  ▐░░░▌     ▄▄▄  ▐░░░▌   ▐░░░▌   ▐░░░▌ ▐░░░▌▐░░░▌ ▐░░░▌▐░░░▌▐░░░▌ ▐░░░▌ ▄▄▄  ▐░░░▌\r\n"
        L"▐▒▒▒▌▓▒█   ▐▒▒▒▌    ▐▒▒▒▌▐▒▒▒▌ ▐▒▒▒▌ ▐▒▒▒▌ ▐▒▒▒▌▐▒▒▒▌ ▐▒▒▒▌    ▐▒▒▒▌ ▐▒▒▒▌   ▐▒▒▒▌   ▐▒▒▒▌ ▐▒▒▒▌▐▒▒▒▌ ▐▒▒▒▌▐▒▒▒▌▐▒▒▒▌ ▐▒▒▒▌▐▒▒▒▌ ▐▒▒▒▌\r\n"
        L"▐▓▓▓▌▐▓▓█▄ ▐▓▓▓▓▄▄▄ ▐▓▓▓▌▐▓▓▓▓▄▓▓▓▓▓▌▐▓▓▓▌ ▐▓▓▓▌▐▓▓▓▌ ▐▓▓▓▌    ▐▓▓▓▌ ▐▓▓▓▌   ▐▓▓▓▌    ▌▓▓▓▄▓▓▓▐ ▐▓▓▓▌ ▐▓▓▓▌▐▓▓▓▌▐▓▓▓▌ ▐▓▓▓▌▐▓▓▓▌ ▐▓▓▓▌\r\n"
        L"▐███▌ ▀███▌ ▀▄█████▌▐███▌ ▀▄████████▌▐███▌ ▐███▌▐████████▀     ▐████████▀    ▐███▌    ▀▄█████▄▀ ▐████████▀ ▐███▌ ▀███████▀ ▐████████▀";
}

const wchar_t* dragonbytez_block_logo() {
    return
        L"██████╗ ██████╗  █████╗  ██████╗  ██████╗ ███╗   ██╗██████╗ ██╗   ██╗████████╗███████╗███████╗\r\n"
        L"██╔══██╗██╔══██╗██╔══██╗██╔════╝ ██╔═══██╗████╗  ██║██╔══██╗╚██╗ ██╔╝╚══██╔══╝██╔════╝╚══███╔╝\r\n"
        L"██║  ██║██████╔╝███████║██║  ███╗██║   ██║██╔██╗ ██║██████╔╝ ╚████╔╝    ██║   █████╗    ███╔╝ \r\n"
        L"██║  ██║██╔══██╗██╔══██║██║   ██║██║   ██║██║╚██╗██║██╔══██╗  ╚██╔╝     ██║   ██╔══╝   ███╔╝  \r\n"
        L"██████╔╝██║  ██║██║  ██║╚██████╔╝╚██████╔╝██║ ╚████║██████╔╝   ██║      ██║   ███████╗███████╗\r\n"
        L"╚═════╝ ╚═╝  ╚═╝╚═╝  ╚═╝ ╚═════╝  ╚═════╝ ╚═╝  ╚═══╝╚═════╝    ╚═╝      ╚═╝   ╚══════╝╚══════╝";
}

HICON load_dragonbytez_icon(HINSTANCE instance, int width, int height) {
    HICON icon = static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(application_icon_id),
        IMAGE_ICON,
        width,
        height,
        LR_DEFAULTCOLOR | LR_SHARED));
    if (icon) return icon;
    return LoadIconW(nullptr, IDI_APPLICATION);
}

void draw_ascii_logo(
    HDC device,
    const RECT& client,
    const wchar_t* logo_text,
    float materialize,
    float dissolve,
    int top_offset,
    int font_width,
    int font_height,
    uint32_t effect_seed) {
    materialize = std::clamp(materialize, 0.0f, 1.0f);
    dissolve = std::clamp(dissolve, 0.0f, 1.0f);
    const float visibility = std::clamp(materialize * (1.0f - dissolve), 0.0f, 1.0f);
    if (visibility <= 0.01f) return;

    HFONT font = CreateFontW(
        -font_height, font_width, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (!font) return;

    HGDIOBJ old_font = SelectObject(device, font);
    SetBkMode(device, TRANSPARENT);
    RECT logo{
        12,
        top_offset,
        client.right - 12,
        client.bottom - 84};

    const float palette = splash_palette_progress();
    const COLORREF logo_colour = splash_scale(
        splash_blend(RGB(48, 152, 255), RGB(255, 128, 32), palette), visibility);
    const COLORREF logo_shadow = splash_scale(
        splash_blend(RGB(0, 32, 96), RGB(92, 36, 0), palette), visibility);

    HRGN fragments = nullptr;
    if (materialize < 0.999f || dissolve > 0.001f) {
        constexpr int cell = 28;
        fragments = CreateRectRgn(0, 0, 0, 0);
        const int columns = std::max(1, (int(logo.right - logo.left) + cell - 1) / cell);
        const int rows = std::max(1, (int(logo.bottom - logo.top) + cell - 1) / cell);
        for (int row = 0; row < rows; ++row) {
            for (int column = 0; column < columns; ++column) {
                const uint32_t reveal_random = splash_hash(
                    effect_seed ^ uint32_t(row * 4099 + column * 131));
                const float reveal_threshold = float(reveal_random & 0xFFFFu) / 65535.0f;
                if (reveal_threshold > materialize) continue;

                const uint32_t dissolve_random = splash_hash(
                    (effect_seed ^ 0x9E3779B9u) ^ uint32_t(row * 8191 + column * 271));
                const float dissolve_threshold = float(dissolve_random & 0xFFFFu) / 65535.0f;
                if (dissolve_threshold < dissolve) continue;

                const LONG left = logo.left + LONG(column * cell);
                const LONG top = logo.top + LONG(row * cell);
                HRGN fragment = CreateRectRgn(
                    left, top,
                    std::min<LONG>(logo.right, left + cell),
                    std::min<LONG>(logo.bottom, top + cell));
                CombineRgn(fragments, fragments, fragment, RGN_OR);
                DeleteObject(fragment);
            }
        }
    }

    auto draw_layer = [&](RECT rectangle, COLORREF colour, int offset_x, int offset_y) {
        OffsetRect(&rectangle, offset_x, offset_y);
        const int saved = SaveDC(device);
        if (fragments) {
            if (offset_x || offset_y) OffsetRgn(fragments, offset_x, offset_y);
            ExtSelectClipRgn(device, fragments, RGN_AND);
        }
        SetTextColor(device, colour);
        DrawTextW(device, logo_text, -1, &rectangle,
                  DT_CENTER | DT_TOP | DT_NOPREFIX | DT_NOCLIP);
        RestoreDC(device, saved);
        if (fragments && (offset_x || offset_y)) OffsetRgn(fragments, -offset_x, -offset_y);
    };

    draw_layer(logo, logo_shadow, 2, 2);
    draw_layer(logo, logo_colour, 0, 0);
    if (fragments) DeleteObject(fragments);
    SelectObject(device, old_font);
    DeleteObject(font);
}

void draw_presents_text(HDC device, const RECT& client, float fade_in, float dissolve) {
    const float visibility = std::clamp(fade_in * (1.0f - dissolve), 0.0f, 1.0f);
    if (visibility <= 0.01f) return;
    HFONT font = CreateFontW(
        -30, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    if (!font) return;
    HGDIOBJ old_font = SelectObject(device, font);
    SetBkMode(device, TRANSPARENT);
    SetTextColor(device, splash_scale(RGB(220, 232, 255), visibility));
    RECT text{0, client.bottom / 2 + 82, client.right, client.bottom / 2 + 132};
    DrawTextW(device, L"PRESENTS...", -1, &text,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(device, old_font);
    DeleteObject(font);
}

void draw_intro_titles(HDC device, const RECT& client) {
    const float tick = float(splash_tick);
    const int width = int(client.right - client.left);

    const float reigns_in = splash_smooth((tick - 42.0f) / 18.0f);
    const float reigns_out = splash_smooth((tick - 87.0f) / 22.0f);
    draw_ascii_logo(
        device, client, reigns_studios_block_logo(), reigns_in, reigns_out,
        std::max(28, int(client.bottom) / 2 - 122),
        width < 1100 ? 5 : 6, width < 1100 ? 10 : 12, 0x51A70000u);

    const float presents_in = splash_smooth((tick - 62.0f) / 14.0f);
    const float presents_out = splash_smooth((tick - 106.0f) / 18.0f);
    draw_presents_text(device, client, presents_in, presents_out);

    const float dragon_in = splash_smooth((tick - 101.0f) / 22.0f);
    const float dragon_out = splash_smooth((tick - 214.0f) / 24.0f);
    draw_ascii_logo(
        device, client, dragonbytez_block_logo(), dragon_in, dragon_out,
        std::max(72, int(client.bottom) / 2 - 82),
        width < 1100 ? 6 : 8, width < 1100 ? 11 : 14, 0xD24A90B1u);
}

void start_intro_audio() {
    PlaySoundW(
        MAKEINTRESOURCEW(intro_audio_id),
        GetModuleHandleW(nullptr),
        SND_RESOURCE | SND_ASYNC | SND_NODEFAULT);
}

void stop_intro_audio() {
    PlaySoundW(nullptr, nullptr, 0);
}

LRESULT CALLBACK splash_window_procedure(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam) {
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_CREATE) {
        splash_tick = 0;
        splash_skip_requested = false;
        SetTimer(window, 1, 40, nullptr);
        return 0;
    }
    if (message == WM_SIZE) {
        reset_splash_drops(LOWORD(lparam), HIWORD(lparam));
        return 0;
    }
    if (message == WM_TIMER && wparam == 1) {
        ++splash_tick;
        RECT client{};
        GetClientRect(window, &client);
        for (DragonByteZSplashDrop& drop : splash_drops) {
            drop.head_y += drop.speed;
            if (drop.head_y - float(drop.trail * splash_cell) > float(client.bottom)) {
                const uint32_t random = splash_hash(
                    drop.seed ^ uint32_t(splash_tick * 0x9E37));
                drop.head_y = -float(40u + random % 360u);
                drop.speed = 5.0f + float((random >> 8) % 9u);
                drop.trail = 7 + int((random >> 18) % 15u);
            }
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    if (message == WM_LBUTTONDOWN || message == WM_KEYDOWN) {
        splash_skip_requested = true;
        return 0;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC device = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        const int width = static_cast<int>(std::max<LONG>(1, client.right - client.left));
        const int height = static_cast<int>(std::max<LONG>(1, client.bottom - client.top));
        HDC memory_device = CreateCompatibleDC(device);
        HBITMAP frame = CreateCompatibleBitmap(device, width, height);
        HGDIOBJ old_bitmap = SelectObject(memory_device, frame);
        HBRUSH transparent_background = CreateSolidBrush(splash_transparency_key);
        FillRect(memory_device, &client, transparent_background);
        DeleteObject(transparent_background);

        draw_splash_panel(memory_device, client);
        if (splash_tick >= 58) {
            draw_splash_matrix_rain(memory_device, client);
            draw_intro_titles(memory_device, client);
        }
        draw_capsule_skill_intro(memory_device, client);
        if (splash_tick >= 34) {
            draw_splash_skip_hint(memory_device, client);
        }

        BitBlt(device, 0, 0, width, height, memory_device, 0, 0, SRCCOPY);
        SelectObject(memory_device, old_bitmap);
        DeleteObject(frame);
        DeleteDC(memory_device);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_DESTROY) {
        KillTimer(window, 1);
        if (splash_rain_font) {
            DeleteObject(splash_rain_font);
            splash_rain_font = nullptr;
        }
        if (splash_logo_font) {
            DeleteObject(splash_logo_font);
            splash_logo_font = nullptr;
            splash_logo_font_width = 0;
            splash_logo_font_height = 0;
        }
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void pump_splash_messages(HWND splash) {
    MSG message{};
    while (PeekMessageW(&message, splash, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

HWND create_dragonbytez_splash(HINSTANCE instance, ULONGLONG& started_at) {
    WNDCLASSEXW splash_class{};
    splash_class.cbSize = sizeof(splash_class);
    splash_class.style = CS_DROPSHADOW;
    splash_class.lpfnWndProc = splash_window_procedure;
    splash_class.hInstance = instance;
    splash_class.hIcon = load_dragonbytez_icon(instance, 32, 32);
    splash_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    splash_class.lpszClassName = splash_class_name;
    RegisterClassExW(&splash_class);

    RECT work_area{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    const int available_width = static_cast<int>(
        std::max<LONG>(640, work_area.right - work_area.left));
    const int available_height = static_cast<int>(
        std::max<LONG>(420, work_area.bottom - work_area.top));
    const int width = std::min(1320, available_width - 64);
    const int height = std::min(600, available_height - 64);
    const int x = work_area.left + (available_width - width) / 2;
    const int y = work_area.top + (available_height - height) / 2;
    splash_final_rectangle = RECT{x, y, x + width, y + height};

    HWND splash = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        splash_class.lpszClassName,
        L"DragonByteZ",
        WS_POPUP,
        x,
        y,
        width,
        height,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!splash) return nullptr;

    SetWindowRgn(
        splash,
        CreateRoundRectRgn(0, 0, width + 1, height + 1, 26, 26),
        TRUE);
    SetLayeredWindowAttributes(
        splash,
        splash_transparency_key,
        255,
        LWA_COLORKEY | LWA_ALPHA);
    ShowWindow(splash, SW_SHOW);
    SetForegroundWindow(splash);
    SetFocus(splash);
    UpdateWindow(splash);
    start_intro_audio();
    started_at = GetTickCount64();
    return splash;
}

void finish_dragonbytez_splash(HWND splash, ULONGLONG started_at) {
    if (!splash) return;
    while (!splash_skip_requested && splash_tick < 250 &&
           GetTickCount64() - started_at < 10000u) {
        pump_splash_messages(splash);
        Sleep(10);
    }
    stop_intro_audio();
    KillTimer(splash, 1);
    for (int alpha = 255; alpha >= 0; alpha -= 17) {
        SetLayeredWindowAttributes(
            splash,
            splash_transparency_key,
            BYTE(alpha),
            LWA_COLORKEY | LWA_ALPHA);
        pump_splash_messages(splash);
        Sleep(8);
    }
    DestroyWindow(splash);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    INITCOMMONCONTROLSEX common_controls{};
    common_controls.dwSize = sizeof(common_controls);
    common_controls.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&common_controls);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput gdiplus_input;
    if (Gdiplus::GdiplusStartup(
            &gdiplus_token, &gdiplus_input, nullptr) != Gdiplus::Ok) {
        CoUninitialize();
        return 1;
    }

    const int large_icon_width = GetSystemMetrics(SM_CXICON);
    const int large_icon_height = GetSystemMetrics(SM_CYICON);
    const int small_icon_width = GetSystemMetrics(SM_CXSMICON);
    const int small_icon_height = GetSystemMetrics(SM_CYSMICON);
    HICON large_icon = load_dragonbytez_icon(instance, large_icon_width, large_icon_height);
    HICON small_icon = load_dragonbytez_icon(instance, small_icon_width, small_icon_height);

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_procedure;
    window_class.hInstance = instance;
    window_class.hIcon = large_icon;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground =
        reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = window_class_name;
    window_class.hIconSm = small_icon;
    if (!RegisterClassExW(&window_class)) {
        Gdiplus::GdiplusShutdown(gdiplus_token);
        CoUninitialize();
        return 1;
    }

    ULONGLONG splash_started_at = 0;
    HWND splash = create_dragonbytez_splash(instance, splash_started_at);

    HWND window = CreateWindowExW(
        0,
        window_class_name,
        application_title,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1100,
        760,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!window) {
        Gdiplus::GdiplusShutdown(gdiplus_token);
        CoUninitialize();
        return 1;
    }

    SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(large_icon));
    SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
    SendMessageW(window, WM_SETICON, ICON_SMALL2, reinterpret_cast<LPARAM>(small_icon));

    finish_dragonbytez_splash(splash, splash_started_at);

    ShowWindow(window, show_command);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    Gdiplus::GdiplusShutdown(gdiplus_token);
    CoUninitialize();
    return static_cast<int>(message.wParam);
}

#endif
