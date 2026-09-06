# Updating the Rive Runtime in CasparFork (AirPlayEngine)

The playbook for syncing `rive-runtime/` to a newer upstream release **without losing our patches and without shipping a broken binary**. It is written for the next person (or agent) doing the sync: follow it top to bottom. Every rule here was paid for — §10 records what went wrong the last time and how it was fixed, §11 lists advice from older revisions of this file that is now obsolete.

Companion documents: `CUSTOM_PATCHES.md` (what we patch and why, with the retired/ADDRESSED record), AirPlayEngine's `docs/RIVE_RUNTIME_SUBMODULE_HANDOFF.md` (the fork + submodule layout and the daily rules shared with the editor) and `docs/RIVE_SYNC_PLAN_2026-09.md` (the full log of the last sync, including every command that was actually run).

---

## 0. The whole sync on one page

| Step | What | Proof it worked |
|---|---|---|
| 1 | Tag + push the parent (`rive-pre-sync-YYYY-MM-DD`) | tag on origin |
| 2 | In the submodule: `upstream` remote present, `git fetch upstream --tags` | `git tag -l runtime-v0.1.N` prints the tag |
| 3 | Know the base: our branch = `runtime-v0.1.<current>` + N airz commits | `git rev-list --count runtime-v0.1.<current>..HEAD` matches expectations |
| 4 | New branch `airz/merge-v0.1.N`, `rebase --onto runtime-v0.1.N runtime-v0.1.<current>` | guard-rail diff = patched sources + `CMakeLists.txt` + docs + `dependencies/` |
| 5 | Triage every patch: upstream has it? drop it → ADDRESSED | `CUSTOM_PATCHES.md` updated |
| 6 | Re-check every dependency pin with the symbol scan | 0 `MISSING` |
| 7 | Absorb source churn into `CMakeLists.txt` | configure passes |
| 8 | Regenerate shaders (no `make` here) | 105 minify outputs + 8 `d3d/*.h` |
| 9 | Touch-all → build → obj-date histogram → delete orphans | `ABI GUARD PASS` |
| 10 | The 7 functional tests (§8) on the real exe | all pass |
| 11 | `git diff runtime-v0.1.N..HEAD --stat` on the branch = patched sources + CMakeLists + docs + dependencies → push branch to the fork → bump the submodule in AirPlayEngine **and** in the editor (run its test suite) | both parents pin the same commit |

Two failures **build clean and only break on air** — a missing scripting define (§5) and a stale/mismatched object (§7). Steps 9 and 10 exist for those. Never skip them.

---

## 1. Repository layout — one fork, two consumers (since 2026-09-05)

- **The runtime lives in the fork `https://github.com/unspokenlanguage/rive-runtime`** (remote `origin`;
  `upstream` = `https://github.com/rive-app/rive-runtime`). Branch **`airz/merge-<tag>`** = upstream tag
  + our patches (`CUSTOM_PATCHES.md`) + `CMakeLists.txt` + these docs + the vendored `dependencies/<vendor>_<name>_<tag>/`
  trees (tracked on the branch since 2026-09-05, so a fresh clone builds).
- **AirPlayEngine (`CasparFork`) and the airZStudio editor both consume that branch as a git submodule
  `rive-runtime/`**, pinned to the same commit. Preview and playout run identical code by construction.
  Bump both parents to the new commit in the same sync (the editor bump also re-runs its exporter tests,
  which are the format check — see §8b).
- There is no separate workbench clone any more: the submodule checkout *is* the clone with full upstream
  history, so 3-way merges of our patches work in place. `.rive_head` is Rive's monorepo SHA — a fingerprint
  of the upstream commit (see §2 step 2).
- Rollback points: the parent tag `rive-pre-submodule-2026-09-05` (last plain-file tree) and every
  `airz/merge-<tag>` branch kept on the fork.
- Upstream tags **every** commit on `main` as `runtime-v0.1.N`; sync to a tag and record both tags in the sync log.

---

## 2. Sync procedure

1. **Rollback point first** — in AirPlayEngine, and note the editor's current submodule commit (`git -C rive-runtime rev-parse HEAD` in both parents must be equal before you start):
   ```bash
   git tag -a rive-pre-sync-YYYY-MM-DD -m "parent state before rive-runtime sync" && git push origin rive-pre-sync-YYYY-MM-DD
   ```
   Everything below runs **inside `rive-runtime/` (the submodule)**.
2. **Get upstream's tags.** The fork clone carries the upstream *commits* (they are ancestors of our branch) but not the `runtime-v*` *tags*, and a fresh checkout has no `upstream` remote — both were missing right after the 2026-09-05 conversion, so add them once per checkout:
   ```bash
   git remote get-url upstream 2>/dev/null || git remote add upstream https://github.com/rive-app/rive-runtime.git
   git fetch upstream --tags
   git tag -l 'runtime-v0.1.N'                     # must print the tag you are syncing to
   ```
   (If the base tag of the current branch is ever unknown, `.rive_head` is Rive's monorepo SHA and a unique fingerprint of the upstream commit: `git log --all -S"$(tr -d '\r\n' < .rive_head)" -- .rive_head` — oldest hit is the base.)
3. **Know the base.** Our branch is *upstream tag + our commits* (2026-09: `runtime-v0.1.359` + 7 airz commits: P5/P6 + CMake, build-system updates, docs, vendored dependencies). Check it before rebasing so a surprise shows up here, not mid-rebase:
   ```bash
   git rev-list --count runtime-v0.1.<current>..HEAD          # the airz commit count you expect
   git diff runtime-v0.1.<current>..HEAD --name-only | grep -v '^dependencies/'   # patched sources + CMakeLists.txt + docs only (--stat abbreviates paths; use --name-only)
   ```
   Optional: squash the airz commits into one before rebasing so every conflict is resolved once (`git checkout -b tmp && git reset --soft runtime-v0.1.<current> && git commit`), then rebase that.
4. **Rebase onto the new tag on a new branch** (branch names are `airz/merge-v0.1.N`, matching the existing `airz/merge-v0.1.359`):
   ```bash
   git checkout -b airz/merge-v0.1.N
   git rebase --onto runtime-v0.1.N runtime-v0.1.<current>
   ```
   Only files that upstream touched *and* we patched conflict. In a rebase **`--ours` is upstream** (the branch you are rebasing onto) and **`--theirs` is our patch** — the opposite of a merge. The vendored `dependencies/` commit replays untouched unless upstream added a file at the same path.
5. **Triage each conflicting (and each non-conflicting!) patch** per §3. A patch that applied *cleanly* is easy to overlook: in 2026-09 P3 auto-applied and had to be explicitly reverted to upstream (`git checkout runtime-v0.1.N -- <file>` + amend).
6. **Guard-rail:**
   ```bash
   git diff runtime-v0.1.N..HEAD --stat
   ```
   must list **only** the intended patched sources + `CMakeLists.txt` + `CUSTOM_PATCHES.md` + `UPDATING_RIVE.md` + `dependencies/`. Anything else is a leaked conflict resolution. Note the new `.rive_head` value now (`tr -d '\r\n' < .rive_head`) for the parent commit.
7. Continue with §4 (pins), §5 (build system), §6 (shaders), §7 (build + guard), §8 (tests), §8b (editor), §9 (publish + bump both parents).

---

## 3. Patch triage — the ADDRESSED policy

For **every** patch in `CUSTOM_PATCHES.md`, before re-applying anything:

1. Look at the fresh upstream file: `git show runtime-v0.1.N:<path> | less`, and search history: `git log --all -S'<distinctive snippet>' -- <path>`.
2. Decide one of:
   - **Addressed upstream** → drop it, keep upstream logic, move the section to *Retired patches* in `CUSTOM_PATCHES.md` and add an ADDRESSED row with the **evidence** (file/line or commit).
   - **Not addressed, still needed** → re-apply *narrowly* on top of upstream's current structure; keep the section in *Active patches*.
   - **Unknown (upstream restructured the area)** → **drop it for a test-first build** and let the functional test (§8) decide. That is what happened to P1/P2/P3 in 2026-09 and they turned out to be unnecessary.
3. Do not trust an existing ADDRESSED entry from a previous sync without re-checking — this file once claimed P5 was "fixed upstream as of v0.1.230"; it never was, and the mistaken entry nearly dropped a required patch.

`rive-runtime/CMakeLists.txt` is **entirely ours** (upstream builds with premake), so it never conflicts; it is updated by hand in §5.

---

## 4. Dependency pins — re-check after EVERY sync

Upstream's pins live in `dependencies/premake5_*_v2.lua` and `scripting/premake5.lua`. Ours are the vendored `dependencies/<vendor>_<name>_<tag>/` trees, **tracked on the fork branch** (since 2026-09-05; before that the parent tracked them). As of `runtime-v0.1.359`:

| Dependency | Upstream pin | Ours | Verdict |
|---|---|---|---|
| yoga | `rive-app/yoga` `rive_changes_v2_0_1_3_grid` | same | **Bumped 2026-09** — rive uses 10 grid symbols `v2_0_1_2` lacked. Explicit 13-file source list; `rive_yoga` must be **C++20** on MSVC (§5). |
| harfbuzz | `rive-app/harfbuzz` `rive_13.1.1` | `rive_10.1.0` | **Not bumped — compatible.** The only symbols 10.1.0 lacks are `hb_ct_*` (Apple CoreText), used solely by `font_hb_apple.*`, which we exclude. |
| luau | `luigi-rosso/luau` `rive_0_734` | same | Bumped (trimmed `Common/` + `VM/` only; the Compiler/Ast are never built — templates ship pre-compiled bytecode). |
| libhydrogen | `luigi-rosso/libhydrogen` `rive_0_2` | same | compiled into `rive_core` with `HYDRO_SIGN_VERIFY_ONLY=1` |
| sheenbidi | `Tehreer/SheenBidi` `v2.6` | same | — |
| miniaudio | `rive-app/miniaudio` `rive_changes_5` | same | — |

**Decide a bump by evidence, not by version number.** List the API symbols rive's sources use and check each exists in our vendored headers:
```bash
cd rive-runtime; T=runtime-v0.1.N
USED=$(for p in src/layout_component.cpp $(git ls-tree -r --name-only $T src/layout); do git show $T:$p; done | grep -oh '\bYG[A-Za-z0-9_]*' | sort -u)
for s in $USED; do grep -rqw "$s" dependencies/<yoga-dir>/yoga/ || echo "MISSING $s"; done
```
Any `MISSING` → bump to upstream's pin. Same idea for harfbuzz (`hb_[a-z_]*`). **False alarm to expect:** a naive `lua_*` scan for Luau lists rive's *own* helpers (`lua_gpu`, `lua_pushvec2d`, `lua_register_rive`…) — they live in `src/lua/`, not in Luau.

Fetching a new pin (from `rive-runtime/dependencies`):
```bash
git clone --depth 1 -b <tag> https://github.com/<owner>/<repo>.git <vendor>_<name>_<tag>
# luau/libhydrogen keep their unversioned dir names (CMakeLists expects dependencies/luau, dependencies/libhydrogen):
git clone --depth 1 -b rive_0_734 https://github.com/luigi-rosso/luau.git        luau
git clone --depth 1 -b rive_0_2   https://github.com/luigi-rosso/libhydrogen.git libhydrogen
grep "dependency.github('luigi-rosso" scripting/premake5.lua      # the authoritative luau/libhydrogen refs
```
Remove the old vendored tree, update the `*_DIR` variable in `CMakeLists.txt`, re-check the explicit source list if the library has one (yoga does), and commit the new tree on the branch (it is what makes a fresh `--recurse-submodules` clone build).

---

## 5. Build system — what `CMakeLists.txt` must absorb

`rive_core` is built from `file(GLOB_RECURSE src/*.cpp)` with exclusions, so most upstream churn (39 added / 5 deleted sources in 2026-09) needs no edit. After each sync check:

- **New subtrees needing exclusion.** Current excludes: `profiler/`, `wasm/` (needs WAMR; safe because every wasm call site outside `src/wasm` is under `#ifdef WITH_RIVE_SCRIPTING_WASM`, and `lua_gpu.cpp`'s are under `RIVE_WASM_MODULE` inside `RIVE_CANVAS && RIVE_ORE` — never define any of those), `font_hb_apple*`. If configure or compile fails on a new directory, ask what platform/feature it serves before excluding it.
- **Explicit source lists** (yoga; the `luau_vm` glob): compare against the new tree.
- **Defines — ABI critical.** `WITH_RIVE_TEXT`, `WITH_RIVE_LAYOUT`, `WITH_RIVE_SCRIPTING`, **`WITH_RIVE_SCRIPTING_LUAU`** are PUBLIC on `rive_core` *and* `rive_renderer` and thus reach the `rive` module. `WITH_RIVE_SCRIPTING` adds `m_scriptingVM` to `Artboard`/`File` in public headers; `_LUAU` (upstream split it out in 2026) selects the Luau VM implementation and gates the same member. A mismatch between targets changes struct sizes → silent stack corruption on air. **Without `_LUAU` everything builds and scripting runs as inert stubs**: data populates, nothing animates. Whenever upstream adds a `WITH_RIVE_*` define, grep `include/` for it and decide.
- **C++ standard per target — do not change casually:** `rive_yoga` **C++20** (its `grid/*.h` use designated initializers; MSVC gates them on `/std:c++20`, clang/gcc accept them at C++17 which is why upstream's `cppdialect('C++17')` works), `rive_core` **C++17** (upstream's core dialect; no `yoga/*.h` includes `grid/`, so the C++20 headers never reach it), `rive_renderer` **C++20**. MSVC's STL is binary-compatible across `/std:` modes, so the mix is safe.
- **`/fp:precise` on `rive_yoga`, `rive_core`, `luau_vm`** — the engine sets `/fp:fast` globally (`src/CMakeModules/Bootstrap_Windows.cmake`) and that folds Yoga's NaN-based "undefined" checks to constants: every layout resolves to 0×0 (see `CUSTOM_PATCHES.md` Patch 7). Any new target that compiles Yoga or Luau sources needs it too.
- **Engine-side API drift**: the calls `src/modules/rive/producer/rive_producer.cpp` makes (`File::import`, `createDefaultViewModelInstance`, `bindViewModelInstance`, `syncStyleChangesWithUpdate`, …) — the compiler finds these, but read the upstream changelog for semantic changes.

Not rive-specific but bites here: if `build/` was wiped, CMake tries to download TBB/SFML/CEF from private mirrors and fails — copy the archives from `dist_\external\` into `build\external\` before configuring.

---

## 6. Shader codegen — this machine has no `make`

`rive_renderer` is compiled from source (for ABI parity with `rive_core`), and its generated D3D headers must exist **before** CMake configures (the `RIVE_RENDERER_GENERATED` glob). Regenerate whenever upstream touched `renderer/src/shaders/` (2026-09 renamed a shader). The Makefile's `d3d` target is only `minify.py` + `fxc`; this replicates it exactly (needs `pip install ply`; fxc from the Windows Kits):
```bash
cd rive-runtime/renderer/src/shaders
OUT=C:/Users/kutla/Desktop/Github/CasparFork/build/modules/rive/rive-runtime/include/generated/shaders
FXC="/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/fxc.exe"
rm -rf "$OUT"; mkdir -p "$OUT/d3d"                       # wipe first: a renamed shader must not leave a stale header in the glob
python minify.py -o "$OUT" *.glsl *.vert *.frag            # expect 105 outputs
export MSYS_NO_PATHCONV=1                                  # else Git Bash turns fxc's /D into D:/
for h in d3d/*.hlsl; do n=$(basename "$h" .hlsl)
  "$FXC" /nologo /D VERTEX   /I "$OUT" /T vs_5_0 /Fh "$OUT/d3d/$n.vert.h" "$h"
  [ "$n" != render_atlas ] && "$FXC" /nologo /D FRAGMENT /I "$OUT" /T ps_5_0 /Fh "$OUT/d3d/$n.frag.h" "$h"
done
"$FXC" /nologo /D FRAGMENT /D ATLAS_FEATHERED_STROKE /I "$OUT" /T ps_5_0 /Fh "$OUT/d3d/render_atlas_stroke.frag.h" d3d/render_atlas.hlsl
"$FXC" /nologo /D FRAGMENT /D ATLAS_FEATHERED_FILL   /I "$OUT" /T ps_5_0 /Fh "$OUT/d3d/render_atlas_fill.frag.h"   d3d/render_atlas.hlsl
"$FXC" /nologo /I "$OUT" /T rootsig_1_1 /E ROOT_SIG /Fh "$OUT/d3d/root.sig.h" d3d/root.sig
ls "$OUT/d3d" | wc -l                                      # expect 8
```
Rules learned the hard way: **forward slashes only** in the `/Fh` path (a `\\`-separated path inside a bash string collapsed to a literal `$n` and every loop iteration overwrote one file); `render_atlas.hlsl` gets no generic frag (it is `.PHONY` upstream — only the stroke/fill variants); re-read the Makefile each sync in case a new variant appeared. If GNU `make` ever becomes available: `make -C rive-runtime/renderer/src/shaders -j8 OUT=<abs path> FLAGS="" d3d`.

---

## 7. Build + the ABI guard (mandatory)

Any object compiled against *old* headers that survives into the link is a Frankenstein binary: it builds, links, and crashes on air with stack corruption (this shipped once, with v0.1.230, after a copy that preserved mtimes). Force a full recompile of the rive tree and **prove it**:
```bash
# from CasparFork root
find rive-runtime/src rive-runtime/include rive-runtime/renderer/src rive-runtime/renderer/include \
     rive-runtime/dependencies/luau rive-runtime/dependencies/<yoga-dir>/yoga -type f \
     \( -name '*.cpp' -o -name '*.hpp' -o -name '*.c' -o -name '*.h' \) -exec touch {} +
cmake --build build --config Release --target airplayengine --parallel -- -v:minimal
find build/modules/rive -iname '*.obj' ! -newermt today -printf '%h\n' | sort | uniq -c   # stale objects by target dir
cmake --build build --config Release --target airplayengine_copy_dependencies
```
Read the histogram by **target directory**:
- Older objects are acceptable **only** in self-contained third-party libs that include no rive headers: `rive_harfbuzz`, `rive_sheenbidi`, `rive_miniaudio`.
- Any older object under `rive_core`, `rive_renderer`, `rive_yoga`, `luau_vm` or the `rive` module fails the guard — **except the known false positive:** sources that upstream *deleted* leave orphaned `.obj` files behind (2026-09: five in `rive_core`, exactly the five deleted sources). They are not linked (the regenerated `.vcxproj` omits them) but they trip the date check. Delete them and rebuild; a rebuild that finishes in seconds proves nothing needed them:
  ```bash
  find build/modules/rive/rive-runtime/rive_core.dir -iname '*.obj' ! -newermt today -delete
  ```
- From Git Bash use `--parallel -- -v:minimal`, **not** `-- /maxcpucount`: MSYS rewrites leading-slash switches into paths. From PowerShell `/maxcpucount` is fine.

---

## 8. Functional verification — the tests that decide

Run on the freshly built `build/shell/Release/airZstudio.exe` with the real templates. Each item proves one thing; all seven passed on 2026-09-05 for `runtime-v0.1.359`:

1. **Dynamic image injected into an `Image` inside a Group** → layout size propagation through groups (the former P1/P2/P3 area). Failing here means upstream's `propagateSizeToChildren` regressed; re-apply narrowly, P3 → P1 → P2, see `CUSTOM_PATCHES.md` retired section.
2. **Arrow + percentage "Stateful Components" render non-blank** → `/fp:precise` on yoga/core (Patch 7) + the layout pass at load.
3. **Nested artboard with no default animation still data-binds** → Patch 6.
4. **Nested state machine with unset `animationId` (image sequences)** → Patch 5.
5. **Scripted ticker scrolls** (not just populates) → `WITH_RIVE_SCRIPTING_LUAU` reached every target and `luau_vm` linked.
6. **`CG UPDATE` with `__reload` hot-reloads; a `.riv` is loaded once, not twice** → engine-side regressions (`rive_producer.cpp`, `cg_proxy.cpp` leaf unwrap).
7. **PLAY / CG ADD / UPDATE / INVOKE smoke** with no `[rive]` SEH warnings and non-zero layout sizes in the log.

Upstream's unit-test assets help for 1–2: `tests/unit_tests/assets/stateful_component_image_test.riv`, `image_fit_alignment_updated_test.riv`.

### 8b. The editor side — same commit, its tests are the format check

The airZStudio editor (`airZStudioEditor`, local checkout beside this repo) consumes the same fork branch as a submodule and **must pin the same commit**. Its test suite runs the exporter against `rive::File::import` and the checker, so a `.riv` format change surfaces there first. After §8 passes here:
```bash
cd ../airZStudioEditor && git -C rive-runtime fetch origin && git -C rive-runtime checkout <new commit> && flutter test
```
- Update the editor's pin statements (`CLAUDE.md`, `AGENTS.md`, `README`) and its `.gitmodules` branch.
- If `rive::File::majorVersion` / `minorVersion` changed, the editor's writer must be bumped in the same change.
- Host behaviour the editor preview mirrors lives in `airzscene-core/src/core.cpp`; when `rive_producer.cpp` changes host behaviour (slot gating, event reporting, focus/keyboard, nested default machine), say so in the commit message.
- Runtime ≥ 0.1.359: the `FocusManager` lives on the artboard instance and `File::artboardAt()->instance()` does **not** create one — call `ensureFocusManager()` before relying on keyboard/text listeners. Playout (`rive_producer.cpp`) uses no keyboard or pointer listeners today, so nothing to do here until it does. Reported events carry `CustomProperty*` children; the editor's `bindings.json` lists them per controller.

---

## 9. Publishing — push the branch, bump both parents

The parents no longer carry runtime files: each pins one submodule commit. Four checks, all of which can fail:

1. **Guard-rail per file** (inside the submodule): the delta against the tag is exactly the expected set, and no line-ending noise hides in it:
   ```bash
   git diff runtime-v0.1.N..HEAD --name-only | grep -v '^dependencies/'          # patched sources, CMakeLists.txt, CUSTOM_PATCHES.md, UPDATING_RIVE.md — nothing else
   diff <(git diff runtime-v0.1.N..HEAD --numstat) <(git diff runtime-v0.1.N..HEAD --ignore-cr-at-eol --numstat) && echo NO-CR-NOISE
   ```
   (Both repos store LF and check out CRLF; a file that shows a CR-only difference has mixed endings — normalise it to one style, then `git add --renormalize <file>`.)
2. **Branch on the fork = local HEAD**, or the parents will pin a commit nobody can fetch:
   ```bash
   git push origin airz/merge-v0.1.N
   [ "$(git ls-remote origin refs/heads/airz/merge-v0.1.N | cut -f1)" = "$(git rev-parse HEAD)" ] && echo PUSHED
   ```
3. **Bump AirPlayEngine** — the gitlink and the branch name in `.gitmodules`, one commit:
   ```bash
   cd .. && git submodule set-branch --branch airz/merge-v0.1.N rive-runtime
   git add rive-runtime .gitmodules && git commit -F <message-file>   # chore(rive): sync rive-runtime <old-tag> → <new-tag> (<n> upstream commits) … per-patch verdicts, pin/define changes, both tags, rollback tag
   git push origin master
   git submodule status            # commit must be the one you pushed in step 2
   ```
4. **Bump the editor to the same commit** (§8b) and prove the two parents agree — this is the invariant the handoff insists on:
   ```bash
   [ "$(git -C rive-runtime rev-parse HEAD)" = "$(git -C ../airZStudioEditor/rive-runtime rev-parse HEAD)" ] && echo SAME-PIN
   ```
Update `CUSTOM_PATCHES.md` ("Currently at" + sync log) and keep the rollback tags for a while. A docs-only change on the branch still moves the commit, so it still needs both bumps.

---

## 10. Lessons log — what failed in the 2026-09 sync and how it was fixed

Kept so the next sync does not rediscover them. Full command-level detail: `docs/RIVE_SYNC_PLAN_2026-09.md`. Rows 1–3, 9, 13 and 14 date from the plain-file layout (the parent tracked 8.5k runtime files and an unregistered workbench clone did the merging, both retired the same day by the submodule conversion); their fixes are superseded by §2/§9, the failure modes are still instructive.

| # | What happened | Root cause | Fix / rule now in this doc |
|---|---|---|---|
| 1 | Previous sync method (copy upstream files over, re-apply patches by hand) could not tell what upstream changed *inside our patched files*, and re-derived patches from a doc that was wrong about P5. | No 3-way base. | Nested git workbench, `rebase --onto` (§1–§2). |
| 2 | After `reset --mixed`, 2,297 files showed as modified. | CRLF stat churn (`core.autocrlf=true`). | `git add --renormalize` → exactly the real patches (§2 step 3). |
| 3 | P3 (`image.cpp`) auto-applied in the rebase although it was meant to be dropped. | Clean rebase application is silent. | Guard-rail diff after rebase; explicit `git checkout <tag> -- <file>` (§2 step 5–6). |
| 4 | First build: 27 errors, all in `rive_yoga` (`C2429`, `C7555`). | New grid yoga uses designated initializers; `rive_yoga` had no C++ standard set (MSVC default = C++14). | `target_compile_features(rive_yoga PRIVATE cxx_std_20)`; core stays 17 (§5). |
| 5 | fxc "Too many files specified ('D:/')". | Git Bash rewrote `/D VERTEX` into a path. | `MSYS_NO_PATHCONV=1` (§6). |
| 6 | Only 3 of 8 D3D headers appeared, one file overwritten by every loop iteration. | `\\`-separated output path inside a bash string collapsed to a literal `$n`. | Forward slashes only (§6). |
| 7 | Luau "missing symbols" — looked like a needed bump. | The scan matched rive's own `lua_*` helpers. | Bumped to upstream's pin anyway; false-alarm note (§4). |
| 8 | ABI histogram flagged 5 stale objects in `rive_core`. | Orphaned `.obj` of the 5 sources upstream deleted; not linked. | Delete orphans, rebuild (13 s) → PASS (§7). |
| 9 | Parent diff differed raw vs CR-normalised; `LF will be replaced by CRLF` warnings. | A sed-edited file was all-LF in the worktree (harmless; git stores LF) — and an earlier `$'\r'` grep check itself mis-counted. | Byte-level check with python; normalise to one style before commit (§9). |
| 10 | `.rive_head` looked useless as a version pointer (monorepo SHA). | It is a fingerprint, not a runtime commit. | `git log -S<sha> -- .rive_head` finds the baseline in seconds (§2 step 2). |
| 11 | Long bash heredocs containing Python or Markdown failed to parse in the agent tooling. | Quote-heavy content trips the command parser. | Write scripts/docs to a file first, then run/copy them. |
| 12 | The sync took `.riv` layout patches P1–P3 on faith for a year. | Nobody had tested unmodified upstream. | Test-first policy (§3): drop uncertain patches, let §8 decide. Result: all three unnecessary. |
| 13 | The documented `hash-object --path` spot check printed DIFFERENT right before the parent commit. | It was written against `artboard.cpp`, a file upstream changed — the check only means something on an upstream-unchanged file. | §9 now uses `LICENSE`/`mat2d.cpp` and, above all, the blob-level index-vs-tag proof (check 0), which found the parent equal to upstream except the two patched sources. |
| 14 | 24 upstream-changed files showed a 1-line raw-vs-`--ignore-cr-at-eol` difference. | Stray carriage returns in upstream's own blobs (mixed-ending files are stored as-is). | Not ours to fix; the blob-level proof showed the stored blobs equal upstream's. Only files that *differ from upstream* need line-ending attention. |
| 15 | After the submodule conversion the handoff's first sync command (`git fetch upstream --tags` → `rebase --onto runtime-v0.1.N runtime-v0.1.359`) could not run: the checkout had no `upstream` remote and none of the `runtime-v*` tags. | Tags are not pushed with a branch; the fork clone only had `origin`. | Remote added and tags fetched 2026-09-05; §2 step 2 makes it a checked prerequisite. |

Methodology in one sentence: **every claim gets a check that can fail** — the pin table has a symbol scan, the rebase has a guard-rail diff, the build has the obj histogram, the patch list has the seven on-air tests, the commit has `hash-object --path`.

---

## 11. Scratched from earlier revisions of this file (resolved — for the record)

- **"Clone upstream to a temp dir and copy files over, then re-apply every patch by hand."** Retired 2026-09; replaced by §2. It caused the mtime-preserving copy that shipped the v0.1.230 Frankenstein binary.
- **"Re-apply Patches 1–4 (stale-Yoga cache fallback, Group recursion, `controlSize` skip-guard, `[RIVE DIAG]` removal)."** P1–P3 proved unnecessary on `runtime-v0.1.359` (test 1 of §8 passed on unmodified upstream); P4 was never upstream — the diagnostics were our own debug lines. Their diffs are preserved in `CUSTOM_PATCHES.md` → *Retired patches* and in workbench branch `airz/patches-on-4a10679b`.
- **"Wrap `LayoutScaleType::fixed` uses in `#ifdef fixed / #undef fixed`" (Windows `rpcndr.h` macro collision).** Obsolete: upstream `layout_component.cpp` uses `LayoutScaleType::fixed` nine times and builds clean on MSVC without any guard at v0.1.359. Re-check only if a `C2059`/`fixed` error appears.
- **"Patch 5 was fixed natively upstream as of runtime-v0.1.230."** False; corrected — it is still required (`CUSTOM_PATCHES.md` Patch 5).
- **"Add a `m_layoutData == nullptr` guard in `calculateLayoutInternal`."** Upstream now has an equivalent (`if (m_style == nullptr || m_layoutData == nullptr)`); do not re-add ours.
- **Luau pin `rive_0_731`.** Now `rive_0_734`; the pin table (§4) is the single place versions are recorded.
- **"`WITH_RIVE_SCRIPTING` alone enables scripting."** Since upstream split the define, `WITH_RIVE_SCRIPTING_LUAU` is also required (§5) or scripting is silently inert.
- **"`rive-runtime/` is tracked by the parent as plain files; an unregistered nested workbench clone does the merging; `dependencies/*` are the parent's job."** True for a few hours on 2026-09-05. Superseded the same day by the submodule on the `unspokenlanguage/rive-runtime` fork with the dependency trees on the branch (§1). The workbench's `.git` is backed up at AirPlayEngine `.git/rive-workbench-backup` (133 MB) until someone deletes it.
- **"Blob-level parent-index-vs-tag proof and the `hash-object --path` spot check before the parent commit."** Only meaningful when the parent indexed runtime files; the parent now stores one gitlink. The equivalent proof is §9 check 1 (guard-rail per file inside the submodule) plus check 2 (fork branch = local HEAD).
