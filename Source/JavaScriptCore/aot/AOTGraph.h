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

// The ahead-of-time compiler's IR: the bytecode of one function in SSA form, with a proven Type on every value.
//
// Most nodes are bytecode instructions. A node reads its immediates from the instruction and finds its inputs by the virtual
// registers the instruction names (Node::use()), so an opcode needs no code of its own to enter the IR; BytecodeUseDef reports what
// every instruction reads and writes. What an opcode does need is a type rule (AOTTypeInference.cpp; the default is "any value")
// and a lowering (AOTLower*.cpp). A function that contains an instruction with no lowering is left to the other tiers.
//
// There is no speculation. A lowering selects code based on the proven types of its inputs. Where those are unknown it emits the
// operation's full semantics, out of line if that is long.
enum class NodeKind : uint8_t {
    Bytecode, // An instruction. Defines at most one register, or several through Proj nodes.
    Constant, // A compile-time constant that is not a cell.
    ConstantCell, // A constant register that holds a cell. Loaded from the function's constants.
    Intrinsic, // One of the realm's ImmutableIntrinsics that is a cell. Loaded from the Instance.
    LinkTimeConstant, // A link-time constant of the realm (`intrinsic` says which). It is materialized on first use, so this node marks where it is read.
    Argument, // The value an argument register has on entry.
    Phi,
    Proj, // One of the registers defined by an instruction that defines several. uses[0] is the instruction.
    GetStack, // Reads a register that lives in the frame (see Graph::livesInFrame).
    SetStack, // Writes one. uses[0] is the value.
    Guard, // Ends a block of a loop's fast copy (see BasicBlock::isGeneric). Has the same uses as the instruction that follows it.
    Narrow, // uses[0] re-entering the fast copy of a loop, with its type narrowed to that of `target` (see BasicBlock::isReentry).
};

// A call that has a fast path if the callee turns out to be the host function suggested by the property name it was read from. The
// name only justifies emitting the fast path; the callee is still checked.
enum class CallIntrinsic : uint8_t { None, MathSqrt, MathAbs, MathFloor, MathCeil, MathTrunc, MathFround, MathMin, MathMax, MathIMul, StringCharCodeAt, ArrayPush };
inline bool hasNoEffects(CallIntrinsic intrinsic) { return intrinsic != CallIntrinsic::ArrayPush; }
CallIntrinsic callIntrinsicFor(UniquedStringImpl* propertyName, unsigned argumentCountIncludingThis);

// What a guard checks. Created by optimizeLoops() unless noted otherwise.
enum class GuardKind : uint8_t {
    Whole, // The whole instruction: everything its fast path requires.
    Nothing, // Always passes.
    Reentry, // The inputs of the block's Narrow nodes have the types those nodes claim.
    Structure, // The base (uses[0]) has the Structure that the cache of `site` holds.
    SlotsAgree, // The caches of `site` and `otherSite` hold the same Structure.
    SlotIsPlain, // The cache of `site` is for a property stored in the base itself (not Slot::isIndirect).
    // These bracket a run of SlotsAgree and SlotIsPlain guards, which depend only on caches. The run is skipped if no cache has
    // changed since it last passed.
    BeginSlotChecks,
    EndSlotChecks,
    Callee, // The callee is what the call site expects.
    KnownCallee, // The callee (uses[0]) is a closure of the function the call was compiled against (Graph::knownCallee()).
    TypedArrayStorage, // The typed array (uses[0]) has a fixed length. Also loads its length and storage pointer.
    IsIntrinsicOfArray, // IsIntrinsic, and uses[1] is an array. Created by inlineCalls().
    IsIntrinsic, // uses[0] is the ImmutableIntrinsic numbered Node::intrinsic. Created by inlineCalls().
    // With Options::useAOTFunctionSplitting(). For an op_get_by_id or op_put_by_id whose base has a static type (TypeTable): the base was
    // allocated with the property in Node::slotOfField (its layout is in [firstLayout, lastLayout]) and the property is still
    // there. The guard also performs the access. Afterwards the base's layout is known (a Narrow with narrowedTo), so later guards
    // on it check less.
    Field,
};

// A constructor typically starts by initializing the new object's properties:
//     constructor(a, b) { this.a = a; this.b = b; }
// Until something other than a store observes the object, its intermediate states are invisible. The allocation is therefore
// deferred until after the last of those stores, and the object is created with all its properties in place.
//
// Derived from the bytecode alone, by the compiler and again by the slow path operation.
struct NewObjectPlan {
    struct Property {
        unsigned identifier;
        bool isDefined; // The store defines the property regardless of the prototype chain. Otherwise setters and read-only properties on the chain apply.
        bool isStrict; // Whether a rejected store throws.
    };
    Vector<Property, 8> properties; // In insertion order.
    struct Store {
        unsigned offset; // Bytecode offset of the op_put_by_id.
        unsigned property; // Index into `properties`.
    };
    Vector<Store, 8> stores;

    // The pseudo-register under which a property's value appears among the node's uses.
    static VirtualRegister registerOf(unsigned property) { return VirtualRegister(0x20000000 + static_cast<int>(property)); }

    static NewObjectPlan forCreateThis(const JSInstructionStream&, unsigned offsetOfCreateThis);
};

class Graph;
struct BasicBlock;
struct Node;

// The result of escape analysis for an allocation: whether it can still be reachable after the function returns and, if so, the
// first reason found (analyzeEscapes()).
enum class Escape : uint8_t {
    NotAnalyzed,
    StaysHere, // Only this function's code (including inlined callees) sees it.
    IsOnlyLent, // It is passed only to known functions that do not retain it.
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
    PassedToKnownCalleeThatRetainsIt,
    PassedInList,
    PassedInTailCall,
    Constructed,
    StoredInEscapingObject,
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

struct Use {
    VirtualRegister reg;
    Node* node { nullptr };
};

struct Node {
    // The graph the node came from: the function being compiled, or one that was inlined into it (inlineCalls()).
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
    JSValue constant; // NodeKind::Constant
    VirtualRegister reg; // ConstantCell, Argument, Proj, GetStack, SetStack, Phi (the register it merges)
    Node* replacement { nullptr }; // Set during phi simplification.
    Node* guard { nullptr }; // For an instruction: the guard that must pass for its fast path to be reached.
    Node* guarded { nullptr }; // For a guard: the instruction it guards.
    Node* target { nullptr }; // NodeKind::Narrow: the loop header phi that the value flows into.
    bool checksNarrowedType { false }; // NodeKind::Narrow with narrowedTo: nothing has checked the type yet, so this node checks it and throws a TypeError on failure.
    Type narrowedTo { TNone }; // NodeKind::Narrow with no target: the type that a GuardKind::Field established.
    uint16_t slotOfField { 0 }; // GuardKind::Field
    uint16_t firstLayout { 0 }; // GuardKind::Field, and the Narrow that follows it.
    uint16_t lastLayout { 0 };
    // GuardKind::Field on a read: layouts that lack the property, for which the result is undefined. 0, 0 means none.
    uint16_t firstWithout { 0 };
    uint16_t lastWithout { 0 };
    // GuardKind::Field with Options::useAOTTypedFields(): the field type (TypeTable::FieldType). Zero kinds means unconstrained.
    uint16_t fieldTypeKinds { 0 };
    uint16_t fieldTypeFirst { 0 };
    uint16_t fieldTypeLast { 0 };
    // Whether the value is known to have a layout in [first, last], from its type or because it passed a check for a subrange.
    bool hasLayoutInRange(uint16_t first, uint16_t last) const
    {
        for (const Node* node = this; node->kind == NodeKind::Narrow && node->narrowedTo; node = node->uses[0].node) {
            if (node->firstLayout >= first && node->lastLayout <= last)
                return true;
        }
        // For an open layout the tag alone proves nothing. See Lowering::fieldStorageFor().
        if (isBytecode(op_type_tag) && firstLayout >= first && lastLayout <= last && !Options::auditAOTTypedFields() && isTrusted)
            return true;
        return type && isSubtype(type, TCell) && hasLayoutInRangeIfCell(first, last);
    }
    // The same, provided the value is a cell. It may also be undefined, null or a number.
    bool hasLayoutInRangeIfCell(uint16_t first, uint16_t last) const
    {
        if (Type cells = type & TCell; cells && isSubtype(cells, TFinalObject)) {
            auto layouts = layoutRangeOf(cells);
            if (layouts.lowest >= first && layouts.highest <= last)
                return true;
        }
        // A field type gives the exact range, whereas a Type can only express the high-order bits the range has in common.
        const Node* node = this;
        while (node->kind == NodeKind::Narrow || node->isBytecode(op_check_type) || node->isBytecode(op_check_tdz))
            node = node->uses[0].node;
        if (node->kind != NodeKind::Bytecode || !node->guard || node->guard->guardKind != GuardKind::Field || !node->guard->fieldTypeFirst)
            return false;
        constexpr unsigned notCells = MaskUndefined | MaskNull | MaskBoolean | MaskNumber;
        return !(node->guard->fieldTypeKinds & ~(notCells | MaskOtherObject)) && node->guard->fieldTypeFirst >= first && node->guard->fieldTypeLast <= last;
    }
    unsigned expectedMask { 0 }; // op_get_by_val: the mask of the op_check_type that immediately consumes the result, if any.
    GuardKind guardKind { GuardKind::Whole };
    uint16_t intrinsic { 0 }; // NodeKind::Intrinsic: its index.
    // For a call: the built-in function being called, by its ImmutableIntrinsics index, provided the receiver is the given kind of
    // Receiver (none means any receiver). The same fields are set on the read of the callee. See Graph::findBuiltinsCalled().
    uint16_t builtinCalled { 0 };
    uint8_t receiverOfBuiltin { 0 };
    bool structureIsChecked { false }; // For a property access guard: another guard has already checked the base's Structure.
    bool slotIsPlain { false }; // For a property access guard: another guard has already checked SlotIsPlain.
    bool calleeIsChecked { false };
    bool wasInferredUnreachable { false }; // inferTypes() found no value it could produce. For Options::validateAOTInferredTypes().
    bool isElided { false }; // Its value is unused and computing it has no side effects. Not lowered.
    // An op_get_by_id of a non-escaping method whose value is unused. All that remains is the throw if the base is null or
    // undefined.
    bool isReadOnlyToBeCalled { false };
    bool isTrusted { false }; // op_type_tag with a typed layout: the operand is known to have the layout (TypeTable::isTrusted()). Otherwise it may be anything. See Lowering::fieldStorageFor().
    // Set by promoteEnvironments(). An op_create_lexical_environment that allocates nothing: its variables become locals of the
    // function.
    bool isPromoted { false };
    // An op_get_from_scope or op_put_to_scope on a variable of a promoted environment: the environment and the variable's offset.
    Node* promotedEnvironment { nullptr };
    unsigned offsetInEnvironment { 0 };
    // An op_resolve_scope or op_get_parent_scope that reaches a real scope object through promoted environments: the result is
    // hopsFromThere scopes out from scopeToStartFrom.
    Node* scopeToStartFrom { nullptr };
    unsigned hopsFromThere { 0 };
    // For an op_resolve_scope, scopeToStartFrom is only where the search starts. This is the number of promoted environments
    // skipped, none of which declares the name.
    unsigned environmentsPassedOver { 0 };
    Escape escape { Escape::NotAnalyzed }; // For an allocation (kindOfAllocation()).
    Node* site { nullptr }; // GuardKind::Structure, SlotsAgree: the property access guards involved.
    Node* otherSite { nullptr };
    // op_new_object: how many of Graph::storesOfLiteral() are folded into it.
    // op_create_this: how many properties the object is created with (NewObjectPlan).
    // In both cases the values are the uses at NewObjectPlan::registerOf().
    unsigned numberOfLiteralProperties { 0 };
    // An op_call to a function that returns this many values in registers (planMultiValueReturns()). The call's own result is
    // unused.
    uint8_t numberOfReturnValues { 0 };
    uint8_t returnValueIndex { 0 }; // A NodeKind::Proj of such a call, replacing the op_get_by_id that read the property: which value.
    // An op_get_by_val or op_get_length on an array, in a loop that modifies no array: the header of the loop before which the
    // array's butterfly and length are loaded (BasicBlock::arraysViewed).
    BasicBlock* viewedAheadOf { nullptr };
    Node* arrayViewed { nullptr }; // The corresponding entry in arraysViewed: the base as it enters the loop.
    Node* storage { nullptr }; // For a guard on a typed array element access: its GuardKind::TypedArrayStorage.

    // Lowering state.
    B3::Value* lowered { nullptr };
    B3::Value* loweredAsJSValue { nullptr }; // The boxed form, if that is how the value was produced and it differs from rep().
    B3::Value* loweredLength { nullptr }; // GuardKind::TypedArrayStorage
    unsigned useCount { 0 };
    bool isHandled { false }; // For a guard: it performed the instruction's work. Otherwise it always passes and the instruction does the work itself.

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

    // Whether the value is guaranteed to pass an op_check_type with this mask, from its type or because it passed a check that is
    // at least as strict. Types are too coarse to prove this for every mask (see typeProvingMask()).
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
    // In a loop only because it belongs to a callback that a built-in calls once per iteration of its own loop (inlineCalls()). The
    // trip count is unknown.
    bool isOnlyInLoopOfBuiltin { false };
    // A split loop has two copies. The fast copy contains no slow paths: where one would be, the block ends in a guard whose
    // successors are the rest of the fast copy and the same instruction in the generic copy. The fast copy therefore never has to
    // account for the effects of a slow path (which could be anything), so what it has loaded and checked stays valid. The generic
    // copy returns to the fast copy at the next loop header.
    bool isGeneric { false };
    bool endsWithGuard { false };
    // The entry to a loop's fast copy, from outside the loop and from the generic copy. Everything except the fast copy's own back
    // edges passes through it, so loop-invariant checks can be done here once. It ends in a guard whose other successor is the
    // generic copy of the loop header.
    bool isPreHeader { false };
    // The path from a loop's generic copy back to the fast copy, placed before the pre-header. The types of variables in the fast
    // copy depend only on what enters the loop and on what the fast copy does. The generic copy knows less about its values, and
    // merging them in directly would widen the fast copy's types. Instead they must pass a check here (GuardKind::Reentry). If one
    // fails, execution stays in the generic copy for another iteration.
    bool isReentry { false };
    bool isSeldomReached { false }; // The fallback for a failed GuardKind::IsIntrinsic.

    // Computed by optimizeLoops().
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
    // For registers that live in the frame (Graph::livesInFrame()): those read by a handler that something in this block can throw
    // to, and those read by a handler reachable from the end of this block before being overwritten.
    BitVector readByHandlersOfBlock;
    BitVector readByHandlersAfterBlock;
    Vector<Node*> valuesAtTail; // Indexed by Graph::registerIndex().

    B3::BasicBlock* lowered { nullptr };
    // For the header of an unsplit loop that modifies no array: the loop-invariant arrays it accesses. Every entry to the loop
    // except its own back edges passes through a block that loads their butterfly and length.
    Vector<Node*, 2> arraysViewed;
    BitVector bodyOfLoop; // By block index.
    B3::BasicBlock* loweredAhead { nullptr };
    B3::BasicBlock* loweredTail { nullptr }; // The B3 block that received the last of this block's code.

    Node* terminal() const { return nodes.isEmpty() ? nullptr : nodes.last(); }
};

// How a free variable of a function is resolved, derived from the scopes that statically enclose the function. These are the same
// for every closure created from it.
struct ScopeChainEntry {
    enum Kind : uint8_t {
        Lexical, // JSLexicalEnvironment (or a module environment). symbolTable describes its contents.
        GlobalLexical,
        Global,
        Opaque, // A with scope, or a scope that sloppy eval can add variables to. Nothing beyond it can be resolved statically.
        Unknown, // The enclosing scope is not known at compile time. Only the function's own bytecode is available.
    };
    Kind kind { Opaque };
    bool isModule { false };
    class JSC::SymbolTable* symbolTable { nullptr };
};
using ScopeChain = Vector<ScopeChainEntry, 4>; // From the scope that a closure of the function captures, outwards.

class Graph {
    WTF_MAKE_NONCOPYABLE(Graph);
public:
    Graph(VM&, UnlinkedCodeBlock*, const ScopeChain&);
    ~Graph();

    VM& vm() { return m_vm; }
    UnlinkedCodeBlock* codeBlock() { return m_codeBlock; }

    // ---- Inlining (inlineCalls()). The callee is parsed into a graph of its own, which the caller's graph then takes over: the
    // callee's blocks join the caller's, and the callee's graph remains the source of information about its own code. A query about
    // a node is forwarded to the graph the node came from. Everything produced by lowering (slots, sites, calls) belongs to the
    // outermost graph.
    Graph& outermost() { return *m_outermost; }
    bool isOutermost() const { return m_outermost == this; }
    // Index into inlineFrames. Zero for the function being compiled.
    unsigned inlineFrame() const { return m_inlineFrame; }
    struct InlineFrame {
        unsigned parent; // The inline frame that contains the call, or zero.
        uint32_t callSite; // The call's location there (CallSiteIndex::bits()).
        unsigned knownCallee; // The callee: indexOfKnownCallee().
        bool isTailCall { false }; // The caller's frame must appear to be gone once the callee runs.
    };
    Vector<InlineFrame> inlineFrames; // Indexed from 1. Only used on the outermost graph.
    // Takes over the other graph's blocks and nodes, and keeps it alive for queries about its code.
    void adoptInlinee(std::unique_ptr<Graph>&&, InlineFrame);
    void computeOrderOfBlocks(); // Recomputes m_rpo after blocks have been added.
    const ModuleLinkage* linkage() const { return m_linkage; }
    // For an inlined closure whose scope is available at the call site: that scope. It is the value of the closure's op_get_scope
    // (and is among that node's uses).
    Node* scopeOfClosure { nullptr };
    // A return from this graph is a return from the function being compiled: either it is that function, or it was inlined at a
    // tail call in a graph for which the same holds. A tail call in it is then still a tail call.
    bool isInTailPosition { true };
    // Its loops are not split, because callbacks will be inlined into them (see BasicBlock::isGeneric).
    bool loopsAreNotSplit { false };
    Vector<UnlinkedFunctionExecutable*> functionsMade; // On the outermost graph: the functions that the generated code instantiates.
    // An inline-only variant of an array method (builtins/ArrayPrototype.js): op_get_by_val yields the empty value (JSValue())
    // where there is no element.
    bool readsElementsOrEmpty { false };
    bool isInlinedBuiltin { false }; // One of JSC's own builtins.
    bool wasCalledInLoop { false }; // Inlined at a call site that is in a loop.
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

    // A register that is live into an exception handler lives in the frame for the whole function, because the unwinder transfers
    // control to a handler with nothing in machine registers.
    bool livesInFrame(VirtualRegister reg) const { return m_homed.get(registerIndex(reg)); }
    bool hasFrameRegisters() const { return !m_homed.isEmpty(); }
    // Frame registers are contiguous in the frame. Returns the register's index among them.
    unsigned homeOf(VirtualRegister reg) const
    {
        ASSERT(livesInFrame(reg));
        unsigned result = 0;
        for (unsigned index : m_homed) {
            if (index == registerIndex(reg))
                return result;
            ++result;
        }
        RELEASE_ASSERT_NOT_REACHED();
        return 0;
    }
    unsigned numberOfFrameRegisters() const { return m_homed.bitCount(); }
    Convention convention() const { return m_convention; }
    HowValuesArePassed howValuesArePassed() const { return AOT::howValuesArePassed(m_summary, m_convention); }

    Node* addNode(NodeKind);
    BasicBlock* addBlock();

    Node* constant(JSValue);
    // With Options::useImmutableIntrinsics(). If the instruction reads something that cannot change (a global variable, or a
    // property of `base`), returns its value. For an op_resolve_scope, returns the global object. The instruction is then
    // unnecessary.
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
    // The static type tag (op_type_tag) for the instruction at this offset, or for the node. Zero if there is none.
    uint32_t typeTagAt(unsigned bytecodeOffset) const { return m_typeTags.get(bytecodeOffset); }
    // With TypeTable::tableHasTypedFields(): the field that an op_get_by_id or op_put_by_id accesses without a check, if any.
    static std::optional<TypeTable::Field> typedFieldAccessedBy(const Node*);
    static bool isThisOfEscapingFunction(const Node*);
    // With typed fields: the field with this name in the typed layout that `base` is proven to have, if any.
    static std::optional<TypeTable::Field> fieldOfTypedBase(const Node* base, UniquedStringImpl* name);
    static uint16_t layoutIDOfNewObject(const Node*); // With TypeTable::tableHasTypedFields(): the typed layout of what an op_new_object allocates, or zero.
    // ---- Classes (ClassesOfProgram).
    // For a call to @noteClass: the class's type (TypeTable::isClass()). Zero if the node is not such a call or the class has no
    // type.
    static uint32_t classNotedBy(const Node*);
    void noteClassesDefined();
    // For an op_get_by_id that always reads one particular non-escaping method: that function's number. Otherwise zero.
    static uint32_t closedMethodReadBy(const Node*);
    // The typed layout of `this` in this code, which must be a constructor, method or initializer of a class. Zero if unknown.
    uint16_t layoutIDOfThis() const;
    Type typeOfThisOnEntry() const;
    static uint32_t typeTagOf(const Node* node) { return node->kind == NodeKind::Bytecode && node->instruction ? node->graph->typeTagAt(node->bytecodeIndex.offset()) : 0; }
    // For an object literal: the offsets of the op_put_by_id instructions that initialize the object of an op_new_object, as far as
    // they are certain to execute consecutively. The code between them that computes the stored values cannot observe the object,
    // so the object can be allocated fully initialized at the last store.
    static Vector<unsigned, 16> storesOfLiteral(const JSInstructionStream&, unsigned offsetOfNewObject);
    // For an op_get_by_val or op_put_by_val, or its guard: the typed array type the base is known to have, if its elements are
    // numbers.
    static std::optional<JSType> typedArrayAccessed(const Node*);
    // The array that the node stores an element into, and the element. Applies to op_put_by_val and to calls assumed to be
    // Array.prototype.push.
    std::pair<Node*, Node*> arrayAndElementStored(const Node*) const;
    // An array allocated by this function. What the function stores in it is a good prediction of its contents.
    static bool isArrayMadeHere(const Node* node) { return node->isBytecode(op_new_array) || node->isBytecode(op_new_array_with_size); }
    // The function that the call or construct probably targets, if that can be determined.
    const KnownFunction* knownCallee(const Node*, bool* isExact = nullptr) const;
    const KnownFunction* knownCalleeIgnoringSummaries(const Node*, bool* isExact) const;
    bool calleeIsExact(const Node*) const; // See KnownFunction::isExact.
    // For use before the graph exists and scopes can be resolved. Valid unless the code declares a variable with the same name.
    const KnownFunction* probablyFunctionInVariableOfModule(unsigned identifier, unsigned scopeOffset) const;
    const KnownFunction* knownFunctionReadBy(const Node* getFromScope, bool* isExact = nullptr) const;
    // A call to a function that never uses its callee (KnownFunction::needsNoFunctionObject). The callee is not passed.
    bool passesNoFunctionObject(const Node* call);
    // Whether this function needs its callee, and whether its scope is the module environment.
    bool needsFunctionObject() const { return m_needsFunctionObject; }
    bool scopeIsEnvironmentOfModule() const { return m_scopeIsEnvironmentOfModule; }
    uint32_t distanceOfEnvironmentOfModule();
    // Elides reads whose value is only used as the callee of calls that do not pass it (Node::isElided).
    void elideReadsOfCalleesNotPassed();
    // For `array[Symbol.iterator] === something` where the read has no other use: elides the read (Node::isElided).
    // Lowering::lowerEquality() handles the comparison.
    void elideReadsOfIteratorMethodsOfArrays();
    static bool isReadOfIteratorMethodOfArray(const Node*);
    static bool methodMayBeOverridden(ASCIILiteral nameOfClass, Node* read); // Whether a program class that extends this built-in class may override the method.
    static bool isIteratorMethodOfAnyArray(const Node*); // Array.prototype.values, which is also Array.prototype[Symbol.iterator].
    void findBuiltinsCalled(); // Sets Node::builtinCalled. Requires types.
    bool hasTwoCopiesOfAll { false }; // With Options::useAOTFunctionSplitting(): the whole function has two copies, not just its loops.
    // See FunctionSummary.
    void recordUsesOfKnownFunctions(const FunctionSummaryMap&);
    void noteFieldsComparedWithStrings(); // TypeTable::noteComparedWithString()
    // For f(a, ...b) and f.apply(o, arguments): the callee's arguments are assembled directly from their sources
    // (Stub::CallVarargs, Stub::CallList). An array that would be built only to be copied from immediately before the call is
    // elided (Node::isElided). So is a rest array or arguments object with no other use, since it only duplicates this function's
    // own arguments.
    void findListsOfArguments();
    // The argument list, if the node is a call that takes one and passes all of it.
    static Node* listOfArgumentsOf(const Node*);
    // A link-time constant: a value that only builtins can name and that user code cannot replace.
    static std::optional<LinkTimeConstant> linkTimeConstantOf(const Node*);
    // For an op_get_from_scope or op_put_to_scope: the distance below the Instance of the environment that holds the variable, if
    // it is a module environment with a fixed location (ImageEnvironment::distance). The instruction's scope operand is then
    // unused.
    std::optional<uint32_t> distanceOfEnvironmentAccessed(const Node*);
    std::optional<uint32_t> distanceOfEnvironmentResolvedTo(const Node*); // The same for an op_resolve_scope.
    static bool isThatManyScopesOut(const Node* scope, unsigned hops);
    // A scope passed as `this` in a call to a variable found in it. Calls to variables are compiled that way in case the scope is a
    // with scope. This one is not, so the callee sees undefined or the global `this`.
    bool isScopeThatStandsForNoThis(const Node*);
    void setCalleeHints(const CalleeHints* hints) { m_hints = hints; }
    // The interprocedural summary for this function's call code block.
    void setSummary(const FunctionSummary* summary) { m_summary = summary; }
    const FunctionSummary* summary() const { return m_summary; }
    // reader: the unit to reanalyze if a variable it read later widens (VariableSummaries::read()).
    void setVariableSummaries(VariableSummaries* summaries, unsigned reader = VariableSummaries::nobody)
    {
        m_variableSummaries = summaries;
        m_summaryReader = reader;
    }
    VariableSummaries* variableSummaries() const { return m_variableSummaries; }
    // With Options::logAOTTypeInference(): the name to use for this code in the log.
    void setNameForLog(const String& name) { m_nameForLog = name; }
    const String& nameForLog() const { return m_nameForLog; }
    unsigned summaryReader() const { return m_summaryReader; }
    // Which source scope the value is an environment record for (Variable::scope). Null if unknown.
    const void* identityOfScope(const Node*, unsigned depth = 0);
    // For op_get_from_scope and op_put_to_scope. Empty if the variable is not in an environment record or cannot be identified.
    Variable variableAccessedBy(const Node*);
    // Collects what must be known about variables before their types are tracked. See VariableSummaries.
    void recordUntrackableVariableAccesses(VariableSummaries&);
    Type typeOfArgumentOnEntry(unsigned indexIncludingThis) const
    {
        if (!m_summary || !m_summary->isNonEscaping || indexIncludingThis >= FunctionSummary::mostParameters)
            return TTop;
        if (!indexIncludingThis)
            return m_summary->thisType.load();
        return m_summary->parameterTypes[indexIncludingThis].load();
    }
    const CalleeHints* calleeHints() const { return m_hints; }
    unsigned indexOfKnownCallee(const ImageKey&);
    Vector<ImageKey> knownCallees;
    bool callsItself { false }; // Treated like a loop.
    unsigned numberOfRegisterReturnValues { 0 }; // Number of values this function returns in registers. Zero for an ordinary return.
    UncheckedKeyHashMap<Node*, Vector<Node*, 8>> returnValueReads; // Keyed by the call. See Node::returnValueIndex.
    bool makesCalls { false }; // Calls functions that have their own frames.
    bool emitsCalls { false }; // The generated code may contain a call instruction, if only to a stub. A jump does not count.
    bool alwaysEmitsCalls { false }; // It does, in a way that inspecting the generated code cannot detect.
    mutable std::optional<bool> callsAreLeft; // Cache for hasNoFrame().
    // Integer constants in the program that are large enough to look like addresses (see the check in AOTCompiler.cpp).
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
    Vector<BasicBlock*> genericTargetForOffset; // If non-null, where a jump from the generic copy of a loop goes instead.
    Vector<BasicBlock*> headerForOffset; // Where a back edge in the fast copy of a loop goes. blockForOffset holds the pre-header.
    UncheckedKeyHashSet<uint64_t, WTF::IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> jumpsBack; // (target offset << 32) | end offset of the jumping block.
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
            Closure, // In the scope `depth` levels out, at `offset`.
            Import, // An import of the module whose environment is `depth` levels out, known to be at `offset` in the environment held in that module's `import.slot`.
            ModuleImport, // An import of the module whose environment is `depth` levels out. Its binding is resolved when it is first read.
            Unresolved, // Known only not to be in a scope that eval or with can extend.
            Dynamic,
        } kind { Dynamic };
        unsigned depth { 0 };
        ScopeOffset offset;
        bool inModule { false };
        bool isReadOnly { false };
        bool isInGlobalScopes { false }; // Unresolved: no scope enclosing the function declares it.
        bool isGlobal { false }; // The same, however it was established.
        bool isInOutermostEnvironment { false }; // Closure: a top-level variable of the module.
        StaticImport import;

        // op_resolve_scope: the result is `depth` scopes out. For an unlinked import, that is the importing module.
        bool isAtStaticDepth() const { return kind == Closure || kind == ModuleImport; }
        // op_get_from_scope: the location is looked up once and cached in a Slot.
        bool isCachedInSlot() const { return kind == Unresolved || kind == ModuleImport; }
    };
    StaticVariable resolveStatically(unsigned identifierIndex, unsigned localScopeDepth, ResolveType);
    // The extra bits passed to the operations through the Site, besides the identifier.
    unsigned extraOfResolveScope(const OpResolveScope&);
    unsigned extraOfGetFromScope(const OpGetFromScope&);
    void setLinkage(const ModuleLinkage*, const DeclaredNamesLink*);
    bool usesStaticImports { false }; // The code is only valid for a module linked as its ModuleLinkage describes.
    bool startsCold { false }; // CompiledFunctionInfo::startsCold
    bool m_needsFunctionObject { true };
    bool m_scopeIsEnvironmentOfModule { false };
    BitVector m_homed;
    Vector<Type> homedTypes; // Indexed by registerIndex(): the union of everything stored to each frame register.
    unsigned numICSlots { 0 };
    Vector<Site> sites; // For slots that have one. The rest are blank or past the end.
    // See the fields of the same names in CompiledFunctionInfo.
    Vector<uint32_t> siteConstants;
    Vector<UniquedStringImpl*> selectors;
    Vector<KnownShape> shapes;
    void noteSelectorOfSite(unsigned slot, UniquedStringImpl*);
    void noteShapeOfSite(unsigned slot, KnownShape&&);
    Vector<uint32_t> quotableSites; // CompiledFunctionInfo::quotableSites
    Vector<uint32_t> callSites; // CompiledFunctionInfo::callSites
    Vector<SiteOfSpread> sitesOfSpreads; // CompiledFunctionInfo::sitesOfSpreads
    Vector<uint32_t> plans;
    void notePlanOfSite(unsigned firstSlot, Vector<uint32_t, 16>&& words); // See AllocationPlan.
    // For an op_new_object that is allocated fully initialized (Node::numberOfLiteralProperties): its shape. Nullopt if the shape
    // cannot be numbered.
    std::optional<KnownShape> shapeOfLiteral(const Node*) const;
    StubCalls stubCalls;
    IndexReferences indexReferences;

private:
    VM& m_vm;
    UnlinkedCodeBlock* m_codeBlock;
    ScopeChain m_scopeChain;
    const CalleeHints* m_hints { nullptr };
    const FunctionSummary* m_summary { nullptr };
    VariableSummaries* m_variableSummaries { nullptr };
    String m_nameForLog;
    unsigned m_summaryReader { VariableSummaries::nobody };
    UncheckedKeyHashMap<int, Vector<Node*>, WTF::IntHash<int>, WTF::UnsignedWithZeroKeyHashTraits<int>> m_storesToFrameRegisters; // See identityOfScope().
    bool m_hasStoresToFrameRegisters { false };
    const ModuleLinkage* m_linkage { nullptr };
    const DeclaredNamesLink* m_declaredNames { nullptr };
    BitVector m_namesAssignedTo; // By identifier index: assigned by an op_put_to_scope that resolves by name.
    UncheckedKeyHashMap<unsigned, uint32_t, WTF::IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_typeTags; // By bytecode offset.
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
    unsigned m_numberOfInlinedNodes { 0 };
    Vector<std::unique_ptr<Graph>> m_inlinees;
};

// The users of each node's value.
class UsersOfNodes {
public:
    explicit UsersOfNodes(Graph&);
    std::span<Node* const> of(Node* node) const
    {
        auto it = m_users.find(node);
        return it == m_users.end() ? std::span<Node* const> { } : it->value.span();
    }
    // The uses of a freshly allocated object with plain properties of these names, if it is only ever read.
    struct OnlyRead {
        Vector<Node*, 4> handedOn; // The object and its aliases (checks it passed through, and so on).
        Vector<std::pair<Node*, unsigned>, 8> reads; // Each op_get_by_id and the index of the name it reads.
        Vector<Node*, 2> tests; // Null or undefined checks, which are always false.
    };
    // layoutID: the object's typed layout, or zero.
    std::optional<OnlyRead> isOnlyRead(Node* object, std::span<UniquedStringImpl* const> names, uint16_t layoutID) const;

private:
    UncheckedKeyHashMap<Node*, Vector<Node*, 2>> m_users;
};

// Phases. Each returns false (with the reason in Graph::failureReason()) if the function cannot be compiled ahead of time.
bool parseBytecode(Graph&);
// Returns the function's return type.
// calleesRead: the functions whose KnownFunction::returnType was used.
// calleesWithWidenedInputs: the non-escaping callees whose FunctionSummary::parameterTypes this function widened.
Type inferTypes(Graph&, Vector<const KnownFunction*>* calleesRead = nullptr, Vector<const KnownFunction*>* calleesWithWidenedInputs = nullptr);
void inferRanges(Graph&);
void optimizeLoops(Graph&);
void simplify(Graph&);
void inlineCalls(Graph&, const CodeOfProgram&);
void analyzeEscapes(Graph&); // Sets Node::escape.
void promoteEnvironments(Graph&); // Sets Node::isPromoted.
void scalarReplaceReadOnlyObjects(Graph&); // Runs before type inference.
void recordReturnedLiterals(Graph&); // Feeds MultiValueReturnTable::note(). Runs on the code before any transformation.
void planMultiValueReturns(Graph&); // Runs after type inference and before anything depends on the types.
// Computes FunctionSummary::escapingParameters from the current summaries of its callees (calleesRead).
uint32_t escapingParameters(Graph&, Vector<const KnownFunction*>* calleesRead);

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
