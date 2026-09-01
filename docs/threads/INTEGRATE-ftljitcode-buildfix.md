# INTEGRATE: cross-tier JITData layout fix (build round)

Status: approved 3/3 by adversarial review. The failing file is
`Source/JavaScriptCore/ftl/FTLJITCode.cpp` (static_asserts at lines 45-48), but the
sound fix lands in `Source/JavaScriptCore/ftl/FTLJITCode.h`, which is owned by another
workstream this round. Per review amendment, the change is routed here instead of
being applied cross-file. `FTLJITCode.cpp` is intentionally left unchanged: the
asserts are load-bearing and must NOT be deleted or weakened (shared handler-IC stubs
in `bytecode/InlineCacheCompiler.cpp` load
`[jitDataRegister + BaselineJITData::offsetOfStackOffset()]` tier-agnostically;
with the current FTL layout they would read `m_globalObject` as a stack offset and
corrupt SP after handler calls).

## Why

`BaselineJITData` (`jit/BaselineJITCode.h:118`) and `DFG::JITData`
(`dfg/DFGJITCode.h:160`) derive from `WTF::ButterflyArray`
(`Source/WTF/wtf/ButterflyArray.h:41`), whose header is two `unsigned`s
(`m_leadingSize`/`m_trailingSize`, bytes 0-7), so their `m_globalObject` /
`m_stackOffset` sit at offsets 8/16. `FTL::JITData` (`ftl/FTLJITCode.h:52`) is a
plain class with the same members at 0/8 — exactly the reported `0 == 8` /
`8 == 16` assert failures. Padding FTL::JITData with an equivalent 8-byte dummy
header makes all four asserts in `FTLJITCode.cpp` pass. `offsetOfDummyArrayProfile`
is `OBJECT_OFFSETOF`-derived and shifts automatically; nothing reads the pad fields.

## Exact change for Source/JavaScriptCore/ftl/FTLJITCode.h

Insertion point: inside `class JITData`, immediately before the `m_globalObject`
member (currently line 61). Replace this line:

```cpp
    JSGlobalObject* m_globalObject { nullptr }; // This is not marked since the owner CodeBlock will mark JSGlobalObject.
```

with:

```cpp
    // BaselineJITData and DFG::JITData derive from WTF::ButterflyArray, whose
    // header (m_leadingSize/m_trailingSize, two unsigneds) occupies the first
    // 8 bytes, putting m_globalObject/m_stackOffset at offsets 8/16. FTL::JITData
    // is not a ButterflyArray, so pad with an equivalent dummy header to keep the
    // field offsets identical across tiers for the shared handler-IC stubs
    // (enforced by the static_asserts in FTLJITCode.cpp).
    unsigned m_unusedButterflyArrayLeadingSize { 0 };
    unsigned m_unusedButterflyArrayTrailingSize { 0 };
    JSGlobalObject* m_globalObject { nullptr }; // This is not marked since the owner CodeBlock will mark JSGlobalObject.
```

No other change anywhere. Do not modify `FTLJITCode.cpp`.
