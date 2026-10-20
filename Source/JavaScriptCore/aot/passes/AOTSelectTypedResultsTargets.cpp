/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectTypedResultsTargets.h"

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "AOTExpandOwnKeysOrders.h"
#include "AOTSelectTypedResultsTypeNodes.h"
#include "AOTUseLists.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ClaimedType {
    TypeID type;
    KnowledgeLevel level { KnowledgeLevel::Unknown };
};

using ClaimedTypes = Vector<ClaimedType, 4>;
using TypePair = std::pair<TypeID, TypeID>;

} // anonymous namespace

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

static void consider(const Program& program, TypeID id, KnowledgeLevel level, ClaimedTypes& claimedTypes)
{
    if (!id || TypeTable::isProvisional(id))
        return;
    const Type& type = program.at(id);
    if (membersOf(program, type).empty() && !type.findClassPart(BuiltinClasses::Array))
        return;
    claimedTypes.append(ClaimedType { id, level });
}

static void considerCast(const Program& program, const Value& cast, ClaimedTypes& claimedTypes)
{
    if (cast.opcode == Opcode::Claim)
        consider(program, cast.castType(), KnowledgeLevel::Written, claimedTypes);
    else if (cast.opcode == Opcode::Predict)
        consider(program, cast.predicted(), KnowledgeLevel::Predicted, claimedTypes);
}

static void considerTailCasts(const Program& program, const Terminator& terminator, ClaimedTypes& claimedTypes)
{
    if (terminator.kind != TerminatorKind::ReturnCall)
        return;
    for (TailCast cast : terminator.casts()) {
        consider(program, cast.declared.type, KnowledgeLevel::Written, claimedTypes);
        consider(program, expectationOf(cast.declared), KnowledgeLevel::Predicted, claimedTypes);
    }
}

static ClaimedTypes collectClaimedTypes(const Program& program, const Function& function, const UseLists& uses, ValueID call)
{
    struct FollowedValue {
        ValueID value;
        bool mayBePromise;
    };

    ClaimedTypes claimedTypes;
    Vector<FollowedValue, 4> worklist { { call, mayReturnPromise(typedResultFormOf(program, function.at(call))) } };
    BitVector visited;
    visited.set(call.index());

    while (!worklist.isEmpty()) {
        FollowedValue followed = worklist.takeLast();
        for (const Use& use : uses.usesOf(followed.value)) {
            switch (use.kind) {
            case Use::Kind::Operand: {
                const Value& user = function.at(use.user);
                bool isSameValue = isCast(user.opcode);
                bool isResolvedValue = followed.mayBePromise && isAwaitedOperand(user, use.operandIndex);
                if (!isSameValue && !isResolvedValue)
                    break;
                considerCast(program, user, claimedTypes);
                if (visited.add(use.user.index()))
                    worklist.append({ use.user, followed.mayBePromise && isSameValue });
                break;
            }
            case Use::Kind::TerminatorOperand:
                considerTailCasts(program, function.at(use.block).terminator, claimedTypes);
                break;
            case Use::Kind::EdgeArgument:
            case Use::Kind::HandlerArgument:
            case Use::Kind::BlockSubject:
                break;
            }
        }
    }
    return claimedTypes;
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

static ClaimedType findMostSpecific(const Program& program, std::span<const ClaimedType> claimedTypes)
{
    ClaimedType best;
    for (const ClaimedType& claimed : claimedTypes) {
        if (!best.type || isDeepSubset(program, claimed.type, best.type))
            best = claimed;
    }
    for (const ClaimedType& claimed : claimedTypes) {
        if (!isDeepSubset(program, best.type, claimed.type))
            return { };
        if (claimed.type == best.type && claimed.level == KnowledgeLevel::Written)
            best.level = KnowledgeLevel::Written;
    }
    return best;
}

TypedResultTarget findTypedResultTarget(const Program& program, const Function& function, const UseLists& uses, ValueID call)
{
    ClaimedTypes claimedTypes = collectClaimedTypes(program, function, uses, call);
    ClaimedType target = findMostSpecific(program, claimedTypes.span());
    if (!target.type)
        return { };
    return { target.type, target.level, layoutsReachedBy(program, target.type) };
}

Vector<LayoutID, 4> layoutsReachedBy(const Program& program, TypeID root)
{
    Vector<LayoutID, 4> layouts;
    HashSet<LayoutID> visitedLayouts;
    HashSet<TypeID> visitedTypes;
    Vector<TypeID, 8> worklist;
    auto visit = [&](TypeID type) {
        if (type && visitedTypes.add(type).isNewEntry)
            worklist.append(type);
    };

    visit(root);
    while (!worklist.isEmpty()) {
        for (const TypeAlternative& alternative : alternativesOf(program, worklist.takeLast())) {
            switch (alternative.shape) {
            case ResultShape::Array:
                visit(alternative.contents);
                break;
            case ResultShape::Object:
                if (!visitedLayouts.add(alternative.layout).isNewEntry)
                    break;
                layouts.append(alternative.layout);
                for (const Field& field : program.at(alternative.layout).fields) {
                    if (isEnumerableStringField(program, field))
                        visit(expectationOf(field.declared));
                }
                break;
            case ResultShape::Unknown:
            case ResultShape::Never:
            case ResultShape::Undefined:
            case ResultShape::Null:
            case ResultShape::AnyObject:
            case ResultShape::String:
            case ResultShape::Number:
            case ResultShape::Boolean:
            case ResultShape::BigInt:
            case ResultShape::BooleanLiteral:
            case ResultShape::NumberLiteral:
            case ResultShape::StringLiteral:
            case ResultShape::Tuple:
            case ResultShape::Dictionary:
            case ResultShape::Union:
                break;
            }
        }
    }
    std::ranges::sort(layouts);
    return layouts;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
