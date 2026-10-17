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
struct FormatPatternState;
enum class FormatPatternsOrigin : uint8_t { NotCompared, ThisICU, AnotherICU };
struct JSONWideSegment;
struct JSONPlanCache;
struct StringAppender;

#if ASAN_ENABLED && CPU(X86_64) && OS(LINUX)
#define AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE 1
#define FOR_EACH_AOT_OPERATION_FOR_ASAN(v) \
    v(operationAOTReportPoisonedAccess)
#else
#define AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE 0
#define FOR_EACH_AOT_OPERATION_FOR_ASAN(v)
#endif

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
    v(operationAOTCompareEqToString) \
    v(operationAOTCompareStrictEq) \
    v(operationAOTGetById) \
    v(operationAOTPutById) \
    v(operationAOTGetByNameAndCache) \
    v(operationAOTPutByNameAndCache) \
    v(operationAOTGetByVal) \
    v(operationAOTGetByValAndCache) \
    v(operationAOTPutByValAndCache) \
    v(operationAOTPutByValAndCacheSloppy) \
    v(operationAOTInByValAndCache) \
    v(operationAOTDeleteByValAndCache) \
    v(operationAOTDeleteByValAndCacheSloppy) \
    v(operationAOTPutByVal) \
    v(operationAOTWriteBarrier) \
    v(operationAOTGetGeneric) \
    v(operationAOTHasGeneric) \
    v(operationAOTGetGenericAtSite) \
    v(operationAOTGetGenericAndCache) \
    v(operationAOTGetByKeyGeneric) \
    v(operationAOTPutByKeyGeneric) \
    v(operationAOTHasByKeyGeneric) \
    v(operationAOTPrepareProxyGet) \
    v(operationAOTValidateProxyGet) \
    FOR_EACH_AOT_OBJECT_OPERATION(v) \
    FOR_EACH_AOT_BUILTIN_OPERATION(v) \
    FOR_EACH_AOT_TYPED_OPERATION(v) \
    FOR_EACH_AOT_TYPED_BUILTIN_OPERATION(v) \
    FOR_EACH_AOT_OPERATION_FOR_ASAN(v) \
    FOR_EACH_AOT_OPERATION_SHARED_WITH_JIT(v) \

#define FOR_EACH_AOT_OPERATION_SHARED_WITH_JIT(v) \
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
    v(RawTypedHasOwnPropertyByName) \
    v(RawTypedToString) \
    v(RawTypedBitNot) \
    v(RawTypedConstruct) \
    v(RawTypedCloneObject) \
    v(RawTypedCloneObjectAndExtend) \
    v(RawTypedCloneObjectAndExtendWithNumber) \
    v(RawTypedCloneObjectWithSubtypeCheck) \
    v(RawTypedCloneObjectAs) \
    v(RawTypedCoerce) \
    v(RawTypedConstructSuper) \
    v(ErrorConstructorClassInfo) \
    v(NonConstructorHostFunction) \
    v(NewArraySlowPath) \
    v(ValueAddSlowPath) \
    v(AsyncGeneratorClassInfo) \
    v(GeneratorClassInfo) \
    v(DrivenIteratorMarker) \
    v(ResumeFunctionReturnAddress) \
    v(WebAssemblyFunctionClassInfo) \

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

bool hasInstanceParameter(Entry);

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
    void countAllocatedBytes(const char* kind, const Subspace*, size_t cellSize, const ClassInfo* ownerClassInfo, size_t bytes);
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
    void countMegamorphicCacheEpochChange(uint16_t epoch)
    {
        if (std::exchange(m_lastMegamorphicCacheEpoch, epoch) != epoch)
            countOperation("MegamorphicCache", "epoch-changed");
    }
    void countMegamorphicCacheFill(const char* kind, uint16_t epoch, uint64_t structureIDs, const void* uid, const char* outcome)
    {
        if (outcome) {
            countOperation(kind, outcome);
            return;
        }
        if (std::exchange(m_filledPairsEpoch, epoch) != epoch)
            m_filledPairs.clear();
        countOperation(kind, m_filledPairs.add({ kind, uid, structureIDs }).isNewEntry ? "new-pair" : "pair-again");
    }
    void removeDeadFilledPairs(VM&);
    bool recordLookup(const char* operation, uint64_t structureID, const void* uid) { return m_filledPairs.add({ operation, uid, structureID }).isNewEntry; }
    void recordSample(const char* kind, StringView group, String&& sample);
    uint64_t operationCount(StringView nameAndDetail) const;
    void dumpOperationCounts(PrintStream&) const;
    void writeOperationCounts() const;

private:
    void* m_entries[numberOfEntries];
    Vector<std::unique_ptr<VirtualCallInfo>> m_callLinkInfos;
    UncheckedKeyHashMap<std::pair<const char*, const char*>, uint64_t> m_operationCounts;
    UncheckedKeyHashMap<std::tuple<const char*, const void*, size_t>, UTF8CString> m_allocatedBytesDetails;
    UncheckedKeyHashMap<UTF8CString, std::pair<uint64_t, uint64_t>> m_allocatedBytesByStack;
    size_t m_bytesAllocatedSinceLastWrite { 0 };
    UncheckedKeyHashMap<uint64_t, UTF8CString> m_siteDetails;
    uint16_t m_lastMegamorphicCacheEpoch { 0 };
    uint16_t m_filledPairsEpoch { 0 };
    UncheckedKeyHashSet<std::tuple<const char*, const void*, size_t>> m_filledPairs;
    UncheckedKeyHashMap<String, std::pair<uint64_t, String>> m_samples;
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
    static constexpr uint32_t missesBeforeCaching = 12;
    static constexpr uint32_t missesBeforePolymorphic = 4;
    static_assert(missesBeforeCaching + 2 < maxAttempts);
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
    static constexpr uint32_t compiledFieldFlags = isIndirect | isGetter | pointerIsNotCell;
    enum class FieldRead : uint8_t {
        Boxed,
        Cell32,
        Int32OrNumberBox,
        Int32,
        Double,
        Boolean,
        OptionalBoolean,
        Int64,
        Uint8 = 0x30,
        Int8,
        Uint16,
        Int16,
        Uint32,
        NoField = 0x37,
        KnownCustomGetter = 0x36,
    };
    static constexpr unsigned fieldReadShift = 32;
    static constexpr uint64_t knownCustomGetter = static_cast<uint64_t>(FieldRead::NoField) << fieldReadShift | 1;
    static constexpr uint64_t isLookedUp = static_cast<uint64_t>(FieldRead::NoField) << fieldReadShift | 2;
    static constexpr uint64_t isAbsentWhileCountUnchanged = static_cast<uint64_t>(FieldRead::NoField) << fieldReadShift | 4;
    static constexpr unsigned countShift = 3;
    static constexpr uint32_t countIsValidated = 1u << 28;
    static_assert((static_cast<unsigned>(FieldRead::NoField) & 0x36) == 0x36);
    static constexpr unsigned customGetterIndexShift = 40;
    static constexpr uint32_t customGetterSerialStep = 8;
    static constexpr unsigned extraPropertyShift = 40;
    static constexpr uint32_t extraPropertyIsOutOfLine = 1u << 19;
    static constexpr uint32_t extraPropertyIsValue = 1u << 20;
    static constexpr uint32_t extraPropertyIsAbsent = 1u << 21;
    static constexpr uint32_t extraPropertyOffsetMask = extraPropertyIsOutOfLine - 1;
    static constexpr uint64_t extraPropertyAt(uint32_t extraPropertiesStructureID, uint32_t offsetAndKind) { return static_cast<uint64_t>(offsetAndKind) << extraPropertyShift | static_cast<uint64_t>(FieldRead::NoField) << fieldReadShift | extraPropertiesStructureID; }
    static constexpr uint64_t knownCustomGetterAt(uint32_t tableIndex, uint32_t serial) { return static_cast<uint64_t>(tableIndex) << customGetterIndexShift | static_cast<uint64_t>(FieldRead::KnownCustomGetter) << fieldReadShift | serial; }
    struct FieldWrite {
        enum Kept : uint32_t { Boxed, Cell32, Int32OrNumberBox, Double, Boolean, Int64, Int32 };
        static constexpr uint32_t representationMask = 7;
        static constexpr uint32_t isOptional = 1u << 3;
        static constexpr uint32_t storesLiteralStringWords = isOptional;
        static constexpr uint32_t acceptsInt32 = 1u << 4;
        static constexpr uint32_t acceptsNumber = 1u << 5;
        static constexpr uint32_t acceptsString = 1u << 6;
        static constexpr uint32_t acceptsSymbol = 1u << 7;
        static constexpr uint32_t acceptsBigInt = 1u << 8;
        static constexpr uint32_t acceptsObject = 1u << 9;
        static constexpr uint32_t acceptsUndefined = 1u << 10;
        static constexpr uint32_t acceptsNull = 1u << 11;
        static constexpr uint32_t acceptsBoolean = 1u << 12;
        static constexpr uint32_t isNotField = 1u << 13;
        static constexpr uint32_t isInGlobalProxyTarget = 1u << 14;
        static constexpr uint32_t isInExtension = isNotField;
        static constexpr uint32_t hasNoFieldInfo = isNotField | isInGlobalProxyTarget | 1u << 31;
        static constexpr unsigned offsetBits = 16;
        static constexpr unsigned typeShift = 15;
        static constexpr unsigned typeBitsInUpperHalf = 15;
        static constexpr uint64_t withType(uint32_t offsetInData) { return static_cast<uint64_t>(offsetInData >> 2 & 0xffff) << offsetBits | static_cast<uint64_t>(offsetInData >> 18) << (32 + typeShift); }
        static constexpr uint32_t isFieldWrite = 1u << 31;
    };
    static constexpr uint64_t isStoreTransition = 1ull << 62;
    static constexpr uint64_t absentFieldIsUndefined = 1ull << (fieldReadShift + 3);
    static constexpr uint64_t fieldIsInExtension = 1ull << (fieldReadShift + 4);
    static constexpr uint64_t fieldHoldsLazyClosure = 1ull << (fieldReadShift + 7);
    static constexpr unsigned fieldClosureShift = fieldReadShift + 8;
    static constexpr unsigned fieldClosureKindShift = 62;
    static constexpr uint64_t fieldClosureCapturesHolder = 1ull << 63;
    static_assert(fieldClosureShift + ClosureWord::numberOfFunctionBits <= fieldClosureKindShift);
    bool isPolymorphic() const { return !structureID && (offset & flagsMask) == polymorphicFlags; }
    bool isNameOnly() const { return !structureID && !(offset & (flagsMask | attemptsMask)) && offset >> nameIDShift; }
    bool readsExtraProperty() const
    {
        uint64_t word = std::bit_cast<uint64_t>(pointer);
        return (offset & flagsMask) == compiledFieldFlags && ((word >> fieldReadShift) & 0xff) == static_cast<uint64_t>(FieldRead::NoField) && word >> extraPropertyShift && !(word >> 62);
    }
    StructureID extraPropertiesStructureID() const { return std::bit_cast<StructureID>(static_cast<uint32_t>(std::bit_cast<uint64_t>(pointer))); }

    void clear()
    {
        structureID = StructureID();
        offset &= attemptsMask;
        pointer = nullptr;
    }

    void clearAndResetAttempts()
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
    static constexpr unsigned numberOfSlots = 8;
    static constexpr unsigned structureShift = 4;
    static_assert(hasOneBitSet(numberOfSlots));
    static constexpr unsigned indexFor(StructureID id) { return id.bits() >> structureShift & (numberOfSlots - 1); }
    Slot* slotOf(StructureID id)
    {
        unsigned index = indexFor(id);
        return slots[index].structureID == id ? &slots[index] : slots[index ^ 1].structureID == id ? &slots[index ^ 1] : nullptr;
    }
    static constexpr uint32_t maxBulkFillAttempts = 12;
    static constexpr uint32_t missesPerRefill = 1024;
    static constexpr uint32_t missesPerEarlyRefill = 4;
    static_assert(hasOneBitSet(missesPerRefill) && hasOneBitSet(missesPerEarlyRefill));

    static constexpr ptrdiff_t offsetOfName() { return OBJECT_OFFSETOF(PolymorphicSlots, name); }
    static constexpr ptrdiff_t offsetOfMissesUntilRefill() { return OBJECT_OFFSETOF(PolymorphicSlots, missesUntilRefill); }
    static constexpr ptrdiff_t offsetOfRemainingBulkFillAttempts() { return OBJECT_OFFSETOF(PolymorphicSlots, remainingBulkFillAttempts); }
    static constexpr ptrdiff_t offsetOfSlots() { return OBJECT_OFFSETOF(PolymorphicSlots, slots); }
    static constexpr ptrdiff_t offsetOfByName() { return OBJECT_OFFSETOF(PolymorphicSlots, byName); }
    static constexpr ptrdiff_t offsetOfIdentifierIndex() { return OBJECT_OFFSETOF(PolymorphicSlots, identifierIndex); }
    static constexpr ptrdiff_t offsetOfRefillsAreThrottled() { return OBJECT_OFFSETOF(PolymorphicSlots, refillsAreThrottled); }

    static constexpr unsigned numberOfInlineSlotsByName = 4;
    static constexpr unsigned inlineNameSlotsShift = 16;
    static constexpr unsigned nameTableFillValueShift = 48;
    static constexpr uint64_t initialByName = Structure::firstReservedPropertyNameID;
    static constexpr uint8_t maxNameTableFillFailures = 16;
    bool fillsNameTables() const { return !(byName >> nameTableFillValueShift); }
    bool isDueForRefill() const { return !refillsAreThrottled || missesUntilRefill == (remainingBulkFillAttempts ? missesPerEarlyRefill : missesPerRefill); }
    void didMissWithoutRefill()
    {
        remainingBulkFillAttempts = std::min(remainingBulkFillAttempts + 1, maxBulkFillAttempts);
        missesUntilRefill = std::min(missesUntilRefill, missesPerEarlyRefill - 1);
        shouldRefillEarly = true;
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
    uint32_t missesUntilRefill;
    uint32_t remainingBulkFillAttempts;
    Data* owner;
    uint8_t numberOfUsedInlineNameSlots;
    uint8_t remainingNameTableFillFailures;
    bool refillsAreThrottled;
    bool shouldRefillEarly;
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
    uint32_t isTyped : 1;
    uint32_t isHiddenInStackTraces : 1;
    uint32_t hasNoHandlers : 1;
    uint32_t stateNumber : 32 - stateNumberShift;
};
static_assert(sizeof(FunctionInfo) == 8);

enum class Receiver : uint8_t { None, String, Array, Map, Set, WeakMap, WeakSet, RegExp, Date, Number };
enum class MapOrSetMethod : uint8_t { MapGet, MapHas, MapSet, SetHas, SetAdd };

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

enum class JSONSourceEncoding : uint32_t { Latin1, UTF8, Saturated16Bit };

template<typename T>
class ZeroedStorage {
public:
    T& get() { return *reinterpret_cast<T*>(m_bytes); }
    const T& get() const { return *reinterpret_cast<const T*>(m_bytes); }

private:
    alignas(T) uint8_t m_bytes[sizeof(T)];
};

struct Instance {
    JS_EXPORT_PRIVATE static Instance& ensure(JSModuleLoader*);
    JS_EXPORT_PRIVATE static Instance& ensure(JSGlobalObject*);
    static Instance* of(JSFunction*);
    JS_EXPORT_PRIVATE ScriptExecutable* topLevelExecutableOf(uint32_t moduleID);
    JS_EXPORT_PRIVATE void setTopLevelExecutableOf(uint32_t moduleID, ScriptExecutable*);
    JS_EXPORT_PRIVATE uint64_t prepareModuleCode(ModuleProgramExecutable*, JSScope*);
    JS_EXPORT_PRIVATE JSValue didRunModuleCode(AbstractModuleRecord*, ModuleProgramExecutable*, JSValue yielded);
    JS_EXPORT_PRIVATE static JSObject* compiledEntryPointImport(AbstractModuleRecord*);
    JS_EXPORT_PRIVATE void didFinishModuleEvaluation(ModuleProgramExecutable*);
    JSModuleLoader* loader() const;
    bool loaderWasCleared() const;
    JS_EXPORT_PRIVATE static void destroyUnneededInstances(VM&);
    JS_EXPORT_PRIVATE static void destroy(Instance*);

    template<typename Visitor> void visit(Visitor&, bool newOnly);
    template<typename Visitor> void visitCompiledCodeRoots(Visitor&);
    void finalizeUnconditionally(bool newOnly);
    void clearCachesValidatedByMegamorphicCacheEpoch(bool isAtEndOfCollection = false, bool newOnly = false);
    void sweepKeyedTables(bool newOnly);
    void clearNewTarget();

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
    static constexpr ptrdiff_t offsetOfWaiterResolution() { return OBJECT_OFFSETOF(Instance, waiterResolution); }
    static constexpr ptrdiff_t offsetOfWaiterAsyncContext() { return OBJECT_OFFSETOF(Instance, waiterAsyncContext); }
    static constexpr ptrdiff_t offsetOfEnteredSavedFrame() { return OBJECT_OFFSETOF(Instance, enteredSavedFrame); }
    static constexpr ptrdiff_t offsetOfProgramIdentifiers() { return OBJECT_OFFSETOF(Instance, programIdentifiers); }
    static constexpr ptrdiff_t offsetOfProgramIdentifierPositions() { return OBJECT_OFFSETOF(Instance, programIdentifierPositions); }
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
            tryEnsureData(index);
            return;
        }
        counter = static_cast<uint16_t>(after);
    }

    static constexpr uint16_t linkedWithoutDataState() { return OBJECT_OFFSETOF(Instance, linkedFunctionsSharedData) / sizeof(Data*); }
    static constexpr unsigned numberOfMissCounters = 4096;
    static constexpr unsigned maxNumberOfOwnData = 56 * 1024;
    static constexpr uint32_t minStateWithData = 1u << 17;
    static constexpr unsigned stateWithDataShift = 4;
    bool isLinked(uint32_t index) const { return states[infos[index].stateNumber]; }
    Data* dataIfExists(uint32_t index) const
    {
        uint16_t number = states[infos[index].stateNumber];
        return number > linkedWithoutDataState() ? std::bit_cast<const BiasedDataPointer*>(this)[number].get() : nullptr;
    }
    bool hasCapacityForData() const;
    JS_EXPORT_PRIVATE void tryEnsureData(uint32_t index);
    void setData(uint32_t index, Data*);
    void setNotLinked(uint32_t index);
    void didCacheTransition(Slot*);
    void didAddCell(JSCell*);
    PolymorphicSlots* createSiteSlots(Data*, const Slot*, UniquedStringImpl* name);
    void* allocateForData(size_t);
    void freeDataMemory(void*, size_t);
    void didStopWatching(Data*, Slot*, bool wasWatched);
    void validateUnwatchedSlots(const Slot* slotBeingFilled, bool isAtCollection);
    void clearUnwatchedSlots(Data*);
    static constexpr uint32_t extraMissesAllowed = 4;
    static constexpr uint32_t cacheMissLimitFor(uint32_t numSlots) { return std::min<uint32_t>(numSlots + extraMissesAllowed, std::numeric_limits<uint16_t>::max()); }

    JS_EXPORT_PRIVATE JSCell** environmentSlot(ImageEnvironment) const;

    Structure* literalStructure(Structure* empty, std::span<UniquedStringImpl* const>, uint32_t structureDescriptorPlusOne = 0);
    Structure* emptyStructureWithoutPrototype(unsigned inlineCapacity);

    enum class PropertyRunBudget : uint8_t { None, SymbolKeyed, CopiedNames };
    struct PropertyRunTarget {
        Structure* last { nullptr };
        Vector<StructureID, 4> prototypeStructures;
        PropertyRunBudget room { PropertyRunBudget::None };
    };
    bool reservePropertyRunTarget(PropertyRunBudget);
    const PropertyRunTarget& propertyRunTarget(Structure*, const uint32_t* run, const ScopedLambda<void(Vector<UniquedStringImpl*, 16>&)>& collectNames);
    const uint32_t* canonicalPropertyRun(std::span<UniquedStringImpl* const>);
    bool hasPropertyRunTarget(Structure*, std::span<UniquedStringImpl* const>);
    struct CopiedProperties {
        Structure* last { nullptr };
        Vector<std::pair<PropertyOffset, PropertyOffset>, 8> offsets;
    };
    const CopiedProperties& copiedProperties(Structure* target, Structure* source, const IdentifierSet* excluded);
    struct ClassStructures {
        Structure* classObjectStructure { nullptr };
        Structure* ofPrototype { nullptr };
    };
    ClassStructures classStructures(uint32_t classIndex, JSObject* classObjectParent, JSCell* prototypeParent);
    void cacheClassStructures(uint32_t classIndex, JSObject* classObjectParent, JSCell* prototypeParent, ClassStructures);
    static constexpr unsigned maxObservedInlineCapacity = 16;
    void didAddOutOfLineProperty(Structure*);
    unsigned learnedInlineCapacityOfDescriptor(uint32_t number);
    unsigned frameSizeInBytesOf(uint32_t function) const;
    const void* resumeFunctionReturnAddress() const { return runtimeTable[static_cast<unsigned>(Entry::ResumeFunctionReturnAddress)]; }
    JS_EXPORT_PRIVATE void clearResolvedGlobalScopes();
    JS_EXPORT_PRIVATE void clearGlobalPropertyValues();
    void recordPropertyRun(Structure* from, Structure* to);
    JSFunction* constructorForStructure(Structure*);
    void recordInlineCapacity(JSFunction* constructor, Structure*);
    static PropertyOffset offsetAfter(PropertyOffset offset, unsigned inlineCapacity)
    {
        if (offset == invalidOffset)
            return inlineCapacity ? 0 : firstOutOfLineOffset;
        return offset + 1 == static_cast<PropertyOffset>(inlineCapacity) ? firstOutOfLineOffset : offset + 1;
    }

    struct CloneAndExtendPlan {
        Structure* structure { nullptr };
        std::array<PropertyOffset, 2> offsets { invalidOffset, invalidOffset };
    };
    const CloneAndExtendPlan* cloneAndExtendPlan(Structure* source, uint64_t more);
    struct CopyExcluding {
        Structure* structure { nullptr };
        Vector<PropertyOffset, 4> offsets;
        Vector<RefPtr<UniquedStringImpl>, 4> excluded;
    };
    const CopyExcluding* copyExcluding(Structure* source, std::span<UniquedStringImpl* const> excluded);
    const CopyExcluding* setCopyExcluding(Structure* source, CopyExcluding&&);
    const CloneAndExtendPlan* setCloneAndExtendPlan(Structure* source, uint64_t more, CloneAndExtendPlan);
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
    static constexpr ptrdiff_t offsetOfStructureIDBase() { return negativeOffsetOf(OBJECT_OFFSETOF(NegativeOffsetFields, structureIDBase)); }
    static constexpr ptrdiff_t offsetOfLookupTableBaseForStructureIDZero() { return OBJECT_OFFSETOF(Instance, lookupTableBaseForStructureIDZero); }
    static constexpr ptrdiff_t offsetOfLookupTableSlots() { return OBJECT_OFFSETOF(Instance, lookupTableSlots); }
    static constexpr unsigned numberOfByNameEntryKinds = 32;
    static constexpr ptrdiff_t offsetOfGetByNameHandlers(unsigned form) { return OBJECT_OFFSETOF(Instance, getByNameHandlers) + form * numberOfByNameEntryKinds * sizeof(void*); }
    static constexpr ptrdiff_t offsetOfPutByNameHandlers(unsigned variant) { return OBJECT_OFFSETOF(Instance, putByNameHandlers) + variant * numberOfByNameEntryKinds * sizeof(void*); }
    static constexpr ptrdiff_t offsetOfCellCageBase() { return OBJECT_OFFSETOF(Instance, cellCageBase); }
    static constexpr ptrdiff_t offsetOfNumberTag() { return OBJECT_OFFSETOF(Instance, numberTag); }
    static constexpr ptrdiff_t offsetOfNotCellMask() { return OBJECT_OFFSETOF(Instance, notCellMask); }
    static constexpr ptrdiff_t offsetOfNotUndefinedOrNullMask() { return negativeOffsetOf(OBJECT_OFFSETOF(NegativeOffsetFields, notUndefinedOrNullMask)); }
    static constexpr ptrdiff_t offsetOfNumberBoxBase() { return OBJECT_OFFSETOF(Instance, numberBoxBase); }
    static constexpr ptrdiff_t offsetOfBoxedNaN() { return negativeOffsetOf(OBJECT_OFFSETOF(NegativeOffsetFields, boxedNaN)); }
    static constexpr int minCachedIntegerDouble = -1;
    static constexpr int maxCachedIntegerDouble = 127;
    static constexpr uint64_t otherCachedDoubleBits[] = {
        std::bit_cast<uint64_t>(0.5), std::bit_cast<uint64_t>(65536.0), std::bit_cast<uint64_t>(2147483647.0), std::bit_cast<uint64_t>(2147483648.0), std::bit_cast<uint64_t>(-2147483648.0),
        std::bit_cast<uint64_t>(4294967295.0), std::bit_cast<uint64_t>(4294967296.0), std::bit_cast<uint64_t>(-4294967296.0), std::bit_cast<uint64_t>(9007199254740992.0), std::bit_cast<uint64_t>(-9007199254740992.0),
        std::bit_cast<uint64_t>(9223372036854775808.0), 0x7ff8ull << 48, Typed::undefinedDouble, Typed::nullDouble, Typed::absentDouble,
    };
    static constexpr ptrdiff_t offsetOfCachedIntegerDoubles() { return OBJECT_OFFSETOF(Instance, cachedIntegerDoubles); }
    static std::optional<ptrdiff_t> offsetOfDouble(double value)
    {
        if (value == 1)
            return negativeOffsetOf(OBJECT_OFFSETOF(NegativeOffsetFields, one));
        if (std::bit_cast<uint64_t>(value) == 0x7ff8ull << 48)
            return negativeOffsetOf(OBJECT_OFFSETOF(NegativeOffsetFields, pureNaN));
        if (value >= minCachedIntegerDouble && value <= maxCachedIntegerDouble && value && value == static_cast<int>(value))
            return offsetOfCachedIntegerDoubles() + (static_cast<int>(value) - minCachedIntegerDouble) * static_cast<ptrdiff_t>(sizeof(double));
        if (std::bit_cast<uint64_t>(value) == Typed::undefinedDouble)
            return OBJECT_OFFSETOF(Instance, undefinedDouble);
        for (size_t i = 0; i < std::size(otherCachedDoubleBits); ++i) {
            if (std::bit_cast<uint64_t>(value) == otherCachedDoubleBits[i])
                return OBJECT_OFFSETOF(Instance, otherCachedDoubles) + i * static_cast<ptrdiff_t>(sizeof(double));
        }
        return std::nullopt;
    }
    static constexpr ptrdiff_t offsetOfFieldIDAtDirectLocationBase() { return OBJECT_OFFSETOF(Instance, fieldIDAtDirectLocationBase); }
    static constexpr ptrdiff_t offsetOfObjectPrototype() { return OBJECT_OFFSETOF(Instance, objectPrototype); }

    struct NegativeOffsetFields {
        uint64_t newArrayWithInt32Header;
        uint64_t newArrayWithDoubleHeader;
        uint64_t newArrayWithContiguousHeader;
        int64_t notUndefinedOrNullMask;
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
        uint32_t typedPrototypeEpoch;
        uint32_t typedClosureStructureID;
        JSCell** typedBuiltins;
        uintptr_t structureIDBase;
        uint64_t* typedStrings;
        EncodedJSValue boxedNaN;
        JSCell* emptyString;
    };
    NegativeOffsetFields& negativeOffsetFields() { return reinterpret_cast<NegativeOffsetFields*>(this)[-1]; }
    const NegativeOffsetFields& negativeOffsetFields() const { return reinterpret_cast<const NegativeOffsetFields*>(this)[-1]; }
    static constexpr ptrdiff_t negativeOffsetOf(size_t offsetInNegativeOffsetFields) { return static_cast<ptrdiff_t>(offsetInNegativeOffsetFields) - static_cast<ptrdiff_t>(sizeof(NegativeOffsetFields)); }

    BiasedDataPointer sharedData;
    const uint32_t* typedShapeSets;
    VM* vm;
    ZeroedStorage<StackManager::Mirror> stackMirror;
    uint32_t barrierThreshold;
    uint8_t mutatorShouldBeFenced;
    uint8_t awaitMustSuspend;
    bool isResumingWaiters;
    uint64_t typedSlimClosureHeaders[2];
    uint64_t undefinedDouble;
    uintptr_t numberBoxBase;
    void* typedClosures;
    void* singleCharacterStrings;
    int64_t numberTag;
    int64_t notCellMask;
    JSGlobalObject* globalObject;
    JSCell* waiterToResume;
    EncodedJSValue waiterResolution;
    EncodedJSValue waiterAsyncContext;
    JSCell* enteredSavedFrame;
    const uint64_t* typedFunctions;
    uintptr_t cellCageBase;
    const void* savedFrameReturnAddress;
    void* savedFrameSpace;
    uint64_t savedFrameHeader;
    static constexpr unsigned arrayKindShift = 1;
    static constexpr unsigned numberOfArrayKinds = 16;
    static_assert(((IndexingShapeMask | CopyOnWrite) >> arrayKindShift) == numberOfArrayKinds - 1);
    uint32_t originalArrayStructureIDs[numberOfArrayKinds];
    static constexpr ptrdiff_t offsetOfOriginalArrayStructureIDs() { return OBJECT_OFFSETOF(Instance, originalArrayStructureIDs); }
    uint32_t firstThisStructureDescriptorPlusOne;
    static constexpr ptrdiff_t offsetOfFirstThisStructureDescriptorPlusOne() { return OBJECT_OFFSETOF(Instance, firstThisStructureDescriptorPlusOne); }
    const double* powersOfTen;
    const uint32_t* functionMetadataOffsets;
    const uint8_t* code;
    const uint32_t* codeGranules;
    const uint32_t* subsequentFunctionStarts;
    struct TypedAllocation {
        uint64_t header;
        uint64_t words;
        void* allocator;
        const void* fieldTemplate;
    };
    Typed::State* typed;
    const FunctionInfo* infos;
    static constexpr unsigned typedCardShift = 8;
    static constexpr uint8_t typedCardIsDirty = 1;
    static constexpr uint8_t typedCardWasScanned = 2;
    void didStoreRootInStatics(uint32_t offset) { typedCards[offset >> typedCardShift] = typedCardIsDirty; }
    uint16_t* typedAllocationIndices;
    TypedAllocation* typedAllocation(uint32_t number) const
    {
        uint16_t row = typedAllocationIndices[number];
        return row ? reinterpret_cast<TypedAllocation*>(typedAllocationIndices) - row : nullptr;
    }
    uint8_t* typedModifiedClassObjects;
    JSCell** typedClassPrototypes;
    static constexpr unsigned numberOfTypedClosureStructures = 5;
    uint32_t typedClosureStructureIDs[numberOfTypedClosureStructures - 1];
    uint32_t& typedClosureStructureID(unsigned which) { return which ? typedClosureStructureIDs[which - 1] : negativeOffsetFields().typedClosureStructureID; }
    uint64_t typedClosureStructureIDPair;
    LocalAllocator* typedSlimClosureAllocators[3];
    JSCell* typedSlimClosureLengthGetter;
    JSCell* typedSlimClosureNameGetter;
    uint8_t typedModifiedLiterals[256];
    const uint8_t* typedData;
    JSCell** typedWideStrings;
    JSCell** typedLiteralStrings;
    uint64_t* typedLiteralStringCache;
    uint8_t* typedCards;
    static constexpr unsigned numberOfTypedExecutables = 6;
    JSCell* typedExecutables[numberOfTypedExecutables];
    uint8_t* typedStatics;
    struct KeyedRead;
    struct KeyedAddition;
    KeyedRead* keyedReads;
    Slot* keyedHas;
    KeyedAddition* keyedAdditions;
    uintptr_t** typedOrderKeys;
    uint16_t* typedOrderKeyIndices;
    uint16_t numberOfTypedOrderKeyIndicesUsed;
    static constexpr ptrdiff_t offsetOfTypedOrderKeys() { return OBJECT_OFFSETOF(Instance, typedOrderKeys); }
    static constexpr ptrdiff_t offsetOfTypedOrderKeyIndices() { return OBJECT_OFFSETOF(Instance, typedOrderKeyIndices); }
    static size_t numberOfTypedOrderKeyIndices(uint32_t numberOfLayouts) { return std::min<size_t>(numberOfLayouts, UINT16_MAX) + 1; }
    void setTypedOrderKeys(uint32_t shape, uintptr_t* keys)
    {
        if (!typedOrderKeyIndices[shape]) {
            if (numberOfTypedOrderKeyIndicesUsed == UINT16_MAX)
                return;
            typedOrderKeyIndices[shape] = ++numberOfTypedOrderKeyIndicesUsed;
        }
        typedOrderKeys[typedOrderKeyIndices[shape]] = keys;
    }
    struct TypedKeyList {
        uint32_t orderWord;
        uint32_t shapeWord;
        JSCell* list;
    };
    static constexpr uint32_t numberOfTypedKeyLists = 1024;
    static constexpr uint32_t indexOfTypedKeyList(uint32_t shapeWord, uint32_t orderWord) { return (shapeWord + orderWord) % numberOfTypedKeyLists; }
    TypedKeyList* typedKeyLists;
    static constexpr ptrdiff_t offsetOfTypedKeyLists() { return OBJECT_OFFSETOF(Instance, typedKeyLists); }
    JSCell* emptyKeyList;
    static constexpr ptrdiff_t offsetOfEmptyKeyList() { return OBJECT_OFFSETOF(Instance, emptyKeyList); }
    JSCell* typedArrayLengthGetter;
    static constexpr ptrdiff_t offsetOfTypedArrayLengthGetter() { return OBJECT_OFFSETOF(Instance, typedArrayLengthGetter); }
    struct TypedArrayWithBuiltinLength {
        uint32_t structureID;
        uint32_t prototypeStructureID;
        uint32_t secondPrototypeStructureID;
        uint32_t unused;
        JSCell* prototype;
        JSCell* secondPrototype;
    };
    static_assert(sizeof(TypedArrayWithBuiltinLength) == 32);
    TypedArrayWithBuiltinLength typedArraysWithBuiltinLength[NumberOfTypedArrayTypesExcludingDataView][2];
    JSCell* typedArrayViewPrototype;
    JSCell* typedArrayLengthAccessor;
    intptr_t typedArrayLengthOffsetInButterfly;
    uint32_t typedArrayViewPrototypeStructureID;
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
    const uint16_t* programIdentifierPositions;
    uintptr_t lookupTableBaseForStructureIDZero;
    Slot* lookupTableSlots;
    void* getByNameHandlers[3][numberOfByNameEntryKinds];
    void* putByNameHandlers[6][numberOfByNameEntryKinds];
    JSObject* objectPrototype;
    JSObject* stringPrototype;
    JSObject* numberPrototype;
    JSObject* booleanPrototype;
    JSObject* symbolPrototype;
    static constexpr ptrdiff_t offsetOfStringPrototype() { return OBJECT_OFFSETOF(Instance, stringPrototype); }
    static constexpr ptrdiff_t offsetOfNumberPrototype() { return OBJECT_OFFSETOF(Instance, numberPrototype); }
    static constexpr ptrdiff_t offsetOfBooleanPrototype() { return OBJECT_OFFSETOF(Instance, booleanPrototype); }
    static constexpr ptrdiff_t offsetOfSymbolPrototype() { return OBJECT_OFFSETOF(Instance, symbolPrototype); }
    JSCell* functionPrototypeCall;
    uint32_t boundFunctionStructureID;
    uint32_t hostFunctionStructureID;
    uintptr_t fieldIDAtDirectLocationBase;
    JSObject* realmObjectPrototype;
    static constexpr ptrdiff_t offsetOfRealmObjectPrototype() { return OBJECT_OFFSETOF(Instance, realmObjectPrototype); }
    JSObject* lockedRealmObjectPrototype;
    static constexpr ptrdiff_t offsetOfLockedRealmObjectPrototype() { return OBJECT_OFFSETOF(Instance, lockedRealmObjectPrototype); }
    const uint32_t* nameDisplacements;
    static constexpr ptrdiff_t offsetOfNameDisplacements() { return OBJECT_OFFSETOF(Instance, nameDisplacements); }
    const uint32_t* offsetsByNameAndStructure;
    static constexpr ptrdiff_t offsetOfOffsetsByNameAndStructure() { return OBJECT_OFFSETOF(Instance, offsetsByNameAndStructure); }
    static constexpr ptrdiff_t offsetOfTypedPrototypeEpoch() { return negativeOffsetOf(OBJECT_OFFSETOF(NegativeOffsetFields, typedPrototypeEpoch)); }
    static constexpr ptrdiff_t offsetOfFunctionPrototypeCall() { return OBJECT_OFFSETOF(Instance, functionPrototypeCall); }
    static constexpr ptrdiff_t offsetOfBoundFunctionStructureID() { return OBJECT_OFFSETOF(Instance, boundFunctionStructureID); }
    static constexpr ptrdiff_t offsetOfHostFunctionStructureID() { return OBJECT_OFFSETOF(Instance, hostFunctionStructureID); }
    JSCell* boundFunctionExecutable;
    Structure* argumentsStructure;
    std::array<Structure*, 2> emptyStructuresForKeyedStores;
    std::array<uint32_t, 128> transitionsRequestedOnce;
    bool wasTransitionRequestedBefore(StructureID structureID, const UniquedStringImpl* name)
    {
        uint32_t step = (structureID.bits() + static_cast<uint32_t>(std::bit_cast<uintptr_t>(name) >> 4)) * 0x9e3779b1u | 1;
        uint32_t& noted = transitionsRequestedOnce[step >> 25];
        return std::exchange(noted, step) == step;
    }
    JSCell* argumentsCallee;
    JSCell* argumentsIterator;
    static constexpr ptrdiff_t offsetOfArgumentsCallee() { return OBJECT_OFFSETOF(Instance, argumentsCallee); }
    static constexpr ptrdiff_t offsetOfArgumentsIterator() { return OBJECT_OFFSETOF(Instance, argumentsIterator); }
    static constexpr ptrdiff_t offsetOfBoundFunctionExecutable() { return OBJECT_OFFSETOF(Instance, boundFunctionExecutable); }
    const ClassInfo* boundFunctionClassInfo;
    static constexpr ptrdiff_t offsetOfBoundFunctionClassInfo() { return OBJECT_OFFSETOF(Instance, boundFunctionClassInfo); }
    EncodedJSValue intrinsics[ImmutableIntrinsics::maximumCount];
    EncodedJSValue linkTimeConstants[numberOfLinkTimeConstants];
    static constexpr unsigned numberOfReceivers = 16;
    uint32_t receiverStructureIDs[numberOfReceivers];
    static constexpr ptrdiff_t offsetOfReceiverStructureID(Receiver receiver)
    {
        switch (receiver) {
        case Receiver::Map:
            return negativeOffsetOf(OBJECT_OFFSETOF(NegativeOffsetFields, mapStructureID));
        case Receiver::Set:
            return negativeOffsetOf(OBJECT_OFFSETOF(NegativeOffsetFields, setStructureID));
        case Receiver::RegExp:
            return negativeOffsetOf(OBJECT_OFFSETOF(NegativeOffsetFields, regExpStructureID));
        case Receiver::WeakMap:
            return negativeOffsetOf(OBJECT_OFFSETOF(NegativeOffsetFields, weakMapStructureID));
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
    uint8_t nextVectorLengths[maxVectorLengthGrownByStub + 1];
    static constexpr ptrdiff_t offsetOfNextVectorLengths() { return OBJECT_OFFSETOF(Instance, nextVectorLengths); }
    uint64_t newArrayWithTypedContiguousHeader;
    uint64_t regExpMatchesArrayHeader;
    static constexpr ptrdiff_t offsetOfRegExpMatchesArrayHeader() { return OBJECT_OFFSETOF(Instance, regExpMatchesArrayHeader); }
    Structure* cachedTypedContiguousArrayStructure;
    JS_EXPORT_PRIVATE Structure* typedContiguousArrayStructure();
    uint64_t newCopyOnWriteArrayHeaders[2];
    uint64_t arrayUnderConstructionHeaders[3];
    Structure* arrayUnderConstructionStructures[3];
    static constexpr unsigned arrayUnderConstructionIndex(IndexingType indexingType) { return ((indexingType & IndexingShapeMask) - Int32Shape) / 2; }
    static constexpr ptrdiff_t offsetOfArrayUnderConstructionHeaders() { return OBJECT_OFFSETOF(Instance, arrayUnderConstructionHeaders); }
    JS_EXPORT_PRIVATE bool isArrayUnderConstruction(JSCell*) const;
    Structure* arrayUnderConstructionStructure(IndexingType);
    void* typedCastTransitions;
    static ptrdiff_t offsetOfNewArrayHeader(IndexingType indexingType)
    {
        switch (indexingType) {
        case ArrayWithInt32:
            return negativeOffsetOf(OBJECT_OFFSETOF(NegativeOffsetFields, newArrayWithInt32Header));
        case ArrayWithDouble:
            return negativeOffsetOf(OBJECT_OFFSETOF(NegativeOffsetFields, newArrayWithDoubleHeader));
        case ArrayWithTypedContiguous:
            return OBJECT_OFFSETOF(Instance, newArrayWithTypedContiguousHeader);
        default:
            RELEASE_ASSERT(indexingType == ArrayWithContiguous);
            return negativeOffsetOf(OBJECT_OFFSETOF(NegativeOffsetFields, newArrayWithContiguousHeader));
        }
    }
    static constexpr ptrdiff_t offsetOfNewArrayWithInt32StructureID() { return negativeOffsetOf(OBJECT_OFFSETOF(NegativeOffsetFields, newArrayWithInt32Header)); }
    static constexpr ptrdiff_t offsetOfNewArrayWithContiguousStructureID() { return negativeOffsetOf(OBJECT_OFFSETOF(NegativeOffsetFields, newArrayWithContiguousHeader)); }
    static constexpr ptrdiff_t offsetOfNewCopyOnWriteArrayHeader(IndexingType indexingType)
    {
        if ((indexingType & IndexingShapeMask) == ContiguousShape)
            return negativeOffsetOf(OBJECT_OFFSETOF(NegativeOffsetFields, newCopyOnWriteArrayWithContiguousHeader));
        return OBJECT_OFFSETOF(Instance, newCopyOnWriteArrayHeaders) + ((indexingType & IndexingShapeMask) - Int32Shape) / 2 * sizeof(uint64_t);
    }
    static constexpr ptrdiff_t offsetOfStringHeader() { return negativeOffsetOf(OBJECT_OFFSETOF(NegativeOffsetFields, stringHeader)); }
    uint32_t arraysHaveNoIsConcatSpreadable;
    uint32_t arraysHaveNoInheritedElements;
    uint64_t iteratorResultHeader;
    uint32_t generatorStepStatus;
    void* generatorTemplates;
    JSCell* iteratorSymbol;
    struct IteratorResultShape {
        uint32_t structureID;
        uint16_t offsetOfDone;
        uint16_t offsetOfValue;
    };
    static constexpr unsigned numberOfIteratorResultShapes = 2;
    IteratorResultShape iteratorResultShapes[numberOfIteratorResultShapes];
    uint32_t nextIteratorResultShape;
    static constexpr ptrdiff_t offsetOfIteratorResultShapes() { return OBJECT_OFFSETOF(Instance, iteratorResultShapes); }
    static constexpr ptrdiff_t offsetOfIteratorSymbol() { return OBJECT_OFFSETOF(Instance, iteratorSymbol); }
    static constexpr ptrdiff_t offsetOfGeneratorTemplates() { return OBJECT_OFFSETOF(Instance, generatorTemplates); }
    static constexpr ptrdiff_t offsetOfGeneratorStepStatus() { return OBJECT_OFFSETOF(Instance, generatorStepStatus); }
    static constexpr ptrdiff_t offsetOfIteratorResultHeader() { return OBJECT_OFFSETOF(Instance, iteratorResultHeader); }
    static constexpr ptrdiff_t offsetOfIteratorResultStructureID() { return OBJECT_OFFSETOF(Instance, iteratorResultHeader); }
#define AOT_COUNT_BUFFER_ACCESSOR(...) + 1
    static constexpr unsigned numberOfBufferAccessors = 0 FOR_EACH_AOT_TYPED_BUFFER_ACCESSOR(AOT_COUNT_BUFFER_ACCESSOR);
#undef AOT_COUNT_BUFFER_ACCESSOR
    static constexpr uint32_t bufferAccessorLookupDisabled = 1;
    uint32_t bufferAccessorStructureIDs[numberOfBufferAccessors];
    static constexpr uint8_t maxBufferAccessorAttempts = 16;
    uint8_t bufferAccessorAttempts[numberOfBufferAccessors];
    JSCell* bufferAccessors[numberOfBufferAccessors];
    Structure* bufferStructure;
    JSObject* bufferAccessorHolder;
    static constexpr ptrdiff_t offsetOfBufferAccessorStructureID(unsigned row) { return OBJECT_OFFSETOF(Instance, bufferAccessorStructureIDs) + row * sizeof(uint32_t); }
    static constexpr ptrdiff_t offsetOfBufferAccessor(unsigned row) { return OBJECT_OFFSETOF(Instance, bufferAccessors) + row * sizeof(JSCell*); }
    void watchBufferAccessor(unsigned row, const ObjectPropertyCondition&);
    uint32_t asyncLocalStorageStructureIDs[2];
    uint32_t asyncContextFrameStructureID;
    uint32_t asyncLocalStorageDefaultValueOffset;
    Structure* asyncLocalStorageStructures[2];
    Structure* asyncContextFrameStructure;
    static constexpr ptrdiff_t offsetOfAsyncLocalStorageStructureID(unsigned which) { return OBJECT_OFFSETOF(Instance, asyncLocalStorageStructureIDs) + which * sizeof(uint32_t); }
    static constexpr ptrdiff_t offsetOfAsyncContextFrameStructureID() { return OBJECT_OFFSETOF(Instance, asyncContextFrameStructureID); }
    static constexpr ptrdiff_t offsetOfAsyncLocalStorageDefaultValueOffset() { return OBJECT_OFFSETOF(Instance, asyncLocalStorageDefaultValueOffset); }
    void registerAsyncLocalStorage(JSObject* storage, JSObject* frame, JSCell* getStore);
    void* auxiliarySpace;
    void* cellSpace;
    const void* knownCompactTableGrowths;
    JSCell** youngRootsCursor;
    JSCell** youngRootsEnd;
    Latin1Character* jsonBufferStart;
    Latin1Character* jsonBufferEnd;
    EncodedJSValue preparedJSONValue;
    StringImpl* preparedJSONString;
    Vector<JSONWideSegment>* jsonWideSegments;
    JSONPlanCache* jsonPlanCache;
    FormatPatternState* formatPatternState;
    uint32_t jsonWideSegmentCount;
    uint32_t jsonParserGaveUp;
    JSONSourceEncoding jsonSourceEncoding;
    FormatPatternsOrigin formatPatternsOrigin;
    uint32_t jsonBufferCapacityHint;
    const char16_t* jsonSource16Bit;
    uint8_t* jsonSourceStart;
    const uint8_t* jsonSourceEnd;
    size_t jsonSourceCapacity;
    uint64_t jsonCursor;
    double jsonNumber;
    uint64_t jsonParserFrame;
    Latin1Character* stringAppenderOrigin;
    uint64_t stringAppenderLength;
    uint64_t stringAppenderLimit;
    uint64_t stringAppenderOwner;
    uint64_t* stringAppenderStartsTop;
    uint64_t* stringAppenderStartsEnd;
    StringAppender* stringAppender;
    static constexpr ptrdiff_t offsetOfYoungRootsCursor() { return OBJECT_OFFSETOF(Instance, youngRootsCursor); }
    static constexpr ptrdiff_t offsetOfYoungRootsEnd() { return OBJECT_OFFSETOF(Instance, youngRootsEnd); }
    static constexpr ptrdiff_t offsetOfJSONBufferStart() { return OBJECT_OFFSETOF(Instance, jsonBufferStart); }
    static constexpr ptrdiff_t offsetOfJSONBufferEnd() { return OBJECT_OFFSETOF(Instance, jsonBufferEnd); }
    static constexpr ptrdiff_t offsetOfJSONParserGaveUp() { return OBJECT_OFFSETOF(Instance, jsonParserGaveUp); }
    static constexpr ptrdiff_t offsetOfJSONSourceEnd() { return OBJECT_OFFSETOF(Instance, jsonSourceEnd); }
    static constexpr ptrdiff_t offsetOfJSONCursor() { return OBJECT_OFFSETOF(Instance, jsonCursor); }
    static constexpr ptrdiff_t offsetOfJSONNumber() { return OBJECT_OFFSETOF(Instance, jsonNumber); }
    static constexpr ptrdiff_t offsetOfJSONParserFrame() { return OBJECT_OFFSETOF(Instance, jsonParserFrame); }
    static constexpr ptrdiff_t offsetOfStringAppenderLength() { return OBJECT_OFFSETOF(Instance, stringAppenderLength); }
    static constexpr ptrdiff_t offsetOfStringAppenderOwner() { return OBJECT_OFFSETOF(Instance, stringAppenderOwner); }
    static constexpr ptrdiff_t offsetOfStringAppenderStartsTop() { return OBJECT_OFFSETOF(Instance, stringAppenderStartsTop); }
    static constexpr ptrdiff_t offsetOfStringAppenderStartsEnd() { return OBJECT_OFFSETOF(Instance, stringAppenderStartsEnd); }
    static constexpr ptrdiff_t offsetOfBarrierThreshold() { return OBJECT_OFFSETOF(Instance, barrierThreshold); }
    static constexpr ptrdiff_t offsetOfMutatorShouldBeFenced() { return OBJECT_OFFSETOF(Instance, mutatorShouldBeFenced); }
    double cachedIntegerDoubles[maxCachedIntegerDouble - minCachedIntegerDouble + 1];
    uint64_t otherCachedDoubles[std::size(otherCachedDoubleBits)];
    JSCell* sentinelString;
    uint32_t compactTableStructureID;
    static constexpr ptrdiff_t offsetOfAuxiliarySpace() { return OBJECT_OFFSETOF(Instance, auxiliarySpace); }
    static constexpr ptrdiff_t offsetOfCellSpace() { return OBJECT_OFFSETOF(Instance, cellSpace); }
    static constexpr ptrdiff_t offsetOfKnownCompactTableGrowths() { return OBJECT_OFFSETOF(Instance, knownCompactTableGrowths); }
    static constexpr ptrdiff_t offsetOfCompactTableStructureID() { return OBJECT_OFFSETOF(Instance, compactTableStructureID); }
    static constexpr ptrdiff_t offsetOfArrayAllocator() { return negativeOffsetOf(OBJECT_OFFSETOF(NegativeOffsetFields, arrayAllocator)); }
    static constexpr ptrdiff_t offsetOfRopeStringAllocator() { return negativeOffsetOf(OBJECT_OFFSETOF(NegativeOffsetFields, ropeStringAllocator)); }
    static constexpr ptrdiff_t offsetOfSingleCharacterStrings() { return OBJECT_OFFSETOF(Instance, singleCharacterStrings); }
    static constexpr ptrdiff_t offsetOfEmptyString() { return negativeOffsetOf(OBJECT_OFFSETOF(NegativeOffsetFields, emptyString)); }
    static constexpr ptrdiff_t offsetOfPowersOfTen() { return OBJECT_OFFSETOF(Instance, powersOfTen); }
    static constexpr ptrdiff_t offsetOfStringStructureID() { return negativeOffsetOf(OBJECT_OFFSETOF(NegativeOffsetFields, stringHeader)); }
    const void* smallIntStrings;
    const void* intStrings;
    static constexpr ptrdiff_t offsetOfSmallIntStrings() { return OBJECT_OFFSETOF(Instance, smallIntStrings); }
    static constexpr ptrdiff_t offsetOfIntStrings() { return OBJECT_OFFSETOF(Instance, intStrings); }
    uint64_t hashSecrets[2];
    static constexpr ptrdiff_t offsetOfHashSecrets() { return OBJECT_OFFSETOF(Instance, hashSecrets); }
    enum class InlineAllocation : unsigned { Promise, Generator, AsyncFunctionGenerator, Map, Set, BoundFunction, BigInt, DataPropertyDescriptor, RegExpObject, Arguments, GetterSetter };
    static constexpr unsigned numberOfInlineAllocations = 11;
    struct InlineAllocationData {
        void* allocator;
        uint64_t header;
    };
    InlineAllocationData inlineAllocations[numberOfInlineAllocations];
    JSObject* nullGetterFunction;
    JSObject* nullSetterFunction;
    static constexpr ptrdiff_t offsetOfNullGetterFunction() { return OBJECT_OFFSETOF(Instance, nullGetterFunction); }
    static constexpr ptrdiff_t offsetOfNullSetterFunction() { return OBJECT_OFFSETOF(Instance, nullSetterFunction); }
    struct TypedArrayAllocation {
        void* allocator;
        uint64_t header;
        uint64_t elementSize;
        uint64_t unused;
    };
    static_assert(sizeof(TypedArrayAllocation) == 32);
    static constexpr unsigned numberOfTypedArrayAllocations = 16;
    TypedArrayAllocation typedArrayAllocations[numberOfTypedArrayAllocations];
    void* primitiveAuxiliarySpace;
    static constexpr ptrdiff_t offsetOfTypedArrayAllocations() { return OBJECT_OFFSETOF(Instance, typedArrayAllocations); }
    static constexpr ptrdiff_t offsetOfPrimitiveAuxiliarySpace() { return OBJECT_OFFSETOF(Instance, primitiveAuxiliarySpace); }
    static constexpr ptrdiff_t offsetOfAllocatorFor(InlineAllocation kind) { return OBJECT_OFFSETOF(Instance, inlineAllocations) + static_cast<unsigned>(kind) * sizeof(InlineAllocationData) + OBJECT_OFFSETOF(InlineAllocationData, allocator); }
    static constexpr ptrdiff_t offsetOfHeaderFor(InlineAllocation kind) { return OBJECT_OFFSETOF(Instance, inlineAllocations) + static_cast<unsigned>(kind) * sizeof(InlineAllocationData) + OBJECT_OFFSETOF(InlineAllocationData, header); }
    JS_EXPORT_PRIVATE void didHaveBadTime();
    static constexpr unsigned numberOfOwnPropertyResults = 512;
    static constexpr uint32_t maxNameWithOwnPropertyResult = INT32_MAX;
    static constexpr uint32_t ownPropertyResultSalt(uint32_t name) { return name * 0x9e3779b1u >> 16; }
    static constexpr unsigned ownPropertyResultIndex(uint32_t structureID, uint32_t name) { return ((structureID >> 4) ^ ownPropertyResultSalt(name)) & (numberOfOwnPropertyResults - 1); }
    uint64_t ownPropertyResults[numberOfOwnPropertyResults];
    bool hasOwnPropertyResults;
    static constexpr ptrdiff_t offsetOfOwnPropertyResults() { return OBJECT_OFFSETOF(Instance, ownPropertyResults); }
    static constexpr unsigned ownPropertyResultWays = 4;
    void cacheOwnPropertyResult(uint32_t structureID, uint32_t name, bool answer)
    {
        unsigned index = ownPropertyResultIndex(structureID, name);
        uint64_t pair = structureID | static_cast<uint64_t>(name) << 32;
        unsigned place = ownPropertyResultWays;
        for (unsigned step = ownPropertyResultWays; step--;) {
            uint64_t noted = ownPropertyResults[index ^ step];
            if (!noted || noted << 1 == pair << 1)
                place = step;
        }
        if (place == ownPropertyResultWays) {
            keyedTableRandomState = keyedTableRandomState * 1664525u + 1013904223u;
            place = keyedTableRandomState >> 30;
        }
        ownPropertyResults[index ^ place] = pair | static_cast<uint64_t>(answer) << 63;
        hasOwnPropertyResults = true;
        hasNewOwnPropertyResults = true;
    }
    struct OwnPropertyLocation {
        static constexpr uint32_t isPresent = 1;
        static constexpr uint32_t attributes = 2 | 4 | 8;
        static constexpr uint32_t isOutOfLine = 16;
        static constexpr uint32_t isNotValue = 32;
        static constexpr uint32_t isCompiledField = 64;
        static constexpr unsigned byteShift = 8;
        static constexpr unsigned maskShiftShift = 11;
        static constexpr unsigned markerShift = 17;
        static constexpr unsigned wordShift = 19;
        static constexpr unsigned displacementShift = 8;

        UniquedStringImpl* uid;
        uint32_t structureID;
        uint32_t place;
    };
    static constexpr unsigned numberOfOwnPropertyLocations = 512;
    static constexpr unsigned ownPropertyLocationWays = 4;
    static constexpr unsigned ownPropertyLocationIndex(uint32_t structureID, uintptr_t uid) { return ((uid + 5 * static_cast<uintptr_t>(structureID)) >> 4) & (numberOfOwnPropertyLocations - 1); }
    OwnPropertyLocation ownPropertyLocations[numberOfOwnPropertyLocations];
    bool hasOwnPropertyLocations;
    static constexpr ptrdiff_t offsetOfOwnPropertyLocations() { return OBJECT_OFFSETOF(Instance, ownPropertyLocations); }
    void cacheOwnPropertyLocation(uint32_t structureID, UniquedStringImpl* uid, uint32_t place);
    struct ObjectCreateCacheEntry {
        JSCell* prototype;
        uint64_t header;
    };
    static constexpr unsigned objectCreateCacheSize = 64;
    static constexpr unsigned objectCreateCacheIndex(uintptr_t prototype) { return (prototype >> 6) & (objectCreateCacheSize - 1); }
    ObjectCreateCacheEntry objectCreateCache[objectCreateCacheSize];
    bool objectCreateCacheIsDirty;
    static constexpr unsigned numberOfCheckedStructures = 32;
    uint32_t checkedStructureIDs[numberOfCheckedStructures];
    bool hasCheckedStructures;
    void forgetCheckedStructures()
    {
        if (std::exchange(hasCheckedStructures, false))
            zeroSpan(std::span { checkedStructureIDs });
    }
    bool watchProperty(JSObject* holder, UniquedStringImpl*);
    struct PrototypeChangeCacheEntry {
        JSCell* prototype;
        uint32_t from;
        uint32_t to;
    };
    static constexpr unsigned prototypeChangeCacheSize = 16;
    static constexpr unsigned prototypeChangeCacheIndex(uintptr_t prototype) { return (prototype >> 6) & (prototypeChangeCacheSize - 1); }
    PrototypeChangeCacheEntry prototypeChangeCache[prototypeChangeCacheSize];
    bool prototypeChangeCacheIsDirty;
    void forgetPrototypeChanges()
    {
        if (std::exchange(prototypeChangeCacheIsDirty, false))
            zeroSpan(std::span { prototypeChangeCache });
    }
    static constexpr unsigned numberOfThisAllocations = 64;
    static constexpr unsigned slotsPerThisAllocation = 4;
    static_assert(hasOneBitSet(numberOfThisAllocations) && hasOneBitSet(slotsPerThisAllocation * sizeof(Slot)));
    ZeroedStorage<Slot[numberOfThisAllocations * slotsPerThisAllocation]> thisAllocations;
    bool hasThisAllocations;
    struct TypedThisAllocation {
        JSCell* newTarget;
        JSCell* constructor;
        void* allocator;
        const void* body;
        uint64_t header;
        uint32_t epoch;
        uint32_t bodySize;
        uint64_t padding[2];
    };
    static constexpr unsigned numberOfTypedThisAllocations = 16;
    static_assert(sizeof(TypedThisAllocation) == 64);
    ZeroedStorage<TypedThisAllocation[numberOfTypedThisAllocations]> typedThisAllocations;
    bool hasTypedThisAllocations;
    static constexpr ptrdiff_t offsetOfTypedThisAllocations() { return OBJECT_OFFSETOF(Instance, typedThisAllocations); }
    void forgetTypedThisAllocations()
    {
        if (std::exchange(hasTypedThisAllocations, false))
            zeroSpan(std::span { typedThisAllocations.get() });
    }
    static constexpr uint16_t maxSymbolKeyedRuns = 256;
    static constexpr uint16_t maxCopiedNameRuns = 1024;
    uint16_t remainingSymbolKeyedRuns;
    uint16_t remainingCopiedNameRuns;
    PropertyRunBudget reservedPropertyRunBudget;
    Slot* thisAllocationFor(const JSCell* newTarget) { return &thisAllocations.get()[(std::bit_cast<uintptr_t>(newTarget) >> 4 & (numberOfThisAllocations - 1)) * slotsPerThisAllocation]; }
    static constexpr ptrdiff_t offsetOfThisAllocations() { return OBJECT_OFFSETOF(Instance, thisAllocations); }
    Structure* emptyLiteralStructures[16];
    static constexpr ptrdiff_t offsetOfObjectCreateCache() { return OBJECT_OFFSETOF(Instance, objectCreateCache); }
    static constexpr ptrdiff_t offsetOfCheckedStructureID(unsigned index) { return OBJECT_OFFSETOF(Instance, checkedStructureIDs) + index * sizeof(uint32_t); }
    static constexpr ptrdiff_t offsetOfPrototypeChangeCache() { return OBJECT_OFFSETOF(Instance, prototypeChangeCache); }
    struct CustomGetter {
        uint32_t structureID;
        uint32_t serial;
        uint16_t epoch;
        UniquedStringImpl* uid;
        void* getter;
        JSObject* passedForReceiver;
        JSGlobalObject* globalObject;
        uint32_t lastSerial;
        uint32_t offsetOfCachedValue;
        uint32_t padding[2];
    };
    static_assert(sizeof(CustomGetter) == 64);
    static constexpr unsigned numberOfCustomGetters = 256;
    static_assert(numberOfCustomGetters * sizeof(CustomGetter) < 1ull << (64 - Slot::customGetterIndexShift));
    CustomGetter* customGetters;
    static constexpr ptrdiff_t offsetOfCustomGetters() { return OBJECT_OFFSETOF(Instance, customGetters); }
    static_assert(numberOfCustomGetters == 1u << 8);
    static unsigned indexOfCustomGetter(uint32_t structureID, UniquedStringImpl* uid) { return ((structureID >> 4) ^ static_cast<uint32_t>(std::bit_cast<uintptr_t>(uid) >> 4) * 0x85ebca6bu) * 0x9e3779b1u >> (32 - 8); }
    static constexpr unsigned customGetterWays = 4;
    const CustomGetter* customGetterFor(uint32_t structureID, UniquedStringImpl* uid) const
    {
        if (!customGetters)
            return nullptr;
        unsigned first = indexOfCustomGetter(structureID, uid);
        for (unsigned i = 0; i < customGetterWays; ++i) {
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
        for (unsigned i = 0; i < customGetterWays; ++i) {
            auto& record = customGetters[(first + i) % numberOfCustomGetters];
            if (record.structureID == structureID && record.uid == uid)
                return record;
            if (!empty && !record.serial)
                empty = &record;
        }
        if (empty)
            return *empty;
        keyedTableRandomState = keyedTableRandomState * 1664525u + 1013904223u;
        static_assert(customGetterWays == 4);
        return customGetters[(first + (keyedTableRandomState >> 30)) % numberOfCustomGetters];
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
    PropertyRun* propertyRuns;
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
    InheritedSetter inheritedSetters[numberOfInheritedSetters];
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
    uint32_t hasCachedNativeSetter;
    static constexpr ptrdiff_t offsetOfHasCachedNativeSetter() { return OBJECT_OFFSETOF(Instance, hasCachedNativeSetter); }
    struct CaseConversion {
        JSCell* source;
        JSCell* result;
    };
    static_assert(sizeof(CaseConversion) == 16);
    static constexpr unsigned numberOfCaseConversions = 64;
    static_assert(hasOneBitSet(numberOfCaseConversions));
    CaseConversion caseConversions[2][numberOfCaseConversions];
    bool hasCaseConversions;
    struct SpreadCopyCacheEntry {
        uint64_t more;
        uint64_t header;
        void* allocator;
        uint32_t structureID;
        uint8_t copiedWordCount;
        uint8_t clearedWordCount;
        uint8_t places[2];
    };
    static_assert(sizeof(SpreadCopyCacheEntry) == 32);
    static constexpr unsigned spreadCopyCacheSize = 512;
    static_assert(hasOneBitSet(spreadCopyCacheSize));
    SpreadCopyCacheEntry* spreadCopyCache;
    bool spreadCopyCacheIsDirty;
    struct CopyKeyOrderEntry {
        uint64_t sourceHeaderWords;
        const Slot* site;
        uint32_t orderWordAndSource;
        uint32_t unused[3];
    };
    static_assert(sizeof(CopyKeyOrderEntry) == 32);
    static constexpr unsigned copyKeyOrderCacheSize = 512;
    static constexpr unsigned copyKeyOrderHashShift = 32 - 9;
    static_assert(copyKeyOrderCacheSize == 1u << (32 - copyKeyOrderHashShift));
    static constexpr unsigned copyKeyOrderSiteShift = 4;
    static constexpr uint32_t copyKeyOrderFromSecondSource = 1u << 31;
    static constexpr uint32_t copyAlwaysFails = 1u << 30;
    CopyKeyOrderEntry* copyKeyOrderCache;
    bool copyKeyOrderCacheIsDirty;
    static constexpr ptrdiff_t offsetOfCopyKeyOrderCache() { return OBJECT_OFFSETOF(Instance, copyKeyOrderCache); }
    void cacheCopyKeyOrder(uint64_t sourceHeaderWords, const Slot* site, uint32_t orderWordAndSource)
    {
        uint32_t mixed = static_cast<uint32_t>(sourceHeaderWords) ^ static_cast<uint32_t>(sourceHeaderWords >> 32) ^ static_cast<uint32_t>(std::bit_cast<uintptr_t>(site) >> copyKeyOrderSiteShift);
        copyKeyOrderCache[mixed * spreadCopyHashMultiplier >> copyKeyOrderHashShift] = { sourceHeaderWords, site, orderWordAndSource, { } };
        copyKeyOrderCacheIsDirty = true;
    }
    static constexpr unsigned proxyTrapKeyCacheSize = 256;
    JSString* proxyTrapKeyCache[proxyTrapKeyCacheSize];
    bool proxyTrapKeyCacheIsDirty;
    JS_EXPORT_PRIVATE JSString* proxyTrapKey(VM&, UniquedStringImpl*);
    static constexpr ptrdiff_t offsetOfSpreadCopyCache() { return OBJECT_OFFSETOF(Instance, spreadCopyCache); }
    static constexpr unsigned secondAddedNameShift = 24;
    static constexpr uint32_t spreadCopyHashMultiplier = 0x9e3779b1u;
    static constexpr unsigned spreadCopyHashShift = 32 - 8;
    static_assert(spreadCopyCacheSize == 2u << (32 - spreadCopyHashShift));
    static unsigned spreadCopyCacheIndex(uint32_t structureID, uint64_t more)
    {
        uint32_t names = (static_cast<uint32_t>(more) ^ static_cast<uint32_t>(more >> secondAddedNameShift)) * spreadCopyHashMultiplier;
        return ((structureID ^ names) * spreadCopyHashMultiplier >> spreadCopyHashShift) * 2;
    }
    static constexpr ptrdiff_t offsetOfCaseConversions(bool isUpper) { return OBJECT_OFFSETOF(Instance, caseConversions) + isUpper * numberOfCaseConversions * sizeof(CaseConversion); }
    void cacheCaseConversion(JSCell* source, JSCell* result, bool isUpper)
    {
        caseConversions[isUpper][(std::bit_cast<uintptr_t>(source) / sizeof(CaseConversion)) % numberOfCaseConversions] = { source, result };
        hasCaseConversions = true;
    }
    static constexpr unsigned numberOfKeyedReads = 2048;
    static_assert(hasOneBitSet(numberOfKeyedReads));
    static constexpr uint32_t keyMayNotBeEvicted = 1u << Slot::offsetBits;
    static_assert(!(keyMayNotBeEvicted & (Slot::offsetMask | Slot::flagsMask)));
    static constexpr unsigned keyedStructureShift = 6;
    static_assert(sizeof(Structure) >= 1 << keyedStructureShift);
    static constexpr unsigned keyedKeyShift = 4;
    struct KeyedRead {
        Slot slot;
        JSCell* holder;
        uint32_t epoch;
        uint32_t unused;
    };
    static_assert(sizeof(KeyedRead) == 32);
    static constexpr uint32_t keyedStructureHashMultiplier = 0x9e3779b1;
    static constexpr unsigned keyedHashShift = 16;
    uint32_t keyedTableRandomState;
    int32_t keyedReadsByNameUntilCacheAttempt;
    static constexpr ptrdiff_t offsetOfKeyedReadsByNameUntilCacheAttempt() { return OBJECT_OFFSETOF(Instance, keyedReadsByNameUntilCacheAttempt); }
    const JSCell* lastAtomizedKey;
    static constexpr unsigned keyedTableWays = 4;
    alignas(16) std::array<uint8_t, 32> keyedScratchEntry;
    template<typename Entry>
    ALWAYS_INLINE Entry& keyedEntryFor(Entry* table, unsigned numberOfEntries, StructureID structureID, const JSCell* key)
    {
        static_assert(sizeof(Entry) <= sizeof(keyedScratchEntry));
        Entry* place = allocateKeyedEntry(table, numberOfEntries, structureID, key);
        return place ? *place : *std::bit_cast<Entry*>(keyedScratchEntry.data());
    }
    static constexpr size_t keyedEntryGroupBytes = 256;
    void didUseKeyedEntry(const void*);
    template<typename Entry>
    ALWAYS_INLINE Entry* allocateKeyedEntry(Entry* table, unsigned numberOfEntries, StructureID structureID, const JSCell* key, bool mayEvict = true)
    {
        Entry* place = allocateKeyedEntrySlow(table, numberOfEntries, structureID, key, mayEvict);
        if (place)
            didUseKeyedEntry(place);
        return place;
    }
    template<typename Entry>
    ALWAYS_INLINE Entry* allocateKeyedEntrySlow(Entry* table, unsigned numberOfEntries, StructureID structureID, const JSCell* key, bool mayEvict = true)
    {
        constexpr unsigned shift = std::countr_zero(sizeof(Entry));
        static_assert(hasOneBitSet(sizeof(Entry)) && shift >= keyedKeyShift && shift <= keyedStructureShift);
        uint32_t offset = (((structureID.bits() * keyedStructureHashMultiplier) >> (keyedHashShift - shift)) + (static_cast<uint32_t>(std::bit_cast<uintptr_t>(key)) << (shift - keyedKeyShift))) & ((numberOfEntries - 1) << shift);
        auto at = [&](unsigned place) -> Entry& {
            return *std::bit_cast<Entry*>(std::bit_cast<uint8_t*>(table) + (offset ^ static_cast<uint32_t>(place * sizeof(Entry))));
        };
        auto slotAt = [&](unsigned place) -> Slot& {
            return *std::bit_cast<Slot*>(&at(place));
        };
        auto matchesPair = [&](const Slot& slot) {
            return slot.structureID == structureID && slot.pointer == key;
        };
        if (!slotAt(0).structureID || matchesPair(slotAt(0)))
            return &at(0);
        keyedTableRandomState = keyedTableRandomState * 1664525u + 1013904223u;
        if (mayEvict && keyedTableRandomState >> 30)
            return nullptr;
        Entry* firstFree = nullptr;
        for (unsigned place = 1; place < keyedTableWays; ++place) {
            if (matchesPair(slotAt(place)))
                return &at(place);
            if (!firstFree && !slotAt(place).structureID)
                firstFree = &at(place);
        }
        if (firstFree)
            return firstFree;
        if (!mayEvict || keyedTableRandomState >> 29)
            return nullptr;
        static_assert(keyedTableWays == 4);
        return &at(keyedTableRandomState >> 27 & 3);
    }
    KeyedRead* allocateKeyedRead(StructureID structureID, const JSCell* key) { return allocateKeyedEntry(keyedReads, numberOfKeyedReads, structureID, key); }
    KeyedRead& keyedReadFor(StructureID structureID, const JSCell* key) { return keyedEntryFor(keyedReads, numberOfKeyedReads, structureID, key); }
    static constexpr ptrdiff_t offsetOfKeyedReads() { return OBJECT_OFFSETOF(Instance, keyedReads); }
    static constexpr unsigned numberOfKeyedHas = 1024;
    static_assert(hasOneBitSet(numberOfKeyedHas));
    static constexpr unsigned keyedHasResultShift = 16;
    Slot& keyedHasFor(StructureID structureID, const JSCell* key) { return keyedEntryFor(keyedHas, numberOfKeyedHas, structureID, key); }
    static constexpr ptrdiff_t offsetOfKeyedHas() { return OBJECT_OFFSETOF(Instance, keyedHas); }
    static constexpr unsigned numberOfReservedDescriptorIndices = sizeof(Slot) / sizeof(uint32_t);
    uint32_t& descriptorSlotIndex(uint32_t number) { return reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(this) + structureDescriptorsDisplacement())[numberOfReservedDescriptorIndices + number]; }
    Slot& descriptorSlotAt(uint32_t place) { return *reinterpret_cast<Slot*>(reinterpret_cast<char*>(this) + structureDescriptorsDisplacement() + place); }
    Slot& row(uint32_t number) { return descriptorSlotAt(descriptorSlotIndex(number)); }
    void clearStructureDescriptorSlot(uint32_t number);
    static constexpr uint32_t structureDescriptorTypeInfoBlob = TypeInfoBlob::typeInfoBlob(NonArray, FinalObjectType, TypeInfo(FinalObjectType, JSFinalObject::StructureFlags).inlineTypeFlags());
    void fillStructureDescriptorSlot(uint32_t number, Structure*, uint32_t slotsToClear, LocalAllocator*);
    void didFillGlobalPropertyCache();
    static constexpr uint32_t keyIsAbsent = Slot::isGetter | Slot::pointerIsNotCell;
    static constexpr uint32_t keyIsCompiledField = Slot::isGetter | Slot::isIndirect;
    static constexpr uint32_t epochIsDictionary = 1u << 16;
    static constexpr uint32_t epochIsValidated = 1u << 17;
    void validateKeyedTables();
    static constexpr uint32_t keyNeedsLookup = Slot::isGetter | Slot::isIndirect | Slot::pointerIsNotCell;
    static constexpr uint32_t keyIsPermanentlyAbsent = Slot::isGetter | Slot::pointerIsNotCell | Slot::pointerIsCell;
    static constexpr uint32_t ownAccessorIsAtLocation = 1u << (Slot::offsetBits + 1);
    static constexpr uint32_t keyIsOwnAccessor = Slot::isGetter | ownAccessorIsAtLocation;
    static_assert(!(ownAccessorIsAtLocation & (Slot::offsetMask | Slot::flagsMask | keyMayNotBeEvicted)));
    static constexpr uint32_t keyedHasNeedsLookup = Slot::isGetter | Slot::pointerIsNotCell;
    static constexpr uint32_t keyedHasReadsField = Slot::isGetter | Slot::isIndirect;
    enum class KeyedHasField : uint8_t { Word32, Word64, Byte, Int32OrNumberBox, Double, Int64 };
    static constexpr unsigned keyedHasFieldKindShift = 16;
    static constexpr unsigned keyedHasFieldIndexShift = 19;
    static constexpr uint32_t maxKeyedHasFieldIndex = (Slot::isIndirect >> keyedHasFieldIndexShift) - 1;
    static constexpr size_t cannotLookUp = 2;
    static constexpr uint32_t nameIsIdentifierIndex = 1u << 31;
    struct KeyedAddition {
        Slot slot;
        StructureID newStructureID;
        uint32_t epoch;
        uint32_t newStorageCapacity;
        uint32_t holderValueCount;
    };
    static_assert(sizeof(KeyedAddition) == 32);
    static constexpr uintptr_t keyedAdditionIsGetter = 1;
    static constexpr uintptr_t keyedAdditionIsSetter = 2;
    static constexpr uintptr_t keyedAdditionKeyTagMask = keyedAdditionIsGetter | keyedAdditionIsSetter;
    static constexpr uint32_t keyedAdditionCompletesAccessorPair = 2;
    static constexpr uintptr_t keyedAdditionIsDeletion = keyedAdditionIsGetter | keyedAdditionIsSetter;
    static_assert(keyedAdditionKeyTagMask < 1u << keyedKeyShift);
    static constexpr uint32_t keyedAdditionIsExtraProperty = UINT32_MAX;
    static constexpr uint32_t keyedExtraPropertyAdditionSeenOnce = UINT32_MAX - 1;
    static constexpr unsigned keyedExtraPropertyEpochShift = 16;
    static constexpr unsigned numberOfKeyedAdditions = 1024;
    static_assert(hasOneBitSet(numberOfKeyedAdditions));
    KeyedAddition& keyedAdditionFor(StructureID structureID, const JSCell* key) { return keyedEntryFor(keyedAdditions, numberOfKeyedAdditions, structureID, key); }
    KeyedAddition* allocateKeyedAddition(StructureID structureID, const JSCell* key) { return allocateKeyedEntry(keyedAdditions, numberOfKeyedAdditions, structureID, key); }
    KeyedAddition* freeKeyedAdditionEntry(StructureID structureID, const JSCell* key) { return allocateKeyedEntry(keyedAdditions, numberOfKeyedAdditions, structureID, key, false); }
    static constexpr size_t sizeOfKeyedTables = numberOfKeyedReads * sizeof(KeyedRead) + numberOfKeyedAdditions * sizeof(KeyedAddition) + numberOfKeyedHas * sizeof(Slot);
    static_assert(!(sizeOfKeyedTables % (16 * KB)));
    static constexpr ptrdiff_t offsetOfKeyedAdditions() { return OBJECT_OFFSETOF(Instance, keyedAdditions); }
    struct NamedField {
        uint64_t shapeWordAndName;
        uint64_t access;
    };
    static constexpr unsigned numberOfNamedFields = 2048;
    static constexpr unsigned namedFieldShapeShift = 5;
    static_assert(hasOneBitSet(numberOfNamedFields) && sizeof(NamedField) == 16);
    NamedField* namedFieldReads;
    NamedField* namedFieldWrites;
    static constexpr unsigned numberOfOwnFieldPlaces = 128;
    static constexpr unsigned ownFieldIndexShift = 3;
    NamedField ownFieldPlaces[numberOfOwnFieldPlaces];
    static constexpr ptrdiff_t offsetOfOwnFieldPlaces() { return OBJECT_OFFSETOF(Instance, ownFieldPlaces); }
    void cacheOwnFieldPlace(uint32_t shapeWord, uint32_t name, KeyedHasField kind, uint32_t index)
    {
        ownFieldPlaces[((shapeWord << namedFieldShapeShift) + name) % numberOfOwnFieldPlaces] = { shapeWord | static_cast<uint64_t>(name) << 32, index << ownFieldIndexShift | static_cast<uint32_t>(kind) };
    }
    struct ImmutableElement {
        uint64_t cellAndIndex;
        EncodedJSValue value;
    };
    static constexpr unsigned immutableElementCacheSize = 1024;
    static constexpr unsigned immutableElementIndexBits = 16;
    static_assert(hasOneBitSet(immutableElementCacheSize) && sizeof(ImmutableElement) == 16);
    ImmutableElement* immutableElementCache;
    bool immutableElementCacheIsDirty;
    static constexpr ptrdiff_t offsetOfImmutableElementCache() { return OBJECT_OFFSETOF(Instance, immutableElementCache); }
    void cacheImmutableElement(JSCell* cell, uint32_t index, JSValue value)
    {
        uintptr_t bits = std::bit_cast<uintptr_t>(cell);
        uint64_t cellAndIndex = static_cast<uint64_t>(bits) << immutableElementIndexBits | index;
        unsigned first = ((bits >> 4) + index) & (immutableElementCacheSize - 1);
        unsigned second = first ^ immutableElementCacheSize / 2;
        auto canBeEvicted = [&](unsigned place) {
            return !immutableElementCache[place].cellAndIndex || immutableElementCache[place].cellAndIndex == cellAndIndex;
        };
        unsigned place = canBeEvicted(first) ? first : second;
        if (!canBeEvicted(place)) {
            keyedTableRandomState = keyedTableRandomState * 1664525u + 1013904223u;
            if (keyedTableRandomState >> 30)
                return;
            place = keyedTableRandomState >> 29 & 1 ? first : second;
        }
        immutableElementCache[place] = { cellAndIndex, JSValue::encode(value) };
        immutableElementCacheIsDirty = true;
    }
    EncodedJSValue thrownValue;
    void* typedKnownStoreTransitions;
    void* typedExtraPropertiesHandleSegments;
    void* typedExtraPropertyLocations;
    void* typedLateFieldCreations;
    void* typedCloneCache;
    void* typedConversionCache;
    static constexpr ptrdiff_t offsetOfNamedFieldReads() { return OBJECT_OFFSETOF(Instance, namedFieldReads); }
    static constexpr ptrdiff_t offsetOfNamedFieldWrites() { return OBJECT_OFFSETOF(Instance, namedFieldWrites); }
    static constexpr ptrdiff_t offsetOfTypedKnownStoreTransitions() { return OBJECT_OFFSETOF(Instance, typedKnownStoreTransitions); }
    static constexpr ptrdiff_t offsetOfTypedExtraPropertiesHandleSegments() { return OBJECT_OFFSETOF(Instance, typedExtraPropertiesHandleSegments); }
    static constexpr ptrdiff_t offsetOfTypedExtraPropertyLocations() { return OBJECT_OFFSETOF(Instance, typedExtraPropertyLocations); }
    static constexpr ptrdiff_t offsetOfTypedLateFieldCreations() { return OBJECT_OFFSETOF(Instance, typedLateFieldCreations); }
    static constexpr ptrdiff_t offsetOfTypedCloneCache() { return OBJECT_OFFSETOF(Instance, typedCloneCache); }
    static constexpr ptrdiff_t offsetOfTypedConversionCache() { return OBJECT_OFFSETOF(Instance, typedConversionCache); }
    static void cacheNamedField(NamedField* table, uint32_t shapeWord, uint32_t name, uint64_t access)
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
    struct StackTraceInfo {
        enum Kind : uint32_t { NeedsResolution, IsVisible, IsHidden, IsInInlinedBody };
        uint32_t typedFunction { 0 };
        uint32_t callSiteBits : 30 { 0 };
        uint32_t kind : 2 { NeedsResolution };
        static constexpr unsigned numberOfBitsWithConstructedCallee = 27;
        static constexpr uint32_t hasConstructedCallee = 1u << 29;
        bool calleeIsConstructed() const { return (kind == NeedsResolution || kind == IsInInlinedBody) && callSiteBits & hasConstructedCallee; }
        StackTraceInfo withConstructedCallee() const
        {
            StackTraceInfo result;
            result.typedFunction = typedFunction;
            result.callSiteBits = callSiteBits | kind << numberOfBitsWithConstructedCallee | hasConstructedCallee;
            result.kind = kind == IsInInlinedBody ? IsInInlinedBody : NeedsResolution;
            return result;
        }
        StackTraceInfo withoutConstructedCallee() const
        {
            StackTraceInfo result;
            result.typedFunction = typedFunction;
            result.callSiteBits = callSiteBits & ((1u << numberOfBitsWithConstructedCallee) - 1);
            result.kind = callSiteBits >> numberOfBitsWithConstructedCallee & 3;
            return result;
        }
        StackTraceInfo forSite() const { return calleeIsConstructed() ? withoutConstructedCallee() : *this; }
    };
    struct UnwindInfo {
        enum State : uint8_t { NotComputed, HasNoHandler, NotUnwindable, HasHandler, HasHandlerTakingException, HasHandlerIgnoringValue, RejectsPromise };
        union {
            uint32_t savedRegisters { 0 };
            uint32_t handlerOffsetInImage;
        };
        uint16_t savedRegistersStart { 0 };
        State state { NotComputed };
        bool siteIsInVisibleFunction { false };
    };
    static_assert(sizeof(StackTraceInfo) == 8 && sizeof(UnwindInfo) == 8);
    static constexpr unsigned numberOfCachedAddressInfos = 512;
    CachedAddressInfo cachedAddressInfos[numberOfCachedAddressInfos];
    CachedAddressInfo& cachedAddressInfo(const void* address) { return cachedAddressInfos[(std::bit_cast<uintptr_t>(address) >> 2) % numberOfCachedAddressInfos]; }
    CachedAddressInfo* cachedReturnAddresses;
    JS_EXPORT_PRIVATE CachedAddressInfo* allocateCachedReturnAddresses();
    CachedAddressInfo* ensureCachedReturnAddresses() { return cachedReturnAddresses ? cachedReturnAddresses : allocateCachedReturnAddresses(); }
    static size_t primaryReturnAddressIndex(const void* address) { return (std::bit_cast<uintptr_t>(address) >> 2) % numberOfCachedAddressInfos; }
    static size_t secondaryReturnAddressIndex(const void* address)
    {
        static_assert(numberOfCachedAddressInfos == 1 << 9);
        return std::bit_cast<uintptr_t>(address) * 0x9e3779b97f4a7c15ull >> (64 - 9);
    }
    const CachedAddressInfo* findCachedReturnAddress(const void* address) const
    {
        const CachedAddressInfo* all = cachedReturnAddresses;
        if (!all) [[unlikely]]
            return nullptr;
        if (all[primaryReturnAddressIndex(address)].address == address) [[likely]]
            return &all[primaryReturnAddressIndex(address)];
        return all[secondaryReturnAddressIndex(address)].address == address ? &all[secondaryReturnAddressIndex(address)] : nullptr;
    }
    StackTraceInfo* stackTraceInfos() { return reinterpret_cast<StackTraceInfo*>(ensureCachedReturnAddresses() + numberOfCachedAddressInfos); }
    UnwindInfo* unwindInfos() { return reinterpret_cast<UnwindInfo*>(stackTraceInfos() + numberOfCachedAddressInfos); }
    JS_EXPORT_PRIVATE const UnwindInfo& findUnwindInfo(const CachedAddressInfo&);
    JS_EXPORT_PRIVATE bool nearestHandlerIgnoresValue(VM&, CallFrame*, void* returnPC);
    JS_EXPORT_PRIVATE const CachedAddressInfo* resolveReturnAddress(const void*);
    uint32_t operationSamplingState;
    uint32_t moduleEvaluationDepth;
    bool evictsSecondaryReturnAddress;
    bool thrownValueIsAtStackTraceItemSite;
    const CallSiteOverride* callSiteOverrides;
    static_assert(!(sizeOfKeyedTables % keyedEntryGroupBytes));
    ZeroedStorage<WTF::BitSet<sizeOfKeyedTables / keyedEntryGroupBytes>> keyedGroupsUsedInThisEntry;
    ZeroedStorage<WTF::BitSet<sizeOfKeyedTables / keyedEntryGroupBytes>> keyedGroupsUsedEarlier;
    unsigned entryCountAtLastKeyedUse;
    bool hasNewOwnPropertyResults;
    bool hasNewOwnPropertyLocations;
    static constexpr ptrdiff_t offsetOfTypedStatics() { return OBJECT_OFFSETOF(Instance, typedStatics); }
    static constexpr size_t bytesReservedForEnvironments = 1 * MB;
    static constexpr size_t bytesReservedForLoopPollPage = 16 * KB;
    static constexpr ptrdiff_t offsetOfLoopPollPage() { return -static_cast<ptrdiff_t>(bytesReservedForEnvironments); }
    static constexpr size_t granuleBelowInstance = 64 * KB;
    static constexpr size_t bytesReservedForDescriptorSlotIndices = 1 * MB;
    static constexpr size_t maxStructureDescriptors = bytesReservedForDescriptorSlotIndices / sizeof(uint32_t) - numberOfReservedDescriptorIndices;
    static constexpr size_t maxFilledStructureDescriptors = 1 << 16;
    static constexpr size_t bytesReservedForStructureDescriptors = bytesReservedForDescriptorSlotIndices + maxFilledStructureDescriptors * sizeof(Slot);
    static_assert(!(bytesReservedForStructureDescriptors % granuleBelowInstance));
    static constexpr ptrdiff_t structureDescriptorsDisplacement() { return -static_cast<ptrdiff_t>(bytesReservedForEnvironments + bytesReservedForStructureDescriptors); }
    static constexpr size_t roundedUpToGranule(size_t size) { return (size + granuleBelowInstance - 1) & ~(granuleBelowInstance - 1); }
    static constexpr ptrdiff_t typedStaticsDisplacement(size_t sizeOfStatics) { return -static_cast<ptrdiff_t>(bytesReservedForEnvironments + bytesReservedForStructureDescriptors + roundedUpToGranule(sizeOfStatics)); }
    static constexpr ptrdiff_t typedCardsDisplacement(size_t sizeOfStatics) { return typedStaticsDisplacement(sizeOfStatics) - static_cast<ptrdiff_t>(roundedUpToGranule(sizeOfStatics) >> typedCardShift); }
    static constexpr size_t bytesReservedBelowInstance(size_t sizeOfStatics) { return bytesReservedForEnvironments + bytesReservedForStructureDescriptors + roundedUpToGranule(sizeOfStatics) + roundedUpToGranule(roundedUpToGranule(sizeOfStatics) >> typedCardShift); }
    static constexpr ptrdiff_t offsetOfTypedCards() { return OBJECT_OFFSETOF(Instance, typedCards); }
    static constexpr ptrdiff_t offsetOfModuleEvaluationDepth() { return OBJECT_OFFSETOF(Instance, moduleEvaluationDepth); }
    static constexpr ptrdiff_t offsetOfThrownValue() { return OBJECT_OFFSETOF(Instance, thrownValue); }
    static constexpr ptrdiff_t offsetOfTypedAllocationIndices() { return OBJECT_OFFSETOF(Instance, typedAllocationIndices); }
    static constexpr ptrdiff_t offsetOfTypedModifiedClassObjects() { return OBJECT_OFFSETOF(Instance, typedModifiedClassObjects); }
    static constexpr ptrdiff_t offsetOfTypedClassPrototypes() { return OBJECT_OFFSETOF(Instance, typedClassPrototypes); }
    static constexpr ptrdiff_t offsetOfTypedShapeSets() { return OBJECT_OFFSETOF(Instance, typedShapeSets); }
    static constexpr ptrdiff_t offsetOfTypedCastTransitions() { return OBJECT_OFFSETOF(Instance, typedCastTransitions); }
    static constexpr ptrdiff_t offsetOfTypedClosureStructureID(unsigned which = 0) { return which ? static_cast<ptrdiff_t>(OBJECT_OFFSETOF(Instance, typedClosureStructureIDs) + (which - 1) * sizeof(uint32_t)) : negativeOffsetOf(OBJECT_OFFSETOF(NegativeOffsetFields, typedClosureStructureID)); }
    static constexpr ptrdiff_t offsetOfTypedSlimClosureLengthGetter() { return OBJECT_OFFSETOF(Instance, typedSlimClosureLengthGetter); }
    static constexpr ptrdiff_t offsetOfTypedSlimClosureNameGetter() { return OBJECT_OFFSETOF(Instance, typedSlimClosureNameGetter); }
    static constexpr ptrdiff_t offsetOfTypedClosureStructureIDPair() { return OBJECT_OFFSETOF(Instance, typedClosureStructureIDPair); }
    static constexpr ptrdiff_t offsetOfTypedSlimClosureHeader(unsigned kind) { return OBJECT_OFFSETOF(Instance, typedSlimClosureHeaders) + kind * sizeof(uint64_t); }
    static constexpr ptrdiff_t offsetOfTypedSlimClosureAllocator(unsigned numberOfCaptures) { return OBJECT_OFFSETOF(Instance, typedSlimClosureAllocators) + (numberOfCaptures + 1) / 2 * sizeof(LocalAllocator*); }
    static constexpr ptrdiff_t offsetOfTypedModifiedLiterals() { return OBJECT_OFFSETOF(Instance, typedModifiedLiterals); }
    static constexpr ptrdiff_t offsetOfTypedStrings() { return negativeOffsetOf(OBJECT_OFFSETOF(NegativeOffsetFields, typedStrings)); }
    static constexpr ptrdiff_t offsetOfSavedFrameReturnAddress() { return OBJECT_OFFSETOF(Instance, savedFrameReturnAddress); }
    static constexpr ptrdiff_t offsetOfSavedFrameSpace() { return OBJECT_OFFSETOF(Instance, savedFrameSpace); }
    static constexpr ptrdiff_t offsetOfSavedFrameHeader() { return OBJECT_OFFSETOF(Instance, savedFrameHeader); }
    static constexpr ptrdiff_t offsetOfTypedData() { return OBJECT_OFFSETOF(Instance, typedData); }
    static constexpr ptrdiff_t offsetOfTypedWideStrings() { return OBJECT_OFFSETOF(Instance, typedWideStrings); }
    static constexpr ptrdiff_t offsetOfTypedLiteralStrings() { return OBJECT_OFFSETOF(Instance, typedLiteralStrings); }
    static constexpr ptrdiff_t offsetOfTypedLiteralStringCache() { return OBJECT_OFFSETOF(Instance, typedLiteralStringCache); }
    static constexpr ptrdiff_t offsetOfTyped() { return OBJECT_OFFSETOF(Instance, typed); }
    static constexpr ptrdiff_t offsetOfTypedFunctions() { return OBJECT_OFFSETOF(Instance, typedFunctions); }
    static constexpr ptrdiff_t offsetOfTypedExecutable(unsigned kind) { return OBJECT_OFFSETOF(Instance, typedExecutables) + kind * sizeof(JSCell*); }
    static constexpr ptrdiff_t offsetOfTypedClosures() { return OBJECT_OFFSETOF(Instance, typedClosures); }
    static constexpr ptrdiff_t offsetOfTypedBuiltins() { return negativeOffsetOf(OBJECT_OFFSETOF(NegativeOffsetFields, typedBuiltins)); }
    static constexpr unsigned numberOfHostClasses = Typed::maxNumberOfHostClasses;
    uint32_t hostClassStructureIDs[numberOfHostClasses];
    static constexpr ptrdiff_t offsetOfHostClassStructureID(unsigned number) { return OBJECT_OFFSETOF(Instance, hostClassStructureIDs) + number * sizeof(uint32_t); }
    BiasedDataPointer linkedFunctionsSharedData;
    BiasedDataPointer ownData[maxNumberOfOwnData];
    static constexpr unsigned numberOfTypedStringsWithWord = 4096;
    uint32_t typedStringWords[numberOfTypedStringsWithWord];
    static constexpr ptrdiff_t offsetOfTypedStringWord(unsigned place) { return OBJECT_OFFSETOF(Instance, typedStringWords) + place * sizeof(uint32_t); }
    uint16_t missCounters[numberOfMissCounters];
    uint16_t states[0];
};
static_assert(std::is_trivially_default_constructible_v<Instance>, "Instance lives in zeroed pages and is never constructed, so default member initializers would be ignored");
static_assert(sizeof(Instance::NegativeOffsetFields) <= 256);
static_assert(Instance::offsetOfTypedData() < 4096 * sizeof(uint32_t));

JS_EXPORT_PRIVATE const RegisterAtOffsetList* calleeSaveRegistersOf(const ImageFunction&);

struct Data {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(Data);

    static Data* create(Instance&, ScriptExecutable*, UnlinkedCodeBlock*, JITCode&, CodeBlock* = nullptr);
    static Data* create(Instance&, const ImageFunction&);
    static Data* create(Instance&, ScriptExecutable*, UnlinkedCodeBlock*, const ImageFunction&, JITCode*, CodeBlock*);
    const ImageFunction* imageFunction() const;
    static void destroy(Data*);
    void didFill();
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

class PropertyLookupTable {
    WTF_MAKE_TZONE_ALLOCATED(PropertyLookupTable);
    WTF_MAKE_NONCOPYABLE(PropertyLookupTable);
public:
    static constexpr uint32_t maxName = (1u << 15) - 1;
    static constexpr uint32_t instanceOfName = 0;
    static constexpr uint32_t hasKindAndCode = 1u << 15;
    static constexpr uint32_t numberOfBases = 1u << 21;
    static constexpr uint32_t numberOfColumns = maxName + 1;
    static constexpr uint32_t numberOfHotNames = 1u << 11;
    static constexpr uint32_t hotNameMultiplier = 1779;
    static constexpr uint32_t hotNameInverseMultiplier = 571;
    static_assert(hotNameMultiplier * hotNameInverseMultiplier % numberOfHotNames == 1);
    static constexpr uint32_t nameForIdentifierIndex(uint32_t number) { return number < numberOfHotNames ? number * hotNameMultiplier % numberOfHotNames : number; }
    static constexpr uint32_t identifierIndexForName(uint32_t name) { return name < numberOfHotNames ? name * hotNameInverseMultiplier % numberOfHotNames : name; }
    static constexpr int32_t offsetOfFirstColumn = 0;
    static constexpr uint32_t entriesPerPlane = numberOfBases + numberOfColumns + 1;
    struct Cell {
        uint32_t readOffsetOrPayload;
        uint8_t code;
        uint8_t kindOrStoreOffset;
        uint16_t name;
    };
    static_assert(sizeof(Cell) == sizeof(uint64_t));
    static constexpr int32_t offsetOfOverflow = static_cast<int32_t>(entriesPerPlane * sizeof(Cell));
    static constexpr int32_t offsetOfReadOffsetInEntry = OBJECT_OFFSETOF(Cell, readOffsetOrPayload);
    static constexpr int32_t offsetOfPayloads = OBJECT_OFFSETOF(Cell, readOffsetOrPayload);
    enum class Kind : uint8_t {
        Absent,
        AbsentAndDefineAddsInline,
        AbsentAndDefineAddsOutOfLine,
        AbsentAndDefineAddsInNewStorage,
        AbsentAndStoreAddsInline,
        AbsentAndStoreAddsOutOfLine,
        AbsentAndStoreAddsInNewStorage,
        DefineAddsInline,
        DefineAddsOutOfLine,
        DefineAddsInNewStorage,
        StoreAddsInline,
        StoreAddsOutOfLine,
        StoreAddsInNewStorage,
        OutOfLine,
        OutOfLineAndWritten,
        OwnAccessorInline,
        Accessor,
        AccessorWithSetterCall,
        InheritedInline,
        InheritedOutOfLine,
        FieldCell32,
        FieldInt32,
        FieldBoxed,
        FieldDouble,
        FieldBoolean,
        FieldOptionalBoolean,
        ExtraProperty,
        KnownCustomGetter,
        ArrayLength,
        LazyClosure,
        NamespaceExport,
        Unknown,
    };
    static constexpr unsigned numberOfKinds = static_cast<unsigned>(Kind::Unknown) + 1;
    enum class AddedStorage : uint8_t { Inline, OutOfLine, InNewStorage };
    static constexpr Kind addingKind(bool isKnownToBeAbsent, bool storesAlsoAdd, AddedStorage where) { return static_cast<Kind>(1 + (isKnownToBeAbsent ? 0 : 6) + (storesAlsoAdd ? 3 : 0) + static_cast<unsigned>(where)); }
    static constexpr bool impliesAbsent(Kind kind) { return kind <= Kind::AbsentAndStoreAddsInNewStorage; }
    static constexpr bool adds(Kind kind) { return kind >= Kind::AbsentAndDefineAddsInline && kind <= Kind::StoreAddsInNewStorage; }
    static constexpr bool storeAdds(Kind kind) { return adds(kind) && (static_cast<unsigned>(kind) - 1) / 3 % 2; }
    static constexpr AddedStorage addedStorage(Kind kind) { return static_cast<AddedStorage>((static_cast<unsigned>(kind) - 1) % 3); }
    static constexpr int32_t offsetOfCodeInEntry = OBJECT_OFFSETOF(Cell, code);
    static constexpr int32_t offsetOfStoreOffsetInEntry = OBJECT_OFFSETOF(Cell, kindOrStoreOffset);
    static constexpr int32_t offsetOfKindInEntry = OBJECT_OFFSETOF(Cell, kindOrStoreOffset);
    static constexpr int32_t offsetOfNameInEntry = OBJECT_OFFSETOF(Cell, name);
    static constexpr uint8_t rowHasOverflowNames = 1;
    static constexpr int32_t offsetOfRowFlags = offsetOfFirstColumn - static_cast<int32_t>(sizeof(Cell)) + offsetOfCodeInEntry;
    static constexpr unsigned bitsOfKind = 5;
    static_assert(numberOfKinds <= 1u << bitsOfKind);
    static constexpr uint8_t valueIsInStorage = 1u << bitsOfKind;
    static constexpr uint8_t holderIsInPayload = 2u << bitsOfKind;
    static constexpr uint8_t storeNeedsNoHandler = 4u << bitsOfKind;
    static constexpr uint8_t storesAlsoAdd = 0x80;
    static constexpr uint8_t flagsForKind(Kind kind)
    {
#if CPU(ARM64)
        switch (kind) {
        case Kind::OutOfLine:
            return valueIsInStorage;
        case Kind::OutOfLineAndWritten:
            return valueIsInStorage | storeNeedsNoHandler;
        case Kind::InheritedInline:
            return holderIsInPayload;
        case Kind::InheritedOutOfLine:
            return holderIsInPayload | valueIsInStorage;
        default:
            return adds(kind) && addedStorage(kind) == AddedStorage::Inline ? storeNeedsNoHandler : 0;
        }
#else
        UNUSED_PARAM(kind);
        return 0;
#endif
    }
    static constexpr Kind ordinaryInstanceOfKind = Kind::OutOfLine;
    static constexpr Kind ownHasInstanceKind = Kind::OwnAccessorInline;
    static constexpr Kind inheritedHasInstanceKind = Kind::Accessor;
    static constexpr uint32_t instanceOfNameAndKind(Kind kind) { return (hasKindAndCode | instanceOfName) << 8 | static_cast<uint8_t>(kind) | flagsForKind(kind); }
    static constexpr uint8_t codeFlagForKind(Kind kind)
    {
#if CPU(ARM64)
        return adds(kind) && addedStorage(kind) == AddedStorage::Inline && storeAdds(kind) ? storesAlsoAdd : 0;
#else
        UNUSED_PARAM(kind);
        return 0;
#endif
    }
    static constexpr int32_t maxOutOfLineIndex = 125;
    static constexpr uint32_t codeForOutOfLineIndex(uint32_t place) { return static_cast<uint8_t>(-static_cast<int32_t>(place) - 2); }
    static constexpr uint32_t maxOwnOutOfLineIndex = (1u << 18) - 3;
    static constexpr uint32_t distanceBelowButterfly(uint32_t place) { return (place + 2) * static_cast<uint32_t>(sizeof(uint64_t)); }
    static constexpr uint32_t wordForOwnOutOfLineIndex(uint32_t place) { return 0 - distanceBelowButterfly(place); }
    static constexpr uint32_t ownAccessorIsInline = 2;
    static constexpr uint32_t wordForOwnInlineAccessor(uint32_t place) { return place * static_cast<uint32_t>(sizeof(uint64_t)) | ownAccessorIsInline; }
    static constexpr uint32_t minHolderWord = 1u << 21;
    static constexpr uint8_t farInheritedCode = 1;
    static constexpr uint32_t farInheritedCodeAndKind = farInheritedCode | static_cast<uint32_t>(Kind::Unknown) << 8;
    static constexpr uint8_t delegatesToPrototypeCode = 2;
    static constexpr uint32_t delegatesToPrototypeCodeAndKind = delegatesToPrototypeCode | static_cast<uint32_t>(Kind::Unknown) << 8;
    static_assert(offsetOfKindInEntry == offsetOfCodeInEntry + 1);
    static constexpr uint32_t firstCodeOutOfLine = 0x80;
    static constexpr uint32_t needsFirstStorage = 2;
    static constexpr uint32_t growsStorage = 4;
    static constexpr uint32_t newStructureFlags = needsFirstStorage | growsStorage;
    static constexpr uint32_t storeAddsExtraProperty = 0x80;
    static constexpr uint32_t onlyDefinitionAddsExtraProperty = 0x40;
    static constexpr uint32_t extraPropertyUsesHandle = 0x40;
    static constexpr uint32_t firstOutOfLineExtraPropertyCode = 0x20;
    static constexpr uint32_t extraPropertyIsAccessor = 2;
    static constexpr uint32_t extraPropertyIsWritten = 4;
    static constexpr uint32_t extraPropertyHasDefaultAttributes = 8;
    static constexpr unsigned extraPropertiesOffsetShift = 8;
    static constexpr uint32_t extraPropertyIsKnownToBeAbsent = needsFirstStorage;
    static constexpr uint32_t storeCreatesExtraProperties = growsStorage;

    enum class Access : uint8_t { Read, Store, Define };
    static constexpr uint32_t payloadIsSlots = 1;
    static constexpr uint32_t slotsPerPayload = 3;
    static constexpr uint32_t maxNumberOfSlots = 1u << 20;
    static constexpr int32_t offsetOfSlotFromScaledPayload(Access access) { return static_cast<int32_t>(static_cast<unsigned>(access) * sizeof(Slot)) - 8; }

    static PropertyLookupTable& of(VM&);
    static void destroy(VM&);
    ~PropertyLookupTable();

    const uint32_t* emptyRowEntries() const { return &m_cells[1].readOffsetOrPayload; }
    Data* dataFor(Instance*);
    bool isSharedByInstances() const { return m_isSharedByInstances; }

    bool setOwnOffset(Structure*, uint32_t name, PropertyOffset, bool isStoredTo);
    bool setAbsent(Structure*, uint32_t name, UniquedStringImpl* prototypeDependentUID);
    bool setInherited(Structure*, uint32_t name, JSCell* holder, PropertyOffset, UniquedStringImpl* prototypeDependentUID);
    bool setDelegatesToPrototype(Structure*, uint32_t name, JSCell* prototype);
    bool setInstanceOf(Structure*, Kind, PropertyOffset, JSCell* holderOrNull, UniquedStringImpl* hasInstanceUID);
    bool replaceSlotWithInherited(Structure*, uint32_t name, JSCell* holder, PropertyOffset);
    bool setExtraProperty(Structure*, uint32_t name, PropertyOffset, bool isAccessor, bool isStoredTo, bool hasDefaultAttributes = false);
    bool setExtraPropertyAdded(Structure*, uint32_t name, Structure* after, PropertyOffset, bool createsExtraProperties, UniquedStringImpl* uid, bool isDefinition = false);
    bool setAccessor(Structure*, uint32_t name, JSCell* holderOrNull, PropertyOffset, UniquedStringImpl* prototypeDependentUID, bool storesCallSetter = false);
    bool setField(Structure*, uint32_t name, uint64_t fieldReadInfo);
    void setKnownCustomGetter(Structure*, uint32_t name, uint32_t offsetOfCachedValue, UniquedStringImpl*);
    bool setArrayLength(Structure*, uint32_t name, bool hasDoubleShape);
    bool setLazyClosure(Structure*, uint32_t name, PropertyOffset, Structure* after);
    bool setNamespaceExport(Structure*, uint32_t name, uint32_t number);
    bool setAdded(Structure*, uint32_t name, Access, Structure* after, PropertyOffset, uint32_t newStructureFlags, UniquedStringImpl* prototypeDependentUID);
    Slot* slotIfExists(Structure*, uint32_t name, Access);
    void didChangePrototype(UniquedStringImpl* uidOrNull);
    bool hasPrototypeDependentEntries() const { return !m_nameListLinks.isEmpty(); }
    void willCreateDeclaredMembers() { ++m_declaredMemberCreationDepth; }
    void didCreateDeclaredMembers() { --m_declaredMemberCreationDepth; }
    void forget(Structure*, uint32_t name);
    struct EntriesReferencingProperty {
        uint32_t name { 0 };
        Vector<Structure*, 8> owners;
    };
    EntriesReferencingProperty entriesReferencing(UniquedStringImpl*, JSCell* holder, PropertyOffset) const;

    Slot* ensureSlot(Structure*, uint32_t name, Access);
    void releaseSlot(Structure*, uint32_t name, Access);
    bool isWatched(Slot*) const;

    void structureDidDie(Structure*);
    void markIfShapeDiedBefore(SlotVisitor&, StructureID);
    void markShapesThatDiedBefore(SlotVisitor&, uint32_t version);
    void finalizeUnconditionally(VM&, bool isEdenCollection);
    void didCreate(Instance*);
    void willDestroy(Instance*);

private:
    PropertyLookupTable();

    struct Bases {
        uint32_t first { 1 };
        uint32_t limit { 0 };
        uint32_t next { 1 };
        uint32_t end { 1 };
    };
    class RowOwner {
    public:
        RowOwner() = default;
        RowOwner(Structure* structure)
            : m_id(structure ? StructureID::encode(structure) : StructureID())
        {
        }
        operator Structure*() const { return m_id ? m_id.decode() : nullptr; }
        Structure* operator->() const { return m_id.decode(); }

    private:
        StructureID m_id;
    };
    class RowNames {
        WTF_MAKE_NONCOPYABLE(RowNames);
    public:
        RowNames() = default;
        ~RowNames() { clear(); }
        unsigned size() const { return m_size; }
        std::span<const uint16_t> span() const { return { isInline() ? m_storage : outOfLineStorage(), m_size }; }
        const uint16_t* begin() const { return span().data(); }
        const uint16_t* end() const { return span().data() + m_size; }
        void append(uint16_t);
        void clear();

    private:
        static constexpr uint16_t inlineCapacity = sizeof(uint16_t*) / sizeof(uint16_t);
        bool isInline() const { return m_capacity == inlineCapacity; }
        uint16_t* outOfLineStorage() const
        {
            uint16_t* result;
            memcpy(&result, m_storage, sizeof(result));
            return result;
        }

        uint16_t m_size { 0 };
        uint16_t m_capacity { inlineCapacity };
        uint16_t m_storage[inlineCapacity] { };
    };
    struct Row {
        RowOwner owner;
        RowNames names;
    };
    static_assert(sizeof(Row) == 16);
    static constexpr uint32_t baseMarker = 0xfffe0000;
    static constexpr uint32_t baseMarkerWithOverflowNames = baseMarker | static_cast<uint32_t>(rowHasOverflowNames) << 16;
    static bool isBaseMarker(uint32_t entry) { return entry == baseMarker || entry == baseMarkerWithOverflowNames; }

    static constexpr uint16_t isInOverflow = 1u << 15;
    static bool isInlineEntry(uint32_t entry) { return entry & maxName && !(entry & hasKindAndCode); }
    static bool isInlineEntry(const Cell& cell) { return static_cast<uint16_t>(cell.name - 1) < maxName; }
    static bool hasOwnOffsetInWord(uint32_t entry) { return isInlineEntry(entry) || (entry & hasKindAndCode && (kindOf(entry) == Kind::OutOfLine || kindOf(entry) == Kind::OutOfLineAndWritten)); }
    uint32_t entryAt(uint32_t index) const
    {
        static_assert(offsetOfCodeInEntry == 4 && offsetOfKindInEntry == 5 && offsetOfNameInEntry == 6);
        uint32_t entry = std::rotl(static_cast<uint32_t>(std::bit_cast<uint64_t>(m_cells[index]) >> 32), 16);
#if CPU(ARM64)
        if (entry & hasKindAndCode) {
            entry &= (1u << (24 + bitsOfKind)) - 1;
            entry &= ~(static_cast<uint32_t>(codeFlagForKind(kindOf(entry))) << 16);
        }
#endif
        return entry;
    }
    void setEntryAt(uint32_t index, uint32_t entry)
    {
        Cell& cell = m_cells[index];
        uint32_t low = isInlineEntry(entry) ? codeOf(entry) : isInlineEntry(cell) ? 0 : cell.readOffsetOrPayload;
#if CPU(ARM64)
        if (entry & hasKindAndCode)
            entry |= static_cast<uint32_t>(codeFlagForKind(kindOf(entry))) << 16 | static_cast<uint32_t>(flagsForKind(kindOf(entry))) << 24;
#endif
        cell = std::bit_cast<Cell>(static_cast<uint64_t>(std::rotl(entry, 16)) << 32 | low);
    }
    uint32_t payload(uint32_t index) const { return isInlineEntry(m_cells[index]) ? 0 : m_cells[index].readOffsetOrPayload; }
    void setPayload(uint32_t index, uint32_t payload)
    {
        ASSERT(!isInlineEntry(m_cells[index]));
        m_cells[index].readOffsetOrPayload = payload;
    }
    Slot* slotsAt(uint32_t index) { return payload(index) & payloadIsSlots ? m_slots + (payload(index) >> 1) : nullptr; }
    static bool usesPayloadDirectly(Kind);
    static bool addsExtraProperty(uint32_t entry) { return entry & hasKindAndCode && kindOf(entry) == Kind::ExtraProperty && codeOf(entry) & storeAddsExtraProperty; }
    static bool payloadIsStructure(uint32_t entry) { return adds(kindOf(entry)) || kindOf(entry) == Kind::LazyClosure || addsExtraProperty(entry); }
    static bool isFarInherited(uint32_t entry) { return entry & hasKindAndCode && kindOf(entry) == Kind::Unknown && codeOf(entry) == farInheritedCode; }
    static bool delegatesToPrototype(uint32_t entry) { return entry & hasKindAndCode && kindOf(entry) == Kind::Unknown && codeOf(entry) == delegatesToPrototypeCode; }
    static bool usesPayloadDirectly(uint32_t entry) { return entry & hasKindAndCode && (usesPayloadDirectly(kindOf(entry)) || isFarInherited(entry) || delegatesToPrototype(entry)); }
    static Kind kindOf(uint32_t entry) { return static_cast<Kind>(entry >> 24); }
    static uint32_t codeOf(uint32_t entry) { return entry >> 16 & 0xff; }
    static uint32_t entryWithKind(uint32_t name, Kind kind, uint32_t code) { return name | hasKindAndCode | code << 16 | static_cast<uint32_t>(kind) << 24; }
    bool isKindEntryFor(uint32_t entry, uint32_t name) const { return (entry & 0xffff) == (name | hasKindAndCode); }
    bool isEntryFor(uint32_t entry, uint32_t name) const { return (entry & maxName) == name && (entry & 0xffff); }
    uint32_t baseOf(Structure*) const;
    static uint32_t indexOf(uint32_t base, uint32_t name) { return base + 1 + name; }
    static uint32_t indexOfRowName(uint32_t base, uint16_t listed) { return listed & isInOverflow ? entriesPerPlane + indexOf(base, listed & maxName) : indexOf(base, listed); }
    uint32_t findIndex(Structure*, uint32_t name) const;
    uint32_t findIndexOfSlots(Structure*, uint32_t name) const;
    uint32_t ensureIndexInOverflow(Structure*, uint32_t name);
    static size_t bitOf(uint32_t index) { return index; }
    Row& rowAt(uint32_t base) { return m_rows[payload(base)]; }
    uint32_t ensureIndex(Structure*, uint32_t name);
    uint32_t createRow(Structure*, uint32_t name);
    uint32_t moveRow(uint32_t base, std::optional<uint16_t> nameToAdd);
    uint64_t freeCellsFrom(uint32_t index) const;
    uint32_t firstFittingBase(uint32_t from, std::span<const uint16_t> names) const;
    uint32_t findBase(std::span<const uint16_t> names, uint32_t preferredBase = 0);
    void occupy(uint32_t index, uint32_t entry);
    uint32_t vacate(uint32_t index);
    uint32_t allocateRowIndexFor(Structure*);
    void releaseSlotsAt(uint32_t index);
    bool releaseSlotsUnlessUsedByStores(uint32_t index);
    static uint32_t entryForOwnOffset(uint32_t name, PropertyOffset);
    bool setEntryWithKind(Structure*, uint32_t name, Kind, uint32_t code, uint32_t payload, UniquedStringImpl* prototypeDependentUID);
    bool setInheritedFarOutOfLine(Structure*, uint32_t name, JSCell* holder, uint32_t place, UniquedStringImpl* prototypeDependentUID);
    void setDependsOnPrototypes(uint32_t index, uint32_t name, UniquedStringImpl*);
    uint32_t entryRow(uint32_t index, uint32_t name) const;
    void appendToNameList(uint32_t row, uint32_t& head);
    void freeNameList(uint32_t name);
    void clearNameLists();
    static constexpr uint32_t namesPerHeadGroup = 4;
    using NameListHeadGroup = std::array<uint32_t, namesPerHeadGroup>;
    uint32_t* nameListHeadIfExists(uint32_t name)
    {
        uint16_t group = m_nameListHeadGroup[name / namesPerHeadGroup];
        return group ? &m_nameListHeads[group - 1][name % namesPerHeadGroup] : nullptr;
    }
    uint32_t nameListHead(uint32_t name) const
    {
        uint16_t group = m_nameListHeadGroup[name / namesPerHeadGroup];
        return group ? m_nameListHeads[group - 1][name % namesPerHeadGroup] : 0;
    }
    uint32_t& ensureNameListHead(uint32_t name)
    {
        uint16_t& group = m_nameListHeadGroup[name / namesPerHeadGroup];
        if (!group) [[unlikely]] {
            m_nameListHeads.append(NameListHeadGroup { });
            group = static_cast<uint16_t>(m_nameListHeads.size());
        }
        return m_nameListHeads[group - 1][name % namesPerHeadGroup];
    }
    template<typename Functor> void forEachIndexInNameList(uint32_t name, const Functor& functor) const
    {
        for (uint32_t position = nameListHead(name); position; position = m_nameListLinks[position - 1].next) {
            Structure* owner = m_rows[m_nameListLinks[position - 1].row].owner;
            if (uint32_t index = owner ? findIndex(owner, name) : 0)
                functor(index);
        }
    }
    void invalidatePrototypeDependentEntries(uint32_t index);
    void invalidateEntriesNamed(uint32_t name);
    void removeDeadNewStructures(VM&, Vector<uint32_t>& indices);
    void release(uint32_t base);
    void clear();

    Cell* m_cells { nullptr };
    Bases m_bases;
    uint64_t* m_occupied { nullptr };
    SegmentedVector<Row, 128> m_rows;
    Vector<uint32_t> m_freeRows;
    struct RecentRow {
        uint16_t name { 0 };
        uint32_t row { 0 };
    };
    std::array<RecentRow, 256> m_recentRows { };
    BitVector m_dependsOnPrototypes;
    Vector<Structure*> m_structuresWithNewRows;
    static constexpr unsigned deadShapeIndexBits = 11;
    static uint32_t shapeHash(Structure*);
    uint32_t& deadShapeEntry(uint32_t word) { return m_deadShapes[word * 0x9e3779b1u >> (32 - deadShapeIndexBits)]; }
    std::array<uint32_t, 1u << deadShapeIndexBits> m_deadShapes { };
    uint32_t m_lastNewShapeScanVersion { 0 };
    Vector<uint32_t> m_additionIndices;
    Vector<uint32_t> m_newAdditionIndices;
    struct NameListLink {
        uint32_t row;
        uint32_t next;
    };
    UncheckedKeyHashMap<UniquedStringImpl*, uint16_t> m_nameByUID;
    UniquedStringImpl* m_hasInstanceUID { nullptr };
    BitVector m_registeredNames;
    static_assert((maxName + 1) / namesPerHeadGroup <= UINT16_MAX);
    std::array<uint16_t, (maxName + 1) / namesPerHeadGroup> m_nameListHeadGroup { };
    Vector<NameListHeadGroup> m_nameListHeads;
    SegmentedVector<NameListLink, 512> m_nameListLinks;
    uint32_t m_firstFreeNameListLink { 0 };

    Slot* m_slots { nullptr };
    uint32_t m_numberOfSlots { slotsPerPayload };
    Vector<uint32_t> m_freeSlots;
    Data* m_data { nullptr };
    bool m_isSharedByInstances { false };
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
    static constexpr uint32_t isTypedFunctionPositions = 1;
    static constexpr uint32_t isTypedFunctionHandlersAndPositions = 2;
    static constexpr uint8_t someHandlerIgnoresValue = 0x80;
    static constexpr unsigned sitesBetweenPositionCheckpoints = 512;
    static constexpr bool isTypedFunctionWithHandlers(uint32_t word) { return (word & 3) == isTypedFunctionHandlersAndPositions; }
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
    return data->functionIndexAndFlags()[this - data->infos()];
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
    unsigned numberOfInlineEmptyArrayLiterals { 0 };
    unsigned numberOfStubEmptyArrayLiterals { 0 };
    bool startsCold { false };
    RegisterAtOffsetList calleeSaveRegisters;
    Vector<StubCall> stubCalls;
    Vector<ArgumentMoves> argumentMoves;
    Vector<Site> sites;
    Vector<ImageKey> knownCallees;
    uint32_t valueEntry { StubCall::noFunction };
    uint32_t valueEntrySignatureIndex { StubCall::noFunction };
    uint32_t signatureAdapter { StubCall::noFunction };
    uint32_t adapterResult { StubCall::noFunction };
    uint32_t adapterArgumentCount { 0 };
    uint32_t compatibleResultsMaskSize { 0 };
    uint64_t compatibleResultsMask { 0 };
    static constexpr uint32_t siteConstantIsPlan = 1u << 30;
    Vector<uint32_t> siteConstants;
    Vector<uint32_t> plans;
    Vector<uint32_t> quotableSites;
    Vector<uint32_t> constructSites;
    Vector<uint32_t> constructingSites;
    Vector<ParameterCheckSite> parameterCheckSites;
    Vector<uint32_t> callSites;
    Vector<uint32_t> identifierIndices;
    Vector<uint32_t> typedIdentifierIndices;
    Vector<String> typedIdentifierNames;
    Vector<std::pair<uint32_t, uint32_t>> typedCatchEntrypointsForCallSites;
    uint32_t numberOfSitesWithoutCode { 0 };
    struct FaultingAccess {
        uint32_t codeOffset;
        uint32_t callSite;
        Vector<uint32_t, 3> throwSite;
        bool isLoopPoll { false };
    };
    Vector<FaultingAccess> faultingAccesses;
    bool hasNoFrame { false };
    uint32_t stackUsedAtCalls { 0 };
    bool isEnteredWithoutStackCheck { false };
    bool isProvenToFitOnStack { false };
    bool isGenericEntrypoint { false };
    uint8_t kindByte { 0 };
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
    uint32_t hasValueEntry : 1;
    uint32_t hasCompatibleResultsMask : 1;

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
    enum class EntryKind : uint8_t { TopLevel, Call, Construct };
    static EntryKind entryKindFor(UnlinkedCodeBlock*);
    JITCode(void* code, const ImageFunction&, EntryKind);
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
Stub getByNameHandler(unsigned form, PropertyLookupTable::Kind);
Stub putByNameHandler(unsigned variant, PropertyLookupTable::Kind);
static_assert(Instance::numberOfByNameEntryKinds == PropertyLookupTable::numberOfKinds);
static_assert(PropertyLookupTable::distanceBelowButterfly(PropertyLookupTable::maxOwnOutOfLineIndex) < PropertyLookupTable::minHolderWord && PropertyLookupTable::wordForOwnInlineAccessor(UINT8_MAX) < PropertyLookupTable::minHolderWord);
static_assert(PropertyLookupTable::addingKind(true, true, PropertyLookupTable::AddedStorage::OutOfLine) == PropertyLookupTable::Kind::AbsentAndStoreAddsOutOfLine && PropertyLookupTable::addingKind(false, false, PropertyLookupTable::AddedStorage::InNewStorage) == PropertyLookupTable::Kind::DefineAddsInNewStorage && PropertyLookupTable::storeAdds(PropertyLookupTable::Kind::StoreAddsInline) && !PropertyLookupTable::storeAdds(PropertyLookupTable::Kind::DefineAddsInNewStorage));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
