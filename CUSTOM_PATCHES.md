# Custom Patches Applied to Rive Runtime

This document details the custom modifications and patches applied to the `rive-runtime` C++ engine in our fork of the repository. If you update the `rive-runtime` codebase from the upstream repository (`https://github.com/rive-app/rive-runtime.git`), you will need to re-verify or re-apply these changes to ensure compatibility with dynamic image injection (DataBinding) and real-time playout performance.

---

## Patch 1: Fixed Stale Yoga Layout Cache in Layout Size Propagation
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

## Patch 5: Nested Component Default Animation Fallback
> [!NOTE]
> **Fixed natively upstream as of runtime-v0.1.230 — no longer re-applied.** The
> `animationId() == (uint32_t)-1` underflow guard now lives in
> `NestedStateMachine::initializeAnimation` (`src/animation/nested_state_machine.cpp`),
> which is where it belongs. Do NOT re-add the loop change to `onAddedClean`.
> Kept here for history and in case a future runtime regresses it.

* **File:** [nested_artboard.cpp](file:///c:/Users/kutla/Desktop/Github/CasparFork/rive-runtime/src/nested_artboard.cpp)
* **Function:** NestedArtboard::onAddedClean
* **Description:** When an animationId defaults to -1 (meaning no specific animation selected, use default), the uint32_t type underflows to 4294967295. This caused nested image sequences to fail loading because the state machine at index 4294967295 does not exist.
* **Resolution:** Replaced the 
sm->animationId() < 0 logic with 
sm->animationId() == (uint32_t)-1 and correctly applied the default state machine fallback.

### Diff:
``diff
@@ -371,7 +371,11 @@ StatusCode NestedArtboard::onAddedClean(CoreContext* context)
         if (animation->is<NestedStateMachine>())
         {
             auto nsm = animation->as<NestedStateMachine>();
-            if (nsm->animationId() < 0)
+            if (nsm->animationId() == (uint32_t)-1)
             {
                 int defaultId = m_Instance->defaultStateMachineIndex();
                 nsm->animationId(defaultId >= 0 ? defaultId : 0);
``

---

## Patch 6: Nested Component Empty Animation Fallback
* **File:** [nested_artboard.cpp] 
* **Function:** NestedArtboard::onAddedClean`n* **Description:** When an authored nested artboard lacks an active default animation, m_NestedAnimations is empty and DataBinds fail to propagate into it. We must force-instantiate the default StateMachine.
* **Resolution:** Injected a fallback to create and append a NestedStateMachine if the vector is empty.

---

## Patch 7: Compile Yoga/layout with `/fp:precise` (fixes ALL layout / "Stateful Components")
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

