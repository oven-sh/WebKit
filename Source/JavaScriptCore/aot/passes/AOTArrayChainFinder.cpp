/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayChains.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include "AOTResolveCallsCallees.h"
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class ChainFinder {
    WTF_MAKE_NONCOPYABLE(ChainFinder);
public:
    explicit ChainFinder(FunctionPassContext&);

    Vector<ArrayChain> run();

private:
    using Links = Vector<ArrayMethodCall, ChainShape::maxNumberOfLinks>;

    void tryChain(BlockID, const ArrayMethodCall& last);
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
    BitVector m_isInChain;
    Vector<ArrayChain> m_chains;
};

ChainFinder::ChainFinder(FunctionPassContext& context)
    : m_context(context)
    , m_function(context.function)
    , m_analyses(context.analyses)
{
}

Vector<ArrayChain> ChainFinder::run()
{
    const BlockFrequencies& frequencies = m_analyses.blockFrequencies();
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        if (frequencies.classOf(block) == BlockFrequency::Rare || m_context.runsAtMostOnce(block) || isCold(heatClassOf(m_context, block)))
            continue;

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

void ChainFinder::tryChain(BlockID block, const ArrayMethodCall& last)
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
    if (numberOfLinks + (chain.rangeCall ? 1 : 0) + (chain.shape.end != ChainEndKind::Array ? 1 : 0) < 2)
        return;

    for (size_t i = 0; i < numberOfLinks; ++i) {
        const ArrayMethodCall& link = linksFromLast[i];
        if (!prependCallback(chain, link, link.argumentOrNull(1)))
            return;
        chain.shape.links[numberOfLinks - 1 - i] = link.method->link;
    }

    m_isInChain.set(chain.lastCall.index());
    if (chain.rangeCall)
        m_isInChain.set(chain.rangeCall.index());
    for (const ArrayMethodCall& link : linksFromLast)
        m_isInChain.set(link.call.index());
    m_chains.append(WTF::move(chain));
}

bool ChainFinder::takeEnd(ArrayChain& chain, const ArrayMethodCall& last)
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

bool ChainFinder::takeRange(ArrayChain& chain, const ArrayMethodCall& slice)
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

ValueID ChainFinder::collectLinks(ArrayChain& chain, const ArrayMethodCall& last, Links& linksFromLast)
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

bool ChainFinder::prependCallback(ArrayChain& chain, const ArrayMethodCall& call, ValueID thisArgument)
{
    ValueID function = call.argumentOrNull(0);
    if (!function)
        return false;
    chain.callbacks.insert(0, ChainCallback { call.call, function, thisArgument, calleeOf(function) });
    return true;
}

bool ChainFinder::isUsedOnlyBy(ValueID receiver, const ArrayMethodCall& consumer)
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
            if (use.kind != Use::Kind::Operand || (use.user != user && use.user != consumer.call))
                return false;
        }
        user = value;
    }
    return true;
}

bool ChainFinder::isPassingCast(ValueID cast)
{
    if (!m_analyses.mayThrow(cast))
        return true;
    const ClassPart* part = typeOf(cast).singleBuiltinClass();
    return part && part->builtinClass == BuiltinClasses::Array;
}

FunctionID ChainFinder::calleeOf(ValueID callback) const
{
    CalleeCandidates candidates = calleeCandidates(m_context.program, m_context.inputs.flow, m_function, callback, CalleeSetID(), AccessSiteID());
    if (!candidates.isProven() || candidates.proven.size() != 1 || candidates.hasOtherCallees || candidates.mayBeNullish)
        return { };
    return candidates.proven[0];
}

} // anonymous namespace

Vector<ArrayChain> findArrayChains(FunctionPassContext& context)
{
    return ChainFinder(context).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
