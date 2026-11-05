/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkStrings.h"

#if ENABLE(AOT)

#include "AOTBuiltinPristine.h"
#include "AOTBuiltinTypes.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr PristineAspectSet aspectsThatLookAtCellProvesForStringOperands { PristineAspects::Storage, PristineAspects::Names, PristineAspects::NamesKeptBelow, PristineAspects::Prototype };

namespace {

struct ArmOfCallOnOperandAsMade {
    BlockID last;
    Reference result;
};

} // anonymous namespace

bool goesByOneRowOnOperandAsMade(Walker& walker, const BodyValue& call, BuiltinID builtin, Reference value, TypeID behindTest)
{
    TypeArena& types = walker.types();
    const BuiltinTable& table = walker.program().builtins();
    Type typeBehindTest = types.at(behindTest);
    const ClassPart* onlyClass = walker.has(Mechanism::EscapeAnalysis) && typeBehindTest.isObject() ? singleBuiltinClassOf(types, typeBehindTest) : nullptr;
    if (!onlyClass || pristineTestOf(table, onlyClass->builtinClass) != PristineTest::MayPass)
        return false;

    BuiltinOperandTypes assumed;
    bool isEachBuilt = true;
    auto describe = [&](ValueID input, TypeID& type, PristineAspectSet& aspects) {
        Reference operand = walker.referenceOf(input);
        isEachBuilt &= !!operand;
        if (!operand)
            return;
        type = operand == value ? behindTest : walker.typeOf(operand);
        if (operand == value || isListAsMade(walker, operand))
            aspects = aspectsThatLookAtCellProvesForStringOperands;
    };
    if (ValueID receiver = call.thisValueOperand())
        describe(receiver, assumed.receiver, assumed.aspectsOfReceiver);
    for (ValueID argument : call.argumentsOperands()) {
        TypeID type;
        PristineAspectSet aspects;
        describe(argument, type, aspects);
        assumed.arguments.append(type);
        assumed.aspectsOfArguments.append(aspects);
    }

    BuiltinTypeContext context { walker.program(), types };
    if (!isEachBuilt || !rowsSpeakOfCallOnReceiver(context, builtin, assumed.receiver))
        return false;
    Vector<BuiltinOverloadID, 4> reached = overloadsReachedBy(context, builtin, BuiltinCallMode::Call, assumed);
    return reached.size() == 1 && !table.at(reached[0]).isUndeclared();
}

void openGuessAtOperandTakenAsMade(Walker& walker, const BodyValue& call, Reference value, TypeID tested)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BlockID inputBlock = index.blockOf(call.id());
    uint32_t place = call.id().index() - index.block(inputBlock).firstValue().index();
    BodyTerminator end = index.terminator(inputBlock);
    bool isReturnedAtOnce = end.kind() == TerminatorKind::ReturnCall && end.operand(0) == call.id();
    bool isReadBehind = !index.isUnused(call.id());

    Vector<ValueID, 2> names;
    call.forEachOperand([&](ValueID operand, uint32_t) {
        if (walker.referenceOf(operand) == value && !names.contains(operand))
            names.append(operand);
    });

    Origin origin = walker.originOf(call);
    origin.isTailCall = false;
    BlockID closedBlock;
    auto goOnWhereTestPasses = [&](Reference test) {
        BlockID passed = walker.addBlock();
        if (!closedBlock)
            closedBlock = walker.addBlock(BlockFrequency::Rare);
        walker.branch(test, passed, closedBlock);
        walker.continueIn(passed);
    };

    Reference isOfClass = buildTypeTestForStrings(walker, value, tested, origin);
    if (!isOfClass)
        return;
    goOnWhereTestPasses(isOfClass);
    Reference proven = buildProofBehindTestForStrings(walker, value, isOfClass, OperandIndex::TypeTest::value, origin);
    if (Reference isAsMade = proven ? buildTestOfObjectAsMade(walker, proven, origin) : Reference()) {
        goOnWhereTestPasses(isAsMade);
        noteListAsMade(walker, proven);
    }
    if (walker.hasFailed())
        return;
    BlockID typedBlock = walker.isUnreachable() ? BlockID() : walker.currentBlock();
    BlockID meetingBlock = walker.addBlock();
    walker.noteGuessOpened();

    Vector<ArmOfCallOnOperandAsMade, 2> arms;
    auto walkCallToEndOfArm = [&] {
        walker.walkStretch(inputBlock, place, place + 1);
        if (isReturnedAtOnce)
            walker.endArmWithReturnOf(inputBlock);
        if (walker.isUnreachable())
            return;
        Reference result = isReadBehind ? walker.referenceOf(call.id()) : Reference();
        if (isReadBehind && (!result || !result.isRecord()))
            return walker.fail("an arm of a guess at an operand has no value for what is read behind it"_s);
        arms.append({ walker.currentBlock(), result });
    };

    walker.continueIn(closedBlock);
    {
        Walker::ClosedArm closedArm(walker);
        walkCallToEndOfArm();
    }
    if (walker.hasFailed())
        return;

    if (typedBlock) {
        walker.continueIn(typedBlock);
        Vector<Reference, 2> boundBefore;
        for (ValueID name : names) {
            boundBefore.append(walker.referenceBoundTo(name));
            walker.bind(name, proven);
        }
        walkCallToEndOfArm();
        for (size_t position = 0; position < names.size(); ++position)
            walker.bind(names[position], boundBefore[position]);
    }
    if (walker.hasFailed())
        return;

    bool meetsInParameter = false;
    if (isReadBehind && !arms.isEmpty()) {
        Reference one = arms.first().result;
        Reference other = arms.last().result;
        Reference met = one;
        if (one != other) {
            const Code& code = walker.code();
            TypeID type = joinOf(walker.types(), walker.facts().families(), code.at(one.id()).type, code.at(other.id()).type);
            Representation forms[] { walker.representationOf(one), walker.representationOf(other) };
            met = walker.addParameter(meetingBlock, type, walker.formOfParameter(type, forms));
            meetsInParameter = true;
        }
        walker.bind(call.id(), met);
    }
    for (const ArmOfCallOnOperandAsMade& arm : arms) {
        walker.continueIn(arm.last);
        if (meetsInParameter)
            walker.jumpTo(meetingBlock, singleElementSpan(arm.result));
        else
            walker.jumpTo(meetingBlock);
        if (walker.hasFailed())
            return;
    }
    walker.continueIn(meetingBlock);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
