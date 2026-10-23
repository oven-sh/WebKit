/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTSchemaWalkers.h"
#include "AOTSort.h"
#include "AOTStringArena.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

// Lists the layouts of a set that a record names by itself or in a set of layouts. The types are no records of that sort: see createTypesBehindDictionaries().
struct NamedLayouts {
    NamedLayouts(const Program& program, const BitVector& layouts, const ParallelLoops& loops)
        : program(program)
        , layouts(layouts)
        , setsNamingThem(program.types.numberOfLayoutSets())
    {
        for (uint32_t index : findIndicesInParallel(loops, program.types.numberOfLayoutSets(), [&](unsigned index) {
            return std::ranges::any_of(program.at(LayoutSetID(index)).members, [&](LayoutID member) { return layouts.quickGet(member.index()); });
        }))
            setsNamingThem.quickSet(index);
    }

    const Program& program;
    const BitVector& layouts;
    BitVector setsNamingThem;
};

struct NamedLayoutFinder {
    void operator()(LayoutID id) const
    {
        if (id && named.layouts.quickGet(id.index()))
            found.append(id.index());
    }
    void operator()(LayoutSetID id) const
    {
        if (!id || !named.setsNamingThem.quickGet(id.index()))
            return;
        for (LayoutID member : named.program.at(id).members)
            (*this)(member);
    }
    void operator()(const auto&) const { }

    const NamedLayouts& named;
    Vector<uint32_t>& found;
};

} // anonymous namespace

// What Object.create makes is an ordinary object wherever it has got no field, also where it stays empty: it is made to stand above or below other objects.
static bool isLayoutOfLiteralWithoutFields(const Layout& layout, const FlowLayoutUpdate& update)
{
    bool isOfCreatedObject = layout.flags == OptionSet<LayoutFlags> { LayoutFlags::IsOfCreatedObject };
    return layout.kind == LayoutKind::Record && layout.fields.isEmpty() && layout.staticFields.isEmpty() && layout.supertypes.isEmpty()
        && !layout.base && !layout.constructor && !layout.name && !layout.computedKeyCount && (layout.flags.isEmpty() || isOfCreatedObject)
        && update.appendedFields.isEmpty() && (isOfCreatedObject || !update.flow.impossibleMutations.contains(ObjectMutation::AddProperty));
}

// `const levels: Record<string, number> = { debug: 0, info: 1 }`: the type allows any name.
static bool isLayoutOfLiteralUsedAsDictionary(const Layout& layout, const FlowLayoutUpdate& update)
{
    return layout.kind == LayoutKind::Record && layout.flags == OptionSet<LayoutFlags> { LayoutFlags::IsWrittenUnderDictionaryType } && layout.staticFields.isEmpty() && layout.supertypes.isEmpty()
        && !layout.base && !layout.constructor && !layout.name && !layout.computedKeyCount
        && !update.flow.impossibleMutations.containsAll({ ObjectMutation::AddProperty, ObjectMutation::DeleteField });
}

static void listLayoutsNamedIn(const Program& program, const Function& function, const NamedLayoutFinder& finder)
{
    forEachReference(static_cast<const FunctionRecord&>(function), finder);
    // A test can find a layout, and nothing of an ordinary object: it stops looking for a layout that nothing has any more. One that would look for nothing at all keeps what it looks for.
    auto listLayoutsTestedAlone = [&](TypeID tested) {
        Type rest = program.at(tested);
        LayoutSetID layouts = std::exchange(rest.layouts, LayoutSetID());
        if (!layouts || !rest.isNever())
            return;
        const LayoutSet& set = program.at(layouts);
        if (!set.hasMembersWithoutLayout && std::ranges::all_of(set.members, [&](LayoutID member) { return finder.named.layouts.quickGet(member.index()); }))
            finder(layouts);
    };
    for (const Value& value : function.values) {
        if (!layoutOfEmptyLiteral(program, value))
            forEachReference(value, finder);
        if (value.opcode == Opcode::Check)
            listLayoutsTestedAlone(value.castType());
        if (value.opcode == Opcode::TypeTest)
            listLayoutsTestedAlone(value.testedType());
    }
    for (const Block& block : function.blocks) {
        forEachReference(block.terminator, finder);
        if (block.terminator.kind != TerminatorKind::ReturnCall)
            continue;
        for (const TailCast& cast : block.terminator.casts())
            finder(program.at(cast.declared.type).layouts);
    }
}

enum class InPlaceOfDictionaries : bool { Nothing, Object };

static Type typeBehindDictionaries(const Program& program, const BitVector& layoutsOfDictionaries, const Type& type, TypeArena& types, const Invocable<TypeID(TypeID)> auto& behindOf, InPlaceOfDictionaries inPlace = InPlaceOfDictionaries::Object)
{
    Type behind = type;
    bool namesDictionaries = false;
    if (type.layouts) {
        LayoutSet set = program.at(type.layouts);
        namesDictionaries = set.members.removeAllMatching([&](LayoutID member) {
            return layoutsOfDictionaries.quickGet(member.index());
        });
        if (namesDictionaries)
            behind.layouts = set.members.isEmpty() && !set.hasMembersWithoutLayout ? LayoutSetID() : types.intern(WTF::move(set));
    }
    for (ClassPart& part : behind.classes) {
        for (TypeID& contents : part.contents)
            contents = behindOf(contents);
    }
    if (namesDictionaries && inPlace == InPlaceOfDictionaries::Object)
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

// A type names such a layout by itself or in what a container holds, however deep. A container has its turn behind what it holds.
static void createTypesBehindDictionaries(const Program& program, const BitVector& layoutsOfDictionaries, TypeTable& types, FlowApplication& application, const ParallelLoops& loops)
{
    application.layoutSetsNamingDictionaries = NamedLayouts(program, layoutsOfDictionaries, loops).setsNamingThem;
    unsigned numberOfTypes = types.size();
    Vector<TypeID> behindOf(numberOfTypes);
    auto holdsChangedType = [&](const Type& type) {
        return std::ranges::any_of(type.classes, [&](const ClassPart& part) {
            return std::ranges::any_of(part.contents, [&](TypeID contents) { return contents && behindOf[contents.index()]; });
        });
    };

    Vector<uint32_t> typesThatMayChange = findIndicesInParallel(loops, numberOfTypes, [&](unsigned index) {
        const Type& type = program.at(TypeID(index));
        bool namesDictionaries = type.layouts && application.layoutSetsNamingDictionaries.quickGet(type.layouts.index());
        return namesDictionaries || std::ranges::any_of(type.classes, [](const ClassPart& part) {
            return !part.contents.isEmpty();
        });
    });
    Vector<uint32_t> changedTypes;
    Vector<TypeID> testedBehind;
    TypeInterningPhase phase(types, 1);
    {
        TypeArena arena(phase, 0);
        for (bool didChange = true; std::exchange(didChange, false);) {
            for (uint32_t index : typesThatMayChange) {
                const Type& type = program.at(TypeID(index));
                bool namesDictionaries = type.layouts && application.layoutSetsNamingDictionaries.quickGet(type.layouts.index());
                if (!namesDictionaries && !holdsChangedType(type))
                    continue;
                TypeID behind = arena.intern(typeBehindDictionaries(program, layoutsOfDictionaries, type, arena, [&](TypeID contents) {
                    return contents && behindOf[contents.index()] ? behindOf[contents.index()] : contents;
                }));
                if (behindOf[index] == behind)
                    continue;
                if (!behindOf[index])
                    changedTypes.append(index);
                behindOf[index] = behind;
                didChange = true;
            }
        }
        AOT::sort(changedTypes);
        testedBehind = WTF::map(changedTypes, [&](uint32_t index) {
            return arena.intern(typeBehindDictionaries(program, layoutsOfDictionaries, program.at(TypeID(index)), arena, [&](TypeID contents) {
                return contents && behindOf[contents.index()] ? behindOf[contents.index()] : contents;
            }, InPlaceOfDictionaries::Nothing));
        });
    }
    TypeRemap remap = phase.finish(StringRemap { }, &loops);
    application.typesBehindDictionaries = WTF::map(changedTypes, [&](uint32_t index) {
        return std::pair { TypeID(index), remap(behindOf[index]) };
    });
    application.typesTestedBehindDictionaries.reserveInitialCapacity(changedTypes.size());
    for (size_t position = 0; position < changedTypes.size(); ++position)
        application.typesTestedBehindDictionaries.append({ TypeID(changedTypes[position]), remap(testedBehind[position]) });
    application.typesNamingDictionaries = BitVector(numberOfTypes);
    for (uint32_t index : changedTypes)
        application.typesNamingDictionaries.quickSet(index);
}

// A compiled object without a slot has nothing that an ordinary object lacks, and every name that is stored into it lies in a second object behind it.
BitVector computeLayoutsOfDictionaries(const ValueFlowResultInputs& inputs, TypeTable& types, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;
    BitVector candidates(program.layouts.size());
    if (!inputs.build.isEnabledEverywhere(FlowResultKind::NarrowedTypes) || !inputs.build.isEnabledEverywhere(FlowResultKind::ProvenLayoutSets))
        return candidates;
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        if (isLayoutOfLiteralWithoutFields(program.layouts[index], application.layouts[index]) || isLayoutOfLiteralUsedAsDictionary(program.layouts[index], application.layouts[index]))
            candidates.quickSet(index);
    }
    if (candidates.isEmpty())
        return candidates;

    NamedLayouts named(program, candidates, inputs.loops());
    Vector<uint32_t> found = collectInParallel<uint32_t>(inputs.loops(), program.functions.size(), [&](unsigned index, Vector<uint32_t>& foundInFunction) {
        listLayoutsNamedIn(program, program.functions[index], { named, foundInFunction });
    });
    auto searchEach = [&](unsigned count, const auto& search) {
        found.appendVector(collectInParallel<uint32_t>(inputs.loops(), count, [&](unsigned index, Vector<uint32_t>& foundInRecord) {
            search(index, NamedLayoutFinder { named, foundInRecord });
        }));
    };
    auto searchList = [&](const auto& list) {
        searchEach(list.size(), [&](unsigned index, const NamedLayoutFinder& finder) {
            forEachReference(list[index], finder);
        });
    };
    searchEach(program.layouts.size(), [&](unsigned index, const NamedLayoutFinder& finder) {
        forEachReference(static_cast<const LayoutRecord&>(program.layouts[index]), finder);
    });
    searchList(program.typeSites);
    searchList(program.expectedResults);
    searchList(program.variables);
    searchList(program.modules);
    searchList(program.moduleFailures);
    searchList(program.evalScopes);
    searchList(program.entries);
    searchList(program.runtimeFunctions);
    searchList(program.webAssemblyModules);
    searchEach(program.data.size(), [&](unsigned index, const NamedLayoutFinder& finder) {
        const Data& data = program.data[index];
        const auto* object = data.root.tryAs<DataNode::Object>();
        if (!object || !object->members.isEmpty())
            forEachReference(data, finder);
    });
    size_t numberOfCandidates = candidates.bitCount();
    for (uint32_t index : found)
        candidates.quickClear(index);
    if (inputs.build.options().logging.logsPhaseTimes) [[unlikely]]
        dataLogLn("AOT flow: the empty literals of ", candidates.bitCount(), " layouts make ordinary objects; ", numberOfCandidates - candidates.bitCount(), " more layouts without a field are named by something else");

    // Nothing has the layout from here on: no test may look for it, and no guess by a name may find it.
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
