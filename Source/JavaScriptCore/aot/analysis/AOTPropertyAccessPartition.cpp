/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyAccessMembers.h"

#if ENABLE(AOT)

#include "AOTPropertyAccessCache.h"
#include "AOTPropertyAccessInternal.h"
#include "AOTVector.h"
#include <optional>
#include <wtf/CommaPrinter.h>

namespace JSC { namespace AOT { namespace Typed {

bool belongToOneArm(const MemberAnswer& a, const MemberAnswer& b)
{
    // An arm needs the tests of all its members, so a member that needs one would cost the proven members their proof.
    if (a.kind != b.kind || a.needsNoTest() != b.needsNoTest())
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

#if ASSERT_ENABLED
static bool isSameAnswer(const MemberAnswer& a, const MemberAnswer& b)
{
    return a.kind == b.kind
        && a.level == b.level
        && a.holder == b.holder
        && a.field == b.field
        && a.method == b.method
        && a.getter == b.getter
        && a.setter == b.setter
        && a.variable == b.variable
        && a.module == b.module
        && a.builtin == b.builtin
        && a.receiverProofs == b.receiverProofs
        && a.conditions == b.conditions
        && a.cause == b.cause;
}
#endif

// The layouts that are derived from one declared layout hold its fields and find every other name on the prototype objects of its class. A record finds every other name on Object.prototype.
static bool mayAnswerLikeOtherLayouts(const Layout& layout)
{
    switch (layout.kind) {
    case LayoutKind::Record:
    case LayoutKind::Container:
        return true;
    case LayoutKind::Instance:
        return layout.isDerived();
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

// Gives what findMember() gives, for one member of a family after the other, and asks it once for all the members that it can't tell apart. findInstanceMember() and what it calls read nothing of such a layout but what is compared here.
class FamilyMemberFinder {
public:
    FamilyMemberFinder(const PropertyAccessContext& context, const MemberQuery& query)
        : m_context(context)
        , m_program(context.program)
        , m_query(query)
        , m_storesValue(query.access == Access::Store || query.access == Access::Define)
    {
    }

    MemberAnswer find(LayoutID);

private:
    static constexpr size_t maxAnswersPastFields = 8;

    struct FieldAnswer {
        BuiltinClassID builtinBase;
        MemberAnswer answer;
    };

    // What the lookup reads of a layout that has no field of the name.
    struct LookupPastFields {
        LayoutKind kind { LayoutKind::Record };
        LayoutID declaredClass;
        BuiltinClassID builtinBase;
        bool mayGainOwnProperty { true };
        bool mayChangePrototype { true };

        friend bool operator==(const LookupPastFields&, const LookupPastFields&) = default;
    };

    struct AnswerPastFields {
        LookupPastFields lookup;
        MemberAnswer answer;
    };

    MemberAnswer findLikeOtherLayouts(LayoutID);
    std::optional<FieldReference> findFieldOf(LayoutID);
    MemberAnswer answerForField(FieldReference, BuiltinClassID builtinBase);
    MemberAnswer answerPastFields(const LookupPastFields&);

    const PropertyAccessContext& m_context;
    const Program& m_program;
    MemberQuery m_query;
    bool m_storesValue;
    uint32_t m_likelyPosition { 0 };
    std::optional<FieldAnswer> m_fieldAnswer;
    Vector<AnswerPastFields, maxAnswersPastFields> m_answersPastFields;
    size_t m_oldestAnswerPastFields { 0 };
};

MemberAnswer FamilyMemberFinder::find(LayoutID layout)
{
    m_query.layout = layout;
    if (!layout || !m_query.key || !mayAnswerLikeOtherLayouts(m_program.at(layout)))
        return findMember(m_context, m_query);

    MemberAnswer answer = findLikeOtherLayouts(layout);
    ASSERT(isSameAnswer(answer, findMember(m_context, m_query)));
    return answer;
}

MemberAnswer FamilyMemberFinder::findLikeOtherLayouts(LayoutID layout)
{
    BuiltinClassID builtinBase = builtinBaseClassOf(m_program, layout);
    if (std::optional<FieldReference> field = findFieldOf(layout))
        return answerForField(*field, builtinBase);

    LayoutKind kind = m_program.at(layout).kind;
    LookupPastFields lookup;
    lookup.kind = kind;
    if (kind == LayoutKind::Instance)
        lookup.declaredClass = declaredLayoutOf(m_program, layout);
    lookup.builtinBase = builtinBase;
    lookup.mayGainOwnProperty = ownPropertyGainOf(m_program, layout, m_query.key) != OwnPropertyGain::Impossible;
    lookup.mayChangePrototype = !receiverStateOf(m_program, layout).cannot(ObjectMutation::ChangePrototype);
    return answerPastFields(lookup);
}

// A layout lists a name once, and the fields that the members of a family inherit from one layout keep their positions.
std::optional<FieldReference> FamilyMemberFinder::findFieldOf(LayoutID id)
{
    const Layout& layout = m_program.at(id);
    if (m_likelyPosition < layout.fields.size() && layout.fields[m_likelyPosition].key == m_query.key)
        return FieldReference { id, m_likelyPosition };

    std::optional<FieldReference> found = findField(m_program, id, m_query.key, BaseChain::Proven);
    if (found && found->layout == id)
        m_likelyPosition = found->field;
    return found;
}

// Only what the constructor of a built-in base class defines comes before a field, and only a store asks more of a field than that it is there.
MemberAnswer FamilyMemberFinder::answerForField(FieldReference field, BuiltinClassID builtinBase)
{
    bool isAnswered = m_fieldAnswer && m_fieldAnswer->builtinBase == builtinBase && !(m_storesValue && isNeverStored(m_program, field));
    if (isAnswered) {
        MemberAnswer answer = m_fieldAnswer->answer;
        answer.holder = field.layout;
        answer.field = field.field;
        return answer;
    }

    MemberAnswer answer = findMember(m_context, m_query);
    if (answer.kind == MemberKind::Field)
        m_fieldAnswer = FieldAnswer { builtinBase, answer };
    return answer;
}

// The members that are derived from one layout follow each other, so a few answers are enough however many classes the family has.
MemberAnswer FamilyMemberFinder::answerPastFields(const LookupPastFields& lookup)
{
    for (const AnswerPastFields& known : m_answersPastFields) {
        if (known.lookup == lookup)
            return known.answer;
    }

    MemberAnswer answer = findMember(m_context, m_query);
    if (m_answersPastFields.size() < maxAnswersPastFields) {
        m_answersPastFields.append(AnswerPastFields { lookup, answer });
        return answer;
    }
    m_answersPastFields[m_oldestAnswerPastFields] = AnswerPastFields { lookup, answer };
    m_oldestAnswerPastFields = (m_oldestAnswerPastFields + 1) % maxAnswersPastFields;
    return answer;
}

} // anonymous namespace

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
    if (candidates.size() < PropertyAccessCache::fewestLayouts) {
        return partitionByAnswer(candidates, [&](LayoutID layout) {
            query.layout = layout;
            return findMember(context, query);
        });
    }

    FamilyMemberFinder finder(context, query);
    return partitionByAnswer(candidates, [&](LayoutID layout) {
        return finder.find(layout);
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
    }).partition;
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

static void dumpLayouts(PrintStream& out, const LayoutList& layouts)
{
    CommaPrinter comma;
    out.print("[");
    for (LayoutID layout : layouts)
        out.print(comma, "L", layout.index());
    out.print("]");
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
