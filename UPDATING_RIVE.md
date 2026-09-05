# Updating Rive Runtime in CasparFork (AirPlayEngine)

This document provides a detailed, step-by-step methodology for updating the `rive-runtime` dependency from the upstream repository. Because we maintain custom patches for real-time playout and dynamic data binding, updating the runtime cannot be done with a blind overwrite. Follow this guide to ensure that the engine continues to compile and function correctly.

## 1. Upstream Source and Syncing
The core C++ runtime is sourced from Rive's official repository:
- **Upstream Repository**: `https://github.com/rive-app/rive-runtime.git`
- **Branch**: Typically `master` or a specific release tag.

### Safe Sync Process
When copying over new files from upstream, you **must preserve** our CasparFork-specific build configurations and documentation. 
1. **Clone the upstream repo** into a temporary directory outside the project.
2. **Copy the upstream files** over the `rive-runtime` directory in this repository.
3. **DO NOT OVERWRITE OR DELETE** the following custom files/directories in our `rive-runtime` folder:
   - `CMakeLists.txt` (Our custom CMake build script integration)
   - `CUSTOM_PATCHES.md` (Documentation of our required C++ logic changes)
   - `UPDATING_RIVE.md` (This document)
   - Any pre-compiled static libraries or renderer caches (e.g., `dependencies/` if present).
4. **Force a full recompile of the rive tree after copying — this is MANDATORY, not optional.**
   File-copy tools (`robocopy`, `xcopy`, Explorer copy, `Copy-Item`) usually **preserve the upstream files' modification times**, which are often *older* than the object files already in `build/`. MSBuild/Ninja decide what to rebuild by comparing mtimes, so they will see the "new" headers as older than the existing `.obj` files and **silently skip recompiling** — even though the header *content* changed. Only files you hand-edit afterward (e.g. re-applying a patch) get recompiled.
   The result is an **ABI Frankenstein**: a few objects built against the new struct/vtable layouts, the rest still against the old ones, linked into one library. It links cleanly and then **crashes at runtime** (e.g. the render thread dies silently after a `.riv` loads and binds). This exact failure shipped in the v0.1.230 update.
   To prevent it, after copying, bump every rive source/header mtime to "now" and rebuild, or delete the rive build outputs:
   ```bash
   # from repo root — force all rive_core + rive-module objects to be rebuilt
   find rive-runtime/src rive-runtime/include -type f \
     \( -name '*.cpp' -o -name '*.hpp' -o -name '*.c' -o -name '*.h' \) -exec touch {} +
   cmake --build build --config Release --target rive_core rive
   ```
   After the build, sanity-check that it actually happened: `find build/modules/rive/rive-runtime -iname '*.obj' -printf '%TY-%Tm-%Td\n' | sort | uniq -c` should show **all** objects dated today, not a single fresh one among hundreds of stale ones.

## 2. Re-applying Custom C++ Patches
The upstream runtime may not natively support some of the dynamic layout injection (DataBinding) and performance requirements of our playout engine. 

> [!WARNING]
> **Do not blindly inject these patches.** Before applying any changes, always inspect the freshly synced upstream C++ files to verify if the Rive team has already addressed the issue natively. Only re-apply the patch if the upstream code still lacks the fix.

After syncing the code and verifying the upstream state, consult `CUSTOM_PATCHES.md` and manually re-apply our custom C++ patches if necessary. Historically, these include:
1. **Stale Layout Cache Fallback**: Modifying `LayoutComponent::propagateSizeToChildren` in `src/layout_component.cpp` to use the authored deterministic dimensions when Yoga hasn't completed its layout pass yet.
2. **Group Node Recursion Blocker**: Removing `if (child->coreType() == NodeBase::typeKey)` inside `src/layout_component.cpp` so that layouts correctly propagate sizes into `Group` nodes.
3. **Image Scaling Skip-Guard**: Removing the `m_layoutWidth != size.x` early-exit guard in `Image::controlSize()` in `src/shapes/image.cpp` so that asynchronously loaded dynamic images are always resized upon loading.
4. **Diagnostic Log Latency Fix**: Ensuring that high-latency synchronous `fprintf(stderr, "[RIVE DIAG]...")` calls are removed from the render loops to prevent broadcast frame drops.

*Note: Watch out for Windows SDK macro collisions! Specifically, the `fixed` macro defined in Windows headers (like `rpcndr.h`) will collide with `LayoutScaleType::fixed` in `layout_component.cpp`. Always wrap the affected code blocks with `#ifdef fixed / #undef fixed / #endif`.*

## 3. Resolving Dependencies and Shader Codegen
A clean CMake build of CasparFork will often fail during the Rive runtime linkage or dependency fetching due to private repository locks or missing target generations. 

Before configuring CMake, verify the following:
1. **Shader Codegen**:
   The `rive_renderer` is compiled from source in our CMake build to guarantee ABI parity with `rive_core` (specifically sharing the `WITH_RIVE_TEXT` and `WITH_RIVE_LAYOUT` defines). However, the generated D3D shader headers must be built beforehand using the Python minifier.
   - Run the shader generation step (requires `pip install ply`):
     ```bash
     make -C rive-runtime/renderer/src/shaders -j8 \
          OUT=../../../../../build/modules/rive/rive-runtime/include/generated/shaders \
          FLAGS="" d3d
     ```
2. **External Cached Archives (TBB, SFML, CEF)**:
   If you wipe the `build/` directory, CMake will attempt to download dependencies (like TBB) from private GitHub mirrors and fail. 
   - Bypass this by manually copying the `.zip` and `.tar.gz` archives from `dist_\external\` into `build\external\` before running the CMake configuration step.
3. **Scripting dependencies (Luau + libhydrogen)** — see §4 below. These are NOT vendored in the rive-runtime tarball and must be cloned into `dependencies/` before configuring, or the `luau_vm` target and every `src/lua/**` source will fail to compile.

## 4. Rive Scripting (Luau)
Some templates implement their behavior as **Rive Scripts** (Luau) rather than pure state machines/data binding — for example the scrolling-ticker `TickerNode` script that reads `tickerItems`/`speed`/`gap` and animates the scroll. Our build enables `WITH_RIVE_SCRIPTING`. If scripting is disabled, every `scripted/` object compiles to an **empty stub**: script-driven templates populate their data but never animate, and assets the script would decode (e.g. an embedded logo) fail to load. This is why such templates "work in Flutter but not in the engine."

### 4.1 Fetch the scripting dependencies
These are pulled from GitHub by Rive's own build (`scripting/premake5.lua`) and are **not** in the rive-runtime source drop. Clone them into `dependencies/`:
```bash
# from rive-runtime/dependencies
git clone --depth 1 -b rive_0_734 https://github.com/luigi-rosso/luau.git        luau
git clone --depth 1 -b rive_0_2   https://github.com/luigi-rosso/libhydrogen.git libhydrogen
```
> [!IMPORTANT]
> The refs above (`rive_0_734`, `rive_0_2`) are pins for this runtime version. **After every rive-runtime sync, re-read `scripting/premake5.lua`** and match the exact refs — Rive bumps them periodically:
> `grep "dependency.github('luigi-rosso" scripting/premake5.lua`

### 4.2 CMake wiring (already in `CMakeLists.txt`)
- A `luau_vm` static-lib target compiles **only** `luau/VM/src` (defines `LUA_USE_LONGJMP`, `RIVE_LUAU`, `/fp:precise`, force-includes `scripting/rive_luau.hpp`). The Luau **Compiler/Ast are intentionally NOT built** — templates are exported with pre-compiled bytecode ("force export" in the editor), so the VM alone runs them.
- `src/lua/**` is no longer excluded from `rive_core`; `libhydrogen.c` is compiled into `rive_core` with `HYDRO_SIGN_VERIFY_ONLY=1`.
- `rive_core` links `luau_vm` and adds the Luau/`libhydrogen`/`scripting`/`renderer/include` include paths.

> [!WARNING]
> **ABI:** `WITH_RIVE_SCRIPTING` adds `m_scriptingVM` to `Artboard` and `File` in the **public** headers (`include/rive/artboard.hpp`, `include/rive/file.hpp`). It must be defined **identically** on `rive_core`, `rive_renderer`, **and** the `rive` module (it propagates to the module via `rive_full`'s PUBLIC defines). A mismatch changes `Artboard`/`File` struct sizes and produces the same silent stack-corruption crash as the renderer ABI bug. Keep it PUBLIC on both libs.

### 4.3 Verify
Play a script-driven template (the scrolling ticker). It should **scroll**, not just populate. If the data appears but nothing animates, scripting linked as stubs — recheck `WITH_RIVE_SCRIPTING` and the `luau_vm` link.

## 5. Building the Playout Engine
Once the C++ patches are applied and the dependency archives are restored, you can build the main execution target.

Run the following from the root `CasparFork` directory:

```powershell
# 1. Build the main executable and link the Rive runtime
cmake --build build --config Release --target airplayengine -- /maxcpucount

# 2. Copy all dynamic libraries (DLLs) and CEF resources to the output folder
cmake --build build --config Release --target airplayengine_copy_dependencies -- /maxcpucount
```

## 6. Verification
After the build completes, verify the output directory (`build\shell\Release\`):
- Ensure `airZstudio.exe` has been generated.
- Ensure all FFmpeg (`avcodec-61.dll`), SFML (`sfml-graphics-2.dll`), TBB (`tbb12.dll`), and Chromium CEF (`libcef.dll`, `chrome_elf.dll`, `resources.pak`) files are present alongside the executable.
- Run `airZstudio.exe` to test if dynamic `.riv` files load correctly without layout scaling bugs or recursive Group node traversal failures.
