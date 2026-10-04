/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltins.h"
#include "AOTEmitter.h"
#include "AOTGraph.h"
#include "AOTRuntime.h"
#include "AOTTypeTable.h"
#include "B3AbstractHeapRepository.h"
#include "B3MemoryValue.h"
#include "B3Procedure.h"
#include "B3Variable.h"
#include "B3VariableValue.h"
#include "FTLAbbreviatedTypes.h"
#include "FTLOutput.h"
#include "FTLValueFromBlock.h"
#include "FTLWeightedTarget.h"
#include "ScopeOffset.h"

namespace JSC { namespace AOT {

class Lowering : public Emitter {
    WTF_MAKE_NONCOPYABLE(Lowering);
public:
    Lowering(Graph&, B3::Procedure&);

    bool run();

    static constexpr double coldFrequency = 1.0 / 1024;

private:
    friend struct LoweringAccess;

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
    B3::Variable* environmentVariable(Node* environment, unsigned offset);
    LValue readPromotedVariable(Node* environment, unsigned offset);
    void writePromotedVariable(Node* environment, unsigned offset, LValue);
    LValue ancestorScope(Node* scope, unsigned hops);
    LValue scopeAbove(Node* scope, unsigned hops);
    void noteIndexConstant(int32_t);
    UncheckedKeyHashMap<Node*, Vector<B3::Variable*>> m_environmentVariables;
    struct ArrayView {
        LValue butterfly { nullptr };
        LValue length { nullptr };
        LValue limit { nullptr };
    };
    struct ArrayViewVariables {
        B3::Variable* butterfly { nullptr };
        B3::Variable* length { nullptr };
        B3::Variable* limit { nullptr };
    };
    const ArrayView* viewOf(Node* access, Node* base);
    void hoistArrayStorageLoadsAheadOf(BasicBlock*);
    void loadArrayView(Node* origin, Node* array, const ArrayViewVariables&);
    void reloadArrayViews();
    LBasicBlock entryBlockFor(BasicBlock* successor);
    Vector<std::tuple<BasicBlock*, Node*, ArrayViewVariables>, 4> m_arrayViews;
    ArrayView m_arrayViewHere;
    void unsupported(Node*);
    void incrementTypeCoverageCounter(uint32_t);
    void coverOperation(Node*, BasicBlock*, bool isElided = false);
    void coverCall(StringView name, uint32_t whichCounter);
    FTL::WeightedTarget rarely(LBasicBlock block, const char* file = __builtin_FILE(), unsigned line = __builtin_LINE())
    {
        if (Options::aotTypeCoveragePath()) [[unlikely]]
            m_blocksReachedRarely.add(block);
        if (Options::aotGuardToDropForTesting()) [[unlikely]]
            dropGuardIfSelected(file, line);
        return FTL::rarely(block);
    }
    void dropGuardIfSelected(const char* file, unsigned line);
    FTL::WeightedTarget usually(LBasicBlock block)
    {
        if (Options::aotTypeCoveragePath()) [[unlikely]]
            m_blocksReachedOtherwise.add(block);
        return FTL::usually(block);
    }
    FTL::WeightedTarget unsure(LBasicBlock block)
    {
        if (Options::aotTypeCoveragePath()) [[unlikely]]
            m_blocksReachedOtherwise.add(block);
        return FTL::unsure(block);
    }
    LBasicBlock m_blockWhereOperationStarts { nullptr };
    UncheckedKeyHashSet<LBasicBlock> m_blocksReachedRarely;
    UncheckedKeyHashSet<LBasicBlock> m_blocksReachedOtherwise;

    LValue lowRaw(Node*);
    LValue lowJSValue(Node*);
    LValue lowJSValuePreferringInt32(Node*);
    LValue lowInt32(Node*);
    LValue lowInt64(Node*);
    LValue lowDouble(Node*);
    LValue lowBoolean(Node*);
    LValue lowCell(Node* node) { return lowJSValue(node); }
    LValue lowAs(Node* node, Rep rep)
    {
        Rep from = node->rep();
        bool canBe = rep == Rep::JSValue || from == Rep::JSValue || from == rep || (from != Rep::Boolean && rep != Rep::Boolean);
        if (!canBe) [[unlikely]] {
            dataLog("AOT: incompatible representation for a parameter or result: ");
            node->dump(WTF::dataFile());
            dataLogLn(" has rep ", static_cast<unsigned>(from), ", expected rep ", static_cast<unsigned>(rep), " at bc#", m_node ? m_node->bytecodeIndex.offset() : 0, " ", m_node ? opcodeNames[m_node->opcode] : "", " in ", m_graph.nameForLog());
            RELEASE_ASSERT_NOT_REACHED();
        }
        return rep == Rep::JSValue ? lowJSValue(node) : convert(lowRaw(node), from, node->type, rep);
    }
    LValue lowParameterOnEntryAsJSValue(unsigned index)
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
    LValue heldCapture(Graph&, const void* scope, unsigned offset);
    LValue callee()
    {
        RELEASE_ASSERT(m_calleeSlot);
        return m_out.load64(m_out.address(m_heaps.variables.atAnyIndex(), m_calleeSlot));
    }
    LValue lowConstantRegister(VirtualRegister reg) { return lowConstantRegister(code(), reg); }
    LValue lowConstantRegister(Graph&, VirtualRegister);
    LValue constantThroughStub(uint32_t number, Stub);
    bool isInRunOnceCode() const { return m_graph.codeBlock()->codeType() == ModuleCode && !m_block->isInLoop; }
    uint32_t programConstantIndex(Node*);
    Graph& code() { return m_code ? *m_code : m_graph; }
    LValue convert(LValue, Rep from, Type fromType, Rep to);
    void setJSValue(Node*, LValue);
    void setInt32(Node*, LValue);
    void setInt64(Node*, LValue);
    void setDouble(Node*, LValue);
    void setBoolean(Node*, LValue);
    void setResult(Node*, LValue, Rep);
    void verifyInferredType(Node*, LValue);
    void setProj(Node*, VirtualRegister, LValue, Rep = Rep::JSValue);

    LValue doubleToInt32(LValue);
    LValue toBoolean(Node*);

    TypedPointer addressFor(VirtualRegister);
    LValue wordByIndex(LValue base, uint32_t addend, uint32_t scale, bool mayChange);
    void lowerEntry();
    LValue numberOfArgumentsPassed();
    LValue argumentsPassed();
    LValue argumentPassedOrUndefined(unsigned index);
    std::optional<TypeTable::Field> fieldAccessedBy(Node*, unsigned identifier);
    LValue layoutOf(LValue cell);
    LValue loadTypedLayoutID(LValue cell);
    LValue loadTypedLayoutIDOrZero(Node*, LValue);
    TypedPointer fieldAddress(LValue, const TypeTable::Field&);
    LValue toFieldRepresentation(Node* valueNode, LValue, TypeTable::FieldType);
    Node* m_aliasTarget { nullptr };
    void checkTypedLayout(Node* originNode, Node* valueNode, LValue, uint16_t layoutID);
    void trap();
    void throwUnlessMadeFromFunction(Node* origin, LValue, uint32_t function, bool trapsWhenValidating);
    void lowerReadOfClosedMethod(Node* read, Node* receiver, uint32_t function);
    struct FieldStorage {
        LValue pointer;
        bool mayBePlaceholder;
    };
    FieldStorage fieldStorageFor(Node* originNode, Node* baseNode, LValue base, uint16_t layoutID);
    LValue coerceToTypedLayout(Node* originNode, Node* valueNode, LValue, uint16_t layoutID);
    LValue cachedCoercionFor(Node* valueNode, uint16_t layoutID);
    UncheckedKeyHashMap<Node*, LValue> m_coercions;
    UncheckedKeyHashMap<Node*, std::pair<BasicBlock*, LValue>> m_loadedLayoutIDs;
    bool branchUnlessAccepted(Node* valueNode, LValue, TypeTable::FieldType, LBasicBlock otherwise);
    struct AvailableField {
        Node* base;
        uint16_t layoutID;
        uint16_t slot;
        uint16_t id;
        Rep rep;
        LValue value;
        LValue asJSValue;
    };
    Vector<AvailableField> m_availableFields;
    UncheckedKeyHashMap<BasicBlock*, Vector<AvailableField>> m_availableFieldsAtEndOf;
    bool m_nodePreservesFields { false };
    const AvailableField* availableField(Node* base, const TypeTable::Field&) const;
    void recordAvailableField(Node* base, const TypeTable::Field&, LValue, Rep, LValue asJSValue, bool isWritten);
    static bool preservesFields(Node*);
    struct AvailableRead {
        Node* base;
        UniquedStringImpl* name;
        LValue value;
        LValue effectEpoch;
        friend bool operator==(const AvailableRead&, const AvailableRead&) = default;
    };
    static constexpr unsigned maxAvailableReads = 16;
    Vector<AvailableRead> m_availableReads;
    UncheckedKeyHashMap<BasicBlock*, Vector<AvailableRead>> m_availableReadsAtEndOf;
    LValue m_effectEpochBeforeStore { nullptr };
    bool m_nodeKeepsReads { false };
    bool m_isOnOnePathOnly { false };
    struct ReadVariables {
        B3::Variable* value { nullptr };
        B3::Variable* effectEpoch { nullptr };
    };
    UncheckedKeyHashMap<std::pair<Node*, UniquedStringImpl*>, ReadVariables> m_readVariables;
    ReadVariables variablesFor(const AvailableRead&);
    void findReadsAvailableAtHeadOf(BasicBlock*);
    void publishAvailableReads(BasicBlock*);
    void forgetReadsChangedInLoop(BasicBlock* header);
    LValue loadEffectEpoch();
    void recordAvailableRead(Node* base, UniquedStringImpl* name, LValue value, LValue effectEpoch);
    void forgetReadsChangedBy(Node*);
    static std::optional<String> constantStringOf(Node*);
    static bool isAtomIfString(Node*, unsigned depth = 0);
    static bool isAtomIfShortString(Node*, unsigned depth = 0);
    LValue areEqualAssumingAtomStrings(Node* left, LValue, Node* right, LValue);
    void atomizeIfString(Node*, LValue);
    static bool isEscapingFunctionThis(Node*);
    LValue isStringEqualTo(Node* comparison, Node* valueNode, LValue, const String&, Node* literalString);
    LValue isStringEqualToAtom(Node* valueNode, LValue, LValue literalString);
    void validateNewObject(Node*, LValue, uint32_t layout, const Vector<Node*, 8>& inSlots, const Vector<LValue, 8>& values, const Vector<TypeTable::FieldType, 8>* fieldTypesIfKnown = nullptr);
    LValue isOneOf(LValue layout, uint16_t first, uint16_t last);
    struct OwnData {
        LValue hasAny;
        LValue data;
    };
    OwnData ownData();
    TypedPointer slotWord(unsigned slot, unsigned word);
    LValue slotAddress(unsigned slot);
    LValue dataHere();
    unsigned allocateSlot() { return m_graph.numICSlots++; }
    uint32_t callSiteBitsOf(Node*);
    uint32_t siteOf(Node*);
    uint32_t bytecodeOwner(Node* node) { return node->graph->isOutermost() ? 0 : m_graph.inlineFrames[node->graph->inlineFrame()].knownCallee + 1; }
    unsigned allocateSlots(unsigned count)
    {
        unsigned first = m_graph.numICSlots;
        m_graph.numICSlots += count;
        return first;
    }
    TypedPointer scratchWord(unsigned index);
    LValue scratchAddress() { return m_scratch; }
    LValue storeToScratch(Node*, VirtualRegister first, unsigned count);
    LValue isSentinelCell(LValue cell) { return isCellOfType(cell, SentinelType); }
    template<typename Functor> LValue isCellAnd(Node*, LValue jsValue, const Functor&);

    template<typename... Args> LValue vmCall(Node*, LType, Entry, Args...);
    template<typename... Args> LValue plainCall(LType, Entry, Args...);
    void storeBarrier(LValue owner);
    bool mayCollectOrThrow(Node*);
    std::optional<unsigned> offsetOfVariableStoredInline(Node*);
    bool isFollowedByStoreBarrier(Node*);
    bool isLiveAfterNextNode(Node*) const;

    struct StubArgument {
        LValue value;
        Reg reg;
    };
    struct StubImmediate {
        GPRReg reg;
        uint32_t value;
    };
    enum class StubClobbers : uint8_t { CallerSavedRegisters, Temporaries, Nothing };
    B3::PatchpointValue* callStub(Stub, LType, const Vector<StubArgument, 8>&, const Vector<StubImmediate, 2>&, StubClobbers = StubClobbers::CallerSavedRegisters, Node* place = nullptr);
    LValue callOperationThroughStub(Node*, LType, Entry, const Vector<LValue, 8>& arguments);
    LValue contextOf(Entry operation) const { return takesInstance(operation) ? m_instance : m_globalObject; }
    LValue callHelper(Stub, const Vector<LValue, 4>& arguments);
    template<typename Slow> LValue withHelper(Stub, const Vector<LValue, 4>& arguments, const Slow&);
    enum class ColdCall : uint8_t { HasArbitraryEffects, ChangesNothing };
    void coldCall(Node*, Entry, LValue first = nullptr, LValue second = nullptr, ColdCall = ColdCall::HasArbitraryEffects);
    LValue coldCallForValue(Node*, Entry, LValue first, LValue second = nullptr, ColdCall = ColdCall::HasArbitraryEffects);
    B3::PatchpointValue* emitColdCall(Node*, LType, Entry, LValue first, LValue second, ColdCall);
    LValue callBinaryStub(Node*, Stub, LType, LValue, LValue);
    static RegisterSet stubTemporaries(unsigned count)
    {
        RELEASE_ASSERT(count <= std::size(stubTemporaryGPRs));
        RegisterSet result;
        for (unsigned i = 0; i < count; ++i)
            result.add(stubTemporaryGPRs[i], IgnoreVectors);
        return result;
    }
    static RegisterSet registersClobberedByCalls()
    {
#if CPU(ARM64)
        return RegisterSet { callMarkerGPR };
#else
        RegisterSet result = stubTemporaries(std::size(stubTemporaryGPRs));
        result.add(callMarkerGPR, IgnoreVectors);
        return result;
#endif
    }
    bool isCompact() const
    {
        if (!usesDataStubs())
            return false;
        if (!Options::useAOTInlineFastPathsInLoops() || m_block->isGeneric)
            return true;
        return !m_block->isInProfitableLoop || m_block->isInBuiltinLoopOnly;
    }
    bool prefersCalls() const { return isCompact() || (m_graph.codeBlock()->codeType() != FunctionCode && !m_block->isInLoop); }
    LValue compareWithLiteral(LValue characters, std::span<const Latin1Character> written);
    struct Latin1Characters {
        LValue characters;
        LValue length;
    };
    Latin1Characters latin1CharactersOf(LValue string, LBasicBlock otherwise, Vector<ValueFromBlock, 2>& lengthOtherwise);
    struct StringCase {
        const StringImpl* string;
        LBasicBlock target;
        Node* constant;
    };
    void dispatchOnString(Node* place, Node* scrutinee, LValue, Vector<StringCase, 16>&, LBasicBlock defaultBlock, bool isKnownCell = false);
    struct ChainArm {
        BasicBlock* block;
        Node* constant;
        BasicBlock* target;
    };
    struct ComparisonChain {
        Node* value { nullptr };
        Vector<ChainArm, 8> arms;
        BasicBlock* otherwise { nullptr };
    };
    Vector<ComparisonChain> m_chains;
    UncheckedKeyHashMap<BasicBlock*, unsigned> m_chainsByFirstBlock;
    UncheckedKeyHashSet<BasicBlock*> m_blocksInsideChains;
    void findComparisonChains();
    void lowerComparisonChain(BasicBlock*, const ComparisonChain&);
    bool isFusedWithGetFromScope(Node*);
    LValue scopeToResolveFrom(Node* resolve);
    unsigned numberOf(Graph& graph, unsigned identifier)
    {
        auto* numbers = programIdentifierIndices();
        return numbers ? numbers->get(graph.codeBlock()->identifier(identifier).impl()) : identifier;
    }
    unsigned numberOf(unsigned identifier) { return numberOf(code(), identifier); }
    unsigned numberOf(const Node* whose, unsigned identifier) { return numberOf(*whose->graph, identifier); }
    unsigned allocateSite(Node*, unsigned identifier, unsigned extra = 0);
    unsigned sharedSite(Node*, unsigned identifier, unsigned extra = 0);

    bool tryLowerArith(Node*);
    void lowerBinaryArith(Node*, VirtualRegister lhs, VirtualRegister rhs);
    void lowerBitOp(Node*, VirtualRegister lhs, VirtualRegister rhs);
    void lowerUnaryArith(Node*, VirtualRegister operand);
    LValue numberToString(Node* origin, Node* number);
    LValue lowerCompare(Node*, OpcodeID canonical, VirtualRegister lhs, VirtualRegister rhs);
    LValue lowerEquality(Node*, bool strict, VirtualRegister lhs, VirtualRegister rhs);
    LValue toInt32ForBitOp(Node* operand);

    bool tryLowerAccess(Node*);
    void lowerGetById(Node*);
    void lowerPutById(Node*);
    using PropertyRun = Vector<Node*, 16>;
    void findPropertyRuns(BasicBlock*);
    void lowerPropertyRun(const PropertyRun&);
    void lowerGetByVal(Node*);
    void lowerPutByVal(Node*);
    void lowerResolveScope(Node*);
    void lowerGetFromScope(Node*);
    void lowerPutToScope(Node*);
    LValue loadProperty(LValue, LValue offset);
    using StaticVariable = Graph::StaticVariable;
    StaticVariable resolveStatically(unsigned identifierIndex, unsigned localScopeDepth, ResolveType type) { return code().resolveStatically(identifierIndex, localScopeDepth, type); }

    TypedPointer cachedPropertyAddress(LValue, LValue firstSlotWord, const B3::AbstractHeap* = nullptr);
    LValue getByIdCached(Node*, LValue base, Type baseType, Entry operation, unsigned identifier);
    LValue getByIdWithThisCached(Node*, LValue base, LValue thisValue, unsigned identifier);
    LValue lowIndex(Node* property, LBasicBlock indexReady, LBasicBlock notIndex);
    struct ConstantKey {
        unsigned identifier;
        UniquedStringImpl* name;
    };
    std::optional<ConstantKey> constantKeyOf(Node* property);

    void lowerGuard(BasicBlock*, Node*);
    void emitGuard(Node*);
    unsigned propertyGuardSlot(Node*);
    LValue loadSlotWord(unsigned slot, unsigned word);
    void checkStructure(Node* baseNode, LValue base, LValue firstSlotWord);
    void checkCallee(Node*);
    void lowerGuarded(Node*);
    LBasicBlock newColdBlock();
    LValue trapBits();
    void exitUnless(LValue condition);
    void exitUnlessType(LValue jsValue, Type from, Type wanted);
    void guardReentry(BasicBlock*);
    void guardGetById(Node*);
    void guardPutById(Node*);
    void guardGetByVal(Node*);
    void guardPutByVal(Node*);
    TypedPointer typedArrayElement(Node* guard, Node* base, Node* property, JSType);
    TypedPointer typedArrayElement(Node* base, Node* property, JSType, LBasicBlock outOfBounds, LBasicBlock slowCase);
    LValue loadTypedArrayElement(JSType, TypedPointer);
    void storeTypedArrayElement(JSType, LValue, TypedPointer);
    void guardGetLength(Node*);
    void guardCheckType(Node*);
    bool guardResolveScope(Node*);
    bool guardGetFromScope(Node*);
    bool guardCall(Node*);
    void emitTypeTests(Node* value, LValue jsValue, unsigned mask, LBasicBlock passed, LBasicBlock undecided);
    void emitTypeTests(std::nullptr_t, Type valueType, LValue jsValue, unsigned mask, LBasicBlock passed, LBasicBlock undecided);

    bool tryLowerObjects(Node*);
    bool tryLowerAllocation(Node*);
    bool tryLowerConversion(Node*);
    bool tryLowerPropertyVariant(Node*);
    LValue allocateObjectWithProperties(unsigned slot, const Vector<LValue, 8>& values, LBasicBlock otherwise);
    void lowerGetLength(Node*);
    LValue lengthOfArray(Node* origin, LValue array);
    void lowerToThis(Node*);
    void throwTDZError(Node*);
    void throwStaticError(Node*);

    bool tryLowerIteration(Node*);
    void lowerIteratorOpen(Node*, bool isAsync);
    void lowerIteratorNext(Node*);
    void lowerAsyncIteratorNext(Node*);
    void lowerIteratorCloseCheck(Node*);
    LValue iteratorCloseCheckCondition(Node*);
    void checkIteratorResultIsObject(Node*, LValue);
    LValue inlineWatchpointSetIsStillValid(LValue set);

    using Arguments = Vector<LValue, 8>;
    enum class CallMode : uint8_t { Call, Construct, TailCall };
    bool tryLowerCall(Node*);
    Arguments lowerArguments(Node*, unsigned argc, unsigned argv);
    LValue emitCall(Node*, LValue callee, const Arguments&, CallMode = CallMode::Call, StubIntrinsic = StubIntrinsic::None, std::optional<Stub> hostCallStub = std::nullopt);
    std::optional<Stub> stubForHostCallee(Node* calleeNode, CallMode);
    void lowerCall(Node*, VirtualRegister callee, unsigned argc, unsigned argv, CallMode, bool hasResult);
    bool lowerCallToKnownFunction(Node*, VirtualRegister callee, unsigned argv, const Arguments&, CallMode, bool hasResult);
    bool lowerCallToPromiseFunction(Node*, LinkTimeConstant, const Arguments&, bool hasResult);
    void lowerCallVarargs(Node*, VirtualRegister callee, VirtualRegister thisValue, VirtualRegister arguments, int firstVarArg, CallMode);
    void lowerCallWithItems(Node*, Node* calleeNode, LValue callee, LValue thisValue, Node* list, CallMode);
    void lowerCallDirectEval(Node*);
    LValue storeArgumentsToScratch(const Arguments&);
    void finishCall(B3::PatchpointValue*, CallMode, Rep result = Rep::JSValue);
    LBasicBlock branchIfCalleeIsFunction(Node* calleeNode, LValue callee);

    LValue isReceiverKind(Node* baseNode, LValue base, Receiver);
    bool receiverMayHaveChangedSince(Node* read, Node* call) const;
    void lowerBuiltinRead(Node*, Node* baseNode);
    bool lowerSizeOfMapOrSet(Node*, Node* baseNode);
    bool lowerBuiltinCall(Node*, Node* calleeNode, unsigned argc, unsigned argv, const Arguments&, bool hasResult, LBasicBlock& afterwards, Vector<ValueFromBlock, 2>& results, Rep& repOfResults);
    UncheckedKeyHashMap<Node*, LValue> m_receiverChecks;

    Graph& m_graph;

    ValueRepresentations m_valueRepresentations;
    LValue m_dataOnEntry { nullptr };
    B3::Variable* m_dataInLoops { nullptr };
    LValue m_callFrame { nullptr };
    LValue m_data { nullptr };
    LValue m_dataOrNull { nullptr };
    LValue m_calleeSlot { nullptr };
    LValue m_listSlot { nullptr };
    LValue m_frameRegisterStorage { nullptr };
    LValue m_scratch { nullptr };
    unsigned m_numberOfScratchWords { 0 };
    LBasicBlock m_returnBlock { nullptr };
    Vector<ValueFromBlock, 4> m_returnValues;
    Vector<Vector<ValueFromBlock, 4>, 8> m_registerReturnValues;
    Vector<Rep, 8> m_returnValueReps;
    LBasicBlock m_exit { nullptr };
    Vector<std::pair<BasicBlock*, LBasicBlock>, 2> m_edges;
    LBasicBlock m_afterSlotChecks { nullptr };
    LValue m_slotEpoch { nullptr };
    unsigned m_slotCheckSlot { 0 };
    UncheckedKeyHashMap<uint64_t, unsigned, WTF::IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_sharedSites;
    BasicBlock* m_block { nullptr };
    Vector<PropertyRun> m_propertyRuns;
    UncheckedKeyHashMap<Node*, unsigned> m_propertyRunOfStore;
    unsigned m_nodeIndex { 0 };
    Node* m_node { nullptr };
    Vector<Node*, 2> m_newCells;
    Graph* m_code { nullptr };
};

template<typename... Args>
LValue Lowering::vmCall(Node* node, LType type, Entry function, Args... args)
{
    return callOperationThroughStub(node, type, function, { args... });
}

template<typename Slow>
LValue Lowering::withHelper(Stub stub, const Vector<LValue, 4>& arguments, const Slow& slow)
{
    if (isCompact() || !usesDataStubs())
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
