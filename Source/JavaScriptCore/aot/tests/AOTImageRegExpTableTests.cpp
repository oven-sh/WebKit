/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT) && ENABLE(YARR_JIT)

#include "AOTIRTestSupport.h"
#include "AOTIdentifierNumbering.h"
#include "AOTImageTestSupport.h"
#include "AOTRegExpCompiler.h"
#include "AOTRegExpRecords.h"
#include "AOTRegExpSummary.h"
#include "AOTStringUses.h"
#include "AOTTextEncoder.h"
#include "AOTVector.h"
#include "JSLock.h"
#include "VM.h"
#include "YarrPattern.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
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
    // For a pattern that is no literal: the build put the text together from constants.
    bool isEvaluated { false };
};

struct RequestWithForm {
    RegExpRequest request;
    Form form;
};

class RegExpImage {
    WTF_MAKE_NONCOPYABLE(RegExpImage);
public:
    RegExpImage(std::span<const Pattern> literals, std::span<const Pattern> candidates = { }, const TestOptions& = { });
    RegExpImage(std::initializer_list<Pattern> literals, std::initializer_list<Pattern> candidates = { }, const TestOptions& = { });
    ~RegExpImage();

    VM& vm() { return *m_vm; }
    RegExpRecords records() const { return RegExpRecords(*image); }
    std::span<ImageRegExpHash> hashes();
    size_t indexOfRequest(const String& text, RegExpFlags, RegExpRequestOrigin) const;
    const RegExpSummary& summaryOf(size_t request);

    Typed::TestProgram program;
    BuildContext context;
    RegExpRequests requests;
    CompiledRegExps compiled;
    RegExpSection section;
    RegExpSections sections;
    AlignedBytes bytes;
    std::unique_ptr<Image> image;

private:
    RequestWithForm makeRequest(const Pattern&, RegExpRequestOrigin);
    Vector<RegExpRequest> makeRequests(std::span<const Pattern>, RegExpRequestOrigin originUnlessEvaluated);
    EncodedText encodeNamedText();

    RefPtr<VM> m_vm;
    std::optional<JSLockHolder> m_lock;
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
    Typed::StringTable& strings = program.program.strings;
    RegExpRequest request {
        .pattern = strings.intern(StringView(pattern.text)),
        .flags = pattern.flags,
        .origin = origin,
        .sites = { RegExpSite { FunctionID(0u), ValueID(0u) } },
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

EncodedText RegExpImage::encodeNamedText()
{
    // Only the image writer makes headers of strings, and this image is put together by hand. So its strings are of the kind that has none.
    StringUses uses;
    BitVector stringsOnlyShown;
    requests.forEachString([&](StringID string) {
        stringsOnlyShown.set(string.index());
    });
    IdentifierNumbering identifiers;
    StringConstantNumbering constants;
    return encodeText({ program.program, uses, identifiers, constants, stringsOnlyShown, &stringsOnlyShown }, context);
}

RegExpImage::RegExpImage(std::span<const Pattern> literals, std::span<const Pattern> candidates, const TestOptions& options)
    : context(wireOptions(options))
{
    Vector<RegExpRequest> literalRequests = makeRequests(literals, RegExpRequestOrigin::Literal);
    requests = RegExpRequests(WTF::move(literalRequests), makeRequests(candidates, RegExpRequestOrigin::Candidate));
    sections = encodeRegExpRecords({ requests, compiled, section }, context);
    EncodedText text = encodeNamedText();
    AOT_TEST_CHECK(!context.diagnostics().hasErrors());

    std::span encodedStrings = spanReinterpretCast<const ImageString>(text.strings.span());
    Vector<uint32_t> listedStrings;
    Vector<ImageStringWord> stringsInImage(encodedStrings.size(), [&](size_t index) {
        const ImageString& string = encodedStrings[index];
        if (!string.hasText())
            return string.inImage(0);
        ImageLongString range = string.range(spanReinterpretCast<const ImageLongString>(text.longStrings.span()));
        uint32_t firstWord = static_cast<uint32_t>(listedStrings.size());
        listedStrings.grow(firstWord + ImageListedString::numberOfWords(range));
        ImageListedString::write(listedStrings.mutableSpan().subspan(firstWord), range);
        return string.inImage(firstWord);
    });
    std::span encodedIdentifiers = spanReinterpretCast<const ImageIdentifier>(text.identifiers.span());
    Vector<ImageIdentifierWord> identifiersInImage(encodedIdentifiers.size(), [&](size_t number) {
        return encodedIdentifiers[number].inImage();
    });
    bytes = imageWithSections({
        { ImageSection::Strings, asByteSpan(stringsInImage.span()) },
        { ImageSection::LongStrings, asByteSpan(listedStrings.span()) },
        { ImageSection::Text, text.text.span() },
        { ImageSection::Identifiers, asByteSpan(identifiersInImage.span()) },
        { ImageSection::RegExps, sections.records.span() },
        { ImageSection::RegExpHashes, sections.hashes.span() },
        { ImageSection::RegExpDetails, sections.details.span() },
    }, 64 * (compiled.results.size() + 2));
    std::span<const uint8_t> imageBytes = bytes.span();
    image = Image::create(imageBytes, imageBytes.data() + reinterpret_cast<const ImageHeader*>(imageBytes.data())->codeOffset);

    m_vm = VM::create();
    m_lock.emplace(*m_vm);
    m_vm->aotState().image = image.get();
    m_vm->aotState().hasCompiledCode = true;
}

RegExpImage::RegExpImage(std::initializer_list<Pattern> literals, std::initializer_list<Pattern> candidates, const TestOptions& options)
    : RegExpImage(std::span<const Pattern> { literals }, std::span<const Pattern> { candidates }, options)
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

size_t RegExpImage::indexOfRequest(const String& text, RegExpFlags flags, RegExpRequestOrigin origin) const
{
    std::span<const RegExpRequest> all = requests.all();
    auto request = std::ranges::find_if(all, [&](const RegExpRequest& candidate) {
        return candidate.origin == origin && candidate.flags == flags && program.program.strings.toWTFString(candidate.pattern) == text;
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
    size_t request = regExps.indexOfRequest(pattern, flags, RegExpRequestOrigin::Literal);
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

    size_t plain = regExps.indexOfRequest("a+b"_s, { }, RegExpRequestOrigin::Literal);
    size_t ignoringCase = regExps.indexOfRequest("a+b"_s, Yarr::Flags::IgnoreCase, RegExpRequestOrigin::Literal);
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
    String nested = "((a+))"_s;
    RegExpImage regExps({ { "c+d"_s }, { "abc"_s }, { "e+f"_s, { }, false }, { nested } }, { }, { .loweredLimit = LimitID::RegExpNestingDepthWithCode, .loweredLimitValue = 1 });
    AOT_TEST_CHECK(regExps.records().size() == 4);
    AOT_TEST_CHECK(checkFoundLiteral(regExps, "c+d"_s).form() == Form::Code);
    AOT_TEST_CHECK(checkFoundLiteral(regExps, "abc"_s).form() == Form::Atom);

    const ImageRegExp& withoutCode = checkFoundLiteral(regExps, "e+f"_s);
    AOT_TEST_CHECK(withoutCode.form() == Form::NoCode && !withoutCode.codeFor8Bit && !withoutCode.codeFor16Bit);

    const ImageRegExp* withoutMetadata = findRegExpRecord(regExps.vm(), nested, { });
    AOT_TEST_CHECK(withoutMetadata && withoutMetadata->form() == Form::NoMetadata);
    AOT_TEST_CHECK(!withoutMetadata->numSubpatterns && !withoutMetadata->minimumSize && !withoutMetadata->details);
    AOT_TEST_CHECK(regExps.sections.maxOutputSize == regExpOutputSizeWithoutSubpatterns);
}

static void testCandidates()
{
    String nested = "((n+))"_s;
    RegExpImage regExps({ { "l+m"_s } }, { { "c+0"_s }, { "c+1"_s, { }, false }, { "c+(2)"_s }, { "abc"_s }, { nested } }, { .loweredLimit = LimitID::RegExpNestingDepthWithCode, .loweredLimitValue = 1 });
    AOT_TEST_CHECK(regExps.records().size() == 4);
    checkFoundLiteral(regExps, "l+m"_s);
    for (const String& pattern : { String("c+0"_s), String("c+(2)"_s), String("abc"_s) })
        checkFound(regExps, pattern, { }, regExps.indexOfRequest(pattern, { }, RegExpRequestOrigin::Candidate));
    // Flow names far more texts than the program creates, so one that gets no code gets no record either.
    AOT_TEST_CHECK(!findRegExpRecord(regExps.vm(), "c+1"_s, { }) && !findRegExpRecord(regExps.vm(), nested, { }));
    AOT_TEST_CHECK(regExps.sections.maxOutputSize == 4);
}

static void testEvaluatedTexts()
{
    String nested = "((n+))"_s;
    RegExpImage regExps({ { "l+m"_s } }, {
        { .text = "e+0"_s, .isEvaluated = true },
        { .text = "e+1"_s, .fitsCodeLimits = false, .isEvaluated = true },
        { .text = "c+1"_s, .fitsCodeLimits = false },
        { .text = "abc"_s, .isEvaluated = true },
        { .text = nested, .isEvaluated = true },
    }, { .loweredLimit = LimitID::RegExpNestingDepthWithCode, .loweredLimitValue = 1 });
    AOT_TEST_CHECK(regExps.records().size() == 4);
    checkFoundLiteral(regExps, "l+m"_s);
    for (const String& pattern : { String("e+0"_s), String("e+1"_s), String("abc"_s) })
        checkFound(regExps, pattern, { }, regExps.indexOfRequest(pattern, { }, RegExpRequestOrigin::Evaluated));
    // With the record the run time creates the object without parsing the text. A record without metadata would save it nothing.
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
    std::span<const RegExpRequest> literals = regExps.requests.literals();
    AOT_TEST_CHECK(records.size() == patterns.size() && literals.size() == patterns.size());
    for (uint32_t index = 0; index < records.size(); ++index)
        AOT_TEST_CHECK(records.at(RegExpRecordID(index)).pattern == literals[index].pattern.index());

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
    size_t request = regExps.indexOfRequest(text, { }, RegExpRequestOrigin::Literal);
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
    std::array<Pattern, 5> literals { Pattern { "ab+c"_s }, Pattern { "abc"_s }, Pattern { "(?<n>a+)|(?<n>b+)"_s }, Pattern { "[a-z\\u0400-\\u04ff]+"_s }, Pattern { "ab+c"_s, Yarr::Flags::IgnoreCase } };
    std::array<Pattern, 4> candidates { Pattern { "c+0"_s }, Pattern { "c+1"_s, { }, false }, Pattern { .text = "e+1"_s, .fitsCodeLimits = false, .isEvaluated = true }, Pattern { .text = "e+2"_s, .isEvaluated = true } };
    RegExpImage first(literals, candidates);
    RegExpImage second(literals, candidates, { .threadCount = 4 });
    AOT_TEST_CHECK(!first.sections.records.isEmpty() && !first.sections.hashes.isEmpty() && !first.sections.details.isEmpty());
    AOT_TEST_CHECK(first.sections.records == second.sections.records);
    AOT_TEST_CHECK(first.sections.hashes == second.sections.hashes);
    AOT_TEST_CHECK(first.sections.details == second.sections.details);
    AOT_TEST_CHECK(first.sections.maxOutputSize == second.sections.maxOutputSize);

    RegExpSections again = encodeRegExpRecords({ first.requests, first.compiled, first.section }, first.context);
    AOT_TEST_CHECK(again.records == first.sections.records && again.hashes == first.sections.hashes && again.details == first.sections.details);
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
    };
    return tests;
}

} // namespace JSC::AOT::ImageTests

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT) && ENABLE(YARR_JIT)
