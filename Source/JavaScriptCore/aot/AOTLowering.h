/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltins.h"
#include "B3MemoryValue.h"
#include "AOTEmitter.h"
#include "AOTGraph.h"
#include "AOTTypeTable.h"
#include "AOTRuntime.h"
#include "B3AbstractHeapRepository.h"
#include "B3Procedure.h"
#include "B3Variable.h"
#include "B3VariableValue.h"
#include "FTLAbbreviatedTypes.h"
#include "FTLOutput.h"
#include "FTLValueFromBlock.h"
#include "FTLWeightedTarget.h"
#include "ScopeOffset.h"

namespace JSC { namespace AOT {

// Lowers the AOT IR to B3. Implemented in AOTLowerCore.cpp (control flow, values, calls into C++), AOTLowerArith.cpp,
// AOTLowerAccess.cpp (properties and scopes), AOTLowerObjects.cpp (allocation, conversions, less common property accesses),
// AOTLowerIteration.cpp (for-of and for-in), AOTLowerCalls.cpp and AOTLowerVarargs.cpp (all calls other than plain ones).

class Lowering : public Emitter {
    WTF_MAKE_NONCOPYABLE(Lowering);
public:
    Lowering(Graph&, B3::Procedure&);

    bool run();

    // Relative frequency of a block for a rare case, compared with its common sibling (see estimateFrequencies()).
    static constexpr double coldFrequency = 1.0 / 1024;

private:
    friend struct LoweringAccess;

    // AOTLowerCore.cpp
    void lowerBlock(BasicBlock*);
    void lowerNode(Node*);
    void lowerBytecode(Node*);
    void lowerTerminalOrFallThrough(BasicBlock*, Node* terminal);
    template<typename Conditional, typename IsUndefinedOrNull, typename EqualsNull>
    void lowerTerminal(BasicBlock*, Node*, const Conditional&, const IsUndefinedOrNull&, const EqualsNull&);
    LValue isUndefinedOrNull(Node*);
    LValue equalsNull(Node*, bool nullCounts);
    void emitUpsilons(BasicBlock*, BasicBlock* successor);
    LBasicBlock edgeTo(BasicBlock* successor);
    void lowerCatch(Node*);
    void lowerSwitch(Node*);
    void lowerMisc(Node*);
    bool tryLowerMisc(Node*);

    LBasicBlock blockFor(Node* branch, int relativeOffset);
    // See Node::isPromoted.
    B3::Variable* variableOfEnvironment(Node* environment, unsigned offset);
    LValue ancestorScope(Node* scope, unsigned hops);
    UncheckedKeyHashMap<Node*, Vector<B3::Variable*>> m_variablesOfEnvironments;
    // See BasicBlock::arraysViewed.
    struct ArrayView {
        LValue butterfly { nullptr };
        LValue length { nullptr }; // Int64.
        LValue limit { nullptr }; // Int64. Indices below this hold a JSValue in the butterfly. Null if the array does not store its elements that way.
    };
    const ArrayView* viewOf(Node* access, Node* base);
    void hoistArrayStorageLoadsAheadOf(BasicBlock*);
    LBasicBlock entryBlockFor(BasicBlock* successor);
    Vector<std::tuple<BasicBlock*, Node*, ArrayView>, 4> m_arrayViews;
    void unsupported(Node*);

    // Values.
    LValue lowRaw(Node*);
    LValue lowJSValue(Node*);
    LValue lowInt32(Node*);
    LValue lowInt64(Node*); // The node must be an integer of either width.
    LValue lowDouble(Node*);
    LValue lowBoolean(Node*);
    LValue lowCell(Node* node) { return lowJSValue(node); }
    LValue lowAs(Node* node, Rep rep)
    {
        // The representation of a parameter or result is derived from the union of everything passed or returned there, so the
        // node's type must be compatible with it. If not, the interprocedural summary is wrong.
        Rep from = node->rep();
        bool canBe = rep == Rep::JSValue || from == Rep::JSValue || from == rep || (from != Rep::Boolean && rep != Rep::Boolean);
        if (!canBe) [[unlikely]] {
            dataLog("AOT: incompatible representation for a parameter or result: ");
            node->dump(WTF::dataFile());
            dataLogLn(" has rep ", static_cast<unsigned>(from), ", expected rep ", static_cast<unsigned>(rep), " at bc#", m_node ? m_node->bytecodeIndex.offset() : 0, " ", m_node ? opcodeNames[m_node->opcode] : "", " in ", m_graph.nameForLog());
            RELEASE_ASSERT_NOT_REACHED();
        }
        // A node that is considered unreachable has the empty type and is represented as a JSValue.
        return rep == Rep::JSValue ? lowJSValue(node) : convert(lowRaw(node), from, node->type, rep);
    }
    LValue lowJSValueOfParameterOnEntry(unsigned index)
    {
        switch (m_valueRepresentations.parameters[index]) {
        case Rep::Int32:
            return boxInt32(m_out.castToInt32(registerOnEntry(argumentGPR(index))));
        case Rep::Boolean:
            return boxBoolean(m_out.castToInt32(registerOnEntry(argumentGPR(index))));
        case Rep::Double:
            return boxDouble(registerOnEntry(FPRInfo::toArgumentRegister(index)));
        default:
            return registerOnEntry(argumentGPR(index));
        }
    }
    LValue environmentAt(uint32_t distance)
    {
        LValue result = m_out.loadPtr(m_out.address(m_heaps.root, m_instance, -static_cast<ptrdiff_t>(distance)));
        static_cast<B3::MemoryValue*>(result)->setReadsMutability(B3::Mutability::Immutable);
        return result;
    }
    // The function object this function was called as.
    LValue callee()
    {
        RELEASE_ASSERT(m_calleeSlot);
        return m_out.load64(m_out.address(m_heaps.variables.atAnyIndex(), m_calleeSlot));
    }
    LValue lowConstantRegister(VirtualRegister reg) { return lowConstantRegister(code(), reg); } // For an operand that BytecodeUseDef does not report as a use.
    LValue lowConstantRegister(Graph&, VirtualRegister);
    LValue constantThroughStub(uint32_t number, Stub);
    uint32_t numberOfConstantOfProgram(Node*); // Of a NodeKind::ConstantCell.
    // The graph that the node being lowered came from (Node::graph), which differs from m_graph for inlined code. Output always
    // goes to m_graph.
    Graph& code() { return m_code ? *m_code : m_graph; }
    LValue convert(LValue, Rep from, Type fromType, Rep to);
    void setJSValue(Node*, LValue);
    void setInt32(Node*, LValue);
    void setInt64(Node*, LValue);
    void setDouble(Node*, LValue);
    void setBoolean(Node*, LValue);
    void setResult(Node*, LValue, Rep);
    // For an instruction that defines several registers: sets the value of one of them.
    void setProj(Node*, VirtualRegister, LValue, Rep = Rep::JSValue);

    LValue doubleToInt32(LValue); // ToInt32.
    LValue toBoolean(Node*);

    TypedPointer addressFor(VirtualRegister); // The address of a register that lives in the frame (Graph::livesInFrame()).
    LValue wordByIndex(LValue base, uint32_t addend, uint32_t scale, bool mayChange);
    void lowerEntry();
    // For a function with Signature::List.
    LValue numberOfArgumentsPassed(); // Excluding `this`.
    LValue argumentsPassed(); // Address of the first.
    LValue argumentPassedOrUndefined(unsigned index);
    // What the static type of the access's base says about the property (TypeTable).
    std::optional<TypeTable::Field> fieldAccessedBy(Node*, unsigned identifier);
    // The cell's layout number (Structure::knownShape()). Use isOneOf() to test it against a range.
    LValue layoutOf(LValue cell);
    LValue loadTypedLayoutID(LValue cell); // Structure::typedLayoutID()
    LValue loadTypedLayoutIDOrZero(Node*, LValue);
    TypedPointer addressOfField(LValue object, const TypeTable::Field&);
    LValue toFieldRepresentation(Node* valueNode, LValue value, TypeTable::FieldType); // The value as stored in a typed field: numbers are encoded as doubles.
    Node* m_sameAs { nullptr }; // For setResult(): the node is an alias of this one.
    void checkTypedLayout(Node* onBehalfOf, Node* valueNode, LValue value, uint16_t layoutID); // For a closed layout. Execution continues only if the value has the layout.
    // The object whose slots typed code reads and writes. For an open layout this may be an empty placeholder object, standing in
    // for a value that does not have the layout and cannot be converted to it. An access that finds an empty slot when `pointer` is
    // not the original object must take the slow path.
    struct FieldStorage {
        LValue pointer;
        bool mayBeStandIn;
    };
    FieldStorage fieldStorageFor(Node* onBehalfOf, Node* baseNode, LValue base, uint16_t layoutID);
    LValue coerceToTypedLayout(Node* onBehalfOf, Node* valueNode, LValue value, uint16_t layoutID);
    LValue cachedCoercionFor(Node* valueNode, uint16_t layoutID); // Null if the value has not been coerced yet.
    UncheckedKeyHashMap<Node*, LValue> m_coercions; // Keyed by op_type_tag node.
    UncheckedKeyHashMap<Node*, std::pair<BasicBlock*, LValue>> m_loadedLayoutIDs; // The loaded layout ID, and the block it was loaded in.
    bool branchUnlessAccepted(Node* valueNode, LValue value, TypeTable::FieldType, LBasicBlock otherwise); // Returns false if the value is always accepted, in which case nothing branches to `otherwise`.
    // The known value of a field of an object with a closed layout, from a previous read or write. Valid until something executes
    // that could change it. It carries through the rest of the block and into blocks that are only reachable from it.
    struct AvailableField {
        Node* base;
        uint16_t layoutID;
        uint16_t slot;
        uint16_t id; // TypeTable::Field::id. With field IDs, several fields may share a slot.
        Rep rep;
        LValue value;
        LValue asJSValue; // Null until boxed.
    };
    Vector<AvailableField> m_availableFields;
    UncheckedKeyHashMap<BasicBlock*, Vector<AvailableField>> m_availableFieldsAtEndOf;
    bool m_nodePreservesFields { false }; // Set while lowering a node, to override preservesFields().
    const AvailableField* availableField(Node* base, const TypeTable::Field&) const;
    void recordAvailableField(Node* base, const TypeTable::Field&, LValue value, Rep, LValue asJSValue, bool isWritten);
    static bool preservesFields(Node*);
    // The node's value, if it is an 8-bit string literal.
    static std::optional<String> constantStringOf(Node*);
    // If the value is a string, it is an atom: either a literal, or read from a field with atomized strings
    // (TypeTable::FieldType::atoms).
    static bool isAtomIfString(Node*, unsigned depth = 0);
    static bool isAtomIfShortString(Node*, unsigned depth = 0); // Or longer than TypedLayoutTable::maxLengthOfAtomizedString.
    LValue areEqualAssumingAtomStrings(Node* left, LValue, Node* right, LValue); // Neither operand is a number or a BigInt.
    void atomizeIfString(Node*, LValue);
    // `this` in a function that may escape.
    static bool isThisOfEscapingFunction(Node*);
    LValue isStringEqualTo(Node* comparison, Node* valueNode, LValue value, const String&, Node* theString);
    LValue isStringEqualToAtom(Node* valueNode, LValue value, LValue theString);
    // With Options::useAOTTypedFields(): checks a newly allocated object with this layout and these initial slot values (null for
    // empty), and leaves no slot holding a value that its field type rejects.
    void validateNewObject(Node*, LValue object, uint32_t layout, const Vector<Node*, 8>& inSlots, const Vector<LValue, 8>& values, const Vector<TypeTable::FieldType, 8>* fieldTypesIfKnown = nullptr);
    void guardField(Node* guard);
    LValue isOneOf(LValue layout, uint16_t first, uint16_t last);
    // This function's entry in Instance::states: whether it has its own Data yet, and the Data's address if so.
    struct OwnData {
        LValue hasAny;
        LValue data;
    };
    OwnData ownData();
    TypedPointer slotWord(unsigned slot, unsigned word); // Word 0: structureID and offset. Word 1: pointer.
    LValue slotAddress(unsigned slot);
    unsigned allocateSlot() { return m_graph.numICSlots++; }
    // The call site bits that the frame reports while the node's operation runs elsewhere.
    uint32_t callSiteBitsOf(Node*);
    uint32_t siteOf(Node*); // The same, for a location that is already registered as a call site or will never be queried.
    // For an operation that takes an index into bytecode tables: identifies whose bytecode the index refers to. See
    // bytecodeOwnerOfCaller().
    uint32_t whoseBytecode(Node* node) { return node->graph->isOutermost() ? 0 : m_graph.inlineFrames[node->graph->inlineFrame()].knownCallee + 1; }
    unsigned allocateSlots(unsigned count)
    {
        unsigned first = m_graph.numICSlots;
        m_graph.numICSlots += count;
        return first;
    }
    // Scratch words in the frame, used to pass lists of values to operations and to receive additional results. Nothing is kept
    // there across operations.
    TypedPointer scratchWord(unsigned index);
    LValue scratchAddress() { return m_scratch; }
    LValue storeToScratch(Node*, VirtualRegister first, unsigned count); // Stores the registers first, first - 1, ... in that order.
    LValue isSentinelCell(LValue cell) { return isCellOfType(cell, SentinelType); }
    template<typename Functor> LValue isCellAnd(Node*, LValue jsValue, const Functor&); // False for a non-cell.

    // Calls into C++.
    // For an operation declared with JSC_DECLARE_JIT_OPERATION. Checks for an exception and returns the result.
    template<typename... Args> LValue vmCall(Node*, LType, Entry, Args...);
    // For an operation declared NOEXCEPT.
    template<typename... Args> LValue plainCall(LType, Entry, Args...);
    void storeBarrier(LValue owner);
    // Whether the value being lowered is used by anything other than the next node. Nearly every node is a call, so a value that
    // lives longer belongs in a callee-saved register.
    bool isLiveAfterNextNode(Node*) const;

    // Calls to stubs (AOTStubs.h).
    struct StubArgument {
        LValue value;
        Reg reg;
    };
    struct StubImmediate {
        GPRReg reg;
        uint32_t value;
    };
    enum class StubClobbers : uint8_t { CallerSavedRegisters, Temporaries, Nothing };
    // place: the node whose location the function reports during the call, if not the one being lowered.
    B3::PatchpointValue* callStub(Stub, LType, const Vector<StubArgument, 8>&, const Vector<StubImmediate, 2>&, StubClobbers = StubClobbers::CallerSavedRegisters, Node* place = nullptr);
    LValue callOperationThroughStub(Node*, LType, Entry, const Vector<LValue, 8>& arguments); // A null node means the operation does not throw.
    LValue contextOf(Entry operation) const { return takesInstance(operation) ? m_instance : m_globalObject; }
    // Calls a helper (generateHelper()). Returns null if the helper bailed out.
    LValue callHelper(Stub, const Vector<LValue, 4>& arguments);
    // The same, falling back to `slow` if the helper bailed out.
    template<typename Slow> LValue withHelper(Stub, const Vector<LValue, 4>& arguments, const Slow&);
    // Calls an operation that takes the global object and these arguments and returns nothing, from a rarely executed path. The
    // call does not force the function to have a frame or to move values out of their registers.
    // ChangesNothing: the operation only reads state or throws, so previously loaded values stay valid.
    enum class ColdCall : uint8_t { MayDoAnything, ChangesNothing };
    void coldCall(Node*, Entry, LValue first = nullptr, LValue second = nullptr, ColdCall = ColdCall::MayDoAnything);
    LValue coldCallForValue(Node*, Entry, LValue first, LValue second = nullptr, ColdCall = ColdCall::MayDoAnything);
    B3::PatchpointValue* emitColdCall(Node*, LType, Entry, LValue first, LValue second, ColdCall);
    LValue callBinaryStub(Node*, Stub, LType, LValue, LValue);
    // Whether a program class that extends this built-in class may override the property being read (BUN_AOT_OVERRIDDEN_METHODS).
    static bool mayBeOverridden(ASCIILiteral nameOfClass, Node* read);
    // Whether an operation that has a stub calls it instead of emitting its fast path inline. Inline fast paths are faster in hot
    // code (25-40% on small kernels) but larger everywhere (about 10% of the code of a large program that spends little time in
    // loops). The only hotness information available ahead of time is whether code is in a loop.
    bool isCompact() const
    {
        if (!Options::useAOTInlineFastPathsInLoops() || m_block->isGeneric)
            return true;
        return (!m_block->isInLoop || m_block->isOnlyInLoopOfBuiltin) && !m_graph.callsItself;
    }
    LValue compareWithLiteral(LValue characters, std::span<const Latin1Character> written);
    // The characters and length of a string, if it is 8-bit and its characters are directly accessible: either resolved, or a
    // substring of a resolved string (which stays a substring). Otherwise branches to `otherwise`, still providing the length.
    struct NarrowCharacters {
        LValue characters;
        LValue length;
    };
    NarrowCharacters narrowCharactersOf(LValue string, LBasicBlock otherwise, Vector<ValueFromBlock, 2>& lengthOtherwise);
    // Determines which of several string literals a value equals, if any: first by length, then by content, a word at a time.
    struct StringCase {
        const StringImpl* string;
        LBasicBlock target;
        Node* constant; // May be null.
    };
    void dispatchOnString(Node* place, Node* scrutinee, LValue value, Vector<StringCase, 16>&, LBasicBlock defaultBlock, bool isKnownToBeCell = false);
    // A run of blocks that only compare one value against a series of constants with ===, each falling through to the next on a
    // mismatch:
    //     if (x === "a") ... else if (x === "b") ... else if (x === 3) ...        x === "a" || x === "b" || x === "c"
    // The first block dispatches on the value once, and the remaining blocks become unreachable.
    struct ArmOfChain {
        BasicBlock* block;
        Node* constant;
        BasicBlock* target;
    };
    struct ChainOfComparisons {
        Node* value { nullptr };
        Vector<ArmOfChain, 8> arms;
        BasicBlock* otherwise { nullptr };
    };
    Vector<ChainOfComparisons> m_chains;
    UncheckedKeyHashMap<BasicBlock*, unsigned> m_chainsByFirstBlock;
    UncheckedKeyHashSet<BasicBlock*> m_blocksInsideChains;
    void findChainsOfComparisons();
    void lowerChainOfComparisons(BasicBlock*, const ChainOfComparisons&);
    // An op_resolve_scope whose only use is the op_get_from_scope that follows it. The two are lowered as one call.
    bool isFusedWithGetFromScope(Node*);
    LValue scopeToResolveFrom(Node* resolve);
    // The number that compiled code uses for one of the function's identifiers. See AOT::NumbersOfIdentifiers.
    unsigned numberOf(Graph& graph, unsigned identifier)
    {
        auto* numbers = numbersOfIdentifiersOfProgram();
        return numbers ? numbers->get(graph.codeBlock()->identifier(identifier).impl()) : identifier;
    }
    unsigned numberOf(unsigned identifier) { return numberOf(code(), identifier); } // For the code being lowered.
    unsigned numberOf(const Node* whose, unsigned identifier) { return numberOf(*whose->graph, identifier); }
    // Allocates a slot together with its Site, so that it can be passed to a stub.
    unsigned allocateSite(Node*, unsigned identifier, unsigned extra = 0);
    // The single site shared by both copies of an instruction in a split loop. What the generic copy caches, the fast copy uses.
    unsigned sharedSite(Node*, unsigned identifier, unsigned extra = 0);

    // AOTLowerArith.cpp
    bool tryLowerArith(Node*);
    void lowerBinaryArith(Node*, VirtualRegister lhs, VirtualRegister rhs);
    void lowerBitOp(Node*, VirtualRegister lhs, VirtualRegister rhs);
    void lowerUnaryArith(Node*, VirtualRegister operand);
    LValue lowerCompare(Node*, OpcodeID canonical, VirtualRegister lhs, VirtualRegister rhs); // Returns a boolean.
    LValue lowerEquality(Node*, bool strict, VirtualRegister lhs, VirtualRegister rhs);
    LValue toInt32ForBitOp(Node* operand);

    // AOTLowerAccess.cpp
    bool tryLowerAccess(Node*);
    void lowerGetById(Node*);
    void lowerPutById(Node*);
    void lowerGetByVal(Node*);
    void lowerPutByVal(Node*);
    void lowerResolveScope(Node*);
    void lowerGetFromScope(Node*);
    void lowerPutToScope(Node*);
    LValue loadProperty(LValue object, LValue offset);
    using StaticVariable = Graph::StaticVariable;
    StaticVariable resolveStatically(unsigned identifierIndex, unsigned localScopeDepth, ResolveType type) { return code().resolveStatically(identifierIndex, localScopeDepth, type); }

    TypedPointer cachedPropertyAddress(LValue object, LValue firstSlotWord, const B3::AbstractHeap* = nullptr);
    LValue getByIdCached(Node*, LValue base, Type baseType, Entry operation, unsigned identifier);
    LValue lowIndex(Node* property, LBasicBlock haveIndex, LBasicBlock notIndex);

    // AOTLowerGuards.cpp
    void lowerGuard(BasicBlock*, Node*);
    void emitGuard(Node*);
    unsigned slotOfPropertyGuard(Node*);
    LValue loadSlotWord(unsigned slot, unsigned word);
    void checkStructure(Node* baseNode, LValue base, LValue firstSlotWord);
    void checkCallee(Node*);
    void lowerGuarded(Node*);
    LBasicBlock newColdBlock();
    LValue trapBits();
    void exitUnless(LValue condition);
    void exitUnlessOfType(LValue jsValue, Type from, Type wanted);
    void guardReentry(BasicBlock*);
    void guardGetById(Node*);
    void guardPutById(Node*);
    void guardGetByVal(Node*);
    void guardPutByVal(Node*);
    // The element's address, after a bounds check.
    TypedPointer elementOfTypedArray(Node* guard, Node* base, Node* property, JSType);
    void guardGetLength(Node*);
    void guardCheckType(Node*);
    // These return false if no guard is needed or none is possible.
    bool guardResolveScope(Node*);
    bool guardGetFromScope(Node*);
    bool guardCall(Node*);
    // Branches to `passed` if the value's type is guaranteed to pass the check, and to `undecided` otherwise.
    void emitTypeTests(Node* value, LValue jsValue, unsigned mask, LBasicBlock passed, LBasicBlock undecided);
    void emitTypeTests(std::nullptr_t, Type typeOfValue, LValue jsValue, unsigned mask, LBasicBlock passed, LBasicBlock undecided);

    // AOTLowerObjects.cpp
    bool tryLowerObjects(Node*);
    bool tryLowerAllocation(Node*);
    bool tryLowerConversion(Node*);
    bool tryLowerPropertyVariant(Node*);
    // Allocates an object with the Structure cached in `slot` (fillAllocationCache()) and these initial property values. Branches
    // to `otherwise` if the cache is empty or the allocator has no free cell.
    LValue allocateObjectWithProperties(unsigned slot, const Vector<LValue, 8>& values, LBasicBlock otherwise);
    void lowerGetLength(Node*);
    void lowerToThis(Node*);
    void throwTDZError(Node*);
    void throwStaticError(Node*);

    // AOTLowerIteration.cpp
    bool tryLowerIteration(Node*);
    void lowerIteratorOpen(Node*, bool isAsync);
    void lowerIteratorNext(Node*);
    void lowerAsyncIteratorNext(Node*);
    void lowerIteratorCloseCheck(Node*);
    LValue iteratorCloseCheckCondition(Node*);
    void checkIsObjectOrThrowIteratorResultIsNotObject(Node*, LValue);
    LValue inlineWatchpointSetIsStillValid(LValue set);

    // AOTLowerCalls.cpp
    using Arguments = Vector<LValue, 8>; // The first is `this`, or new.target for a construct.
    enum class CallMode : uint8_t { Call, Construct, TailCall };
    bool tryLowerCall(Node*);
    Arguments lowerArguments(Node*, unsigned argc, unsigned argv);
    // Calls an arbitrary callee. A tail call terminates the block and has no result.
    LValue emitCall(Node*, LValue callee, const Arguments&, CallMode = CallMode::Call, StubIntrinsic = StubIntrinsic::None);
    void lowerCall(Node*, VirtualRegister callee, unsigned argc, unsigned argv, CallMode, bool hasResult);
    bool lowerCallToKnownFunction(Node*, VirtualRegister callee, unsigned argv, const Arguments&, CallMode, bool hasResult);
    void lowerCallVarargs(Node*, VirtualRegister callee, VirtualRegister thisValue, VirtualRegister arguments, int firstVarArg, CallMode);
    void lowerCallWithItems(Node*, Node* calleeNode, LValue callee, LValue thisValue, Node* list, CallMode);
    void lowerCallDirectEval(Node*);
    LValue storeArgumentsToScratch(const Arguments&); // Stores all but the first. Returns their address.
    void finishCall(B3::PatchpointValue*, CallMode, Rep result = Rep::JSValue);
    LBasicBlock branchIfCalleeIsFunction(Node* calleeNode, LValue callee);

    // AOTLowerBuiltins.cpp
    LValue isReceiverOfKind(Node* read, Node* baseNode, LValue base, Receiver);
    void lowerReadOfBuiltin(Node*, Node* baseNode);
    bool lowerCallOfBuiltin(Node*, Node* calleeNode, unsigned argc, unsigned argv, const Arguments&, bool hasResult, LBasicBlock& afterwards, Vector<ValueFromBlock, 2>& results);
    UncheckedKeyHashMap<Node*, LValue> m_receiverChecks; // Result of isReceiverOfKind(), keyed by the read of the callee.

    Graph& m_graph;

    ValueRepresentations m_valueRepresentations; // For this function.
    LValue m_dataOnEntry { nullptr }; // For a cold-start function: the value of m_data outside loops.
    B3::Variable* m_dataInLoops { nullptr }; // For a cold-start function with loops: the Data found at the top of the current loop iteration.
    LValue m_callFrame { nullptr };
    LValue m_data { nullptr };
    LValue m_dataOrNull { nullptr };
    LValue m_calleeSlot { nullptr };
    LValue m_listSlot { nullptr }; // Signature::List: the argument count and address.
    LValue m_frameRegisterStorage { nullptr };
    LValue m_scratch { nullptr };
    LBasicBlock m_returnBlock { nullptr };
    Vector<ValueFromBlock, 4> m_returnValues;
    // With Graph::numberOfRegisterReturnValues: each return value from every return site, and its representation.
    Vector<Vector<ValueFromBlock, 4>, 8> m_registerReturnValues;
    Vector<Rep, 8> m_returnValueReps;
    LBasicBlock m_exit { nullptr }; // While lowering a guard: the exit to the generic copy.
    Vector<std::pair<BasicBlock*, LBasicBlock>, 2> m_edges; // While lowering a branch: edge blocks for successors that have phis.
    LBasicBlock m_afterSlotChecks { nullptr };
    LValue m_slotEpoch { nullptr };
    unsigned m_slotOfSlotChecks { 0 };
    UncheckedKeyHashMap<uint64_t, unsigned, WTF::IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_sharedSites; // Keyed by bytecode offset and identifier.
    BasicBlock* m_block { nullptr };
    unsigned m_nodeIndex { 0 }; // Index in m_block of the node being lowered.
    Node* m_node { nullptr }; // The node being lowered, if it has a bytecode location.
    Graph* m_code { nullptr }; // See code().
};

template<typename... Args>
LValue Lowering::vmCall(Node* node, LType type, Entry function, Args... args)
{
    return callOperationThroughStub(node, type, function, { args... });
}

template<typename Slow>
LValue Lowering::withHelper(Stub stub, const Vector<LValue, 4>& arguments, const Slow& slow)
{
    // Compact code calls the operation directly, which has the helper in front of it (FOR_EACH_AOT_OPERATION_BEHIND_HELPER).
    if (isCompact())
        return slow();
    LValue helperResult = callHelper(stub, arguments);
    LBasicBlock otherwise = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    ValueFromBlock fastResult = m_out.anchor(helperResult);
    m_out.branch(m_out.notNull(helperResult), usually(continuation), rarely(otherwise));
    m_out.appendTo(otherwise);
    ValueFromBlock slowResult = m_out.anchor(slow());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return m_out.phi(B3::pointerType(), fastResult, slowResult);
}

template<typename Functor>
LValue Lowering::isCellAnd(Node* node, LValue value, const Functor& test)
{
    if (!mayBe(node->type, TCell))
        return m_out.booleanFalse;
    if (isSubtype(node->type, TCell))
        return test(value);
    LBasicBlock cellCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    ValueFromBlock notCellResult = m_out.anchor(m_out.booleanFalse);
    m_out.branch(isCell(value), unsure(cellCase), unsure(continuation));
    m_out.appendTo(cellCase, continuation);
    ValueFromBlock cellResult = m_out.anchor(test(value));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return m_out.phi(B3::Int32, notCellResult, cellResult);
}

template<typename... Args>
LValue Lowering::plainCall(LType type, Entry function, Args... args)
{
    return callOperationThroughStub(nullptr, type, function, { args... });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
