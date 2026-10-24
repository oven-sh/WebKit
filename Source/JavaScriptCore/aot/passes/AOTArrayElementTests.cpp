/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestSite.h"

#if ENABLE(AOT)

#include "AOTArrayCapacities.h"
#include "AOTArrayElementPresence.h"
#include "AOTArrayStorageForms.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTypes.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTGuardAvailability.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTIntegerRelations.h"
#include "AOTResolveAccesses.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(OperandIndex::IsElementPresent::array == testedOperandIndex && OperandIndex::IsInsideView::view == testedOperandIndex);

using ElementAccessTests = Vector<Value, 6>;

// ResolveAccesses has rewritten these: the types of their operands needed no test. Whether the position holds an element is still open.
static constexpr OpcodeSet resolvedAccessesWithPresenceTest { Opcode::GetArrayElement, Opcode::PutArrayElement, Opcode::GetTypedArrayElement, Opcode::PutTypedArrayElement };

namespace {

// What the function itself does with an array says what to expect of it: a position that it compares with the length lies inside, and an array that it stores into has storage of its own.
class ArrayUses {
    WTF_MAKE_NONCOPYABLE(ArrayUses);
public:
    explicit ArrayUses(FunctionPassContext&);

    bool isComparedWithLength(ValueID array, ValueID position, BlockID at);
    bool isStoredInto(ValueID array);

private:
    void collectOnce();
    ValueID arrayMeasuredBy(const Value&) const;
    ValueID lengthAtCreation(const Value&) const;

    const Program& m_program;
    const Function& m_function;
    FunctionAnalyses& m_analyses;
    bool m_isCollected { false };
    struct Creation {
        ValueID length;
        BlockID block;
    };

    HashMap<ValueID, Vector<ValueID, 2>> m_lengthReads;
    Vector<Creation, 2> m_creations;
    HashSet<ValueID> m_storedArrays;
    std::optional<IntegerRelations> m_relations;
};

ArrayUses::ArrayUses(FunctionPassContext& context)
    : m_program(context.program)
    , m_function(context.function)
    , m_analyses(context.analyses)
{
}

void ArrayUses::collectOnce()
{
    if (m_isCollected)
        return;
    m_isCollected = true;

    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        for (ValueID id : m_function.at(block).values) {
            const Value& value = m_function.at(id);
            if (ValueID array = arrayMeasuredBy(value))
                m_lengthReads.ensure(m_analyses.identityOf(array), [] { return Vector<ValueID, 2>(); }).iterator->value.append(id);
            if (ValueID length = lengthAtCreation(value)) {
                m_lengthReads.ensure(m_analyses.identityOf(id), [] { return Vector<ValueID, 2>(); }).iterator->value.append(length);
                m_creations.append({ length, block });
            }
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

// An array of holes has its length from the start.
ValueID ArrayUses::lengthAtCreation(const Value& value) const
{
    switch (value.opcode) {
    case Opcode::NewArrayLike:
    case Opcode::NewContiguousArray:
    case Opcode::NewDoubleArray:
        return value.lengthOperand();
    case Opcode::CallBuiltin:
        if (inlineImplementationOf(m_program, value) == InlineBuiltin::ArrayWithLength && !value.argumentsOperands().empty())
            return value.argumentsOperands()[0];
        return { };
    default:
        return { };
    }
}

bool ArrayUses::isStoredInto(ValueID array)
{
    collectOnce();
    return m_storedArrays.contains(m_analyses.identityOf(array));
}

bool ArrayUses::isComparedWithLength(ValueID array, ValueID position, BlockID at)
{
    collectOnce();
    auto lengthReads = m_lengthReads.find(m_analyses.identityOf(array));
    if (lengthReads == m_lengthReads.end())
        return false;

    using Answer = IntegerRelations::Answer;
    if (!m_relations) {
        m_relations.emplace(m_analyses);
        // Any other number has thrown a RangeError.
        for (const Creation& creation : m_creations)
            m_relations->assumeRange(creation.length, Range::uint32(), creation.block);
    }
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

static bool isDenseArray(const Type& array)
{
    return pristineAspectsProvenBy(array, BuiltinClasses::Array).contains(PristineAspects::Dense);
}

static TypeID testedTypeOfPrimitive(const TestSiteReporter& reporter, ValueID value)
{
    std::optional<Type> expected = reporter.expectedPartOf(value);
    if (!expected)
        return { };
    return expected->isNumber() ? testedNumberTypeContaining(*expected) : reporter.types().intern(*expected);
}

// Where nothing is expected of the key itself, the plan tests it because its receiver has elements: an index is expected.
static TypeID testedTypeOfKey(const TestSiteReporter& reporter, ValueID key)
{
    if (TypeID tested = testedTypeOfPrimitive(reporter, key))
        return tested;
    if (reporter.function().at(key).expected())
        return { };
    return reporter.typeOf(key).overlaps(reporter.types().at(TypeTable::int32), reporter.program()) ? TypeTable::int32 : TypeID();
}

static std::optional<ElementAccessTests> createTests(const TestSiteReporter& reporter, ArrayUses& arrays, const Value& use, const ElementAccessPlan& plan)
{
    ValueID receiver = use.objectOperand();
    ValueID key = use.propertyOperand();
    ElementAccessTests tests;

    // IsPristine proves a guard and leaves the type of the receiver as it is, and the element operations take an operand typed as the class.
    if (plan.testsReceiverClass) {
        std::optional<Type> expected = reporter.expectedPartOf(receiver);
        if (!expected)
            return std::nullopt;
        Type instances = typeOfInstances(reporter.program().builtins(), plan.builtinClass, *expected);
        tests.append(makeTypeTest(receiver, reporter.types().intern(instances)));
    } else if (plan.checksNullish) {
        // The tests behind this one look into the receiver. Undefined and null fail it, and the access by key throws what it always throws.
        tests.append(makeTypeTest(receiver, TypeTable::object));
    }
    if (plan.testsReceiverIsString)
        tests.append(makeTypeTest(receiver, TypeTable::string));
    if (!plan.requiredAspects.isEmpty())
        tests.append(makePristineTest(receiver, plan.builtinClass, plan.requiredAspects));
    if (plan.testsArrayStorage)
        tests.append(makeArrayStorageTest(receiver, plan.arrayStorage, arrays.isStoredInto(receiver)));
    if (plan.testsKeyIsNumber) {
        TypeID tested = testedTypeOfKey(reporter, key);
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
    // Last: a test with a guard operand gets the proofs of the earlier tests of its site, and this one relies on the kind of storage.
    if (plan.testsPresence)
        tests.append(plan.form == ElementAccessForm::Array ? makePresenceTest(receiver, key, plan.arrayStorage, plan.keyIsIntegral) : makeViewBoundsTest(receiver, key, plan.typedArrayType, plan.keyIsIntegral));
    return tests;
}

static Type keyTypeAfterTests(const TestSiteReporter& reporter, const Value& use, const ElementAccessPlan& plan)
{
    ValueID key = use.propertyOperand();
    if (plan.testsKeyIsNumber) {
        if (TypeID tested = testedTypeOfKey(reporter, key))
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
static void expectStorageOfNewArrayOfHoles(FunctionPassContext& context, const Value& access, ElementAccessPlan& plan)
{
    if (plan.form != ElementAccessForm::Array || plan.arrayStorage != ArrayStorage::Other)
        return;
    std::optional<ArrayStorage> allocated = arrayStorageOfNewHoles(context.function.at(context.analyses.identityOf(access.objectOperand())));
    if (!allocated)
        return;
    plan.arrayStorage = *allocated;
    plan.testsArrayStorage = true;
    plan.level = KnowledgeLevel::Expected;
}

// The test is placed only where the comparison says how it will come out: an append by index, `a[n++] = x`, fails it every time.
static void expectPositionInsideLength(const TestSiteReporter& reporter, ArrayUses& arrays, const Value& use, ElementAccessPlan& plan)
{
    if (plan.form != ElementAccessForm::Array || !hasArrayStorageForm(plan.arrayStorage) || !Value::opcodesWithIsInBounds.contains(plan.operation))
        return;
    if (!keyTypeAfterTests(reporter, use, plan).isInteger())
        return;
    // A store that converts the storage is no store of one element.
    if (OperandPosition::value(plan.operation) && !canStoreWithoutConversion(plan.arrayStorage, reporter.typeOf(use.valueOperand())))
        return;
    plan.testsInBounds = arrays.isComparedWithLength(use.objectOperand(), use.propertyOperand(), reporter.point().block);
}

static PresenceQuery presenceQueryOf(ValueID access, BlockID block, const Value& use, const ElementAccessPlan& plan, const Type& receiver, const Type& key)
{
    return {
        .access = access,
        .block = block,
        .receiver = use.objectOperand(),
        .kind = *elementAccessOf(use.opcode),
        .form = plan.form,
        .arrayStorage = plan.arrayStorage,
        .typedArrayType = plan.typedArrayType,
        .keyIsInteger = key.isInteger(),
        .isDense = isDenseArray(receiver),
    };
}

// Where nothing asks whether the element is there, an absent element leaves typed code: the access behind the test has no arm for it.
static bool expectPresentElement(const TestSiteReporter& reporter, const Value& use, ElementAccessPlan& plan)
{
    if (plan.form != ElementAccessForm::Array && plan.form != ElementAccessForm::TypedArray)
        return false;

    PresenceQuery query = presenceQueryOf(reporter.use(), reporter.point().block, use, plan, reporter.typeOf(use.objectOperand()), keyTypeAfterTests(reporter, use, plan));
    std::optional<PresenceTest> test = choosePresenceTest(reporter.context(), query);
    if (!test)
        return false;

    switch (*test) {
    case PresenceTest::InBounds:
        plan.testsInBounds = true;
        return true;
    case PresenceTest::InBoundsWithHolesAsNaN:
        plan.testsInBounds = true;
        plan.readsHoleAsNaN = true;
        return true;
    case PresenceTest::ElementPresent:
        // An own element that is present answers whatever the prototypes hold, and a key that is no index fails the test.
        plan.testsPresence = true;
        plan.testsInBounds = false;
        plan.requiredAspects = { };
        return true;
    case PresenceTest::InsideView:
        plan.testsPresence = true;
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void countPresenceTest(const TestSiteReporter& reporter)
{
    if (reporter.didReport())
        reporter.context().build.statistics().count(StatisticID::ElementPresenceTestsPlaced);
}

static bool hasTestedPosition(const Value& access)
{
    if (access.opcode == Opcode::GetArrayElement)
        return access.isPresent() || access.isInBounds();
    if (access.opcode == Opcode::PutArrayElement)
        return access.isInBounds();
    return access.isInsideView();
}

// Flow proves a kind of storage only for what a literal or a step over entries has allocated, which is storage of the array's own.
static void reportBoundsTestOfResolvedStore(TestSiteReporter& reporter, ArrayUses& arrays, const Value& use)
{
    if (!hasArrayStorageForm(use.arrayStorage()) || proofsNeededFromGuard(reporter.program(), reporter.function(), use).contains(Proof::ArrayStorage))
        return;
    const Type& positionType = reporter.typeOf(use.positionOperand());
    if (!positionType.isInteger() || !canStoreWithoutConversion(use.arrayStorage(), reporter.typeOf(use.valueOperand())))
        return;
    if (!arrays.isComparedWithLength(use.arrayOperand(), use.positionOperand(), reporter.point().block))
        return;

    Effects effects = declaredBoundOf(use.opcode);
    refineElementEffects(use.opcode, positionType, ElementPosition::InsideLength, effects);
    reporter.report({ makeBoundsTest(use.arrayOperand(), use.positionOperand()) }, effects);
}

static void reportPresenceTestOfResolvedAccess(TestSiteReporter& reporter, ArrayUses& arrays, const Value& use)
{
    if (hasTestedPosition(use))
        return;
    if (use.opcode == Opcode::PutArrayElement) {
        reportBoundsTestOfResolvedStore(reporter, arrays, use);
        return;
    }

    bool isArrayRead = use.opcode == Opcode::GetArrayElement;
    ValueID receiver = isArrayRead ? use.arrayOperand() : use.viewOperand();
    ValueID position = use.positionOperand();
    const Type& positionType = reporter.typeOf(position);
    std::optional<PresenceTest> chosen = choosePresenceTest(reporter.context(), PresenceQuery {
        .access = reporter.use(),
        .block = reporter.point().block,
        .receiver = receiver,
        .kind = use.opcode == Opcode::PutTypedArrayElement ? Access::Store : Access::Read,
        .form = isArrayRead ? ElementAccessForm::Array : ElementAccessForm::TypedArray,
        .arrayStorage = isArrayRead ? use.arrayStorage() : ArrayStorage::Other,
        .typedArrayType = isArrayRead ? NotTypedArray : use.typedArrayType(),
        .keyIsInteger = positionType.isInteger(),
        .isDense = isArrayRead && isDenseArray(reporter.typeOf(receiver)),
    });
    if (!chosen)
        return;

    ElementAccessTests tests;
    switch (*chosen) {
    case PresenceTest::InBounds:
    case PresenceTest::InBoundsWithHolesAsNaN:
        tests.append(makeBoundsTest(receiver, position));
        break;
    case PresenceTest::ElementPresent: {
        // A test is reported without a guard. Where the read has the kind of storage from its own guard, the test of the kind is asked for again, and the proof that still holds answers.
        Value test = makePresenceTest(receiver, position, use.arrayStorage(), use.keyIsIntegral());
        if (proofsNeededFromGuard(reporter.program(), reporter.function(), test).contains(Proof::ArrayStorage))
            tests.append(makeArrayStorageTest(receiver, use.arrayStorage(), arrays.isStoredInto(receiver)));
        tests.append(WTF::move(test));
        break;
    }
    case PresenceTest::InsideView:
        tests.append(makeViewBoundsTest(receiver, position, use.typedArrayType(), use.keyIsIntegral()));
        break;
    }

    Effects effects = declaredBoundOf(use.opcode);
    if (isArrayRead)
        refineElementEffects(use.opcode, positionType, ElementPosition::InsideLength, effects);
    reporter.report(tests.span(), effects);
    countPresenceTest(reporter);
}

void reportElementAccessTests(TestSiteReporter& reporter)
{
    ArrayUses arrays(reporter.context());
    reporter.forEachCandidate([&](const Value& use) {
        if (resolvedAccessesWithPresenceTest.contains(use.opcode)) {
            reportPresenceTestOfResolvedAccess(reporter, arrays, use);
            return;
        }
        if (!elementAccessOf(use.opcode))
            return;

        ProgramPoint point = reporter.point();
        std::optional<ElementAccessSite> site = planElementAccessSite(reporter.context(), ValueSite { point.block, point.index, reporter.use() });
        if (!site)
            return;
        expectStorageOfNewArrayOfHoles(reporter.context(), use, site->plan);
        expectPositionInsideLength(reporter, arrays, use, site->plan);
        bool expectsPresentElement = expectPresentElement(reporter, use, site->plan);
        if (!hasGuardedForm(site->plan))
            return;

        std::optional<ElementAccessTests> tests = createTests(reporter, arrays, use, site->plan);
        if (!tests)
            return;
        reporter.report(tests->span(), effectsAfterTests(reporter, use, site->plan));
        if (expectsPresentElement)
            countPresenceTest(reporter);
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
        plan.testsInBounds = false;
        return;
    }
    if (test.opcode == Opcode::IsElementPresent) {
        plan.isPresent = true;
        plan.testsPresence = false;
        return;
    }
    if (test.opcode == Opcode::IsInsideView) {
        plan.isInsideView = true;
        plan.testsPresence = false;
        return;
    }
    RELEASE_ASSERT(test.opcode == Opcode::TypeTest);
}

static void markPositionAsTested(TestSiteRewriter& rewriter)
{
    ValueID guard = rewriter.guard();
    RELEASE_ASSERT(guard);
    if (ValueID earlier = rewriter.useValue().guardOperand())
        guard = rewriter.insertBeforeUse(IRBuilder::make(Opcode::AllGuards, { earlier, guard }));

    Value& use = rewriter.useValue();
    use.setGuardOperand(guard);
    for (const Value& test : rewriter.tests()) {
        if (test.opcode == Opcode::IsElementPresent)
            use.setIsPresent(true);
        else if (test.opcode == Opcode::IsInBounds) {
            use.setIsInBounds(true);
            if (Value::opcodesWithReadsHoleAsNaN.contains(use.opcode))
                use.setReadsHoleAsNaN(!isDenseArray(rewriter.program().typeOf(rewriter.function(), use.arrayOperand())));
        } else if (test.opcode == Opcode::IsInsideView)
            use.setIsInsideView(true);
        else
            RELEASE_ASSERT(test.opcode == Opcode::HasArrayStorage);
    }
}

// A comparison with the length also puts the test of the bounds, so the test alone does not say what was expected of a hole.
static bool readsHoleAsNaNBehindTests(TestSiteRewriter& rewriter, const ElementAccessPlan& plan)
{
    if (plan.operation != Opcode::GetArrayElement || !plan.isInBounds)
        return false;
    const Value& use = rewriter.useValue();
    const Program& program = rewriter.program();
    const Function& function = rewriter.function();
    PresenceQuery query = presenceQueryOf(rewriter.use(), rewriter.point().block, use, plan, program.typeOf(function, use.objectOperand()), program.typeOf(function, use.propertyOperand()));
    return choosePresenceTest(rewriter.context(), query) == PresenceTest::InBoundsWithHolesAsNaN;
}

// A test of the position is no test of a type: the compilers of typed languages make it too.
static bool testsTypeOfOperand(const Value& test)
{
    return provesOf(test.opcode) != Proof::InBounds;
}

void rewriteTestedElementAccess(TestSiteRewriter& rewriter)
{
    if (resolvedAccessesWithPresenceTest.contains(rewriter.useValue().opcode)) {
        markPositionAsTested(rewriter);
        return;
    }

    ProgramPoint point = rewriter.point();
    std::optional<ElementAccessSite> site = planElementAccessSite(rewriter.context(), ValueSite { point.block, point.index, rewriter.use() });
    RELEASE_ASSERT(site);
    // A character read has no operation of its own before selection. Where the access behind its tests asks for more than they found, it stays the access by key, which is right whatever they have found.
    ASSERT(site->plan.isResolved() || site->plan.form == ElementAccessForm::String);
    if (!site->plan.isResolved())
        return;

    for (const Value& test : rewriter.tests()) {
        RELEASE_ASSERT(rewriter.guard() || provesForever(provesOf(test.opcode)));
        applyPassedTest(site->plan, test);
    }
    // Sites that share the test of the class share the value that passed it, which has the expected type of one of them. A storage that this site did not test is not relied on.
    if (site->plan.testsArrayStorage) {
        site->plan.arrayStorage = ArrayStorage::Other;
        site->plan.testsArrayStorage = false;
        site->plan.isInBounds = false;
        site->plan.isPresent = false;
    }
    // The plan is made again here and knows nothing of what was expected of the position when the tests were reported.
    if (site->plan.isPresent)
        site->plan.requiredAspects = { };
    site->plan.readsHoleAsNaN = readsHoleAsNaNBehindTests(rewriter, site->plan);
    ASSERT(site->plan.needsNoTest());
    if (!site->plan.needsNoTest())
        return;
    site->plan.level = std::ranges::any_of(rewriter.tests(), testsTypeOfOperand) ? KnowledgeLevel::Expected : KnowledgeLevel::Proven;

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
