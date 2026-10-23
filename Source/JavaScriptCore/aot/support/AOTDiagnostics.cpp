/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTDiagnostics.h"

#if ENABLE(AOT)

#include "AOTDiagnosticMessages.h"
#include "AOTIR.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <tuple>
#include <utility>

namespace JSC { namespace AOT {

static const Typed::Function* tryGetFunction(const Typed::Program* program, Typed::FunctionID id)
{
    if (!program || id.index() >= program->functions.size())
        return nullptr;
    return &program->functions[id.index()];
}

static const Typed::Layout* tryGetLayout(const Typed::Program* program, Typed::LayoutID id)
{
    if (!program || id.index() >= program->layouts.size())
        return nullptr;
    return &program->layouts[id.index()];
}

Typed::FunctionID findFunctionAsRead(const Typed::Program* program, Typed::FunctionID id)
{
    // The chain is cyclic only in a program that the verifier rejected, whose diagnostics come through here.
    Vector<Typed::FunctionID, 4> visited;
    while (auto* function = tryGetFunction(program, id)) {
        if (id.index() < program->numberOfFunctionsAsRead())
            return id;
        if (visited.contains(id))
            break;
        visited.append(id);
        id = function->derivation.source;
    }
    return { };
}

Typed::LayoutID findLayoutAsRead(const Typed::Program* program, Typed::LayoutID id)
{
    if (!program || id.index() >= program->numberOfLayoutsAsRead())
        return { };
    return id;
}

std::optional<SourceLocation> findLocation(const Typed::Program* program, const Subject& subject)
{
    if (hasModule(subject.location))
        return subject.location;
    if (auto* function = tryGetFunction(program, subject.function)) {
        if (subject.value.index() < function->values.size()) {
            SourceLocation location = function->locationOf(subject.value);
            if (hasModule(location))
                return location;
        }
        SourceLocation declaration = function->declarationLocation();
        if (hasModule(declaration))
            return declaration;
    }
    auto* layout = tryGetLayout(program, subject.layout);
    if (!layout || !layout->module)
        return std::nullopt;
    return SourceLocation { layout->module.index(), layout->position };
}

template<typename Record>
static void resolveLocations(Vector<Record>& records, const Typed::Program* program)
{
    if (!program)
        return;
    for (Record& record : records) {
        if (hasModule(record.location))
            continue;
        Subject subject { .function = record.function, .block = record.block, .value = record.value, .layout = record.layout };
        if (auto location = findLocation(program, subject))
            record.location = *location;
    }
}

static auto orderKey(const UserDiagnostic& diagnostic)
{
    const auto& [kind, function, block, value, layout, limit, count, location, origin, siteKind, minimum, maximum] = diagnostic;
    return std::tuple { location.module, location.position, function, block, value, layout, kind, limit, count, origin.module, origin.position, siteKind, minimum, maximum };
}

namespace {

struct OrderedText {
    StringView text;

    friend bool operator==(const OrderedText& a, const OrderedText& b) { return a.text == b.text; }
    friend std::weak_ordering operator<=>(const OrderedText& a, const OrderedText& b) { return codePointCompare(a.text, b.text); }
};

} // anonymous namespace

static auto orderKey(const InternalError& error)
{
    const auto& [kind, function, block, value, layout, limit, count, location, textPosition, pass, detail] = error;
    LineColumn lineColumn = textPosition.value_or(LineColumn { });
    return std::tuple { location.module, location.position, function, block, value, layout, kind, limit, count, pass, lineColumn.line, lineColumn.column, OrderedText { detail } };
}

template<typename Record>
static void sortRecords(Vector<Record>& records)
{
    AOT::sort(records, { }, [](const Record& record) {
        return orderKey(record);
    });
}

template<typename Record>
static void removeDuplicates(Vector<Record>& records)
{
    removeRepeatedElements(records, [](const Record& a, const Record& b) {
        return orderKey(a) == orderKey(b);
    });
}

template<typename Record>
static void convertToIDsAsRead(Vector<Record>& records, const Typed::Program* program)
{
    if (!program)
        return;
    for (Record& record : records) {
        Typed::FunctionID function = findFunctionAsRead(program, record.function);
        if (function != record.function) {
            record.function = function;
            record.block = { };
            record.value = { };
        }
        record.layout = findLayoutAsRead(program, record.layout);
    }
}

template<typename Record>
static Vector<String> finishRecords(Vector<Record>& records, const Typed::Program* program)
{
    resolveLocations(records, program);
    sortRecords(records);
    removeDuplicates(records);
    Vector<String> messages = WTF::map(records, [&](const Record& record) {
        return formatMessage(record, program);
    });
    convertToIDsAsRead(records, program);
    return messages;
}

template<typename Record>
static void assertNoProvisionalFunctions(const Vector<Record>& records)
{
    for (const Record& record : records)
        RELEASE_ASSERT(!Typed::ProvisionalFunctions::contains(record.function));
}

Diagnostics::Diagnostics(Diagnostics&& other)
{
    *this = WTF::move(other);
}

Diagnostics& Diagnostics::operator=(Diagnostics&& other) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    if (this == &other)
        return *this;
    m_userDiagnostics = std::exchange(other.m_userDiagnostics, { });
    m_internalErrors = std::exchange(other.m_internalErrors, { });
    m_userDiagnosticMessages = std::exchange(other.m_userDiagnosticMessages, { });
    m_internalErrorMessages = std::exchange(other.m_internalErrorMessages, { });
    m_hasErrors.store(other.m_hasErrors.exchange(false, std::memory_order_relaxed), std::memory_order_relaxed);
    m_isFinished = std::exchange(other.m_isFinished, false);
    return *this;
}

void Diagnostics::add(const UserDiagnostic& diagnostic)
{
    RELEASE_ASSERT(!Typed::ProvisionalFunctions::contains(diagnostic.function));

    Locker locker { m_lock };
    RELEASE_ASSERT(!m_isFinished);
    m_userDiagnostics.append(diagnostic);
    if (failsBuild(diagnostic.kind))
        m_hasErrors.store(true, std::memory_order_relaxed);
}

void Diagnostics::add(const InternalError& error)
{
    RELEASE_ASSERT(!Typed::ProvisionalFunctions::contains(error.function));

    Locker locker { m_lock };
    RELEASE_ASSERT(!m_isFinished);
    m_internalErrors.append(error);
    m_hasErrors.store(true, std::memory_order_relaxed);
}

void Diagnostics::append(Vector<UserDiagnostic>&& diagnostics)
{
    if (diagnostics.isEmpty())
        return;
    assertNoProvisionalFunctions(diagnostics);
    bool containsError = diagnostics.containsIf([](const UserDiagnostic& diagnostic) {
        return failsBuild(diagnostic.kind);
    });

    Locker locker { m_lock };
    RELEASE_ASSERT(!m_isFinished);
    m_userDiagnostics.appendVector(WTF::move(diagnostics));
    if (containsError)
        m_hasErrors.store(true, std::memory_order_relaxed);
}

void Diagnostics::append(Vector<InternalError>&& errors)
{
    if (errors.isEmpty())
        return;
    assertNoProvisionalFunctions(errors);

    Locker locker { m_lock };
    RELEASE_ASSERT(!m_isFinished);
    m_internalErrors.appendVector(WTF::move(errors));
    m_hasErrors.store(true, std::memory_order_relaxed);
}

void Diagnostics::resolveLocations(const Typed::Program& program)
{
    Locker locker { m_lock };
    RELEASE_ASSERT(!m_isFinished);
    AOT::resolveLocations(m_userDiagnostics, &program);
    AOT::resolveLocations(m_internalErrors, &program);
}

void Diagnostics::finish(const Typed::Program* program)
{
    Locker locker { m_lock };
    RELEASE_ASSERT(!m_isFinished);
    m_userDiagnosticMessages = finishRecords(m_userDiagnostics, program);
    m_internalErrorMessages = finishRecords(m_internalErrors, program);
    m_isFinished = true;
}

const Vector<UserDiagnostic>& Diagnostics::userDiagnostics() const WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    RELEASE_ASSERT(m_isFinished);
    return m_userDiagnostics;
}

const Vector<InternalError>& Diagnostics::internalErrors() const WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    RELEASE_ASSERT(m_isFinished);
    return m_internalErrors;
}

const String& Diagnostics::messageOfUserDiagnostic(size_t index) const WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    RELEASE_ASSERT(m_isFinished);
    return m_userDiagnosticMessages[index];
}

const String& Diagnostics::messageOfInternalError(size_t index) const WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    RELEASE_ASSERT(m_isFinished);
    return m_internalErrorMessages[index];
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
