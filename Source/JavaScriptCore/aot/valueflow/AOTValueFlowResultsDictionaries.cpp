/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTCFG.h"
#include "AOTSchemaWalkers.h"
#include "AOTSort.h"
#include "AOTStringArena.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

static BitVector findLayoutSetsNaming(const Program& program, const BitVector& layouts, const ParallelLoops& loops)
{
    BitVector sets(program.types.numberOfLayoutSets());
    for (uint32_t index : findIndicesInParallel(loops, program.types.numberOfLayoutSets(), [&](unsigned index) {
        return std::ranges::any_of(program.at(LayoutSetID(index)).written(), [&](LayoutID member) { return layouts.quickGet(member.index()); });
    }))
        sets.quickSet(index);
    return sets;
}

namespace {

struct NamedLayoutFinder {
    void operator()(LayoutID id) const
    {
        if (id && layouts.quickGet(id.index()))
            found.append(id.index());
    }
    void operator()(LayoutSetID id) const
    {
        if (id && setsNamingThem.quickGet(id.index()))
            std::ranges::for_each(program.at(id).written(), *this);
    }
    void operator()(const auto&) const { }

    const Program& program;
    const BitVector& layouts;
    const BitVector& setsNamingThem;
    Vector<uint32_t>& found;
};

} // anonymous namespace

static bool mayHaveDictionaries(const Layout& layout, const FlowLayoutUpdate& update)
{
    bool isOfCreatedObject = layout.flags == OptionSet<LayoutFlags> { LayoutFlags::IsOfCreatedObject };
    if (layout.kind != (isOfCreatedObject ? LayoutKind::Instance : LayoutKind::Record) || !layout.staticFields.isEmpty() || layout.base || layout.constructor || layout.name || layout.computedKeyCount)
        return false;
    if (update.objectsArePrototypesOfCreatedObjects && layout.flags.isEmpty())
        return true;
    if (!layout.supertypes.isEmpty())
        return false;
    if (layout.flags == OptionSet<LayoutFlags> { LayoutFlags::IsWrittenUnderDictionaryType })
        return !update.flow.impossibleMutations.containsAll({ ObjectMutation::AddProperty, ObjectMutation::DeleteField });
    return layout.fields.isEmpty() && (layout.flags.isEmpty() || isOfCreatedObject) && update.appendedFields.isEmpty()
        && (isOfCreatedObject || !update.flow.impossibleMutations.contains(ObjectMutation::AddProperty));
}

static void listLiteralsThatKeepTheirSlots(const FlowApplication& application, const Function& function, bool functionRunsAtMostOnce, const NamedLayoutFinder& finder)
{
    auto listsMembers = [&](const Value& value) {
        return value.opcode == Opcode::NewObject && !value.fields().empty() && finder.layouts.quickGet(value.layout().index());
    };
    if (std::ranges::none_of(function.values, listsMembers))
        return;
    CFG cfg(function);
    BlockCycles cycles(cfg);
    for (BlockID block : cfg.reversePostOrder()) {
        bool runsAtMostOnce = functionRunsAtMostOnce && !cycles.isInCycle(block);
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            if (listsMembers(value) && (!runsAtMostOnce || !application.layouts[value.layout().index()].isHandedToObjectCreateAlone))
                finder(value.layout());
        }
    }
}

static void listLayoutsNamedIn(const Program& program, const FlowApplication& application, const Function& function, bool functionRunsAtMostOnce, const NamedLayoutFinder& finder)
{
    forEachReference(static_cast<const FunctionRecord&>(function), finder);
    listLiteralsThatKeepTheirSlots(application, function, functionRunsAtMostOnce, finder);
    auto listLayoutsTestedAlone = [&](TypeID tested) {
        Type rest = program.at(tested);
        LayoutSetID layouts = std::exchange(rest.layouts, LayoutSetID());
        if (!layouts || !rest.isNever())
            return;
        auto all = program.at(layouts).all();
        if (all && std::ranges::all_of(*all, [&](LayoutID member) { return finder.layouts.quickGet(member.index()); }))
            finder(layouts);
    };
    for (const Value& value : function.values) {
        if (!layoutOfPlainLiteral(program, value))
            forEachReference(value, finder);
        if (value.opcode == Opcode::Check)
            listLayoutsTestedAlone(value.expectedType().predicted());
        if (value.opcode == Opcode::TypeTest)
            listLayoutsTestedAlone(value.testedType());
    }
    for (const Block& block : function.blocks) {
        forEachReference(block.terminator, finder);
        if (block.terminator.kind != TerminatorKind::ReturnCall)
            continue;
        for (const TailCast& cast : block.terminator.casts()) {
            if (cast.resolution == CastResolution::Check)
                finder(program.at(cast.declared.written()).layouts);
        }
    }
}

static Type typeBehindDictionaries(const Program& program, const BitVector& layoutsOfDictionaries, const Type& type, TypeArena& types, const Invocable<TypeID(TypeID)> auto& behindOf, bool namesObjectInTheirPlace = true)
{
    Type behind = type;
    bool namesDictionaries = false;
    if (type.layouts) {
        Vector<LayoutID> members = program.at(type.layouts).written();
        namesDictionaries = members.removeAllMatching([&](LayoutID member) {
            return layoutsOfDictionaries.quickGet(member.index());
        });
        if (namesDictionaries)
            behind.layouts = members.isEmpty() ? LayoutSetID() : types.intern(LayoutSet::complete(WTF::move(members)));
    }
    for (ClassPart& part : behind.classes) {
        for (TypeID& contents : part.writtenContents)
            contents = behindOf(contents);
    }
    if (namesDictionaries && namesObjectInTheirPlace)
        behind = behind.unionWith(Type::builtinClass(BuiltinClasses::Object), types);
    behind.canonicalize();
    return behind;
}

Type typeBehindDictionaries(const Program& program, const BitVector& layoutsOfDictionaries, std::span<const std::pair<TypeID, TypeID>> ascendingTypes, const Type& type, TypeArena& types)
{
    return typeBehindDictionaries(program, layoutsOfDictionaries, type, types, [&](TypeID contents) {
        return typeBehindDictionaries(ascendingTypes, contents);
    });
}

static void createTypesBehindDictionaries(const Program& program, const BitVector& layoutsOfDictionaries, TypeTable& types, FlowApplication& application, const ParallelLoops& loops)
{
    application.layoutSetsNamingDictionaries = findLayoutSetsNaming(program, layoutsOfDictionaries, loops);
    unsigned numberOfTypes = types.size();
    Vector<TypeID> changed(numberOfTypes);
    auto isChanged = [&](TypeID type) {
        return type && changed[type.index()];
    };
    auto behindOf = [&](TypeID type) {
        return isChanged(type) ? changed[type.index()] : type;
    };
    auto namesDictionaries = [&](const Type& type) {
        return type.layouts && application.layoutSetsNamingDictionaries.quickGet(type.layouts.index());
    };
    Vector<uint32_t> typesThatMayChange = findIndicesInParallel(loops, numberOfTypes, [&](unsigned index) {
        const Type& type = program.at(TypeID(index));
        return namesDictionaries(type) || std::ranges::any_of(type.classes, [](const ClassPart& part) { return !part.writtenContents.isEmpty(); });
    });
    Vector<uint32_t> changedTypes;
    Vector<TypeID> testedBehind;
    TypeInterningPhase phase(types, 1);
    {
        TypeArena arena(phase, 0);
        for (bool didChange = true; std::exchange(didChange, false);) {
            for (uint32_t index : typesThatMayChange) {
                const Type& type = program.at(TypeID(index));
                if (!namesDictionaries(type) && !std::ranges::any_of(type.classes, [&](const ClassPart& part) { return std::ranges::any_of(part.writtenContents, isChanged); }))
                    continue;
                TypeID behind = arena.intern(typeBehindDictionaries(program, layoutsOfDictionaries, type, arena, behindOf));
                if (changed[index] == behind)
                    continue;
                if (!changed[index])
                    changedTypes.append(index);
                changed[index] = behind;
                didChange = true;
            }
        }
        AOT::sort(changedTypes);
        testedBehind = WTF::map(changedTypes, [&](uint32_t index) {
            return arena.intern(typeBehindDictionaries(program, layoutsOfDictionaries, program.at(TypeID(index)), arena, behindOf, false));
        });
    }
    TypeRemap remap = phase.finish(StringRemap { }, &loops);
    application.typesNamingDictionaries = BitVector(numberOfTypes);
    for (size_t position = 0; position < changedTypes.size(); ++position) {
        TypeID type(changedTypes[position]);
        application.typesBehindDictionaries.append({ type, remap(changed[type.index()]) });
        application.typesTestedBehindDictionaries.append({ type, remap(testedBehind[position]) });
        application.typesNamingDictionaries.quickSet(type.index());
    }
}

BitVector computeLayoutsOfDictionaries(const ValueFlowResultInputs& inputs, TypeTable& types, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;
    BitVector candidates(program.layouts.size());
    if (!inputs.build.isEnabledEverywhere(FlowResultKind::NarrowedTypes) || !inputs.build.isEnabledEverywhere(FlowResultKind::ProvenLayoutSets))
        return candidates;
    for (unsigned index = 0; index < program.layouts.size(); ++index)
        candidates.quickSet(index, mayHaveDictionaries(program.layouts[index], application.layouts[index]));
    if (candidates.isEmpty())
        return candidates;

    BitVector setsNamingCandidates = findLayoutSetsNaming(program, candidates, inputs.loops());
    Vector<uint32_t> found;
    auto searchEach = [&](unsigned count, const auto& search) {
        found.appendVector(collectInParallel<uint32_t>(inputs.loops(), count, [&](unsigned index, Vector<uint32_t>& foundInRecord) {
            search(index, NamedLayoutFinder { program, candidates, setsNamingCandidates, foundInRecord });
        }));
    };
    auto searchLists = [&](const auto&... lists) {
        (searchEach(lists.size(), [&](unsigned index, const NamedLayoutFinder& finder) { forEachReference(lists[index], finder); }), ...);
    };
    searchEach(program.functions.size(), [&](unsigned index, const NamedLayoutFinder& finder) {
        listLayoutsNamedIn(program, application, program.functions[index], tables.functions.functions[index].executions == ExecutionCount::AtMostOnce, finder);
    });
    searchEach(program.layouts.size(), [&](unsigned index, const NamedLayoutFinder& finder) {
        forEachReference(static_cast<const LayoutRecord&>(program.layouts[index]), finder);
    });
    searchLists(program.typeSites, program.expectedResults, program.variables, program.modules, program.moduleFailures, program.evalScopes, program.entries, program.runtimeFunctions, program.webAssemblyModules);
    searchEach(program.data.size(), [&](unsigned index, const NamedLayoutFinder& finder) {
        const auto* object = program.data[index].root.tryAs<DataNode::Object>();
        if (!object || !object->members.isEmpty())
            forEachReference(program.data[index], finder);
    });
    for (uint32_t index : found)
        candidates.quickClear(index);

    for (size_t index : candidates) {
        application.layouts[index].objectsAreDictionaries = true;
        application.layouts[index].flow.allocation = AllocationState::NeverAllocated;
    }
    application.hasDictionaries = !candidates.isEmpty();
    if (application.hasDictionaries)
        createTypesBehindDictionaries(program, candidates, types, application, inputs.loops());
    return candidates;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
