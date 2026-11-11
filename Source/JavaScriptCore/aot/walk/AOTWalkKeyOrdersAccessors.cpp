/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTLinkTables.h"
#include "AOTNameClass.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t closedOperationsOfAccessThatCallsAccessorInKeyOrdersAccessors = 2;

static ALWAYS_INLINE LayoutID onlyLayoutOfInKeyOrdersAccessors(const Walker& walker, TypeID listed)
{
    if (!listed)
        return { };
    Type type = walker.types().at(listed);
    if (!type.isJavaScriptValue() || type.primitives || !type.hasOnlyLayouts() || type.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow) || !holdsOneMember(type.layouts))
        return { };
    return singleLayoutOf(walker.types(), type);
}

static std::optional<FieldReference> findAccessorSlotInKeyOrdersAccessors(const Walker& walker, LayoutID layout, PropertyKeyID key)
{
    const Program& program = walker.program();
    const Plan& plan = walker.plan();
    if (!layout || !key || liesInThread(key) || program.at(key).kind != PropertyKeyKind::Text || program.isIndexLike(key) || program.nameClassOf(key) == NameClass::Proto)
        return std::nullopt;
    if (!isFoundByShapeTest(program.at(layout)) || !plan.isInUse(layout) || !plan.slots().holdsCompiledObjects(layout))
        return std::nullopt;
    std::optional<FieldIndex> field = plan.slots().findField(program, layout, key);
    if (!field || !plan.slot(layout, *field).holdsAccessor || !plan.slot(layout, *field).hasStorage())
        return std::nullopt;
    return FieldReference { layout, *field };
}

static FunctionID findAccessorExpectedInKeyOrdersAccessors(const Walker& walker, FieldReference field, bool isStore)
{
    const LinkTables& linked = walker.facts().linked();
    const LinkedAccessorsOfField* expected = linked.findAccessorsExpectedIn(field);
    FunctionID accessor = expected ? (isStore ? expected->setter : expected->getter) : FunctionID();
    return accessor && linked.mayBeMentionedBy(walker.unit(), accessor) ? accessor : FunctionID();
}

static bool isProvenHalfInKeyOrdersAccessors(const Walker& walker, FieldReference field, bool isStore)
{
    const LinkedAccessorsOfField* laid = walker.facts().linked().findAccessorsExpectedIn(field);
    return laid && (isStore ? laid->setterIsProven : laid->getterIsProven);
}

static bool halfMayNotBeLaidYetInKeyOrdersAccessors(const Walker& walker, FieldReference field, bool isStore)
{
    const LinkedAccessorsOfField* laid = walker.facts().linked().findAccessorsExpectedIn(field);
    return laid && !isStore && laid->getterMayNotBeLaidYet;
}

static void checkProvenHalfInKeyOrdersAccessors(Walker& walker, const BodyValue& access, Reference object, FieldReference field)
{
    if (!walker.checksProofs())
        return;
    bool isStore = access.opcode() == Opcode::PropSet;
    Origin origin = walker.originOf(access);
    origin.isTailCall = false;
    Draft load(isStore ? Opcode::GetSlotSetter : Opcode::GetSlotGetter, origin);
    load.setObjectOperand(object);
    load.setKey(access.key());
    load.setReceivers(walker.types().at(walker.types().forLayout(field.layout)).layouts);
    load.setLevel(KnowledgeLevel::Proven);
    if (isStore)
        load.setIsInStrictCode(access.mode() == ECMAMode::Strict);
    walker.buildTestOfCheck(WTF::move(load));
}

bool callProvenAccessorOfItsOpcodes(Walker& walker, const BodyValue& access)
{
    bool isStore = access.opcode() == Opcode::PropSet;
    if (access.isFreeName())
        return false;
    PropertyKeyID key = access.key();
    if (!key || liesInThread(key) || !walker.facts().linked().keysOfAccessorsExpectedInFields.get(key.index()))
        return false;
    if (!walker.has(Mechanism::TypedOperations))
        return false;
    Reference object = walker.referenceOf(access.objectOperand());
    Reference stored = isStore ? walker.referenceOf(access.valueOperand()) : Reference();
    if (!object || !object.isValue() || (isStore && !stored))
        return false;
    std::optional<FieldReference> field = findAccessorSlotInKeyOrdersAccessors(walker, onlyLayoutOfInKeyOrdersAccessors(walker, walker.typeOf(object)), key);
    if (!field || !isProvenHalfInKeyOrdersAccessors(walker, *field, isStore) || halfMayNotBeLaidYetInKeyOrdersAccessors(walker, *field, isStore))
        return false;
    FunctionID half = findAccessorExpectedInKeyOrdersAccessors(walker, *field, isStore);
    if (!half)
        return false;
    checkProvenHalfInKeyOrdersAccessors(walker, access, object, *field);
    return walker.isUnreachable() || callAccessor(walker, access, half, object, stored);
}

static void fillSlotOperationInKeyOrdersAccessors(Walker& walker, Draft& draft, Reference object, FieldReference field, PropertyKeyID key)
{
    draft.setObjectOperand(object);
    draft.setKey(key);
    draft.setReceivers(walker.types().at(walker.types().forLayout(field.layout)).layouts);
    draft.setLevel(KnowledgeLevel::Proven);
}

bool callAccessorInSlot(Walker& walker, const BodyValue& access)
{
    Opcode opcode = access.opcode();
    bool isStore = opcode == Opcode::PropSet;
    if ((opcode != Opcode::PropGet && !isStore) || access.isFreeName())
        return false;
    if (PropertyKeyID name = access.key(); !name || liesInThread(name) || !walker.facts().linked().keysOfAccessorsExpectedInFields.get(name.index()))
        return false;
    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !walker.has(Mechanism::DirectCalls) || !walker.has(Mechanism::TypedOperations))
        return false;
    Reference object = walker.referenceOf(access.objectOperand());
    Reference stored = isStore ? walker.referenceOf(access.valueOperand()) : Reference();
    if (!object || !object.isValue() || (isStore && !stored))
        return false;

    TypeArena& types = walker.types();
    TypeID proven = walker.typeOf(object);
    LayoutID layout = onlyLayoutOfInKeyOrdersAccessors(walker, proven);
    bool layoutIsProven = !!layout;
    if (!layoutIsProven) {
        ExpectationOfValue expectation = findExpectation(walker, access.objectOperand());
        TypeID expected = expectation.type && expectation.isDeclared ? types.withoutNullish(expectation.type) : TypeID();
        if (!expected || !proven || !types.overlap(proven, expected))
            return false;
        layout = onlyLayoutOfInKeyOrdersAccessors(walker, expected);
    }
    PropertyKeyID key = access.key();
    std::optional<FieldReference> field = findAccessorSlotInKeyOrdersAccessors(walker, layout, key);
    bool halfIsProven = field && isProvenHalfInKeyOrdersAccessors(walker, *field, isStore);
    FunctionID provenHalf = halfIsProven ? findAccessorExpectedInKeyOrdersAccessors(walker, *field, isStore) : FunctionID();
    bool slotIsFilled = provenHalf && !halfMayNotBeLaidYetInKeyOrdersAccessors(walker, *field, isStore);
    if (!field || !shouldOpenGuess(walker, object, types.forLayout(layout), true, closedOperationsOfAccessThatCallsAccessorInKeyOrdersAccessors))
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(access.id()));
    uint32_t place = access.id().index() - block.firstValue().index();
    Origin origin = walker.originOf(access);
    origin.isTailCall = false;

    BlockID asWritten = walker.addBlock(BlockFrequency::Rare);
    BlockID meeting = walker.addBlock();
    walker.recordSpeculation();

    Walker::StretchInArms stretch = walker.stretchInArms(block.id(), place, place + 1);
    Walker::ArmsThatMeet arms;
    auto continueIfCheckPasses = [&](Reference test, BlockID whereTestFails) {
        if (!test)
            return false;
        BlockID passed = walker.addBlock();
        walker.branch(test, passed, whereTestFails);
        walker.continueIn(passed);
        return !walker.isUnreachable();
    };
    auto buildProof = [&](Reference value, Reference test, uint32_t operandIndex) {
        Draft proof(Opcode::Narrowed, origin);
        proof.setValueOperand(value);
        proof.setTestOperand(test);
        proof.setOperandIndex(operandIndex);
        proof.setIsOnTakenEdge(true);
        return walker.build(WTF::move(proof));
    };
    auto callByValue = [&](Reference half) {
        Draft call(Opcode::Call, origin);
        if (!isStore)
            call.inputValueGiven = access.id();
        call.setCalleeOperand(half);
        call.setThisValueOperand(object);
        if (isStore)
            call.setArgumentsOperands(singleElementSpan(stored));
        Reference called = walker.build(WTF::move(call));
        if (!isStore)
            walker.bind(access.id(), called);
        walker.listArm(stretch, arms);
    };

    walker.letGuessFailUnasked(asWritten, origin);
    bool goesOn = true;
    if (!layoutIsProven) {
        Draft test(Opcode::ShapeTest, origin);
        test.setObjectOperand(object);
        test.setLayouts(types.at(types.forLayout(layout)).layouts);
        Reference hasShape = walker.build(WTF::move(test));
        goesOn = continueIfCheckPasses(hasShape, asWritten);
        if (goesOn) {
            object = buildProof(object, hasShape, OperandIndex::ShapeTest::object);
            goesOn = !!object;
            if (goesOn && slotIsFilled)
                checkProvenHalfInKeyOrdersAccessors(walker, access, object, *field);
            if (goesOn && slotIsFilled && callAccessor(walker, access, provenHalf, object, stored)) {
                walker.listArm(stretch, arms);
                goesOn = false;
            }
        }
    }
    if (goesOn) {
        Draft look(Opcode::IsSlotFilled, origin);
        fillSlotOperationInKeyOrdersAccessors(walker, look, object, *field, key);
        goesOn = continueIfCheckPasses(walker.build(WTF::move(look)), asWritten);
    }
    Reference half;
    if (goesOn) {
        Draft load(isStore ? Opcode::GetSlotSetter : Opcode::GetSlotGetter, origin);
        fillSlotOperationInKeyOrdersAccessors(walker, load, object, *field, key);
        if (isStore)
            load.setIsInStrictCode(access.mode() == ECMAMode::Strict);
        else {
            if (!halfIsProven)
                load.representation = Representation::JSValue;
        }
        half = walker.build(WTF::move(load));
    }
    if (walker.hasFailed())
        return true;

    if (half && provenHalf) {
        if (callAccessor(walker, access, provenHalf, object, stored, half))
            walker.listArm(stretch, arms);
        else
            callByValue(half);
        if (walker.hasFailed())
            return true;
        half = { };
    }

    if (half) {
        FunctionID expected = findAccessorExpectedInKeyOrdersAccessors(walker, *field, isStore);
        BlockID otherFunction = expected ? walker.addBlock(BlockFrequency::Unlikely) : BlockID();
        Reference isExpected;
        if (expected) {
            Draft test(Opcode::TypeTest, origin);
            test.setValueOperand(half);
            test.setTestedType(types.forFunction(expected));
            isExpected = walker.build(WTF::move(test));
        }
        if (isExpected) {
            Reference closure = continueIfCheckPasses(isExpected, otherFunction) ? buildProof(half, isExpected, OperandIndex::TypeTest::value) : Reference();
            if (closure && callAccessor(walker, access, expected, object, stored, closure))
                walker.listArm(stretch, arms);
            else if (closure)
                callByValue(closure);
            if (walker.hasFailed())
                return true;
            walker.continueIn(otherFunction);
        }
        if (!walker.isUnreachable())
            callByValue(half);
        if (walker.hasFailed())
            return true;
    }

    walker.walkClosedArm(asWritten, stretch, arms);
    if (!walker.hasFailed())
        walker.joinArms(stretch, arms, meeting);
    return true;
}

static VariableID findVariableOfStaticFieldInKeyOrdersAccessors(Walker& walker, TypeID receiver, PropertyKeyID key)
{
    const Program& program = walker.program();
    if (!receiver || !key || liesInThread(key) || program.at(key).kind != PropertyKeyKind::Text)
        return { };
    TypeArena& types = walker.types();
    Type type = types.at(receiver);
    if (!type.isObject() || type.primitives || !type.hasOnlyClosures())
        return { };
    FunctionID constructor = singleFunctionOf(types, type);
    LayoutID classLayout = constructor ? classOfClassObject(program, constructor) : LayoutID();
    if (!classLayout || !walker.facts().isEvaluatedAtMostOnce(classLayout) || !walker.plan().slots().classCreation(classLayout).createsClassObjectOnMutation)
        return { };
    const Layout& record = program.at(classLayout);
    std::optional<uint32_t> field = record.findStaticField(key);
    if (!field)
        return { };
    if (classHolderIsMadeWithName(program, ClassSide::Static, key))
        return { };
    VariableID variable = record.staticFields[*field].variable;
    PlannedVariable planned = walker.plan().variable(variable);
    if (!planned.hasWordInStatics() || planned.encoding != VariableEncoding::Plain || planned.representation != SlotRepresentation::JSValue)
        return { };
    return variable;
}

static bool fieldStillLiesWhereArmHasSeenIt(Walker& walker, Walker::StaticFieldSeenInVariable& seen)
{
    const Code& code = walker.code();
    uint32_t oldest = seen.blockWhereSeen.index();
    BitVector isOnWays;
    Vector<BlockID, 8> blocksOnWays;
    auto takeOntoWays = [&](BlockID block) {
        if (block.index() < oldest)
            return false;
        if (!isOnWays.set(block.index() - oldest))
            blocksOnWays.append(block);
        return true;
    };
    auto everyWayComesFromWhereItWasSeen = [&] {
        if (!takeOntoWays(walker.currentBlock()))
            return false;
        for (size_t next = 0; next < blocksOnWays.size(); ++next) {
            BlockID block = blocksOnWays[next];
            if (block == seen.blockWhereSeen)
                continue;
            if (!code.at(block).firstEdgeIn() || code.at(block).isLoopHead || code.at(block).isHandlerBlock() || code.at(block).beliefAheadOfEdges != BeliefAheadOfEdges::NoEdgeIsAwaited)
                return false;
            for (EdgeID edge = code.at(block).firstEdgeIn(); edge; edge = code.at(edge).nextInto()) {
                BlockID from = code.at(edge).from;
                if (code.at(from).handler == edge || !takeOntoWays(from))
                    return false;
            }
        }
        return isOnWays.get(0);
    };
    auto nothingBuiltOnWaysReachesField = [&] {
        for (uint32_t built = seen.firstOperationBehind; built < code.numberOfValues(); ++built) {
            const Operation& operation = code.at(ValueID(built));
            if (operation.opcode == Opcode::GetModuleVariable || !operation.block() || operation.block().index() < oldest || !isOnWays.get(operation.block().index() - oldest))
                continue;
            if (operation.opcode == Opcode::PutStaticField) {
                if (operation.variable() == seen.variable)
                    return false;
                continue;
            }
            EffectTraits traits = operation.traits();
            if (traits.mayRunOtherCode() || traits.mayCollect() || traits.mayBeTerminated() || traits.mayWriteOutsideObjects())
                return false;
            if (traits.mayTouchMemory() && declaredEffectsOfOperation(walker.program(), operation.opcode, operation).writes.containsAny(bookkeepingHeaps))
                return false;
        }
        return true;
    };
    if (everyWayComesFromWhereItWasSeen() && nothingBuiltOnWaysReachesField())
        return true;
    seen.variable = { };
    return false;
}

static bool namesSameHolderInKeyOrdersAccessors(const BodyIndex& index, ValueID one, ValueID other)
{
    auto holder = [&](ValueID named) {
        named = index.stripCasts(named);
        return !index.isParameter(named) && index.opcodeOf(named) == Opcode::CheckInitialized ? index.stripCasts(index.value(named).operand(0)) : named;
    };
    one = holder(one);
    other = holder(other);
    if (one == other)
        return true;
    if (index.isParameter(one) || index.isParameter(other) || index.opcodeOf(one) != Opcode::GetModuleVariable || index.opcodeOf(other) != Opcode::GetModuleVariable)
        return false;
    return index.value(one).variable() == index.value(other).variable();
}

bool accessStaticFieldInVariable(Walker& walker, const BodyValue& access)
{
    Opcode opcode = access.opcode();
    bool isStore = opcode == Opcode::PropSet;
    if ((opcode != Opcode::PropGet && !isStore) || access.isFreeName())
        return false;
    if (PropertyKeyID name = access.key(); !name || liesInThread(name) || !walker.facts().linked().keysOfStaticFields.get(name.index()))
        return false;
    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !walker.has(Mechanism::TypedOperations))
        return false;
    Reference object = walker.referenceOf(access.objectOperand());
    Reference stored = isStore ? walker.referenceOf(access.valueOperand()) : Reference();
    if (!object || !object.isValue() || (isStore && !stored))
        return false;
    TypeArena& types = walker.types();
    VariableID variable = findVariableOfStaticFieldInKeyOrdersAccessors(walker, walker.typeOf(object), access.key());
    if (!variable)
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(access.id()));
    uint32_t place = access.id().index() - block.firstValue().index();
    Origin origin = walker.originOf(access);
    origin.isTailCall = false;

    if (Walker::StaticFieldSeenInVariable* seen = walker.staticFieldSeenInVariableByArm(); seen && seen->variable == variable && fieldStillLiesWhereArmHasSeenIt(walker, *seen)) {
        walker.count(StatisticID::StaticFieldGuardsReused);
        if (!isStore) {
            Reference held = seen->held;
            if (Reference asArmKnowsIt = seen->readThatNamesIt && seen->inlineFrameOfRead == origin.inlineFrame ? walker.referenceOf(seen->readThatNamesIt) : Reference())
                held = asArmKnowsIt;
            walker.bind(access.id(), held);
            *seen = { variable, held, walker.currentBlock(), walker.code().numberOfValues(), access.id(), origin.inlineFrame };
            return true;
        }
        Draft store(Opcode::PutStaticField, origin);
        store.setVariable(variable);
        store.setClassObjectOperand(object);
        store.setValueOperand(stored);
        walker.build(WTF::move(store));
        *seen = { variable, stored, walker.currentBlock(), walker.code().numberOfValues(), { }, 0 };
        return true;
    }

    Draft read(Opcode::GetStaticFieldOrAbsent, origin);
    read.setVariable(variable);
    read.setValueType(types.withPrimitives(TypeTable::anything, Primitive::Absent));
    read.setClassObjectOperand(object);
    Reference held = walker.build(WTF::move(read));
    if (!held || walker.hasFailed())
        return true;
    bool mayRestartIteration = !isStore && walker.canRestartIteration();
    Reference isAbsent;
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, mayRestartIteration && walker.isInvariantInSpecializedVersion(held));
        Draft test(Opcode::TypeTest, origin);
        test.setValueOperand(held);
        test.setTestedType(TypeArena::fromPrimitives(Primitive::Absent));
        isAbsent = walker.build(WTF::move(test));
    }
    if (!isAbsent || walker.hasFailed())
        return true;
    if (mayRestartIteration && walker.tryBranchInPreheader(isAbsent, false, Walker::IterationPurity::EffectFree, Walker::GuardedIterations::SomeWithUnguardedVersion)) {
        Walker::SpecializedPreheaderScope preheaderScope(walker, true);
        Draft proof(Opcode::Narrowed, origin);
        proof.setValueOperand(held);
        proof.setTestOperand(isAbsent);
        proof.setOperandIndex(OperandIndex::TypeTest::value);
        proof.setIsOnTakenEdge(false);
        walker.recordSpeculation();
        walker.bind(access.id(), walker.build(WTF::move(proof)));
        walker.count(StatisticID::StaticFieldGuardsHoisted);
        return true;
    }

    uint32_t end = place + 1;
    static constexpr OpcodeSet operationsBetweenAccessesOfOneLook = constantsInFormOfUses | inputIdentityOpcodes | allNumericOperators | allRelations | allEqualities | OpcodeSet { Opcode::GetModuleVariable, Opcode::CheckInitialized };
    bool mayTakeMore = walker.isInLoop() && !walker.staticFieldSeenInVariableByArm() && !walker.isNestedTooDeeply();
    for (uint32_t later = end, limit = mayTakeMore ? limitOfStretchOfArms(walker, block) : 0; later < limit; ++later) {
        BodyValue other = index.value(ValueID(block.firstValue().index() + later));
        if ((other.opcode() == Opcode::PropGet || other.opcode() == Opcode::PropSet) && !other.isFreeName() && other.key() == access.key() && namesSameHolderInKeyOrdersAccessors(index, other.objectOperand(), access.objectOperand()))
            end = later + 1;
        else if (!operationsBetweenAccessesOfOneLook.contains(other.opcode()))
            break;
    }

    BlockID asWritten = walker.addBlock(BlockFrequency::Rare);
    BlockID liesInVariable = walker.addBlock();
    BlockID meeting = walker.addBlock();
    walker.recordSpeculation();
    walker.branch(isAbsent, asWritten, liesInVariable);

    Walker::StretchInArms stretch = walker.stretchInArms(block.id(), place, end);
    Walker::ArmsThatMeet arms;

    walker.continueIn(liesInVariable);
    if (!walker.isUnreachable()) {
        Reference liesThere = stored;
        if (isStore) {
            Draft store(Opcode::PutStaticField, origin);
            store.setVariable(variable);
            store.setClassObjectOperand(object);
            store.setValueOperand(stored);
            walker.build(WTF::move(store));
        } else {
            Draft proof(Opcode::Narrowed, origin);
            proof.setValueOperand(held);
            proof.setTestOperand(isAbsent);
            proof.setOperandIndex(OperandIndex::TypeTest::value);
            proof.setIsOnTakenEdge(false);
            liesThere = walker.build(WTF::move(proof));
            walker.bind(access.id(), liesThere);
        }
        if (end > place + 1 && liesThere && !walker.hasFailed() && !walker.isUnreachable()) {
            Walker::StaticFieldSeenInVariable seen { variable, liesThere, liesInVariable, walker.code().numberOfValues(), isStore ? ValueID() : access.id(), origin.inlineFrame };
            SetForScope ofThisArm(walker.staticFieldSeenInVariableByArm(), &seen);
            if (TypeID expected = isStore ? TypeID() : findExpectation(walker, access.id()).type)
                openGuess(walker, liesThere, expected, block.id(), place + 1, end);
            else
                walker.walkStretch(block.id(), place + 1, end);
        }
        walker.listArm(stretch, arms);
    }
    if (walker.hasFailed())
        return true;

    walker.walkClosedArm(asWritten, stretch, arms);
    if (!walker.hasFailed())
        walker.joinArms(stretch, arms, meeting);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
