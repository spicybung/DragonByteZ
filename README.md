# DragonByteZ

DragonByteZ is a reverse-engineering tool currently compatible for the Game Boy Advance Webfoot Dragon Ball Z games:

- *Dragon Ball Z: The Legacy of Goku* Europe Rev 0 (`ALGP`)
- *Dragon Ball Z: The Legacy of Goku II* Europe Rev 0 (`ALFP`)
- *Dragon Ball Z: The Legacy of Goku II* USA Rev 0 (`ALFE`)
- *Dragon Ball Z: Buu's Fury* USA Rev 0 (`BG3E`)

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
