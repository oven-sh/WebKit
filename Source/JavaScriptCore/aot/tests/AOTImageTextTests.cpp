/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTIdentifierNumbering.h"
#include "AOTImageTestBodies.h"
#include "AOTImageText.h"
#include "AOTTextEncoder.h"
#include <array>
#include <cstring>
#include <initializer_list>
#include <memory>
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

static constexpr ASCIILiteral textCreatedRepeatedly = "created repeatedly"_s;
static constexpr ASCIILiteral textInUnreachedFunction = "never used"_s;
static constexpr ASCIILiteral nameOfFreeVariable = "sink"_s;
static constexpr ASCIILiteral pathShownInTraces = "shown/main.js"_s;

namespace {

struct ExpectedText {
    String text;
    bool is16Bit;
};

struct TextImage {
    TextImage();

    StringID idOf(StringView) const;
    const ImageText& text() const { return image.image->text(); }

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
    unsigned nextValue = 2;
    for (const ExpectedText& expected : expectedTexts()) {
        unsigned constant = nextValue++;
        text.append("    v"_s, constant, " = String string="_s);
        appendQuoted(text, expected.text);
        text.append("\n    v"_s, nextValue++, " = Call v1, -, v"_s, constant, '\n');
    }
    unsigned closure = nextValue++;
    text.append("    v"_s, closure, " = NewClosure function=f1\n"_s);
    text.append("    v"_s, nextValue, " = Call v1, -, v"_s, closure, '\n');
    text.append("    Return\n}\n"_s);
    text.append("function f1 name=\"repeated\" results=[{type=anything}] {\n  b0():\n"_s);
    text.append("    v0 = Builtin builtin=GlobalThis\n"_s);
    text.append("    v1 = PropGet v0 key=\""_s, nameOfFreeVariable, "\" [isFreeName]\n"_s);
    text.append("    v2 = String string=\""_s, textCreatedRepeatedly, "\"\n"_s);
    text.append("    v3 = Call v1, -, v2\n"_s);
    text.append("    Return\n}\n"_s);
    text.append("function f2 name=\"unreached\" results=[{type=anything}] {\n  b0():\n"_s);
    text.append("    v0 = String string=\""_s, textInUnreachedFunction, "\"\n"_s);
    text.append("    Return v0\n}\n"_s);
    text.append(runtimeFunctionsText(3));
    return text.toString();
}

TextImage::TextImage()
    : build(textProgram(), { })
    , image(builtImage(build, bodies(plainBody(0))))
{
}

StringID TextImage::idOf(StringView text) const
{
    StringID id = build.program().strings.find(Typed::encodeProgramString(text).span());
    AOT_TEST_CHECK(id);
    return id;
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
    static StringImpl::StaticStringImpl wide { u"a wide text \u03b1\u03b2 of which the program only makes strings" };
    Vector<String> constants { String { static_cast<StringImpl&>(narrow) }, String { static_cast<StringImpl&>(wide) } };
    for (const String& constant : constants)
        AOT_TEST_CHECK(constant.impl()->isStatic() && !constant.impl()->isAtom());
    return constants;
}

// Native code makes an atom of a String and goes on with the bare pointer: it counts on the String to keep the atom alive, because an ordinary string becomes the atom itself.
// The atom of a static string is a block of its own, and nothing but the one who asked held it: expect(..).toHaveProperty() looked a freed name up. Now the table holds it.
static void testAtomOfConstantOutlivesWhoeverAskedForIt()
{
    for (const String& constant : constantsThatAreNoAtoms()) {
        StringImpl* atom = AtomString(constant).impl();
        AOT_TEST_CHECK(AtomStringImpl::lookUp(constant.impl()).get() == atom);
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
    const IdentifierNumbering& numbering = image.build.context().identifierNumbering();
    AOT_TEST_CHECK(text.numberOfIdentifiers() == numbering.count());
    AOT_TEST_CHECK(!text.identifier(0).text());

    StringID name = image.idOf(nameOfFreeVariable);
    std::optional<uint32_t> number = text.identifierNumber(name);
    AOT_TEST_CHECK(number);
    AOT_TEST_CHECK(number == numbering.numberOfText(name));
    AOT_TEST_CHECK(text.isAtom(name));
    const ImageIdentifierWord& identifier = text.identifier(*number);
    AOT_TEST_CHECK(identifier.text() == name);
    AOT_TEST_CHECK(identifier.kind() == PropertyKeyKind::Text);
    AOT_TEST_CHECK(identifier.nameClass() == NameClass::Ordinary);
    AOT_TEST_CHECK(text.hash(name) == String(nameOfFreeVariable).impl()->hash());
    AOT_TEST_CHECK(text.view(name) == nameOfFreeVariable);

    AOT_TEST_CHECK(!text.identifierNumber(image.idOf("plain text"_s)));
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
    const StringConstantNumbering& constants = image.build.plan().stringConstants;
    AOT_TEST_CHECK(text.numberOfStringConstants() == constants.count());
    AOT_TEST_CHECK(image.image.image->programTables().instanceSizes.numberOfStringCells == constants.count());
    for (uint32_t number = 0; number < constants.count(); ++number)
        AOT_TEST_CHECK(text.stringConstant(number) == constants.stringOf(number));

    AOT_TEST_CHECK(constants.numberOf(image.idOf(textCreatedRepeatedly)));
    AOT_TEST_CHECK(!constants.numberOf(image.idOf("plain text"_s)));
}

static void testEncodedHashes()
{
    TextImage& image = textImage();
    TestBuild& build = image.build;
    BitVector namedByNoTable;
    EncodedText encoded = encodeText({ build.program(), build.plan().uses.strings, build.context().identifierNumbering(), build.plan().stringConstants, namedByNoTable }, build.context());
    for (const ExpectedText& expected : expectedTexts())
        AOT_TEST_CHECK(encoded.hashOf(image.idOf(expected.text)) == freshCopyOf(expected.text).impl()->hash());
    AOT_TEST_CHECK(encoded.hashOf(image.idOf(nameOfFreeVariable)) == String(nameOfFreeVariable).impl()->hash());
}

// The paths of a module are only shown. A string that a table holds as a value has its header. A constant of which only the cell is made, whoever else shows it, gets its header with the cell.
static void testStringOnlyShownHasNoHeader()
{
    TextImage& image = textImage();
    TestBuild& build = image.build;
    constexpr ASCIILiteral pathOfModule = pathShownInTraces;
    StringID path = image.idOf(pathOfModule);
    StringID constant = image.idOf(textCreatedRepeatedly);
    BitVector named;
    named.set(path.index());
    named.set(constant.index());
    auto encodeRecords = [&](const BitVector* onlyShown) {
        EncodedText encoded = encodeText({ build.program(), build.plan().uses.strings, build.context().identifierNumbering(), build.plan().stringConstants, named, onlyShown }, build.context());
        return Vector<ImageString>(spanReinterpretCast<const ImageString>(encoded.strings.span()));
    };
    Vector<ImageString> allWithHeaders = encodeRecords(nullptr);
    AOT_TEST_CHECK(allWithHeaders[path.index()].hasHeader() && allWithHeaders[constant.index()].hasHeader());
    Vector<ImageString> records = encodeRecords(&named);
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
    AOT_TEST_CHECK(text.hasListedHash(constant) && !text.hasListedHash(path) && !text.hasListedHash(name));
    AOT_TEST_CHECK(text.view(constant) == textCreatedRepeatedly && text.hash(constant) == String(textCreatedRepeatedly).impl()->hash());
    AOT_TEST_CHECK(text.createString(constant) == textCreatedRepeatedly && text.createString(constant).impl() != text.createString(constant).impl());
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

namespace {

struct SharedTexts {
    SharedTexts(std::initializer_list<String> texts, std::initializer_list<unsigned> names);

    bool isHeldBy(unsigned part, unsigned whole, uint32_t gap) const { return sharing.holders[part] == whole && sharing.gaps[part] == gap; }
    bool holdsItself(unsigned index) const { return isHeldBy(index, index, 0); }

    TextSharing sharing;
};

} // anonymous namespace

SharedTexts::SharedTexts(std::initializer_list<String> texts, std::initializer_list<unsigned> names)
{
    Vector<DecodedString> decoded;
    BitVector hasText;
    for (const String& text : texts) {
        hasText.set(decoded.size());
        decoded.append(decode(text));
    }
    BitVector isName;
    for (unsigned name : names)
        isName.set(name);
    BuildContext context(wireOptions());
    sharing = shareText(decoded.span(), hasText, isName, context);
}

static void testSharedTailsAndInnerParts()
{
    SharedTexts shared({ "hello world"_s, "world"_s, "lo wo"_s, "other"_s }, { });
    AOT_TEST_CHECK(shared.holdsItself(0));
    AOT_TEST_CHECK(shared.isHeldBy(1, 0, 0));
    AOT_TEST_CHECK(shared.isHeldBy(2, 0, 3));
    AOT_TEST_CHECK(shared.holdsItself(3));
}

static void testNameIsHeldOnlyByName()
{
    SharedTexts shared({ "kitchen sink"_s, "sink"_s, "ink"_s, "chen s"_s, "inner name"_s, "an inner name here"_s }, { 1, 4 });
    AOT_TEST_CHECK(shared.holdsItself(1));
    AOT_TEST_CHECK(shared.holdsItself(4));
    AOT_TEST_CHECK(shared.sharing.holders[2] != 2);
    AOT_TEST_CHECK(shared.isHeldBy(3, 0, 3));

    SharedTexts names({ "kitchen sink"_s, "sink"_s }, { 0, 1 });
    AOT_TEST_CHECK(names.isHeldBy(1, 0, 0));
}

static void testWidthsDoNotShare()
{
    static constexpr std::array<char16_t, 3> whole { 0x3b1, 0x3b2, 0x3b3 };
    static constexpr std::array<char16_t, 2> tail { 0x3b2, 0x3b3 };
    static constexpr std::array<Latin1Character, 4> bytesOfTail { 0xb2, 0x03, 0xb3, 0x03 };
    SharedTexts shared({ String(std::span<const char16_t>(whole)), String(std::span<const char16_t>(tail)), String(std::span<const Latin1Character>(bytesOfTail)) }, { });
    AOT_TEST_CHECK(shared.holdsItself(0));
    AOT_TEST_CHECK(shared.isHeldBy(1, 0, 0));
    AOT_TEST_CHECK(shared.holdsItself(2));
}

static void testInnerPartStartsAtWholeUnit()
{
    static constexpr std::array<char16_t, 5> whole { 0x0102, 0x0304, 0x0506, 0x0708, 0x090a };
    static constexpr std::array<char16_t, 2> atWholeUnit { 0x0304, 0x0506 };
    static constexpr std::array<char16_t, 2> betweenUnits { 0x0401, 0x0603 };
    SharedTexts shared({ String(std::span<const char16_t>(whole)), String(std::span<const char16_t>(atWholeUnit)), String(std::span<const char16_t>(betweenUnits)) }, { });
    AOT_TEST_CHECK(shared.isHeldBy(1, 0, 2 * sizeof(char16_t)));
    AOT_TEST_CHECK(shared.holdsItself(2));
}

static void testSharedTextInImage()
{
    static constexpr ASCIILiteral program = "module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry\n"
        "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
        "  b0():\n"
        "    v0 = Builtin builtin=GlobalThis\n"
        "    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"
        "    v2 = String string=\"hello world\"\n"
        "    v3 = String string=\"world\"\n"
        "    v4 = Call v1, -, v2, v3\n"
        "    Return\n"
        "}\n"_s;
    TestBuild build(makeString(program, runtimeFunctionsText(1)), { });
    TestImage image = builtImage(build, bodies(plainBody(0)));
    auto viewOf = [&](ASCIILiteral text) {
        return image.image->text().view(build.program().strings.find(byteCast<uint8_t>(text.span())));
    };
    AOT_TEST_CHECK(viewOf("hello world"_s) == "hello world"_s);
    AOT_TEST_CHECK(viewOf("world"_s) == "world"_s);
    AOT_TEST_CHECK(viewOf("world"_s).span8().data() == viewOf("hello world"_s).span8().data() + strlen("hello "));
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
    AOT_TEST_CHECK(imageText.createAtom(StringID(1u)) == String(std::span<const char16_t>(farWideText)));
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
        { "image/text/shared-text-in-image"_s, testSharedTextInImage },
        { "image/text/text-past-short-offsets"_s, testTextPastShortOffsets },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
