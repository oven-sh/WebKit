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
        return true;
    case MemberKind::Field:
        return a.isAccessorInSlot == b.isAccessorInSlot;
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
    // It answers for reads of its fields alone, whatever a layout of this program with the same fields answers.
    if (layout.has(LayoutFlags::IsOfImageBelow))
        return false;
    switch (layout.kind) {
    case LayoutKind::Record:
    case LayoutKind::Container:
        return true;
    case LayoutKind::Instance:
        return true;
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
        break;
    case MemberKind::Field:
        member = answer.isAccessorInSlot;
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

// `use` is handed what answers for one candidate after the other.
static auto withAnswersFor(const PropertyAccessContext& context, std::span<const LayoutID> candidates, MemberQuery query, NOESCAPE const auto& use)
{
    const Program& program = context.program;
    if (candidates.size() < PropertyAccessCache::fewestLayouts) {
        return use([&](LayoutID layout) {
            query.layout = layout;
            return findMember(context, query);
        });
    }

    struct LookupPastFields {
        LayoutKind kind;
        LayoutID declaredClass;
        BuiltinClassID builtinBase;
        bool comesToObjectPrototypeWithoutNote;
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
    AOT_THRESHOLD(unsigned, mostClassesInRowAskedInVain, 4);
    unsigned classesInRowThatHaveName = 0;

    auto findLikeOtherLayouts = [&](LayoutID id) -> MemberAnswer {
        const Layout& layout = program.at(id);
        // The layout that a class declares answers like that of another class only where neither has the name: a thousand classes asked for a method of one of them.
        bool isDeclaredByClass = layout.kind == LayoutKind::Instance && !layout.isDerived();
        if (isDeclaredByClass) {
            // Where one class after another has the name, most have: to ask each whether it lacks the name is asking twice.
            bool lacksName = classesInRowThatHaveName < mostClassesInRowAskedInVain && !layout.has(LayoutFlags::HasHeritage) && !findField(program, id, query.key, BaseChain::Proven)
                && walkComesToObjectPrototypeWithoutNote(program, declaredLayoutOf(program, id), query.key, query.access);
            if (!lacksName) {
                ++classesInRowThatHaveName;
                return findMember(context, query);
            }
            classesInRowThatHaveName = 0;
        }
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
            answer.isAccessorInSlot = fieldAt(program, *field).form.contains(FieldForm::IsAccessor);
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
            layout.kind == LayoutKind::Instance && !isDeclaredByClass ? declaredLayoutOf(program, id) : LayoutID(),
            builtinBase,
            isDeclaredByClass,
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
    return use([&](LayoutID layout) {
        query.layout = layout;
        if (!layout || !query.key || !mayAnswerLikeOtherLayouts(program.at(layout)))
            return findMember(context, query);
        MemberAnswer answer = findLikeOtherLayouts(layout);
        ASSERT(answer == findMember(context, query));
        RELEASE_ASSERT(!program.propertyAccessCache() || !program.propertyAccessCache()->checksAnswers || answer == findMember(context, query));
        return answer;
    });
}

static MemberPartition computePartition(const PropertyAccessContext& context, std::span<const LayoutID> candidates, const MemberQuery& query)
{
    return withAnswersFor(context, candidates, query, [&](const auto& answerFor) {
        return partitionByAnswer(candidates, answerFor);
    });
}

// The numbers of an answer. Most are those of no record: an answer names what its kind has.
static void forEachNumberOf(auto& answer, NOESCAPE const auto& functor)
{
    functor(answer.holder);
    functor(answer.field);
    functor(answer.method);
    functor(answer.getter);
    functor(answer.setter);
    functor(answer.variable);
    functor(answer.module);
    functor(answer.builtin);
}

static uint32_t wordOf(uint32_t number) { return number; }
static uint32_t wordOf(const auto& id) { return id.index(); }

// How many arms; how many layouts are the rest; those layouts. Then each arm: kind, level, cause and which numbers follow; how many layouts, and whether proofs follow; how many conditions, and whether the slot holds an accessor;
// the proofs; the numbers; the layouts; the conditions.
PackedMemberPartition::PackedMemberPartition(const MemberPartition& partition)
{
    static_assert(sizeof(ProofSet) == sizeof(uint64_t));
    constexpr size_t mostWordsOfArmWithoutLists = 3 + 2 + 8;
    size_t mostWords = 2 + partition.rest.size() + mostWordsOfArmWithoutLists * partition.arms.size();
    for (const MemberArm& arm : partition.arms)
        mostWords += arm.layouts.size() + 3 * arm.answer.conditions.size();
    Vector<uint32_t, 256> words;
    words.reserveInitialCapacity(mostWords);

    words.append(static_cast<uint32_t>(partition.arms.size()));
    words.append(static_cast<uint32_t>(partition.rest.size()));
    words.append(spanReinterpretCast<const uint32_t>(partition.rest.span()));
    for (const MemberArm& arm : partition.arms) {
        const MemberAnswer& answer = arm.answer;
        bool hasProofs = !answer.receiverProofs.isEmpty();
        RELEASE_ASSERT(arm.layouts.size() <= UINT32_MAX >> 1);
        size_t first = words.size();
        words.append(static_cast<uint32_t>(answer.kind) | static_cast<uint32_t>(answer.level) << 8 | static_cast<uint32_t>(answer.cause) << 16);
        words.append(static_cast<uint32_t>(arm.layouts.size()) << 1 | static_cast<uint32_t>(hasProofs));
        RELEASE_ASSERT(answer.conditions.size() <= UINT32_MAX >> 1);
        words.append(static_cast<uint32_t>(answer.conditions.size()) << 1 | static_cast<uint32_t>(answer.isAccessorInSlot));
        if (hasProofs) {
            uint64_t proofs = std::bit_cast<uint64_t>(answer.receiverProofs);
            words.append(static_cast<uint32_t>(proofs));
            words.append(static_cast<uint32_t>(proofs >> 32));
        }
        uint32_t bit = 1u << 24;
        forEachNumberOf(answer, [&](const auto& number) {
            if (number != std::remove_cvref_t<decltype(number)>()) {
                words[first] |= bit;
                words.append(wordOf(number));
            }
            bit <<= 1;
        });
        words.append(spanReinterpretCast<const uint32_t>(arm.layouts.span()));
        for (const MemberCondition& condition : answer.conditions) {
            words.append(static_cast<uint32_t>(condition.kind) | static_cast<uint32_t>(condition.side) << 8);
            words.append(condition.layout.index());
            words.append(condition.key.index());
        }
    }
    m_words = Vector<uint32_t>(words.span());
}

MemberPartition PackedMemberPartition::unpack() const
{
    std::span<const uint32_t> words = m_words.span();
    size_t next = 0;
    auto takeLayouts = [&](LayoutList& layouts, uint32_t count) {
        for (uint32_t layout : words.subspan(next, count))
            layouts.add(LayoutID(layout));
        next += count;
    };
    MemberPartition partition;
    uint32_t numberOfArms = words[next++];
    uint32_t sizeOfRest = words[next++];
    takeLayouts(partition.rest, sizeOfRest);
    partition.arms.grow(numberOfArms);
    for (MemberArm& arm : partition.arms) {
        MemberAnswer& answer = arm.answer;
        uint32_t first = words[next++];
        uint32_t second = words[next++];
        uint32_t third = words[next++];
        uint32_t numberOfConditions = third >> 1;
        answer.isAccessorInSlot = third & 1;
        answer.kind = static_cast<MemberKind>(first & 0xff);
        answer.level = static_cast<KnowledgeLevel>(first >> 8 & 0xff);
        answer.cause = static_cast<UnprovenCause>(first >> 16 & 0xff);
        if (second & 1) {
            answer.receiverProofs = std::bit_cast<ProofSet>(static_cast<uint64_t>(words[next]) | static_cast<uint64_t>(words[next + 1]) << 32);
            next += 2;
        }
        uint32_t bit = 1u << 24;
        forEachNumberOf(answer, [&](auto& number) {
            if (first & bit)
                number = std::remove_cvref_t<decltype(number)>(words[next++]);
            bit <<= 1;
        });
        takeLayouts(arm.layouts, second >> 1);
        for (; numberOfConditions; --numberOfConditions) {
            answer.conditions.append(MemberCondition { static_cast<MemberCondition::Kind>(words[next] & 0xff), LayoutID(words[next + 1]), static_cast<ClassSide>(words[next] >> 8 & 0xff), PropertyKeyID(words[next + 2]) });
            next += 3;
        }
    }
    RELEASE_ASSERT(next == words.size());
    return partition;
}

static MemberPartition partition(const PropertyAccessContext& context, std::span<const LayoutID> candidates, const MemberQuery& query)
{
    PropertyAccessCache* cache = context.program.propertyAccessCache();
    if (!cache || candidates.size() < PropertyAccessCache::fewestLayouts || TypeTable::isProvisional(query.key))
        return computePartition(context, candidates, query);

    PropertyAccessCache::PartitionKey key { cache->lists.idOf(candidates), query.key, query.access, query.isOwnLookup, context.flow };
    uint32_t hash = computeHash(key.candidates, key.key, key.access, key.isOwnLookup, key.flow);
    auto isWanted = [&](const PropertyAccessCache::Partition& entry) {
        return entry.key == key;
    };
    if (const PropertyAccessCache::Partition* known = cache->partitions.find(hash, isWanted))
        return known->value.unpack();
    MemberPartition computed = computePartition(context, candidates, query);
    const PropertyAccessCache::Partition& kept = cache->partitions.add(hash, { key, PackedMemberPartition(computed) }, isWanted);
    if (cache->checksAnswers) [[unlikely]] {
        MemberPartition unpacked = kept.value.unpack();
        RELEASE_ASSERT(unpacked.rest == computed.rest && std::ranges::equal(unpacked.arms, computed.arms, [](const MemberArm& a, const MemberArm& b) {
            return a.answer == b.answer && a.layouts == b.layouts;
        }));
    }
    return computed;
}

MemberPartition partitionMembers(const PropertyAccessContext& context, std::span<const LayoutID> candidates, PropertyKeyID key, Access access)
{
    return partition(context, candidates, { MemberStart::Instance, { }, key, access, false });
}

MemberPartition partitionOwnMembers(const PropertyAccessContext& context, std::span<const LayoutID> candidates, PropertyKeyID key)
{
    return partition(context, candidates, { MemberStart::Instance, { }, key, Access::Has, true });
}

bool readsMethodOfEachWithoutTests(const PropertyAccessContext& context, std::span<const LayoutID> candidates, PropertyKeyID key)
{
    auto isMethodWithoutTests = [](const MemberAnswer& answer) {
        return answer.kind == MemberKind::Method && answer.receiverProofs.isEmpty() && answer.conditions.isEmpty();
    };
    MemberQuery query { MemberStart::Instance, { }, key, Access::Read, false };
    bool result = withAnswersFor(context, candidates, query, [&](const auto& answerFor) {
        return std::ranges::all_of(candidates, [&](LayoutID layout) {
            return isMethodWithoutTests(answerFor(layout));
        });
    });
    if (PropertyAccessCache* cache = context.program.propertyAccessCache(); cache && cache->checksAnswers) [[unlikely]] {
        MemberPartition partition = computePartition(context, candidates, query);
        RELEASE_ASSERT(result == (partition.rest.isEmpty() && std::ranges::all_of(partition.arms, isMethodWithoutTests, &MemberArm::answer)));
    }
    return result;
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
