/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyAccessMembers.h"

#if ENABLE(AOT)

#include "AOTPropertyAccessCache.h"
#include <wtf/CommaPrinter.h>

namespace JSC { namespace AOT { namespace Typed {

static bool haveSameMember(const MemberAnswer& a, const MemberAnswer& b)
{
    if (a.kind != b.kind)
        return false;
    switch (a.kind) {
    case MemberKind::Unknown:
        return false;
    case MemberKind::Absent:
    case MemberKind::Field:
        return true;
    case MemberKind::Method:
        return a.method == b.method;
    case MemberKind::Accessor:
        return a.getter == b.getter && a.setter == b.setter;
    case MemberKind::Constant:
        return a.constant == b.constant && a.integer == b.integer;
    case MemberKind::Variable:
        return a.variable == b.variable;
    case MemberKind::Namespace:
        return a.module == b.module;
    case MemberKind::Builtin:
        return a.builtin == b.builtin;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool belongToOneArm(const MemberAnswer& a, const MemberAnswer& b)
{
    return a.needsNoTest() == b.needsNoTest() && haveSameMember(a, b);
}

void mergeIntoArm(MemberArm& arm, LayoutID layout, const MemberAnswer& answer)
{
    MemberAnswer& merged = arm.answer;
    arm.layouts.add(layout);
    merged.receiverProofs |= answer.receiverProofs;
    addConditions(merged.conditions, answer.conditions.span());
    if (merged.holder != answer.holder || merged.field != answer.field) {
        merged.holder = { };
        merged.field = 0;
    }
    if (merged.cause == UnprovenCause::Unknown)
        merged.cause = answer.cause;
    setLevel(merged);
}

void mergeArmsWithSameMember(MemberPartition& partition, ArmsToMerge armsToMerge)
{
    for (size_t index = 0; index < partition.arms.size(); ++index) {
        partition.arms.removeAllMatching([&](const MemberArm& later) {
            MemberArm& arm = partition.arms[index];
            if (arm.answer.needsNoTest() == later.answer.needsNoTest() || !haveSameMember(arm.answer, later.answer))
                return false;
            const MemberArm& proven = arm.answer.needsNoTest() ? arm : later;
            if (armsToMerge == ArmsToMerge::WhereFewerAreProven && 2 * proven.layouts.size() > arm.layouts.size() + later.layouts.size())
                return false;
            for (LayoutID layout : later.layouts)
                mergeIntoArm(arm, layout, later.answer);
            return true;
        }, index + 1);
    }
}

static bool mayAnswerLikeOtherLayouts(const Layout& layout)
{
    switch (layout.kind) {
    case LayoutKind::Record:
    case LayoutKind::Container:
        return true;
    case LayoutKind::Instance:
        return layout.isDerived();
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static MemberPartition partitionByAnswer(std::span<const LayoutID> candidates, NOESCAPE const Invocable<MemberAnswer(LayoutID)> auto& answerFor)
{
    MemberPartition result;
    for (LayoutID layout : candidates) {
        MemberAnswer answer = answerFor(layout);
        if (!answer.isKnown()) {
            result.rest.add(layout);
            continue;
        }
        size_t armIndex = result.arms.findIf([&](const MemberArm& arm) {
            return belongToOneArm(arm.answer, answer);
        });
        if (armIndex != notFound) {
            mergeIntoArm(result.arms[armIndex], layout, answer);
            continue;
        }
        result.arms.append(MemberArm { WTF::move(answer), LayoutList { layout } });
    }
    return result;
}

static MemberPartition computePartition(const PropertyAccessContext& context, std::span<const LayoutID> candidates, MemberQuery query)
{
    const Program& program = context.program;
    if (candidates.size() < PropertyAccessCache::fewestLayouts) {
        return partitionByAnswer(candidates, [&](LayoutID layout) {
            query.layout = layout;
            return findMember(context, query);
        });
    }

    struct LookupPastFields {
        LayoutKind kind;
        LayoutID declaredClass;
        BuiltinClassID builtinBase;
        bool mayGainOwnProperty;
        bool mayChangePrototype;

        bool operator==(const LookupPastFields&) const = default;
    };
    struct AnswerPastFields {
        LookupPastFields lookup;
        MemberAnswer answer;
    };
    constexpr size_t maxAnswersPastFields = 8;

    bool storesValue = query.access == Access::Store || query.access == Access::Define;
    uint32_t likelyPosition = 0;
    std::optional<std::pair<BuiltinClassID, MemberAnswer>> fieldAnswer;
    Vector<AnswerPastFields, maxAnswersPastFields> answersPastFields;
    size_t oldestAnswerPastFields = 0;

    auto findLikeOtherLayouts = [&](LayoutID id) -> MemberAnswer {
        const Layout& layout = program.at(id);
        BuiltinClassID builtinBase = builtinBaseClassOf(program, id);

        std::optional<FieldReference> field;
        if (likelyPosition < layout.fields.size() && layout.fields[likelyPosition].key == query.key)
            field = FieldReference { id, likelyPosition };
        else {
            field = findField(program, id, query.key, BaseChain::Proven);
            if (field && field->layout == id)
                likelyPosition = field->field;
        }

        if (field && fieldAnswer && fieldAnswer->first == builtinBase && !(storesValue && isNeverStored(program, *field))) {
            MemberAnswer answer = fieldAnswer->second;
            answer.holder = field->layout;
            answer.field = field->field;
            return answer;
        }
        if (field) {
            MemberAnswer answer = findMember(context, query);
            if (answer.kind == MemberKind::Field)
                fieldAnswer = std::pair { builtinBase, answer };
            return answer;
        }

        LookupPastFields lookup {
            layout.kind,
            layout.kind == LayoutKind::Instance ? declaredLayoutOf(program, id) : LayoutID(),
            builtinBase,
            ownPropertyGainOf(program, id, query.key) != OwnPropertyGain::Impossible,
            !receiverStateOf(program, id).cannot(ObjectMutation::ChangePrototype),
        };
        for (const AnswerPastFields& known : answersPastFields) {
            if (known.lookup == lookup)
                return known.answer;
        }
        MemberAnswer answer = findMember(context, query);
        if (answersPastFields.size() < maxAnswersPastFields) {
            answersPastFields.append(AnswerPastFields { lookup, answer });
            return answer;
        }
        answersPastFields[oldestAnswerPastFields] = AnswerPastFields { lookup, answer };
        oldestAnswerPastFields = (oldestAnswerPastFields + 1) % maxAnswersPastFields;
        return answer;
    };
    return partitionByAnswer(candidates, [&](LayoutID layout) {
        query.layout = layout;
        if (!layout || !query.key || !mayAnswerLikeOtherLayouts(program.at(layout)))
            return findMember(context, query);
        MemberAnswer answer = findLikeOtherLayouts(layout);
        ASSERT(answer == findMember(context, query));
        return answer;
    });
}

static MemberPartition partition(const PropertyAccessContext& context, std::span<const LayoutID> candidates, const MemberQuery& query)
{
    PropertyAccessCache* cache = context.program.propertyAccessCache();
    if (!cache || candidates.size() < PropertyAccessCache::fewestLayouts || TypeTable::isProvisional(query.key))
        return computePartition(context, candidates, query);

    PropertyAccessCache::PartitionKey key { cache->lists.idOf(candidates), query.key, query.access, query.isOwnLookup, context.flow };
    return findOrCompute(cache->partitions, key, computeHash(key.candidates, key.key, key.access, key.isOwnLookup, key.flow), [&] {
        return PropertyAccessCache::Partition { key, computePartition(context, candidates, query) };
    }).value;
}

MemberPartition partitionMembers(const PropertyAccessContext& context, std::span<const LayoutID> candidates, PropertyKeyID key, Access access)
{
    return partition(context, candidates, { MemberStart::Instance, { }, key, access, false });
}

MemberPartition partitionOwnMembers(const PropertyAccessContext& context, std::span<const LayoutID> candidates, PropertyKeyID key)
{
    return partition(context, candidates, { MemberStart::Instance, { }, key, Access::Has, true });
}

const MemberArm* MemberPartition::findArm(MemberKind kind) const
{
    for (const MemberArm& arm : arms) {
        if (arm.answer.kind == kind)
            return &arm;
    }
    return nullptr;
}

void MemberPartition::dump(PrintStream& out) const
{
    CommaPrinter newline("\n"_s);
    for (const MemberArm& arm : arms) {
        out.print(newline, "arm ");
        dumpLayouts(out, arm.layouts);
        out.print(": ", arm.answer);
    }
    if (rest.isEmpty())
        return;
    out.print(newline, "rest ");
    dumpLayouts(out, rest);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
