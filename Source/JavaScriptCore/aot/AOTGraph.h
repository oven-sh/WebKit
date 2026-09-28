/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(FTL_JIT)

#include "AOTProgram.h"
#include "AOTStubs.h"
#include "AOTType.h"
#include "BytecodeIndex.h"
#include "BytecodeStructs.h"
#include "CallFrame.h"
#include "Instruction.h"
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
    ConstantCell, // A constant register that holds a cell or a link time constant: loaded from the CodeBlock.
    Intrinsic, // One of the realm's ImmutableIntrinsics that is a cell: loaded from the Instance.
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

struct Use {
    VirtualRegister reg;
    Node* node { nullptr };
};

struct Node {
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
    unsigned expectedMask { 0 }; // op_get_by_val: the mask of the op_check_type that what it gets goes to next, if it does.
    GuardKind guardKind { GuardKind::Whole };
    uint16_t intrinsic { 0 }; // NodeKind::Intrinsic: its number.
    bool structureIsChecked { false }; // A guard of a property access: another guard has seen to the base's structure.
    bool slotIsPlain { false }; // Likewise: another guard has seen to that.
    bool calleeIsChecked { false };
    bool isElided { false }; // Nothing wants its value, and getting that does nothing else. It is not lowered.
    Node* site { nullptr }; // GuardKind::Structure, SlotsAgree: guards of property accesses.
    Node* otherSite { nullptr };
    // op_new_object: how many of Graph::storesOfLiteral() are part of it. Their values are the uses at NewObjectPlan::registerOf().
    // op_create_this: how many properties the object is made with (NewObjectPlan). Their values are the uses at NewObjectPlan::registerOf().
    unsigned numberOfLiteralProperties { 0 };
    Node* storage { nullptr }; // A guard of an access to an element of a typed array: the GuardKind::TypedArrayStorage that goes for it.

    // Lowering state.
    B3::Value* lowered { nullptr };
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
    Vector<Node*> valuesAtTail; // Indexed by Graph::registerIndex().

    B3::BasicBlock* lowered { nullptr };
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
    bool calleeIsProven(const Node*) const; // See KnownFunction::isProven.
    const KnownFunction* knownFunctionReadBy(const Node* getFromScope, bool* isProven = nullptr) const;
    // A call of a function that wants nothing of the object it is called as (KnownFunction::needsNoFunctionObject). None is passed.
    bool passesNoFunctionObject(const Node* call);
    // Whether this function is one of those, and whether its scope is where the module's environment is.
    bool needsFunctionObject() const { return m_needsFunctionObject; }
    bool scopeIsEnvironmentOfModule() const { return m_scopeIsEnvironmentOfModule; }
    uint32_t distanceOfEnvironmentOfModule();
    // What is read only to be called, by calls that do not pass it, is not read (Node::isElided).
    void elideReadsOfCalleesNotPassed();
    // An op_get_from_scope or an op_put_to_scope: how far below the Instance the environment is that has the variable, if it is
    // one of a module, whose place is known (ImageEnvironment::distance). Then the scope that the instruction names is not needed.
    std::optional<uint32_t> distanceOfEnvironmentAccessed(const Node*);
    std::optional<uint32_t> distanceOfEnvironmentResolvedTo(const Node*); // Likewise, an op_resolve_scope.
    static bool isThatManyScopesOut(const Node* scope, unsigned hops);
    // A scope that stands for `this` in a call of what was found in it, which is how a call of a variable is made in case the scope
    // is that of a `with`: and this one is not, so that whoever is called makes undefined of it, or the global `this`.
    bool isScopeThatStandsForNoThis(const Node*);
    void setCalleeHints(const CalleeHints* hints) { m_hints = hints; }
    const CalleeHints* calleeHints() const { return m_hints; }
    unsigned indexOfKnownCallee(const ImageKey&);
    Vector<ImageKey> knownCallees;
    bool callsItself { false }; // As good as a loop.
    bool makesCalls { false }; // Of functions, in frames of their own.
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
    // An op_new_object that is made whole (Node::numberOfLiteralProperties): what with. Nothing, if it is not a shape to be known by.
    std::optional<KnownShape> shapeOfLiteral(const Node*) const;
    StubCalls stubCalls;
    HeaderReferences headerReferences;
    B3::Air::StackSlot* calleeSlot { nullptr }; // Where the object the function was called as is kept: see CodeHeader::calleeSlot.

private:
    VM& m_vm;
    UnlinkedCodeBlock* m_codeBlock;
    ScopeChain m_scopeChain;
    const CalleeHints* m_hints { nullptr };
    const ModuleLinkage* m_linkage { nullptr };
    const DeclaredNamesLink* m_declaredNames { nullptr };
    BitVector m_namesAssignedTo; // By index of the identifier: op_put_to_scope by name.
    unsigned m_numArguments;
    unsigned m_numLocals;
    SegmentedVector<Node, 32> m_nodes;
    Node* m_emptyConstant { nullptr };
    UncheckedKeyHashMap<EncodedJSValue, Node*, EncodedJSValueHash, EncodedJSValueHashTraits> m_constants;
    UncheckedKeyHashMap<unsigned, Node*, DefaultHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_intrinsics;
    ASCIILiteral m_failureReason;
    OpcodeID m_failureOpcode { op_nop };
};

// Phases. Each returns false (and Graph::failed() says why) if the function is not for the static compiler.
bool parseBytecode(Graph&);
// What the function returns. calleesConsulted: the functions whose KnownFunction::returnType that went by.
Type inferTypes(Graph&, Vector<const KnownFunction*>* calleesConsulted = nullptr);
void inferRanges(Graph&);
void optimizeLoops(Graph&);
void simplify(Graph&);

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
