/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTReaderInternal.h"

#if ENABLE(AOT)

#include "AOTEffectsNet.h"
#include "AOTPhaseTimes.h"
#include "AOTSectionWriter.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include <wtf/StdLibExtras.h>

#if COMPILER(CLANG)
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
#endif

namespace JSC { namespace AOT { namespace Typed {

static constexpr ASCIILiteral stepsOfRead = "ReadAndVerify: read"_s;
static constexpr ASCIILiteral stepsOfReadSideBySide = "ReadAndVerify: read, side by side"_s;

Reader::Reader(Program& program, BuildContext& build)
    : m_program(program)
    , m_build(build)
    , m_builtins(program.builtins())
    , m_buffer(program.buffer())
{
}

bool Reader::run()
{
    m_program.admitsReplacedBuiltins = m_build.options().admitsReplacedBuiltins();
    m_program.shouldAssumeNullishHostResults = AOT::hasFlag(m_build.options().testing, Wire::TestingSwitch::AssumeNullishHostResults);
    UnderstatedEffects understatedEffects = UnderstatedEffects::decode(m_build.options().understatedEffects);
    if (understatedEffects.part == PartOfEffects::RangeOfResult)
        m_program.builtinInjectedAsMinimumInteger = understatedEffects.row;
    if (understatedEffects.part == PartOfEffects::KindOfResult && understatedEffects.kind == KindOfEffectsRow::Operation)
        m_program.opcodeInjectedAsOtherKind = understatedEffects.row + 1;
    else if (understatedEffects.part == PartOfEffects::KindOfResult)
        m_program.builtinInjectedAsNull = understatedEffects.row;
    if (understatedEffects.part == PartOfEffects::ContentsOfResult)
        m_program.builtinInjectedAsHoldingNull = understatedEffects.row;
    if (understatedEffects.part == PartOfEffects::FreshnessOfResult)
        m_program.builtinInjectedAsFresh = understatedEffects.row;
    m_program.admitsInspector = m_build.options().admitsUnseenCode();
    m_program.excludedCode = m_build.options().excludedCode;

    const ParallelLoops& loops = m_build.parallelLoops();
    StepTimes steps(m_build, stepsOfRead);
    if (!readHeader())
        return false;
    steps.didFinish("header and bytes"_s);

    m_types.emplace(m_program, m_sections, m_build, m_externalBodies);
    readList(m_sections.replaceableBuiltins, m_program.replaceableBuiltins);
    readList(m_sections.openedNames, m_program.openedNames);
    bool unlockingReadsStrings = !m_program.openedNames.isEmpty();
    if (!unlockingReadsStrings)
        m_program.unlockReplaceableBuiltins();
    auto makeObjectsAndFamiliesOfFunctions = [&] {
        StepTimes stepsOfLane(m_build, stepsOfReadSideBySide);
        m_types->makeObjects();
        stepsOfLane.didFinish("objects of functions"_s);
        m_types->computeFunctionFamilies();
        stepsOfLane.didFinish("families of functions"_s);
    };
    bool hasDeclaredMembers = false;
    runLanes(loops, {
        [&] {
            StepTimes stepsOfLane(m_build, stepsOfReadSideBySide);
            hasDeclaredMembers = m_types->computeDeclaredMembers();
            stepsOfLane.didFinish("families of layouts"_s);
        },
        [&] {
            StepTimes stepsOfLane(m_build, stepsOfReadSideBySide);
            readStrings();
            stepsOfLane.didFinish("strings"_s);
            if (!unlockingReadsStrings)
                return;
            m_program.unlockReplaceableBuiltins();
            makeObjectsAndFamiliesOfFunctions();
        },
        [&] {
            if (!unlockingReadsStrings)
                makeObjectsAndFamiliesOfFunctions();
        },
        [&] {
            StepTimes stepsOfLane(m_build, stepsOfReadSideBySide);
            readPropertyKeys();
            stepsOfLane.didFinish("property keys"_s);
            makeRoomForTables();
            stepsOfLane.didFinish("room for the tables"_s);
        },
    });
    if (!hasDeclaredMembers || !m_types->reportErrorInFunctionFamilies())
        return false;
    if (!checkListSize(m_sections.layouts.size() + m_types->numberOfDerivedLayouts(), sizeof(Layout), m_build))
        return false;
    steps.didFinish("families of layouts beside strings, objects, families of functions and property keys"_s);
    m_types->readTypes();
    steps.didFinish("types"_s);
    m_program.unitsOfBodies.initialize(m_externalBodies, m_program.types.typesOfWireIndexes(), numbersOfRecords(), loops);
    steps.didFinish("units of bodies"_s);

    runLanes(loops, {
        [&] {
            StepTimes stepsOfLane(m_build, stepsOfReadSideBySide);
            readList(m_sections.typeSites, m_program.typeSites);
            readProgramLists();
            readLayouts();
            stepsOfLane.didFinish("lists and layouts"_s);
            m_types->finishLayouts();
            stepsOfLane.didFinish("finish layouts"_s);
        },
        [&] {
            StepTimes stepsOfLane(m_build, stepsOfReadSideBySide);
            readFunctions();
            stepsOfLane.didFinish("functions"_s);
            m_types->findModulesNamedByBodies();
            stepsOfLane.didFinish("modules that bodies name"_s);
        },
    });
    steps.didFinish("lists and layouts beside functions"_s);

    m_program.types.reserveMembersOfLayoutSets(static_cast<uint32_t>(m_program.layouts.size()));
    runLanes(loops, {
        [&] {
            StepTimes stepsOfLane(m_build, stepsOfReadSideBySide);
            m_types->finishObjects();
            setConstructedClasses();
            stepsOfLane.didFinish("finish objects"_s);
        },
        [&] {
            StepTimes stepsOfLane(m_build, stepsOfReadSideBySide);
            m_program.indexViews(m_build.parallelLoops());
            stepsOfLane.didFinish("index of views"_s);
            setParameterSiteFunctions();
            stepsOfLane.didFinish("functions of parameter sites"_s);
        },
        [&] {
            StepTimes stepsOfLane(m_build, stepsOfReadSideBySide);
            m_program.indexClasses();
            stepsOfLane.didFinish("index of classes"_s);
            m_program.indexNames(m_build.parallelLoops());
            stepsOfLane.didFinish("index of names"_s);
        },
    });
    steps.didFinish("finish objects beside the indexes"_s);
    return true;
}

bool Reader::report(const InternalError& error)
{
    m_build.diagnostics().add(error);
    return false;
}

template<typename WireRecord, typename Record, size_t inlineCapacity>
void Reader::readList(std::span<const WireRecord> records, Vector<Record, inlineCapacity>& result)
{
    constexpr size_t recordsPerChunk = 64;
    const ParallelLoops& loops = m_build.parallelLoops();
    growFromEmpty(loops, result, records.size());
    readInChunks(loops, records.size(), recordsPerChunk, [&](uint32_t index) {
        RecordChecker checker(*this);
        read(checker, records[index], result[index]);
    });
}

bool Reader::readHeader()
{
    if (m_buffer.size() < sizeof(Wire::Header) || reinterpret_cast<uintptr_t>(m_buffer.data()) % alignof(Wire::Header))
        return report({ .kind = InternalErrorKind::ProgramTooShort });
    m_header = &reinterpretCastSpanStartTo<Wire::Header>(m_buffer);
    if (m_header->magic != Wire::Magic)
        return report({ .kind = InternalErrorKind::NotAProgram });
    if (m_schemaHashCheck == SchemaHashCheck::Required && static_cast<uint64_t>(m_header->schemaHash) != Wire::schemaHash)
        return report({ .kind = InternalErrorKind::SchemaMismatch });
    if (m_header->sizeInBytes != m_buffer.size())
        return report({ .kind = InternalErrorKind::ProgramTooShort });

    auto sections = Wire::Sections::open(m_buffer, *m_header);
    if (!sections)
        return report({ .kind = InternalErrorKind::SectionOutOfBounds, .count = sections.error() });
    m_sections = *sections;
    if (!tablesHaveRoomFor(m_sections))
        return false;
    if (!m_program.ownsBuffer())
        m_sections.bytes = m_program.copyBorrowedBytes(m_sections.bytes, m_build.parallelLoops());
    return true;
}

void Reader::readStrings()
{
    constexpr size_t stringsPerChunk = 1024;
    auto records = m_sections.strings;
    Vector<StringAsRead> strings(records.size());
    readInChunks(m_build.parallelLoops(), records.size(), stringsPerChunk, [&](uint32_t index) {
        const Wire::String& wire = records[index];
        RecordChecker checker(*this);
        strings[index] = {
            checker.bytes({ wire.offset, wire.length }),
            checker.enumerator<NameClass>(wire.nameClass),
            checker.optionalExternalID<BuiltinName>(wire.builtinName),
        };
    });
    m_program.strings.initialize(m_builtins, m_build.parallelLoops(), WTF::move(strings));
}

void Reader::readPropertyKeys()
{
    auto records = m_sections.propertyKeys;
    ArenasOfWorkers arenas(m_program.types, m_build.parallelLoops());
    Vector<PropertyKeyID> ids(records.size());
    arenas.internInOrder(ids.mutableSpan(), [&](uint32_t index) {
        RecordChecker checker(*this);
        PropertyKey key;
        read(checker, records[index], key);
        return key;
    });
    for (size_t index = 0; index < ids.size(); ++index)
        RELEASE_ASSERT(ids[index].index() == index);
}

NumbersOfProgramRecords Reader::numbersOfRecords() const
{
    static_assert(numberOfRemappedIDKinds == 12);
    NumbersOfProgramRecords numbers;
    numbers[RemappedIDKind::String] = m_sections.strings.size();
    numbers[RemappedIDKind::PropertyKey] = m_sections.propertyKeys.size();
    numbers[RemappedIDKind::Type] = m_sections.types.size();
    numbers[RemappedIDKind::TypeSite] = m_sections.typeSites.size();
    numbers[RemappedIDKind::ExpectedResult] = m_sections.expectedResults.size();
    numbers[RemappedIDKind::Layout] = m_sections.layouts.size();
    numbers[RemappedIDKind::Variable] = m_sections.variables.size();
    numbers[RemappedIDKind::Module] = m_sections.modules.size();
    numbers[RemappedIDKind::ModuleFailure] = m_sections.moduleFailures.size();
    numbers[RemappedIDKind::EvalScope] = m_sections.evalScopes.size();
    numbers[RemappedIDKind::Function] = m_sections.functions.size();
    numbers[RemappedIDKind::Data] = m_sections.data.size();
    return numbers;
}

void Reader::forEachListReadIntoTableOfItsName(const Wire::Sections& sections, const auto& functor)
{
    functor(sections.expectedResults, m_program.expectedResults);
    functor(sections.variables, m_program.variables);
    functor(sections.modules, m_program.modules);
    functor(sections.moduleFailures, m_program.moduleFailures);
    functor(sections.ambiguousExports, m_program.ambiguousExports);
    functor(sections.sourceStamps, m_program.sourceStamps);
    functor(sections.declaredRecords, m_program.declaredRecords);
    functor(sections.evalScopes, m_program.evalScopes);
    functor(sections.data, m_program.data);
    functor(sections.entries, m_program.entries);
    functor(sections.runtimeFunctions, m_program.runtimeFunctions);
    functor(sections.webAssemblyModules, m_program.webAssemblyModules);
    functor(sections.builtinsUsedAtStart, m_program.builtinsUsedAtStart);
}

void Reader::makeRoomForTables()
{
    const ParallelLoops& loops = m_build.parallelLoops();
    makeRoomFor(loops, m_program.functions, m_sections.functions.size());
    makeRoomFor(loops, m_program.typeSites, m_sections.typeSites.size());
    forEachListReadIntoTableOfItsName(m_sections, [&](auto records, auto& table) {
        makeRoomFor(loops, table, records.size());
    });
}

void Reader::readProgramLists()
{
    forEachListReadIntoTableOfItsName(m_sections, [&](auto records, auto& table) {
        readList(records, table);
    });
}

bool Reader::tablesHaveRoomFor(const Wire::Sections& sections)
{
    bool haveRoom = checkListSize(sections.strings.size(), sizeof(StringAsRead), m_build);
    auto ask = [&]<typename Record, size_t inlineCapacity>(auto records, const Vector<Record, inlineCapacity>&) {
        haveRoom = haveRoom && checkListSize(records.size(), sizeof(Record), m_build);
    };
    ask(sections.functions, m_program.functions);
    ask(sections.typeSites, m_program.typeSites);
    ask(sections.replaceableBuiltins, m_program.replaceableBuiltins);
    ask(sections.openedNames, m_program.openedNames);
    forEachListReadIntoTableOfItsName(sections, ask);
    return haveRoom;
}

void Reader::readLayouts()
{
    constexpr size_t layoutsPerChunk = 64;
    const ParallelLoops& loops = m_build.parallelLoops();
    auto records = m_sections.layouts;
    makeRoomFor(loops, m_program.layouts, records.size() + m_types->numberOfDerivedLayouts());
    growFromEmpty(loops, m_program.layouts, records.size());
    readInChunks(loops, records.size(), layoutsPerChunk, [&](uint32_t index) {
        RecordChecker checker(*this);
        read(checker, records[index], static_cast<LayoutRecord&>(m_program.layouts[index]));
        std::span<const Wire::Field> fields = checker.slice<Wire::Field>(records[index].fields);
        for (size_t field = 0; field < fields.size(); ++field) {
            DeclaredType& declared = m_program.layouts[index].fields[field].declared;
            if (declared.isEstablished())
                continue;
            if (TypeID expected = m_types->typeExpectedBesideViews(fields[field].declared.type))
                declared.expect(ExpectedType(expected));
        }
    });
}

void Reader::setConstructedClasses()
{
    for (uint32_t index = 0; index < m_program.layouts.size(); ++index) {
        FunctionID constructor = m_program.layouts[index].constructor;
        if (!constructor || constructor.index() >= m_program.functions.size())
            continue;
        Function& function = m_program.at(constructor);
        function.constructedClass = LayoutID(index);
    }
}

void Reader::setParameterSiteFunctions()
{
    struct SiteOfFunction {
        TypeSiteID site;
        FunctionID function;
    };
    Vector<SiteOfFunction> sites = collectInParallel<SiteOfFunction>(m_build.parallelLoops(), static_cast<unsigned>(m_program.functions.size()), [&](unsigned index, Vector<SiteOfFunction>& sites) {
        for (const DeclaredType& parameter : m_program.functions[index].parameters) {
            if (parameter.site && parameter.site.index() < m_program.typeSites.size())
                sites.append({ parameter.site, FunctionID(index) });
        }
    });
    for (SiteOfFunction site : sites)
        m_program.typeSites[site.site.index()].function = site.function;
}

void RecordChecker::didRead(LayoutRecord& record)
{
    auto firstInherited = std::ranges::find_if(record.fields, [](const Field& field) {
        return field.flags.contains(FieldFlags::IsInherited);
    });
    record.ownFieldCount = static_cast<uint32_t>(firstInherited - record.fields.begin());
}

static std::unique_ptr<Program> readProgramFromBytes(std::span<const uint8_t> bytes, const ExternalFunctionBodies* externalBodies, BuildContext& build, SchemaHashCheck schemaHashCheck)
{
    std::unique_ptr<Program> program;
    {
        StepTimes steps(build, stepsOfRead);
        program = makeUnique<Program>(BuiltinTable::singleton());
        steps.didFinish("table of built-ins"_s);
    }
    program->borrowBuffer(bytes);
    Reader reader(*program, build);
    reader.setExternalFunctionBodies(externalBodies);
    reader.setSchemaHashCheck(schemaHashCheck);
    if (!reader.run())
        return nullptr;
    StepTimes steps(build, stepsOfRead);
    program->copyBorrowedWordsOfPositions(build.parallelLoops());
    steps.didFinish("words of positions"_s);
    return program;
}

std::unique_ptr<Program> readProgramWithExternalBodies(std::span<const uint8_t> bytes, const ExternalFunctionBodies* externalBodies, BuildContext& build)
{
    return readProgramFromBytes(bytes, externalBodies, build, SchemaHashCheck::Required);
}

std::unique_ptr<Program> Program::read(std::span<const uint8_t> bytes, BuildContext& build, SchemaHashCheck schemaHashCheck)
{
    return readProgramFromBytes(bytes, nullptr, build, schemaHashCheck);
}

} } } // namespace JSC::AOT::Typed

#if COMPILER(CLANG)
#pragma clang attribute pop
#endif

#endif // ENABLE(AOT)
