/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkStrings.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTBuiltinPristine.h"
#include "AOTBuiltinTypes.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr PristineAspectSet aspectsThatLookAtCellProvesForStringOperands { PristineAspects::Storage, PristineAspects::Names, PristineAspects::NamesKeptBelow, PristineAspects::Prototype };

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

bool openTestOfListAtReadOfMethodOfItsOpcodes(Walker& walker, const BodyValue& read)
{
    if (read.isFreeName() || !walker.isInLoop() || walker.walksStretchInsideAnother() || walker.isUnreachable() || !walker.has(Mechanism::Inlining) || !walker.has(Mechanism::BuiltinCalls) || !walker.has(Mechanism::EscapeAnalysis))
        return false;
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    TypeArena& types = walker.types();
    Reference receiver = walker.referenceOf(read.objectOperand());
    TypeID proven = receiver && receiver.isValue() ? walker.typeOf(receiver) : TypeID();
    const ClassPart* onlyClass = proven && !isNeverType(proven) && types.at(proven).isObject() ? singleBuiltinClassOf(types, types.at(proven)) : nullptr;
    if (!onlyClass || !read.key() || pristineTestOf(table, onlyClass->builtinClass) != PristineTest::MayPass)
        return false;
    BuiltinClassID builtinClass = onlyClass->builtinClass;
    BuiltinID member = findOwnMember(program, table.at(builtinClass).prototype, read.key());
    if (!member)
        return false;
    bool copiesLibraryText = !!findLibraryFunctionsBehindBuiltin(walker, types.forBuiltin(table.canonicalID(member))).enteredByValue;
    if (!copiesLibraryText && ((builtinClass != BuiltinClasses::Map && builtinClass != BuiltinClasses::Set) || !table.boxedCallStubOf(table.canonicalID(member))))
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(read.id()));
    ValueID callID = index.onlyUsingValue(read.id());
    if (!callID || index.isParameter(callID) || index.blockOf(callID) != block.id() || index.opcodeOf(callID) != Opcode::Call || callID.index() - block.firstValue().index() >= walker.endOfStretchBeingWalked())
        return false;
    BodyValue call = index.value(callID);
    ValueID receiverID = read.objectOperand();
    auto arguments = call.as<Opcode::Call>().argumentsOperands();
    if (call.calleeOperand() != read.id() || call.thisValueOperand() != receiverID)
        return false;
    ValueID handedOver = copiesLibraryText && !arguments.empty() ? index.valueBehindCasts(arguments[0]) : ValueID();
    if (copiesLibraryText && (!handedOver || index.isParameter(handedOver) || index.opcodeOf(handedOver) != Opcode::NewClosure))
        return false;
    if (copiesLibraryText && walker.facts().linked().function(index.value(handedOver).function()).weightAsLeaf == LinkedFunction::notLeaf)
        return false;
    for (uint32_t number = read.id().index() + 1; number < callID.index(); ++number) {
        ValueID user = index.onlyUsingValue(ValueID(number));
        if (!user || callID < user)
            return false;
    }

    TypeID instances = types.builtinClass(builtinClass);
    if (!types.isSubset(proven, instances) || isListAsMade(walker, receiver) || receiverStaysAsMadeByLink(walker, read))
        return false;
    openGuessAtOperandTakenAsMade(walker, call, receiver, instances, read.id().index() - block.firstValue().index());
    return true;
}

void openGuessAtOperandTakenAsMade(Walker& walker, const BodyValue& call, Reference value, TypeID tested, std::optional<uint32_t> firstPlace)
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

    Reference proven = value;
    if (!walker.types().isSubset(walker.typeOf(value), tested)) {
        Reference isOfClass = buildTypeTestForStrings(walker, value, tested, origin);
        if (!isOfClass)
            return;
        goOnWhereTestPasses(isOfClass);
        proven = buildProofBehindTestForStrings(walker, value, isOfClass, OperandIndex::TypeTest::value, origin);
    }
    if (Reference isAsMade = proven ? buildTestOfObjectAsMade(walker, proven, origin) : Reference()) {
        goOnWhereTestPasses(isAsMade);
        noteListAsMade(walker, proven);
    }
    if (walker.hasFailed())
        return;
    BlockID typedBlock = walker.isUnreachable() ? BlockID() : walker.currentBlock();
    BlockID meetingBlock = walker.addBlock();
    walker.noteGuessOpened();

    Vector<Walker::ArmThatMeets, 2> arms;
    auto walkCallToEndOfArm = [&] {
        walker.walkStretch(inputBlock, firstPlace.value_or(place), place + 1);
        if (isReturnedAtOnce)
            walker.endArmWithReturnOf(inputBlock);
        if (walker.isUnreachable())
            return;
        Reference result = isReadBehind ? walker.referenceOf(call.id()) : Reference();
        if (isReadBehind && (!result || !result.isRecord()))
            return walker.fail("an arm of a guess at an operand has no value for what is read behind it"_s);
        arms.append(Walker::ArmThatMeets { walker.currentBlock(), { result } });
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

    Reference met;
    if (walker.letArmsMeet(arms.span(), meetingBlock, singleElementSpan(met).first(isReadBehind)) && isReadBehind)
        walker.bind(call.id(), met);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
