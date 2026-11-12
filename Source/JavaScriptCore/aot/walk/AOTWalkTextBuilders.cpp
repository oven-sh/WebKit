/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkTextBuilders.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTTypeArena.h"
#include "AOTWalkStrings.h"
#include <algorithm>
#include <limits>
#include <functional>
#include <optional>
#include <span>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet stringsMadeForReadersInTextBuilders { Opcode::StringBuilderToString, Opcode::StackStringBuilderToString };

static const NameOfGrowingText* findNameInTextBuilders(const GrowingText& web, ValueID value)
{
    if (web.names.isEmpty() || value < web.names.first().value || web.names.last().value < value)
        return nullptr;
    auto found = std::ranges::lower_bound(web.names, value, { }, &NameOfGrowingText::value);
    return found != web.names.end() && found->value == value ? &*found : nullptr;
}

template<typename Functor>
static void forEachNameLiveAtHeadInTextBuilders(const BodyLiveness& liveness, const GrowingText& web, BlockID block, const Functor& functor)
{
    for (const NameOfGrowingText& name : web.names) {
        if (liveness.isLiveAtHead(name.value, block))
            functor(name);
    }
}

static bool belongsToTopFrame(const Walker& walker, const StringInBuilder& text)
{
    if (text.frameIndex != walker.topFrameIndex() || !text.builder.isValue() || text.web.names.isEmpty())
        return false;
    uint32_t begunIn = walker.code().at(text.builder.id()).inlineFrame;
    auto frames = walker.code().inlineFrames();
    for (uint32_t frame = walker.originOfOperation().inlineFrame; frame != begunIn; frame = frames[frame].parent) {
        if (!frame)
            return false;
    }
    return true;
}

bool canKeepStringsInBuilders(const Walker& walker)
{
    return walker.has(Mechanism::TypedOperations) && walker.has(Mechanism::LoopInvariants) && walker.unit().role != CodeRole::SlowBody && walker.opensGuesses();
}

StringInBuilder* findStringInBuilder(Walker& walker, ValueID input)
{
    auto& texts = walker.stringsInBuilders();
    for (size_t place = walker.stringInBuilderSearchEnd(input); place--;) {
        if (texts[place].frameIndex < walker.topFrameIndex())
            break;
        if (findNameInTextBuilders(texts[place].web, input) && belongsToTopFrame(walker, texts[place]))
            return &texts[place];
    }
    return nullptr;
}

bool isBoundToStringInBuilder(Walker& walker, ValueID input)
{
    return walker.hasStringsInBuilders() && findStringInBuilder(walker, input) && isStringInBuilder(walker, walker.nameLastBoundTo(input));
}

static Reference builderBehindNameInTextBuilders(const Walker& walker, Reference bound)
{
    if (isStringInBuilder(walker, bound))
        return bound;
    if (!bound.isValue())
        return { };
    const Operation& made = walker.code().at(bound.id());
    return stringsMadeForReadersInTextBuilders.contains(made.opcode) ? Reference::value(made.builderOperand()) : Reference();
}

static StringInBuilder* findTextByRecordInTextBuilders(Walker& walker, Reference name)
{
    auto& texts = walker.stringsInBuilders();
    for (size_t place = texts.size(); place--;) {
        StringInBuilder& text = texts[place];
        if (!belongsToTopFrame(walker, text))
            continue;
        bool isNamed = text.builder == name || std::ranges::any_of(text.web.names, [&](const NameOfGrowingText& candidate) {
            return walker.nameLastBoundTo(candidate.value) == name;
        });
        if (isNamed)
            return &text;
    }
    return nullptr;
}

static StringInBuilder* findTextOfOperandInTextBuilders(Walker& walker, ValueID operand, Reference name)
{
    StringInBuilder* text = findStringInBuilder(walker, operand);
    if (!text)
        text = findTextByRecordInTextBuilders(walker, name);
    if (!text)
        walker.fail("a string in a builder is bound to a value that no scan has listed"_s);
    return text;
}

void typeBuildersAsStrings(Walker& walker, std::span<const Reference> entering, Vector<TypeID, 8>& entryTypes)
{
    for (size_t index = 0; index < entering.size(); ++index) {
        if (isStringInBuilder(walker, entering[index]))
            entryTypes[index] = TypeTable::string;
    }
}

TypeID typeOfRecordForLooks(const Walker& walker, TypeID type)
{
    if (!walker.hasStringsInBuilders() || !type || holdsOnlyPrimitives(type))
        return type;
    return walker.types().at(type).isOpaque(OpaqueKind::StringBuilder) ? TypeTable::string : type;
}

static bool comesRoundLoopAroundAsStringInTextBuilders(Walker& walker, const BodyIndex& index, BlockID inputHead, uint32_t parameterIndex)
{
    LoopID loop = index.loopOf(inputHead);
    if (!index.loop(loop).parent)
        return false;
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, inputHead)) {
        std::span<const ValueID> arguments = edge.target.arguments();
        if (index.isInLoop(edge.from, loop) || parameterIndex >= arguments.size() || !arguments[parameterIndex])
            continue;
        ValueID front = index.stripCasts(arguments[parameterIndex]);
        while (!index.isParameter(front) && operationsThatJoinTexts.contains(index.opcodeOf(front)) && index.value(front).operand(0))
            front = index.stripCasts(index.value(front).operand(0));
        if (!index.isParameter(front))
            continue;
        BlockID holder = index.blockOf(front);
        if (index.isLoopHead(holder) && index.isInLoop(inputHead, index.loopOf(holder)))
            return true;
    }
    return false;
}

static bool passesQuestionsInTextBuilders(Walker& walker, BlockID inputHead, const GrowingText& web)
{
    return !web.standsUnderHandlers || mayKeepTextUnderHandlers(walker, inputHead, web);
}

struct WaysOutOfLoopInTextBuilders {
    struct Way {
        BlockID block;
        ValueID returned;
    };

    WaysOutOfLoopInTextBuilders(const BodyIndex& index, BlockID loopHead)
    {
        liveness.compute(index);
        LoopID loop = index.loopOf(loopHead);
        auto list = [&](BlockID block) {
            if (!index.isInLoop(block, loop))
                ways.append({ block, { } });
        };
        for (BlockID member : index.membersOf(loop)) {
            BodyBlock block = index.block(member);
            BodyTerminator terminator = block.terminator();
            for (BodyTarget target : terminator.targets())
                list(target.block());
            if (block.hasHandler())
                list(block.handler().block());
            if (ValueID returned = terminator.kind() == TerminatorKind::Return ? terminator.operand(0) : ValueID())
                ways.append({ { }, returned });
        }
    }

    BodyLiveness liveness;
    Vector<Way, 4> ways;
};

static void bindNamesReadBehindLoopAnewInTextBuilders(Walker& walker, const BodyIndex& index, std::optional<WaysOutOfLoopInTextBuilders>& waysOut, const GrowingText& web)
{
    if (!waysOut)
        waysOut.emplace(index, web.loopHead);
    for (const WaysOutOfLoopInTextBuilders::Way& way : waysOut->ways) {
        if (way.returned) {
            if (findNameInTextBuilders(web, way.returned))
                walker.bindAnewAtEveryBlock(way.returned);
            continue;
        }
        forEachNameLiveAtHeadInTextBuilders(waysOut->liveness, web, way.block, [&](const NameOfGrowingText& name) {
            walker.bindAnewAtEveryBlock(name.value);
        });
    }
}

static constexpr size_t maxEnteringParts = 16;

static Vector<Reference, 8> collectEnteringParts(const Walker& walker, Reference enters)
{
    const Code& code = walker.code();
    Vector<Reference, 8> parts { enters };
    for (size_t place = 0; place < parts.size();) {
        ValueID root = parts[place].isValue() ? underlyingCellOf(code, parts[place].id()) : ValueID();
        if (!root || code.isConstant(root) || !operationsThatJoinTexts.contains(code.at(root).opcode)) {
            ++place;
            continue;
        }
        const Operation& join = code.at(root);
        auto hasOwnText = [&](ValueID part) {
            TypeID type = part ? walker.typeOf(code.isConstant(part) ? Reference::constant(part) : Reference::value(part)) : TypeID();
            return type && !isNeverType(type) && hasTextByItself(walker.types().at(type));
        };
        bool isConcatenation = std::ranges::all_of(join.operands(), hasOwnText) && std::ranges::any_of(join.operands(), [&](ValueID part) {
            return walker.types().at(walker.typeOf(code.isConstant(part) ? Reference::constant(part) : Reference::value(part))).isAnyString();
        });
        if (!isConcatenation || (join.opcode == Opcode::Strcat && join.isLikelyKey()) || parts.size() + join.numberOfOperands() > maxEnteringParts) {
            ++place;
            continue;
        }
        parts.removeAt(place);
        size_t end = place;
        for (ValueID part : join.operands())
            parts.insert(end++, code.isConstant(part) ? Reference::constant(part) : Reference::value(part));
    }
    return parts;
}

static bool isJoinFailureMovable(const Walker& walker, ValueID join)
{
    static constexpr unsigned maxBlocks = 4;
    const Code& code = walker.code();
    BlockID block = walker.currentBlock();
    for (unsigned blocks = 0; blocks < maxBlocks; ++blocks) {
        for (ValueID id = code.lastOperationOf(block); id; id = code.at(id).previous) {
            if (id == join)
                return true;
            if (!code.at(id).traits().isRemovableWhenUnused())
                return false;
        }
        BlockID before;
        unsigned numberOfEdges = 0;
        code.forEachIncomingEdgeUnsealed(block, [&](const Edge& edge) {
            before = edge.from;
            ++numberOfEdges;
        });
        if (numberOfEdges != 1 || code.at(before).terminator.kind != TerminatorKind::Jump)
            return false;
        block = before;
    }
    return false;
}

static ValueID findEnteringJoin(Walker& walker, const BodyIndex& index, BlockID inputHead, uint32_t parameterIndex, Reference enters)
{
    ValueID written;
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, inputHead)) {
        if (index.isInLoop(edge.from, index.loopOf(inputHead)))
            continue;
        std::span<const ValueID> arguments = edge.target.arguments();
        if (written || parameterIndex >= arguments.size() || !arguments[parameterIndex])
            return { };
        written = arguments[parameterIndex];
    }
    while (written && !index.isParameter(written) && index.hasOneUse(written) && opcodesThatStateTypes.contains(index.opcodeOf(written)))
        written = index.value(written).operand(0);
    if (!written || index.isParameter(written) || !index.hasOneUse(written) || !operationsThatJoinTexts.contains(index.opcodeOf(written)))
        return { };
    const Code& code = walker.code();
    ValueID root = underlyingCellOf(code, enters.id());
    if (code.isConstant(root) || !operationsThatJoinTexts.contains(code.at(root).opcode) || !code.at(root).traits().withoutThrowByRule().isRemovableWhenUnused())
        return { };
    return isJoinFailureMovable(walker, root) ? root : ValueID();
}

static StringInBuilder* beginTextInTextBuilders(Walker& walker, const BodyIndex& index, BlockID inputHead, std::optional<WaysOutOfLoopInTextBuilders>& waysOut, GrowingText&& web, Reference enters, Origin origin)
{
    Vector<Reference, 8> enteringParts = collectEnteringParts(walker, enters);
    ValueID enteringJoin = enteringParts.size() > 1 ? findEnteringJoin(walker, index, inputHead, web.parameterIndex, enters) : ValueID();
    StringInBuilder text;
    text.web = WTF::move(web);
    text.frameIndex = static_cast<uint32_t>(walker.topFrameIndex());
    text.liesInFrame = textBegunHereMayLieInFrame(walker, inputHead, text.web);

    Draft begin(text.liesInFrame ? Opcode::StackStringBuilderBegin : Opcode::StringBuilderBegin, origin);
    walker.chooseConservativeFields(begin);
    text.builder = walker.build(WTF::move(begin));
    if (!text.builder)
        return nullptr;

    auto& texts = walker.stringsInBuilders();
    texts.append(WTF::move(text));
    StringInBuilder& begun = texts.last();
    if (begun.liesInFrame) {
        Draft start(Opcode::StackStringBuilderStart, origin);
        walker.chooseConservativeFields(start);
        start.setBuilderOperand(begun.builder);
        if (!walker.build(WTF::move(start)))
            return nullptr;
    }

    begun.beganEmpty = textOfKnownPartOfStringBuilder(walker, enters) == walker.program().strings.emptyString();
    begun.beganAsText = walker.types().at(walker.typeOf(enters)).isAnyString();
    if (!begun.beganEmpty) {
        appendPartsToStringBuilder(walker, begun, begun.builder, enteringParts.span(), origin);
        if (walker.isUnreachable())
            return nullptr;
        if (enteringJoin && !walker.hasFailed())
            walker.code().letJoinFailWhereItsReaderFails(enteringJoin);
    }
    bindNamesReadBehindLoopAnewInTextBuilders(walker, index, waysOut, begun.web);
    return &begun;
}

void beginStringsInBuilders(Walker& walker, BlockID inputHead, LoopPreview& preview, Vector<Reference, 8>& entering, Vector<TypeID, 8>& entryTypes)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock head = index.block(inputHead);
    uint32_t numberOfParameters = head.numberOfParameters();
    bool someTextEntersInBuilder = walker.hasStringsInBuilders() && std::ranges::any_of(entering, [&](Reference reference) {
        return isStringInBuilder(walker, reference);
    });
    bool mayBegin = canKeepStringsInBuilders(walker) && !preview.effects.maySuspend;
    if (!mayBegin && !someTextEntersInBuilder)
        return;

    Vector<GrowingText, 2> webs;
    if (mayBegin)
        findGrowingTexts(walker, inputHead, preview, webs);
    if (webs.isEmpty() && !someTextEntersInBuilder)
        return;

    TypeID typeOfBuilder = walker.types().forOpaque(OpaqueKind::StringBuilder);
    Origin origin = walker.originOfOperation();
    origin.isTailCall = false;
    bool someTextMayGrow = false;
    auto letEnterInBuilder = [&](uint32_t parameterIndex, Reference builder) {
        entering[parameterIndex] = builder;
        entryTypes[parameterIndex] = typeOfBuilder;
        preview.typesOfParameters[parameterIndex] = typeOfBuilder;
        someTextMayGrow = true;
    };

    ASSERT(std::ranges::is_sorted(webs, { }, &GrowingText::parameterIndex));
    std::optional<WaysOutOfLoopInTextBuilders> waysOut;
    size_t nextWeb = 0;
    for (uint32_t parameterIndex = 0; parameterIndex < numberOfParameters; ++parameterIndex) {
        Reference enters = entering[parameterIndex];
        while (nextWeb < webs.size() && webs[nextWeb].parameterIndex < parameterIndex)
            ++nextWeb;
        size_t placeOfWeb = nextWeb < webs.size() && webs[nextWeb].parameterIndex == parameterIndex ? nextWeb : notFound;
        GrowingText* web = placeOfWeb != notFound && passesQuestionsInTextBuilders(walker, inputHead, webs[placeOfWeb]) ? &webs[placeOfWeb] : nullptr;

        if (isStringInBuilder(walker, enters)) {
            if (findStringInBuilder(walker, head.parameter(parameterIndex))) {
                letEnterInBuilder(parameterIndex, enters);
                continue;
            }
            StringInBuilder* before = findTextByRecordInTextBuilders(walker, enters);
            if (!before) {
                walker.fail("a string enters a loop in a builder that was never begun"_s);
                return;
            }
            if (std::exchange(before->waitsForNextLoop, false))
                before->headThatTookIt = inputHead;
            if (web && (!before->liesInFrame || textBegunHereMayLieInFrame(walker, inputHead, *web)) && (!web->beginsAnewOnSomeEdge || (before->liesInFrame && walker.hasOpenTrialVisit(walker.topFrameIndex(), inputHead)))) {
                StringInBuilder text;
                text.web = WTF::move(*web);
                text.frameIndex = before->frameIndex;
                text.builder = before->builder;
                text.beganAsText = before->beganAsText;
                text.liesInFrame = before->liesInFrame;
                walker.stringsInBuilders().append(WTF::move(text));
                bindNamesReadBehindLoopAnewInTextBuilders(walker, index, waysOut, walker.stringsInBuilders().last().web);
                letEnterInBuilder(parameterIndex, enters);
                continue;
            }
            enters = finishStringInBuilder(walker, *before, enters, origin);
            if (!enters)
                return;
            entering[parameterIndex] = enters;
            entryTypes[parameterIndex] = walker.code().at(enters.id()).type;
        }

        if (!web || !enters || !enters.isRecord())
            continue;
        Type typeOfWhatEnters = walker.types().at(walker.typeOf(enters));
        if (!typeOfWhatEnters.isAnyString() && (web->beginsAnewOnSomeEdge || !web->isOnlyConcatenated || !hasTextByItself(typeOfWhatEnters) || !walker.hasOpenTrialVisit(walker.topFrameIndex(), inputHead)))
            continue;
        if (comesRoundLoopAroundAsStringInTextBuilders(walker, index, inputHead, parameterIndex))
            continue;
        if (web->beginsAnewOnSomeEdge && (!walker.hasOpenTrialVisit(walker.topFrameIndex(), inputHead) || !textBegunHereMayLieInFrame(walker, inputHead, *web)))
            continue;
        StringInBuilder* begun = beginTextInTextBuilders(walker, index, inputHead, waysOut, WTF::move(*web), enters, origin);
        if (!begun)
            return;
        letEnterInBuilder(parameterIndex, begun->builder);
    }

    if (someTextMayGrow)
        preview.effects.merge(declaredBoundInTable(Opcode::StringBuilderAppendValue));
}

bool bindCastOfStringInBuilder(Walker& walker, const BodyValue& value)
{
    StringInBuilder* text = findStringInBuilder(walker, value.id());
    const NameOfGrowingText* name = text ? findNameInTextBuilders(text->web, value.id()) : nullptr;
    if (!name || name->version == value.id())
        return false;

    Reference builder;
    value.forEachOperand([&](ValueID operand, uint32_t) {
        if (!builder && operand && findNameInTextBuilders(text->web, operand))
            builder = builderBehindNameInTextBuilders(walker, walker.nameLastBoundTo(operand));
    });
    if (!builder)
        return false;
    walker.bind(value.id(), builder);
    return true;
}

bool isAppendToStringInBuilder(Walker& walker, const BodyValue& value)
{
    StringInBuilder* text = walker.hasStringsInBuilders() ? findStringInBuilder(walker, value.id()) : nullptr;
    return text && findNameInTextBuilders(text->web, value.id())->version == value.id();
}

MaterializedStringsScope::MaterializedStringsScope(Walker& walker, const BodyValue& value)
    : m_walker(walker)
{
    StringInBuilder* named = findStringInBuilder(walker, value.id());
    m_namesText = !!named;
    Origin origin = walker.originOf(value);
    origin.isTailCall = false;
    auto bindForLife = [&](ValueID operand, Reference bound, Reference reference) {
        m_boundBefore.append({ operand, bound });
        walker.bind(operand, reference);
    };

    value.forEachOperand([&](ValueID operand, uint32_t) {
        if (!operand || m_hasBoundOperation || walker.isUnreachable())
            return;
        Reference bound = walker.nameLastBoundTo(operand);
        if (named && findNameInTextBuilders(named->web, operand)) {
            Reference builder = builderBehindNameInTextBuilders(walker, bound);
            if (builder && builder != bound)
                bindForLife(operand, bound, builder);
            return;
        }
        if (!isStringInBuilder(walker, bound))
            return;
        StringInBuilder* text = findTextOfOperandInTextBuilders(walker, operand, bound);
        if (!text)
            return;
        if (readsTextWithoutString(walker, value, *text, bound)) {
            m_hasBoundOperation = true;
            return;
        }
        if (Reference string = materializeStringInBuilder(walker, *text, bound, origin))
            bindForLife(operand, bound, string);
    });
}

bool terminatorUsesStringInBuilder(Walker& walker, const BodyTerminator& terminator)
{
    bool takesText = false;
    terminator.forEachOperand([&](ValueID operand, uint32_t) {
        takesText |= operand && isStringInBuilder(walker, walker.nameLastBoundTo(operand));
    });
    return takesText;
}

bool Walker::branchOnLengthOfSwitchedString(const BodyTerminator& terminator)
{
    auto overStrings = terminator.as<TerminatorKind::SwitchString>();
    ValueID subject = overStrings.subjectOperand();
    Reference name = nameLastBoundTo(subject);
    if (!isStringInBuilder(*this, name) || isUnreachable())
        return false;
    StringInBuilder* text = findTextOfOperandInTextBuilders(*this, subject, name);
    if (!text || (text->stringOfState && text->blockOfState == currentBlock()))
        return false;
    Vector<StringID, 16> cases;
    for (StringID written : overStrings.strings())
        cases.append(written);
    if (cases.isEmpty())
        return false;

    unsigned shortest = std::numeric_limits<unsigned>::max();
    unsigned longest = 0;
    for (StringID written : cases) {
        unsigned length = lengthOfTextForStrings(*this, written);
        shortest = std::min(shortest, length);
        longest = std::max(longest, length);
    }
    Origin origin = m_origin;
    origin.isTailCall = false;
    Reference length = lengthOfStringInBuilder(*this, *text, name, origin);
    if (!length)
        return true;
    count(StatisticID::BuilderSwitchLengthChecks);
    BlockID otherwise = addBlock();
    auto leaveIf = [&](Reference isOutside) {
        if (!isOutside)
            return;
        BlockID inside = addBlock();
        branch(isOutside, otherwise, inside);
        continueIn(inside);
    };
    if (shortest)
        leaveIf(buildIntegerOperator(*this, Opcode::Less, length, integerForStrings(*this, shortest), origin));
    if (!isUnreachable())
        leaveIf(buildIntegerOperator(*this, Opcode::Less, integerForStrings(*this, longest), length, origin));
    Vector<BlockID, 16> matches;
    for (size_t place = 0; place < cases.size(); ++place)
        matches.append(addBlock());
    static constexpr size_t mostCasesCompared = 24;
    bool comparesInPlace = cases.size() <= mostCasesCompared && std::ranges::all_of(cases, [&](StringID written) { return canCompareBuilderWithLiteral(*this, *text, written); });
    if (Reference isOnStack = comparesInPlace && !isUnreachable() ? buildBuilderIsOnStack(*this, name, origin) : Reference()) {
        BlockID onStack = addBlock();
        BlockID elsewhere = addBlock(BlockFrequency::Unlikely);
        branch(isOnStack, onStack, elsewhere);
        continueIn(onStack);
        Vector<int64_t, 16> lengths;
        Vector<BlockID, 16> casesOfLength;
        for (StringID written : cases) {
            int64_t caseLength = lengthOfTextForStrings(*this, written);
            if (lengths.contains(caseLength))
                continue;
            lengths.append(caseLength);
            casesOfLength.append(addBlock());
        }
        switchOn(length, lengths.span(), casesOfLength.span(), otherwise);
        for (size_t group = 0; group < lengths.size(); ++group) {
            continueIn(casesOfLength[group]);
            for (size_t place = 0; place < cases.size() && !isUnreachable(); ++place) {
                if (static_cast<int64_t>(lengthOfTextForStrings(*this, cases[place])) != lengths[group])
                    continue;
                Reference isEqual = buildBuilderEqualsLiteral(*this, name, cases[place], origin);
                if (!isEqual)
                    break;
                BlockID next = addBlock();
                branch(isEqual, matches[place], next);
                continueIn(next);
            }
            if (!isUnreachable())
                jumpTo(otherwise);
        }
        continueIn(elsewhere);
        text->stringOfState = { };
    }
    if (Reference string = isUnreachable() ? Reference() : materializeStringInBuilder(*this, *text, name, origin)) {
        TerminatorDraft compare(TerminatorKind::SwitchString, origin);
        compare.setSubjectOperand(string);
        compare.setStrings(cases.span());
        endWithSwitchOverOwnBlocks(compare, matches.span(), otherwise);
    }
    for (size_t place = 0; place < matches.size() && !m_hasFailed; ++place) {
        continueIn(matches[place]);
        EdgeToTargetOfBody match { terminator.block(), static_cast<uint32_t>(place) };
        endBlockLeadingTo(TerminatorDraft(TerminatorKind::Jump, origin), singleElementSpan(match));
    }
    if (m_hasFailed)
        return true;
    continueIn(otherwise);
    EdgeToTargetOfBody noMatch { terminator.block(), static_cast<uint32_t>(cases.size()) };
    endBlockLeadingTo(TerminatorDraft(TerminatorKind::Jump, origin), singleElementSpan(noMatch));
    return true;
}

MaterializedStringsScope::MaterializedStringsScope(Walker& walker, const BodyTerminator& terminator)
    : m_walker(walker)
{
    terminator.forEachOperand([&](ValueID operand, uint32_t) {
        if (!operand || walker.isUnreachable())
            return;
        Reference bound = walker.nameLastBoundTo(operand);
        if (!isStringInBuilder(walker, bound))
            return;
        if (Reference read = referenceOfOperandOfTerminator(walker, operand)) {
            m_boundBefore.append({ operand, bound });
            walker.bind(operand, read);
        }
    });
}

MaterializedStringsScope::~MaterializedStringsScope()
{
    for (auto [operand, bound] : m_boundBefore)
        m_walker.bind(operand, bound);
}

Reference referenceOfOperandOfTerminator(Walker& walker, ValueID operand)
{
    Reference name = walker.nameLastBoundTo(operand);
    if (!isStringInBuilder(walker, name))
        return walker.referenceOf(operand);
    StringInBuilder* text = findTextOfOperandInTextBuilders(walker, operand, name);
    if (!text)
        return { };

    Origin origin = walker.originOfOperation();
    if (walker.currentBodyIndex().terminator(walker.inputBlockBeingWalked()).kind() == TerminatorKind::Branch) {
        if (Reference truth = buildTruthinessOfStringInBuilder(walker, *text, name, origin))
            return truth;
    }
    return materializeStringInBuilder(walker, *text, name, origin);
}

Reference materializeStringInBuilder(Walker& walker, StringInBuilder& text, Reference name, Origin origin)
{
    origin.isTailCall = false;
    if (walker.topFrameIndex() == text.frameIndex) {
        const BodyIndex& index = walker.currentBodyIndex();
        if (!index.isInLoop(walker.inputBlockBeingWalked(), index.loopOf(text.web.loopHead))) {
            walker.fail("a string in a builder is read outside the loop that grows it"_s);
            return { };
        }
    }
    if (text.blockOfState != walker.currentBlock()) {
        text.stringOfState = { };
        text.lengthOfState = { };
        text.blockOfState = walker.currentBlock();
    }
    if (text.stringOfState)
        return text.stringOfState;

    Draft string(Opcode::StringBuilderToString, origin);
    walker.chooseConservativeFields(string);
    string.setBuilderOperand(name);
    becomeOperationOfStringBuilderInFrame(walker, text, name, string, Opcode::StackStringBuilderToString);
    text.stringOfState = walker.build(WTF::move(string));
    return text.stringOfState;
}

static Reference finishInTextBuilders(Walker&, StringInBuilder&, Reference name, Origin);

static void appendPartsBehindFrontInTextBuilders(Walker& walker, StringInBuilder& text, Reference name, std::span<const Reference> parts, Origin origin)
{
    if (text.web.standsUnderHandlers)
        appendPartsRolledBackWhereTheyFail(walker, text, name, parts.subspan(1), origin);
    else
        appendPartsToStringBuilder(walker, text, name, parts.subspan(1), origin);
}

bool ensureAppendIsConcatenation(Walker& walker, const StringInBuilder& text, std::span<const Reference> partsBehindFront)
{
    if (text.beganAsText)
        return true;
    Reference first = partsBehindFront.empty() ? Reference() : partsBehindFront.front();
    bool firstJoinGivesText = first && walker.types().at(walker.typeOf(first)).isAnyString();
    if (!firstJoinGivesText)
        walker.walkLoopAgainWithoutBuilders(text);
    return firstJoinGivesText;
}

bool tryAppendJoinToStringInBuilder(Walker& walker, const BodyValue& join, Vector<Reference, 8>& parts)
{
    if (!walker.hasStringsInBuilders() || parts.isEmpty())
        return false;
    Origin origin = walker.originOf(join);
    origin.isTailCall = false;

    StringInBuilder* grown = isAppendToStringInBuilder(walker, join) ? findStringInBuilder(walker, join.id()) : nullptr;
    Reference name = grown ? builderBehindNameInTextBuilders(walker, parts[0]) : Reference();
    for (size_t place = name ? 1 : 0; place < parts.size(); ++place) {
        if (!isStringInBuilder(walker, parts[place]))
            continue;
        StringInBuilder* read = findTextByRecordInTextBuilders(walker, parts[place]);
        if (!read) {
            walker.fail("a string in a builder is bound to a value that no scan has listed"_s);
            return true;
        }
        parts[place] = materializeStringInBuilder(walker, *read, parts[place], origin);
        if (!parts[place])
            return true;
    }
    if (!name)
        return false;
    if (!ensureAppendIsConcatenation(walker, *grown, parts.span().subspan(1)))
        return true;

    appendPartsBehindFrontInTextBuilders(walker, *grown, name, parts.span(), origin);
    if (!walker.isUnreachable())
        walker.bind(join.id(), name);
    return true;
}

static ValueID findFinishBeforeJoin(const Walker& walker, Reference string)
{
    static constexpr unsigned maxBlocks = 4;
    static constexpr OpcodeSet finishes { Opcode::StackStringBuilderFinish, Opcode::StringBuilderFinish };
    const Code& code = walker.code();
    ValueID finish = string.isValue() ? underlyingCellOf(code, string.id()) : ValueID();
    if (!finish || code.isConstant(finish) || !finishes.contains(code.at(finish).opcode))
        return { };
    BlockID block = walker.currentBlock();
    for (unsigned blocks = 0; blocks < maxBlocks; ++blocks) {
        for (ValueID id = code.lastOperationOf(block); id; id = code.at(id).previous) {
            if (id == finish)
                return finish;
            const Operation& record = code.at(id);
            bool readsString = std::ranges::any_of(record.operands(), [&](ValueID operand) {
                return operand && !code.isConstant(operand) && underlyingCellOf(code, operand) == finish;
            });
            if (OperandPosition::builder(record.opcode) || record.traits().maySuspend() || (readsString && !cellPreservingOpcodes.contains(record.opcode)))
                return { };
        }
        BlockID before;
        unsigned numberOfEdges = 0;
        bool passesString = false;
        code.forEachIncomingEdgeUnsealed(block, [&](const Edge& edge) {
            before = edge.from;
            ++numberOfEdges;
            passesString |= std::ranges::any_of(edge.arguments(), [&](ValueID argument) {
                return argument && !code.isConstant(argument) && underlyingCellOf(code, argument) == finish;
            });
        });
        if (numberOfEdges != 1 || passesString || code.at(before).terminator.kind != TerminatorKind::Jump)
            return { };
        block = before;
    }
    return { };
}

static bool isFinalStringDead(Walker& walker, const BodyValue& firstJoin, const BodyValue& lastJoin, ValueID finish)
{
    const BodyIndex& index = walker.currentBodyIndex();
    const Code& code = walker.code();
    auto namesString = [&](ValueID value) {
        Reference bound = value ? walker.nameLastBoundTo(value) : Reference();
        return bound && bound.isValue() && underlyingCellOf(code, bound.id()) == finish;
    };
    BodyBlock block = index.block(index.blockOf(firstJoin.id()));
    if (block.hasHandler() || index.blockOf(lastJoin.id()) != block.id() || walker.standsUnderHandler())
        return false;
    bool isRead = false;
    for (uint32_t id = firstJoin.id().index() + 1; id < block.endOfValues().index(); ++id) {
        index.value(ValueID(id)).forEachOperand([&](ValueID operand, uint32_t) {
            isRead |= namesString(operand);
        });
    }
    BodyTerminator terminator = block.terminator();
    terminator.forEachOperand([&](ValueID operand, uint32_t) {
        isRead |= namesString(operand);
    });
    BodyLiveness liveness;
    liveness.compute(index);
    for (BodyTarget target : terminator.targets()) {
        for (ValueID argument : target.arguments())
            isRead |= namesString(argument);
        isRead |= !target.proofs().empty();
        liveness.forEachLiveAtHead(target.block(), [&](ValueID value) {
            isRead |= namesString(value);
        });
    }
    return !isRead;
}

bool tryAppendJoinToFinishedString(Walker& walker, const BodyValue& firstJoin, const BodyValue& lastJoin, std::span<const Reference> parts)
{
    if (parts.size() < 2 || !canKeepStringsInBuilders(walker) || walker.isUnreachable())
        return false;
    ValueID finish = findFinishBeforeJoin(walker, parts[0]);
    if (!finish || !isFinalStringDead(walker, firstJoin, lastJoin, finish))
        return false;
    for (Reference part : parts.subspan(1)) {
        TypeID type = part ? walker.typeOf(part) : TypeID();
        if (!type || isStringInBuilder(walker, part) || !hasTextByItself(walker.types().at(type)) || (part.isValue() && underlyingCellOf(walker.code(), part.id()) == finish))
            return false;
    }

    Origin origin = walker.originOf(lastJoin);
    origin.isTailCall = false;
    StringInBuilder text;
    text.frameIndex = static_cast<uint32_t>(walker.topFrameIndex());
    text.builder = Reference::value(walker.code().at(finish).builderOperand());
    text.liesInFrame = walker.code().at(finish).opcode == Opcode::StackStringBuilderFinish;
    walker.code().turnFinishIntoUnusedString(finish);
    appendPartsToStringBuilder(walker, text, text.builder, parts.subspan(1), origin);
    if (walker.isUnreachable() || walker.hasFailed())
        return true;
    if (Reference joined = finishInTextBuilders(walker, text, text.builder, origin))
        walker.bind(lastJoin.id(), joined);
    return true;
}

template<typename Functor>
static void forEachTextLeftOnEdgeInTextBuilders(Walker& walker, BlockID from, BlockID to, const Functor& functor)
{
    const BodyIndex& index = walker.currentBodyIndex();
    auto& texts = walker.stringsInBuilders();
    BitVector parametersOfHeadsSeen;
    for (uint32_t place : walker.stringsInBuildersAtLoopExit(from, to)) {
        StringInBuilder& text = texts[place];
        if (!belongsToTopFrame(walker, text))
            continue;
        LoopID loop = index.loopOf(text.web.loopHead);
        if (!index.isInLoop(from, loop) || index.isInLoop(to, loop) || parametersOfHeadsSeen.set(text.web.names.first().value.index()))
            continue;
        if (functor(text) == IterationStatus::Done)
            return;
    }
}

bool edgeExitsBuilderLoop(Walker& walker, BlockID to)
{
    bool leaves = false;
    forEachTextLeftOnEdgeInTextBuilders(walker, walker.inputBlockBeingWalked(), to, [&](StringInBuilder&) {
        leaves = true;
        return IterationStatus::Done;
    });
    return leaves;
}

template<typename Functor>
static void forEachTextBegunAnewOnEdgeInTextBuilders(Walker& walker, BlockID to, std::span<const Reference> arguments, const Functor& functor)
{
    BodyBlock entered = walker.currentBodyIndex().block(to);
    for (uint32_t index = 0; index < arguments.size() && index < entered.numberOfParameters(); ++index) {
        if (!arguments[index] || isStringInBuilder(walker, arguments[index]))
            continue;
        StringInBuilder* text = findStringInBuilder(walker, entered.parameter(index));
        if (text && text->web.beginsAnewOnSomeEdge)
            functor(*text, index);
    }
}

bool edgeResetsStringInBuilder(Walker& walker, BlockID to, std::span<const Reference> arguments)
{
    bool begins = false;
    forEachTextBegunAnewOnEdgeInTextBuilders(walker, to, arguments, [&](StringInBuilder&, uint32_t) {
        begins = true;
    });
    return begins;
}

void resetStringsInBuildersOnEdge(Walker& walker, BlockID to, Vector<Reference, 8>& arguments)
{
    Origin origin = walker.originOfOperation();
    origin.isTailCall = false;
    Vector<std::pair<StringInBuilder*, uint32_t>, 2> begun;
    forEachTextBegunAnewOnEdgeInTextBuilders(walker, to, arguments.span(), [&](StringInBuilder& text, uint32_t index) {
        begun.append({ &text, index });
    });
    for (auto [text, index] : begun) {
        Reference begins = arguments[index];
        Type typeOfBeginning = walker.types().at(walker.typeOf(begins));
        if (!typeOfBeginning.isAnyString() || longestTextOf(walker.types(), typeOfBeginning) == JSString::MaxLength) {
            walker.walkLoopAgainWithoutBuilders(*text);
            return;
        }
        Reference name = walker.nameLastBoundTo(walker.currentBodyIndex().block(text->web.loopHead).parameter(text->web.parameterIndex));
        if (!isStringInBuilder(walker, name))
            name = text->builder;
        RELEASE_ASSERT(text->liesInFrame);
        Draft discard(Opcode::StackStringBuilderDiscard, origin);
        walker.chooseConservativeFields(discard);
        discard.setBuilderOperand(name);
        walker.build(WTF::move(discard));
        text->stringOfState = { };
        text->lengthOfState = { };
        if (walker.isUnreachable())
            return;
        if (textOfKnownPartOfStringBuilder(walker, begins) != walker.program().strings.emptyString()) {
            appendPartsToStringBuilder(walker, *text, name, singleElementSpan(begins), origin);
            if (walker.isUnreachable())
                return;
        }
        arguments[index] = name;
    }
}

template<typename Functor>
static void forEachTextHandedToReaderOnEdgeInTextBuilders(Walker& walker, BlockID to, std::span<const Reference> arguments, const Functor& functor)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock entered = index.block(to);
    for (uint32_t place = 0; place < arguments.size() && place < entered.numberOfParameters(); ++place) {
        if (!arguments[place] || !walker.someScanFoundParameterToReadText(entered.parameter(place)) || !isStringInBuilder(walker, arguments[place]))
            continue;
        StringInBuilder* text = findTextByRecordInTextBuilders(walker, arguments[place]);
        if (text && text->web.parametersThatReadText.contains(entered.parameter(place)))
            functor(*text, place);
    }
}

bool edgePassesStringInBuilder(Walker& walker, BlockID to, std::span<const Reference> writtenArguments)
{
    bool hands = false;
    forEachTextHandedToReaderOnEdgeInTextBuilders(walker, to, writtenArguments, [&](StringInBuilder&, uint32_t) {
        hands = true;
    });
    return hands;
}

void materializeStringsPassedOnEdge(Walker& walker, BlockID to, std::span<Reference> writtenArguments)
{
    Origin origin = walker.originOfOperation();
    origin.isTailCall = false;
    Vector<std::pair<StringInBuilder*, uint32_t>, 2> handed;
    forEachTextHandedToReaderOnEdgeInTextBuilders(walker, to, writtenArguments, [&](StringInBuilder& text, uint32_t place) {
        handed.append({ &text, place });
    });
    for (auto [text, place] : handed) {
        Reference string = materializeStringInBuilder(walker, *text, writtenArguments[place], origin);
        if (!string || walker.isUnreachable())
            return;
        writtenArguments[place] = string;
    }
}

static Reference finishInTextBuilders(Walker& walker, StringInBuilder& text, Reference name, Origin origin)
{
    origin.isTailCall = false;
    text.stringOfState = { };
    text.lengthOfState = { };
    if (handlerReadsStringInBuilder(walker, text))
        return materializeStringInBuilder(walker, text, name, origin);
    if (text.liesInFrame)
        return finishTextInFrame(walker, name, origin);

    Draft finish(Opcode::StringBuilderFinish, origin);
    walker.chooseConservativeFields(finish);
    finish.setBuilderOperand(name);
    return walker.build(WTF::move(finish));
}

Reference finishStringInBuilder(Walker& walker, StringInBuilder& text, Reference name, Origin origin)
{
    return finishInTextBuilders(walker, text, name, origin);
}

void discardStringInBuilder(Walker& walker, StringInBuilder& text, Reference name, Origin origin)
{
    origin.isTailCall = false;
    if (handlerReadsStringInBuilder(walker, text))
        return;
    Draft discard(text.liesInFrame ? Opcode::StackStringBuilderDiscard : Opcode::StringBuilderDiscard, origin);
    walker.chooseConservativeFields(discard);
    discard.setBuilderOperand(name);
    walker.build(WTF::move(discard));
}

static bool comesRoundAsJoinInTextBuilders(Walker& walker, const BodyIndex& index, BlockID head, uint32_t parameterIndex)
{
    LoopID loop = index.loopOf(head);
    BodyBlock block = index.block(head);
    ValueID parameter = block.parameter(parameterIndex);
    bool changesOnSomeEdge = false;
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, head)) {
        if (!index.isInLoop(edge.from, loop))
            continue;
        std::span<const ValueID> arguments = edge.target.arguments();
        if (parameterIndex >= arguments.size() || !arguments[parameterIndex])
            return false;
        ValueID root = index.stripCasts(arguments[parameterIndex]);
        if (root == parameter)
            continue;
        bool isMadeInLoop = root.index() >= block.firstValue().index();
        if (!isMadeInLoop || (!index.isParameter(root) && !operationsThatJoinTexts.contains(index.opcodeOf(root))))
            return false;
        changesOnSomeEdge = true;
    }
    return changesOnSomeEdge;
}

bool canStayInBuilderOnEdge(Walker& walker, const StringInBuilder& text, BlockID to, BodyTarget target)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (text.frameIndex != walker.topFrameIndex() || text.web.standsUnderHandlers || index.hasIrreducibleFlow())
        return false;
    LoopID left = index.loopOf(text.web.loopHead);
    if (!left || index.isInLoop(to, left))
        return false;
    if (walker.exitsLoopBeingVersioned(to))
        return false;
    LoopID around = index.loop(left).parent;

    Vector<ValueID, 4> handedOn;
    auto holdsText = [&](ValueID value) {
        return value && (findNameInTextBuilders(text.web, value) || handedOn.contains(value));
    };

    std::optional<uint32_t> taking;
    for (;;) {
        if (!target.proofs().empty())
            return false;
        taking = std::nullopt;
        std::span<const ValueID> arguments = target.arguments();
        for (uint32_t argumentIndex = 0; argumentIndex < arguments.size(); ++argumentIndex) {
            if (!holdsText(arguments[argumentIndex]))
                continue;
            if (taking)
                return false;
            taking = argumentIndex;
        }

        BodyBlock block = index.block(to);
        if (block.isHandlerBlock() || block.hasHandler() || (taking && *taking >= block.numberOfParameters()))
            return false;
        if (index.isLoopHead(to))
            break;
        if (index.loopOf(to) != around || index.numberOfIncomingEdges(to) != 1)
            return false;
        for (BodyValue operation : block.values()) {
            if (!declaredBoundInTable(operation.opcode()).isUnobservable())
                return false;
            bool mentionsText = false;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                mentionsText |= holdsText(operand);
            });
            if (mentionsText)
                return false;
        }
        BodyTerminator terminator = block.terminator();
        if (terminator.kind() != TerminatorKind::Jump)
            return false;
        if (taking)
            handedOn.append(block.parameter(*taking));
        target = terminator.next();
        to = target.block();
    }

    LoopID entered = index.loopOf(to);
    if (!taking || index.loop(entered).parent != around || index.isEnteredByHandlerEdge(to))
        return false;
    size_t numberOfEntries = std::ranges::count_if(walker.incomingEdgesOf(index, to), [&](const BodyIncomingEdge& edge) {
        return !index.isInLoop(edge.from, entered);
    });
    if (numberOfEntries != 1)
        return false;

    for (const NameOfGrowingText& name : text.web.names) {
        if (walker.isLiveAtHeadOf(name.value, to))
            return false;
    }
    for (ValueID parameter : handedOn) {
        if (walker.isLiveAtHeadOf(parameter, to))
            return false;
    }

    if (text.liesInFrame) {
        for (BlockID member : index.membersOf(entered)) {
            if (index.block(member).hasHandler())
                return false;
        }
    }
    return comesRoundAsJoinInTextBuilders(walker, index, to, *taking);
}

void finishStringsInBuildersOnEdge(Walker& walker, BlockID to, BodyTarget target, Vector<Reference, 8>& arguments, StringsFinishedOnEdge& taken)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BlockID from = walker.inputBlockBeingWalked();
    BodyLiveness liveness;
    liveness.compute(index);
    Origin origin = walker.originOfOperation();

    std::span<const ValueID> written = target.arguments();
    if (written.size() + target.proofs().size() != arguments.size()) {
        walker.fail("the arguments of an edge that exits the loop of a string in a builder differ from the arguments in the source"_s);
        return;
    }

    forEachTextLeftOnEdgeInTextBuilders(walker, from, to, [&](StringInBuilder& text) {
        if (canStayInBuilderOnEdge(walker, text, to, target)) {
            text.waitsForNextLoop = true;
            return IterationStatus::Continue;
        }

        Vector<Reference, 2> records;
        auto noteReader = [&](Reference record) {
            if (isStringInBuilder(walker, record) && !records.contains(record))
                records.append(record);
        };
        for (size_t place = 0; place < arguments.size(); ++place) {
            bool namesText = place < written.size() ? written[place] && findNameInTextBuilders(text.web, written[place]) : isStringInBuilder(walker, arguments[place]) && findTextByRecordInTextBuilders(walker, arguments[place]) == &text;
            if (namesText)
                noteReader(arguments[place]);
        }
        forEachNameLiveAtHeadInTextBuilders(liveness, text.web, to, [&](const NameOfGrowingText& name) {
            noteReader(walker.nameLastBoundTo(name.value));
            taken.names.append(name.value);
        });

        if (records.isEmpty()) {
            if (isStringInBuilder(walker, walker.nameLastBoundTo(text.web.names.first().value)))
                discardStringInBuilder(walker, text, text.builder, origin);
            return walker.isUnreachable() ? IterationStatus::Done : IterationStatus::Continue;
        }

        Reference string = finishInTextBuilders(walker, text, records.first(), origin);
        if (!string)
            return IterationStatus::Done;
        for (Reference record : records) {
            taken.strings.append({ record, string });
            for (Reference& argument : arguments) {
                if (argument == record)
                    argument = string;
            }
        }
        return IterationStatus::Continue;
    });
}

void materializeReturnedStringInBuilder(Walker& walker, const BodyTerminator& terminator)
{
    ValueID returned = terminator.kind() == TerminatorKind::Return ? terminator.operand(0) : ValueID();
    Reference name = returned ? walker.nameLastBoundTo(returned) : Reference();
    if (!isStringInBuilder(walker, name))
        return;
    StringInBuilder* text = findTextOfOperandInTextBuilders(walker, returned, name);
    if (!text)
        return;
    if (Reference string = materializeStringInBuilder(walker, *text, name, walker.originOfOperation()))
        walker.bind(returned, string);
}

bool Walker::isLiveAtHeadOf(ValueID value, BlockID inputBlock)
{
    const BodyIndex& index = *m_frames.last().index;
    if (!m_liveness.isComputedFor(index))
        m_liveness.compute(index);
    return m_liveness.isLiveAtHead(value, inputBlock);
}

const StringInBuilderIndex& Walker::stringInBuilderIndex()
{
    StringInBuilderIndex& places = m_stringInBuilderIndex;
    for (uint32_t place = places.endsBeforeInSameLoop.size(); place < m_stringsInBuilders.size(); ++place) {
        const StringInBuilder& text = std::as_const(m_stringsInBuilders)[place];
        uint64_t frame = static_cast<uint64_t>(text.frameIndex) << 32;
        places.before.append({ text.frameIndex < places.endsByFrame.size() ? places.endsByFrame[text.frameIndex] : 0, static_cast<uint32_t>(places.endsByNameBefore.size()) });
        for (const NameOfGrowingText& name : text.web.names) {
            uint32_t& end = places.endsByName.add(frame | name.value.index(), 0).iterator->value;
            places.endsByNameBefore.append({ frame | name.value.index(), std::exchange(end, place + 1) });
        }
        uint32_t& endInSameLoop = places.endsByLoopHead.add(frame | text.web.loopHead.index(), 0).iterator->value;
        places.endsBeforeInSameLoop.append(std::exchange(endInSameLoop, place + 1));
        while (places.endsByFrame.size() <= text.frameIndex)
            places.endsByFrame.append(0);
        places.endsByFrame[text.frameIndex] = place + 1;
    }
    return places;
}

void Walker::endStringsInBuildersFrom(size_t numberOfTexts)
{
    const auto& texts = std::as_const(m_stringsInBuilders);
    size_t kept = texts.size();
    while (kept > numberOfTexts && !texts[kept - 1].waitsForNextLoop)
        --kept;
    truncateStringInBuilderIndex(kept);
    while (texts.size() > kept)
        m_stringsInBuilders.removeLast();
    for (size_t place = numberOfTexts; place < kept; ++place) {
        if (!texts[place].waitsForNextLoop)
            m_stringsInBuilders[place].isNoLongerNeeded = true;
    }
}

void Walker::letGoOfTextsThatLoopTook(BlockID inputHead)
{
    const auto& texts = std::as_const(m_stringsInBuilders);
    for (size_t place = 0; place < texts.size(); ++place) {
        if (texts[place].headThatTookIt == inputHead && texts[place].frameIndex == m_frames.size() - 1 && !texts[place].waitsForNextLoop && !texts[place].isNoLongerNeeded)
            m_stringsInBuilders[place].isNoLongerNeeded = true;
    }
    size_t kept = texts.size();
    while (kept && texts[kept - 1].isNoLongerNeeded)
        --kept;
    truncateStringInBuilderIndex(kept);
    while (texts.size() > kept)
        m_stringsInBuilders.removeLast();
}

void Walker::walkLoopAgainWithoutBuilders(const StringInBuilder& text)
{
    keepTextsOfLoopOutOfBuilders(text.web.loopHead);
    bool isInsideLoopOfText = false;
    for (TrialVisit& trialVisit : m_trialVisits) {
        isInsideLoopOfText |= trialVisit.frameIndex == text.frameIndex && trialVisit.inputHead == text.web.loopHead;
        trialVisit.misfits |= isInsideLoopOfText;
    }
    fail("a text that began as no text grows by what is no text"_s);
}

void Walker::truncateStringInBuilderIndex(size_t size)
{
    StringInBuilderIndex& places = m_stringInBuilderIndex;
    while (places.endsBeforeInSameLoop.size() > size) {
        const StringInBuilder& text = std::as_const(m_stringsInBuilders)[places.endsBeforeInSameLoop.size() - 1];
        StringInBuilderIndex::Before before = places.before.takeLast();
        while (places.endsByNameBefore.size() > before.numberOfNamesBefore) {
            auto name = places.endsByNameBefore.takeLast();
            places.endsByName.set(name.first, name.second);
        }
        places.endsByLoopHead.set(static_cast<uint64_t>(text.frameIndex) << 32 | text.web.loopHead.index(), places.endsBeforeInSameLoop.takeLast());
        places.endsByFrame[text.frameIndex] = before.endOfFrame;
    }
}

static constexpr size_t minStringsInBuildersToIndex = 8;

size_t Walker::stringInBuilderSearchEnd(ValueID name)
{
    if (m_stringsInBuilders.size() < minStringsInBuildersToIndex)
        return m_stringsInBuilders.size();
    const StringInBuilderIndex& places = stringInBuilderIndex();
    size_t top = topFrameIndex();
    uint32_t end = places.endsByName.get(static_cast<uint64_t>(top) << 32 | name.index());
    auto endsOfFramesBelow = places.endsByFrame.span().first(std::min(top, places.endsByFrame.size()));
    return std::ranges::any_of(endsOfFramesBelow, [&](uint32_t endOfFrameBelow) { return endOfFrameBelow > end; }) ? 0 : end;
}

Vector<uint32_t, 8> Walker::stringsInBuildersAtLoopExit(BlockID from, BlockID to)
{
    Vector<uint32_t, 8> found;
    const BodyIndex& index = currentBodyIndex();
    LoopID innermost = index.loopOf(from);
    if (!innermost || index.isInLoop(to, innermost))
        return found;
    if (m_stringsInBuilders.size() < minStringsInBuildersToIndex) {
        for (uint32_t place = m_stringsInBuilders.size(); place--;)
            found.append(place);
        return found;
    }
    const StringInBuilderIndex& places = stringInBuilderIndex();
    uint64_t frame = static_cast<uint64_t>(topFrameIndex()) << 32;
    for (LoopID loop = innermost; loop && !index.isInLoop(to, loop); loop = index.loop(loop).parent) {
        for (uint32_t end = places.endsByLoopHead.get(frame | index.loop(loop).head.index()); end; end = places.endsBeforeInSameLoop[end - 1])
            found.append(end - 1);
    }
    std::ranges::sort(found, std::greater());
    return found;
}

void Walker::bindAnewAtEveryBlock(ValueID value)
{
    m_frames.last().addDuplicatedValue(value);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
