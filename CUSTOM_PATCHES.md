# Custom Patches Applied to the Rive Runtime

What we change in `rive-runtime/` relative to upstream (`https://github.com/rive-app/rive-runtime.git`), why, and — just as important — what we **used to** change and no longer need to. The sync procedure that keeps this list honest is `UPDATING_RIVE.md`; its §3 defines the triage rule: *every patch is re-verified against fresh upstream on every sync; anything upstream now covers moves to "Retired" with evidence.*

**Currently at:** `runtime-v0.1.359` (2026-09-05) — fork `unspokenlanguage/rive-runtime`, branch `airz/merge-v0.1.359`, consumed as a submodule by AirPlayEngine and the airZStudio editor, which must pin the same commit. **Active patches: 3** (5, 6, 7). **Retired: 4** (1, 2, 3, 4) + one null guard.

---

## Active patches — re-apply (and re-verify) on every sync

## Patch 5: Nested State Machine — unset `animationId` (`(uint32_t)-1`) falls back to the default state machine
> [!IMPORTANT]
> **Correction (2026-09 sync): this was NEVER fixed upstream.** An earlier revision of this file
> claimed it was "fixed natively upstream as of runtime-v0.1.230". That was wrong: the guard has
> never existed in `rive-app/rive-runtime` history (verified with `git log --all -S'(uint32_t)-1'`),
> and upstream's `initializeAnimation` still does a bare `artboard->stateMachineAt(animationId())`,
> which for `(uint32_t)-1` indexes 4294967295. It lives in `initializeAnimation` because **we** moved
> it there. **It remains required** and was re-applied cleanly onto `runtime-v0.1.359` (0 upstream
> commits touched the file).

* **File:** `src/animation/nested_state_machine.cpp`
* **Function:** `NestedStateMachine::initializeAnimation`
* **Description:** When a nested state machine's `animationId` is unset (-1 = "use the default"), the `uint32_t` underflows to 4294967295, no state machine is found, and nested image sequences never load.
* **Resolution:** If `animationId() == (uint32_t)-1`, substitute the artboard's `defaultStateMachineIndex()` (or 0). Originally applied in `NestedArtboard::onAddedClean`; moved into `initializeAnimation`, where the lookup actually happens.

### Diff:
```diff
 void NestedStateMachine::initializeAnimation(ArtboardInstance* artboard)
 {
+    if (animationId() == (uint32_t)-1)
+    {
+        int defaultId = artboard->defaultStateMachineIndex();
+        animationId(defaultId >= 0 ? defaultId : 0);
+    }
     m_StateMachineInstance = artboard->stateMachineAt(animationId());
```

---

## Patch 6: Nested Component Empty Animation Fallback
* **File:** `src/nested_artboard.cpp`
* **Function:** `NestedArtboard::onAddedClean`
* **Description:** When an authored nested artboard has no active default animation, `m_NestedAnimations` is empty and data binds never propagate into it. We force-instantiate the default state machine.
* **Resolution:** After the `initializeAnimation` loop, if `m_NestedAnimations` is empty and the instance has state machines, create a `NestedStateMachine` for the default index, initialize and add it, and take ownership via `m_boundNestedStateMachine` so it is released with the `NestedArtboard`. *(Diff recovered in the 2026-09 sync — it was previously undocumented. Re-applied cleanly onto `runtime-v0.1.359`; `m_boundNestedStateMachine` / `addNestedAnimation` are unchanged upstream.)*

### Diff:
```diff
         for (auto animation : m_NestedAnimations)
         {
             animation->initializeAnimation(m_Instance.get());
         }
+
+        if (m_NestedAnimations.empty() && m_Instance->stateMachineCount() > 0)
+        {
+            auto nestedStateMachine = new NestedStateMachine();
+            int defaultId = m_Instance->defaultStateMachineIndex();
+            nestedStateMachine->animationId(defaultId >= 0 ? defaultId : 0);
+            nestedStateMachine->initializeAnimation(m_Instance.get());
+            addNestedAnimation(nestedStateMachine);
+            // Take ownership so the injected default state machine is released
+            // with this NestedArtboard (matches how updateComponents binds nested
+            // state machines). See CUSTOM_PATCHES.md Patch 6.
+            m_boundNestedStateMachine.reset(
+                static_cast<NestedStateMachine*>(nestedStateMachine));
+        }
+
         m_referencedArtboard->host(this);
```

---

## Patch 7: Compile Yoga/layout with `/fp:precise` (fixes ALL layout / "Stateful Components")
> [!NOTE]
> **2026-09 sync:** `rive-runtime/CMakeLists.txt` is *entirely ours* (upstream builds with premake), so it never conflicts — but it must absorb upstream's source churn (39 added / 5 deleted `.cpp` this time; `rive_core` uses `GLOB_RECURSE`, so only `src/wasm/*` needed excluding). Yoga is now `rive-app_yoga_rive_changes_v2_0_1_3_grid` (13 explicit sources) and **`rive_yoga` compiles as C++20** — its `grid/AutoPlacement.h` / `TrackSizing.h` use designated initializers, which MSVC gates on `/std:c++20` (clang/gcc accept them as a C++17 extension). `rive_core` stays C++17 (upstream's core `cppdialect`; the grid headers never reach it). `/fp:precise` is unchanged and still required.
* **File:** [CMakeLists.txt](file:///c:/Users/kutla/Desktop/Github/CasparFork/rive-runtime/CMakeLists.txt) — the `rive_yoga` and `rive_core` targets.
* **Symptom:** Rive **"Stateful Components"** (layout-mode Leaf/Node nested artboards — e.g. an arrow + percentage group) rendered completely blank in the playout engine, while absolutely-positioned text on the same artboard rendered fine. They render correctly in the Rive editor and in Flutter runtimes. More generally: **any Rive layout (Yoga) produced zero-size results.**
* **Root cause — the compiler flag `/fp:fast`.** The engine sets `/fp:fast` globally (`src/CMakeModules/Bootstrap_Windows.cmake`). Yoga represents every *undefined* value as a **NaN** (`YGUndefined = std::numeric_limits<float>::quiet_NaN()`, `yoga/YGValue.h`) and its entire algorithm branches on `YGFloatIsUndefined()` / `std::isnan()` / `NaN != NaN`. Under `/fp:fast` the compiler is permitted to assume NaNs never occur, so those checks fold to constants and NaN comparisons stop working — which corrupts Yoga's flex resolution and makes **every dimension resolve to 0**. Proven with an isolated pure-C-API self-test: a fresh `YGNodeNew()` with `YGNodeStyleSetWidth(1920)/SetHeight(1080)` and `YGNodeCalculateLayout(...)` returned **`0 × 0`** under `/fp:fast` and **`1920 × 1080`** under `/fp:precise`. (Not an ABI/standard mismatch — `sizeof(YGNode)`/`sizeof(YGStyle)` were verified identical across `rive_core` (C++17) and `rive_yoga`.)
* **Resolution:** Add `/fp:precise` to the `rive_yoga` and `rive_core` MSVC compile options. Because it is appended *after* the global `/fp:fast`, MSVC uses the last flag (`Precise`), restoring correct NaN semantics for the layout engine. This is the true, minimal fix — no runtime logic changes needed; all Yoga layout now computes correctly.

### Diff (rive-runtime/CMakeLists.txt):
```diff
 add_library(rive_yoga STATIC ${YOGA_SOURCES})
 ...
 if(MSVC)
-    target_compile_options(rive_yoga PRIVATE /W0
+    target_compile_options(rive_yoga PRIVATE /W0 /fp:precise
         /FI"${RIVE_DEPS_DIR}/rive_yoga_renames.h"
     )
 endif()
 ...
 if(MSVC)
     target_compile_definitions(rive_core PUBLIC _USE_MATH_DEFINES)
-    target_compile_options(rive_core PRIVATE /W3 /wd4244 /wd4267 /wd4305 /wd4996 /wd4100 /wd4189 /wd4127 /bigobj)
+    target_compile_options(rive_core PRIVATE /W3 /fp:precise /wd4244 /wd4267 /wd4305 /wd4996 /wd4100 /wd4189 /wd4127 /bigobj)
 endif()
```

> [!IMPORTANT]
> Any translation unit that touches Yoga, or does NaN checks on Yoga's `YGUndefined` values, must be built `/fp:precise`. If a future change adds another target that compiles Yoga headers/sources, give it `/fp:precise` too. Do **not** revert to `/fp:fast` for `rive_yoga`/`rive_core`.
>
> A small `m_layoutData == nullptr` guard was also kept in `LayoutComponent::calculateLayoutInternal` as defensive null-safety (harmless). The engine side (`rive_producer.cpp`) forces a layout pass at load via `artboard->syncStyleChangesWithUpdate(true)` after the state machine settles, so layout-mode components are sized before the first rendered frame.

---

---

## Retired patches — resolved, kept for the record

These were real fixes for real on-air bugs (dynamic image injection into layouts, 2025–26). They are **not applied** since the 2026-09 sync: upstream's reworked layout code makes them unnecessary, proven by the on-air test list in `UPDATING_RIVE.md` §8 (all seven passed on `runtime-v0.1.359` without them). Their diffs stay here so the symptom → fix mapping survives if the area ever regresses. Patch numbers are kept for cross-references.

## Patch 1: Fixed Stale Yoga Layout Cache in Layout Size Propagation
> [!NOTE]
> **RETIRED — resolved by upstream (`runtime-v0.1.359`, verified on air 2026-09-05).** Dropped for a test-first build; the decisive test (dynamic image injected into an `Image` inside a Group) and all six other on-air checks in `UPDATING_RIVE.md` §8 passed on **unmodified** upstream layout code. Upstream's reworked `propagateSizeToChildren` (`stopsContentSizing()`, `LayoutNodeProvider` participants, `shouldPropagateSizeToChildren`) covers the case. **Do not re-apply.** Diff kept below for the record; also in workbench branch `airz/patches-on-4a10679b`.
* **File:** [layout_component.cpp](file:///c:/Users/kutla/Desktop/Github/CasparFork/rive-runtime/src/layout_component.cpp#L937-L958)
* **Function:** `LayoutComponent::propagateSizeToChildren`
* **Description:** When a dynamic image is injected at runtime via DataBinding, the child component receives stale layout dimensions from the initial placeholder rather than the authored fixed dimensions because Yoga's layout calculation has not caught up.
* **Resolution:** Derived and used the deterministic authored layout width/height if the layout style has a fixed scale type and the dimensions are greater than zero.

### Diff:
```diff
void LayoutComponent::propagateSizeToChildren(ContainerComponent* component)
{
    if (isHidden())
    {
        return;
    }
-    for (auto child : component->children())
+    // Determine effective layout dimensions. For fixed-size layouts,
+    // m_layout may be stale if Yoga hasn't recalculated yet. In that case,
+    // use the authored width()/height() which are deterministic for fixed layouts.
+    float effectiveW = m_layout.width();
+    float effectiveH = m_layout.height();
+    if (m_style != nullptr)
+    {
+        if (m_style->widthScaleType() == LayoutScaleType::fixed && width() > 0)
+        {
+            effectiveW = std::max(effectiveW, width());
+        }
+        if (m_style->heightScaleType() == LayoutScaleType::fixed && height() > 0)
+        {
+            effectiveH = std::max(effectiveH, height());
+        }
+    }
+    for (auto child : component->children())
     {
         if (child->is<LayoutComponent>())
         {
             continue;
         }
         auto sizeableChild = IntrinsicallySizeable::from(child);
         if (sizeableChild != nullptr)
         {
             LayoutScaleType widthScaleType = m_style != nullptr ? m_style->widthScaleType() : LayoutScaleType::fixed;
             LayoutScaleType heightScaleType = m_style != nullptr ? m_style->heightScaleType() : LayoutScaleType::fixed;
             sizeableChild->controlSize(
-                Vec2D(m_layout.width(), m_layout.height()),
+                Vec2D(effectiveW, effectiveH),
                 widthScaleType,
                 heightScaleType,
                 actualDirection());
```

---

## Patch 2: Enabled Recursion Through Group Nodes (NodeBase)
> [!NOTE]
> **RETIRED — resolved by upstream (`runtime-v0.1.359`, verified on air 2026-09-05).** Dropped for a test-first build; the decisive test (dynamic image injected into an `Image` inside a Group) and all six other on-air checks in `UPDATING_RIVE.md` §8 passed on **unmodified** upstream layout code. Upstream's reworked `propagateSizeToChildren` (`stopsContentSizing()`, `LayoutNodeProvider` participants, `shouldPropagateSizeToChildren`) covers the case. **Do not re-apply.** Diff kept below for the record; also in workbench branch `airz/patches-on-4a10679b`.
> Note: upstream now *deliberately* blocks content-sizing at groups (`stopsContentSizing()` — "a group's contents are free content"). This is a design choice, not a missing fix; if it ever has to come back, keep it narrow (recurse into a plain `NodeBase` group only to reach `IntrinsicallySizeable` children like `Image`; keep the `Solo` and participant rules).
* **File:** [layout_component.cpp](file:///c:/Users/kutla/Desktop/Github/CasparFork/rive-runtime/src/layout_component.cpp#L959-L986)
* **Function:** `LayoutComponent::propagateSizeToChildren`
* **Description:** The original code contained a hardcoded exclusion skipping any child of type `NodeBase` (Group). This prevented size propagation from reaching nested components (such as an `Image`) when they were placed inside a Group container.
* **Resolution:** Removed the check/exclusion of `NodeBase::typeKey` so that the layout engine successfully recurses into Group nodes.

### Diff:
```diff
void LayoutComponent::propagateSizeToChildren(ContainerComponent* component)
{
    ...
    for (auto child : component->children())
    {
        if (child->is<LayoutComponent>())
        {
            continue;
        }
-        if (child->coreType() == NodeBase::typeKey)
-        {
-            continue;
-        }
        auto sizeableChild = IntrinsicallySizeable::from(child);
        if (sizeableChild != nullptr)
        ...
```

---

## Patch 3: Eliminated controlSize Skip-if-Unchanged Race Condition
> [!NOTE]
> **RETIRED — resolved by upstream (`runtime-v0.1.359`, verified on air 2026-09-05).** Dropped for a test-first build; the decisive test (dynamic image injected into an `Image` inside a Group) and all six other on-air checks in `UPDATING_RIVE.md` §8 passed on **unmodified** upstream layout code. Upstream's reworked `propagateSizeToChildren` (`stopsContentSizing()`, `LayoutNodeProvider` participants, `shouldPropagateSizeToChildren`) covers the case. **Do not re-apply.** Diff kept below for the record; also in workbench branch `airz/patches-on-4a10679b`.
> (Upstream still has the skip-guard (`if (m_layoutWidth != size.x || m_layoutHeight != size.y)`, ~line 233 — and the test passes with it.)
* **File:** [image.cpp](file:///c:/Users/kutla/Desktop/Github/CasparFork/rive-runtime/src/shapes/image.cpp#L231-L244)
* **Function:** `Image::controlSize`
* **Description:** The layout dimensions and the backing `renderImage` are initialized at different points in the lifecycle during dynamic image loading. The original skip guard prevented updating the scale if the layout dimensions hadn't changed since the last invocation, meaning the image scaling wouldn't run after the asset completed loading and became valid.
* **Resolution:** Removed the conditional skip guard. The inner `updateImageScale()` function already safely checks for `renderImage != nullptr` and non-NaN values.

### Diff:
```diff
void Image::controlSize(Vec2D size,
                        LayoutScaleType widthScaleType,
                        LayoutScaleType heightScaleType,
                        LayoutDirection direction)
{
-    if (m_layoutWidth != size.x || m_layoutHeight != size.y)
-    {
-        m_layoutWidth = size.x;
-        m_layoutHeight = size.y;
-        updateImageScale();
-    }
+    // Always update and recalculate — the render image may have arrived
+    // between layout passes, so we must not skip based on cached values.
+    m_layoutWidth = size.x;
+    m_layoutHeight = size.y;
+    updateImageScale();
}
```

---

## Patch 4: Removed High-Latency Diagnostic Logging from Render/Layout Paths
> [!NOTE]
> **Retired (2026-09 sync) — see ADDRESSED.** The `[RIVE DIAG]` lines were never upstream; they were our own debug additions, since removed. `image_base.hpp` is not patched at all. Nothing to re-apply.
* **Files:**
  - [image_base.hpp](file:///c:/Users/kutla/Desktop/Github/CasparFork/rive-runtime/include/rive/generated/shapes/image_base.hpp#L143-L148)
  - [layout_component.cpp](file:///c:/Users/kutla/Desktop/Github/CasparFork/rive-runtime/src/layout_component.cpp)
  - [image.cpp](file:///c:/Users/kutla/Desktop/Github/CasparFork/rive-runtime/src/shapes/image.cpp)
* **Description:** The native runtime contained a set of verbose `[RIVE DIAG]` log statements outputting directly to `stderr`. Because the render loop and layout synchronization threads invoke these paths multiple times per frame, these synchronous I/O operations caused severe CPU-side rendering bottlenecks, resulting in frame-pacing drops and laggy output in broadcast.
* **Resolution:** Deleted all `fprintf(stderr, "[RIVE DIAG] ...\n", ...)` logging calls.

### Diff Details:

#### `rive-runtime/include/rive/generated/shapes/image_base.hpp`:
```diff
             case fitPropertyKey:
                 m_Fit = CoreUintType::deserialize(reader);
-                fprintf(stderr, "[RIVE DIAG] Deserialized image fit (Property 974): %u\n", m_Fit);
                 return true;
```

#### `rive-runtime/src/layout_component.cpp`:
```diff
@@ -523,7 +523,6 @@ void LayoutComponent::syncStyle()
     {
         ygNode.setContext(this);
         ygNode.setMeasureFunc(measureFunc);
-        fprintf(stderr, "[RIVE DIAG] syncStyle: intrinsicallySized=true, isLeaf=%d -> using measureFunc\n", isLeaf());
     }
     else
     {
@@ -536,10 +535,6 @@ void LayoutComponent::syncStyle()
     auto realHeight = height();
     auto realHeightUnits = m_style->heightUnits();
     auto realHeightScaleType = m_style->heightScaleType();
-    fprintf(stderr, "[RIVE DIAG] syncStyle: w=%f wUnits=%d wScale=%d h=%f hUnits=%d hScale=%d intrinsic=%d leaf=%d\n",
-            realWidth, (int)realWidthUnits, (int)realWidthScaleType,
-            realHeight, (int)realHeightUnits, (int)realHeightScaleType,
-            (int)m_style->intrinsicallySized(), (int)isLeaf());
     auto parentIsRow =
         layoutParent() != nullptr ? layoutParent()->mainAxisIsRow() : true;
 
@@ -961,8 +956,6 @@ void LayoutComponent::propagateSizeToChildren(ContainerComponent* component)
             effectiveH = std::max(effectiveH, height());
         }
     }
-    fprintf(stderr, "[RIVE DIAG] propagateSize: m_layout w=%f h=%f, effective w=%f h=%f, authored w=%f h=%f\n",
-            m_layout.width(), m_layout.height(), effectiveW, effectiveH, width(), height());
     for (auto child : component->children())
     {
         if (child->is<LayoutComponent>())
@@ -970,7 +963,6 @@ void LayoutComponent::propagateSizeToChildren(ContainerComponent* component)
             continue;
         }
         auto sizeableChild = IntrinsicallySizeable::from(child);
-        fprintf(stderr, "[RIVE DIAG] propagateSizeToChildren: found child (isSizeable=%d)\n", sizeableChild != nullptr);
         if (sizeableChild != nullptr)
         {
```

#### `rive-runtime/src/shapes/image.cpp`:
```diff
@@ -114,13 +114,11 @@ void Image::markParentLayoutDirty()
     {
         if (parentNode->is<LayoutComponent>())
         {
-            fprintf(stderr, "[RIVE DIAG] markParentLayoutDirty: FOUND parent layout!\n");
             parentNode->as<LayoutComponent>()->markLayoutNodeDirty(true);
             return;
         }
         parentNode = parentNode->parent();
     }
-    fprintf(stderr, "[RIVE DIAG] markParentLayoutDirty: NO layout parent found!\n");
 }
 
 void Image::setAsset(rcp<FileAsset> asset)
@@ -237,7 +235,6 @@ void Image::controlSize(Vec2D size,
                         LayoutScaleType heightScaleType,
                         LayoutDirection direction)
 {
-    fprintf(stderr, "[RIVE DIAG] Image::controlSize called: w=%f, h=%f (prev w=%f, h=%f)\n", size.x, size.y, m_layoutWidth, m_layoutHeight);
     // Always update and recalculate — the render image may have arrived
     // between layout passes, so we must not skip based on cached values.
     m_layoutWidth = size.x;
@@ -256,7 +253,6 @@ void Image::updateImageScale()
 {
     if (imageAsset() == nullptr)
     {
-        fprintf(stderr, "[RIVE DIAG] updateImageScale: imageAsset is NULL, aborting\n");
         if (m_layoutOffsetX != 0.0f || m_layoutOffsetY != 0.0f)
         {
             m_layoutOffsetX = 0.0f;
@@ -269,8 +265,6 @@ void Image::updateImageScale()
     float newOffsetX = 0.0f;
     float newOffsetY = 0.0f;
     auto renderImage = imageAsset()->renderImage();
-    fprintf(stderr, "[RIVE DIAG] updateImageScale: renderImage=%p, layoutW=%f, layoutH=%f, isNanW=%d, isNanH=%d\n",
-            (void*)renderImage, m_layoutWidth, m_layoutHeight, std::isnan(m_layoutWidth), std::isnan(m_layoutHeight));
     if (renderImage != nullptr && !std::isnan(m_layoutWidth) &&
         !std::isnan(m_layoutHeight))
     {
@@ -278,7 +272,6 @@ void Image::updateImageScale()
         float imgH = (float)renderImage->height();
         float newScaleX, newScaleY;
         auto imageFit = static_cast<Fit>(fit());
-        fprintf(stderr, "[RIVE DIAG] updateImageScale: COMPUTING fit=%u, imgW=%f, imgH=%f, layoutW=%f, layoutH=%f\n", fit(), imgW, imgH, m_layoutWidth, m_layoutHeight);
         switch (imageFit)
         {
             case Fit::fill:
@@ -290,7 +283,6 @@ void Image::updateImageScale()
                 float s =
                     std::fmin(m_layoutWidth / imgW, m_layoutHeight / imgH);
                 newScaleX = newScaleY = s;
-                fprintf(stderr, "[RIVE DIAG] updateImageScale: CONTAIN s=%f, newScaleX=%f, newScaleY=%f\n", s, newScaleX, newScaleY);
                 break;
             }
             case Fit::cover:
```

---

## ADDRESSED

Patches upstream now covers, or that turned out to be no-ops. Retired after the 2026-09 sync to `runtime-v0.1.359`; kept for history. Rows here must cite **evidence** (file/line, commit, or the on-air test that passed). **Verify against the fresh upstream before trusting any entry** — this file has been wrong about this before (see Patch 5).

| Patch | Status | Evidence |
|---|---|---|
| **P4** `[RIVE DIAG]` log removal | **No-op — never upstream** | 0 occurrences of `RIVE DIAG` in `src/shapes/image.cpp`, `src/layout_component.cpp`, `include/rive/generated/shapes/image_base.hpp` at v0.1.359; `image_base.hpp` was never patched. They were our own debug lines, since removed. |
| `m_layoutData == nullptr` guard (Patch 7 note) | **Addressed upstream** | `layout_component.cpp` ~1088: `if (m_style == nullptr \|\| m_layoutData == nullptr)`. Do not re-add ours. |
| **P5** `(uint32_t)-1` guard | **NOT addressed — attribution corrected** | Previously (wrongly) listed as fixed upstream. Never in upstream history; still required; re-applied. Listed here only so the correction is visible. |
| **P1 / P2 / P3** dynamic-image-in-layout sizing | **Addressed upstream — verified on air 2026-09-05** | Dropped for a test-first build; the decisive test (dynamic image injected into an `Image` inside a Group) plus all six other checks in `UPDATING_RIVE.md` §8 passed on unmodified `runtime-v0.1.359`. Covered by upstream's restructured `propagateSizeToChildren` (14 commits: `stopsContentSizing()`, `LayoutNodeProvider`, `shouldPropagateSizeToChildren`). Diffs kept in *Retired patches*. |

## Sync log

| Date | From → To | `.rive_head` | Notes |
|---|---|---|---|
| 2026-09-05 | `runtime-v0.1.230` (`4a10679b`) → `runtime-v0.1.359` (`621f2a2e`), 129 upstream commits | `9e498b2635…` → `a87af1c977…` | Git 3-way rebase in the nested workbench (`UPDATING_RIVE.md` §1). Kept P5, P6, P7. **P1/P2/P3 retired** — dropped test-first, then all 7 on-air checks (`UPDATING_RIVE.md` §8) passed on unmodified upstream 2026-09-05. Yoga → `v2_0_1_3_grid` (10 grid symbols required), `rive_yoga` → C++20. Luau → `rive_0_734`. HarfBuzz stays `10.1.0` (only Apple-only `hb_ct_*` missing). Added `WITH_RIVE_SCRIPTING_LUAU`; excluded `src/wasm`. Full record: `docs/RIVE_SYNC_PLAN_2026-09.md`. |
| 2026-09-05 | layout change, same runtime: plain-file tree → **submodule** on fork branch `airz/merge-v0.1.359` (`3eb17fe3`, dependency trees vendored on the branch in `4e9d14ee`); editor pinned to the same commit | unchanged | Content byte-identical to the tracked tree; rollback tag `rive-pre-submodule-2026-09-05`. Docs reconciled the same day (`upstream` remote + tags were missing from the checkout; §2 step 2, §8b, §9 rewritten for the two-parent layout). |
