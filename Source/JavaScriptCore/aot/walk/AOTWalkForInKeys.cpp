/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTSchemaInlineBuiltins.h"
#include "AOTTypeRelations.h"
#include <optional>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet usesOfKeyBehindTestInWalkForInKeys { Opcode::ElemGet, Opcode::ElemHas, Opcode::Call };
static constexpr OpcodeSet accessesUnderKeyInWalkForInKeys { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas, Opcode::ElemDelete };
static constexpr uint32_t closedOperationsOfUseOfKeyInWalkForInKeys = 1;

namespace {

struct UseOfKeyInWalkForInKeys {
    ValueID object;
    ValueID key;
};

struct ArmInWalkForInKeys {
    BlockID last;
    Reference value;
};

}

Walker::IterationByIndex* findForInThatHandedOut(Walker& walker, ValueID key)
{
    const BodyIndex& index = walker.currentBodyIndex();
    key = key ? index.valueBehindCasts(key) : ValueID();
    if (!key || index.isParameter(key) || index.opcodeOf(key) != Opcode::ResultAt)
        return nullptr;
    BodyValue read = index.value(key);
    ValueID step = read.resultsOperand();
    if (read.index() != static_cast<uint32_t>(ForInStepResult::Key) || !step || index.isParameter(step) || index.opcodeOf(step) != Opcode::ForInStep)
        return nullptr;
    Reference state = walker.referenceOf(index.value(step).enumeratorOperand());
    if (!state || !state.isVirtual())
        return nullptr;
    Walker::IterationByIndex& iteration = walker.iterationByIndex(state);
    if (iteration.otherForm != Walker::IterationByIndex::OtherForm::Enumeration || iteration.newestStep != step || !iteration.list || !iteration.foundAt)
        return nullptr;
    return &iteration;
}

static bool standsInTurnInWalkForInKeys(Walker& walker, const Walker::IterationByIndex& iteration, const BodyValue& operation)
{
    const BodyIndex& index = walker.currentBodyIndex();
    return iteration.inputHead && index.isInLoop(index.blockOf(operation.id()), index.loopOf(iteration.inputHead));
}

static bool asksObjectOfStepInWalkForInKeys(Walker& walker, const Walker::IterationByIndex& iteration, ValueID object)
{
    const BodyIndex& index = walker.currentBodyIndex();
    return object && index.valueBehindCasts(object) == index.valueBehindCasts(index.value(iteration.newestStep).objectOperand());
}

static std::optional<UseOfKeyInWalkForInKeys> findQuestionOfCallInWalkForInKeys(Walker& walker, const BodyValue& call)
{
    Reference callee = walker.referenceOf(call.calleeOperand());
    if (!callee || !callee.isRecord() || call.isDoneInPlace())
        return std::nullopt;
    Type typeOfCallee = walker.types().at(walker.typeOf(callee));
    BuiltinID builtin = typeOfCallee.isObject() ? singleBuiltinOf(walker.types(), typeOfCallee) : BuiltinID();
    if (!builtin)
        return std::nullopt;

    const BuiltinTable& table = walker.program().builtins();
    std::optional<bool> asksReceiver;
    for (const BuiltinOverload& overload : table.overloadsOf(table.canonicalID(builtin))) {
        if (overload.mode != BuiltinCallMode::Call)
            continue;
        if (overload.code.implementation.inlineBuiltin != InlineBuiltin::ObjectHasOwn || (asksReceiver && *asksReceiver != !!overload.receiver))
            return std::nullopt;
        asksReceiver = !!overload.receiver;
    }
    if (!asksReceiver)
        return std::nullopt;
    std::span<const ValueID> arguments = call.argumentsOperands();
    size_t placeOfKey = *asksReceiver ? 0 : 1;
    if (arguments.size() <= placeOfKey)
        return std::nullopt;
    const BodyIndex& index = walker.currentBodyIndex();
    BodyTerminator end = index.terminator(index.blockOf(call.id()));
    if (end.kind() == TerminatorKind::ReturnCall && end.operand(0) == call.id())
        return std::nullopt;
    return UseOfKeyInWalkForInKeys { *asksReceiver ? call.thisValueOperand() : arguments[0], arguments[placeOfKey] };
}

bool useKeyOfForInBehindTestOfItsOpcodes(Walker& walker, const BodyValue& operation)
{
    Opcode opcode = operation.opcode();
    if (!usesOfKeyBehindTestInWalkForInKeys.contains(opcode) || !walker.has(Mechanism::Guesses) || !walker.has(Mechanism::TypedOperations))
        return false;
    if (!walker.currentBodyIndex().holdsAnyOf(OpcodeSet { Opcode::ForInStep }))
        return false;
    std::optional<UseOfKeyInWalkForInKeys> use = opcode == Opcode::Call ? findQuestionOfCallInWalkForInKeys(walker, operation) : std::optional(UseOfKeyInWalkForInKeys { operation.objectOperand(), operation.propertyOperand() });
    Walker::IterationByIndex* iteration = use ? findForInThatHandedOut(walker, use->key) : nullptr;
    if (!iteration || !standsInTurnInWalkForInKeys(walker, *iteration, operation) || !asksObjectOfStepInWalkForInKeys(walker, *iteration, use->object))
        return false;
    if (iteration->keyForAccesses)
        return false;
    Reference object = walker.referenceOf(use->object);
    Reference key = walker.referenceOf(use->key);
    if (!object || !object.isValue() || !key || !key.isValue() || !walker.types().at(walker.typeOf(object)).hasObjects())
        return false;
    if (opcode == Opcode::ElemGet && !walker.types().at(widestTypeOfOperation(walker, operation)).isAnything())
        return false;
    if (!shouldOpenGuess(walker, object, TypeTable::object, true, closedOperationsOfUseOfKeyInWalkForInKeys))
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(operation.id()));
    uint32_t place = operation.id().index() - block.firstValue().index();
    Origin origin = walker.originOf(operation);
    origin.isTailCall = false;

    Draft test(Opcode::IsEnumeratorCurrent, origin);
    walker.chooseConservativeFields(test);
    test.setEnumeratorOperand(iteration->list);
    test.setObjectOperand(object);
    test.setPositionOperand(iteration->foundAt);
    Reference isCurrent = walker.build(WTF::move(test));
    if (!isCurrent)
        return true;

    Reference enumerator = iteration->list;
    Reference foundAt = iteration->foundAt;
    BlockID current = walker.addBlock();
    BlockID asWritten = walker.addBlock(BlockFrequency::Unlikely);
    BlockID meeting = walker.addBlock();
    walker.branch(isCurrent, current, asWritten);
    walker.noteGuessOpened();

    Vector<ArmInWalkForInKeys, 2> arms;
    auto endArm = [&] {
        if (walker.isUnreachable() || walker.hasFailed())
            return;
        arms.append(ArmInWalkForInKeys { walker.currentBlock(), walker.referenceOf(operation.id()) });
    };

    walker.continueIn(asWritten);
    {
        Walker::ClosedArm closedArm(walker);
        walker.walkStretch(block.id(), place, place + 1);
        endArm();
    }
    if (walker.hasFailed())
        return true;

    walker.continueIn(current);
    Reference answer;
    if (opcode == Opcode::ElemGet) {
        Draft written = walker.draftOf(operation);
        Draft load(Opcode::EnumeratorGetByVal, origin);
        walker.chooseConservativeFields(load);
        load.setObjectOperand(object);
        load.setPropertyOperand(key);
        load.setEnumeratorOperand(enumerator);
        load.setPositionOperand(foundAt);
        load.expected = written.expected;
        load.inputValueGiven = written.inputValueGiven;
        answer = walker.build(WTF::move(load));
    } else {
        Draft yes(Opcode::Boolean, origin);
        yes.setBoolean(true);
        answer = walker.constant(WTF::move(yes));
    }
    if (answer) {
        walker.bind(operation.id(), answer);
        endArm();
    }
    if (walker.hasFailed())
        return true;

    Reference met;
    if (arms.size() == 2 && arms[0].value && arms[1].value) {
        const Code& code = walker.code();
        TypeID type = joinOf(walker.types(), walker.facts().families(), code.at(arms[0].value.id()).type, code.at(arms[1].value.id()).type);
        Representation forms[] { walker.representationOf(arms[0].value), walker.representationOf(arms[1].value) };
        met = walker.addParameter(meeting, type, walker.formOfParameter(type, forms));
    }
    for (const ArmInWalkForInKeys& arm : arms) {
        walker.continueIn(arm.last);
        if (met)
            walker.jumpTo(meeting, singleElementSpan(arm.value));
        else
            walker.jumpTo(meeting);
        if (walker.hasFailed())
            return true;
    }
    walker.continueIn(meeting);
    if (met)
        walker.bind(operation.id(), met);
    else if (arms.size() == 1 && arms[0].value)
        walker.bind(operation.id(), arms[0].value);
    return true;
}

static bool walkAccessByNameInWalkForInKeys(Walker& walker, const BodyValue& written, PropertyKeyID key)
{
    AccessUnderProvenKey access(written, key);
    BodyValue byName = access.value();
    if ((walker.has(Mechanism::Folding) && foldOperation(walker, byName)) || buildTypedOperation(walker, byName) || replaceOperation(walker, byName))
        return true;
    return walker.copy(byName);
}

bool buildAccessUnderIndexOfForInOfItsOpcodes(Walker& walker, const BodyValue& access)
{
    if (!accessesUnderKeyInWalkForInKeys.contains(access.opcode()))
        return false;
    ValueID property = access.propertyOperand();
    Walker::IterationByIndex* iteration = findForInThatHandedOut(walker, property);
    if (!iteration || !standsInTurnInWalkForInKeys(walker, *iteration, access))
        return false;

    Reference bound = walker.referenceBoundTo(property);
    if (!bound)
        return false;
    if (!iteration->takenAtOpen.isEmpty() && bound.isConstant() && walker.code().at(bound.id()).opcode == Opcode::String) {
        StringID name = walker.code().at(bound.id()).string();
        PropertyKeyID key = liesInThread(name) ? PropertyKeyID() : walker.types().textKey(name);
        if (!key || liesInThread(key) || walker.program().isIndexLike(key))
            return false;
        return walkAccessByNameInWalkForInKeys(walker, access, key);
    }

    Reference index = iteration->keyForAccesses;
    if (!index || bound == index)
        return false;
    if (property == access.objectOperand() || (access.opcode() == Opcode::ElemSet && property == access.valueOperand()))
        return false;
    const BodyIndex& body = walker.currentBodyIndex();
    BodyBlock block = body.block(body.blockOf(access.id()));
    uint32_t place = access.id().index() - block.firstValue().index();
    walker.bind(property, index);
    walker.walkStretch(block.id(), place, place + 1);
    walker.bind(property, bound);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
