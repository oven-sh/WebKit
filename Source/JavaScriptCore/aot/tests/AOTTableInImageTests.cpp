/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTTableInImage.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include <optional>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

std::span<const NativeTest> tableInImageTests();

namespace {

class EncodedTable {
public:
    EncodedTable(Vector<String>&& texts, std::span<const uint64_t> values = { }, std::optional<unsigned> hashOfEveryKey = std::nullopt)
        : m_texts(WTF::move(texts))
    {
        Vector<TableInImage::Key> keys;
        for (uint32_t number = 0; number < m_texts.size(); ++number)
            keys.append({ number, hashOfEveryKey.value_or(m_texts[number].impl()->hash()) });
        std::optional<Vector<uint8_t>> bytes = TableInImage::tryEncode(keys.span(), values);
        AOT_TEST_CHECK(bytes);
        m_words.grow((bytes->size() + sizeof(uint64_t) - 1) / sizeof(uint64_t));
        memcpy(m_words.mutableSpan().data(), bytes->span().data(), bytes->size());
        m_size = bytes->size();
    }

    const TableInImage& table() const { return *reinterpret_cast<const TableInImage*>(m_words.span().data()); }
    size_t size() const { return m_size; }
    const Vector<String>& texts() const { return m_texts; }

    std::optional<uint32_t> find(StringView key, unsigned hash) const
    {
        return table().findWithHash(hash, [&](uint32_t entry) {
            AOT_TEST_CHECK(entry < table().count);
            return m_texts[table().strings()[entry]] == key;
        });
    }

private:
    Vector<String> m_texts;
    Vector<uint64_t> m_words;
    size_t m_size { 0 };
};

} // anonymous namespace

static String in16Bits(const String& text)
{
    Vector<char16_t> characters;
    for (unsigned i = 0; i < text.length(); ++i)
        characters.append(text[i]);
    String result = String(characters.span());
    AOT_TEST_CHECK(!result.is8Bit() && result == text);
    return result;
}

static Vector<String> numberedTexts(unsigned count)
{
    Vector<String> texts;
    for (unsigned number = 0; number < count; ++number)
        texts.append(makeString("key"_s, number));
    return texts;
}

static void checkEveryKeyIsFound(const EncodedTable& encoded)
{
    for (uint32_t entry = 0; entry < encoded.texts().size(); ++entry) {
        const String& text = encoded.texts()[entry];
        unsigned hash = text.impl()->hash();
        AOT_TEST_CHECK(encoded.find(text, hash) == entry);

        String wide = in16Bits(text);
        AOT_TEST_CHECK(wide.impl()->hash() == hash);
        AOT_TEST_CHECK(encoded.find(wide, wide.impl()->hash()) == entry);
    }
}

static void checkAbsentKeysAreNotFound(const EncodedTable& encoded)
{
    for (unsigned number = 0; number < 64; ++number) {
        String absent = makeString("absent"_s, number);
        AOT_TEST_CHECK(!encoded.find(absent, absent.impl()->hash()));
        AOT_TEST_CHECK(!encoded.find(in16Bits(absent), absent.impl()->hash()));
    }
    for (const String& text : encoded.texts())
        AOT_TEST_CHECK(!encoded.find("absent"_s, text.impl()->hash()));
}

static void checkTableOf(unsigned count, CompactTable::SlotKind expectedSlotKind)
{
    EncodedTable encoded(numberedTexts(count));
    const TableInImage& table = encoded.table();
    AOT_TEST_CHECK(table.count == count && table.alive == count);
    AOT_TEST_CHECK(table.capacity >= count && table.capacity >= 2 && !(table.capacity % 2) && table.capacity <= count + 1);
    AOT_TEST_CHECK(CompactTable::slotKindFor(table.capacity) == expectedSlotKind);
    AOT_TEST_CHECK(!table.hasValues);
    AOT_TEST_CHECK(reinterpret_cast<const uint8_t*>(table.strings() + table.capacity) == reinterpret_cast<const uint8_t*>(&table) + encoded.size());
    AOT_TEST_CHECK(table.index() == reinterpret_cast<const uint8_t*>(&table) + sizeof(CompactTable));
    if (expectedSlotKind != CompactTable::SlotKind::Tag)
        AOT_TEST_CHECK(4 * static_cast<uint64_t>(count) <= 3 * (static_cast<uint64_t>(1) << table.indexBits));
    for (uint32_t entry = 0; entry < count; ++entry)
        AOT_TEST_CHECK(table.stringAt(entry) == StringID(entry));

    checkEveryKeyIsFound(encoded);
    checkAbsentKeysAreNotFound(encoded);
}

static void testOneKey() { checkTableOf(1, CompactTable::SlotKind::Tag); }
static void testEightKeys() { checkTableOf(8, CompactTable::SlotKind::Tag); }
static void testNineKeys() { checkTableOf(9, CompactTable::SlotKind::Short); }
static void testThreeHundredKeys() { checkTableOf(300, CompactTable::SlotKind::Long); }

static void testKeysWithOneHash()
{
    for (unsigned count : { 8u, 9u, 300u }) {
        constexpr unsigned hash = 0xabcdef;
        EncodedTable encoded(numberedTexts(count), { }, hash);
        for (uint32_t entry = 0; entry < count; ++entry)
            AOT_TEST_CHECK(encoded.find(encoded.texts()[entry], hash) == entry);
        AOT_TEST_CHECK(!encoded.find("absent"_s, hash));
        AOT_TEST_CHECK(!encoded.find(encoded.texts()[0], hash ^ 0xff0000));
    }
}

static void testValues()
{
    for (unsigned count : { 1u, 8u, 9u, 300u }) {
        Vector<uint64_t> values;
        for (unsigned number = 0; number < count; ++number)
            values.append(0x1000000000ull + number);
        EncodedTable encoded(numberedTexts(count), values.span());
        const TableInImage& table = encoded.table();
        AOT_TEST_CHECK(table.hasValues);
        AOT_TEST_CHECK(!(reinterpret_cast<uintptr_t>(table.values()) % sizeof(uint64_t)));
        AOT_TEST_CHECK(reinterpret_cast<const uint8_t*>(table.values() + table.capacity) == reinterpret_cast<const uint8_t*>(&table) + encoded.size());
        for (uint32_t entry = 0; entry < count; ++entry) {
            const String& text = encoded.texts()[entry];
            std::optional<uint32_t> found = encoded.find(text, text.impl()->hash());
            AOT_TEST_CHECK(found && table.values()[*found] == values[entry]);
        }
    }
}

static void testWhatIsNotEncoded()
{
    TableInImage::Key keys[] = { { 0, 1 }, { 1, 2 } };
    uint64_t values[] = { 7, 8, 9 };
    AOT_TEST_CHECK(TableInImage::tryEncode(std::span(keys), { }));
    AOT_TEST_CHECK(TableInImage::tryEncode(std::span(keys), std::span(values).first(2)));

    AOT_TEST_CHECK(!TableInImage::tryEncode({ }, { }));
    AOT_TEST_CHECK(!TableInImage::tryEncode(std::span(keys), std::span(values).first(1)));
    AOT_TEST_CHECK(!TableInImage::tryEncode(std::span(keys), std::span(values)));
    AOT_TEST_CHECK(!TableInImage::tryEncode(unsafeMakeSpan<const TableInImage::Key>(keys, static_cast<size_t>(CompactTable::maxCapacity) + 1), { }));
}

static String textWithAccent()
{
    const Latin1Character characters[] = { 'c', 'a', 'f', 0xe9 };
    String text = String(std::span(characters));
    AOT_TEST_CHECK(text.is8Bit());
    return text;
}

static String textWithSurrogatePair()
{
    const char16_t characters[] = { 0xd83d, 0xde00, 's', 'm', 'i', 'l', 'e' };
    return String(std::span(characters));
}

static String textWithOtherSurrogatePair()
{
    const char16_t characters[] = { 0xd83d, 0xde01, 's', 'm', 'i', 'l', 'e' };
    return String(std::span(characters));
}

static void testKeysOutsideASCII()
{
    for (unsigned numberOfOtherKeys : { 0u, 6u, 7u, 298u }) {
        Vector<String> texts = numberedTexts(numberOfOtherKeys);
        texts.append(textWithAccent());
        texts.append(textWithSurrogatePair());
        EncodedTable encoded(WTF::move(texts));
        checkEveryKeyIsFound(encoded);
        checkAbsentKeysAreNotFound(encoded);

        AOT_TEST_CHECK(!encoded.find("cafe"_s, String("cafe"_s).impl()->hash()));
        AOT_TEST_CHECK(!encoded.find("cafe"_s, textWithAccent().impl()->hash()));
        String other = textWithOtherSurrogatePair();
        AOT_TEST_CHECK(!encoded.find(other, other.impl()->hash()));
        AOT_TEST_CHECK(!encoded.find(other, textWithSurrogatePair().impl()->hash()));
    }
}

static constexpr ASCIILiteral programWithTexts =
    "layout L0 name=\"Names\" fields=["
    "{key=\"plain\" declared={type=number}}, "
    "{key=\"caf\\u{e9}\" declared={type=number}}, "
    "{key=\"\\u{1f600}smile\" declared={type=number}}]\n"
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "entry module=m0\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = Integer integer=1\n"
    "    v1 = NewObject -, v0, v0, v0 layout=L0 fields=[0, 1, 2]\n"
    "    Return v1\n"
    "}\n"_s;

static StringID findText(const ImageText& text, StringView wanted)
{
    for (uint32_t index = 0; index < text.numberOfStrings(); ++index) {
        if (text.hasText(StringID(index)) && text.equals(StringID(index), wanted))
            return StringID(index);
    }
    failNativeTest(__FILE__, __LINE__, "the image has the text");
}

static void testTextOfTheImage(LoadedProgram&)
{
    const Image& image = *Image::current();
    const ImageText& text = image.text();
    const String texts[] = { "plain"_s, textWithAccent(), textWithSurrogatePair() };

    Vector<TableInImage::Key> keys;
    for (const String& key : texts) {
        StringID string = findText(text, key);
        AOT_TEST_CHECK(text.hash(string) == key.impl()->hash());
        keys.append({ string.index(), text.hash(string) });
    }
    std::optional<Vector<uint8_t>> bytes = TableInImage::tryEncode(keys.span(), { });
    AOT_TEST_CHECK(bytes);
    Vector<uint64_t> words(FillWith { }, (bytes->size() + sizeof(uint64_t) - 1) / sizeof(uint64_t), static_cast<uint64_t>(0));
    memcpy(words.mutableSpan().data(), bytes->span().data(), bytes->size());
    const TableInImage& table = *reinterpret_cast<const TableInImage*>(words.span().data());

    for (uint32_t entry = 0; entry < std::size(texts); ++entry) {
        const String& key = texts[entry];
        AOT_TEST_CHECK(table.textAt(image, entry) == key);
        AOT_TEST_CHECK(table.find(image, key, key.impl()->hash()) == entry);
        String wide = in16Bits(key);
        AOT_TEST_CHECK(table.find(image, wide, wide.impl()->hash()) == entry);
    }
    AOT_TEST_CHECK(table.textAt(image, 0).is8Bit() && !table.textAt(image, 2).is8Bit());

    String absent = "cafe"_s;
    AOT_TEST_CHECK(!table.find(image, absent, absent.impl()->hash()));
    AOT_TEST_CHECK(!table.find(image, absent, texts[1].impl()->hash()));
    String other = textWithOtherSurrogatePair();
    AOT_TEST_CHECK(!table.find(image, other, other.impl()->hash()));
    AOT_TEST_CHECK(!table.find(image, other, texts[2].impl()->hash()));
}

std::span<const NativeTest> tableInImageTests()
{
    static constexpr NativeTest tests[] = {
        { "table-in-image/one-key"_s, testOneKey },
        { "table-in-image/eight-keys"_s, testEightKeys },
        { "table-in-image/nine-keys"_s, testNineKeys },
        { "table-in-image/three-hundred-keys"_s, testThreeHundredKeys },
        { "table-in-image/keys-with-one-hash"_s, testKeysWithOneHash },
        { "table-in-image/values"_s, testValues },
        { "table-in-image/what-is-not-encoded"_s, testWhatIsNotEncoded },
        { "table-in-image/keys-outside-ascii"_s, testKeysOutsideASCII },
        { "table-in-image/text-of-the-image"_s, runNativeTestWithLoadedProgram<tableInImageTests, programWithTexts, testTextOfTheImage> },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
