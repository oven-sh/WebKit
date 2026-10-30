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
#include <wtf/Noncopyable.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC {

class DeletePropertySlot;
class PropertySlot;
class PutPropertySlot;

namespace AOT {

class Object;
class PropertyCaches;

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
    Removal,
    ExtraPropertyRemoval,
    FieldRemoval,
    NothingToRemove,
    ArrayLength,
    TypedArrayLength,
    StringLength,
    ClosureLength,
    ClosureName,
    Constant,
};

struct PropertyAnswer {
    static PropertyAnswer uncacheable(SlotAccess access, bool didFlatten = false) { return { .access = access, .didFlatten = didFlatten }; }
    static PropertyAnswer handlerOfProxyAnswers(SlotAccess access, JSCell* key) { return { .access = access, .keyForTrap = key }; }
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
    // Slot::typeIsGivenByBits, of a store to a field of another image.
    uint32_t bitsOfType { 0 };
    bool readsByteOffset { false };
    std::optional<std::pair<uint32_t, uint32_t>> orderWords { };
    bool didFlatten { false };
    PropertyOffset offset { invalidOffset };
    JSObject* holder { nullptr };
    Structure* newStructure { nullptr };
    uint32_t newStorageCapacity { 0 };
    bool storageHasIndexingHeader { false };
    const FieldData* field { nullptr };
    SlotData variable { };
    // Of a variable of another image of the realm: the position of that image and one, in the place of the offset of a hidden word and in its unit. Zero: the variable is of the image that asks.
    uint16_t imageOfVariable { 0 };
    uint32_t customAccessor { 0 };
    JSValue constant { };
    // Of an access that is not cacheable because the receiver is a proxy: what the trap is handed as the key.
    JSCell* keyForTrap { nullptr };
    uint16_t columnOfNameForTrap { 0 };
    ChainObjects changeableObjects { };
    int16_t wordOfInheritedField { 0 };
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

// The deletion has been made and has succeeded.
struct DeletionQuery {
    JSGlobalObject* globalObject;
    JSObject* base;
    Structure* structureBefore;
    uint32_t orderWordBefore;
    UniquedStringImpl* uid;
    const DeletePropertySlot& slot;
};

PropertyAnswer classifyRead(const ReadQuery&);
PropertyAnswer classifyStore(const StoreQuery&);
PropertyAnswer classifyDeletion(const DeletionQuery&);
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
// See PropertyCaches::prepareHoldersBeforeLookupsUnder().
bool isPreparedBeforeLookupsSlow(VM&, UniquedStringImpl* symbol);
ALWAYS_INLINE bool isPreparedBeforeLookups(VM& vm, UniquedStringImpl* uid) { return uid->isSymbol() && isPreparedBeforeLookupsSlow(vm, uid); }
// Calls what the library has registered for the name, if anything, on the compiled object. May throw.
void prepareForLookup(JSGlobalObject*, JSObject* compiledObject, UniquedStringImpl*);
// The same for each field of the object, before its values are handed out one after the other.
void prepareForWalk(JSGlobalObject*, JSObject*);
// An operation of compiled code that names the property. Only the text of the library names one by such a symbol, and it finds there what the library keeps there: while one of these lives, the object is not asked.
class LookByName {
    WTF_MAKE_NONCOPYABLE(LookByName);
public:
    ALWAYS_INLINE LookByName(VM& vm, JSValue holder, UniquedStringImpl* uid)
    {
        if (uid->isSymbol()) [[unlikely]]
            begin(vm, holder, uid);
    }

    ALWAYS_INLINE ~LookByName()
    {
        if (m_caches) [[unlikely]]
            end();
    }

private:
    void begin(VM&, JSValue holder, UniquedStringImpl* symbol);
    void end();

    PropertyCaches* m_caches { nullptr };
    JSCell* m_previous { nullptr };
};
std::optional<PropertyOffset> findOffset(VM&, Structure*, UniquedStringImpl*, ChainWalkMode, unsigned& attributes);
bool structureProvesOwnAbsence(VM&, JSCell* base, Structure*, UniquedStringImpl*, ChainWalkMode);
bool hasOrdinaryDefineForName(VM&, JSCell* base, Structure*, UniquedStringImpl*);
bool isHostObjectOverridingPut(Structure*);
bool flattenOnce(VM&, JSObject*);
bool flattenForListing(VM&, JSObject*);
bool isLastAdditionOfRun(Structure* before, Structure* after, UniquedStringImpl*);
// For an addition that leads from one Structure to the next. Zero: the storage out of line stays. Nothing: no stub makes the addition.
std::optional<uint32_t> capacityOfStorageGrownByStub(VM&, Structure* before, Structure* after, PropertyOffset);
bool slotAgreesWithStructures(VM&, const Slot&, SlotAccess, UniquedStringImpl*);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
