/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinWords.h"
#include "AOTCodegenScratchWords.h"
#include "AOTCoroutineWords.h"
#include "AOTInstanceLayout.h"
#include "AOTJSONWords.h"
#include "AOTModuleWords.h"
#include "AOTObjectWords.h"
#include "AOTProgramTables.h"
#include "AOTPropertyCachesWords.h"
#include "AOTRuntimeRecords.h"
#include "AOTRuntimeWords.h"
#include "AOTSlotRepresentation.h"
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
#include <wtf/text/UniquedStringImpl.h>

#define AOT_INSTANCE_OFFSET(path) OBJECT_OFFSETOF(JSC::AOT::Instance, path)
#define AOT_INSTANCE_PREFIX_OFFSET(member) (static_cast<ptrdiff_t>(OBJECT_OFFSETOF(JSC::AOT::InstancePrefix, member)) - static_cast<ptrdiff_t>(sizeof(JSC::AOT::InstancePrefix)))

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

struct ClassInfo;
class MarkStackArray;
class MicrotaskQueue;
class StoredCellLog;
class VM;

namespace AOT {

struct ImageFunction;
struct ImageStringRecord;
struct ProgramSymbols;
struct RecordedOptions;
struct State;

enum class Receiver : uint8_t { Map, Set, RegExp, Buffer };
static constexpr unsigned numberOfReceivers = 4;
static_assert(static_cast<unsigned>(Receiver::Buffer) + 1 == numberOfReceivers);

template<typename T>
class ZeroedStorage {
public:
    T& get() { return *reinterpret_cast<T*>(m_bytes); }
    const T& get() const { return *reinterpret_cast<const T*>(m_bytes); }

private:
    alignas(T) uint8_t m_bytes[sizeof(T)];
};

// A function that writes an object whose type may hold itself compares its frame with this limit. It is the limit of the stack, or lies above every frame while a string builder watches which objects are written inside which.
struct JSONLevelWords {
    ZeroedStorage<StackManager::Mirror> limit;
};

// On x86-64 an operand within 128 bytes below the instance takes a one-byte displacement, and one farther away takes four bytes. What compiled code names most lies within.
struct InstancePrefix {
    int64_t minInt64;
    uint64_t newCopyOnWriteArrayWithContiguousHeader;
    uint64_t stringHeader;
    void* ropeStringAllocator;
    uint32_t collectionStructureIDs[numberOfReceivers];
    EncodedJSValue boxedNaN;
    uint64_t newArrayWithInt32Header;
    uint64_t newArrayWithDoubleHeader;
    uint64_t newArrayWithContiguousHeader;
    // Constants that take ten bytes as an immediate: AOTB3Policy.cpp.
    uint64_t doubleEncodeOffset;
    uint64_t boxedOne;
    int64_t undefinedInteger;
    int64_t nullInteger;
    int64_t absentInteger;
    uint64_t undefinedDouble;
    uint64_t nullDouble;
    uint64_t absentDouble;
    double one;
    double pureNaN;
    int64_t notUndefinedOrNullMask;
    void* arrayAllocator;
    JSCell** builtinCells;
    uintptr_t structureIDBase;
    JSCell* emptyString;
};

struct InstanceHeader {
#if defined(BUN_HARDENED)
    uint64_t unused;
#else
    void** runtimeTable;
#endif
    const uint64_t* shapeSets;
    VM* vm;
    ZeroedStorage<StackManager::Mirror> stackMirror;
    uint32_t barrierThreshold;
    uint8_t mutatorShouldBeFenced;
    uint8_t awaitMustSuspend;
    uint64_t slimClosureHeaders[numberOfSlimClosureKinds];
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
    union {
        // The atoms of the image, read-only, one header per identifier number: the name with the number n is programIdentifiers + n.
        UniquedStringImpl* programIdentifiers;
        const ImageStringRecord* stringRecords;
    };
    union {
        uint64_t sizeOfProgramIdentifiers;
        const uint32_t* lazyStringHeaderOffsets;
    };
    // ImageSection::Strings, one ImageStringWord for each StringID, and ImageSection::StringConstants, the StringID of each string cell.
    const uint32_t* stringWords;
    const uint32_t* stringConstants;
    // ImageSection::LongStrings, which says where the strings without a header are, and ImageSection::Text.
    const uint32_t* listedStrings;
    const uint8_t* text;
    // The symbols and private names made so far, by identifier number. The table grows, this record stays where it is.
    const ProgramSymbols* programSymbols;
    const uint32_t* fieldTableEntries;
    const uint16_t* fieldRecordIndices;
    const uint64_t* classAccessors;
    // ProgramTables::data(): compiled code points an array of constants at the storage in its node.
    const uint64_t* data;
    // Where the kind of function 0 would lie if ProgramTables::closureFunctionKinds() began with it: a stub adds the number of a closure's function.
    uintptr_t closureFunctionKindsOrigin;
};

struct LazyStringWords {
    uint8_t* headers;
    uint64_t capacity;
};

// Behind DefaultFinalObject: the same object with room in its cell for two more properties, for four more and so on.
enum class InlineAllocation : unsigned { Promise, Map, BoundFunction, BigInt, RegExpObject, NullPrototypeObject, RegExpMatchesArray, Set, DefaultFinalObject };
static constexpr unsigned numberOfRoomierFinalObjects = 13;
static constexpr unsigned numberOfInlineAllocations = 9 + numberOfRoomierFinalObjects;
static_assert(static_cast<unsigned>(InlineAllocation::DefaultFinalObject) + 1 + numberOfRoomierFinalObjects == numberOfInlineAllocations);

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

struct ClonedStructure {
    uint64_t header;
    uint64_t cellSize;
};

struct AllocationWords {
    static constexpr unsigned numberOfTypedArrayAllocations = 16;
    static constexpr unsigned numberOfClonedStructures = 16;
    static constexpr unsigned clonedStructureShift = 4;
    static constexpr unsigned placeOfClonedStructure(uint32_t structureID) { return structureID >> clonedStructureShift & (numberOfClonedStructures - 1); }

    void* auxiliarySpace;
    void* cagedAuxiliarySpace;
    void* cellSpace;
    void* primitiveAuxiliarySpace;
    InlineAllocationData inlineAllocations[numberOfInlineAllocations];
    TypedArrayAllocation typedArrayAllocations[numberOfTypedArrayAllocations];
    uint64_t newCopyOnWriteArrayHeaders[2];
    // Of an array of values that lies in a frame. Zero unless the Structure of such arrays is pristine in every aspect.
    uint64_t frameArrayHeader;
    uint64_t compactTableHeader;
    // CompactTable::growthPlans(). Null until an operation has added to a Map or Set.
    const uint64_t* compactTableGrowthPlans;
    ClonedStructure clonedStructures[numberOfClonedStructures];
};

// Most objects that have extra properties have one or two.
static constexpr unsigned extraPropertiesInlineCapacity = 2;

struct RealmWords {
    JSObject* objectPrototype;
    JSCell* functionPrototypeCall;
    JSCell* boundFunctionExecutable;
    Structure* emptyExtraPropertiesStructure;
    // What a call without surplus arguments hands to a function that takes the list of them. Nothing writes to it.
    JSCell* emptyArgumentList;
    const ClassInfo* boundFunctionClassInfo;
    const ClassInfo* const* instanceClassInfos;
    uint32_t boundFunctionStructureID;
    uint32_t hostFunctionStructureID;
    EncodedJSValue linkTimeConstants[JSC::numberOfLinkTimeConstants];
    const void* smallIntStrings;
    const void* intStrings;
    const uint64_t* integerKeys;
    uint64_t integerKeyMask;
    // StringKeyCache::tryFindCaseConversion().
    const uint64_t* caseConversions;
    MicrotaskQueue* microtaskQueue;
    // Of the ordinary object { read, written } that an encoder answers with. Null until the first is made.
    Structure* readAndWrittenCountsStructure;
    // Of the ordinary object that was made last from the fields that a direct entry answered, and the number of its layout in the table of built-ins. Null until the first is made.
    Structure* builtinLayoutStructure;
    uint32_t builtinLayoutOfStructure;
};

// A function object, and what the stack pointer is once its call has returned.
struct RecordedCallee {
    const void* stackPointerOfCaller;
    JSObject* closure;
};

// Outermost first. In front of the first lies an entry that every call lies above.
struct RecordedCalleeList {
    RecordedCallee* top;
    RecordedCallee* end;
};

struct HeapWords {
    StoredCellLog* storedCellLog;
    MarkStackArray* mutatorMarkStack;
    uintptr_t* barriersExecuted;
    RecordedCalleeList* recordedCallees;
};

struct IterationWords {
    JSCell* emptyEnumerator;
};

struct NumberWords {
    const double* powersOfTen;
    uint64_t hashSecrets[2];
};

// Nothing writes to a part of the second kind once Instance::create() has returned.
#define FOR_EACH_AOT_INSTANCE_PART_BY_KIND(v, writtenOnce) \
    v(propertyCaches, PropertyCacheWords) \
    writtenOnce(image, ImageWords) \
    v(allocation, AllocationWords) \
    v(heap, HeapWords) \
    v(realm, RealmWords) \
    v(numbers, NumberWords) \
    v(runtime, RuntimeWords) \
    v(modules, ModuleWords) \
    v(objects, ObjectWords) \
    writtenOnce(functionTables, FunctionTableWords) \
    v(closures, ClosureWords) \
    v(classes, ClassWords) \
    v(coroutines, CoroutineWords) \
    v(builtins, BuiltinWords) \
    writtenOnce(builtinBindings, BuiltinBindingWords) \
    v(arrays, ArrayWords) \
    v(iteration, IterationWords) \
    v(json, JSONWords) \
    v(stringBuilder, StringBuilderWords) \
    v(jsonLevels, JSONLevelWords) \
    writtenOnce(stubs, StubWords) \
    v(scratch, CodegenScratchWords) \
    v(constructWords, ConstructWordCache) \
    v(lazyStrings, LazyStringWords)
#define FOR_EACH_AOT_INSTANCE_PART(v) FOR_EACH_AOT_INSTANCE_PART_BY_KIND(v, v)

struct Instance : InstanceHeader {
#define AOT_DECLARE_INSTANCE_PART(member, Type) Type member;
#if defined(BUN_HARDENED)
#define AOT_SKIP_INSTANCE_PART(member, Type)
#define AOT_ADD_SIZE_OF_INSTANCE_PART(member, Type) + sizeof(Type)
    FOR_EACH_AOT_INSTANCE_PART_BY_KIND(AOT_DECLARE_INSTANCE_PART, AOT_SKIP_INSTANCE_PART)
    State* state;
    uint8_t unusedBelowWordsWrittenOnce[InstanceLayout::offsetOfWordsWrittenOnce - (sizeof(InstanceHeader) + sizeof(State*) FOR_EACH_AOT_INSTANCE_PART_BY_KIND(AOT_ADD_SIZE_OF_INSTANCE_PART, AOT_SKIP_INSTANCE_PART))];
    void** runtimeTable;
    SlotHandlers handlers;
    FOR_EACH_AOT_INSTANCE_PART_BY_KIND(AOT_SKIP_INSTANCE_PART, AOT_DECLARE_INSTANCE_PART)
#undef AOT_ADD_SIZE_OF_INSTANCE_PART
#undef AOT_SKIP_INSTANCE_PART
#else
    FOR_EACH_AOT_INSTANCE_PART(AOT_DECLARE_INSTANCE_PART)
    State* state;
#endif
#undef AOT_DECLARE_INSTANCE_PART

    static Instance* of(JSGlobalObject* realm) { return realm->aotInstance(); }
    static Instance* create(JSGlobalObject* realm);
    JS_EXPORT_PRIVATE static void destroy(Instance*);
    JS_EXPORT_PRIVATE static void destroyDeadInstances(VM&);
    JS_EXPORT_PRIVATE static void destroyAll(VM&);

    InstancePrefix& prefix() { return reinterpret_cast<InstancePrefix*>(this)[-1]; }
    const InstancePrefix& prefix() const { return reinterpret_cast<const InstancePrefix*>(this)[-1]; }
    const ProgramTables& tables() const { return *image.tables; }
    const RecordedOptions& options() const;

    void captureCellHeaders();
    void didCreateInstanceStructure(BuiltinClassID, Structure*);

    uint8_t* statics() { return reinterpret_cast<uint8_t*>(this) + InstanceLayout::offsetOfStatics(tables().instanceSizes); }
    const uint8_t* statics() const { return reinterpret_cast<const uint8_t*>(this) + InstanceLayout::offsetOfStatics(tables().instanceSizes); }
    uint8_t* cards() { return reinterpret_cast<uint8_t*>(this) + InstanceLayout::offsetOfCards(tables().instanceSizes); }
    template<typename T> T* tableAt(size_t offsetInInstance) { return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(this) + offsetInInstance); }
    std::span<uint32_t> methodTable()
    {
        InstanceSizes sizes = tables().instanceSizes;
        return { reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(this) + InstanceLayout::offsetOfMethodTable(sizes)), sizes.sizeOfMethodTable / sizeof(uint32_t) };
    }
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

#if defined(BUN_HARDENED)
    SlotHandlers& slotHandlers() { return handlers; }
#else
    SlotHandlers& slotHandlers() { return propertyCaches.handlers; }
#endif
    static constexpr ptrdiff_t offsetOfSlotHandlers(SlotHandlerTable);

    std::span<BoxedCallEntry, StubWords::numberOfBoxedCallEntries> boxedCalls() { return std::span<BoxedCallEntry, StubWords::numberOfBoxedCallEntries> { reinterpret_cast<BoxedCallEntry*>(reinterpret_cast<uint8_t*>(this) + offsetOfBoxedCalls()), StubWords::numberOfBoxedCallEntries }; }
    static constexpr ptrdiff_t offsetOfBoxedCalls();
    std::span<KeyedValuePlace, KeyedValuePlace::numberOfPlaces> keyedValuePlaces() { return std::span<KeyedValuePlace, KeyedValuePlace::numberOfPlaces> { reinterpret_cast<KeyedValuePlace*>(reinterpret_cast<uint8_t*>(this) + InstanceLayout::offsetOfKeyedValuePlaces), KeyedValuePlace::numberOfPlaces }; }

    template<typename Visitor>
    void visitRoots(Visitor& visitor)
    {
        JSCell* const cells[] = { realm.objectPrototype, realm.functionPrototypeCall, realm.boundFunctionExecutable, realm.emptyExtraPropertiesStructure, realm.emptyArgumentList, realm.readAndWrittenCountsStructure, realm.builtinLayoutStructure };
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

constexpr ptrdiff_t Instance::offsetOfSlotHandlers(SlotHandlerTable table)
{
#if defined(BUN_HARDENED)
    constexpr ptrdiff_t offsetOfHandlers = AOT_INSTANCE_OFFSET(handlers);
#else
    constexpr ptrdiff_t offsetOfHandlers = AOT_INSTANCE_OFFSET(propertyCaches.handlers);
#endif
    return offsetOfHandlers + static_cast<ptrdiff_t>(static_cast<unsigned>(table) * numberOfSlotKindsWithHandlers * sizeof(void*));
}

constexpr ptrdiff_t Instance::offsetOfBoxedCalls()
{
#if defined(BUN_HARDENED)
    return InstanceLayout::offsetOfBoxedCalls;
#else
    return AOT_INSTANCE_OFFSET(stubs.boxedCalls);
#endif
}

static_assert(std::is_trivially_default_constructible_v<Instance>, "Instance lives in zeroed pages and is never constructed, so default member initializers would be ignored");
static_assert(std::is_trivially_default_constructible_v<InstancePrefix>);
static_assert(std::is_trivially_default_constructible_v<InstanceHeader>);
#define AOT_CHECK_INSTANCE_PART(member, Type) static_assert(std::is_trivially_default_constructible_v<Type>);
FOR_EACH_AOT_INSTANCE_PART(AOT_CHECK_INSTANCE_PART)
#undef AOT_CHECK_INSTANCE_PART
static_assert(OBJECT_OFFSETOF(InstanceHeader, vm) == 16, "JSWebAssemblyInstance::offsetOfVM()");
#if defined(BUN_HARDENED)
static_assert(AOT_INSTANCE_OFFSET(runtimeTable) == InstanceLayout::offsetOfWordsWrittenOnce);
static_assert(UINT8_MAX * sizeof(BoxedCallEntry) <= InstanceLayout::bytesReservedForBoxedCalls);
static_assert(InstanceLayout::offsetOfBoxedCalls + static_cast<ptrdiff_t>(InstanceLayout::bytesReservedForBoxedCalls + sizeof(InstancePrefix)) <= 0);
#else
static_assert(!OBJECT_OFFSETOF(InstanceHeader, runtimeTable));
#endif
static_assert(OBJECT_OFFSETOF(InstanceHeader, globalObject) <= 128);
static_assert(AOT_INSTANCE_PREFIX_OFFSET(newArrayWithContiguousHeader) == -128);
static_assert(sizeof(Instance) <= 32 * KB, "On ARM64, the immediate offset of a 64-bit load reaches 32 KB.");
static_assert(AOT_INSTANCE_OFFSET(propertyCaches.hashedSlots) <= 504, "On ARM64, the immediate offset of a load of a pair reaches 504 bytes.");
static_assert(sizeof(Instance) <= InstanceLayout::offsetOfKeyOrderSteps);
static_assert(InstanceLayout::offsetOfKeyOrderSteps + numberOfKeyOrderStepCaches * sizeof(KeyOrderStepCache) <= InstanceLayout::offsetOfBuiltinLocationStates);
static_assert(sizeof(InstancePrefix) + InstanceLayout::bytesReservedForLoopPollPage <= InstanceLayout::bytesReservedForPrefixAndPollPage);
static_assert(KeyedValuePlace::numberOfPlaces * sizeof(KeyedValuePlace) <= InstanceLayout::bytesReservedForKeyedValuePlaces && sizeof(InstancePrefix) <= 32 * KB);
static_assert(sizeof(RememberedSlot<Slot>) == InstanceLayout::sizeOfSiteCache && sizeof(RememberedSlot<uint32_t>) == InstanceLayout::sizeOfStringCell);
static_assert(sizeof(TypedArrayAllocation) == 32);
static_assert(sizeof(UniquedStringImpl) == 24, "The image holds a header of this size per identifier, and a stub scales an identifier number by it.");

JS_EXPORT_PRIVATE JSCell* livenessToken(Instance*);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
