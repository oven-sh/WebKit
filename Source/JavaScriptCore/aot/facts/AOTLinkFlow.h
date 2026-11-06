/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinsInReach.h"
#include "AOTLargeList.h"
#include "AOTLinkTables.h"
#include "AOTLinkKeyOrders.h"
#include "AOTNameClass.h"
#include "AOTTestRuns.h"
#include "AOTThreads.h"
#include "AOTUnitedFamilies.h"
#include <array>
#include <bit>
#include <memory>
#include <set>
#include <span>
#include <type_traits>
#include <utility>
#include <wtf/threads/BinarySemaphore.h>
#include <wtf/SpinBackoff.h>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/ScopedLambda.h>
#include <wtf/SegmentedVector.h>

namespace JSC { namespace AOT {

struct BuiltinSlot;

namespace Typed {

using SiteID = ID<struct SiteIDTag>;

struct LinkType {
    enum class Objects : uint8_t {
        None,
        Layout,
        Set,
        Prototype,
        Prototypes,
        Family,
        Lost,
    };
    enum class Functions : uint8_t {
        None,
        Closure,
        Set,
        Constructors,
        Lost,
    };
    enum class Classes : uint8_t {
        None,
        Site,
        Mixed,
    };

    static constexpr uint16_t int32 = 1 << 14;
    static_assert(!(int32 & Primitive::Defined));
    static constexpr uint16_t oneSymbol = 1 << 15;
    static_assert(!(oneSymbol & Primitive::Defined));
    static constexpr uint16_t closed(uint16_t primitives)
    {
        if (primitives & Primitive::Integer)
            primitives |= int32;
        if (primitives & Primitive::Symbol)
            primitives |= oneSymbol;
        return primitives;
    }
    static constexpr uint16_t withoutLiterals(uint16_t primitives)
    {
        uint32_t widened = primitives & (Primitive::Defined | int32 | oneSymbol) & ~(Primitive::LiteralString | Primitive::LiteralSymbol);
        if (primitives & Primitive::LiteralString)
            widened |= Primitive::String;
        if (primitives & Primitive::LiteralSymbol)
            widened |= Primitive::Symbol;
        return closed(static_cast<uint16_t>(widened));
    }
    static constexpr bool holdsOneSymbol(uint16_t primitives) { return (primitives & (Primitive::Symbol | oneSymbol)) == oneSymbol; }
    static constexpr uint16_t primitivesOf(uint16_t primitivesOfType, Range integers)
    {
        if (!(primitivesOfType & Primitive::Integer))
            return primitivesOfType;
        return static_cast<uint16_t>(integers.isSubsetOf(Range::int32()) ? (primitivesOfType & ~Primitive::Integer) | int32 : primitivesOfType | int32);
    }
    static constexpr uint16_t inNamesOfPrimitive(uint16_t primitives)
    {
        if (primitives & Primitive::WholeNumber)
            primitives |= int32;
        if (primitives & int32)
            primitives = static_cast<uint16_t>((primitives & ~int32) | Primitive::Integer);
        if (primitives & oneSymbol)
            primitives = static_cast<uint16_t>((primitives & ~oneSymbol) | Primitive::Symbol);
        return primitives;
    }
    static constexpr Range integersOf(uint16_t primitives)
    {
        if (primitives & Primitive::Integer)
            return Range::full();
        return primitives & (int32 | Primitive::WholeNumber) ? Range::int32() : Range::empty();
    }
    TypeID primitivesAsType() const { return *tryMakeTypeOfPrimitives(inNamesOfPrimitive(primitives), integersOf(primitives)); }
    TypeID primitivesWithoutLiteralsAsType() const
    {
        uint16_t widened = withoutLiterals(primitives);
        return *tryMakeTypeOfPrimitives(inNamesOfPrimitive(widened), integersOf(widened));
    }

    uint16_t primitives { 0 };
    Objects objects : 3 { Objects::None };
    Functions functions : 3 { Functions::None };
    Classes classes : 2 { Classes::None };
    bool holdsUnseen : 1 { false };
    bool holdsLostBuiltins : 1 { false };
    bool unseenIsByEscapeAlone : 1 { false };
    uint8_t bitsNotInUse : 5 { 0 };
    uint16_t primitivesByEscapeAlone { 0 };
    uint16_t primitivesSeen() const { return primitives & ~primitivesByEscapeAlone; }
    bool holdsUnseenInSight() const { return holdsUnseen && !unseenIsByEscapeAlone; }
    bool hasSameShadowsAs(const LinkType& other) const { return primitivesSeen() == other.primitivesSeen() && holdsUnseenInSight() == other.holdsUnseenInSight(); }
    LinkType asSeen() const
    {
        LinkType seen = *this;
        seen.primitives = primitivesSeen();
        seen.holdsUnseen = holdsUnseenInSight();
        seen.primitivesByEscapeAlone = 0;
        seen.unseenIsByEscapeAlone = false;
        return seen;
    }
    LinkType byEscapeAlone() const
    {
        LinkType marked = *this;
        marked.primitivesByEscapeAlone = primitives;
        marked.unseenIsByEscapeAlone = holdsUnseen;
        return marked;
    }
    BuiltinClassID builtinClass;
    BuiltinID builtin;
    uint16_t bytesNotInUse { 0 };
    LayoutID layout;
    FamilyID family;
    FunctionID function;
    FamilyID constructorsOf;
    SiteID site;

    void dump(PrintStream&) const;

    ALWAYS_INLINE bool isNothing() const { return holdsSameAs(LinkType { }); }
    static LinkType anything()
    {
        LinkType type;
        type.primitives = closed(Primitive::All);
        type.holdsUnseen = true;
        return type;
    }
    ALWAYS_INLINE bool isAnything() const { return holdsSameAs(anything()); }
    bool holdsPrimitivesAlone() const { return objects == Objects::None && functions == Functions::None && classes == Classes::None && !builtin && !holdsUnseen; }

    using Words = std::array<uint64_t, 4>;
    ALWAYS_INLINE Words words() const { return std::bit_cast<Words>(*this); }
    static ALWAYS_INLINE Words wordsOfShadows()
    {
        LinkType marked;
        marked.unseenIsByEscapeAlone = true;
        marked.primitivesByEscapeAlone = UINT16_MAX;
        Words shadows = marked.words();
        Words nothing = LinkType { }.words();
        for (size_t index = 0; index < shadows.size(); ++index)
            shadows[index] ^= nothing[index];
        return shadows;
    }
    ALWAYS_INLINE Words wordsWithoutShadows() const
    {
        Words held = words();
        Words shadows = wordsOfShadows();
        for (size_t index = 0; index < held.size(); ++index)
            held[index] &= ~shadows[index];
        return held;
    }

    ALWAYS_INLINE bool holdsSameAs(const LinkType& other) const
    {
        if (primitives != other.primitives)
            return false;
        Words own = wordsWithoutShadows();
        Words ofOther = other.wordsWithoutShadows();
        uint64_t difference = 0;
        for (size_t index = 0; index < own.size(); ++index)
            difference |= own[index] ^ ofOther[index];
        return !difference;
    }

    friend bool operator==(const LinkType& first, const LinkType& second) { return first.holdsSameAs(second); }
};
static_assert(std::has_unique_object_representations_v<LinkType>);

template<typename MemberID>
class SetsOfFourInLinkFlow {
    WTF_MAKE_NONCOPYABLE(SetsOfFourInLinkFlow);
public:
    static constexpr size_t mostMembers = 4;

    SetsOfFourInLinkFlow() = default;
    ~SetsOfFourInLinkFlow()
    {
        for (auto& part : m_parts)
            delete[] part.load(std::memory_order_relaxed);
    }

    std::span<const MemberID> membersOf(uint32_t set) const
    {
        const Members& members = recordOf(set);
        size_t size = 2;
        while (size < mostMembers && members[size])
            ++size;
        return std::span<const MemberID>(members).first(size);
    }
    uint32_t numberOf(std::span<const MemberID> sortedMembers)
    {
        RELEASE_ASSERT(sortedMembers.size() >= 2 && sortedMembers.size() <= mostMembers);
        Members members { };
        std::ranges::copy(sortedMembers, members.begin());
        std::pair<uint64_t, uint64_t> words { static_cast<uint64_t>(members[0].bits()) << 32 | members[1].bits(), static_cast<uint64_t>(members[2].bits()) << 32 | members[3].bits() };
        while (m_isLocked.load(std::memory_order_relaxed) || m_isLocked.exchange(true, std::memory_order_acquire))
            simde_mm_pause();
        uint32_t next = m_size.load(std::memory_order_relaxed);
        auto added = m_numbers.add(words, next);
        if (added.isNewEntry) {
            auto [part, position] = placeOf(next);
            Members* records = m_parts[part].load(std::memory_order_relaxed);
            if (!records) {
                records = new Members[static_cast<size_t>(1) << (part + log2OfSetsInFirstPart)] { };
                m_parts[part].store(records, std::memory_order_release);
            }
            records[position] = members;
            m_size.store(next + 1, std::memory_order_release);
        }
        uint32_t number = added.iterator->value;
        m_isLocked.store(false, std::memory_order_release);
        return number;
    }
    uint32_t size() const { return m_size.load(std::memory_order_acquire); }

private:
    using Members = std::array<MemberID, mostMembers>;
    static constexpr unsigned log2OfSetsInFirstPart = 10;
    static constexpr size_t numberOfParts = 32 - log2OfSetsInFirstPart;

    static std::pair<unsigned, uint32_t> placeOf(uint32_t set)
    {
        uint32_t stretch = (set >> log2OfSetsInFirstPart) + 1;
        unsigned part = 31 - std::countl_zero(stretch);
        return { part, set - (((1u << part) - 1) << log2OfSetsInFirstPart) };
    }
    const Members& recordOf(uint32_t set) const
    {
        auto [part, position] = placeOf(set);
        return m_parts[part].load(std::memory_order_acquire)[position];
    }

    std::array<std::atomic<Members*>, numberOfParts> m_parts { };
    UncheckedKeyHashMap<std::pair<uint64_t, uint64_t>, uint32_t> m_numbers;
    std::atomic<uint32_t> m_size { 0 };
    std::atomic<bool> m_isLocked { false };
};
using SetsOfLayoutsInLinkFlow = SetsOfFourInLinkFlow<LayoutID>;
using SetsOfFunctionsInLinkFlow = SetsOfFourInLinkFlow<FunctionID>;

using FlowNodeID = ID<struct FlowNodeIDTag>;

struct FlowObserver {
    enum class Kind : uint8_t { SubjectOfCall, ReceiverOfCall, SubjectOfAccess, KeyOfAccess, SubjectOfChange, OtherOfChange, OperandOfOperator, Inspected, MemberOfSharedCall, MemberOfSharedAccess, MemberOfAccess, SymbolOfPreparation, PassedToBeNamed, FieldOfAccessorLaid, FieldIsOnlyAsked };

    Kind kind { Kind::SubjectOfCall };
    FunctionID function;
    uint32_t record { 0 };

    void dump(PrintStream&) const;

    friend bool operator==(const FlowObserver&, const FlowObserver&) = default;
};

struct BreachOfClosure {
    FlowNodeID node;
    LinkType before;
    LinkType after;
    ASCIILiteral set;
    FlowObserver told;
    FlowNodeID goneOver;
};

class StateOfLinkReach;
struct DeleterOfStateOfLinkReach {
    void operator()(StateOfLinkReach*) const;
};

class StateOfLinkCalls;
struct DeleterOfStateOfLinkCalls {
    void operator()(StateOfLinkCalls*) const;
};

class StateOfLinkAccesses;
struct DeleterOfStateOfLinkAccesses {
    void operator()(StateOfLinkAccesses*) const;
};

class StateOfLinkOperators;
struct DeleterOfStateOfLinkOperators {
    void operator()(StateOfLinkOperators*) const;
};

class WriterOfLinkLayouts;
struct DeleterOfWriterOfLinkLayouts {
    void operator()(WriterOfLinkLayouts*) const;
};

bool isPrivateNameInLink(const Program&, PropertyKeyID);

class LinkFlow {
    WTF_MAKE_NONCOPYABLE(LinkFlow);
public:
    LinkFlow(Program&, const ThinSummaries&, const Families&, const LooksAtBodies&, Mechanisms, BuildContext&, OrderOfWorklist = OrderOfWorklist::NewestFirst);
    ~LinkFlow();

    OrderOfWorklist orderOfWorklist() const { return m_order; }
    Program& program;
    const ThinSummaries& summaries;
    const Families& families;
    const Families* familiesBehindFlow { nullptr };
    const Vector<DeclaredSetForFamilies>* setsDeclaredInProgram { nullptr };
    const LooksAtBodies& looks;
    Mechanisms mechanisms;
    BuildContext& build;
    const ParallelLoops& loops;
    bool isGivenMore { false };

    FlowNodeID parameter(FunctionID function, uint32_t entryParameter) const
    {
        const NodesOfFunction& nodes = m_nodesOfFunctions[function.index()];
        RELEASE_ASSERT(entryParameter < nodes.firstResult - nodes.firstParameter);
        return FlowNodeID(nodes.firstParameter + entryParameter);
    }
    FlowNodeID result(FunctionID function, uint32_t index) const
    {
        const NodesOfFunction& nodes = m_nodesOfFunctions[function.index()];
        RELEASE_ASSERT(index < nodes.thrown - nodes.firstResult);
        return FlowNodeID(nodes.firstResult + index);
    }
    FlowNodeID thrown(FunctionID) const;
    FlowNodeID capture(FunctionID, uint32_t index) const;
    FlowNodeID variable(VariableID) const;
    FlowNodeID field(FieldReference reference) const
    {
        uint32_t first = m_firstFieldOfLayout[reference.layout.index()];
        RELEASE_ASSERT(reference.field < m_firstFieldOfLayout[reference.layout.index() + 1] - first);
        return FlowNodeID(m_nodeOfField[first + reference.field]);
    }
    FlowNodeID member(LayoutID classLayout, ClassSide side, uint32_t index) const
    {
        size_t sideIndex = 2 * static_cast<size_t>(classLayout.index()) + static_cast<size_t>(side);
        uint32_t first = m_firstMemberNodeOfSide[sideIndex];
        if (index >= m_firstMemberNodeOfSide[sideIndex + 1] - first)
            return unseen();
        return FlowNodeID(first + index);
    }
    enum class MakerOfSite : uint8_t {
        PartOfRow,
        WrapperAtCall,
        PairAtCall,
        ListOfArguments,
    };
    SiteID siteOf(MakerOfSite, uint32_t number, uint32_t part, BuiltinClassID);
    SiteID siteOfSummary(FunctionID, uint32_t rank, BuiltinClassID);
    uint32_t numberOfSitesOfSummary(FunctionID function) const { return m_firstSiteOfFunction[function.index() + 1] - m_firstSiteOfFunction[function.index()]; }
    SiteID listOfArgumentsOf(FunctionID);
    enum class CallerOfGatherer : uint8_t { InSight, UnseenByEscapeAlone };
    FlowNodeID argumentGatheredAt(FunctionID, uint32_t place);
    FlowNodeID argumentsGatheredAtAnyPlace(FunctionID);
    void noteNumberOfArgumentsGathered(FunctionID, uint32_t number, CallerOfGatherer = CallerOfGatherer::InSight);
    void gatherWhatListsThatArriveHold(FunctionID);
    void passOnGatheredArgument(FunctionID gatherer, uint32_t place, FlowNodeID to);
    void passOnGatheredArguments(FunctionID gatherer, uint32_t firstPlace, FunctionID gathererBehind);
    BuiltinClassID classOf(SiteID site) const
    {
        return m_sites[site.index()].builtinClass;
    }
    BuiltinLayoutID layoutOfRecordsOfFamilyOf(SiteID site) const
    {
        // Without the lock: a site never moves; it is written right behind siteOf(), by whoever has made the site, and where families merge, which is never while several threads are at work.
        return m_sites[m_familiesOfSites.representativeOf(site.index())].layoutOfRecords;
    }
    void noteRecordsAreMadeAt(SiteID, BuiltinLayoutID);
    bool mayHoldCellsOfCompiledLayouts(SiteID site) const
    {
        LockerOfWhatIsShared locker(*this);
        return m_sites[m_familiesOfSites.representativeOf(site.index())].mayHoldCellsOfCompiledLayouts;
    }
    size_t numberOfSites() const { return m_sites.size(); }
    struct ClassesOfSites {
        BitVector haveEscaped;
        BitVector areClimbedFrom;
    };
    ClassesOfSites findClassesOfSitesThatHaveEscapedOrAreClimbedFrom() const;
    static LinkType typeOf(SiteID, BuiltinClassID);
    LinkType typeOf(SiteID site) const { return typeOf(site, classOf(site)); }
    FlowNodeID contents(SiteID, SlotOfContents);
    void takeAsContents(SiteID, SlotOfContents, FlowNodeID);
    FlowNodeID contentsOf(const LinkType& type, SlotOfContents slot) { return type.classes == LinkType::Classes::None ? FlowNodeID() : contents(type.site, slot); }
    FlowNodeID findContentsOf(const LinkType& type, SlotOfContents slot) const { return type.classes == LinkType::Classes::None ? FlowNodeID() : findContents(m_familiesOfSites.representativeOf(type.site.index()), slot); }
    static SlotOfContents slotOf(const BuiltinSlot&);
    SlotOfContents slotOf(BuiltinSlotID) const;
    Vector<SlotOfContents, 2> slotsBehind(BuiltinClassID, SlotOfContents) const;
    void unite(SiteID, SiteID);
    void escape(SiteID);
    bool hasEscaped(SiteID site) const { return m_sites[m_familiesOfSites.representativeOf(site.index())].hasEscaped; }
    SiteID familyOf(SiteID site) const { return SiteID(m_familiesOfSites.representativeOf(site.index())); }
    FlowNodeID unseen() const { return FlowNodeID(0u); }
    Vector<FlowNodeID, 1> nodesOfWhatFunctionAnswers(FunctionID) const;
    FlowNodeID nodeOf(FunctionID function, uint32_t nodeInSummary) const
    {
        std::span<const NodesOfFunction> nodesOfFunctions = m_nodesOfFunctions.span();
        size_t index = function.index();
        RELEASE_ASSERT(index + 1 < nodesOfFunctions.size());
        uint32_t base = nodesOfFunctions[index].base;
        RELEASE_ASSERT(nodeInSummary < nodesOfFunctions[index + 1].firstParameter - base);
        return FlowNodeID(m_nodesOfSummaries[base - m_firstNodeOfFunctions + nodeInSummary]);
    }
    std::span<const uint32_t> nodesOfSummaryOf(FunctionID function) const LIFETIME_BOUND
    {
        uint32_t base = m_nodesOfFunctions[function.index()].base;
        return m_nodesOfSummaries.span().subspan(base - m_nodesOfFunctions[0].firstParameter, m_nodesOfFunctions[function.index() + 1].firstParameter - base);
    }
    void prefetchNodesOf(FunctionID) const;

    struct FoundMember {
        LayoutID classLayout;
        uint32_t index { 0 };
        MethodKind kind { MethodKind::Method };
    };
    class FoundMembers {
    public:
        const FoundMember* begin() const LIFETIME_BOUND { return m_members.begin(); }
        const FoundMember* end() const LIFETIME_BOUND { return m_members.end(); }
        size_t size() const { return m_members.size(); }
        const FoundMember& operator[](size_t index) const LIFETIME_BOUND { return m_members[index]; }
        bool mayAnswer() const { return !m_members.isEmpty(); }
        bool lookupGoesOnAboveClasses() const { return m_lookupGoesOnAboveClasses; }

    private:
        friend class LinkFlow;
        Vector<FoundMember, 2> m_members;
        bool m_lookupGoesOnAboveClasses { true };
    };
    FoundMembers findMembers(LayoutID, ClassSide, PropertyKeyID) const;
    bool namesMayBeIncomplete(LayoutID, ClassSide) const;
    bool someClassDeclaresMemberUnder(ClassSide side, PropertyKeyID key) const { return m_keysOfMembers[static_cast<size_t>(side)].get(key.index()); }
    bool findsPrototypeLinkOfSubject(LayoutID layout, ClassSide side, PropertyKeyID key, bool hasOtherReceiver) const { return !hasOtherReceiver && !findMembers(layout, side, key).mayAnswer(); }
    bool classesAnswerReadsUnderNumbers(LayoutID layout) const { return m_layoutsBelowMembersUnderNumbers.get(layout.index()) || namesMayBeIncomplete(layout, ClassSide::Prototype); }
    bool isAbsentAboveClasses(PropertyKeyID) const;
    LayoutID classOfConstructor(FunctionID) const;
    NameClass nameClassOf(PropertyKeyID key) const { return m_nameClassesOfKeys[key.index()]; }

    LayoutID layoutOfConstructorFunction(FunctionID) const;
    bool isOfConstructorFunction(LayoutID layout) const { return m_layoutsOfConstructorFunctions.get(layout.index()); }
    FlowNodeID storedOnPrototypeOf(LayoutID, PropertyKeyID);
    FlowNodeID foundOnPrototypesAbove(LayoutID, PropertyKeyID);
    FlowNodeID foundOnPrototypesAboveUnderNumbers(LayoutID);
    void lookForSetterOnPrototypesAbove(LayoutID, PropertyKeyID);
    bool isBelowConstructorFunction(LayoutID layout) const { return m_layoutsBelowConstructorFunctions.get(layout.index()); }
    bool mayNotBeLinkedYetBelow(LayoutID from, LayoutID classAbove) const;
    FlowNodeID objectsMadeForClassBelowConstructorFunction(LayoutID);
    FlowNodeID objectsMadeThroughConstructorOf(LayoutID);
    void defineOnPrototypeOf(LayoutID, PropertyKeyID);
    void openPrototypeOf(LayoutID);
    struct NameOnPrototype {
        LayoutID layout;
        PropertyKeyID key;
        FlowNodeID stored;
        FlowNodeID storedInSight;
        bool isStoredInto { false };
        bool isPassedByLookups { false };
        bool isDefinedByDescriptor { false };
        bool isReadUnderAnyKey { false };
        uint32_t nextOfLayout { UINT32_MAX };
    };
    std::span<const NameOnPrototype> namesOnPrototypes() const LIFETIME_BOUND { return m_namesOnPrototypes.span(); }

    struct NameOnClosures {
        FunctionID function;
        PropertyKeyID key;
        FlowNodeID laid;
        uint32_t next { UINT32_MAX };
    };
    bool followsNamesOnClosuresOf(FunctionID, PropertyKeyID) const;
    FlowNodeID laidOnClosuresOf(FunctionID, PropertyKeyID);
    void tieNamesOnLostClosuresOf(FunctionID);
    std::span<const NameOnClosures> namesOnClosures() const LIFETIME_BOUND { return m_namesOnClosures.span(); }
    const NameOnPrototype* findNameOnPrototype(LayoutID, PropertyKeyID) const LIFETIME_BOUND;
    bool mayLieOnPrototypeAbove(LayoutID, PropertyKeyID, bool countsKeysThatNobodyKnows = true) const;

    FlowNodeID foundBesideFieldsOf(LayoutID);
    FlowNodeID storedBesideFieldsOf(LayoutID);
    Vector<FlowNodeID, 8> nodesWithOwnValuesOf(LayoutID);
    Vector<FlowNodeID, 4> cellsOfWrittenPrivateNamesOf(LayoutID);
    bool isInspectedByComparisonOfPrivateFields(FlowNodeID) const;
    struct OwnValue {
        PropertyKeyID key;
        FlowNodeID node;
    };
    std::span<const OwnValue> ownValuesOf(LayoutID) LIFETIME_BOUND;
    FlowNodeID foundBesideFieldsUnder(PropertyKeyID);
    FlowNodeID appendedField(LayoutID, PropertyKeyID);
    FlowNodeID findAppendedField(LayoutID, PropertyKeyID) const;
    void forEachAppendedField(const Invocable<void(PropertyKeyID, FlowNodeID)> auto& functor) const
    {
        for (const AppendedField& field : m_appendedFields)
            functor(field.key, field.node);
    }
    FlowNodeID foundOnClosuresOf(FunctionID);
    void openClosuresOf(FunctionID);

    FlowNodeID fieldReadThrough(FamilyID, PropertyKeyID);
    FlowNodeID fieldStoredThrough(FamilyID, PropertyKeyID, bool isPartOfConstruction = false);
    FlowNodeID fieldReadThroughLost(LayoutID member, PropertyKeyID);
    FlowNodeID fieldStoredThroughLost(LayoutID member, PropertyKeyID);
    void letLostFamilyEscape(LayoutID member);
    FlowNodeID anyFieldReadThrough(LinkType::Objects, LayoutID, FamilyID, bool keyIsNumber = false);
    FlowNodeID anyFieldStoredThrough(LinkType::Objects, LayoutID, FamilyID, bool keyIsNumber = false);
    FlowNodeID anyFieldDefinedOn(LayoutID);
    FlowNodeID whatStandsAbove(LayoutID layout) { return nodeOnDemand(NodeOnDemand::WhatStandsAbove, layout.index(), { }).first; }
    FlowNodeID whatStandsAbove(SiteID site) { return nodeOnDemand(NodeOnDemand::WhatStandsAbove, m_familiesOfSites.representativeWithoutShortening(site.index()) | siteAmongNodesOnDemand, { }).first; }
    FlowNodeID findWhatStandsAbove(LayoutID layout) const { return findNodeOnDemand(NodeOnDemand::WhatStandsAbove, layout.index(), { }); }
    FlowNodeID findWhatStandsAbove(SiteID site) const { return findNodeOnDemand(NodeOnDemand::WhatStandsAbove, m_familiesOfSites.representativeWithoutShortening(site.index()) | siteAmongNodesOnDemand, { }); }
    FlowNodeID whatStandsBelow(LayoutID layout) { return nodeOnDemand(NodeOnDemand::WhatStandsBelow, layout.index(), { }).first; }
    FlowNodeID whatStandsBelow(SiteID site) { return nodeOnDemand(NodeOnDemand::WhatStandsBelow, m_familiesOfSites.representativeWithoutShortening(site.index()) | siteAmongNodesOnDemand, { }).first; }
    static constexpr unsigned mostStepsAboveCreatedObject = 8;
    FlowNodeID findWhatConstructorLaysAbove(FunctionID, const SummaryChange&) const;
    FlowNodeID heirsThatRun(FunctionID member) { return nodeOnDemand(NodeOnDemand::WhatStandsBelow, member.index() | functionAmongNodesOnDemand, { }).first; }
    FlowNodeID findHeirsThatRun(FunctionID member) const { return findNodeOnDemand(NodeOnDemand::WhatStandsBelow, member.index() | functionAmongNodesOnDemand, { }); }
    FlowNodeID laidByDescriptorsOn(LayoutID layout, PropertyKeyID key) { return nodeOnDemand(NodeOnDemand::AnyFieldDefinedOnLayout, layout.index(), key).first; }
    FlowNodeID findLaidByDescriptorsOn(LayoutID layout, PropertyKeyID key) const { return findNodeOnDemand(NodeOnDemand::AnyFieldDefinedOnLayout, layout.index(), key); }
    bool hasSetterWritten(FieldReference field) const { return m_fieldsWithSettersWritten.get(m_firstFieldOfLayout[field.layout.index()] + field.field); }
    FlowNodeID parameterOfLostFunctions(uint32_t positionalArgument);
    FlowNodeID receiverOfLostFunctions();
    FlowNodeID resultOfLostFunctions();
    FlowNodeID madeForLostFunctionsWithoutClass();
    void noteCallOfLostFunctions(const LinkType& callee);

    LinkType typeAt(FlowNodeID id) const
    {
        if (id.index() >= m_nodes.size())
            return { };
        const LinkType& type = m_nodes[id.index()].type;
        if (!m_flowIsAtRest)
            return type;
        if (ViewAtWork::current())
            return type.asSeen();
        if (type.holdsUnseen && m_typesAreSettled && (type.functions != LinkType::Functions::None || type.classes != LinkType::Classes::None))
            return settled(type);
        return type;
    }
    void prefetchTypeAt(FlowNodeID id) const { __builtin_prefetch(&m_nodes[id.index()].type); }
    bool holdsInstancesOfOneSiteAt(FlowNodeID id) const { return id.index() < m_nodes.size() && m_nodes[id.index()].type.classes == LinkType::Classes::Site; }
    FunctionID closureAloneAt(FlowNodeID id) const
    {
        if (id.index() >= m_nodes.size() || m_nodes[id.index()].type.functions != LinkType::Functions::Closure)
            return { };
        return typeAt(id).function;
    }
    void arrive(FlowNodeID, const LinkType&, PropertyKeyID symbol = { });
    PropertyKeyID symbolAt(FlowNodeID node) const { return LinkType::holdsOneSymbol(typeAt(node).primitives) ? symbolNotedAt(node) : PropertyKeyID(); }
    PropertyKeyID symbolSeenAt(FlowNodeID node) const { return LinkType::holdsOneSymbol(typeAt(node).primitivesSeen()) ? symbolNotedAt(node) : PropertyKeyID(); }
    PropertyKeyID keySeenOf(FunctionID, const SummaryAccess&) const;
    static bool isSymbolAsFarAsIsSeen(const LinkType& key)
    {
        if (key.objects != LinkType::Objects::None || key.functions != LinkType::Functions::None || key.classes != LinkType::Classes::None || key.builtin || key.holdsLostBuiltins || key.holdsUnseenInSight())
            return false;
        return !(key.primitivesSeen() & LinkType::closed(Primitive::All & ~Primitive::UndefinedOrNull) & ~LinkType::oneSymbol);
    }
    void addEdge(FlowNodeID from, FlowNodeID to, TypeID passes = { });
    void observe(FlowNodeID, const FlowObserver&);
    void run();
    void settleTypes();
    void countAllocatedMembersOfFamilies();
    FamilyID familyWhoseAllocatedMembersAre(std::span<const LayoutID>) const;
    LinkType settled(LinkType) const;
    LinkType withoutObjectsThatHaveEscaped(LinkType) const;
    Vector<BreachOfClosure> findBreachesOfClosure();
    void describe(PrintStream&, FlowNodeID) const;
    SizeOfLinkFlow size() const;
    void forgetWaysOfFlow();
    class UnknownByEscapeAlone {
        WTF_MAKE_NONCOPYABLE(UnknownByEscapeAlone);
    public:
        enum class Marks : uint8_t { Nothing, OfRule, OfEscape };
        explicit UnknownByEscapeAlone(bool isSo, Marks marks = Marks::OfEscape)
            : m_outer(std::exchange(s_marksAtWork, isSo ? marks : Marks::Nothing))
        {
        }
        ~UnknownByEscapeAlone() { s_marksAtWork = m_outer; }
        static Marks marksAtWork() { return s_marksAtWork; }
    private:
        static constinit thread_local Marks s_marksAtWork;
        Marks m_outer;
    };
    static bool isUnknownByEscapeAloneIfAtAll(const LinkType& type) { return !type.holdsUnseenInSight(); }

    mutable std::unique_ptr<WriterOfLinkLayouts, DeleterOfWriterOfLinkLayouts> writerOfLayouts;

    std::span<const LayoutID> layoutsOfSet(const LinkType& type) const LIFETIME_BOUND
    {
        ASSERT(type.objects == LinkType::Objects::Set);
        return m_setsOfLayouts.membersOf(type.layout.index());
    }
    std::span<const LayoutID> layoutsOfFamily(FamilyID) const LIFETIME_BOUND;
    LinkType typeOfLayouts(std::span<const LayoutID> sortedMembers) const;
    enum class LayoutsNamed : uint8_t { Exactly, AtMost };
    LayoutsNamed forEachLayoutOf(const LinkType& type, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
    {
        switch (type.objects) {
        case LinkType::Objects::None:
        case LinkType::Objects::Prototype:
        case LinkType::Objects::Prototypes:
            return LayoutsNamed::Exactly;
        case LinkType::Objects::Layout:
            functor(type.layout);
            return LayoutsNamed::Exactly;
        case LinkType::Objects::Set:
            for (LayoutID member : layoutsOfSet(type))
                functor(member);
            return LayoutsNamed::Exactly;
        case LinkType::Objects::Family:
            for (LayoutID member : layoutsOfFamily(type.family))
                functor(member);
            return LayoutsNamed::AtMost;
        case LinkType::Objects::Lost:
            if (type.layout) {
                for (LayoutID member : membersOfLostFamily(unitedLayouts.representativeWithoutShortening(type.layout.index())))
                    functor(member);
            }
            return LayoutsNamed::AtMost;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    uint32_t numberOfSetsOfLayouts() const { return m_setsOfLayouts.size(); }
    std::span<const FunctionID> functionsOfSet(const LinkType& type) const LIFETIME_BOUND
    {
        ASSERT(type.functions == LinkType::Functions::Set);
        return m_setsOfFunctions.membersOf(type.function.index());
    }
    LinkType typeOfFunctions(std::span<const FunctionID> sortedMembers) const;

    LinkType joinOf(const LinkType&, const LinkType&);
    LinkType passedBy(const LinkType&, TypeID test) const;
    LinkType fromTypeID(TypeID) const;
    TypeID toTypeID(TypeArena&, const LinkType&) const;
    void nameLayoutsThatMet(TypeArena&);
    void forgetNamesThatAreNoFamilies(const Families& behindFlow);
    std::span<const LayoutSetID> layoutsThatMet() const LIFETIME_BOUND { return m_layoutsThatMet.span(); }
    bool tablesMayName(FunctionID function) const
    {
        const BitVector* functions = ViewAtWork::current() ? ViewAtWork::current()->functionsThatTablesMayName : nullptr;
        return !functions || functions->get(function.index());
    }

    void reach(FunctionID);
    void becomeLost(LayoutID);
    void becomeLost(FamilyID);
    void becomeLost(FunctionID);
    void escape(LayoutID);
    void escape(FunctionID);
    void escapeAllOf(const LinkType&);
    void arriveFromSource(FlowNodeID, LayoutID);
    void arriveFromSource(FlowNodeID, FamilyID);
    void arriveFromSource(FlowNodeID, const LinkType&);
    void arriveFromSource(FlowNodeID, TypeID);
    void noteAllocated(LayoutID);
    void noteAllocatedWithRowAbove(LayoutID);
    void reachEntries();
    bool enterLibraryInReach();
    BuiltinsInReach& builtinsInReach();
    bool takeForConstant(BuiltinID member);
    bool isOutOfSight(BuiltinID canonical) const;
    const BuiltinCondition* lightFormOf(BuiltinID canonical) const;
    bool isAskedForOtherNames(BuiltinID canonical) const;
    bool isWorthFollowingToWrites(BuiltinID canonical) const;

    void assertHasWhatIsSharedToItself() const { RELEASE_ASSERT((!m_severalThreadsTell && !s_laneAtWork) || s_depthOfLockOfWhatIsShared); }
    template<typename T>
    class Shared {
    public:
        T& of(const LinkFlow& flow)
        {
            flow.assertHasWhatIsSharedToItself();
            return m_table;
        }

    private:
        T m_table;
    };
    class LockerOfWhatIsShared {
        WTF_MAKE_NONCOPYABLE(LockerOfWhatIsShared);
    public:
        explicit LockerOfWhatIsShared(const LinkFlow& flow) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
            : m_flow(flow.m_severalThreadsTell ? &flow : nullptr)
        {
            if (m_flow && !s_depthOfLockOfWhatIsShared++)
                m_flow->lockWhatIsShared();
        }
        ~LockerOfWhatIsShared() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
        {
            if (m_flow && !--s_depthOfLockOfWhatIsShared)
                m_flow->m_whatIsSharedIsLocked.store(false, std::memory_order_release);
        }

    private:
        const LinkFlow* m_flow;
    };
    enum class MadeOnDemand : uint8_t { Contents, ContentsOfFamily, TakenForConstant, FieldReadThroughLost, FieldStoredThroughLost, FieldReadThrough, FieldStoredThrough, FoundOnPrototypesAbove, FoundBesideFieldsUnder, FoundBesideFieldsOf, StoredBesideFieldsOf, FoundWhereFieldIsEmpty, AppendedField, FoundOnClosuresOf, LaidOnClosuresOf, WrittenAtMember, ParameterOfLostFunctions, ReceiverOfLostFunctions, ResultOfLostFunctions, NameAddedByTests, FunctionOnObjectPrototype, AnyFieldDefinedOn, AnyFieldReadThrough, AnyFieldStoredThrough, ResultOfOperator };
    static constexpr size_t numberOfKindsMadeOnDemand = static_cast<size_t>(MadeOnDemand::ResultOfOperator) + 1;
    uint32_t askedOnceByLane(MadeOnDemand kind, uint64_t words, const Invocable<uint32_t()> auto& make) const
    {
        if (!m_severalThreadsTell)
            return make();
        if (std::optional<uint32_t> known = findWhatLaneWasGiven(kind, words))
            return *known;
        if (std::optional<uint32_t> known = findWhatSomeLaneWasGiven(kind, words)) {
            keepWhatLaneWasGiven(kind, words, *known);
            return *known;
        }
        uint32_t made;
        {
            LockerOfWhatIsShared locker(*this);
            made = make();
        }
        keepWhatLaneWasGiven(kind, words, made);
        keepWhatSomeLaneWasGiven(kind, words, made);
        return made;
    }
    FlowNodeID nodeAskedOnceByLane(MadeOnDemand kind, uint64_t words, const Invocable<FlowNodeID()> auto& make) const
    {
        return FlowNodeID::fromBits(askedOnceByLane(kind, words, [&]() -> uint32_t {
            return make().bits();
        }));
    }
    std::optional<uint32_t> findWhatLaneWasGiven(MadeOnDemand, uint64_t words) const;
    void keepWhatLaneWasGiven(MadeOnDemand, uint64_t words, uint32_t) const;
    std::optional<uint32_t> findWhatSomeLaneWasGiven(MadeOnDemand, uint64_t words) const;
    void keepWhatSomeLaneWasGiven(MadeOnDemand, uint64_t words, uint32_t) const;
    void forgetWhatSomeLaneWasGiven(MadeOnDemand);
    enum class Note : uint8_t { NameRead, NameCalled, ReadUnderKeyNobodyKnows, BuiltinRead, BuiltinCalled, BuiltinCalledOutOfSight, FieldWrittenAfterConstruction, LayoutAllocated, ChangeOfLayout, WriteOnBuiltin, BuiltinHandedWhatIsNoTextNumberOrBoolean, WriteThroughLostBuiltins, ReadThroughLostBuiltins, WriteThroughUnseenByProgram, NameWrittenThroughUnseenByLibrary, OptionThatRunsCodePassed, OperandMayMeetCondition, ObjectHandedToFormatter, TypeHandedToHandler, BuiltinReadThroughAggregate, ExportIsShown, BuiltinCalledAtSite, LostFunctionCalled, EveryLostFunctionCalled, LayoutGainsWhatUnseenCodeGaveSources, LayoutDefinedOnUnderWrittenKeys, PrototypeLaidAboveOtherObjects, FieldMayBeEmpty, PrototypeObjectRead };
    void note(Note, uint32_t number, uint32_t more = 0);
    static constexpr uint32_t byCode = 1;
    static constexpr uint32_t byRowThatMayRunCode = 2;
    static constexpr uint32_t asConstructor = 4;
    static constexpr uint32_t withoutRow = 1;
    static constexpr uint32_t onlyDeletes = 1 << 16;
    unsigned numberOfLanes() const { return static_cast<unsigned>(m_lanes.size()); }
    bool bringAliveOnLanes(std::span<const FunctionID>, const ScopedLambda<void(FunctionID)>& bringAlive);
    bool takeStepsOfReachOnLanes(size_t numberOfSteps, const ScopedLambda<void(unsigned lane)>& takeSteps);
    bool severalThreadsTell() const { return m_severalThreadsTell; }
    bool lanesTakeStepsOfReach() const { return m_lanesTakeStepsOfReach && m_severalThreadsTell; }
    struct LeftToReach {
        uint8_t what;
        uint32_t index;
    };
    void leaveToReach(uint8_t what, uint32_t index);
    Vector<LeftToReach> takeWhatLaneLeftToReach(unsigned lane);
    bool listsSourceOfObjectsBehindPass(FlowNodeID, LayoutID);
    bool tellsBuiltinsInReachBehindPass(FunctionID);
    void listSourceOfObjects(FlowNodeID, LayoutID);
    bool letsReachWait() const { return m_letsReachWait; }
    unsigned laneOfObserversOf(FunctionID function) const { return laneIndexOf(FlowNodeID(m_nodesOfFunctions[function.index()].firstParameter)); }
    unsigned indexOfLaneAtWork() const;
    const LinkType& typeObservedBefore() const LIFETIME_BOUND;
    class CodeAtWork {
        WTF_MAKE_NONCOPYABLE(CodeAtWork);
    public:
        CodeAtWork(LinkFlow& flow, FunctionID function)
            : m_flow(flow)
            , m_wasOfLibrary(flow.setCodeAtWorkIsOfLibrary(flow.m_isFunctionOfLibrary.quickGet(function.index())))
        {
        }
        ~CodeAtWork() { m_flow.setCodeAtWorkIsOfLibrary(m_wasOfLibrary); }

    private:
        LinkFlow& m_flow;
        bool m_wasOfLibrary;
    };
    bool setCodeAtWorkIsOfLibrary(bool);
    bool codeAtWorkIsOfLibrary() const;
    bool rowsSpeakFor(FunctionID function) const { return m_isFunctionThatRowsSpeakFor.quickGet(function.index()); }
    bool rowsSpeakOfKeyFor(FunctionID function, PropertyKeyID key) const { return rowsSpeakFor(function) && !m_keysThatCodeTellsOfItself.contains(key); }
    bool objectsReachUnseen() const { return m_objectsReachUnseen; }
    void noteRowPassesWhatNobodyFollows() { LockerOfWhatIsShared locker(*this); m_rowPassesWhatNobodyFollows = true; }
    bool rowPassesWhatNobodyFollows() const { return m_rowPassesWhatNobodyFollows; }
    bool letPromisesBeHandedOut();
    void noteLossOf(BuiltinID builtin) const
    {
        if (builtin)
            noteLossOfBuiltin(builtin);
    }
    Vector<BuiltinID> takeBuiltinsLost() { return std::exchange(m_builtinsLost, { }); }
    Vector<BuiltinID> takeBuiltinsLostInSight() { return std::exchange(m_builtinsLostInSight, { }); }
    Vector<BuiltinID> takeBuiltinsAtUnseen() { return std::exchange(m_builtinsAtUnseen, { }); }
    void handToAccessorsLaidAbove(FlowNodeID receiver);
    bool letGoWhoWaitsForAccessorsLaidAbove();
    bool lostBuiltinsReachUnseen() const { return m_lostBuiltinsReachUnseen; }
    LinkType anythingBeside(BuiltinID canonical, FunctionID reader) const;
    LinkType unseenObjectsBelow(BuiltinID canonicalPrototype, FunctionID maker) const;
    enum class WhatIsStored : uint8_t { Anything, TextsNumbersAndBooleans, Nothing };
    void noteWriteOn(const LinkType& subject, PropertyKeyID, FunctionID writer, WhatIsStored = WhatIsStored::Anything);
    void noteWhatIsStoredOn(const LinkType& subject, WhatIsStored);
    void noteWhatMayMakeThenableOf(const LinkType& subject, FunctionID writer, std::optional<PropertyKeyID> writtenUnder, bool subjectMayBeWhatNobodyKnows = true);
    FlowNodeID whereInstancesAreMadeThenables();
    FlowNodeID whereInstancesLoseTheirConstructor();
    void noteWhatNativeCodeHandsToWhatIsLaidOn(BuiltinID written, PropertyKeyID) const;
    void findWhatNativeCodeHandsToLibrary();
    Vector<std::pair<BuiltinID, BuiltinID>> m_holdersAtOrAboveWhatNativeCodeHandsToLibrary;
    void noteSiteOfWrite(FunctionID writer, ValueID site, FlowNodeID subject);
    void noteSiteOfCopy(FunctionID writer, ValueID site, FlowNodeID subject, FlowNodeID source);
    Vector<WriteFollowedToBuiltin> takeWritesFollowedToBuiltins();
    bool appendNamesOfObjectsAmong(const LinkType&, Vector<PropertyKeyID>&) const;
    LinkType builtinsAbove(const LinkType&, FunctionID asker);
    BuiltinID prototypeOfInstancesOf(BuiltinClassID) const;
    bool isClimbedFrom(SiteID site) const { return m_sites[m_familiesOfSites.representativeOf(site.index())].isClimbedFrom; }
    FlowNodeID writtenAtMember(BuiltinID member);
    FlowNodeID laidOnBuiltin(BuiltinID holder, PropertyKeyID key) { return nodeOnDemand(NodeOnDemand::LaidOnClosuresUnderKey, holder.index() | builtinAmongNodesOnDemand, key).first; }
    FlowNodeID findLaidOnBuiltin(BuiltinID holder, PropertyKeyID key) const { return findNodeOnDemand(NodeOnDemand::LaidOnClosuresUnderKey, holder.index() | builtinAmongNodesOnDemand, key); }
    FlowNodeID findWrittenAtMember(BuiltinID member) const;
    TestRunsInLink& testRuns() { return *m_testRuns; }
    const TestRunsInLink& testRuns() const { return *m_testRuns; }

    BitVector reached;
    BitVector allocatedLayouts;
    BitVector lostLayouts;
    UnitedFamilies unitedLayouts;
    BitVector lostFunctions;
    BitVector escapedLayouts;
    BitVector prototypesLaidAboveOtherObjects;
    Vector<TaggedMaker> taggedMakers;

    struct CopyLookedAt {
        LinkType subject;
        LinkType other;
        bool waitsForNothingBesideFields { false };
        bool waitsForWhatIsSeenAlone { false };
    };
    struct CopiesObserved {
        HashMap<uint64_t, CopyLookedAt, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> lookedAt;
        HashSet<std::pair<uint64_t, uint32_t>> toldOfWhatLiesBesideFields;
        HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> accessorsThatWait;
    };
    CopiesObserved& copiesObservedIn(FunctionID);

    void noteAccessorLaid(LayoutID, const AccessorLaidInField&);
    BitVector escapedFunctions;
    mutable BitVector layoutsMetInFamilies;
    bool mayBeKnownByFamily(LayoutID layout) const { return layoutsMetInFamilies.get(layout.index()) || lostLayouts.get(layout.index()); }
    void noteSomethingBefell(FamilyID family) const { m_outermostFamiliesWhereSomethingBefell.quickSet(m_outermostFamilies[family.index()].index()); }
    BitVector escapedVariables;
    BitVector variablesStoredFirst;
    Vector<LinkedVariableStoredFirst> firstReadersOfVariablesStoredFirst;
    BitVector libraryFunctionsEntered;
    BitVector exportsShown;
    void showExport(ModuleID, uint32_t index);

    void callObserved(const FlowObserver&, const LinkType&);
    bool resolveCallsThatWereTold();
    void inspectionObserved(const FlowObserver&, const LinkType&);
    void accessObserved(const FlowObserver&, const LinkType&);
    PropertyKeyID keyOf(FunctionID, const SummaryAccess&) const;
    bool everySymbolOfLibraryMayHavePreparation() const { return m_everySymbolOfLibraryMayHavePreparation; }
    Vector<PropertyKeyID> symbolsWithPreparation() const;
    void noteSymbolHandedToPreparation(FunctionID caller, FlowNodeID);
    enum class UnknownSymbolIs : uint8_t { UnknownInSightToo, UnknownByEscapeAlone, UnknownInSightAlone };
    void noteUnknownSymbolHandedToPreparation(UnknownSymbolIs = UnknownSymbolIs::UnknownInSightToo);
    void preparationObserved(const FlowObserver&, const LinkType&);
    void noteKeyOfAccessInLibrary(FunctionID, uint32_t record, const SummaryAccess&);
    void changeObserved(const FlowObserver&, const LinkType&);
    void passedToBeNamedObserved(const FlowObserver&, const LinkType&);
    bool isPlainClosureMadeWith(FunctionID, PropertyKeyID) const;
    void operatorObserved(const FlowObserver&, const LinkType&);
    std::optional<uint16_t> primitivesThatOperatorGivesWhateverArrives(FunctionID, uint32_t record);

    struct ListsOfCalleesOnHeap {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ListsOfCalleesOnHeap);
        Vector<FunctionID, 4> listed;
        Vector<BuiltinID, 4> builtins;
        Vector<uint32_t, 2> shared;
    };
    struct InlineRoomOfCalleesOfCall {
        union {
            std::array<FunctionID, 2> inlineListed { };
            ListsOfCalleesOnHeap* listsOnHeap;
        };
        std::array<uint32_t, 1> inlineShared { };
        std::array<BuiltinID, 1> inlineBuiltins { };
        bool mayEnterLost : 1 { false };
        bool mayEnterUnseen : 1 { false };
        bool liesOnHeap : 1 { false };
        uint8_t numberOfInlineListed : 2 { 0 };
        uint8_t numberOfInlineBuiltins : 1 { 0 };
        uint8_t numberOfInlineShared : 1 { 0 };
    };
    class CalleesOfCall : private InlineRoomOfCalleesOfCall {
    public:
        template<typename Member>
        class List {
        public:
            explicit List(std::span<const Member> members)
                : m_members(members)
            {
            }

            auto begin() const { return m_members.begin(); }
            auto end() const { return m_members.end(); }
            size_t size() const { return m_members.size(); }
            bool isEmpty() const { return m_members.empty(); }
            bool contains(Member member) const { return std::ranges::contains(m_members, member); }
            Member operator[](size_t index) const
            {
                RELEASE_ASSERT(index < m_members.size());
                return m_members[index];
            }
            std::span<const Member> span() const { return m_members; }

        private:
            std::span<const Member> m_members;
        };

        using InlineRoomOfCalleesOfCall::mayEnterLost;
        using InlineRoomOfCalleesOfCall::mayEnterUnseen;

        CalleesOfCall() = default;
        CalleesOfCall(const CalleesOfCall& other)
            : InlineRoomOfCalleesOfCall(other)
        {
            if (liesOnHeap)
                listsOnHeap = new ListsOfCalleesOnHeap(*other.listsOnHeap);
        }
        CalleesOfCall(CalleesOfCall&& other)
            : InlineRoomOfCalleesOfCall(std::exchange<InlineRoomOfCalleesOfCall>(other, { }))
        {
        }
        ~CalleesOfCall()
        {
            if (liesOnHeap)
                delete listsOnHeap;
        }
        CalleesOfCall& operator=(CalleesOfCall other)
        {
            std::swap<InlineRoomOfCalleesOfCall>(*this, other);
            return *this;
        }

        List<FunctionID> listed() const LIFETIME_BOUND { return List<FunctionID>(liesOnHeap ? listsOnHeap->listed.span() : std::span<const FunctionID>(inlineListed).first(numberOfInlineListed)); }
        List<BuiltinID> builtins() const LIFETIME_BOUND { return List<BuiltinID>(liesOnHeap ? listsOnHeap->builtins.span() : std::span<const BuiltinID>(inlineBuiltins).first(numberOfInlineBuiltins)); }
        List<uint32_t> shared() const LIFETIME_BOUND { return List<uint32_t>(liesOnHeap ? listsOnHeap->shared.span() : std::span<const uint32_t>(inlineShared).first(numberOfInlineShared)); }

        void list(FunctionID callee)
        {
            if (!liesOnHeap && numberOfInlineListed < inlineListed.size()) {
                inlineListed[numberOfInlineListed++] = callee;
                return;
            }
            ensureListsOnHeap().listed.append(callee);
        }
        void add(BuiltinID builtin)
        {
            if (builtins().contains(builtin))
                return;
            if (!liesOnHeap && numberOfInlineBuiltins < inlineBuiltins.size()) {
                inlineBuiltins[numberOfInlineBuiltins++] = builtin;
                return;
            }
            ensureListsOnHeap().builtins.append(builtin);
        }
        void addShared(uint32_t index)
        {
            if (shared().contains(index))
                return;
            if (!liesOnHeap && numberOfInlineShared < inlineShared.size()) {
                inlineShared[numberOfInlineShared++] = index;
                return;
            }
            ensureListsOnHeap().shared.append(index);
        }

        void removeListedMatching(NOESCAPE const Invocable<bool(FunctionID)> auto& matches)
        {
            if (liesOnHeap) {
                listsOnHeap->listed.removeAllMatching(matches);
                return;
            }
            uint8_t numberKept = 0;
            for (FunctionID callee : listed()) {
                if (!matches(callee))
                    inlineListed[numberKept++] = callee;
            }
            numberOfInlineListed = numberKept;
        }
        void clearBuiltins()
        {
            if (liesOnHeap)
                listsOnHeap->builtins.clear();
            numberOfInlineBuiltins = 0;
        }
        void clearShared()
        {
            if (liesOnHeap)
                listsOnHeap->shared.clear();
            numberOfInlineShared = 0;
        }
        void replaceShared(std::span<const uint32_t> indexes)
        {
            clearShared();
            for (uint32_t index : indexes)
                addShared(index);
        }
        void sortListedAndBuiltins()
        {
            if (liesOnHeap) {
                std::ranges::sort(listsOnHeap->listed);
                std::ranges::sort(listsOnHeap->builtins);
                return;
            }
            if (numberOfInlineListed > 1)
                std::ranges::sort(std::span(inlineListed).first(numberOfInlineListed));
        }

    private:
        NEVER_INLINE ListsOfCalleesOnHeap& ensureListsOnHeap()
        {
            if (liesOnHeap)
                return *listsOnHeap;
            auto* lists = new ListsOfCalleesOnHeap;
            lists->listed.append(listed().span());
            lists->builtins.append(builtins().span());
            lists->shared.append(shared().span());
            numberOfInlineListed = 0;
            numberOfInlineBuiltins = 0;
            numberOfInlineShared = 0;
            listsOnHeap = lists;
            liesOnHeap = true;
            return *lists;
        }
    };
    static_assert(sizeof(CalleesOfCall) == 16);
    Vector<uint32_t> firstCallOfFunction;
    LargeList<CalleesOfCall> calleesOfCalls;
    Vector<CalleesOfCall> calleesOfSharedCalls;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> callsOfBuiltinsTold;
    struct ViewOfHeldReading {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ViewOfHeldReading);
        ~ViewOfHeldReading();
        BitVector escapedLayouts;
        std::span<CalleesOfCall> calleesOfCalls;
        Vector<CalleesOfCall> calleesOfSharedCalls;
        const BitVector* functionsThatTablesMayName { nullptr };
        std::unique_ptr<WriterOfLinkLayouts, DeleterOfWriterOfLinkLayouts> writerOfLayouts;
        const ParallelLoops* loops { nullptr };
    };
    std::unique_ptr<ViewOfHeldReading> makeViewOfHeldReading();
    class ViewAtWork {
        WTF_MAKE_NONCOPYABLE(ViewAtWork);
    public:
        explicit ViewAtWork(ViewOfHeldReading* view)
            : m_outer(static_cast<ViewOfHeldReading*>(std::exchange(t_whatHelpersInherit, view)))
        {
        }
        ~ViewAtWork() { t_whatHelpersInherit = m_outer; }
        static ViewOfHeldReading* current() { return static_cast<ViewOfHeldReading*>(t_whatHelpersInherit); }
    private:
        ViewOfHeldReading* m_outer;
    };
    const ParallelLoops& loopsOfReading() const { return ViewAtWork::current() ? *ViewAtWork::current()->loops : loops; }
    WhatUnseenCodeDoes reading() const { return ViewAtWork::current() ? WhatUnseenCodeDoes::OnlyReadsAndCalls : WhatUnseenCodeDoes::Anything; }
    const BitVector& escapedLayoutsAsRead() const { return ViewAtWork::current() ? ViewAtWork::current()->escapedLayouts : escapedLayouts; }
    std::span<const CalleesOfCall> calleesOfCallsAsSettled() const { return ViewAtWork::current() ? std::span<const CalleesOfCall>(ViewAtWork::current()->calleesOfCalls) : calleesOfCalls.span(); }
    const Vector<CalleesOfCall>& calleesOfSharedCallsAsSettled() const { return ViewAtWork::current() ? ViewAtWork::current()->calleesOfSharedCalls : calleesOfSharedCalls; }
    std::unique_ptr<WriterOfLinkLayouts, DeleterOfWriterOfLinkLayouts>& writerOfLayoutsOfReading() const { return ViewAtWork::current() ? ViewAtWork::current()->writerOfLayouts : writerOfLayouts; }
    struct SubjectOfSharedCall {
        FamilyID family;
        PropertyKeyID key;
        uint32_t form { 0 };

        friend constexpr auto operator<=>(const SubjectOfSharedCall&, const SubjectOfSharedCall&) = default;
    };
    Vector<SubjectOfSharedCall> subjectsOfSharedCalls;
    FlowNodeID makeSharedNode() { return makeNode(); }
    void settleSharedCalls() { settleSharedCallsAs(WhatUnseenCodeDoes::Anything); }
    void settleSharedCallsAs(WhatUnseenCodeDoes);
    void enter(FunctionID caller, uint32_t callInSummary, FunctionID callee);

    Vector<OptionSet<LayoutChange>> possibleChanges;
    Vector<AccessorLaidInField> accessorsLaidInFields;
    BitVector layoutsWithKnownAccessors;
    BitVector layoutsDefinedOnUnderWrittenKeys;
    BitVector functionsThatReadPrototypeObjects;
    Vector<FieldReference> fieldsWithKnownAccessors;
    BitVector fieldsWrittenAfterConstruction;
    BitVector layoutsThatUnseenCodeMayChange;
    BitVector modulesWhoseValueStaysInSight;
    BitVector layoutsThatGainPropertiesThroughLost;
    BitVector layoutsThatGainWhatUnseenCodeGaveSources;
    BitVector fieldsThatUnseenCodeMayWrite;
    OptionSet<LayoutChange> possibleChangesOf(size_t layout) const
    {
        OptionSet<LayoutChange> changes = layout < possibleChanges.size() ? possibleChanges[layout] : OptionSet<LayoutChange> { };
        if (reading() != WhatUnseenCodeDoes::Anything)
            return changes;
        if (layoutsThatUnseenCodeMayChange.get(layout))
            changes.add({ LayoutChange::IsFrozen, LayoutChange::GainsProperty, LayoutChange::LosesProperty, LayoutChange::ChangesPrototype, LayoutChange::GetsAccessor });
        if (layoutsThatGainPropertiesThroughLost.get(layout) || layoutsThatGainWhatUnseenCodeGaveSources.get(layout))
            changes.add(LayoutChange::GainsProperty);
        return changes;
    }
    bool isWrittenAfterConstruction(size_t cell) const { return fieldsWrittenAfterConstruction.get(cell) || (reading() == WhatUnseenCodeDoes::Anything && fieldsThatUnseenCodeMayWrite.get(cell)); }

    BitVector fieldsThatMayBeEmpty;
    BitVector fieldsNotOfBirth;
    BitVector gettersThatMayBeAbsent;
    BitVector settersThatMayBeAbsent;
    void noteBirthsOf(FunctionID);
    void noteConstructionsOf(FunctionID);
    FlowNodeID foundWhereFieldIsEmpty(FieldReference);
    bool mayReadInFrontOfDefinition(FunctionID reader, const SummaryAccess&, LayoutID) const;
    bool superCallMayAnswerWithOlderObject(LayoutID id) const { return m_layoutsWhoseSuperCallMayAnswerWithOlderObject.get(id.index()); }
    bool constructionMayAnswerWithOlderObject(LayoutID id) const { return m_layoutsWhoseConstructionMayAnswerWithOlderObject.get(id.index()); }
    bool everyMemberStandsAtOrBelow(FamilyID family, LayoutID above) const { return m_stretchesAtAndBelowLayouts[above.index()].holds(m_stretchesOfFamilies[family.index()]); }
    LayoutID topOfRow(LayoutID id) const { return m_topsOfRows[id.index()]; }
    bool standsAtOrBelow(LayoutID id, LayoutID above) const
    {
        uint32_t place = m_stretchesAtAndBelowLayouts[id.index()].first;
        return m_stretchesAtAndBelowLayouts[above.index()].holds({ place, place + 1 });
    }
    bool mayStoreIntoOlderObjectThatSuperCallAnswered(FunctionID writer, const SummaryAccess&) const;
    void forEachNodeAnsweredBesideObjectOfConstruction(FunctionID constructor, NOESCAPE const ScopedLambda<void(uint32_t nodeInSummary)>&) const;
    bool isPartOfConstructionOf(LayoutID, FunctionID writer, const SummaryAccess&) const;
    bool isPartOfConstructionOfAllOf(FamilyID, FunctionID writer, const SummaryAccess&) const;

private:
    struct Edge {
        FlowNodeID to;
        TypeID passes;
        uint32_t next { UINT32_MAX };
    };

    struct Node {
        LinkType type;
        TypeID typeOfPosition;
    };
    static_assert(sizeof(Node) == 36);
    struct ListsOfNode {
        uint32_t firstEdge { UINT32_MAX };
        uint32_t firstObserver { UINT32_MAX };
    };
    LargeList<Node> m_nodes;
    using SetOfNumbers = HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    bool hasPreparation(PropertyKeyID symbol) const { return m_everySymbolOfLibraryMayHavePreparation || m_symbolsWithPreparation.contains(symbol.bits()); }
    struct SymbolsWithPreparationInSight {
        Vector<PropertyKeyID> notedBesideOtherKeys;
        Vector<PropertyKeyID> notedWithPreparation;
        bool unknownSymbolWasNoted { false };
        SetOfNumbers withPreparation;
        bool everySymbolOfLibraryMayHaveOne { false };
        bool areNames { true };
        SetOfNumbers besideOtherKeys;
        bool hasPreparation(PropertyKeyID symbol) const { return everySymbolOfLibraryMayHaveOne || withPreparation.contains(symbol.bits()); }
    };
    SymbolsWithPreparationInSight m_symbolsInSight;
    bool takeInWhatWasNotedOfSymbols();
    Vector<PropertyKeyID> m_symbolsNotedBesideOtherKeys;
    Vector<PropertyKeyID> m_symbolsNotedWithPreparation;
    bool m_unknownSymbolWasNotedWithPreparation { false };
    void tellAccessesUnderSymbolsWithPreparation();
    Shared<SetOfNumbers> m_nodesHandedToPreparation;
    SetOfNumbers m_symbolsWithPreparation;
    bool m_everySymbolOfLibraryMayHavePreparation { false };
    bool m_symbolsWithPreparationAreNames { true };
    SetOfNumbers m_symbolsOfLibraryBesideOtherKeys;
    Vector<std::pair<PropertyKeyID, FlowObserver>> m_accessesUnderSymbolsOfLibrary;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_accessesUnderSymbolsOfLibraryListed;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_symbolsAtNodes;
    LargeList<ListsOfNode> m_listsOfNodes;
    std::optional<SizeOfLinkFlow> m_sizeWhenWaysWereForgotten;
    BitVector m_flowsIntoUnseen;
    BitVector m_accessorsWaitAt;
    BitVector m_hasEdgesInSet;
    struct ArrivalHeldBack {
        FlowNodeID node;
        PropertyKeyID symbol;
        LinkType type;
    };
    struct EdgeLaid {
        FlowNodeID from;
        FlowNodeID to;
        TypeID passes;
    };
    struct ObserverPut {
        FlowNodeID node;
        FlowObserver observer;
    };
    struct Lane;
    Vector<std::unique_ptr<Lane>> m_lanes;
    unsigned m_log2OfNodesInBlock { 12 };
    unsigned m_maskOfLanes { 0 };
    unsigned laneIndexOf(FlowNodeID node) const { return (node.index() >> m_log2OfNodesInBlock) & m_maskOfLanes; }
    Lane& laneOf(FlowNodeID node) { return *m_lanes[laneIndexOf(node)]; }
    static constinit thread_local Lane* s_laneAtWork;
    bool m_flowIsAtRest { false };
    bool m_typesAreSettled { false };
    BitVector m_familiesWhoseConstructorsHaveEscaped;
    BitVector m_sitesThatHaveEscaped;
    bool m_holdsBack { false };
    bool m_tellsObservers { false };
    bool m_severalThreadsTell { false };
    bool m_lanesTakeStepsOfReach { false };
    alignas(128) mutable std::atomic<bool> m_whatIsSharedIsLocked { false };
    void lockWhatIsShared() const
    {
        static constexpr unsigned pausesBeforeGivingWay = 4096;
        unsigned pauses = 0;
        while (m_whatIsSharedIsLocked.load(std::memory_order_relaxed) || m_whatIsSharedIsLocked.exchange(true, std::memory_order_acquire)) {
            if (++pauses < pausesBeforeGivingWay)
                simde_mm_pause();
            else
                Thread::yield();
        }
    }
    static constinit thread_local unsigned s_depthOfLockOfWhatIsShared;
    struct alignas(128) PartOfWhatSomeLaneWasGiven {
        std::atomic<bool> isLocked { false };
        HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> given;
    };
    static constexpr size_t numberOfPartsOfWhatSomeLaneWasGiven = 16;
    mutable std::array<std::array<PartOfWhatSomeLaneWasGiven, numberOfPartsOfWhatSomeLaneWasGiven>, numberOfKindsMadeOnDemand> m_givenToSomeLane;
    PartOfWhatSomeLaneWasGiven& lockPartOfWhatSomeLaneWasGiven(MadeOnDemand, uint64_t words) const;
    static const LinkType s_nothing;
    std::atomic<size_t> m_numberOfNodesMade { 0 };
    FlowNodeID contentsFoundOrMadeByLane(SiteID, SlotOfContents);
    void makeContentsInLane(uint32_t family, SlotOfContents, FlowNodeID);
    void listContentsThatLanesMade();
    void listContentsThatLanesMadeIn(unsigned part);
    static bool isToldByOneThread(FlowObserver::Kind);
    void tellOnLanes();
    void prepareAccessesForSeveralThreads();
    void prepareOperatorsForSeveralThreads();
    void sortTellsOfLane(unsigned lane);
    void tellInLane(unsigned lane);
    void takeOverWhatLanesDidWhileTelling();
    void takeOverNewObserversOfLanes();
    void noteFunctionToReach(FunctionID);
    void reachAll(std::span<const FunctionID>);
public:
    void noteLayoutToTie(LayoutID layout) { m_layoutsToTie.append(layout); }
private:
    void holdBackWhile(const ScopedLambda<void()>&);
    void takeNote(Note, uint32_t number, uint32_t more);
    void takeNotesOfPassOnOneThread();
    bool resolveCallsThatWereToldIn(unsigned lane);
    bool m_setsMeetingsAside { false };
    Lane* m_firstLane { nullptr };
    ALWAYS_INLINE Lane& laneThatHoldsBack() const
    {
        Lane* lane = s_laneAtWork;
        return lane ? *lane : *m_firstLane;
    }
    BitVector m_isToBeTold;
    uint64_t m_numberOfRounds { 0 };
    void flow();
    void goAlongEdges();
    void sendFromLane(unsigned lane);
    void receiveInLane(unsigned lane);
    void foldObjects(LinkType& into, std::span<const LinkType> met) const;
    void foldObjectsTakenApart(LinkType& into, std::span<const LinkType> met) const;
    void foldFunctions(LinkType& into, std::span<const LinkType> met) const;
    bool severalThreadsPayFor(size_t numberOfSteps) const;
    void wakeWhoWaitsForWhatWasHeldBack();
    void takeInWhatWasHeldBack();
    void layWhatWasHeldBackFor(unsigned lane);
    void gatherWhatLanesNoted();
    void runOnLanes(const Invocable<void(unsigned lane)> auto&);
    bool lanesMayWorkForReach(size_t numberOfSteps) const;
    void workOnLanesForReach(const Invocable<void(unsigned lane)> auto&);
    bool layEdge(Lane&, FlowNodeID from, FlowNodeID to, TypeID passes);
    void addEdgeAtOnce(FlowNodeID from, FlowNodeID to, TypeID passes);
    void putObserver(Lane&, FlowNodeID, const FlowObserver&);
    bool isObservedAlready(FlowNodeID, const FlowObserver&);
    void send(Lane& sender, FlowNodeID to, const LinkType&, PropertyKeyID symbol);
    void receive(FlowNodeID, const LinkType&, PropertyKeyID symbol);
    void arriveAtOnce(FlowNodeID, const LinkType&, PropertyKeyID symbol);
    void wakeAccessorsThatWaitAt(Lane&, FlowNodeID);
    void putObserverThatWaitsUntilNodeIsAsked(Lane&, FlowNodeID, const FlowObserver&);
    bool isAskedForTheFirstTime(Lane&, FlowNodeID);
    bool hasListsToItself(FlowNodeID node) const { return node.index() < m_listsOfNodes.size() && (!m_severalThreadsTell || s_laneAtWork == m_lanes[laneIndexOf(node)].get()); }
    bool wouldNameSomethingNewAtUnseen(const LinkType&) const;
    void noteLossOfBuiltin(BuiltinID) const;
    void noteMetInFamily(LayoutID) const;
    bool hasEscapedAsFamily(FamilyID) const;
    void catchUpWithFamiliesAbove(LayoutID) const;
    Vector<FamilyID> m_outermostFamilies;
    mutable BitVector m_outermostFamiliesWhereSomethingBefell;
    PropertyKeyID symbolNotedAt(FlowNodeID) const;
    std::pair<FlowNodeID, LinkType> takeNode(Lane&);
    std::pair<FlowObserver, FlowNodeID> takeNewObserver();
    uint64_t nextScatteredNumber();
    mutable Lock m_lockOfSymbolsAtNodes;
    const OrderOfWorklist m_order;
    size_t m_firstOfNewObservers { 0 };
    uint64_t m_scatteredNumber { 88172645463325252ull };
    Vector<BreachOfClosure>* m_breachesOfClosure { nullptr };
    FlowObserver m_observerToldOnceMore;
    FlowNodeID m_nodeGoneOver;
    void noteBreachOfClosure(BreachOfClosure&&);
    BitVector m_familiesThatHaveEscaped;
    BitVector m_lostFamiliesThatHaveEscaped;
    Vector<LayoutSetID> m_layoutsThatMet;
    UncheckedKeyHashMap<uint32_t, LayoutSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_layoutsThatMetByRepresentative;
    BitVector m_isOnWorklist;
    uint64_t m_numberOfObserversTold { 0 };
    struct NodesOfFunction {
        uint32_t firstParameter;
        uint32_t firstResult;
        uint32_t thrown;
        uint32_t base;
    };
    Vector<NodesOfFunction> m_nodesOfFunctions;
    uint32_t m_firstNodeOfFunctions { 0 };
    Vector<uint32_t> m_nodesOfSummaries;
    Vector<NameClass> m_nameClassesOfKeys;

    StateOfLinkReach& stateOfLinkReach();
    bool fillParametersThatUnseenCodeAlonePasses();
    std::unique_ptr<StateOfLinkReach, DeleterOfStateOfLinkReach> m_stateOfLinkReach;

    StateOfLinkCalls& stateOfLinkCalls();
    std::unique_ptr<StateOfLinkCalls, DeleterOfStateOfLinkCalls> m_stateOfLinkCalls;

    StateOfLinkAccesses& stateOfLinkAccesses();
    std::unique_ptr<StateOfLinkAccesses, DeleterOfStateOfLinkAccesses> m_stateOfLinkAccesses;

    std::unique_ptr<StateOfLinkOperators, DeleterOfStateOfLinkOperators> m_stateOfLinkOperators;

    std::unique_ptr<TestRunsInLink> m_testRuns;

    LinkType m_typeObservedBefore;
    Vector<uint32_t> m_firstFieldOfLayout;

    void noteFieldMayBeEmpty(FieldReference);
    Vector<uint32_t> m_nodesFoundWhereFieldsAreEmpty;

    void indexConstructorFunctions();
    uint32_t recordOfNameOnPrototype(LayoutID, PropertyKeyID);
    uint32_t firstNameOnPrototypeOf(LayoutID) const;
    void forEachPrototypePassedAbove(LayoutID, PropertyKeyID, const Invocable<void(uint32_t record)> auto&);
    void tiePrototypesAboveLostLayout(LayoutID);
    bool letPrototypesFoundThroughLostEscape();
    class CopiesOfMembers;
    bool layMembersThatCopiesBring();
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_edgesLaidByCopiesOfMembers;
    BitVector m_layoutsOfConstructorFunctions;
    BitVector m_layoutsBelowConstructorFunctions;
    BitVector m_prototypesPassedByLookups;
    BitVector m_prototypesWithDefinitions;
    BitVector m_prototypesAboveLostLayouts;
    Vector<NameOnPrototype> m_namesOnPrototypes;
    struct NamesOfPrototype {
        uint32_t first { UINT32_MAX };
        uint32_t last { UINT32_MAX };
    };
    HashMap<uint32_t, NamesOfPrototype, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_namesOfPrototypes;
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_recordsOfNamesOnPrototypes;
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_nodesFoundOnPrototypesAbove;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_nodesOfObjectsMadeForClassesBelow;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_nodesOfObjectsMadeThroughConstructors;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_nodesOfMembersWritten;

    enum class NodeOnDemand : uint8_t { ReadThroughFamily, StoredThroughFamily, StoredInConstructionThroughFamily, AnyFieldReadOfLayout, AnyFieldStoredOfLayout, AnyFieldReadOfFamily, AnyFieldStoredOfFamily, AnyFieldReadOfSet, ElementReadOfSet, AnyFieldStoredOfSet, ElementStoredOfSet, FoundBesideFieldsUnderKey, LaidOnClosuresUnderKey, AnyFieldDefinedOnLayout, WhatStandsAbove, WhatStandsBelow };
    enum class AggregateAccess : uint8_t { Read, Store };

    struct MemberOfClass {
        LayoutID classLayout;
        ClassSide side { ClassSide::Prototype };
        PropertyKeyID key;
        MethodKind kind { MethodKind::Method };
        uint32_t index { 0 };
        FunctionID function;
    };

    struct Deferred {
        enum class Kind : uint8_t { LayoutIsLost, FamilyIsLost, LayoutsAreUnited, LostFamilyEscapes, FunctionIsLost, ConstructorsAreLost, SitesAreUnited, LayoutEscapes, FunctionEscapes, ConstructorsEscape, PrototypesEscape, SiteEscapes, TypeEscapes };

        Kind kind { Kind::TypeEscapes };
        uint32_t number { 0 };
        LinkType type;
        uint32_t other { 0 };
    };

    struct LostKey {
        PropertyKeyID key;
        uint32_t read { UINT32_MAX };
        uint32_t stored { UINT32_MAX };
        uint32_t firstHolder { UINT32_MAX };
        uint32_t lastHolder { UINT32_MAX };
        uint32_t family { 0 };
        uint32_t next { UINT32_MAX };
        bool tiesEveryMember { false };
        bool isListedAsTyingEveryMember { false };
        bool hasBeenMerged { false };
    };

    struct LostFamily {
        uint32_t foundBesideFields { UINT32_MAX };
        uint32_t storedBesideFields { UINT32_MAX };
        uint32_t anyFieldRead { UINT32_MAX };
        uint32_t anyFieldReadUnderMarksOfRule { UINT32_MAX };
        uint32_t firstKey { UINT32_MAX };
        Vector<uint32_t> keysThatTieEveryMember;
        bool reachesUnseen { false };
        std::set<uint32_t> placesInForestOfFamilies;
    };

    struct HolderOfLostKey {
        LayoutID layout;
        uint32_t next { UINT32_MAX };
    };

    Vector<uint32_t> indexMembersOfClasses();
    void numberNodes(Vector<uint32_t>&& numbersOfMemberNodesOfSides);
    void makeTablesThatNeedNoNodes();
    FlowNodeID findNodeOf(FunctionID, uint32_t nodeInSummary) const;
    FlowNodeID makeNode();
    std::span<const MemberOfClass> membersOf(LayoutID, ClassSide) const LIFETIME_BOUND;
    std::span<const MemberOfClass> membersFoundUnder(LayoutID, ClassSide, PropertyKeyID) const LIFETIME_BOUND;
    bool lookupGoesOnAboveClasses(LayoutID from, std::span<const MemberOfClass> members) const { return members.empty() || mayNotBeLinkedYetBelow(from, members[0].classLayout); }

    LinkType joined(const LinkType&, const LinkType&) const;
    bool loseInSight(BuiltinID canonical) const;
    Vector<BuiltinID>& builtinsLostInSightByThread() const;
    bool isNodeOfLibrary(size_t index) const { return index < m_isNodeOfLibrary.size() && m_isNodeOfLibrary.quickGet(index); }
    LinkType passedByTestOfEdge(const LinkType&, TypeID test);
    void joinObjects(LinkType& into, const LinkType&) const;
    void joinFunctions(LinkType& into, const LinkType&) const;
    void joinClasses(LinkType& into, const LinkType&) const;
    bool standsOutsideFamilies(const LinkType&) const;
    bool namesOnlyLayoutsOfSet(const LinkType&, const LinkType& set) const;
    void deferLoss(LayoutID) const;
    void deferUnion(LayoutID, LayoutID) const;
    LayoutID memberStandingFor(const LinkType&) const;
    mutable SetsOfLayoutsInLinkFlow m_setsOfLayouts;
    bool mayStandInOneSet(std::span<const LayoutID> sortedMembers) const;
    mutable SetsOfFunctionsInLinkFlow m_setsOfFunctions;
    bool namesOnlyFunctionsOfSet(const LinkType&, const LinkType& set) const;
    void foldFunctionsTakenApart(LinkType& into, std::span<const LinkType> met) const;
    void uniteLost(LayoutID, LayoutID);
    void loseFamiliesOfMembersThatMeet(uint32_t kept, uint32_t absorbed);
    struct TiesToLay {
        enum class Members : uint8_t { HoldersOfLostKey, LostFamily, DeclaredFamily, Listed };
        enum class Access : uint8_t { Read, Store, StoreInConstruction, ReadOfAnyField, ReadOfAnyFieldUnderNumbers };
        Members members;
        Access access;
        FlowNodeID aggregate;
        uint32_t number;
        PropertyKeyID key;
        uint32_t end { 0 };
        UnknownByEscapeAlone::Marks marks { UnknownByEscapeAlone::Marks::Nothing };
        bool codeAtWorkIsOfLibrary { false };
    };
    bool leavesTiesToLanes(TiesToLay);
    void layTiesThatLanesLeft();
    bool settlingLeavesTies() const { return m_isSettling && m_holdsBack && !m_severalThreadsTell && !s_laneAtWork && !m_breachesOfClosure; }
    void layTiesThatSettlingLeft();
    Vector<TiesToLay> m_tiesLeftBySettling;
    Vector<LayoutID> m_membersOfTiesLeftBySettling;
    void layTie(const TiesToLay&, LayoutID member);
    uint32_t placeInForestOfFamilies(LayoutID);
    Vector<uint32_t> m_placesInForestOfFamilies;
    Vector<LayoutID> m_layoutsByPlaceInForestOfFamilies;
    void mergeLostFamilies(uint32_t kept, uint32_t absorbed);
    Vector<LayoutID> membersOfLostFamily(uint32_t representative) const;
    void deferLoss(FamilyID) const;
    void deferLoss(FunctionID) const;
    void deferUnion(SiteID, SiteID) const;
    void deferLossOfConstructors(FamilyID) const;
    void loseFamilyThatHoldsLostConstructorsWith(FamilyID, LayoutID onlyMemberLost);
    struct LostConstructorsOfTree {
        FamilyID nearestFamily;
        LayoutID onlyMember;
    };
    HashMap<uint32_t, LostConstructorsOfTree, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_lostConstructorsOfTrees;
    bool deferEscape(LayoutID) const;
    bool deferEscape(FunctionID) const;
    void deferEscape(SiteID) const;
    void noteReachesUnseen(const LinkType&) const;
    void settle()
    {
        if (!m_isSettling && !m_tellsObservers && !m_deferred.isEmpty())
            settleWhatWasDeferred();
    }
    void settleWhatWasDeferred();
    void tell(const FlowObserver&, const LinkType& now, const LinkType& before);

    bool tieInWhatGrew();
    bool tieInLostLayouts();
    void tieInLostLayout(LayoutID);
    bool tieInLostFunctions();
    void tieInLostFunction(FunctionID);
    void open(FlowNodeID contents);
    FlowNodeID findContents(uint32_t family, SlotOfContents) const;
    void addContents(uint32_t family, SlotOfContents, FlowNodeID);
    void listContents(uint32_t site, SlotOfContents, FlowNodeID);
    bool openLayoutsThatChanged();
    bool letMembersOfLostEscape();
    bool tieAppendedFieldsOfLostLayouts();
    void noteLostLayoutsGainingProperties();

    static uint64_t wordOfNodeOnDemand(NodeOnDemand, uint32_t number, PropertyKeyID);
    std::pair<FlowNodeID, bool> nodeOnDemand(NodeOnDemand, uint32_t number, PropertyKeyID);
    uint64_t wordOfAnyFieldThrough(LinkType::Objects, LayoutID, FamilyID, bool keyIsNumber) const;
    FlowNodeID findOrMakeAnyFieldReadThrough(LinkType::Objects, LayoutID, FamilyID, bool keyIsNumber);
    FlowNodeID findOrMakeAnyFieldStoredThrough(LinkType::Objects, LayoutID, FamilyID, bool keyIsNumber);
    Vector<NameOnClosures> m_namesOnClosures;
    Vector<uint32_t> m_newestNameOnClosuresOf;
    void makeNodesBesideFieldsOf(LayoutID);
    void open(LayoutID);
    LinkType whatLiesAboveClasses(LayoutID, PropertyKeyID) const;
    bool tieRead(FlowNodeID aggregate, LayoutID, PropertyKeyID);
    bool tieStore(FlowNodeID aggregate, LayoutID, PropertyKeyID, bool isPartOfConstruction);
    void tieReadOfAnyField(FlowNodeID aggregate, LayoutID, bool keyIsNumber = false);
    void tieStoreOfAnyField(FlowNodeID aggregate, LayoutID, bool keyIsNumber = false);
    void tieAccessor(FlowNodeID aggregate, LayoutID receiver, const MemberOfClass&, AggregateAccess);
    void tieAccessorThatNobodyKnows(FlowNodeID aggregate, LayoutID receiver, FlowNodeID holder, AggregateAccess);
    void tieToUnseen(FlowNodeID aggregate, LayoutID, AggregateAccess);
    uint32_t recordOfLostKey(uint32_t representative, PropertyKeyID);
    FlowNodeID fieldThroughLost(LayoutID member, PropertyKeyID, AggregateAccess);
    FlowNodeID argumentOfLostFunctions(uint32_t positionalArgument);
    FlowNodeID gatheredByLostFunctions();
    struct ArgumentsGathered {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ArgumentsGathered);
        struct Taker {
            FlowNodeID node;
            uint32_t place { 0 };
        };
        struct GathererBehind {
            FunctionID function;
            uint32_t firstPlace { 0 };
        };
        uint32_t fewestInSight { UINT32_MAX };
        uint32_t fewestByEscapeAlone { UINT32_MAX };
        FlowNodeID atAnyPlace;
        Vector<FlowNodeID> atPlaces;
        Vector<Taker> takers;
        Vector<GathererBehind> gatherersBehind;
    };
    ArgumentsGathered& argumentsGatheredBy(FunctionID);
    Shared<HashMap<uint32_t, std::unique_ptr<ArgumentsGathered>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>> m_argumentsGathered;
    Shared<HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>> m_placesOfTakersOfGatheredArguments;

    Vector<std::pair<FlowObserver, FlowNodeID>> m_newObservers;
    size_t m_numberOfNewObserversLeftWithLanes { 0 };
    size_t numberOfNewObservers() const { return m_newObservers.size() + m_numberOfNewObserversLeftWithLanes; }
    mutable Vector<Deferred> m_deferred;
    bool m_isSettling { false };
    bool m_letsReachWait { false };
    void bringAliveWhatSettlingReached();

    uint32_t m_firstVariable { 0 };
    Vector<uint32_t> m_nodeOfField;
    Vector<uint32_t> m_firstDeclaredFieldOfLayout;
    uint32_t m_firstNodeOfFields { 0 };
    BitVector m_fieldsWhoseSlotsHaveDoors;
    Vector<uint32_t> m_firstMemberNodeOfSide;
    Vector<uint32_t> m_firstMemberOfSide;
    Vector<MemberOfClass> m_membersOfClasses;
    std::array<BitVector, 2> m_keysOfMembers;
    BitVector m_sidesWithComputedKeys;
    BitVector m_fieldsWithSettersWritten;
    static constexpr uint32_t siteAmongNodesOnDemand = 1u << 27;
    static constexpr uint32_t builtinAmongNodesOnDemand = 1u << 27;
    static constexpr uint32_t functionAmongNodesOnDemand = 1u << 26;
    static constexpr uint32_t underMarksOfRuleAmongNodesOnDemand = 1u << 27;
    BitVector m_sidesBelowIncompleteNames;
    void findSidesBelowIncompleteNames();
    BitVector m_layoutsBelowMembersUnderNumbers;
    void findLayoutsBelowMembersUnderNumbers();
    BitVector m_layoutsWithFieldsUnderComputedKeys;
    struct KeysThatRowOfConstructorsFills {
        uint32_t untilFirstSight { 0 };
        uint32_t untilEnds { 0 };
        uint32_t leftEmptyUntilFirstSight { UINT32_MAX };
        uint32_t leftEmptyUntilEnds { UINT32_MAX };
    };
    Vector<KeysThatRowOfConstructorsFills> m_keysThatRowsOfConstructorsFill;
    Vector<Vector<PropertyKeyID>> m_listsOfKeysThatRowsOfConstructorsFill;
    Vector<Vector<uint32_t>> m_listsOfFieldsThatRowsOfConstructorsLeaveEmpty;
    void forEachFieldLeftEmptyByRowOfConstructors(LayoutID, uint32_t placeOfKeysFilled, uint32_t placeOfFieldsLeftEmpty, const Invocable<void(uint32_t)> auto&) const;
    void findWhatRowsOfConstructorsFill();
    void findSuperCallsThatMayAnswerWithOlderObject();
    bool answersOnlyWithObjectOfItsConstruction(FunctionID constructor) const;
    void numberTreeOfBases();
    struct StretchInTreeOfBases {
        bool holds(const StretchInTreeOfBases& other) const { return first <= other.first && other.end <= end; }
        uint32_t first { UINT32_MAX };
        uint32_t end { 0 };
    };
    BitVector m_layoutsWhoseSuperCallMayAnswerWithOlderObject;
    BitVector m_layoutsWhoseConstructionMayAnswerWithOlderObject;
    Vector<uint32_t> m_numbersOfAllocatedMembersOfFamilies;
    Vector<StretchInTreeOfBases> m_stretchesAtAndBelowLayouts;
    Vector<StretchInTreeOfBases> m_stretchesOfFamilies;
    Vector<LayoutID> m_topsOfRows;

    mutable BitVector m_layoutsToldLost;
    mutable BitVector m_familiesToldLost;
    mutable BitVector m_functionsToldLost;
    mutable BitVector m_layoutsToldToEscape;
    mutable BitVector m_functionsToldToEscape;
    mutable BitVector m_layoutsAtUnseen;
    mutable BitVector m_prototypesAtUnseen;
    mutable BitVector m_familiesAtUnseen;
    mutable BitVector m_functionsAtUnseen;
    mutable BitVector m_constructorsToldLost;
    mutable BitVector m_constructorsAtUnseen;
    mutable BitVector m_prototypesOfFamiliesAtUnseen;
    mutable bool m_lostFunctionsReachUnseen { false };
    mutable bool m_objectsReachUnseen { false };
    bool m_rowPassesWhatNobodyFollows { false };
    Vector<PropertyKeyID, 2> m_keysThatCodeTellsOfItself;
    bool m_promisesMayBeHandedOut { false };
    mutable Vector<BuiltinID> m_builtinsLost;
    mutable Vector<BuiltinID> m_builtinsLostInSight;
    struct SiteOfWrite {
        FunctionID writer;
        ValueID site;
        FlowNodeID subject;
        FlowNodeID source { };
    };
    Shared<Vector<SiteOfWrite>> m_sitesOfWritesOnBuiltins;
    mutable Vector<BuiltinID> m_builtinsAtUnseen;
    Vector<FlowNodeID> m_receiversThatWaitForAccessorsLaidAbove;
    bool m_operandIsLaidAbovePrototype { false };
    BitVector m_isNodeOfLibrary;
    BitVector m_isFunctionOfLibrary;
    bool m_tellsObserverOfLibrary { false };
    BitVector m_isFunctionThatRowsSpeakFor;
    mutable bool m_lostBuiltinsReachUnseen { false };

    struct TestOfEdge {
        enum class Passes : uint8_t { Everything, PrimitivesAlone, EveryObject, SomeObjects };

        Passes passes { Passes::SomeObjects };
        uint16_t primitives { 0 };
    };
    HashMap<uint32_t, TestOfEdge, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_testsOfEdges;

    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_nodesOnDemand;
    Vector<uint32_t> m_nodesBesideFields;
    Vector<Vector<OwnValue>> m_ownValuesOfLayouts;
    BitVector m_layoutsWhoseOwnValuesAreListed;
    Vector<OwnValue, 8> listOwnValuesOf(LayoutID);
    Vector<LayoutID> m_layoutsWithNodesBesideFields;
    BitVector m_openLayouts;
    struct AppendedField {
        PropertyKeyID key;
        FlowNodeID node;
        uint32_t next { UINT32_MAX };
    };
    Vector<AppendedField> m_appendedFields;
    struct ReadThroughFamily {
        PropertyKeyID key;
        FlowNodeID aggregate;
    };
    struct SortedMembersOfFamily {
        Vector<LayoutID> tiedOneByOne;
        HashMap<uint32_t, Vector<LayoutID, 1>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> membersWithNothingAboveByKey;
        uint32_t numberWithNothingAbove { 0 };
        FlowNodeID foundBesideTheirFields;
        uint32_t numberLost { 0 };
        Vector<ReadThroughFamily> readsWithoutLostMember;
    };
    bool hasNothingAbove(LayoutID);
    BitVector m_layoutsWithNothingAbove;
    bool m_hasListedLayoutsWithNothingAbove { false };
    HashMap<uint64_t, Vector<FlowNodeID, 1>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_appendedFieldsThatWaitForRead;
    SortedMembersOfFamily& sortedMembersOf(FamilyID);
    bool noteLostMemberWithNothingAbove(FamilyID, LayoutID);
    void tieReadThroughSortedMembers(FlowNodeID aggregate, FamilyID, PropertyKeyID);
    FlowNodeID findNodeOnDemand(NodeOnDemand, uint32_t number, PropertyKeyID) const;
    HashMap<uint32_t, std::unique_ptr<SortedMembersOfFamily>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_sortedMembersOfFamilies;
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_placesOfAppendedFields;
    Vector<uint32_t> m_newestAppendedFieldOfLayout;
    BitVector m_lostLayoutsWhoseAppendedFieldsAreTied;
    struct Site {
        BuiltinClassID builtinClass;
        BuiltinLayoutID layoutOfRecords;
        bool hasEscaped { false };
        bool isToldToEscape { false };
        bool isClimbedFrom { false };
        bool mayHoldCellsOfCompiledLayouts { false };
        uint32_t newestContents { UINT32_MAX };
        uint32_t numberOfContents { 0 };
    };
    struct ContentsOfSite {
        SlotOfContents slot;
        FlowNodeID node;
        uint32_t next { UINT32_MAX };
    };
    // SITES ARE MADE WHILE SEVERAL THREADS TELL OBSERVERS, WITHOUT THE LOCK OF WHAT IS SHARED, and are read without any. So a site never moves: the blocks double, and there are as many as a number has bits.
    class ListOfSites {
        WTF_MAKE_NONCOPYABLE(ListOfSites);
    public:
        ListOfSites() = default;
        ~ListOfSites()
        {
            for (auto& block : m_blocks)
                delete[] block.load(std::memory_order_relaxed);
        }
        size_t size() const { return m_size.load(std::memory_order_relaxed); }
        Site& operator[](size_t index)
        {
            size_t position = index + (static_cast<size_t>(1) << log2OfFirstBlock);
            unsigned block = static_cast<unsigned>(std::bit_width(position)) - 1 - log2OfFirstBlock;
            return m_blocks[block].load(std::memory_order_acquire)[position - (static_cast<size_t>(1) << (block + log2OfFirstBlock))];
        }
        uint32_t append(const Site& site)
        {
            size_t index = m_size.fetch_add(1, std::memory_order_relaxed);
            RELEASE_ASSERT(index < UINT32_MAX);
            ensureBlockOf(index);
            (*this)[index] = site;
            return static_cast<uint32_t>(index);
        }
        void growTo(size_t size)
        {
            if (size <= this->size())
                return;
            for (size_t index = 0; index < size; index = 2 * index + (static_cast<size_t>(1) << log2OfFirstBlock))
                ensureBlockOf(index);
            m_size.store(size, std::memory_order_relaxed);
        }

    private:
        void ensureBlockOf(size_t index)
        {
            size_t position = index + (static_cast<size_t>(1) << log2OfFirstBlock);
            unsigned block = static_cast<unsigned>(std::bit_width(position)) - 1 - log2OfFirstBlock;
            if (m_blocks[block].load(std::memory_order_acquire))
                return;
            while (m_blockIsLaidOut.exchange(true, std::memory_order_acquire))
                simde_mm_pause();
            if (!m_blocks[block].load(std::memory_order_relaxed))
                m_blocks[block].store(new Site[static_cast<size_t>(1) << (block + log2OfFirstBlock)], std::memory_order_release);
            m_blockIsLaidOut.store(false, std::memory_order_release);
        }

        static constexpr unsigned log2OfFirstBlock = 10;
        std::array<std::atomic<Site*>, 33 - log2OfFirstBlock> m_blocks { };
        std::atomic<size_t> m_size { 0 };
        std::atomic<bool> m_blockIsLaidOut { false };
    };
    mutable ListOfSites m_sites;
    UnitedFamilies m_familiesOfSites;
    struct PartOfContentsOfSites {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(PartOfContentsOfSites);
        Vector<ContentsOfSite> listed;
        HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> ofFamiliesWithManySlots;
    };
    Vector<std::unique_ptr<PartOfContentsOfSites>> m_contentsOfSites;
    unsigned partOfContentsOf(uint32_t site) const { return (site >> 6) & m_maskOfLanes; }
    Vector<ContentsOfSite>& contentsListedOf(uint32_t site) const { return m_contentsOfSites[partOfContentsOf(site)]->listed; }
    struct alignas(128) PartOfSitesOfMakers {
        std::atomic<bool> isLocked { false };
        HashMap<std::pair<uint64_t, uint32_t>, uint32_t, DefaultHash<std::pair<uint64_t, uint32_t>>, WTF::PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>> sites;
    };
    static constexpr unsigned log2OfPartsOfSitesOfMakers = 8;
    std::unique_ptr<std::array<PartOfSitesOfMakers, 1 << log2OfPartsOfSitesOfMakers>> m_sitesOfMakers { makeUniqueWithoutFastMallocCheck<std::array<PartOfSitesOfMakers, 1 << log2OfPartsOfSitesOfMakers>>() };
    Vector<uint32_t> m_firstSiteOfFunction;
    void fillSiteJustMade(SiteID);
    Vector<uint32_t> m_nodesFoundOnClosures;
    bool m_hasStoresThroughLost { false };

    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_recordsOfLostKeys;
    Vector<LostFamily> m_lostFamilies;
    bool m_anyFieldIsReadThroughLost { false };
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_keysLookedUpThroughLost;
    Vector<LostKey> m_lostKeys;
    mutable HashMap<uint32_t, BuiltinID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_functionsOnObjectPrototype;
    Vector<HolderOfLostKey> m_holdersOfLostKeys;
    Vector<LayoutID> m_layoutsToTie;
    bool m_lostLayoutsMayHaveGrown { false };

    Vector<uint32_t> m_argumentsOfLostFunctions;
    uint32_t m_firstArgumentGatheredByLostFunctions { UINT32_MAX };
    uint32_t m_gatheredByLostFunctions { UINT32_MAX };
    uint32_t m_whereInstancesAreMadeThenables { UINT32_MAX };
    uint32_t m_whereInstancesLoseTheirConstructor { UINT32_MAX };
    BitVector m_lostFunctionsTied;
    size_t m_numberOfLostFunctionsTied { 0 };
    bool m_lostFunctionsAreCalled { false };
    BitVector m_lostFunctionsThatCallsName;
    bool m_someCallNamesLostFunctions { false };

    bool mayBeNamedByNumber(PropertyKeyID key) const { return !key || mayBeElementKey(nameClassOf(key)); }
    FlowNodeID foundBesideFieldsUnderAnyNumber();
    std::pair<FlowNodeID, bool> aggregateOfReadsUnderNumbers(Vector<uint32_t>& nodes, size_t numberOfHolders, uint32_t holder);
    void passPrototypesAboveUnderNumbers(LayoutID);
    uint32_t m_foundBesideFieldsUnderAnyNumber { UINT32_MAX };
    Vector<uint32_t> m_elementsReadThroughLayouts;
    Vector<uint32_t> m_elementsReadThroughFamilies;
    Vector<uint32_t> m_elementsReadThroughLostFamilies;
    Vector<uint32_t> m_elementsStoredThroughLayouts;
    Vector<uint32_t> m_elementsStoredThroughFamilies;
    BitVector m_prototypesPassedByNumbers;
};

struct TurnsOfReadings {
    LinkTables held;
    BinarySemaphore provingTablesAreWritten;
    std::unique_ptr<LinkFlow> provingFlow;
};
LinkTables link(std::unique_ptr<LinkFlow>&&, TurnsOfReadings* = nullptr);
void finishReadingsOfLink(Program&, Mechanisms, TurnsOfReadings&, LinkTables& proven, LinkTables& held);
void printBreachesOfClosureOfLink(PrintStream&, Program&, const ThinSummaries&, const Families&, const LooksAtBodies&, Mechanisms, BuildContext&);
void summarizeCallees(const LinkFlow&, LinkTables&);
void writePrototypeObjectsThatAreSettled(const LinkFlow&, LinkTables&);
void layReferrersOfHeldReadingOver(LinkTables& proven, LinkTables& held, const ParallelLoops&);
void rankReferrersFirst(LinkTables&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
