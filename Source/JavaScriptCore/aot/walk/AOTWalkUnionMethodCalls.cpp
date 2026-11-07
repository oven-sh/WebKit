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

static constexpr size_t mostArmsInUnionMethodCalls = 4;

constinit const OpcodeSet opcodesThatCallMethodsBehindTestsOfShapeTakes { Opcode::PropGet };

bool callMethodsBehindTestsOfShapeOfItsOpcodes(Walker& walker, const BodyValue& read)
{
    if (walker.standsInArmOfRowOfShapeTests() || !walker.has(Mechanism::DirectCalls) || !walker.has(Mechanism::TypedOperations))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    auto lookup = read.as<Opcode::PropGet>();
    ValueID call = index.onlyUsingValue(read.id());
    if (lookup.isFreeName() || !lookup.objectOperand() || !call || index.opcodeOf(call) != Opcode::Call || index.blockOf(call) != index.blockOf(read.id()))
        return false;
    auto written = index.value(call).as<Opcode::Call>();
    if (written.calleeOperand() != read.id() || written.thisValueOperand() != lookup.objectOperand())
        return false;
    BodyBlock block = index.block(index.blockOf(read.id()));
    uint32_t first = block.firstValue().index();
    uint32_t begin = read.id().index() - first;
    uint32_t end = call.index() - first + 1;
    Reference receiver = walker.referenceOf(lookup.objectOperand());
    if (end > walker.endOfStretchBeingWalked() || !receiver || !receiver.isValue())
        return false;

    TypeArena& types = walker.types();
    const Program& program = walker.program();
    const Plan& plan = walker.plan();
    TypeID proven = walker.typeOf(receiver);
    if (!proven || !types.at(proven).isJavaScriptValue() || (types.at(proven).primitives & ~Primitive::UndefinedOrNull) || !types.at(proven).hasOnlyLayouts() || holdsOneMember(types.at(proven).layouts))
        return false;

    bool mayBeNothing = types.at(proven).primitives;
    bool somethingIsLeft = false;
    Vector<TypeID, mostArmsInUnionMethodCalls> arms;
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
        if (typedFormOf(walker, read, TypesOfInputs(typeBehindTest)) == TypedForm::None)
            somethingIsLeft = true;
        else if (arms.size() == mostArmsInUnionMethodCalls)
            return false;
        else
            arms.append(tested);
    }
    if (arms.isEmpty())
        return false;

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

    walker.count(StatisticID::MethodCallsToldApartByShape);
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

    SetForScope standsInArm(walker.numberOfFramesAtRowOfShapeTests(), walker.depthOfCopies() + 1);
    BlockID meeting = walker.addBlock();
    walker.noteGuessOpened();
    for (size_t place = 0; place < arms.size() && !walker.isUnreachable(); ++place) {
        Draft test(Opcode::ShapeTest, origin);
        test.setObjectOperand(left);
        test.setLayouts(types.at(arms[place]).layouts);
        Reference passes = walker.build(WTF::move(test));
        if (!passes)
            return true;
        bool isLast = place + 1 == arms.size();
        BlockID passed = walker.addBlock();
        BlockID failed = walker.addBlock(isLast ? BlockFrequency::Rare : BlockFrequency::Normal);
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
            left = narrowedBy(left, passes, false);
            if (!left)
                return true;
        } else if (somethingIsLeft) {
            Walker::ClosedArm closedArm(walker);
            walkArm();
        } else
            walker.endAsUnreachable(origin);
    }
    if (!walker.hasFailed())
        walker.letArmsMeet(stretch, built, meeting);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
