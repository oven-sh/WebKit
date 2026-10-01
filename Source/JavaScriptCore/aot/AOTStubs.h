/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(FTL_JIT)

#include "AOTConvention.h"
#include "CCallHelpers.h"
#include <wtf/Vector.h>

namespace JSC {

class LinkBuffer;

namespace AOT {

// What every function would otherwise have a copy of. A program that is compiled whole has a few hundred thousand functions and
// a few million property accesses, and what each of them does that is the same is done here, once: the function has a call
// instruction. Like the functions, the stubs have no address in them, and they go in the image with the functions, within reach
// of a call instruction (ImageBuilder puts in as many copies as that takes).
//
// They have conventions of their own. A0.. are the argument registers, T9.. the temporaries of those names. None of them is passed
// anything in the macro assembler's scratch registers, which they all clobber. Unless it says otherwise a stub clobbers what a
// C function may. A stub that can throw does not come back if something is thrown: it goes to the handler.
//
// "site" is the address of a Slot of the function, which has a Site that goes with it.
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

// A call of something that was found under one of these names, with as many arguments as the function of that name takes. If the
// callee turns out to be that function, and `this` and the arguments what it has a quick way with, a stub does what the function
// would have done. Anything else is called as anything is. For a name that more than one kind of object has a function of, which
// of them it is goes by what `this` is.
// The last says which calls it is for: all, those whose result is wanted, or those whose result is not.
#define FOR_EACH_AOT_STUB_INTRINSIC(v) \
    v(CharCodeAt, "charCodeAt", 1, Any) \
    v(CodePointAt, "codePointAt", 1, Any) \
    v(CharAt, "charAt", 1, Any) \
    v(Push, "push", 1, Any) \
    v(Pop, "pop", 0, Any) \
    v(IsArray, "isArray", 1, Any) \
    /* Of a Map, or a Set. What it takes to put something in that is not there is more than a stub does, and it is the */ \
    /* function's unless nobody wants what the function returns. */ \
    v(Get, "get", 1, Any) \
    v(Has, "has", 1, Any) \
    v(Set, "set", 2, Wanted) \
    v(SetAndForget, "set", 2, NotWanted) \
    v(Add, "add", 1, Wanted) \
    v(AddAndForget, "add", 1, NotWanted) \

enum class StubIntrinsic : uint8_t {
    None,
#define AOT_DEFINE_STUB_INTRINSIC(name, text, argumentCount, result) name,
    FOR_EACH_AOT_STUB_INTRINSIC(AOT_DEFINE_STUB_INTRINSIC)
#undef AOT_DEFINE_STUB_INTRINSIC
    NumberOfStubIntrinsics
};
static constexpr unsigned numberOfStubIntrinsics = static_cast<unsigned>(StubIntrinsic::NumberOfStubIntrinsics) - 1;
StubIntrinsic stubIntrinsicFor(UniquedStringImpl* name, unsigned argumentCountIncludingThis, bool resultIsWanted);

// What an adapter (see adapt()) saves, from its frame pointer.
static constexpr ptrdiff_t offsetOfInstanceRegisterInAdapter = -24; // (In the order a RegisterAtOffsetList has them in.)
static constexpr ptrdiff_t offsetOfNumberTagRegisterInAdapter = -16;
static constexpr ptrdiff_t offsetOfNotCellMaskRegisterInAdapter = -8;
static constexpr ptrdiff_t offsetOfInstanceInAdapter = -32; // And there: the Instance of the code that it lets in.
static constexpr unsigned sizeOfWhatAdapterSaves = 32;

// Instance::granulesOfCode has an entry for each so many bytes of an image's code.
static constexpr unsigned shiftOfGranuleOfCode = 10;

// The operations that have a helper ahead of them, as others have a front end (AOTThunks.h): what calls the operation gets what the helper makes, if it makes anything. So code
// that is not worth its size calls the one thing.
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
    /* After the frame pointer is set up. T9 = the size of the frame. Checks that there is stack for it and sets the stack */ \
    /* pointer. Clobbers only T11, T12. */ \
    v(Prologue) \
    /* Calls whatever it is given. calleeGPR = that, thisGPR = this (Construct: new.target), countGPR = how many arguments, which */ \
    /* are where a function with Signature::Registers has its parameters. Result in the return value register. Jumped to, */ \
    /* with nothing of the caller's on the stack, it is a tail call. (These come first: other stubs end in them.) */ \
    v(Call) \
    v(Construct) \
    /* The same, of arguments that are in memory, as for a function with Signature::List: argumentGPR(0) = how many, */ \
    /* argumentGPR(1) = where. They stay there until this comes back. */ \
    v(CallList) \
    v(ConstructList) \
    /* The same, of what is in a list of some kind: argumentGPR(0) = the list, as op_call_varargs has it, argumentGPR(1) = how many */ \
    /* of the first to leave out. */ \
    v(CallVarargs) \
    v(ConstructVarargs) \
    /* The same as CallVarargs, in tail position. It is called, by a function that has put back the registers it saved and still has */ \
    /* its frame, and does not come back: what the callee returns goes to whoever called that function, whose frame is gone by then. */ \
    v(TailCallVarargs) \
    /* A call, as Call is, of what may be the function that a StubIntrinsic is for: T9 = which. There is one for each, and */ \
    /* no other way of getting here. */ \
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
    /* Called by a function that has found that it has nothing of the realm yet, and needs it. Every register is left as it was. */ \
    v(LinkFunction) \
    /* Calls to C++. T9 = Entry * 8, the arguments where C++ wants them. The Plain ones are for operations that do not throw and */ \
    /* do not look at the stack. In the ones so field the first argument is the global object, or the VM, and the stub */ \
    /* supplies it. */ \
    v(OperationValue) \
    v(OperationVoid) \
    v(OperationDouble) \
    v(OperationValueWithGlobalObject) \
    v(OperationVoidWithGlobalObject) \
    v(OperationDoubleWithGlobalObject) \
    /* Likewise (the global object, then A1 and A2; nothing comes back), for what is hardly ever called: all registers but T9, T10 and the */ \
    /* assembler's own are as they were. OfLeaf: the caller has no frame, and T10 = where it is to return to. It has one for the while. */ \
    v(ColdOperationVoid) \
    v(ColdOperationVoidOfLeaf) \
    /* Likewise, and what comes back is where a value is returned. */ \
    v(ColdOperationValue) \
    v(ColdOperationValueOfLeaf) \
    v(PlainOperation) \
    v(PlainOperationWithGlobalObject) \
    v(PlainOperationWithVM) \
    /* A0 = the cell that was stored to. Clobbers only T9-T11. */ \
    v(WriteBarrier) \
    /* A0 = a value. Leaves whether it is truthy, 0 or 1, in A0. Clobbers only that and T9-T11. */ \
    v(ToBoolean) \
    /* A0, A1 = two values. Leaves whether they are equal, 0 or 1, in A0. */ \
    v(StrictEqual) \
    v(LooseEqual) \
    /* A0, A1 = the operands. The result, a JSValue, in A0. */ \
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
    /* A0 = base, A1 = an integer, as it is (Rep::Int64). Result in A0. What it has no quick way with goes on into the next. */ \
    v(GetByValAtIndex) \
    /* A0 = base, A1 = property. Result in A0. */ \
    v(GetByVal) \
    /* As the next, but for A1, which is an integer as it is. Goes on into it. */ \
    v(PutByValAtIndex) \
    /* A0 = base, A1 = property, A2 = value, A3 = whether the code is strict. */ \
    v(PutByVal) \
    /* Likewise, as op_put_by_val_direct has them: the base is an object. */ \
    v(PutByValDirect) \
    /* A0 = base, A1 = site, whose identifier is WellKnownIdentifier::Length. Result in A0. */ \
    v(GetLength) \
    /* op_resolve_scope and the op_get_from_scope it is for, in one. A0 = scope, A1 = the site of the first; the second's is */ \
    /* the next. Result in A0. */ \
    v(GetGlobal) \
    /* A0 = base, A1 = site. Result in A0. */ \
    v(GetById) \
    /* A0 = base, A1 = the id of a field whose slot is that one (TypedLayoutTable::Field::id). Result in A0: what the field fieldType, or it does not come back. */ \
    v(ReadSlot0) \
    v(ReadSlot1) \
    v(ReadSlot2) \
    v(ReadSlot3) \
    v(ReadSlot4) \
    v(ReadSlot5) \
    v(ReadSlot6) \
    v(ReadSlot7) \
    /* Likewise, but undefined will do, of an object that has no such property. */ \
    v(ReadSlotOrUndefined0) \
    v(ReadSlotOrUndefined1) \
    v(ReadSlotOrUndefined2) \
    v(ReadSlotOrUndefined3) \
    v(ReadSlotOrUndefined4) \
    v(ReadSlotOrUndefined5) \
    v(ReadSlotOrUndefined6) \
    v(ReadSlotOrUndefined7) \
    /* A0 = base, A1 = value, A2 = site. */ \
    v(PutById) \
    /* A0 = base, A1 = the name, or the brand, A2 = site. GetPrivateName leaves the result in A0. */ \
    v(GetPrivateName) \
    v(CheckPrivateBrand) \
    /* A0 = base, A1 = the name, A2 = value, A3 = site, whose extra checkStore whether this defines the field. */ \
    v(PutPrivateName) \
    /* A0 = scope, A1 = site. Result in A0. */ \
    v(ResolveScope) \
    v(GetFromScope) \
    /* A0 = scope, A1 = value, A2 = site. */ \
    v(PutToScope) \
    /* As GetById, for a site whose identifier is a WellKnownIdentifier. */ \
    v(GetByIdWellKnown) \
    /* A0 = an object, A1 = a value. Whether A1 is on A0's prototype chain, 0 or 1, in A0. */ \
    v(InstanceOf) \
    /* A0 = next, A1 = iterator, A2 = iterable, as op_iterator_next has them; A3 = two slots, one after the other, for done and value. */ \
    /* Comes back with A0 = done, A1 = value, A2 = next. */ \
    v(IteratorNext) \
    /* A0 = iterable, A1 = what its @@iterator is, as op_iterator_open has them; A2 = a slot, for next. Comes back with A0 = iterator, A1 = next. */ \
    v(IteratorOpen) \
    /* A0 = iterator, A1 = iterable, A2 = next, as op_iterator_close_check has them. Comes back with A0 = the iterator to close: see lowerIteratorCloseCheck(). */ \
    v(IteratorCloseCheck) \
    /* What follows is what the JIT has thunks for, which there may not be a JIT to make. */ \
    /* Jumped to, in the frame of a function of the static compiler's in which, or in something called by which, an exception was */ \
    /* thrown, with the link register saying where it is at. Finds the handler and goes there. */ \
    v(HandleException) \
    /* Jumped to from Prologue, in the frame there is no room for. */ \
    v(ThrowStackOverflowAtPrologue) \
    /* Where the unwinder sends control for a handler in code from the static compiler (genericUnwind()), from any kind of frame. */ \
    /* Puts the callee saves and the frame pointer back and goes to VM::targetMachinePCAfterCatch. */ \
    v(Catch) \
    /* As the JIT's virtual call thunks: called, or for a tail call jumped to, with the frame made but for its CodeBlock. */ \
    /* T0 = callee, T2 = the CallLinkInfo of a VirtualCallInfo. */ \
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

// What there is to know about a place in the code that has a Slot, and that only matters when the slot is of no help. It does not
// change, so it is not in the slot, and it is not in the code, where it would take instructions to say.
struct Site {
    static constexpr unsigned identifierBits = 20;
    static constexpr unsigned extraBits = 32 - identifierBits;
    static bool fits(unsigned identifier, unsigned extra) { return identifier < (1u << identifierBits) && extra < (1u << extraBits); }

    // op_resolve_scope: how many scopes the function itself has made by then, or that only the global scopes need looking in.
    static constexpr unsigned resolvesInGlobalScopes = (1u << extraBits) - 1;
    // op_get_from_scope.
    static constexpr unsigned throwsIfNotFound = 1;
    static constexpr unsigned isImport = 2; // The scope is the environment of a module, and the name one of its imports.

    uint32_t identifierAndExtra { 0 }; // The index of an identifier of the function, and above it whatever else the operation is told.
};

// What an object is made with, where all of that is plain from the code that makes it: the names of its properties, in the order
// they are added in. All objects made with the same are of one Structure, whose layout is known when the program is compiled.
struct KnownShape {
    unsigned inlineCapacity { 0 }; // Zero: whatever it is, it is enough for all of them.
    Vector<UniquedStringImpl*, 8> names;
    // If it is one of the layouts of the table of types (TypeTable::Layout): which, and the slot that each property is in. It is known
    // by that number. If not, the properties are one after the other, and it is given a number.
    uint32_t number { 0 };
    Vector<uint16_t, 8> slots;
    // If the layouts are of structs (TypeTable::tableHasTypedFields()): the family, and how many slots every object of it has.
    uint16_t layoutID { 0 };
    uint16_t reserved { 0 };
    uint16_t inlineSlots { 0 }; // Slots from that one on are outside the object.
    bool hasSlotsOutside() const { return layoutID && reserved > inlineSlots; }
    unsigned numberOfSlots() const { return slots.isEmpty() ? names.size() : std::max<unsigned>(*std::ranges::max_element(slots) + 1, reserved); }

    static constexpr unsigned maxProperties = 1000; // See ImageDispatchEntry.
    static unsigned inlineCapacityFor(unsigned numberOfProperties);
};

// Which function: the module it is in (whatever the embedder numbers its modules by), where it starts in the module's source
// and what it is there (OrderFunctionKey), and which of its two code blocks.
struct ImageKey {
    uint32_t module { 0 };
    uint32_t start { 0 };
    uint32_t kind { 0 }; // OrderFunctionKind << 1 | isConstruct
    uint32_t record { 0 }; // Offset in the records, plus one. Zero: an empty bucket.

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

// Where a function calls a stub: all that has to be filled in when the function's code is put somewhere.
struct StubCall {
    static constexpr uint32_t noFunction = std::numeric_limits<uint32_t>::max();
    static constexpr uint32_t noCallSite = std::numeric_limits<uint32_t>::max();

    uint32_t offset; // Of the call instruction, in the function's code.
    Stub stub;
    bool isTailCall; // A jump.
    uint16_t thunk { 0 }; // One more than which of StubBlob::thunkOffsets it is by way of. Zero: none.
    // Not to a stub at all, but to a function: which of the caller's known callees (CompiledFunctionInfo::knownCallees). If it ends up
    // out of reach, it is got to in two steps.
    uint32_t function { noFunction };
    // Where in the bytecode the function is while what is called runs (CallSiteIndex::bits()): what is going to be returned to is all that
    // says so. None: nobody is going to ask.
    uint32_t callSite { noCallSite };
};

// A way into a stub that puts a number in T9 first. There are few enough numbers that a stub is given there, and enough places that
// give them, for it to be worth an instruction at each. None: there is no such way in, and the number is for the caller to put there.
std::optional<unsigned> thunkFor(Stub, uint32_t valueOfT9);

// A way into a stub for the first thing it is given being in some other register than the one the stub takes it in. Nearly everything that
// is given to a stub is wanted afterwards as well, so it is in a register that calls leave alone, and would have to be copied: at every one
// of hundreds of thousands of places.
// Whether there are such ways in. (valueOfT9: of a stub that calls an operation, which. It is put there on the way, as by thunkFor().)
bool takesOperandAnywhere(Stub, std::optional<uint32_t> valueOfT9);
// Where the stub itself takes it.
GPRReg whereOperandIsTaken(Stub);
// Whether it may be there. What may not hold it is for whoever calls the stub to keep it out of.
bool operandMayBeIn(Stub, GPRReg);
// Whether the way in leaves whereOperandIsTaken() as it was.
bool leavesAloneWhereOperandIsTaken(Stub);
unsigned thunkForOperandIn(Stub, std::optional<uint32_t> valueOfT9, GPRReg);
// Likewise the first two things it is given, which it takes in the first two argument registers. (Not in T9 to T15. Anywhere else will do: if there is no way in for it, they are moved.)
bool takesTwoOperandsAnywhere(Stub);
// And what it hands back: in a register that calls leave alone, which is where it would be copied to if it is wanted for long. (It cannot be had in one that the call clobbers.)
bool givesResultAnywhere(Stub, std::optional<uint32_t> valueOfT9);

struct StubBlob {
    Vector<uint8_t> bytes;
    unsigned offsets[numberOfStubs];
    Vector<unsigned> thunkOffsets; // By thunkFor().
    Vector<unsigned> returnsIntoAdapters; // Where what an adapter calls comes back to: see WhatIsAt::Adapter.
    void* inJITMemory; // A copy that code in the JIT's memory can call.
};
const StubBlob& stubBlob();

// What in a function's code goes by which function it is: its index is not known until the image is put together, and is then written
// into the instructions.
struct IndexReference {
    uint32_t offset; // Of the first of two instructions.
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

// What Stub::CallVarargs is told in argumentGPR(1). Either argumentGPR(0) is a list as op_call_varargs has it, and this is how many of the
// first to leave out; or it is where a number of items are, in memory of the caller's, one after the other, and this says what each is.
struct ListDescriptor {
    enum Kind : uint32_t {
        Value, // One argument.
        Spread, // Something to iterate over: as many arguments as that comes to.
        Passed, // Two words: how many, and where they are.
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

// In a function that others have been made part of (Graph::adopt()), a call site says which of those calls it is in, above where it is in
// the bytecode of the function that was called.
struct PackedSite {
    static constexpr unsigned shift = 24;
    static constexpr unsigned mostInlineFrames = 254; // (StubCall::noCallSite is not one.)
    // A tail call, in a function that has become part of one that goes on afterwards. It is made like any other call; but once what is
    // called runs, whoever made it is gone, as far as anybody can tell.
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

// A call that is passed items (ListDescriptor) is where all of them are gone through, as far as its frame says. This says where in the
// bytecode each would have been, for whoever asks while that one is being gone through.
struct SiteOfSpread {
    uint32_t callSite;
    uint32_t item;
    uint32_t site;
};

struct CallSite {
    uint32_t bits { StubCall::noCallSite };
};

#if CPU(ARM64)
// For stubs and the like: which function of the image it is that has `pc` in it, in indexOfFunctionGPR. Clobbers x14 and the assembler's own.
static constexpr GPRReg indexOfFunctionGPR = ARM64Registers::x15;
void loadIndexOfFunctionAt(CCallHelpers&, GPRReg pc);
#endif

// The calls of one compilation.
class StubCalls {
public:
    void call(CCallHelpers&, Stub, CallSite);
    void call(CCallHelpers&, Stub, uint32_t valueOfT9, CallSite);
    void callWithOperandIn(CCallHelpers&, Stub, std::optional<uint32_t> valueOfT9, GPRReg, CallSite); // takesOperandAnywhere()
    void callWithOperandsIn(CCallHelpers&, Stub, GPRReg first, GPRReg second, CallSite); // takesTwoOperandsAnywhere()
    void callForResultIn(CCallHelpers&, Stub, uint32_t valueOfT9, GPRReg operand, GPRReg result, CallSite); // givesResultAnywhere(). operand: where the first is, which is where it is taken unless takesOperandAnywhere().
    void tailCall(CCallHelpers&, Stub);
    void tailCall(CCallHelpers&, Stub, uint32_t valueOfT9);
    void callFunction(CCallHelpers&, uint32_t knownCallee, CallSite);
    void jumpToFunction(CCallHelpers&, uint32_t knownCallee);
    // Links them to the copy in the JIT's memory, and says where they are.
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
static constexpr size_t reachOfStubCall = 96 * MB; // With room to spare.
// A jump to anywhere in the image, for a call to go by way of.
static constexpr size_t sizeOfVeneer = 3 * sizeof(uint32_t);
static constexpr size_t roomToSpareInReachOfStubCall = 24 * MB; // What is within reach is still within reach if it ends up this much further.
void writeVeneer(uint8_t* base, size_t veneer, size_t target); // Offsets from the same base, which is the start of a page.

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
