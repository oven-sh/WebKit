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
#include <algorithm>
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
    liveness.forEachLiveAtHead(block, web.names.first().value, ValueID(web.names.last().value.index() + 1), [&](ValueID value) {
        if (const NameOfGrowingText* name = findNameInTextBuilders(web, value))
            functor(*name);
    });
}

static bool belongsToFrameOnTopInTextBuilders(const Walker& walker, const TextInStringBuilder& text)
{
    if (text.frameIndex != walker.indexOfFrameOnTop() || !text.builder.isValue() || text.web.names.isEmpty())
        return false;
    uint32_t begunIn = walker.code().at(text.builder.id()).inlineFrame;
    auto frames = walker.code().inlineFrames();
    for (uint32_t frame = walker.originOfOperation().inlineFrame; frame != begunIn; frame = frames[frame].parent) {
        if (!frame)
            return false;
    }
    return true;
}

bool mayKeepTextsInStringBuilders(const Walker& walker)
{
    return walker.has(Mechanism::TypedOperations) && walker.has(Mechanism::LoopInvariants) && walker.unit().role != CodeRole::SlowBody && walker.opensGuesses();
}

bool isTextInStringBuilder(const Walker& walker, Reference reference)
{
    if (!reference.isValue())
        return false;
    TypeID type = walker.code().at(reference.id()).type;
    return type && !holdsOnlyPrimitives(type) && walker.types().at(type).isOpaque(OpaqueKind::StringBuilder);
}

TextInStringBuilder* findTextNamedBy(Walker& walker, ValueID input)
{
    Vector<TextInStringBuilder, 2>& texts = walker.textsInStringBuilders();
    for (size_t place = walker.endOfTextsInStringBuildersNamedBy(input); place--;) {
        if (texts[place].frameIndex < walker.indexOfFrameOnTop())
            break;
        if (findNameInTextBuilders(texts[place].web, input) && belongsToFrameOnTopInTextBuilders(walker, texts[place]))
            return &texts[place];
    }
    return nullptr;
}

bool isNameOfTextInStringBuilder(Walker& walker, ValueID input)
{
    return walker.hasTextsInStringBuilders() && findTextNamedBy(walker, input) && isTextInStringBuilder(walker, walker.nameLastBoundTo(input));
}

static Reference builderBehindNameInTextBuilders(const Walker& walker, Reference bound)
{
    if (isTextInStringBuilder(walker, bound))
        return bound;
    if (!bound.isValue())
        return { };
    const Operation& made = walker.code().at(bound.id());
    return stringsMadeForReadersInTextBuilders.contains(made.opcode) ? Reference::value(made.builderOperand()) : Reference();
}

static TextInStringBuilder* findTextByRecordInTextBuilders(Walker& walker, Reference name)
{
    Vector<TextInStringBuilder, 2>& texts = walker.textsInStringBuilders();
    for (size_t place = texts.size(); place--;) {
        TextInStringBuilder& text = texts[place];
        if (!belongsToFrameOnTopInTextBuilders(walker, text))
            continue;
        bool isNamed = text.builder == name || std::ranges::any_of(text.web.names, [&](const NameOfGrowingText& candidate) {
            return walker.nameLastBoundTo(candidate.value) == name;
        });
        if (isNamed)
            return &text;
    }
    return nullptr;
}

static TextInStringBuilder* findTextOfOperandInTextBuilders(Walker& walker, ValueID operand, Reference name)
{
    TextInStringBuilder* text = findTextNamedBy(walker, operand);
    if (!text)
        text = findTextByRecordInTextBuilders(walker, name);
    if (!text)
        walker.fail("a text in a builder is met under a value that no scan has named"_s);
    return text;
}

void typeTextsInStringBuildersForLooks(Walker& walker, std::span<const Reference> entering, Vector<TypeID, 8>& entryTypes)
{
    for (size_t index = 0; index < entering.size(); ++index) {
        if (isTextInStringBuilder(walker, entering[index]))
            entryTypes[index] = TypeTable::string;
    }
}

TypeID typeOfRecordForLooks(const Walker& walker, TypeID type)
{
    if (!walker.hasTextsInStringBuilders() || !type || holdsOnlyPrimitives(type))
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
        ValueID front = index.valueBehindCasts(arguments[parameterIndex]);
        while (!index.isParameter(front) && operationsThatJoinTexts.contains(index.opcodeOf(front)) && index.value(front).operand(0))
            front = index.valueBehindCasts(index.value(front).operand(0));
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

static void bindNamesReadBehindLoopAnewInTextBuilders(Walker& walker, const BodyIndex& index, const GrowingText& web)
{
    BodyLiveness liveness;
    liveness.compute(index);
    LoopID loop = index.loopOf(web.loopHead);
    auto bindNamesLiveAtHeadOf = [&](BlockID block) {
        if (index.isInLoop(block, loop))
            return;
        forEachNameLiveAtHeadInTextBuilders(liveness, web, block, [&](const NameOfGrowingText& name) {
            walker.bindAnewAtEveryBlock(name.value);
        });
    };
    for (BlockID member : index.membersOf(loop)) {
        BodyBlock block = index.block(member);
        BodyTerminator terminator = block.terminator();
        for (BodyTarget target : terminator.targets())
            bindNamesLiveAtHeadOf(target.block());
        if (block.hasHandler())
            bindNamesLiveAtHeadOf(block.handler().block());
        ValueID returned = terminator.kind() == TerminatorKind::Return ? terminator.operand(0) : ValueID();
        if (returned && findNameInTextBuilders(web, returned))
            walker.bindAnewAtEveryBlock(returned);
    }
}

static TextInStringBuilder* beginTextInTextBuilders(Walker& walker, const BodyIndex& index, BlockID inputHead, GrowingText&& web, Reference enters, Origin origin)
{
    TextInStringBuilder text;
    text.web = WTF::move(web);
    text.frameIndex = static_cast<uint32_t>(walker.indexOfFrameOnTop());
    text.liesInFrame = textBegunHereMayLieInFrame(walker, inputHead, text.web);

    Draft begin(text.liesInFrame ? Opcode::StackStringBuilderBegin : Opcode::StringBuilderBegin, origin);
    walker.chooseConservativeFields(begin);
    text.builder = walker.build(WTF::move(begin));
    if (!text.builder)
        return nullptr;

    Vector<TextInStringBuilder, 2>& texts = walker.textsInStringBuilders();
    texts.append(WTF::move(text));
    TextInStringBuilder& begun = texts.last();
    if (begun.liesInFrame) {
        Draft start(Opcode::StackStringBuilderStart, origin);
        walker.chooseConservativeFields(start);
        start.setBuilderOperand(begun.builder);
        if (!walker.build(WTF::move(start)))
            return nullptr;
    }

    begun.beganEmpty = textOfKnownPartOfStringBuilder(walker, enters) == walker.program().strings.emptyString();
    if (!begun.beganEmpty) {
        appendPartsToStringBuilder(walker, begun, begun.builder, singleElementSpan(enters), origin);
        if (walker.isUnreachable())
            return nullptr;
    }
    bindNamesReadBehindLoopAnewInTextBuilders(walker, index, begun.web);
    return &begun;
}

void beginTextsInStringBuilders(Walker& walker, BlockID inputHead, LoopPreview& preview, Vector<Reference, 8>& entering, Vector<TypeID, 8>& entryTypes)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock head = index.block(inputHead);
    uint32_t numberOfParameters = head.numberOfParameters();
    bool someTextEntersInBuilder = walker.hasTextsInStringBuilders() && std::ranges::any_of(entering, [&](Reference reference) {
        return isTextInStringBuilder(walker, reference);
    });
    bool mayBegin = mayKeepTextsInStringBuilders(walker) && !preview.effects.maySuspend;
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

    for (uint32_t parameterIndex = 0; parameterIndex < numberOfParameters; ++parameterIndex) {
        Reference enters = entering[parameterIndex];
        size_t placeOfWeb = webs.findIf([&](const GrowingText& candidate) {
            return candidate.parameterIndex == parameterIndex;
        });
        GrowingText* web = placeOfWeb != notFound && passesQuestionsInTextBuilders(walker, inputHead, webs[placeOfWeb]) ? &webs[placeOfWeb] : nullptr;

        if (isTextInStringBuilder(walker, enters)) {
            if (findTextNamedBy(walker, head.parameter(parameterIndex))) {
                letEnterInBuilder(parameterIndex, enters);
                continue;
            }
            TextInStringBuilder* before = findTextByRecordInTextBuilders(walker, enters);
            if (!before) {
                walker.fail("a text enters a loop in a builder that nobody has begun"_s);
                return;
            }
            if (web && (!before->liesInFrame || textBegunHereMayLieInFrame(walker, inputHead, *web))) {
                TextInStringBuilder text;
                text.web = WTF::move(*web);
                text.frameIndex = before->frameIndex;
                text.builder = before->builder;
                text.liesInFrame = before->liesInFrame;
                walker.textsInStringBuilders().append(WTF::move(text));
                bindNamesReadBehindLoopAnewInTextBuilders(walker, index, walker.textsInStringBuilders().last().web);
                letEnterInBuilder(parameterIndex, enters);
                continue;
            }
            enters = finishTextInStringBuilder(walker, *before, enters, origin);
            if (!enters)
                return;
            entering[parameterIndex] = enters;
            entryTypes[parameterIndex] = walker.code().at(enters.id()).type;
        }

        if (!web || !enters || !enters.isRecord())
            continue;
        Type typeOfWhatEnters = walker.types().at(walker.typeOf(enters));
        if (!typeOfWhatEnters.isAnyString() && (!web->isOnlyConcatenated || !hasTextByItself(typeOfWhatEnters)))
            continue;
        if (comesRoundLoopAroundAsStringInTextBuilders(walker, index, inputHead, parameterIndex))
            continue;
        TextInStringBuilder* begun = beginTextInTextBuilders(walker, index, inputHead, WTF::move(*web), enters, origin);
        if (!begun)
            return;
        letEnterInBuilder(parameterIndex, begun->builder);
    }

    if (someTextMayGrow)
        preview.effects.merge(declaredBoundInTable(Opcode::StringBuilderAppendValue));
}

bool bindCastOfTextInStringBuilder(Walker& walker, const BodyValue& value)
{
    TextInStringBuilder* text = findTextNamedBy(walker, value.id());
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

bool isGrowthOfTextInStringBuilder(Walker& walker, const BodyValue& value)
{
    TextInStringBuilder* text = walker.hasTextsInStringBuilders() ? findTextNamedBy(walker, value.id()) : nullptr;
    return text && findNameInTextBuilders(text->web, value.id())->version == value.id();
}

StringsInPlaceOfTextsInBuilders::StringsInPlaceOfTextsInBuilders(Walker& walker, const BodyValue& value)
    : m_walker(walker)
{
    TextInStringBuilder* named = findTextNamedBy(walker, value.id());
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
        if (!isTextInStringBuilder(walker, bound))
            return;
        if (value.opcode() == Opcode::ReleaseHint) {
            bindForLife(operand, bound, { });
            return;
        }
        TextInStringBuilder* text = findTextOfOperandInTextBuilders(walker, operand, bound);
        if (!text)
            return;
        if (readsTextWithoutString(walker, value, *text, bound)) {
            m_hasBoundOperation = true;
            return;
        }
        if (Reference string = stringOfTextInStringBuilder(walker, *text, bound, origin))
            bindForLife(operand, bound, string);
    });
}

bool terminatorTakesTextInStringBuilder(Walker& walker, const BodyTerminator& terminator)
{
    bool takesText = false;
    terminator.forEachOperand([&](ValueID operand, uint32_t) {
        takesText |= operand && isTextInStringBuilder(walker, walker.nameLastBoundTo(operand));
    });
    return takesText;
}

StringsInPlaceOfTextsInBuilders::StringsInPlaceOfTextsInBuilders(Walker& walker, const BodyTerminator& terminator)
    : m_walker(walker)
{
    terminator.forEachOperand([&](ValueID operand, uint32_t) {
        if (!operand || walker.isUnreachable())
            return;
        Reference bound = walker.nameLastBoundTo(operand);
        if (!isTextInStringBuilder(walker, bound))
            return;
        if (Reference read = referenceOfOperandOfTerminator(walker, operand)) {
            m_boundBefore.append({ operand, bound });
            walker.bind(operand, read);
        }
    });
}

StringsInPlaceOfTextsInBuilders::~StringsInPlaceOfTextsInBuilders()
{
    for (auto [operand, bound] : m_boundBefore)
        m_walker.bind(operand, bound);
}

Reference referenceOfOperandOfTerminator(Walker& walker, ValueID operand)
{
    Reference name = walker.nameLastBoundTo(operand);
    if (!isTextInStringBuilder(walker, name))
        return walker.referenceOf(operand);
    TextInStringBuilder* text = findTextOfOperandInTextBuilders(walker, operand, name);
    if (!text)
        return { };

    Origin origin = walker.originOfOperation();
    if (walker.currentBodyIndex().terminator(walker.inputBlockBeingWalked()).kind() == TerminatorKind::Branch) {
        if (Reference truth = truthOfTextInStringBuilder(walker, *text, name, origin))
            return truth;
    }
    return stringOfTextInStringBuilder(walker, *text, name, origin);
}

Reference stringOfTextInStringBuilder(Walker& walker, TextInStringBuilder& text, Reference name, Origin origin)
{
    origin.isTailCall = false;
    if (walker.indexOfFrameOnTop() == text.frameIndex) {
        const BodyIndex& index = walker.currentBodyIndex();
        if (!index.isInLoop(walker.inputBlockBeingWalked(), index.loopOf(text.web.loopHead))) {
            walker.fail("a text in a builder is read outside the loop in which it grows"_s);
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

static void appendPartsBehindFrontInTextBuilders(Walker& walker, TextInStringBuilder& text, Reference name, const BodyValue& join, std::span<const Reference> parts, Origin origin)
{
    if (text.web.standsUnderHandlers) {
        appendPartsRolledBackWhereTheyFail(walker, text, name, parts.subspan(1), origin);
        return;
    }
    Vector<ValueID, 8> operands;
    if (text.web.takesTextOfJSONStringify) {
        join.forEachOperand([&](ValueID operand, uint32_t) {
            operands.append(operand);
        });
    }
    if (operands.size() != parts.size()) {
        appendPartsToStringBuilder(walker, text, name, parts.subspan(1), origin);
        return;
    }
    for (size_t place = 1; place < parts.size() && !walker.isUnreachable(); ++place) {
        if (!appendJSONTextStraight(walker, text, name, operands[place], origin)) {
            appendPartsToStringBuilder(walker, text, name, parts.subspan(place, 1), origin);
            continue;
        }
        text.stringOfState = { };
        text.lengthOfState = { };
    }
}

bool appendRowToTextInStringBuilder(Walker& walker, const BodyValue& join, Vector<Reference, 8>& parts)
{
    if (!walker.hasTextsInStringBuilders() || parts.isEmpty())
        return false;
    Origin origin = walker.originOf(join);
    origin.isTailCall = false;

    TextInStringBuilder* grown = isGrowthOfTextInStringBuilder(walker, join) ? findTextNamedBy(walker, join.id()) : nullptr;
    Reference name = grown ? builderBehindNameInTextBuilders(walker, parts[0]) : Reference();
    for (size_t place = name ? 1 : 0; place < parts.size(); ++place) {
        if (!isTextInStringBuilder(walker, parts[place]))
            continue;
        TextInStringBuilder* read = findTextByRecordInTextBuilders(walker, parts[place]);
        if (!read) {
            walker.fail("a text in a builder is met under a value that no scan has named"_s);
            return true;
        }
        parts[place] = stringOfTextInStringBuilder(walker, *read, parts[place], origin);
        if (!parts[place])
            return true;
    }
    if (!name)
        return false;

    appendPartsBehindFrontInTextBuilders(walker, *grown, name, join, parts.span(), origin);
    if (!walker.isUnreachable())
        walker.bind(join.id(), name);
    return true;
}

bool handlerInForceReadsTextInStringBuilder(Walker&, const TextInStringBuilder&);

template<typename Functor>
static void forEachTextLeftOnEdgeInTextBuilders(Walker& walker, BlockID from, BlockID to, const Functor& functor)
{
    const BodyIndex& index = walker.currentBodyIndex();
    Vector<TextInStringBuilder, 2>& texts = walker.textsInStringBuilders();
    Vector<ValueID, 4> parametersOfHeadsSeen;
    for (uint32_t place : walker.placesOfTextsInStringBuildersLeftOnEdge(from, to)) {
        TextInStringBuilder& text = texts[place];
        if (!belongsToFrameOnTopInTextBuilders(walker, text))
            continue;
        LoopID loop = index.loopOf(text.web.loopHead);
        if (!index.isInLoop(from, loop) || index.isInLoop(to, loop) || parametersOfHeadsSeen.contains(text.web.names.first().value))
            continue;
        parametersOfHeadsSeen.append(text.web.names.first().value);
        if (functor(text) == IterationStatus::Done)
            return;
    }
}

bool edgeLeavesLoopOfTextInStringBuilder(Walker& walker, BlockID to)
{
    bool leaves = false;
    forEachTextLeftOnEdgeInTextBuilders(walker, walker.inputBlockBeingWalked(), to, [&](TextInStringBuilder&) {
        leaves = true;
        return IterationStatus::Done;
    });
    return leaves;
}

static Reference finishInTextBuilders(Walker& walker, TextInStringBuilder& text, Reference name, Origin origin)
{
    origin.isTailCall = false;
    text.stringOfState = { };
    text.lengthOfState = { };
    if (handlerInForceReadsTextInStringBuilder(walker, text))
        return stringOfTextInStringBuilder(walker, text, name, origin);
    if (text.liesInFrame)
        return finishTextInFrame(walker, name, origin);

    Draft finish(Opcode::StringBuilderFinish, origin);
    walker.chooseConservativeFields(finish);
    finish.setBuilderOperand(name);
    return walker.build(WTF::move(finish));
}

Reference finishTextInStringBuilder(Walker& walker, TextInStringBuilder& text, Reference name, Origin origin)
{
    return finishInTextBuilders(walker, text, name, origin);
}

void discardTextInStringBuilder(Walker& walker, TextInStringBuilder& text, Reference name, Origin origin)
{
    origin.isTailCall = false;
    if (handlerInForceReadsTextInStringBuilder(walker, text))
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
        ValueID root = index.valueBehindCasts(arguments[parameterIndex]);
        if (root == parameter)
            continue;
        bool isMadeInLoop = root.index() >= block.firstValue().index();
        if (!isMadeInLoop || (!index.isParameter(root) && !operationsThatJoinTexts.contains(index.opcodeOf(root))))
            return false;
        changesOnSomeEdge = true;
    }
    return changesOnSomeEdge;
}

bool textStaysInStringBuilderOnEdge(Walker& walker, const TextInStringBuilder& text, BlockID to, BodyTarget target)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (text.frameIndex != walker.indexOfFrameOnTop() || text.web.standsUnderHandlers || index.hasIrreducibleFlow())
        return false;
    LoopID left = index.loopOf(text.web.loopHead);
    if (!left || index.isInLoop(to, left))
        return false;
    if (walker.edgeLeavesLoopBeingBuiltTwice(to))
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

void takeTextsOutOfStringBuildersOnEdge(Walker& walker, BlockID to, BodyTarget target, Vector<Reference, 8>& arguments, TextsTakenOutOnEdge& taken)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BlockID from = walker.inputBlockBeingWalked();
    BodyLiveness liveness;
    liveness.compute(index);
    Origin origin = walker.originOfOperation();

    std::span<const ValueID> written = target.arguments();
    if (written.size() + target.proofs().size() != arguments.size()) {
        walker.fail("the arguments of an edge out of the loop of a text in a builder are not those that are written"_s);
        return;
    }

    forEachTextLeftOnEdgeInTextBuilders(walker, from, to, [&](TextInStringBuilder& text) {
        if (textStaysInStringBuilderOnEdge(walker, text, to, target))
            return IterationStatus::Continue;

        Vector<Reference, 2> records;
        auto noteReader = [&](Reference record) {
            if (isTextInStringBuilder(walker, record) && !records.contains(record))
                records.append(record);
        };
        for (size_t place = 0; place < arguments.size(); ++place) {
            bool namesText = place < written.size() ? written[place] && findNameInTextBuilders(text.web, written[place]) : isTextInStringBuilder(walker, arguments[place]) && findTextByRecordInTextBuilders(walker, arguments[place]) == &text;
            if (namesText)
                noteReader(arguments[place]);
        }
        forEachNameLiveAtHeadInTextBuilders(liveness, text.web, to, [&](const NameOfGrowingText& name) {
            noteReader(walker.nameLastBoundTo(name.value));
            taken.names.append(name.value);
        });

        if (records.isEmpty()) {
            Reference builder = walker.nameLastBoundTo(text.web.names.first().value);
            if (isTextInStringBuilder(walker, builder))
                discardTextInStringBuilder(walker, text, builder, origin);
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

void takeTextsOutOfStringBuildersAtEndOfWay(Walker& walker, const BodyTerminator& terminator)
{
    ValueID returned = terminator.kind() == TerminatorKind::Return ? terminator.operand(0) : ValueID();
    Reference name = returned ? walker.nameLastBoundTo(returned) : Reference();
    if (!isTextInStringBuilder(walker, name))
        return;
    TextInStringBuilder* text = findTextOfOperandInTextBuilders(walker, returned, name);
    if (!text)
        return;
    if (Reference string = stringOfTextInStringBuilder(walker, *text, name, walker.originOfOperation()))
        walker.bind(returned, string);
}

bool Walker::isLiveAtHeadOf(ValueID value, BlockID inputBlock)
{
    const BodyIndex& index = *m_frames.last().index;
    if (!m_liveness.isComputedFor(index))
        m_liveness.compute(index);
    return m_liveness.isLiveAtHead(value, inputBlock);
}

const PlacesOfTextsInStringBuilders& Walker::placesOfTextsInStringBuilders()
{
    PlacesOfTextsInStringBuilders& places = m_placesOfTextsInStringBuilders;
    for (uint32_t place = places.endsBeforeInSameLoop.size(); place < m_textsInStringBuilders.size(); ++place) {
        const TextInStringBuilder& text = m_textsInStringBuilders[place];
        uint64_t frame = static_cast<uint64_t>(text.frameIndex) << 32;
        for (const NameOfGrowingText& name : text.web.names)
            places.endsByName.set(frame | name.value.index(), place + 1);
        uint32_t& endInSameLoop = places.endsByLoopHead.add(frame | text.web.loopHead.index(), 0).iterator->value;
        places.endsBeforeInSameLoop.append(std::exchange(endInSameLoop, place + 1));
        while (places.endsByFrame.size() <= text.frameIndex)
            places.endsByFrame.append(0);
        places.endsByFrame[text.frameIndex] = place + 1;
    }
    return places;
}

static constexpr size_t minTextsInStringBuildersToIndex = 8;

size_t Walker::endOfTextsInStringBuildersNamedBy(ValueID name)
{
    if (m_textsInStringBuilders.size() < minTextsInStringBuildersToIndex)
        return m_textsInStringBuilders.size();
    const PlacesOfTextsInStringBuilders& places = placesOfTextsInStringBuilders();
    size_t top = indexOfFrameOnTop();
    uint32_t end = places.endsByName.get(static_cast<uint64_t>(top) << 32 | name.index());
    auto endsOfFramesBelow = places.endsByFrame.span().first(std::min(top, places.endsByFrame.size()));
    return std::ranges::any_of(endsOfFramesBelow, [&](uint32_t endOfFrameBelow) { return endOfFrameBelow > end; }) ? 0 : end;
}

Vector<uint32_t, 8> Walker::placesOfTextsInStringBuildersLeftOnEdge(BlockID from, BlockID to)
{
    Vector<uint32_t, 8> found;
    const BodyIndex& index = currentBodyIndex();
    LoopID innermost = index.loopOf(from);
    if (!innermost || index.isInLoop(to, innermost))
        return found;
    if (m_textsInStringBuilders.size() < minTextsInStringBuildersToIndex) {
        for (uint32_t place = m_textsInStringBuilders.size(); place--;)
            found.append(place);
        return found;
    }
    const PlacesOfTextsInStringBuilders& places = placesOfTextsInStringBuilders();
    uint64_t frame = static_cast<uint64_t>(indexOfFrameOnTop()) << 32;
    for (LoopID loop = innermost; loop && !index.isInLoop(to, loop); loop = index.loop(loop).parent) {
        for (uint32_t end = places.endsByLoopHead.get(frame | index.loop(loop).head.index()); end; end = places.endsBeforeInSameLoop[end - 1])
            found.append(end - 1);
    }
    std::ranges::sort(found, std::greater());
    return found;
}

void Walker::bindAnewAtEveryBlock(ValueID value)
{
    m_frames.last().listAsBuiltTwice(value);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
