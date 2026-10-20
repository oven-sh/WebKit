/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTSchemaWalkers.h"
#include "AOTStronglyConnectedComponents.h"
#include <algorithm>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Forward.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FlowResults;

enum class CallerSet : uint8_t { Unknown, Complete };
enum class CalleeSetState : uint8_t { Unknown, Complete };
enum class EntrySet : uint8_t { Unknown, DirectCallers };
enum class CallSiteSet : uint8_t { Unknown, Complete };
enum class Recursion : uint8_t { Unknown, NotRecursive };

class CallGraph {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(CallGraph);
public:
    CallGraph() = default;

    unsigned numberOfFunctions() const { return m_directCallSites.size(); }
    std::span<const FunctionID> calleesOf(FunctionID function) const LIFETIME_BOUND { return m_callees.of(function); }
    std::span<const FunctionID> callersOf(FunctionID function) const LIFETIME_BOUND { return m_callers.of(function); }
    unsigned numberOfDirectCallSites(FunctionID function) const { return function.index() < m_directCallSites.size() ? m_directCallSites[function.index()] : 0; }
    // Every operation that may enter the function: a direct call, a call by value whose callee set lists it, an access or a built-in that flow linked to it.
    unsigned numberOfCallSites(FunctionID function) const { return function.index() < m_callSites.size() ? m_callSites[function.index()] : 0; }
    // The host enters a function through an entrypoint.
    unsigned numberOfCallSitesInCode(FunctionID function) const { return function.index() < m_callSites.size() ? m_callSites[function.index()] - m_callSitesOfHost[function.index()] : 0; }
    std::span<const FunctionID> roots() const LIFETIME_BOUND { return m_roots.span(); }
    std::span<const FunctionID> functionsCalledByUnknownCode() const LIFETIME_BOUND { return m_calledByUnknownCode.span(); }

    CallerSet callerSet(FunctionID function) const { return m_hasCompleteCallerSet.get(function.index()) ? CallerSet::Complete : CallerSet::Unknown; }
    CalleeSetState calleeSet(FunctionID function) const { return m_hasCompleteCalleeSet.get(function.index()) ? CalleeSetState::Complete : CalleeSetState::Unknown; }
    EntrySet entrySet(FunctionID function) const { return m_hasOnlyDirectCallers.get(function.index()) ? EntrySet::DirectCallers : EntrySet::Unknown; }
    CallSiteSet callSiteSet(FunctionID function) const { return m_hasCompleteCallSiteSet.get(function.index()) ? CallSiteSet::Complete : CallSiteSet::Unknown; }

    Recursion recursion(FunctionID function) const
    {
        if (function.index() >= numberOfFunctions() || m_components.isInCycle.get(function.index()))
            return Recursion::Unknown;
        return Recursion::NotRecursive;
    }

    unsigned bottomUpRank(FunctionID function) const { return function.index() < numberOfFunctions() ? m_components.componentOf[function.index()] : 0; }
    void dump(PrintStream&, const Program&) const;

private:
    friend CallGraph buildCallGraph(const Program&, const FlowResults&, BuildContext&);

    struct Adjacency {
        std::span<const FunctionID> of(FunctionID function) const LIFETIME_BOUND
        {
            size_t index = function.index();
            if (index + 1 >= offsets.size())
                return { };
            return targets.subspan(offsets[index], offsets[index + 1] - offsets[index]);
        }

        Adjacency inverse() const;

        Vector<uint32_t> offsets;
        Vector<FunctionID> targets;
    };

    Adjacency m_callees;
    Adjacency m_callers;
    Vector<uint32_t> m_directCallSites;
    Vector<uint32_t> m_callSites;
    Vector<uint32_t> m_callSitesOfHost;
    Vector<FunctionID> m_roots;
    Vector<FunctionID> m_calledByUnknownCode;
    BitVector m_hasCompleteCallerSet;
    BitVector m_hasCompleteCalleeSet;
    BitVector m_hasOnlyDirectCallers;
    BitVector m_hasCompleteCallSiteSet;
    StronglyConnectedComponents m_components;
};

CallGraph buildCallGraph(const Program&, const FlowResults&, BuildContext&);

constexpr bool keepsFunctionAlive(FunctionReferenceKind kind)
{
    switch (kind) {
    case FunctionReferenceKind::Call:
    case FunctionReferenceKind::Closure:
    case FunctionReferenceKind::Constructor:
        return true;
    case FunctionReferenceKind::Dispatch:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

void forEachFunctionReference(const Program& program, const Function& function, const Invocable<void(FunctionID, FunctionReferenceKind)> auto& functor)
{
    auto visitWritten = [&](FunctionID target, FunctionReferenceKind kind) {
        if (target && keepsFunctionAlive(kind))
            functor(target, kind);
    };
    Vector<LayoutID, 16> layouts;
    Vector<CalleeSetID, 16> calleeSets;
    auto visitInstruction = [&](const auto& instruction) {
        forEachFunctionReference(instruction, visitWritten);
        forEachCalleeSet(instruction, [&](CalleeSetID set) {
            if (set && (calleeSets.isEmpty() || calleeSets.last() != set))
                calleeSets.append(set);
        });
        forEachLayout(instruction, [&](LayoutID named) {
            LayoutID layout = declaredLayoutOf(program, named);
            if (layout && (layouts.isEmpty() || layouts.last() != layout))
                layouts.append(layout);
        });
        // The value of a function declaration's variable is the closure, which the first read creates.
        forEachVariable(instruction, [&](VariableID variable) {
            if (FunctionID declaration = program.at(variable).function)
                functor(declaration, FunctionReferenceKind::Closure);
        });
    };
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values)
            visitInstruction(function.at(id));
        visitInstruction(block.terminator);
    }

    std::ranges::sort(calleeSets);
    removeRepeatedElements(calleeSets);
    for (CalleeSetID set : calleeSets) {
        for (FunctionID callee : program.at(set).functions)
            functor(callee, FunctionReferenceKind::Dispatch);
    }

    std::ranges::sort(layouts);
    removeRepeatedElements(layouts);
    for (LayoutID id : layouts) {
        const Layout& layout = program.at(id);
        if (layout.constructor)
            functor(layout.constructor, FunctionReferenceKind::Constructor);
        for (const ClassMember& member : layout.classFlow.members)
            functor(member.function, FunctionReferenceKind::Closure);
        for (const ClassMember& member : layout.classFlow.namedMembers) {
            if (member.function)
                functor(member.function, FunctionReferenceKind::Closure);
        }
    }

    // An entrypoint enters its target by a call in its body, or holds a copy of it.
    const auto* entrypoint = function.creator.tryAs<CreatorRecord::Entrypoint>();
    forEachFunctionReference(function.creator, [&](FunctionID target, FunctionReferenceKind kind) {
        if (!entrypoint || target != entrypoint->target)
            visitWritten(target, kind);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
