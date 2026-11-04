/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTControlEmitter.h"

#if ENABLE(AOT)

#include "AOTConversionEmitter.h"
#include "AOTNumberLiteralStrings.h"
#include "AOTSlotRepresentation.h"
#include "AOTSort.h"
#include "AOTStringEmitter.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

AOT_THRESHOLD(unsigned, maxLiteralsComparedByCalls, 2);
AOT_THRESHOLD(unsigned, minStringsForWordDispatch, 3);
static constexpr uint16_t otherCellPrimitives = cellPrimitives & ~Primitive::AnyString;

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
        RELEASE_ASSERT(m_terminator->targets.size() == literals.size() + 1);
        ControlEmitter& control = emitter<ControlEmitter>();
        Vector<SwitchCase, 8> cases;
        cases.reserveInitialCapacity(literals.size());
        for (uint32_t index = 0; index < literals.size(); ++index)
            cases.append(SwitchCase { literals[index], m_strings.textOf(literals[index]), control.countedEdgeTo(m_terminator, index) });
        m_otherwise = control.countedEdgeTo(m_terminator, static_cast<uint32_t>(literals.size()));

        ValueID subjectID = m_terminator->subjectOperand();
        const Type& type = typeOf(subjectID);
        if (cases.isEmpty() || !(type.primitives & Primitive::AnyString)) {
            m_out.jump(m_otherwise.block());
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
        case Representation::DoubleWithPayload:
        case Representation::ImpureDouble:
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
    struct SwitchCase {
        StringID string;
        String text;
        B3::FrequentedBlock target;
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

    void switchOn(Integer value, std::span<const SwitchTarget> cases)
    {
        if (cases.empty()) {
            m_out.jump(m_otherwise.block());
            return;
        }
        if (cases.size() == 1) {
            branch(equal(value, integer(cases[0].caseValue)), { cases[0].target, m_otherwise });
            return;
        }
        m_out.switchOn(value.value(), cases, m_otherwise);
    }

    void emitTestChain(const CaseGroup& cases, NOESCAPE const Invocable<void(const SwitchCase&, B3::FrequentedBlock)> auto& emitTest)
    {
        size_t remaining = cases.size();
        RELEASE_ASSERT(remaining);
        for (const SwitchCase* switchCase : cases) {
            B3::FrequentedBlock next = --remaining ? normalEdge(newBlock()) : m_otherwise;
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
            if (std::optional<uint32_t> number = inputs().literalStrings.indexOf(switchCase->string))
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
        if (cases.size() <= maxLiteralsComparedByCalls) {
            emitTestChain(cases, [&](const SwitchCase& switchCase, B3::FrequentedBlock next) {
                m_strings.branchOnEqualsLiteral(m_terminator, ComparedOperand::Value, subject.value(), switchCase.string, { switchCase.target, next });
            });
            return;
        }

        // A text with a unit above 0xFF equals no 8-bit string.
        CaseGroup narrowCases;
        for (const SwitchCase* switchCase : cases) {
            if (switchCase->text.is8Bit())
                narrowCases.append(switchCase);
        }
        bool hasWideText = narrowCases.size() != cases.size();

        bool textsAreShort = !hasWideText && std::ranges::all_of(cases, [](const SwitchCase* switchCase) {
            return !switchCase->text.isEmpty() && switchCase->text.length() <= maxShortStringLength;
        });
        if (textsAreShort) {
            switchByShortString(subject, type, cases);
            return;
        }

        CellPointer string = requireString(subject, type);
        if ((hasWideText && m_terminator.shape() != CodeShape::Compact) || shouldDispatchOnWord(cases))
            switchByLengthThenWord(string, narrowCases, cases);
        else
            switchByLengthThenCalls(string, m_strings.lengthOf(string), cases);
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

    void compareByCall(CellPointer string, const SwitchCase& switchCase, B3::FrequentedBlock next)
    {
        m_strings.branchOnEqualsLiteral(m_terminator, ComparedOperand::String, string.value(), switchCase.string, { switchCase.target, next });
    }

    void switchByShortString(Boxed subject, const Type& type, const CaseGroup& cases)
    {
        Vector<SwitchTarget, 8> byWord;
        for (const SwitchCase* switchCase : cases)
            byWord.append(SwitchTarget { static_cast<int64_t>(shortStringWord(switchCase->text.span8())), switchCase->target });
        // The stub resolves ropes and 16-bit strings, and returns noShortString for every other value.
        auto switchOnWordFromStub = [&] {
            switchOn(Integer(callStub(m_terminator, shortStringOfValue(), { subject.value() }).first), byWord.span());
        };
        if (m_terminator.shape() == CodeShape::Compact) {
            switchOnWordFromStub();
            return;
        }

        build().statistics().count(StatisticID::ShortStringsReadInPlace);
        LBasicBlock needsStub = newBlock();
        CellPointer string = requireString(subject, type);
        Pointer impl = loadField<Pointer>(string, m_heaps.JSString_value);
        m_strings.continueIf(hasNoBits(impl, JSString::isRopeInPointer), unlikelyEdge(needsStub));
        m_strings.continueIf(hasBits(loadField<Word32>(impl, m_heaps.StringImpl_hashAndFlags), StringImpl::flagIs8Bit()), unlikelyEdge(needsStub));
        switchOverLengthGroups(string, { loadField<Pointer>(impl, m_heaps.StringImpl_data), loadField<Word32>(impl, m_heaps.StringImpl_length), boolean(true), string }, cases);

        m_out.appendTo(needsStub);
        switchOnWordFromStub();
    }

    void switchByLengthThenCalls(CellPointer string, Word32 length, const CaseGroup& cases)
    {
        switchOverLength(length, cases, [&](const CaseGroup& group) {
            emitTestChain(group, [&](const SwitchCase& switchCase, B3::FrequentedBlock next) {
                compareByCall(string, switchCase, next);
            });
        });
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
        if (narrowCases.size() != allCases.size())
            switchOverLengthGroups(string, { view.characters, view.length, boolean(false), view.owner }, allCases);
        else
            switchByLengthThenCalls(string, view.length, allCases);
    }

    void switchOverLengthGroups(CellPointer string, const CharacterView& viewOfKnownWidth, const CaseGroup& cases)
    {
        switchOverLength(viewOfKnownWidth.length, cases, [&](const CaseGroup& group) {
            emitLengthGroup(string, viewOfKnownWidth, group);
        });
    }

    static bool shouldDispatchOnWord(const CaseGroup& cases)
    {
        Vector<unsigned, 8> lengths = cases.map<Vector<unsigned, 8>>([](const SwitchCase* switchCase) {
            return switchCase->text.length();
        });
        AOT::sort(lengths);
        for (size_t first = 0; first + minStringsForWordDispatch <= lengths.size(); ++first) {
            if (lengths[first] == lengths[first + minStringsForWordDispatch - 1])
                return true;
        }
        return false;
    }

    Word32 loadWord(const CharacterView& viewOfKnownWidth, DistinguishingWord word)
    {
        const B3::AbstractHeap& heap = viewOfKnownWidth.is8Bit.value()->asInt() ? m_heaps.characters8.atAnyIndex() : m_heaps.characters16.atAnyIndex();
        LValue load = loadBytes(m_out, word.width, m_out.address(heap, viewOfKnownWidth.characters.value(), word.offset));
        load->as<B3::MemoryValue>()->setReadsMutability(B3::Mutability::Immutable);
        return Word32(load);
    }

    void emitLengthGroup(CellPointer string, const CharacterView& view, const CaseGroup& group)
    {
        bool is8Bit = view.is8Bit.value()->asInt();
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
            compareSequentially(string, view, leaf);
        });
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

    void compareSequentially(CellPointer string, const CharacterView& view, const CaseGroup& group)
    {
        emitTestChain(group, [&](const SwitchCase& switchCase, B3::FrequentedBlock next) {
            if (!m_strings.canCompareInline(switchCase.string)) {
                compareByCall(string, switchCase, next);
                return;
            }
            m_strings.branchOnTextAt(view, word32(0), switchCase.string, { switchCase.target, next });
        });
    }

    StringEmitter& m_strings;
    const TerminatorContext& m_terminator;
    B3::FrequentedBlock m_otherwise;
};

} // anonymous namespace

void ControlEmitter::generateSwitchString(const TerminatorContext& terminator)
{
    StringSwitchEmitter(generator(), terminator).generate();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
