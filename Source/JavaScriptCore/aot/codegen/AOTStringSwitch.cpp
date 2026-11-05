/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTControlEmitter.h"

#if ENABLE(AOT)

#include "AOTConversionEmitter.h"
#include "AOTLiteralStrings.h"
#include "AOTSlotRepresentation.h"
#include "AOTSort.h"
#include "AOTStringEmitter.h"
#include "AOTTypeTable.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class StringSwitchEmitter final : public OperationEmitter {
public:
    StringSwitchEmitter(CodeGenerator& generator, const TerminatorContext& terminator)
        : OperationEmitter(generator)
        , m_strings(generator.emitter<StringEmitter>())
        , m_terminator(terminator)
    {
    }

    void generate()
    {
        std::span<const StringID> literals = m_terminator->strings();
        RELEASE_ASSERT(m_terminator.numberOfTargets() == literals.size() + 1);
        Vector<SwitchCase, 8> cases;
        cases.reserveInitialCapacity(literals.size());
        for (uint32_t index = 0; index < literals.size(); ++index)
            cases.append(SwitchCase { literals[index], m_strings.textOf(literals[index]), generator().edgeTo(m_terminator, index) });
        m_otherwise = generator().edgeTo(m_terminator, static_cast<uint32_t>(literals.size()));

        ValueID subjectID = m_terminator->subjectOperand();
        const Type& type = typeOf(subjectID);
        if (cases.isEmpty() || !(type.primitives & Primitive::AnyString)) {
            m_out.jump(m_otherwise);
            return;
        }
        CaseGroup allCases = cases.map<CaseGroup>([](const SwitchCase& switchCase) {
            return &switchCase;
        });

        Operand subject = get(subjectID);
        switch (subject.representation) {
        case Representation::LiteralString:
            switchOnLiteralStringWord(subject.as<LiteralStringWord>(), allCases);
            return;
        case Representation::Cell:
            switchOnValue(boxCell(subject.as<CellPointer>()), type, allCases);
            return;
        case Representation::JSValue:
            switchOnValue(subject.as<Boxed>(), type, allCases);
            return;
        case Representation::Unchosen:
        case Representation::Void:
        case Representation::Boolean:
        case Representation::Int32:
        case Representation::Int64:
        case Representation::Double:
        case Representation::OptionalInt64:
        case Representation::OptionalDouble:
        case Representation::Cell32:
        case Representation::RawWord:
            fail(m_terminator, InternalErrorKind::OpcodeWithoutCode);
            m_out.unreachable();
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

private:
    static constexpr unsigned maxLiteralsComparedOneByOne = 1;
    static constexpr unsigned minStringsForWordDispatch = 3;
    static constexpr size_t mostCharactersWrittenDown = std::tuple_size_v<decltype(NarrowCharactersOfString::characters)>;
    static constexpr uint16_t otherCellPrimitives = cellPrimitives & ~Primitive::AnyString;

    // No two cases have the same text.
    struct SwitchCase {
        StringID string;
        String text;
        BlockEdge target;
    };
    using CaseGroup = Vector<const SwitchCase*, 8>;
    struct KeyedCaseGroup {
        uint64_t key;
        LBasicBlock block;
        CaseGroup cases;
    };
    using KeyedCaseGroups = Vector<KeyedCaseGroup, 8>;
    struct DistinguishingWord {
        size_t offset;
        unsigned width;
    };
    using TextBytes = Vector<uint8_t, 16>;

    // The text as it lies in a string of that width.
    static TextBytes bytesOf(const String& text, bool is8Bit)
    {
        TextBytes bytes;
        for (unsigned i = 0; i < text.length(); ++i) {
            char16_t unit = text[i];
            bytes.append(static_cast<uint8_t>(unit));
            if (!is8Bit)
                bytes.append(static_cast<uint8_t>(unit >> 8));
        }
        return bytes;
    }

    static uint32_t littleEndianWordAt(std::span<const uint8_t> text, size_t offset, unsigned width)
    {
        uint32_t word = 0;
        for (unsigned i = 0; i < width; ++i)
            word |= static_cast<uint32_t>(text[offset + i]) << (8 * i);
        return word;
    }

    void switchOn(Integer value, std::span<const SwitchTarget> cases)
    {
        if (cases.empty()) {
            m_out.jump(m_otherwise);
            return;
        }
        if (cases.size() == 1) {
            branch(equal(value, integer(cases[0].caseValue)), { cases[0].target, m_otherwise });
            return;
        }
        m_out.switchOn(value.value(), cases, m_otherwise);
    }

    void emitTestChain(const CaseGroup& cases, NOESCAPE const Invocable<void(const SwitchCase&, BlockEdge)> auto& emitTest)
    {
        size_t remaining = cases.size();
        RELEASE_ASSERT(remaining);
        for (const SwitchCase* switchCase : cases) {
            BlockEdge next = --remaining ? normalEdge(newBlock()) : m_otherwise;
            emitTest(*switchCase, next);
            if (remaining)
                m_out.appendTo(next.block());
        }
    }

    KeyedCaseGroups groupCases(const CaseGroup& cases, NOESCAPE const Invocable<uint64_t(const SwitchCase&)> auto& keyOf)
    {
        KeyedCaseGroups groups;
        UncheckedKeyHashMap<uint64_t, size_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> indexByKey;
        for (const SwitchCase* switchCase : cases) {
            uint64_t key = keyOf(*switchCase);
            size_t index = indexByKey.ensure(key, [&] {
                groups.append(KeyedCaseGroup { key, newBlock(), { } });
                return groups.size() - 1;
            }).iterator->value;
            groups[index].cases.append(switchCase);
        }
        return groups;
    }

    void switchOverGroups(Integer key, const KeyedCaseGroups& groups, NOESCAPE const Invocable<void(const CaseGroup&)> auto& emitGroup)
    {
        Vector<SwitchTarget, 8> targets;
        for (const KeyedCaseGroup& group : groups)
            targets.append(SwitchTarget { static_cast<int64_t>(group.key), normalEdge(group.block) });
        switchOn(key, targets.span());
        for (const KeyedCaseGroup& group : groups) {
            m_out.appendTo(group.block);
            emitGroup(group.cases);
        }
    }

    void switchOverLength(Word32 length, const CaseGroup& cases, NOESCAPE const Invocable<void(const CaseGroup&)> auto& emitGroup)
    {
        KeyedCaseGroups groups = groupCases(cases, [](const SwitchCase& switchCase) -> uint64_t {
            return switchCase.text.length();
        });
        switchOverGroups(zeroExtended(length), groups, emitGroup);
    }

    void switchOnLiteralStringWord(LiteralStringWord subject, const CaseGroup& cases)
    {
        ConversionEmitter& conversions = emitter<ConversionEmitter>();
        Vector<SwitchTarget, 8> byWord;
        CaseGroup casesWithoutWord;
        for (const SwitchCase* switchCase : cases) {
            if (std::optional<uint32_t> number = literalStrings().indexOf(switchCase->string))
                byWord.append(SwitchTarget { literalStringWord(*number), switchCase->target });
            else
                casesWithoutWord.append(switchCase);
        }

        if (!casesWithoutWord.isEmpty()) {
            // A string without a word is represented by its cell.
            LBasicBlock cellCase = newBlock();
            LBasicBlock wordCase = newBlock();
            branch(conversions.isStringCell(subject), { normalEdge(cellCase), normalEdge(wordCase) });
            m_out.appendTo(cellCase);
            switchOnValue(Boxed(subject.value()), m_program.at(TypeTable::string), casesWithoutWord);
            m_out.appendTo(wordCase);
        }
        if (std::optional<NarrowReference> narrow = narrowWordBehind(subject)) {
            switchOn(conversions.bitsOf(*narrow), byWord.span());
            return;
        }
        switchOn(conversions.bitsOf(subject), byWord.span());
    }

    void switchOnValue(Boxed subject, const Type& type, const CaseGroup& cases)
    {
        if (cases.size() <= maxLiteralsComparedOneByOne) {
            bool everyCellIsString = !type.hasObjects() && !(type.primitives & otherCellPrimitives);
            emitTestChain(cases, [&](const SwitchCase& switchCase, BlockEdge next) {
                m_strings.branchOnEqualsLiteral(m_terminator, ComparedOperand::Value, subject.value(), switchCase.string, { switchCase.target, next }, std::nullopt, everyCellIsString);
            });
            return;
        }

        // A text with a unit above 0xFF equals no 8-bit string.
        CaseGroup narrowCases;
        for (const SwitchCase* switchCase : cases) {
            m_strings.makeCellsHoldAtomOf(switchCase->string);
            if (switchCase->text.is8Bit())
                narrowCases.append(switchCase);
        }
        bool hasWideText = narrowCases.size() != cases.size();

        CellPointer string = requireString(subject, type);
        if (hasWideText)
            switchByLengthThenWord(string, narrowCases, cases);
        else
            switchByCharactersOfNarrowString(string, cases);
    }

    CellPointer requireString(Boxed subject, const Type& type)
    {
        CellPointer cell = unboxCell(subject);
        if (type.primitives & ~cellPrimitives)
            m_strings.continueIf(isCell(subject), m_otherwise);
        if (type.hasObjects() || type.primitives & otherCellPrimitives)
            m_strings.continueIf(hasCellType(cell, StringType), m_otherwise);
        return cell;
    }

    // A rope and a 16-bit string go elsewhere.
    CharacterView viewOfFlatNarrowStringOrElse(CellPointer string, BlockEdge otherwise)
    {
        Pointer impl = loadField<Pointer>(string, m_heaps.JSString_value);
        m_strings.continueIf(hasNoBits(impl, JSString::isRopeInPointer), otherwise);
        m_strings.continueIf(hasBits(loadField<Word32>(impl, m_heaps.StringImpl_hashAndFlags), StringImpl::flagIs8Bit()), otherwise);
        Pointer characters = loadField<Pointer>(impl, m_heaps.StringImpl_data);
        return { characters, loadField<Word32>(impl, m_heaps.StringImpl_length), boolean(true), string };
    }

    // For a text that is too long to be compared in place.
    void compareByCall(CellPointer string, const SwitchCase& switchCase, BlockEdge next)
    {
        m_strings.branchOnEqualsLiteralByStub(m_terminator, ComparedOperand::String, string.value(), switchCase.string, { switchCase.target, next });
    }

    // As bun_core::comptime_string_map! compiles: a jump by the length, then words of the characters against words of the texts. What stands in front is what it takes to have the characters and the length of a string in hand.
    void switchByCharactersOfNarrowString(CellPointer string, const CaseGroup& cases)
    {
        LBasicBlock needsStub = newBlock();
        LBasicBlock hasCharacters = newBlock();
        ScopeFork fork = m_out.forkScope();
        // Rare: the block that calls sets up a frame of its own and takes it down again, and whoever brings a flat 8-bit string passes through without one.
        CharacterView flat = viewOfFlatNarrowStringOrElse(string, rareEdge(needsStub));
        LValue characters = m_out.phiIn(hasCharacters, flat.characters.value()->type());
        LValue length = m_out.phiIn(hasCharacters, flat.length.value()->type());
        auto goOnWith = [&](Pointer itsCharacters, Word32 itsLength) {
            m_out.upsilon(itsCharacters.value(), characters);
            m_out.upsilon(itsLength.value(), length);
            m_out.jump(normalEdge(hasCharacters));
        };
        goOnWith(flat.characters, flat.length);

        // One call for the whole switch, whatever the number of its texts: the stub writes the characters down as 8-bit characters, and the same compares read them. A rope that is made for the switch and dropped behind it is never made flat.
        static_assert(maxLengthOfTextComparedInPlace <= mostCharactersWrittenDown);
        m_out.appendTo(needsStub);
        Pointer answer(callStub(m_terminator, narrowCharactersOfString(), { string.value() }).first);
        Word32 lengthWrittenDown(loadBytes(m_out, sizeof(uint32_t), m_out.address(m_heaps.characters8.atAnyIndex(), answer.value(), OBJECT_OFFSETOF(NarrowCharactersOfString, length))));
        goOnWith(Pointer(m_out.add(answer.value(), m_out.constIntPtr(OBJECT_OFFSETOF(NarrowCharactersOfString, characters)))), lengthWrittenDown);

        m_out.appendToJoin(hasCharacters, fork);
        switchOverLengthGroups(string, { Pointer(characters), Word32(length), boolean(true), string }, cases);
    }

    void switchByLengthThenWord(CellPointer string, const CaseGroup& narrowCases, const CaseGroup& allCases)
    {
        CharacterView view = m_strings.viewOfString(m_terminator, string);
        LBasicBlock narrow = newBlock();
        LBasicBlock wide = newBlock();
        branch(view.is8Bit, { normalEdge(narrow), unlikelyEdge(wide) });

        m_out.appendTo(narrow);
        switchOverLengthGroups(string, { view.characters, view.length, boolean(true), view.owner }, narrowCases);

        // Whoever writes a case that only a 16-bit string can equal expects such strings.
        m_out.appendTo(wide);
        switchOverLengthGroups(string, { view.characters, view.length, boolean(false), view.owner }, allCases);
    }

    void switchOverLengthGroups(CellPointer string, const CharacterView& viewOfKnownWidth, const CaseGroup& cases)
    {
        switchOverLength(viewOfKnownWidth.length, cases, [&](const CaseGroup& group) {
            emitLengthGroup(string, viewOfKnownWidth, group);
        });
    }

    Word32 loadWord(const CharacterView& viewOfKnownWidth, DistinguishingWord word)
    {
        // The characters may be what the stub has written down.
        RELEASE_ASSERT(!viewOfKnownWidth.is8Bit.value()->asInt() || word.offset + word.width <= mostCharactersWrittenDown);
        const Machine::AbstractHeap& heap = viewOfKnownWidth.is8Bit.value()->asInt() ? m_heaps.characters8.atAnyIndex() : m_heaps.characters16.atAnyIndex();
        return Word32(loadBytes(m_out, word.width, m_out.address(heap, viewOfKnownWidth.characters.value(), word.offset)));
    }

    // The subject has the length of the texts.
    void emitLengthGroup(CellPointer string, const CharacterView& view, const CaseGroup& group)
    {
        bool is8Bit = view.is8Bit.value()->asInt();
        // Of a subject that is longer than what the stub writes down no character may be read: such texts are compared off the cell, one after the other.
        if (group[0]->text.length() > mostCharactersWrittenDown) {
            compareSequentially(string, view, group);
            return;
        }
        Vector<TextBytes, 8> texts = group.map<Vector<TextBytes, 8>>([&](const SwitchCase* switchCase) {
            return bytesOf(switchCase->text, is8Bit);
        });
        std::optional<DistinguishingWord> word = findDistinguishingWord(texts.span());
        if (!word) {
            compareSequentially(string, view, group);
            return;
        }
        auto wordOf = [&](const SwitchCase& switchCase) -> uint64_t {
            return littleEndianWordAt(bytesOf(switchCase.text, is8Bit).span(), word->offset, word->width);
        };
        Integer loaded = zeroExtended(loadWord(view, *word));
        ensureStillAliveHere(view.owner);

        if (word->width == texts[0].size()) {
            Vector<SwitchTarget, 8> byText;
            for (const SwitchCase* switchCase : group)
                byText.append(SwitchTarget { static_cast<int64_t>(wordOf(*switchCase)), switchCase->target });
            switchOn(loaded, byText.span());
            return;
        }

        switchOverGroups(loaded, groupCases(group, wordOf), [&](const CaseGroup& leaf) {
            compareSequentially(string, view, leaf, word);
        });
    }

    // StringEmitter::branchOnTextAt() reads the whole text in words of up to eight bytes, the last of which may lie over the one in front of it.
    static size_t numberOfLoadsOfWholeText(size_t size)
    {
        size_t width = size >= 8 ? 8 : size >= 4 ? 4 : size >= 2 ? 2 : 1;
        return (size + width - 1) / width;
    }

    // The subject has the length of the text, and at `known` it holds the bytes of the text: the switch has looked at them. False, and nothing is emitted: the bytes around them take no fewer loads than the whole text.
    bool tryBranchOnBytesAround(DistinguishingWord known, const CharacterView& view, const SwitchCase& switchCase, BlockEdge next)
    {
        TextBytes text = bytesOf(switchCase.text, view.is8Bit.value()->asInt());
        Vector<DistinguishingWord, 8> pieces;
        auto cover = [&](size_t begin, size_t end) {
            while (begin < end) {
                unsigned width = end - begin >= 4 ? 4 : end - begin >= 2 ? 2 : 1;
                pieces.append(DistinguishingWord { begin, width });
                begin += width;
            }
        };
        cover(0, known.offset);
        cover(known.offset + known.width, text.size());
        if (pieces.isEmpty() || pieces.size() >= numberOfLoadsOfWholeText(text.size()))
            return false;

        size_t remaining = pieces.size();
        for (DistinguishingWord piece : pieces) {
            Condition isSame = equal(loadWord(view, piece), word32(static_cast<int32_t>(littleEndianWordAt(text.span(), piece.offset, piece.width))));
            if (--remaining) {
                m_strings.continueIf(isSame, next);
                continue;
            }
            ensureStillAliveHere(view.owner);
            branch(isSame, { switchCase.target, next });
        }
        return true;
    }

    static std::optional<DistinguishingWord> findDistinguishingWord(std::span<const TextBytes> group)
    {
        if (group.size() < minStringsForWordDispatch)
            return std::nullopt;
        size_t length = group[0].size();
        unsigned width = length >= 4 ? 4 : length >= 2 ? 2 : 1;
        std::optional<DistinguishingWord> best;
        size_t mostWords = 1;
        Vector<uint32_t, 8> words(group.size());
        for (size_t offset = 0; offset + width <= length && mostWords < group.size(); ++offset) {
            for (size_t i = 0; i < group.size(); ++i)
                words[i] = littleEndianWordAt(group[i].span(), offset, width);
            AOT::sort(words);
            size_t distinctWords = std::unique(words.begin(), words.end()) - words.begin();
            if (distinctWords > mostWords) {
                mostWords = distinctWords;
                best = DistinguishingWord { offset, width };
            }
        }
        return best;
    }

    void compareSequentially(CellPointer string, const CharacterView& view, const CaseGroup& group, std::optional<DistinguishingWord> known = std::nullopt)
    {
        emitTestChain(group, [&](const SwitchCase& switchCase, BlockEdge next) {
            if (!m_strings.canCompareInline(switchCase.string)) {
                compareByCall(string, switchCase, next);
                return;
            }
            if (known && tryBranchOnBytesAround(*known, view, switchCase, next))
                return;
            m_strings.branchOnTextAt(view, word32(0), switchCase.string, { switchCase.target, next });
        });
    }

    StringEmitter& m_strings;
    const TerminatorContext& m_terminator;
    BlockEdge m_otherwise;
};

} // anonymous namespace

void ControlEmitter::generateSwitchString(const TerminatorContext& terminator)
{
    StringSwitchEmitter(generator(), terminator).generate();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
