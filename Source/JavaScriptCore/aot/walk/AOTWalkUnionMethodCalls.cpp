/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTPlan.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <span>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr size_t mostArmsInUnionMethodCalls = 8;
static constexpr size_t maxNonLeafArms = 4;

constinit const OpcodeSet opcodesThatCallMethodsBehindTestsOfShapeTakes { Opcode::PropGet };

static ValueID findCallOfMethodReadInUnionMethodCalls(const BodyIndex& index, const BodyValue& read)
{
    ValueID call = index.onlyUsingValue(read.id());
    ValueID callee = read.id();
    while (call && inputPassThroughOpcodes.contains(index.opcodeOf(call))) {
        callee = call;
        call = index.onlyUsingValue(call);
    }
    if (!call || !index.isInBlock(call, index.blockOf(read.id())))
        return { };
    if (index.opcodeOf(call) == Opcode::Construct) {
        auto written = index.value(call).as<Opcode::Construct>();
        return written.calleeOperand() == callee && !written.newTargetOperand() ? call : ValueID();
    }
    if (index.opcodeOf(call) != Opcode::Call || callee != read.id())
        return { };
    auto written = index.value(call).as<Opcode::Call>();
    return written.calleeOperand() == read.id() && written.thisValueOperand() == read.as<Opcode::PropGet>().objectOperand() ? call : ValueID();
}

static bool walkBehindTestsOfShapeInUnionMethodCalls(Walker& walker, const BodyValue& read, ValueID call, bool takesWhatIsOnlyExpected = false, bool standsForGuess = false)
{
    if (walker.standsInArmOfRowOfShapeTests() || !walker.has(Mechanism::DirectCalls) || !walker.has(Mechanism::TypedOperations))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    auto lookup = read.as<Opcode::PropGet>();
    if (lookup.isFreeName() || !lookup.objectOperand())
        return false;
    BodyBlock block = index.block(index.blockOf(read.id()));
    uint32_t first = block.firstValue().index();
    uint32_t begin = read.id().index() - first;
    uint32_t end = (call ? call : read.id()).index() - first + 1;
    Reference receiver = walker.referenceOf(lookup.objectOperand());
    if (end > walker.endOfStretchBeingWalked() || !receiver || !receiver.isValue())
        return false;

    TypeArena& types = walker.types();
    const Program& program = walker.program();
    const Plan& plan = walker.plan();
    TypeID proven = walker.typeOf(receiver);
    bool isOnlyExpected = false;
    if (takesWhatIsOnlyExpected) {
        if (!proven || !types.isUninformative(proven) || liesInThread(lookup.key()))
            return false;
        TypeID expected = objectsExpectedOfOperandOfQuestion(walker, lookup.objectOperand());
        Vector<LayoutID, mostArmsInUnionMethodCalls> holders;
        if (!expected || !walker.facts().layoutsByNames().appendHoldersAmongUnlessMoreThan(mostArmsInUnionMethodCalls, lookup.key(), types.listOf(types.at(expected).layouts), holders) || holders.size() < 2)
            return false;
        TypeID whoAnswer = types.forLayouts(holders.span());
        auto typeAsExpected = [&](ValueID input) {
            return input == lookup.objectOperand() ? whoAnswer : walker.typeOf(walker.referenceOf(input));
        };
        if (typedFormOf(walker, read, TypesOfInputs(typeAsExpected)) != TypedForm::None)
            return false;
        proven = whoAnswer;
        isOnlyExpected = true;
    }
    if (!proven || !types.at(proven).isJavaScriptValue() || (types.at(proven).primitives & ~Primitive::UndefinedOrNull) || !types.at(proven).hasOnlyLayouts() || holdsOneMember(types.at(proven).layouts))
        return false;

    bool mayBeNothing = types.at(proven).primitives;
    if (call && !isOnlyExpected && !mayBeNothing && findsOneMethodThroughLookups(walker, read, proven))
        return false;
    bool somethingIsLeft = isOnlyExpected;
    bool someAnswerByGetter = false;
    Vector<TypeID, mostArmsInUnionMethodCalls> arms;
    Vector<TypeID, mostArmsInUnionMethodCalls> armsThatFindNeitherGetterNorField;
    bool hasTwoLayouts = false;
    LayoutID classAboveBoth;
    if (!call) {
        size_t numberInUse = 0;
        for (LayoutID member : membersOf(types, types.at(proven))) {
            if (plan.isInUse(member) && ++numberInUse > mostArmsInUnionMethodCalls)
                return false;
        }
        hasTwoLayouts = standsForGuess && numberInUse == 2;
        bool someClassHasAccessor = walker.isInLoop() || hasTwoLayouts || std::ranges::any_of(membersOf(types, types.at(proven)), [&](LayoutID member) {
            return plan.isInUse(member) && walker.facts().linked().findMember(program, member, ClassSide::Prototype, lookup.key()).kind == MemberFound::Kind::Accessor;
        });
        if (!someClassHasAccessor)
            return false;
    }
    for (size_t place = 0; place < membersOf(types, types.at(proven)).size(); ++place) {
        LayoutID member = membersOf(types, types.at(proven))[place];
        if (!plan.isInUse(member))
            continue;
        if (!plan.slots().holdsCompiledObjects(member) || !isFoundByShapeTest(program.at(member)))
            return false;
        TypeID tested = types.forLayout(member);
        auto typeBehindTest = [&](ValueID input) {
            return input == lookup.objectOperand() ? tested : walker.typeOf(walker.referenceOf(input));
        };
        TypedForm form = typedFormOf(walker, read, TypesOfInputs(typeBehindTest));
        someAnswerByGetter |= form == TypedForm::BuiltAsCall;
        if (hasTwoLayouts) {
            LayoutID above = declaredLayoutOf(program, member) ? program.at(declaredLayoutOf(program, member)).base : LayoutID();
            hasTwoLayouts = form == TypedForm::Built && findField(program, member, lookup.key()) && program.at(member).kind == LayoutKind::Instance && above && (!classAboveBoth || above == classAboveBoth);
            classAboveBoth = above;
        }
        if (form == TypedForm::None) {
            if (!call)
                return false;
            somethingIsLeft = true;
        } else if (arms.size() + armsThatFindNeitherGetterNorField.size() == mostArmsInUnionMethodCalls)
            return false;
        else if (!call && form != TypedForm::BuiltAsCall && !findField(program, member, lookup.key()))
            armsThatFindNeitherGetterNorField.append(tested);
        else
            arms.append(tested);
    }
    arms.appendVector(armsThatFindNeitherGetterNorField);
    auto callsSmallLeaves = [&](PropertyKeyID key, ValueID called) {
        return index.opcodeOf(called) == Opcode::Call && std::ranges::all_of(arms, [&](TypeID arm) {
            MemberFound found = walker.facts().linked().findMember(program, singleLayoutOf(types, types.at(arm)), ClassSide::Prototype, key);
            return found.kind == MemberFound::Kind::Method && found.method && isSmallLeaf(walker, found.method, index.value(called).argumentsOperands().size());
        });
    };
    if (arms.size() > maxNonLeafArms && !(call && walker.isInLoop() && callsSmallLeaves(lookup.key(), call)))
        return false;
    auto mayTake = [&](const BodyValue& operation) {
        if (operation.opcode() == Opcode::PropGet && walker.referenceOf(operation.as<Opcode::PropGet>().objectOperand()) == receiver) {
            ValueID called = findCallOfMethodReadInUnionMethodCalls(index, operation);
            return !called || callsSmallLeaves(operation.as<Opcode::PropGet>().key(), called);
        }
        return operation.opcode() != Opcode::Call || std::ranges::none_of(operation.argumentsOperands(), [&](ValueID handed) {
            return handed && walker.referenceOf(handed) == receiver;
        });
    };
    if (arms.isEmpty() || (!call && !someAnswerByGetter && !walker.isInLoop() && !hasTwoLayouts))
        return false;
    if (call && walker.isInLoop())
        end = findArmStretchEnd(walker, index.value(call), end - 1, limitOfStretchOfArms(walker, block), receiver, scopedLambda<bool(const BodyValue&)>(mayTake));
    if (walker.shouldDropLastCase() && !somethingIsLeft && arms.size() > 1)
        arms.removeLast();

    uint32_t firstOfStretch = first + begin;
    uint32_t size = end - begin;
    Vector<ValueID, 4> names;
    for (uint32_t place = 0; place < size; ++place) {
        index.value(ValueID(firstOfStretch + place)).forEachOperand([&](ValueID operand, uint32_t) {
            if (operand.index() < firstOfStretch && walker.referenceOf(operand) == receiver && !names.contains(operand))
                names.append(operand);
        });
    }

    Origin origin = walker.originOf(read);
    Walker::StretchInArms stretch = walker.stretchInArms(block.id(), begin, end);
    Walker::ArmsThatMeet built;
    auto walkArm = [&] {
        for (uint32_t place = 0; place < size; ++place)
            walker.bind(ValueID(firstOfStretch + place), Reference());
        walker.walkStretch(block.id(), begin, end);
        walker.listArm(stretch, built);
    };
    auto narrowedBy = [&](Reference value, Reference test, bool isOnTakenEdge) {
        Draft narrowed(Opcode::Narrowed, origin);
        narrowed.setValueOperand(value);
        narrowed.setTestOperand(test);
        narrowed.setOperandIndex(OperandIndex::ShapeTest::object);
        narrowed.setIsOnTakenEdge(isOnTakenEdge);
        return walker.build(WTF::move(narrowed));
    };

    walker.count(call ? StatisticID::MethodCallsDispatchedByShape : StatisticID::PropertyReadsDispatchedByShape);
    if (isOnlyExpected) {
        walker.count(StatisticID::SpeculatedReceiversDispatchedByShape);
        walker.noteTestSeenInTries(receiver, read.id(), proven);
    }
    Reference left = receiver;
    if (mayBeNothing) {
        Draft check(Opcode::RequireObjectCoercible, origin);
        check.setValueOperand(receiver);
        check.setNullishAccess(NullishAccess::Read);
        if (!liesInThread(lookup.key())) {
            if (const PropertyKey& key = program.at(lookup.key()); key.kind == PropertyKeyKind::Text)
                check.setName(key.text);
        }
        left = walker.build(WTF::move(check));
        if (!left || walker.isUnreachable())
            return true;
        walker.state().noteCheckedName(receiver, left);
    }

    Reference asItEnteredRow = left;
    SetForScope standsInArm(walker.numberOfFramesAtRowOfShapeTests(), walker.depthOfCopies() + 1);
    BlockID meeting = walker.addBlock();
    BlockID closedArmOfAll = isOnlyExpected ? walker.addBlock(BlockFrequency::Rare) : BlockID();
    if (closedArmOfAll)
        walker.code().markRareBlockThatKeepsSites(closedArmOfAll);
    SetForScope failingLooksOfArmsGoThere(walker.closedArmOfRowOfShapeTests(), Walker::ClosedArmOfRowOfShapeTests { closedArmOfAll, read.id(), walker.depthOfCopies() });
    walker.recordSpeculation();
    for (size_t place = 0; place < arms.size() && !walker.isUnreachable(); ++place) {
        Draft test(Opcode::ShapeTest, origin);
        test.setObjectOperand(left);
        test.setLayouts(types.at(arms[place]).layouts);
        test.setRejectsMarkedObjects(isOnlyExpected);
        Reference passes = walker.build(WTF::move(test));
        if (!passes)
            return true;
        bool isLast = place + 1 == arms.size();
        BlockID passed = walker.addBlock();
        BlockID failed = isLast && closedArmOfAll ? closedArmOfAll : walker.addBlock(isLast ? BlockFrequency::Rare : BlockFrequency::Normal);
        walker.branch(passes, passed, failed);
        walker.continueIn(passed);
        if (Reference narrowed = narrowedBy(left, passes, true)) {
            walker.state().noteShape(narrowed, walker.typeOf(narrowed));
            Vector<Reference, 4> boundBefore;
            for (ValueID name : names) {
                boundBefore.append(walker.referenceBoundTo(name));
                walker.bind(name, narrowed);
            }
            walkArm();
            for (size_t name = 0; name < names.size(); ++name)
                walker.bind(names[name], boundBefore[name]);
        }
        if (walker.hasFailed())
            return true;
        walker.continueIn(failed);
        if (!isLast) {
            if (!isOnlyExpected)
                left = narrowedBy(left, passes, false);
            if (!left)
                return true;
        } else if (somethingIsLeft) {
            walker.closedArmOfRowOfShapeTests() = { };
            Walker::ClosedArm closedArm(walker, isOnlyExpected);
            walkArm();
        } else {
            TypeID layoutsOfArms = arms[0];
            for (TypeID arm : arms)
                layoutsOfArms = types.unionOf(layoutsOfArms, arm);
            walker.checkProof(asItEnteredRow, layoutsOfArms, TypeWithoutProof(TypeTable::anything), ProofSource::OtherOperation, origin);
            walker.endAsUnreachable(origin);
        }
    }
    if (!walker.hasFailed())
        walker.joinArms(stretch, built, meeting);
    return true;
}

GuessOpened testNameAfterShapeTests(Walker& walker, const BodyValue& question)
{
    if (walker.standsInArmOfRowOfShapeTests() || walker.isNestedTooDeeply() || (!walker.isInLoop() && walker.program().at(walker.unit().function).isModuleBody()))
        return GuessOpened::None;
    const BodyIndex& index = walker.currentBodyIndex();
    auto asked = question.as<Opcode::PropHas>();
    if (!asked.objectOperand())
        return GuessOpened::None;
    BodyBlock block = index.block(index.blockOf(question.id()));
    uint32_t begin = question.id().index() - block.firstValue().index();
    uint32_t end = begin + 1;
    Reference receiver = walker.referenceOf(asked.objectOperand());
    if (end > walker.endOfStretchBeingWalked() || !receiver || !receiver.isValue())
        return GuessOpened::None;

    TypeArena& types = walker.types();
    const Program& program = walker.program();
    const Plan& plan = walker.plan();
    TypeID proven = walker.typeOf(receiver);
    if (!proven || !types.at(proven).isJavaScriptValue() || (types.at(proven).primitives & ~Primitive::UndefinedOrNull) || !types.at(proven).hasOnlyLayouts() || holdsOneMember(types.at(proven).layouts))
        return GuessOpened::None;
    auto isAnsweredBy = [&](TypeID tested) {
        auto typeBehindTest = [&](ValueID input) {
            return input == asked.objectOperand() ? tested : walker.typeOf(walker.referenceOf(input));
        };
        return typedFormOf(walker, question, TypesOfInputs(typeBehindTest)) == TypedForm::Built;
    };
    static constexpr size_t mostLayouts = 8;
    Vector<LayoutID, mostLayouts> haveName;
    Vector<LayoutID, mostLayouts> lackName;
    bool mayBeNothing = types.at(proven).primitives;
    bool someLayoutIsLeft = false;
    for (size_t place = 0; place < membersOf(types, types.at(proven)).size(); ++place) {
        LayoutID member = membersOf(types, types.at(proven))[place];
        if (!plan.isInUse(member))
            continue;
        if (!plan.slots().holdsCompiledObjects(member) || !isFoundByShapeTest(program.at(member)) || haveName.size() + lackName.size() == mostLayouts)
            return GuessOpened::None;
        if (!isAnsweredBy(types.forLayout(member))) {
            someLayoutIsLeft = true;
            continue;
        }
        bool listsName = findField(program, member, asked.key()) || std::ranges::contains(walker.facts().linked().appendedKeysOf(member), asked.key());
        (listsName ? haveName : lackName).append(member);
    }
    Origin origin = walker.originOf(question);
    auto narrowedBy = [&](Reference value, Reference test, uint32_t operandIndex, bool isOnTakenEdge) {
        Draft narrowed(Opcode::Narrowed, origin);
        narrowed.setValueOperand(value);
        narrowed.setTestOperand(test);
        narrowed.setOperandIndex(operandIndex);
        narrowed.setIsOnTakenEdge(isOnTakenEdge);
        return walker.build(WTF::move(narrowed));
    };
    bool shapeDecides = !someLayoutIsLeft && !haveName.isEmpty() && !lackName.isEmpty() && areReceiversFilledAtAllReads(walker, types.forLayouts(haveName.span()), asked.key()) && isAnsweredBy(types.forLayouts(lackName.span()));
    if (shapeDecides) {
        walker.count(StatisticID::PropertyReadsDispatchedByShape);
        Reference object = receiver;
        if (mayBeNothing) {
            Draft isNothing(Opcode::TypeTest, origin);
            isNothing.setValueOperand(receiver);
            isNothing.setTestedType(TypeArena::fromPrimitives(Primitive::UndefinedOrNull));
            Reference nothing = walker.build(WTF::move(isNothing));
            if (!nothing || walker.isUnreachable())
                return GuessOpened::AsOneOperation;
            BlockID refused = walker.addBlock(BlockFrequency::Rare);
            BlockID goesOn = walker.addBlock();
            walker.branch(nothing, refused, goesOn);
            walker.continueIn(refused);
            Draft asWritten(Opcode::PropHas, origin);
            walker.chooseConservativeFields(asWritten);
            asWritten.setObjectOperand(receiver);
            asWritten.setKey(asked.key());
            walker.build(WTF::move(asWritten));
            walker.endAsUnreachable(origin);
            walker.continueIn(goesOn);
            object = narrowedBy(receiver, nothing, OperandIndex::TypeTest::value, false);
            if (!object || walker.isUnreachable())
                return GuessOpened::AsOneOperation;
        }
        Draft test(Opcode::ShapeTest, origin);
        test.setObjectOperand(object);
        test.setLayouts(types.at(types.forLayouts(haveName.span())).layouts);
        test.inputValueGiven = question.id();
        walker.bind(question.id(), walker.build(WTF::move(test)));
        return GuessOpened::AsOneOperation;
    }

    bool somethingIsLeft = mayBeNothing || someLayoutIsLeft;
    Vector<TypeID, mostLayouts> arms;
    for (std::span<const LayoutID> answerAlike : { std::span<const LayoutID>(haveName.span()), std::span<const LayoutID>(lackName.span()) }) {
        if (answerAlike.empty())
            continue;
        if (TypeID together = types.forLayouts(answerAlike); isAnsweredBy(together)) {
            arms.append(together);
            continue;
        }
        for (LayoutID layout : answerAlike)
            arms.append(types.forLayout(layout));
    }
    if (arms.isEmpty() || arms.size() > mostArmsInUnionMethodCalls)
        return GuessOpened::None;
    if (walker.shouldDropLastCase() && !somethingIsLeft && arms.size() > 1)
        arms.removeLast();

    Walker::StretchInArms stretch = walker.stretchInArms(block.id(), begin, end);
    Walker::ArmsThatMeet built;
    auto walkArm = [&] {
        walker.bind(question.id(), Reference());
        walker.walkStretch(block.id(), begin, end);
        walker.listArm(stretch, built);
    };
    walker.count(StatisticID::PropertyReadsDispatchedByShape);
    SetForScope standsInArm(walker.numberOfFramesAtRowOfShapeTests(), walker.depthOfCopies() + 1);
    BlockID meeting = walker.addBlock();
    SetForScope failingLooksOfArmsGoThere(walker.closedArmOfRowOfShapeTests(), Walker::ClosedArmOfRowOfShapeTests { BlockID(), question.id(), walker.depthOfCopies() });
    walker.recordSpeculation();
    Reference left = receiver;
    for (size_t place = 0; place < arms.size() && !walker.isUnreachable(); ++place) {
        Draft test(Opcode::ShapeTest, origin);
        test.setObjectOperand(left);
        test.setLayouts(types.at(arms[place]).layouts);
        Reference passes = walker.build(WTF::move(test));
        if (!passes)
            return GuessOpened::WithArms;
        bool isLast = place + 1 == arms.size();
        BlockID passed = walker.addBlock();
        BlockID failed = walker.addBlock(isLast ? BlockFrequency::Rare : BlockFrequency::Normal);
        walker.branch(passes, passed, failed);
        walker.continueIn(passed);
        if (Reference narrowed = narrowedBy(left, passes, OperandIndex::ShapeTest::object, true)) {
            walker.state().noteShape(narrowed, walker.typeOf(narrowed));
            Reference boundBefore = walker.referenceBoundTo(asked.objectOperand());
            walker.bind(asked.objectOperand(), narrowed);
            walkArm();
            walker.bind(asked.objectOperand(), boundBefore);
        }
        if (walker.hasFailed())
            return GuessOpened::WithArms;
        walker.continueIn(failed);
        if (!isLast) {
            left = narrowedBy(left, passes, OperandIndex::ShapeTest::object, false);
            if (!left)
                return GuessOpened::WithArms;
        } else if (somethingIsLeft) {
            walker.closedArmOfRowOfShapeTests() = { };
            Walker::ClosedArm closedArm(walker, false);
            walkArm();
        } else {
            TypeID layoutsOfArms = arms[0];
            for (TypeID arm : arms)
                layoutsOfArms = types.unionOf(layoutsOfArms, arm);
            walker.checkProof(receiver, layoutsOfArms, TypeWithoutProof(TypeTable::anything), ProofSource::OtherOperation, origin);
            walker.endAsUnreachable(origin);
        }
    }
    if (!walker.hasFailed())
        walker.joinArms(stretch, built, meeting);
    return GuessOpened::WithArms;
}

bool callMethodsBehindTestsOfShapeOfItsOpcodes(Walker& walker, const BodyValue& read)
{
    ValueID call = findCallOfMethodReadInUnionMethodCalls(walker.currentBodyIndex(), read);
    return call && walkBehindTestsOfShapeInUnionMethodCalls(walker, read, call);
}

bool callMethodsOfExpectedReceiversBehindTestsOfShape(Walker& walker, const BodyValue& read)
{
    if (!walker.isInLoop() || !walker.opensGuesses() || !walker.facts().layoutsByNames().severalLayoutsHold(read.as<Opcode::PropGet>().key()) || walker.isNestedTooDeeply())
        return false;
    Reference receiver = walker.referenceOf(read.as<Opcode::PropGet>().objectOperand());
    if (!receiver || !receiver.isValue())
        return false;
    bool isReceivedByLoop = !walker.code().at(walker.code().blockOf(receiver.id())).isInLoop;
    if (isReceivedByLoop && !walker.facts().layoutsByNames().isInheritedByInstances(read.as<Opcode::PropGet>().key()))
        return false;
    ValueID call = findCallOfMethodReadInUnionMethodCalls(walker.currentBodyIndex(), read);
    return call && walkBehindTestsOfShapeInUnionMethodCalls(walker, read, call, true);
}

static bool readAfterShapeTests(Walker& walker, const BodyValue& read, bool standsForGuess)
{
    ValueID object = read.as<Opcode::PropGet>().objectOperand();
    TypeID proven = object ? walker.typeOf(walker.referenceOf(object)) : TypeID();
    if (!proven || !walker.types().at(proven).hasOnlyLayouts())
        return false;
    if (walker.isNestedTooDeeply() || (!walker.isInLoop() && walker.program().at(walker.unit().function).isModuleBody()))
        return false;
    return !findCallOfMethodReadInUnionMethodCalls(walker.currentBodyIndex(), read) && walkBehindTestsOfShapeInUnionMethodCalls(walker, read, { }, false, standsForGuess);
}

bool readBehindTestsOfShapeOfItsOpcodes(Walker& walker, const BodyValue& read)
{
    return readAfterShapeTests(walker, read, false);
}

bool readTwoLayoutsAfterShapeTests(Walker& walker, const BodyValue& read)
{
    return readAfterShapeTests(walker, read, true);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
