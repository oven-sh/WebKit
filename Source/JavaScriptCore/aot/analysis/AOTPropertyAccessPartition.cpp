/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyAccessMembers.h"

#if ENABLE(AOT)

#include "AOTPropertyAccessCache.h"
#include "AOTThreshold.h"
#include <wtf/CommaPrinter.h>
#include <wtf/HashMap.h>

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

// The same for answers that belong to one arm. Merging an answer into an arm leaves it as it is.
static uint64_t hashOfArm(const MemberAnswer& answer)
{
    uint64_t member = 0;
    switch (answer.kind) {
    case MemberKind::Unknown:
    case MemberKind::Absent:
    case MemberKind::Field:
        break;
    case MemberKind::Method:
        member = answer.method.index();
        break;
    case MemberKind::Accessor:
        member = static_cast<uint64_t>(answer.getter.index()) << 32 | answer.setter.index();
        break;
    case MemberKind::Variable:
        member = answer.variable.index();
        break;
    case MemberKind::Namespace:
        member = answer.module.index();
        break;
    case MemberKind::Builtin:
        member = answer.builtin.index();
        break;
    }
    return (member * 0x9e3779b97f4a7c15ull ^ static_cast<uint64_t>(answer.kind) << 1 ^ static_cast<uint64_t>(answer.needsNoTest())) * 0x9e3779b97f4a7c15ull;
}

static MemberPartition partitionByAnswer(std::span<const LayoutID> candidates, NOESCAPE const Invocable<MemberAnswer(LayoutID)> auto& answerFor)
{
    MemberPartition result;
    // A thousand classes with a method each are a thousand arms: each layout would be held against all of them.
    AOT_THRESHOLD(size_t, mostArmsSearchedOneByOne, 16);
    UncheckedKeyHashMap<uint64_t, Vector<uint32_t, 1>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> armsByHash;
    auto noteArm = [&](size_t index) {
        armsByHash.add(hashOfArm(result.arms[index].answer), Vector<uint32_t, 1> { }).iterator->value.append(static_cast<uint32_t>(index));
    };
    auto findArmFor = [&](const MemberAnswer& answer) -> size_t {
        if (result.arms.size() <= mostArmsSearchedOneByOne) {
            return result.arms.findIf([&](const MemberArm& arm) {
                return belongToOneArm(arm.answer, answer);
            });
        }
        if (armsByHash.isEmpty()) {
            for (size_t index = 0; index < result.arms.size(); ++index)
                noteArm(index);
        }
        auto found = armsByHash.find(hashOfArm(answer));
        if (found == armsByHash.end())
            return notFound;
        for (uint32_t index : found->value) {
            if (belongToOneArm(result.arms[index].answer, answer))
                return index;
        }
        return notFound;
    };
    for (LayoutID layout : candidates) {
        MemberAnswer answer = answerFor(layout);
        if (!answer.isKnown()) {
            result.rest.add(layout);
            continue;
        }
        size_t armIndex = findArmFor(answer);
        if (armIndex != notFound) {
            mergeIntoArm(result.arms[armIndex], layout, answer);
            continue;
        }
        result.arms.append(MemberArm { WTF::move(answer), LayoutList { layout } });
        if (!armsByHash.isEmpty())
            noteArm(result.arms.size() - 1);
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

        if (field && fieldAnswer && fieldAnswer->first == builtinBase && !(storesValue && isNeverStored(program, *field)) && !mayBeEmptyBelowAccessor(program, id, *field)) {
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
