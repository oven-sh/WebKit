/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinWords.h"
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

class Image;
struct CaseConversions;
struct ImageFunction;
struct ImageRegExp;
struct ImageStringRecord;
struct MappedCopy;
struct ProgramSymbols;
struct RecordedOptions;
struct State;
struct VMState;

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

// A stub that makes a tail call by value compares the stack pointer with this limit. It is the limit of the stack.
struct CallByValueWords {
    ZeroedStorage<StackManager::Mirror> limit;
};

// A function that writes an object as JSON compares its frame with this limit. It is the limit of the stack, or lies above every frame while a string builder watches for an object that is written inside itself:
// StringBuilderStorage::enterWatchedLevel().
struct JSONLevelWords {
    ZeroedStorage<StackManager::Mirror> limit;
};

// For the stub that reads the closure table and makes the function object that is not there yet.
struct ClosureTableWords {
    // RememberedSlotSet::cards() of the table. Null until the static variables of the program are filled in.
    uint8_t* cards;
};

// On x86-64 an operand within 128 bytes below the instance takes a one-byte displacement, and one farther away takes four bytes. What compiled code names most lies within.
struct InstancePrefix {
    int64_t minIntegerAddedTo;
    int64_t maxIntegerAddedTo;
    int64_t distanceOfIntegersAddedToFromZero;
    int64_t spanOfIntegersAddedTo;
    int64_t minInt64;
    uint64_t newCopyOnWriteArrayWithContiguousHeader;
    uint64_t stringHeader;
    void* ropeStringAllocator;
    uint32_t collectionStructureIDs[numberOfReceivers];
    uint64_t newArrayWithInt32Header;
    uint64_t newArrayWithDoubleHeader;
    uint64_t newArrayWithContiguousHeader;
    // The next eleven words, the first five and the two tags of the header hold constants that take ten bytes as an immediate. On x86-64 compiled code names them as operands in memory: Machine::findPinnedConstant().
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
    // ImageSection::Strings, one ImageStringWord for each StringID, and ImageSection::StringConstants, the ImageStringWord of each string cell and behind them all the StringID of each.
    const uint32_t* stringWords;
    const uint32_t* stringConstants;
    // ImageSection::NamesOfSites, two bytes for the number of a site as its index of SiteAnswers has, from so far in front of it that the name of a place lies as far behind this as the index of the place lies behind the instance.
    const uint8_t* namesOfSitesByDistance;
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
    // ProgramTables::closureFunctionKinds(), by closure number.
    const FunctionKindData* closureFunctionKinds;
    // ImageSection::RegExps, one record for each RegExpRecordID, and Image::regExpCode(), from which ImageRegExp::codeFor8Bit and codeFor16Bit count.
    const ImageRegExp* regExps;
    const uint8_t* regExpCode;
    // ImageSection::FunctionStarts, by image function index: where in `code` the function begins. The stub StartModule finds the bodies of modules by it.
    const uint32_t* functionStarts;
};

struct LazyStringWords {
    uint8_t* headers;
    uint64_t capacity;
};

// Behind DefaultFinalObject: the same object with room in its cell for two more properties, for four more and so on.
enum class InlineAllocation : unsigned { Promise, Map, BoundFunction, BigInt, RegExpObject, NullPrototypeObject, RegExpMatchesArray, Set, GetterSetter, Arguments, MappedArguments, DefaultFinalObject };
static constexpr unsigned numberOfRoomierFinalObjects = 13;
static constexpr unsigned numberOfInlineAllocations = 12 + numberOfRoomierFinalObjects;
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

struct AllocationWords {
    static constexpr unsigned numberOfTypedArrayAllocations = 16;
    // A copy of properties that takes loads and stores alone is kept for the stub at a place that follows from the order and shape words of the two objects and the names left out.
    static constexpr unsigned numberOfCopiesWithoutFrame = 2048;
    static constexpr unsigned placeOfCopyWithoutFrame(uint64_t targetWords, uint64_t sourceWords, uint32_t excludedRunNumberPlusOne)
    {
        return static_cast<uint32_t>(sourceWords + (sourceWords >> 32) + 2 * (targetWords >> 32) + excludedRunNumberPlusOne) & (numberOfCopiesWithoutFrame - 1);
    }

    void* auxiliarySpace;
    void* cagedAuxiliarySpace;
    void* cellSpace;
    void* primitiveAuxiliarySpace;
    InlineAllocationData inlineAllocations[numberOfInlineAllocations];
    // What a GetterSetter holds in place of a getter and of a setter that it lacks. Null until InlineAllocation::GetterSetter has its allocator.
    JSCell* getterOfAccessorWithoutGetter;
    JSCell* setterOfAccessorWithoutSetter;
    // What the `arguments` object of strict code has as its callee. Null until InlineAllocation::Arguments has its allocator.
    JSCell* calleeOfStrictArguments;
    TypedArrayAllocation typedArrayAllocations[numberOfTypedArrayAllocations];
    uint64_t newCopyOnWriteArrayHeaders[2];
    // Of an array of values that lies in a frame. Zero unless the Structure of such arrays is pristine in every aspect.
    uint64_t frameArrayHeader;
    uint64_t compactTableHeader;
    // CompactTable::growthPlans(). Null until an operation has added to a Map or Set.
    const uint64_t* compactTableGrowthPlans;
    // Of the buffers of growing texts: AOTGrowingText.h.
    uint32_t textBufferStructureID;
};
static_assert(AllocationWords::numberOfCopiesWithoutFrame * sizeof(const MappedCopy*) <= InstanceLayout::bytesReservedForCopiesWithoutFrame);

// The LocalAllocator of a cell that holds an array and storage for an odd number of elements. Until the heap has that allocator, and in a realm
// without headers for plain arrays, the entry points to an allocator without free cells, so the stubs test neither case.
struct ShortArrayWords {
    static constexpr unsigned numberOfAllocators = 9;
    static constexpr unsigned indexOfAllocator(unsigned vectorLength) { return vectorLength / 2; }
    static constexpr unsigned wordsOfExhaustedAllocator = 24;
    bool lacksAllocatorFor(unsigned vectorLength) const { return indexOfAllocator(vectorLength) < numberOfAllocators && allocators[indexOfAllocator(vectorLength)] == exhaustedAllocator; }
    void* allocators[numberOfAllocators];
    uint64_t exhaustedAllocator[wordsOfExhaustedAllocator];
};

// The header and the tags of a table without entries. Code that looks into the table of a Map or Set reads these for a collection that has no table yet.
struct EmptyTableWords {
    uint64_t words[4];
};

// Most objects that have extra properties have one or two.
static constexpr unsigned extraPropertiesInlineCapacity = 2;

struct RealmWords {
    JSObject* objectPrototype;
    JSCell* functionPrototypeCall;
    JSCell* boundFunctionExecutable;
    Structure* emptyExtraPropertiesStructure;
    // Of extra properties that hold elements and no name, and the first word of such a cell.
    Structure* extraPropertiesWithElementsStructure;
    uint64_t headerOfExtraPropertiesWithElements;
    // What a call without surplus arguments hands to a function that takes the list of them. Nothing writes to it.
    JSCell* emptyArgumentList;
    const ClassInfo* boundFunctionClassInfo;
    const ClassInfo* const* instanceClassInfos;
    uint32_t boundFunctionStructureID;
    uint32_t hostFunctionStructureID;
    EncodedJSValue linkTimeConstants[JSC::numberOfLinkTimeConstants];
    const void* smallIntStrings;
    const void* intStrings;
    // StringKeyCache::tryFindIntegerKey(). Null until the first is entered.
    const uint64_t* integerKeys;
    uint64_t integerKeyMask;
    // StringKeyCache::KeyOfPair. Never null.
    const void* keysOfPairs;
    uint64_t keyOfPairMask;
    // Null until the first conversion is remembered.
    CaseConversions* caseConversions;
    MicrotaskQueue* microtaskQueue;
    // Of the ordinary object { read, written } that an encoder answers with. Null until the first is made.
    Structure* readAndWrittenCountsStructure;
    // Of the ordinary object that was made last from the fields that a direct entry answered, and the number of its layout in the table of built-ins. Null until the first is made.
    Structure* builtinLayoutStructure;
    // A function object of the engine that only hands on to a compiled function, and the closure of that function. Null until the run time has handed on once.
    JSCell* functionOfEngineOverCompiledFunction;
    JSCell* compiledFunctionUnderFunctionOfEngine;
    uint32_t builtinLayoutOfStructure;
};

struct HeapWords {
    StoredCellLog* storedCellLog;
    MarkStackArray* mutatorMarkStack;
    uintptr_t* barriersExecuted;
};

struct IterationWords {
    JSCell* emptyEnumerator;
    // appendIterable() and takeFromIterable() of the library, which the run time enters for a spread and a pattern: each is made at its first use and kept. Instance::visitRoots().
    JSCell* functionThatAppendsIterable;
    JSCell* functionThatTakesFromIterable;
};

struct CollationWords {
    // What the default collator orders ASCII by at the first level, by code unit below 256. Zero: no weight.
    const uint8_t* ducetLevel1Weights;
};

struct NumberWords {
    // 10 ** n for each whole n from 0 whose power is finite, as operationMathPow answers it.
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
    writtenOnce(stubs, StubWords) \
    v(constructWords, ConstructWordCache) \
    v(lazyStrings, LazyStringWords) \
    v(callsByValue, CallByValueWords) \
    v(jsonLevels, JSONLevelWords) \
    v(closureTable, ClosureTableWords) \
    v(shortArrays, ShortArrayWords) \
    v(misfits, MisfitWords) \
    writtenOnce(emptyTable, EmptyTableWords) \
    writtenOnce(collation, CollationWords)
#define FOR_EACH_AOT_INSTANCE_PART(v) FOR_EACH_AOT_INSTANCE_PART_BY_KIND(v, v)

struct Instance : InstanceHeader {
#define AOT_DECLARE_INSTANCE_PART(member, Type) Type member;
#if defined(BUN_HARDENED)
#define AOT_SKIP_INSTANCE_PART(member, Type)
#define AOT_ADD_SIZE_OF_INSTANCE_PART(member, Type) + sizeof(Type)
    FOR_EACH_AOT_INSTANCE_PART_BY_KIND(AOT_DECLARE_INSTANCE_PART, AOT_SKIP_INSTANCE_PART)
    State* state;
    uintptr_t pollIsDue;
    const Image* program;
    uint8_t unusedBelowWordsWrittenOnce[InstanceLayout::offsetOfWordsWrittenOnce - (sizeof(InstanceHeader) + sizeof(State*) + sizeof(uintptr_t) + sizeof(const Image*) FOR_EACH_AOT_INSTANCE_PART_BY_KIND(AOT_ADD_SIZE_OF_INSTANCE_PART, AOT_SKIP_INSTANCE_PART))];
    void** runtimeTable;
    SlotHandlers handlers;
    FOR_EACH_AOT_INSTANCE_PART_BY_KIND(AOT_SKIP_INSTANCE_PART, AOT_DECLARE_INSTANCE_PART)
#undef AOT_ADD_SIZE_OF_INSTANCE_PART
#undef AOT_SKIP_INSTANCE_PART
#else
    FOR_EACH_AOT_INSTANCE_PART(AOT_DECLARE_INSTANCE_PART)
    State* state;
    // Not zero while the polls of the VM are switched on. A stub reads it after an operation that allocates.
    uintptr_t pollIsDue;
    const Image* program;
#endif
#undef AOT_DECLARE_INSTANCE_PART

    static Instance* of(JSGlobalObject* realm)
    {
        return realm->aotInstance();
    }
    JS_EXPORT_PRIVATE static Instance* owning(const JSCell*);
    static Instance* create(JSGlobalObject* realm);
    JS_EXPORT_PRIVATE static void destroy(Instance*);
    JS_EXPORT_PRIVATE static void destroyDeadInstances(VM&);
    JS_EXPORT_PRIVATE static void destroyAll(VM&);

    InstancePrefix& prefix() { return reinterpret_cast<InstancePrefix*>(this)[-1]; }
    const InstancePrefix& prefix() const { return reinterpret_cast<const InstancePrefix*>(this)[-1]; }
    const ProgramTables& tables() const { return *image.tables; }
    const RecordedOptions& options() const;

    void captureCellHeaders();
    void findShortArrayAllocators();
    // Behind the allocation of an array with storage for that many elements in its cell.
    void findMissingShortArrayAllocator(unsigned vectorLength)
    {
        if (shortArrays.lacksAllocatorFor(vectorLength)) [[unlikely]]
            findShortArrayAllocators();
    }
    void didCreateInstanceStructure(BuiltinClassID, Structure*);

    uint8_t* statics() { return reinterpret_cast<uint8_t*>(this) + InstanceLayout::offsetOfStatics(tables().instanceSizes); }
    const uint8_t* statics() const { return reinterpret_cast<const uint8_t*>(this) + InstanceLayout::offsetOfStatics(tables().instanceSizes); }
    uint8_t* cards() { return reinterpret_cast<uint8_t*>(this) + InstanceLayout::offsetOfCards(tables().instanceSizes); }
    template<typename T> T* tableAt(size_t offsetInInstance) { return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(this) + offsetInInstance); }
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
    std::span<SiteAnswers> siteAnswers() { return { reinterpret_cast<SiteAnswers*>(reinterpret_cast<uint8_t*>(this) + InstanceLayout::offsetOfSiteAnswers), InstanceLayout::bytesReservedForSiteAnswers / sizeof(SiteAnswers) }; }
    std::span<uint16_t> indicesOfSiteAnswers()
    {
        InstanceSizes sizes = tables().instanceSizes;
        return { reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(this) + InstanceLayout::offsetOfIndicesOfSiteAnswers(sizes)), sizes.numberOfSiteCaches };
    }
    // Its pages are read-only but while AOTClassGuards.cpp repoints an entry.
    std::span<uint32_t> methodTable()
    {
        InstanceSizes sizes = tables().instanceSizes;
        return { reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(this) + InstanceLayout::offsetOfMethodTable(sizes)), sizes.sizeOfMethodTable / sizeof(uint32_t) };
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
    std::span<const MappedCopy*, AllocationWords::numberOfCopiesWithoutFrame> copiesWithoutFrame() { return std::span<const MappedCopy*, AllocationWords::numberOfCopiesWithoutFrame> { reinterpret_cast<const MappedCopy**>(reinterpret_cast<uint8_t*>(this) + InstanceLayout::offsetOfCopiesWithoutFrame), AllocationWords::numberOfCopiesWithoutFrame }; }
    std::span<KeyedValuePlace, KeyedValuePlace::numberOfPlaces> keyedValuePlaces() { return std::span<KeyedValuePlace, KeyedValuePlace::numberOfPlaces> { reinterpret_cast<KeyedValuePlace*>(reinterpret_cast<uint8_t*>(this) + InstanceLayout::offsetOfKeyedValuePlaces), KeyedValuePlace::numberOfPlaces }; }
    // Behind the places and what is left of the places for stores.
    static constexpr ptrdiff_t offsetOfAcceptedKindsOfValues = InstanceLayout::offsetOfKeyedValuePlaces + static_cast<ptrdiff_t>(KeyedValuePlace::numberOfPlaces * sizeof(KeyedValuePlace) + KeyedValuePlace::numberOfPlacesPerAccess * sizeof(RestOfPlaceForStores));
    std::span<AcceptedKindOfValue, AcceptedKindOfValue::numberOfEntries> acceptedKindsOfValues() { return std::span<AcceptedKindOfValue, AcceptedKindOfValue::numberOfEntries> { reinterpret_cast<AcceptedKindOfValue*>(reinterpret_cast<uint8_t*>(this) + offsetOfAcceptedKindsOfValues), AcceptedKindOfValue::numberOfEntries }; }

    template<typename Visitor>
    void visitRoots(Visitor& visitor)
    {
        JSCell* const cells[] = { realm.objectPrototype, realm.functionPrototypeCall, realm.boundFunctionExecutable, realm.emptyExtraPropertiesStructure, realm.extraPropertiesWithElementsStructure, realm.emptyArgumentList, realm.readAndWrittenCountsStructure, realm.builtinLayoutStructure, realm.functionOfEngineOverCompiledFunction, realm.compiledFunctionUnderFunctionOfEngine };
        for (JSCell* function : { iteration.functionThatAppendsIterable, iteration.functionThatTakesFromIterable }) {
            if (function)
                visitor.appendUnbarriered(function);
        }
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
#if AOT_BOXED_CALLS_LIE_OUTSIDE_INSTANCE
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
#else
static_assert(!OBJECT_OFFSETOF(InstanceHeader, runtimeTable));
#endif
#if AOT_BOXED_CALLS_LIE_OUTSIDE_INSTANCE
static_assert(UINT8_MAX * sizeof(BoxedCallEntry) <= InstanceLayout::bytesReservedForBoxedCalls);
static_assert(InstanceLayout::offsetOfBoxedCalls < 0
    ? InstanceLayout::offsetOfBoxedCalls + static_cast<ptrdiff_t>(InstanceLayout::bytesReservedForBoxedCalls + sizeof(InstancePrefix)) <= 0
    : static_cast<ptrdiff_t>(sizeof(Instance)) <= InstanceLayout::offsetOfBoxedCalls);
#endif
static_assert(OBJECT_OFFSETOF(InstanceHeader, globalObject) <= 128);
static_assert(AOT_INSTANCE_PREFIX_OFFSET(newArrayWithContiguousHeader) >= -128, "On x86-64, a one-byte displacement reaches 128 bytes below the instance.");
static_assert(sizeof(Instance) <= 32 * KB, "On ARM64, the immediate offset of a 64-bit load reaches 32 KB.");
static_assert(AOT_INSTANCE_OFFSET(propertyCaches.hashedSlots) <= 504, "On ARM64, the immediate offset of a load of a pair reaches 504 bytes.");
static_assert(sizeof(Instance) <= InstanceLayout::offsetOfTablesAboveInstance);
static_assert(InstanceLayout::offsetOfLoopPollPage() < 0 || sizeof(Instance) <= static_cast<size_t>(InstanceLayout::offsetOfLoopPollPage()), "On ARM64 the loop poll page lies behind the instance, where one load reaches it.");
static_assert(sizeof(InstancePrefix) + InstanceLayout::bytesReservedForLoopPollPage <= InstanceLayout::bytesReservedForPrefixAndPollPage);
static_assert(sizeof(InstancePrefix) <= 32 * KB);
static_assert(sizeof(RememberedSlot<uint32_t>) == InstanceLayout::sizeOfStringCell);
static_assert(Instance::offsetOfAcceptedKindsOfValues + static_cast<ptrdiff_t>(AcceptedKindOfValue::numberOfEntries * sizeof(AcceptedKindOfValue)) <= InstanceLayout::offsetOfKeyedValuePlaces + static_cast<ptrdiff_t>(InstanceLayout::bytesReservedForKeyedValuePlaces));
static_assert(sizeof(GlobalBinding) == InstanceLayout::sizeOfGlobalBinding);
static_assert(sizeof(TypedArrayAllocation) == 32);
static_assert(sizeof(UniquedStringImpl) == 24, "The image holds a header of this size per identifier, and a stub scales an identifier number by it.");

JS_EXPORT_PRIVATE JSCell* livenessToken(Instance*);
// A host retires a realm by discarding its microtasks. A retired realm runs no more code, so the next realm can have an instance before the collector frees the old one.
size_t numberOfRunningInstances(const VMState&);
void setPollsEnabled(Instance&, bool);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
