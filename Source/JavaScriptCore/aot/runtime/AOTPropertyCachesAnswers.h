/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTLayoutData.h"
#include "AOTRuntimeRecords.h"
#include "JSType.h"
#include "PropertyOffset.h"
#include <optional>

namespace JSC {

class PropertySlot;
class PutPropertySlot;

namespace AOT {

class Object;

enum class PropertyAnswerKind : uint8_t {
    Uncacheable,
    Value,
    Absent,
    Accessor,
    CustomAccessor,
    Field,
    FieldAccessor,
    FieldCreation,
    FieldOrInheritedValue,
    FieldOrInheritedAccessor,
    InheritedField,
    FieldOrInheritedField,
    Variable,
    ExtraProperty,
    ExtraAccessor,
    ExtraPropertyAddition,
    Addition,
    ArrayLength,
    TypedArrayLength,
    StringLength,
    ClosureLength,
    ClosureName,
    Constant,
};

struct PropertyAnswer {
    static PropertyAnswer uncacheable(SlotAccess access, bool didFlatten = false) { return { .access = access, .didFlatten = didFlatten }; }
    static PropertyAnswer uncacheableAfter(SlotAccess access, ChainWalk&& walk) { return { .access = access, .didFlatten = walk.didFlatten, .changeableObjects = WTF::move(walk.changeableObjects) }; }
    static PropertyAnswer own(PropertyAnswerKind kind, SlotAccess access, PropertyOffset offset = invalidOffset) { return { .kind = kind, .access = access, .isOwn = true, .offset = offset }; }
    static PropertyAnswer above(PropertyAnswerKind kind, SlotAccess access, ChainWalk&& walk) { return { .kind = kind, .access = access, .offset = walk.offset, .holder = walk.holder, .changeableObjects = WTF::move(walk.changeableObjects) }; }

    bool isCacheable() const { return kind != PropertyAnswerKind::Uncacheable; }
    bool dependsOnChain() const { return !changeableObjects.isEmpty(); }

    PropertyAnswerKind kind { PropertyAnswerKind::Uncacheable };
    SlotAccess access { SlotAccess::Read };
    bool isOwn { false };
    bool absentIsUndefined { false };
    bool emptySlotMayBeUndefined { false };
    bool undefinedNeedsNoStorage { false };
    std::optional<std::pair<uint32_t, uint32_t>> orderWords { };
    bool didFlatten { false };
    PropertyOffset offset { invalidOffset };
    JSObject* holder { nullptr };
    Structure* newStructure { nullptr };
    uint32_t newStorageCapacity { 0 };
    bool storageHasIndexingHeader { false };
    const FieldData* field { nullptr };
    SlotData variable { };
    uint32_t customAccessor { 0 };
    JSValue constant { };
    ChainObjects changeableObjects { };
};

struct ReadQuery {
    JSGlobalObject* globalObject;
    JSValue base;
    Structure* structureBefore;
    UniquedStringImpl* uid;
    const PropertySlot& slot;
    ChainWalkMode mode { ChainWalkMode::MayAllocate };
};

struct StoreQuery {
    JSGlobalObject* globalObject;
    JSObject* base;
    Structure* structureBefore;
    UniquedStringImpl* uid;
    const PutPropertySlot& slot;
    PutMode mode;
};

PropertyAnswer classifyRead(const ReadQuery&);
PropertyAnswer classifyStore(const StoreQuery&);
PropertyAnswer classifyCompiledObjectStore(const StoreQuery&, Object&);
PropertyAnswer classifyNamespaceRead(const ReadQuery&);
PropertyAnswer classifyGlobalProxyRead(const ReadQuery&);
ChainWalk walkChainThroughGlobalProxy(JSGlobalObject*, const Slot&, UniquedStringImpl*);
bool repeatsPropertyAddition(Structure*, UniquedStringImpl*);

Slot slotFor(const PropertyAnswer&, Structure* receiverStructure);

enum class LazyPrototypeReadResult : uint8_t { NotApplicable, Answered, ContinuesAbove };

struct LazyPrototypeRead {
    LazyPrototypeReadResult result { LazyPrototypeReadResult::NotApplicable };
    PropertyAnswer answer;
    JSObject* firstObjectAbove { nullptr };
};

LazyPrototypeRead classifyReadThroughLazyPrototypes(JSGlobalObject*, JSObject* base, UniquedStringImpl*);
JSValue readThroughAnswer(JSGlobalObject*, JSValue base, const PropertyAnswer&);

Structure* structureForPrimitive(JSGlobalObject*, JSValue primitive);
constexpr bool isProxyType(JSType type) { return type == ProxyObjectType || type == GlobalProxyType; }
bool isNeverCachedName(VM&, UniquedStringImpl*);
std::optional<PropertyOffset> findOffset(VM&, Structure*, UniquedStringImpl*, ChainWalkMode, unsigned& attributes);
bool structureProvesOwnAbsence(VM&, JSCell* base, Structure*, UniquedStringImpl*, ChainWalkMode);
bool hasOrdinaryDefineForName(VM&, JSCell* base, Structure*, UniquedStringImpl*);
bool isHostObjectOverridingPut(Structure*);
bool flattenOnce(VM&, JSObject*);
bool flattenForListing(VM&, JSObject*);
bool isLastAdditionOfRun(Structure* before, Structure* after, UniquedStringImpl*);
bool slotAgreesWithStructures(VM&, const Slot&, SlotAccess, UniquedStringImpl*);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
