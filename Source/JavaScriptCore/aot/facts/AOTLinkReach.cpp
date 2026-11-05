/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkFlow.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTFactRecords.h"
#include "AOTIRProgram.h"
#include "AOTLinkCoroutines.h"
#include "AOTLinkUnions.h"
#include "AOTLinkExceptions.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <wtf/Noncopyable.h>
#include <wtf/SetForScope.h>

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

static LinkType typeOfUnseenObjectsForLinkReach()
{
    LinkType type;
    type.holdsUnseen = true;
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
    BuiltinsInReach& builtinsInReach() { return m_builtinsInReach; }
    void finish();

private:
    static constexpr uint32_t none = UINT32_MAX;

    enum class Step : uint8_t { EnterProgram, BringAlive, EvaluateModule, TieLostLayout, TieLostFunction, OpenLayout, OpenPrototype, OpenFunction };

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

    void enterProgram();
    void fillVariables();
    void evaluate(ModuleID);
    void evaluateModule(ModuleID);
    void noteCodeOfModule(ModuleID);
    void escapeExportsOf(ModuleID);
    void findModulesWhoseValueStaysInSight();
    void escapeValueOf(ModuleID);
    BitVector findFilesThatHaveRunToTheirEndWhereOthersNameThem() const;
    BitVector findVariablesStoredFirst() const;
    void escapeVariable(VariableID);
    void escapeField(FieldReference);

    bool mayHold(const BuiltinCondition*, BuiltinID canonical);
    Vector<BuiltinID> m_builtinsWhoseWriteIsAwaited;
    void enterLibraryBehind(BuiltinID canonical);
    void enterLibraryFunction(RuntimeFunctionID);
    void letGoWhatHostHandsOver(const BuiltinHostUses&);
    void loadLibraryModule(RuntimeModuleID);
    bool m_hasEnteredLoaderFunctionsForTestRun { false };
    void enterWhatUndeclaredNativeCodeCalls();
    void enterWhatHostLoadsToPrintObjects();
    void enterLoaderOfHost();
    void enterWhatUnseenCodeFinds();

    void bringAlive(FunctionID);
    void letGoWhatDirectEvalsShowIn(FunctionID);
    void prefetchSummaryOf(FunctionID) const;
    void prefetchRecordsOf(FunctionID, const ThinSummary&) const;
    SiteID siteAt(FunctionID, std::span<const uint32_t> classInstancesInSummary, uint32_t classInstanceInSummary);
    void fillStateThatNoSummaryFollows(SiteID, bool isObjectOfCoroutine);
    void fillEntryParameters(FunctionID, const Function&);
    void arriveWhatIsEstablished(FlowNodeID, const DeclaredType*);
    void arriveWhatNoNodeBrings(FlowNodeID, const DeclaredType&);
    void fillFields();
    void watch(FunctionID, uint32_t nodeInSummary, FlowObserver::Kind, uint32_t record);

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
    void openClassObject(LayoutID);
    void shareMembers(LayoutID classLayout, ClassSide);

    uint32_t cellOf(FieldReference reference) const { return m_firstFieldOfLayout[reference.layout.index()] + reference.field; }
    std::span<const MemberOfClass> membersOf(LayoutID id) const LIFETIME_BOUND { return m_membersOfClasses.span().subspan(m_firstMemberOfClass[id.index()], m_firstMemberOfClass[id.index() + 1] - m_firstMemberOfClass[id.index()]); }

    LinkFlow& m_flow;
    const Program& m_program;
    uint32_t m_numberOfLayouts { 0 };
    BitVector m_functionsCalledByHost;

    Vector<QueuedStep> m_steps;
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
    BitVector m_modulesWithReachedCode;
    BitVector m_modulesWithEscapedExports;
    BitVector m_modulesWhoseValueStaysInSight;

    Vector<std::pair<ModuleID, VariableID>> m_variablesOfLibraryFiles;
    BuiltinsInReach m_builtinsInReach;
    Vector<BuiltinID> m_builtinsThatCameIntoReach;
    Vector<BuiltinID> m_builtinsThatCameOutOfSight;
    Vector<BuiltinID> m_membersWrittenAfterAll;
    BitVector m_isCodeBehindBuiltin;
    bool m_nativeCodeMayHoldObjects { false };
    bool m_hasEnteredWhatUndeclaredNativeCodeCalls { false };
    bool m_hasEnteredWhatHostLoadsToPrintObjects { false };
    bool m_hasEnteredLoaderOfHost { false };
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
    m_modulesWithReachedCode = BitVector(m_program.modules.size());
    m_modulesWithEscapedExports = BitVector(m_program.modules.size());
    findModulesWhoseValueStaysInSight();

    m_moduleWithBody.fill(ModuleID(), numberOfFunctions);
    for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
        if (FunctionID body = m_program.modules[index].body)
            m_moduleWithBody[body.index()] = ModuleID(index);
    }

    m_firstFieldOfLayout.reserveInitialCapacity(m_numberOfLayouts + 1);
    uint32_t numberOfFields = 0;
    for (const Layout& layout : m_program.layouts) {
        m_firstFieldOfLayout.append(numberOfFields);
        numberOfFields += static_cast<uint32_t>(layout.fields.size());
    }
    m_firstFieldOfLayout.append(numberOfFields);

    m_newestSourceOfLayout.fill(none, m_numberOfLayouts);
    m_sourcesOfFamilies.fill(FlowNodeID(), flow.families.size());

    m_firstMemberOfClass.fill(0, m_numberOfLayouts + 2);
    for (size_t index = 0; index < numberOfFunctions; ++index) {
        for (const SummaryMember& member : flow.summaries.at(FunctionID(index)).members)
            ++m_firstMemberOfClass[member.classLayout.index() + 2];
    }
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
    if (m_isWorking)
        return;
    SetForScope isWorking(m_isWorking, true);
    Vector<FunctionID> wave;
    while (true) {
        while (!m_steps.isEmpty())
            take(m_steps.takeLast());
        if (m_functionsToBringAlive.isEmpty() || m_flow.letsFunctionsWaitToBeBroughtAlive())
            return;
        wave.shrink(0);
        std::swap(wave, m_functionsToBringAlive);
        if (m_flow.bringAliveOnLanes(wave.span(), scopedLambda<void(FunctionID)>([&](FunctionID function) {
            bringAlive(function);
        })))
            continue;
        for (size_t index = 0; index < wave.size(); ++index) {
            if (index + 1 < wave.size())
                prefetchSummaryOf(wave[index + 1]);
            bringAlive(wave[index]);
        }
    }
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
    LinkFlow::UnknownByEscapeAlone unknownByEscapeAlone(queued.step == Step::OpenLayout || queued.step == Step::OpenPrototype || queued.step == Step::OpenFunction);
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
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void StateOfLinkReach::queueEntries()
{
    if (!std::exchange(m_hasQueuedEntries, true))
        m_steps.append(QueuedStep { Step::EnterProgram, 0 });
}

void StateOfLinkReach::reach(FunctionID id)
{
    if (!m_flow.reached.set(id.index()))
        m_functionsToBringAlive.append(id);
}

void StateOfLinkReach::loseLayout(LayoutID id)
{
    if (!m_flow.lostLayouts.set(id.index())) {
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
    if (!m_flow.escapedLayouts.set(id.index()))
        m_steps.append(QueuedStep { Step::OpenLayout, id.index() });
}

void StateOfLinkReach::escapePrototype(LayoutID id)
{
    if (!m_escapedPrototypes.set(id.index()))
        m_steps.append(QueuedStep { Step::OpenPrototype, id.index() });
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
    case LinkType::Functions::Lost:
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
    BitVector isRequested(m_program.modules.size());
    for (const Module& module : m_program.modules) {
        for (const ModuleRequest& request : module.requests)
            isRequested.quickSet(request.target.index());
    }
    for (uint32_t index = 0; index < m_program.functions.size(); ++index) {
        const ThinSummary& summary = m_flow.summaries.at(FunctionID(index));
        for (FunctionID named : summary.functionsNamed) {
            if (ModuleID module = m_moduleWithBody[named.index()])
                isRequested.quickSet(module.index());
        }
        for (ModuleID module : summary.modulesNamedInBlocksNeverEntered)
            isRequested.quickSet(module.index());
    }
    for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
        const Module& module = m_program.modules[index];
        bool isOfProgram = isProgramModuleOf(module.kind) && !module.flags.contains(ModuleFlags::IsLibrary);
        bool isHandedOut = module.flags.containsAny({ ModuleFlags::IsHandedOutByTestRunner, ModuleFlags::IsShownByEntryOfCache });
        if (!isHandedOut && !isAskedFor.get(index) && (!isOfProgram || isRequested.get(index)))
            continue;
        evaluate(ModuleID(index));
        escapeExportsOf(ModuleID(index));
    }
}

BitVector StateOfLinkReach::findFilesThatHaveRunToTheirEndWhereOthersNameThem() const
{
    static constexpr OptionSet<ModuleFlags> flagsOfModulesThatOthersMayFindUnfinished {
        ModuleFlags::IsFoundByName, ModuleFlags::MayBeMocked, ModuleFlags::MayBeEvicted, ModuleFlags::RunsAgain, ModuleFlags::IsImportedAsNamespace, ModuleFlags::AnswersLoaderWithNamespace,
        ModuleFlags::IsListedInCache, ModuleFlags::IsShownByEntryOfCache, ModuleFlags::IsHandedOutByTestRunner, ModuleFlags::MayBeImportedWhileItRuns, ModuleFlags::HasTopLevelAwait,
    };
    uint32_t numberOfModules = static_cast<uint32_t>(m_program.modules.size());
    BitVector found(numberOfModules);
    for (uint32_t index = 0; index < numberOfModules; ++index) {
        const Module& module = m_program.modules[index];
        if (module.kind == ModuleKind::CommonJs && module.body && module.flags.contains(ModuleFlags::KeepsError) && !module.flags.containsAny(flagsOfModulesThatOthersMayFindUnfinished))
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

BitVector StateOfLinkReach::findVariablesStoredFirst() const
{
    BitVector isLeftOut(m_program.modules.size());
    BitVector hasRunToItsEnd = findFilesThatHaveRunToTheirEndWhereOthersNameThem();
    for (uint32_t index = 0; index < m_program.functions.size(); ++index) {
        for (ModuleID module : m_flow.summaries.at(FunctionID(index)).otherModulesNamed) {
            if (!hasRunToItsEnd.quickGet(module.index()))
                isLeftOut.quickSet(module.index());
        }
    }
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
    for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
        FunctionID body = m_program.modules[index].body;
        if (!body || bodiesOfSeveralModules.quickGet(body.index()))
            isLeftOut.quickSet(index);
        if (isLeftOut.quickGet(index))
            continue;
        for (SummaryPlaceInBody use : m_flow.summaries.at(body).firstUsesOfFunctions)
            starts.append(Start { use.position, FunctionID(use.number) });
    }
    std::ranges::sort(starts, { }, &Start::position);

    BitVector couldRun(m_program.functions.size());
    Vector<uint32_t> placeOfFirstReader;
    placeOfFirstReader.fill(UINT32_MAX, m_program.variables.size());
    Vector<FunctionID> toVisit;
    for (const Start& start : starts) {
        ModuleID module = m_program.at(start.function).module;
        toVisit.append(start.function);
        while (!toVisit.isEmpty()) {
            FunctionID function = toVisit.takeLast();
            if (m_program.at(function).module != module || m_moduleWithBody[function.index()] || couldRun.quickSet(function.index()))
                continue;
            const ThinSummary& summary = m_flow.summaries.at(function);
            for (const SummaryNode& node : summary.nodes) {
                if (node.kind == SummaryNode::Kind::Closure)
                    toVisit.append(FunctionID(node.first));
                if (node.kind != SummaryNode::Kind::Variable)
                    continue;
                placeOfFirstReader[node.first] = std::min(placeOfFirstReader[node.first], start.position);
                if (FunctionID declared = m_program.variables[node.first].function)
                    toVisit.append(declared);
            }
            for (const SummaryCall& call : summary.calls) {
                if (call.callee == SummaryCall::Callee::Binding && call.function)
                    toVisit.append(call.function);
            }
        }
    }

    BitVector found(m_program.variables.size());
    for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
        if (isLeftOut.quickGet(index))
            continue;
        for (SummaryPlaceInBody store : m_flow.summaries.at(m_program.modules[index].body).firstStoresOfBody) {
            if (store.position <= placeOfFirstReader[store.number])
                found.quickSet(store.number);
        }
    }
    return found;
}

void StateOfLinkReach::fillVariables()
{
    LinkType absent = m_flow.fromTypeID(TypeTable::absent);
    LinkType undefined = m_flow.fromTypeID(TypeTable::undefined);
    m_flow.variablesStoredFirst = findVariablesStoredFirst();
    const BitVector& isStoredFirst = m_flow.variablesStoredFirst;
    for (uint32_t index = 0; index < m_program.variables.size(); ++index) {
        const Variable& variable = m_program.variables[index];
        FlowNodeID node = m_flow.variable(VariableID(index));
        arriveWhatNoNodeBrings(node, variable.declared);
        if (variable.owner) {
            m_flow.arrive(node, absent);
            escapeVariable(VariableID(index));
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
            escapeVariable(VariableID(index));
    }
    std::ranges::sort(m_variablesOfLibraryFiles);

    for (const Module& module : m_program.modules) {
        if (module.exportsVariable)
            escapeVariable(module.exportsVariable);
        for (const SyntheticExport& synthetic : module.syntheticExports) {
            if (synthetic.variable)
                escapeVariable(synthetic.variable);
        }
    }
}

void StateOfLinkReach::letGoWhatDirectEvalsShowIn(FunctionID id)
{
    const BodyIndex* index = bodiesLookedAt(m_flow.looks).find(id);
    RELEASE_ASSERT(index);
    for (uint32_t block = 0; block < index->numberOfBlocks(); ++block) {
        for (BodyValue value : index->block(BlockID(block)).values()) {
            if (value.opcode() != Opcode::DirectEval)
                continue;
            for (const EvalBinding& binding : m_program.at(value.as<Opcode::DirectEval>().evalScope()).bindings) {
                if (binding.variable)
                    escapeVariable(binding.variable);
                if (binding.layout && binding.field)
                    escapeField({ binding.layout, *binding.field });
            }
        }
    }
}

void StateOfLinkReach::fillFields()
{
    for (uint32_t index = 0; index < m_numberOfLayouts; ++index) {
        const Layout& layout = m_program.layouts[index];
        for (uint32_t field = 0; field < layout.fields.size(); ++field)
            arriveWhatNoNodeBrings(m_flow.field({ LayoutID(index), field }), layout.fields[field].declared);
    }
}

void StateOfLinkReach::evaluate(ModuleID id)
{
    if (!m_evaluatedModules.set(id.index()))
        m_steps.append(QueuedStep { Step::EvaluateModule, id.index() });
}

void StateOfLinkReach::evaluateModule(ModuleID id)
{
    const Module& module = m_program.at(id);
    m_builtinsInReach.noteEvaluated(id);
    if (module.body)
        reach(module.body);
    else
        noteCodeOfModule(id);
    if (module.flags.contains(ModuleFlags::IsImportedAsNamespace))
        escapeExportsOf(id);
}

void StateOfLinkReach::noteCodeOfModule(ModuleID id)
{
    if (!id || m_modulesWithReachedCode.set(id.index()))
        return;
    for (auto [module, variable] : std::ranges::equal_range(m_variablesOfLibraryFiles, id, { }, &std::pair<ModuleID, VariableID>::first))
        escapeVariable(variable);
    bool hasBody = !!m_program.at(id).body;
    for (const ModuleRequest& request : m_program.at(id).requests) {
        if (request.kind == ModuleRequestKind::Require && hasBody)
            continue;
        evaluate(request.target);
        if (request.kind == ModuleRequestKind::Require)
            escapeExportsOf(request.target);
    }
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
        const Module& module = m_program.at(modules.takeLast());
        for (const ModuleExport& exported : module.exports) {
            if (exported.variable && !m_variablesReadByUnseenCode.set(exported.variable.index())) {
                m_flow.note(LinkFlow::Note::ExportIsShown, exported.variable.index());
                m_flow.addEdge(m_flow.variable(exported.variable), m_flow.unseen());
            }
            add(exported.module);
        }
        for (const ModuleRequest& request : module.requests) {
            if (request.isDynamicStarExport)
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
    if (!libraryRunsCompiledAlone)
        return;

    BitVector isLoadedByNativeCode;
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        for (const BuiltinModuleLoad& load : builtins.moduleLoadsOf(BuiltinID(index))) {
            if (load.module)
                isLoadedByNativeCode.set(load.module.index());
        }
    }

    static constexpr OptionSet<ModuleFlags> flagsOfModulesHandedOut {
        ModuleFlags::IsFoundByName, ModuleFlags::MayBeMocked, ModuleFlags::MayBeEvicted, ModuleFlags::RunsAgain, ModuleFlags::IsImportedAsNamespace, ModuleFlags::AnswersLoaderWithNamespace,
        ModuleFlags::IsListedInCache, ModuleFlags::IsShownByEntryOfCache, ModuleFlags::IsHandedOutByTestRunner, ModuleFlags::MayBeImportedWhileItRuns,
    };
    BitVector isAskedFor = modulesThatHostAsksFor(m_program);
    for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
        const Module& module = m_program.modules[index];
        if (module.kind != ModuleKind::RuntimeSource || !module.runtimeModule || module.flags.containsAny(flagsOfModulesHandedOut) || isAskedFor.get(index))
            continue;
        if (builtins.at(module.runtimeModule).flags.contains(RuntimeModuleFlag::IsPublic) || isLoadedByNativeCode.get(module.runtimeModule.index()))
            continue;
        m_modulesWhoseValueStaysInSight.quickSet(index);
    }
    for (const Module& module : m_program.modules) {
        for (const ModuleRequest& request : module.requests) {
            if (request.kind != ModuleRequestKind::Require)
                m_modulesWhoseValueStaysInSight.quickClear(request.target.index());
        }
        for (const RequiredName& required : module.requiredNames)
            m_modulesWhoseValueStaysInSight.quickClear(required.module.index());
    }
}

void StateOfLinkReach::escapeValueOf(ModuleID id)
{
    if (!m_modulesWhoseValueStaysInSight.quickClear(id.index()))
        return;
    FunctionID body = m_program.at(id).body;
    for (uint32_t result = 0; body && result < m_program.at(body).results.size(); ++result)
        m_flow.addEdge(m_flow.result(body, result), m_flow.unseen());
}

void StateOfLinkReach::escapeVariable(VariableID id)
{
    if (m_flow.escapedVariables.set(id.index()))
        return;
    FlowNodeID node = m_flow.variable(id);
    m_flow.arrive(node, LinkType::anything());
    m_flow.addEdge(node, m_flow.unseen());
}

void StateOfLinkReach::escapeField(FieldReference reference)
{
    FlowNodeID node = m_flow.field(reference);
    m_flow.addEdge(node, m_flow.unseen());
    m_flow.arrive(node, LinkType::anything().byEscapeAlone());
    m_flow.fieldsThatUnseenCodeMayWrite.set(cellOf(reference));
    m_flow.fieldsThatUnseenCodeMayWrite.set(cellOf(declarationOf(m_program, reference)));
}

bool StateOfLinkReach::mayHold(const BuiltinCondition* condition, BuiltinID canonical)
{
    if (!condition)
        return true;
    if (condition->buildAdmitsRuntimeCode)
        return false;
    if (condition->objectNobodyKnowsIsWritten && !m_builtinsInReach.mayBeHandedWhatIsNoTextNumberOrBoolean(canonical)) {
        if (!m_builtinsWhoseWriteIsAwaited.contains(canonical))
            m_builtinsWhoseWriteIsAwaited.append(canonical);
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
    bool isNativeCodeOfHost = entry.origin == BuiltinOrigin::Host && !entry.flags.contains(BuiltinFlag::IsCompilerHelper);
    for (const BuiltinOverload& overload : builtins.overloadsOf(canonical)) {
        if (isNativeCodeOfHost && overload.isUndeclared())
            enterWhatUndeclaredNativeCodeCalls();
    }
    for (const BuiltinCalledFunction& called : builtins.calledFunctionsOf(canonical)) {
        if (!mayHold(called.condition, canonical))
            continue;
        if (builtins.contains(called.function) && !builtins.at(called.function).platforms.contains(targetOfThisBuild().platform))
            continue;
        enterLibraryFunction(called.function);
        letGoWhatHostHandsOver(called.uses);
    }
    for (const BuiltinModuleLoad& load : builtins.moduleLoadsOf(canonical)) {
        if (!mayHold(load.condition, canonical))
            continue;
        loadLibraryModule(load.module);
        for (const BuiltinHostUses& uses : load.usesOfExports)
            letGoWhatHostHandsOver(uses);
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
    if (std::exchange(m_hasEnteredLoaderOfHost, true))
        return;
    for (RuntimeFunctionID function : m_program.builtins().moduleLoaderFunctions())
        enterLibraryFunction(function);
    for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
        const Module& module = m_program.modules[index];
        if (isProgramModuleOf(module.kind) && !module.flags.contains(ModuleFlags::IsLibrary))
            continue;
        evaluate(ModuleID(index));
        escapeExportsOf(ModuleID(index));
    }
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

bool StateOfLinkReach::enterLibraryInReach()
{
    RELEASE_ASSERT(m_steps.isEmpty() && m_functionsToBringAlive.isEmpty());
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
    LinkFlow::ClassesOfSites classes = m_flow.findClassesOfSitesThatHaveEscapedOrAreClimbedFrom();
    for (size_t builtinClass : classes.haveEscaped)
        m_builtinsInReach.noteInstanceAtUnseen(BuiltinClassID(static_cast<uint32_t>(builtinClass)));
    for (size_t builtinClass : classes.areClimbedFrom)
        m_builtinsInReach.noteLost(m_flow.prototypeOfInstancesOf(BuiltinClassID(static_cast<uint32_t>(builtinClass))));
    m_builtinsThatCameIntoReach.shrink(0);
    m_builtinsThatCameOutOfSight.shrink(0);
    m_builtinsInReach.settle(m_builtinsThatCameIntoReach, m_builtinsThatCameOutOfSight);
    m_membersWrittenAfterAll = m_builtinsInReach.takeMembersWrittenAfterAll();
    for (BuiltinID member : m_membersWrittenAfterAll)
        m_flow.arrive(m_flow.writtenAtMember(member), LinkType::anything());
    if (m_flow.testRuns().isTestRun()) {
        for (BuiltinID cameIntoReach : m_builtinsThatCameIntoReach)
            m_flow.testRuns().noteCameIntoReach(cameIntoReach);
        m_flow.testRuns().lookAtWhatHasSettled();
    }
    for (BuiltinID awaited : std::exchange(m_builtinsWhoseWriteIsAwaited, { })) {
        if (m_builtinsInReach.mayBeHandedWhatIsNoTextNumberOrBoolean(awaited))
            enterLibraryBehind(awaited);
        else
            m_builtinsWhoseWriteIsAwaited.append(awaited);
    }
    for (BuiltinID builtin : m_builtinsThatCameIntoReach)
        enterLibraryBehind(builtin);
    for (BuiltinID builtin : m_builtinsThatCameOutOfSight) {
        for (const BuiltinOverload& overload : m_program.builtins().overloadsOf(builtin))
            enterLibraryFunction(overload.code.implementation.function);
    }
    if (m_nativeCodeMayHoldObjects && m_builtinsInReach.objectsMayHaveMethodCalledWithStartupLoads())
        enterWhatHostLoadsToPrintObjects();
    if (m_flow.testRuns().isTestRun() && !std::exchange(m_hasEnteredLoaderFunctionsForTestRun, true)) {
        for (RuntimeFunctionID function : m_program.builtins().moduleLoaderFunctions())
            enterLibraryFunction(function);
        enterLibraryBehind(m_program.builtins().canonicalID(Builtins::ModuleRequireResolve));
    }
    if (m_builtinsInReach.loaderOfHostMayRun())
        enterLoaderOfHost();
    if (m_builtinsInReach.unseenCodeCanRun())
        enterWhatUnseenCodeFinds();
    bool somebodyCanBeHandedSomething = m_nativeCodeMayHoldObjects || m_flow.rowPassesWhatNobodyFollows() || m_builtinsInReach.unseenCodeCanRun();
    bool promisesHaveChanged = somebodyCanBeHandedSomething && m_flow.letPromisesBeHandedOut();
    bool hasEntered = !m_steps.isEmpty() || !m_functionsToBringAlive.isEmpty() || !m_membersWrittenAfterAll.isEmpty() || promisesHaveChanged;
    finish();
    return hasEntered;
}

void StateOfLinkReach::bringAlive(FunctionID id)
{
    const Function& function = m_program.at(id);
    const ThinSummary& summary = m_flow.summaries.at(id);
    prefetchRecordsOf(id, summary);
    LinkFlow::CodeAtWork codeAtWork(m_flow, id);
    LinkFlow::UnknownByEscapeAlone unknownByEscapeAlone(false);

    bool moduleIsKnown = !function.module || m_modulesWithReachedCode.get(function.module.index());
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
    if (summary.has(SummaryTrait::HasDirectEval)) {
        LinkFlow::LockerOfWhatIsShared locker(m_flow);
        letGoWhatDirectEvalsShowIn(id);
    }
    for (PropertyKeyID key : summary.textsThatNameEvents)
        m_flow.note(LinkFlow::Note::NameRead, key.bits(), true);
    for (const ThinSummary::AnswerOfRequire& answer : summary.answersOfRequire) {
        const Module& module = m_program.at(answer.module);
        if (module.requireResult == RequireResult::ModuleExports && module.exportsVariable)
            m_flow.addEdge(m_flow.variable(module.exportsVariable), m_flow.nodeOf(id, answer.node));
        else if (module.requireResult == RequireResult::DefaultExport && module.exportRule == SyntheticExportRule::RuntimeModule && module.body && !m_program.at(module.body).results.isEmpty())
            m_flow.addEdge(m_flow.result(module.body, 0), m_flow.nodeOf(id, answer.node));
    }
    {
        for (const ThinSummary::AnswerOfRequire& answer : summary.objectsOfCompiledModules) {
            const Module& module = m_program.at(answer.module);
            if (module.requireResult == RequireResult::DefaultExport && module.exportRule == SyntheticExportRule::RuntimeModule && module.body && !m_program.at(module.body).results.isEmpty())
                m_flow.addEdge(m_flow.result(module.body, 0), m_flow.nodeOf(id, answer.node));
        }
        for (const ThinSummary::NameThatLoaderTakes& name : summary.namesThatLoaderTakes)
            m_flow.addEdge(m_flow.nodeOf(id, name.read), m_flow.variable(name.variable));
    }
    if (m_functionsCalledByHost.get(id.index()) && !m_isCodeBehindBuiltin.get(id.index()))
        m_flow.escape(id);
    if (function.constructedClass && function.kind != FunctionKind::DerivedConstructor)
        m_flow.noteAllocated(function.constructedClass);
    m_flow.noteConstructionsOf(id);
    comeAliveForUnions(m_flow, id);
    fillEntryParameters(id, function);
    for (uint32_t result = 0; result < function.results.size(); ++result)
        arriveWhatNoNodeBrings(m_flow.result(id, result), function.results[result]);

    m_flow.addEdge(m_flow.thrown(id), m_flow.unseen());

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
                m_flow.takeAsContents(siteAt(id, classInstances.span(), node.first), SlotOfContents::fromNumber(node.second), m_flow.nodeOf(id, index));
            break;
        case SummaryNode::Kind::Constant:
            m_flow.arriveFromSource(m_flow.nodeOf(id, index), m_flow.fromTypeID(TypeID::fromBits(node.first)));
            break;
        case SummaryNode::Kind::Allocation: {
            LayoutID layout(node.first);
            FunctionID constructor = m_program.at(layout).constructor;
            if (!constructor || m_program.at(constructor).constructedClass != layout)
                m_flow.noteAllocated(layout);
            arriveFromSource(m_flow.nodeOf(id, index), typeOfObjectsForLinkReach(layout));
            break;
        }
        case SummaryNode::Kind::ClassInstance: {
            SiteID site = siteAt(id, classInstances.span(), index);
            m_flow.arrive(m_flow.nodeOf(id, index), m_flow.typeOf(site));
            fillStateThatNoSummaryFollows(site, !node.second);
            break;
        }
        case SummaryNode::Kind::Closure:
            m_flow.arrive(m_flow.nodeOf(id, index), typeOfClosuresForLinkReach(FunctionID(node.first)));
            break;
        case SummaryNode::Kind::Builtin:
            m_flow.arrive(m_flow.nodeOf(id, index), typeOfBuiltinForLinkReach(m_program, BuiltinID(node.first)));
            break;
        }
    }

    for (const SummaryEdge& edge : summary.edges)
        m_flow.addEdge(m_flow.nodeOf(id, edge.from), m_flow.nodeOf(id, edge.to), edge.passes);

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
        for (const StaticField& field : m_program.at(evaluated).staticFields) {
            m_flow.arrive(m_flow.variable(field.variable), m_flow.fromTypeID(TypeTable::absent));
            LinkFlow::LockerOfWhatIsShared locker(m_flow);
            escapeVariable(field.variable);
        }
    }

    for (uint32_t index = 0; index < summary.calls.size(); ++index) {
        const SummaryCall& call = summary.calls[index];
        switch (call.callee) {
        case SummaryCall::Callee::Binding:
            RELEASE_ASSERT(call.function);
            m_flow.enter(id, index, call.function);
            break;
        case SummaryCall::Callee::Name:
            watch(id, call.subject, FlowObserver::Kind::SubjectOfCall, index);
            watch(id, call.receiver, FlowObserver::Kind::ReceiverOfCall, index);
            break;
        case SummaryCall::Callee::Value:
            watch(id, call.subject, FlowObserver::Kind::SubjectOfCall, index);
            break;
        case SummaryCall::Callee::Builtin:
            watch(id, call.receiver, FlowObserver::Kind::ReceiverOfCall, index);
            m_flow.observe(m_flow.unseen(), FlowObserver { FlowObserver::Kind::SubjectOfCall, id, index });
            break;
        }
    }

    for (uint32_t index = 0; index < summary.accesses.size(); ++index) {
        const SummaryAccess& access = summary.accesses[index];
        watch(id, access.subject, FlowObserver::Kind::SubjectOfAccess, index);
        watch(id, access.keyNode, FlowObserver::Kind::KeyOfAccess, index);
    }

    for (uint32_t index = 0; index < summary.changes.size(); ++index) {
        const SummaryChange& change = summary.changes[index];
        watch(id, change.subject, FlowObserver::Kind::SubjectOfChange, index);
        watch(id, change.other, FlowObserver::Kind::OtherOfChange, index);
        watch(id, change.getterInSight, FlowObserver::Kind::OtherOfChange, index);
        watch(id, change.setterInSight, FlowObserver::Kind::OtherOfChange, index);
    }

    for (uint32_t index = 0; index < summary.operators.size(); ++index) {
        const SummaryOperator& operation = summary.operators[index];
        for (uint32_t operand = 0; operand < SummaryOperator::mostOperands; ++operand) {
            watch(id, operation.operands[operand], FlowObserver::Kind::OperandOfOperator, index);
            watch(id, operation.converted[operand], FlowObserver::Kind::OperandOfOperator, index);
        }
    }
}

void StateOfLinkReach::fillStateThatNoSummaryFollows(SiteID site, bool isObjectOfCoroutine)
{
    const BuiltinTable& builtins = m_program.builtins();
    for (BuiltinSlotID id : builtins.at(m_flow.classOf(site)).slots) {
        const BuiltinSlot& slot = builtins.at(id);
        if (slot.contentsSlot || (isObjectOfCoroutine && slot.holdsWhatStepsHandOut))
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
            if (!layoutsBelow.empty() && !onlyTaggedMembersLieBelow(m_program, function.constructedClass))
                m_flow.arrive(node, typeOfUnseenObjectsForLinkReach());
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
        m_flow.arriveFromSource(node, m_flow.fromTypeID(*established));
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
            type.unseenIsByEscapeAlone = !type.holdsUnseen;
            type.holdsUnseen = true;
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

void StateOfLinkReach::watch(FunctionID function, uint32_t nodeInSummary, FlowObserver::Kind kind, uint32_t record)
{
    if (nodeInSummary != noSummaryNode)
        m_flow.observe(m_flow.nodeOf(function, nodeInSummary), FlowObserver { kind, function, record });
}

void StateOfLinkReach::tieLostLayout(LayoutID id)
{
    if (m_flow.namesMayBeIncomplete(id, ClassSide::Prototype))
        escapeLayout(id);
}

void StateOfLinkReach::tieLostFunction(FunctionID id)
{
    reach(id);
    m_flow.tieNamesOnLostClosuresOf(id);
    if (m_lostFunctionsHaveEscaped || (m_program.at(id).constructedClass && !m_flow.layoutOfConstructorFunction(id)))
        escapeFunction(id);
}

void StateOfLinkReach::openLayout(LayoutID id)
{
    const Layout& layout = m_program.at(id);
    if (layout.kind == LayoutKind::ModuleNamespace) {
        if (layout.namespaceOf)
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
    m_flow.layoutsThatUnseenCodeMayChange.set(id.index());
    LinkType unseenObjects = typeOfUnseenObjectsForLinkReach().byEscapeAlone();
    for (uint32_t source = m_newestSourceOfLayout[id.index()]; source != none; source = m_sourcesOfObjects[source].next)
        m_flow.arrive(m_sourcesOfObjects[source].node, unseenObjects);

    openClassObject(id);
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
    if (LayoutID base = m_program.at(id).base)
        escapePrototype(base);
}

void StateOfLinkReach::openFunction(FunctionID id)
{
    const Function& function = m_program.at(id);
    LinkType anything = LinkType::anything();
    FlowNodeID unseen = m_flow.unseen();
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
            m_parametersThatUnseenCodePasses.append(m_flow.parameter(id, parameter));
            m_flow.arrive(m_flow.parameter(id, parameter), anything.byEscapeAlone());
            break;
        case EntryParameter::ArgumentCount:
        case EntryParameter::HiddenRest:
            m_flow.arrive(m_flow.parameter(id, parameter), anything);
            break;
        }
    }
    if (function.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest }))
        m_flow.arrive(m_flow.contents(m_flow.listOfArgumentsOf(id), ContentsSlot::Element), anything);
    ModuleID moduleWithBody = m_moduleWithBody[id.index()];
    bool resultStaysInSight = moduleWithBody && m_modulesWhoseValueStaysInSight.get(moduleWithBody.index());
    for (uint32_t result = 0; !resultStaysInSight && result < function.results.size(); ++result)
        m_flow.addEdge(m_flow.result(id, result), unseen);
    m_flow.addEdge(m_flow.thrown(id), unseen);
    m_flow.openClosuresOf(id);

    if (function.constructedClass)
        openClassObject(function.constructedClass);
}

void StateOfLinkReach::openClassObject(LayoutID classLayout)
{
    if (m_openClassObjects.set(classLayout.index()))
        return;
    m_flow.noteAllocated(classLayout);
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
        m_flow.arrive(node, LinkType::anything().byEscapeAlone());
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
    if (type.objects != LinkType::Objects::Layout && type.objects != LinkType::Objects::Family) {
        arrive(node, type);
        return;
    }
    LinkType arriving = type;
    if (arriving.objects == LinkType::Objects::Family)
        arriveFromSource(node, arriving.family);
    stateOfLinkReach().arriveFromSource(node, arriving);
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

bool LinkFlow::takeForConstant(BuiltinID member)
{
    return askedOnceByLane(MadeOnDemand::TakenForConstant, member.index(), [&]() -> uint32_t {
        return builtinsInReach().takeForConstant(member);
    });
}

BuiltinsInReach& LinkFlow::builtinsInReach()
{
    assertHasWhatIsSharedToItself();
    return stateOfLinkReach().builtinsInReach();
}

bool LinkFlow::enterLibraryInReach()
{
    return stateOfLinkReach().enterLibraryInReach();
}

bool StateOfLinkReach::fillParametersThatUnseenCodeAlonePasses()
{
    bool hasFilledSome = false;
    for (FlowNodeID parameter : std::exchange(m_parametersThatUnseenCodePasses, { })) {
        if (!m_flow.typeAt(parameter).asSeen().isNothing())
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
    state.queueEntries();
    holdBackWhile(scopedLambda<void()>([&] {
        state.finish();
    }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
