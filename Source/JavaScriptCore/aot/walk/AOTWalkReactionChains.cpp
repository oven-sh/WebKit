/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkCoroutinePromises.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTCallingConventions.h"
#include "AOTFunctionKinds.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTReactionChain.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include "AOTWalkCoroutineState.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ChainOfReactions {
    Reference receiver;
    bool receiverIsValue { false };
    Reference promise;
    FunctionID lastHandler;
    unsigned numberOfHandlers { 0 };
    Vector<uint32_t, 16> handlers;
    Vector<Reference, 4> words;
};

struct KeptByReactionChains final : KeptForUnitOfCoroutines {
    std::optional<ChainOfReactions> chain;
};

struct LinkOfReactions {
    ValueID call;
    ValueID closure;
    FunctionID handler;
};

} // anonymous namespace

static KeptByReactionChains& keptByReactionChains(Walker& walker)
{
    std::unique_ptr<KeptForUnitOfCoroutines>& kept = walker.coroutineState().keptByReactionChains;
    if (!kept)
        kept = makeUniqueWithoutFastMallocCheck<KeptByReactionChains>();
    return static_cast<KeptByReactionChains&>(*kept);
}

static bool canBeReactionChainHandler(const Walker& walker, FunctionID id)
{
    constexpr OptionSet<FunctionFlags> otherForms { FunctionFlags::IsGenerator, FunctionFlags::HasNewTarget, FunctionFlags::TakesCallee };
    const Program& program = walker.program();
    const Function& function = program.at(id);
    const CallingConventions& conventions = walker.plan().conventions();
    CodeUnit entry = conventions.closureEntryOf(id);
    if (function.hasAny(otherForms) || isClassConstructorOf(function.kind) || !conventions.exists(entry) || conventions.entryConventionOf(entry) != EntryConvention::GenericRegisters)
        return false;
    ClosureCaptureCounts counts = closureCaptureCountsOf(program, conventions, id);
    return walker.plan().functionKind(program, entry).isSlim && counts.captures <= ReactionChain::maxCapturesOfHandler && counts.captures == counts.references;
}

static std::optional<LinkOfReactions> findLinkOfReactions(const Walker& walker, const BodyIndex& index, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || call.isDoneInPlace() || !call.thisValueOperand() || call.argumentsOperands().size() != 1)
        return std::nullopt;
    ValueID closure = call.argumentsOperands()[0];
    if (index.isParameter(closure) || index.opcodeOf(closure) != Opcode::NewClosure || index.onlyUsingValue(closure) != call.id() || index.blockOf(closure) != index.blockOf(call.id()))
        return std::nullopt;
    FunctionID handler = index.value(closure).as<Opcode::NewClosure>().function();
    return canBeReactionChainHandler(walker, handler) ? std::optional(LinkOfReactions { call.id(), closure, handler }) : std::nullopt;
}

static std::optional<bool> handlesRejectionsOf(const Walker& walker, BuiltinID builtin)
{
    const BuiltinTable& table = walker.program().builtins();
    if (!builtin)
        return std::nullopt;
    builtin = table.canonicalID(builtin);
    if (builtin == table.canonicalID(Builtins::PromisePrototypeThen))
        return false;
    if (builtin == table.canonicalID(Builtins::PromisePrototypeCatch))
        return true;
    return std::nullopt;
}

static bool isNextHandlerOfReactions(const Walker& walker, const ChainOfReactions& chain, FunctionID handler)
{
    if (!chain.numberOfHandlers)
        return true;
    return chain.numberOfHandlers < ReactionChain::maxHandlers && chain.lastHandler < handler && walker.plan().conventions().haveNeighboringClosureNumbers(chain.lastHandler, handler);
}

static bool appendToReactions(Walker& walker, const BodyIndex& index, ChainOfReactions& chain, const LinkOfReactions& link, bool handlesRejections)
{
    if (!isNextHandlerOfReactions(walker, chain, link.handler))
        return false;
    const Program& program = walker.program();
    const CallingConventions& conventions = walker.plan().conventions();
    std::span<const ValueID> captures = index.value(link.closure).as<Opcode::NewClosure>().capturesOperands();
    if (captures.size() != program.at(link.handler).captures.size())
        return false;
    Vector<Reference, 4> words(chain.words);
    std::array<uint32_t, ReactionChain::maxCapturesOfHandler> wordsBySlot { };
    unsigned numberOfSlots = 0;
    for (unsigned capture = 0; capture < captures.size(); ++capture) {
        if (closureCaptureRepresentationOf(program, conventions, link.handler, capture) == Representation::Void)
            continue;
        Reference reference = walker.referenceOf(captures[capture]);
        if (!reference)
            return false;
        size_t word = words.find(reference);
        if (word == notFound) {
            word = words.size();
            words.append(reference);
        }
        unsigned slot = closureCaptureSlotOf(program, conventions, link.handler, capture);
        if (slot >= wordsBySlot.size() || words.size() > ReactionChain::maxWords)
            return false;
        wordsBySlot[slot] = static_cast<uint32_t>(word) + 1;
        ++numberOfSlots;
    }
    for (unsigned slot = 0; slot < numberOfSlots; ++slot) {
        if (!wordsBySlot[slot])
            return false;
    }
    uint32_t bits = handlesRejections ? ReactionChain::isRejectHandlerBit : 0;
    for (unsigned slot = 0; slot < numberOfSlots; ++slot)
        bits |= wordsBySlot[slot] << slot * ReactionChain::bitsOfCapture;
    chain.words = WTF::move(words);
    chain.handlers.append(link.handler.index());
    chain.handlers.append(bits);
    chain.lastHandler = link.handler;
    ++chain.numberOfHandlers;
    return true;
}

static std::optional<LinkOfReactions> findNextLinkOfReactions(Walker& walker, const BodyIndex& index, const BodyValue& call, Reference receiver, const ChainOfReactions& chain)
{
    BodyBlock block = index.block(index.blockOf(call.id()));
    uint32_t end = block.firstValue().index() + std::min<uint32_t>(block.numberOfValues(), walker.endOfStretchBeingWalked());
    if (index.useCount(call.id()) != 2)
        return std::nullopt;
    ValueID read;
    for (uint32_t place = call.id().index() + 1; place < end; ++place) {
        BodyValue value = index.value(ValueID(place));
        switch (value.opcode()) {
        case Opcode::NewClosure:
            continue;
        case Opcode::PropGet:
            if (read || value.isFreeName() || value.objectOperand() != call.id() || !index.hasOneUse(value.id()) || !handlesRejectionsOf(walker, findConstantMemberReadOff(walker, receiver, value.key())))
                return std::nullopt;
            read = value.id();
            continue;
        case Opcode::Call: {
            if (!read || value.calleeOperand() != read || value.thisValueOperand() != call.id())
                return std::nullopt;
            std::optional<LinkOfReactions> link = findLinkOfReactions(walker, index, value);
            return link && isNextHandlerOfReactions(walker, chain, link->handler) ? link : std::nullopt;
        }
        default:
            return std::nullopt;
        }
    }
    return std::nullopt;
}

static void startReactions(Walker& walker, const ChainOfReactions& chain, Origin origin)
{
    origin.isTailCall = false;
    Draft start(Opcode::StartReactionChain, origin);
    walker.chooseConservativeFields(start);
    start.setReceiverOperand(chain.receiver);
    start.setReceiverIsValue(chain.receiverIsValue);
    start.setPromiseOperand(chain.promise);
    start.setHandlers(chain.handlers.span());
    start.setCapturesOperands(chain.words.span());
    walker.count(StatisticID::ReactionChainsBuilt);
    walker.build(WTF::move(start));
}

static Reference buildPromiseOfReactions(Walker& walker, Origin origin)
{
    origin.isTailCall = false;
    Draft allocation(Opcode::NewPromise, origin);
    walker.chooseConservativeFields(allocation);
    allocation.setEndsReactionChain(true);
    Reference promise = walker.build(WTF::move(allocation));
    if (promise && promise.isValue() && walker.has(Mechanism::EscapeAnalysis)) {
        walker.state().noteShape(promise, walker.code().at(promise.id()).type);
        walker.state().noteFresh(promise);
    }
    return promise;
}

Reference buildLinkOfReactionChain(Walker& walker, const BodyValue& call, BuiltinID builtin, Reference receiver, Origin origin)
{
    std::optional<bool> handlesRejections = handlesRejectionsOf(walker, builtin);
    if (!handlesRejections || !walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::EscapeAnalysis) || walker.isUnreachable() || !receiver)
        return { };
    const BodyIndex& index = walker.currentBodyIndex();
    KeptByReactionChains& kept = keptByReactionChains(walker);
    bool goesOn = kept.chain && receiver.isValue() && underlyingCellOf(walker.code(), receiver.id()) == kept.chain->promise.id();
    std::optional<LinkOfReactions> link = findLinkOfReactions(walker, index, call);
    if (!goesOn) {
        kept.chain = std::nullopt;
        if (!link)
            return { };
        ChainOfReactions chain;
        if (!appendToReactions(walker, index, chain, *link, *handlesRejections))
            return { };
        if (!findNextLinkOfReactions(walker, index, call, receiver, chain) || walker.isUnreachable() || walker.hasFailed())
            return { };
        Reference promise = buildPromiseOfReactions(walker, origin);
        if (!promise || !promise.isValue() || walker.hasFailed())
            return { };
        chain.promise = promise;
        Reference value = valueOfPromiseFulfilledHere(walker, receiver);
        chain.receiver = value ? value : receiver;
        chain.receiverIsValue = !!value;
        kept.chain = WTF::move(chain);
        return promise;
    }

    Reference promise = kept.chain->promise;
    if (!link || !appendToReactions(walker, index, *kept.chain, *link, *handlesRejections)) {
        startReactions(walker, *kept.chain, origin);
        kept.chain = std::nullopt;
        return { };
    }
    if (!findNextLinkOfReactions(walker, index, call, promise, *kept.chain)) {
        startReactions(walker, *kept.chain, origin);
        kept.chain = std::nullopt;
    }
    return promise;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
