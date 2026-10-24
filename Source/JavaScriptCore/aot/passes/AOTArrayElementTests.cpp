/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestSite.h"

#if ENABLE(AOT)

#include "AOTArrayCapacities.h"
#include "AOTArrayStorageForms.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTypes.h"
#include "AOTEdgeResults.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTFunctionAnalyses.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTIntegerRelations.h"
#include "AOTPropertyAccessElements.h"
#include "AOTResolveAccesses.h"
#include "AOTTypedArrayKinds.h"
#include <algorithm>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(OperandIndex::IsElementPresent::array == testedOperandIndex && OperandIndex::IsInsideView::view == testedOperandIndex);

using ElementAccessTests = Vector<Value, 6>;

static constexpr unsigned maxValuesFollowed = 8;

// ResolveAccesses has rewritten these: the types of their operands needed no test. Whether the position holds an element is still open.
static constexpr OpcodeSet resolvedAccessesWithPresenceTest { Opcode::GetArrayElement, Opcode::PutArrayElement, Opcode::GetTypedArrayElement, Opcode::PutTypedArrayElement };

// SinkAllocations takes such an array apart by its accesses, which stay as they are. The result of `x.map(f)` is an allocation too, and its reads get the test.
static constexpr OpcodeSet localArrayAllocations { Opcode::NewArray, Opcode::NewArrayLike, Opcode::NewArrayWithCapacity, Opcode::NewContiguousArray, Opcode::NewDoubleArray, Opcode::CreateRest };

static constexpr OpcodeSet equalityComparisons { Opcode::Eq, Opcode::StrictEq };

// Each applies ToNumeric to an operand that is a primitive.
static constexpr OpcodeSet operatorsOnNumericOperands {
    Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Mod, Opcode::Pow,
    Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::LShift, Opcode::RShift, Opcode::URShift,
    Opcode::Negate, Opcode::ToNumeric, Opcode::Increment, Opcode::Decrement, Opcode::BitNot,
    Opcode::ToNumber, Opcode::ToInt32, Opcode::ToUint32,
    Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq,
};

namespace {

// The test in front of an element access that has no arm for an absent element: where it fails, the access is made by the generic copy.
enum class PresenceTest : uint8_t { InBounds, InBoundsWithHolesAsNaN, ElementPresent, InsideView };

struct PresenceQuery {
    ValueID access;
    BlockID block;
    ValueID receiver;
    Access kind { Access::Read };
    ElementAccessForm form { ElementAccessForm::ByKey };
    ArrayStorage arrayStorage { ArrayStorage::Other };
    TypedArrayType typedArrayType { NotTypedArray };
    bool keyIsInteger { false };
    bool isDense { false };
};

// What the function itself does with an array says what to expect of it: a position that it compares with the length lies inside, and an array that it stores into has storage of its own.
struct ArrayUses {
    const Program& program;
    const Function& function;
    FunctionAnalyses& analyses;
    bool isCollected { false };
    HashMap<ValueID, Vector<ValueID, 2>> lengthReads { };
    Vector<std::pair<ValueID, BlockID>, 2> creations { };
    HashSet<ValueID> storedArrays { };
    std::optional<IntegerRelations> relations { };

    void collectOnce()
    {
        if (std::exchange(isCollected, true))
            return;
        auto addLength = [&](ValueID array, ValueID length) {
            lengthReads.ensure(analyses.identityOf(array), [] { return Vector<ValueID, 2>(); }).iterator->value.append(length);
        };
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            for (ValueID id : function.at(block).values) {
                const Value& value = function.at(id);
                std::optional<MeasuredStorage> measured = storageMeasuredBy(program, value);
                if (measured && measured->heap == AbstractHeap::ArrayStorage && measured->owner)
                    addLength(measured->owner, id);
                if (ValueID length = lengthAtCreation(value)) {
                    addLength(id, length);
                    creations.append({ length, block });
                }
                if (value.opcode == Opcode::PutArrayElement)
                    storedArrays.add(analyses.identityOf(value.arrayOperand()));
                else if (std::optional<Access> access = elementAccessOf(value.opcode); access == Access::Store || access == Access::Define)
                    storedArrays.add(analyses.identityOf(value.objectOperand()));
            }
        }
    }

    // An array of holes has its length from the start.
    ValueID lengthAtCreation(const Value& value) const
    {
        if (value.opcode == Opcode::NewArrayLike || value.opcode == Opcode::NewContiguousArray || value.opcode == Opcode::NewDoubleArray)
            return value.lengthOperand();
        bool isCallWithLength = value.opcode == Opcode::CallBuiltin && inlineImplementationOf(program, value) == InlineBuiltin::ArrayWithLength && !value.argumentsOperands().empty();
        return isCallWithLength ? value.argumentsOperands()[0] : ValueID();
    }

    bool isStoredInto(ValueID array)
    {
        collectOnce();
        return storedArrays.contains(analyses.identityOf(array));
    }

    bool isComparedWithLength(ValueID array, ValueID position, BlockID at)
    {
        using Answer = IntegerRelations::Answer;
        collectOnce();
        auto lengths = lengthReads.find(analyses.identityOf(array));
        if (lengths == lengthReads.end())
            return false;
        if (!relations) {
            relations.emplace(analyses);
            // Any other number has thrown a RangeError.
            for (auto [length, block] : creations)
                relations->assumeRange(length, Range::uint32(), block);
        }
        return relations->isAtLeast(position, 0, at) == Answer::Proven && std::ranges::any_of(lengths->value, [&](ValueID length) {
            return relations->isAtMost(position, length, -1, at) == Answer::Proven;
        });
    }
};

} // anonymous namespace

static bool asksWhetherValueIsAbsent(const Program& program, const Function& function, const Value& user, uint32_t operandIndex)
{
    if (user.opcode == Opcode::ToBoolean || user.opcode == Opcode::TypeOf)
        return true;
    // `??` and `?.` are tests for undefined and null. A test for a number is what a check of the element has become, and it folds once the read is typed.
    if (user.opcode == Opcode::TypeTest)
        return program.at(user.testedType()).isUndefinedOrNull();
    // A comparison with what is never undefined or null looks for a value.
    return equalityComparisons.contains(user.opcode) && program.typeOf(function, user.operand(1 - operandIndex)).mayBeUndefinedOrNull();
}

// A wrong "no" costs speed only: the failed test takes the generic copy.
static bool isTestedForAbsence(FunctionAnalyses& analyses, ValueID read)
{
    const Program& program = analyses.program();
    const Function& function = analyses.function();
    const UseLists& useLists = analyses.useLists();
    Vector<ValueID, maxValuesFollowed> followed { read };
    auto follow = [&](ValueID value) {
        if (followed.size() < maxValuesFollowed && !followed.contains(value) && program.typeOf(function, value).mayBeUndefinedOrNull())
            followed.append(value);
    };
    // What passes a test of its type goes on under the narrower type, as behind a cast. What fails it has left for other code.
    auto followWhatPassed = [&](ValueID test, uint32_t operandIndex) {
        for (const Use& use : useLists.usesOf(test)) {
            const Terminator& terminator = function.at(use.block).terminator;
            if (use.kind != Use::Kind::TerminatorOperand || !terminator.isConditionalBranch())
                continue;
            const Target& taken = terminator.taken();
            uint32_t firstResult = firstEdgeResultParameter(function, use.block, 0);
            for (uint32_t resultIndex = 0; resultIndex < taken.proofs.size(); ++resultIndex) {
                if (taken.proofs[resultIndex] == operandIndex)
                    follow(function.at(taken.block).parameters[firstResult + resultIndex]);
            }
        }
    };
    for (size_t index = 0; index < followed.size(); ++index) {
        for (const Use& use : useLists.usesOf(followed[index])) {
            switch (use.kind) {
            case Use::Kind::Operand: {
                const Value& user = function.at(use.user);
                if (asksWhetherValueIsAbsent(program, function, user, use.operandIndex))
                    return true;
                if (isCast(user.opcode) && !use.operandIndex)
                    follow(use.user);
                else if (user.opcode == Opcode::TypeTest)
                    followWhatPassed(use.user, use.operandIndex);
                break;
            }
            // The cases of a switch are numbers or strings: what is absent goes where every other value goes.
            case Use::Kind::TerminatorOperand:
                if (function.at(use.block).terminator.isConditionalBranch())
                    return true;
                break;
            case Use::Kind::EdgeArgument:
            case Use::Kind::HandlerArgument:
                follow(function.at(targetOf(function, use.edge()).block).parameters[use.operandIndex]);
                break;
            case Use::Kind::BlockSubject:
                break;
            }
        }
    }
    return false;
}

// Undefined converts to NaN, and the hole of Double storage is a NaN.
static bool everyUseConvertsToNumber(const Program& program, FunctionAnalyses& analyses, ValueID read)
{
    const Function& function = analyses.function();
    bool isUsed = false;
    Vector<ValueID, 4> worklist { read };
    while (!worklist.isEmpty()) {
        for (const Use& use : analyses.useLists().usesOf(worklist.takeLast())) {
            if (use.kind != Use::Kind::Operand)
                return false;
            // A cast that throws tells undefined from NaN.
            const Value& user = function.at(use.user);
            if (isCastWithoutTest(user.opcode) && !use.operandIndex) {
                worklist.append(use.user);
                continue;
            }
            // Beside a string the sum would hold the text "undefined".
            if (!operatorsOnNumericOperands.contains(user.opcode) && (user.opcode != Opcode::Add || !program.typeOf(function, user.operand(1 - use.operandIndex)).isNumber()))
                return false;
            isUsed = true;
        }
    }
    return isUsed;
}

static bool hasFormWithoutAbsentElement(const Program& program, const Function& function, const PresenceQuery& query)
{
    // A store outside a typed array does nothing, and no program relies on that.
    if (query.form == ElementAccessForm::TypedArray)
        return (query.kind == Access::Read || query.kind == Access::Store) && hasInlineElementAccess(query.typedArrayType);
    if (query.form != ElementAccessForm::Array || query.kind != Access::Read || !hasArrayStorageForm(query.arrayStorage))
        return false;
    // The first elements of such a result are delivered without the array, each as the element or undefined.
    const Value& array = function.at(sameObjectThroughCasts(function, query.receiver));
    return !localArrayAllocations.contains(array.opcode) && (array.opcode != Opcode::CallBuiltin || !program.builtins().at(array.overload()).code.prefixForm);
}

// That a hole reads as undefined is a fact about the prototypes, whose test pays only in front of a loop.
static bool canReadHolesAsNaN(FunctionPassContext& context, const PresenceQuery& query)
{
    // The read keeps its type less undefined, which has to admit the NaN. An operand of any other type is tested by its operator in the same round, and a test is no conversion.
    const Type& type = context.program.typeOf(context.function, query.access);
    if (query.arrayStorage != ArrayStorage::Double || !query.keyIsInteger || !type.isNumberOrNullish() || !type.mayBeNaN())
        return false;
    const NaturalLoops& loops = context.analyses.naturalLoops();
    const NaturalLoop* loop = loops.innerMostLoopOf(query.block);
    bool isInvariantInLoop = loop && std::ranges::any_of(CastChain(context.function, query.receiver), [&](ValueID candidate) {
        ValueLocation location = context.analyses.useLists().locationOf(candidate);
        return location.placement != ValuePlacement::Detached && !loops.belongsTo(location.block, *loop);
    });
    return isInvariantInLoop && everyUseConvertsToNumber(context.program, context.analyses, query.access);
}

// Nothing where the access keeps its arm for an absent element.
static std::optional<PresenceTest> choosePresenceTest(FunctionPassContext& context, const PresenceQuery& query)
{
    if (!hasFormWithoutAbsentElement(context.program, context.function, query) || context.analyses.blockFrequencies().classOf(query.block) == BlockFrequency::Rare)
        return std::nullopt;
    if (query.kind == Access::Read && isTestedForAbsence(context.analyses, query.access))
        return std::nullopt;
    if (query.form == ElementAccessForm::TypedArray)
        return PresenceTest::InsideView;
    if (query.isDense && query.keyIsInteger)
        return PresenceTest::InBounds;
    return canReadHolesAsNaN(context, query) ? PresenceTest::InBoundsWithHolesAsNaN : PresenceTest::ElementPresent;
}

static bool hasGuardedForm(const ElementAccessPlan& plan)
{
    if (plan.isCharacterReadAfterTests())
        return true;
    if (!plan.isResolved() || plan.needsNoTest())
        return false;
    return plan.form != ElementAccessForm::Array || plan.arrayStorage == ArrayStorage::Other || hasArrayStorageForm(plan.arrayStorage);
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
    bool mayBeIndex = !reporter.function().at(key).expected() && reporter.typeOf(key).overlaps(reporter.types().at(TypeTable::int32), reporter.program());
    return mayBeIndex ? TypeTable::int32 : TypeID();
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
        tests.append(makeTypeTest(receiver, reporter.types().intern(typeOfInstances(reporter.program().builtins(), plan.builtinClass, *expected))));
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
    TypeID tested = plan.testsKeyIsNumber ? testedTypeOfKey(reporter, key) : TypeID();
    return tested ? reporter.typeOf(key).intersectWith(reporter.types().at(tested), reporter.types()) : reporter.typeOf(key);
}

// An array of holes is allocated with the kind that its allocation names, and has its length from the start: a store into it is a store inside the length, which the forms with a kind have inline.
static void expectStorageOfNewArrayOfHoles(FunctionPassContext& context, const Value& access, ElementAccessPlan& plan)
{
    if (plan.form != ElementAccessForm::Array || plan.arrayStorage != ArrayStorage::Other)
        return;
    if (std::optional<ArrayStorage> allocated = arrayStorageOfNewHoles(context.function.at(context.analyses.identityOf(access.objectOperand())))) {
        plan.arrayStorage = *allocated;
        plan.testsArrayStorage = true;
        plan.level = KnowledgeLevel::Expected;
    }
}

// The test is placed only where the comparison says how it will come out: an append by index, `a[n++] = x`, fails it every time.
static void expectPositionInsideLength(const TestSiteReporter& reporter, ArrayUses& arrays, const Value& use, ElementAccessPlan& plan)
{
    if (plan.form != ElementAccessForm::Array || !hasArrayStorageForm(plan.arrayStorage) || !Value::opcodesWithIsInBounds.contains(plan.operation) || !keyTypeAfterTests(reporter, use, plan).isInteger())
        return;
    // A store that converts the storage is no store of one element.
    if (OperandPosition::value(plan.operation) && !canStoreWithoutConversion(plan.arrayStorage, reporter.typeOf(use.valueOperand())))
        return;
    plan.testsInBounds = arrays.isComparedWithLength(use.objectOperand(), use.propertyOperand(), reporter.point().block);
}

static PresenceQuery presenceQueryOf(ValueID access, BlockID block, const Value& use, const ElementAccessPlan& plan, const Type& receiver, const Type& key)
{
    return { access, block, use.objectOperand(), *elementAccessOf(use.opcode), plan.form, plan.arrayStorage, plan.typedArrayType, key.isInteger(), isDenseArray(receiver) };
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
    case PresenceTest::InBoundsWithHolesAsNaN:
        plan.readsHoleAsNaN = true;
        [[fallthrough]];
    case PresenceTest::InBounds:
        plan.testsInBounds = true;
        return true;
    case PresenceTest::ElementPresent:
        // An own element that is present answers whatever the prototypes hold, and a key that is no index fails the test.
        plan.testsInBounds = false;
        plan.requiredAspects = { };
        [[fallthrough]];
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
    bool isArrayRead = use.opcode == Opcode::GetArrayElement;
    if (isArrayRead ? use.isPresent() || use.isInBounds() : use.opcode == Opcode::PutArrayElement ? use.isInBounds() : use.isInsideView())
        return;
    if (use.opcode == Opcode::PutArrayElement)
        return reportBoundsTestOfResolvedStore(reporter, arrays, use);

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
    ArrayUses arrays { reporter.program(), reporter.function(), reporter.analyses() };
    reporter.forEachCandidate([&](const Value& use) {
        if (resolvedAccessesWithPresenceTest.contains(use.opcode))
            return reportPresenceTestOfResolvedAccess(reporter, arrays, use);
        ProgramPoint point = reporter.point();
        std::optional<ElementAccessSite> site = elementAccessOf(use.opcode) ? planElementAccessSite(reporter.context(), ValueSite { point.block, point.index, reporter.use() }) : std::nullopt;
        if (!site)
            return;
        ElementAccessPlan& plan = site->plan;
        expectStorageOfNewArrayOfHoles(reporter.context(), use, plan);
        expectPositionInsideLength(reporter, arrays, use, plan);
        bool expectsPresentElement = expectPresentElement(reporter, use, plan);
        std::optional<ElementAccessTests> tests = hasGuardedForm(plan) ? createTests(reporter, arrays, use, plan) : std::nullopt;
        if (!tests)
            return;
        reporter.report(tests->span(), plan.isCharacterReadAfterTests() ? declaredBoundOf(Opcode::StringRangeCharacterOrUndefined) : effectsAfterRewrite(plan, keyTypeAfterTests(reporter, use, plan)));
        if (expectsPresentElement)
            countPresenceTest(reporter);
    });
}

static void applyPassedTest(ElementAccessPlan& plan, const Value& test)
{
    if (test.opcode == Opcode::HasArrayStorage) {
        plan.arrayStorage = test.arrayStorage();
        plan.testsArrayStorage = false;
    } else if (test.opcode == Opcode::IsPristine)
        plan.requiredAspects = plan.requiredAspects - test.aspects();
    else if (test.opcode == Opcode::IsInBounds) {
        plan.isInBounds = true;
        plan.testsInBounds = false;
    } else if (test.opcode == Opcode::IsElementPresent || test.opcode == Opcode::IsInsideView) {
        (test.opcode == Opcode::IsElementPresent ? plan.isPresent : plan.isInsideView) = true;
        plan.testsPresence = false;
    } else
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

void rewriteTestedElementAccess(TestSiteRewriter& rewriter)
{
    if (resolvedAccessesWithPresenceTest.contains(rewriter.useValue().opcode))
        return markPositionAsTested(rewriter);

    ProgramPoint point = rewriter.point();
    std::optional<ElementAccessSite> site = planElementAccessSite(rewriter.context(), ValueSite { point.block, point.index, rewriter.use() });
    RELEASE_ASSERT(site);
    ElementAccessPlan& plan = site->plan;
    // A character read has no operation of its own before selection. Where the access behind its tests asks for more than they found, it stays the access by key, which is right whatever they have found.
    ASSERT(plan.isResolved() || plan.form == ElementAccessForm::String);
    if (!plan.isResolved())
        return;

    for (const Value& test : rewriter.tests()) {
        RELEASE_ASSERT(rewriter.guard() || provesForever(provesOf(test.opcode)));
        applyPassedTest(plan, test);
    }
    // Sites that share the test of the class share the value that passed it, which has the expected type of one of them. A storage that this site did not test is not relied on.
    if (plan.testsArrayStorage) {
        plan.arrayStorage = ArrayStorage::Other;
        plan.testsArrayStorage = false;
        plan.isInBounds = false;
        plan.isPresent = false;
    }
    // The plan is made again here and knows nothing of what was expected of the position when the tests were reported.
    if (plan.isPresent)
        plan.requiredAspects = { };
    plan.readsHoleAsNaN = readsHoleAsNaNBehindTests(rewriter, plan);
    ASSERT(plan.needsNoTest());
    if (!plan.needsNoTest())
        return;
    // A test of the position is no test of a type: the compilers of typed languages make it too.
    bool testsTypeOfOperand = std::ranges::any_of(rewriter.tests(), [](const Value& test) {
        return provesOf(test.opcode) != Proof::InBounds;
    });
    plan.level = testsTypeOfOperand ? KnowledgeLevel::Expected : KnowledgeLevel::Proven;

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
