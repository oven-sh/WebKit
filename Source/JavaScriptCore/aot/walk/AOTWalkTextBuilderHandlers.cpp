/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkTextBuilders.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class HandlerOfTextInBuilder : uint8_t {
    LetsTextGo,
    TakesString,
    GoesOnInBuilder,
};

} // anonymous namespace

static const NameOfGrowingText* findNameInTextBuilderHandlers(const GrowingText& web, ValueID value)
{
    auto found = std::ranges::lower_bound(web.names, value, { }, &NameOfGrowingText::value);
    return found != web.names.end() && found->value == value ? &*found : nullptr;
}

static HandlerOfTextInBuilder classifyHandlerInTextBuilderHandlers(const BodyIndex& index, const GrowingText& web, BodyTarget handler)
{
    if (index.isInLoop(handler.block(), index.loopOf(web.loopHead)))
        return HandlerOfTextInBuilder::GoesOnInBuilder;
    for (ValueID argument : handler.arguments()) {
        if (findNameInTextBuilderHandlers(web, argument))
            return HandlerOfTextInBuilder::TakesString;
    }
    return HandlerOfTextInBuilder::LetsTextGo;
}

static bool mayThrowUnderHandlerInTextBuilderHandlers(const Effects& bound)
{
    return bound.mayThrow || bound.mayCollect;
}

namespace {

class HandlersOfTextScan {
    WTF_MAKE_NONCOPYABLE(HandlersOfTextScan);
public:
    HandlersOfTextScan(Walker& walker, const BodyIndex& index, BlockID head, const GrowingText& web)
        : m_walker(walker)
        , m_index(index)
        , m_web(web)
        , m_head(head)
        , m_loop(index.loopOf(head))
    {
    }

    bool run()
    {
        BodyBlock head = m_index.block(m_head);
        if (m_web.parameterIndex >= head.numberOfParameters())
            return false;
        ValueID parameter = head.parameter(m_web.parameterIndex);
        const NameOfGrowingText* first = findName(parameter);
        if (!first || first->version != parameter)
            return false;

        m_versionsAtEntries.fill(ValueID(), m_index.loop(m_loop).last.index() - m_head.index() + 1);
        versionAtEntryOf(m_head) = parameter;
        for (BlockID member : m_index.membersOf(m_loop)) {
            if (!scanBlock(m_index.block(member)))
                return false;
        }
        return true;
    }

private:
    struct NamesLiveAtHead {
        BlockID block;
        ValueID version;
        bool isAnyLive { false };
        bool haveSeveralVersions { false };
    };

    const NameOfGrowingText* findName(ValueID value) const { return value ? findNameInTextBuilderHandlers(m_web, value) : nullptr; }
    ValueID& versionAtEntryOf(BlockID block) LIFETIME_BOUND { return m_versionsAtEntries[block.index() - m_head.index()]; }
    bool mentionsOnly(ValueID value, ValueID current) const
    {
        const NameOfGrowingText* name = findName(value);
        return !name || name->version == current;
    }

    NamesLiveAtHead namesLiveAtHeadOf(BlockID block)
    {
        for (const NamesLiveAtHead& known : m_namesLiveAtHeads) {
            if (known.block == block)
                return known;
        }
        NamesLiveAtHead live { block, { }, false, false };
        for (const NameOfGrowingText& name : m_web.names) {
            if (!m_walker.isLiveAtHeadOf(name.value, block))
                continue;
            live.haveSeveralVersions |= live.isAnyLive && live.version != name.version;
            live.isAnyLive = true;
            live.version = name.version;
        }
        m_namesLiveAtHeads.append(live);
        return live;
    }

    bool namesLiveAtHeadAreOf(BlockID block, ValueID version)
    {
        NamesLiveAtHead live = namesLiveAtHeadOf(block);
        return !live.isAnyLive || (!live.haveSeveralVersions && live.version == version);
    }

    bool arriveAt(BodyBlock entered, std::optional<uint32_t> taking, ValueID arriving)
    {
        if (taking && *taking >= entered.numberOfParameters())
            return false;
        for (uint32_t index = 0; index < entered.numberOfParameters(); ++index) {
            const NameOfGrowingText* name = findName(entered.parameter(index));
            if (!!name != (taking && *taking == index))
                return false;
            if (!name)
                continue;
            if (name->version == name->value)
                arriving = name->value;
            else if (name->version != arriving)
                return false;
        }
        ValueID& atEntry = versionAtEntryOf(entered.id());
        if (!atEntry)
            atEntry = arriving;
        return atEntry == arriving;
    }

    bool followEdge(const BodyTerminator& terminator, uint32_t targetIndex, BodyTarget target, ValueID current)
    {
        std::optional<uint32_t> taking;
        auto passOver = [&](ValueID value, uint32_t parameterIndex) {
            const NameOfGrowingText* name = findName(value);
            if (!name)
                return true;
            if (name->version != current || taking)
                return false;
            taking = parameterIndex;
            return true;
        };

        std::span<const ValueID> arguments = target.arguments();
        for (uint32_t argumentIndex = 0; argumentIndex < arguments.size(); ++argumentIndex) {
            if (!passOver(arguments[argumentIndex], argumentIndex))
                return false;
        }
        for (uint32_t proofIndex = 0; proofIndex < target.proofs().size(); ++proofIndex) {
            std::optional<BodyEdgeResult> result = m_index.edgeResult(terminator, targetIndex, target, proofIndex);
            if (!result)
                continue;
            if (m_index.isUnused(result->parameter) ? !mentionsOnly(result->subject, current) : !passOver(result->subject, static_cast<uint32_t>(arguments.size()) + proofIndex))
                return false;
        }

        BlockID to = target.block();
        if (!m_index.isInLoop(to, m_loop))
            return namesLiveAtHeadAreOf(to, current);
        return arriveAt(m_index.block(to), taking, current);
    }

    bool followHandlerEdge(BodyBlock block, ValueID atEntry, bool mayThrowBehindGrowth)
    {
        BodyTarget handler = block.handler();
        std::optional<uint32_t> taking;
        std::span<const ValueID> arguments = handler.arguments();
        for (uint32_t argumentIndex = 0; argumentIndex < arguments.size(); ++argumentIndex) {
            const NameOfGrowingText* name = findName(arguments[argumentIndex]);
            if (!name)
                continue;
            if (name->version != atEntry || taking)
                return false;
            taking = argumentIndex;
        }

        BlockID to = handler.block();
        if (!m_index.isInLoop(to, m_loop)) {
            if (namesLiveAtHeadOf(to).isAnyLive)
                return false;
            return !taking || !mayThrowBehindGrowth;
        }

        TerminatorKind end = block.terminator().kind();
        if (mayThrowBehindGrowth || end == TerminatorKind::Throw || end == TerminatorKind::Rethrow)
            return false;
        return namesLiveAtHeadAreOf(to, atEntry) && arriveAt(m_index.block(to), taking, atEntry);
    }

    bool scanBlock(BodyBlock block)
    {
        ValueID atEntry = versionAtEntryOf(block.id());
        if (!atEntry || findName(block.subject()))
            return false;

        ValueID current = atEntry;
        bool mayThrowBehindGrowth = false;
        for (BodyValue operation : block.values()) {
            mayThrowBehindGrowth |= current != atEntry && mayThrowUnderHandlerInTextBuilderHandlers(declaredBoundInTable(operation.opcode()));
            bool mentionsOlderState = false;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                mentionsOlderState |= !mentionsOnly(operand, current);
            });
            if (mentionsOlderState)
                return false;
            const NameOfGrowingText* name = findName(operation.id());
            if (!name)
                continue;
            if (name->version == name->value)
                current = name->value;
            else if (name->version != current)
                return false;
        }

        BodyTerminator terminator = block.terminator();
        mayThrowBehindGrowth |= current != atEntry && mayThrowUnderHandlerInTextBuilderHandlers(declaredBoundOf(terminator.kind()));
        bool mentionsOlderState = false;
        terminator.forEachOperand([&](ValueID operand, uint32_t) {
            mentionsOlderState |= !mentionsOnly(operand, current);
        });
        if (mentionsOlderState)
            return false;

        uint32_t targetIndex = 0;
        for (BodyTarget target : terminator.targets()) {
            if (!followEdge(terminator, targetIndex++, target, current))
                return false;
        }
        return !block.hasHandler() || followHandlerEdge(block, atEntry, mayThrowBehindGrowth);
    }

    Walker& m_walker;
    const BodyIndex& m_index;
    const GrowingText& m_web;
    BlockID m_head;
    LoopID m_loop;
    Vector<ValueID, 32> m_versionsAtEntries;
    Vector<NamesLiveAtHead, 4> m_namesLiveAtHeads;
};

} // anonymous namespace

bool mayKeepTextUnderHandlers(Walker& walker, BlockID inputHead, const GrowingText& web)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!walker.has(Mechanism::Handlers) || !index.isLoopHead(inputHead) || index.hasIrreducibleFlow())
        return false;
    BodyBlock head = index.block(inputHead);
    if (head.hasHandler() && index.isInLoop(head.handler().block(), index.loopOf(inputHead)))
        return false;
    return HandlersOfTextScan(walker, index, inputHead, web).run();
}

bool everyHandlerLetsTextGo(Walker& walker, const GrowingText& web)
{
    const BodyIndex& index = walker.currentBodyIndex();
    for (BlockID member : index.membersOf(index.loopOf(web.loopHead))) {
        BodyBlock block = index.block(member);
        if (!block.hasHandler())
            continue;
        BodyTarget handler = block.handler();
        if (classifyHandlerInTextBuilderHandlers(index, web, handler) != HandlerOfTextInBuilder::LetsTextGo)
            return false;
        for (const NameOfGrowingText& name : web.names) {
            if (walker.isLiveAtHeadOf(name.value, handler.block()))
                return false;
        }
    }
    return true;
}

static bool writesCharactersInTextBuilderHandlers(const Walker& walker, Reference part)
{
    if (!part.isConstant())
        return true;
    const Operation& constant = walker.code().at(part.id());
    return constant.opcode != Opcode::String || constant.string() != walker.program().strings.emptyString();
}

static bool handlerInForceReadsTextInTextBuilderHandlers(Walker& walker, const StringInBuilder& text)
{
    if (walker.topFrameIndex() != text.frameIndex)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(walker.inputBlockBeingWalked());
    return block.hasHandler() && classifyHandlerInTextBuilderHandlers(index, text.web, block.handler()) != HandlerOfTextInBuilder::LetsTextGo;
}

bool handlerReadsStringInBuilder(Walker& walker, const StringInBuilder& text)
{
    return text.web.standsUnderHandlers && handlerInForceReadsTextInTextBuilderHandlers(walker, text);
}

void appendPartsRolledBackWhereTheyFail(Walker& walker, StringInBuilder& text, Reference name, std::span<const Reference> parts, Origin origin)
{
    size_t numberOfPartsWritten = std::ranges::count_if(parts, [&](Reference part) {
        return writesCharactersInTextBuilderHandlers(walker, part);
    });
    if (numberOfPartsWritten < 2 || !handlerInForceReadsTextInTextBuilderHandlers(walker, text)) {
        appendPartsToStringBuilder(walker, text, name, parts, origin);
        return;
    }

    origin.isTailCall = false;
    Draft lengthInFront(Opcode::StringBuilderCheckpoint, origin);
    walker.chooseConservativeFields(lengthInFront);
    lengthInFront.setBuilderOperand(name);
    Reference checkpoint = walker.build(WTF::move(lengthInFront));
    if (!checkpoint)
        return;

    Code& code = walker.code();
    BlockID handler = code.addBlock(BlockRole::CleanupHandler, BlockFrequency::Rare);
    code.setLoopMarks(handler, walker.isInLoop(), false);
    BlockID stretch = walker.addBlock();
    BlockID behind = walker.addBlock();
    walker.jumpTo(stretch);
    if (walker.isUnreachable())
        return;
    code.closeEdge(code.addHandlerEdge(stretch), handler, { });
    WalkState stateInFront = walker.state();

    appendPartsToStringBuilder(walker, text, name, parts, origin);
    if (walker.hasFailed())
        return;
    if (!walker.isUnreachable() && walker.currentBlock() != stretch) {
        walker.fail("the parts of an append that is rolled back where it fails do not stand in one block"_s);
        return;
    }
    walker.jumpTo(behind);

    walker.continueIn(handler);
    walker.state() = WTF::move(stateInFront);
    walker.state().invalidate(declaredBoundInTable(Opcode::StringBuilderAppendValue));
    Reference exception = walker.build(Draft(Opcode::CatchException, origin));
    Draft rollBack(Opcode::StringBuilderRollBack, origin);
    walker.chooseConservativeFields(rollBack);
    rollBack.setBuilderOperand(name);
    rollBack.setCheckpointOperand(checkpoint);
    walker.build(WTF::move(rollBack));
    if (exception)
        walker.endWithRethrow(exception, origin);
    walker.continueIn(behind);
}

void Walker::endWithRethrow(Reference exception, Origin origin)
{
    TerminatorDraft draft(TerminatorKind::Rethrow, origin);
    draft.setExceptionOperand(exception);
    endWithoutEdges(draft);
}

std::optional<size_t> Walker::findTextHeldInFrame(size_t frameIndex, ValueID value) const
{
    const Frame& frame = m_frames[frameIndex];
    Reference bound = frame.lastNameOf(value);
    if (!isStringInBuilder(*this, bound) && !(bound.isValue() && m_code.at(bound.id()).opcode == Opcode::StringBuilderToString))
        return std::nullopt;
    for (size_t place = m_stringsInBuilders.size(); place--;) {
        const StringInBuilder& text = m_stringsInBuilders[place];
        if (text.frameIndex != frameIndex || !text.builder.isValue() || m_code.at(text.builder.id()).origin().inlineFrame != frame.inlineFrame)
            continue;
        if (frame.index->isInLoop(frame.block, frame.index->loopOf(text.web.loopHead)) && findNameInTextBuilderHandlers(text.web, value))
            return place;
    }
    return std::nullopt;
}

bool Walker::leadHandlerEdgeBehindStringsOfTexts(size_t frameIndex, BodyTarget target, Vector<Reference, 4>& arguments, Vector<TypeID, 4>& types, Vector<std::pair<ValueID, Reference>, 2>& names)
{
    const BodyIndex& index = *m_frames[frameIndex].index;
    std::span<const ValueID> written = target.arguments();
    Vector<std::pair<uint32_t, Reference>, 2> taken;
    for (uint32_t place = 0; place < written.size(); ++place) {
        std::optional<size_t> found = findTextHeldInFrame(frameIndex, written[place]);
        if (!found)
            continue;
        const StringInBuilder& text = m_stringsInBuilders[*found];
        if (!index.isInLoop(target.block(), index.loopOf(text.web.loopHead))) {
            taken.append({ place, text.builder });
            continue;
        }
        arguments[place] = text.builder;
        types[place] = ownTypeOf(text.builder);
    }
    if (taken.isEmpty())
        return false;

    BlockID throwing = m_block;
    WalkState stateAtThrow = m_state;
    SetForScope isWalkingAccess(m_isWalkingAccess, false);
    SetForScope loopsAroundEdge(m_numberOfLoopsAroundEdgeBeingLed, numberOfOpenLoopsAround(frameIndex, target.block()));
    Origin origin = m_origin;
    origin.isTailCall = false;
    auto goOnWhereBlockMayThrow = [&] {
        m_block = throwing;
        m_state = WTF::move(stateAtThrow);
        return true;
    };

    BlockID landing = m_code.addBlock(BlockRole::CleanupHandler, BlockFrequency::Rare);
    m_code.setLoopMarks(landing, false, false);
    m_code.closeEdge(m_code.addHandlerEdge(throwing), landing, { });
    m_block = landing;
    Reference exception = build(Draft(Opcode::CatchException, origin));
    if (!exception)
        return goOnWhereBlockMayThrow();
    for (auto [place, builder] : taken) {
        Draft string(Opcode::StringBuilderToString, origin);
        chooseConservativeFields(string);
        string.setBuilderOperand(builder);
        Reference made = build(WTF::move(string));
        if (!made)
            return goOnWhereBlockMayThrow();
        arguments[place] = made;
        types[place] = ownTypeOf(made);
    }

    BlockID rethrowing = m_code.addBlock(BlockRole::Ordinary, BlockFrequency::Rare);
    m_code.setLoopMarks(rethrowing, false, false);
    m_code.end(landing, TerminatorDraft(TerminatorKind::Jump, origin), 1);
    m_code.closeEdge(m_code.at(landing).terminator.next(), rethrowing, { });
    m_block = rethrowing;

    uint32_t place = addOpenEdge(frameIndex, target.block());
    if (place == noOpenEdge)
        return goOnWhereBlockMayThrow();
    OpenEdge& open = m_openEdges[place];
    open.edge = m_code.addHandlerEdge(rethrowing);
    open.arguments = WTF::move(arguments);
    open.types = WTF::move(types);
    takeNamesOntoEdge(open, frameIndex);
    for (auto [value, name] : names)
        m_frames[frameIndex].duplicatedValues->nameOtherwiseOn(open.names, value, name);
    endWithRethrow(exception, origin);
    return goOnWhereBlockMayThrow();
}

void Walker::bindTextsAtHeadOfHandlerBlock(BlockID inputBlock)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    Vector<ValueID, 2> parametersOfHeadsSeen;
    for (size_t place = m_stringsInBuilders.size(); place--;) {
        const StringInBuilder& text = m_stringsInBuilders[place];
        if (text.frameIndex != frameIndex || !text.builder.isValue() || m_code.at(text.builder.id()).origin().inlineFrame != m_frames[frameIndex].inlineFrame)
            continue;
        if (text.web.names.isEmpty() || !index.isInLoop(inputBlock, index.loopOf(text.web.loopHead)) || parametersOfHeadsSeen.contains(text.web.names[0].value))
            continue;
        parametersOfHeadsSeen.append(text.web.names[0].value);
        for (const NameOfGrowingText& name : text.web.names) {
            if (isLiveAtHeadOf(name.value, inputBlock))
                bind(name.value, text.builder);
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
