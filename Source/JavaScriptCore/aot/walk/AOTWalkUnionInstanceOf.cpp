/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinPristine.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTBuiltinTypes.h"
#include "AOTFactRecords.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTLinkTables.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include "AOTWalkKnown.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using ConditionsOfInstanceOf = Vector<MemberConditionRecord, 4>;

enum class ChainEndForInstanceOf : uint8_t { Unknown, MeetsClass, MissesClass };

struct ChainsForInstanceOf {
    Vector<LayoutID, 8> decided;
    Vector<LayoutID, 8> instances;
    bool testsObjects { false };
    bool hasUndecidedLayouts { false };
};

struct ClassTestedByInstanceOf {
    LayoutID layout;
    bool isConstructorFunction { false };
    ConditionsOfInstanceOf conditions;
};

struct FormOfInstanceOf {
    LayoutID tested;
    bool testedIsConstructorFunction { false };
    ConditionsOfInstanceOf conditions;
    LayoutSetID instances;
    LayoutSetID hierarchy;
    std::span<const MemberConditionRecord> hierarchyGuards;
    bool hierarchyDecidesEveryShape { false };
    bool missMeansFalse { false };
    bool areAllInstances { false };
    bool valueIsSurelyObject { false };
    bool testsPrototypeOfInstance { false };
};


using NumbersOfClassesForInstanceOf = UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

struct ClassHierarchyNumbering {
    Vector<LayoutID> candidates;
    Vector<LayoutID> layouts;
    Vector<uint32_t> classPreorderNumbers;
    Vector<uint32_t> classSubtreeEnds;
    Vector<MemberConditionRecord> links;
    UncheckedKeyHashMap<uint32_t, std::pair<uint32_t, uint32_t>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> subtreeRanges;
};

struct KeptByInstanceOf final : Walker::KeptForBuild {
    UncheckedKeyHashMap<uint32_t, std::unique_ptr<ClassHierarchyNumbering>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> numberingsByFamily;
    UncheckedKeyHashMap<uint32_t, Vector<std::unique_ptr<ClassHierarchyNumbering>, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> numberingsByLayoutList;
};

struct BuiltinTestedByInstanceOf {
    BuiltinID constructor;
    BuiltinID prototype;
    BuiltinClassID builtinClass;
};

class AnswersOfInstanceOf {
public:
    explicit AnswersOfInstanceOf(Walker& walker)
        : m_walker(walker)
    {
    }

    void letArmsEndWithBranchBehind(const BodyValue& test) { m_testWhoseArmsEndWithBranch = test; }
    bool armsEndWithBranch() const { return !!m_testWhoseArmsEndWithBranch; }

    void arrive(Reference answer)
    {
        if (!answer || m_walker.isUnreachable())
            return;
        if (m_testWhoseArmsEndWithBranch) {
            m_walker.bind(m_testWhoseArmsEndWithBranch->id(), answer);
            if (!m_walker.threadBranchAfter(*m_testWhoseArmsEndWithBranch))
                m_walker.fail("an arm of instanceof does not walk the end of its block"_s);
            return;
        }
        if (!m_meeting) {
            m_meeting = m_walker.addBlock();
            m_answer = m_walker.addParameter(m_meeting, TypeTable::boolean, m_walker.formOfParameter(TypeTable::boolean, { }));
        }
        m_walker.jumpTo(m_meeting, singleElementSpan(answer));
    }

    Reference meet()
    {
        if (!m_meeting)
            return { };
        m_walker.continueIn(m_meeting);
        return m_walker.isUnreachable() ? Reference() : m_answer;
    }

private:
    Walker& m_walker;
    std::optional<BodyValue> m_testWhoseArmsEndWithBranch;
    BlockID m_meeting;
    Reference m_answer;
};

} // anonymous namespace

static void addConditionOfInstanceOf(ConditionsOfInstanceOf& conditions, const MemberConditionRecord& condition)
{
    if (!conditions.contains(condition))
        conditions.append(condition);
}

static OptionSet<LayoutChange> possibleChangesForInstanceOf(const LinkTables& link, LayoutID layout)
{
    return layout.index() < link.layouts.size() ? link.layout(layout).possibleChanges : LinkedLayout { }.possibleChanges;
}

static bool classStaysAsWrittenForInstanceOf(const LinkTables& link, LayoutID classLayout)
{
    return !possibleChangesForbid(possibleChangesForInstanceOf(link, classLayout), TypedAccess::FindsMethodAbove);
}

static bool maySpendBytesForTimeInInstanceOf(const Walker& walker)
{
    return walker.isInLoop() || walker.isCurrentFrameRecursive();
}

static bool mayBuildArmsForInstanceOf(const Walker& walker)
{
    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses())
        return false;
    if (walker.isInLoop())
        return true;
    FunctionID function = walker.currentFunction();
    return !walker.program().at(function).isModuleBody();
}

static Reference booleanAnswerOfInstanceOf(Walker& walker, bool answer)
{
    Draft constant(Opcode::Boolean);
    constant.setBoolean(answer);
    return walker.constant(WTF::move(constant));
}

static Reference buildInstanceOfAsWritten(Walker& walker, const BodyValue& test)
{
    Draft written = walker.draftOf(test);
    walker.chooseConservativeFields(written);
    return walker.build(WTF::move(written));
}

static Reference buildNarrowedValue(Walker& walker, Reference value, Reference test, uint32_t operandIndex, Origin origin)
{
    Draft proof(Opcode::Narrowed, origin);
    walker.chooseConservativeFields(proof);
    proof.setValueOperand(value);
    proof.setTestOperand(test);
    proof.setOperandIndex(operandIndex);
    proof.setIsOnTakenEdge(true);
    return walker.build(WTF::move(proof));
}

static Reference buildTypeTestOfInstanceOf(Walker& walker, Reference value, TypeID tested, Origin origin)
{
    Draft test(Opcode::TypeTest, origin);
    walker.chooseConservativeFields(test);
    test.setValueOperand(value);
    test.setTestedType(tested);
    return walker.build(WTF::move(test));
}

static Reference buildWalkOfChainForInstanceOf(Walker& walker, Reference value, Reference prototype, Origin origin)
{
    if (!prototype)
        return { };
    Draft chainWalk(Opcode::HasInPrototypeChain, origin);
    walker.chooseConservativeFields(chainWalk);
    chainWalk.setValueOperand(value);
    chainWalk.setPrototypeOperand(prototype);
    return walker.build(WTF::move(chainWalk));
}

static Reference buildWalkOfChainThatGivesWayForInstanceOf(Walker& walker, Reference value, Reference prototype, Origin origin)
{
    if (!prototype)
        return { };
    Draft chainWalk(Opcode::TryFindInPrototypeChain, origin);
    walker.chooseConservativeFields(chainWalk);
    chainWalk.setValueOperand(value);
    chainWalk.setPrototypeOperand(prototype);
    Reference found = walker.build(WTF::move(chainWalk));
    if (!found)
        return { };
    walker.count(StatisticID::InstanceOfMissesWalkedInline);
    auto isAnswer = [&](int32_t answer) {
        Draft asked(Opcode::Integer);
        asked.setInteger(answer);
        Draft comparison(Opcode::StrictEq, origin);
        walker.chooseConservativeFields(comparison);
        comparison.setLeftOperand(found);
        comparison.setRightOperand(walker.constant(WTF::move(asked)));
        comparison.setForm(OperatorForm::Integers);
        return walker.build(WTF::move(comparison));
    };
    Reference isTrapped = isAnswer(answerOfChainWalkAtTrap);
    if (!isTrapped)
        return { };
    BlockID givesWay = walker.addBlock(BlockFrequency::Rare);
    BlockID isAnswered = walker.addBlock();
    walker.branch(isTrapped, givesWay, isAnswered);
    walker.continueIn(givesWay);
    if (!walker.isUnreachable())
        walker.restartIteration();
    walker.continueIn(isAnswered);
    return isAnswer(1);
}

static bool hasOneClassObjectForInstanceOf(const Walker& walker, LayoutID classLayout)
{
    if (!walker.facts().isEvaluatedAtMostOnce(classLayout))
        return false;
    const Layout& layout = walker.program().at(classLayout);
    if (!layout.has(LayoutFlags::IsConstructorFunction))
        return true;
    return layout.constructor && walker.program().at(layout.constructor).captures.size() == walker.plan().promotedEnvironments().numberOfRemovedCapturesOf(layout.constructor);
}

static bool recordsExpectNothingUnderNameOnStaticSidesForInstanceOf(const Walker& walker, LayoutID tested, PropertyKeyID key)
{
    const Program& program = walker.program();
    size_t numberOfClassesLeft = program.layouts.size();
    for (LayoutID current = tested; current;) {
        const Layout& layout = program.at(current);
        if (!numberOfClassesLeft-- || !layout.constructor || !walker.plan().hasClassRecord(current) || layout.findStaticField(key) || walker.facts().linked().layout(current).namesAreDefinedPastLists[static_cast<size_t>(ClassSide::Static)])
            return false;
        for (const ClassMember& declared : walker.facts().membersOf(current)) {
            if (declared.side == ClassSide::Static && (!declared.key || declared.key == key))
                return false;
        }
        LayoutID parent;
        if (!findParentNamedByClassRecordForTypedAccess(walker, current, parent))
            return false;
        current = parent;
    }
    return true;
}

static bool findsOrdinaryHasInstanceForInstanceOf(const Walker& walker, const ClassTestedByInstanceOf& tested, ConditionsOfInstanceOf& conditions)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    const LinkTables& link = walker.facts().linked();
    PropertyKeyID key = program.keyOf(table.nameOf(WellKnownSymbol::HasInstance));
    BuiltinID ordinary = findConstantMemberFrom(program, walker.facts(), Builtins::FunctionPrototype, key);
    if (!ordinary || table.canonicalID(ordinary) != Builtins::FunctionPrototypeSymbolHasInstance)
        return false;

    if ((tested.isConstructorFunction || classStaysAsWrittenForInstanceOf(link, tested.layout)) && link.findsNothing(program, tested.layout, ClassSide::Static, key, Of::HolderAndAbove))
        return true;

    if (!recordsExpectNothingUnderNameOnStaticSidesForInstanceOf(walker, tested.layout, key))
        return false;
    addConditionOfInstanceOf(conditions, { MemberConditionKind::MemberUnchanged, tested.layout, ClassSide::Static, key });
    return true;
}

static MemberConditionRecord conditionOnPrototypePropertyForInstanceOf(const Program& program, LayoutID constructorFunction)
{
    return { MemberConditionKind::MemberUnchanged, constructorFunction, ClassSide::Static, program.keyOf(BuiltinNames::prototype) };
}

static std::optional<ClassTestedByInstanceOf> describeClassTestedByInstanceOf(const Walker& walker, TypeID typeOfConstructor)
{
    const Program& program = walker.program();
    TypeArena& types = walker.types();
    Type type = types.at(typeOfConstructor);
    FunctionID constructor = type.isObject() && type.isJavaScriptValue() ? singleFunctionOf(types, type) : FunctionID();
    if (!constructor)
        return std::nullopt;

    ClassTestedByInstanceOf tested;
    tested.layout = classOfClassObject(program, constructor);
    if (!tested.layout) {
        tested.layout = classOfConstructorFunction(program, constructor);
        tested.isConstructorFunction = true;
    }
    if (!tested.layout || program.at(tested.layout).kind != LayoutKind::Instance)
        return std::nullopt;

    bool isEvaluatedOnce = hasOneClassObjectForInstanceOf(walker, tested.layout);
    if (tested.isConstructorFunction && !walker.facts().isEvaluatedAtMostOnce(tested.layout))
        return std::nullopt;
    if (!findsOrdinaryHasInstanceForInstanceOf(walker, tested, tested.conditions))
        return std::nullopt;

    if (tested.isConstructorFunction && !walker.facts().linked().layouts[tested.layout.index()].prototypeObjectIsSettledAheadOfEveryObject)
        addConditionOfInstanceOf(tested.conditions, conditionOnPrototypePropertyForInstanceOf(program, tested.layout));
    if (!walker.facts().isEvaluatedAtMostOnce(tested.layout))
        addConditionOfInstanceOf(tested.conditions, { MemberConditionKind::ClassEvaluatedOnce, tested.layout, ClassSide::Prototype, { } });
    if (!tested.conditions.isEmpty() && (!walker.plan().hasClassRecord(tested.layout) || (tested.isConstructorFunction && !isEvaluatedOnce)))
        return std::nullopt;
    return tested;
}

bool findConditionsOfOrdinaryInstanceOf(const Walker& walker, TypeID typeOfConstructor, Vector<MemberConditionRecord, 16>& conditions)
{
    std::optional<ClassTestedByInstanceOf> tested = describeClassTestedByInstanceOf(walker, typeOfConstructor);
    if (!tested)
        return false;
    for (const MemberConditionRecord& condition : tested->conditions)
        conditions.append(condition);
    return true;
}

bool canDecideInstanceOfByHierarchy(const Walker& walker, TypeID typeOfConstructor)
{
    std::optional<ClassTestedByInstanceOf> tested = describeClassTestedByInstanceOf(walker, typeOfConstructor);
    return tested && !tested->isConstructorFunction && walker.plan().hasClassRecord(tested->layout) && walker.facts().families().nearestFamilyOf(tested->layout);
}

void noteWhatInstanceOfDenied(Walker& walker, const BodyValue& test)
{
    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses() || walker.isUnreachable())
        return;
    Reference value = walker.referenceOf(test.valueOperand());
    Reference constructor = walker.referenceOf(test.constructorOperand());
    if (!value || !value.isValue() || !constructor)
        return;
    std::optional<ClassTestedByInstanceOf> tested = describeClassTestedByInstanceOf(walker, walker.typeOf(constructor));
    if (!tested)
        return;
    TypeArena& types = walker.types();
    Vector<LayoutID, 8> layouts { tested->layout };
    layouts.append(walker.program().layoutsExpectedBelow(tested->layout));
    TypeID denied = types.forLayouts(layouts.span());
    if (std::optional<uint64_t> before = findKnown(walker, KnownNote::ClassesThatInstanceOfDenied, value))
        denied = types.unionOf(denied, types.forLayoutSet(LayoutSetID::fromBits(static_cast<uint32_t>(*before))));
    noteKnown(walker, KnownNote::ClassesThatInstanceOfDenied, value, { }, 0, LocationMasks(), types.at(denied).layouts.bits());
}

static ChainEndForInstanceOf followChainForInstanceOf(const Walker& walker, LayoutID layout, LayoutID tested)
{
    const Program& program = walker.program();
    const LinkTables& link = walker.facts().linked();
    const Layout& start = program.at(layout);
    if (start.has(LayoutFlags::IsOfCreatedObject))
        return ChainEndForInstanceOf::Unknown;
    if (start.kind == LayoutKind::Record)
        return !start.constructor && !start.has(LayoutFlags::IsConstructorFunction) ? ChainEndForInstanceOf::MissesClass : ChainEndForInstanceOf::Unknown;
    if (start.kind != LayoutKind::Instance)
        return ChainEndForInstanceOf::Unknown;

    LayoutID current = declaredLayoutOf(program, layout);
    for (size_t unvisitedClasses = program.layouts.size(); current && unvisitedClasses; --unvisitedClasses) {
        if (current == tested)
            return ChainEndForInstanceOf::MeetsClass;
        const Layout& passed = program.at(current);
        if (passed.kind != LayoutKind::Instance || !passed.constructor)
            return ChainEndForInstanceOf::Unknown;
        if (passed.has(LayoutFlags::IsConstructorFunction)) {
            bool standsBelowObjectPrototype = !passed.base && link.layouts[current.index()].prototypeObjectIsSettledAheadOfEveryObject;
            return standsBelowObjectPrototype ? ChainEndForInstanceOf::MissesClass : ChainEndForInstanceOf::Unknown;
        }

        LayoutID next = classProvenAbove(program, link, current);
        if (!next)
            return classStaysAsWrittenForInstanceOf(link, current) && !passed.has(LayoutFlags::HasHeritage) ? ChainEndForInstanceOf::MissesClass : ChainEndForInstanceOf::Unknown;
        if (program.at(next).has(LayoutFlags::IsConstructorFunction))
            return ChainEndForInstanceOf::Unknown;
        current = next;
    }
    return ChainEndForInstanceOf::Unknown;
}

static ChainsForInstanceOf followChainsForInstanceOf(const Walker& walker, std::span<const LayoutID> ascendingLayouts, LayoutID tested)
{
    const Program& program = walker.program();
    const LinkTables& link = walker.facts().linked();
    bool prototypeObjectMayBeShared = program.at(tested).has(LayoutFlags::IsConstructorFunction) && !link.layouts[tested.index()].prototypeObjectIsSettledAheadOfEveryObject;
    ChainsForInstanceOf chains;
    for (LayoutID layout : ascendingLayouts) {
        if (!walker.plan().isInUse(layout))
            continue;

        ChainEndForInstanceOf end = followChainForInstanceOf(walker, layout, tested);
        bool isMadeByConstructorFunction = program.at(layout).kind == LayoutKind::Instance && program.at(declaredLayoutOf(program, layout)).has(LayoutFlags::IsConstructorFunction)
            && !link.layouts[declaredLayoutOf(program, layout).index()].prototypeObjectIsSettledAheadOfEveryObject;
        bool mayGetOtherPrototype = possibleChangesForInstanceOf(link, layout).contains(LayoutChange::ChangesPrototype);
        bool testsObject = false;
        if (end == ChainEndForInstanceOf::MissesClass && (mayGetOtherPrototype || prototypeObjectMayBeShared))
            end = ChainEndForInstanceOf::Unknown;
        if (end == ChainEndForInstanceOf::MeetsClass && (isMadeByConstructorFunction || mayGetOtherPrototype)) {
            if (isFoundByShapeTest(program.at(layout)))
                testsObject = true;
            else
                end = ChainEndForInstanceOf::Unknown;
        }
        if (end == ChainEndForInstanceOf::Unknown) {
            chains.hasUndecidedLayouts = true;
            continue;
        }

        chains.decided.append(layout);
        if (end == ChainEndForInstanceOf::MeetsClass)
            chains.instances.append(layout);
        chains.testsObjects |= testsObject;
    }
    return chains;
}

static bool isTestableByShapeForInstanceOf(const Walker&, std::span<const LayoutID>);

static KeptByInstanceOf& keptByInstanceOf(const Walker& walker)
{
    std::unique_ptr<Walker::KeptForBuild>& kept = walker.keptByInstanceOf();
    if (!kept)
        kept = makeUniqueWithoutFastMallocCheck<KeptByInstanceOf>();
    return static_cast<KeptByInstanceOf&>(*kept);
}

static void numberClassHierarchy(const Walker& walker, LayoutSetID candidates, ClassHierarchyNumbering& hierarchy)
{
    const Program& program = walker.program();
    constexpr uint32_t unknown = std::numeric_limits<uint32_t>::max();
    NumbersOfClassesForInstanceOf topmostOfClass;
    NumbersOfClassesForInstanceOf parentOfClass;
    Vector<uint32_t, 16> passed;
    auto findTopmostOf = [&](LayoutID classLayout) {
        passed.shrink(0);
        uint32_t topmost = unknown;
        LayoutID current = classLayout;
        for (size_t unvisitedClasses = program.layouts.size(); unvisitedClasses; --unvisitedClasses) {
            if (auto found = topmostOfClass.find(current.index()); found != topmostOfClass.end()) {
                topmost = found->value;
                break;
            }
            passed.append(current.index());
            const Layout& layout = program.at(current);
            if (layout.kind != LayoutKind::Instance || !layout.constructor || !walker.plan().hasClassRecord(current) || !hasOneClassObjectForInstanceOf(walker, current))
                break;
            LayoutID parent;
            if (!findParentNamedByClassRecordForTypedAccess(walker, current, parent))
                break;
            if (!parent) {
                topmost = current.index();
                break;
            }
            parentOfClass.set(current.index(), parent.index());
            current = parent;
        }
        for (uint32_t number : passed)
            topmostOfClass.set(number, topmost);
        return topmost;
    };

    Vector<uint32_t> topmostClasses;
    for (LayoutID member : membersOf(walker.types(), candidates)) {
        if (program.at(member).kind != LayoutKind::Instance || program.at(member).has(LayoutFlags::IsOfCreatedObject) || !walker.plan().isInUse(member) || !isTestableByShapeForInstanceOf(walker, singleElementSpan(member)))
            continue;
        LayoutID classLayout = declaredLayoutOf(program, member);
        uint32_t topmost = findTopmostOf(classLayout);
        if (topmost == unknown)
            continue;
        topmostClasses.append(topmost);
        hierarchy.layouts.append(member);
        hierarchy.classPreorderNumbers.append(classLayout.index());
    }
    std::ranges::sort(topmostClasses);
    topmostClasses.shrink(std::ranges::unique(topmostClasses).begin() - topmostClasses.begin());

    Vector<std::pair<uint32_t, uint32_t>> classesBelowParents;
    for (auto& [number, topmost] : topmostOfClass) {
        if (topmost != unknown && number != topmost)
            classesBelowParents.append({ parentOfClass.get(number), number });
    }
    std::ranges::sort(classesBelowParents);
    auto classesBelow = [&](uint32_t parent) {
        auto [first, last] = std::ranges::equal_range(classesBelowParents, parent, { }, &std::pair<uint32_t, uint32_t>::first);
        return std::span(first, last);
    };
    struct ClassEntered {
        uint32_t number;
        uint32_t place;
        size_t numberOfClassesBelowEntered;
    };
    Vector<ClassEntered, 16> entered;
    uint32_t nextPlace = 0;
    for (uint32_t topmost : topmostClasses) {
        entered.append({ topmost, nextPlace++, 0 });
        while (!entered.isEmpty()) {
            ClassEntered& top = entered.last();
            auto below = classesBelow(top.number);
            if (top.numberOfClassesBelowEntered < below.size()) {
                uint32_t next = below[top.numberOfClassesBelowEntered++].second;
                entered.append({ next, nextPlace++, 0 });
                continue;
            }
            hierarchy.subtreeRanges.set(top.number, std::pair { top.place, nextPlace });
            hierarchy.links.append({ MemberConditionKind::PrototypeLinkUnchanged, LayoutID(top.number), ClassSide::Prototype, { } });
            entered.removeLast();
        }
    }
    std::ranges::sort(hierarchy.links, { }, [](const MemberConditionRecord& link) {
        return link.layout.index();
    });
    for (uint32_t& place : hierarchy.classPreorderNumbers) {
        auto [first, end] = hierarchy.subtreeRanges.get(place);
        place = first;
        hierarchy.classSubtreeEnds.append(end);
    }
}

static const ClassHierarchyNumbering* findClassHierarchyNumbering(const Walker& walker, LayoutID tested, TypeID expectedOfValue)
{
    const Families& families = walker.facts().families();
    TypeArena& types = walker.types();
    LayoutSetID candidates;
    FamilyID nearest = families.nearestFamilyOf(tested);
    if (nearest)
        candidates = families.layoutsOf(families.topmostFamilyAtOrAbove(nearest));
    LayoutSetID expected = expectedOfValue ? types.at(expectedOfValue).layouts : LayoutSetID();
    bool familyHoldsAllExpected = !expected || (candidates && std::ranges::all_of(membersOf(types, expected), [&](LayoutID layout) {
        return std::ranges::binary_search(membersOf(types, candidates), layout);
    }));
    KeptByInstanceOf& kept = keptByInstanceOf(walker);
    auto holdsTested = [&](const ClassHierarchyNumbering& hierarchy) {
        return hierarchy.subtreeRanges.contains(tested.index()) ? &hierarchy : nullptr;
    };
    if (!familyHoldsAllExpected) {
        Vector<LayoutID, 16> all { tested };
        if (candidates)
            all.append(membersOf(types, candidates));
        for (LayoutID layout : membersOf(types, expected)) {
            all.append(layout);
            if (FamilyID around = families.nearestFamilyOf(layout))
                all.append(membersOf(types, families.layoutsOf(families.topmostFamilyAtOrAbove(around))));
        }
        std::ranges::sort(all);
        all.shrink(std::ranges::unique(all).begin() - all.begin());
        uint32_t hash = 0;
        for (LayoutID layout : all)
            hash = pairIntHash(hash, layout.index());
        auto& keptUnderHash = kept.numberingsByLayoutList.ensure(hash, [] {
            return Vector<std::unique_ptr<ClassHierarchyNumbering>, 1> { };
        }).iterator->value;
        for (const auto& hierarchy : keptUnderHash) {
            if (std::ranges::equal(hierarchy->candidates, all))
                return holdsTested(*hierarchy);
        }
        auto found = makeUniqueWithoutFastMallocCheck<ClassHierarchyNumbering>();
        found->candidates.append(all.span());
        numberClassHierarchy(walker, types.layoutSets().ofSorted(all.span()), *found);
        keptUnderHash.append(WTF::move(found));
        return holdsTested(*keptUnderHash.last());
    }
    if (!candidates)
        return nullptr;
    return holdsTested(*kept.numberingsByFamily.ensure(families.topmostFamilyAtOrAbove(nearest).index(), [&] {
        auto found = makeUniqueWithoutFastMallocCheck<ClassHierarchyNumbering>();
        numberClassHierarchy(walker, candidates, *found);
        return found;
    }).iterator->value);
}

bool isRecursiveThroughPrototype(const Walker& walker, const BodyIndex& index, FunctionID id)
{
    const Function& function = walker.program().at(id);
    if (function.kind != FunctionKind::Plain || !index.opcodes().contains(Opcode::Call) || !index.opcodes().contains(Opcode::PropGet))
        return false;
    auto laidUnder = std::ranges::equal_range(walker.facts().linked().functionsWrittenRightOfNamesOnPrototypes, id, { }, &std::pair<FunctionID, PropertyKeyID>::first);
    if (laidUnder.empty())
        return false;
    bool callsItself = false;
    index.body().forEachBlock([&](BodyBlock block) {
        for (BodyValue value : block.values()) {
            if (callsItself || value.opcode() != Opcode::Call)
                continue;
            ValueID callee = index.stripCasts(value.as<Opcode::Call>().calleeOperand());
            if (!callee || index.isParameter(callee) || index.opcodeOf(callee) != Opcode::PropGet)
                continue;
            BodyValue read = index.value(callee);
            ValueID receiver = index.stripCasts(read.as<Opcode::PropGet>().objectOperand());
            if (receiver && !index.isParameter(receiver) && index.opcodeOf(receiver) == Opcode::ToThis)
                receiver = index.stripCasts(index.value(receiver).operand(0));
            if (read.isFreeName() || !receiver || !index.isParameter(receiver) || index.blockOf(receiver) != entryBlock || receiver.index() >= function.numberOfEntryParameters() || function.entryParameterKind(receiver.index()) != EntryParameter::This)
                continue;
            callsItself = std::ranges::any_of(laidUnder, [&](const auto& name) {
                return name.second == read.key();
            });
        }
    });
    return callsItself;
}

static bool programMakesNoProxyForInstanceOf(const Walker& walker)
{
    const Facts& facts = walker.facts();
    if (facts.unseenCodeCanRun())
        return false;
    auto isInReach = [&](BuiltinID builtin) {
        return !!findFactRow(facts.builtinUses(), builtin.index(), &BuiltinUseRecord::builtin);
    };
    return !isInReach(Builtins::Proxy) && !isInReach(Builtins::ProxyRevocable);
}

static bool isTestableByShapeForInstanceOf(const Walker& walker, std::span<const LayoutID> layouts)
{
    return std::ranges::all_of(layouts, [&](LayoutID layout) {
        return isFoundByShapeTest(walker.program().at(layout)) && walker.plan().slots().holdsCompiledObjects(layout) && !walker.plan().shapes().shapesOf(layout).isEmpty();
    });
}

static std::optional<FormOfInstanceOf> planFormOfInstanceOf(const Walker& walker, TypeID typeOfValue, TypeID typeOfConstructor, bool valueHasLayoutPrototype, TypeID expectedOfValue)
{
    if (!walker.plan().hasOneInstance())
        return std::nullopt;
    std::optional<ClassTestedByInstanceOf> tested = describeClassTestedByInstanceOf(walker, typeOfConstructor);
    if (!tested)
        return std::nullopt;
    const Program& program = walker.program();
    TypeArena& types = walker.types();
    Type value = types.at(typeOfValue);
    if (!value.isJavaScriptValue() || value.mayBeUnspecified())
        return std::nullopt;

    Vector<LayoutID, 8> considered;
    bool areAllConsidered = !value.hasObjects();
    if (value.hasObjects()) {
        Type objects = types.at(types.withOnlyObjects(typeOfValue));
        areAllConsidered = objects.hasOnlyLayouts() && !objects.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow);
        if (areAllConsidered)
            considered.append(membersOf(types, objects.layouts));
        else {
            considered.append(tested->layout);
            considered.append(program.layoutsExpectedBelow(tested->layout));
            std::ranges::sort(considered);
        }
    }

    ChainsForInstanceOf chains = followChainsForInstanceOf(walker, considered.span(), tested->layout);
    Vector<LayoutID, 8> instancesOutsideHierarchy = chains.instances;
    const ClassHierarchyNumbering* hierarchy = nullptr;
    bool canUseHierarchy = maySpendBytesForTimeInInstanceOf(walker) && mayBuildArmsForInstanceOf(walker) && (!tested->isConstructorFunction || (walker.plan().hasClassRecord(tested->layout) && hasOneClassObjectForInstanceOf(walker, tested->layout)));
    if ((!areAllConsidered || chains.hasUndecidedLayouts) && canUseHierarchy && (!chains.instances.isEmpty() || areAllConsidered))
        hierarchy = findClassHierarchyNumbering(walker, tested->layout, expectedOfValue);
    bool hierarchyHasSiblings = false;
    bool hierarchyDecidesEveryShape = false;
    if (areAllConsidered && chains.hasUndecidedLayouts && valueHasLayoutPrototype && value.isObject() && canUseHierarchy) {
        const ClassHierarchyNumbering* found = findClassHierarchyNumbering(walker, tested->layout, typeOfValue);
        hierarchyDecidesEveryShape = found && std::ranges::all_of(considered, [&](LayoutID layout) {
            return std::ranges::binary_search(found->layouts, layout);
        });
        if (hierarchyDecidesEveryShape) {
            uint32_t placeOfTested = found->subtreeRanges.get(tested->layout.index()).first;
            hierarchyDecidesEveryShape = std::ranges::none_of(considered, [&](LayoutID layout) {
                size_t index = std::ranges::lower_bound(found->layouts, layout) - found->layouts.begin();
                return !walker.shouldSkipSuperclassCheck() && found->classPreorderNumbers[index] < placeOfTested && placeOfTested < found->classSubtreeEnds[index];
            });
        }
        if (hierarchyDecidesEveryShape) {
            auto [first, end] = found->subtreeRanges.get(tested->layout.index());
            chains = { };
            chains.decided = considered;
            for (LayoutID layout : considered) {
                uint32_t place = found->classPreorderNumbers[std::ranges::lower_bound(found->layouts, layout) - found->layouts.begin()];
                if (place >= first && place < end)
                    chains.instances.append(layout);
            }
            chains.testsObjects = !walker.shouldSkipPrototypeChecks();
            instancesOutsideHierarchy.shrink(0);
            hierarchy = found;
        }
    }
    if (hierarchy && !hierarchyDecidesEveryShape) {
        auto [first, end] = hierarchy->subtreeRanges.get(tested->layout.index());
        for (size_t index = 0; index < hierarchy->layouts.size(); ++index) {
            if (hierarchy->classPreorderNumbers[index] >= first && hierarchy->classPreorderNumbers[index] < end)
                chains.instances.append(hierarchy->layouts[index]);
            else
                hierarchyHasSiblings = true;
        }
        std::ranges::sort(chains.instances);
        chains.instances.shrink(std::ranges::unique(chains.instances).begin() - chains.instances.begin());
        chains.testsObjects = !walker.shouldSkipPrototypeChecks();
    }
    FormOfInstanceOf form;
    form.tested = tested->layout;
    form.testedIsConstructorFunction = tested->isConstructorFunction;
    form.conditions = WTF::move(tested->conditions);
    form.valueIsSurelyObject = value.isObject();
    form.testsPrototypeOfInstance = chains.testsObjects;

    const LinkTables& link = walker.facts().linked();
    bool onlyInstancesStandBelow = !form.testedIsConstructorFunction && form.tested.index() < link.layouts.size() && link.layout(form.tested).onlyOwnObjectsStandBelowPrototype && programMakesNoProxyForInstanceOf(walker);
    form.missMeansFalse = !chains.hasUndecidedLayouts && (areAllConsidered || onlyInstancesStandBelow);
    form.areAllInstances = areAllConsidered && form.missMeansFalse && !chains.decided.isEmpty() && chains.instances.size() == chains.decided.size();

    bool guardsAnswerForLinksLaidBetween = !chains.testsObjects || std::ranges::all_of(chains.instances, [&](LayoutID layout) {
        return walker.plan().hasClassRecord(declaredLayoutOf(program, layout));
    });
    if (!chains.instances.isEmpty()) {
        if (guardsAnswerForLinksLaidBetween && isTestableByShapeForInstanceOf(walker, chains.instances.span()))
            form.instances = types.layoutSets().ofSorted(chains.instances.span());
        else {
            form.missMeansFalse = false;
            form.areAllInstances = false;
        }
    }
    form.hierarchyDecidesEveryShape = hierarchyDecidesEveryShape;
    if (hierarchy && (form.instances || hierarchyDecidesEveryShape)) {
        form.hierarchyGuards = hierarchy->links.span();
        if (!form.missMeansFalse && hierarchyHasSiblings) {
            uint32_t placeOfTested = hierarchy->subtreeRanges.get(form.tested.index()).first;
            Vector<LayoutID, 8> nonAncestorLayouts;
            for (size_t index = 0; index < hierarchy->layouts.size(); ++index) {
                if (walker.shouldSkipSuperclassCheck() || hierarchy->classPreorderNumbers[index] >= placeOfTested || placeOfTested >= hierarchy->classSubtreeEnds[index])
                    nonAncestorLayouts.append(hierarchy->layouts[index]);
            }
            form.hierarchy = types.layoutSets().ofSorted(nonAncestorLayouts.span());
        }
    }
    if (form.instances && chains.testsObjects) {
        for (LayoutID layout : instancesOutsideHierarchy)
            addConditionOfInstanceOf(form.conditions, { MemberConditionKind::PrototypeLinkUnchanged, declaredLayoutOf(program, layout), ClassSide::Prototype, { } });
    }
    if (maySpendBytesForTimeInInstanceOf(walker) && form.testedIsConstructorFunction && (!form.missMeansFalse || hierarchyDecidesEveryShape) && walker.plan().hasClassRecord(form.tested) && hasOneClassObjectForInstanceOf(walker, form.tested))
        addConditionOfInstanceOf(form.conditions, conditionOnPrototypePropertyForInstanceOf(program, form.tested));
    return form;
}

static bool buildInstanceOfClassOfProgram(Walker& walker, const BodyValue& test, const InputsOfTypedOperation& inputs)
{
    TypeID typeOfValue = inputs.typeOf(test.valueOperand());
    TypeID typeOfConstructor = inputs.typeOf(test.constructorOperand());
    if (!typeOfValue || !typeOfConstructor)
        return false;
    bool valueHasLayoutPrototype = !inputs.areOnlyTypes() && knownCellFacts(walker, walker.referenceOf(test.valueOperand())).contains(CellFact::HasLayoutPrototype);
    valueHasLayoutPrototype |= walker.shouldAssumeLayoutPrototype() && !inputs.areOnlyTypes();
    TypeID expectedOfValue = !inputs.areOnlyTypes() && maySpendBytesForTimeInInstanceOf(walker) ? findExpectation(walker, test.valueOperand()).type : TypeID();
    std::optional<FormOfInstanceOf> form = planFormOfInstanceOf(walker, typeOfValue, typeOfConstructor, valueHasLayoutPrototype, expectedOfValue);
    if (!form)
        return false;

    bool isConstant = !form->instances ? form->missMeansFalse : form->areAllInstances && form->valueIsSurelyObject && !form->testsPrototypeOfInstance;
    if (isConstant && !walker.has(Mechanism::Folding)) {
        form->instances = { };
        form->hierarchy = { };
        form->hierarchyGuards = { };
        form->missMeansFalse = false;
        form->areAllInstances = false;
    }
    bool answersWithoutArms = form->conditions.isEmpty() && (!form->instances || (form->missMeansFalse && !form->testsPrototypeOfInstance));
    if (!answersWithoutArms && !mayBuildArmsForInstanceOf(walker)) {
        if (!form->conditions.isEmpty())
            return false;
        form->instances = { };
        form->hierarchy = { };
        form->hierarchyGuards = { };
        form->missMeansFalse = false;
        form->areAllInstances = false;
    }
    bool prototypeObjectIsInTable = !form->testedIsConstructorFunction || form->conditions.contains(conditionOnPrototypePropertyForInstanceOf(walker.program(), form->tested));
    if (!prototypeObjectIsInTable && !form->instances && !form->missMeansFalse)
        return false;
    bool hasOneClosedArm = !form->conditions.isEmpty() && !maySpendBytesForTimeInInstanceOf(walker);
    if (hasOneClosedArm && !form->instances && !form->missMeansFalse)
        return false;
    if (inputs.areOnlyTypes())
        return true;

    Reference value = walker.referenceOf(test.valueOperand());
    Reference constructor = walker.referenceOf(test.constructorOperand());
    if (!value || !value.isRecord() || !constructor || !constructor.isRecord())
        return false;
    if (form->instances && !value.isValue()) {
        if (form->testedIsConstructorFunction || !form->conditions.isEmpty())
            return false;
        form->instances = { };
        form->hierarchy = { };
        form->hierarchyGuards = { };
        form->missMeansFalse = false;
        form->areAllInstances = false;
    }

    Origin origin = walker.originOf(test);
    AnswersOfInstanceOf answers(walker);
    if ((form->hierarchy || (form->hierarchyDecidesEveryShape && form->instances && !form->areAllInstances)) && walker.canThreadBranchAfter(test)) {
        answers.letArmsEndWithBranchBehind(test);
        walker.count(StatisticID::InstanceOfTestsJumpThreaded);
    }
    auto recordCheckedName = [&](Reference name) {
        if (answers.armsEndWithBranch() && name && !walker.isUnreachable() && !walker.state().checkedNameOf(value))
            walker.state().noteCheckedName(value, name);
    };
    if (form->hierarchyDecidesEveryShape)
        walker.count(StatisticID::InstanceOfTestsDecidedByHierarchy);
    bool hasArms = false;
    BlockID asWritten;
    for (std::span<const MemberConditionRecord> conditions : { std::span<const MemberConditionRecord>(form->conditions.span()), form->hierarchyGuards }) {
        if (conditions.empty() || (walker.shouldSkipHierarchyGuards() && conditions.data() == form->hierarchyGuards.data()) || (walker.shouldSkipClassGuards() && conditions.data() == form->conditions.span().data()))
            continue;
        bool asksInFront = prototypeObjectIsInTable && !hasOneClosedArm && walker.canRestartIteration() && walker.iterationsPreserveMemberGuards();
        Reference areUnchanged;
        {
            Walker::SpecializedPreheaderScope preheaderScope(walker, asksInFront);
            Draft guards(Opcode::AreMembersUnchanged, origin);
            guards.setConditions(conditions);
            areUnchanged = walker.isUnreachable() ? Reference() : walker.build(WTF::move(guards));
        }
        if (!areUnchanged)
            return true;
        walker.recordSpeculation();
        if (asksInFront && walker.tryBranchInPreheader(areUnchanged, true, Walker::IterationPurity::EffectFree, Walker::GuardedIterations::SomeWithUnguardedVersion))
            walker.count(StatisticID::InstanceOfGuardsHoisted);
        else {
            if (asksInFront)
                walker.count(StatisticID::InstanceOfGuardsNotHoisted);
            BlockID unchanged = walker.addBlock();
            if (!asWritten)
                asWritten = walker.addBlock(BlockFrequency::Rare);
            walker.branch(areUnchanged, unchanged, asWritten);
            hasArms = true;
            walker.continueIn(unchanged);
        }
    }

    auto verifyAgainstChainWalk = [&](bool shapeBasedResult) {
        if (!walker.checksProofs() || !prototypeObjectIsInTable || walker.isUnreachable())
            return;
        WalkState believed = walker.state();
        Draft read(Opcode::GetClassPrototype, origin);
        walker.chooseConservativeFields(read);
        read.setClassObjectOperand(constructor);
        read.setLayout(form->tested);
        if (Reference prototype = walker.build(WTF::move(read))) {
            Draft chainWalk(Opcode::TryFindInPrototypeChain, origin);
            walker.chooseConservativeFields(chainWalk);
            chainWalk.setValueOperand(value);
            chainWalk.setPrototypeOperand(prototype);
            if (Reference found = walker.buildTestOfCheck(WTF::move(chainWalk))) {
                Draft answer(Opcode::Integer);
                answer.setInteger(shapeBasedResult);
                Draft comparison(Opcode::StrictEq, origin);
                walker.chooseConservativeFields(comparison);
                comparison.setLeftOperand(found);
                comparison.setRightOperand(walker.constant(WTF::move(answer)));
                comparison.setForm(OperatorForm::Integers);
                if (Reference agrees = walker.buildTestOfCheck(WTF::move(comparison)))
                    walker.checkProof(agrees, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::OtherOperation, origin);
            }
        }
        if (!walker.isUnreachable()) {
            Draft look(Opcode::HasDefaultHasInstance, origin);
            walker.chooseConservativeFields(look);
            look.setConstructorOperand(constructor);
            if (Reference findsDefault = walker.buildTestOfCheck(WTF::move(look)))
                walker.checkProof(findsDefault, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::OtherOperation, origin);
        }
        if (!walker.isUnreachable())
            walker.state() = WTF::move(believed);
    };

    bool shouldVerifyHierarchyResults = walker.checksProofs() && form->valueIsSurelyObject;
    auto buildAnswerOfMiss = [&]() -> Reference {
        if (form->missMeansFalse && shouldVerifyHierarchyResults)
            verifyAgainstChainWalk(false);
        if (form->missMeansFalse)
            return booleanAnswerOfInstanceOf(walker, false);
        if (!prototypeObjectIsInTable)
            return buildInstanceOfAsWritten(walker, test);
        Draft read(Opcode::GetClassPrototype, origin);
        walker.chooseConservativeFields(read);
        read.setClassObjectOperand(constructor);
        read.setLayout(form->tested);
        Reference prototype = walker.build(WTF::move(read));
        return walker.canRestartIteration() ? buildWalkOfChainThatGivesWayForInstanceOf(walker, value, prototype, origin) : buildWalkOfChainForInstanceOf(walker, value, prototype, origin);
    };

    bool testsPrototype = form->testsPrototypeOfInstance && !knownCellFacts(walker, value).contains(CellFact::HasLayoutPrototype) && !(walker.shouldAssumeLayoutPrototype() && form->hierarchyDecidesEveryShape);
    if (form->testsPrototypeOfInstance && !testsPrototype)
        walker.checkFactsRememberedOfCell(value, origin, CellFact::HasLayoutPrototype);
    auto buildAnswerWhereClassIsUnchanged = [&]() -> Reference {
        if (!form->instances)
            return buildAnswerOfMiss();
        if (form->areAllInstances && form->valueIsSurelyObject && !testsPrototype && walker.has(Mechanism::Folding)) {
            if (shouldVerifyHierarchyResults)
                verifyAgainstChainWalk(true);
            return booleanAnswerOfInstanceOf(walker, true);
        }
        Draft shapeTest(Opcode::ShapeTest, origin);
        walker.chooseConservativeFields(shapeTest);
        shapeTest.setObjectOperand(value);
        shapeTest.setLayouts(form->instances);
        bool shapeTestIsAnswer = form->missMeansFalse && !testsPrototype && !shouldVerifyHierarchyResults && !answers.armsEndWithBranch();
        if (!hasArms && shapeTestIsAnswer)
            shapeTest.inputValueGiven = test.id();
        Reference isInstance = walker.build(WTF::move(shapeTest));
        if (!isInstance || shapeTestIsAnswer)
            return isInstance;

        BlockID hasShape = walker.addBlock();
        bool missIsAskedAsWritten = asWritten && (hasOneClosedArm || !prototypeObjectIsInTable);
        BlockID walksChain = missIsAskedAsWritten ? asWritten : walker.addBlock(walker.canRestartIteration() ? BlockFrequency::Normal : BlockFrequency::Rare);
        bool asksWhetherMissIsObject = hasOneClosedArm && !form->missMeansFalse && !form->valueIsSurelyObject;
        bool testsMissForSibling = form->hierarchy && !form->missMeansFalse && !asksWhetherMissIsObject && !missIsAskedAsWritten;
        BlockID hasOtherShape = form->missMeansFalse || asksWhetherMissIsObject || testsMissForSibling ? walker.addBlock() : walksChain;
        walker.branch(isInstance, hasShape, hasOtherShape);
        if (!hasArms)
            walker.recordSpeculation();
        hasArms = true;
        if (asksWhetherMissIsObject) {
            walker.continueIn(hasOtherShape);
            if (Reference isObject = buildTypeTestOfInstanceOf(walker, value, TypeTable::object, origin)) {
                BlockID isPrimitive = walker.addBlock();
                walker.branch(isObject, walksChain, isPrimitive);
                walker.continueIn(isPrimitive);
                answers.arrive(booleanAnswerOfInstanceOf(walker, false));
            }
        }

        walker.continueIn(hasShape);
        if (testsPrototype) {
            Reference instance = buildNarrowedValue(walker, value, isInstance, OperandIndex::ShapeTest::object, origin);
            Reference prototypeCheck;
            if (instance) {
                Draft prototypeCheckDraft(Opcode::HasLayoutPrototype, origin);
                walker.chooseConservativeFields(prototypeCheckDraft);
                prototypeCheckDraft.setObjectOperand(instance);
                prototypeCheck = walker.build(WTF::move(prototypeCheckDraft));
            }
            if (prototypeCheck) {
                BlockID instanceCase = walker.addBlock();
                walker.branch(prototypeCheck, instanceCase, walksChain);
                walker.continueIn(instanceCase);
                verifyAgainstChainWalk(true);
                recordCheckedName(instance);
                walker.state().recordCellFacts(value, CellFact::HasLayoutPrototype);
                walker.state().recordCellFacts(instance, CellFact::HasLayoutPrototype);
            } else
                verifyAgainstChainWalk(true);
        } else if (answers.armsEndWithBranch()) {
            Reference instance = buildNarrowedValue(walker, value, isInstance, OperandIndex::ShapeTest::object, origin);
            verifyAgainstChainWalk(true);
            if (instance) {
                recordCheckedName(instance);
                walker.state().recordCellFacts(instance, CellFact::HasLayoutPrototype);
            }
        } else
            verifyAgainstChainWalk(true);
        answers.arrive(booleanAnswerOfInstanceOf(walker, true));

        if (testsMissForSibling) {
            walker.continueIn(hasOtherShape);
            Draft siblingTest(Opcode::ShapeTest, origin);
            walker.chooseConservativeFields(siblingTest);
            siblingTest.setObjectOperand(value);
            siblingTest.setLayouts(form->hierarchy);
            Reference isSibling = walker.build(WTF::move(siblingTest));
            Reference prototypeCheck;
            Reference sibling;
            if (isSibling) {
                BlockID siblingShapeCase = walker.addBlock();
                walker.branch(isSibling, siblingShapeCase, walksChain);
                walker.continueIn(siblingShapeCase);
                sibling = buildNarrowedValue(walker, value, isSibling, OperandIndex::ShapeTest::object, origin);
                if (sibling) {
                    Draft prototypeCheckDraft(Opcode::HasLayoutPrototype, origin);
                    walker.chooseConservativeFields(prototypeCheckDraft);
                    prototypeCheckDraft.setObjectOperand(sibling);
                    prototypeCheck = walker.build(WTF::move(prototypeCheckDraft));
                }
            }
            if (prototypeCheck) {
                BlockID siblingCase = walker.addBlock();
                walker.branch(prototypeCheck, siblingCase, walker.shouldIgnoreSiblingPrototypes() ? siblingCase : walksChain);
                walker.continueIn(siblingCase);
                walker.count(StatisticID::InstanceOfMissesDecidedByHierarchy);
                verifyAgainstChainWalk(false);
                recordCheckedName(sibling);
                walker.state().recordCellFacts(value, CellFact::HasLayoutPrototype);
                walker.state().recordCellFacts(sibling, CellFact::HasLayoutPrototype);
                answers.arrive(booleanAnswerOfInstanceOf(walker, false));
            } else if (!walker.isUnreachable())
                walker.jumpTo(walksChain);
        }

        if (form->missMeansFalse) {
            walker.continueIn(hasOtherShape);
            if (form->hierarchyDecidesEveryShape)
                walker.count(StatisticID::InstanceOfMissesDecidedByHierarchy);
            Reference sibling;
            if (answers.armsEndWithBranch() && !walker.isUnreachable()) {
                Draft left(Opcode::Narrowed, origin);
                walker.chooseConservativeFields(left);
                left.setValueOperand(value);
                left.setTestOperand(isInstance);
                left.setOperandIndex(OperandIndex::ShapeTest::object);
                left.setIsOnTakenEdge(false);
                if (walker.resultTypeOf(left))
                    sibling = walker.build(WTF::move(left));
            }
            if (shouldVerifyHierarchyResults)
                verifyAgainstChainWalk(false);
            if (sibling && !walker.isUnreachable()) {
                recordCheckedName(sibling);
                walker.state().recordCellFacts(sibling, CellFact::HasLayoutPrototype);
            }
            answers.arrive(booleanAnswerOfInstanceOf(walker, false));
        }
        if (!missIsAskedAsWritten && (!form->missMeansFalse || testsPrototype)) {
            walker.continueIn(walksChain);
            bool missMeansFalse = std::exchange(form->missMeansFalse, false);
            if (answers.armsEndWithBranch()) {
                Walker::ClosedArm closedArm(walker);
                answers.arrive(buildAnswerOfMiss());
            } else
                answers.arrive(buildAnswerOfMiss());
            form->missMeansFalse = missMeansFalse;
        }
        return { };
    };

    Reference answer = buildAnswerWhereClassIsUnchanged();
    if (hasArms) {
        answers.arrive(answer);
        if (asWritten) {
            walker.continueIn(asWritten);
            if (prototypeObjectIsInTable && !hasOneClosedArm && !walker.isUnreachable() && walker.canRestartIteration())
                walker.restartIteration();
            else if (answers.armsEndWithBranch()) {
                Walker::ClosedArm closedArm(walker);
                answers.arrive(buildInstanceOfAsWritten(walker, test));
            } else
                answers.arrive(buildInstanceOfAsWritten(walker, test));
        }
        answer = answers.meet();
    }
    if (answer)
        walker.bind(test.id(), answer);
    return true;
}

static std::optional<BuiltinTestedByInstanceOf> describeBuiltinTestedByInstanceOf(const Walker& walker, TypeID typeOfConstructor)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    Type written = walker.types().at(typeOfConstructor);
    BuiltinID constructor = written.isObject() ? singleBuiltinOf(walker.types(), written) : BuiltinID();
    if (!constructor || table.at(constructor).kind != BuiltinKind::Constructor)
        return std::nullopt;

    BuiltinTestedByInstanceOf tested;
    tested.constructor = table.canonicalID(constructor);
    if (tested.constructor == table.canonicalID(Builtins::File))
        return std::nullopt;
    BuiltinID asked = findConstantMemberFrom(program, walker.facts(), constructor, program.keyOf(table.nameOf(WellKnownSymbol::HasInstance)));
    if (!asked || table.canonicalID(asked) != Builtins::FunctionPrototypeSymbolHasInstance)
        return std::nullopt;

    BuiltinID location = findConstantMemberFrom(program, walker.facts(), constructor, program.keyOf(BuiltinNames::prototype));
    BuiltinID prototype = location ? table.canonicalID(location) : BuiltinID();
    if (!prototype || !isObjectKind(table.at(prototype).kind) || !BuiltinCellKey::object(prototype).hasCell(table) || isMadeWithCodeOfLibrary(table, location))
        return tested;
    tested.prototype = prototype;

    BuiltinClassID builtinClass = table.at(tested.constructor).instanceClass;
    if (!builtinClass)
        return tested;
    const BuiltinClass& instanceClass = table.at(builtinClass);
    if (instanceClass.constructor && instanceClass.prototype && table.canonicalID(instanceClass.constructor) == tested.constructor && table.canonicalID(instanceClass.prototype) == prototype)
        tested.builtinClass = builtinClass;
    return tested;
}

static bool holdsNoObjectForInstanceOf(const Walker& walker, TypeID typeOfValue)
{
    Type subject = walker.types().at(typeOfValue);
    return subject.isJavaScriptValue() && !subject.mayBeUnspecified() && !subject.hasObjects();
}

static bool classTestAnswersBuiltinInstanceOf(const Walker& walker, const BuiltinTestedByInstanceOf& tested, TypeID typeOfValue)
{
    const BuiltinTable& table = walker.program().builtins();
    if (!tested.builtinClass || !walker.has(Mechanism::EscapeAnalysis) || !walker.plan().hasOneInstance())
        return false;
    if (pristineTestOf(table, tested.builtinClass) != PristineTest::MayPass || !table.mayHavePristineInstances(tested.builtinClass))
        return false;
    TypeArena& types = walker.types();
    TypeID instances = types.builtinClass(tested.builtinClass);
    if (!types.overlap(typeOfValue, instances))
        return false;
    return (hasInstanceTest(table, tested.builtinClass) && types.testablePartOf(instances) == instances) || types.isSubset(typeOfValue, instances);
}

static Reference buildWalkOfChainToBuiltinForInstanceOf(Walker& walker, Reference value, BuiltinID prototype, Origin origin)
{
    Draft found(Opcode::Builtin, origin);
    walker.chooseConservativeFields(found);
    found.setBuiltin(prototype);
    return buildWalkOfChainForInstanceOf(walker, value, walker.build(WTF::move(found)), origin);
}

static Reference buildClassTestsOfBuiltinInstanceOf(Walker& walker, const BuiltinTestedByInstanceOf& tested, Reference value, Origin origin)
{
    TypeArena& types = walker.types();
    TypeID instances = types.builtinClass(tested.builtinClass);
    AnswersOfInstanceOf answers(walker);
    BlockID walksChain = walker.addBlock(BlockFrequency::Unlikely);
    walker.recordSpeculation();

    Reference cell = value;
    if (!types.isSubset(walker.typeOf(value), instances)) {
        Reference isOfClass = buildTypeTestOfInstanceOf(walker, value, instances, origin);
        if (!isOfClass)
            return { };
        BlockID ofClass = walker.addBlock();
        walker.branch(isOfClass, ofClass, walksChain);
        walker.continueIn(ofClass);
        cell = buildNarrowedValue(walker, value, isOfClass, OperandIndex::TypeTest::value, origin);
        if (!cell)
            return { };
    }
    Reference isAsMade = buildTestOfObjectAsMade(walker, cell, origin);
    if (isAsMade) {
        BlockID asMade = walker.addBlock();
        walker.branch(isAsMade, asMade, walksChain);
        walker.continueIn(asMade);
        answers.arrive(booleanAnswerOfInstanceOf(walker, true));
    } else if (!walker.isUnreachable())
        walker.jumpTo(walksChain);

    walker.continueIn(walksChain);
    answers.arrive(buildWalkOfChainToBuiltinForInstanceOf(walker, value, tested.prototype, origin));
    return answers.meet();
}

static bool buildInstanceOfBuiltinByValue(Walker& walker, const BodyValue& test, const InputsOfTypedOperation& inputs)
{
    TypeID typeOfValue = inputs.typeOf(test.valueOperand());
    TypeID typeOfConstructor = inputs.typeOf(test.constructorOperand());
    if (!typeOfValue || !typeOfConstructor)
        return false;
    std::optional<BuiltinTestedByInstanceOf> tested = describeBuiltinTestedByInstanceOf(walker, typeOfConstructor);
    if (!tested)
        return false;

    bool holdsNoObject = walker.has(Mechanism::Folding) && holdsNoObjectForInstanceOf(walker, typeOfValue);
    bool testsClass = false;
    if (!holdsNoObject && tested->prototype && mayBuildArmsForInstanceOf(walker) && classTestAnswersBuiltinInstanceOf(walker, *tested, typeOfValue)) {
        TypeID expected = walker.isInLoop() ? TypeID() : findExpectation(walker, test.valueOperand()).type;
        testsClass = walker.isInLoop() || (expected && walker.types().overlap(expected, walker.types().builtinClass(tested->builtinClass)));
    }
    if (inputs.areOnlyTypes())
        return holdsNoObject || testsClass;

    Reference value = walker.referenceOf(test.valueOperand());
    Reference constructor = walker.referenceOf(test.constructorOperand());
    if (!value || !value.isRecord() || !constructor || !constructor.isRecord())
        return false;
    Origin origin = walker.originOf(test);
    Reference answer;
    if (holdsNoObject)
        answer = booleanAnswerOfInstanceOf(walker, false);
    else {
        bool isSeenAsMade = tested->builtinClass && walker.has(Mechanism::Folding) && walker.plan().hasOneInstance() && walker.types().isSubset(walker.typeOf(value), walker.types().builtinClass(tested->builtinClass)) && isKnownPristine(walker, value);
        if (isSeenAsMade) {
            walker.verifyPristineObject(value);
            answer = booleanAnswerOfInstanceOf(walker, true);
        }
        else if (testsClass && value.isValue())
            answer = buildClassTestsOfBuiltinInstanceOf(walker, *tested, value, origin);
        else
            return false;
    }
    if (answer)
        walker.bind(test.id(), answer);
    return true;
}

static_assert(opcodesThatBuildFormOnShapesTakes == (OpcodeSet { Opcode::InstanceOf }));

bool buildFormOnShapesOfItsOpcodes(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    switch (value.opcode()) {
    case Opcode::InstanceOf:
        return buildInstanceOfClassOfProgram(walker, value, inputs) || buildInstanceOfBuiltinByValue(walker, value, inputs);
    default:
        return false;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
