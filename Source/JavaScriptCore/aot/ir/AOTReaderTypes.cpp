/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTReaderInternal.h"

#if ENABLE(AOT)

#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static InternalError inLayout(uint32_t index)
{
    return { .kind = InternalErrorKind::Unknown, .layout = LayoutID(index), .count = index };
}

static Failure checkDeclaredSupertypes(const Reader& reader, uint32_t index)
{
    std::span<const Wire::Layout> layouts = reader.sections().layouts;
    const Wire::Layout& layout = layouts[index];
    RecordChecker checker(reader, inLayout(index));

    checker.enumerator<LayoutKind>(layout.kind);
    checker.require(layout.predictedBase == Wire::none || layout.predictedBase < layouts.size(), InternalErrorKind::IdOutOfRange);
    for (uint32_t supertype : checker.words(layout.supertypes))
        checker.require(supertype < layouts.size(), InternalErrorKind::IdOutOfRange);
    return checker.takeFailure();
}

static bool isEnvironment(const Wire::Layout& checkedLayout)
{
    return isEnvironmentKind(static_cast<LayoutKind>(checkedLayout.kind));
}

static Vector<Vector<LayoutID>> collectDeclaredMembers(const Wire::Sections& checkedSections)
{
    std::span<const Wire::Layout> layouts = checkedSections.layouts;
    Vector<Vector<LayoutID>> members(layouts.size());
    Vector<uint32_t> stamps(FillWith { }, layouts.size(), 0u);
    Vector<uint32_t, 16> worklist;
    for (uint32_t index = 0; index < layouts.size(); ++index) {
        LayoutID member(index);
        if (isEnvironment(layouts[index])) {
            members[index].append(member);
            continue;
        }

        uint32_t stamp = index + 1;
        auto visit = [&](uint32_t layout) {
            if (std::exchange(stamps[layout], stamp) != stamp && !isEnvironment(layouts[layout]))
                worklist.append(layout);
        };
        visit(index);
        while (!worklist.isEmpty()) {
            uint32_t reachedIndex = worklist.takeLast();
            const Wire::Layout& reached = layouts[reachedIndex];
            members[reachedIndex].append(member);
            if (reached.predictedBase != Wire::none)
                visit(reached.predictedBase);
            for (uint32_t supertype : checkedSections.words.subspan(reached.supertypes.start, reached.supertypes.count))
                visit(supertype);
        }
    }
    return members;
}

bool Reader::computeDeclaredMembers()
{
    uint32_t numberOfLayouts = static_cast<uint32_t>(m_sections.layouts.size());
    auto numberOfNamespaceLayouts = std::ranges::count_if(m_sections.modules, [](const Wire::Module& module) {
        return module.kind == static_cast<uint32_t>(ModuleKind::SourceText);
    });
    m_numberOfDerivedLayouts = m_builtins.layoutCount() + static_cast<uint32_t>(numberOfNamespaceLayouts);

    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (auto failure = checkDeclaredSupertypes(*this, index))
            return report(*failure);
    }

    Vector<Vector<LayoutID>> members = collectDeclaredMembers(m_sections);
    planTaggedLayouts(members);
    size_t numberOfLayoutIDs = numberOfRecords<LayoutID>();
    m_declaredMembers.reserveInitialCapacity(numberOfLayoutIDs);
    for (Vector<LayoutID>& membersOfLayout : members)
        m_declaredMembers.append(m_program.types.intern(LayoutSet { WTF::move(membersOfLayout), true }));
    for (size_t index = numberOfLayouts; index < numberOfLayoutIDs; ++index)
        m_declaredMembers.append(m_program.types.intern(LayoutSet { { LayoutID(index) }, true }));
    return true;
}

static bool isSignature(const Wire::Function& function)
{
    return function.role == static_cast<uint32_t>(FunctionRole::Signature);
}

static Failure checkFunctionTypes(const Reader& reader, uint32_t index)
{
    std::span<const Wire::Function> functions = reader.sections().functions;
    RecordChecker checker(reader, { .kind = InternalErrorKind::Unknown, .function = FunctionID(index), .count = index });
    for (uint32_t functionType : checker.words(functions[index].functionTypes))
        checker.require(functionType < functions.size() && isSignature(functions[functionType]), InternalErrorKind::IdOutOfRange);
    return checker.takeFailure();
}

static Vector<Vector<FunctionID>> collectFunctionFamilies(const Wire::Sections& checkedSections)
{
    std::span<const Wire::Function> functions = checkedSections.functions;
    Vector<Vector<FunctionID>> families(functions.size());
    Vector<uint32_t> stamps(FillWith { }, functions.size(), 0u);
    Vector<uint32_t, 16> worklist;
    for (uint32_t index = 0; index < functions.size(); ++index) {
        if (isSignature(functions[index]))
            continue;

        uint32_t stamp = index + 1;
        worklist.append(index);
        while (!worklist.isEmpty()) {
            uint32_t reachedIndex = worklist.takeLast();
            families[reachedIndex].append(FunctionID(index));
            Wire::Span functionTypes = functions[reachedIndex].functionTypes;
            for (uint32_t functionType : checkedSections.words.subspan(functionTypes.start, functionTypes.count)) {
                if (std::exchange(stamps[functionType], stamp) != stamp)
                    worklist.append(functionType);
            }
        }
    }
    return families;
}

static StatisticID statisticForFamilyOf(size_t numberOfMembers, uint64_t maxDispatchCases)
{
    if (!numberOfMembers)
        return StatisticID::FunctionTypesWithoutMembers;
    if (numberOfMembers == 1)
        return StatisticID::FunctionTypesWithOneMember;
    return numberOfMembers <= maxDispatchCases ? StatisticID::FunctionTypesWithFewMembers : StatisticID::FunctionTypesWithManyMembers;
}

bool Reader::computeFunctionFamilies()
{
    uint32_t numberOfFunctions = static_cast<uint32_t>(m_sections.functions.size());
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        if (auto failure = checkFunctionTypes(*this, index))
            return report(*failure);
    }

    Vector<Vector<FunctionID>> families = collectFunctionFamilies(m_sections);
    m_functionFamilies.reserveInitialCapacity(numberOfFunctions);
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        if (isSignature(m_sections.functions[index]))
            m_build.statistics().count(statisticForFamilyOf(families[index].size(), m_build.limit(LimitID::MaxDispatchCases)), 1);
        m_functionFamilies.append(m_program.types.intern(CalleeSet { WTF::move(families[index]), { }, false }));
    }
    return true;
}

template<typename IDType>
static Vector<IDType, 8> collectAscendingIDs(RecordChecker& checker, std::span<const uint32_t> words)
{
    Vector<IDType, 8> ids;
    ids.reserveInitialCapacity(words.size());
    for (uint32_t word : words) {
        IDType id = checker.id<IDType>(word);
        if (!ids.isEmpty())
            checker.requireAscending(ids.last(), id);
        ids.append(id);
    }
    return ids;
}

static TypeID earlierType(RecordChecker& checker, std::span<const TypeID> earlierTypes, uint32_t word)
{
    if (word >= earlierTypes.size()) {
        checker.fail(InternalErrorKind::TypeRefersForward);
        return { };
    }
    return earlierTypes[word];
}

static void readPrimitives(RecordChecker& checker, const Wire::Type& wire, OptionSet<TypeFlags> flags, Type& type)
{
    type = Type::fromPrimitives(fromWire(checker.flags<Primitives>(wire.primitives)));
    if (!flags.contains(TypeFlags::HasIntegers))
        return;
    type.primitives |= Primitive::Integer;
    type.integers = type.integers.unionWith(Range { wire.minimum, wire.maximum }.intersectWith(Range::full()));
}

static SortedSet<BuiltinID> readBuiltins(RecordChecker& checker, Wire::Span span)
{
    const BuiltinTable& builtins = checker.reader().builtins();
    Vector<BuiltinID, 4> canonicalIDs;
    BuiltinID previous;
    for (uint32_t word : checker.words(span)) {
        BuiltinID builtin = checker.external<BuiltinID>(word);
        if (checker.hasFailed())
            break;
        if (previous)
            checker.requireAscending(previous, builtin);
        previous = builtin;
        canonicalIDs.append(builtins.canonicalID(builtin));
    }
    return SortedSet<BuiltinID>(canonicalIDs.span());
}

static LayoutSetID internLayouts(TypeTable& types, IRForm form, std::span<const LayoutSetID> declaredMembers, std::span<const LayoutID> written)
{
    if (written.empty())
        return { };
    if (form != IRForm::AsRead)
        return types.intern(LayoutSet { Vector<LayoutID>(written), true });
    if (written.size() == 1)
        return declaredMembers[written[0].index()];

    Vector<LayoutID> members;
    for (LayoutID layout : written)
        members.appendVector(types.at(declaredMembers[layout.index()]).members);
    return types.intern(LayoutSet { WTF::move(members), true });
}

void Reader::addFunctions(Type& type, std::span<const FunctionID> written) const
{
    if (written.empty() || type.anyFunction)
        return;
    if (m_form != IRForm::AsRead) {
        type.functions = SortedSet<FunctionID>(written);
        return;
    }

    Vector<FunctionID, 8> members;
    for (FunctionID function : written) {
        const Vector<FunctionID>& family = m_program.at(m_functionFamilies[function.index()]).functions;
        if (family.isEmpty()) {
            type.anyFunction = true;
            return;
        }
        members.appendVector(family);
    }
    type.functions = SortedSet<FunctionID>(members.span());
    if (type.functions.size() > m_build.limit(LimitID::MaxFunctionsInType)) {
        type.functions = { };
        type.anyFunction = true;
    }
}

bool Reader::readTypes()
{
    uint32_t numberOfTypes = static_cast<uint32_t>(m_sections.types.size());
    m_types.reserveInitialCapacity(numberOfTypes);
    for (uint32_t index = 0; index < numberOfTypes; ++index) {
        Type type;
        if (auto failure = readType(index, type))
            return report(*failure);
        m_types.append(m_program.types.intern(type));
    }
    return true;
}

Failure Reader::readType(uint32_t index, Type& type)
{
    const Wire::Type& wire = m_sections.types[index];
    RecordChecker checker(*this, inRecord(index));

    auto flags = checker.flags<TypeFlags>(wire.flags);
    readPrimitives(checker, wire, flags, type);
    type.anyObject = flags.contains(TypeFlags::AnyObject);
    type.anyFunction = flags.contains(TypeFlags::AnyFunction);

    type.stringLiterals = SortedSet<StringID>(collectAscendingIDs<StringID>(checker, checker.words(wire.strings)).span());
    auto layouts = collectAscendingIDs<LayoutID>(checker, checker.words(wire.layouts));
    auto functions = collectAscendingIDs<FunctionID>(checker, checker.words(wire.functions));

    auto classParts = checker.slice<Wire::ClassPart>(wire.classes);
    type.classes.reserveInitialCapacity(classParts.size());
    for (auto& wirePart : classParts) {
        ClassPart part;
        if (auto failure = readClassPart(checker, index, wirePart, part))
            return failure;
        if (!type.classes.isEmpty())
            checker.requireAscending(type.classes.last().builtinClass, part.builtinClass);
        type.classes.append(WTF::move(part));
    }

    type.builtins = readBuiltins(checker, wire.builtins);
    type.opaque = checker.optionalEnumerator<OpaqueKind>(wire.opaque);
    type.environment = checker.optionalID<LayoutID>(wire.environment);
    if (auto failure = checker.takeFailure())
        return failure;

    type.layouts = internLayouts(m_program.types, m_form, m_declaredMembers.span(), layouts.span());
    addFunctions(type, functions.span());

    if (m_completeType)
        m_completeType(index, type);
    return std::nullopt;
}

Failure Reader::readClassPart(RecordChecker& checker, uint32_t typeIndex, const Wire::ClassPart& wire, ClassPart& part) const
{
    part.builtinClass = checker.external<BuiltinClassID>(wire.builtinClass);
    if (auto failure = checker.takeFailure())
        return failure;
    checker.require(wire.contents.count == m_builtins.numberOfSlotsInType(part.builtinClass), InternalErrorKind::ListOutOfBounds);

    auto contents = checker.words(wire.contents);
    auto earlierTypes = m_types.span().first(typeIndex);
    part.contents.reserveInitialCapacity(contents.size());
    for (uint32_t word : contents)
        part.contents.append(earlierType(checker, earlierTypes, word));
    return checker.takeFailure();
}

bool Reader::computePromisedTypes()
{
    if (m_form != IRForm::AsRead)
        return true;

    uint32_t numberOfTypes = m_program.types.size();
    m_promisedTypes.reserveInitialCapacity(numberOfTypes);
    for (uint32_t index = 0; index < numberOfTypes; ++index) {
        TypeID asWritten(index);
        if (!m_program.at(asWritten).layouts) {
            m_promisedTypes.append(asWritten);
            continue;
        }
        Type promised = m_program.at(asWritten);
        promised.layouts = { };
        promised.anyObject = true;
        m_promisedTypes.append(m_program.types.intern(promised));
    }
    m_anythingOrAbsent = m_program.types.intern(Type::anything().withPrimitives(Primitive::Absent));
    return true;
}

// A mismatch that is only reported lets the value through, so a written type holds for nothing that enters the position.
bool Reader::promisesType(const DeclaredType& declared) const
{
    switch (m_build.options().recorded.policy()) {
    case MismatchPolicy::Throw:
        return true;
    case MismatchPolicy::Report:
        return declared.flags.contains(PositionFlags::IsFromCompiler);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Nothing tests what enters an int32 promise when the program runs.
bool Reader::isTestedAtRunTime(const DeclaredType& declared) const
{
    return promisesType(declared) && !declared.flags.contains(PositionFlags::IsInt32Promise);
}

TypeID Reader::promisedTypeOf(const DeclaredType& declared) const
{
    TypeID asWritten = declared.type;
    if (isTestedAtRunTime(declared) || !asWritten || asWritten.index() >= m_promisedTypes.size())
        return promisedTypeOf(asWritten);
    return m_program.at(asWritten).primitives & Primitive::Absent ? m_anythingOrAbsent : TypeTable::anything;
}

TypeID Reader::promisedTypeOf(TypeID asWritten) const
{
    if (!asWritten || asWritten.index() >= m_promisedTypes.size())
        return asWritten;
    return m_promisedTypes[asWritten.index()];
}

static void appendDerivedLayouts(Program& program, BuildContext& build, std::span<const LayoutSetID> declaredMembers, std::span<const TaggedFamily> taggedFamilies, size_t numberOfFunctions)
{
    size_t numberOfLayoutsAsRead = program.layouts.size();
    // The passes add about as many functions again. Room that is not used is not touched, and moving every function to a larger buffer is work for one thread.
    program.functions.reserveInitialCapacity(numberOfFunctions + numberOfFunctions * 3 / 2);
    program.functions.grow(numberOfFunctions);
    program.recordCountsAsRead();

    program.internBuiltinNameKeys();
    program.appendBuiltinLayouts();
    program.appendNamespaceLayouts();
    appendTaggedLayouts(program, taggedFamilies);
    RELEASE_ASSERT(program.layouts.size() == declaredMembers.size());

    for (size_t index = 0; index < numberOfLayoutsAsRead; ++index)
        program.setDeclaredMembers(LayoutID(index), declaredMembers[index]);
    initializeLayouts(program, build);
    appendFieldsOfTaggedLayouts(program, taggedFamilies);
}

bool Reader::readLayouts()
{
    constexpr size_t layoutsPerChunk = 64;
    m_program.layouts.grow(m_sections.layouts.size());
    auto failures = readInChunks(m_build.parallelLoops(), m_sections.layouts.size(), layoutsPerChunk, [&](uint32_t index) {
        RecordChecker checker(*this, inLayout(index));
        read(checker, m_sections.layouts[index], static_cast<LayoutRecord&>(m_program.layouts[index]));
        return checker.takeFailure();
    });
    if (!reportFirst(failures.span()))
        return false;

    appendDerivedLayouts(m_program, m_build, m_declaredMembers.span(), m_taggedFamilies.span(), m_sections.functions.size());
    m_program.setFunctionFamilies(WTF::move(m_functionFamilies));
    return checkTypesAgainstLayouts();
}

bool Reader::checkTypesAgainstLayouts()
{
    for (uint32_t index = 0; index < m_types.size(); ++index) {
        const Type& type = m_program.at(m_types[index]);
        for (LayoutID layout : membersOf(m_program, type)) {
            if (m_program.at(layout).isEnvironment())
                return report({ .kind = InternalErrorKind::InvalidLayout, .layout = layout, .count = index });
        }
        if (type.environment && !m_program.at(type.environment).isEnvironment())
            return report({ .kind = InternalErrorKind::InvalidLayout, .layout = type.environment, .count = index });
    }
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
