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

class JSCell;
class JSGlobalObject;
class JSObject;
class PropertySlot;
class PutPropertySlot;
class Structure;
class VM;

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
    static PropertyAnswer uncacheable(SlotAccess access, bool didFlatten = false)
    {
        PropertyAnswer answer;
        answer.access = access;
        answer.didFlatten = didFlatten;
        return answer;
    }

    bool isCacheable() const { return kind != PropertyAnswerKind::Uncacheable; }
    bool dependsOnChain() const { return !changeableObjects.isEmpty(); }

    PropertyAnswerKind kind { PropertyAnswerKind::Uncacheable };
    SlotAccess access { SlotAccess::Read };
    bool isOwn { false };
    bool absentIsUndefined { false };
    bool emptySlotMayBeUndefined { false };
    bool undefinedNeedsNoStorage { false };
    // Of FieldCreation: the order word of the receiver before and after, where whoever asks has seen both and the field, in a slot in the cell that holds any value as it is, takes any value.
    std::optional<std::pair<uint32_t, uint32_t>> orderWords;
    bool didFlatten { false };
    PropertyOffset offset { invalidOffset };
    JSObject* holder { nullptr };
    Structure* newStructure { nullptr };
    uint32_t newStorageCapacity { 0 };
    bool storageHasIndexingHeader { false };
    const FieldData* field { nullptr };
    SlotData variable { };
    uint32_t customAccessor { 0 };
    JSValue constant;
    ChainObjects changeableObjects;
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
// False for the first object that gets this property after the ones the Structure lists. An addition is filed when it is repeated: code that runs once leaves no slots behind.
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

PropertyAnswer classifyCompiledObjectRead(const ReadQuery&, Object&);
PropertyAnswer classifyCompiledObjectStore(const StoreQuery&, Object&);
PropertyAnswer classifyNamespaceRead(const ReadQuery&);
// The base is the global proxy of query.globalObject.
PropertyAnswer classifyGlobalProxyRead(const ReadQuery&);
// The objects that a slot filed under the Structure of a global proxy depends on. Allocates nothing, and is Unknown for the proxy of another realm.
ChainWalk walkChainThroughGlobalProxy(JSGlobalObject*, const Slot&, UniquedStringImpl*);
PropertyAnswer classifyCustomGetter(const ReadQuery&);
PropertyAnswer classifyAbsentClosurePrototype(const ReadQuery&);
PropertyAnswer classifyCustomSetter(const StoreQuery&);

Structure* structureForPrimitive(JSGlobalObject*, JSValue primitive);

constexpr bool isProxyType(JSType type) { return type == ProxyObjectType || type == GlobalProxyType; }
bool isNeverCachedName(VM&, UniquedStringImpl*);
bool isFunctionOwnName(VM&, UniquedStringImpl*);
bool hasCacheableArrayLength(VM&, JSCell* base, Structure*, UniquedStringImpl*);
std::optional<PropertyOffset> findOffset(VM&, Structure*, UniquedStringImpl*, ChainWalkMode, unsigned& attributes);
bool structureProvesOwnAbsence(VM&, JSCell* base, Structure*, UniquedStringImpl*, ChainWalkMode);
bool hasOrdinaryDefineForName(VM&, JSCell* base, Structure*, UniquedStringImpl*);
bool hasOrdinaryDefineOwnProperty(Structure*);
bool isHostObjectOverridingPut(Structure*);
bool flattenOnce(VM&, JSObject*);

enum class OutOfLineStorageChange : uint8_t { Unsupported, Stays, IsFirst, Doubled, GrowsBesideIndexingHeader };

// A literal adds its properties at once, and their Structure comes behind that of the empty object.
bool isLastAdditionOfRun(Structure* before, Structure* after, UniquedStringImpl*);

// leadsToStructureOfRun: isLastAdditionOfRun() holds, so the Structure does not name the one in front of the addition as its predecessor.
OutOfLineStorageChange outOfLineStorageChange(Structure* before, Structure* after, PropertyOffset, bool leadsToStructureOfRun = false);

enum class StructureLookup : uint8_t { Unknown, Absent, Own, Inherited };

struct StructureLookupResult {
    StructureLookup outcome { StructureLookup::Unknown };
    JSObject* holder { nullptr };
    PropertyOffset offset { invalidOffset };
    unsigned attributes { 0 };
};

StructureLookupResult lookUpByStructure(VM&, Structure*, UniquedStringImpl*);
bool slotAgreesWithStructures(VM&, const Slot&, SlotAccess, UniquedStringImpl*);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
