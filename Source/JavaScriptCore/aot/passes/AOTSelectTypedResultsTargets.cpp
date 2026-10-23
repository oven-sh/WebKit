/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectTypedResultsTargets.h"

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "AOTCFG.h"
#include "AOTExpandOwnKeysOrders.h"
#include "AOTSelectTypedResultsTypeNodes.h"
#include "AOTSort.h"
#include "AOTUseLists.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

using RequestedTypes = Vector<TypeID, 4>;
using TypePair = std::pair<TypeID, TypeID>;

TypedResultForm typedResultFormOf(const Program& program, const Value& call)
{
    if (call.opcode != Opcode::CallBuiltin)
        return TypedResultForm::None;
    return program.builtins().at(call.overload()).result.typedResultForm;
}

ValueID typedResultSourceOf(const Program& program, const Value& call)
{
    if (call.opcode != Opcode::CallBuiltin)
        return { };
    BuiltinCall builtinCall = builtinCallOf(program, call);
    std::optional<BuiltinOperand> source = program.builtins().at(builtinCall.overload).result.typedResultSource;
    if (!source)
        return { };
    return builtinCall.operandAt(*source);
}

static bool mayReturnPromise(TypedResultForm form)
{
    switch (form) {
    case TypedResultForm::ParsedBody:
    case TypedResultForm::Rows:
        return true;
    case TypedResultForm::None:
        // Flow asks about calls that are not resolved to a built-in yet, and those record no form.
        return true;
    case TypedResultForm::ParsedText:
    case TypedResultForm::ValidatedValue:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isAwaitedOperand(const Value& user, uint32_t operandIndex)
{
    if (user.opcode == Opcode::Await)
        return operandIndex == OperandIndex::Await::value;
    return user.opcode == Opcode::AwaitBySavingFrame && operandIndex == OperandIndex::AwaitBySavingFrame::awaited;
}

static void consider(const Program& program, TypeID id, RequestedTypes& requestedTypes)
{
    if (!id || TypeTable::isProvisional(id))
        return;
    const Type& type = program.at(id);
    if (membersOf(program, type).empty() && !type.findClassPart(BuiltinClasses::Array))
        return;
    requestedTypes.append(id);
}

static void considerCast(const Program& program, const Value& cast, RequestedTypes& requestedTypes)
{
    if (cast.opcode == Opcode::Check)
        consider(program, cast.castType(), requestedTypes);
    if (TypeID expected = cast.expected())
        consider(program, expected, requestedTypes);
}

// AddGenericVersion makes a test and a branch of a Check.
static void considerTest(const Program& program, const Value& test, uint32_t operandIndex, RequestedTypes& requestedTypes)
{
    if (test.opcode == Opcode::TypeTest && operandIndex == OperandIndex::TypeTest::value)
        consider(program, test.testedType(), requestedTypes);
}

static void considerArgument(const Program& program, const Value& call, uint32_t operandIndex, RequestedTypes& requestedTypes)
{
    if (call.opcode != Opcode::CallDirect)
        return;
    std::optional<uint32_t> entryParameter = call.variadicOperandIndex(operandIndex);
    const Function& callee = program.at(call.function());
    if (!entryParameter || *entryParameter >= callee.numberOfEntryParameters())
        return;
    if (callee.entryParameterKind(*entryParameter) != EntryParameter::Parameter)
        return;
    consider(program, callee.declaredEntryParameter(*entryParameter)->type, requestedTypes);
}

static void considerTailCasts(const Program& program, const Terminator& terminator, RequestedTypes& requestedTypes)
{
    if (terminator.kind != TerminatorKind::ReturnCall)
        return;
    for (TailCast cast : terminator.casts())
        consider(program, cast.declared.type, requestedTypes);
}

static RequestedTypes collectRequestedTypes(const Program& program, const Function& function, const UseLists& uses, ValueID call)
{
    struct FollowedValue {
        ValueID value;
        bool mayBePromise;
    };

    RequestedTypes requestedTypes;
    Vector<FollowedValue, 4> worklist { { call, mayReturnPromise(typedResultFormOf(program, function.at(call))) } };
    BitVector visited;
    visited.set(call.index());

    while (!worklist.isEmpty()) {
        FollowedValue followed = worklist.takeLast();
        for (const Use& use : uses.usesOf(followed.value)) {
            switch (use.kind) {
            case Use::Kind::Operand: {
                const Value& user = function.at(use.user);
                considerTest(program, user, use.operandIndex, requestedTypes);
                considerArgument(program, user, use.operandIndex, requestedTypes);
                bool isSameValue = isCast(user.opcode) && !use.operandIndex;
                bool isResolvedValue = followed.mayBePromise && isAwaitedOperand(user, use.operandIndex);
                if (!isSameValue && !isResolvedValue)
                    break;
                considerCast(program, user, requestedTypes);
                if (visited.add(use.user.index()))
                    worklist.append({ use.user, followed.mayBePromise && isSameValue });
                break;
            }
            case Use::Kind::TerminatorOperand:
                considerTailCasts(program, function.at(use.block).terminator, requestedTypes);
                break;
            case Use::Kind::EdgeArgument:
            case Use::Kind::HandlerArgument:
            case Use::Kind::BlockSubject:
                break;
            }
        }
    }
    return requestedTypes;
}

static bool isDeepSubset(const Program& program, TypeID narrow, TypeID wide)
{
    Vector<TypePair, 4> worklist;
    HashSet<TypePair> visited;
    worklist.append(TypePair { narrow, wide });
    while (!worklist.isEmpty()) {
        TypePair pair = worklist.takeLast();
        if (pair.first == pair.second || !visited.add(pair).isNewEntry)
            continue;
        const Type& narrowType = program.at(pair.first);
        const Type& wideType = program.at(pair.second);
        if (!narrowType.isSubsetOf(wideType, program))
            return false;
        for (const ClassPart& part : narrowType.classes) {
            const ClassPart* partner = wideType.findClassPart(part.builtinClass);
            if (!partner)
                continue;
            for (unsigned index = 0; index < partner->contents.size(); ++index)
                worklist.append(TypePair { part.contentsAt(index), partner->contentsAt(index) });
        }
    }
    return true;
}

static TypeID findMostSpecific(const Program& program, std::span<const TypeID> requestedTypes)
{
    TypeID best;
    for (TypeID requested : requestedTypes) {
        if (!best || isDeepSubset(program, requested, best))
            best = requested;
    }
    for (TypeID requested : requestedTypes) {
        if (!isDeepSubset(program, best, requested))
            return { };
    }
    return best;
}

std::span<const LayoutID> HeadLayoutsOfFamilies::of(const Program& program, const Type& type) const
{
    {
        Locker locker { m_lock };
        if (auto found = m_headsOfFamily.find(type.layouts); found != m_headsOfFamily.end())
            return found->value->span();
    }
    auto heads = makeUniqueWithoutFastMallocCheck<Vector<LayoutID, 4>>(headLayoutsOf(program, type));
    Locker locker { m_lock };
    return m_headsOfFamily.ensure(type.layouts, [&] {
        return WTF::move(heads);
    }).iterator->value->span();
}

TypeID findTypeOfTypedResultTarget(const Program& program, const Function& function, const UseLists& uses, ValueID call)
{
    RequestedTypes requestedTypes = collectRequestedTypes(program, function, uses, call);
    return findMostSpecific(program, requestedTypes.span());
}

TypedResultTarget findTypedResultTarget(const Program& program, const Function& function, const UseLists& uses, ValueID call, size_t maxNumberOfLayouts, const HeadLayoutsOfFamilies* headLayouts)
{
    RequestedTypes requestedTypes = collectRequestedTypes(program, function, uses, call);
    TypeID target = findMostSpecific(program, requestedTypes.span());
    if (!target)
        return { };
    return { target, layoutsReachedBy(program, target, maxNumberOfLayouts, headLayouts) };
}

Vector<LayoutID, 4> layoutsReachedBy(const Program& program, TypeID root, size_t maxNumberOfLayouts, const HeadLayoutsOfFamilies* headLayouts)
{
    Vector<LayoutID, 4> layouts;
    auto append = [&](LayoutID layout) {
        layouts.append(layout);
        return layouts.size() > maxNumberOfLayouts ? IterationStatus::Done : IterationStatus::Continue;
    };
    forEachLayoutReachedBy(program, root, headLayouts, ScopedLambda<IterationStatus(LayoutID)>(append));
    AOT::sort(layouts);
    return layouts;
}

void forEachLayoutReachedBy(const Program& program, TypeID root, const HeadLayoutsOfFamilies* headLayouts, const ScopedLambda<IterationStatus(LayoutID)>& functor)
{
    HashSet<LayoutID> visitedLayouts;
    HashSet<LayoutSetID> visitedFamilies;
    HashSet<TypeID> visitedTypes;
    Vector<TypeID, 8> worklist;
    auto visit = [&](TypeID type) {
        if (type && visitedTypes.add(type).isNewEntry)
            worklist.append(type);
    };

    visit(root);
    while (!worklist.isEmpty()) {
        const Type& type = program.at(worklist.takeLast());
        if (const ClassPart* array = type.findClassPart(BuiltinClasses::Array))
            visit(array->contentsAt(program.builtins(), ContentsSlot::Element));
        if (!type.layouts || !visitedFamilies.add(type.layouts).isNewEntry)
            continue;
        Vector<LayoutID, 4> headsOfThisWalk;
        if (!headLayouts)
            headsOfThisWalk = headLayoutsOf(program, type);
        for (LayoutID layout : headLayouts ? headLayouts->of(program, type) : headsOfThisWalk.span()) {
            if (!visitedLayouts.add(layout).isNewEntry)
                continue;
            if (functor(layout) == IterationStatus::Done)
                return;
            for (const Field& field : program.at(layout).fields) {
                if (isEnumerableStringField(program, field))
                    visit(expectationOf(field.declared));
            }
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
