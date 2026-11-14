/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkFlow.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTypes.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTFactRecords.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTLinkStrings.h"
#include "AOTLinkExceptions.h"
#include "AOTNameClass.h"
#include "AOTPrimitives.h"
#include "AOTResultRules.h"
#include <algorithm>
#include <optional>
#include <tuple>
#include <wtf/HashMap.h>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

static LinkType primitivesForLinkAccesses(uint16_t primitives)
{
    LinkType type;
    type.primitives = LinkType::closed(primitives);
    if (!type.hasNumbers())
        type.counts = LinkType::Counts::BelowTwoTo31;
    else if (!(type.primitives & Primitive::Number))
        type.counts = LinkType::Counts::Int32;
    return type;
}

static LinkType instanceForLinkAccesses(LayoutID layout)
{
    LinkType type;
    type.objects = LinkType::Objects::Layout;
    type.layout = layout;
    return type;
}

static LinkType familyForLinkAccesses(FamilyID family)
{
    LinkType type;
    type.objects = LinkType::Objects::Family;
    type.family = family;
    return type;
}

static LinkType prototypeForLinkAccesses(LayoutID layout)
{
    LinkType type;
    type.objects = LinkType::Objects::Prototype;
    type.layout = layout;
    return type;
}

static LinkType prototypesForLinkAccesses(FamilyID family)
{
    LinkType type;
    type.objects = LinkType::Objects::Prototypes;
    type.family = family;
    return type;
}

static LinkType constructorsForLinkAccesses(FamilyID family)
{
    LinkType type;
    type.functions = LinkType::Functions::Constructors;
    type.constructorsOf = family;
    return type;
}

static LinkType closureForLinkAccesses(FunctionID function)
{
    LinkType type;
    type.functions = LinkType::Functions::Closure;
    type.function = function;
    return type;
}

static LinkType instancesAmongForLinkAccesses(const LinkType& type)
{
    return type.instancesAlone();
}

static bool holdsObjectsForLinkAccesses(const LinkType& type)
{
    return type.objects != LinkType::Objects::None || type.functions != LinkType::Functions::None || type.classes != LinkType::Classes::None || type.hasUnknownObjects || type.builtin;
}

struct SharedAccessInLinkAccesses {
    enum class Subjects : uint8_t { PrototypeObjects, ClassObjects };

    Subjects subjects { Subjects::PrototypeObjects };
    FunctionID firstFunction;
    SummaryAccess access;
    bool keyIsOnlyNumbers { false };
    bool keyCannotNameBuiltin { false };
    bool keyMayNamePrototypeLink { false };
    FlowNodeID subject;
    FlowNodeID value;
    FlowNodeID thrown;
};

struct MemberOfSharedAccessInLinkAccesses {
    uint32_t sharedAccess { 0 };
    LayoutID layout;
};

class StateOfLinkAccesses {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(StateOfLinkAccesses);
    WTF_MAKE_NONCOPYABLE(StateOfLinkAccesses);
public:
    StateOfLinkAccesses() = default;

    Vector<HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>> lookups;
    BytesSharedByThreads codeFoundAboveClosures;
    BitVector globalNamesStored;
    BytesSharedByThreads classesWhoseStaticFieldsAreLetGo;
    Vector<uint32_t> nearestClassesWithStaticFields;
    Vector<HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>> nearestClassesWithStaticFieldUnderKey;
    Vector<HashMap<uint64_t, uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>> whatBlocksRunBehindSteps;

    HashMap<std::pair<uint64_t, uint32_t>, uint32_t, DefaultHash<std::pair<uint64_t, uint32_t>>, WTF::PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>> indexesOfSharedAccesses;
    Vector<std::unique_ptr<SharedAccessInLinkAccesses>> sharedAccesses;
    Vector<MemberOfSharedAccessInLinkAccesses> membersOfSharedAccesses;
};

void DeleterOfStateOfLinkAccesses::operator()(StateOfLinkAccesses* state) const
{
    delete state;
}

static bool isAboveHomeObjectOfClassForLinkAccesses(const LinkType& type)
{
    bool namesOnlyPrototypes = type.objects == LinkType::Objects::None || type.objects == LinkType::Objects::Prototype || type.objects == LinkType::Objects::Prototypes;
    bool namesOnlyClassObjects = type.functions == LinkType::Functions::None || type.functions == LinkType::Functions::Closure || type.functions == LinkType::Functions::Constructors;
    return namesOnlyPrototypes && namesOnlyClassObjects && type.classes == LinkType::Classes::None && !type.hasUnknownObjects && !type.builtin && !(type.primitives & ~Primitive::UndefinedOrNull);
}

namespace {

enum class LookupInLinkAccesses : uint8_t { Absent, Found, Unknown };

struct PrivatePartsInLinkAccesses {
    const LinkType& typeObservedBefore;
    const Vector<uint32_t>& firstFieldOfLayout;
    StateOfLinkAccesses& state;
};

class AccessInLinkAccesses {
public:
    AccessInLinkAccesses(LinkFlow&, const PrivatePartsInLinkAccesses&, const FlowObserver&);
    void goByKey(PropertyKeyID key)
    {
        m_access.key = key;
        m_nameClass = m_flow.nameClassOf(key);
    }
    AccessInLinkAccesses(LinkFlow&, const PrivatePartsInLinkAccesses&, const SharedAccessInLinkAccesses&, uint32_t member, bool laysObservers);

    void resolve();
    void resolveMemberOfSharedAccess();

private:
    using Kind = SummaryAccess::Kind;
    using SharedAccess = SharedAccessInLinkAccesses;

    bool isRead() const { return m_access.kind == Kind::Read; }
    bool mayChangeListThatIsFollowed(const LinkType& instances) const
    {
        return !isRead() && m_access.kind != Kind::Has && (instances.classes == LinkType::Classes::Mixed || instances.builtinClass == BuiltinClasses::Array) && !m_flow.hasEscaped(instances.site);
    }
    void noteSiteOfCopyThatSpellsStore();
    void defineWrittenPrototypeAccessor(LayoutID);
    bool readsThroughSuperWhatIsOnlyCalled() const;
    bool isNamed() const { return !!m_access.key; }
    bool isTakenByLoader() const { return isRead() && m_function && isLibraryCode(m_program, m_program.at(m_function)) && std::ranges::contains(m_flow.summaries.at(m_function).namesThatLoaderTakes, m_access.value, &ThinSummary::NameThatLoaderTakes::read); }
    bool readKey();
    bool bodyConvertsKey() const;
    bool isNothingDefinedFrom(BuiltinID start, PropertyKeyID);
    bool isTextOfFunctions(const LinkType& key);
    bool readsOrStoresUnderTextsThatBodyWrites();
    bool accessesNamesFromLoop();
    using PrototypeNames = LinkFlow::PrototypeNames;
    bool appendNamesOfRecords(FlowNodeID, PrototypeNames, Vector<PropertyKeyID, 16>& names);
    PrototypeNames prototypeNamesAt(const BodyIndex&, ValueID object, ValueID name, const LinkType& objects);
    enum class ListedNames : bool { AreThere, MayBeGone };
    bool storeEachListedValue(const LinkType& listed, PrototypeNames, ListedNames);
    void resolveUnderNames(std::span<const PropertyKeyID>);
    bool storesUnderCallbackKeys(const LinkType& subject);
    bool accessesListedNames(const LinkType& subject);
    bool accessesPassedNames();
    FlowNodeID findGatheredArguments() const;
    void readGatheredArguments();
    bool holdsValueUnderKey(const BodyIndex&, const BodyOperation<Opcode::ElemSet>&, const LinkFlow::ListedObject&) const;
    bool isListedObject(const BodyIndex&, ValueID holder, const LinkFlow::ListedObject&) const;
    bool resolveUnderTexts(std::span<const StringID>);
    bool storesCopiedNamesOnPrototype(const LinkType& subject, const LinkType& source);
    bool isFinalListRead();

    void give(const LinkType&);
    void giveSymbol(PropertyKeyID);
    void giveAnything() { give(LinkType::anything()); }
    void givePrimitives(uint16_t primitives) { give(primitivesForLinkAccesses(primitives)); }
    void giveFrom(FlowNodeID);
    void giveWhatIsHeldAt(FlowNodeID);
    void giveWhatIsEstablishedOf(const DeclaredType&);
    void storeInto(FlowNodeID);
    LayoutID classTestedByInstanceOf();
    bool isExcludedByInstanceOf(LayoutID);
    LayoutID findClassRejectedByTest();
    LayoutID classRejectedByTest();
    bool isRejectedByTest(LayoutID);
    bool storesTextNumberOrBooleanThatIsWritten() const;
    bool isStoreByBodyOfFileInItsOwnText() const;
    bool storesWhatSetterOfPrototypeLinkPassesOver() const;
    void storeIntoUnseen() { storeInto(m_flow.unseen()); }
    void mayThrow();
    enum class UnknownIs : uint8_t { ByEscapeAlone, InSight };
    enum class TellsName : uint8_t { No, Yes };
    void cannotTell(UnknownIs = UnknownIs::InSight, TellsName = TellsName::Yes);
    void cannotTellWhatRuns(UnknownIs = UnknownIs::InSight);

    void noteChange(LayoutID layout, LayoutChange change)
    {
        if (!m_flow.changesByMoreThanStoresInSight[layout.index()].contains(change))
            m_flow.note(LinkFlow::Note::ChangeOfLayout, layout.index(), static_cast<uint32_t>(change));
    }
    void noteWrittenAfterConstruction(FieldReference reference)
    {
        uint32_t cell = m_parts.firstFieldOfLayout[reference.layout.index()] + reference.field;
        if (!m_flow.fieldsWrittenAfterConstruction.get(cell))
            m_flow.note(LinkFlow::Note::FieldWrittenAfterConstruction, cell);
    }
    template<typename Functor> void forEachLayoutUpTheBases(LayoutID, const Functor&) const;

    std::optional<LayoutID> m_classTestedByInstanceOf;
    std::optional<LayoutID> m_classRejectedByTest;
    FlowNodeID extraPropertyReadNode(LayoutID id) { return m_flow.extraPropertyReadNode(id); }
    FlowNodeID foundBesideFieldsUnderKey() { return m_flow.foundBesideFieldsUnder(m_access.key); }
    void storeBesideFieldsOf(LayoutID);
    bool resolveOnWhatStandsAbove(FlowNodeID above, unsigned stepsLeft);
    bool mayStoreIntoPrototypeLink() const { return m_access.kind == Kind::Store && m_keyMayNamePrototypeLink; }
    std::span<const uint32_t> definitionsByDescriptorAtSite() const;
    FlowNodeID findValueThatWrittenDescriptorLays() const;
    bool changeLaysAccessorInSight() const;
    bool changeLaysWhatDescriptorsInSightHold();
    bool namesOnlyElements() const { return !isNamed() && m_keyIsOnlyNumbers && indexesAreAbsentFrom(Builtins::ObjectPrototype); }
    bool namesNothingOnPlainClosures() const;

    void resolveUnderPrivateName();
    void resolveOnLayout(LayoutID, const LinkType& pooled);
    void resolvePrototypeLinkOfLayout(LayoutID, const Layout&);
    void resolveOnNamespace(LayoutID, const Layout&);
    void giveWhatNamespaceDoesNotList(LayoutID, ModuleID);
    void giveExport(ModuleID, uint32_t index);
    bool wasNamedBefore(LayoutID) const;
    void resolveOnSet(const LinkType& subject);
    void resolveOnFamily(FamilyID);
    LinkFlow::Lays howItLays(bool isPartOfConstruction) const;
    void resolveOnLostObjects(LayoutID member);
    void resolveOnPrototype(LayoutID);
    void resolveOnPrototypes(FamilyID);
    bool resolveThroughSharedAccess(SharedAccess::Subjects, FamilyID);
    FlowObserver observerOfWhatWasLookedUp() const { return FlowObserver { m_shared ? FlowObserver::Kind::MemberOfSharedAccess : FlowObserver::Kind::MemberOfAccess, m_function, m_record }; }
    FlowNodeID thrownNode() const { return m_shared ? m_shared->thrown : nodeTakingWhatIsThrown(m_flow, m_function, m_access.thrownTo); }
    void letPrototypeEscape(LayoutID id) { m_flow.escapeAllOf(prototypeForLinkAccesses(id)); }

    void resolveOnClosure(FunctionID);
    void resolvePrototypePropertyOf(FunctionID, LayoutID constructed);
    void giveConstructorsOfRowAbove(LayoutID);
    void resolveOnClassObject(FunctionID, LayoutID classLayout);
    void resolveOnConstructors(FamilyID);
    void resolveOnLostFunctions();
    void letClassObjectEscape(FunctionID, LayoutID classLayout);
    bool mayRunCodeFoundAboveClosures();
    bool findsCodeToRunAboveClosures();

    static bool findsGetterAmong(const LinkFlow::FoundMembers& members) { return std::ranges::contains(members, MethodKind::Getter, &LinkFlow::FoundMember::kind); }
    void enterAccessorAt(FlowNodeID member);
    void enterAccessor(FunctionID);
    void layOnReceiverUnlessAccessorAnswers(const LinkFlow::FoundMembers&);

    void resolveOnPrimitives(uint16_t);
    bool primitiveStoresRunNoCode(uint16_t primitives);
    void resolveOnClasses(const LinkType& instances);
    void resolveUnderNameOnClass(const LinkType& instances);
    template<typename Functor> void forEachListOfElements(const LinkType& instances, const Functor&);
    void resolveContents(const LinkType& subject);
    void resolveContentsOfInstances(const LinkType& instances);
    bool settlesPromiseThatCallHasJustMade();
    bool nothingRanSinceStepThatGaveKey() const;
    bool runsNoCodeOfProgram(const BodyIndex&, BodyValue, ValueID nameThatLoopGave) const;
    bool callOnlyLooksAtItsOperands(const BodyIndex&, BodyValue, ValueID nameThatLoopGave) const;
    BuiltinID memberThatTableLocksFrom(BuiltinID start, PropertyKeyID) const;
    bool isRefusedStore(BuiltinID holder) const;
    void handOn(const LinkType&);
    void resolveOnBuiltin(BuiltinID);
    bool testsMayHaveLaidNameFrom(BuiltinID start);
    BuiltinID prototypeOfInstancesOf(BuiltinClassID) const;
    bool indexesAreAbsentFrom(BuiltinID start) const;
    std::optional<uint32_t> indexWrittenAtAccess() const;
    bool hasNoIndexedElements(const LinkType& instances) const;
    bool textsOfPrimitivesAreAbsentFrom(BuiltinID start) const;
    bool keyNamesNoMemberAbove(const LinkType& instances);
    bool membersOfObjectPrototypeAreWhatTableLists();
    KeyAffixes affixesOfKeyRead() const;
    LookupInLinkAccesses lookUpFrom(BuiltinID start, BuiltinID& member) const;
    LookupInLinkAccesses lookUpInTableFrom(BuiltinID start, PropertyKeyID, BuiltinID& member) const;
    enum class EngineMayDefineInheritedNames : bool { No, Yes };
    bool giveWhatIsFoundFrom(BuiltinID start, EngineMayDefineInheritedNames = EngineMayDefineInheritedNames::No);
    bool areListsThatOnlyProgramNames(const LinkType& instances);
    const BuiltinHostObject* findHostObjectThatSubjectIsReadAs() const;
    bool giveWhatOtherNamesHold(const BuiltinHostObject&);
    bool tellsWhatMayAnswerFrom(BuiltinID start);
    bool tellsWhatMayAnswerUnderAnyNameBelow(BuiltinClassID);
    bool isServedByLightFormOf(BuiltinID) const;
    bool tellsMemberThatTableFindsFrom(BuiltinID) const;
    bool isKeyAbsentFromChain(BuiltinID) const;
    bool isFollowedOnGlobalObject(BuiltinID);
    bool isFollowedOnBuiltin(BuiltinID);
    bool storesListedObject(BuiltinID holder);
    bool isNameUndefinedFrom(BuiltinID start, BuiltinID followed = { });
    bool giveWhatLightFormServes(BuiltinID);
    LinkType anythingInPlaceOf(BuiltinID canonical);
    bool mayRunCodeFoundFrom(BuiltinID start);
    bool nativeSetterFoundFromKeepsReceiver(BuiltinID start);
    bool holdsWhatTableLists(BuiltinID member);
    LinkType typeHeldAtLocation(BuiltinID member);
    LinkType typeHeldAtLocationThatRealmHas(BuiltinID member);
    LinkType typeAnsweredByReadsOf(BuiltinID member);
    LinkType typeWrittenBy(BuiltinTypeID);
    LinkType typeWrittenOfLightForm(BuiltinTypeID, BuiltinID canonical) const;

    LinkFlow& m_flow;
    const PrivatePartsInLinkAccesses& m_parts;
    const Program& m_program;
    const BuiltinTable& m_table;
    FunctionID m_function;
    uint32_t m_record;
    const SharedAccess* m_shared { nullptr };
    SummaryAccess m_access;
    FlowNodeID m_subject;
    FlowNodeID m_receiver;
    FlowNodeID m_value;
    LinkType m_before;
    NameClass m_nameClass { NameClass::Ordinary };
    bool m_laysObservers;
    bool m_isUnderListedName { false };
    struct WordThatItCannotTell {
        FlowNodeID value;
        PropertyKeyID key;
        uint8_t how;
        bool operator==(const WordThatItCannotTell&) const = default;
    };
    Vector<WordThatItCannotTell, 4> m_wordsThatItCannotTell;
    bool m_keyIsWritten { true };
    bool m_flowBroughtHolder { false };
    void tellAgainOnceClimbs(FlowNodeID);
    void tellAgainOnceClimbsWhoeverTells(FlowNodeID);
    bool laysWatchAt(FlowNodeID, const FlowObserver&, unsigned line = __builtin_LINE()) const;
    void enterAccessorsOnPrototypesAbove(LayoutID);
    bool m_keyIsOnlyNumbers { false };
    bool m_keyCannotNameBuiltin { false };
    bool m_keyIsNumberBooleanOrNullish { false };
    bool m_keyIsSymbolAsFarAsIsSeen { false };
    bool m_seenKeysAreNumbers { false };
    bool m_keyMayNamePrototypeLink { false };
    bool m_keyIsLiteral { false };
    bool m_hasThrown { false };
    bool m_wasAskedToThrow { false };
    bool m_hasLeanedOnMemberTakenForConstant { false };
};

} // anonymous namespace

AccessInLinkAccesses::AccessInLinkAccesses(LinkFlow& flow, const PrivatePartsInLinkAccesses& parts, const FlowObserver& observer)
    : m_flow(flow)
    , m_parts(parts)
    , m_program(flow.program)
    , m_table(flow.program.builtins())
    , m_function(observer.function)
    , m_record(observer.record)
    , m_access(flow.summaries.at(observer.function).accesses[observer.record])
    , m_subject(flow.nodeOf(observer.function, m_access.subject))
    , m_receiver(m_access.receiver == noSummaryNode ? m_subject : flow.nodeOf(observer.function, m_access.receiver))
    , m_laysObservers(observer.kind == FlowObserver::Kind::SubjectOfAccess || (observer.kind == FlowObserver::Kind::KeyOfAccess && !m_access.isToContents()))
    , m_keyIsWritten(m_access.key)
{
    if (observer.kind == FlowObserver::Kind::SubjectOfAccess)
        m_before = parts.typeObservedBefore;
    if (m_access.value != noSummaryNode) {
        m_value = flow.nodeOf(observer.function, m_access.value);
        flow.prefetchTypeAt(m_value);
    }
    if (m_access.key)
        m_nameClass = flow.nameClassOf(m_access.key);
}

AccessInLinkAccesses::AccessInLinkAccesses(LinkFlow& flow, const PrivatePartsInLinkAccesses& parts, const SharedAccess& shared, uint32_t member, bool laysObservers)
    : m_flow(flow)
    , m_parts(parts)
    , m_program(flow.program)
    , m_table(flow.program.builtins())
    , m_function(shared.firstFunction)
    , m_record(member)
    , m_shared(&shared)
    , m_access(shared.access)
    , m_subject(shared.subject)
    , m_receiver(shared.subject)
    , m_value(shared.value)
    , m_laysObservers(laysObservers)
    , m_keyIsOnlyNumbers(shared.keyIsOnlyNumbers)
    , m_keyCannotNameBuiltin(shared.keyCannotNameBuiltin)
    , m_keyMayNamePrototypeLink(shared.keyMayNamePrototypeLink)
{
    if (m_access.key)
        m_nameClass = flow.nameClassOf(m_access.key);
}

bool AccessInLinkAccesses::laysWatchAt(FlowNodeID node, const FlowObserver& observer, unsigned line) const
{
    UNUSED_PARAM(line);
    return m_flow.isWatchedForFirstTime(node, observer);
}

void AccessInLinkAccesses::tellAgainOnceClimbs(FlowNodeID node)
{
    if (m_laysObservers)
        m_flow.observe(node, observerOfWhatWasLookedUp());
}

void AccessInLinkAccesses::tellAgainOnceClimbsWhoeverTells(FlowNodeID node)
{
    if (laysWatchAt(node, observerOfWhatWasLookedUp()))
        m_flow.observe(node, observerOfWhatWasLookedUp());
}

void AccessInLinkAccesses::give(const LinkType& type)
{
    if (isRead() && m_value)
        m_flow.arriveFromSource(m_value, type);
}

void AccessInLinkAccesses::giveSymbol(PropertyKeyID symbol)
{
    if (!isRead() || !m_value)
        return;
    LinkType oneSymbol;
    oneSymbol.primitives = LinkType::oneSymbol;
    m_flow.arrive(m_value, oneSymbol, symbol);
}

void AccessInLinkAccesses::giveFrom(FlowNodeID from)
{
    if (isRead() && m_value)
        m_flow.addEdge(from, m_value);
}

void AccessInLinkAccesses::giveWhatIsHeldAt(FlowNodeID from)
{
    if (isRead() && m_value)
        m_flow.addEdge(from, m_value, TypeTable::anything);
}

void AccessInLinkAccesses::giveWhatIsEstablishedOf(const DeclaredType& declared)
{
    std::optional<ProvenInEveryVersion> established = declared.established();
    if (!established)
        return;
    if (TypeID type = *established; type && isRead() && m_value)
        m_flow.arriveFromSource(m_value, type);
}

void AccessInLinkAccesses::storeInto(FlowNodeID to)
{
    if (!isRead() && m_value)
        m_flow.addEdge(m_value, to);
}

static ValueID stripCastsAndProofs(const BodyIndex& index, ValueID value)
{
    while (true) {
        ValueID stripped = index.stripCasts(value);
        if (ValueID subject = index.valueNarrowedInto(stripped))
            stripped = subject;
        if (stripped == value)
            return value;
        value = stripped;
    }
}

static LayoutID classOfBinding(const Program& program, const BodyIndex& index, ValueID value)
{
    while (!index.isParameter(value) && index.opcodeOf(value) == Opcode::CheckInitialized)
        value = index.value(value).operand(0);
    if (index.isParameter(value) || index.opcodeOf(value) != Opcode::GetModuleVariable)
        return { };
    const Variable& variable = program.at(index.value(value).as<Opcode::GetModuleVariable>().variable());
    if (!variable.flags.contains(VariableFlags::IsImmutable) || !variable.declared.isEstablished() || !variable.declared.written())
        return { };
    Type declared = program.at(variable.declared.written());
    declared.primitives &= ~Primitive::Absent;
    FunctionID constructor = singleFunctionOf(TypeReader(program), declared);
    return constructor && isClassConstructorOf(program.at(constructor).kind) ? program.at(constructor).constructedClass : LayoutID();
}

LinkFlow::InstanceOfGuardedReads LinkFlow::findInstanceOfGuardedReads(const Program& program, const BodyIndex& index, const ThinSummary& summary)
{
    struct Place {
        uint32_t subject { 0 };
        uint32_t dominatorNumber { 0 };
        uint32_t lastBelow { 0 };
        ValueID read;
        ValueID test;
        LayoutID tested;
    };
    InstanceOfGuardedReads found;
    Vector<Place> places;
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> subjects;
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        BlockID block(number);
        std::optional<BodyIncomingEdge> edge = index.isReachable(block) ? index.onlyIncomingEdge(block) : std::nullopt;
        if (!edge || edge->targetIndex)
            continue;
        BodyTerminator end = index.terminator(edge->from);
        if (end.kind() != TerminatorKind::Branch)
            continue;
        ValueID condition = end.as<TerminatorKind::Branch>().conditionOperand();
        while (!index.isParameter(condition) && index.opcodeOf(condition) == Opcode::ToBoolean)
            condition = index.value(condition).operand(0);
        if (index.isParameter(condition) || index.opcodeOf(condition) != Opcode::InstanceOf)
            continue;
        auto test = index.value(condition).as<Opcode::InstanceOf>();
        LayoutID tested = classOfBinding(program, index, test.constructorOperand());
        if (!tested)
            continue;
        uint32_t subject = stripCastsAndProofs(index, test.valueOperand()).index();
        subjects.add(subject);
        places.append(Place { subject, index.dominatorNumber(block), index.lastDominatorNumberBelow(block), { }, condition, tested });
    }
    if (places.isEmpty())
        return found;
    for (const SummaryAccess& access : summary.accesses) {
        if (access.kind != SummaryAccess::Kind::Read || !access.key || access.site.index() >= index.numberOfValues() || index.isParameter(access.site) || index.opcodeOf(access.site) != Opcode::PropGet)
            continue;
        uint32_t subject = stripCastsAndProofs(index, index.value(access.site).as<Opcode::PropGet>().objectOperand()).index();
        if (subjects.contains(subject))
            places.append(Place { subject, index.dominatorNumber(index.blockOf(access.site)), 0, access.site, { }, { } });
    }
    std::ranges::sort(places, [](const Place& first, const Place& second) {
        return std::tuple(first.subject, first.dominatorNumber, !!first.read, first.read.bits()) < std::tuple(second.subject, second.dominatorNumber, !!second.read, second.read.bits());
    });
    Vector<const Place*, 8> testsAround;
    for (const Place& place : places) {
        while (!testsAround.isEmpty() && (testsAround.last()->subject != place.subject || testsAround.last()->lastBelow < place.dominatorNumber))
            testsAround.removeLast();
        if (!place.read)
            testsAround.append(&place);
        else if (!testsAround.isEmpty())
            found.bySite.append(InstanceOfGuardedRead { place.read, testsAround.last()->test, testsAround.last()->tested });
    }
    std::ranges::sort(found.bySite, { }, &InstanceOfGuardedRead::site);
    for (uint32_t number = 0; number < summary.accesses.size(); ++number) {
        const SummaryAccess& access = summary.accesses[number];
        auto read = std::ranges::lower_bound(found.bySite, access.site, { }, &InstanceOfGuardedRead::site);
        if (access.kind == SummaryAccess::Kind::Read && access.value != noSummaryNode && read != found.bySite.end() && read->site == access.site)
            found.accessesByTest.append({ read->test, number });
    }
    std::ranges::sort(found.accessesByTest);
    return found;
}

LayoutID AccessInLinkAccesses::classTestedByInstanceOf()
{
    if (!m_classTestedByInstanceOf) {
        m_classTestedByInstanceOf = LayoutID();
        if (const LinkFlow::InstanceOfGuardedReads* reads = m_shared || !isRead() || !isNamed() ? nullptr : m_flow.instanceOfGuardedReads(m_function)) {
            auto read = std::ranges::lower_bound(reads->bySite, m_access.site, { }, &LinkFlow::InstanceOfGuardedRead::site);
            if (read != reads->bySite.end() && read->site == m_access.site)
                m_classTestedByInstanceOf = read->tested;
        }
    }
    return *m_classTestedByInstanceOf;
}

bool AccessInLinkAccesses::isExcludedByInstanceOf(LayoutID id)
{
    if (m_program.at(id).kind != LayoutKind::Instance || m_program.at(id).has(LayoutFlags::IsOfCreatedObject))
        return false;
    LayoutID tested = classTestedByInstanceOf();
    if (!tested)
        return false;
    size_t stepsLeft = m_program.layouts.size();
    for (LayoutID current = declaredLayoutOf(m_program, id); current && stepsLeft; --stepsLeft) {
        const Layout& layout = m_program.at(current);
        if (current == tested || !layout.constructor || !isClassConstructorOf(m_program.at(layout.constructor).kind) || (layout.has(LayoutFlags::HasHeritage) && !layout.base))
            return false;
        current = layout.base;
    }
    return true;
}

LayoutID AccessInLinkAccesses::findClassRejectedByTest()
{
    const BodyIndex* index = m_shared || isRead() || !isNamed() || m_program.at(m_function).kind != FunctionKind::DerivedConstructor ? nullptr : bodiesLookedAt(m_flow.looks).find(m_function);
    if (!index || !index->opcodes().contains(Opcode::IsUnlockedInstance) || m_access.site.index() >= index->numberOfValues() || index->isParameter(m_access.site))
        return { };
    BodyValue store = index->value(m_access.site);
    ValueID object = store.opcode() == Opcode::PropDefine ? store.as<Opcode::PropDefine>().objectOperand() : store.opcode() == Opcode::PropSet ? store.as<Opcode::PropSet>().objectOperand() : ValueID();
    std::optional<BodyIncomingEdge> edge = object ? index->onlyIncomingEdge(index->blockOf(m_access.site)) : std::nullopt;
    if (!edge || !edge->targetIndex)
        return { };
    BodyTerminator end = index->terminator(edge->from);
    if (end.kind() != TerminatorKind::Branch)
        return { };
    ValueID condition = end.as<TerminatorKind::Branch>().conditionOperand();
    if (index->isParameter(condition) || index->opcodeOf(condition) != Opcode::IsUnlockedInstance || index->stripCasts(index->value(condition).operand(0)) != index->stripCasts(object))
        return { };
    const Type& tested = m_program.at(index->value(condition).as<Opcode::IsUnlockedInstance>().testedType());
    std::span<const LayoutID> layouts = m_program.types.listOf(tested.layouts);
    if (layouts.size() != 1 || m_flow.hasLockableInstances(layouts[0]))
        return { };
    FlowNodeID told = m_flow.instanceLockNode(layouts[0]);
    FlowObserver observer { FlowObserver::Kind::MemberOfAccess, m_function, m_record };
    if (laysWatchAt(told, observer))
        m_flow.observe(told, observer);
    return m_flow.hasLockableInstances(layouts[0]) ? LayoutID() : layouts[0];
}

LayoutID AccessInLinkAccesses::classRejectedByTest()
{
    if (!m_classRejectedByTest)
        m_classRejectedByTest = findClassRejectedByTest();
    return *m_classRejectedByTest;
}

bool AccessInLinkAccesses::isRejectedByTest(LayoutID id)
{
    LayoutID refused = m_program.at(id).kind == LayoutKind::Instance && !m_program.at(id).has(LayoutFlags::IsOfCreatedObject) ? classRejectedByTest() : LayoutID();
    return refused && m_flow.standsAtOrBelow(declaredLayoutOf(m_program, id), refused);
}

bool AccessInLinkAccesses::isStoreByBodyOfFileInItsOwnText() const
{
    const Function& whoStores = m_program.at(m_function);
    if (m_shared || !whoStores.isModuleBody())
        return false;
    if (isLibraryCode(m_program, whoStores))
        return true;
    if (m_access.value == noSummaryNode)
        return false;
    const SummaryNode& stored = m_flow.summaries.at(m_function).nodes[m_access.value];
    if (stored.kind == SummaryNode::Kind::Closure)
        return true;
    if (stored.kind != SummaryNode::Kind::Variable)
        return false;
    const Variable& variable = m_program.at(VariableID(stored.first));
    return variable.initialization == VariableInitialization::Function && variable.flags.contains(VariableFlags::IsImmutable);
}

bool AccessInLinkAccesses::storesTextNumberOrBooleanThatIsWritten() const
{
    if (m_shared || m_access.value == noSummaryNode)
        return false;
    constexpr uint16_t textsNumbersAndBooleans = LinkType::closed(Primitive::AnyString | Primitive::Number | Primitive::Boolean);
    const SummaryNode& stored = m_flow.summaries.at(m_function).nodes[m_access.value];
    if (stored.kind == SummaryNode::Kind::ResultOfOperator) {
        std::optional<uint16_t> given = m_flow.primitivesThatOperatorGivesWhateverArrives(m_function, stored.first);
        return given && !(*given & ~textsNumbersAndBooleans);
    }
    if (stored.kind == SummaryNode::Kind::ResultOfCall) {
        const SummaryCall& call = m_flow.summaries.at(m_function).calls[stored.first];
        if (call.callee != SummaryCall::Callee::Builtin || stored.second || m_program.isUnlocked(call.builtin) || m_program.isUnlocked(m_table.canonicalID(call.builtin)))
            return false;
        static constexpr OptionSet<BuiltinPrimitive> textsNumbersAndBooleansOfTable { BuiltinPrimitive::String, BuiltinPrimitive::Number, BuiltinPrimitive::Boolean };
        bool hasRow = false;
        for (const BuiltinOverload& row : m_table.overloadsOf(m_table.canonicalID(call.builtin))) {
            if (row.mode != (call.isConstruction ? BuiltinCallMode::Construct : BuiltinCallMode::Call))
                continue;
            if (row.isUndeclared() || row.resultIsDeclaredOnly() || row.hasTypedResult() || !row.result.type)
                return false;
            const BuiltinType& answered = m_table.at(row.result.type);
            if (answered.mayBeObject() || !(answered.primitives - textsNumbersAndBooleansOfTable).isEmpty())
                return false;
            hasRow = true;
        }
        return hasRow;
    }
    if (stored.kind != SummaryNode::Kind::Constant)
        return false;
    LinkType type = m_flow.fromTypeID(TypeID::fromBits(stored.first));
    return !holdsObjectsForLinkAccesses(type) && !(type.primitives & ~textsNumbersAndBooleans);
}

bool AccessInLinkAccesses::storesWhatSetterOfPrototypeLinkPassesOver() const
{
    if (m_shared || m_access.kind != Kind::Store || m_access.value == noSummaryNode)
        return false;
    constexpr uint16_t passedOver = LinkType::closed(Primitive::All & ~Primitive::Null);
    const SummaryNode& stored = m_flow.summaries.at(m_function).nodes[m_access.value];
    if (stored.kind == SummaryNode::Kind::ResultOfOperator) {
        std::optional<uint16_t> given = m_flow.primitivesThatOperatorGivesWhateverArrives(m_function, stored.first);
        return given && !(*given & ~passedOver);
    }
    if (stored.kind != SummaryNode::Kind::Constant)
        return false;
    LinkType type = m_flow.fromTypeID(TypeID::fromBits(stored.first));
    return !holdsObjectsForLinkAccesses(type) && !(type.primitives & ~passedOver);
}

void AccessInLinkAccesses::mayThrow()
{
    m_wasAskedToThrow = true;
    if (!std::exchange(m_hasThrown, true)) {
        FlowNodeID thrown = thrownNode();
        if (m_flow.arrivalOfAnythingInSightBringsNothingTo(thrown))
            return;
        LinkFlow::UnknownByEscapeAlone unknownInSight(false);
        m_flow.arrive(thrown, LinkType::anything());
    }
}

bool AccessInLinkAccesses::readsThroughSuperWhatIsOnlyCalled() const
{
    const BodyIndex* index = m_shared ? nullptr : bodiesLookedAt(m_flow.looks).find(m_function);
    if (!index || !index->opcodes().contains(Opcode::SuperGet))
        return false;
    if (m_access.site.index() >= index->numberOfValues() || index->isParameter(m_access.site) || index->value(m_access.site).opcode() != Opcode::SuperGet)
        return false;
    ValueID user = index->onlyUsingValue(m_access.site);
    if (!user)
        return false;
    BodyValue call = index->value(user);
    return (call.opcode() == Opcode::Call || call.opcode() == Opcode::CallList) && call.operand(0) == m_access.site && call.operand(1);
}

void AccessInLinkAccesses::noteSiteOfCopyThatSpellsStore()
{
    const ThinSummary& summary = m_flow.summaries.at(m_function);
    auto found = std::ranges::equal_range(summary.callsAtSitesOfCopiesThatSpellStores.span(), m_access.site.index(), { }, [&](uint32_t call) {
        return summary.calls[call].site.index();
    });
    for (uint32_t place : found) {
        const SummaryCall& call = summary.calls[place];
        if (call.callee != SummaryCall::Callee::Builtin || call.passesList)
            continue;
        for (uint32_t argument = 1; argument < call.numberOfArguments; ++argument)
            m_flow.noteSiteOfCopy(m_function, m_access.site, m_subject, m_flow.nodeOf(m_function, summary.arguments[call.firstArgument + argument]));
    }
}

static bool onlyLooksAtOperandForLinkAccesses(const BuiltinParameter& parameter)
{
    return (parameter.uses - OperandUses { OperandUse::Inspects, OperandUse::PassesToUserCode }).isEmpty() && (parameter.protocols - Protocols { Protocol::OwnKeys, Protocol::PrototypeChain }).isEmpty()
        && (parameter.conversion == BuiltinConversion::None || parameter.conversion == BuiltinConversion::ToObject) && parameter.namedMembers.empty() && parameter.calledMembers.empty() && parameter.convertedMembers.empty();
}

BuiltinID AccessInLinkAccesses::memberThatTableLocksFrom(BuiltinID start, PropertyKeyID key) const
{
    BuiltinID member;
    if (!key || lookUpInTableFrom(start, key, member) != LookupInLinkAccesses::Found)
        return { };
    const Builtin& entry = m_table.at(member);
    bool isLocked = (entry.mutability == BuiltinMutability::AlwaysLocked || m_table.isLocationOfLockedObject(member)) && !m_program.isUnlocked(member) && !m_program.isUnlocked(m_table.canonicalID(member));
    return isLocked && entry.kind != BuiltinKind::Accessor && entry.scope == BuiltinScope::Realm && !entry.flags.contains(BuiltinFlag::IsOwnPropertyOfInstances) ? member : BuiltinID();
}

bool AccessInLinkAccesses::isRefusedStore(BuiltinID holder) const
{
    if (m_access.kind != Kind::Store || !isNamed() || m_receiver != m_subject || m_program.admitsReplacedBuiltins)
        return false;
    BuiltinID member = findOwnMember(m_program, m_table.canonicalID(holder), m_access.key);
    return member && memberThatTableLocksFrom(holder, m_access.key) == member;
}

bool AccessInLinkAccesses::callOnlyLooksAtItsOperands(const BodyIndex& index, BodyValue call, ValueID nameThatLoopGave) const
{
    auto builtinWrittenAs = [&](ValueID value) {
        ValueID written = value ? index.stripCasts(value) : ValueID();
        return written && !index.isParameter(written) && index.opcodeOf(written) == Opcode::Builtin ? m_table.canonicalID(index.value(written).as<Opcode::Builtin>().builtin()) : BuiltinID();
    };
    ValueID receiver = call.operand(1);
    std::span<const ValueID> operands = call.variadicOperands();
    BuiltinID called = builtinWrittenAs(call.operand(0));
    if (!called) {
        ValueID callee = index.stripCasts(call.operand(0));
        if (index.isParameter(callee) || index.opcodeOf(callee) != Opcode::PropGet)
            return false;
        called = builtinWrittenAs(index.value(callee).as<Opcode::PropGet>().objectOperand());
        BuiltinID member = called && builtinWrittenAs(receiver) == called ? memberThatTableLocksFrom(called, index.value(callee).as<Opcode::PropGet>().key()) : BuiltinID();
        if (!member || m_table.canonicalID(member) != Builtins::FunctionPrototypeCall)
            return false;
        receiver = operands.empty() ? ValueID() : operands.front();
        operands = operands.subspan(operands.empty() ? 0 : 1);
    }
    auto isOnlyLookedAt = [&](const BuiltinOverload& row, ValueID operand, BuiltinOperand position, bool isReceiver = false) {
        ValueID written = operand ? index.stripCasts(operand) : ValueID();
        if (!written || written == nameThatLoopGave || (!index.isParameter(written) && primitiveConstants.contains(index.opcodeOf(written))))
            return true;
        const BuiltinParameter* parameter = m_table.parameterFor(row, position);
        return parameter ? onlyLooksAtOperandForLinkAccesses(*parameter) : isReceiver;
    };
    bool hasRow = false;
    for (const BuiltinOverload& row : m_table.overloadsOf(called)) {
        if (row.mode != BuiltinCallMode::Call)
            continue;
        hasRow = true;
        bool runsOtherCode = row.effects.capability == BuiltinCapability::JS && !row.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode);
        if (row.isUndeclared() || runsOtherCode || !row.flow.callbacks.empty() || !row.flow.handlerCalls.empty() || operands.size() > BuiltinOperand::maxArgumentIndex)
            return false;
        for (const BuiltinPropertyAction& action : row.flow.propertyActions) {
            if (action.kind != BuiltinPropertyActionKind::TestsByKey)
                return false;
        }
        if (!isOnlyLookedAt(row, receiver, BuiltinOperand::receiver(), true))
            return false;
        for (uint32_t position = 0; position < operands.size(); ++position) {
            if (!isOnlyLookedAt(row, operands[position], BuiltinOperand::argument(position)))
                return false;
        }
    }
    return hasRow;
}

bool AccessInLinkAccesses::runsNoCodeOfProgram(const BodyIndex& index, BodyValue value, ValueID nameThatLoopGave) const
{
    Effects storage;
    const Effects& effects = declaredEffectsOfOperation(m_program, value.opcode(), value, storage);
    if (!effects.mayCallUnknownCode && !effects.maySuspend && effects.writes.containsOnly(unobservableWriteHeaps)) {
        bool someOperandMayRunCode = false;
        if (operandsMayRunCode(value.opcode())) {
            static constexpr OpcodeSet opcodesWhoseResultsRunNothing = primitiveConstants | allNumericOperators | allRelations | allEqualities;
            value.forEachOperand([&](ValueID operand, uint32_t) {
                ValueID written = operand ? index.stripCasts(operand) : ValueID();
                if (written && written != nameThatLoopGave)
                    someOperandMayRunCode |= index.isParameter(written) || !opcodesWhoseResultsRunNothing.contains(index.opcodeOf(written));
            });
        }
        if (!someOperandMayRunCode)
            return true;
    }
    if (value.opcode() == Opcode::PropHas)
        return true;
    if (value.opcode() == Opcode::ElemHas)
        return index.stripCasts(value.as<Opcode::ElemHas>().propertyOperand()) == nameThatLoopGave;
    if (value.opcode() == Opcode::PropGet) {
        ValueID object = index.stripCasts(value.as<Opcode::PropGet>().objectOperand());
        return !index.isParameter(object) && index.opcodeOf(object) == Opcode::Builtin && memberThatTableLocksFrom(index.value(object).as<Opcode::Builtin>().builtin(), value.as<Opcode::PropGet>().key());
    }
    return value.opcode() == Opcode::Call && callOnlyLooksAtItsOperands(index, value, nameThatLoopGave);
}

bool AccessInLinkAccesses::nothingRanSinceStepThatGaveKey() const
{
    const BodyIndex* index = m_shared ? nullptr : bodiesLookedAt(m_flow.looks).find(m_function);
    if (!index || index->isParameter(m_access.site) || index->opcodeOf(m_access.site) != Opcode::ElemGet)
        return false;
    ValueID nameThatLoopGave = index->stripCasts(index->value(m_access.site).as<Opcode::ElemGet>().propertyOperand());
    ValueID step = index->value(nameThatLoopGave).as<Opcode::ResultAt>().resultsOperand();
    BlockID blockOfStep = index->blockOf(step);

    constexpr uint32_t isNotReached = 0;
    constexpr uint32_t runsNothing = UINT32_MAX;
    auto& known = m_parts.state.whatBlocksRunBehindSteps[m_flow.indexOfLaneAtWork()];
    auto keyOf = [&](BlockID block) { return static_cast<uint64_t>(m_function.index()) << 32 | block.index(); };
    Vector<BlockID, 8> blocksToAsk;
    uint32_t above = runsNothing;
    for (BlockID block = index->blockOf(m_access.site);;) {
        if (auto found = known.find(keyOf(block)); found != known.end() && found->value >> 32 == step.index()) {
            above = static_cast<uint32_t>(found->value);
            break;
        }
        blocksToAsk.append(block);
        if (block == blockOfStep)
            break;
        std::optional<BodyIncomingEdge> edge = index->onlyIncomingEdge(block);
        if (!edge) {
            above = isNotReached;
            break;
        }
        block = edge->from;
    }
    while (!blocksToAsk.isEmpty()) {
        BlockID block = blocksToAsk.takeLast();
        uint32_t first = above == runsNothing ? runsNothing : isNotReached;
        if (first) {
            for (BodyValue value : index->block(block).values()) {
                if ((block != blockOfStep || value.id().index() > step.index()) && !runsNoCodeOfProgram(*index, value, nameThatLoopGave)) {
                    first = value.id().index() + 1;
                    break;
                }
            }
        }
        known.set(keyOf(block), static_cast<uint64_t>(step.index()) << 32 | first);
        above = first;
    }
    return m_access.site.index() < above;
}

void AccessInLinkAccesses::cannotTell(UnknownIs unknownIs, TellsName tellsName)
{
    LinkFlow::UnknownByEscapeAlone unknownByEscapeAlone(unknownIs == UnknownIs::ByEscapeAlone, LinkFlow::UnknownByEscapeAlone::Marks::OfRule);
    bool isToldOf = tellsName == TellsName::Yes && isRead() && !m_access.answerIsOnlyCalled && (m_shared || !m_flow.rowsSpeakOfKeyFor(m_function, m_access.key));
    bool tellsSite = !m_shared && unknownIs == UnknownIs::InSight && !m_flow.isInLibraryCode() && m_access.site.index() < 1u << 24;
    bool isOnlyCalled = isToldOf && isNamed() && readsThroughSuperWhatIsOnlyCalled();
    bool answerStaysInSight = isToldOf && isNamed() && !isOnlyCalled && !m_flow.builtinsReadByNameAreUsed() && m_flow.answerOfReadStaysInSight(m_access.key);
    if (isNamed()) {
        WordThatItCannotTell word { m_value, m_access.key, static_cast<uint8_t>((unknownIs == UnknownIs::InSight) | isToldOf << 1 | isOnlyCalled << 2 | answerStaysInSight << 3) };
        if (m_wordsThatItCannotTell.contains(word))
            return;
        m_wordsThatItCannotTell.append(word);
    }
    if (answerStaysInSight)
        m_flow.note(LinkFlow::Note::NameReadWithAnswerInSight, m_access.key.bits(), !m_flow.isInLibraryCode());
    else if (isToldOf && isNamed())
        m_flow.note(isOnlyCalled ? LinkFlow::Note::NameCalled : LinkFlow::Note::NameRead, m_access.key.bits(), !m_flow.isInLibraryCode());
    else if (isToldOf && !m_keyCannotNameBuiltin && m_access.keyIsNameThatLoopOverSubjectGave && nothingRanSinceStepThatGaveKey())
        m_flow.note(LinkFlow::Note::ReadUnderNameThatLoopGave, 0);
    else if (isToldOf && !m_keyCannotNameBuiltin)
        m_flow.note(LinkFlow::Note::ReadUnderKeyNobodyKnows, tellsSite ? m_function.index() + 1 : 0, tellsSite ? m_access.site.index() : 0);
    mayThrow();
    give(answerStaysInSight ? LinkType::anythingWithBuiltinsReadByName() : LinkType::anything());
    storeIntoUnseen();
    if (m_receiver != m_subject)
        m_flow.addEdge(m_receiver, m_flow.unseen());
}

void AccessInLinkAccesses::cannotTellWhatRuns(UnknownIs unknownIs)
{
    m_flow.addEdge(m_subject, m_flow.unseen());
    cannotTell(unknownIs);
}

template<typename Functor>
void AccessInLinkAccesses::forEachLayoutUpTheBases(LayoutID first, const Functor& stopsAt) const
{
    size_t steps = 0;
    for (LayoutID current = first; current && steps < m_program.layouts.size(); current = m_program.at(current).base, ++steps) {
        if (stopsAt(current))
            return;
    }
}

void AccessInLinkAccesses::storeBesideFieldsOf(LayoutID id)
{
    storeInto(m_flow.appendedStoreNode(id, m_access.key));
}

bool AccessInLinkAccesses::resolveOnWhatStandsAbove(FlowNodeID above, unsigned stepsLeft)
{
    if ((m_laysObservers && !m_isUnderListedName) || m_flow.isWatchedForFirstTime(above, observerOfWhatWasLookedUp()))
        m_flow.observe(above, observerOfWhatWasLookedUp());
    const LinkType type = m_flow.typeAt(above);
    bool setterTakesStore = false;
    auto lookAt = [&](LayoutID holder) {
        std::optional<FieldReference> field = findField(m_program, holder, m_access.key);
        if (!field) {
            if (stepsLeft && m_program.at(holder).has(LayoutFlags::IsOfCreatedObject))
                setterTakesStore |= resolveOnWhatStandsAbove(m_flow.whatStandsAbove(holder), stepsLeft - 1);
            return;
        }
        if (isRead())
            giveWhatIsHeldAt(m_flow.field(*field));
        else if (m_access.kind == Kind::Store && m_flow.hasSetterWritten(*field)) {
            storeInto(m_flow.field(*field));
            setterTakesStore = true;
        }
    };
    switch (type.objects) {
    case LinkType::Objects::Layout:
        lookAt(type.layout);
        break;
    case LinkType::Objects::Set:
        for (LayoutID member : m_flow.layoutsOfSet(type))
            lookAt(member);
        break;
    case LinkType::Objects::Family:
        m_program.at(m_flow.families.layoutsOf(type.family)).forEach(lookAt);
        break;
    case LinkType::Objects::None:
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
    case LinkType::Objects::Lost:
        break;
    }
    if (type.classes == LinkType::Classes::Site && stepsLeft)
        setterTakesStore |= resolveOnWhatStandsAbove(m_flow.whatStandsAbove(type.site), stepsLeft - 1);
    return setterTakesStore;
}

bool AccessInLinkAccesses::bodyConvertsKey() const
{
    const BodyIndex* index = m_shared || !m_access.site ? nullptr : bodiesLookedAt(m_flow.looks).find(m_function);
    if (!index || m_access.site.index() >= index->numberOfValues() || index->isParameter(m_access.site))
        return false;
    switch (index->opcodeOf(m_access.site)) {
    case Opcode::ElemGet:
    case Opcode::ElemSet:
    case Opcode::ElemHas:
    case Opcode::ElemDelete:
        return true;
    default:
        return false;
    }
}

bool AccessInLinkAccesses::isNothingDefinedFrom(BuiltinID start, PropertyKeyID key)
{
    MembersUnderKey members(m_program, key);
    for (BuiltinID holder = m_table.canonicalID(start);; holder = m_table.canonicalID(m_table.at(holder).inheritsFrom)) {
        if (members.findOwnMember(holder) || !listsEveryMemberUnder(m_program, holder, key) || !m_table.hasLockedPrototypeLink(holder))
            return false;
        bool isClosed = members.isClosedKey(holder) || m_flow.stillNothing(holder, key, [&](FlowNodeID laid) {
            tellAgainOnceClimbsWhoeverTells(laid);
            return true;
        }, LinkFlow::LockAbsentName::IfPossible);
        if (!isClosed)
            return false;
        if (m_table.at(holder).prototypeLink != BuiltinPrototypeLink::Builtin)
            return m_table.at(holder).prototypeLink == BuiltinPrototypeLink::Null;
    }
}

bool AccessInLinkAccesses::isTextOfFunctions(const LinkType& key)
{
    bool holdsOthers = key.objects != LinkType::Objects::None || key.classes != LinkType::Classes::None || key.builtin || key.hasUnknownObjects || key.holdsLostBuiltins;
    if (m_shared || holdsOthers || (key.functions != LinkType::Functions::Closure && key.functions != LinkType::Functions::Set) || m_flow.testRuns().isTestRun())
        return false;
    PropertyKeyID toString = m_program.keyOf(BuiltinNames::toString);
    PropertyKeyID toPrimitive = m_program.keyOf(m_table.nameOf(WellKnownSymbol::ToPrimitive));
    BuiltinID member;
    if (toString && (lookUpInTableFrom(Builtins::FunctionPrototype, toString, member) != LookupInLinkAccesses::Found || !holdsWhatTableLists(member)))
        return false;
    if (toPrimitive && !isNothingDefinedFrom(Builtins::FunctionPrototype, toPrimitive))
        return false;
    auto holdsNothing = [&](FlowNodeID laid) {
        tellAgainOnceClimbsWhoeverTells(laid);
        return m_flow.typeAt(laid).isNever();
    };
    auto isText = [&](FunctionID function) {
        if (isClassConstructorOf(m_program.at(function).kind) || m_flow.escapedFunctions.get(function.index()) || !holdsNothing(m_flow.foundOnClosuresOf(function)))
            return false;
        return std::ranges::all_of(std::initializer_list<PropertyKeyID> { toString, toPrimitive }, [&](PropertyKeyID name) {
            return !name || holdsNothing(m_flow.followsNamesOnClosuresOf(function, name) ? m_flow.laidOnClosuresOf(function, name) : m_flow.foundBesideFieldsUnder(name));
        });
    };
    if (key.functions == LinkType::Functions::Closure)
        return isText(key.function);
    return std::ranges::all_of(m_flow.functionsOfSet(key), isText);
}

bool AccessInLinkAccesses::readKey()
{
    bool hasNode = m_access.keyNode != noSummaryNode;
    FlowNodeID node = hasNode ? m_flow.nodeOf(m_function, m_access.keyNode) : FlowNodeID();
    LinkType key = hasNode ? m_flow.typeAt(node) : LinkType::anything();
    key.primitives &= LinkType::closed(Primitive::All);
    if (key.isNever())
        return false;
    if (hasNode && !m_access.isToContents())
        m_flow.noteKeyOfAccessInLibrary(m_function, m_record, m_access);
    if (PropertyKeyID symbol = m_flow.keyOf(m_function, m_access)) {
        m_access.key = symbol;
        m_nameClass = m_flow.nameClassOf(symbol);
        return true;
    }

    bool holdsObjects = holdsObjectsForLinkAccesses(key);
    if (holdsObjects) {
        if (hasNode && !bodyConvertsKey())
            m_flow.addEdge(node, m_flow.unseen());
        mayThrow();
    }
    m_keyIsOnlyNumbers = !holdsObjects && !(key.primitives & ~LinkType::closed(Primitive::Number));
    m_keyCannotNameBuiltin = !(key.primitives & LinkType::closed(Primitive::AnyString | Primitive::AnySymbol)) && (!holdsObjects || (isRead() && isTextOfFunctions(key)));
    m_keyIsNumberBooleanOrNullish = !holdsObjects && !(key.primitives & ~LinkType::closed(Primitive::Number | Primitive::Boolean | Primitive::UndefinedOrNull));
    m_keyIsSymbolAsFarAsIsSeen = hasNode && LinkFlow::isSymbolAsFarAsIsSeen(key);
    LinkType seenKeys = key.directPart();
    m_seenKeysAreNumbers = !holdsObjectsForLinkAccesses(seenKeys) && !(seenKeys.primitives & ~LinkType::closed(Primitive::Number));
    m_keyIsLiteral = !holdsObjects && !(key.primitives & (Primitive::String | LinkType::closed(Primitive::AnySymbol)));
    m_keyMayNamePrototypeLink = (holdsObjects || key.primitives & Primitive::String) && keyThatBeginsSoMayNamePrototypeLink(m_program, m_access) && !storesWhatSetterOfPrototypeLinkPassesOver();
    return true;
}

static bool mayLeaveLoopForLinkAccesses(const BodyIndex& index, ValueID key)
{
    key = index.stripCasts(key);
    if (index.isParameter(key))
        return index.blockOf(key) == entryBlock;
    return index.opcodeOf(key) == Opcode::ResultAt || index.opcodeOf(key) == Opcode::ElemGet;
}

static bool readsElementInBoundsForLinkAccesses(const Program&, const BodyIndex&, BodyValue element, ValueID list);

static VariableID variableReadAtForLinkAccesses(const BodyIndex& index, ValueID value)
{
    while (value && !index.isParameter(value) && inputPassThroughOpcodes.contains(index.opcodeOf(value)))
        value = index.value(value).operand(0);
    return value && !index.isParameter(value) && index.opcodeOf(value) == Opcode::GetModuleVariable ? index.value(value).as<Opcode::GetModuleVariable>().variable() : VariableID();
}

static bool onlyReadsItsListForLinkAccesses(const Program& program, PropertyKeyID key, bool& takesCallback)
{
    const BuiltinID plain[] { Builtins::ArrayPrototypeIncludes, Builtins::ArrayPrototypeIndexOf, Builtins::ArrayPrototypeLastIndexOf, Builtins::ArrayPrototypeJoin, Builtins::ArrayPrototypeAt };
    const BuiltinID withCallback[] { Builtins::ArrayPrototypeForEach, Builtins::ArrayPrototypeMap, Builtins::ArrayPrototypeSome, Builtins::ArrayPrototypeEvery, Builtins::ArrayPrototypeFilter, Builtins::ArrayPrototypeFind, Builtins::ArrayPrototypeFindIndex };
    const BuiltinTable& builtins = program.builtins();
    BuiltinID found = key ? findOwnMember(program, builtins.canonicalID(Builtins::ArrayPrototype), key) : BuiltinID();
    auto isFound = [&](BuiltinID method) {
        return found && found == builtins.canonicalID(method) && !program.isUnlocked(method) && !program.isUnlocked(found);
    };
    takesCallback = std::ranges::any_of(withCallback, isFound);
    return takesCallback || std::ranges::any_of(plain, isFound);
}

static bool appendTextsOfWrittenListForLinkAccesses(const Program& program, const BodyIndex& index, ValueID list, Vector<StringID, 8>& texts)
{
    BodyValue made = index.value(list);
    if (made.opcode() == Opcode::NewArray) {
        if (!made.as<Opcode::NewArray>().holes().empty())
            return false;
        for (ValueID element : made.variadicOperands()) {
            if (!element || index.isParameter(element) || index.opcodeOf(element) != Opcode::String)
                return false;
            texts.append(index.value(element).as<Opcode::String>().string());
        }
        return !texts.isEmpty();
    }
    if (made.opcode() != Opcode::Data)
        return false;
    DataID data = made.as<Opcode::Data>().data();
    const DataNode::Array* array = data ? program.at(data).root.tryAs<DataNode::Array>() : nullptr;
    if (!array)
        return false;
    for (const DataNode& element : array->elements) {
        if (element.kind() != DataNode::Kind::String)
            return false;
        texts.append(element.as<DataNode::String>().value);
    }
    return !texts.isEmpty();
}

static bool appendFirstTextsOfWrittenPairsForLinkAccesses(const BodyIndex& index, ValueID list, Vector<StringID, 8>& texts)
{
    BodyValue made = index.value(list);
    if (made.opcode() != Opcode::NewArray || !made.as<Opcode::NewArray>().holes().empty())
        return false;
    for (ValueID pair : made.variadicOperands()) {
        if (!pair || index.isParameter(pair) || index.opcodeOf(pair) != Opcode::NewArray || !index.hasOneUse(pair) || !index.value(pair).as<Opcode::NewArray>().holes().empty())
            return false;
        auto elements = index.value(pair).variadicOperands();
        if (elements.empty() || !elements[0] || index.isParameter(elements[0]) || index.opcodeOf(elements[0]) != Opcode::String)
            return false;
        texts.append(index.value(elements[0]).as<Opcode::String>().string());
    }
    return !texts.isEmpty();
}

static bool appendRowStringsForLinkAccesses(const Program& program, const BodyIndex& index, ValueID list, uint32_t position, uint32_t length, Vector<StringID, 8>& strings)
{
    auto appendStringOfData = [&](const DataNode& row) {
        const auto* elements = row.tryAs<DataNode::Array>();
        if (!elements || elements->elements.size() < length || elements->elements[position].kind() != DataNode::Kind::String)
            return false;
        strings.append(elements->elements[position].as<DataNode::String>().value);
        return true;
    };
    BodyValue made = index.value(list);
    if (made.opcode() == Opcode::Data) {
        DataID data = made.as<Opcode::Data>().data();
        const auto* rows = data ? program.at(data).root.tryAs<DataNode::Array>() : nullptr;
        if (!rows)
            return false;
        for (const DataNode& row : rows->elements) {
            if (!appendStringOfData(row))
                return false;
        }
        return !strings.isEmpty();
    }
    if (made.opcode() != Opcode::NewArray || !made.as<Opcode::NewArray>().holes().empty())
        return false;
    for (ValueID row : made.variadicOperands()) {
        if (!row || index.isParameter(row) || !index.hasOneUse(row))
            return false;
        BodyValue rowMade = index.value(row);
        if (rowMade.opcode() == Opcode::Data) {
            DataID data = rowMade.as<Opcode::Data>().data();
            if (!data || !appendStringOfData(program.at(data).root))
                return false;
            continue;
        }
        if (rowMade.opcode() != Opcode::NewArray || !rowMade.as<Opcode::NewArray>().holes().empty())
            return false;
        auto elements = rowMade.variadicOperands();
        if (elements.size() < length || !elements[position] || index.isParameter(elements[position]) || index.opcodeOf(elements[position]) != Opcode::String)
            return false;
        strings.append(index.value(elements[position]).as<Opcode::String>().string());
    }
    return !strings.isEmpty();
}

static ValueID whatStepGoesThroughForLinkAccesses(const BodyIndex& index, ValueID value, bool takesApart)
{
    while (value && !index.isParameter(value) && opcodesThatStateTypes.contains(index.opcodeOf(value)))
        value = index.value(value).operand(0);
    if (!value || index.isParameter(value) || index.opcodeOf(value) != Opcode::ResultAt || index.value(value).as<Opcode::ResultAt>().index() != static_cast<uint32_t>(IteratorResumeResult::Value))
        return { };
    ValueID step = index.value(value).as<Opcode::ResultAt>().resultsOperand();
    if (index.isParameter(step) || index.opcodeOf(step) != Opcode::IteratorResume)
        return { };
    auto resumed = index.value(step).as<Opcode::IteratorResume>();
    if (resumed.readsValueWhenDone() || resumed.resumeMode() != ResumeMode::Next || resumed.sentOperand() || (takesApart && resumed.stepIndex() != std::optional<uint32_t>(0)))
        return { };
    ValueID open = index.value(step).operand(0);
    if (index.isParameter(open) || index.opcodeOf(open) != Opcode::IteratorOpen)
        return { };
    auto opened = index.value(open).as<Opcode::IteratorOpen>();
    if (opened.iteratorKind() != IteratorKind::Sync || opened.isDestructuring() != takesApart || opened.isDelegating())
        return { };
    return index.value(open).operand(0);
}

FunctionID LinkFlow::findOnlyReferrer(FunctionID function) const
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    Locker locker { m_lockOfOnlyReferrers };
    if (m_onlyReferrers.isEmpty()) {
        m_onlyReferrers.fill(FunctionID(), program.functions.size());
        BitVector hasSeveralReferrers(program.functions.size());
        for (uint32_t index = 0; index < program.functions.size(); ++index) {
            for (FunctionID named : summaries.at(FunctionID(index)).functionsNamed) {
                if (hasSeveralReferrers.get(named.index()))
                    continue;
                FunctionID known = std::exchange(m_onlyReferrers[named.index()], FunctionID(index));
                if (!known)
                    continue;
                hasSeveralReferrers.set(named.index());
                m_onlyReferrers[named.index()] = { };
            }
        }
    }
    return m_onlyReferrers[function.index()];
}

bool LinkFlow::appendCallbackKeys(FunctionID callback, const BodyIndex& index, ValueID key, Vector<StringID, 8>& keys) const
{
    if (!isPlainCallback(callback, index))
        return false;
    const Function& record = program.at(callback);
    BodyBlock entry = index.block(entryBlock);
    ValueID element = entry.parameter(record.firstParameterIndex());

    auto integerAt = [&](ValueID value) -> std::optional<uint32_t> {
        value = index.stripCasts(value);
        if (index.isParameter(value) || index.opcodeOf(value) != Opcode::Integer)
            return std::nullopt;
        auto integer = index.value(value).as<Opcode::Integer>().integer();
        if (integer < 0 || integer >= std::numeric_limits<int32_t>::max())
            return std::nullopt;
        return static_cast<uint32_t>(integer);
    };
    key = index.stripCasts(key);
    std::optional<uint32_t> position;
    uint32_t length = 0;
    if (key != element) {
        if (index.isParameter(key) || index.opcodeOf(key) != Opcode::ElemGet || index.blockOf(key) != entryBlock)
            return false;
        auto read = index.value(key).as<Opcode::ElemGet>();
        position = integerAt(read.propertyOperand());
        if (!position || index.stripCasts(read.objectOperand()) != element)
            return false;
        length = *position + 1;
        for (BodyValue value : entry.values()) {
            if (value.id() == key)
                break;
            bool onlyReadsRow = true;
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (!operand || index.stripCasts(operand) != element || opcodesThatStateTypes.contains(value.opcode()))
                    return;
                std::optional<uint32_t> other = value.opcode() == Opcode::ElemGet && operandIndex == OperandIndex::ElemGet::object ? integerAt(value.as<Opcode::ElemGet>().propertyOperand()) : std::nullopt;
                onlyReadsRow &= !!other;
                length = std::max(length, other.value_or(0) + 1);
            });
            if (!onlyReadsRow)
                return false;
        }
    }

    FunctionID maker;
    ValueID list = findCallbackList(callback, maker);
    const BodyIndex* made = list ? bodiesLookedAt(looks).find(maker) : nullptr;
    if (!made)
        return false;
    if (made->isParameter(list))
        return !position && made->blockOf(list) == entryBlock && appendArrayParameterStrings(maker, *made, list, keys);
    if (!position && (made->useCount(list) > 2 || variableReadAtForLinkAccesses(*made, list)))
        return appendReadListTexts(maker, *made, list, keys);
    return position ? appendRowStringsForLinkAccesses(program, *made, list, *position, length, keys) : appendTextsOfWrittenListForLinkAccesses(program, *made, list, keys);
}

static FunctionID findCalledFunctionForLinkAccesses(const Program& program, const BodyIndex& index, ValueID callee)
{
    callee = callee ? index.stripCasts(callee) : ValueID();
    if (!callee || index.isParameter(callee))
        return { };
    if (index.opcodeOf(callee) == Opcode::NewClosure)
        return index.value(callee).as<Opcode::NewClosure>().function();
    if (index.opcodeOf(callee) != Opcode::GetModuleVariable)
        return { };
    const Variable& variable = program.at(index.value(callee).as<Opcode::GetModuleVariable>().variable());
    return variable.initialization == VariableInitialization::Function && variable.flags.contains(VariableFlags::IsImmutable) ? variable.function : FunctionID();
}

static bool callsCallbackWithElements(const Program& program, PropertyKeyID key)
{
    if (!key)
        return false;
    const BuiltinTable& builtins = program.builtins();
    BuiltinID method = findOwnMember(program, builtins.canonicalID(Builtins::ArrayPrototype), key);
    if (!method)
        return false;
    if (method == builtins.canonicalID(Builtins::ArrayPrototypeForEach))
        return true;
    const NamedBuiltin others[] { Builtins::ArrayPrototypeMap, Builtins::ArrayPrototypeFilter, Builtins::ArrayPrototypeSome, Builtins::ArrayPrototypeEvery, Builtins::ArrayPrototypeFind, Builtins::ArrayPrototypeFindIndex };
    return std::ranges::any_of(others, [&](BuiltinID other) {
        return method == builtins.canonicalID(other) && !program.isUnlocked(other) && !program.isUnlocked(method);
    });
}

ValueID LinkFlow::findCallbackReadList(FunctionID maker, const BodyIndex& made, ValueID site, ValueID closure) const
{
    auto call = made.value(site).as<Opcode::Call>();
    ValueID callee = call.calleeOperand() ? made.stripCasts(call.calleeOperand()) : ValueID();
    ValueID list = call.thisValueOperand();
    if (!list || made.isParameter(list) || call.argumentsOperands().empty() || call.argumentsOperands()[0] != closure || !callee || made.isParameter(callee) || made.opcodeOf(callee) != Opcode::PropGet)
        return { };
    auto method = made.value(callee).as<Opcode::PropGet>();
    bool goesThroughList = method.objectOperand() == list && callsCallbackWithElements(program, method.key());
    bool hasOtherUses = made.useCount(list) != 2 || variableReadAtForLinkAccesses(made, list);
    return goesThroughList && hasOtherUses && (findReadList(maker, made, list).list || onlyReadsKeys(maker, made, made.stripCasts(list))) ? made.stripCasts(list) : ValueID();
}

ValueID LinkFlow::findCallbackList(FunctionID callback, FunctionID& maker) const
{
    {
        Locker locker { m_lockOfLists };
        if (auto known = m_callbackLists.find(callback.index()); known != m_callbackLists.end()) {
            maker = known->value.first;
            return known->value.second;
        }
    }
    ValueID list = findCallbackListSlow(callback, maker);
    Locker locker { m_lockOfLists };
    m_callbackLists.add(callback.index(), std::pair { maker, list });
    return list;
}

ValueID LinkFlow::findCallbackListSlow(FunctionID callback, FunctionID& maker) const
{
    maker = findOnlyReferrer(callback);
    const BodyIndex* made = maker && maker != callback ? bodiesLookedAt(looks).find(maker) : nullptr;
    if (!made)
        return { };
    ValueID closure;
    for (auto& [function, value] : std::ranges::equal_range(closuresInBody(maker), callback, { }, &std::pair<FunctionID, ValueID>::first)) {
        if (made->opcodeOf(value) != Opcode::NewClosure)
            continue;
        if (closure)
            return { };
        closure = value;
    }
    if (std::ranges::any_of(summaries.at(maker).calls, [&](const SummaryCall& call) { return call.callee == SummaryCall::Callee::Binding && call.function == callback; }))
        return { };
    std::optional<BodyUser> user = closure ? made->onlyUser(closure) : std::nullopt;
    while (user && user->kind == BodyUseKind::Operand && opcodesThatStateTypes.contains(made->opcodeOf(user->value))) {
        closure = user->value;
        user = made->onlyUser(closure);
    }
    if (!user || user->kind != BodyUseKind::Operand || made->opcodeOf(user->value) != Opcode::Call)
        return { };
    auto call = made->value(user->value).as<Opcode::Call>();
    ValueID callee = call.calleeOperand() ? made->stripCasts(call.calleeOperand()) : ValueID();
    std::span<const ValueID> arguments = call.argumentsOperands();
    ValueID list;
    if (FunctionID called = findCalledFunctionForLinkAccesses(program, *made, callee)) {
        std::optional<uint32_t> place = findCallbackParameterArray(called, static_cast<uint32_t>(std::ranges::find(arguments, closure) - arguments.begin()));
        list = place && *place < arguments.size() ? arguments[*place] : ValueID();
        if (!list || !made->hasOneUse(list))
            return { };
    } else {
        if (ValueID readList = findCallbackReadList(maker, *made, user->value, closure))
            return readList;
        if (arguments.empty() || arguments[0] != closure || !callee || made->isParameter(callee) || made->opcodeOf(callee) != Opcode::PropGet)
            return { };
        auto method = made->value(callee).as<Opcode::PropGet>();
        if (!callsCallbackWithElements(program, method.key()))
            return { };
        list = call.thisValueOperand();
        if (!list || method.objectOperand() != list || made->useCount(list) != 2)
            return { };
    }
    while (!made->isParameter(list) && opcodesThatStateTypes.contains(made->opcodeOf(list))) {
        list = made->value(list).operand(0);
        if (!list || !made->hasOneUse(list))
            return { };
    }
    return list;
}

bool LinkFlow::onlyReadsKeys(FunctionID id, const BodyIndex& index, ValueID list) const
{
    if (!list || index.isParameter(list) || index.opcodeOf(list) != Opcode::Call)
        return false;
    uint64_t key = static_cast<uint64_t>(id.index() + 1) << 32 | list.index();
    {
        Locker locker { m_lockOfLists };
        if (auto known = m_readLists.find(key); known != m_readLists.end())
            return !!known->value.list;
    }
    ReadList found = onlyReadsList(id, list, { }) ? ReadList { id, list } : ReadList { };
    Locker locker { m_lockOfLists };
    m_readLists.add(key, found);
    return !!found.list;
}

std::optional<LinkFlow::ListedObject> LinkFlow::findListedObject(FunctionID id, const BodyIndex& index, ValueID key) const
{
    ValueID name = index.stripCasts(key);
    if (!name)
        return std::nullopt;
    if (!index.isParameter(name) && index.opcodeOf(name) == Opcode::ResultAt && index.value(name).as<Opcode::ResultAt>().index() == static_cast<uint32_t>(ForInStepResult::Key)) {
        ValueID step = index.value(name).as<Opcode::ResultAt>().resultsOperand();
        if (!index.isParameter(step) && index.opcodeOf(step) == Opcode::ForInStep) {
            const ThinSummary& summary = summaries.at(id);
            auto listed = std::ranges::find_if(summary.inspections, [&](const ThinSummary::Inspection& inspection) {
                return inspection.site == step && inspection.kind == ThinSummary::Inspection::Kind::ListsKeys;
            });
            if (listed == summary.inspections.end())
                return std::nullopt;
            return ListedObject { id, index.stripCasts(index.value(step).as<Opcode::ForInStep>().objectOperand()), listed->subject, false, true };
        }
    }

    ValueID pair = whatStepGoesThroughForLinkAccesses(index, key, true);
    for (ValueID part = pair; part; part = index.value(part).operand(0)) {
        if (index.isParameter(part))
            break;
        if (!index.hasOneUse(part))
            return std::nullopt;
        if (!opcodesThatStateTypes.contains(index.opcodeOf(part)))
            break;
    }
    FunctionID maker = id;
    const BodyIndex* made = &index;
    ValueID list = whatStepGoesThroughForLinkAccesses(index, pair ? pair : key, false);
    ValueID element = pair ? ValueID() : name;
    if (list) {
        while (!index.isParameter(list) && index.hasOneUse(list) && opcodesThatStateTypes.contains(index.opcodeOf(list)))
            list = index.value(list).operand(0);
        if (!index.isParameter(list) && !index.hasOneUse(list) && !pair && onlyReadsKeys(id, index, index.stripCasts(list)))
            list = index.stripCasts(list);
        else if (index.isParameter(list) || !index.hasOneUse(list))
            return std::nullopt;
    } else if (element && !index.isParameter(element) && index.opcodeOf(element) == Opcode::ElemGet) {
        list = index.stripCasts(index.value(element).as<Opcode::ElemGet>().objectOperand());
        if (!onlyReadsKeys(id, index, list))
            return std::nullopt;
    } else {
        ValueID element = index.stripCasts(pair ? pair : key);
        bool isElement = index.isParameter(element) && index.blockOf(element) == entryBlock && isPlainCallback(id, index) && element == index.block(entryBlock).parameter(program.at(id).firstParameterIndex());
        if (!isElement || (pair && !index.hasOneUse(element)))
            return std::nullopt;
        list = findCallbackList(id, maker);
        made = list ? bodiesLookedAt(looks).find(maker) : nullptr;
        if (!made || made->isParameter(list))
            return std::nullopt;
    }

    BuiltinID lister = program.builtins().canonicalID(pair ? BuiltinID(Builtins::ObjectEntries) : BuiltinID(Builtins::ObjectKeys));
    if (made->opcodeOf(list) != Opcode::Call || program.isUnlocked(lister))
        return std::nullopt;
    auto call = made->value(list).as<Opcode::Call>();
    ValueID callee = call.calleeOperand() ? made->stripCasts(call.calleeOperand()) : ValueID();
    if (!callee || made->isParameter(callee) || made->opcodeOf(callee) != Opcode::Builtin || program.builtins().canonicalID(made->value(callee).as<Opcode::Builtin>().builtin()) != lister)
        return std::nullopt;
    const ThinSummary& summary = summaries.at(maker);
    auto listed = std::ranges::find_if(summary.calls, [&](const SummaryCall& made) {
        return made.site == list && made.callee == SummaryCall::Callee::Builtin && !made.isConstruction && !made.passesList && !made.isMadeByProtocol && made.numberOfArguments;
    });
    uint32_t node = listed == summary.calls.end() ? noSummaryNode : summary.arguments[listed->firstArgument];
    if (node == noSummaryNode || call.argumentsOperands().empty() || !call.argumentsOperands()[0])
        return std::nullopt;
    return ListedObject { maker, made->stripCasts(call.argumentsOperands()[0]), node, !!pair };
}

bool LinkFlow::storesUnderListedNames(FunctionID id, ValueID site) const
{
    const BodyIndex* index = site ? bodiesLookedAt(looks).find(id) : nullptr;
    return index && !index->isParameter(site) && index->opcodeOf(site) == Opcode::ElemSet && findListedObject(id, *index, index->value(site).as<Opcode::ElemSet>().propertyOperand());
}

bool LinkFlow::isPlainCallback(FunctionID callback, const BodyIndex& index) const
{
    const Function& record = program.at(callback);
    bool isPlain = (record.kind == FunctionKind::Plain || record.kind == FunctionKind::Arrow) && !record.isCoroutine() && !record.calleeParameterIndex();
    if (!isPlain || !record.numberOfPositionalParameters() || record.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest }) || summaries.at(callback).has(SummaryTrait::UsesArguments))
        return false;
    return record.numberOfPositionalParameters() < 3 || !index.useCount(index.block(entryBlock).parameter(record.firstParameterIndex() + 2));
}

LinkFlow::ComputedStoreNames LinkFlow::findComputedStoreNames(FunctionID id, ValueID site) const
{
    ComputedStoreNames found;
    const BodyIndex* index = site ? bodiesLookedAt(looks).find(id) : nullptr;
    if (!index || index->isParameter(site) || index->opcodeOf(site) != Opcode::ElemSet)
        return found;
    ValueID iterated = whatStepGoesThroughForLinkAccesses(*index, index->value(site).as<Opcode::ElemSet>().propertyOperand(), false);
    iterated = iterated ? index->stripCasts(iterated) : ValueID();
    Vector<StringID, 8> texts;
    if (iterated && index->isParameter(iterated) && index->blockOf(iterated) == entryBlock) {
        if (!index->hasOneUse(iterated) || !appendArrayParameterStrings(id, *index, iterated, texts))
            return found;
    } else if (!iterated || index->isParameter(iterated)) {
        ValueID key = index->value(site).as<Opcode::ElemSet>().propertyOperand();
        bool isPassed = appendTextsOfParameter(id, *index, key, texts);
        if (!isPassed)
            texts.shrink(0);
        if (!isPassed && !appendCallbackKeys(id, *index, key, texts))
            return found;
    } else if (index->opcodeOf(iterated) == Opcode::Call) {
        ValueID callee = index->stripCasts(index->value(iterated).as<Opcode::Call>().calleeOperand());
        BuiltinID called = callee && !index->isParameter(callee) && index->opcodeOf(callee) == Opcode::Builtin ? program.builtins().canonicalID(index->value(callee).as<Opcode::Builtin>().builtin()) : BuiltinID();
        found.areAllOwnNames = called == BuiltinID(Builtins::ObjectGetOwnPropertyNames) || called == BuiltinID(Builtins::ReflectOwnKeys);
        return found;
    } else if (!appendTextsOfWrittenListForLinkAccesses(program, *index, iterated, texts))
        return found;
    for (StringID text : texts) {
        std::optional<PropertyKeyID> name = program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = text });
        if (name && !found.names.contains(*name))
            found.names.append(*name);
    }
    return found;
}

bool AccessInLinkAccesses::storesCopiedNamesOnPrototype(const LinkType& subject, const LinkType& source)
{
    if (m_access.kind != Kind::Store || subject.objects != LinkType::Objects::Prototype || !(subject == prototypeForLinkAccesses(subject.layout)) || !m_flow.followsNamesOnPrototypeOf(subject.layout))
        return false;
    if (m_program.admitsReplacedBuiltins || (!source.isNever() && !m_flow.holdsOnlyPlainRecords(source)))
        return false;
    Vector<LinkFlow::OwnValue, 16> stored;
    bool namesAreListed = true;
    m_flow.forEachLayoutOf(source, [&](LayoutID layout) {
        namesAreListed = namesAreListed && !m_flow.ownValuesOf(layout).empty();
        for (const LinkFlow::OwnValue& value : m_flow.ownValuesOf(layout)) {
            if (value.key) {
                namesAreListed = namesAreListed && m_flow.nameClassOf(value.key) != NameClass::Proto && !m_program.isIndexLike(value.key) && !mayBeElementKey(m_flow.nameClassOf(value.key));
                stored.append(value);
                continue;
            }
            tellAgainOnceClimbs(value.node);
            namesAreListed = namesAreListed && m_flow.typeAt(value.node).isNever();
        }
    });
    if (!namesAreListed)
        return false;
    mayThrow();
    for (const LinkFlow::OwnValue& value : stored) {
        AccessInLinkAccesses underName = *this;
        underName.goByKey(value.key);
        underName.m_access.isCopySpelledByLoop = false;
        underName.m_value = value.node;
        underName.m_keyIsWritten = true;
        underName.m_keyIsOnlyNumbers = false;
        underName.m_keyCannotNameBuiltin = false;
        underName.m_keyIsSymbolAsFarAsIsSeen = false;
        underName.m_seenKeysAreNumbers = false;
        underName.m_keyMayNamePrototypeLink = false;
        underName.m_before = { };
        underName.resolve();
    }
    return true;
}

bool AccessInLinkAccesses::readsOrStoresUnderTextsThatBodyWrites()
{
    bool stores = m_access.kind == Kind::Store;
    if ((!isRead() && !stores) || m_shared || m_program.admitsReplacedBuiltins || m_flow.testRuns().isTestRun())
        return false;
    for (BuiltinID row : { Builtins::ArrayPrototypeValues, Builtins::ArrayIteratorPrototypeNext }) {
        if (m_program.isUnlocked(row) || m_program.isUnlocked(m_table.canonicalID(row)))
            return false;
    }
    const BodyIndex* index = bodiesLookedAt(m_flow.looks).find(m_function);
    if (!index || index->isParameter(m_access.site) || index->opcodeOf(m_access.site) != (stores ? Opcode::ElemSet : Opcode::ElemGet))
        return false;
    ValueID key = stores ? index->value(m_access.site).as<Opcode::ElemSet>().propertyOperand() : index->value(m_access.site).as<Opcode::ElemGet>().propertyOperand();
    Vector<StringID, 8> texts;
    return m_flow.appendTextsOfKey(m_function, *index, key, texts) && resolveUnderTexts(texts.span());
}

const LinkFlow::CallsInSight& LinkFlow::findCallsInSight(FunctionID callee) const
{
    {
        Locker locker { m_lockOfCallsInSight };
        if (auto known = m_callsInSight.find(callee.index()); known != m_callsInSight.end())
            return *known->value;
    }
    std::unique_ptr<CallsInSight> made = listCallsInSight(callee);
    Locker locker { m_lockOfCallsInSight };
    auto added = m_callsInSight.add(callee.index(), nullptr);
    if (added.isNewEntry)
        added.iterator->value = WTF::move(made);
    return *added.iterator->value;
}

const LinkFlow::ClosuresInBody& LinkFlow::closuresInBody(FunctionID maker) const
{
    {
        Locker locker { m_lockOfCallsInSight };
        if (auto known = m_closuresInBodies.find(maker.index()); known != m_closuresInBodies.end())
            return *known->value;
    }
    std::unique_ptr<ClosuresInBody> made = listClosuresInBody(maker);
    Locker locker { m_lockOfCallsInSight };
    auto added = m_closuresInBodies.add(maker.index(), nullptr);
    if (added.isNewEntry)
        added.iterator->value = WTF::move(made);
    return *added.iterator->value;
}

std::unique_ptr<LinkFlow::ClosuresInBody> LinkFlow::listClosuresInBody(FunctionID maker) const
{
    auto closures = makeUniqueWithoutFastMallocCheck<ClosuresInBody>();
    const BodyIndex& index = *bodiesLookedAt(looks).find(maker);
    if (!index.holdsAnyOf({ Opcode::NewClosure, Opcode::GetModuleVariable }))
        return closures;
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        for (BodyValue value : index.block(BlockID(number)).values()) {
            FunctionID function;
            if (value.opcode() == Opcode::NewClosure)
                function = value.as<Opcode::NewClosure>().function();
            else if (value.opcode() == Opcode::GetModuleVariable)
                function = functionKeptBy(value.as<Opcode::GetModuleVariable>().variable());
            if (function)
                closures->append({ function, value.id() });
        }
    }
    std::ranges::sort(*closures);
    closures->shrinkToFit();
    return closures;
}

bool LinkFlow::isExportedVariable(VariableID id) const
{
    std::call_once(m_exportedVariablesAreListed, [&] {
        m_exportedVariables.ensureSize(program.variables.size());
        for (size_t number = 0; number < program.modules.size(); ++number) {
            const Module& module = program.modules[number];
            auto note = [&](VariableID exported) {
                if (exported && program.at(exported).module == ModuleID(static_cast<uint32_t>(number)))
                    m_exportedVariables.quickSet(exported.index());
            };
            note(module.exportsVariable);
            for (const ModuleExport& exported : module.exports)
                note(exported.variable);
            for (const SyntheticExport& exported : module.syntheticExports)
                note(exported.variable);
        }
    });
    return m_exportedVariables.quickGet(id.index());
}

FunctionID LinkFlow::functionKeptBy(VariableID id) const
{
    const Variable& variable = program.at(id);
    bool keepsFunction = variable.initialization == VariableInitialization::Function && variable.flags.contains(VariableFlags::IsImmutable) && variable.module && !isExportedVariable(id);
    return keepsFunction ? variable.function : FunctionID();
}

LinkFlow::SitesFound LinkFlow::appendOnlySites(const BodyIndex& index, FunctionID callee, ValueID closure, Vector<ValueID, 8>& sites) const
{
    if (!index.isReachable(index.blockOf(closure)))
        return SitesFound::All;
    while (std::optional<BodyUser> user = index.firstUser(closure)) {
        if (user->kind == BodyUseKind::TerminatorOperand || user->kind == BodyUseKind::EdgeArgument || user->kind == BodyUseKind::HandlerArgument)
            return index.isReachable(user->block) ? SitesFound::None : SitesFound::MustBeSought;
        if (user->kind != BodyUseKind::Operand || !index.isReachable(index.blockOf(user->value)))
            return SitesFound::MustBeSought;
        BodyValue value = index.value(user->value);
        bool passesThrough = inputPassThroughOpcodes.contains(value.opcode());
        bool calls = value.opcode() == Opcode::Call && value.as<Opcode::Call>().calleeOperand() == closure;
        bool isKept = value.opcode() == Opcode::PutModuleVariable && functionKeptBy(value.as<Opcode::PutModuleVariable>().variable()) == callee;
        if (!passesThrough && !calls && !isKept)
            return SitesFound::None;
        if (!index.hasOneUse(closure))
            return SitesFound::MustBeSought;
        if (calls)
            sites.append(user->value);
        if (!passesThrough || value.operand(0) != closure)
            return SitesFound::All;
        closure = user->value;
    }
    return SitesFound::All;
}

const LinkFlow::CallsInBody& LinkFlow::callsInBody(FunctionID caller) const
{
    {
        Locker locker { m_lockOfCallsInSight };
        if (auto known = m_callsInBodies.find(caller.index()); known != m_callsInBodies.end())
            return *known->value;
    }
    std::unique_ptr<CallsInBody> made = listCallsInBody(caller);
    Locker locker { m_lockOfCallsInSight };
    auto added = m_callsInBodies.add(caller.index(), nullptr);
    if (added.isNewEntry)
        added.iterator->value = WTF::move(made);
    return *added.iterator->value;
}

std::unique_ptr<LinkFlow::CallsInBody> LinkFlow::listCallsInBody(FunctionID caller) const
{
    static constexpr size_t mostCalls = 64;
    const BodyIndex& index = *bodiesLookedAt(looks).find(caller);
    auto found = makeUniqueWithoutFastMallocCheck<CallsInBody>();
    auto& sites = found->sitesOfOnlyCalled;
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> usedOtherwise;
    Vector<FunctionID> closures;
    auto closureAt = [&](ValueID value) { return value && !closures.isEmpty() ? closures[value.index()] : FunctionID(); };
    auto noteOtherUse = [&](ValueID operand) {
        if (FunctionID used = closureAt(operand))
            usedOtherwise.add(used.index());
    };
    for (BlockID block : index.blocksInDominatorOrder()) {
        for (BodyValue value : index.block(block).values()) {
            Opcode opcode = value.opcode();
            bool passesThrough = inputPassThroughOpcodes.contains(opcode);
            FunctionID named;
            if (opcode == Opcode::NewClosure)
                named = value.as<Opcode::NewClosure>().function();
            else if (opcode == Opcode::GetModuleVariable)
                named = functionKeptBy(value.as<Opcode::GetModuleVariable>().variable());
            else if (passesThrough)
                named = closureAt(value.operand(0));
            if (named) {
                if (closures.isEmpty())
                    closures.fill(FunctionID(), index.numberOfValues());
                closures[value.id().index()] = named;
            }
            FunctionID called = opcode == Opcode::Call ? closureAt(value.as<Opcode::Call>().calleeOperand()) : FunctionID();
            if (called)
                sites.add(called.index(), Vector<ValueID>()).iterator->value.append(value.id());
            if (passesThrough || closures.isEmpty())
                continue;
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                FunctionID used = closureAt(operand);
                if (!used)
                    return;
                bool isKept = opcode == Opcode::PutModuleVariable && functionKeptBy(value.as<Opcode::PutModuleVariable>().variable()) == used;
                if (!isKept && !(called == used && operandIndex == OperandIndex::Call::callee))
                    usedOtherwise.add(used.index());
            });
        }
        if (closures.isEmpty())
            continue;
        index.terminator(block).forEachOperand([&](ValueID operand, uint32_t) {
            noteOtherUse(operand);
        });
        for (BodyTarget target : index.terminator(block).targets()) {
            for (ValueID argument : target.arguments())
                noteOtherUse(argument);
        }
        if (index.block(block).hasHandler()) {
            for (ValueID argument : index.block(block).handler().arguments())
                noteOtherUse(argument);
        }
    }
    sites.removeIf([&](auto& entry) {
        return usedOtherwise.contains(entry.key) || entry.value.size() > mostCalls;
    });
    return found;
}

std::unique_ptr<LinkFlow::CallsInSight> LinkFlow::listCallsInSight(FunctionID callee) const
{
    auto found = makeUniqueWithoutFastMallocCheck<CallsInSight>();
    const Function& record = program.at(callee);
    bool isPlain = (record.kind == FunctionKind::Plain || record.kind == FunctionKind::Arrow) && !record.isCoroutine() && !record.calleeParameterIndex() && !record.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest });
    FunctionID caller = isPlain && !summaries.at(callee).has(SummaryTrait::UsesArguments) ? findOnlyReferrer(callee) : FunctionID();
    const BodyIndex* index = caller && caller != callee ? bodiesLookedAt(looks).find(caller) : nullptr;
    if (!index || summaries.at(caller).has(SummaryTrait::HasDirectEval))
        return found;
    static constexpr size_t mostCalls = 64;
    Vector<ValueID, 8> only;
    std::span<const ValueID> sites;
    for (auto& [function, closure] : std::ranges::equal_range(closuresInBody(caller), callee, { }, &std::pair<FunctionID, ValueID>::first)) {
        SitesFound ofClosure = appendOnlySites(*index, callee, closure, only);
        if (ofClosure == SitesFound::None)
            return found;
        if (ofClosure == SitesFound::All)
            continue;
        const auto& calls = callsInBody(caller).sitesOfOnlyCalled;
        auto sought = calls.find(callee.index());
        if (sought == calls.end())
            return found;
        sites = sought->value.span();
        break;
    }
    if (sites.empty()) {
        if (only.isEmpty() || only.size() > mostCalls)
            return found;
        std::ranges::sort(only, { }, [&](ValueID site) { return std::pair { index->dominatorNumber(index->blockOf(site)), site.index() }; });
        sites = only.span();
    }
    const ThinSummary& summary = summaries.at(caller);
    for (ValueID site : sites) {
        auto call = std::ranges::find(summary.calls, site, &SummaryCall::site);
        if (call == summary.calls.end() || call->isConstruction || call->passesList || call->isMadeByProtocol || (call->callee != SummaryCall::Callee::Value && call->callee != SummaryCall::Callee::Binding)) {
            found->calls.clear();
            return found;
        }
        found->calls.append({ site, static_cast<uint32_t>(&*call - &*summary.calls.begin()) });
    }
    found->caller = caller;
    return found;
}

std::optional<uint32_t> LinkFlow::findPassedPosition(FunctionID id, const BodyIndex& index, ValueID value) const
{
    value = value ? index.stripCasts(value) : ValueID();
    if (!value || !index.isParameter(value) || index.blockOf(value) != entryBlock)
        return std::nullopt;
    const Function& record = program.at(id);
    for (uint32_t position = 0; position < record.numberOfPositionalParameters(); ++position) {
        if (index.block(entryBlock).parameter(record.firstParameterIndex() + position) == value)
            return position;
    }
    return std::nullopt;
}

StringID LinkFlow::findPassedText(const CallsInSight& found, size_t call, uint32_t position) const
{
    const BodyIndex* index = bodiesLookedAt(looks).find(found.caller);
    std::span<const ValueID> arguments = index->value(found.calls[call].first).as<Opcode::Call>().argumentsOperands();
    ValueID passed = position < arguments.size() && arguments[position] ? index->stripCasts(arguments[position]) : ValueID();
    return passed && !index->isParameter(passed) && index->opcodeOf(passed) == Opcode::String ? index->value(passed).as<Opcode::String>().string() : StringID();
}

FlowNodeID LinkFlow::findPassedNode(FunctionID callee, const CallsInSight& found, size_t call, uint32_t position) const
{
    const ThinSummary& summary = summaries.at(found.caller);
    const SummaryCall& made = summary.calls[found.calls[call].second];
    uint32_t place = made.callee == SummaryCall::Callee::Binding ? program.at(callee).firstParameterIndex() + position : position;
    uint32_t node = place < made.numberOfArguments ? summary.arguments[made.firstArgument + place] : noSummaryNode;
    return node == noSummaryNode ? FlowNodeID() : nodeOf(found.caller, node);
}

bool LinkFlow::appendTextsOfParameter(FunctionID id, const BodyIndex& index, ValueID key, Vector<StringID, 8>& texts) const
{
    std::optional<uint32_t> position = findPassedPosition(id, index, key);
    if (!position)
        return false;
    const CallsInSight& found = findCallsInSight(id);
    for (size_t call = 0; call < found.calls.size(); ++call) {
        StringID text = findPassedText(found, call, *position);
        if (!text)
            return false;
        if (!texts.contains(text))
            texts.append(text);
    }
    return !found.calls.isEmpty();
}

bool AccessInLinkAccesses::accessesPassedNames()
{
    bool stores = m_access.kind == Kind::Store;
    if ((!stores && !isRead()) || m_shared || m_receiver != m_subject || m_program.admitsReplacedBuiltins || m_flow.testRuns().isTestRun())
        return false;
    const BodyIndex* index = bodiesLookedAt(m_flow.looks).find(m_function);
    if (!index || index->isParameter(m_access.site) || index->opcodeOf(m_access.site) != (stores ? Opcode::ElemSet : Opcode::ElemGet))
        return false;
    ValueID key = stores ? index->value(m_access.site).as<Opcode::ElemSet>().propertyOperand() : index->value(m_access.site).as<Opcode::ElemGet>().propertyOperand();
    std::optional<uint32_t> position = m_flow.findPassedPosition(m_function, *index, key);
    Vector<StringID, 8> texts;
    Vector<PropertyKeyID, 8> names;
    if (!position || !m_flow.appendTextsOfParameter(m_function, *index, key, texts) || !m_flow.appendNamesOfTexts(texts.span(), names))
        return false;
    std::optional<uint32_t> subjectPosition = m_flow.findPassedPosition(m_function, *index, stores ? index->value(m_access.site).as<Opcode::ElemSet>().objectOperand() : index->value(m_access.site).as<Opcode::ElemGet>().objectOperand());
    std::optional<uint32_t> valuePosition = stores && m_value ? m_flow.findPassedPosition(m_function, *index, index->value(m_access.site).as<Opcode::ElemSet>().valueOperand()) : std::nullopt;
    const LinkFlow::CallsInSight& found = m_flow.findCallsInSight(m_function);
    struct Passed {
        PropertyKeyID name;
        FlowNodeID subject;
        FlowNodeID value;
    };
    Vector<Passed, 8> passed;
    for (size_t call = 0; call < found.calls.size(); ++call) {
        PropertyKey key { .kind = PropertyKeyKind::Text, .text = m_flow.findPassedText(found, call, *position) };
        Passed each { *m_program.types.find(key), m_subject, m_value };
        if (subjectPosition)
            each.subject = m_flow.findPassedNode(m_function, found, call, *subjectPosition);
        if (valuePosition)
            each.value = m_flow.findPassedNode(m_function, found, call, *valuePosition);
        if (!each.subject || (m_value && !each.value))
            return false;
        passed.append(each);
    }
    for (const Passed& each : passed) {
        AccessInLinkAccesses ofCall = *this;
        ofCall.m_subject = each.subject;
        ofCall.m_receiver = each.subject;
        ofCall.m_value = each.value;
        if (each.subject != m_subject)
            tellAgainOnceClimbsWhoeverTells(each.subject);
        ofCall.resolveUnderNames(std::span<const PropertyKeyID>(&each.name, 1));
    }
    m_flow.build.statistics().count(StatisticID::AccessesUnderPassedNames);
    return true;
}

bool LinkFlow::appendArrayParameterStrings(FunctionID id, const BodyIndex& index, ValueID list, Vector<StringID, 8>& texts) const
{
    const Function& record = program.at(id);
    bool isPlain = (record.kind == FunctionKind::Plain || record.kind == FunctionKind::Arrow) && !record.isCoroutine() && !record.calleeParameterIndex() && !record.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest });
    if (!isPlain || summaries.at(id).has(SummaryTrait::UsesArguments))
        return false;
    std::optional<uint32_t> place;
    for (uint32_t position = 0; position < record.numberOfPositionalParameters(); ++position) {
        if (index.block(entryBlock).parameter(record.firstParameterIndex() + position) == list)
            place = position;
    }
    FunctionID maker = place ? findOnlyReferrer(id) : FunctionID();
    const BodyIndex* made = maker && maker != id ? bodiesLookedAt(looks).find(maker) : nullptr;
    if (!made)
        return false;
    ValueID closure;
    for (uint32_t number = 0; number < made->numberOfBlocks(); ++number) {
        for (BodyValue value : made->block(BlockID(number)).values()) {
            if (value.opcode() != Opcode::NewClosure || value.as<Opcode::NewClosure>().function() != id)
                continue;
            if (closure)
                return false;
            closure = value.id();
        }
    }
    for (std::optional<BodyUser> user = closure ? made->onlyUser(closure) : std::nullopt; user && user->kind == BodyUseKind::Operand && opcodesThatStateTypes.contains(made->opcodeOf(user->value)); user = made->onlyUser(closure))
        closure = user->value;
    uint32_t numberOfCalls = 0;
    for (uint32_t number = 0; closure && number < made->numberOfBlocks(); ++number) {
        for (BodyValue value : made->block(BlockID(number)).values()) {
            if (value.opcode() != Opcode::Call || value.as<Opcode::Call>().calleeOperand() != closure)
                continue;
            std::span<const ValueID> arguments = value.as<Opcode::Call>().argumentsOperands();
            ValueID passed = *place < arguments.size() ? arguments[*place] : ValueID();
            while (passed && !made->isParameter(passed) && made->hasOneUse(passed) && opcodesThatStateTypes.contains(made->opcodeOf(passed)))
                passed = made->value(passed).operand(0);
            if (!passed || made->isParameter(passed) || !made->hasOneUse(passed) || !appendTextsOfWrittenListForLinkAccesses(program, *made, passed, texts))
                return false;
            ++numberOfCalls;
        }
    }
    return numberOfCalls && numberOfCalls < BodyIndex::maxCountedUses && numberOfCalls == made->useCount(closure);
}

bool LinkFlow::readsUnderIndex(FunctionID id, BodyValue element) const
{
    bool isSurelyIndex = std::ranges::any_of(summaries.at(id).accesses, [&](const SummaryAccess& access) {
        return access.site == element.id() && access.kind == SummaryAccess::Kind::Read && access.keyIsSurelyIndex;
    });
    if (isSurelyIndex)
        return true;
    const BodyIndex* index = bodiesLookedAt(looks).find(id);
    return index && readsElementInBoundsForLinkAccesses(program, *index, index->value(element.id()), element.as<Opcode::ElemGet>().objectOperand());
}

bool LinkFlow::onlyReadsList(FunctionID id, ValueID list, VariableID variable) const
{
    ASSERT(list || variable);
    const Body& body = program.at(id).body;
    BitVector lists(body.numberOfValues());
    BitVector methods(body.numberOfValues());
    if (list)
        lists.quickSet(list.index());
    auto isIn = [](const BitVector& values, ValueID value) {
        return value && values.quickGet(value.index());
    };
    Vector<BodyValue> definitions(FillWith { }, body.numberOfValues(), BodyValue());
    body.forEachBlock([&](const BodyBlock& block) {
        for (BodyValue value : block.values())
            definitions[value.id().index()] = value;
    });
    bool isOnlyRead = true;
    bool hasNewNames = true;
    while (hasNewNames) {
        hasNewNames = false;
        body.forEachBlock([&](const BodyBlock& block) {
            for (BodyValue value : block.values()) {
                Opcode opcode = value.opcode();
                bool takesCallback = false;
                BitVector* names = nullptr;
                if (opcode == Opcode::GetModuleVariable && variable && value.as<Opcode::GetModuleVariable>().variable() == variable)
                    names = &lists;
                else if (inputPassThroughOpcodes.contains(opcode) && isIn(lists, value.operand(0)))
                    names = &lists;
                else if (inputPassThroughOpcodes.contains(opcode) && isIn(methods, value.operand(0)))
                    names = &methods;
                else if (opcode == Opcode::PropGet && isIn(lists, value.as<Opcode::PropGet>().objectOperand()) && onlyReadsItsListForLinkAccesses(program, value.key(), takesCallback))
                    names = &methods;
                if (names && !names->quickSet(value.id().index()))
                    hasNewNames = true;
            }
        });
    }
    body.forEachBlock([&](const BodyBlock& block) {
        for (BodyValue value : block.values()) {
            Opcode opcode = value.opcode();
            if (inputPassThroughOpcodes.contains(opcode))
                continue;
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (isIn(methods, operand)) {
                    bool calls = opcode == Opcode::Call && value.as<Opcode::Call>().calleeOperand() == operand && isIn(lists, value.as<Opcode::Call>().thisValueOperand()) && operandIndex == OperandIndex::Call::callee;
                    isOnlyRead &= calls;
                } else if (isIn(lists, operand)) {
                    switch (opcode) {
                    case Opcode::ElemGet:
                        isOnlyRead &= operandIndex == OperandIndex::ElemGet::object && readsUnderIndex(id, value);
                        break;
                    case Opcode::PropGet:
                        isOnlyRead &= operandIndex == OperandIndex::PropGet::object && (value.key() == program.keyOf(BuiltinNames::length) || methods.quickGet(value.id().index()));
                        break;
                    case Opcode::IteratorOpen: {
                        auto opened = value.as<Opcode::IteratorOpen>();
                        isOnlyRead &= !operandIndex && opened.iteratorKind() == IteratorKind::Sync && !opened.isDelegating();
                        break;
                    }
                    case Opcode::PutModuleVariable:
                        isOnlyRead &= variable && value.as<Opcode::PutModuleVariable>().variable() == variable;
                        break;
                    case Opcode::Call:
                        isOnlyRead &= operandIndex == OperandIndex::Call::thisValue && isIn(methods, value.as<Opcode::Call>().calleeOperand());
                        break;
                    default:
                        isOnlyRead = false;
                    }
                }
            });
            if (opcode != Opcode::Call || !isIn(methods, value.as<Opcode::Call>().calleeOperand()))
                continue;
            BodyValue method = definitions[value.as<Opcode::Call>().calleeOperand().index()];
            while (inputPassThroughOpcodes.contains(method.opcode()))
                method = definitions[method.operand(0).index()];
            bool takesCallback = false;
            onlyReadsItsListForLinkAccesses(program, method.key(), takesCallback);
            if (!takesCallback)
                continue;
            std::span<const ValueID> arguments = value.as<Opcode::Call>().argumentsOperands();
            BodyValue callback = arguments.empty() || !arguments[0] ? BodyValue() : definitions[arguments[0].index()];
            while (callback && inputPassThroughOpcodes.contains(callback.opcode()))
                callback = callback.operand(0) ? definitions[callback.operand(0).index()] : BodyValue();
            if (!callback || callback.opcode() != Opcode::NewClosure) {
                isOnlyRead = false;
                continue;
            }
            FunctionID called = callback.as<Opcode::NewClosure>().function();
            const Function& record = program.at(called);
            isOnlyRead &= record.numberOfPositionalParameters() < 3 && !record.calleeParameterIndex() && !record.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest }) && !summaries.at(called).has(SummaryTrait::UsesArguments) && !summaries.at(called).has(SummaryTrait::HasDirectEval);
        }
        BodyTerminator end = block.terminator();
        end.forEachOperand([&](ValueID operand, uint32_t) {
            isOnlyRead &= !isIn(lists, operand) && !isIn(methods, operand);
        });
        block.forEachOutgoingEdge([&](BodyTarget target, bool) {
            for (ValueID argument : target.arguments())
                isOnlyRead &= !isIn(lists, argument) && !isIn(methods, argument);
        });
    });
    return isOnlyRead;
}

bool LinkFlow::isListVariableOnlyRead(VariableID id) const
{
    const Variable& variable = program.at(id);
    ModuleID moduleID = variable.module;
    bool isOnlyRead = variable.flags.contains(VariableFlags::IsImmutable) && moduleID;
    if (isOnlyRead) {
        const Module& module = program.at(moduleID);
        isOnlyRead = module.exportsVariable != id && std::ranges::none_of(module.exports, [&](const ModuleExport& exported) { return exported.variable == id; }) && std::ranges::none_of(module.syntheticExports, [&](const SyntheticExport& exported) { return exported.variable == id; });
    }
    if (!isOnlyRead)
        return false;
    if (m_modulesWithDirectEval.at(moduleID.index()))
        return false;
    uint32_t first = m_firstNamerOfVariable[id.index()];
    return std::ranges::all_of(m_namersOfVariables.span().subspan(first, m_firstNamerOfVariable[id.index() + 1] - first), [&](FunctionID function) {
        return function == program.at(moduleID).body || onlyReadsList(function, { }, id);
    });
}

void LinkFlow::listNamersOfVariables()
{
    struct Naming {
        uint32_t variable;
        FunctionID namer;
    };
    m_modulesWithDirectEval = BytesSharedByThreads(program.modules.size());
    Vector<Naming> namings = collectInParallel<Naming>(build.parallelLoops(), static_cast<unsigned>(program.functions.size()), [&](unsigned index, Vector<Naming>& namings) {
        const Function& function = program.functions[index];
        if (!function.module || !function.hasBody())
            return;
        const ThinSummary& summary = summaries.at(FunctionID(index));
        if (summary.has(SummaryTrait::HasDirectEval))
            m_modulesWithDirectEval.set(function.module.index(), 1);
        if (!summary.holdsNodesOf(SummaryNode::Kind::Variable))
            return;
        for (const SummaryNode& node : summary.nodes) {
            if (node.kind != SummaryNode::Kind::Variable || node.first >= program.variables.size())
                continue;
            const Variable& variable = program.variables[node.first];
            if (variable.module == function.module && variable.flags.contains(VariableFlags::IsImmutable))
                namings.append({ node.first, FunctionID(index) });
        }
    });
    m_firstNamerOfVariable.fill(0, program.variables.size() + 1);
    for (const Naming& naming : namings)
        ++m_firstNamerOfVariable[naming.variable + 1];
    for (size_t variable = 0; variable < program.variables.size(); ++variable)
        m_firstNamerOfVariable[variable + 1] += m_firstNamerOfVariable[variable];
    m_namersOfVariables.grow(namings.size());
    Vector<uint32_t> next(m_firstNamerOfVariable);
    for (const Naming& naming : namings)
        m_namersOfVariables[next[naming.variable]++] = naming.namer;
}

LinkFlow::ReadList LinkFlow::findReadList(FunctionID id, const BodyIndex& index, ValueID list) const
{
    if (!list || index.isParameter(index.stripCasts(list)))
        return { };
    VariableID variable = variableReadAtForLinkAccesses(index, list);
    list = index.stripCasts(list);
    if (!variable && index.opcodeOf(list) != Opcode::NewArray && index.opcodeOf(list) != Opcode::Data)
        return { };
    uint64_t key = variable ? static_cast<uint64_t>(variable.index()) : static_cast<uint64_t>(id.index() + 1) << 32 | list.index();
    {
        Locker locker { m_lockOfLists };
        if (auto known = m_readLists.find(key); known != m_readLists.end())
            return known->value;
    }
    ReadList found = findReadListSlow(id, index, variable, list);
    Locker locker { m_lockOfLists };
    m_readLists.add(key, found);
    return found;
}

LinkFlow::ReadList LinkFlow::findReadListSlow(FunctionID id, const BodyIndex& index, VariableID variable, ValueID list) const
{
    FunctionID owner = id;
    const BodyIndex* made = &index;
    if (variable) {
        ModuleID module = program.at(variable).module;
        owner = module && program.at(variable).flags.contains(VariableFlags::IsImmutable) ? program.at(module).body : FunctionID();
        made = owner ? bodiesLookedAt(looks).find(owner) : nullptr;
        list = { };
    }
    for (uint32_t number = 0; made && number < made->numberOfBlocks(); ++number) {
        for (BodyValue value : made->block(BlockID(number)).values()) {
            if (value.opcode() != Opcode::PutModuleVariable)
                continue;
            auto put = value.as<Opcode::PutModuleVariable>();
            ValueID stored = put.valueOperand() ? made->stripCasts(put.valueOperand()) : ValueID();
            if (variable && !list && put.variable() == variable)
                list = stored;
            else if (variable ? put.variable() == variable : stored == list) {
                if (variable)
                    return { };
                variable = put.variable();
            }
        }
    }
    if (!made || !list || made->isParameter(list) || (made->opcodeOf(list) != Opcode::NewArray && made->opcodeOf(list) != Opcode::Data) || summaries.at(owner).has(SummaryTrait::HasDirectEval))
        return { };
    Vector<StringID, 8> texts;
    if (!appendTextsOfWrittenListForLinkAccesses(program, *made, list, texts) || !onlyReadsList(owner, list, variable))
        return { };
    if (variable && (owner != program.at(program.at(variable).module).body || !isListVariableOnlyRead(variable)))
        return { };
    return { owner, list };
}

bool LinkFlow::appendReadListTexts(FunctionID id, const BodyIndex& index, ValueID list, Vector<StringID, 8>& texts) const
{
    ReadList found = findReadList(id, index, list);
    const BodyIndex* made = found.list ? bodiesLookedAt(looks).find(found.owner) : nullptr;
    return made && appendTextsOfWrittenListForLinkAccesses(program, *made, found.list, texts);
}

bool LinkFlow::appendTextsOfElement(FunctionID id, const BodyIndex& index, BodyValue element, Vector<StringID, 8>& texts) const
{
    ValueID list = element.as<Opcode::ElemGet>().objectOperand();
    ValueID made = list ? index.stripCasts(list) : ValueID();
    bool mayBeWritten = made && (index.isParameter(made) ? index.blockOf(made) == entryBlock && findOnlyReferrer(id) : index.opcodeOf(made) == Opcode::NewArray || index.opcodeOf(made) == Opcode::Data || variableReadAtForLinkAccesses(index, made));
    if (!mayBeWritten || !readsElementInBoundsForLinkAccesses(program, index, element, list))
        return false;
    if (!index.isParameter(made))
        return appendReadListTexts(id, index, list, texts);
    uint32_t numberOfReads = 0;
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        for (BodyValue value : index.block(BlockID(number)).values()) {
            numberOfReads += value.opcode() == Opcode::ElemGet && value.as<Opcode::ElemGet>().objectOperand() == list && readsUnderIndex(id, value);
            numberOfReads += value.opcode() == Opcode::PropGet && value.as<Opcode::PropGet>().objectOperand() == list && value.key() == program.keyOf(BuiltinNames::length);
        }
    }
    return list == made && numberOfReads < BodyIndex::maxCountedUses && numberOfReads == index.useCount(list) && appendArrayParameterStrings(id, index, list, texts);
}

static constexpr unsigned mostChosenTexts = 8;
static constexpr unsigned mostBlocksOfChoice = 16;
static constexpr unsigned mostChoices = 3;

static bool appendChosenTexts(const BodyIndex& index, ValueID value, unsigned depth, Vector<StringID, 8>& texts)
{
    value = index.stripCasts(value);
    if (!index.isParameter(value)) {
        if (index.opcodeOf(value) != Opcode::String || texts.size() == mostChosenTexts)
            return false;
        texts.append(index.value(value).as<Opcode::String>().string());
        return true;
    }
    BlockID block = index.blockOf(value);
    BlockID chooser = block == entryBlock ? BlockID() : index.immediateDominator(block);
    if (!depth || !chooser || chooser.index() >= block.index() || block.index() - chooser.index() > mostBlocksOfChoice || index.isEnteredByHandlerEdge(block))
        return false;
    uint32_t place = 0;
    while (index.block(block).parameter(place) != value)
        ++place;
    uint32_t numberSeen = 0;
    for (uint32_t from = chooser.index(); from < block.index(); ++from) {
        for (BodyTarget target : index.terminator(BlockID(from)).targets()) {
            if (target.block() != block)
                continue;
            if (place >= target.arguments().size() || !appendChosenTexts(index, target.arguments()[place], depth - 1, texts))
                return false;
            ++numberSeen;
        }
    }
    return numberSeen && numberSeen == index.numberOfIncomingEdges(block);
}

static bool isChosenAmongTexts(const BodyIndex& index, ValueID key)
{
    ValueID name = key ? index.stripCasts(key) : ValueID();
    Vector<StringID, 8> texts;
    return name && index.isParameter(name) && appendChosenTexts(index, name, mostChoices, texts);
}

bool LinkFlow::appendTextsOfKey(FunctionID id, const BodyIndex& index, ValueID key, Vector<StringID, 8>& texts) const
{
    if (isChosenAmongTexts(index, key))
        return appendChosenTexts(index, key, mostChoices, texts);
    if (appendTextsOfParameter(id, index, key, texts))
        return true;
    texts.shrink(0);
    if (ValueID name = key ? index.stripCasts(key) : ValueID(); name && !index.isParameter(name) && index.opcodeOf(name) == Opcode::ElemGet)
        return appendTextsOfElement(id, index, index.value(name), texts);
    ValueID list = whatStepGoesThroughForLinkAccesses(index, key, false);
    bool keyIsFirstOfPair = !list;
    if (keyIsFirstOfPair) {
        ValueID pair = whatStepGoesThroughForLinkAccesses(index, key, true);
        for (ValueID part = pair; part; part = index.value(part).operand(0)) {
            if (index.isParameter(part) || !index.hasOneUse(part))
                return false;
            if (!opcodesThatStateTypes.contains(index.opcodeOf(part)))
                break;
        }
        list = pair ? whatStepGoesThroughForLinkAccesses(index, pair, false) : ValueID();
    }
    if (!list)
        return false;
    while (!index.isParameter(list) && index.hasOneUse(list) && opcodesThatStateTypes.contains(index.opcodeOf(list)))
        list = index.value(list).operand(0);
    if (!keyIsFirstOfPair && index.isParameter(list) && index.blockOf(list) == entryBlock)
        return index.hasOneUse(list) && appendArrayParameterStrings(id, index, list, texts);
    if (!keyIsFirstOfPair && !index.isParameter(list) && (!index.hasOneUse(list) || variableReadAtForLinkAccesses(index, list)))
        return appendReadListTexts(id, index, list, texts);
    if (index.isParameter(list) || !index.hasOneUse(list) || opcodesThatStateTypes.contains(index.opcodeOf(list)))
        return false;
    return keyIsFirstOfPair ? appendFirstTextsOfWrittenPairsForLinkAccesses(index, list, texts) : appendTextsOfWrittenListForLinkAccesses(program, index, list, texts);
}

Vector<PropertyKeyID, 8> LinkFlow::findNamesOfStore(FunctionID id, const SummaryAccess& access) const
{
    Vector<PropertyKeyID, 8> names;
    if (access.kind != SummaryAccess::Kind::Store || access.key || access.keyNode == noSummaryNode || access.isToContents() || program.admitsReplacedBuiltins || testRuns().isTestRun())
        return names;
    const BodyIndex* index = access.site ? bodiesLookedAt(looks).find(id) : nullptr;
    if (!index || index->isParameter(access.site) || index->opcodeOf(access.site) != Opcode::ElemSet)
        return names;
    ValueID key = index->value(access.site).as<Opcode::ElemSet>().propertyOperand();
    if (!mayLeaveLoopForLinkAccesses(*index, key) && !isChosenAmongTexts(*index, key))
        return names;
    for (BuiltinID row : { Builtins::ArrayPrototypeValues, Builtins::ArrayIteratorPrototypeNext, Builtins::ArrayPrototypeForEach }) {
        if (program.isUnlocked(row) || program.isUnlocked(program.builtins().canonicalID(row)))
            return names;
    }
    Vector<StringID, 8> texts;
    if (!appendTextsOfKey(id, *index, key, texts)) {
        texts.shrink(0);
        if (!appendCallbackKeys(id, *index, key, texts))
            return names;
    }
    if (!appendNamesOfTexts(texts.span(), names))
        names.shrink(0);
    return names;
}

Vector<PropertyKeyID, 8> LinkFlow::findAffixedNamesOfStore(FunctionID id, const SummaryAccess& access) const
{
    Vector<PropertyKeyID, 8> names;
    const BodyIndex* index = access.site ? bodiesLookedAt(looks).find(id) : nullptr;
    if (!index || index->isParameter(access.site) || index->opcodeOf(access.site) != Opcode::ElemSet)
        return names;
    KeyAffixes affixes;
    ValueID key = valueBetweenAffixes(program, *index, index->value(access.site).as<Opcode::ElemSet>().propertyOperand(), affixes);
    if (!key || !mayLeaveLoopForLinkAccesses(*index, key))
        return names;
    std::optional<ListedObject> listed = findListedObject(id, *index, key);
    if (!listed || !program.at(listed->function).isModuleBody())
        return names;
    const LinkType records = typeAt(nodeOf(listed->function, listed->node));
    if (records.objects != LinkType::Objects::Layout)
        return names;
    auto append = [&](PropertyKeyID key) {
        if (!key || program.at(key).kind != PropertyKeyKind::Text)
            return;
        std::span<const uint8_t> middle = program.string(program.at(key).text);
        Vector<uint8_t, 64> joined;
        joined.append(affixes.beginning);
        joined.append(middle);
        joined.append(affixes.ending);
        StringID text = program.strings.find(joined.span());
        std::optional<PropertyKeyID> name = text ? program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = text }) : std::nullopt;
        if (!name || program.isIndexLike(*name) || nameClassOf(*name) == NameClass::Proto || mayBeElementKey(nameClassOf(*name)))
            return;
        names.append(*name);
    };
    for (const Field& field : program.at(records.layout).fields)
        append(field.computedKey ? PropertyKeyID() : field.key);
    forEachAppendedField(records.layout, [&](PropertyKeyID key, FlowNodeID) {
        append(key);
    });
    return names;
}

bool LinkFlow::appendNamesOfTexts(std::span<const StringID> texts, Vector<PropertyKeyID, 8>& names) const
{
    for (StringID text : texts) {
        std::optional<PropertyKeyID> name = program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = text });
        if (!name || program.isIndexLike(*name) || nameClassOf(*name) == NameClass::Proto || mayBeElementKey(nameClassOf(*name)))
            return false;
        if (!names.contains(*name))
            names.append(*name);
    }
    return true;
}

bool AccessInLinkAccesses::storesUnderCallbackKeys(const LinkType& subject)
{
    if (m_access.kind != Kind::Store || m_shared || subject.objects == LinkType::Objects::None || m_program.admitsReplacedBuiltins || m_flow.testRuns().isTestRun())
        return false;
    const BodyIndex* index = bodiesLookedAt(m_flow.looks).find(m_function);
    if (!index || index->isParameter(m_access.site) || index->opcodeOf(m_access.site) != Opcode::ElemSet || !mayLeaveLoopForLinkAccesses(*index, index->value(m_access.site).as<Opcode::ElemSet>().propertyOperand()))
        return false;
    BuiltinID forEach = Builtins::ArrayPrototypeForEach;
    if (m_program.isUnlocked(forEach) || m_program.isUnlocked(m_table.canonicalID(forEach)))
        return false;
    Vector<StringID, 8> keys;
    return m_flow.appendCallbackKeys(m_function, *index, index->value(m_access.site).as<Opcode::ElemSet>().propertyOperand(), keys) && resolveUnderTexts(keys.span());
}

bool LinkFlow::iteratesListsAsBuilt() const
{
    for (BuiltinID row : { Builtins::ArrayPrototypeValues, Builtins::ArrayIteratorPrototypeNext, Builtins::ArrayPrototypeForEach }) {
        if (program.isUnlocked(row) || program.isUnlocked(program.builtins().canonicalID(row)))
            return false;
    }
    return !program.admitsReplacedBuiltins && !testRuns().isTestRun() && !program.isSomeNameOpenedOn(program.builtins().canonicalID(Builtins::ObjectPrototype));
}

static ValueID callThatAloneLeadsIntoBlock(const BodyIndex& index, BlockID block)
{
    std::optional<BodyIncomingEdge> edge = index.onlyIncomingEdge(block);
    if (!edge)
        return { };
    BodyTerminator terminator = index.terminator(edge->from);
    if (terminator.kind() != TerminatorKind::Branch)
        return { };
    bool holds = !edge->targetIndex;
    ValueID tested = index.stripCasts(terminator.as<TerminatorKind::Branch>().conditionOperand());
    while (!index.isParameter(tested) && (index.opcodeOf(tested) == Opcode::ToBoolean || index.opcodeOf(tested) == Opcode::LogicalNot)) {
        holds ^= index.opcodeOf(tested) == Opcode::LogicalNot;
        tested = index.stripCasts(index.value(tested).operand(0));
    }
    return holds && !index.isParameter(tested) && index.opcodeOf(tested) == Opcode::Call ? tested : ValueID();
}

std::span<const BlockID> LinkFlow::blocksBehindTestsByCalls(FunctionID id, const BodyIndex& index) const
{
    {
        Locker locker { m_lockOfCallsInSight };
        if (auto known = m_blocksBehindTestsByCalls.find(id.index()); known != m_blocksBehindTestsByCalls.end())
            return known->value.span();
    }
    Vector<BlockID> nearest(FillWith { }, index.numberOfBlocks(), BlockID());
    for (BlockID block : index.blocksInDominatorOrder()) {
        BlockID above = index.immediateDominator(block);
        if (above)
            nearest[block.index()] = callThatAloneLeadsIntoBlock(index, block) ? block : nearest[above.index()];
    }
    Locker locker { m_lockOfCallsInSight };
    return m_blocksBehindTestsByCalls.add(id.index(), WTF::move(nearest)).iterator->value.span();
}

bool LinkFlow::isBehindOwnNameTest(FunctionID id, const BodyIndex& index, ValueID site, ValueID object, ValueID name, const LinkType& objects, Vector<FlowNodeID, 4>& watched) const
{
    BuiltinID hasOwnProperty = program.builtins().canonicalID(Builtins::ObjectPrototypeHasOwnProperty);
    PropertyKeyID hasOwnKey = program.keyOf(BuiltinNames::hasOwnProperty);
    PropertyKeyID callKey = program.keyOf(BuiltinNames::call);
    auto isOpened = [&](BuiltinName opened) {
        StringID text = program.stringOf(opened);
        return text && program.isOpenedSomewhere(text);
    };
    auto isBuiltin = [&](ValueID value, BuiltinID builtin) {
        return !index.isParameter(value) && index.opcodeOf(value) == Opcode::Builtin && program.builtins().canonicalID(index.value(value).as<Opcode::Builtin>().builtin()) == program.builtins().canonicalID(builtin);
    };
    auto inheritsTest = [&] {
        LockerOfWhatIsShared lockerOfWhatIsShared(*this);
        bool inherits = true;
        forEachLayoutOf(objects, [&](LayoutID layout) {
            FlowNodeID appended = findAppendedField(layout, hasOwnKey);
            if (appended)
                watched.append(appended);
            inherits = inherits && !findField(program, layout, hasOwnKey) && (!appended || typeAt(appended).isNever()) && !findMembers(layout, ClassSide::Prototype, hasOwnKey).mayAnswer() && !mayLieOnPrototypeAbove(layout, hasOwnKey, true, PrimitivesOnPrototypes::Count);
        });
        return inherits;
    };
    auto asksOwnName = [&](ValueID tested) {
        auto call = index.value(tested).as<Opcode::Call>();
        ValueID callee = call.calleeOperand() ? index.stripCasts(call.calleeOperand()) : ValueID();
        std::span<const ValueID> arguments = call.argumentsOperands();
        auto isAt = [&](size_t place, ValueID expected) {
            return place < arguments.size() && arguments[place] && index.stripCasts(arguments[place]) == expected;
        };
        if (!callee || index.isParameter(callee))
            return false;
        if (isBuiltin(callee, Builtins::ObjectHasOwn))
            return isAt(0, object) && isAt(1, name);
        if (index.opcodeOf(callee) != Opcode::PropGet || !call.thisValueOperand())
            return false;
        auto method = index.value(callee).as<Opcode::PropGet>();
        ValueID holder = index.stripCasts(method.objectOperand());
        if (holder != index.stripCasts(call.thisValueOperand()))
            return false;
        if (method.key() == hasOwnKey)
            return holder == object && isAt(0, name) && !isOpened(BuiltinNames::hasOwnProperty) && inheritsTest();
        if (method.key() != callKey || isOpened(BuiltinNames::call) || isOpened(BuiltinNames::hasOwnProperty) || !isAt(0, object) || !isAt(1, name))
            return false;
        if (isBuiltin(holder, Builtins::ObjectPrototypeHasOwnProperty))
            return true;
        const ThinSummary& summary = summaries.at(id);
        auto made = std::ranges::find(summary.calls, tested, &SummaryCall::site);
        if (made == summary.calls.end() || made->key != callKey || made->isConstruction || made->passesList || made->receiver == noSummaryNode)
            return false;
        FlowNodeID node = nodeOf(id, made->receiver);
        const LinkType& held = typeAt(node);
        LinkType alone;
        alone.builtin = held.builtin;
        if (!(held == alone) || !held.builtin || program.builtins().canonicalID(held.builtin) != hasOwnProperty)
            return false;
        watched.append(node);
        return true;
    };
    BlockID blockOfName = index.blockOf(name);
    std::span<const BlockID> behindTests = blocksBehindTestsByCalls(id, index);
    for (BlockID block = behindTests[index.blockOf(site).index()]; block && index.strictlyDominates(blockOfName, block); block = behindTests[index.immediateDominator(block).index()]) {
        if (asksOwnName(callThatAloneLeadsIntoBlock(index, block)))
            return true;
    }
    return false;
}

Vector<PropertyKeyID, 16> LinkFlow::findListedNamesOfStore(FunctionID id, const SummaryAccess& access) const
{
    Vector<PropertyKeyID, 16> names;
    if (access.kind != SummaryAccess::Kind::Store || access.key || access.keyNode == noSummaryNode || access.isToContents() || !iteratesListsAsBuilt())
        return names;
    const BodyIndex* index = access.site ? bodiesLookedAt(looks).find(id) : nullptr;
    if (!index || index->isParameter(access.site) || index->opcodeOf(access.site) != Opcode::ElemSet)
        return names;
    ValueID key = index->value(access.site).as<Opcode::ElemSet>().propertyOperand();
    std::optional<ListedObject> listed = mayLeaveLoopForLinkAccesses(*index, key) ? findListedObject(id, *index, key) : std::nullopt;
    if (!listed)
        return names;
    const LinkType& objects = typeAt(nodeOf(listed->function, listed->node));
    Vector<FlowNodeID, 4> watched;
    bool listsOwnNames = !listed->isLoop || isBehindOwnNameTest(id, *index, access.site, listed->object, index->stripCasts(key), objects, watched);
    if (objects.isNever() || !appendListedNames(objects, listsOwnNames ? PrototypeNames::Excluded : PrototypeNames::Included, names))
        names.shrink(0);
    std::ranges::sort(names, { }, &PropertyKeyID::bits);
    names.shrink(std::ranges::unique(names).begin() - names.begin());
    return names;
}

bool AccessInLinkAccesses::accessesListedNames(const LinkType& subject)
{
    bool stores = m_access.kind == Kind::Store;
    if ((!stores && !isRead()) || m_shared || (!stores && m_keyIsOnlyNumbers) || subject.objects == LinkType::Objects::None)
        return false;
    if (!stores && subject.objects != LinkType::Objects::Layout && subject.objects != LinkType::Objects::Set)
        return false;
    if (!m_flow.iteratesListsAsBuilt())
        return false;
    const BodyIndex* index = bodiesLookedAt(m_flow.looks).find(m_function);
    if (!index || index->isParameter(m_access.site) || index->opcodeOf(m_access.site) != (stores ? Opcode::ElemSet : Opcode::ElemGet))
        return false;
    if (!stores) {
        auto read = index->value(m_access.site).as<Opcode::ElemGet>();
        std::optional<LinkFlow::ListedObject> listed = mayLeaveLoopForLinkAccesses(*index, read.propertyOperand()) ? m_flow.findListedObject(m_function, *index, read.propertyOperand()) : std::nullopt;
        if (!listed)
            return false;
        FlowNodeID node = isListedObject(*index, index->stripCasts(read.objectOperand()), *listed) ? m_subject : m_flow.nodeOf(listed->function, listed->node);
        tellAgainOnceClimbsWhoeverTells(node);
        PrototypeNames prototypeNames = listed->isLoop ? prototypeNamesAt(*index, listed->object, index->stripCasts(read.propertyOperand()), m_flow.typeAt(node)) : PrototypeNames::Excluded;
        Vector<PropertyKeyID, 16> names;
        if (!appendNamesOfRecords(node, prototypeNames, names))
            return false;
        resolveUnderNames(names.span());
        return true;
    }
    if (!mayLeaveLoopForLinkAccesses(*index, index->value(m_access.site).as<Opcode::ElemSet>().propertyOperand()))
        return false;
    auto store = index->value(m_access.site).as<Opcode::ElemSet>();
    std::optional<LinkFlow::ListedObject> listed = m_flow.findListedObject(m_function, *index, store.propertyOperand());
    if (!listed)
        return false;
    FlowNodeID node = m_flow.nodeOf(listed->function, listed->node);
    tellAgainOnceClimbsWhoeverTells(node);
    const LinkType records = m_flow.typeAt(node);
    PrototypeNames prototypeNames = listed->isLoop ? prototypeNamesAt(*index, listed->object, index->stripCasts(store.propertyOperand()), records) : PrototypeNames::Excluded;
    Vector<PropertyKeyID, 16> names;
    if (!appendNamesOfRecords(node, prototypeNames, names))
        return false;
    if (!holdsValueUnderKey(*index, store, *listed) || !storeEachListedValue(records, prototypeNames, listed->givesPairs ? ListedNames::AreThere : ListedNames::MayBeGone))
        resolveUnderNames(names.span());
    return true;
}

bool AccessInLinkAccesses::holdsValueUnderKey(const BodyIndex& index, const BodyOperation<Opcode::ElemSet>& store, const LinkFlow::ListedObject& listed) const
{
    ValueID key = index.stripCasts(store.propertyOperand());
    ValueID stored = store.valueOperand() ? index.stripCasts(store.valueOperand()) : ValueID();
    if (!stored || index.isParameter(stored))
        return false;
    if (listed.givesPairs) {
        auto openOf = [&](ValueID part, uint32_t place) -> ValueID {
            if (index.isParameter(part) || index.opcodeOf(part) != Opcode::ResultAt || index.value(part).as<Opcode::ResultAt>().index() != static_cast<uint32_t>(IteratorResumeResult::Value))
                return { };
            ValueID step = index.value(part).as<Opcode::ResultAt>().resultsOperand();
            if (index.isParameter(step) || index.opcodeOf(step) != Opcode::IteratorResume || index.value(step).as<Opcode::IteratorResume>().stepIndex() != std::optional<uint32_t>(place))
                return { };
            return index.value(step).operand(0);
        };
        ValueID open = openOf(stored, 1);
        return open && openOf(key, 0) == open;
    }
    if (index.opcodeOf(stored) != Opcode::ElemGet)
        return false;
    auto isSameElement = [&](ValueID read, ValueID other) {
        if (index.isParameter(read) || index.isParameter(other) || index.opcodeOf(read) != Opcode::ElemGet || index.opcodeOf(other) != Opcode::ElemGet)
            return false;
        auto first = index.value(read).as<Opcode::ElemGet>();
        auto second = index.value(other).as<Opcode::ElemGet>();
        return index.stripCasts(first.objectOperand()) == index.stripCasts(second.objectOperand()) && index.stripCasts(first.propertyOperand()) == index.stripCasts(second.propertyOperand());
    };
    if (ValueID readUnder = index.stripCasts(index.value(stored).as<Opcode::ElemGet>().propertyOperand()); readUnder != key && !isSameElement(readUnder, key))
        return false;
    return isListedObject(index, index.stripCasts(index.value(stored).as<Opcode::ElemGet>().objectOperand()), listed);
}

bool AccessInLinkAccesses::isListedObject(const BodyIndex& index, ValueID holder, const LinkFlow::ListedObject& listed) const
{
    if (listed.function == m_function)
        return holder == listed.object;
    const BodyIndex* maker = bodiesLookedAt(m_flow.looks).find(listed.function);
    if (!maker)
        return false;
    auto variableRead = [](const BodyIndex& body, ValueID read) {
        if (read && !body.isParameter(read) && body.opcodeOf(read) == Opcode::CheckInitialized)
            read = body.stripCasts(body.value(read).operand(0));
        return read && !body.isParameter(read) && body.opcodeOf(read) == Opcode::GetModuleVariable ? body.value(read).as<Opcode::GetModuleVariable>().variable() : VariableID();
    };
    VariableID variable = variableRead(index, holder);
    bool isCapture = index.isParameter(holder) && index.blockOf(holder) == entryBlock && holder.index() < m_program.at(m_function).captures.size();
    if (!isCapture && (!variable || !m_program.variables[variable.index()].flags.contains(VariableFlags::IsImmutable)))
        return false;
    if (variable && variableRead(*maker, listed.object) == variable)
        return true;
    for (uint32_t number = 0; number < maker->numberOfBlocks(); ++number) {
        for (BodyValue value : maker->block(BlockID(number)).values()) {
            if (isCapture && value.opcode() == Opcode::NewClosure && value.as<Opcode::NewClosure>().function() == m_function) {
                std::span<const ValueID> captured = value.variadicOperands();
                return holder.index() < captured.size() && captured[holder.index()] && maker->stripCasts(captured[holder.index()]) == listed.object;
            }
            if (variable && value.opcode() == Opcode::PutModuleVariable && value.as<Opcode::PutModuleVariable>().variable() == variable)
                return value.operand(0) && maker->stripCasts(value.operand(0)) == listed.object;
        }
    }
    return false;
}

bool AccessInLinkAccesses::resolveUnderTexts(std::span<const StringID> texts)
{
    Vector<PropertyKeyID, 8> names;
    if (!m_flow.appendNamesOfTexts(texts, names))
        return false;
    resolveUnderNames(names.span());
    return true;
}

void AccessInLinkAccesses::resolveUnderNames(std::span<const PropertyKeyID> names)
{
    if (m_access.kind == Kind::Store)
        m_flow.build.statistics().count(StatisticID::StoresResolvedToLiteralKeys);
    for (PropertyKeyID name : names) {
        AccessInLinkAccesses underName = *this;
        underName.m_access.key = name;
        underName.m_access.isCopySpelledByLoop = false;
        underName.m_isUnderListedName = true;
        underName.m_nameClass = m_flow.nameClassOf(name);
        underName.m_keyIsWritten = true;
        underName.m_keyIsOnlyNumbers = false;
        underName.m_keyCannotNameBuiltin = false;
        underName.m_keyIsSymbolAsFarAsIsSeen = false;
        underName.m_seenKeysAreNumbers = false;
        underName.m_keyMayNamePrototypeLink = false;
        underName.m_before = { };
        underName.resolve();
    }
}

bool AccessInLinkAccesses::appendNamesOfRecords(FlowNodeID node, PrototypeNames prototypeNames, Vector<PropertyKeyID, 16>& names)
{
    const LinkType records = m_flow.typeAt(node);
    if (records.objects == LinkType::Objects::Layout || records.objects == LinkType::Objects::Set) {
        m_flow.forEachLayoutOf(records, [&](LayoutID id) {
            tellAgainOnceClimbsWhoeverTells(m_flow.extraPropertyStoreNode(id));
            tellAgainOnceClimbsWhoeverTells(m_flow.joinedExtraProperties(id));
            m_flow.trackAppendedNames(id);
            if (m_program.at(id).kind == LayoutKind::Instance)
                m_flow.build.statistics().count(StatisticID::InstanceNamesReadByLoops);
        });
    }
    size_t numberOfNames = 0;
    if (!m_flow.appendListedNames(records, prototypeNames, names, &numberOfNames))
        return false;
    m_flow.noteNamesOfRecordsRead(observerOfWhatWasLookedUp(), node, numberOfNames);
    std::ranges::sort(names, { }, &PropertyKeyID::bits);
    names.shrink(std::ranges::unique(names).begin() - names.begin());
    return true;
}

auto AccessInLinkAccesses::prototypeNamesAt(const BodyIndex& index, ValueID object, ValueID name, const LinkType& objects) -> PrototypeNames
{
    Vector<FlowNodeID, 4> watched;
    if (!m_flow.isBehindOwnNameTest(m_function, index, m_access.site, object, name, objects, watched))
        return PrototypeNames::Included;
    for (FlowNodeID node : watched)
        tellAgainOnceClimbsWhoeverTells(node);
    return PrototypeNames::Excluded;
}

bool AccessInLinkAccesses::storeEachListedValue(const LinkType& listed, PrototypeNames prototypeNames, ListedNames listedNames)
{
    Vector<LinkFlow::OwnValue, 16> stored;
    FlowNodeID undefinedIfGone;
    bool holdsValuesInSight = true;
    m_flow.forEachLayoutOf(listed, [&](LayoutID layout) {
        Vector<LinkFlow::OwnValue, 16> values(m_flow.ownValuesOf(layout));
        holdsValuesInSight = holdsValuesInSight && !values.isEmpty() && !m_flow.possibleChanges[layout.index()].containsAny({ LayoutChange::ChangesPrototype, LayoutChange::GetsAccessor });
        if (!holdsValuesInSight)
            return;
        if (listedNames == ListedNames::MayBeGone) {
            FlowNodeID lostName = m_flow.lostNameNode(layout);
            tellAgainOnceClimbsWhoeverTells(lostName);
            if (!m_flow.typeAt(lostName).isNever())
                undefinedIfGone = lostName;
        }
        size_t numberOfListedValues = values.size();
        m_flow.appendAppendedNames(layout, values);
        bool isInstance = m_program.at(layout).kind == LayoutKind::Instance;
        if (isInstance) {
            values.removeAllMatching([&](const LinkFlow::OwnValue& value) {
                return m_flow.typeAt(value.node).isNever();
            }, numberOfListedValues);
        }
        Vector<PropertyKeyID, 16> above;
        if (isInstance && m_flow.hasFollowedPrototypes(layout) && !m_flow.appendPrototypeNames(layout, above)) {
            holdsValuesInSight = false;
            return;
        }
        for (const LinkFlow::OwnValue& value : values) {
            if (!value.key || m_program.at(value.key).kind != PropertyKeyKind::Text)
                continue;
            stored.append(value);
            if (isInstance && (!m_flow.isAbsentFromObjectPrototype(value.key) || m_flow.findMembers(layout, ClassSide::Prototype, value.key).mayAnswer()))
                holdsValuesInSight = false;
        }
        for (PropertyKeyID name : above) {
            bool isOwn = std::ranges::contains(values, name, &LinkFlow::OwnValue::key);
            if (isOwn || prototypeNames == PrototypeNames::Included)
                stored.append({ name, m_flow.foundOnPrototypesAbove(layout, name) });
        }
    });
    if (!holdsValuesInSight)
        return false;
    FlowNodeID storedUnderAnyName = m_value;
    for (const LinkFlow::OwnValue& value : stored) {
        m_value = value.node;
        resolveUnderNames(std::span<const PropertyKeyID>(&value.key, 1));
        if (undefinedIfGone) {
            m_value = undefinedIfGone;
            resolveUnderNames(std::span<const PropertyKeyID>(&value.key, 1));
        }
    }
    m_value = storedUnderAnyName;
    m_flow.build.statistics().count(StatisticID::ListedValuesStoredByName);
    return true;
}

bool AccessInLinkAccesses::accessesNamesFromLoop()
{
    bool stores = m_access.kind == Kind::Store;
    if ((!isRead() && !stores) || m_shared || m_flow.testRuns().isTestRun() || m_program.isSomeNameOpenedOn(m_table.canonicalID(Builtins::ObjectPrototype)))
        return false;
    const BodyIndex* index = bodiesLookedAt(m_flow.looks).find(m_function);
    if (!index || index->isParameter(m_access.site) || index->opcodeOf(m_access.site) != (stores ? Opcode::ElemSet : Opcode::ElemGet))
        return false;
    ValueID key = index->stripCasts(stores ? index->value(m_access.site).as<Opcode::ElemSet>().propertyOperand() : index->value(m_access.site).as<Opcode::ElemGet>().propertyOperand());
    if (!key || index->isParameter(key) || index->opcodeOf(key) != Opcode::ResultAt || index->value(key).as<Opcode::ResultAt>().index() != static_cast<uint32_t>(ForInStepResult::Key))
        return false;
    ValueID step = index->value(key).as<Opcode::ResultAt>().resultsOperand();
    if (index->isParameter(step) || index->opcodeOf(step) != Opcode::ForInStep)
        return false;
    ValueID subject = index->stripCasts(index->value(step).as<Opcode::ForInStep>().objectOperand());
    if (subject && !index->isParameter(subject) && index->opcodeOf(subject) == Opcode::CheckInitialized)
        subject = index->stripCasts(index->value(subject).operand(0));
    FlowNodeID node;
    if (subject && index->isParameter(subject) && subject.index() < m_program.at(m_function).numberOfEntryParameters())
        node = m_flow.parameter(m_function, subject.index());
    else if (subject && !index->isParameter(subject) && index->opcodeOf(subject) == Opcode::GetModuleVariable)
        node = m_flow.variable(index->value(subject).as<Opcode::GetModuleVariable>().variable());
    else if (!stores && subject && index->stripCasts(index->value(m_access.site).as<Opcode::ElemGet>().objectOperand()) == subject)
        node = m_subject;
    else if (stores && m_access.isCopySpelledByLoop)
        node = m_flow.nodeOf(m_function, m_flow.sourceOfCopySpelledByLoop(m_function, m_access.site));
    else
        return false;

    tellAgainOnceClimbsWhoeverTells(node);
    const LinkType listed = m_flow.typeAt(node);
    PrototypeNames prototypeNames = prototypeNamesAt(*index, subject, key, listed);
    Vector<PropertyKeyID, 16> names;
    if (!appendNamesOfRecords(node, prototypeNames, names))
        return false;
    if (!stores || !m_access.isCopySpelledByLoop || !storeEachListedValue(listed, prototypeNames, ListedNames::AreThere))
        resolveUnderNames(names.span());
    return true;
}

static void collectListAliases(const BodyIndex& index, BitVector& aliases)
{
    bool grows = true;
    auto unite = [&](ValueID one, ValueID other) {
        if (!one || !other || aliases.get(one.index()) == aliases.get(other.index()))
            return;
        aliases.set(one.index());
        aliases.set(other.index());
        grows = true;
    };
    while (std::exchange(grows, false)) {
        for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
            BodyBlock block = index.block(BlockID(number));
            for (uint32_t place = 0; place < block.numberOfParameters(); ++place)
                unite(block.parameter(place), index.valueNarrowedInto(block.parameter(place)));
            for (BodyValue value : block.values()) {
                if (opcodesThatStateTypes.contains(value.opcode()))
                    unite(value.id(), value.operand(0));
            }
            for (BodyTarget target : index.terminator(block.id()).targets()) {
                std::span<const ValueID> arguments = target.arguments();
                for (uint32_t place = 0; place < arguments.size(); ++place)
                    unite(arguments[place], index.block(target.block()).parameter(place));
            }
        }
    }
}

static bool isLocalListMethod(const Program& program, PropertyKeyID key)
{
    const BuiltinName methods[] = { BuiltinNames::push, BuiltinNames::pop, BuiltinNames::shift, BuiltinNames::unshift, BuiltinNames::indexOf, BuiltinNames::includes, BuiltinNames::at };
    return std::ranges::any_of(methods, [&](BuiltinName method) {
        return key == program.keyOf(method);
    });
}

static bool usesListLocally(const Program& program, const BodyIndex& index, const BitVector& aliases, const BodyValue& user, uint32_t operandIndex)
{
    switch (user.opcode()) {
    case Opcode::PropGet:
        return operandIndex == OperandIndex::PropGet::object;
    case Opcode::ElemGet:
        return operandIndex == OperandIndex::ElemGet::object;
    case Opcode::ElemSet:
        return operandIndex == OperandIndex::ElemSet::object;
    case Opcode::Call: {
        if (operandIndex != OperandIndex::Call::thisValue)
            return false;
        ValueID callee = index.stripCasts(user.as<Opcode::Call>().calleeOperand());
        if (index.isParameter(callee) || index.opcodeOf(callee) != Opcode::PropGet)
            return false;
        auto method = index.value(callee).as<Opcode::PropGet>();
        return aliases.get(method.objectOperand().index()) && isLocalListMethod(program, method.key());
    }
    default:
        return opcodesThatStateTypes.contains(user.opcode());
    }
}

bool AccessInLinkAccesses::isFinalListRead()
{
    const BodyIndex* index = isRead() && !isNamed() && !m_shared && m_receiver == m_subject && !m_program.at(m_function).isCoroutine() ? bodiesLookedAt(m_flow.looks).find(m_function) : nullptr;
    if (!index || m_access.site.index() >= index->numberOfValues() || index->isParameter(m_access.site) || index->opcodeOf(m_access.site) != Opcode::ElemGet)
        return false;
    BodyBlock last = index->block(index->blockOf(m_access.site));
    if (last.hasHandler() || index->terminator(last.id()).numberOfTargets())
        return false;
    ValueID list = index->value(m_access.site).as<Opcode::ElemGet>().objectOperand();
    ValueID root = index->stripCasts(list);
    auto isRoot = [&](ValueID value) {
        return index->opcodeOf(value) == Opcode::NewArray || (index->opcodeOf(value) == Opcode::Data && !!m_program.at(index->value(value).as<Opcode::Data>().data()).root.tryAs<DataNode::Array>());
    };
    if (index->isParameter(root) ? index->blockOf(root) == entryBlock : !isRoot(root))
        return false;

    BitVector aliases(index->numberOfValues());
    aliases.set(list.index());
    collectListAliases(*index, aliases);
    for (uint32_t number = 0; number < index->numberOfBlocks(); ++number) {
        BodyBlock block = index->block(BlockID(number));
        for (uint32_t place = 0; place < block.numberOfParameters(); ++place) {
            if (aliases.get(block.parameter(place).index()) && (block.id() == entryBlock || block.isHandlerBlock()))
                return false;
        }
        for (BodyValue value : block.values()) {
            if (aliases.get(value.id().index()) && !opcodesThatStateTypes.contains(value.opcode()) && !isRoot(value.id()))
                return false;
            bool isAfterRead = block.id() == last.id() && value.id().index() > m_access.site.index();
            bool staysLocal = true;
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                staysLocal &= !operand || !aliases.get(operand.index()) || (!isAfterRead && usesListLocally(m_program, *index, aliases, value, operandIndex));
            });
            if (!staysLocal)
                return false;
        }
        bool staysInBlocks = true;
        index->terminator(block.id()).forEachOperand([&](ValueID operand, uint32_t) {
            staysInBlocks &= !operand || !aliases.get(operand.index());
        });
        if (!staysInBlocks)
            return false;
        if (block.hasHandler()) {
            for (ValueID argument : block.handler().arguments()) {
                if (aliases.get(argument.index()))
                    return false;
            }
        }
    }
    return true;
}

std::span<const uint32_t> AccessInLinkAccesses::definitionsByDescriptorAtSite() const
{
    const ThinSummary& summary = m_flow.summaries.at(m_function);
    auto found = std::ranges::equal_range(summary.definitionsByDescriptorInOrderOfSites.span(), m_access.site.index(), { }, [&](uint32_t change) {
        return summary.changes[change].site.index();
    });
    return { found.begin(), found.end() };
}

bool AccessInLinkAccesses::changeLaysWhatDescriptorsInSightHold()
{
    if (m_shared || m_access.kind != Kind::Define || !m_access.isByDescriptor || !m_access.key || m_value != m_flow.unseen())
        return false;
    for (uint32_t place : definitionsByDescriptorAtSite()) {
        const SummaryChange& change = m_flow.summaries.at(m_function).changes[place];
        if (change.descriptorIsInSight || change.other == noSummaryNode)
            continue;
        FlowNodeID descriptor = m_flow.nodeOf(m_function, change.other);
        tellAgainOnceClimbs(descriptor);
        const LinkType descriptors = m_flow.typeAt(descriptor);
        Vector<FlowNodeID, 4> values;
        FlowObserver toldAgain = observerOfWhatWasLookedUp();
        return descriptors.isNever() || m_flow.descriptorsInSightLayValuesAlone(descriptors, &toldAgain, values);
    }
    return false;
}

FlowNodeID AccessInLinkAccesses::findValueThatWrittenDescriptorLays() const
{
    if (m_shared || m_access.kind != Kind::Define || !m_access.isByDescriptor || !m_access.key || m_value != m_flow.unseen())
        return { };
    for (uint32_t place : definitionsByDescriptorAtSite()) {
        const SummaryChange& change = m_flow.summaries.at(m_function).changes[place];
        if (change.key == m_access.key && change.descriptorIsInSight && change.descriptorLaysNoAccessor && change.valueInSight != noSummaryNode)
            return m_flow.nodeOf(m_function, change.valueInSight);
    }
    return { };
}

bool AccessInLinkAccesses::changeLaysAccessorInSight() const
{
    if (m_shared || m_access.kind != Kind::Define || !m_access.isByDescriptor || !m_access.key || m_value != m_flow.unseen())
        return false;
    return std::ranges::any_of(definitionsByDescriptorAtSite(), [&](uint32_t place) {
        const SummaryChange& change = m_flow.summaries.at(m_function).changes[place];
        return change.key == m_access.key && change.lays == SummaryChange::Lays::WhatDescriptorSays
            && change.descriptorIsInSight && !change.descriptorLaysNoAccessor;
    });
}

static bool isAtLeastForLinkAccesses(const BodyIndex& index, ValueID value, int64_t bound, Vector<ValueID, 8>& met)
{
    value = index.stripCasts(value);
    if (!value || met.contains(value))
        return !!value;
    if (!index.isParameter(value)) {
        BodyValue made = index.value(value);
        if (made.opcode() == Opcode::Integer)
            return made.as<Opcode::Integer>().integer() >= bound;
        return made.opcode() == Opcode::Increment && isAtLeastForLinkAccesses(index, made.operand(0), bound, met);
    }
    BlockID block = index.blockOf(value);
    if (block == entryBlock || index.valueNarrowedInto(value))
        return false;
    met.append(value);
    bool isReached = false;
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        if (index.block(BlockID(number)).hasHandler() && index.block(BlockID(number)).handler().block() == block)
            return false;
        for (BodyTarget target : index.terminator(BlockID(number)).targets()) {
            if (target.block() != block)
                continue;
            std::span<const ValueID> arguments = target.arguments();
            for (uint32_t place = 0; place < arguments.size(); ++place) {
                if (index.block(block).parameter(place) != value)
                    continue;
                if (!arguments[place] || !isAtLeastForLinkAccesses(index, arguments[place], bound, met))
                    return false;
                isReached = true;
            }
        }
    }
    return isReached;
}

static bool readsElementInBoundsForLinkAccesses(const Program& program, const BodyIndex& index, BodyValue element, ValueID list)
{
    ValueID counter = index.stripCasts(element.as<Opcode::ElemGet>().propertyOperand());
    Vector<ValueID, 8> met;
    if (!counter || !isAtLeastForLinkAccesses(index, counter, 0, met))
        return false;
    auto isList = [&](ValueID value) {
        VariableID variable = variableReadAtForLinkAccesses(index, value);
        return value == list || (variable && variable == variableReadAtForLinkAccesses(index, list) && program.at(variable).flags.contains(VariableFlags::IsImmutable));
    };
    auto isLength = [&](ValueID value) {
        value = value ? index.stripCasts(value) : ValueID();
        return value && !index.isParameter(value) && index.opcodeOf(value) == Opcode::PropGet && index.value(value).key() == program.keyOf(BuiltinNames::length) && isList(index.value(value).as<Opcode::PropGet>().objectOperand());
    };
    for (BlockID block = index.blockOf(element.id()); block && block != entryBlock; block = index.immediateDominator(block)) {
        std::optional<BodyIncomingEdge> edge = index.onlyIncomingEdge(block);
        if (!edge)
            continue;
        BodyTerminator end = index.terminator(edge->from);
        ValueID condition = end.isConditionalBranch() && end.taken().block() == block && end.notTaken().block() != block && end.operand(0) ? index.stripCasts(end.operand(0)) : ValueID();
        if (!condition || index.isParameter(condition) || index.opcodeOf(condition) != Opcode::Less)
            continue;
        BodyValue comparison = index.value(condition);
        if (comparison.operand(0) && index.stripCasts(comparison.operand(0)) == counter && isLength(comparison.operand(1)))
            return true;
    }
    return false;
}

std::optional<uint32_t> LinkFlow::findCallbackParameterArray(FunctionID id, uint32_t callbackParameter) const
{
    const Function& record = program.at(id);
    const BodyIndex* index = bodiesLookedAt(looks).find(id);
    bool isPlain = (record.kind == FunctionKind::Plain || record.kind == FunctionKind::Arrow) && !record.isCoroutine() && !record.calleeParameterIndex() && !record.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest });
    if (!index || !isPlain || callbackParameter >= record.numberOfPositionalParameters() || summaries.at(id).has(SummaryTrait::UsesArguments))
        return std::nullopt;
    BodyBlock entry = index->block(entryBlock);
    ValueID callback = entry.parameter(record.firstParameterIndex() + callbackParameter);
    ValueID list;
    uint32_t numberOfCalls = 0;
    Vector<ValueID, 4> elements;
    for (uint32_t number = 0; number < index->numberOfBlocks(); ++number) {
        for (BodyValue value : index->block(BlockID(number)).values()) {
            if (value.opcode() != Opcode::Call || value.as<Opcode::Call>().calleeOperand() != callback)
                continue;
            std::span<const ValueID> arguments = value.as<Opcode::Call>().argumentsOperands();
            ValueID element = arguments.empty() ? ValueID() : arguments[0];
            while (element && !index->isParameter(element) && index->hasOneUse(element) && opcodesThatStateTypes.contains(index->opcodeOf(element)))
                element = index->value(element).operand(0);
            if (!element || index->isParameter(element) || index->opcodeOf(element) != Opcode::ElemGet || !index->hasOneUse(element))
                return std::nullopt;
            ValueID from = index->value(element).as<Opcode::ElemGet>().objectOperand();
            if (!from || !index->isParameter(from) || index->blockOf(from) != entryBlock || (list && list != from))
                return std::nullopt;
            list = from;
            elements.append(element);
            ++numberOfCalls;
        }
    }
    if (!numberOfCalls || numberOfCalls >= BodyIndex::maxCountedUses || numberOfCalls != index->useCount(callback))
        return std::nullopt;

    uint32_t numberOfReads = numberOfCalls;
    for (uint32_t number = 0; number < index->numberOfBlocks(); ++number) {
        for (BodyValue value : index->block(BlockID(number)).values())
            numberOfReads += value.opcode() == Opcode::PropGet && value.as<Opcode::PropGet>().objectOperand() == list && value.key() == program.keyOf(BuiltinNames::length);
    }
    bool isOnlyRead = numberOfReads < BodyIndex::maxCountedUses && numberOfReads == index->useCount(list);
    if (!isOnlyRead || !std::ranges::all_of(elements, [&](ValueID element) { return readsElementInBoundsForLinkAccesses(program, *index, index->value(element), list); }))
        return std::nullopt;
    for (uint32_t parameter = 0; parameter < record.numberOfPositionalParameters(); ++parameter) {
        if (parameter != callbackParameter && entry.parameter(record.firstParameterIndex() + parameter) == list)
            return parameter;
    }
    return std::nullopt;
}

FlowNodeID AccessInLinkAccesses::findGatheredArguments() const
{
    if (!isRead() || m_shared || isNamed() || !m_access.site || !m_flow.summaries.at(m_function).has(SummaryTrait::UsesArguments))
        return { };
    const BodyIndex* index = bodiesLookedAt(m_flow.looks).find(m_function);
    if (!index || index->isParameter(m_access.site) || index->opcodeOf(m_access.site) != Opcode::ElemGet)
        return { };
    auto read = index->value(m_access.site).as<Opcode::ElemGet>();
    ValueID object = index->stripCasts(read.objectOperand());
    if (!object || index->isParameter(object))
        return { };
    BodyValue made = index->value(object);
    ValueID gathered;
    if (made.opcode() == Opcode::CreateUnmappedArguments)
        gathered = made.as<Opcode::CreateUnmappedArguments>().restOperand();
    else if (made.opcode() == Opcode::CreateMappedArguments)
        gathered = made.as<Opcode::CreateMappedArguments>().restOperand();
    if (!gathered || !index->isParameter(gathered) || index->blockOf(gathered) != entryBlock || gathered.index() >= m_program.at(m_function).numberOfEntryParameters())
        return { };

    PropertyKeyID length = m_program.keyOf(BuiltinNames::length);
    uint32_t numberOfReads = 0;
    for (uint32_t number = 0; number < index->numberOfBlocks(); ++number) {
        for (BodyValue value : index->block(BlockID(number)).values()) {
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (operand != object)
                    return;
                bool readsElement = value.opcode() == Opcode::ElemGet && operandIndex == OperandIndex::ElemGet::object;
                bool readsLength = value.opcode() == Opcode::PropGet && operandIndex == OperandIndex::PropGet::object && value.key() == length;
                numberOfReads += readsElement || readsLength;
            });
        }
    }
    if (numberOfReads >= BodyIndex::maxCountedUses || numberOfReads != index->useCount(object))
        return { };
    Vector<ValueID, 8> met;
    if (!isAtLeastForLinkAccesses(*index, read.propertyOperand(), static_cast<int64_t>(made.variadicOperands().size()), met))
        return { };
    return m_flow.parameter(m_function, gathered.index());
}

NEVER_INLINE void AccessInLinkAccesses::readGatheredArguments()
{
    FlowNodeID gathered = m_access.isToContents() ? FlowNodeID() : findGatheredArguments();
    if (!gathered || gathered == m_subject)
        return;
    tellAgainOnceClimbs(gathered);
    if (m_receiver == m_subject)
        m_receiver = gathered;
    m_subject = gathered;
}

void AccessInLinkAccesses::resolve()
{
    if (!m_access.key && isRead() && !m_shared) [[unlikely]]
        readGatheredArguments();
    const LinkType subject = m_flow.typeAt(m_subject);
    if (subject.isNever())
        return;
    if (!m_shared && m_access.storesExport)
        m_flow.note(LinkFlow::Note::ExportIsShown, m_flow.summaries.at(m_function).nodes[m_access.value].first);
    if (m_access.isToContents()) {
        resolveContents(subject);
        return;
    }
    if (isPrivateNameInLink(m_program, m_access.key)) {
        if (!isRead() && m_access.kind != Kind::Has && subject.classes != LinkType::Classes::None)
            m_flow.arrive(m_flow.contentsOf(subject, SlotOfContents::noLongerAsMade()), m_flow.fromTypeID(TypeTable::undefined));
        resolveUnderPrivateName();
        return;
    }
    if (!m_shared && m_access.isCopySpelledByLoop) {
        FlowNodeID source = m_flow.nodeOf(m_function, m_flow.sourceOfCopySpelledByLoop(m_function, m_access.site));
        tellAgainOnceClimbs(source);
        if (m_flow.followsCopySpelledByLoop(subject, m_flow.typeAt(source))) {
            m_flow.forEachLayoutOf(m_flow.typeAt(source), [&](LayoutID copied) {
                if (m_program.at(copied).kind != LayoutKind::Instance)
                    return;
                m_flow.build.statistics().count(StatisticID::InstancesCopiedByLoops);
                tellAgainOnceClimbsWhoeverTells(m_flow.extraPropertyStoreNode(copied));
            });
            mayThrow();
            return;
        }
        if (storesCopiedNamesOnPrototype(subject, m_flow.typeAt(source)))
            return;
    }
    if (!isNamed() && !readKey())
        return;
    if (!isNamed() && !m_keyIsOnlyNumbers && accessesPassedNames())
        return;
    if (!isNamed() && (readsOrStoresUnderTextsThatBodyWrites() || storesUnderCallbackKeys(subject) || accessesNamesFromLoop() || accessesListedNames(subject)))
        return;
    if (subject.builtin && m_flow.lightFormOf(subject.builtin) && !isServedByLightFormOf(subject.builtin))
        m_flow.noteLossOf(subject.builtin);

    if (m_access.kind == Kind::Has) {
        if (!isNamed())
            m_flow.noteKeysBecomeTextsAmong(subject);
        if (subject.builtin && (m_shared || !m_flow.isSpelledInBody(m_function, m_access.subject))) {
            BuiltinID member;
            LookupInLinkAccesses outcome = isNamed() ? lookUpFrom(subject.builtin, member) : m_keyCannotNameBuiltin ? LookupInLinkAccesses::Absent : LookupInLinkAccesses::Unknown;
            if (outcome == LookupInLinkAccesses::Unknown && isNamed() && m_nameClass != NameClass::Proto && isKeyAbsentFromChain(subject.builtin))
                outcome = LookupInLinkAccesses::Absent;
            if (outcome == LookupInLinkAccesses::Found)
                m_flow.note(LinkFlow::Note::BuiltinRead, member.index());
            else if (outcome == LookupInLinkAccesses::Unknown)
                m_flow.note(LinkFlow::Note::BuiltinUsedViaFlow, m_table.canonicalID(subject.builtin).index());
        }
        mayThrow();
        if (m_value)
            m_flow.arrive(m_value, primitivesForLinkAccesses(Primitive::Boolean));
        return;
    }

    if (!isRead() || subject.primitives & Primitive::UndefinedOrNull)
        mayThrow();
    if (m_receiver != m_subject && !isAboveHomeObjectOfClassForLinkAccesses(subject))
        m_flow.addEdge(m_receiver, m_flow.unseen());
    if (const BuiltinHostObject* object = subject == LinkType::anything() ? findHostObjectThatSubjectIsReadAs() : nullptr; object && giveWhatOtherNamesHold(*object)) {
        cannotTell(UnknownIs::ByEscapeAlone);
        return;
    }
    if (!isRead()) {
        LinkFlow::WhatIsStored stored = m_access.subjectIsMadeInBody || m_access.holdsNewValueOfNativeCode || m_access.laysTextNumberOrBooleanThatIsWritten || storesTextNumberOrBooleanThatIsWritten() ? LinkFlow::WhatIsStored::TextsNumbersAndBooleans : LinkFlow::WhatIsStored::Anything;
        if (isNamed() || !m_keyIsOnlyNumbers) {
            m_flow.noteWriteOn(subject, m_access.key, m_function, stored, m_access.subjectIsMadeInBody, isNamed() && m_access.kind == Kind::Store);
            if (m_access.kind == Kind::Define && (m_access.isByDescriptor || m_access.laysAccessorAsWritten))
                m_flow.noteAccessorMayBeLaidOn(subject, m_access.key, m_function);
            if (!m_shared && m_access.isSpelledByCopy)
                noteSiteOfCopyThatSpellsStore();
            else if (!m_shared)
                m_flow.noteSiteOfWrite(m_function, m_access.site, m_subject);
        } else
            m_flow.noteWhatIsStoredOn(subject, stored);
    }
    if (!isNamed() && !m_keyIsOnlyNumbers && m_access.kind != Kind::Define && !membersOfObjectPrototypeAreWhatTableLists() && !isFinalListRead())
        m_flow.addEdge(m_subject, m_flow.unseen());
    if ((subject.hasUnknownObjects && !m_before.hasUnknownObjects) || (subject.hasDirectUnknownObjects() && !m_before.hasDirectUnknownObjects()))
        cannotTell(subject.hasDirectUnknownObjects() ? UnknownIs::InSight : UnknownIs::ByEscapeAlone);
    if (isRead() && m_value && subject.holdsLostBuiltins && isNamed()) {
        m_flow.note(LinkFlow::Note::ReadThroughLostBuiltins, m_access.key.bits());
        LinkType found = LinkType::anything();
        found.holdsLostBuiltins = true;
        give(found);
    }
    bool hasSaidAllOfPrimitives = subject.hasUnknownObjects && (m_keyIsWritten || !m_before.hasUnknownObjects);
    if (!hasSaidAllOfPrimitives)
        resolveOnPrimitives(static_cast<uint16_t>(subject.primitives & ~m_before.primitives));

    if (subject.objects != m_before.objects || subject.layout != m_before.layout || subject.family != m_before.family) {
        std::optional<SetForScope<FlowNodeID>> valueThatDescriptorLays;
        if (m_access.kind == Kind::Define && (subject.objects == LinkType::Objects::Layout || subject.objects == LinkType::Objects::Set || subject.objects == LinkType::Objects::Family)) {
            if (FlowNodeID laid = findValueThatWrittenDescriptorLays())
                valueThatDescriptorLays.emplace(m_value, laid);
            else if (changeLaysAccessorInSight() || changeLaysWhatDescriptorsInSightHold())
                valueThatDescriptorLays.emplace(m_value, FlowNodeID());
        }
        switch (subject.objects) {
        case LinkType::Objects::None:
            break;
        case LinkType::Objects::Layout:
            resolveOnLayout(subject.layout, subject);
            break;
        case LinkType::Objects::Set:
            resolveOnSet(subject);
            break;
        case LinkType::Objects::Prototype:
            resolveOnPrototype(subject.layout);
            break;
        case LinkType::Objects::Prototypes:
            resolveOnPrototypes(subject.family);
            break;
        case LinkType::Objects::Family:
            resolveOnFamily(subject.family);
            break;
        case LinkType::Objects::Lost:
            resolveOnLostObjects(subject.layout);
            break;
        }
    }

    auto holdsLostFunctions = [](const LinkType& type) {
        return type.functions == LinkType::Functions::Set || type.functions == LinkType::Functions::Lost;
    };
    auto resolvedEachBefore = [&] {
        return subject.functions == LinkType::Functions::Lost && m_before.functions == LinkType::Functions::Set && !m_shared
            && (m_flow.readsEachFunction(m_access) || m_flow.isPrototypeReadByInstanceOf(m_function, m_access));
    };
    bool functionsAreNew = subject.functions != m_before.functions || subject.function != m_before.function || subject.constructorsOf != m_before.constructorsOf;
    bool isOfSet = functionsAreNew && subject.functions == LinkType::Functions::Set && !m_shared;
    if (isOfSet && m_flow.readsEachFunction(m_access)) {
        for (FunctionID member : m_flow.functionsOfSet(subject))
            resolveOnClosure(member);
    } else if (isOfSet && m_flow.isPrototypeReadByInstanceOf(m_function, m_access)) {
        giveAnything();
        for (FunctionID member : m_flow.functionsOfSet(subject)) {
            if (!m_flow.layoutOfConstructorFunction(member))
                resolveOnClosure(member);
        }
    } else if (functionsAreNew && (!(holdsLostFunctions(subject) && holdsLostFunctions(m_before)) || resolvedEachBefore())) {
        switch (subject.functions) {
        case LinkType::Functions::None:
            break;
        case LinkType::Functions::Closure: {
            std::optional<SetForScope<FlowNodeID>> valueThatDescriptorLays;
            FlowNodeID laid = m_access.kind == Kind::Define && m_flow.isPlainClosureOrConstructorFunction(subject.function) ? findValueThatWrittenDescriptorLays() : FlowNodeID();
            if (laid)
                valueThatDescriptorLays.emplace(m_value, laid);
            resolveOnClosure(subject.function);
            break;
        }
        case LinkType::Functions::Constructors:
            resolveOnConstructors(subject.constructorsOf);
            break;
        case LinkType::Functions::Set:
        case LinkType::Functions::Lost:
            resolveOnLostFunctions();
            break;
        }
    }

    if (subject.classes != LinkType::Classes::None && !subject.holdsSameInstancesAs(m_before))
        resolveOnClasses(instancesAmongForLinkAccesses(subject));

    if (subject.builtin && subject.builtin != m_before.builtin) {
        SetForScope flowBroughtHolder(m_flowBroughtHolder, m_shared || (!m_access.readsPrototypeOfHeritage && !m_flow.isSpelledInBody(m_function, m_access.subject)));
        if (m_flowBroughtHolder && (m_shared || (!tellsMemberThatTableFindsFrom(subject.builtin) && !isServedByLightFormOf(subject.builtin))))
            m_flow.note(LinkFlow::Note::BuiltinUsedViaFlow, m_table.canonicalID(subject.builtin).index());
        resolveOnBuiltin(subject.builtin);
    }
}

void AccessInLinkAccesses::resolveUnderPrivateName()
{
    if (!m_before.isNever())
        return;
    mayThrow();
    if (m_access.kind == Kind::Has) {
        if (m_value)
            m_flow.arrive(m_value, primitivesForLinkAccesses(Primitive::Boolean));
        return;
    }
    LayoutID owner = m_program.at(m_access.key).owner;
    std::optional<FieldReference> field = owner ? findField(m_program, owner, m_access.key) : std::nullopt;
    if (!field || !fieldAt(m_program, *field).form.isEmpty()) {
        cannotTellWhatRuns();
        return;
    }
    if (isRead()) {
        giveWhatIsHeldAt(m_flow.field(*field));
        giveWhatIsEstablishedOf(fieldAt(m_program, *field).declared);
        return;
    }
    storeInto(m_flow.field(*field));
    if (!m_access.isPartOfConstruction)
        noteWrittenAfterConstruction(*field);
}

void AccessInLinkAccesses::resolveOnLayout(LayoutID id, const LinkType& pooled)
{
    if (isRejectedByTest(id))
        return;
    const Layout& layout = m_program.at(id);
    switch (layout.kind) {
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
    case LayoutKind::BuiltinResult:
        break;
    case LayoutKind::ModuleNamespace:
        resolveOnNamespace(id, layout);
        return;
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
        m_flow.escape(id);
        cannotTell();
        return;
    }

    if (m_flow.escapedLayouts.get(id.index()))
        cannotTell(UnknownIs::ByEscapeAlone);
    if (!isNamed()) {
        if (isRead())
            m_flow.noteKeysBecomeTexts(id);
        if (mayStoreIntoPrototypeLink())
            m_flow.escape(id);
        mayThrow();
        if (KeyAffixes affixes = affixesOfKeyRead(); affixes && m_flow.tieReadUnderAffixes(m_value, id, affixes))
            return;
        if (isRead() && m_keyIsLiteral && !namesOnlyElements() && (m_flow.hasPristinePrototypes(id) || m_flow.hasFollowedPrototypes(id)))
            giveWhatIsHeldAt(m_flow.literalKeyReadOf(id));
        else if (isRead())
            giveWhatIsHeldAt(m_flow.anyFieldReadThrough(pooled.objects, pooled.layout, pooled.family, namesOnlyElements()));
        else if (m_keyIsSymbolAsFarAsIsSeen) {
            m_flow.escape(id);
            storeIntoUnseen();
        } else
            storeInto(m_flow.anyFieldStoredThrough(pooled.objects, pooled.layout, pooled.family, m_keyIsOnlyNumbers));
        return;
    }
    if (m_nameClass == NameClass::Proto) {
        resolvePrototypeLinkOfLayout(id, layout);
        return;
    }

    bool isDefinition = m_access.kind == Kind::Define;
    if (!isDefinition && m_flow.namesMayBeIncomplete(id, ClassSide::Prototype)) {
        m_flow.escape(id);
        cannotTell();
    }

    std::optional<FieldReference> field = findField(m_program, id, m_access.key);
    if (field && fieldAt(m_program, *field).form.contains(FieldForm::IsAccessor)) {
        m_flow.escape(id);
        cannotTell();
        return;
    }

    LinkFlow::FoundMembers members = m_flow.findMembers(id, ClassSide::Prototype, m_access.key);
    bool findsAccessor = false;
    bool findsGetter = findsGetterAmong(members);
    for (const LinkFlow::FoundMember& found : members) {
        FlowNodeID member = m_flow.member(found.classLayout, ClassSide::Prototype, found.index);
        switch (found.kind) {
        case MethodKind::Method:
            if (isRead())
                giveFrom(member);
            else if (!field)
                storeInto(member);
            break;
        case MethodKind::Getter:
            findsAccessor = true;
            if (isRead())
                enterAccessorAt(member);
            break;
        case MethodKind::Setter:
            findsAccessor = true;
            if (m_access.kind == Kind::Store)
                enterAccessorAt(member);
            if (!findsGetter)
                givePrimitives(Primitive::Undefined);
            break;
        }
    }

    bool isPrivateName = m_program.at(m_access.key).kind == PropertyKeyKind::PrivateName;
    if (!field && !isDefinition && !isPrivateName && layout.has(LayoutFlags::IsOfCreatedObject) && resolveOnWhatStandsAbove(m_flow.whatStandsAbove(id), LinkFlow::mostStepsAboveCreatedObject))
        return;
    bool goesOnAboveClasses = members.lookupGoesOnAboveClasses() && !isPrivateName;
    FlowNodeID foundOnPrototypes;
    if (!isPrivateName && m_flow.isBelowFollowedPrototype(id)) {
        if (isRead())
            foundOnPrototypes = m_flow.foundOnPrototypesAbove(id, m_access.key, LinkFlow::AccessorsAbove::AreEnteredByCaller);
        else if (!isDefinition)
            m_flow.lookForSetterOnPrototypesAbove(id, m_access.key, LinkFlow::AccessorsAbove::AreEnteredByCaller);
        else
            m_flow.letAccessorsOnPrototypesAboveEscape(id, m_access.key);
        if (!isDefinition)
            enterAccessorsOnPrototypesAbove(id);
    }

    if (isRead()) {
        giveFrom(m_flow.foundBesideFieldsFor(id, m_access.key));
        if (foundOnPrototypes)
            giveFrom(foundOnPrototypes);
        if (field) {
            giveWhatIsHeldAt(m_flow.field(*field));
            giveWhatIsEstablishedOf(fieldAt(m_program, *field).declared);
        } else
            giveFrom(m_flow.appendedField(id, m_access.key));
        if (!goesOnAboveClasses)
            return;
        if (field && field->layout == id && !m_flow.mayReadInFrontOfDefinition(m_function, m_access, id) && m_flow.isAbsentFromObjectPrototype(m_access.key)) {
            giveFrom(m_flow.foundWhereFieldIsEmpty(*field));
            return;
        }
        bool mayFindClassAbove = m_nameClass == NameClass::Constructor && m_flow.isBelowConstructorFunction(id);
        if (mayFindClassAbove)
            giveConstructorsOfRowAbove(id);
        if (m_nameClass == NameClass::Constructor && layout.kind == LayoutKind::Instance && layout.constructor && !mayFindClassAbove)
            give(closureForLinkAccesses(layout.constructor));
        else if (BuiltinID member; !field && isExcludedByInstanceOf(id) && lookUpFrom(Builtins::ObjectPrototype, member) == LookupInLinkAccesses::Absent)
            return;
        else if (!giveWhatIsFoundFrom(Builtins::ObjectPrototype))
            m_flow.escape(id);
        return;
    }

    if (m_access.kind == Kind::Store && goesOnAboveClasses && mayRunCodeFoundFrom(Builtins::ObjectPrototype)) {
        m_flow.escape(id);
        storeIntoUnseen();
    }
    if (field) {
        storeInto(m_flow.field(*field));
        if (!m_flow.isPartOfConstructionOf(id, m_function, m_access))
            noteWrittenAfterConstruction(*field);
        return;
    }
    if (findsAccessor && !isDefinition && !goesOnAboveClasses) {
        storeInto(m_flow.storedWhereAccessorStandsAbove(id));
        return;
    }
    if (findsAccessor && isDefinition)
        m_flow.escape(id);
    storeBesideFieldsOf(id);
}

void AccessInLinkAccesses::resolvePrototypeLinkOfLayout(LayoutID id, const Layout& layout)
{
    switch (m_access.kind) {
    case Kind::Read:
        if (!m_flow.findsPrototypeLinkOfSubject(id, ClassSide::Prototype, m_access.key, m_receiver != m_subject)) {
            m_flow.escape(id);
            cannotTell();
            return;
        }
        if (layout.kind == LayoutKind::Instance)
            give(prototypeForLinkAccesses(id));
        else
            giveAnything();
        giveFrom(m_flow.foundBesideFieldsFor(id, m_access.key));
        giveFrom(m_flow.appendedField(id, m_access.key));
        return;
    case Kind::Store:
        noteChange(id, LayoutChange::ChangesPrototype);
        m_flow.escape(id);
        cannotTell();
        return;
    case Kind::Define:
        storeBesideFieldsOf(id);
        return;
    case Kind::Has:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void AccessInLinkAccesses::resolveOnNamespace(LayoutID id, const Layout& layout)
{
    mayThrow();
    if (layout.namespaceOf && m_program.at(layout.namespaceOf).kind == ModuleKind::RuntimeSource)
        m_flow.escape(id);
    if (!isRead()) {
        storeIntoUnseen();
        return;
    }
    if (!layout.namespaceOf || m_program.at(layout.namespaceOf).kind == ModuleKind::RuntimeSource) {
        giveAnything();
        return;
    }
    if (isNamed()) {
        if (std::optional<uint32_t> field = layout.findField(m_access.key))
            giveExport(layout.namespaceOf, *field);
        else
            giveWhatNamespaceDoesNotList(id, layout.namespaceOf);
        return;
    }
    uint32_t numberOfFields = static_cast<uint32_t>(layout.fields.size());
    for (uint32_t field = 0; field < numberOfFields; ++field)
        giveExport(layout.namespaceOf, field);
    if (std::ranges::any_of(m_program.at(layout.namespaceOf).requests, &ModuleRequest::isDynamicStarExport))
        m_flow.escape(id);
    giveAnything();
}

void AccessInLinkAccesses::giveWhatNamespaceDoesNotList(LayoutID id, ModuleID module)
{
    const PropertyKey& key = m_program.at(m_access.key);
    auto cannotFollow = [&] {
        if (std::ranges::any_of(m_program.at(module).requests, &ModuleRequest::isDynamicStarExport))
            m_flow.escape(id);
        giveAnything();
    };
    if (key.kind != PropertyKeyKind::Text || m_access.key == m_program.keyOf(BuiltinNames::__esModule) || !refusedCodeOf(m_program).codeOutsideImage || m_flow.testRuns().isTestRun()) {
        cannotFollow();
        return;
    }
    givePrimitives(Primitive::Undefined);
    if (m_access.key == m_program.keyOf(BuiltinNames::defaultKeyword))
        return;
    for (const ModuleRequest& request : m_program.at(module).requests) {
        if (!request.isDynamicStarExport)
            continue;
        const Module& facade = m_program.at(request.target);
        auto listed = std::ranges::find(facade.syntheticExports, key.text, &SyntheticExport::name);
        if (!commonJsModuleCapturedBy(m_program, facade) || listed == facade.syntheticExports.end() || !listed->variable) {
            cannotFollow();
            return;
        }
        giveWhatIsHeldAt(m_flow.variable(listed->variable));
    }
}

void AccessInLinkAccesses::giveExport(ModuleID module, uint32_t index)
{
    m_flow.noteExportRead(module, index);
    for (size_t steps = 0; steps <= m_program.modules.size(); ++steps) {
        const auto& exports = m_program.at(module).exports;
        if (index >= exports.size())
            break;
        const ModuleExport& entry = exports[index];
        if (m_flow.testRuns().mayComeThroughMock(module, index)) {
            giveAnything();
            return;
        }
        if (entry.isHiddenUntilInitialized)
            break;
        if (isNamed() && entry.isFromStarExport && m_program.at(module).requests.containsIf([](const ModuleRequest& request) { return request.isDynamicStarExport; }))
            givePrimitives(Primitive::Undefined);
        switch (entry.kind) {
        case ModuleExportKind::Variable:
            if (entry.variable)
                giveWhatIsHeldAt(m_flow.variable(entry.variable));
            else
                giveAnything();
            return;
        case ModuleExportKind::Namespace: {
            LayoutID whole = entry.module && entry.phase == ImportPhase::Evaluation ? m_program.layoutOfWholeNamespace(entry.module) : LayoutID();
            if (whole)
                give(instanceForLinkAccesses(whole));
            else {
                if (entry.module)
                    m_flow.noteLossOfObjectOfBuiltinModule(entry.module);
                giveAnything();
            }
            return;
        }
        case ModuleExportKind::Indirect: {
            if (!entry.module || !entry.importName) {
                giveAnything();
                return;
            }
            m_flow.noteLossOfExportOfBuiltinModule(entry.module, entry.importName);
            const auto& exportsOfTarget = m_program.at(entry.module).exports;
            uint32_t found = 0;
            while (found < exportsOfTarget.size() && exportsOfTarget[found].name != entry.importName)
                ++found;
            module = entry.module;
            index = found;
            break;
        }
        case ModuleExportKind::Field:
            giveAnything();
            return;
        }
    }
    if (LayoutID whole = m_program.layoutOfWholeNamespace(module); whole && std::ranges::any_of(m_program.at(module).requests, &ModuleRequest::isDynamicStarExport))
        m_flow.escape(whole);
    giveAnything();
}

bool AccessInLinkAccesses::wasNamedBefore(LayoutID layout) const
{
    switch (m_before.objects) {
    case LinkType::Objects::Layout:
        return m_before.layout == layout;
    case LinkType::Objects::Set:
        return std::ranges::contains(m_flow.layoutsOfSet(m_before), layout);
    case LinkType::Objects::None:
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
    case LinkType::Objects::Family:
    case LinkType::Objects::Lost:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void AccessInLinkAccesses::resolveOnSet(const LinkType& subject)
{
    std::span<const LayoutID> members = m_flow.layoutsOfSet(subject);
    bool poolOfSetWouldLetNamespaceEscape = std::ranges::any_of(members, [&](LayoutID member) {
        return m_program.at(member).kind == LayoutKind::ModuleNamespace;
    });
    for (LayoutID member : members) {
        if (!wasNamedBefore(member))
            resolveOnLayout(member, poolOfSetWouldLetNamespaceEscape ? instanceForLinkAccesses(member) : subject);
    }
}

LinkFlow::Lays AccessInLinkAccesses::howItLays(bool isPartOfConstruction) const
{
    switch (m_access.kind) {
    case Kind::Store:
        return isPartOfConstruction ? LinkFlow::Lays::AssignsInConstruction : LinkFlow::Lays::Assigns;
    case Kind::Define:
        return isPartOfConstruction ? LinkFlow::Lays::DefinesInConstruction : LinkFlow::Lays::Defines;
    case Kind::Read:
    case Kind::Has:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void AccessInLinkAccesses::resolveOnFamily(FamilyID family)
{
    mayThrow();
    if (isNamed()) {
        if (isRead() && m_access.isPartOfConstruction) {
            bool mayComeInFrontOfDefinition = false;
            m_program.at(m_flow.families.layoutsOf(family)).forEach([&](LayoutID member) {
                mayComeInFrontOfDefinition |= m_flow.mayReadInFrontOfDefinition(m_function, m_access, member);
            });
            if (mayComeInFrontOfDefinition)
                givePrimitives(Primitive::Undefined);
        }
        if (LayoutID tested = classTestedByInstanceOf()) {
            if (auto read = m_flow.findGuardedFamilyPropertyRead(family, m_access.key, tested)) {
                giveWhatIsHeldAt(read->fields);
                giveWhatIsHeldAt(read->besideFields);
                return;
            }
        }
        if (LayoutID refused = classRejectedByTest(); refused && m_flow.everyMemberStandsAtOrBelow(family, refused))
            return;
        if (isRead())
            giveWhatIsHeldAt(m_flow.fieldReadThrough(family, m_access.key));
        else
            storeInto(m_flow.fieldStoredThrough(family, m_access.key, howItLays(m_flow.isPartOfConstructionOfAllOf(family, m_function, m_access))));
        return;
    }
    if (isRead()) {
        giveWhatIsHeldAt(m_flow.anyFieldReadThrough(LinkType::Objects::Family, LayoutID(), family, namesOnlyElements()));
        return;
    }
    if (mayStoreIntoPrototypeLink() || m_keyIsSymbolAsFarAsIsSeen)
        m_flow.escapeAllOf(familyForLinkAccesses(family));
    if (m_keyIsSymbolAsFarAsIsSeen) {
        storeIntoUnseen();
        return;
    }
    storeInto(m_flow.anyFieldStoredThrough(LinkType::Objects::Family, LayoutID(), family, m_keyIsOnlyNumbers));
}

void AccessInLinkAccesses::resolveOnLostObjects(LayoutID member)
{
    mayThrow();
    if (isNamed()) {
        if (isRead() && m_access.isPartOfConstruction)
            givePrimitives(Primitive::Undefined);
        if (isRead())
            giveWhatIsHeldAt(m_flow.fieldReadThroughLost(member, m_access.key));
        else
            storeInto(m_flow.fieldStoredThroughLost(member, m_access.key, howItLays(false)));
    } else if (isRead())
        giveWhatIsHeldAt(m_flow.anyFieldReadThrough(LinkType::Objects::Lost, member, FamilyID(), namesOnlyElements()));
    else if (m_keyIsSymbolAsFarAsIsSeen) {
        LinkType lost;
        lost.objects = LinkType::Objects::Lost;
        lost.layout = member;
        m_flow.escapeAllOf(lost);
        storeIntoUnseen();
    }
    else
        storeInto(m_flow.anyFieldStoredThrough(LinkType::Objects::Lost, member, FamilyID()));
}

void AccessInLinkAccesses::resolveOnPrototype(LayoutID id)
{
    const Layout& layout = m_program.at(id);
    tellAgainOnceClimbs(extraPropertyReadNode(id));
    if (m_flow.escapedLayouts.get(id.index()))
        cannotTell(UnknownIs::ByEscapeAlone);
    if (!isNamed() && m_access.kind == Kind::Define && m_value == m_flow.unseen() && m_flow.isOfConstructorFunction(id)) {
        bool keyIsSomeSymbol = !m_shared && m_access.keyNode != noSummaryNode && LinkFlow::holdsSymbolsAlone(m_flow.typeAt(m_flow.nodeOf(m_function, m_access.keyNode)));
        m_flow.defineOnPrototypeOf(id, { }, keyIsSomeSymbol ? LinkFlow::UnknownKey::IsSomeSymbol : LinkFlow::UnknownKey::IsAnyKey);
        return;
    }
    if (!isNamed()) {
        letPrototypeEscape(id);
        cannotTell();
        return;
    }

    if (m_nameClass == NameClass::Proto) {
        if (isRead() && !m_flow.findsPrototypeLinkOfSubject(id, ClassSide::Prototype, m_access.key, m_receiver != m_subject)) {
            letPrototypeEscape(id);
            cannotTell();
            return;
        }
        if (isRead()) {
            if (layout.base) {
                give(prototypeForLinkAccesses(layout.base));
                giveFrom(extraPropertyReadNode(id));
            }
            if (!layout.base && layout.has(LayoutFlags::HasHeritage) && !layout.writtenBuiltinBase)
                letPrototypeEscape(id);
            if (!layout.base || m_flow.mayNotBeLinkedYetBelow(id, layout.base))
                giveAnything();
            return;
        }
        if (m_access.kind == Kind::Store && m_value && m_flow.isOfConstructorFunction(id) && m_flow.findsPrototypeLinkOfSubject(id, ClassSide::Prototype, m_access.key, m_receiver != m_subject)) {
            tellAgainOnceClimbs(m_value);
            if (m_flow.namesParentOfPrototype(m_flow.typeAt(m_value), id))
                return;
        }
        noteChange(id, LayoutChange::ChangesPrototype);
        letPrototypeEscape(id);
        cannotTell();
        return;
    }

    bool isDefinition = m_access.kind == Kind::Define;
    if (!isDefinition && m_flow.namesMayBeIncomplete(id, ClassSide::Prototype)) {
        letPrototypeEscape(id);
        cannotTell();
    }

    LinkFlow::FoundMembers members = m_flow.findMembers(id, ClassSide::Prototype, m_access.key);
    layOnReceiverUnlessAccessorAnswers(members);
    bool isPrivateName = m_program.at(m_access.key).kind == PropertyKeyKind::PrivateName;
    if (!isDefinition && !isPrivateName && m_flow.isBelowFollowedPrototype(id)) {
        if (!isRead())
            m_flow.lookForSetterOnPrototypesAbove(id, m_access.key, LinkFlow::AccessorsAbove::AreEnteredByCaller);
        else if (FlowNodeID foundOnPrototypes = m_flow.foundOnPrototypesAbove(id, m_access.key, LinkFlow::AccessorsAbove::AreEnteredByCaller))
            giveFrom(foundOnPrototypes);
        enterAccessorsOnPrototypesAbove(id);
    }
    bool definesFollowedName = m_flow.isOfConstructorFunction(id) || (!isRead() && m_flow.followsNamesOnPrototypeOf(id) && m_nameClass != NameClass::Constructor && members.lookupGoesOnAboveClasses() && !m_flow.instancesMayHoldName(id, m_access.key));
    if (!isRead() && !isPrivateName && m_receiver == m_subject && definesFollowedName) {
        bool findsAccessor = std::ranges::any_of(members, [](const LinkFlow::FoundMember& found) {
            return found.kind != MethodKind::Method;
        });
        if (!isDefinition && (findsAccessor || (members.lookupGoesOnAboveClasses() && mayRunCodeFoundFrom(Builtins::ObjectPrototype)))) {
            letPrototypeEscape(id);
            storeIntoUnseen();
            return;
        }
        if (isDefinition && m_access.laysAccessorAsWritten && m_flow.isDefinedByDescriptorSomewhere(m_access.key))
            defineWrittenPrototypeAccessor(id);
        else if (isDefinition && m_value == m_flow.unseen())
            m_flow.defineOnPrototypeOf(id, m_access.key);
        else
            storeInto(m_flow.storedOnPrototypeOf(id, m_access.key, isStoreByBodyOfFileInItsOwnText(), isDefinition, isDefinition && m_access.isByDescriptor));
        return;
    }
    bool goesOnAboveClasses = members.lookupGoesOnAboveClasses();
    if (goesOnAboveClasses && isRead())
        giveFrom(m_flow.foundBesideFieldsFor(id, m_access.key));
    if (goesOnAboveClasses && isRead() && !isPrivateName) {
        if (m_nameClass == NameClass::Constructor && m_flow.isBelowConstructorFunction(id))
            giveConstructorsOfRowAbove(id);
        if (m_nameClass != NameClass::Constructor || m_flow.isBelowConstructorFunction(id)) {
            if (!giveWhatIsFoundFrom(Builtins::ObjectPrototype))
                letPrototypeEscape(id);
        } else if (layout.constructor)
            give(closureForLinkAccesses(layout.constructor));
        else
            giveAnything();
    } else if (goesOnAboveClasses && !isRead()) {
        letPrototypeEscape(id);
        storeIntoUnseen();
    }

    bool findsGetter = findsGetterAmong(members);
    for (const LinkFlow::FoundMember& found : members) {
        FlowNodeID member = m_flow.member(found.classLayout, ClassSide::Prototype, found.index);
        switch (found.kind) {
        case MethodKind::Method:
            giveFrom(member);
            storeInto(member);
            break;
        case MethodKind::Getter:
            if (isRead())
                enterAccessorAt(member);
            break;
        case MethodKind::Setter:
            if (m_access.kind == Kind::Store)
                enterAccessorAt(member);
            if (!findsGetter)
                givePrimitives(Primitive::Undefined);
            break;
        }
        if (isDefinition && found.kind != MethodKind::Method) {
            letPrototypeEscape(id);
            letPrototypeEscape(found.classLayout);
            storeIntoUnseen();
        }
    }
}

void AccessInLinkAccesses::resolveOnPrototypes(FamilyID family)
{
    if (resolveThroughSharedAccess(SharedAccess::Subjects::PrototypeObjects, family))
        return;
    m_program.at(m_flow.families.layoutsOf(family)).forEach([&](LayoutID layout) {
        resolveOnPrototype(layout);
    });
}

bool AccessInLinkAccesses::resolveThroughSharedAccess(SharedAccess::Subjects subjects, FamilyID family)
{
    if (m_receiver != m_subject)
        return false;
    LinkFlow::LockerOfWhatIsShared locker(m_flow);

    StateOfLinkAccesses& state = m_parts.state;
    bool valueIsUnseen = m_value == m_flow.unseen();
    std::pair<uint64_t, uint32_t> place {
        static_cast<uint64_t>(family.index()) << 32 | (isNamed() ? m_access.key.index() : 0),
        static_cast<uint32_t>(subjects) | static_cast<uint32_t>(m_access.kind) << 1 | isNamed() << 3 | !!m_value << 4 | valueIsUnseen << 5
            | m_access.storesObjectMadeForIt << 6 | m_keyIsOnlyNumbers << 7 | m_keyMayNamePrototypeLink << 8 | isLibraryCode(m_program, m_program.at(m_function)) << 9 | m_keyCannotNameBuiltin << 10 | m_access.setsFlagsAlone << 11,
    };
    auto found = state.indexesOfSharedAccesses.add(place, static_cast<uint32_t>(state.sharedAccesses.size()));
    uint32_t index = found.iterator->value;
    if (found.isNewEntry) {
        auto made = makeUniqueWithoutFastMallocCheck<SharedAccess>();
        made->subjects = subjects;
        made->firstFunction = m_function;
        made->access.kind = m_access.kind;
        made->access.key = m_access.key;
        made->access.storesObjectMadeForIt = m_access.storesObjectMadeForIt;
        made->access.setsFlagsAlone = m_access.setsFlagsAlone;
        made->keyIsOnlyNumbers = m_keyIsOnlyNumbers;
        made->keyCannotNameBuiltin = m_keyCannotNameBuiltin;
        made->keyMayNamePrototypeLink = m_keyMayNamePrototypeLink;
        made->subject = m_flow.makeSharedNode();
        if (m_value)
            made->value = valueIsUnseen ? m_flow.unseen() : m_flow.makeSharedNode();
        made->thrown = m_flow.makeSharedNode();
        state.sharedAccesses.append(WTF::move(made));
        const SharedAccess& shared = *state.sharedAccesses[index];
        bool isOnClassObjects = subjects == SharedAccess::Subjects::ClassObjects;
        m_flow.arrive(shared.subject, isOnClassObjects ? constructorsForLinkAccesses(family) : prototypesForLinkAccesses(family));

        uint32_t firstMember = static_cast<uint32_t>(state.membersOfSharedAccesses.size());
        m_program.at(m_flow.families.layoutsOf(family)).forEach([&](LayoutID layout) {
            FunctionID constructor = m_program.at(layout).constructor;
            if (!isOnClassObjects || (constructor && m_flow.classOfConstructor(constructor) == layout))
                state.membersOfSharedAccesses.append(MemberOfSharedAccessInLinkAccesses { index, layout });
        });
        uint32_t endOfMembers = static_cast<uint32_t>(state.membersOfSharedAccesses.size());
        for (uint32_t member = firstMember; member < endOfMembers; ++member)
            AccessInLinkAccesses(m_flow, m_parts, shared, member, true).resolveMemberOfSharedAccess();
    }

    const SharedAccess& shared = *state.sharedAccesses[index];
    if (!valueIsUnseen) {
        giveFrom(shared.value);
        storeInto(shared.value);
    }
    m_flow.addEdge(shared.thrown, thrownNode());
    return true;
}

void AccessInLinkAccesses::resolveMemberOfSharedAccess()
{
    LayoutID layout = m_parts.state.membersOfSharedAccesses[m_record].layout;
    switch (m_shared->subjects) {
    case SharedAccess::Subjects::PrototypeObjects:
        resolveOnPrototype(layout);
        return;
    case SharedAccess::Subjects::ClassObjects:
        resolveOnClosure(m_program.at(layout).constructor);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void AccessInLinkAccesses::resolveOnClosure(FunctionID function)
{
    const Function& record = m_program.at(function);
    LayoutID classLayout = record.constructedClass;
    if (m_nameClass == NameClass::Prototype && isNamed() && isRead() && classLayout && isClassConstructorOf(record.kind)) {
        give(prototypeForLinkAccesses(classLayout));
        return;
    }
    tellAgainOnceClimbsWhoeverTells(m_flow.foundOnClosuresOf(function));
    if (isRead() && m_access.key == m_program.keyOf(BuiltinNames::name))
        m_flow.noteNameBecomesText(function);
    if (m_flow.escapedFunctions.get(function.index()))
        cannotTell(UnknownIs::ByEscapeAlone);
    bool isResolvedOnClassObject = isNamed() && m_nameClass != NameClass::Prototype && m_nameClass != NameClass::Proto && classLayout && !m_flow.isOfConstructorFunction(classLayout);
    if (!isResolvedOnClassObject)
        layOnReceiverUnlessAccessorAnswers(LinkFlow::FoundMembers());
    if (!isNamed()) {
        bool findsNoMember = isRead() && (!classLayout || m_flow.isOfConstructorFunction(classLayout)) && namesNothingOnPlainClosures();
        if (!findsNoMember)
            letClassObjectEscape(function, classLayout);
        cannotTell();
        return;
    }

    if (m_nameClass == NameClass::Prototype) {
        if (LayoutID constructed = m_flow.layoutOfConstructorFunction(function)) {
            resolvePrototypePropertyOf(function, constructed);
            return;
        }
        if (m_access.kind == Kind::Store && m_receiver == m_subject && !m_shared && !classLayout && record.takesThisFromConstruction() && !record.isCoroutine())
            return;
        if (!isRead() && m_access.kind == Kind::Store && m_value && m_flow.mayHoldOtherClassPrototype(function)) {
            mayThrow();
            storeInto(m_flow.laidOnClosuresOf(function, m_access.key));
            return;
        }
        m_flow.escape(function);
        if (classLayout) {
            if (!isRead())
                noteChange(classLayout, LayoutChange::ChangesPrototype);
            m_flow.escape(classLayout);
        }
        cannotTell();
        return;
    }

    if (m_nameClass == NameClass::Proto) {
        LayoutID classOfClassObject = isClassConstructorOf(record.kind) ? classLayout : LayoutID();
        if (!isRead() || !m_flow.findsPrototypeLinkOfSubject(classOfClassObject, ClassSide::Static, m_access.key, m_receiver != m_subject)) {
            letClassObjectEscape(function, classLayout);
            cannotTell();
            return;
        }
        LayoutID base = classOfClassObject ? m_program.at(classOfClassObject).base : LayoutID();
        if (base && m_program.at(base).constructor) {
            give(closureForLinkAccesses(m_program.at(base).constructor));
            giveFrom(m_flow.unseenReplacementNode(classLayout));
        } else {
            std::optional<unsigned> ownCallee = m_program.at(m_function).calleeParameterIndex();
            bool isReadBySuperCall = !m_shared && ownCallee && m_flow.summaries.at(m_function).nodes[m_access.subject] == SummaryNode { SummaryNode::Kind::Parameter, *ownCallee, 0 };
            if (!isReadBySuperCall && classOfClassObject && m_program.at(classOfClassObject).has(LayoutFlags::HasHeritage) && !m_program.at(classOfClassObject).writtenBuiltinBase)
                letClassObjectEscape(function, classLayout);
            giveAnything();
        }
        return;
    }

    if (classLayout && !m_flow.isOfConstructorFunction(classLayout)) {
        resolveOnClassObject(function, classLayout);
        return;
    }
    if (isTakenByLoader())
        giveFrom(foundBesideFieldsUnderKey());
    if (mayRunCodeFoundAboveClosures())
        m_flow.escape(function);
    bool nameIsFollowed = m_access.kind != Kind::Has && m_flow.followsNamesOnClosuresOf(function, m_access.key);
    if (!nameIsFollowed && m_access.isByDescriptor && !classHolderIsMadeWithName(m_program, ClassSide::Static, m_access.key)) {
        m_flow.escape(function);
        storeIntoUnseen();
    } else if (!nameIsFollowed)
        storeInto(foundBesideFieldsUnderKey());
    if (nameIsFollowed) {
        FlowNodeID laid = m_flow.laidOnClosuresOf(function, m_access.key);
        if (!isRead()) {
            storeInto(laid);
            return;
        }
        giveFrom(laid);
        bool standsRightBelowFunctionPrototype = m_flow.typeAt(m_flow.foundOnClosuresOf(function)).isNever();
        if (BuiltinID member; standsRightBelowFunctionPrototype && lookUpFrom(Builtins::FunctionPrototype, member) == LookupInLinkAccesses::Absent) {
            givePrimitives(Primitive::Undefined);
            return;
        }
        forEachLayoutUpTheBases(standsRightBelowFunctionPrototype ? LayoutID() : m_flow.layoutOfConstructorFunction(function), [&](LayoutID current) {
            if (FunctionID holder = m_program.at(current).constructor; holder && m_flow.followsNamesOnClosuresOf(holder, m_access.key))
                giveFrom(m_flow.laidOnClosuresOf(holder, m_access.key));
            return false;
        });
    }
    cannotTell();
}

void AccessInLinkAccesses::giveConstructorsOfRowAbove(LayoutID id)
{
    size_t stepsLeft = m_program.layouts.size();
    for (LayoutID current = id; current && stepsLeft; current = m_program.at(current).base, --stepsLeft) {
        if (FunctionID constructor = m_program.at(current).constructor)
            give(closureForLinkAccesses(constructor));
    }
}

void AccessInLinkAccesses::resolvePrototypePropertyOf(FunctionID function, LayoutID constructed)
{
    if (isRead()) {
        give(prototypeForLinkAccesses(constructed));
        giveFrom(m_flow.unseenReplacementNode(constructed));
        if (!m_shared && m_flow.isOfConstructorFunction(constructed) && !m_flow.functionsThatReadPrototypeObjects.get(m_function.index()))
            m_flow.note(LinkFlow::Note::PrototypeObjectRead, m_function.index());
        return;
    }
    if (m_access.setsFlagsAlone)
        return;
    m_flow.notePrototypeIsReplaced(constructed);
    if (m_access.storesObjectMadeForIt)
        return;
    if (m_access.storesConstructedObject && !m_shared && m_value) {
        const ThinSummary& summary = m_flow.summaries.at(m_function);
        const SummaryNode& stored = summary.nodes[m_access.value];
        bool isCopied = stored.kind == SummaryNode::Kind::ResultOfCall && summary.calls[stored.first].copiesIntoPrototype;
        if (isCopied) {
            tellAgainOnceClimbs(m_value);
            if (m_flow.typeAt(m_value).isNever())
                return;
        }
        uint32_t call = isCopied ? m_flow.firstCallOfFunction[m_function.index()] + stored.first : 0;
        if (!isCopied || m_flow.followsCopyingCall(call)) {
            m_flow.addConstructedPrototype(function, constructed, m_value, isCopied ? std::optional(call) : std::nullopt);
            return;
        }
    }
    if (m_value)
        tellAgainOnceClimbs(m_value);
    if (m_value && m_flow.typeAt(m_value).isNever())
        return;
    if (m_value)
        m_flow.noteInstancesLaidAsPrototype(m_flow.typeAt(m_value));
    noteChange(constructed, LayoutChange::ChangesPrototype);
    m_flow.escape(function);
    m_flow.escape(constructed);
    storeIntoUnseen();
}

void AccessInLinkAccesses::resolveOnClassObject(FunctionID function, LayoutID classLayout)
{
    bool isDefinition = m_access.kind == Kind::Define;
    bool isHiddenByEveryClass = classHolderIsMadeWithName(m_program, ClassSide::Static, m_access.key);
    bool looksNothingUpAbove = isHiddenByEveryClass && !m_flow.hasComputedKeysOfItsOwn(classLayout, ClassSide::Static);
    if (!isDefinition && !looksNothingUpAbove && m_flow.namesMayBeIncomplete(classLayout, ClassSide::Static))
        cannotTellWhatRuns();

    bool findsField = false;
    size_t stepsLeft = m_program.layouts.size();
    for (LayoutID current = isHiddenByEveryClass ? classLayout : m_flow.nearestClassWithStaticFieldThatMayMatch(classLayout, m_access.key); current && stepsLeft--; current = m_flow.nearestClassWithStaticFieldThatMayMatch(m_program.at(current).base, m_access.key)) {
        m_program.at(current).forEachStaticFieldThatMayMatch(m_access.key, [&](const StaticField& field) {
            findsField |= !!field.key;
            giveWhatIsHeldAt(m_flow.variable(field.variable));
            storeInto(m_flow.variable(field.variable));
        });
        if ((findsField && !isRead()) || isHiddenByEveryClass)
            break;
    }

    LinkFlow::FoundMembers members = m_flow.findMembers(classLayout, ClassSide::Static, m_access.key);
    FunctionID holderInRow = members.mayAnswer() && !members.lookupGoesOnAboveClasses() ? FunctionID() : m_flow.classRowNameHolder(classLayout, m_access.key);
    if (isRead() && holderInRow)
        giveFrom(m_flow.laidOnClosuresOf(holderInRow, m_access.key));
    if (findsField || members.mayAnswer())
        holderInRow = { };
    layOnReceiverUnlessAccessorAnswers(findsField ? LinkFlow::FoundMembers() : members);
    bool findsAccessor = false;
    bool findsGetter = findsGetterAmong(members);
    for (const LinkFlow::FoundMember& found : members) {
        FlowNodeID member = m_flow.member(found.classLayout, ClassSide::Static, found.index);
        switch (found.kind) {
        case MethodKind::Method:
            giveFrom(member);
            if (!findsField)
                storeInto(member);
            break;
        case MethodKind::Getter:
            findsAccessor = true;
            if (isRead())
                enterAccessorAt(member);
            break;
        case MethodKind::Setter:
            findsAccessor = true;
            if (m_access.kind == Kind::Store)
                enterAccessorAt(member);
            if (!findsGetter)
                givePrimitives(Primitive::Undefined);
            break;
        }
    }

    if (findsField) {
        givePrimitives(Primitive::Undefined);
        if (isPrivateNameInLink(m_program, m_access.key))
            return;
        if (members.lookupGoesOnAboveClasses() && mayRunCodeFoundAboveClosures()) {
            letClassObjectEscape(function, classLayout);
            if (!isRead())
                storeIntoUnseen();
        }
        if (!isRead())
            return;
        giveFrom(foundBesideFieldsUnderKey());
        giveFrom(m_flow.foundOnClassObjectsFrom(classLayout));
        BuiltinID above;
        if (isHiddenByEveryClass || (members.lookupGoesOnAboveClasses() && lookUpFrom(Builtins::FunctionPrototype, above) != LookupInLinkAccesses::Absent))
            cannotTell();
        return;
    }
    if (members.lookupGoesOnAboveClasses() && mayRunCodeFoundAboveClosures())
        m_flow.escape(function);
    if (isRead()) {
        if (!members.lookupGoesOnAboveClasses())
            return;
        BuiltinID above;
        bool findsNothingAbove = !members.mayAnswer() && !m_flow.namesMayBeIncomplete(classLayout, ClassSide::Static)
            && m_flow.typeAt(m_flow.foundOnClosuresOf(function)).isNever() && lookUpFrom(Builtins::FunctionPrototype, above) == LookupInLinkAccesses::Absent;
        if (!findsNothingAbove) {
            cannotTell();
            return;
        }
        giveFrom(foundBesideFieldsUnderKey());
        giveFrom(m_flow.foundOnClassObjectsFrom(classLayout));
        givePrimitives(Primitive::Undefined);
        return;
    }
    if (findsAccessor && !isDefinition)
        return;
    if (findsAccessor)
        m_flow.escape(classLayout);
    if (isDefinition && !m_flow.isClassObjectMadeWith(function, m_access.key)) {
        if (!members.mayAnswer())
            letClassObjectEscape(function, classLayout);
        storeIntoUnseen();
        return;
    }
    if (holderInRow) {
        storeInto(m_flow.laidOnClosuresOf(holderInRow, m_access.key));
        return;
    }
    storeInto(foundBesideFieldsUnderKey());
    storeIntoUnseen();
}

void AccessInLinkAccesses::resolveOnConstructors(FamilyID family)
{
    if (isRead() && m_nameClass == NameClass::Prototype) {
        give(prototypesForLinkAccesses(family));
        return;
    }
    if (resolveThroughSharedAccess(SharedAccess::Subjects::ClassObjects, family))
        return;
    m_program.at(m_flow.families.layoutsOf(family)).forEach([&](LayoutID layout) {
        FunctionID constructor = m_program.at(layout).constructor;
        if (constructor && m_flow.classOfConstructor(constructor) == layout)
            resolveOnClosure(constructor);
    });
}

void AccessInLinkAccesses::resolveOnLostFunctions()
{
    if (isRead() && m_access.key == m_program.keyOf(BuiltinNames::name))
        m_flow.noteNamesOfLostFunctionsBecomeTexts();
    if (isRead() && namesNothingOnPlainClosures()) {
        cannotTell();
        return;
    }
    bool functionsLeaveWithDefinition = isNamed() && m_access.isByDescriptor && !classHolderIsMadeWithName(m_program, ClassSide::Static, m_access.key);
    if (!isNamed() || m_nameClass == NameClass::Prototype || functionsLeaveWithDefinition || mayRunCodeFoundAboveClosures())
        m_flow.addEdge(m_subject, m_flow.unseen());
    if (functionsLeaveWithDefinition)
        storeIntoUnseen();
    else if (isNamed())
        storeInto(foundBesideFieldsUnderKey());
    cannotTell();
}

bool AccessInLinkAccesses::namesNothingOnPlainClosures() const
{
    if (isNamed() || !m_keyIsOnlyNumbers)
        return false;
    const BuiltinClassID classesOfFunctions[] { BuiltinClasses::Function, BuiltinClasses::AsyncFunction, BuiltinClasses::GeneratorFunction, BuiltinClasses::AsyncGeneratorFunction };
    return std::ranges::all_of(classesOfFunctions, [&](BuiltinClassID functionClass) {
        BuiltinID prototype = prototypeOfInstancesOf(functionClass);
        return prototype && indexesAreAbsentFrom(prototype);
    });
}

bool AccessInLinkAccesses::mayRunCodeFoundAboveClosures()
{
    static constexpr uint8_t isKnown = 1 << 0;
    static constexpr uint8_t mayRunCode = 1 << 1;
    static constexpr uint8_t lookupMayThrow = 1 << 2;

    if (m_access.kind == Kind::Define)
        return false;
    if (m_flow.testRuns().isTestRun()) {
        if (FlowNodeID added = m_flow.testRuns().nodeOfNameAdded(m_access.key)) {
            tellAgainOnceClimbs(added);
            if (m_flow.testRuns().isNameAdded(added))
                return true;
        }
        return findsCodeToRunAboveClosures();
    }
    BytesSharedByThreads& answers = m_parts.state.codeFoundAboveClosures;
    bool asksForLibrary = isLibraryCode(m_program, m_program.at(m_function)) || m_flow.isInLibraryCode();
    size_t place = 4 * static_cast<size_t>(m_access.key.index()) + 2 * isRead() + asksForLibrary;
    if (uint8_t answer = answers.at(place)) {
        if (answer & lookupMayThrow)
            mayThrow();
        return answer & mayRunCode;
    }

    bool wasAskedToThrow = std::exchange(m_wasAskedToThrow, false);
    bool hadLeaned = std::exchange(m_hasLeanedOnMemberTakenForConstant, false);
    bool finds = findsCodeToRunAboveClosures();
    if (finds || !m_hasLeanedOnMemberTakenForConstant)
        answers.set(place, isKnown | (finds ? mayRunCode : 0) | (m_wasAskedToThrow ? lookupMayThrow : 0));
    m_wasAskedToThrow |= wasAskedToThrow;
    m_hasLeanedOnMemberTakenForConstant |= hadLeaned;
    return finds;
}

bool AccessInLinkAccesses::findsCodeToRunAboveClosures()
{
    BuiltinName name = m_program.builtinNameOf(m_access.key);
    const BuiltinClassID classesOfFunctions[] { BuiltinClasses::Function, BuiltinClasses::AsyncFunction, BuiltinClasses::GeneratorFunction, BuiltinClasses::AsyncGeneratorFunction };
    for (BuiltinClassID functionClass : classesOfFunctions) {
        if (name && m_table.findOwnPropertyOfInstances(functionClass, name))
            continue;
        BuiltinID prototype = prototypeOfInstancesOf(functionClass);
        if (!prototype || mayRunCodeFoundFrom(prototype))
            return true;
    }
    return false;
}

void AccessInLinkAccesses::letClassObjectEscape(FunctionID function, LayoutID classLayout)
{
    m_flow.escape(function);
    if (!classLayout)
        return;
    m_flow.escape(classLayout);
    BytesSharedByThreads& areLetGo = m_parts.state.classesWhoseStaticFieldsAreLetGo;
    forEachLayoutUpTheBases(classLayout, [&](LayoutID current) {
        if (areLetGo.at(current.index()))
            return true;
        areLetGo.set(current.index(), 1);
        for (const StaticField& field : m_program.at(current).staticFields) {
            FlowNodeID variable = m_flow.variable(field.variable);
            m_flow.addEdge(variable, m_flow.unseen());
            m_flow.addEdge(m_flow.unseen(), variable);
        }
        return false;
    });
}

void AccessInLinkAccesses::enterAccessorAt(FlowNodeID member)
{
    tellAgainOnceClimbsWhoeverTells(member);

    const LinkType held = m_flow.typeAt(member);
    if (held.hasUnknownObjects ? held.directPrimitives() : held.primitives)
        givePrimitives(Primitive::Undefined);
    if (held.builtin)
        m_flow.note(LinkFlow::Note::BuiltinUsedViaFlow, m_table.canonicalID(held.builtin).index());
    if (held.hasDirectUnknownObjects() || held.builtin || held.objects != LinkType::Objects::None || held.classes != LinkType::Classes::None)
        cannotTellWhatRuns();
    else if (held.hasUnknownObjects)
        cannotTellWhatRuns(UnknownIs::ByEscapeAlone);

    switch (held.functions) {
    case LinkType::Functions::None:
        break;
    case LinkType::Functions::Closure:
        enterAccessor(held.function);
        break;
    case LinkType::Functions::Constructors:
        m_flow.addEdge(member, m_flow.unseen());
        cannotTellWhatRuns();
        break;
    case LinkType::Functions::Set:
    case LinkType::Functions::Lost:
        mayThrow();
        m_flow.noteCallOfLostFunctions(held);
        m_flow.addEdge(m_receiver, m_flow.receiverOfLostFunctions(held));
        giveFrom(m_flow.resultOfLostFunctions(held));
        storeInto(m_flow.parameterOfLostFunctions(held, 0));
        break;
    }
}

void AccessInLinkAccesses::enterAccessorsOnPrototypesAbove(LayoutID id)
{
    for (FlowNodeID half : m_flow.halvesOfAccessorsOnPrototypesAbove(id, m_access.key, !isRead()))
        enterAccessorAt(half);
}

void AccessInLinkAccesses::layOnReceiverUnlessAccessorAnswers(const LinkFlow::FoundMembers& members)
{
    if (isRead() || m_receiver == m_subject)
        return;
    bool findsAccessor = std::ranges::any_of(members, [](const LinkFlow::FoundMember& member) {
        return member.kind != MethodKind::Method;
    });
    if (findsAccessor && !members.lookupGoesOnAboveClasses())
        return;
    m_flow.addEdge(m_receiver, m_flow.unseen());
    storeIntoUnseen();
}

void AccessInLinkAccesses::enterAccessor(FunctionID id)
{
    const Function& callee = m_program.at(id);
    if (callee.kind != FunctionKind::Getter && callee.kind != FunctionKind::Setter) {
        bool isPlain = callee.kind == FunctionKind::Plain || callee.kind == FunctionKind::Method || callee.kind == FunctionKind::Arrow;
        if (isPlain && callee.hasBody() && !callee.isCoroutine() && !callee.constructedClass && callee.results.size() == 1 && !m_flow.summaries.at(id).has(SummaryTrait::UsesArguments))
            m_flow.noteEnteredAsAccessor(id);
        else
            m_flow.escape(id);
    }
    if (!callee.hasBody()) {
        cannotTellWhatRuns();
        return;
    }

    m_flow.reach(id);
    m_flow.addEdge(m_flow.thrown(id), thrownNode());

    uint32_t numberOfParameters = callee.numberOfEntryParameters();
    uint32_t firstParameter = callee.firstParameterIndex();
    uint32_t numberOfPositionalParameters = callee.numberOfPositionalParameters();
    for (uint32_t index = 0; index < numberOfParameters; ++index) {
        FlowNodeID parameter = m_flow.parameter(id, index);
        switch (callee.entryParameterKind(index)) {
        case EntryParameter::Capture:
            break;
        case EntryParameter::This:
            m_flow.addEdge(m_receiver, parameter);
            break;
        case EntryParameter::Callee:
            m_flow.arrive(parameter, closureForLinkAccesses(id));
            break;
        case EntryParameter::NewTarget:
            m_flow.arrive(parameter, primitivesForLinkAccesses(Primitive::Undefined));
            break;
        case EntryParameter::ArgumentCount:
            m_flow.arrive(parameter, primitivesForLinkAccesses(Primitive::Integer));
            break;
        case EntryParameter::Parameter:
            if (index - firstParameter >= numberOfPositionalParameters)
                m_flow.arrive(parameter, LinkType::anything());
            else if (!isRead() && index == firstParameter)
                storeInto(parameter);
            else
                m_flow.arrive(parameter, primitivesForLinkAccesses(Primitive::Undefined));
            break;
        case EntryParameter::HiddenRest:
            m_flow.arrive(parameter, LinkType::anything());
            break;
        }
    }

    if (!isRead()) {
        if (!numberOfPositionalParameters)
            storeIntoUnseen();
        return;
    }
    if (callee.isCoroutine() || callee.results.size() != 1)
        giveAnything();
    else
        giveFrom(m_flow.resultToFollow(id, 0));
}

static constexpr uint16_t primitiveKindsForLinkAccesses[] { Primitive::AnyString, Primitive::AnySymbol, Primitive::BigInt, Primitive::Number, Primitive::Boolean };

bool AccessInLinkAccesses::primitiveStoresRunNoCode(uint16_t primitives)
{
    if (m_access.kind != Kind::Store || !isNamed() || m_nameClass == NameClass::Proto || mayBeElementKey(m_nameClass))
        return false;
    BuiltinName name = m_program.builtinNameOf(m_access.key);
    return std::ranges::all_of(primitiveKindsForLinkAccesses, [&](uint16_t kind) {
        if (!(primitives & LinkType::closed(kind)))
            return true;
        BuiltinClassID builtinClass = primitiveClassOf(kind);
        BuiltinID prototype = prototypeOfInstancesOf(builtinClass);
        return prototype && !(name && m_table.isRedefinedBelow(builtinClass, name)) && (!mayRunCodeFoundFrom(prototype) || isNameUndefinedFrom(prototype));
    });
}

void AccessInLinkAccesses::resolveOnPrimitives(uint16_t primitives)
{
    primitives &= ~(Primitive::UndefinedOrNull | Primitive::Absent | Primitive::Unspecified);
    if (!primitives)
        return;
    if (!isRead()) {
        if (primitiveStoresRunNoCode(primitives))
            mayThrow();
        else
            cannotTell();
        return;
    }

    for (uint16_t kind : primitiveKindsForLinkAccesses) {
        if (!(primitives & LinkType::closed(kind)))
            continue;
        BuiltinClassID builtinClass = primitiveClassOf(kind);
        BuiltinID prototype = prototypeOfInstancesOf(builtinClass);
        if (!prototype) {
            cannotTell();
            continue;
        }
        if (m_nameClass == NameClass::Proto && !m_flow.isInLibraryCode()) {
            m_flow.noteLossOf(m_table.canonicalID(prototype));
            m_flow.note(LinkFlow::Note::NameRead, m_access.key.bits(), true);
        }
        if ((isNamed() ? mayBeElementKey(m_nameClass) : m_keyIsOnlyNumbers) && m_table.definesCharactersOnNewInstances(builtinClass)) {
            givePrimitives(Primitive::String);
            if (indexesAreAbsentFrom(prototype))
                givePrimitives(Primitive::Undefined);
            else
                cannotTell();
            continue;
        }
        if (!isNamed() && m_keyIsOnlyNumbers && indexesAreAbsentFrom(prototype)) {
            givePrimitives(Primitive::Undefined);
            continue;
        }
        if (!isNamed()) {
            cannotTell(UnknownIs::InSight, tellsWhatMayAnswerUnderAnyNameBelow(builtinClass) ? TellsName::No : TellsName::Yes);
            continue;
        }

        BuiltinName name = m_program.builtinNameOf(m_access.key);
        if (name && m_table.isRedefinedBelow(builtinClass, name))
            cannotTell();
        else if (BuiltinID own = name ? m_table.findOwnPropertyOfInstances(builtinClass, name) : BuiltinID()) {
            m_flow.note(LinkFlow::Note::BuiltinRead, own.index());
            give(typeAnsweredByReadsOf(own));
        } else
            giveWhatIsFoundFrom(prototype);
    }
}

template<typename Functor>
void AccessInLinkAccesses::forEachListOfElements(const LinkType& instances, const Functor& functor)
{
    if (instances.classes == LinkType::Classes::Mixed) {
        functor(m_flow.contentsOf(instances, ContentsSlot::Element));
        return;
    }
    for (BuiltinClassID current = instances.builtinClass; current; current = m_table.at(current).parent) {
        BuiltinSlotID slot = m_table.findSlot(current, ContentsSlot::Element);
        if (slot && m_table.at(slot).heap == AbstractHeap::Elements) {
            functor(m_flow.contentsOf(instances, ContentsSlot::Element));
            return;
        }
    }
}

std::optional<uint32_t> AccessInLinkAccesses::indexWrittenAtAccess() const
{
    if (m_shared || !isRead() || isNamed())
        return std::nullopt;
    return indexWrittenAtInLinkTables(m_access);
}

bool AccessInLinkAccesses::hasNoIndexedElements(const LinkType& instances) const
{
    BuiltinClassID builtinClass = instances.builtinClass;
    if (instances.classes == LinkType::Classes::Mixed || !builtinClass || m_table.hasClassBelow(builtinClass))
        return false;
    if (!m_table.mayHavePristineInstances(builtinClass) || m_table.mayDefineOnNewInstances(builtinClass, { }) || m_table.definesCharactersOnNewInstances(builtinClass))
        return false;
    BuiltinID prototype = prototypeOfInstancesOf(builtinClass);
    return prototype && indexesAreAbsentFrom(prototype);
}

void AccessInLinkAccesses::resolveOnClasses(const LinkType& instances)
{
    if (instances.classes == LinkType::Classes::Mixed && m_flow.namesClassesOf(instances)) {
        m_flow.forEachClassOf(instances, [&](const LinkType& instancesOfOneClass) {
            resolveOnClasses(instancesOfOneClass);
        });
        return;
    }
    bool isOneClass = instances.classes != LinkType::Classes::Mixed;
    BuiltinClassID builtinClass = instances.builtinClass;
    if (m_nameClass == NameClass::Proto && !isRead()) {
        forEachListOfElements(instances, [&](FlowNodeID elements) {
            m_flow.addEdge(m_flow.unseen(), elements);
        });
        m_flow.addEdge(m_subject, m_flow.unseen());
        cannotTell();
        return;
    }

    bool mayBeElement = !isNamed() || mayBeElementKey(m_nameClass);
    bool isElement = isNamed() ? mayBeElement : m_keyIsOnlyNumbers;
    if (!mayBeElement) {
        if (!isRead() && m_access.key == m_program.keyOf(BuiltinNames::length) && !m_access.storesSurelyIndex)
            m_flow.arrive(m_flow.contentsOf(instances, SlotOfContents::noLongerAsMade()), m_flow.fromTypeID(TypeTable::undefined));
        if (!isRead() && builtinClass == BuiltinClasses::Array)
            m_flow.arrive(m_flow.contentsOf(instances, SlotOfContents::isChanged()), m_flow.fromTypeID(TypeTable::undefined));
        if (mayChangeListThatIsFollowed(instances) && m_access.key == m_program.keyOf(BuiltinNames::length)) {
            m_flow.markSite(instances.site, SlotOfContents::mayLoseElements());
            if (!m_access.storesZero)
                m_flow.markSite(instances.site, SlotOfContents::mayHaveHoles());
        }
        resolveUnderNameOnClass(instances);
        return;
    }
    bool namesNoMemberAbove = !isElement && keyNamesNoMemberAbove(instances);
    if (!isElement && !namesNoMemberAbove && isFinalListRead()) {
        m_flow.addEdge(m_flow.contentsOf(instances, SlotOfContents::underAnyName()), m_flow.unseen());
        forEachListOfElements(instances, [&](FlowNodeID elements) {
            m_flow.addEdge(elements, m_flow.unseen());
        });
    } else if (!isElement && !namesNoMemberAbove)
        m_flow.addEdge(m_subject, m_flow.unseen());
    if (isRead() && !isElement)
        giveFrom(m_flow.contentsOf(instances, SlotOfContents::underAnyName()));
    if (!isRead() && namesNoMemberAbove) {
        for (BuiltinName text : { BuiltinNames::undefined, BuiltinNames::null, BuiltinNames::trueKeyword, BuiltinNames::falseKeyword })
            storeInto(m_flow.contentsOf(instances, SlotOfContents::under(m_program.keyOf(text))));
        m_flow.arrive(m_flow.contentsOf(instances, SlotOfContents::noLongerAsMade()), m_flow.fromTypeID(TypeTable::undefined));
    }

    auto tellsOfInstances = [&] {
        return isOneClass && builtinClass && m_table.mayHavePristineInstances(builtinClass) && tellsWhatMayAnswerUnderAnyNameBelow(builtinClass);
    };
    TypedArrayType typedArray = isOneClass ? m_table.typedArrayTypeOf(builtinClass) : NotTypedArray;
    if (typedArray != NotTypedArray && typedArray != TypeDataView) {
        give(m_flow.fromTypeID(elementTypeOf(typedArray)));
        if (!m_shared && isRead() && instances.classes == LinkType::Classes::Site)
            m_flow.noteReadsOffViewMadeAtSite(m_function);
        std::optional<uint32_t> index = instances.classes == LinkType::Classes::Site ? indexWrittenAtAccess() : std::nullopt;
        if (index && *index < m_flow.writtenLengthOf(instances.site))
            giveFrom(m_flow.contentsOf(instances, SlotOfContents::mayBeShorter()));
        else
            givePrimitives(Primitive::Undefined);
        storeIntoUnseen();
        if (!isElement && !namesNoMemberAbove)
            cannotTell(m_seenKeysAreNumbers ? UnknownIs::ByEscapeAlone : UnknownIs::InSight, tellsOfInstances() ? TellsName::No : TellsName::Yes);
        return;
    }

    std::optional<uint32_t> placeRead = isElement && instances.classes == LinkType::Classes::Site && builtinClass == BuiltinClasses::Array ? indexWrittenAtAccess() : std::nullopt;
    BuiltinID prototypeOfListRead = placeRead && *placeRead < m_flow.numberOfPlacesWrittenOf(instances.site) && SlotOfContents::hasSlotAt(*placeRead) ? prototypeOfInstancesOf(builtinClass) : BuiltinID();
    auto isChangedAsFarAsIsSeen = [&] {
        FlowNodeID isChanged = m_flow.contentsOf(instances, SlotOfContents::isChanged());
        FlowObserver observer { FlowObserver::Kind::MemberOfAccess, m_function, m_record };
        if (laysWatchAt(isChanged, observer))
            m_flow.observe(isChanged, observer);
        return !m_flow.typeAt(isChanged).isNever();
    };
    if (prototypeOfListRead && indexesAreAbsentFrom(prototypeOfListRead) && !isChangedAsFarAsIsSeen()) {
        giveWhatIsHeldAt(m_flow.whatIsWrittenAt(instances.site, *placeRead));
        m_flow.build.statistics().count(StatisticID::ArrayLiteralElementReadsResolved);
        return;
    }

    bool hasElements = false;
    forEachListOfElements(instances, [&](FlowNodeID elements) {
        hasElements = true;
        giveWhatIsHeldAt(elements);
        storeInto(elements);
    });
    if (isRead() && !hasElements && !isNamed() && m_keyIsOnlyNumbers && hasNoIndexedElements(instances)) {
        giveFrom(m_flow.contentsOf(instances, SlotOfContents::underUnknownNames()));
        givePrimitives(Primitive::Undefined);
        return;
    }
    if (!isRead() && !hasElements)
        storeInto(m_flow.contentsOf(instances, SlotOfContents::underUnknownNames()));
    if (!isRead() && builtinClass == BuiltinClasses::Array)
        m_flow.arrive(m_flow.contentsOf(instances, SlotOfContents::isChanged()), m_flow.fromTypeID(TypeTable::undefined));
    if (mayChangeListThatIsFollowed(instances))
        m_flow.markSite(instances.site, SlotOfContents::mayHaveHoles());
    if (!isRead() && (isNamed() ? m_nameClass != NameClass::Index : !m_access.keyIsSurelyIndex)) {
        bool storesUnderNumber = m_access.kind == Kind::Store && isElement && !isNamed();
        m_flow.arrive(m_flow.contentsOf(instances, storesUnderNumber ? SlotOfContents::mayHaveNumberNames() : SlotOfContents::noLongerAsMade()), m_flow.fromTypeID(TypeTable::undefined));
    }
    bool namesOnlyOwnElements = isElement || namesNoMemberAbove;
    BuiltinID prototype = isOneClass && (namesOnlyOwnElements || m_seenKeysAreNumbers) && hasElements ? prototypeOfInstancesOf(builtinClass) : BuiltinID();
    bool indexesAreAbsentAbove = prototype && indexesAreAbsentFrom(prototype);
    if (indexesAreAbsentAbove)
        givePrimitives(Primitive::Undefined);
    if (!indexesAreAbsentAbove || !namesOnlyOwnElements)
        cannotTell(indexesAreAbsentAbove ? UnknownIs::ByEscapeAlone : UnknownIs::InSight, !isElement && tellsOfInstances() ? TellsName::No : TellsName::Yes);
}

void AccessInLinkAccesses::resolveUnderNameOnClass(const LinkType& instances)
{
    BuiltinClassID builtinClass = instances.builtinClass;
    if (isRead())
        giveFrom(m_flow.contentsOf(instances, SlotOfContents::under(m_access.key)));

    BuiltinName name = m_program.builtinNameOf(m_access.key);
    if (BuiltinID own = isRead() && builtinClass && name ? m_table.findOwnPropertyOfInstances(builtinClass, name) : BuiltinID()) {
        m_flow.note(LinkFlow::Note::BuiltinRead, own.index());
        give(typeAnsweredByReadsOf(own));
        return;
    }

    if (BuiltinLayoutID record = isRead() && name && instances.classes == LinkType::Classes::Site ? m_flow.layoutOfRecordsOfFamilyOf(instances.site) : BuiltinLayoutID()) {
        bool isListed = std::ranges::any_of(m_table.at(record).fields, [&](const BuiltinLayoutField& field) {
            return field.name == name && !field.isOptional;
        });
        if (isListed) {
            tellAgainOnceClimbsWhoeverTells(m_flow.contentsOf(instances, SlotOfContents::hasSeveralRecordLayouts()));
            giveFrom(m_flow.contentsOf(instances, SlotOfContents::underUnknownNames()));
            return;
        }
    }

    BuiltinID prototype = builtinClass ? prototypeOfInstancesOf(builtinClass) : BuiltinID();
    bool tableSpeaksOfObject = prototype && m_nameClass != NameClass::Proto && m_table.mayHavePristineInstances(builtinClass) && !m_table.mayDefineOnNewInstances(builtinClass, name);
    if (tableSpeaksOfObject && name) {
        for (BuiltinClassID other : m_table.classesWithInstanceMember(name))
            tableSpeaksOfObject &= other == builtinClass || !m_table.isSameOrSubclassOf(other, builtinClass);
    }
    if (isRead() && tableSpeaksOfObject) {
        giveFrom(m_flow.contentsOf(instances, SlotOfContents::underUnknownNames()));
        BuiltinID member;
        bool onlyProgramLaysName = lookUpFrom(prototype, member) == LookupInLinkAccesses::Unknown && isKeyAbsentFromChain(prototype) && areListsThatOnlyProgramNames(instances);
        if (!giveWhatIsFoundFrom(prototype, onlyProgramLaysName ? EngineMayDefineInheritedNames::No : EngineMayDefineInheritedNames::Yes))
            m_flow.escapeAllOf(instances);
        return;
    }

    if (!isRead() && builtinClass && name && m_table.findOwnPropertyOfInstances(builtinClass, name)) {
        mayThrow();
        storeIntoUnseen();
        return;
    }

    if (isRead()) {
        if (m_nameClass == NameClass::Proto && prototype && !m_flow.isInLibraryCode())
            m_flow.noteLossOf(m_table.canonicalID(prototype));
        if (!prototype || mayRunCodeFoundFrom(prototype))
            m_flow.escapeAllOf(instances);
        giveFrom(m_flow.contentsOf(instances, SlotOfContents::underUnknownNames()));
        cannotTell();
        for (BuiltinID holder = name ? prototype : BuiltinID(); holder; holder = m_table.at(holder).prototypeLink == BuiltinPrototypeLink::Builtin ? m_table.canonicalID(m_table.at(holder).inheritsFrom) : BuiltinID()) {
            BuiltinID member = m_table.findOwnMember(holder, name);
            if (!member)
                continue;
            BuiltinID canonical = m_table.canonicalID(member);
            if (m_table.at(member).scope == BuiltinScope::Realm && isObjectKind(m_table.at(canonical).kind))
                give(m_flow.anythingBeside(canonical, m_function));
            break;
        }
        return;
    }

    mayThrow();
    if (m_access.kind == Kind::Store && tableSpeaksOfObject && nativeSetterFoundFromKeepsReceiver(prototype))
        return;
    auto onlyProgramLaysName = [&] {
        return tableSpeaksOfObject && areListsThatOnlyProgramNames(instances) && isNameUndefinedFrom(prototype);
    };
    if (m_access.kind != Kind::Define && (!prototype || (mayRunCodeFoundFrom(prototype) && !onlyProgramLaysName()))) {
        m_flow.escapeAllOf(instances);
        storeIntoUnseen();
    }
    bool isNameOfProgram = instances.classes == LinkType::Classes::Site && builtinClass == BuiltinClasses::Array && !name && m_program.at(m_access.key).kind == PropertyKeyKind::Text;
    if (isNameOfProgram && m_access.kind != Kind::Has) {
        storeInto(m_flow.contentsOf(instances, SlotOfContents::under(m_access.key)));
        m_flow.arrive(m_flow.contentsOf(instances, SlotOfContents::noLongerAsMade()), m_flow.fromTypeID(TypeTable::undefined));
        return;
    }
    storeInto(m_flow.contentsOf(instances, SlotOfContents::underUnknownNames()));
}

void AccessInLinkAccesses::handOn(const LinkType& type)
{
    if (uint32_t node = m_access.whatHoldsNothing(); node != noSummaryNode)
        m_flow.arriveFromSource(m_flow.nodeOf(m_function, node), type);
}

void AccessInLinkAccesses::defineWrittenPrototypeAccessor(LayoutID id)
{
    RELEASE_ASSERT(!m_shared);
    const SummaryWrittenAccessor& written = m_flow.summaries.at(m_function).writtenAccessorOf(m_record);
    m_flow.layAccessorOnPrototypeOf(id, m_access.key, written.getter, written.setter);
}

void AccessInLinkAccesses::resolveContents(const LinkType& subject)
{
    bool settles = m_access.whatHoldsNothing() != noSummaryNode;
    if (uint16_t primitives = (subject.primitives & ~m_before.primitives) | (subject.directPrimitives() & ~m_before.directPrimitives())) {
        LinkType handed = primitivesForLinkAccesses(primitives);
        handed.escapeOnlyPrimitives = handed.primitives & ~LinkType::closed(subject.directPrimitives());
        handOn(handed);
    }

    LinkType others = subject;
    others.primitives = 0;
    others.dropInstances();
    LinkType othersBefore = m_before;
    othersBefore.primitives = 0;
    othersBefore.dropInstances();
    if (!others.isNever() && (others != othersBefore || others.hasDirectUnknownObjects() != othersBefore.hasDirectUnknownObjects())) {
        handOn(others);
        if (!settles || others.hasUnknownObjects) {
            LinkType unseenAlone;
            unseenAlone.hasUnknownObjects = true;
            LinkFlow::UnknownByEscapeAlone unknownByEscapeAlone(others == unseenAlone && !others.hasDirectUnknownObjects(), LinkFlow::UnknownByEscapeAlone::Marks::OfRule);
            giveAnything();
            storeIntoUnseen();
        }
    }

    if (subject.classes != LinkType::Classes::None && !subject.holdsSameInstancesAs(m_before))
        resolveContentsOfInstances(instancesAmongForLinkAccesses(subject));

    if (settles && m_access.keyNode != noSummaryNode) {
        const LinkType found = m_flow.typeAt(m_flow.nodeOf(m_function, m_access.keyNode));
        if (holdsObjectsForLinkAccesses(found))
            giveAnything();
    }
}

bool AccessInLinkAccesses::settlesPromiseThatCallHasJustMade()
{
    if (!m_access.standsRightBehindCall)
        return false;
    const ThinSummary& summary = m_flow.summaries.at(m_function);
    const SummaryNode& subject = summary.nodes[m_access.subject];
    if (subject.kind != SummaryNode::Kind::ResultOfCall)
        return false;
    const SummaryCall& call = summary.calls[subject.first];
    if (call.isConstruction)
        return false;
    auto makesItsPromise = [&](FunctionID function) {
        const Function& record = m_program.at(function);
        return record.has(FunctionFlags::IsAsync) && !record.has(FunctionFlags::IsGenerator);
    };
    auto isClosureOfSuchFunctionAt = [&](FlowNodeID node) {
        if (m_laysObservers)
            m_flow.observe(node, FlowObserver { FlowObserver::Kind::MemberOfAccess, m_function, m_record });
        LinkType callee = m_flow.typeAt(node);
        callee.primitives = 0;
        return callee.functions == LinkType::Functions::Closure && callee == closureForLinkAccesses(callee.function) && makesItsPromise(callee.function);
    };
    switch (call.callee) {
    case SummaryCall::Callee::Binding:
        return makesItsPromise(call.function);
    case SummaryCall::Callee::Value:
        return call.subject != noSummaryNode && isClosureOfSuchFunctionAt(m_flow.nodeOf(m_function, call.subject));
    case SummaryCall::Callee::Name: {
        if (call.receiver == noSummaryNode || !call.receiver || !call.key)
            return false;
        FlowNodeID receiverNode = m_flow.nodeOf(m_function, call.receiver);
        if (m_laysObservers)
            m_flow.observe(receiverNode, FlowObserver { FlowObserver::Kind::MemberOfAccess, m_function, m_record });
        LinkType receiver = m_flow.typeAt(receiverNode);
        receiver.primitives &= ~Primitive::UndefinedOrNull;
        LinkType objects;
        objects.objects = LinkType::Objects::Layout;
        objects.layout = receiver.layout;
        if (receiver.objects != LinkType::Objects::Layout || receiver != objects)
            return false;
        LayoutID layout = receiver.layout;
        if (m_flow.namesMayBeIncomplete(layout, ClassSide::Prototype) || m_flow.escapedLayouts.get(layout.index()))
            return false;
        auto holdsObjectAt = [&](FlowNodeID node) {
            if (laysWatchAt(node, FlowObserver { FlowObserver::Kind::MemberOfAccess, m_function, m_record }))
                m_flow.observe(node, FlowObserver { FlowObserver::Kind::MemberOfAccess, m_function, m_record });
            LinkType held = m_flow.typeAt(node);
            held.primitives = 0;
            return !held.isNever();
        };
        std::optional<FieldReference> field = findField(m_program, layout, call.key);
        if ((!field && holdsObjectAt(m_flow.appendedField(layout, call.key))) || holdsObjectAt(m_flow.extraPropertyReadNode(layout)))
            return false;
        LinkFlow::FoundMembers members = m_flow.findMembers(layout, ClassSide::Prototype, call.key);
        bool mayLieBelowPrototypes = m_flow.isBelowFollowedPrototype(layout);
        if (field || (!members.mayAnswer() && mayLieBelowPrototypes)) {
            if (members.mayAnswer() || m_program.at(layout).has(LayoutFlags::IsOfCreatedObject) || !m_flow.isAbsentFromObjectPrototype(call.key))
                return false;
            if (!field)
                return m_flow.halvesOfAccessorsOnPrototypesAbove(layout, call.key, false).isEmpty() && isClosureOfSuchFunctionAt(m_flow.foundOnPrototypesAbove(layout, call.key, LinkFlow::AccessorsAbove::AreEnteredByCaller));
            const Field& record = fieldAt(m_program, *field);
            if (mayLieBelowPrototypes || field->layout != layout || record.form.contains(FieldForm::IsAccessor) || record.declared.established())
                return false;
            return !holdsObjectAt(m_flow.foundWhereFieldIsEmpty(*field)) && !holdsObjectAt(m_flow.foundBesideFieldsFor(layout, call.key)) && isClosureOfSuchFunctionAt(m_flow.field(*field));
        }
        return members.size() == 1 && members[0].kind == MethodKind::Method && !members.lookupGoesOnAboveClasses() && isClosureOfSuchFunctionAt(m_flow.member(members[0].classLayout, ClassSide::Prototype, members[0].index));
    }
    case SummaryCall::Callee::Builtin:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void AccessInLinkAccesses::resolveContentsOfInstances(const LinkType& instances)
{
    bool settles = m_access.whatHoldsNothing() != noSummaryNode;
    bool isOneClass = instances.classes != LinkType::Classes::Mixed;

    if (!isOneClass && m_access.slot.indexOfInternalField()) {
        m_flow.addEdge(m_subject, m_flow.unseen());
        giveAnything();
        storeIntoUnseen();
        return;
    }

    if (settles) {
        bool mayBePromise = !isOneClass || m_table.isSameOrSubclassOf(instances.builtinClass, BuiltinClasses::Promise);
        if (!isOneClass || !mayBePromise)
            handOn(instances);
        if (!mayBePromise)
            return;
    }

    TypedArrayType typedArray = isOneClass ? m_table.typedArrayTypeOf(instances.builtinClass) : NotTypedArray;
    if (typedArray != NotTypedArray && typedArray != TypeDataView && m_access.slot == SlotOfContents(ContentsSlot::Element)) {
        give(m_flow.fromTypeID(elementTypeOf(typedArray)));
        return;
    }

    for (SlotOfContents slot : m_flow.slotsBehind(instances.builtinClass, m_access.slot)) {
        FlowNodeID held = m_flow.contentsOf(instances, slot);
        giveFrom(held);
        storeInto(held);
        if (!isRead() && slot == SlotOfContents(ContentsSlot::Element))
            m_flow.tellWrittenListIsChanged(instances);
    }
    if (!settles)
        return;

    auto mayHoldSomething = [&](FlowNodeID node) {
        if (laysWatchAt(node, FlowObserver { FlowObserver::Kind::MemberOfAccess, m_function, m_record }))
            m_flow.observe(node, FlowObserver { FlowObserver::Kind::MemberOfAccess, m_function, m_record });
        return !m_flow.typeAt(node).isNever();
    };
    bool hasJustBeenMade = settlesPromiseThatCallHasJustMade();
    if (mayHoldSomething(m_flow.contentsOf(instances, SlotOfContents::underUnknownNames())) && !hasJustBeenMade) {
        m_flow.addEdge(m_subject, m_flow.unseen());
        giveAnything();
    }

    for (BuiltinClassID current = isOneClass ? instances.builtinClass : BuiltinClassID(BuiltinClasses::Promise); current; current = m_table.at(current).parent) {
        for (BuiltinSlotID id : m_table.at(current).slots) {
            if (SlotOfContents other = m_flow.slotOf(id); other != m_access.slot)
                m_flow.addEdge(m_flow.contentsOf(instances, other), m_flow.thrown(m_function));
        }
    }
}

bool AccessInLinkAccesses::testsMayHaveLaidNameFrom(BuiltinID start)
{
    if (!m_flow.testRuns().isTestRun() || (!isNamed() && (isRead() || m_keyIsOnlyNumbers)))
        return false;
    if (FlowNodeID added = isNamed() ? m_flow.testRuns().nodeOfNameAdded(m_access.key) : m_flow.testRuns().nodeOfSomeNameAdded()) {
        tellAgainOnceClimbs(added);
        if (m_flow.testRuns().isNameAdded(added))
            return true;
    }
    StringID name;
    if (isNamed()) {
        const PropertyKey& record = m_program.at(m_access.key);
        if (record.kind != PropertyKeyKind::Text || !m_program.isOpenedSomewhere(record.text))
            return false;
        name = record.text;
    }
    BuiltinID holder = m_table.canonicalID(start);
    while (true) {
        if (name ? m_program.isOpened(holder, name) : m_program.isSomeNameOpenedOn(holder))
            return true;
        const Builtin& object = m_table.at(holder);
        switch (object.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return true;
        case BuiltinPrototypeLink::Null:
            return false;
        case BuiltinPrototypeLink::Builtin:
            holder = m_table.canonicalID(object.inheritsFrom);
            break;
        }
    }
}

void AccessInLinkAccesses::resolveOnBuiltin(BuiltinID builtin)
{
    if (isRefusedStore(builtin)) {
        mayThrow();
        return;
    }
    if (m_access.kind != Kind::Define && lookupPassesHolderLinkedAnew(m_program, builtin, m_access.key))
        m_flow.handToAccessorsLaidAbove(m_receiver);
    if (m_access.kind != Kind::Define && testsMayHaveLaidNameFrom(builtin))
        m_flow.addEdge(m_receiver, m_flow.unseen());
    bool isStoredGlobalName = isNamed() && m_nameClass != NameClass::Proto && m_parts.state.globalNamesStored.get(m_access.key.index()) && m_table.canonicalID(builtin) == m_table.canonicalID(Builtins::GlobalThis) && isKeyAbsentFromChain(builtin);
    if (isRead() && isNamed()) {
        if (m_nameClass == NameClass::Proto && !m_table.isObjectOfClass(builtin) && m_table.at(builtin).prototypeLink == BuiltinPrototypeLink::Builtin && !m_flow.isInLibraryCode())
            m_flow.noteLossOf(m_table.canonicalID(m_table.at(builtin).inheritsFrom));
        if (giveWhatLightFormServes(builtin))
            return;
        if (isFollowedOnBuiltin(builtin)) {
            mayThrow();
            givePrimitives(Primitive::Undefined);
            giveWhatIsHeldAt(m_flow.heldByBuiltin(m_table.canonicalID(builtin), m_access.key));
            return;
        }
        BuiltinID member;
        bool isUnknown = lookUpFrom(builtin, member) == LookupInLinkAccesses::Unknown;
        const auto& holding = m_table.at(m_table.canonicalID(builtin));
        bool inheritsFromObjectAlone = holding.prototypeLink != BuiltinPrototypeLink::Builtin || m_table.at(m_table.canonicalID(holding.inheritsFrom)).prototypeLink != BuiltinPrototypeLink::Builtin;
        if (isUnknown && inheritsFromObjectAlone && !isStoredGlobalName && !m_flow.isGlobalObject(m_table.canonicalID(builtin)) && isNameUndefinedFrom(builtin)) {
            givePrimitives(Primitive::Undefined);
            return;
        }
        if (isUnknown && tellsWhatMayAnswerFrom(builtin)) {
            if (isStoredGlobalName)
                giveWhatIsHeldAt(m_flow.heldByGlobalObject(m_access.key));
            cannotTell(isStoredGlobalName ? UnknownIs::ByEscapeAlone : UnknownIs::InSight, TellsName::No);
            return;
        }
        giveWhatIsFoundFrom(builtin, EngineMayDefineInheritedNames::Yes);
        return;
    }
    if (isRead())
        m_flow.noteLossOf(builtin);
    bool mayStoreUnderPrototypeLink = isNamed() ? m_flow.nameClassOf(m_access.key) == NameClass::Proto : !m_keyIsOnlyNumbers;
    if (m_access.kind == Kind::Store && mayStoreUnderPrototypeLink && !m_table.hasLockedPrototypeLink(builtin)) {
        LinkType stored = LinkType::anything();
        if (m_value) {
            tellAgainOnceClimbs(m_value);
            stored = m_flow.typeAt(m_value);
        }
        stored.primitives = 0;
        if (!stored.isNever())
            m_flow.addEdge(m_subject, m_flow.unseen());
    }
    if (m_access.kind == Kind::Store && isFollowedOnBuiltin(builtin)) {
        FlowNodeID held = m_flow.heldByBuiltin(m_table.canonicalID(builtin), m_access.key);
        mayThrow();
        storeInto(held);
        tellAgainOnceClimbsWhoeverTells(held);
        if (!m_flow.typeAt(held).hasUnknownObjects)
            return;
    }
    if (m_access.kind == Kind::Store && storesListedObject(builtin)) {
        mayThrow();
        return;
    }
    if (isStoredGlobalName && m_access.kind == Kind::Store)
        storeInto(m_flow.heldByGlobalObject(m_access.key));
    cannotTell();
}

bool AccessInLinkAccesses::storesListedObject(BuiltinID holder)
{
    BuiltinID member = isNamed() && m_value ? findOwnMember(m_program, m_table.canonicalID(holder), m_access.key) : BuiltinID();
    if (!member || !isObjectKind(m_table.at(m_table.canonicalID(member)).kind) || m_table.at(member).kind == BuiltinKind::Accessor)
        return false;
    tellAgainOnceClimbs(m_value);
    LinkType stored = m_flow.typeAt(m_value);
    BuiltinID object = std::exchange(stored.builtin, BuiltinID());
    return object && m_table.canonicalID(object) == m_table.canonicalID(member) && stored.isNever();
}

const BuiltinHostObject* AccessInLinkAccesses::findHostObjectThatSubjectIsReadAs() const
{
    if (!isRead() || !isNamed() || m_shared || !m_function)
        return nullptr;
    const ThinSummary& summary = m_flow.summaries.at(m_function);
    const SummaryNode& subject = summary.nodes[m_access.subject];
    if (subject.kind != SummaryNode::Kind::ResultOfAccess)
        return nullptr;
    const SummaryAccess& path = summary.accesses[subject.first];
    if (path.kind != Kind::Read || !path.key || path.isToContents() || summary.nodes[path.subject].kind != SummaryNode::Kind::Builtin)
        return nullptr;
    BuiltinName name = m_program.builtinNameOf(path.key);
    BuiltinMemberLookup found = name ? m_table.findMember(m_table.canonicalID(BuiltinID(summary.nodes[path.subject].first)), name) : BuiltinMemberLookup();
    if (found.outcome != BuiltinMemberLookup::Outcome::Found)
        return nullptr;
    for (const BuiltinHostObject& object : m_table.hostObjects()) {
        if (object.holder && m_table.canonicalID(object.holder) == m_table.canonicalID(found.member))
            return &object;
    }
    return nullptr;
}

bool AccessInLinkAccesses::giveWhatOtherNamesHold(const BuiltinHostObject& object)
{
    LinkType held;
    bool listsOtherNames = false;
    for (const BuiltinHostObjectField& field : object.fields) {
        if (field.name || field.value.kind != BuiltinPlaceKind::NewValue)
            continue;
        held = m_flow.joinOf(held, typeWrittenBy(field.value.newValueType));
        listsOtherNames = true;
    }
    if (!listsOtherNames || held.hasUnknownObjects)
        return false;
    held.primitives |= Primitive::Undefined;
    give(held);
    return true;
}

bool AccessInLinkAccesses::isKeyAbsentFromChain(BuiltinID start) const
{
    MembersUnderKey members(m_program, m_access.key);
    for (BuiltinID holder = m_table.canonicalID(start); ; holder = m_table.canonicalID(m_table.at(holder).inheritsFrom)) {
        if (compiledModuleWithObject(m_program, holder) || m_table.at(m_table.rootOf(holder)).scope != BuiltinScope::Realm)
            return false;
        if (members.findOwnMember(holder) || !listsEveryMemberUnder(m_program, holder, m_access.key))
            return false;
        if (m_table.at(holder).prototypeLink != BuiltinPrototypeLink::Builtin)
            return m_table.at(holder).prototypeLink == BuiltinPrototypeLink::Null;
    }
}

bool AccessInLinkAccesses::tellsMemberThatTableFindsFrom(BuiltinID builtin) const
{
    BuiltinID member;
    if (!isNamed() || m_nameClass == NameClass::Proto || m_flow.lightFormOf(builtin))
        return false;
    if (isRead())
        return lookUpFrom(builtin, member) != LookupInLinkAccesses::Unknown || isKeyAbsentFromChain(builtin);
    return !m_access.isByDescriptor && !m_access.laysAccessorAsWritten && isKeyAbsentFromChain(builtin);
}

bool AccessInLinkAccesses::isServedByLightFormOf(BuiltinID builtin) const
{
    const BuiltinCondition* light = m_flow.lightFormOf(builtin);
    BuiltinName name = light && isRead() && isNamed() ? m_program.builtinNameOf(m_access.key) : BuiltinName();
    if (!name)
        return false;
    if (std::ranges::contains(light->valuesServed, name))
        return true;
    return std::ranges::contains(light->methodsServed, name) && !m_shared && m_flow.isSpelledInBody(m_function, m_access.subject);
}

bool AccessInLinkAccesses::giveWhatLightFormServes(BuiltinID builtin)
{
    if (!isServedByLightFormOf(builtin))
        return false;
    mayThrow();
    giveAnything();
    return true;
}

BuiltinID AccessInLinkAccesses::prototypeOfInstancesOf(BuiltinClassID builtinClass) const
{
    BuiltinID prototype;
    for (BuiltinClassID current = builtinClass; current && !prototype; current = m_table.at(current).parent)
        prototype = m_table.at(current).prototype;
    return prototype;
}

bool AccessInLinkAccesses::membersOfObjectPrototypeAreWhatTableLists()
{
    if (!m_program.admitsReplacedBuiltins)
        return true;
    for (BuiltinID member : m_table.membersOf(m_table.canonicalID(Builtins::ObjectPrototype))) {
        if (m_program.isUnlocked(member) || m_program.isUnlocked(m_table.canonicalID(member)))
            return false;
        if (!m_program.isLockLeftToProgram(member))
            continue;
        if (!m_flow.takeForConstant(member))
            return false;
        tellAgainOnceClimbs(m_flow.writtenAtMember(member));
    }
    return true;
}

KeyAffixes AccessInLinkAccesses::affixesOfKeyRead() const
{
    if (!isRead() || isNamed() || m_shared || !m_value || m_receiver != m_subject || m_program.admitsReplacedBuiltins || m_flow.testRuns().isTestRun())
        return { };
    const BodyIndex* index = bodiesLookedAt(m_flow.looks).find(m_function);
    if (!index || m_access.site.index() >= index->numberOfValues() || index->isParameter(m_access.site) || index->opcodeOf(m_access.site) != Opcode::ElemGet)
        return { };
    return affixesOfKey(m_program, *index, index->value(m_access.site).as<Opcode::ElemGet>().propertyOperand());
}

bool AccessInLinkAccesses::indexesAreAbsentFrom(BuiltinID start) const
{
    BuiltinID holder = m_table.canonicalID(start);
    while (true) {
        if (!m_table.closesIndexKeys(holder) || !m_table.hasLockedPrototypeLink(holder))
            return false;
        const Builtin& object = m_table.at(holder);
        switch (object.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return false;
        case BuiltinPrototypeLink::Null:
            return true;
        case BuiltinPrototypeLink::Builtin:
            holder = m_table.canonicalID(object.inheritsFrom);
            break;
        }
    }
}

bool AccessInLinkAccesses::textsOfPrimitivesAreAbsentFrom(BuiltinID start) const
{
    BuiltinID holder = m_table.canonicalID(start);
    while (true) {
        if (m_program.isSomeNameOpenedOn(holder))
            return false;
        const Builtin& object = m_table.at(holder);
        for (BuiltinName text : { BuiltinNames::undefined, BuiltinNames::null, BuiltinNames::trueKeyword, BuiltinNames::falseKeyword }) {
            bool isClosed = object.flags.contains(BuiltinFlag::IsLockedObject) || m_table.isClosedKey(holder, text);
            if (!isClosed || m_table.findOwnMember(holder, text))
                return false;
        }
        switch (object.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return false;
        case BuiltinPrototypeLink::Null:
            return true;
        case BuiltinPrototypeLink::Builtin:
            holder = m_table.canonicalID(object.inheritsFrom);
            break;
        }
    }
}

bool AccessInLinkAccesses::keyNamesNoMemberAbove(const LinkType& instances)
{
    if (isNamed() || m_keyIsOnlyNumbers || !m_keyIsNumberBooleanOrNullish || (!isRead() && m_access.kind != Kind::Store))
        return false;
    if (instances.classes != LinkType::Classes::Site || !instances.builtinClass || !m_flow.isOfClassItself(instances.site))
        return false;
    BuiltinID prototype = prototypeOfInstancesOf(instances.builtinClass);
    if (!prototype || !indexesAreAbsentFrom(prototype) || !textsOfPrimitivesAreAbsentFrom(prototype) || lookupPassesHolderLinkedAnew(m_program, m_table.canonicalID(prototype), { }))
        return false;
    FlowNodeID mark = m_flow.contentsOf(instances, SlotOfContents::mayBeOfClassBelow());
    if (!mark)
        return false;
    tellAgainOnceClimbs(mark);
    return m_flow.typeAt(mark).isNever();
}

LookupInLinkAccesses AccessInLinkAccesses::lookUpFrom(BuiltinID start, BuiltinID& member) const
{
    if (FlowNodeID added = m_flow.testRuns().nodeOfNameAdded(m_access.key)) {
        if (m_laysObservers)
            m_flow.observe(added, observerOfWhatWasLookedUp());
        if (m_flow.testRuns().isNameAdded(added))
            return LookupInLinkAccesses::Unknown;
    }
    auto& lookups = m_parts.state.lookups[m_flow.indexOfLaneAtWork()];
    uint64_t question = static_cast<uint64_t>(start.index()) << 32 | m_access.key.index();
    if (auto known = lookups.find(question); known != lookups.end()) {
        member = BuiltinID(known->value & 0xffff);
        return static_cast<LookupInLinkAccesses>(known->value >> 16);
    }
    LookupInLinkAccesses outcome = lookUpInTableFrom(start, m_access.key, member);
    lookups.add(question, static_cast<uint32_t>(outcome) << 16 | member.index());
    return outcome;
}

LookupInLinkAccesses AccessInLinkAccesses::lookUpInTableFrom(BuiltinID start, PropertyKeyID key, BuiltinID& member) const
{
    MembersUnderKey members(m_program, key);
    BuiltinID holder = m_table.canonicalID(start);
    while (true) {
        if (compiledModuleWithObject(m_program, holder) || m_table.at(m_table.rootOf(holder)).scope != BuiltinScope::Realm)
            return LookupInLinkAccesses::Unknown;
        member = members.findOwnMember(holder);
        if (member)
            return LookupInLinkAccesses::Found;
        if (!listsEveryMemberUnder(m_program, holder, key) || !members.isClosedKey(holder) || !m_table.hasLockedPrototypeLink(holder))
            return LookupInLinkAccesses::Unknown;
        const Builtin& object = m_table.at(holder);
        switch (object.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return LookupInLinkAccesses::Unknown;
        case BuiltinPrototypeLink::Null:
            return LookupInLinkAccesses::Absent;
        case BuiltinPrototypeLink::Builtin:
            holder = m_table.canonicalID(object.inheritsFrom);
            break;
        }
    }
}

bool AccessInLinkAccesses::tellsWhatMayAnswerFrom(BuiltinID start)
{
    if (m_flow.testRuns().isTestRun())
        return false;
    MembersUnderKey members(m_program, m_access.key);
    BuiltinID holder = m_table.canonicalID(start);
    while (true) {
        if (compiledModuleWithObject(m_program, holder) || m_table.at(m_table.rootOf(holder)).scope != BuiltinScope::Realm || !(m_table.hasLockedPrototypeLink(holder) || m_table.at(holder).flags.contains(BuiltinFlag::IsGlobalObject)))
            return false;
        if (BuiltinID member = members.findOwnMember(holder)) {
            m_flow.note(LinkFlow::Note::BuiltinRead, member.index());
            m_flow.noteLossOf(m_table.canonicalID(member));
            if (members.isLockedLocation(holder))
                return true;
        }
        const Builtin& object = m_table.at(holder);
        switch (object.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return false;
        case BuiltinPrototypeLink::Null:
            return true;
        case BuiltinPrototypeLink::Builtin:
            holder = m_table.canonicalID(object.inheritsFrom);
            break;
        }
    }
}

bool AccessInLinkAccesses::tellsWhatMayAnswerUnderAnyNameBelow(BuiltinClassID builtinClass)
{
    if (!isRead() || !builtinClass || m_flow.testRuns().isTestRun() || m_keyCannotNameBuiltin || m_access.answerIsOnlyCalled || m_access.keyIsNameThatLoopOverSubjectGave)
        return false;
    Vector<BuiltinID, 8> holders;
    bool hasClassBelow = m_table.hasClassBelow(builtinClass);
    for (unsigned index = hasClassBelow ? 0 : builtinClass.index(); index < (hasClassBelow ? m_table.classCount() : builtinClass.index() + 1); ++index) {
        BuiltinClassID below(index);
        if (!below || !m_table.isSameOrSubclassOf(below, builtinClass))
            continue;
        BuiltinID holder = prototypeOfInstancesOf(below);
        while (holder) {
            holder = m_table.canonicalID(holder);
            if (holders.contains(holder))
                break;
            const Builtin& object = m_table.at(holder);
            if (compiledModuleWithObject(m_program, holder) || m_table.at(m_table.rootOf(holder)).scope != BuiltinScope::Realm || !m_table.hasLockedPrototypeLink(holder)
                || !(object.flags.contains(BuiltinFlag::ListsAllMembers) || object.origin == BuiltinOrigin::Engine) || m_program.isSomeNameOpenedOn(holder) || object.prototypeLink == BuiltinPrototypeLink::Unknown)
                return false;
            holders.append(holder);
            holder = object.prototypeLink == BuiltinPrototypeLink::Builtin ? object.inheritsFrom : BuiltinID();
        }
    }
    if (holders.isEmpty() || lookupPassesHolderLinkedAnew(m_program, holders[0], { }))
        return false;
    for (BuiltinID holder : holders)
        m_flow.note(LinkFlow::Note::ReadUnderKeyNobodyKnowsOffHolder, holder.index());
    if (!m_shared && !m_flow.isInLibraryCode() && m_access.site.index() < 1u << 24)
        m_flow.note(LinkFlow::Note::ReadOfProgramUnderKeyNobodyKnows, m_function.index() + 1, m_access.site.index());
    return true;
}

bool AccessInLinkAccesses::areListsThatOnlyProgramNames(const LinkType& instances)
{
    if (instances.classes != LinkType::Classes::Site || instances.builtinClass != BuiltinClasses::Array || !m_flow.isOfClassItself(instances.site) || !m_flow.isCreatedWithoutEngineNames(instances.site))
        return false;
    for (SlotOfContents slot : { SlotOfContents::mayBeOfClassBelow(), SlotOfContents::mayHaveNamesOfEngine() }) {
        FlowNodeID mark = m_flow.contentsOf(instances, slot);
        tellAgainOnceClimbsWhoeverTells(mark);
        if (!m_flow.typeAt(mark).isNever())
            return false;
    }
    return true;
}

bool AccessInLinkAccesses::isNameUndefinedFrom(BuiltinID start, BuiltinID followed)
{
    if (!isNamed() || m_program.at(m_access.key).kind != PropertyKeyKind::Text || mayBeElementKey(m_nameClass) || m_nameClass == NameClass::Proto)
        return false;
    if (FlowNodeID added = m_flow.testRuns().nodeOfNameAdded(m_access.key)) {
        tellAgainOnceClimbsWhoeverTells(added);
        if (m_flow.testRuns().isNameAdded(added))
            return false;
    }
    if (!isKeyAbsentFromChain(start))
        return false;
    MembersUnderKey members(m_program, m_access.key);
    auto tellAgainAt = [&](FlowNodeID laid) {
        tellAgainOnceClimbsWhoeverTells(laid);
        return true;
    };
    for (BuiltinID holder = m_table.canonicalID(start); ; holder = m_table.canonicalID(m_table.at(holder).inheritsFrom)) {
        bool isGlobalObject = m_flow.isGlobalObject(holder);
        if (!m_table.hasLockedPrototypeLink(holder) && !isGlobalObject)
            return false;
        bool isClosed = members.isClosedKey(holder) || (isGlobalObject || holder == followed ? m_flow.followsBuiltinUnder(holder, m_access.key, tellAgainAt) : !!m_flow.stillNothing(holder, m_access.key, tellAgainAt));
        if (!isClosed)
            return false;
        if (m_table.at(holder).prototypeLink != BuiltinPrototypeLink::Builtin)
            return true;
    }
}

bool AccessInLinkAccesses::isFollowedOnGlobalObject(BuiltinID builtin)
{
    BuiltinID canonical = m_table.canonicalID(builtin);
    if (!m_flow.isGlobalObject(canonical) || !isNamed() || m_flow.testRuns().isTestRun() || !hasFlag(m_program.excludedCode, Wire::ExcludedCode::UnlistedGlobals))
        return false;
    static constexpr std::array namesDefinedByOptions { "$vm"_s, "Decimal"_s, "Loader"_s, "gc"_s };
    const PropertyKey& key = m_program.at(m_access.key);
    if (key.kind != PropertyKeyKind::Text)
        return false;
    Vector<char16_t> buffer;
    StringView name = m_program.strings.viewOf(key.text, buffer);
    if (name.startsWith("__"_s) || std::ranges::any_of(namesDefinedByOptions, [&](ASCIILiteral laid) { return name == laid; }))
        return false;
    return isNameUndefinedFrom(canonical);
}

bool AccessInLinkAccesses::isFollowedOnBuiltin(BuiltinID builtin)
{
    BuiltinID canonical = m_table.canonicalID(builtin);
    if (m_flow.isGlobalObject(canonical))
        return isFollowedOnGlobalObject(builtin);
    if (canonical != m_table.canonicalID(Builtins::Object) || !isNamed() || m_flow.testRuns().isTestRun() || !hasFlag(m_program.excludedCode, Wire::ExcludedCode::UnlistedGlobals))
        return false;
    return isNameUndefinedFrom(canonical, canonical);
}

bool AccessInLinkAccesses::giveWhatIsFoundFrom(BuiltinID start, EngineMayDefineInheritedNames engineMayDefineInheritedNames)
{
    BuiltinID member;
    switch (lookUpFrom(start, member)) {
    case LookupInLinkAccesses::Absent:
        givePrimitives(Primitive::Undefined);
        return true;
    case LookupInLinkAccesses::Found: {
        m_flow.note(LinkFlow::Note::BuiltinRead, member.index());
        LinkType held = typeHeldAtLocation(member);
        if (m_flowBroughtHolder && held.builtin)
            m_flow.note(LinkFlow::Note::BuiltinFoundWhereFlowBroughtHolder, m_table.canonicalID(held.builtin).index(), m_function.index());
        if (PropertyKeyID symbol = held == primitivesForLinkAccesses(Primitive::Symbol) ? keyOfWellKnownSymbol(m_program, member) : PropertyKeyID())
            giveSymbol(symbol);
        else
            give(held);
        return !held.hasUnknownObjects;
    }
    case LookupInLinkAccesses::Unknown:
        if (engineMayDefineInheritedNames == EngineMayDefineInheritedNames::No && isRead() && isNameUndefinedFrom(start)) {
            givePrimitives(Primitive::Undefined);
            return true;
        }
        cannotTell();
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool AccessInLinkAccesses::mayRunCodeFoundFrom(BuiltinID start)
{
    BuiltinID member;
    switch (lookUpFrom(start, member)) {
    case LookupInLinkAccesses::Absent:
        return false;
    case LookupInLinkAccesses::Found:
        return (!isRead() && m_table.at(member).kind == BuiltinKind::Accessor) || typeHeldAtLocation(member).hasUnknownObjects;
    case LookupInLinkAccesses::Unknown:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool AccessInLinkAccesses::nativeSetterFoundFromKeepsReceiver(BuiltinID start)
{
    BuiltinID member;
    if (!m_value || lookUpFrom(start, member) != LookupInLinkAccesses::Found || m_table.at(member).kind != BuiltinKind::Accessor)
        return false;
    bool hasSetter = false;
    for (const BuiltinOverload& overload : m_table.overloadsOf(member)) {
        if (overload.mode != BuiltinCallMode::Set)
            continue;
        static constexpr OperandUses usesThatLeaveReceiverOtherThanMade {
            OperandUse::StoresByKey, OperandUse::DeletesByKey, OperandUse::AssignsFrom, OperandUse::DefinesName, OperandUse::TakesDescriptor,
            OperandUse::ChangesIntegrity, OperandUse::ChangesPrototype, OperandUse::BecomesPrototype,
        };
        static constexpr OperandUses usesThatLeaveReceiverAsMade {
            OperandUse::Inspects, OperandUse::Retains, OperandUse::Returns, OperandUse::Calls, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly,
            OperandUse::ReadsContents, OperandUse::WritesContents, OperandUse::RetainsContents, OperandUse::MovesElements, OperandUse::PassesToUserCode,
            OperandUse::AppliesTransitively, OperandUse::AppliesToContents,
        };
        static_assert(everyOperandUseIsInExactlyOneOf({ usesThatLeaveReceiverOtherThanMade, usesThatLeaveReceiverAsMade }));
        const BuiltinParameter* receiver = m_table.parameterFor(overload, BuiltinOperand::receiver());
        if (overload.isUndeclared() || (receiver && receiver->uses.containsAny(usesThatLeaveReceiverOtherThanMade)))
            return false;
        hasSetter = true;
    }
    if (!hasSetter || !holdsWhatTableLists(member))
        return false;
    tellAgainOnceClimbs(m_value);
    if (holdsObjectsForLinkAccesses(m_flow.typeAt(m_value)))
        return false;
    m_flow.note(LinkFlow::Note::BuiltinRead, member.index());
    return true;
}

LinkType AccessInLinkAccesses::typeHeldAtLocation(BuiltinID member)
{
    LinkType held = typeHeldAtLocationThatRealmHas(member);
    if (m_table.has(BuiltinMark::MayBeAbsent, member))
        held.primitives |= Primitive::Undefined;
    return held;
}

bool AccessInLinkAccesses::holdsWhatTableLists(BuiltinID member)
{
    const Builtin& entry = m_table.at(member);
    bool isLockedByTable = entry.mutability == BuiltinMutability::AlwaysLocked || m_table.isLocationOfLockedObject(member);
    if (entry.scope != BuiltinScope::Realm || entry.flags.contains(BuiltinFlag::IsOwnPropertyOfInstances)
        || m_program.isUnlocked(member) || m_program.isUnlocked(m_table.canonicalID(member)) || (!isLockedByTable && !m_flow.takeForConstant(member)))
        return false;
    if (!isLockedByTable) {
        m_hasLeanedOnMemberTakenForConstant = true;
        tellAgainOnceClimbs(m_flow.writtenAtMember(member));
    }
    return true;
}

LinkType AccessInLinkAccesses::typeHeldAtLocationThatRealmHas(BuiltinID member)
{
    const Builtin& entry = m_table.at(member);
    BuiltinID canonical = m_table.canonicalID(member);
    if (!holdsWhatTableLists(member)) {
        mayThrow();
        anythingInPlaceOf(canonical);
        return m_flow.anythingBeside(entry.scope == BuiltinScope::Realm && isObjectKind(m_table.at(canonical).kind) ? canonical : BuiltinID(), m_function);
    }

    switch (entry.kind) {
    case BuiltinKind::Namespace:
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Prototype:
    case BuiltinKind::Method: {
        if (compiledModuleWithObject(m_program, canonical))
            return LinkType::anything();
        LinkType type;
        type.builtin = canonical;
        return type;
    }
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
        return typeAnsweredByReadsOf(member);
    case BuiltinKind::Constant:
        if (std::optional<double> number = m_table.numberOf(member)) {
            uint16_t primitive = static_cast<uint16_t>(primitiveOfNumber(*number));
            return primitivesForLinkAccesses(LinkType::primitivesOf(primitive, primitive == Primitive::Integer ? Range::one(static_cast<int64_t>(*number)) : Range::empty()));
        }
        if (m_table.nameOfSymbol(member))
            return primitivesForLinkAccesses(Primitive::Symbol);
        if (m_table.stringOf(member))
            return primitivesForLinkAccesses(Primitive::String);
        return LinkType::anything();
    case BuiltinKind::NativeBinding:
        return LinkType::anything();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

LinkType AccessInLinkAccesses::anythingInPlaceOf(BuiltinID canonical)
{
    if (m_flow.lightFormOf(canonical) && (m_shared || !m_flow.isSpelledInBody(m_function, m_access.subject)))
        m_flow.noteLossOf(canonical);
    return LinkType::anything();
}

LinkType AccessInLinkAccesses::typeAnsweredByReadsOf(BuiltinID member)
{
    BuiltinID canonical = m_table.canonicalID(member);
    bool standsForLightForm = m_flow.lightFormOf(canonical);
    if (standsForLightForm) {
        tellAgainOnceClimbs(m_flow.writtenAtMember(canonical));
        standsForLightForm = !m_flow.isAskedForOtherNames(canonical);
    }
    LinkType answered;
    bool hasRow = false;
    for (const BuiltinOverload& overload : m_table.overloadsOf(member)) {
        if (overload.mode != BuiltinCallMode::Get)
            continue;
        if (overload.throws())
            mayThrow();
        if (overload.isUndeclared() || overload.resultIsDeclaredOnly())
            return anythingInPlaceOf(canonical);
        LinkType written = standsForLightForm ? typeWrittenOfLightForm(overload.result.type, canonical) : typeWrittenBy(overload.result.type);
        if (standsForLightForm && !written.builtin)
            return anythingInPlaceOf(canonical);
        answered = m_flow.joinOf(answered, written);
        hasRow = true;
    }
    return hasRow ? answered : anythingInPlaceOf(canonical);
}

LinkType AccessInLinkAccesses::typeWrittenOfLightForm(BuiltinTypeID id, BuiltinID canonical) const
{
    if (!id || m_table.at(id).isAnything())
        return LinkType::anything();
    const BuiltinType& written = m_table.at(id);
    LinkType type = primitivesForLinkAccesses(LinkType::primitivesOf(primitivesOfBuiltinType(written), integerRangeWrittenBy(written)));
    if (!written.stringLiterals.empty())
        type.primitives |= LinkType::closed(Primitive::String);
    type.builtin = canonical;
    return type;
}

LinkType AccessInLinkAccesses::typeWrittenBy(BuiltinTypeID id)
{
    if (!id)
        return LinkType::anything();
    const BuiltinType& written = m_table.at(id);
    if (written.isAnything() || written.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasAnyFunction, BuiltinTypeFlag::HasPlainData }))
        return LinkType::anything();

    LinkType type = primitivesForLinkAccesses(LinkType::primitivesOf(primitivesOfBuiltinType(written), integerRangeWrittenBy(written)));
    if (!written.stringLiterals.empty())
        type.primitives |= LinkType::closed(Primitive::String);
    for (const BuiltinTypePart& part : written.parts) {
        if (part.kind == BuiltinTypePartKind::Operand && part.operand->isReceiver() && isRead() && m_value) {
            giveFrom(m_receiver);
            continue;
        }
        if (part.kind == BuiltinTypePartKind::Instance) {
            LinkType instances = LinkType::anything();
            instances.holdsLostBuiltins = m_flow.unseenObjectsBelow(m_flow.prototypeOfInstancesOf(part.builtinClass), m_function).holdsLostBuiltins;
            return instances;
        }
        if (part.kind != BuiltinTypePartKind::Builtin)
            return LinkType::anything();
        BuiltinID builtin = m_table.canonicalID(part.builtin);
        if (type.builtin && type.builtin != builtin)
            return LinkType::anything();
        type.builtin = builtin;
    }
    return type;
}

PropertyKeyID LinkFlow::keyOf(FunctionID function, const SummaryAccess& access) const
{
    if (access.key || access.keyNode == noSummaryNode || access.isToContents())
        return access.key;
    return keyHeldAt(function, access.keyNode);
}

PropertyKeyID LinkFlow::keyHeldAt(FunctionID function, uint32_t keyNode) const
{
    return keyHeldAt(function, nodeOf(function, keyNode));
}

PropertyKeyID LinkFlow::keyHeldAt(FunctionID function, FlowNodeID node) const
{
    LinkType oneSymbol;
    oneSymbol.primitives = LinkType::oneSymbol;
    LinkType held = typeAt(node);
    held.primitives &= LinkType::closed(ViewAtWork::current() ? Primitive::All & ~Primitive::UndefinedOrNull : Primitive::All);
    if (held != oneSymbol)
        return { };
    PropertyKeyID symbol = symbolAt(node);
    if (!symbol || !isNameInTextOf(program, symbol, function))
        return { };
    if (ViewAtWork::current()) {
        if (isSymbolOfLibrary(program, symbol) && m_symbolsInSight.goesByValue(symbol))
            return { };
        return symbol;
    }
    if (isSymbolOfLibrary(program, symbol) && goesByValue(symbol))
        return { };
    return symbol;
}

ALWAYS_INLINE_INTO_OTHER_FILES PropertyKeyID LinkFlow::keySeenOf(FunctionID function, const SummaryAccess& access) const
{
    if (access.key || access.keyNode == noSummaryNode || access.isToContents())
        return { };
    FlowNodeID node = nodeOf(function, access.keyNode);
    const LinkType& held = typeAt(node);
    if (!isSymbolAsFarAsIsSeen(held) || !(held.directPrimitives() & LinkType::oneSymbol) || !(held.primitives & LinkType::closed(Primitive::All) & ~LinkType::oneSymbol))
        return { };
    PropertyKeyID symbol = symbolNotedAt(node);
    if (!symbol || !isNameInTextOf(program, symbol, function))
        return { };
    if (isSymbolOfLibrary(program, symbol) && m_symbolsInSight.goesByValue(symbol))
        return { };
    return symbol;
}

void LinkFlow::noteKeyOfAccessInLibrary(FunctionID function, uint32_t record, const SummaryAccess& access)
{
    if (PropertyKeyID seen = symbolSeenAt(nodeOf(function, access.keyNode)); seen && isSymbolOfLibrary(program, seen) && isLibraryCode(program, program.at(function)) && !m_symbolsInSight.everySymbolOfLibraryMayHaveOne && !keyOf(function, access) && !keySeenOf(function, access)) {
        LockerOfWhatIsShared lockerOfWhatIsShared(*this);
        if (!m_symbolsInSight.besideOtherKeys.contains(seen.bits()))
            m_symbolsInSight.notedBesideOtherKeys.append(seen);
    }
    PropertyKeyID symbol = symbolAt(nodeOf(function, access.keyNode));
    if (!symbol || !isSymbolOfLibrary(program, symbol) || !isLibraryCode(program, program.at(function)))
        return;
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (keyOf(function, access)) {
        if (m_accessesUnderSymbolsOfLibraryListed.add(static_cast<uint64_t>(function.index()) << 32 | record).isNewEntry)
            m_accessesUnderSymbolsOfLibrary.append({ symbol, FlowObserver { FlowObserver::Kind::KeyOfAccess, function, record } });
        return;
    }
    if (m_everySymbolOfLibraryMayHavePreparation)
        return;
    if (!m_symbolsOfLibraryBesideOtherKeys.contains(symbol.bits()))
        m_symbolsNotedBesideOtherKeys.append(symbol);
}

bool LinkFlow::letSymbolsThatShareFunctionGoByValue(const Vector<Preparation>& preparations, const SetOfNumbers& besideOtherKeys, SetOfNumbers& goByValue)
{
    bool hasGrown = false;
    for (bool grows = true; std::exchange(grows, false);) {
        for (const Preparation& one : preparations) {
            if (!besideOtherKeys.contains(one.symbol.bits()) && !goByValue.contains(one.symbol.bits()))
                continue;
            for (const Preparation& other : preparations)
                grows |= other.function == one.function && goByValue.add(other.symbol.bits()).isNewEntry;
        }
        hasGrown |= grows;
    }
    return hasGrown;
}

bool LinkFlow::takeInWhatWasNotedOfSymbols()
{
    for (PropertyKeyID symbol : std::exchange(m_symbolsNotedWithoutTheirNames, { })) {
        m_symbolsInSight.notedBesideOtherKeys.append(symbol);
        m_symbolsNotedBesideOtherKeys.append(symbol);
    }
    {
        SymbolsWithPreparationInSight& inSight = m_symbolsInSight;
        for (PropertyKeyID symbol : std::exchange(inSight.notedBesideOtherKeys, { }))
            inSight.besideOtherKeys.add(symbol.bits());
        for (const Preparation& preparation : std::exchange(inSight.notedWithPreparation, { })) {
            if (!inSight.preparations.contains(preparation))
                inSight.preparations.append(preparation);
        }
        if (std::exchange(inSight.unknownSymbolWasNoted, false))
            inSight.everySymbolOfLibraryMayHaveOne = true;
        letSymbolsThatShareFunctionGoByValue(inSight.preparations, inSight.besideOtherKeys, inSight.goByValue);
    }
    if (m_symbolsNotedBesideOtherKeys.isEmpty() && m_preparationsNoted.isEmpty() && !m_unknownSymbolWasNotedWithPreparation)
        return false;
    bool hasGrown = false;
    for (PropertyKeyID symbol : std::exchange(m_symbolsNotedBesideOtherKeys, { }))
        hasGrown |= m_symbolsOfLibraryBesideOtherKeys.add(symbol.bits()).isNewEntry;
    for (const Preparation& preparation : std::exchange(m_preparationsNoted, { })) {
        m_symbolsWithPreparation.add(preparation.symbol.bits());
        if (m_preparations.contains(preparation))
            continue;
        m_preparations.append(preparation);
        hasGrown = true;
    }
    if (std::exchange(m_unknownSymbolWasNotedWithPreparation, false))
        hasGrown |= !std::exchange(m_everySymbolOfLibraryMayHavePreparation, true);
    if (!hasGrown)
        return false;
    if (letSymbolsThatShareFunctionGoByValue(m_preparations, m_symbolsOfLibraryBesideOtherKeys, m_symbolsThatGoByValue) || m_everySymbolOfLibraryMayHavePreparation)
        tellAccessesUnderSymbolsWithPreparation();
    return true;
}

void LinkFlow::tellAccessesUnderSymbolsWithPreparation()
{
    for (size_t index = 0; index < m_accessesUnderSymbolsOfLibrary.size(); ++index) {
        auto [symbol, observer] = m_accessesUnderSymbolsOfLibrary[index];
        if (goesByValue(symbol))
            tell(observer, typeAt(nodeOf(observer.function, summaries.at(observer.function).accesses[observer.record].keyNode)), LinkType { });
    }
}

void LinkFlow::noteUnknownSymbolHandedToPreparation(UnknownSymbolIs unknownSymbolIs)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (unknownSymbolIs != UnknownSymbolIs::UnknownByEscapeAlone)
        m_symbolsInSight.unknownSymbolWasNoted = true;
    if (unknownSymbolIs != UnknownSymbolIs::UnknownInSightAlone && !m_everySymbolOfLibraryMayHavePreparation)
        m_unknownSymbolWasNotedWithPreparation = true;
}

void LinkFlow::noteSymbolHandedToPreparation(FunctionID caller, FlowNodeID symbol, FlowNodeID function)
{
    FlowObserver observer { FlowObserver::Kind::SymbolOfPreparation, caller, static_cast<uint32_t>(symbol.index()) };
    {
        LockerOfWhatIsShared lockerOfWhatIsShared(*this);
        Vector<uint32_t, 1>& functions = m_nodesOfFunctionsHandedWithSymbolAt.of(*this).add(static_cast<uint32_t>(symbol.index()), Vector<uint32_t, 1> { }).iterator->value;
        if (functions.contains(static_cast<uint32_t>(function.index())))
            return;
        functions.append(static_cast<uint32_t>(function.index()));
        if (functions.size() == 1)
            observe(symbol, observer);
        observe(function, observer);
    }
}

void LinkFlow::preparationObserved(const FlowObserver& observer, const LinkType&)
{
    LinkType held = typeAt(FlowNodeID(observer.record));
    held.primitives &= LinkType::closed(Primitive::All);
    if (held.isNever())
        return;
    Vector<FunctionID, 1> functions;
    {
        LockerOfWhatIsShared lockerOfWhatIsShared(*this);
        for (uint32_t node : m_nodesOfFunctionsHandedWithSymbolAt.of(*this).get(observer.record)) {
            LinkType function = typeAt(FlowNodeID(node));
            function.primitives = 0;
            if (function.isNever())
                continue;
            if (function != closureForLinkAccesses(function.function)) {
                functions.clear();
                held = LinkType::anything();
                break;
            }
            functions.append(function.function);
        }
    }
    LinkType oneSymbol;
    oneSymbol.primitives = LinkType::oneSymbol;
    if (LinkType seen = held.directPart(); !seen.isNever()) {
        PropertyKeyID symbolSeen = seen == oneSymbol ? symbolSeenAt(FlowNodeID(observer.record)) : PropertyKeyID();
        if (!symbolSeen)
            noteUnknownSymbolHandedToPreparation(UnknownSymbolIs::UnknownInSightAlone);
        else {
            LockerOfWhatIsShared lockerOfWhatIsShared(*this);
            for (FunctionID function : functions)
                m_symbolsInSight.notedWithPreparation.append({ symbolSeen, function });
        }
    }
    PropertyKeyID symbol = held == oneSymbol ? symbolAt(FlowNodeID(observer.record)) : PropertyKeyID();
    if (!symbol) {
        noteUnknownSymbolHandedToPreparation(UnknownSymbolIs::UnknownByEscapeAlone);
        return;
    }
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    for (FunctionID function : functions)
        m_preparationsNoted.append({ symbol, function });
}

void LinkFlow::accessObserved(const FlowObserver& observer, const LinkType&)
{
    PrivatePartsInLinkAccesses parts { typeObservedBefore(), m_firstFieldOfLayout, stateOfLinkAccesses() };
    if (observer.kind == FlowObserver::Kind::MemberOfSharedAccess) {
        const SharedAccessInLinkAccesses& shared = *parts.state.sharedAccesses[parts.state.membersOfSharedAccesses[observer.record].sharedAccess];
        AccessInLinkAccesses(*this, parts, shared, observer.record, false).resolveMemberOfSharedAccess();
        return;
    }
    [[clang::always_inline]] AccessInLinkAccesses(*this, parts, observer).resolve();
    if (PropertyKeyID symbol = keySeenOf(observer.function, summaries.at(observer.function).accesses[observer.record])) {
        AccessInLinkAccesses underSymbol(*this, parts, observer);
        underSymbol.goByKey(symbol);
        underSymbol.resolve();
    }
}

void LinkFlow::storeOnClassHolder(FunctionID writer, LayoutID classLayout, ClassSide side, PropertyKeyID key, FlowNodeID value)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    PrivatePartsInLinkAccesses parts { typeObservedBefore(), m_firstFieldOfLayout, stateOfLinkAccesses() };
    auto made = makeUniqueWithoutFastMallocCheck<SharedAccessInLinkAccesses>();
    bool isOnPrototype = side == ClassSide::Prototype;
    made->subjects = isOnPrototype ? SharedAccessInLinkAccesses::Subjects::PrototypeObjects : SharedAccessInLinkAccesses::Subjects::ClassObjects;
    made->firstFunction = writer;
    made->access.kind = SummaryAccess::Kind::Store;
    made->access.key = key;
    made->subject = makeSharedNode();
    made->value = value;
    made->thrown = makeSharedNode();
    arrive(made->subject, isOnPrototype ? prototypeForLinkAccesses(classLayout) : closureForLinkAccesses(program.at(classLayout).constructor));
    addEdge(made->thrown, thrown(writer));
    uint32_t member = static_cast<uint32_t>(parts.state.membersOfSharedAccesses.size());
    parts.state.membersOfSharedAccesses.append(MemberOfSharedAccessInLinkAccesses { static_cast<uint32_t>(parts.state.sharedAccesses.size()), classLayout });
    parts.state.sharedAccesses.append(WTF::move(made));
    AccessInLinkAccesses(*this, parts, *parts.state.sharedAccesses.last(), member, true).resolveMemberOfSharedAccess();
}

StateOfLinkAccesses& LinkFlow::makeStateOfLinkAccesses()
{
    if (!m_stateOfLinkAccesses) {
        m_stateOfLinkAccesses.reset(new StateOfLinkAccesses);
        m_stateOfLinkAccesses->lookups.grow(numberOfLanes());
        m_stateOfLinkAccesses->whatBlocksRunBehindSteps.grow(numberOfLanes());
        m_stateOfLinkAccesses->codeFoundAboveClosures = BytesSharedByThreads(4 * static_cast<size_t>(program.types.numberOfPropertyKeys()));
        m_stateOfLinkAccesses->classesWhoseStaticFieldsAreLetGo = BytesSharedByThreads(program.layouts.size());
        BuiltinID globalObject = program.builtins().canonicalID(Builtins::GlobalThis);
        BitVector namesOfOtherValues;
        using GlobalNameStored = std::pair<PropertyKeyID, bool>;
        for (auto [name, storesFunction] : collectInParallel<GlobalNameStored>(loops.ofCurrentThread(), static_cast<unsigned>(summaries.size()), [&](unsigned number, Vector<GlobalNameStored>& found) {
            const ThinSummary& summary = summaries.at(FunctionID(number));
            for (const SummaryAccess& access : summary.accesses) {
                if (access.kind != SummaryAccess::Kind::Store || !access.key)
                    continue;
                const SummaryNode& subject = summary.nodes[access.subject];
                if (subject.kind != SummaryNode::Kind::Builtin || program.builtins().canonicalID(BuiltinID(subject.first)) != globalObject)
                    continue;
                found.append({ access.key, access.value != noSummaryNode && summary.nodes[access.value].kind == SummaryNode::Kind::Closure });
            }
        }))
            (storesFunction ? m_stateOfLinkAccesses->globalNamesStored : namesOfOtherValues).set(name.index());
        m_stateOfLinkAccesses->globalNamesStored.exclude(namesOfOtherValues);
        m_stateOfLinkAccesses->nearestClassesWithStaticFieldUnderKey.grow(numberOfLanes());

        static constexpr uint32_t isNotAsked = UINT32_MAX - 1;
        Vector<uint32_t>& nearest = m_stateOfLinkAccesses->nearestClassesWithStaticFields;
        nearest.fill(isNotAsked, program.layouts.size());
        Vector<uint32_t, 16> passed;
        for (uint32_t first = 0; first < program.layouts.size(); ++first) {
            uint32_t found = UINT32_MAX;
            passed.shrink(0);
            for (LayoutID current = LayoutID(first); current && passed.size() < program.layouts.size(); current = program.at(current).base) {
                if (nearest[current.index()] != isNotAsked) {
                    found = nearest[current.index()];
                    break;
                }
                passed.append(current.index());
                if (!program.at(current).staticFields.isEmpty()) {
                    found = current.index();
                    break;
                }
            }
            for (uint32_t layout : passed)
                nearest[layout] = found;
        }
    }
    return *m_stateOfLinkAccesses;
}

LayoutID LinkFlow::nearestClassWithStaticFieldThatMayMatch(LayoutID from, PropertyKeyID key)
{
    StateOfLinkAccesses& state = stateOfLinkAccesses();
    auto& nearestUnderKey = state.nearestClassesWithStaticFieldUnderKey[indexOfLaneAtWork()];
    auto wordOf = [&](LayoutID layout) {
        return static_cast<uint64_t>(layout.index()) << 32 | key.bits();
    };
    LayoutID found;
    Vector<LayoutID, 8> passed;
    size_t stepsLeft = program.layouts.size();
    for (LayoutID current = from; current && stepsLeft--; current = program.at(current).base) {
        uint32_t withFields = state.nearestClassesWithStaticFields[current.index()];
        if (withFields == UINT32_MAX)
            break;
        current = LayoutID(withFields);
        bool mayMatch = false;
        program.at(current).forEachStaticFieldThatMayMatch(key, [&](const StaticField&) {
            mayMatch = true;
        });
        if (mayMatch) {
            found = current;
            break;
        }
        if (auto known = nearestUnderKey.find(wordOf(current)); known != nearestUnderKey.end()) {
            found = known->value == UINT32_MAX ? LayoutID() : LayoutID(known->value);
            break;
        }
        passed.append(current);
    }
    for (LayoutID layout : passed)
        nearestUnderKey.add(wordOf(layout), found ? found.index() : UINT32_MAX);
    return found;
}

void LinkFlow::prepareAccessesForSeveralThreads()
{
    stateOfLinkAccesses();
    if (m_nodesFoundWhereFieldsAreEmpty.isEmpty())
        m_nodesFoundWhereFieldsAreEmpty.fill(UINT32_MAX, m_firstFieldOfLayout.last());
    if (m_newestNameOnClosuresOf.isEmpty())
        m_newestNameOnClosuresOf.fill(UINT32_MAX, program.functions.size());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
