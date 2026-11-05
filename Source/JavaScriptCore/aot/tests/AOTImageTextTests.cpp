/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTIdentifierNumbering.h"
#include "AOTImageTestBodies.h"
#include "AOTImageText.h"
#include "AOTNumbering.h"
#include "AOTTextEncoder.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <wtf/ASCIICType.h>
#include <wtf/HexNumber.h>
#include <wtf/MathExtras.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Threading.h>
#include <wtf/text/AtomString.h>
#include <wtf/text/AtomStringImpl.h>
#include <wtf/text/AtomStringTable.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringHasherInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

using namespace ImageTests;

static constexpr ASCIILiteral textReadThroughHeader = "read through its header"_s;
static constexpr ASCIILiteral textInUnreachedFunction = "never used"_s;
static constexpr ASCIILiteral nameOfFreeVariable = "sink"_s;
static constexpr ASCIILiteral pathShownInTraces = "shown/main.js"_s;

static constexpr ASCIILiteral textThatHoldsOthers = "a text that holds others"_s;
static constexpr ASCIILiteral tailOfText = "holds others"_s;
static constexpr ASCIILiteral innerPartOfText = "that hol"_s;
static constexpr ASCIILiteral textHeldByNone = "held by none"_s;
static constexpr ASCIILiteral textThatEndsWithName = "kitchen sink"_s;
static constexpr ASCIILiteral textAroundName = "a name_within_text here"_s;
static constexpr ASCIILiteral nameWithinText = "name_within_text"_s;
static constexpr ASCIILiteral nameThatHoldsName = "name_with_tail_of_name"_s;
static constexpr ASCIILiteral nameHeldByName = "tail_of_name"_s;
static constexpr ASCIILiteral textHeldByName = "of_name"_s;
static constexpr std::array<ASCIILiteral, 3> namesThatMeetOtherTexts { nameWithinText, nameThatHoldsName, nameHeldByName };
static constexpr std::array<ASCIILiteral, 4> piecesOfMadeTexts { "made by"_s, " the walk"_s, ", added to"_s, " twice"_s };
static constexpr ASCIILiteral textThatEndsWithMadeText = "a text made by the walk"_s;
static constexpr std::array<char16_t, 3> wideTextWithTail { 0x3b1, 0x3b2, 0x3b3 };
static constexpr std::array<char16_t, 2> tailOfWideText { 0x3b2, 0x3b3 };
static constexpr std::array<Latin1Character, 4> bytesOfTailOfWideText { 0xb2, 0x03, 0xb3, 0x03 };
static constexpr std::array<char16_t, 5> wideTextWithInnerPart { 0x0102, 0x0304, 0x0506, 0x0708, 0x090a };
static constexpr std::array<char16_t, 2> partAtWholeUnit { 0x0304, 0x0506 };
static constexpr std::array<char16_t, 2> bytesBetweenUnits { 0x0401, 0x0603 };

namespace {

struct ExpectedText {
    String text;
    bool is16Bit;
};

struct TextImage {
    TextImage();

    StringID idOf(StringView);
    PropertyKeyID keyOf(StringView);
    CompiledCode bodyOfReader();
    const ImageText& text() const { return image.image->text(); }
    const ImageNumbers& numbers() const { return *image.built.numbers; }
    ImageStringWord wordOf(StringID id) const { return image.image->section<ImageStringWord>(ImageSection::Strings)[id.index()]; }

    TestBuild build;
    TestImage image;
};

} // anonymous namespace

static String repeatedText(char16_t unit, unsigned length)
{
    StringBuilder text;
    for (unsigned i = 0; i < length; ++i)
        text.append(unit);
    return text.toString();
}

template<typename Unit, size_t length>
static String textOfUnits(const std::array<Unit, length>& units)
{
    return String(std::span<const Unit>(units));
}

static const Vector<ExpectedText>& expectedTexts()
{
    static constexpr std::array<Latin1Character, 4> latin1 { 'c', 'a', 'f', 0xe9 };
    static constexpr std::array<char16_t, 2> greek { 0x3b1, 0x3b2 };
    static constexpr std::array<char16_t, 2> loneSurrogate { 0xd800, 'x' };
    static NeverDestroyed<Vector<ExpectedText>> texts(Vector<ExpectedText> {
        { "plain text"_s, false },
        { String(std::span<const Latin1Character>(latin1)), false },
        { String(std::span<const char16_t>(greek)), true },
        { String(std::span<const char16_t>(loneSurrogate)), true },
        { emptyString(), false },
        { repeatedText('a', 255), false },
        { repeatedText('b', 256), false },
        { repeatedText('c', 300), false },
        { repeatedText(0x3b3, 300), true },
        { textThatHoldsOthers, false },
        { tailOfText, false },
        { innerPartOfText, false },
        { textHeldByNone, false },
        { textThatEndsWithName, false },
        { textAroundName, false },
        { textHeldByName, false },
        { textThatEndsWithMadeText, false },
        { textOfUnits(wideTextWithTail), true },
        { textOfUnits(tailOfWideText), true },
        { textOfUnits(bytesOfTailOfWideText), false },
        { textOfUnits(wideTextWithInnerPart), true },
        { textOfUnits(partAtWholeUnit), true },
        { textOfUnits(bytesBetweenUnits), true },
    });
    return texts.get();
}

static void appendQuoted(StringBuilder& out, StringView text)
{
    out.append('"');
    for (char16_t unit : text.codeUnits()) {
        if (isASCIIPrintable(unit) && unit != '"' && unit != '\\')
            out.append(unit);
        else
            out.append("\\u{"_s, hex(unit, Lowercase), '}');
    }
    out.append('"');
}

static String textProgram()
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\""_s, pathShownInTraces, "\" body=f0\nentry\n"_s);
    text.append("function f0 name=\"main\" kind=Module results=[{type=anything}] {\n  b0():\n"_s);
    text.append("    v0 = Builtin builtin=GlobalThis\n"_s);
    text.append("    v1 = PropGet v0 key=\""_s, nameOfFreeVariable, "\" [isFreeName]\n"_s);
    text.append("    v2 = NewClosure function=f1\n"_s);
    text.append("    v3 = Call v1, -, v2\n"_s);
    text.append("    Return\n}\n"_s);
    text.append("function f1 name=\"reader\" results=[{type=anything}] {\n  b0():\n"_s);
    text.append("    v0 = Builtin builtin=GlobalThis\n"_s);
    text.append("    v1 = PropGet v0 key=\""_s, nameOfFreeVariable, "\" [isFreeName]\n"_s);
    text.append("    v2 = String string=\""_s, textReadThroughHeader, "\"\n"_s);
    text.append("    v3 = Call v1, -, v2\n"_s);
    unsigned nextValue = 4;
    for (const ExpectedText& expected : expectedTexts()) {
        unsigned constant = nextValue++;
        text.append("    v"_s, constant, " = String string="_s);
        appendQuoted(text, expected.text);
        text.append("\n    v"_s, nextValue++, " = Call v1, -, v"_s, constant, '\n');
    }
    for (ASCIILiteral name : namesThatMeetOtherTexts)
        text.append("    v"_s, nextValue++, " = PropGet v0 key=\""_s, name, "\" [isFreeName]\n"_s);
    text.append("    Return\n}\n"_s);
    text.append("function f2 name=\"unreached\" results=[{type=anything}] {\n  b0():\n"_s);
    text.append("    v0 = String string=\""_s, textInUnreachedFunction, "\"\n"_s);
    text.append("    Return v0\n}\n"_s);
    text.append(runtimeFunctionsText(3));
    return text.toString();
}

TextImage::TextImage()
    : build(textProgram(), { })
    , image(builtImage(build, bodies(bodyOfReader())))
{
}

StringID TextImage::idOf(StringView text)
{
    StringID id = build.program().strings.find(Typed::encodeProgramString(text).span());
    AOT_TEST_CHECK(id);
    return id;
}

PropertyKeyID TextImage::keyOf(StringView text)
{
    std::optional<PropertyKeyID> key = build.program().types.find(Typed::PropertyKey { .kind = PropertyKeyKind::Text, .text = idOf(text) });
    AOT_TEST_CHECK(key);
    return *key;
}

CompiledCode TextImage::bodyOfReader()
{
    MentionList mentions;
    mentions.add(NumberSpace::Name, keyOf(nameOfFreeVariable));
    for (ASCIILiteral name : namesThatMeetOtherTexts)
        mentions.add(NumberSpace::Name, keyOf(name));
    mentions.add(NumberSpace::String, idOf(textReadThroughHeader));
    for (const ExpectedText& expected : expectedTexts()) {
        if (!expected.text.isEmpty())
            mentions.add(NumberSpace::StringCell, idOf(expected.text));
    }
    Typed::TypeArena arena(build.program().types, Typed::ArenaKeeps::UntilReset);
    StringID made;
    for (ASCIILiteral piece : piecesOfMadeTexts) {
        StringID text = arena.internText(byteCast<uint8_t>(piece.span8()));
        if (!made) {
            made = text;
            continue;
        }
        made = arena.internJoinedTexts(made, text);
        mentions.add(NumberSpace::StringCell, made);
    }
    CompiledCode code = plainBody(1);
    mentions.moveTo(code.body);
    copyMadeTextsMentionedBy(code.body, arena);
    return code;
}

static TextImage& textImage()
{
    static NeverDestroyed<TextImage> shared;
    return shared.get();
}

static String freshCopyOf(const String& text)
{
    if (text.is8Bit())
        return String(text.span8());
    return String(text.span16());
}

static void testIdentifiers()
{
    TextImage& image = textImage();
    const ImageText& text = image.text();
    const IdentifierNumbering& numbering = image.numbers().identifiers();
    AOT_TEST_CHECK(text.numberOfIdentifiers() == numbering.count());
    AOT_TEST_CHECK(!text.identifier(0).text());

    StringID name = image.idOf(nameOfFreeVariable);
    std::optional<uint32_t> number = numbering.numberOfText(name);
    AOT_TEST_CHECK(number);
    AOT_TEST_CHECK(number == numbering.numberOf(image.keyOf(nameOfFreeVariable)));
    AOT_TEST_CHECK(image.wordOf(name).hasIdentifierNumber() && image.wordOf(name).number() == *number);
    AOT_TEST_CHECK(text.isAtom(name));
    const ImageIdentifierWord& identifier = text.identifier(*number);
    AOT_TEST_CHECK(identifier.text() == name);
    AOT_TEST_CHECK(identifier.kind() == PropertyKeyKind::Text);
    AOT_TEST_CHECK(identifier.nameClass() == NameClass::Ordinary);
    AOT_TEST_CHECK(text.hash(name) == String(nameOfFreeVariable).impl()->hash());
    AOT_TEST_CHECK(text.view(name) == nameOfFreeVariable);

    AOT_TEST_CHECK(!image.wordOf(image.idOf("plain text"_s)).hasIdentifierNumber());
    for (uint32_t other = 1; other < text.numberOfIdentifiers(); ++other) {
        const NumberedIdentifier& expected = numbering.at(other);
        AOT_TEST_CHECK(text.identifier(other).text() == expected.text);
        if (expected.key)
            AOT_TEST_CHECK(text.identifier(other).kind() == image.build.program().at(expected.key).kind);
    }
}

static void testStringOnlyShownHasNoHeader()
{
    TextImage& image = textImage();
    constexpr ASCIILiteral pathOfModule = pathShownInTraces;
    constexpr ASCIILiteral textOfConstant = "plain text"_s;
    StringID path = image.idOf(pathOfModule);
    StringID constant = image.idOf(textOfConstant);
    StringID readByCode = image.idOf(textReadThroughHeader);
    const ImageNumbers& numbers = image.numbers();
    AOT_TEST_CHECK(numbers.namedStrings().get(path.index()) && numbers.stringsOnlyShown().get(path.index()));
    AOT_TEST_CHECK(numbers.namedStrings().get(readByCode.index()) && !numbers.stringsOnlyShown().get(readByCode.index()));
    EncodedText encoded = encodeText({ image.build.program(), numbers }, image.build.context());
    std::span<const ImageString> records = spanReinterpretCast<const ImageString>(encoded.strings.span());
    AOT_TEST_CHECK(records[readByCode.index()].hasHeader() && !records[readByCode.index()].hasListedHash());
    AOT_TEST_CHECK(records[path.index()].hasText() && !records[path.index()].hasHeader());
    AOT_TEST_CHECK(!records[constant.index()].hasHeader() && records[constant.index()].hasListedHash() && !records[path.index()].hasListedHash());

    const ImageText& text = image.text();
    unsigned hash = String(pathOfModule).impl()->hash();
    AOT_TEST_CHECK(text.hasText(path) && text.view(path) == pathOfModule && text.hash(path) == hash);
    String created = text.createString(path);
    AOT_TEST_CHECK(created == pathOfModule && created.impl()->hash() == hash);
    AOT_TEST_CHECK(text.createString(path).impl() != created.impl());
    StringID name = image.idOf(nameOfFreeVariable);
    AOT_TEST_CHECK(text.createString(name).impl() == text.createString(name).impl());
    AOT_TEST_CHECK(text.createString(readByCode) == textReadThroughHeader && text.createString(readByCode).impl() == text.createString(readByCode).impl());
    AOT_TEST_CHECK(text.hasListedHash(constant) && !text.hasListedHash(path) && !text.hasListedHash(name) && !text.hasListedHash(readByCode));
    AOT_TEST_CHECK(text.view(constant) == textOfConstant && text.hash(constant) == String(textOfConstant).impl()->hash());
    AOT_TEST_CHECK(text.createString(constant) == textOfConstant && text.createString(constant).impl() != text.createString(constant).impl());
    StringID publicPath = image.idOf("main.js"_s);
    AOT_TEST_CHECK(text.createString(publicPath) == "main.js"_s && text.createString(publicPath).impl() != text.createString(publicPath).impl());
    AOT_TEST_CHECK(image.image.image->containsCharactersOf(*created.impl()));
    String atom = text.createAtom(path);
    AOT_TEST_CHECK(atom == pathOfModule && atom.impl()->isAtom());
}

static std::span<const uint8_t> bytesOfTextInImage(StringView text)
{
    TextImage& image = textImage();
    StringView view = image.text().view(image.idOf(text));
    AOT_TEST_CHECK(view == text);
    return view.is8Bit() ? asBytes(view.span8()) : asBytes(view.span16());
}

static bool textLiesWithin(StringView part, StringView whole, size_t offsetInBytes)
{
    return bytesOfTextInImage(part).data() == bytesOfTextInImage(whole).data() + offsetInBytes;
}

static bool textsLieApart(StringView first, StringView second)
{
    std::span<const uint8_t> bytesOfFirst = bytesOfTextInImage(first);
    std::span<const uint8_t> bytesOfSecond = bytesOfTextInImage(second);
    return bytesOfFirst.data() + bytesOfFirst.size() <= bytesOfSecond.data() || bytesOfSecond.data() + bytesOfSecond.size() <= bytesOfFirst.data();
}

static void testTextsThatAddToOneAnotherLieAtFrontOfLongest()
{
    String first = makeString(piecesOfMadeTexts[0], piecesOfMadeTexts[1]);
    String second = makeString(first, piecesOfMadeTexts[2]);
    String third = makeString(second, piecesOfMadeTexts[3]);
    AOT_TEST_CHECK(textLiesWithin(first, third, 0) && textLiesWithin(second, third, 0));
    AOT_TEST_CHECK(textsLieApart(textThatEndsWithMadeText, third));
    TextImage& image = textImage();
    for (const String& text : { first, second, third }) {
        AOT_TEST_CHECK(image.numbers().stringConstants().numberOf(image.idOf(text)));
        AOT_TEST_CHECK(image.text().hash(image.idOf(text)) == freshCopyOf(text).impl()->hash());
    }
}

namespace {

class ForgedImage {
public:
    void setSection(ImageSection section, std::span<const uint8_t> bytes) { m_sections[static_cast<size_t>(section)] = Vector<uint8_t>(bytes); }
    const Image& create();

private:
    std::array<Vector<uint8_t>, numberOfImageSections> m_sections;
    AlignedBytes m_bytes;
    std::unique_ptr<Image> m_image;
};

} // anonymous namespace

const Image& ForgedImage::create()
{
    ImageHeader header { };
    size_t size = sizeof(ImageHeader);
    for (unsigned section = 0; section < numberOfImageSections; ++section) {
        size = WTF::roundUpToMultipleOf<imageSectionAlignment>(size);
        header.sections[section] = { static_cast<uint32_t>(size), static_cast<uint32_t>(m_sections[section].size()) };
        size += m_sections[section].size();
    }
    Vector<uint8_t> bytes(FillWith { }, size, 0);
    memcpySpan(bytes.mutableSpan(), asByteSpan(header));
    for (unsigned section = 0; section < numberOfImageSections; ++section)
        memcpySpan(bytes.mutableSpan().subspan(header.sections[section].offset), m_sections[section].span());
    m_bytes = AlignedBytes(bytes.span());
#if !ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    ImageSectionRange stringHeaders = header.sections[static_cast<size_t>(ImageSection::StringHeaders)];
    for (ImageStringHeader& stringHeader : spanReinterpretCast<ImageStringHeader>(m_bytes.span().subspan(stringHeaders.offset, stringHeaders.size)))
        stringHeader.characters += header.sections[static_cast<size_t>(ImageSection::Text)].offset;
    relocateImage(m_bytes.span(), std::bit_cast<uintptr_t>(m_bytes.span().data()));
#endif
    m_image = Image::create(std::as_const(m_bytes).span(), m_bytes.span().data());
    return *m_image;
}

#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
static void testHeadersAreCreatedAtFirstUse()
{
    static constexpr std::array<Latin1Character, 8> characters { 'o', 'n', 'e', 't', 'w', 'o', 'x', 'y' };
    static constexpr std::array<char16_t, 2> wideCharacters { 0x3b1, 0x3b2 };
    Vector<uint8_t> text;
    text.append(asBytes(std::span(characters)));
    text.append(asBytes(std::span(wideCharacters)));
    auto computeHash = [](auto span) { return StringHasher::computeHashAndMaskTop8Bits(span); };
    std::span<const Latin1Character> one = std::span(characters).first(3);
    std::span<const Latin1Character> two = std::span(characters).subspan(3, 3);
    auto rangeWord = [](uint32_t offset, size_t length) { return offset << ImageString::lengthBits | static_cast<uint32_t>(length); };
    std::array<ImageStringRecord, 4> stringHeaders { {
        { },
        ImageStringRecord::create(rangeWord(0, one.size()), computeHash(one), false, true),
        ImageStringRecord::create(0u << ImageString::lengthBits, computeHash(two), false, false),
        ImageStringRecord::create(rangeWord(characters.size(), wideCharacters.size()), computeHash(std::span<const char16_t>(wideCharacters)), true, false),
    } };
    std::array<uint32_t, 1> listedStrings { rangeWord(3, two.size()) };

    ForgedImage forged;
    forged.setSection(ImageSection::LongStrings, asBytes(std::span(listedStrings)));
    forged.setSection(ImageSection::StringHeaders, asBytes(std::span(stringHeaders)));
    forged.setSection(ImageSection::Text, text.span());
    const Image& image = forged.create();
    const ImageText& imageText = image.text();

    AOT_TEST_CHECK(imageText.stringRecords().size() == stringHeaders.size() && imageText.lazyHeadersCapacity() == (stringHeaders.size() + 1) * sizeof(LazyStringHeader));
    AOT_TEST_CHECK(imageText.lazyHeadersSize() == sizeof(LazyStringHeader));
    for (uint32_t number = 0; number < stringHeaders.size(); ++number)
        AOT_TEST_CHECK(!imageText.tryGetLazyHeader(number) && !imageText.lazyHeaderOffsets()[number]);
    AOT_TEST_CHECK(imageText.recordView(0).isEmpty() && imageText.recordView(1) == "one"_s && imageText.recordView(2) == "two"_s);
    AOT_TEST_CHECK(imageText.recordView(3) == StringView(std::span<const char16_t>(wideCharacters)));
    AOT_TEST_CHECK(imageText.lazyHeadersSize() == sizeof(LazyStringHeader));

    StringImpl& twoHeader = imageText.ensureLazyHeader(2);
    StringImpl& wideHeader = imageText.ensureLazyHeader(3);
    AOT_TEST_CHECK(std::bit_cast<uint8_t*>(&twoHeader) == imageText.lazyHeaders() + sizeof(LazyStringHeader) && std::bit_cast<uint8_t*>(&wideHeader) == imageText.lazyHeaders() + 2 * sizeof(LazyStringHeader));
    AOT_TEST_CHECK(imageText.lazyHeaderOffsets()[2] == sizeof(LazyStringHeader) && imageText.lazyHeaderOffsets()[3] == 2 * sizeof(LazyStringHeader) && !imageText.lazyHeaderOffsets()[1]);
    AOT_TEST_CHECK(&imageText.ensureLazyHeader(2) == &twoHeader && imageText.tryGetLazyHeader(2) == &twoHeader && !imageText.tryGetLazyHeader(1));
    AOT_TEST_CHECK(imageText.lazyHeadersSize() == 3 * sizeof(LazyStringHeader));

    AOT_TEST_CHECK(twoHeader.isStatic() && !twoHeader.isAtom() && twoHeader.is8Bit() && StringView(twoHeader) == "two"_s && twoHeader.existingHash() == computeHash(two));
    AOT_TEST_CHECK(wideHeader.isStatic() && !wideHeader.is8Bit() && StringView(wideHeader) == StringView(std::span<const char16_t>(wideCharacters)));
    AOT_TEST_CHECK(image.containsCharactersOf(twoHeader) && image.containsCharactersOf(wideHeader) && Image::isBackedByStaticData(twoHeader));
    AOT_TEST_CHECK(imageText.lazyHeaderNumber(twoHeader) == 2u && imageText.lazyHeaderNumber(wideHeader) == 3u);
    String other = String("two"_s);
    AOT_TEST_CHECK(!imageText.lazyHeaderNumber(*other.impl()));

    std::array<StringImpl*, 4> seen { };
    Vector<Ref<Thread>> threads;
    for (StringImpl*& result : seen) {
        threads.append(Thread::create("image-text-tests"_s, [&imageText, &result] {
            result = &imageText.ensureLazyHeader(1);
        }));
    }
    for (Ref<Thread>& thread : threads)
        thread->waitForCompletion();
    for (StringImpl* result : seen)
        AOT_TEST_CHECK(result == seen[0] && result->isAtom() && StringView(*result) == "one"_s);
    AOT_TEST_CHECK(imageText.lazyHeadersSize() == 4 * sizeof(LazyStringHeader));
}

static void testImageHoldsNoAddress()
{
    TextImage& image = textImage();
    std::span<const uint8_t> bytes = std::as_const(image.image.bytes).span();
    AlignedBytes copy(bytes);
    std::span<const uint8_t> bytesOfCopy = std::as_const(copy).span();
    AOT_TEST_CHECK(bytesOfCopy.data() != bytes.data());
    std::unique_ptr<Image> imageOfCopy = Image::create(bytesOfCopy, bytesOfCopy.data() + image.image.image->header().codeOffset);
    for (const ExpectedText& expected : expectedTexts()) {
        StringID id = image.idOf(expected.text);
        String created = imageOfCopy->text().createString(id);
        AOT_TEST_CHECK(created == expected.text && imageOfCopy->text().view(id) == expected.text && imageOfCopy->text().hash(id) == image.text().hash(id));
        if (!expected.text.isEmpty())
            AOT_TEST_CHECK(imageOfCopy->containsCharactersOf(*created.impl()) && !image.image.image->containsCharactersOf(*created.impl()));
    }
    AOT_TEST_CHECK(equalSpans(bytes, bytesOfCopy));
}
#endif

std::span<const NativeTest> imageTextTests()
{
    static constexpr NativeTest tests[] = {
        { "image/text/identifiers"_s, testIdentifiers },
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
        { "image/text/headers-are-created-at-first-use"_s, testHeadersAreCreatedAtFirstUse },
        { "image/text/image-holds-no-address"_s, testImageHoldsNoAddress },
#endif
        { "image/text/string-only-shown-has-no-header"_s, testStringOnlyShownHasNoHeader },
        { "image/text/texts-that-add-to-one-another-lie-at-the-front-of-the-longest"_s, testTextsThatAddToOneAnotherLieAtFrontOfLongest },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
