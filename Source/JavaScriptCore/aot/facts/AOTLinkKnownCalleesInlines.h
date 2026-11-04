/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// Members of CallInLinkCalls, which AOTLinkCalls.cpp declares. That file includes this one at its foot.
// Link knows no contexts: the node of a result holds what every caller gets. Where all that a result can be is what the caller has handed over, an edge from what THIS call passes stands in the place of the edge from that node.
// It goes by the summary of the callee and by the form of the call alone, so it is the same in every order of the worklist, and what it lays only ever grows.

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

// passedAt(entryParameter): what the call passes there. Nothing: nobody can tell, or it passes nothing.
// It answers nothing where the result goes by the node of the callee.
template<typename Functor>
std::optional<ValuesInLinkCalls> CallInLinkCalls::resultThatIsParameter(FunctionID callee, uint32_t index, const Functor& passedAt)
{
    const Function& function = m_program.at(callee);
    uint32_t firstPositional = function.firstParameterIndex();
    uint32_t endOfPositional = firstPositional + function.numberOfPositionalParameters();
    Values answer;
    bool isListed = false;
    for (const ThinSummary::ParameterThatIsResult& entry : m_flow.summaries.at(callee).knownEffectsApart.parametersThatAreResults()) {
        if (entry.result != index)
            continue;
        isListed = true;
        // The entry itself makes the list of a rest parameter, and says what the callee and new.target are.
        bool isThis = function.thisParameterIndex() == std::optional<unsigned>(entry.entryParameter);
        bool isPositional = entry.entryParameter >= firstPositional && entry.entryParameter < endOfPositional;
        std::optional<Values> passed = isThis || isPositional ? passedAt(entry.entryParameter) : std::nullopt;
        if (!passed)
            return std::nullopt;
        // Of two tests one is kept: what passes it holds all that passes both.
        for (const Source& source : passed->sources)
            add(answer, source.node, source.distance, source.passes ? source.passes : entry.passes);
        add(answer, entry.passes ? m_flow.passedBy(passed->made, entry.passes) : passed->made);
    }
    return isListed ? std::optional(WTF::move(answer)) : std::nullopt;
}

// `const id = x => x; id(box).width`.
ValuesInLinkCalls CallInLinkCalls::resultGivenTo(FunctionID callee, uint32_t index, const Passed& passed, const Values& constructed)
{
    // A construction answers with what it makes. What a receiver of its own brings for each layout that finds the callee is handed over once for each, and the results are laid once: enter().
    if (!passed.isConstruction && &passed == &passedByCall()) {
        const Function& function = m_program.at(callee);
        std::optional<Values> answer = resultThatIsParameter(callee, index, [&](uint32_t entryParameter) -> std::optional<Values> {
            if (function.thisParameterIndex() == std::optional<unsigned>(entryParameter))
                return passed.receiver;
            uint32_t argument = entryParameter - function.firstParameterIndex();
            return argument < passed.arguments.size() ? std::optional(passed.arguments[argument]) : std::nullopt;
        });
        if (answer)
            return WTF::move(*answer);
    }
    return resultOf(callee, index, passed.isConstruction, constructed, notObservedForLinkCalls);
}

// The arguments are the entry parameters of the callee, in their order.
ValuesInLinkCalls CallInLinkCalls::resultGivenToCallByBinding(FunctionID callee, uint32_t index, const Values& constructed)
{
    if (!m_call.isConstruction) {
        std::optional<Values> answer = resultThatIsParameter(callee, index, [&](uint32_t entryParameter) -> std::optional<Values> {
            if (entryParameter >= m_call.numberOfArguments)
                return std::nullopt;
            return valuesAtNode(m_summary.arguments[m_call.firstArgument + entryParameter]);
        });
        if (answer)
            return WTF::move(*answer);
    }
    return resultOf(callee, index, m_call.isConstruction, constructed, notObservedForLinkCalls);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
