/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTProgram.h"
#include "AOTStubs.h"
#include "AOTType.h"
#include "AOTTypeTable.h"
#include "BytecodeIndex.h"
#include "BytecodeStructs.h"
#include "CallFrame.h"
#include "GetPutInfo.h"
#include "Instruction.h"
#include "LinkTimeConstant.h"
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

enum class NodeKind : uint8_t {
    Bytecode,
    Constant,
    ConstantCell,
    Intrinsic,
    LinkTimeConstant,
    Argument,
    Phi,
    Proj,
    GetStack,
    SetStack,
    Guard,
    Narrow,
};

enum class CallIntrinsic : uint8_t { None, MathSqrt, MathAbs, MathFloor, MathCeil, MathTrunc, MathFround, MathMin, MathMax, MathIMul, StringCharCodeAt, ArrayPush };
inline bool hasNoEffects(CallIntrinsic intrinsic) { return intrinsic != CallIntrinsic::ArrayPush; }
CallIntrinsic callIntrinsicFor(UniquedStringImpl* propertyName, unsigned argumentCountIncludingThis);

enum class PropertyEffect : uint8_t {
    None,
    OnSlowPathOnly,
    StoresNamedProperty,
    Call,
    Arbitrary,
};

enum class GuardKind : uint8_t {
    Whole,
    Nothing,
    Reentry,
    Entry,
    Structure,
    SlotsAgree,
    SlotIsDirect,
    BeginSlotChecks,
    EndSlotChecks,
    Callee,
    KnownCallee,
    TypedArrayStorage,
    Uint8ArrayStorageIfAny,
    IsArrayIntrinsic,
    IsIntrinsic,
    IsLikelyFunction,
};

struct NewObjectPlan {
    struct Property {
        unsigned identifier;
        bool isDefined;
        bool isStrict;
        bool isAssigned;
    };
    Vector<Property, 8> properties;
    struct Store {
        unsigned offset;
        unsigned property;
    };
    Vector<Store, 8> stores;

    static VirtualRegister registerOf(unsigned property) { return VirtualRegister(0x20000000 + static_cast<int>(property)); }

    static NewObjectPlan forCreateThis(const JSInstructionStream&, unsigned offsetOfCreateThis);
};

class Graph;
struct BasicBlock;
struct Node;

enum class Escape : uint8_t {
    NotAnalyzed,
    StaysHere,
    IsOnlyBorrowed,
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
    PassedToLocalClosure,
    PassedToParameter,
    PassedToVariable,
    PassedToUnknown,
    PassedToRetainingCallee,
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
inline bool stays(Escape escape) { return escape == Escape::StaysHere || escape == Escape::IsOnlyBorrowed; }
ASCIILiteral nameOf(Escape);
enum class AllocationKind : uint8_t { Object, Array, Closure, Environment };
static constexpr unsigned numberOfAllocationKinds = 4;
inline ASCIILiteral nameOf(AllocationKind kind)
{
    static constexpr ASCIILiteral names[] = { "object"_s, "array"_s, "closure"_s, "environment"_s };
    return names[static_cast<unsigned>(kind)];
}
std::optional<AllocationKind> allocationKind(const Node*);

struct Use {
    VirtualRegister reg;
    Node* node { nullptr };
};

struct Node {
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
    JSValue constant;
    VirtualRegister reg;
    Node* replacement { nullptr };
    UnlinkedCodeBlock* ownerOfConstant { nullptr };
    inline UnlinkedCodeBlock* codeBlockOfConstant() const;
    uint16_t slotInConstantObjectPlusOne { 0 };
    uint8_t inlineCapacityOfConstantObject { 0 };
    bool onlyChecksConstantObject { false };
    bool constantObjectIsNeverAllocated { false };
    bool propertyOfConstantObjectIsAbsent { false };
    Node* guard { nullptr };
    Node* guarded { nullptr };
    Node* target { nullptr };
    Node* iteratorMethodRead { nullptr };
    bool checksNarrowedType { false };
    Type narrowedTo { TNone };
    Type speculatedType { TNone };
    uint32_t likelyFunction { 0 };
    uint16_t firstLayout { 0 };
    uint16_t lastLayout { 0 };
    bool hasLayoutInRange(uint16_t first, uint16_t last) const
    {
        for (const Node* node = this; node->kind == NodeKind::Narrow && node->narrowedTo; node = node->uses[0].node) {
            if (node->firstLayout >= first && node->lastLayout <= last)
                return true;
        }
        if (isBytecode(op_type_tag) && firstLayout >= first && lastLayout <= last && !Options::auditAOTTypedFields() && isTrusted)
            return true;
        return type && isSubtype(type, TCell) && hasLayoutInRangeIfCell(first, last);
    }
    bool hasLayoutInRangeIfCell(uint16_t first, uint16_t last) const
    {
        Type cells = type & TCell;
        if (!cells || !isSubtype(cells, TFinalObject))
            return false;
        auto layouts = layoutRangeOf(cells);
        return layouts.lowest >= first && layouts.highest <= last;
    }
    unsigned expectedMask { 0 };
    GuardKind guardKind { GuardKind::Whole };
    uint16_t intrinsic { 0 };
    uint16_t builtinCalled { 0 };
    uint8_t builtinReceiver { 0 };
    bool structureIsChecked { false };
    bool slotIsDirect { false };
    bool calleeIsChecked { false };
    bool wasInferredUnreachable { false };
    bool isElided { false };
    bool isReadOnlyForCall { false };
    bool isTrusted { false };
    bool isPromoted { false };
    bool isNeverEmpty { false };
    bool accessesLocalEnvironment { false };
    bool mayBeInFrame { false };
    Node* standIn { nullptr };
    bool isSharedRegExpLiteral { false };
    bool isInFrame { false };
    unsigned firstFrameSlot { 0 };
    unsigned extendedFrameSize { 0 };
    Node* promotedEnvironment { nullptr };
    unsigned offsetInEnvironment { 0 };
    Node* scopeToStartFrom { nullptr };
    unsigned remainingHops { 0 };
    unsigned skippedEnvironments { 0 };
    Escape escape { Escape::NotAnalyzed };
    Node* site { nullptr };
    Node* otherSite { nullptr };
    unsigned numberOfLiteralProperties { 0 };
    uint8_t numberOfReturnValues { 0 };
    uint8_t returnValueIndex { 0 };
    BasicBlock* viewedAheadOf { nullptr };
    Node* arrayViewed { nullptr };
    Node* storage { nullptr };

    B3::Value* lowered { nullptr };
    B3::Value* loweredAsJSValue { nullptr };
    B3::Value* loweredLength { nullptr };
    unsigned useCount { 0 };
    bool isHandled { false };

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

    bool isKnownToPass(unsigned mask) const
    {
        if (isSubtype(type, typeProvingMask(mask)))
            return true;
        for (const Node* node = this; node->isBytecode(op_check_type); node = node->use(node->as<OpCheckType>().m_value)) {
            unsigned earlier = node->as<OpCheckType>().m_mask;
            if (soundTypeTagsForMask(earlier) & ~soundTypeTagsForMask(mask))
                continue;
            if (!soundTypeMaskNamesTypedArray(mask) || !(earlier & MaskOtherObject) || (earlier >> SoundTypeTypedArrayShift) == (mask >> SoundTypeTypedArrayShift))
                return true;
        }
        return false;
    }

    void dump(PrintStream&) const;
};

struct BasicBlock {
    Graph* graph { nullptr };
    unsigned index { 0 };
    unsigned bytecodeBegin { 0 };
    unsigned bytecodeEnd { 0 };
    bool isCatchEntrypoint { false };
    bool isReachable { false };
    bool isLoopHeader { false };
    bool isInLoop { false };
    bool isInProfitableLoop { false };
    bool isInBuiltinLoopOnly { false };
    bool isGeneric { false };
    bool endsWithGuard { false };
    bool isPreHeader { false };
    bool isReentry { false };
    bool isRarelyExecuted { false };

    BasicBlock* immediateDominator { nullptr };
    unsigned rpoIndex { 0 };
    unsigned dominatorPreNumber { 0 };
    unsigned dominatorPostNumber { 0 };
    bool dominates(const BasicBlock* other) const { return dominatorPreNumber <= other->dominatorPreNumber && dominatorPostNumber >= other->dominatorPostNumber; }
    Vector<Node*> phis;
    Vector<Node*> nodes;
    Vector<BasicBlock*, 2> predecessors;
    Vector<BasicBlock*, 2> successors;
    BitVector liveIn;
    BitVector readByBlockHandlers;
    BitVector readByHandlersAfterBlock;
    Vector<Node*> valuesAtTail;

    B3::BasicBlock* lowered { nullptr };
    Vector<Node*, 2> arraysViewed;
    BitVector loopBody;
    B3::BasicBlock* loweredAhead { nullptr };
    B3::BasicBlock* loweredTail { nullptr };

    Node* terminal() const { return nodes.isEmpty() ? nullptr : nodes.last(); }
};

struct ScopeChainEntry {
    enum Kind : uint8_t {
        Lexical,
        GlobalLexical,
        Global,
        Opaque,
        Unknown,
    };
    Kind kind { Opaque };
    bool isModule { false };
    class JSC::SymbolTable* symbolTable { nullptr };
};
using ScopeChain = Vector<ScopeChainEntry, 4>;

class Graph {
    WTF_MAKE_NONCOPYABLE(Graph);
public:
    Graph(VM&, UnlinkedCodeBlock*, const ScopeChain&);
    ~Graph();

    VM& vm() { return m_vm; }
    UnlinkedCodeBlock* codeBlock() { return m_codeBlock; }

    Graph& outermost() { return *m_outermost; }
    bool isOutermost() const { return m_outermost == this; }
    unsigned inlineFrame() const { return m_inlineFrame; }
    struct InlineFrame {
        unsigned parent;
        uint32_t callSite;
        unsigned knownCallee;
        bool isTailCall { false };
    };
    Vector<InlineFrame> inlineFrames;
    void adoptInlinee(std::unique_ptr<Graph>&&, InlineFrame);
    void computeBlockOrder();
    void computeDominators();
    void sinkIteratorMethodReads();
    const ModuleLinkage* linkage() const { return m_linkage; }
    Node* closureScope { nullptr };
    Node* closureFunction { nullptr };
    Node* currentClosureFunction() const
    {
        Node* function = closureFunction;
        while (function && function->replacement)
            function = function->replacement;
        return function;
    }
    const FunctionSummary* summaryOfInlinedFunction { nullptr };
    const FunctionSummary* summaryWithCaptures() const { return summaryOfInlinedFunction ? summaryOfInlinedFunction : m_summary; }
    unsigned dissolvedScopesOutside(unsigned hops) const;
    unsigned dissolvedScopesAbove(const Node* scope, unsigned hops);
    Node* onlyEnvironmentWithIdentity(const void*);
    Node* environmentRestoredBy(const Node*, bool evenIfItHasAnObject = false);
    UncheckedKeyHashMap<const void*, Node*> m_environmentsByIdentity;
    bool m_hasEnvironmentsByIdentity { false };
    UncheckedKeyHashMap<Node*, Vector<std::pair<Node*, unsigned>, 4>> capturesOfClosures;
    bool isInTailPosition { true };
    bool loopSplittingIsDisabled { false };
    Vector<UnlinkedFunctionExecutable*> functionsCreated;
    bool readsElementsOrEmpty { false };
    bool isInlinedBuiltin { false };
    bool wasCalledInLoop { false };
    unsigned numberOfNodes() const { return m_nodes.size(); }
    Node* lastNode() { return &m_nodes.last(); }
    const ScopeChain& scopeChain() const { return m_scopeChain; }

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

    bool livesInFrame(VirtualRegister reg) const { return m_frameRegisters.get(registerIndex(reg)); }
    bool isLiveIntoHandler(VirtualRegister reg) const { return m_registersLiveIntoHandlers.get(registerIndex(reg)); }
    bool isArrayOperandRegister(VirtualRegister reg) const { return m_arrayOperandRegisters.get(registerIndex(reg)); }
    bool hasFrameRegisters() const { return !m_frameRegisters.isEmpty(); }
    unsigned frameRegisterIndex(VirtualRegister reg) const
    {
        ASSERT(livesInFrame(reg));
        return m_frameRegisterIndices[registerIndex(reg)];
    }
    static constexpr unsigned maximumArrayOperandsInRegisters = 32;
    static bool readsOperandsFromFrame(const JSInstruction* instruction) { return instruction->opcodeID() == op_new_array && instruction->as<OpNewArray>().m_argc > maximumArrayOperandsInRegisters; }
    static bool readsOperandsFromFrame(const Node* node) { return node->isBytecode(op_new_array) && readsOperandsFromFrame(node->instruction); }
    unsigned numberOfFrameRegisters() const { return m_frameRegisters.bitCount(); }
    Convention convention() const { return m_convention; }
    ValueRepresentations valueRepresentations() const { return AOT::valueRepresentations(m_summary, m_convention); }

    Node* addNode(NodeKind);
    BasicBlock* addBlock();

    Node* constant(JSValue);
    Node* intrinsicReadBy(const JSInstruction*, Node* base);
    Node* intrinsic(unsigned number);
    unsigned numberOfIntrinsicReads { 0 };
    CallIntrinsic callIntrinsic(const Node* callOrGuard) const;
    struct CallOperands {
        VirtualRegister callee;
        unsigned argc;
        unsigned argv;
        VirtualRegister argument(unsigned indexIncludingThis) const { return VirtualRegister(-static_cast<int>(argv) + CallFrame::thisArgumentOffset() + static_cast<int>(indexIncludingThis)); }
    };
    static CallOperands callOperands(const JSInstruction*);
    uint32_t typeTagAt(unsigned bytecodeOffset) const { return m_typeTags.get(bytecodeOffset); }
    static std::optional<TypeTable::Field> typedFieldAccessedBy(const Node*);
    static bool isEscapingFunctionThis(const Node*);
    static std::optional<TypeTable::Field> typedBaseField(const Node* base, UniquedStringImpl* name);
    static uint16_t newObjectLayoutID(const Node*);
    static uint32_t classRecordedBy(const Node*);
    void noteClassesDefined();
    static uint32_t closedMethodReadBy(const Node*);
    uint16_t thisLayoutID() const;
    Type thisTypeOnEntry() const;
    static uint32_t typeTagOf(const Node* node) { return node->kind == NodeKind::Bytecode && node->instruction ? node->graph->typeTagAt(node->bytecodeIndex.offset()) : 0; }
    const Vector<unsigned, 4>& literalStores(unsigned offsetOfNewObject);
    static std::optional<JSType> typedArrayAccessed(const Node*);
    std::pair<Node*, Node*> arrayAndElementStored(const Node*) const;
    static bool isLocallyAllocatedArray(const Node* node) { return (node->isBytecode(op_new_array) && !readsOperandsFromFrame(node)) || node->isBytecode(op_new_array_with_size); }
    const KnownFunction* knownCallee(const Node*, bool* isExact = nullptr) const;
    const KnownFunction* knownCalleeIgnoringSummaries(const Node*, bool* isExact) const;
    bool calleeIsExact(const Node*) const;
    const KnownFunction* likelyFunctionInModuleVariable(unsigned identifier, unsigned scopeOffset) const;
    const KnownFunction* knownFunctionReadBy(const Node* getFromScope, bool* isExact = nullptr) const;
    bool passesNoFunctionObject(const Node* call);
    bool needsFunctionObject() const { return m_needsFunctionObject; }
    bool scopeIsModuleEnvironment() const { return m_scopeIsModuleEnvironment; }
    uint32_t moduleEnvironmentDepth();
    void elideUnpassedCalleeReads();
    void elideArrayIteratorMethodReads();
    static bool isArrayIteratorMethodRead(const Node*);
    static bool hasOverriddenMethodInfo();
    static bool methodMayBeOverridden(ASCIILiteral className, Node* read);
    static bool isAnyArrayIteratorMethod(const Node*);
    void findBuiltinsCalled();
    void recordKnownFunctionUses(const FunctionSummaryMap&, const FunctionSummary* currentSummary);
    void recordObjectsInVariables(VariableSummaries&);
    Node* constantCellOf(UnlinkedCodeBlock*, VirtualRegister);
    static PropertyEffect propertyEffectOf(const Node*);
    const FunctionSummary::PropertyEffects* propertyEffectsOfCall(const Node*) const;
    void recordPropertyEffects() const;
    void noteFieldsComparedWithStrings();
    void findArgumentLists();
    static Node* argumentListFor(const Node*);
    static std::optional<LinkTimeConstant> linkTimeConstantOf(const Node*);
    std::optional<uint32_t> accessedEnvironmentDepth(const Node*);
    std::optional<uint32_t> resolvedEnvironmentDepth(const Node*);
    static bool isScopeAtDepth(const Node* scope, unsigned hops);
    bool isScopeUsedAsImplicitThis(const Node*);
    void setCalleeHints(const CalleeHints* hints) { m_hints = hints; }
    void setSummary(const FunctionSummary* summary) { m_summary = summary; }
    const FunctionSummary* summary() const { return m_summary; }
    bool isCalledRepeatedly() const { return m_summary && m_summary->isCalledRepeatedly.load(std::memory_order_relaxed); }
    void setVariableSummaries(VariableSummaries* summaries, unsigned reader = VariableSummaries::nobody)
    {
        m_variableSummaries = summaries;
        m_summaryReader = reader;
    }
    VariableSummaries* variableSummaries() const { return m_variableSummaries; }
    void setNameForLog(const String& name) { m_nameForLog = name; }
    const String& nameForLog() const { return m_nameForLog; }
    unsigned summaryReader() const { return m_summaryReader; }
    const void* scopeIdentity(const Node*, unsigned depth = 0);
    Variable variableAccessedBy(const Node*);
    bool isGeneratorFrame(const void* scope) { return scope && scope == generatorFrameIdentity(); }
    void recordUntrackableVariableAccesses(VariableSummaries&);
    Type argumentTypeOnEntry(unsigned indexIncludingThis) const
    {
        if (!m_summary || !m_summary->isNonEscaping || indexIncludingThis >= FunctionSummary::maxParameters)
            return TTop;
        if (!indexIncludingThis)
            return m_summary->thisType.load();
        return m_summary->parameterTypes[indexIncludingThis].load();
    }
    const CalleeHints* calleeHints() const { return m_hints; }
    unsigned knownCalleeIndex(const ImageKey&);
    Vector<ImageKey> knownCallees;
    bool callsItself { false };
    unsigned numberOfRegisterReturnValues { 0 };
    UncheckedKeyHashMap<Node*, Vector<Node*, 8>> returnValueReads;
    bool makesCalls { false };
    bool emitsCalls { false };
    UncheckedKeyHashSet<B3::Value*> patchpointsTakingData;
    Vector<String> remarks;
    Vector<CoveredOperation> coverage;
    uint32_t firstTypeCoverageCounter { 0 };
    bool isCoveringOperation() const { return m_isCoveringOperation; }
    void remark(ASCIILiteral what, StringView detail = { }, bool isOffUsualPath = false)
    {
        if (Options::aotRemarksPath() || Options::aotTypeCoveragePath()) [[unlikely]]
            addRemark(what, detail, isOffUsualPath);
    }
    void addRemark(ASCIILiteral what, StringView detail, bool isOffUsualPath);
    void beginCoveredOperation(const Node*, const BasicBlock*, bool isElided = false);
    CoveredOperation coveredOperationAt(unsigned bytecodeOffset, uint8_t flags);
    bool m_isCoveringOperation { false };
    bool alwaysEmitsCalls { false };
    mutable std::optional<bool> hasRemainingCalls;
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
    Vector<BasicBlock*> blockForOffset;
    Vector<BasicBlock*> genericTargetForOffset;
    Vector<BasicBlock*> headerForOffset;
    UncheckedKeyHashSet<uint64_t, WTF::IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> jumpsBack;
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
            Closure,
            Import,
            ModuleImport,
            Unresolved,
            Dynamic,
        } kind { Dynamic };
        unsigned depth { 0 };
        ScopeOffset offset;
        bool inModule { false };
        bool isReadOnly { false };
        bool isInGlobalScopes { false };
        bool isGlobal { false };
        bool isInOutermostEnvironment { false };
        StaticImport import;

        bool isAtStaticDepth() const { return kind == Closure || kind == ModuleImport; }
        bool isCachedInSlot() const { return kind == Unresolved || kind == ModuleImport; }
    };
    StaticVariable resolveStatically(unsigned identifierIndex, unsigned localScopeDepth, ResolveType);
    unsigned resolveScopeExtra(const OpResolveScope&);
    unsigned getFromScopeExtra(const OpGetFromScope&);
    void setLinkage(const ModuleLinkage*, const DeclaredNamesLink*);
    bool usesStaticImports { false };
    bool startsCold { false };
    bool isGetByValOnThis { false };
    bool mayReturnScopeVariable { false };
    std::optional<std::pair<uint32_t, uint32_t>> returnedVariable;
    bool m_needsFunctionObject { true };
    bool m_scopeIsModuleEnvironment { false };
    BitVector m_frameRegisters;
    BitVector m_registersLiveIntoHandlers;
    BitVector m_arrayOperandRegisters;
    Vector<unsigned> m_frameRegisterIndices;
    Vector<Type> frameRegisterTypes;
    unsigned numICSlots { 0 };
    Vector<Site> sites;
    Vector<uint32_t> siteConstants;
    Vector<UniquedStringImpl*> selectors;
    Vector<KnownShape> shapes;
    void noteSiteSelector(unsigned slot, UniquedStringImpl*);
    void noteSiteShape(unsigned slot, KnownShape&&);
    Vector<uint32_t> quotableSites;
    Vector<uint32_t> callSites;
    Vector<SpreadSite> spreadSites;
    Vector<uint32_t> plans;
    void noteSitePlan(unsigned firstSlot, Vector<uint32_t, 16>&& words);
    std::optional<KnownShape> literalShape(const Node*) const;
    StubCalls stubCalls;
    IndexReferences indexReferences;

private:
    VM& m_vm;
    UnlinkedCodeBlock* m_codeBlock;
    ScopeChain m_scopeChain;
    const CalleeHints* m_hints { nullptr };
    const FunctionSummary* m_summary { nullptr };
    VariableSummaries* m_variableSummaries { nullptr };
    UncheckedKeyHashMap<std::pair<UnlinkedCodeBlock*, int>, Node*> m_constantCellsOfOtherCode;
    String m_nameForLog;
    unsigned m_summaryReader { VariableSummaries::nobody };
    UncheckedKeyHashMap<int, Vector<Node*>, WTF::IntHash<int>, WTF::UnsignedWithZeroKeyHashTraits<int>> m_storesToFrameRegisters;
    bool m_hasStoresToFrameRegisters { false };
    std::optional<const void*> m_generatorFrameIdentity;
    const void* generatorFrameIdentity();
    const ModuleLinkage* m_linkage { nullptr };
    const DeclaredNamesLink* m_declaredNames { nullptr };
    BitVector m_namesAssignedTo;
    UncheckedKeyHashMap<unsigned, uint32_t, WTF::IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_typeTags;
    unsigned m_numArguments;
    unsigned m_numLocals;
    Convention m_convention;
    SegmentedVector<Node, 32> m_nodes;
    Node* m_emptyConstant { nullptr };
    UncheckedKeyHashMap<EncodedJSValue, Node*, EncodedJSValueHash, EncodedJSValueHashTraits> m_constants;
    UncheckedKeyHashMap<unsigned, Node*, DefaultHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_intrinsics;
    void findLiteralStores();
    UncheckedKeyHashMap<unsigned, Vector<unsigned, 4>, DefaultHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_literalStores;
    bool m_hasFoundLiteralStores { false };
    ASCIILiteral m_failureReason;
    OpcodeID m_failureOpcode { op_nop };
    Graph* m_outermost { this };
    unsigned m_inlineFrame { 0 };
    unsigned m_numberOfInlinedNodes { 0 };
    Vector<std::unique_ptr<Graph>> m_inlinees;
};

class NodeUsers {
public:
    explicit NodeUsers(Graph&);
    std::span<Node* const> of(Node* node) const
    {
        auto it = m_users.find(node);
        return it == m_users.end() ? std::span<Node* const> { } : it->value.span();
    }
    struct OnlyRead {
        Vector<Node*, 4> aliasingUsers;
        Vector<std::pair<Node*, unsigned>, 8> reads;
        Vector<Node*, 2> absentReads;
        Vector<Node*, 2> tests;
    };
    enum class AbsentReads : bool { Disallow, Allow };
    std::optional<OnlyRead> isOnlyRead(Node*, std::span<UniquedStringImpl* const> names, uint16_t layoutID, AbsentReads = AbsentReads::Disallow) const;

private:
    UncheckedKeyHashMap<Node*, Vector<Node*, 2>> m_users;
};

bool parseBytecode(Graph&);
Type inferTypes(Graph&, Vector<const KnownFunction*>* calleesRead = nullptr, Vector<const KnownFunction*>* calleesWithWidenedInputs = nullptr);
void inferRanges(Graph&);
void optimizeLoops(Graph&);
void simplify(Graph&);
void inlineCalls(Graph&, const ProgramCode&);
void analyzeEscapes(Graph&);
void shareRegExpLiterals(Graph&);
void promoteEnvironments(Graph&);
void saveRegistersAtDefinitions(Graph&);
void clearDeadFrameSlots(Graph&);
bool mayPromoteEnvironmentsOf(Graph&);
void recordScopes(Graph&, VariableSummaries&, const FunctionSummaryMap&, const FunctionSummary* current);
void scalarReplaceReadOnlyObjects(Graph&);
void replaceReadsOfConstantObjects(Graph&);
bool isAbsentFromObjectPrototype(UniquedStringImpl*);

inline UnlinkedCodeBlock* Node::codeBlockOfConstant() const { return ownerOfConstant ? ownerOfConstant : graph->codeBlock(); }
void recordReturnedLiterals(Graph&);
void planMultiValueReturns(Graph&);
uint32_t escapingParameters(Graph&, Vector<const KnownFunction*>* calleesRead);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
