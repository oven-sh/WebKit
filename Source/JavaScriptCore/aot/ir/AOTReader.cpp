/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTReaderInternal.h"

#if ENABLE(AOT)

#include "AOTFlowSiteData.h"
#include "AOTImagesBelow.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTTypeClosure.h"
#include "AOTVector.h"
#include "AOTVerifierInternal.h"
#include <algorithm>
#include <compare>
#include <wtf/StdLibExtras.h>

#if COMPILER(CLANG)
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
#endif

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
        && readNamesAndFamilies()
        && readReplaceableBuiltins()
        && readTypes()
        && readListsLayoutsAndFunctions()
        && finishProgram();
}

// Before anything asks the table about a lock, and while one thread reads.
bool Reader::readReplaceableBuiltins()
{
    auto& members = m_program.replaceableBuiltins;
    if (!readList(m_sections.replaceableBuiltins, members) || !readList(m_sections.openedNames, m_program.openedNames))
        return false;
    for (size_t index = 1; index < members.size(); ++index) {
        RecordChecker checker(*this, inRecord(static_cast<uint32_t>(index)));
        checker.requireAscending(members[index - 1].member, members[index].member);
        if (auto failure = checker.takeFailure())
            return report(*failure);
    }
    if (const ImagesBelow* below = m_build.imagesBelow()) {
        below->forEachUnlockedBuiltinMember([&](BuiltinID member) {
            members.append(ReplaceableBuiltin { member });
        });
        AOT::sort(members, { }, &ReplaceableBuiltin::member);
        members.shrink(members.size() - std::ranges::unique(members, { }, &ReplaceableBuiltin::member).size());
    }
    m_program.takesAnyNameFromTests = m_build.options().holdsLibraryAlone();
    m_program.unlockReplaceableBuiltins();
    return true;
}

bool Reader::readNamesAndFamilies()
{
    const auto& options = m_build.options();
    // The families take in the records of the images below by the texts of their names.
    if (options.verifiesProgram || options.isVerifyOnly() || m_build.imagesBelow())
        return readStrings() && readPropertyKeys() && computeFamilies();

    bool hasStrings = false;
    bool hasPropertyKeys = false;
    bool hasFamilies = false;
    runTasksInParallel(m_build.parallelLoops(), {
        [&] {
            hasFamilies = computeFamilies();
        },
        [&] {
            hasStrings = readStrings();
        },
        [&] {
            hasPropertyKeys = readPropertyKeys();
        },
    });
    return hasStrings && hasPropertyKeys && hasFamilies;
}

// The value flow analysis identifies a string by its key. Every string held by an operation or constant data has one, whether or not a property has that name.
static void internKeysOfStringConstants(Program& program, const ParallelLoops& loops)
{
    Vector<StringID> ofOperations = collectInParallel<StringID>(loops, program.functions.size(), [&](unsigned index, Vector<StringID>& found) {
        for (const Value& value : program.functions[index].values.span()) {
            if (value.opcode == Opcode::String)
                found.append(value.string());
        }
    });
    Vector<StringID> ofData = collectInParallel<StringID>(loops, program.data.size(), [&](unsigned index, Vector<StringID>& found) {
        forEachDataNodeInPreorder(program.data[index].root, [&](uint32_t, const DataNode& node) {
            if (const auto* text = node.tryAs<DataNode::String>())
                found.append(text->value);
        });
    });
    for (const Vector<StringID>& texts : { ofOperations, ofData }) {
        for (StringID text : texts)
            program.types.intern(PropertyKey { .kind = PropertyKeyKind::Text, .text = text });
    }
}

bool Reader::readListsLayoutsAndFunctions()
{
    if (!readListInParallel(m_sections.typeSites, m_program.typeSites))
        return false;
    if (m_form == IRForm::AsRead)
        m_layoutUses.emplace(m_program, numberOfRecords<LayoutID>(), m_build.parallelLoops().width());
    if (m_preparesFunctionsForTyping) {
        m_longExpectations.emplace(m_program, m_build.parallelLoops());
        m_constructorsOfTaggedFamilies.ensureSize(m_sections.functions.size());
        for (const TaggedFamily& family : m_program.taggedFamilies) {
            uint32_t constructor = m_sections.layouts[family.maker.index()].constructor;
            if (constructor < m_sections.functions.size())
                m_constructorsOfTaggedFamilies.quickSet(constructor);
        }
        m_program.setReaderPreparedFunctionsForTyping();
    }

    const auto& options = m_build.options();
    if (options.verifiesProgram || options.isVerifyOnly()) {
        if (!readProgramLists() || !readLayouts() || !reportFirst(readFunctions().span()))
            return false;
        internKeysOfStringConstants(m_program, m_build.parallelLoops());
        return true;
    }

    bool hasListsAndLayouts = false;
    Vector<Failure> failuresInFunctions;
    runTasksInParallel(m_build.parallelLoops(), {
        [&] {
            hasListsAndLayouts = readProgramLists() && readLayouts();
        },
        [&] {
            failuresInFunctions = readFunctions();
        },
    });
    if (!hasListsAndLayouts || !reportFirst(failuresInFunctions.span()))
        return false;
    internKeysOfStringConstants(m_program, m_build.parallelLoops());
    return true;
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
    growFromEmpty(m_build.parallelLoops(), result, records.size());
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
    if (!m_program.ownsBuffer())
        m_sections.bytes = m_program.copyBorrowedBytes(m_sections.bytes, m_build.parallelLoops());
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

    auto whatIsSaid = m_preparesFunctionsForTyping ? StringTable::WhatIsSaidOfStrings::IsBelieved : StringTable::WhatIsSaidOfStrings::IsChecked;
    auto result = m_program.strings.initialize(m_builtins, m_build.parallelLoops(), WTF::move(strings), whatIsSaid);
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
    bool isValid = readList(m_sections.expectedResults, m_program.expectedResults)
        && readListInParallel(m_sections.variables, m_program.variables)
        && readListInParallel(m_sections.modules, m_program.modules)
        && readList(m_sections.moduleFailures, m_program.moduleFailures)
        && readList(m_sections.ambiguousExports, m_program.ambiguousExports)
        && readList(m_sections.sourceStamps, m_program.sourceStamps)
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
    if (m_layoutUses) {
        markUnallocatedLayouts(m_program, m_build.parallelLoops(), *m_layoutUses);
        m_layoutUses.reset();
        expectFieldsFilledWithOtherData(m_program, m_build);
    }
    m_program.indexClasses();
    m_program.indexNames(m_build.parallelLoops());
    setParameterSiteFunctions();
    m_program.setForm(m_form);
    return true;
}

bool Reader::setConstructedClasses()
{
    for (uint32_t index = 0; index < m_program.layouts.size(); ++index) {
        FunctionID constructor = m_program.layouts[index].constructor;
        if (!constructor)
            continue;
        Function& function = m_program.at(constructor);
        if (function.constructedClass)
            return report({ .kind = InternalErrorKind::InvalidClass, .layout = LayoutID(index) });
        function.constructedClass = LayoutID(index);
        // The body as it was written: no pass has changed it yet, and blanks and comments are not in it.
        AOT_THRESHOLD(uint32_t, mostValuesOfShortConstructor, 77);
        if (function.values.size() > mostValuesOfShortConstructor)
            function.capabilities.add(FunctionCapabilities::IsLongConstructor);
    }
    return true;
}

void Reader::setParameterSiteFunctions()
{
    struct SiteOfFunction {
        TypeSiteID site;
        FunctionID function;
    };
    Vector<SiteOfFunction> sites = collectInParallel<SiteOfFunction>(m_build.parallelLoops(), m_program.functions.size(), [&](unsigned index, Vector<SiteOfFunction>& sites) {
        for (const DeclaredType& parameter : m_program.functions[index].parameters) {
            if (parameter.site)
                sites.append({ parameter.site, FunctionID(index) });
        }
    });
    for (SiteOfFunction site : sites)
        m_program.typeSites[site.site.index()].function = site.function;
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
    require(!declared.isExpected() || !declared.has(PositionFlags::IsInt32Promise), InternalErrorKind::InvalidDeclaredType);
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

enum class ReadFor : bool { Itself, Pipeline };

static std::unique_ptr<Program> readProgram(std::span<const uint8_t> bytes, const ExternalFunctionBodies* externalBodies, BuildContext& build, ReadFor purpose)
{
    auto program = makeUnique<Program>(BuiltinTable::singleton());
    program->borrowBuffer(bytes);
    Reader reader(*program, IRForm::AsRead, build);
    reader.setExternalFunctionBodies(externalBodies);
    const auto& options = build.options();
    bool isLookedAtAsRead = build.validates() || options.verifiesProgram || options.isVerifyOnly() || options.logging.dumpedPass;
    if (purpose == ReadFor::Pipeline && !isLookedAtAsRead)
        reader.setPreparesFunctionsForTyping();
    if (!reader.run())
        return nullptr;
    program->copyBorrowedWordsOfPositions(build.parallelLoops());
    program->collectConstructionStarts();
    program->borrowBuffer({ });
    return program;
}

std::unique_ptr<Program> readProgramWithExternalBodies(std::span<const uint8_t> bytes, const ExternalFunctionBodies* externalBodies, BuildContext& build)
{
    return readProgram(bytes, externalBodies, build, ReadFor::Pipeline);
}

std::unique_ptr<Program> Program::read(std::span<const uint8_t> bytes, BuildContext& build)
{
    return readProgram(bytes, nullptr, build, ReadFor::Itself);
}

} } } // namespace JSC::AOT::Typed

#if COMPILER(CLANG)
#pragma clang attribute pop
#endif

#endif // ENABLE(AOT)
