/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowTypedResults.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTCFG.h"
#include "AOTFlowRecords.h"
#include "AOTFlowSiteData.h"
#include "AOTFlowStamps.h"
#include "AOTSelectTypedResultsTargets.h"
#include "AOTSelectTypedResultsTypeNodes.h"
#include "AOTSort.h"
#include "AOTStringArena.h"
#include "AOTTypeArena.h"
#include "AOTUseLists.h"
#include "AOTValueFlowPositions.h"
#include "AOTValueFlowResultsInternal.h"
#include "AOTValueFlowRules.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

TypeID typedElementTypeOf(const Program& program, const ClassPart& array)
{
    return array.contentsAt(program.builtins(), ContentsSlot::Element);
}

void followTypes(const Program& program, ValueFlowTypedResult& result)
{
    HashSet<TypeID> visitedTypes;
    BitVector isNamed(result.layouts.size());
    Vector<TypeID, 8> worklist;
    auto visit = [&](TypeID type) {
        if (type && visitedTypes.add(type).isNewEntry)
            worklist.append(type);
    };

    visit(result.type);
    while (!worklist.isEmpty()) {
        TypeID id = worklist.takeLast();
        const Type& type = program.at(id);
        if (const ClassPart* array = type.findClassPart(BuiltinClasses::Array)) {
            result.typesWithArrays.append(id);
            visit(typedElementTypeOf(program, *array));
        }
        std::span<const LayoutID> members = membersOf(program, type);
        for (size_t index = 0; index < result.layouts.size() && !members.empty(); ++index) {
            LayoutID layout = result.layouts[index];
            if (isNamed.get(index) || !std::ranges::binary_search(members, layout))
                continue;
            isNamed.set(index);
            for (const Field& field : program.at(layout).fields) {
                if (field.key)
                    visit(expectationOf(field.declared));
            }
        }
    }
    AOT::sort(result.typesWithArrays);

    for (size_t index = 0; index < result.layouts.size(); ++index) {
        if (!isNamed.get(index))
            result.layoutsAmongPlainData.append(result.layouts[index]);
    }
}

bool answersPromise(const BuiltinTable& builtins, const BuiltinOverload& overload)
{
    return std::ranges::any_of(builtins.at(overload.result.type).parts, [](const BuiltinTypePart& part) {
        return part.kind == BuiltinTypePartKind::Instance && part.builtinClass == BuiltinClasses::Promise;
    });
}

ValueFlowTypedResult typedResultOf(const Program& program, ExpectedResultID id, uint64_t mostObjects)
{
    const ExpectedResult& expected = program.at(id);
    ValueFlowTypedResult result;
    result.type = expected.type;
    result.layouts.appendRange(expected.layouts.begin(), expected.layouts.end());
    AOT::sort(result.layouts);
    result.layouts.shrink(std::ranges::unique(result.layouts).begin() - result.layouts.begin());
    if (result.type && result.numberOfObjects() <= mostObjects)
        followTypes(program, result);
    return result;
}

} // anonymous namespace

static ValueFlowTypedResult findTypedResult(const Program& program, const ValueFlowBuiltinTables& tables, const Function& function, const UseLists& uses, ValueID call)
{
    const Value& operation = function.at(call);
    if (operation.opcode != Opcode::Call)
        return { };

    uint64_t mostObjects = tables.mostObjectsOfTypedResult();
    ValueFlowTypedResult result;
    if (ExpectedResultID id = operation.expectedResult())
        result = typedResultOf(program, id, mostObjects);
    else {
        TypedResultTarget target = findTypedResultTarget(program, function, uses, call, mostObjects, &tables.headLayoutsOfFamilies());
        result.type = target.type;
        result.layouts = WTF::move(target.layouts);
        if (result.type && result.numberOfObjects() <= mostObjects)
            followTypes(program, result);
    }
    if (!result.type || result.numberOfObjects() > mostObjects)
        return { };
    return result;
}

static bool mayCallFunctionWithTypedResult(const Program& program, const ValueFlowBuiltinTables& tables, const Function& function, const Value& call)
{
    if (PropertyKeyID key = call.calleeKey()) {
        return std::ranges::any_of(tables.namesOfFunctionsWithTypedResults(), [&](BuiltinName name) {
            return program.keyOf(name) == key;
        });
    }
    const Value& callee = function.at(sameObjectThroughCasts(function, call.operand(0)));
    return callee.opcode == Opcode::Builtin && tables.hasTypedResult(callee.builtin());
}

void appendSitesOfTypedResults(const Program& program, const ValueFlowBuiltinTables& tables, const Function& function, const CFG& cfg, Vector<ValueFlowSite>& sites)
{
    BitVector candidates;
    for (uint32_t index = 0; index < function.values.size(); ++index) {
        const Value& value = function.values[index];
        if (value.opcode == Opcode::Call && mayCallFunctionWithTypedResult(program, tables, function, value))
            candidates.set(index);
    }
    if (candidates.isEmpty())
        return;

    UseLists uses(function, cfg);
    for (size_t index : candidates) {
        ValueID call(static_cast<uint32_t>(index));
        ValueFlowTypedResult result = findTypedResult(program, tables, function, uses, call);
        if (!result)
            continue;
        for (size_t layout = 0; layout < result.layouts.size(); ++layout)
            sites.append({ ValueFlowSiteOrigin::TypedResult, call, result.ordinalOfLayout(layout), result.layouts[layout], { } });
        for (size_t array = 0; array <= result.typesWithArrays.size(); ++array)
            sites.append({ ValueFlowSiteOrigin::TypedResult, call, result.ordinalOfArray(array), { }, BuiltinClasses::Array });
    }
}

void allocateTypedResult(const BuiltinOverload& overload, const ValueFlowStatement& call, const ValueFlowBuiltinCall& description, ValueFlowObjectID promise, ValueSetID plainDataOfCall, ValueFlowRuleContext& context)
{
    if (call.kind != ValueFlowStatementKind::Operation || call.opcode != Opcode::Call || description.kind != ValueFlowCallKind::Call)
        return;

    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    const Function& function = program.at(call.function);
    ExpectedResultID expected = function.at(call.site).expectedResult();
    TypedResultForm form = overload.result.typedResultForm;
    if (form == TypedResultForm::None || !universe.siteObject(call, ValueFlowTypedResult::firstOrdinal))
        return;

    ValueFlowNodeID target = description.result;
    if (!expected && form != TypedResultForm::ParsedText && answersPromise(program.builtins(), overload)) {
        if (!promise)
            return;
        target = context.written(ClimbingSet(context.maker().of(promise)), { ValueFlowNameKind::Slot, BuiltinSlots::PromiseValue.index() });
    }

    CFG cfg(function);
    ValueFlowTypedResult result = findTypedResult(program, universe.builtinTables(), function, UseLists(function, cfg), call.site);
    RELEASE_ASSERT(result);

    auto objectAt = [&](uint32_t ordinal) {
        ValueFlowObjectID object = universe.siteObject(call, ordinal);
        RELEASE_ASSERT(object);
        return object;
    };
    auto only = [&](uint32_t ordinal) {
        return ClimbingSet(context.maker().of(objectAt(ordinal)));
    };
    auto plainData = [&]() -> ValueSetID {
        return plainDataOfCall ? plainDataOfCall : unknownValue(context, ValueFlowUnknownSource::DataFromOutside);
    };
    auto valueOf = [&](TypeID id) -> ValueSetID {
        if (!id)
            return plainData();
        const Type& type = program.at(id);
        if (!type.isJavaScriptValue() || type.isAnything())
            return plainData();

        Vector<ValueFlowObjectID, 16> members;
        for (uint16_t rest = type.primitives & ~(Primitive::LiteralString | Primitive::LiteralSymbol); rest; rest &= rest - 1)
            members.append(ValueFlowObjectID(static_cast<uint32_t>(std::countr_zero(rest))));
        if (type.primitives & Primitive::AnyString)
            members.append(ValueFlowUniverse::primitive(PrimitiveSet::String));
        if (type.primitives & Primitive::AnySymbol)
            members.append(ValueFlowUniverse::primitive(PrimitiveSet::Symbol));

        bool admitsPlainObjects = type.anyObject || type.anyFunction || !type.functions.isEmpty() || !type.builtins.isEmpty() || (type.layouts && program.at(type.layouts).hasMembersWithoutLayout);
        std::span<const LayoutID> layoutsOfType = membersOf(program, type);
        for (size_t index = 0; index < result.layouts.size() && !layoutsOfType.empty(); ++index) {
            if (std::ranges::binary_search(layoutsOfType, result.layouts[index]))
                members.append(objectAt(result.ordinalOfLayout(index)));
        }
        for (const ClassPart& part : type.classes) {
            auto listed = std::ranges::lower_bound(result.typesWithArrays, id);
            if (part.builtinClass != BuiltinClasses::Array || listed == result.typesWithArrays.end() || *listed != id) {
                admitsPlainObjects = true;
                continue;
            }
            members.append(objectAt(result.ordinalOfArray(listed - result.typesWithArrays.begin())));
            members.append(objectAt(result.ordinalOfPlainArray()));
        }

        ValueSetID value = context.maker().of(members.span());
        return admitsPlainObjects ? context.maker().unite(value, context.maker().intersect(plainData(), universe.objects())) : value;
    };
    auto fillField = [&](ClimbingSet object, const Field& field) {
        ValueFlowNodeID node = context.written(object, program.isIndexLike(field.key) ? elementsName : ValueFlowName::property(field.key));
        context.add(node, valueOf(expectationOf(field.declared)));
        if (!field.declared.written() || program.at(field.declared.expected().predicted()).primitives & Primitive::Undefined)
            context.add(node, universe.undefinedValue());
    };

    for (size_t index = 0; index < result.layouts.size(); ++index) {
        ClimbingSet object = only(result.ordinalOfLayout(index));
        const Layout& layout = program.at(result.layouts[index]);
        for (const Field& field : layout.fields) {
            if (field.key)
                fillField(object, field);
        }
        HashSet<LayoutID> visited;
        for (LayoutID base = predictedHeritageOf(layout); base && visited.add(base).isNewEntry; base = predictedHeritageOf(program.at(base))) {
            for (const Field& field : program.at(base).fields) {
                if (field.key && !layout.findField(field.key))
                    fillField(object, field);
            }
        }
        context.add(context.written(object, unknownKeyName), plainData());
        context.mark(object, ObjectMutation::AddProperty);
    }
    for (size_t index = 0; index < result.typesWithArrays.size(); ++index) {
        const ClassPart* array = program.at(result.typesWithArrays[index]).findClassPart(BuiltinClasses::Array);
        context.add(context.written(only(result.ordinalOfArray(index)), elementsName), valueOf(typedElementTypeOf(program, *array)));
    }
    context.add(context.written(only(result.ordinalOfPlainArray()), elementsName), plainData());

    ClimbingSet plainObjects = plainDataOfCall ? context.unite(ClimbingSet(context.maker().intersect(plainDataOfCall, universe.objects())), only(result.ordinalOfPlainArray())) : ClimbingSet();
    for (LayoutID layout : result.layoutsAmongPlainData) {
        size_t index = std::ranges::lower_bound(result.layouts, layout) - result.layouts.begin();
        if (!plainDataOfCall) {
            context.add(ValueFlowProgramNodes::escaped(), only(result.ordinalOfLayout(index)));
            continue;
        }
        for (ValueFlowName name : { unknownKeyName, elementsName })
            context.add(context.written(plainObjects, name), only(result.ordinalOfLayout(index)));
    }
    if (target)
        context.add(target, valueOf(result.type));
}

void computeTypedResultResults(const ValueFlowResultInputs& inputs, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    FlowApplication& application = *tables.application;

    inputs.sets().forEachMember(inputs.solver().possibleValuesOf(inputs.bodies().nodes().ofHost(ValueFlowHostNode::RecordsMadeByParsers)), [&](ValueFlowObjectID record) {
        application.layouts[universe.layoutOf(record).index()].flow.observedUses.add(ObservedUse::ReachesTypedParse);
    });

    auto parsesTypedResult = [&](const ValueFlowStatement& statement) {
        return statement.kind == ValueFlowStatementKind::Operation && statement.opcode == Opcode::Call && universe.siteObject(statement, ValueFlowTypedResult::firstOrdinal);
    };
    Vector<const ValueFlowStatement*> calls;
    for (uint32_t index : findIndicesInParallel(inputs.loops(), program.functions.size(), [&](unsigned index) {
        return inputs.isReached(FunctionID(index)) && std::ranges::any_of(inputs.bodies().statementsOf(index), parsesTypedResult);
    })) {
        for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(index)) {
            if (parsesTypedResult(statement))
                calls.append(&statement);
        }
    }
    if (calls.isEmpty())
        return;

    RELEASE_ASSERT(inputs.heapIndex);
    Vector<ValueFlowHeapIndex::Stretch> stretches = inputs.heapIndex->stretchesOfNodesWrittenUnder([](ValueFlowName name) {
        return name.kind == ValueFlowNameKind::Elements;
    });
    BitVector isFilled;
    for (uint32_t object : collectInParallel<uint32_t>(inputs.loops(), stretches.size(), [&](unsigned stretch, Vector<uint32_t>& found) {
        for (const ValueFlowHeapIndex::WrittenNode& node : stretches[stretch].nodes) {
            if (ValueSets::isComplement(node.objects))
                continue;
            ValueFlowObjectID object = inputs.sets().onlyMember(node.objects);
            const ValueFlowSite* site = object ? universe.siteOf(object) : nullptr;
            if (site && site->origin == ValueFlowSiteOrigin::TypedResult)
                found.append(object.index());
        }
    }))
        isFilled.set(object);
    size_t numberOfOtherSites = tables.sites.allocationSites.size();
    tables.sites.typedResultSites.fill(AllocationSiteID(), program.layouts.size());
    FunctionID functionOfAccessSites;
    Vector<AccessSiteID> accessSites;
    FunctionID functionOfUses;
    std::optional<CFG> cfg;
    std::optional<UseLists> uses;
    for (const ValueFlowStatement* call : calls) {
        const Function& function = program.at(call->function);
        if (functionOfUses != call->function) {
            functionOfUses = call->function;
            cfg.emplace(function);
            uses.emplace(function, *cfg);
        }
        ValueFlowTypedResult result = findTypedResult(program, universe.builtinTables(), function, *uses, call->site);
        RELEASE_ASSERT(result);
        ValueFlowObjectID plainArray = universe.siteObject(*call, result.ordinalOfPlainArray());
        if (!isFilled.get(plainArray.index()))
            continue;
        // A call whose callee has become unknown since no longer fills its objects, and nothing else says that they exist.
        bool isFilledByLastRun = false;
        ValueFlowStatementID id(static_cast<uint32_t>(call - inputs.bodies().statements().data()));
        inputs.forEachMadeNodeAskedForBy(id, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Written }), false, [&](const ValueFlowNodeKey& key, ValueFlowStatementID) {
            isFilledByLastRun |= key.kind == ValueFlowMadeNodeKind::Written && key.name().kind == ValueFlowNameKind::Elements && !ValueSets::isComplement(key.set) && inputs.sets().onlyMember(key.set) == plainArray;
        });
        if (!isFilledByLastRun)
            continue;

        if (functionOfAccessSites != call->function) {
            functionOfAccessSites = call->function;
            accessSites = accessSitesByValue(function, application.accessSites, call->function);
        }
        if (AccessSiteID accessSite = accessSites[call->site.index()])
            tables.sites.typedResultCalls.append({ accessSite, result.type, result.layouts });

        for (LayoutID layout : result.layouts) {
            application.layouts[layout.index()].flow.observedUses.add({ ObservedUse::ReachesTypedParse, ObservedUse::IsParsedByType });
            const Vector<AllocationClassID>& classes = application.layouts[layout.index()].flow.allocationClasses;
            AllocationSiteID& site = tables.sites.typedResultSites[layout.index()];
            if (site || classes.isEmpty())
                continue;
            site = AllocationSiteID(tables.sites.allocationSites.size());
            tables.sites.allocationSites.append(AllocationSite { call->function, layout, { }, classes[0], function.locationOf(call->site) });
        }
    }
    AOT::sort(tables.sites.typedResultCalls, { }, &FlowTypedResultCall::call);
    inputs.build.statistics().count(StatisticID::FlowAllocationSites, tables.sites.allocationSites.size() - numberOfOtherSites);
}

static constexpr OptionSet<ObjectMutation> mutationsOfArraysWithoutMark { ObjectMutation::ChangePrototype, ObjectMutation::BecomePrototype, ObjectMutation::StoreOfOtherType };

void computeDataNodeResults(const ValueFlowResultInputs& inputs, const ValueFlowContainerResults& containers, TypeTable& types, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    FlowApplication& application = *tables.application;
    unsigned indexOfElements = indexOfArrayElementsInType(program.builtins());
    size_t numberOfOtherSites = tables.sites.allocationSites.size();
    std::array<AllocationClassID, 2> classesOfArrays;
    size_t numberOfOtherNodes = tables.sites.dataNodes.size();
    ValueFlowPositionCounts counts(nullptr);

    struct Node {
        ValueID operation;
        uint32_t ordinal;
        LayoutID layout;
        BuiltinClassID builtinClass;
        AllocationClassID classOfLayout;
        OptionSet<ObjectMutation> markedMutations;
        bool neverChanges;
        TypeID elements;
        TypeID type;
        ArrayStorage arrayStorage { ArrayStorage::Other };
    };
    const ParallelLoops& loops = inputs.loops();
    Vector<uint32_t> functions = findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        return inputs.isReached(FunctionID(index)) && std::ranges::any_of(inputs.bodies().statementsOf(index), [](const ValueFlowStatement& statement) {
            return statement.kind == ValueFlowStatementKind::Operation && statement.opcode == Opcode::Data;
        });
    });
    TypeInterningPhase phase(types, functions.size());
    Vector<Vector<Node>> nodesOfFunction(functions.size());
    forEachIndexInParallel(loops, functions.size(), [&](unsigned task) {
        uint32_t index = functions[task];
        const Function& function = program.functions[index];
        TypeArena arena(phase, task);
        std::optional<ValueFlowPositions> positions;
        for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(index)) {
            if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::Data)
                continue;
            forEachDataNodeInPreorder(program.at(function.at(statement.site).data()).root, [&](uint32_t ordinal, const DataNode& dataNode) {
                if (!dataNode.is<DataNode::Array>() && !dataNode.is<DataNode::Object>())
                    return;
                ValueFlowObjectID object = universe.siteObject(statement, ordinal);
                RELEASE_ASSERT(object);
                const ValueFlowSite& site = *universe.siteOf(object);
                Node node { statement.site, ordinal, site.layout, site.builtinClass, { }, { }, false, { }, { } };
                if (site.layout) {
                    const Vector<AllocationClassID>& classes = application.layouts[site.layout.index()].flow.allocationClasses;
                    if (!classes.isEmpty())
                        node.classOfLayout = classes[0];
                } else {
                    node.markedMutations = markedMutationsOf(inputs, object);
                    node.neverChanges = neverChangesOnceMade(inputs, object);
                    const ValueFlowContainerFacts* facts = containers.find(object);
                    if (facts && indexOfElements < facts->contents.size() && facts->contents[indexOfElements] != TypeTable::never)
                        node.elements = facts->contents[indexOfElements];
                    if (facts)
                        node.arrayStorage = facts->arrayStorageToldToCreator;
                    if (facts && ordinal && (facts->contentsAreProven || !facts->pristine.isEmpty())) {
                        if (!positions)
                            positions.emplace(inputs, arena, counts, index);
                        node.type = arena.intern(positions->typeOfObject(object));
                    }
                }
                nodesOfFunction[task].append(node);
            });
        }
    });

    auto classOfArray = [&](const Node& node) -> AllocationClassID& {
        return classesOfArrays[node.markedMutations.contains(ObjectMutation::StoreAfterCreation)];
    };
    Vector<uint32_t> firstSiteOfFunction;
    firstSiteOfFunction.reserveInitialCapacity(functions.size());
    uint32_t numberOfSites = static_cast<uint32_t>(numberOfOtherSites);
    for (const Vector<Node>& nodes : nodesOfFunction) {
        firstSiteOfFunction.append(numberOfSites);
        for (const Node& node : nodes) {
            uint32_t site = numberOfSites++;
            if (node.layout)
                continue;
            AllocationClassID& classOfArrays = classOfArray(node);
            if (!classOfArrays) {
                classOfArrays = AllocationClassID(tables.sites.allocationClasses.size());
                AllocationClass arrays;
                arrays.builtinClass = BuiltinClasses::Array;
                arrays.impossibleMutations = allObjectMutations - mutationsOfArraysWithoutMark;
                tables.sites.allocationClasses.append(WTF::move(arrays));
            }
            tables.sites.allocationClasses[classOfArrays.index()].impossibleMutations.remove(node.markedMutations);
            if (node.neverChanges && inputs.build.isEnabled(FlowResultKind::HolderMutations, node.builtinClass.index()))
                tables.sites.sitesOfObjectsThatNeverChange.set(site);
        }
    }
    tables.sites.allocationSites.grow(numberOfSites);
    tables.sites.dataNodes.grow(numberOfOtherNodes + (numberOfSites - numberOfOtherSites));
    TypeRemap remap = phase.finish(StringRemap { }, &loops);
    forEachIndexInParallel(loops, functions.size(), [&](unsigned task) {
        uint32_t index = functions[task];
        const Function& function = program.functions[index];
        Vector<FlowAllocationStamp>& stamps = application.bodies[index].allocations;
        size_t numberOfOtherStamps = stamps.size();
        uint32_t nextSite = firstSiteOfFunction[task];
        ValueID operation;
        AllocationSiteID rootSite;
        for (const Node& node : nodesOfFunction[task]) {
            if (node.operation != operation) {
                operation = node.operation;
                rootSite = { };
            }
            FlowDataNodeStamp stamp { AllocationSiteID(nextSite++), node.layout ? node.classOfLayout : classOfArray(node), node.elements, node.arrayStorage, node.type };
            remap.apply(stamp.type);
            tables.sites.allocationSites[stamp.site.index()] = AllocationSite { FunctionID(index), node.layout, node.builtinClass, stamp.allocationClass, function.locationOf(node.operation) };
            if (!node.ordinal) {
                rootSite = stamp.site;
                stamps.append(FlowAllocationStamp { .value = node.operation, .site = rootSite, .allocationClass = stamp.allocationClass });
            }
            tables.sites.dataNodes[numberOfOtherNodes + (stamp.site.index() - numberOfOtherSites)] = { rootSite, node.ordinal, stamp };
        }
        if (stamps.size() != numberOfOtherStamps)
            AOT::sort(stamps, { }, &FlowAllocationStamp::value);
    });

    for (AllocationClassID classOfArrays : classesOfArrays) {
        if (!classOfArrays)
            continue;
        AllocationClass& arrays = tables.sites.allocationClasses[classOfArrays.index()];
        if (!inputs.build.isEnabled(FlowResultKind::HolderMutations, BuiltinClasses::Array.index()))
            arrays.impossibleMutations = { };
        inputs.build.statistics().count(StatisticID::FlowAllocationClasses, 1);
    }
    inputs.build.statistics().count(StatisticID::FlowAllocationSites, tables.sites.allocationSites.size() - numberOfOtherSites);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
