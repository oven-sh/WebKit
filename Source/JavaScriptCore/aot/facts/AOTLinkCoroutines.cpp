/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkCoroutines.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTLinkTypesOnAllThreads.h"
#include "AOTObjects.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

namespace {

} // anonymous namespace

const LinkedCoroutine* LinkTables::findCoroutine(FunctionID function) const
{
    auto found = std::ranges::lower_bound(coroutines, function, { }, &LinkedCoroutine::function);
    return found != coroutines.end() && found->function == function ? &*found : nullptr;
}

static TypeID typeHeldInLinkCoroutines(const LinkFlow& flow, TypeArena& types, LinkType held)
{
    if (held.isNever())
        return TypeTable::never;
    if (held.isAnything())
        return { };
    held = flow.withoutObjectsThatHaveEscaped(held);

    TypeID written = flow.toTypeID(types, held);
    if (!written || holdsOnlyPrimitives(written))
        return written;
    return types.at(written).isAnything() ? TypeID() : written;
}

static uint32_t findNodeOfWhatBodyReturnsInLinkCoroutines(const ThinSummary& summary)
{
    for (uint32_t index = 1; index < summary.nodes.size(); ++index) {
        const SummaryNode& node = summary.nodes[index];
        if (node.kind == SummaryNode::Kind::Contents && node.second == SlotOfContents::returnedAlone().number())
            return index;
    }
    return noSummaryNode;
}

static thread_local TypesOfThreadInLink s_typesOfThreadInLinkCoroutines;

void writeCoroutinesOfLink(const Program& program, const LinkFlow& flow, TypeArena& types, LinkTables& tables)
{
    const BuiltinTable& builtins = program.builtins();
    bool promisesFindThenOfTable = std::ranges::all_of(builtins.keysReadBy(Protocol::Thenable), [&](BuiltinName name) {
        BuiltinID member = builtins.findForInstance(BuiltinClasses::Promise, name).member;
        return member && !program.isUnlocked(member) && !program.isUnlocked(builtins.canonicalID(member));
    });

    bool writesWhatIsExpected = flow.reading() == WhatUnseenCodeDoes::OnlyReadsAndCalls;

    Vector<uint32_t> coroutines = findIndicesInParallel(flow.loopsOfReading(), static_cast<unsigned>(program.functions.size()), [&](unsigned index) {
        FunctionID id(index);
        const Function& function = program.at(id);
        if (!function.isCoroutine() || !function.hasBody() || function.isModuleBody() || function.results.size() != 1 || !flow.reached.get(index))
            return false;
        return flow.typeAt(flow.result(id, 0)).classes == LinkType::Classes::Site;
    });
    auto typesOfThread = [&]() -> TypeArena& {
        return s_typesOfThreadInLinkCoroutines.types ? *s_typesOfThreadInLinkCoroutines.types : types;
    };
    tables.coroutines.fill(LinkedCoroutine { }, coroutines.size());
    writeTypedPositionsOnAllThreads(flow, flow.program.types, static_cast<unsigned>(coroutines.size()), [](TypesOfThreadInLink typesHanded) {
        s_typesOfThreadInLinkCoroutines = typesHanded;
    }, [&](unsigned place) {
        uint32_t index = coroutines[place];
        FunctionID id(index);
        const Function& function = program.at(id);
        const LinkType& objects = flow.typeAt(flow.result(id, 0));
        bool isGenerator = function.has(FunctionFlags::IsGenerator);
        uint32_t returned = findNodeOfWhatBodyReturnsInLinkCoroutines(flow.summaries.at(id));
        LinkedCoroutine coroutine;
        coroutine.function = id;
        if (returned != noSummaryNode)
            coroutine.returned = typeHeldInLinkCoroutines(flow, typesOfThread(), flow.typeAt(flow.nodeOf(id, returned)));

        if (FlowNodeID value = !writesWhatIsExpected || isGenerator || !promisesFindThenOfTable ? FlowNodeID() : flow.findContentsOf(objects, ContentsSlot::Value))
            coroutine.fulfilledWith = typeHeldInLinkCoroutines(flow, typesOfThread(), flow.typeAt(value));

        tables.coroutines[place] = coroutine;
        return isOfProgramInLink(coroutine.returned) && isOfProgramInLink(coroutine.fulfilledWith);
    });
}

ALWAYS_INLINE_INTO_OTHER_FILES void forEachFunctionNamedByCoroutinesCalledBy(const Program& program, const LinkTables& tables, FunctionID referrer, const ScopedLambda<void(FunctionID)>& functor)
{
    if (tables.coroutines.isEmpty())
        return;
    TypeReader reader(program);
    for (const LinkedCall& call : tables.callsOf(referrer)) {
        bool calleeIsCoroutine = call.callee && call.callee.index() < program.functions.size() && program.at(call.callee).isCoroutine();
        const LinkedCoroutine* coroutine = calleeIsCoroutine ? tables.findCoroutine(call.callee) : nullptr;
        if (!coroutine)
            continue;
        for (TypeID type : { coroutine->fulfilledWith, coroutine->returned }) {
            if (type && !holdsOnlyPrimitives(type))
                forEachFunctionOf(reader, reader.at(type), functor);
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
