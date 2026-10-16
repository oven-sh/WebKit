/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunction.h"
#include "AOTFunctionWithCaptures.h"
#include "AOTOperationsBuiltins.h"
#include "AOTOperationsObjects.h"
#include "AOTProgramData.h"
#include "AOTSlotWatchpoint.h"
#include "AOTStubs.h"
#include "AOTTypedOperations.h"
#include "BytecodeIndex.h"
#include "CallLinkInfo.h"
#include "ExecutableAllocator.h"
#include "ImmutableIntrinsics.h"
#include "IndexingType.h"
#include "JITCode.h"
#include "LineColumn.h"
#include "LinkTimeConstant.h"
#include "Opcode.h"
#include "RegisterAtOffsetList.h"
#include "StackManager.h"
#include "Structure.h"
#include "StructureID.h"
#include <wtf/BitVector.h>
#include <wtf/BitSet.h>
#include <wtf/HashMap.h>
#include <wtf/SegmentedVector.h>
#include <wtf/TZoneMalloc.h>

namespace JSC {

class AbstractModuleRecord;
class CodeBlock;
class JSGlobalObject;
class CallFrame;
class FunctionExecutable;
class MicrotaskQueue;
class ScriptExecutable;
class UnlinkedCodeBlock;
class VM;

namespace AOT {

namespace Typed {
struct State;
}
struct JSONWideSegment;

#define FOR_EACH_AOT_OPERATION(v) \
    v(operationAOTValueAdd) \
    v(operationAOTValueSub) \
    v(operationAOTValueMul) \
    v(operationAOTValueDiv) \
    v(operationAOTValueMod) \
    v(operationAOTValuePow) \
    v(operationAOTValueBitAnd) \
    v(operationAOTValueBitOr) \
    v(operationAOTValueBitXor) \
    v(operationAOTValueLShift) \
    v(operationAOTValueRShift) \
    v(operationAOTValueURShift) \
    v(operationAOTToBoolean) \
    v(operationAOTCompareLess) \
    v(operationAOTCompareLessEq) \
    v(operationAOTCompareGreater) \
    v(operationAOTCompareGreaterEq) \
    v(operationAOTCompareEq) \
    v(operationAOTCompareEqToText) \
    v(operationAOTCompareStrictEq) \
    v(operationAOTGetById) \
    v(operationAOTPutById) \
    v(operationAOTGetByNameAndFillTable) \
    v(operationAOTPutByNameAndFillTable) \
    v(operationAOTGetByVal) \
    v(operationAOTGetByValAndCache) \
    v(operationAOTPutByValAndCache) \
    v(operationAOTPutByValAndCacheSloppy) \
    v(operationAOTInByValAndCache) \
    v(operationAOTDeleteByValAndCache) \
    v(operationAOTDeleteByValAndCacheSloppy) \
    v(operationAOTPutByVal) \
    v(operationAOTWriteBarrier) \
    v(operationAOTGetPlainly) \
    v(operationAOTHasPlainly) \
    v(operationAOTGetPlainlyAtSite) \
    v(operationAOTGetPlainlyAndFill) \
    v(operationAOTGetByKeyPlainly) \
    v(operationAOTPutByKeyPlainly) \
    v(operationAOTHasByKeyPlainly) \
    FOR_EACH_AOT_OBJECT_OPERATION(v) \
    FOR_EACH_AOT_BUILTIN_OPERATION(v) \
    FOR_EACH_AOT_TYPED_OPERATION(v) \
    FOR_EACH_AOT_TYPED_BUILTIN_OPERATION(v) \
    FOR_EACH_AOT_OPERATION_OF_THE_OTHER_TIERS(v) \

#define FOR_EACH_AOT_OPERATION_OF_THE_OTHER_TIERS(v) \
    v(operationStringIndexOfWithIndex) \
    v(operationStringLocaleCompare) \
    v(operationStringReplaceStringString) \
    v(operationStringProtoFuncReplaceRegExpString) \
    v(operationObjectAssignUntyped) \
    v(operationArrayIndexOfValueInt32OrContiguous) \
    v(operationArrayIncludesValueInt32OrContiguous) \

#define FOR_EACH_AOT_THUNK(v) \
    v(HandleException) \
    v(VirtualCall) \
    v(VirtualConstruct) \
    v(VirtualTailCall) \

#define FOR_EACH_AOT_POINTER(v) \
    v(CallLinkInfoForCall) \
    v(CallLinkInfoForConstruct) \
    v(CallLinkInfoForTailCall) \
    v(StructureIDBase) \
    v(LookupExceptionHandler) \
    v(ThrowStackOverflowError) \
    v(NativeCallTrampoline) \
    v(InternalFunctionCallTrampoline) \
    v(InternalFunctionConstructTrampoline) \
    v(MegamorphicCache) \
    v(UndefinedHolder) \
    v(HostMathSqrt) \
    v(HostMathAbs) \
    v(HostMathFloor) \
    v(HostMathCeil) \
    v(HostMathTrunc) \
    v(HostMathFround) \
    v(HostMathMin) \
    v(HostMathMax) \
    v(HostMathIMul) \
    v(HostStringCharCodeAt) \
    v(HostArrayPush) \
    v(HostStringCodePointAt) \
    v(HostStringCharAt) \
    v(HostStringSlice) \
    v(HostArrayPop) \
    v(HostArrayIsArray) \
    v(HostMapGet) \
    v(HostMapHas) \
    v(HostMapSet) \
    v(HostSetHas) \
    v(HostSetAdd) \
    v(RawGetById) \
    v(RawGetByVal) \
    v(RawPutById) \
    v(RawPutByVal) \
    v(RawNewObjectLiteral) \
    v(RawCompareStrictEq) \
    v(RawCompareEq) \
    v(RawTypedPutExtra) \
    v(RawTypedGetExtra) \
    v(RawTypedConstruct) \
    v(HostFunctionThatIsNoConstructor) \
    v(NewArraySlowPath) \
    v(ValueAddSlowPath) \
    v(ClassInfoOfAsyncGenerators) \
    v(ReturnAddressOfResumeFunctions) \

enum class Entry : uint16_t {
#define AOT_DEFINE_ENTRY(name) name,
    FOR_EACH_AOT_OPERATION(AOT_DEFINE_ENTRY)
    FOR_EACH_AOT_THUNK(AOT_DEFINE_ENTRY)
    FOR_EACH_AOT_POINTER(AOT_DEFINE_ENTRY)
#undef AOT_DEFINE_ENTRY
    NumberOfEntries
};

static constexpr unsigned numberOfEntries = static_cast<unsigned>(Entry::NumberOfEntries);
ASCIILiteral nameOf(Entry);

bool takesInstance(Entry);

struct VirtualCallInfo {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(VirtualCallInfo);

    static constexpr ptrdiff_t offsetOfFindTarget() { return OBJECT_OFFSETOF(VirtualCallInfo, findTarget); }
    static constexpr ptrdiff_t offsetOfLookupExceptionHandler() { return OBJECT_OFFSETOF(VirtualCallInfo, lookupExceptionHandler); }

    DataOnlyCallLinkInfo callLinkInfo;
    void* findTarget;
    void* lookupExceptionHandler;
};

struct Slot;

class RuntimeTable {
    WTF_MAKE_TZONE_ALLOCATED(RuntimeTable);
    WTF_MAKE_NONCOPYABLE(RuntimeTable);
public:
    explicit RuntimeTable(VM&);
    ~RuntimeTable();

    void** entries() { return m_entries; }
    static constexpr ptrdiff_t offsetOfEntries() { return OBJECT_OFFSETOF(RuntimeTable, m_entries); }
    void countOperation(const char* name, const char* detail = nullptr, uint64_t count = 1) { m_operationCounts.add(std::pair { name, detail }, 0).iterator->value += count; }
    void countOperationBySlotState(const char* name, const Slot*);
    void countOperationAtSite(const char* name, uint32_t function, unsigned bytecodeOffset, unsigned line, unsigned column);
    void countAllocatedBytes(const char* kind, const Subspace*, size_t cellSize, const ClassInfo* classOfOwner, size_t bytes);
    bool shouldWriteOperationCountsAfterAllocating(size_t bytes)
    {
        m_bytesAllocatedSinceLastWrite += bytes;
        if (m_bytesAllocatedSinceLastWrite < MB)
            return false;
        m_bytesAllocatedSinceLastWrite = 0;
        return true;
    }
    void clearLiveBytes();
    void setLiveBytes(const Subspace&, size_t cellSize, size_t bytes);
    void countChangeOfMegamorphicCacheEpoch(uint16_t epoch)
    {
        if (std::exchange(m_lastMegamorphicCacheEpoch, epoch) != epoch)
            countOperation("MegamorphicCache", "epoch-changed");
    }
    void countFillOfMegamorphicCache(const char* kind, uint16_t epoch, uint64_t structureIDs, const void* uid, const char* outcome)
    {
        if (outcome) {
            countOperation(kind, outcome);
            return;
        }
        if (std::exchange(m_epochOfFilledPairs, epoch) != epoch)
            m_filledPairs.clear();
        countOperation(kind, m_filledPairs.add({ kind, uid, structureIDs }).isNewEntry ? "new-pair" : "pair-again");
    }
    void forgetFilledPairsOfDeadStructures(VM&);
    void noteGuest(const char* kind, StringView group, String&& sample);
    uint64_t operationCount(StringView nameAndDetail) const;
    void dumpOperationCounts(PrintStream&) const;
    void writeOperationCounts() const;

private:
    void* m_entries[numberOfEntries];
    Vector<std::unique_ptr<VirtualCallInfo>> m_callLinkInfos;
    UncheckedKeyHashMap<std::pair<const char*, const char*>, uint64_t> m_operationCounts;
    UncheckedKeyHashMap<std::tuple<const char*, const void*, size_t>, UTF8CString> m_detailsOfAllocatedBytes;
    UncheckedKeyHashMap<UTF8CString, std::pair<uint64_t, uint64_t>> m_allocatedBytesByStack;
    size_t m_bytesAllocatedSinceLastWrite { 0 };
    UncheckedKeyHashMap<uint64_t, UTF8CString> m_detailsOfSites;
    uint16_t m_lastMegamorphicCacheEpoch { 0 };
    uint16_t m_epochOfFilledPairs { 0 };
    UncheckedKeyHashSet<std::tuple<const char*, const void*, size_t>> m_filledPairs;
    UncheckedKeyHashMap<String, std::pair<uint64_t, String>> m_guests;
};

RuntimeTable& runtimeTable(VM&);
void countAllocatedBytes(VM&, const char* kind, const Subspace*, size_t cellSize, const JSCell* owner, size_t bytes);
void setLoopPollPagesProtected(VM&, bool);
JS_EXPORT_PRIVATE uint64_t operationCount(VM&, StringView nameAndDetail);
JS_EXPORT_PRIVATE void writeOperationCounts(VM&);

struct Data;

class BiasedDataPointer {
public:
    static constexpr ptrdiff_t bias = 48 + 128;
    BiasedDataPointer() = default;
    BiasedDataPointer& operator=(Data* data)
    {
        m_bits = data ? std::bit_cast<uintptr_t>(data) + bias : 0;
        return *this;
    }
    Data* get() const { return m_bits ? std::bit_cast<Data*>(m_bits - bias) : nullptr; }
    operator Data*() const { return get(); }
    Data* operator->() const { return get(); }

private:
    uintptr_t m_bits;
};
static_assert(sizeof(BiasedDataPointer) == sizeof(Data*) && std::is_trivially_default_constructible_v<BiasedDataPointer>);

static constexpr unsigned SoundTypeUndefined   = 1u << 0;
static constexpr unsigned SoundTypeNull        = 1u << 1;
static constexpr unsigned SoundTypeBoolean     = 1u << 2;
static constexpr unsigned SoundTypeNumber      = 1u << 3;
static constexpr unsigned SoundTypeString      = 1u << 4;
static constexpr unsigned SoundTypeFunction    = 1u << 7;
static constexpr unsigned SoundTypeArray       = 1u << 8;
static constexpr unsigned SoundTypeOtherObject = 1u << 9;
static constexpr unsigned SoundTypeAnyObject   = SoundTypeFunction | SoundTypeArray | SoundTypeOtherObject;

struct Slot {
    static constexpr unsigned offsetBits = 24;
    static constexpr uint32_t offsetMask = (1u << offsetBits) - 1;
    static constexpr unsigned directLocationBits = 8;
    static constexpr uint32_t directLocationMask = (1u << directLocationBits) - 1;
    static constexpr unsigned nameIDShift = directLocationBits;
    static_assert(offsetBits - nameIDShift == 16);
    static_assert(JSFinalObject::maxInlineCapacity + JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue) <= directLocationMask);
    static constexpr unsigned attemptsShift = 24;
    static constexpr uint32_t maxAttempts = 15;
    static constexpr uint32_t maxAttemptsForInheritedProperty = 4;
    static constexpr uint32_t missesServedBeforeLearning = 12;
    static constexpr uint32_t missesServedBeforeSeveralSlots = 4;
    static_assert(missesServedBeforeLearning + 2 < maxAttempts);
    static constexpr uint32_t attemptsMask = maxAttempts << attemptsShift;
    static constexpr uint32_t isIndirect = 1u << 28;
    static constexpr uint32_t isGetter = 1u << 29;
    static constexpr uint32_t hasFieldType = 1u << 29;
    static constexpr uint32_t pointerIsNotCell = 1u << 30;
    static constexpr uint32_t pointerIsCell = 1u << 31;
    static constexpr uint32_t resolvesByDepth = 1u << 31;

    bool hasPointer() const { return offset & (pointerIsCell | pointerIsNotCell); }
    static constexpr uint32_t flagsMask = isIndirect | isGetter | pointerIsNotCell | pointerIsCell;
    static constexpr uint32_t polymorphicFlags = isIndirect | pointerIsNotCell;
    // Of a read: the property is a field of an object of a compiled layout, which the structure stands for. `pointer` is no pointer: the offset of the field in the object, in bytes, and from fieldReadShift on how it is read.
    static constexpr uint32_t compiledFieldFlags = isIndirect | isGetter | pointerIsNotCell;
    enum class FieldRead : uint8_t {
        // Eight bytes, a JSValue or the address of a cell. 0: absent.
        Boxed,
        // Typed::Store::Cell32, OptionalCell32.
        Cell32,
        Int32OrElsewhere,
        Int32,
        // Also an optional one: what stands for no number is a NaN.
        Double,
        Boolean,
        OptionalBoolean,
        // Also an optional one: what stands for no number is beyond int32.
        Int64,
        // With the bit of fieldIsInExtension and the one above it: an integer in fewer than eight bytes. It is in the object and never absent. readCompiledField() asks the upper bit before it takes the lower one for "in the extension".
        Uint8 = 0x30,
        Int8,
        Uint16,
        Int16,
        Uint32,
        // The name is no field of the layout: an extra property, if anything. No stub serves that: it goes to the operation by the shortest way.
        NoField = 0x37,
        KnownCustomGetter = 0x36,
    };
    static constexpr unsigned fieldReadShift = 32;
    static constexpr uint64_t knownCustomGetter = static_cast<uint64_t>(FieldRead::NoField) << fieldReadShift | 1;
    static constexpr uint64_t isLookedUp = static_cast<uint64_t>(FieldRead::NoField) << fieldReadShift | 2;
    static constexpr uint64_t isAbsentWhileCountStands = static_cast<uint64_t>(FieldRead::NoField) << fieldReadShift | 4;
    static constexpr unsigned countShift = 3;
    static constexpr uint32_t countIsValidated = 1u << 28;
    static_assert((static_cast<unsigned>(FieldRead::NoField) & 0x36) == 0x36);
    static constexpr unsigned customGetterPlaceShift = 40;
    static constexpr uint32_t customGetterSerialStep = 8;
    static constexpr unsigned extraPropertyShift = 40;
    static constexpr uint32_t extraPropertyIsOutOfLine = 1u << 19;
    static constexpr uint32_t extraPropertyIsValue = 1u << 20;
    static constexpr uint32_t extraPropertyIsAbsent = 1u << 21;
    static constexpr uint32_t extraPropertyOffsetMask = extraPropertyIsOutOfLine - 1;
    static constexpr uint64_t extraPropertyAt(uint32_t structureIDOfExtraProperties, uint32_t offsetAndKind) { return static_cast<uint64_t>(offsetAndKind) << extraPropertyShift | static_cast<uint64_t>(FieldRead::NoField) << fieldReadShift | structureIDOfExtraProperties; }
    static constexpr uint64_t knownCustomGetterAt(uint32_t placeInTable, uint32_t serial) { return static_cast<uint64_t>(placeInTable) << customGetterPlaceShift | static_cast<uint64_t>(FieldRead::KnownCustomGetter) << fieldReadShift | serial; }
    // Of a store, with pointerIsNotCell: the property is a field of an object of a compiled layout. The low 16 bits of `pointer` are the offset of the field in the object, in bytes. See typeShift for the RuntimeType that tells which objects may be stored without asking. The upper half, which is never zero:
    struct FieldWrite {
        enum Kept : uint32_t { Boxed, Cell32, Int32OrElsewhere, Double, Boolean, Int64, Int32 };
        static constexpr uint32_t keptMask = 7;
        // The way it is kept has something that stands for "absent": OptionalDouble, OptionalBoolean, OptionalInt64. (Boxed, Cell32 and Int32OrElsewhere always have.)
        static constexpr uint32_t isOptional = 1u << 3;
        static constexpr uint32_t storesLiteralStringWords = isOptional;
        // What may be stored without asking.
        static constexpr uint32_t acceptsInt32 = 1u << 4;
        // Any number.
        static constexpr uint32_t acceptsNumber = 1u << 5;
        static constexpr uint32_t acceptsString = 1u << 6;
        static constexpr uint32_t acceptsSymbol = 1u << 7;
        static constexpr uint32_t acceptsBigInt = 1u << 8;
        static constexpr uint32_t acceptsObject = 1u << 9;
        static constexpr uint32_t acceptsUndefined = 1u << 10;
        static constexpr uint32_t acceptsNull = 1u << 11;
        static constexpr uint32_t acceptsBoolean = 1u << 12;
        // The name is no field of the layout: an extra property, if anything. No stub serves that: it goes to the operation by the shortest way.
        static constexpr uint32_t isNoField = 1u << 13;
        static constexpr uint32_t isInGlobalProxyTarget = 1u << 14;
        static constexpr unsigned offsetBits = 16;
        static constexpr unsigned typeShift = 15;
        static constexpr unsigned typeBitsInUpperHalf = 15;
        static constexpr uint64_t withType(uint32_t offsetInData) { return static_cast<uint64_t>(offsetInData >> 2 & 0xffff) << offsetBits | static_cast<uint64_t>(offsetInData >> 18) << (32 + typeShift); }
        static constexpr uint32_t isFieldWrite = 1u << 31;
    };
    // Of a store, with pointerIsNotCell: it makes an object of a compiled layout, which the structure stands for, one of a specialization that has a field by the name. The rest of `pointer` is the address of a Typed::StoreTransition.
    static constexpr uint64_t isStoreTransition = 1ull << 62;
    // Nothing on the prototype chain supplies the name, which is watched: where the field is absent, the read yields `undefined`.
    static constexpr uint64_t absentFieldIsUndefined = 1ull << (fieldReadShift + 3);
    // The offset is one in the extension of the object, where the slot holds a JSValue. An extension that ends before it: the field is absent.
    static constexpr uint64_t fieldIsInExtension = 1ull << (fieldReadShift + 4);
    bool isPolymorphic() const { return !structureID && (offset & flagsMask) == polymorphicFlags; }
    bool isNameOnly() const { return !structureID && !(offset & (flagsMask | attemptsMask)) && offset >> nameIDShift; }
    bool readsExtraProperty() const
    {
        uint64_t word = std::bit_cast<uint64_t>(pointer);
        return (offset & flagsMask) == compiledFieldFlags && ((word >> fieldReadShift) & 0xff) == static_cast<uint64_t>(FieldRead::NoField) && word >> extraPropertyShift && !(word >> 62);
    }
    StructureID structureIDOfExtraProperties() const { return std::bit_cast<StructureID>(static_cast<uint32_t>(std::bit_cast<uint64_t>(pointer))); }

    void clear()
    {
        structureID = StructureID();
        offset &= attemptsMask;
        pointer = nullptr;
    }

    void clearAndForgetAttempts()
    {
        clear();
        offset = 0;
    }

    StructureID structureID;
    uint32_t offset;
    union {
        void* pointer;
        UniquedStringImpl* name;
        struct {
            StructureID newStructureID;
            uint32_t fieldType;
        };
    };
};
static_assert(sizeof(Slot) == 16);

struct Data;

struct PolymorphicSlots {
    // A structure has two places among them: indexFor() and its neighbour. Structures lie 176 bytes apart: eleven times 16: neighbours have places of their own.
    static constexpr unsigned numberOfSlots = 8;
    static constexpr unsigned structureShift = 4;
    static_assert(hasOneBitSet(numberOfSlots));
    static constexpr unsigned indexFor(StructureID id) { return id.bits() >> structureShift & (numberOfSlots - 1); }
    Slot* slotOf(StructureID id)
    {
        unsigned index = indexFor(id);
        return slots[index].structureID == id ? &slots[index] : slots[index ^ 1].structureID == id ? &slots[index ^ 1] : nullptr;
    }
    static constexpr uint32_t maxBulkLearnAttempts = 12;
    static constexpr uint32_t missesPerLearningAttempt = 1024;
    // While remainingBulkLearnAttempts is not 0. The misses between are for the table that all sites share.
    static constexpr uint32_t missesPerEarlyLesson = 4;
    static_assert(hasOneBitSet(missesPerLearningAttempt) && hasOneBitSet(missesPerEarlyLesson));

    static constexpr ptrdiff_t offsetOfName() { return OBJECT_OFFSETOF(PolymorphicSlots, name); }
    static constexpr ptrdiff_t offsetOfMissesUntilLesson() { return OBJECT_OFFSETOF(PolymorphicSlots, missesUntilLesson); }
    static constexpr ptrdiff_t offsetOfRemainingBulkLearnAttempts() { return OBJECT_OFFSETOF(PolymorphicSlots, remainingBulkLearnAttempts); }
    static constexpr ptrdiff_t offsetOfSlots() { return OBJECT_OFFSETOF(PolymorphicSlots, slots); }
    static constexpr ptrdiff_t offsetOfByName() { return OBJECT_OFFSETOF(PolymorphicSlots, byName); }
    static constexpr ptrdiff_t offsetOfIdentifierIndex() { return OBJECT_OFFSETOF(PolymorphicSlots, identifierIndex); }
    static constexpr ptrdiff_t offsetOfOwesItsLessons() { return OBJECT_OFFSETOF(PolymorphicSlots, owesItsLessons); }

    static constexpr unsigned numberOfInlineSlotsByName = 4;
    static constexpr unsigned inlineNameSlotsShift = 16;
    static constexpr unsigned nameTableFillValueShift = 48;
    static constexpr uint64_t initialByName = Structure::firstReservedPropertyNameID;
    static constexpr uint8_t maxNameTableFillFailures = 16;
    bool fillsNameTables() const { return !(byName >> nameTableFillValueShift); }
    bool isAtLesson() const { return !owesItsLessons || missesUntilLesson == (remainingBulkLearnAttempts ? missesPerEarlyLesson : missesPerLearningAttempt); }
    void didArriveAtNoLesson()
    {
        remainingBulkLearnAttempts = std::min(remainingBulkLearnAttempts + 1, maxBulkLearnAttempts);
        missesUntilLesson = std::min(missesUntilLesson, missesPerEarlyLesson - 1);
        hasEarnedLesson = true;
    }
    void didFailToFillNameTable()
    {
        if (!--remainingNameTableFillFailures)
            byName |= static_cast<uint64_t>(Structure::firstReservedPropertyNameID) << nameTableFillValueShift;
    }
    bool addInlineNameSlot(uint16_t nameID, unsigned inlineSlot)
    {
        ASSERT(inlineSlot < Structure::numberOfSlotsWithPropertyNameIDs && nameID && nameID < Structure::firstReservedPropertyNameID);
        if (!numberOfUsedInlineNameSlots) {
            byName = (byName >> nameTableFillValueShift << nameTableFillValueShift) | static_cast<uint64_t>(inlineSlot * 0x01010101u) << inlineNameSlotsShift | nameID;
            numberOfUsedInlineNameSlots = 1;
            return true;
        }
        RELEASE_ASSERT(static_cast<uint16_t>(byName) == nameID);
        for (unsigned i = 0; i < numberOfUsedInlineNameSlots; ++i) {
            if ((byName >> (inlineNameSlotsShift + i * 8) & 0xff) == inlineSlot)
                return true;
        }
        if (numberOfUsedInlineNameSlots == numberOfInlineSlotsByName)
            return false;
        unsigned shift = inlineNameSlotsShift + numberOfUsedInlineNameSlots++ * 8;
        byName = (byName & ~(0xffull << shift)) | static_cast<uint64_t>(inlineSlot) << shift;
        return true;
    }

    UniquedStringImpl* name;
    uint32_t missesUntilLesson;
    uint32_t remainingBulkLearnAttempts;
    Data* owner;
    uint8_t numberOfUsedInlineNameSlots;
    uint8_t remainingNameTableFillFailures;
    bool owesItsLessons;
    bool hasEarnedLesson;
    static constexpr uint32_t hasNotReadCompiledField = 1u << 31;
    uint32_t identifierIndex;
    uint64_t byName;
    Slot slots[numberOfSlots];
};

struct Data;
struct ImageEnvironment;
struct ImageFunction;
struct Site;

struct FunctionInfo {
    static constexpr uint32_t maxEncodedSlots = (1u << 13) - 1;

    static constexpr ptrdiff_t offsetOfSites() { return OBJECT_OFFSETOF(FunctionInfo, sites); }
    static constexpr ptrdiff_t offsetOfBitsAndStateNumber() { return OBJECT_OFFSETOF(FunctionInfo, sites) + sizeof(uint32_t); }
    static constexpr unsigned stateNumberShift = 4;

    static uint32_t encodeIndexAndFlags(uint32_t indexPlusOne, CodeSpecializationKind kind, CodeType codeType)
    {
        static_assert(FunctionCode < 4 && ModuleCode < 4 && GlobalCode < 4);
        RELEASE_ASSERT(!(indexPlusOne >> 29));
        return indexPlusOne << 3 | static_cast<uint32_t>(codeType) << 1 | (kind == CodeSpecializationKind::CodeForConstruct ? 1 : 0);
    }
    static uint32_t encodeTypedFunction(uint32_t typedFunction)
    {
        RELEASE_ASSERT(!(typedFunction >> 29));
        return typedFunction << 3 | static_cast<uint32_t>(FunctionCode) << 1;
    }
    inline uint32_t indexAndFlags() const;
    // Its number in Typed::Data.
    uint32_t typedFunction() const
    {
        ASSERT(isTyped);
        return indexAndFlags() >> 3;
    }
    CodeType codeType() const { return static_cast<CodeType>(indexAndFlags() >> 1 & 3); }
    bool isTopLevelCode() const { return codeType() != FunctionCode; }
    CodeSpecializationKind kind() const { return indexAndFlags() & 1 ? CodeSpecializationKind::CodeForConstruct : CodeSpecializationKind::CodeForCall; }
    bool hasCodeInImage() const { return sites; }
    inline bool hasSiteConstants() const;
    inline bool startsCold() const;
    inline uint32_t numberOfSlotsForMisses() const;
    JS_EXPORT_PRIVATE const Site* sitesInImage() const;
    inline const ImageFunction* function() const;

    uint32_t sites;
    uint32_t hasInlineFrames : 1;
    // Compiled from types: no executable and no bytecode. A "bytecode index" is a call-site number, and with inline frames all positions
    // are still in this function's own ExpressionInfo.
    uint32_t isTyped : 1;
    // The entry function of a typed program, which stands in for the code of an empty module.
    uint32_t isHiddenInStackTraces : 1;
    // Compiled from types, and FunctionMetadata::Handlers is not there: whoever unwinds need not know where in the function a frame is.
    uint32_t hasNoHandlers : 1;
    uint32_t stateNumber : 32 - stateNumberShift;
};
static_assert(sizeof(FunctionInfo) == 8);

// See Instance::receiverStructureIDs.
enum class Receiver : uint8_t { None, String, Array, Map, Set, WeakMap, WeakSet, RegExp, Date, Number };
enum class TableMethod : uint8_t { MapGet, MapHas, MapSet, SetHas, SetAdd };

inline constexpr auto powersOfTen = [] {
    std::array<double, 309> result { };
    for (int32_t k = 0; k < static_cast<int32_t>(result.size()); ++k) {
        double power = 1;
        double square = 10;
        for (int32_t bits = k; bits; bits >>= 1) {
            if (bits & 1)
                power *= square;
            if (bits >> 1)
                square *= square;
        }
        result[k] = power;
    }
    return result;
}();

struct Instance {
    JS_EXPORT_PRIVATE static Instance& ensure(JSModuleLoader*);
    JS_EXPORT_PRIVATE static Instance& ensure(JSGlobalObject*);
    static Instance* of(JSFunction*);
    JS_EXPORT_PRIVATE ScriptExecutable* topLevelExecutableOf(uint32_t moduleID);
    JS_EXPORT_PRIVATE void setTopLevelExecutableOf(uint32_t moduleID, ScriptExecutable*);
    JS_EXPORT_PRIVATE uint64_t prepareModuleCode(ModuleProgramExecutable*, JSScope*);
    JS_EXPORT_PRIVATE JSValue didRunModuleCode(AbstractModuleRecord*, ModuleProgramExecutable*, JSValue yielded);
    JS_EXPORT_PRIVATE static JSObject* importOfCompiledEntryPoint(AbstractModuleRecord*);
    JS_EXPORT_PRIVATE void didFinishModuleEvaluation(ModuleProgramExecutable*);
    JSModuleLoader* loader() const;
    bool loaderWasCleared() const;
    JS_EXPORT_PRIVATE static void destroyUnneededInstances(VM&);
    JS_EXPORT_PRIVATE static void destroy(Instance*);

    template<typename Visitor> void visit(Visitor&, bool newOnly);
    void finalizeUnconditionally(bool newOnly);
    void clearCachesValidatedByMegamorphicCacheEpoch(bool isAtEndOfCollection = false, bool newOnly = false);
    void sweepKeyedTables(bool newOnly);
    void forgetNewTarget();

    static constexpr ptrdiff_t offsetOfRuntimeTable() { return OBJECT_OFFSETOF(Instance, runtimeTable); }
    static constexpr ptrdiff_t offsetOfVM() { return OBJECT_OFFSETOF(Instance, vm); }
    static constexpr ptrdiff_t offsetOfSoftStackLimit() { return OBJECT_OFFSETOF(Instance, stackMirror) + StackManager::Mirror::offsetOfSoftStackLimit(); }
    static constexpr ptrdiff_t offsetOfTrapAwareSoftStackLimit() { return OBJECT_OFFSETOF(Instance, stackMirror) + StackManager::Mirror::offsetOfTrapAwareSoftStackLimit(); }
    static constexpr ptrdiff_t offsetOfGlobalObject() { return OBJECT_OFFSETOF(Instance, globalObject); }
    static constexpr ptrdiff_t offsetOfStates() { return OBJECT_OFFSETOF(Instance, states); }
    static constexpr ptrdiff_t offsetOfMissCounters() { return OBJECT_OFFSETOF(Instance, missCounters); }
    static constexpr ptrdiff_t offsetOfOwnData() { return OBJECT_OFFSETOF(Instance, ownData); }
    static constexpr ptrdiff_t offsetOfInfos() { return OBJECT_OFFSETOF(Instance, infos); }
    static constexpr ptrdiff_t offsetOfAddressOfStates() { return OBJECT_OFFSETOF(Instance, addressOfStates); }
    static constexpr ptrdiff_t offsetOfMicrotaskQueue() { return OBJECT_OFFSETOF(Instance, microtaskQueue); }
    static constexpr ptrdiff_t offsetOfAwaitMustSuspend() { return OBJECT_OFFSETOF(Instance, awaitMustSuspend); }
    static constexpr ptrdiff_t offsetOfWaiterToResume() { return OBJECT_OFFSETOF(Instance, waiterToResume); }
    static constexpr ptrdiff_t offsetOfResolutionForWaiter() { return OBJECT_OFFSETOF(Instance, resolutionForWaiter); }
    static constexpr ptrdiff_t offsetOfAsyncContextOfWaiter() { return OBJECT_OFFSETOF(Instance, asyncContextOfWaiter); }
    static constexpr ptrdiff_t offsetOfSavedFrameEntered() { return OBJECT_OFFSETOF(Instance, savedFrameEntered); }
    static constexpr ptrdiff_t offsetOfProgramIdentifiers() { return OBJECT_OFFSETOF(Instance, programIdentifiers); }
    static constexpr ptrdiff_t offsetOfProgramIdentifierPlaces() { return OBJECT_OFFSETOF(Instance, programIdentifierPlaces); }
    static constexpr ptrdiff_t offsetOfImage() { return OBJECT_OFFSETOF(Instance, image); }
    static constexpr ptrdiff_t offsetOfSharedData() { return OBJECT_OFFSETOF(Instance, sharedData); }
    static constexpr ptrdiff_t offsetOfCode() { return OBJECT_OFFSETOF(Instance, code); }
    static constexpr ptrdiff_t offsetOfCodeGranules() { return OBJECT_OFFSETOF(Instance, codeGranules); }
    static constexpr ptrdiff_t offsetOfSubsequentFunctionStarts() { return OBJECT_OFFSETOF(Instance, subsequentFunctionStarts); }
    JS_EXPORT_PRIVATE Data* ensureData(uint32_t index);
    JS_EXPORT_PRIVATE Data* dataWithSlot(const Slot*, const void* returnAddress);
    void countMisses(uint32_t index, uint32_t count)
    {
        ASSERT(!dataIfExists(index));
        if (!infos[index].stateNumber)
            return;
        uint32_t numberOfSlots = infos[index].numberOfSlotsForMisses();
        if (!numberOfSlots)
            return;
        uint16_t& counter = missCounters[index % numberOfMissCounters];
        uint32_t after = counter + count;
        if (after >= cacheMissLimitFor(numberOfSlots)) {
            ensureDataIfThereIsRoom(index);
            return;
        }
        counter = static_cast<uint16_t>(after);
    }

    static constexpr uint16_t stateOfLinkedWithoutData() { return OBJECT_OFFSETOF(Instance, sharedDataOfLinkedFunctions) / sizeof(Data*); }
    static constexpr unsigned numberOfMissCounters = 4096;
    static constexpr unsigned maxNumberOfOwnData = 56 * 1024;
    static constexpr uint32_t minStateWithData = 1u << 17;
    static constexpr unsigned stateWithDataShift = 4;
    bool isLinked(uint32_t index) const { return states[infos[index].stateNumber]; }
    Data* dataIfExists(uint32_t index) const
    {
        uint16_t number = states[infos[index].stateNumber];
        return number > stateOfLinkedWithoutData() ? std::bit_cast<const BiasedDataPointer*>(this)[number].get() : nullptr;
    }
    bool hasRoomForData() const;
    JS_EXPORT_PRIVATE void ensureDataIfThereIsRoom(uint32_t index);
    void setData(uint32_t index, Data*);
    void setNotLinked(uint32_t index);
    void noteTransitionCached(Slot*);
    void noteCellAdded(JSCell*);
    PolymorphicSlots* makeSiteSlots(Data*, const Slot*, UniquedStringImpl* name);
    void* allocateForData(size_t);
    void freeDataMemory(void*, size_t);
    void didStopWatching(Data*, Slot*, bool wasWatched);
    void validateSlotsNoLongerWatched(const Slot* beingFilled, bool isAtCollection);
    void forgetSlotsNoLongerWatched(Data*);
    // A function gets slots of its own when it has missed once for each and this often more: in its second run as a rule.
    static constexpr uint32_t missesBeyondItsSlots = 4;
    static constexpr uint32_t cacheMissLimitFor(uint32_t numSlots) { return std::min<uint32_t>(numSlots + missesBeyondItsSlots, std::numeric_limits<uint16_t>::max()); }

    JS_EXPORT_PRIVATE JSCell** environmentSlot(ImageEnvironment) const;

    Structure* literalStructure(Structure* empty, std::span<UniquedStringImpl* const>, uint32_t numberOfRowPlusOne = 0);
    Structure* emptyStructureWithoutPrototype(unsigned inlineCapacity);

    struct PropertyRunTarget {
        Structure* last { nullptr };
        Vector<StructureID, 4> prototypeStructures;
    };
    const PropertyRunTarget& propertyRunTarget(Structure*, const uint32_t* run, const ScopedLambda<void(Vector<UniquedStringImpl*, 16>&)>& collectNames);
    // One address for every row of the same names, for `run`: so that whoever adds them to an object of a structure comes to the same one.
    const uint32_t* canonicalPropertyRun(std::span<UniquedStringImpl* const>);
    bool hasPropertyRunTarget(Structure*, std::span<UniquedStringImpl* const>);
    struct CopiedProperties {
        Structure* last { nullptr };
        Vector<std::pair<PropertyOffset, PropertyOffset>, 8> offsets;
    };
    const CopiedProperties& copiedProperties(Structure* target, Structure* source, const IdentifierSet* excluded);
    struct StructuresOfClass {
        Structure* ofClassObject { nullptr };
        Structure* ofPrototype { nullptr };
    };
    StructuresOfClass structuresOfClass(uint32_t classIndex, JSObject* parentOfClassObject, JSCell* parentOfPrototype);
    void noteStructuresOfClass(uint32_t classIndex, JSObject* parentOfClassObject, JSCell* parentOfPrototype, StructuresOfClass);
    static constexpr unsigned maxLearnedInlineCapacity = 16;
    void noteOutOfLineProperty(Structure*);
    unsigned learnedRoomOfRow(uint32_t number);
    unsigned frameSizeInBytesOf(uint32_t function) const;
    const void* returnAddressOfResumeFunctions() const { return runtimeTable[static_cast<unsigned>(Entry::ReturnAddressOfResumeFunctions)]; }
    JS_EXPORT_PRIVATE void forgetResolvedGlobalScopes();
    JS_EXPORT_PRIVATE void forgetValuesOfGlobalProperties();
    void noteRunOfProperties(Structure* from, Structure* to);
    JSFunction* constructorOfObjectsWith(Structure*);
    void learnInlineCapacity(JSFunction* constructor, Structure*);
    static PropertyOffset offsetAfter(PropertyOffset offset, unsigned inlineCapacity)
    {
        if (offset == invalidOffset)
            return inlineCapacity ? 0 : firstOutOfLineOffset;
        return offset + 1 == static_cast<PropertyOffset>(inlineCapacity) ? firstOutOfLineOffset : offset + 1;
    }

    struct CopyForMore {
        Structure* structure { nullptr };
        std::array<PropertyOffset, 2> offsets { invalidOffset, invalidOffset };
    };
    const CopyForMore* copyForMore(Structure* source, uint64_t more);
    struct CopyExcluding {
        Structure* structure { nullptr };
        Vector<PropertyOffset, 4> offsets;
        Vector<RefPtr<UniquedStringImpl>, 4> excluded;
    };
    const CopyExcluding* copyExcluding(Structure* source, std::span<UniquedStringImpl* const> excluded);
    const CopyExcluding* setCopyExcluding(Structure* source, CopyExcluding&&);
    const CopyForMore* setCopyForMore(Structure* source, uint64_t more, CopyForMore);
    struct SpreadLiteralPlan {
        static constexpr unsigned maxSources = 3;
        static constexpr uint32_t isSpread = UINT32_MAX;
        using Sources = std::array<Structure*, maxSources>;
        struct Move {
            uint32_t value;
            PropertyOffset from;
        };
        const void* site { nullptr };
        Sources sources { };
        Structure* structure { nullptr };
        Vector<Move, 8> moves;
    };
    const SpreadLiteralPlan* spreadLiteralPlan(const void* site, const SpreadLiteralPlan::Sources&);
    const SpreadLiteralPlan* addSpreadLiteralPlan(SpreadLiteralPlan&&);
    static constexpr ptrdiff_t offsetOfStructureIDBase() { return offsetBelow(OBJECT_OFFSETOF(FieldsBelow, structureIDBase)); }
    static constexpr ptrdiff_t offsetOfPlacesByNameOfStructureIDZero() { return OBJECT_OFFSETOF(Instance, placesByNameOfStructureIDZero); }
    static constexpr ptrdiff_t offsetOfSlotsOfPlaceTable() { return OBJECT_OFFSETOF(Instance, slotsOfPlaceTable); }
    static constexpr unsigned numberOfKindsOfEntriesByName = 32;
    static constexpr ptrdiff_t offsetOfHandlersOfReadsByName(unsigned form) { return OBJECT_OFFSETOF(Instance, handlersOfReadsByName) + form * numberOfKindsOfEntriesByName * sizeof(void*); }
    static constexpr ptrdiff_t offsetOfHandlersOfStoresByName(unsigned variant) { return OBJECT_OFFSETOF(Instance, handlersOfStoresByName) + variant * numberOfKindsOfEntriesByName * sizeof(void*); }
    static constexpr ptrdiff_t offsetOfCellCageBase() { return OBJECT_OFFSETOF(Instance, cellCageBase); }
    static constexpr ptrdiff_t offsetOfNumberTag() { return OBJECT_OFFSETOF(Instance, numberTag); }
    static constexpr ptrdiff_t offsetOfNotCellMask() { return OBJECT_OFFSETOF(Instance, notCellMask); }
    static constexpr ptrdiff_t offsetOfNumberBoxBase() { return OBJECT_OFFSETOF(Instance, numberBoxBase); }
    static constexpr ptrdiff_t offsetOfBoxedNaN() { return offsetBelow(OBJECT_OFFSETOF(FieldsBelow, boxedNaN)); }
    static constexpr int leastIntegerKeptAsDouble = -1;
    static constexpr int greatestIntegerKeptAsDouble = 127;
    static constexpr uint64_t bitsOfOtherDoublesKept[] = {
        std::bit_cast<uint64_t>(0.5), std::bit_cast<uint64_t>(65536.0), std::bit_cast<uint64_t>(2147483647.0), std::bit_cast<uint64_t>(2147483648.0), std::bit_cast<uint64_t>(-2147483648.0),
        std::bit_cast<uint64_t>(4294967295.0), std::bit_cast<uint64_t>(4294967296.0), std::bit_cast<uint64_t>(-4294967296.0), std::bit_cast<uint64_t>(9007199254740992.0), std::bit_cast<uint64_t>(-9007199254740992.0),
        std::bit_cast<uint64_t>(9223372036854775808.0), 0x7ff8ull << 48, Typed::undefinedDouble, Typed::nullDouble, Typed::absentDouble,
    };
    static constexpr ptrdiff_t offsetOfIntegersKeptAsDoubles() { return OBJECT_OFFSETOF(Instance, integersKeptAsDoubles); }
    static std::optional<ptrdiff_t> offsetOfDouble(double value)
    {
        if (value == 1)
            return offsetBelow(OBJECT_OFFSETOF(FieldsBelow, one));
        if (std::bit_cast<uint64_t>(value) == 0x7ff8ull << 48)
            return offsetBelow(OBJECT_OFFSETOF(FieldsBelow, pureNaN));
        if (value >= leastIntegerKeptAsDouble && value <= greatestIntegerKeptAsDouble && value && value == static_cast<int>(value))
            return offsetOfIntegersKeptAsDoubles() + (static_cast<int>(value) - leastIntegerKeptAsDouble) * static_cast<ptrdiff_t>(sizeof(double));
        if (std::bit_cast<uint64_t>(value) == Typed::undefinedDouble)
            return OBJECT_OFFSETOF(Instance, undefinedDouble);
        for (size_t i = 0; i < std::size(bitsOfOtherDoublesKept); ++i) {
            if (std::bit_cast<uint64_t>(value) == bitsOfOtherDoublesKept[i])
                return OBJECT_OFFSETOF(Instance, otherDoubles) + i * static_cast<ptrdiff_t>(sizeof(double));
        }
        return std::nullopt;
    }
    static constexpr ptrdiff_t offsetOfFieldIDAtDirectLocationBase() { return OBJECT_OFFSETOF(Instance, fieldIDAtDirectLocationBase); }
    static constexpr ptrdiff_t offsetOfObjectPrototype() { return OBJECT_OFFSETOF(Instance, objectPrototype); }

    struct FieldsBelow {
        uint64_t newArrayWithInt32Header;
        uint64_t newArrayWithDoubleHeader;
        uint64_t newArrayWithContiguousHeader;
        uint64_t newCopyOnWriteArrayWithContiguousHeader;
        uint64_t stringHeader;
        void* ropeStringAllocator;
        void* arrayAllocator;
        double one;
        uint64_t pureNaN;
        uint32_t mapStructureID;
        uint32_t setStructureID;
        uint32_t regExpStructureID;
        uint32_t weakMapStructureID;
        // Goes up whenever the `prototype` property of a closure gets another value. See Typed::State::structureForThis().
        uint32_t typedPrototypeEpoch;
        uint32_t typedClosureStructureID;
        // By the number Typed::Builtins gives a function or symbol of the engine. They live as long as the global object.
        JSCell** typedBuiltins;
        uintptr_t structureIDBase;
        // See Typed::startInStringTable().
        uint64_t* typedStrings;
        EncodedJSValue boxedNaN;
        JSCell* emptyString;
    };
    FieldsBelow& fieldsBelow() { return reinterpret_cast<FieldsBelow*>(this)[-1]; }
    const FieldsBelow& fieldsBelow() const { return reinterpret_cast<const FieldsBelow*>(this)[-1]; }
    static constexpr ptrdiff_t offsetBelow(size_t offsetInFieldsBelow) { return static_cast<ptrdiff_t>(offsetInFieldsBelow) - static_cast<ptrdiff_t>(sizeof(FieldsBelow)); }

    BiasedDataPointer sharedData;
    // Typed::Data::shapeSetsOffset, as an address.
    const uint32_t* typedShapeSets { nullptr };
    VM* vm;
    StackManager::Mirror stackMirror;
    // Heap::m_barrierThreshold and Heap::m_mutatorShouldBeFenced. Heap::setMutatorShouldBeFenced() keeps them.
    uint32_t barrierThreshold { 0 };
    uint8_t mutatorShouldBeFenced { 0 };
    uint8_t awaitMustSuspend { 0 };
    bool isResumingWaiters { false };
    uint64_t typedSlimClosureHeaders[2] { };
    uint64_t undefinedDouble;
    uintptr_t numberBoxBase;
    // By function: the one closure of a function that captures nothing, once it has been asked for: its Cell32 LESS ONE, so that a row nobody has written stands for the Cell32 1, which
    // nothing is: neither a cell nor what a slot holds in the place of one. With wide references a pointer. Typed::State::closureIfMade().
    void* typedClosures { nullptr };
    void* singleCharacterStrings { nullptr };
    int64_t numberTag;
    int64_t notCellMask;
    JSGlobalObject* globalObject;
    JSCell* waiterToResume { nullptr };
    EncodedJSValue resolutionForWaiter { 0 };
    EncodedJSValue asyncContextOfWaiter { 0 };
    JSCell* savedFrameEntered { nullptr };
    // By function: its ClosureWord. Read-only: ProgramData::closureWords().
    const uint64_t* typedFunctions { nullptr };
    // MarkedBlockCage::base().
    uintptr_t cellCageBase;
    const void* returnAddressOfSavedFrames { nullptr };
    void* spaceOfSavedFrames { nullptr };
    uint64_t headerOfSavedFrames { 0 };
    static constexpr unsigned arrayKindShift = 1;
    static constexpr unsigned numberOfArrayKinds = 16;
    static_assert(((IndexingShapeMask | CopyOnWrite) >> arrayKindShift) == numberOfArrayKinds - 1);
    uint32_t originalArrayStructureIDs[numberOfArrayKinds] { };
    static constexpr ptrdiff_t offsetOfOriginalArrayStructureIDs() { return OBJECT_OFFSETOF(Instance, originalArrayStructureIDs); }
    uint32_t firstRowOfThisPlusOne { UINT32_MAX };
    static constexpr ptrdiff_t offsetOfFirstRowOfThisPlusOne() { return OBJECT_OFFSETOF(Instance, firstRowOfThisPlusOne); }
    const double* powersOfTen { nullptr };
    const uint32_t* functionMetadataOffsets;
    const uint8_t* code;
    const uint32_t* codeGranules;
    const uint32_t* subsequentFunctionStarts;
    // What an allocation stores into the object. All clear until the first one, which takes the slow path.
    struct TypedAllocation {
        uint64_t header;
        // The order word and, in the upper half, the shape word: what is at offset 8. Only of Record and Instance layouts.
        uint64_t words;
        void* allocator;
        // Only of Record and Instance layouts, with an allocator and Typed::AllocationData::usesFieldTemplate: the bytes of a new object from Typed::offsetOfFirstField to the
        // allocator's cell size, every field absent. Set before `allocator`. See Typed::State::fieldTemplate().
        const void* fieldTemplate;
    };
    Typed::State* typed { nullptr };
    const FunctionInfo* infos;
    static constexpr unsigned typedCardShift = 8;
    static constexpr uint8_t typedCardIsStoredTo = 1;
    static constexpr uint8_t typedCardWasLookedAt = 2;
    void didStoreRootInStatics(uint32_t offset) { typedCards[offset >> typedCardShift] = typedCardIsStoredTo; }
    // By the number of the Typed::AllocationData: its row, or 0 until the first object. Row r is the r-th TypedAllocation below this address: rows are given out in the order of first use.
    uint16_t* typedAllocationRows { nullptr };
    TypedAllocation* typedAllocation(uint32_t number) const
    {
        uint16_t row = typedAllocationRows[number];
        return row ? reinterpret_cast<TypedAllocation*>(typedAllocationRows) - row : nullptr;
    }
    // By Typed::LayoutData::index of a class, a byte. Not 0: under the name of a static method or accessor that the class declares or inherits
    // its class object may find something else by now. Compiled code asks the class object. See Typed::State::watchMembers().
    uint8_t* typedAlteredClassObjects { nullptr };
    // Of a closure as it is allocated: of a plain function, an async function, a generator function, an async generator function, which
    // differ in the prototype; of a class object, which has properties that its Structure does not list. Clear until there is a closure.
    static constexpr unsigned numberOfTypedClosureStructures = 5;
    uint32_t typedClosureStructureIDs[numberOfTypedClosureStructures - 1] { };
    uint32_t& typedClosureStructureID(unsigned which) { return which ? typedClosureStructureIDs[which - 1] : fieldsBelow().typedClosureStructureID; }
    uint64_t typedClosureStructureIDPair { 0 };
    LocalAllocator* typedSlimClosureAllocators[3] { };
    JSCell* typedSlimClosureLengthGetter { nullptr };
    JSCell* typedSlimClosureNameGetter { nullptr };
    // See Typed::fieldIsPinned. Not 0: some field with that guard may hold something else than the string it is pinned to. It is never cleared.
    uint8_t typedAlteredLiterals[256] { };
    // For Stub::StringIsText: the Typed::Data, with the words of the texts of the image right behind its header (Typed::Data::stringsOffset).
    const uint8_t* typedData { nullptr };
    JSCell** typedWideStrings { nullptr };
    JSCell** typedLiteralStrings { nullptr };
    uint64_t* typedLiteralStringCache { nullptr };
    uint8_t* typedCards { nullptr };
    // What closures have for an executable: by Typed::FunctionTables::kindOfExecutable().
    static constexpr unsigned numberOfTypedExecutables = 6;
    JSCell* typedExecutables[numberOfTypedExecutables] { };
    uint8_t* typedStatics { nullptr };
    struct KeyedRead;
    struct KeyedAddition;
    KeyedRead* keyedReads { nullptr };
    Slot* keyedHas { nullptr };
    KeyedAddition* keyedAdditions { nullptr };
    // By shape, for Stub::DidCreateTypedField. Null: nothing has asked yet. [0]: the number of key orders of the layout. [1 + order]: the
    // order as 16-bit words: its length, then for each key the offset of its slot in the cell, or Typed::noSlotInCell. Then for each count
    // from 0 to the length a pair: the slot of the field that was created at that count the last time the order did not say so, or
    // Typed::noSlotInCell, and the order the object goes by from there on.
    uintptr_t** typedOrderKeys { nullptr };
    uint16_t* typedPlacesOfOrderKeys { nullptr };
    uint16_t numberOfTypedPlacesOfOrderKeysTaken { 0 };
    static constexpr ptrdiff_t offsetOfTypedOrderKeys() { return OBJECT_OFFSETOF(Instance, typedOrderKeys); }
    static constexpr ptrdiff_t offsetOfTypedPlacesOfOrderKeys() { return OBJECT_OFFSETOF(Instance, typedPlacesOfOrderKeys); }
    static size_t numberOfPlacesForTypedOrderKeys(uint32_t numberOfLayouts) { return std::min<size_t>(numberOfLayouts, UINT16_MAX) + 1; }
    void setTypedOrderKeys(uint32_t shape, uintptr_t* keys)
    {
        if (!typedPlacesOfOrderKeys[shape]) {
            if (numberOfTypedPlacesOfOrderKeysTaken == UINT16_MAX)
                return;
            typedPlacesOfOrderKeys[shape] = ++numberOfTypedPlacesOfOrderKeysTaken;
        }
        typedOrderKeys[typedPlacesOfOrderKeys[shape]] = keys;
    }
    // A cache, by the low bits of the shape word: order word and shape word of the object whose keys were listed last under the entry, and the list, see Typed::Object::keys().
    // Typed::State keeps the lists alive. No object has the shape word 0.
    struct TypedKeyList {
        uint32_t orderWord;
        uint32_t shapeWord;
        JSCell* list;
    };
    static constexpr uint32_t numberOfTypedKeyLists = 1024;
    static constexpr uint32_t indexOfTypedKeyList(uint32_t shapeWord, uint32_t orderWord) { return (shapeWord + orderWord) % numberOfTypedKeyLists; }
    TypedKeyList* typedKeyLists { nullptr };
    static constexpr ptrdiff_t offsetOfTypedKeyLists() { return OBJECT_OFFSETOF(Instance, typedKeyLists); }
    JSCell* emptyKeyList { nullptr };
    static constexpr ptrdiff_t offsetOfEmptyKeyList() { return OBJECT_OFFSETOF(Instance, emptyKeyList); }
    JSCell* typedArrayLengthGetter { nullptr };
    static constexpr ptrdiff_t offsetOfTypedArrayLengthGetter() { return OBJECT_OFFSETOF(Instance, typedArrayLengthGetter); }
    struct TypedArrayWithBuiltinLength {
        uint32_t structureID { 0 };
        uint32_t prototypeStructureID { 0 };
        uint32_t secondPrototypeStructureID { 0 };
        uint32_t unused { 0 };
        JSCell* prototype { nullptr };
        JSCell* secondPrototype { nullptr };
    };
    static_assert(sizeof(TypedArrayWithBuiltinLength) == 32);
    TypedArrayWithBuiltinLength typedArraysWithBuiltinLength[NumberOfTypedArrayTypesExcludingDataView][2] { };
    JSCell* typedArrayViewPrototype { nullptr };
    JSCell* typedArrayLengthAccessor { nullptr };
    intptr_t typedArrayLengthOffsetInButterfly { 0 };
    uint32_t typedArrayViewPrototypeStructureID { 0 };
    bool typedArrayHasBuiltinLength(JSCell* typedArray) const
    {
        uint32_t structureID = typedArray->structureID().bits();
        for (auto& entry : typedArraysWithBuiltinLength[typedArray->type() - FirstTypedArrayType]) {
            if (entry.structureID != structureID)
                continue;
            return entry.prototype->structureID().bits() == entry.prototypeStructureID && entry.secondPrototype->structureID().bits() == entry.secondPrototypeStructureID
                && typedArrayViewPrototype->structureID().bits() == typedArrayViewPrototypeStructureID && *std::bit_cast<JSCell* const*>(std::bit_cast<intptr_t>(asObject(typedArrayViewPrototype)->butterfly()) + typedArrayLengthOffsetInButterfly) == typedArrayLengthAccessor;
        }
        return false;
    }
    static constexpr ptrdiff_t offsetOfTypedArraysWithBuiltinLength() { return OBJECT_OFFSETOF(Instance, typedArraysWithBuiltinLength); }
    static constexpr ptrdiff_t offsetOfTypedArrayViewPrototype() { return OBJECT_OFFSETOF(Instance, typedArrayViewPrototype); }
    static constexpr ptrdiff_t offsetOfTypedArrayLengthAccessor() { return OBJECT_OFFSETOF(Instance, typedArrayLengthAccessor); }
    static constexpr ptrdiff_t offsetOfTypedArrayLengthOffsetInButterfly() { return OBJECT_OFFSETOF(Instance, typedArrayLengthOffsetInButterfly); }
    static constexpr ptrdiff_t offsetOfTypedArrayViewPrototypeStructureID() { return OBJECT_OFFSETOF(Instance, typedArrayViewPrototypeStructureID); }
    UniquedStringImpl* const* programIdentifiers;
    const uint16_t* programIdentifierPlaces;
    uintptr_t placesByNameOfStructureIDZero;
    Slot* slotsOfPlaceTable { nullptr };
    void* handlersOfReadsByName[3][numberOfKindsOfEntriesByName] { };
    void* handlersOfStoresByName[6][numberOfKindsOfEntriesByName] { };
    JSObject* objectPrototype;
    JSCell* functionPrototypeCall { nullptr };
    uint32_t boundFunctionStructureID { 0 };
    uint32_t hostFunctionStructureID { 0 };
    uintptr_t fieldIDAtDirectLocationBase;
    JSObject* objectPrototypeOfRealm { nullptr };
    static constexpr ptrdiff_t offsetOfObjectPrototypeOfRealm() { return OBJECT_OFFSETOF(Instance, objectPrototypeOfRealm); }
    const uint32_t* displacementsOfNames { nullptr };
    static constexpr ptrdiff_t offsetOfDisplacementsOfNames() { return OBJECT_OFFSETOF(Instance, displacementsOfNames); }
    const uint32_t* placesByNameAndRow { nullptr };
    static constexpr ptrdiff_t offsetOfPlacesByNameAndRow() { return OBJECT_OFFSETOF(Instance, placesByNameAndRow); }
    static constexpr ptrdiff_t offsetOfTypedPrototypeEpoch() { return offsetBelow(OBJECT_OFFSETOF(FieldsBelow, typedPrototypeEpoch)); }
    static constexpr ptrdiff_t offsetOfFunctionPrototypeCall() { return OBJECT_OFFSETOF(Instance, functionPrototypeCall); }
    static constexpr ptrdiff_t offsetOfBoundFunctionStructureID() { return OBJECT_OFFSETOF(Instance, boundFunctionStructureID); }
    static constexpr ptrdiff_t offsetOfHostFunctionStructureID() { return OBJECT_OFFSETOF(Instance, hostFunctionStructureID); }
    JSCell* boundFunctionExecutable { nullptr };
    Structure* argumentsStructure { nullptr };
    std::array<Structure*, 2> emptyStructuresFilledByKey { };
    std::array<uint32_t, 128> stepsAskedForOnce { };
    bool wasStepAskedForBefore(StructureID structureID, const UniquedStringImpl* name)
    {
        uint32_t step = (structureID.bits() + static_cast<uint32_t>(std::bit_cast<uintptr_t>(name) >> 4)) * 0x9e3779b1u | 1;
        uint32_t& noted = stepsAskedForOnce[step >> 25];
        return std::exchange(noted, step) == step;
    }
    JSCell* calleeOfArguments { nullptr };
    JSCell* iteratorOfArguments { nullptr };
    static constexpr ptrdiff_t offsetOfCalleeOfArguments() { return OBJECT_OFFSETOF(Instance, calleeOfArguments); }
    static constexpr ptrdiff_t offsetOfIteratorOfArguments() { return OBJECT_OFFSETOF(Instance, iteratorOfArguments); }
    static constexpr ptrdiff_t offsetOfBoundFunctionExecutable() { return OBJECT_OFFSETOF(Instance, boundFunctionExecutable); }
    const ClassInfo* boundFunctionClassInfo { nullptr };
    static constexpr ptrdiff_t offsetOfBoundFunctionClassInfo() { return OBJECT_OFFSETOF(Instance, boundFunctionClassInfo); }
    EncodedJSValue intrinsics[ImmutableIntrinsics::maximumCount];
    EncodedJSValue linkTimeConstants[numberOfLinkTimeConstants];
    static constexpr unsigned numberOfReceivers = 16;
    uint32_t receiverStructureIDs[numberOfReceivers] { };
    static constexpr ptrdiff_t offsetOfReceiverStructureID(Receiver receiver)
    {
        switch (receiver) {
        case Receiver::Map:
            return offsetBelow(OBJECT_OFFSETOF(FieldsBelow, mapStructureID));
        case Receiver::Set:
            return offsetBelow(OBJECT_OFFSETOF(FieldsBelow, setStructureID));
        case Receiver::RegExp:
            return offsetBelow(OBJECT_OFFSETOF(FieldsBelow, regExpStructureID));
        case Receiver::WeakMap:
            return offsetBelow(OBJECT_OFFSETOF(FieldsBelow, weakMapStructureID));
        default:
            return OBJECT_OFFSETOF(Instance, receiverStructureIDs) + static_cast<unsigned>(receiver) * sizeof(uint32_t);
        }
    }
    uint32_t& receiverStructureID(Receiver receiver) { return *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(this) + offsetOfReceiverStructureID(receiver)); }
    struct Collections;
    Collections* collections;
    const uint16_t* addressOfStates;
    MicrotaskQueue* microtaskQueue;
    const uint8_t* image;
    VMProgram* program;
    const ProgramData* programData;
    void** runtimeTable;
    static constexpr unsigned maxVectorLengthGrownByStub = 31;
    uint8_t nextVectorLengths[maxVectorLengthGrownByStub + 1] { };
    static constexpr ptrdiff_t offsetOfNextVectorLengths() { return OBJECT_OFFSETOF(Instance, nextVectorLengths); }
    uint64_t newArrayWithTypedContiguousHeader { 0 };
    uint64_t regExpMatchesArrayHeader { 0 };
    static constexpr ptrdiff_t offsetOfRegExpMatchesArrayHeader() { return OBJECT_OFFSETOF(Instance, regExpMatchesArrayHeader); }
    Structure* structureOfTypedContiguousArrays { nullptr };
    JS_EXPORT_PRIVATE Structure* typedContiguousArrayStructure();
    uint64_t newCopyOnWriteArrayHeaders[2] { };
    uint64_t arrayInTheMakingHeaders[3] { };
    Structure* arrayInTheMakingStructures[3] { };
    static constexpr unsigned indexOfArrayInTheMaking(IndexingType indexingType) { return ((indexingType & IndexingShapeMask) - Int32Shape) / 2; }
    static constexpr ptrdiff_t offsetOfArrayInTheMakingHeaders() { return OBJECT_OFFSETOF(Instance, arrayInTheMakingHeaders); }
    JS_EXPORT_PRIVATE bool isArrayInTheMaking(JSCell*) const;
    Structure* arrayInTheMakingStructure(IndexingType);
    void* typedCastTransitions { nullptr };
    static ptrdiff_t offsetOfNewArrayHeader(IndexingType indexingType)
    {
        switch (indexingType) {
        case ArrayWithInt32:
            return offsetBelow(OBJECT_OFFSETOF(FieldsBelow, newArrayWithInt32Header));
        case ArrayWithDouble:
            return offsetBelow(OBJECT_OFFSETOF(FieldsBelow, newArrayWithDoubleHeader));
        case ArrayWithTypedContiguous:
            return OBJECT_OFFSETOF(Instance, newArrayWithTypedContiguousHeader);
        default:
            RELEASE_ASSERT(indexingType == ArrayWithContiguous);
            return offsetBelow(OBJECT_OFFSETOF(FieldsBelow, newArrayWithContiguousHeader));
        }
    }
    static constexpr ptrdiff_t offsetOfNewArrayWithInt32StructureID() { return offsetBelow(OBJECT_OFFSETOF(FieldsBelow, newArrayWithInt32Header)); }
    static constexpr ptrdiff_t offsetOfNewArrayWithContiguousStructureID() { return offsetBelow(OBJECT_OFFSETOF(FieldsBelow, newArrayWithContiguousHeader)); }
    static constexpr ptrdiff_t offsetOfNewCopyOnWriteArrayHeader(IndexingType indexingType)
    {
        if ((indexingType & IndexingShapeMask) == ContiguousShape)
            return offsetBelow(OBJECT_OFFSETOF(FieldsBelow, newCopyOnWriteArrayWithContiguousHeader));
        return OBJECT_OFFSETOF(Instance, newCopyOnWriteArrayHeaders) + ((indexingType & IndexingShapeMask) - Int32Shape) / 2 * sizeof(uint64_t);
    }
    static constexpr ptrdiff_t offsetOfStringHeader() { return offsetBelow(OBJECT_OFFSETOF(FieldsBelow, stringHeader)); }
    uint32_t arraysLackIsConcatSpreadable { 0 };
    uint32_t arraysLackInheritedElements { 0 };
    uint64_t headerOfIteratorResults { 0 };
    static constexpr ptrdiff_t offsetOfHeaderOfIteratorResults() { return OBJECT_OFFSETOF(Instance, headerOfIteratorResults); }
    static constexpr ptrdiff_t offsetOfStructureIDOfIteratorResults() { return OBJECT_OFFSETOF(Instance, headerOfIteratorResults); }
    // The accessors of the embedder's Buffer, by row of FOR_EACH_AOT_TYPED_BUFFER_ACCESSOR. An object with the structure finds the function under the name of the row: it has no property of
    // that name, it inherits from the holder, and a watchpoint sees to it that the holder keeps the function there. Zero: nobody has looked yet. When the holder loses the function, both are
    // cleared for good. See operationAOTTypedLookForBufferAccessor().
#define AOT_COUNT_BUFFER_ACCESSOR(...) + 1
    static constexpr unsigned numberOfBufferAccessors = 0 FOR_EACH_AOT_TYPED_BUFFER_ACCESSOR(AOT_COUNT_BUFFER_ACCESSOR);
#undef AOT_COUNT_BUFFER_ACCESSOR
    // No Structure has the ID.
    static constexpr uint32_t bufferAccessorIsNotLookedFor = 1;
    uint32_t bufferAccessorStructureIDs[numberOfBufferAccessors] { };
    uint8_t bufferAccessorAttempts[numberOfBufferAccessors] { };
    JSCell* bufferAccessors[numberOfBufferAccessors] { };
    Structure* bufferStructure { nullptr };
    JSObject* bufferAccessorHolder { nullptr };
    static constexpr ptrdiff_t offsetOfBufferAccessorStructureID(unsigned row) { return OBJECT_OFFSETOF(Instance, bufferAccessorStructureIDs) + row * sizeof(uint32_t); }
    static constexpr ptrdiff_t offsetOfBufferAccessor(unsigned row) { return OBJECT_OFFSETOF(Instance, bufferAccessors) + row * sizeof(JSCell*); }
    void watchBufferAccessor(unsigned row, const ObjectPropertyCondition&);
    uint32_t asyncLocalStorageStructureIDs[2] { };
    uint32_t asyncContextFrameStructureID { 0 };
    uint32_t placeOfDefaultValueInAsyncLocalStorage { 0 };
    Structure* asyncLocalStorageStructures[2] { };
    Structure* asyncContextFrameStructure { nullptr };
    static constexpr ptrdiff_t offsetOfAsyncLocalStorageStructureID(unsigned which) { return OBJECT_OFFSETOF(Instance, asyncLocalStorageStructureIDs) + which * sizeof(uint32_t); }
    static constexpr ptrdiff_t offsetOfAsyncContextFrameStructureID() { return OBJECT_OFFSETOF(Instance, asyncContextFrameStructureID); }
    static constexpr ptrdiff_t offsetOfPlaceOfDefaultValueInAsyncLocalStorage() { return OBJECT_OFFSETOF(Instance, placeOfDefaultValueInAsyncLocalStorage); }
    void noteAsyncLocalStorage(JSObject* storage, JSObject* frame, JSCell* getStore);
    void* auxiliarySpace { nullptr };
    void* cellSpace { nullptr };
    const void* knownGrowthsOfCompactTables { nullptr };
    // See Typed::State::rememberElement().
    JSCell** youngRootsCursor { nullptr };
    JSCell** youngRootsEnd { nullptr };
    Latin1Character* jsonBufferStart { nullptr };
    Latin1Character* jsonBufferEnd { nullptr };
    EncodedJSValue preparedJSONValue { 0 };
    StringImpl* preparedJSONText { nullptr };
    Vector<JSONWideSegment>* jsonWideSegments { nullptr };
    uint32_t jsonWideSegmentCount { 0 };
    static constexpr ptrdiff_t offsetOfYoungRootsCursor() { return OBJECT_OFFSETOF(Instance, youngRootsCursor); }
    static constexpr ptrdiff_t offsetOfYoungRootsEnd() { return OBJECT_OFFSETOF(Instance, youngRootsEnd); }
    static constexpr ptrdiff_t offsetOfJSONBufferStart() { return OBJECT_OFFSETOF(Instance, jsonBufferStart); }
    static constexpr ptrdiff_t offsetOfJSONBufferEnd() { return OBJECT_OFFSETOF(Instance, jsonBufferEnd); }
    static constexpr ptrdiff_t offsetOfJSONWideSegmentCount() { return OBJECT_OFFSETOF(Instance, jsonWideSegmentCount); }
    static constexpr ptrdiff_t offsetOfBarrierThreshold() { return OBJECT_OFFSETOF(Instance, barrierThreshold); }
    static constexpr ptrdiff_t offsetOfMutatorShouldBeFenced() { return OBJECT_OFFSETOF(Instance, mutatorShouldBeFenced); }
    double integersKeptAsDoubles[greatestIntegerKeptAsDouble - leastIntegerKeptAsDouble + 1];
    uint64_t otherDoubles[std::size(bitsOfOtherDoublesKept)];
    JSCell* sentinelString { nullptr };
    // Zero until the first Typed::CompactTable is made.
    uint32_t compactTableStructureID { 0 };
    static constexpr ptrdiff_t offsetOfAuxiliarySpace() { return OBJECT_OFFSETOF(Instance, auxiliarySpace); }
    static constexpr ptrdiff_t offsetOfCellSpace() { return OBJECT_OFFSETOF(Instance, cellSpace); }
    static constexpr ptrdiff_t offsetOfKnownGrowthsOfCompactTables() { return OBJECT_OFFSETOF(Instance, knownGrowthsOfCompactTables); }
    static constexpr ptrdiff_t offsetOfCompactTableStructureID() { return OBJECT_OFFSETOF(Instance, compactTableStructureID); }
    static constexpr ptrdiff_t offsetOfArrayAllocator() { return offsetBelow(OBJECT_OFFSETOF(FieldsBelow, arrayAllocator)); }
    static constexpr ptrdiff_t offsetOfRopeStringAllocator() { return offsetBelow(OBJECT_OFFSETOF(FieldsBelow, ropeStringAllocator)); }
    static constexpr ptrdiff_t offsetOfSingleCharacterStrings() { return OBJECT_OFFSETOF(Instance, singleCharacterStrings); }
    static constexpr ptrdiff_t offsetOfEmptyString() { return offsetBelow(OBJECT_OFFSETOF(FieldsBelow, emptyString)); }
    static constexpr ptrdiff_t offsetOfPowersOfTen() { return OBJECT_OFFSETOF(Instance, powersOfTen); }
    static constexpr ptrdiff_t offsetOfStringStructureID() { return offsetBelow(OBJECT_OFFSETOF(FieldsBelow, stringHeader)); }
    const void* smallIntStrings { nullptr };
    const void* intStrings { nullptr };
    static constexpr ptrdiff_t offsetOfSmallIntStrings() { return OBJECT_OFFSETOF(Instance, smallIntStrings); }
    static constexpr ptrdiff_t offsetOfIntStrings() { return OBJECT_OFFSETOF(Instance, intStrings); }
    uint64_t hashSecrets[2] { };
    static constexpr ptrdiff_t offsetOfHashSecrets() { return OBJECT_OFFSETOF(Instance, hashSecrets); }
    enum class InlineAllocation : unsigned { Promise, Generator, AsyncFunctionGenerator, Map, Set, BoundFunction, BigInt, DataPropertyDescriptor, RegExpObject, Arguments };
    static constexpr unsigned numberOfInlineAllocations = 10;
    struct InlineAllocationData {
        void* allocator;
        uint64_t header;
    };
    InlineAllocationData inlineAllocations[numberOfInlineAllocations] { };
    struct TypedArrayAllocation {
        void* allocator;
        uint64_t header;
        uint64_t elementSize;
        uint64_t unused;
    };
    static_assert(sizeof(TypedArrayAllocation) == 32);
    static constexpr unsigned numberOfTypedArrayAllocations = 16;
    TypedArrayAllocation typedArrayAllocations[numberOfTypedArrayAllocations] { };
    void* primitiveAuxiliarySpace { nullptr };
    static constexpr ptrdiff_t offsetOfTypedArrayAllocations() { return OBJECT_OFFSETOF(Instance, typedArrayAllocations); }
    static constexpr ptrdiff_t offsetOfPrimitiveAuxiliarySpace() { return OBJECT_OFFSETOF(Instance, primitiveAuxiliarySpace); }
    static constexpr ptrdiff_t offsetOfAllocatorFor(InlineAllocation kind) { return OBJECT_OFFSETOF(Instance, inlineAllocations) + static_cast<unsigned>(kind) * sizeof(InlineAllocationData) + OBJECT_OFFSETOF(InlineAllocationData, allocator); }
    static constexpr ptrdiff_t offsetOfHeaderFor(InlineAllocation kind) { return OBJECT_OFFSETOF(Instance, inlineAllocations) + static_cast<unsigned>(kind) * sizeof(InlineAllocationData) + OBJECT_OFFSETOF(InlineAllocationData, header); }
    JS_EXPORT_PRIVATE void didHaveBadTime();
    // Whether every object with the Structure has an own property of the name, as HasOwnPropertyCache has it, for a name that is a string of the typed program: the structure ID in the low
    // half, above it the number of the string, in the top bit the answer. No Structure has the ID zero. A collection takes out those of dead Structures.
    static constexpr unsigned numberOfOwnPropertyAnswers = 512;
    static constexpr uint32_t maxNameWithOwnPropertyAnswer = INT32_MAX;
    static constexpr uint32_t saltOfOwnPropertyAnswer(uint32_t name) { return name * 0x9e3779b1u >> 16; }
    static constexpr unsigned ownPropertyAnswerIndex(uint32_t structureID, uint32_t name) { return ((structureID >> 4) ^ saltOfOwnPropertyAnswer(name)) & (numberOfOwnPropertyAnswers - 1); }
    uint64_t ownPropertyAnswers[numberOfOwnPropertyAnswers] { };
    bool hasOwnPropertyAnswers { false };
    static constexpr ptrdiff_t offsetOfOwnPropertyAnswers() { return OBJECT_OFFSETOF(Instance, ownPropertyAnswers); }
    static constexpr unsigned numberOfPlacesOfOwnPropertyAnswer = 4;
    void noteOwnPropertyAnswer(uint32_t structureID, uint32_t name, bool answer)
    {
        unsigned index = ownPropertyAnswerIndex(structureID, name);
        uint64_t pair = structureID | static_cast<uint64_t>(name) << 32;
        unsigned place = numberOfPlacesOfOwnPropertyAnswer;
        for (unsigned step = numberOfPlacesOfOwnPropertyAnswer; step--;) {
            uint64_t noted = ownPropertyAnswers[index ^ step];
            if (!noted || noted << 1 == pair << 1)
                place = step;
        }
        if (place == numberOfPlacesOfOwnPropertyAnswer) {
            keyedTableRandomState = keyedTableRandomState * 1664525u + 1013904223u;
            place = keyedTableRandomState >> 30;
        }
        ownPropertyAnswers[index ^ place] = pair | static_cast<uint64_t>(answer) << 63;
        hasOwnPropertyAnswers = true;
        hasNewOwnPropertyAnswers = true;
    }
    struct OwnPropertyPlace {
        static constexpr uint32_t isPresent = 1;
        // ReadOnly, DontEnum and DontDelete, as PropertyAttribute numbers them.
        static constexpr uint32_t attributes = 2 | 4 | 8;
        // From the butterfly. Otherwise: from the object.
        static constexpr uint32_t isOutOfLine = 16;
        // An accessor, or what the engine works out when it is asked.
        static constexpr uint32_t isNoValue = 32;
        // Of an object of a compiled layout, with isNoValue and without isPresent: the property is a field, which such an object has or has not: its slot says. In the place of the displacement: which byte the slot starts with, of eight that are aligned;
        // how far ~0 is shifted right for a mask of its width; what in it stands for "absent": 0, Typed::absentInt32, absentDouble, absentInteger; which eight bytes of the cell.
        static constexpr uint32_t isCompiledField = 64;
        static constexpr unsigned byteShift = 8;
        static constexpr unsigned maskShiftShift = 11;
        static constexpr unsigned markerShift = 17;
        static constexpr unsigned wordShift = 19;
        // Above: how many bytes from there, with a sign.
        static constexpr unsigned displacementShift = 8;

        UniquedStringImpl* uid;
        uint32_t structureID;
        uint32_t place;
    };
    static constexpr unsigned numberOfOwnPropertyPlaces = 512;
    // Structures lie next to one another and so do names: the factor keeps the pairs of a few shapes and their names apart. A pair has four places: its entry and the three beside it.
    static constexpr unsigned numberOfPlacesOfOwnPropertyPlace = 4;
    static constexpr unsigned ownPropertyPlaceIndex(uint32_t structureID, uintptr_t uid) { return ((uid + 5 * static_cast<uintptr_t>(structureID)) >> 4) & (numberOfOwnPropertyPlaces - 1); }
    OwnPropertyPlace ownPropertyPlaces[numberOfOwnPropertyPlaces] { };
    bool hasOwnPropertyPlaces { false };
    static constexpr ptrdiff_t offsetOfOwnPropertyPlaces() { return OBJECT_OFFSETOF(Instance, ownPropertyPlaces); }
    // It keeps the name while the entry may be in the table: of an entry that says "none", or "a field of a compiled layout", no Structure does. See Typed::State::m_cachedFields.
    void noteOwnPropertyPlace(uint32_t structureID, UniquedStringImpl* uid, uint32_t place);
    // The header of an empty object, of the default size, that inherits from the object: what Object.create() makes. Every collection clears them.
    struct HeirOfPrototype {
        JSCell* prototype;
        uint64_t header;
    };
    static constexpr unsigned numberOfHeirsOfPrototypes = 8;
    static constexpr unsigned heirOfPrototypeIndex(uintptr_t prototype) { return (prototype >> 4) & (numberOfHeirsOfPrototypes - 1); }
    HeirOfPrototype heirsOfPrototypes[numberOfHeirsOfPrototypes] { };
    bool hasHeirsOfPrototypes { false };
    // What Typed::Lowering::createThis() keeps at a site, for the whole program, by new.target: for the sites that meet several. The first three slots of an entry are laid out as at a site; the fourth holds the constructor.
    static constexpr unsigned numberOfThisAllocations = 64;
    static constexpr unsigned slotsPerThisAllocation = 4;
    static_assert(hasOneBitSet(numberOfThisAllocations) && hasOneBitSet(slotsPerThisAllocation * sizeof(Slot)));
    Slot thisAllocations[numberOfThisAllocations * slotsPerThisAllocation] { };
    bool hasThisAllocations { false };
    static constexpr uint16_t maxRunsUnderSymbols = 256;
    static constexpr uint16_t maxRunsOfCopiedNames = 1024;
    uint16_t roomForRunsUnderSymbols;
    uint16_t roomForRunsOfCopiedNames;
    Slot* thisAllocationFor(const JSCell* newTarget) { return &thisAllocations[(std::bit_cast<uintptr_t>(newTarget) >> 4 & (numberOfThisAllocations - 1)) * slotsPerThisAllocation]; }
    static constexpr ptrdiff_t offsetOfThisAllocations() { return OBJECT_OFFSETOF(Instance, thisAllocations); }
    Structure* emptyStructuresOfLiterals[16] { };
    static constexpr ptrdiff_t offsetOfHeirsOfPrototypes() { return OBJECT_OFFSETOF(Instance, heirsOfPrototypes); }
    struct CustomGetter {
        uint32_t structureID;
        uint32_t serial;
        uint16_t epoch;
        UniquedStringImpl* uid;
        void* getter;
        JSObject* passedForReceiver;
        JSGlobalObject* globalObject;
        uint32_t lastSerial;
        uint32_t padding[3];
    };
    static_assert(sizeof(CustomGetter) == 64);
    static constexpr unsigned numberOfCustomGetters = 128;
    static_assert(numberOfCustomGetters * sizeof(CustomGetter) < 1ull << (64 - Slot::customGetterPlaceShift));
    CustomGetter* customGetters { nullptr };
    static constexpr ptrdiff_t offsetOfCustomGetters() { return OBJECT_OFFSETOF(Instance, customGetters); }
    static unsigned indexOfCustomGetter(uint32_t structureID, UniquedStringImpl* uid) { return ((structureID >> 4) ^ static_cast<uint32_t>(std::bit_cast<uintptr_t>(uid) >> 4)) % numberOfCustomGetters; }
    static constexpr unsigned placesForCustomGetter = 4;
    const CustomGetter* customGetterFor(uint32_t structureID, UniquedStringImpl* uid) const
    {
        if (!customGetters)
            return nullptr;
        unsigned first = indexOfCustomGetter(structureID, uid);
        for (unsigned i = 0; i < placesForCustomGetter; ++i) {
            auto& record = customGetters[(first + i) % numberOfCustomGetters];
            if (record.structureID == structureID && record.uid == uid)
                return &record;
        }
        return &customGetters[first];
    }
    CustomGetter& ensureCustomGetterFor(uint32_t structureID, UniquedStringImpl* uid)
    {
        if (!customGetters) [[unlikely]]
            customGetters = static_cast<CustomGetter*>(fastZeroedMalloc(numberOfCustomGetters * sizeof(CustomGetter)));
        unsigned first = indexOfCustomGetter(structureID, uid);
        CustomGetter* empty = nullptr;
        for (unsigned i = 0; i < placesForCustomGetter; ++i) {
            auto& record = customGetters[(first + i) % numberOfCustomGetters];
            if (record.structureID == structureID && record.uid == uid)
                return record;
            if (!empty && !record.serial)
                empty = &record;
        }
        return empty ? *empty : customGetters[first];
    }
    struct PropertyRun {
        uint32_t structureID;
        uint32_t newStructureID;
        const Slot* site;
        uint32_t count;
        uint16_t epoch;
        uint16_t first;
    };
    static_assert(sizeof(PropertyRun) == 24);
    static constexpr unsigned numberOfPropertyRuns = 256;
    PropertyRun* propertyRuns { nullptr };
    static constexpr ptrdiff_t offsetOfPropertyRuns() { return OBJECT_OFFSETOF(Instance, propertyRuns); }
    static unsigned indexOfPropertyRun(uint32_t structureID, const Slot* site) { return ((structureID >> 4) * 9 + static_cast<uint32_t>(std::bit_cast<uintptr_t>(site) >> 4)) % numberOfPropertyRuns; }
    struct InheritedSetter {
        uint32_t structureID;
        uint16_t epoch;
        uint16_t offset;
        UniquedStringImpl* uid;
        JSObject* holder;
        void* nativeSetter;
    };
    static_assert(sizeof(InheritedSetter) == 32);
    static constexpr unsigned numberOfInheritedSetters = 128;
    static_assert(hasOneBitSet(numberOfInheritedSetters));
    InheritedSetter inheritedSetters[numberOfInheritedSetters] { };
    InheritedSetter& inheritedSetterFor(uint32_t structureID, UniquedStringImpl* uid)
    {
        unsigned index = ((structureID >> 4) ^ static_cast<uint32_t>(std::bit_cast<uintptr_t>(uid) >> 4)) % numberOfInheritedSetters;
        InheritedSetter& first = inheritedSetters[index];
        InheritedSetter& second = inheritedSetters[index ^ 1];
        if (second.structureID == structureID && second.uid == uid)
            return second;
        if (first.structureID && (first.structureID != structureID || first.uid != uid) && !second.structureID)
            return second;
        return first;
    }
    static constexpr ptrdiff_t offsetOfInheritedSetters() { return OBJECT_OFFSETOF(Instance, inheritedSetters); }
    uint32_t hasNotedNativeSetter { 0 };
    static constexpr ptrdiff_t offsetOfHasNotedNativeSetter() { return OBJECT_OFFSETOF(Instance, hasNotedNativeSetter); }
    struct CaseConversion {
        JSCell* source;
        JSCell* result;
    };
    static_assert(sizeof(CaseConversion) == 16);
    static constexpr unsigned numberOfCaseConversions = 64;
    static_assert(hasOneBitSet(numberOfCaseConversions));
    CaseConversion caseConversions[2][numberOfCaseConversions] { };
    bool hasCaseConversions { false };
    static constexpr ptrdiff_t offsetOfCaseConversions(bool isUpper) { return OBJECT_OFFSETOF(Instance, caseConversions) + isUpper * numberOfCaseConversions * sizeof(CaseConversion); }
    void noteCaseConversion(JSCell* source, JSCell* result, bool isUpper)
    {
        caseConversions[isUpper][(std::bit_cast<uintptr_t>(source) / sizeof(CaseConversion)) % numberOfCaseConversions] = { source, result };
        hasCaseConversions = true;
    }
    // Accesses by a key that is a cell, of all sites. By the structure of the object and the cell of the key: where the property is in the object. Slot::isGetter: the pair is not for the table, the general stub is asked.
    static constexpr unsigned numberOfKeyedReads = 2048;
    static_assert(hasOneBitSet(numberOfKeyedReads));
    static constexpr uint32_t keyMayNotBeReplaced = 1u << Slot::offsetBits;
    static_assert(!(keyMayNotBeReplaced & (Slot::offsetMask | Slot::flagsMask)));
    static constexpr unsigned keyedStructureShift = 6;
    static_assert(sizeof(Structure) >= 1 << keyedStructureShift);
    static constexpr unsigned keyedKeyShift = 4;
    // With Slot::isGetter | Slot::pointerIsCell: the object has no such property; while the megamorphic cache has the epoch, it is found in the holder, where the Slot says, or, without a holder, nowhere.
    struct KeyedRead {
        Slot slot;
        JSCell* holder;
        uint32_t epoch;
        uint32_t unused;
    };
    static_assert(sizeof(KeyedRead) == 32);
    static constexpr uint32_t keyedStructureFactor = 0x9e3779b1;
    static constexpr unsigned keyedProductShift = 16;
    uint32_t keyedTableRandomState { 1 };
    const JSCell* keyGivenItsAtomLast { nullptr };
    static constexpr unsigned placesOfKeyedPair = 4;
    alignas(16) std::array<uint8_t, 32> keyedScratchEntry { };
    template<typename Entry>
    ALWAYS_INLINE Entry& keyedEntryFor(Entry* table, unsigned numberOfEntries, StructureID structureID, const JSCell* key)
    {
        static_assert(sizeof(Entry) <= sizeof(keyedScratchEntry));
        Entry* place = placeInKeyedTable(table, numberOfEntries, structureID, key);
        return place ? *place : *std::bit_cast<Entry*>(keyedScratchEntry.data());
    }
    static constexpr size_t bytesInGroupOfKeyedEntries = 256;
    void didHandOutKeyedEntry(const void*);
    template<typename Entry>
    ALWAYS_INLINE Entry* placeInKeyedTable(Entry* table, unsigned numberOfEntries, StructureID structureID, const JSCell* key)
    {
        Entry* place = findPlaceInKeyedTable(table, numberOfEntries, structureID, key);
        if (place)
            didHandOutKeyedEntry(place);
        return place;
    }
    template<typename Entry>
    ALWAYS_INLINE Entry* findPlaceInKeyedTable(Entry* table, unsigned numberOfEntries, StructureID structureID, const JSCell* key)
    {
        constexpr unsigned shift = std::countr_zero(sizeof(Entry));
        static_assert(hasOneBitSet(sizeof(Entry)) && shift >= keyedKeyShift && shift <= keyedStructureShift);
        uint32_t offset = (((structureID.bits() * keyedStructureFactor) >> (keyedProductShift - shift)) + (static_cast<uint32_t>(std::bit_cast<uintptr_t>(key)) << (shift - keyedKeyShift))) & ((numberOfEntries - 1) << shift);
        auto at = [&](unsigned place) -> Entry& {
            return *std::bit_cast<Entry*>(std::bit_cast<uint8_t*>(table) + (offset ^ static_cast<uint32_t>(place * sizeof(Entry))));
        };
        auto slotAt = [&](unsigned place) -> Slot& {
            return *std::bit_cast<Slot*>(&at(place));
        };
        auto isOfPair = [&](const Slot& slot) {
            return slot.structureID == structureID && slot.pointer == key;
        };
        if (!slotAt(0).structureID || isOfPair(slotAt(0)))
            return &at(0);
        keyedTableRandomState = keyedTableRandomState * 1664525u + 1013904223u;
        if (keyedTableRandomState >> 30)
            return nullptr;
        Entry* firstFree = nullptr;
        for (unsigned place = 1; place < placesOfKeyedPair; ++place) {
            if (isOfPair(slotAt(place)))
                return &at(place);
            if (!firstFree && !slotAt(place).structureID)
                firstFree = &at(place);
        }
        if (firstFree)
            return firstFree;
        if (keyedTableRandomState >> 29)
            return nullptr;
        static_assert(placesOfKeyedPair == 4);
        return &at(keyedTableRandomState >> 27 & 3);
    }
    KeyedRead* placeForKeyedRead(StructureID structureID, const JSCell* key) { return placeInKeyedTable(keyedReads, numberOfKeyedReads, structureID, key); }
    KeyedRead& keyedReadFor(StructureID structureID, const JSCell* key) { return keyedEntryFor(keyedReads, numberOfKeyedReads, structureID, key); }
    static constexpr ptrdiff_t offsetOfKeyedReads() { return OBJECT_OFFSETOF(Instance, keyedReads); }
    // `key in object`. In Slot::offset: the epoch of the megamorphic cache under which the answer holds, and the answer. Slot::isGetter: the pair is not for the table.
    static constexpr unsigned numberOfKeyedHas = 1024;
    static_assert(hasOneBitSet(numberOfKeyedHas));
    static constexpr unsigned keyedHasAnswerShift = 16;
    Slot& keyedHasFor(StructureID structureID, const JSCell* key) { return keyedEntryFor(keyedHas, numberOfKeyedHas, structureID, key); }
    static constexpr ptrdiff_t offsetOfKeyedHas() { return OBJECT_OFFSETOF(Instance, keyedHas); }
    static constexpr unsigned numberOfPlacesOfNoRow = sizeof(Slot) / sizeof(uint32_t);
    uint32_t& placeOfRow(uint32_t number) { return reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(this) + displacementOfRows())[numberOfPlacesOfNoRow + number]; }
    Slot& rowAtPlace(uint32_t place) { return *reinterpret_cast<Slot*>(reinterpret_cast<char*>(this) + displacementOfRows() + place); }
    Slot& row(uint32_t number) { return rowAtPlace(placeOfRow(number)); }
    void emptyRow(uint32_t number);
    static constexpr uint32_t typeInfoBlobOfRows = TypeInfoBlob::typeInfoBlob(NonArray, FinalObjectType, TypeInfo(FinalObjectType, JSFinalObject::StructureFlags).inlineTypeFlags());
    void fillRow(uint32_t number, Structure*, uint32_t placesToClear, LocalAllocator*);
    void didFillGlobalPropertyCache();
    // In keyedReads: no object of the structure has an own property under the key. (With Slot::isGetter, so that a read, which goes on along the prototype chain, takes it for what is not for the table.)
    static constexpr uint32_t keyIsAbsent = Slot::isGetter | Slot::pointerIsNotCell;
    // In keyedReads: the property is a field of an object of a compiled layout. `holder` is no cell: what Slot::pointer is with Slot::compiledFieldFlags.
    static constexpr uint32_t keyIsCompiledField = Slot::isGetter | Slot::isIndirect;
    static constexpr uint32_t epochIsOfDictionary = 1u << 16;
    static constexpr uint32_t epochIsValidated = 1u << 17;
    void validateKeyedTables();
    static constexpr uint32_t keyIsLookedUp = Slot::isGetter | Slot::isIndirect | Slot::pointerIsNotCell;
    static constexpr uint32_t keyIsAbsentForGood = Slot::isGetter | Slot::pointerIsNotCell | Slot::pointerIsCell;
    static constexpr uint32_t keyedHasIsLookedUp = Slot::isGetter | Slot::pointerIsNotCell;
    static constexpr size_t cannotLookUp = 2;
    // The definition of a property that the object lacks: where it goes, and the structure the object has afterwards.
    struct KeyedAddition {
        Slot slot;
        StructureID newStructureID;
        // Of a store, which has asked the prototype chain: MegamorphicCache::epoch() at that time. Of the definition of a private name: MegamorphicCache::invalidEpoch.
        uint32_t epoch;
        // 0: the object keeps the storage out of line it has. Otherwise it gets new storage of this capacity: the first, or twice what it had.
        uint32_t capacityOfNewStorage;
        uint32_t numberOfValuesOfHolder;
    };
    static_assert(sizeof(KeyedAddition) == 32);
    static constexpr unsigned numberOfKeyedAdditions = 1024;
    static_assert(hasOneBitSet(numberOfKeyedAdditions));
    KeyedAddition& keyedAdditionFor(StructureID structureID, const JSCell* key) { return keyedEntryFor(keyedAdditions, numberOfKeyedAdditions, structureID, key); }
    KeyedAddition* placeForKeyedAddition(StructureID structureID, const JSCell* key) { return placeInKeyedTable(keyedAdditions, numberOfKeyedAdditions, structureID, key); }
    static constexpr size_t sizeOfKeyedTables = numberOfKeyedReads * sizeof(KeyedRead) + numberOfKeyedAdditions * sizeof(KeyedAddition) + numberOfKeyedHas * sizeof(Slot);
    static_assert(!(sizeOfKeyedTables % (16 * KB)));
    static constexpr ptrdiff_t offsetOfKeyedAdditions() { return OBJECT_OFFSETOF(Instance, keyedAdditions); }
    // A field of the objects of a compiled layout, for every site that has no slot to remember it in: by the shape word of the object and the number of the name among the identifiers of the program. `access`: what
    // Slot::pointer is for a read of the field (Slot::compiledFieldFlags), or for a store to it (Slot::FieldWrite). A shape is for good, so an entry is never wrong. No other OBJECT has a negative number where the shape word is (a cell that is no object may: JSBigInt::m_hash): the stub asks the cell's type first.
    struct NamedField {
        uint64_t shapeWordAndName;
        uint64_t access;
    };
    static constexpr unsigned numberOfNamedFields = 2048;
    // The fields of one shape stand side by side as far as the numbers of their names do.
    static constexpr unsigned namedFieldShapeShift = 5;
    static_assert(hasOneBitSet(numberOfNamedFields) && sizeof(NamedField) == 16);
    NamedField* namedFieldReads { nullptr };
    NamedField* namedFieldWrites { nullptr };
    EncodedJSValue thrownValue { 0 };
    void* typedKnownStoreTransitions { nullptr };
    void* typedExtraPropertiesOfObjects { nullptr };
    void* typedPlacesOfExtraProperties { nullptr };
    static constexpr ptrdiff_t offsetOfNamedFieldReads() { return OBJECT_OFFSETOF(Instance, namedFieldReads); }
    static constexpr ptrdiff_t offsetOfNamedFieldWrites() { return OBJECT_OFFSETOF(Instance, namedFieldWrites); }
    static constexpr ptrdiff_t offsetOfTypedKnownStoreTransitions() { return OBJECT_OFFSETOF(Instance, typedKnownStoreTransitions); }
    static constexpr ptrdiff_t offsetOfTypedExtraPropertiesOfObjects() { return OBJECT_OFFSETOF(Instance, typedExtraPropertiesOfObjects); }
    static constexpr ptrdiff_t offsetOfTypedPlacesOfExtraProperties() { return OBJECT_OFFSETOF(Instance, typedPlacesOfExtraProperties); }
    static void noteNamedField(NamedField* table, uint32_t shapeWord, uint32_t name, uint64_t access)
    {
        table[((shapeWord << namedFieldShapeShift) + name) % numberOfNamedFields] = { shapeWord | static_cast<uint64_t>(name) << 32, access };
    }
    struct CachedAddressInfo {
        static constexpr uint32_t siteNotResolved = std::numeric_limits<uint32_t>::max();
        static constexpr uint32_t hasNoSite = siteNotResolved - 1;
        const void* address;
        uint32_t function;
        uint32_t site;
    };
    struct PlaceInTrace {
        enum Kind : uint32_t { IsNotPlain, IsShown, IsHidden, IsInInlinedBody };
        uint32_t typedFunction { 0 };
        uint32_t callSiteBits : 30 { 0 };
        uint32_t kind : 2 { IsNotPlain };
        static constexpr unsigned numberOfBitsWithConstructedCallee = 27;
        static constexpr uint32_t hasConstructedCallee = 1u << 29;
        bool calleeIsConstructed() const { return (kind == IsNotPlain || kind == IsInInlinedBody) && callSiteBits & hasConstructedCallee; }
        PlaceInTrace withConstructedCallee() const
        {
            PlaceInTrace result;
            result.typedFunction = typedFunction;
            result.callSiteBits = callSiteBits | kind << numberOfBitsWithConstructedCallee | hasConstructedCallee;
            result.kind = kind == IsInInlinedBody ? IsInInlinedBody : IsNotPlain;
            return result;
        }
        PlaceInTrace withoutConstructedCallee() const
        {
            PlaceInTrace result;
            result.typedFunction = typedFunction;
            result.callSiteBits = callSiteBits & ((1u << numberOfBitsWithConstructedCallee) - 1);
            result.kind = callSiteBits >> numberOfBitsWithConstructedCallee & 3;
            return result;
        }
        PlaceInTrace ofSite() const { return calleeIsConstructed() ? withoutConstructedCallee() : *this; }
    };
    struct PlaceInUnwinding {
        enum State : uint8_t { IsNotKnown, IsLeftByWhateverIsThrown, IsNotLeft, HasHandler, HasHandlerThatTakesException, HasHandlerThatIgnoresValue, RejectsPromise };
        union {
            uint32_t savedRegisters { 0 };
            uint32_t offsetOfHandlerInImage;
        };
        uint16_t savedRegistersStart { 0 };
        State state { IsNotKnown };
        bool siteIsInShownFunction { false };
    };
    static_assert(sizeof(PlaceInTrace) == 8 && sizeof(PlaceInUnwinding) == 8);
    static constexpr unsigned numberOfCachedAddressInfos = 512;
    CachedAddressInfo cachedAddressInfos[numberOfCachedAddressInfos] { };
    CachedAddressInfo& cachedAddressInfo(const void* address) { return cachedAddressInfos[(std::bit_cast<uintptr_t>(address) >> 2) % numberOfCachedAddressInfos]; }
    CachedAddressInfo* cachedReturnAddresses { nullptr };
    JS_EXPORT_PRIVATE CachedAddressInfo* allocateCachedReturnAddresses();
    CachedAddressInfo* ensureCachedReturnAddresses() { return cachedReturnAddresses ? cachedReturnAddresses : allocateCachedReturnAddresses(); }
    static size_t firstPlaceOfReturnAddress(const void* address) { return (std::bit_cast<uintptr_t>(address) >> 2) % numberOfCachedAddressInfos; }
    static size_t secondPlaceOfReturnAddress(const void* address)
    {
        static_assert(numberOfCachedAddressInfos == 1 << 9);
        return std::bit_cast<uintptr_t>(address) * 0x9e3779b97f4a7c15ull >> (64 - 9);
    }
    const CachedAddressInfo* findCachedReturnAddress(const void* address) const
    {
        const CachedAddressInfo* all = cachedReturnAddresses;
        if (!all) [[unlikely]]
            return nullptr;
        if (all[firstPlaceOfReturnAddress(address)].address == address) [[likely]]
            return &all[firstPlaceOfReturnAddress(address)];
        return all[secondPlaceOfReturnAddress(address)].address == address ? &all[secondPlaceOfReturnAddress(address)] : nullptr;
    }
    PlaceInTrace* placesInTraces() { return reinterpret_cast<PlaceInTrace*>(ensureCachedReturnAddresses() + numberOfCachedAddressInfos); }
    PlaceInUnwinding* placesInUnwinding() { return reinterpret_cast<PlaceInUnwinding*>(placesInTraces() + numberOfCachedAddressInfos); }
    JS_EXPORT_PRIVATE const PlaceInUnwinding& findPlaceInUnwinding(const CachedAddressInfo&);
    JS_EXPORT_PRIVATE bool nearestHandlerIgnoresValue(VM&, CallFrame*, void* returnPC);
    JS_EXPORT_PRIVATE const CachedAddressInfo* resolveReturnAddress(const void*);
    uint32_t operationSamplingState { 0 };
    uint32_t moduleEvaluationDepth { 0 };
    bool evictsSecondPlaceOfReturnAddress { false };
    bool thrownValueIsAtSiteOfItsItem { false };
    const CallSiteOverride* callSiteOverrides { nullptr };
    static_assert(!(sizeOfKeyedTables % bytesInGroupOfKeyedEntries));
    WTF::BitSet<sizeOfKeyedTables / bytesInGroupOfKeyedEntries> keyedGroupsHandedOutInThisEntry;
    WTF::BitSet<sizeOfKeyedTables / bytesInGroupOfKeyedEntries> keyedGroupsHandedOutEarlier;
    unsigned entryCountAtLastKeyedHandOut { 0 };
    bool hasNewOwnPropertyAnswers { false };
    bool hasNewOwnPropertyPlaces { false };
    static constexpr ptrdiff_t offsetOfTypedStatics() { return OBJECT_OFFSETOF(Instance, typedStatics); }
    static constexpr size_t roomForEnvironments = 1 * MB;
    static constexpr size_t roomForLoopPollPage = 16 * KB;
    static constexpr ptrdiff_t offsetOfLoopPollPage() { return -static_cast<ptrdiff_t>(roomForEnvironments); }
    static constexpr size_t granuleBelowInstance = 64 * KB;
    static constexpr size_t roomForPlacesOfRows = 1 * MB;
    static constexpr size_t maxNumberOfRows = roomForPlacesOfRows / sizeof(uint32_t) - numberOfPlacesOfNoRow;
    static constexpr size_t maxNumberOfFilledRows = 1 << 16;
    static constexpr size_t roomForRows = roomForPlacesOfRows + maxNumberOfFilledRows * sizeof(Slot);
    static_assert(!(roomForRows % granuleBelowInstance));
    static constexpr ptrdiff_t displacementOfRows() { return -static_cast<ptrdiff_t>(roomForEnvironments + roomForRows); }
    static constexpr size_t roundedUpToGranule(size_t size) { return (size + granuleBelowInstance - 1) & ~(granuleBelowInstance - 1); }
    static constexpr ptrdiff_t displacementOfTypedStatics(size_t sizeOfStatics) { return -static_cast<ptrdiff_t>(roomForEnvironments + roomForRows + roundedUpToGranule(sizeOfStatics)); }
    static constexpr ptrdiff_t displacementOfTypedCards(size_t sizeOfStatics) { return displacementOfTypedStatics(sizeOfStatics) - static_cast<ptrdiff_t>(roundedUpToGranule(sizeOfStatics) >> typedCardShift); }
    static constexpr size_t roomBelowInstance(size_t sizeOfStatics) { return roomForEnvironments + roomForRows + roundedUpToGranule(sizeOfStatics) + roundedUpToGranule(roundedUpToGranule(sizeOfStatics) >> typedCardShift); }
    static constexpr ptrdiff_t offsetOfTypedCards() { return OBJECT_OFFSETOF(Instance, typedCards); }
    static constexpr ptrdiff_t offsetOfModuleEvaluationDepth() { return OBJECT_OFFSETOF(Instance, moduleEvaluationDepth); }
    static constexpr ptrdiff_t offsetOfThrownValue() { return OBJECT_OFFSETOF(Instance, thrownValue); }
    static constexpr ptrdiff_t offsetOfTypedAllocationRows() { return OBJECT_OFFSETOF(Instance, typedAllocationRows); }
    static constexpr ptrdiff_t offsetOfTypedAlteredClassObjects() { return OBJECT_OFFSETOF(Instance, typedAlteredClassObjects); }
    static constexpr ptrdiff_t offsetOfTypedShapeSets() { return OBJECT_OFFSETOF(Instance, typedShapeSets); }
    static constexpr ptrdiff_t offsetOfTypedCastTransitions() { return OBJECT_OFFSETOF(Instance, typedCastTransitions); }
    static constexpr ptrdiff_t offsetOfTypedClosureStructureID(unsigned which = 0) { return which ? static_cast<ptrdiff_t>(OBJECT_OFFSETOF(Instance, typedClosureStructureIDs) + (which - 1) * sizeof(uint32_t)) : offsetBelow(OBJECT_OFFSETOF(FieldsBelow, typedClosureStructureID)); }
    static constexpr ptrdiff_t offsetOfTypedSlimClosureLengthGetter() { return OBJECT_OFFSETOF(Instance, typedSlimClosureLengthGetter); }
    static constexpr ptrdiff_t offsetOfTypedSlimClosureNameGetter() { return OBJECT_OFFSETOF(Instance, typedSlimClosureNameGetter); }
    static constexpr ptrdiff_t offsetOfTypedClosureStructureIDPair() { return OBJECT_OFFSETOF(Instance, typedClosureStructureIDPair); }
    static constexpr ptrdiff_t offsetOfTypedSlimClosureHeader(unsigned kind) { return OBJECT_OFFSETOF(Instance, typedSlimClosureHeaders) + kind * sizeof(uint64_t); }
    static constexpr ptrdiff_t offsetOfTypedSlimClosureAllocator(unsigned numberOfCaptures) { return OBJECT_OFFSETOF(Instance, typedSlimClosureAllocators) + (numberOfCaptures + 1) / 2 * sizeof(LocalAllocator*); }
    static constexpr ptrdiff_t offsetOfTypedAlteredLiterals() { return OBJECT_OFFSETOF(Instance, typedAlteredLiterals); }
    static constexpr ptrdiff_t offsetOfTypedStrings() { return offsetBelow(OBJECT_OFFSETOF(FieldsBelow, typedStrings)); }
    static constexpr ptrdiff_t offsetOfReturnAddressOfSavedFrames() { return OBJECT_OFFSETOF(Instance, returnAddressOfSavedFrames); }
    static constexpr ptrdiff_t offsetOfSpaceOfSavedFrames() { return OBJECT_OFFSETOF(Instance, spaceOfSavedFrames); }
    static constexpr ptrdiff_t offsetOfHeaderOfSavedFrames() { return OBJECT_OFFSETOF(Instance, headerOfSavedFrames); }
    static constexpr ptrdiff_t offsetOfTypedData() { return OBJECT_OFFSETOF(Instance, typedData); }
    static constexpr ptrdiff_t offsetOfTypedWideStrings() { return OBJECT_OFFSETOF(Instance, typedWideStrings); }
    static constexpr ptrdiff_t offsetOfTypedLiteralStrings() { return OBJECT_OFFSETOF(Instance, typedLiteralStrings); }
    static constexpr ptrdiff_t offsetOfTypedLiteralStringCache() { return OBJECT_OFFSETOF(Instance, typedLiteralStringCache); }
    static constexpr ptrdiff_t offsetOfTyped() { return OBJECT_OFFSETOF(Instance, typed); }
    static constexpr ptrdiff_t offsetOfTypedFunctions() { return OBJECT_OFFSETOF(Instance, typedFunctions); }
    static constexpr ptrdiff_t offsetOfTypedExecutable(unsigned kind) { return OBJECT_OFFSETOF(Instance, typedExecutables) + kind * sizeof(JSCell*); }
    static constexpr ptrdiff_t offsetOfTypedClosures() { return OBJECT_OFFSETOF(Instance, typedClosures); }
    static constexpr ptrdiff_t offsetOfTypedBuiltins() { return offsetBelow(OBJECT_OFFSETOF(FieldsBelow, typedBuiltins)); }
    BiasedDataPointer sharedDataOfLinkedFunctions;
    BiasedDataPointer ownData[maxNumberOfOwnData];
    static constexpr unsigned numberOfTypedStringsWithWord = 4096;
    uint32_t wordsOfTypedStrings[numberOfTypedStringsWithWord];
    static constexpr ptrdiff_t offsetOfWordOfTypedString(unsigned place) { return OBJECT_OFFSETOF(Instance, wordsOfTypedStrings) + place * sizeof(uint32_t); }
    uint16_t missCounters[numberOfMissCounters];
    uint16_t states[0];
};
// Within reach of a load with an unsigned scaled offset on ARM64, 32-bit loads too, or of one with a signed offset.
static_assert(sizeof(Instance::FieldsBelow) <= 256);
static_assert(Instance::offsetOfTypedData() < 4096 * sizeof(uint32_t));

JS_EXPORT_PRIVATE const RegisterAtOffsetList* calleeSaveRegistersOf(const ImageFunction&);

struct Data {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(Data);

    static Data* create(Instance&, ScriptExecutable*, UnlinkedCodeBlock*, JITCode&, CodeBlock* = nullptr);
    static Data* create(Instance&, const ImageFunction&);
    static Data* create(Instance&, ScriptExecutable*, UnlinkedCodeBlock*, const ImageFunction&, JITCode*, CodeBlock*);
    const ImageFunction* imageFunction() const;
    static void destroy(Data*);
    void noteFilled();
    template<typename Visitor> void visit(Visitor&);

    JS_EXPORT_PRIVATE CodeBlock* ensureCodeBlock();
    FunctionRef function() const;

    void finalizeUnconditionally(VM&);
    void finalizeSlot(VM&, Slot&);
    void retryAbandonedSlots();

    static constexpr ptrdiff_t offsetOfSites() { return OBJECT_OFFSETOF(Data, sites); }
    static constexpr ptrdiff_t offsetOfSlots() { return OBJECT_OFFSETOF(Data, slots); }
    static constexpr ptrdiff_t offsetOfSlotEpoch() { return OBJECT_OFFSETOF(Data, slotEpoch); }
    static constexpr ptrdiff_t offsetOfHasBeenFilledSinceLastCollection() { return OBJECT_OFFSETOF(Data, hasBeenFilledSinceLastCollection); }

    struct Linkage {
        CodeBlock* codeBlock;
        ScriptExecutable* executable;
        UnlinkedCodeBlock* unlinkedCodeBlock;
        JITCode* code;
    };
    static constexpr size_t allocationSize(unsigned numSlots, bool hasLinkage) { return OBJECT_OFFSETOF(Data, slots) + numSlots * sizeof(Slot) + (hasLinkage ? sizeof(Linkage) : 0); }
    Linkage* linkage() { return hasLinkage ? std::bit_cast<Linkage*>(slots + numSlots) : nullptr; }
    const Linkage* linkage() const { return const_cast<Data*>(this)->linkage(); }
    CodeBlock* codeBlock() const { return hasLinkage ? linkage()->codeBlock : nullptr; }
    ScriptExecutable* executable() const { return hasLinkage ? linkage()->executable : nullptr; }
    UnlinkedCodeBlock* unlinkedCodeBlock() const { return hasLinkage ? linkage()->unlinkedCodeBlock : nullptr; }
    JITCode* code() const { return hasLinkage ? linkage()->code : nullptr; }

    Instance* instance;
    const Site* sites;
    SlotWatchpointMap* watchpoints;
    unsigned numSlots;
    bool hasBeenFilledSinceLastCollection;
    bool hasSiteConstants;
    bool hasSitesInMegamorphicCache;
    bool hasLinkage;
    unsigned indexInAllList;
    unsigned indexInFilledList;
    uint64_t slotEpoch;
    Slot slots[0];
};
static_assert(BiasedDataPointer::bias == Data::offsetOfSlots() + 128 && !(BiasedDataPointer::bias % sizeof(Slot)));

class PlaceTable {
    WTF_MAKE_TZONE_ALLOCATED(PlaceTable);
    WTF_MAKE_NONCOPYABLE(PlaceTable);
public:
    static constexpr uint32_t maxName = (1u << 15) - 1;
    static constexpr uint32_t isElsewhere = 1u << 15;
    static constexpr uint32_t numberOfBases = 1u << 21;
    static constexpr uint32_t numberOfColumns = 1u << 11;
    static constexpr uint32_t columnOf(uint32_t name) { return name & (numberOfColumns - 1); }
    static constexpr uint32_t multiplierOfBusyNames = 1779;
    static constexpr uint32_t inverseOfMultiplierOfBusyNames = 571;
    static_assert(multiplierOfBusyNames * inverseOfMultiplierOfBusyNames % numberOfColumns == 1);
    static constexpr uint32_t nameOfIdentifierNumbered(uint32_t number) { return number < numberOfColumns ? number * multiplierOfBusyNames % numberOfColumns : number; }
    static constexpr uint32_t numberOfIdentifierNamed(uint32_t name) { return name < numberOfColumns ? name * inverseOfMultiplierOfBusyNames % numberOfColumns : name; }
    static constexpr uint32_t columnInOverflowOf(uint32_t name) { return (name ^ name >> 4) & (numberOfColumns - 1); }
    static constexpr int32_t offsetOfFirstColumn = 0;
    static constexpr uint32_t entriesPerPlane = numberOfBases + numberOfColumns + 1;
    struct Cell {
        uint32_t placeToReadOrPayload;
        uint8_t code;
        uint8_t kindOrPlaceToStore;
        uint16_t name;
    };
    static_assert(sizeof(Cell) == sizeof(uint64_t));
    static constexpr int32_t offsetOfOverflow = static_cast<int32_t>(entriesPerPlane * sizeof(Cell));
    static constexpr int32_t offsetOfPlaceToReadInEntry = OBJECT_OFFSETOF(Cell, placeToReadOrPayload);
    static constexpr int32_t offsetOfPayloads = OBJECT_OFFSETOF(Cell, placeToReadOrPayload);
    enum class Kind : uint8_t {
        Absent,
        AbsentAndDefinitionAddsInObject,
        AbsentAndDefinitionAddsOutOfLine,
        AbsentAndDefinitionAddsInNewStorage,
        AbsentAndStoreAddsInObject,
        AbsentAndStoreAddsOutOfLine,
        AbsentAndStoreAddsInNewStorage,
        DefinitionAddsInObject,
        DefinitionAddsOutOfLine,
        DefinitionAddsInNewStorage,
        StoreAddsInObject,
        StoreAddsOutOfLine,
        StoreAddsInNewStorage,
        OutOfLine,
        OutOfLineAndStoredTo,
        OwnAccessorInObject,
        Accessor,
        AccessorCalledByStores,
        InheritedInObject,
        InheritedOutOfLine,
        FieldCell32,
        FieldInt32,
        FieldBoxed,
        FieldDouble,
        FieldBoolean,
        FieldOptionalBoolean,
        ExtraAccessorInLine,
        KnownCustomGetter,
        ArrayLength,
        LazyClosure,
        ExportOfNamespace,
        Unknown,
    };
    static constexpr unsigned numberOfKinds = static_cast<unsigned>(Kind::Unknown) + 1;
    enum class WhereAdded : uint8_t { InObject, OutOfLine, InNewStorage };
    static constexpr Kind kindThatAdds(bool isKnownToBeAbsent, bool storesAddToo, WhereAdded where) { return static_cast<Kind>(1 + (isKnownToBeAbsent ? 0 : 6) + (storesAddToo ? 3 : 0) + static_cast<unsigned>(where)); }
    static constexpr bool saysAbsent(Kind kind) { return kind <= Kind::AbsentAndStoreAddsInNewStorage; }
    static constexpr bool adds(Kind kind) { return kind >= Kind::AbsentAndDefinitionAddsInObject && kind <= Kind::StoreAddsInNewStorage; }
    static constexpr bool storesAdd(Kind kind) { return adds(kind) && (static_cast<unsigned>(kind) - 1) / 3 % 2; }
    static constexpr WhereAdded whereAdded(Kind kind) { return static_cast<WhereAdded>((static_cast<unsigned>(kind) - 1) % 3); }
    static constexpr int32_t offsetOfCodeInEntry = OBJECT_OFFSETOF(Cell, code);
    static constexpr int32_t offsetOfPlaceToStoreInEntry = OBJECT_OFFSETOF(Cell, kindOrPlaceToStore);
    static constexpr int32_t offsetOfKindInEntry = OBJECT_OFFSETOF(Cell, kindOrPlaceToStore);
    static constexpr int32_t offsetOfNameInEntry = OBJECT_OFFSETOF(Cell, name);
    static constexpr unsigned bitsOfKind = 5;
    static_assert(numberOfKinds <= 1u << bitsOfKind);
    static constexpr uint8_t valueIsInStorage = 1u << bitsOfKind;
    static constexpr uint8_t holderIsInPayload = 2u << bitsOfKind;
    static constexpr uint8_t storeNeedsNoHandler = 4u << bitsOfKind;
    static constexpr uint8_t storesAddToo = 0x80;
    static constexpr uint8_t flagsBesideKind(Kind kind)
    {
#if CPU(ARM64)
        switch (kind) {
        case Kind::OutOfLine:
            return valueIsInStorage;
        case Kind::OutOfLineAndStoredTo:
            return valueIsInStorage | storeNeedsNoHandler;
        case Kind::InheritedInObject:
            return holderIsInPayload;
        case Kind::InheritedOutOfLine:
            return holderIsInPayload | valueIsInStorage;
        default:
            return adds(kind) && whereAdded(kind) == WhereAdded::InObject ? storeNeedsNoHandler : 0;
        }
#else
        UNUSED_PARAM(kind);
        return 0;
#endif
    }
    static constexpr uint8_t flagBesideCode(Kind kind)
    {
#if CPU(ARM64)
        return adds(kind) && whereAdded(kind) == WhereAdded::InObject && storesAdd(kind) ? storesAddToo : 0;
#else
        UNUSED_PARAM(kind);
        return 0;
#endif
    }
    static constexpr int32_t maxPlaceOutOfLine = 125;
    static constexpr uint32_t codeOfPlaceOutOfLine(uint32_t place) { return static_cast<uint8_t>(-static_cast<int32_t>(place) - 2); }
    static constexpr uint32_t firstCodeOutOfLine = 0x80;
    static constexpr uint32_t needsFirstStorage = 2;
    static constexpr uint32_t growsStorage = 4;
    static constexpr uint32_t flagsOfStructureAfter = needsFirstStorage | growsStorage;

    enum class Access : uint8_t { Read, Store, Define };
    static constexpr uint32_t payloadIsSlots = 1;
    static constexpr uint32_t slotsPerPayload = 3;
    static constexpr uint32_t maxNumberOfSlots = 1u << 20;
    static constexpr int32_t offsetOfSlotBehindPayloadTimesEight(Access access) { return static_cast<int32_t>(static_cast<unsigned>(access) * sizeof(Slot)) - 8; }

    static PlaceTable& of(VM&);
    static void destroy(VM&);
    ~PlaceTable();

    const uint32_t* emptyRowOfPlaces() const { return &m_cells[1].placeToReadOrPayload; }
    Data* dataFor(Instance*);
    bool servesSeveralInstances() const { return m_servesSeveralInstances; }

    bool setOwnPlace(Structure*, uint32_t name, PropertyOffset, bool isStoredTo);
    bool setAbsent(Structure*, uint32_t name, UniquedStringImpl* uidIfItDependsOnPrototypes);
    bool setInherited(Structure*, uint32_t name, JSCell* holder, PropertyOffset, UniquedStringImpl* uidIfItDependsOnPrototypes);
    bool setInheritedInPlaceOfSlot(Structure*, uint32_t name, JSCell* holder, PropertyOffset);
    bool setExtraAccessorInPlaceOfSlot(Structure*, uint32_t name, StructureID ofExtraProperties, uint32_t offsetInBytes);
    bool setAccessor(Structure*, uint32_t name, JSCell* holderUnlessOwn, PropertyOffset, UniquedStringImpl* uidIfItDependsOnPrototypes, bool storesCallIt = false);
    bool setField(Structure*, uint32_t name, uint64_t howTheFieldIsRead);
    void setKnownCustomGetterIsInSlot(Structure*, uint32_t name);
    bool setArrayLength(Structure*, uint32_t name, bool holdsDoubles);
    bool setLazyClosure(Structure*, uint32_t name, PropertyOffset, Structure* after);
    bool setExportOfNamespace(Structure*, uint32_t name, uint32_t number);
    bool setAdded(Structure*, uint32_t name, Access, Structure* after, PropertyOffset, uint32_t flagsOfStructureAfter, UniquedStringImpl* uidIfItDependsOnPrototypes);
    Slot* slotIfAny(Structure*, uint32_t name, Access);
    void didChangePrototype(UniquedStringImpl* uidIfOnlyOneNameIsConcerned);
    void willMakeDeclaredMembers() { ++m_declaredMemberCreationDepth; }
    void didMakeDeclaredMembers() { --m_declaredMemberCreationDepth; }
    void forget(Structure*, uint32_t name);
    struct EntriesPointingAtPlace {
        uint32_t name { 0 };
        Vector<Structure*, 8> owners;
    };
    EntriesPointingAtPlace entriesPointingAt(UniquedStringImpl*, JSCell* holder, PropertyOffset) const;

    Slot* ensureSlot(Structure*, uint32_t name, Access);
    void releaseSlot(Structure*, uint32_t name, Access);
    bool isWatched(Slot*) const;

    void noteDeathOf(Structure*);
    void keepIfShapeHasDiedBefore(SlotVisitor&, StructureID);
    void keepShapesThatHaveDiedBefore(SlotVisitor&);
    void finalizeUnconditionally(VM&, bool isEdenCollection);
    void didCreate(Instance*);
    void willDestroy(Instance*);

private:
    PlaceTable();

    struct Bases {
        uint32_t first { 1 };
        uint32_t limit { 0 };
        uint32_t next { 1 };
        uint32_t end { 1 };
    };
    struct Row {
        Structure* owner { nullptr };
        Vector<uint16_t, 4> names;
    };
    static constexpr uint32_t markOfBase = 0xffff0000;

    static constexpr uint16_t isInOverflow = 1u << 15;
    static bool isInObject(uint32_t entry) { return entry & maxName && !(entry & isElsewhere); }
    static bool isInObject(const Cell& cell) { return static_cast<uint16_t>(cell.name - 1) < maxName; }
    uint32_t entryAt(uint32_t index) const
    {
        static_assert(offsetOfCodeInEntry == 4 && offsetOfKindInEntry == 5 && offsetOfNameInEntry == 6);
        uint32_t entry = std::rotl(static_cast<uint32_t>(std::bit_cast<uint64_t>(m_cells[index]) >> 32), 16);
#if CPU(ARM64)
        if (entry & isElsewhere) {
            entry &= (1u << (24 + bitsOfKind)) - 1;
            entry &= ~(static_cast<uint32_t>(flagBesideCode(kindOf(entry))) << 16);
        }
#endif
        return entry;
    }
    void setEntryAt(uint32_t index, uint32_t entry)
    {
        Cell& cell = m_cells[index];
        uint32_t low = isInObject(entry) ? codeOf(entry) : isInObject(cell) ? 0 : cell.placeToReadOrPayload;
#if CPU(ARM64)
        if (entry & isElsewhere)
            entry |= static_cast<uint32_t>(flagBesideCode(kindOf(entry))) << 16 | static_cast<uint32_t>(flagsBesideKind(kindOf(entry))) << 24;
#endif
        cell = std::bit_cast<Cell>(static_cast<uint64_t>(std::rotl(entry, 16)) << 32 | low);
    }
    uint32_t payload(uint32_t index) const { return isInObject(m_cells[index]) ? 0 : m_cells[index].placeToReadOrPayload; }
    void setPayload(uint32_t index, uint32_t payload)
    {
        ASSERT(!isInObject(m_cells[index]));
        m_cells[index].placeToReadOrPayload = payload;
    }
    Slot* slotsAt(uint32_t index) { return payload(index) & payloadIsSlots ? m_slots + (payload(index) >> 1) : nullptr; }
    static bool usesPayloadItself(Kind);
    static bool payloadIsStructure(Kind kind) { return adds(kind) || kind == Kind::ExtraAccessorInLine || kind == Kind::LazyClosure; }
    static bool usesPayloadItself(uint32_t entry) { return entry & isElsewhere && usesPayloadItself(kindOf(entry)); }
    static Kind kindOf(uint32_t entry) { return static_cast<Kind>(entry >> 24); }
    static uint32_t codeOf(uint32_t entry) { return entry >> 16 & 0xff; }
    static uint32_t entryElsewhere(uint32_t name, Kind kind, uint32_t code) { return name | isElsewhere | code << 16 | static_cast<uint32_t>(kind) << 24; }
    bool isElsewhereEntryOf(uint32_t entry, uint32_t name) const { return (entry & 0xffff) == (name | isElsewhere); }
    bool isEntryOf(uint32_t entry, uint32_t name) const { return (entry & maxName) == name && (entry & 0xffff); }
    uint32_t baseOf(Structure*) const;
    static uint32_t indexOf(uint32_t base, uint32_t name) { return base + 1 + columnOf(name); }
    static uint32_t indexOfListed(uint32_t base, uint16_t listed) { return listed & isInOverflow ? entriesPerPlane + base + 1 + columnInOverflowOf(listed & maxName) : indexOf(base, listed); }
    uint32_t findIndex(Structure*, uint32_t name) const;
    uint32_t findIndexOfSlots(Structure*, uint32_t name) const;
    uint32_t ensureIndexInOverflow(Structure*, uint32_t name);
    static size_t bitOf(uint32_t index) { return index; }
    Row& rowAt(uint32_t base) { return m_rows[payload(base)]; }
    uint32_t ensureIndex(Structure*, uint32_t name, bool mayBeInOverflow = false);
    uint32_t makeRow(Structure*, uint32_t name);
    uint32_t moveRow(uint32_t base, std::optional<uint16_t> listedToAdd);
    uint64_t freeCellsFrom(uint32_t index) const;
    uint32_t firstBaseThatFits(uint32_t from, std::span<const uint16_t> names) const;
    uint32_t findBase(std::span<const uint16_t> names);
    void occupy(uint32_t index, uint32_t entry);
    uint32_t vacate(uint32_t index);
    uint32_t takeNumberOfRowFor(Structure*);
    void releaseSlotsAt(uint32_t index);
    bool releaseSlotsUnlessStoresUseThem(uint32_t index);
    static uint32_t entryForOwnPlace(uint32_t name, PropertyOffset);
    bool setElsewhere(Structure*, uint32_t name, Kind, uint32_t code, uint32_t payload, UniquedStringImpl* uidIfItDependsOnPrototypes);
    void setDependsOnPrototypes(uint32_t index, uint32_t name, UniquedStringImpl*);
    uint32_t rowOfEntry(uint32_t index, uint32_t name) const;
    void appendToNameList(uint32_t row, uint32_t name);
    void freeNameList(uint32_t name);
    void clearNameLists();
    template<typename Functor> void forEachIndexInNameList(uint32_t name, const Functor& functor) const
    {
        for (uint32_t position = m_nameListHeads[name]; position; position = m_nameListLinks[position - 1].next) {
            Structure* owner = m_rows[m_nameListLinks[position - 1].row].owner;
            if (uint32_t index = owner ? findIndex(owner, name) : 0)
                functor(index);
        }
    }
    void forgetWhatDependsOnPrototypes(uint32_t index);
    void forgetDeadStructuresAfter(VM&, Vector<uint32_t>& indices);
    void release(uint32_t base);
    void clear();

    Cell* m_cells { nullptr };
    Bases m_bases;
    uint64_t* m_occupied { nullptr };
    Vector<Row> m_rows;
    Vector<uint32_t> m_freeRows;
    BitVector m_dependsOnPrototypes;
    Vector<Structure*> m_structuresWithRowsMadeSinceLastCollection;
    static constexpr unsigned bitsOfPlaceOfShapeThatDied = 11;
    static uint32_t wordOfShape(Structure*);
    uint32_t& placeOfShapeThatDied(uint32_t word) { return m_shapesThatDied[word * 0x9e3779b1u >> (32 - bitsOfPlaceOfShapeThatDied)]; }
    std::array<uint32_t, 1u << bitsOfPlaceOfShapeThatDied> m_shapesThatDied { };
    Vector<uint32_t> m_indicesOfAdditions;
    Vector<uint32_t> m_indicesOfAdditionsMadeSinceLastCollection;
    struct NameListLink {
        uint32_t row;
        uint32_t next;
    };
    UncheckedKeyHashMap<UniquedStringImpl*, uint16_t> m_nameByUID;
    BitVector m_registeredNames;
    uint32_t* m_nameListHeads { nullptr };
    SegmentedVector<NameListLink, 512> m_nameListLinks;
    uint32_t m_firstFreeNameListLink { 0 };

    Slot* m_slots { nullptr };
    uint32_t m_numberOfSlots { slotsPerPayload };
    Vector<uint32_t> m_freeSlots;
    Data* m_data { nullptr };
    bool m_servesSeveralInstances { false };
    unsigned m_declaredMemberCreationDepth { 0 };
};

struct FunctionMetadata {
    enum Section : uint32_t {
        ExpressionInfo = 1 << 0,
        Handlers = 1 << 1,
        StringSwitchJumpTables = 1 << 4,
        ConstantIdentifierSets = 1 << 6,
        Scalars = 1 << 7,
    };
    static constexpr uint32_t isBuiltinFunction = 1 << 8;
    static constexpr unsigned instructionsSizeShift = 9;
    static constexpr uint32_t isPositionsOfTypedFunction = 1;
    static constexpr uint32_t isHandlersAndPositionsOfTypedFunction = 2;
    static constexpr uint8_t someHandlerIgnoresValue = 0x80;
    static constexpr unsigned sitesBetweenPositionCheckpoints = 512;
    static constexpr bool isOfTypedFunctionWithHandlers(uint32_t word) { return (word & 3) == isHandlersAndPositionsOfTypedFunction; }
    static unsigned wordsFor(Section section) { return section == Handlers ? 2 : 1; }

    const uint32_t* find(Section section) const
    {
        if (!(flagsAndInstructionsSize & section))
            return nullptr;
        const uint32_t* word = &flagsAndInstructionsSize + 1;
        for (uint32_t earlier = 1; earlier < section; earlier <<= 1) {
            if (flagsAndInstructionsSize & earlier)
                word += wordsFor(static_cast<Section>(earlier));
        }
        return word;
    }
    unsigned instructionsSize() const { return flagsAndInstructionsSize >> instructionsSizeShift; }

    uint32_t flagsAndInstructionsSize;
};

struct SharedData {
    static constexpr unsigned maxSlots = 8192;
    static constexpr size_t size = sizeof(Data) + maxSlots * sizeof(Slot);
    JS_EXPORT_PRIVATE static Data* LEGACY_NODELETE get();
    static bool contains(const Slot* slot) { return std::bit_cast<uintptr_t>(slot) - std::bit_cast<uintptr_t>(get()) < size; }
};

inline const FunctionInfo* ProgramData::infos() const { return at<FunctionInfo>(infosOffset); }
inline uint32_t FunctionInfo::indexAndFlags() const
{
    const ProgramData* data = ProgramData::get();
    return data->indexAndFlagsOfFunctions()[this - data->infos()];
}
inline const ProgramData& FunctionRef::programData() const { return instance ? instance->program->data() : *ProgramData::get(); }
inline const FunctionInfo& FunctionRef::info() const { return instance ? instance->infos[index] : ProgramData::get()->infos()[index]; }

struct Quote {
    enum Kind : uint8_t {
        Exact,
        Approximate,
        Call,
    };
    uint32_t bytecodeOffset;
    uint32_t start;
    Kind kind;
    UTF8CString text;
};

struct CompiledFunctionInfo {
    unsigned codeSize { 0 };
    Convention convention;
    Vector<IndexReference> indexReferences;
    uint32_t stateNumber { 0 };
    Vector<SpreadSite> spreadSites;
    struct InlineFrame {
        uint32_t parent;
        uint32_t callSite;
        uint32_t knownCallee;
        bool isTailCall;
        bool isConstruction { false };
    };
    Vector<InlineFrame> inlineFrames;
    unsigned frameSizeInBytes { 0 };
    unsigned numSlots { 0 };
    unsigned numberOfEmptyArrayLiteralsMadeInPlace { 0 };
    unsigned numberOfEmptyArrayLiteralsMadeByStub { 0 };
    bool startsCold { false };
    RegisterAtOffsetList calleeSaveRegisters;
    Vector<StubCall> stubCalls;
    Vector<ArgumentMoves> argumentMoves;
    Vector<Site> sites;
    Vector<ImageKey> knownCallees;
    uint32_t valueEntry { StubCall::noFunction };
    uint32_t signatureAnswered { StubCall::noFunction };
    uint32_t adapterOfSignature { StubCall::noFunction };
    uint32_t resultOfAdapter { StubCall::noFunction };
    uint32_t numberOfArgumentsOfAdapter { 0 };
    uint32_t sizeOfResultsHoldingWhatIsYielded { 0 };
    uint64_t resultsHoldingWhatIsYielded { 0 };
    static constexpr uint32_t siteConstantIsPlan = 1u << 30;
    Vector<uint32_t> siteConstants;
    Vector<uint32_t> plans;
    Vector<uint32_t> quotableSites;
    Vector<uint32_t> constructSites;
    Vector<uint32_t> sitesThatConstruct;
    Vector<ParameterCheckSite> parameterCheckSites;
    Vector<uint32_t> callSites;
    Vector<uint32_t> identifierIndices;
    Vector<uint32_t> typedIdentifierIndices;
    Vector<String> typedIdentifierNames;
    Vector<std::pair<uint32_t, uint32_t>> typedCatchEntrypointsForCallSites;
    // See Typed::LoweringContext::numberOfSitesWithoutCode. The sites with code have the numbers behind these, in the order of the code.
    uint32_t numberOfSitesWithoutCode { 0 };
    // A load or store that stands for a test of its base for `undefined` and `null`. The words: see Typed::ThrowSite.
    struct FaultingAccess {
        uint32_t codeOffset;
        uint32_t callSite;
        Vector<uint32_t, 3> throwSite;
        bool isLoopPoll { false };
    };
    Vector<FaultingAccess> faultingAccesses;
    bool hasNoFrame { false };
    uint32_t stackTakenAtCalls { 0 };
    bool isEnteredWithoutStackCheck { false };
    bool fitsOnStackByProof { false };
    bool isGenericEntrypoint { false };
    uint8_t kindByte { 0 };
    // See FunctionReportableSites. With typedFunction set, InlineFrame::knownCallee is a number in Typed::Data too.
    uint32_t typedFunction { FunctionReportableSites::notTyped };
    Vector<TypedSitePosition> typedPositions;
    Vector<UTF8CString> typedSourceNames;
    Vector<uint32_t> constantIndices;
    Vector<std::pair<uint32_t, uint32_t>> constructionStarts;
    Vector<Quote> quotes;
};


struct ImageFrame {
    static constexpr unsigned maxCalleeSavesStart = (1u << 6) - 1;

    uint32_t calleeSaveRegisters;
    uint16_t calleeSavesStart;
    uint16_t frameSizeInUnits;

    unsigned frameSizeInBytes() const { return frameSizeInUnits * stackAlignmentBytes(); }
    uint64_t bits() const { return static_cast<uint64_t>(calleeSaveRegisters) << 32 | static_cast<uint64_t>(calleeSavesStart) << 16 | frameSizeInUnits; }
};
static_assert(sizeof(ImageFrame) == 8);

struct ImageFunction {
    static constexpr unsigned numberOfIndexBits = 24;
    static constexpr unsigned numberOfCallSitesBits = 27;
    static constexpr unsigned numberOfSlotsBits = 16;
    static constexpr unsigned numberOfFrameBits = 12;

    uint32_t index : numberOfIndexBits;
    uint32_t numberOfParameters : 8;
    uint32_t callSites : numberOfCallSitesBits;
    uint32_t hasQuotes : 1;
    uint32_t takesList : 1;
    uint32_t hasInlineFrames : 1;
    uint32_t hasSiteConstants : 1;
    uint32_t numSlots : numberOfSlotsBits;
    uint32_t frame : numberOfFrameBits;
    uint32_t startsCold : 1;
    uint32_t saysWhatItAnswers : 1;
    uint32_t saysWhatItYields : 1;

    Convention convention() const { return { takesList ? Signature::List : Signature::Registers, numberOfParameters, true }; }
#if CPU(ARM64)
    static constexpr unsigned firstEncodedGPR = 16;
    static constexpr unsigned firstEncodedFPR = 32;
#else
    static constexpr unsigned firstEncodedGPR = 0;
    static constexpr unsigned firstEncodedFPR = 16;
#endif
    static uint64_t unpackRegisters(uint32_t packed) { return static_cast<uint64_t>(packed & 0xffff) << firstEncodedGPR | static_cast<uint64_t>(packed >> 16) << firstEncodedFPR; }
    static uint32_t packRegisters(uint64_t mask)
    {
        uint32_t packed = static_cast<uint32_t>(mask >> firstEncodedGPR & 0xffff) | static_cast<uint32_t>(mask >> firstEncodedFPR & 0xffff) << 16;
        RELEASE_ASSERT(unpackRegisters(packed) == mask);
        return packed;
    }

    static constexpr ptrdiff_t offsetOfNumSlotsFromSites() { return -static_cast<ptrdiff_t>(sizeof(uint32_t)); }
    const Site* sites() const { return reinterpret_cast<const Site*>(this + 1); }
    const uint32_t* siteConstants() const { return reinterpret_cast<const uint32_t*>(sites() + numSlots); }
    const uint32_t* endOfSiteConstants() const { return siteConstants() + (hasSiteConstants ? numSlots : 0); }
    uint32_t quotes() const { return hasQuotes ? *endOfSiteConstants() : 0; }
    const uint32_t* plans() const { return endOfSiteConstants() + hasQuotes; }
    static constexpr uint32_t noSuchFunction = std::numeric_limits<uint32_t>::max();
};

static_assert(sizeof(ImageFunction) == 12);

inline const ImageFunction* FunctionInfo::function() const
{
    return hasCodeInImage() ? reinterpret_cast<const ImageFunction*>(sitesInImage()) - 1 : nullptr;
}
inline bool FunctionInfo::hasSiteConstants() const { return !isTyped && hasCodeInImage() && function()->hasSiteConstants; }
inline bool FunctionInfo::startsCold() const { return hasCodeInImage() && function()->startsCold && isTyped; }
inline uint32_t FunctionInfo::numberOfSlotsForMisses() const { return hasCodeInImage() ? std::min<uint32_t>(function()->numSlots, maxEncodedSlots) : 0; }

class JITCode final : public JSC::JITCode {
public:
    enum class Way : uint8_t { TopLevel, Call, Construct };
    static Way entryBlockFor(UnlinkedCodeBlock*);
    JITCode(void* code, const ImageFunction&, Way);
    ~JITCode() final;

    CodePtr<JSEntryPtrTag> addressForCall(ArityCheckMode) final;
    void* executableAddressAtOffset(size_t offset) final;
    void* dataAddressAtOffset(size_t offset) final;
    unsigned offsetOf(void* pointerIntoCode) final;
    size_t size() final;
    bool contains(void*) final;

    const RegisterAtOffsetList* calleeSaveRegisters() const { return m_calleeSaveRegisters; }
    const ImageFunction* imageFunction() const { return m_function; }
    uint32_t index() const { return m_function->index; }
    unsigned codeSize() const;
    unsigned frameSizeInBytes() const;
    unsigned numSlots() const { return m_function->numSlots; }
    const Site* sites() const { return m_function->sites(); }
    const void* start() const { return m_code; }
    static constexpr ptrdiff_t offsetOfEntry() { return OBJECT_OFFSETOF(JITCode, m_entry); }
    uint64_t entry() const { return m_entry; }
    static constexpr ptrdiff_t offsetOfInstance() { return OBJECT_OFFSETOF(JITCode, m_instance); }
    Instance* instance() const { return m_instance; }
    void setInstance(Instance& instance) { m_instance = &instance; }

private:
    void* m_code;
    uint64_t m_entry;
    Instance* m_instance { nullptr };
    const ImageFunction* m_function { nullptr };
    const RegisterAtOffsetList* m_calleeSaveRegisters;
};

Instance& instanceOf(JSScope*);

JS_EXPORT_PRIVATE CodePtr<JSEntryPtrTag> moduleCodeEntrypoint();
CodePtr<JSEntryPtrTag> closureEntrypoint();
JS_EXPORT_PRIVATE CodePtr<JSEntryPtrTag> slimClosureEntrypoint();
CodePtr<JSEntryPtrTag> resumeEntrypoint();
void* catchThunk();
void* rejectInFrameThunk();
CodePtr<JSEntryPtrTag> savedFrameEntrypoint();
CodePtr<JSEntryPtrTag> asyncGeneratorEntrypoint();
void* stubAddress(Stub);
Stub handlerOfReadByName(unsigned form, PlaceTable::Kind);
Stub handlerOfStoreByName(unsigned variant, PlaceTable::Kind);
static_assert(Instance::numberOfKindsOfEntriesByName == PlaceTable::numberOfKinds);
static_assert(PlaceTable::kindThatAdds(true, true, PlaceTable::WhereAdded::OutOfLine) == PlaceTable::Kind::AbsentAndStoreAddsOutOfLine && PlaceTable::kindThatAdds(false, false, PlaceTable::WhereAdded::InNewStorage) == PlaceTable::Kind::DefinitionAddsInNewStorage && PlaceTable::storesAdd(PlaceTable::Kind::StoreAddsInObject) && !PlaceTable::storesAdd(PlaceTable::Kind::DefinitionAddsInNewStorage));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
