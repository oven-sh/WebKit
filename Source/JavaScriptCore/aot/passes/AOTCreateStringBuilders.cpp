/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinCalls.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTResultRules.h"
#include "AOTRuleSupport.h"
#include "AOTSchemaInlineBuiltins.h"
#include "AOTStringBuilderAnalysis.h"
#include "AOTStringRanges.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t stateOperandIndex = 0;
static_assert(OperandIndex::Add::left == stateOperandIndex);
static_assert(OperandIndex::Strcat::parts == stateOperandIndex);

static constexpr OpcodeSet equalityComparisons { Opcode::StrictEq, Opcode::Eq };
static constexpr OpcodeSet closingOperations { Opcode::StringBuilderFinish, Opcode::StringBuilderDiscard };
static constexpr OpcodeSet redirectableOperations = closingOperations | OpcodeSet {
    Opcode::StringBuilderAppend, Opcode::StringBuilderAppendBuiltin, Opcode::StringBuilderAppendJSON, Opcode::StringBuilderCheckpoint, Opcode::StringBuilderRollBack, Opcode::CallDirect,
};
static constexpr AbstractHeapSet stringCreationHeaps = bookkeepingHeaps | AbstractHeapSet { AbstractHeap::StringStorage };

static void retype(FunctionPassContext& context, Value& value)
{
    value.type = resultTypeOf(context.program, context.function, value, context.types);
    value.setExpected({ });
}

// The append makes the same conversion, so it runs out of memory where the call would have.
static bool onlyCreatesString(const Effects& effects)
{
    return !effects.mayRunOtherCode() && !effects.mayBeTerminated && effects.writes.containsOnly(stringCreationHeaps);
}

static Value makeAppend(ValueID handle, std::span<const ValueID> parts)
{
    Value append = IRBuilder::make(Opcode::StringBuilderAppend, { handle });
    append.setPartsOperands(parts);
    return append;
}

static Value makeAppendBuiltin(ValueID handle, const Value& call)
{
    Value append = IRBuilder::make(Opcode::StringBuilderAppendBuiltin, { handle, call.receiverOperand() });
    append.setOverload(call.overload());
    append.setArgumentsOperands(call.argumentsOperands());
    return append;
}

static std::optional<BlockID> frontOf(const Function& function, const CFG& cfg, BlockID block)
{
    std::optional<Edge> edge = cfg.singleIncomingEdge(block);
    if (!edge || !function.at(block).parameters.isEmpty())
        return std::nullopt;
    const Block& front = function.at(edge->from);
    return front.terminator.kind == TerminatorKind::Jump && !front.handler ? std::optional<BlockID>(edge->from) : std::nullopt;
}

static bool splitInFrontOfCoveredBlocks(FunctionPassContext& context, const StringBuilderPlan& plan)
{
    Function& function = context.function;
    const CFG& cfg = context.analyses.cfg();
    BitVector blocksWithoutFront;
    auto requireFront = [&](BlockID block) {
        if (!frontOf(function, cfg, block))
            blocksWithoutFront.set(block.index());
    };
    for (const StringWeb& web : plan.webs()) {
        if (web.decision != StringBuilderDeclineReason::Created)
            continue;
        for (const StringWebEntry& entry : web.entries) {
            if (entry.edge && entry.edge->isHandlerEdge())
                requireFront(entry.edge->from);
        }
        for (const StringStateRead& read : web.reads) {
            if (read.use.kind == Use::Kind::HandlerArgument)
                requireFront(read.use.block);
        }
    }
    if (blocksWithoutFront.isEmpty())
        return false;

    IRBuilder builder(context.analyses.context(), function, context.types);
    for (size_t blockIndex : blocksWithoutFront) {
        BlockID block(blockIndex);
        const Block& contents = function.at(block);
        uint32_t index = indexAfterCaughtValues(function, contents);
        builder.setOrigin(index < contents.values.size() ? function.at(contents.values[index]).origin() : contents.terminator.origin());
        builder.splitBlock(ProgramPoint { block, index });
        builder.removeHandler(block);
    }
    return true;
}

namespace {

class StringBuilderRewriter {
public:
    // The builder gets no FunctionAnalyses: m_cfg and m_useLists have to outlive split edges and queued insertions.
    StringBuilderRewriter(FunctionPassContext& context, const StringBuilderPlan& plan)
        : m_context(context)
        , m_plan(plan)
        , m_function(context.function)
        , m_builder(context.analyses.context(), context.function, context.types)
        , m_cfg(&context.analyses.cfg())
        , m_useLists(&context.analyses.useLists())
    {
        for (const StringWeb& web : plan.webs()) {
            if (web.decision == StringBuilderDeclineReason::Created)
                m_createdWebs.append(&web);
        }
    }

    PassOutcome run()
    {
        reportDecisions();
        if (!m_createdWebs.isEmpty())
            rewriteWebs();
        foldAppendForms();
        return m_didChange ? PassOutcome::Changed : PassOutcome::Unchanged;
    }

private:
    struct InsertionSite {
        ProgramPoint point;
        Origin origin;
    };

    struct Materialization {
        ValueID member;
        ProgramPoint point;
        ValueID string;
    };

    struct HostAppendFold {
        ValueID append;
        ProgramPoint point;
        BitVector operands;
    };

    const Type& typeOf(ValueID id) const { return m_context.program.typeOf(m_function, id); }

    void reportDecisions()
    {
        for (const StringWeb& web : m_plan.webs()) {
            switch (web.decision) {
            case StringBuilderDeclineReason::Created:
                m_context.build.statistics().count(StatisticID::StringBuildersCreated);
                break;
            case StringBuilderDeclineReason::TooFewAppends:
                m_context.addDecline(Wire::DeclineKind::StringBuilderNotCreated, web.decidedAt, web.decision, LimitID::MinAppendsForStringBuilder, web.numberOfAppends);
                break;
            case StringBuilderDeclineReason::WebLimit:
                m_context.addDecline(Wire::DeclineKind::StringBuilderNotCreated, web.decidedAt, web.decision, LimitID::MaxStringWebsPerFunction);
                break;
            case StringBuilderDeclineReason::Unknown:
            case StringBuilderDeclineReason::TwoStatesLive:
            case StringBuilderDeclineReason::StateIsNotFirstOperand:
                m_context.addDecline(Wire::DeclineKind::StringBuilderNotCreated, web.decidedAt, web.decision);
                break;
            }
        }
    }

    void rewriteWebs()
    {
        m_handleType = m_context.types.intern(Type::forOpaque(OpaqueKind::StringBuilder));
        m_handleOf.grow(m_function.values.size());

        for (const StringWeb* web : m_createdWebs)
            splitEdges(*web);
        for (const StringWeb* web : m_createdWebs)
            assignHandles(*web);
        for (const StringWeb* web : m_createdWebs) {
            for (const StringStateRead& read : web->reads)
                rewriteRead(read);
        }
        for (const StringWeb* web : m_createdWebs)
            rewriteWeb(*web);
        for (const StringWeb* web : m_createdWebs)
            emitDiscards(*web);

        m_builder.applyInsertions();
        m_context.analyses.didChangeControlFlow();
        m_cfg = &m_context.analyses.cfg();
        m_useLists = &m_context.analyses.useLists();
        m_didChange = true;
    }

    void splitEdges(const StringWeb& web)
    {
        for (const StringWebEntry& entry : web.entries) {
            if (!entry.edge)
                continue;
            if (!entry.edge->isHandlerEdge()) {
                if (!hasMatchingHandlerEntry(entry))
                    splitEdge(*entry.edge);
                continue;
            }
            uint32_t numberOfTargets = m_function.at(entry.edge->from).terminator.targets.size();
            for (uint32_t targetIndex = 0; targetIndex < numberOfTargets; ++targetIndex) {
                Edge edge { entry.edge->from, targetIndex };
                if (!findEntryArgument(entry, edge))
                    splitEdge(edge);
            }
        }
        for (const StringStateRead& read : web.reads) {
            if (read.use.kind == Use::Kind::EdgeArgument)
                splitEdge(read.use.edge());
        }
        for (const StringStateEdgeDiscard& discard : web.discardedEdges)
            splitEdge(discard.edge);
    }

    void assignHandles(const StringWeb& web)
    {
        for (ValueID member : web.members) {
            Value& value = m_function.edit(member);
            if (value.opcode != Opcode::Parameter)
                continue;
            value.type = m_handleType;
            value.setExpected({ });
            m_handleOf[member.index()] = member;
        }
        for (const StringWebEntry& entry : web.entries) {
            if (entry.concatenation)
                m_handleOf[entry.concatenation.index()] = insert(siteOf(entry.concatenation), IRBuilder::make(Opcode::StringBuilderBegin));
        }
        for (ValueID member : web.members) {
            ValueID state = member;
            while (!m_handleOf[state.index()])
                state = m_function.at(state).operand(stateOperandIndex);
            ValueID handle = m_handleOf[state.index()];
            for (ValueID next = member; next != state; next = m_function.at(next).operand(stateOperandIndex))
                m_handleOf[next.index()] = handle;
        }
    }

    void rewriteRead(const StringStateRead& read)
    {
        const Use& use = read.use;
        ValueID handle = m_handleOf[read.member.index()];
        switch (read.kind) {
        case StringStateUse::Unknown:
            break;
        case StringStateUse::Length:
            rewriteLengthRead(use, handle);
            return;
        case StringStateUse::Truthiness:
            replaceUse(use, insert(siteOf(use), IRBuilder::make(Opcode::StringBuilderLength, { handle })));
            return;
        case StringStateUse::Read: {
            InsertionSite site = siteOf(use);
            if (read.operationBeforeCursors)
                site.point = m_useLists->pointOf(read.operationBeforeCursors);
            replaceUse(use, materialize(read.member, site, read.endsWeb));
            return;
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void rewriteLengthRead(const Use& use, ValueID handle)
    {
        if (!equalityComparisons.contains(m_function.at(use.user).opcode)) {
            Value& user = m_function.edit(use.user);
            user.become(Opcode::StringBuilderLength);
            user.setBuilderOperand(handle);
            retype(m_context, user);
            return;
        }
        InsertionSite site = siteOf(use.user);
        ValueID length = insert(site, IRBuilder::make(Opcode::StringBuilderLength, { handle }));
        ValueID lengthOfEmptyString = insert(site, makeInteger(0));
        Value& comparison = m_function.edit(use.user);
        comparison.setLeftOperand(length);
        comparison.setRightOperand(lengthOfEmptyString);
    }

    void rewriteWeb(const StringWeb& web)
    {
        for (ValueID member : web.members) {
            if (m_function.at(member).opcode != Opcode::Parameter)
                rewriteConcatenation(member);
        }
        for (const StringWebEntry& entry : web.entries) {
            if (entry.edge && !entry.edge->isHandlerEdge() && !hasMatchingHandlerEntry(entry))
                rewriteEntryEdge(entry);
        }
        for (const StringWebEntry& entry : web.entries) {
            if (entry.edge && entry.edge->isHandlerEdge())
                rewriteHandlerEntry(entry);
        }
        rewriteEdges(web);
    }

    void rewriteConcatenation(ValueID id)
    {
        Opcode opcode = m_function.at(id).opcode;
        bool continuesBuilder = m_plan.webOf(m_function.at(id).operand(stateOperandIndex)) == m_plan.webOf(id);
        Vector<ValueID, 8> parts;
        m_function.at(id).forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (operandIndex != stateOperandIndex || !continuesBuilder)
                parts.append(operand);
        });
        for (ValueID& part : parts) {
            if (!isStringBuilderPart(typeOf(part)))
                part = insertConversion(siteOf(id), opcode, part);
        }
        Value& append = m_function.edit(id);
        append.become(Opcode::StringBuilderAppend);
        append.setBuilderOperand(m_handleOf[id.index()]);
        append.setPartsOperands(parts.span());
        retype(m_context, append);
        m_appends.append(id);
    }

    void rewriteEntryEdge(const StringWebEntry& entry)
    {
        ValueID handle = beginBuilder(siteOnEdge(*entry.edge), entry.value);
        targetOnEdge(*entry.edge).arguments[m_useLists->locationOf(entry.parameter).index] = handle;
    }

    void rewriteHandlerEntry(const StringWebEntry& entry)
    {
        BlockID block = entry.edge->from;
        ValueID handle = beginBuilder(siteInFrontOf(block), entry.value);
        uint32_t numberOfTargets = m_function.at(block).terminator.targets.size();
        for (uint32_t targetIndex = 0; targetIndex < numberOfTargets; ++targetIndex) {
            Edge edge { block, targetIndex };
            if (std::optional<uint32_t> argumentIndex = findEntryArgument(entry, edge))
                targetOnEdge(edge).arguments[*argumentIndex] = handle;
            else
                insert(siteOnEdge(edge), IRBuilder::make(Opcode::StringBuilderDiscard, { handle }));
        }
        targetOnEdge(*entry.edge).arguments[m_useLists->locationOf(entry.parameter).index] = handle;
    }

    void rewriteEdges(const StringWeb& web)
    {
        size_t numberOfReads = 0;
        for (ValueID member : web.members) {
            for (const Use& use : m_useLists->usesOf(member)) {
                if (!continuesWeb(member, use))
                    ++numberOfReads;
                else if (use.isEdgeUse())
                    replaceUse(use, m_handleOf[member.index()]);
            }
        }
        RELEASE_ASSERT(numberOfReads == web.reads.size());
    }

    void emitDiscards(const StringWeb& web)
    {
        for (const StringStateDiscard& discard : web.discardedAfter)
            insert({ m_useLists->pointAfter(discard.after), m_function.at(discard.after).origin() }, IRBuilder::make(Opcode::StringBuilderDiscard, { m_handleOf[discard.member.index()] }));
        for (const StringStateEdgeDiscard& discard : web.discardedEdges)
            insert(siteOnEdge(discard.edge), IRBuilder::make(Opcode::StringBuilderDiscard, { m_handleOf[discard.member.index()] }));
    }

    // Code on an edge never runs under a handler that could still get the handle.
    void splitEdge(Edge edge)
    {
        if (!endsInUncoveredJump(edge.from) && !blockOnEdge(edge))
            m_blocksOnEdges.set(m_builder.splitEdge(edge).index());
    }

    bool endsInUncoveredJump(BlockID block) const { return m_function.at(block).terminator.kind == TerminatorKind::Jump && !m_function.at(block).handler; }

    std::optional<BlockID> blockOnEdge(Edge edge) const
    {
        BlockID target = targetOf(m_function, edge).block;
        return m_blocksOnEdges.get(target.index()) ? std::optional<BlockID>(target) : std::nullopt;
    }

    ProgramPoint pointOnEdge(Edge edge) const
    {
        if (std::optional<BlockID> block = blockOnEdge(edge))
            return pointBeforeTerminatorOf(m_function, *block);
        RELEASE_ASSERT(endsInUncoveredJump(edge.from));
        return pointBeforeTerminatorOf(m_function, edge.from);
    }

    Target& targetOnEdge(Edge edge)
    {
        if (std::optional<BlockID> block = blockOnEdge(edge))
            return m_function.edit(*block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator.next();
        return editTarget(m_function, edge);
    }

    BlockID blockInFrontOf(BlockID block) const
    {
        std::optional<BlockID> front = frontOf(m_function, *m_cfg, block);
        RELEASE_ASSERT(front);
        return *front;
    }

    InsertionSite siteBeforeTerminatorOf(BlockID block) const { return { pointBeforeTerminatorOf(m_function, block), m_function.at(block).terminator.origin() }; }
    InsertionSite siteOnEdge(Edge edge) const { return { pointOnEdge(edge), m_function.at(edge.from).terminator.origin() }; }
    InsertionSite siteInFrontOf(BlockID block) const { return siteBeforeTerminatorOf(blockInFrontOf(block)); }
    InsertionSite siteOf(ValueID value) const { return { m_useLists->pointOf(value), m_function.at(value).origin() }; }

    InsertionSite siteOf(const Use& use) const
    {
        switch (use.kind) {
        case Use::Kind::Operand:
            return siteOf(use.user);
        case Use::Kind::TerminatorOperand:
            return siteBeforeTerminatorOf(use.block);
        case Use::Kind::EdgeArgument:
            return siteOnEdge(use.edge());
        case Use::Kind::HandlerArgument:
            return siteInFrontOf(use.block);
        case Use::Kind::BlockSubject:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    ValueID insert(const InsertionSite& site, Value&& value)
    {
        m_builder.setOrigin(site.origin);
        return m_builder.insertBefore(site.point, WTF::move(value));
    }

    ValueID insertConversion(const InsertionSite& site, Opcode concatenation, ValueID part)
    {
        if (concatenation == Opcode::Strcat)
            return insert(site, IRBuilder::make(Opcode::ToString, { part }));
        RELEASE_ASSERT(concatenation == Opcode::Add);
        Value emptyString = IRBuilder::make(Opcode::String);
        emptyString.setString(m_context.program.strings.emptyString());
        ValueID left = insert(site, WTF::move(emptyString));
        return insert(site, IRBuilder::make(Opcode::Add, { left, part }));
    }

    ValueID beginBuilder(const InsertionSite& site, ValueID firstState)
    {
        ValueID handle = insert(site, IRBuilder::make(Opcode::StringBuilderBegin));
        // No use of an Unspecified value is reached, and an append on the edge would be one that is.
        if (!isEmptyString(firstState) && m_function.at(firstState).opcode != Opcode::Unspecified)
            insert(site, IRBuilder::make(Opcode::StringBuilderAppend, { handle, firstState }));
        return handle;
    }

    ValueID materialize(ValueID member, const InsertionSite& site, bool endsWeb)
    {
        if (m_lastMaterialization.member == member && m_lastMaterialization.point == site.point)
            return m_lastMaterialization.string;
        Opcode opcode = endsWeb ? Opcode::StringBuilderFinish : Opcode::StringBuilderToString;
        ValueID string = insert(site, IRBuilder::make(opcode, { m_handleOf[member.index()] }));
        m_lastMaterialization = { member, site.point, string };
        return string;
    }

    void replaceUse(const Use& use, ValueID replacement)
    {
        if (use.kind == Use::Kind::EdgeArgument)
            targetOnEdge(use.edge()).arguments[use.operandIndex] = replacement;
        else
            Typed::replaceUse(m_function, use, replacement);
    }

    bool continuesWeb(ValueID member, const Use& use)
    {
        std::optional<uint32_t> web = m_plan.webOf(member);
        if (use.isEdgeUse())
            return m_plan.webOf(m_function.at(targetOnEdge(use.edge()).block).parameters[use.operandIndex]) == web;
        return use.kind == Use::Kind::Operand && use.operandIndex == stateOperandIndex && m_plan.webOf(use.user) == web;
    }

    std::optional<uint32_t> findEntryArgument(const StringWebEntry& entry, Edge edge)
    {
        std::optional<uint32_t> web = m_plan.webOf(entry.parameter);
        const Target& target = targetOnEdge(edge);
        const auto& parameters = m_function.at(target.block).parameters;
        for (uint32_t index = 0; index < target.arguments.size(); ++index) {
            if (target.arguments[index] == entry.value && m_plan.webOf(parameters[index]) == web)
                return index;
        }
        return std::nullopt;
    }

    bool hasMatchingHandlerEntry(const StringWebEntry& entry)
    {
        BlockID block = entry.edge->from;
        return m_function.at(block).handler && findEntryArgument(entry, Edge::toHandlerOf(block));
    }

    bool isEmptyString(ValueID value) const { return stringConstantOf(m_context.program, m_function, value) == m_context.program.strings.emptyString(); }

    // Every fold is found before the first value is added: FunctionAnalyses answers for the values it has seen.
    void foldAppendForms()
    {
        Vector<HostAppendFold, 4> folds;
        for (ValueID append : m_appends) {
            ValueLocation location = m_useLists->locationOf(append);
            if (location.placement != ValuePlacement::InBlock)
                continue;
            foldConversions(append);
            ProgramPoint point { location.block, location.index };
            BitVector operands = findHostAppends(append, point);
            if (!operands.isEmpty())
                folds.append(HostAppendFold { append, point, WTF::move(operands) });
        }
        for (const HostAppendFold& fold : folds)
            splitAppend(fold);
        if (m_blocksWithErasedValues.isEmpty())
            return;

        m_builder.applyInsertions();
        for (size_t blockIndex : m_blocksWithErasedValues) {
            BlockID block(blockIndex);
            const BlockValues& values = m_function.at(block).values;
            BitVector erasedIndexes;
            for (uint32_t index = 0; index < values.size(); ++index) {
                if (m_valuesToErase.get(values[index].index()))
                    erasedIndexes.set(index);
            }
            m_builder.eraseValues(block, erasedIndexes);
        }
        m_context.analyses.didChangeValues();
        m_didChange = true;
    }

    const BuiltinForms* appendFormsOf(ValueID append, ValueID part) const
    {
        const Value& call = m_function.at(part);
        if (call.opcode != Opcode::CallBuiltin || call.builtinCallForm() != BuiltinCallForm::Whole || !typeOf(part).isString())
            return nullptr;
        std::optional<Use> use = m_useLists->singleUse(part);
        if (!use || use->user != append || m_useLists->blockOf(part) != use->block)
            return nullptr;
        const BuiltinTable& builtins = m_context.program.builtins();
        return builtins.formsOf(builtins.at(call.overload()).builtin);
    }

    ValueID convertedOperandOf(const Value& call, const BuiltinForms& forms) const
    {
        std::span<const ValueID> arguments = call.argumentsOperands();
        if (forms.appendForm == InlineBuiltin::CallStringConstructorAppend)
            return arguments.size() == 1 && isStringBuilderPart(typeOf(arguments[0])) ? arguments[0] : ValueID();
        if (forms.appendForm != InlineBuiltin::NumberToStringAppend)
            return { };
        ValueID receiver = call.receiverOperand();
        bool isDecimal = arguments.empty() || (arguments.size() == 1 && integerConstantThroughCasts(m_context.program, m_function, arguments[0]) == 10);
        return receiver && typeOf(receiver).isNumber() && isDecimal ? receiver : ValueID();
    }

    void foldConversions(ValueID append)
    {
        uint32_t numberOfOperands = m_function.at(append).numberOfOperands();
        for (uint32_t operandIndex = OperandIndex::StringBuilderAppend::parts; operandIndex < numberOfOperands; ++operandIndex) {
            ValueID part = m_function.at(append).operand(operandIndex);
            const BuiltinForms* forms = appendFormsOf(append, part);
            if (!forms)
                continue;
            ValueID converted = convertedOperandOf(m_function.at(part), *forms);
            if (!converted || !onlyCreatesString(m_context.analyses.effectsOf(part).onAnyEdge()))
                continue;
            m_function.edit(append, ValueEdit::Definition).setOperand(operandIndex, converted);
            erase(part);
        }
    }

    BitVector findHostAppends(ValueID append, ProgramPoint point)
    {
        BitVector foldedOperands;
        ValueID handle = m_function.at(append).builderOperand();
        m_function.at(append).forEachOperand([&](ValueID part, uint32_t operandIndex) {
            if (operandIndex < OperandIndex::StringBuilderAppend::parts)
                return;
            const BuiltinForms* forms = appendFormsOf(append, part);
            if (forms && forms->hasHostAppendFunction && overloadCanAppend(m_context.program, m_function, makeAppendBuiltin(handle, m_function.at(part))))
                foldedOperands.set(operandIndex);
        });
        size_t numberOfCandidates = foldedOperands.bitCount();
        const BlockValues& values = m_function.at(point.block).values;
        Effects effectsUpToAppend = Effects::none();
        uint32_t firstFoldedOperand = m_function.at(append).numberOfOperands();
        for (uint32_t index = point.index; index-- && numberOfCandidates;) {
            ValueID id = values[index];
            if (m_valuesToErase.get(id.index()))
                continue;
            Effects effects = m_context.analyses.effectsOf(id).onAnyEdge();
            std::optional<Use> use = m_useLists->singleUse(id);
            if (use && use->user == append && foldedOperands.get(use->operandIndex)) {
                --numberOfCandidates;
                Effects allowedEffects = declaredEffectsOf(m_context.program, makeAppendBuiltin(handle, m_function.at(id)));
                if (use->operandIndex < firstFoldedOperand && effects.isSubsetOf(allowedEffects) && !effects.interferesWith(effectsUpToAppend)) {
                    firstFoldedOperand = use->operandIndex;
                    continue;
                }
                foldedOperands.clear(use->operandIndex);
            }
            effectsUpToAppend.merge(effects);
        }
        return foldedOperands;
    }

    void splitAppend(const HostAppendFold& fold)
    {
        Origin origin = m_function.at(fold.append).origin();
        ValueID handle = m_function.at(fold.append).builderOperand();
        uint32_t numberOfOperands = m_function.at(fold.append).numberOfOperands();
        Vector<ValueID, 8> parts;
        for (uint32_t operandIndex = OperandIndex::StringBuilderAppend::parts; operandIndex < numberOfOperands; ++operandIndex) {
            ValueID part = m_function.at(fold.append).operand(operandIndex);
            if (!fold.operands.get(operandIndex)) {
                parts.append(part);
                continue;
            }
            if (!parts.isEmpty())
                insert({ fold.point, origin }, makeAppend(handle, parts.span()));
            parts.shrink(0);
            insert({ fold.point, m_function.at(part).origin() }, makeAppendBuiltin(handle, m_function.at(part)));
            erase(part);
        }
        if (parts.isEmpty())
            erase(fold.append);
        else
            m_function.edit(fold.append, ValueEdit::Definition).setPartsOperands(parts.span());
    }

    void erase(ValueID value)
    {
        m_valuesToErase.set(value.index());
        m_blocksWithErasedValues.set(m_useLists->blockOf(value).index());
    }

    FunctionPassContext& m_context;
    const StringBuilderPlan& m_plan;
    Function& m_function;
    IRBuilder m_builder;
    const CFG* m_cfg;
    const UseLists* m_useLists;
    Vector<const StringWeb*, 4> m_createdWebs;
    TypeID m_handleType;
    Vector<ValueID> m_handleOf;
    BitVector m_blocksOnEdges;
    Materialization m_lastMaterialization;
    Vector<ValueID, 8> m_appends;
    BitVector m_valuesToErase;
    BitVector m_blocksWithErasedValues;
    bool m_didChange { false };
};

class NestedBuilderJoiner {
public:
    explicit NestedBuilderJoiner(FunctionPassContext& context)
        : m_context(context)
        , m_function(context.function)
        , m_useLists(context.analyses.useLists())
        , m_builder(context.analyses.context(), context.function, context.types)
    {
    }

    PassOutcome run()
    {
        findNestedBuilders();
        if (m_nestedBuilders.isEmpty())
            return PassOutcome::Unchanged;

        BitVector trackedValues(m_function.values.size());
        for (const NestedBuilder& nested : m_nestedBuilders) {
            trackedValues.quickSet(nested.begin.index());
            if (ValueID outer = outerBuilderOf(nested))
                trackedValues.quickSet(outer.index());
        }
        LivenessOptions options;
        options.trackedValues = &trackedValues;
        m_liveness.emplace(m_function, m_context.analyses.cfg(), m_useLists, options);

        m_nestedBuilders.removeAllMatching([&](const NestedBuilder& nested) {
            if (canJoin(nested))
                return false;
            m_context.addDecline(Wire::DeclineKind::StringBuildersNotJoined, nested.concatenation);
            return true;
        });
        if (m_nestedBuilders.isEmpty())
            return PassOutcome::Unchanged;
        for (const NestedBuilder& nested : m_nestedBuilders)
            join(nested);
        m_context.build.statistics().count(StatisticID::StringBuildersJoined, m_nestedBuilders.size());
        m_builder.applyInsertions();
        m_context.analyses.didChangeValues();
        return PassOutcome::Changed;
    }

private:
    struct NestedBuilder {
        ValueID begin;
        ValueID concatenation;
        uint32_t operandIndex { 0 };
    };

    ValueID outerBuilderOf(const NestedBuilder& nested) const
    {
        const Value& concatenation = m_function.at(nested.concatenation);
        return concatenation.opcode == Opcode::StringBuilderAppend ? concatenation.builderOperand() : ValueID();
    }

    void findNestedBuilders()
    {
        for (BlockID block : m_context.analyses.cfg().reversePostOrder()) {
            for (ValueID id : m_function.at(block).values) {
                if (m_function.at(id).opcode != Opcode::StringBuilderFinish)
                    continue;
                ValueID begin = m_function.at(id).builderOperand();
                std::optional<Use> use = findConcatenationUse(id);
                if (!use)
                    continue;
                if (m_function.at(begin).opcode != Opcode::StringBuilderBegin || !hasOnlyRedirectableUses(begin)) {
                    m_context.addDecline(Wire::DeclineKind::StringBuildersNotJoined, use->user);
                    continue;
                }
                m_nestedBuilders.append(NestedBuilder { begin, use->user, use->operandIndex });
                m_nestedBegins.set(begin.index());
            }
        }
    }

    std::optional<Use> findConcatenationUse(ValueID finish) const
    {
        bool didFollowEdge = false;
        ValueID string = finish;
        while (std::optional<Use> use = m_useLists.singleUse(string)) {
            if (use->kind == Use::Kind::EdgeArgument) {
                if (didFollowEdge || use->block != m_useLists.blockOf(finish) || m_function.at(use->block).terminator.kind != TerminatorKind::Jump)
                    return std::nullopt;
                didFollowEdge = true;
                string = m_function.at(targetOf(m_function, use->edge()).block).parameters[use->operandIndex];
                continue;
            }
            if (use->kind != Use::Kind::Operand)
                return std::nullopt;
            const Value& user = m_function.at(use->user);
            if (user.opcode == Opcode::Upcast) {
                string = use->user;
                continue;
            }
            if (user.opcode == Opcode::StringBuilderAppend ? use->operandIndex >= OperandIndex::StringBuilderAppend::parts : isStringConcatenation(m_context.program, m_function, user))
                return use;
            return std::nullopt;
        }
        return std::nullopt;
    }

    bool hasOnlyRedirectableUses(ValueID begin) const
    {
        unsigned numberOfFinishes = 0;
        for (const Use& use : m_useLists.usesOf(begin)) {
            if (use.kind != Use::Kind::Operand)
                return false;
            Opcode opcode = m_function.at(use.user).opcode;
            if (familyOf(opcode) != OperationFamily::JSONWrite && !redirectableOperations.contains(opcode))
                return false;
            if (opcode == Opcode::StringBuilderFinish)
                ++numberOfFinishes;
        }
        return numberOfFinishes == 1;
    }

    bool canJoin(const NestedBuilder& nested)
    {
        ValueID outer = outerBuilderOf(nested);
        if (m_joinedConcatenations.get(nested.concatenation.index()) || (outer && m_nestedBegins.get(outer.index())))
            return false;

        FunctionAnalyses& analyses = m_context.analyses;
        ProgramPoint start = m_useLists.pointOf(nested.begin);
        ProgramPoint end = m_useLists.pointOf(nested.concatenation);
        if (!analyses.dominates(nested.begin, end) || (outer && !analyses.dominates(outer, start)))
            return false;
        if (start.block != end.block && analyses.isInCycle(end.block) && analyses.cfg().isInCycleAvoiding(end.block, start.block))
            return false;
        bool canAppendParts = true;
        m_function.at(nested.concatenation).forEachOperand([&](ValueID part, uint32_t operandIndex) {
            if (part == outer)
                return;
            canAppendParts &= isStringBuilderPart(m_context.program.typeOf(m_function, part));
            if (operandIndex < nested.operandIndex)
                canAppendParts &= isRematerializable(m_function.at(part)) || analyses.dominates(part, start);
        });
        if (!canAppendParts || !hasOnlySafeExits(nested.begin, outer, end))
            return false;
        m_joinedConcatenations.set(nested.concatenation.index());
        return true;
    }

    // Between the begin and the end nothing else may see the outer builder, and nothing may reach the end with half a text.
    bool hasOnlySafeExits(ValueID begin, ValueID outer, ProgramPoint end)
    {
        const Dominators& dominators = m_context.analyses.dominators();
        ProgramPoint start = m_useLists.pointAfter(begin);
        BitVector closingBlocks;
        for (const Use& use : m_useLists.usesOf(begin)) {
            if (closingOperations.contains(m_function.at(use.user).opcode))
                closingBlocks.set(use.block.index());
        }

        BitVector visitedBlocks;
        visitedBlocks.set(start.block.index());
        Vector<BlockID, 8> worklist { start.block };
        bool isSafe = true;
        while (isSafe && !worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            bool isFirst = block == start.block;
            bool isLast = block == end.block;
            if (outer && isUsedIn(outer, block, isFirst ? start.index : 0, isLast ? std::optional<uint32_t>(end.index) : std::nullopt))
                return false;
            bool isClosed = closingBlocks.get(block.index()) || (!isFirst && !m_liveness->isLiveAtHead(begin, block));
            forEachOutgoingEdge(m_function, block, [&](Edge edge, const Target& target) {
                if (isLast && !edge.isHandlerEdge())
                    return;
                bool hasWholeText = isClosed && !edge.isHandlerEdge();
                bool staysInside = target.block != start.block && dominators.dominates(start.block, target.block);
                bool isFollowed = m_liveness->isLiveAtHead(begin, target.block);
                if (outer && target.arguments.contains(outer))
                    isSafe = false;
                else if (!isFollowed && !outer)
                    isSafe &= hasWholeText || !staysInside;
                else if (!isFollowed && m_liveness->isLiveAtHead(outer, target.block)) {
                    isSafe &= hasWholeText && staysInside;
                    isFollowed = true;
                }
                if (isFollowed && !visitedBlocks.set(target.block.index()))
                    worklist.append(target.block);
            });
        }
        return isSafe;
    }

    bool isUsedIn(ValueID used, BlockID id, uint32_t firstIndex, std::optional<uint32_t> endIndex) const
    {
        const Block& block = m_function.at(id);
        bool isUsed = false;
        auto visitOperand = [&](ValueID operand, uint32_t) { isUsed |= operand == used; };
        for (ValueID value : block.values.span().subspan(firstIndex, endIndex ? *endIndex - firstIndex : std::dynamic_extent))
            m_function.at(value).forEachOperand(visitOperand);
        if (!endIndex)
            block.terminator.forEachOperand(visitOperand);
        return isUsed;
    }

    void join(const NestedBuilder& nested)
    {
        ValueID outer = outerBuilderOf(nested);
        Vector<ValueID, 8> earlierParts;
        Vector<ValueID, 8> laterParts;
        m_function.at(nested.concatenation).forEachOperand([&](ValueID part, uint32_t operandIndex) {
            if (part != outer)
                (operandIndex < nested.operandIndex ? earlierParts : laterParts).append(part);
        });

        ProgramPoint start = m_useLists.pointOf(nested.begin);
        m_builder.setOrigin(m_function.at(nested.begin).origin());
        if (!outer)
            outer = m_builder.insertBefore(start, IRBuilder::make(Opcode::StringBuilderBegin));
        for (ValueID& part : earlierParts) {
            if (isRematerializable(m_function.at(part)))
                part = m_builder.insertBefore(start, Value(m_function.at(part)));
        }
        if (!earlierParts.isEmpty())
            m_builder.insertBefore(start, makeAppend(outer, earlierParts.span()));
        redirectWrites(nested.begin, outer);
        finishConcatenation(nested.concatenation, outer, laterParts.span());
    }

    void redirectWrites(ValueID begin, ValueID outer)
    {
        for (const Use& use : m_useLists.usesOf(begin)) {
            Value& user = m_function.edit(use.user);
            if (!closingOperations.contains(user.opcode)) {
                user.setOperand(use.operandIndex, outer);
                continue;
            }
            if (user.opcode == Opcode::StringBuilderFinish) {
                user.become(Opcode::String);
                user.setString(m_context.program.strings.emptyString());
            } else {
                user.become(Opcode::StringBuilderRollBack);
                user.setBuilderOperand(outer);
                user.setCheckpointOperand(begin);
            }
            retype(m_context, user);
        }
        Value& checkpoint = m_function.edit(begin);
        checkpoint.become(Opcode::StringBuilderCheckpoint);
        checkpoint.setBuilderOperand(outer);
        retype(m_context, checkpoint);
    }

    void finishConcatenation(ValueID concatenation, ValueID outer, std::span<const ValueID> laterParts)
    {
        if (m_function.at(concatenation).opcode == Opcode::StringBuilderAppend) {
            m_function.edit(concatenation, ValueEdit::Definition).setPartsOperands(laterParts);
            return;
        }
        bool isLikelyKey = m_function.at(concatenation).opcode == Opcode::Strcat && m_function.at(concatenation).isLikelyKey();
        m_builder.setOrigin(m_function.at(concatenation).origin());
        m_builder.insertBefore(m_useLists.pointOf(concatenation), makeAppend(outer, laterParts));
        Value& finish = m_function.edit(concatenation);
        finish.become(Opcode::StringBuilderFinish);
        finish.setBuilderOperand(outer);
        finish.setIsLikelyKey(isLikelyKey);
        retype(m_context, finish);
    }

    FunctionPassContext& m_context;
    Function& m_function;
    const UseLists& m_useLists;
    IRBuilder m_builder;
    std::optional<Liveness> m_liveness;
    Vector<NestedBuilder, 4> m_nestedBuilders;
    BitVector m_nestedBegins;
    BitVector m_joinedConcatenations;
};

} // anonymous namespace

static PassOutcome createAndJoinBuilders(FunctionPassContext& context)
{
    PassOutcome outcome = PassOutcome::Unchanged;
    StringBuilderPlan plan = analyzeStringBuilders(context);
    while (mergeNestedStringWebs(context, plan) || splitInFrontOfCoveredBlocks(context, plan)) {
        context.analyses.didChangeControlFlow();
        plan = analyzeStringBuilders(context);
        outcome = PassOutcome::Changed;
    }
    if (plan.webs().empty())
        return outcome;
    outcome |= StringBuilderRewriter(context, plan).run();
    outcome |= NestedBuilderJoiner(context).run();
    if (markLikelyKeys(context))
        outcome = PassOutcome::Changed;
    return outcome | expandAppendedStringRanges(context);
}

PassOutcome createStringBuilders(FunctionPassContext& context)
{
    PassOutcome outcome = createAndJoinBuilders(context);
    // Each step that edits tells the analyses when it is done, for the step behind it asks them. So whatever they hold was made behind the last edit.
    if (outcome == PassOutcome::Changed)
        context.keptAnalyses = allAnalyses;
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
