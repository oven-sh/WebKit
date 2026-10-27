/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResults.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTFlowRecords.h"
#include "AOTPhaseTimes.h"
#include "AOTSort.h"
#include "AOTValueFlowPositions.h"
#include "AOTValueFlowResultsInternal.h"
#include "AOTVector.h"
#include <algorithm>
#include <tuple>
#include <wtf/Condition.h>
#include <wtf/Deque.h>
#include <wtf/Function.h>
#include <wtf/Lock.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OptionSet<ObjectMutation> mutationsOfBuiltinInstancesWithoutMark { ObjectMutation::ChangePrototype, ObjectMutation::BecomePrototype, ObjectMutation::StoreOfOtherType };

static void stampAllocationsOfBuiltinInstances(const ValueFlowResultInputs& inputs, const ValueFlowContainerResults& containers, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    FlowApplication& application = *tables.application;
    unsigned indexOfElements = indexOfArrayElementsInType(program.builtins());
    Vector<AllocationClassID> classOfBuiltinClass(program.builtins().classCount());
    AllocationClassID classOfUnwrittenArrays;
    size_t numberOfOtherSites = tables.sites.allocationSites.size();
    size_t numberOfOtherClasses = tables.sites.allocationClasses.size();

    struct Instance {
        ValueID operation;
        ValueFlowObjectID object;
        BuiltinClassID builtinClass;
        OptionSet<ObjectMutation> markedMutations;
        bool neverChanges;
    };
    ParallelArray<Vector<Instance>> instancesOfFunction(inputs.loops(), program.functions.size(), [&](unsigned index) {
        Vector<Instance> instances;
        if (!inputs.isReached(FunctionID(index)) || !inputs.bodies().holdsAnyOf(FunctionID(index), notedOperations({ ValueFlowNotedOperation::AllocationSite })))
            return instances;
        const Function& function = program.functions[index];
        for (uint32_t valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
            ValueFlowObjectID object = PayloadWord::allocationSite(function.values[valueIndex].opcode) ? universe.siteObject(FunctionID(index), ValueID(valueIndex)) : ValueFlowObjectID();
            const ValueFlowSite* site = object ? universe.siteOf(object) : nullptr;
            if (site && site->origin == ValueFlowSiteOrigin::Operation && !site->layout && site->builtinClass)
                instances.append({ ValueID(valueIndex), object, site->builtinClass, markedMutationsOf(inputs, object), neverChangesOnceMade(inputs, object) });
        }
        return instances;
    });

    auto functionsWithInstances = findIndicesInParallel(inputs.loops(), program.functions.size(), [&](unsigned index) {
        return !instancesOfFunction[index].isEmpty();
    });
    auto classOf = [&](const Instance& instance) -> AllocationClassID& {
        bool isUnwrittenArray = instance.builtinClass == BuiltinClasses::Array && !instance.markedMutations.contains(ObjectMutation::StoreAfterCreation);
        return isUnwrittenArray ? classOfUnwrittenArrays : classOfBuiltinClass[instance.builtinClass.index()];
    };
    Vector<uint32_t> firstSiteOfFunction;
    firstSiteOfFunction.reserveInitialCapacity(functionsWithInstances.size());
    uint32_t numberOfSites = static_cast<uint32_t>(numberOfOtherSites);
    for (uint32_t index : functionsWithInstances) {
        firstSiteOfFunction.append(numberOfSites);
        for (const Instance& instance : instancesOfFunction[index]) {
            AllocationClassID& allocationClass = classOf(instance);
            if (!allocationClass) {
                allocationClass = AllocationClassID(tables.sites.allocationClasses.size());
                AllocationClass instances;
                instances.builtinClass = instance.builtinClass;
                instances.impossibleMutations = allObjectMutations - mutationsOfBuiltinInstancesWithoutMark;
                tables.sites.allocationClasses.append(WTF::move(instances));
            }
            tables.sites.allocationClasses[allocationClass.index()].impossibleMutations.remove(instance.markedMutations);
            if (instance.neverChanges && inputs.build.isEnabled(FlowResultKind::HolderMutations, instance.builtinClass.index()))
                tables.sites.sitesOfObjectsThatNeverChange.set(numberOfSites);
            ++numberOfSites;
        }
    }
    tables.sites.allocationSites.grow(numberOfSites);
    forEachIndexInParallel(inputs.loops(), functionsWithInstances.size(), [&](unsigned position) {
        uint32_t index = functionsWithInstances[position];
        const Function& function = program.functions[index];
        Vector<FlowAllocationStamp>& stamps = application.bodies[index].allocations;
        uint32_t nextSite = firstSiteOfFunction[position];
        for (const Instance& instance : instancesOfFunction[index]) {
            ValueID operation = instance.operation;
            AllocationClassID allocationClass = classOf(instance);
            const ValueFlowContainerFacts* facts = containers.find(instance.object);
            AllocationSiteID site(nextSite++);
            tables.sites.allocationSites[site.index()] = AllocationSite { FunctionID(index), { }, instance.builtinClass, allocationClass, function.locationOf(operation) };
            bool hasElements = facts && function.at(operation).opcode == Opcode::NewArray && indexOfElements < facts->contents.size();
            stamps.append(FlowAllocationStamp { .value = operation, .site = site, .allocationClass = allocationClass, .elements = hasElements ? facts->contents[indexOfElements] : TypeID(), .arrayStorage = hasElements ? facts->arrayStorageToldToCreator : ArrayStorage::Other });
        }
        AOT::sort(stamps, { }, &FlowAllocationStamp::value);
    });

    classOfBuiltinClass.append(classOfUnwrittenArrays);
    for (AllocationClassID allocationClass : classOfBuiltinClass) {
        if (!allocationClass)
            continue;
        AllocationClass& instances = tables.sites.allocationClasses[allocationClass.index()];
        if (!inputs.build.isEnabled(FlowResultKind::HolderMutations, instances.builtinClass.index()))
            instances.impossibleMutations = { };
    }
    inputs.build.statistics().count(StatisticID::FlowAllocationSites, tables.sites.allocationSites.size() - numberOfOtherSites);
    inputs.build.statistics().count(StatisticID::FlowAllocationClasses, tables.sites.allocationClasses.size() - numberOfOtherClasses);
}

static void orderAllocationSites(const ParallelLoops& loops, FlowResultTables& tables)
{
    Vector<AllocationSite>& sites = tables.sites.allocationSites;
    Vector<AllocationSiteID> orderedSiteOf(sites.size());
    uint32_t numberOfOrderedSites = 0;
    auto order = [&](AllocationSiteID site) {
        if (!orderedSiteOf[site.index()])
            orderedSiteOf[site.index()] = AllocationSiteID(numberOfOrderedSites++);
    };
    Vector<FlowBodyUpdate>& bodies = tables.application->bodies;
    Vector<AllocationSiteID> sitesOfStamps = collectInParallel<AllocationSiteID>(loops, bodies.size(), [&](unsigned index, Vector<AllocationSiteID>& found) {
        AOT::sort(bodies[index].allocations, { }, &FlowAllocationStamp::value);
        for (const FlowAllocationStamp& stamp : bodies[index].allocations)
            found.append(stamp.site);
    });
    for (AllocationSiteID site : sitesOfStamps)
        order(site);
    for (uint32_t index = 0; index < sites.size(); ++index)
        order(AllocationSiteID(index));

    Vector<AllocationSite> orderedSites(sites.size());
    for (uint32_t index = 0; index < sites.size(); ++index)
        orderedSites[orderedSiteOf[index].index()] = WTF::move(sites[index]);
    sites = WTF::move(orderedSites);
    BitVector orderedSitesOfObjectsThatNeverChange;
    for (size_t index : tables.sites.sitesOfObjectsThatNeverChange)
        orderedSitesOfObjectsThatNeverChange.set(orderedSiteOf[index].index());
    tables.sites.sitesOfObjectsThatNeverChange = WTF::move(orderedSitesOfObjectsThatNeverChange);

    auto renumber = [&](AllocationSiteID& site) {
        if (site)
            site = orderedSiteOf[site.index()];
    };
    forEachIndexInParallel(loops, bodies.size(), [&](unsigned index) {
        for (FlowAllocationStamp& stamp : bodies[index].allocations)
            renumber(stamp.site);
    });
    for (AllocationSiteID& site : tables.sites.typedResultSites)
        renumber(site);
    for (FlowDataNode& node : tables.sites.dataNodes) {
        renumber(node.rootSite);
        renumber(node.stamp.site);
    }
    AOT::sort(tables.sites.dataNodes, { }, [](const FlowDataNode& node) {
        return std::tuple(node.rootSite, node.ordinal);
    });
}

static void reportRulesThatWereNotDue(const ValueFlowSolver& solver, BuildContext& build)
{
    if (const ValueFlowSolver::AddedByGeneralRules& added = solver.addedByGeneralRules(); added.isAnything()) {
        StringBuilder detail;
        detail.append("with every rule on its general way: "_s, added.nodesThatHoldMore, " sets that were there hold more, "_s, added.functionsReached, " functions reached, "_s, added.builtinsUsedMore, " built-ins used in more ways; the first nodes:"_s);
        for (ValueFlowNodeID node : added.firstNodesThatHoldMore) {
            detail.append(' ', node.index());
            if (const ValueFlowNodeKey* key = solver.keyOfMadeNode(node))
                detail.append(" (made: kind "_s, static_cast<unsigned>(key->kind), ", detail "_s, key->detail, ')');
        }
        build.diagnostics().add(InternalError { .kind = InternalErrorKind::FlowEndedTooEarly, .count = static_cast<uint32_t>(std::min<uint64_t>(added.nodesThatHoldMore + added.functionsReached + added.builtinsUsedMore, UINT32_MAX)), .detail = detail.toString() });
    }
    uint64_t runs = solver.numberOfRunsThatWereNotDueAndAdded();
    if (!runs)
        return;
    ValueFlowSolver::Round added;
    for (const ValueFlowSolver::Round& round : solver.rounds()) {
        if (!round.isFull)
            continue;
        added.functionsReached += round.functionsReached;
        added.nodesMade += round.nodesMade;
        added.edgesAdded += round.edgesAdded;
        added.nodesMoved += round.nodesMoved;
    }
    String whatWasAdded = makeString(added.functionsReached, " functions reached, "_s, added.nodesMade, " nodes made, "_s, added.edgesAdded, " edges added, "_s, added.nodesMoved, " sets moved"_s);
    build.diagnostics().add(InternalError { .kind = InternalErrorKind::FlowEndedTooEarly, .count = static_cast<uint32_t>(std::min<uint64_t>(runs, UINT32_MAX)), .detail = WTF::move(whatWasAdded) });
    for (ValueFlowStatementID id : solver.firstStatementsThatWereNotDueAndAdded()) {
        const ValueFlowStatement& statement = solver.statementAt(id);
        String detail = statement.kind == ValueFlowStatementKind::Operation ? makeString(nameOf(statement.opcode)) : makeString("a statement of kind "_s, static_cast<unsigned>(statement.kind), " that the solve has made"_s);
        build.diagnostics().add(InternalError { .kind = InternalErrorKind::FlowRuleWasNotDue, .function = statement.function, .value = statement.site, .detail = WTF::move(detail) });
    }
}

namespace {

class ResultSteps final : public ThreadSafeRefCounted<ResultSteps> {
public:
    using StepID = unsigned;

    static Ref<ResultSteps> create(const ParallelLoops& loops) { return adoptRef(*new ResultSteps(loops)); }

    StepID add(std::initializer_list<StepID> awaited, WTF::Function<void()>&& run, TaskPriority priority = TaskPriority::Foreground)
    {
        RELEASE_ASSERT(priority != TaskPriority::Idle);
        StepID id = m_steps.size();
        m_steps.append(Step { WTF::move(run), static_cast<unsigned>(awaited.size()), priority, { } });
        for (StepID earlier : awaited)
            m_steps[earlier].followers.append(id);
        return id;
    }

    void run()
    {
        if (m_loops.width() == 1) {
            for (Step& step : m_steps)
                step.run();
            return;
        }
        {
            Locker locker { m_lock };
            m_numberUnfinished = m_steps.size();
            for (StepID id = 0; id < m_steps.size(); ++id) {
                if (!m_steps[id].numberAwaited)
                    dueOf(m_steps[id].priority).append(id);
            }
            if (size_t numberDue = dueOf(TaskPriority::Foreground).size())
                askForThreads(TaskPriority::Foreground, numberDue - 1);
            askForThreads(TaskPriority::Background, dueOf(TaskPriority::Background).size());
        }
        for (;;) {
            StepID step;
            {
                Locker locker { m_lock };
                Deque<StepID>& due = dueOf(TaskPriority::Foreground);
                while (due.isEmpty() && m_numberUnfinished) {
                    m_callerWaits = true;
                    m_hasChanged.wait(m_lock);
                }
                m_callerWaits = false;
                if (!m_numberUnfinished)
                    return;
                step = due.takeFirst();
            }
            runStep(step);
        }
    }

private:
    struct Step {
        WTF::Function<void()> run;
        unsigned numberAwaited;
        TaskPriority priority;
        Vector<StepID, 4> followers;
    };

    explicit ResultSteps(const ParallelLoops& loops)
        : m_loops(loops)
    {
    }

    void runStep(StepID id)
    {
        m_steps[id].run();
        Locker locker { m_lock };
        --m_numberUnfinished;
        std::array<size_t, numberOfPriorities> numberDue { };
        for (StepID follower : m_steps[id].followers) {
            if (--m_steps[follower].numberAwaited)
                continue;
            dueOf(m_steps[follower].priority).append(follower);
            ++numberDue[static_cast<size_t>(m_steps[follower].priority)];
        }
        if (!m_numberUnfinished || (numberDue[static_cast<size_t>(TaskPriority::Foreground)] && m_callerWaits))
            m_hasChanged.notifyAll();
        for (TaskPriority priority : { TaskPriority::Foreground, TaskPriority::Background }) {
            size_t numberTaken = (priority == m_steps[id].priority) + (priority == TaskPriority::Foreground && m_callerWaits);
            if (numberDue[static_cast<size_t>(priority)] > numberTaken)
                askForThreads(priority, numberDue[static_cast<size_t>(priority)] - numberTaken);
        }
    }

    void runOnThreadOfPool(TaskPriority priority)
    {
        for (;;) {
            StepID step;
            {
                Locker locker { m_lock };
                Deque<StepID>& due = dueOf(priority);
                if (due.size() <= static_cast<size_t>(priority == TaskPriority::Foreground && m_callerWaits))
                    return;
                step = due.takeFirst();
            }
            runStep(step);
        }
    }

    void askForThreads(TaskPriority priority, size_t number) WTF_REQUIRES_LOCK(m_lock)
    {
        for (size_t index = 0; index < number; ++index) {
            ref();
            m_loops.schedule(priority, [](void* steps) {
                adoptRef(*static_cast<ResultSteps*>(steps))->runOnThreadOfPool(ParallelLoops::currentPriority());
            }, this);
        }
    }

    static constexpr size_t numberOfPriorities = 2;
    Deque<StepID>& dueOf(TaskPriority priority) WTF_REQUIRES_LOCK(m_lock) { return m_due[static_cast<size_t>(priority)]; }

    const ParallelLoops& m_loops;
    Vector<Step> m_steps;
    Lock m_lock;
    WTF::Condition m_hasChanged;
    std::array<Deque<StepID>, numberOfPriorities> m_due WTF_GUARDED_BY_LOCK(m_lock);
    size_t m_numberUnfinished WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    bool m_callerWaits WTF_GUARDED_BY_LOCK(m_lock) { false };
};

} // anonymous namespace

FlowResults analyzeValueFlow(Program& program, TypeTable& types, BuildContext& build, const ListenerForSetsOutsideTypes* setsOutsideTypes)
{
    RELEASE_ASSERT(&types == &program.types);
    RELEASE_ASSERT(program.form() == IRForm::AsRead);

    PhaseTimes times(build, PassID::AnalyzeFlow);
    if (build.options().admitsUnseenCode())
        return FlowResults(createUnknownResultTables(program, build));

    bool runsEveryRuleOnceMore = build.options().verifiesAfterEveryStage || build.options().checksFlowFixpoint;
    ReportSet reports = build.options().reports;
    bool showsCauses = setsOutsideTypes || hasFlag(reports, ReportSet::Flow) || hasFlag(reports, ReportSet::Functions) || hasFlag(reports, ReportSet::Arrivals);
    auto solution = makeUnique<ValueFlowSolution>(program, build, runsEveryRuleOnceMore ? ValueFlowSolver::Rounds::Due : ValueFlowSolver::Rounds::DueWithoutCheck, ValueFlowSolution::Witnesses::None, UINT32_MAX, EnumSet<ValueFlowUnknownSource> { },
        showsCauses ? ValueFlowSolution::WaysOfValues::AreAskedFor : ValueFlowSolution::WaysOfValues::AreNotAskedFor, ValueFlowSolution::CoveredMembers::AreLeftOut);
    times.didFinish("solve"_s);
    RELEASE_ASSERT(solution->hasResult());
    if (build.options().checksFlowFixpoint)
        reportRulesThatWereNotDue(solution->solver(), build);

    FlowResultTables tables;
    ValueFlowResultInputs inputs { program, *solution, build };
    ValueFlowLayoutsOfSites layoutsOfSites;
    ValueFlowClassesOfBuiltinResults classesOfBuiltinResults;
    std::unique_ptr<ValueFlowHeapIndex> heapIndex;
    std::unique_ptr<ValueFlowNodesAskedFor> nodesAskedFor;

    Vector<ValueFlowWriteUnderNewName> writesUnderNewNames;
    ValueFlowStampedCalls calls;
    BitVector symbolsCreatedOnce;
    std::unique_ptr<ValueFlowUsesOfHeap, ValueFlowUsesOfHeapDeleter> usesOfHeap;
    std::unique_ptr<ValueFlowAccessesThroughObjects, ValueFlowAccessesThroughObjectsDeleter> accessesThroughObjects;
    std::unique_ptr<ValueFlowOperationsOnLocations, ValueFlowOperationsOnLocationsDeleter> operationsOnLocations;
    ValueFlowVariablesOnObjects variablesOnObjects;
    std::unique_ptr<ValueFlowNumbersOfValues> numbersOfValues;
    ValueFlowConstructions constructions;
    ValueFlowFieldContents fieldContents;
    std::unique_ptr<ValueFlowWritesToContainers, ValueFlowWritesToContainersDeleter> writesToContainers;
    BitVector layoutsOfDictionaries;
    ValueFlowContainerResults containers;
    Vector<PropertyKeyKind> kindsOfKeys;

    Ref<ResultSteps> steps = ResultSteps::create(build.parallelLoops());
    auto add = [&](ASCIILiteral name, std::initializer_list<ResultSteps::StepID> awaited, WTF::Function<void(PhaseTimes&)>&& run, TaskPriority priority = TaskPriority::Foreground) {
        return steps->add(awaited, [&build, name, run = WTF::move(run)] {
            PhaseTimes timesOfStep(build, PassID::AnalyzeFlow);
            run(timesOfStep);
            timesOfStep.didFinish(name);
        }, priority);
    };

    auto indexOfHeap = add("index of the heap"_s, { }, [&](PhaseTimes&) {
        heapIndex = makeUnique<ValueFlowHeapIndex>(inputs);
        ValueFlowResultInputs inputsWithIndex = inputs;
        inputsWithIndex.heapIndex = heapIndex.get();
        ValueFlowHeapView heap(inputsWithIndex);
        heapIndex->findOperandsOfCalls(inputsWithIndex, heap);
    });
    auto askedFor = add("nodes asked for"_s, { }, [&](PhaseTimes&) {
        nodesAskedFor = makeUnique<ValueFlowNodesAskedFor>(inputs);
    });
    auto unknownResults = add("unknown results"_s, { }, [&](PhaseTimes&) {
        tables = createUnknownResultTables(program, build);
        tables.sites.accessSiteLosses.grow(tables.application->accessSites.numberOfSites());
    });
    auto classesOfResults = add("classes of what built-ins answer"_s, { }, [&](PhaseTimes&) {
        classesOfBuiltinResults = computeClassesOfBuiltinResults(inputs);
    });
    auto layoutsPicked = add("layouts that allocations pick"_s, { }, [&](PhaseTimes&) {
        layoutsOfSites = findLayoutsOfSites(inputs);
    });
    auto firstResults = steps->add({ indexOfHeap, askedFor, unknownResults, classesOfResults, layoutsPicked }, [&] {
        inputs.heapIndex = heapIndex.get();
        inputs.nodesAskedFor = nodesAskedFor.get();
        inputs.classesOfBuiltinResults = &classesOfBuiltinResults;
        inputs.layoutsOfSites = &layoutsOfSites;
    });
    if (build.options().verifiesProgram) {
        add("operations without a value"_s, { firstResults }, [&](PhaseTimes&) {
            reportOperationsWithoutValue(inputs);
        });
    }
    auto stampedCalls = add("calls"_s, { firstResults }, [&](PhaseTimes&) {
        calls = computeCallResults(inputs, types, tables);
    });
    auto usesOfTheHeap = add("uses of the heap"_s, { stampedCalls }, [&](PhaseTimes&) {
        usesOfHeap = computeUsesOfHeap(inputs);
    });
    auto executions = add("executions"_s, { stampedCalls }, [&](PhaseTimes& timesOfStep) {
        computeFunctionResults(inputs, calls, tables);
        computeSinkOperandResults(inputs, calls, tables);
        timesOfStep.didFinish("functions"_s);
        computeExecutionResults(inputs, calls, tables);
        symbolsCreatedOnce = computeCreatedSymbolResults(inputs, tables);
        computeClosuresMadeOnce(inputs, tables);
        inputs.symbolsCreatedOnce = &symbolsCreatedOnce;
        for (unsigned index = 0; index < inputs.program.types.numberOfPropertyKeys(); ++index) {
            if (inputs.program.at(PropertyKeyID(index)).kind == PropertyKeyKind::CreatedSymbol && !symbolsCreatedOnce.get(index))
                tables.wholeProgram.keysOfSeveralSymbols.append(PropertyKeyID(index));
        }
    });
    auto accessesCollected = add("variables on objects: accesses"_s, { executions }, [&](PhaseTimes&) {
        accessesThroughObjects = collectAccessesThroughObjects(inputs, calls);
    });
    auto operationsNoted = add("locations: operations"_s, { executions }, [&](PhaseTimes&) {
        operationsOnLocations = noteOperationsOnLocations(inputs, tables);
    });
    auto variables = add("variables on objects"_s, { accessesCollected, usesOfTheHeap }, [&](PhaseTimes&) {
        variablesOnObjects = computeVariablesOnObjects(inputs, *accessesThroughObjects, *usesOfHeap);
        accessesThroughObjects = nullptr;
        inputs.variablesOnObjects = &variablesOnObjects;
    });
    auto locations = add("locations"_s, { variables, operationsNoted }, [&](PhaseTimes&) {
        computeLocationResults(inputs, *operationsOnLocations, tables);
        operationsOnLocations = nullptr;
    });
    auto numbersPrepared = add("numbers: prepared"_s, { executions }, [&](PhaseTimes&) {
        ValueFlowHeapView heap(inputs);
        numbersOfValues = prepareNumbersOfPositions(inputs, heap, tables);
    });
    auto numbers = add("numbers"_s, { numbersPrepared, locations }, [&](PhaseTimes&) {
        computeNumbersOfPositions(*numbersOfValues, tables);
        inputs.numbersOfValues = numbersOfValues.get();
    });
    auto storesListed = add("fields: stores listed"_s, { usesOfTheHeap, executions }, [&](PhaseTimes&) {
        listStoresByLayout(inputs, *usesOfHeap, tables);
    });
    auto objects = add("objects"_s, { executions }, [&](PhaseTimes&) {
        computeObjectResults(inputs, calls, tables);
    });
    auto constructionOrders = add("constructions"_s, { executions }, [&](PhaseTimes&) {
        ValueFlowHeapView heap(inputs);
        constructions = computeConstructions(inputs, heap, tables);
    });
    auto classes = add("classes"_s, { locations }, [&](PhaseTimes&) {
        computeClassResults(inputs, tables);
    });
    auto order = add("order"_s, { locations }, [&](PhaseTimes&) {
        computeOrderResults(inputs, calls, tables);
    }, TaskPriority::Background);
    auto builtins = add("built-ins"_s, { stampedCalls }, [&](PhaseTimes&) {
        writesUnderNewNames = computeBuiltinResults(inputs, tables);
    }, TaskPriority::Background);
    auto closuresNeverCompared = add("closures never compared"_s, { storesListed }, [&](PhaseTimes&) {
        ValueFlowHeapView heap(inputs);
        computeClosuresNeverCompared(inputs, heap, tables);
        computeLayoutsNeverHeldWeakly(inputs, heap, *usesOfHeap, tables);
    }, TaskPriority::Background);
    auto fields = add("fields"_s, { numbers, objects, constructionOrders, classes, storesListed, builtins }, [&](PhaseTimes&) {
        findHoldersWithListedKeys(inputs, *usesOfHeap, tables.builtins);
        fieldContents = computeFieldResults(inputs, *usesOfHeap, constructions, tables);
        kindsOfKeys = Vector<PropertyKeyKind>(program.types.numberOfPropertyKeys(), [&](size_t index) {
            return program.at(PropertyKeyID(static_cast<uint32_t>(index))).kind;
        });
    });
    auto writes = add("writes to containers"_s, { numbers, builtins }, [&](PhaseTimes&) {
        writesToContainers = collectWritesToContainers(inputs, tables, types);
    }, TaskPriority::Background);
    auto literalKeyOrders = add("literal key orders"_s, { fields }, [&](PhaseTimes&) {
        computeLiteralKeyOrders(inputs, heapViewOf(*usesOfHeap), WTF::move(kindsOfKeys), tables);
    });
    auto contents = add("contents"_s, { fields, writes, order, closuresNeverCompared }, [&](PhaseTimes& timesOfStep) {
        layoutsOfDictionaries = computeLayoutsOfDictionaries(inputs, types, tables);
        inputs.layoutsOfDictionaries = &layoutsOfDictionaries;
        inputs.typesBehindDictionaries = tables.application->typesBehindDictionaries.span();
        inputs.layoutSetsNamingDictionaries = &tables.application->layoutSetsNamingDictionaries;
        timesOfStep.didFinish("dictionaries"_s);
        containers = computeContainerResults(*writesToContainers);
        writesToContainers = nullptr;
        timesOfStep.didFinish("contents: types of contents"_s);
        inputs.containers = &containers;
        stampAllocationsOfBuiltinInstances(inputs, containers, tables);
        computeDataNodeResults(inputs, containers, types, tables);
        computeTypedResultResults(inputs, tables);
        orderAllocationSites(inputs.loops(), tables);
    });
    auto stampsOfAccesses = add("accesses: stamps"_s, { contents }, [&](PhaseTimes& timesOfStep) {
        computeTypeResults(inputs, fieldContents, types, tables, setsOutsideTypes);
        timesOfStep.didFinish("types: positions"_s);
        computeLayoutsNeverStoredOtherTypes(inputs, fieldContents, types, tables);
        computeSymbolConstantResults(inputs, tables);
        timesOfStep.didFinish("types"_s);
        computeAccessResults(inputs, types, tables);
    });
    add("accesses"_s, { stampsOfAccesses, literalKeyOrders }, [&](PhaseTimes&) {
        computeStoreNamingResults(inputs, *usesOfHeap, tables);
        build.destroyLater(WTF::move(usesOfHeap));
    });
    steps->run();
    tables.application->accessSites.valueOfSite = { };
    inputs.heapIndex = nullptr;
    build.destroyLater(WTF::move(heapIndex));
    inputs.nodesAskedFor = nullptr;
    build.destroyLater(WTF::move(nodesAskedFor));
    times.didFinish("the results behind the rounds"_s);
    recordLockedWritesUnderNewNames(program, types, writesUnderNewNames.span(), tables);

    inputs.numbersOfValues = nullptr;
    build.destroyLater(WTF::move(numbersOfValues));
    build.destroyLater(WTF::move(containers));
    build.destroyLater(WTF::move(fieldContents));
    build.destroyLater(WTF::move(constructions));
    build.destroyLater(WTF::move(variablesOnObjects));
    build.destroyLater(WTF::move(solution));
    return FlowResults(WTF::move(tables));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
