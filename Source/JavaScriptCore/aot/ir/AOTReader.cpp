/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTReaderInternal.h"

#if ENABLE(AOT)

#include "AOTVerifierInternal.h"
#include <algorithm>
#include <compare>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

Reader::Reader(Program& program, IRForm form, BuildContext& build)
    : m_program(program)
    , m_form(form)
    , m_build(build)
    , m_builtins(program.builtins())
    , m_buffer(program.buffer())
{
}

bool Reader::run()
{
    return readHeader()
        && readStrings()
        && readPropertyKeys()
        && computeDeclaredMembers()
        && computeFunctionFamilies()
        && readTypes()
        && computePromisedTypes()
        && readProgramLists()
        && readLayouts()
        && readFunctions()
        && finishProgram();
}

bool Reader::report(const InternalError& error)
{
    m_build.diagnostics().add(error);
    return false;
}

bool Reader::reportFirst(std::span<const Failure> failures)
{
    for (auto& failure : failures) {
        if (failure)
            return report(*failure);
    }
    return true;
}

template<typename WireRecord, typename Record, size_t inlineCapacity>
bool Reader::readList(std::span<const WireRecord> records, Vector<Record, inlineCapacity>& result)
{
    result.reserveInitialCapacity(records.size());
    for (size_t index = 0; index < records.size(); ++index) {
        RecordChecker checker(*this, inRecord(static_cast<uint32_t>(index)));
        Record record;
        read(checker, records[index], record);
        if (auto failure = checker.takeFailure())
            return report(*failure);
        result.append(WTF::move(record));
    }
    return true;
}

template<typename WireRecord, typename Record, size_t inlineCapacity>
bool Reader::readListInParallel(std::span<const WireRecord> records, Vector<Record, inlineCapacity>& result)
{
    constexpr size_t recordsPerChunk = 64;
    result.grow(records.size());
    auto failures = readInChunks(m_build.parallelLoops(), records.size(), recordsPerChunk, [&](uint32_t index) {
        RecordChecker checker(*this, inRecord(index));
        read(checker, records[index], result[index]);
        return checker.takeFailure();
    });
    return reportFirst(failures.span());
}

bool Reader::readHeader()
{
    if (m_buffer.size() < sizeof(Wire::Header) || reinterpret_cast<uintptr_t>(m_buffer.data()) % alignof(Wire::Header))
        return report({ .kind = InternalErrorKind::ProgramTooShort });
    m_header = &reinterpretCastSpanStartTo<Wire::Header>(m_buffer);
    if (m_header->magic != Wire::Magic)
        return report({ .kind = InternalErrorKind::NotAProgram });
    if (static_cast<uint64_t>(m_header->schemaHash) != Wire::schemaHash)
        return report({ .kind = InternalErrorKind::SchemaMismatch });
    if (m_header->sizeInBytes != m_buffer.size())
        return report({ .kind = InternalErrorKind::ProgramTooShort });

    auto sections = Wire::Sections::open(m_buffer, *m_header);
    if (!sections)
        return report({ .kind = InternalErrorKind::SectionOutOfBounds, .count = sections.error() });
    m_sections = *sections;
    return true;
}

bool Reader::readStrings()
{
    constexpr size_t stringsPerChunk = 1024;
    auto records = m_sections.strings;
    Vector<StringAsRead> strings(records.size());
    auto failures = readInChunks(m_build.parallelLoops(), records.size(), stringsPerChunk, [&](uint32_t index) {
        const Wire::String& wire = records[index];
        RecordChecker checker(*this, inRecord(index));
        strings[index] = {
            checker.bytes({ wire.offset, wire.length }),
            checker.enumerator<NameClass>(wire.nameClass),
            checker.optionalExternalID<BuiltinName>(wire.builtinName),
        };
        return checker.takeFailure();
    });
    if (!reportFirst(failures.span()))
        return false;

    auto result = m_program.strings.initialize(m_builtins, m_build.parallelLoops(), WTF::move(strings));
    switch (result.error) {
    case StringTable::Error::None:
        return true;
    case StringTable::Error::DuplicateString:
        return report({ .kind = InternalErrorKind::DuplicateString, .count = result.string.index() });
    case StringTable::Error::StringNotCanonical:
        return report({ .kind = InternalErrorKind::StringNotCanonical, .count = result.string.index() });
    case StringTable::Error::NameClassMismatch:
        return report({ .kind = InternalErrorKind::NameClassMismatch, .count = result.string.index() });
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool Reader::readPropertyKeys()
{
    auto records = m_sections.propertyKeys;
    for (size_t index = 0; index < records.size(); ++index) {
        RecordChecker checker(*this, inRecord(static_cast<uint32_t>(index)));
        PropertyKey key;
        read(checker, records[index], key);
        if (auto failure = checker.takeFailure())
            return report(*failure);
        if (m_program.types.intern(key).index() != index)
            return report({ .kind = InternalErrorKind::DuplicatePropertyKey, .count = static_cast<uint32_t>(index) });
    }
    return true;
}

bool Reader::readProgramLists()
{
    bool isValid = readListInParallel(m_sections.typeSites, m_program.typeSites)
        && readList(m_sections.expectedResults, m_program.expectedResults)
        && readListInParallel(m_sections.variables, m_program.variables)
        && readListInParallel(m_sections.modules, m_program.modules)
        && readList(m_sections.moduleFailures, m_program.moduleFailures)
        && readList(m_sections.evalScopes, m_program.evalScopes)
        && readListInParallel(m_sections.data, m_program.data)
        && readList(m_sections.entries, m_program.entries)
        && readList(m_sections.runtimeFunctions, m_program.runtimeFunctions)
        && readList(m_sections.webAssemblyModules, m_program.webAssemblyModules);
    if (!isValid)
        return false;

    auto& runtimeFunctions = m_program.runtimeFunctions;
    for (size_t index = 1; index < runtimeFunctions.size(); ++index) {
        RecordChecker checker(*this, inRecord(static_cast<uint32_t>(index)));
        checker.requireAscending(runtimeFunctions[index - 1].id, runtimeFunctions[index].id);
        if (auto failure = checker.takeFailure())
            return report(*failure);
    }
    return true;
}

bool Reader::finishProgram()
{
    if (!setConstructedClasses())
        return false;
    setParameterSiteFunctions();
    m_program.setForm(m_form);
    allocateTaggedLayouts(m_program, m_build, m_taggedFamilies.span());
    return true;
}

bool Reader::setConstructedClasses()
{
    for (uint32_t index = 0; index < m_program.layouts.size(); ++index) {
        FunctionID constructor = m_program.layouts[index].constructor;
        if (!constructor)
            continue;
        LayoutID& constructedClass = m_program.at(constructor).constructedClass;
        if (constructedClass)
            return report({ .kind = InternalErrorKind::InvalidClass, .layout = LayoutID(index) });
        constructedClass = LayoutID(index);
    }
    return true;
}

void Reader::setParameterSiteFunctions()
{
    for (uint32_t index = 0; index < m_program.functions.size(); ++index) {
        for (const DeclaredType& parameter : m_program.functions[index].parameters) {
            if (parameter.site)
                m_program.typeSites[parameter.site.index()].function = FunctionID(index);
        }
    }
}

void RecordChecker::requireAscendingStrings(StringID previous, StringID next)
{
    if (hasFailed())
        return;
    const StringTable& strings = m_reader.program().strings;
    requireAscending(compareProgramStrings(strings[previous], strings[next]));
}

void RecordChecker::didRead(DeclaredType& declared)
{
    if (m_reader.form() != IRForm::AsRead)
        return;
    if (!m_reader.promisesType(declared))
        declared.flags.remove(PositionFlags::IsInt32Promise);
    TypeID promised = m_reader.promisedTypeOf(declared);
    if (promised == declared.type)
        return;
    // A written type licenses a TypeError at a typed read. An int32 promise licenses none: it is a prediction that the build has proven.
    TypeID& asWritten = declared.flags.contains(PositionFlags::IsInt32Promise) ? declared.predicted : declared.written;
    if (!asWritten)
        asWritten = declared.type;
    declared.type = promised;
}

void RecordChecker::didRead(ClaimSource& source)
{
    require(source.kind() != ClaimSource::Kind::None, InternalErrorKind::UnknownVariantCase);
}

void RecordChecker::didRead(PropertyKey& key)
{
    require(isValidPropertyKey(key), InternalErrorKind::InvalidPropertyKey);
}

void RecordChecker::didRead(FunctionRecord& record)
{
    require(!record.results.isEmpty(), InternalErrorKind::ListOutOfBounds);
    require(record.captures.size() <= m_reader.build().limit(LimitID::MaxCaptures), InternalErrorKind::ListOutOfBounds);
}

void RecordChecker::didRead(LayoutRecord& record)
{
    record.ownFieldCount = static_cast<uint32_t>(record.fields.size());
}

std::unique_ptr<Program> Program::read(std::span<const uint8_t> bytes, BuildContext& build)
{
    auto program = makeUnique<Program>(BuiltinTable::singleton());
    program->borrowBuffer(bytes);
    if (!Reader(*program, IRForm::AsRead, build).run())
        return nullptr;
    return program;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
