# Building

The native client overlay currently targets Windows x64 and requires:

- A C++23-capable Visual Studio toolchain and CMake 3.22 or newer.
- A UE4SS import library compatible with the deployed UE4SS build.
- Dear ImGui at the tested `v1.92.1` revision.
- Kiero commit `5539f06a03154ad3c30702792f88a220ac54958d` with its MinHook
  submodule at `8fda4f5481fed5797dc2651cd91e238e9b3928c6`.

Configure `PIRATE_SIGNALS_DEPS_DIR` to a directory containing `imgui` and
`kiero` checkouts, and set `UE4SS_IMPORT_LIB` to the compatible `UE4SS.lib`.
If the dependency checkouts do not share a parent, set `IMGUI_DIR` and
`KIERO_DIR` separately instead:

```powershell
cmake -S src/native -B build/native -A x64 `
  -DPIRATE_SIGNALS_DEPS_DIR=C:\path\to\dependencies `
  -DUE4SS_IMPORT_LIB=C:\path\to\UE4SS.lib
cmake --build build/native --config Release
```

The resulting `main.dll` belongs at `PirateSignals/dlls/main.dll` in the
client UE4SS mod folder.

The LogicMods `.pak`, `.utoc`, and `.ucas` files are cooked Unreal artifacts.
They require a compatible Windrose/Unreal development environment and are not
reproducible in a generic GitHub Actions runner. They should be distributed as
versioned release artifacts with SHA-256 hashes, not committed to source.
