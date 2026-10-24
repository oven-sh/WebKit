/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayChains.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include "AOTResolveCallsCallees.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class ArrayChainFinder {
    WTF_MAKE_NONCOPYABLE(ArrayChainFinder);
public:
    ArrayChainFinder(FunctionPassContext&, const BitVector* callsTakenAlone);

    Vector<ArrayChain> run();

private:
    using Links = Vector<ArrayMethodCall, ChainShape::maxNumberOfLinks>;

    void tryChain(BlockID, const ArrayMethodCall& last);
    bool isWorthFusingIn(const ArrayChain&);
    bool paysAlone(const ArrayMethodCall&) const;
    bool takeEnd(ArrayChain&, const ArrayMethodCall& last);
    bool takeRange(ArrayChain&, const ArrayMethodCall& slice);
    ValueID collectLinks(ArrayChain&, const ArrayMethodCall& last, Links&);
    bool prependCallback(ArrayChain&, const ArrayMethodCall&, ValueID thisArgument);

    bool isUsedOnlyBy(ValueID receiver, const ArrayMethodCall& consumer);
    bool isPassingCast(ValueID cast);
    FunctionID calleeOf(ValueID callback) const;
    const Type& typeOf(ValueID id) const { return m_context.program.typeOf(m_function, id); }

    FunctionPassContext& m_context;
    const Function& m_function;
    FunctionAnalyses& m_analyses;
    const BitVector* m_callsTakenAlone;
    BitVector m_isInChain;
    Vector<ArrayChain> m_chains;
};

ArrayChainFinder::ArrayChainFinder(FunctionPassContext& context, const BitVector* callsTakenAlone)
    : m_context(context)
    , m_function(context.function)
    , m_analyses(context.analyses)
    , m_callsTakenAlone(callsTakenAlone)
{
}

Vector<ArrayChain> ArrayChainFinder::run()
{
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        const BlockValues& values = m_function.at(block).values;
        for (size_t index = values.size(); index--;) {
            ValueID id = values[index];
            if (m_isInChain.get(id.index()))
                continue;
            if (std::optional<ArrayMethodCall> call = arrayMethodCallOf(m_context.program, m_function, id))
                tryChain(block, *call);
        }
    }
    return WTF::move(m_chains);
}

// The loop of the traversal runs once for each element, however seldom the chain itself is reached. In cold code it pays where each callback goes into the loop.
bool ArrayChainFinder::isWorthFusingIn(const ArrayChain& chain)
{
    if (m_analyses.blockFrequencies().classOf(chain.block) == BlockFrequency::Rare || m_context.runsAtMostOnce(chain.block))
        return false;
    if (!isCold(heatClassOf(m_context, chain.block)))
        return true;
    // Outside the loop the text is put together by calls, which cost what `join` costs and more.
    if (m_context.inputs.heat.isCompact(m_context.program, m_context.id) || chain.shape.end == ChainEndKind::Join)
        return false;
    return !chain.callbacks.isEmpty() && std::ranges::all_of(chain.callbacks, [](const ChainCallback& callback) {
        return !!callback.callee;
    });
}

void ArrayChainFinder::tryChain(BlockID block, const ArrayMethodCall& last)
{
    ArrayChain chain;
    chain.block = block;
    chain.lastCall = last.call;

    Links linksFromLast;
    switch (last.method->role) {
    case ChainMethodRole::Range:
        return;
    case ChainMethodRole::Link:
        chain.shape.end = ChainEndKind::Array;
        linksFromLast.append(last);
        break;
    case ChainMethodRole::End:
        chain.shape.end = last.method->end;
        if (!takeEnd(chain, last))
            return;
        break;
    }

    chain.source = collectLinks(chain, last, linksFromLast);

    size_t numberOfLinks = linksFromLast.size();
    if (numberOfLinks + (chain.rangeCall ? 1 : 0) + (chain.shape.end != ChainEndKind::Array ? 1 : 0) < 2 && !paysAlone(last))
        return;

    for (size_t i = 0; i < numberOfLinks; ++i) {
        const ArrayMethodCall& link = linksFromLast[i];
        if (!prependCallback(chain, link, link.argumentOrNull(1)))
            return;
        chain.shape.links[numberOfLinks - 1 - i] = link.method->link;
    }
    if (!isWorthFusingIn(chain))
        return;

    m_isInChain.set(chain.lastCall.index());
    if (chain.rangeCall)
        m_isInChain.set(chain.rangeCall.index());
    for (const ArrayMethodCall& link : linksFromLast)
        m_isInChain.set(link.call.index());
    m_chains.append(WTF::move(chain));
}

// The traversal knows how long the array of a filter or of a flatMap can get, and gives the array of a map storage for the numbers that the callback returns.
bool ArrayChainFinder::paysAlone(const ArrayMethodCall& call) const
{
    if (!m_callsTakenAlone || !m_callsTakenAlone->get(call.call.index()))
        return false;
    if (call.method->role == ChainMethodRole::End)
        return call.method->end == ChainEndKind::FlatMap;
    switch (call.method->link) {
    case ChainLinkKind::None:
        return false;
    case ChainLinkKind::Filter:
        return true;
    case ChainLinkKind::Map:
        break;
    }
    FunctionID callee = call.arguments.isEmpty() ? FunctionID() : calleeOf(call.arguments[0]);
    if (!callee || m_context.program.at(callee).results.size() != 1)
        return false;
    return arrayStorageAtAllocation(m_context.program.at(m_context.program.at(callee).results[0].type)) != ArrayStorage::Contiguous;
}

bool ArrayChainFinder::takeEnd(ArrayChain& chain, const ArrayMethodCall& last)
{
    switch (last.method->end) {
    case ChainEndKind::Array:
    case ChainEndKind::Length:
        return true;
    case ChainEndKind::ForEach:
    case ChainEndKind::Some:
    case ChainEndKind::Every:
    case ChainEndKind::Find:
    case ChainEndKind::FindIndex:
    case ChainEndKind::FlatMap:
        return prependCallback(chain, last, last.argumentOrNull(1));
    case ChainEndKind::Reduce:
        if (last.arguments.size() >= 2) {
            chain.shape.hasInitialValue = true;
            chain.endOperand = last.arguments[1];
        }
        return prependCallback(chain, last, { });
    case ChainEndKind::Join:
        if (last.arguments.isEmpty())
            return true;
        if (last.arguments.size() > 1 || !typeOf(last.arguments[0]).isAnyString())
            return false;
        chain.endOperand = last.arguments[0];
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool ArrayChainFinder::takeRange(ArrayChain& chain, const ArrayMethodCall& slice)
{
    ValueID from = slice.argumentOrNull(0);
    ValueID to = slice.argumentOrNull(1);
    if ((from && !typeOf(from).isInteger()) || (to && !typeOf(to).isInteger()))
        return false;

    chain.rangeCall = slice.call;
    chain.from = from;
    chain.to = to;
    return true;
}

ValueID ArrayChainFinder::collectLinks(ArrayChain& chain, const ArrayMethodCall& last, Links& linksFromLast)
{
    ArrayMethodCall consumer = last;
    while (true) {
        ValueID receiver = consumer.receiver;
        ValueID producer = m_analyses.sameObjectThroughCasts(receiver);
        std::optional<ArrayMethodCall> link = arrayMethodCallOf(m_context.program, m_function, producer);
        if (!link || m_analyses.useLists().blockOf(producer) != chain.block || !isUsedOnlyBy(receiver, consumer))
            return receiver;

        switch (link->method->role) {
        case ChainMethodRole::End:
            return receiver;
        case ChainMethodRole::Range:
            return takeRange(chain, *link) ? link->receiver : receiver;
        case ChainMethodRole::Link:
            if (linksFromLast.size() == ChainShape::maxNumberOfLinks)
                return receiver;
            linksFromLast.append(*link);
            consumer = WTF::move(*link);
            break;
        }
    }
}

bool ArrayChainFinder::prependCallback(ArrayChain& chain, const ArrayMethodCall& call, ValueID thisArgument)
{
    ValueID function = call.argumentOrNull(0);
    if (!function)
        return false;
    chain.callbacks.insert(0, ChainCallback { call.call, function, thisArgument, calleeOf(function) });
    return true;
}

bool ArrayChainFinder::isUsedOnlyBy(ValueID receiver, const ArrayMethodCall& consumer)
{
    CastChain casts(m_function, receiver);
    for (ValueID argument : consumer.arguments) {
        if (m_analyses.sameObjectThroughCasts(argument) == casts.last())
            return false;
    }

    const UseLists& useLists = m_analyses.useLists();
    ValueID user = consumer.call;
    for (ValueID value : casts) {
        if (value != casts.last() && !isPassingCast(value))
            return false;
        for (const Use& use : useLists.usesOf(value)) {
            if (use.kind != Use::Kind::Operand)
                return false;
            // A validating build looks at each array of the chain, and goes without the look where there is no array.
            bool isValidation = m_function.at(use.user).opcode == Opcode::ValidateType && use.block == useLists.blockOf(value);
            if (!isValidation && use.user != user && use.user != consumer.call)
                return false;
        }
        user = value;
    }
    return true;
}

bool ArrayChainFinder::isPassingCast(ValueID cast)
{
    if (!m_analyses.mayThrow(cast))
        return true;
    const ClassPart* part = typeOf(cast).singleBuiltinClass();
    return part && part->builtinClass == BuiltinClasses::Array;
}

FunctionID ArrayChainFinder::calleeOf(ValueID callback) const
{
    CalleeCandidates candidates = calleeCandidates(m_context.program, m_function, callback);
    if (!candidates.isProven() || candidates.proven.size() != 1 || candidates.hasOtherCallees || candidates.mayBeNullish)
        return { };
    return candidates.proven[0];
}

} // anonymous namespace

Vector<ArrayChain> findArrayChains(FunctionPassContext& context, const BitVector* callsTakenAlone)
{
    return ArrayChainFinder(context, callsTakenAlone).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
