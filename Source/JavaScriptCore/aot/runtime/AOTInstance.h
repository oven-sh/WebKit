/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinWords.h"
#include "AOTCodegenScratchWords.h"
#include "AOTCoroutineWords.h"
#include "AOTHeapWords.h"
#include "AOTInstanceLayout.h"
#include "AOTIterationWords.h"
#include "AOTJSONWords.h"
#include "AOTModuleWords.h"
#include "AOTObjectWords.h"
#include "AOTProgramTables.h"
#include "AOTPropertyCachesWords.h"
#include "AOTRuntimeRecords.h"
#include "AOTRuntimeWords.h"
#include "AOTStringBuilderWords.h"
#include "AOTStubWords.h"
#include "AOTStubsArrayWords.h"
#include "IndexingType.h"
#include "JSCJSValue.h"
#include "JSGlobalObject.h"
#include "LinkTimeConstant.h"
#include "RememberedSlot.h"
#include "StackManager.h"
#include "TypedArrayType.h"
#include <span>
#include <type_traits>

#define AOT_INSTANCE_OFFSET(path) OBJECT_OFFSETOF(JSC::AOT::Instance, path)
#define AOT_INSTANCE_PREFIX_OFFSET(member) (static_cast<ptrdiff_t>(OBJECT_OFFSETOF(JSC::AOT::InstancePrefix, member)) - static_cast<ptrdiff_t>(sizeof(JSC::AOT::InstancePrefix)))

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

struct ClassInfo;
class MicrotaskQueue;
class VM;

namespace AOT {

struct ImageFunction;
struct RecordedOptions;
struct State;

enum class Receiver : uint8_t { Map, Set, RegExp };
static constexpr unsigned numberOfReceivers = 3;
static_assert(static_cast<unsigned>(Receiver::RegExp) + 1 == numberOfReceivers);

template<typename T>
class ZeroedStorage {
public:
    T& get() { return *reinterpret_cast<T*>(m_bytes); }
    const T& get() const { return *reinterpret_cast<const T*>(m_bytes); }

private:
    alignas(T) uint8_t m_bytes[sizeof(T)];
};

struct InstancePrefix {
    uint64_t newArrayWithInt32Header;
    uint64_t newArrayWithDoubleHeader;
    uint64_t newArrayWithContiguousHeader;
    int64_t notUndefinedOrNullMask;
    uint64_t newCopyOnWriteArrayWithContiguousHeader;
    uint64_t stringHeader;
    void* ropeStringAllocator;
    void* arrayAllocator;
    uint32_t collectionStructureIDs[numberOfReceivers];
    uint32_t unused;
    JSCell** builtinCells;
    uintptr_t structureIDBase;
    EncodedJSValue boxedNaN;
    JSCell* emptyString;
};

struct InstanceHeader {
    void** runtimeTable;
    const uint32_t* shapeSets;
    VM* vm;
    ZeroedStorage<StackManager::Mirror> stackMirror;
    uint32_t barrierThreshold;
    uint8_t mutatorShouldBeFenced;
    uint8_t awaitMustSuspend;
    uint64_t slimClosureHeaders[numberOfSlimClosureKinds];
    uint64_t shapeEpoch;
    void* closureCells;
    void* singleCharacterStrings;
    int64_t numberTag;
    int64_t notCellMask;
    JSGlobalObject* globalObject;
    uintptr_t cellCageBase;
};

struct ImageWords {
    const uint8_t* code;
    const uint32_t* codeGranules;
    const uint32_t* subsequentFunctionStarts;
    const ProgramTables* tables;
    UniquedStringImpl* const* programIdentifiers;
    const uint32_t* fieldTableEntries;
    const uint16_t* fieldRecordIndices;
};

enum class InlineAllocation : unsigned { Promise, Map, BoundFunction, BigInt, DefaultFinalObject, RegExpObject, NullPrototypeObject, RegExpMatchesArray };
static constexpr unsigned numberOfInlineAllocations = 8;
static_assert(static_cast<unsigned>(InlineAllocation::RegExpMatchesArray) + 1 == numberOfInlineAllocations);

struct InlineAllocationData {
    void* allocator;
    uint64_t header;
};

struct TypedArrayAllocation {
    void* allocator;
    uint64_t header;
    uint64_t elementSize;
    uint64_t unused;
};

struct AllocationWords {
    static constexpr unsigned numberOfTypedArrayAllocations = 16;

    void* auxiliarySpace;
    void* cagedAuxiliarySpace;
    void* cellSpace;
    void* primitiveAuxiliarySpace;
    InlineAllocationData inlineAllocations[numberOfInlineAllocations];
    TypedArrayAllocation typedArrayAllocations[numberOfTypedArrayAllocations];
    uint64_t newCopyOnWriteArrayHeaders[2];
    uint64_t compactTableHeader;
};

// Most objects that have extra properties have one or two.
static constexpr unsigned extraPropertiesInlineCapacity = 2;

struct RealmWords {
    JSObject* objectPrototype;
    JSCell* functionPrototypeCall;
    JSCell* boundFunctionExecutable;
    Structure* emptyExtraPropertiesStructure;
    const ClassInfo* boundFunctionClassInfo;
    const ClassInfo* const* instanceClassInfos;
    uint32_t boundFunctionStructureID;
    uint32_t hostFunctionStructureID;
    EncodedJSValue linkTimeConstants[JSC::numberOfLinkTimeConstants];
    const void* smallIntStrings;
    const void* intStrings;
    MicrotaskQueue* microtaskQueue;
};

struct NumberWords {
    const double* powersOfTen;
    uint64_t hashSecrets[2];
};

#define FOR_EACH_AOT_INSTANCE_PART(v) \
    v(propertyCaches, PropertyCacheWords) \
    v(image, ImageWords) \
    v(allocation, AllocationWords) \
    v(heap, HeapWords) \
    v(realm, RealmWords) \
    v(numbers, NumberWords) \
    v(runtime, RuntimeWords) \
    v(modules, ModuleWords) \
    v(objects, ObjectWords) \
    v(closures, ClosureWords) \
    v(classes, ClassWords) \
    v(coroutines, CoroutineWords) \
    v(builtins, BuiltinWords) \
    v(arrays, ArrayWords) \
    v(iteration, IterationWords) \
    v(json, JSONWords) \
    v(stringBuilder, StringBuilderWords) \
    v(stubs, StubWords) \
    v(scratch, CodegenScratchWords)

struct Instance : InstanceHeader {
#define AOT_DECLARE_INSTANCE_PART(member, Type) Type member;
    FOR_EACH_AOT_INSTANCE_PART(AOT_DECLARE_INSTANCE_PART)
#undef AOT_DECLARE_INSTANCE_PART
    State* state;

    static Instance* of(JSGlobalObject* realm) { return realm->aotInstance(); }
    static Instance* create(JSGlobalObject* realm);
    JS_EXPORT_PRIVATE static void destroy(Instance*);
    JS_EXPORT_PRIVATE static void destroyDeadInstances(VM&);
    JS_EXPORT_PRIVATE static void destroyAll(VM&);

    InstancePrefix& prefix() { return reinterpret_cast<InstancePrefix*>(this)[-1]; }
    const InstancePrefix& prefix() const { return reinterpret_cast<const InstancePrefix*>(this)[-1]; }
    const ProgramTables& tables() const { return *image.tables; }
    const RecordedOptions& options() const;

    void bumpShapeEpoch() { ++shapeEpoch; }
    void captureCellHeaders();
    void didCreateInstanceStructure(BuiltinClassID, Structure*);

    uint8_t* statics() { return reinterpret_cast<uint8_t*>(this) + InstanceLayout::offsetOfStatics(tables().instanceSizes); }
    const uint8_t* statics() const { return reinterpret_cast<const uint8_t*>(this) + InstanceLayout::offsetOfStatics(tables().instanceSizes); }
    uint8_t* cards() { return reinterpret_cast<uint8_t*>(this) + InstanceLayout::offsetOfCards(tables().instanceSizes); }
    RememberedSlot<uint32_t>& stringCell(uint32_t number)
    {
        ASSERT(number < tables().instanceSizes.numberOfStringCells);
        return *reinterpret_cast<RememberedSlot<uint32_t>*>(reinterpret_cast<uint8_t*>(this) + InstanceLayout::offsetOfStringCell(number));
    }
    std::span<RememberedSlot<Slot>> siteCaches()
    {
        InstanceSizes sizes = tables().instanceSizes;
        return { reinterpret_cast<RememberedSlot<Slot>*>(reinterpret_cast<uint8_t*>(this) + InstanceLayout::offsetOfSiteCaches(sizes)), sizes.numberOfSiteCaches };
    }

    static constexpr ptrdiff_t offsetOfShapeEpoch() { return OBJECT_OFFSETOF(InstanceHeader, shapeEpoch); }
    static constexpr ptrdiff_t offsetOfReceiverStructureID(Receiver receiver) { return AOT_INSTANCE_PREFIX_OFFSET(collectionStructureIDs) + static_cast<unsigned>(receiver) * static_cast<ptrdiff_t>(sizeof(uint32_t)); }
    uint32_t& receiverStructureID(Receiver receiver) { return prefix().collectionStructureIDs[static_cast<unsigned>(receiver)]; }

    static constexpr ptrdiff_t offsetOfNewArrayHeader(IndexingType indexingType)
    {
        switch (indexingType) {
        case ArrayWithInt32:
            return AOT_INSTANCE_PREFIX_OFFSET(newArrayWithInt32Header);
        case ArrayWithDouble:
            return AOT_INSTANCE_PREFIX_OFFSET(newArrayWithDoubleHeader);
        case ArrayWithContiguous:
            return AOT_INSTANCE_PREFIX_OFFSET(newArrayWithContiguousHeader);
        }
        RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
    }

    static constexpr ptrdiff_t offsetOfNewCopyOnWriteArrayHeader(IndexingType);

    template<typename Visitor>
    void visitRoots(Visitor& visitor)
    {
        JSCell* const cells[] = { realm.objectPrototype, realm.functionPrototypeCall, realm.boundFunctionExecutable, realm.emptyExtraPropertiesStructure };
        for (JSCell* cell : cells) {
            if (cell)
                visitor.appendUnbarriered(cell);
        }
    }
};

constexpr ptrdiff_t Instance::offsetOfNewCopyOnWriteArrayHeader(IndexingType indexingType)
{
    switch (indexingType & IndexingShapeMask) {
    case Int32Shape:
        return AOT_INSTANCE_OFFSET(allocation.newCopyOnWriteArrayHeaders[0]);
    case DoubleShape:
        return AOT_INSTANCE_OFFSET(allocation.newCopyOnWriteArrayHeaders[1]);
    case ContiguousShape:
        return AOT_INSTANCE_PREFIX_OFFSET(newCopyOnWriteArrayWithContiguousHeader);
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static_assert(std::is_trivially_default_constructible_v<Instance>, "Instance lives in zeroed pages and is never constructed, so default member initializers would be ignored");
static_assert(std::is_trivially_default_constructible_v<InstancePrefix>);
static_assert(std::is_trivially_default_constructible_v<InstanceHeader>);
#define AOT_CHECK_INSTANCE_PART(member, Type) static_assert(std::is_trivially_default_constructible_v<Type>);
FOR_EACH_AOT_INSTANCE_PART(AOT_CHECK_INSTANCE_PART)
#undef AOT_CHECK_INSTANCE_PART
static_assert(OBJECT_OFFSETOF(InstanceHeader, vm) == 16, "JSWebAssemblyInstance::offsetOfVM()");
static_assert(!OBJECT_OFFSETOF(InstanceHeader, runtimeTable));
static_assert(OBJECT_OFFSETOF(InstanceHeader, globalObject) <= 128);
static_assert(Instance::offsetOfShapeEpoch() < 128);
static_assert(sizeof(InstancePrefix) <= 128, "On x86-64, an operand within 128 bytes below the instance takes a one-byte displacement, and one farther away takes four bytes.");
static_assert(sizeof(Instance) <= 32 * KB, "On ARM64, the immediate offset of a 64-bit load reaches 32 KB.");
static_assert(AOT_INSTANCE_OFFSET(propertyCaches.hashedSlots) <= 504, "On ARM64, the immediate offset of a load of a pair reaches 504 bytes.");
static_assert(sizeof(Instance) <= InstanceLayout::offsetOfStringCells);
static_assert(sizeof(InstancePrefix) + InstanceLayout::bytesReservedForLoopPollPage <= InstanceLayout::bytesReservedForPrefixAndPollPage);
static_assert(sizeof(RememberedSlot<Slot>) == InstanceLayout::sizeOfSiteCache && sizeof(RememberedSlot<uint32_t>) == InstanceLayout::sizeOfStringCell);
static_assert(sizeof(TypedArrayAllocation) == 32);

JS_EXPORT_PRIVATE JSCell* livenessToken(Instance*);

constexpr uint64_t runtimeLayoutHash()
{
    constexpr uint64_t numbers[] = {
        sizeof(InstancePrefix), sizeof(InstanceHeader), sizeof(Instance),
#define AOT_HASH_INSTANCE_PART(member, Type) OBJECT_OFFSETOF(Instance, member), sizeof(Type),
        FOR_EACH_AOT_INSTANCE_PART(AOT_HASH_INSTANCE_PART)
#undef AOT_HASH_INSTANCE_PART
        OBJECT_OFFSETOF(Instance, state),
        sizeof(Slot), sizeof(PackedSlot), sizeof(SiteCacheState), sizeof(CustomAccessorRecord), numberOfSlotKinds,
        sizeof(AllocationSiteRecord), sizeof(KeyOrderTransition), sizeof(KeyListCacheEntry),
        sizeof(LayoutData), sizeof(RareLayoutData), sizeof(MessageLayoutData), sizeof(ClassData), sizeof(FieldData), sizeof(MethodData), sizeof(MemberGuardData),
        sizeof(EnvironmentData), sizeof(KeyOrderData), sizeof(ShapeTestData), sizeof(RuntimeType), sizeof(RuntimeCellTest),
        numberOfProgramTables,
#define AOT_HASH_PROGRAM_TABLE(accessor, ElementType) sizeof(ElementType),
        FOR_EACH_AOT_PROGRAM_TABLE(AOT_HASH_PROGRAM_TABLE)
#undef AOT_HASH_PROGRAM_TABLE
        InstanceLayout::offsetOfStringCells, InstanceLayout::bytesReservedForPrefixAndPollPage, InstanceLayout::granuleBelowInstance, InstanceLayout::granuleAboveInstance, InstanceLayout::cardShift,
    };
    uint64_t hash = 14695981039346656037ull;
    for (uint64_t number : numbers)
        hash = (hash ^ number) * 1099511628211ull;
    return hash;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
