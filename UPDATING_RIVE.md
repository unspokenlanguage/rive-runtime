# Updating Rive Runtime in CasparFork (AirPlayEngine)

This document provides a detailed, step-by-step methodology for updating the `rive-runtime` dependency from the upstream repository. Because we maintain custom patches for real-time playout and dynamic data binding, updating the runtime cannot be done with a blind overwrite. Follow this guide to ensure that the engine continues to compile and function correctly.

## 1. Upstream Source and Syncing
The core C++ runtime is sourced from Rive's official repository:
- **Upstream Repository**: `https://github.com/rive-app/rive-runtime.git`
- **Target**: a `runtime-v0.1.N` tag — Rive tags every commit on `main`, so the tip is always a tagged release. Record both tag names in the sync log.

### Sync process — git 3-way merge in the nested workbench (replaces the old blind file-copy)
`rive-runtime/` is tracked by the **parent** repo as plain files: that is the source of truth and the rollback point. Inside it, an *unregistered* nested `.git` pointing at `https://github.com/rive-app/rive-runtime.git` is our **merge workbench** — it holds full upstream history so git can 3-way-merge our patches instead of us re-deriving them by hand. Never register it as a submodule; the parent must stay self-contained.

1. **Rollback point first** — tag the parent and push it:
   `git tag -a rive-pre-sync-YYYY-MM-DD -m "..." && git push origin rive-pre-sync-YYYY-MM-DD`
2. **Find the current baseline** (the upstream commit our copy was vendored from). `.rive_head` is Rive's *monorepo* SHA, not a runtime commit — but it is a fingerprint:
   ```bash
   cd rive-runtime && git fetch origin --tags
   git log --all --format='%h %ad %s' --date=short -S"$(tr -d '\r\n' < .rive_head)" -- .rive_head   # oldest hit = baseline
   git describe --tags <baseline>
   ```
3. **Make our delta one commit on the baseline:**
   ```bash
   git reset --mixed <baseline>                                          # HEAD+index = baseline; worktree untouched
   git add --renormalize -- . ':(exclude)tests/android_tests/.gradle'   # clears CRLF stat churn; stages ONLY real patches
   git diff --cached --stat                                              # must be just the patched files (4 in 2026-09)
   git checkout -b airz/patches-on-<baseline>
   git add CMakeLists.txt CUSTOM_PATCHES.md UPDATING_RIVE.md && git commit
   ```
   Put `dependencies/*/` and `**/.gradle/` in `.git/info/exclude` — the vendored dependency trees are the parent's job.
4. **Rebase onto the new upstream tag:**
   ```bash
   git checkout -b airz/merge-<tag> airz/patches-on-<baseline>
   git rebase --onto runtime-v0.1.N <baseline>
   ```
   Only files upstream touched *and* we patched will conflict. **Rebase gotcha:** `--ours` = upstream (the branch rebased onto), `--theirs` = our patch. For each conflict, first check whether upstream now contains the fix (`git show runtime-v0.1.N:<file>`); take upstream where it does, re-apply narrowly where it doesn't, and record the verdict in `CUSTOM_PATCHES.md` → **ADDRESSED**. A patch that applies *cleanly* can still be one you intended to drop — check the guard-rail.
5. **Guard-rail:** `git diff runtime-v0.1.N..HEAD --stat` must list only the intended patched files + `CMakeLists.txt` + the two `.md`. Anything else is a leaked resolution.
6. Then §3 (dependency pins + shader codegen), §5 (build — the touch-all step is **mandatory**), §6 (verify), and finally commit to the parent (§6).

> The old process — clone upstream to a temp dir and copy files over, then re-apply every patch by hand — is retired. It could not see what upstream changed *in our patched files*, and it re-derived patches from a doc that turned out to be wrong (see Patch 5).

## 2. Re-applying Custom C++ Patches
The upstream runtime may not natively support some of the dynamic layout injection (DataBinding) and performance requirements of our playout engine. 

> [!WARNING]
> **Do not blindly inject these patches.** Before applying any changes, always inspect the freshly synced upstream C++ files to verify if the Rive team has already addressed the issue natively. Only re-apply the patch if the upstream code still lacks the fix.
> Record every verdict in `CUSTOM_PATCHES.md` → **ADDRESSED** and its sync log.

After syncing the code and verifying the upstream state, consult `CUSTOM_PATCHES.md` and manually re-apply our custom C++ patches if necessary. Historically, these include:
1. **Stale Layout Cache Fallback**: Modifying `LayoutComponent::propagateSizeToChildren` in `src/layout_component.cpp` to use the authored deterministic dimensions when Yoga hasn't completed its layout pass yet.
2. **Group Node Recursion Blocker**: Removing `if (child->coreType() == NodeBase::typeKey)` inside `src/layout_component.cpp` so that layouts correctly propagate sizes into `Group` nodes.
3. **Image Scaling Skip-Guard**: Removing the `m_layoutWidth != size.x` early-exit guard in `Image::controlSize()` in `src/shapes/image.cpp` so that asynchronously loaded dynamic images are always resized upon loading.
4. **Diagnostic Log Latency Fix**: Ensuring that high-latency synchronous `fprintf(stderr, "[RIVE DIAG]...")` calls are removed from the render loops to prevent broadcast frame drops.

> **Status after the 2026-09 sync (`runtime-v0.1.359`):** patches 1–3 are deliberately **dropped** for a test-first build (upstream restructured `propagateSizeToChildren`; verdict pending the dynamic-image-inside-a-Group test); patch 4 is **retired** (never upstream). Patches **5 and 6** (nested state-machine default / empty-animation fallback) **are required** and were re-applied. See `CUSTOM_PATCHES.md` → ADDRESSED.

*Note: Watch out for Windows SDK macro collisions! Specifically, the `fixed` macro defined in Windows headers (like `rpcndr.h`) will collide with `LayoutScaleType::fixed` in `layout_component.cpp`. Always wrap the affected code blocks with `#ifdef fixed / #undef fixed / #endif`.*

## 3. Resolving Dependencies and Shader Codegen
A clean CMake build of CasparFork will often fail during the Rive runtime linkage or dependency fetching due to private repository locks or missing target generations. 

Before configuring CMake, verify the following:
1. **Shader Codegen** — regenerate whenever upstream's `renderer/src/shaders/` changed (it did in 2026-09, including a renamed shader):
   `rive_renderer` is compiled from source in our CMake build to guarantee ABI parity with `rive_core` (sharing the `WITH_RIVE_TEXT` / `WITH_RIVE_LAYOUT` / `WITH_RIVE_SCRIPTING*` defines), but the generated D3D shader headers must exist **before** CMake configures (the `RIVE_RENDERER_GENERATED` glob). With GNU `make` available (`pip install ply` first):
   ```bash
   make -C rive-runtime/renderer/src/shaders -j8 \
        OUT=../../../../../build/modules/rive/rive-runtime/include/generated/shaders \
        FLAGS="" d3d
   ```
   **This Windows setup has no `make`.** The `d3d` target is only `minify.py` + `fxc` (Windows Kits `x64/fxc.exe`); the exact working replication is in `docs/RIVE_SYNC_PLAN_2026-09.md` → "Shader codegen without make". Its gotchas: run fxc with `MSYS_NO_PATHCONV=1` (Git Bash otherwise turns `/D` into `D:/`); build the `/Fh` output path with **forward slashes** (a double-backslash-separated path inside a bash string collapsed to a literal `$n` and every loop output overwrote one file); `render_atlas.hlsl` gets no generic frag — only the `_stroke`/`_fill` variants; **wipe the output dir first** so a renamed shader leaves no stale header in the glob. Expect 105 minify outputs + 8 `d3d/*.h`.
2. **External Cached Archives (TBB, SFML, CEF)**:
   If you wipe the `build/` directory, CMake will attempt to download dependencies (like TBB) from private GitHub mirrors and fail. 
   - Bypass this by manually copying the `.zip` and `.tar.gz` archives from `dist_\external\` into `build\external\` before running the CMake configuration step.
3. **Scripting dependencies (Luau + libhydrogen)** — see §4 below. These are NOT vendored in the rive-runtime tarball and must be cloned into `dependencies/` before configuring, or the `luau_vm` target and every `src/lua/**` source will fail to compile.

### Dependency pins — re-check after EVERY sync
Upstream's pins live in `dependencies/premake5_*_v2.lua` and `scripting/premake5.lua`; ours are the vendored `dependencies/<vendor>_<name>_<tag>/` trees (tracked by the parent). As of `runtime-v0.1.359`:

| Dependency | Upstream pin | Ours | Verdict |
|---|---|---|---|
| yoga | `rive-app/yoga` `rive_changes_v2_0_1_3_grid` | same | **Bumped** 2026-09 — rive uses 10 grid symbols the old `v2_0_1_2` lacked. Explicit source list in `CMakeLists.txt` (13 files); `rive_yoga` must compile as **C++20** on MSVC. |
| harfbuzz | `rive-app/harfbuzz` `rive_13.1.1` | `rive_10.1.0` | **Not bumped — compatible.** The only symbols rive uses that 10.1.0 lacks are `hb_ct_*` (Apple CoreText), used solely by `font_hb_apple.*`, which we exclude. |
| luau | `luigi-rosso/luau` `rive_0_734` | same | Bumped (trimmed `Common/` + `VM/` only). |
| libhydrogen | `luigi-rosso/libhydrogen` `rive_0_2` | same | — |
| sheenbidi | `Tehreer/SheenBidi` `v2.6` | same | — |
| miniaudio | `rive-app/miniaudio` `rive_changes_5` | same | — |

**How to decide whether a bump is needed** (decisive, no guessing): list the API symbols rive's code uses and check each exists in our vendored headers, e.g. for yoga:
```bash
cd rive-runtime; T=runtime-v0.1.N
USED=$(for p in src/layout_component.cpp $(git ls-tree -r --name-only $T src/layout); do git show $T:$p; done | grep -oh '\bYG[A-Za-z0-9_]*' | sort -u)
for s in $USED; do grep -rqw "$s" dependencies/<yoga-dir>/yoga/ || echo "MISSING $s"; done
```
Any `MISSING` means bump. Beware the false alarm: a naive `lua_*` scan for Luau lists rive's **own** helpers (`lua_gpu`, `lua_pushvec2d`, `lua_register_rive`…), which are not Luau API.

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

- **`WITH_RIVE_SCRIPTING_LUAU` (upstream split the define):** plain `WITH_RIVE_SCRIPTING` is the scripted-object machinery; `_LUAU` selects the Luau VM *implementation* and gates `m_scriptingVM` on `Artboard`/`File` in **public headers**. Define it on `rive_core` **and** `rive_renderer` (PUBLIC, so it reaches the `rive` module). Without it scripting compiles to inert stubs — data populates, nothing animates — and a mismatch between targets is the same silent ABI crash described below.
- **`src/wasm/*` is excluded** from the `rive_core` glob (the new WASM VM needs WAMR, which we don't build). Safe because every wasm call site outside `src/wasm` is under `#ifdef WITH_RIVE_SCRIPTING_WASM`, and `lua_gpu.cpp`'s are under `RIVE_WASM_MODULE` inside `RIVE_CANVAS && RIVE_ORE` — never define any of those.

> [!WARNING]
> **ABI:** `WITH_RIVE_SCRIPTING` adds `m_scriptingVM` to `Artboard` and `File` in the **public** headers (`include/rive/artboard.hpp`, `include/rive/file.hpp`). It must be defined **identically** on `rive_core`, `rive_renderer`, **and** the `rive` module (it propagates to the module via `rive_full`'s PUBLIC defines). A mismatch changes `Artboard`/`File` struct sizes and produces the same silent stack-corruption crash as the renderer ABI bug. Keep it PUBLIC on both libs.

### 4.3 Verify
Play a script-driven template (the scrolling ticker). It should **scroll**, not just populate. If the data appears but nothing animates, scripting linked as stubs — recheck `WITH_RIVE_SCRIPTING` and the `luau_vm` link.

## 5. Building the Playout Engine
Once the C++ patches are applied and the dependency archives are restored, you can build the main execution target.

**First — mandatory — force a full recompile of the rive tree** (the ABI-Frankenstein guard; it shipped the v0.1.230 crash). Even with a git-based sync, headers changed by the merge must be recompiled into *every* object: bump every source/header mtime, rebuild, then **prove it**:
```bash
find rive-runtime/src rive-runtime/include rive-runtime/renderer/src rive-runtime/renderer/include \
     rive-runtime/dependencies/luau rive-runtime/dependencies/<yoga-dir>/yoga -type f \
     \( -name '*.cpp' -o -name '*.hpp' -o -name '*.c' -o -name '*.h' \) -exec touch {} +
cmake --build build --config Release --target airplayengine --parallel -- -v:minimal
find build/modules/rive -iname '*.obj' -printf '%TY-%Tm-%Td\n' | sort | uniq -c   # ALL today — not one fresh among hundreds
```
Use `--parallel` and `-v:minimal` rather than `/maxcpucount`: Git Bash mangles leading-slash switches into paths. Third-party libs whose sources you did not touch and that do not include rive headers (harfbuzz, sheenbidi, miniaudio, libhydrogen) may legitimately keep older objects — verify by target directory that every stale object belongs to one of those, never to `rive_core`, `rive_renderer`, `rive_yoga`, `luau_vm` or the `rive` module. **Known false positive — orphaned objects:** sources that upstream *deleted* leave their old `.obj` behind in `<target>.dir/Release/` (2026-09: five in `rive_core`, exactly the five sources removed between v0.1.230 and v0.1.359). They are not linked — the regenerated `.vcxproj` no longer lists them — but they trip the date check. Delete them and rebuild so the check stays meaningful: `find build/modules/rive/rive-runtime/rive_core.dir -iname '*.obj' ! -newermt today -delete`; a rebuild that finishes in seconds proves nothing needed them. **C++ standards per target — do not change casually:** `rive_yoga` **C++20** (its grid headers use designated initializers; MSVC requires it), `rive_core` **C++17** (matches upstream's core `cppdialect`; the grid headers never reach it), `rive_renderer` **C++20**. MSVC's STL is binary-compatible across `/std:` modes, so this mix is safe.

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
- **Before committing to the parent**, prove the commit will contain only real changes (both repos store LF; Windows checks out CRLF):
  ```bash
  [ "$(git rev-parse HEAD:rive-runtime/src/artboard.cpp)" = "$(git hash-object --path=rive-runtime/src/artboard.cpp rive-runtime/src/artboard.cpp)" ] && echo SAME
  ```
  Must print `SAME` — the `--path` is essential; without it `hash-object` skips the clean filter and the check proves nothing. Then `git add rive-runtime` (never the `tests/android_tests/.gradle` lock files) and commit with the sync details.
