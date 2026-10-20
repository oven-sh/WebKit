/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestSite.h"

#if ENABLE(AOT)

#include "AOTArrayCapacities.h"
#include "AOTArrayStorageForms.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTBuiltinMembers.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTIntegerRelations.h"
#include "AOTResolveAccesses.h"
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

using ElementAccessTests = Vector<Value, 6>;

namespace {

// What the function itself does with an array says what to expect of it: a position that it compares with the length lies inside, and an array that it stores into has storage of its own.
class ArrayUses {
    WTF_MAKE_NONCOPYABLE(ArrayUses);
public:
    explicit ArrayUses(FunctionPassContext&);

    bool isComparedWithLength(ValueID array, ValueID position, BlockID at);
    bool isStoredInto(ValueID array) const { return m_storedArrays.contains(m_analyses.identityOf(array)); }

private:
    ValueID arrayMeasuredBy(const Value&) const;

    const Program& m_program;
    FunctionAnalyses& m_analyses;
    HashMap<ValueID, Vector<ValueID, 2>> m_lengthReads;
    HashSet<ValueID> m_storedArrays;
    std::optional<IntegerRelations> m_relations;
};

ArrayUses::ArrayUses(FunctionPassContext& context)
    : m_program(context.program)
    , m_analyses(context.analyses)
{
    const Function& function = context.function;
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            if (ValueID array = arrayMeasuredBy(value))
                m_lengthReads.ensure(m_analyses.identityOf(array), [] { return Vector<ValueID, 2>(); }).iterator->value.append(id);
            if (value.opcode == Opcode::PutArrayElement)
                m_storedArrays.add(m_analyses.identityOf(value.arrayOperand()));
            else if (std::optional<Access> access = elementAccessOf(value.opcode); access == Access::Store || access == Access::Define)
                m_storedArrays.add(m_analyses.identityOf(value.objectOperand()));
        }
    }
}

ValueID ArrayUses::arrayMeasuredBy(const Value& value) const
{
    std::optional<MeasuredStorage> measured = storageMeasuredBy(m_program, value);
    return measured && measured->heap == AbstractHeap::ArrayStorage ? measured->owner : ValueID();
}

bool ArrayUses::isComparedWithLength(ValueID array, ValueID position, BlockID at)
{
    auto lengthReads = m_lengthReads.find(m_analyses.identityOf(array));
    if (lengthReads == m_lengthReads.end())
        return false;

    using Answer = IntegerRelations::Answer;
    if (!m_relations)
        m_relations.emplace(m_analyses);
    if (m_relations->isAtLeast(position, 0, at) != Answer::Proven)
        return false;
    return std::ranges::any_of(lengthReads->value, [&](ValueID length) {
        return m_relations->isAtMost(position, length, -1, at) == Answer::Proven;
    });
}

} // anonymous namespace

static bool hasGuardedForm(const ElementAccessPlan& plan)
{
    if (plan.isCharacterReadAfterTests())
        return true;
    if (!plan.isResolved() || plan.needsNoTest())
        return false;
    if (plan.form != ElementAccessForm::Array || plan.arrayStorage == ArrayStorage::Other)
        return true;
    return hasArrayStorageForm(plan.arrayStorage);
}

static TypeID testedTypeOfPrimitive(const TestSiteReporter& reporter, ValueID value)
{
    std::optional<Type> predicted = reporter.predictedPartOf(value);
    if (!predicted)
        return { };
    return predicted->isNumber() ? testedNumberTypeContaining(*predicted) : reporter.types().intern(*predicted);
}

static std::optional<ElementAccessTests> createTests(const TestSiteReporter& reporter, const ArrayUses& arrays, const Value& use, const ElementAccessPlan& plan)
{
    ValueID receiver = use.objectOperand();
    ValueID key = use.propertyOperand();
    ElementAccessTests tests;

    // IsPristine proves a guard and leaves the type of the receiver as it is, and the element operations take an operand typed as the class.
    if (plan.testsReceiverClass) {
        std::optional<Type> predicted = reporter.predictedPartOf(receiver);
        if (!predicted)
            return std::nullopt;
        Type instances = typeOfInstances(reporter.program().builtins(), plan.builtinClass, *predicted);
        tests.append(makeTypeTest(receiver, reporter.types().intern(instances)));
    }
    if (plan.testsReceiverIsString)
        tests.append(makeTypeTest(receiver, TypeTable::string));
    if (!plan.requiredAspects.isEmpty())
        tests.append(makePristineTest(receiver, plan.builtinClass, plan.requiredAspects));
    if (plan.testsArrayStorage)
        tests.append(makeArrayStorageTest(receiver, plan.arrayStorage, arrays.isStoredInto(receiver)));
    if (plan.testsKeyIsNumber) {
        TypeID tested = testedTypeOfPrimitive(reporter, key);
        if (!tested)
            return std::nullopt;
        tests.append(makeTypeTest(key, tested));
    }
    if (plan.testsStoredValue) {
        TypeID tested = testedTypeOfPrimitive(reporter, use.valueOperand());
        if (!tested)
            return std::nullopt;
        tests.append(makeTypeTest(use.valueOperand(), tested));
    }
    if (plan.testsInBounds)
        tests.append(makeBoundsTest(receiver, key));
    return tests;
}

static Type keyTypeAfterTests(const TestSiteReporter& reporter, const Value& use, const ElementAccessPlan& plan)
{
    ValueID key = use.propertyOperand();
    if (plan.testsKeyIsNumber) {
        if (TypeID tested = testedTypeOfPrimitive(reporter, key))
            return reporter.typeOf(key).intersectWith(reporter.types().at(tested), reporter.types());
    }
    return reporter.typeOf(key);
}

static Effects effectsAfterTests(const TestSiteReporter& reporter, const Value& use, const ElementAccessPlan& plan)
{
    if (plan.isCharacterReadAfterTests())
        return declaredBoundOf(Opcode::StringRangeCharacterOrUndefined);
    return effectsAfterRewrite(plan, keyTypeAfterTests(reporter, use, plan));
}

// An array of holes is allocated with the kind that its allocation names, and has its length from the start: a store into it is a store inside the length, which the forms with a kind have inline.
static void predictStorageOfNewArrayOfHoles(FunctionPassContext& context, const Value& access, ElementAccessPlan& plan)
{
    if (plan.form != ElementAccessForm::Array || plan.arrayStorage != ArrayStorage::Other)
        return;
    std::optional<ArrayStorage> allocated = arrayStorageOfNewHoles(context.function.at(context.analyses.identityOf(access.objectOperand())));
    if (!allocated)
        return;
    plan.arrayStorage = *allocated;
    plan.testsArrayStorage = true;
    plan.level = KnowledgeLevel::Predicted;
}

// The test is placed only where the comparison says how it will come out: an append by index, `a[n++] = x`, fails it every time.
static void predictPositionInsideLength(const TestSiteReporter& reporter, ArrayUses& arrays, const Value& use, ElementAccessPlan& plan)
{
    if (plan.form != ElementAccessForm::Array || !plan.testsArrayStorage || !Value::opcodesWithIsInBounds.contains(plan.operation))
        return;
    if (!keyTypeAfterTests(reporter, use, plan).isInteger())
        return;
    // A store that converts the storage is no store of one element.
    if (OperandPosition::value(plan.operation) && !canStoreWithoutConversion(plan.arrayStorage, reporter.typeOf(use.valueOperand())))
        return;
    plan.testsInBounds = arrays.isComparedWithLength(use.objectOperand(), use.propertyOperand(), reporter.point().block);
}

void reportElementAccessTests(TestSiteReporter& reporter)
{
    std::optional<ArrayUses> arrays;
    reporter.forEachCandidate([&](const Value& use) {
        if (!elementAccessOf(use.opcode))
            return;

        ProgramPoint point = reporter.point();
        std::optional<ElementAccessSite> site = planElementAccessSite(reporter.context(), ValueSite { point.block, point.index, reporter.use() });
        if (!site)
            return;
        predictStorageOfNewArrayOfHoles(reporter.context(), use, site->plan);
        if (!hasGuardedForm(site->plan))
            return;
        if (!arrays)
            arrays.emplace(reporter.context());
        predictPositionInsideLength(reporter, *arrays, use, site->plan);

        std::optional<ElementAccessTests> tests = createTests(reporter, *arrays, use, site->plan);
        if (!tests)
            return;
        reporter.report(tests->span(), effectsAfterTests(reporter, use, site->plan));
    });
}

static void applyPassedTest(ElementAccessPlan& plan, const Value& test)
{
    if (test.opcode == Opcode::HasArrayStorage) {
        plan.arrayStorage = test.arrayStorage();
        plan.testsArrayStorage = false;
        return;
    }
    if (test.opcode == Opcode::IsPristine) {
        plan.requiredAspects = plan.requiredAspects - test.aspects();
        return;
    }
    if (test.opcode == Opcode::IsInBounds) {
        plan.isInBounds = true;
        return;
    }
    RELEASE_ASSERT(test.opcode == Opcode::TypeTest);
}

void rewriteTestedElementAccess(TestSiteRewriter& rewriter)
{
    ProgramPoint point = rewriter.point();
    std::optional<ElementAccessSite> site = planElementAccessSite(rewriter.context(), ValueSite { point.block, point.index, rewriter.use() });
    RELEASE_ASSERT(site);
    // The tests have given the operands the types of a character read, which has no operation of its own before selection.
    if (site->plan.form == ElementAccessForm::String)
        return;
    RELEASE_ASSERT(site->plan.isResolved());

    for (const Value& test : rewriter.tests()) {
        RELEASE_ASSERT(rewriter.guard() || provesForever(provesOf(test.opcode)));
        applyPassedTest(site->plan, test);
    }
    // Sites that share the test of the class share the value that passed it, which has the prediction of one of them. A storage that this site did not test is not relied on.
    if (site->plan.testsArrayStorage) {
        site->plan.arrayStorage = ArrayStorage::Other;
        site->plan.testsArrayStorage = false;
        site->plan.isInBounds = false;
    }
    RELEASE_ASSERT(site->plan.needsNoTest());
    site->plan.level = KnowledgeLevel::Predicted;

    const Value& use = rewriter.useValue();
    Access access = *elementAccessOf(use.opcode);
    bool storesValue = access == Access::Store || access == Access::Define;
    AccessOperands operands { use.objectOperand(), storesValue ? use.valueOperand() : ValueID(), rewriter.guard() };

    // A ScopedLambda hands its functor each argument as an lvalue.
    auto insertBeforeUse = [&](auto&& value) -> ValueID {
        return rewriter.insertBeforeUse(WTF::move(value));
    };
    InsertValue insert(insertBeforeUse);
    AccessRewriter(rewriter.context(), insert).rewrite(*site, operands);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
