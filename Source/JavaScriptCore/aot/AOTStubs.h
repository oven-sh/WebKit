/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "CCallHelpers.h"
#include <wtf/Vector.h>

namespace JSC {

class LinkBuffer;

namespace AOT {

// Shared code that every function would otherwise duplicate. A whole program has a few hundred thousand functions and a few million
// property accesses. The common part of each operation lives here once, and the function contains only a call instruction. Like the
// functions, stubs contain no absolute addresses. They are placed in the image with the functions, within range of a call
// instruction (ImageBuilder emits as many copies as that requires).
//
// Stubs have their own calling conventions. A0... are the argument registers and T9... the temporaries with those names. No stub
// takes an argument in the macro assembler's scratch registers, and every stub clobbers them. Unless stated otherwise, a stub
// clobbers what a C function may clobber. A stub that can throw does not return if an exception is thrown; it jumps to the handler.
//
// "site" is the address of one of the function's Slots, which has a corresponding Site.
#define FOR_EACH_AOT_OPERATION_WITH_FRONT_END(v) \
    v(GetById) \
    v(GetByVal) \
    v(PutById) \
    v(PutByVal) \
    v(NewObject) \
    v(NewObjectLiteral) \
    v(CreateThis) \
    v(CreateThisWithProperties) \
    v(NewFunction) \
    v(CompareStrictEq) \
    v(CompareEq) \
    v(InById) \

// A call to a method with one of these names and the matching argument count. If the callee turns out to be the built-in function
// of that name, and `this` and the arguments are in a form the stub handles, the stub performs the operation itself. Otherwise the
// call proceeds normally. For a name shared by several built-in classes, the type of `this` selects the function. The last column
// says which calls qualify: all, those whose result is used, or those whose result is unused.
#define FOR_EACH_AOT_STUB_INTRINSIC(v) \
    v(CharCodeAt, "charCodeAt", 1, Any) \
    v(CodePointAt, "codePointAt", 1, Any) \
    v(CharAt, "charAt", 1, Any) \
    v(Push, "push", 1, Any) \
    v(Pop, "pop", 0, Any) \
    v(IsArray, "isArray", 1, Any) \
    /* For a Map or a Set. Inserting a key that is not present is beyond what the stub does itself. That case calls the */ \
    /* function, unless the result is unused. */ \
    v(Get, "get", 1, Any) \
    v(Has, "has", 1, Any) \
    v(Set, "set", 2, Wanted) \
    v(SetIgnoringResult, "set", 2, ResultUnused) \
    v(Add, "add", 1, Wanted) \
    v(AddIgnoringResult, "add", 1, ResultUnused) \

enum class StubIntrinsic : uint8_t {
    None,
#define AOT_DEFINE_STUB_INTRINSIC(name, text, argumentCount, result) name,
    FOR_EACH_AOT_STUB_INTRINSIC(AOT_DEFINE_STUB_INTRINSIC)
#undef AOT_DEFINE_STUB_INTRINSIC
    NumberOfStubIntrinsics
};
static constexpr unsigned numberOfStubIntrinsics = static_cast<unsigned>(StubIntrinsic::NumberOfStubIntrinsics) - 1;
StubIntrinsic stubIntrinsicFor(UniquedStringImpl* name, unsigned argumentCountIncludingThis, bool usesResult);

// Offsets of what an entry adapter (see adapt()) saves, relative to its frame pointer.
static constexpr ptrdiff_t offsetOfInstanceRegisterInAdapter = -24; // In RegisterAtOffsetList order.
static constexpr ptrdiff_t offsetOfNumberTagRegisterInAdapter = -16;
static constexpr ptrdiff_t offsetOfNotCellMaskRegisterInAdapter = -8;
static constexpr ptrdiff_t offsetOfInstanceInAdapter = -32; // The Instance of the code being entered.
static constexpr unsigned adapterSaveAreaSize = 32;

// Instance::granulesOfCode has one entry per 1 << shiftOfGranuleOfCode bytes of image code.
static constexpr unsigned shiftOfGranuleOfCode = 10;

// Operations that have a helper in front of them, in the way others have a front end (AOTThunks.h). A caller of the operation gets
// the helper's result if the helper succeeds. This lets size-sensitive code make a single call.
#define FOR_EACH_AOT_OPERATION_BEHIND_HELPER(v) \
    v(NewArray, operationAOTNewArray) \
    v(NewArrayBuffer, operationAOTNewArrayBuffer) \
    v(NewArrayWithSpread, operationAOTNewArrayWithSpread) \
    v(NewArrayWithSpecies, operationAOTNewArrayWithSpecies) \
    v(CreateRest, operationAOTCreateRest) \
    v(CreateLexicalEnvironment, operationAOTCreateLexicalEnvironment) \
    v(MakeRope2, operationMakeRope2) \
    v(MakeRope3, operationMakeRope3) \
    v(StringSliceWithEnd, operationStringSliceWithEnd) \
    v(StringSubstringWithEnd, operationStringSubstringWithEnd) \
    v(ToLowerCase, operationToLowerCase) \
    v(ObjectKeysObject, operationObjectKeysObject) \
    v(ValueAdd, operationAOTValueAdd) \

#define FOR_EACH_AOT_HELPER(v) \
    v(HelperNewArray) \
    v(HelperNewArrayOfInt32) \
    v(HelperNewArrayBuffer) \
    v(HelperNewActivation) \
    v(HelperNewArrayWithSpread) \
    v(HelperNewArrayWithSpecies) \
    v(HelperStringSlice) \
    v(HelperStringSubstring) \
    v(HelperMakeRope2) \
    v(HelperMakeRope3) \
    v(HelperToLowerCase) \
    v(HelperObjectKeys) \
    v(HelperAddField) \
    v(HelperSetArrayLength) \
    v(HelperAddStrings) \

#define FOR_EACH_AOT_STUB(v) \
    /* Called after the frame pointer is set up. T9 = frame size. Checks for stack overflow and sets the stack pointer. */ \
    /* Clobbers only T11 and T12. */ \
    v(Prologue) \
    /* Where a Signature::List function returns to when Call called it: the frame in which Call put the list is given up. One */ \
    /* place for all of Call's copies, so that Call can tell such a frame by its return address. */ \
    v(ReturnFromCallWithList) \
    /* Generic call. calleeGPR = callee, thisGPR = this (for Construct: new.target), countGPR = argument count. Arguments are */ \
    /* in the registers in which a Signature::Registers function takes its parameters. Result in the return value register. */ \
    /* Jumping here with nothing of the caller's on the stack makes it a tail call. These stubs come first because other stubs */ \
    /* end by jumping to them. */ \
    v(Call) \
    v(Construct) \
    /* The same, with arguments in memory as for a Signature::List function: argumentGPR(0) = count, argumentGPR(1) = address. */ \
    /* The memory must stay valid until the stub returns. */ \
    v(CallList) \
    v(ConstructList) \
    /* The same, with arguments from an array-like object: argumentGPR(0) = the object, as in op_call_varargs, argumentGPR(1) = */ \
    /* number of leading elements to skip. */ \
    v(CallVarargs) \
    v(ConstructVarargs) \
    /* CallVarargs in tail position. The caller has restored its callee-saved registers but still has its frame. It calls this */ \
    /* stub, which does not return: the callee returns directly to the caller's caller, and the caller's frame is gone by then. */ \
    v(TailCallVarargs) \
    /* CallList in tail position, under the same terms. The list is in the caller's frame. */ \
    v(TailCallList) \
    /* Like Call, where the callee may be the function of a StubIntrinsic. T9 = which intrinsic. There is one thunk per */ \
    /* intrinsic, and the stub is only entered through them. */ \
    v(CallIntrinsic) \
    /* See generateEnter(). */ \
    v(Enter) \
    v(EnterFunctionForCall) \
    v(EnterFunctionForConstruct) \
    /* See generateEnterStaticFunction(). */ \
    v(EnterStaticFunctionForCall) \
    v(EnterStaticFunctionForConstruct) \
    /* See generateConstructByCalling(). */ \
    v(ConstructByCalling) \
    /* See generateCallBoundFunction(). */ \
    v(CallBoundFunction) \
    /* Called by a function that finds it has not been linked to the realm yet. Preserves every register. */ \
    v(LinkFunction) \
    /* Calls to C++. T9 = Entry * 8, with arguments where the C++ ABI expects them. The Plain variants are for operations that */ \
    /* neither throw nor walk the stack. In the WithGlobalObject and WithVM variants the stub supplies the first argument. */ \
    v(OperationValue) \
    v(OperationVoid) \
    v(OperationDouble) \
    v(OperationValueWithGlobalObject) \
    v(OperationVoidWithGlobalObject) \
    v(OperationDoubleWithGlobalObject) \
    /* The same (global object, then A1 and A2; no result), for rarely executed calls. Preserves all registers except T9, T10 */ \
    /* and the assembler's scratch registers. OfLeaf: the caller has no frame and T10 = its return address. The stub sets up a */ \
    /* frame for the duration of the call. */ \
    v(ColdOperationVoid) \
    v(ColdOperationVoidOfLeaf) \
    /* The same, with the result in the return value register. */ \
    v(ColdOperationValue) \
    v(ColdOperationValueOfLeaf) \
    v(PlainOperation) \
    v(PlainOperationWithGlobalObject) \
    v(PlainOperationWithVM) \
    /* A0 = the cell that was stored to. Clobbers only T9-T11. */ \
    v(WriteBarrier) \
    /* A0 = a value. Returns its truthiness (0 or 1) in A0. Clobbers only A0 and T9-T11. */ \
    v(ToBoolean) \
    /* A0, A1 = values. Returns whether they are equal (0 or 1) in A0. */ \
    v(StrictEqual) \
    v(LooseEqual) \
    /* A0, A1 = operands. Result (a JSValue) in A0. */ \
    v(Add) \
    v(IsStringEqualTo) \
    v(NarrowCharacters) \
    v(Mod) \
    v(Sub) \
    v(Mul) \
    v(BitAnd) \
    v(BitOr) \
    v(BitXor) \
    v(LShift) \
    v(RShift) \
    v(URShift) \
    /* The same, but the result is 0 or 1. */ \
    v(Less) \
    v(LessEq) \
    v(Greater) \
    v(GreaterEq) \
    /* A0 = base, A1 = an unboxed integer index (Rep::Int64). Result in A0. Falls through to GetByVal for cases it does not */ \
    /* handle. */ \
    v(GetByValAtIndex) \
    /* A0 = base, A1 = property. Result in A0. */ \
    v(GetByVal) \
    /* Like PutByVal, except that A1 is an unboxed integer index. Falls through to PutByVal. */ \
    v(PutByValAtIndex) \
    /* A0 = base, A1 = property, A2 = value, A3 = whether the code is strict. */ \
    v(PutByVal) \
    /* The same, for op_put_by_val_direct. The base is an object. */ \
    v(PutByValDirect) \
    /* A0 = base, A1 = site, whose identifier is WellKnownIdentifier::Length. Result in A0. */ \
    v(GetLength) \
    /* op_resolve_scope fused with the op_get_from_scope that uses it. A0 = scope, A1 = site of the first. The second's site */ \
    /* follows it. Result in A0. */ \
    v(GetGlobal) \
    /* A0 = base, A1 = site. Result in A0. */ \
    v(GetById) \
    /* A0 = base, A1 = the ID of a field that uses this slot (TypedLayoutTable::Field::id). Result in A0: the field's value. If */ \
    /* it cannot produce one, an exception is thrown and the stub does not return. */ \
    v(ReadSlot0) \
    v(ReadSlot1) \
    v(ReadSlot2) \
    v(ReadSlot3) \
    v(ReadSlot4) \
    v(ReadSlot5) \
    v(ReadSlot6) \
    v(ReadSlot7) \
    v(ReadSlot8) \
    v(ReadSlot9) \
    v(ReadSlot10) \
    v(ReadSlot11) \
    v(ReadSlot12) \
    v(ReadSlot13) \
    v(ReadSlot14) \
    v(ReadSlot15) \
    /* The same, but returns undefined for an object that lacks the property. */ \
    v(ReadSlotOrUndefined0) \
    v(ReadSlotOrUndefined1) \
    v(ReadSlotOrUndefined2) \
    v(ReadSlotOrUndefined3) \
    v(ReadSlotOrUndefined4) \
    v(ReadSlotOrUndefined5) \
    v(ReadSlotOrUndefined6) \
    v(ReadSlotOrUndefined7) \
    v(ReadSlotOrUndefined8) \
    v(ReadSlotOrUndefined9) \
    v(ReadSlotOrUndefined10) \
    v(ReadSlotOrUndefined11) \
    v(ReadSlotOrUndefined12) \
    v(ReadSlotOrUndefined13) \
    v(ReadSlotOrUndefined14) \
    v(ReadSlotOrUndefined15) \
    /* A0 = base, A1 = value, A2 = site. */ \
    v(PutById) \
    /* A0 = base, A1 = private name or brand, A2 = site. GetPrivateName returns the result in A0. */ \
    v(GetPrivateName) \
    v(CheckPrivateBrand) \
    /* A0 = base, A1 = private name, A2 = value, A3 = site, whose extra bits say whether this defines the field. */ \
    v(PutPrivateName) \
    /* A0 = scope, A1 = site. Result in A0. */ \
    v(ResolveScope) \
    v(GetFromScope) \
    /* A0 = scope, A1 = value, A2 = site. */ \
    v(PutToScope) \
    /* GetById for a site whose identifier is a WellKnownIdentifier. */ \
    v(GetByIdWellKnown) \
    /* A0 = an object, A1 = a value. Returns whether A1 is on A0's prototype chain (0 or 1) in A0. */ \
    v(InstanceOf) \
    /* A0 = next, A1 = iterator, A2 = iterable, as in op_iterator_next. A3 = two consecutive slots, for `done` and `value`. */ \
    /* Returns A0 = done, A1 = value, A2 = next. */ \
    v(IteratorNext) \
    /* A0 = iterable, A1 = its @@iterator, as in op_iterator_open. A2 = a slot, for `next`. Returns A0 = iterator, A1 = next. */ \
    v(IteratorOpen) \
    /* A0 = iterator, A1 = iterable, A2 = next, as in op_iterator_close_check. Returns A0 = the iterator to close. See */ \
    /* lowerIteratorCloseCheck(). */ \
    v(IteratorCloseCheck) \
    /* The following replace thunks that the JIT normally generates, since there may be no JIT. */ \
    /* Jumped to from the frame of a compiled function in which (or in a callee of which) an exception was thrown, with the */ \
    /* link register holding the current location. Finds the handler and jumps to it. */ \
    v(HandleException) \
    /* Jumped to from Prologue, in the frame that does not fit. */ \
    v(ThrowStackOverflowAtPrologue) \
    /* Where the unwinder (genericUnwind()) transfers control for a handler in compiled code, from any kind of frame. Restores */ \
    /* the callee-saved registers and the frame pointer, then jumps to VM::targetMachinePCAfterCatch. */ \
    v(Catch) \
    /* Like the JIT's virtual call thunks. Called (or, for a tail call, jumped to) with the frame set up except for its */ \
    /* CodeBlock. T0 = callee, T2 = the CallLinkInfo of a VirtualCallInfo. */ \
    v(VirtualCall) \
    v(VirtualConstruct) \
    v(VirtualTailCall) \
    /* See AOTThunks.h. */ \
    v(FrontEndGetById) \
    v(FrontEndGetByVal) \
    v(FrontEndPutById) \
    v(FrontEndPutByVal) \
    v(FrontEndNewObject) \
    v(FrontEndNewObjectLiteral) \
    v(FrontEndCreateThis) \
    v(FrontEndCreateThisWithProperties) \
    v(FrontEndNewFunction) \
    v(FrontEndCompareStrictEq) \
    v(FrontEndCompareEq) \
    v(FrontEndInById) \
    /* See generateHelper(). */ \
    FOR_EACH_AOT_HELPER(v) \
    v(AheadOfNewArray) \
    v(AheadOfNewArrayBuffer) \
    v(AheadOfNewArrayWithSpread) \
    v(AheadOfNewArrayWithSpecies) \
    v(AheadOfCreateRest) \
    v(AheadOfCreateLexicalEnvironment) \
    v(AheadOfMakeRope2) \
    v(AheadOfMakeRope3) \
    v(AheadOfStringSliceWithEnd) \
    v(AheadOfStringSubstringWithEnd) \
    v(AheadOfToLowerCase) \
    v(AheadOfObjectKeysObject) \
    v(AheadOfValueAdd) \

enum class Stub : uint8_t {
#define AOT_DEFINE_STUB(name) name,
    FOR_EACH_AOT_STUB(AOT_DEFINE_STUB)
#undef AOT_DEFINE_STUB
    NumberOfStubs
};
static constexpr unsigned numberOfStubs = static_cast<unsigned>(Stub::NumberOfStubs);
static constexpr bool isHelper(Stub stub) { return stub >= Stub::HelperNewArray && stub <= Stub::HelperAddStrings; }

static constexpr bool usesStubs =
#if CPU(ARM64)
    true;
#else
    false;
#endif

// Immutable information about a code location that has a Slot, needed only on the slow path. It is kept out of the Slot because it
// never changes, and out of the code because materializing it there would cost instructions.
struct Site {
    static constexpr unsigned identifierBits = 20;
    static constexpr unsigned extraBits = 32 - identifierBits;
    static bool fits(unsigned identifier, unsigned extra) { return identifier < (1u << identifierBits) && extra < (1u << extraBits); }

    // op_resolve_scope: the number of scopes the function itself has pushed at that point, or resolvesInGlobalScopes if only the
    // global scopes need to be searched.
    static constexpr unsigned resolvesInGlobalScopes = (1u << extraBits) - 1;
    // op_get_from_scope.
    static constexpr unsigned throwsIfNotFound = 1;
    static constexpr unsigned isImport = 2; // The scope is a module environment and the name is one of its imports.

    uint32_t identifierAndExtra { 0 }; // Index of one of the function's identifiers, with operation-specific extra bits above it.
};

// The initial properties of an object, where the allocating code makes them evident: the property names in insertion order. All
// objects allocated with the same KnownShape share one Structure, whose layout is known at compile time.
struct KnownShape {
    unsigned inlineCapacity { 0 }; // Zero means any capacity that fits all the properties.
    Vector<UniquedStringImpl*, 8> names;
    // If this is one of the type table's layouts (TypeTable::Layout): its number and the slot of each property. Otherwise the
    // properties use consecutive slots and a number is assigned.
    uint32_t number { 0 };
    Vector<uint16_t, 8> slots;
    // With typed fields (TypeTable::tableHasTypedFields()): the typed layout ID and the number of slots every object with that
    // layout has.
    uint16_t layoutID { 0 };
    uint16_t reserved { 0 };
    uint16_t inlineSlots { 0 }; // Slots at or beyond this index are out of line.
    bool hasSlotsOutside() const { return layoutID && reserved > inlineSlots; }
    unsigned numberOfSlots() const { return slots.isEmpty() ? names.size() : std::max<unsigned>(*std::ranges::max_element(slots) + 1, reserved); }

    static constexpr unsigned maxProperties = 1000; // See ImageDispatchEntry.
    static unsigned inlineCapacityFor(unsigned numberOfProperties);
};

// Identifies a function: its module (numbered by the embedder), its start offset in the module's source and its kind there
// (OrderFunctionKey), and which of its two code blocks.
struct ImageKey {
    uint32_t module { 0 };
    uint32_t start { 0 };
    uint32_t kind { 0 }; // OrderFunctionKind << 1 | isConstruct
    uint32_t record { 0 }; // Offset in the records, plus one. Zero marks an empty bucket.

    bool sameFunction(const ImageKey& other) const { return module == other.module && start == other.start && kind == other.kind; }
    unsigned hash() const
    {
        uint64_t h = (static_cast<uint64_t>(module) << 32 | start) * 0x9e3779b97f4a7c15ULL;
        h ^= (h >> 29) + kind * 0x85ebca6bU;
        h *= 0xbf58476d1ce4e5b9ULL;
        return static_cast<unsigned>(h >> 32);
    }
};
static_assert(sizeof(ImageKey) == 16);

// A stub call in a function's code. These are the only relocations needed when the code is placed.
struct StubCall {
    static constexpr uint32_t noFunction = std::numeric_limits<uint32_t>::max();
    static constexpr uint32_t noCallSite = std::numeric_limits<uint32_t>::max();

    uint32_t offset; // Offset of the call instruction in the function's code.
    Stub stub;
    bool isTailCall; // Emitted as a jump.
    uint16_t thunk { 0 }; // Index into StubBlob::thunkOffsets plus one, if the call goes through a thunk. Otherwise zero.
    // If set, this is a direct call to a function rather than to a stub: an index into the caller's
    // CompiledFunctionInfo::knownCallees. If the target ends up out of range, the call goes through a veneer.
    uint32_t function { noFunction };
    // The bytecode location (CallSiteIndex::bits()) to report while the callee runs. It is looked up by return address. noCallSite
    // if it is never queried.
    uint32_t callSite { noCallSite };
};

// An entry point into a stub that first loads a constant into T9. Few distinct constants are used and many call sites use them, so
// this saves an instruction per site. Returns nullopt if there is no such entry point, in which case the caller must set T9.
std::optional<unsigned> thunkFor(Stub, uint32_t valueOfT9);

// Entry points into a stub that accept its first operand in a register other than the default. Most stub operands are still live
// after the call, so they are in callee-saved registers and would otherwise need a move at each of hundreds of thousands of call
// sites.
// Returns whether the stub has such entry points. valueOfT9: for a stub that calls an operation, which operation. The entry point
// also sets T9, as with thunkFor().
bool acceptsOperandInAnyRegister(Stub, std::optional<uint32_t> valueOfT9);
// The register in which the stub itself takes the operand.
GPRReg defaultOperandRegister(Stub);
// Whether the operand may be in this register. The caller must keep it out of registers for which this returns false.
bool operandMayBeIn(Stub, GPRReg);
// Whether the entry point preserves defaultOperandRegister().
bool preservesOperandRegister(Stub);
unsigned thunkForOperandIn(Stub, std::optional<uint32_t> valueOfT9, GPRReg);
// The same for the first two operands, which the stub takes in the first two argument registers. They may be anywhere but T9-T15.
// If no entry point exists for the pair, they are moved.
bool acceptsTwoOperandsInAnyRegisters(Stub);
// The same for the result: it is delivered in a callee-saved register, where a long-lived result would be moved anyway. It cannot
// be delivered in a register the call clobbers.
bool returnsResultInAnyRegister(Stub, std::optional<uint32_t> valueOfT9);

struct StubBlob {
    Vector<uint8_t> bytes;
    unsigned offsets[numberOfStubs];
    Vector<unsigned> thunkOffsets; // Indexed by thunkFor().
    Vector<unsigned> returnsIntoAdapters; // Return addresses of the calls that adapters make. See ImageAddressInfo::Adapter.
    Vector<std::pair<String, unsigned>> names; // What is at which offset, the last being the end. Only for a log or a map.
};
const StubBlob& stubBlob();

// A place in a function's code that depends on the function's index. The index is not known until the image is assembled, and is
// then patched into the instructions.
struct IndexReference {
    uint32_t offset; // Offset of the first of two instructions.
    uint32_t addend;
    uint16_t scale;
};
class IndexReferences {
public:
    // dest = the word at base + addend + index * scale
    void load(CCallHelpers&, GPRReg base, GPRReg dest, uint32_t addend, uint32_t scale);
    Vector<IndexReference> link(LinkBuffer&);
    static void fill(uint8_t* code, const IndexReference&, uint32_t index);

private:
    struct Reference {
        CCallHelpers::Label instructions;
        uint32_t addend;
        uint32_t scale;
    };
    Vector<Reference, 2> m_references;
};

// The value Stub::CallVarargs takes in argumentGPR(1). Either argumentGPR(0) is an array-like object as in op_call_varargs and this
// is the number of leading elements to skip, or argumentGPR(0) points to consecutive items in the caller's memory and this gives
// the kind of each.
struct ListDescriptor {
    enum Kind : uint32_t {
        Value, // One argument.
        Spread, // An iterable, which expands to as many arguments as it yields.
        Passed, // Two words: a count and an address.
    };
    static constexpr unsigned mostItems = 13;
    static constexpr uint32_t ofList(uint32_t firstVarArg) { return firstVarArg << 1; }
    static constexpr uint32_t ofItems(uint32_t count) { return 1 | count << 1; }
    static constexpr uint32_t kindOfItem(unsigned index, Kind kind) { return static_cast<uint32_t>(kind) << (5 + 2 * index); }
    static_assert(5 + 2 * mostItems <= 32 && mostItems < 16);

    bool isOfItems() const { return bits & 1; }
    uint32_t firstVarArg() const { return bits >> 1; }
    unsigned numberOfItems() const { return bits >> 1 & 15; }
    Kind kindOf(unsigned index) const { return static_cast<Kind>(bits >> (5 + 2 * index) & 3); }

    uint32_t bits;
};
static constexpr unsigned mostItemsInList = ListDescriptor::mostItems;

// In a function with inlined callees (Graph::adoptInlinee()), a call site records which inline frame it is in, above its location in the
// inlined callee's bytecode.
struct PackedSite {
    static constexpr unsigned shift = 24;
    static constexpr unsigned mostInlineFrames = 254; // StubCall::noCallSite must not be a valid encoding.
    // A tail call in an inlined callee whose caller continues afterwards. It is emitted as an ordinary call, but once the callee
    // runs, the frame that made the call must appear to be gone.
    static constexpr uint32_t isTailCall = 1u << (shift - 1);
    static bool fits(uint32_t bits) { return !(bits >> (shift - 1)); }
    static uint32_t pack(unsigned inlineFrame, uint32_t bits, bool tailCall = false)
    {
        RELEASE_ASSERT(inlineFrame <= mostInlineFrames && fits(bits));
        return inlineFrame << shift | (tailCall ? isTailCall : 0) | bits;
    }
    static unsigned inlineFrame(uint32_t site) { return site >> shift; }
    static uint32_t bits(uint32_t site) { return site & (isTailCall - 1); }
    static bool isOfTailCall(uint32_t site) { return site & isTailCall; }
};

// A call that takes items (ListDescriptor) iterates all of its spreads at one call site, as far as its frame shows. This gives the
// bytecode location to report while each item is being iterated.
struct SiteOfSpread {
    uint32_t callSite;
    uint32_t item;
    uint32_t site;
};

struct CallSite {
    uint32_t bits { StubCall::noCallSite };
};

#if CPU(ARM64)
// For stubs: loads into indexOfFunctionGPR the index of the image function that contains `pc`. Clobbers x14 and the assembler's
// scratch registers.
static constexpr GPRReg indexOfFunctionGPR = ARM64Registers::x15;
void loadIndexOfFunctionAt(CCallHelpers&, GPRReg pc);
#endif

// The stub calls of one compilation.
class StubCalls {
public:
    void call(CCallHelpers&, Stub, CallSite);
    void call(CCallHelpers&, Stub, uint32_t valueOfT9, CallSite);
    void callWithOperandIn(CCallHelpers&, Stub, std::optional<uint32_t> valueOfT9, GPRReg, CallSite); // Requires acceptsOperandInAnyRegister().
    void callWithOperandsIn(CCallHelpers&, Stub, GPRReg first, GPRReg second, CallSite); // Requires acceptsTwoOperandsInAnyRegisters().
    void callForResultIn(CCallHelpers&, Stub, uint32_t valueOfT9, GPRReg operand, GPRReg result, CallSite); // Requires returnsResultInAnyRegister(). `operand` must be the default register unless acceptsOperandInAnyRegister().
    void tailCall(CCallHelpers&, Stub);
    void tailCall(CCallHelpers&, Stub, uint32_t valueOfT9);
    void callFunction(CCallHelpers&, uint32_t knownCallee, CallSite);
    void jumpToFunction(CCallHelpers&, uint32_t knownCallee);
    // Returns the locations of the calls, for ImageBuilder to retarget.
    Vector<StubCall> link(LinkBuffer&);

private:
    struct Pending {
        CCallHelpers::Call call;
        Stub stub;
        bool isTailCall;
        uint32_t callSite;
        uint32_t function { StubCall::noFunction };
        uint16_t thunk { 0 };
    };
    Vector<Pending, 8> m_pending;
};

// Points the call instruction at `instruction` to `target`, both offsets from the same base.
void retargetStubCall(uint8_t* base, size_t instruction, size_t target, bool isTailCall);
static constexpr size_t reachOfStubCall = 96 * MB; // Less than the range of a branch instruction, to leave slack.
// A veneer: a jump to anywhere in the image, for calls whose target is out of range.
static constexpr size_t sizeOfVeneer = 3 * sizeof(uint32_t);
static constexpr size_t roomToSpareInReachOfStubCall = 24 * MB; // A target considered in range stays in range if it ends up this much further away.
void writeVeneer(uint8_t* base, size_t veneer, size_t target); // Offsets from the same base, which must be page-aligned.

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
