/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(FTL_JIT)

#include "AOTProgram.h"
#include "AOTStubs.h"
#include "AOTType.h"
#include "AOTTypeTable.h"
#include "BytecodeIndex.h"
#include "BytecodeStructs.h"
#include "CallFrame.h"
#include "Instruction.h"
#include "LinkTimeConstant.h"
#include "GetPutInfo.h"
#include "Opcode.h"
#include "ScopeOffset.h"
#include "VirtualRegister.h"
#include <wtf/BitVector.h>
#include <wtf/SegmentedVector.h>
#include <wtf/Vector.h>

namespace JSC {

class UnlinkedCodeBlock;

namespace B3 {
class Value;
class BasicBlock;
namespace Air {
class StackSlot;
}
}

namespace AOT {

struct BasicBlock;
struct Node;
class CalleeHints;
struct KnownFunction;

// The static compiler's IR: the bytecode of one function in SSA form, with a proven Type on every value.
//
// A node is, for the most part, a bytecode instruction: its immediates are read from the instruction, and its inputs are
// found by the virtual register the instruction names them by (Node::use()), so no opcode needs code of its own to get into
// the IR (BytecodeUseDef says what every instruction reads and writes). What an opcode does need is a type rule
// (AOTTypeInference.cpp; the default is "any value") and a lowering (AOTLowerToB3.cpp; a function that has an instruction
// without one is left to the other tiers).
//
// There is no speculation. A lowering picks its code by the types of the inputs, and where they say nothing it emits the
// whole of the operation's semantics, out of line where that is long.
enum class NodeKind : uint8_t {
    Bytecode, // An instruction. Defines at most one register, or several through Proj nodes.
    Constant, // A value known now: anything that is not a cell.
    ConstantCell, // A constant register that holds a cell: loaded from the CodeBlock.
    Intrinsic, // One of the realm's ImmutableIntrinsics that is a cell: loaded from the Instance.
    LinkTimeConstant, // What the realm has for one of the things only the engine's own functions can name (`intrinsic` says which). It is made when it is first wanted, so this is where it is read.
    Argument, // The value an argument register has on entry.
    Phi,
    Proj, // One of the registers defined by an instruction that defines several. uses[0] is the instruction.
    GetStack, // Reads a register that lives in memory (see Graph::isHomed).
    SetStack, // Writes one. uses[0] is the value.
    Guard, // Ends a block of a loop's fast copy (see BasicBlock::isGeneric). Reads what the instruction that comes next reads.
    Narrow, // uses[0], on its way back into the fast copy of a loop, known to be what `target` is (see BasicBlock::isReentry).
};

// A call that there is a short way of making if the callee turns out to be the host function that the name it was found under
// suggests. The name is a reason to have the code for it, and no more.
enum class CallIntrinsic : uint8_t { None, MathSqrt, MathAbs, MathFloor, MathCeil, MathTrunc, MathFround, MathMin, MathMax, MathIMul, StringCharCodeAt, ArrayPush };
inline bool hasNoEffects(CallIntrinsic intrinsic) { return intrinsic != CallIntrinsic::ArrayPush; }
CallIntrinsic callIntrinsicFor(UniquedStringImpl* propertyName, unsigned argumentCountIncludingThis);

// What a guard sees to. All but the first two are made by optimizeLoops().
enum class GuardKind : uint8_t {
    Whole, // The instruction.
    Nothing, // It lets everything by.
    Reentry, // That the values that the block's Narrow nodes are of are what those say.
    Structure, // That the base (uses[0]) is of the structure that the cache of `site` is for.
    SlotsAgree, // That the caches of `site` and `otherSite` are for the same structure.
    SlotIsPlain, // That the cache of `site` is for a property in the base itself (not Slot::isIntricate).
    // Around guards of those two kinds, which have nothing to go by but caches: skipped if none has changed since they last held.
    BeginSlotChecks,
    EndSlotChecks,
    Callee, // That the callee is what the call takes it for.
    KnownCallee, // That the callee (uses[0]) is a closure of the function that the call was compiled for (Graph::knownCallee()).
    TypedArrayStorage, // That the typed array (uses[0]) is of a fixed length. It is where the length and the storage are loaded.
    IsIntrinsic, // That uses[0] is the one of the realm's ImmutableIntrinsics that Node::intrinsic says. Made by inlineCalls().
    // Options::aotAssertsTypes(). Of an op_get_by_id or an op_put_by_id whose base has a type (TypeTable): that the base was born with the property in
    // Node::slotOfField (as one of firstLayout to lastLayout), and that it is still there. It does what the instruction does. After
    // it the base is known for what it was born as (a Narrow with narrowedTo), so the next one has that much less to see to.
    Field,
};

// What a constructor does first, as a rule, is give the new object its properties:
//     constructor(a, b) { this.a = a; this.b = b; }
// Until something other than a store gets hold of the object, nobody can tell in what state it is or, indeed, whether it is there. So
// the object is made after the last of those stores, with everything in it.
//
// This is worked out from the bytecode alone, by the compiler and again by the operation that the compiled code falls back to.
struct NewObjectPlan {
    struct Property {
        unsigned identifier;
        bool isDefined; // By a store that makes a property, whatever the prototypes have to say. If not, it is up to them.
        bool isStrict; // Whether it is an error if they say no.
    };
    Vector<Property, 8> properties; // In the order they are added in.
    struct Store {
        unsigned offset; // Of the op_put_by_id.
        unsigned property; // Which of the above.
    };
    Vector<Store, 8> stores;

    // What stands for the register of a property's value, among the uses of the node.
    static VirtualRegister registerOf(unsigned property) { return VirtualRegister(0x20000000 + static_cast<int>(property)); }

    static NewObjectPlan forCreateThis(const JSInstructionStream&, unsigned offsetOfCreateThis);
};

class Graph;
struct BasicBlock;
struct Node;

// What becomes of something that the function makes: whether anything can still get at it once the function has returned, and if so
// the first thing that was seen to make that so (analyzeEscapes()).
enum class Escape : uint8_t {
    NotLookedAt,
    StaysHere, // Nothing sees it but the code of this function (and what has been made part of that).
    IsOnlyLent, // It is passed to functions that are known, and known to be done with it when they return.
    Returned,
    Thrown,
    StoredInProperty,
    StoredInLiteral,
    StoredInVariable,
    Merged,
    Homed,
    PassedToMethod,
    PassedAsThisToMethod,
    PassedToBuiltin,
    PassedToClosureMadeHere,
    PassedToParameter,
    PassedToVariable,
    PassedToUnknown,
    PassedToKnownThatKeepsIt,
    PassedInList,
    PassedInTailCall,
    Constructed,
    HeldByWhatEscapes,
    ClosureLetsScopeOut,
    Iterated,
    Converted,
    Suspended,
    Other,
    NumberOfThem,
};
inline bool stays(Escape escape) { return escape == Escape::StaysHere || escape == Escape::IsOnlyLent; }
ASCIILiteral nameOf(Escape);
enum class AllocationKind : uint8_t { Object, Array, Closure, Environment };
static constexpr unsigned numberOfAllocationKinds = 4;
inline ASCIILiteral nameOf(AllocationKind kind)
{
    static constexpr ASCIILiteral names[] = { "object"_s, "array"_s, "closure"_s, "environment"_s };
    return names[static_cast<unsigned>(kind)];
}
std::optional<AllocationKind> kindOfAllocation(const Node*);
unsigned bytesOfAllocation(const Node*);

struct Use {
    VirtualRegister reg;
    Node* node { nullptr };
};

// EXPERIMENT: Options::aotFacts(). See ~/code/tmp/aot/tsfacts/tofacts.ts.
enum : unsigned { FactField = 1, FactDirect, FactBuiltin, FactBody, FactArray, FactElement, FactWrite };
inline bool isFact(unsigned mask) { return mask >= 1u << 28; }
inline Type typeHeldByFact(unsigned holds)
{
    switch (holds) {
    case 2:
        return TNumber;
    case 3:
        return TBoolean;
    case 4:
        return TString;
    case 5:
        return TFinalObject;
    case 6:
        return TArray;
    case 7:
        return TFunction;
    case 8:
        return TObject | TTypedArray;
    default:
        return TTop;
    }
}

struct Node {
    // Whose code it is of: the function that is being compiled, or one that has been made part of it (inlineCalls()).
    Graph* graph { nullptr };
    NodeKind kind { NodeKind::Bytecode };
    OpcodeID opcode { op_nop };
    Type type { TNone };
    IntegerRange range;
    unsigned rangeUpdates { 0 };
    unsigned index { 0 };
    BytecodeIndex bytecodeIndex;
    const JSInstruction* instruction { nullptr };
    BasicBlock* block { nullptr };
    Vector<Use, 4> uses;
    JSValue constant; // Constant
    VirtualRegister reg; // ConstantCell, Argument, Proj, GetStack, SetStack, Phi (the register it merges)
    Node* replacement { nullptr }; // Set while phis are simplified.
    Node* guard { nullptr }; // An instruction that is only got to when this has found that there is a short way to do it.
    Node* guarded { nullptr }; // The other way round.
    Node* target { nullptr }; // NodeKind::Narrow: the phi of the loop's header that the value is on its way to.
    Type narrowedTo { TNone }; // NodeKind::Narrow, if there is no target: what a GuardKind::Field found the value to be.
    uint16_t slotOfField { 0 }; // GuardKind::Field
    uint16_t firstLayout { 0 }; // Likewise, and the Narrow that comes after it.
    uint16_t lastLayout { 0 };
    // GuardKind::Field, of a read: layouts that have no such property, of which it is undefined. 0, 0: none.
    uint16_t firstWithout { 0 };
    uint16_t lastWithout { 0 };
    // GuardKind::Field, with Options::aotTypesFields(): what the slot holds (TypeTable::Holds). No kinds: anything.
    uint16_t heldKinds { 0 };
    uint16_t heldFirst { 0 };
    uint16_t heldLast { 0 };
    // Whether the value is certain to have been born as one of those: by its type, or because it is what got past a test for no more than those.
    bool isKnownToBeBornWithin(uint16_t first, uint16_t last) const
    {
        for (const Node* node = this; node->kind == NodeKind::Narrow && node->narrowedTo; node = node->uses[0].node) {
            if (node->firstLayout >= first && node->lastLayout <= last)
                return true;
        }
        // (Of a family that is open it says no such thing: Lowering::structToLookIn().)
        if (isBytecode(op_type_tag) && firstLayout >= first && lastLayout <= last && !Options::aotAuditsTypes() && isTakenAtItsWord)
            return true;
        return type && isSubtype(type, TCell) && isBornWithinIfCell(first, last);
    }
    // Likewise, if it is a cell at all: it may be undefined, or null, or a number.
    bool isBornWithinIfCell(uint16_t first, uint16_t last) const
    {
        if (Type cells = type & TCell; cells && isSubtype(cells, TFinalObject)) {
            auto layouts = layoutsBornAs(cells);
            if (layouts.lowest >= first && layouts.highest <= last)
                return true;
        }
        // What was read from a slot says exactly. (A type says what the numbers have in common.)
        const Node* node = this;
        while (node->kind == NodeKind::Narrow || node->isBytecode(op_check_type) || node->isBytecode(op_check_tdz))
            node = node->uses[0].node;
        if (node->kind != NodeKind::Bytecode || !node->guard || node->guard->guardKind != GuardKind::Field || !node->guard->heldFirst)
            return false;
        constexpr unsigned notCells = MaskUndefined | MaskNull | MaskBoolean | MaskNumber;
        return !(node->guard->heldKinds & ~(notCells | MaskOtherObject)) && node->guard->heldFirst >= first && node->guard->heldLast <= last;
    }
    unsigned expectedMask { 0 }; // op_get_by_val: the mask of the op_check_type that what it gets goes to next, if it does.
    GuardKind guardKind { GuardKind::Whole };
    uint16_t intrinsic { 0 }; // NodeKind::Intrinsic: its number.
    // A call: the function of the language that it is a call of, by its number (ImmutableIntrinsics), if what it is made on is a Receiver of that kind. (None: whatever it is made on.)
    // The read of what is called: likewise. See Graph::findBuiltinsCalled().
    uint16_t builtinCalled { 0 };
    uint8_t receiverOfBuiltin { 0 };
    bool structureIsChecked { false }; // A guard of a property access: another guard has seen to the base's structure.
    bool slotIsPlain { false }; // Likewise: another guard has seen to that.
    bool calleeIsChecked { false };
    bool wasTakenNeverToBeReached { false }; // inferTypes(): nothing was found that it could give. For Options::aotVerifiesFacts().
    bool isElided { false }; // Nothing wants its value, and getting that does nothing else. It is not lowered.
    // An op_get_by_id of a closed method whose value nothing wants: all that is left of it is that it throws if there is no object to read from.
    bool isReadOnlyToBeCalled { false };
    bool isTakenAtItsWord { false }; // op_type_tag of a family: what it is given is of the family (TypeTable::isTakenAtItsWord()). If not, it may be anything: Lowering::structToLookIn().
    // promoteEnvironments(). An op_create_lexical_environment that is no object: its variables are the function's own.
    bool isPromoted { false };
    // An op_get_from_scope or op_put_to_scope of a variable of such an environment: which, and where in it.
    Node* promotedEnvironment { nullptr };
    unsigned offsetInEnvironment { 0 };
    // An op_resolve_scope or op_get_parent_scope that gets to a scope that is an object by way of environments that are not: it is that many out from this.
    Node* scopeToStartFrom { nullptr };
    unsigned hopsFromThere { 0 };
    // Of an op_resolve_scope: that is not what it gives, only where it starts looking, which is this many scopes out from where it says to. (Those are not there. What it looks for is in none of them.)
    unsigned environmentsPassedOver { 0 };
    Escape escape { Escape::NotLookedAt }; // If it makes something (kindOfAllocation()).
    uint32_t fact { 0 }; // EXPERIMENT: Options::aotFacts().
    uint8_t iteratedFact { 0 }; // Likewise: it is an array. What an element holds, plus one.
    bool hasFact(unsigned kind, unsigned bitOfOption) const { return fact >> 28 == kind && (Options::aotFacts() & bitOfOption); }
    Node* site { nullptr }; // GuardKind::Structure, SlotsAgree: guards of property accesses.
    Node* otherSite { nullptr };
    // op_new_object: how many of Graph::storesOfLiteral() are part of it. Their values are the uses at NewObjectPlan::registerOf().
    // op_create_this: how many properties the object is made with (NewObjectPlan). Their values are the uses at NewObjectPlan::registerOf().
    unsigned numberOfLiteralProperties { 0 };
    // An op_get_by_val or op_get_length of an array, in a loop that changes no array: the header of the loop ahead of which the array is looked at (BasicBlock::arraysViewed).
    BasicBlock* viewedAheadOf { nullptr };
    Node* storage { nullptr }; // A guard of an access to an element of a typed array: the GuardKind::TypedArrayStorage that goes for it.

    // Lowering state.
    B3::Value* lowered { nullptr };
    B3::Value* loweredAsJSValue { nullptr }; // If that is how it came, and not how it is held (rep()).
    B3::Value* loweredLength { nullptr }; // GuardKind::TypedArrayStorage
    unsigned useCount { 0 };
    bool isHandled { false }; // A guard: it has done what the instruction does. If not, it lets everything by, and the instruction is on its own.

    bool isBytecode(OpcodeID id) const { return kind == NodeKind::Bytecode && opcode == id; }
    bool isConstant() const { return kind == NodeKind::Constant; }
    bool isInt32Constant() const { return isConstant() && constant.isInt32(); }
    bool isNumberConstant() const { return isConstant() && constant.isNumber(); }

    Node* use(VirtualRegister r) const
    {
        for (auto& use : uses) {
            if (use.reg == r)
                return use.node;
        }
        RELEASE_ASSERT_NOT_REACHED();
        return nullptr;
    }

    Rep rep() const
    {
        if (range.isKnown() && type && isSubtype(type, TNumber))
            return range.fitsInt32() ? Rep::Int32 : Rep::Int64;
        return repForType(type);
    }
    bool isInteger() const
    {
        Rep rep = this->rep();
        return rep == Rep::Int32 || rep == Rep::Int64;
    }

    template<typename Op> Op as() const { return instruction->as<Op>(); }

    // Whether the value is certain to get past an op_check_type with that mask: by its type, or because it is what got past a check
    // that let no more by. The types are too coarse to say that of every mask (see typeProvingMask()).
    bool isKnownToPass(unsigned mask) const
    {
        if (isSubtype(type, typeProvingMask(mask)))
            return true;
        for (const Node* node = this; node->isBytecode(op_check_type); node = node->use(node->as<OpCheckType>().m_value)) {
            unsigned earlier = node->as<OpCheckType>().m_mask;
            if (soundTypeTagsOfMask(earlier) & ~soundTypeTagsOfMask(mask))
                continue;
            if (!soundTypeMaskNamesTypedArray(mask) || !(earlier & MaskOtherObject) || (earlier >> SoundTypeTypedArrayShift) == (mask >> SoundTypeTypedArrayShift))
                return true;
        }
        return false;
    }

    void dump(PrintStream&) const;
};

struct BasicBlock {
    Graph* graph { nullptr }; // As Node::graph.
    unsigned index { 0 };
    unsigned bytecodeBegin { 0 };
    unsigned bytecodeEnd { 0 }; // Exclusive.
    bool isCatchEntrypoint { false };
    bool isReachable { false };
    bool isLoopHeader { false };
    bool isInLoop { false };
    // There are two copies of the code of a loop. The fast one does not have the long way of doing anything in it: where that would
    // be, a block ends in a guard, whose successors are the rest of the fast copy and the same instruction in the generic copy.
    // So nothing that the long ways can do (which is anything) has to be reckoned with in the fast copy: what it has loaded and
    // checked stays loaded and checked. The generic copy goes back to the fast one at the next loop header.
    bool isGeneric { false };
    bool endsWithGuard { false };
    // The way into the fast copy of a loop, from outside and from the generic copy: everything but the fast copy's own jumps back
    // comes by here. Whatever stays the same for as long as control stays in the fast copy can be seen to here, once. It ends in a
    // guard, whose other successor is the generic copy of the loop's header.
    bool isPreHeader { false };
    // How the generic copy of a loop gets back to the fast one: it comes before the pre-header. What a variable can be in the fast
    // copy is a matter of what goes into the loop and of what the fast copy does to it. The generic copy knows less about its values,
    // and if they counted, the fast copy would know no more. So they have to pass a test here (GuardKind::Reentry) and if one does
    // not, it is another time round the generic copy.
    bool isReentry { false };
    bool isSeldomReached { false }; // What is done if a GuardKind::IsIntrinsic does not hold.

    // optimizeLoops()
    BasicBlock* immediateDominator { nullptr };
    unsigned rpoIndex { 0 };
    unsigned dominatorPreNumber { 0 };
    unsigned dominatorPostNumber { 0 };
    bool dominates(const BasicBlock* other) const { return dominatorPreNumber <= other->dominatorPreNumber && dominatorPostNumber >= other->dominatorPostNumber; }
    Vector<Node*> phis;
    Vector<Node*> nodes;
    Vector<BasicBlock*, 2> predecessors;
    Vector<BasicBlock*, 2> successors; // For a conditional jump: taken, then not taken. For a switch: the cases in table order, then the default.
    BitVector liveIn; // Indexed by Graph::registerIndex().
    // Of the registers that live in memory (Graph::isHomed()): those that a handler reads that something in the block can throw to; and
    // those that some handler that can be got to from the end of the block reads, unless they are written first.
    BitVector readByHandlersOfBlock;
    BitVector readByHandlersAfterBlock;
    Vector<Node*> valuesAtTail; // Indexed by Graph::registerIndex().

    B3::BasicBlock* lowered { nullptr };
    // The header of a loop that is kept whole and changes no array: the arrays that it goes by, which are the same ones every time round. All ways in but the loop's own jumps back
    // go by a block that looks at them.
    Vector<Node*, 2> arraysViewed;
    BitVector bodyOfLoop; // By block index.
    B3::BasicBlock* loweredAhead { nullptr };
    B3::BasicBlock* loweredTail { nullptr }; // The B3 block that the block's last code went to.

    Node* terminal() const { return nodes.isEmpty() ? nullptr : nodes.last(); }
};

// Where a variable that a function does not declare itself is found: worked out from the scopes that statically enclose the
// function, which are the same for every closure made from it.
struct ScopeChainEntry {
    enum Kind : uint8_t {
        Lexical, // JSLexicalEnvironment (or the module's environment): symbolTable says what is in it.
        GlobalLexical,
        Global,
        Opaque, // A with scope, or a scope that sloppy eval can add variables to: nothing beyond it can be resolved now.
        Unknown, // Whatever the function is going to be closed over. All there is to go by is what its bytecode says.
    };
    Kind kind { Opaque };
    bool isModule { false };
    class JSC::SymbolTable* symbolTable { nullptr };
};
using ScopeChain = Vector<ScopeChainEntry, 4>; // From the scope a closure of the function captures, outwards.

class Graph {
    WTF_MAKE_NONCOPYABLE(Graph);
public:
    Graph(VM&, UnlinkedCodeBlock*, const ScopeChain&);
    ~Graph();

    VM& vm() { return m_vm; }
    UnlinkedCodeBlock* codeBlock() { return m_codeBlock; }

    // ---- A call of a function that is known can be done away with: what the function does is done where the call was (inlineCalls()).
    // The function is parsed on its own, into a graph of its own, and the graph of the caller takes that over: its blocks are among the
    // caller's from then on, and it stays what there is to ask about the code they are of. Whatever is asked of a graph about a node is
    // answered by the graph the node is of. What comes of lowering (slots, sites, calls) is all the caller's.
    Graph& outermost() { return *m_outermost; }
    bool isOutermost() const { return m_outermost == this; }
    // Which of InlineFrames it is. None: the function that is being compiled.
    unsigned inlineFrame() const { return m_inlineFrame; }
    struct InlineFrame {
        unsigned parent; // Another of these, or none: what the call was in.
        uint32_t callSite; // Where it was, there (CallSiteIndex::bits()).
        unsigned knownCallee; // What was called: indexOfKnownCallee().
        bool isTailCall { false }; // Whoever made it is gone by the time what was called runs, as far as anybody can tell.
    };
    Vector<InlineFrame> inlineFrames; // From 1. Of the outermost.
    // Everything that is left of the other but for what it knows.
    void adopt(std::unique_ptr<Graph>&&, InlineFrame);
    void computeOrderOfBlocks(); // m_rpo, after blocks have been added.
    const ModuleLinkage* linkage() const { return m_linkage; }
    // Of a closure that was made where its scope is at hand: that. It is what its op_get_scope gets (and is among that node's uses).
    Node* scopeOfClosure { nullptr };
    // What it returns is what the function that is being compiled returns, there and then: it is that function, or the call that it took
    // the place of was a tail call in code of which the same goes. Then a tail call in it is one still.
    bool isInTailPosition { true };
    // Its loops are left as they are: what they call is going to be part of them (see BasicBlock::isGeneric).
    bool loopsAreNotSplit { false };
    unsigned numberOfNodes() const { return m_nodes.size(); }
    Node* lastNode() { return &m_nodes.last(); }
    const ScopeChain& scopeChain() const { return m_scopeChain; }

    // Registers are numbered arguments first (this is 0), then locals.
    unsigned numRegisters() const { return m_numArguments + m_numLocals; }
    unsigned numArguments() const { return m_numArguments; }
    unsigned numLocals() const { return m_numLocals; }
    unsigned registerIndex(VirtualRegister reg) const
    {
        if (reg.isArgument())
            return reg.toArgument();
        ASSERT(reg.isLocal());
        return m_numArguments + reg.toLocal();
    }
    VirtualRegister registerForIndex(unsigned index) const
    {
        if (index < m_numArguments)
            return virtualRegisterForArgumentIncludingThis(index);
        return virtualRegisterForLocal(index - m_numArguments);
    }
    bool isTracked(VirtualRegister reg) const
    {
        if (reg.isArgument())
            return static_cast<unsigned>(reg.toArgument()) < m_numArguments;
        return reg.isLocal() && static_cast<unsigned>(reg.toLocal()) < m_numLocals;
    }

    // A register that is live into an exception handler lives in memory for the whole function: control gets to a handler
    // from the unwinder, with nothing in machine registers.
    bool isHomed(VirtualRegister reg) const { return m_homed.get(registerIndex(reg)); }
    bool hasHomedRegisters() const { return !m_homed.isEmpty(); }
    // Where: they are next to each other in the frame, and this is which of them it is.
    unsigned homeOf(VirtualRegister reg) const
    {
        ASSERT(isHomed(reg));
        unsigned result = 0;
        for (unsigned index : m_homed) {
            if (index == registerIndex(reg))
                return result;
            ++result;
        }
        RELEASE_ASSERT_NOT_REACHED();
        return 0;
    }
    unsigned numberOfHomes() const { return m_homed.bitCount(); }
    Convention convention() const { return m_convention; }
    HowValuesArePassed howValuesArePassed() const { return AOT::howValuesArePassed(m_facts, m_convention); }

    Node* addNode(NodeKind);
    BasicBlock* addBlock();

    Node* constant(JSValue);
    // With Options::useImmutableIntrinsics(). What the instruction reads, if it reads what cannot be anything else: a variable of the
    // global object, or a property of `base`. (For an op_resolve_scope, the global object.) There is then no need for the instruction.
    Node* intrinsicReadBy(const JSInstruction*, Node* base);
    Node* intrinsic(unsigned number);
    unsigned numberOfIntrinsicReads { 0 };
    CallIntrinsic intrinsicOfCall(const Node* callOrItsGuard) const;
    struct CallOperands {
        VirtualRegister callee;
        unsigned argc;
        unsigned argv;
        VirtualRegister argument(unsigned indexIncludingThis) const { return VirtualRegister(-static_cast<int>(argv) + CallFrame::thisArgumentOffset() + static_cast<int>(indexIncludingThis)); }
    };
    static CallOperands operandsOfCall(const JSInstruction*); // op_call or op_call_ignore_result
    // What the source says of the instruction that is there (op_type_tag), or of the one that the node is. None: 0.
    uint32_t typeTagAt(unsigned bytecodeOffset) const { return m_typeTags.get(bytecodeOffset); }
    // TypeTable::hasStructs(): the field that an op_get_by_id or op_put_by_id gets at without asking, if it does.
    static std::optional<TypeTable::Field> fieldOfStructGotAtBy(const Node*);
    static bool isThisOfWhatAnybodyMayCall(const Node*);
    // Of structs: the field of that name of the family that the value is proven to have been born into, if it is proven to have been born into one.
    static std::optional<TypeTable::Field> fieldOfWhatIsBornAs(const Node* base, UniquedStringImpl* name);
    static uint16_t familyOfNewObject(const Node*); // TypeTable::hasStructs(): what an op_new_object makes is born as that. Zero: nothing.
    // ---- Classes (ClassesOfProgram).
    // A call of @noteClass: the type that says what class it is (TypeTable::isClass()). Zero: it is no such call, or nothing is said.
    static uint32_t classNotedBy(const Node*);
    void noteClassesDefined();
    // An op_get_by_id that reads a closed method: the number of the function. It gives that and nothing else. Zero: it is not one.
    static uint32_t closedMethodReadBy(const Node*);
    // What `this` is born as in this code, which is that of a constructor, a method or an initializer of a class. Zero: there is no telling.
    uint16_t familyOfThis() const;
    Type typeOfThisOnEntry() const;
    static uint32_t typeTagOf(const Node* node) { return node->kind == NodeKind::Bytecode && node->instruction ? node->graph->typeTagAt(node->bytecodeIndex.offset()) : 0; }
    // The properties that an object literal starts out with: functor(index of the identifier, register the value is in).
    // An object literal: where the op_put_by_id are that make the object of an op_new_object what the literal says, as far as they
    // are sure to be got to one after the other. Whatever is done in between to work out what to store knows nothing of the object,
    // so that the object can be made, whole, where the last of them is.
    static Vector<unsigned, 16> storesOfLiteral(const JSInstructionStream&, unsigned offsetOfNewObject);
    // Of an op_get_by_val or op_put_by_val or the guard of one: the type of typed array that the base is known to be, if it holds numbers.
    static std::optional<JSType> typedArrayAccessed(const Node*);
    // The array that the node puts an element in, and the element: op_put_by_val, or a call that is taken for one of Array.prototype.push.
    std::pair<Node*, Node*> arrayAndElementStored(const Node*) const;
    // An array that the function makes itself: what it puts in it is a good guess at what is in it.
    static bool isArrayMadeHere(const Node* node) { return node->isBytecode(op_new_array) || node->isBytecode(op_new_array_with_size); }
    // The function that the call or construction is probably of, if there is any telling.
    const KnownFunction* knownCallee(const Node*, bool* isProven = nullptr) const;
    const KnownFunction* knownCalleeWithoutFacts(const Node*, bool* isProven) const;
    // What is iterated by an op_iterator_open, op_iterator_next or op_iterator_close_check is an array: Node::iteratedFact. Or 0.
    static unsigned iteratedFactOf(const Node*);
    bool calleeIsProven(const Node*) const; // See KnownFunction::isProven.
    // Before there is a graph to tell which scope a read is from: unless the code has a variable of its own of that name, there.
    const KnownFunction* probablyFunctionInVariableOfModule(unsigned identifier, unsigned scopeOffset) const;
    const KnownFunction* knownFunctionReadBy(const Node* getFromScope, bool* isProven = nullptr) const;
    // A call of a function that wants nothing of the object it is called as (KnownFunction::needsNoFunctionObject). None is passed.
    bool passesNoFunctionObject(const Node* call);
    // Whether this function is one of those, and whether its scope is where the module's environment is.
    bool needsFunctionObject() const { return m_needsFunctionObject; }
    bool scopeIsEnvironmentOfModule() const { return m_scopeIsEnvironmentOfModule; }
    uint32_t distanceOfEnvironmentOfModule();
    // What is read only to be called, by calls that do not pass it, is not read (Node::isElided).
    void elideReadsOfCalleesNotPassed();
    void findBuiltinsCalled(); // Node::directMethod. Once the types are known.
    bool hasTwoCopiesOfAll { false }; // Options::aotAssertsTypes(): not just of its loops.
    // See ProgramFacts.
    void noteUsesOfProvenFunctions(const FactsOfExecutables&);
    // f(a, ...b), f.apply(o, arguments): what the callee is passed is put together from where it is (Stub::CallVarargs, Stub::CallList).
    // What would have been made only to be copied from, right before the call, is not (Node::isElided). Nor is an array of the rest of
    // the arguments, or an arguments object, that nothing is done with but that: it says nothing that what this function was passed
    // does not say.
    void findListsOfArguments();
    // The list, if the node is a call that takes one and takes all of it.
    static Node* listOfArgumentsOf(const Node*);
    // One of the things that only the engine's own functions can name, and that nobody can put anything else in the place of.
    static std::optional<LinkTimeConstant> linkTimeConstantOf(const Node*);
    // An op_get_from_scope or an op_put_to_scope: how far below the Instance the environment is that has the variable, if it is
    // one of a module, whose place is known (ImageEnvironment::distance). Then the scope that the instruction names is not needed.
    std::optional<uint32_t> distanceOfEnvironmentAccessed(const Node*);
    std::optional<uint32_t> distanceOfEnvironmentResolvedTo(const Node*); // Likewise, an op_resolve_scope.
    static bool isThatManyScopesOut(const Node* scope, unsigned hops);
    // A scope that stands for `this` in a call of what was found in it, which is how a call of a variable is made in case the scope
    // is that of a `with`: and this one is not, so that whoever is called makes undefined of it, or the global `this`.
    bool isScopeThatStandsForNoThis(const Node*);
    void setCalleeHints(const CalleeHints* hints) { m_hints = hints; }
    // What the program says of the function that this is the code of, for a call.
    void setFacts(const ProgramFacts* facts) { m_facts = facts; }
    const ProgramFacts* facts() const { return m_facts; }
    // reader: who is to look again if there turns out to be more to what it has read (VariableFacts::read()).
    void setVariableFacts(VariableFacts* facts, unsigned reader = VariableFacts::nobody)
    {
        m_variableFacts = facts;
        m_readerOfFacts = reader;
    }
    VariableFacts* variableFacts() const { return m_variableFacts; }
    // Options::aotLogsFacts(): what to call the code, when it comes to saying what it contributes.
    void setNameForLog(const String& name) { m_nameForLog = name; }
    const String& nameForLog() const { return m_nameForLog; }
    unsigned readerOfFacts() const { return m_readerOfFacts; }
    // Which scope of the source the value is an environment record of (Variable::scope). Null: there is no telling.
    const void* identityOfScope(const Node*, unsigned depth = 0);
    // op_get_from_scope, op_put_to_scope. None: it is not in an environment record, or there is no telling which.
    Variable variableAccessedBy(const Node*);
    // What has to be known of variables before anything is said of what they hold: see VariableFacts.
    void noteWhatCannotBeToldOfVariables(VariableFacts&);
    Type typeOfArgumentOnEntry(unsigned indexIncludingThis) const
    {
        if (!m_facts || !m_facts->isClosed || indexIncludingThis >= ProgramFacts::mostParameters)
            return TTop;
        if (!indexIncludingThis)
            return m_facts->thisType.load();
        return m_facts->parameterTypes[indexIncludingThis].load();
    }
    const CalleeHints* calleeHints() const { return m_hints; }
    unsigned indexOfKnownCallee(const ImageKey&);
    Vector<ImageKey> knownCallees;
    bool callsItself { false }; // As good as a loop.
    bool makesCalls { false }; // Of functions, in frames of their own.
    bool emitsCalls { false }; // There may be an instruction in the code that calls something, if only a stub. (A jump is not one.)
    bool emitsCallsWhateverIsLeft { false }; // And there is no telling by looking at what the code has come to.
    mutable std::optional<bool> callsAreLeft; // hasNoFrame()
    // Integers of the program's that are as big as addresses are (see the check for those in AOTCompiler.cpp).
    UncheckedKeyHashSet<int64_t, WTF::IntHash<int64_t>, WTF::UnsignedWithZeroKeyHashTraits<int64_t>> wideIntegerConstants;

    void fail(ASCIILiteral reason, OpcodeID = op_nop);
    bool failed() const { return !m_failureReason.isNull(); }
    ASCIILiteral failureReason() const { return m_failureReason; }
    OpcodeID failureOpcode() const { return m_failureOpcode; }

    void dump(PrintStream&) const;

    Vector<BasicBlock*> blocksInReversePostOrder() const { return m_rpo; }

    BasicBlock* root { nullptr };
    Vector<std::unique_ptr<BasicBlock>> blocks;
    Vector<BasicBlock*> catchEntrypoints;
    Vector<BasicBlock*> m_rpo;
    Vector<BasicBlock*> blockForOffset; // Indexed by bytecode offset; null unless a block starts there.
    Vector<BasicBlock*> genericTargetForOffset; // Where a jump from the generic copy of a loop goes instead, if not null.
    Vector<BasicBlock*> headerForOffset; // Where a jump back in the fast copy of a loop goes: blockForOffset has the pre-header.
    UncheckedKeyHashSet<uint64_t, WTF::IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> jumpsBack; // The end of the block that jumps, and above it where to.
    bool hasGuards() const { return !genericTargetForOffset.isEmpty(); }
    BasicBlock* targetFrom(BasicBlock* from, unsigned offset) const
    {
        if (!hasGuards())
            return blockForOffset[offset];
        if (from->isGeneric) {
            if (BasicBlock* target = genericTargetForOffset[offset])
                return target;
        } else if (BasicBlock* header = headerForOffset[offset]; header && jumpsBack.contains(static_cast<uint64_t>(offset) << 32 | from->bytecodeEnd))
            return header;
        return blockForOffset[offset];
    }

    struct StaticVariable {
        enum Kind : uint8_t {
            Closure, // In the scope that is `depth` out, at `offset`.
            Import, // Of the module whose environment is `depth` out, and known to be at `offset` in the environment that is in that one's `import.slot`.
            ModuleImport, // Of the module whose environment is `depth` out. What it is bound to is found out when it is read.
            Unresolved, // Not in a scope that eval or with can add to, and that is all.
            Dynamic,
        } kind { Dynamic };
        unsigned depth { 0 };
        ScopeOffset offset;
        bool inModule { false };
        bool isReadOnly { false };
        bool isInGlobalScopes { false }; // Unresolved: none of the scopes of the code around the function has it.
        bool isGlobal { false }; // Likewise, however that is known.
        bool isInOutermostEnvironment { false }; // Closure: it is a variable of the module itself.
        StaticImport import;

        // op_resolve_scope: the answer is that many scopes out. (For an import that is not linked, the module that imports it.)
        bool isAtStaticDepth() const { return kind == Closure || kind == ModuleImport; }
        // op_get_from_scope: where it is gets looked up once, and kept in a Slot.
        bool isCachedInSlot() const { return kind == Unresolved || kind == ModuleImport; }
    };
    StaticVariable resolveStatically(unsigned identifierIndex, unsigned localScopeDepth, ResolveType);
    // What the operations are told about the instruction other than the name (Site).
    unsigned extraOfResolveScope(const OpResolveScope&);
    unsigned extraOfGetFromScope(const OpGetFromScope&);
    void setLinkage(const ModuleLinkage*, const DeclaredNamesLink*);
    bool usesStaticImports { false }; // The code is only good for a module that is linked as its ModuleLinkage says.
    bool startsCold { false }; // CompiledFunctionInfo::startsCold
    bool m_needsFunctionObject { true };
    bool m_scopeIsEnvironmentOfModule { false };
    BitVector m_homed;
    Vector<Type> homedTypes; // Indexed by registerIndex(): everything that is ever stored to a homed register.
    unsigned numICSlots { 0 };
    Vector<Site> sites; // Of the slots that have one; the rest are past the end, or blank.
    // CompiledFunctionInfo's.
    Vector<uint32_t> siteConstants;
    Vector<UniquedStringImpl*> selectors;
    Vector<KnownShape> shapes;
    void noteSelectorOfSite(unsigned slot, UniquedStringImpl*);
    void noteShapeOfSite(unsigned slot, KnownShape&&);
    Vector<uint32_t> quotableSites; // CompiledFunctionInfo::quotableSites
    Vector<uint32_t> callSites; // CompiledFunctionInfo::callSites
    Vector<SiteOfSpread> sitesOfSpreads; // CompiledFunctionInfo::sitesOfSpreads
    Vector<uint32_t> plans;
    void notePlanOfSite(unsigned firstSlot, Vector<uint32_t, 16>&& words); // AllocationPlan
    // An op_new_object that is made whole (Node::numberOfLiteralProperties): what with. Nothing, if it is not a shape to be known by.
    std::optional<KnownShape> shapeOfLiteral(const Node*) const;
    StubCalls stubCalls;
    IndexReferences indexReferences;

private:
    VM& m_vm;
    UnlinkedCodeBlock* m_codeBlock;
    ScopeChain m_scopeChain;
    const CalleeHints* m_hints { nullptr };
    const ProgramFacts* m_facts { nullptr };
    VariableFacts* m_variableFacts { nullptr };
    String m_nameForLog;
    unsigned m_readerOfFacts { VariableFacts::nobody };
    UncheckedKeyHashMap<int, Vector<Node*>, WTF::IntHash<int>, WTF::UnsignedWithZeroKeyHashTraits<int>> m_storesToHomed; // See identityOfScope().
    bool m_hasStoresToHomed { false };
    const ModuleLinkage* m_linkage { nullptr };
    const DeclaredNamesLink* m_declaredNames { nullptr };
    BitVector m_namesAssignedTo; // By index of the identifier: op_put_to_scope by name.
    UncheckedKeyHashMap<unsigned, uint32_t, WTF::IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_typeTags; // By where the instruction is.
    unsigned m_numArguments;
    unsigned m_numLocals;
    Convention m_convention;
    SegmentedVector<Node, 32> m_nodes;
    Node* m_emptyConstant { nullptr };
    UncheckedKeyHashMap<EncodedJSValue, Node*, EncodedJSValueHash, EncodedJSValueHashTraits> m_constants;
    UncheckedKeyHashMap<unsigned, Node*, DefaultHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_intrinsics;
    ASCIILiteral m_failureReason;
    OpcodeID m_failureOpcode { op_nop };
    Graph* m_outermost { this };
    unsigned m_inlineFrame { 0 };
    unsigned m_numberOfNodesAdopted { 0 };
    Vector<std::unique_ptr<Graph>> m_adopted;
};

// Phases. Each returns false (and Graph::failed() says why) if the function is not for the static compiler.
bool parseBytecode(Graph&);
// What the function returns. calleesConsulted: the functions whose KnownFunction::returnType that went by.
// calleesGivenMore: the closed functions that it calls with something that nobody was known to pass them (ProgramFacts::parameterTypes).
Type inferTypes(Graph&, Vector<const KnownFunction*>* calleesConsulted = nullptr, Vector<const KnownFunction*>* calleesGivenMore = nullptr);
void inferRanges(Graph&);
void optimizeLoops(Graph&);
void simplify(Graph&);
void inlineCalls(Graph&, const CodeOfProgram&);
void analyzeEscapes(Graph&); // Node::escape
void promoteEnvironments(Graph&); // Node::isPromoted
// ProgramFacts::parametersThatEscape, going by what is said so far of the functions it calls (calleesConsulted).
uint32_t parametersThatEscape(Graph&, Vector<const KnownFunction*>* calleesConsulted);
void reportEscapeStatistics();

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
