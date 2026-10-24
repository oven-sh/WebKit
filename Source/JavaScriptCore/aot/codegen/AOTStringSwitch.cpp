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
    explicit StringSwitchEmitter(CodeGenerator&);

    void generate(const TerminatorContext&);

private:
    struct SwitchCase {
        StringID string;
        String text;
        B3::FrequentedBlock target;
    };
    using SwitchCases = Vector<SwitchCase, 8>;
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

    void switchOnLiteralStringWord(const TerminatorContext&, LiteralStringWord subject, const CaseGroup&, B3::FrequentedBlock otherwise);
    void switchOnValue(const TerminatorContext&, Boxed subject, const Type&, const CaseGroup&, B3::FrequentedBlock otherwise);
    CellPointer requireString(Boxed subject, const Type&, B3::FrequentedBlock otherwise);

    void switchByCalls(const TerminatorContext&, Boxed subject, const CaseGroup&, B3::FrequentedBlock otherwise);
    void switchByShortString(const TerminatorContext&, Boxed subject, const Type&, const CaseGroup&, B3::FrequentedBlock otherwise);
    void switchByLengthThenCalls(const TerminatorContext&, CellPointer string, const CaseGroup&, B3::FrequentedBlock otherwise);
    void switchByLengthThenWord(const TerminatorContext&, CellPointer string, const CaseGroup& narrowCases, const CaseGroup& allCases, B3::FrequentedBlock otherwise);

    void switchOverLengthGroups(const TerminatorContext&, CellPointer string, const CharacterView& viewOfKnownWidth, const CaseGroup&, B3::FrequentedBlock otherwise);
    void emitLengthGroup(const TerminatorContext&, CellPointer string, const CharacterView&, const CaseGroup&, B3::FrequentedBlock otherwise);
    void compareSequentially(const TerminatorContext&, CellPointer string, const CharacterView&, const CaseGroup&, B3::FrequentedBlock otherwise);
    void compareWideString(const TerminatorContext&, CellPointer string, Word32 length, const CaseGroup&, B3::FrequentedBlock otherwise);
    void compareByCall(const TerminatorContext&, CellPointer string, const SwitchCase&, B3::FrequentedBlock next);

    static bool shouldDispatchOnWord(const CaseGroup&);
    static std::optional<DistinguishingWord> findDistinguishingWord(std::span<const TextBytes>);
    Word32 loadWord(const CharacterView& viewOfKnownWidth, DistinguishingWord);

    KeyedCaseGroups groupCases(const CaseGroup&, NOESCAPE const Invocable<uint64_t(const SwitchCase&)> auto& keyOf);
    KeyedCaseGroups groupCasesByLength(const CaseGroup&);
    void switchOn(Integer value, std::span<const SwitchTarget>, B3::FrequentedBlock otherwise);
    void switchOverGroups(Integer key, const KeyedCaseGroups&, B3::FrequentedBlock otherwise);
    void switchOverLength(Word32 length, const CaseGroup&, B3::FrequentedBlock otherwise, NOESCAPE const Invocable<void(const SwitchCase&, B3::FrequentedBlock)> auto& emitTest);
    void emitTestChain(const CaseGroup&, B3::FrequentedBlock otherwise, NOESCAPE const Invocable<void(const SwitchCase&, B3::FrequentedBlock)> auto& emitTest);

    StringEmitter& m_strings;
};

StringSwitchEmitter::StringSwitchEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
    , m_strings(generator.emitter<StringEmitter>())
{
}

void StringSwitchEmitter::switchOn(Integer value, std::span<const SwitchTarget> cases, B3::FrequentedBlock otherwise)
{
    if (cases.empty()) {
        m_out.jump(otherwise.block());
        return;
    }
    if (cases.size() == 1) {
        branch(equal(value, integer(cases[0].caseValue)), { cases[0].target, otherwise });
        return;
    }
    m_out.switchOn(value.value(), cases, otherwise);
}

void StringSwitchEmitter::emitTestChain(const CaseGroup& cases, B3::FrequentedBlock otherwise, NOESCAPE const Invocable<void(const SwitchCase&, B3::FrequentedBlock)> auto& emitTest)
{
    size_t remaining = cases.size();
    RELEASE_ASSERT(remaining);
    for (const SwitchCase* switchCase : cases) {
        B3::FrequentedBlock next = --remaining ? normalEdge(newBlock()) : otherwise;
        emitTest(*switchCase, next);
        if (remaining)
            m_out.appendTo(next.block());
    }
}

auto StringSwitchEmitter::groupCases(const CaseGroup& cases, NOESCAPE const Invocable<uint64_t(const SwitchCase&)> auto& keyOf) -> KeyedCaseGroups
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

auto StringSwitchEmitter::groupCasesByLength(const CaseGroup& cases) -> KeyedCaseGroups
{
    return groupCases(cases, [](const SwitchCase& switchCase) -> uint64_t {
        return switchCase.text.length();
    });
}

void StringSwitchEmitter::switchOverGroups(Integer key, const KeyedCaseGroups& groups, B3::FrequentedBlock otherwise)
{
    Vector<SwitchTarget, 8> targets;
    for (const KeyedCaseGroup& group : groups)
        targets.append(SwitchTarget { static_cast<int64_t>(group.key), normalEdge(group.block) });
    switchOn(key, targets.span(), otherwise);
}

void StringSwitchEmitter::switchOverLength(Word32 length, const CaseGroup& cases, B3::FrequentedBlock otherwise, NOESCAPE const Invocable<void(const SwitchCase&, B3::FrequentedBlock)> auto& emitTest)
{
    KeyedCaseGroups groups = groupCasesByLength(cases);
    switchOverGroups(zeroExtended(length), groups, otherwise);
    for (const KeyedCaseGroup& group : groups) {
        m_out.appendTo(group.block);
        emitTestChain(group.cases, otherwise, emitTest);
    }
}

void StringSwitchEmitter::generate(const TerminatorContext& terminator)
{
    std::span<const StringID> literals = terminator->strings();
    RELEASE_ASSERT(terminator->targets.size() == literals.size() + 1);
    ControlEmitter& control = emitter<ControlEmitter>();
    SwitchCases cases;
    cases.reserveInitialCapacity(literals.size());
    for (uint32_t index = 0; index < literals.size(); ++index)
        cases.append(SwitchCase { literals[index], m_strings.textOf(literals[index]), control.countedEdgeTo(terminator, index) });
    B3::FrequentedBlock otherwise = control.countedEdgeTo(terminator, static_cast<uint32_t>(literals.size()));

    ValueID subjectID = terminator->subjectOperand();
    const Type& type = typeOf(subjectID);
    if (cases.isEmpty() || !(type.primitives & Primitive::AnyString)) {
        m_out.jump(otherwise.block());
        return;
    }
    CaseGroup allCases = cases.map<CaseGroup>([](const SwitchCase& switchCase) {
        return &switchCase;
    });

    Operand subject = get(subjectID);
    switch (subject.representation) {
    case Representation::LiteralString:
        switchOnLiteralStringWord(terminator, subject.as<LiteralStringWord>(), allCases, otherwise);
        return;
    case Representation::Cell:
        switchOnValue(terminator, boxCell(subject.as<CellPointer>()), type, allCases, otherwise);
        return;
    case Representation::JSValue:
        switchOnValue(terminator, subject.as<Boxed>(), type, allCases, otherwise);
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
        fail(terminator, InternalErrorKind::OpcodeWithoutCode);
        m_out.unreachable();
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void StringSwitchEmitter::switchOnLiteralStringWord(const TerminatorContext& terminator, LiteralStringWord subject, const CaseGroup& cases, B3::FrequentedBlock otherwise)
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
        switchOnValue(terminator, Boxed(subject.value()), m_program.at(TypeTable::string), casesWithoutWord, otherwise);
        m_out.appendTo(wordCase);
    }
    if (std::optional<NarrowReference> narrow = narrowWordBehind(subject)) {
        switchOn(conversions.bitsOf(*narrow), byWord.span(), otherwise);
        return;
    }
    switchOn(conversions.bitsOf(subject), byWord.span(), otherwise);
}

void StringSwitchEmitter::switchOnValue(const TerminatorContext& terminator, Boxed subject, const Type& type, const CaseGroup& cases, B3::FrequentedBlock otherwise)
{
    if (cases.size() <= maxLiteralsComparedByCalls) {
        switchByCalls(terminator, subject, cases, otherwise);
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
        switchByShortString(terminator, subject, type, cases, otherwise);
        return;
    }

    CellPointer string = requireString(subject, type, otherwise);
    if ((hasWideText && terminator.shape() != CodeShape::Compact) || shouldDispatchOnWord(cases))
        switchByLengthThenWord(terminator, string, narrowCases, cases, otherwise);
    else
        switchByLengthThenCalls(terminator, string, cases, otherwise);
}

CellPointer StringSwitchEmitter::requireString(Boxed subject, const Type& type, B3::FrequentedBlock otherwise)
{
    auto continueIf = [&](Condition condition) {
        LBasicBlock continuation = newBlock();
        branch(condition, { normalEdge(continuation), otherwise });
        m_out.appendTo(continuation);
    };
    CellPointer cell = unboxCell(subject);
    if (type.primitives & ~cellPrimitives)
        continueIf(isCell(subject));
    if (type.hasObjects() || type.primitives & otherCellPrimitives)
        continueIf(hasCellType(cell, StringType));
    return cell;
}

void StringSwitchEmitter::compareByCall(const TerminatorContext& terminator, CellPointer string, const SwitchCase& switchCase, B3::FrequentedBlock next)
{
    m_strings.branchOnEqualsLiteral(terminator, ComparedOperand::String, string.value(), switchCase.string, { switchCase.target, next });
}

void StringSwitchEmitter::switchByCalls(const TerminatorContext& terminator, Boxed subject, const CaseGroup& cases, B3::FrequentedBlock otherwise)
{
    emitTestChain(cases, otherwise, [&](const SwitchCase& switchCase, B3::FrequentedBlock next) {
        m_strings.branchOnEqualsLiteral(terminator, ComparedOperand::Value, subject.value(), switchCase.string, { switchCase.target, next });
    });
}

void StringSwitchEmitter::switchByShortString(const TerminatorContext& terminator, Boxed subject, const Type& type, const CaseGroup& cases, B3::FrequentedBlock otherwise)
{
    Vector<SwitchTarget, 8> byWord;
    for (const SwitchCase* switchCase : cases)
        byWord.append(SwitchTarget { static_cast<int64_t>(shortStringWord(switchCase->text.span8())), switchCase->target });
    // The stub resolves ropes and 16-bit strings, and returns noShortString for every other value.
    auto switchOnWordFromStub = [&] {
        switchOn(Integer(callStub(terminator, shortStringOfValue(), { subject.value() }).first), byWord.span(), otherwise);
    };
    if (terminator.shape() == CodeShape::Compact) {
        switchOnWordFromStub();
        return;
    }

    build().statistics().count(StatisticID::ShortStringsReadInPlace);
    LBasicBlock needsStub = newBlock();
    auto continueIf = [&](Condition condition) {
        LBasicBlock continuation = newBlock();
        branch(condition, { normalEdge(continuation), unlikelyEdge(needsStub) });
        m_out.appendTo(continuation);
    };
    CellPointer string = requireString(subject, type, otherwise);
    Pointer impl = loadField<Pointer>(string, m_heaps.JSString_value);
    continueIf(hasNoBits(impl, JSString::isRopeInPointer));
    continueIf(hasBits(loadField<Word32>(impl, m_heaps.StringImpl_hashAndFlags), StringImpl::flagIs8Bit()));
    switchOverLengthGroups(terminator, string, { loadField<Pointer>(impl, m_heaps.StringImpl_data), loadField<Word32>(impl, m_heaps.StringImpl_length), boolean(true), string }, cases, otherwise);

    m_out.appendTo(needsStub);
    switchOnWordFromStub();
}

void StringSwitchEmitter::switchByLengthThenCalls(const TerminatorContext& terminator, CellPointer string, const CaseGroup& cases, B3::FrequentedBlock otherwise)
{
    switchOverLength(m_strings.lengthOf(string), cases, otherwise, [&](const SwitchCase& switchCase, B3::FrequentedBlock next) {
        compareByCall(terminator, string, switchCase, next);
    });
}

void StringSwitchEmitter::switchByLengthThenWord(const TerminatorContext& terminator, CellPointer string, const CaseGroup& narrowCases, const CaseGroup& allCases, B3::FrequentedBlock otherwise)
{
    CharacterView view = m_strings.viewOfString(terminator, string);
    LBasicBlock narrow = newBlock();
    LBasicBlock wide = newBlock();
    branch(view.is8Bit, { normalEdge(narrow), unlikelyEdge(wide) });

    m_out.appendTo(narrow);
    switchOverLengthGroups(terminator, string, { view.characters, view.length, boolean(true), view.owner }, narrowCases, otherwise);

    // Whoever writes a case that only a 16-bit string can equal expects such strings.
    m_out.appendTo(wide);
    if (narrowCases.size() != allCases.size())
        switchOverLengthGroups(terminator, string, { view.characters, view.length, boolean(false), view.owner }, allCases, otherwise);
    else
        compareWideString(terminator, string, view.length, allCases, otherwise);
}

void StringSwitchEmitter::switchOverLengthGroups(const TerminatorContext& terminator, CellPointer string, const CharacterView& view, const CaseGroup& cases, B3::FrequentedBlock otherwise)
{
    KeyedCaseGroups groups = groupCasesByLength(cases);
    switchOverGroups(zeroExtended(view.length), groups, otherwise);
    for (const KeyedCaseGroup& group : groups) {
        m_out.appendTo(group.block);
        emitLengthGroup(terminator, string, view, group.cases, otherwise);
    }
}

bool StringSwitchEmitter::shouldDispatchOnWord(const CaseGroup& cases)
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

Word32 StringSwitchEmitter::loadWord(const CharacterView& view, DistinguishingWord word)
{
    const B3::AbstractHeap& heap = view.is8Bit.value()->asInt() ? m_heaps.characters8.atAnyIndex() : m_heaps.characters16.atAnyIndex();
    TypedPointer address = m_out.address(heap, view.characters.value(), word.offset);
    LValue load = word.width == 4 ? m_out.load32(address) : word.width == 2 ? m_out.load16ZeroExt32(address) : m_out.load8ZeroExt32(address);
    load->as<B3::MemoryValue>()->setReadsMutability(B3::Mutability::Immutable);
    return Word32(load);
}

void StringSwitchEmitter::emitLengthGroup(const TerminatorContext& terminator, CellPointer string, const CharacterView& view, const CaseGroup& group, B3::FrequentedBlock otherwise)
{
    bool is8Bit = view.is8Bit.value()->asInt();
    Vector<TextBytes, 8> texts = group.map<Vector<TextBytes, 8>>([&](const SwitchCase* switchCase) {
        return bytesOf(switchCase->text, is8Bit);
    });
    std::optional<DistinguishingWord> word = findDistinguishingWord(texts.span());
    if (!word) {
        compareSequentially(terminator, string, view, group, otherwise);
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
        switchOn(loaded, byText.span(), otherwise);
        return;
    }

    KeyedCaseGroups leaves = groupCases(group, wordOf);
    switchOverGroups(loaded, leaves, otherwise);
    for (const KeyedCaseGroup& leaf : leaves) {
        m_out.appendTo(leaf.block);
        compareSequentially(terminator, string, view, leaf.cases, otherwise);
    }
}

auto StringSwitchEmitter::findDistinguishingWord(std::span<const TextBytes> group) -> std::optional<DistinguishingWord>
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

void StringSwitchEmitter::compareSequentially(const TerminatorContext& terminator, CellPointer string, const CharacterView& view, const CaseGroup& group, B3::FrequentedBlock otherwise)
{
    emitTestChain(group, otherwise, [&](const SwitchCase& switchCase, B3::FrequentedBlock next) {
        if (!m_strings.canCompareInline(switchCase.string)) {
            compareByCall(terminator, string, switchCase, next);
            return;
        }
        m_strings.branchOnTextAt(view, word32(0), switchCase.string, { switchCase.target, next });
    });
}

void StringSwitchEmitter::compareWideString(const TerminatorContext& terminator, CellPointer string, Word32 length, const CaseGroup& cases, B3::FrequentedBlock otherwise)
{
    switchOverLength(length, cases, otherwise, [&](const SwitchCase& switchCase, B3::FrequentedBlock next) {
        compareByCall(terminator, string, switchCase, next);
    });
}

} // anonymous namespace

void ControlEmitter::generateSwitchString(const TerminatorContext& terminator)
{
    StringSwitchEmitter(generator()).generate(terminator);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
