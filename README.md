# DragonByteZ

DragonByteZ is a C++17 Windows desktop and command-line reverse-engineering tool
for the Game Boy Advance Webfoot Dragon Ball Z games:

- *Dragon Ball Z: The Legacy of Goku* Europe Rev 0 (`ALGP`)
- *Dragon Ball Z: The Legacy of Goku II* Europe Rev 0 (`ALFP`)
- *Dragon Ball Z: The Legacy of Goku II* USA Rev 0 (`ALFE`)
- *Dragon Ball Z: Buu's Fury* USA Rev 0 (`BG3E`)

Repository: https://github.com/spicybung/DragonByteZ

Special Thanks to Zeke Luna.

## DragonByteZ 0.6.23

### Legacy of Goku

- Exports 109 transparent OBJ/OAM animation sheets from the hidden Sprite Viewer.
- Full-screen menu captures and character artwork are no longer presented as
  sprites.
- Samples the live preview animation, crops transparent bounds, and removes
  duplicate frames before composing each sheet.
- Exports 18 distinct sound-test selections as cached 12-second 44.1 kHz stereo
  WAV previews.
- Does not generate sequence BIN, GSF, or miniGSF files during normal analysis.

### Legacy of Goku II

- Decodes the authoritative 150-entry sprite-ID table rather than scanning an
  arbitrary subset of compressed blocks.
- Exports sprite IDs 7 through 156, 898 animations, and 13,336 four-direction
  frames from the supplied Europe Rev 0 ROM.
- Each frame is reconstructed from its sprite structure, animation sequence,
  12-byte frame record, Webfoot-compressed 8bpp pixels, OBJ flips, and RGB555
  object palette. Frames are not arranged using guessed tile-grid widths.
- Retains the 168 level atlases, 327 map records, rebuilt local tilesets, map
  layers, composites, level gallery, WAV samples, and 44 BGM previews.

### Buu's Fury

- Directly decodes the character pointer table into 295 non-null structures,
  1,562 animations, and 20,768 directional frames for the supplied USA ROM.
- Retains the 452 map entries, 187 global tile atlases, rebuilt local tilesets,
  map previews, and 53 BGM WAV previews.

## Output

### Legacy of Goku graphics

```text
graphics/
  sprites_gallery.html
  sprites.csv
  LOG1_level_structure_status.txt
  sprites/
    sprite_000.png
    ...
    sprite_108.png
```

### Music

```text
soundtrack/
  tracks.csv
  DragonByteZ_LOG1.m3u
  tracks/
    track_00.wav
    ...
```

LOG2 and Buu's Fury place their tracks under `soundtrack/level_music/tracks/`.
Batch BGM WAV files are twelve-second playable previews and are cached between
analyses. Soundtrack analysis is unchecked by default in the GUI because audio
emulation is slower than graphics extraction.

## Windows GUI

The native Win32 GUI provides ROM browsing and drag-and-drop, graphics and music
analysis, recursive result browsing, PNG/CSV/TXT/WAV previews, searchable level
and sprite galleries, seven animated Dragon Balls beside the DragonByteZ title,
and a skippable startup sequence. The startup uses the application capsule icon,
which starts small, spins, grows, produces a cloud burst, and transitions into
the Reigns Studios intro. The intro remains limited to ten seconds and preserves its final fade-out.

## Build

### Windows

Run `build_windows.bat` from the project folder or a Visual Studio 2022
Developer Command Prompt:

```text
build-windows\Release\DragonByteZ.exe
build-windows\Release\dragonbytez-cli.exe
```

DragonByteZ source is compiled with `/W4 /WX /permissive- /utf-8`. Third-party
GBA-engine warnings are isolated from the project's warning-as-error policy.

### Linux command-line build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## CLI

```sh
./dragonbytez-cli header game.gba
./dragonbytez-cli analyze game.gba -o analysis
./dragonbytez-cli graphics game.gba -o graphics
./dragonbytez-cli soundtrack game.gba -o soundtrack
./dragonbytez-cli decompress game.gba 0x69C570 -o output.bin
```

The explicit `decompress` command can still write a user-requested raw file. The
normal game analysis and soundtrack workflows do not emit BIN files.

DragonByteZ does not contain or redistribute ROM data.
