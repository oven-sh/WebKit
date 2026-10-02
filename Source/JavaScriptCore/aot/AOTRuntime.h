/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

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

// Ahead-of-time compiled code contains no absolute addresses: not the VM's, not a C++ function's, not a thunk's. Everything it
// needs beyond its arguments is reached through the Instance, which is kept in a pinned register (AOTConvention.h).
//
//     Instance -> runtimeTable[Entry]              C++ operations and thunks: one table per VM
//              -> vm, globalObject
//              -> states[index of the function] -> its Data -> constants[i]
//                                             -> identifiers[i]
//                                             -> slots[i]      the function's inline caches
//
// The same code bytes therefore run wherever they are mapped, in any VM of any process.

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
    v(operationAOTFindEqualAtom) \
    v(operationAOTGetFieldSlow) \
    v(operationAOTReadField) \
    v(operationAOTGetLengthSlow) \
    v(operationAOTValidateTypedObject) \
    v(operationAOTVerifyInferredType) \
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

// Operations shared with the DFG and FTL, used for method calls whose receiver and argument types are known (DirectMethod).
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
    v(ConstructByCalling) \
    v(MegamorphicCache) \
    /* TypedLayoutTable::layoutIDsOfFieldsInSlot() */ \
    v(LayoutIDsOfFieldsInSlot0) \
    v(LayoutIDsOfFieldsInSlot1) \
    v(LayoutIDsOfFieldsInSlot2) \
    v(LayoutIDsOfFieldsInSlot3) \
    v(LayoutIDsOfFieldsInSlot4) \
    v(LayoutIDsOfFieldsInSlot5) \
    v(LayoutIDsOfFieldsInSlot6) \
    v(LayoutIDsOfFieldsInSlot7) \
    v(LayoutIDsOfFieldsInSlot8) \
    v(LayoutIDsOfFieldsInSlot9) \
    v(LayoutIDsOfFieldsInSlot10) \
    v(LayoutIDsOfFieldsInSlot11) \
    v(LayoutIDsOfFieldsInSlot12) \
    v(LayoutIDsOfFieldsInSlot13) \
    v(LayoutIDsOfFieldsInSlot14) \
    v(LayoutIDsOfFieldsInSlot15) \
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

bool takesInstance(Entry);
bool takesGlobalObject(Entry);

// The CallLinkInfo passed to the virtual call stubs is embedded in one of these. A tail call has already popped the caller's frame
// by the time the callee needs the slow path, so the pointers normally found through that frame are kept here.
struct VirtualCallInfo {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(VirtualCallInfo);

    static constexpr ptrdiff_t offsetOfFindTarget() { return OBJECT_OFFSETOF(VirtualCallInfo, findTarget); }
    static constexpr ptrdiff_t offsetOfLookupExceptionHandler() { return OBJECT_OFFSETOF(VirtualCallInfo, lookupExceptionHandler); }

    DataOnlyCallLinkInfo callLinkInfo;
    void* findTarget; // llint_virtual_call()
    void* lookupExceptionHandler; // operationLookupExceptionHandler()
};

// One per VM, created the first time the VM runs ahead-of-time compiled code.
class RuntimeTable {
    WTF_MAKE_TZONE_ALLOCATED(RuntimeTable);
    WTF_MAKE_NONCOPYABLE(RuntimeTable);
public:
    explicit RuntimeTable(VM&);
    ~RuntimeTable();

    void** entries() { return m_entries; }

private:
    void* m_entries[numberOfEntries]; // Must be first. See VM::offsetOfAOTRuntimeTable().
    Vector<std::unique_ptr<VirtualCallInfo>> m_callLinkInfos;
};

RuntimeTable& runtimeTable(VM&);

// An inline cache entry. Compiled code and stubs read it; the slow path operation fills it in. All zeros means empty (no Structure
// has ID zero).
//
// The GC visits every Slot with a nonzero structureID without knowing which kind of cache it belongs to. Cells are held weakly
// (Data::finalizeUnconditionally()), and a cached transition keeps the new Structure alive while the old one is alive
// (CodeBlock::propagateTransitions()). Such a Slot must therefore describe its second word, using the bits of `offset` that a
// PropertyOffset does not need. The GC ignores a Slot with no structureID.
// The upper 32 bits of every Structure address. StructureMemoryManager reserves the structure heap directly after the static
// region. If that address range is unavailable, the program cannot run.
static constexpr uintptr_t structureIDBaseOfImages = bmalloc::StaticRegion::base + bmalloc::StaticRegion::reservation;
static_assert(!(structureIDBaseOfImages & 0xffffffff));

struct Slot {
    static constexpr unsigned offsetBits = 24;
    static constexpr uint32_t offsetMask = (1u << offsetBits) - 1;
    // op_get_by_id, when !isIndirect: the low bits of `offset` are the location (in words from the start of the object) and the
    // remaining bits are the property name ID, or zero.
    static constexpr unsigned directLocationBits = 8;
    static constexpr uint32_t directLocationMask = (1u << directLocationBits) - 1;
    static constexpr unsigned nameIDShift = directLocationBits;
    static_assert(offsetBits - nameIDShift == 16);
    static_assert(JSFinalObject::maxInlineCapacity + JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue) <= directLocationMask);
    static constexpr unsigned attemptsShift = 24; // Number of attempts to set up an expensive cache. See cacheGetById().
    static constexpr uint32_t maxAttempts = 15;
    static constexpr uint32_t attemptsMask = maxAttempts << attemptsShift;
    static constexpr uint32_t isIndirect = 1u << 28; // op_get_by_id, op_put_by_id: the access is more than a load or store at that location in the base object.
    static constexpr uint32_t isGetter = 1u << 29; // op_get_by_id: the location holds a GetterSetter whose getter must be called.
    static constexpr uint32_t hasFieldType = 1u << 29; // op_put_by_id: the bits above newStructureID are a packed field type, not the rest of a pointer.
    static constexpr uint32_t pointerIsNotCell = 1u << 30; // `pointer` refers to something that lives as long as the VM.
    static constexpr uint32_t pointerIsCell = 1u << 31; // `pointer` is a cell. If neither bit is set, the union holds newStructureID (possibly zero).
    static constexpr uint32_t resolvesByDepth = 1u << 31; // op_resolve_scope: the rest of `offset` is the scope depth.

    bool hasPointer() const { return offset & (pointerIsCell | pointerIsNotCell); }
    // op_get_by_id: the site has seen more than one Structure. structureID is zero and `pointer` is a PolymorphicSlots.
    static constexpr uint32_t flagsMask = isIndirect | isGetter | pointerIsNotCell | pointerIsCell;
    static constexpr uint32_t polymorphicFlags = isIndirect | pointerIsNotCell;
    bool isPolymorphic() const { return !structureID && (offset & flagsMask) == polymorphicFlags; }
    // op_get_by_id: the site has seen more than one Structure, all with the name at the same inline offset. structureID is zero, so
    // that the stub's comparison of Structures fails for every object. All sites share that branch: it can be predicted if its
    // outcome depends on the site, and cannot if it depends on the object.
    bool isByNameOnly() const { return !structureID && !(offset & (flagsMask | attemptsMask)) && offset >> nameIDShift; }

    void clear()
    {
        structureID = StructureID();
        offset &= attemptsMask;
        pointer = nullptr;
    }

    StructureID structureID; // The Structure the base must have.
    uint32_t offset; // Property location (see locationOfProperty()), or cache-specific data.
    union {
        void* pointer; // Global variable caches: the variable's address. Prototype hits: the holder.
        UniquedStringImpl* name; // op_get_by_id, when !isIndirect: the property name, once the stub has looked it up. Used to probe the megamorphic cache.
        struct {
            StructureID newStructureID; // For transitions.
            uint32_t fieldType; // op_put_by_id: if nonzero, PutPropertySlot's packed field type. Only values it accepts may be stored.
        };
    };
};
static_assert(sizeof(Slot) == 16);

struct Data;

// The state of a polymorphic property read site (Slot::isPolymorphic()): one Slot for each of the last few Structures seen. Each is
// an ordinary Slot as far as the stubs, the GC and the watchpoints are concerned.
struct PolymorphicSlots {
    static constexpr unsigned numberOfSlots = 4;
    // On a miss the stub probes the megamorphic cache, which is cheaper than calling the slow path to add an entry. New entries are
    // therefore only added occasionally: on each of the first few misses...
    static constexpr uint32_t timesToLearnAtOnce = 12;
    // ...and after that on one miss in every missesBetweenLearning.
    static constexpr uint32_t missesBetweenLearning = 1024;
    static_assert(hasOneBitSet(missesBetweenLearning));

    static constexpr ptrdiff_t offsetOfName() { return OBJECT_OFFSETOF(PolymorphicSlots, name); }
    static constexpr ptrdiff_t offsetOfMisses() { return OBJECT_OFFSETOF(PolymorphicSlots, misses); }
    static constexpr ptrdiff_t offsetOfTimesLeftToLearnAtOnce() { return OBJECT_OFFSETOF(PolymorphicSlots, timesLeftToLearnAtOnce); }
    static constexpr ptrdiff_t offsetOfSlots() { return OBJECT_OFFSETOF(PolymorphicSlots, slots); }
    static constexpr ptrdiff_t offsetOfByName() { return OBJECT_OFFSETOF(PolymorphicSlots, byName); }

    // A plain property in one of an object's first inline slots is found by name instead. A Structure records which name is in each
    // of those slots (Structure::fieldIDInSlot()), so one inline slot number covers every Structure that has the name there,
    // however many there are.
    // Bits 0 to 15: the property name ID. Bits 16 to 47: four inline slot numbers, of which the unused ones repeat the first.
    // Bits 48 to 63: the value in a Structure's table for which the stub calls the slow path, to have the table filled in: zero,
    // or a value that no table holds once the site has met too many Structures whose tables cannot be filled in.
    static constexpr unsigned numberOfInlineSlotsByName = 4;
    static constexpr unsigned shiftOfInlineSlotsByName = 16;
    static constexpr unsigned shiftOfValueToFillInTableFor = 48;
    static constexpr uint64_t initialByName = Structure::firstReservedPropertyNameID;
    static constexpr uint8_t timesToTolerateTableThatCannotBeFilledIn = 16;
    bool fillsInTables() const { return !(byName >> shiftOfValueToFillInTableFor); }
    void noteTableThatCannotBeFilledIn()
    {
        if (!--timesLeftToTolerateTableThatCannotBeFilledIn)
            byName |= static_cast<uint64_t>(Structure::firstReservedPropertyNameID) << shiftOfValueToFillInTableFor;
    }
    // Returns false if there is no room for another inline slot number.
    bool addInlineSlotByName(uint16_t nameID, unsigned inlineSlot)
    {
        ASSERT(inlineSlot < Structure::numberOfSlotsWithFieldIDs && nameID && nameID < Structure::firstReservedPropertyNameID);
        if (!numberOfInlineSlotsByNameInUse) {
            byName = (byName >> shiftOfValueToFillInTableFor << shiftOfValueToFillInTableFor) | static_cast<uint64_t>(inlineSlot * 0x01010101u) << shiftOfInlineSlotsByName | nameID;
            numberOfInlineSlotsByNameInUse = 1;
            return true;
        }
        RELEASE_ASSERT(static_cast<uint16_t>(byName) == nameID);
        for (unsigned i = 0; i < numberOfInlineSlotsByNameInUse; ++i) {
            if ((byName >> (shiftOfInlineSlotsByName + i * 8) & 0xff) == inlineSlot)
                return true;
        }
        if (numberOfInlineSlotsByNameInUse == numberOfInlineSlotsByName)
            return false;
        unsigned shift = shiftOfInlineSlotsByName + numberOfInlineSlotsByNameInUse++ * 8;
        byName = (byName & ~(0xffull << shift)) | static_cast<uint64_t>(inlineSlot) << shift;
        return true;
    }

    UniquedStringImpl* name; // The property name.
    uint32_t misses; // Incremented by the stub.
    uint32_t timesLeftToLearnAtOnce;
    Data* owner;
    uint32_t next; // Index of the entry to evict next.
    uint8_t numberOfInlineSlotsByNameInUse;
    uint8_t timesLeftToTolerateTableThatCannotBeFilledIn;
    uint64_t byName;
    Slot slots[numberOfSlots];
};

struct Data;
struct ImageEnvironment;
struct ImageFunction;
struct Site;

// Per-function data that stubs and compiled code read and that never changes while the function is linked. Indexed by function
// index (Instance::infos).
struct FunctionInfo {
    static constexpr uint16_t hasSiteConstants = 1; // ImageFunction::siteConstants() follow the last Site.
    static constexpr uint16_t startsCold = 2; // See CompiledFunctionInfo::startsCold.
    static constexpr uint16_t sitesHaveTheirConstants = 4; // A Site's identifier field holds its site constant, if it has one.

    // The bits above the flags hold the number of slots, saturated at maxEncodedSlots.
    static constexpr unsigned numberOfFlagBits = 3;
    static constexpr uint32_t maxEncodedSlots = (1u << (16 - numberOfFlagBits)) - 1;
    static constexpr uint16_t slotsAmongFlags(uint32_t numSlots) { return static_cast<uint16_t>(std::min(numSlots, maxEncodedSlots) << numberOfFlagBits); }

    static constexpr ptrdiff_t offsetOfConstants() { return OBJECT_OFFSETOF(FunctionInfo, constants); }
    static constexpr ptrdiff_t offsetOfIdentifiers() { return OBJECT_OFFSETOF(FunctionInfo, identifiers); }
    static constexpr ptrdiff_t offsetOfSites() { return OBJECT_OFFSETOF(FunctionInfo, sites); }
    static constexpr ptrdiff_t offsetOfFlags() { return OBJECT_OFFSETOF(FunctionInfo, flags); }

    // May be null, in which case only the Data knows the executable.
    void setExecutable(ScriptExecutable* executable, CodeSpecializationKind kind, bool hasOnlyRealmIndependentConstants)
    {
        uintptr_t bits = std::bit_cast<uintptr_t>(executable);
        RELEASE_ASSERT(!(bits >> 48) && !(bits & 7));
        bits |= (kind == CodeSpecializationKind::CodeForConstruct ? 1 : 0) | (hasOnlyRealmIndependentConstants ? 2 : 0);
        executableAndMoreLow = static_cast<uint32_t>(bits);
        executableAndMoreHigh = static_cast<uint16_t>(bits >> 32);
    }
    ScriptExecutable* executable() const { return std::bit_cast<ScriptExecutable*>((static_cast<uintptr_t>(executableAndMoreHigh) << 32 | executableAndMoreLow) & ~static_cast<uintptr_t>(7)); }
    CodeSpecializationKind kind() const { return executableAndMoreLow & 1 ? CodeSpecializationKind::CodeForConstruct : CodeSpecializationKind::CodeForCall; }
    bool hasOnlyRealmIndependentConstants() const { return executableAndMoreLow & 2; } // `constants` is complete; the Data has none of its own.
    bool isOfCodeInImage() const { return flags & (hasSiteConstants | sitesHaveTheirConstants); }
    inline const ImageFunction* function() const; // For code in an image: located directly before the sites.

    const void* constants; // const WriteBarrier<Unknown>*. Unless hasOnlyRealmIndependentConstants(), the Data holds the constants instead.
    const void* identifiers; // const Identifier*
    const Site* sites; // One per slot.
    uint32_t executableAndMoreLow; // See setExecutable().
    uint16_t executableAndMoreHigh;
    uint16_t flags;
};
static_assert(sizeof(FunctionInfo) == 32);

// One per realm that runs ahead-of-time compiled code.
struct Instance {
    JS_EXPORT_PRIVATE static Instance& ensure(JSModuleLoader*);
    static Instance& ensure(JSGlobalObject*); // Of the realm's own loader.
    static Instance* of(JSFunction*);
    Structure* structureOfFunctions(Structure* ofRealm, FunctionExecutable*);
    JS_EXPORT_PRIVATE JSFunction* makeFunction(FunctionExecutable*, JSScope*);
    ScriptExecutable* topLevelExecutableOf(SourceProvider*);
    JS_EXPORT_PRIVATE void setTopLevelExecutableOf(SourceProvider*, ScriptExecutable*);
    JSModuleLoader* loader() const;
    static bool convertToTypedLayout(VM&, JSObject*, uint16_t layoutID); // TypedLayoutTable::ConvertFunction
    Structure* emptyStructureForLayout(uint16_t layoutID);
    Structure* emptyStructureForLayout(uint16_t layoutID, JSObject* prototype); // Creates a new Structure. The caller must keep it alive.
    static JSObject* newObjectOf(VM&, Structure*); // Allocates an empty object, with out-of-line storage if the Structure has out-of-line slots.
    JS_EXPORT_PRIVATE static void destroy(Instance*);

    // For the GC. Slots hold their referents weakly.
    template<typename Visitor> void visit(Visitor&, bool onlyNew);
    void finalizeUnconditionally(bool onlyNew);

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
    // Returns the function's own Data, allocating it if the function has been using SharedData. The function must be linked.
    JS_EXPORT_PRIVATE Data* ensureData(uint32_t index);
    // For a function that has no Data of its own.
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
    Data* dataIfExists(uint32_t index) const
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
    // A Slot now caches a transition. The new Structure must stay alive as long as the old one.
    void noteTransitionCached(Slot*);
    PolymorphicSlots* makeSlotsOfSite(Data*, UniquedStringImpl* name); // Owned by the Data.
    // Datas are allocated in the address range after the Instance, so that a 32-bit offset identifies one. The memory is zeroed.
    void* allocateForData(size_t);
    void freeOfData(void*, size_t);
    // How many cache misses a function with this many slots may take before it is given its own Data.
    uint32_t missesToPutUpWithFor(uint32_t numSlots) const { return std::min<uint32_t>((numSlots * missesForEightSlots >> 3) + missesToSpare, std::numeric_limits<uint16_t>::max()); }
    static constexpr ptrdiff_t offsetOfMissesForEightSlots() { return OBJECT_OFFSETOF(Instance, missesForEightSlots); }
    static constexpr ptrdiff_t offsetOfMissesToSpare() { return OBJECT_OFFSETOF(Instance, missesToSpare); }

    // Upper bound on the number of functions. This reserves address space, not memory.
    static constexpr size_t maxFunctions = 4 << 20;

    // If the image requests it, the program's module environments are placed below the Instance, each at the same distance in every
    // realm, so compiled code can reach a module variable directly from the Instance. Returns null if this realm's environments are
    // ordinary GC allocations, in which case code that relies on the fixed placement cannot run in this realm.
    JS_EXPORT_PRIVATE JSCell** slotOfEnvironment(ImageEnvironment) const;

    // Structure::createWithProperties() applied to the empty object literal Structure with that inline capacity. Cached by property
    // names, and never freed.
    Structure* structureOfLiteral(Structure* empty, std::span<UniquedStringImpl* const>);

    // The Structure for a shape numbered in the image (ImageShape), with Structure::knownShape() set. Created on first use and
    // never freed. `names` are the shape's property names.
    Structure* structureOfKnownShape(uint32_t shape, std::span<UniquedStringImpl* const> names);

    // { ...source }, where the source's Structure has a known shape. The copy must not have that Structure, because objects of a
    // known shape only come from the literals that have the shape. It gets a Structure without a shape that has the same properties
    // at the same offsets, so that the slots can be copied. Returns null if there is no such Structure.
    JSObject* tryCopySlotsForSpread(JSObject* source);
    // The slot of each property, if the slots are not consecutive (KnownShape::slots).
    std::span<const uint16_t> slotsOfKnownShape(uint32_t shape) const;
    static constexpr ptrdiff_t offsetOfStructureIDBase() { return OBJECT_OFFSETOF(Instance, structureIDBase); }
    // A property missing from an object with a known shape is absent altogether if the object inherits only from Object.prototype
    // and Object.prototype lacks it too. Refreshes the cached view of Object.prototype if its Structure has changed.
    void lookAtObjectPrototype();
    static constexpr ptrdiff_t offsetOfIntrinsics() { return OBJECT_OFFSETOF(Instance, intrinsics); }
    static constexpr ptrdiff_t offsetOfLinkTimeConstants() { return OBJECT_OFFSETOF(Instance, linkTimeConstants); }
    static constexpr ptrdiff_t offsetOfObjectPrototype() { return OBJECT_OFFSETOF(Instance, objectPrototype); }
    static constexpr ptrdiff_t offsetOfStructureIDOfObjectPrototype() { return OBJECT_OFFSETOF(Instance, structureIDOfObjectPrototype); }
    static constexpr ptrdiff_t offsetOfSelectorsOnObjectPrototype() { return OBJECT_OFFSETOF(Instance, selectorsOnObjectPrototype); }
    static constexpr ptrdiff_t offsetOfDispatch() { return OBJECT_OFFSETOF(Instance, dispatch); }
    static constexpr ptrdiff_t offsetOfRowsOfSelectors() { return OBJECT_OFFSETOF(Instance, rowsOfSelectors); }


    void** runtimeTable;
    JSGlobalObject* globalObject;
    VM* vm; // At the same offset as in JSWebAssemblyInstance, so code that finds the VM from a frame need not distinguish the two.
    struct Collections;
    Collections* collections; // Bookkeeping for the Datas.
    FunctionInfo* infos; // By function index.
    Data* sharedData; // SharedData::get()
    const uint32_t* functionMetadataOffsets; // By function index: the offset of its FunctionMetadata in Arena::Data, or zero. Null if no function has metadata.
    // For mapping a code address to a function (loadIndexOfFunctionAt(), Image::classifyAddress()): the start of the image's code;
    // for each granule of code, the last function that starts at or before it; and the start of every function but the first.
    const uint8_t* code;
    const uint32_t* granulesOfCode;
    const uint32_t* startsOfFunctionsAfterFirst;
    // Fields that compiled code reads come first, within reach of a load with an immediate offset. Large fields that only the
    // runtime uses come last.
    // The `length` getter of %TypedArray%.prototype, set once a Slot has cached it (tryCacheGetById()). The stubs inline it. Kept
    // alive by the prototype.
    JSCell* getterOfLengthOfTypedArrays { nullptr };
    static constexpr ptrdiff_t offsetOfGetterOfLengthOfTypedArrays() { return OBJECT_OFFSETOF(Instance, getterOfLengthOfTypedArrays); }
    const void* constantsOfProgram; // EncodedJSValue[]. See NumbersOfConstants. Code that uses it only runs in a realm that has it.
    uint32_t missesForEightSlots; // Options::aotCacheMissesPerEightSlotsBeforeOwnData()
    uint32_t missesToSpare;
    uintptr_t structureIDBase; // Added to a StructureID to get the Structure's address.
    const uint32_t* dispatch; // From the image. See ImageDispatchEntry.
    const uint32_t* rowsOfSelectors;
    JSObject* objectPrototype; // Kept alive by the realm.
    // One bit per field with a field ID (slot << 16 | id), set when a read of the field was satisfied by something other than a
    // plain data property or its absence. See Lowering::lowerGetById().
    uint8_t* fieldsWithObservableReads;
    static constexpr size_t sizeOfFieldsWithObservableReads = (static_cast<size_t>(Structure::numberOfSlotsWithFieldIDs) << 16) / 8;
    static constexpr ptrdiff_t offsetOfFieldsWithObservableReads() { return OBJECT_OFFSETOF(Instance, fieldsWithObservableReads); }
    void noteObservableRead(unsigned slot, uint16_t id) { fieldsWithObservableReads[(slot << 16 | id) >> 3] |= 1 << (id & 7); }
    // The realm's Function.prototype.call and its bound function Structure, for Stub::Call.
    JSCell* functionPrototypeCall { nullptr };
    uint32_t structureIDOfBoundFunctions { 0 };
    static constexpr ptrdiff_t offsetOfFunctionPrototypeCall() { return OBJECT_OFFSETOF(Instance, functionPrototypeCall); }
    static constexpr ptrdiff_t offsetOfStructureIDOfBoundFunctions() { return OBJECT_OFFSETOF(Instance, structureIDOfBoundFunctions); }
    uint8_t* selectorsOnObjectPrototype; // One bit per selector, valid while Object.prototype has the Structure below.
    uint32_t structureIDOfObjectPrototype; // Zero if not yet computed, or if the Structure cannot be relied on.
    // Copy of JSGlobalObject::immutableIntrinsics(), so that compiled code reaches one with a single load.
    EncodedJSValue intrinsics[ImmutableIntrinsics::maximumCount];
    // The link-time constants (JSGlobalObject::linkTimeConstant()) that compiled code uses. Each is materialized on first use; zero
    // until then. Kept alive by the realm.
    EncodedJSValue linkTimeConstants[numberOfLinkTimeConstants];
    // The Structure the realm gives new objects of each built-in class (Receiver). An object that still has it has no own
    // properties added and inherits from the realm's original prototype, which cannot change under
    // VM::useImmutableIntrinsics, so its methods are known. Zero if there is none.
    static constexpr unsigned numberOfReceivers = 16;
    uint32_t structureIDsOfReceivers[numberOfReceivers] { };
    static constexpr ptrdiff_t offsetOfStructureIDsOfReceivers() { return OBJECT_OFFSETOF(Instance, structureIDsOfReceivers); }
    // JSGlobalObject::originalArrayStructureForIndexingType(), indexed by the indexing shape and copy-on-write bits.
    static constexpr unsigned shiftOfKindOfArray = 1;
    static constexpr unsigned numberOfKindsOfArray = 16;
    static_assert(((IndexingShapeMask | CopyOnWrite) >> shiftOfKindOfArray) == numberOfKindsOfArray - 1);
    uint32_t structureIDsOfOriginalArrays[numberOfKindsOfArray] { };
    static constexpr ptrdiff_t offsetOfStructureIDsOfOriginalArrays() { return OBJECT_OFFSETOF(Instance, structureIDsOfOriginalArrays); }
    // Structures for objects that compiled code allocates inline. Zero means the runtime must allocate
    // (JSGlobalObject::haveABadTime()).
    uint32_t structureIDOfNewArrayWithInt32 { 0 };
    uint32_t structureIDOfNewArrayWithContiguous { 0 };
    uint32_t structureIDsOfNewCopyOnWriteArrays[3] { }; // Int32, Double, Contiguous.
    uint32_t structureIDOfActivation { 0 };
    static constexpr ptrdiff_t offsetOfStructureIDOfNewArrayWithInt32() { return OBJECT_OFFSETOF(Instance, structureIDOfNewArrayWithInt32); }
    static constexpr ptrdiff_t offsetOfStructureIDOfNewArrayWithContiguous() { return OBJECT_OFFSETOF(Instance, structureIDOfNewArrayWithContiguous); }
    static constexpr ptrdiff_t offsetOfStructureIDsOfNewCopyOnWriteArrays() { return OBJECT_OFFSETOF(Instance, structureIDsOfNewCopyOnWriteArrays); }
    static constexpr ptrdiff_t offsetOfStructureIDOfActivation() { return OBJECT_OFFSETOF(Instance, structureIDOfActivation); }
    // What is so until the program adds a property to a prototype of arrays. Built-in objects being immutable does not rule that
    // out: what they have cannot be changed, but they can be given more. Nonzero while it is so (AssumptionWatchpoint).
    uint32_t arraysInheritNoIsConcatSpreadable { 0 }; // JSGlobalObject::arrayIsConcatSpreadableWatchpointSet()
    uint32_t arraysInheritNoElements { 0 }; // JSGlobalObject::arrayPrototypeChainIsSaneWatchpointSet()
    static constexpr ptrdiff_t offsetOfArraysInheritNoIsConcatSpreadable() { return OBJECT_OFFSETOF(Instance, arraysInheritNoIsConcatSpreadable); }
    static constexpr ptrdiff_t offsetOfArraysInheritNoElements() { return OBJECT_OFFSETOF(Instance, arraysInheritNoElements); }
    // Allocators and shared cells owned by the VM. Their addresses are stable for the VM's lifetime.
    void* auxiliarySpace { nullptr }; // CompleteSubspace* for butterflies.
    void* spaceOfActivations { nullptr }; // CompleteSubspace*
    void* allocatorOfArrays { nullptr }; // LocalAllocator*
    void* allocatorOfRopeStrings { nullptr };
    void* singleCharacterStrings { nullptr }; // JSString*[]
    JSCell* emptyString { nullptr };
    JSCell* sentinelOfArrayIteration { nullptr }; // VM::fastArrayUnboxedSentinel()
    static constexpr ptrdiff_t offsetOfSentinelOfArrayIteration() { return OBJECT_OFFSETOF(Instance, sentinelOfArrayIteration); }
    JSCell* sentinelString { nullptr }; // SmallStrings::sentinelString(), which op_enumerator_next returns at the end.
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
    // Cache of transitions that add a typed field: (Structure, slot) -> new Structure. The caller has already checked that the
    // field type accepts the value. Cleared at every GC, so it keeps nothing alive.
    struct FieldAddition {
        uint32_t structureID;
        uint32_t slot;
        uint32_t structureIDAfterAddition;
        uint32_t unused;
    };
    static constexpr unsigned numberOfFieldAdditions = 1024;
    static constexpr unsigned indexOfFieldAddition(uint32_t structureID, unsigned slot) { return ((structureID >> 4) ^ (slot * 0x9e5u)) & (numberOfFieldAdditions - 1); }
    FieldAddition fieldAdditions[numberOfFieldAdditions] { };
    static constexpr ptrdiff_t offsetOfFieldAdditions() { return OBJECT_OFFSETOF(Instance, fieldAdditions); }
    void noteFieldAddition(Structure* before, unsigned slot, Structure* afterwards);
    // Cache of custom getters (PropertySlot::isCacheableCustom()), keyed by Structure and property name. The getter is still called
    // every time; only the lookup is skipped. Entries are added under the same conditions as in the other tiers (tryCacheGetBy())
    // and are valid for one megamorphic cache epoch.
    struct CustomGetter {
        uint32_t structureID;
        uint16_t epoch; // MegamorphicCache::epoch()
        bool passesHolder; // Pass the holder instead of the base: PropertyAttribute::CustomAccessor is not set.
        UniquedStringImpl* uid;
        void* getter; // GetValueFunc
        JSObject* holder;
    };
    static constexpr unsigned numberOfCustomGetters = 128;
    CustomGetter customGetters[numberOfCustomGetters] { };
    CustomGetter& customGetterFor(uint32_t structureID, UniquedStringImpl* uid) { return customGetters[((structureID >> 4) ^ static_cast<uint32_t>(std::bit_cast<uintptr_t>(uid) >> 4)) % numberOfCustomGetters]; }
    // Cache of code address lookups (FunctionRef::at(), locationForReturnAddress()). The results never change.
    struct CachedAddressInfo {
        static constexpr uint32_t siteNotLookedFor = std::numeric_limits<uint32_t>::max();
        static constexpr uint32_t hasNoSite = siteNotLookedFor - 1;
        const void* address;
        uint32_t function;
        uint32_t site;
    };
    static constexpr unsigned numberOfCachedAddressInfos = 512;
    CachedAddressInfo cachedAddressInfos[numberOfCachedAddressInfos] { };
    CachedAddressInfo& cachedAddressInfo(const void* address) { return cachedAddressInfos[(std::bit_cast<uintptr_t>(address) >> 2) % numberOfCachedAddressInfos]; }
    uint32_t uncountedOperations { 0 }; // See countOperationFor().
    // See CallSiteOverride.
    const void* overriddenReturnAddress { nullptr };
    uint32_t overridingSite { 0 };
    // By function index. Kept small because reading an entry faults in its page.
    //   - Below leastStateWithData: the function uses SharedData. The low 16 bits count cache misses. isLinkedWithoutData is set
    //     once the function has been linked in this realm (a function that is only called directly need not be).
    //   - Otherwise: the function is linked, and its Data is at this value times 16 bytes from the Instance.
    uint32_t states[0];
};

// Whether an unlinked code block's constants can be shared by all realms. A SymbolTable qualifies only if it was created at build
// time (SymbolTable::isSharedAcrossRealms()). The compiler runs before that is decided, so the caller says which to assume.
enum class SymbolTablesAreShared : bool { No, Yes };
JS_EXPORT_PRIVATE bool hasOnlyRealmIndependentConstants(UnlinkedCodeBlock*, SymbolTablesAreShared = SymbolTablesAreShared::No);

JS_EXPORT_PRIVATE const RegisterAtOffsetList* calleeSaveRegistersOf(const ImageFunction&);

struct Data {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(Data);

    // create() registers the Data with the Instance; destroy() unregisters it. Program and module code comes with a CodeBlock,
    // because the interpreter needs one to enter it. Functions do not. Returns null if an exception was thrown.
    static Data* create(Instance&, ScriptExecutable*, UnlinkedCodeBlock*, JITCode&, CodeBlock* = nullptr);
    static void destroy(Data*);
    void noteFilled();
    template<typename Visitor> void visit(Visitor&);

    // The CodeBlock that the rest of the engine sees for this function's frames, created on demand. Compiled code itself never
    // needs it. Main thread only, and not during GC.
    JS_EXPORT_PRIVATE CodeBlock* ensureCodeBlock();
    FunctionRef function() const;
    // Executables for the function's inner functions, created when the first closure is.
    FunctionExecutable* functionDecl(unsigned);
    FunctionExecutable* functionExpr(unsigned);

    // StructureIDs are reused after a Structure dies, so dead ones must be cleared.
    void finalizeUnconditionally(VM&);
    void finalizeSlot(VM&, Slot&); // One of this Data's slots, or a slot of one of its PolymorphicSlots.

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
    JITCode* code; // Owns a reference.
    FunctionExecutable** functions; // Declarations, then expressions. Null until first used.
    const void* constants; // const WriteBarrier<Unknown>*
    const void* identifiers; // const Identifier*
    const Site* sites; // One per slot. Owned by the code.
    SlotWatchpointMap* watchpoints; // For slots whose caches depend on watchpoints. Null until needed.
    unsigned numSlots;
    bool hasBeenFilledSinceLastCollection;
    bool ownsConstants; // Some constants are realm-specific, so `constants` is a private copy with those filled in.
    uint32_t numberOfOwnConstants;
    bool hasSiteConstants; // ImageFunction::siteConstants() follow the last Site.
    unsigned indexAmongAll; // Index in the Instance's list of all Datas.
    unsigned indexAmongFilled; // Valid if hasBeenFilledSinceLastCollection.
    // Incremented whenever a property access cache is cleared or retargeted. Conclusions that compiled code drew from such caches
    // alone stay valid while this is unchanged (GuardKind::BeginSlotChecks).
    uint64_t slotEpoch;
    Slot slots[0];
};

// Rarely needed, immutable information about a function. A program built with a static heap has this instead of the function's
// UnlinkedCodeBlock, which stays undecoded in the payload (FunctionRef::ensureUnlinkedCodeBlock()). The layout is one header word
// followed by one or two words for each item present, in enum order.
struct FunctionMetadata {
    enum Section : uint32_t {
        ExpressionInfo = 1 << 0, // Offset of the record to decode (decodeBorrowedExpressionInfo()).
        Handlers = 1 << 1, // UnlinkedHandlerInfo: offset and count.
        FunctionDecls = 1 << 2, // WriteBarrier<UnlinkedFunctionExecutable>: offset and count.
        FunctionExprs = 1 << 3,
        StringSwitchJumpTables = 1 << 4, // UnlinkedStringJumpTable: offset of the first.
        // Present unless FunctionInfo::hasOnlyRealmIndependentConstants(). Offset of: the number of constants, the number that are
        // SourceCodeRepresentation::LinkTimeConstant, and their indices.
        RealmConstants = 1 << 5,
        // For an async function body: the resume target for each suspended state, taken from its last UnlinkedSimpleJumpTable.
        // Offset of: the lowest state, the number of states, and one bytecode offset per state.
        ResumePoints = 1 << 6,
        ConstantIdentifierSets = 1 << 7, // IdentifierSet: offset of the first.
        Scalars = 1 << 8, // Offset of the result of scalarsToMakeFunctionCodeFrom().
    };
    static constexpr uint32_t isBuiltinFunction = 1 << 9; // A flag only. It has no data word.
    static constexpr unsigned shiftOfInstructionsSize = 10;
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
    unsigned instructionsSize() const { return flagsAndInstructionsSize >> shiftOfInstructionsSize; }

    uint32_t flagsAndInstructionsSize;
};

// The Data used by every function that starts cold (CompiledFunctionInfo::startsCold) until it gets its own. It is shared and never
// written. Each of its slots is empty and marked as having given up, so a lookup falls through to the shared caches (dispatch
// table, megamorphic cache) and then to the slow path operation, which leaves the slot alone.
//
// A function that receives its own Data while running keeps using this one until it returns, so code identifies a shared slot by
// its address (contains()).
struct SharedData {
    static constexpr unsigned maxSlots = 8192;
    static constexpr size_t size = sizeof(Data) + maxSlots * sizeof(Slot);
    JS_EXPORT_PRIVATE static Data* NODELETE get();
    static bool contains(const Slot* slot) { return std::bit_cast<uintptr_t>(slot) - std::bit_cast<uintptr_t>(get()) < size; }
};

inline const FunctionInfo& FunctionRef::info() const { return instance->infos[index]; }

// The source text at a bytecode offset, for use in an error message. Long text is truncated.
struct Quote {
    enum Kind : uint8_t {
        Exact, // ErrorInstance::SourceTextWhereErrorOccurred::FoundExactSource
        Approximate, // FoundApproximateSource
        Call, // Exact, up to the start of the arguments, which are omitted. "...)" is appended.
    };
    uint32_t bytecodeOffset;
    uint32_t start; // Offset in the source. Quotes with the same start share a prefix.
    Kind kind;
    CString text; // UTF-8
};

// Everything a compilation produces besides the code. Plain data with no addresses.
struct CompiledFunctionInfo {
    unsigned codeSize { 0 }; // The entry point is at offset zero.
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
    // The function expressions that appear in its bytecode (only that bytecode can instantiate them), and the functions that the
    // compiled code actually instantiates.
    Vector<ImageKey> functionExpressionsInCode;
    Vector<ImageKey> functionsCreated;
    bool isOnlyCalledDirectly { false }; // FunctionSummary::isNonEscaping: only reached by direct calls.
    uint32_t numberOfFunction { 0 }; // FunctionSummary::number
    unsigned frameSizeInBytes { 0 };
    unsigned numSlots { 0 };
    bool usesStaticImports { false };
    // Most functions that run at all run only once or twice, and a Data only pays off after that. A cold-start function uses
    // SharedData until it has taken enough cache misses. This requires that the code reach its immutable data through FunctionInfo
    // and that it contain no loops.
    bool startsCold { false };
    RegisterAtOffsetList calleeSaveRegisters;
    Vector<std::pair<unsigned, unsigned>> catchEntrypoints; // (bytecode offset of the op_catch, code offset)
    Vector<StubCall> stubCalls; // For relocating the code.
    Vector<Site> sites; // numSlots entries.
    Vector<ImageKey> knownCallees; // The functions that direct calls were compiled against.
    // Optional, one per slot. A function-local number that the image builder replaces with a program-wide one: an index plus one
    // into `selectors` (property accesses) or `shapes` (allocations). The slot after one with a shape may hold an index plus one
    // into `plans`, which is left as it is.
    static constexpr uint32_t siteConstantIsShape = 1u << 31;
    static constexpr uint32_t siteConstantIsPlan = 1u << 30;
    Vector<uint32_t> siteConstants;
    Vector<uint32_t> plans; // See AllocationPlan.
    // Bytecode offsets at which an error that quotes the source can be created. Sorted.
    Vector<uint32_t> quotableSites;
    Vector<uint32_t> constructSites; // Bytecode offsets of constructions, sorted (collectConstructSites()).
    // Every bytecode offset that a frame of this function can report. Sorted.
    Vector<uint32_t> callSites;
    Vector<uint32_t> numbersOfIdentifiers; // ReportableSitesOfFunction::numbersOfIdentifiers
    Vector<uint32_t> numbersOfConstants; // ReportableSitesOfFunction::numbersOfConstants
    // For each entry in constructSites, where the `new` expression starts. See ReportableSitesOfFunction::Construction.
    Vector<std::pair<uint32_t, uint32_t>> startsOfConstructions;
    Vector<Quote> quotes; // Source text for quotableSites, for programs built without source text (collectQuotes()).
    Vector<UniquedStringImpl*> selectors;
    Vector<KnownShape> shapes;
};

struct ImageCatchEntrypoint {
    uint32_t bytecodeOffset;
    uint32_t codeOffset;
};

// A frame layout. Deduplicated: a program has a few thousand across all its functions.
struct ImageFrame {
    // The callee-saved registers the function saves (ImageFunction::packRegisters()). They are contiguous in the frame, in
    // Reg::index() order, as laid out by Air::Code. whereCalleeSavesStart is the distance of the first below the frame pointer, in
    // registers.
    uint32_t calleeSaveRegisters;
    uint16_t whereCalleeSavesStart;
    uint16_t frameSizeInUnits; // In units of stackAlignmentBytes().

    unsigned frameSizeInBytes() const { return frameSizeInUnits * stackAlignmentBytes(); }
    uint64_t bits() const { return static_cast<uint64_t>(calleeSaveRegisters) << 32 | static_cast<uint64_t>(whereCalleeSavesStart) << 16 | frameSizeInUnits; }
};
static_assert(sizeof(ImageFrame) == 8);

// Followed by: numSlots Sites; optionally numSlots uint32_t (CompiledFunctionInfo::siteConstants with program-wide numbering, zero
// for none); numberOfKnownCallees uint32_t; numberOfCatchEntrypoints ImageCatchEntrypoints; CompiledFunctionInfo::plans. The entry
// point is the start of the code.
struct ImageFunction {
    // Functions are numbered in code order. The start and end of the code come from ImageHeader::startsOfFunctionsOffset
    // (Image::codeFor()).
    uint32_t index;
    uint32_t numSlots;
    uint32_t quotes; // Offset from ImageHeader::quotesOffset, or zero. See Image::quoteAt() and Image::constructsAt().
    uint32_t callSites; // Offset from ImageHeader::callSitesOffset, or zero. See callSiteAt().
    uint32_t numberOfKnownCallees : 17;
    uint32_t frame : 15; // Index of its ImageFrame (Image::frameOf()).
    uint16_t numberOfCatchEntrypoints;
    uint8_t numberOfParameters; // Convention::numberOfParameters
    uint8_t takesList : 1; // Signature::List
    uint8_t hasInlineFrames : 1; // Its call sites are PackedSites.
    uint8_t hasSiteConstants : 1; // If not set, see FunctionInfo::sitesHaveTheirConstants.
    uint8_t usesStaticImports : 1; // See Graph::usesStaticImports.
    uint8_t startsCold : 1; // See CompiledFunctionInfo::startsCold.

    Convention convention() const { return { takesList ? Signature::List : Signature::Registers, numberOfParameters, true }; }
    // One bit per register by Reg::index(), restricted to the ranges that contain callee-saved registers.
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

    const Site* sites() const { return reinterpret_cast<const Site*>(this + 1); } // FunctionInfo::function() relies on this layout.
    const uint32_t* siteConstants() const { return reinterpret_cast<const uint32_t*>(sites() + numSlots); }
    const uint32_t* knownCallees() const { return siteConstants() + (hasSiteConstants ? numSlots : 0); } // The function index of each, or noSuchFunction if the image has no code for it.
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
    // The code lives in an image, which stays mapped for the life of the process, as does its metadata.
    // Way: which entry adapter is used by callers that build interpreter-style frames (generateEnter(), generateEnterFunction()).
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
    static constexpr ptrdiff_t offsetOfEntry() { return OBJECT_OFFSETOF(JITCode, m_entry); } // An EntryWord.
    // A JITCode belongs to one executable, and therefore to one realm.
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

// Installs the code on the function, without creating a CodeBlock. Returns false if an exception was thrown.
bool install(VM&, FunctionExecutable*, CodeSpecializationKind, UnlinkedCodeBlock*, JSGlobalObject*, Ref<JITCode>&&);
// Called when an executable from the static heap (FunctionExecutable::aotEntryFor()) is about to run for the first time. Links its
// code to the realm. Returns false if the code cannot run in the function's realm.
bool linkStaticFunction(Instance*, FunctionExecutable*, CodeSpecializationKind, JSScope*);

// An address within near-call range that JIT code can use to call `code`. Any thread.
void* catchThunk();
// A permanent copy of the stub, from an image if one is loaded, so that nothing needs to be generated.
void* addressOfStub(Stub);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
