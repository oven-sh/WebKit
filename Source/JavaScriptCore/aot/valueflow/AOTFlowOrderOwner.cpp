/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowOrderInternal.h"

#if ENABLE(AOT)

#include "AOTFlowNamespaces.h"
#include <algorithm>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OperandUses usesOfForwardedOperand { OperandUse::Inspects, OperandUse::Returns, OperandUse::DefinesName };

static bool isForwardedOperand(const OperandInfo& operand)
{
    return operand.uses.contains(OperandUse::Returns) && operand.uses.containsOnly(usesOfForwardedOperand) && operand.protocols.isEmpty();
}

static ValueID forwardedSourceOf(const Function& function, ValueID value)
{
    while (true) {
        const Value& operation = function.at(value);
        Vector<ValueID, 1> sources;
        operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (isForwardedOperand(operandInfoOf(operation.opcode, operandIndex)))
                sources.append(operand);
        });
        if (sources.size() != 1)
            return value;
        value = sources[0];
    }
}

FlowScopeFrame::FlowScopeFrame(const FlowScopeIndex& index, FunctionID function, FlowScopeFrame* caller, ValueID call)
    : function(function)
    , body(index.program().at(function))
    , analyses(AnalysisContext { index.inputs.build(), index.program(), nullptr, nullptr, function }, body)
    , caller(caller)
    , call(call)
    , depth(caller ? caller->depth + 1 : 0)
{
}

FlowScopeOwnerAnalysis::FlowScopeOwnerAnalysis(const FlowScopeIndex& index, const FlowScopeOwner& owner)
    : m_index(index)
    , m_owner(owner)
{
    createFrames();
}

void FlowScopeOwnerAnalysis::appendFrame(FunctionID function, FlowScopeFrame* caller, ValueID call)
{
    m_frames.append(makeUnique<FlowScopeFrame>(m_index, function, caller, call));
    m_framesByFunction.add(function, m_frames.last().get());
}

void FlowScopeOwnerAnalysis::createFrames()
{
    appendFrame(m_owner.function, nullptr, ValueID());
    for (size_t index = 0; index < m_frames.size(); ++index) {
        FlowScopeFrame* caller = m_frames[index].get();
        for (const FlowFunctionMention& mention : m_index.bodies[caller->function.index()]->references) {
            bool runsOnlyDuringCall = m_index.calls.soleCallSites[mention.function.index()] == FlowCallSite { caller->function, mention.value };
            if (runsOnlyDuringCall && pointOf(*caller, mention.value))
                appendFrame(mention.function, caller, mention.value);
        }
    }
}

FlowScopeFrame* FlowScopeOwnerAnalysis::findFrame(FunctionID function)
{
    return m_framesByFunction.get(function);
}

FlowScopeFrame& FlowScopeOwnerAnalysis::frameOf(const FlowScopeEntry& entry)
{
    RELEASE_ASSERT(entry.state == FlowScopeEntryState::AfterPoint);
    FlowScopeFrame* frame = findFrame(entry.function);
    RELEASE_ASSERT(frame);
    return *frame;
}

// A handler edge can leave the common dominator before any of its values, so only a block whose terminator dominates both points has run to its end.
ProgramPoint FlowScopeOwnerAnalysis::meet(FlowScopeFrame& frame, ProgramPoint first, ProgramPoint second)
{
    if (first.block == second.block)
        return first.index <= second.index ? first : second;
    Dominators& dominators = frame.analyses.dominators();
    BlockID common = dominators.nearestCommonDominator(first.block, second.block);
    auto isReachedThroughTerminator = [&](BlockID block) {
        return block == common || dominators.terminatorDominates(common, block);
    };
    if (!isReachedThroughTerminator(first.block) || !isReachedThroughTerminator(second.block))
        return ProgramPoint::atHeadOf(common);
    if (common == first.block)
        return first;
    if (common == second.block)
        return second;
    return pointBeforeTerminatorOf(frame.body, common);
}

FlowScopeEntry FlowScopeOwnerAnalysis::entryInCaller(const FlowScopeEntry& entry)
{
    FlowScopeFrame& frame = frameOf(entry);
    return entryAt(*frame.caller, frame.call);
}

// Every run that passes the later point has passed the earlier one. A point in a frame that has returned was passed when it lies on every way to a return.
bool FlowScopeOwnerAnalysis::isPassedBefore(const FlowScopeEntry& earlier, const FlowScopeEntry& later)
{
    FlowScopeFrame& frame = frameOf(earlier);
    FlowScopeEntry reached = later;
    while (frameOf(reached).depth > frame.depth)
        reached = entryInCaller(reached);
    if (frame.function == reached.function)
        return meet(frame, earlier.point, reached.point) == earlier.point;

    FlowScopeEntry exit = exitOf(frame);
    bool isPassedBeforeReturn = exit.state == FlowScopeEntryState::Never || meet(frame, earlier.point, exit.point) == earlier.point;
    return isPassedBeforeReturn && precedes({ 0, frame.caller->function, frame.call }, later);
}

FlowScopeEntry FlowScopeOwnerAnalysis::meet(const FlowScopeEntry& first, const FlowScopeEntry& second)
{
    if (first.state == FlowScopeEntryState::Unknown || second.state == FlowScopeEntryState::Unknown)
        return unknownScopeEntry;
    if (first.state == FlowScopeEntryState::Never)
        return second;
    if (second.state == FlowScopeEntryState::Never)
        return first;
    if (first.function == second.function)
        return scopeEntryAfter(first.function, meet(frameOf(first), first.point, second.point));
    if (isPassedBefore(first, second))
        return first;
    if (isPassedBefore(second, first))
        return second;

    unsigned firstDepth = frameOf(first).depth;
    unsigned secondDepth = frameOf(second).depth;
    return meet(firstDepth >= secondDepth ? entryInCaller(first) : first, secondDepth >= firstDepth ? entryInCaller(second) : second);
}

// Both entries are bounds of one event. Either answer is right, the later one says more.
FlowScopeEntry FlowScopeOwnerAnalysis::laterOf(const FlowScopeEntry& first, const FlowScopeEntry& second)
{
    if (first.state == FlowScopeEntryState::Never || second.state == FlowScopeEntryState::Never)
        return neverScopeEntry;
    if (first.state == FlowScopeEntryState::Unknown)
        return second;
    if (second.state == FlowScopeEntryState::Unknown)
        return first;
    return isPassedBefore(second, first) ? first : second;
}

FlowScopeEntry& FlowScopeOwnerAnalysis::ensureEntry(FunctionID function)
{
    return m_entries.add(function, neverScopeEntry).iterator->value;
}

void FlowScopeOwnerAnalysis::lower(FunctionID function, FlowScopeEntry entry)
{
    FlowScopeEntry current = ensureEntry(function);
    FlowScopeEntry lowered = meet(current, entry);
    if (lowered == current)
        return;
    ensureEntry(function) = lowered;
    m_queue.append(function);
}

std::optional<ProgramPoint> FlowScopeOwnerAnalysis::pointOf(FlowScopeFrame& frame, ValueID value)
{
    ValueLocation location = frame.analyses.useLists().locationOf(value);
    if (location.placement != ValuePlacement::InBlock)
        return std::nullopt;
    return ProgramPoint { location.block, location.index };
}

FlowScopeEntry FlowScopeOwnerAnalysis::entryAt(FlowScopeFrame& frame, ValueID value)
{
    auto point = pointOf(frame, value);
    return point ? scopeEntryAfter(frame.function, *point) : unknownScopeEntry;
}

FlowScopeEntry FlowScopeOwnerAnalysis::entryOf(FunctionID function)
{
    auto entry = m_entries.find(function);
    return entry == m_entries.end() ? unknownScopeEntry : entry->value;
}

FlowScopeEntry FlowScopeOwnerAnalysis::entryByUseOf(FunctionID function)
{
    auto entry = m_entriesByUse.find(function);
    return entry == m_entriesByUse.end() ? unknownScopeEntry : entry->value;
}

FlowScopeEntry FlowScopeOwnerAnalysis::entryByCallsOf(FunctionID function)
{
    if (m_index.calls.functionsEnteredByHost.get(function.index()))
        return unknownScopeEntry;
    FlowScopeEntry entry = neverScopeEntry;
    for (const FlowScopeCall& call : recordsWith(m_index.calls.byCallee.span(), function, &FlowScopeCall::callee)) {
        FlowScopeFrame* frame = findFrame(call.caller);
        entry = meet(entry, frame ? entryAt(*frame, call.site) : laterOf(entryOf(call.caller), call.subject ? entryByStoresTo(call.subject) : unknownScopeEntry));
        // Nothing lies below: a function of the library has thousands of callers, and the first that has nothing to do with this owner decides.
        if (entry.state == FlowScopeEntryState::Unknown)
            break;
    }
    return entry;
}

FlowScopeEntry FlowScopeOwnerAnalysis::entryByStoresTo(VariableID id)
{
    auto known = m_entriesByStores.find(id);
    if (known != m_entriesByStores.end())
        return known->value;
    FlowScopeEntry entry = computeEntryByStoresTo(id);
    m_entriesByStores.add(id, entry);
    return entry;
}

FlowScopeEntry FlowScopeOwnerAnalysis::computeEntryByStoresTo(VariableID id)
{
    FlowScopeLocation location = id.index();
    const Variable& variable = m_index.program().at(id);
    bool isStaticField = !!variable.owner;
    if (isStaticField || variable.initialization == VariableInitialization::Function)
        return unknownScopeEntry;
    if (m_index.locationOwners[location] != m_owner.function || m_index.opaqueLocations.get(location))
        return unknownScopeEntry;

    FlowScopeEntry entry = neverScopeEntry;
    for (const FlowScopeLocationAccess& store : accessesAt(m_index.stores.span(), location)) {
        FlowScopeFrame* frame = findFrame(store.function);
        entry = meet(entry, frame ? entryAt(*frame, store.value) : unknownScopeEntry);
    }
    return entry;
}

static std::optional<uint32_t> subjectOperandOf(Opcode opcode)
{
    if (opcode == Opcode::Call)
        return OperandIndex::Call::callee;
    if (opcode == Opcode::Construct)
        return OperandIndex::Construct::callee;
    return receiverOf(opcode);
}

VariableID variableBehindSubjectOf(const Function& function, ValueID operation)
{
    std::optional<uint32_t> subject = subjectOperandOf(function.at(operation).opcode);
    while (subject) {
        operation = forwardedSourceOf(function, function.at(operation).operand(*subject));
        Opcode opcode = function.at(operation).opcode;
        if (opcode == Opcode::GetModuleVariable)
            return function.at(operation).variable();
        subject = accessOf(opcode) == Access::Read ? receiverOf(opcode) : std::nullopt;
    }
    return { };
}

FlowScopeEntry FlowScopeOwnerAnalysis::entryBySubjectOf(FunctionID function, ValueID operation)
{
    VariableID variable = variableBehindSubjectOf(m_index.program().at(function), operation);
    return variable ? entryByStoresTo(variable) : unknownScopeEntry;
}

// A call in the body may enter a closure over an environment of an earlier activation or iteration, whose own store has not run. A variable is one location.
FlowScopeEntry FlowScopeOwnerAnalysis::entryOf(const FlowScopeLocationAccess& access)
{
    if (FlowScopeFrame* frame = findFrame(access.function))
        return entryAt(*frame, access.value);
    return m_index.numbering.isOneLocation(access.location) ? entryOf(access.function) : entryByUseOf(access.function);
}

FlowScopeEntry FlowScopeOwnerAnalysis::computeExit(FlowScopeFrame& frame)
{
    FlowScopeEntry exit = neverScopeEntry;
    for (BlockID block : frame.analyses.cfg().reversePostOrder()) {
        if (returnsToCaller(frame.body.at(block).terminator.kind))
            exit = meet(exit, scopeEntryAfter(frame.function, pointBeforeTerminatorOf(frame.body, block)));
    }
    return exit;
}

FlowScopeEntry FlowScopeOwnerAnalysis::exitOf(FlowScopeFrame& frame)
{
    if (!frame.exit)
        frame.exit = computeExit(frame);
    return *frame.exit;
}

std::span<const FlowExportedVariable> FlowScopeOwnerAnalysis::exportsOf(FlowScopeLocation location) const
{
    return recordsWith(m_owner.exports.span(), location, [](const FlowExportedVariable& exported) {
        return exported.variable.index();
    });
}

bool FlowScopeOwnerAnalysis::isExported(FlowScopeLocation location) const
{
    return !exportsOf(location).empty();
}

std::optional<ValueID> FlowScopeOwnerAnalysis::forwardedValue(const FlowScopeFrame& frame, const Use& use) const
{
    if (use.kind != Use::Kind::Operand)
        return std::nullopt;
    if (!isForwardedOperand(operandInfoOf(frame.body.at(use.user).opcode, use.operandIndex)))
        return std::nullopt;
    return use.user;
}

auto FlowScopeOwnerAnalysis::parameterReceiving(FlowScopeFrame& frame, const Use& use) -> std::optional<FollowedValue>
{
    if (use.kind != Use::Kind::Operand)
        return std::nullopt;
    const Value& user = frame.body.at(use.user);
    if (user.opcode != Opcode::CallDirect)
        return std::nullopt;
    FlowScopeFrame* callee = findFrame(user.function());
    if (!callee || callee->caller != &frame || callee->call != use.user)
        return std::nullopt;
    auto argumentIndex = user.variadicOperandIndex(use.operandIndex);
    const auto& parameters = callee->body.at(entryBlock).parameters;
    if (!argumentIndex || *argumentIndex >= parameters.size())
        return std::nullopt;
    return FollowedValue { callee, parameters[*argumentIndex] };
}

// The definition of an own property of a function object runs no code and hands the object to nobody.
bool FlowScopeOwnerAnalysis::isReceiverOfDefinition(const FlowScopeFrame& frame, const Use& use) const
{
    return use.kind == Use::Kind::Operand && use.operandIndex == OperandIndex::PropDefine::object && frame.body.at(use.user).opcode == Opcode::PropDefine;
}

void FlowScopeOwnerAnalysis::recordClassEvaluation(FlowScopeFrame& creator, ValueID closure, FunctionID constructor)
{
    LayoutID constructedClass = m_index.program().at(constructor).constructedClass;
    if (constructedClass && m_index.calls.numberOfMentions[constructor.index()] == 1 && !canRunTwice({ 0, creator.function, closure }))
        m_classesEvaluatedOnce.set(constructedClass.index());
}

void FlowScopeOwnerAnalysis::recordDefinition(const FlowScopeFrame& frame, ValueID definition, FunctionID receiver)
{
    VariableID staticField = findStaticFieldVariable(m_index.program(), receiver, frame.body.at(definition).key());
    if (staticField && m_index.locationOwners[staticField.index()] == m_owner.function)
        m_definitions.append({ staticField.index(), frame.function, definition });
}

std::optional<FlowScopeLocation> FlowScopeOwnerAnalysis::storedLocation(const FlowScopeFrame& frame, const Use& use) const
{
    if (use.kind != Use::Kind::Operand)
        return std::nullopt;
    const Value& user = frame.body.at(use.user);
    const FlowScopeLocationNumbering& numbering = m_index.numbering;
    if (user.opcode == Opcode::PutModuleVariable && use.operandIndex == OperandIndex::PutModuleVariable::value)
        return numbering.locationOf(user.variable());
    if (user.opcode == Opcode::PutEnvironmentField && use.operandIndex == OperandIndex::PutEnvironmentField::value)
        return numbering.locationOf(user.layout(), user.field());
    if (user.opcode == Opcode::NewObject && m_index.program().at(user.layout()).kind == LayoutKind::Environment) {
        if (auto listedField = user.variadicOperandIndex(use.operandIndex))
            return numbering.locationOf(user.layout(), user.fields()[*listedField]);
    }
    return std::nullopt;
}

void FlowScopeOwnerAnalysis::recordStoredFunction(FlowScopeLocation location, FunctionID function, const FlowScopeEntry& store)
{
    if (m_index.opaqueLocations.get(location)) {
        lower(function, unknownScopeEntry);
        return;
    }
    ensureEntry(function);
    m_storedFunctions.append({ location, function, store });
}

void FlowScopeOwnerAnalysis::recordClosureUse(FlowScopeFrame& frame, const Use& use, FunctionID function, StoredClosureEntry storedClosureEntry)
{
    auto location = storedLocation(frame, use);
    bool isOpaque = location && m_index.opaqueLocations.get(*location);
    bool isEnteredAtRead = location && storedClosureEntry == StoredClosureEntry::AtRead && m_index.locationOwners[*location] == m_owner.function;
    FlowScopeEntry entryAtUse = scopeEntryAfter(frame.function, frame.analyses.useLists().pointOf(use));
    if (isOpaque || isEnteredAtRead) {
        recordStoredFunction(*location, function, entryAtUse);
        return;
    }
    lower(function, entryAtUse);
}

void FlowScopeOwnerAnalysis::collectPerIterationAllocations()
{
    for (LayoutID layout : m_owner.environments) {
        for (const FlowEnvironmentAllocation& site : allocationsOf(m_index, layout)) {
            auto point = pointOf(ownerFrame(), site.value);
            if (point && ownerFrame().analyses.blockCycles().isInCycle(point->block))
                m_perIterationAllocations.append(site.value);
        }
    }
}

// A location can hold a closure over an earlier environment of a cycle, and a store that dominates the read of the location has run only for the latest one.
auto FlowScopeOwnerAnalysis::storedClosureEntryOf(FlowScopeFrame& frame, ValueID closure) -> StoredClosureEntry
{
    FlowScopeEntry creation = entryAt(frame, closure);
    while (creation.state == FlowScopeEntryState::AfterPoint && creation.function != m_owner.function)
        creation = entryInCaller(creation);
    bool canCapturePerIterationEnvironment = creation.state != FlowScopeEntryState::AfterPoint || std::ranges::any_of(m_perIterationAllocations, [&](ValueID allocation) {
        return ownerFrame().analyses.dominates(allocation, creation.point);
    });
    return canCapturePerIterationEnvironment ? StoredClosureEntry::AtStore : StoredClosureEntry::AtRead;
}

static bool isOperandOfMethodDefinition(const Function& function, const Use& use, uint32_t operandIndex)
{
    return use.kind == Use::Kind::Operand && use.operandIndex == operandIndex && function.at(use.user).opcode == Opcode::DefineMethod;
}

// The closure that the value is, when each operation between them hands its operand on. Every other name of the object then comes from a use of that closure.
static std::optional<ValueID> findForwardedClosure(const Function& function, ValueID value)
{
    ValueID source = forwardedSourceOf(function, value);
    if (function.at(source).opcode != Opcode::NewClosure)
        return std::nullopt;
    return source;
}

void FlowScopeOwnerAnalysis::followClosure(FlowScopeFrame& creator, ValueID closure, FunctionID function)
{
    StoredClosureEntry storedClosureEntry = storedClosureEntryOf(creator, closure);
    recordClassEvaluation(creator, closure, function);
    ensureEntry(function);
    Vector<FollowedValue, 4> worklist;
    worklist.append({ &creator, closure });
    while (!worklist.isEmpty()) {
        FollowedValue followed = worklist.takeLast();
        FlowScopeFrame& frame = *followed.frame;
        for (const Use& use : frame.analyses.useLists().usesOf(followed.value)) {
            if (auto forwarded = forwardedValue(frame, use)) {
                worklist.append({ &frame, *forwarded });
                continue;
            }
            if (auto parameter = parameterReceiving(frame, use)) {
                worklist.append(*parameter);
                continue;
            }
            if (isReceiverOfDefinition(frame, use)) {
                recordDefinition(frame, use.user, function);
                continue;
            }
            // The definition of a method runs no code, and the method can be entered only through the class object that holds it.
            if (isOperandOfMethodDefinition(frame.body, use, OperandIndex::DefineMethod::classObject))
                continue;
            if (isOperandOfMethodDefinition(frame.body, use, OperandIndex::DefineMethod::function)) {
                if (auto constructor = findForwardedClosure(frame.body, frame.body.at(use.user).operand(OperandIndex::DefineMethod::classObject))) {
                    worklist.append({ &frame, *constructor });
                    continue;
                }
            }
            recordClosureUse(frame, use, function, storedClosureEntry);
        }
    }
}

void FlowScopeOwnerAnalysis::recordOwnReferences()
{
    for (auto& frame : m_frames) {
        for (const FlowFunctionMention& reference : m_index.bodies[frame->function.index()]->references) {
            switch (reference.kind) {
            case FunctionReferenceKind::Closure:
                followClosure(*frame, reference.value, reference.function);
                break;
            case FunctionReferenceKind::Call:
            case FunctionReferenceKind::Constructor:
            case FunctionReferenceKind::Dispatch:
                if (!findFrame(reference.function))
                    lower(reference.function, entryAt(*frame, reference.value));
                break;
            }
        }
    }
}

void FlowScopeOwnerAnalysis::recordInitialFunctions()
{
    for (VariableID id : m_owner.variables) {
        const Variable& variable = m_index.program().at(id);
        if (variable.initialization == VariableInitialization::Function && variable.function)
            recordStoredFunction(id.index(), variable.function, unknownScopeEntry);
    }
}

void FlowScopeOwnerAnalysis::recordOutsideReader(FunctionID reader)
{
    if (findFrame(reader))
        return;
    const Function& function = m_index.program().at(reader);
    if (!function.module || m_index.functionsEnteredWithoutMention.quickGet(reader.index())) {
        lower(reader, unknownScopeEntry);
        return;
    }
    if (function.module == m_owner.module)
        return;
    bool isAfterExit = m_index.moduleOrder.orderingOf(m_owner.module, function.module) == FlowModuleOrdering::CompletesBefore;
    lower(reader, isAfterExit ? m_exit : unknownScopeEntry);
}

void FlowScopeOwnerAnalysis::recordOutsideReaders()
{
    for (VariableID variable : m_owner.variables) {
        for (const FlowScopeLocationAccess& read : accessesAt(m_index.reads.span(), variable.index()))
            recordOutsideReader(read.function);
        for (const FlowScopeLocationAccess& access : accessesAt(m_index.accessesThroughClassObjects.span(), variable.index()))
            recordOutsideReader(access.function);
    }
    for (FlowScopeLocation member : m_owner.members) {
        for (const FlowScopeLocationAccess& read : accessesAt(m_index.reads.span(), member))
            recordOutsideReader(read.function);
    }
    for (ModuleID exporter : m_owner.exporters) {
        for (const FlowModuleValueRead& read : valueReadsOf(m_index, exporter))
            recordOutsideReader(read.function);
    }
}

void FlowScopeOwnerAnalysis::recordReadsOfStoredFunctions()
{
    std::ranges::sort(m_storedFunctions, { }, &StoredFunction::location);
    m_functionsTakenOut = m_storedFunctions;
    auto recordReader = [&](const FlowScopeLocationAccess& read, uint32_t first, uint32_t count) {
        FlowScopeFrame* frame = findFrame(read.function);
        if (!frame) {
            m_storedFunctionReaders.append({ read.function, first, count });
            return;
        }
        FlowScopeEntry entry = entryAt(*frame, read.value);
        for (uint32_t index = first; index < first + count; ++index)
            lower(m_functionsTakenOut[index].function, laterOf(m_functionsTakenOut[index].store, entry));
    };

    // A library's module exports hundreds of functions and is required in thousands of places. So the readers of its value are listed once for the module, not once for each variable.
    struct Run {
        ModuleID exporter;
        uint32_t first;
        uint32_t count;
    };
    Vector<Run> runsOfExporters;
    for (uint32_t first = 0; first < m_storedFunctions.size();) {
        FlowScopeLocation location = m_storedFunctions[first].location;
        uint32_t count = recordsWith(m_storedFunctions.span().subspan(first), location, &StoredFunction::location).size();
        for (const FlowScopeLocationAccess& read : accessesAt(m_index.reads.span(), location))
            recordReader(read, first, count);
        for (const FlowExportedVariable& exported : exportsOf(location))
            runsOfExporters.append({ exported.exporter, first, count });
        for (const FlowScopeLocationAccess& access : accessesAt(m_index.accessesThroughClassObjects.span(), location))
            recordReader(access, first, count);
        if (isExported(location)) {
            for (uint32_t index = first; index < first + count; ++index)
                lower(m_storedFunctions[index].function, laterOf(m_storedFunctions[index].store, m_hostEntry));
        }
        first += count;
    }
    std::ranges::stable_sort(runsOfExporters, { }, &Run::exporter);
    for (size_t position = 0; position < runsOfExporters.size();) {
        ModuleID exporter = runsOfExporters[position].exporter;
        uint32_t first = m_functionsTakenOut.size();
        for (; position < runsOfExporters.size() && runsOfExporters[position].exporter == exporter; ++position)
            m_functionsTakenOut.append(m_storedFunctions.subspan(runsOfExporters[position].first, runsOfExporters[position].count));
        uint32_t count = m_functionsTakenOut.size() - first;
        for (const FlowModuleValueRead& read : valueReadsOf(m_index, exporter))
            recordReader(FlowScopeLocationAccess { m_functionsTakenOut[first].location, read.function, read.value }, first, count);
    }
    std::ranges::sort(m_storedFunctionReaders, { }, [](const StoredFunctionReader& read) {
        return std::tuple(read.reader, read.first);
    });
}

void FlowScopeOwnerAnalysis::runFixpoint()
{
    while (!m_queue.isEmpty()) {
        FunctionID function = m_queue.takeLast();
        FlowScopeEntry entry = ensureEntry(function);
        const auto& body = m_index.bodies[function.index()];
        if (!body)
            continue;
        for (const FlowFunctionMention& reference : body->references)
            lower(reference.function, entry);
        constexpr size_t mostEntriesToRemember = 8;
        for (const StoredFunctionReader& read : recordsWith(m_storedFunctionReaders.span(), function, &StoredFunctionReader::reader)) {
            Vector<FlowScopeEntry, 2>& entriesOfReaders = m_entriesOfReadersOfRuns.add(read.first, Vector<FlowScopeEntry, 2>()).iterator->value;
            if (entriesOfReaders.contains(entry))
                continue;
            if (entriesOfReaders.size() < mostEntriesToRemember)
                entriesOfReaders.append(entry);
            for (const StoredFunction& stored : m_functionsTakenOut.subspan(read.first, read.count))
                lower(stored.function, laterOf(stored.store, entry));
        }
    }
}

// A function is entered only by a call that flow has linked, after the caller was entered. The entries so far say when somebody can first reach the function.
void FlowScopeOwnerAnalysis::refineEntriesByCalls()
{
    m_entriesByUse = m_entries;
    if (m_owner.variables.isEmpty() && m_owner.members.isEmpty())
        return;
    for (auto& entry : m_entries) {
        entry.value = neverScopeEntry;
        m_queue.append(entry.key);
    }
    // First come, first served, and nobody waits twice. A helper that a thousand functions call and that calls a thousand (the wrapper of a bundler's modules) looks at all its callers each time it is
    // looked at: where the last to come was served first, it was looked at again behind each of its callees, each time that its own entry moved: the cube of the number of modules.
    std::ranges::sort(m_queue, std::greater { });
    HashSet<FunctionID> waiting;
    for (FunctionID function : m_queue)
        waiting.add(function);
    for (size_t next = 0; next < m_queue.size(); ++next) {
        FunctionID function = m_queue[next];
        waiting.remove(function);
        FlowScopeEntry current = entryOf(function);
        if (current == unknownScopeEntry)
            continue;
        FlowScopeEntry refined = meet(current, laterOf(entryByUseOf(function), entryByCallsOf(function)));
        if (refined == current)
            continue;
        m_entries.set(function, refined);
        for (const FlowScopeCall& call : recordsWith(m_index.calls.byCaller.span(), function, &FlowScopeCall::caller)) {
            auto callee = m_entries.find(call.callee);
            if (callee != m_entries.end() && callee->value != unknownScopeEntry && waiting.add(call.callee).isNewEntry)
                m_queue.append(call.callee);
        }
    }
    m_queue.shrink(0);
}

void FlowScopeOwnerAnalysis::computeEntries()
{
    if (m_owner.module) {
        m_exit = exitOf(ownerFrame());
        bool isReadByHostAtAnyTime = m_index.modulesLoadedByName.get(m_owner.module.index()) || std::ranges::any_of(m_owner.exporters, [&](ModuleID exporter) {
            return m_index.modulesReadByHost.get(exporter.index()) || m_index.modulesLoadedByName.get(exporter.index());
        });
        if (m_index.moduleOrder.runsAfterRequests(m_owner.module) && !isReadByHostAtAnyTime)
            m_hostEntry = m_exit;
    }
    collectPerIterationAllocations();
    recordOwnReferences();
    recordInitialFunctions();
    if (m_owner.module)
        recordOutsideReaders();
    recordReadsOfStoredFunctions();
    runFixpoint();
    refineEntriesByCalls();
    std::ranges::sort(m_definitions);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
