/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(FTL_JIT)

#include "BytecodeIndex.h"
#include "LineColumn.h"
#include "AOTFunction.h"
#include "AOTOperationsBuiltins.h"
#include "AOTOperationsObjects.h"
#include "IndexingType.h"
#include "AOTSlotWatchpoint.h"
#include "AOTStubs.h"
#include "CallLinkInfo.h"
#include "ExecutableAllocator.h"
#include "ImmutableIntrinsics.h"
#include "JITCode.h"
#include "LinkTimeConstant.h"
#include "Opcode.h"
#include "RegisterAtOffsetList.h"
#include "StructureID.h"
#include <wtf/TZoneMalloc.h>
#include <bmalloc/StaticRegion.h>

namespace JSC {

class CodeBlock;
class JSGlobalObject;
class CallFrame;
class FunctionExecutable;
class ScriptExecutable;
class UnlinkedCodeBlock;
class VM;

namespace AOT {

// Code from the static compiler has no address in it: not of the VM, not of a C++ function, not of a thunk. Everything it needs
// that it was not passed it finds from the Instance, which is in a register all the while (AOTConvention.h).
//
//     Instance -> runtimeTable[Entry]              C++ operations and thunks: one table per VM
//              -> vm, globalObject
//              -> states[index of the function] -> its Data -> constants[i]
//                                             -> identifiers[i]
//                                             -> slots[i]      the function's inline caches
//
// So the same bytes run wherever they are mapped, in any VM of any process.

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
    v(operationAOTAssertBornAs) \
    v(operationAOTViewAs) \
    v(operationAOTNarrowAtomThatSaysTheSame) \
    v(operationAOTGetFieldTheLongWay) \
    v(operationAOTGetLengthTheLongWay) \
    v(operationAOTSettleStruct) \
    v(operationAOTVerifyFact) \
    v(operationAOTHandleTraps) \
    v(operationAOTWriteBarrier) \
    v(operationAOTCatch) \
    v(operationAOTSwitchString) \
    v(operationAOTSwitchChar) \
    v(operationAOTFMod) \
    v(operationAOTPow) \
    v(operationAOTDoubleToInt32) \
    FOR_EACH_AOT_OBJECT_OPERATION(v) \
    FOR_EACH_AOT_BUILTIN_OPERATION(v) \
    FOR_EACH_AOT_OPERATION_OF_THE_OTHER_TIERS(v) \

// What the DFG and the FTL call for a method whose receiver and arguments they know the types of (DirectMethod).
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
    v(MegamorphicCache) \
    /* Host functions that compiled code knows when it sees them (CallIntrinsic). */ \
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
    /* And those that a stub knows when a call gets to it (StubIntrinsic). */ \
    v(HostStringCodePointAt) \
    v(HostStringCharAt) \
    v(HostArrayPop) \
    v(HostArrayIsArray) \
    v(HostMapGet) \
    v(HostMapHas) \
    v(HostMapSet) \
    v(HostSetHas) \
    v(HostSetAdd) \
    /* The operations that have something in front of them (AOTThunks.h). */ \
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
    /* FOR_EACH_AOT_OPERATION_BEHIND_HELPER */ \
    v(BehindNewArray) \
    v(BehindNewArrayBuffer) \
    v(BehindNewArrayWithSpread) \
    v(BehindNewArrayWithSpecies) \
    v(BehindCreateRest) \
    v(BehindCreateLexicalEnvironment) \
    v(BehindMakeRope2) \
    v(BehindMakeRope3) \
    v(BehindStringSliceWithEnd) \
    v(BehindStringSubstringWithEnd) \
    v(BehindToLowerCase) \
    v(BehindObjectKeysObject) \
    v(BehindValueAdd) \

enum class Entry : uint16_t {
#define AOT_DEFINE_ENTRY(name) name,
    FOR_EACH_AOT_OPERATION(AOT_DEFINE_ENTRY)
    FOR_EACH_AOT_THUNK(AOT_DEFINE_ENTRY)
    FOR_EACH_AOT_POINTER(AOT_DEFINE_ENTRY)
#undef AOT_DEFINE_ENTRY
    NumberOfEntries
};

static constexpr unsigned numberOfEntries = static_cast<unsigned>(Entry::NumberOfEntries);

// What the stubs that call whatever they are given take a CallLinkInfo to be part of. When the callee of a tail call turns out to
// take finding out about, the caller's frame, from which everything else is found, is gone.
struct VirtualCallInfo {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(VirtualCallInfo);

    static constexpr ptrdiff_t offsetOfFindTarget() { return OBJECT_OFFSETOF(VirtualCallInfo, findTarget); }
    static constexpr ptrdiff_t offsetOfLookupExceptionHandler() { return OBJECT_OFFSETOF(VirtualCallInfo, lookupExceptionHandler); }

    DataOnlyCallLinkInfo callLinkInfo;
    void* findTarget; // llint_virtual_call()
    void* lookupExceptionHandler; // operationLookupExceptionHandler()
};

// One per VM, made when the VM first runs code from the static compiler.
class RuntimeTable {
    WTF_MAKE_TZONE_ALLOCATED(RuntimeTable);
    WTF_MAKE_NONCOPYABLE(RuntimeTable);
public:
    explicit RuntimeTable(VM&);
    ~RuntimeTable();

    void** entries() { return m_entries; }

private:
    void* m_entries[numberOfEntries]; // First: see VM::offsetOfAOTRuntimeTable().
    Vector<std::unique_ptr<VirtualCallInfo>> m_callLinkInfos;
};

RuntimeTable& runtimeTable(VM&);

// An inline cache: data that the code reads, and that the operation the code falls back to writes. All zero means empty, and
// no structure has ID zero.
//
// The collector looks at every slot that has a structureID, without knowing what kind of cache it is: cells are held weakly
// (Data::finalizeUnconditionally()), and a cached transition keeps the new structure alive while the old one is
// (CodeBlock::propagateTransitions()). So such a slot has to say what its second word is, which it does in the bits of offset
// that a PropertyOffset has no use for. A slot without a structureID is nobody's business but its owner's.
// What a StructureID is the low half of an address with. Structures are where StructureMemoryManager asks for them to be, which is right after the static region: as with that, whoever
// is there first gets in the way of the program running at all.
static constexpr uintptr_t structureIDBaseOfImages = bmalloc::StaticRegion::base + bmalloc::StaticRegion::reservation;
static_assert(!(structureIDBaseOfImages & 0xffffffff));

struct Slot {
    static constexpr unsigned offsetBits = 24;
    static constexpr uint32_t offsetMask = (1u << offsetBits) - 1;
    static constexpr unsigned attemptsShift = 24; // How often a cache that is expensive to set up has been (see cacheGetById()).
    static constexpr uint32_t maxAttempts = 15;
    static constexpr uint32_t attemptsMask = maxAttempts << attemptsShift;
    static constexpr uint32_t isIntricate = 1u << 28; // op_get_by_id, op_put_by_id: there is more to it than a load or a store at that place in the base itself.
    static constexpr uint32_t isGetter = 1u << 29; // op_get_by_id: what is at that place is a GetterSetter, whose getter has the answer.
    static constexpr uint32_t saysWhatIsHeld = 1u << 29; // op_put_by_id: what is above newStructureID is `held`, and not the rest of an address.
    static constexpr uint32_t pointerIsNotCell = 1u << 30; // pointer: something that is there for as long as the VM is.
    static constexpr uint32_t pointerIsCell = 1u << 31; // pointer: a cell. Neither: newStructureID, which may be none.
    static constexpr uint32_t resolvesByDepth = 1u << 31; // op_resolve_scope: the rest of offset is how many scopes out it is.

    bool hasPointer() const { return offset & (pointerIsCell | pointerIsNotCell); }
    // op_get_by_id: the place has seen more than one structure. No structure, and pointer is a SlotsOfSite.
    static constexpr uint32_t flagsMask = isIntricate | isGetter | pointerIsNotCell | pointerIsCell;
    static constexpr uint32_t flagsIfOfSeveral = isIntricate | pointerIsNotCell;
    bool isOfSeveral() const { return !structureID && (offset & flagsMask) == flagsIfOfSeveral; }

    void clear()
    {
        structureID = StructureID();
        offset &= attemptsMask;
        pointer = nullptr;
    }

    StructureID structureID; // What the base's structure has to be.
    uint32_t offset; // Where the property is (see locationOfProperty()), or whatever the kind of cache wants.
    union {
        void* pointer; // Global variable caches: the address of the variable. Prototype hits: the holder.
        UniquedStringImpl* name; // op_get_by_id, unless isIntricate: what is read, if the stub has found that out. It looks in the megamorphic cache with it.
        struct {
            StructureID newStructureID; // Transitions.
            uint32_t held; // op_put_by_id, if not zero: PutPropertySlot::held(). Only that may be stored.
        };
    };
};
static_assert(sizeof(Slot) == 16);

struct Data;

// What a place that reads a property has once it has seen objects of more than one structure (Slot::isOfSeveral()): a slot for each of the last few. Each is a slot like any other: to the stub, once it
// has found the one that is for the structure, to the collector, and to what watches on a slot's behalf.
struct SlotsOfSite {
    static constexpr unsigned numberOfSlots = 4;
    // When it is none of them the megamorphic cache is asked, which is quicker than finding out and remembering. So that is only done now and then: at once to begin with...
    static constexpr uint32_t timesToLearnAtOnce = 12;
    // ...and after that once in so many.
    static constexpr uint32_t missesBetweenLearning = 1024;
    static_assert(hasOneBitSet(missesBetweenLearning));

    static constexpr ptrdiff_t offsetOfName() { return OBJECT_OFFSETOF(SlotsOfSite, name); }
    static constexpr ptrdiff_t offsetOfMisses() { return OBJECT_OFFSETOF(SlotsOfSite, misses); }
    static constexpr ptrdiff_t offsetOfTimesLeftToLearnAtOnce() { return OBJECT_OFFSETOF(SlotsOfSite, timesLeftToLearnAtOnce); }
    static constexpr ptrdiff_t offsetOfSlots() { return OBJECT_OFFSETOF(SlotsOfSite, slots); }

    UniquedStringImpl* name; // What is read.
    uint32_t misses; // The stub counts.
    uint32_t timesLeftToLearnAtOnce;
    Data* owner;
    uint32_t next; // Which makes way, when there is no room.
    Slot slots[numberOfSlots];
};

struct Data;
struct ImageEnvironment;
struct ImageFunction;
struct Site;

// What the stubs, and the code itself, want to know about a function that stays as it is for as long as the function is there. By
// the function's index: Instance::infos.
struct FunctionInfo {
    static constexpr uint16_t hasSiteConstants = 1; // After the last of the sites: ImageFunction::siteConstants().
    static constexpr uint16_t startsCold = 2; // See CompiledFunctionInfo::startsCold.
    static constexpr uint16_t sitesHaveTheirConstants = 4; // Or where a site has its identifier, which is the constant if it has one.

    // Above those: how many slots it has, or as many as can be said.
    static constexpr unsigned numberOfFlagBits = 3;
    static constexpr uint32_t mostSlotsSaid = (1u << (16 - numberOfFlagBits)) - 1;
    static constexpr uint16_t slotsAmongFlags(uint32_t numSlots) { return static_cast<uint16_t>(std::min(numSlots, mostSlotsSaid) << numberOfFlagBits); }

    static constexpr ptrdiff_t offsetOfConstants() { return OBJECT_OFFSETOF(FunctionInfo, constants); }
    static constexpr ptrdiff_t offsetOfIdentifiers() { return OBJECT_OFFSETOF(FunctionInfo, identifiers); }
    static constexpr ptrdiff_t offsetOfSites() { return OBJECT_OFFSETOF(FunctionInfo, sites); }
    static constexpr ptrdiff_t offsetOfFlags() { return OBJECT_OFFSETOF(FunctionInfo, flags); }

    // The executable may not be known: it takes a Data to say which it is then.
    void setExecutable(ScriptExecutable* executable, CodeSpecializationKind kind, bool constantsAreOfNoRealm)
    {
        uintptr_t bits = std::bit_cast<uintptr_t>(executable);
        RELEASE_ASSERT(!(bits >> 48) && !(bits & 7));
        bits |= (kind == CodeSpecializationKind::CodeForConstruct ? 1 : 0) | (constantsAreOfNoRealm ? 2 : 0);
        executableAndMoreLow = static_cast<uint32_t>(bits);
        executableAndMoreHigh = static_cast<uint16_t>(bits >> 32);
    }
    ScriptExecutable* executable() const { return std::bit_cast<ScriptExecutable*>((static_cast<uintptr_t>(executableAndMoreHigh) << 32 | executableAndMoreLow) & ~static_cast<uintptr_t>(7)); }
    CodeSpecializationKind kind() const { return executableAndMoreLow & 1 ? CodeSpecializationKind::CodeForConstruct : CodeSpecializationKind::CodeForCall; }
    bool constantsAreOfNoRealm() const { return executableAndMoreLow & 2; } // `constants` are all there are, if any.
    bool isOfCodeInImage() const { return flags & (hasSiteConstants | sitesHaveTheirConstants); }
    inline const ImageFunction* function() const; // If the code is in an image: what comes right before its sites.

    const void* constants; // const WriteBarrier<Unknown>*. Unless constantsAreOfNoRealm, the Data has them.
    const void* identifiers; // const Identifier*
    const Site* sites; // One for each slot.
    uint32_t executableAndMoreLow; // See setExecutable().
    uint16_t executableAndMoreHigh;
    uint16_t flags;
};
static_assert(sizeof(FunctionInfo) == 32);

// One for each realm that runs code from the static compiler.
struct Instance {
    static Instance& ensure(JSGlobalObject*);
    static bool adopt(VM&, JSObject*, uint16_t family); // SlotsOfBornObjects::Adopt
    Structure* emptyStructureOfFamily(uint16_t family);
    Structure* emptyStructureOfFamily(uint16_t family, JSObject* prototype); // A new one: whoever asks keeps it.
    static JSObject* newObjectOf(VM&, Structure*); // With nothing in it, and room outside it if the Structure has slots there.
    static void destroy(Instance*);

    // For the collector. Nothing is kept alive because a slot refers to it.
    template<typename Visitor> void visit(Visitor&, bool onlyWhatIsNew);
    void finalizeUnconditionally(bool onlyWhatIsNew);

    static constexpr ptrdiff_t offsetOfRuntimeTable() { return OBJECT_OFFSETOF(Instance, runtimeTable); }
    static constexpr ptrdiff_t offsetOfVM() { return OBJECT_OFFSETOF(Instance, vm); }
    static constexpr ptrdiff_t offsetOfGlobalObject() { return OBJECT_OFFSETOF(Instance, globalObject); }
    static constexpr ptrdiff_t offsetOfStates() { return OBJECT_OFFSETOF(Instance, states); }
    static constexpr ptrdiff_t offsetOfInfos() { return OBJECT_OFFSETOF(Instance, infos); }
    static constexpr ptrdiff_t offsetOfConstantsOfProgram() { return OBJECT_OFFSETOF(Instance, constantsOfProgram); }
    static constexpr ptrdiff_t offsetOfSharedData() { return OBJECT_OFFSETOF(Instance, sharedData); }
    static constexpr ptrdiff_t offsetOfCode() { return OBJECT_OFFSETOF(Instance, code); }
    static constexpr ptrdiff_t offsetOfGranulesOfCode() { return OBJECT_OFFSETOF(Instance, granulesOfCode); }
    static constexpr ptrdiff_t offsetOfStartsOfFunctionsAfterFirst() { return OBJECT_OFFSETOF(Instance, startsOfFunctionsAfterFirst); }
    // The function's own Data, which it gets now if it has been doing without (SharedData). It has been linked.
    JS_EXPORT_PRIVATE Data* ensureData(uint32_t index);
    // For one that has none of its own.
    void countMisses(uint32_t index, uint32_t count)
    {
        uint32_t state = states[index];
        ASSERT(state < leastStateWithData);
        uint32_t before = state & mostMisses;
        uint32_t after = std::min<uint32_t>(before + count, mostMisses);
        states[index] = (state & ~mostMisses) | after;
        uint32_t limit = static_cast<uint16_t>(missesToPutUpWithFor(infos[index].flags >> FunctionInfo::numberOfFlagBits));
        if (before < limit && after >= limit)
            ensureData(index);
    }

    // See states.
    static constexpr uint32_t mostMisses = 0xffff;
    static constexpr uint32_t isLinkedWithoutData = 1u << 16;
    static constexpr uint32_t leastStateWithData = 1u << 17;
    static constexpr unsigned shiftOfStateWithData = 4;
    bool isLinked(uint32_t index) const { return states[index] >= isLinkedWithoutData; }
    Data* dataIfItHasAny(uint32_t index) const
    {
        uint32_t state = states[index];
        if (state < leastStateWithData)
            return nullptr;
        return std::bit_cast<Data*>(std::bit_cast<uintptr_t>(this) + (static_cast<uintptr_t>(state) << shiftOfStateWithData));
    }
    void setLinkedWithoutData(uint32_t index)
    {
        ASSERT(states[index] < isLinkedWithoutData);
        states[index] |= isLinkedWithoutData;
    }
    void setData(uint32_t index, Data* data)
    {
        uintptr_t distance = std::bit_cast<uintptr_t>(data) - std::bit_cast<uintptr_t>(this);
        RELEASE_ASSERT(!(distance & ((1u << shiftOfStateWithData) - 1)) && distance >> shiftOfStateWithData >= leastStateWithData && !(distance >> shiftOfStateWithData >> 32));
        states[index] = static_cast<uint32_t>(distance >> shiftOfStateWithData);
    }
    void setNotLinked(uint32_t index) { states[index] = 0; }
    // A slot has been given a transition: from one structure to another, which is to be kept for as long as the first is.
    void noteTransitionCached(Slot*);
    SlotsOfSite* makeSlotsOfSite(Data*, UniquedStringImpl* name); // The Data's, for as long as that is there.
    // Where the Datas are: after the Instance, so that it takes half a word to say where one is. Zeroed.
    void* allocateForData(size_t);
    void freeOfData(void*, size_t);
    // How often a slot may fail a function that has that many before it gets a Data.
    uint32_t missesToPutUpWithFor(uint32_t numSlots) const { return std::min<uint32_t>((numSlots * missesForEightSlots >> 3) + missesToSpare, std::numeric_limits<uint16_t>::max()); }
    static constexpr ptrdiff_t offsetOfMissesForEightSlots() { return OBJECT_OFFSETOF(Instance, missesForEightSlots); }
    static constexpr ptrdiff_t offsetOfMissesToSpare() { return OBJECT_OFFSETOF(Instance, missesToSpare); }

    // As many as there could ever be. It is addresses that are set aside, not memory.
    static constexpr size_t maxFunctions = 4 << 20;

    // Below it, if the program's image says so, is where the environments of the program's modules are: each at the same distance
    // in every realm, so that code gets at a variable of a module from here, without looking for the module. Null: this realm's
    // are wherever the collector put them, and code that does that is not for this realm.
    JS_EXPORT_PRIVATE void* placeForEnvironment(ImageEnvironment) const;

    // Structure::createWithProperties() of the structure of an empty object literal with that inline capacity: the same one for
    // the same names. They stay.
    Structure* structureOfLiteral(Structure* empty, std::span<UniquedStringImpl* const>);

    // The Structure of a shape that the image numbers (ImageShape), which says so (Structure::knownShape()). It is made when it is
    // first asked for, and stays. names: its properties', which whoever asks has at hand.
    Structure* structureOfKnownShape(uint32_t shape, std::span<UniquedStringImpl* const> names);
    // The slot of each of its properties, if they are not one after the other (KnownShape::slots).
    std::span<const uint16_t> slotsOfKnownShape(uint32_t shape) const;
    static constexpr ptrdiff_t offsetOfStructureIDBase() { return OBJECT_OFFSETOF(Instance, structureIDBase); }
    // What an object of a known shape does not have itself, it does not have at all if it inherits from Object.prototype alone and
    // that does not have it either. This looks at what that has now, if it is not what it had when this last looked.
    void lookAtObjectPrototype();
    static constexpr ptrdiff_t offsetOfIntrinsics() { return OBJECT_OFFSETOF(Instance, intrinsics); }
    static constexpr ptrdiff_t offsetOfLinkTimeConstants() { return OBJECT_OFFSETOF(Instance, linkTimeConstants); }
    static constexpr ptrdiff_t offsetOfObjectPrototype() { return OBJECT_OFFSETOF(Instance, objectPrototype); }
    static constexpr ptrdiff_t offsetOfStructureIDOfObjectPrototype() { return OBJECT_OFFSETOF(Instance, structureIDOfObjectPrototype); }
    static constexpr ptrdiff_t offsetOfSelectorsOnObjectPrototype() { return OBJECT_OFFSETOF(Instance, selectorsOnObjectPrototype); }
    static constexpr ptrdiff_t offsetOfDispatch() { return OBJECT_OFFSETOF(Instance, dispatch); }
    static constexpr ptrdiff_t offsetOfRowsOfSelectors() { return OBJECT_OFFSETOF(Instance, rowsOfSelectors); }

    JS_EXPORT_PRIVATE void dumpSlotStatistics(PrintStream&); // TEMPORARY-SLOT-STATS
    static void noteView(ASCIILiteral whatCameOfIt); // TEMPORARY-SHAPE-COUNTS: operationAOTViewAs()

    void** runtimeTable;
    JSGlobalObject* globalObject;
    VM* vm; // Where a JSWebAssemblyInstance has its own: code that finds the VM from any frame need not tell the two apart.
    struct Collections;
    Collections* collections; // Of what there is in data.
    FunctionInfo* infos; // By the index of the function, like data.
    Data* sharedData; // SharedData::get()
    const uint32_t* factsOfFunctions; // By the index of the function: StaticHeap::factsAt(). Zero: none. Null: no function has any.
    // For telling which function an address is in (loadIndexOfFunctionAt(), Image::whatIsAt()): where the image's code is; for each
    // granule of it, the last function that starts no later than the granule does; and where each function but the first starts.
    const uint8_t* code;
    const uint32_t* granulesOfCode;
    const uint32_t* startsOfFunctionsAfterFirst;
    // (What code gets at comes first, where a load reaches it as it is. What is big, and is only looked at by the runtime, comes last.)
    // %TypedArray%.prototype's getter of `length`, once a slot has been filled with it (tryCacheGetById()): the stubs do what it does themselves. The prototype keeps it.
    JSCell* getterOfLengthOfTypedArrays { nullptr };
    static constexpr ptrdiff_t offsetOfGetterOfLengthOfTypedArrays() { return OBJECT_OFFSETOF(Instance, getterOfLengthOfTypedArrays); }
    const void* constantsOfProgram; // EncodedJSValue[]: see NumbersOfConstants. Code that goes by it is not given to a realm that has none.
    uint32_t missesForEightSlots; // Options::aotMissesForEightSlots()
    uint32_t missesToSpare;
    uintptr_t structureIDBase; // What a StructureID is added to.
    const uint32_t* dispatch; // The image's: see ImageDispatchEntry.
    const uint32_t* rowsOfSelectors;
    JSObject* objectPrototype; // The realm's, which keeps it.
    uint8_t* selectorsOnObjectPrototype; // A bit for each selector, as of when its Structure was the one below.
    uint32_t structureIDOfObjectPrototype; // Zero: nobody has looked, or there is no telling from its Structure.
    // The realm's (JSGlobalObject::immutableIntrinsics()), where code gets at them with one load.
    EncodedJSValue intrinsics[ImmutableIntrinsics::maximumCount];
    // The realm's (JSGlobalObject::linkTimeConstant()), which keeps them: those that code has asked for. It makes each when it is first wanted. Zero: not yet.
    EncodedJSValue linkTimeConstants[numberOfLinkTimeConstants];
    // The Structure that the realm makes such an object with (Receiver). One that still has it has been given nothing of its own, and inherits from what the realm made for it,
    // which stays as it is (Options::useImmutableIntrinsics()): so what a method of it is is known. Zero: there is none to go by.
    static constexpr unsigned numberOfReceivers = 16;
    uint32_t structureIDsOfReceivers[numberOfReceivers] { };
    static constexpr ptrdiff_t offsetOfStructureIDsOfReceivers() { return OBJECT_OFFSETOF(Instance, structureIDsOfReceivers); }
    // JSGlobalObject::originalArrayStructureForIndexingType(), by the bits of the indexing type that say how the elements are kept and whose they are.
    static constexpr unsigned shiftOfKindOfArray = 1;
    static constexpr unsigned numberOfKindsOfArray = 16;
    static_assert(((IndexingShapeMask | CopyOnWrite) >> shiftOfKindOfArray) == numberOfKindsOfArray - 1);
    uint32_t structureIDsOfOriginalArrays[numberOfKindsOfArray] { };
    static constexpr ptrdiff_t offsetOfStructureIDsOfOriginalArrays() { return OBJECT_OFFSETOF(Instance, structureIDsOfOriginalArrays); }
    // What code makes for itself, with no need to have made one before: the Structure. Zero: it is for the runtime to make (JSGlobalObject::haveABadTime()).
    uint32_t structureIDOfNewArrayWithInt32 { 0 };
    uint32_t structureIDOfNewArrayWithContiguous { 0 };
    uint32_t structureIDsOfNewCopyOnWriteArrays[3] { }; // Int32, Double, Contiguous.
    uint32_t structureIDOfActivation { 0 };
    static constexpr ptrdiff_t offsetOfStructureIDOfNewArrayWithInt32() { return OBJECT_OFFSETOF(Instance, structureIDOfNewArrayWithInt32); }
    static constexpr ptrdiff_t offsetOfStructureIDOfNewArrayWithContiguous() { return OBJECT_OFFSETOF(Instance, structureIDOfNewArrayWithContiguous); }
    static constexpr ptrdiff_t offsetOfStructureIDsOfNewCopyOnWriteArrays() { return OBJECT_OFFSETOF(Instance, structureIDsOfNewCopyOnWriteArrays); }
    static constexpr ptrdiff_t offsetOfStructureIDOfActivation() { return OBJECT_OFFSETOF(Instance, structureIDOfActivation); }
    // What the VM has that code allocates from and hands out. They are where they are for as long as there is a VM.
    void* auxiliarySpace { nullptr }; // CompleteSubspace*: where arrays keep their elements.
    void* spaceOfActivations { nullptr }; // CompleteSubspace*
    void* allocatorOfArrays { nullptr }; // LocalAllocator*
    void* allocatorOfRopeStrings { nullptr };
    void* singleCharacterStrings { nullptr }; // JSString*[]
    JSCell* emptyString { nullptr };
    JSCell* sentinelOfArrayIteration { nullptr }; // VM::fastArrayUnboxedSentinel()
    static constexpr ptrdiff_t offsetOfSentinelOfArrayIteration() { return OBJECT_OFFSETOF(Instance, sentinelOfArrayIteration); }
    JSCell* sentinelString { nullptr }; // SmallStrings::sentinelString(): what op_enumerator_next gives when there are no more.
    static constexpr ptrdiff_t offsetOfSentinelString() { return OBJECT_OFFSETOF(Instance, sentinelString); }
    uint32_t structureIDOfStrings { 0 };
    static constexpr ptrdiff_t offsetOfAuxiliarySpace() { return OBJECT_OFFSETOF(Instance, auxiliarySpace); }
    static constexpr ptrdiff_t offsetOfSpaceOfActivations() { return OBJECT_OFFSETOF(Instance, spaceOfActivations); }
    static constexpr ptrdiff_t offsetOfAllocatorOfArrays() { return OBJECT_OFFSETOF(Instance, allocatorOfArrays); }
    static constexpr ptrdiff_t offsetOfAllocatorOfRopeStrings() { return OBJECT_OFFSETOF(Instance, allocatorOfRopeStrings); }
    static constexpr ptrdiff_t offsetOfSingleCharacterStrings() { return OBJECT_OFFSETOF(Instance, singleCharacterStrings); }
    static constexpr ptrdiff_t offsetOfEmptyString() { return OBJECT_OFFSETOF(Instance, emptyString); }
    static constexpr ptrdiff_t offsetOfStructureIDOfStrings() { return OBJECT_OFFSETOF(Instance, structureIDOfStrings); }
    JS_EXPORT_PRIVATE void didHaveABadTime();
    // A struct is given a field it did not have: what it is of afterwards, by what it was of and which slot. Whoever finds it here has seen to it that the slot holds the value.
    // (Forgotten at every collection: nothing is kept for being here.)
    struct AddOfField {
        uint32_t structureID;
        uint32_t slot;
        uint32_t structureIDAfterwards;
        uint32_t unused;
    };
    static constexpr unsigned numberOfAddsOfFields = 1024;
    static constexpr unsigned indexOfAddOfField(uint32_t structureID, unsigned slot) { return ((structureID >> 4) ^ (slot * 0x9e5u)) & (numberOfAddsOfFields - 1); }
    AddOfField addsOfFields[numberOfAddsOfFields] { };
    static constexpr ptrdiff_t offsetOfAddsOfFields() { return OBJECT_OFFSETOF(Instance, addsOfFields); }
    void noteAddOfField(Structure* before, unsigned slot, Structure* afterwards);
    // A property that a function of the runtime's answers for (PropertySlot::isCacheableCustom()): which function, by the Structure of the object and the name. The function is called
    // every time. What is saved is finding it. On the conditions that the other tiers remember the same thing on (tryCacheGetBy()), and for as long as the megamorphic cache would.
    struct CustomGetter {
        uint32_t structureID;
        uint16_t epoch; // MegamorphicCache::epoch()
        bool isGivenHolder; // Not the object that was asked: PropertyAttribute::CustomAccessor is not set.
        UniquedStringImpl* uid;
        void* getter; // GetValueFunc
        JSObject* holder;
    };
    static constexpr unsigned numberOfCustomGetters = 128;
    CustomGetter customGetters[numberOfCustomGetters] { };
    CustomGetter& customGetterFor(uint32_t structureID, UniquedStringImpl* uid) { return customGetters[((structureID >> 4) ^ static_cast<uint32_t>(std::bit_cast<uintptr_t>(uid) >> 4)) % numberOfCustomGetters]; }
    // Addresses in the code that have been asked about (FunctionRef::at(), placeAt()), and the answers, which are the same every time.
    struct PlaceAskedAbout {
        static constexpr uint32_t siteNotLookedFor = std::numeric_limits<uint32_t>::max();
        static constexpr uint32_t hasNoSite = siteNotLookedFor - 1;
        const void* address;
        uint32_t function;
        uint32_t site;
    };
    static constexpr unsigned numberOfPlacesAskedAbout = 512;
    PlaceAskedAbout placesAskedAbout[numberOfPlacesAskedAbout] { };
    PlaceAskedAbout& placeAskedAbout(const void* address) { return placesAskedAbout[(std::bit_cast<uintptr_t>(address) >> 2) % numberOfPlacesAskedAbout]; }
    uint32_t operationsNotCounted { 0 }; // See countOperationOnBehalfOf().
    // SiteInPlaceOfCallSite
    const void* returnAddressWithSiteInPlace { nullptr };
    uint32_t siteInPlace { 0 };
    // TEMPORARY-ESCAPE-STATS: Options::aotCountsAllocations(). By AllocationKind and Escape: how many, and how many bytes.
    static constexpr unsigned numberOfAllocationCounts = 4 * 32 * 2;
    uint64_t allocationCounts[numberOfAllocationCounts] { };
    static constexpr ptrdiff_t offsetOfAllocationCounts() { return OBJECT_OFFSETOF(Instance, allocationCounts); }
    // TEMPORARY-SHAPE-COUNTS: likewise. What became of the accesses that go by a type.
    enum ShapeCount : unsigned { ReadHas, ReadLacks, ReadOther, ReadNotCell, WriteHas, WriteOther, LiteralWithLayout, LiteralWithout, ReadUntyped, WriteUntyped, ConstructedWithLayout, ConstructedWithout, AssertionMade, ServedWithoutAssertion, ExitTaken, AssertionRepeated, TakenOutAtBirth, ExitBaseIsNoCell, ExitBaseIsNoPlainObject, ExitBaseWasNeverBorn, ExitBaseWasNeverBornAndHasNoRoom, ExitBaseWasBornOtherwise, ExitSlotIsEmpty, ExitOther, Adopted, NumberOfShapeCounts };
    uint64_t shapeCounts[NumberOfShapeCounts] { };
    uint64_t readsForReason[1024] { };
    uint64_t pathsOfStubs[32] { }; // TEMPORARY: BUN_AOT_COUNTS_STUB_PATHS
    static constexpr ptrdiff_t offsetOfPathsOfStubs() { return OBJECT_OFFSETOF(Instance, pathsOfStubs); }
    static constexpr unsigned numberOfCountsOfSites = 8192; // TEMPORARY-SITE-COUNTS: kindOfSite()
    uint64_t countsOfSites[numberOfCountsOfSites] { };
    static constexpr ptrdiff_t offsetOfCountsOfSites() { return OBJECT_OFFSETOF(Instance, countsOfSites); }
    static constexpr ptrdiff_t offsetOfReadsForReason() { return OBJECT_OFFSETOF(Instance, readsForReason); }
    static constexpr ptrdiff_t offsetOfShapeCounts() { return OBJECT_OFFSETOF(Instance, shapeCounts); }
    // By the index of the function. Reading one is enough to have the page it is on, so they are small.
    //     Less than leastStateWithData: it has no Data of its own (SharedData). The low half is how often a slot has failed it, and
    //     isLinkedWithoutData whether it has been linked in this realm: one that only those call who know what they are calling need not be.
    //     From there up: it has been linked, and its Data is that many times sixteen bytes from the Instance.
    uint32_t states[0];
};

// TEMPORARY-SHAPE-STATS: structures whose layout the compiler could have known. 1: of an object literal. 2: what a constructor's stores end in.
void noteKnownShape(Structure*, uint8_t kind);
uint8_t kindOfKnownShape(Structure*);

// Whether the constants of the unlinked code will do for any realm as they are. A SymbolTable does if it is one that was made when the
// program was built (SymbolTable::isItsOwnClone()), which is not for the compiler to say: it comes first.
enum class SymbolTablesWillDo : bool { No, Yes };
JS_EXPORT_PRIVATE bool constantsAreOfNoRealm(UnlinkedCodeBlock*, SymbolTablesWillDo = SymbolTablesWillDo::No);

JS_EXPORT_PRIVATE const RegisterAtOffsetList* calleeSaveRegistersOf(const ImageFunction&);

struct Data {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(Data);

    // Puts it in its place in the Instance, and takes it out. The code of a program or a module comes with a CodeBlock, since that is
    // what the interpreter enters it with. A function does not. Null: an exception was thrown.
    static Data* create(Instance&, ScriptExecutable*, UnlinkedCodeBlock*, JITCode&, CodeBlock* = nullptr);
    static void destroy(Data*);
    void noteFilled();
    template<typename Visitor> void visit(Visitor&);

    // What the rest of the engine takes the function's frames to be running, for whoever asks: it is made then. Nothing that the
    // function itself does asks. Not while the collector is at work, and on no thread but the VM's.
    JS_EXPORT_PRIVATE CodeBlock* ensureCodeBlock();
    FunctionRef function() const;
    // For the functions that the function makes closures of: made when the first closure is.
    FunctionExecutable* functionDecl(unsigned);
    FunctionExecutable* functionExpr(unsigned);

    // Structures do not keep their IDs to themselves when they die.
    void finalizeUnconditionally(VM&);
    void finalizeSlot(VM&, Slot&); // One of its own, or of one of its SlotsOfSite.

    static constexpr ptrdiff_t offsetOfConstants() { return OBJECT_OFFSETOF(Data, constants); }
    static constexpr ptrdiff_t offsetOfIdentifiers() { return OBJECT_OFFSETOF(Data, identifiers); }
    static constexpr ptrdiff_t offsetOfSites() { return OBJECT_OFFSETOF(Data, sites); }
    static constexpr ptrdiff_t offsetOfSlots() { return OBJECT_OFFSETOF(Data, slots); }
    static constexpr ptrdiff_t offsetOfSlotEpoch() { return OBJECT_OFFSETOF(Data, slotEpoch); }
    static constexpr ptrdiff_t offsetOfNumSlots() { return OBJECT_OFFSETOF(Data, numSlots); }
    static constexpr ptrdiff_t offsetOfHasSiteConstants() { return OBJECT_OFFSETOF(Data, hasSiteConstants); }
    static constexpr ptrdiff_t offsetOfHasBeenFilledSinceLastCollection() { return OBJECT_OFFSETOF(Data, hasBeenFilledSinceLastCollection); }

    CodeBlock* codeBlock; // See ensureCodeBlock().
    Instance* instance;
    ScriptExecutable* executable;
    UnlinkedCodeBlock* unlinkedCodeBlock;
    JITCode* code; // Has a reference.
    FunctionExecutable** functions; // The declarations, then the expressions. Null until one is asked for.
    const void* constants; // const WriteBarrier<Unknown>*
    const void* identifiers; // const Identifier*
    const Site* sites; // One for each slot. The code's, not this CodeBlock's.
    SlotWatchpointMap* watchpoints; // For the slots whose caches rest on more than the code checks. Null until there is one.
    unsigned numSlots;
    bool hasBeenFilledSinceLastCollection;
    bool ownsConstants; // Some are of the realm: this is a copy of the unlinked code's with those filled in.
    uint32_t numberOfOwnConstants;
    bool hasSiteConstants; // After the last of the sites: ImageFunction::siteConstants().
    unsigned indexAmongAll; // Where it is in the Instance's lists.
    unsigned indexAmongFilled; // If hasBeenFilledSinceLastCollection.
    // Another number whenever the cache of a property access has become one for something else, or for nothing. What code has found
    // out about such caches, and nothing but them, it need not find out again while this is the same (GuardKind::BeginSlotChecks).
    uint64_t slotEpoch;
    Slot slots[0];
};

// What a function that starts cold (CompiledFunctionInfo::startsCold) has for a Data until it gets one: the same for all of them,
// and never written to. Each of its slots has nothing and has given up on ever having anything, so whoever looks in one goes on
// to what all functions share (the dispatch table, the megamorphic cache) and then to the operation, which leaves it alone.
//
// A function that gets its Data while it is running goes on with this until it returns. So whose a slot is is told from where it is.
// What else stays the same about a function, and is seldom asked for. A program that is built with a static heap has these instead
// of the functions' unlinked code, which stays in the payload it would be decoded from (FunctionRef::ensureUnlinkedCodeBlock()).
// One word, and then a word or two for each thing that there is, in this order.
struct FunctionFacts {
    enum Fact : uint32_t {
        ExpressionInfo = 1 << 0, // Where the record is that it is decoded from (decodeBorrowedExpressionInfo()).
        Handlers = 1 << 1, // UnlinkedHandlerInfo: where, and how many.
        FunctionDecls = 1 << 2, // WriteBarrier<UnlinkedFunctionExecutable>: where, and how many.
        FunctionExprs = 1 << 3,
        StringSwitchJumpTables = 1 << 4, // UnlinkedStringJumpTable: where the first is.
        // Unless FunctionInfo::constantsAreOfNoRealm. Where there are: how many constants there are, how many of them are
        // SourceCodeRepresentation::LinkTimeConstant, and which those are.
        RealmConstants = 1 << 5,
        // Of the body of an async function: where it goes on from in each of the states it can be waiting in, which is what the
        // last of its UnlinkedSimpleJumpTables says. Where there are: the least state, how many there are, and an offset for each.
        ResumePoints = 1 << 6,
        ConstantIdentifierSets = 1 << 7, // IdentifierSet: where the first is.
        Scalars = 1 << 8, // Where what scalarsToMakeFunctionCodeFrom() gave is.
    };
    static constexpr uint32_t isBuiltinFunction = 1 << 9; // (After the last that there could be: there is no word for it.)
    static constexpr unsigned shiftOfInstructionsSize = 10;
    static unsigned wordsFor(Fact fact) { return fact == Handlers || fact == FunctionDecls || fact == FunctionExprs ? 2 : 1; }

    const uint32_t* find(Fact fact) const
    {
        if (!(flagsAndInstructionsSize & fact))
            return nullptr;
        const uint32_t* word = &flagsAndInstructionsSize + 1;
        for (uint32_t earlier = 1; earlier < fact; earlier <<= 1) {
            if (flagsAndInstructionsSize & earlier)
                word += wordsFor(static_cast<Fact>(earlier));
        }
        return word;
    }
    unsigned instructionsSize() const { return flagsAndInstructionsSize >> shiftOfInstructionsSize; }

    uint32_t flagsAndInstructionsSize;
};

struct SharedData {
    static constexpr unsigned maxSlots = 8192;
    static constexpr size_t size = sizeof(Data) + maxSlots * sizeof(Slot);
    JS_EXPORT_PRIVATE static Data* NODELETE get();
    static bool contains(const Slot* slot) { return std::bit_cast<uintptr_t>(slot) - std::bit_cast<uintptr_t>(get()) < size; }
};

inline const FunctionInfo& FunctionRef::info() const { return instance->infos[index]; }

// What the source says at a place in a function's bytecode, for an error message. It is not all there if it is long.
struct Quote {
    enum Kind : uint8_t {
        Exact, // ErrorInstance::SourceTextWhereErrorOccurred::FoundExactSource
        Approximate, // FoundApproximateSource
        Call, // Exact, up to where the arguments start. They are left out: "...)" goes after it.
    };
    uint32_t bytecodeOffset;
    uint32_t start; // Where it starts in the text. What starts in the same place starts the same.
    Kind kind;
    CString text; // UTF-8
};

// What a compilation produces, other than the code: all of it is plain data, and none of it is an address.
struct CompiledFunctionInfo {
    unsigned codeSize { 0 }; // The way in is where it starts.
    Convention convention;
    Vector<IndexReference> indexReferences;
    Vector<SiteOfSpread> sitesOfSpreads;
    struct InlineFrame {
        uint32_t parent;
        uint32_t callSite;
        uint32_t knownCallee;
        bool isTailCall;
    };
    Vector<InlineFrame> inlineFrames; // Graph::inlineFrames
    // The functions that are written in it as expressions: nothing makes one of those but an instruction of its bytecode. And the functions that its code, as compiled, makes.
    Vector<ImageKey> functionExpressionsWritten;
    Vector<ImageKey> functionsMade;
    bool isOnlyCalledDirectly { false }; // ProgramFacts::isClosed: by a call instruction that goes to it, and in no other way.
    uint32_t numberOfFunction { 0 }; // ProgramFacts::number
    unsigned frameSizeInBytes { 0 };
    unsigned numSlots { 0 };
    bool usesStaticImports { false };
    // Most functions that are run at all are run once or twice, and what a Data is for is the times after that. Such a function does
    // without one until it has shown that it is not one of those (SharedData). That takes code that gets at what stays the same
    // by way of the FunctionInfo, and that does not go round and round in one call.
    bool startsCold { false };
    RegisterAtOffsetList calleeSaveRegisters;
    Vector<std::pair<unsigned, unsigned>> catchEntrypoints; // Bytecode offset of the op_catch, offset in the code.
    Vector<StubCall> stubCalls; // For whoever moves the code.
    Vector<Site> sites; // numSlots of them.
    Vector<ImageKey> knownCallees; // The functions that calls were compiled for.
    // For each slot, or for none: a number that whoever puts the program together replaces with one that means the same thing all
    // over the program. Here it is one more than an index into selectors (at a property access) or shapes (where an object is made).
    // Or, of the slot after the one that has a shape, one more than an index into plans, which stays what it is.
    static constexpr uint32_t siteConstantIsShape = 1u << 31;
    static constexpr uint32_t siteConstantIsPlan = 1u << 30;
    Vector<uint32_t> siteConstants;
    Vector<uint32_t> plans; // See AllocationPlan.
    // The bytecode offsets that a frame can be at when an error is made that says what the source says there. In order.
    Vector<uint32_t> quotableSites;
    Vector<uint32_t> constructSites; // Where in the bytecode something is constructed, in order (collectConstructSites()).
    // Every bytecode offset that a frame of the function can say it is at. In order.
    Vector<uint32_t> callSites;
    Vector<uint32_t> numbersOfIdentifiers; // ReportableSitesOfFunction::numbersOfIdentifiers
    Vector<uint32_t> numbersOfConstants; // Likewise.
    // For each of constructSites, where the expression starts (the `new`): see ReportableSitesOfFunction::Construction.
    Vector<std::pair<uint32_t, uint32_t>> startsOfConstructions;
    Vector<Quote> quotes; // And what it says at each that it says anything at, for a program that goes without its text (collectQuotes()).
    Vector<UniquedStringImpl*> selectors;
    Vector<KnownShape> shapes;
    Vector<std::pair<uint32_t, CString>> notesOfSites; // Options::aotWritesMap()
};

struct ImageCatchEntrypoint {
    uint32_t bytecodeOffset;
    uint32_t codeOffset;
};

// What the frame of a function is like. A program has a few thousand of these between all its functions.
struct ImageFrame {
    // The registers that the function saves (ImageFunction::packRegisters()). They are next to each other in the frame, in the order of
    // Reg::index(), the way Air::Code puts them: the first is this many registers below what the frame pointer points at.
    uint32_t calleeSaveRegisters;
    uint16_t whereCalleeSavesStart;
    uint16_t frameSizeInUnits; // Of stackAlignmentBytes().

    unsigned frameSizeInBytes() const { return frameSizeInUnits * stackAlignmentBytes(); }
    uint64_t bits() const { return static_cast<uint64_t>(calleeSaveRegisters) << 32 | static_cast<uint64_t>(whereCalleeSavesStart) << 16 | frameSizeInUnits; }
};
static_assert(sizeof(ImageFrame) == 8);

// Followed by numSlots Site, then perhaps numSlots uint32_t (CompiledFunctionInfo::siteConstants, as the image numbers them: zero
// for none), then numberOfKnownCallees uint32_t, then numberOfCatchEntrypoints ImageCatchEntrypoint, then CompiledFunctionInfo::plans.
// The way in is where the code starts.
struct ImageFunction {
    // Which function it is: they are numbered in the order their code is in. Where that starts, and so where it ends, is for ImageHeader::startsOfFunctionsOffset to say (Image::codeFor()).
    uint32_t index;
    uint32_t numSlots;
    uint32_t quotes; // From ImageHeader::quotesOffset. Zero: none. See Image::quoteAt(), and after that Image::constructsAt().
    uint32_t callSites; // From ImageHeader::callSitesOffset: see callSiteAt(). Zero: none.
    uint32_t numberOfKnownCallees : 17;
    uint32_t frame : 15; // Which ImageFrame (Image::frameOf()).
    uint16_t numberOfCatchEntrypoints;
    uint8_t numberOfParameters; // Convention::numberOfParameters
    uint8_t takesList : 1; // Signature::List
    uint8_t hasInlineFrames : 1; // Its call sites are PackedSites.
    uint8_t hasSiteConstants : 1; // If not, see FunctionInfo::sitesHaveTheirConstants.
    uint8_t usesStaticImports : 1; // See Graph::usesStaticImports.
    uint8_t startsCold : 1; // See CompiledFunctionInfo::startsCold.

    Convention convention() const { return { takesList ? Signature::List : Signature::Registers, numberOfParameters, true }; }
    // A bit for each by Reg::index(), in half the room: no callee saves any of the rest.
#if CPU(ARM64)
    static constexpr unsigned firstGPRSaid = 16;
    static constexpr unsigned firstFPRSaid = 32;
#else
    static constexpr unsigned firstGPRSaid = 0;
    static constexpr unsigned firstFPRSaid = 16;
#endif
    static uint64_t unpackRegisters(uint32_t packed) { return static_cast<uint64_t>(packed & 0xffff) << firstGPRSaid | static_cast<uint64_t>(packed >> 16) << firstFPRSaid; }
    static uint32_t packRegisters(uint64_t mask)
    {
        uint32_t packed = static_cast<uint32_t>(mask >> firstGPRSaid & 0xffff) | static_cast<uint32_t>(mask >> firstFPRSaid & 0xffff) << 16;
        RELEASE_ASSERT(unpackRegisters(packed) == mask);
        return packed;
    }

    const Site* sites() const { return reinterpret_cast<const Site*>(this + 1); } // (FunctionInfo::function() goes by that.)
    const uint32_t* siteConstants() const { return reinterpret_cast<const uint32_t*>(sites() + numSlots); }
    const uint32_t* knownCallees() const { return siteConstants() + (hasSiteConstants ? numSlots : 0); } // the index of the function of each. Or, if the image has no code for it, noSuchFunction.
    const ImageCatchEntrypoint* catchEntrypoints() const { return reinterpret_cast<const ImageCatchEntrypoint*>(knownCallees() + numberOfKnownCallees); }
    const uint32_t* plans() const { return reinterpret_cast<const uint32_t*>(catchEntrypoints() + numberOfCatchEntrypoints); }
    static constexpr uint32_t noSuchFunction = std::numeric_limits<uint32_t>::max();
};

static_assert(sizeof(ImageFunction) == 24);

inline const ImageFunction* FunctionInfo::function() const
{
    return isOfCodeInImage() ? reinterpret_cast<const ImageFunction*>(sites) - 1 : nullptr;
}

class JITCode final : public JSC::JITCode {
public:
    // The code is in an image, which is mapped for as long as the process lives. What there is to know about it is in the image, and
    // stays there.
    // Way: how whoever makes frames the way the interpreter wants them gets in (generateEnter(), generateEnterFunction()).
    enum class Way : uint8_t { TopLevel, Call, Construct };
    static Way wayInto(UnlinkedCodeBlock*);
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
    static constexpr ptrdiff_t offsetOfEntry() { return OBJECT_OFFSETOF(JITCode, m_entry); } // An EntryWord.
    // One of these is the code of one executable, which is of one realm.
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

// Makes the code the function's. It gets no CodeBlock. False: an exception was thrown.
bool install(VM&, FunctionExecutable*, CodeSpecializationKind, UnlinkedCodeBlock*, JSGlobalObject*, Ref<JITCode>&&);
// An executable that was made when the program was built (FunctionExecutable::aotEntryFor()) is about to be run for the first time:
// gives its code what it has of the realm. False if the code is not for the realm the function is of.
bool linkStaticFunction(VM&, FunctionExecutable*, CodeSpecializationKind, JSScope*);

// Where code from the JIT that wants to call `code` with a call instruction, whose reach is limited, can call. Any thread.
void* nearCallTargetFor(void* code);
void* catchThunk();
// A copy of a stub that is there for good: in an image if there is one, so that nothing has to be generated.
void* addressOfStub(Stub);

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
