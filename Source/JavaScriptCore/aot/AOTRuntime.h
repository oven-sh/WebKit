/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(FTL_JIT)

#include "BytecodeIndex.h"
#include "LineColumn.h"
#include "AOTFunction.h"
#include "AOTOperationsObjects.h"
#include "AOTSlotWatchpoint.h"
#include "AOTStubs.h"
#include "CallLinkInfo.h"
#include "ExecutableAllocator.h"
#include "ImmutableIntrinsics.h"
#include "JITCode.h"
#include "Opcode.h"
#include "RegisterAtOffsetList.h"
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

// Code from the static compiler has no address in it: not of the VM, not of a C++ function, not of a thunk. Everything it needs
// that is not in its frame it finds from the Instance, which is where a frame of the interpreter's has its CodeBlock, as a frame
// of WebAssembly's has its instance there.
//
//     frame -> Instance -> runtimeTable[Entry]              C++ operations and thunks: one table per VM
//                       -> vm, globalObject
//                       -> data[index of the function] -> constants[i]
//                                                      -> identifiers[i]
//                                                      -> slots[i]      the function's inline caches
//
// And where such a frame has its callee is what says which function it is a frame of: the CodeHeader in front of the function's
// code, which the function finds from where it is itself. So the same bytes run wherever they are mapped, in any realm of any VM
// of any process.

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
    v(operationAOTPutByVal) \
    v(operationAOTResolveScope) \
    v(operationAOTGetFromScope) \
    v(operationAOTReadLazyClosureVar) \
    v(operationAOTFillImportSlot) \
    v(operationAOTPutToScope) \
    v(operationAOTThrow) \
    v(operationAOTCheckType) \
    v(operationAOTHandleTraps) \
    v(operationAOTWriteBarrier) \
    v(operationAOTCatch) \
    v(operationAOTSwitchString) \
    v(operationAOTSwitchChar) \
    v(operationAOTFMod) \
    v(operationAOTPow) \
    v(operationAOTDoubleToInt32) \
    FOR_EACH_AOT_OBJECT_OPERATION(v) \

#define FOR_EACH_AOT_THUNK(v) \
    v(HandleException) \
    v(ThrowStackOverflowAtPrologue) \
    v(ArityFixup) \
    v(VirtualCall) \
    v(VirtualConstruct) \
    v(VirtualTailCall) \

#define FOR_EACH_AOT_POINTER(v) \
    v(CallLinkInfoForCall) \
    v(CallLinkInfoForConstruct) \
    v(CallLinkInfoForTailCall) \
    v(StructureIDBase) \
    v(LookupExceptionHandler) \
    v(LookupExceptionHandlerFromCallerFrame) \
    v(ThrowStackOverflowError) \
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
    v(RawCreateLexicalEnvironment) \
    v(RawCompareStrictEq) \
    v(RawCompareEq) \

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
struct Slot {
    static constexpr unsigned offsetBits = 24;
    static constexpr uint32_t offsetMask = (1u << offsetBits) - 1;
    static constexpr unsigned attemptsShift = 24; // How often a cache that is expensive to set up has been (see cacheGetById()).
    static constexpr uint32_t maxAttempts = 15;
    static constexpr uint32_t attemptsMask = maxAttempts << attemptsShift;
    static constexpr uint32_t isIntricate = 1u << 28; // op_get_by_id, op_put_by_id: there is more to it than a load or a store at that place in the base itself.
    static constexpr uint32_t isGetter = 1u << 29; // op_get_by_id: what is at that place is a GetterSetter, whose getter has the answer.
    static constexpr uint32_t pointerIsNotCell = 1u << 30; // pointer: something that is there for as long as the VM is.
    static constexpr uint32_t pointerIsCell = 1u << 31; // pointer: a cell. Neither: newStructureID, which may be none.
    static constexpr uint32_t resolvesByDepth = 1u << 31; // op_resolve_scope: the rest of offset is how many scopes out it is.

    bool hasPointer() const { return offset & (pointerIsCell | pointerIsNotCell); }

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
        struct {
            StructureID newStructureID; // Transitions.
            uint32_t unused;
        };
    };
};
static_assert(sizeof(Slot) == 16);

// What comes right before the code of a function. To whoever finds it in a frame it is a NativeCallee.
struct alignas(16) CodeHeader {
    uint64_t refCount { 3 }; // ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr: one strong reference, nothing else. Stays that way.
    NativeCallee::Category category { NativeCallee::Category::AOT };
    ImplementationVisibility visibility { ImplementationVisibility::Public };
    // Where in its frames the function keeps the object it was called as: which Register, from the frame pointer. 0: it does not
    // (Graph::needsFunctionObject()), and there may have been none.
    int16_t calleeSlot { 0 };
    uint32_t index { 0 }; // Which function: see Instance::data.

    static const CodeHeader* fromCallee(CalleeBits bits) { return std::bit_cast<const CodeHeader*>(bits.asNativeCallee()); }
};
static_assert(sizeof(CodeHeader) == sizeof(NativeCallee) && alignof(CodeHeader) == alignof(NativeCallee));

struct Data;
struct ImageEnvironment;
struct ImageFunction;
struct Site;

// What the stubs, and the code itself, want to know about a function that stays as it is for as long as the function is there. By
// CodeHeader::index: Instance::infos.
struct FunctionInfo {
    static constexpr uint16_t hasSiteConstants = 1; // After the last of the sites: ImageFunction::siteConstants().
    static constexpr uint16_t sitesHaveTheirConstants = 16; // Or where a site has its identifier, which is the constant if it has one.
    static constexpr uint16_t constructs = 4; // kind(), where there is no executable to say it with.
    static constexpr uint16_t constantsAreOfNoRealm = 8; // `constants` are all there are, if any.
    static constexpr uint16_t startsCold = 2; // See CompiledFunctionInfo::startsCold.

    static constexpr ptrdiff_t offsetOfConstants() { return OBJECT_OFFSETOF(FunctionInfo, constants); }
    static constexpr ptrdiff_t offsetOfIdentifiers() { return OBJECT_OFFSETOF(FunctionInfo, identifiers); }
    static constexpr ptrdiff_t offsetOfSites() { return OBJECT_OFFSETOF(FunctionInfo, sites); }
    static constexpr ptrdiff_t offsetOfNumSlots() { return OBJECT_OFFSETOF(FunctionInfo, numSlots); }
    static constexpr ptrdiff_t offsetOfFlags() { return OBJECT_OFFSETOF(FunctionInfo, flags); }

    ScriptExecutable* executable() const { return std::bit_cast<ScriptExecutable*>(executableAndKind & ~static_cast<uintptr_t>(1)); }
    CodeSpecializationKind kind() const { return executableAndKind & 1 || flags & constructs ? CodeSpecializationKind::CodeForConstruct : CodeSpecializationKind::CodeForCall; }

    const void* constants; // const WriteBarrier<Unknown>*. Unless constantsAreOfNoRealm, the Data has them.
    const void* identifiers; // const Identifier*
    const Site* sites; // One for each slot.
    const ImageFunction* function; // If the code is in an image.
    uintptr_t executableAndKind; // With the low bit set if the code is for construction.
    uint32_t numSlots;
    uint16_t unused;
    uint16_t flags;
};
static_assert(sizeof(FunctionInfo) == 48);

// One for each realm that runs code from the static compiler.
struct Instance {
    static Instance& ensure(JSGlobalObject*);
    static void destroy(Instance*);

    // For the collector. Nothing is kept alive because a slot refers to it.
    template<typename Visitor> void visit(Visitor&, bool onlyWhatIsNew);
    void finalizeUnconditionally(bool onlyWhatIsNew);

    static constexpr ptrdiff_t offsetOfRuntimeTable() { return OBJECT_OFFSETOF(Instance, runtimeTable); }
    static constexpr ptrdiff_t offsetOfVM() { return OBJECT_OFFSETOF(Instance, vm); }
    static constexpr ptrdiff_t offsetOfGlobalObject() { return OBJECT_OFFSETOF(Instance, globalObject); }
    static constexpr ptrdiff_t offsetOfData() { return OBJECT_OFFSETOF(Instance, data); }
    static constexpr ptrdiff_t offsetOfInfos() { return OBJECT_OFFSETOF(Instance, infos); }
    static constexpr ptrdiff_t offsetOfConstantsOfProgram() { return OBJECT_OFFSETOF(Instance, constantsOfProgram); }
    static constexpr ptrdiff_t offsetOfSharedData() { return OBJECT_OFFSETOF(Instance, sharedData); }
    static constexpr ptrdiff_t offsetOfMisses() { return OBJECT_OFFSETOF(Instance, misses); }
    // The function's own Data, which it gets now if it has been doing without (SharedData). It has been linked.
    JS_EXPORT_PRIVATE Data* ensureData(uint32_t index);
    void countMiss(uint32_t index)
    {
        if (++misses[index] == static_cast<uint16_t>(missesToPutUpWithFor(infos[index].numSlots)))
            ensureData(index);
    }
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
    static constexpr ptrdiff_t offsetOfStructureIDBase() { return OBJECT_OFFSETOF(Instance, structureIDBase); }
    // What an object of a known shape does not have itself, it does not have at all if it inherits from Object.prototype alone and
    // that does not have it either. This looks at what that has now, if it is not what it had when this last looked.
    void lookAtObjectPrototype();
    static constexpr ptrdiff_t offsetOfIntrinsics() { return OBJECT_OFFSETOF(Instance, intrinsics); }
    static constexpr ptrdiff_t offsetOfObjectPrototype() { return OBJECT_OFFSETOF(Instance, objectPrototype); }
    static constexpr ptrdiff_t offsetOfStructureIDOfObjectPrototype() { return OBJECT_OFFSETOF(Instance, structureIDOfObjectPrototype); }
    static constexpr ptrdiff_t offsetOfSelectorsOnObjectPrototype() { return OBJECT_OFFSETOF(Instance, selectorsOnObjectPrototype); }
    static constexpr ptrdiff_t offsetOfDispatch() { return OBJECT_OFFSETOF(Instance, dispatch); }
    static constexpr ptrdiff_t offsetOfRowsOfSelectors() { return OBJECT_OFFSETOF(Instance, rowsOfSelectors); }

    JS_EXPORT_PRIVATE void dumpSlotStatistics(); // TEMPORARY-SLOT-STATS

    void** runtimeTable;
    JSGlobalObject* globalObject;
    VM* vm; // Where a JSWebAssemblyInstance has its own: code that finds the VM from any frame need not tell the two apart.
    struct Collections;
    Collections* collections; // Of what there is in data.
    FunctionInfo* infos; // By CodeHeader::index, like data.
    Data* sharedData; // SharedData::get()
    uint16_t* misses; // By CodeHeader::index: how often a slot has failed a function that has no Data of its own.
    const uint32_t* factsOfFunctions; // By CodeHeader::index: StaticHeap::factsAt(). Zero: none. Null: no function has any.
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
    Data* data[0]; // By CodeHeader::index. Null: the function has not been linked in this realm.
};

// TEMPORARY-SHAPE-STATS: structures whose layout the compiler could have known. 1: of an object literal. 2: what a constructor's stores end in.
void noteKnownShape(Structure*, uint8_t kind);
uint8_t kindOfKnownShape(Structure*);

// Whether the constants of the unlinked code will do for any realm as they are. A SymbolTable does if it is one that was made when the
// program was built (SymbolTable::isItsOwnClone()), which is not for the compiler to say: it comes first.
enum class SymbolTablesWillDo : bool { No, Yes };
JS_EXPORT_PRIVATE bool constantsAreOfNoRealm(UnlinkedCodeBlock*, SymbolTablesWillDo = SymbolTablesWillDo::No);

// A number for a function that is compiled in this process. The functions of an image have theirs already, from zero.
uint32_t allocateFunctionIndex();
bool reserveFunctionIndicesForImage(uint32_t count); // False: too late.

// Whether something that may have been read out of a frame at any moment at all is one. Any thread.
JS_EXPORT_PRIVATE bool isCodeHeader(const void*);
// About a frame whose callee is a CodeHeader.
JS_EXPORT_PRIVATE Data* dataOf(const CallFrame*);
JS_EXPORT_PRIVATE CodeBlock* codeBlockOf(const CallFrame*);
JS_EXPORT_PRIVATE JSObject* calleeOf(const CallFrame*); // Null if the function has no use for it (CodeHeader::calleeSlot).
JS_EXPORT_PRIVATE VM& vmOf(const CallFrame*);
JS_EXPORT_PRIVATE JSGlobalObject* globalObjectOf(const CallFrame*);

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
    unsigned codeSize { 0 };
    unsigned entryOffset { 0 };
    unsigned arityCheckOffset { 0 };
    // For a caller that knows, without looking, that this is the function it is calling: which may then be the first call there ever
    // was, with nothing of what the function has of the realm made yet. Zero: there is no such way in.
    unsigned directEntryOffset { 0 };
    unsigned frameSizeInBytes { 0 };
    unsigned numSlots { 0 };
    unsigned bytecodeHash { 0 }; // hashOfBytecode() of what it was compiled from.
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
};

struct ImageCatchEntrypoint {
    uint32_t bytecodeOffset;
    uint32_t codeOffset;
};

// Followed by numberOfCatchEntrypoints ImageCatchEntrypoint, then numSlots Site, then
// numSlots uint32_t (CompiledFunctionInfo::siteConstants, as the image numbers them: zero for none), then numberOfKnownCallees ImageKey,
// then CompiledFunctionInfo::plans.
struct ImageFunction {
    uint32_t codeOffset; // In the code.
    uint32_t codeSize;
    uint16_t entryOffset;
    uint16_t arityCheckOffset;
    uint16_t directEntryOffset;
    uint16_t numberOfCatchEntrypoints;
    uint32_t frameSizeInBytes;
    uint32_t numSlots;
    uint32_t bytecodeHash;
    uint32_t numberOfKnownCallees : 29;
    uint32_t hasSiteConstants : 1; // If not, see FunctionInfo::sitesHaveTheirConstants.
    uint32_t usesStaticImports : 1; // See Graph::usesStaticImports.
    uint32_t startsCold : 1; // See CompiledFunctionInfo::startsCold.
    uint32_t quotes; // From ImageHeader::quotesOffset. Zero: none.
    uint32_t constructSites; // Likewise. See Image::constructsAt().
    // The registers that the function saves, by Reg::index(). They are next to each other in the frame, in that order, the way
    // Air::Code puts them: this is where the first of them is.
    uint32_t calleeSaveRegisters[2];
    int32_t offsetOfCalleeSaves;

    const ImageCatchEntrypoint* catchEntrypoints() const { return reinterpret_cast<const ImageCatchEntrypoint*>(this + 1); }
    const Site* sites() const { return reinterpret_cast<const Site*>(catchEntrypoints() + numberOfCatchEntrypoints); }
    const uint32_t* siteConstants() const { return reinterpret_cast<const uint32_t*>(sites() + numSlots); }
    const uint32_t* knownCallees() const { return siteConstants() + (hasSiteConstants ? numSlots : 0); } // CodeHeader::index of each. Or, if the image has no code for it, noSuchFunction.
    const uint32_t* plans() const { return knownCallees() + numberOfKnownCallees; }
    static constexpr uint32_t noSuchFunction = std::numeric_limits<uint32_t>::max();
};

class JITCode final : public JSC::JITCode {
public:
    // The code is either in memory that the handle owns, or in an image that is mapped for as long as the process lives. What there
    // is to know about code in an image is in the image, and stays there.
    // Way: how whoever makes frames the way the interpreter wants them gets in (generateEnter(), generateEnterFunction()).
    enum class Way : uint8_t { TopLevel, Call, Construct };
    static Way wayInto(UnlinkedCodeBlock*);
    JITCode(void* code, RefPtr<ExecutableMemoryHandle>&&, CompiledFunctionInfo&&, Way);
    JITCode(void* code, const ImageFunction&, Way);
    ~JITCode() final;

    CodePtr<JSEntryPtrTag> addressForCall(ArityCheckMode) final;
    void* executableAddressAtOffset(size_t offset) final;
    void* dataAddressAtOffset(size_t offset) final;
    unsigned offsetOf(void* pointerIntoCode) final;
    size_t size() final;
    bool contains(void*) final;

    const RegisterAtOffsetList* calleeSaveRegisters() const { return m_calleeSaveRegisters; }
    const CompiledFunctionInfo& info() const { return m_owned->info; } // Not of code in an image.
    bool isFromImage() const { return !!m_function; }
    const ImageFunction* imageFunction() const { return m_function; }
    unsigned codeSize() const { return m_function ? m_function->codeSize : m_owned->info.codeSize; }
    unsigned entryOffset() const { return m_function ? m_function->entryOffset : m_owned->info.entryOffset; }
    unsigned arityCheckOffset() const { return m_function ? m_function->arityCheckOffset : m_owned->info.arityCheckOffset; }
    void* directEntry() const { return tagCodePtr<JSEntryPtrTag>(static_cast<uint8_t*>(m_code) + (m_function ? m_function->directEntryOffset : m_owned->info.directEntryOffset)); }
    unsigned frameSizeInBytes() const { return m_function ? m_function->frameSizeInBytes : m_owned->info.frameSizeInBytes; }
    unsigned numSlots() const { return m_function ? m_function->numSlots : m_owned->info.numSlots; }
    const Site* sites() const { return m_function ? m_function->sites() : m_owned->info.sites.span().data(); }
    template<typename Functor> void forEachCatchEntrypoint(const Functor& functor) const // (offset of the op_catch, offset in the code)
    {
        if (m_function) {
            for (unsigned i = 0; i < m_function->numberOfCatchEntrypoints; ++i)
                functor(m_function->catchEntrypoints()[i].bytecodeOffset, m_function->catchEntrypoints()[i].codeOffset);
            return;
        }
        for (auto& [bytecodeOffset, codeOffset] : m_owned->info.catchEntrypoints)
            functor(bytecodeOffset, codeOffset);
    }
    const void* start() const { return m_code; }
    const CodeHeader& header() const { return *static_cast<const CodeHeader*>(m_code); }
    // Where a caller that has put the Instance in the frame goes. Checks the number of arguments.
    static constexpr ptrdiff_t offsetOfEntry() { return OBJECT_OFFSETOF(JITCode, m_entry); }
    // One of these is the code of one executable, which is of one realm.
    static constexpr ptrdiff_t offsetOfInstance() { return OBJECT_OFFSETOF(JITCode, m_instance); }
    Instance* instance() const { return m_instance; }
    void setInstance(Instance& instance) { m_instance = &instance; }

private:
    struct Owned {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Owned);
        RefPtr<ExecutableMemoryHandle> handle;
        CompiledFunctionInfo info;
    };

    void* m_code;
    void* m_entry;
    Instance* m_instance { nullptr };
    const ImageFunction* m_function { nullptr };
    const RegisterAtOffsetList* m_calleeSaveRegisters;
    std::unique_ptr<Owned> m_owned;
};

// Makes the code the function's. It gets no CodeBlock. False: an exception was thrown.
bool install(VM&, FunctionExecutable*, CodeSpecializationKind, UnlinkedCodeBlock*, JSGlobalObject*, Ref<JITCode>&&);
// An executable that was made when the program was built (FunctionExecutable::aotEntryFor()) is about to be run for the first time:
// gives its code what it has of the realm. False if the code is not for the realm the function is of.
bool linkStaticFunction(VM&, FunctionExecutable*, CodeSpecializationKind, JSScope*);

unsigned hashOfBytecode(UnlinkedCodeBlock*);
// Where code from the JIT that wants to call `code` with a call instruction, whose reach is limited, can call. Any thread.
void* nearCallTargetFor(void* code);
void* catchThunk();
// A copy of a stub that is there for good: in an image if there is one, so that nothing has to be generated.
void* addressOfStub(Stub);

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
