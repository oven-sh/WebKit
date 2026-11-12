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
#include "AOTFactRecords.h"
#include "AOTIRProgram.h"
#include "AOTLinkCoroutines.h"
#include "AOTLinkUnions.h"
#include "AOTOpcodeSets.h"
#include "AOTTypeRelations.h"
#include "AOTLinkExceptions.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <wtf/Noncopyable.h>
#include <wtf/SetForScope.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

static LinkType typeOfObjectsForLinkReach(LayoutID layout)
{
    LinkType type;
    type.objects = LinkType::Objects::Layout;
    type.layout = layout;
    return type;
}

static LinkType typeOfClosuresForLinkReach(FunctionID function)
{
    LinkType type;
    type.functions = LinkType::Functions::Closure;
    type.function = function;
    return type;
}

static LinkType typeOfBuiltinForLinkReach(const Program& program, BuiltinID canonical)
{
    switch (program.builtins().at(canonical).kind) {
    case BuiltinKind::Namespace:
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Prototype:
    case BuiltinKind::Method: {
        if (compiledModuleWithObject(program, canonical))
            return LinkType::anything();
        LinkType type;
        type.builtin = canonical;
        return type;
    }
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
    case BuiltinKind::Constant:
    case BuiltinKind::NativeBinding:
        return LinkType::anything();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

PropertyKeyID keyOfWellKnownSymbol(const Program& program, BuiltinID constant)
{
    BuiltinName name = program.builtins().nameOfSymbol(constant);
    return name ? program.keyOf(name) : PropertyKeyID();
}

PropertyKeyID provenSymbolAt(const Program& program, const ThinSummaries& summaries, FunctionID function, const BodyIndex& index, ValueID value)
{
    while (value && !index.isParameter(value) && inputPassThroughOpcodes.contains(index.opcodeOf(value)))
        value = index.value(value).operand(0);
    if (!value || index.isParameter(value))
        return { };
    BodyValue definition = index.value(value);
    if (definition.opcode() == Opcode::Builtin)
        return keyOfWellKnownSymbol(program, program.builtins().canonicalID(definition.as<Opcode::Builtin>().builtin()));
    if (definition.opcode() == Opcode::GetModuleVariable)
        return summaries.symbolHeldBy(definition.as<Opcode::GetModuleVariable>().variable());
    if (definition.opcode() != Opcode::Call)
        return { };
    std::span<const SummaryKeyAtSite> symbols = summaries.at(function).symbolsMadeOnce.span();
    auto found = std::ranges::lower_bound(symbols, value, { }, &SummaryKeyAtSite::site);
    return found != symbols.end() && found->site == value ? found->key : PropertyKeyID();
}
static LinkType typeOfUnseenObjectsForLinkReach()
{
    LinkType type;
    type.hasUnknownObjects = true;
    return type;
}

static bool isMadeByRunTimeAloneForLinkReach(LayoutKind kind)
{
    switch (kind) {
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        return true;
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Functor>
static void forEachLayoutMadeAtNoNodeForLinkReach(const Program& program, const Functor& functor)
{
    Vector<const DataNode*, 32> nodesToVisit;
    for (const Module& module : program.modules) {
        if (!module.data)
            continue;
        nodesToVisit.append(&program.at(module.data).root);
        while (!nodesToVisit.isEmpty()) {
            const DataNode& node = *nodesToVisit.takeLast();
            switch (node.kind()) {
            case DataNode::Kind::Undefined:
            case DataNode::Kind::Null:
            case DataNode::Kind::Hole:
            case DataNode::Kind::Boolean:
            case DataNode::Kind::Number:
            case DataNode::Kind::String:
            case DataNode::Kind::BigInt:
                break;
            case DataNode::Kind::Array:
                for (const DataNode& element : node.as<DataNode::Array>().elements)
                    nodesToVisit.append(&element);
                break;
            case DataNode::Kind::Object: {
                const auto& object = node.as<DataNode::Object>();
                for (const DataMember& member : object.members)
                    nodesToVisit.append(&member.value);
                if (object.layout)
                    functor(object.layout);
                break;
            }
            }
        }
    }

    for (const ExpectedResult& expected : program.expectedResults) {
        if (!expected.flags.contains(ExpectedResultFlags::IsMadeByParser))
            continue;
        for (LayoutID layout : expected.layouts) {
            if (parsersCouldMakeRecordsOf(program, layout))
                functor(layout);
        }
    }

    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (isMadeByRunTimeAloneForLinkReach(layout.kind) || layout.has(LayoutFlags::IsOfCreatedObject))
            functor(LayoutID(index));
    }
}

class StateOfLinkReach {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(StateOfLinkReach);
    WTF_MAKE_NONCOPYABLE(StateOfLinkReach);
public:
    explicit StateOfLinkReach(LinkFlow&);

    void queueEntries();
    void reach(FunctionID);
    void loseLayout(LayoutID);
    void loseFamily(FamilyID);
    bool hasEscapedAsFamily(FamilyID id) const { return m_escapedFamilies.get(id.index()); }
    void loseFunction(FunctionID);
    void escapeLayout(LayoutID);
    void escapeFunction(FunctionID);
    void escapeAllOf(const LinkType&);
    void arriveFromSource(FlowNodeID, LinkType);
    void listSourceOfObjects(FlowNodeID, LayoutID);
    bool fillParametersThatUnseenCodeAlonePasses();
    FlowNodeID sourceOfFamily(FamilyID id) const { return m_sourcesOfFamilies[id.index()]; }
    void makeSourceOfFamily(FamilyID, FlowNodeID);
    bool enterLibraryInReach();
    bool somebodyCanBeHandedPromises();
    void stopUnlessRecordsHandedOutFrozenStayAsBorn();
    bool somebodyCanClimbToWhatIsLaidAbovePrototype();
    BuiltinsInReach& builtinsInReach() { return m_builtinsInReach; }
    void finish();

private:
    static constexpr uint32_t none = UINT32_MAX;

    enum class Step : uint8_t { EnterProgram, BringAlive, EvaluateModule, TieLostLayout, TieLostFunction, OpenLayout, OpenPrototype, OpenFunction, OpenClassObject };
    enum class Left : uint8_t { LayoutEscapes, PrototypeEscapes, FunctionEscapes, ClassObjectOpens, FunctionIsReached, FunctionIsNamed, ExportsEscape, VariableHasEscaped, FieldMayBeWritten, LayoutMayChange, ParameterIsPassed };

    struct QueuedStep {
        Step step { Step::EnterProgram };
        uint32_t index { 0 };
    };

    struct MemberOfClass {
        ClassSide side { ClassSide::Prototype };
        uint32_t index { 0 };
    };

    struct SourceOfObjects {
        FlowNodeID node;
        uint32_t next { none };
    };

    void take(QueuedStep);
    bool takeStepsOnLanes();
    void leave(Left what, uint32_t index) { m_flow.leaveToReach(static_cast<uint8_t>(what), index); }
    void takeWhatLanesLeft();
    void takeWhatLaneLeft(Left, uint32_t index);

    void enterProgram();
    void fillVariables();
    void evaluate(ModuleID);
    void evaluateModule(ModuleID);
    void noteCodeOfModule(ModuleID);
    void escapeExportsOf(ModuleID);
    void findModulesWhoseValueStaysInSight();
    void escapeValueOf(ModuleID);
    bool hostReadsNamesAloneOff(ModuleID) const;
    enum class HostWith : bool { Looks, Takes };
    void showExportsThatHostReadsOff(const Entry&, HostWith);
    bool globalObjectShowsModuleObjects() const;
    bool showModuleObjectsOfGlobalObject();
    void escapeFacadesWithGetters();
    bool mayHaveAccessor(const LinkType& exports);
    void tellWhatHostFindsAtEntries();
    BitVector findFilesThatHaveRunToTheirEndWhereOthersNameThem() const;
    Vector<ModuleID> findModulesOfBundlesByFunction() const;
    void findVariablesStoredFirst(LinkFlow&, std::span<const ModuleID> modulesOfBundles, BitVector& found) const;
    void escapeVariable(VariableID, bool isExpectedToStayAsLaid = false);
    void escapeField(FieldReference);

    bool mayHold(const BuiltinCondition*, BuiltinID canonical);
    Vector<BuiltinID> m_builtinsWaitingOnConditions;
    void waitOnConditionsOf(BuiltinID canonical);
    void enterLibraryBehind(BuiltinID canonical);
    void enterLibraryFunction(RuntimeFunctionID);
    void letGoWhatHostHandsOver(const BuiltinHostUses&);
    void loadLibraryModule(RuntimeModuleID);
    void letGoWhatNativeCodeReadsOffModules();
    void handOutRecordsOfInternalFiles();
    bool handsBackRecordThatStaysAsBorn(ModuleID);
    Vector<ModuleID> m_modulesThatNativeCodeReadsNamesOff;
    bool m_hasEnteredLoaderFunctionsForTestRun { false };
    bool m_hasEnteredLoaderFunctionsForValueOfHost { false };
    void enterWhatUndeclaredNativeCodeCalls();
    void enterWhatHostLoadsToPrintObjects();
    void enterLoaderOfHost();
    void enterWhatUnseenCodeFinds();
    void letExportsOfEntriesEscape();

    void bringAlive(FunctionID);
    void giveMoreToNodesOf(FunctionID);
    void letGoWhatDirectEvalsShowIn(FunctionID);
    void prefetchSummaryOf(FunctionID) const;
    void prefetchRecordsOf(FunctionID, const ThinSummary&) const;
    SiteID siteAt(FunctionID, std::span<const uint32_t> classInstancesInSummary, uint32_t classInstanceInSummary);
    void fillStateThatNoSummaryFollows(SiteID, bool isObjectOfCoroutine);
    void fillEntryParameters(FunctionID, const Function&);
    void arriveWhatIsEstablished(FlowNodeID, const DeclaredType*);
    void arriveWhatNoNodeBrings(FlowNodeID, const DeclaredType&);
    void fillFields();


    void tieLostLayout(LayoutID);
    void tieLostFunction(FunctionID);

    void escapePrototype(LayoutID);
    void escapeFamily(FamilyID);
    void escapePrototypesOf(FamilyID);
    void escapeConstructorsOf(FamilyID);
    void escapeLostFunctions();
    void openLayout(LayoutID);
    void openPrototype(LayoutID);
    void openFunction(FunctionID);
    void escapeClassObject(LayoutID);
    void openClassObject(LayoutID);
    void shareMembers(LayoutID classLayout, ClassSide);

    uint32_t cellOf(FieldReference reference) const { return m_firstFieldOfLayout[reference.layout.index()] + reference.field; }
    std::span<const MemberOfClass> membersOf(LayoutID id) const LIFETIME_BOUND { return m_membersOfClasses.span().subspan(m_firstMemberOfClass[id.index()], m_firstMemberOfClass[id.index() + 1] - m_firstMemberOfClass[id.index()]); }

    LinkFlow& m_flow;
    const Program& m_program;
    uint32_t m_numberOfLayouts { 0 };
    BitVector m_functionsCalledByHost;

    Vector<QueuedStep> m_steps;
    Vector<QueuedStep> m_stepsThatOneThreadTakes;
    Vector<FunctionID> m_functionsToBringAlive;
    bool m_isWorking { false };

    bool m_hasQueuedEntries { false };
    bool m_lostFunctionsHaveEscaped { false };
    BitVector m_lostFamilies;
    BitVector m_escapedFamilies;
    BitVector m_familiesWithEscapedPrototypes;
    BitVector m_familiesWithEscapedConstructors;
    BitVector m_escapedPrototypes;
    Vector<FlowNodeID> m_parametersThatUnseenCodePasses;
    BitVector m_openClassObjects;
    BitVector m_variablesReadByUnseenCode;
    BitVector m_evaluatedModules;
    BitVector m_scopesAroundDirectEvalsLetGo;
    BytesSharedByThreads m_modulesWithReachedCode;
    bool waitsUntilReachedCodeNamesIt(FunctionID);
    void noteFunctionsNamedBy(FunctionID);
    void noteNamedByReachedCode(FunctionID);
    BitVector m_modulesThatSomeCodeNames;
    Vector<std::pair<FunctionID, FunctionID>> m_functionsDeclaredBesideBodies;
    BitVector m_functionsThatSomeCodeNames;
    BitVector m_functionsThatReachedCodeNames;
    BitVector m_functionsThatWaitToBeNamed;
    BitVector m_modulesWithEscapedExports;
    BitVector m_modulesWhoseValueStaysInSight;
    BitVector m_layoutsOfRecordsHandedOut;
    Vector<ModuleID> m_initiallyTrackedFacades;
    Vector<uint32_t, 1> m_entriesThatHandHostNothing;
    bool m_showsWaysBetweenModuleObjects { false };
    bool m_globalObjectMayShowModuleObjects { false };

    Vector<std::pair<ModuleID, VariableID>> m_variablesOfLibraryFiles;
    BuiltinsInReach m_builtinsInReach;
    Vector<BuiltinID> m_builtinsThatCameIntoReach;
    Vector<BuiltinID> m_builtinsThatCameOutOfSight;
    Vector<BuiltinID> m_membersWrittenAfterAll;
    BitVector m_isCodeBehindBuiltin;
    bool m_nativeCodeMayHoldObjects { false };
    Vector<BuiltinID, 1> m_holdersLinkedAnew;
    Vector<BuiltinID, 24> m_builtinsThatLeadToHoldersLinkedAnew;
    bool m_hasEnteredWhatUndeclaredNativeCodeCalls { false };
    bool m_hasEnteredWhatHostLoadsToPrintObjects { false };
    bool m_hasEnteredLoaderOfHost { false };
    bool m_hasEnteredWhatLoaderOfHostFinds { false };
    bool m_hasLetExportsOfEntriesEscape { false };
    bool m_hasEnteredWhatUnseenCodeFinds { false };

    Vector<ModuleID> m_moduleWithBody;
    Vector<uint32_t> m_firstFieldOfLayout;
    Vector<uint32_t> m_firstMemberOfClass;
    Vector<MemberOfClass> m_membersOfClasses;
    Vector<uint32_t> m_newestSourceOfLayout;
    Vector<SourceOfObjects> m_sourcesOfObjects;
    Vector<FlowNodeID> m_sourcesOfFamilies;
};

StateOfLinkReach::StateOfLinkReach(LinkFlow& flow)
    : m_flow(flow)
    , m_program(flow.program)
    , m_numberOfLayouts(static_cast<uint32_t>(flow.program.layouts.size()))
    , m_functionsCalledByHost(functionsCalledByHost(flow.program, flow.loops))
    , m_builtinsInReach(flow.program, flow.looks, flow.summaries, flow.build)
{
    size_t numberOfFunctions = m_program.functions.size();
    RELEASE_ASSERT(flow.summaries.size() == numberOfFunctions);
    m_isCodeBehindBuiltin.ensureSize(numberOfFunctions);
    for (const RuntimeFunctionEntry& entry : m_program.runtimeFunctions) {
        if (entry.function)
            m_isCodeBehindBuiltin.set(entry.function.index());
    }

    flow.escapedVariables.ensureSize(m_program.variables.size());
    m_lostFamilies = BitVector(flow.families.size());
    m_escapedFamilies = BitVector(flow.families.size());
    m_familiesWithEscapedPrototypes = BitVector(flow.families.size());
    m_familiesWithEscapedConstructors = BitVector(flow.families.size());
    m_escapedPrototypes = BitVector(m_numberOfLayouts);
    m_openClassObjects = BitVector(m_numberOfLayouts);
    m_variablesReadByUnseenCode = BitVector(m_program.variables.size());
    m_evaluatedModules = BitVector(m_program.modules.size());
    m_scopesAroundDirectEvalsLetGo = BitVector(m_program.evalScopes.size());
    m_modulesWithReachedCode = BytesSharedByThreads(m_program.modules.size());
    m_modulesWithEscapedExports = BitVector(m_program.modules.size());
    m_showsWaysBetweenModuleObjects = namesModuleObjects(m_program);
    m_globalObjectMayShowModuleObjects = !m_showsWaysBetweenModuleObjects && reachesModuleObjects(m_program);
    findModulesWhoseValueStaysInSight();

    m_moduleWithBody.fill(ModuleID(), numberOfFunctions);
    for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
        if (FunctionID body = m_program.modules[index].body)
            m_moduleWithBody[body.index()] = ModuleID(index);
    }

    m_functionsThatSomeCodeNames = BitVector(numberOfFunctions);
    m_functionsThatReachedCodeNames = BitVector(numberOfFunctions);
    m_functionsThatWaitToBeNamed = BitVector(numberOfFunctions);
    m_modulesThatSomeCodeNames = BitVector(m_program.modules.size());
    m_firstMemberOfClass.fill(0, m_numberOfLayouts + 2);
    for (size_t index = 0; index < numberOfFunctions; ++index) {
        const ThinSummary& summary = flow.summaries.at(FunctionID(index));
        for (FunctionID named : summary.functionsNamed) {
            if (named.index() != index)
                m_functionsThatSomeCodeNames.quickSet(named.index());
            if (ModuleID module = m_moduleWithBody[named.index()])
                m_modulesThatSomeCodeNames.quickSet(module.index());
        }
        for (ModuleID module : summary.modulesNamedInBlocksNeverEntered)
            m_modulesThatSomeCodeNames.quickSet(module.index());
        for (const SummaryMember& member : summary.members)
            ++m_firstMemberOfClass[member.classLayout.index() + 2];
    }
    for (const Variable& variable : m_program.variables) {
        if (variable.initialization != VariableInitialization::Function || !variable.module)
            continue;
        if (FunctionID body = m_program.at(variable.module).body) {
            m_functionsThatSomeCodeNames.quickSet(variable.function.index());
            m_functionsDeclaredBesideBodies.append({ body, variable.function });
        }
    }
    std::ranges::sort(m_functionsDeclaredBesideBodies);

    m_firstFieldOfLayout.reserveInitialCapacity(m_numberOfLayouts + 1);
    uint32_t numberOfFields = 0;
    for (const Layout& layout : m_program.layouts) {
        m_firstFieldOfLayout.append(numberOfFields);
        numberOfFields += static_cast<uint32_t>(layout.fields.size());
    }
    m_firstFieldOfLayout.append(numberOfFields);

    m_newestSourceOfLayout.fill(none, m_numberOfLayouts);
    m_sourcesOfFamilies.fill(FlowNodeID(), flow.families.size());


    for (uint32_t layout = 0; layout < m_numberOfLayouts; ++layout)
        m_firstMemberOfClass[layout + 2] += m_firstMemberOfClass[layout + 1];
    m_membersOfClasses.grow(m_firstMemberOfClass.last());
    for (size_t index = 0; index < numberOfFunctions; ++index) {
        for (const SummaryMember& member : flow.summaries.at(FunctionID(index)).members)
            m_membersOfClasses[m_firstMemberOfClass[member.classLayout.index() + 1]++] = MemberOfClass { member.side, member.index };
    }
}

void StateOfLinkReach::finish()
{
    if (m_isWorking || m_flow.letsReachWait())
        return;
    SetForScope isWorking(m_isWorking, true);
    Vector<FunctionID> wave;
    while (true) {
        while (!m_stepsThatOneThreadTakes.isEmpty() || !m_steps.isEmpty()) {
            if (!m_stepsThatOneThreadTakes.isEmpty())
                take(m_stepsThatOneThreadTakes.takeLast());
            else if (!takeStepsOnLanes())
                take(m_steps.takeLast());
        }
        if (m_functionsToBringAlive.isEmpty())
            return;
        wave.shrink(0);
        std::swap(wave, m_functionsToBringAlive);
        if (m_flow.bringAliveOnLanes(wave.span(), scopedLambda<void(FunctionID)>([&](FunctionID function) {
            bringAlive(function);
        }))) {
            takeWhatLanesLeft();
            continue;
        }
        for (size_t index = 0; index < wave.size(); ++index) {
            if (index + 1 < wave.size())
                prefetchSummaryOf(wave[index + 1]);
            bringAlive(wave[index]);
        }
    }
}

bool StateOfLinkReach::takeStepsOnLanes()
{
    unsigned numberOfLanes = m_flow.numberOfLanes();
    Vector<QueuedStep> wave = std::exchange(m_steps, { });
    if (!m_flow.takeStepsOfReachOnLanes(wave.size(), scopedLambda<void(unsigned)>([&](unsigned lane) {
        for (size_t index = lane; index < wave.size(); index += numberOfLanes)
            take(wave[index]);
    }))) {
        m_steps = WTF::move(wave);
        return false;
    }
    takeWhatLanesLeft();
    return true;
}

void StateOfLinkReach::takeWhatLanesLeft()
{
    for (unsigned lane = 0; lane < m_flow.numberOfLanes(); ++lane) {
        for (LinkFlow::LeftToReach left : m_flow.takeWhatLaneLeftToReach(lane))
            takeWhatLaneLeft(static_cast<Left>(left.what), left.index);
    }
}

ALWAYS_INLINE void StateOfLinkReach::takeWhatLaneLeft(Left what, uint32_t index)
{
    switch (what) {
    case Left::LayoutEscapes:
        escapeLayout(LayoutID(index));
        return;
    case Left::PrototypeEscapes:
        escapePrototype(LayoutID(index));
        return;
    case Left::FunctionEscapes:
        escapeFunction(FunctionID(index));
        return;
    case Left::ClassObjectOpens:
        escapeClassObject(LayoutID(index));
        return;
    case Left::FunctionIsReached:
        reach(FunctionID(index));
        return;
    case Left::FunctionIsNamed:
        noteNamedByReachedCode(FunctionID(index));
        return;
    case Left::ExportsEscape: {
        LinkFlow::UnknownByEscapeAlone unknownByEscapeAlone(true);
        escapeExportsOf(ModuleID(index));
        return;
    }
    case Left::VariableHasEscaped:
        m_flow.escapedVariables.set(index);
        return;
    case Left::FieldMayBeWritten:
        m_flow.fieldsThatUnseenCodeMayWrite.set(index);
        return;
    case Left::LayoutMayChange:
        m_flow.layoutsThatUnseenCodeMayChange.set(index);
        return;
    case Left::ParameterIsPassed:
        m_parametersThatUnseenCodePasses.append(FlowNodeID(index));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void StateOfLinkReach::prefetchSummaryOf(FunctionID id) const
{
    __builtin_prefetch(&m_program.at(id));
    const char* summary = reinterpret_cast<const char*>(&m_flow.summaries.at(id));
    for (size_t offset = 0; offset < sizeof(ThinSummary); offset += 64)
        __builtin_prefetch(summary + offset);
}

void StateOfLinkReach::prefetchRecordsOf(FunctionID id, const ThinSummary& summary) const
{
    auto prefetch = [](const auto& list) {
        const char* address = reinterpret_cast<const char*>(list.span().data());
        const char* end = address + list.span().size_bytes();
        for (; address < end; address += 64)
            __builtin_prefetch(address);
    };
    prefetch(summary.nodes);
    prefetch(summary.edges);
    prefetch(summary.calls);
    prefetch(summary.arguments);
    prefetch(summary.accesses);
    m_flow.prefetchNodesOf(id);
}

void StateOfLinkReach::take(QueuedStep queued)
{
    LinkFlow::UnknownByEscapeAlone unknownByEscapeAlone(queued.step == Step::OpenLayout || queued.step == Step::OpenPrototype || queued.step == Step::OpenFunction || queued.step == Step::OpenClassObject);
    switch (queued.step) {
    case Step::EnterProgram:
        enterProgram();
        return;
    case Step::BringAlive:
        bringAlive(FunctionID(queued.index));
        return;
    case Step::EvaluateModule:
        evaluateModule(ModuleID(queued.index));
        return;
    case Step::TieLostLayout:
        tieLostLayout(LayoutID(queued.index));
        return;
    case Step::TieLostFunction:
        tieLostFunction(FunctionID(queued.index));
        return;
    case Step::OpenLayout:
        openLayout(LayoutID(queued.index));
        return;
    case Step::OpenPrototype:
        openPrototype(LayoutID(queued.index));
        return;
    case Step::OpenFunction:
        openFunction(FunctionID(queued.index));
        return;
    case Step::OpenClassObject:
        openClassObject(LayoutID(queued.index));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void StateOfLinkReach::queueEntries()
{
    if (!std::exchange(m_hasQueuedEntries, true))
        m_stepsThatOneThreadTakes.append(QueuedStep { Step::EnterProgram, 0 });
}

void StateOfLinkReach::reach(FunctionID id)
{
    if (m_flow.reached.get(id.index()))
        return;
    if (m_flow.lanesTakeStepsOfReach()) {
        leave(Left::FunctionIsReached, id.index());
        return;
    }
    if (waitsUntilReachedCodeNamesIt(id))
        return;
    m_flow.reached.set(id.index());
    m_functionsToBringAlive.append(id);
}

bool StateOfLinkReach::waitsUntilReachedCodeNamesIt(FunctionID id)
{
    if (!m_functionsThatSomeCodeNames.quickGet(id.index()) || m_functionsThatReachedCodeNames.quickGet(id.index()))
        return false;
    if (m_isCodeBehindBuiltin.get(id.index()) || m_moduleWithBody[id.index()] || m_functionsCalledByHost.get(id.index()))
        return false;
    m_functionsThatWaitToBeNamed.quickSet(id.index());
    return true;
}

void StateOfLinkReach::noteFunctionsNamedBy(FunctionID namer)
{
    bool leavesItToCaller = m_flow.severalThreadsTell();
    auto note = [&](FunctionID named) {
        if (named == namer || m_functionsThatReachedCodeNames.quickGet(named.index()))
            return;
        if (leavesItToCaller)
            leave(Left::FunctionIsNamed, named.index());
        else
            noteNamedByReachedCode(named);
    };
    for (FunctionID named : m_flow.summaries.at(namer).functionsNamed)
        note(named);
    if (!m_moduleWithBody[namer.index()])
        return;
    for (auto [body, declared] : std::ranges::equal_range(m_functionsDeclaredBesideBodies, namer, { }, &std::pair<FunctionID, FunctionID>::first))
        note(declared);
}

void StateOfLinkReach::noteNamedByReachedCode(FunctionID named)
{
    m_functionsThatReachedCodeNames.quickSet(named.index());
    if (m_functionsThatWaitToBeNamed.quickClear(named.index()))
        reach(named);
}

void StateOfLinkReach::loseLayout(LayoutID id)
{
    if (!m_flow.lostLayouts.set(id.index())) {
        m_flow.noteLockableInstances(id);
        m_flow.noteLayoutToTie(id);
        m_steps.append(QueuedStep { Step::TieLostLayout, id.index() });
    }
}

void StateOfLinkReach::loseFamily(FamilyID id)
{
    if (m_lostFamilies.set(id.index()))
        return;
    m_program.at(m_flow.families.layoutsOf(id)).forEach([&](LayoutID member) {
        loseLayout(member);
    });
}

void StateOfLinkReach::loseFunction(FunctionID id)
{
    if (!m_flow.lostFunctions.set(id.index()))
        m_steps.append(QueuedStep { Step::TieLostFunction, id.index() });
}

void StateOfLinkReach::escapeLayout(LayoutID id)
{
    if (m_flow.lanesTakeStepsOfReach()) {
        if (!m_flow.escapedLayouts.get(id.index()))
            leave(Left::LayoutEscapes, id.index());
        return;
    }
    if (m_flow.escapedLayouts.set(id.index()))
        return;
    m_flow.noteLockableInstances(id);
    m_steps.append(QueuedStep { Step::OpenLayout, id.index() });
    if (m_program.at(id).kind == LayoutKind::ModuleNamespace) {
        m_flow.arrive(m_flow.whereInstancesLoseTheirConstructor(), LinkType::anything());
        m_flow.note(LinkFlow::Note::InstancesLoseTheirConstructor, 0);
    }
}

void StateOfLinkReach::escapePrototype(LayoutID id)
{
    if (m_flow.lanesTakeStepsOfReach()) {
        if (!m_escapedPrototypes.get(id.index()))
            leave(Left::PrototypeEscapes, id.index());
        return;
    }
    if (!m_escapedPrototypes.set(id.index()))
        m_steps.append(QueuedStep { Step::OpenPrototype, id.index() });
}

void StateOfLinkReach::escapeClassObject(LayoutID id)
{
    if (m_flow.lanesTakeStepsOfReach()) {
        if (!m_openClassObjects.get(id.index()))
            leave(Left::ClassObjectOpens, id.index());
        return;
    }
    if (!m_openClassObjects.set(id.index()))
        m_steps.append(QueuedStep { Step::OpenClassObject, id.index() });
}

void StateOfLinkReach::escapeFamily(FamilyID id)
{
    if (m_escapedFamilies.set(id.index()))
        return;
    m_flow.noteSomethingBefell(id);
    m_program.at(m_flow.families.layoutsOf(id)).forEach([&](LayoutID member) {
        if (m_flow.mayBeKnownByFamily(member))
            escapeLayout(member);
    });
}

void StateOfLinkReach::escapePrototypesOf(FamilyID id)
{
    if (m_familiesWithEscapedPrototypes.set(id.index()))
        return;
    m_program.at(m_flow.families.layoutsOf(id)).forEach([&](LayoutID member) {
        escapePrototype(member);
    });
}

void StateOfLinkReach::escapeConstructorsOf(FamilyID id)
{
    if (m_familiesWithEscapedConstructors.set(id.index()))
        return;
    m_program.at(m_flow.families.layoutsOf(id)).forEach([&](LayoutID member) {
        if (FunctionID constructor = m_program.at(member).constructor)
            escapeFunction(constructor);
    });
}

void StateOfLinkReach::escapeFunction(FunctionID id)
{
    if (m_flow.lanesTakeStepsOfReach()) {
        if (!m_flow.escapedFunctions.get(id.index()))
            leave(Left::FunctionEscapes, id.index());
        return;
    }
    if (!m_flow.escapedFunctions.set(id.index()))
        m_steps.append(QueuedStep { Step::OpenFunction, id.index() });
}

void StateOfLinkReach::escapeLostFunctions()
{
    if (std::exchange(m_lostFunctionsHaveEscaped, true))
        return;
    for (size_t index : m_flow.lostFunctions)
        escapeFunction(FunctionID(index));
}

void StateOfLinkReach::escapeAllOf(const LinkType& type)
{
    switch (type.objects) {
    case LinkType::Objects::None:
        break;
    case LinkType::Objects::Layout:
        escapeLayout(type.layout);
        break;
    case LinkType::Objects::Set:
        for (LayoutID member : m_flow.layoutsOfSet(type))
            escapeLayout(member);
        break;
    case LinkType::Objects::Prototype:
        escapePrototype(type.layout);
        break;
    case LinkType::Objects::Prototypes:
        escapePrototypesOf(type.family);
        break;
    case LinkType::Objects::Family:
        escapeFamily(type.family);
        break;
    case LinkType::Objects::Lost:
        m_flow.letLostFamilyEscape(type.layout);
        break;
    }

    switch (type.functions) {
    case LinkType::Functions::None:
        break;
    case LinkType::Functions::Closure:
        escapeFunction(type.function);
        break;
    case LinkType::Functions::Constructors:
        escapeConstructorsOf(type.constructorsOf);
        break;
    case LinkType::Functions::Set:
        for (FunctionID member : m_flow.functionsOfSet(type))
            escapeFunction(member);
        break;
    case LinkType::Functions::Lost:
        if (type.function)
            m_flow.letFamilyOfLostFunctionsEscape(type.function);
        else
            escapeLostFunctions();
        break;
    }

    if (type.classes != LinkType::Classes::None) {
        LinkType instances;
        instances.classes = type.classes;
        instances.builtinClass = type.builtinClass;
        instances.site = type.site;
        m_flow.arrive(m_flow.unseen(), instances);
    }
}

void StateOfLinkReach::enterProgram()
{
    fillVariables();
    fillFields();

    forEachLayoutMadeAtNoNodeForLinkReach(m_program, [&](LayoutID layout) {
        m_flow.noteAllocated(layout);
        if (m_program.at(layout).kind != LayoutKind::ModuleNamespace)
            escapeLayout(layout);
    });

    for (uint32_t index = 0; index < m_program.functions.size(); ++index) {
        if (!m_program.functions[index].hasBody())
            escapeFunction(FunctionID(index));
    }

    BitVector isAskedFor = modulesThatHostAsksFor(m_program);
    BitVector isRequested = std::exchange(m_modulesThatSomeCodeNames, { });
    for (const Module& module : m_program.modules) {
        for (const ModuleRequest& request : module.requests)
            isRequested.quickSet(request.target.index());
    }
    struct EntryStartedByHost {
        uint32_t entry;
        ModuleID file;
        FunctionID body;
        SummaryNode hostArgument;
        bool passesLiteral;
        bool isReachedInItsFile { false };
    };
    Vector<EntryStartedByHost, 1> started;
    for (uint32_t entry = 0; entry < m_program.entries.size(); ++entry) {
        ModuleID file = m_program.entries[entry].module;
        FunctionID body = m_program.at(file).body;
        if (!body)
            continue;
        const auto& summary = m_flow.summaries.at(body);
        auto made = std::ranges::find_if(summary.calls, [](const SummaryCall& call) {
            return call.isMadeByHost;
        });
        if (made != summary.calls.end())
            started.append({ entry, file, body, summary.nodes[summary.arguments[made->firstArgument]], made->passesLiteral });
    }
    if (!started.isEmpty()) {
        static constexpr uint32_t isNotStarted = UINT32_MAX;
        BitVector modulesNamed(m_program.modules.size());
        BitVector functionsNamed(m_program.functions.size());
        Vector<uint32_t> startedOfModule;
        startedOfModule.fill(isNotStarted, m_program.modules.size());
        for (uint32_t place = started.size(); place--;)
            startedOfModule[started[place].file.index()] = place;
        for (uint32_t index = 0; index < m_program.functions.size(); ++index) {
            const auto& summary = m_flow.summaries.at(FunctionID(index));
            for (ModuleID named : summary.otherModulesNamed)
                modulesNamed.quickSet(named.index());
            for (FunctionID named : summary.functionsNamed)
                functionsNamed.quickSet(named.index());
            ModuleID file = m_program.at(FunctionID(index)).module;
            uint32_t place = file ? startedOfModule[file.index()] : isNotStarted;
            if (place == isNotStarted)
                continue;
            started[place].isReachedInItsFile |= summary.has(SummaryTrait::HasDirectEval);
            if (FunctionID(index) != started[place].body)
                started[place].isReachedInItsFile |= std::ranges::contains(summary.nodes, started[place].hostArgument);
        }
        for (const EntryStartedByHost& each : started) {
            if (modulesNamed.quickGet(each.file.index()) || functionsNamed.quickGet(each.body.index()) || (each.passesLiteral && started[startedOfModule[each.file.index()]].isReachedInItsFile))
                continue;
            m_flow.modulesWhoseExportHostTakesByCallInSight.set(each.file.index());
            m_builtinsInReach.noteHostStartsByCallInSight(each.entry);
        }
    }
    for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
        const Module& module = m_program.modules[index];
        bool isOfProgram = isProgramModuleOf(module.kind) && !module.flags.contains(ModuleFlags::IsLibrary);
        bool isExposedByHost = module.flags.containsAny({ ModuleFlags::IsExposedByTestRunner, ModuleFlags::IsExposedByRequireCache });
        if (!isExposedByHost && !isAskedFor.get(index) && (!isOfProgram || isRequested.get(index)))
            continue;
        evaluate(ModuleID(index));
        if (!isExposedByHost && isOfProgram && hostReadsNamesAloneOff(ModuleID(index)))
            continue;
        escapeExportsOf(ModuleID(index));
    }
    for (uint32_t index = 0; index < m_program.entries.size(); ++index) {
        const Entry& entry = m_program.entries[index];
        if (hostReadsNamesAloneOff(entry.module))
            showExportsThatHostReadsOff(entry, m_entriesThatHandHostNothing.contains(index) ? HostWith::Looks : HostWith::Takes);
    }
}

bool StateOfLinkReach::hostReadsNamesAloneOff(ModuleID id) const
{
    static constexpr OptionSet<ModuleFlags> flagsOfModulesThatHostAsksForWhole {
        ModuleFlags::IsFoundByName, ModuleFlags::MayBeMocked, ModuleFlags::MayBeEvicted, ModuleFlags::IsImportedAsNamespace, ModuleFlags::LoaderReturnsNamespace,
        ModuleFlags::IsListedInCache, ModuleFlags::IsExposedByRequireCache, ModuleFlags::IsExposedByTestRunner, ModuleFlags::IsChangedOnDiskByRun,
    };
    const Module& module = m_program.at(id);
    if (module.kind == ModuleKind::CommonJs)
        return m_modulesWhoseValueStaysInSight.quickGet(id.index()) && std::ranges::contains(m_program.entries, id, &Entry::module);
    if (module.kind != ModuleKind::SourceText || module.exportRule != SyntheticExportRule::None || module.flags.containsAny(flagsOfModulesThatHostAsksForWhole))
        return false;
    if (m_program.admitsInspector || !refusedCodeOf(m_program).codeOutsideImage)
        return false;
    bool hostLooksThroughStars = module.body && !m_flow.summaries.at(module.body).namesThatHostLooksFor.isEmpty();
    return (hostLooksThroughStars || std::ranges::none_of(module.requests, &ModuleRequest::isDynamicStarExport)) && std::ranges::contains(m_program.entries, id, &Entry::module);
}

void StateOfLinkReach::showExportsThatHostReadsOff(const Entry& entry, HostWith host)
{
    const Module& module = m_program.at(entry.module);
    for (const HostRead& read : entry.hostReads) {
        for (uint32_t index = 0; index < module.exports.size(); ++index) {
            if (module.exports[index].name != read.exportName)
                continue;
            VariableID shown = m_program.variableBehindExport(entry.module, index);
            bool isHandedOverByCallInSight = shown && read.builtin && m_flow.modulesWhoseExportHostTakesByCallInSight.get(entry.module.index());
            if (isHandedOverByCallInSight || (shown && host == HostWith::Looks))
                m_flow.note(LinkFlow::Note::ExportIsShown, shown.index());
            else if (shown && read.exportName != m_program.stringOf(BuiltinNames::then))
                m_flow.showExport(entry.module, index);
            else
                escapeExportsOf(entry.module);
        }
    }
}

BitVector StateOfLinkReach::findFilesThatHaveRunToTheirEndWhereOthersNameThem() const
{
    static constexpr OptionSet<ModuleFlags> flagsOfModulesThatOthersMayFindUnfinished {
        ModuleFlags::IsFoundByName, ModuleFlags::MayBeMocked, ModuleFlags::MayBeEvicted, ModuleFlags::MayBeReevaluated, ModuleFlags::IsImportedAsNamespace, ModuleFlags::LoaderReturnsNamespace,
        ModuleFlags::IsExposedByRequireCache, ModuleFlags::IsExposedByTestRunner, ModuleFlags::IsImportableDuringEvaluation, ModuleFlags::HasTopLevelAwait,
    };
    uint32_t numberOfModules = static_cast<uint32_t>(m_program.modules.size());
    BitVector found(numberOfModules);
    for (uint32_t index = 0; index < numberOfModules; ++index) {
        const Module& module = m_program.modules[index];
        bool isFoundInCache = module.flags.contains(ModuleFlags::IsListedInCache) && !module.requests.isEmpty();
        if (module.kind == ModuleKind::CommonJs && module.body && module.flags.contains(ModuleFlags::CachesEvaluationError) && !isFoundInCache && !module.flags.containsAny(flagsOfModulesThatOthersMayFindUnfinished))
            found.quickSet(index);
    }
    for (const Module& module : m_program.modules) {
        for (const ModuleRequest& request : module.requests) {
            if (request.kind != ModuleRequestKind::Require || !request.completes)
                found.quickClear(request.target.index());
        }
    }

    return found;
}

Vector<ModuleID> StateOfLinkReach::findModulesOfBundlesByFunction() const
{
    struct Text {
        uint32_t file;
        uint32_t start;
        uint32_t end;
        ModuleID module;
    };
    Vector<Text> texts;
    for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
        FunctionID body = m_program.modules[index].body;
        if (!body || !isModuleOfBundle(m_program, ModuleID(index)))
            continue;
        const Function& function = m_program.at(body);
        if (function.sourceRange.start < function.sourceRange.end)
            texts.append(Text { function.module.index(), function.sourceRange.start, function.sourceRange.end, ModuleID(index) });
    }
    if (texts.isEmpty())
        return { };
    auto placeOf = [](const Text& text) { return std::pair { text.file, text.start }; };
    std::ranges::sort(texts, { }, placeOf);
    Vector<ModuleID> modules(m_program.functions.size());
    for (uint32_t index = 0; index < m_program.functions.size(); ++index) {
        const Function& function = m_program.functions[index];
        uint32_t start = function.sourceRange.start < function.sourceRange.end ? function.sourceRange.start : function.position;
        if (!function.module)
            continue;
        auto next = std::ranges::upper_bound(texts, std::pair { function.module.index(), start }, { }, placeOf);
        if (next == texts.begin())
            continue;
        const Text& text = *(next - 1);
        if (text.file == function.module.index() && start < text.end)
            modules[index] = text.module;
    }
    return modules;
}

void StateOfLinkReach::findVariablesStoredFirst(LinkFlow& flow, std::span<const ModuleID> modulesOfBundles, BitVector& found) const
{
    bool isOfBundles = !modulesOfBundles.empty();
    auto moduleOf = [&](FunctionID function) { return isOfBundles ? modulesOfBundles[function.index()] : m_program.at(function).module; };
    Vector<LinkedVariableStoredFirst>& firstReaders = flow.firstReadersOfVariablesStoredFirst;
    Vector<SummaryClassInVariable>& classesInVariables = flow.classesInVariables;
    Vector<SummaryUnitInVariable>& unitsInVariables = flow.unitsInVariables;
    BitVector isLeftOut(m_program.modules.size());
    BitVector hasRunToItsEnd = isOfBundles ? BitVector() : findFilesThatHaveRunToTheirEndWhereOthersNameThem();
    for (uint32_t index = 0; index < m_program.functions.size(); ++index) {
        if (isOfBundles) {
            if (modulesOfBundles[index] && m_flow.summaries.at(FunctionID(index)).has(SummaryTrait::HasDirectEval))
                isLeftOut.quickSet(modulesOfBundles[index].index());
            continue;
        }
        for (ModuleID module : m_flow.summaries.at(FunctionID(index)).otherModulesNamed) {
            if (!hasRunToItsEnd.quickGet(module.index()))
                isLeftOut.quickSet(module.index());
        }
    }
    BitVector filesOfBundles(m_program.modules.size());
    for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
        bool isOfBundle = isModuleOfBundle(m_program, ModuleID(index));
        if (isOfBundle != isOfBundles)
            isLeftOut.quickSet(index);
        if (FunctionID body = isOfBundle ? m_program.modules[index].body : FunctionID())
            filesOfBundles.quickSet(m_program.at(body).module.index());
    }
    Vector<FunctionID> bodies(m_program.modules.size());
    for (uint32_t index = 0; index < m_program.modules.size(); ++index)
        bodies[index] = m_program.modules[index].body;
    for (uint32_t index = 0; !isOfBundles && m_flow.listsStoresOfWrappedBodies && index < m_program.functions.size(); ++index) {
        if (isBodyOfWrappedModule(m_program, FunctionID(index)))
            bodies[m_program.functions[index].module.index()] = FunctionID(index);
    }
    auto summaryOfBody = [&](ModuleID module) -> const ThinSummary& { return m_flow.summaries.at(bodies[module.index()]); };
    BitVector bodiesSeen(m_program.functions.size());
    BitVector bodiesOfSeveralModules(m_program.functions.size());
    for (const Module& module : m_program.modules) {
        if (module.body && bodiesSeen.quickSet(module.body.index()))
            bodiesOfSeveralModules.quickSet(module.body.index());
    }

    struct Start {
        uint32_t position;
        FunctionID function;
    };
    Vector<Start> starts;
    Vector<SummaryMethodOfClass> methodsOfClasses;
    Vector<ModuleID> modulesWithStoresOnPrototypes;
    for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
        FunctionID body = m_program.modules[index].body;
        if (!body || bodiesOfSeveralModules.quickGet(body.index()))
            isLeftOut.quickSet(index);
        if (isLeftOut.quickGet(index))
            continue;
        const ThinSummary& summary = summaryOfBody(ModuleID(index));
        for (SummaryPlaceInBody use : summary.firstUsesOfFunctions)
            starts.append(Start { use.position, FunctionID(use.number) });
        methodsOfClasses.append(summary.methodsOfClasses.span());
        classesInVariables.append(summary.classesInVariables.span());
        unitsInVariables.append(summary.unitsInVariables.span());
        if (!summary.storesOnPrototypes.isEmpty())
            modulesWithStoresOnPrototypes.append(ModuleID(index));
    }
    std::ranges::sort(unitsInVariables);
    std::ranges::sort(classesInVariables);
    auto forEachFunctionHeld = [&](VariableID variable, NOESCAPE const Invocable<void(FunctionID)> auto& functor) {
        if (FunctionID declared = m_program.at(variable).function)
            functor(declared);
        for (const SummaryClassInVariable& held : std::ranges::equal_range(classesInVariables, variable, { }, &SummaryClassInVariable::variable))
            functor(held.constructor);
    };
    auto forEachOwner = [&](const SummaryStoreOnPrototype& store, NOESCAPE const Invocable<void(FunctionID)> auto& functor) {
        if (store.function)
            functor(store.function);
        else
            forEachFunctionHeld(store.variable, functor);
    };
    for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
        if (isLeftOut.quickGet(index))
            continue;
        for (SummaryPlaceInBody use : summaryOfBody(ModuleID(index)).firstUsesOfVariables)
            forEachFunctionHeld(VariableID(use.number), [&](FunctionID function) { starts.append(Start { use.position, function }); });
    }

    constexpr uint32_t nobody = UINT32_MAX;
    constexpr uint32_t others = UINT32_MAX - 1;
    Vector<uint32_t> namerOfVariable;
    if (isOfBundles) {
        namerOfVariable.fill(nobody, m_program.variables.size());
        for (uint32_t index = 0; index < m_program.functions.size(); ++index) {
            ModuleID module = modulesOfBundles[index];
            ModuleID file = m_program.functions[index].module;
            const ThinSummary& summary = m_flow.summaries.at(FunctionID(index));
            bool mayName = (file && filesOfBundles.quickGet(file.index())) || std::ranges::any_of(summary.otherModulesNamed, [&](ModuleID named) { return filesOfBundles.quickGet(named.index()); });
            if (!mayName)
                continue;
            auto noteUse = [&](FunctionID used) {
                if (modulesOfBundles[used.index()] && modulesOfBundles[used.index()] != module)
                    starts.append(Start { 0, used });
            };
            for (const SummaryNode& node : summary.nodes) {
                if (node.kind != SummaryNode::Kind::Variable)
                    continue;
                uint32_t& namer = namerOfVariable[node.first];
                if (!module || (namer != nobody && namer != module.index()))
                    namer = others;
                else
                    namer = module.index();
                forEachFunctionHeld(VariableID(node.first), noteUse);
            }
            for (const SummaryCall& call : summary.calls) {
                if (call.callee == SummaryCall::Callee::Binding && call.function)
                    noteUse(call.function);
            }
        }
    }
    auto isNamedByOthers = [&](ModuleID module, uint32_t variable) {
        return isOfBundles && namerOfVariable[variable] != nobody && namerOfVariable[variable] != module.index();
    };

    Vector<uint32_t> placeOfFirstReader;
    Vector<uint32_t> placeOfFirstRun;
    Vector<FunctionID> functionWhoseUseCounts;
    auto findFirstReaders = [&](std::span<const Start> startsBesideStores, std::span<const SummaryMethodOfClass> methodsBesideStores, NOESCAPE const Invocable<bool(ModuleID, const SummaryStoreOnPrototype&)> auto& makesMethod) {
        Vector<Start> starts(startsBesideStores);
        Vector<SummaryMethodOfClass> methods(methodsBesideStores);
        for (ModuleID module : modulesWithStoresOnPrototypes) {
            for (const SummaryStoreOnPrototype& store : summaryOfBody(module).storesOnPrototypes) {
                bool isMethod = makesMethod(module, store);
                forEachOwner(store, [&](FunctionID holder) {
                    if (!isMethod)
                        starts.append(Start { store.position, holder });
                    else if (store.method)
                        methods.append(SummaryMethodOfClass { holder, store.method });
                });
                if (!isMethod && store.method)
                    starts.append(Start { store.position, store.method });
            }
        }
        std::ranges::sort(methods);
        std::ranges::sort(starts, { }, &Start::position);

        BitVector couldRun(m_program.functions.size());
        placeOfFirstReader.fill(UINT32_MAX, m_program.variables.size());
        placeOfFirstRun.fill(UINT32_MAX, m_program.functions.size());
        functionWhoseUseCounts.fill(FunctionID(), m_program.variables.size());
        Vector<FunctionID> toVisit;
        for (const Start& start : starts) {
            ModuleID module = moduleOf(start.function);
            if (!module)
                continue;
            toVisit.append(start.function);
            while (!toVisit.isEmpty()) {
                FunctionID function = toVisit.takeLast();
                if (moduleOf(function) != module || m_moduleWithBody[function.index()] == module || bodies[module.index()] == function || couldRun.quickSet(function.index()))
                    continue;
                placeOfFirstRun[function.index()] = start.position;
                for (const SummaryMethodOfClass& defined : std::ranges::equal_range(methods, function, { }, &SummaryMethodOfClass::constructor))
                    toVisit.append(defined.method);
                const ThinSummary& summary = m_flow.summaries.at(function);
                for (const SummaryNode& node : summary.nodes) {
                    if (node.kind == SummaryNode::Kind::Closure)
                        toVisit.append(FunctionID(node.first));
                    if (node.kind != SummaryNode::Kind::Variable)
                        continue;
                    if (start.position < placeOfFirstReader[node.first]) {
                        placeOfFirstReader[node.first] = start.position;
                        functionWhoseUseCounts[node.first] = start.function;
                    }
                    forEachFunctionHeld(VariableID(node.first), [&](FunctionID held) { toVisit.append(held); });
                }
                for (const SummaryCall& call : summary.calls) {
                    if (call.callee == SummaryCall::Callee::Binding && call.function)
                        toVisit.append(call.function);
                }
            }
        }
    };
    auto isStoredBeforeReaders = [&](ModuleID module, SummaryPlaceInBody store) {
        return store.position <= placeOfFirstReader[store.number] && !isNamedByOthers(module, store.number) && !m_program.variables[store.number].flags.contains(VariableFlags::HoldsInstance);
    };
    auto isMethodOfPlainFunction = [&](ModuleID module, const SummaryStoreOnPrototype& store) {
        unsigned numberOfHolders = 0;
        bool isPlain = true;
        forEachOwner(store, [&](FunctionID holder) {
            const Function& function = m_program.at(holder);
            isPlain &= !function.isCoroutine() && function.kind == FunctionKind::Plain && (!isOfBundles || moduleOf(holder) == module);
            ++numberOfHolders;
        });
        return numberOfHolders == 1 && isPlain;
    };
    auto isUnderClosedName = [&](const SummaryStoreOnPrototype& store) {
        BuiltinID objectPrototype = m_program.builtins().canonicalID(Builtins::ObjectPrototype);
        if (m_program.nameClassOf(store.key) == NameClass::ObjectPrototypeMember)
            return !m_program.admitsReplacedBuiltins && isLockedLocation(m_program, objectPrototype, store.key);
        return !isOnObjectPrototype(m_program.nameClassOf(store.key)) && !findOwnMember(m_program, objectPrototype, store.key) && listsEveryMemberUnder(m_program, objectPrototype, store.key) && isClosedKey(m_program, objectPrototype, store.key);
    };
    auto isStoredBeforeFirstUse = [&](ModuleID module, const SummaryStoreOnPrototype& store) {
        bool isFirst = store.isOnEveryPath && store.method && isMethodOfPlainFunction(module, store);
        if (isFirst)
            forEachOwner(store, [&](FunctionID holder) { isFirst &= store.position <= placeOfFirstRun[holder.index()]; });
        return isFirst;
    };
    findFirstReaders(starts.span(), methodsOfClasses.span(), [](ModuleID, const SummaryStoreOnPrototype&) { return false; });
    if (!modulesWithStoresOnPrototypes.isEmpty()) {
        BitVector moduleGains(m_program.modules.size());
        auto forEachFirstStore = [&](NOESCAPE const Invocable<void(ModuleID, SummaryPlaceInBody)> auto& functor) {
            for (ModuleID module : modulesWithStoresOnPrototypes) {
                for (SummaryPlaceInBody store : summaryOfBody(module).firstStoresOfBody)
                    functor(module, store);
            }
        };
        BitVector wasStoredBeforeReaders(m_program.variables.size());
        forEachFirstStore([&](ModuleID module, SummaryPlaceInBody store) { if (isStoredBeforeReaders(module, store)) wasStoredBeforeReaders.quickSet(store.number); });
        findFirstReaders(starts.span(), methodsOfClasses.span(), isMethodOfPlainFunction);
        forEachFirstStore([&](ModuleID module, SummaryPlaceInBody store) { if (isStoredBeforeReaders(module, store) && !wasStoredBeforeReaders.quickGet(store.number)) moduleGains.quickSet(module.index()); });
        Vector<LinkedMethodOnPrototype> methodsOnPrototypes;
        for (ModuleID module : modulesWithStoresOnPrototypes) {
            FunctionID body = bodies[module.index()];
            auto isFirst = [&](const SummaryStoreOnPrototype& store) { return isStoredBeforeFirstUse(module, store); };
            if (!moduleGains.quickGet(module.index()) && std::ranges::none_of(m_flow.summaries.at(body).storesOnPrototypes.span(), isFirst))
                continue;
            for (const SummaryStoreOnPrototype& store : m_flow.summaries.at(body).storesOnPrototypes) {
                if (isMethodOfPlainFunction(module, store) && isUnderClosedName(store))
                    methodsOnPrototypes.append(LinkedMethodOnPrototype { body, store.site });
            }
        }
        std::ranges::sort(methodsOnPrototypes);
        auto isMethod = [&](ModuleID module, const SummaryStoreOnPrototype& store) {
            return std::ranges::binary_search(methodsOnPrototypes, LinkedMethodOnPrototype { bodies[module.index()], store.site });
        };
        findFirstReaders(starts.span(), methodsOfClasses.span(), isMethod);
        for (ModuleID module : modulesWithStoresOnPrototypes) {
            for (const SummaryStoreOnPrototype& store : summaryOfBody(module).storesOnPrototypes) {
                if (isMethod(module, store) && isStoredBeforeFirstUse(module, store))
                    forEachOwner(store, [&](FunctionID holder) { flow.methodsStoredFirst.append(LinkedMethodStoredFirst { holder, store.key, store.method }); });
            }
        }
        std::ranges::sort(flow.methodsStoredFirst);
        flow.methodsOnPrototypes.appendVector(methodsOnPrototypes);
        std::ranges::sort(flow.methodsOnPrototypes);
    }

    for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
        if (isLeftOut.quickGet(index))
            continue;
        for (SummaryPlaceInBody store : summaryOfBody(ModuleID(index)).firstStoresOfBody) {
            if (!isStoredBeforeReaders(ModuleID(index), store))
                continue;
            found.quickSet(store.number);
            if (functionWhoseUseCounts[store.number])
                firstReaders.append(LinkedVariableStoredFirst { bodies[index], functionWhoseUseCounts[store.number], VariableID(store.number) });
        }
    }
    std::ranges::sort(firstReaders);
}

void StateOfLinkReach::fillVariables()
{
    LinkType absent = m_flow.fromTypeID(TypeTable::absent);
    LinkType undefined = m_flow.fromTypeID(TypeTable::undefined);
    m_flow.firstReadersOfVariablesStoredFirst.shrink(0);
    m_flow.classesInVariables.shrink(0);
    m_flow.unitsInVariables.shrink(0);
    m_flow.methodsOnPrototypes.shrink(0);
    m_flow.methodsStoredFirst.shrink(0);
    m_flow.variablesStoredFirst = BitVector(m_program.variables.size());
    findVariablesStoredFirst(m_flow, { }, m_flow.variablesStoredFirst);
    if (Vector<ModuleID> modules = findModulesOfBundlesByFunction(); !modules.isEmpty())
        findVariablesStoredFirst(m_flow, modules.span(), m_flow.variablesStoredFirst);
    const BitVector& isStoredFirst = m_flow.variablesStoredFirst;
    for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
        const Module& module = m_program.modules[index];
        if (module.exportsVariable && !m_modulesWhoseValueStaysInSight.quickGet(index))
            escapeVariable(module.exportsVariable);
        bool isTrackedFacade = module.kind == ModuleKind::SourceText && m_modulesWhoseValueStaysInSight.quickGet(index);
        bool isFollowedLibraryFile = loaderFillsVariablesOffValueOf(module) && m_modulesWhoseValueStaysInSight.quickGet(index);
        for (const SyntheticExport& synthetic : module.syntheticExports) {
            if (synthetic.variable && !isTrackedFacade && !isFollowedLibraryFile)
                escapeVariable(synthetic.variable);
        }
    }
    for (uint32_t index = 0; index < m_program.variables.size(); ++index) {
        const Variable& variable = m_program.variables[index];
        FlowNodeID node = m_flow.variable(VariableID(index));
        arriveWhatNoNodeBrings(node, variable.declared);
        if (variable.owner) {
            m_flow.arrive(node, absent);
            continue;
        }
        switch (variable.initialization) {
        case VariableInitialization::Uninitialized:
            if (!isStoredFirst.quickGet(index))
                m_flow.arrive(node, absent);
            break;
        case VariableInitialization::Undefined:
            if (!isStoredFirst.quickGet(index))
                m_flow.arrive(node, undefined);
            break;
        case VariableInitialization::Function:
            m_flow.arrive(node, typeOfClosuresForLinkReach(variable.function));
            break;
        }
        const Module& module = m_program.at(variable.module);
        if (!module.body && module.flags.contains(ModuleFlags::IsLibrary)) {
            m_variablesOfLibraryFiles.append({ variable.module, VariableID(index) });
            continue;
        }
        if (variable.flags.containsAny({ VariableFlags::IsReplaceable, VariableFlags::HoldsMock, VariableFlags::OriginalMayRun }) || !module.body)
            escapeVariable(VariableID(index), module.body && !variable.flags.containsAny({ VariableFlags::HoldsMock, VariableFlags::OriginalMayRun }));
    }
    std::ranges::sort(m_variablesOfLibraryFiles);
}

void StateOfLinkReach::letGoWhatDirectEvalsShowIn(FunctionID id)
{
    const BodyIndex* index = bodiesLookedAt(m_flow.looks).find(id);
    RELEASE_ASSERT(index);
    auto letGo = [&](const EvalScope& scope) {
        for (const EvalBinding& binding : scope.bindings) {
            if (binding.variable)
                escapeVariable(binding.variable, true);
            if (binding.layout && binding.field)
                escapeField({ binding.layout, *binding.field });
        }
    };
    for (uint32_t block = 0; block < index->numberOfBlocks(); ++block) {
        for (BodyValue value : index->block(BlockID(block)).values()) {
            if (value.opcode() != Opcode::DirectEval)
                continue;
            const EvalScope& scope = m_program.at(value.as<Opcode::DirectEval>().evalScope());
            letGo(scope);
            if (scope.outer && !m_scopesAroundDirectEvalsLetGo.set(scope.outer.index()))
                letGo(m_program.at(scope.outer));
        }
    }
}

void StateOfLinkReach::fillFields()
{
    for (uint32_t index = 0; index < m_numberOfLayouts; ++index) {
        const Layout& layout = m_program.layouts[index];
        uint32_t numberOfFields = baseWhoseFieldListIsCopiedWhole(m_program, LayoutID(index)) ? layout.ownFieldCount : static_cast<uint32_t>(layout.fields.size());
        for (uint32_t field = 0; field < numberOfFields; ++field)
            arriveWhatNoNodeBrings(m_flow.field({ LayoutID(index), field }), layout.fields[field].declared);
    }
    for (FieldReference exported : m_program.exportedFields()) {
        if (fieldAt(m_program, exported).flags.contains(FieldFlags::IsReplaceable))
            escapeField(exported);
    }
    if (m_program.moduleInstances.isEmpty())
        return;
    Vector<VariableID> instanceVariables(m_program.modules.size());
    for (uint32_t index = 0; index < m_program.variables.size(); ++index) {
        if (m_program.variables[index].flags.contains(VariableFlags::HoldsInstance))
            instanceVariables[m_program.variables[index].module.index()] = VariableID(index);
    }
    for (const Program::InstanceField& filled : m_program.instanceFields()) {
        if (filled.linkTarget)
            m_flow.addEdge(m_flow.variable(instanceVariables[filled.linkTarget.index()]), m_flow.field(filled.field));
        else
            escapeField(filled.field);
    }
}

void StateOfLinkReach::evaluate(ModuleID id)
{
    if (!m_evaluatedModules.set(id.index()))
        m_stepsThatOneThreadTakes.append(QueuedStep { Step::EvaluateModule, id.index() });
}

void StateOfLinkReach::evaluateModule(ModuleID id)
{
    const Module& module = m_program.at(id);
    m_builtinsInReach.noteEvaluated(id);
    if (module.body)
        reach(module.body);
    else
        noteCodeOfModule(id);
}

void StateOfLinkReach::noteCodeOfModule(ModuleID id)
{
    if (!id || m_modulesWithReachedCode.at(id.index()))
        return;
    m_modulesWithReachedCode.set(id.index(), true);
    for (auto [module, variable] : std::ranges::equal_range(m_variablesOfLibraryFiles, id, { }, &std::pair<ModuleID, VariableID>::first)) {
        const Variable& declared = m_program.at(variable);
        if (declared.initialization != VariableInitialization::Function || !declared.flags.contains(VariableFlags::IsImmutable))
            escapeVariable(variable);
    }
    bool hasBody = !!m_program.at(id).body;
    for (const ModuleRequest& request : m_program.at(id).requests) {
        if (request.kind == ModuleRequestKind::Require && hasBody) {
            if (m_showsWaysBetweenModuleObjects && !isModuleOfBundle(m_program, request.target)) {
                escapeExportsOf(request.target);
                escapeValueOf(request.target);
            }
            continue;
        }
        evaluate(request.target);
        if (request.kind == ModuleRequestKind::Require)
            escapeExportsOf(request.target);
    }
}

bool StateOfLinkReach::globalObjectShowsModuleObjects() const
{
    if (m_builtinsInReach.unseenCodeCanRun() || m_builtinsInReach.loaderOfMachineCodeIsInUse())
        return true;
    const BuiltinTable& builtins = m_program.builtins();
    BuiltinID mainModule = builtins.canonicalID(Builtins::ProcessMainModule);
    return m_builtinsInReach.isInReach(mainModule) || m_builtinsInReach.isInReach(builtins.canonicalID(Builtins::ModuleRequireMain))
        || m_builtinsInReach.isOutOfSight(builtins.canonicalID(Builtins::GlobalThis)) || m_builtinsInReach.isOutOfSight(builtins.canonicalID(builtins.at(mainModule).holder));
}

bool StateOfLinkReach::showModuleObjectsOfGlobalObject()
{
    if (!m_globalObjectMayShowModuleObjects || m_showsWaysBetweenModuleObjects || !globalObjectShowsModuleObjects())
        return false;
    m_showsWaysBetweenModuleObjects = true;
    for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
        const Module& module = m_program.modules[index];
        if ((module.kind == ModuleKind::CommonJs || commonJsModuleCapturedBy(m_program, module)) && !isModuleOfBundle(m_program, ModuleID(index)))
            escapeValueOf(ModuleID(index));
        if (!module.body || !m_modulesWithReachedCode.at(index))
            continue;
        for (const ModuleRequest& request : module.requests) {
            if (request.kind != ModuleRequestKind::Require || isModuleOfBundle(m_program, request.target))
                continue;
            escapeExportsOf(request.target);
            escapeValueOf(request.target);
        }
    }
    return true;
}

void StateOfLinkReach::escapeExportsOf(ModuleID first)
{
    Vector<ModuleID, 8> modules;
    auto add = [&](ModuleID id) {
        if (id && !m_modulesWithEscapedExports.set(id.index()))
            modules.append(id);
    };
    add(first);
    while (!modules.isEmpty()) {
        ModuleID id = modules.takeLast();
        const Module& module = m_program.at(id);
        if (module.kind != ModuleKind::RuntimeSource || (module.runtimeModule && m_program.builtins().at(module.runtimeModule).flags.contains(RuntimeModuleFlag::IsPublic)))
            escapeValueOf(id);
        for (const ModuleExport& exported : module.exports) {
            if (exported.variable && !m_variablesReadByUnseenCode.set(exported.variable.index())) {
                m_flow.note(LinkFlow::Note::ExportIsShown, exported.variable.index());
                m_flow.addEdge(m_flow.variable(exported.variable), m_flow.unseen());
            }
            if (exported.kind == ModuleExportKind::Field && exported.field)
                m_flow.addEdge(m_flow.field({ exported.layout, *exported.field }), m_flow.unseen());
            if (exported.kind == ModuleExportKind::Namespace && exported.module)
                m_flow.noteLossOfObjectOfBuiltinModule(exported.module);
            if (exported.kind == ModuleExportKind::Indirect && exported.module && exported.importName)
                m_flow.noteLossOfExportOfBuiltinModule(exported.module, exported.importName);
            add(exported.module);
        }
        for (const ModuleRequest& request : module.requests) {
            if (!request.isDynamicStarExport)
                continue;
            m_flow.noteLossOfObjectOfBuiltinModule(request.target);
            add(request.target);
        }
    }
}

void StateOfLinkReach::findModulesWhoseValueStaysInSight()
{
    const BuiltinTable& builtins = m_program.builtins();
    m_modulesWhoseValueStaysInSight = BitVector(m_program.modules.size());
    bool libraryRunsCompiledAlone = refusedCodeOf(m_program).codeOutsideImage && std::ranges::none_of(m_program.modules, [](const Module& module) {
        return module.kind == ModuleKind::External && module.externalKind == ExternalKind::UncompiledCode;
    });
    m_flow.modulesWhoseValueStaysInSight = m_modulesWhoseValueStaysInSight;
    m_flow.modulesWhoseRecordIsHandedOutFrozen = BitVector(m_program.modules.size());

    static constexpr OptionSet<ModuleFlags> flagsOfModulesHandedOut {
        ModuleFlags::IsFoundByName, ModuleFlags::MayBeMocked, ModuleFlags::MayBeEvicted, ModuleFlags::MayBeReevaluated, ModuleFlags::IsImportedAsNamespace, ModuleFlags::LoaderReturnsNamespace,
        ModuleFlags::IsListedInCache, ModuleFlags::IsExposedByRequireCache, ModuleFlags::IsExposedByTestRunner, ModuleFlags::IsImportableDuringEvaluation, ModuleFlags::IsChangedOnDiskByRun,
    };
    BitVector isAskedFor = modulesThatHostAsksFor(m_program);
    for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
        const Module& module = m_program.modules[index];
        if (!refusedCodeOf(m_program).codeOutsideImage && module.kind != ModuleKind::RuntimeSource)
            continue;
        if (module.kind == ModuleKind::CommonJs) {
            bool isReadBySitesAlone = module.body && module.exportsVariable && module.requireResult == RequireResult::ModuleExports && module.syntheticExports.isEmpty();
            bool hostLooksForNamesAlone = isReadBySitesAlone && libraryRunsCompiledAlone && !m_program.admitsInspector && std::ranges::contains(m_program.entries, ModuleID(index), &Entry::module)
                && !m_flow.summaries.at(module.body).namesThatHostLooksFor.isEmpty();
            if (isReadBySitesAlone && (!m_showsWaysBetweenModuleObjects || isModuleOfBundle(m_program, ModuleID(index))) && !module.flags.containsAny(flagsOfModulesHandedOut - ModuleFlags::MayBeReevaluated) && (!isAskedFor.get(index) || hostLooksForNamesAlone))
                m_modulesWhoseValueStaysInSight.quickSet(index);
            continue;
        }
        if (ModuleID captured = commonJsModuleCapturedBy(m_program, module)) {
            bool isFilledByCaptureAlone = module.body && module.requests.size() == 1 && module.exports.isEmpty() && std::ranges::none_of(module.syntheticExports, &SyntheticExport::isReplaceable);
            bool isLoadedByLoader = module.flags.contains(ModuleFlags::IsLoadedByLoader) || m_program.at(captured).flags.contains(ModuleFlags::IsLoadedByLoader);
            if (isFilledByCaptureAlone && !isLoadedByLoader && !m_showsWaysBetweenModuleObjects && !module.flags.containsAny(flagsOfModulesHandedOut) && !isAskedFor.get(index))
                m_modulesWhoseValueStaysInSight.quickSet(index);
            continue;
        }
        if (module.kind != ModuleKind::RuntimeSource || !module.runtimeModule || module.flags.containsAny(flagsOfModulesHandedOut) || isAskedFor.get(index))
            continue;
        const RuntimeModuleData& row = builtins.at(module.runtimeModule);
        if (row.flags.contains(RuntimeModuleFlag::NativeCodeReadsEveryExport))
            continue;
        if (!libraryRunsCompiledAlone && row.flags.contains(RuntimeModuleFlag::IsPublic))
            continue;
        if (!libraryRunsCompiledAlone)
            m_flow.modulesWhoseRecordIsHandedOutFrozen.quickSet(index);
        if (row.flags.contains(RuntimeModuleFlag::IsPublic) && std::ranges::any_of(module.syntheticExports, &SyntheticExport::isReplaceable))
            continue;
        m_modulesWhoseValueStaysInSight.quickSet(index);
        if (module.body && !row.exportsReadByNativeCode.empty())
            m_modulesThatNativeCodeReadsNamesOff.append(ModuleID(index));
    }
    for (const Module& module : m_program.modules) {
        for (const ModuleRequest& request : module.requests) {
            bool bindsFacadeVariables = request.phase == ImportPhase::Evaluation && commonJsModuleCapturedBy(m_program, m_program.at(request.target));
            bool bindsVariablesOfLibraryFile = request.phase == ImportPhase::Evaluation && !request.isDynamicStarExport && loaderFillsVariablesOffValueOf(m_program.at(request.target)) && !importStatementListsInRequireCache(m_program, request.target);
            if (request.kind != ModuleRequestKind::Require && !bindsFacadeVariables && !bindsVariablesOfLibraryFile)
                m_modulesWhoseValueStaysInSight.quickClear(request.target.index());
        }
        for (const RequiredName& required : module.requiredNames) {
            if (!commonJsModuleCapturedBy(m_program, m_program.at(required.module)))
                m_modulesWhoseValueStaysInSight.quickClear(required.module.index());
        }
        for (const ModuleExport& exported : module.exports) {
            if (exported.module)
                m_modulesWhoseValueStaysInSight.quickClear(exported.module.index());
        }
        if (module.kind == ModuleKind::External && module.referrer)
            m_modulesWhoseValueStaysInSight.quickClear(module.referrer.index());
    }
    for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
        ModuleID captured = commonJsModuleCapturedBy(m_program, m_program.modules[index]);
        if (captured && (!m_modulesWhoseValueStaysInSight.quickGet(index) || !m_modulesWhoseValueStaysInSight.quickGet(captured.index()))) {
            m_modulesWhoseValueStaysInSight.quickClear(index);
            m_modulesWhoseValueStaysInSight.quickClear(captured.index());
        } else if (captured)
            m_initiallyTrackedFacades.append(ModuleID(index));
    }
    for (uint32_t index = 0; index < m_program.entries.size(); ++index) {
        ModuleID id = m_program.entries[index].module;
        FunctionID body = m_program.at(id).body;
        if (!body || m_flow.summaries.at(body).namesThatHostLooksFor.isEmpty() || !hostReadsNamesAloneOff(id))
            continue;
        m_entriesThatHandHostNothing.append(index);
        m_builtinsInReach.noteEntryHandsHostNothing(index);
    }
    m_flow.modulesWhoseValueStaysInSight = m_modulesWhoseValueStaysInSight;
}

void StateOfLinkReach::tellWhatHostFindsAtEntries()
{
    m_entriesThatHandHostNothing.removeAllMatching([&](uint32_t index) {
        ModuleID id = m_program.entries[index].module;
        bool mayFindSomething = false;
        auto lookAtExportsOf = [&](ModuleID file) {
            const LinkType& exports = m_flow.typeAt(m_flow.variable(m_program.at(file).exportsVariable));
            mayFindSomething |= !m_modulesWhoseValueStaysInSight.quickGet(file.index()) || exports.functions == LinkType::Functions::Lost || exports.classes != LinkType::Classes::None
                || exports.builtin || exports.hasUnknownObjects || exports.holdsLostBuiltins || mayHaveAccessor(exports);
        };
        if (m_program.at(id).kind == ModuleKind::CommonJs)
            lookAtExportsOf(id);
        for (const ModuleRequest& request : m_program.at(id).requests) {
            if (!request.isDynamicStarExport)
                continue;
            mayFindSomething |= !m_modulesWhoseValueStaysInSight.quickGet(request.target.index());
            lookAtExportsOf(commonJsModuleCapturedBy(m_program, m_program.at(request.target)));
        }
        FunctionID body = m_program.at(id).body;
        for (uint32_t read : m_flow.summaries.at(body).namesThatHostLooksFor) {
            const LinkType& found = m_flow.typeAt(m_flow.nodeOf(body, read));
            LinkType nothing;
            nothing.primitives = found.primitives & (Primitive::UndefinedOrNull | Primitive::Absent);
            mayFindSomething |= !found.holdsSameAs(nothing);
        }
        if (!mayFindSomething)
            return false;
        m_builtinsInReach.noteHostMayFindSomethingAt(index);
        if (m_program.at(id).kind == ModuleKind::CommonJs || std::ranges::any_of(m_program.at(id).requests, &ModuleRequest::isDynamicStarExport))
            escapeExportsOf(id);
        else
            showExportsThatHostReadsOff(m_program.entries[index], HostWith::Takes);
        return true;
    });
}

void StateOfLinkReach::escapeFacadesWithGetters()
{
    for (ModuleID facade : m_initiallyTrackedFacades) {
        uint32_t index = facade.index();
        ModuleID captured = m_modulesWhoseValueStaysInSight.quickGet(index) ? commonJsModuleCapturedBy(m_program, m_program.modules[index]) : ModuleID();
        if (!captured)
            continue;
        if (mayHaveAccessor(m_flow.typeAt(m_flow.variable(m_program.at(captured).exportsVariable))))
            escapeValueOf(ModuleID(index));
    }
}

bool StateOfLinkReach::mayHaveAccessor(const LinkType& exports)
{
    auto hasAccessor = [&](LayoutID layout) {
        return std::ranges::contains(m_flow.fieldsWithKnownAccessors, layout, &FieldReference::layout);
    };
    switch (exports.objects) {
    case LinkType::Objects::None:
        return false;
    case LinkType::Objects::Layout:
        return hasAccessor(exports.layout);
    case LinkType::Objects::Set:
        return std::ranges::any_of(m_flow.layoutsOfSet(exports), hasAccessor);
    case LinkType::Objects::Family: {
        bool found = false;
        m_program.at(m_flow.families.layoutsOf(exports.family)).forEach([&](LayoutID member) {
            found |= hasAccessor(member);
        });
        return found;
    }
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
    case LinkType::Objects::Lost:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void StateOfLinkReach::escapeValueOf(ModuleID id)
{
    if (!m_modulesWhoseValueStaysInSight.quickClear(id.index()))
        return;
    m_flow.modulesWhoseValueStaysInSight.quickClear(id.index());
    m_flow.modulesWhoseRecordIsHandedOutFrozen.quickClear(id.index());
    if (VariableID exportsVariable = m_program.at(id).exportsVariable)
        escapeVariable(exportsVariable);
    if (ModuleID captured = commonJsModuleCapturedBy(m_program, m_program.at(id))) {
        for (const SyntheticExport& synthetic : m_program.at(id).syntheticExports) {
            if (synthetic.variable)
                escapeVariable(synthetic.variable);
        }
        escapeValueOf(captured);
    }
    if (loaderFillsVariablesOffValueOf(m_program.at(id))) {
        for (const SyntheticExport& synthetic : m_program.at(id).syntheticExports) {
            if (synthetic.variable)
                escapeVariable(synthetic.variable);
        }
    }
    FunctionID body = m_program.at(id).body;
    for (uint32_t result = 0; body && result < m_program.at(body).results.size(); ++result)
        m_flow.addEdge(m_flow.resultToFollow(body, result), m_flow.unseen());
}

void StateOfLinkReach::escapeVariable(VariableID id, bool isExpectedToStayAsLaid)
{
    if (m_flow.lanesTakeStepsOfReach()) {
        if (m_flow.escapedVariables.get(id.index()))
            return;
        leave(Left::VariableHasEscaped, id.index());
    } else if (m_flow.escapedVariables.set(id.index()))
        return;
    FlowNodeID node = m_flow.variable(id);
    m_flow.arrive(node, isExpectedToStayAsLaid ? LinkType::anything().asEscapeOnly() : LinkType::anything());
    m_flow.addEdge(node, m_flow.unseen());
}

void StateOfLinkReach::escapeField(FieldReference reference)
{
    uint32_t cell = cellOf(reference);
    if (m_flow.fieldsThatUnseenCodeMayWrite.get(cell))
        return;
    FlowNodeID node = m_flow.field(reference);
    m_flow.addEdge(node, m_flow.unseen());
    m_flow.arrive(node, LinkType::anything().asEscapeOnly());
    uint32_t cellOfDeclaration = cellOf(declarationOf(m_program, reference));
    if (m_flow.lanesTakeStepsOfReach()) {
        leave(Left::FieldMayBeWritten, cell);
        if (cellOfDeclaration != cell)
            leave(Left::FieldMayBeWritten, cellOfDeclaration);
        return;
    }
    m_flow.fieldsThatUnseenCodeMayWrite.set(cell);
    m_flow.fieldsThatUnseenCodeMayWrite.set(cellOfDeclaration);
}

void StateOfLinkReach::waitOnConditionsOf(BuiltinID canonical)
{
    if (m_builtinsWaitingOnConditions.isEmpty() || m_builtinsWaitingOnConditions.last() != canonical)
        m_builtinsWaitingOnConditions.append(canonical);
}

bool StateOfLinkReach::mayHold(const BuiltinCondition* condition, BuiltinID canonical)
{
    if (!condition)
        return true;
    if (condition->buildAdmitsRuntimeCode)
        return false;
    bool waitsForWrite = condition->objectNobodyKnowsIsWritten && !m_builtinsInReach.mayBeHandedWhatIsNoTextNumberOrBoolean(canonical);
    bool waitsForCall = condition->callRunsOtherCode && !m_builtinsInReach.hasCallThatMayRunCode(canonical);
    bool waitsToBeAskedForMore = condition->isAskedForOtherNames && !m_builtinsInReach.isAskedForOtherNames(m_program.builtins().canonicalID(condition->isAskedForOtherNames));
    bool waitsForOperand = condition->nameToldUnder() && !m_builtinsInReach.hasCallWhoseOperandMayMeet(*condition, canonical);
    bool waitsForStore = condition->programStores && m_builtinsInReach.takeForConstant(condition->programStores);
    if (waitsForWrite || waitsForCall || waitsToBeAskedForMore || waitsForOperand || waitsForStore) {
        waitOnConditionsOf(canonical);
        return false;
    }
    return !condition->testsReplaceBuiltins || m_program.testsReplaceBuiltins();
}

void StateOfLinkReach::enterLibraryFunction(RuntimeFunctionID id)
{
    const RuntimeFunctionEntry* held = findRuntimeFunction(m_program, id);
    if (!held || !held->function)
        return;
    m_flow.libraryFunctionsEntered.set(held->function.index());
    escapeFunction(held->function);
}

void StateOfLinkReach::letGoWhatHostHandsOver(const BuiltinHostUses& uses)
{
    const BuiltinTable& builtins = m_program.builtins();
    auto letGo = [&](const BuiltinPlace& place) {
        if (place.kind != BuiltinPlaceKind::NativeCell)
            return;
        for (const BuiltinHostObject& object : builtins.hostObjects()) {
            if (object.nativeCell == place.nativeCell && object.function)
                m_builtinsInReach.noteLetGo(builtins.canonicalID(object.function));
        }
    };
    for (const BuiltinHostUse& use : uses.list) {
        if (use.use != BuiltinHostReadUse::Called || !use.call)
            continue;
        letGo(use.call->thisValue);
        for (const BuiltinPlace& argument : use.call->arguments)
            letGo(argument);
        letGo(use.call->otherArguments);
        letGoWhatHostHandsOver(use.call->result);
    }
}

void StateOfLinkReach::loadLibraryModule(RuntimeModuleID id)
{
    for (uint32_t index = 0; id && index < m_program.modules.size(); ++index) {
        if (m_program.modules[index].runtimeModule != id)
            continue;
        evaluate(ModuleID(index));
        escapeExportsOf(ModuleID(index));
    }
}

void StateOfLinkReach::enterLibraryBehind(BuiltinID canonical)
{
    const BuiltinTable& builtins = m_program.builtins();
    const Builtin& entry = builtins.at(canonical);
    bool mayRun = m_builtinsInReach.mayRun(canonical);
    bool mayRunOrHasObjects = m_builtinsInReach.mayRunOrHasObjects(canonical);
    if (!mayRun)
        waitOnConditionsOf(canonical);
    bool isNativeCodeOfHost = entry.origin == BuiltinOrigin::Host && !entry.flags.contains(BuiltinFlag::IsCompilerHelper);
    for (const BuiltinOverload& overload : builtins.overloadsOf(canonical)) {
        if (isNativeCodeOfHost && mayRun && overload.isUndeclared())
            enterWhatUndeclaredNativeCodeCalls();
    }
    for (const BuiltinCalledFunction& called : builtins.calledFunctionsOf(canonical)) {
        if (!mayRunOrHasObjects && called.uses.areListed && !hostUsesPutValueInto(builtins, called.uses, canonical))
            continue;
        if (!mayHold(called.condition, canonical))
            continue;
        if (builtins.contains(called.function) && !builtins.at(called.function).platforms.contains(targetOfThisBuild().platform))
            continue;
        enterLibraryFunction(called.function);
        letGoWhatHostHandsOver(called.uses);
    }
    for (const BuiltinModuleLoad& load : builtins.moduleLoadsOf(canonical)) {
        bool makesValue = std::ranges::any_of(load.usesOfExports, [&](const BuiltinHostUses& uses) {
            return hostUsesPutValueInto(builtins, uses, canonical);
        });
        bool rowSaysWhereExportsGo = !load.readsEveryExport && std::ranges::all_of(load.usesOfExports, &BuiltinHostUses::areListed);
        if (!mayRunOrHasObjects && rowSaysWhereExportsGo && !makesValue)
            continue;
        if (!mayHold(load.condition, canonical))
            continue;
        loadLibraryModule(load.module);
        for (const BuiltinHostUses& uses : load.usesOfExports)
            letGoWhatHostHandsOver(uses);
    }
}

void StateOfLinkReach::letGoWhatNativeCodeReadsOffModules()
{
    for (ModuleID id : m_modulesThatNativeCodeReadsNamesOff) {
        if (!m_modulesWhoseValueStaysInSight.quickGet(id.index()))
            continue;
        const Module& module = m_program.at(id);
        LinkType value = m_flow.typeAt(m_flow.result(module.body, 0));
        bool everyNameIsField = value.functions == LinkType::Functions::None && value.classes == LinkType::Classes::None && !value.builtin
            && value.objects != LinkType::Objects::Prototype && value.objects != LinkType::Objects::Prototypes;
        m_flow.forEachLayoutOf(value, [&](LayoutID holder) {
            for (BuiltinName name : m_program.builtins().at(module.runtimeModule).exportsReadByNativeCode) {
                PropertyKeyID key = m_program.keyOf(name);
                std::optional<FieldReference> field = key ? findField(m_program, holder, key) : std::nullopt;
                if (field && std::ranges::contains(m_program.builtins().at(module.runtimeModule).memberOnlyExports, name))
                    continue;
                if (field)
                    escapeField(*field);
                else
                    everyNameIsField = false;
            }
        });
        if (!everyNameIsField)
            escapeValueOf(id);
    }
}

bool StateOfLinkReach::handsBackRecordThatStaysAsBorn(ModuleID id)
{
    LinkType value = m_flow.typeAt(m_flow.result(m_program.at(id).body, 0));
    bool staysAsBorn = value.functions == LinkType::Functions::None && value.classes == LinkType::Classes::None && !value.builtin && !value.hasUnknownObjects && !value.holdsLostBuiltins
        && (value.objects == LinkType::Objects::None || value.objects == LinkType::Objects::Layout || value.objects == LinkType::Objects::Set);
    if (!staysAsBorn)
        return false;
    m_flow.forEachLayoutOf(value, [&](LayoutID holder) {
        const Layout& layout = m_program.at(holder);
        staysAsBorn &= layout.kind == LayoutKind::Record && !m_flow.escapedLayouts.get(holder.index()) && (m_flow.possibleChanges[holder.index()] - LayoutChange::IsFrozen).isEmpty() && !m_flow.somethingIsStoredBesideFieldsOf(holder);
        for (uint32_t field = 0; field < layout.fields.size(); ++field)
            staysAsBorn &= !m_flow.fieldsWrittenAfterConstruction.get(cellOf(FieldReference { holder, field }));
    });
    return staysAsBorn;
}

void StateOfLinkReach::stopUnlessRecordsHandedOutFrozenStayAsBorn()
{
    for (size_t index : m_flow.modulesWhoseRecordIsHandedOutFrozen)
        RELEASE_ASSERT(!m_modulesWhoseValueStaysInSight.quickGet(index) || handsBackRecordThatStaysAsBorn(ModuleID(static_cast<uint32_t>(index))));
}

void StateOfLinkReach::handOutRecordsOfInternalFiles()
{
    BitVector frozenRecordModules = m_flow.modulesWhoseRecordIsHandedOutFrozen;
    for (size_t index : frozenRecordModules) {
        ModuleID id(static_cast<uint32_t>(index));
        FunctionID body = m_program.at(id).body;
        if (!body || !m_modulesWhoseValueStaysInSight.quickGet(index)) {
            escapeValueOf(id);
            m_flow.modulesWhoseRecordIsHandedOutFrozen.quickClear(index);
            continue;
        }
        if (!handsBackRecordThatStaysAsBorn(id)) {
            escapeValueOf(id);
            continue;
        }
        m_flow.forEachLayoutOf(m_flow.typeAt(m_flow.result(body, 0)), [&](LayoutID holder) {
            if (!m_layoutsOfRecordsHandedOut.add(holder.index()))
                return;
            m_flow.note(LinkFlow::Note::ChangeOfLayout, holder.index(), static_cast<uint32_t>(LayoutChange::IsFrozen));
            for (uint32_t field = 0; field < m_program.at(holder).fields.size(); ++field)
                m_flow.addEdge(m_flow.field(FieldReference { holder, field }), m_flow.unseen());
        });
    }
}

void StateOfLinkReach::enterWhatUndeclaredNativeCodeCalls()
{
    if (std::exchange(m_hasEnteredWhatUndeclaredNativeCodeCalls, true))
        return;
    enterLibraryBehind(m_program.builtins().canonicalID(Builtins::UndeclaredNativeCode));
    enterWhatHostLoadsToPrintObjects();
}

void StateOfLinkReach::enterWhatHostLoadsToPrintObjects()
{
    if (std::exchange(m_hasEnteredWhatHostLoadsToPrintObjects, true))
        return;
    const BuiltinTable& builtins = m_program.builtins();
    for (const BuiltinModuleLoad& load : builtins.startupLoads()) {
        loadLibraryModule(load.module);
        for (const BuiltinHostUses& uses : load.usesOfExports)
            letGoWhatHostHandsOver(uses);
    }
    for (RuntimeModuleID module : builtins.startupModules())
        loadLibraryModule(module);
    for (RuntimeFunctionID function : builtins.functionsCalledWithStartupLoads())
        enterLibraryFunction(function);
    for (const BuiltinCalledFunction& called : builtins.startupCalledFunctions()) {
        enterLibraryFunction(called.function);
        letGoWhatHostHandsOver(called.uses);
    }
}

void StateOfLinkReach::enterLoaderOfHost()
{
    if (!std::exchange(m_hasEnteredLoaderOfHost, true)) {
        for (RuntimeFunctionID function : m_program.builtins().moduleLoaderFunctions())
            enterLibraryFunction(function);
    }
    if (!m_builtinsInReach.defersRuntimeNames() && !std::exchange(m_hasEnteredWhatLoaderOfHostFinds, true)) {
        for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
            const Module& module = m_program.modules[index];
            if (isProgramModuleOf(module.kind) && !module.flags.contains(ModuleFlags::IsLibrary))
                continue;
            evaluate(ModuleID(index));
            escapeExportsOf(ModuleID(index));
        }
    }
    letExportsOfEntriesEscape();
}

void StateOfLinkReach::letExportsOfEntriesEscape()
{
    if (std::exchange(m_hasLetExportsOfEntriesEscape, true))
        return;
    for (const Entry& entry : m_program.entries)
        escapeExportsOf(entry.module);
}

void StateOfLinkReach::enterWhatUnseenCodeFinds()
{
    if (std::exchange(m_hasEnteredWhatUnseenCodeFinds, true))
        return;
    for (const RuntimeFunctionEntry& entry : m_program.runtimeFunctions) {
        if (entry.function && m_functionsCalledByHost.get(entry.function.index()))
            enterLibraryFunction(entry.id);
    }
    enterWhatUndeclaredNativeCodeCalls();
    enterLoaderOfHost();
}

bool StateOfLinkReach::somebodyCanClimbToWhatIsLaidAbovePrototype()
{
    const BuiltinTable& table = m_program.builtins();
    if (m_builtinsThatLeadToHoldersLinkedAnew.isEmpty()) {
        for (uint32_t index = 0; index < table.builtinCount(); ++index) {
            BuiltinID builtin(index);
            if (table.canonicalID(builtin) != builtin || !lookupPassesHolderLinkedAnew(m_program, builtin, { }))
                continue;
            m_builtinsThatLeadToHoldersLinkedAnew.append(builtin);
            if (table.at(builtin).prototypeLink != BuiltinPrototypeLink::Unknown)
                continue;
            m_holdersLinkedAnew.append(builtin);
            for (BuiltinID member : table.membersOf(builtin))
                m_builtinsThatLeadToHoldersLinkedAnew.append(table.canonicalID(member));
        }
    }
    if (m_builtinsInReach.loaderOfMachineCodeIsInUse() || !m_flow.everythingLaidAbovePrototypeMayWait(m_holdersLinkedAnew.span()))
        return true;
    return std::ranges::any_of(m_builtinsThatLeadToHoldersLinkedAnew, [&](BuiltinID builtin) {
        return m_builtinsInReach.isOutOfSight(builtin);
    });
}

bool StateOfLinkReach::somebodyCanBeHandedPromises()
{
    if (m_builtinsInReach.unseenCodeCanRun() || m_builtinsInReach.loaderOfMachineCodeIsInUse() || m_flow.testRuns().isTestRun())
        return true;
    const BuiltinTable& builtins = m_program.builtins();
    return m_flow.listenerMayBeHandedPromises() || std::ranges::any_of(builtins.marked(BuiltinMark::MayLayListenerForUncaughtExceptions), [&](const BuiltinMarkedEntry& marked) {
        return m_builtinsInReach.isOutOfSight(builtins.canonicalID(marked.builtin));
    });
}

bool StateOfLinkReach::enterLibraryInReach()
{
    RELEASE_ASSERT(m_steps.isEmpty() && m_stepsThatOneThreadTakes.isEmpty() && m_functionsToBringAlive.isEmpty());
    escapeFacadesWithGetters();
    tellWhatHostFindsAtEntries();
    bool somethingHasLeft = m_flow.letGoWhoWaitsForAccessorsLaidAbove();
    if (m_flow.objectsReachUnseen() && !std::exchange(m_nativeCodeMayHoldObjects, true)) {
        m_builtinsInReach.noteNativeCodeMayHoldObjects();
    }
    for (BuiltinID builtin : m_flow.takeBuiltinsLost())
        m_builtinsInReach.noteLetGo(builtin);
    for (BuiltinID builtin : m_flow.takeBuiltinsLostInSight())
        m_builtinsInReach.noteLost(builtin);
    for (BuiltinID builtin : m_flow.takeBuiltinsAtUnseen())
        m_builtinsInReach.noteAtUnseen(builtin);
    if (m_flow.lostBuiltinsReachUnseen())
        m_builtinsInReach.noteLostBuiltinsReachUnseen();
    if (m_flow.builtinsReadByNameReachUnseen())
        m_flow.note(LinkFlow::Note::BuiltinsReadByNameAreUsed, 0);
    if (m_flow.builtinsLostAtJoinsReachUnseen())
        m_flow.note(LinkFlow::Note::BuiltinsLostAtJoinsAreUsed, 0);
    LinkFlow::ClassesOfSites classes = m_flow.findClassesOfSitesThatHaveEscapedOrAreClimbedFrom();
    for (size_t builtinClass : classes.haveEscaped)
        m_builtinsInReach.noteInstanceAtUnseen(BuiltinClassID(static_cast<uint32_t>(builtinClass)));
    for (size_t builtinClass : classes.haveEscaped)
        m_builtinsInReach.noteInstanceHasEscaped(BuiltinClassID(static_cast<uint32_t>(builtinClass)));
    for (size_t builtinClass : classes.areClimbedFrom)
        m_builtinsInReach.noteLost(m_flow.prototypeOfInstancesOf(BuiltinClassID(static_cast<uint32_t>(builtinClass))));
    for (FunctionID answering : m_builtinsInReach.functionsThatAnswerEveryNameOnInstances()) {
        for (uint32_t index = 0; index < m_program.at(answering).results.size(); ++index) {
            LinkType answered = m_flow.typeAt(m_flow.resultToFollow(answering, index));
            answered.primitives = 0;
            if (!answered.isNever())
                m_builtinsInReach.noteInstancesAnswerEveryNameWithObjects();
        }
    }
    m_builtinsThatCameIntoReach.shrink(0);
    m_builtinsThatCameOutOfSight.shrink(0);
    m_flow.takeOverCallsWhoseRowsLinkTells();
    m_builtinsInReach.settle(m_builtinsThatCameIntoReach, m_builtinsThatCameOutOfSight);
    if (m_flow.hasSomethingLaidAbovePrototype()) {
        m_flow.noteWhatIsLaidAbovePrototype();
        if (somebodyCanClimbToWhatIsLaidAbovePrototype()) {
            m_flow.letGoWhatIsLaidAbovePrototype();
            somethingHasLeft = true;
        }
    }
    m_membersWrittenAfterAll = m_builtinsInReach.takeMembersWrittenAfterAll();
    for (BuiltinID member : m_membersWrittenAfterAll)
        m_flow.arrive(m_flow.writtenAtMember(member), LinkType::anything());
    bool holdersThatAreWatchedHaveComeOutOfSight = false;
    for (const BuiltinsInReach::NameOnHolder& laid : m_builtinsInReach.takeNamesLaidAfterAll()) {
        m_flow.arrive(m_flow.laidOnBuiltin(laid.holder, laid.key), LinkType::anything());
        if (m_flow.isGlobalObject(laid.holder))
            m_flow.arrive(m_flow.heldByGlobalObject(laid.key), LinkType::anything());
        holdersThatAreWatchedHaveComeOutOfSight = true;
    }
    for (BuiltinID builtin : m_builtinsThatCameOutOfSight) {
        if (isPrototypeOfPrimitives(m_program.builtins(), builtin))
            continue;
        FlowNodeID watched = m_flow.findLaidOnBuiltin(builtin, { });
        if (!watched || !m_flow.typeAt(watched).isNever())
            continue;
        m_flow.arrive(watched, LinkType::anything());
        holdersThatAreWatchedHaveComeOutOfSight = true;
    }
    somethingHasLeft |= showModuleObjectsOfGlobalObject();
    for (BuiltinID cameIntoReach : m_builtinsThatCameIntoReach)
        m_flow.testRuns().noteCameIntoReach(cameIntoReach);
    m_flow.testRuns().lookAtWhatHasSettled();
    for (BuiltinID waiting : std::exchange(m_builtinsWaitingOnConditions, { }))
        enterLibraryBehind(waiting);
    bool entriesHaveComeToBeAskedForMore = false;
    for (const BuiltinCondition* condition : m_program.builtins().conditionsOnLightForms()) {
        BuiltinID entry = m_program.builtins().canonicalID(condition->isAskedForOtherNames);
        FlowNodeID observed = m_flow.findWrittenAtMember(entry);
        if (!observed || !m_flow.typeAt(observed).isNever() || !m_builtinsInReach.isAskedForOtherNames(entry))
            continue;
        m_flow.arrive(observed, LinkType::anything());
        entriesHaveComeToBeAskedForMore = true;
    }
    for (BuiltinID builtin : m_builtinsThatCameIntoReach)
        enterLibraryBehind(builtin);
    for (BuiltinID builtin : m_builtinsThatCameOutOfSight) {
        for (const BuiltinOverload& overload : m_program.builtins().overloadsOf(builtin))
            enterLibraryFunction(overload.code.implementation.function);
        for (const BuiltinCalledFunction& called : m_program.builtins().calledFunctionsOf(m_program.builtins().canonicalID(builtin))) {
            if (!called.condition && called.uses.areListed)
                enterLibraryFunction(called.function);
        }
    }
    if (m_nativeCodeMayHoldObjects && m_builtinsInReach.objectsMayHaveMethodCalledWithStartupLoads())
        enterWhatHostLoadsToPrintObjects();
    if (m_flow.testRuns().isTestRun() && !std::exchange(m_hasEnteredLoaderFunctionsForTestRun, true)) {
        for (RuntimeFunctionID function : m_program.builtins().moduleLoaderFunctions())
            enterLibraryFunction(function);
        enterLibraryBehind(m_program.builtins().canonicalID(Builtins::ModuleRequireResolve));
    }
    if (m_builtinsInReach.hostIsAskedForValueOfModule() && !std::exchange(m_hasEnteredLoaderFunctionsForValueOfHost, true)) {
        for (RuntimeFunctionID function : m_program.builtins().moduleLoaderFunctions())
            enterLibraryFunction(function);
    }
    if (m_builtinsInReach.defersRuntimeNames() && m_builtinsInReach.unseenCodeCanRun())
        letExportsOfEntriesEscape();
    if (m_builtinsInReach.defersRuntimeNames() && (m_builtinsInReach.unseenCodeCanRun() || m_builtinsInReach.loaderOfHostMayRun())) {
        for (ModuleID module : m_builtinsInReach.takeModulesSpelledInTexts()) {
            evaluate(module);
            escapeExportsOf(module);
            const Module& spelled = m_program.at(module);
            const BuiltinTable& builtins = m_program.builtins();
            BuiltinID object = spelled.kind == ModuleKind::Runtime && spelled.runtimeModule ? builtins.canonicalID(builtins.namespaceOf(spelled.runtimeModule)) : BuiltinID();
            if (object && !builtins.at(object).holder)
                m_builtinsInReach.noteLetGo(object);
        }
    }
    if (m_builtinsInReach.loaderOfHostMayRun())
        enterLoaderOfHost();
    if (m_builtinsInReach.unseenCodeCanRun() && !m_builtinsInReach.defersRuntimeNames())
        enterWhatUnseenCodeFinds();
    letGoWhatNativeCodeReadsOffModules();
    handOutRecordsOfInternalFiles();
    bool somebodyCanBeHandedSomething = m_nativeCodeMayHoldObjects || m_flow.rowPassesWhatNobodyFollows() || m_builtinsInReach.unseenCodeCanRun();
    bool promisesHaveChanged = somebodyCanBeHandedSomething && somebodyCanBeHandedPromises() && m_flow.letPromisesBeHandedOut();
    bool hasEntered = holdersThatAreWatchedHaveComeOutOfSight || !m_steps.isEmpty() || !m_stepsThatOneThreadTakes.isEmpty() || !m_functionsToBringAlive.isEmpty() || !m_membersWrittenAfterAll.isEmpty() || promisesHaveChanged || entriesHaveComeToBeAskedForMore;
    finish();
    return hasEntered || somethingHasLeft || m_flow.hasBuiltinsLostAtJoinsToLetGo();
}

void StateOfLinkReach::bringAlive(FunctionID id)
{
    const Function& function = m_program.at(id);
    const ThinSummary& summary = m_flow.summaries.at(id);
    prefetchRecordsOf(id, summary);
    LinkFlow::CodeAtWork codeAtWork(m_flow, id);
    LinkFlow::UnknownByEscapeAlone unknownByEscapeAlone(false);

    noteFunctionsNamedBy(id);
    bool moduleIsKnown = !function.module || m_modulesWithReachedCode.at(function.module.index());
    if (!m_builtinsInReach.hasNoFindingsIn(id) && !m_flow.tellsBuiltinsInReachBehindPass(id))
        m_builtinsInReach.noteFindingsIn(id);
    if (!m_builtinsInReach.knowsWhatBodyMakes(id)) {
        LinkFlow::LockerOfWhatIsShared locker(m_flow);
        m_builtinsInReach.noteWhatBodyMakes(id);
    }
    if (!moduleIsKnown || m_moduleWithBody[id.index()] || !summary.modulesHandedOut.isEmpty()) {
        LinkFlow::LockerOfWhatIsShared locker(m_flow);
        noteCodeOfModule(function.module);
        if (ModuleID module = m_moduleWithBody[id.index()]) {
            evaluate(module);
            noteCodeOfModule(module);
        }
        for (ModuleID module : summary.modulesHandedOut) {
            escapeExportsOf(module);
            escapeValueOf(module);
        }
    }
    if (summary.has(SummaryTrait::HasDirectEval) && !m_flow.leavesOutDirectEvals) {
        LinkFlow::LockerOfWhatIsShared locker(m_flow);
        for (uint32_t handed : summary.nodesHandedToEvals)
            m_flow.addEdge(m_flow.nodeOf(id, handed), m_flow.unseen());
        letGoWhatDirectEvalsShowIn(id);
    }
    for (PropertyKeyID key : summary.textsThatNameEvents)
        m_flow.note(LinkFlow::Note::NameRead, key.bits(), true);
    for (const ThinSummary::AnswerOfRequire& answer : summary.answersOfRequire) {
        const Module& module = m_program.at(answer.module);
        if (module.requireResult == RequireResult::ModuleExports && module.exportsVariable)
            m_flow.addEdge(m_flow.variable(module.exportsVariable), m_flow.nodeOf(id, answer.node));
        else if (objectOfLibraryFileIsResultOfBody(m_program, answer.module))
            m_flow.addEdge(m_flow.resultToFollow(module.body, 0), m_flow.nodeOf(id, answer.node));
    }
    {
        for (const ThinSummary::AnswerOfRequire& answer : summary.objectsOfCompiledModules) {
            LinkFlow::LockerOfWhatIsShared locker(m_flow);
            if (objectOfLibraryFileIsResultOfBody(m_program, answer.module)) {
                m_flow.addEdge(m_flow.resultToFollow(m_program.at(answer.module).body, 0), m_flow.nodeOf(id, answer.node));
                evaluate(answer.module);
                continue;
            }
            escapeValueOf(answer.module);
        }
        for (const ThinSummary::NameThatLoaderTakes& name : summary.namesThatLoaderTakes)
            m_flow.addEdge(m_flow.nodeOf(id, name.read), m_flow.variable(name.variable));
        for (uint32_t read : summary.namesThatHostLooksFor)
            m_flow.addEdge(m_flow.nodeOf(id, read), m_flow.unseen());
    }
    if (m_functionsCalledByHost.get(id.index()) && !m_isCodeBehindBuiltin.get(id.index()))
        m_flow.escape(id);
    if (function.constructedClass && function.kind != FunctionKind::DerivedConstructor && (function.kind != FunctionKind::BaseConstructor || m_flow.constructionMayAnswerWithOlderObject(function.constructedClass)))
        m_flow.noteAllocated(function.constructedClass);
    m_flow.noteConstructionsOf(id);
    comeAliveForUnions(m_flow, id);
    fillEntryParameters(id, function);
    for (uint32_t result = 0; result < function.results.size(); ++result)
        arriveWhatNoNodeBrings(m_flow.result(id, result), function.results[result]);

    m_flow.addEdge(m_flow.thrown(id), m_flow.unseen());

    std::span<const uint32_t> nodesOfSummary = m_flow.nodesOfSummaryOf(id);
    auto nodeAt = [&](uint32_t nodeInSummary) {
        RELEASE_ASSERT(nodeInSummary < nodesOfSummary.size());
        return FlowNodeID(nodesOfSummary[nodeInSummary]);
    };
    auto watch = [&](uint32_t nodeInSummary, FlowObserver::Kind kind, uint32_t record) {
        if (nodeInSummary != noSummaryNode)
            m_flow.observe(nodeAt(nodeInSummary), FlowObserver { kind, id, record });
    };

    Vector<uint32_t, 16> classInstances;
    if (m_flow.numberOfSitesOfSummary(id)) {
        for (uint32_t index = 0; index < summary.nodes.size(); ++index) {
            if (summary.nodes[index].kind == SummaryNode::Kind::ClassInstance)
                classInstances.append(index);
        }
    }
    for (uint32_t index = 0; index < summary.nodes.size(); ++index) {
        const SummaryNode& node = summary.nodes[index];
        switch (node.kind) {
        case SummaryNode::Kind::Unknown:
        case SummaryNode::Kind::Parameter:
        case SummaryNode::Kind::Result:
        case SummaryNode::Kind::Thrown:
        case SummaryNode::Kind::Capture:
        case SummaryNode::Kind::Variable:
        case SummaryNode::Kind::Field:
        case SummaryNode::Kind::Member:
        case SummaryNode::Kind::Join:
        case SummaryNode::Kind::ResultOfCall:
        case SummaryNode::Kind::ResultOfAccess:
        case SummaryNode::Kind::ResultOfOperator:
        case SummaryNode::Kind::ResultOfConversion:
            break;
        case SummaryNode::Kind::Contents:
            if (!slotSpeaksOfBodyOfCoroutine(SlotOfContents::fromNumber(node.second)))
                m_flow.takeAsContents(siteAt(id, classInstances.span(), node.first), SlotOfContents::fromNumber(node.second), nodeAt(index));
            break;
        case SummaryNode::Kind::Constant:
            if (TypeID written = TypeID::fromBits(node.first); holdsOnlyPrimitives(written)) {
                LinkType type = m_flow.fromTypeID(written);
                type.counts = LinkType::countsOf(primitivesInID(written), integersInID(written));
                WrittenUnit unit = WrittenUnit::fromBits(node.second);
                bool isLiteralName = unit.kind() == WrittenUnit::Kind::Text && !isOnObjectPrototype(classifyName(m_flow.program.string(unit.text())));
                if (node.second == WrittenUnit::literalString || isLiteralName)
                    type.primitives &= static_cast<uint16_t>(~Primitive::String);
                m_flow.arrive(nodeAt(index), type);
            } else
                m_flow.arriveFromSource(nodeAt(index), written);
            break;
        case SummaryNode::Kind::Allocation: {
            LayoutID layout(node.first);
            FunctionID constructor = m_program.at(layout).constructor;
            if (!constructor || m_program.at(constructor).constructedClass != layout)
                m_flow.noteAllocated(layout);
            arriveFromSource(nodeAt(index), typeOfObjectsForLinkReach(layout));
            break;
        }
        case SummaryNode::Kind::ClassInstance: {
            SiteID site = siteAt(id, classInstances.span(), index);
            m_flow.arrive(nodeAt(index), m_flow.typeOf(site));
            fillStateThatNoSummaryFollows(site, !node.second);
            break;
        }
        case SummaryNode::Kind::Closure:
            m_flow.arrive(nodeAt(index), typeOfClosuresForLinkReach(FunctionID(node.first)));
            break;
        case SummaryNode::Kind::Builtin:
            if (PropertyKeyID symbol = keyOfWellKnownSymbol(m_program, BuiltinID(node.first))) {
                LinkType oneSymbol;
                oneSymbol.primitives = LinkType::oneSymbol;
                m_flow.arrive(nodeAt(index), oneSymbol, symbol);
                break;
            }
            m_flow.arrive(nodeAt(index), typeOfBuiltinForLinkReach(m_program, BuiltinID(node.first)));
            break;
        }
    }
    std::span<const uint32_t> placesOfWrittenLists = summary.placesOfWrittenLists.span();
    for (uint32_t head = 1; !placesOfWrittenLists.empty() && head < placesOfWrittenLists[0]; head += 2 + placesOfWrittenLists[head + 1])
        m_flow.noteListIsWritten(siteAt(id, classInstances.span(), placesOfWrittenLists[head]), id, head + 2, placesOfWrittenLists[head + 1]);
    if (m_flow.isGivenMore) [[unlikely]]
        giveMoreToNodesOf(id);

    bool leavesResultUnlaid = m_flow.listsReturns(id) && !m_flow.resultIsLaid(id);
    for (const SummaryEdge& edge : summary.edges) {
        if (edge.isConversionByEquality || (leavesResultUnlaid && summary.nodes[edge.to].kind == SummaryNode::Kind::Result))
            continue;
        m_flow.addEdge(nodeAt(edge.from), nodeAt(edge.to), edge.passes);
    }

    for (const SummaryMember& member : summary.members) {
        if (member.key)
            continue;
        switch (member.side) {
        case ClassSide::Prototype:
            m_flow.escape(member.classLayout);
            break;
        case ClassSide::Static:
            if (FunctionID constructor = m_program.at(member.classLayout).constructor)
                m_flow.escape(constructor);
            break;
        }
    }

    for (LayoutID evaluated : summary.classesEvaluated) {
        for (const StaticField& field : m_program.at(evaluated).staticFields)
            m_flow.arrive(m_flow.variable(field.variable), m_flow.fromTypeID(TypeTable::absent));
    }

    for (uint32_t index = 0; index < summary.calls.size(); ++index) {
        const SummaryCall& call = summary.calls[index];
        switch (call.callee) {
        case SummaryCall::Callee::Binding:
            RELEASE_ASSERT(call.function);
            m_flow.enter(id, index, call.function);
            break;
        case SummaryCall::Callee::Name:
            watch(call.subject, FlowObserver::Kind::SubjectOfCall, index);
            watch(call.receiver, FlowObserver::Kind::ReceiverOfCall, index);
            break;
        case SummaryCall::Callee::Value:
            watch(call.subject, FlowObserver::Kind::CalleeOfCall, index);
            break;
        case SummaryCall::Callee::Builtin:
            watch(call.receiver, FlowObserver::Kind::ReceiverOfCall, index);
            m_flow.observe(m_flow.unseen(), FlowObserver { FlowObserver::Kind::SubjectOfCall, id, index });
            break;
        }
    }

    for (uint32_t index = 0; index < summary.accesses.size(); ++index) {
        const SummaryAccess& access = summary.accesses[index];
        watch(access.subject, FlowObserver::Kind::SubjectOfAccess, index);
        watch(access.keyNode, FlowObserver::Kind::KeyOfAccess, index);
    }

    for (uint32_t index = 0; index < summary.changes.size(); ++index) {
        const SummaryChange& change = summary.changes[index];
        watch(change.subject, FlowObserver::Kind::SubjectOfChange, index);
        watch(change.other, FlowObserver::Kind::OtherOfChange, index);
        if (FlowNodeID laid = m_flow.findWhatConstructorLaysAbove(id, change))
            m_flow.observe(laid, FlowObserver { FlowObserver::Kind::SubjectOfChange, id, index });
        watch(change.getterInSight, FlowObserver::Kind::OtherOfChange, index);
        watch(change.setterInSight, FlowObserver::Kind::OtherOfChange, index);
        watch(change.keyNode, FlowObserver::Kind::OtherOfChange, index);
    }

    for (uint32_t index = 0; index < summary.operators.size(); ++index) {
        const SummaryOperator& operation = summary.operators[index];
        for (uint32_t operand = 0; operand < SummaryOperator::mostOperands; ++operand) {
            watch(operation.operands[operand], FlowObserver::Kind::OperandOfOperator, index);
            watch(operation.converted[operand], FlowObserver::Kind::OperandOfOperator, index);
        }
    }

    for (const ThinSummary::Inspection& inspection : summary.inspections) {
        if (inspection.kind == ThinSummary::Inspection::Kind::ListsKeys)
            watch(inspection.subject, FlowObserver::Kind::KeysAreListed, 0);
    }
}

void StateOfLinkReach::giveMoreToNodesOf(FunctionID id)
{
    static constexpr std::array<uint8_t, 4> bitsThatMustBeZero { 2, 4, 7, 11 };
    static constexpr unsigned bitThatPicksOtherNode = 11;
    uint32_t seed = m_flow.build.options().shuffleSeed;
    if ((seed & 1) && isLibraryCode(m_program, m_program.at(id)))
        return;
    bool givesToEveryNode = seed >> 30 & 1;
    uint32_t mask = givesToEveryNode ? 0 : (1u << bitsThatMustBeZero[seed >> 1 & 3]) - 1;
    bool givesWhatOtherNodesHold = seed & 8;
    const ThinSummary& summary = m_flow.summaries.at(id);
    std::span<const uint32_t> nodesOfSummary = m_flow.nodesOfSummaryOf(id);
    uint32_t numberGivenMore = 0;
    for (uint32_t index = 0; index < summary.nodes.size(); ++index) {
        SummaryNode::Kind kind = summary.nodes[index].kind;
        if (kind != SummaryNode::Kind::ResultOfCall && kind != SummaryNode::Kind::ResultOfAccess)
            continue;
        uint32_t drawn = drawBySeed(seed, DrawnFor::NodeOfLink, id.index(), index);
        if (drawn & mask)
            continue;
        if (givesWhatOtherNodesHold && (drawn >> bitThatPicksOtherNode & 1))
            m_flow.addEdge(FlowNodeID(nodesOfSummary[(drawn >> (bitThatPicksOtherNode + 1)) % summary.nodes.size()]), FlowNodeID(nodesOfSummary[index]));
        else
            m_flow.arrive(FlowNodeID(nodesOfSummary[index]), LinkType::anything());
        ++numberGivenMore;
    }
    if (numberGivenMore)
        m_flow.build.statistics().count(StatisticID::FactNodesWidened, numberGivenMore);
}

void StateOfLinkReach::fillStateThatNoSummaryFollows(SiteID site, bool isObjectOfCoroutine)
{
    const BuiltinTable& builtins = m_program.builtins();
    for (BuiltinSlotID id : builtins.at(m_flow.classOf(site)).slots) {
        const BuiltinSlot& slot = builtins.at(id);
        if (slot.contentsSlot || (isObjectOfCoroutine && slot.holdsStepValues))
            continue;
        m_flow.arrive(m_flow.contents(site, LinkFlow::slotOf(slot)), LinkType::anything());
    }
}

SiteID StateOfLinkReach::siteAt(FunctionID id, std::span<const uint32_t> classInstancesInSummary, uint32_t classInstanceInSummary)
{
    const SummaryNode& node = m_flow.summaries.at(id).nodes[classInstanceInSummary];
    RELEASE_ASSERT(node.kind == SummaryNode::Kind::ClassInstance);
    auto found = std::ranges::lower_bound(classInstancesInSummary, classInstanceInSummary);
    return m_flow.siteOfSummary(id, static_cast<uint32_t>(found - classInstancesInSummary.begin()), BuiltinClassID(node.first));
}

void StateOfLinkReach::fillEntryParameters(FunctionID id, const Function& function)
{
    uint32_t numberOfParameters = function.numberOfEntryParameters();
    uint32_t endOfPositionalParameters = function.firstParameterIndex() + function.numberOfPositionalParameters();
    for (uint32_t parameter = 0; parameter < numberOfParameters; ++parameter) {
        FlowNodeID node = m_flow.parameter(id, parameter);
        const DeclaredType* declared = function.declaredEntryParameter(parameter);
        if (declared)
            arriveWhatNoNodeBrings(node, *declared);
        switch (function.entryParameterKind(parameter)) {
        case EntryParameter::Capture:
            break;
        case EntryParameter::This: {
            if (!function.takesThisFromConstruction() || !function.constructedClass)
                break;
            arriveFromSource(node, typeOfObjectsForLinkReach(function.constructedClass));
            std::span<const LayoutID> layoutsBelow = m_program.layoutsExpectedBelow(function.constructedClass);
            for (LayoutID below : layoutsBelow)
                arriveFromSource(node, typeOfObjectsForLinkReach(below));
            break;
        }
        case EntryParameter::Callee:
            m_flow.arrive(node, typeOfClosuresForLinkReach(id));
            break;
        case EntryParameter::Parameter:
            if (parameter < endOfPositionalParameters) {
                if (declared->written() && (m_program.at(declared->written()).primitives & Primitive::Absent))
                    m_flow.arrive(node, m_flow.fromTypeID(TypeTable::absent));
                break;
            }
            m_flow.arrive(node, m_flow.typeOf(m_flow.listOfArgumentsOf(id)));
            break;
        case EntryParameter::HiddenRest:
            m_flow.arrive(node, m_flow.typeOf(m_flow.listOfArgumentsOf(id)));
            break;
        case EntryParameter::NewTarget:
            break;
        case EntryParameter::ArgumentCount:
            if (!declared || !declared->established())
                m_flow.arrive(node, LinkType::anything());
            arriveWhatIsEstablished(node, declared);
            break;
        }
    }
}

void StateOfLinkReach::arriveWhatIsEstablished(FlowNodeID node, const DeclaredType* declared)
{
    if (!declared)
        return;
    if (std::optional<ProvenInEveryVersion> established = declared->established())
        m_flow.arriveFromSource(node, *established);
}

void StateOfLinkReach::arriveWhatNoNodeBrings(FlowNodeID node, const DeclaredType& declared)
{
    std::optional<ProvenInEveryVersion> established = declared.established();
    if (!established)
        return;
    TypeID type = *established;
    if (type && !holdsOnlyPrimitives(type) && m_program.types.at(type).opaque)
        m_flow.arrive(node, LinkType::anything());
}

void StateOfLinkReach::listSourceOfObjects(FlowNodeID node, LayoutID layout)
{
    m_sourcesOfObjects.append(SourceOfObjects { node, m_newestSourceOfLayout[layout.index()] });
    m_newestSourceOfLayout[layout.index()] = static_cast<uint32_t>(m_sourcesOfObjects.size() - 1);
}

void LinkFlow::listSourceOfObjects(FlowNodeID node, LayoutID layout)
{
    stateOfLinkReach().listSourceOfObjects(node, layout);
}

void StateOfLinkReach::arriveFromSource(FlowNodeID node, LinkType type)
{
    if (type.objects == LinkType::Objects::Layout && m_program.at(type.layout).kind != LayoutKind::ModuleNamespace) {
        uint32_t layout = type.layout.index();
        if (m_flow.escapedLayouts.get(layout)) {
            type.isUnknownEscapeOnly = !type.hasUnknownObjects;
            type.hasUnknownObjects = true;
        }
        else if (!m_flow.listsSourceOfObjectsBehindPass(node, type.layout))
            listSourceOfObjects(node, type.layout);
    }
    m_flow.arrive(node, type);
}

void StateOfLinkReach::makeSourceOfFamily(FamilyID id, FlowNodeID source)
{
    m_sourcesOfFamilies[id.index()] = source;
    m_program.at(m_flow.families.layoutsOf(id)).forEach([&](LayoutID member) {
        arriveFromSource(source, typeOfObjectsForLinkReach(member));
    });
}

void StateOfLinkReach::tieLostLayout(LayoutID id)
{
    if (m_flow.namesMayBeIncomplete(id, ClassSide::Prototype))
        escapeLayout(id);
}

void StateOfLinkReach::tieLostFunction(FunctionID id)
{
    m_flow.tieNamesOnLostClosuresOf(id);
    if (m_lostFunctionsHaveEscaped || (m_program.at(id).constructedClass && !m_flow.layoutOfConstructorFunction(id)))
        escapeFunction(id);
}

void StateOfLinkReach::openLayout(LayoutID id)
{
    const Layout& layout = m_program.at(id);
    bool lanesTakeSteps = m_flow.lanesTakeStepsOfReach();
    if (layout.kind == LayoutKind::ModuleNamespace) {
        if (layout.namespaceOf && lanesTakeSteps)
            leave(Left::ExportsEscape, layout.namespaceOf.index());
        else if (layout.namespaceOf)
            escapeExportsOf(layout.namespaceOf);
        return;
    }

    LayoutID current = id;
    for (uint32_t steps = 0; current && steps < m_numberOfLayouts; ++steps) {
        const Layout& listing = m_program.at(current);
        for (uint32_t index = 0; index < listing.fields.size(); ++index) {
            if (!isPrivateNameInLink(m_program, listing.fields[index].key))
                escapeField({ current, index });
        }
        if (listing.inheritedFields == InheritedFields::AllOfRow)
            break;
        current = listing.base;
    }
    if (lanesTakeSteps)
        leave(Left::LayoutMayChange, id.index());
    else
        m_flow.layoutsThatUnseenCodeMayChange.set(id.index());
    LinkType unseenObjects = typeOfUnseenObjectsForLinkReach().asEscapeOnly();
    for (uint32_t source = m_newestSourceOfLayout[id.index()]; source != none; source = m_sourcesOfObjects[source].next)
        m_flow.arrive(m_sourcesOfObjects[source].node, unseenObjects);

    escapeClassObject(id);
    if (layout.constructor)
        escapeFunction(layout.constructor);
    for (LayoutID below : m_program.layoutsExpectedBelow(id))
        escapeLayout(below);
}

void StateOfLinkReach::openPrototype(LayoutID id)
{
    shareMembers(id, ClassSide::Prototype);
    escapeLayout(id);
    m_flow.openPrototypeOf(id);
    m_flow.openWhatLeavesWithClassObject(id);
    if (LayoutID base = m_program.at(id).base)
        escapePrototype(base);
}

void StateOfLinkReach::openFunction(FunctionID id)
{
    const Function& function = m_program.at(id);
    LinkType anything = LinkType::anything();
    FlowNodeID unseen = m_flow.unseen();
    bool lanesTakeSteps = m_flow.lanesTakeStepsOfReach();
    reach(id);

    uint32_t numberOfParameters = function.numberOfEntryParameters();
    for (uint32_t parameter = 0; parameter < numberOfParameters; ++parameter) {
        switch (function.entryParameterKind(parameter)) {
        case EntryParameter::Capture:
        case EntryParameter::Callee:
            break;
        case EntryParameter::This:
        case EntryParameter::NewTarget:
        case EntryParameter::Parameter:
            if (lanesTakeSteps)
                leave(Left::ParameterIsPassed, m_flow.parameter(id, parameter).index());
            else
                m_parametersThatUnseenCodePasses.append(m_flow.parameter(id, parameter));
            m_flow.arrive(m_flow.parameter(id, parameter), anything.asEscapeOnly());
            break;
        case EntryParameter::ArgumentCount:
        case EntryParameter::HiddenRest:
            m_flow.arrive(m_flow.parameter(id, parameter), anything);
            break;
        }
    }
    if (function.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest }))
        m_flow.arrive(m_flow.contents(m_flow.listOfArgumentsOf(id), ContentsSlot::Element), anything);
    if (m_flow.summaries.at(id).has(SummaryTrait::OnlyPassesOnGatheredArguments)) {
        m_flow.arrive(m_flow.argumentsGatheredAtAnyPlace(id), anything.asEscapeOnly());
        m_flow.noteNumberOfArgumentsGathered(id, 0, LinkFlow::CallerOfGatherer::UnseenByEscapeAlone);
    }
    ModuleID moduleWithBody = m_moduleWithBody[id.index()];
    bool resultStaysInSight = moduleWithBody && m_modulesWhoseValueStaysInSight.get(moduleWithBody.index());
    for (uint32_t result = 0; !resultStaysInSight && result < function.results.size(); ++result)
        m_flow.addEdge(m_flow.resultToFollow(id, result), unseen);
    m_flow.addEdge(m_flow.thrown(id), unseen);
    m_flow.openClosuresOf(id);

    if (function.constructedClass)
        escapeClassObject(function.constructedClass);
}

void StateOfLinkReach::openClassObject(LayoutID classLayout)
{

    m_flow.noteAllocatedWithRowAbove(classLayout);
    m_flow.openWhatLeavesWithClassObject(classLayout);
    escapePrototype(classLayout);
    shareMembers(classLayout, ClassSide::Static);
    for (const StaticField& field : m_program.at(classLayout).staticFields)
        escapeVariable(field.variable);
}

void StateOfLinkReach::shareMembers(LayoutID classLayout, ClassSide side)
{
    for (const MemberOfClass& member : membersOf(classLayout)) {
        if (member.side != side)
            continue;
        FlowNodeID node = m_flow.member(classLayout, side, member.index);
        m_flow.arrive(node, LinkType::anything().asEscapeOnly());
        m_flow.addEdge(node, m_flow.unseen());
    }
}

void DeleterOfStateOfLinkReach::operator()(StateOfLinkReach* state) const
{
    delete state;
}

StateOfLinkReach& LinkFlow::stateOfLinkReach()
{
    if (!m_stateOfLinkReach)
        m_stateOfLinkReach.reset(new StateOfLinkReach(*this));
    return *m_stateOfLinkReach;
}

bool LinkFlow::hasEscapedAsFamily(FamilyID id) const
{
    return m_stateOfLinkReach && m_stateOfLinkReach->hasEscapedAsFamily(id);
}

void LinkFlow::reach(FunctionID id)
{
    if (reached.get(id.index()))
        return;
    if (m_severalThreadsTell) {
        noteFunctionToReach(id);
        return;
    }
    StateOfLinkReach& state = stateOfLinkReach();
    state.reach(id);
    state.finish();
}

void LinkFlow::bringAliveWhatSettlingReached()
{
    stateOfLinkReach().finish();
}

void LinkFlow::reachAll(std::span<const FunctionID> functions)
{
    StateOfLinkReach& state = stateOfLinkReach();
    for (FunctionID function : functions)
        state.reach(function);
    state.finish();
}

void LinkFlow::becomeLost(LayoutID id)
{
    if (lostLayouts.get(id.index()))
        return;
    m_lostLayoutsMayHaveGrown = true;
    StateOfLinkReach& state = stateOfLinkReach();
    state.loseLayout(id);
    state.finish();
}

void LinkFlow::becomeLost(FamilyID id)
{
    m_lostLayoutsMayHaveGrown = true;
    StateOfLinkReach& state = stateOfLinkReach();
    state.loseFamily(id);
    state.finish();
}

void LinkFlow::becomeLost(FunctionID id)
{
    if (lostFunctions.get(id.index()))
        return;
    StateOfLinkReach& state = stateOfLinkReach();
    state.loseFunction(id);
    state.finish();
}

void LinkFlow::escape(LayoutID id)
{
    if (escapedLayouts.get(id.index()))
        return;
    if (m_tellsObservers) {
        deferEscape(id);
        return;
    }
    StateOfLinkReach& state = stateOfLinkReach();
    state.escapeLayout(id);
    state.finish();
}

void LinkFlow::escape(FunctionID id)
{
    if (escapedFunctions.get(id.index()))
        return;
    if (m_tellsObservers) {
        deferEscape(id);
        return;
    }
    StateOfLinkReach& state = stateOfLinkReach();
    state.escapeFunction(id);
    state.finish();
}

void LinkFlow::escapeAllOf(const LinkType& type)
{
    if (m_tellsObservers) {
        noteReachesUnseen(type);
        return;
    }
    LinkType named = type;
    StateOfLinkReach& state = stateOfLinkReach();
    state.escapeAllOf(named);
    state.finish();
}

void LinkFlow::arriveFromSource(FlowNodeID node, LayoutID layout)
{
    stateOfLinkReach().arriveFromSource(node, typeOfObjectsForLinkReach(layout));
}

void LinkFlow::arriveFromSource(FlowNodeID node, FamilyID family)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    StateOfLinkReach& state = stateOfLinkReach();
    FlowNodeID source = state.sourceOfFamily(family);
    if (!source) {
        source = makeNode();
        state.makeSourceOfFamily(family, source);
    }
    addEdge(source, node);
}

void LinkFlow::arriveFromSource(FlowNodeID node, const LinkType& type)
{
    if (type.objects != LinkType::Objects::Set && type.objects != LinkType::Objects::Layout && type.objects != LinkType::Objects::Family) [[likely]] {
        arrive(node, type);
        return;
    }
    arriveObjectsFromSource(node, type);
}

NEVER_INLINE void LinkFlow::arriveObjectsFromSource(FlowNodeID node, const LinkType& type)
{
    if (type.objects == LinkType::Objects::Set) {
        LinkType rest = type;
        rest.objects = LinkType::Objects::None;
        rest.layout = { };
        arrive(node, rest);
        for (LayoutID member : layoutsOfSet(type))
            stateOfLinkReach().arriveFromSource(node, typeOfObjectsForLinkReach(member));
        return;
    }
    LinkType arriving = type;
    if (arriving.objects == LinkType::Objects::Family)
        arriveFromSource(node, arriving.family);
    stateOfLinkReach().arriveFromSource(node, arriving);
}

void LinkFlow::arriveFromSource(FlowNodeID node, TypeID id)
{
    LinkType type = fromTypeID(id);
    if (type.objects != LinkType::Objects::Lost || !type.layout) {
        arriveFromSource(node, type);
        return;
    }
    type.objects = LinkType::Objects::None;
    type.layout = { };
    arrive(node, type);
    for (LayoutID member : program.types.listOf(program.types.at(id).layouts))
        stateOfLinkReach().arriveFromSource(node, typeOfObjectsForLinkReach(member));
}

void LinkFlow::noteAllocatedWithRowAbove(LayoutID id)
{
    BitVector& noted = layoutsWithNotedBases();
    for (LayoutID current = id; current && !noted.set(current.index()); current = program.at(current).base)
        noteAllocated(current);
}

void LinkFlow::noteConstructed(LayoutID id)
{
    if (!constructedClasses.get(id.index()))
        note(Note::ClassConstructed, id.index());
}

void LinkFlow::noteAllocated(LayoutID id)
{
    if (!allocatedLayouts.get(id.index()))
        note(Note::LayoutAllocated, id.index());
}

bool LinkFlow::isWorthFollowingToWrites(BuiltinID canonical) const
{
    return m_stateOfLinkReach->builtinsInReach().isWorthFollowingToWrites(canonical);
}

bool LinkFlow::staysUnlaidOutOfSight(BuiltinID canonicalHolder, PropertyKeyID key) const
{
    return isPrototypeOfPrimitives(program.builtins(), canonicalHolder) && !m_stateOfLinkReach->builtinsInReach().mayHaveNameLaid(canonicalHolder, key);
}

bool LinkFlow::isOutOfSight(BuiltinID canonical) const
{
    return m_stateOfLinkReach->builtinsInReach().isOutOfSight(canonical);
}

void LinkFlow::takeForUnlaid(BuiltinID canonicalHolder, PropertyKeyID key)
{
    LockerOfWhatIsShared locker(*this);
    builtinsInReach().takeForUnlaid(canonicalHolder, key);
}

bool LinkFlow::takeForConstant(BuiltinID member)
{
    return askedOnceByLane(MadeOnDemand::TakenForConstant, member.index(), [&]() -> uint32_t {
        bool isTaken = builtinsInReach().takeForConstant(member);
        if (isTaken && gathersWhatLookupsTake()) [[unlikely]]
            m_membersThatLookupsTookForConstants.set(member.index());
        return isTaken;
    });
}

void LinkFlow::noteHolderPassedInSight(BuiltinID canonicalHolder, PropertyKeyID key)
{
    if (!gathersWhatLookupsTake()) [[likely]]
        return;
    LockerOfWhatIsShared locker(*this);
    m_holdersPassedInSight.appendIfNotContains(HolderPassedInSight { canonicalHolder, key });
}

bool LinkFlow::gathersWhatLookupsTake() const
{
    return build.options().recorded.validates || build.options().checksFlowMonotonicity;
}

Vector<String> LinkFlow::findWhatWasTakenAndNeitherHoldsNorWasTold(LinkFlow& flowThatIsAsked)
{
    const BuiltinTable& table = program.builtins();
    BuiltinsInReach& asked = flowThatIsAsked.builtinsInReach();
    auto wasTold = [&](FlowNodeID watched) { return watched && !typeAt(watched).isNever(); };
    Vector<String> found;
    uint32_t numberAsked = 0;
    for (const HolderPassedInSight& passed : m_holdersPassedInSight) {
        if (wasTold(findLaidOnBuiltin(passed.holder, passed.key)) || wasTold(findLaidOnBuiltin(passed.holder, { })))
            continue;
        ++numberAsked;
        if (asked.isOutOfSight(passed.holder) && !isPrototypeOfPrimitives(table, passed.holder))
            found.append(makeString(table.tokenOf(passed.holder), " was passed as in sight and is out of sight"_s));
        else if (asked.mayHaveNameLaid(passed.holder, passed.key))
            found.append(makeString("a name was taken for unlaid on "_s, table.tokenOf(passed.holder), " and may be laid there"_s));
    }
    for (size_t index : m_membersThatLookupsTookForConstants) {
        BuiltinID member(static_cast<uint32_t>(index));
        if (wasTold(findWrittenAtMember(member)))
            continue;
        ++numberAsked;
        if (!asked.mayStillBeTakenForConstant(member))
            found.append(makeString(table.tokenOf(member), " was taken for a constant and may be written"_s));
    }
    build.statistics().count(StatisticID::BuiltinLookupAssumptionsRechecked, numberAsked);
    return found;
}

const BuiltinCondition* LinkFlow::lightFormOf(BuiltinID canonical) const
{
    return m_stateOfLinkReach->builtinsInReach().lightFormOf(canonical);
}

bool LinkFlow::isAskedForOtherNames(BuiltinID canonical) const
{
    return m_stateOfLinkReach->builtinsInReach().isAskedForOtherNames(canonical);
}

BuiltinsInReach& LinkFlow::builtinsInReach()
{
    assertHasWhatIsSharedToItself();
    return stateOfLinkReach().builtinsInReach();
}

void LinkFlow::stopUnlessRecordsHandedOutFrozenStayAsBorn()
{
    stateOfLinkReach().stopUnlessRecordsHandedOutFrozenStayAsBorn();
}

bool LinkFlow::enterLibraryInReach()
{
    return stateOfLinkReach().enterLibraryInReach();
}

bool StateOfLinkReach::fillParametersThatUnseenCodeAlonePasses()
{
    bool hasFilledSome = false;
    for (FlowNodeID parameter : std::exchange(m_parametersThatUnseenCodePasses, { })) {
        if (!m_flow.typeAt(parameter).directPart().isNever())
            continue;
        m_flow.arrive(parameter, LinkType::anything());
        hasFilledSome = true;
    }
    return hasFilledSome;
}

bool LinkFlow::fillParametersThatUnseenCodeAlonePasses()
{
    return stateOfLinkReach().fillParametersThatUnseenCodeAlonePasses();
}

void LinkFlow::reachEntries()
{
    StateOfLinkReach& state = stateOfLinkReach();
    if (leavesOutDirectEvals)
        state.builtinsInReach().leaveOutDirectEvals();
    state.queueEntries();
    runWithDeferredUpdates(scopedLambda<void()>([&] {
        state.finish();
    }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
