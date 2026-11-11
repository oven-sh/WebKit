/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWireTypes.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTObjects.h"
#include "AOTPhaseTimes.h"
#include "AOTRowsOfLayouts.h"
#include "AOTTaggedLayouts.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include "AOTTypeClosure.h"
#include "AOTWireTypesInternal.h"
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <type_traits>
#include <wtf/MallocSpan.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t wireTypeUnitesContentsOfClasses = UINT32_MAX;

static bool hasWireTypeFlag(const Wire::Type& type, TypeFlags flag)
{
    return type.flags & static_cast<uint32_t>(flag);
}

static std::span<const uint32_t> wordsListedByWireType(const Wire::Sections& checkedSections, Wire::Span list)
{
    return checkedSections.words.subspan(list.start, list.count);
}

template<typename MemberID, typename SetID>
static SetID setOfWireTypeMembers(SetsOf<MemberID, SetID>& sets, std::span<const std::type_identity_t<MemberID>> ascendingWithoutRepeats)
{
    if (ascendingWithoutRepeats.empty())
        return { };
    if (ascendingWithoutRepeats.size() == 1)
        return SetsOf<MemberID, SetID>::of(ascendingWithoutRepeats[0]);
    return sets.ofSorted(ascendingWithoutRepeats);
}

static LayoutSetID uniteFamiliesOfWireLayouts(TypeArena& arena, const Vector<LayoutSetID>& familyOfLayout, std::span<const uint32_t> namedLayouts)
{
    if (namedLayouts.empty())
        return { };
    if (namedLayouts.size() == 1)
        return familyOfLayout[namedLayouts[0]];

    Vector<LayoutSetID, 16> families;
    for (uint32_t layout : namedLayouts)
        families.append(familyOfLayout[layout]);
    return arena.layoutSets().unionOf(families.span());
}

static ObjectSetID objectsOfWireBuiltins(TypeArena& arena, std::span<const uint32_t> namedBuiltins)
{
    if (namedBuiltins.empty())
        return { };

    const BuiltinTable& builtins = arena.builtins();
    const ObjectTable& objects = arena.objects();
    Vector<ObjectID, 8> named;
    for (uint32_t builtin : namedBuiltins)
        named.append(objects.objectOf(builtins.canonicalID(BuiltinID(builtin))));
    std::ranges::sort(named);
    removeRepeatedElements(named);
    return setOfWireTypeMembers(arena.objectSets(), named.span());
}

static ClassPartsID readClassPartsOfWireType(TypeArena& arena, const Wire::Sections& checkedSections, uint32_t index, const Vector<TypeID>& typesOfEarlierIndexes)
{
    Wire::Span list = checkedSections.types[index].classes;
    if (!list.count)
        return { };

    Vector<ClassPart, 8> parts;
    for (const Wire::ClassPart& wire : checkedSections.classParts.subspan(list.start, list.count)) {
        ClassPart part;
        part.builtinClass = BuiltinClassID(wire.builtinClass);
        std::span<const uint32_t> contents = wordsListedByWireType(checkedSections, wire.contents);
        RELEASE_ASSERT(contents.size() <= mostContentsOfClass);
        for (size_t position = 0; position < contents.size(); ++position) {
            RELEASE_ASSERT(contents[position] < index);
            part.writtenContents[position] = typesOfEarlierIndexes[contents[position]];
        }
        parts.append(part);
    }
    return arena.internClassParts(parts.span());
}

static ALWAYS_INLINE uint32_t endOfTypesNamedByWireType(const Wire::Sections& checkedSections, uint32_t index)
{
    Wire::Span list = checkedSections.types[index].classes;
    std::span<const Wire::ClassPart> parts = checkedSections.classParts.subspan(list.start, list.count);
    uint32_t end = 0;
    for (size_t position = 0; position < parts.size(); ++position) {
        if (position && BuiltinClassID(parts[position - 1].builtinClass) >= BuiltinClassID(parts[position].builtinClass))
            return wireTypeUnitesContentsOfClasses;
        for (uint32_t named : wordsListedByWireType(checkedSections, parts[position].contents))
            end = std::max(end, named == UINT32_MAX ? named : named + 1);
    }
    return end;
}

static void internStringTagKeys(Program& program, const ParallelLoops& loops)
{
    Vector<StringID> literals = collectInParallel<StringID>(loops, static_cast<unsigned>(program.layouts.size()), [&](unsigned index, Vector<StringID>& found) {
        for (const Field& field : program.layouts[index].fields) {
            TypeID written = field.declared.written();
            if (!written || holdsOnlyPrimitives(written))
                continue;
            StringSetID literalsOfField = program.at(written).stringLiterals;
            if (!literalsOfField)
                continue;
            program.types.at(literalsOfField).forEach([&](StringID literal) {
                found.append(literal);
            });
        }
    });

    ArenasOfWorkers arenas(program.types, loops);
    Vector<PropertyKeyID> keys(literals.size());
    arenas.internInOrder(keys.mutableSpan(), [&](uint32_t index) {
        return PropertyKey { .kind = PropertyKeyKind::Text, .text = literals[index] };
    });
}

WireTypeReader::WireTypeReader(Program& program, const Wire::Sections& checkedSections, BuildContext& build, const ExternalFunctionBodies* externalBodies)
    : m_program(program)
    , m_sections(checkedSections)
    , m_build(build)
    , m_externalBodies(externalBodies)
    , m_numberOfDerivedLayouts(program.builtins().layoutCount())
{
}

WireTypeReader::~WireTypeReader() = default;

static FunctionInOrderOfObjects placeInOrderOfObjects(const Wire::Function& function)
{
    using Group = FunctionInOrderOfObjects::Group;
    if (!function.body.blockCount)
        return { Group::WithoutBody, false, false };

    constexpr uint32_t flagsOfCoroutines = OptionSet<FunctionFlags> { FunctionFlags::IsAsync, FunctionFlags::IsGenerator }.toRaw();
    constexpr uint32_t flagsOfRestArrays = OptionSet<FunctionFlags> { FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest }.toRaw();
    FunctionInOrderOfObjects place { Group::EnteredByEngineAlone, !!(function.flags & flagsOfCoroutines), !!(function.flags & flagsOfRestArrays) };
    switch (static_cast<FunctionKind>(function.kind)) {
    case FunctionKind::Plain:
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
        place.group = Group::PlainFunctionOrClass;
        break;
    case FunctionKind::Arrow:
        place.group = Group::Arrow;
        break;
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
        place.group = Group::MethodOrAccessor;
        break;
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        break;
    }
    return place;
}

void WireTypeReader::makeObjects()
{
    Vector<FunctionInOrderOfObjects> functions;
    functions.grow(m_sections.functions.size());
    forEachRangeOfIndicesInParallel(m_build.parallelLoops(), functions.size(), [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index)
            functions[index] = placeInOrderOfObjects(m_sections.functions[index]);
    });
    m_program.types.setObjects(makeUniqueWithoutFastMallocCheck<ObjectTable>(m_program.builtins(), functions.span(), &m_build.parallelLoops()));
}

bool WireTypeReader::readsAsAnything(std::span<const uint32_t> namedLayouts) const
{
    return std::ranges::any_of(namedLayouts, [&](uint32_t layout) {
        return m_typeParametersWithoutFamily.get(layout) || m_viewsWithoutLayouts.get(layout);
    });
}

TypeParts WireTypeReader::readPartsWithoutClasses(TypeArena& arena, uint32_t index, ViewsWithoutLayouts views) const
{
    const Wire::Type& wire = m_sections.types[index];
    std::span<const uint32_t> namedLayouts = wordsListedByWireType(m_sections, wire.layouts);
    Vector<uint32_t, 8> layoutsThatSomebodyMakes;
    if (views == ViewsWithoutLayouts::AreLeftOut) {
        for (uint32_t layout : namedLayouts) {
            if (!m_viewsWithoutLayouts.get(layout))
                layoutsThatSomebodyMakes.append(layout);
        }
        namedLayouts = layoutsThatSomebodyMakes.span();
    }

    TypeParts parts;
    parts.primitives = fromWire(static_cast<Wire::Primitives>(wire.primitives));
    if (readsAsAnything(namedLayouts)) {
        parts.primitives = Primitive::All | (parts.primitives & Primitive::Absent);
        parts.integers = Range::full();
        parts.anyObject = true;
        parts.anyFunction = true;
        return parts;
    }

    if (parts.primitives & Primitive::Integer)
        parts.integers = Range::full();
    if (hasWireTypeFlag(wire, TypeFlags::HasIntegers)) {
        Range written = Range { wire.minimum, wire.maximum }.intersectWith(Range::full());
        if (!written.isEmpty()) {
            parts.primitives |= Primitive::Integer;
            parts.integers = parts.integers.unionWith(written);
        }
    }

    parts.anyObject = hasWireTypeFlag(wire, TypeFlags::AnyObject);
    parts.anyFunction = hasWireTypeFlag(wire, TypeFlags::AnyFunction);
    if (wire.opaque != Wire::none)
        parts.opaque = static_cast<OpaqueKind>(wire.opaque);
    if (wire.environment != Wire::none)
        parts.environment = LayoutID(wire.environment);

    if (!(parts.primitives & Primitive::String)) {
        std::span<const uint32_t> strings = wordsListedByWireType(m_sections, wire.strings);
        ASSERT(std::ranges::adjacent_find(strings, std::ranges::greater_equal { }) == strings.end());
        parts.stringLiterals = setOfWireTypeMembers(arena.stringSets(), spanReinterpretCast<const StringID>(strings));
    }
    if (!parts.anyObject)
        parts.unnamedLayouts = uniteFamiliesOfWireLayouts(arena, m_declaredMembers, namedLayouts);

    parts.objects = objectsOfWireBuiltins(arena, wordsListedByWireType(m_sections, wire.builtins));
    for (uint32_t function : wordsListedByWireType(m_sections, wire.functions)) {
        ObjectSetID family = m_functionFamilies[function];
        if (!family || isSignature(m_sections.functions[function]))
            parts.anyFunction = true;
        parts.objects = arena.objectSets().unionOf(parts.objects, family);
    }
    return parts;
}

void WireTypeReader::readTypes()
{
    const ParallelLoops& loops = m_build.parallelLoops();
    uint32_t numberOfTypes = static_cast<uint32_t>(m_sections.types.size());

    auto endOfNamedTypes = MallocSpan<uint32_t>::malloc(numberOfTypes * sizeof(uint32_t));
    forEachRangeOfIndicesInParallel(loops, numberOfTypes, [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index)
            endOfNamedTypes[index] = endOfTypesNamedByWireType(m_sections, static_cast<uint32_t>(index));
    });

    ArenasOfWorkers arenas(m_program.types, loops);
    Vector<TypeID> types(numberOfTypes);
    auto partsOf = [&](uint32_t index, TypeArena& arena) {
        TypeParts parts = readPartsWithoutClasses(arena, index);
        if (!parts.anyObject)
            parts.unnamedClasses = readClassPartsOfWireType(arena, m_sections, index, types);
        return parts;
    };

    for (uint32_t first = 0; first < numberOfTypes;) {
        if (endOfNamedTypes[first] == wireTypeUnitesContentsOfClasses) {
            TypeArena& arena = arenas.of(0);
            types[first] = arena.intern(partsOf(first, arena));
            ++first;
            continue;
        }
        uint32_t end = first + 1;
        while (end < numberOfTypes && endOfNamedTypes[end] <= first)
            ++end;
        arenas.internInOrder(types.mutableSpan().subspan(first, end - first), [&](uint32_t offset, TypeArena& arena) {
            return partsOf(first + offset, arena);
        });
        first = end;
    }

    findTypesExpectedBesideViews(arenas, types);
    m_program.types.setTypesOfWireIndexes(WTF::move(types));
}

void WireTypeReader::findTypesExpectedBesideViews(ArenasOfWorkers& arenas, const Vector<TypeID>& typesOfWireIndexes)
{
    struct PartsBesideViews {
        uint32_t index;
        bool unitesContentsOfClasses;
        TypeParts parts;
    };
    auto partsBesideViewsOf = [&](uint32_t index, TypeArena& arena) -> std::optional<TypeParts> {
        TypeParts parts = readPartsWithoutClasses(arena, index, ViewsWithoutLayouts::AreLeftOut);
        if (!parts.anyObject)
            parts.unnamedClasses = readClassPartsOfWireType(arena, m_sections, index, typesOfWireIndexes);
        bool namesSomethingElse = (parts.primitives & ~(Primitive::UndefinedOrNull | Primitive::Absent)) || parts.stringLiterals || parts.objects || parts.unnamedLayouts || parts.unnamedClasses || parts.anyFunction;
        if (!namesSomethingElse || parts.anyObject)
            return std::nullopt;
        return parts;
    };

    const ParallelLoops& loops = m_build.parallelLoops();
    uint32_t numberOfTypes = static_cast<uint32_t>(m_sections.types.size());
    ChunksOfIndices division = divideIntoChunks(numberOfTypes);
    Vector<Vector<PartsBesideViews>> foundInChunks(division.size());
    forEachIndexInParallel(loops, division.size(), [&](unsigned chunk, unsigned worker) {
        for (uint32_t index = division.firstIndexOf[chunk]; index < division.firstIndexOf[chunk + 1]; ++index) {
            std::span<const uint32_t> namedLayouts = wordsListedByWireType(m_sections, m_sections.types[index].layouts);
            if (!readsAsAnything(namedLayouts) || std::ranges::any_of(namedLayouts, [&](uint32_t layout) { return m_typeParametersWithoutFamily.get(layout); }))
                continue;
            if (endOfTypesNamedByWireType(m_sections, index) == wireTypeUnitesContentsOfClasses)
                foundInChunks[chunk].append({ index, true, { } });
            else if (std::optional<TypeParts> parts = partsBesideViewsOf(index, arenas.of(worker)))
                foundInChunks[chunk].append({ index, false, *parts });
        }
    });

    Vector<PartsBesideViews> found;
    for (Vector<PartsBesideViews>& foundInChunk : foundInChunks)
        found.appendVector(WTF::move(foundInChunk));

    Vector<TypeID> expected(found.size());
    for (size_t first = 0; first < found.size();) {
        if (found[first].unitesContentsOfClasses) {
            TypeArena& arena = arenas.of(0);
            if (std::optional<TypeParts> parts = partsBesideViewsOf(found[first].index, arena))
                expected[first] = arena.intern(*parts);
            ++first;
            continue;
        }
        size_t end = first + 1;
        while (end < found.size() && !found[end].unitesContentsOfClasses)
            ++end;
        arenas.internInOrder(expected.mutableSpan().subspan(first, end - first), [&](uint32_t offset, TypeArena&) {
            return found[first + offset].parts;
        });
        first = end;
    }

    for (size_t place = 0; place < found.size(); ++place) {
        if (!expected[place])
            continue;
        if (m_typesExpectedBesideViews.isEmpty())
            m_typesExpectedBesideViews.fill(TypeID(), numberOfTypes);
        m_typesExpectedBesideViews[found[place].index] = expected[place];
    }
}

void WireTypeReader::finishLayouts()
{
    uint32_t numberOfWireLayouts = static_cast<uint32_t>(m_sections.layouts.size());
    RELEASE_ASSERT(m_program.layouts.size() == numberOfWireLayouts);

    StepTimes steps(m_build, "ReadAndVerify: finish layouts"_s);
    m_program.internBuiltinNameKeys();
    internStringTagKeys(m_program, m_build.parallelLoops());
    steps.didFinish("keys of built-in names and of tags"_s);

    m_program.appendBuiltinLayouts();
    steps.didFinish("layouts of built-ins"_s);
    m_program.appendNamespaceLayouts(m_build.parallelLoops());
    steps.didFinish("layouts of namespaces"_s);
    appendTaggedLayouts(m_program, m_program.taggedFamilies.span());
    steps.didFinish("tagged layouts"_s);
    Layout layoutOfClosures;
    layoutOfClosures.kind = LayoutKind::Closure;
    m_program.layouts.append(WTF::move(layoutOfClosures));
    RELEASE_ASSERT(m_program.layouts.size() == numberOfWireLayouts + m_numberOfDerivedLayouts);
    RELEASE_ASSERT(m_program.layouts.size() == m_declaredMembers.size());

    for (uint32_t index = 0; index < m_declaredMembers.size(); ++index)
        m_program.setDeclaredMembers(LayoutID(index), m_declaredMembers[index]);
    steps.didFinish("declared members"_s);

    initializeLayouts(m_program, m_build);
    steps.didFinish("initialize layouts"_s);

    BitVector layoutsWithAcyclicBases = answerAlongRows(numberOfWireLayouts, false, [&](LayoutID id) -> StepAlongRow {
        LayoutID base = m_program.at(id).base;
        if (!base)
            return { true, { } };
        return { std::nullopt, base };
    });
    for (size_t index : layoutsWithAcyclicBases) {
        if (m_program.layouts[index].base || m_program.layouts[index].has(LayoutFlags::HasHeritage))
            appendInheritedFields(m_program, LayoutID(index));
    }
    appendFieldsOfTaggedLayouts(m_program, m_program.taggedFamilies.span());
    steps.didFinish("inherited fields"_s);
}

static BitVector modulesNamedByModuleBuiltins(const Program& program, uint32_t numberOfModules, const ParallelLoops& loops)
{
    constexpr uint32_t functionsPerTask = 64;
    uint32_t numberOfFunctions = static_cast<uint32_t>(program.functions.size());
    auto isNamed = std::make_unique<std::atomic<bool>[]>(numberOfModules);
    forEachIndexInParallel(loops, (numberOfFunctions + functionsPerTask - 1) / functionsPerTask, [&](unsigned task) {
        uint32_t end = std::min(numberOfFunctions, (task + 1) * functionsPerTask);
        for (uint32_t index = task * functionsPerTask; index < end; ++index) {
            const Body& body = program.functions[index].body;
            if (!body)
                continue;
            const UnitIDRemap* unit = &body.unit();
            BodyBlock block = body.entry();
            for (uint32_t blockIndex = 0; blockIndex < body.numberOfBlocks(); ++blockIndex) {
                BodyValues::Iterator cursor = block.values().begin();
                for (uint32_t remaining = block.numberOfValues(); remaining--; ++cursor) {
                    BodyValue value = *cursor;
                    if (value.opcode() != Opcode::ModuleBuiltin)
                        continue;
                    ModuleID module = value.as<Opcode::ModuleBuiltin>().module();
                    if (module && module.index() < numberOfModules)
                        isNamed[module.index()].store(true, std::memory_order_relaxed);
                }
                BodyTerminator terminator(cursor.words(), unit, BlockID(blockIndex));
                block = BodyBlock(terminator.end(), nullptr, unit, BlockID(blockIndex + 1), block.endOfValues());
            }
        }
    });

    BitVector modules(numberOfModules);
    for (uint32_t index = 0; index < numberOfModules; ++index) {
        if (isNamed[index].load(std::memory_order_relaxed))
            modules.quickSet(index);
    }
    return modules;
}

void WireTypeReader::findModulesNamedByBodies()
{
    m_modulesNamedByBodies = modulesNamedByModuleBuiltins(m_program, static_cast<uint32_t>(m_sections.modules.size()), m_build.parallelLoops());
}

void WireTypeReader::finishObjects()
{
    StepTimes steps(m_build, "ReadAndVerify: finish objects"_s);
    m_program.types.objects().addObjectsOfProgram(m_program, m_modulesNamedByBodies, m_build.parallelLoops());
    steps.didFinish("objects of the program"_s);
    expectFieldsFilledWithOtherData(m_program, m_build);
    steps.didFinish("fields filled with other data"_s);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
