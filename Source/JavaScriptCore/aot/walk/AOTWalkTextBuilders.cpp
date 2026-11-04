/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// TypedOperations with LoopInvariants: `for (..) s += "k" + i + ","`. A text that grows in a loop lies in a builder: every name of it is bound to the builder's record, a growth appends its parts,
// whoever else takes a name gets a string, and every edge out of the loop gives the builder back.
// At a suspension: no builder is begun for a loop that may suspend. A builder lives from the block in front of its loop to the edges out of it, so none is live where a coroutine suspends. At a throw: what leaves the unit leaves the builder to the collector, and under a handler no builder is begun unless mayKeepTextUnderHandlers().
// Withheld, either of the two: no builder is begun, and every join is built as it is without this file.

#include "config.h"
#include "AOTWalkTextBuilders.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet stringsMadeForReadersInTextBuilders { Opcode::StringBuilderToString, Opcode::StackStringBuilderToString };
static constexpr OpcodeSet appendsOfStringsInTextBuilders { Opcode::StringBuilderAppendString, Opcode::StackStringBuilderAppendString };
static_assert(accessesUnderComputedKeys == OpcodeSet { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas, Opcode::ElemDelete, Opcode::ElemDefine });
static_assert(OperandIndex::ElemGet::property == OperandIndex::ElemSet::property && OperandIndex::ElemGet::property == OperandIndex::ElemHas::property);
static_assert(OperandIndex::ElemGet::property == OperandIndex::ElemDelete::property && OperandIndex::ElemGet::property == OperandIndex::ElemDefine::property);

// ---- Names

static const NameOfGrowingText* findNameInTextBuilders(const GrowingText& web, ValueID value)
{
    if (web.names.isEmpty() || value < web.names.first().value || web.names.last().value < value)
        return nullptr;
    auto found = std::ranges::lower_bound(web.names, value, { }, &NameOfGrowingText::value);
    return found != web.names.end() && found->value == value ? &*found : nullptr;
}

// functor(const NameOfGrowingText&)
template<typename Functor>
static void forEachNameLiveAtHeadInTextBuilders(const BodyLiveness& liveness, const GrowingText& web, BlockID block, const Functor& functor)
{
    liveness.forEachLiveAtHead(block, web.names.first().value, ValueID(web.names.last().value.index() + 1), [&](ValueID value) {
        if (const NameOfGrowingText* name = findNameInTextBuilders(web, value))
            functor(*name);
    });
}

// A record stays when the frame of its body has gone, and the copy of another callee may stand at the same depth. No two copies have one frame of Code.
// The record that the copy of a callee has of a text of its caller names a builder that was begun further down.
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
    // A copy that is built for size is walked as a closed arm is.
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
    for (size_t place = texts.size(); place--;) {
        // A record of a frame further down was made while that frame was on top: it is older than the frame on top, and so is every record in front of it.
        if (texts[place].frameIndex < walker.indexOfFrameOnTop())
            break;
        if (findNameInTextBuilders(texts[place].web, input) && belongsToFrameOnTopInTextBuilders(walker, texts[place]))
            return &texts[place];
    }
    return nullptr;
}

bool isNameOfTextInStringBuilder(Walker& walker, ValueID input)
{
    return walker.hasTextsInStringBuilders() && findTextNamedBy(walker, input) && isTextInStringBuilder(walker, walker.referenceBoundTo(input));
}

// While an operation that reads a text is walked, the name that it takes is bound to the string. A link may walk on from there to a growth of the same text.
static Reference builderBehindNameInTextBuilders(const Walker& walker, Reference bound)
{
    if (isTextInStringBuilder(walker, bound))
        return bound;
    if (!bound.isValue())
        return { };
    const Operation& made = walker.code().at(bound.id());
    return stringsMadeForReadersInTextBuilders.contains(made.opcode) ? Reference::value(made.builderOperand()) : Reference();
}

// Of a record that came without the value of the body that it stands for: a part of a row, what enters a loop behind the loop of the text.
static TextInStringBuilder* findTextByRecordInTextBuilders(Walker& walker, Reference name)
{
    Vector<TextInStringBuilder, 2>& texts = walker.textsInStringBuilders();
    for (size_t place = texts.size(); place--;) {
        TextInStringBuilder& text = texts[place];
        if (!belongsToFrameOnTopInTextBuilders(walker, text))
            continue;
        bool isNamed = text.builder == name || std::ranges::any_of(text.web.names, [&](const NameOfGrowingText& candidate) {
            return walker.referenceBoundTo(candidate.value) == name;
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

// ---- The looks

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

// ---- At the head of a loop

// The text goes round a loop around as a string: its scan has refused it. A builder for the loop inside would be begun with the whole text at every turn of the outer one.
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
    if (web.standsUnderHandlers && !mayKeepTextUnderHandlers(walker, inputHead, web))
        return false;
    return !web.hasPartsThatMayRunCode || convertsPartsThatMayRunCode();
}

// An edge out of the loop brings a string under each name that is read behind it, and the blocks of the loop that stand behind such a block in the stream get the builder back: Frame::valuesBuiltTwice.
// Only those names: each edge of the body asks of each value in that list whether it is live.
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
        // takeTextsOutOfStringBuildersAtEndOfWay()
        ValueID returned = terminator.kind() == TerminatorKind::Return ? terminator.operand(0) : ValueID();
        if (returned && findNameInTextBuilders(web, returned))
            walker.bindAnewAtEveryBlock(returned);
    }
}

// The walk stands in the block in front of the loop. Null: unreachable or failed.
static TextInStringBuilder* beginTextInTextBuilders(Walker& walker, const BodyIndex& index, BlockID inputHead, GrowingText&& web, Reference enters, Origin origin)
{
    TextInStringBuilder text;
    text.web = WTF::move(web);
    text.frameIndex = static_cast<uint32_t>(walker.indexOfFrameOnTop());
    // The copy of a callee appends through a record of its own, which speaks of no cursor.
    text.liesInFrame = !text.web.growsInCallees && textBegunHereMayLieInFrame(walker, inputHead, text.web);

    Draft begin(text.liesInFrame ? Opcode::StackStringBuilderBegin : Opcode::StringBuilderBegin, origin);
    walker.chooseConservativeFields(begin);
    text.builder = walker.build(WTF::move(begin));
    if (!text.builder)
        return nullptr;

    // Whoever keeps the cursor goes by the place of the record.
    Vector<TextInStringBuilder, 2>& texts = walker.textsInStringBuilders();
    texts.append(WTF::move(text));
    TextInStringBuilder& begun = texts.last();
    if (begun.liesInFrame) {
        Draft start(Opcode::StackStringBuilderStart, origin);
        walker.chooseConservativeFields(start);
        start.setBuilderOperand(begun.builder);
        Reference cursor = walker.build(WTF::move(start));
        if (!cursor)
            return nullptr;
        noteCursorBehind(walker, begun, cursor);
    }

    begun.beganEmpty = textOfKnownPartOfStringBuilder(walker, enters) == walker.program().strings.emptyString();
    if (!begun.beganEmpty) {
        appendPartsToStringBuilder(walker, begun, begun.builder, singleElementSpan(enters), origin);
        if (walker.isUnreachable())
            return nullptr;
        ValueID last = walker.code().lastOperationOf(walker.currentBlock());
        if (last && appendsOfStringsInTextBuilders.contains(walker.code().at(last).opcode) && Reference::value(walker.code().at(last).builderOperand()) == begun.builder) {
            begun.firstPart = enters;
            if (begun.liesInFrame)
                begun.endOfFirstPart = Reference::value(last);
        }
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
            // The scan of a loop around has named the parameter: the text goes through this loop in its builder, whatever the walk may begin here.
            if (findTextNamedBy(walker, head.parameter(parameterIndex))) {
                letEnterInBuilder(parameterIndex, enters);
                continue;
            }
            TextInStringBuilder* before = findTextByRecordInTextBuilders(walker, enters);
            if (!before) {
                walker.fail("a text enters a loop in a builder that nobody has begun"_s);
                return;
            }
            // `for (..) s += a; for (..) s += b`: the second loop goes on with the builder of the first.
            if (web && (!before->liesInFrame || (!web->growsInCallees && textBegunHereMayLieInFrame(walker, inputHead, *web)))) {
                TextInStringBuilder text;
                text.web = WTF::move(*web);
                text.frameIndex = before->frameIndex;
                text.builder = before->builder;
                text.liesInFrame = before->liesInFrame;
                text.firstPart = before->firstPart;
                text.endOfFirstPart = before->endOfFirstPart;
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
        // What is only joined is never looked at as it entered: `let n = 0; for (..) n += "x"`.
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

    // No row of the input says that a turn writes what a look at a builder reads.
    if (someTextMayGrow)
        preview.effects.merge(declaredBoundInTable(Opcode::StringBuilderAppendValue));
}

// ---- Names, growths, readers

bool bindCastOfTextInStringBuilder(Walker& walker, const BodyValue& value)
{
    TextInStringBuilder* text = findTextNamedBy(walker, value.id());
    const NameOfGrowingText* name = text ? findNameInTextBuilders(text->web, value.id()) : nullptr;
    if (!name || name->version == value.id())
        return false;

    Reference builder;
    value.forEachOperand([&](ValueID operand, uint32_t) {
        if (!builder && operand && findNameInTextBuilders(text->web, operand))
            builder = builderBehindNameInTextBuilders(walker, walker.referenceBoundTo(operand));
    });
    if (!builder)
        return false;
    walker.bind(value.id(), builder);
    return true;
}

// Of the operations among the names only a growth is a version of its own.
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
        Reference bound = walker.referenceBoundTo(operand);
        if (named && findNameInTextBuilders(named->web, operand)) {
            Reference builder = builderBehindNameInTextBuilders(walker, bound);
            if (builder && builder != bound)
                bindForLife(operand, bound, builder);
            return;
        }
        if (!isTextInStringBuilder(walker, bound))
            return;
        // It reads nothing, and a hint that names nothing is dropped.
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
        takesText |= operand && isTextInStringBuilder(walker, walker.referenceBoundTo(operand));
    });
    return takesText;
}

StringsInPlaceOfTextsInBuilders::StringsInPlaceOfTextsInBuilders(Walker& walker, const BodyTerminator& terminator)
    : m_walker(walker)
{
    terminator.forEachOperand([&](ValueID operand, uint32_t) {
        if (!operand || walker.isUnreachable())
            return;
        Reference bound = walker.referenceBoundTo(operand);
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
    Reference name = walker.referenceBoundTo(operand);
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
    // On every edge out of the loop the builder is given back, so behind it nothing is left to read.
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

// The parts stand for the operands of the join one by one only where no row was gathered: each join that a row takes in brings a part more than it takes the place of.
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
    // `s += "<" + s`: the string is made of the text as it stands in front of the first append.
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

// ---- The ways out

// AOTWalkTextBuilderHandlers.cpp. The block being walked, or left by the edge being led, stands under a handler that is handed the text or that goes on with its builder.
bool handlerInForceReadsTextInStringBuilder(Walker&, const TextInStringBuilder&);

// functor(TextInStringBuilder&) -> IterationStatus: the texts of the frame on top in whose loop `from` lies and `to` does not. Of several records for one parameter of one head the last is the one being walked.
template<typename Functor>
static void forEachTextLeftOnEdgeInTextBuilders(Walker& walker, BlockID from, BlockID to, const Functor& functor)
{
    const BodyIndex& index = walker.currentBodyIndex();
    Vector<TextInStringBuilder, 2>& texts = walker.textsInStringBuilders();
    Vector<ValueID, 4> parametersOfHeadsSeen;
    for (size_t place = texts.size(); place--;) {
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

// By the first mention in the stream, behind whatever only hands the text on. It decides what pays, never what is right: a key is a string like any other.
static bool isFirstReadAsKeyInTextBuilders(const BodyIndex& index, ValueID text)
{
    while (true) {
        std::optional<BodyUser> mention = index.firstUser(text);
        if (!mention || mention->kind != BodyUseKind::Operand)
            return false;
        BodyValue user = index.value(mention->value);
        if (accessesUnderComputedKeys.contains(user.opcode()))
            return user.operand(OperandIndex::ElemGet::property) == text;
        if (!opcodesThatStateTypes.contains(user.opcode()))
            return false;
        text = user.id();
    }
}

// Of a name that the loop mentions too: by its last reader.
static bool isLastReadAsKeyInTextBuilders(const BodyIndex& index, const BodyLiveness& liveness, ValueID text)
{
    ValueID last = liveness.lastUse(text);
    if (last.index() >= index.numberOfValues() || index.isParameter(last))
        return false;
    BodyValue user = index.value(last);
    if (accessesUnderComputedKeys.contains(user.opcode()))
        return user.operand(OperandIndex::ElemGet::property) == text;
    return opcodesThatStateTypes.contains(user.opcode()) && user.operand(0) == text && isFirstReadAsKeyInTextBuilders(index, user.id());
}

static Reference finishInTextBuilders(Walker& walker, TextInStringBuilder& text, Reference name, Origin origin, bool isLikelyKey)
{
    origin.isTailCall = false;
    text.stringOfState = { };
    text.lengthOfState = { };
    // Whatever throws from here on under such a handler, this operation too, has the string made of the builder once more, or goes on with the builder. So the builder stays out of the pool.
    if (handlerInForceReadsTextInStringBuilder(walker, text))
        return stringOfTextInStringBuilder(walker, text, name, origin);
    if (text.liesInFrame)
        return finishTextInFrame(walker, text, name, origin);
    if (Reference string = lastPartOrFinishedText(walker, text, name, origin))
        return string;
    if (walker.isUnreachable())
        return { };

    Draft finish(Opcode::StringBuilderFinish, origin);
    walker.chooseConservativeFields(finish);
    finish.setBuilderOperand(name);
    finish.setIsLikelyKey(isLikelyKey);
    return walker.build(WTF::move(finish));
}

Reference finishTextInStringBuilder(Walker& walker, TextInStringBuilder& text, Reference name, Origin origin)
{
    return finishInTextBuilders(walker, text, name, origin, false);
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

void finishTextsInStringBuildersHandedOver(Walker& walker, Vector<Reference, 8>& values)
{
    for (size_t place = 0; place < values.size() && !walker.isUnreachable(); ++place) {
        Reference name = values[place];
        if (!isTextInStringBuilder(walker, name))
            continue;
        TextInStringBuilder* text = findTextByRecordInTextBuilders(walker, name);
        if (!text) {
            walker.fail("a text in a builder is met under a value that no scan has named"_s);
            return;
        }
        Reference string = finishTextInStringBuilder(walker, *text, name, walker.originOfOperation());
        if (!string)
            return;
        for (Reference& value : values) {
            if (value == name)
                value = string;
        }
    }
}

void takeTextsOutOfStringBuildersOnEdge(Walker& walker, BlockID to, BodyTarget target, Vector<Reference, 8>& arguments, TextsTakenOutOnEdge& taken)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BlockID from = walker.inputBlockBeingWalked();
    BodyBlock entered = index.block(to);
    BodyLiveness liveness;
    liveness.compute(index);
    Origin origin = walker.originOfOperation();

    // The arguments as written, and behind them the proofs. Whoever leads the edge may stand in another block than the one whose terminator bears the proofs, so a proof is known by its record.
    std::span<const ValueID> written = target.arguments();
    if (written.size() + target.proofs().size() != arguments.size()) {
        walker.fail("the arguments of an edge out of the loop of a text in a builder are not those that are written"_s);
        return;
    }

    forEachTextLeftOnEdgeInTextBuilders(walker, from, to, [&](TextInStringBuilder& text) {
        if (textStaysInStringBuilderOnEdge(walker, text, to, target))
            return IterationStatus::Continue;

        // Every name that is read behind the edge is of the version that is current on it, so all of them stand for what the builder holds here.
        Vector<Reference, 2> records;
        bool everyReaderTakesKey = true;
        auto noteReader = [&](Reference record, bool takesKey) {
            if (!isTextInStringBuilder(walker, record))
                return;
            if (!records.contains(record))
                records.append(record);
            everyReaderTakesKey &= takesKey;
        };
        for (size_t place = 0; place < arguments.size(); ++place) {
            bool namesText = place < written.size() ? written[place] && findNameInTextBuilders(text.web, written[place]) : isTextInStringBuilder(walker, arguments[place]) && findTextByRecordInTextBuilders(walker, arguments[place]) == &text;
            if (namesText)
                noteReader(arguments[place], place < entered.numberOfParameters() && isFirstReadAsKeyInTextBuilders(index, entered.parameter(static_cast<uint32_t>(place))));
        }
        forEachNameLiveAtHeadInTextBuilders(liveness, text.web, to, [&](const NameOfGrowingText& name) {
            noteReader(walker.referenceBoundTo(name.value), isLastReadAsKeyInTextBuilders(index, liveness, name.value));
        });

        if (records.isEmpty()) {
            // The parameter of the head names the builder all over the loop. Where the walk has bound it to a string since, or stands in a copy of the loop whose text lies in no builder, the collector takes what there is.
            Reference builder = walker.referenceBoundTo(text.web.names.first().value);
            if (isTextInStringBuilder(walker, builder))
                discardTextInStringBuilder(walker, text, builder, origin);
            return walker.isUnreachable() ? IterationStatus::Done : IterationStatus::Continue;
        }

        Reference string = finishInTextBuilders(walker, text, records.first(), origin, everyReaderTakesKey);
        if (!string)
            return IterationStatus::Done;
        for (Reference record : records) {
            taken.append({ record, string });
            for (Reference& argument : arguments) {
                if (argument == record)
                    argument = string;
            }
        }
        return IterationStatus::Continue;
    });
}

void bringStringsInPlaceOfTexts(const TextsTakenOutOnEdge& taken, Vector<std::pair<ValueID, Reference>, 2>& namesOnEdge)
{
    for (auto& [value, reference] : namesOnEdge) {
        for (auto [record, string] : taken) {
            if (reference == record)
                reference = string;
        }
    }
}

void takeTextsOutOfStringBuildersAtEndOfWay(Walker& walker, const BodyTerminator& terminator)
{
    ValueID returned = terminator.kind() == TerminatorKind::Return ? terminator.operand(0) : ValueID();
    Reference name = returned ? walker.referenceBoundTo(returned) : Reference();
    if (!isTextInStringBuilder(walker, name))
        return;
    TextInStringBuilder* text = findTextOfOperandInTextBuilders(walker, returned, name);
    if (!text)
        return;
    if (Reference string = stringOfTextInStringBuilder(walker, *text, name, walker.originOfOperation()))
        walker.bind(returned, string);
}

// ---- The doors in the Walker

bool Walker::isLiveAtHeadOf(ValueID value, BlockID inputBlock)
{
    const BodyIndex& index = *m_frames.last().index;
    if (!m_liveness.isComputedFor(index))
        m_liveness.compute(index);
    return m_liveness.isLiveAtHead(value, inputBlock);
}

void Walker::bindAnewAtEveryBlock(ValueID value)
{
    Frame& frame = m_frames.last();
    if (!frame.valuesBuiltTwice.contains(value))
        frame.valuesBuiltTwice.append(value);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
