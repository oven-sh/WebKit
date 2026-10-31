/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTForwardDataflow.h"
#include "AOTFreshResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeldOperands.h"
#include "AOTIRBuilder.h"
#include "AOTLiveness.h"
#include "AOTSinkAllocations.h"
#include "AOTVector.h"
#include "AOTVirtualObjectShapes.h"
#include <algorithm>
#include <compare>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

template<typename Entry, size_t inlineCapacity, typename Key>
size_t positionOfEntry(const Vector<Entry, inlineCapacity>& entries, Key Entry::* key, Key value)
{
    return std::ranges::lower_bound(entries, value, { }, key) - entries.begin();
}

template<typename Entry, size_t inlineCapacity, typename Key>
const Entry* findEntry(const Vector<Entry, inlineCapacity>& entries, Key Entry::* key, Key value)
{
    size_t position = positionOfEntry(entries, key, value);
    return position < entries.size() && entries[position].*key == value ? &entries[position] : nullptr;
}

template<typename Entry, size_t inlineCapacity, typename Key>
Entry& ensureEntry(Vector<Entry, inlineCapacity>& entries, Key Entry::* key, Key value)
{
    size_t position = positionOfEntry(entries, key, value);
    if (position == entries.size() || entries[position].*key != value) {
        entries.insert(position, Entry { });
        entries[position].*key = value;
    }
    return entries[position];
}

template<typename Entry, size_t inlineCapacity, typename Key>
void removeEntry(Vector<Entry, inlineCapacity>& entries, Key Entry::* key, Key value)
{
    if (const Entry* entry = findEntry(entries, key, value))
        entries.removeAt(entry - entries.begin());
}

// `values` is in ascending order, like the entries.
template<typename Entry, size_t inlineCapacity, typename Key>
void removeEntries(Vector<Entry, inlineCapacity>& entries, Key Entry::* key, std::span<const Key> values)
{
    if (values.empty())
        return;
    entries.removeAllMatching([&](const Entry& entry) {
        return std::ranges::binary_search(values, entry.*key);
    });
}

struct ObjectVariant {
    std::optional<uint8_t> tagBit;
    VirtualShapeID shape;

    uint32_t tagMask() const { return tagBit ? 1u << *tagBit : 0; }

    friend bool operator==(const ObjectVariant&, const ObjectVariant&) = default;
    friend auto operator<=>(const ObjectVariant&, const ObjectVariant&) = default;
};

struct HeldObject {
    uint32_t field { 0 };
    VirtualObjectID object;

    friend bool operator==(const HeldObject&, const HeldObject&) = default;
};

struct ObjectState {
    Vector<ObjectVariant, 1> variants;
    Vector<HeldObject, 1> heldObjects;
    BitVector fieldsValidInCell;
    BlockID cellJoinedAt;
    Vector<TypeID, 1> assumedTypes;
    bool lacksFieldsOnSomeWay { false };
    bool isMissingOnSomeWay { false };
    // The same of an object that is a cell on the other ways: what stands for the cell may be an Unspecified value.
    bool isCellMissingOnSomeWay { false };

    static ObjectState withShape(VirtualShapeID shape) { return { { ObjectVariant { std::nullopt, shape } }, { }, { }, { }, { }, false, false, false }; }

    bool hasCell() const;
    bool isCell() const { return variants.size() == 1 && variants[0].shape == cellShape; }
    bool isVirtual() const { return !hasCell(); }
    bool hasOneShape() const;
    bool needsTag() const { return !hasOneShape(); }
    bool hasConflictingTags() const;
    uint32_t tagBits() const;

    VirtualObjectID objectHeldIn(uint32_t field) const;
    void hold(uint32_t field, VirtualObjectID object) { ensureEntry(heldObjects, &HeldObject::field, field).object = object; }
    void release(uint32_t field) { removeEntry(heldObjects, &HeldObject::field, field); }

    void assume(TypeID);
    void keepAssumedTypesOf(const ObjectState&);

    void addVariant(ObjectVariant);
    void becomeCell();
    void normalize();

    friend bool operator==(const ObjectState&, const ObjectState&) = default;
};

struct ObjectEntry {
    VirtualObjectID object;
    ObjectState state;

    friend bool operator==(const ObjectEntry&, const ObjectEntry&) = default;
};

enum class AliasKind : uint8_t { Object, Guard, Results };

struct AliasEntry {
    ValueID value;
    VirtualObjectID object;
    AliasKind kind { AliasKind::Object };

    friend bool operator==(const AliasEntry&, const AliasEntry&) = default;
};

struct TestEntry {
    ValueID test;
    VirtualObjectID object;
    std::optional<bool> result;
    uint32_t tagBitsWhenTrue { 0 };

    friend bool operator==(const TestEntry&, const TestEntry&) = default;
};

struct SinkingState {
    bool isReachable { true };
    bool isAtHead { true };
    BlockID block;
    Vector<ObjectEntry, 2> objects;
    Vector<AliasEntry, 4> aliases;
    Vector<TestEntry, 1> tests;

    static SinkingState unreachableAt(BlockID block) { return { .isReachable = false, .block = block }; }
    static SinkingState nothingAt(BlockID block) { return { .block = block }; }

    bool holdsNothing() const { return objects.isEmpty() && aliases.isEmpty() && tests.isEmpty(); }
    ObjectState* find(VirtualObjectID object) { return const_cast<ObjectState*>(std::as_const(*this).find(object)); }
    const ObjectState* find(VirtualObjectID) const;
    std::optional<size_t> indexOf(VirtualObjectID) const;
    ObjectState& add(VirtualObjectID, ObjectState&&);
    void set(std::span<ObjectEntry> movedEntries);
    size_t weight() const;
    void remove(VirtualObjectID object) { removeEntry(objects, &ObjectEntry::object, object); }
    bool canReach(VirtualObjectID from, VirtualObjectID to) const;

    const AliasEntry* findAlias(ValueID value) const { return findEntry(aliases, &AliasEntry::value, value); }
    void setAlias(ValueID value, VirtualObjectID object, AliasKind kind) { ensureEntry(aliases, &AliasEntry::value, value) = { value, object, kind }; }
    bool isDestructured(VirtualObjectID) const;

    const TestEntry* findTest(ValueID test) const { return findEntry(tests, &TestEntry::test, test); }
    void setTest(const TestEntry& entry) { ensureEntry(tests, &TestEntry::test, entry.test) = entry; }

    void dump(PrintStream&) const;

    friend bool operator==(const SinkingState&, const SinkingState&) = default;
};

class SinkingDecisions {
public:
    bool isBuiltAtHead(BlockID block, VirtualObjectID object) const { return m_objectsBuiltAtHeads.contains(pairOf(block.index(), object.index())); }
    bool buildsAtHeadOf(BlockID block) const { return m_blocksWithObjectsBuiltAtHeads.get(block.index()); }
    bool holdsCells(ValueID parameterOrCast) const { return m_parametersHoldingCells.get(parameterOrCast.index()); }
    bool holdsCells(VirtualObjectID holder, uint32_t field) const { return m_fieldsHoldingCells.contains(pairOf(holder.index(), field)); }

    void buildAtHead(BlockID, VirtualObjectID);
    void holdCells(ValueID parameterOrCast, VirtualObjectID boundObject);
    void holdCellsAsBefore(ValueID parameter) { m_parametersHoldingCells.set(parameter.index()); }
    void holdObjectsAgain(ValueID parameter) { m_parametersHoldingCells.clear(parameter.index()); }
    void holdCells(VirtualObjectID holder, uint32_t field);

    bool takeDidChange() { return std::exchange(m_didChange, false); }
    std::span<const VirtualObjectID> objectsOfNewDecisions() const LIFETIME_BOUND { return m_objectsOfNewDecisions.span(); }
    void forgetNewDecisions() { m_objectsOfNewDecisions.clear(); }

private:
    using PairSet = UncheckedKeyHashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

    static uint64_t pairOf(uint32_t first, uint32_t second) { return static_cast<uint64_t>(first) << 32 | second; }
    void didDecide(VirtualObjectID);

    PairSet m_objectsBuiltAtHeads;
    BitVector m_blocksWithObjectsBuiltAtHeads;
    PairSet m_fieldsHoldingCells;
    BitVector m_parametersHoldingCells;
    Vector<VirtualObjectID, 4> m_objectsOfNewDecisions;
    bool m_didChange { false };
};

inline constexpr OpcodeSet storesWithoutValue { Opcode::AppendHole, Opcode::PropDelete };

class AllocationSinking;
class ObjectInterpreter;

enum class SinkingMode : uint8_t { Analyze, Count, Rewrite };
enum class ReturnedObjects : uint8_t { AreBuilt, StayVirtual };

// An analysis goes through the function about this often: the solver until its decisions stand, a count, and both again for each round of candidates given up.
inline constexpr uint64_t expectedNumberOfSweeps = 16;
inline constexpr uint64_t timesTheWorkThatOneSweepMayTake = 4;

class Heat;

struct BlocksThatMayGetTests {
    const Heat& heat;
    BitVector asked { };
    BitVector answers { };
};

struct SinkingPolicy {
    ReturnedObjects returnedObjects { ReturnedObjects::AreBuilt };
    bool recordsLosses { true };
    bool leavesWhenNoAllocationPays { false };
    // In a field whose slot has room for them. The parts of a result and of an argument have the types of the fields alone.
    bool followsUndefinedAndNullInFields { false };
    BlocksThatMayGetTests* blocksThatMayGetTests { nullptr };
    // Ascending: what addTracesOfFruitlessAllocations() has listed behind earlier analyses of the function. Allocations with such a number are not followed again.
    const Vector<uint64_t>* tracesOfFruitlessAllocations { nullptr };
    TypeArena* types { nullptr };
};

// One rule for whatever puts a value into a field: a store, a copy out of the field of another object, and the look ahead that says which stores will fold.
enum class Admission : uint8_t { Refused, IsOfFieldType, TakesRoomBesideFieldType };
Admission admissionOf(const AllocationSinking&, const VirtualObjectDescription&, const VirtualField&, FieldContents stored);

struct BuildCause {
    std::optional<Opcode> opcode;
    EscapeReasons reasons;
    LimitID limit { LimitID::None };
    // A store by name counts as code that nobody knows. It may stand where such code follows at once: no guard and no read is carried past that anyway.
    bool admitsStoresByName { false };
};

// What a report says of an object that the use makes necessary.
BuildCause describeCause(FunctionAnalyses&, const Use&);

struct Candidate {
    ValueID definition;
    BlockID block;
    VirtualFamilyID family;
    uint32_t descriptionIndex { 0 };
    std::optional<uint8_t> tagBitWhenAbsent;
    bool isJoin { false };
    bool isAbandoned { false };
    bool needsGuard { false };
    bool diesVirtual { false };
    bool diesOutsideJoins { false };
    bool isBuiltBeforeBranches { false };
    // The same, from the point where the object holds its referrer. The solver requests it in one round and follows it from the next round on.
    bool isBuiltBeforeBranchesOnceCyclic { false };
    bool willBeBuiltBeforeBranchesOnceCyclic { false };
    // Created under a handler and needed where no handler covers: it is built before the path leaves the handler, so that a failed allocation throws to that handler. Requested and followed as above.
    bool isBuiltUnderHandler { false };
    bool willBeBuiltUnderHandler { false };
    bool wasFruitlessBefore { false };
    // Abandoned before the families were made: no field of it is known, so nothing may take it up again.
    bool isLeftOut { false };
    unsigned numberOfBuilds { 0 };
    unsigned numberOfRarerBuilds { 0 };
    unsigned numberOfBorrowingBuilds { 0 };
    unsigned numberOfFoldedStores { 0 };
    unsigned numberOfRepeatedStores { 0 };
    Vector<VirtualObjectID, 1> holdersThatStoreItLate;
    BlockID mergeBlock;
    std::optional<BuildCause> firstCause;
    LimitID abandonedBy { LimitID::None };
};

struct VirtualReturn {
    BlockID block;
    ValueID value;
    VirtualObjectID object;
    ObjectState state;
};

// The first six are filled for a block when AllocationSinking::prepare() is called for it.
struct SinkingInputs {
    BitVector throwingValues;
    BitVector valuesWritingObjects;
    BitVector valuesCallingUnknownCode;
    UncheckedKeyHashMap<uint32_t, uint64_t> memberGuardsEndedByValues;
    UncheckedKeyHashMap<uint32_t, uint64_t> memberGuardsReadByValues;
    BitVector throwingTerminators;
    BitVector conditionsOnlyBranchedOn;
    BitVector guardsThatMoreThanOperationsTake;
    BitVector blocksInCycles;
    BitVector mergeBlocks;
    BitVector blocksWithCandidates;
    BitVector blocksWithJoins;
    // Each block's handler block, as the function was read.
    Vector<BlockID> handlers;
    bool hasOperationsOnSlots { false };
    bool hasObjectsGivenPrototypes { false };
    bool isLargeForItsWork { false };
};

// A name is a value that may stand for a candidate, for a guard of one, for its results or for a test of one: the candidates, and what an interpreter may bind to one.
struct SinkingNames {
    BitVector names;
    BitVector valuesMentioningNames;
    BitVector terminatorsMentioningNames;
    // A parameter of the block or its subject is a name.
    BitVector blocksBindingNames;
};

struct ObjectValues {
    ValueID tag;
    ValueID cell;
    ValueID guard;
    Vector<ValueID, 4> fields;

    ValueID field(uint32_t index) const { return index < fields.size() ? fields[index] : ValueID(); }
    void setField(uint32_t index, ValueID);
};

class ObjectValueMap {
public:
    ObjectValues& ensure(VirtualObjectID object) { return ensureEntry(m_entries, &Entry::object, object).values; }
    const ObjectValues* find(VirtualObjectID) const;
    void remove(VirtualObjectID object) { removeEntry(m_entries, &Entry::object, object); }
    void remove(std::span<const VirtualObjectID> ascendingObjects) { removeEntries(m_entries, &Entry::object, ascendingObjects); }

private:
    struct Entry {
        VirtualObjectID object;
        ObjectValues values;
    };

    Vector<Entry, 2> m_entries;
};

struct IncomingBinding {
    enum class Kind : uint8_t { Argument, NarrowedOperand, Guard, Other };

    Kind kind { Kind::Other };
    ValueID value;
};

struct OutgoingEdge {
    Edge edge;
    BlockID target;
    Vector<IncomingBinding, 4> bindings;
};

OutgoingEdge outgoingEdgeOf(const Function&, Edge);

enum class HeadValueKind : uint8_t { Tag, Cell, Guard, Field };

struct HeadValue {
    VirtualObjectID object;
    HeadValueKind kind { HeadValueKind::Field };
    uint32_t field { 0 };
};

struct RewriteTables {
    Vector<Vector<ValueID, 4>> headValues;
    Vector<uint32_t> firstHeadParameter;
    BitVector blocksWithHeadParameters;
    // Those that a rewritten block still leads to once its terminator is folded, and the entry.
    BitVector enteredBlocks;
    Vector<BlockID, 2> blocksWithDecidedBranches;
    Vector<std::pair<ValueID, ValueID>> replacedReads;
    Vector<ValueID> claimsAssumedOfContents;
    Vector<ValueID> entryConstants;
    Vector<BlockID> order;
    // No object is there, none is made and none is entered into a block behind: to rewrite such a block is to put it together again as it was.
    BitVector blocksLeftAsTheyAre;
    // Of a function that reads its arguments. No analysis can be made of a function that is half rewritten.
    Vector<ValueID> identityOfValue;
};

class RewriteCursor {
    WTF_MAKE_NONCOPYABLE(RewriteCursor);
public:
    RewriteCursor(AllocationSinking&, IRBuilder&);

    IRBuilder& builder() { return m_builder; }
    BlockID block() const { return m_builder.currentBlock(); }

    void beginBlock(BlockID original, std::optional<Target>&& handler, ObjectInterpreter&);

    ValueID emit(Value&& value) { return m_builder.append(WTF::move(value)); }
    void keep(ValueID id) { m_function.edit(block(), BlockEdit::Values).values.append(id); }
    ValueID takeConditionOf(const Terminator&);
    ValueID emitSelection(ValueID condition, ValueID whenTrue, ValueID whenFalse, TypeID);
    void setOrigin(Origin origin) { m_builder.setOrigin(origin); }

    void stateDidChange() { m_handlerArgumentsAreStale = true; }
    // Building may fail to allocate and throw. Every block added between the two is covered like the block where the first one is called, so the handler sees the objects as they were before.
    void willBuild(ObjectInterpreter&);
    void didBuild();
    // What is built on the exits of a block goes before its terminator, so the handler sees the objects as they are there.
    void willEnterEdges(ObjectInterpreter& atTerminator) { m_interpreterAtTerminator = &atTerminator; }
    void didEnterEdges() { m_interpreterAtTerminator = nullptr; }
    void willKeepThrowingValue(ObjectInterpreter&);
    void cut(ObjectInterpreter&);

    BlockID addUncoveredBlock();
    BlockID addCoveredBlock(ObjectInterpreter&);
    void moveTo(BlockID);

private:
    std::optional<Target> handlerTargetFor(ObjectInterpreter&);

    AllocationSinking& m_sinking;
    IRBuilder& m_builder;
    Function& m_function;
    BlockID m_originalBlock;
    std::optional<Target> m_originalHandler;
    Expected<BlockFrequency> m_frequency { BlockFrequency::Normal };
    bool m_handlerArgumentsAreStale { false };
    unsigned m_numberOfBuildsBegun { 0 };
    std::optional<Target> m_handlerOfBuild;
    ObjectInterpreter* m_interpreterAtTerminator { nullptr };
};

// What an analysis ended with: which candidates it gave up, and what it decided to build where.
struct SinkingFindings {
    Vector<Candidate> candidates;
    SinkingDecisions decisions;
};

class AllocationSinking {
    WTF_MAKE_NONCOPYABLE(AllocationSinking);
public:
    using Heads = Vector<std::optional<SinkingState>>;

    AllocationSinking(FunctionAnalyses&, const SinkingPolicy&);
    ~AllocationSinking();

    bool collectCandidates();
    void analyze();
    // For the same function with some of its edges split. An empty block on an edge changes nothing that the analysis found, so it need not find it step by step again.
    bool analyzeStartingFrom(SinkingFindings&&);
    SinkingFindings takeFindings() { return { WTF::move(m_candidates), WTF::move(m_decisions) }; }
    bool hasCandidates() const;
    bool splitEdgesNeedingCode(IRBuilder&);
    bool duplicateLiveRange(IRBuilder&);
    bool testElementsOfLists(IRBuilder&);
    void rewrite(IRBuilder&, const FreshResultShape* = nullptr);
    void letTestsAwaitClassesOfObjectsThatAreNotMade(IRBuilder&);
    void raiseFrequenciesBehindDecidedBranches();
    void recordLosses();
    void countStatistics();
    Vector<SunkAllocation> outcomes() const;

    FunctionAnalyses& analyses()
    {
        RELEASE_ASSERT(!m_tables);
        return m_analyses;
    }
    const FunctionAnalyses& analyses() const
    {
        RELEASE_ASSERT(!m_tables);
        return m_analyses;
    }
    void holdRoadAgainstCount(SinkingMode mode, ValueID id, uint32_t road)
    {
        if (mode == SinkingMode::Count)
            m_roadsCounted.set(id.index() + 1, road);
        else if (mode == SinkingMode::Rewrite) {
            auto counted = m_roadsCounted.find(id.index() + 1);
            RELEASE_ASSERT(counted == m_roadsCounted.end() || counted->value == road);
        }
    }
    const AnalysisContext& context() const { return m_analyses.context(); }
    const Program& program() const { return m_program; }
    const Function& function() const { return m_function; }
    const SinkingPolicy& policy() const { return m_policy; }
    const VirtualObjectContext& objectContext() const LIFETIME_BOUND { return m_objectContext; }
    ShapeContext shapeContext(VirtualObjectID) const;
    // By family. Empty: none does.
    const Vector<VirtualObjectID>& objectsWhoseCreationMayBeOrderedWithGuards() const LIFETIME_BOUND;

    uint32_t numberOfCandidates() const { return m_candidates.size(); }
    Candidate& at(VirtualObjectID object) LIFETIME_BOUND
    {
        if (m_trialRange) [[unlikely]]
            willAccessInTrial(object.index());
        return m_candidates[object.index()];
    }
    const Candidate& at(VirtualObjectID object) const LIFETIME_BOUND { return const_cast<AllocationSinking&>(*this).at(object); }
    VirtualFamily& familyOf(VirtualObjectID object) LIFETIME_BOUND { return m_families[at(object).family.index()]; }
    const VirtualFamily& familyOf(VirtualObjectID object) const LIFETIME_BOUND { return m_families[at(object).family.index()]; }
    const VirtualObjectDescription& descriptionOf(VirtualObjectID) const LIFETIME_BOUND;
    VirtualObjectID candidateDefinedBy(ValueID) const;
    ValueID sameValueThroughTests(ValueID);
    KeyOrderTable& orders() { return m_orders; }
    const KeyOrderTable& orders() const { return m_orders; }
    SinkingDecisions& decisions() { return m_decisions; }
    std::span<const ValueID> guardsOfNoObjectIn(ValueID guards) const LIFETIME_BOUND
    {
        auto found = m_guardsOfNoObject.find(guards.index() + 1);
        return found == m_guardsOfNoObject.end() ? std::span<const ValueID> { } : found->value.span();
    }
    void setGuardsOfNoObjectIn(ValueID guards, Vector<ValueID, 2>&& others) { m_guardsOfNoObject.set(guards.index() + 1, WTF::move(others)); }
    const SinkingInputs& inputs() const { return m_inputs; }
    // Before anything is asked about the values or the terminator of the block.
    void prepare(BlockID block)
    {
        if (!m_preparedBlocks.quickSet(block.index()))
            computeInputsOf(block);
    }
    // The indexes of the values that may change a state or that a handler has to see: those that mention a name, the candidates, what writes objects and, under a handler, what may throw.
    static constexpr uint32_t onlyWritesOrThrows = 1u << 31;
    std::span<const uint32_t> relevantValuesOf(BlockID block) const LIFETIME_BOUND
    {
        ASSERT(m_preparedBlocks.quickGet(block.index()));
        RelevantValues range = m_relevantValuesOfBlocks[block.index()];
        return m_relevantValues.span().subspan(range.first, range.end - range.first);
    }
    bool mentionsName(const Terminator&, BlockID) const;
    // Nothing is bound, tested or decided by a name on such an edge: objects go along as they are, or die.
    bool isPlain(Edge edge, BlockID target) const
    {
        return !edge.isHandlerEdge() && m_ignoredEdges.isEmpty() && !m_names.terminatorsMentioningNames.quickGet(edge.from.index()) && !m_names.blocksBindingNames.quickGet(target.index());
    }
    bool checksShortcuts() const { return m_checksShortcuts; }
    bool checksThatModesAgree() const;
    const BitVector& loadableFieldsOf(VirtualObjectID);
    bool storesByNameWaitIn(BlockID) const;
    bool mayGetTests(BlockID) const;
    // The object has to be built where no store by name may stand. It is built where the store stands instead, in the rare arm, as an object that takes no such value always was.
    void refuseUnprovenValues(VirtualObjectID);
    void letStoresByNameWait(VirtualObjectID);
    void didStoreUndefinedOrNull(VirtualObjectID, uint32_t field);
    const SinkingState* headOf(BlockID) const;
    // What can be said of the state at a head without making one for a block that is entered with nothing.
    bool isReached(BlockID block) const { return isEnteredWithNothing(block) || headOf(block); }
    bool hasObjectsAtHead(BlockID block) const
    {
        if (isEnteredWithNothing(block))
            return false;
        const SinkingState* head = headOf(block);
        return head && !head->objects.isEmpty();
    }
    bool holdsNothingAtHead(BlockID block) const
    {
        if (isEnteredWithNothing(block))
            return true;
        const SinkingState* head = headOf(block);
        return head && head->holdsNothing();
    }
    std::span<const VirtualReturn> virtualReturns() const LIFETIME_BOUND { return m_returns.span(); }
    const FreshResultShape* resultShape() const { return m_resultShape; }
    void addTracesOfFruitlessAllocations(Vector<uint64_t>&) const;

    // While the function is analyzed. Few of its values name an object or a test of one.
    bool isLiveAtHead(ValueID, BlockID) const;
    // Whether the definition of the candidate is a parameter of the block or live at its head.
    bool isDefinitionLiveIn(VirtualObjectID object, BlockID block)
    {
        const std::optional<BitVector>& blocks = m_blocksWithLiveDefinition[object.index()];
        if (!blocks) [[unlikely]]
            return findBlocksWithLiveDefinition(object).quickGet(block.index());
        return blocks->quickGet(block.index());
    }

    uint64_t limit(LimitID) const;
    void didWork(uint64_t amount) { m_work += amount; }
    void startWorkOver() { m_work = 0; }
    void didCarry(VirtualObjectID object) { ++m_blocksCarrying[object.index()]; }
    bool hasSpentItsWork() const { return m_work > m_maxWork; }
    bool hasSpentMoreThanOneSweepMayTake() const { return m_work > m_maxWork * timesTheWorkThatOneSweepMayTake; }
    bool hasStartedOverWithFewerCandidates() const { return m_hasStartedOverWithFewerCandidates; }
    bool exceeds(LimitID, uint64_t amount, ValueID);
    void abandon(VirtualObjectID, LimitID = LimitID::None);
    void abandonJoinsOfFamilyOf(VirtualObjectID);
    void tagVariants(BlockID, VirtualObjectID, ObjectState&);
    void buildAtHeadForLackOfTagBits(BlockID, VirtualObjectID);
    bool isIgnored(Edge) const;
    bool isOutsideTrialRange(BlockID block) const { return m_trialRange && !m_trialRange->quickGet(block.index()); }
    void addVirtualReturn(VirtualReturn&&);
    void addEdgeNeedingCode(Edge);
    void didEnterJoin(VirtualObjectID join, VirtualObjectID source) { m_entriesIntoJoins.append(EntryIntoJoin { join, source }); }
    void didBuildAfterMerge(VirtualObjectID, BlockID mergeBlock);
    bool buildCycleBeforeBranches(const SinkingState&, VirtualObjectID holder, VirtualObjectID held);
    void buildUnderHandler(VirtualObjectID);
    BlockID handlerOf(BlockID block) const { return m_inputs.handlers[block.index()]; }
    bool mayBeListOfNumbers(VirtualObjectID) const;
    void didTestListThatMayNotFit(ValueID literal) { m_testedListsThatMayNotFit.appendIfNotContains(literal); }

    RewriteTables& tables() { return *m_tables; }
    RewriteCursor& cursor() { return *m_cursor; }
    ValueID entryConstant(Value&&);
    void forEachHeadValue(const SinkingState& head, NOESCAPE const ScopedLambda<void(const HeadValue&)>&) const;
    Vector<ValueID, 8> collectHeadValues(ObjectInterpreter&, BlockID target, bool isHandlerEdge);
    TypeID typeOfHeadValue(const SinkingState& head, const HeadValue&, TypeArena&) const;

private:
    friend class RewriteCursor;

    Vector<DescribedObject> collectAllocations();
    bool findsThatNoAllocationPays(std::span<const DescribedObject>);
    void collectJoins();
    void uniteFamilies(Vector<uint32_t>& parentOfCandidate);
    void createFamilies(std::span<const DescribedObject>, std::span<const uint32_t> rootOfCandidate);
    bool isJoinParameter(BlockID, uint32_t parameterIndex) const;
    void findNames();
    const BitVector& findBlocksWithLiveDefinition(VirtualObjectID);
    void findWhetherObjectsAreBuiltBeforeBranches();
    void computeInputs();
    void computeInputsOf(BlockID);

    void solve();
    void solveAgainIn(std::span<const BlockID> range);
    void giveUpOnEveryCandidate();
    bool startOverWithFewerCandidates();
    void giveUpOnCandidatesCarriedTooFar();
    void giveUpForLackOfWork(std::span<const uint32_t> candidates);
    void analyzeUntilNothingChanges();
    void setAsideJoinsThatCarryOnlyCells();
    void setAsideJoinsOfAllocationsBuiltInTheirBlocks();
    bool isBuiltInItsBlock(const Candidate&) const;
    bool isBuiltAtOnce(const Candidate&) const;
    void abandonAllocationsBuiltAtOnce();
    void setAside(uint32_t join);
    void takeBackJoinsSetAside();
    void invalidateResults();
    void count();
    void countIn(std::span<const BlockID> range);
    void countBlock(BlockID);
    bool abandonUnprofitableCandidates();
    bool buildBeforeBranchesInsteadOfAbandoning();
    void abandonJoinsThatOnlyFeedAbandonedJoins();
    bool paysToSink(const Candidate&) const;
    void resetCounters();
    void willAccessInTrial(uint32_t candidate);

    bool meetsObjectMadeOnAnotherWay(const Candidate&) const;
    bool handOnToNextJoin(IRBuilder&, const Candidate& join);
    Vector<Edge, 2> collectCellEdges(VirtualObjectID, BlockID mergeBlock);
    Vector<BlockID> collectLiveRange(VirtualObjectID, BlockID mergeBlock);
    bool savesAllocation(VirtualObjectID, BlockID merge, const BitVector& allocatedOnEveryPath) const;
    bool mayPayWithoutEdges(VirtualObjectID, std::span<const Edge>, std::span<const BlockID> range, const BitVector& allocatedOnEveryPath);
    bool paysWithoutEdges(VirtualObjectID, std::span<const Edge>);
    void duplicate(IRBuilder&, std::span<const BlockID> region, std::span<const Edge> cellEdges);

    void addHeadParameters(IRBuilder&);
    bool needsHeadParameters(BlockID) const;
    bool isLeftAsItIs(BlockID) const;
    void rewriteBlock(BlockID);
    void rewriteEdge(ObjectInterpreter&, Terminator&, const OutgoingEdge&);
    bool foldTerminator(ObjectInterpreter&, Terminator&);
    void convertProofsToArguments(ObjectInterpreter&, const OutgoingEdge&, Target&);
    void loadHeadValues(BlockID, const SinkingState& head, ObjectValueMap&);
    void fillTarget(ObjectInterpreter&, const OutgoingEdge&, Target&);
    bool isBoundParameter(BlockID, ValueID parameter) const;
    void removeBoundParameters(IRBuilder&);

    void recordRemovedAllocation(const Candidate&);
    void recordDecline(const Candidate&);

    FunctionAnalyses& m_analyses;
    const Program& m_program;
    const Function& m_function;
    const SinkingPolicy m_policy;
    Vector<std::optional<VirtualConstant>> m_constantsBeforeRewriting;
    const VirtualObjectContext m_objectContext;
    Vector<Candidate> m_candidates;
    Vector<VirtualObjectID> m_candidateOfValue;
    mutable std::optional<LivenessOfAskedValues> m_liveness;
    Vector<std::optional<BitVector>> m_blocksWithLiveDefinition;
    Vector<uint64_t> m_tracesOfAllocations;
    Vector<VirtualFamily> m_families;
    mutable std::optional<Vector<VirtualObjectID>> m_objectsWhoseCreationMayBeOrderedWithGuards;
    KeyOrderTable m_orders;
    SinkingDecisions m_decisions;
    UncheckedKeyHashMap<uint32_t, Vector<ValueID, 2>> m_guardsOfNoObject;
    UncheckedKeyHashMap<uint32_t, uint32_t> m_roadsCounted;
    SinkingInputs m_inputs;
    SinkingNames m_names;
    struct RelevantValues {
        uint32_t first { 0 };
        uint32_t end { 0 };
    };
    BitVector m_preparedBlocks;
    Vector<RelevantValues> m_relevantValuesOfBlocks;
    Vector<uint32_t> m_relevantValues;
    // A family gets fields while it is analyzed.
    struct LoadableFields {
        uint32_t numberOfFieldsLookedAt { 0 };
        BitVector fields;
    };
    Vector<LoadableFields> m_loadableFields;
    const bool m_checksShortcuts;
    bool m_buildsSomeObjectBeforeBranches { false };
    bool isEnteredWithNothing(BlockID block) const { return m_blocksEnteredWithNothing.get(block.index()); }

    // A block of the set has no state in the list until somebody asks for it.
    mutable Heads m_heads;
    mutable BitVector m_blocksEnteredWithNothing;
    Vector<uint32_t> m_joinsSetAside;
    struct EntryIntoJoin {
        VirtualObjectID join;
        VirtualObjectID source;

        friend bool operator==(const EntryIntoJoin&, const EntryIntoJoin&) = default;
    };
    Vector<EntryIntoJoin> m_entriesIntoJoins;
    bool m_setsAsideJoins { false };
    Vector<VirtualReturn, 2> m_returns;
    Vector<Edge, 2> m_ignoredEdges;
    const BitVector* m_trialRange { nullptr };
    // A trial saves a candidate when it first asks for it, and resets its counters when it first asks for it after resetCounters(): it costs the same however many candidates there are.
    struct TrialsOfCandidate {
        uint32_t lastSavedIn { 0 };
        uint32_t lastResetIn { 0 };
    };
    Vector<TrialsOfCandidate> m_trialsOfCandidates;
    Vector<std::pair<uint32_t, Candidate>> m_candidatesSavedInTrial;
    uint32_t m_trial { 0 };
    bool m_didResetCountersInTrial { false };
    Vector<Edge, 2> m_edgesNeedingCode;
    Vector<ValueID, 2> m_testedListsThatMayNotFit;
    const FreshResultShape* m_resultShape { nullptr };
    bool m_mustSolveAgain { false };
    uint64_t m_work { 0 };
    // Since the analysis last started over: how often it went through the function, and through how many blocks it carried each candidate.
    uint64_t m_numberOfSweeps { 0 };
    Vector<uint32_t> m_blocksCarrying;
    uint64_t m_maxWork;
    bool m_hasRecordedLackOfWork { false };
    bool m_hasStartedOverWithFewerCandidates { false };
    std::optional<RewriteTables> m_tables;
    std::optional<RewriteCursor> m_cursor;
};

// The operands of a call of which native code takes nothing but the bytes, and the overload that takes them from a part of each view.
struct BytesOperands {
    BuiltinOverloadID overloadInRanges;
    Vector<uint32_t, 2> operandIndexes;
};
std::optional<BytesOperands> findBytesOperands(const Program&, const Function&, const Value& call, BuiltinCallForm formOfCall);

bool isFrameAllocatable(const AllocationSinking&, const Candidate&);
// It goes where an operand of it is an object that is not built there.
inline bool goesWithUnbuiltOperands(Opcode opcode)
{
    return familyOf(opcode) == OperationFamily::RuntimeValidation || opcode == Opcode::ReleaseHint;
}

bool buildsOperand(const Program&, const Value& user, uint32_t operandIndex);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
