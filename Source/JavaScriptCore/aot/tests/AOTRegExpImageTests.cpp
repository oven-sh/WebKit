/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT) && ENABLE(YARR_JIT)

#include "AOTCodeTemplates.h"
#include "AOTImageTestSupport.h"
#include "AOTRegExpCompiler.h"
#include "AOTRegExpTestSupport.h"
#include "AOTVector.h"
#include "JSLock.h"
#include "VM.h"
#include "YarrInterpreter.h"
#include "YarrJIT.h"
#include "YarrMatchingContextHolder.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <wtf/BumpPointerAllocator.h>
#include <wtf/DataLog.h>
#include <wtf/HexNumber.h>
#include <wtf/MathExtras.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

using namespace RegExpTests;

namespace {

using Kind = RegExpSectionItem::Kind;

struct LaidOutRegExps {
    Vector<Source> sources;
    Vector<CompiledRegExp> regExps;
    RegExpSection section;
};

class Matcher {
    WTF_MAKE_NONCOPYABLE(Matcher);
public:
    Matcher()
        : m_vm(VM::create())
    {
        m_lock.emplace(*m_vm);
    }

    ~Matcher()
    {
        m_vm = nullptr;
        m_lock.reset();
    }

    MatchResult match(Yarr::YarrCodeBlock&, std::span<int> output, StringView subject);
    Vector<int> execute(Yarr::YarrCodeBlock&, unsigned outputSize, StringView subject);
    void checkAnswers(const LaidOutRegExps& laidOut, const ImageTests::ExecutableCopy& code, std::span<const String> subjects) { checkAnswers(laidOut, code.start(), subjects); }
    void checkAnswers(const LaidOutRegExps&, const uint8_t* code, std::span<const String> subjects);

private:
    RefPtr<VM> m_vm;
    std::optional<JSLockHolder> m_lock;
};

} // anonymous namespace

static constexpr unsigned numberOfLatin1Classes = 20;
static constexpr size_t byteTableSize = Yarr::CharacterClass::latin1TableSize;
static constexpr size_t maxPadding = 16;

static String wideString(std::u16string_view characters)
{
    return String(std::span<const char16_t> { characters });
}

static String narrowString(std::u16string_view characters)
{
    return String::make8Bit(std::span<const char16_t> { characters });
}

static unsigned countItems(const RegExpSection& section, Kind kind)
{
    unsigned count = 0;
    for (const RegExpSectionItem& item : section.items)
        count += item.kind == kind;
    return count;
}

static Vector<uint32_t> byteTableOffsets(const RegExpSection& section)
{
    Vector<uint32_t> offsets;
    for (const RegExpSectionItem& item : section.items) {
        if (item.kind == Kind::Table && item.size == byteTableSize)
            offsets.append(item.offset);
    }
    return offsets;
}

template<typename Alternative>
static unsigned countSymbols(const Vector<CompiledRegExp>& regExps)
{
    unsigned count = 0;
    for (const CompiledRegExp& regExp : regExps) {
        for (auto& code : regExp.code) {
            for (const RegExpSymbol& symbol : code->symbols)
                count += WTF::holdsAlternative<Alternative>(symbol);
        }
    }
    return count;
}

static bool hasItemAt(const RegExpSection& section, Kind kind, uint32_t offset)
{
    return section.items.containsIf([&](const RegExpSectionItem& item) {
        return item.kind == kind && item.offset == offset;
    });
}

static bool isPadding(const RegExpSection& section, size_t begin, size_t end)
{
    static const NeverDestroyed<Vector<uint8_t>> breakpoint = CodeTemplates::create(policyFor(nullptr), nullptr)->breakpoint;
    for (size_t offset = begin; offset < end; ++offset) {
        if (section.bytes[offset] != (offset < section.dataOffset ? breakpoint.get()[offset % breakpoint->size()] : 0))
            return false;
    }
    return true;
}

static void checkSection(const RegExpSection& section, std::span<const CompiledRegExp> regExps)
{
    AOT_TEST_CHECK(!(section.dataOffset % RegExpSection::alignment) && section.dataOffset <= section.bytes.size());
    size_t end = 0;
    for (const RegExpSectionItem& item : section.items) {
        AOT_TEST_CHECK(item.regExp < regExps.size());
        if (item.kind == Kind::DuplicateBody) {
            AOT_TEST_CHECK(hasItemAt(section, Kind::Body, item.offset));
            continue;
        }
        AOT_TEST_CHECK(item.kind == Kind::Table || item.offset + static_cast<size_t>(item.size) <= section.dataOffset);
        AOT_TEST_CHECK(item.offset >= end && item.offset - end < (item.offset == section.dataOffset ? RegExpSection::alignment : maxPadding));
        AOT_TEST_CHECK(item.offset + static_cast<size_t>(item.size) <= section.bytes.size());
        AOT_TEST_CHECK(isPadding(section, end, item.offset));
        end = item.offset + static_cast<size_t>(item.size);
    }
    AOT_TEST_CHECK(end == section.bytes.size() || (section.dataOffset == section.bytes.size() && section.dataOffset - end < RegExpSection::alignment));
    AOT_TEST_CHECK(isPadding(section, end, section.bytes.size()));

    AOT_TEST_CHECK(section.entries.size() == regExps.size());
    for (size_t index = 0; index < regExps.size(); ++index) {
        for (uint32_t entry : section.entries[index])
            AOT_TEST_CHECK(hasItemAt(section, regExps[index].form == Form::Code ? Kind::Body : Kind::Routine, entry));
    }
}

static bool isSameSection(const RegExpSection& a, const RegExpSection& b)
{
    return a.bytes == b.bytes && a.entries == b.entries && std::ranges::equal(a.items, b.items, [](const RegExpSectionItem& left, const RegExpSectionItem& right) {
        return left.kind == right.kind && left.offset == right.offset && left.size == right.size && left.regExp == right.regExp;
    });
}

static RegExpSection layOutSection(std::span<const CompiledRegExp> regExps)
{
    AssemblerPolicy policy = policyFor(nullptr);
    auto templates = CodeTemplates::create(policy, nullptr);
    AOT_TEST_CHECK(templates.has_value());
    std::optional<RegExpSection> section = layOutRegExpSection(regExps, policy, *templates, nullptr);
    AOT_TEST_CHECK(section.has_value());
    checkSection(*section, regExps);
    return WTF::move(*section);
}

static LaidOutRegExps layOut(Vector<Source>&& sources)
{
    LaidOutRegExps result;
    result.sources = WTF::move(sources);
    for (const Source& source : result.sources)
        result.regExps.append(compileToCode(source));
    result.section = layOutSection(result.regExps.span());
    return result;
}

static ImageTests::ExecutableCopy load(const RegExpSection& section)
{
    ImageTests::ExecutableCopy code(section.bytes.span());
    AOT_TEST_CHECK(!(std::bit_cast<uintptr_t>(code.start()) % RegExpSection::alignment));
    return code;
}

class Interpreter {
public:
    explicit Interpreter(const Source& source)
        : m_pattern(source.pattern, source.flags, m_error, Yarr::ExecutionMode::IncludeSubpatterns, Yarr::PatternAnalysis::Performed)
    {
        AOT_TEST_CHECK(!Yarr::hasError(m_error));
        m_bytecode = Yarr::byteCompile(m_pattern, &m_allocator, m_error);
        AOT_TEST_CHECK(m_bytecode && !Yarr::hasError(m_error));
    }

    Vector<int> match(StringView subject)
    {
        Vector<unsigned> output(FillWith { }, m_pattern.offsetsSize(), Yarr::offsetNoMatch);
        if (Yarr::interpret(m_bytecode.get(), subject, 0, output.mutableSpan().data()) == Yarr::offsetNoMatch)
            return { };
        return output.map([](unsigned offset) {
            return static_cast<int>(offset);
        });
    }

private:
    Yarr::ErrorCode m_error { Yarr::ErrorCode::NoError };
    Yarr::YarrPattern m_pattern;
    BumpPointerAllocator m_allocator;
    std::unique_ptr<Yarr::BytecodePattern> m_bytecode;
};

MatchResult Matcher::match(Yarr::YarrCodeBlock& codeBlock, std::span<int> output, StringView subject)
{
    Yarr::MatchingContextHolder context(*m_vm, nullptr, Yarr::MatchFrom::CompilerThread);
    return subject.is8Bit() ? codeBlock.execute(subject.span8(), 0, output.data(), &context) : codeBlock.execute(subject.span16(), 0, output.data(), &context);
}

Vector<int> Matcher::execute(Yarr::YarrCodeBlock& codeBlock, unsigned outputSize, StringView subject)
{
    Vector<int> output(FillWith { }, outputSize, 0);
    MatchResult result = match(codeBlock, output.mutableSpan(), subject);
    if (!result)
        return { };
    AOT_TEST_CHECK(static_cast<int>(result.start) == output[0] && static_cast<int>(result.end) == output[1]);
    return output;
}

void Matcher::checkAnswers(const LaidOutRegExps& laidOut, const uint8_t* code, std::span<const String> subjects)
{
    for (size_t index = 0; index < laidOut.sources.size(); ++index) {
        const Source& source = laidOut.sources[index];
        unsigned outputSize = (summarizeRegExp(source.pattern, source.flags).numSubpatterns + 1) * 2;
        auto [offsetFor8Bit, offsetFor16Bit] = laidOut.section.entries[index];
        Yarr::YarrCodeBlock codeBlock(nullptr);
        codeBlock.setCodeFromImage(code + offsetFor8Bit, code + offsetFor16Bit);
        Interpreter interpreter(source);
        for (const String& subject : subjects) {
            bool isSame = execute(codeBlock, outputSize, subject) == interpreter.match(subject);
            if (!isSame)
                dataLogLn("/", source.pattern, "/ and the interpreter disagree on \"", subject, "\"");
            AOT_TEST_CHECK(isSame);
        }
    }
}

static std::unique_ptr<RegExpCode> makeCode(std::span<const RegExpSymbol> symbols, PCRelativeReach reach, size_t fillerSize)
{
    AssemblerPolicy policy = policyFor(nullptr);
    CCallHelpers jit;
    RelocationRecorder recorder(jit, policy);
    for (uint32_t index = 0; index < symbols.size(); ++index)
        recorder.moveAddress({ SymbolSpace::RegExp, index }, GPRInfo::regT0, reach);
    jit.ret();
    auto body = recorder.finalize(LinkBuffer::Profile::YarrJIT, nullptr);
    AOT_TEST_CHECK(body.has_value());

    auto code = makeUnique<RegExpCode>();
    code->body = WTF::move(*body);
    code->body.bytes.insertFill(code->body.bytes.size(), 0, fillerSize);
    code->symbols = Vector<RegExpSymbol>(symbols);
    code->reach = reach;
    return code;
}

static CompiledRegExp makeRegExp(std::span<const RegExpSymbol> symbols, PCRelativeReach reach, size_t fillerSize = 0)
{
    CompiledRegExp regExp;
    regExp.form = Form::Code;
    for (auto& code : regExp.code)
        code = makeCode(symbols, reach, fillerSize);
    return regExp;
}

static Latin1BitPlane makePlane(uint8_t bit, std::initializer_list<uint8_t> members)
{
    Latin1BitPlane plane;
    plane.bit = bit;
    for (uint8_t member : members)
        plane.members.set(member);
    return plane;
}

static Yarr::ClassTable makeClassTable(uint32_t seed, size_t numberOfBlocks)
{
    Yarr::ClassTable table;
    for (size_t index = 0; index < numberOfBlocks; ++index) {
        Yarr::ClassTable::Block block { };
        block[0] = seed;
        block[1] = static_cast<uint32_t>(index);
        table.pageBlockIndexes.append(static_cast<uint16_t>(index));
        table.blocks.append(block);
    }
    return table;
}

static String makeLatin1Class(unsigned seed)
{
    StringBuilder builder;
    builder.append('[');
    for (unsigned member = 0; member < 12; ++member)
        builder.append("\\x"_s, hex(0x21 + (seed * 5 + member * 17) % 0xde, 2, Lowercase));
    builder.append(']');
    return builder.toString();
}

static Vector<Source> latin1ClassSources()
{
    Vector<Source> sources;
    for (unsigned seed = 0; seed < numberOfLatin1Classes; ++seed)
        sources.append({ makeString(makeLatin1Class(seed), '+'), { } });
    return sources;
}

static Vector<Source> classTableSources()
{
    return {
        { "\\p{L}+"_s, { Yarr::Flags::Unicode } },
        { "\\p{Lu}\\p{Ll}*"_s, { Yarr::Flags::Unicode } },
        { "[\\u0400-\\u04ff\\u0530-\\u058f\\u05d0-\\u05ea\\u0620-\\u064a\\u0905-\\u0939\\u0e01-\\u0e30\\u3041-\\u3096\\u30a1-\\u30fa]"_s, { } },
    };
}

static Vector<Source> backreferenceSources()
{
    return {
        { "(\\u00e9)\\1"_s, { Yarr::Flags::IgnoreCase } },
        { "(\\u00e8)\\1"_s, { Yarr::Flags::IgnoreCase } },
    };
}

static Vector<Source> mixedSources()
{
    return {
        { "ab+c"_s, { } },
        { "(\\w+)\\s(\\w+)"_s, { } },
        { makeString(makeLatin1Class(0), '+'), { } },
        { "\\p{L}+"_s, { Yarr::Flags::Unicode } },
        { "(\\u00e9)\\1"_s, { Yarr::Flags::IgnoreCase } },
    };
}

static Vector<Source> allSources()
{
    Vector<Source> sources = mixedSources();
    sources.appendVector(latin1ClassSources());
    sources.appendVector(classTableSources());
    sources.appendVector(backreferenceSources());
    return sources;
}

static Vector<String> latin1Subjects()
{
    Vector<Latin1Character> ascending;
    for (unsigned character = 1; character < byteTableSize; ++character)
        ascending.append(static_cast<uint8_t>(character));
    Vector<Latin1Character> descending = ascending;
    descending.reverse();
    String wide { ascending.span() };
    wide.convertTo16Bit();
    return { emptyString(), String { ascending.span() }, String { descending.span() }, wide };
}

static Vector<String> wideSubjects()
{
    return {
        wideString(u"12 \u0416\u0438\u0437\u043d\u044c 34"),
        wideString(u"\u3042\u30a2"),
        wideString(u"abc DEF"),
        wideString(u"1\U00010400\U00010428x"),
        wideString(u"\xd801"),
        wideString(u"1234 \u0500"),
        wideString(u"\u0531\u0561"),
        wideString(u"12 34"),
        "abc DEF"_s,
        emptyString(),
    };
}

static Vector<String> mixedSubjects()
{
    Vector<String> subjects = wideSubjects();
    subjects.appendVector(latin1Subjects());
    subjects.appendList({
        String { "xabbbc abc"_s },
        wideString(u"xabbbc abc"),
        narrowString(u"\u00e9\u00c9"),
        wideString(u"x\u00c9\u00e9"),
        wideString(u"\u00e9\u00e8"),
    });
    return subjects;
}

static void checkEveryShapeOfRun(unsigned length, OptionSet<Yarr::Flags> flags)
{
    constexpr unsigned classAtFifthPlace = 1u << 3;
    Vector<Source> sources;
    for (unsigned shape = 0; shape < (1u << (length - 1)); ++shape) {
        if (!(shape & classAtFifthPlace))
            continue;
        StringBuilder pattern;
        pattern.append('a');
        for (unsigned place = 1; place < length; ++place) {
            if ((shape >> (place - 1)) & 1)
                pattern.append("[a-z]"_s);
            else
                pattern.append(static_cast<char>('a' + place));
        }
        sources.append({ pattern.toString(), flags });
    }

    Vector<Latin1Character> characters;
    for (unsigned place = 0; place < length; ++place)
        characters.append(static_cast<Latin1Character>('a' + place));
    auto makeSubject = [&](bool is16Bit) {
        String subject { characters.span() };
        if (is16Bit)
            subject.convertTo16Bit();
        return subject;
    };
    Vector<String> subjects { makeSubject(false), makeSubject(true) };
    for (unsigned place = 0; place < length; ++place) {
        Latin1Character kept = characters[place];
        for (Latin1Character other : { '~', '\0' }) {
            characters[place] = other;
            subjects.append(makeSubject(true));
        }
        characters[place] = kept;
    }

    LaidOutRegExps laidOut = layOut(WTF::move(sources));
    Matcher matcher;
    matcher.checkAnswers(laidOut, load(laidOut.section), subjects.span());
}

static void testEveryShapeOfRunMatchesWideSubjects()
{
    checkEveryShapeOfRun(12, { });
    checkEveryShapeOfRun(12, { Yarr::Flags::IgnoreCase });
    checkEveryShapeOfRun(12, { Yarr::Flags::Unicode });
    checkEveryShapeOfRun(13, { });
}

static void testEqualBodiesShareCode()
{
    LaidOutRegExps same = layOut({ { "ab+c"_s, { } }, { "ab+c"_s, { } } });
    AOT_TEST_CHECK(same.section.entries[0] == same.section.entries[1]);
    AOT_TEST_CHECK(same.section.entries[0][0] != same.section.entries[0][1]);
    AOT_TEST_CHECK(countItems(same.section, Kind::Body) == 2);
    AOT_TEST_CHECK(countItems(same.section, Kind::DuplicateBody) == 2);

    LaidOutRegExps different = layOut({ { "ab+c"_s, { } }, { "ab+d"_s, { } } });
    AOT_TEST_CHECK(different.section.entries[0][0] != different.section.entries[1][0]);
    AOT_TEST_CHECK(different.section.entries[0][1] != different.section.entries[1][1]);
    AOT_TEST_CHECK(countItems(different.section, Kind::Body) == 4);
    AOT_TEST_CHECK(!countItems(different.section, Kind::DuplicateBody));

    Matcher matcher;
    Vector<String> subjects { "xabbbc"_s, "abd"_s, "ab"_s, wideString(u"abbd abc") };
    matcher.checkAnswers(same, load(same.section), subjects.span());
    matcher.checkAnswers(different, load(different.section), subjects.span());
}

static void testLatin1ClassesShareByteTables()
{
    LaidOutRegExps laidOut = layOut(latin1ClassSources());
    AOT_TEST_CHECK(countSymbols<Latin1BitPlane>(laidOut.regExps) >= numberOfLatin1Classes);
    size_t numberOfByteTables = byteTableOffsets(laidOut.section).size();
    AOT_TEST_CHECK(numberOfByteTables >= (numberOfLatin1Classes + Latin1BitPlane::planesPerByteTable - 1) / Latin1BitPlane::planesPerByteTable);
    AOT_TEST_CHECK(numberOfByteTables <= numberOfLatin1Classes);

    Matcher matcher;
    matcher.checkAnswers(laidOut, load(laidOut.section), latin1Subjects().span());
}

static void testEqualLatin1ClassesShareBitPlane()
{
    String characterClass = makeLatin1Class(0);
    Source first { makeString(characterClass, '+'), { } };
    Source second { makeString('z', characterClass), { } };
    AOT_TEST_CHECK(byteTableOffsets(layOut({ first }).section).size() == 1);

    LaidOutRegExps both = layOut({ first, second });
    AOT_TEST_CHECK(byteTableOffsets(both.section).size() == 1);

    Matcher matcher;
    matcher.checkAnswers(both, load(both.section), latin1Subjects().span());
}

static void checkByteTable(const RegExpSection& section, uint32_t offset, std::span<const RegExpSymbol> planes)
{
    for (size_t character = 0; character < byteTableSize; ++character) {
        uint8_t expected = 0;
        for (const RegExpSymbol& symbol : planes) {
            auto& plane = std::get<Latin1BitPlane>(symbol);
            if (plane.members.get(character))
                expected |= 1 << plane.bit;
        }
        AOT_TEST_CHECK(section.bytes[offset + character] == expected);
    }
}

static void testBitPlanesFillByteTables()
{
    constexpr uint8_t planesPerByteTable = Latin1BitPlane::planesPerByteTable;
    Vector<RegExpSymbol> planes;
    for (uint8_t bit = 0; bit < planesPerByteTable; ++bit)
        planes.append(makePlane(bit, { static_cast<uint8_t>('a' + bit), static_cast<uint8_t>(0x80 + bit), 'z' }));
    planes.append(makePlane(3, { '0', 0xff }));
    planes.append(makePlane(5, { '0', '1' }));
    auto firstTablePlanes = planes.span().first(planesPerByteTable);
    auto secondTablePlanes = planes.span().subspan(planesPerByteTable);

    std::array<CompiledRegExp, 1> oneTable { makeRegExp(firstTablePlanes, PCRelativeReach::Near) };
    RegExpSection section = layOutSection(oneTable);
    Vector<uint32_t> offsets = byteTableOffsets(section);
    AOT_TEST_CHECK(offsets.size() == 1 && countItems(section, Kind::Table) == 1);
    checkByteTable(section, offsets[0], firstTablePlanes);

    std::array<CompiledRegExp, 1> twoTables { makeRegExp(planes.span(), PCRelativeReach::Near) };
    section = layOutSection(twoTables);
    offsets = byteTableOffsets(section);
    AOT_TEST_CHECK(offsets.size() == 2 && countItems(section, Kind::Table) == 2);
    checkByteTable(section, offsets[0], firstTablePlanes);
    checkByteTable(section, offsets[1], secondTablePlanes);
}

static void testClassTables()
{
    LaidOutRegExps laidOut = layOut(classTableSources());
    AOT_TEST_CHECK(countSymbols<Yarr::ClassTable>(laidOut.regExps));

    Matcher matcher;
    matcher.checkAnswers(laidOut, load(laidOut.section), wideSubjects().span());
}

static void testSharedRoutineIsPlacedOnce()
{
    Vector<Source> sources = backreferenceSources();
    LaidOutRegExps oneUser = layOut({ sources[0] });
    AOT_TEST_CHECK(countSymbols<Yarr::SharedRoutine>(oneUser.regExps));
    AOT_TEST_CHECK(countSymbols<Yarr::SharedTableID>(oneUser.regExps));
    unsigned numberOfRoutines = countItems(oneUser.section, Kind::Routine);
    AOT_TEST_CHECK(numberOfRoutines);

    LaidOutRegExps twoUsers = layOut(WTF::move(sources));
    AOT_TEST_CHECK(countItems(twoUsers.section, Kind::Routine) == numberOfRoutines);

    Matcher matcher;
    matcher.checkAnswers(twoUsers, load(twoUsers.section), mixedSubjects().span());
}

static void testSectionIsPositionIndependent()
{
    LaidOutRegExps laidOut = layOut(mixedSources());
    ImageTests::ExecutableCopy first = load(laidOut.section);
    ImageTests::ExecutableCopy second = load(laidOut.section);
    AOT_TEST_CHECK(first.start() != second.start());

    Matcher matcher;
    Vector<String> subjects = mixedSubjects();
    matcher.checkAnswers(laidOut, first, subjects.span());
    matcher.checkAnswers(laidOut, second, subjects.span());
}

static void testLayoutIsDeterministic()
{
    LaidOutRegExps laidOut = layOut(allSources());
    AOT_TEST_CHECK(isSameSection(laidOut.section, layOutSection(laidOut.regExps.span())));
    AOT_TEST_CHECK(isSameSection(laidOut.section, layOut(allSources()).section));
}

static void testOnlyCodeIsPlaced()
{
    RegExpSection empty = layOutSection({ });
    AOT_TEST_CHECK(empty.bytes.isEmpty() && empty.entries.isEmpty() && empty.items.isEmpty());

    std::array<CompiledRegExp, 2> withoutCode { compile({ "abc"_s, { } }), CompiledRegExp { } };
    AOT_TEST_CHECK(withoutCode[0].form == Form::Atom && withoutCode[1].form == Form::NoMetadata);
    RegExpSection section = layOutSection(withoutCode);
    AOT_TEST_CHECK(section.entries.size() == 2 && section.entries[0] == section.entries[1]);
    AOT_TEST_CHECK(section.items.size() == 1 && section.items[0].kind == Kind::Routine);

    std::array<CompiledRegExp, 3> mixed { compile({ "abc"_s, { } }), compileToCode({ "[ab][cd]+e"_s, { } }), CompiledRegExp { } };
    section = layOutSection(mixed);
    AOT_TEST_CHECK(countItems(section, Kind::Body) == 2 && countItems(section, Kind::Routine) == 1);
    AOT_TEST_CHECK(section.entries[0] == section.entries[2]);
    for (const RegExpSectionItem& item : section.items)
        AOT_TEST_CHECK(item.regExp == (item.kind == Kind::Routine ? 0u : 1u));
}

static void testEntryWithoutCodeGivesMatchUp()
{
    std::array<CompiledRegExp, 1> atom { compile({ "abc"_s, { } }) };
    AOT_TEST_CHECK(atom[0].form == Form::Atom);
    RegExpSection section = layOutSection(atom);
    ImageTests::ExecutableCopy code = load(section);
    Yarr::YarrCodeBlock codeBlock(nullptr);
    codeBlock.setCodeFromImage(code.start() + section.entries[0][0], code.start() + section.entries[0][1]);

    Matcher matcher;
    std::array<int, 2> output { };
    for (const String& subject : { String { "xabc"_s }, wideString(u"Жabc") })
        AOT_TEST_CHECK(matcher.match(codeBlock, output, subject).start == static_cast<size_t>(Yarr::JSRegExpResult::JITCodeFailure));
}

static void checkUsersFarApartShareUnit(const RegExpSymbol& symbol)
{
    constexpr size_t fillerSize = 1 * MB + 64 * KB;
    Kind kind = WTF::holdsAlternative<Yarr::SharedRoutine>(symbol) ? Kind::Routine : Kind::Table;
    std::array<CompiledRegExp, 3> regExps {
        makeRegExp(singleElementSpan(symbol), PCRelativeReach::Far),
        makeRegExp({ }, PCRelativeReach::Far, fillerSize),
        makeRegExp(singleElementSpan(symbol), PCRelativeReach::Far, 4),
    };
    AOT_TEST_CHECK(countItems(layOutSection(regExps), kind) == 1);
}

static void testUsersFarApartShareUnit()
{
    checkUsersFarApartShareUnit(PlainData { { 1, 2, 3, 4, 5, 6, 7, 8 } });
    checkUsersFarApartShareUnit(Literal64 { 0x0123456789abcdef });
    checkUsersFarApartShareUnit(makePlane(0, { 'a' }));
    checkUsersFarApartShareUnit(Yarr::SharedRoutine { Yarr::SharedRoutine::MatchSet { GPRInfo::regT0, GPRInfo::regT1, { 'a' }, { } } });
    checkUsersFarApartShareUnit(makeClassTable(1, 4));
}

static void testRoutineCallAcrossLargeBody()
{
    constexpr size_t beyondCallRange = 130 * MB;
    LaidOutRegExps laidOut;
    laidOut.sources = { backreferenceSources()[0] };
    laidOut.regExps.append(compileToCode(laidOut.sources[0]));
    AOT_TEST_CHECK(laidOut.regExps[0].code[1]->symbols.containsIf([](const RegExpSymbol& symbol) {
        return WTF::holdsAlternative<Yarr::SharedRoutine>(symbol);
    }));
    CompiledRegExp large;
    large.form = Form::Code;
    large.code[0] = makeCode({ }, PCRelativeReach::Far, beyondCallRange);
    large.code[1] = makeCode({ }, PCRelativeReach::Far, sizeof(uint32_t));
    laidOut.regExps.append(WTF::move(large));

    AssemblerPolicy policy = policyFor(nullptr);
    auto templates = CodeTemplates::create(policy, nullptr);
    AOT_TEST_CHECK(templates.has_value());
    std::optional<RegExpSection> section = layOutRegExpSection(laidOut.regExps.span(), policy, *templates, nullptr);
    AOT_TEST_CHECK(section.has_value());
    laidOut.section = WTF::move(*section);
    AOT_TEST_CHECK(laidOut.section.entries[0][1] > laidOut.section.entries[0][0] + beyondCallRange);

#if OS(LINUX)
    size_t size = WTF::roundUpToMultipleOf(pageSize(), laidOut.section.bytes.size());
    auto* code = static_cast<uint8_t*>(OSAllocator::reserveAndCommit(size, OSAllocator::UnknownUsage, nullptr, true, true));
    memcpySpan(std::span<uint8_t> { code, size }, laidOut.section.bytes.span());
    MacroAssembler::cacheFlush(code, size);
    Matcher matcher;
    matcher.checkAnswers(laidOut, code, mixedSubjects().span());
    OSAllocator::decommitAndRelease(code, size);
#endif
}

static void testLongSubjectAllowsMatchLimitTurns()
{
    Source source { "^(x+)+y"_s, { } };
    LaidOutRegExps laidOut = layOut({ source });
    ImageTests::ExecutableCopy code = load(laidOut.section);
    Yarr::YarrCodeBlock codeBlock(nullptr);
    codeBlock.setCodeFromImage(code.start() + laidOut.section.entries[0][0], code.start() + laidOut.section.entries[0][1]);

    constexpr size_t greatestLength = 954429895;
    size_t reservedSize = WTF::roundUpToMultipleOf(pageSize(), greatestLength);
    void* reserved = OSAllocator::tryReserveUncommitted(reservedSize);
    AOT_TEST_CHECK(reserved);
    OSAllocator::commit(reserved, pageSize(), true, false);
    auto* characters = static_cast<Latin1Character*>(reserved);

    Matcher matcher;
    std::array<int, 4> output { };
    auto answerAt = [&](unsigned numberOfX, size_t length) {
        memset(characters, 'x', numberOfX);
        characters[numberOfX] = 'c';
        return matcher.match(codeBlock, output, StringView(std::span<const Latin1Character> { characters, length })).start;
    };
    constexpr size_t noMatch = WTF::notFound;
    constexpr size_t givenToCaller = static_cast<size_t>(Yarr::JSRegExpResult::JITCodeFailure);
    AOT_TEST_CHECK(answerAt(15, 40) == noMatch);
    AOT_TEST_CHECK(answerAt(17, 40) == givenToCaller);
    for (size_t length : { size_t { 477211306 }, size_t { 477211307 }, greatestLength }) {
        AOT_TEST_CHECK(answerAt(26, length) == noMatch);
        AOT_TEST_CHECK(answerAt(27, length) == givenToCaller);
    }
    OSAllocator::decommit(reserved, pageSize());
    OSAllocator::releaseDecommitted(reserved, reservedSize, 0);
}

template<typename Emit>
static std::expected<std::unique_ptr<RegExpCode>, Yarr::JITFailureReason> emitForImage(const Emit& emit)
{
    AssemblerPolicy policy = policyFor(nullptr);
    CCallHelpers jit;
    ImageCodeAddressing addressing(jit, PCRelativeReach::Near, policy, nullptr);
    emit(jit, addressing);
    jit.ret();
    return addressing.takeCode();
}

static void testEqualTablesAreOneSymbol()
{
    Yarr::CharacterClass::ByteTable table;
    table.data['a'] = 1;
    table.data[0xe9] = 1;
    Yarr::CharacterClass::ByteTable equalTable = table;
    Yarr::CharacterClass::ByteTable otherTable = table;
    otherTable.data['b'] = 1;

    auto emitTests = [](std::initializer_list<const Yarr::CharacterClass::ByteTable*> tables) {
        return emitForImage([&](CCallHelpers& jit, ImageCodeAddressing& addressing) {
            CCallHelpers::JumpList isMember;
            for (auto* testedTable : tables)
                isMember.append(addressing.branchTestLatin1Table(CCallHelpers::NonZero, GPRInfo::regT0, *testedTable));
            isMember.link(&jit);
        });
    };

    auto equalTables = emitTests({ &table, &equalTable });
    AOT_TEST_CHECK(equalTables.has_value());
    AOT_TEST_CHECK((*equalTables)->symbols.size() == 1 && (*equalTables)->body.relocations.size() == 2);

    auto differentTables = emitTests({ &table, &otherTable });
    AOT_TEST_CHECK(differentTables.has_value());
    AOT_TEST_CHECK((*differentTables)->symbols.size() == 2 && (*differentTables)->body.relocations.size() == 2);
}

static void testPointerImmediateDeclines()
{
    auto withPointer = emitForImage([](CCallHelpers& jit, ImageCodeAddressing&) {
        jit.move(CCallHelpers::TrustedImmPtr(&jit), GPRInfo::regT0);
    });
    AOT_TEST_CHECK(!withPointer && withPointer.error() == Yarr::JITFailureReason::ProcessAddressInCode);

    auto withInteger = emitForImage([](CCallHelpers& jit, ImageCodeAddressing&) {
        jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(&jit)), GPRInfo::regT0);
    });
    AOT_TEST_CHECK(withInteger.has_value());
}

std::span<const NativeTest> regExpImageTests()
{
    static constexpr NativeTest tests[] = {
        { "regexp-image/every-shape-of-run-matches-wide-subjects"_s, testEveryShapeOfRunMatchesWideSubjects },
        { "regexp-image/equal-bodies-share-code"_s, testEqualBodiesShareCode },
        { "regexp-image/latin1-classes-share-byte-tables"_s, testLatin1ClassesShareByteTables },
        { "regexp-image/equal-latin1-classes-share-bit-plane"_s, testEqualLatin1ClassesShareBitPlane },
        { "regexp-image/bit-planes-fill-byte-tables"_s, testBitPlanesFillByteTables },
        { "regexp-image/class-tables"_s, testClassTables },
        { "regexp-image/shared-routine-is-placed-once"_s, testSharedRoutineIsPlacedOnce },
        { "regexp-image/section-is-position-independent"_s, testSectionIsPositionIndependent },
        { "regexp-image/layout-is-deterministic"_s, testLayoutIsDeterministic },
        { "regexp-image/only-code-is-placed"_s, testOnlyCodeIsPlaced },
        { "regexp-image/entry-without-code-gives-match-up"_s, testEntryWithoutCodeGivesMatchUp },
        { "regexp-image/users-far-apart-share-unit"_s, testUsersFarApartShareUnit },
        { "regexp-image/routine-is-called-across-large-body"_s, testRoutineCallAcrossLargeBody },
        { "regexp-image/long-subject-allows-match-limit-turns"_s, testLongSubjectAllowsMatchLimitTurns },
        { "regexp-image/equal-tables-are-one-symbol"_s, testEqualTablesAreOneSymbol },
        { "regexp-image/pointer-immediate-declines"_s, testPointerImmediateDeclines },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT) && ENABLE(YARR_JIT)
