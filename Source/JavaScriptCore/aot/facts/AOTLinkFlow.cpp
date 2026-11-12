/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkFlow.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTLinkStrings.h"
#include "AOTLinkUnions.h"
#include "AOTNameClass.h"
#include "AOTPhaseTimes.h"
#include "AOTRowsOfLayouts.h"
#include "AOTTestRuns.h"
#include "AOTThreads.h"
#include "AOTTypeRelations.h"
#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <span>
#include <tuple>
#include <wtf/FastMalloc.h>
#include <wtf/CommaPrinter.h>
#include <wtf/HexNumber.h>
#include <wtf/PrintStream.h>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t noNodeInLinkFlow = UINT32_MAX;
static constexpr uint32_t numberOfEdgesLookedThroughInLinkFlow = 8;
static constexpr uint32_t numberOfSlotsLookedThroughInLinkFlow = 8;

enum class FixedNodeOfLinkFlow : uint32_t {
    Unseen,
    ReceiverOfLostFunctions,
    ResultOfLostFunctions,
    MadeForLostFunctionsWithoutClass,
};
static constexpr uint32_t numberOfFixedNodesInLinkFlow = static_cast<uint32_t>(FixedNodeOfLinkFlow::MadeForLostFunctionsWithoutClass) + 1;

static_assert(!static_cast<uint32_t>(FixedNodeOfLinkFlow::Unseen));

static constexpr FlowNodeID fixedNodeInLinkFlow(FixedNodeOfLinkFlow node)
{
    return FlowNodeID(static_cast<uint32_t>(node));
}

static constexpr uint16_t withoutLiteralsInLinkFlow(uint16_t primitives)
{
    return LinkType::withoutLiterals(primitives);
}

static constexpr uint16_t withoutLiteralsThatAreRareInLinkFlow(uint16_t primitives)
{
    if (!(primitives & (Primitive::LiteralString | Primitive::LiteralSymbol))) [[likely]]
        return LinkType::closed(primitives & (Primitive::Defined | LinkType::int32 | LinkType::oneSymbol));
    return LinkType::withoutLiterals(primitives);
}

static ALWAYS_INLINE bool bringsNoPrimitiveInLinkFlow(uint16_t arriving, uint16_t held)
{
    if (arriving & (Primitive::LiteralString | Primitive::LiteralSymbol)) [[unlikely]]
        return !(withoutLiteralsInLinkFlow(arriving) & ~held);
    return !(arriving & ~held);
}

static constexpr uint16_t primitivesOfTypeInLinkFlow(uint16_t primitives, Range integers)
{
    return withoutLiteralsInLinkFlow(LinkType::primitivesOf(primitives, integers));
}

static LinkType typeOfPrimitivesInLinkFlow(uint16_t primitives)
{
    LinkType type;
    type.primitives = withoutLiteralsInLinkFlow(primitives);
    if (!type.hasNumbers())
        type.counts = LinkType::Counts::BelowTwoTo31;
    else if (!(type.primitives & Primitive::Number))
        type.counts = LinkType::Counts::Int32;
    return type;
}

static LinkType typeOfLayoutInLinkFlow(LayoutID layout)
{
    LinkType type;
    type.objects = LinkType::Objects::Layout;
    type.layout = layout;
    return type;
}

static LinkType typeOfPrototypeInLinkFlow(LayoutID layout)
{
    LinkType type;
    type.objects = LinkType::Objects::Prototype;
    type.layout = layout;
    return type;
}

static LinkType typeOfPrototypesInLinkFlow(FamilyID family)
{
    LinkType type;
    type.objects = LinkType::Objects::Prototypes;
    type.family = family;
    return type;
}

static LinkType typeOfFamilyInLinkFlow(FamilyID family)
{
    LinkType type;
    type.objects = LinkType::Objects::Family;
    type.family = family;
    return type;
}

static LinkType typeOfClosureInLinkFlow(FunctionID function)
{
    LinkType type;
    type.functions = LinkType::Functions::Closure;
    type.function = function;
    return type;
}

static LinkType typeOfBuiltinInLinkFlow(BuiltinID builtin)
{
    LinkType type;
    type.builtin = builtin;
    return type;
}

static LinkType typeOfUnseenObjectsInLinkFlow()
{
    LinkType type;
    type.hasUnknownObjects = true;
    return type;
}

namespace {

class RowOfBasesInLinkFlow {
public:
    RowOfBasesInLinkFlow(const Program& program, LayoutID first)
        : m_program(program)
        , m_current(first)
        , m_trailing(first)
    {
    }

    LayoutID current() const { return m_current; }

    void advance()
    {
        m_current = m_program.at(m_current).base;
        if (m_trailingMovesNext)
            m_trailing = m_program.at(m_trailing).base;
        m_trailingMovesNext = !m_trailingMovesNext;
        if (m_current && m_current == m_trailing)
            m_current = { };
    }

private:
    const Program& m_program;
    LayoutID m_current;
    LayoutID m_trailing;
    bool m_trailingMovesNext { false };
};

} // anonymous namespace

bool isPrivateNameInLink(const Program& program, PropertyKeyID key)
{
    return key && program.at(key).kind == PropertyKeyKind::PrivateName;
}

template<typename Functor>
static void forEachFieldListedInLinkFlow(const Program& program, LayoutID layout, const Functor& functor)
{
    for (RowOfBasesInLinkFlow row(program, layout); row.current(); row.advance()) {
        const Layout& current = program.at(row.current());
        for (uint32_t position = 0; position < current.fields.size(); ++position)
            functor(FieldReference { row.current(), position });
        if (current.inheritedFields == InheritedFields::AllOfRow)
            break;
    }
}

static bool keepsValuesInFieldsInLinkFlow(LayoutKind kind)
{
    switch (kind) {
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
    case LayoutKind::BuiltinResult:
        return true;
    case LayoutKind::ModuleNamespace:
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool mayHaveAppendedFieldsInLinkFlow(const Layout& layout)
{
    return keepsValuesInFieldsInLinkFlow(layout.kind) && !layout.has(LayoutFlags::IsNeverAllocated);
}

static void arriveWhatIsEstablishedInLinkFlow(LinkFlow& flow, FlowNodeID node, const Field& field)
{
    std::optional<ProvenInEveryVersion> established = field.declared.established();
    if (!established)
        return;
    if (TypeID type = *established)
        flow.arriveFromSource(node, type);
}

static void sortNumbersInLinkFlow(Vector<uint64_t>& numbers, Vector<uint64_t>& room, size_t numberOfNodes)
{
    static constexpr unsigned bitsOfDigit = 12;
    static constexpr size_t fewestForDigits = 4096;
    if (numbers.size() < fewestForDigits) {
        std::ranges::sort(numbers);
        return;
    }
    room.resize(numbers.size());
    std::array<uint32_t, 1 << bitsOfDigit> places;
    for (unsigned shift = 32; shift < 32 + std::bit_width(numberOfNodes); shift += bitsOfDigit) {
        places.fill(0);
        for (uint64_t number : numbers)
            ++places[(number >> shift) & ((1 << bitsOfDigit) - 1)];
        uint32_t next = 0;
        for (uint32_t& place : places)
            next += std::exchange(place, next);
        for (uint64_t number : numbers)
            room[places[(number >> shift) & ((1 << bitsOfDigit) - 1)]++] = number;
        std::swap(numbers, room);
    }
}

class EdgesOfBusyNodesInLinkFlow {
public:
    bool add(uint32_t from, uint32_t to, uint32_t passes)
    {
        if ((m_size + 1) * 10 > m_slots.size() * 7)
            grow();
        if (!insert(Slot { from, to, passes }))
            return false;
        ++m_size;
        return true;
    }

private:
    struct Slot {
        uint32_t from { UINT32_MAX };
        uint32_t to { 0 };
        uint32_t passes { 0 };
    };

    bool insert(const Slot& slot)
    {
        size_t mask = m_slots.size() - 1;
        Slot* slots = m_slots.mutableSpan().data();
        uint64_t words = (static_cast<uint64_t>(slot.from) << 32 | slot.to) ^ static_cast<uint64_t>(slot.passes) << 15;
        for (size_t place = (words * 0x9e3779b97f4a7c15ull) >> m_shift;; place = (place + 1) & mask) {
            Slot& held = slots[place];
            if (held.from == UINT32_MAX) {
                held = slot;
                return true;
            }
            if (held.from == slot.from && held.to == slot.to && held.passes == slot.passes)
                return false;
        }
    }

    void grow()
    {
        Vector<Slot> old = std::exchange(m_slots, { });
        m_slots.fill(Slot { }, old.isEmpty() ? 1024 : old.size() * 2);
        m_shift = 64 - std::countr_zero(m_slots.size());
        for (const Slot& slot : old) {
            if (slot.from != UINT32_MAX)
                insert(slot);
        }
    }

    Vector<Slot> m_slots;
    size_t m_size { 0 };
    unsigned m_shift { 64 };
};

class ListedFlowObserver {
public:
    ListedFlowObserver(const FlowObserver& observer, uint32_t next)
        : m_kindAndFunction(static_cast<uint32_t>(observer.kind) << bitsOfFunction | (observer.function.bits() + 1))
        , m_record(observer.record)
        , m_next(next)
    {
        RELEASE_ASSERT(observer.function.bits() + 1 < 1u << bitsOfFunction);
    }

    FlowObserver::Kind kind() const { return static_cast<FlowObserver::Kind>(m_kindAndFunction >> bitsOfFunction); }
    FlowObserver observer() const { return { kind(), FunctionID::fromBits((m_kindAndFunction & ((1u << bitsOfFunction) - 1)) - 1), m_record }; }
    uint32_t next() const { return m_next; }

private:
    static constexpr unsigned bitsOfFunction = 27;
    static_assert(static_cast<uint32_t>(FlowObserver::Kind::FieldIsOnlyAsked) < 1u << (32 - bitsOfFunction));

    uint32_t m_kindAndFunction;
    uint32_t m_record;
    uint32_t m_next;
};
static_assert(sizeof(ListedFlowObserver) == 12);

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

template<typename T>
class SegmentedListOfLane {
    WTF_MAKE_NONCOPYABLE(SegmentedListOfLane);
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>);
public:
    SegmentedListOfLane() = default;
    ~SegmentedListOfLane()
    {
        for (size_t segment = 0; segment < m_addressesOfIndexZero.size(); ++segment)
            fastFree(reinterpret_cast<void*>(m_addressesOfIndexZero[segment] + (segment << logOfSegmentSize) * sizeof(T)));
    }

    size_t size() const { return m_size; }
    ALWAYS_INLINE const T& operator[](size_t index) const LIFETIME_BOUND
    {
        RELEASE_ASSERT(index < m_size);
        return *reinterpret_cast<const T*>(m_addressesOfIndexZero.span().data()[index >> logOfSegmentSize] + index * sizeof(T));
    }
    ALWAYS_INLINE void append(const T& value)
    {
        if (m_next == m_endOfSegment) [[unlikely]]
            addSegment();
        new (NotNull, m_next++) T(value);
        ++m_size;
    }

private:
    static constexpr unsigned logOfSegmentSize = 16;

    NEVER_INLINE void addSegment()
    {
        static constexpr size_t bytesOfSegment = sizeof(T) << logOfSegmentSize;
        uint8_t* segment = static_cast<uint8_t*>(fastMalloc(bytesOfSegment));
        m_addressesOfIndexZero.append(reinterpret_cast<uintptr_t>(segment) - m_size * sizeof(T));
        m_next = reinterpret_cast<T*>(segment);
        m_endOfSegment = reinterpret_cast<T*>(segment + bytesOfSegment);
    }

    Vector<uintptr_t> m_addressesOfIndexZero;
    T* m_next { nullptr };
    T* m_endOfSegment { nullptr };
    size_t m_size { 0 };
};

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

struct alignas(128) LinkFlow::Lane {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Lane);

    unsigned index { 0 };
    SegmentedListOfLane<Edge> edges;
    SegmentedListOfLane<ListedFlowObserver> observers;
    LinkReferenceTable::RecentNumbers recentNumbers;
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> nodesThatAccessorsFoundAsked;
    EdgesOfBusyNodesInLinkFlow edgesOfBusyNodes;
    Vector<uint64_t> numbersOfNodesToTell;
    Vector<uint64_t> roomToSort;
    Vector<FlowNodeID> worklist;
    Vector<LinkType> typesBeforeClimb;
    size_t firstInWorklist { 0 };
    Vector<std::pair<FlowNodeID, LinkType>> nodesToTell;
    Vector<Vector<ArrivalHeldBack>> boxes;
    Vector<Vector<EdgeLaid>> edgesLaid;
    Vector<Vector<ObserverPut>> observersPut;
    bool hasFilledBoxes { false };
    bool hasEdgesLaid { false };
    bool hasObserversPut { false };
    uint32_t lanesOfFilledBoxesInThisPass { 0 };
    uint32_t lanesOfEdgesLaidInThisPass { 0 };
    uint32_t lanesOfObserversPutInThisPass { 0 };
    struct Tell {
        FlowObserver observer;
        FlowNodeID node;
        uint32_t placeOfTypeBefore;
    };
    Vector<Vector<Tell>> tells;
    Vector<Tell> tellsForOneThread;

    CopiesObserved copiesObserved;
    BitVector layoutsWhoseOwnValuesAreSeen;
    Vector<std::pair<LayoutID, AccessorLaidInField>> accessorsLaid;
    Vector<std::pair<FlowObserver, FlowNodeID>> newObservers;
    Vector<FunctionID> functionsToReach;
    Vector<FunctionID> functionsToBringAlive;
    Vector<LeftToReach> leftToReach;
    struct ContentsOfLane {
        uint32_t site;
        SlotOfContents slot;
        FlowNodeID node;
    };
    Vector<std::pair<FlowNodeID, LayoutID>> sourcesOfObjects;
    Vector<FunctionID> functionsToTellBuiltinsInReach;
    Vector<Vector<ContentsOfLane>> contentsMade;
    Vector<Vector<ContentsOfLane>> contentsGiven;
    Vector<TiesToLay> tiesToLay;
    std::array<HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>, numberOfKindsMadeOnDemand> given;
    struct NoteTaken {
        Note note;
        uint32_t number;
        uint32_t more;
    };
    Vector<NoteTaken> notes;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> notesListed;
    std::array<uint64_t, 1024> notesListedLately { };
    BitVector layoutsWithNotedBases;
    HashMap<uint32_t, Vector<uint32_t>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> fieldsNotNotedEmpty;
    LinkType typeObservedBefore;
    uint64_t numberOfObserversTold { 0 };
    Vector<std::pair<FlowNodeID, LinkType>> meetings;
    Vector<Deferred> deferred;
    Vector<BuiltinID> builtinsLost;
    Vector<BuiltinID> builtinsLostAtJoins;
    Vector<BuiltinID> builtinsLostInSight;
    bool isInLibraryCode { false };
    Vector<LayoutID> layoutsMetInFamilies;
    Vector<LinkType> typesAtUnseen;
    HashMap<uint32_t, TestOfEdge, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> testsOfEdges;
    HashMap<uint32_t, std::unique_ptr<InstanceOfGuardedReads>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> instanceOfGuardedReads;
    uint64_t numberOfNodesTakenOffWorklist { 0 };
    uint64_t scatteredNumber { 88172645463325252ull };
};

static ALWAYS_INLINE void forEachLaneIn(uint32_t lanes, NOESCAPE const Invocable<void(unsigned lane)> auto& functor)
{
    for (; lanes; lanes &= lanes - 1)
        functor(static_cast<unsigned>(std::countr_zero(lanes)));
}

template<typename T>
static ALWAYS_INLINE uint32_t lanesOfFilledListsIn(const Vector<Vector<T>>& lists, size_t& numberInAll)
{
    uint32_t lanes = 0;
    for (unsigned lane = 0; lane < lists.size(); ++lane) {
        size_t number = lists[lane].size();
        numberInAll += number;
        lanes |= static_cast<uint32_t>(!!number) << lane;
    }
    return lanes;
}

constinit thread_local LinkFlow::Lane* LinkFlow::s_laneAtWork = nullptr;
constinit thread_local unsigned LinkFlow::s_depthOfLockOfWhatIsShared = 0;
const LinkType LinkFlow::s_nothing;

LinkFlow::~LinkFlow() = default;

static constexpr bool hasPrivateFlowNode(const SummaryNode& node)
{
    return node.kind >= SummaryNode::Kind::Contents;
}

LinkFlow::LinkFlow(Program& program, const ThinSummaries& summaries, const Families& families, const LooksAtBodies& looks, Mechanisms mechanisms, BuildContext& build, OrderOfWorklist order)
    : program(program)
    , summaries(summaries)
    , families(families)
    , looks(looks)
    , mechanisms(mechanisms)
    , build(build)
    , loops(build.parallelLoops())
    , isGivenMore(AOT::hasFlag(build.options().testing, Wire::TestingSwitch::WidenFlowNodes))
    , m_order(order)
{
    RELEASE_ASSERT(summaries.size() == program.functions.size());
    m_keysOfLayoutAreTold = BytesSharedByThreads(program.layouts.size());
    m_nameOfFunctionIsTold = BytesSharedByThreads(program.functions.size());
    m_functionsThatReadOffViewsMadeAtSites = BytesSharedByThreads(program.functions.size());
    for (size_t index : summaries.namesUnderWhichEventsHang()) {
        BuiltinName name(static_cast<uint32_t>(index));
        if (PropertyKeyID key = program.keyOf(name))
            m_keysThatNameEvents.set(key.index());
        if (StringID text = program.stringOf(name))
            m_textsThatNameEvents.set(text.index());
    }

    unsigned numberOfLanes = 32;
    if (order == OrderOfWorklist::Scattered) {
        numberOfLanes = 4;
        m_log2OfNodesInBlock = 6;
    }
    RELEASE_ASSERT(numberOfLanes <= 32);
    m_maskOfLanes = numberOfLanes - 1;
    for (unsigned lane = 0; lane < numberOfLanes; ++lane) {
        m_lanes.append(makeUnique<Lane>());
        m_lanes[lane]->index = lane;
        m_lanes[lane]->scatteredNumber += lane;
        m_lanes[lane]->boxes.grow(numberOfLanes);
        m_lanes[lane]->edgesLaid.grow(numberOfLanes);
        m_lanes[lane]->observersPut.grow(numberOfLanes);
        m_lanes[lane]->tells.grow(numberOfLanes);
        m_lanes[lane]->contentsMade.grow(numberOfLanes);
        m_lanes[lane]->contentsGiven.grow(numberOfLanes);
        m_contentsOfSites.append(makeUnique<PartOfContentsOfSites>());
    }
    m_firstLane = m_lanes[0].get();

    Vector<uint32_t> layoutsThatListComputedKeys;
    BinarySemaphore membersOfClassesAreIndexed;
    runLanes(loops, {
        [&] {
            StepTimes stepsOfLane(build, "facts"_s);
            Vector<uint32_t> numbersOfMemberNodesOfSides = indexMembersOfClasses();
            membersOfClassesAreIndexed.signal();
            stepsOfLane.didFinish("the flow: members of classes"_s);
            numberNodes(WTF::move(numbersOfMemberNodesOfSides));
            stepsOfLane.didFinish("the flow: nodes"_s);
        },
        [&] {
            StepTimes stepsOfLane(build, "facts"_s);
            indexConstructorFunctions();
            m_nameClassesOfKeys.grow(program.types.numberOfPropertyKeys());
            forEachRangeOfIndicesInParallel(build.parallelLoops(), m_nameClassesOfKeys.size(), [&](size_t first, size_t end) {
                for (size_t key = first; key < end; ++key)
                    m_nameClassesOfKeys[key] = program.nameClassOf(PropertyKeyID(key));
            });
            size_t numberOfLayouts = program.layouts.size();
            m_layoutsWithFieldsUnderComputedKeys.ensureSize(numberOfLayouts);
            layoutsThatListComputedKeys = findIndicesInParallel(build.parallelLoops(), static_cast<unsigned>(numberOfLayouts), [&](unsigned index) {
                bool listsComputedKey = false;
                forEachFieldListedInLinkFlow(program, LayoutID(index), [&](FieldReference reference) {
                    listsComputedKey |= !!fieldAt(program, reference).computedKey;
                });
                return listsComputedKey;
            });
            for (uint32_t index : layoutsThatListComputedKeys)
                m_layoutsWithFieldsUnderComputedKeys.quickSet(index);
            stepsOfLane.didFinish("the flow: constructor functions, name classes, computed keys"_s);
            membersOfClassesAreIndexed.wait();
            findSidesBelowIncompleteNames();
            stepsOfLane.didFinish("the flow: sides below incomplete names"_s);
            findWhatRowsOfConstructorsFill();
            stepsOfLane.didFinish("the flow: rows of constructors"_s);
            findLayoutsBelowMembersUnderNumbers();
            stepsOfLane.didFinish("the flow: layouts below members under numbers"_s);
        },
        [&] {
            StepTimes stepsOfLane(build, "facts"_s);
            findSuperCallsThatMayAnswerWithOlderObject();
            stepsOfLane.didFinish("the flow: super calls, tree of bases"_s);
        },
        [&] {
            StepTimes stepsOfLane(build, "facts"_s);
            makeTablesThatNeedNoNodes();
            stepsOfLane.didFinish("the flow: tables that need no nodes"_s);
        },
    });

    findWhatNativeCodeHandsToLibrary();
    fieldsWrittenAfterConstruction.ensureSize(m_firstFieldOfLayout.last());
    fieldsThatMayBeEmpty.ensureSize(m_firstFieldOfLayout.last());
    fieldsNotOfBirth.ensureSize(m_firstFieldOfLayout.last());
    m_classesWithLockableInstances.ensureSize(program.layouts.size());
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (const Layout& layout = program.layouts[index]; layout.kind != LayoutKind::Instance || !isFoundByShapeTest(layout))
            noteLockableInstances(LayoutID(index));
    }
    gettersThatMayBeAbsent.ensureSize(m_firstFieldOfLayout.last());
    settersThatMayBeAbsent.ensureSize(m_firstFieldOfLayout.last());

    for (uint32_t index : layoutsThatListComputedKeys) {
        forEachFieldListedInLinkFlow(program, LayoutID(index), [&](FieldReference reference) {
            if (!fieldAt(program, reference).computedKey)
                return;
            fieldsWrittenAfterConstruction.quickSet(m_firstFieldOfLayout[reference.layout.index()] + reference.field);
            addEdge(field(reference), unseen());
            addEdge(unseen(), field(reference));
        });
    }
    RELEASE_ASSERT(!m_stateOfLinkReach);
}

void LinkFlow::makeTablesThatNeedNoNodes()
{
    m_testRuns = makeUnique<TestRunsInLink>(*this);
    size_t numberOfFunctions = program.functions.size();
    size_t numberOfLayouts = program.layouts.size();
    reached.ensureSize(numberOfFunctions);
    iterationCallbacks.ensureSize(numberOfFunctions);
    repeatedCallbacks.ensureSize(numberOfFunctions);
    lostFunctions.ensureSize(numberOfFunctions);
    escapedFunctions.ensureSize(numberOfFunctions);
    libraryFunctionsEntered.ensureSize(numberOfFunctions);
    exportsShown.ensureSize(program.variables.size());
    allocatedLayouts.ensureSize(numberOfLayouts);
    constructedClasses.ensureSize(numberOfLayouts);
    lostLayouts.ensureSize(numberOfLayouts);
    escapedLayouts.ensureSize(numberOfLayouts);
    escapedVariables.ensureSize(program.variables.size());
    layoutsWithKnownAccessors.ensureSize(program.layouts.size());
    layoutsWhoseOwnFieldsListAccessor.ensureSize(numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (std::ranges::any_of(program.at(LayoutID(index)).fields, [](const Field& field) { return field.form.contains(FieldForm::IsAccessor); }))
            layoutsWhoseOwnFieldsListAccessor.quickSet(index);
    }
    layoutsDefinedOnUnderWrittenKeys.ensureSize(program.layouts.size());
    layoutsDefinedOnUnderKeysThatAreNotWritten.ensureSize(program.layouts.size());
    layoutsGivenAccessorsByDescriptorsInSight.ensureSize(program.layouts.size());
    layoutsThatGetNullAbove.ensureSize(program.layouts.size());
    layoutsWhoseInstancesAreLaidAsPrototypes.ensureSize(program.layouts.size());
    prototypesLaidAboveOtherObjects.ensureSize(program.layouts.size());
    taggedMakers = findTaggedMakers(program, summaries, mechanisms);
    findKeysDefinedByDescriptors();
    possibleChanges.fill(OptionSet<LayoutChange> { }, numberOfLayouts);
    changesByMoreThanStoresInSight.fill(OptionSet<LayoutChange> { }, numberOfLayouts);

    m_extraPropertyNodes.fill(noNodeInLinkFlow, 2 * numberOfLayouts);
    m_unknownKeyStoreNodes.fill(noNodeInLinkFlow, numberOfLayouts);
    m_numericKeyStoreNodes.fill(noNodeInLinkFlow, numberOfLayouts);
    m_nonNumericKeyReadNodes.fill(noNodeInLinkFlow, numberOfLayouts);
    m_nodesStoredWhereAccessorStandsAbove.fill(noNodeInLinkFlow, numberOfLayouts);
    m_newestAppendedFieldOfLayout.fill(UINT32_MAX, numberOfLayouts);

    for (BitVector* byLayout : { &layoutsMetInFamilies, &m_layoutsToldLost, &m_layoutsToldToEscape, &m_layoutsAtUnseen, &m_prototypesAtUnseen, &m_openLayouts, &m_lostLayoutsWhoseAppendedFieldsAreTied, &m_layoutsWithPooledAppendedFields, &m_layoutsWithJoinedProperties })
        byLayout->ensureSize(numberOfLayouts);
    for (BitVector* byFunction : { &m_functionsToldLost, &m_functionsToldToEscape, &m_functionsAtUnseen, &m_lostFunctionsTied, &m_familiesOfFunctionsAtUnseen, &m_calledFunctionFamilies })
        byFunction->ensureSize(numberOfFunctions);
    unitedFunctions.growTo(static_cast<uint32_t>(numberOfFunctions));
    m_poolOfFamilyOfFunctions.fill(UINT32_MAX, numberOfFunctions);
    for (BitVector* byFamily : { &m_familiesToldLost, &m_familiesAtUnseen, &m_constructorsToldLost, &m_constructorsAtUnseen, &m_prototypesOfFamiliesAtUnseen })
        byFamily->ensureSize(families.size());
    m_outermostFamiliesWhereSomethingBefell.ensureSize(families.size());
    m_outermostFamilies.grow(families.size());
    for (uint32_t family = families.size(); family--;) {
        FamilyID above = families.nearestFamilyAbove(FamilyID(family));
        m_outermostFamilies[family] = above ? m_outermostFamilies[above.index()] : FamilyID(family);
    }
    unitedLayouts.growTo(static_cast<uint32_t>(numberOfLayouts));
    m_lostFamilies.grow(numberOfLayouts);
}

Vector<uint32_t> LinkFlow::indexMembersOfClasses()
{
    size_t numberOfLayouts = program.layouts.size();
    size_t numberOfSides = 2 * numberOfLayouts;
    auto sideOf = [](const MemberOfClass& member) {
        return 2 * static_cast<size_t>(member.classLayout.index()) + static_cast<size_t>(member.side);
    };

    m_membersOfClasses = collectInParallel<MemberOfClass>(loops, static_cast<unsigned>(summaries.size()), [&](unsigned index, Vector<MemberOfClass>& found) {
        const ThinSummary& summary = summaries.at(FunctionID(index));
        for (const SummaryMember& member : summary.members) {
            if (member.isRedefined)
                continue;
            const SummaryNode& value = summary.nodes[member.value];
            FunctionID function = value.kind == SummaryNode::Kind::Closure ? FunctionID(value.first) : FunctionID();
            found.append(MemberOfClass { member.classLayout, member.side, member.key, member.kind, member.index, function });
        }
    });

    Vector<uint32_t> numbersOfMemberNodesOfSides;
    numbersOfMemberNodesOfSides.fill(0, numberOfSides + 1);
    m_sidesWithComputedKeys.ensureSize(numberOfSides);
    for (const MemberOfClass& member : m_membersOfClasses) {
        RELEASE_ASSERT(member.classLayout.index() < numberOfLayouts);
        uint32_t& numberOfNodes = numbersOfMemberNodesOfSides[sideOf(member) + 1];
        numberOfNodes = std::max(numberOfNodes, member.index + 1);
        if (!member.key)
            m_sidesWithComputedKeys.quickSet(sideOf(member));
    }

    m_membersOfClasses.removeAllMatching([](const MemberOfClass& member) {
        return !member.key;
    });
    auto orderOf = [](const MemberOfClass& member) {
        return std::tuple(member.classLayout, member.side, member.key, member.kind, member.index, member.function);
    };
    sortAndRemoveDuplicates(loops, m_membersOfClasses, [&](const MemberOfClass& first, const MemberOfClass& second) {
        return orderOf(first) < orderOf(second);
    }, [&](const MemberOfClass& first, const MemberOfClass& second) {
        return orderOf(first) == orderOf(second);
    });

    m_firstMemberOfSide.fill(0, numberOfSides + 1);
    for (const MemberOfClass& member : m_membersOfClasses) {
        ++m_firstMemberOfSide[sideOf(member) + 1];
        m_keysOfMembers[static_cast<size_t>(member.side)].set(member.key.index());
    }
    for (size_t side = 0; side < numberOfSides; ++side)
        m_firstMemberOfSide[side + 1] += m_firstMemberOfSide[side];
    return numbersOfMemberNodesOfSides;
}

void LinkFlow::numberNodes(Vector<uint32_t>&& numbersOfMemberNodesOfSides)
{
    const ParallelLoops& loopsOfLane = build.parallelLoops();
    size_t numberOfFunctions = program.functions.size();
    size_t numberOfLayouts = program.layouts.size();

    uint64_t next = numberOfFixedNodesInLinkFlow;
    auto take = [&](uint64_t count) {
        uint64_t first = next;
        next += count;
        return static_cast<uint32_t>(first);
    };

    m_firstVariable = take(program.variables.size());

    m_firstFieldOfLayout.grow(numberOfLayouts + 1);
    forEachRangeOfIndicesInParallel(loopsOfLane, numberOfLayouts, [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index)
            m_firstFieldOfLayout[index] = program.layouts[index].fields.size();
    });
    uint64_t numberOfFields = 0;
    for (size_t index = 0; index < numberOfLayouts; ++index)
        numberOfFields += std::exchange(m_firstFieldOfLayout[index], static_cast<uint32_t>(numberOfFields));
    RELEASE_ASSERT(numberOfFields < UINT32_MAX);
    m_firstFieldOfLayout[numberOfLayouts] = static_cast<uint32_t>(numberOfFields);
    auto endOfFieldsThatMayBeDeclaredHere = [&](size_t index) {
        return baseWhoseFieldListIsCopiedWhole(program, LayoutID(index)) ? program.layouts[index].ownFieldCount : static_cast<uint32_t>(program.layouts[index].fields.size());
    };
    auto declaresAllItsFields = [&](size_t index) {
        return program.layouts[index].ownFieldCount == program.layouts[index].fields.size();
    };
    m_firstDeclaredFieldOfLayout.grow(numberOfLayouts + 1);
    forEachRangeOfIndicesInParallel(loopsOfLane, numberOfLayouts, [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index) {
            if (declaresAllItsFields(index)) {
                m_firstDeclaredFieldOfLayout[index] = static_cast<uint32_t>(program.layouts[index].fields.size());
                continue;
            }
            m_firstDeclaredFieldOfLayout[index] = static_cast<uint32_t>(std::ranges::count_if(program.layouts[index].fields.span().first(endOfFieldsThatMayBeDeclaredHere(index)), [](const Field& field) {
                return !field.flags.contains(FieldFlags::IsInherited);
            }));
        }
    });
    uint32_t numberOfDeclaredFields = 0;
    for (size_t index = 0; index < numberOfLayouts; ++index)
        numberOfDeclaredFields += std::exchange(m_firstDeclaredFieldOfLayout[index], numberOfDeclaredFields);
    m_firstDeclaredFieldOfLayout[numberOfLayouts] = numberOfDeclaredFields;
    uint32_t firstField = take(numberOfDeclaredFields);

    size_t numberOfSides = 2 * numberOfLayouts;
    Vector<uint32_t> firstMemberNodeOfSide = WTF::move(numbersOfMemberNodesOfSides);
    RELEASE_ASSERT(firstMemberNodeOfSide.size() == numberOfSides + 1);
    for (size_t side = 0; side < numberOfSides; ++side)
        firstMemberNodeOfSide[side] = take(firstMemberNodeOfSide[side + 1]);
    firstMemberNodeOfSide[numberOfSides] = static_cast<uint32_t>(next);
    m_firstMemberNodeOfSide = WTF::move(firstMemberNodeOfSide);

    for (const BuiltinMarkedEntry& marked : program.builtins().marked(BuiltinMark::IsKeyReadOnlyByRowsThatNameIt)) {
        BuiltinName name = program.builtins().nameOfSymbol(marked.builtin);
        if (PropertyKeyID key = name ? program.keyOf(name) : PropertyKeyID())
            m_keysThatCodeTellsOfItself.append(key);
    }
    static constexpr uint8_t isOfLibrary = 1;
    static constexpr uint8_t rowsSpeakForIt = 2;
    Vector<uint8_t> whoseFunctionsAre;
    whoseFunctionsAre.grow(numberOfFunctions);
    m_nodesOfFunctions.grow(numberOfFunctions + 1);
    firstCallOfFunction.grow(numberOfFunctions + 1);
    forEachRangeOfIndicesInParallel(loopsOfLane, numberOfFunctions, [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index) {
            const Function& function = program.functions[index];
            const ThinSummary& summary = summaries.at(FunctionID(index));
            m_nodesOfFunctions[index].firstParameter = function.numberOfEntryParameters();
            m_nodesOfFunctions[index].firstResult = function.results.size();
            uint32_t numberOfPrivateNodes = 0;
            for (uint32_t inSummary = 0; inSummary < summary.nodes.size(); ++inSummary)
                numberOfPrivateNodes += hasPrivateFlowNode(summary.nodes[inSummary]);
            m_nodesOfFunctions[index].base = numberOfPrivateNodes;
            m_nodesOfFunctions[index].firstSummaryNode = summary.nodes.size();
            firstCallOfFunction[index] = summary.calls.size();
            whoseFunctionsAre[index] = 0;
            if (!isLibraryCode(program, function))
                continue;
            whoseFunctionsAre[index] = isOfLibrary;
            if (RuntimeFunctionID enclosing = function.enclosingRuntimeFunction; enclosing && program.builtins().contains(enclosing) && program.builtins().at(enclosing).role != RuntimeFunctionRole::HostBuiltin)
                whoseFunctionsAre[index] |= rowsSpeakForIt;
        }
    });
    uint64_t numberOfCalls = 0;
    uint64_t numberOfSummaryNodes = 0;
    for (size_t index = 0; index < numberOfFunctions; ++index) {
        NodesOfFunction& nodes = m_nodesOfFunctions[index];
        nodes.firstParameter = take(nodes.firstParameter);
        nodes.firstResult = take(nodes.firstResult);
        nodes.thrown = take(1);
        nodes.base = take(nodes.base);
        numberOfSummaryNodes += std::exchange(nodes.firstSummaryNode, static_cast<uint32_t>(numberOfSummaryNodes));
        numberOfCalls += std::exchange(firstCallOfFunction[index], static_cast<uint32_t>(numberOfCalls));
    }
    RELEASE_ASSERT(next <= FlowNodeID::largestIndex);
    RELEASE_ASSERT(numberOfCalls < UINT32_MAX);
    RELEASE_ASSERT(numberOfSummaryNodes < UINT32_MAX);
    uint32_t endOfFunctions = static_cast<uint32_t>(next);
    m_nodesOfFunctions[numberOfFunctions] = NodesOfFunction { endOfFunctions, endOfFunctions, endOfFunctions, endOfFunctions, static_cast<uint32_t>(numberOfSummaryNodes) };
    firstCallOfFunction[numberOfFunctions] = static_cast<uint32_t>(numberOfCalls);

    m_nodeOfField.grow(numberOfFields);
    m_firstNodeOfFields = firstField;

    forEachRangeOfIndicesInParallel(loopsOfLane, numberOfLayouts, [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index) {
            uint32_t node = firstField + m_firstDeclaredFieldOfLayout[index];
            bool declaresAll = declaresAllItsFields(index);
            for (uint32_t position = 0, end = endOfFieldsThatMayBeDeclaredHere(index); position < end; ++position) {
                if (declaresAll || !program.layouts[index].fields[position].flags.contains(FieldFlags::IsInherited))
                    m_nodeOfField[m_firstFieldOfLayout[index] + position] = node++;
            }
        }
    });
    forEachRangeOfIndicesInParallel(loopsOfLane, numberOfLayouts, [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index) {
            if (declaresAllItsFields(index) || baseWhoseFieldListIsCopiedWhole(program, LayoutID(index)))
                continue;
            uint32_t numberOfFieldsOfLayout = m_firstFieldOfLayout[index + 1] - m_firstFieldOfLayout[index];
            for (uint32_t position = 0; position < numberOfFieldsOfLayout; ++position) {
                if (!program.layouts[index].fields[position].flags.contains(FieldFlags::IsInherited))
                    continue;
                FieldReference declaration = declarationOf(program, { LayoutID(index), position });
                m_nodeOfField[m_firstFieldOfLayout[index] + position] = m_nodeOfField[m_firstFieldOfLayout[declaration.layout.index()] + declaration.field];
            }
        }
    });
    BitVector holdsNodesOfBase;
    holdsNodesOfBase.ensureSize(numberOfLayouts);
    Vector<LayoutID, 16> rowToCopyDown;
    for (size_t below = 0; below < numberOfLayouts; ++below) {
        for (LayoutID layout(below); !declaresAllItsFields(layout.index()) && baseWhoseFieldListIsCopiedWhole(program, layout) && !holdsNodesOfBase.quickSet(layout.index()); layout = writtenBaseOf(program.at(layout)))
            rowToCopyDown.append(layout);
        while (!rowToCopyDown.isEmpty()) {
            LayoutID layout = rowToCopyDown.takeLast();
            LayoutID base = writtenBaseOf(program.at(layout));
            std::ranges::copy(m_nodeOfField.span().subspan(m_firstFieldOfLayout[base.index()], program.at(base).fields.size()), m_nodeOfField.mutableSpan().subspan(m_firstFieldOfLayout[layout.index()] + program.at(layout).ownFieldCount).begin());
        }
    }

    size_t roomForNodes = next + next / 2 + 64;
    runLanes(loopsOfLane, {
        [&] {
            m_nodes.reserve(roomForNodes);
            m_nodes.grow(build.parallelLoops(), next);
            m_listsOfNodes.reserve(roomForNodes);
            m_listsOfNodes.grow(build.parallelLoops(), next);
            setTypeOfNode(unseen().index(), LinkType::anything(), LinkType { });
            for (uint32_t node = m_firstMemberNodeOfSide[0]; node < m_nodesOfFunctions[0].firstParameter; ++node)
                m_nodes[node].typeOfPosition = TypeTable::anything;
            auto bind = [&](FlowNodeID node, const DeclaredType& declared) {
                TypeID held = declared.written() ? TypeID(program.provenTypeOf(declared)) : TypeID(TypeTable::anything);
                m_nodes[node.index()].typeOfPosition = held == TypeTable::anythingOrAbsent ? TypeID() : held;
            };
            forEachRangeOfIndicesInParallel(build.parallelLoops(), numberOfFunctions, [&](size_t first, size_t end) {
                for (size_t index = first; index < end; ++index) {
                    for (uint32_t node = m_nodesOfFunctions[index].firstParameter; node <= m_nodesOfFunctions[index].thrown; ++node)
                        m_nodes[node].typeOfPosition = TypeTable::anything;
                    FunctionID id(index);
                    const Function& function = program.functions[index];
                    uint32_t numberOfParameters = function.numberOfEntryParameters();
                    for (uint32_t position = 0; position < numberOfParameters; ++position) {
                        if (const DeclaredType* declared = function.declaredEntryParameter(position))
                            bind(parameter(id, position), *declared);
                    }
                    for (uint32_t position = 0; position < function.results.size(); ++position)
                        bind(result(id, position), function.results[position]);
                }
            });
            forEachRangeOfIndicesInParallel(build.parallelLoops(), program.variables.size(), [&](size_t first, size_t end) {
                for (size_t index = first; index < end; ++index)
                    bind(variable(VariableID(index)), program.variables[index].declared);
            });
            forEachRangeOfIndicesInParallel(build.parallelLoops(), numberOfLayouts, [&](size_t first, size_t end) {
                for (size_t index = first; index < end; ++index) {
                    const Layout& layout = program.layouts[index];
                    for (uint32_t position = 0; position < endOfFieldsThatMayBeDeclaredHere(index); ++position) {
                        FieldReference reference { LayoutID(index), position };
                        if (declarationOf(program, reference) == reference)
                            bind(field(reference), layout.fields[position].declared);
                    }
                }
            });
        },
        [&] {
            m_nodesOfSummaries.grow(m_nodesOfFunctions[numberOfFunctions].firstSummaryNode);
            m_firstSiteOfFunction.grow(numberOfFunctions + 1);
            m_firstSiteOfFunction[0] = 0;
            forEachRangeOfIndicesInParallel(build.parallelLoops(), numberOfFunctions, [&](size_t first, size_t end) {
                for (size_t index = first; index < end; ++index) {
                    uint32_t numberOfNodes = m_nodesOfFunctions[index + 1].firstSummaryNode - m_nodesOfFunctions[index].firstSummaryNode;
                    std::span<uint32_t> nodes = m_nodesOfSummaries.mutableSpan().subspan(m_nodesOfFunctions[index].firstSummaryNode, numberOfNodes);
                    const ThinSummary& summary = summaries.at(FunctionID(index));
                    uint32_t numberOfSites = 0;
                    uint32_t nextPrivateNode = m_nodesOfFunctions[index].base;
                    for (uint32_t node = 0; node < numberOfNodes; ++node) {
                        nodes[node] = hasPrivateFlowNode(summary.nodes[node]) ? nextPrivateNode++ : findSharedNodeOf(FunctionID(index), node).index();
                        numberOfSites += summary.nodes[node].kind == SummaryNode::Kind::ClassInstance;
                    }
                    m_firstSiteOfFunction[index + 1] = numberOfSites;
                }
            });
            for (size_t index = 0; index < numberOfFunctions; ++index)
                m_firstSiteOfFunction[index + 1] += m_firstSiteOfFunction[index];
            m_sites.growTo(m_firstSiteOfFunction.last());
            m_familiesOfSites.growTo(m_firstSiteOfFunction.last());
        },
        [&] {
            m_isNodeOfLibrary.ensureSize(endOfFunctions);
            m_isFunctionOfLibrary.ensureSize(numberOfFunctions);
            m_isFunctionThatRowsSpeakFor.ensureSize(numberOfFunctions);
            Vector<uint32_t> contentsInLibrary = collectInParallel<uint32_t>(build.parallelLoops(), static_cast<unsigned>(numberOfFunctions), [&](unsigned index, Vector<uint32_t>& found) {
                if (!whoseFunctionsAre[index])
                    return;
                const ThinSummary& summary = summaries.at(FunctionID(index));
                uint32_t nextPrivateNode = m_nodesOfFunctions[index].base;
                for (uint32_t inSummary = 0; inSummary < summary.nodes.size(); ++inSummary) {
                    if (summary.nodes[inSummary].kind == SummaryNode::Kind::Contents)
                        found.append(nextPrivateNode);
                    nextPrivateNode += hasPrivateFlowNode(summary.nodes[inSummary]);
                }
            });
            for (size_t index = 0; index < numberOfFunctions; ++index) {
                if (!whoseFunctionsAre[index])
                    continue;
                m_isFunctionOfLibrary.quickSet(index);
                if (whoseFunctionsAre[index] & rowsSpeakForIt)
                    m_isFunctionThatRowsSpeakFor.quickSet(index);
                uint32_t end = m_nodesOfFunctions[index + 1].firstParameter;
                for (uint32_t node = m_nodesOfFunctions[index].firstParameter; node < end; ++node)
                    m_isNodeOfLibrary.quickSet(node);
            }
            for (uint32_t node : contentsInLibrary)
                m_isNodeOfLibrary.quickClear(node);
            m_isOnWorklist.ensureSize(roomForNodes);
            m_isToBeTold.ensureSize(roomForNodes);
            m_flowsIntoUnseen.ensureSize(roomForNodes);
            m_accessorsWaitAt.ensureSize(roomForNodes);
            m_hasEdgesInSet.ensureSize(roomForNodes);
            m_fieldsWhoseSlotsHaveDoors.ensureSize(numberOfDeclaredFields);
            m_fieldsThatKeepExactIntegers.ensureSize(numberOfDeclaredFields);
            for (size_t index = 0; index < numberOfLayouts; ++index) {
                LayoutKind kind = program.layouts[index].kind;
                if (kind != LayoutKind::Record && kind != LayoutKind::Instance && kind != LayoutKind::Container)
                    continue;
                for (uint32_t field = m_firstDeclaredFieldOfLayout[index]; field < m_firstDeclaredFieldOfLayout[index + 1]; ++field) {
                    m_fieldsWhoseSlotsHaveDoors.quickSet(field);
                    if (keepsExactIntegersInFields(kind))
                        m_fieldsThatKeepExactIntegers.quickSet(field);
                }
            }
        },
        [&] {
            m_fieldsWithSettersWritten.ensureSize(numberOfFields);
            m_layoutsWithAccessorsWritten.ensureSize(program.layouts.size());
            for (uint32_t cell : collectInParallel<uint32_t>(build.parallelLoops(), static_cast<unsigned>(numberOfFunctions), [&](unsigned index, Vector<uint32_t>& found) {
                const ThinSummary& summary = summaries.at(FunctionID(index));
                for (const SummaryChange& change : summary.changes) {
                    if (change.kind != SummaryChange::Kind::DefinesByDescriptor || change.lays == SummaryChange::Lays::WhatDescriptorSays || summary.nodes[change.subject].kind != SummaryNode::Kind::Allocation)
                        continue;
                    found.append(static_cast<uint32_t>(numberOfFields) + summary.nodes[change.subject].first);
                    if (change.lays != SummaryChange::Lays::Setter || !change.key)
                        continue;
                    if (std::optional<FieldReference> field = findField(program, LayoutID(summary.nodes[change.subject].first), change.key))
                        found.append(m_firstFieldOfLayout[field->layout.index()] + field->field);
                }
            }))
                (cell < numberOfFields ? m_fieldsWithSettersWritten : m_layoutsWithAccessorsWritten).quickSet(cell < numberOfFields ? cell : cell - numberOfFields);
            calleesOfCalls.grow(build.parallelLoops(), numberOfCalls);
        },
    });
}

LinkReferenceTable::LinkReferenceTable()
{
    makeRoomFor(0);
    m_records[0] = LinkType::noReferences();
}

LinkReferenceTable::~LinkReferenceTable()
{
    fastFree(m_records);
}

NEVER_INLINE void LinkReferenceTable::grow(size_t needed)
{
    size_t used = std::min<size_t>(m_nextNumber.load(std::memory_order_relaxed), m_room);
    m_room = std::max(needed, m_room + m_room / 2);
    RELEASE_ASSERT(m_room <= UINT32_MAX);
    auto* records = static_cast<References*>(fastMalloc(m_room * sizeof(References)));
    if (used)
        memcpy(records, m_records, used * sizeof(References));
    fastFree(std::exchange(m_records, records));
}

uint32_t LinkReferenceTable::takeNumber(Shard& shard)
{
    if (shard.next == shard.end) {
        shard.next = m_nextNumber.fetch_add(recordBatchSize, std::memory_order_relaxed);
        shard.end = shard.next + recordBatchSize;
        RELEASE_ASSERT(shard.next < shard.end && shard.end <= m_room);
    }
    return shard.next++;
}

uint32_t LinkReferenceTable::numberOf(const References& references, RecentNumbers& recentNumbers)
{
    uint64_t hash = hashOf(references);
    Recent& recent = recentNumbers[hash >> 32 & (recentNumbers.size() - 1)];
    if (!LinkType::areSameReferences(recent.references, references))
        recent = { references, findOrAdd(references, hash) };
    return recent.number;
}

uint32_t LinkReferenceTable::findOrAdd(const References& references, uint64_t hash)
{
    Shard& shard = m_shards[hash % numberOfShards];
    while (shard.isLocked.load(std::memory_order_relaxed) || shard.isLocked.exchange(true, std::memory_order_acquire))
        simde_mm_pause();
    if (4 * static_cast<size_t>(shard.size) >= 3 * shard.slots.size())
        growSlots(shard);
    size_t mask = shard.slots.size() - 1;
    size_t slot = hash / numberOfShards & mask;
    uint32_t number;
    while ((number = shard.slots[slot]) && !LinkType::areSameReferences(at(number), references))
        slot = (slot + 1) & mask;
    if (!number) {
        number = takeNumber(shard);
        m_records[number] = references;
        shard.slots[slot] = number;
        ++shard.size;
    }
    shard.isLocked.store(false, std::memory_order_release);
    return number;
}

uint32_t LinkFlow::numberOfReferences(const LinkType::References& references)
{
    if (Lane* lane = s_laneAtWork)
        return m_references.numberOf(references, lane->recentNumbers);
    m_references.makeRoomFor(1);
    return m_references.numberOf(references, m_recentNumbers);
}

void LinkReferenceTable::growSlots(Shard& shard)
{
    Vector<uint32_t> larger;
    larger.fill(0, std::max<size_t>(32, 2 * shard.slots.size()));
    size_t mask = larger.size() - 1;
    for (uint32_t number : shard.slots) {
        if (!number)
            continue;
        size_t slot = hashOf(at(number)) / numberOfShards & mask;
        while (larger[slot])
            slot = (slot + 1) & mask;
        larger[slot] = number;
    }
    shard.slots = WTF::move(larger);
}

FlowNodeID LinkFlow::makeNode()
{
    if (m_severalThreadsTell)
        return FlowNodeID(m_numberOfNodesMade.fetch_add(1, std::memory_order_relaxed));
    FlowNodeID node(m_nodes.size());
    m_nodes.grow(m_nodes.size() + 1);
    m_listsOfNodes.grow(m_nodes.size());
    if (m_nodes.size() > m_isOnWorklist.size()) {
        m_isOnWorklist.ensureSize(m_nodes.size() + m_nodes.size() / 4 + 64);
        m_isToBeTold.ensureSize(m_isOnWorklist.size());
        m_flowsIntoUnseen.ensureSize(m_isOnWorklist.size());
        m_accessorsWaitAt.ensureSize(m_isOnWorklist.size());
        m_hasEdgesInSet.ensureSize(m_isOnWorklist.size());
    }
    return node;
}

FlowNodeID LinkFlow::thrown(FunctionID function) const
{
    RELEASE_ASSERT(function.index() < program.functions.size());
    return FlowNodeID(m_nodesOfFunctions[function.index()].thrown);
}

FlowNodeID LinkFlow::capture(FunctionID function, uint32_t index) const
{
    RELEASE_ASSERT(index < program.at(function).captures.size());
    return parameter(function, index);
}

FlowNodeID LinkFlow::variable(VariableID id) const
{
    RELEASE_ASSERT(id.index() < program.variables.size());
    return FlowNodeID(m_firstVariable + id.index());
}

void LinkFlow::prefetchNodesOf(FunctionID function) const
{
    static constexpr size_t bytesOfLine = 64;
    auto prefetch = [](const auto& list, size_t first, size_t end) {
        const char* address = reinterpret_cast<const char*>(list.span().data() + first);
        const char* endOfBytes = reinterpret_cast<const char*>(list.span().data() + end);
        for (; address < endOfBytes; address += bytesOfLine)
            __builtin_prefetch(address);
    };
    size_t first = m_nodesOfFunctions[function.index()].firstParameter;
    size_t end = m_nodesOfFunctions[function.index() + 1].firstParameter;
    prefetch(m_nodesOfSummaries, m_nodesOfFunctions[function.index()].firstSummaryNode, m_nodesOfFunctions[function.index() + 1].firstSummaryNode);
    prefetch(m_nodes, first, end);
    prefetch(m_listsOfNodes, first, end);
}

ALWAYS_INLINE_INTO_OTHER_FILES FlowNodeID LinkFlow::findSharedNodeOf(FunctionID function, uint32_t nodeInSummary) const
{
    static constexpr uint32_t staticSideBit = 1u << 31;
    const SummaryNode& node = summaries.at(function).nodes[nodeInSummary];
    switch (node.kind) {
    case SummaryNode::Kind::Unknown:
        return unseen();
    case SummaryNode::Kind::Parameter:
        return parameter(function, node.first);
    case SummaryNode::Kind::Result:
        return result(function, node.first);
    case SummaryNode::Kind::Thrown:
        return thrown(function);
    case SummaryNode::Kind::Capture:
        return capture(node.second ? FunctionID(node.second - 1) : function, node.first);
    case SummaryNode::Kind::Variable:
        return variable(VariableID(node.first));
    case SummaryNode::Kind::Field:
        return field({ LayoutID(node.first), node.second });
    case SummaryNode::Kind::Member:
        return member(LayoutID(node.first), node.second & staticSideBit ? ClassSide::Static : ClassSide::Prototype, node.second & ~staticSideBit);
    case SummaryNode::Kind::Contents:
    case SummaryNode::Kind::Constant:
    case SummaryNode::Kind::Allocation:
    case SummaryNode::Kind::ClassInstance:
    case SummaryNode::Kind::Closure:
    case SummaryNode::Kind::Builtin:
    case SummaryNode::Kind::Join:
    case SummaryNode::Kind::ResultOfCall:
    case SummaryNode::Kind::ResultOfAccess:
    case SummaryNode::Kind::ResultOfOperator:
    case SummaryNode::Kind::ResultOfConversion:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::span<const LinkFlow::MemberOfClass> LinkFlow::membersOf(LayoutID classLayout, ClassSide side) const
{
    size_t sideIndex = 2 * static_cast<size_t>(classLayout.index()) + static_cast<size_t>(side);
    return m_membersOfClasses.span().subspan(m_firstMemberOfSide[sideIndex], m_firstMemberOfSide[sideIndex + 1] - m_firstMemberOfSide[sideIndex]);
}

std::span<const LinkFlow::MemberOfClass> LinkFlow::membersFoundUnder(LayoutID layout, ClassSide side, PropertyKeyID key) const
{
    if (!key || !m_keysOfMembers[static_cast<size_t>(side)].get(key.index()))
        return { };
    bool isHiddenByEveryClass = classHolderIsMadeWithName(program, side, key);
    for (RowOfBasesInLinkFlow row(program, layout); row.current(); row.advance()) {
        auto found = std::ranges::equal_range(membersOf(row.current(), side), key, { }, &MemberOfClass::key);
        if (!found.empty())
            return { found.begin(), found.end() };
        if (isHiddenByEveryClass && !program.at(row.current()).has(LayoutFlags::IsConstructorFunction))
            return { };
    }
    return { };
}

LinkFlow::FoundMembers LinkFlow::findMembers(LayoutID layout, ClassSide side, PropertyKeyID key) const
{
    FoundMembers found;
    std::span<const MemberOfClass> members = membersFoundUnder(layout, side, key);
    for (const MemberOfClass& member : members) {
        if (found.m_members.isEmpty() || found.m_members.last().index != member.index || found.m_members.last().kind != member.kind)
            found.m_members.append(FoundMember { member.classLayout, member.index, member.kind });
    }
    found.m_lookupGoesOnAboveClasses = lookupGoesOnAboveClasses(layout, members);
    return found;
}

LayoutID LinkFlow::classOfConstructor(FunctionID function) const
{
    const Function& record = program.at(function);
    if (!record.constructedClass || !isClassConstructorOf(record.kind) || program.at(record.constructedClass).constructor != function)
        return { };
    return record.constructedClass;
}

bool LinkFlow::hasComputedKeysOfItsOwn(LayoutID layout, ClassSide side) const
{
    return m_sidesWithComputedKeys.get(2 * static_cast<size_t>(layout.index()) + static_cast<size_t>(side));
}

bool LinkFlow::namesMayBeIncomplete(LayoutID layout, ClassSide side) const
{
    if (side == ClassSide::Prototype && m_layoutsWithFieldsUnderComputedKeys.get(layout.index()))
        return true;
    return m_sidesBelowIncompleteNames.quickGet(2 * static_cast<size_t>(layout.index()) + static_cast<size_t>(side));
}

bool LinkFlow::extendsAloneLinksClassObjects(LayoutID below, LayoutID above) const
{
    LayoutID function = m_nearestConstructorFunctionsFrom[below.index()];
    return !function || (above != function && m_nearestConstructorFunctionsFrom[above.index()] == function);
}

void LinkFlow::findSidesBelowIncompleteNames()
{
    size_t numberOfLayouts = program.layouts.size();
    m_sidesBelowIncompleteNames.ensureSize(2 * numberOfLayouts);
    auto findLayoutsBelowIncompleteNames = [&](ClassSide side, bool namesOnConstructorFunctionsAreFollowed) {
        return answerAlongRows(numberOfLayouts, true, [&](LayoutID id) -> StepAlongRow {
            const Layout& current = program.at(id);
            if (m_sidesWithComputedKeys.get(2 * static_cast<size_t>(id.index()) + static_cast<size_t>(side))
                || current.writtenBuiltinBase
                || (current.has(LayoutFlags::IsConstructorFunction) && (!namesOnConstructorFunctionsAreFollowed || !isOfConstructorFunction(id)))
                || (!current.base && current.has(LayoutFlags::HasHeritage)))
                return { true, { } };
            if (!current.base)
                return { false, { } };
            return { std::nullopt, current.base };
        });
    };
    for (ClassSide side : { ClassSide::Prototype, ClassSide::Static }) {
        for (size_t index : findLayoutsBelowIncompleteNames(side, side == ClassSide::Prototype))
            m_sidesBelowIncompleteNames.quickSet(2 * index + static_cast<size_t>(side));
    }
    m_staticSidesBelowIncompleteNamesBesideConstructorFunctions = findLayoutsBelowIncompleteNames(ClassSide::Static, true);
    m_staticSidesBelowIncompleteNamesBesideConstructorFunctions.ensureSize(numberOfLayouts);
}

void LinkFlow::findLayoutsBelowMembersUnderNumbers()
{
    bool someMemberIsUnderNumber = false;
    for (size_t key : m_keysOfMembers[static_cast<size_t>(ClassSide::Prototype)])
        someMemberIsUnderNumber |= mayBeElementKey(m_nameClassesOfKeys[key]);
    if (!someMemberIsUnderNumber)
        return;
    m_layoutsBelowMembersUnderNumbers = answerAlongRows(program.layouts.size(), true, [&](LayoutID id) -> StepAlongRow {
        for (const MemberOfClass& member : membersOf(id, ClassSide::Prototype)) {
            if (member.kind != MethodKind::Setter && mayBeNamedByNumber(member.key))
                return { true, { } };
        }
        LayoutID base = program.at(id).base;
        if (!base)
            return { false, { } };
        return { std::nullopt, base };
    });
}

std::span<const LayoutID> LinkFlow::layoutsOfFamily(FamilyID family) const
{
    return program.types.listOf(families.layoutsOf(family));
}

LinkType LinkFlow::typeOfLayouts(std::span<const LayoutID> sortedMembers) const
{
    if (sortedMembers.empty())
        return { };
    if (sortedMembers.size() == 1)
        return typeOfLayoutInLinkFlow(sortedMembers[0]);
    LinkType type;
    type.objects = LinkType::Objects::Set;
    type.layout = LayoutID(m_setsOfLayouts.numberOf(sortedMembers));
    return type;
}

LinkType LinkFlow::typeOfFunctions(std::span<const FunctionID> sortedMembers) const
{
    if (sortedMembers.empty())
        return { };
    if (sortedMembers.size() == 1)
        return typeOfClosureInLinkFlow(sortedMembers[0]);
    LinkType type;
    type.functions = LinkType::Functions::Set;
    type.function = FunctionID(m_setsOfFunctions.numberOf(sortedMembers));
    return type;
}

void LinkFlow::loseFunctionsOfSet(const LinkType& type) const
{
    std::span<const FunctionID> members = functionsOfSet(type);
    for (FunctionID member : members) {
        deferLoss(member);
        deferUnion(members[0], member);
    }
}

bool LinkFlow::mayStandInOneSet(std::span<const FunctionID> members) const
{
    static constexpr size_t maxNonLeafMembers = 4;
    static constexpr uint32_t smallLeafWeight = 16;
    auto isSmallLeaf = [&](FunctionID member) {
        if (!program.at(member).hasBody())
            return false;
        const ThinSummary& summary = summaries.at(member);
        return summary.weightOfOperations <= smallLeafWeight && !summary.has(SummaryTrait::HasLoops) && std::ranges::all_of(summary.calls, [](const SummaryCall& call) {
            return call.callee == SummaryCall::Callee::Builtin || call.isMadeByProtocol;
        });
    };
    if (members.size() > maxNonLeafMembers && !std::ranges::all_of(members, isSmallLeaf))
        return false;
    for (size_t first = 0; first < members.size(); ++first) {
        LayoutID classOfFirst = classOfConstructor(members[first]);
        for (size_t second = first + 1; classOfFirst && second < members.size(); ++second) {
            LayoutID classOfSecond = classOfConstructor(members[second]);
            if (classOfSecond && families.nearestCommonFamily(classOfFirst, classOfSecond))
                return false;
        }
    }
    return true;
}

bool LinkFlow::namesOnlyFunctionsOfSet(const LinkType& type, const LinkType& set) const
{
    std::span<const FunctionID> members = functionsOfSet(set);
    if (type.functions == LinkType::Functions::Closure)
        return std::ranges::find(members, type.function) != members.end();
    if (type.functions != LinkType::Functions::Set)
        return false;
    return std::ranges::all_of(functionsOfSet(type), [&](FunctionID member) {
        return std::ranges::find(members, member) != members.end();
    });
}

bool LinkFlow::namesOnlyLayoutsOfSet(const LinkType& type, const LinkType& set) const
{
    std::span<const LayoutID> members = layoutsOfSet(set);
    if (type.objects == LinkType::Objects::Layout)
        return std::ranges::find(members, type.layout) != members.end();
    if (type.objects != LinkType::Objects::Set)
        return false;
    return std::ranges::all_of(layoutsOfSet(type), [&](LayoutID member) {
        return std::ranges::find(members, member) != members.end();
    });
}

bool LinkFlow::standsOutsideFamilies(const LinkType& type) const
{
    switch (type.objects) {
    case LinkType::Objects::None:
    case LinkType::Objects::Set:
    case LinkType::Objects::Family:
    case LinkType::Objects::Lost:
        return false;
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
        return true;
    case LinkType::Objects::Layout:
        return program.at(type.layout).isEnvironment();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void LinkFlow::deferLoss(LayoutID layout) const
{
    if (Lane* lane = s_laneAtWork) {
        if (!m_layoutsToldLost.get(layout.index()))
            lane->deferred.append(Deferred { Deferred::Kind::LayoutIsLost, layout.index(), { } });
        return;
    }
    if (m_layoutsToldLost.add(layout.index()))
        m_deferred.append(Deferred { Deferred::Kind::LayoutIsLost, layout.index(), { } });
}

void LinkFlow::deferUnion(LayoutID one, LayoutID other) const
{
    Deferred deferred { Deferred::Kind::LayoutsAreUnited, one.index(), { } };
    deferred.other = other.index();
    if (Lane* lane = s_laneAtWork) {
        if (unitedLayouts.representativeWithoutShortening(one.index()) != unitedLayouts.representativeWithoutShortening(other.index()))
            lane->deferred.append(deferred);
        return;
    }
    if (unitedLayouts.areUnited(one.index(), other.index()))
        return;
    m_deferred.append(deferred);
}

ALWAYS_INLINE_INTO_OTHER_FILES LayoutID LinkFlow::memberStandingFor(const LinkType& type) const
{
    switch (type.objects) {
    case LinkType::Objects::Layout:
    case LinkType::Objects::Lost:
        return type.layout;
    case LinkType::Objects::Set:
        return layoutsOfSet(type)[0];
    case LinkType::Objects::Family:
        return program.types.listOf(families.layoutsOf(type.family))[0];
    case LinkType::Objects::None:
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void LinkFlow::deferLoss(FamilyID family) const
{
    if (Lane* lane = s_laneAtWork) {
        if (!m_familiesToldLost.get(family.index()))
            lane->deferred.append(Deferred { Deferred::Kind::FamilyIsLost, family.index(), { } });
        return;
    }
    if (m_familiesToldLost.add(family.index())) {
        noteSomethingBefell(family);
        m_deferred.append(Deferred { Deferred::Kind::FamilyIsLost, family.index(), { } });
    }
}

void LinkFlow::deferLoss(FunctionID function) const
{
    if (Lane* lane = s_laneAtWork) {
        if (!m_functionsToldLost.get(function.index()))
            lane->deferred.append(Deferred { Deferred::Kind::FunctionIsLost, function.index(), { } });
        return;
    }
    if (m_functionsToldLost.add(function.index()))
        m_deferred.append(Deferred { Deferred::Kind::FunctionIsLost, function.index(), { } });
}

void LinkFlow::deferUnion(FunctionID one, FunctionID other) const
{
    Lane* lane = s_laneAtWork;
    if (!lane && !m_tellsObservers) {
        unite(one, other);
        return;
    }
    if (unitedFunctions.representativeWithoutShortening(one.index()) == unitedFunctions.representativeWithoutShortening(other.index()))
        return;
    Deferred deferred { Deferred::Kind::FunctionsAreUnited, one.index(), { } };
    deferred.other = other.index();
    (lane ? lane->deferred : m_deferred).append(deferred);
}

void LinkFlow::deferUnion(SiteID one, SiteID other) const
{
    Lane* lane = s_laneAtWork;
    if (lane ? m_familiesOfSites.representativeWithoutShortening(one.index()) == m_familiesOfSites.representativeWithoutShortening(other.index()) : m_familiesOfSites.areUnited(one.index(), other.index()))
        return;
    LinkType second;
    second.site = other;
    (lane ? lane->deferred : m_deferred).append(Deferred { Deferred::Kind::SitesAreUnited, one.index(), second });
}

void LinkFlow::deferLossOfConstructors(FamilyID family) const
{
    if (Lane* lane = s_laneAtWork) {
        if (!m_constructorsToldLost.get(family.index()))
            lane->deferred.append(Deferred { Deferred::Kind::ConstructorsAreLost, family.index(), { } });
        return;
    }
    if (m_constructorsToldLost.add(family.index()))
        m_deferred.append(Deferred { Deferred::Kind::ConstructorsAreLost, family.index(), { } });
}

bool LinkFlow::deferEscape(LayoutID layout) const
{
    if (Lane* lane = s_laneAtWork) {
        if (m_layoutsToldToEscape.get(layout.index()))
            return false;
        lane->deferred.append(Deferred { Deferred::Kind::LayoutEscapes, layout.index(), { } });
        return true;
    }
    if (!m_layoutsToldToEscape.add(layout.index()))
        return false;
    m_deferred.append(Deferred { Deferred::Kind::LayoutEscapes, layout.index(), { } });
    return true;
}

bool LinkFlow::deferEscape(FunctionID function) const
{
    if (Lane* lane = s_laneAtWork) {
        if (m_functionsToldToEscape.get(function.index()))
            return false;
        lane->deferred.append(Deferred { Deferred::Kind::FunctionEscapes, function.index(), { } });
        return true;
    }
    if (!m_functionsToldToEscape.add(function.index()))
        return false;
    m_deferred.append(Deferred { Deferred::Kind::FunctionEscapes, function.index(), { } });
    return true;
}

void LinkFlow::deferEscape(SiteID site) const
{
    if (Lane* lane = s_laneAtWork) {
        if (!m_sites[m_familiesOfSites.representativeWithoutShortening(site.index())].isToldToEscape)
            lane->deferred.append(Deferred { Deferred::Kind::SiteEscapes, site.index(), { } });
        return;
    }
    if (!std::exchange(m_sites[m_familiesOfSites.representativeOf(site.index())].isToldToEscape, true))
        m_deferred.append(Deferred { Deferred::Kind::SiteEscapes, site.index(), { } });
}

void LinkFlow::joinObjectsSlowCase(LinkType& into, const LinkType& other) const
{
    using Objects = LinkType::Objects;
    if (into.objects == Objects::Lost && other.objects == Objects::Lost) {
        if (!into.layout)
            into.layout = other.layout;
        else if (other.layout)
            deferUnion(into.layout, other.layout);
        return;
    }
    joinObjectsOfOtherKinds(into, other);
}

NEVER_INLINE void LinkFlow::joinObjectsOfOtherKinds(LinkType& into, const LinkType& other) const
{
    using Objects = LinkType::Objects;
    auto take = [&](const LinkType& from) {
        into.objects = from.objects;
        into.layout = from.layout;
        into.family = from.family;
    };

    auto nearestCommonFamily = [&](bool intoNamesLayout, bool otherNamesLayout) {
        if (intoNamesLayout && otherNamesLayout)
            return families.nearestCommonFamily(into.layout, other.layout);
        if (intoNamesLayout)
            return families.nearestCommonFamily(other.family, into.layout);
        if (otherNamesLayout)
            return families.nearestCommonFamily(into.family, other.layout);
        return families.nearestCommonFamily(into.family, other.family);
    };
    auto holdsPrototypes = [](const LinkType& type) {
        return type.objects == Objects::Prototype || type.objects == Objects::Prototypes;
    };

    ASSERT(into.objects != Objects::None);
    bool otherStandsOutside = standsOutsideFamilies(other);
    if (into.objects == Objects::Lost && !into.layout && !otherStandsOutside) {
        take(other);
        return;
    }
    if (other.objects == Objects::Lost && !other.layout && !standsOutsideFamilies(into))
        return;

    bool intoStandsOutside = standsOutsideFamilies(into);
    if (intoStandsOutside || otherStandsOutside) {
        if (holdsPrototypes(into) && holdsPrototypes(other)) {
            if (FamilyID common = nearestCommonFamily(into.objects == Objects::Prototype, other.objects == Objects::Prototype)) {
                take(typeOfPrototypesInLinkFlow(common));
                return;
            }
        }

        auto giveUp = [&](Objects objects, LayoutID layout, FamilyID family) {
            into.hasUnknownObjects = true;
            switch (objects) {
            case Objects::Prototype:
                deferEscape(layout);
                noteReachesUnseen(typeOfPrototypeInLinkFlow(layout));
                return;
            case Objects::Prototypes:
                noteReachesUnseen(typeOfPrototypesInLinkFlow(family));
                return;
            case Objects::Layout:
                deferEscape(layout);
                into.primitives |= LinkType::closed(Primitive::All);
                into.counts = LinkType::Counts::Unknown;
                return;
            case Objects::None:
            case Objects::Set:
            case Objects::Family:
            case Objects::Lost:
                break;
            }
            RELEASE_ASSERT_NOT_REACHED();
        };
        if (intoStandsOutside) {
            giveUp(into.objects, into.layout, into.family);
            take(LinkType { });
        }
        if (otherStandsOutside)
            giveUp(other.objects, other.layout, other.family);
        else if (intoStandsOutside)
            take(other);
        if (into.objects == Objects::None)
            into.objects = Objects::Lost;
        return;
    }

    foldObjects(into, std::span<const LinkType>(&other, 1));
}

ALWAYS_INLINE_INTO_OTHER_FILES void LinkFlow::joinFunctions(LinkType& into, const LinkType& other) const
{
    using Functions = LinkType::Functions;
    if (other.functions == Functions::None)
        return;
    if (into.functions == Functions::None) {
        into.functions = other.functions;
        into.function = other.function;
        into.constructorsOf = other.constructorsOf;
        return;
    }
    if (into.functions == other.functions && into.function == other.function && into.constructorsOf == other.constructorsOf)
        return;
    if (into.functions == Functions::Lost && (other.functions == Functions::Lost || other.functions == Functions::Closure)) {
        if (other.functions == Functions::Closure)
            deferLoss(other.function);
        if (!other.function)
            return;
        if (into.function)
            deferUnion(into.function, other.function);
        else
            into.function = other.function;
        return;
    }
    foldFunctions(into, std::span<const LinkType>(&other, 1));
}

void LinkFlow::joinClassesSlowCase(LinkType& into, const LinkType& other) const
{
    if (into.classes == LinkType::Classes::None) {
        into.classes = other.classes;
        into.builtinClass = other.builtinClass;
        into.setOfClasses = other.setOfClasses;
        into.site = other.site;
        return;
    }
    if (into.site == other.site && into.classes == other.classes && into.builtinClass == other.builtinClass && into.setOfClasses == other.setOfClasses)
        return;
    joinClassesThatDifferOrLieAtOtherSites(into, other);
}

NEVER_INLINE void LinkFlow::joinClassesThatDifferOrLieAtOtherSites(LinkType& into, const LinkType& other) const
{
    using Classes = LinkType::Classes;
    if (into.site != other.site)
        deferUnion(into.site, other.site);
    if (into.classes == other.classes && into.builtinClass == other.builtinClass && into.setOfClasses == other.setOfClasses)
        return;
    uint16_t set = 0;
    if (namesClassesOf(into) && namesClassesOf(other)) {
        auto membersOf = [&](const LinkType& type) {
            return type.classes == Classes::Site ? std::span<const BuiltinClassID>(&type.builtinClass, 1) : m_setsOfClasses.membersOf(type.setOfClasses);
        };
        std::span<const BuiltinClassID> held = membersOf(into);
        std::span<const BuiltinClassID> arriving = membersOf(other);
        if (std::ranges::includes(held, arriving))
            return;
        if (std::ranges::includes(arriving, held))
            set = other.setOfClasses;
        else {
            Vector<BuiltinClassID, 8> members(held.size() + arriving.size());
            members.shrink(std::ranges::set_union(held, arriving, members.begin()).out - members.begin());
            set = m_setsOfClasses.numberOf(members.span());
        }
    }
    into.classes = Classes::Mixed;
    into.builtinClass = { };
    into.setOfClasses = set;
}

static ALWAYS_INLINE uint64_t wordOfWhatIsByEscapeAloneInLinkFlow()
{
    LinkType marked;
    marked.isUnknownEscapeOnly = true;
    marked.escapeOnlyPrimitives = UINT16_MAX;
    return marked.words()[0] ^ LinkType { }.words()[0];
}

static ALWAYS_INLINE void joinShadowsInLinkFlow(LinkType& result, const LinkType& first, const LinkType& second)
{
    result.escapeOnlyPrimitives = result.primitives & ~withoutLiteralsInLinkFlow(first.directPrimitives() | second.directPrimitives());
    result.isUnknownEscapeOnly = result.hasUnknownObjects && !first.hasDirectUnknownObjects() && !second.hasDirectUnknownObjects() && (first.hasUnknownObjects || second.hasUnknownObjects);
    result.marks |= second.marks;
    result.counts = static_cast<LinkType::Counts>(static_cast<uint8_t>(result.counts) | static_cast<uint8_t>(second.counts));
}

static ALWAYS_INLINE LinkType joinedWithUninformativeType(const LinkType& first, const LinkType& second)
{
    LinkType result = first;
    if (first == second) {
        joinShadowsInLinkFlow(result, first, second);
        return result;
    }
    result.primitives = withoutLiteralsThatAreRareInLinkFlow(first.primitives | second.primitives);
    result.hasUnknownObjects |= second.hasUnknownObjects;
    result.holdsLostBuiltins |= second.holdsLostBuiltins;
    if ((first.head() | second.head()) & wordOfWhatIsByEscapeAloneInLinkFlow())
        joinShadowsInLinkFlow(result, first, second);
    else {
        result.marks |= second.marks;
        result.counts = static_cast<LinkType::Counts>(static_cast<uint8_t>(result.counts) | static_cast<uint8_t>(second.counts));
    }
    return result;
}

LinkType LinkFlow::joined(const LinkType& first, const LinkType& second, LossAtJoin loss) const
{
    if (first == second) {
        LinkType same = first;
        joinShadowsInLinkFlow(same, first, second);
        return same;
    }

    LinkType result = first;
    uint16_t primitivesOfBoth = withoutLiteralsThatAreRareInLinkFlow(first.primitives | second.primitives);
    result.primitives = primitivesOfBoth;
    result.hasUnknownObjects |= second.hasUnknownObjects;
    result.holdsLostBuiltins |= second.holdsLostBuiltins;
    if (second.objects != LinkType::Objects::None)
        joinObjects(result, second);
    if (second.functions != LinkType::Functions::None)
        joinFunctions(result, second);
    joinClasses(result, second);
    if (second.builtin) {
        if (!result.builtin)
            result.builtin = second.builtin;
        else if (result.builtin != second.builtin)
            result.hasUnknownObjects = true;
    }
    if ((first.words()[0] | second.words()[0]) & wordOfWhatIsByEscapeAloneInLinkFlow() || result.primitives != primitivesOfBoth)
        joinShadowsInLinkFlow(result, first, second);
    else {
        result.marks |= second.marks;
        result.counts = static_cast<LinkType::Counts>(static_cast<uint8_t>(result.counts) | static_cast<uint8_t>(second.counts));
    }
    if (result.hasUnknownObjects && result.builtin) {
        bool waits = loss == LossAtJoin::WaitsForUse && !(m_marksThatAreUsed & LinkType::BuiltinsLostAtJoins);
        for (BuiltinID lost : { first.builtin, second.builtin }) {
            if (!waits || (lost && lightFormOf(lost)))
                noteLossOf(lost);
            else if (lost)
                (s_laneAtWork ? s_laneAtWork->builtinsLostAtJoins : m_builtinsLostAtJoins).append(lost);
            result.holdsLostBuiltins |= loseInSight(lost);
        }
        if (waits)
            result.marks |= LinkType::BuiltinsLostAtJoins;
        result.builtin = { };
        result.isUnknownEscapeOnly = false;
    }
    if (result.hasUnknownObjects)
        result.setOfClasses = 0;
    return result;
}

bool LinkFlow::loseInSight(BuiltinID canonical) const
{
    if (!canonical || (s_laneAtWork ? s_laneAtWork->isInLibraryCode : m_isInLibraryCode) || !isWorthFollowingToWrites(canonical))
        return false;
    builtinsLostInSightByThread().append(canonical);
    return true;
}

Vector<BuiltinID>& LinkFlow::builtinsLostInSightByThread() const
{
    return s_laneAtWork ? s_laneAtWork->builtinsLostInSight : m_builtinsLostInSight;
}

bool LinkFlow::isInLibraryCode() const
{
    return s_laneAtWork ? s_laneAtWork->isInLibraryCode : m_isInLibraryCode;
}

bool LinkFlow::setIsInLibraryCode(bool isOfLibrary)
{
    return std::exchange(s_laneAtWork ? s_laneAtWork->isInLibraryCode : m_isInLibraryCode, isOfLibrary);
}

LinkType LinkFlow::anythingBeside(BuiltinID canonical, FunctionID reader) const
{
    LinkType type = LinkType::anything();
    noteLossOf(canonical);
    type.holdsLostBuiltins = !isLibraryCode(program, program.at(reader)) && loseInSight(canonical);
    return type;
}

LinkType LinkFlow::unseenObjectsBelow(BuiltinID canonicalPrototype, FunctionID maker) const
{
    LinkType type;
    type.hasUnknownObjects = true;
    type.holdsLostBuiltins = !isLibraryCode(program, program.at(maker)) && loseInSight(canonicalPrototype);
    return type;
}

void LinkFlow::noteWhatIsStoredOn(const LinkType& subject, WhatIsStored stored)
{
    if (stored != WhatIsStored::Anything)
        return;
    if (subject.builtin)
        note(Note::BuiltinHandedWhatIsNoTextNumberOrBoolean, subject.builtin.index());
    if (subject.holdsLostBuiltins || subject.hasUnknownObjects)
        note(Note::BuiltinHandedWhatIsNoTextNumberOrBoolean, BuiltinID().index());
}

void LinkFlow::findWhatNativeCodeHandsToLibrary()
{
    const BuiltinTable& table = program.builtins();
    BitVector isHanded(table.builtinCount());
    auto lookAt = [&](const BuiltinHostUses& uses, const auto& lookAtResult) -> void {
        for (const BuiltinHostUse& use : uses.list) {
            if (use.use != BuiltinHostReadUse::Called || !use.call)
                continue;
            for (const BuiltinPlace& argument : use.call->arguments) {
                if (argument.kind != BuiltinPlaceKind::NewValue || !argument.newValueType)
                    continue;
                for (const BuiltinTypePart& part : table.at(argument.newValueType).parts) {
                    if (part.kind == BuiltinTypePartKind::Builtin)
                        isHanded.quickSet(table.canonicalID(part.builtin).index());
                }
            }
            lookAtResult(use.call->result, lookAtResult);
        }
    };
    for (unsigned index = 0; index < table.builtinCount(); ++index) {
        for (const BuiltinCalledFunction& called : table.calledFunctionsOf(BuiltinID(index)))
            lookAt(called.uses, lookAt);
    }
    for (const BuiltinCalledFunction& called : table.startupCalledFunctions())
        lookAt(called.uses, lookAt);
    for (size_t index : isHanded) {
        BuiltinID handed(static_cast<uint32_t>(index));
        uint32_t stepsLeft = table.builtinCount();
        for (BuiltinID holder = handed; holder && stepsLeft--; holder = table.at(holder).prototypeLink == BuiltinPrototypeLink::Builtin ? table.canonicalID(table.at(holder).inheritsFrom) : BuiltinID())
            m_holdersAtOrAboveWhatNativeCodeHandsToLibrary.append({ holder, handed });
    }
}

void LinkFlow::noteWhatNativeCodeHandsToWhatIsLaidOn(BuiltinID written, PropertyKeyID key) const
{
    const BuiltinTable& table = program.builtins();
    BuiltinID canonical = table.canonicalID(written);
    auto handOutWhatLibraryIsHanded = [&] {
        for (auto [holder, handed] : m_holdersAtOrAboveWhatNativeCodeHandsToLibrary) {
            if (holder == canonical)
                noteLossOf(handed);
        }
    };
    auto handOut = [&](const BuiltinPlace& place) {
        if (place.kind == BuiltinPlaceKind::Operand && place.operand && place.operand->isReceiver())
            noteLossOf(canonical);
        if (place.kind != BuiltinPlaceKind::NewValue || !place.newValueType)
            return;
        for (const BuiltinTypePart& part : table.at(place.newValueType).parts) {
            if (part.kind == BuiltinTypePartKind::Builtin)
                noteLossOf(table.canonicalID(part.builtin));
        }
    };
    auto lookAt = [&](BuiltinID member) {
        for (const BuiltinOverload& row : table.overloadsOf(table.canonicalID(member))) {
            if (row.mode != BuiltinCallMode::Set)
                continue;
            for (const BuiltinCallback& callback : row.flow.callbacks) {
                handOut(callback.thisValue);
                handOut(callback.spreadArguments);
                std::ranges::for_each(callback.arguments, handOut);
            }
        }
    };
    if (key) {
        BuiltinName name = program.builtinNameOf(key);
        BuiltinMemberLookup found = name ? table.findMember(canonical, name) : BuiltinMemberLookup();
        bool isTakenBySetter = found.outcome == BuiltinMemberLookup::Outcome::Found && std::ranges::contains(table.overloadsOf(table.canonicalID(found.member)), BuiltinCallMode::Set, &BuiltinOverload::mode);
        if (found.outcome == BuiltinMemberLookup::Outcome::Found)
            lookAt(found.member);
        if (!isTakenBySetter)
            handOutWhatLibraryIsHanded();
        return;
    }
    handOutWhatLibraryIsHanded();
    uint32_t stepsLeft = table.builtinCount();
    for (BuiltinID holder = canonical; holder && stepsLeft--; holder = table.at(holder).prototypeLink == BuiltinPrototypeLink::Builtin ? table.canonicalID(table.at(holder).inheritsFrom) : BuiltinID())
        std::ranges::for_each(table.membersOf(holder), lookAt);
}

void LinkFlow::noteWriteOn(const LinkType& subject, PropertyKeyID key, FunctionID writer, WhatIsStored stored, bool subjectIsMadeInBody)
{
    if (subject.builtin && stored == WhatIsStored::Anything)
        noteWhatNativeCodeHandsToWhatIsLaidOn(subject.builtin, key);
    if (key && (subject.builtin || subject.holdsLostBuiltins) && m_testRuns->isTestRun()) {
        LockerOfWhatIsShared locker(*this);
        m_testRuns->noteWriteOn(subject, key, writer);
    }
    if (subject.builtin && !m_testRuns->takesWriteOnBuiltin(subject.builtin, key, writer))
        note(Note::WriteOnBuiltin, key.bits(), subject.builtin.index() | (stored == WhatIsStored::Nothing ? onlyDeletes : 0));
    if (subject.builtin)
        arrive(laidOnBuiltin(program.builtins().canonicalID(subject.builtin), key), LinkType::anything());
    noteWhatIsStoredOn(subject, stored);
    if (stored != WhatIsStored::Nothing)
        noteWhatMayMakeThenableOf(subject, writer, key, stored == WhatIsStored::Anything, subjectIsMadeInBody);
    if (subject.holdsLostBuiltins)
        note(Note::WriteThroughLostBuiltins, key.bits(), isFileOfProductInTestRun(program, writer));
    if (subject.hasUnknownObjects && !m_isFunctionOfLibrary.quickGet(writer.index()))
        note(Note::WriteThroughUnseenByProgram, key.bits(), isFileOfProductInTestRun(program, writer));
    else if (subject.hasUnknownObjects && key)
        note(Note::NameWrittenThroughUnseenByLibrary, key.bits());
}

BuiltinID LinkFlow::prototypeOfInstancesOf(BuiltinClassID builtinClass) const
{
    const BuiltinTable& table = program.builtins();
    for (BuiltinClassID current = builtinClass; current; current = table.at(current).parent) {
        if (BuiltinID prototype = table.at(current).prototype)
            return table.canonicalID(prototype);
    }
    return { };
}

void LinkFlow::noteSiteOfWrite(FunctionID writer, ValueID site, FlowNodeID subject)
{
    if (site && typeAt(subject).builtin && !isLibraryCode(program, program.at(writer))) {
        LockerOfWhatIsShared lockerOfWhatIsShared(*this);
        m_sitesOfWritesOnBuiltins.of(*this).append(SiteOfWrite { writer, site, subject });
    }
}

void LinkFlow::noteSiteOfCopy(FunctionID writer, ValueID site, FlowNodeID subject, FlowNodeID source)
{
    if (site && typeAt(subject).builtin && !isLibraryCode(program, program.at(writer))) {
        LockerOfWhatIsShared lockerOfWhatIsShared(*this);
        m_sitesOfWritesOnBuiltins.of(*this).append(SiteOfWrite { writer, site, subject, source });
    }
}

bool LinkFlow::appendNamesOfObjectsAmong(const LinkType& listed, Vector<PropertyKeyID>& keys) const
{
    if (listed.builtin || listed.functions != LinkType::Functions::None || listed.classes != LinkType::Classes::None || listed.primitives & LinkType::closed(Primitive::AnyString))
        return false;
    if (listed.hasUnknownObjects && listed.objects == LinkType::Objects::None)
        return false;
    bool knowsAll = true;
    auto appendNamesOf = [&](LayoutID id) {
        uint32_t stepsLeft = static_cast<uint32_t>(program.layouts.size());
        for (LayoutID current = id; current && stepsLeft--; current = program.at(current).base) {
            const Layout& layout = program.at(current);
            knowsAll &= !layout.computedKeyCount && !namesMayBeIncomplete(current, ClassSide::Prototype);
            for (const Field& field : layout.fields) {
                if (!field.key || field.computedKey)
                    knowsAll = false;
                else if (!isPrivateNameInLink(program, field.key))
                    keys.append(field.key);
            }
        }
    };
    switch (listed.objects) {
    case LinkType::Objects::None:
        break;
    case LinkType::Objects::Layout:
        appendNamesOf(listed.layout);
        break;
    case LinkType::Objects::Set:
        for (LayoutID member : layoutsOfSet(listed))
            appendNamesOf(member);
        break;
    case LinkType::Objects::Family:
        program.at(families.layoutsOf(listed.family)).forEach([&](LayoutID member) {
            appendNamesOf(member);
        });
        break;
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
    case LinkType::Objects::Lost:
        return false;
    }
    return knowsAll;
}

Vector<WriteFollowedToBuiltin> LinkFlow::takeWritesFollowedToBuiltins()
{
    Vector<SiteOfWrite> sites = std::exchange(m_sitesOfWritesOnBuiltins.of(*this), { });
    auto order = [](const SiteOfWrite& write) {
        return std::tuple(write.writer.index(), write.site.index(), write.subject.index(), !write.source, write.source ? write.source.index() : 0);
    };
    std::ranges::sort(sites, { }, order);
    sites.shrink(sites.size() - std::ranges::unique(sites, { }, order).size());

    Vector<WriteFollowedToBuiltin> writes;
    for (const SiteOfWrite& write : sites) {
        const LinkType& subject = typeAt(write.subject);
        bool namesOnlyBuiltin = subject.builtin && !subject.hasUnknownObjects && subject.objects == LinkType::Objects::None && subject.functions == LinkType::Functions::None && subject.classes == LinkType::Classes::None;
        if (!namesOnlyBuiltin)
            continue;
        bool isSiteOfLast = !writes.isEmpty() && writes.last().function == write.writer && writes.last().site == write.site && writes.last().holder == subject.builtin;
        if (!isSiteOfLast) {
            writes.append(WriteFollowedToBuiltin { write.writer, write.site, subject.builtin });
            writes.last().knowsNamesBrought = !!write.source;
        }
        if (write.source)
            writes.last().knowsNamesBrought &= appendNamesOfObjectsAmong(typeAt(write.source), writes.last().namesBrought);
    }
    return writes;
}

LinkType LinkFlow::builtinsAbove(const LinkType& below, FunctionID asker)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    const BuiltinTable& table = program.builtins();
    LinkType above;
    auto add = [&](BuiltinID builtin) {
        if (!builtin)
            return;
        LinkType type;
        type.builtin = table.canonicalID(builtin);
        above = joinOf(above, type);
    };
    if (below.hasUnknownObjects)
        builtinsInReach().noteClimbFromUnseenObjects();
    if (isLibraryCode(program, program.at(asker)))
        return above;
    if (below.holdsLostBuiltins) {
        builtinsInReach().noteClimbFromLostBuiltins();
        above.hasUnknownObjects = true;
        above.holdsLostBuiltins = true;
    }
    if (below.builtin && table.at(below.builtin).prototypeLink == BuiltinPrototypeLink::Builtin)
        add(table.at(below.builtin).inheritsFrom);
    if (below.functions == LinkType::Functions::Closure)
        add(m_testRuns->prototypeAboveClosuresOf(below.function));
    auto writtenRightAbove = [&](LayoutID layout) {
        return layout && !program.at(layout).base ? program.at(layout).writtenBuiltinBase : BuiltinClassID();
    };
    if (BuiltinClassID written = below.functions == LinkType::Functions::Closure ? writtenRightAbove(classOfConstructor(below.function)) : BuiltinClassID())
        add(table.at(written).constructor);
    if (BuiltinClassID written = below.objects == LinkType::Objects::Prototype ? writtenRightAbove(below.layout) : BuiltinClassID())
        add(table.at(written).prototype);
    switch (below.classes) {
    case LinkType::Classes::None:
        break;
    case LinkType::Classes::Site:
        add(prototypeOfInstancesOf(below.builtinClass));
        break;
    case LinkType::Classes::Mixed:
        m_sites[m_familiesOfSites.representativeOf(below.site.index())].isClimbedFrom = true;
        above.hasUnknownObjects = true;
        above.holdsLostBuiltins = true;
        break;
    }
    return above;
}

LinkType LinkFlow::joinOf(const LinkType& first, const LinkType& second, LossAtJoin loss)
{
    LinkType result = joined(first, second, loss);
    settle();
    return result;
}

void LinkFlow::noteMetInFamily(LayoutID layout) const
{
    if (Lane* lane = s_laneAtWork) {
        if (!layoutsMetInFamilies.quickGet(layout.index()))
            lane->layoutsMetInFamilies.append(layout);
        return;
    }
    if (!layoutsMetInFamilies.quickSet(layout.index()))
        catchUpWithFamiliesAbove(layout);
}

void LinkFlow::catchUpWithFamiliesAbove(LayoutID layout) const
{
    FamilyID nearest = families.nearestFamilyOf(layout);
    if (!nearest || !m_outermostFamiliesWhereSomethingBefell.quickGet(m_outermostFamilies[nearest.index()].index()))
        return;
    families.forEachFamilyOf(layout, [&](FamilyID family) {
        if (m_familiesToldLost.get(family.index()))
            deferUnion(program.types.listOf(families.layoutsOf(family))[0], layout);
        if (hasEscapedAsFamily(family))
            deferEscape(layout);
    });
}

void LinkFlow::handToAccessorsLaidAbove(FlowNodeID receiver)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (!m_receiversThatWaitForAccessorsLaidAbove.contains(receiver))
        m_receiversThatWaitForAccessorsLaidAbove.append(receiver);
}

bool LinkFlow::letGoWhoWaitsForAccessorsLaidAbove()
{
    if (!m_operandIsLaidAbovePrototype || m_receiversThatWaitForAccessorsLaidAbove.isEmpty())
        return false;
    for (FlowNodeID receiver : std::exchange(m_receiversThatWaitForAccessorsLaidAbove, { }))
        addEdge(receiver, unseen());
    return true;
}

void LinkFlow::layAbovePrototype(FlowNodeID operand)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (!m_operandsLaidAbovePrototype.contains(operand))
        m_operandsLaidAbovePrototype.append(operand);
}

void LinkFlow::noteWhatIsLaidAbovePrototype()
{
    for (FlowNodeID operand : m_operandsLaidAbovePrototype)
        notePrototypesLaidAboveOtherObjects(*this, typeAt(operand));
}

bool LinkFlow::everythingLaidAbovePrototypeMayWait(std::span<const BuiltinID> holdersLinkedAnew) const
{
    return std::ranges::all_of(m_operandsLaidAbovePrototype, [&](FlowNodeID operand) {
        LinkType laid = typeAt(operand);
        laid.primitives = 0;
        if (laid.isNever())
            return true;
        if (laid != typeOfPrototypeInLinkFlow(laid.layout) || !isOfConstructorFunction(laid.layout))
            return false;
        for (uint32_t record = firstNameOnPrototypeOf(laid.layout); record != UINT32_MAX; record = m_namesOnPrototypes[record].nextOfLayout) {
            const NameOnPrototype& name = m_namesOnPrototypes[record];
            if (!name.isStoredInto && !name.mayHoldAccessor())
                continue;
            if (name.key && program.at(name.key).kind == PropertyKeyKind::CreatedSymbol)
                continue;
            bool isAnsweredBelow = name.key && std::ranges::all_of(holdersLinkedAnew, [&](BuiltinID holder) {
                return !!findOwnMember(program, holder, name.key);
            });
            if (!isAnsweredBelow && (name.mayHoldAccessor() || !typeAt(name.stored).holdsPrimitivesAlone()))
                return false;
        }
        return true;
    });
}

void LinkFlow::letGoWhatIsLaidAbovePrototype()
{
    for (FlowNodeID operand : std::exchange(m_operandsLaidAbovePrototype, { }))
        addEdge(operand, unseen());
}

bool LinkFlow::isSpelledInBody(FunctionID function, uint32_t summaryNode) const
{
    const ThinSummary& summary = summaries.at(function);
    while (true) {
        const SummaryNode& node = summary.nodes[summaryNode];
        if (node.kind == SummaryNode::Kind::Builtin)
            return true;
        if (node.kind != SummaryNode::Kind::ResultOfAccess)
            return false;
        const SummaryAccess& access = summary.accesses[node.first];
        if (access.kind != SummaryAccess::Kind::Read || !access.key || access.isToContents())
            return false;
        summaryNode = access.subject;
    }
}

void LinkFlow::noteLossOfBuiltin(BuiltinID builtin) const
{
    if (Lane* lane = s_laneAtWork)
        lane->builtinsLost.append(builtin);
    else
        m_builtinsLost.append(builtin);
}

void LinkFlow::noteLossOfObjectOfBuiltinModule(ModuleID id) const
{
    const Module& module = program.at(id);
    if (module.kind != ModuleKind::Runtime || !module.runtimeModule)
        return;
    const BuiltinTable& builtins = program.builtins();
    noteLossOf(builtins.canonicalID(builtins.namespaceOf(module.runtimeModule)));
}

void LinkFlow::noteLossOfExportOfBuiltinModule(ModuleID id, StringID name) const
{
    const Module& module = program.at(id);
    if (module.kind != ModuleKind::Runtime || !module.runtimeModule)
        return;
    const BuiltinTable& builtins = program.builtins();
    BuiltinID object = builtins.canonicalID(builtins.namespaceOf(module.runtimeModule));
    std::optional<PropertyKeyID> key = program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = name });
    BuiltinID member = object && key ? findOwnMember(program, object, *key) : BuiltinID();
    noteLossOf(member ? builtins.canonicalID(member) : object);
}

PropertyKeyID LinkFlow::symbolNotedAt(FlowNodeID node) const
{
    Locker locker { m_lockOfSymbolsAtNodes };
    return PropertyKeyID::fromBits(m_symbolsAtNodes.get(node.index()));
}

void LinkFlow::noteSymbolThatLosesItsName(PropertyKeyID symbol)
{
    if (!symbol || !isSymbolOfLibrary(program, symbol))
        return;
    Locker locker { m_lockOfSymbolsAtNodes };
    if (m_symbolsThatLostTheirNames.add(symbol.bits()).isNewEntry)
        m_symbolsNotedWithoutTheirNames.append(symbol);
}

ALWAYS_INLINE_INTO_OTHER_FILES bool LinkFlow::wouldNameSomethingNewAtUnseen(const LinkType& type) const
{
    if (type.builtin || (type.holdsLostBuiltins && !m_lostBuiltinsReachUnseen) || type.marks & ~m_marksThatReachUnseen)
        return true;
    if (!m_objectsReachUnseen && (type.objects != LinkType::Objects::None || type.functions != LinkType::Functions::None || type.classes != LinkType::Classes::None))
        return true;
    switch (type.objects) {
    case LinkType::Objects::None:
        break;
    case LinkType::Objects::Layout:
        if (!m_layoutsAtUnseen.get(type.layout.index()))
            return true;
        break;
    case LinkType::Objects::Set:
        for (LayoutID member : layoutsOfSet(type)) {
            if (!m_layoutsAtUnseen.get(member.index()))
                return true;
        }
        break;
    case LinkType::Objects::Prototype:
        if (!m_prototypesAtUnseen.get(type.layout.index()))
            return true;
        break;
    case LinkType::Objects::Prototypes:
        if (!m_prototypesOfFamiliesAtUnseen.get(type.family.index()))
            return true;
        break;
    case LinkType::Objects::Family:
        if (!m_familiesAtUnseen.get(type.family.index()))
            return true;
        break;
    case LinkType::Objects::Lost:
        if (type.layout && !m_lostFamilies[unitedLayouts.representativeWithoutShortening(type.layout.index())].reachesUnseen)
            return true;
        break;
    }
    switch (type.functions) {
    case LinkType::Functions::None:
        break;
    case LinkType::Functions::Closure:
        if (!m_functionsAtUnseen.get(type.function.index()))
            return true;
        break;
    case LinkType::Functions::Constructors:
        if (!m_constructorsAtUnseen.get(type.constructorsOf.index()))
            return true;
        break;
    case LinkType::Functions::Set:
        for (FunctionID member : functionsOfSet(type)) {
            if (!m_functionsAtUnseen.get(member.index()))
                return true;
        }
        break;
    case LinkType::Functions::Lost:
        if (type.function && !m_familiesOfFunctionsAtUnseen.get(unitedFunctions.representativeWithoutShortening(type.function.index())))
            return true;
        break;
    }
    return type.classes != LinkType::Classes::None && !m_sites[m_familiesOfSites.representativeWithoutShortening(type.site.index())].isToldToEscape;
}

void LinkFlow::noteReachesUnseen(const LinkType& type) const
{
    if (Lane* lane = s_laneAtWork) {
        if (wouldNameSomethingNewAtUnseen(type) && (lane->typesAtUnseen.isEmpty() || lane->typesAtUnseen.last() != type || lane->typesAtUnseen.last().marks != type.marks))
            lane->typesAtUnseen.append(type);
        return;
    }
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    m_objectsReachUnseen |= type.objects != LinkType::Objects::None || type.functions != LinkType::Functions::None || type.classes != LinkType::Classes::None;
    noteLossOf(type.builtin);
    if (type.builtin)
        m_builtinsAtUnseen.append(type.builtin);
    m_lostBuiltinsReachUnseen |= type.holdsLostBuiltins;
    m_marksThatReachUnseen |= type.marks;
    bool namesSomethingNew = false;
    switch (type.objects) {
    case LinkType::Objects::None:
        break;
    case LinkType::Objects::Layout:
        namesSomethingNew |= m_layoutsAtUnseen.add(type.layout.index());
        break;
    case LinkType::Objects::Set:
        for (LayoutID member : layoutsOfSet(type))
            namesSomethingNew |= m_layoutsAtUnseen.add(member.index());
        break;
    case LinkType::Objects::Prototype:
        namesSomethingNew |= m_prototypesAtUnseen.add(type.layout.index());
        break;
    case LinkType::Objects::Prototypes:
        if (m_prototypesOfFamiliesAtUnseen.add(type.family.index()))
            m_deferred.append(Deferred { Deferred::Kind::PrototypesEscape, type.family.index(), { } });
        break;
    case LinkType::Objects::Family:
        namesSomethingNew |= m_familiesAtUnseen.add(type.family.index());
        break;
    case LinkType::Objects::Lost:
        if (type.layout && !m_lostFamilies[unitedLayouts.representativeOf(type.layout.index())].reachesUnseen)
            m_deferred.append(Deferred { Deferred::Kind::LostFamilyEscapes, type.layout.index(), { } });
        break;
    }
    switch (type.functions) {
    case LinkType::Functions::None:
        break;
    case LinkType::Functions::Closure:
        namesSomethingNew |= m_functionsAtUnseen.add(type.function.index());
        break;
    case LinkType::Functions::Constructors:
        if (m_constructorsAtUnseen.add(type.constructorsOf.index()))
            m_deferred.append(Deferred { Deferred::Kind::ConstructorsEscape, type.constructorsOf.index(), { } });
        break;
    case LinkType::Functions::Set:
        for (FunctionID member : functionsOfSet(type))
            namesSomethingNew |= m_functionsAtUnseen.add(member.index());
        break;
    case LinkType::Functions::Lost:
        if (type.function && !m_familiesOfFunctionsAtUnseen.get(unitedFunctions.representativeOf(type.function.index()))) {
            noteFamilyOfFunctionsAtUnseen(type.function);
            namesSomethingNew = true;
        }
        break;
    }
    switch (type.classes) {
    case LinkType::Classes::None:
        break;
    case LinkType::Classes::Site:
    case LinkType::Classes::Mixed:
        if (!std::exchange(m_sites[m_familiesOfSites.representativeOf(type.site.index())].isToldToEscape, true))
            m_deferred.append(Deferred { Deferred::Kind::SiteEscapes, type.site.index(), { } });
        break;
    }
    if (!namesSomethingNew)
        return;
    LinkType named = type;
    named.dropInstances();
    if (named.objects == LinkType::Objects::Prototypes || named.objects == LinkType::Objects::Lost) {
        named.objects = LinkType::Objects::None;
        named.layout = { };
        named.family = { };
    }
    if (named.functions == LinkType::Functions::Constructors) {
        named.functions = LinkType::Functions::None;
        named.constructorsOf = { };
    }
    m_deferred.append(Deferred { Deferred::Kind::TypeEscapes, 0, named });
}

void LinkFlow::loseFamilyThatHoldsLostConstructorsWith(FamilyID family, LayoutID onlyMemberLost)
{
    if (!family)
        return;
    LostConstructorsOfTree& lost = m_lostConstructorsOfTrees.add(families.topmostFamilyAtOrAbove(family).index(), LostConstructorsOfTree { }).iterator->value;
    if (!lost.nearestFamily) {
        lost = { family, onlyMemberLost };
        return;
    }
    if (onlyMemberLost && lost.onlyMember == onlyMemberLost)
        return;
    FamilyID holdsAll = families.nearestCommonFamily(lost.nearestFamily, family);
    bool wasToldLost = !lost.onlyMember && holdsAll == lost.nearestFamily;
    lost = { holdsAll, { } };
    if (!wasToldLost)
        deferLossOfConstructors(holdsAll);
}

void LinkFlow::settleWhatWasDeferred()
{
    m_isSettling = true;
    while (!m_deferred.isEmpty()) {
        m_letsReachWait = true;
        UnknownByEscapeAlone unknownByEscapeAlone(true);
        Deferred next = m_deferred.takeLast();
        switch (next.kind) {
        case Deferred::Kind::LayoutIsLost:
            becomeLost(LayoutID(next.number));
            break;
        case Deferred::Kind::FamilyIsLost: {
            std::span<const LayoutID> members = program.types.listOf(families.layoutsOf(FamilyID(next.number)));
            for (LayoutID member : members) {
                if (member == members[0] || mayBeKnownByFamily(member))
                    uniteLost(members[0], member);
            }
            break;
        }
        case Deferred::Kind::LayoutsAreUnited:
            uniteLost(LayoutID(next.number), LayoutID(next.other));
            break;
        case Deferred::Kind::LostFamilyEscapes:
            letLostFamilyEscape(LayoutID(next.number));
            break;
        case Deferred::Kind::FunctionIsLost:
            becomeLost(FunctionID(next.number));
            if (LayoutID lostClass = classOfConstructor(FunctionID(next.number)))
                loseFamilyThatHoldsLostConstructorsWith(families.nearestFamilyOf(lostClass), lostClass);
            break;
        case Deferred::Kind::FunctionsAreUnited:
            unite(FunctionID(next.number), FunctionID(next.other));
            break;
        case Deferred::Kind::PoolsOfLostFunctionsAreUnited:
            {
                BitVector keptInThisSettling;
                for (auto [kept, absorbed] : std::exchange(m_poolsOfLostFunctionsToTie, { })) {
                    ++m_tiesOfPools.all;
                    m_tiesOfPools.ofRowsOfUnequalLength += m_poolsOfLostFunctions[kept].arguments.size() != m_poolsOfLostFunctions[absorbed].arguments.size();
                    m_tiesOfPools.behindAnotherOfSameSettling += keptInThisSettling.get(absorbed);
                    keptInThisSettling.set(kept);
                    tiePoolsOfLostFunctions(kept, absorbed);
                    m_poolsOfLostFunctions[absorbed].keeper = kept;
                }
            }
            break;
        case Deferred::Kind::FamilyOfFunctionsEscapes:
            noteFamilyOfFunctionsAtUnseen(FunctionID(next.number));
            break;
        case Deferred::Kind::ConstructorsAreLost:
            loseFamilyThatHoldsLostConstructorsWith(FamilyID(next.number), { });
            {
                FunctionID first;
                for (LayoutID layout : program.types.listOf(families.layoutsOf(FamilyID(next.number)))) {
                    FunctionID constructor = program.at(layout).constructor;
                    if (!constructor)
                        continue;
                    deferLoss(constructor);
                    if (first)
                        deferUnion(first, constructor);
                    else
                        first = constructor;
                }
            }
            break;
        case Deferred::Kind::SitesAreUnited:
            unite(SiteID(next.number), next.type.site);
            break;
        case Deferred::Kind::LayoutEscapes:
            escape(LayoutID(next.number));
            break;
        case Deferred::Kind::FunctionEscapes:
            escape(FunctionID(next.number));
            break;
        case Deferred::Kind::ConstructorsEscape:
            for (LayoutID layout : program.types.listOf(families.layoutsOf(FamilyID(next.number)))) {
                if (FunctionID constructor = program.at(layout).constructor)
                    deferEscape(constructor);
            }
            break;
        case Deferred::Kind::PrototypesEscape:
            for (LayoutID layout : program.types.listOf(families.layoutsOf(FamilyID(next.number)))) {
                deferEscape(layout);
                noteReachesUnseen(typeOfPrototypeInLinkFlow(layout));
            }
            break;
        case Deferred::Kind::SiteEscapes:
            escape(SiteID(next.number));
            break;
        case Deferred::Kind::TypeEscapes:
            escapeAllOf(next.type);
            break;
        }
        if (m_deferred.isEmpty())
            layTiesThatSettlingLeft();
        if (m_deferred.isEmpty()) {
            m_letsReachWait = false;
            bringAliveWhatSettlingReached();
        }
    }
    RELEASE_ASSERT(m_tiesLeftBySettling.isEmpty());
    m_isSettling = false;
}

static ALWAYS_INLINE bool bringsNothingToShadowsInLinkFlow(const LinkType& type, const LinkType& held)
{
    return bringsNoPrimitiveInLinkFlow(type.directPrimitives(), held.directPrimitives()) && (!type.hasDirectUnknownObjects() || held.hasDirectUnknownObjects());
}

constinit thread_local LinkFlow::UnknownByEscapeAlone::Marks LinkFlow::UnknownByEscapeAlone::s_marksAtWork = LinkFlow::UnknownByEscapeAlone::Marks::Nothing;

bool LinkFlow::arrivalOfAnythingInSightBringsNothingTo(FlowNodeID node) const
{
    if (!m_isDeferringUpdates || node == unseen())
        return false;
    const LinkType& held = typeAt(node);
    return held.hasDirectUnknownObjects() && !(withoutLiteralsInLinkFlow(LinkType::anything().primitives) & ~held.directPrimitives());
}

void LinkFlow::arrive(FlowNodeID node, const LinkType& arriving, PropertyKeyID symbol)
{
    if (UnknownByEscapeAlone::marksAtWork() != UnknownByEscapeAlone::Marks::Nothing && arriving.hasUnknownObjects && !arriving.isUnknownEscapeOnly) [[unlikely]] {
        arrive(node, arriving.asEscapeOnly(), symbol);
        return;
    }
    const LinkType& type = arriving;
    if (!m_isDeferringUpdates) {
        arriveAtOnce(node, type, symbol);
        return;
    }
    if (type.isNever() || (node == unseen() && type.tellsUnseenNothing()))
        return;
    LinkType held = m_flowIsAtRest || node.index() >= m_nodes.size() ? typeAt(node) : headOfNode(node.index());
    bool namesNothing = type.objects == LinkType::Objects::None && type.functions == LinkType::Functions::None && type.classes == LinkType::Classes::None && !type.builtin
        && (!(type.primitives & LinkType::oneSymbol) || (held.directPrimitives() & Primitive::Symbol && !symbol) || !(type.directPrimitives() & LinkType::oneSymbol));
    if (namesNothing && node != unseen() && bringsNoPrimitiveInLinkFlow(type.primitives, held.primitives) && type.raisesNoFlagOver(held) && (!type.holdsLostBuiltins || held.holdsLostBuiltins || isNodeOfLibrary(node.index())) && bringsNothingToShadowsInLinkFlow(type, held))
        return;
    Lane& holder = deferringLane();
    holder.boxes[laneIndexOf(node)].constructAndAppend(node, symbol, type);
    holder.hasFilledBoxes = true;
}

ALWAYS_INLINE bool LinkFlow::isUnitedWith(const LinkType& held, const LinkType& arriving) const
{
    if (held.builtin || arriving.builtin || held.builtinClass != arriving.builtinClass || held.setOfClasses != arriving.setOfClasses || (held.hasUnknownObjects && held.setOfClasses))
        return false;
    auto isUnited = [](const auto& united, auto heldMember, auto arrivingMember) ALWAYS_INLINE_LAMBDA {
        return heldMember == arrivingMember || !arrivingMember || (heldMember && united.representativeWithoutShortening(heldMember.index()) == united.representativeWithoutShortening(arrivingMember.index()));
    };
    return isUnited(unitedLayouts, held.layout, arriving.layout) && isUnited(unitedFunctions, held.function, arriving.function)
        && (held.site == arriving.site || (held.site && arriving.site && m_familiesOfSites.representativeWithoutShortening(held.site.index()) == m_familiesOfSites.representativeWithoutShortening(arriving.site.index())));
}

ALWAYS_INLINE void LinkFlow::joinHeads(FlowNodeID node, Node& target, const LinkType& before, const LinkType& arriving)
{
    LinkType after;
    bool heldNothing = !before.primitives && before.objects == LinkType::Objects::None && before.functions == LinkType::Functions::None && before.classes == LinkType::Classes::None && !before.hasUnknownObjects && !before.holdsLostBuiltins;
    if (heldNothing && !(arriving.primitives & LinkType::oneSymbol)) {
        after = arriving;
        after.escapeOnlyPrimitives = withoutLiteralsInLinkFlow(arriving.primitives) & ~withoutLiteralsInLinkFlow(arriving.directPrimitives());
        after.primitives = withoutLiteralsInLinkFlow(arriving.primitives);
    } else
        after = joinedWithUninformativeType(before, arriving);
    if (after.holdsLostBuiltins && isNodeOfLibrary(node.index()))
        after.holdsLostBuiltins = false;
    if (after.holdsSameWithShadowsAs(before))
        return;
    if (m_breachesOfClosure) [[unlikely]]
        noteBreachOfClosure({ .node = node, .before = before, .after = after });
    target.head = after.head();
    if (!m_isOnWorklist.quickSet(node.index())) {
        Lane& lane = laneOf(node);
        lane.worklist.append(node);
        lane.typesBeforeClimb.append(before);
    }
}

ALWAYS_INLINE_INTO_OTHER_FILES void LinkFlow::receive(FlowNodeID node, const LinkType& type, PropertyKeyID symbol)
{
    if (node == unseen()) {
        noteReachesUnseen(type);
        return;
    }

    size_t index = node.index();
    Node& target = m_nodes[index];
    LinkType held = storedNodeType(target);
    bool namesNothing = (type.namesNothing() || bringsOwnReferences(target, held, type) || (isLostThroughout(type) && isLostThroughout(held) && admitsEveryName(target) && isUnitedWith(held, type)))
        && (!type.holdsLostBuiltins || held.holdsLostBuiltins || isNodeOfLibrary(index)) && (!(type.primitives & LinkType::oneSymbol) || (held.directPrimitives() & Primitive::Symbol && !symbol) || !(type.directPrimitives() & LinkType::oneSymbol));
    if (namesNothing && bringsNoPrimitiveInLinkFlow(type.primitives, held.primitives) && type.raisesNoFlagOver(held) && bringsNothingToShadowsInLinkFlow(type, held))
        return;
    if (type.marks & m_marksThatAreUsed) [[unlikely]] {
        receiveWithoutMarksThatAreUsed(node, type, symbol);
        return;
    }

    TypeID bound = target.typeOfPosition;
    LinkType arriving = type;
    if (bound == TypeTable::anything)
        arriving.primitives &= LinkType::closed(Primitive::All);
    else if (bound)
        arriving = passedBy(type, bound);
    if (bound && !arriving.primitives && arriving.isNever())
        return;
    if (((arriving.primitives | held.primitives) & Primitive::Double) && index - m_firstNodeOfFields < m_fieldsWhoseSlotsHaveDoors.size() && m_fieldsWhoseSlotsHaveDoors.quickGet(index - m_firstNodeOfFields)) {
        LinkType::Counts counts = held.primitives ? static_cast<LinkType::Counts>(static_cast<uint8_t>(held.counts) | static_cast<uint8_t>(arriving.counts)) : arriving.counts;
        bool areExactIntegers = m_fieldsThatKeepExactIntegers.quickGet(index - m_firstNodeOfFields) && !((arriving.primitives | held.primitives) & Primitive::FractionalNumber) && LinkType::areExact(counts);
        if (!areExactIntegers && (arriving.primitives & Primitive::Double || !(held.primitives & Primitive::FractionalNumber))) {
            uint16_t added = LinkType::closed(Primitive::Number) & ~arriving.primitives;
            arriving.primitives |= added;
            arriving.escapeOnlyPrimitives |= added;
            arriving.counts = LinkType::Counts::Unknown;
        }
    }
    const LinkType& before = held;
    if (!target.references && !symbol && arriving.namesNothing() && arriving.hasNoReferences() && !before.holdsOneSymbolInAnyPart() && !arriving.holdsOneSymbolInAnyPart()) {
        joinHeads(node, target, LinkType::ofHead(before.head()), LinkType::ofHead(arriving.head()));
        return;
    }
    bool heldNothing = !before.primitives && before.objects == LinkType::Objects::None && before.functions == LinkType::Functions::None && before.classes == LinkType::Classes::None && !before.hasUnknownObjects && !before.holdsLostBuiltins && !before.builtin;
    if (heldNothing && !(arriving.hasUnknownObjects && arriving.builtin) && !(arriving.primitives & LinkType::oneSymbol)) {
        arriving.escapeOnlyPrimitives = withoutLiteralsInLinkFlow(arriving.primitives) & ~withoutLiteralsInLinkFlow(arriving.directPrimitives());
        arriving.primitives = withoutLiteralsInLinkFlow(arriving.primitives);
        if (arriving.holdsLostBuiltins && isNodeOfLibrary(index))
            arriving.holdsLostBuiltins = false;
        if (arriving.holdsSameWithShadowsAs(before))
            return;
        if (m_breachesOfClosure) [[unlikely]]
            noteBreachOfClosure({ .node = node, .before = before, .after = arriving });
        storeNodeType(target, arriving, before);
        if (!m_isOnWorklist.quickSet(index)) {
            Lane& lane = laneOf(node);
            lane.worklist.append(node);
            lane.typesBeforeClimb.append(before);
        }
        return;
    }
    bool arrivingNamesNothing = arriving.namesNothing() && !before.builtin && !before.setOfClasses;
    if (m_setsMeetingsAside && !arrivingNamesNothing) {
        bool objectsMeet = arriving.objects != LinkType::Objects::None && before.objects != LinkType::Objects::None && before.objects != LinkType::Objects::Lost
            && (arriving.objects != before.objects || arriving.layout != before.layout || arriving.family != before.family);
        if (objectsMeet && before.objects == LinkType::Objects::Set && namesOnlyLayoutsOfSet(arriving, before)) {
            objectsMeet = false;
            arriving.objects = before.objects;
            arriving.layout = before.layout;
        }
        bool functionsMeet = arriving.functions != LinkType::Functions::None && before.functions != LinkType::Functions::None && before.functions != LinkType::Functions::Lost
            && (arriving.functions != before.functions || arriving.function != before.function || arriving.constructorsOf != before.constructorsOf);
        if (functionsMeet && before.functions == LinkType::Functions::Set && namesOnlyFunctionsOfSet(arriving, before)) {
            functionsMeet = false;
            arriving.functions = before.functions;
            arriving.function = before.function;
        }
        if (objectsMeet || functionsMeet) {
            LinkType met;
            if (objectsMeet) {
                met.objects = arriving.objects;
                arriving.objects = LinkType::Objects::None;
                met.layout = std::exchange(arriving.layout, { });
                met.family = std::exchange(arriving.family, { });
            }
            if (functionsMeet) {
                met.functions = arriving.functions;
                arriving.functions = LinkType::Functions::None;
                met.function = std::exchange(arriving.function, { });
                met.constructorsOf = std::exchange(arriving.constructorsOf, { });
            }
            laneOf(node).meetings.append({ node, met });
        }
    }
    LinkType after;
    if (arrivingNamesNothing) {
        after = joinedWithUninformativeType(before, arriving);
        if (after.holdsLostBuiltins && isNodeOfLibrary(index))
            after.holdsLostBuiltins = false;
    } else {
        Vector<BuiltinID>& lostInSight = builtinsLostInSightByThread();
        size_t numberLostInSight = lostInSight.size();
        after = joined(before, arriving, LossAtJoin::WaitsForUse);
        if (after.holdsLostBuiltins && isNodeOfLibrary(index)) {
            after.holdsLostBuiltins = false;
            lostInSight.shrink(numberLostInSight);
        }
    }
    if (LinkType::holdsOneSymbol(after.primitives) && arriving.primitives & LinkType::oneSymbol) {
        Locker locker { m_lockOfSymbolsAtNodes };
        if (!symbol || (before.primitives & LinkType::oneSymbol && m_symbolsAtNodes.get(static_cast<uint32_t>(index)) != symbol.bits()))
            after.primitives |= Primitive::Symbol;
        else
            m_symbolsAtNodes.set(static_cast<uint32_t>(index), symbol.bits());
    } else if (LinkType::holdsOneSymbol(after.directPrimitives()) && arriving.directPrimitives() & LinkType::oneSymbol) {
        Locker locker { m_lockOfSymbolsAtNodes };
        if (!symbol || (before.directPrimitives() & LinkType::oneSymbol && m_symbolsAtNodes.get(static_cast<uint32_t>(index)) != symbol.bits()))
            after.escapeOnlyPrimitives &= ~(Primitive::Symbol | LinkType::oneSymbol);
        else
            m_symbolsAtNodes.set(static_cast<uint32_t>(index), symbol.bits());
    }
    if ((symbol || LinkType::holdsOneSymbol(before.directPrimitives())) && after.directPrimitives() & Primitive::Symbol) [[unlikely]] {
        if (arriving.directPrimitives() & LinkType::oneSymbol)
            noteSymbolThatLosesItsName(symbol);
        if (LinkType::holdsOneSymbol(before.directPrimitives()))
            noteSymbolThatLosesItsName(symbolNotedAt(node));
    }
    if (!after.holdsSameWithShadowsAs(before)) {
        if (m_breachesOfClosure) [[unlikely]]
            noteBreachOfClosure({ .node = node, .before = before, .after = after });
        setTypeOfNode(index, after, before);
        if (!m_isOnWorklist.quickSet(index)) {
            Lane& lane = laneOf(node);
            lane.worklist.append(node);
            lane.typesBeforeClimb.append(before);
        }
    }
}

NEVER_INLINE void LinkFlow::receiveWithoutMarksThatAreUsed(FlowNodeID node, const LinkType& type, PropertyKeyID symbol)
{
    receive(node, type.without(m_marksThatAreUsed), symbol);
}

NEVER_INLINE void LinkFlow::arriveAtOnce(FlowNodeID node, const LinkType& type, PropertyKeyID symbol)
{
    if (!type.isNever())
        receive(node, type, symbol);
    settle();
}

ALWAYS_INLINE_INTO_OTHER_FILES bool LinkFlow::layEdge(Lane& lane, FlowNodeID from, FlowNodeID to, TypeID passes)
{
    ListsOfNode& lists = m_listsOfNodes[from.index()];
    uint32_t head = lists.firstEdge;
    uint32_t numberLookedThrough = 0;
    uint32_t next = head;
    bool isKnownToBeNew = false;
    if (to == unseen() && !passes) {
        if (m_flowsIntoUnseen.quickSet(from.index()))
            return false;
        isKnownToBeNew = true;
    }
    bool isAskedOfSet = !isKnownToBeNew && m_hasEdgesInSet.quickGet(from.index());
    for (; !isKnownToBeNew && !isAskedOfSet && next != UINT32_MAX && numberLookedThrough < numberOfEdgesLookedThroughInLinkFlow; ++numberLookedThrough) {
        const Edge& edge = lane.edges[next];
        if (edge.to == to && edge.passes == passes)
            return false;
        next = edge.next;
    }
    if (isAskedOfSet || numberLookedThrough == numberOfEdgesLookedThroughInLinkFlow) {
        uint32_t numberOfFrom = static_cast<uint32_t>(from.index());
        if (!m_hasEdgesInSet.quickSet(from.index())) {
            for (uint32_t listed = head; listed != UINT32_MAX; listed = lane.edges[listed].next)
                lane.edgesOfBusyNodes.add(numberOfFrom, static_cast<uint32_t>(lane.edges[listed].to.index()), lane.edges[listed].passes.bits());
        }
        if (!lane.edgesOfBusyNodes.add(numberOfFrom, static_cast<uint32_t>(to.index()), passes.bits()))
            return false;
    }
    if (head == UINT32_MAX && m_accessorsWaitAt.quickGet(from.index())) [[unlikely]]
        wakeAccessorsThatWaitAt(lane, from);
    lists.firstEdge = static_cast<uint32_t>(lane.edges.size());
    lane.edges.append(Edge { to, passes, head });
    return true;
}

void LinkFlow::addEdge(FlowNodeID from, FlowNodeID to, TypeID passes)
{
    if (from == to)
        return;
    if (from != unseen() && m_isDeferringUpdates && (!m_severalThreadsTell || !hasListsToItself(from))) {
        if (to == unseen() && !passes && !m_severalThreadsTell && m_flowsIntoUnseen.get(from.index()))
            return;
        Lane& holder = deferringLane();
        holder.edgesLaid[laneIndexOf(from)].constructAndAppend(from, to, passes);
        holder.hasEdgesLaid = true;
        return;
    }
    addEdgeAtOnce(from, to, passes);
}

NEVER_INLINE void LinkFlow::addEdgeAtOnce(FlowNodeID from, FlowNodeID to, TypeID passes)
{
    if (from == unseen()) {
        UnknownByEscapeAlone ofEscapeAlone(UnknownByEscapeAlone::marksAtWork() == UnknownByEscapeAlone::Marks::OfEscape);
        LinkType held = typeOfNode(from.index());
        if (passes)
            arrive(to, passedBy(held, passes));
        else
            arrive(to, held);
        return;
    }
    UnknownByEscapeAlone keepsItsOwnMarks(false);
    if (layEdge(laneOf(from), from, to, passes) && !isNeverAt(from.index()))
        arrive(to, passedByTestOfEdge(typeOfNode(from.index()), passes), symbolSeenAt(from));
}

NEVER_INLINE void LinkFlow::wakeAccessorsThatWaitAt(Lane& lane, FlowNodeID node)
{
    m_accessorsWaitAt.quickClear(node.index());
    for (uint32_t listed = m_listsOfNodes[node.index()].firstObserver; listed != UINT32_MAX; listed = lane.observers[listed].next()) {
        if (lane.observers[listed].kind() == FlowObserver::Kind::FieldOfAccessorLaid)
            lane.newObservers.append(std::pair { lane.observers[listed].observer(), node });
    }
}

bool LinkFlow::isAskedByMoreThanAccessorsThatWait(Lane& lane, FlowNodeID node)
{
    const ListsOfNode& lists = m_listsOfNodes[node.index()];
    if (lists.firstEdge != UINT32_MAX || !isNeverAt(node.index()))
        return true;
    if (m_accessorsWaitAt.quickGet(node.index()))
        return false;
    if (lane.nodesThatAccessorsFoundAsked.contains(node.index()))
        return true;
    bool isAsked = false;
    for (uint32_t listed = lists.firstObserver; listed != UINT32_MAX && !isAsked; listed = lane.observers[listed].next())
        isAsked = lane.observers[listed].kind() != FlowObserver::Kind::FieldOfAccessorLaid;
    if (isAsked)
        lane.nodesThatAccessorsFoundAsked.add(node.index());
    return isAsked;
}

NEVER_INLINE void LinkFlow::putObserverThatWaitsUntilNodeIsAsked(Lane& lane, FlowNodeID node, const FlowObserver& observer)
{
    bool isAsked = isAskedByMoreThanAccessorsThatWait(lane, node);
    if (isAsked)
        lane.newObservers.append(std::pair { observer, node });
    else
        m_accessorsWaitAt.quickSet(node.index());
}

NEVER_INLINE bool LinkFlow::isAskedForTheFirstTime(Lane& lane, FlowNodeID node)
{
    bool isAsked = isAskedByMoreThanAccessorsThatWait(lane, node);
    if (!isAsked && m_accessorsWaitAt.quickGet(node.index()))
        wakeAccessorsThatWaitAt(lane, node);
    return !isAsked;
}

ALWAYS_INLINE_INTO_OTHER_FILES void LinkFlow::putObserver(Lane& lane, FlowNodeID node, const FlowObserver& observer)
{
    static_assert(FlowObserver::Kind::FieldIsOnlyAsked > FlowObserver::Kind::FieldOfAccessorLaid);
    if (observer.kind >= FlowObserver::Kind::FieldOfAccessorLaid) [[unlikely]] {
        if (observer.kind == FlowObserver::Kind::FieldOfAccessorLaid)
            putObserverThatWaitsUntilNodeIsAsked(lane, node, observer);
        else if (!isAskedForTheFirstTime(lane, node))
            return;
    } else if (m_accessorsWaitAt.quickGet(node.index())) [[unlikely]]
        wakeAccessorsThatWaitAt(lane, node);
    ListsOfNode& lists = m_listsOfNodes[node.index()];
    uint32_t index = static_cast<uint32_t>(lane.observers.size());
    lane.observers.append(ListedFlowObserver(observer, lists.firstObserver));
    lists.firstObserver = index;
}

NEVER_INLINE bool LinkFlow::isObservedAlready(FlowNodeID node, const FlowObserver& observer)
{
    Lane& lane = laneOf(node);
    bool isThere = false;
    for (uint32_t listed = m_listsOfNodes[node.index()].firstObserver; listed != UINT32_MAX && !isThere; listed = lane.observers[listed].next())
        isThere = lane.observers[listed].observer() == observer;
    for (auto& holder : m_lanes) {
        for (const ObserverPut& put : holder->observersPut[laneIndexOf(node)])
            isThere |= put.node == node && put.observer == observer;
    }
    return isThere;
}

void LinkFlow::observe(FlowNodeID node, const FlowObserver& observer)
{
    if (m_breachesOfClosure && isObservedAlready(node, observer)) [[unlikely]]
        return;
    if (m_isDeferringUpdates && !hasListsToItself(node)) {
        Lane& holder = deferringLane();
        holder.observersPut[laneIndexOf(node)].constructAndAppend(node, observer);
        holder.hasObserversPut = true;
    } else
        putObserver(laneOf(node), node, observer);
    if (holdsNothingAt(node))
        return;
    if (m_severalThreadsTell)
        s_laneAtWork->newObservers.constructAndAppend(observer, node);
    else
        m_newObservers.constructAndAppend(observer, node);
}

static bool isAskedOfBuiltinsReadByNameInLinkFlow(const Program& program, PropertyKeyID key)
{
    return key == program.keyOf(BuiltinNames::constructor) || key == program.keyOf(BuiltinNames::prototype) || key == program.keyOf(BuiltinNames::name) || key == program.keyOf(BuiltinNames::length);
}

bool LinkFlow::answerOfReadStaysInSight(PropertyKeyID key) const
{
    return key == program.keyOf(BuiltinNames::constructor) || key == program.keyOf(BuiltinNames::prototype);
}

enum class ObserverBuiltinUse : uint8_t { DecidesItself, Inspects, Forwards, Uses };

static constexpr ObserverBuiltinUse observerBuiltinUse(FlowObserver::Kind kind)
{
    switch (kind) {
    case FlowObserver::Kind::SubjectOfCall:
    case FlowObserver::Kind::CalleeOfCall:
    case FlowObserver::Kind::ReceiverOfCall:
    case FlowObserver::Kind::MemberOfSharedCall:
    case FlowObserver::Kind::SubjectOfAccess:
    case FlowObserver::Kind::SubjectOfChange:
    case FlowObserver::Kind::OtherOfChange:
    case FlowObserver::Kind::Inspected:
        return ObserverBuiltinUse::DecidesItself;
    case FlowObserver::Kind::OperandOfOperator:
    case FlowObserver::Kind::OperandOnlyLookedAt:
    case FlowObserver::Kind::FieldIsOnlyAsked:
        return ObserverBuiltinUse::Inspects;
    case FlowObserver::Kind::MemberOfAccess:
    case FlowObserver::Kind::MemberOfSharedAccess:
        return ObserverBuiltinUse::Forwards;
    case FlowObserver::Kind::KeyOfAccess:
    case FlowObserver::Kind::SymbolOfPreparation:
    case FlowObserver::Kind::PassedToBeNamed:
    case FlowObserver::Kind::PassedToBeLinked:
    case FlowObserver::Kind::FieldOfAccessorLaid:
    case FlowObserver::Kind::KeysAreListed:
        return ObserverBuiltinUse::Uses;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool LinkFlow::onlyLooksAtBuiltinsReadByName(const FlowObserver& observer) const
{
    if (observerBuiltinUse(observer.kind) == ObserverBuiltinUse::Inspects)
        return true;
    if (observer.kind != FlowObserver::Kind::SubjectOfAccess)
        return false;
    const SummaryAccess& access = summaries.at(observer.function).accesses[observer.record];
    if (access.kind != SummaryAccess::Kind::Read || access.isToContents() || !access.key)
        return false;
    return isAskedOfBuiltinsReadByNameInLinkFlow(program, access.key);
}

bool LinkFlow::onlyLooksAtBuiltinsLostAtJoins(const FlowObserver& observer) const
{
    if (!onlyLooksAtBuiltinsReadByName(observer))
        return false;
    return observer.kind != FlowObserver::Kind::SubjectOfAccess || !answerOfReadStaysInSight(summaries.at(observer.function).accesses[observer.record].key);
}

void LinkFlow::noteAccessorMayBeLaidOn(const LinkType& subject, PropertyKeyID key, FunctionID writer)
{
    if (everyMarkIsUsed() || (!subject.builtin && !subject.holdsLostBuiltins && (!subject.hasUnknownObjects || m_isFunctionOfLibrary.quickGet(writer.index()))))
        return;
    if (!key || isAskedOfBuiltinsReadByNameInLinkFlow(program, key)) {
        note(Note::BuiltinsReadByNameAreUsed, 0);
        if (subject.marks & LinkType::BuiltinsLostAtJoins)
            note(Note::BuiltinsLostAtJoinsAreUsed, 0);
    }
}

void LinkFlow::tell(const FlowObserver& observer, const LinkType& now, const LinkType& before)
{
    if (uint8_t marksNotShownBefore = now.marks & ~before.marks) {
        if (marksNotShownBefore & LinkType::BuiltinsReadByName && !onlyLooksAtBuiltinsReadByName(observer))
            note(Note::BuiltinsReadByNameAreUsed, 0);
        if (marksNotShownBefore & LinkType::BuiltinsLostAtJoins && !onlyLooksAtBuiltinsLostAtJoins(observer))
            note(Note::BuiltinsLostAtJoinsAreUsed, 0);
        LinkType unmarked = now.without(marksNotShownBefore);
        if (unmarked == before && unmarked.hasSameShadowsAs(before))
            return;
    }
    auto recordBuiltinUse = [&](const LinkType& shown) {
        if (shown.builtin)
            note(Note::BuiltinUsedViaFlow, program.builtins().canonicalID(shown.builtin).index());
    };
    LinkType& typeObservedBefore = s_laneAtWork ? s_laneAtWork->typeObservedBefore : m_typeObservedBefore;
    LinkType outer = std::exchange(typeObservedBefore, before);
    CodeAtWork codeAtWork(*this, observer.function);
    ++(s_laneAtWork ? s_laneAtWork->numberOfObserversTold : m_numberOfObserversTold);
    if (now.functions == LinkType::Functions::Set && observer.kind != FlowObserver::Kind::SubjectOfCall && observer.kind != FlowObserver::Kind::CalleeOfCall && observer.kind != FlowObserver::Kind::PassedToBeLinked && observer.kind != FlowObserver::Kind::FieldIsOnlyAsked && observer.kind != FlowObserver::Kind::KeysAreListed && !needsPrimitive(observer))
        loseFunctionsOfSet(now);
    switch (observer.kind) {
    case FlowObserver::Kind::SubjectOfCall:
    case FlowObserver::Kind::CalleeOfCall:
    case FlowObserver::Kind::ReceiverOfCall:
    case FlowObserver::Kind::MemberOfSharedCall:
    case FlowObserver::Kind::OperandOnlyLookedAt:
        callObserved(observer, now);
        break;
    case FlowObserver::Kind::KeyOfAccess:
        static_assert(observerBuiltinUse(FlowObserver::Kind::KeyOfAccess) == ObserverBuiltinUse::Uses);
        recordBuiltinUse(now);
        [[fallthrough]];
    case FlowObserver::Kind::SubjectOfAccess:
    case FlowObserver::Kind::MemberOfAccess:
    case FlowObserver::Kind::MemberOfSharedAccess:
        accessObserved(observer, now);
        break;
    case FlowObserver::Kind::FieldOfAccessorLaid:
        static_assert(observerBuiltinUse(FlowObserver::Kind::FieldOfAccessorLaid) == ObserverBuiltinUse::Uses);
        recordBuiltinUse(now);
        [[fallthrough]];
    case FlowObserver::Kind::SubjectOfChange:
    case FlowObserver::Kind::OtherOfChange:
        changeObserved(observer, now);
        break;
    case FlowObserver::Kind::FieldIsOnlyAsked:
        break;
    case FlowObserver::Kind::KeysAreListed:
        static_assert(observerBuiltinUse(FlowObserver::Kind::KeysAreListed) == ObserverBuiltinUse::Uses);
        recordBuiltinUse(now);
        noteLossOfLightFormAmong(now);
        noteKeysBecomeTextsAmong(now);
        break;
    case FlowObserver::Kind::OperandOfOperator:
        operatorObserved(observer, now);
        break;
    case FlowObserver::Kind::SymbolOfPreparation:
        static_assert(observerBuiltinUse(FlowObserver::Kind::SymbolOfPreparation) == ObserverBuiltinUse::Uses);
        recordBuiltinUse(now);
        preparationObserved(observer, now);
        break;
    case FlowObserver::Kind::Inspected:
        inspectionObserved(observer, now);
        break;
    case FlowObserver::Kind::PassedToBeNamed:
        static_assert(observerBuiltinUse(FlowObserver::Kind::PassedToBeNamed) == ObserverBuiltinUse::Uses);
        recordBuiltinUse(now);
        passedToBeNamedObserved(observer, now);
        break;
    case FlowObserver::Kind::PassedToBeLinked:
        static_assert(observerBuiltinUse(FlowObserver::Kind::PassedToBeLinked) == ObserverBuiltinUse::Uses);
        recordBuiltinUse(now);
        prototypeHelperCallObserved(observer);
        break;
    }
    typeObservedBefore = outer;
}

void LinkFlow::tellKeysThatBecomeTexts(LayoutID id)
{
    const Layout& layout = program.at(id);
    auto namesEvent = [&](const Field& field) {
        return field.key && m_keysThatNameEvents.get(field.key.index());
    };
    uint8_t told = m_keysOfLayoutAreTold.at(id.index());
    if (told == nothingIsLeftToTell)
        return;
    if (!told) {
        bool isOfProgram = (layout.kind == LayoutKind::Record || layout.kind == LayoutKind::Instance || layout.kind == LayoutKind::Container) && layout.module
            && program.at(layout.module).kind != ModuleKind::RuntimeSource && !program.at(layout.module).flags.contains(ModuleFlags::IsLibrary);
        bool waits = isOfProgram && std::ranges::any_of(layout.fields, namesEvent);
        m_keysOfLayoutAreTold.set(id.index(), waits ? waitsForProgramToAsk : nothingIsLeftToTell);
        if (!waits)
            return;
    }
    if (isInLibraryCode())
        return;
    m_keysOfLayoutAreTold.set(id.index(), nothingIsLeftToTell);
    for (const Field& field : layout.fields) {
        if (namesEvent(field))
            note(Note::NameRead, field.key.bits(), true);
    }
}

void LinkFlow::noteKeysBecomeTextsAmongSet(const LinkType& type)
{
    for (LayoutID member : layoutsOfSet(type))
        noteKeysBecomeTexts(member);
}

void LinkFlow::tellNameThatBecomesText(FunctionID id)
{
    const Function& function = program.at(id);
    uint8_t told = m_nameOfFunctionIsTold.at(id.index());
    if (told == nothingIsLeftToTell)
        return;
    if (!told) {
        bool waits = function.name && m_textsThatNameEvents.get(function.name.index()) && !isLibraryCode(program, function);
        m_nameOfFunctionIsTold.set(id.index(), waits ? waitsForProgramToAsk : nothingIsLeftToTell);
        if (!waits)
            return;
    }
    if (isInLibraryCode())
        return;
    m_nameOfFunctionIsTold.set(id.index(), nothingIsLeftToTell);
    note(Note::NameRead, program.keyOf(program.strings.builtinName(function.name)).bits(), true);
}

void LinkFlow::noteNamesOfLostFunctionsBecomeTexts()
{
    if (m_namesOfLostFunctionsAreTold.at(0) || isInLibraryCode())
        return;
    m_namesOfLostFunctionsAreTold.set(0, nothingIsLeftToTell);
    for (uint32_t index = 0; index < program.functions.size(); ++index)
        noteNameBecomesText(FunctionID(index));
}

const LinkType& LinkFlow::typeObservedBefore() const
{
    return s_laneAtWork ? s_laneAtWork->typeObservedBefore : m_typeObservedBefore;
}

unsigned LinkFlow::indexOfLaneAtWork() const
{
    return s_laneAtWork ? s_laneAtWork->index : 0;
}

LinkFlow::CopiesObserved& LinkFlow::copiesObservedIn(FunctionID function)
{
    Lane& lane = *m_lanes[laneOfObserversOf(function)];
    RELEASE_ASSERT(!s_laneAtWork || s_laneAtWork == &lane);
    return lane.copiesObserved;
}

void LinkFlow::noteAccessorLaid(LayoutID layout, const AccessorLaidInField& laid)
{
    if (m_severalThreadsTell) {
        s_laneAtWork->accessorsLaid.append({ layout, laid });
        return;
    }
    layoutsWithKnownAccessors.set(layout.index());
    if (!laid.appendedKey)
        fieldsWithKnownAccessors.append(laid.field);
    accessorsLaidInFields.append(laid);
}

std::optional<uint32_t> LinkFlow::findWhatLaneWasGiven(MadeOnDemand kind, uint64_t words) const
{
    auto& given = s_laneAtWork->given[static_cast<size_t>(kind)];
    auto found = given.find(words);
    if (found == given.end())
        return std::nullopt;
    return found->value;
}

void LinkFlow::keepWhatLaneWasGiven(MadeOnDemand kind, uint64_t words, uint32_t made) const
{
    s_laneAtWork->given[static_cast<size_t>(kind)].set(words, made);
}

auto LinkFlow::lockPartOfWhatSomeLaneWasGiven(MadeOnDemand kind, uint64_t words) const -> PartOfWhatSomeLaneWasGiven&
{
    PartOfWhatSomeLaneWasGiven& part = m_givenToSomeLane[static_cast<size_t>(kind)][DefaultHash<uint64_t>::hash(words) % numberOfPartsOfWhatSomeLaneWasGiven];
    while (part.isLocked.load(std::memory_order_relaxed) || part.isLocked.exchange(true, std::memory_order_acquire))
        simde_mm_pause();
    return part;
}

std::optional<uint32_t> LinkFlow::findWhatSomeLaneWasGiven(MadeOnDemand kind, uint64_t words) const
{
    PartOfWhatSomeLaneWasGiven& part = lockPartOfWhatSomeLaneWasGiven(kind, words);
    auto found = part.given.find(words);
    std::optional<uint32_t> known = found == part.given.end() ? std::nullopt : std::optional<uint32_t>(found->value);
    part.isLocked.store(false, std::memory_order_release);
    return known;
}

void LinkFlow::keepWhatSomeLaneWasGiven(MadeOnDemand kind, uint64_t words, uint32_t made) const
{
    PartOfWhatSomeLaneWasGiven& part = lockPartOfWhatSomeLaneWasGiven(kind, words);
    part.given.set(words, made);
    part.isLocked.store(false, std::memory_order_release);
}

void LinkFlow::forgetWhatSomeLaneWasGiven(MadeOnDemand kind)
{
    for (PartOfWhatSomeLaneWasGiven& part : m_givenToSomeLane[static_cast<size_t>(kind)])
        part.given.clear();
}

void LinkFlow::showExport(ModuleID module, uint32_t index)
{
    auto show = [&](VariableID shown) {
        note(Note::ExportIsShown, shown.index());
        addEdge(variable(shown), unseen());
    };
    bool mayBeDropped = index < program.at(module).exports.size() && program.at(module).exports[index].isHiddenUntilInitialized;
    if (VariableID shown = mayBeDropped ? VariableID() : program.variableBehindExport(module, index)) {
        show(shown);
        return;
    }
    if (index >= program.at(module).exports.size())
        return;
    Vector<std::pair<ModuleID, StringID>, 4> namesThatRunFinds;
    auto leaveToRun = [&](ModuleID barrel, StringID name) {
        if (!namesThatRunFinds.contains(std::pair { barrel, name }))
            namesThatRunFinds.append({ barrel, name });
    };
    auto follow = [&](ModuleID exporter, size_t place) {
        for (size_t steps = 0; steps <= program.modules.size(); ++steps) {
            const ModuleExport& entry = program.at(exporter).exports[place];
            if (entry.kind == ModuleExportKind::Variable && entry.variable)
                show(entry.variable);
            if (entry.kind == ModuleExportKind::Field && entry.field)
                addEdge(field({ entry.layout, *entry.field }), unseen());
            if (entry.isHiddenUntilInitialized)
                leaveToRun(exporter, entry.name);
            if (entry.kind != ModuleExportKind::Indirect || !entry.module || !entry.importName)
                return;
            const auto& exportsOfTarget = program.at(entry.module).exports;
            auto listed = std::ranges::find(exportsOfTarget, entry.importName, &ModuleExport::name);
            if (listed == exportsOfTarget.end()) {
                leaveToRun(entry.module, entry.importName);
                return;
            }
            exporter = entry.module;
            place = listed - exportsOfTarget.begin();
        }
    };
    follow(module, index);
    for (size_t next = 0; next < namesThatRunFinds.size(); ++next) {
        auto [barrel, name] = namesThatRunFinds[next];
        for (const ModuleRequest& request : program.at(barrel).requests) {
            if (!request.isDynamicStarExport)
                continue;
            const auto& exportsOfTarget = program.at(request.target).exports;
            auto listed = std::ranges::find(exportsOfTarget, name, &ModuleExport::name);
            if (listed == exportsOfTarget.end())
                leaveToRun(request.target, name);
            else
                follow(request.target, listed - exportsOfTarget.begin());
        }
    }
}

void LinkFlow::note(Note note, uint32_t number, uint32_t more)
{
    if (!m_severalThreadsTell && !m_tellsObservers) {
        applyNote(note, number, more);
        return;
    }
    RELEASE_ASSERT(more < 1u << 24);
    Lane& lane = m_severalThreadsTell ? *s_laneAtWork : *m_firstLane;
    uint64_t words = static_cast<uint64_t>(note) << 56 | static_cast<uint64_t>(more) << 32 | number;
    uint64_t& lately = lane.notesListedLately[(words * 0x9e3779b97f4a7c15ull) >> 54];
    if (lately == ~words)
        return;
    lately = ~words;
    if (lane.notesListed.add(words).isNewEntry)
        lane.notes.append({ note, number, more });
}

BitVector& LinkFlow::layoutsWithNotedBases()
{
    return m_severalThreadsTell ? s_laneAtWork->layoutsWithNotedBases : m_layoutsWithNotedBases;
}

Vector<uint32_t>& LinkFlow::fieldsThatLaneAtWorkHasNotNotedEmpty(LayoutID layout)
{
    Lane& lane = m_severalThreadsTell ? *s_laneAtWork : *m_firstLane;
    return lane.fieldsNotNotedEmpty.ensure(layout.index(), [&] {
        return Vector<uint32_t>(program.at(layout).fields.size(), [](size_t field) { return static_cast<uint32_t>(field); });
    }).iterator->value;
}

void LinkFlow::flushFirstLaneNotes()
{
    for (size_t place = 0; place < m_firstLane->notes.size(); ++place) {
        Lane::NoteTaken taken = m_firstLane->notes[place];
        applyNote(taken.note, taken.number, taken.more);
    }
    m_firstLane->notes.shrink(0);
}

void LinkFlow::applyNote(Note note, uint32_t number, uint32_t more)
{
    if (m_testRuns->isTestRun()) {
        if (note == Note::NameRead || note == Note::NameCalled || note == Note::NameReadWithAnswerInSight)
            m_testRuns->noteNameFoundOffUnknownValue(PropertyKeyID::fromBits(number));
        else if (note == Note::BuiltinRead || note == Note::BuiltinCalled || note == Note::BuiltinCalledOutOfSight)
            m_testRuns->noteFound(BuiltinID(number));
    }
    if ((note == Note::BuiltinCalled || note == Note::BuiltinCalledOutOfSight) && program.builtins().has(BuiltinMark::LaysOperandAbovePrototype, BuiltinID(number)))
        m_operandIsLaidAbovePrototype = true;
    if (m_operandIsLaidAbovePrototype && !builtinsReadByNameAreUsed() && note != Note::BuiltinsReadByNameAreUsed)
        applyNote(Note::BuiltinsReadByNameAreUsed, 0, 0);
    if (m_operandIsLaidAbovePrototype)
        m_marksThatAreUsed |= LinkType::BuiltinsLostAtJoins;
    switch (note) {
    case Note::NameRead:
        builtinsInReach().noteNameRead(PropertyKeyID::fromBits(number));
        if (more)
            builtinsInReach().noteNameSpelledByProgram(PropertyKeyID::fromBits(number));
        return;
    case Note::NameReadWithAnswerInSight:
        if (builtinsReadByNameAreUsed())
            builtinsInReach().noteNameRead(PropertyKeyID::fromBits(number));
        else if (!m_namesReadWithAnswersInSight.contains(PropertyKeyID::fromBits(number)))
            m_namesReadWithAnswersInSight.append(PropertyKeyID::fromBits(number));
        if (more)
            builtinsInReach().noteNameSpelledByProgram(PropertyKeyID::fromBits(number));
        return;
    case Note::BuiltinsLostAtJoinsAreUsed:
        m_marksThatAreUsed |= LinkType::BuiltinsLostAtJoins;
        return;
    case Note::BuiltinsReadByNameAreUsed:
        m_marksThatAreUsed |= LinkType::BuiltinsReadByName;
        for (PropertyKeyID key : std::exchange(m_namesReadWithAnswersInSight, { }))
            builtinsInReach().noteNameRead(key);
        return;
    case Note::NameCalled:
        builtinsInReach().noteNameCalled(PropertyKeyID::fromBits(number), !(more & passesNoOperand));
        if (more & spelledByProgram)
            builtinsInReach().noteNameSpelledByProgram(PropertyKeyID::fromBits(number));
        return;
    case Note::ReadUnderNameThatLoopGave:
        builtinsInReach().noteReadUnderNameThatLoopGave();
        return;
    case Note::ReadUnderKeyNobodyKnowsOffHolder:
        builtinsInReach().noteReadUnderKeyNobodyKnowsOff(BuiltinID(number));
        return;
    case Note::ReadOfProgramUnderKeyNobodyKnows:
        builtinsInReach().noteReadOfProgramUnderKeyNobodyKnows(FunctionID(number - 1), ValueID(more));
        return;
    case Note::ReadUnderKeyNobodyKnows:
        builtinsInReach().noteReadUnderKeyNobodyKnows();
        if (number)
            builtinsInReach().noteReadOfProgramUnderKeyNobodyKnows(FunctionID(number - 1), ValueID(more));
        return;
    case Note::LostFunctionCalled:
        m_lostFunctionsThatCallsName.set(number);
        m_someCallNamesLostFunctions = true;
        return;
    case Note::LostFunctionsCalledByFamily:
        if (m_calledFunctionFamilies.add(unitedFunctions.representativeOf(number)))
            noteCallOfFamilyMembers(FunctionID(number));
        return;
    case Note::EveryLostFunctionCalled:
        m_lostFunctionsAreCalled = true;
        return;
    case Note::BuiltinRead:
    case Note::BuiltinReadThroughAggregate:
        builtinsInReach().noteRead(BuiltinID(number));
        return;
    case Note::BuiltinCalled:
        builtinsInReach().noteCalled(BuiltinID(number));
        if (more & byRowThatMayRunCode)
            builtinsInReach().noteCallMayRunCode(BuiltinID(number));
        if (more & asConstructor)
            builtinsInReach().noteConstructed(BuiltinID(number));
        if (more & byCode)
            builtinsInReach().noteCalledByCode(BuiltinID(number));
        return;
    case Note::BuiltinCalledOutOfSight:
        builtinsInReach().noteCalledOutOfSight(BuiltinID(number));
        if (more & withoutRow)
            builtinsInReach().noteCalledWithoutRow(BuiltinID(number));
        return;
    case Note::BuiltinCalledWithOperandsNobodyRead:
        builtinsInReach().noteCalledWithOperandsNobodyRead(BuiltinID(number));
        return;
    case Note::NameCalledByNativeCodeOffUnseenObjects:
        builtinsInReach().noteNameCalledByNativeCodeOffUnseenObjects(PropertyKeyID::fromBits(number));
        return;
    case Note::FieldWrittenAfterConstruction:
        fieldsWrittenAfterConstruction.set(number);
        return;
    case Note::FieldMayBeEmpty:
        if (fieldsThatMayBeEmpty.add(number) && !m_nodesFoundWhereFieldsAreEmpty.isEmpty() && m_nodesFoundWhereFieldsAreEmpty[number] != UINT32_MAX)
            arrive(FlowNodeID(m_nodesFoundWhereFieldsAreEmpty[number]), fromTypeID(TypeTable::undefined));
        return;
    case Note::LayoutAllocated:
        allocatedLayouts.set(number);
        return;
    case Note::ClassConstructed:
        constructedClasses.set(number);
        return;
    case Note::ExportIsShown:
        exportsShown.set(number);
        return;
    case Note::ChangeOfLayout:
        possibleChanges[number].add(OptionSet<LayoutChange>::fromRaw(static_cast<uint8_t>(more)));
        if (!(more & changeIsByStoreInSight))
            changesByMoreThanStoresInSight[number].add(OptionSet<LayoutChange>::fromRaw(static_cast<uint8_t>(more)));
        if (more & static_cast<uint32_t>(LayoutChange::IsFrozen))
            noteLockableInstances(LayoutID(number));
        return;
    case Note::LayoutGainsWhatUnseenCodeGaveSources:
        layoutsThatGainWhatUnseenCodeGaveSources.set(number);
        return;
    case Note::LayoutDefinedOnUnderWrittenKeys:
        layoutsDefinedOnUnderWrittenKeys.set(number);
        noteLockableInstances(LayoutID(number));
        return;
    case Note::LayoutDefinedOnUnderKeyThatIsNotWritten:
        layoutsDefinedOnUnderKeysThatAreNotWritten.set(number);
        return;
    case Note::LayoutGivenAccessorByDescriptorInSight:
        layoutsGivenAccessorsByDescriptorsInSight.set(number);
        return;
    case Note::FieldGivenAccessorByDescriptorInSight:
        fieldsGivenAccessorsByDescriptorsInSight.set(number);
        return;
    case Note::LayoutGetsNullAbove:
        layoutsThatGetNullAbove.set(number);
        return;
    case Note::PrototypeObjectRead:
        functionsThatReadPrototypeObjects.set(number);
        return;
    case Note::PrototypeLaidAboveOtherObjects:
        prototypesLaidAboveOtherObjects.quickSet(number);
        return;
    case Note::WriteOnBuiltin:
        builtinsInReach().noteWrite(BuiltinID(more & 0xffff), PropertyKeyID::fromBits(number), !!(more & onlyDeletes));
        return;
    case Note::BuiltinHandedWhatIsNoTextNumberOrBoolean:
        builtinsInReach().noteHandedWhatIsNoTextNumberOrBoolean(BuiltinID(number));
        return;
    case Note::ReadThroughLostBuiltins:
        builtinsInReach().noteReadThroughLostBuiltins(PropertyKeyID::fromBits(number));
        return;
    case Note::WriteThroughLostBuiltins:
        builtinsInReach().noteWriteThroughLostBuiltins(PropertyKeyID::fromBits(number), more);
        return;
    case Note::WriteThroughUnseenByProgram:
        builtinsInReach().noteWriteThroughUnseenByProgram(PropertyKeyID::fromBits(number), more);
        return;
    case Note::NameWrittenThroughUnseenByLibrary:
        builtinsInReach().noteNameWrittenThroughUnseenByLibrary(PropertyKeyID::fromBits(number));
        return;
    case Note::BuiltinCalledAtSite: {
        if (!callsOfBuiltinsTold.add(static_cast<uint64_t>(number) << 24 | more).isNewEntry)
            return;
        BuiltinID row(more & 0x7fffff);
        bool isConstruction = more >> 23;
        if (number >= calleesOfCalls.size()) {
            builtinsInReach().noteCalledAtSite({ }, { }, isConstruction, row);
            return;
        }
        FunctionID caller(static_cast<uint32_t>(std::ranges::upper_bound(firstCallOfFunction, number) - firstCallOfFunction.begin()) - 1);
        builtinsInReach().noteCalledAtSite(caller, summaries.at(caller).calls[number - firstCallOfFunction[caller.index()]].site, isConstruction, row);
        return;
    }
    case Note::RowApplied:
        builtinsInReach().noteRowApplied(BuiltinID(more), BuiltinOverloadID(number));
        return;
    case Note::CallLeftToUnseen: {
        FunctionID caller(static_cast<uint32_t>(std::ranges::upper_bound(firstCallOfFunction, number) - firstCallOfFunction.begin()) - 1);
        builtinsInReach().noteCallLeftToUnseen(caller, summaries.at(caller).calls[number - firstCallOfFunction[caller.index()]].site);
        return;
    }

    case Note::OptionThatRunsCodePassed:
        builtinsInReach().noteOptionThatRunsCodePassed(BuiltinID(number));
        return;
    case Note::OperandMayMeetCondition:
        builtinsInReach().noteOperandMayMeetCondition(BuiltinID(number), BuiltinName(more));
        return;
    case Note::InstancesLoseTheirConstructor:
        builtinsInReach().noteInstancesLoseTheirConstructor();
        return;
    case Note::ObjectHandedToFormatter:
        builtinsInReach().noteObjectHandedToFormatter(number == walkedByFormatter);
        return;
    case Note::TypeHandedToHandler:
        builtinsInReach().noteTypeHandedToHandler(BuiltinTypeID(number));
        return;
    case Note::BuiltinStandsAboveClass:
        builtinsInReach().noteStandsAboveClass(BuiltinID(number), FunctionID(more));
        return;
    case Note::BuiltinUsedViaFlow:
        builtinsInReach().recordUsedViaFlow(BuiltinID(number));
        return;
    case Note::BuiltinFoundWhereFlowBroughtHolder:
        builtinsInReach().noteFoundWhereFlowBroughtHolder(BuiltinID(number), FunctionID(more));
        return;
    case Note::InstanceMadeOutOfSight:
        builtinsInReach().noteInstanceAtUnseen(BuiltinClassID(static_cast<uint16_t>(number)));
        return;
    case Note::InstanceHandedToFormatter:
        builtinsInReach().noteInstanceHandedToFormatter(more ? BuiltinClassID(static_cast<uint16_t>(number)) : BuiltinClassID());
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void LinkFlow::noteFunctionToReach(FunctionID function)
{
    s_laneAtWork->functionsToReach.append(function);
}

void LinkFlow::leaveToReach(uint8_t what, uint32_t index)
{
    s_laneAtWork->leftToReach.append({ what, index });
}

Vector<LinkFlow::LeftToReach> LinkFlow::takeWhatLaneLeftToReach(unsigned lane)
{
    return std::exchange(m_lanes[lane]->leftToReach, { });
}

bool LinkFlow::isToldByOneThread(FlowObserver::Kind kind)
{
    switch (kind) {
    case FlowObserver::Kind::SubjectOfCall:
    case FlowObserver::Kind::CalleeOfCall:
    case FlowObserver::Kind::ReceiverOfCall:
    case FlowObserver::Kind::SubjectOfAccess:
    case FlowObserver::Kind::KeyOfAccess:
    case FlowObserver::Kind::MemberOfAccess:
    case FlowObserver::Kind::SubjectOfChange:
    case FlowObserver::Kind::OtherOfChange:
    case FlowObserver::Kind::FieldOfAccessorLaid:
    case FlowObserver::Kind::FieldIsOnlyAsked:
    case FlowObserver::Kind::OperandOfOperator:
    case FlowObserver::Kind::PassedToBeNamed:
    case FlowObserver::Kind::OperandOnlyLookedAt:
    case FlowObserver::Kind::KeysAreListed:
        return false;
    case FlowObserver::Kind::MemberOfSharedCall:
    case FlowObserver::Kind::MemberOfSharedAccess:
    case FlowObserver::Kind::SymbolOfPreparation:
    case FlowObserver::Kind::Inspected:
    case FlowObserver::Kind::PassedToBeLinked:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uint64_t LinkFlow::nextScatteredNumber()
{
    m_scatteredNumber = m_scatteredNumber * 6364136223846793005ull + 1442695040888963407ull;
    return m_scatteredNumber >> 33;
}

ALWAYS_INLINE_INTO_OTHER_FILES std::pair<FlowNodeID, LinkType> LinkFlow::takeNode(Lane& lane)
{
    switch (m_order) {
    case OrderOfWorklist::NewestFirst:
        break;
    case OrderOfWorklist::OldestFirst: {
        std::pair taken { lane.worklist[lane.firstInWorklist], lane.typesBeforeClimb[lane.firstInWorklist] };
        if (++lane.firstInWorklist == lane.worklist.size()) {
            lane.worklist.shrink(0);
            lane.typesBeforeClimb.shrink(0);
            lane.firstInWorklist = 0;
        }
        return taken;
    }
    case OrderOfWorklist::Scattered: {
        lane.scatteredNumber = lane.scatteredNumber * 6364136223846793005ull + 1442695040888963407ull;
        size_t place = (lane.scatteredNumber >> 33) % lane.worklist.size();
        std::swap(lane.worklist[place], lane.worklist.last());
        std::swap(lane.typesBeforeClimb[place], lane.typesBeforeClimb.last());
        break;
    }
    }
    FlowNodeID node = lane.worklist.takeLast();
    return { node, lane.typesBeforeClimb.takeLast() };
}

std::pair<FlowObserver, FlowNodeID> LinkFlow::takeNewObserver()
{
    switch (m_order) {
    case OrderOfWorklist::NewestFirst:
        break;
    case OrderOfWorklist::OldestFirst: {
        std::pair taken = m_newObservers[m_firstOfNewObservers];
        if (++m_firstOfNewObservers == m_newObservers.size()) {
            m_newObservers.shrink(0);
            m_firstOfNewObservers = 0;
        }
        return taken;
    }
    case OrderOfWorklist::Scattered:
        std::swap(m_newObservers[nextScatteredNumber() % m_newObservers.size()], m_newObservers.last());
        break;
    }
    return m_newObservers.takeLast();
}

void LinkFlow::run()
{
    flow();
    while (layMembersThatCopiesBring() || fillParametersThatUnseenCodeAlonePasses() || builtinsInReach().allowRuntimeNames())
        flow();
    noteLostLayoutsGainingProperties();
    countAllocatedMembersOfFamilies();
    for (const PoolOfLostFunctions& pool : m_poolsOfLostFunctions) {
        uint32_t keeper = m_lostFunctionsAreCalled ? poolOfAllLostFunctions : pool.keeper;
        RELEASE_ASSERT(keeper == UINT32_MAX || pool.arguments.size() <= m_poolsOfLostFunctions[keeper].arguments.size());
    }
}

ALWAYS_INLINE_INTO_OTHER_FILES LinkType LinkFlow::withoutObjectsThatHaveEscaped(LinkType type) const
{
    if (!type.hasUnknownObjects || reading() != WhatUnseenCodeDoes::Anything)
        return type;
    bool objectsHaveEscaped = false;
    switch (type.objects) {
    case LinkType::Objects::None:
        break;
    case LinkType::Objects::Layout:
    case LinkType::Objects::Prototype:
        objectsHaveEscaped = escapedLayouts.get(type.layout.index());
        break;
    case LinkType::Objects::Set:
        objectsHaveEscaped = std::ranges::all_of(layoutsOfSet(type), [&](LayoutID member) {
            return escapedLayouts.get(member.index());
        });
        break;
    case LinkType::Objects::Prototypes:
    case LinkType::Objects::Family:
        objectsHaveEscaped = m_familiesThatHaveEscaped.quickGet(type.family.index());
        break;
    case LinkType::Objects::Lost:
        objectsHaveEscaped = !type.layout || m_lostFamiliesThatHaveEscaped.quickGet(type.layout.index());
        break;
    }
    if (objectsHaveEscaped) {
        type.objects = LinkType::Objects::None;
        type.layout = { };
        type.family = { };
    }
    return type;
}

std::unique_ptr<LinkFlow::ViewOfHeldReading> LinkFlow::makeViewOfHeldReading()
{
    auto view = makeUnique<ViewOfHeldReading>();
    settleSharedCallsAs(WhatUnseenCodeDoes::OnlyReadsAndCalls);
    view->escapedLayouts.ensureSize(escapedLayouts.size());
    view->calleesOfSharedCalls = calleesOfSharedCalls;
    view->calleesOfCalls = unsafeMakeSpan(static_cast<CalleesOfCall*>(fastMalloc(calleesOfCalls.size() * sizeof(CalleesOfCall))), calleesOfCalls.size());
    forEachRangeOfIndicesInParallel(loops, calleesOfCalls.size(), [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index)
            new (NotNull, &view->calleesOfCalls[index]) CalleesOfCall(calleesOfCalls[index]);
    });
    return view;
}

LinkFlow::ViewOfHeldReading::~ViewOfHeldReading()
{
    for (CalleesOfCall& callees : calleesOfCalls)
        callees.~CalleesOfCall();
    fastFree(calleesOfCalls.data());
}

void LinkFlow::settleTypes()
{
    BitVector familiesWithLayoutThatHasNotEscaped = families.familiesWithSomeMemberThat([&](LayoutID member) {
        return !escapedLayouts.get(member.index());
    });
    BitVector familiesWithConstructorThatHasNotEscaped = families.familiesWithSomeMemberThat([&](LayoutID member) {
        FunctionID constructor = program.at(member).constructor;
        return constructor && !escapedFunctions.get(constructor.index());
    });
    m_familiesThatHaveEscaped = BitVector(families.size());
    m_familiesWhoseConstructorsHaveEscaped = BitVector(families.size());
    for (uint32_t family = 0; family < families.size(); ++family) {
        if (!familiesWithLayoutThatHasNotEscaped.quickGet(family))
            m_familiesThatHaveEscaped.quickSet(family);
        if (!familiesWithConstructorThatHasNotEscaped.quickGet(family))
            m_familiesWhoseConstructorsHaveEscaped.quickSet(family);
    }
    m_lostFamiliesThatHaveEscaped = BitVector(program.layouts.size());
    for (size_t layout : lostLayouts) {
        if (unitedLayouts.representativeOf(static_cast<uint32_t>(layout)) != layout)
            continue;
        bool everyMemberHasEscaped = true;
        unitedLayouts.forEachMemberOfFamilyOf(static_cast<uint32_t>(layout), [&](uint32_t member) {
            everyMemberHasEscaped &= escapedLayouts.get(member);
        });
        if (!everyMemberHasEscaped)
            continue;
        unitedLayouts.forEachMemberOfFamilyOf(static_cast<uint32_t>(layout), [&](uint32_t member) {
            m_lostFamiliesThatHaveEscaped.quickSet(member);
        });
    }
    m_sitesThatHaveEscaped = BitVector(m_sites.size());
    for (uint32_t site = 0; site < m_sites.size(); ++site) {
        if (hasEscaped(SiteID(site)))
            m_sitesThatHaveEscaped.quickSet(site);
    }

    m_typesAreSettled = true;
}

LinkType LinkFlow::settled(LinkType type) const
{
    ASSERT(type.hasUnknownObjects);

    bool functionsHaveEscaped = false;
    switch (type.functions) {
    case LinkType::Functions::None:
        break;
    case LinkType::Functions::Closure:
        functionsHaveEscaped = escapedFunctions.get(type.function.index());
        break;
    case LinkType::Functions::Constructors:
        functionsHaveEscaped = m_familiesWhoseConstructorsHaveEscaped.quickGet(type.constructorsOf.index());
        break;
    case LinkType::Functions::Set:
        functionsHaveEscaped = std::ranges::all_of(functionsOfSet(type), [&](FunctionID member) {
            return escapedFunctions.get(member.index());
        });
        break;
    case LinkType::Functions::Lost:
        functionsHaveEscaped = type.function && m_familiesOfFunctionsAtUnseen.get(unitedFunctions.representativeWithoutShortening(type.function.index()));
        break;
    }
    if (functionsHaveEscaped) {
        type.functions = LinkType::Functions::None;
        type.function = { };
        type.constructorsOf = { };
    }

    if (type.classes != LinkType::Classes::None && m_sitesThatHaveEscaped.quickGet(type.site.index()))
        type.dropInstances();
    return type;
}

static constexpr size_t fewestStepsForSeveralThreadsInLinkFlow = 512;

void LinkFlow::runOnLanes(const Invocable<void(unsigned lane)> auto& work)
{
    forEachIndexInParallel(loops, static_cast<unsigned>(m_lanes.size()), [&](unsigned lane) {
        s_laneAtWork = m_lanes[lane].get();
        work(lane);
        s_laneAtWork = nullptr;
    });
}

bool LinkFlow::severalThreadsPayFor(size_t numberOfSteps) const
{
    return (numberOfSteps >= fewestStepsForSeveralThreadsInLinkFlow || m_order == OrderOfWorklist::Scattered) && !m_breachesOfClosure;
}

ALWAYS_INLINE_INTO_OTHER_FILES void LinkFlow::send(Lane& sender, FlowNodeID to, const LinkType& type, PropertyKeyID symbol)
{
    if (to == unseen()) {
        if (!type.tellsUnseenNothing())
            noteReachesUnseen(type);
        return;
    }
    LinkType held = headOfNode(to.index());
    bool namesNothing = type.objects == LinkType::Objects::None && type.functions == LinkType::Functions::None && type.classes == LinkType::Classes::None && !type.builtin
        && (!(type.primitives & LinkType::oneSymbol) || (held.directPrimitives() & Primitive::Symbol && !symbol) || !(type.directPrimitives() & LinkType::oneSymbol));
    if (namesNothing && bringsNoPrimitiveInLinkFlow(type.primitives, held.primitives) && type.raisesNoFlagOver(held) && (!type.holdsLostBuiltins || held.holdsLostBuiltins || isNodeOfLibrary(to.index())) && bringsNothingToShadowsInLinkFlow(type, held))
        return;
    sender.boxes[laneIndexOf(to)].constructAndAppend(to, symbol, type);
    sender.hasFilledBoxes = true;
}

void LinkFlow::sendFromLane(unsigned index)
{
    Lane& lane = *m_lanes[index];
    if (lane.worklist.isEmpty())
        return;

    static constexpr size_t lengthOfStretch = 16;
    std::array<Edge, lengthOfStretch> edges;
    while (!lane.worklist.isEmpty()) {
        auto [node, before] = takeNode(lane);
        ++lane.numberOfNodesTakenOffWorklist;
        m_isOnWorklist.quickClear(node.index());
        const ListsOfNode& lists = m_listsOfNodes[node.index()];
        if (lists.firstObserver != UINT32_MAX && !m_isToBeTold.quickSet(node.index()))
            lane.nodesToTell.constructAndAppend(node, before);
        uint32_t next = lists.firstEdge;
        if (next == UINT32_MAX || isNeverAt(node.index()))
            continue;
        LinkType type = typeOfNode(node.index());
        PropertyKeyID symbol = LinkType::holdsOneSymbol(type.directPrimitives()) ? symbolNotedAt(node) : PropertyKeyID();
        while (next != UINT32_MAX) {
            Edge* endOfEdges = edges.data();
            Edge* const endOfStretch = endOfEdges + lengthOfStretch;
            while (next != UINT32_MAX && endOfEdges != endOfStretch) {
                Edge& edge = *endOfEdges++;
                edge = lane.edges[next];
                m_nodes.prefetch(edge.to.index());
                next = edge.next;
            }
            for (const Edge* edge = edges.data(); edge != endOfEdges; ++edge) {
                if (!edge->passes)
                    send(lane, edge->to, type, symbol);
                else if (LinkType passed = passedByTestOfEdge(type, edge->passes); !passed.isNever())
                    send(lane, edge->to, passed, symbol);
            }
        }
    }
}

void LinkFlow::foldObjects(LinkType& into, std::span<const LinkType> met) const
{
    using Objects = LinkType::Objects;
    bool someStandOutside = standsOutsideFamilies(into);
    for (const LinkType& other : met)
        someStandOutside |= standsOutsideFamilies(other);
    if (someStandOutside) {
        for (const LinkType& other : met)
            joinObjects(into, other);
        return;
    }

    Vector<LayoutID, 16> members;
    bool someIsSet = false;
    bool allNameTheirLayouts = true;
    auto gather = [&](const LinkType& type) {
        switch (type.objects) {
        case Objects::None:
            return;
        case Objects::Layout:
            members.append(type.layout);
            return;
        case Objects::Set:
            someIsSet = true;
            members.append(layoutsOfSet(type));
            return;
        case Objects::Prototype:
        case Objects::Prototypes:
        case Objects::Family:
        case Objects::Lost:
            allNameTheirLayouts = false;
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    gather(into);
    for (const LinkType& other : met)
        gather(other);
    if (allNameTheirLayouts) {
        std::ranges::sort(members, { }, &LayoutID::index);
        members.shrink(members.size() - std::ranges::unique(members).size());
        if (into.objects == Objects::Set && members.size() == layoutsOfSet(into).size())
            return;
        if (members.size() <= SetsOfLayoutsInLinkFlow::mostMembers) {
            LinkType named = typeOfLayouts(members.span());
            into.objects = named.objects;
            into.layout = named.layout;
            into.family = { };
            return;
        }
    }
    if (!someIsSet) {
        foldObjectsTakenApart(into, met);
        return;
    }

    Vector<LinkType, 16> apart;
    auto takeApart = [&](const LinkType& type) {
        if (type.objects != Objects::Set) {
            apart.append(type);
            return;
        }
        for (LayoutID member : layoutsOfSet(type))
            apart.append(typeOfLayoutInLinkFlow(member));
    };
    takeApart(into);
    for (const LinkType& other : met)
        takeApart(other);
    LinkType folded = apart[0];
    foldObjectsTakenApart(folded, apart.span().subspan(1));
    into.objects = folded.objects;
    into.layout = folded.layout;
    into.family = folded.family;
}

void LinkFlow::foldObjectsTakenApart(LinkType& into, std::span<const LinkType> met) const
{
    using Objects = LinkType::Objects;

    auto familyHoldingBoth = [&](const LinkType& one, const LinkType& other) -> FamilyID {
        if (one.objects == Objects::Layout && other.objects == Objects::Layout)
            return families.nearestCommonFamily(one.layout, other.layout);
        if (one.objects == Objects::Layout)
            return families.nearestCommonFamily(other.family, one.layout);
        if (other.objects == Objects::Layout)
            return families.nearestCommonFamily(one.family, other.layout);
        return families.nearestCommonFamily(one.family, other.family);
    };
    bool oneFamilyHoldsAll = into.objects != Objects::Lost;
    LinkType common = into;
    for (const LinkType& other : met) {
        if (!oneFamilyHoldsAll)
            break;
        if (other.objects == Objects::Lost) {
            oneFamilyHoldsAll = !other.layout;
            continue;
        }
        if (other.objects == common.objects && other.layout == common.layout && other.family == common.family)
            continue;
        FamilyID family = familyHoldingBoth(common, other);
        oneFamilyHoldsAll = !!family;
        if (family)
            common = typeOfFamilyInLinkFlow(family);
    }
    auto take = [&](const LinkType& from) {
        into.objects = from.objects;
        into.layout = from.layout;
        into.family = from.family;
    };
    if (oneFamilyHoldsAll) {
        if (common.objects == Objects::Family) {
            if (into.objects == Objects::Layout)
                noteMetInFamily(into.layout);
            for (const LinkType& other : met) {
                if (other.objects == Objects::Layout)
                    noteMetInFamily(other.layout);
            }
        }
        take(common);
        return;
    }

    LayoutID first;
    auto lose = [&](const LinkType& type) {
        if (type.objects == Objects::Layout)
            deferLoss(type.layout);
        else if (type.objects == Objects::Family)
            deferLoss(type.family);
        LayoutID member = memberStandingFor(type);
        if (!member)
            return;
        if (first)
            deferUnion(first, member);
        else
            first = member;
    };
    lose(into);
    for (const LinkType& other : met)
        lose(other);
    into.objects = Objects::Lost;
    into.layout = first;
    into.family = { };
}

void LinkFlow::foldFunctions(LinkType& into, std::span<const LinkType> met) const
{
    using Functions = LinkType::Functions;
    Vector<FunctionID, 16> members;
    bool someIsSet = false;
    bool allAreNamed = true;
    auto gather = [&](const LinkType& type) {
        switch (type.functions) {
        case Functions::None:
            return;
        case Functions::Closure:
            members.append(type.function);
            return;
        case Functions::Set:
            someIsSet = true;
            members.append(functionsOfSet(type));
            return;
        case Functions::Constructors:
        case Functions::Lost:
            allAreNamed = false;
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    gather(into);
    for (const LinkType& other : met)
        gather(other);
    if (allAreNamed) {
        std::ranges::sort(members, { }, &FunctionID::index);
        members.shrink(members.size() - std::ranges::unique(members).size());
        if (into.functions == Functions::Set && members.size() == functionsOfSet(into).size())
            return;
        if (members.size() <= SetsOfFunctionsInLinkFlow::mostMembers && mayStandInOneSet(members.span())) {
            LinkType named = typeOfFunctions(members.span());
            into.functions = named.functions;
            into.function = named.function;
            into.constructorsOf = { };
            return;
        }
    }
    if (!someIsSet) {
        foldFunctionsTakenApart(into, met);
        return;
    }

    Vector<LinkType, 16> apart;
    auto takeApart = [&](const LinkType& type) {
        if (type.functions != Functions::Set) {
            apart.append(type);
            return;
        }
        for (FunctionID member : functionsOfSet(type))
            apart.append(typeOfClosureInLinkFlow(member));
    };
    takeApart(into);
    for (const LinkType& other : met)
        takeApart(other);
    LinkType folded = apart[0];
    foldFunctionsTakenApart(folded, apart.span().subspan(1));
    into.functions = folded.functions;
    into.function = folded.function;
    into.constructorsOf = folded.constructorsOf;
}

void LinkFlow::foldFunctionsTakenApart(LinkType& into, std::span<const LinkType> met) const
{
    using Functions = LinkType::Functions;
    auto familyHoldingBoth = [&](const LinkType& one, const LinkType& other) -> FamilyID {
        if (one.functions == Functions::Constructors && other.functions == Functions::Constructors)
            return families.nearestCommonFamily(one.constructorsOf, other.constructorsOf);
        LayoutID classOfOne = one.functions == Functions::Closure ? classOfConstructor(one.function) : LayoutID();
        LayoutID classOfOther = other.functions == Functions::Closure ? classOfConstructor(other.function) : LayoutID();
        if (one.functions == Functions::Constructors)
            return classOfOther ? families.nearestCommonFamily(one.constructorsOf, classOfOther) : FamilyID();
        if (other.functions == Functions::Constructors)
            return classOfOne ? families.nearestCommonFamily(other.constructorsOf, classOfOne) : FamilyID();
        return classOfOne && classOfOther ? families.nearestCommonFamily(classOfOne, classOfOther) : FamilyID();
    };
    bool oneFamilyHoldsAll = into.functions != Functions::Lost;
    LinkType common = into;
    for (const LinkType& other : met) {
        if (!oneFamilyHoldsAll)
            break;
        if (other.functions == common.functions && other.function == common.function && other.constructorsOf == common.constructorsOf)
            continue;
        FamilyID family = other.functions == Functions::Lost ? FamilyID() : familyHoldingBoth(common, other);
        oneFamilyHoldsAll = !!family;
        if (family) {
            common.functions = Functions::Constructors;
            common.function = { };
            common.constructorsOf = family;
        }
    }
    if (oneFamilyHoldsAll) {
        into.functions = common.functions;
        into.function = common.function;
        into.constructorsOf = common.constructorsOf;
        return;
    }
    FunctionID named;
    auto name = [&](FunctionID member) {
        if (named)
            deferUnion(named, member);
        else
            named = member;
    };
    auto lose = [&](const LinkType& type) {
        switch (type.functions) {
        case Functions::None:
            return;
        case Functions::Closure:
            deferLoss(type.function);
            name(type.function);
            return;
        case Functions::Constructors: {
            deferLossOfConstructors(type.constructorsOf);
            FunctionID first;
            for (LayoutID layout : program.types.listOf(families.layoutsOf(type.constructorsOf))) {
                if ((first = program.at(layout).constructor))
                    break;
            }
            if (first)
                name(first);
            return;
        }
        case Functions::Lost:
            if (type.function)
                name(type.function);
            return;
        case Functions::Set:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    lose(into);
    for (const LinkType& other : met)
        lose(other);
    into.functions = Functions::Lost;
    into.function = named;
    into.constructorsOf = { };
}

void LinkFlow::receiveInLane(unsigned index)
{
    Lane& lane = *m_lanes[index];
    for (auto& sender : m_lanes) {
        if (!(sender->lanesOfFilledBoxesInThisPass >> index & 1))
            continue;
        Vector<ArrivalHeldBack>& box = sender->boxes[index];
        static constexpr ptrdiff_t ahead = 12;
        const ArrivalHeldBack* const endOfBox = box.end();
        for (const ArrivalHeldBack* arrival = box.begin(); arrival != endOfBox; ++arrival) {
            if (endOfBox - arrival > ahead)
                m_nodes.prefetch(arrival[ahead].node.index());
            receive(arrival->node, arrival->type, arrival->symbol);
        }
        box.shrink(0);
    }
    if (lane.meetings.isEmpty())
        return;

    auto orderOf = [](const std::pair<FlowNodeID, LinkType>& meeting) {
        const LinkType& type = meeting.second;
        return std::tuple(meeting.first.index(), static_cast<uint8_t>(type.objects), type.layout, type.family, static_cast<uint8_t>(type.functions), type.function, type.constructorsOf);
    };
    std::ranges::sort(lane.meetings, { }, orderOf);
    Vector<LinkType, 8> objects;
    Vector<LinkType, 8> functions;
    for (size_t first = 0, end = 0; first < lane.meetings.size(); first = end) {
        FlowNodeID node = lane.meetings[first].first;
        objects.shrink(0);
        functions.shrink(0);
        unsigned numberOfSetsOfLayouts = 0;
        for (end = first; end < lane.meetings.size() && lane.meetings[end].first == node; ++end) {
            const LinkType& met = lane.meetings[end].second;
            if (met.objects != LinkType::Objects::None)
                objects.append(met);
            if (met.functions != LinkType::Functions::None)
                functions.append(met);
            numberOfSetsOfLayouts += met.objects == LinkType::Objects::Set;
        }
        if (numberOfSetsOfLayouts > 1) {
            auto firstSet = std::ranges::find_if(objects, [](const LinkType& met) {
                return met.objects == LinkType::Objects::Set;
            });
            std::ranges::sort(firstSet, firstSet + numberOfSetsOfLayouts, [&](const LinkType& one, const LinkType& other) {
                return std::ranges::lexicographical_compare(layoutsOfSet(one), layoutsOfSet(other));
            });
        }
        LinkType before = typeOfNode(node.index());
        LinkType after = before;
        if (!objects.isEmpty())
            foldObjects(after, objects.span());
        if (!functions.isEmpty())
            foldFunctions(after, functions.span());
        if (after.hasUnknownObjects && after.builtin) {
            bool waits = !(m_marksThatAreUsed & LinkType::BuiltinsLostAtJoins);
            if (!waits || lightFormOf(after.builtin))
                noteLossOf(after.builtin);
            else
                (s_laneAtWork ? s_laneAtWork->builtinsLostAtJoins : m_builtinsLostAtJoins).append(after.builtin);
            if (waits)
                after.marks |= LinkType::BuiltinsLostAtJoins;
            if (!isNodeOfLibrary(node.index()))
                after.holdsLostBuiltins |= loseInSight(after.builtin);
            after.builtin = { };
            after.isUnknownEscapeOnly = false;
        }
        if (after.hasUnknownObjects)
            after.setOfClasses = 0;
        if (after == before && after.hasSameShadowsAs(before))
            continue;
        if (m_breachesOfClosure) [[unlikely]]
            noteBreachOfClosure({ .node = node, .before = before, .after = after });
        setTypeOfNode(node.index(), after, before);
        if (!m_isOnWorklist.quickSet(node.index())) {
            lane.worklist.append(node);
            lane.typesBeforeClimb.append(before);
        }
    }
    lane.meetings.shrink(0);
}

void LinkFlow::goAlongEdges()
{
    bool wasSettling = std::exchange(m_isSettling, true);
    size_t mostSent = 0;
    while (true) {
        size_t numberSent = 0;
        uint32_t lanesSentTo = 0;
        for (auto& sender : m_lanes) {
            sender->lanesOfFilledBoxesInThisPass = std::exchange(sender->hasFilledBoxes, false) ? lanesOfFilledListsIn(sender->boxes, numberSent) : 0;
            lanesSentTo |= sender->lanesOfFilledBoxesInThisPass;
        }
        mostSent = std::max(mostSent, numberSent);
        if (numberSent) {
            m_setsMeetingsAside = true;
            if (severalThreadsPayFor(numberSent)) {
                m_references.makeRoomFor(2 * numberSent);
                runOnLanes([&](unsigned lane) {
                    receiveInLane(lane);
                });
                gatherWhatLanesNoted();
            } else {
                forEachLaneIn(lanesSentTo, [&](unsigned lane) {
                    receiveInLane(lane);
                });
            }
            m_setsMeetingsAside = false;
        }

        size_t numberThatClimbed = 0;
        for (auto& lane : m_lanes)
            numberThatClimbed += lane->worklist.size() - lane->firstInWorklist;
        if (!numberThatClimbed)
            break;
        if (severalThreadsPayFor(numberThatClimbed)) {
            runOnLanes([&](unsigned lane) {
                sendFromLane(lane);
            });
            gatherWhatLanesNoted();
        } else {
            for (unsigned lane = 0; lane < m_lanes.size(); ++lane)
                sendFromLane(lane);
        }
    }
    m_isSettling = wasSettling;
    freeIdleRoomOfLanes(mostSent);
}

void LinkFlow::freeIdleRoomOfLanes(size_t mostSent)
{
    static constexpr size_t muchMore = 32;
    static constexpr unsigned manyPasses = 32;
    if (mostSent * muchMore >= m_mostSentSinceFree) {
        m_mostSentSinceFree = std::max(m_mostSentSinceFree, mostSent);
        m_quietPasses = 0;
        return;
    }
    if (++m_quietPasses < manyPasses)
        return;
    m_quietPasses = 0;
    m_mostSentSinceFree = mostSent;
    auto freeRoom = [](auto& list) {
        if (list.isEmpty())
            list.clear();
    };
    for (auto& lane : m_lanes) {
        freeRoom(lane->worklist);
        freeRoom(lane->typesBeforeClimb);
        std::ranges::for_each(lane->boxes, freeRoom);
        std::ranges::for_each(lane->edgesLaid, freeRoom);
        std::ranges::for_each(lane->observersPut, freeRoom);
        std::ranges::for_each(lane->tells, freeRoom);
    }
}

void LinkFlow::gatherWhatLanesNoted()
{
    for (auto& lane : m_lanes) {
        for (const Deferred& deferred : lane->deferred) {
            switch (deferred.kind) {
            case Deferred::Kind::LayoutIsLost:
                deferLoss(LayoutID(deferred.number));
                break;
            case Deferred::Kind::FamilyIsLost:
                deferLoss(FamilyID(deferred.number));
                break;
            case Deferred::Kind::LayoutsAreUnited:
                deferUnion(LayoutID(deferred.number), LayoutID(deferred.other));
                break;
            case Deferred::Kind::FunctionIsLost:
                deferLoss(FunctionID(deferred.number));
                break;
            case Deferred::Kind::FunctionsAreUnited:
                deferUnion(FunctionID(deferred.number), FunctionID(deferred.other));
                break;
            case Deferred::Kind::FamilyOfFunctionsEscapes:
                letFamilyOfLostFunctionsEscape(FunctionID(deferred.number));
                break;
            case Deferred::Kind::ConstructorsAreLost:
                deferLossOfConstructors(FamilyID(deferred.number));
                break;
            case Deferred::Kind::SitesAreUnited:
                deferUnion(SiteID(deferred.number), deferred.type.site);
                break;
            case Deferred::Kind::LayoutEscapes:
                deferEscape(LayoutID(deferred.number));
                break;
            case Deferred::Kind::FunctionEscapes:
                deferEscape(FunctionID(deferred.number));
                break;
            case Deferred::Kind::SiteEscapes:
                deferEscape(SiteID(deferred.number));
                break;
            case Deferred::Kind::LostFamilyEscapes:
            case Deferred::Kind::PoolsOfLostFunctionsAreUnited:
            case Deferred::Kind::ConstructorsEscape:
            case Deferred::Kind::PrototypesEscape:
            case Deferred::Kind::TypeEscapes:
                RELEASE_ASSERT_NOT_REACHED();
            }
        }
        lane->deferred.shrink(0);
        m_builtinsLost.appendVector(lane->builtinsLost);
        lane->builtinsLost.shrink(0);
        m_builtinsLostAtJoins.appendVector(lane->builtinsLostAtJoins);
        lane->builtinsLostAtJoins.shrink(0);
        m_builtinsLostInSight.appendVector(lane->builtinsLostInSight);
        lane->builtinsLostInSight.shrink(0);
        for (LayoutID layout : lane->layoutsMetInFamilies)
            noteMetInFamily(layout);
        lane->layoutsMetInFamilies.shrink(0);
        for (const LinkType& type : lane->typesAtUnseen)
            noteReachesUnseen(type);
        lane->typesAtUnseen.shrink(0);
    }
}

void LinkFlow::applyDeferredUpdates(unsigned index)
{
    Lane& lane = *m_lanes[index];

    for (auto& holder : m_lanes) {
        if (!(holder->lanesOfObserversPutInThisPass >> index & 1))
            continue;
        static constexpr size_t ahead = 12;
        Vector<ObserverPut>& puts = holder->observersPut[index];
        for (size_t place = 0; place < puts.size(); ++place) {
            if (place + ahead < puts.size())
                m_listsOfNodes.prefetch(puts[place + ahead].node.index());
            putObserver(lane, puts[place].node, puts[place].observer);
        }
        puts.shrink(0);
    }
    for (auto& holder : m_lanes) {
        if (!(holder->lanesOfEdgesLaidInThisPass >> index & 1))
            continue;
        static constexpr size_t ahead = 12;
        Vector<EdgeLaid>& edgesLaid = holder->edgesLaid[index];
        for (size_t place = 0; place < edgesLaid.size(); ++place) {
            if (place + ahead < edgesLaid.size()) {
                const EdgeLaid& next = edgesLaid[place + ahead];
                m_listsOfNodes.prefetch(next.from.index());
                m_nodes.prefetch(next.from.index());
                m_nodes.prefetch(next.to.index());
            }
            const EdgeLaid& laid = edgesLaid[place];
            if (!layEdge(lane, laid.from, laid.to, laid.passes))
                continue;
            if (isNeverAt(laid.from.index()))
                continue;
            LinkType type = typeOfNode(laid.from.index());
            PropertyKeyID symbol = LinkType::holdsOneSymbol(type.directPrimitives()) ? symbolNotedAt(laid.from) : PropertyKeyID();
            if (!laid.passes)
                send(lane, laid.to, type, symbol);
            else if (LinkType passed = passedByTestOfEdge(type, laid.passes); !passed.isNever())
                send(lane, laid.to, passed, symbol);
        }
        if (edgesLaid.capacity() > 1 << 14)
            edgesLaid.clear();
        else
            edgesLaid.shrink(0);
    }

}

void LinkFlow::wakeDeferredUpdateWaiters()
{
    size_t deferredUpdateCount = 0;
    uint32_t lanesWithDeferredUpdates = 0;
    for (auto& holder : m_lanes) {
        holder->lanesOfObserversPutInThisPass = std::exchange(holder->hasObserversPut, false) ? lanesOfFilledListsIn(holder->observersPut, deferredUpdateCount) : 0;
        holder->lanesOfEdgesLaidInThisPass = holder->hasEdgesLaid ? lanesOfFilledListsIn(holder->edgesLaid, deferredUpdateCount) : 0;
        lanesWithDeferredUpdates |= holder->lanesOfObserversPutInThisPass | holder->lanesOfEdgesLaidInThisPass;
    }
    if (!deferredUpdateCount)
        return;
    auto wakeIn = [&](unsigned index) {
        Lane& lane = *m_lanes[index];
        for (auto& holder : m_lanes) {
            if (!(holder->lanesOfObserversPutInThisPass >> index & 1))
                continue;
            Vector<ObserverPut>& puts = holder->observersPut[index];
            for (const ObserverPut& put : puts)
                putObserver(lane, put.node, put.observer);
            puts.shrink(0);
        }
        for (auto& holder : m_lanes) {
            if (!(holder->lanesOfEdgesLaidInThisPass >> index & 1))
                continue;
            for (const EdgeLaid& laid : holder->edgesLaid[index]) {
                if (m_accessorsWaitAt.quickGet(laid.from.index())) [[unlikely]]
                    wakeAccessorsThatWaitAt(lane, laid.from);
            }
        }
    };
    if (severalThreadsPayFor(deferredUpdateCount))
        runOnLanes(wakeIn);
    else
        forEachLaneIn(lanesWithDeferredUpdates, wakeIn);
    takeOverNewObserversOfLanes();
}

void LinkFlow::applyDeferredUpdates()
{
    bool wasSettling = std::exchange(m_isSettling, true);
    size_t deferredUpdateCount = 0;
    uint32_t lanesWithDeferredUpdates = 0;
    for (auto& holder : m_lanes) {
        holder->lanesOfObserversPutInThisPass = std::exchange(holder->hasObserversPut, false) ? lanesOfFilledListsIn(holder->observersPut, deferredUpdateCount) : 0;
        holder->lanesOfEdgesLaidInThisPass = std::exchange(holder->hasEdgesLaid, false) ? lanesOfFilledListsIn(holder->edgesLaid, deferredUpdateCount) : 0;
        lanesWithDeferredUpdates |= holder->lanesOfObserversPutInThisPass | holder->lanesOfEdgesLaidInThisPass;
    }
    if (severalThreadsPayFor(deferredUpdateCount)) {
        runOnLanes([&](unsigned lane) {
            applyDeferredUpdates(lane);
        });
        gatherWhatLanesNoted();
    } else {
        forEachLaneIn(lanesWithDeferredUpdates, [&](unsigned lane) {
            applyDeferredUpdates(lane);
        });
    }
    if (!m_severalThreadsTell)
        takeOverNewObserversOfLanes();
    m_isSettling = wasSettling;
}

void LinkFlow::sortTellsOfLane(unsigned index)
{
    Lane& lane = *m_lanes[index];
    Vector<uint64_t>& numbers = lane.numbersOfNodesToTell;
    switch (m_order) {
    case OrderOfWorklist::NewestFirst:
        numbers.shrink(0);
        numbers.reserveCapacity(lane.nodesToTell.size());
        for (const auto& [node, before] : lane.nodesToTell)
            numbers.append(static_cast<uint64_t>(node.index()) << 32 | static_cast<uint32_t>(numbers.size()));
        sortNumbersInLinkFlow(numbers, lane.roomToSort, m_nodes.size());
        break;
    case OrderOfWorklist::OldestFirst:
        std::ranges::reverse(lane.nodesToTell);
        break;
    case OrderOfWorklist::Scattered:
        for (size_t place = lane.nodesToTell.size(); place > 1; --place) {
            lane.scatteredNumber ^= lane.scatteredNumber << 13;
            lane.scatteredNumber ^= lane.scatteredNumber >> 7;
            lane.scatteredNumber ^= lane.scatteredNumber << 17;
            std::swap(lane.nodesToTell[place - 1], lane.nodesToTell[lane.scatteredNumber % place]);
        }
        break;
    }
    auto hand = [&](const FlowObserver& observer, FlowNodeID node, uint32_t placeOfTypeBefore) {
        if (isToldByOneThread(observer.kind))
            lane.tellsForOneThread.constructAndAppend(observer, node, placeOfTypeBefore);
        else
            lane.tells[laneOfObserversOf(observer.function)].constructAndAppend(observer, node, placeOfTypeBefore);
    };
    for (uint32_t turn = 0; turn < lane.nodesToTell.size(); ++turn) {
        uint32_t place = m_order == OrderOfWorklist::NewestFirst ? static_cast<uint32_t>(numbers[turn]) : turn;
        FlowNodeID node = lane.nodesToTell[place].first;
        m_isToBeTold.quickClear(node.index());
        for (uint32_t listed = m_listsOfNodes[node.index()].firstObserver; listed != UINT32_MAX;) {
            FlowObserver observer = lane.observers[listed].observer();
            uint32_t next = lane.observers[listed].next();
            hand(observer, node, place);
            listed = next;
        }
    }
    for (auto& [observer, node] : lane.newObservers)
        hand(observer, node, UINT32_MAX);
    lane.newObservers.shrink(0);
}

void LinkFlow::tellInLane(unsigned index)
{
    for (auto& sender : m_lanes) {
        Vector<Lane::Tell>& tells = sender->tells[index];
        for (size_t place = 0; place < tells.size(); ++place) {
            if (place + 4 < tells.size()) {
                FunctionID function = tells[place + 4].observer.function;
                const ThinSummary& summary = summaries.at(function);
                __builtin_prefetch(&summary.calls);
                __builtin_prefetch(&summary.accesses);
                __builtin_prefetch(&m_nodesOfFunctions[function.index()]);
                __builtin_prefetch(&firstCallOfFunction[function.index()]);
            }
            const Lane::Tell& told = tells[place];
            [[clang::always_inline]] tell(told.observer, typeAt(told.node), told.placeOfTypeBefore == UINT32_MAX ? s_nothing : sender->nodesToTell[told.placeOfTypeBefore].second);
        }
        tells.shrink(0);
    }
    resolveNotifiedCalls(index);
}

bool LinkFlow::listsSourceOfObjectsBehindPass(FlowNodeID node, LayoutID layout)
{
    if (!m_severalThreadsTell)
        return false;
    s_laneAtWork->sourcesOfObjects.append({ node, layout });
    return true;
}

bool LinkFlow::tellsBuiltinsInReachBehindPass(FunctionID function)
{
    if (!m_severalThreadsTell)
        return false;
    s_laneAtWork->functionsToTellBuiltinsInReach.append(function);
    return true;
}

void LinkFlow::takeOverWhatLanesDidWhileTelling()
{
    if (m_numberOfNodesMade.load() > m_nodes.size()) {
        m_nodes.grow(loops, m_numberOfNodesMade.load());
        m_listsOfNodes.grow(loops, m_numberOfNodesMade.load());
        if (m_nodes.size() > m_isOnWorklist.size()) {
            m_isOnWorklist.ensureSize(m_nodes.size() + m_nodes.size() / 4 + 64);
            m_isToBeTold.ensureSize(m_isOnWorklist.size());
            m_flowsIntoUnseen.ensureSize(m_isOnWorklist.size());
            m_accessorsWaitAt.ensureSize(m_isOnWorklist.size());
            m_hasEdgesInSet.ensureSize(m_isOnWorklist.size());
        }
    }
    m_familiesOfSites.growTo(static_cast<uint32_t>(m_sites.size()));
    listContentsThatLanesMade();
    gatherWhatLanesNoted();
    for (auto& lane : m_lanes) {
        for (auto [node, layout] : lane->sourcesOfObjects)
            listSourceOfObjects(node, layout);
        lane->sourcesOfObjects.shrink(0);
        if (!lane->functionsToTellBuiltinsInReach.isEmpty())
            builtinsInReach().noteFindingsIn(lane->functionsToTellBuiltinsInReach.span());
        lane->functionsToTellBuiltinsInReach.shrink(0);
        for (const Lane::NoteTaken& taken : lane->notes)
            applyNote(taken.note, taken.number, taken.more);
        lane->notes.shrink(0);
        for (const auto& [layout, laid] : lane->accessorsLaid)
            noteAccessorLaid(layout, laid);
        lane->accessorsLaid.shrink(0);
    }
    for (auto& lane : m_lanes)
        m_numberOfObserversTold += std::exchange(lane->numberOfObserversTold, 0);
    takeOverNewObserversOfLanes();
    Vector<FunctionID> functionsToReach;
    for (auto& lane : m_lanes) {
        functionsToReach.appendVector(lane->functionsToReach);
        lane->functionsToReach.shrink(0);
    }
    reachAll(functionsToReach.span());
}

void LinkFlow::takeOverNewObserversOfLanes()
{
    m_numberOfNewObserversLeftWithLanes = 0;
    for (auto& lane : m_lanes)
        m_numberOfNewObserversLeftWithLanes += lane->newObservers.size();
    if (severalThreadsPayFor(m_numberOfNewObserversLeftWithLanes))
        return;
    m_numberOfNewObserversLeftWithLanes = 0;
    for (auto& lane : m_lanes) {
        m_newObservers.appendVector(lane->newObservers);
        lane->newObservers.shrink(0);
    }
}

void LinkFlow::runWithDeferredUpdates(const ScopedLambda<void()>& work)
{
    RELEASE_ASSERT(!m_isDeferringUpdates);
    m_isDeferringUpdates = true;
    work();
    m_isDeferringUpdates = false;
    applyDeferredUpdates();
}

bool LinkFlow::lanesMayWorkForReach(size_t numberOfSteps) const
{
    static constexpr size_t fewestStepsForSeveralThreads = 256;
    return m_isDeferringUpdates && !s_laneAtWork && !m_breachesOfClosure && (numberOfSteps >= fewestStepsForSeveralThreads || m_order == OrderOfWorklist::Scattered);
}

void LinkFlow::workOnLanesForReach(const Invocable<void(unsigned lane)> auto& work)
{
    stateOfLinkCalls();
    prepareAccessesForSeveralThreads();
    prepareOperatorsForSeveralThreads();
    m_numberOfNodesMade = m_nodes.size();
    for (auto& lane : m_lanes) {
        lane->given[static_cast<size_t>(MadeOnDemand::TakenForConstant)].clear();
        lane->given[static_cast<size_t>(MadeOnDemand::ContentsOfFamily)].clear();
    }
    forgetWhatSomeLaneWasGiven(MadeOnDemand::TakenForConstant);
    bool toldObservers = std::exchange(m_tellsObservers, true);
    m_severalThreadsTell = true;
    runOnLanes(work);
    layTiesThatLanesLeft();
    m_severalThreadsTell = false;
    m_tellsObservers = toldObservers;
    takeOverWhatLanesDidWhileTelling();
}

bool LinkFlow::bringAliveOnLanes(std::span<const FunctionID> functions, const ScopedLambda<void(FunctionID)>& bringAlive)
{
    if (!lanesMayWorkForReach(functions.size()))
        return false;
    for (FunctionID function : functions)
        m_lanes[laneOfObserversOf(function)]->functionsToBringAlive.append(function);
    workOnLanesForReach([&](unsigned index) {
        Lane& lane = *m_lanes[index];
        for (FunctionID function : lane.functionsToBringAlive)
            bringAlive(function);
        lane.functionsToBringAlive.shrink(0);
    });
    return true;
}

bool LinkFlow::takeStepsOfReachOnLanes(size_t numberOfSteps, const ScopedLambda<void(unsigned lane)>& takeSteps)
{
    if (!lanesMayWorkForReach(numberOfSteps))
        return false;
    m_lanesTakeStepsOfReach = true;
    workOnLanesForReach([&](unsigned lane) {
        takeSteps(lane);
    });
    m_lanesTakeStepsOfReach = false;
    return true;
}

void LinkFlow::tellOnLanes()
{
    stateOfLinkReach();
    stateOfLinkCalls();
    prepareAccessesForSeveralThreads();
    prepareOperatorsForSeveralThreads();

    m_lanes[0]->newObservers.appendVector(m_newObservers);
    m_newObservers.shrink(0);
    m_firstOfNewObservers = 0;
    m_numberOfNewObserversLeftWithLanes = 0;
    runOnLanes([&](unsigned lane) {
        sortTellsOfLane(lane);
    });
    m_numberOfNodesMade = m_nodes.size();
    for (auto& lane : m_lanes) {
        lane->given[static_cast<size_t>(MadeOnDemand::TakenForConstant)].clear();
        lane->given[static_cast<size_t>(MadeOnDemand::ContentsOfFamily)].clear();
    }
    forgetWhatSomeLaneWasGiven(MadeOnDemand::TakenForConstant);
    m_severalThreadsTell = true;
    runOnLanes([&](unsigned lane) {
        tellInLane(lane);
    });
    layTiesThatLanesLeft();
    m_severalThreadsTell = false;
    Vector<Vector<Lane::Tell>> tellsForOneThread;
    Vector<Vector<std::pair<FlowNodeID, LinkType>>> nodesTold;
    for (auto& lane : m_lanes) {
        tellsForOneThread.append(std::exchange(lane->tellsForOneThread, { }));
        nodesTold.append(std::exchange(lane->nodesToTell, { }));
    }
    takeOverWhatLanesDidWhileTelling();
    for (unsigned lane = 0; lane < m_lanes.size(); ++lane) {
        for (const Lane::Tell& told : tellsForOneThread[lane]) {

            LinkType now = typeAt(told.node);
            tell(told.observer, now, told.placeOfTypeBefore == UINT32_MAX ? s_nothing : nodesTold[lane][told.placeOfTypeBefore].second);
        }
    }
    for (unsigned lane = 0; lane < m_lanes.size(); ++lane) {
        if (m_lanes[lane]->nodesToTell.isEmpty() && !m_lanes[lane]->nodesToTell.capacity()) {
            nodesTold[lane].shrink(0);
            m_lanes[lane]->nodesToTell = WTF::move(nodesTold[lane]);
        }
        if (m_lanes[lane]->tellsForOneThread.isEmpty() && !m_lanes[lane]->tellsForOneThread.capacity()) {
            tellsForOneThread[lane].shrink(0);
            m_lanes[lane]->tellsForOneThread = WTF::move(tellsForOneThread[lane]);
        }
    }
}

void LinkFlow::flow()
{
    static constexpr size_t lengthOfStretch = 16;
    std::array<FlowObserver, lengthOfStretch> observers;

    auto tellObserversOf = [&](FlowNodeID node, const LinkType& before) {
        Lane& lane = laneOf(node);
        LinkType type = typeOfNode(node.index());
        for (uint32_t index = m_listsOfNodes[node.index()].firstObserver; index != UINT32_MAX;) {
            size_t numberOfObservers = 0;
            for (; index != UINT32_MAX && numberOfObservers < lengthOfStretch; ++numberOfObservers) {
                observers[numberOfObservers] = lane.observers[index].observer();
                index = lane.observers[index].next();
                FunctionID function = observers[numberOfObservers].function;
                __builtin_prefetch(&summaries.at(function).calls);
                __builtin_prefetch(&summaries.at(function).accesses);
                __builtin_prefetch(&m_nodesOfFunctions[function.index()]);
                __builtin_prefetch(&firstCallOfFunction[function.index()]);
            }
            for (size_t position = 0; position < numberOfObservers; ++position)
                tell(observers[position], type, before);
        }
    };

    Vector<std::pair<FlowNodeID, LinkType>> nodesToTell;
    Vector<uint64_t> numbers;
    Vector<uint64_t> roomToSort;
    while (true) {
        goAlongEdges();

        size_t numberOfNodesToTell = 0;
        for (auto& lane : m_lanes)
            numberOfNodesToTell += lane->nodesToTell.size();
        // WHAT A JOIN HAS CALLED FOR COMES FIRST, AND WHAT IT TIES GOES ALONG THE EDGES, BEFORE ANYBODY LOOKS. Of two families that are united either may be the one that stays, and until their nodes are tied and have told each other what they hold,
        if (!m_deferred.isEmpty()) {
            m_isDeferringUpdates = true;
            settle();
            m_isDeferringUpdates = false;
            applyDeferredUpdates();
            continue;
        }
        if (numberOfNodesToTell || numberOfNewObservers() || hasMembersToResolve() || !m_symbolsNotedBesideOtherKeys.isEmpty() || !m_symbolsNotedWithoutTheirNames.isEmpty() || !m_preparationsNoted.isEmpty() || m_unknownSymbolWasNotedWithPreparation) {
            ++m_numberOfRounds;
            m_isDeferringUpdates = true;
            m_tellsObservers = true;
            if (severalThreadsPayFor(numberOfNodesToTell + numberOfNewObservers()))
                tellOnLanes();
            for (auto& lane : m_lanes) {
                nodesToTell.appendVector(lane->nodesToTell);
                lane->nodesToTell.shrink(0);
            }
            switch (m_order) {
            case OrderOfWorklist::NewestFirst:
                numbers.shrink(0);
                for (uint32_t place = 0; place < nodesToTell.size(); ++place)
                    numbers.append(static_cast<uint64_t>(nodesToTell[place].first.index()) << 32 | place);
                sortNumbersInLinkFlow(numbers, roomToSort, m_nodes.size());
                break;
            case OrderOfWorklist::OldestFirst:
                std::ranges::reverse(nodesToTell);
                break;
            case OrderOfWorklist::Scattered:
                for (size_t place = nodesToTell.size(); place > 1; --place)
                    std::swap(nodesToTell[place - 1], nodesToTell[nextScatteredNumber() % place]);
                break;
            }
            for (uint32_t turn = 0; turn < nodesToTell.size(); ++turn) {
                auto& [node, before] = nodesToTell[m_order == OrderOfWorklist::NewestFirst ? static_cast<uint32_t>(numbers[turn]) : turn];
                m_isToBeTold.quickClear(node.index());
                tellObserversOf(node, before);
            }
            nodesToTell.shrink(0);
            flushFirstLaneNotes();
            auto resolveCalls = [&] {
                bool hasResolvedSome = resolveNotifiedCalls();
                flushFirstLaneNotes();
                return hasResolvedSome;
            };
            do {
                do {
                    if (severalThreadsPayFor(numberOfNewObservers()))
                        tellOnLanes();
                    while (!m_newObservers.isEmpty()) {
                        auto [observer, node] = takeNewObserver();
                        LinkType type = typeOfNode(node.index());
                        tell(observer, type, LinkType { });
                    }
                    flushFirstLaneNotes();
                } while (resolveCalls() || numberOfNewObservers() || takeInWhatWasNotedOfSymbols());
                wakeDeferredUpdateWaiters();
            } while (numberOfNewObservers());
            m_tellsObservers = false;
            settle();
            m_isDeferringUpdates = false;
            applyDeferredUpdates();
            continue;
        }

        m_isDeferringUpdates = true;
        bool hasTied = tieInWhatGrew();
        m_isDeferringUpdates = false;
        applyDeferredUpdates();
        bool somethingWaits = std::ranges::any_of(m_lanes, [](const auto& lane) {
            return !lane->worklist.isEmpty() || std::ranges::any_of(lane->boxes, [](const auto& box) {
                return !box.isEmpty();
            });
        });
        if (!hasTied && !somethingWaits && !numberOfNewObservers() && m_deferred.isEmpty())
            break;
    }
}

void LinkFlow::noteBreachOfClosure(BreachOfClosure&& breach)
{
    breach.told = m_observerToldOnceMore;
    breach.goneOver = m_nodeGoneOver;
    m_breachesOfClosure->append(WTF::move(breach));
}

Vector<BreachOfClosure> LinkFlow::findBreachesOfClosure()
{
    flow();
    Vector<BreachOfClosure> breaches;
    m_breachesOfClosure = &breaches;

    struct SetOfFlow {
        ASCIILiteral name;
        const BitVector& set;
        size_t sizeBefore;
    };
    SetOfFlow sets[] {
        { "reached"_s, reached, reached.bitCount() },
        { "lostLayouts"_s, lostLayouts, lostLayouts.bitCount() },
        { "lostFunctions"_s, lostFunctions, lostFunctions.bitCount() },
        { "escapedLayouts"_s, escapedLayouts, escapedLayouts.bitCount() },
        { "escapedFunctions"_s, escapedFunctions, escapedFunctions.bitCount() },
        { "escapedVariables"_s, escapedVariables, escapedVariables.bitCount() },
        { "fieldsThatMayBeEmpty"_s, fieldsThatMayBeEmpty, fieldsThatMayBeEmpty.bitCount() },
        { "layoutsMetInFamilies"_s, layoutsMetInFamilies, layoutsMetInFamilies.bitCount() },
        { "layoutsThatGainWhatUnseenCodeGaveSources"_s, layoutsThatGainWhatUnseenCodeGaveSources, layoutsThatGainWhatUnseenCodeGaveSources.bitCount() },
        { "declared families told lost"_s, m_familiesToldLost, m_familiesToldLost.bitCount() },
    };
    auto numberOfSitesThatHaveEscaped = [&] {
        size_t number = 0;
        for (uint32_t site = 0; site < m_sites.size(); ++site)
            number += hasEscaped(SiteID(site));
        return number;
    };
    auto numberOfPossibleChanges = [&] {
        size_t number = 0;
        for (OptionSet<LayoutChange> changes : possibleChanges)
            number += std::popcount(static_cast<uint32_t>(changes.toRaw()));
        for (OptionSet<LayoutChange> changes : changesByMoreThanStoresInSight)
            number += std::popcount(static_cast<uint32_t>(changes.toRaw()));
        return number;
    };
    size_t sitesThatHadEscaped = numberOfSitesThatHaveEscaped();
    size_t possibleChangesBefore = numberOfPossibleChanges();
    uint8_t marksThatWereUsed = m_marksThatAreUsed | m_marksThatReachUnseen;

    for (uint32_t site = 0; site < m_sites.size(); ++site) {
        if (!hasEscaped(SiteID(site)))
            continue;
        for (uint32_t index = m_sites[site].newestContents; index != UINT32_MAX;) {
            ContentsOfSite held = contentsListedOf(site)[index];
            index = held.next;
            m_nodeGoneOver = held.node;
            openContents(held.slot, held.node);
        }
    }

    size_t numberOfNodes = m_nodes.size();
    Vector<Edge> edges;
    Vector<FlowObserver> observers;
    for (size_t index = 0; index < numberOfNodes; ++index) {
        LinkType type = typeOfNode(index);
        if (type.isNever() || index == unseen().index())
            continue;
        m_nodeGoneOver = FlowNodeID(index);
        m_observerToldOnceMore = { };
        edges.shrink(0);
        Lane& lane = laneOf(FlowNodeID(index));
        for (uint32_t listed = m_listsOfNodes[index].firstEdge; listed != UINT32_MAX; listed = lane.edges[listed].next)
            edges.append(lane.edges[listed]);
        for (const Edge& edge : edges)
            arrive(edge.to, passedByTestOfEdge(type, edge.passes), symbolSeenAt(FlowNodeID(index)));
        observers.shrink(0);
        for (uint32_t listed = m_listsOfNodes[index].firstObserver; listed != UINT32_MAX; listed = lane.observers[listed].next())
            observers.append(lane.observers[listed].observer());
        for (const FlowObserver& observer : observers) {
            m_observerToldOnceMore = observer;
            tell(observer, type, LinkType { });
        }
    }
    m_nodeGoneOver = { };
    m_observerToldOnceMore = { };
    for (size_t index : BitVector(m_lostFunctionsTied))
        tieInLostFunction(FunctionID(index));
    wakeEverySleeperToSeeThatNothingGrows();
    m_isDeferringUpdates = true;
    m_tellsObservers = true;
    while (resolveNotifiedCalls())
        flushFirstLaneNotes();
    m_tellsObservers = false;
    settle();
    m_isDeferringUpdates = false;
    applyDeferredUpdates();
    flow();

    for (const SetOfFlow& set : sets) {
        if (set.set.bitCount() != set.sizeBefore)
            noteBreachOfClosure({ .set = set.name });
    }
    if (numberOfSitesThatHaveEscaped() != sitesThatHadEscaped)
        noteBreachOfClosure({ .set = "sites that have escaped"_s });
    if (numberOfPossibleChanges() != possibleChangesBefore)
        noteBreachOfClosure({ .set = "possibleChanges"_s });
    if (!m_storesUnderUnknownNamesToTie.isEmpty())
        noteBreachOfClosure({ .set = "stores under unknown names that wait to be tied"_s });
    if ((m_marksThatAreUsed | m_marksThatReachUnseen) != marksThatWereUsed)
        noteBreachOfClosure({ .set = "marks that are used"_s });
    m_breachesOfClosure = nullptr;
    return breaches;
}

void LinkType::dump(PrintStream& out) const
{
    CommaPrinter comma(" "_s);
    if (primitives)
        out.print(comma, "primitives=", RawHex(primitives));
    switch (objects) {
    case Objects::None:
        break;
    case Objects::Layout:
        out.print(comma, "L", layout.index());
        break;
    case Objects::Set:
        out.print(comma, "setOfLayouts");
        break;
    case Objects::Prototype:
        out.print(comma, "prototype(L", layout.index(), ")");
        break;
    case Objects::Prototypes:
        out.print(comma, "prototypes(family", family.index(), ")");
        break;
    case Objects::Family:
        out.print(comma, "family", family.index());
        break;
    case Objects::Lost:
        out.print(comma, "lost");
        if (layout)
            out.print("(L", layout.index(), ")");
        break;
    }
    switch (functions) {
    case Functions::None:
        break;
    case Functions::Closure:
        out.print(comma, "f", function.index());
        break;
    case Functions::Set:
        out.print(comma, "setOfFunctions");
        break;
    case Functions::Constructors:
        out.print(comma, "constructors(family", constructorsOf.index(), ")");
        break;
    case Functions::Lost:
        out.print(comma, "lostFunctions");
        break;
    }
    if (classes != Classes::None) {
        out.print(comma, "classes=", static_cast<unsigned>(classes));
        if (builtinClass)
            out.print(":", builtinClass.index());
        if (setOfClasses)
            out.print(":set", setOfClasses);
    }
    if (builtin)
        out.print(comma, "builtin", builtin.index());
    if (hasUnknownObjects)
        out.print(comma, "unseen");
    if (isNever())
        out.print("nothing");
}

void FlowObserver::dump(PrintStream& out) const
{
    static constexpr ASCIILiteral names[] { "subject of call"_s, "callee of call"_s, "receiver of call"_s, "subject of access"_s, "key of access"_s, "subject of change"_s, "other of change"_s, "operand of operator"_s, "step of inspection"_s, "member of shared call"_s, "member of shared access"_s, "member of access"_s, "symbol of preparation"_s, "passed to be named"_s, "operand only looked at"_s, "keys are listed"_s, "field of an accessor that was laid"_s, "field that is only asked"_s };
    out.print(names[static_cast<size_t>(kind)], " ", record, " of f", function.index());
}

void LinkFlow::describeLazyNode(PrintStream& out, FlowNodeID id) const
{
    auto printKey = [&](PropertyKeyID key) {
        if (!key)
            return;
        Vector<char16_t> buffer;
        out.print(" under \"", program.strings.viewOf(program.at(key).text, buffer), "\"");
    };
    for (const auto& [word, place] : m_placesOfAppendedFields) {
        const AppendedField& field = m_appendedFields[place];
        if (field.node != id && field.layoutStoreNode != id)
            continue;
        out.print(field.node == id ? "(born beside the fields of L" : "(stored through its layout beside the fields of L", word >> 32);
        printKey(field.key);
        out.print(")");
        return;
    }
    auto printIfListed = [&](const Vector<uint32_t>& nodes, size_t nodesOfLayout, size_t which, ASCIILiteral what) {
        for (size_t place = which; place < nodes.size(); place += nodesOfLayout) {
            if (nodes[place] != id.index())
                continue;
            out.print("(", what, " L", place / nodesOfLayout, ")");
            return true;
        }
        return false;
    };
    if (printIfListed(m_extraPropertyNodes, 2, 0, "found beside the fields of"_s) || printIfListed(m_extraPropertyNodes, 2, 1, "stored beside the fields of"_s)
        || printIfListed(m_nonNumericKeyReadNodes, 1, 0, "found under names that no number spells beside the fields of"_s)
        || printIfListed(m_unknownKeyStoreNodes, 1, 0, "stored under unknown names on"_s) || printIfListed(m_numericKeyStoreNodes, 1, 0, "stored under numbers on"_s))
        return;
    static constexpr ASCIILiteral kinds[] { "read through family"_s, "laid through family"_s, "any field read of L"_s, "any field stored of L"_s, "any field read of family"_s, "any field stored of family"_s, "any field read of set"_s, "element read of set"_s, "any field stored of set"_s, "element stored of set"_s, "found beside fields"_s, "laid on closures"_s, "any field defined on L"_s, "what stands above"_s, "what stands below"_s, "laid on constructor functions from"_s };
    static_assert(std::size(kinds) == static_cast<size_t>(NodeOnDemand::ConstructorChainStoreNode) + 1);
    for (const auto& [word, node] : m_nodesOnDemand) {
        if (node != id.index())
            continue;
        out.print("(", kinds[word >> 60], " ", (word >> 32) & ((1u << 28) - 1));
        printKey(PropertyKeyID::fromBits(static_cast<uint32_t>(word)));
        out.print(")");
        return;
    }
    describeSharedCallNode(out, id);
}

void LinkFlow::describe(PrintStream& out, FlowNodeID id) const
{
    size_t node = id.index();
    size_t numberOfFunctions = program.functions.size();
    if (node < m_firstVariable) {
        out.print("fixed", node);
        return;
    }
    if (node < m_firstVariable + program.variables.size()) {
        out.print("variable", node - m_firstVariable);
        return;
    }
    size_t firstField = m_firstVariable + program.variables.size();
    if (node < firstField + m_firstDeclaredFieldOfLayout.last()) {
        size_t layout = std::ranges::upper_bound(m_firstDeclaredFieldOfLayout, static_cast<uint32_t>(node - firstField)) - m_firstDeclaredFieldOfLayout.begin() - 1;
        uint32_t field = 0;
        while (m_nodeOfField[m_firstFieldOfLayout[layout] + field] != node)
            ++field;
        out.print("L", layout, ".field", field);
        return;
    }
    if (node < m_nodesOfFunctions[0].firstParameter) {
        size_t side = std::ranges::upper_bound(m_firstMemberNodeOfSide, static_cast<uint32_t>(node)) - m_firstMemberNodeOfSide.begin() - 1;
        out.print("L", side / 2, side % 2 ? ".staticMember" : ".member", node - m_firstMemberNodeOfSide[side]);
        return;
    }
    if (node >= m_nodesOfFunctions[numberOfFunctions].firstParameter) {
        out.print("madeOnDemand", node - m_nodesOfFunctions[numberOfFunctions].firstParameter);
        describeLazyNode(out, id);
        return;
    }
    size_t function = std::ranges::upper_bound(m_nodesOfFunctions, static_cast<uint32_t>(node), { }, &NodesOfFunction::firstParameter) - m_nodesOfFunctions.begin() - 1;
    const NodesOfFunction& nodes = m_nodesOfFunctions[function];
    if (node < nodes.firstResult)
        out.print("f", function, ".parameter", node - nodes.firstParameter);
    else if (node < nodes.thrown)
        out.print("f", function, ".result", node - nodes.firstResult);
    else if (node == nodes.thrown)
        out.print("f", function, ".thrown");
    else
        out.print("f", function, ".node", std::ranges::find(nodesOfSummaryOf(FunctionID(static_cast<uint32_t>(function))), static_cast<uint32_t>(node)) - nodesOfSummaryOf(FunctionID(static_cast<uint32_t>(function))).begin());
}

void LinkFlow::noteLostLayoutsGainingProperties()
{
    if (!m_hasStoresThroughLost)
        return;
    Vector<uint32_t> numberOfStoredKeysListed;
    numberOfStoredKeysListed.fill(0, program.layouts.size());
    Vector<uint32_t> numberOfStoredKeys;
    numberOfStoredKeys.fill(0, program.layouts.size());
    for (const LostKey& record : m_lostKeys) {
        if (record.hasBeenMerged || (record.stored == noNodeInLinkFlow && record.defined == noNodeInLinkFlow))
            continue;
        ++numberOfStoredKeys[unitedLayouts.representativeOf(record.family)];
        for (uint32_t index = record.firstHolder; index != UINT32_MAX; index = m_holdersOfLostKeys[index].next) {
            LayoutID holder = m_holdersOfLostKeys[index].layout;
            if (findField(program, holder, record.key))
                ++numberOfStoredKeysListed[holder.index()];
        }
    }
    for (size_t index : lostLayouts) {
        if (numberOfStoredKeysListed[index] < numberOfStoredKeys[unitedLayouts.representativeOf(static_cast<uint32_t>(index))])
            layoutsThatGainPropertiesThroughLost.set(index);
    }
}

bool LinkFlow::tieInWhatGrew()
{
    bool hasTied = false;
    hasTied |= tieInLostLayouts();
    hasTied |= tieInLostFunctions();
    hasTied |= openLayoutsThatChanged();
    hasTied |= markFieldsSkippedByCopies();
    hasTied |= tieStoresUnderUnknownNames();
    hasTied |= letMembersOfLostEscape();
    hasTied |= poolAppendedFields();
    hasTied |= tieAppendedFieldsOfLostLayouts();
    hasTied |= letPrototypesFoundThroughLostEscape();
    hasTied |= enterLibraryInReach();
    return hasTied;
}

LinkType LinkFlow::fromTypeID(TypeID id) const
{
    if (!id)
        return LinkType::anything();
    if (holdsOnlyPrimitives(id)) {
        LinkType result = typeOfPrimitivesInLinkFlow(LinkType::primitivesOf(primitivesInID(id), integersInID(id)));
        if (LinkType::Counts counts = LinkType::countsOf(primitivesInID(id), integersInID(id)); counts == LinkType::Counts::ExactFromZeroUp || counts == LinkType::Counts::Exact)
            result.counts = counts;
        return result;
    }

    Type type = program.types.at(id);
    if (type.opaque)
        return LinkType::anything();

    LinkType result = typeOfPrimitivesInLinkFlow(LinkType::primitivesOf(type.primitives, type.integers));
    if (type.environment)
        return joined(result, typeOfLayoutInLinkFlow(type.environment));
    result.hasUnknownObjects = type.hasUnboundedUnknown();

    TypeReader reader(program);
    if (type.layouts) {
        if (FamilyID family = holdsOneMember(type.layouts) ? FamilyID() : families.familyOf(type.layouts)) {
            result = joined(result, typeOfFamilyInLinkFlow(family));
            for (LayoutID member : layoutsOfFamily(family))
                noteMetInFamily(member);
        } else {
            reader.at(type.layouts).forEach([&](LayoutID layout) {
                result = joined(result, typeOfLayoutInLinkFlow(layout));
            });
        }
    }
    if (!classPartsOf(reader, type).empty())
        result = joined(result, typeOfUnseenObjectsInLinkFlow());
    if (!type.objects)
        return result;

    reader.at(type.objects).forEach([&](ObjectID object) {
        const ObjectRecord& record = reader.at(object);
        switch (record.kind) {
        case ObjectKind::Closure:
            result = joined(result, typeOfClosureInLinkFlow(FunctionID(record.payload)));
            return;
        case ObjectKind::Builtin:
        case ObjectKind::ModuleBuiltin:
            result = joined(result, typeOfBuiltinInLinkFlow(BuiltinID(record.payload)));
            return;
        case ObjectKind::Prototype:
            result = joined(result, typeOfPrototypeInLinkFlow(LayoutID(record.payload)));
            return;
        case ObjectKind::FunctionPrototype:
            if (LayoutID constructed = program.at(FunctionID(record.payload)).constructedClass)
                result = joined(result, typeOfPrototypeInLinkFlow(constructed));
            else {
                noteReachesUnseen(typeOfClosureInLinkFlow(FunctionID(record.payload)));
                result = joined(result, typeOfUnseenObjectsInLinkFlow());
            }
            return;
        case ObjectKind::Unknown:
        case ObjectKind::ResultOfBuiltin:
        case ObjectKind::HostObject:
        case ObjectKind::EscapedFunctionsOfHolder:
        case ObjectKind::EscapedOfClass:
        case ObjectKind::EscapedWithoutShape:
        case ObjectKind::CoroutineResult:
        case ObjectKind::RestArray:
        case ObjectKind::ConstructedObject:
        case ObjectKind::Namespace:
        case ObjectKind::Instance:
        case ObjectKind::EscapedOfLayout:
            if (!record.traits.containsAny({ ObjectTrait::HasLayout, ObjectTrait::HasClass }) || record.has(ObjectTrait::MayHaveLayoutBelow))
                result = joined(result, typeOfUnseenObjectsInLinkFlow());
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
    return result;
}

void LinkFlow::countAllocatedMembersOfFamilies()
{
    m_numbersOfAllocatedMembersOfFamilies.fill(0, families.size());
    for (size_t layout : allocatedLayouts) {
        if (FamilyID nearest = families.nearestFamilyOf(LayoutID(layout)))
            ++m_numbersOfAllocatedMembersOfFamilies[nearest.index()];
    }
    for (uint32_t family = 0; family < families.size(); ++family) {
        if (FamilyID above = families.nearestFamilyAbove(FamilyID(family)))
            m_numbersOfAllocatedMembersOfFamilies[above.index()] += m_numbersOfAllocatedMembersOfFamilies[family];
    }
}

FamilyID LinkFlow::familyWhoseAllocatedMembersAre(std::span<const LayoutID> members) const
{
    if (m_numbersOfAllocatedMembersOfFamilies.isEmpty())
        return { };
    FamilyID common = families.nearestCommonFamily(members[0], members[1]);
    for (size_t index = 2; index < members.size() && common; ++index)
        common = families.nearestCommonFamily(common, members[index]);
    if (!common || m_numbersOfAllocatedMembersOfFamilies[common.index()] != members.size())
        return { };
    return std::ranges::all_of(members, [&](LayoutID member) { return allocatedLayouts.get(member.index()); }) ? common : FamilyID();
}

void LinkFlow::nameLayoutsThatMet(TypeArena& types)
{
    RELEASE_ASSERT(m_typesAreSettled && m_layoutsThatMet.isEmpty());
    for (size_t layout : lostLayouts) {
        uint32_t smallestMember = static_cast<uint32_t>(layout);
        if (unitedLayouts.isAlone(smallestMember) || m_lostFamiliesThatHaveEscaped.quickGet(smallestMember))
            continue;
        auto named = m_layoutsThatMetByRepresentative.add(unitedLayouts.representativeWithoutShortening(smallestMember), LayoutSetID());
        if (!named.isNewEntry)
            continue;
        named.iterator->value = types.layoutSets().ofUnsorted(membersOfLostFamily(smallestMember).span());
        m_layoutsThatMet.append(named.iterator->value);
    }
}

void LinkFlow::forgetNamesThatAreNoFamilies(const Families& behindFlow)
{
    m_layoutsThatMetByRepresentative.removeIf([&](auto& named) {
        return program.at(named.value).size() > SetsOfLayoutsInLinkFlow::mostMembers && !behindFlow.familyOf(named.value);
    });
}

TypeID LinkFlow::toTypeID(TypeArena& types, const LinkType& type) const
{
    if (type.isNever())
        return { };
    if (type.builtin && lightFormOf(type.builtin))
        return toTypeID(types, LinkType::anything());

    uint16_t primitives = withoutLiteralsInLinkFlow(type.primitives);
    TypeParts parts;
    parts.primitives = LinkType::inNamesOfPrimitive(primitives);
    parts.integers = type.integersHeldAmong(primitives);
    bool holdsObjects = type.objects != LinkType::Objects::None || type.functions != LinkType::Functions::None || type.classes != LinkType::Classes::None || type.builtin || type.hasUnknownObjects;
    if (!holdsObjects)
        return *tryMakeTypeOfPrimitives(parts.primitives, parts.integers);

    auto holdAnyObject = [&](UnknownSeed seed) {
        parts.anyObject = true;
        parts.anyFunction = true;
        parts.seeds.add(seed);
    };

    switch (type.objects) {
    case LinkType::Objects::None:
        break;
    case LinkType::Objects::Layout:
        if (!program.at(type.layout).isEnvironment())
            parts.unnamedLayouts = LayoutSets::of(type.layout);
        else if ((parts.primitives & ~(Primitive::Absent | Primitive::Unspecified)) || type.functions != LinkType::Functions::None || type.classes != LinkType::Classes::None || type.builtin || type.hasUnknownObjects) {
            parts.primitives |= Primitive::All;
            parts.integers = Range::full();
            holdAnyObject(UnknownSeed::ContentsOfEscapedObject);
        } else
            parts.environment = type.layout;
        break;
    case LinkType::Objects::Set:
        if (FamilyID family = familyWhoseAllocatedMembersAre(layoutsOfSet(type)))
            parts.unnamedLayouts = families.layoutsOf(family);
        else
            parts.unnamedLayouts = types.layoutSets().ofSorted(layoutsOfSet(type));
        break;
    case LinkType::Objects::Family:
        parts.unnamedLayouts = families.layoutsOf(type.family);
        break;
    case LinkType::Objects::Lost:
        if (type.layout) {
            if (auto named = m_layoutsThatMetByRepresentative.find(unitedLayouts.representativeWithoutShortening(type.layout.index())); named != m_layoutsThatMetByRepresentative.end()) {
                parts.unnamedLayouts = named->value;
                break;
            }
        }
        [[fallthrough]];
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
        holdAnyObject(UnknownSeed::ContentsOfEscapedObject);
        break;
    }

    switch (type.functions) {
    case LinkType::Functions::None:
        break;
    case LinkType::Functions::Closure:
        if (!tablesMayName(type.function)) {
            parts.anyFunction = true;
            break;
        }
        parts.objects = types.objectSets().with(parts.objects, types.objects().closureOf(type.function));
        break;
    case LinkType::Functions::Set:
        if (reading() == WhatUnseenCodeDoes::Anything) {
            parts.anyFunction = true;
            parts.seeds.add(UnknownSeed::ContentsOfEscapedObject);
            break;
        }
        for (FunctionID member : functionsOfSet(type)) {
            if (!tablesMayName(member))
                parts.anyFunction = true;
            else
                parts.objects = types.objectSets().with(parts.objects, types.objects().closureOf(member));
        }
        break;
    case LinkType::Functions::Constructors:
    case LinkType::Functions::Lost:
        parts.anyFunction = true;
        parts.seeds.add(UnknownSeed::ContentsOfEscapedObject);
        break;
    }

    switch (type.classes) {
    case LinkType::Classes::None:
        break;
    case LinkType::Classes::Site:
        if (type.builtinClass == BuiltinClasses::Object && mayHoldCellsOfCompiledLayouts(type.site))
            holdAnyObject(UnknownSeed::ContentsOfEscapedObject);
        else if (!program.builtins().at(type.builtinClass).isFollowedBesideUnseenObject || !m_familiesOfSites.isAlone(type.site.index()))
            parts.unnamedClasses = TypeArena::partsOf(type.builtinClass);
        break;
    case LinkType::Classes::Mixed: {
        bool mayHoldCompiledCells = !namesClassesOf(type) || type.hasUnknownObjects || reading() == WhatUnseenCodeDoes::Anything;
        Vector<ClassPart, 8> classes;
        if (!mayHoldCompiledCells) {
            for (BuiltinClassID member : m_setsOfClasses.membersOf(type.setOfClasses)) {
                mayHoldCompiledCells |= member == BuiltinClasses::Object && mayHoldCellsOfCompiledLayouts(type.site);
                classes.append(ClassPart { member, { } });
            }
        }
        if (mayHoldCompiledCells)
            holdAnyObject(UnknownSeed::ContentsOfEscapedObject);
        else
            parts.unnamedClasses = types.internClassParts(classes.span());
        break;
    }
    }

    if (type.builtin)
        parts.objects = types.objectSets().with(parts.objects, types.objects().objectOf(type.builtin));
    if (type.hasUnknownObjects)
        holdAnyObject(UnknownSeed::UnseenCode);
    TypeID written = types.intern(parts);
    if (type.classes != LinkType::Classes::Site || !program.builtins().at(type.builtinClass).isFollowedBesideUnseenObject || !m_familiesOfSites.isAlone(type.site.index()))
        return written;
    auto seenIn = [&](ContentsSlot slot) {
        BuiltinSlotID word = program.builtins().findSlot(type.builtinClass, slot);
        FlowNodeID node = word ? findContentsOf(type, slotOf(word)) : FlowNodeID();
        LinkType held = node ? typeAt(node) : LinkType();
        held.classes = LinkType::Classes::None;
        held.builtinClass = { };
        held.site = { };
        held.hasUnknownObjects = false;
        TypeID seen = toTypeID(types, held);
        if (uint32_t written = seen && program.builtins().at(word).wordOfCell == BuiltinWordOfCell::MarkedWhereFilled ? writtenLengthOf(type.site) : 0)
            return types.integer(Range::one(written));
        return seen ? seen : TypeID(TypeTable::never);
    };
    std::array<TypeID, 3> contents { seenIn(ContentsSlot::Value), seenIn(ContentsSlot::Key), seenIn(ContentsSlot::Element) };
    return types.unionOf(written, types.builtinClass(type.builtinClass, contents));
}

const LinkFlow::InstanceOfGuardedReads* LinkFlow::instanceOfGuardedReads(FunctionID reader)
{
    const BodyIndex* index = bodiesLookedAt(looks).find(reader);
    if (!index || !index->opcodes().contains(Opcode::InstanceOf))
        return nullptr;
    auto& known = s_laneAtWork ? s_laneAtWork->instanceOfGuardedReads : m_instanceOfGuardedReads;
    return known.ensure(reader.index(), [&] {
        return makeUniqueWithoutFastMallocCheck<InstanceOfGuardedReads>(findInstanceOfGuardedReads(program, *index, summaries.at(reader)));
    }).iterator->value.get();
}

LinkType LinkFlow::passedByTestOfEdgeSlowCase(const LinkType& type, TypeID test)
{
    if (type.isNever() || holdsOnlyPrimitives(test))
        return passedBySlowCase(type, test);

    auto& testsOfEdges = s_laneAtWork ? s_laneAtWork->testsOfEdges : m_testsOfEdges;
    auto known = testsOfEdges.find(test.bits());
    if (known == testsOfEdges.end()) {
        Type tested = program.types.at(test);
        TestOfEdge asked;
        asked.primitives = primitivesOfTypeInLinkFlow(tested.primitives, tested.integers);
        if (!tested.isJavaScriptValue())
            asked.passes = TestOfEdge::Passes::Everything;
        else if (!tested.hasObjects())
            asked.passes = TestOfEdge::Passes::PrimitivesAlone;
        else if (tested.anyObject || tested.traitsOfNamed.containsAll({ ObjectTrait::HasNoShape, ObjectTrait::IsOtherObject }))
            asked.passes = TestOfEdge::Passes::EveryObject;
        known = testsOfEdges.add(test.bits(), asked).iterator;
    }

    switch (known->value.passes) {
    case TestOfEdge::Passes::Everything:
        return type;
    case TestOfEdge::Passes::PrimitivesAlone: {
        LinkType passed = typeOfPrimitivesInLinkFlow(type.primitives & known->value.primitives);
        passed.escapeOnlyPrimitives = passed.primitives & ~withoutLiteralsInLinkFlow(type.directPrimitives() & known->value.primitives);
        passed.counts = type.counts;
        return passed;
    }
    case TestOfEdge::Passes::EveryObject: {
        LinkType passed = type;
        passed.primitives = type.primitives & known->value.primitives;
        return passed;
    }
    case TestOfEdge::Passes::SomeObjects:
        return passedBy(type, test);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void keepIntegersWithin(LinkType& type, Range bounds)
{
    constexpr uint16_t wideNumbers = Primitive::Integer | Primitive::WholeNumber;
    bool areExactIntegers = !(type.primitives & Primitive::Double) || (LinkType::areExact(type.counts) && !(type.primitives & Primitive::FractionalNumber));
    if (bounds.isEmpty() || bounds == Range::full() || !type.hasNumbers() || !areExactIntegers)
        return;
    bool holdsWideIntegers = type.primitives & wideNumbers;
    bool startsAtZero = bounds.minimum >= 0 || LinkType::startsAtZero(type.counts);
    bool startsInInt32 = startsAtZero || bounds.minimum >= INT32_MIN || type.counts == LinkType::Counts::Int32 || type.counts == LinkType::Counts::FromInt32MinBelowTwoTo32;
    if (!holdsWideIntegers || (bounds.maximum <= INT32_MAX && startsInInt32)) {
        type.primitives = static_cast<uint16_t>((type.primitives & ~wideNumbers) | LinkType::int32);
        type.escapeOnlyPrimitives &= type.primitives;
        type.counts = startsAtZero ? LinkType::Counts::BelowTwoTo31 : LinkType::Counts::Int32;
    } else if (startsAtZero && LinkType::areExact(type.counts))
        type.counts = static_cast<LinkType::Counts>(static_cast<uint8_t>(type.counts) & 3);
}

LinkType LinkFlow::passedBySlowCase(const LinkType& type, TypeID test) const
{
    if (type.isNever())
        return type;
    auto primitivesAlone = [&](uint16_t primitives) {
        LinkType passed = typeOfPrimitivesInLinkFlow(primitives);
        passed.escapeOnlyPrimitives = passed.primitives & ~withoutLiteralsInLinkFlow(primitives & ~type.escapeOnlyPrimitives);
        passed.counts = type.counts;
        return passed;
    };
    if (holdsOnlyPrimitives(test)) {
        LinkType passed = primitivesAlone(type.primitives & primitivesOfTypeInLinkFlow(primitivesInID(test), integersInID(test)));
        keepIntegersWithin(passed, integersInID(test));
        return passed;
    }

    Type tested = program.types.at(test);
    if (!tested.isJavaScriptValue())
        return type;

    uint16_t primitives = type.primitives & primitivesOfTypeInLinkFlow(tested.primitives, tested.integers);
    if (!tested.hasObjects())
        return primitivesAlone(primitives);

    LinkType passed = type;
    passed.primitives = primitives;
    if (tested.anyObject || tested.traitsOfNamed.containsAll({ ObjectTrait::HasNoShape, ObjectTrait::IsOtherObject }))
        return passed;

    TypeReader reader(program);
    const BuiltinTable& builtins = program.builtins();
    std::span<const ClassPart> classes = classPartsOf(reader, tested);

    auto passObjects = [&](const LinkType& objects) {
        passed.objects = objects.objects;
        passed.layout = objects.layout;
        passed.family = objects.family;
    };
    LayoutID onlyLayoutTested = tested.layouts && holdsOneMember(tested.layouts) ? LayoutID(positionOf(tested.layouts)) : LayoutID();
    FamilyID familyTested = tested.layouts && !onlyLayoutTested ? families.familyOf(tested.layouts) : FamilyID();
    switch (type.objects) {
    case LinkType::Objects::None:
        break;
    case LinkType::Objects::Layout:
        if (!(tested.layouts && reader.at(tested.layouts).contains(type.layout)) && !mayBeInsideSomeClassPart(reader, type.layout, classes))
            passObjects(LinkType { });
        break;
    case LinkType::Objects::Set: {
        Vector<LayoutID, SetsOfLayoutsInLinkFlow::mostMembers> kept;
        for (LayoutID member : layoutsOfSet(type)) {
            if ((tested.layouts && reader.at(tested.layouts).contains(member)) || mayBeInsideSomeClassPart(reader, member, classes))
                kept.append(member);
        }
        if (kept.size() != layoutsOfSet(type).size())
            passObjects(typeOfLayouts(kept.span()));
        break;
    }
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
        if (classes.empty() && !tested.layouts)
            passObjects(LinkType { });
        break;
    case LinkType::Objects::Family:
        if (!classes.empty())
            break;
        if (!tested.layouts)
            passObjects(LinkType { });
        else if (onlyLayoutTested)
            passObjects(families.nearestCommonFamily(type.family, onlyLayoutTested) == type.family ? typeOfLayoutInLinkFlow(onlyLayoutTested) : LinkType { });
        else if (familyTested) {
            FamilyID common = families.nearestCommonFamily(type.family, familyTested);
            if (common == type.family)
                passObjects(typeOfFamilyInLinkFlow(familyTested));
            else if (common != familyTested)
                passObjects(LinkType { });
        }
        break;
    case LinkType::Objects::Lost:
        if (!classes.empty())
            break;
        if (!tested.layouts)
            passObjects(LinkType { });
        else if (onlyLayoutTested)
            passObjects(typeOfLayoutInLinkFlow(onlyLayoutTested));
        else if (familyTested)
            passObjects(typeOfFamilyInLinkFlow(familyTested));
        break;
    }

    bool mayHoldAnyFunction = tested.anyFunction || tested.traitsOfNamed.contains(ObjectTrait::IsBuiltin) || std::ranges::any_of(classes, [&](const ClassPart& part) {
        return isFunctionClass(builtins, part.builtinClass) || !hasInstanceTest(builtins, part.builtinClass);
    });
    bool passesFunctions = true;
    switch (type.functions) {
    case LinkType::Functions::None:
        break;
    case LinkType::Functions::Closure:
        passesFunctions = mayHoldAnyFunction || namesFunction(reader, tested, type.function);
        break;
    case LinkType::Functions::Set: {
        if (mayHoldAnyFunction)
            break;
        Vector<FunctionID, SetsOfFunctionsInLinkFlow::mostMembers> kept;
        for (FunctionID member : functionsOfSet(type)) {
            if (namesFunction(reader, tested, member))
                kept.append(member);
        }
        if (kept.size() != functionsOfSet(type).size()) {
            LinkType named = typeOfFunctions(kept.span());
            passed.functions = named.functions;
            passed.function = named.function;
        }
        break;
    }
    case LinkType::Functions::Constructors:
    case LinkType::Functions::Lost:
        passesFunctions = mayHoldAnyFunction || tested.traitsOfNamed.contains(ObjectTrait::IsClosure);
        break;
    }
    if (!passesFunctions) {
        passed.functions = LinkType::Functions::None;
        passed.function = { };
        passed.constructorsOf = { };
    }

    bool passesObjectsOfFunctionClasses = mayHoldAnyFunction || tested.mayBeFunction();
    bool passesClasses = true;
    switch (type.classes) {
    case LinkType::Classes::None:
        break;
    case LinkType::Classes::Site:
        passesClasses = tested.layouts || (passesObjectsOfFunctionClasses && isFunctionClass(builtins, type.builtinClass)) || std::ranges::any_of(classes, [&](const ClassPart& part) {
            return relate(builtins, type.builtinClass, part.builtinClass) != ClassRelation::Disjoint;
        });
        break;
    case LinkType::Classes::Mixed:
        passesClasses = tested.layouts || !classes.empty() || (passesObjectsOfFunctionClasses && (!namesClassesOf(type) || std::ranges::any_of(m_setsOfClasses.membersOf(type.setOfClasses), [&](BuiltinClassID member) {
            return isFunctionClass(builtins, member);
        })));
        break;
    }
    if (!passesClasses)
        passed.dropInstances();

    if (!mayHoldAnyFunction && classes.empty() && !tested.traitsOfNamed.contains(ObjectTrait::IsClosure))
        passed.builtin = { };
    return passed;
}

uint64_t LinkFlow::wordOfNodeOnDemand(NodeOnDemand kind, uint32_t number, PropertyKeyID key)
{
    static_assert(static_cast<uint64_t>(NodeOnDemand::ConstructorChainStoreNode) < 1u << 4);
    RELEASE_ASSERT(number < 1u << 28);
    return static_cast<uint64_t>(kind) << 60 | static_cast<uint64_t>(number) << 32 | key.bits();
}

std::pair<FlowNodeID, bool> LinkFlow::nodeOnDemand(NodeOnDemand kind, uint32_t number, PropertyKeyID key)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    auto added = m_nodesOnDemand.add(wordOfNodeOnDemand(kind, number, key), noNodeInLinkFlow);
    if (added.isNewEntry)
        added.iterator->value = makeNode().index();
    return { FlowNodeID(added.iterator->value), added.isNewEntry };
}

FlowNodeID LinkFlow::findNodeOnDemand(NodeOnDemand kind, uint32_t number, PropertyKeyID key) const
{
    auto found = m_nodesOnDemand.find(wordOfNodeOnDemand(kind, number, key));
    return found == m_nodesOnDemand.end() ? FlowNodeID() : FlowNodeID(found->value);
}

bool LinkFlow::isAbsentFromObjectPrototype(PropertyKeyID key) const
{
    if (isOnObjectPrototype(nameClassOf(key)))
        return false;
    const PropertyKey& record = program.at(key);
    if (record.kind == PropertyKeyKind::Text && program.isOpenedSomewhere(record.text))
        return false;
    BuiltinName name = program.builtinNameOf(key);
    if (!name)
        return true;
    const BuiltinTable& builtins = program.builtins();
    BuiltinClassID objectClass = BuiltinClasses::Object;
    return builtins.findMember(builtins.at(objectClass).prototype, name).outcome == BuiltinMemberLookup::Outcome::Absent;
}

static BuiltinID functionLockedOnObjectPrototypeInLinkFlow(const Program& program, PropertyKeyID key)
{
    const BuiltinTable& builtins = program.builtins();
    BuiltinID holder = builtins.canonicalID(Builtins::ObjectPrototype);
    if (compiledModuleWithObject(program, holder) || builtins.at(builtins.rootOf(holder)).scope != BuiltinScope::Realm)
        return { };
    MembersUnderKey members(program, key);
    BuiltinID member = members.findOwnMember(holder);
    if (!member || !members.isLockedLocation(holder))
        return { };
    const Builtin& entry = builtins.at(member);
    BuiltinID canonical = builtins.canonicalID(member);
    if (entry.kind != BuiltinKind::Method && entry.kind != BuiltinKind::Function)
        return { };
    return compiledModuleWithObject(program, canonical) ? BuiltinID() : canonical;
}

LinkType LinkFlow::whatLiesAboveClasses(LayoutID layout, PropertyKeyID key) const
{
    if (key && isAbsentFromObjectPrototype(key))
        return typeOfPrimitivesInLinkFlow(Primitive::Undefined);

    LinkType found = typeOfPrimitivesInLinkFlow(Primitive::UndefinedOrNull);
    found.hasUnknownObjects = true;
    found.isUnknownEscapeOnly = !key && UnknownByEscapeAlone::marksAtWork() == UnknownByEscapeAlone::Marks::Nothing;
    NameClass nameClass = key ? nameClassOf(key) : NameClass::Ordinary;
    bool mayFindClass = !key || nameClass == NameClass::Constructor;
    bool mayFindPrototype = !key || nameClass == NameClass::Proto;
    if (!mayFindClass && !mayFindPrototype) {
        uint32_t known = askedOnceByLane(MadeOnDemand::FunctionOnObjectPrototype, key.index(), [&]() -> uint32_t {
            BuiltinID function = m_functionsOnObjectPrototype.ensure(key.index(), [&] {
                return functionLockedOnObjectPrototypeInLinkFlow(program, key);
            }).iterator->value;
            return function ? function.index() + 1 : 0;
        });
        return known ? typeOfBuiltinInLinkFlow(BuiltinID(known - 1)) : found;
    }

    for (RowOfBasesInLinkFlow row(program, layout); row.current(); row.advance()) {
        FunctionID constructor = program.at(row.current()).constructor;
        if (!constructor)
            continue;
        if (mayFindClass) {
            found.functions = LinkType::Functions::Closure;
            found.function = constructor;
        }
        if (mayFindPrototype) {
            found.objects = LinkType::Objects::Prototype;
            found.layout = row.current();
        }
        break;
    }
    return found;
}

static void arriveConstructorsOfRowAboveInLinkFlow(LinkFlow& flow, FlowNodeID aggregate, LayoutID layout)
{
    if (!flow.isBelowConstructorFunction(layout))
        return;
    for (RowOfBasesInLinkFlow row(flow.program, layout); row.current(); row.advance()) {
        if (FunctionID constructor = flow.program.at(row.current()).constructor)
            flow.arrive(aggregate, typeOfClosureInLinkFlow(constructor));
    }
}

void LinkFlow::ensureExtraPropertyNodes(LayoutID layout)
{
    size_t index = 2 * static_cast<size_t>(layout.index());
    if (m_extraPropertyNodes[index] != noNodeInLinkFlow)
        return;
    m_extraPropertyNodes[index] = makeNode().index();
    m_extraPropertyNodes[index + 1] = makeNode().index();
    m_extraPropertyNodeLayouts.append(layout);
    if (namesMayBeIncomplete(layout, ClassSide::Prototype)) {
        deferEscape(layout);
        open(layout);
    }
}

FlowNodeID LinkFlow::extraPropertyReadNode(LayoutID layout)
{
    return nodeAskedOnceByLane(MadeOnDemand::ExtraPropertyReadNode, layout.index(), [&]() -> FlowNodeID {
        ensureExtraPropertyNodes(layout);
        return FlowNodeID(m_extraPropertyNodes[2 * static_cast<size_t>(layout.index())]);
    });
}

FlowNodeID LinkFlow::extraPropertyStoreNode(LayoutID layout)
{
    return nodeAskedOnceByLane(MadeOnDemand::ExtraPropertyStoreNode, layout.index(), [&]() -> FlowNodeID {
        ensureExtraPropertyNodes(layout);
        return FlowNodeID(m_extraPropertyNodes[2 * static_cast<size_t>(layout.index()) + 1]);
    });
}

void LinkFlow::joinExtraProperties(LayoutID layout, FlowNodeID joined)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (m_layoutsWithJoinedProperties.set(layout.index()))
        return;
    for (uint32_t place = m_newestAppendedFieldOfLayout[layout.index()]; place != UINT32_MAX;) {
        AppendedField field = m_appendedFields[place];
        place = field.next;
        addEdge(field.node, joined);
    }
}

FlowNodeID LinkFlow::joinedExtraProperties(LayoutID layout)
{
    FlowNodeID joined = extraPropertyStoreNode(layout);
    joinExtraProperties(layout, joined);
    return joined;
}

FlowNodeID LinkFlow::whereLossIsTold(LayoutID layout)
{
    return nodeAskedOnceByLane(MadeOnDemand::WhereLossIsTold, layout.index(), [&]() -> FlowNodeID {
        auto added = m_nodesWhereLossIsTold.add(layout.index(), UINT32_MAX);
        if (added.isNewEntry)
            added.iterator->value = makeNode().index();
        return FlowNodeID(added.iterator->value);
    });
}

FlowNodeID LinkFlow::unknownKeyStoreNode(LayoutID layout, bool keyIsNumber)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    uint32_t& node = (keyIsNumber ? m_numericKeyStoreNodes : m_unknownKeyStoreNodes)[layout.index()];
    if (node == noNodeInLinkFlow) {
        node = makeNode().index();
        addEdge(FlowNodeID(node), extraPropertyStoreNode(layout));
        addEdge(FlowNodeID(node), extraPropertyReadNode(layout));
        if (!keyIsNumber)
            addEdge(FlowNodeID(node), nonNumericKeyReadNode(layout));
        if (uint32_t whereAccessorStands = m_nodesStoredWhereAccessorStandsAbove[layout.index()]; whereAccessorStands != noNodeInLinkFlow)
            addEdge(FlowNodeID(whereAccessorStands), FlowNodeID(node));
    }
    return FlowNodeID(node);
}

FlowNodeID LinkFlow::storedWhereAccessorStandsAbove(LayoutID layout)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    uint32_t& node = m_nodesStoredWhereAccessorStandsAbove[layout.index()];
    if (node == noNodeInLinkFlow) {
        node = makeNode().index();
        for (uint32_t stored : { m_unknownKeyStoreNodes[layout.index()], m_numericKeyStoreNodes[layout.index()] }) {
            if (stored != noNodeInLinkFlow)
                addEdge(FlowNodeID(node), FlowNodeID(stored));
        }
    }
    return FlowNodeID(node);
}

FlowNodeID LinkFlow::nonNumericKeyReadNode(LayoutID layout)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    uint32_t& node = m_nonNumericKeyReadNodes[layout.index()];
    if (node == noNodeInLinkFlow) {
        FlowNodeID found = extraPropertyReadNode(layout);
        node = makeNode().index();
        if (m_openLayouts.get(layout.index()))
            addEdge(unseen(), FlowNodeID(node));
        if (typeAt(found).primitives & Primitive::Undefined && layoutsThatGetNullAbove.get(layout.index()))
            arrive(FlowNodeID(node), fromTypeID(TypeTable::undefined));
        if (uint32_t stored = m_unknownKeyStoreNodes[layout.index()]; stored != noNodeInLinkFlow)
            addEdge(FlowNodeID(stored), FlowNodeID(node));
    }
    return FlowNodeID(node);
}

FlowNodeID LinkFlow::writtenAtMember(BuiltinID member)
{
    return nodeAskedOnceByLane(MadeOnDemand::WrittenAtMember, member.index(), [&]() -> FlowNodeID {
        auto added = m_nodesOfMembersWritten.add(member.index(), UINT32_MAX);
        if (added.isNewEntry)
            added.iterator->value = makeNode().index();
        return FlowNodeID(added.iterator->value);
    });
}

FlowNodeID LinkFlow::findWrittenAtMember(BuiltinID member) const
{
    auto found = m_nodesOfMembersWritten.find(member.index());
    return found == m_nodesOfMembersWritten.end() ? FlowNodeID() : FlowNodeID(found->value);
}

bool LinkFlow::isPlainClosureMadeWith(FunctionID function, PropertyKeyID key) const
{
    LayoutID constructed = program.at(function).constructedClass;
    if (!key || (constructed && !isOfConstructorFunction(constructed)))
        return false;
    return key == program.keyOf(BuiltinNames::name) || key == program.keyOf(BuiltinNames::length);
}

bool LinkFlow::needsPrimitive(const FlowObserver& observer) const
{
    if (observer.kind == FlowObserver::Kind::OperandOfOperator)
        return true;
    if (observer.kind != FlowObserver::Kind::ReceiverOfCall)
        return false;
    const SummaryCall& call = summaries.at(observer.function).calls[observer.record];
    return call.isMadeByProtocol && call.callee == SummaryCall::Callee::Name && std::ranges::any_of(program.builtins().keysReadBy(Protocol::ToPrimitive), [&](BuiltinName name) {
        return program.keyOf(name) == call.key;
    });
}

bool LinkFlow::isClassObjectMadeWith(FunctionID function, PropertyKeyID key) const
{
    LayoutID classLayout = program.at(function).constructedClass;
    if (!classLayout || isOfConstructorFunction(classLayout) || program.at(classLayout).constructor != function || hasComputedKeysOfItsOwn(classLayout, ClassSide::Static))
        return false;
    if (key != program.keyOf(BuiltinNames::name) && key != program.keyOf(BuiltinNames::length))
        return false;
    bool isDeclared = findMembers(classLayout, ClassSide::Static, key).mayAnswer();
    program.at(classLayout).forEachStaticFieldThatMayMatch(key, [&](const StaticField&) {
        isDeclared = true;
    });
    return !isDeclared;
}

void LinkFlow::passedToBeNamedObserved(const FlowObserver& observer, const LinkType& now)
{
    LinkType passedOn = now;
    bool isPlainClosure = now.functions == LinkType::Functions::Closure && isPlainClosureMadeWith(now.function, program.keyOf(BuiltinNames::name));
    if (isPlainClosure || now.functions == LinkType::Functions::Set || now.functions == LinkType::Functions::Lost) {
        passedOn.functions = LinkType::Functions::Lost;
        passedOn.function = { };
    }
    arrive(parameter(observer.function, observer.record), passedOn);
}

bool LinkFlow::followsNamesOnClosuresOf(FunctionID function, PropertyKeyID key) const
{
    const Function& record = program.at(function);
    if (!key || liesInThread(key) || record.isCoroutine() || isClassConstructorOf(record.kind) || !record.hasBody())
        return false;
    return key == program.keyOf(program.builtins().nameOf(WellKnownSymbol::HasInstance)) || isNameFollowedOnClosures(key);
}

FunctionID LinkFlow::classRowNameHolder(LayoutID classLayout, PropertyKeyID key) const
{
    LayoutID root = m_rootsOfClassRows[classLayout.index()];
    FunctionID holder = root ? program.at(root).constructor : FunctionID();
    if (!key || liesInThread(key) || !holder || !isClassConstructorOf(program.at(holder).kind) || program.at(holder).constructedClass != root || isOfConstructorFunction(root))
        return { };
    return isNameFollowedOnClosures(key) ? holder : FunctionID();
}

void LinkFlow::indexRootsOfClassRows()
{
    m_rootsOfClassRows.fill(LayoutID(), program.layouts.size());
    Vector<LayoutID, 16> row;
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        row.shrink(0);
        LayoutID current(index);
        while (!m_rootsOfClassRows[current.index()] && program.at(current).base && row.size() < program.layouts.size()) {
            row.append(current);
            current = program.at(current).base;
        }
        LayoutID root = m_rootsOfClassRows[current.index()] ? m_rootsOfClassRows[current.index()] : current;
        m_rootsOfClassRows[current.index()] = root;
        for (LayoutID below : row)
            m_rootsOfClassRows[below.index()] = root;
    }
}

bool LinkFlow::isNameFollowedOnClosures(PropertyKeyID key) const
{
    if (program.at(key).kind != PropertyKeyKind::Text)
        return false;
    NameClass nameClass = nameClassOf(key);
    if (nameClass == NameClass::Proto || nameClass == NameClass::Prototype || nameClass == NameClass::Length || mayBeElementKey(nameClass))
        return false;
    for (BuiltinName own : { BuiltinName(BuiltinNames::name), BuiltinName(BuiltinNames::length), BuiltinName(BuiltinNames::arguments), BuiltinName(BuiltinNames::caller) }) {
        if (program.keyOf(own) == key)
            return false;
    }
    return true;
}

FlowNodeID LinkFlow::laidOnClosuresOf(FunctionID function, PropertyKeyID key)
{
    FlowNodeID underKey = foundBesideFieldsUnder(key);
    return nodeAskedOnceByLane(MadeOnDemand::LaidOnClosuresOf, static_cast<uint64_t>(function.index()) << 32 | key.bits(), [&]() -> FlowNodeID {
        auto [node, isNew] = nodeOnDemand(NodeOnDemand::LaidOnClosuresUnderKey, function.index(), key);
        if (isNew) {
            if (m_newestNameOnClosuresOf.isEmpty())
                m_newestNameOnClosuresOf.fill(UINT32_MAX, program.functions.size());
            m_namesOnClosures.append(NameOnClosures { function, key, node, std::exchange(m_newestNameOnClosuresOf[function.index()], static_cast<uint32_t>(m_namesOnClosures.size())) });
            if (const Function& holder = program.at(function); isClassConstructorOf(holder.kind)) {
                if (m_classObjectsThatAreOpen.get(holder.constructedClass.index()))
                    addEdge(node, unseen());
                else
                    m_nodesThatLeaveWithClassObjects.ensure(holder.constructedClass.index(), [] { return Vector<FlowNodeID, 1>(); }).iterator->value.append(node);
            }
            if (lostFunctions.get(function.index())) {
                addEdge(underKey, node);
                addEdge(node, unseen());
            }
            if (uint32_t laidOnClosuresOfPool = m_lostFunctionsTied.get(function.index()) ? m_poolsOfLostFunctions[poolOfLostFunctions(unitedFunctions.representativeOf(function.index()))].laidOnClosures : UINT32_MAX; laidOnClosuresOfPool != UINT32_MAX)
                addEdge(node, FlowNodeID(laidOnClosuresOfPool));
        }
        return node;
    });
}

void LinkFlow::letLeaveWithClassObject(LayoutID classLayout, FlowNodeID node)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (m_classObjectsThatAreOpen.get(classLayout.index())) {
        addEdge(node, unseen());
        return;
    }
    Vector<FlowNodeID, 1>& nodes = m_nodesThatLeaveWithClassObjects.ensure(classLayout.index(), [] { return Vector<FlowNodeID, 1>(); }).iterator->value;
    if (!nodes.contains(node))
        nodes.append(node);
}

void LinkFlow::openWhatLeavesWithClassObject(LayoutID classLayout)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (m_classObjectsThatAreOpen.set(classLayout.index()))
        return;
    for (FlowNodeID node : m_nodesThatLeaveWithClassObjects.take(classLayout.index()))
        addEdge(node, unseen());
}

void LinkFlow::tieNamesOnLostClosuresOf(FunctionID function)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (m_newestNameOnClosuresOf.isEmpty())
        return;
    for (uint32_t index = m_newestNameOnClosuresOf[function.index()]; index != UINT32_MAX;) {
        NameOnClosures name = m_namesOnClosures[index];
        addEdge(foundBesideFieldsUnder(name.key), name.laid);
        addEdge(name.laid, unseen());
        index = name.next;
    }
}

FlowNodeID LinkFlow::foundBesideFieldsUnder(PropertyKeyID key)
{
    return nodeAskedOnceByLane(MadeOnDemand::FoundBesideFieldsUnder, key.bits(), [&]() -> FlowNodeID {
        auto [node, isNew] = nodeOnDemand(NodeOnDemand::FoundBesideFieldsUnderKey, 0, key);
        if (isNew && mayBeNamedByNumber(key))
            addEdge(node, foundBesideFieldsUnderAnyNumber());
        return node;
    });
}

FlowNodeID LinkFlow::foundBesideFieldsUnderAnyNumber()
{
    if (m_foundBesideFieldsUnderAnyNumber == noNodeInLinkFlow)
        m_foundBesideFieldsUnderAnyNumber = makeNode().index();
    return FlowNodeID(m_foundBesideFieldsUnderAnyNumber);
}

FlowNodeID LinkFlow::elementsSeenOn(LayoutID layout)
{
    return aggregateOfReadsUnderNumbers(m_elementsSeenOnLayouts, program.layouts.size(), layout.index()).first;
}

std::pair<FlowNodeID, bool> LinkFlow::aggregateOfReadsUnderNumbers(Vector<uint32_t>& nodes, size_t numberOfHolders, uint32_t holder)
{
    if (nodes.isEmpty())
        nodes.fill(noNodeInLinkFlow, numberOfHolders);
    if (nodes[holder] != noNodeInLinkFlow)
        return { FlowNodeID(nodes[holder]), false };
    nodes[holder] = makeNode().index();
    return { FlowNodeID(nodes[holder]), true };
}

std::span<const LinkFlow::OwnValue> LinkFlow::ownValuesOf(LayoutID layout)
{
    Lane* lane = m_severalThreadsTell ? s_laneAtWork : nullptr;
    if (lane && lane->layoutsWhoseOwnValuesAreSeen.get(layout.index()))
        return m_ownValuesOfLayouts[layout.index()].span();
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (m_ownValuesOfLayouts.isEmpty()) {
        m_ownValuesOfLayouts.grow(program.layouts.size());
        m_layoutsWithListedFieldWithoutKey.ensureSize(program.layouts.size());
        m_layoutsWhoseOwnValuesAreListed.ensureSize(program.layouts.size());
    }
    if (!m_layoutsWhoseOwnValuesAreListed.quickGet(layout.index())) {
        Vector<OwnValue, 8> values = listOwnValuesOf(layout);
        if (!m_layoutsWhoseOwnValuesAreListed.quickSet(layout.index())) {
            m_ownValuesOfLayouts[layout.index()].appendVector(values);
            if (std::ranges::count_if(values, [](const OwnValue& value) { return !value.key; }) > numberOfOwnValuesBesideFields)
                m_layoutsWithListedFieldWithoutKey.quickSet(layout.index());
        }
    }
    if (lane)
        lane->layoutsWhoseOwnValuesAreSeen.ensureSizeAndSet(layout.index(), program.layouts.size());
    return m_ownValuesOfLayouts[layout.index()].span();
}

std::span<const LinkFlow::OwnValue> LinkFlow::ownValuesThatMayHaveNoKeyOf(LayoutID layout)
{
    std::span<const OwnValue> values = ownValuesOf(layout);
    return values.empty() || m_layoutsWithListedFieldWithoutKey.quickGet(layout.index()) ? values : values.last(numberOfOwnValuesBesideFields);
}

Vector<LinkFlow::OwnValue, 8> LinkFlow::listOwnValuesOf(LayoutID layout)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    Vector<OwnValue, 8> values;
    if (!keepsValuesInFieldsInLinkFlow(program.at(layout).kind))
        return values;
    bool listsAccessor = false;
    forEachFieldListedInLinkFlow(program, layout, [&](FieldReference reference) {
        const Field& listed = fieldAt(program, reference);
        listsAccessor |= listed.form.contains(FieldForm::IsAccessor);
        if (!isPrivateNameInLink(program, listed.key))
            values.append(OwnValue { listed.key, field(reference) });
    });
    if (listsAccessor)
        return { };
    values.append(OwnValue { { }, joinedExtraProperties(layout) });
    values.append(OwnValue { { }, extraPropertyReadNode(layout) });
    return values;
}

Vector<FlowNodeID, 8> LinkFlow::nodesWithOwnValuesOf(LayoutID layout)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    Vector<FlowNodeID, 8> nodes;
    for (const OwnValue& value : ownValuesOf(layout))
        nodes.append(value.node);
    return nodes;
}

Vector<FlowNodeID, 4> LinkFlow::cellsOfWrittenPrivateNamesOf(LayoutID layout)
{
    Vector<FlowNodeID, 4> cells;
    forEachFieldListedInLinkFlow(program, layout, [&](FieldReference reference) {
        const Field& listed = fieldAt(program, reference);
        if (!isPrivateNameInLink(program, listed.key) || listed.flags.contains(FieldFlags::IsHidden))
            return;
        LayoutID owner = program.at(listed.key).owner;
        if (std::optional<FieldReference> written = owner ? findField(program, owner, listed.key) : std::nullopt)
            cells.append(field(*written));
    });
    return cells;
}

FlowNodeID LinkFlow::appendedField(LayoutID layout, PropertyKeyID key)
{
    bool isNew = false;
    FlowNodeID appended = nodeAskedOnceByLane(MadeOnDemand::AppendedField, static_cast<uint64_t>(layout.index()) << 32 | key.bits(), [&]() -> FlowNodeID {
        RELEASE_ASSERT(key);
        auto added = m_placesOfAppendedFields.add(static_cast<uint64_t>(layout.index()) << 32 | key.bits(), static_cast<uint32_t>(m_appendedFields.size()));
        if (!added.isNewEntry)
            return m_appendedFields[added.iterator->value].node;

        isNew = true;
        FlowNodeID node = makeNode();
        m_appendedFields.append(AppendedField { key, node, std::exchange(m_newestAppendedFieldOfLayout[layout.index()], static_cast<uint32_t>(m_appendedFields.size())) });
        if (mayHaveAppendedFieldsInLinkFlow(program.at(layout))) {
            bool waitsForRead = hasNothingAbove(layout);
            if (!waitsForRead)
                m_fieldsBornBelowClassesByKey.add(key.bits(), Vector<FieldBornBesideFields, 1>()).iterator->value.append(FieldBornBesideFields { layout, node });
            families.forEachFamilyOf(layout, [&](FamilyID family) {
                if (!m_keepersOfSleepersByName.isEmpty()) {
                    if (auto keepers = m_keepersOfSleepersByName.find(static_cast<uint64_t>(family.index()) << 32 | key.bits()); keepers != m_keepersOfSleepersByName.end()) {
                        for (const FlowObserver& keeper : Vector<FlowObserver, 1>(keepers->value))
                            letKeeperWatch(keeper, node);
                    }
                }
                if (FlowNodeID read = findNodeOnDemand(NodeOnDemand::ReadThroughFamily, family.index(), key))
                    addEdge(node, read);
                else if (waitsForRead)
                    m_appendedFieldsThatWaitForRead.add(static_cast<uint64_t>(family.index()) << 32 | key.bits(), Vector<FlowNodeID, 1>()).iterator->value.append(node);
            });
        }
        return node;
    });
    if (isNew) {
        FlowNodeID joined = extraPropertyStoreNode(layout);
        {
            LockerOfWhatIsShared lockerOfWhatIsShared(*this);
            if (m_layoutsWithJoinedProperties.get(layout.index()))
                addEdge(appended, joined);
        }
        bool isLost = lostLayouts.get(layout.index());
        if (isLost || mayBeNamedByNumber(key) || !mayHaveAppendedFieldsInLinkFlow(program.at(layout)) || m_layoutsWithPooledAppendedFields.get(layout.index()))
            addEdge(appended, foundBesideFieldsUnder(key));
        if (isLost)
            addEdge(foundBesideFieldsUnder(key), appended);
    }
    return appended;
}

FlowNodeID LinkFlow::appendedStoreNode(LayoutID layout, PropertyKeyID key)
{
    FlowNodeID appended = appendedField(layout, key);
    bool isNew = false;
    FlowNodeID stored = nodeAskedOnceByLane(MadeOnDemand::AppendedStoreNode, static_cast<uint64_t>(layout.index()) << 32 | key.bits(), [&]() -> FlowNodeID {
        auto place = m_placesOfAppendedFields.find(static_cast<uint64_t>(layout.index()) << 32 | key.bits());
        RELEASE_ASSERT(place != m_placesOfAppendedFields.end());
        if (FlowNodeID made = m_appendedFields[place->value].layoutStoreNode)
            return made;
        isNew = true;
        FlowNodeID node = makeNode();
        m_appendedFields[place->value].layoutStoreNode = node;
        return node;
    });
    if (isNew)
        addEdge(stored, appended);
    return stored;
}

bool LinkFlow::somethingIsStoredBesideFieldsOf(LayoutID layout)
{
    for (uint32_t place = m_newestAppendedFieldOfLayout[layout.index()]; place != UINT32_MAX; place = m_appendedFields[place].next) {
        if (m_appendedFields[place].layoutStoreNode || !typeAt(m_appendedFields[place].node).isNever())
            return true;
    }
    return false;
}

FlowNodeID LinkFlow::findAppendedStoreNode(LayoutID layout, PropertyKeyID key) const
{
    auto found = m_placesOfAppendedFields.find(static_cast<uint64_t>(layout.index()) << 32 | key.bits());
    return found == m_placesOfAppendedFields.end() ? FlowNodeID() : m_appendedFields[found->value].layoutStoreNode;
}

FlowNodeID LinkFlow::findAppendedField(LayoutID layout, PropertyKeyID key) const
{
    auto found = m_placesOfAppendedFields.find(static_cast<uint64_t>(layout.index()) << 32 | key.bits());
    return found == m_placesOfAppendedFields.end() ? FlowNodeID() : m_appendedFields[found->value].node;
}

bool LinkFlow::poolAppendedFields()
{
    BitVector fresh = lostLayouts;
    fresh.merge(escapedLayouts);
    fresh.exclude(m_layoutsWithPooledAppendedFields);
    if (fresh.isEmpty())
        return false;
    m_layoutsWithPooledAppendedFields.merge(fresh);
    bool hasTied = false;
    for (size_t layout : fresh) {
        for (uint32_t place = m_newestAppendedFieldOfLayout[layout]; place != UINT32_MAX;) {
            AppendedField field = m_appendedFields[place];
            place = field.next;
            addEdge(field.node, foundBesideFieldsUnder(field.key));
            hasTied = true;
        }
    }
    return hasTied;
}

bool LinkFlow::tieAppendedFieldsOfLostLayouts()
{
    BitVector fresh = lostLayouts;
    fresh.exclude(m_lostLayoutsWhoseAppendedFieldsAreTied);
    if (fresh.isEmpty())
        return false;
    m_lostLayoutsWhoseAppendedFieldsAreTied.merge(fresh);
    bool hasTied = false;
    for (size_t layout : fresh) {
        if (LayoutID lost(static_cast<uint32_t>(layout)); mayHaveAppendedFieldsInLinkFlow(program.at(lost))) {
            families.forEachFamilyOf(lost, [&](FamilyID family) {
                hasTied |= noteLostMemberWithNothingAbove(family, lost);
                hasTied |= tellPorterThatMemberIsLost(family, lost);
            });
        }
        for (uint32_t place = m_newestAppendedFieldOfLayout[layout]; place != UINT32_MAX;) {
            AppendedField field = m_appendedFields[place];
            place = field.next;
            addEdge(foundBesideFieldsUnder(field.key), field.node);
            hasTied = true;
        }
    }
    return hasTied;
}

FlowNodeID LinkFlow::foundOnClosuresOf(FunctionID function)
{
    return nodeAskedOnceByLane(MadeOnDemand::FoundOnClosuresOf, function.index(), [&]() -> FlowNodeID {
        RELEASE_ASSERT(function.index() < program.functions.size());
        if (m_nodesFoundOnClosures.isEmpty())
            m_nodesFoundOnClosures.fill(noNodeInLinkFlow, program.functions.size());
        if (m_nodesFoundOnClosures[function.index()] == noNodeInLinkFlow) {
            FlowNodeID node = makeNode();
            m_nodesFoundOnClosures[function.index()] = node.index();
            if (escapedFunctions.get(function.index()))
                arrive(node, LinkType::anything().asEscapeOnly());
        }
        return FlowNodeID(m_nodesFoundOnClosures[function.index()]);
    });
}

FlowNodeID LinkFlow::foundOnClassObjectsFrom(LayoutID first)
{
    FlowNodeID found;
    FlowNodeID below;
    size_t steps = 0;
    for (LayoutID current = first; current && steps < program.layouts.size(); current = program.at(current).base, ++steps) {
        bool isMadeHere = false;
        FlowNodeID node = nodeAskedOnceByLane(MadeOnDemand::FoundOnClassObjectsFrom, current.index(), [&]() -> FlowNodeID {
            if (m_nodesFoundOnClassObjectsFrom.isEmpty())
                m_nodesFoundOnClassObjectsFrom.fill(noNodeInLinkFlow, program.layouts.size());
            if (m_nodesFoundOnClassObjectsFrom[current.index()] == noNodeInLinkFlow) {
                m_nodesFoundOnClassObjectsFrom[current.index()] = makeNode().index();
                isMadeHere = true;
            }
            return FlowNodeID(m_nodesFoundOnClassObjectsFrom[current.index()]);
        });
        if (!found)
            found = node;
        if (below)
            addEdge(node, below);
        if (!isMadeHere)
            break;
        if (FunctionID constructor = program.at(current).constructor)
            addEdge(foundOnClosuresOf(constructor), node);
        below = node;
    }
    return found;
}

FlowNodeID LinkFlow::constructorChainStoreNode(LayoutID first, PropertyKeyID key)
{
    FlowNodeID found;
    FlowNodeID below;
    for (LayoutID current = m_nearestConstructorFunctionsFrom[first.index()]; current;) {
        auto [node, isMadeHere] = nodeOnDemand(NodeOnDemand::ConstructorChainStoreNode, current.index(), key);
        if (!found)
            found = node;
        if (below)
            addEdge(node, below);
        if (!isMadeHere)
            break;
        FunctionID holder = program.at(current).constructor;
        addEdge(followsNamesOnClosuresOf(holder, key) ? laidOnClosuresOf(holder, key) : foundBesideFieldsUnder(key), node);
        below = node;
        LayoutID base = program.at(current).base;
        current = base ? m_nearestConstructorFunctionsFrom[base.index()] : LayoutID();
    }
    return found;
}

void LinkFlow::openClosuresOf(FunctionID function)
{
    if (!m_nodesFoundOnClosures.isEmpty() && m_nodesFoundOnClosures[function.index()] != noNodeInLinkFlow)
        arrive(FlowNodeID(m_nodesFoundOnClosures[function.index()]), LinkType::anything());
}

void LinkFlow::open(LayoutID layout)
{
    if (!m_openLayouts.add(layout.index()))
        return;
    UnknownByEscapeAlone unknownByEscapeAlone(true);
    size_t index = 2 * static_cast<size_t>(layout.index());
    addEdge(unseen(), FlowNodeID(m_extraPropertyNodes[index]));
    if (uint32_t underNames = m_nonNumericKeyReadNodes[layout.index()]; underNames != noNodeInLinkFlow)
        addEdge(unseen(), FlowNodeID(underNames));
    FlowNodeID joined(m_extraPropertyNodes[index + 1]);
    joinExtraProperties(layout, joined);
    addEdge(joined, unseen());
}

bool LinkFlow::openLayoutsThatChanged()
{
    bool hasOpened = false;
    for (size_t position = 0; position < m_extraPropertyNodeLayouts.size(); ++position) {
        LayoutID layout = m_extraPropertyNodeLayouts[position];
        if (m_openLayouts.get(layout.index()))
            continue;
        if (!escapedLayouts.get(layout.index()) && !layoutsThatGainWhatUnseenCodeGaveSources.get(layout.index()) && !(lostLayouts.get(layout.index()) ? possibleChanges : changesByMoreThanStoresInSight)[layout.index()].containsAny({ LayoutChange::GainsProperty, LayoutChange::ChangesPrototype, LayoutChange::GetsAccessor })) {
            for (uint32_t node : { m_extraPropertyNodes[2 * static_cast<size_t>(layout.index())], m_nonNumericKeyReadNodes[layout.index()] }) {
                if (!layoutsThatGetNullAbove.get(layout.index()) || node == noNodeInLinkFlow || typeAt(FlowNodeID(node)).primitives & Primitive::Undefined)
                    continue;
                arrive(FlowNodeID(node), fromTypeID(TypeTable::undefined));
                hasOpened = true;
            }
            continue;
        }
        open(layout);
        hasOpened = true;
    }
    return hasOpened;
}

bool LinkFlow::tieStoresUnderUnknownNames()
{
    if (m_storesUnderUnknownNamesToTie.isEmpty())
        return false;
    Vector<std::pair<uint32_t, uint32_t>> waiting = std::exchange(m_storesUnderUnknownNamesToTie, { });
    std::ranges::sort(waiting);
    bool hasTied = false;
    for (auto [layout, aggregate] : waiting) {
        if (m_openLayouts.get(layout))
            continue;
        addValueEdge(FlowNodeID(aggregate & ~keyOfStoreToTieIsNumber), unknownKeyStoreNode(LayoutID(layout), aggregate & keyOfStoreToTieIsNumber));
        hasTied = true;
    }
    return hasTied;
}

void LinkFlow::tieAccessorThatNobodyKnows(FlowNodeID aggregate, LayoutID receiver, FlowNodeID holder, AggregateAccess access)
{
    addEdge(holder, unseen());
    tieToUnseen(aggregate, receiver, access);
}

void LinkFlow::tieToUnseen(FlowNodeID aggregate, LayoutID layout, AggregateAccess access)
{
    deferEscape(layout);
    switch (access) {
    case AggregateAccess::Read:
        addEdge(unseen(), aggregate);
        break;
    case AggregateAccess::Store:
        addEdge(aggregate, unseen());
        break;
    }
    settle();
}

void LinkFlow::tieAccessor(FlowNodeID aggregate, LayoutID receiver, const MemberOfClass& accessor, AggregateAccess access)
{
    FunctionID id = accessor.function;
    auto canBeEntered = [&] {
        if (!id)
            return false;
        const Function& function = program.at(id);
        return (function.kind == FunctionKind::Getter || function.kind == FunctionKind::Setter) && function.hasBody() && !summaries.at(id).has(SummaryTrait::UsesArguments);
    };
    if (!canBeEntered()) {
        tieAccessorThatNobodyKnows(aggregate, receiver, member(accessor.classLayout, accessor.side, accessor.index), access);
        return;
    }

    const Function& function = program.at(id);
    reach(id);
    uint32_t firstPositional = function.firstParameterIndex();
    uint32_t numberOfPositional = function.numberOfPositionalParameters();
    uint32_t numberOfParameters = function.numberOfEntryParameters();
    for (uint32_t index = 0; index < numberOfParameters; ++index) {
        FlowNodeID node = parameter(id, index);
        switch (function.entryParameterKind(index)) {
        case EntryParameter::Capture:
            break;
        case EntryParameter::This:
            arriveFromSource(node, receiver);
            break;
        case EntryParameter::Callee:
            arrive(node, typeOfClosureInLinkFlow(id));
            break;
        case EntryParameter::NewTarget:
            arrive(node, typeOfPrimitivesInLinkFlow(Primitive::Undefined));
            break;
        case EntryParameter::ArgumentCount:
            arrive(node, typeOfPrimitivesInLinkFlow(Primitive::Integer));
            break;
        case EntryParameter::Parameter:
            if (index - firstPositional >= numberOfPositional) {
                arrive(node, LinkType::anything());
                if (access == AggregateAccess::Store)
                    addEdge(aggregate, unseen());
            } else if (access == AggregateAccess::Store && index == firstPositional)
                addEdge(aggregate, node);
            else
                arrive(node, typeOfPrimitivesInLinkFlow(Primitive::Undefined));
            break;
        case EntryParameter::HiddenRest:
            arrive(node, LinkType::anything());
            if (access == AggregateAccess::Store)
                addEdge(aggregate, unseen());
            break;
        }
    }

    if (access == AggregateAccess::Store)
        return;
    if (!function.isCoroutine() && function.results.size() == 1) {
        addEdge(result(id, 0), aggregate);
        return;
    }
    for (uint32_t index = 0; index < function.results.size(); ++index)
        addEdge(result(id, index), unseen());
    arrive(aggregate, LinkType::anything());
}

bool LinkFlow::tieRead(FlowNodeID aggregate, LayoutID layout, PropertyKeyID key)
{
    if (!keepsValuesInFieldsInLinkFlow(program.at(layout).kind)) {
        tieToUnseen(aggregate, layout, AggregateAccess::Read);
        return false;
    }

    std::optional<FieldReference> listed = findField(program, layout, key);
    if (listed) {
        if (fieldAt(program, *listed).form.contains(FieldForm::IsAccessor))
            tieAccessorThatNobodyKnows(aggregate, layout, field(*listed), AggregateAccess::Read);
        else {
            addEdge(field(*listed), aggregate);
            arriveWhatIsEstablishedInLinkFlow(*this, aggregate, fieldAt(program, *listed));
        }
    }

    if (FlowNodeID foundOnPrototypes = program.at(key).kind == PropertyKeyKind::PrivateName ? FlowNodeID() : foundOnPrototypesAbove(layout, key))
        addEdge(foundOnPrototypes, aggregate);

    std::span<const MemberOfClass> members = membersFoundUnder(layout, ClassSide::Prototype, key);
    bool findsGetter = std::ranges::contains(members, MethodKind::Getter, &MemberOfClass::kind);
    for (const MemberOfClass& found : members) {
        switch (found.kind) {
        case MethodKind::Method:
            addEdge(member(found.classLayout, ClassSide::Prototype, found.index), aggregate);
            break;
        case MethodKind::Getter:
            tieAccessor(aggregate, layout, found, AggregateAccess::Read);
            break;
        case MethodKind::Setter:
            if (!findsGetter)
                arrive(aggregate, typeOfPrimitivesInLinkFlow(Primitive::Undefined));
            break;
        }
    }

    bool goesOnAboveClasses = lookupGoesOnAboveClasses(layout, members);
    if (goesOnAboveClasses && listed && listed->layout == layout && isAbsentFromObjectPrototype(key))
        addEdge(foundWhereFieldIsEmpty(*listed), aggregate);
    else if (goesOnAboveClasses) {
        LinkType above = whatLiesAboveClasses(layout, key);
        if (above.hasUnknownObjects && above.functions == LinkType::Functions::None && nameClassOf(key) != NameClass::Proto)
            deferEscape(layout);
        if (above.builtin)
            note(Note::BuiltinReadThroughAggregate, above.builtin.index());
        arrive(aggregate, above);
        if (nameClassOf(key) == NameClass::Constructor)
            arriveConstructorsOfRowAboveInLinkFlow(*this, aggregate, layout);
    }
    return !!listed;
}

bool LinkFlow::tieStore(FlowNodeID aggregate, LayoutID layout, PropertyKeyID key, Lays lays)
{
    if (!keepsValuesInFieldsInLinkFlow(program.at(layout).kind)) {
        tieToUnseen(aggregate, layout, AggregateAccess::Store);
        return false;
    }
    if (namesMayBeIncomplete(layout, ClassSide::Prototype))
        deferEscape(layout);
    if (program.at(key).kind != PropertyKeyKind::PrivateName)
        lookForSetterOnPrototypesAbove(layout, key);

    std::optional<FieldReference> listed = findField(program, layout, key);
    if (listed) {
        if (fieldAt(program, *listed).form.contains(FieldForm::IsAccessor))
            tieAccessorThatNobodyKnows(aggregate, layout, field(*listed), AggregateAccess::Store);
        else {
            switch (lays) {
            case Lays::Assigns:
            case Lays::Defines:
                note(Note::FieldWrittenAfterConstruction, m_firstFieldOfLayout[listed->layout.index()] + listed->field);
                break;
            case Lays::AssignsInConstruction:
            case Lays::DefinesInConstruction:
                break;
            }
            addEdge(aggregate, field(*listed));
        }
    }

    std::span<const MemberOfClass> members = membersFoundUnder(layout, ClassSide::Prototype, key);
    for (const MemberOfClass& found : members) {
        switch (found.kind) {
        case MethodKind::Method:
            if (!listed)
                addEdge(aggregate, member(found.classLayout, ClassSide::Prototype, found.index));
            break;
        case MethodKind::Getter:
            switch (lays) {
            case Lays::Assigns:
            case Lays::AssignsInConstruction:
                break;
            case Lays::DefinesInConstruction:
            case Lays::Defines:
                if (!listed)
                    deferEscape(layout);
                break;
            }
            break;
        case MethodKind::Setter:
            switch (lays) {
            case Lays::Assigns:
            case Lays::AssignsInConstruction:
                tieAccessor(aggregate, layout, found, AggregateAccess::Store);
                break;
            case Lays::DefinesInConstruction:
            case Lays::Defines:
                if (!listed)
                    deferEscape(layout);
                break;
            }
            break;
        }
    }

    if (lookupGoesOnAboveClasses(layout, members) && !isAbsentFromObjectPrototype(key))
        tieToUnseen(aggregate, layout, AggregateAccess::Store);
    settle();
    return !!listed;
}

void LinkFlow::tieReadOfAnyField(FlowNodeID aggregate, LayoutID layout, bool keyIsNumber)
{
    if (!keepsValuesInFieldsInLinkFlow(program.at(layout).kind)) {
        tieToUnseen(aggregate, layout, AggregateAccess::Read);
        return;
    }

    forEachFieldListedInLinkFlow(program, layout, [&](FieldReference reference) {
        if (isPrivateNameInLink(program, fieldAt(program, reference).key) || (keyIsNumber && !mayBeNamedByNumber(fieldAt(program, reference).key)))
            return;
        if (fieldAt(program, reference).form.contains(FieldForm::IsAccessor))
            tieAccessorThatNobodyKnows(aggregate, layout, field(reference), AggregateAccess::Read);
        else {
            addEdge(field(reference), aggregate);
            arriveWhatIsEstablishedInLinkFlow(*this, aggregate, fieldAt(program, reference));
        }
    });
    for (RowOfBasesInLinkFlow row(program, layout); row.current(); row.advance()) {
        for (const MemberOfClass& listed : membersOf(row.current(), ClassSide::Prototype)) {
            if (keyIsNumber && !mayBeNamedByNumber(listed.key))
                continue;
            switch (listed.kind) {
            case MethodKind::Method:
                addEdge(member(listed.classLayout, ClassSide::Prototype, listed.index), aggregate);
                break;
            case MethodKind::Getter:
                tieAccessor(aggregate, layout, listed, AggregateAccess::Read);
                break;
            case MethodKind::Setter:
                break;
            }
        }
    }
    if (keyIsNumber) {
        passPrototypesAboveUnderNumbers(layout);
        arrive(aggregate, typeOfPrimitivesInLinkFlow(Primitive::Undefined));
        return;
    }
    if (program.isSomeNameOpenedOn(program.builtins().canonicalID(Builtins::ObjectPrototype)))
        deferEscape(layout);
    addEdge(joinedExtraProperties(layout), aggregate);
    if (FlowNodeID foundOnPrototypes = foundOnPrototypesAbove(layout, { }))
        addEdge(foundOnPrototypes, aggregate);
    arrive(aggregate, whatLiesAboveClasses(layout, { }));
    arriveConstructorsOfRowAboveInLinkFlow(*this, aggregate, layout);
}

bool LinkFlow::tieReadUnderAffixes(FlowNodeID value, LayoutID layout, const KeyAffixes& affixes)
{
    const Layout& record = program.at(layout);
    if ((record.kind != LayoutKind::Record && record.kind != LayoutKind::Instance) || record.has(LayoutFlags::IsOfCreatedObject))
        return false;
    if (namesMayBeIncomplete(layout, ClassSide::Prototype) || isBelowFollowedPrototype(layout) || isBelowConstructorFunction(layout))
        return false;
    for (RowOfBasesInLinkFlow row(program, layout); row.current(); row.advance()) {
        const Layout& current = program.at(row.current());
        if (current.writtenBuiltinBase || isOfConstructorFunction(row.current()) || (!current.base && current.has(LayoutFlags::HasHeritage)))
            return false;
    }
    const BuiltinTable& builtins = program.builtins();
    BuiltinID objectPrototype = builtins.canonicalID(Builtins::ObjectPrototype);
    if (program.isSomeNameOpenedOn(objectPrototype))
        return false;
    auto mayMatch = [&](PropertyKeyID key) {
        const PropertyKey& name = program.at(key);
        return name.kind == PropertyKeyKind::Text && affixes.mayMatch(program.string(name.text));
    };
    for (BuiltinID member : builtins.membersOf(objectPrototype)) {
        if (BuiltinName name = builtins.at(member).name; name && mayMatch(program.keyOf(name)))
            return false;
    }

    bool mayRunAccessor = false;
    forEachFieldListedInLinkFlow(program, layout, [&](FieldReference reference) {
        mayRunAccessor |= fieldAt(program, reference).form.contains(FieldForm::IsAccessor) && mayMatch(fieldAt(program, reference).key);
    });
    for (RowOfBasesInLinkFlow row(program, layout); row.current(); row.advance()) {
        for (const MemberOfClass& listed : membersOf(row.current(), ClassSide::Prototype))
            mayRunAccessor |= listed.kind != MethodKind::Method && mayMatch(listed.key);
    }
    if (mayRunAccessor)
        return false;

    forEachFieldListedInLinkFlow(program, layout, [&](FieldReference reference) {
        if (!mayMatch(fieldAt(program, reference).key))
            return;
        addEdge(field(reference), value, TypeTable::anything);
        arriveWhatIsEstablishedInLinkFlow(*this, value, fieldAt(program, reference));
    });
    for (RowOfBasesInLinkFlow row(program, layout); row.current(); row.advance()) {
        for (const MemberOfClass& listed : membersOf(row.current(), ClassSide::Prototype)) {
            if (mayMatch(listed.key))
                addEdge(member(listed.classLayout, ClassSide::Prototype, listed.index), value);
        }
    }
    addEdge(extraPropertyReadNode(layout), value);
    addEdge(joinedExtraProperties(layout), value, TypeTable::anything);
    arrive(value, typeOfPrimitivesInLinkFlow(Primitive::Undefined));
    return true;
}

void LinkFlow::tieStoreOfAnyField(FlowNodeID aggregate, LayoutID layout, bool keyIsNumber)
{
    if (!keepsValuesInFieldsInLinkFlow(program.at(layout).kind)) {
        tieToUnseen(aggregate, layout, AggregateAccess::Store);
        return;
    }
    if (namesMayBeIncomplete(layout, ClassSide::Prototype))
        deferEscape(layout);
    lookForSetterOnPrototypesAbove(layout, { });
    if (program.isSomeNameOpenedOn(program.builtins().canonicalID(Builtins::ObjectPrototype)))
        deferEscape(layout);

    note(Note::ChangeOfLayout, layout.index(), OptionSet<LayoutChange> { LayoutChange::GainsProperty, LayoutChange::ChangesPrototype }.toRaw() | changeIsByStoreInSight);
    addValueEdge(aggregate, extraPropertyStoreNode(layout));
    forEachFieldListedInLinkFlow(program, layout, [&](FieldReference reference) {
        if (isPrivateNameInLink(program, fieldAt(program, reference).key) || (keyIsNumber && !mayBeNamedByNumber(fieldAt(program, reference).key)))
            return;
        if (fieldAt(program, reference).form.contains(FieldForm::IsAccessor)) {
            tieAccessorThatNobodyKnows(aggregate, layout, field(reference), AggregateAccess::Store);
            return;
        }
        note(Note::FieldWrittenAfterConstruction, m_firstFieldOfLayout[reference.layout.index()] + reference.field);
        addEdge(aggregate, field(reference));
    });
    for (RowOfBasesInLinkFlow row(program, layout); row.current(); row.advance()) {
        for (const MemberOfClass& listed : membersOf(row.current(), ClassSide::Prototype)) {
            if (keyIsNumber && !mayBeNamedByNumber(listed.key))
                continue;
            switch (listed.kind) {
            case MethodKind::Method:
                addEdge(aggregate, member(listed.classLayout, ClassSide::Prototype, listed.index));
                break;
            case MethodKind::Getter:
                break;
            case MethodKind::Setter:
                tieAccessor(aggregate, layout, listed, AggregateAccess::Store);
                break;
            }
        }
    }
    settle();
}

bool LinkFlow::hasNothingAbove(LayoutID id)
{
    if (!std::exchange(m_hasListedLayoutsWithNothingAbove, true)) {
        m_layoutsWithNothingAbove.ensureSize(program.layouts.size());
        for (uint32_t index = 0; index < program.layouts.size(); ++index) {
            const Layout& layout = program.layouts[index];
            if (keepsValuesInFieldsInLinkFlow(layout.kind) && !layout.base && !m_layoutsBelowFollowedPrototypes.get(index) && membersOf(LayoutID(index), ClassSide::Prototype).empty())
                m_layoutsWithNothingAbove.quickSet(index);
        }
    }
    return m_layoutsWithNothingAbove.quickGet(id.index());
}

LinkFlow::SortedMembersOfFamily& LinkFlow::sortedMembersOf(FamilyID family)
{
    if (auto known = m_sortedMembersOfFamilies.find(family.index()); known != m_sortedMembersOfFamilies.end())
        return *known->value;

    auto sorted = makeUniqueWithoutFastMallocCheck<SortedMembersOfFamily>();
    Vector<LayoutID> withNothingAbove;
    for (LayoutID id : program.types.listOf(families.layoutsOf(family))) {
        if (!hasNothingAbove(id)) {
            if (program.at(id).kind == LayoutKind::Instance && !program.at(id).has(LayoutFlags::IsOfCreatedObject) && !m_layoutsBelowFollowedPrototypes.get(id.index()) && !namesMayBeIncomplete(id, ClassSide::Prototype)) {
                sorted->instancesOfClasses.append(id);
                sorted->someInstanceOfClassMayHaveAppendedFields |= mayHaveAppendedFieldsInLinkFlow(program.at(id));
                if (const Layout& layout = program.at(id); layout.has(LayoutFlags::IsNeverAllocated) || layout.has(LayoutFlags::IsWrittenUnderDictionaryType) || builtinClassOfInstances(layout) || layout.kind == LayoutKind::BuiltinResult)
                    sorted->instancesOfClassesThatIndexOfNamesLeavesOut.append(id);
                if (LayoutID base = program.at(id).base; !base || !isMemberOf(family, base))
                    sorted->topmostInstancesOfClasses.append(id);
                else
                    sorted->heirsOfMembers.add(base.index(), Vector<LayoutID, 1>()).iterator->value.append(id);
            } else
                sorted->tiedOneByOne.append(id);
            if (mayHaveAppendedFieldsInLinkFlow(program.at(id)) && m_lostLayoutsWhoseAppendedFieldsAreTied.get(id.index()))
                ++sorted->numberLost;
            continue;
        }
        withNothingAbove.append(id);
        const Layout& layout = program.at(id);
        for (uint32_t index = 0; index < layout.fields.size(); ++index) {
            PropertyKeyID key = layout.fields[index].key;
            if (key && layout.findField(key) == index)
                sorted->membersWithNothingAboveByKey.add(key.bits(), Vector<LayoutID, 1>()).iterator->value.append(id);
        }
        if (mayHaveAppendedFieldsInLinkFlow(layout) && m_lostLayoutsWhoseAppendedFieldsAreTied.get(id.index()))
            ++sorted->numberLost;
    }
    sorted->numberWithNothingAbove = static_cast<uint32_t>(withNothingAbove.size());
    sorted->withNothingAbove = withNothingAbove;
    if (!withNothingAbove.isEmpty() || !sorted->tiedOneByOne.isEmpty() || !sorted->instancesOfClasses.isEmpty()) {
        sorted->foundBesideTheirFields = makeNode();
        for (LayoutID id : withNothingAbove)
            addEdge(extraPropertyReadNode(id), sorted->foundBesideTheirFields);
        for (LayoutID id : sorted->tiedOneByOne)
            addEdge(extraPropertyReadNode(id), sorted->foundBesideTheirFields);
        for (LayoutID id : sorted->instancesOfClasses)
            addEdge(extraPropertyReadNode(id), sorted->foundBesideTheirFields);
    }
    return *m_sortedMembersOfFamilies.add(family.index(), WTF::move(sorted)).iterator->value;
}

std::span<const LayoutID> LinkFlow::instancesOfClassesIn(FamilyID family)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    return sortedMembersOf(family).instancesOfClasses.span();
}

bool LinkFlow::listMembersThatMayAnswerUnder(FamilyID family, PropertyKeyID key, Vector<LayoutID>& members)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (!key || !isAbsentFromObjectPrototype(key) || nameClassOf(key) == NameClass::Proto || program.at(key).kind == PropertyKeyKind::PrivateName || classHolderIsMadeWithName(program, ClassSide::Prototype, key))
        return false;
    if (mayBeNamedByNumber(key))
        return false;
    const SortedMembersOfFamily& sorted = sortedMembersOf(family);
    std::span<const DeclarerOfMember> declarers = classesThatDeclareOnPrototype(key);
    if (sorted.instancesOfClasses.isEmpty() || program.layoutsWithName(key).size() + declarers.size() > sorted.instancesOfClasses.size())
        return false;
    for (LayoutID listing : program.layoutsWithName(key)) {
        if (isMemberOf(family, listing))
            return false;
    }
    for (LayoutID leftOut : sorted.instancesOfClassesThatIndexOfNamesLeavesOut) {
        if (findField(program, leftOut, key))
            return false;
    }
    for (LayoutID topmost : sorted.topmostInstancesOfClasses) {
        if (findField(program, topmost, key) || !membersFoundUnder(topmost, ClassSide::Prototype, key).empty())
            return false;
    }
    members.appendVector(sorted.withNothingAbove);
    members.appendVector(sorted.tiedOneByOne);
    Vector<LayoutID, 8> atOrBelowDeclarers;
    for (const DeclarerOfMember& declarer : declarers) {
        if (isMemberOf(family, declarer.classLayout))
            atOrBelowDeclarers.append(declarer.classLayout);
    }
    while (!atOrBelowDeclarers.isEmpty()) {
        LayoutID member = atOrBelowDeclarers.takeLast();
        members.append(member);
        if (auto heirs = sorted.heirsOfMembers.find(member.index()); heirs != sorted.heirsOfMembers.end())
            atOrBelowDeclarers.appendVector(heirs->value);
    }
    std::ranges::sort(members);
    members.shrink(std::ranges::unique(members).begin() - members.begin());
    return true;
}

Vector<LinkFlow::FieldBornOnMember> LinkFlow::letKeeperHearOfFieldsBornUnder(FamilyID family, PropertyKeyID key, const FlowObserver& keeper)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    auto& keepers = m_keepersOfSleepersByName.add(static_cast<uint64_t>(family.index()) << 32 | key.bits(), Vector<FlowObserver, 1>()).iterator->value;
    if (!keepers.contains(keeper))
        keepers.append(keeper);
    return fieldsBornOnMembersUnder(family, key);
}

Vector<LinkFlow::FieldBornOnMember> LinkFlow::fieldsBornOnMembersUnder(FamilyID family, PropertyKeyID key)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    Vector<FieldBornOnMember> bornAlready;
    if (auto born = m_fieldsBornBelowClassesByKey.find(key.bits()); born != m_fieldsBornBelowClassesByKey.end()) {
        for (const FieldBornBesideFields& field : born->value) {
            if (isMemberOf(family, field.layout))
                bornAlready.append({ field.layout, field.node });
        }
    }
    return bornAlready;
}

bool LinkFlow::noteLostMemberWithNothingAbove(FamilyID family, LayoutID lost)
{
    auto known = m_sortedMembersOfFamilies.find(family.index());
    if (known == m_sortedMembersOfFamilies.end())
        return false;
    SortedMembersOfFamily& sorted = *known->value;
    ++sorted.numberLost;
    return sorted.readsWithoutLostMember.removeAllMatching([&](const ReadThroughFamily& read) {
        if (program.at(lost).findField(read.key))
            return false;
        addEdge(foundBesideFieldsUnder(read.key), read.aggregate);
        return true;
    });
}

bool LinkFlow::isMemberOf(FamilyID family, LayoutID layout) const
{
    bool isMember = false;
    families.forEachFamilyOf(layout, [&](FamilyID holding) {
        isMember |= holding == family;
    });
    return isMember;
}

std::span<const LinkFlow::DeclarerOfMember> LinkFlow::classesThatDeclareOnPrototype(PropertyKeyID key)
{
    if (!std::exchange(m_hasListedDeclarersOnPrototypes, true)) {
        for (const MemberOfClass& member : m_membersOfClasses) {
            if (member.side == ClassSide::Prototype)
                m_declarersOnPrototypesByKey.append(DeclarerOfMember { member.key.bits(), member.classLayout, member.kind });
        }
        std::ranges::stable_sort(m_declarersOnPrototypesByKey, { }, &DeclarerOfMember::key);
    }
    auto found = std::ranges::equal_range(m_declarersOnPrototypesByKey, key.bits(), { }, &DeclarerOfMember::key);
    return { found.begin(), found.end() };
}

bool LinkFlow::tieReadThroughClassesThatDeclare(const SortedMembersOfFamily& sorted, FlowNodeID aggregate, FamilyID family, PropertyKeyID key)
{
    if (classHolderIsMadeWithName(program, ClassSide::Prototype, key))
        return false;
    if (AOT::hasFlag(build.options().testing, Wire::TestingSwitch::DisableFamilyReadShortcut))
        return false;
    std::span<const DeclarerOfMember> declarers = classesThatDeclareOnPrototype(key);
    if (program.layoutsWithName(key).size() + declarers.size() > sorted.instancesOfClasses.size())
        return false;
    for (LayoutID listing : program.layoutsWithName(key)) {
        if (isMemberOf(family, listing))
            return false;
    }
    for (LayoutID leftOut : sorted.instancesOfClassesThatIndexOfNamesLeavesOut) {
        if (findField(program, leftOut, key))
            return false;
    }
    for (const DeclarerOfMember& declarer : declarers) {
        if (declarer.kind != MethodKind::Method && isMemberOf(family, declarer.classLayout))
            return false;
    }
    for (LayoutID topmost : sorted.topmostInstancesOfClasses) {
        if (findField(program, topmost, key) || std::ranges::any_of(membersFoundUnder(topmost, ClassSide::Prototype, key), [](const MemberOfClass& found) { return found.kind != MethodKind::Method; }))
            return false;
    }

    build.statistics().count(StatisticID::FamilyReadsLinkedToDeclarers);
    for (LayoutID topmost : sorted.topmostInstancesOfClasses)
        tieRead(aggregate, topmost, key);
    LayoutID last;
    for (const DeclarerOfMember& declarer : declarers) {
        if (declarer.classLayout != std::exchange(last, declarer.classLayout) && isMemberOf(family, declarer.classLayout) && !m_layoutsBelowFollowedPrototypes.get(declarer.classLayout.index()))
            tieRead(aggregate, declarer.classLayout, key);
    }
    if (auto born = m_fieldsBornBelowClassesByKey.find(key.bits()); born != m_fieldsBornBelowClassesByKey.end()) {
        for (const FieldBornBesideFields& field : born->value) {
            if (isMemberOf(family, field.layout))
                addEdge(field.node, aggregate);
        }
    }
    return true;
}

void LinkFlow::tieReadThroughSortedMembers(FlowNodeID aggregate, FamilyID family, PropertyKeyID key)
{
    SortedMembersOfFamily& sorted = sortedMembersOf(family);
    bool someMayHoldItBesideTheirFields = false;
    uint32_t numberThatList = 0;
    uint32_t numberLostThatList = 0;
    auto tieOne = [&](LayoutID layout) {
        bool mayHaveAppendedFields = mayHaveAppendedFieldsInLinkFlow(program.at(layout));
        if (FlowNodeID born = mayHaveAppendedFields ? findAppendedField(layout, key) : FlowNodeID())
            addEdge(born, aggregate);
        if (tieRead(aggregate, layout, key))
            numberLostThatList += mayHaveAppendedFields && m_lostLayoutsWhoseAppendedFieldsAreTied.get(layout.index());
        else
            someMayHoldItBesideTheirFields |= mayHaveAppendedFields;
    };
    for (LayoutID layout : sorted.tiedOneByOne)
        tieOne(layout);
    if (!sorted.instancesOfClasses.isEmpty() && tieReadThroughClassesThatDeclare(sorted, aggregate, family, key))
        someMayHoldItBesideTheirFields |= sorted.someInstanceOfClassMayHaveAppendedFields;
    else {
        for (LayoutID layout : sorted.instancesOfClasses)
            tieOne(layout);
    }
    for (FlowNodeID appended : m_appendedFieldsThatWaitForRead.take(static_cast<uint64_t>(family.index()) << 32 | key.bits()))
        addEdge(appended, aggregate);
    if (sorted.foundBesideTheirFields)
        addEdge(sorted.foundBesideTheirFields, aggregate);
    if (auto listing = sorted.membersWithNothingAboveByKey.find(key.bits()); listing != sorted.membersWithNothingAboveByKey.end()) {
        for (LayoutID layout : listing->value) {
            tieRead(aggregate, layout, key);
            ++numberThatList;
            numberLostThatList += mayHaveAppendedFieldsInLinkFlow(program.at(layout)) && m_lostLayoutsWhoseAppendedFieldsAreTied.get(layout.index());
        }
    }
    if (numberThatList != sorted.numberWithNothingAbove)
        arrive(aggregate, typeOfPrimitivesInLinkFlow(Primitive::Undefined));
    else if (!someMayHoldItBesideTheirFields)
        return;

    if (sorted.numberLost > numberLostThatList)
        addEdge(foundBesideFieldsUnder(key), aggregate);
    else
        sorted.readsWithoutLostMember.append(ReadThroughFamily { key, aggregate });
}

std::optional<LinkFlow::GuardedFamilyRead> LinkFlow::findGuardedFamilyPropertyRead(FamilyID family, PropertyKeyID key, LayoutID tested)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (!isAbsentFromObjectPrototype(key) || nameClassOf(key) == NameClass::Proto || program.at(key).kind == PropertyKeyKind::PrivateName || mayBeNamedByNumber(key))
        return std::nullopt;
    SortedMembersOfFamily& sorted = sortedMembersOf(family);
    if (!sorted.hasOnlyWrittenClasses) {
        auto isOfWrittenClass = [&](LayoutID id) {
            const Layout& layout = program.at(id);
            return layout.constructor && isClassConstructorOf(program.at(layout.constructor).kind) && (!layout.has(LayoutFlags::HasHeritage) || layout.base);
        };
        bool isWritten = sorted.tiedOneByOne.isEmpty() && sorted.withNothingAbove.isEmpty() && std::ranges::all_of(sorted.instancesOfClasses, isOfWrittenClass);
        for (LayoutID topmost : sorted.topmostInstancesOfClasses) {
            for (LayoutID above = program.at(topmost).base; isWritten && above; above = program.at(above).base)
                isWritten = isOfWrittenClass(above);
        }
        sorted.hasOnlyWrittenClasses = isWritten;
        if (isWritten) {
            for (LayoutID member : sorted.instancesOfClasses)
                sorted.placesOfMembers.append(m_stretchesAtAndBelowLayouts[member.index()].first);
            std::ranges::sort(sorted.placesOfMembers);
        }
    }
    if (!*sorted.hasOnlyWrittenClasses)
        return std::nullopt;
    auto added = sorted.membersListingName.add(key.bits(), SortedMembersOfFamily::MembersListingName());
    SortedMembersOfFamily::MembersListingName& listed = added.iterator->value;
    if (added.isNewEntry) {
        auto list = [&](LayoutID id) {
            listed.places.append(m_stretchesAtAndBelowLayouts[id.index()].first);
            if (!listed.fields)
                listed.fields = makeNode();
            UnknownByEscapeAlone ofEscapeAlone(UnknownByEscapeAlone::marksAtWork() == UnknownByEscapeAlone::Marks::OfEscape);
            tieRead(listed.fields, id, key);
        };
        for (LayoutID listing : program.layoutsWithName(key)) {
            if (isMemberOf(family, listing))
                list(listing);
        }
        for (LayoutID leftOut : sorted.instancesOfClassesThatIndexOfNamesLeavesOut) {
            if (findField(program, leftOut, key))
                list(leftOut);
        }
        std::ranges::sort(listed.places);
    }
    auto numberBelowTested = [&](const Vector<uint32_t>& places) {
        const StretchInTreeOfBases& below = m_stretchesAtAndBelowLayouts[tested.index()];
        return std::ranges::lower_bound(places, below.end) - std::ranges::lower_bound(places, below.first);
    };
    auto numberListed = numberBelowTested(listed.places);
    if (!numberListed || numberListed != numberBelowTested(sorted.placesOfMembers))
        return std::nullopt;
    return GuardedFamilyRead { listed.fields, sorted.foundBesideTheirFields };
}

FlowNodeID LinkFlow::fieldReadThrough(FamilyID family, PropertyKeyID key)
{
    return nodeAskedOnceByLane(MadeOnDemand::FieldReadThrough, static_cast<uint64_t>(family.index()) << 32 | key.bits(), [&]() -> FlowNodeID {
        RELEASE_ASSERT(key);
        auto [aggregate, isNew] = nodeOnDemand(NodeOnDemand::ReadThroughFamily, family.index(), key);
        if (!isNew)
            return aggregate;

        UnknownByEscapeAlone ofEscapeAlone(UnknownByEscapeAlone::marksAtWork() == UnknownByEscapeAlone::Marks::OfEscape);
        if (isAbsentFromObjectPrototype(key)) {
            tieReadThroughSortedMembers(aggregate, family, key);
            return aggregate;
        }
        TiesToLay ties { TiesToLay::Members::DeclaredFamily, TiesToLay::Access::Read, aggregate, family.index(), key };
        if (leavesTiesToLanes(ties))
            return aggregate;
        for (LayoutID layout : program.types.listOf(families.layoutsOf(family)))
            layTie(ties, layout);
        return aggregate;
    });
}

FlowNodeID LinkFlow::fieldStoredThrough(FamilyID family, PropertyKeyID key, Lays lays)
{
    RELEASE_ASSERT(family.index() < 1u << 26);
    uint32_t number = static_cast<uint32_t>(lays) << 26 | family.index();
    return nodeAskedOnceByLane(MadeOnDemand::FieldStoredThrough, static_cast<uint64_t>(number) << 32 | key.bits(), [&]() -> FlowNodeID {
        RELEASE_ASSERT(key);
        auto [aggregate, isNew] = nodeOnDemand(NodeOnDemand::LaidThroughFamily, number, key);
        if (!isNew)
            return aggregate;

        UnknownByEscapeAlone ofEscapeAlone(UnknownByEscapeAlone::marksAtWork() == UnknownByEscapeAlone::Marks::OfEscape);
        auto access = [&] {
            switch (lays) {
            case Lays::Assigns:
                return TiesToLay::Access::Assign;
            case Lays::AssignsInConstruction:
                return TiesToLay::Access::AssignInConstruction;
            case Lays::DefinesInConstruction:
                return TiesToLay::Access::DefineInConstruction;
            case Lays::Defines:
                return TiesToLay::Access::Define;
            }
            RELEASE_ASSERT_NOT_REACHED();
        }();
        TiesToLay ties { TiesToLay::Members::DeclaredFamily, access, aggregate, family.index(), key };
        if (leavesTiesToLanes(ties))
            return aggregate;
        for (LayoutID layout : program.types.listOf(families.layoutsOf(family)))
            layTie(ties, layout);
        return aggregate;
    });
}

static uint64_t placeOfLostKeyInLinkFlow(uint32_t representative, PropertyKeyID key)
{
    return static_cast<uint64_t>(representative) << 32 | key.index();
}

uint32_t LinkFlow::recordOfLostKey(uint32_t representative, PropertyKeyID key)
{
    auto added = m_recordsOfLostKeys.add(placeOfLostKeyInLinkFlow(representative, key), static_cast<uint32_t>(m_lostKeys.size()));
    if (added.isNewEntry) {
        LostKey record;
        record.key = key;
        record.family = representative;
        record.next = m_lostFamilies[representative].firstKey;
        NameClass nameClass = nameClassOf(key);
        record.tiesEveryMember = nameClass == NameClass::Constructor || nameClass == NameClass::Proto;
        m_lostFamilies[representative].firstKey = static_cast<uint32_t>(m_lostKeys.size());
        m_lostKeys.append(record);
    }
    return added.iterator->value;
}

Vector<LayoutID> LinkFlow::membersOfLostFamily(uint32_t representative) const
{
    Vector<LayoutID> members;
    unitedLayouts.forEachMemberOfFamilyOf(representative, [&](uint32_t member) {
        members.append(LayoutID(member));
    });
    return members;
}

FlowNodeID LinkFlow::fieldThroughLost(LayoutID member, PropertyKeyID key, TiesToLay::Access access)
{
    RELEASE_ASSERT(key);
    RELEASE_ASSERT(access == TiesToLay::Access::Read || access == TiesToLay::Access::Assign || access == TiesToLay::Access::Define);
    uint32_t LostKey::* place = access == TiesToLay::Access::Read ? &LostKey::read : access == TiesToLay::Access::Assign ? &LostKey::stored : &LostKey::defined;
    if (!member)
        return unseen();
    tieInLostLayouts();
    uint32_t representative = unitedLayouts.representativeOf(member.index());
    uint32_t record = recordOfLostKey(representative, key);
    uint32_t existing = m_lostKeys[record].*place;
    if (existing != noNodeInLinkFlow)
        return FlowNodeID(existing);

    UnknownByEscapeAlone ofEscapeAlone(UnknownByEscapeAlone::marksAtWork() == UnknownByEscapeAlone::Marks::OfEscape);
    FlowNodeID aggregate = makeNode();
    m_keysLookedUpThroughLost.add(key.index());
    bool tiesEveryMember = m_lostKeys[record].tiesEveryMember;
    if (tiesEveryMember && !std::exchange(m_lostKeys[record].isListedAsTyingEveryMember, true))
        m_lostFamilies[representative].keysThatTieEveryMember.append(record);

    bool letsMembersGo = false;
    m_lostKeys[record].*place = aggregate.index();
    if (access == TiesToLay::Access::Read) {
        addEdge(FlowNodeID(m_lostFamilies[representative].foundBesideFields), aggregate);
        addEdge(foundBesideFieldsUnder(key), aggregate);
        if (!tiesEveryMember) {
            LinkType above = whatLiesAboveClasses(member, key);
            letsMembersGo = above.hasUnknownObjects && above.functions == LinkType::Functions::None;
            if (above.builtin)
                note(Note::BuiltinReadThroughAggregate, above.builtin.index());
            arrive(aggregate, above);
        }
    } else {
        m_hasStoresThroughLost = true;
        addValueEdge(aggregate, FlowNodeID(m_lostFamilies[representative].storedBesideFields));
        addValueEdge(aggregate, foundBesideFieldsUnder(key));
        if (!tiesEveryMember && !isAbsentFromObjectPrototype(key)) {
            letsMembersGo = true;
            addEdge(aggregate, unseen());
        }
    }
    if (letsMembersGo) {
        LinkType lost;
        lost.objects = LinkType::Objects::Lost;
        lost.layout = member;
        noteReachesUnseen(lost);
    }

    TiesToLay ties { tiesEveryMember ? TiesToLay::Members::LostFamily : TiesToLay::Members::HoldersOfLostKey, access, aggregate, tiesEveryMember ? representative : m_lostKeys[record].firstHolder, key };
    if (leavesTiesToLanes(ties))
        return aggregate;
    if (tiesEveryMember) {
        for (LayoutID layout : membersOfLostFamily(representative))
            layTie(ties, layout);
        return aggregate;
    }
    for (uint32_t index = ties.number; index != UINT32_MAX;) {
        HolderOfLostKey holder = m_holdersOfLostKeys[index];
        index = holder.next;
        layTie(ties, holder.layout);
    }
    return aggregate;
}

void LinkFlow::layTie(const TiesToLay& ties, LayoutID member)
{
    bool isThroughDeclaredFamily = ties.members == TiesToLay::Members::DeclaredFamily;
    auto lay = [&](Lays lays) {
        bool isListed = tieStore(ties.aggregate, member, ties.key, lays);
        if (isThroughDeclaredFamily && !isListed && mayHaveAppendedFieldsInLinkFlow(program.at(member)))
            addValueEdge(ties.aggregate, appendedField(member, ties.key));
    };
    switch (ties.access) {
    case TiesToLay::Access::ReadOfAnyField:
    case TiesToLay::Access::ReadOfAnyFieldUnderNumbers:
        tieReadOfAnyField(ties.aggregate, member, ties.access == TiesToLay::Access::ReadOfAnyFieldUnderNumbers);
        return;
    case TiesToLay::Access::Read: {
        bool isListed = tieRead(ties.aggregate, member, ties.key);
        if (!isThroughDeclaredFamily)
            return;
        if (!isListed && mayHaveAppendedFieldsInLinkFlow(program.at(member)))
            addEdge(appendedField(member, ties.key), ties.aggregate);
        else if (isListed && mayHaveAppendedFieldsInLinkFlow(program.at(member))) {
            FlowNodeID born;
            {
                LockerOfWhatIsShared lockerOfWhatIsShared(*this);
                born = findAppendedField(member, ties.key);
            }
            if (born)
                addEdge(born, ties.aggregate);
        }
        addEdge(extraPropertyReadNode(member), ties.aggregate);
        return;
    }
    case TiesToLay::Access::Assign:
        lay(Lays::Assigns);
        return;
    case TiesToLay::Access::AssignInConstruction:
        lay(Lays::AssignsInConstruction);
        return;
    case TiesToLay::Access::DefineInConstruction:
        lay(Lays::DefinesInConstruction);
        return;
    case TiesToLay::Access::Define:
        lay(Lays::Defines);
        return;
    }
}

bool LinkFlow::leavesTiesToLanes(TiesToLay ties)
{
    if (!m_severalThreadsTell)
        return false;
    ties.marks = UnknownByEscapeAlone::marksAtWork();
    ties.isInLibraryCode = isInLibraryCode();
    s_laneAtWork->tiesToLay.append(ties);
    return true;
}

void LinkFlow::layTiesThatSettlingLeft()
{
    if (m_tiesLeftBySettling.isEmpty())
        return;
    if (!severalThreadsPayFor(m_membersOfTiesLeftBySettling.size())) {
        Vector<LayoutID> members = std::exchange(m_membersOfTiesLeftBySettling, { });
        for (const TiesToLay& ties : std::exchange(m_tiesLeftBySettling, { })) {
            UnknownByEscapeAlone marksOfWhoeverAsked(ties.marks != UnknownByEscapeAlone::Marks::Nothing, ties.marks);
            bool wasOfLibrary = setIsInLibraryCode(ties.isInLibraryCode);
            for (uint32_t member = ties.number; member < ties.end; ++member)
                layTie(ties, members[member]);
            setIsInLibraryCode(wasOfLibrary);
        }
        return;
    }
    stateOfLinkCalls();
    prepareAccessesForSeveralThreads();
    prepareOperatorsForSeveralThreads();
    m_numberOfNodesMade = m_nodes.size();
    for (auto& lane : m_lanes) {
        lane->given[static_cast<size_t>(MadeOnDemand::TakenForConstant)].clear();
        lane->given[static_cast<size_t>(MadeOnDemand::ContentsOfFamily)].clear();
    }
    forgetWhatSomeLaneWasGiven(MadeOnDemand::TakenForConstant);
    bool toldObservers = std::exchange(m_tellsObservers, true);
    m_severalThreadsTell = true;
    layTiesThatLanesLeft();
    m_severalThreadsTell = false;
    m_tellsObservers = toldObservers;
    takeOverWhatLanesDidWhileTelling();
}

void LinkFlow::layTiesThatLanesLeft()
{
    static constexpr uint32_t membersInStretch = 128;
    struct Stretch {
        uint32_t ties;
        uint32_t first;
        uint32_t end;
        bool isListedBySettling;
    };
    Vector<TiesToLay> all;
    Vector<LayoutID> members;
    Vector<Stretch> stretches;
    HashMap<uint32_t, std::pair<uint32_t, uint32_t>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> membersOfLostFamilies;
    while (true) {
        all.shrink(0);
        Vector<LayoutID> listedBySettling = std::exchange(m_membersOfTiesLeftBySettling, { });
        all.appendVector(std::exchange(m_tiesLeftBySettling, { }));
        size_t numberLeftBySettling = all.size();
        for (auto& lane : m_lanes) {
            all.appendVector(lane->tiesToLay);
            lane->tiesToLay.shrink(0);
        }
        if (all.isEmpty())
            return;
        std::ranges::sort(all.mutableSpan().subspan(numberLeftBySettling), { }, [](const TiesToLay& ties) {
            return std::tuple { ties.members, ties.access, ties.number, ties.key.bits(), ties.aggregate.index() };
        });
        members.shrink(0);
        stretches.shrink(0);
        membersOfLostFamilies.clear();
        for (uint32_t index = 0; index < all.size(); ++index) {
            const TiesToLay& ties = all[index];
            uint32_t first = members.size();
            uint32_t end = first;
            switch (ties.members) {
            case TiesToLay::Members::HoldersOfLostKey:
                for (uint32_t holder = ties.number; holder != UINT32_MAX; holder = m_holdersOfLostKeys[holder].next)
                    members.append(m_holdersOfLostKeys[holder].layout);
                end = members.size();
                break;
            case TiesToLay::Members::LostFamily: {
                auto added = membersOfLostFamilies.add(ties.number, std::pair { first, first });
                if (added.isNewEntry) {
                    members.appendVector(membersOfLostFamily(ties.number));
                    added.iterator->value.second = members.size();
                }
                std::tie(first, end) = added.iterator->value;
                break;
            }
            case TiesToLay::Members::DeclaredFamily:
                members.append(program.types.listOf(families.layoutsOf(FamilyID(ties.number))));
                end = members.size();
                break;
            case TiesToLay::Members::Listed:
                first = ties.number;
                end = ties.end;
                break;
            }
            for (; first < end; first += membersInStretch)
                stretches.append(Stretch { index, first, std::min(end, first + membersInStretch), ties.members == TiesToLay::Members::Listed });
        }
        unsigned numberOfLanes = m_lanes.size();
        runOnLanes([&](unsigned lane) {
            for (size_t place = lane; place < stretches.size(); place += numberOfLanes) {
                const Stretch& stretch = stretches[place];
                const TiesToLay& ties = all[stretch.ties];
                UnknownByEscapeAlone marksOfWhoeverAsked(ties.marks != UnknownByEscapeAlone::Marks::Nothing, ties.marks);
                bool wasOfLibrary = setIsInLibraryCode(ties.isInLibraryCode);
                for (uint32_t member = stretch.first; member < stretch.end; ++member)
                    layTie(ties, stretch.isListedBySettling ? listedBySettling[member] : members[member]);
                setIsInLibraryCode(wasOfLibrary);
            }
        });
    }
}

FlowNodeID LinkFlow::fieldReadThroughLost(LayoutID member, PropertyKeyID key)
{
    if (!member)
        return unseen();
    return nodeAskedOnceByLane(MadeOnDemand::FieldReadThroughLost, static_cast<uint64_t>(unitedLayouts.representativeOf(member.index())) << 32 | key.bits(), [&]() -> FlowNodeID {
        return fieldThroughLost(member, key, TiesToLay::Access::Read);
    });
}

FlowNodeID LinkFlow::fieldStoredThroughLost(LayoutID member, PropertyKeyID key, Lays lays)
{
    if (!member)
        return unseen();
    auto access = [&] {
        switch (lays) {
        case Lays::Assigns:
        case Lays::AssignsInConstruction:
            return TiesToLay::Access::Assign;
        case Lays::DefinesInConstruction:
        case Lays::Defines:
            return TiesToLay::Access::Define;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }();
    return nodeAskedOnceByLane(MadeOnDemand::FieldStoredThroughLost, static_cast<uint64_t>(access == TiesToLay::Access::Define) << 63 | static_cast<uint64_t>(unitedLayouts.representativeOf(member.index())) << 32 | key.bits(), [&]() -> FlowNodeID {
        return fieldThroughLost(member, key, access);
    });
}

uint64_t LinkFlow::wordOfAnyFieldThrough(LinkType::Objects objects, LayoutID layout, FamilyID family, bool keyIsNumber) const
{
    uint32_t number = objects == LinkType::Objects::Family || objects == LinkType::Objects::Prototypes ? family.index() : layout.index();
    if (objects == LinkType::Objects::Lost && layout)
        number = unitedLayouts.representativeOf(layout.index());
    bool isUnderMarksOfRule = UnknownByEscapeAlone::marksAtWork() == UnknownByEscapeAlone::Marks::OfRule;
    return static_cast<uint64_t>(objects) << 34 | static_cast<uint64_t>(isUnderMarksOfRule) << 33 | static_cast<uint64_t>(keyIsNumber) << 32 | number;
}

FlowNodeID LinkFlow::anyFieldReadThrough(LinkType::Objects objects, LayoutID layout, FamilyID family, bool keyIsNumber)
{
    return nodeAskedOnceByLane(MadeOnDemand::AnyFieldReadThrough, wordOfAnyFieldThrough(objects, layout, family, keyIsNumber), [&] {
        return findOrMakeAnyFieldReadThrough(objects, layout, family, keyIsNumber);
    });
}

FlowNodeID LinkFlow::findOrMakeAnyFieldReadThrough(LinkType::Objects objects, LayoutID layout, FamilyID family, bool keyIsNumber)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    bool isUnderMarksOfRule = UnknownByEscapeAlone::marksAtWork() == UnknownByEscapeAlone::Marks::OfRule;
    RELEASE_ASSERT(!keyIsNumber || !isUnderMarksOfRule);
    auto numberInName = [&](uint32_t number) {
        RELEASE_ASSERT(number < underMarksOfRuleAmongNodesOnDemand);
        return isUnderMarksOfRule ? number | underMarksOfRuleAmongNodesOnDemand : number;
    };
    switch (objects) {
    case LinkType::Objects::None:
        return unseen();
    case LinkType::Objects::Layout: {
        auto [aggregate, isNew] = keyIsNumber ? aggregateOfReadsUnderNumbers(m_elementsReadThroughLayouts, program.layouts.size(), layout.index()) : nodeOnDemand(NodeOnDemand::AnyFieldReadOfLayout, numberInName(layout.index()), { });
        if (isNew) {
            if (keyIsNumber)
                addEdge(foundBesideFieldsUnderAnyNumber(), aggregate);
            addEdge(extraPropertyReadNode(layout), aggregate);
            if (keyIsNumber)
                addEdge(elementsSeenOn(layout), aggregate);
            tieReadOfAnyField(aggregate, layout, keyIsNumber);
        }
        return aggregate;
    }
    case LinkType::Objects::Set: {
        auto [aggregate, isNew] = nodeOnDemand(keyIsNumber ? NodeOnDemand::ElementReadOfSet : NodeOnDemand::AnyFieldReadOfSet, numberInName(layout.index()), { });
        if (isNew) {
            for (LayoutID member : m_setsOfLayouts.membersOf(layout.index()))
                addEdge(anyFieldReadThrough(LinkType::Objects::Layout, member, { }, keyIsNumber), aggregate);
        }
        return aggregate;
    }
    case LinkType::Objects::Prototype:
        deferEscape(layout);
        settle();
        return unseen();
    case LinkType::Objects::Prototypes:
        for (LayoutID member : program.types.listOf(families.layoutsOf(family)))
            deferEscape(member);
        settle();
        return unseen();
    case LinkType::Objects::Family: {
        auto [aggregate, isNew] = keyIsNumber ? aggregateOfReadsUnderNumbers(m_elementsReadThroughFamilies, families.size(), family.index()) : nodeOnDemand(NodeOnDemand::AnyFieldReadOfFamily, numberInName(family.index()), { });
        if (isNew) {
            if (keyIsNumber)
                addEdge(foundBesideFieldsUnderAnyNumber(), aggregate);
            for (LayoutID member : program.types.listOf(families.layoutsOf(family))) {
                addEdge(extraPropertyReadNode(member), aggregate);
                if (keyIsNumber)
                    addEdge(elementsSeenOn(member), aggregate);
                tieReadOfAnyField(aggregate, member, keyIsNumber);
            }
        }
        return aggregate;
    }
    case LinkType::Objects::Lost: {
        if (!layout)
            return unseen();
        tieInLostLayouts();
        uint32_t representative = unitedLayouts.representativeOf(layout.index());
        if (keyIsNumber) {
            auto [aggregate, isNew] = aggregateOfReadsUnderNumbers(m_elementsReadThroughLostFamilies, program.layouts.size(), representative);
            if (isNew) {
                addEdge(foundBesideFieldsUnderAnyNumber(), aggregate);
                addEdge(FlowNodeID(m_lostFamilies[representative].foundBesideFields), aggregate);
                for (LayoutID member : membersOfLostFamily(representative))
                    tieReadOfAnyField(aggregate, member, true);
            }
            return aggregate;
        }
        uint32_t& read = isUnderMarksOfRule ? m_lostFamilies[representative].anyFieldReadUnderMarksOfRule : m_lostFamilies[representative].anyFieldRead;
        if (read != noNodeInLinkFlow)
            return FlowNodeID(read);
        FlowNodeID aggregate = makeNode();
        read = aggregate.index();
        m_anyFieldIsReadThroughLost = true;
        addEdge(FlowNodeID(m_lostFamilies[representative].foundBesideFields), aggregate);
        for (LayoutID member : membersOfLostFamily(representative))
            tieReadOfAnyField(aggregate, member);
        return aggregate;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FlowNodeID LinkFlow::anyFieldDefinedOn(LayoutID layout)
{
    return nodeAskedOnceByLane(MadeOnDemand::AnyFieldDefinedOn, layout.index(), [&]() -> FlowNodeID {
        auto [aggregate, isNew] = nodeOnDemand(NodeOnDemand::AnyFieldDefinedOnLayout, layout.index(), { });
        if (!isNew)
            return aggregate;
        addEdge(aggregate, unseen());
        forEachFieldListedInLinkFlow(program, layout, [&](FieldReference reference) {
            if (isPrivateNameInLink(program, fieldAt(program, reference).key))
                return;
            RELEASE_ASSERT(!fieldAt(program, reference).form.contains(FieldForm::IsAccessor));
            note(Note::FieldWrittenAfterConstruction, m_firstFieldOfLayout[reference.layout.index()] + reference.field);
            addEdge(aggregate, field(reference));
        });
        return aggregate;
    });
}

FlowNodeID LinkFlow::anyFieldStoredThrough(LinkType::Objects objects, LayoutID layout, FamilyID family, bool keyIsNumber)
{
    return nodeAskedOnceByLane(MadeOnDemand::AnyFieldStoredThrough, wordOfAnyFieldThrough(objects, layout, family, keyIsNumber), [&] {
        return findOrMakeAnyFieldStoredThrough(objects, layout, family, keyIsNumber);
    });
}

FlowNodeID LinkFlow::findOrMakeAnyFieldStoredThrough(LinkType::Objects objects, LayoutID layout, FamilyID family, bool keyIsNumber)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    UnknownByEscapeAlone ofEscapeAlone(UnknownByEscapeAlone::marksAtWork() == UnknownByEscapeAlone::Marks::OfEscape);
    switch (objects) {
    case LinkType::Objects::None:
        return unseen();
    case LinkType::Objects::Layout: {
        auto [aggregate, isNew] = keyIsNumber ? aggregateOfReadsUnderNumbers(m_elementsStoredThroughLayouts, program.layouts.size(), layout.index()) : nodeOnDemand(NodeOnDemand::AnyFieldStoredOfLayout, layout.index(), { });
        if (isNew) {
            m_storesUnderUnknownNamesToTie.append({ layout.index(), aggregate.index() | (keyIsNumber ? keyOfStoreToTieIsNumber : 0) });
            addValueEdge(aggregate, elementsSeenOn(layout));
            tieStoreOfAnyField(aggregate, layout, keyIsNumber);
        }
        return aggregate;
    }
    case LinkType::Objects::Set: {
        auto [aggregate, isNew] = nodeOnDemand(keyIsNumber ? NodeOnDemand::ElementStoredOfSet : NodeOnDemand::AnyFieldStoredOfSet, layout.index(), { });
        if (isNew) {
            for (LayoutID member : m_setsOfLayouts.membersOf(layout.index()))
                addEdge(aggregate, anyFieldStoredThrough(LinkType::Objects::Layout, member, { }, keyIsNumber));
        }
        return aggregate;
    }
    case LinkType::Objects::Prototype:
        deferEscape(layout);
        noteReachesUnseen(typeOfPrototypeInLinkFlow(layout));
        settle();
        return unseen();
    case LinkType::Objects::Prototypes:
        noteReachesUnseen(typeOfPrototypesInLinkFlow(family));
        settle();
        return unseen();
    case LinkType::Objects::Family: {
        auto [aggregate, isNew] = keyIsNumber ? aggregateOfReadsUnderNumbers(m_elementsStoredThroughFamilies, families.size(), family.index()) : nodeOnDemand(NodeOnDemand::AnyFieldStoredOfFamily, family.index(), { });
        if (isNew) {
            addEdge(aggregate, unseen());
            for (LayoutID member : program.types.listOf(families.layoutsOf(family))) {
                addValueEdge(aggregate, elementsSeenOn(member));
                tieStoreOfAnyField(aggregate, member, keyIsNumber);
                note(Note::ChangeOfLayout, member.index(), OptionSet<LayoutChange> { LayoutChange::GainsProperty, LayoutChange::ChangesPrototype }.toRaw());
            }
        }
        return aggregate;
    }
    case LinkType::Objects::Lost: {
        LinkType lost;
        lost.objects = LinkType::Objects::Lost;
        lost.layout = layout;
        noteReachesUnseen(lost);
        settle();
        return unseen();
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool LinkFlow::tieInLostLayouts()
{
    if (!std::exchange(m_lostLayoutsMayHaveGrown, false) || m_layoutsToTie.isEmpty())
        return false;
    Vector<LayoutID> fresh = std::exchange(m_layoutsToTie, { });
    std::ranges::sort(fresh);
    for (LayoutID layout : fresh)
        tieInLostLayout(layout);
    return true;
}

void LinkFlow::tieInLostLayout(LayoutID layout)
{
    uint32_t family = layout.index();
    RELEASE_ASSERT(unitedLayouts.isAlone(family));
    FlowNodeID found = makeNode();
    FlowNodeID stored = makeNode();
    m_lostFamilies[family].foundBesideFields = found.index();
    m_lostFamilies[family].storedBesideFields = stored.index();
    addEdge(extraPropertyReadNode(layout), found);
    addEdge(stored, extraPropertyStoreNode(layout));
    if (auto told = m_nodesWhereLossIsTold.find(layout.index()); told != m_nodesWhereLossIsTold.end())
        arrive(FlowNodeID(told->value), LinkType::anything());
    if (!keepsValuesInFieldsInLinkFlow(program.at(layout).kind)) {
        deferEscape(layout);
        addEdge(unseen(), found);
        addEdge(stored, unseen());
    }
    tiePrototypesAboveLostLayout(layout);
    catchUpWithFamiliesAbove(layout);
    if (uint32_t place = placeInForestOfFamilies(layout); place != UINT32_MAX)
        m_lostFamilies[family].placesInForestOfFamilies.insert(place);

    Vector<PropertyKeyID, 32> keys;
    forEachFieldListedInLinkFlow(program, layout, [&](FieldReference reference) {
        if (PropertyKeyID key = fieldAt(program, reference).key)
            keys.append(key);
    });
    for (RowOfBasesInLinkFlow row(program, layout); row.current(); row.advance()) {
        for (const MemberOfClass& listed : membersOf(row.current(), ClassSide::Prototype))
            keys.append(listed.key);
    }
    std::ranges::sort(keys);
    removeRepeatedElements(keys, std::equal_to<PropertyKeyID> { });

    for (PropertyKeyID key : keys) {
        uint32_t record = recordOfLostKey(family, key);
        if (m_lostKeys[record].tiesEveryMember)
            continue;
        m_holdersOfLostKeys.append(HolderOfLostKey { layout, UINT32_MAX });
        m_lostKeys[record].firstHolder = m_lostKeys[record].lastHolder = static_cast<uint32_t>(m_holdersOfLostKeys.size() - 1);
    }
}

void LinkFlow::uniteLost(LayoutID one, LayoutID other)
{
    becomeLost(one);
    becomeLost(other);
    tieInLostLayouts();
    uint32_t kept = unitedLayouts.representativeOf(one.index());
    uint32_t absorbed = unitedLayouts.representativeOf(other.index());
    if (kept == absorbed)
        return;
    if (unitedLayouts.sizeOfFamilyOf(kept) < unitedLayouts.sizeOfFamilyOf(absorbed))
        std::swap(kept, absorbed);
    loseFamiliesOfMembersThatMeet(kept, absorbed);
    mergeLostFamilies(kept, absorbed);
    UnitedFamilies::Union united = unitedLayouts.unite(kept, absorbed);
    RELEASE_ASSERT(united.kept == kept && united.absorbed == absorbed);
}

void LinkFlow::loseFamiliesOfMembersThatMeet(uint32_t kept, uint32_t absorbed)
{
    std::set<uint32_t>& larger = m_lostFamilies[kept].placesInForestOfFamilies;
    std::set<uint32_t> smaller = std::exchange(m_lostFamilies[absorbed].placesInForestOfFamilies, { });
    if (smaller.size() > larger.size())
        std::swap(smaller, larger);
    auto loseFamilyOfBoth = [&](uint32_t one, uint32_t other) {
        if (FamilyID common = families.nearestCommonFamily(m_layoutsByPlaceInForestOfFamilies[one], m_layoutsByPlaceInForestOfFamilies[other]))
            deferLoss(common);
    };
    for (uint32_t place : smaller) {
        auto joined = larger.insert(place).first;
        if (joined != larger.begin())
            loseFamilyOfBoth(*std::prev(joined), place);
        if (auto behind = std::next(joined); behind != larger.end())
            loseFamilyOfBoth(place, *behind);
    }
}

uint32_t LinkFlow::placeInForestOfFamilies(LayoutID layout)
{
    if (m_placesInForestOfFamilies.isEmpty()) {
        uint32_t numberOfFamilies = families.size();
        size_t numberOfLayouts = program.layouts.size();
        Vector<uint32_t> numberOfLeaves(FillWith { }, numberOfFamilies, 0u);
        for (size_t index = 0; index < numberOfLayouts; ++index) {
            if (FamilyID nearest = families.nearestFamilyOf(LayoutID(index)))
                ++numberOfLeaves[nearest.index()];
        }
        for (uint32_t family = 0; family < numberOfFamilies; ++family) {
            if (FamilyID above = families.nearestFamilyAbove(FamilyID(family)))
                numberOfLeaves[above.index()] += numberOfLeaves[family];
        }
        Vector<uint32_t> nextPlace(FillWith { }, numberOfFamilies, 0u);
        uint32_t numberOfPlaces = 0;
        for (uint32_t family = numberOfFamilies; family--;) {
            uint32_t& next = families.nearestFamilyAbove(FamilyID(family)) ? nextPlace[families.nearestFamilyAbove(FamilyID(family)).index()] : numberOfPlaces;
            nextPlace[family] = next;
            next += numberOfLeaves[family];
        }
        m_placesInForestOfFamilies.fill(UINT32_MAX, numberOfLayouts);
        m_layoutsByPlaceInForestOfFamilies.fill(LayoutID(), numberOfPlaces);
        for (size_t index = 0; index < numberOfLayouts; ++index) {
            if (FamilyID nearest = families.nearestFamilyOf(LayoutID(index))) {
                uint32_t place = nextPlace[nearest.index()]++;
                m_placesInForestOfFamilies[index] = place;
                m_layoutsByPlaceInForestOfFamilies[place] = LayoutID(index);
            }
        }
    }
    return m_placesInForestOfFamilies[layout.index()];
}

void LinkFlow::mergeLostFamilies(uint32_t kept, uint32_t absorbed)
{
    auto tieToEachOther = [&](uint32_t one, uint32_t other) {
        addEdge(FlowNodeID(one), FlowNodeID(other));
        addEdge(FlowNodeID(other), FlowNodeID(one));
    };
    auto merge = [&](uint32_t ofKept, uint32_t ofAbsorbed, const auto& tieSideOfKept, const auto& tieSideOfAbsorbed) {
        if (ofKept != noNodeInLinkFlow && ofAbsorbed != noNodeInLinkFlow) {
            tieToEachOther(ofKept, ofAbsorbed);
            return ofKept;
        }
        if (ofAbsorbed != noNodeInLinkFlow) {
            tieSideOfKept(FlowNodeID(ofAbsorbed));
            return ofAbsorbed;
        }
        if (ofKept != noNodeInLinkFlow)
            tieSideOfAbsorbed(FlowNodeID(ofKept));
        return ofKept;
    };

    std::optional<Vector<LayoutID>> membersOfKept;
    std::optional<Vector<LayoutID>> membersOfAbsorbed;
    auto forEachMember = [&](std::optional<Vector<LayoutID>>& members, uint32_t representative, const auto& functor) {
        if (!members)
            members = membersOfLostFamily(representative);
        for (LayoutID member : *members)
            functor(member);
    };
    auto forEachHolder = [&](uint32_t first, const auto& functor) {
        for (uint32_t index = first; index != UINT32_MAX;) {
            HolderOfLostKey holder = m_holdersOfLostKeys[index];
            index = holder.next;
            functor(holder.layout);
        }
    };
    bool leavesTies = settlingLeavesTies();
    std::optional<std::pair<uint32_t, uint32_t>> placeOfKept;
    std::optional<std::pair<uint32_t, uint32_t>> placeOfAbsorbed;
    auto leave = [&](TiesToLay::Access access, FlowNodeID aggregate, PropertyKeyID key, std::pair<uint32_t, uint32_t> place) {
        if (place.first == place.second)
            return;
        TiesToLay ties { TiesToLay::Members::Listed, access, aggregate, place.first, key, place.second };
        ties.marks = UnknownByEscapeAlone::marksAtWork();
        ties.isInLibraryCode = isInLibraryCode();
        m_tiesLeftBySettling.append(ties);
    };
    auto tieMembers = [&](bool ofKept, TiesToLay::Access access, FlowNodeID aggregate, PropertyKeyID key) {
        TiesToLay ties { TiesToLay::Members::Listed, access, aggregate, 0, key };
        if (!leavesTies) {
            forEachMember(ofKept ? membersOfKept : membersOfAbsorbed, ofKept ? kept : absorbed, [&](LayoutID member) { layTie(ties, member); });
            return;
        }
        auto& place = ofKept ? placeOfKept : placeOfAbsorbed;
        if (!place) {
            uint32_t first = m_membersOfTiesLeftBySettling.size();
            forEachMember(ofKept ? membersOfKept : membersOfAbsorbed, ofKept ? kept : absorbed, [&](LayoutID member) { m_membersOfTiesLeftBySettling.append(member); });
            place = std::pair { first, static_cast<uint32_t>(m_membersOfTiesLeftBySettling.size()) };
        }
        leave(access, aggregate, key, *place);
    };
    auto tieHolders = [&](uint32_t firstHolder, TiesToLay::Access access, FlowNodeID aggregate, PropertyKeyID key) {
        TiesToLay ties { TiesToLay::Members::Listed, access, aggregate, 0, key };
        if (!leavesTies) {
            forEachHolder(firstHolder, [&](LayoutID member) { layTie(ties, member); });
            return;
        }
        uint32_t first = m_membersOfTiesLeftBySettling.size();
        forEachHolder(firstHolder, [&](LayoutID member) { m_membersOfTiesLeftBySettling.append(member); });
        leave(access, aggregate, key, { first, static_cast<uint32_t>(m_membersOfTiesLeftBySettling.size()) });
    };

    tieToEachOther(m_lostFamilies[kept].foundBesideFields, m_lostFamilies[absorbed].foundBesideFields);
    tieToEachOther(m_lostFamilies[kept].storedBesideFields, m_lostFamilies[absorbed].storedBesideFields);

    for (auto read : { &LostFamily::anyFieldRead, &LostFamily::anyFieldReadUnderMarksOfRule }) {
        uint32_t anyFieldRead = merge(m_lostFamilies[kept].*read, m_lostFamilies[absorbed].*read, [&](FlowNodeID aggregate) {
            tieMembers(true, TiesToLay::Access::ReadOfAnyField, aggregate, { });
        }, [&](FlowNodeID aggregate) {
            tieMembers(false, TiesToLay::Access::ReadOfAnyField, aggregate, { });
        });
        m_lostFamilies[kept].*read = anyFieldRead;
    }

    if (!m_elementsReadThroughLostFamilies.isEmpty()) {
        m_elementsReadThroughLostFamilies[kept] = merge(m_elementsReadThroughLostFamilies[kept], m_elementsReadThroughLostFamilies[absorbed], [&](FlowNodeID aggregate) {
            tieMembers(true, TiesToLay::Access::ReadOfAnyFieldUnderNumbers, aggregate, { });
        }, [&](FlowNodeID aggregate) {
            tieMembers(false, TiesToLay::Access::ReadOfAnyFieldUnderNumbers, aggregate, { });
        });
    }

    Vector<uint32_t> tyingEveryMember = m_lostFamilies[kept].keysThatTieEveryMember;
    for (uint32_t record : tyingEveryMember) {
        LostKey ofKept = m_lostKeys[record];
        if (m_recordsOfLostKeys.contains(placeOfLostKeyInLinkFlow(absorbed, ofKept.key)))
            continue;
        if (ofKept.read != noNodeInLinkFlow)
            tieMembers(false, TiesToLay::Access::Read, FlowNodeID(ofKept.read), ofKept.key);
        if (ofKept.stored != noNodeInLinkFlow)
            tieMembers(false, TiesToLay::Access::Assign, FlowNodeID(ofKept.stored), ofKept.key);
        if (ofKept.defined != noNodeInLinkFlow)
            tieMembers(false, TiesToLay::Access::Define, FlowNodeID(ofKept.defined), ofKept.key);
    }

    for (uint32_t record = m_lostFamilies[absorbed].firstKey; record != UINT32_MAX;) {
        LostKey ofAbsorbed = m_lostKeys[record];
        uint32_t current = record;
        record = ofAbsorbed.next;
        PropertyKeyID key = ofAbsorbed.key;
        m_recordsOfLostKeys.remove(placeOfLostKeyInLinkFlow(absorbed, key));
        auto added = m_recordsOfLostKeys.add(placeOfLostKeyInLinkFlow(kept, key), current);
        uint32_t standing = added.iterator->value;
        LostKey ofKept = added.isNewEntry ? LostKey { } : m_lostKeys[standing];

        auto mergeAccess = [&](uint32_t nodeOfKept, uint32_t nodeOfAbsorbed, TiesToLay::Access access) -> uint32_t {
            if (nodeOfKept == noNodeInLinkFlow && nodeOfAbsorbed == noNodeInLinkFlow)
                return noNodeInLinkFlow;
            return merge(nodeOfKept, nodeOfAbsorbed, [&](FlowNodeID aggregate) {
                if (ofAbsorbed.tiesEveryMember)
                    tieMembers(true, access, aggregate, key);
                else
                    tieHolders(ofKept.firstHolder, access, aggregate, key);
            }, [&](FlowNodeID aggregate) {
                if (ofAbsorbed.tiesEveryMember)
                    tieMembers(false, access, aggregate, key);
                else
                    tieHolders(ofAbsorbed.firstHolder, access, aggregate, key);
            });
        };
        uint32_t read = mergeAccess(ofKept.read, ofAbsorbed.read, TiesToLay::Access::Read);
        uint32_t stored = mergeAccess(ofKept.stored, ofAbsorbed.stored, TiesToLay::Access::Assign);
        uint32_t defined = mergeAccess(ofKept.defined, ofAbsorbed.defined, TiesToLay::Access::Define);

        LostKey& merged = m_lostKeys[standing];
        merged.read = read;
        merged.stored = stored;
        merged.defined = defined;
        merged.family = kept;
        if (added.isNewEntry) {
            merged.next = m_lostFamilies[kept].firstKey;
            m_lostFamilies[kept].firstKey = standing;
        } else {
            if (ofAbsorbed.firstHolder != UINT32_MAX) {
                if (merged.lastHolder == UINT32_MAX)
                    merged.firstHolder = ofAbsorbed.firstHolder;
                else
                    m_holdersOfLostKeys[merged.lastHolder].next = ofAbsorbed.firstHolder;
                merged.lastHolder = ofAbsorbed.lastHolder;
            }
            m_lostKeys[current].hasBeenMerged = true;
        }
        bool hasNode = read != noNodeInLinkFlow || stored != noNodeInLinkFlow || defined != noNodeInLinkFlow;
        if (merged.tiesEveryMember && hasNode && (added.isNewEntry || !merged.isListedAsTyingEveryMember)) {
            merged.isListedAsTyingEveryMember = true;
            m_lostFamilies[kept].keysThatTieEveryMember.append(standing);
        }
    }
    m_lostFamilies[absorbed].firstKey = UINT32_MAX;
    m_lostFamilies[absorbed].keysThatTieEveryMember.clear();

    if (m_lostFamilies[kept].reachesUnseen != m_lostFamilies[absorbed].reachesUnseen) {
        bool keptHasEscaped = m_lostFamilies[kept].reachesUnseen;
        forEachMember(keptHasEscaped ? membersOfAbsorbed : membersOfKept, keptHasEscaped ? absorbed : kept, [&](LayoutID member) {
            deferEscape(member);
        });
        m_lostFamilies[kept].reachesUnseen = true;
    }
}

void LinkFlow::letLostFamilyEscape(LayoutID member)
{
    if (!member)
        return;
    uint32_t representative = unitedLayouts.representativeOf(member.index());
    if (std::exchange(m_lostFamilies[representative].reachesUnseen, true))
        return;
    for (LayoutID layout : membersOfLostFamily(representative))
        deferEscape(layout);
    settle();
}

Vector<FlowNodeID> LinkFlow::endsOfEdgesOutOf(FlowNodeID node) const
{
    Vector<FlowNodeID> ends;
    if (node.index() >= m_listsOfNodes.size())
        return ends;
    const Lane& lane = *m_lanes[laneIndexOf(node)];
    for (uint32_t listed = m_listsOfNodes[node.index()].firstEdge; listed != UINT32_MAX; listed = lane.edges[listed].next)
        ends.append(lane.edges[listed].to);
    return ends;
}

void LinkFlow::forEachEdgeOutOf(FlowNodeID node, NOESCAPE const ScopedLambda<void(FlowNodeID to, TypeID passes)>& functor) const
{
    if (node.index() >= m_listsOfNodes.size())
        return;
    const Lane& lane = *m_lanes[laneIndexOf(node)];
    for (uint32_t listed = m_listsOfNodes[node.index()].firstEdge; listed != UINT32_MAX; listed = lane.edges[listed].next)
        functor(lane.edges[listed].to, lane.edges[listed].passes);
}

bool LinkFlow::flowsIntoUnseen(FlowNodeID node) const
{
    return m_flowsIntoUnseen.get(node.index());
}

LinkFlow::NodeSubject LinkFlow::subjectOfNode(FlowNodeID id) const
{
    using Kind = NodeSubject::Kind;
    uint32_t node = static_cast<uint32_t>(id.index());
    size_t numberOfFunctions = program.functions.size();
    if (node < m_firstVariable || node >= m_nodesOfFunctions[numberOfFunctions].firstParameter)
        return { };
    if (node < m_firstVariable + program.variables.size())
        return { Kind::Variable, node - m_firstVariable, 0 };
    uint32_t firstField = m_firstVariable + static_cast<uint32_t>(program.variables.size());
    if (node < firstField + m_firstDeclaredFieldOfLayout.last()) {
        uint32_t layout = static_cast<uint32_t>(std::ranges::upper_bound(m_firstDeclaredFieldOfLayout, node - firstField) - m_firstDeclaredFieldOfLayout.begin() - 1);
        uint32_t numberOfFields = m_firstFieldOfLayout[layout + 1] - m_firstFieldOfLayout[layout];
        for (uint32_t field = node - firstField - m_firstDeclaredFieldOfLayout[layout]; field < numberOfFields; ++field) {
            if (m_nodeOfField[m_firstFieldOfLayout[layout] + field] == node)
                return { Kind::Field, layout, field };
        }
        return { };
    }
    if (node < m_nodesOfFunctions[0].firstParameter)
        return { };
    uint32_t function = static_cast<uint32_t>(std::ranges::upper_bound(m_nodesOfFunctions, node, { }, &NodesOfFunction::firstParameter) - m_nodesOfFunctions.begin() - 1);
    const NodesOfFunction& nodes = m_nodesOfFunctions[function];
    if (node < nodes.firstResult)
        return { Kind::Parameter, function, node - nodes.firstParameter };
    if (node < nodes.thrown)
        return { Kind::Result, function, node - nodes.firstResult };
    if (node == nodes.thrown)
        return { Kind::Thrown, function, 0 };
    return { Kind::SummaryNode, function, 0 };
}

void LinkFlow::forEachContents(NOESCAPE const ScopedLambda<void(SiteID, SlotOfContents, FlowNodeID)>& functor) const
{
    for (uint32_t site = 0; site < m_sites.size(); ++site) {
        const Vector<ContentsOfSite>& listed = contentsListedOf(site);
        for (uint32_t index = m_sites[site].newestContents; index != UINT32_MAX; index = listed[index].next)
            functor(SiteID(site), listed[index].slot, listed[index].node);
    }
}

std::optional<ValueID> LinkFlow::valueThatMakes(const SummaryNode& classInstance)
{
    static constexpr uint32_t isInData = 1u << 31;
    if (!classInstance.second || classInstance.second & isInData)
        return std::nullopt;
    return ValueID(classInstance.second - 1);
}

Vector<LinkFlow::SiteMaker> LinkFlow::findSiteMakers() const
{
    Vector<SiteMaker> makers(m_sites.size());
    auto takeCall = [&](uint32_t number, uint32_t site) {
        if (number >= firstCallOfFunction.last())
            return;
        uint32_t function = static_cast<uint32_t>(std::ranges::upper_bound(firstCallOfFunction, number) - firstCallOfFunction.begin() - 1);
        makers[site] = { FunctionID(function), summaries.at(FunctionID(function)).calls[number - firstCallOfFunction[function]].site };
    };
    for (const PartOfSitesOfMakers& part : *m_sitesOfMakers) {
        for (const auto& [words, site] : part.sites) {
            MakerOfSite maker = static_cast<MakerOfSite>(words.first >> 32);
            if (maker != MakerOfSite::ListOfArguments)
                takeCall(static_cast<uint32_t>(words.first), site);
        }
    }
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        uint32_t site = m_firstSiteOfFunction[index];
        for (const SummaryNode& node : summaries.at(FunctionID(index)).nodes) {
            if (node.kind != SummaryNode::Kind::ClassInstance)
                continue;
            if (std::optional<ValueID> value = valueThatMakes(node))
                makers[site] = { FunctionID(index), *value };
            ++site;
        }
    }
    return makers;
}

Vector<FlowObserver> LinkFlow::observersOf(FlowNodeID node) const
{
    Vector<FlowObserver> observers;
    if (node.index() >= m_listsOfNodes.size())
        return observers;
    const Lane& lane = *m_lanes[laneIndexOf(node)];
    for (uint32_t listed = m_listsOfNodes[node.index()].firstObserver; listed != UINT32_MAX; listed = lane.observers[listed].next())
        observers.append(lane.observers[listed].observer());
    return observers;
}

void LinkFlow::forgetWaysOfFlow()
{
    m_sizeWhenWaysWereForgotten = size();
    runOnLanes([&](unsigned index) {
        auto empty = makeUnique<Lane>();
        empty->index = index;
        m_lanes[index] = WTF::move(empty);
    });
    m_firstLane = m_lanes[0].get();
    m_listsOfNodes.clear();
    m_isToBeTold = { };
    m_flowsIntoUnseen = { };
    m_accessorsWaitAt = { };
    m_flowIsAtRest = true;
}

SizeOfLinkFlow LinkFlow::size() const
{
    if (m_sizeWhenWaysWereForgotten)
        return *m_sizeWhenWaysWereForgotten;
    SizeOfLinkFlow size { m_nodes.size(), 0, 0, 0, m_numberOfObserversTold };
    for (auto& lane : m_lanes) {
        size.edges += lane->edges.size();
        size.observers += lane->observers.size();
        size.nodesTakenOffWorklist += lane->numberOfNodesTakenOffWorklist;
    }
    size.rounds = m_numberOfRounds;
    size.tiesOfPools = m_tiesOfPools;
    return size;
}

LinkType LinkFlow::typeOf(SiteID site, BuiltinClassID builtinClass)
{
    LinkType type;
    type.classes = LinkType::Classes::Site;
    type.builtinClass = builtinClass;
    type.site = site;
    return type;
}

SiteID LinkFlow::siteOf(MakerOfSite maker, uint32_t number, uint32_t part, BuiltinClassID builtinClass, bool onlyWaitRightBehindSeesIt, bool isOfClassItself, bool isCreatedWithoutHoles)
{
    RELEASE_ASSERT(builtinClass && part < 1u << 16);
    std::pair<uint64_t, uint32_t> words { static_cast<uint64_t>(maker) << 32 | number, part << 16 | builtinClass.index() };
    PartOfSitesOfMakers& table = (*m_sitesOfMakers)[((words.first ^ static_cast<uint64_t>(words.second) << 35) * 0x9e3779b97f4a7c15ull) >> (64 - log2OfPartsOfSitesOfMakers)];
    bool severalThreadsAsk = m_severalThreadsTell;
    if (severalThreadsAsk) {
        while (table.isLocked.load(std::memory_order_relaxed) || table.isLocked.exchange(true, std::memory_order_acquire))
            simde_mm_pause();
    }
    auto added = table.sites.add(words, 0);
    if (added.isNewEntry) {
        Site site;
        site.builtinClass = builtinClass;
        site.onlyWaitRightBehindSeesIt = onlyWaitRightBehindSeesIt;
        site.isOfClassItself = isOfClassItself;
        site.isCreatedWithoutHoles = isCreatedWithoutHoles;
        site.isCreatedWithoutEngineNames = false;
        added.iterator->value = m_sites.append(site);
    }
    SiteID site(added.iterator->value);
    if (severalThreadsAsk)
        table.isLocked.store(false, std::memory_order_release);
    if (added.isNewEntry)
        fillSiteJustMade(site);
    return site;
}

SiteID LinkFlow::siteOfSummary(FunctionID function, uint32_t rank, BuiltinClassID builtinClass)
{
    RELEASE_ASSERT(builtinClass && rank < numberOfSitesOfSummary(function));
    SiteID site(m_firstSiteOfFunction[function.index()] + rank);
    if (!m_sites[site.index()].builtinClass) {
        m_sites[site.index()].builtinClass = builtinClass;
        fillSiteJustMade(site);
    }
    return site;
}

void LinkFlow::fillSiteJustMade(SiteID site)
{
    if (!m_severalThreadsTell)
        m_familiesOfSites.growTo(static_cast<uint32_t>(m_sites.size()));
    if (m_promisesMayBeHandedOut && !m_sites[site.index()].onlyWaitRightBehindSeesIt && program.builtins().isSameOrSubclassOf(classOf(site), BuiltinClasses::Promise))
        arrive(contents(site, SlotOfContents::underUnknownNames()), LinkType::anything());
}

bool LinkFlow::letPromisesBeHandedOut()
{
    if (std::exchange(m_promisesMayBeHandedOut, true))
        return false;
    bool hasArrived = false;
    for (uint32_t index = 0; index < m_sites.size(); ++index) {
        if (!m_sites[index].builtinClass || m_sites[index].onlyWaitRightBehindSeesIt || !program.builtins().isSameOrSubclassOf(m_sites[index].builtinClass, BuiltinClasses::Promise))
            continue;
        arrive(contents(SiteID(index), SlotOfContents::underUnknownNames()), LinkType::anything());
        hasArrived = true;
    }
    return hasArrived;
}

LinkFlow::ClassesOfSites LinkFlow::findClassesOfSitesThatHaveEscapedOrAreClimbedFrom() const
{
    ClassesOfSites classes;
    Lock lock;
    forEachRangeOfIndicesInParallel(loops, m_sites.size(), [&](size_t first, size_t end) {
        static constexpr uint8_t hasEscaped = 1 << 0;
        static constexpr uint8_t isClimbedFrom = 1 << 1;
        Vector<uint8_t> ofClasses;
        m_sites.forEachIn(first, end, [&](size_t index, const Site& site) {
            if (!site.builtinClass)
                return;
            size_t builtinClass = site.builtinClass.index();
            if (size_t size = ofClasses.size(); builtinClass >= size) {
                ofClasses.grow(std::max(builtinClass + 1, 2 * size));
                std::ranges::fill(ofClasses.mutableSpan().subspan(size), 0);
            }
            uint8_t& known = ofClasses[builtinClass];
            if (known == (hasEscaped | isClimbedFrom))
                return;
            uint32_t representative = m_familiesOfSites.representativeWithoutShortening(static_cast<uint32_t>(index));
            const Site& family = representative == index ? site : m_sites[representative];
            known |= (family.hasEscaped ? hasEscaped : 0) | (family.isClimbedFrom ? isClimbedFrom : 0);
        });
        Locker locker { lock };
        for (size_t builtinClass = 0; builtinClass < ofClasses.size(); ++builtinClass) {
            if (ofClasses[builtinClass] & hasEscaped)
                classes.haveEscaped.set(builtinClass);
            if (ofClasses[builtinClass] & isClimbedFrom)
                classes.areClimbedFrom.set(builtinClass);
        }
    });
    return classes;
}

SiteID LinkFlow::listOfArgumentsOf(FunctionID function)
{
    return siteOf(MakerOfSite::ListOfArguments, function.index(), 0, BuiltinClasses::Array);
}

LinkFlow::ArgumentsGathered& LinkFlow::argumentsGatheredBy(FunctionID function)
{
    auto added = m_argumentsGathered.of(*this).add(function.index(), nullptr);
    if (added.isNewEntry) {
        added.iterator->value = makeUnique<ArgumentsGathered>();
        added.iterator->value->atAnyPlace = makeNode();
    }
    return *added.iterator->value;
}

FlowNodeID LinkFlow::argumentsGatheredAtAnyPlace(FunctionID function)
{
    LockerOfWhatIsShared locker(*this);
    return argumentsGatheredBy(function).atAnyPlace;
}

FlowNodeID LinkFlow::argumentGatheredAt(FunctionID function, uint32_t place)
{
    LockerOfWhatIsShared locker(*this);
    ArgumentsGathered& gathered = argumentsGatheredBy(function);
    if (place >= gathered.atPlaces.size())
        gathered.atPlaces.grow(place + 1);
    if (gathered.atPlaces[place])
        return gathered.atPlaces[place];

    FlowNodeID made = makeNode();
    gathered.atPlaces[place] = made;
    for (size_t index = 0; index < gathered.takers.size(); ++index) {
        ArgumentsGathered::Taker taker = gathered.takers[index];
        if (taker.place == place)
            addEdge(made, taker.node);
    }
    for (size_t index = 0; index < gathered.gatherersBehind.size(); ++index) {
        ArgumentsGathered::GathererBehind behind = gathered.gatherersBehind[index];
        if (place >= behind.firstPlace)
            addEdge(made, argumentGatheredAt(behind.function, place - behind.firstPlace));
    }
    return made;
}

void LinkFlow::noteNumberOfArgumentsGathered(FunctionID function, uint32_t number, CallerOfGatherer caller)
{
    LockerOfWhatIsShared locker(*this);
    ArgumentsGathered& gathered = argumentsGatheredBy(function);
    uint32_t& fewest = caller == CallerOfGatherer::InSight ? gathered.fewestInSight : gathered.fewestByEscapeAlone;
    if (number >= fewest)
        return;
    uint32_t before = std::exchange(fewest, number);
    LinkType undefined = typeOfPrimitivesInLinkFlow(Primitive::Undefined);
    if (caller == CallerOfGatherer::UnseenByEscapeAlone)
        undefined = undefined.asEscapeOnly();
    for (size_t index = 0; index < gathered.takers.size(); ++index) {
        ArgumentsGathered::Taker taker = gathered.takers[index];
        if (taker.place >= number && taker.place < before)
            arrive(taker.node, undefined);
    }
    for (size_t index = 0; index < gathered.gatherersBehind.size(); ++index) {
        ArgumentsGathered::GathererBehind behind = gathered.gatherersBehind[index];
        noteNumberOfArgumentsGathered(behind.function, number > behind.firstPlace ? number - behind.firstPlace : 0, caller);
    }
}

void LinkFlow::gatherWhatListsThatArriveHold(FunctionID function)
{
    const ThinSummary& summary = summaries.at(function);
    FlowNodeID atAnyPlace = argumentsGatheredAtAnyPlace(function);
    for (const SummaryCall& call : summary.calls) {
        if (call.passesOnGatheredArguments)
            addEdge(nodeOf(function, summary.arguments[call.firstArgument]), atAnyPlace);
        else if (call.appliesGatheredArguments)
            arrive(atAnyPlace, LinkType::anything());
    }
    noteNumberOfArgumentsGathered(function, 0);
}

void LinkFlow::passOnGatheredArgument(FunctionID gatherer, uint32_t place, FlowNodeID to)
{
    LockerOfWhatIsShared locker(*this);
    if (!m_takersOfGatheredArgumentsAtPlaces.of(*this).add({ static_cast<uint64_t>(gatherer.index()) << 32 | to.index(), place }).isNewEntry)
        return;
    ArgumentsGathered& gathered = argumentsGatheredBy(gatherer);
    gathered.takers.append({ to, place });
    addEdge(gathered.atAnyPlace, to);
    if (place < gathered.atPlaces.size() && gathered.atPlaces[place])
        addEdge(gathered.atPlaces[place], to);
    LinkType undefined = typeOfPrimitivesInLinkFlow(Primitive::Undefined);
    if (place >= gathered.fewestInSight)
        arrive(to, undefined);
    else if (place >= gathered.fewestByEscapeAlone)
        arrive(to, undefined.asEscapeOnly());
}

void LinkFlow::passOnGatheredArguments(FunctionID gatherer, uint32_t firstPlace, FunctionID gathererBehind)
{
    LockerOfWhatIsShared locker(*this);
    if (!m_takersOfGatheredArgumentsAtPlaces.of(*this).add({ 1ull << 63 | static_cast<uint64_t>(gatherer.index()) << 32 | gathererBehind.index(), firstPlace }).isNewEntry)
        return;
    ArgumentsGathered& gathered = argumentsGatheredBy(gatherer);
    gathered.gatherersBehind.append({ gathererBehind, firstPlace });
    addEdge(gathered.atAnyPlace, argumentsGatheredAtAnyPlace(gathererBehind));
    for (uint32_t place = firstPlace; place < gathered.atPlaces.size(); ++place) {
        if (FlowNodeID from = gathered.atPlaces[place])
            addEdge(from, argumentGatheredAt(gathererBehind, place - firstPlace));
    }
    for (CallerOfGatherer caller : { CallerOfGatherer::InSight, CallerOfGatherer::UnseenByEscapeAlone }) {
        uint32_t fewest = caller == CallerOfGatherer::InSight ? gathered.fewestInSight : gathered.fewestByEscapeAlone;
        if (fewest != UINT32_MAX)
            noteNumberOfArgumentsGathered(gathererBehind, fewest > firstPlace ? fewest - firstPlace : 0, caller);
    }
}

SlotOfContents LinkFlow::slotOf(const BuiltinSlot& slot)
{
    if (slot.contentsSlot && slot.wordOfCell == BuiltinWordOfCell::None)
        return *slot.contentsSlot;
    if (slot.internalField)
        return SlotOfContents::internalField(*slot.internalField);
    return SlotOfContents::stateOf(slot.id);
}

SlotOfContents LinkFlow::slotOf(BuiltinSlotID slot) const
{
    return slotOf(program.builtins().at(slot));
}

Vector<SlotOfContents, 2> LinkFlow::slotsBehind(BuiltinClassID builtinClass, SlotOfContents slot) const
{
    Vector<SlotOfContents, 2> slots;
    if (std::optional<uint32_t> field = slot.indexOfInternalField()) {
        const BuiltinTable& builtins = program.builtins();
        for (BuiltinClassID current = builtinClass; current; current = builtins.at(current).parent) {
            for (BuiltinSlotID id : builtins.at(current).slots) {
                const BuiltinSlot& listed = builtins.at(id);
                if (listed.internalField && *listed.internalField == *field)
                    slots.appendIfNotContains(slotOf(listed));
            }
        }
    }
    if (slots.isEmpty())
        slots.append(slot);
    return slots;
}

void LinkFlow::open(FlowNodeID contents)
{
    addEdge(contents, unseen());
    if (contents != unseen())
        arrive(contents, LinkType::anything().asEscapeOnly());
}

BuiltinWordOfCell LinkFlow::wordOfCell(SlotOfContents slot) const
{
    std::optional<BuiltinSlotID> state = slot.stateSlot();
    return state ? program.builtins().at(*state).wordOfCell : BuiltinWordOfCell::None;
}

bool LinkFlow::isWordOfCell(SlotOfContents slot) const
{
    return wordOfCell(slot) != BuiltinWordOfCell::None;
}

void LinkFlow::openContents(SlotOfContents slot, FlowNodeID contents)
{
    if (slot == SlotOfContents::mayBeShorter()) {
        arrive(contents, fromTypeID(TypeTable::undefined));
        return;
    }
    if (slot == SlotOfContents::hasSeveralRecordLayouts())
        return;
    if (!isWordOfCell(slot))
        open(contents);
}

void LinkFlow::noteRecordsAreMadeAt(SiteID site, BuiltinLayoutID layout)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    m_sites[m_familiesOfSites.representativeOf(site.index())].mayHoldCellsOfCompiledLayouts = true;
    if (m_familiesOfSites.isAlone(site.index()) && m_sites[site.index()].layoutOfRecords != layout)
        m_sites[site.index()].layoutOfRecords = layout;
}

void LinkFlow::noteListIsCopied(SiteID site)
{
    if (m_sites[site.index()].isCreatedWithoutHoles)
        return;
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (m_familiesOfSites.isAlone(site.index()))
        m_sites[site.index()].isCreatedWithoutHoles = true;
}

bool LinkFlow::hasNoHoles(const LinkType& list) const
{
    return m_sites[m_familiesOfSites.representativeOf(list.site.index())].isCreatedWithoutHoles && !isMarked(list.site, SlotOfContents::mayHaveHoles());
}

void LinkFlow::noteLengthIsWritten(SiteID site, uint32_t length)
{
    if (!length || m_sites[site.index()].writtenLength == length)
        return;
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (m_familiesOfSites.isAlone(site.index()))
        m_sites[site.index()].writtenLength = length;
}

void LinkFlow::noteListIsWritten(SiteID site, FunctionID function, uint32_t firstPlace, uint32_t numberOfPlaces)
{
    {
        LockerOfWhatIsShared lockerOfWhatIsShared(*this);
        if (m_familiesOfSites.isAlone(site.index()) && !m_writtenListsOfFamilies.contains(site.index())) {
            m_writtenLists.append(makeUnique<WrittenList>());
            WrittenList& list = *m_writtenLists.last();
            list.function = function;
            list.firstPlace = firstPlace;
            list.numberOfPlaces = numberOfPlaces;
            list.lastOfChain = &list;
            list.numberOfPlacesOfFamily = numberOfPlaces;
            m_writtenListsOfFamilies.add(site.index(), &list);
            return;
        }
    }
    arrive(contents(site, SlotOfContents::isChanged()), fromTypeID(TypeTable::undefined));
}

uint32_t LinkFlow::numberOfPlacesWrittenOf(SiteID site) const
{
    const WrittenList* first = m_writtenListsOfFamilies.get(m_familiesOfSites.representativeWithoutShortening(site.index()));
    return first ? first->numberOfPlacesOfFamily : 0;
}

void LinkFlow::layWhatIsWrittenAt(const WrittenList& list, uint32_t place, FlowNodeID to)
{
    if (place >= list.numberOfPlaces)
        return;
    std::span<const uint32_t> places = summaries.at(list.function).placesOfWrittenLists.span();
    uint32_t origins = places[list.firstPlace + place];
    if (!origins) {
        addEdge(unseen(), to);
        return;
    }
    for (uint32_t origin = 0; origin < places[origins]; ++origin)
        addEdge(nodeOf(list.function, places[origins + 1 + 2 * origin]), to, TypeID::fromBits(places[origins + 2 + 2 * origin]));
}

void LinkFlow::layWhatListsFromWriteAt(const WrittenList* first, SlotOfContents slot, FlowNodeID to)
{
    std::optional<uint32_t> place = slot.writtenPlace();
    for (const WrittenList* list = place ? first : nullptr; list; list = list->next)
        layWhatIsWrittenAt(*list, *place, to);
}

FlowNodeID LinkFlow::whatIsWrittenAt(SiteID member, uint32_t place)
{
    uint32_t family = m_familiesOfSites.representativeWithoutShortening(member.index());
    if (FlowNodeID standing = findContents(family, SlotOfContents::at(place)))
        return standing;
    FlowNodeID written = contents(member, SlotOfContents::at(place));
    if (!m_severalThreadsTell)
        layWhatListsFromWriteAt(m_writtenListsOfFamilies.get(family), SlotOfContents::at(place), written);
    return written;
}

void LinkFlow::tellWrittenListIsChanged(const LinkType& type)
{
    if (type.classes != LinkType::Classes::None && numberOfPlacesWrittenOf(type.site))
        arrive(contents(type.site, SlotOfContents::isChanged()), fromTypeID(TypeTable::undefined));
}

uint32_t LinkFlow::leastLengthOfViewsIn(const LinkType& type) const
{
    if (type.classes != LinkType::Classes::Site || type.objects != LinkType::Objects::None || type.functions != LinkType::Functions::None || type.hasUnknownObjects || type.holdsLostBuiltins || type.builtin || (type.primitives & ~(Primitive::Undefined | Primitive::Null | Primitive::Absent | Primitive::Unspecified)))
        return 0;
    if (hasEscaped(type.site) || program.builtins().typedArrayTypeOf(type.builtinClass) == NotTypedArray)
        return 0;
    FlowNodeID mayBeShorter = findContentsOf(type, SlotOfContents::mayBeShorter());
    return !mayBeShorter || (mayBeShorter != unseen() && typeAt(mayBeShorter).isNever()) ? writtenLengthOf(type.site) : 0;
}

static uint64_t slotOfFamilyInLinkFlow(uint32_t family, SlotOfContents slot)
{
    return static_cast<uint64_t>(family) << 32 | slot.number();
}

FlowNodeID LinkFlow::findContents(uint32_t family, SlotOfContents slot) const
{
    const PartOfContentsOfSites& part = *m_contentsOfSites[partOfContentsOf(family)];
    const Site& holder = m_sites[family];
    if (holder.numberOfContents > numberOfSlotsLookedThroughInLinkFlow) {
        auto found = part.ofFamiliesWithManySlots.find(slotOfFamilyInLinkFlow(family, slot));
        return found == part.ofFamiliesWithManySlots.end() ? FlowNodeID() : FlowNodeID(found->value);
    }
    for (uint32_t index = holder.newestContents; index != UINT32_MAX; index = part.listed[index].next) {
        if (part.listed[index].slot == slot)
            return part.listed[index].node;
    }
    return { };
}

void LinkFlow::listContents(uint32_t site, SlotOfContents slot, FlowNodeID node)
{
    PartOfContentsOfSites& part = *m_contentsOfSites[partOfContentsOf(site)];
    Site& holder = m_sites[site];
    part.listed.append(ContentsOfSite { slot, node, holder.newestContents });
    holder.newestContents = static_cast<uint32_t>(part.listed.size() - 1);
    uint32_t numberOfContents = ++holder.numberOfContents;
    if (numberOfContents == numberOfSlotsLookedThroughInLinkFlow + 1) {
        for (uint32_t index = holder.newestContents; index != UINT32_MAX; index = part.listed[index].next)
            part.ofFamiliesWithManySlots.add(slotOfFamilyInLinkFlow(site, part.listed[index].slot), part.listed[index].node.index());
    } else if (numberOfContents > numberOfSlotsLookedThroughInLinkFlow)
        part.ofFamiliesWithManySlots.add(slotOfFamilyInLinkFlow(site, slot), node.index());
}

void LinkFlow::addContents(uint32_t family, SlotOfContents slot, FlowNodeID node)
{
    listContents(family, slot, node);
    if (slot.isUnderName() || slot == SlotOfContents::underUnknownNames())
        addEdge(node, contents(SiteID(family), SlotOfContents::underAnyName()));
    if (m_sites[family].hasEscaped)
        openContents(slot, node);
    if (isWordOfCell(slot) && !m_familiesOfSites.isAlone(family))
        open(node);
}

FlowNodeID LinkFlow::contentsFoundOrMadeByOneThread(SiteID member, SlotOfContents slot)
{
    if (FlowNodeID node = findContents(member.index(), slot))
        return node;
    uint32_t family = isWordOfCell(slot) ? member.index() : m_familiesOfSites.representativeOf(member.index());
    FlowNodeID node = family == member.index() ? FlowNodeID() : findContents(family, slot);
    if (!node) {
        node = makeNode();
        addContents(family, slot, node);
    }
    if (family != member.index())
        listContents(member.index(), slot, node);
    return node;
}

FlowNodeID LinkFlow::contentsFoundOrMadeByLane(SiteID member, SlotOfContents slot)
{
    uint64_t words = static_cast<uint64_t>(member.index()) << 32 | slot.number();
    if (std::optional<uint32_t> known = findWhatLaneWasGiven(MadeOnDemand::Contents, words))
        return FlowNodeID::fromBits(*known);
    FlowNodeID node = findContents(member.index(), slot);
    if (!node) {
        uint32_t family = isWordOfCell(slot) ? member.index() : m_familiesOfSites.representativeOf(member.index());
        if (family != member.index())
            node = findContents(family, slot);
        if (!node) {
            uint64_t wordsOfFamily = static_cast<uint64_t>(family) << 32 | slot.number();
            if (std::optional<uint32_t> made = findWhatLaneWasGiven(MadeOnDemand::ContentsOfFamily, wordsOfFamily))
                node = FlowNodeID::fromBits(*made);
            else {
                node = makeNode();
                makeContentsInLane(family, slot, node);
            }
        }
        if (family != member.index())
            s_laneAtWork->contentsGiven[partOfContentsOf(member.index())].append({ static_cast<uint32_t>(member.index()), slot, node });
    }
    keepWhatLaneWasGiven(MadeOnDemand::Contents, words, node.bits());
    return node;
}

void LinkFlow::makeContentsInLane(uint32_t family, SlotOfContents slot, FlowNodeID node)
{
    keepWhatLaneWasGiven(MadeOnDemand::ContentsOfFamily, static_cast<uint64_t>(family) << 32 | slot.number(), node.bits());
    s_laneAtWork->contentsMade[partOfContentsOf(family)].append({ family, slot, node });
    if (slot.isUnderName() || slot == SlotOfContents::underUnknownNames())
        addEdge(node, contents(SiteID(family), SlotOfContents::underAnyName()));
    if (m_sites[family].hasEscaped)
        openContents(slot, node);
    if (isWordOfCell(slot) && !m_familiesOfSites.isAlone(family))
        open(node);
}

void LinkFlow::listContentsThatLanesMadeIn(unsigned part)
{
    for (auto& lane : m_lanes) {
        for (const Lane::ContentsOfLane& made : lane->contentsMade[part]) {
            FlowNodeID listed = findContents(made.site, made.slot);
            if (!listed) {
                listContents(made.site, made.slot, made.node);
                layWhatListsFromWriteAt(m_writtenListsOfFamilies.get(made.site), made.slot, made.node);
            } else if (listed != made.node) {
                addEdge(made.node, listed);
                addEdge(listed, made.node);
            }
        }
        lane->contentsMade[part].shrink(0);
    }
    for (auto& lane : m_lanes) {
        for (const Lane::ContentsOfLane& given : lane->contentsGiven[part]) {
            if (!findContents(given.site, given.slot))
                listContents(given.site, given.slot, given.node);
        }
        lane->contentsGiven[part].shrink(0);
    }
}

void LinkFlow::listContentsThatLanesMade()
{
    RELEASE_ASSERT(m_isDeferringUpdates);
    size_t numberToList = 0;
    for (auto& lane : m_lanes) {
        for (unsigned part = 0; part < m_lanes.size(); ++part)
            numberToList += lane->contentsMade[part].size() + lane->contentsGiven[part].size();
    }
    if (!numberToList)
        return;
    if (severalThreadsPayFor(numberToList)) {
        runOnLanes([&](unsigned part) {
            listContentsThatLanesMadeIn(part);
        });
        return;
    }
    for (unsigned part = 0; part < m_lanes.size(); ++part)
        listContentsThatLanesMadeIn(part);
}

void LinkFlow::takeAsContents(SiteID member, SlotOfContents slot, FlowNodeID node)
{
    RELEASE_ASSERT(slot);
    uint32_t family = m_familiesOfSites.representativeOf(member.index());
    FlowNodeID existing = findContents(family, slot);
    if (m_severalThreadsTell && !existing) {
        if (std::optional<uint32_t> made = findWhatLaneWasGiven(MadeOnDemand::ContentsOfFamily, static_cast<uint64_t>(family) << 32 | slot.number()))
            existing = FlowNodeID::fromBits(*made);
        else {
            makeContentsInLane(family, slot, node);
            return;
        }
    }
    if (!existing) {
        addContents(family, slot, node);
        return;
    }
    addEdge(node, existing);
    addEdge(existing, node);
}

void LinkFlow::unite(SiteID one, SiteID other)
{
    bool oneWasAlone = m_familiesOfSites.isAlone(one.index());
    bool otherWasAlone = m_familiesOfSites.isAlone(other.index());
    UnitedFamilies::Union united = m_familiesOfSites.unite(one.index(), other.index());
    if (!united.hasMerged())
        return;

    SiteID kept(united.kept);
    WrittenList* listsOfKept = m_writtenListsOfFamilies.get(united.kept);
    WrittenList* listsOfAbsorbed = m_writtenListsOfFamilies.take(united.absorbed);
    uint32_t placesOfKept = listsOfKept ? listsOfKept->numberOfPlacesOfFamily : 0;
    uint32_t placesOfAbsorbed = listsOfAbsorbed ? listsOfAbsorbed->numberOfPlacesOfFamily : 0;
    Site absorbed = m_sites[united.absorbed];
    Site& siteKept = m_sites[united.kept];
    const Vector<ContentsOfSite>& listedOfAbsorbed = contentsListedOf(united.absorbed);
    for (uint32_t index = absorbed.newestContents; index != UINT32_MAX;) {
        ContentsOfSite held = listedOfAbsorbed[index];
        index = held.next;
        if (isWordOfCell(held.slot)) {
            open(held.node);
            continue;
        }
        FlowNodeID ofKept = findContents(united.kept, held.slot);
        if (!ofKept) {
            listContents(united.kept, held.slot, held.node);
            layWhatListsFromWriteAt(listsOfKept, held.slot, held.node);
            if (siteKept.hasEscaped && !absorbed.hasEscaped)
                openContents(held.slot, held.node);
            continue;
        }
        addEdge(held.node, ofKept);
        addEdge(ofKept, held.node);
    }
    bool keptWasAlone = united.kept == one.index() ? oneWasAlone : united.kept == other.index() && otherWasAlone;
    const Vector<ContentsOfSite>& listedOfKept = contentsListedOf(united.kept);
    for (uint32_t index = keptWasAlone ? siteKept.newestContents : UINT32_MAX; index != UINT32_MAX; index = listedOfKept[index].next) {
        if (isWordOfCell(listedOfKept[index].slot))
            open(listedOfKept[index].node);
    }

    if (siteKept.writtenLength != absorbed.writtenLength) {
        siteKept.writtenLength = std::min(siteKept.writtenLength, absorbed.writtenLength);
        arrive(contents(kept, SlotOfContents::mayBeShorter()), fromTypeID(TypeTable::undefined));
    }
    if (siteKept.isCreatedWithoutHoles != absorbed.isCreatedWithoutHoles) {
        siteKept.isCreatedWithoutHoles = false;
        markSite(kept, SlotOfContents::mayHaveHoles());
    }
    if (siteKept.isCreatedWithoutEngineNames != absorbed.isCreatedWithoutEngineNames) {
        siteKept.isCreatedWithoutEngineNames = false;
        markSite(kept, SlotOfContents::mayHaveNamesOfEngine());
    }
    if (!placesOfKept || !placesOfAbsorbed)
        m_writtenListsOfFamilies.remove(united.kept);
    else {
        for (uint32_t place = 0; place < std::min(placesOfKept, placesOfAbsorbed); ++place) {
            FlowNodeID ofKept = findContents(united.kept, SlotOfContents::at(place));
            if (ofKept && !findContents(united.absorbed, SlotOfContents::at(place)))
                layWhatListsFromWriteAt(listsOfAbsorbed, SlotOfContents::at(place), ofKept);
        }
        listsOfAbsorbed->lastOfChain->next = listsOfKept;
        listsOfAbsorbed->lastOfChain = listsOfKept->lastOfChain;
        listsOfAbsorbed->numberOfPlacesOfFamily = std::min(placesOfKept, placesOfAbsorbed);
        m_writtenListsOfFamilies.set(united.kept, listsOfAbsorbed);
    }
    if (placesOfKept != placesOfAbsorbed) {
        if (FlowNodeID mark = findContents(united.kept, SlotOfContents::isChanged()))
            arrive(mark, fromTypeID(TypeTable::undefined));
    }
    if (siteKept.isOfClassItself != absorbed.isOfClassItself) {
        siteKept.isOfClassItself = false;
        if (FlowNodeID mark = findContents(united.kept, SlotOfContents::mayBeOfClassBelow()))
            arrive(mark, fromTypeID(TypeTable::undefined));
    }

    for (NodeOnDemand kind : { NodeOnDemand::WhatStandsAbove, NodeOnDemand::WhatStandsBelow }) {
        FlowNodeID ofAbsorbed = findNodeOnDemand(kind, united.absorbed | siteAmongNodesOnDemand, { });
        if (!ofAbsorbed)
            continue;
        FlowNodeID ofKept = nodeOnDemand(kind, united.kept | siteAmongNodesOnDemand, { }).first;
        addEdge(ofAbsorbed, ofKept);
        addEdge(ofKept, ofAbsorbed);
    }

    siteKept.isClimbedFrom |= absorbed.isClimbedFrom;
    siteKept.mayHoldCellsOfCompiledLayouts |= absorbed.mayHoldCellsOfCompiledLayouts;
    bool holdsMoreThanRecordsOfOneLayout = absorbed.layoutOfRecords != siteKept.layoutOfRecords;
    if (holdsMoreThanRecordsOfOneLayout) {
        siteKept.layoutOfRecords = { };
        if (FlowNodeID mark = findContents(united.kept, SlotOfContents::hasSeveralRecordLayouts()))
            arrive(mark, fromTypeID(TypeTable::undefined));
    }
    if (absorbed.hasEscaped || absorbed.isToldToEscape || holdsMoreThanRecordsOfOneLayout)
        escape(kept);
}

void LinkFlow::escape(SiteID member)
{
    if (m_tellsObservers) {
        deferEscape(member);
        return;
    }
    uint32_t family = m_familiesOfSites.representativeOf(member.index());
    Site& site = m_sites[family];
    site.isToldToEscape = true;
    if (std::exchange(site.hasEscaped, true))
        return;
    const Vector<ContentsOfSite>& listed = contentsListedOf(family);
    for (uint32_t index = site.newestContents; index != UINT32_MAX;) {
        ContentsOfSite held = listed[index];
        index = held.next;
        openContents(held.slot, held.node);
    }
}

uint32_t LinkFlow::familyOfLostFunctions(const LinkType& callee) const
{
    if (callee.functions == LinkType::Functions::Set)
        return unitedFunctions.representativeWithoutShortening(functionsOfSet(callee)[0].index());
    RELEASE_ASSERT(callee.functions == LinkType::Functions::Lost);
    return callee.function ? unitedFunctions.representativeWithoutShortening(callee.function.index()) : anyLostFunction;
}

uint32_t LinkFlow::poolOfLostFunctions(uint32_t family)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (m_poolsOfLostFunctions.isEmpty()) {
        m_poolsOfLostFunctions.append(PoolOfLostFunctions { });
        m_poolsOfLostFunctions[poolOfAllLostFunctions].receiver = fixedNodeInLinkFlow(FixedNodeOfLinkFlow::ReceiverOfLostFunctions).index();
        m_poolsOfLostFunctions[poolOfAllLostFunctions].result = fixedNodeInLinkFlow(FixedNodeOfLinkFlow::ResultOfLostFunctions).index();
    }
    if (family == anyLostFunction || m_lostFunctionsAreCalled)
        return poolOfAllLostFunctions;
    uint32_t& pool = m_poolOfFamilyOfFunctions[family];
    if (pool == UINT32_MAX) {
        pool = static_cast<uint32_t>(m_poolsOfLostFunctions.size());
        m_poolsOfLostFunctions.append(PoolOfLostFunctions { });
        m_poolsOfLostFunctions.last().receiver = makeNode().index();
        m_poolsOfLostFunctions.last().result = makeNode().index();
    }
    return pool;
}

FlowNodeID LinkFlow::argumentOfLostFunctions(uint32_t pool, uint32_t positionalArgument)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    while (m_poolsOfLostFunctions[pool].arguments.size() <= positionalArgument) {
        FlowNodeID node = makeNode();
        bool isGathered = m_poolsOfLostFunctions[pool].arguments.size() >= m_poolsOfLostFunctions[pool].firstArgumentGathered;
        m_poolsOfLostFunctions[pool].arguments.append(node.index());
        if (isGathered)
            addEdge(node, gatheredByLostFunctions(pool));
    }
    return FlowNodeID(m_poolsOfLostFunctions[pool].arguments[positionalArgument]);
}

void LinkFlow::gatherArgumentsOfLostFunctionsFrom(uint32_t pool, uint32_t firstGathered)
{
    if (firstGathered >= m_poolsOfLostFunctions[pool].firstArgumentGathered)
        return;
    uint32_t end = std::min<uint32_t>(m_poolsOfLostFunctions[pool].firstArgumentGathered, static_cast<uint32_t>(m_poolsOfLostFunctions[pool].arguments.size()));
    m_poolsOfLostFunctions[pool].firstArgumentGathered = firstGathered;
    for (uint32_t index = firstGathered; index < end; ++index)
        addEdge(FlowNodeID(m_poolsOfLostFunctions[pool].arguments[index]), gatheredByLostFunctions(pool));
}

void LinkFlow::tiePoolsOfLostFunctions(uint32_t kept, uint32_t absorbed)
{
    auto tie = [&](FlowNodeID first, FlowNodeID second) {
        addEdge(first, second);
        addEdge(second, first);
    };
    tie(FlowNodeID(m_poolsOfLostFunctions[kept].receiver), FlowNodeID(m_poolsOfLostFunctions[absorbed].receiver));
    tie(FlowNodeID(m_poolsOfLostFunctions[kept].result), FlowNodeID(m_poolsOfLostFunctions[absorbed].result));
    if (m_poolsOfLostFunctions[absorbed].laidOnClosures != UINT32_MAX)
        tie(laidOnClosuresOfPool(kept), FlowNodeID(m_poolsOfLostFunctions[absorbed].laidOnClosures));
    if (m_poolsOfLostFunctions[kept].laidOnClosures != UINT32_MAX) {
        for (FunctionID function : m_poolsOfLostFunctions[absorbed].functionsTied)
            tieWhatIsLaidOnClosuresOf(function, FlowNodeID(m_poolsOfLostFunctions[kept].laidOnClosures));
    }
    m_poolsOfLostFunctions[kept].functionsTied.appendVector(std::exchange(m_poolsOfLostFunctions[absorbed].functionsTied, { }));
    uint32_t numberOfArguments = static_cast<uint32_t>(m_poolsOfLostFunctions[absorbed].arguments.size());
    for (uint32_t index = 0; index < numberOfArguments; ++index)
        tie(argumentOfLostFunctions(kept, index), FlowNodeID(m_poolsOfLostFunctions[absorbed].arguments[index]));
    uint32_t firstGathered = std::min(m_poolsOfLostFunctions[kept].firstArgumentGathered, m_poolsOfLostFunctions[absorbed].firstArgumentGathered);
    if (firstGathered == UINT32_MAX)
        return;
    gatherArgumentsOfLostFunctionsFrom(kept, firstGathered);
    gatherArgumentsOfLostFunctionsFrom(absorbed, firstGathered);
    tie(gatheredByLostFunctions(kept), gatheredByLostFunctions(absorbed));
}

void LinkFlow::unite(FunctionID one, FunctionID other) const
{
    if (unitedFunctions.areUnited(one.index(), other.index()))
        return;
    bool oneIsAtUnseen = m_familiesOfFunctionsAtUnseen.get(unitedFunctions.representativeOf(one.index()));
    if (oneIsAtUnseen != m_familiesOfFunctionsAtUnseen.get(unitedFunctions.representativeOf(other.index()))) {
        unitedFunctions.forEachMemberOfFamilyOf(oneIsAtUnseen ? other.index() : one.index(), [&](uint32_t member) {
            m_membersOfFamiliesNewlyAtUnseen.append(FunctionID(member));
        });
    }
    bool oneIsCalled = m_calledFunctionFamilies.get(unitedFunctions.representativeOf(one.index()));
    if (oneIsCalled != m_calledFunctionFamilies.get(unitedFunctions.representativeOf(other.index())))
        noteCallOfFamilyMembers(oneIsCalled ? other : one);
    uint32_t smaller = unitedFunctions.sizeOfFamilyOf(one.index()) < unitedFunctions.sizeOfFamilyOf(other.index()) ? one.index() : other.index();
    Vector<uint32_t, 16> absorbedMembers;
    unitedFunctions.forEachMemberOfFamilyOf(smaller, [&](uint32_t member) {
        absorbedMembers.append(member);
    });
    UnitedFamilies::Union united = unitedFunctions.unite(one.index(), other.index());
    RELEASE_ASSERT(united.hasMerged());
    for (uint32_t member : absorbedMembers)
        RELEASE_ASSERT(unitedFunctions.representativeOf(member) == united.kept);
    if (m_familiesOfFunctionsAtUnseen.get(united.absorbed))
        m_familiesOfFunctionsAtUnseen.set(united.kept);
    if (m_calledFunctionFamilies.get(united.absorbed))
        m_calledFunctionFamilies.set(united.kept);
    uint32_t absorbed = m_poolOfFamilyOfFunctions[united.absorbed];
    if (absorbed == UINT32_MAX)
        return;
    uint32_t kept = m_poolOfFamilyOfFunctions[united.kept];
    if (kept == UINT32_MAX) {
        m_poolOfFamilyOfFunctions[united.kept] = absorbed;
        return;
    }
    m_poolsOfLostFunctionsToTie.append({ kept, absorbed });
    m_deferred.append(Deferred { Deferred::Kind::PoolsOfLostFunctionsAreUnited, 0, { } });
}

void LinkFlow::letFamilyOfLostFunctionsEscape(FunctionID member) const
{
    Lane* lane = s_laneAtWork;
    if (!lane && !m_tellsObservers) {
        noteFamilyOfFunctionsAtUnseen(member);
        return;
    }
    if (!m_familiesOfFunctionsAtUnseen.get(unitedFunctions.representativeWithoutShortening(member.index())))
        (lane ? lane->deferred : m_deferred).append(Deferred { Deferred::Kind::FamilyOfFunctionsEscapes, member.index(), { } });
}

void LinkFlow::noteCallOfFamilyMembers(FunctionID member) const
{
    unitedFunctions.forEachMemberOfFamilyOf(member.index(), [&](uint32_t other) {
        m_lostFunctionsThatCallsName.set(other);
    });
    m_someCallNamesLostFunctions = true;
}

void LinkFlow::noteFamilyOfFunctionsAtUnseen(FunctionID member) const
{
    if (!m_familiesOfFunctionsAtUnseen.add(unitedFunctions.representativeOf(member.index())))
        return;
    unitedFunctions.forEachMemberOfFamilyOf(member.index(), [&](uint32_t other) {
        m_membersOfFamiliesNewlyAtUnseen.append(FunctionID(other));
    });
}

Vector<FlowNodeID, 1> LinkFlow::nodesOfWhatFunctionAnswers(FunctionID id) const
{
    const Function& function = program.at(id);
    Vector<FlowNodeID, 1> nodes;
    if (function.has(FunctionFlags::IsAsync) && !function.has(FunctionFlags::IsGenerator)) {
        const ThinSummary& summary = summaries.at(id);
        for (uint32_t index = 1; index < summary.nodes.size(); ++index) {
            if (summary.nodes[index].kind == SummaryNode::Kind::Contents && summary.nodes[index].second == SlotOfContents::returnedAlone().number())
                nodes.append(nodeOf(id, index));
        }
        if (!nodes.isEmpty())
            return nodes;
    }
    for (uint32_t result = 0; result < function.results.size(); ++result)
        nodes.append(this->result(id, result));
    return nodes;
}

void LinkFlow::noteWhatMayMakeThenableOf(const LinkType& subject, FunctionID writer, std::optional<PropertyKeyID> writtenUnder, bool subjectMayBeWhatNobodyKnows, bool subjectIsMadeInBody)
{
    bool mayBeInstance = subject.classes != LinkType::Classes::None || (!subjectIsMadeInBody && (subject.builtin || subject.hasUnknownObjects || subject.holdsLostBuiltins));
    bool mayHideConstructor = !writtenUnder || (*writtenUnder ? nameClassOf(*writtenUnder) == NameClass::Proto || program.keyOf(BuiltinNames::constructor) == *writtenUnder : !m_isFunctionOfLibrary.quickGet(writer.index()));
    auto isBelowBuiltinClass = [&](LayoutID layout) {
        for (LayoutID current = layout; current; current = program.at(current).base) {
            if (builtinClassOfInstances(program.at(current)))
                return true;
        }
        return false;
    };
    auto isNoPlainObjectOfProgram = [&] {
        if (subject.objects == LinkType::Objects::Layout)
            return isBelowBuiltinClass(subject.layout);
        return subject.objects == LinkType::Objects::Set && std::ranges::any_of(layoutsOfSet(subject), isBelowBuiltinClass);
    };
    auto standsAboveWhatIsNoPlainObject = [&] {
        if (!writtenUnder || !*writtenUnder)
            return false;
        return subject.objects == LinkType::Objects::Prototypes || (subject.objects == LinkType::Objects::Prototype && isBelowBuiltinClass(subject.layout));
    };
    if ((mayBeInstance || isNoPlainObjectOfProgram() || standsAboveWhatIsNoPlainObject()) && mayHideConstructor) {
        arrive(whereInstancesLoseTheirConstructor(), LinkType::anything());
        note(Note::InstancesLoseTheirConstructor, 0);
    }
    bool namesSuchInstance = subject.classes == LinkType::Classes::Mixed || (subject.classes == LinkType::Classes::Site && !program.builtins().formatterWalksInstancesOf(subject.builtinClass));
    if (!namesSuchInstance && !(subjectMayBeWhatNobodyKnows && (subject.builtin || subject.hasUnknownObjects || subject.holdsLostBuiltins)))
        return;
    if (writtenUnder && *writtenUnder) {
        bool leavesThenAlone = nameClassOf(*writtenUnder) != NameClass::Proto && std::ranges::none_of(program.builtins().keysReadBy(Protocol::Thenable), [&](BuiltinName name) {
            return program.keyOf(name) == *writtenUnder;
        });
        if (leavesThenAlone)
            return;
    }
    if (writtenUnder && !*writtenUnder && !namesSuchInstance && m_isFunctionOfLibrary.quickGet(writer.index()))
        return;
    arrive(whereInstancesAreMadeThenables(), LinkType::anything());
}

FlowNodeID LinkFlow::whereInstancesLoseTheirConstructor()
{
    if (uint32_t made = m_whereInstancesLoseTheirConstructor.load(std::memory_order_acquire); made != noNodeInLinkFlow)
        return FlowNodeID(made);
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (m_whereInstancesLoseTheirConstructor.load(std::memory_order_relaxed) == noNodeInLinkFlow)
        m_whereInstancesLoseTheirConstructor.store(makeNode().index(), std::memory_order_release);
    return FlowNodeID(m_whereInstancesLoseTheirConstructor.load(std::memory_order_relaxed));
}

FlowNodeID LinkFlow::whereInstancesAreMadeThenables()
{
    if (uint32_t made = m_whereInstancesAreMadeThenables.load(std::memory_order_acquire); made != noNodeInLinkFlow)
        return FlowNodeID(made);
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (m_whereInstancesAreMadeThenables.load(std::memory_order_relaxed) == noNodeInLinkFlow)
        m_whereInstancesAreMadeThenables.store(makeNode().index(), std::memory_order_release);
    return FlowNodeID(m_whereInstancesAreMadeThenables.load(std::memory_order_relaxed));
}

FlowNodeID LinkFlow::gatheredByLostFunctions(uint32_t pool)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (m_poolsOfLostFunctions[pool].gathered == noNodeInLinkFlow)
        m_poolsOfLostFunctions[pool].gathered = makeNode().index();
    return FlowNodeID(m_poolsOfLostFunctions[pool].gathered);
}

FlowNodeID LinkFlow::parameterOfLostFunctions(const LinkType& callee, uint32_t positionalArgument)
{
    uint32_t family = familyOfLostFunctions(callee);
    return nodeAskedOnceByLane(MadeOnDemand::ParameterOfLostFunctions, static_cast<uint64_t>(family) << 32 | positionalArgument, [&]() -> FlowNodeID {
        return argumentOfLostFunctions(poolOfLostFunctions(family), positionalArgument);
    });
}

ALWAYS_INLINE_INTO_OTHER_FILES FlowNodeID LinkFlow::receiverOfLostFunctions(const LinkType& callee)
{
    uint32_t family = familyOfLostFunctions(callee);
    return nodeAskedOnceByLane(MadeOnDemand::ReceiverOfLostFunctions, family, [&]() -> FlowNodeID {
        return FlowNodeID(m_poolsOfLostFunctions[poolOfLostFunctions(family)].receiver);
    });
}

FlowNodeID LinkFlow::resultOfLostFunctions(const LinkType& callee)
{
    uint32_t family = familyOfLostFunctions(callee);
    return nodeAskedOnceByLane(MadeOnDemand::ResultOfLostFunctions, family, [&]() -> FlowNodeID {
        return FlowNodeID(m_poolsOfLostFunctions[poolOfLostFunctions(family)].result);
    });
}

FlowNodeID LinkFlow::laidOnLostClosures(const LinkType& callee)
{
    uint32_t family = familyOfLostFunctions(callee);
    return nodeAskedOnceByLane(MadeOnDemand::LaidOnLostClosures, family, [&]() -> FlowNodeID {
        return laidOnClosuresOfPool(poolOfLostFunctions(family));
    });
}

FlowNodeID LinkFlow::laidOnClosuresOfPool(uint32_t pool)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (m_poolsOfLostFunctions[pool].laidOnClosures == UINT32_MAX) {
        FlowNodeID laidOnClosures = makeNode();
        m_poolsOfLostFunctions[pool].laidOnClosures = laidOnClosures.index();
        for (FunctionID function : m_poolsOfLostFunctions[pool].functionsTied)
            tieWhatIsLaidOnClosuresOf(function, laidOnClosures);
    }
    return FlowNodeID(m_poolsOfLostFunctions[pool].laidOnClosures);
}

void LinkFlow::tieWhatIsLaidOnClosuresOf(FunctionID id, FlowNodeID laidOnClosuresOfPool)
{
    addEdge(foundOnClosuresOf(id), laidOnClosuresOfPool);
    for (uint32_t index = m_newestNameOnClosuresOf.isEmpty() ? UINT32_MAX : m_newestNameOnClosuresOf[id.index()]; index != UINT32_MAX; index = m_namesOnClosures[index].next)
        addEdge(m_namesOnClosures[index].laid, laidOnClosuresOfPool);
}

bool LinkFlow::lostFunctionsAreAtUnseen(const LinkType& callee) const
{
    return !callee.function || m_familiesOfFunctionsAtUnseen.get(unitedFunctions.representativeWithoutShortening(callee.function.index()));
}

FlowNodeID LinkFlow::madeForLostFunctionsWithoutClass()
{
    return fixedNodeInLinkFlow(FixedNodeOfLinkFlow::MadeForLostFunctionsWithoutClass);
}

void LinkFlow::noteCallOfLostFunctions(const LinkType& callee)
{
    if (m_lostFunctionsAreCalled)
        return;
    if (callee.functions != LinkType::Functions::Set) {
        if (!callee.function)
            note(Note::EveryLostFunctionCalled, 0);
        else if (!m_calledFunctionFamilies.get(unitedFunctions.representativeWithoutShortening(callee.function.index())))
            note(Note::LostFunctionsCalledByFamily, callee.function.index());
        return;
    }
    for (FunctionID member : functionsOfSet(callee)) {
        if (!m_lostFunctionsThatCallsName.get(member.index()))
            note(Note::LostFunctionCalled, member.index());
    }
}

bool LinkFlow::tieInLostFunctions()
{
    auto tiePoolsToPoolOfAll = [&] {
        if (!m_lostFunctionsAreCalled)
            return false;
        poolOfLostFunctions(anyLostFunction);
        bool hasTied = m_numberOfPoolsTiedToPoolOfAll < m_poolsOfLostFunctions.size();
        for (; m_numberOfPoolsTiedToPoolOfAll < m_poolsOfLostFunctions.size(); ++m_numberOfPoolsTiedToPoolOfAll)
            tiePoolsOfLostFunctions(poolOfAllLostFunctions, static_cast<uint32_t>(m_numberOfPoolsTiedToPoolOfAll));
        return hasTied;
    };
    bool hasTiedPools = tiePoolsToPoolOfAll();
    bool allAreCalled = m_lostFunctionsAreCalled;
    if ((!allAreCalled && !m_someCallNamesLostFunctions) || lostFunctions.bitCount() == m_numberOfLostFunctionsTied)
        return hasTiedPools;
    BitVector fresh = lostFunctions;
    if (!allAreCalled)
        fresh.filter(m_lostFunctionsThatCallsName);
    fresh.exclude(m_lostFunctionsTied);
    if (fresh.isEmpty())
        return hasTiedPools;
    m_lostFunctionsTied.merge(fresh);
    m_numberOfLostFunctionsTied += fresh.bitCount();
    for (size_t index : fresh)
        tieInLostFunction(FunctionID(index));
    tiePoolsToPoolOfAll();
    return true;
}

void LinkFlow::tieInLostFunction(FunctionID id)
{
    const Function& function = program.at(id);
    reach(id);
    uint32_t pool = poolOfLostFunctions(unitedFunctions.representativeOf(id.index()));
    FlowNodeID receiver(m_poolsOfLostFunctions[pool].receiver);
    FlowNodeID resultOfCalls(m_poolsOfLostFunctions[pool].result);
    m_poolsOfLostFunctions[pool].functionsTied.append(id);
    if (m_poolsOfLostFunctions[pool].laidOnClosures != UINT32_MAX)
        tieWhatIsLaidOnClosuresOf(id, FlowNodeID(m_poolsOfLostFunctions[pool].laidOnClosures));

    uint32_t firstPositional = function.firstParameterIndex();
    uint32_t numberOfPositional = function.numberOfPositionalParameters();
    uint32_t numberOfParameters = function.numberOfEntryParameters();
    bool gathersArguments = false;
    for (uint32_t index = 0; index < numberOfParameters; ++index) {
        FlowNodeID node = parameter(id, index);
        switch (function.entryParameterKind(index)) {
        case EntryParameter::Capture:
            break;
        case EntryParameter::This:
            addEdge(receiver, node);
            if (function.takesThisFromConstruction() && function.constructedClass) {
                noteAllocated(function.constructedClass);
                arriveFromSource(node, function.constructedClass);
                addEdge(node, resultOfCalls, TypeTable::object);
            } else if (function.takesThisFromConstruction())
                addEdge(fixedNodeInLinkFlow(FixedNodeOfLinkFlow::MadeForLostFunctionsWithoutClass), node);
            break;
        case EntryParameter::Callee:
            arrive(node, typeOfClosureInLinkFlow(id));
            break;
        case EntryParameter::NewTarget: {
            LinkType newTarget = LinkType::anything();
            newTarget.functions = LinkType::Functions::Closure;
            newTarget.function = id;
            arrive(node, newTarget);
            break;
        }
        case EntryParameter::ArgumentCount:
            arrive(node, LinkType::anything());
            break;
        case EntryParameter::Parameter:
            if (index - firstPositional < numberOfPositional) {
                addEdge(argumentOfLostFunctions(pool, index - firstPositional), node);
                arrive(node, typeOfPrimitivesInLinkFlow(Primitive::Undefined));
                break;
            }
            gathersArguments = true;
            break;
        case EntryParameter::HiddenRest:
            gathersArguments = true;
            break;
        }
    }

    uint32_t firstGathered = gathersArguments ? numberOfPositional : UINT32_MAX;
    if (gathersArguments)
        addEdge(gatheredByLostFunctions(pool), contents(listOfArgumentsOf(id), ContentsSlot::Element));
    if (gathersArguments && summaries.at(id).has(SummaryTrait::OnlyPassesOnGatheredArguments)) {
        addEdge(gatheredByLostFunctions(pool), argumentsGatheredAtAnyPlace(id));
        noteNumberOfArgumentsGathered(id, 0);
    }
    gatherArgumentsOfLostFunctionsFrom(pool, firstGathered);

    if (function.hasBody() && !function.isCoroutine() && function.results.size() == 1) {
        addEdge(result(id, 0), resultOfCalls);
        return;
    }
    for (uint32_t index = 0; index < function.results.size(); ++index)
        addEdge(result(id, index), unseen());
    arrive(resultOfCalls, LinkType::anything());
}

bool LinkFlow::letMembersOfLostEscape()
{
    bool hasDeferred = false;
    m_membersOfFamiliesNewlyAtUnseen.removeAllMatching([&](FunctionID member) {
        if (!lostFunctions.get(member.index()))
            return false;
        if (!escapedFunctions.get(member.index()))
            hasDeferred |= deferEscape(member);
        return true;
    });
    settle();
    return hasDeferred;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
