/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(FTL_JIT)

#include "AOTBuiltins.h"
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

// AOT IR -> B3. Implemented in AOTLowerCore.cpp (structure, values, calls into C++), AOTLowerArith.cpp, AOTLowerAccess.cpp
// (properties and scopes), AOTLowerObjects.cpp (allocation, conversions, the rarer kinds of property access),
// AOTLowerIteration.cpp (for-of and for-in), AOTLowerCalls.cpp and AOTLowerVarargs.cpp (every call that is not a plain one).

class Lowering : public Emitter {
    WTF_MAKE_NONCOPYABLE(Lowering);
public:
    Lowering(Graph&, B3::Procedure&);

    bool run();

    // Index of the B3 entrypoint of each catch block, in the order of Graph::catchEntrypoints; the first is 1.

    // Of a block that is there for what seldom happens, next to one that is not (see estimateFrequencies()).
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
    // Node::isPromoted
    B3::Variable* variableOfEnvironment(Node* environment, unsigned offset);
    LValue scopeThatIsOutFrom(Node* scope, unsigned hops);
    UncheckedKeyHashMap<Node*, Vector<B3::Variable*>> m_variablesOfEnvironments;
    // See BasicBlock::arraysViewed.
    struct ArrayView {
        LValue butterfly { nullptr };
        LValue length { nullptr }; // Int64.
        LValue limit { nullptr }; // Int64: below this, an element is a JSValue where the elements are. Nothing, if that is not how the array keeps them.
    };
    const ArrayView* viewOf(Node* access, Node* base);
    void viewArraysAheadOf(BasicBlock*);
    LBasicBlock wayInto(BasicBlock* successor);
    Vector<std::tuple<BasicBlock*, Node*, ArrayView>, 4> m_arrayViews;
    void unsupported(Node*);

    // Values.
    LValue lowRaw(Node*);
    LValue lowJSValue(Node*);
    LValue lowInt32(Node*);
    LValue lowInt64(Node*); // Of a node that is an integer of either size.
    LValue lowDouble(Node*);
    LValue lowBoolean(Node*);
    LValue lowCell(Node* node) { return lowJSValue(node); }
    LValue lowAs(Node* node, Rep rep)
    {
        // What is passed or returned is of a type that everything passed or returned there is of. If not, something is wrong with the facts.
        Rep from = node->rep();
        bool canBe = rep == Rep::JSValue || from == Rep::JSValue || from == rep || (from != Rep::Boolean && rep != Rep::Boolean);
        if (!canBe) [[unlikely]] {
            dataLog("AOT: A VALUE IS NOT WHAT IS PASSED THERE: ");
            node->dump(WTF::dataFile());
            dataLogLn(" held as ", static_cast<unsigned>(from), " wanted as ", static_cast<unsigned>(rep), " at bc#", m_node ? m_node->bytecodeIndex.offset() : 0, " ", m_node ? opcodeNames[m_node->opcode] : "", " in ", m_graph.nameForLog());
            RELEASE_ASSERT_NOT_REACHED();
        }
        // (What is taken never to be reached is of no type, and held as anything is.)
        return rep == Rep::JSValue ? lowJSValue(node) : convert(lowRaw(node), from, node->type, rep);
    }
    LValue lowJSValueOfParameterOnEntry(unsigned index)
    {
        switch (m_howValuesArePassed.parameters[index]) {
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
    // ImageEnvironment::distance. In two steps, of which the first is the same for everything nearby, and is kept from being merged
    // with the second: what is left of that fits in a load or a store, which the distance from the Instance does not.
    LValue environmentAt(uint32_t distance)
    {
        constexpr uint32_t window = 16 * KB;
        uint32_t start = roundUpToMultipleOf<window>(distance);
        return m_out.add(m_out.opaque(m_out.sub(m_instance, m_out.constIntPtr(start))), m_out.constIntPtr(start - distance));
    }
    // The object the function was called as.
    LValue callee()
    {
        RELEASE_ASSERT(m_calleeSlot);
        return m_out.load64(m_out.address(m_heaps.variables.atAnyIndex(), m_calleeSlot));
    }
    LValue lowConstantRegister(VirtualRegister reg) { return lowConstantRegister(code(), reg); } // For an operand that BytecodeUseDef does not count among the uses.
    LValue lowConstantRegister(Graph&, VirtualRegister);
    // The graph of the code that what is being lowered is of (Node::graph). What comes of lowering goes to m_graph.
    Graph& code() { return m_code ? *m_code : m_graph; }
    LValue convert(LValue, Rep from, Type fromType, Rep to);
    void setJSValue(Node*, LValue);
    void setInt32(Node*, LValue);
    void setInt64(Node*, LValue);
    void setDouble(Node*, LValue);
    void setBoolean(Node*, LValue);
    void setResult(Node*, LValue, Rep);
    // For an instruction that defines several registers: the value of one of them.
    void setProj(Node*, VirtualRegister, LValue, Rep = Rep::JSValue);

    LValue doubleToInt32(LValue); // ToInt32.
    LValue toBoolean(Node*);

    TypedPointer addressFor(VirtualRegister); // Where a register that lives in memory does (Graph::isHomed()).
    LValue wordByIndex(LValue base, uint32_t addend, uint32_t scale, bool mayChange);
    void lowerEntry();
    // Of a function with Signature::List.
    LValue numberOfArgumentsPassed(); // Not counting `this`.
    LValue argumentsPassed(); // Where the first is.
    LValue argumentPassedOrUndefined(unsigned index);
    // What the type that the source gives the base of the access says of the property (TypeTable).
    std::optional<TypeTable::Field> fieldAccessedBy(Node*, unsigned identifier);
    // Which layout the cell is of (Structure::knownShape()), and whether that is one of first to last.
    LValue layoutOf(LValue cell);
    LValue layoutBornAs(LValue cell); // Structure::bornAs()
    LValue layoutBornAsOrNone(Node*, LValue);
    TypedPointer slotOfStruct(LValue object, const TypeTable::Field&);
    LValue asHeld(Node* valueNode, LValue value, TypeTable::Holds); // The value as a slot of a struct has it: a number is encoded as a double.
    Node* m_sameAs { nullptr }; // setResult(): the node is this one by another name.
    void assertBornAs(Node* onBehalfOf, Node* valueNode, LValue value, uint16_t family); // Of a family that is closed. Goes on if it is of the family.
    // Where the slots are that typed code reads and writes. Of a family that is open that may be a struct with nothing in it, which stands for whatever is not one of the
    // family and cannot be made one: so whoever finds nothing in a slot, and pointer is not the object, goes the long way.
    struct StructToLookIn {
        LValue pointer;
        bool mayStandForSomethingElse;
    };
    StructToLookIn structToLookIn(Node* onBehalfOf, Node* baseNode, LValue base, uint16_t family);
    LValue viewAs(Node* onBehalfOf, Node* valueNode, LValue value, uint16_t family);
    LValue viewFoundFor(Node* valueNode, uint16_t family); // Null: none has been made of it.
    UncheckedKeyHashMap<Node*, LValue> m_views; // By op_type_tag.
    UncheckedKeyHashMap<Node*, std::pair<BasicBlock*, LValue>> m_layoutsBornAs; // What that gave, and in which block.
    bool branchUnlessHeld(Node* valueNode, LValue value, TypeTable::Holds, LBasicBlock otherwise); // False: it always is, and nothing goes there.
    // What is in a field of a struct of a closed family, from its having been read or written, for as long as nothing happens that could change it. It goes for the rest of the block, and
    // for the blocks that can only be got to from there.
    struct FieldInHand {
        Node* base;
        uint16_t family;
        uint16_t slot;
        uint16_t id; // TypeTable::Field::id: names of a family whose slots are verified have slots between them.
        Rep rep;
        LValue value;
        LValue asJSValue; // Null: nobody has made that of it.
    };
    Vector<FieldInHand> m_fieldsInHand;
    UncheckedKeyHashMap<BasicBlock*, Vector<FieldInHand>> m_fieldsInHandAtEndOf;
    bool m_nodeLeavesFieldsAlone { false }; // Says the lowering of the node, which knows better than leavesFieldsAlone().
    const FieldInHand* fieldInHand(Node* base, const TypeTable::Field&) const;
    void noteFieldInHand(Node* base, const TypeTable::Field&, LValue value, Rep, LValue asJSValue, bool isWritten);
    static bool leavesFieldsAlone(Node*);
    // What the node is, if it is a string that the program spells out, of characters that take a byte each.
    static std::optional<String> stringWrittenInProgram(Node*);
    // If the value is a string at all it is an atom: it is written in the program, or comes from a slot whose strings are (TypeTable::Holds::atoms).
    static bool isAtomIfString(Node*, unsigned depth = 0);
    static bool isAtomIfShortString(Node*, unsigned depth = 0); // Or is a long one: SlotsOfBornObjects::lengthOfShortString.
    LValue areTheSameGivenThatStringsAreAtoms(Node* left, LValue, Node* right, LValue); // Neither is a number or a BigInt.
    void makeAtomIfString(Node*, LValue);
    // `this`, in the code of a function that is not closed.
    static bool isThisOfWhatAnybodyMayCall(Node*);
    LValue isStringThatSays(Node* comparison, Node* valueNode, LValue value, const String&, LValue theString);
    // Options::aotTypesFields(): what has just been made as that layout, with those in its slots (null: nothing), is left with nothing in a slot that the slot does not hold.
    void settleWhatWasBorn(Node*, LValue object, uint32_t layout, const Vector<Node*, 8>& inSlots, const Vector<LValue, 8>& values, const Vector<TypeTable::Holds, 8>* holdsIfKnown = nullptr);
    void guardField(Node* guard);
    LValue isOneOf(LValue layout, uint16_t first, uint16_t last);
    // Instance::states, of the function that is being compiled: whether it has a Data of its own by now, and where that is if so.
    struct OwnData {
        LValue hasAny;
        LValue data;
    };
    OwnData ownData();
    TypedPointer slotWord(unsigned slot, unsigned word); // Word 0: structureID and offset. Word 1: pointer.
    LValue slotAddress(unsigned slot);
    unsigned allocateSlot() { return m_graph.numICSlots++; }
    // What a frame says where it is with, while what the node does is being done somewhere else.
    uint32_t callSiteBitsOf(Node*);
    uint32_t siteOf(Node*); // The same, of a place that has been said to be one, or is not going to be asked about.
    // For an operation that goes by a number in the bytecode: see functionOfBytecodeOfCaller().
    uint32_t whoseBytecode(Node* node) { return node->graph->isOutermost() ? 0 : m_graph.inlineFrames[node->graph->inlineFrame()].knownCallee + 1; }
    unsigned allocateSlots(unsigned count)
    {
        unsigned first = m_graph.numICSlots;
        m_graph.numICSlots += count;
        return first;
    }
    // Words in the frame that operations read lists of values from and write additional results to. Nothing is kept there.
    TypedPointer scratchWord(unsigned index);
    LValue scratchAddress() { return m_scratch; }
    LValue storeToScratch(Node*, VirtualRegister first, unsigned count); // The registers first, first - 1, ... in that order.
    LValue isSentinelCell(LValue cell) { return isCellOfType(cell, SentinelType); }
    template<typename Functor> LValue isCellAnd(Node*, LValue jsValue, const Functor&); // False for what is not a cell.

    // Calls into C++.
    // For an operation declared with JSC_DECLARE_JIT_OPERATION: hands back the result, having checked for an exception.
    template<typename... Args> LValue vmCall(Node*, LType, Entry, Args...);
    // For one declared NOEXCEPT.
    template<typename... Args> LValue plainCall(LType, Entry, Args...);
    void storeBarrier(LValue owner);
    // Of what is being lowered: whether anything but what comes right after it uses it. (Nearly everything is a call. So what is wanted for longer than that is kept where calls leave it alone.)
    bool isWantedAfterWhatFollows(Node*) const;

    // Calls to stubs (AOTStubs.h).
    struct StubArgument {
        LValue value;
        Reg reg;
    };
    struct StubImmediate {
        GPRReg reg;
        uint32_t value;
    };
    enum class StubClobbers : uint8_t { WhatCallsDo, Temporaries, Nothing };
    // place: where the function is to be said to be meanwhile, if not at what is being lowered.
    B3::PatchpointValue* callStub(Stub, LType, const Vector<StubArgument, 8>&, const Vector<StubImmediate, 2>&, StubClobbers = StubClobbers::WhatCallsDo, Node* place = nullptr);
    LValue callOperationThroughStub(Node*, LType, Entry, const Vector<LValue, 8>& arguments); // No node: it does not throw.
    // What one of the helpers makes of those (generateHelper()). Null: it gave up.
    LValue callHelper(Stub, const Vector<LValue, 4>& arguments);
    // The same, or what `slow` gives if it gave up.
    template<typename Slow> LValue withHelper(Stub, const Vector<LValue, 4>& arguments, const Slow&);
    // Of an operation that takes the global object and these and gives nothing back, from where the code hardly ever gets. It is no reason for the function to have a
    // frame, or to keep anything anywhere but where it is.
    // (ChangesNothing: it looks, or it throws. What has been loaded is as good afterwards.)
    enum class ColdCall : uint8_t { MayDoAnything, ChangesNothing };
    void coldCall(Node*, Entry, LValue first = nullptr, LValue second = nullptr, ColdCall = ColdCall::MayDoAnything);
    LValue coldCallForValue(Node*, Entry, LValue first, LValue second = nullptr, ColdCall = ColdCall::MayDoAnything);
    B3::PatchpointValue* emitColdCall(Node*, LType, Entry, LValue first, LValue second, ColdCall);
    // Code that is run over and over is worth its size. The rest, which is nearly all of it, is not: it calls a stub for what
    // it would otherwise do itself.
    LValue callBinaryStub(Node*, Stub, LType, LValue, LValue);
    // Whether some class of the program that extends that one of the language's may have for itself what is read (BUN_AOT_OVERRIDDEN_METHODS).
    static bool mayBeOverridden(ASCIILiteral nameOfClass, Node* read);
    // Whether an operation that has a stub calls it, instead of having its fast path inline. Inline fast paths are faster where code is hot (25-40% on small kernels) and
    // bigger everywhere (a tenth of the code of a big program whose time is not spent in loops), and all that is known ahead of time is whether code is in a loop.
    bool isCompact() const
    {
        if (!Options::useAOTInlineFastPathsInLoops() || m_block->isGeneric)
            return true;
        return (!m_block->isInLoop || m_block->isOnlyInLoopOfBuiltin) && !m_graph.callsItself;
    }
    // An op_resolve_scope that is only there for the op_get_from_scope that follows it: the two are one call.
    LValue differenceFromWhatIsWritten(LValue characters, std::span<const Latin1Character> written);
    // The characters of a string, and how many, if they are narrow and are to be had for the looking: it is all in one piece, or is a slice of one that is (which is left a slice).
    // If not, how long it is all the same, at `otherwise`.
    struct NarrowCharacters {
        LValue characters;
        LValue length;
    };
    NarrowCharacters narrowCharactersOf(LValue string, LBasicBlock otherwise, Vector<ValueFromBlock, 2>& lengthOtherwise);
    // Which of several strings that the program spells out a value is, if any: by how long it is, and then by what it says, a word at a time.
    struct StringCase {
        const StringImpl* says;
        LBasicBlock target;
        Node* constant; // If there is one.
    };
    void dispatchOnString(Node* place, Node* scrutinee, LValue value, Vector<StringCase, 16>&, LBasicBlock defaultBlock, bool isKnownToBeCell = false);
    // Several blocks that do nothing but compare one value with one constant after another (===), each going on to the next if it is not that:
    //     if (x === "a") ... else if (x === "b") ... else if (x === 3) ...        x === "a" || x === "b" || x === "c"
    // Which it is is settled in the first of them, in one go, and the rest are not got to.
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
    bool isFusedWithGetFromScope(Node*);
    LValue scopeToResolveFrom(Node* resolve);
    // A slot that a stub can be told about.
    // What the code says for one of the function's identifiers. See AOT::NumbersOfIdentifiers.
    unsigned numberOf(Graph& graph, unsigned identifier)
    {
        auto* numbers = numbersOfIdentifiersOfProgram();
        return numbers ? numbers->get(graph.codeBlock()->identifier(identifier).impl()) : identifier;
    }
    unsigned numberOf(unsigned identifier) { return numberOf(code(), identifier); } // Of what is being lowered.
    unsigned numberOf(const Node* whose, unsigned identifier) { return numberOf(*whose->graph, identifier); }
    unsigned allocateSite(Node*, unsigned identifier, unsigned extra = 0);
    // The one site of an instruction that is in both copies of a loop: what the generic copy finds out, the fast one goes by.
    unsigned sharedSite(Node*, unsigned identifier, unsigned extra = 0);

    // AOTLowerArith.cpp
    bool tryLowerArith(Node*);
    void lowerBinaryArith(Node*, VirtualRegister lhs, VirtualRegister rhs);
    void lowerBitOp(Node*, VirtualRegister lhs, VirtualRegister rhs);
    void lowerUnaryArith(Node*, VirtualRegister operand);
    LValue lowerCompare(Node*, OpcodeID canonical, VirtualRegister lhs, VirtualRegister rhs); // A boolean.
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
    // Where the element is, having seen to it that there is one.
    TypedPointer elementOfTypedArray(Node* guard, Node* base, Node* property, JSType);
    void guardGetLength(Node*);
    void guardCheckType(Node*);
    // False: there is nothing to it, or nothing to be done about it.
    bool guardResolveScope(Node*);
    bool guardGetFromScope(Node*);
    bool guardCall(Node*);
    // Goes to passed if the value is of a type that is certain to get past the check, and to the other block if not.
    void emitTypeTests(Node* value, LValue jsValue, unsigned mask, LBasicBlock passed, LBasicBlock notSettled);
    void emitTypeTests(std::nullptr_t, Type typeOfValue, LValue jsValue, unsigned mask, LBasicBlock passed, LBasicBlock notSettled);

    // AOTLowerObjects.cpp
    bool tryLowerObjects(Node*);
    bool tryLowerAllocation(Node*);
    bool tryLowerConversion(Node*);
    bool tryLowerPropertyVariant(Node*);
    // An object of the structure that the cache at `slot` (fillAllocationCache()) is for, with those values for its first properties.
    // Goes to `otherwise` if there is nothing in the cache, or no room where it says to allocate.
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
    using Arguments = Vector<LValue, 8>; // The first is `this`; for a construction, new.target.
    enum class CallMode : uint8_t { Call, Construct, TailCall };
    bool tryLowerCall(Node*);
    Arguments lowerArguments(Node*, unsigned argc, unsigned argv);
    // Of whatever the callee turns out to be. A tail call ends the block, and there is no result.
    LValue emitCall(Node*, LValue callee, const Arguments&, CallMode = CallMode::Call, StubIntrinsic = StubIntrinsic::None);
    void lowerCall(Node*, VirtualRegister callee, unsigned argc, unsigned argv, CallMode, bool hasResult);
    bool lowerCallToKnownFunction(Node*, VirtualRegister callee, unsigned argv, const Arguments&, CallMode, bool hasResult);
    void lowerCallVarargs(Node*, VirtualRegister callee, VirtualRegister thisValue, VirtualRegister arguments, int firstVarArg, CallMode);
    void lowerCallWithItems(Node*, Node* calleeNode, LValue callee, LValue thisValue, Node* list, CallMode);
    void lowerCallDirectEval(Node*);
    LValue storeArgumentsToScratch(const Arguments&); // But for the first. Where they are.
    void finishCall(B3::PatchpointValue*, CallMode, Rep result = Rep::JSValue);
    LBasicBlock leaveIfFunction(Node* calleeNode, LValue callee);

    // AOTLowerBuiltins.cpp
    LValue isSuchAReceiver(Node* read, Node* baseNode, LValue base, Receiver);
    void lowerReadOfBuiltin(Node*, Node* baseNode);
    bool lowerCallOfBuiltin(Node*, Node* calleeNode, unsigned argc, unsigned argv, const Arguments&, bool hasResult, LBasicBlock& afterwards, Vector<ValueFromBlock, 2>& results);
    UncheckedKeyHashMap<Node*, LValue> m_receiverChecks; // By the read of what is called: what isSuchAReceiver() gave.

    Graph& m_graph;

    HowValuesArePassed m_howValuesArePassed; // This function.
    LValue m_dataOnEntry { nullptr }; // Of a function that starts cold: m_data, but for in a loop.
    B3::Variable* m_dataInLoops { nullptr }; // And there, if it has any: what was found at the top of the loop, the last time round.
    LValue m_callFrame { nullptr };
    LValue m_data { nullptr };
    LValue m_dataOrNothing { nullptr };
    LValue m_constants { nullptr }; // Of a function that starts cold: FunctionInfo::constants.
    LValue m_calleeSlot { nullptr };
    LValue m_listSlot { nullptr }; // Signature::List: how many arguments were passed, and where they are.
    LValue m_homes { nullptr };
    LValue m_scratch { nullptr };
    LBasicBlock m_returnBlock { nullptr };
    Vector<ValueFromBlock, 4> m_returnValues;
    // Graph::numberOfThingsReturnedInRegisters: each, from wherever it is returned; and how it goes.
    Vector<Vector<ValueFromBlock, 4>, 8> m_thingsReturned;
    Vector<Rep, 8> m_howThingsAreReturned;
    LBasicBlock m_exit { nullptr }; // While a guard is lowered: the way to the generic copy.
    Vector<std::pair<BasicBlock*, LBasicBlock>, 2> m_edges; // While a branch is lowered: the ways to successors that have phis.
    LBasicBlock m_afterSlotChecks { nullptr };
    LValue m_slotEpoch { nullptr };
    unsigned m_slotOfSlotChecks { 0 };
    UncheckedKeyHashMap<uint64_t, unsigned, WTF::IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_sharedSites; // By bytecode offset and identifier.
    BasicBlock* m_block { nullptr };
    unsigned m_nodeIndex { 0 }; // Of the node being lowered, in m_block.
    Node* m_node { nullptr }; // It, if it has a place in the bytecode.
    Graph* m_code { nullptr }; // code()
};

template<typename... Args>
LValue Lowering::vmCall(Node* node, LType type, Entry function, Args... args)
{
    return callOperationThroughStub(node, type, function, { args... });
}

template<typename Slow>
LValue Lowering::withHelper(Stub stub, const Vector<LValue, 4>& arguments, const Slow& slow)
{
    // Code that is not worth its size calls the operation, which has the helper ahead of it (FOR_EACH_AOT_OPERATION_BEHIND_HELPER).
    if (isCompact())
        return slow();
    LValue quick = callHelper(stub, arguments);
    LBasicBlock otherwise = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    ValueFromBlock made = m_out.anchor(quick);
    m_out.branch(m_out.notNull(quick), usually(continuation), rarely(otherwise));
    m_out.appendTo(otherwise);
    ValueFromBlock madeTheLongWay = m_out.anchor(slow());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return m_out.phi(B3::pointerType(), made, madeTheLongWay);
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

#endif // ENABLE(FTL_JIT)
