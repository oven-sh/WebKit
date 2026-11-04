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

// The atom of a header stays in the table of its thread for good, so the texts of which the tests make atoms are those without a header: code makes only their cells.
// The body of a module that has code needs its generic entry beside it, so the code is that of a function that is no such body.
CompiledCode TextImage::bodyOfReader()
{
    MentionList mentions;
    mentions.add(NumberSpace::Name, keyOf(nameOfFreeVariable));
    for (ASCIILiteral name : namesThatMeetOtherTexts)
        mentions.add(NumberSpace::Name, keyOf(name));
    mentions.add(NumberSpace::String, idOf(textReadThroughHeader));
    for (const ExpectedText& expected : expectedTexts()) {
        // The VM has the cell of the empty string.
        if (!expected.text.isEmpty())
            mentions.add(NumberSpace::StringCell, idOf(expected.text));
    }
    CompiledCode code = plainBody(1);
    mentions.moveTo(code.body);
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

static void testRoundTrip()
{
    TextImage& image = textImage();
    const ImageText& text = image.text();
    AOT_TEST_CHECK(text.numberOfStrings() == image.build.program().strings.size());
    for (const ExpectedText& expected : expectedTexts()) {
        StringID id = image.idOf(expected.text);
        AOT_TEST_CHECK(text.hasText(id));
        StringView view = text.view(id);
        AOT_TEST_CHECK(view == expected.text);
        AOT_TEST_CHECK(view.length() == expected.text.length());
        AOT_TEST_CHECK(view.is8Bit() == !expected.is16Bit);
        AOT_TEST_CHECK(text.equals(id, expected.text));
        AOT_TEST_CHECK(!text.equals(id, "another text"_s));
    }
}

static void testTextThatNothingNames()
{
    TextImage& image = textImage();
    StringID id = image.idOf(textInUnreachedFunction);
    AOT_TEST_CHECK(!image.text().hasText(id));
    AOT_TEST_CHECK(!image.text().equals(id, textInUnreachedFunction));
}

static void testHashes()
{
    TextImage& image = textImage();
    for (const ExpectedText& expected : expectedTexts()) {
        StringID id = image.idOf(expected.text);
        unsigned hash = freshCopyOf(expected.text).impl()->hash();
        AOT_TEST_CHECK(image.text().hash(id) == hash);
        AOT_TEST_CHECK(image.text().createString(id).impl()->hash() == hash);
    }
}

static void testCreateString()
{
    TextImage& image = textImage();
    for (const ExpectedText& expected : expectedTexts()) {
        String created = image.text().createString(image.idOf(expected.text));
        AOT_TEST_CHECK(created == expected.text);
        AOT_TEST_CHECK(!created.isNull());
        if (expected.text.isEmpty())
            continue;
        AOT_TEST_CHECK(created.impl()->hash() == freshCopyOf(expected.text).impl()->hash());
        AOT_TEST_CHECK(created.is8Bit() == !expected.is16Bit);
        AOT_TEST_CHECK(image.image.image->containsCharactersOf(*created.impl()));
        AOT_TEST_CHECK(!image.image.image->containsCharactersOf(*freshCopyOf(expected.text).impl()));
    }
}

static void testCreateAtom()
{
    TextImage& image = textImage();
    for (const ExpectedText& expected : expectedTexts()) {
        String created = image.text().createAtom(image.idOf(expected.text));
        AOT_TEST_CHECK(created.impl()->isAtom());
        AOT_TEST_CHECK(created == expected.text);
        AOT_TEST_CHECK(AtomString(freshCopyOf(expected.text)).impl() == created.impl());
        if (!expected.text.isEmpty())
            AOT_TEST_CHECK(image.image.image->containsCharactersOf(*created.impl()));
    }
}

static void testCreateAtomThatExists()
{
    TextImage& image = textImage();
    for (const ExpectedText& expected : expectedTexts()) {
        AtomString existing(freshCopyOf(expected.text));
        String created = image.text().createAtom(image.idOf(expected.text));
        AOT_TEST_CHECK(created.impl() == existing.impl());
        if (!expected.text.isEmpty())
            AOT_TEST_CHECK(!image.image.image->containsCharactersOf(*created.impl()));
    }
}

// What a string constant of compiled code is: static, and no atom.
static Vector<String> constantsThatAreNoAtoms()
{
    static StringImpl::StaticStringImpl narrow { "a text of which the program only makes strings"_s };
    static StringImpl::StaticStringImpl wide { u"a wide text αβ of which the program only makes strings" };
    Vector<String> constants { String { static_cast<StringImpl&>(narrow) }, String { static_cast<StringImpl&>(wide) } };
    for (const String& constant : constants)
        AOT_TEST_CHECK(constant.impl()->isStatic() && !constant.impl()->isAtom());
    return constants;
}

static void testAtomOfConstantOutlivesWhoeverAskedForIt()
{
    for (const String& constant : constantsThatAreNoAtoms()) {
        AtomString atom { constant };
        AOT_TEST_CHECK(AtomStringImpl::lookUp(constant.impl()).get() == atom.impl());
    }
}

// A Worker's thread has a table of its own, which ends with the Worker: what the table held goes with it.
static void testAtomTableGivesBackTheAtomsOfConstants()
{
    struct Provider {
        AtomStringTable* atomStringTable() { return &table; }
        AtomStringTable table;
    };
    for (const String& constant : constantsThatAreNoAtoms()) {
        RefPtr<StringImpl> atom;
        {
            Provider provider;
            atom = AtomStringImpl::addWithStringTableProvider(provider, constant.impl());
            AOT_TEST_CHECK(atom->isAtom());
            AOT_TEST_CHECK(atom.get() != constant.impl());
            AOT_TEST_CHECK(atom->refCount() == 2);
        }
        AOT_TEST_CHECK(!atom->isAtom());
        AOT_TEST_CHECK(atom->hasOneRef());
    }
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

static void testStringConstants()
{
    TextImage& image = textImage();
    const ImageText& text = image.text();
    const StringConstantNumbering& constants = image.numbers().stringConstants();
    AOT_TEST_CHECK(text.numberOfStringConstants() == constants.count());
    AOT_TEST_CHECK(image.image.image->programTables().instanceSizes.numberOfStringCells == constants.count());
    for (uint32_t number = 0; number < constants.count(); ++number)
        AOT_TEST_CHECK(text.stringConstant(number) == constants.stringOf(number));

    AOT_TEST_CHECK(constants.numberOf(image.idOf("plain text"_s)));
    AOT_TEST_CHECK(!constants.numberOf(image.idOf(textReadThroughHeader)));
}

static void testEncodedHashes()
{
    TextImage& image = textImage();
    EncodedText encoded = encodeText({ image.build.program(), image.numbers() }, image.build.context());
    auto hashOf = [&](StringID id) -> unsigned {
        const ImageString& record = spanReinterpretCast<const ImageString>(encoded.strings.span())[id.index()];
        RELEASE_ASSERT(record.hasText());
        if (!record.hasIdentifierNumber())
            return record.hashOrIdentifierNumber();
        return spanReinterpretCast<const ImageIdentifier>(encoded.identifiers.span())[record.hashOrIdentifierNumber()].hashOrSymbol();
    };
    for (const ExpectedText& expected : expectedTexts())
        AOT_TEST_CHECK(hashOf(image.idOf(expected.text)) == freshCopyOf(expected.text).impl()->hash());
    AOT_TEST_CHECK(hashOf(image.idOf(textReadThroughHeader)) == String(textReadThroughHeader).impl()->hash());
    AOT_TEST_CHECK(hashOf(image.idOf(nameOfFreeVariable)) == String(nameOfFreeVariable).impl()->hash());
}

// The paths of a module are only shown. A string that code reads has its header. A constant of which only the cell is made gets its header with the cell.
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
    // A header in the image is the same string at every call.
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

static DecodedString decode(StringView text)
{
    return decodeForImage(Typed::encodeProgramString(text).span());
}

static void testDecodeForImage()
{
    for (const ExpectedText& expected : expectedTexts()) {
        DecodedString decoded = decode(expected.text);
        AOT_TEST_CHECK(decoded.is16Bit() == expected.is16Bit);
        AOT_TEST_CHECK(decoded.length() == expected.text.length());
        AOT_TEST_CHECK(decoded.hash == freshCopyOf(expected.text).impl()->hash());
        AOT_TEST_CHECK(decoded.bytes().size() == expected.text.length() * (expected.is16Bit ? sizeof(char16_t) : sizeof(Latin1Character)));
        if (expected.is16Bit)
            AOT_TEST_CHECK(StringView(decoded.units16.span()) == expected.text);
        else
            AOT_TEST_CHECK(StringView(byteCast<Latin1Character>(decoded.units8.span())) == expected.text);
    }
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

static void testSharedTailsAndInnerParts()
{
    AOT_TEST_CHECK(textLiesWithin(tailOfText, textThatHoldsOthers, "a text that "_s.length()));
    AOT_TEST_CHECK(textLiesWithin(innerPartOfText, textThatHoldsOthers, "a text "_s.length()));
    AOT_TEST_CHECK(textsLieApart(textHeldByNone, textThatHoldsOthers));
}

static void testNameIsHeldOnlyByName()
{
    AOT_TEST_CHECK(textsLieApart(nameOfFreeVariable, textThatEndsWithName));
    AOT_TEST_CHECK(textsLieApart(nameWithinText, textAroundName));
    AOT_TEST_CHECK(textLiesWithin(nameHeldByName, nameThatHoldsName, "name_with_"_s.length()));
    AOT_TEST_CHECK(textLiesWithin(textHeldByName, nameThatHoldsName, "name_with_tail_"_s.length()));
}

static void testWidthsDoNotShare()
{
    AOT_TEST_CHECK(textLiesWithin(textOfUnits(tailOfWideText), textOfUnits(wideTextWithTail), sizeof(char16_t)));
    AOT_TEST_CHECK(textsLieApart(textOfUnits(bytesOfTailOfWideText), textOfUnits(wideTextWithTail)));
}

static void testInnerPartStartsAtWholeUnit()
{
    AOT_TEST_CHECK(textLiesWithin(textOfUnits(partAtWholeUnit), textOfUnits(wideTextWithInnerPart), sizeof(char16_t)));
    AOT_TEST_CHECK(textsLieApart(textOfUnits(bytesBetweenUnits), textOfUnits(wideTextWithInnerPart)));
}

namespace {

class ForgedImage {
public:
    // The headers of strings say where in ImageSection::Text their characters are. create() turns that into addresses.
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
    stringHeaders[2].hashAndFlags |= ImageStringRecord::isLiteralStringFlag;
    std::array<uint32_t, 2> listedStrings { rangeWord(3, two.size()), 0x1234 };

    ForgedImage forged;
    forged.setSection(ImageSection::LongStrings, asBytes(std::span(listedStrings)));
    forged.setSection(ImageSection::StringHeaders, asBytes(std::span(stringHeaders)));
    forged.setSection(ImageSection::Text, text.span());
    const Image& image = forged.create();
    const ImageText& imageText = image.text();

    AOT_TEST_CHECK(imageText.stringRecords().size() == stringHeaders.size() && imageText.lazyHeadersCapacity() == (stringHeaders.size() + 1) * sizeof(LazyStringHeader));
    AOT_TEST_CHECK(imageText.lazyHeadersSize() == sizeof(LazyStringHeader));
    AOT_TEST_CHECK(reinterpret_cast<const LazyStringHeaderArena*>(imageText.lazyHeaders())->sizeInBytes == imageText.lazyHeadersCapacity());
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
    AOT_TEST_CHECK(imageText.literalStringWordOfHeader(twoHeader) == 0x1234u && imageText.literalStringWordOfHeader(wideHeader) == 0u);
    String other = String("two"_s);
    AOT_TEST_CHECK(!imageText.lazyHeaderNumber(*other.impl()) && !imageText.literalStringWordOfHeader(*other.impl()));

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

static void testTextPastShortOffsets()
{
    constexpr uint32_t endOfShortOffsets = 1u << (32 - ImageString::lengthBits);
    constexpr uint32_t offsetOf8BitText = endOfShortOffsets + 6;
    constexpr uint32_t offsetOf16BitText = endOfShortOffsets + 32;
    static constexpr std::array<Latin1Character, 3> farText { 'f', 'a', 'r' };
    static constexpr std::array<char16_t, 2> farWideText { 0x3b1, 0x3b2 };
    static constexpr std::array<Latin1Character, 4> nearText { 'n', 'e', 'a', 'r' };
    constexpr uint32_t offsetOfNearText = endOfShortOffsets - nearText.size();

    Vector<uint8_t> text(FillWith { }, endOfShortOffsets + 64, 0);
    memcpySpan(text.mutableSpan().subspan(offsetOfNearText), asBytes(std::span(nearText)));
    memcpySpan(text.mutableSpan().subspan(offsetOf8BitText), asBytes(std::span(farText)));
    memcpySpan(text.mutableSpan().subspan(offsetOf16BitText), asBytes(std::span(farWideText)));

    // Without a header: one word for the text that is near, three for the texts that are far.
    std::array<uint32_t, 1 + 2 * ImageListedString::maxNumberOfWords> listedStrings { };
    AOT_TEST_CHECK(ImageListedString::numberOfWords({ offsetOfNearText, nearText.size() }) == 1 && ImageListedString::numberOfWords({ offsetOf8BitText, farText.size() }) == ImageListedString::maxNumberOfWords);
    ImageListedString::write(std::span(listedStrings), { offsetOfNearText, nearText.size() });
    ImageListedString::write(std::span(listedStrings).subspan(1), { offsetOf8BitText, farText.size() });
    ImageListedString::write(std::span(listedStrings).subspan(1 + ImageListedString::maxNumberOfWords), { offsetOf16BitText, farWideText.size() });
    std::array<ImageStringWord, 6> strings { {
        ImageString { 1u << ImageString::lengthBits, 0 }.inImage(0),
        ImageString { 2u << ImageString::lengthBits, ImageString::is16BitFlag }.inImage(1),
        ImageString { offsetOfNearText << ImageString::lengthBits | static_cast<uint32_t>(nearText.size()), 0 }.inImage(2),
        ImageString { 1u << ImageString::lengthBits, ImageString::hasNoHeaderFlag }.inImage(0),
        ImageString { 1u << ImageString::lengthBits, ImageString::hasNoHeaderFlag }.inImage(1),
        ImageString { 1u << ImageString::lengthBits, ImageString::hasNoHeaderFlag | ImageString::is16BitFlag }.inImage(1 + ImageListedString::maxNumberOfWords),
    } };
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    std::array<ImageStringRecord, 3> stringHeaders { {
        ImageStringRecord::create(1u << ImageString::lengthBits, StringHasher::computeHashAndMaskTop8Bits(std::span<const Latin1Character>(farText)), false, false),
        ImageStringRecord::create((1u + ImageListedString::maxNumberOfWords) << ImageString::lengthBits, StringHasher::computeHashAndMaskTop8Bits(std::span<const char16_t>(farWideText)), true, false),
        ImageStringRecord::create(listedStrings[0], StringHasher::computeHashAndMaskTop8Bits(std::span<const Latin1Character>(nearText)), false, false),
#else
    std::array<ImageStringHeader, 3> stringHeaders { {
        ImageStringHeader::create(offsetOf8BitText, farText.size(), StringHasher::computeHashAndMaskTop8Bits(std::span<const Latin1Character>(farText)), false, StringImpl::StringNormal),
        ImageStringHeader::create(offsetOf16BitText, farWideText.size(), StringHasher::computeHashAndMaskTop8Bits(std::span<const char16_t>(farWideText)), true, StringImpl::StringNormal),
        ImageStringHeader::create(offsetOfNearText, nearText.size(), StringHasher::computeHashAndMaskTop8Bits(std::span<const Latin1Character>(nearText)), false, StringImpl::StringNormal),
#endif
    } };

    ForgedImage forged;
    forged.setSection(ImageSection::Strings, asBytes(std::span(strings)));
    forged.setSection(ImageSection::StringHeaders, asBytes(std::span(stringHeaders)));
    forged.setSection(ImageSection::LongStrings, asBytes(std::span(listedStrings)));
    forged.setSection(ImageSection::Text, text.span());
    const ImageText& imageText = forged.create().text();

    AOT_TEST_CHECK(imageText.view(StringID(0u)) == "far"_s);
    AOT_TEST_CHECK(imageText.view(StringID(1u)) == StringView(std::span<const char16_t>(farWideText)));
    AOT_TEST_CHECK(imageText.view(StringID(2u)) == "near"_s);
    AOT_TEST_CHECK(imageText.createString(StringID(0u)).impl()->hash() == String("far"_s).impl()->hash());
    {
        // A table holds the atom of a header until the table ends, and the characters of this atom end with the forged image.
        AtomStringTable table;
        AtomStringTable* tableOfThread = Thread::currentSingleton().setCurrentAtomStringTable(&table);
        AOT_TEST_CHECK(imageText.createAtom(StringID(1u)) == String(std::span<const char16_t>(farWideText)));
        Thread::currentSingleton().setCurrentAtomStringTable(tableOfThread);
    }
    AOT_TEST_CHECK(imageText.view(StringID(3u)) == "near"_s && imageText.view(StringID(4u)) == "far"_s);
    AOT_TEST_CHECK(imageText.view(StringID(5u)) == StringView(std::span<const char16_t>(farWideText)));
    AOT_TEST_CHECK(imageText.createString(StringID(4u)) == "far"_s && imageText.hash(StringID(4u)) == String("far"_s).impl()->hash());
}

std::span<const NativeTest> imageTextTests()
{
    static constexpr NativeTest tests[] = {
        { "image/text/round-trip"_s, testRoundTrip },
        { "image/text/text-that-nothing-names"_s, testTextThatNothingNames },
        { "image/text/hashes"_s, testHashes },
        { "image/text/create-string"_s, testCreateString },
        { "image/text/create-atom"_s, testCreateAtom },
        { "image/text/create-atom-that-exists"_s, testCreateAtomThatExists },
        { "image/text/atom-of-constant-outlives-whoever-asked-for-it"_s, testAtomOfConstantOutlivesWhoeverAskedForIt },
        { "image/text/atom-table-gives-back-the-atoms-of-constants"_s, testAtomTableGivesBackTheAtomsOfConstants },
        { "image/text/identifiers"_s, testIdentifiers },
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
        { "image/text/headers-are-created-at-first-use"_s, testHeadersAreCreatedAtFirstUse },
        { "image/text/image-holds-no-address"_s, testImageHoldsNoAddress },
#endif
        { "image/text/string-constants"_s, testStringConstants },
        { "image/text/encoded-hashes"_s, testEncodedHashes },
        { "image/text/string-only-shown-has-no-header"_s, testStringOnlyShownHasNoHeader },
        { "image/text/decode-for-image"_s, testDecodeForImage },
        { "image/text/shared-tails-and-inner-parts"_s, testSharedTailsAndInnerParts },
        { "image/text/name-is-held-only-by-name"_s, testNameIsHeldOnlyByName },
        { "image/text/widths-do-not-share"_s, testWidthsDoNotShare },
        { "image/text/inner-part-starts-at-whole-unit"_s, testInnerPartStartsAtWholeUnit },
        { "image/text/text-past-short-offsets"_s, testTextPastShortOffsets },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
