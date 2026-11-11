/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT) && ENABLE(YARR_JIT)

#include "AOTIRProgram.h"
#include "AOTImageTestSupport.h"
#include "AOTImageText.h"
#include "AOTLimits.h"
#include "AOTNumbering.h"
#include "AOTRegExpCompiler.h"
#include "AOTRegExpRecords.h"
#include "AOTRegExpSummary.h"
#include "AOTStubCatalog.h"
#include "AOTTestHarness.h"
#include "AOTTextEncoder.h"
#include "AOTVector.h"
#include "JSLock.h"
#include "RegExp.h"
#include "RegExpInlines.h"
#include "VM.h"
#include "YarrPattern.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT::ImageTests {

using Form = RegExpRecordForm;
using RegExpFlags = OptionSet<Yarr::Flags>;

namespace {

struct Pattern {
    String text;
    RegExpFlags flags { };
    bool fitsCodeLimits { true };
    bool isEvaluated { false };
};

struct RequestWithForm {
    RegExpRequest request;
    Form form;
};

struct TextOfRegExpImage {
    Vector<ImageStringWord> strings;
    Vector<uint32_t> listedStrings;
    Vector<uint8_t> characters;
};

enum class CodeOfRegExpImage : bool { Sketched, Compiled };

class RegExpImage {
    WTF_MAKE_NONCOPYABLE(RegExpImage);
public:
    RegExpImage(std::span<const Pattern> literals, const TestOptions& = { }, std::span<const Pattern> predictedTexts = { }, CodeOfRegExpImage = CodeOfRegExpImage::Sketched);
    RegExpImage(std::initializer_list<Pattern> literals, const TestOptions& = { }, std::initializer_list<Pattern> predictedTexts = { }, CodeOfRegExpImage = CodeOfRegExpImage::Sketched);
    ~RegExpImage();

    VM& vm() { return *m_vm; }
    RegExpRecords records() const { return RegExpRecords(*image); }
    std::span<ImageRegExpHash> hashes();
    size_t indexOfRequest(const String& text, RegExpFlags) const;
    const RegExpSummary& summaryOf(size_t request);

    BuildContext context;
    std::unique_ptr<Typed::Program> program;
    RegExpRequests requests;
    CompiledRegExps compiled;
    RegExpSection section;
    StringNumbers strings;
    RegExpSections sections;
    AlignedBytes bytes;
    std::unique_ptr<Image> image;

private:
    RequestWithForm makeRequest(const Pattern&, RegExpRequestOrigin);
    Vector<RegExpRequest> makeRequests(std::span<const Pattern>, RegExpRequestOrigin originUnlessEvaluated);
    void compileAndLayOut();
    TextOfRegExpImage encodeNamedText();

    RefPtr<VM> m_vm;
    std::optional<JSLockHolder> m_lock;
    std::optional<ExecutableCopy> m_executableCode;
};

} // anonymous namespace

static AlignedBytes imageWithSections(std::initializer_list<std::pair<ImageSection, std::span<const uint8_t>>> contents, size_t codeSize)
{
    ImageHeader header { };
    header.magic = imageMagic;
    size_t offset = roundUpToMultipleOf<imageSectionAlignment>(sizeof(ImageHeader));
    for (unsigned index = 0; index < numberOfImageSections; ++index) {
        auto content = std::ranges::find(contents, static_cast<ImageSection>(index), [](auto& entry) { return entry.first; });
        size_t size = content == contents.end() ? 0 : content->second.size();
        header.sections[index] = { static_cast<uint32_t>(offset), static_cast<uint32_t>(size) };
        if (static_cast<ImageSection>(index) == ImageSection::Strings)
            header.numbers.numberOfStringWords = static_cast<uint32_t>(size / sizeof(ImageStringWord));
        offset = roundUpToMultipleOf<imageSectionAlignment>(offset + size);
    }
    header.codeOffset = roundUpToMultipleOf<imagePageSize>(offset);
    header.codeSize = codeSize;
    header.code.constantsOffset = static_cast<uint32_t>(codeSize);
    header.size = roundUpToMultipleOf<imagePageSize>(header.codeOffset + codeSize);

    Vector<uint8_t> bytes(FillWith { }, header.size, 0);
    memcpy(bytes.mutableSpan().data(), &header, sizeof(header));
    for (auto& [section, content] : contents) {
        if (!content.empty())
            memcpy(bytes.mutableSpan().data() + header.sections[static_cast<unsigned>(section)].offset, content.data(), content.size());
    }
    return AlignedBytes(bytes.span());
}

static Form formFor(const RegExpSummary& summary, bool fitsCodeLimits)
{
    if (summary.metadata == RegExpMetadata::Unknown)
        return Form::NoMetadata;
    if (!summary.atom.isNull())
        return Form::Atom;
    return fitsCodeLimits ? Form::Code : Form::NoCode;
}

RequestWithForm RegExpImage::makeRequest(const Pattern& pattern, RegExpRequestOrigin origin)
{
    Typed::StringTable& strings = program->strings;
    RegExpRequest request {
        .pattern = strings.intern(StringView(pattern.text)),
        .flags = pattern.flags,
        .origin = origin,
    };
    const RegExpSummary& summary = context.regExpSummaries().summaryOf(request.pattern, request.flags, pattern.text);
    AOT_TEST_CHECK(summary.isValid());
    if (!summary.atom.isNull())
        request.atom = strings.intern(StringView(summary.atom));
    for (const String& name : summary.captureGroupNames)
        request.captureGroupNames.append(name.isNull() ? StringID() : strings.intern(StringView(name)));
    for (const RegExpNamedGroupSummary& group : summary.namedGroups)
        request.namedGroups.append({ strings.intern(StringView(group.name)), group.parenIndices });
    return { WTF::move(request), formFor(summary, pattern.fitsCodeLimits) };
}

Vector<RegExpRequest> RegExpImage::makeRequests(std::span<const Pattern> patterns, RegExpRequestOrigin originUnlessEvaluated)
{
    Vector<RequestWithForm> requestsWithForms;
    for (const Pattern& pattern : patterns) {
        AOT_TEST_CHECK(!pattern.isEvaluated || originUnlessEvaluated == RegExpRequestOrigin::Candidate);
        requestsWithForms.append(makeRequest(pattern, pattern.isEvaluated ? RegExpRequestOrigin::Evaluated : originUnlessEvaluated));
    }
    std::ranges::sort(requestsWithForms, { }, [](const RequestWithForm& entry) {
        return std::pair { entry.request.pattern.index(), entry.request.flags.toRaw() };
    });

    Vector<RegExpRequest> result;
    for (RequestWithForm& entry : requestsWithForms) {
        uint32_t codeOffset = 64 * static_cast<uint32_t>(compiled.results.size() + 1);
        CompiledRegExp regExp;
        regExp.form = entry.form;
        compiled.results.append(WTF::move(regExp));
        section.entries.append(entry.form == Form::Code ? std::array<uint32_t, 2> { codeOffset, codeOffset + 32 } : std::array<uint32_t, 2> { 0, 0 });
        result.append(WTF::move(entry.request));
    }
    return result;
}

TextOfRegExpImage RegExpImage::encodeNamedText()
{
    BitVector namedStrings;
    requests.forEachString([&](StringID string) {
        namedStrings.set(string.index());
    });

    TextOfRegExpImage text;
    text.strings = Vector<ImageStringWord>(FillWith { }, program->strings.size(), ImageStringWord { 0 });
    for (size_t index : namedStrings) {
        DecodedString string = decodeForImage(program->strings[StringID(index)]);
        if (string.is16Bit() && text.characters.size() % sizeof(char16_t))
            text.characters.append(0);
        ImageLongString range { static_cast<uint32_t>(text.characters.size()), string.length() };
        text.characters.append(string.bytes());

        uint32_t firstWord = static_cast<uint32_t>(text.listedStrings.size());
        AOT_TEST_CHECK(firstWord <= ImageString::hashMask);
        text.listedStrings.grow(firstWord + ImageListedString::numberOfWords(range));
        ImageListedString::write(text.listedStrings.mutableSpan().subspan(firstWord), range);
        text.strings[index] = { ImageString::hasTextFlag | ImageString::hasNoHeaderFlag | (string.is16Bit() ? ImageString::is16BitFlag : 0) | firstWord };
    }
    return text;
}

void RegExpImage::compileAndLayOut()
{
    compiled = compileRegExps(*program, requests, context);
    auto templates = CodeTemplates::create(context.assemblerPolicy(), &context);
    AOT_TEST_CHECK(templates.has_value());
    std::optional<RegExpSection> laidOut = layOutRegExpSection(compiled.results.span(), context.assemblerPolicy(), *templates, &context);
    AOT_TEST_CHECK(laidOut.has_value());
    section = WTF::move(*laidOut);
    if (ExecutableAllocator::singleton().isValid())
        m_executableCode.emplace(section.bytes.span());
}

RegExpImage::RegExpImage(std::span<const Pattern> literals, const TestOptions& options, std::span<const Pattern> predictedTexts, CodeOfRegExpImage code)
    : context(wireOptions(options))
    , program(Typed::createEmptyProgram(context))
{
    AOT_TEST_CHECK(program);
    Vector<RegExpRequest> literalRequests = makeRequests(literals, RegExpRequestOrigin::Literal);
    requests = RegExpRequests(WTF::move(literalRequests), makeRequests(predictedTexts, RegExpRequestOrigin::Candidate));
    if (code == CodeOfRegExpImage::Compiled)
        compileAndLayOut();
    BitVector everyString(program->strings.size());
    for (size_t index = 0; index < program->strings.size(); ++index)
        everyString.quickSet(index);
    strings = StringNumbers(everyString, { }, program->strings.size());
    sections = encodeRegExpRecords({ requests, compiled, section, strings }, context);
    TextOfRegExpImage text = encodeNamedText();
    AOT_TEST_CHECK(!context.diagnostics().hasErrors());

    bytes = imageWithSections({
        { ImageSection::Strings, asByteSpan(text.strings.span()) },
        { ImageSection::LongStrings, asByteSpan(text.listedStrings.span()) },
        { ImageSection::Text, text.characters.span() },
        { ImageSection::RegExps, sections.records.span() },
        { ImageSection::RegExpHashes, sections.hashes.span() },
        { ImageSection::RegExpDetails, sections.details.span() },
    }, code == CodeOfRegExpImage::Compiled ? section.bytes.size() : 64 * (compiled.results.size() + 2));
    std::span<const uint8_t> imageBytes = bytes.span();
    image = Image::create(imageBytes, m_executableCode ? m_executableCode->start() : imageBytes.data() + reinterpret_cast<const ImageHeader*>(imageBytes.data())->codeOffset);

    m_vm = VM::create();
    m_lock.emplace(*m_vm);
    m_vm->aotState().image = image.get();
    m_vm->aotState().hasCompiledCode = true;
}

RegExpImage::RegExpImage(std::initializer_list<Pattern> literals, const TestOptions& options, std::initializer_list<Pattern> predictedTexts, CodeOfRegExpImage code)
    : RegExpImage(std::span<const Pattern> { literals }, options, std::span<const Pattern> { predictedTexts }, code)
{
}

RegExpImage::~RegExpImage()
{
    m_vm->aotState().hasCompiledCode = false;
    m_vm->aotState().image = nullptr;
    m_vm = nullptr;
    m_lock.reset();
}

std::span<ImageRegExpHash> RegExpImage::hashes()
{
    ImageSectionRange range = image->header().sections[static_cast<unsigned>(ImageSection::RegExpHashes)];
    return spanReinterpretCast<ImageRegExpHash>(bytes.span().subspan(range.offset, range.size));
}

size_t RegExpImage::indexOfRequest(const String& text, RegExpFlags flags) const
{
    std::span<const RegExpRequest> all = requests.all();
    auto request = std::ranges::find_if(all, [&](const RegExpRequest& candidate) {
        return candidate.flags == flags && program->strings.toWTFString(candidate.pattern) == text;
    });
    AOT_TEST_CHECK(request != all.end());
    return static_cast<size_t>(request - all.begin());
}

const RegExpSummary& RegExpImage::summaryOf(size_t request)
{
    const RegExpRequest& requested = requests.all()[request];
    const RegExpSummary* summary = context.regExpSummaries().summaryIfComputed(requested.pattern, requested.flags);
    AOT_TEST_CHECK(summary);
    return *summary;
}

static const ImageRegExp& checkFound(RegExpImage& regExps, const String& lookup, RegExpFlags flags, size_t request)
{
    const ImageRegExp* record = findRegExpRecord(regExps.vm(), lookup, flags);
    AOT_TEST_CHECK(record);
    const RegExpRequest& requested = regExps.requests.all()[request];
    const RegExpSummary& summary = regExps.summaryOf(request);
    AOT_TEST_CHECK(record->pattern == requested.pattern.index() && record->flags() == requested.flags);
    AOT_TEST_CHECK(record->form() == regExps.compiled.results[request].form);
    AOT_TEST_CHECK(record->isLiteral == (requested.origin == RegExpRequestOrigin::Literal));
    AOT_TEST_CHECK(record->numSubpatterns == summary.numSubpatterns && record->minimumSize == summary.minimumSize);
    AOT_TEST_CHECK(record->specificPattern() == summary.specificPattern);
    AOT_TEST_CHECK(regExpPatternOf(regExps.vm(), *record) == lookup);
    if (record->form() == Form::Code) {
        RegExpEntrypoints entrypoints = regExpEntrypointsOf(regExps.vm(), *record);
        AOT_TEST_CHECK(entrypoints.for8Bit == regExps.image->code().data() + regExps.section.entries[request][0]);
        AOT_TEST_CHECK(entrypoints.for16Bit == regExps.image->code().data() + regExps.section.entries[request][1]);
    }
    return *record;
}

static const ImageRegExp& checkFoundLiteral(RegExpImage& regExps, const String& pattern, RegExpFlags flags = { })
{
    size_t request = regExps.indexOfRequest(pattern, flags);
    const ImageRegExp& record = checkFound(regExps, pattern, flags, request);
    AOT_TEST_CHECK(regExps.records().idOf(record).index() == request);
    return record;
}

static void forceHash(RegExpImage& regExps, uint32_t hash)
{
    for (ImageRegExpHash& entry : regExps.hashes())
        entry.hash = hash;
}

static String repeatedPattern(unsigned length, char16_t unit, char16_t last = '+')
{
    Vector<char16_t> units(FillWith { }, length, unit);
    units.last() = last;
    return unit <= 0xff && last <= 0xff ? String::make8Bit(units.span()) : String(units.span());
}

static String regExpNestedTooDeepForMetadata()
{
    unsigned depth = static_cast<unsigned>(Limits::regExpParenthesesDepth) + 1;
    return makeString(repeatedPattern(depth, '(', '('), "a+"_s, repeatedPattern(depth, ')', ')'));
}

static String narrowString(std::u16string_view characters)
{
    return String::make8Bit(std::span<const char16_t> { characters });
}

static String wideString(std::u16string_view characters)
{
    return String(std::span<const char16_t> { characters });
}

static void testLiteralsAreNumberedByRequest()
{
    String latin1 = narrowString(u"xé+");
    String wide = wideString(u"x中+");
    String longNarrow = repeatedPattern(300, 'a');
    String longWide = repeatedPattern(300, 0x4e2d);
    RegExpImage regExps({ { "ab+c"_s }, { latin1 }, { wide }, { emptyString() }, { longNarrow }, { longWide } });
    AOT_TEST_CHECK(regExps.records().size() == 6);
    for (const String& pattern : { String("ab+c"_s), latin1, wide, emptyString(), longNarrow, longWide })
        checkFoundLiteral(regExps, pattern);
}

static void testFlags()
{
    RegExpImage regExps({ { "a+b"_s }, { "a+b"_s, Yarr::Flags::IgnoreCase } });
    AOT_TEST_CHECK(regExps.records().size() == 2);

    size_t plain = regExps.indexOfRequest("a+b"_s, { });
    size_t ignoringCase = regExps.indexOfRequest("a+b"_s, Yarr::Flags::IgnoreCase);
    AOT_TEST_CHECK(plain != ignoringCase);
    checkFound(regExps, "a+b"_s, { }, plain);
    checkFound(regExps, "a+b"_s, Yarr::Flags::IgnoreCase, ignoringCase);
    checkFound(regExps, "a+b"_s, Yarr::Flags::Global, plain);
    checkFound(regExps, "a+b"_s, Yarr::Flags::HasIndices, plain);
    checkFound(regExps, "a+b"_s, RegExpFlags { Yarr::Flags::Global, Yarr::Flags::HasIndices }, plain);
    checkFound(regExps, "a+b"_s, RegExpFlags { Yarr::Flags::Global, Yarr::Flags::IgnoreCase }, ignoringCase);
}

static void testWideStringOfNarrowCharacters()
{
    RegExpImage regExps({ { "ab+c"_s } });
    String wide = wideString(u"ab+c");
    AOT_TEST_CHECK(!wide.is8Bit());
    checkFound(regExps, wide, { }, 0);
}

static void testForms()
{
    String nested = regExpNestedTooDeepForMetadata();
    RegExpImage regExps({ { "c+d"_s }, { "abc"_s }, { "e+f"_s, { }, false }, { nested } });
    AOT_TEST_CHECK(regExps.records().size() == 4);
    AOT_TEST_CHECK(checkFoundLiteral(regExps, "c+d"_s).form() == Form::Code);
    AOT_TEST_CHECK(checkFoundLiteral(regExps, "abc"_s).form() == Form::Atom);

    const ImageRegExp& withoutCode = checkFoundLiteral(regExps, "e+f"_s);
    AOT_TEST_CHECK(withoutCode.form() == Form::NoCode && !withoutCode.codeFor8Bit && !withoutCode.codeFor16Bit);

    const ImageRegExp* withoutMetadata = findRegExpRecord(regExps.vm(), nested, { });
    AOT_TEST_CHECK(withoutMetadata && withoutMetadata->form() == Form::NoMetadata);
    AOT_TEST_CHECK(withoutMetadata->isLiteral);
    AOT_TEST_CHECK(!withoutMetadata->numSubpatterns && !withoutMetadata->minimumSize && !withoutMetadata->details);
    AOT_TEST_CHECK(regExps.sections.maxOutputSize == regExpOutputSizeWithoutSubpatterns);
}

static void testCandidates()
{
    String nested = regExpNestedTooDeepForMetadata();
    RegExpImage regExps({ { "l+m"_s } }, { }, { { "c+0"_s }, { "c+1"_s, { }, false }, { "c+(2)"_s }, { "abc"_s }, { nested } });
    AOT_TEST_CHECK(regExps.records().size() == 4);
    checkFoundLiteral(regExps, "l+m"_s);
    for (const String& pattern : { String("c+0"_s), String("c+(2)"_s), String("abc"_s) })
        checkFound(regExps, pattern, { }, regExps.indexOfRequest(pattern, { }));
    AOT_TEST_CHECK(!findRegExpRecord(regExps.vm(), "c+1"_s, { }) && !findRegExpRecord(regExps.vm(), nested, { }));
    AOT_TEST_CHECK(regExps.sections.maxOutputSize == 4);
}

static void testEvaluatedTexts()
{
    String nested = regExpNestedTooDeepForMetadata();
    RegExpImage regExps({ { "l+m"_s } }, { }, {
        { .text = "e+0"_s, .isEvaluated = true },
        { .text = "e+1"_s, .fitsCodeLimits = false, .isEvaluated = true },
        { .text = "c+1"_s, .fitsCodeLimits = false },
        { .text = "abc"_s, .isEvaluated = true },
        { .text = nested, .isEvaluated = true },
    });
    AOT_TEST_CHECK(regExps.records().size() == 4);
    checkFoundLiteral(regExps, "l+m"_s);
    for (const String& pattern : { String("e+0"_s), String("e+1"_s), String("abc"_s) })
        checkFound(regExps, pattern, { }, regExps.indexOfRequest(pattern, { }));
    AOT_TEST_CHECK(findRegExpRecord(regExps.vm(), "e+1"_s, { })->form() == Form::NoCode);
    AOT_TEST_CHECK(!findRegExpRecord(regExps.vm(), "c+1"_s, { }) && !findRegExpRecord(regExps.vm(), nested, { }));
}

static void testOrder()
{
    Vector<Pattern> patterns;
    for (unsigned k = 0; k < 50; ++k)
        patterns.append({ makeString("p"_s, k, '+') });
    RegExpImage regExps(patterns.span());

    RegExpRecords records = regExps.records();
    std::span<const RegExpRequest> requests = regExps.requests.all();
    AOT_TEST_CHECK(records.size() == patterns.size() && requests.size() == patterns.size());
    for (uint32_t index = 0; index < records.size(); ++index)
        AOT_TEST_CHECK(records.at(RegExpRecordID(index)).pattern == requests[index].pattern.index());

    std::span<ImageRegExpHash> hashes = regExps.hashes();
    AOT_TEST_CHECK(hashes.size() == patterns.size());
    AOT_TEST_CHECK(std::ranges::is_sorted(hashes, { }, &ImageRegExpHash::hash));
    BitVector seenRecords(records.size());
    for (const ImageRegExpHash& entry : hashes)
        AOT_TEST_CHECK(entry.record < records.size() && !seenRecords.set(entry.record));
    for (const Pattern& pattern : patterns)
        checkFoundLiteral(regExps, pattern.text);
}

static void checkNotFound(const String& pattern, const String& lookup, RegExpFlags flags = { })
{
    RegExpImage regExps({ { pattern } });
    AOT_TEST_CHECK(!findRegExpRecord(regExps.vm(), lookup, flags));
    forceHash(regExps, ImageRegExp::hashOf(lookup.hash(), flags));
    AOT_TEST_CHECK(!findRegExpRecord(regExps.vm(), lookup, flags));
}

static void testOtherLastCharacter()
{
    checkNotFound("abcd"_s, "abce"_s);
    checkNotFound("ab+cd"_s, "ab+ce"_s);
    checkNotFound(repeatedPattern(300, 'a', 'x'), repeatedPattern(300, 'a', 'y'));
}

static void testOtherFlags()
{
    checkNotFound("ab+cd"_s, "ab+cd"_s, Yarr::Flags::IgnoreCase);
    checkNotFound("ab+cd"_s, "ab+cd"_s, Yarr::Flags::Multiline);
}

static void testOtherLength()
{
    checkNotFound("ab+cd"_s, "ab+cde"_s);
    checkNotFound("ab+cd"_s, "ab+c"_s);
}

static void testEmptyTable()
{
    RegExpImage regExps { std::span<const Pattern> { } };
    AOT_TEST_CHECK(regExps.sections.records.isEmpty() && regExps.sections.hashes.isEmpty() && regExps.sections.details.isEmpty());
    AOT_TEST_CHECK(regExps.sections.maxOutputSize == regExpOutputSizeWithoutSubpatterns);
    AOT_TEST_CHECK(!findRegExpRecord(regExps.vm(), "abcd"_s, { }));
}

static void checkCollidingPatterns(const String& first, const String& second)
{
    RegExpImage regExps({ { first }, { second } });
    for (const String& pattern : { first, second }) {
        forceHash(regExps, ImageRegExp::hashOf(pattern.hash(), { }));
        checkFoundLiteral(regExps, pattern);
    }
}

static void testForcedHashCollision()
{
    checkCollidingPatterns("ab+cd"_s, "ab+ce"_s);
    checkCollidingPatterns(repeatedPattern(300, 'a', 'x'), repeatedPattern(300, 'a', 'y'));
}

static void testAtoms()
{
    RegExpImage regExps({ { "abc"_s }, { "\\."_s } });
    const ImageRegExp& plain = checkFoundLiteral(regExps, "abc"_s);
    AOT_TEST_CHECK(plain.form() == Form::Atom && plain.atom == plain.pattern);
    AOT_TEST_CHECK(regExpAtomOf(regExps.vm(), plain) == "abc"_s);

    const ImageRegExp& escaped = checkFoundLiteral(regExps, "\\."_s);
    AOT_TEST_CHECK(escaped.form() == Form::Atom && escaped.atom != escaped.pattern);
    AOT_TEST_CHECK(regExpAtomOf(regExps.vm(), escaped) == "."_s);
}

static void testSourceIsPattern()
{
    RegExpImage regExps({ { "a\\/b+"_s }, { "a/b+"_s }, { emptyString() } });
    AOT_TEST_CHECK(checkFoundLiteral(regExps, "a\\/b+"_s).sourceIsPattern);
    AOT_TEST_CHECK(!checkFoundLiteral(regExps, "a/b+"_s).sourceIsPattern);
    AOT_TEST_CHECK(!checkFoundLiteral(regExps, emptyString()).sourceIsPattern);
}

static void checkNamedGroupsRoundTrip(const String& text)
{
    Yarr::ErrorCode error = Yarr::ErrorCode::NoError;
    Yarr::YarrPattern pattern(text, { }, error, Yarr::ExecutionMode::IncludeSubpatterns, Yarr::PatternAnalysis::Performed);
    AOT_TEST_CHECK(!Yarr::hasError(error));

    RegExpImage regExps({ { "ab+c"_s }, { text } });
    const ImageRegExp& record = checkFoundLiteral(regExps, text);
    AOT_TEST_CHECK(record.hasNamedGroups);
    AOT_TEST_CHECK(regExps.sections.maxOutputSize == pattern.offsetsSize());

    RegExpNamedGroups groups = decodeRegExpNamedGroups(regExps.vm(), record);
    AOT_TEST_CHECK(groups.numDuplicateNamedCaptureGroups == pattern.m_numDuplicateNamedCaptureGroups);
    AOT_TEST_CHECK(groups.captureGroupNames.size() == pattern.m_captureGroupNames.size());
    for (size_t index = 0; index < groups.captureGroupNames.size(); ++index)
        AOT_TEST_CHECK(groups.captureGroupNames[index].string() == pattern.m_captureGroupNames[index]);
    AOT_TEST_CHECK(groups.namedGroupToParenIndices.size() == pattern.m_namedGroupToParenIndices.size());
    for (auto& [name, parenIndices] : pattern.m_namedGroupToParenIndices) {
        auto found = groups.namedGroupToParenIndices.find(name);
        AOT_TEST_CHECK(found != groups.namedGroupToParenIndices.end() && found->value == parenIndices);
    }
}

static void testNamedGroups()
{
    checkNamedGroupsRoundTrip("(?<year>\\d{4})-(\\d+)-(?<day>\\d{2})"_s);
    checkNamedGroupsRoundTrip("(?<n>a+)|(?<n>b+)|(?<m>c)(?<n>d)"_s);
    checkNamedGroupsRoundTrip(wideString(u"(?<π>a+)"));
}

static void checkCharacterSetRoundTrip(const String& text, Yarr::SpecificPattern specificPattern, std::initializer_list<std::pair<char16_t, bool>> members)
{
    RegExpImage regExps({ { "(?<n>a+)"_s }, { text } });
    size_t request = regExps.indexOfRequest(text, { });
    const ImageRegExp& record = checkFoundLiteral(regExps, text);
    AOT_TEST_CHECK(record.specificPattern() == specificPattern && record.hasCharacterSet());
    AOT_TEST_CHECK(regExps.summaryOf(request).characterSet);

    Yarr::CharacterSetView expected(*regExps.summaryOf(request).characterSet);
    Yarr::CharacterSetView decoded = regExpCharacterSetOf(regExps.vm(), record);
    AOT_TEST_CHECK(!expected.wideRanges().empty());
    AOT_TEST_CHECK(decoded.bits() == expected.bits() && std::ranges::equal(decoded.wideRanges(), expected.wideRanges(), [](const Yarr::CharacterRange& a, const Yarr::CharacterRange& b) {
        return a.begin == b.begin && a.end == b.end;
    }));
    for (auto [character, isMember] : members)
        AOT_TEST_CHECK(decoded.contains(character) == isMember && expected.contains(character) == isMember);
}

static void testCharacterSets()
{
    checkCharacterSetRoundTrip("[a-z\\xff\\u0400-\\u04ff\\u3041-\\u3096]"_s, Yarr::SpecificPattern::CharacterClass, {
        { 0, false }, { 'a', true }, { 'z', true }, { '{', false }, { 0xff, true }, { 0x100, false },
        { 0x3ff, false }, { 0x400, true }, { 0x4ff, true }, { 0x500, false },
        { 0x3040, false }, { 0x3041, true }, { 0x3096, true }, { 0x3097, false }, { 0xffff, false },
    });
    checkCharacterSetRoundTrip("[^a-z\\xff\\u0400-\\u04ff\\u3041-\\u3096]+"_s, Yarr::SpecificPattern::CharacterClassPlus, {
        { 0, true }, { 'a', false }, { 'z', false }, { '{', true }, { 0xff, false }, { 0x100, true },
        { 0x3ff, true }, { 0x400, false }, { 0x4ff, false }, { 0x500, true },
        { 0x3040, true }, { 0x3041, false }, { 0x3096, false }, { 0x3097, true }, { 0xffff, true },
    });
}

static void testEncodingIsDeterministic()
{
    std::array<Pattern, 6> literals { Pattern { "ab+c"_s }, Pattern { "abc"_s }, Pattern { "(?<n>a+)|(?<n>b+)"_s }, Pattern { "[a-z\\u0400-\\u04ff]+"_s }, Pattern { "ab+c"_s, Yarr::Flags::IgnoreCase }, Pattern { "e+f"_s, { }, false } };
    RegExpImage first(literals);
    RegExpImage second(literals, { .threadCount = 4 });
    AOT_TEST_CHECK(!first.sections.records.isEmpty() && !first.sections.hashes.isEmpty() && !first.sections.details.isEmpty());
    AOT_TEST_CHECK(first.sections.records == second.sections.records);
    AOT_TEST_CHECK(first.sections.hashes == second.sections.hashes);
    AOT_TEST_CHECK(first.sections.details == second.sections.details);
    AOT_TEST_CHECK(first.sections.maxOutputSize == second.sections.maxOutputSize);

    RegExpSections again = encodeRegExpRecords({ first.requests, first.compiled, first.section, first.strings }, first.context);
    AOT_TEST_CHECK(again.records == first.sections.records && again.hashes == first.sections.hashes && again.details == first.sections.details);
}

static void testPatternsForWhole8BitSubjects()
{
    auto matchesOnlyWhole8BitSubjects = [](ASCIILiteral pattern, RegExpFlags flags = { }) {
        RegExpSummary summary = summarizeRegExp(pattern, flags);
        AOT_TEST_CHECK(summary.canBeCreated());
        return summary.matchesOnlyWhole8BitSubjects;
    };
    for (ASCIILiteral pattern : { "^[a-z]+$"_s, "^\\d{4}-\\d{2}$"_s, "^[a-z\\xe9]+$"_s, "^(?:a+|b+)c$"_s, "^(a+)-\\1$"_s, "^(?=.)\\w+$"_s, "^(?=([a-z]))\\1\\w*$"_s })
        AOT_TEST_CHECK(matchesOnlyWhole8BitSubjects(pattern));
    for (Yarr::Flags flag : { Yarr::Flags::Sticky, Yarr::Flags::Unicode, Yarr::Flags::UnicodeSets })
        AOT_TEST_CHECK(matchesOnlyWhole8BitSubjects("^[a-z]+$"_s, flag));
    AOT_TEST_CHECK(matchesOnlyWhole8BitSubjects("^\\p{ASCII}+$"_s, Yarr::Flags::Unicode));
    AOT_TEST_CHECK(!matchesOnlyWhole8BitSubjects("^\\p{L}+$"_s, Yarr::Flags::Unicode));
    AOT_TEST_CHECK(!matchesOnlyWhole8BitSubjects("^[^a-z]+$"_s, Yarr::Flags::UnicodeSets));
    AOT_TEST_CHECK(!matchesOnlyWhole8BitSubjects("^[\\q{ab}c]+$"_s, Yarr::Flags::UnicodeSets));

    for (ASCIILiteral pattern : { "[a-z]+"_s, "^[a-z]+"_s, "[a-z]+$"_s, "^a+$|b+"_s })
        AOT_TEST_CHECK(!matchesOnlyWhole8BitSubjects(pattern));
    AOT_TEST_CHECK(!matchesOnlyWhole8BitSubjects("^[a-z]+$"_s, Yarr::Flags::Multiline));
    for (ASCIILiteral pattern : { "^[^a-z]+$"_s, "^.+$"_s, "^\\s+$"_s, "^\\S+$"_s, "^\\W+$"_s, "^\\D+$"_s, "^[a-z\\u0100]+$"_s, "^a\\u4e2d+$"_s, "^(?i:k)+$"_s, "^(?=(.*))\\1$"_s })
        AOT_TEST_CHECK(!matchesOnlyWhole8BitSubjects(pattern));
    AOT_TEST_CHECK(!matchesOnlyWhole8BitSubjects("^[a-z]+$"_s, Yarr::Flags::IgnoreCase));
}

static RegExpSectionItem::Kind kindOfCodeAt(const RegExpSection& section, uint32_t offset)
{
    auto item = std::ranges::find_if(section.items, [&](const RegExpSectionItem& candidate) {
        return candidate.offset == offset && candidate.kind != RegExpSectionItem::Kind::DuplicateBody;
    });
    AOT_TEST_CHECK(item != section.items.end());
    return item->kind;
}

static void testAnchored8BitLiteralHasOneBody()
{
    using Kind = RegExpSectionItem::Kind;
    struct Literal {
        ASCIILiteral pattern;
        RegExpFlags flags;
        bool hasCodeFor8BitSubjectsAlone;
    };
    const Literal literals[] = {
        { "^[a-z]+$"_s, { }, true },
        { "^([a-z]+)-(\\d+)$"_s, { }, true },
        { "^[a-z]+$"_s, Yarr::Flags::Sticky, true },
        { "^[a-z]+$"_s, Yarr::Flags::Unicode, true },
        { "^[a-z]+$"_s, Yarr::Flags::IgnoreCase, false },
        { "^[a-z]+"_s, { }, false },
        { "^[^a-z]+$"_s, { }, false },
    };
    Vector<Pattern> patterns;
    for (const Literal& literal : literals)
        patterns.append({ literal.pattern, literal.flags });
    RegExpImage regExps(patterns.span(), { }, { }, CodeOfRegExpImage::Compiled);

    unsigned numberOfBodies = 0;
    for (const Literal& literal : literals) {
        size_t request = regExps.indexOfRequest(literal.pattern, literal.flags);
        const CompiledRegExp& compiled = regExps.compiled.results[request];
        AOT_TEST_CHECK(regExps.requests.all()[request].origin == RegExpRequestOrigin::Literal);
        bool hasCodeFor16BitSubjects = !!compiled.code[1];
        AOT_TEST_CHECK(compiled.form == Form::Code && compiled.code[0] && hasCodeFor16BitSubjects != literal.hasCodeFor8BitSubjectsAlone);
        AOT_TEST_CHECK(checkFoundLiteral(regExps, literal.pattern, literal.flags).hasCodeFor8BitSubjectsAlone == literal.hasCodeFor8BitSubjectsAlone);

        auto [entryFor8Bit, entryFor16Bit] = regExps.section.entries[request];
        AOT_TEST_CHECK(kindOfCodeAt(regExps.section, entryFor8Bit) == Kind::Body);
        AOT_TEST_CHECK(kindOfCodeAt(regExps.section, entryFor16Bit) == (literal.hasCodeFor8BitSubjectsAlone ? Kind::Routine : Kind::Body));
        numberOfBodies += literal.hasCodeFor8BitSubjectsAlone ? 1 : 2;
    }
    auto placedBodies = std::ranges::count_if(regExps.section.items, [](const RegExpSectionItem& item) {
        return item.kind == Kind::Body || item.kind == Kind::DuplicateBody;
    });
    AOT_TEST_CHECK(static_cast<unsigned>(placedBodies) == numberOfBodies);
}

static void testBytesOfSectionAreCounted()
{
    using Kind = RegExpSectionItem::Kind;
    RegExpImage regExps({ { "ab+c"_s }, { "^[a-z]+$"_s }, { "[\\u0400-\\u04ff\\u0530-\\u058f\\u05d0-\\u05ea\\u0620-\\u064a\\u0905-\\u0939\\u0e01-\\u0e30\\u3041-\\u3096\\u30a1-\\u30fa]+"_s } }, { }, { }, CodeOfRegExpImage::Compiled);
    const RegExpSection& section = regExps.section;
    Vector<uint64_t> statistics = regExps.context.statistics().values();
    auto statistic = [&](StatisticID id) {
        return statistics[static_cast<unsigned>(id)];
    };
    auto isEntry = [&](uint32_t offset, unsigned charSizeIndex) {
        return section.entries.containsIf([&](const std::array<uint32_t, 2>& entries) {
            return entries[charSizeIndex] == offset;
        });
    };

    uint64_t endOfCode = 0;
    uint64_t sizeOf16BitBodies = 0;
    for (const RegExpSectionItem& item : section.items) {
        if (item.kind != Kind::Table)
            endOfCode = std::max<uint64_t>(endOfCode, item.offset + item.size);
        if (item.kind == Kind::Body && isEntry(item.offset, 1) && !isEntry(item.offset, 0))
            sizeOf16BitBodies += item.size;
    }
    AOT_TEST_CHECK(sizeOf16BitBodies && sizeOf16BitBodies < endOfCode && section.dataOffset < section.bytes.size());
    AOT_TEST_CHECK(statistic(StatisticID::RegExpCodeBytesEmitted) == endOfCode);
    AOT_TEST_CHECK(statistic(StatisticID::RegExp16BitCodeBytes) == sizeOf16BitBodies);
    AOT_TEST_CHECK(statistic(StatisticID::RegExpTableBytesEmitted) == section.bytes.size() - section.dataOffset);
    AOT_TEST_CHECK(statistic(StatisticID::PaddingBytesBetweenCodeParts) == section.dataOffset - endOfCode);
}

static Vector<int> matchWithCodeOfImage(RegExpImage& regExps, RegExp* regExp, const String& subject, unsigned start = 0)
{
    std::span<int> offsets = regExp->ovectorSpan();
    int position = regExp->matchInline(nullptr, regExps.vm(), subject, start, offsets);
    if (position < 0)
        return { };
    AOT_TEST_CHECK(position == offsets[0]);
    return Vector<int>(offsets);
}

static void testWideSubjectsOfLiteralWith8BitCodeAlone()
{
    String groups = "^([a-z]+)-(\\d+)$"_s;
    String latin1 = "^[a-z\\xe9]+$"_s;
    String letters = "^[a-z]+$"_s;
    RegExpImage regExps({ { groups }, { latin1 }, { letters, Yarr::Flags::Unicode } }, { }, { }, CodeOfRegExpImage::Compiled);
    VM& vm = regExps.vm();

    for (RegExpFlags flags : { RegExpFlags { }, RegExpFlags { Yarr::Flags::Global } }) {
        const ImageRegExp& record = checkFoundLiteral(regExps, groups);
        AOT_TEST_CHECK(record.hasCodeFor8BitSubjectsAlone);
        RegExp* regExp = RegExp::createFromImage(vm, groups, flags, record);
        const Vector<int> whole { 0, 6, 0, 3, 4, 6 };
        AOT_TEST_CHECK(matchWithCodeOfImage(regExps, regExp, "abc-12"_s) == whole);
        AOT_TEST_CHECK(regExp->getRegExpJITCodeBlockConcurrently()->has8BitCode() && !regExp->getRegExpJITCodeBlockConcurrently()->has16BitCode());
        AOT_TEST_CHECK(matchWithCodeOfImage(regExps, regExp, wideString(u"abc-12")) == whole);
        for (const String& subject : { wideString(u"abc-12中"), wideString(u"中abc-12"), wideString(u"abc中-12"), wideString(u"abc-"), wideString(u"abc-12 ") })
            AOT_TEST_CHECK(matchWithCodeOfImage(regExps, regExp, subject).isEmpty());
        for (const String& subject : { String("abc-12"_s), wideString(u"abc-12"), wideString(u"中bc-12") })
            AOT_TEST_CHECK(matchWithCodeOfImage(regExps, regExp, subject, 1).isEmpty());
    }

    RegExp* regExp = RegExp::createFromImage(vm, latin1, { }, checkFoundLiteral(regExps, latin1));
    AOT_TEST_CHECK(matchWithCodeOfImage(regExps, regExp, wideString(u"café")) == (Vector<int> { 0, 4 }));
    AOT_TEST_CHECK(matchWithCodeOfImage(regExps, regExp, narrowString(u"café")) == (Vector<int> { 0, 4 }));
    AOT_TEST_CHECK(matchWithCodeOfImage(regExps, regExp, wideString(u"cafē")).isEmpty());

    const ImageRegExp& recordOfLetters = checkFoundLiteral(regExps, letters, Yarr::Flags::Unicode);
    AOT_TEST_CHECK(recordOfLetters.hasCodeFor8BitSubjectsAlone);
    regExp = RegExp::createFromImage(vm, letters, Yarr::Flags::Unicode, recordOfLetters);
    AOT_TEST_CHECK(matchWithCodeOfImage(regExps, regExp, wideString(u"abc")) == (Vector<int> { 0, 3 }));
    AOT_TEST_CHECK(matchWithCodeOfImage(regExps, regExp, wideString(u"ab\U0001f600")).isEmpty());
    AOT_TEST_CHECK(matchWithCodeOfImage(regExps, regExp, wideString(u"\U0001f600ab"), 1).isEmpty());
}

std::span<const NativeTest> regExpTableTests()
{
    static constexpr NativeTest tests[] = {
        { "image/regexps/literals-are-numbered-by-request"_s, testLiteralsAreNumberedByRequest },
        { "image/regexps/flags"_s, testFlags },
        { "image/regexps/wide-string-of-narrow-characters"_s, testWideStringOfNarrowCharacters },
        { "image/regexps/forms"_s, testForms },
        { "image/regexps/candidates"_s, testCandidates },
        { "image/regexps/evaluated-texts"_s, testEvaluatedTexts },
        { "image/regexps/order"_s, testOrder },
        { "image/regexps/other-last-character"_s, testOtherLastCharacter },
        { "image/regexps/other-flags"_s, testOtherFlags },
        { "image/regexps/other-length"_s, testOtherLength },
        { "image/regexps/empty-table"_s, testEmptyTable },
        { "image/regexps/forced-hash-collision"_s, testForcedHashCollision },
        { "image/regexps/atoms"_s, testAtoms },
        { "image/regexps/source-is-pattern"_s, testSourceIsPattern },
        { "image/regexps/named-groups"_s, testNamedGroups },
        { "image/regexps/character-sets"_s, testCharacterSets },
        { "image/regexps/encoding-is-deterministic"_s, testEncodingIsDeterministic },
        { "image/regexps/patterns-for-whole-8-bit-subjects"_s, testPatternsForWhole8BitSubjects },
        { "image/regexps/anchored-8-bit-literal-has-one-body"_s, testAnchored8BitLiteralHasOneBody },
        { "image/regexps/bytes-of-section-are-counted"_s, testBytesOfSectionAreCounted },
        { "image/regexps/wide-subjects-of-literal-with-8-bit-code-alone"_s, whenExecutable<testWideSubjectsOfLiteralWith8BitCodeAlone> },
    };
    return tests;
}

} // namespace JSC::AOT::ImageTests

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT) && ENABLE(YARR_JIT)
