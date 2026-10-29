/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowOrderInternal.h"

#if ENABLE(AOT)

#include "AOTFlowNamespaces.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <memory>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

enum class FlowScopeEntryState : uint8_t { Unknown, AfterPoint, Never };

struct FlowScopeEntry {
    FlowScopeEntryState state { FlowScopeEntryState::Unknown };
    FunctionID function;
    ProgramPoint point;

    friend bool operator==(const FlowScopeEntry&, const FlowScopeEntry&) = default;
};

static constexpr FlowScopeEntry unknownScopeEntry { FlowScopeEntryState::Unknown, { }, { } };
static constexpr FlowScopeEntry neverScopeEntry { FlowScopeEntryState::Never, { }, { } };
static_assert(FlowScopeEntry { } == unknownScopeEntry, "a map answers with the unknown entry for a function that it does not hold");

static constexpr FlowScopeEntry scopeEntryAfter(FunctionID function, ProgramPoint point) { return { FlowScopeEntryState::AfterPoint, function, point }; }

static bool isForwardedOperand(const OperandInfo& operand)
{
    constexpr OperandUses usesOfForwardedOperand { OperandUse::Inspects, OperandUse::Returns, OperandUse::DefinesName };
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

struct FlowScopeFrame {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FlowScopeFrame);

    FlowScopeFrame(const FlowScopeIndex& index, FunctionID function, FlowScopeFrame* caller, ValueID call)
        : function(function)
        , body(index.program().at(function))
        , analyses(AnalysisContext { index.inputs.build, index.program(), nullptr, nullptr, function }, body)
        , caller(caller)
        , call(call)
        , depth(caller ? caller->depth + 1 : 0)
    {
    }

    FunctionID function;
    const Function& body;
    FunctionAnalyses analyses;
    FlowScopeFrame* caller;
    ValueID call;
    unsigned depth;
    std::optional<FlowScopeEntry> exit;
};

class FlowScopeOwnerAnalysis {
    WTF_MAKE_NONCOPYABLE(FlowScopeOwnerAnalysis);
public:
    FlowScopeOwnerAnalysis(const FlowScopeIndex& index, const FlowScopeOwner& owner)
        : m_index(index)
        , m_owner(owner)
    {
        auto appendFrame = [&](FunctionID function, FlowScopeFrame* caller, ValueID call) {
            m_frames.append(makeUnique<FlowScopeFrame>(m_index, function, caller, call));
            m_framesByFunction.add(function, m_frames.last().get());
        };
        appendFrame(m_owner.function, nullptr, ValueID());
        for (size_t next = 0; next < m_frames.size(); ++next) {
            FlowScopeFrame* caller = m_frames[next].get();
            for (const FlowFunctionMention& mention : m_index.mentionsIn(caller->function)) {
                FlowCallSite site = m_index.calls.soleCallSites[mention.function.index()];
                bool runsOnlyDuringCall = site.second && site.first == caller->function && (site.second == mention.value || mention.kind == FunctionReferenceKind::Closure);
                if (runsOnlyDuringCall && pointOf(*caller, site.second))
                    appendFrame(mention.function, caller, site.second);
            }
        }
    }

    void computeEntries()
    {
        FlowScopeEntry exit = neverScopeEntry;
        if (m_owner.module) {
            exit = exitOf(ownerFrame());
            bool isReadByHostAtAnyTime = m_index.inputs.modulesLoadedByName.get(m_owner.module.index()) || std::ranges::any_of(m_owner.exporters, [&](ModuleID exporter) {
                return m_index.inputs.modulesLoadedByHost.get(exporter.index()) || m_index.inputs.modulesLoadedByName.get(exporter.index());
            });
            if (m_index.moduleOrder.runsAfterRequests(m_owner.module) && !isReadByHostAtAnyTime)
                m_hostEntry = exit;
        }
        for (LayoutID layout : m_owner.environments) {
            for (const FlowEnvironmentAllocation& site : operationsOn(m_index.allocations, layout)) {
                auto point = pointOf(ownerFrame(), site.value);
                if (point && ownerFrame().analyses.blockCycles().isInCycle(point->block))
                    m_perIterationAllocations.append(site.value);
            }
        }
        for (auto& frame : m_frames) {
            for (const FlowFunctionMention& reference : m_index.mentionsIn(frame->function)) {
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
        for (VariableID id : m_owner.variables) {
            const Variable& variable = m_index.program().at(id);
            if (variable.initialization == VariableInitialization::Function && variable.function)
                recordStoredFunction(id.index(), variable.function, unknownScopeEntry);
        }
        if (m_owner.module) {
            auto recordOutsideReader = [&](FunctionID reader) {
                if (findFrame(reader))
                    return;
                const Function& function = m_index.program().at(reader);
                if (!function.module || m_index.functionsEnteredWithoutMention.quickGet(reader.index())) {
                    lower(reader, unknownScopeEntry);
                    return;
                }
                if (function.module != m_owner.module)
                    lower(reader, m_index.moduleOrder.completesBefore(m_owner.module, function.module) && !m_index.functionsThatFilesOfCyclesMayEnter.quickGet(reader.index()) ? exit : unknownScopeEntry);
            };
            for (VariableID variable : m_owner.variables) {
                for (const FlowScopeLocationAccess& read : operationsOn(m_index.reads, variable.index()))
                    recordOutsideReader(read.function);
                for (const FlowScopeLocationAccess& access : operationsOn(m_index.accessesThroughClassObjects, variable.index()))
                    recordOutsideReader(access.function);
            }
            for (ModuleID exporter : m_owner.exporters) {
                for (const FlowModuleValueRead& read : operationsOn(m_index.moduleValueReads, exporter))
                    recordOutsideReader(read.function);
            }
        }
        recordReadsOfStoredFunctions();
        runFixpoint();
        refineEntriesByCalls();
        AOT::sort(m_definitions);
    }

    void computeOrders(std::span<FlowLocationOrder> orders)
    {
        auto canRunTwiceAtAll = [&](const FlowScopeLocationAccess& store) {
            return canRunTwice(store);
        };
        for (VariableID id : m_owner.variables) {
            FlowScopeLocation location = id.index();
            if (m_index.opaqueLocations.get(location))
                continue;
            const Variable& variable = m_index.program().variables[location];
            bool isSetAtCreation = variable.initialization == VariableInitialization::Function;
            bool startsAbsent = m_index.program().at(variable.declared.written()).primitives & Primitive::Absent;
            orders[location] = { initializationOf(location, isSetAtCreation, startsAbsent), storeCountOf(location, isSetAtCreation, canRunTwiceAtAll) };
            if (orders[location].initialization == InitializationState::StoredBeforeEveryRead && orders[location].stores == StoreCount::AtMostOnce)
                orders[location].isStoredBeforeCallersOfReaders = isStoredBeforeCallersOfReaders(location);
        }
        for (LayoutID layout : m_owner.environments)
            computeEnvironmentOrders(layout, orders);
    }

    Vector<FlowReadOfLocation> takeReadsAfterInitialization() { return std::exchange(m_readsAfterInitialization, { }); }

private:
    struct StoredFunction {
        FlowScopeLocation location;
        FunctionID function;
        FlowScopeEntry store;
    };

    struct StoredFunctionReader {
        FunctionID reader;
        uint32_t first;
        uint32_t count;
    };

    using InitializingStores = Vector<FlowScopeLocationAccess, 4>;

    FlowScopeFrame& ownerFrame() { return *m_frames[0]; }
    FlowScopeFrame* findFrame(FunctionID function) { return m_framesByFunction.get(function); }

    FlowScopeFrame& frameOf(const FlowScopeEntry& entry)
    {
        RELEASE_ASSERT(entry.state == FlowScopeEntryState::AfterPoint);
        FlowScopeFrame* frame = findFrame(entry.function);
        RELEASE_ASSERT(frame);
        return *frame;
    }

    ProgramPoint meet(FlowScopeFrame& frame, ProgramPoint first, ProgramPoint second)
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

    FlowScopeEntry entryInCaller(const FlowScopeEntry& entry)
    {
        FlowScopeFrame& frame = frameOf(entry);
        return entryAt(*frame.caller, frame.call);
    }

    bool isPassedBefore(const FlowScopeEntry& earlier, const FlowScopeEntry& later)
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

    FlowScopeEntry meet(const FlowScopeEntry& first, const FlowScopeEntry& second)
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

    FlowScopeEntry laterOf(const FlowScopeEntry& first, const FlowScopeEntry& second)
    {
        if (first.state == FlowScopeEntryState::Never || second.state == FlowScopeEntryState::Never)
            return neverScopeEntry;
        if (first.state == FlowScopeEntryState::Unknown)
            return second;
        if (second.state == FlowScopeEntryState::Unknown)
            return first;
        return isPassedBefore(second, first) ? first : second;
    }

    FlowScopeEntry& ensureEntry(FunctionID function) { return m_entries.add(function, neverScopeEntry).iterator->value; }

    void lower(FunctionID function, FlowScopeEntry entry)
    {
        FlowScopeEntry current = ensureEntry(function);
        FlowScopeEntry lowered = meet(current, entry);
        if (lowered == current)
            return;
        ensureEntry(function) = lowered;
        m_queue.append(function);
    }

    std::optional<ProgramPoint> pointOf(FlowScopeFrame& frame, ValueID value)
    {
        ValueLocation location = frame.analyses.useLists().locationOf(value);
        if (location.placement != ValuePlacement::InBlock)
            return std::nullopt;
        return ProgramPoint { location.block, location.index };
    }

    FlowScopeEntry entryAt(FlowScopeFrame& frame, ValueID value)
    {
        auto point = pointOf(frame, value);
        return point ? scopeEntryAfter(frame.function, *point) : unknownScopeEntry;
    }

    FlowScopeEntry entryOf(FunctionID function) { return m_entries.get(function); }
    FlowScopeEntry entryByUseOf(FunctionID function) { return m_entriesByUse.get(function); }

    FlowScopeEntry entryByCallsOf(FunctionID function)
    {
        if (m_index.calls.functionsEnteredByHost.get(function.index()))
            return unknownScopeEntry;
        FlowScopeEntry entry = neverScopeEntry;
        m_index.calls.forEachCallOf(function, [&](const FlowScopeCall& call) {
            FlowScopeFrame* frame = findFrame(call.caller);
            entry = meet(entry, frame ? entryAt(*frame, call.site) : laterOf(entryOf(call.caller), call.subject ? entryByStoresTo(call.subject) : unknownScopeEntry));
            return entry.state == FlowScopeEntryState::Unknown ? IterationStatus::Done : IterationStatus::Continue;
        });
        return entry;
    }

    FlowScopeEntry entryByStoresTo(VariableID id)
    {
        return m_entriesByStores.ensure(id, [&] {
            FlowScopeLocation location = id.index();
            const Variable& variable = m_index.program().at(id);
            bool isStaticField = !!variable.owner;
            if (isStaticField || variable.initialization == VariableInitialization::Function)
                return unknownScopeEntry;
            if (m_index.locationOwners[location] != m_owner.function || m_index.opaqueLocations.get(location))
                return unknownScopeEntry;

            FlowScopeEntry entry = neverScopeEntry;
            for (const FlowScopeLocationAccess& store : operationsOn(m_index.stores, location)) {
                FlowScopeFrame* frame = findFrame(store.function);
                entry = meet(entry, frame ? entryAt(*frame, store.value) : unknownScopeEntry);
            }
            return entry;
        }).iterator->value;
    }

    FlowScopeEntry entryBySubjectOf(FunctionID function, ValueID operation)
    {
        VariableID variable = variableBehindSubjectOf(m_index.program().at(function), operation);
        return variable ? entryByStoresTo(variable) : unknownScopeEntry;
    }

    FlowScopeEntry entryOf(const FlowScopeLocationAccess& access)
    {
        if (FlowScopeFrame* frame = findFrame(access.function))
            return entryAt(*frame, access.value);
        return m_index.numbering.isOneLocation(access.subject) ? entryOf(access.function) : entryByUseOf(access.function);
    }

    FlowScopeEntry exitOf(FlowScopeFrame& frame)
    {
        if (frame.exit)
            return *frame.exit;
        FlowScopeEntry exit = neverScopeEntry;
        for (BlockID block : frame.analyses.cfg().reversePostOrder()) {
            if (returnsToCaller(frame.body.at(block).terminator.kind))
                exit = meet(exit, scopeEntryAfter(frame.function, pointBeforeTerminatorOf(frame.body, block)));
        }
        frame.exit = exit;
        return exit;
    }

    std::span<const FlowExportedVariable> exportsOf(FlowScopeLocation location) const
    {
        return recordsWith(m_owner.exports.span(), location, [](const FlowExportedVariable& exported) {
            return exported.variable.index();
        });
    }

    bool isExported(FlowScopeLocation location) const { return !exportsOf(location).empty(); }

    void forEachReader(FlowScopeLocation location, NOESCAPE const Invocable<void(const FlowScopeLocationAccess&)> auto& functor) const
    {
        for (const FlowScopeLocationAccess& read : operationsOn(m_index.reads, location))
            functor(read);
        for (const FlowExportedVariable& exported : exportsOf(location)) {
            for (const FlowModuleValueRead& read : operationsOn(m_index.moduleValueReads, exported.exporter))
                functor(FlowScopeLocationAccess { location, read.function, read.value });
        }
    }

    std::optional<FlowScopeLocation> storedLocation(const FlowScopeFrame& frame, const Use& use) const
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

    void recordStoredFunction(FlowScopeLocation location, FunctionID function, const FlowScopeEntry& store)
    {
        if (m_index.opaqueLocations.get(location)) {
            lower(function, unknownScopeEntry);
            return;
        }
        ensureEntry(function);
        m_storedFunctions.append({ location, function, store });
    }

    void followClosure(FlowScopeFrame& creator, ValueID closure, FunctionID function)
    {
        FlowScopeEntry creation = entryAt(creator, closure);
        while (creation.state == FlowScopeEntryState::AfterPoint && creation.function != m_owner.function)
            creation = entryInCaller(creation);
        bool canCapturePerIterationEnvironment = creation.state != FlowScopeEntryState::AfterPoint || std::ranges::any_of(m_perIterationAllocations, [&](ValueID allocation) {
            return ownerFrame().analyses.dominates(allocation, creation.point);
        });

        LayoutID constructedClass = m_index.program().at(function).constructedClass;
        if (constructedClass && m_index.calls.numberOfMentions[function.index()] == 1 && !canRunTwice({ 0, creator.function, closure }))
            m_classesEvaluatedOnce.set(constructedClass.index());

        ensureEntry(function);
        auto finish = [&](FlowScopeFrame& frame, const Use& use, UseOfClosure kind) {
            if (kind == UseOfClosure::DefinesProperty) {
                VariableID staticField = findStaticFieldVariable(m_index.program(), function, frame.body.at(use.user).key());
                if (staticField && m_index.locationOwners[staticField.index()] == m_owner.function)
                    m_definitions.append({ staticField.index(), frame.function, use.user });
                return;
            }
            auto location = storedLocation(frame, use);
            bool isOpaque = location && m_index.opaqueLocations.get(*location);
            bool isEnteredAtRead = location && !canCapturePerIterationEnvironment && m_index.locationOwners[*location] == m_owner.function;
            FlowScopeEntry entryAtUse = scopeEntryAfter(frame.function, frame.analyses.useLists().pointOf(use));
            if (isOpaque || isEnteredAtRead)
                recordStoredFunction(*location, function, entryAtUse);
            else
                lower(function, entryAtUse);
        };
        forEachUseReachedFrom(creator, closure, [&](FlowScopeFrame& frame, ValueID constructor) {
            for (const ReachedUse& reached : usesReachedFromConstructor(frame, constructor))
                finish(*reached.frame, reached.use, reached.kind);
        }, finish);
    }

    enum class UseOfClosure : uint8_t { Followed, DefinesProperty, Other };

    struct FollowedValue {
        FlowScopeFrame* frame;
        ValueID value;
        bool isConstructorOfMethod { false };
    };

    struct ReachedUse {
        FlowScopeFrame* frame;
        Use use;
        UseOfClosure kind;
    };

    UseOfClosure followUse(FlowScopeFrame& frame, const Use& use, Vector<FollowedValue, 4>& worklist)
    {
        if (use.kind != Use::Kind::Operand)
            return UseOfClosure::Other;
        const Value& user = frame.body.at(use.user);
        if (isForwardedOperand(operandInfoOf(user.opcode, use.operandIndex))) {
            worklist.append({ &frame, use.user });
            return UseOfClosure::Followed;
        }
        FlowScopeFrame* callee = user.opcode == Opcode::CallDirect ? findFrame(user.function()) : nullptr;
        if (callee && callee->caller == &frame && callee->call == use.user) {
            auto argumentIndex = user.variadicOperandIndex(use.operandIndex);
            const auto& parameters = callee->body.at(entryBlock).parameters;
            if (argumentIndex && *argumentIndex < parameters.size()) {
                worklist.append({ callee, parameters[*argumentIndex] });
                return UseOfClosure::Followed;
            }
        }
        if (user.opcode == Opcode::PropDefine && use.operandIndex == OperandIndex::PropDefine::object)
            return UseOfClosure::DefinesProperty;
        if (user.opcode != Opcode::DefineMethod)
            return UseOfClosure::Other;
        if (use.operandIndex == OperandIndex::DefineMethod::classObject)
            return UseOfClosure::Followed;
        if (use.operandIndex != OperandIndex::DefineMethod::function)
            return UseOfClosure::Other;
        ValueID constructor = forwardedSourceOf(frame.body, user.operand(OperandIndex::DefineMethod::classObject));
        if (frame.body.at(constructor).opcode != Opcode::NewClosure)
            return UseOfClosure::Other;
        worklist.append({ &frame, constructor, true });
        return UseOfClosure::Followed;
    }

    // A method goes wherever the constructor of its class goes.
    void forEachUseReachedFrom(FlowScopeFrame& creator, ValueID closure, NOESCAPE const Invocable<void(FlowScopeFrame&, ValueID)> auto& followConstructor, NOESCAPE const Invocable<void(FlowScopeFrame&, const Use&, UseOfClosure)> auto& functor)
    {
        Vector<FollowedValue, 4> worklist;
        worklist.append({ &creator, closure });
        while (!worklist.isEmpty()) {
            FollowedValue followed = worklist.takeLast();
            FlowScopeFrame& frame = *followed.frame;
            if (followed.isConstructorOfMethod) {
                followConstructor(frame, followed.value);
                continue;
            }
            for (const Use& use : frame.analyses.useLists().usesOf(followed.value)) {
                UseOfClosure kind = followUse(frame, use, worklist);
                if (kind != UseOfClosure::Followed)
                    functor(frame, use, kind);
            }
        }
    }

    // The same for every method of the class, and a class object has a use for each method.
    const Vector<ReachedUse>& usesReachedFromConstructor(FlowScopeFrame& frame, ValueID constructor)
    {
        std::pair<FlowScopeFrame*, ValueID> key { &frame, constructor };
        if (auto known = m_usesReachedFromConstructors.find(key); known != m_usesReachedFromConstructors.end())
            return *known->value;
        auto uses = makeUnique<Vector<ReachedUse>>();
        forEachUseReachedFrom(frame, constructor, [&](FlowScopeFrame& frame, ValueID constructor) {
            uses->appendVector(usesReachedFromConstructor(frame, constructor));
        }, [&](FlowScopeFrame& frame, const Use& use, UseOfClosure kind) {
            uses->append({ &frame, use, kind });
        });
        return *m_usesReachedFromConstructors.add(key, WTF::move(uses)).iterator->value;
    }

    void recordReadsOfStoredFunctions()
    {
        AOT::sort(m_storedFunctions, { }, &StoredFunction::location);
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

        struct Run {
            ModuleID exporter;
            uint32_t first;
            uint32_t count;
        };
        Vector<Run> runsOfExporters;
        for (uint32_t first = 0; first < m_storedFunctions.size();) {
            FlowScopeLocation location = m_storedFunctions[first].location;
            uint32_t count = recordsWith(m_storedFunctions.span().subspan(first), location, &StoredFunction::location).size();
            for (const FlowScopeLocationAccess& read : operationsOn(m_index.reads, location))
                recordReader(read, first, count);
            for (const FlowExportedVariable& exported : exportsOf(location))
                runsOfExporters.append({ exported.exporter, first, count });
            for (const FlowScopeLocationAccess& access : operationsOn(m_index.accessesThroughClassObjects, location))
                recordReader(access, first, count);
            if (isExported(location)) {
                for (uint32_t index = first; index < first + count; ++index)
                    lower(m_storedFunctions[index].function, laterOf(m_storedFunctions[index].store, m_hostEntry));
            }
            first += count;
        }
        AOT::stableSort(runsOfExporters, { }, &Run::exporter);
        for (size_t position = 0; position < runsOfExporters.size();) {
            ModuleID exporter = runsOfExporters[position].exporter;
            uint32_t first = m_functionsTakenOut.size();
            for (; position < runsOfExporters.size() && runsOfExporters[position].exporter == exporter; ++position)
                m_functionsTakenOut.append(m_storedFunctions.subspan(runsOfExporters[position].first, runsOfExporters[position].count));
            uint32_t count = m_functionsTakenOut.size() - first;
            for (const FlowModuleValueRead& read : operationsOn(m_index.moduleValueReads, exporter))
                recordReader(FlowScopeLocationAccess { m_functionsTakenOut[first].location, read.function, read.value }, first, count);
        }
        AOT::sort(m_storedFunctionReaders, { }, [](const StoredFunctionReader& read) {
            return std::tuple(read.reader, read.first);
        });
    }

    void runFixpoint()
    {
        HashMap<uint32_t, Vector<FlowScopeEntry, 2>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> entriesOfReadersOfRuns;
        while (!m_queue.isEmpty()) {
            FunctionID function = m_queue.takeLast();
            FlowScopeEntry entry = ensureEntry(function);
            for (const FlowFunctionMention& reference : m_index.mentionsIn(function))
                lower(reference.function, entry);
            AOT_THRESHOLD(size_t, mostEntriesToRemember, 8);
            for (const StoredFunctionReader& read : recordsWith(m_storedFunctionReaders.span(), function, &StoredFunctionReader::reader)) {
                Vector<FlowScopeEntry, 2>& entriesOfReaders = entriesOfReadersOfRuns.add(read.first, Vector<FlowScopeEntry, 2>()).iterator->value;
                if (entriesOfReaders.contains(entry))
                    continue;
                if (entriesOfReaders.size() < mostEntriesToRemember)
                    entriesOfReaders.append(entry);
                for (const StoredFunction& stored : m_functionsTakenOut.subspan(read.first, read.count))
                    lower(stored.function, laterOf(stored.store, entry));
            }
        }
    }

    void refineEntriesByCalls()
    {
        m_entriesByUse = m_entries;
        if (m_owner.variables.isEmpty())
            return;
        for (auto& entry : m_entries) {
            entry.value = neverScopeEntry;
            m_queue.append(entry.key);
        }
        AOT::sort(m_queue, std::greater { });
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
            m_index.calls.forEachCallBy(m_index.inputs.calleeLists, function, [&](const FlowScopeCall& call) {
                auto callee = m_entries.find(call.callee);
                if (callee != m_entries.end() && callee->value != unknownScopeEntry && waiting.add(call.callee).isNewEntry)
                    m_queue.append(call.callee);
                return IterationStatus::Continue;
            });
        }
        m_queue.shrink(0);
    }

    bool completesBeforeReturn(FlowScopeFrame& frame, ValueID value)
    {
        FlowScopeEntry exit = exitOf(frame);
        switch (exit.state) {
        case FlowScopeEntryState::Unknown:
            return false;
        case FlowScopeEntryState::AfterPoint:
            return frame.analyses.dominates(value, exit.point);
        case FlowScopeEntryState::Never:
            return true;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool precedes(const FlowScopeLocationAccess& store, const FlowScopeEntry& entry)
    {
        switch (entry.state) {
        case FlowScopeEntryState::Unknown:
            return false;
        case FlowScopeEntryState::AfterPoint:
            break;
        case FlowScopeEntryState::Never:
            return true;
        }

        FlowScopeFrame* frame = findFrame(store.function);
        RELEASE_ASSERT(frame);
        ValueID completed = store.value;
        FlowScopeEntry entered = entry;
        while (frameOf(entered).depth > frame->depth)
            entered = entryInCaller(entered);
        while (frame->function != entered.function) {
            if (!completesBeforeReturn(*frame, completed))
                return false;
            completed = frame->call;
            frame = frame->caller;
            if (frameOf(entered).depth > frame->depth)
                entered = entryInCaller(entered);
        }
        return frame->analyses.dominates(completed, entered.point);
    }

    bool anyPrecedes(const InitializingStores& stores, const FlowScopeEntry& entry)
    {
        return std::ranges::any_of(stores, [&](const FlowScopeLocationAccess& store) {
            return precedes(store, entry);
        });
    }

    std::span<const FlowScopeLocationAccess> definitionsOf(FlowScopeLocation location) const { return operationsOn(m_definitions, location); }

    InitializingStores initializingStoresOf(FlowScopeLocation location)
    {
        bool isOneLocation = m_index.numbering.isOneLocation(location);
        InitializingStores stores;
        for (const FlowScopeLocationAccess& store : operationsOn(m_index.stores, location)) {
            if (store.function == m_owner.function || (isOneLocation && findFrame(store.function)))
                stores.append(store);
        }
        stores.append(definitionsOf(location));
        return stores;
    }

    bool isDefinedAtEveryAccessThroughClassObject(FlowScopeLocation location, const InitializingStores& stores)
    {
        LayoutID owningClass = m_index.program().variables[location].owner;
        if (!m_classesEvaluatedOnce.get(owningClass.index()))
            return false;

        bool isDefinedWhenHeld = anyPrecedes(stores, entryByUseOf(m_index.program().at(owningClass).constructor));
        if (m_index.inputs.staticFieldsOfEscapedClasses.get(location) && !isDefinedWhenHeld)
            return false;

        auto definitions = definitionsOf(location);
        return std::ranges::all_of(operationsOn(m_index.accessesThroughClassObjects, location), [&](const FlowScopeLocationAccess& access) {
            if (std::ranges::binary_search(definitions, access))
                return true;
            if (isDefinedWhenHeld && !findFrame(access.function))
                return true;
            return anyPrecedes(stores, entryOf(access)) || anyPrecedes(stores, entryBySubjectOf(access.function, access.value));
        });
    }

    InitializationState initializationOf(FlowScopeLocation location, bool isSetAtCreation, bool startsAbsent)
    {
        if (isSetAtCreation)
            return InitializationState::SetAtCreation;
        InitializingStores stores = initializingStoresOf(location);
        bool isEveryReadCovered = true;
        forEachReader(location, [&](const FlowScopeLocationAccess& read) {
            isEveryReadCovered = isEveryReadCovered && anyPrecedes(stores, entryOf(read));
        });
        if (isExported(location))
            isEveryReadCovered = isEveryReadCovered && anyPrecedes(stores, m_hostEntry);
        bool isStaticField = location < m_index.numbering.numberOfVariables && m_index.program().variables[location].owner;
        if (isStaticField)
            isEveryReadCovered = isEveryReadCovered && isDefinedAtEveryAccessThroughClassObject(location, stores);
        if (isEveryReadCovered)
            return InitializationState::StoredBeforeEveryRead;

        if (startsAbsent && !isStaticField && !stores.isEmpty()) {
            for (const FlowScopeLocationAccess& read : operationsOn(m_index.reads, location)) {
                if (anyPrecedes(stores, entryOf(read)))
                    m_readsAfterInitialization.append({ read.function, read.value });
            }
        }
        return InitializationState::Unknown;
    }

    bool anyPrecedesCallersOf(const InitializingStores& stores, FunctionID reader)
    {
        AOT_THRESHOLD(size_t, mostCallersToFollow, 64);
        Vector<FunctionID, 8> followed { reader };
        for (size_t next = 0; next < followed.size(); ++next) {
            FunctionID function = followed[next];
            if (followed.size() > mostCallersToFollow || m_index.calls.functionsEnteredByHost.get(function.index()) || !anyPrecedes(stores, entryOf(function)))
                return false;
            bool precedesCallsInFrames = true;
            m_index.calls.forEachCallOf(function, [&](const FlowScopeCall& call) {
                if (FlowScopeFrame* frame = findFrame(call.caller)) {
                    if (!anyPrecedes(stores, entryAt(*frame, call.site))) {
                        precedesCallsInFrames = false;
                        return IterationStatus::Done;
                    }
                } else if (!followed.contains(call.caller))
                    followed.append(call.caller);
                return IterationStatus::Continue;
            });
            if (!precedesCallsInFrames)
                return false;
        }
        return true;
    }

    bool isStoredBeforeCallersOfReaders(FlowScopeLocation location)
    {
        InitializingStores stores = initializingStoresOf(location);
        bool result = true;
        forEachReader(location, [&](const FlowScopeLocationAccess& read) {
            result = result && (findFrame(read.function) || anyPrecedesCallersOf(stores, read.function));
        });
        return result;
    }

    // Where the first of the functions that capture an environment of the layout may be entered.
    FlowScopeEntry entryOfFirstCapturer(LayoutID layout)
    {
        const Program& program = m_index.program();
        auto captures = [&](FunctionID function) {
            return std::ranges::any_of(program.at(function).captures, [&](const DeclaredType& capture) {
                return program.at(capture.written()).environment == layout;
            });
        };
        Vector<FunctionID> capturers;
        for (FunctionID function : m_entriesByUse.keys()) {
            if (captures(function))
                capturers.append(function);
        }
        AOT::sort(capturers);
        FlowScopeEntry entry = neverScopeEntry;
        for (FunctionID function : capturers)
            entry = meet(entry, entryByUseOf(function));
        for (auto& frame : m_frames) {
            if (frame->caller && captures(frame->function))
                entry = meet(entry, entryAt(*frame->caller, frame->call));
        }
        return entry;
    }

    // Every store puts a new closure there that captures the environment it is stored in, and no other environment of that layout.
    bool holdsClosureOverOwnEnvironment(FlowScopeLocation location, LayoutID layout)
    {
        const Program& program = m_index.program();
        auto stores = operationsOn(m_index.stores, location);
        return !stores.empty() && std::ranges::all_of(stores, [&](const FlowScopeLocationAccess& access) {
            const Function& function = program.at(access.function);
            const Value& store = function.at(access.value);
            if (store.opcode != Opcode::PutEnvironmentField)
                return false;
            const Value& closure = function.at(sameObjectThroughCasts(function, store.valueOperand()));
            if (closure.opcode != Opcode::NewClosure)
                return false;
            auto declared = program.at(closure.function()).captures;
            auto captured = closure.variadicOperands();
            if (declared.size() != captured.size())
                return false;
            RootThroughCasts environment = sameObjectThroughCasts(function, store.environmentOperand());
            unsigned numberOfCaptures = 0;
            for (size_t index = 0; index < declared.size(); ++index) {
                if (program.at(declared[index].written()).environment != layout)
                    continue;
                if (sameObjectThroughCasts(function, captured[index]) != environment)
                    return false;
                ++numberOfCaptures;
            }
            return numberOfCaptures == 1;
        });
    }

    StoreCount storeCountOf(FlowScopeLocation location, bool isSetAtCreation, NOESCAPE const Invocable<bool(const FlowScopeLocationAccess&)> auto& canRunTwice)
    {
        auto stores = operationsOn(m_index.stores, location);
        auto storesThroughClassObjects = operationsOn(m_index.storesThroughClassObjects, location);
        size_t numberOfStores = stores.size() + storesThroughClassObjects.size();
        if (!numberOfStores)
            return isSetAtCreation ? StoreCount::AtMostOnce : StoreCount::Never;
        if (isSetAtCreation || numberOfStores != 1)
            return StoreCount::Unknown;
        return canRunTwice(stores.empty() ? storesThroughClassObjects.front() : stores.front()) ? StoreCount::Unknown : StoreCount::AtMostOnce;
    }

    bool canRunTwice(const FlowScopeLocationAccess& store)
    {
        ValueID value = store.value;
        FlowScopeFrame* frame = findFrame(store.function);
        if (!frame)
            return true;
        for (; frame; value = frame->call, frame = frame->caller) {
            auto point = pointOf(*frame, value);
            if (!point || frame->analyses.blockCycles().isInCycle(point->block))
                return true;
        }
        return false;
    }

    std::optional<BlockID> trackedAllocationBlock(LayoutID layout, std::span<const FlowEnvironmentAllocation> sites)
    {
        if (sites.size() != 1)
            return std::nullopt;
        FlowScopeFrame& frame = ownerFrame();
        auto point = pointOf(frame, sites.front().value);
        if (!point)
            return std::nullopt;
        auto isOwnAccessOfAllocation = [&](const FlowScopeLocationAccess& access) {
            return frame.analyses.sameObjectThroughCasts(frame.body.at(access.value).environmentOperand()) == sites.front().value;
        };
        uint32_t end = m_index.numbering.firstField[layout.index() + 1];
        for (FlowScopeLocation location = m_index.numbering.firstField[layout.index()]; location < end; ++location) {
            auto ownReads = recordsWith(operationsOn(m_index.reads, location), m_owner.function, &FlowScopeLocationAccess::function);
            auto ownStores = recordsWith(operationsOn(m_index.stores, location), m_owner.function, &FlowScopeLocationAccess::function);
            if (!std::ranges::all_of(ownReads, isOwnAccessOfAllocation) || !std::ranges::all_of(ownStores, isOwnAccessOfAllocation))
                return std::nullopt;
        }
        return point->block;
    }

    bool isListedWithUndefinedAlone(std::span<const FlowEnvironmentAllocation> sites, uint32_t field)
    {
        FlowScopeFrame& frame = ownerFrame();
        return std::ranges::all_of(sites, [&](const FlowEnvironmentAllocation& site) {
            const Value& allocation = frame.body.at(site.value);
            std::span<const uint32_t> fields = allocation.fields();
            for (size_t index = 0; index < fields.size(); ++index) {
                if (fields[index] == field && !listsStartOfVar(frame.body, allocation, index))
                    return false;
            }
            return true;
        });
    }

    void computeEnvironmentOrders(LayoutID layout, std::span<FlowLocationOrder> orders)
    {
        auto sites = operationsOn(m_index.allocations, layout);
        auto allocationBlock = trackedAllocationBlock(layout, sites);
        auto canRunTwicePerInstance = [&](const FlowScopeLocationAccess& store) {
            if (store.function != m_owner.function)
                return true;
            auto point = pointOf(ownerFrame(), store.value);
            return !point || ownerFrame().analyses.cfg().isInCycleAvoiding(point->block, *allocationBlock);
        };
        const auto& fields = m_index.program().at(layout).fields;
        uint32_t numberOfFields = fields.size();
        Vector<uint32_t> allocationsListing(FillWith { }, numberOfFields, 0u);
        for (const FlowEnvironmentAllocation& site : sites) {
            for (uint32_t field : ownerFrame().body.at(site.value).fields())
                ++allocationsListing[field];
        }
        for (uint32_t field = 0; field < numberOfFields; ++field) {
            FlowScopeLocation location = *m_index.numbering.locationOf(layout, field);
            if (m_index.opaqueLocations.get(location))
                continue;
            bool isSetAtCreation = allocationsListing[field] == sites.size();
            if (!allocationBlock) {
                bool hasNoStore = operationsOn(m_index.stores, location).empty();
                orders[location] = { isSetAtCreation ? InitializationState::SetAtCreation : InitializationState::Unknown, isSetAtCreation && hasNoStore ? StoreCount::AtMostOnce : StoreCount::Unknown };
                continue;
            }
            if (isSetAtCreation && isListedWithUndefinedAlone(sites, field) && initializationOf(location, false, false) == InitializationState::StoredBeforeEveryRead) {
                orders[location] = { InitializationState::StoredBeforeEveryRead, storeCountOf(location, false, canRunTwicePerInstance), true };
                continue;
            }
            bool startsAbsent = m_index.program().at(fields[field].declared.written()).primitives & Primitive::Absent;
            orders[location] = { initializationOf(location, isSetAtCreation, startsAbsent), storeCountOf(location, isSetAtCreation, canRunTwicePerInstance) };
        }
        FlowScopeEntry firstCapturer = entryOfFirstCapturer(layout);
        for (uint32_t field = 0; field < numberOfFields; ++field) {
            FlowScopeLocation location = *m_index.numbering.locationOf(layout, field);
            if (m_index.opaqueLocations.get(location))
                continue;
            orders[location].isStoredBeforeCapturersRun = orders[location].initialization == InitializationState::StoredBeforeEveryRead && anyPrecedes(initializingStoresOf(location), firstCapturer);
            orders[location].holdsClosureOverOwnEnvironment = !allocationsListing[field] && holdsClosureOverOwnEnvironment(location, layout);
        }
    }

    const FlowScopeIndex& m_index;
    const FlowScopeOwner& m_owner;
    Vector<std::unique_ptr<FlowScopeFrame>, 1> m_frames;
    HashMap<FunctionID, FlowScopeFrame*> m_framesByFunction;
    HashMap<FunctionID, FlowScopeEntry> m_entries;
    HashMap<FunctionID, FlowScopeEntry> m_entriesByUse;
    HashMap<VariableID, FlowScopeEntry> m_entriesByStores;
    Vector<FunctionID> m_queue;
    Vector<StoredFunction> m_storedFunctions;
    HashMap<std::pair<FlowScopeFrame*, ValueID>, std::unique_ptr<Vector<ReachedUse>>> m_usesReachedFromConstructors;
    Vector<StoredFunctionReader> m_storedFunctionReaders;
    Vector<StoredFunction> m_functionsTakenOut;
    Vector<FlowScopeLocationAccess> m_definitions;
    Vector<FlowReadOfLocation> m_readsAfterInitialization;
    BitVector m_classesEvaluatedOnce;
    Vector<ValueID> m_perIterationAllocations;
    FlowScopeEntry m_hostEntry { unknownScopeEntry };
};

static bool runsAfterCompletion(const FlowScopeIndex& index, FunctionID function, ModuleID completed)
{
    ModuleID module = index.program().at(function).module;
    return module && !index.functionsEnteredWithoutMention.quickGet(function.index()) && index.moduleOrder.completesBefore(completed, module);
}

static bool canObserveUnfinishedModule(const FlowScopeIndex& index, const FlowModuleValueRead& read)
{
    const Program& program = index.program();
    Opcode opcode = program.at(read.function).at(read.value).opcode;
    if (opcode == Opcode::ImportModule)
        return index.moduleOrder.modulesThatJobsMayInterrupt.get(read.subject.index());
    if (opcode == Opcode::RequireModule)
        return program.at(read.subject).requireResult != RequireResult::ModuleExports;
    return true;
}

static BitVector findVariablesReadEarly(const FlowScopeIndex& index)
{
    const Program& program = index.program();
    BitVector variables(program.variables.size());
    BitVector isVisited(program.modules.size());
    Vector<ModuleID> worklist;
    auto visit = [&](ModuleID module) {
        if (!isVisited.set(module.index()))
            worklist.append(module);
    };

    for (const FlowModuleValueRead& read : index.moduleValueReads) {
        if (canObserveUnfinishedModule(index, read) && !runsAfterCompletion(index, read.function, read.subject))
            visit(read.subject);
    }
    for (size_t module : index.inputs.modulesLoadedByHost)
        visit(ModuleID(static_cast<uint32_t>(module)));
    while (!worklist.isEmpty()) {
        const Module& module = program.at(worklist.takeLast());
        for (const SyntheticExport& entry : module.syntheticExports) {
            if (entry.variable)
                variables.set(entry.variable.index());
        }
        for (const ModuleExport& entry : module.exports) {
            switch (entry.kind) {
            case ModuleExportKind::Variable:
                if (entry.variable)
                    variables.set(entry.variable.index());
                break;
            case ModuleExportKind::Namespace:
                if (entry.module)
                    visit(entry.module);
                break;
            case ModuleExportKind::Indirect:
                break;
            }
        }
    }
    return variables;
}

static void computeSyntheticExportOrders(const FlowScopeIndex& index, std::span<FlowLocationOrder> orders)
{
    BitVector variablesReadEarly = findVariablesReadEarly(index);
    for (size_t synthetic : index.syntheticVariables) {
        FlowScopeLocation variable = static_cast<FlowScopeLocation>(synthetic);
        if (index.opaqueLocations.get(variable))
            continue;
        ModuleID module = index.program().variables[variable].module;
        if (index.inputs.modulesLoadedByName.get(module.index()))
            continue;
        bool isReadAfterCompletion = !variablesReadEarly.get(variable) && std::ranges::all_of(operationsOn(index.reads, variable), [&](const FlowScopeLocationAccess& read) {
            return runsAfterCompletion(index, read.function, module);
        });
        orders[variable] = { isReadAfterCompletion ? InitializationState::StoredBeforeEveryRead : InitializationState::Unknown, operationsOn(index.stores, variable).empty() ? StoreCount::AtMostOnce : StoreCount::Unknown };
    }
}

FlowScopeOrders computeScopeOrders(const FlowOrderInputs& inputs, const FlowModuleOrder& moduleOrder)
{
    FlowScopeIndex index = createScopeIndex(inputs, moduleOrder);
    Vector<FlowLocationOrder> orders(index.numbering.numberOfLocations());
    Vector<Vector<FlowReadOfLocation>> readsAfterInitializationOfOwners(index.owners.size());
    forEachIndexInParallel(inputs.build.parallelLoops(), index.owners.size(), [&](unsigned owner) {
        FlowScopeOwnerAnalysis analysis(index, index.owners[owner]);
        analysis.computeEntries();
        analysis.computeOrders(orders.mutableSpan());
        readsAfterInitializationOfOwners[owner] = analysis.takeReadsAfterInitialization();
    });
    Vector<FlowReadOfLocation> readsAfterInitialization;
    for (Vector<FlowReadOfLocation>& reads : readsAfterInitializationOfOwners)
        readsAfterInitialization.appendVector(reads);
    AOT::sort(readsAfterInitialization);
    computeSyntheticExportOrders(index, orders.mutableSpan());
    return { WTF::move(index.numbering), WTF::move(orders), WTF::move(readsAfterInitialization) };
}

std::optional<uint32_t> FlowScopeLocationNumbering::locationOf(LayoutID environment, uint32_t fieldIndex) const
{
    if (!environment || environment.index() + 1 >= firstField.size())
        return std::nullopt;
    uint32_t first = firstField[environment.index()];
    if (fieldIndex >= firstField[environment.index() + 1] - first)
        return std::nullopt;
    return first + fieldIndex;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
