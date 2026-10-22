/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTCompileOptionWords.h"
#include "AOTIR.h"
#include "AOTIRPrinter.h"
#include "AOTIRTestSupport.h"
#include "AOTNativeTest.h"
#include "AOTReaderFixture.h"
#include "AOTReaderInternal.h"
#include "AOTVerifier.h"
#include "AOTWire.h"
#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <wtf/DataLog.h>
#include <wtf/Function.h>
#include <wtf/StdLibExtras.h>
#include <wtf/StringPrintStream.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace ReaderTests {

using namespace Typed;
using Kind = InternalErrorKind;
using List = ProgramList;

inline constexpr uint32_t hugeCount = 0x7fffffff;
inline constexpr uint32_t invalidExternal = 0xfffffffe;
inline constexpr uint32_t highBit = 0x80000000;
inline constexpr size_t wordSize = sizeof(uint32_t);

inline constexpr LayoutID L0 { 0u };
inline constexpr LayoutID L1 { 1u };
inline constexpr LayoutID L2 { 2u };
inline constexpr LayoutID L3 { 3u };
inline constexpr ModuleID m0 { 0u };
inline constexpr ModuleID m1 { 1u };
inline constexpr FunctionID f1 { 1u };
inline constexpr FunctionID f5 { 5u };
inline constexpr FunctionID f7 = readerFixtureStreamFunction;
inline constexpr FunctionID f8 = readerFixtureHandlerFunction;

struct ReadResult {
    bool didRead;
    Vector<InternalError> errors;
};

struct Damage {
    ASCIILiteral name;
    size_t offset;
    uint32_t word;
    Kind expected;
};

inline Wire::CompileOptions createOptions()
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    options.threadCount = 1;
    return options;
}

inline ReadResult finishRead(BuildContext& build, const Program* program)
{
    Diagnostics& diagnostics = build.diagnostics();
    diagnostics.finish(program);
    AOT_TEST_CHECK(diagnostics.userDiagnostics().isEmpty());
    for (size_t index = 0; index < diagnostics.internalErrors().size(); ++index)
        AOT_TEST_CHECK(!diagnostics.messageOfInternalError(index).isEmpty());
    return { !!program, diagnostics.internalErrors() };
}

inline ReadResult readBytes(std::span<const uint8_t> bytes)
{
    BuildContext build(createOptions());
    std::unique_ptr<Program> program = Program::read(bytes, build);
    AOT_TEST_CHECK(!program == build.diagnostics().hasErrors());
    if (program) {
        typeProgramAsRead(*program, build);
        bool isValid = verifyProgram(*program, IRForm::AsRead, build);
        AOT_TEST_CHECK(isValid != build.diagnostics().hasErrors());
        if (isValid) {
            StringPrintStream text;
            printProgram(text, *program);
        }
    }
    return finishRead(build, program.get());
}

inline ReadResult readBytesInForm(std::span<const uint8_t> bytes, IRForm form)
{
    BuildContext build(createOptions());
    Program program(BuiltinTable::singleton());
    program.borrowBuffer(bytes);
    bool didRead = Reader(program, form, build).run();
    AOT_TEST_CHECK(didRead != build.diagnostics().hasErrors());
    return finishRead(build, didRead ? &program : nullptr);
}

inline void expectErrors(ASCIILiteral name, const ReadResult& result, bool didRead, std::span<const Kind> expected)
{
    bool isAsExpected = result.didRead == didRead && std::ranges::equal(result.errors, expected, { }, &InternalError::kind);
    if (!isAsExpected) {
        dataLogLn(name, ": ", result.didRead ? "was read" : "was not read");
        for (const InternalError& error : result.errors)
            dataLogLn("    reported ", nameOf(error.kind));
        for (Kind kind : expected)
            dataLogLn("    expected ", nameOf(kind));
    }
    AOT_TEST_CHECK(isAsExpected);
}

inline void expectRejected(ASCIILiteral name, const ReadResult& result, Kind expected)
{
    expectErrors(name, result, false, singleElementSpan(expected));
}

inline void expectAccepted(ASCIILiteral name, const ReadResult& result)
{
    expectErrors(name, result, true, { });
}

inline Vector<uint8_t> damagedCopy(const ReaderFixture& fixture, size_t offset, uint32_t word)
{
    Vector<uint8_t> copy = fixture.bytes;
    setWordAt(copy, offset, word);
    return copy;
}

inline void expectEach(const ReaderFixture& fixture, const Vector<Damage>& damages)
{
    for (const Damage& damage : damages)
        expectRejected(damage.name, readBytes(damagedCopy(fixture, damage.offset, damage.word).span()), damage.expected);
}

inline void expectEachRejectedByVerifier(const ReaderFixture& fixture, const Vector<Damage>& damages)
{
    for (const Damage& damage : damages)
        expectErrors(damage.name, readBytes(damagedCopy(fixture, damage.offset, damage.word).span()), true, singleElementSpan(damage.expected));
}

inline void expectAfter(const ReaderFixture& fixture, ASCIILiteral name, Kind expected, NOESCAPE const WTF::Function<void(Vector<uint8_t>&)>& change)
{
    Vector<uint8_t> copy = fixture.bytes;
    change(copy);
    expectRejected(name, readBytes(copy.span()), expected);
}

inline void expectOrderChecked(const ReaderFixture& fixture, ASCIILiteral name, size_t firstOffset, size_t recordSize)
{
    expectAfter(fixture, name, Kind::UnsortedList, [&](Vector<uint8_t>& bytes) {
        std::span<uint8_t> records = bytes.mutableSpan().subspan(firstOffset, 2 * recordSize);
        std::swap_ranges(records.begin(), records.begin() + recordSize, records.begin() + recordSize);
    });
    expectAfter(fixture, name, Kind::DuplicateListEntry, [&](Vector<uint8_t>& bytes) {
        std::span<uint8_t> records = bytes.mutableSpan().subspan(firstOffset, 2 * recordSize);
        std::copy_n(records.begin(), recordSize, records.begin() + recordSize);
    });
}

inline std::unique_ptr<Program> readChanged(const ReaderFixture& fixture, BuildContext& build, Vector<uint8_t>& storage, size_t offset, uint32_t word)
{
    storage = damagedCopy(fixture, offset, word);
    auto program = Program::read(storage.span(), build);
    AOT_TEST_CHECK(program);
    return program;
}

inline String printedType(const Program& program, TypeID type)
{
    StringPrintStream out;
    printType(out, program, type);
    return out.toString();
}

inline size_t wordOffset(const ReaderFixture& fixture, uint32_t index) { return fixture.offsetOf<uint32_t>(List::Words, index); }
inline size_t stringOffset(const ReaderFixture& fixture, uint32_t index, size_t member) { return fixture.offsetOf<Wire::String>(List::Strings, index, member); }
inline size_t keyOffset(const ReaderFixture& fixture, uint32_t index, size_t member) { return fixture.offsetOf<Wire::PropertyKey>(List::PropertyKeys, index, member); }
inline size_t typeOffset(const ReaderFixture& fixture, uint32_t wireType, size_t member) { return fixture.offsetOf<Wire::Type>(List::Types, wireType, member); }
inline size_t layoutOffset(const ReaderFixture& fixture, LayoutID layout, size_t member) { return fixture.offsetOf<Wire::Layout>(List::Layouts, layout.index(), member); }
inline size_t variableOffset(const ReaderFixture& fixture, uint32_t variable, size_t member) { return fixture.offsetOf<Wire::Variable>(List::Variables, variable, member); }
inline size_t moduleOffset(const ReaderFixture& fixture, ModuleID module, size_t member) { return fixture.offsetOf<Wire::Module>(List::Modules, module.index(), member); }
inline size_t functionOffset(const ReaderFixture& fixture, FunctionID function, size_t member) { return fixture.offsetOf<Wire::Function>(List::Functions, function.index(), member); }

inline size_t fieldOffset(const ReaderFixture& fixture, LayoutID layout, uint32_t field, size_t member)
{
    Wire::Span fields = fixture.spanAt(layoutOffset(fixture, layout, offsetof(Wire::Layout, fields)));
    AOT_TEST_CHECK(field < fields.count);
    return fixture.offsetOf<Wire::Field>(List::Fields, fields.start + field, member);
}

inline uint32_t numberOfLayoutIDs(const ReaderFixture& fixture)
{
    constexpr uint32_t numberOfNamespaceLayouts = 1;
    return fixture.countOf(List::Layouts) + BuiltinTable::singleton().layoutCount() + numberOfNamespaceLayouts;
}

inline uint32_t wireStringOf(const ReaderFixture& fixture, ASCIILiteral text)
{
    for (uint32_t index = 0; index < fixture.countOf(List::Strings); ++index) {
        uint32_t start = fixture.word(stringOffset(fixture, index, offsetof(Wire::String, offset)));
        uint32_t length = fixture.word(stringOffset(fixture, index, offsetof(Wire::String, length)));
        if (length && equalSpans(fixture.bytes.subspan(fixture.offsetOf<uint8_t>(List::Bytes, start), length), text.span8()))
            return index;
    }
    AOT_TEST_CHECK(false);
    return 0;
}

struct WireTypes {
    explicit WireTypes(const ReaderFixture& fixture)
        : number(fixture.word(fieldOffset(fixture, L2, 0, offsetof(Wire::Field, declared.type))))
        , int32(fixture.word(fieldOffset(fixture, L0, 0, offsetof(Wire::Field, declared.type))))
        , literals(fixture.word(variableOffset(fixture, 0, offsetof(Wire::Variable, declared.type))))
        , objects(fixture.word(variableOffset(fixture, 1, offsetof(Wire::Variable, declared.type))))
        , builtins(fixture.word(variableOffset(fixture, 3, offsetof(Wire::Variable, declared.type))))
    {
        Wire::Span captures = fixture.spanAt(functionOffset(fixture, f1, offsetof(Wire::Function, captures)));
        environment = fixture.word(fixture.offsetOf<Wire::DeclaredType>(List::DeclaredTypes, captures.start, offsetof(Wire::DeclaredType, type)));
    }

    uint32_t number;
    uint32_t int32;
    uint32_t literals;
    uint32_t objects;
    uint32_t builtins;
    uint32_t environment;
};

class RecordDamages {
public:
    explicit RecordDamages(const ReaderFixture& fixture)
        : m_fixture(fixture)
    {
    }

    const Vector<Damage>& damages() const { return m_damages; }

    void id(ASCIILiteral name, size_t offset, uint32_t numberOfIDs)
    {
        optionalID(name, offset, numberOfIDs);
        m_damages.append({ name, offset, Wire::none, Kind::IdOutOfRange });
    }

    void id(ASCIILiteral name, size_t offset, List list) { id(name, offset, m_fixture.countOf(list)); }
    void optionalID(ASCIILiteral name, size_t offset, uint32_t numberOfIDs) { m_damages.append({ name, offset, numberOfIDs, Kind::IdOutOfRange }); }
    void optionalID(ASCIILiteral name, size_t offset, List list) { optionalID(name, offset, m_fixture.countOf(list)); }
    void external(ASCIILiteral name, size_t offset) { m_damages.append({ name, offset, invalidExternal, Kind::IdOutOfRange }); }
    template<typename E> void enumerator(ASCIILiteral name, size_t offset) { m_damages.append({ name, offset, numberOfEnumerators<E>, Kind::UnknownEnumerator }); }
    void boolean(ASCIILiteral name, size_t offset) { m_damages.append({ name, offset, 2, Kind::UnknownEnumerator }); }
    void flags(ASCIILiteral name, size_t offset) { m_damages.append({ name, offset, highBit, Kind::UnknownFlag }); }
    void variant(ASCIILiteral name, size_t offset) { m_damages.append({ name, offset, 99, Kind::UnknownVariantCase }); }
    void span(ASCIILiteral name, size_t offset) { m_damages.append({ name, offset + offsetof(Wire::Span, count), hugeCount, Kind::ListOutOfBounds }); }
    void word(ASCIILiteral name, size_t offset, uint32_t damagedWord, Kind expected) { m_damages.append({ name, offset, damagedWord, expected }); }

private:
    const ReaderFixture& m_fixture;
    Vector<Damage> m_damages;
};

template<typename Record>
auto offsetsOf(const ReaderFixture& fixture, List list, uint32_t index)
{
    return [&fixture, list, index](size_t member) {
        return fixture.offsetOf<Record>(list, index, member);
    };
}

inline constexpr uint32_t flagsOfExpectedInt32Promise = static_cast<uint32_t>(PositionFlags::IsExpected) | static_cast<uint32_t>(PositionFlags::IsInt32Promise);

inline void addDeclaredTypeDamages(RecordDamages& damages, size_t offset)
{
    damages.id("DeclaredType.type"_s, offset + offsetof(Wire::DeclaredType, type), List::Types);
    damages.optionalID("DeclaredType.site"_s, offset + offsetof(Wire::DeclaredType, site), List::TypeSites);
    damages.flags("DeclaredType.flags"_s, offset + offsetof(Wire::DeclaredType, flags));
    damages.word("expected position that promises an int32"_s, offset + offsetof(Wire::DeclaredType, flags), flagsOfExpectedInt32Promise, Kind::InvalidDeclaredType);
}

} } } // namespace JSC::AOT::ReaderTests

#endif // ENABLE(AOT)
