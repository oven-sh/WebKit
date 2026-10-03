/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunction.h"
#include "AOTOperationsBuiltins.h"
#include "AOTOperationsObjects.h"
#include "AOTProgramData.h"
#include "AOTSlotWatchpoint.h"
#include "AOTStubs.h"
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
#include "Structure.h"
#include "StructureID.h"
#include <wtf/TZoneMalloc.h>

namespace JSC {

class CodeBlock;
class JSGlobalObject;
class CallFrame;
class FunctionExecutable;
class ScriptExecutable;
class UnlinkedCodeBlock;
class VM;

namespace AOT {

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
    v(operationAOTValueNegate) \
    v(operationAOTValueBitNot) \
    v(operationAOTValueInc) \
    v(operationAOTValueDec) \
    v(operationAOTToNumber) \
    v(operationAOTToNumeric) \
    v(operationAOTToString) \
    v(operationAOTToBoolean) \
    v(operationAOTCompareLess) \
    v(operationAOTCompareLessEq) \
    v(operationAOTCompareGreater) \
    v(operationAOTCompareGreaterEq) \
    v(operationAOTCompareEq) \
    v(operationAOTCompareStrictEq) \
    v(operationAOTGetById) \
    v(operationAOTPutById) \
    v(operationAOTGetByVal) \
    v(operationAOTGetElementOrEmpty) \
    v(operationAOTPutByVal) \
    v(operationAOTResolveScope) \
    v(operationAOTGetFromScope) \
    v(operationAOTReadLazyClosureVar) \
    v(operationAOTFillImportSlot) \
    v(operationAOTPutToScope) \
    v(operationAOTThrow) \
    v(operationAOTCheckType) \
    v(operationAOTCheckTypedLayout) \
    v(operationAOTCoerceToTypedLayout) \
    v(operationAOTLatin1StringEqualTo) \
    v(operationAOTGetFieldSlow) \
    v(operationAOTReadField) \
    v(operationAOTGetLengthSlow) \
    v(operationAOTValidateTypedObject) \
    v(operationAOTVerifyInferredType) \
    v(operationAOTHandleTraps) \
    v(operationAOTWriteBarrier) \
    v(operationAOTCatch) \
    v(operationAOTProgramConstant) \
    v(operationAOTTemplateObject) \
    v(operationAOTCreateTransientConstant) \
    v(operationAOTSwitchString) \
    v(operationAOTSwitchChar) \
    v(operationAOTFMod) \
    v(operationAOTPow) \
    v(operationAOTDoubleToInt32) \
    FOR_EACH_AOT_OBJECT_OPERATION(v) \
    FOR_EACH_AOT_BUILTIN_OPERATION(v) \
    FOR_EACH_AOT_OPERATION_OF_THE_OTHER_TIERS(v) \

#define FOR_EACH_AOT_OPERATION_OF_THE_OTHER_TIERS(v) \
    v(operationStringStartsWith) \
    v(operationStringStartsWithWithIndex) \
    v(operationStringEndsWith) \
    v(operationStringEndsWithWithEndPosition) \
    v(operationStringIndexOf) \
    v(operationStringIndexOfWithIndex) \
    v(operationStringLastIndexOf) \
    v(operationStringSlice) \
    v(operationStringSliceWithEnd) \
    v(operationStringSubstring) \
    v(operationStringSubstringWithEnd) \
    v(operationStringTrim) \
    v(operationStringTrimStart) \
    v(operationStringTrimEnd) \
    v(operationToLowerCase) \
    v(operationToUpperCase) \
    v(operationStringLocaleCompare) \
    v(operationStringReplaceStringString) \
    v(operationStringProtoFuncReplaceRegExpString) \
    v(operationStringProtoFuncReplaceAllRegExpString) \
    v(operationStringSplit) \
    v(operationStringSplitRegExp) \
    v(operationStringMatchRegExp) \
    v(operationStringSearchRegExp) \
    v(operationStringFromCharCode) \
    v(operationToString) \
    v(operationInt32ToStringWithValidRadix) \
    v(operationDoubleToStringWithValidRadix) \
    v(operationParseIntStringNoRadix) \
    v(operationParseIntDoubleNoRadix) \
    v(operationParseIntString) \
    v(operationObjectKeysObject) \
    v(operationObjectGetOwnPropertyNamesObject) \
    v(operationObjectGetOwnPropertySymbolsObject) \
    v(operationGetPrototypeOfObject) \
    v(operationObjectCreate) \
    v(operationObjectAssignUntyped) \
    v(operationSameValue) \
    v(operationArrayShift) \
    v(operationArrayUnshift) \
    v(operationArraySplice) \
    v(operationArraySpliceIgnoreResult) \
    v(operationArrayConcatAppendOne) \
    v(operationArrayJoin) \
    v(operationArrayJoinGeneric) \
    v(operationArrayIndexOfValueInt32OrContiguous) \
    v(operationArrayIncludesValueInt32OrContiguous) \
    v(operationRegExpTestString) \
    v(operationRegExpExecString) \
    v(operationDateNow) \
    v(operationMakeRope2) \
    v(operationMakeRope3) \

#define FOR_EACH_AOT_THUNK(v) \
    v(HandleException) \
    v(ThrowStackOverflowAtPrologue) \
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
    v(EnterStaticFunctionForCall) \
    v(EnterStaticFunctionForConstruct) \
    v(ConstructViaCall) \
    v(MegamorphicCache) \
    v(FieldLayoutIDsInSlot0) \
    v(FieldLayoutIDsInSlot1) \
    v(FieldLayoutIDsInSlot2) \
    v(FieldLayoutIDsInSlot3) \
    v(FieldLayoutIDsInSlot4) \
    v(FieldLayoutIDsInSlot5) \
    v(FieldLayoutIDsInSlot6) \
    v(FieldLayoutIDsInSlot7) \
    v(FieldLayoutIDsInSlot8) \
    v(FieldLayoutIDsInSlot9) \
    v(FieldLayoutIDsInSlot10) \
    v(FieldLayoutIDsInSlot11) \
    v(FieldLayoutIDsInSlot12) \
    v(FieldLayoutIDsInSlot13) \
    v(FieldLayoutIDsInSlot14) \
    v(FieldLayoutIDsInSlot15) \
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
    v(RawNewObject) \
    v(RawNewObjectLiteral) \
    v(RawCreateThis) \
    v(RawCreateThisWithProperties) \
    v(RawNewFunction) \
    v(RawCompareStrictEq) \
    v(RawCompareEq) \
    v(RawInById) \
    v(RawInByVal) \
    v(NewArraySlowPath) \
    v(NewArrayBufferSlowPath) \
    v(NewArrayWithSpreadSlowPath) \
    v(NewArrayWithSpeciesSlowPath) \
    v(CreateRestSlowPath) \
    v(CreateLexicalEnvironmentSlowPath) \
    v(MakeRope2SlowPath) \
    v(MakeRope3SlowPath) \
    v(StringSliceWithEndSlowPath) \
    v(StringSubstringWithEndSlowPath) \
    v(ToLowerCaseSlowPath) \
    v(ObjectKeysObjectSlowPath) \
    v(ValueAddSlowPath) \

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
bool takesGlobalObject(Entry);

struct VirtualCallInfo {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(VirtualCallInfo);

    static constexpr ptrdiff_t offsetOfFindTarget() { return OBJECT_OFFSETOF(VirtualCallInfo, findTarget); }
    static constexpr ptrdiff_t offsetOfLookupExceptionHandler() { return OBJECT_OFFSETOF(VirtualCallInfo, lookupExceptionHandler); }

    DataOnlyCallLinkInfo callLinkInfo;
    void* findTarget;
    void* lookupExceptionHandler;
};

class RuntimeTable {
    WTF_MAKE_TZONE_ALLOCATED(RuntimeTable);
    WTF_MAKE_NONCOPYABLE(RuntimeTable);
public:
    explicit RuntimeTable(VM&);
    ~RuntimeTable();

    void** entries() { return m_entries; }

private:
    void* m_entries[numberOfEntries];
    Vector<std::unique_ptr<VirtualCallInfo>> m_callLinkInfos;
};

RuntimeTable& runtimeTable(VM&);

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
    bool isPolymorphic() const { return !structureID && (offset & flagsMask) == polymorphicFlags; }
    bool isNameOnly() const { return !structureID && !(offset & (flagsMask | attemptsMask)) && offset >> nameIDShift; }

    void clear()
    {
        structureID = StructureID();
        offset &= attemptsMask;
        pointer = nullptr;
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
    static constexpr unsigned numberOfSlots = 4;
    static constexpr uint32_t maxBulkLearnAttempts = 12;
    static constexpr uint32_t missesPerLearningAttempt = 1024;
    static_assert(hasOneBitSet(missesPerLearningAttempt));

    static constexpr ptrdiff_t offsetOfName() { return OBJECT_OFFSETOF(PolymorphicSlots, name); }
    static constexpr ptrdiff_t offsetOfMisses() { return OBJECT_OFFSETOF(PolymorphicSlots, misses); }
    static constexpr ptrdiff_t offsetOfRemainingBulkLearnAttempts() { return OBJECT_OFFSETOF(PolymorphicSlots, remainingBulkLearnAttempts); }
    static constexpr ptrdiff_t offsetOfSlots() { return OBJECT_OFFSETOF(PolymorphicSlots, slots); }
    static constexpr ptrdiff_t offsetOfByName() { return OBJECT_OFFSETOF(PolymorphicSlots, byName); }

    static constexpr unsigned numberOfInlineSlotsByName = 4;
    static constexpr unsigned inlineNameSlotsShift = 16;
    static constexpr unsigned nameTableFillValueShift = 48;
    static constexpr uint64_t initialByName = Structure::firstReservedPropertyNameID;
    static constexpr uint8_t maxNameTableFillFailures = 16;
    bool fillsNameTables() const { return !(byName >> nameTableFillValueShift); }
    void didFailToFillNameTable()
    {
        if (!--remainingNameTableFillFailures)
            byName |= static_cast<uint64_t>(Structure::firstReservedPropertyNameID) << nameTableFillValueShift;
    }
    bool addInlineNameSlot(uint16_t nameID, unsigned inlineSlot)
    {
        ASSERT(inlineSlot < Structure::numberOfSlotsWithFieldIDs && nameID && nameID < Structure::firstReservedPropertyNameID);
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
    uint32_t misses;
    uint32_t remainingBulkLearnAttempts;
    Data* owner;
    uint32_t next;
    uint8_t numberOfUsedInlineNameSlots;
    uint8_t remainingNameTableFillFailures;
    uint64_t byName;
    Slot slots[numberOfSlots];
};

struct Data;
struct ImageEnvironment;
struct ImageFunction;
struct Site;

struct FunctionInfo {
    static constexpr uint16_t hasSiteConstants = 1;
    static constexpr uint16_t startsCold = 2;
    static constexpr uint16_t sitesHaveInlineConstants = 4;

    static constexpr unsigned numberOfFlagBits = 3;
    static constexpr uint32_t maxEncodedSlots = (1u << (16 - numberOfFlagBits)) - 1;
    static constexpr uint16_t encodeSlotCountInFlags(uint32_t numSlots) { return static_cast<uint16_t>(std::min(numSlots, maxEncodedSlots) << numberOfFlagBits); }

    static constexpr ptrdiff_t offsetOfSites() { return OBJECT_OFFSETOF(FunctionInfo, sites); }
    static constexpr ptrdiff_t offsetOfFlags() { return OBJECT_OFFSETOF(FunctionInfo, flags); }

    void set(uint32_t indexPlusOne, CodeSpecializationKind kind, CodeType codeType)
    {
        static_assert(FunctionCode < 4 && ModuleCode < 4 && GlobalCode < 4);
        RELEASE_ASSERT(!(indexPlusOne >> 29));
        indexAndFlags = indexPlusOne << 3 | static_cast<uint32_t>(codeType) << 1 | (kind == CodeSpecializationKind::CodeForConstruct ? 1 : 0);
    }
    uint32_t indexPlusOne() const { return indexAndFlags >> 3; }
    CodeType codeType() const { return static_cast<CodeType>(indexAndFlags >> 1 & 3); }
    bool isTopLevelCode() const { return codeType() != FunctionCode; }
    bool hasExecutable() const { return indexPlusOne() && !isTopLevelCode(); }
    CodeSpecializationKind kind() const { return indexAndFlags & 1 ? CodeSpecializationKind::CodeForConstruct : CodeSpecializationKind::CodeForCall; }
    bool hasCodeInImage() const { return flags & (hasSiteConstants | sitesHaveInlineConstants); }
    JS_EXPORT_PRIVATE const Site* sitesInImage() const;
    inline const ImageFunction* function() const;

    uint32_t sites;
    uint32_t indexAndFlags;
    uint16_t flags;
    uint16_t unused[3];
};
static_assert(sizeof(FunctionInfo) == 16);

struct Instance {
    JS_EXPORT_PRIVATE static Instance& ensure(JSModuleLoader*);
    JS_EXPORT_PRIVATE static Instance& ensure(JSGlobalObject*);
    static Instance* of(JSFunction*);
    Structure* functionStructure(Structure* realmStructure, FunctionExecutable*, JSScope*);
    Structure* functionStructure(Structure* realmStructure);
    JSFunction* tryMakeFunctionWithoutExecutable(uint32_t executableIndex, JSScope*);
    JS_EXPORT_PRIVATE JSFunction* makeFunction(FunctionExecutable*, JSScope*);
    StringImpl* retainUntilNextCall(String&&);
    JSArray* templateObjectFor(uint32_t numberOfDescriptor);
    JS_EXPORT_PRIVATE ScriptExecutable* topLevelExecutableOf(uint32_t moduleID);
    JS_EXPORT_PRIVATE void setTopLevelExecutableOf(uint32_t moduleID, ScriptExecutable*);
    JS_EXPORT_PRIVATE uint64_t prepareModuleCode(ModuleProgramExecutable*, JSScope*);
    JS_EXPORT_PRIVATE void didFinishModuleEvaluation(ModuleProgramExecutable*);
    JSModuleLoader* loader() const;
    bool loaderWasCleared() const;
    JS_EXPORT_PRIVATE static void destroyUnneededInstances(VM&);
    static bool convertToTypedLayout(VM&, JSObject*, uint16_t layoutID);
    Structure* emptyStructureForLayout(uint16_t layoutID);
    Structure* emptyStructureForLayout(uint16_t layoutID, JSObject* prototype);
    static JSObject* newObjectOf(VM&, Structure*);
    JS_EXPORT_PRIVATE static void destroy(Instance*);

    template<typename Visitor> void visit(Visitor&, bool newOnly);
    void finalizeUnconditionally(bool newOnly);
    void noteCalleeCacheFilled(Slot*);

    static constexpr ptrdiff_t offsetOfRuntimeTable() { return OBJECT_OFFSETOF(Instance, runtimeTable); }
    static constexpr ptrdiff_t offsetOfVM() { return OBJECT_OFFSETOF(Instance, vm); }
    static constexpr ptrdiff_t offsetOfGlobalObject() { return OBJECT_OFFSETOF(Instance, globalObject); }
    static constexpr ptrdiff_t offsetOfStates() { return OBJECT_OFFSETOF(Instance, states); }
    static constexpr ptrdiff_t offsetOfInfos() { return OBJECT_OFFSETOF(Instance, infos); }
    static constexpr ptrdiff_t offsetOfProgram() { return OBJECT_OFFSETOF(Instance, program); }
    static constexpr ptrdiff_t offsetOfProgramIdentifiers() { return OBJECT_OFFSETOF(Instance, programIdentifiers); }
    static constexpr ptrdiff_t offsetOfImage() { return OBJECT_OFFSETOF(Instance, image); }
    static constexpr ptrdiff_t offsetOfProgramData() { return OBJECT_OFFSETOF(Instance, programData); }
    static constexpr ptrdiff_t offsetOfStringConstantRecords() { return OBJECT_OFFSETOF(Instance, stringConstantRecords); }
    static constexpr ptrdiff_t offsetOfSharedData() { return OBJECT_OFFSETOF(Instance, sharedData); }
    static constexpr ptrdiff_t offsetOfCode() { return OBJECT_OFFSETOF(Instance, code); }
    static constexpr ptrdiff_t offsetOfCodeGranules() { return OBJECT_OFFSETOF(Instance, codeGranules); }
    static constexpr ptrdiff_t offsetOfSubsequentFunctionStarts() { return OBJECT_OFFSETOF(Instance, subsequentFunctionStarts); }
    JS_EXPORT_PRIVATE Data* ensureData(uint32_t index);
    void countMisses(uint32_t index, uint32_t count)
    {
        uint32_t state = states[index];
        ASSERT(state < minStateWithData);
        uint32_t before = state & maxMisses;
        uint32_t after = std::min<uint32_t>(before + count, maxMisses);
        states[index] = (state & ~maxMisses) | after;
        uint32_t limit = static_cast<uint16_t>(cacheMissLimitFor(infos[index].flags >> FunctionInfo::numberOfFlagBits));
        if (before < limit && after >= limit)
            ensureData(index);
    }

    static constexpr uint32_t maxMisses = 0xffff;
    static constexpr uint32_t isLinkedWithoutData = 1u << 16;
    static constexpr uint32_t minStateWithData = 1u << 17;
    static constexpr unsigned stateWithDataShift = 4;
    bool isLinked(uint32_t index) const { return states[index] >= isLinkedWithoutData; }
    Data* dataIfExists(uint32_t index) const
    {
        uint32_t state = states[index];
        if (state < minStateWithData)
            return nullptr;
        return std::bit_cast<Data*>(std::bit_cast<uintptr_t>(this) + (static_cast<uintptr_t>(state) << stateWithDataShift));
    }
    void setLinkedWithoutData(uint32_t index)
    {
        ASSERT(states[index] < isLinkedWithoutData);
        states[index] |= isLinkedWithoutData;
    }
    void setData(uint32_t index, Data* data)
    {
        uintptr_t distance = std::bit_cast<uintptr_t>(data) - std::bit_cast<uintptr_t>(this);
        RELEASE_ASSERT(!(distance & ((1u << stateWithDataShift) - 1)) && distance >> stateWithDataShift >= minStateWithData && !(distance >> stateWithDataShift >> 32));
        states[index] = static_cast<uint32_t>(distance >> stateWithDataShift);
    }
    void setNotLinked(uint32_t index) { states[index] = 0; }
    void noteTransitionCached(Slot*);
    PolymorphicSlots* makeSiteSlots(Data*, UniquedStringImpl* name);
    void* allocateForData(size_t);
    void freeDataMemory(void*, size_t);
    uint32_t cacheMissLimitFor(uint32_t numSlots) const { return std::min<uint32_t>((numSlots * missLimitPerEightSlots >> 3) + remainingMissBudget, std::numeric_limits<uint16_t>::max()); }
    static constexpr ptrdiff_t offsetOfMissLimitPerEightSlots() { return OBJECT_OFFSETOF(Instance, missLimitPerEightSlots); }
    static constexpr ptrdiff_t offsetOfRemainingMissBudget() { return OBJECT_OFFSETOF(Instance, remainingMissBudget); }

    JS_EXPORT_PRIVATE JSCell** environmentSlot(ImageEnvironment) const;

    Structure* literalStructure(Structure* empty, std::span<UniquedStringImpl* const>);

    Structure* knownShapeStructure(uint32_t shape, std::span<UniquedStringImpl* const> names);
    struct PropertyRunTarget {
        Structure* last { nullptr };
        Vector<StructureID, 4> prototypeStructures;
    };
    const PropertyRunTarget& propertyRunTarget(Structure*, const uint32_t* run, const ScopedLambda<void(Vector<UniquedStringImpl*, 16>&)>& collectNames);
    struct CopiedProperties {
        Structure* last { nullptr };
        Vector<std::pair<PropertyOffset, PropertyOffset>, 8> offsets;
    };
    const CopiedProperties& copiedProperties(Structure* target, Structure* source, const IdentifierSet* excluded);
    static constexpr unsigned maxLearnedInlineCapacity = Structure::numberOfSlotsWithFieldIDs;
    unsigned inlineCapacityFor(JSFunction* constructor, unsigned inlineCapacityInBytecode);
    void noteFirstStructure(Structure*, JSFunction* constructor);
    void noteOutOfLineProperty(Structure*);
    void noteRunOfProperties(Structure* from, Structure* to);
    JSFunction* constructorOfObjectsWith(Structure*);
    void learnInlineCapacity(JSFunction* constructor, Structure*);
    static PropertyOffset offsetAfter(PropertyOffset offset, unsigned inlineCapacity)
    {
        if (offset == invalidOffset)
            return inlineCapacity ? 0 : firstOutOfLineOffset;
        return offset + 1 == static_cast<PropertyOffset>(inlineCapacity) ? firstOutOfLineOffset : offset + 1;
    }

    JSObject* tryCopySlotsForSpread(JSObject* source);
    std::span<const uint16_t> knownShapeSlots(uint32_t shape) const;
    static constexpr ptrdiff_t offsetOfStructureIDBase() { return OBJECT_OFFSETOF(Instance, structureIDBase); }
    void inspectObjectPrototype();
    static constexpr ptrdiff_t offsetOfIntrinsics() { return OBJECT_OFFSETOF(Instance, intrinsics); }
    static constexpr ptrdiff_t offsetOfLinkTimeConstants() { return OBJECT_OFFSETOF(Instance, linkTimeConstants); }
    static constexpr ptrdiff_t offsetOfObjectPrototype() { return OBJECT_OFFSETOF(Instance, objectPrototype); }
    static constexpr ptrdiff_t offsetOfObjectPrototypeStructureID() { return OBJECT_OFFSETOF(Instance, objectPrototypeStructureID); }
    static constexpr ptrdiff_t offsetOfSelectorsOnObjectPrototype() { return OBJECT_OFFSETOF(Instance, selectorsOnObjectPrototype); }
    static constexpr ptrdiff_t offsetOfDispatch() { return OBJECT_OFFSETOF(Instance, dispatch); }
    static constexpr ptrdiff_t offsetOfSelectorRows() { return OBJECT_OFFSETOF(Instance, selectorRows); }

    void** runtimeTable;
    JSGlobalObject* globalObject;
    VM* vm;
    struct Collections;
    Collections* collections;
    const FunctionInfo* infos;
    const uint8_t* image;
    VMProgram* program;
    const ProgramData* programData;
    const uint32_t* stringConstantRecords;
    Data* sharedData;
    const uint32_t* functionMetadataOffsets;
    const uint8_t* code;
    const uint32_t* codeGranules;
    const uint32_t* subsequentFunctionStarts;
    JSCell* typedArrayLengthGetter { nullptr };
    static constexpr ptrdiff_t offsetOfTypedArrayLengthGetter() { return OBJECT_OFFSETOF(Instance, typedArrayLengthGetter); }
    UniquedStringImpl* const* programIdentifiers;
    uint32_t missLimitPerEightSlots;
    uint32_t remainingMissBudget;
    uintptr_t structureIDBase;
    const uint32_t* dispatch;
    const uint32_t* selectorRows;
    JSObject* objectPrototype;
    uint8_t* fieldsWithObservableReads;
    static constexpr size_t sizeOfFieldsWithObservableReads = (static_cast<size_t>(Structure::numberOfSlotsWithFieldIDs) << 16) / 8;
    static constexpr ptrdiff_t offsetOfFieldsWithObservableReads() { return OBJECT_OFFSETOF(Instance, fieldsWithObservableReads); }
    void noteObservableRead(unsigned slot, uint16_t id) { fieldsWithObservableReads[(slot << 16 | id) >> 3] |= 1 << (id & 7); }
    JSCell* functionPrototypeCall { nullptr };
    uint32_t boundFunctionStructureID { 0 };
    uint32_t effectEpoch { 0 };
    static constexpr ptrdiff_t offsetOfEffectEpoch() { return OBJECT_OFFSETOF(Instance, effectEpoch); }
    static constexpr ptrdiff_t offsetOfFunctionPrototypeCall() { return OBJECT_OFFSETOF(Instance, functionPrototypeCall); }
    static constexpr ptrdiff_t offsetOfBoundFunctionStructureID() { return OBJECT_OFFSETOF(Instance, boundFunctionStructureID); }
    uint8_t* selectorsOnObjectPrototype;
    uint32_t objectPrototypeStructureID;
    EncodedJSValue intrinsics[ImmutableIntrinsics::maximumCount];
    EncodedJSValue linkTimeConstants[numberOfLinkTimeConstants];
    static constexpr unsigned numberOfReceivers = 16;
    uint32_t receiverStructureIDs[numberOfReceivers] { };
    static constexpr ptrdiff_t offsetOfReceiverStructureIDs() { return OBJECT_OFFSETOF(Instance, receiverStructureIDs); }
    static constexpr unsigned arrayKindShift = 1;
    static constexpr unsigned numberOfArrayKinds = 16;
    static_assert(((IndexingShapeMask | CopyOnWrite) >> arrayKindShift) == numberOfArrayKinds - 1);
    uint32_t originalArrayStructureIDs[numberOfArrayKinds] { };
    static constexpr ptrdiff_t offsetOfOriginalArrayStructureIDs() { return OBJECT_OFFSETOF(Instance, originalArrayStructureIDs); }
    uint32_t regExpMatchesArrayStructureIDs[2] { };
    static constexpr ptrdiff_t offsetOfRegExpMatchesArrayStructureIDs() { return OBJECT_OFFSETOF(Instance, regExpMatchesArrayStructureIDs); }
    uint32_t newArrayWithInt32StructureID { 0 };
    uint32_t newArrayWithContiguousStructureID { 0 };
    uint32_t newCopyOnWriteArrayStructureIDs[3] { };
    uint32_t activationStructureID { 0 };
    static constexpr ptrdiff_t offsetOfNewArrayWithInt32StructureID() { return OBJECT_OFFSETOF(Instance, newArrayWithInt32StructureID); }
    static constexpr ptrdiff_t offsetOfNewArrayWithContiguousStructureID() { return OBJECT_OFFSETOF(Instance, newArrayWithContiguousStructureID); }
    static constexpr ptrdiff_t offsetOfNewCopyOnWriteArrayStructureIDs() { return OBJECT_OFFSETOF(Instance, newCopyOnWriteArrayStructureIDs); }
    static constexpr ptrdiff_t offsetOfActivationStructureID() { return OBJECT_OFFSETOF(Instance, activationStructureID); }
    uint32_t arraysLackIsConcatSpreadable { 0 };
    uint32_t arraysLackInheritedElements { 0 };
    static constexpr ptrdiff_t offsetOfArraysLackIsConcatSpreadable() { return OBJECT_OFFSETOF(Instance, arraysLackIsConcatSpreadable); }
    static constexpr ptrdiff_t offsetOfArraysLackInheritedElements() { return OBJECT_OFFSETOF(Instance, arraysLackInheritedElements); }
    void* auxiliarySpace { nullptr };
    void* activationSpace { nullptr };
    void* arrayAllocator { nullptr };
    void* ropeStringAllocator { nullptr };
    void* singleCharacterStrings { nullptr };
    JSCell* emptyString { nullptr };
    JSCell* arrayIterationSentinel { nullptr };
    static constexpr ptrdiff_t offsetOfArrayIterationSentinel() { return OBJECT_OFFSETOF(Instance, arrayIterationSentinel); }
    JSCell* sentinelString { nullptr };
    static constexpr ptrdiff_t offsetOfSentinelString() { return OBJECT_OFFSETOF(Instance, sentinelString); }
    uint32_t stringStructureID { 0 };
    static constexpr ptrdiff_t offsetOfAuxiliarySpace() { return OBJECT_OFFSETOF(Instance, auxiliarySpace); }
    static constexpr ptrdiff_t offsetOfActivationSpace() { return OBJECT_OFFSETOF(Instance, activationSpace); }
    static constexpr ptrdiff_t offsetOfArrayAllocator() { return OBJECT_OFFSETOF(Instance, arrayAllocator); }
    static constexpr ptrdiff_t offsetOfRopeStringAllocator() { return OBJECT_OFFSETOF(Instance, ropeStringAllocator); }
    static constexpr ptrdiff_t offsetOfSingleCharacterStrings() { return OBJECT_OFFSETOF(Instance, singleCharacterStrings); }
    static constexpr ptrdiff_t offsetOfEmptyString() { return OBJECT_OFFSETOF(Instance, emptyString); }
    static constexpr ptrdiff_t offsetOfStringStructureID() { return OBJECT_OFFSETOF(Instance, stringStructureID); }
    JS_EXPORT_PRIVATE void didHaveBadTime();
    struct FieldAddition {
        uint32_t structureID;
        uint32_t slot;
        uint32_t structureIDAfterAddition;
        uint32_t unused;
    };
    static constexpr unsigned numberOfFieldAdditions = 1024;
    static constexpr unsigned fieldAdditionIndex(uint32_t structureID, unsigned slot) { return ((structureID >> 4) ^ (slot * 0x9e5u)) & (numberOfFieldAdditions - 1); }
    FieldAddition fieldAdditions[numberOfFieldAdditions] { };
    static constexpr ptrdiff_t offsetOfFieldAdditions() { return OBJECT_OFFSETOF(Instance, fieldAdditions); }
    void noteFieldAddition(Structure* before, unsigned slot, Structure* afterwards);
    struct CustomGetter {
        uint32_t structureID;
        uint16_t epoch;
        bool passesHolder;
        UniquedStringImpl* uid;
        void* getter;
        JSObject* holder;
    };
    static constexpr unsigned numberOfCustomGetters = 128;
    CustomGetter customGetters[numberOfCustomGetters] { };
    CustomGetter& customGetterFor(uint32_t structureID, UniquedStringImpl* uid) { return customGetters[((structureID >> 4) ^ static_cast<uint32_t>(std::bit_cast<uintptr_t>(uid) >> 4)) % numberOfCustomGetters]; }
    struct CachedAddressInfo {
        static constexpr uint32_t siteNotResolved = std::numeric_limits<uint32_t>::max();
        static constexpr uint32_t hasNoSite = siteNotResolved - 1;
        const void* address;
        uint32_t function;
        uint32_t site;
    };
    static constexpr unsigned numberOfCachedAddressInfos = 512;
    CachedAddressInfo cachedAddressInfos[numberOfCachedAddressInfos] { };
    CachedAddressInfo& cachedAddressInfo(const void* address) { return cachedAddressInfos[(std::bit_cast<uintptr_t>(address) >> 2) % numberOfCachedAddressInfos]; }
    uint32_t uncountedOperations { 0 };
    const void* overriddenReturnAddress { nullptr };
    uint32_t overridingSite { 0 };
    uint32_t states[0];
};

JS_EXPORT_PRIVATE const RegisterAtOffsetList* calleeSaveRegistersOf(const ImageFunction&);

struct Data {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(Data);

    static Data* create(Instance&, ScriptExecutable*, UnlinkedCodeBlock*, JITCode&, CodeBlock* = nullptr);
    static void destroy(Data*);
    void noteFilled();
    template<typename Visitor> void visit(Visitor&);

    JS_EXPORT_PRIVATE CodeBlock* ensureCodeBlock();
    FunctionRef function() const;
    FunctionExecutable* functionDecl(unsigned);
    FunctionExecutable* functionExpr(unsigned);

    void finalizeUnconditionally(VM&);
    void finalizeSlot(VM&, Slot&);

    static constexpr ptrdiff_t offsetOfSites() { return OBJECT_OFFSETOF(Data, sites); }
    static constexpr ptrdiff_t offsetOfSlots() { return OBJECT_OFFSETOF(Data, slots); }
    static constexpr ptrdiff_t offsetOfSlotEpoch() { return OBJECT_OFFSETOF(Data, slotEpoch); }
    static constexpr ptrdiff_t offsetOfNumSlots() { return OBJECT_OFFSETOF(Data, numSlots); }
    static constexpr ptrdiff_t offsetOfHasSiteConstants() { return OBJECT_OFFSETOF(Data, hasSiteConstants); }
    static constexpr ptrdiff_t offsetOfHasBeenFilledSinceLastCollection() { return OBJECT_OFFSETOF(Data, hasBeenFilledSinceLastCollection); }

    CodeBlock* codeBlock;
    Instance* instance;
    ScriptExecutable* executable;
    UnlinkedCodeBlock* unlinkedCodeBlock;
    JITCode* code;
    FunctionExecutable** functions;
    const Site* sites;
    SlotWatchpointMap* watchpoints;
    unsigned numSlots;
    bool hasBeenFilledSinceLastCollection;
    bool hasSiteConstants;
    bool hasSitesInMegamorphicCache;
    unsigned indexInAllList;
    unsigned indexInFilledList;
    uint64_t slotEpoch;
    Slot slots[0];
};

struct FunctionMetadata {
    enum Section : uint32_t {
        ExpressionInfo = 1 << 0,
        Handlers = 1 << 1,
        FunctionDecls = 1 << 2,
        FunctionExprs = 1 << 3,
        StringSwitchJumpTables = 1 << 4,
        ResumePoints = 1 << 5,
        ConstantIdentifierSets = 1 << 6,
        Scalars = 1 << 7,
    };
    static constexpr uint32_t isBuiltinFunction = 1 << 8;
    static constexpr unsigned instructionsSizeShift = 9;
    static constexpr uint32_t executableListEntry(uint32_t number) { return (number + 1) << 1; }
    static constexpr uint32_t unlinkedFunctionListEntry(uint32_t number) { return (number + 1) << 1 | 1; }
    static unsigned wordsFor(Section section) { return section == Handlers || section == FunctionDecls || section == FunctionExprs ? 2 : 1; }

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

struct CalleeCache {
    static constexpr uint32_t hasGivenUp = 1;
    static constexpr uint32_t attempt = 2;
    static constexpr uint32_t maxAttempts = 5;
    static constexpr unsigned numberOfSlots = 2;
};

struct SharedData {
    static constexpr unsigned maxSlots = 8192;
    static constexpr size_t size = sizeof(Data) + maxSlots * sizeof(Slot);
    JS_EXPORT_PRIVATE static Data* NODELETE get();
    static bool contains(const Slot* slot) { return std::bit_cast<uintptr_t>(slot) - std::bit_cast<uintptr_t>(get()) < size; }
};

inline const FunctionInfo* ProgramData::infos() const { return at<FunctionInfo>(infosOffset); }
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
    CString text;
};

struct CompiledFunctionInfo {
    unsigned codeSize { 0 };
    Convention convention;
    Vector<IndexReference> indexReferences;
    Vector<SpreadSite> spreadSites;
    struct InlineFrame {
        uint32_t parent;
        uint32_t callSite;
        uint32_t knownCallee;
        bool isTailCall;
    };
    Vector<InlineFrame> inlineFrames;
    Vector<ImageKey> functionExpressionsInCode;
    Vector<ImageKey> functionsCreated;
    bool isOnlyCalledDirectly { false };
    uint32_t numberOfFunction { 0 };
    unsigned frameSizeInBytes { 0 };
    unsigned numSlots { 0 };
    bool usesStaticImports { false };
    bool startsCold { false };
    bool isGetByValOnThis { false };
    std::optional<std::pair<uint32_t, uint32_t>> returnedVariable;
    RegisterAtOffsetList calleeSaveRegisters;
    Vector<std::pair<unsigned, unsigned>> catchEntrypoints;
    Vector<StubCall> stubCalls;
    Vector<Site> sites;
    Vector<ImageKey> knownCallees;
    static constexpr uint32_t siteConstantIsShape = 1u << 31;
    static constexpr uint32_t siteConstantIsPlan = 1u << 30;
    Vector<uint32_t> siteConstants;
    Vector<uint32_t> plans;
    Vector<uint32_t> quotableSites;
    Vector<uint32_t> constructSites;
    Vector<uint32_t> callSites;
    Vector<uint32_t> identifierIndices;
    Vector<uint32_t> constantIndices;
    Vector<std::pair<uint32_t, uint32_t>> constructionStarts;
    Vector<Quote> quotes;
    Vector<UniquedStringImpl*> selectors;
    Vector<KnownShape> shapes;
};

struct ImageCatchEntrypoint {
    uint32_t bytecodeOffset;
    uint32_t codeOffset;
};

struct ImageFrame {
    uint32_t calleeSaveRegisters;
    uint16_t calleeSavesStart;
    uint16_t frameSizeInUnits;

    unsigned frameSizeInBytes() const { return frameSizeInUnits * stackAlignmentBytes(); }
    uint64_t bits() const { return static_cast<uint64_t>(calleeSaveRegisters) << 32 | static_cast<uint64_t>(calleeSavesStart) << 16 | frameSizeInUnits; }
};
static_assert(sizeof(ImageFrame) == 8);

struct ImageFunction {
    uint32_t index;
    uint32_t numSlots;
    uint32_t quotes;
    uint32_t callSites;
    uint32_t numberOfKnownCallees : 17;
    uint32_t frame : 15;
    uint16_t numberOfCatchEntrypoints;
    uint8_t numberOfParameters;
    uint8_t takesList : 1;
    uint8_t hasInlineFrames : 1;
    uint8_t hasSiteConstants : 1;
    uint8_t usesStaticImports : 1;
    uint8_t startsCold : 1;
    uint8_t isGetByValOnThis : 1;
    uint8_t returnsScopeVariable : 1;

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

    const Site* sites() const { return reinterpret_cast<const Site*>(this + 1); }
    const uint32_t* siteConstants() const { return reinterpret_cast<const uint32_t*>(sites() + numSlots); }
    const uint32_t* knownCallees() const { return siteConstants() + (hasSiteConstants ? numSlots : 0); }
    const ImageCatchEntrypoint* catchEntrypoints() const { return reinterpret_cast<const ImageCatchEntrypoint*>(knownCallees() + numberOfKnownCallees); }
    const uint32_t* returnedVariable() const { return reinterpret_cast<const uint32_t*>(catchEntrypoints() + numberOfCatchEntrypoints); }
    const uint32_t* plans() const { return returnedVariable() + (returnsScopeVariable ? 2 : 0); }
    static constexpr uint32_t noSuchFunction = std::numeric_limits<uint32_t>::max();
};

static_assert(sizeof(ImageFunction) == 24);

inline const ImageFunction* FunctionInfo::function() const
{
    return hasCodeInImage() ? reinterpret_cast<const ImageFunction*>(sitesInImage()) - 1 : nullptr;
}

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
bool install(VM&, FunctionExecutable*, CodeSpecializationKind, UnlinkedCodeBlock*, JSScope*, Ref<JITCode>&&);
bool linkStaticFunction(Instance*, FunctionExecutable*, CodeSpecializationKind, JSScope*);
bool linkColdStaticFunction(Instance*, uint32_t functionIndex, JSScope*);

JS_EXPORT_PRIVATE CodePtr<JSEntryPtrTag> moduleCodeEntrypoint();
void* catchThunk();
void* stubAddress(Stub);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
