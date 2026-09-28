/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(FTL_JIT)

#include "AOTOperationsObjects.h"
#include "AOTSlotWatchpoint.h"
#include "AOTStubs.h"
#include "CallLinkInfo.h"
#include "ExecutableAllocator.h"
#include "JITCode.h"
#include "Opcode.h"
#include "RegisterAtOffsetList.h"
#include "StructureID.h"
#include <wtf/TZoneMalloc.h>

namespace JSC {

class CodeBlock;
class JSGlobalObject;
class CallFrame;
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
    void* m_entries[numberOfEntries];
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
    int16_t calleeSlot { 0 }; // Where in its frames the function keeps the object it was called as: which Register, from the frame pointer.
    uint32_t index { 0 }; // Which function: see Instance::data.

    static const CodeHeader* fromCallee(CalleeBits bits) { return std::bit_cast<const CodeHeader*>(bits.asNativeCallee()); }
};
static_assert(sizeof(CodeHeader) == sizeof(NativeCallee) && alignof(CodeHeader) == alignof(NativeCallee));

struct Data;

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

    // As many as there could ever be. It is addresses that are set aside, not memory.
    static constexpr size_t maxFunctions = 4 << 20;

    void** runtimeTable;
    JSGlobalObject* globalObject;
    VM* vm; // Where a JSWebAssemblyInstance has its own: code that finds the VM from any frame need not tell the two apart.
    struct Collections;
    Collections* collections; // Of what there is in data.
    Data* data[0]; // By CodeHeader::index. Null: the function has not been linked in this realm.
};

// A number for a function that is compiled in this process. The functions of an image have theirs already, from zero.
uint32_t allocateFunctionIndex();
bool reserveFunctionIndicesForImage(uint32_t count); // False: too late.

// Whether something that may have been read out of a frame at any moment at all is one. Any thread.
JS_EXPORT_PRIVATE bool isCodeHeader(const void*);
// About a frame whose callee is a CodeHeader.
JS_EXPORT_PRIVATE Data* dataOf(const CallFrame*);
JS_EXPORT_PRIVATE CodeBlock* codeBlockOf(const CallFrame*);
JS_EXPORT_PRIVATE JSObject* calleeOf(const CallFrame*);

struct Data {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(Data);

    // Puts it in its place in the Instance, and takes it out.
    static Data* create(Instance&, CodeBlock*, JITCode&);
    static void destroy(Data*);
    void noteFilled();
    template<typename Visitor> void visit(Visitor&);

    // Structures do not keep their IDs to themselves when they die.
    void finalizeUnconditionally(VM&);

    static constexpr ptrdiff_t offsetOfConstants() { return OBJECT_OFFSETOF(Data, constants); }
    static constexpr ptrdiff_t offsetOfIdentifiers() { return OBJECT_OFFSETOF(Data, identifiers); }
    static constexpr ptrdiff_t offsetOfSites() { return OBJECT_OFFSETOF(Data, sites); }
    static constexpr ptrdiff_t offsetOfSlots() { return OBJECT_OFFSETOF(Data, slots); }
    static constexpr ptrdiff_t offsetOfSlotEpoch() { return OBJECT_OFFSETOF(Data, slotEpoch); }

    CodeBlock* codeBlock; // For the time being: what the rest of the engine takes the function's frames to be running.
    Instance* instance;
    UnlinkedCodeBlock* unlinkedCodeBlock;
    JITCode* code; // Has a reference.
    const void* constants; // const WriteBarrier<Unknown>*
    const void* identifiers; // const Identifier*
    const Site* sites; // One for each slot. The code's, not this CodeBlock's.
    SlotWatchpointMap* watchpoints; // For the slots whose caches rest on more than the code checks. Null until there is one.
    unsigned numSlots;
    bool hasBeenFilledSinceLastCollection;
    // Another number whenever the cache of a property access has become one for something else, or for nothing. What code has found
    // out about such caches, and nothing but them, it need not find out again while this is the same (GuardKind::BeginSlotChecks).
    uint64_t slotEpoch;
    Slot slots[0];
};

// What a compilation produces, other than the code: all of it is plain data, and none of it is an address.
struct CompiledFunctionInfo {
    unsigned codeSize { 0 };
    unsigned entryOffset { 0 };
    unsigned arityCheckOffset { 0 };
    unsigned frameSizeInBytes { 0 };
    unsigned numSlots { 0 };
    unsigned bytecodeHash { 0 }; // hashOfBytecode() of what it was compiled from.
    bool usesStaticImports { false };
    RegisterAtOffsetList calleeSaveRegisters;
    Vector<std::pair<unsigned, unsigned>> catchEntrypoints; // Bytecode offset of the op_catch, offset in the code.
    Vector<StubCall> stubCalls; // For whoever moves the code.
    Vector<Site> sites; // numSlots of them.
    Vector<ImageKey> knownCallees; // The functions that calls were compiled for.
};

struct ImageCalleeSave {
    uint32_t reg;
    int32_t offset;
};
struct ImageCatchEntrypoint {
    uint32_t bytecodeOffset;
    uint32_t codeOffset;
};

// Followed by numberOfCalleeSaves ImageCalleeSave, then numberOfCatchEntrypoints ImageCatchEntrypoint, then numSlots Site, then
// numberOfKnownCallees ImageKey.
struct ImageFunction {
    uint64_t codeOffset; // In the code.
    uint32_t codeSize;
    uint32_t entryOffset;
    uint32_t arityCheckOffset;
    uint32_t frameSizeInBytes;
    uint32_t numSlots;
    uint16_t numberOfCalleeSaves;
    uint16_t numberOfCatchEntrypoints;
    uint32_t bytecodeHash;
    uint32_t numberOfKnownCallees;
    uint32_t usesStaticImports; // See Graph::usesStaticImports.
    uint32_t unused;

    const ImageCalleeSave* calleeSaves() const { return reinterpret_cast<const ImageCalleeSave*>(this + 1); }
    const ImageCatchEntrypoint* catchEntrypoints() const { return reinterpret_cast<const ImageCatchEntrypoint*>(calleeSaves() + numberOfCalleeSaves); }
    const Site* sites() const { return reinterpret_cast<const Site*>(catchEntrypoints() + numberOfCatchEntrypoints); }
    const ImageKey* knownCallees() const { return reinterpret_cast<const ImageKey*>(sites() + numSlots); }
};

class JITCode final : public JSC::JITCode {
public:
    // The code is either in memory that the handle owns, or in an image that is mapped for as long as the process lives. What there
    // is to know about code in an image is in the image, and stays there.
    JITCode(void* code, RefPtr<ExecutableMemoryHandle>&&, CompiledFunctionInfo&&);
    JITCode(void* code, const ImageFunction&);
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
    unsigned codeSize() const { return m_function ? m_function->codeSize : m_owned->info.codeSize; }
    unsigned entryOffset() const { return m_function ? m_function->entryOffset : m_owned->info.entryOffset; }
    unsigned arityCheckOffset() const { return m_function ? m_function->arityCheckOffset : m_owned->info.arityCheckOffset; }
    unsigned frameSizeInBytes() const { return m_function ? m_function->frameSizeInBytes : m_owned->info.frameSizeInBytes; }
    unsigned numSlots() const { return m_function ? m_function->numSlots : m_owned->info.numSlots; }
    const Site* sites() const { return m_function ? m_function->sites() : m_owned->info.sites.span().data(); }
    const ImageKey& knownCallee(unsigned index) const { return m_function ? m_function->knownCallees()[index] : m_owned->info.knownCallees[index]; }
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

private:
    struct Owned {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Owned);
        RefPtr<ExecutableMemoryHandle> handle;
        CompiledFunctionInfo info;
    };

    void* m_code;
    void* m_entry;
    const ImageFunction* m_function { nullptr };
    const RegisterAtOffsetList* m_calleeSaveRegisters;
    std::unique_ptr<Owned> m_owned;
};

unsigned hashOfBytecode(UnlinkedCodeBlock*);
// Where code from the JIT that wants to call `code` with a call instruction, whose reach is limited, can call. Any thread.
void* nearCallTargetFor(void* code);
void* catchThunk();
// A copy of a stub that is there for good: in an image if there is one, so that nothing has to be generated.
void* addressOfStub(Stub);

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
