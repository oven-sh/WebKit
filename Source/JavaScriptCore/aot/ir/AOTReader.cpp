/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTReaderInternal.h"

#if ENABLE(AOT)

#include "AOTPhaseTimes.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include <wtf/StdLibExtras.h>

#if COMPILER(CLANG)
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
#endif

namespace JSC { namespace AOT { namespace Typed {

// The driver's line of the read counts all of these.
static constexpr ASCIILiteral stepsOfRead = "ReadAndVerify: read"_s;
// Of a row of steps that runs beside another row. The line of the read that names both rows counts the longer one.
static constexpr ASCIILiteral stepsOfReadSideBySide = "ReadAndVerify: read, side by side"_s;

Reader::Reader(Program& program, BuildContext& build)
    : m_program(program)
    , m_build(build)
    , m_builtins(program.builtins())
    , m_buffer(program.buffer())
{
}

// Rows of steps of which none reads what another writes run side by side, as lanes (AOTThreads.h): each has stretches of one thread, and loops that leave threads idle.
bool Reader::run()
{
    m_program.admitsReplacedBuiltins = m_build.options().admitsReplacedBuiltins();
    m_program.rowsOfNativeCodeWriteNullish = AOT::hasFlag(m_build.options().testing, Wire::TestingSwitch::NativeCodeAnswersNullish);
    m_program.admitsInspector = m_build.options().admitsUnseenCode();
    m_program.excludedCode = m_build.options().excludedCode;

    const ParallelLoops& loops = m_build.parallelLoops();
    StepTimes steps(m_build, stepsOfRead);
    if (!readHeader())
        return false;
    steps.didFinish("header and bytes"_s);
    // A key names its text by number.
    runLanes(loops, {
        [&] {
            StepTimes stepsOfLane(m_build, stepsOfReadSideBySide);
            readStrings();
            stepsOfLane.didFinish("strings"_s);
        },
        [&] {
            StepTimes stepsOfLane(m_build, stepsOfReadSideBySide);
            readPropertyKeys();
            stepsOfLane.didFinish("property keys"_s);
        },
    });
    readReplaceableBuiltins();
    steps.didFinish("strings beside property keys"_s);

    m_types.emplace(m_program, m_sections, m_build, m_externalBodies);
    if (!m_types->makeObjectsAndComputeFamilies())
        return false;
    steps.didFinish("families of layouts beside objects and families of functions"_s);
    m_types->readTypes();
    steps.didFinish("types"_s);
    m_program.unitsOfBodies.initialize(m_externalBodies, m_program.types.typesOfWireIndexes(), numbersOfRecords(), loops);
    steps.didFinish("units of bodies"_s);

    // A record of a function names everything but its types and its unit by number, and nothing that finishes the layouts asks for a function. Keys and types are interned by the first row alone.
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
        },
    });
    steps.didFinish("lists and layouts beside functions"_s);
    m_types->finishObjects();
    steps.didFinish("finish objects"_s);
    finishProgram();
    steps.didFinish("finish the program"_s);
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
    // The image is built from strings and texts when the memory of the front end is gone.
    if (!m_program.ownsBuffer())
        m_sections.bytes = m_program.copyBorrowedBytes(m_sections.bytes, m_build.parallelLoops());
    return true;
}

// Unlocking rewrites the table of built-ins, so it must happen before anything asks the table about a lock and while no other thread reads the table.
void Reader::readReplaceableBuiltins()
{
    readList(m_sections.replaceableBuiltins, m_program.replaceableBuiltins);
    readList(m_sections.openedNames, m_program.openedNames);
    m_program.unlockReplaceableBuiltins();
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
    // Records and bodies name a key by its place in this list. A key that repeats an earlier one would give every later key another number.
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

void Reader::readProgramLists()
{
    readList(m_sections.expectedResults, m_program.expectedResults);
    readList(m_sections.variables, m_program.variables);
    readList(m_sections.modules, m_program.modules);
    readList(m_sections.moduleFailures, m_program.moduleFailures);
    readList(m_sections.ambiguousExports, m_program.ambiguousExports);
    readList(m_sections.sourceStamps, m_program.sourceStamps);
    readList(m_sections.evalScopes, m_program.evalScopes);
    readList(m_sections.data, m_program.data);
    readList(m_sections.entries, m_program.entries);
    readList(m_sections.runtimeFunctions, m_program.runtimeFunctions);
    readList(m_sections.webAssemblyModules, m_program.webAssemblyModules);
}

void Reader::readLayouts()
{
    constexpr size_t layoutsPerChunk = 64;
    const ParallelLoops& loops = m_build.parallelLoops();
    auto records = m_sections.layouts;
    m_program.layouts.reserveInitialCapacity(records.size() + m_types->numberOfDerivedLayouts());
    growFromEmpty(loops, m_program.layouts, records.size());
    readInChunks(loops, records.size(), layoutsPerChunk, [&](uint32_t index) {
        RecordChecker checker(*this);
        read(checker, records[index], static_cast<LayoutRecord&>(m_program.layouts[index]));
    });
}

void Reader::finishProgram()
{
    StepTimes steps(m_build, "ReadAndVerify: finish the program"_s);
    setConstructedClasses();
    steps.didFinish("constructed classes"_s);
    m_program.indexClasses();
    steps.didFinish("index of classes"_s);
    m_program.indexNames(m_build.parallelLoops());
    steps.didFinish("index of names"_s);
    m_program.indexViews(m_build.parallelLoops());
    steps.didFinish("index of views"_s);
    setParameterSiteFunctions();
    steps.didFinish("functions of parameter sites"_s);
}

// Nothing has checked the ids of records yet: verifyRecords() runs behind the read and reports an id past the end of its list. So this and setParameterSiteFunctions() pass such an id by.
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

// The front end sends a layout's own fields alone. The text of a program that was read once has behind them those that the layout takes from its bases.
void RecordChecker::didRead(LayoutRecord& record)
{
    auto firstInherited = std::ranges::find_if(record.fields, [](const Field& field) {
        return field.flags.contains(FieldFlags::IsInherited);
    });
    record.ownFieldCount = static_cast<uint32_t>(firstInherited - record.fields.begin());
}

// The program goes on borrowing `bytes`: the bodies of its functions lie there, unless they lie where the external bodies point.
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
