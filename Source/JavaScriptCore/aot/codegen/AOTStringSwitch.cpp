/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTControlEmitter.h"

#if ENABLE(AOT)

#include "AOTOpcodeSets.h"
#include "AOTSlotRepresentation.h"
#include "AOTSort.h"
#include "AOTStringEmitter.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class StringSwitchEmitter final : public OperationEmitter {
public:
    StringSwitchEmitter(CodeGenerator& generator, const OperationContext& context)
        : OperationEmitter(generator)
        , m_strings(generator.emitter<StringEmitter>())
        , m_context(context)
    {
    }

    void generate(const TerminatorContext& terminator)
    {
        std::span<const StringID> literals = terminator->strings();
        RELEASE_ASSERT(terminator.numberOfTargets() == literals.size() + 1);
        Vector<SwitchCase, 8> cases;
        cases.reserveInitialCapacity(literals.size());
        for (uint32_t index = 0; index < literals.size(); ++index)
            cases.append(SwitchCase { literals[index], m_strings.textOf(literals[index]), generator().edgeTo(terminator, index) });
        m_otherwise = generator().edgeTo(terminator, static_cast<uint32_t>(literals.size()));
        generate(terminator->subjectOperand(), cases);
    }

    void generate(const ValueContext& first, const CodeGenerator::LiteralChain& chain)
    {
        Vector<SwitchCase, 8> cases;
        for (const CodeGenerator::ComparisonWithLiteral& comparison : chain.comparisons) {
            bool isRepeated = std::ranges::any_of(cases, [&](const SwitchCase& earlier) { return earlier.string == comparison.literal; });
            if (!isRepeated)
                cases.append(SwitchCase { comparison.literal, m_strings.textOf(comparison.literal), cases.isEmpty() ? first.branchTargets().taken : generator().edgeOutOf(comparison.block, 0) });
        }
        m_otherwise = generator().edgeOutOf(chain.comparisons.last().block, 1);
        generate(chain.comparisons.first().subject, cases);
    }

private:
    struct SwitchCase;

    void generate(ValueID subjectID, const Vector<SwitchCase, 8>& cases)
    {
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
            fail(m_context, InternalErrorKind::OpcodeWithoutCode);
            m_out.unreachable();
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    static constexpr unsigned maxLiteralsComparedOneByOne = 1;
    static constexpr unsigned maxHeadersComparedOneByOne = 16;
    static constexpr unsigned minStringsForWordDispatch = 3;
    static constexpr size_t mostCharactersWrittenDown = std::tuple_size_v<decltype(NarrowCharactersOfString::characters)>;
    static constexpr uint16_t otherCellPrimitives = cellPrimitives & ~Primitive::AnyString;

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

    void switchOnValue(Boxed subject, const Type& type, const CaseGroup& cases)
    {
        if (cases.size() <= maxLiteralsComparedOneByOne) {
            bool everyCellIsString = !type.hasObjects() && !(type.primitives & otherCellPrimitives);
            emitTestChain(cases, [&](const SwitchCase& switchCase, BlockEdge next) {
                m_strings.branchOnEqualsLiteral(m_context, ComparedOperand::Value, subject.value(), switchCase.string, { switchCase.target, next }, std::nullopt, everyCellIsString);
            });
            return;
        }

        CaseGroup narrowCases;
        for (const SwitchCase* switchCase : cases) {
            m_strings.makeCellsHoldAtomOf(switchCase->string, switchCase->text);
            if (switchCase->text.is8Bit())
                narrowCases.append(switchCase);
        }
        bool hasWideText = narrowCases.size() != cases.size();

        CellPointer string = requireString(subject, type);
        bool mayHoldName = cases.size() <= maxHeadersComparedOneByOne && std::ranges::all_of(cases, [&](const SwitchCase* switchCase) {
            return m_strings.mayBeNameInLoop(m_context, switchCase->text);
        });
        if (mayHoldName) {
            LBasicBlock holdsName = newBlock();
            LBasicBlock holdsOtherText = newBlock();
            ScopeFork fork = m_out.forkScope();
            branch(m_strings.holdsName(string), { normalEdge(holdsName), normalEdge(holdsOtherText) });
            m_out.appendToArm(holdsName, fork);
            for (const SwitchCase* switchCase : cases)
                m_strings.branchOnHeaderOf(string, switchCase->string, switchCase->target);
            m_out.jump(m_otherwise);
            m_out.appendToArm(holdsOtherText, fork);
        }
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

    CharacterView viewOfFlatNarrowStringOrElse(CellPointer string, BlockEdge otherwise)
    {
        Pointer impl = loadField<Pointer>(string, m_heaps.JSString_value);
        m_strings.continueIf(hasNoBits(impl, JSString::isRopeInPointer), otherwise);
        m_strings.continueIf(hasBits(loadField<Word32>(impl, m_heaps.StringImpl_hashAndFlags), StringImpl::flagIs8Bit()), otherwise);
        Pointer characters = loadField<Pointer>(impl, m_heaps.StringImpl_data);
        return { characters, loadField<Word32>(impl, m_heaps.StringImpl_length), boolean(true), string };
    }

    void compareByCall(CellPointer string, const SwitchCase& switchCase, BlockEdge next)
    {
        m_strings.branchOnEqualsLiteralByStub(m_context, ComparedOperand::String, string.value(), switchCase.string, { switchCase.target, next });
    }

    void switchByCharactersOfNarrowString(CellPointer string, const CaseGroup& cases)
    {
        LBasicBlock needsStub = newBlock();
        LBasicBlock hasCharacters = newBlock();
        ScopeFork fork = m_out.forkScope();
        CharacterView flat = viewOfFlatNarrowStringOrElse(string, rareEdge(needsStub));
        LValue characters = m_out.phiIn(hasCharacters, flat.characters.value()->type());
        LValue length = m_out.phiIn(hasCharacters, flat.length.value()->type());
        auto goOnWith = [&](Pointer itsCharacters, Word32 itsLength) {
            m_out.upsilon(itsCharacters.value(), characters);
            m_out.upsilon(itsLength.value(), length);
            m_out.jump(normalEdge(hasCharacters));
        };
        goOnWith(flat.characters, flat.length);

        static_assert(maxLengthOfTextComparedInPlace <= mostCharactersWrittenDown);
        m_out.appendTo(needsStub);
        if (generator().isInLoop(m_context.block())) {
            build().statistics().count(StatisticID::StringSwitchRopeLengthChecks);
            unsigned shortest = cases[0]->text.length();
            unsigned longest = shortest;
            for (const SwitchCase* switchCase : cases) {
                shortest = std::min(shortest, switchCase->text.length());
                longest = std::max(longest, switchCase->text.length());
            }
            m_strings.continueIf(belowOrEqual(sub(m_strings.lengthOf(string), word32(static_cast<int32_t>(shortest))), word32(static_cast<int32_t>(longest - shortest))), m_otherwise);
        }
        StubCallResult narrow = callStub(m_context, narrowCharactersOfString(), { string.value() });
        goOnWith(Pointer(narrow.first), low32(Integer(narrow.second)));

        m_out.appendToJoin(hasCharacters, fork);
        switchOverLengthGroups(string, { Pointer(characters), Word32(length), boolean(true), string }, cases);
    }

    void switchByLengthThenWord(CellPointer string, const CaseGroup& narrowCases, const CaseGroup& allCases)
    {
        CharacterView view = m_strings.viewOfString(m_context, string);
        LBasicBlock narrow = newBlock();
        LBasicBlock wide = newBlock();
        branch(view.is8Bit, { normalEdge(narrow), unlikelyEdge(wide) });

        m_out.appendTo(narrow);
        switchOverLengthGroups(string, { view.characters, view.length, boolean(true), view.owner }, narrowCases);

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
        RELEASE_ASSERT(!viewOfKnownWidth.is8Bit.value()->asInt() || word.offset + word.width <= mostCharactersWrittenDown);
        const Machine::AbstractHeap& heap = viewOfKnownWidth.is8Bit.value()->asInt() ? m_heaps.characters8.atAnyIndex() : m_heaps.characters16.atAnyIndex();
        return Word32(loadBytes(m_out, word.width, m_out.address(heap, viewOfKnownWidth.characters.value(), word.offset)));
    }

    void emitLengthGroup(CellPointer string, const CharacterView& view, const CaseGroup& group)
    {
        bool is8Bit = view.is8Bit.value()->asInt();
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
        for (const SwitchCase* switchCase : group)
            m_strings.recordInlined(switchCase->string);
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

    static size_t numberOfLoadsOfWholeText(size_t size)
    {
        size_t width = size >= 8 ? 8 : size >= 4 ? 4 : size >= 2 ? 2 : 1;
        return (size + width - 1) / width;
    }

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
            if (!StringEmitter::canCompareInline(switchCase.text)) {
                compareByCall(string, switchCase, next);
                return;
            }
            if (known && tryBranchOnBytesAround(*known, view, switchCase, next))
                return;
            m_strings.branchOnTextAt(view, word32(0), switchCase.string, { switchCase.target, next });
        });
    }

    StringEmitter& m_strings;
    const OperationContext& m_context;
    BlockEdge m_otherwise;
};

} // anonymous namespace

void ControlEmitter::generateSwitchString(const TerminatorContext& terminator)
{
    StringSwitchEmitter(generator(), terminator).generate(terminator);
}

bool ControlEmitter::tryGenerateLiteralChain(const ValueContext& comparison)
{
    if (!comparison.isBranchCondition())
        return false;
    std::optional<CodeGenerator::LiteralChain> chain = generator().findLiteralChain(comparison.block());
    if (!chain)
        return false;
    for (auto [block, name] : chain->names)
        generator().generateName(block, name);
    StringSwitchEmitter(generator(), comparison).generate(comparison, *chain);
    build().statistics().count(StatisticID::LiteralChainsSwitched);
    return true;
}

static ValueID valueUnderNames(const Code& code, ValueID value)
{
    while (opcodesThatNameTheirOperand.contains(code.at(value).opcode) && code.at(code.at(value).operand(0)).representation == code.at(value).representation)
        value = code.at(value).operand(0);
    return value;
}

static std::optional<CodeGenerator::ComparisonWithLiteral> findComparisonWithLiteral(const Code& code, const TypeArena& types, BlockID block, ValueID test)
{
    if (!test)
        return std::nullopt;
    const Operation& comparison = code.at(test);
    switch (comparison.opcode) {
    case Opcode::StringEqualsLiteral:
        if (comparison.lengthOperand() || comparison.flatOperand())
            return std::nullopt;
        return CodeGenerator::ComparisonWithLiteral { block, comparison.stringOperand(), comparison.literal() };
    case Opcode::ValueEqualsLiteral:
        if (comparison.lengthOperand())
            return std::nullopt;
        return CodeGenerator::ComparisonWithLiteral { block, comparison.valueOperand(), comparison.literal() };
    case Opcode::StrictEq:
    case Opcode::Eq:
        for (auto [written, subject] : { std::pair { comparison.rightOperand(), comparison.leftOperand() }, std::pair { comparison.leftOperand(), comparison.rightOperand() } }) {
            if (code.at(written).opcode != Opcode::String || code.isConstant(subject))
                continue;
            if (comparison.opcode == Opcode::Eq && !types.at(code.at(subject).type).isAnyString())
                continue;
            return CodeGenerator::ComparisonWithLiteral { block, subject, code.at(written).string() };
        }
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

std::optional<CodeGenerator::LiteralChain> CodeGenerator::findLiteralChain(BlockID id) const
{
    static constexpr size_t minComparisons = 3;
    if (!code().at(id).isInLoop || isLayingOutTurns() || m_keepsValuesInHeapFrame || m_notesForms)
        return std::nullopt;
    const TypeArena& types = inputs().types;
    std::optional<ComparisonWithLiteral> first = findComparisonWithLiteral(code(), types, id, testOfBranch(id));
    if (!first)
        return std::nullopt;
    ValueID subject = valueUnderNames(code(), first->subject);
    Representation representation = code().at(subject).representation;
    if ((representation != Representation::JSValue && representation != Representation::Cell) || !code().at(code().blockOf(subject)).isInLoop)
        return std::nullopt;

    LiteralChain chain;
    chain.comparisons.append(*first);
    auto blockBehind = [&](BlockID from) -> BlockID {
        EdgeID way = code().at(from).terminator.notTaken();
        while (true) {
            const Edge& edge = code().at(way);
            const Block& block = code().at(edge.to);
            if (!edge.arguments().empty() || !block.parameters().empty() || block.isLoopHead || !block.isInLoop)
                return { };
            if (m_singlePredecessors[edge.to.index()] != from || m_blocksWithOtherJumps.get(edge.to.index()) || code().handlerOf(edge.to) != code().handlerOf(id))
                return { };
            if (block.terminator.kind != TerminatorKind::Jump || !code().valuesOf(edge.to).empty())
                return edge.to;
            from = edge.to;
            way = block.terminator.next();
        }
    };
    while (BlockID behind = blockBehind(chain.comparisons.last().block)) {
        ValueID test = testOfBranch(behind);
        std::optional<ComparisonWithLiteral> next = findComparisonWithLiteral(code(), types, behind, test);
        if (!next || valueUnderNames(code(), next->subject) != subject)
            break;
        size_t numberOfNames = chain.names.size();
        bool onlyCompares = std::ranges::all_of(code().valuesOf(behind), [&](ValueID value) {
            if (value == test)
                return true;
            chain.names.append({ behind, value });
            return opcodesThatNameTheirOperand.contains(opcodeOf(value));
        });
        if (!onlyCompares) {
            chain.names.shrink(numberOfNames);
            break;
        }
        chain.comparisons.append(*next);
    }
    if (chain.comparisons.size() < minComparisons)
        return std::nullopt;
    return chain;
}

BlockEdge CodeGenerator::edgeOutOf(BlockID id, uint32_t targetIndex)
{
    TerminatorContext context(id, code().at(id).terminator, handlerOf(id));
    return edgeTo(context, targetIndex);
}

void CodeGenerator::generateName(BlockID block, ValueID name)
{
    generateValue(block, name, code().at(name), false, { });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
