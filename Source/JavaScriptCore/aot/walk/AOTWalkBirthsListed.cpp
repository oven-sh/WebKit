/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// EscapeAnalysis: an allocation lists what the stores right behind it lay, out of values that stand in front of it. The stores are walked as written, find their value in the slot, and build nothing.
// At a suspension: between the allocation and the last store that it lists stand written constants and casts alone, so none lies there. At a throw: the same, so nobody sees the object with a part of the row.
// Withheld (EscapeAnalysis; Inlining for a construction; Guesses where a cell has to be looked at): the allocation lists what is written, and each store is built.

#include "config.h"
#include "AOTWalkBirths.h"
#include "AOTWalkTextBuilders.h"

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTEffectsDeclared.h"
#include "AOTFamilies.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTThinSummary.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet constantsListedAtBirth { Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String };
// None of them does anything that a store moved up past it could change, or that could see the object.
static constexpr OpcodeSet operationsBetweenStoresOfBirth = constantsListedAtBirth | opcodesThatStateTypes | OpcodeSet { Opcode::FinishObject };

namespace {

struct StoresBehindAllocationInBirths {
    Vector<PropertyKeyID, 16> keys;
    Vector<Reference, 16> values;
    Vector<TypeID, 16> types;
};

struct ArmOfBirth {
    BlockID last;
    Reference value;
};

} // anonymous namespace

// ---- What lies in a slot

static ValueID recordBehindNamesAndFormsInBirths(const Code& code, ValueID record)
{
    while (opcodesThatGiveSameCell.contains(code.at(record).opcode))
        record = code.at(record).operand(0);
    return record;
}

bool storeLaysWhatSlotOfFreshObjectHolds(Walker& walker, Reference object, Reference held, Reference stored)
{
    if (!held || !held.isRecord() || !stored || !stored.isRecord() || !walker.state().isFresh(object))
        return false;
    const Code& code = walker.code();
    ValueID first = recordBehindNamesAndFormsInBirths(code, held.id());
    ValueID second = recordBehindNamesAndFormsInBirths(code, stored.id());
    if (first == second)
        return true;
    // A use gets a written constant in the form that it wants, which is a record of its own.
    const Operation& one = code.at(first);
    const Operation& other = code.at(second);
    return one.opcode == other.opcode && constantsListedAtBirth.contains(one.opcode) && std::ranges::equal(one.payload(), other.payload());
}

// ---- The mark

static bool markAnswersForSomeFieldInBirths(const Plan& plan, LayoutID layout)
{
    const PlannedLayout& planned = plan.layout(layout);
    for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
        if (planned.slots[field].isFilledAtBirth || planned.holdsRawValueFromBirth(field))
            return true;
    }
    return false;
}

bool lookMayFinishBirthOf(Walker& walker, Reference object)
{
    if (object && walker.state().factsOfCell(object).contains(FactOfCell::BearsNoMark))
        return false;
    TypeID proven = object ? walker.typeOf(object) : TypeID();
    if (!proven)
        return true;
    Type type = walker.types().at(proven);
    if (type.primitives || !type.hasOnlyLayouts() || type.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow))
        return true;
    return std::ranges::any_of(membersOf(walker.types(), type.layouts), [&](LayoutID member) {
        return markAnswersForSomeFieldInBirths(walker.plan(), member);
    });
}

void finishBirthAtReturnOfClassConstructor(Walker& walker, Reference returned, Origin origin)
{
    if (!returned || !returned.isValue() || walker.isUnreachable() || !walker.has(Mechanism::TypedOperations))
        return;
    const Plan& plan = walker.plan();
    TypeArena& types = walker.types();
    const Function& function = walker.program().at(walker.currentFunction());
    LayoutID own = function.constructedClass;
    if (!isClassConstructorOf(function.kind) || !own || !plan.isInUse(own) || !plan.layout(own).holdsCompiledObjects() || !markAnswersForSomeFieldInBirths(plan, own))
        return;
    if (walker.state().factsOfCell(returned).contains(FactOfCell::BearsNoMark))
        return;
    origin.isTailCall = false;
    auto tryFinish = [&](Reference object) {
        Draft finish(Opcode::TryFinishBirth, origin);
        finish.setObjectOperand(object);
        walker.chooseConservativeFields(finish);
        walker.build(WTF::move(finish));
    };
    TypeID proven = walker.typeOf(returned);
    if (!proven)
        return;
    Type type = types.at(proven);
    if (!type.primitives && type.hasOnlyLayouts() && !type.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow)) {
        tryFinish(returned);
        return;
    }

    // new.target may have selected a class below, whose own constructor ends the birth. On a type that does not close its layouts the look at the slots is a call of the run time, so the shape is asked first.
    TypeID tested = types.forLayout(own);
    if (plan.shapes().shapesOf(own).isEmpty() || !types.overlap(proven, tested))
        return;
    GuessInFrontOfLoop isOfOwnClass { returned, tested, types.at(tested).layouts, { } };
    Reference test = buildTestInFrontOfLoop(walker, isOfOwnClass, origin);
    if (!test)
        return;
    BlockID finishes = walker.addBlock();
    BlockID leavesToClassBelow = walker.addBlock();
    BlockID behind = walker.addBlock();
    walker.branch(test, finishes, leavesToClassBelow);
    walker.continueIn(finishes);
    if (Reference narrowed = buildProofInFrontOfLoop(walker, isOfOwnClass, test, origin))
        tryFinish(narrowed);
    walker.jumpTo(behind);
    walker.continueIn(leavesToClassBelow);
    walker.jumpTo(behind);
    walker.continueIn(behind);
}

// ---- The stores behind an allocation, as the body has them

static Reference constantWrittenInBodyForBirths(Walker& walker, const BodyValue& written)
{
    Opcode opcode = written.opcode();
    Draft constant(opcode);
    if (opcode == Opcode::Boolean)
        constant.setBoolean(written.boolean());
    else if (opcode == Opcode::Integer)
        constant.setInteger(written.integer());
    else if (opcode == Opcode::Double)
        constant.setNumber(written.number());
    else if (opcode == Opcode::String)
        constant.setString(written.string());
    return walker.constant(WTF::move(constant));
}

static ValueID valueBehindNamesInBodyForBirths(const BodyIndex& index, ValueID value)
{
    while (value && !index.isParameter(value) && inputOperationsThatHandOn.contains(index.opcodeOf(value)))
        value = index.value(value).operand(0);
    return value;
}

// Operations [begin, end) of the block. `object`: the value of the body that all names of the object come to. `valueInFrontOfAllocation`: what stands for a value of the body where the allocation is built, null where nothing does.
// True: it has come to `end`. False: it has stopped in front of an operation that is no part of the row.
static bool collectStoresInBirths(Walker& walker, const BodyIndex& index, const BodyBlock& block, uint32_t begin, uint32_t end, ValueID object, NOESCAPE const ScopedLambda<Reference(ValueID)>& valueInFrontOfAllocation, StoresBehindAllocationInBirths& stores)
{
    uint32_t firstValue = block.firstValue().index();
    for (uint32_t place = begin; place < end; ++place) {
        BodyValue operation = index.value(ValueID(firstValue + place));
        Opcode opcode = operation.opcode();
        if (operationsBetweenStoresOfBirth.contains(opcode))
            continue;
        if (opcode != Opcode::PropSet || operation.isFreeName() || valueBehindNamesInBodyForBirths(index, operation.objectOperand()) != object)
            return false;

        ValueID root = index.valueBehindCasts(operation.valueOperand());
        bool isWrittenConstant = !index.isParameter(root) && constantsListedAtBirth.contains(index.opcodeOf(root));
        Reference value = isWrittenConstant ? constantWrittenInBodyForBirths(walker, index.value(root)) : valueInFrontOfAllocation(root);
        // `const mark = { n: i }; mark.seen = line; .. line += ..`: the store is looked at ahead of its turn. So what it stores is still bound to the builder, of which typeOf() says `string`.
        if (value && walker.hasTextsInStringBuilders() && isTextInStringBuilder(walker, value))
            return false;
        TypeID type = value && value.isRecord() ? walker.typeOf(value) : TypeID();
        if (!type)
            return false;
        stores.keys.append(operation.key());
        stores.values.append(value);
        stores.types.append(type);
    }
    return true;
}

static Vector<uint32_t, 16> sortedKeysOfStoresInBirths(const StoresBehindAllocationInBirths& stores)
{
    Vector<uint32_t, 16> sorted;
    for (PropertyKeyID key : stores.keys)
        sorted.append(key.bits());
    std::ranges::sort(sorted);
    return sorted;
}

// ---- `new Node(kind, pos, end)`

// The record of the birth. What the head of a body stores, the body has stored wherever it returns, so a body of which nothing is listed has no such head.
static std::span<const PropertyKeyID> keysThatBodyFillsOfThisInBirths(const ThinSummary& summary)
{
    for (const SummaryConstruction& construction : summary.constructions) {
        if (construction.object == SummaryConstruction::Object::This && construction.numberOfKeysFilledAtEnd)
            return summary.keysFilledAtEnds.span().subspan(construction.firstKeyFilledAtEnd, construction.numberOfKeysFilledAtEnd);
    }
    return { };
}

// The entry block and the blocks that it only jumps on to.
static void collectStoresAtHeadOfBodyInBirths(Walker& walker, const BodyIndex& body, const Function& constructor, unsigned thisParameter, std::span<const Reference> handedOver, StoresBehindAllocationInBirths& stores)
{
    BodyBlock entry = body.block(entryBlock);
    if (entry.numberOfParameters() != handedOver.size())
        return;
    auto valueHandedOver = [&](ValueID root) -> Reference {
        std::optional<uint32_t> parameter = entry.indexOfParameter(root);
        if (!parameter || constructor.entryParameterKind(*parameter) != EntryParameter::Parameter)
            return { };
        return handedOver[*parameter];
    };
    for (BlockID current = entryBlock;;) {
        BodyBlock block = body.block(current);
        if (block.hasHandler() || !collectStoresInBirths(walker, body, block, 0, block.numberOfValues(), entry.parameter(thisParameter), scopedLambda<Reference(ValueID)>(valueHandedOver), stores))
            return;
        BodyTerminator end = block.terminator();
        if (end.kind() != TerminatorKind::Jump)
            return;
        BodyTarget next = end.next();
        if (next.numberOfIncomingValues() || body.numberOfIncomingEdges(next.block()) != 1 || body.isEnteredByHandlerEdge(next.block()))
            return;
        current = next.block();
    }
}

// What Walker::walkInline() asks of the operands before it builds anything.
static bool copyTakesOperandsInBirths(Walker& walker, FunctionID callee, const Function& constructor, std::span<const Reference> handedOver, unsigned thisParameter, TypeID typeOfThis)
{
    if (handedOver.size() != constructor.numberOfEntryParameters())
        return false;
    TypeArena& types = walker.types();
    const CallingConventions& conventions = walker.plan().conventions();
    for (unsigned index = 0; index < handedOver.size(); ++index) {
        EntryParameter kind = constructor.entryParameterKind(index);
        TypeID arriving = index == thisParameter ? typeOfThis : walker.typeOf(handedOver[index]);
        if (!arriving && (handedOver[index] || kind == EntryParameter::Callee))
            return false;
        const DeclaredType* declared = constructor.declaredEntryParameter(index);
        if (declared && declared->isEstablished() && (arriving || (kind != EntryParameter::Capture && kind != EntryParameter::ArgumentCount))) {
            TypeID established = walker.program().provenTypeOf(*declared);
            if (!arriving || !types.isSubset(arriving, established))
                return false;
        }
        TypeID tested = conventions.typeBehindGuard(callee, index);
        if (tested && (!arriving || !types.overlap(arriving, tested)))
            return false;
    }
    return true;
}

// What shouldOpenGuess() says of a test of a shape with a typed store behind it. It is asked about a value, and the object is not made yet.
static bool looksBehindAllocationPayInBirths(const Walker& walker)
{
    if (!walker.has(Mechanism::Guesses))
        return false;
    if (walker.isInLoop())
        return true;
    FunctionID function = walker.unit().function;
    return !walker.facts().runsAtMostOnce(function) && !walker.program().at(function).isModuleBody();
}

// As walkConstructOfFunctionInWalkCalls() has it, and by the type of what the copy returns. Null: no way leads on.
static Reference buildWhatConstructionGivesInBirths(Walker& walker, FunctionID callee, Reference returned, Reference object, Origin origin)
{
    TypeArena& types = walker.types();
    TypeID provenOfBody = walker.facts().linked().function(callee).result;
    TypeID typeOfReturned = walker.typeOf(returned);
    if ((provenOfBody && !types.overlap(provenOfBody, TypeTable::object)) || (typeOfReturned && !types.overlap(typeOfReturned, TypeTable::object)))
        return object;
    if (typeOfReturned && types.isSubset(typeOfReturned, TypeTable::object))
        return returned;

    Draft test(Opcode::TypeTest, origin);
    test.setValueOperand(returned);
    test.setTestedType(TypeTable::object);
    walker.chooseConservativeFields(test);
    Reference returnsObject = walker.build(WTF::move(test));
    if (!returnsObject)
        return { };
    BlockID takesReturned = walker.addBlock(BlockFrequency::Unlikely);
    BlockID takesThis = walker.addBlock();
    BlockID behind = walker.addBlock();
    Reference result = walker.addParameter(behind, { }, Representation::JSValue);
    walker.branch(returnsObject, takesReturned, takesThis);
    walker.continueIn(takesReturned);
    walker.jumpTo(behind, singleElementSpan(returned));
    walker.continueIn(takesThis);
    walker.jumpTo(behind, singleElementSpan(object));
    walker.continueIn(behind);
    return result;
}

// `function Node(kind, pos, end) { this.pos = pos; this.end = end; this.kind = kind; this.id = 0; .. }`. An assignment to a cell that has no property yet creates the property unless something above the cell takes the store.
// Whether anything does is looked at BEHIND the allocation: nobody has the cell yet, so where a look fails it is dropped, and the construction as written makes its own.
bool listStoresOfCopiedConstructorOfItsOpcodes(Walker& walker, const BodyValue& call)
{
    if (!walker.has(Mechanism::Inlining) || walker.isUnreachable())
        return false;
    auto operation = call.as<Opcode::Construct>();
    if (operation.newTargetOperand() && operation.newTargetOperand() != operation.calleeOperand())
        return false;
    Reference closure = walker.referenceOf(operation.calleeOperand());
    FunctionID callee = closure ? findOnlyCallee(walker, call, closure) : FunctionID();
    if (!callee || !makesThisOfConstruction(walker, callee))
        return false;

    const Program& program = walker.program();
    const Function& constructor = program.at(callee);
    const BodyIndex* body = walker.facts().bodies().find(callee);
    std::optional<unsigned> thisParameter = constructor.thisParameterIndex();
    if (!body || !thisParameter || constructor.isHiddenInStackTraces() || constructor.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest }))
        return false;
    std::span<const PropertyKeyID> keysFilledByBody = keysThatBodyFillsOfThisInBirths(walker.facts().summaries().at(callee));
    if (keysFilledByBody.empty())
        return false;
    LayoutID constructed = constructor.constructedClass;
    if (!chooseLayoutByTagAtConstruction(walker, call, callee, constructed))
        return false;

    Draft written = walker.draftOf(call);
    if (walker.hasFailed() || written.resultIsReturnedAtOnce)
        return false;
    Origin origin = written.origin();
    origin.isTailCall = false;
    Vector<Reference, 8> handedOver = operandsHandedToCopy(written, constructor);

    // Asked in front of the look at the body: only a body that is copied here is read here. What is not made yet is no closure of the program.
    Reference standIn = walker.constant(Draft(Opcode::Undefined, origin));
    Vector<Reference, 8> operandsAskedAbout;
    for (Reference operand : handedOver)
        operandsAskedAbout.append(operand ? operand : standIn);
    Draft site(Opcode::CallDirect, origin);
    site.setFunction(callee);
    site.setIsConstruction(true);
    site.setVariadicOperands(operandsAskedAbout.span());
    if (!shouldCopyCallee(walker, site, callee, operandsAskedAbout.span()))
        return false;

    StoresBehindAllocationInBirths stores;
    collectStoresAtHeadOfBodyInBirths(walker, *body, constructor, *thisParameter, handedOver.span(), stores);
    if (stores.keys.isEmpty() || !copyTakesOperandsInBirths(walker, callee, constructor, handedOver.span(), *thisParameter, walker.types().forLayout(constructed)))
        return false;

    // The stores further on in the body find the one look passed, and ask for none of their own.
    StoresListedAtBirth listed = findStoresThatAllocationMayList(walker, constructed, stores.keys.span(), stores.types.span(), keysFilledByBody);
    // ALL OR NOTHING. A slot that is held to a type ends the list at the first value that is not proven to fit, and the looks behind the allocation cost what two or three listed stores save:
    // `new Sym(open, name)` with `name` unproven listed one store of four: 3,468 > 3,507 M for 20 M constructions. Whole, the object is also born finished.
    if (listed.fields.size() < stores.keys.size())
        return false;
    if ((listed.asksForLayoutPrototype || !listed.conditions.isEmpty()) && !looksBehindAllocationPayInBirths(walker))
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(call.id()));
    uint32_t place = call.id().index() - block.firstValue().index();

    Draft read(Opcode::GetConstructPrototype, origin);
    read.setNewTargetOperand(closure);
    read.setLayout(declaredLayoutOf(program, constructed));
    walker.chooseConservativeFields(read);
    // As in walkConstructOfFunctionInWalkCalls(): `prototype` of a closure of one plain function is a data property of its own.
    Effects ofRead = declaredBoundOf(Opcode::NewObject);
    ofRead.reads.add(AbstractHeap::NamedProperties);
    Reference prototype = walker.build(WTF::move(read), &ofRead);
    if (!prototype)
        return true;
    Draft allocation(Opcode::NewObject, origin);
    allocation.standsForNoValueOfInput = true;
    allocation.setLayout(constructed);
    allocation.setPrototypeOperand(prototype);
    allocation.setFields(listed.fields.span());
    allocation.setValuesOperands(stores.values.span().first(listed.fields.size()));
    walker.chooseConservativeFields(allocation);
    Reference object = walker.hasFailed() ? Reference() : walker.build(WTF::move(allocation));
    if (!object)
        return true;
    noteFreshObject(walker, object, listed.fields.size());

    BlockID closedBlock;
    auto goOnWhereLookPasses = [&](Reference look) {
        BlockID passed = walker.addBlock();
        if (!closedBlock)
            closedBlock = walker.addBlock(BlockFrequency::Rare);
        walker.branch(look, passed, closedBlock);
        walker.continueIn(passed);
    };
    if (listed.asksForLayoutPrototype) {
        Draft look(Opcode::HasLayoutPrototype, origin);
        look.setObjectOperand(object);
        Reference passes = walker.build(WTF::move(look));
        if (!passes)
            return true;
        goOnWhereLookPasses(passes);
        walker.state().noteFactsOfCell(object, FactOfCell::HasLayoutPrototype);
    }
    if (!listed.conditions.isEmpty()) {
        Draft look(Opcode::AreMembersUnchanged, origin);
        look.setConditions(listed.conditions.span());
        Reference passes = walker.build(WTF::move(look));
        if (!passes)
            return true;
        goOnWhereLookPasses(passes);
        for (const MemberConditionRecord& condition : listed.conditions)
            walker.state().noteStoresPassAbove(condition.layout, condition.key, passes);
    }

    Vector<ArmOfBirth, 2> arms;
    auto endArm = [&](Reference value) {
        if (!walker.isUnreachable() && !walker.hasFailed())
            arms.append(ArmOfBirth { walker.currentBlock(), value });
    };
    auto walkAsWritten = [&] {
        Walker::ClosedArm closedArm(walker);
        walker.walkStretch(block.id(), place, place + 1);
        endArm(walker.isUnreachable() ? Reference() : walker.referenceOf(call.id()));
    };

    BlockID typedBlock = walker.currentBlock();
    if (closedBlock) {
        walker.noteGuessOpened();
        walker.continueIn(closedBlock);
        walkAsWritten();
        if (walker.hasFailed())
            return true;
        walker.continueIn(typedBlock);
    }

    handedOver[*thisParameter] = object;
    if (walker.walkInline(call, callee, handedOver.span())) {
        Reference returned = walker.isUnreachable() ? Reference() : walker.referenceBoundTo(call.id());
        if (returned && listed.isBornUnfinished && lookMayFinishBirthOf(walker, object)) {
            Draft finish(Opcode::TryFinishBirth, origin);
            finish.setObjectOperand(object);
            walker.chooseConservativeFields(finish);
            walker.build(WTF::move(finish));
        }
        endArm(returned ? buildWhatConstructionGivesInBirths(walker, callee, returned, object, origin) : Reference());
    } else
        walkAsWritten();
    if (walker.hasFailed())
        return true;

    if (!closedBlock) {
        if (arms.size() == 1 && arms[0].value)
            walker.bind(call.id(), arms[0].value);
        return true;
    }

    BlockID meeting = walker.addBlock();
    Reference met;
    if (arms.size() == 2 && arms[0].value && arms[1].value) {
        const Code& code = walker.code();
        TypeID type = joinOf(walker.types(), walker.facts().families(), code.at(arms[0].value.id()).type, code.at(arms[1].value.id()).type);
        Representation forms[] { walker.representationOf(arms[0].value), walker.representationOf(arms[1].value) };
        met = walker.addParameter(meeting, type, walker.formOfParameter(type, forms));
    }
    for (const ArmOfBirth& arm : arms) {
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
        walker.bind(call.id(), met);
    else if (arms.size() == 1 && arms[0].value)
        walker.bind(call.id(), arms[0].value);
    return true;
}

// ---- `const o = { a }; o.b = b; o.c = 0;`

// Above the object of a literal stands Object.prototype alone, and a cell that is handed no prototype stands below that of its layout: nothing is looked at, so there is one way.
bool listStoresBehindLiteralOfItsOpcodes(Walker& walker, const BodyValue& allocation)
{
    auto asWritten = allocation.as<Opcode::NewObject>();
    const Program& program = walker.program();
    LayoutID layout = asWritten.layout();
    if (asWritten.prototypeOperand() || !layout || program.at(layout).kind != LayoutKind::Record || walker.isUnreachable())
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(walker.inputBlockBeingWalked());
    if (!block.holds(allocation.id()) || allocation.id().index() < block.firstValue().index())
        return false;

    uint32_t place = allocation.id().index() - block.firstValue().index();
    uint32_t end = std::min(block.numberOfValues(), walker.endOfStretchBeingWalked());
    StoresBehindAllocationInBirths stores;
    auto valueBuiltInFront = [&](ValueID root) -> Reference {
        return root.index() < allocation.id().index() ? walker.referenceOf(root) : Reference();
    };
    collectStoresInBirths(walker, index, block, place + 1, end, allocation.id(), scopedLambda<Reference(ValueID)>(valueBuiltInFront), stores);
    if (stores.keys.isEmpty())
        return false;

    Draft draft = walker.draftOf(allocation);
    if (walker.hasFailed())
        return false;
    size_t numberWritten = draft.fields().size();
    if (numberWritten != draft.numberOfVariadicOperands())
        return false;
    StoresBehindAllocationInBirths all;
    Vector<uint32_t, 16> keysOfStores = sortedKeysOfStoresInBirths(stores);
    for (size_t position = 0; position < numberWritten; ++position) {
        PropertyKeyID key = walker.plan().slots().keyOf(program, FieldReference { layout, draft.fields()[position] });
        Reference value = draft.variadicOperands()[position];
        TypeID type = value && value.isRecord() ? walker.typeOf(value) : TypeID();
        // A store over what the literal lists: the object is left as it is written.
        if (!key || !type || std::ranges::binary_search(keysOfStores, key.bits()))
            return false;
        all.keys.append(key);
        all.values.append(value);
        all.types.append(type);
    }
    all.keys.appendVector(stores.keys);
    all.values.appendVector(stores.values);
    all.types.appendVector(stores.types);

    StoresListedAtBirth listed = findStoresThatAllocationMayList(walker, layout, all.keys.span(), all.types.span(), { });
    if (listed.fields.size() <= numberWritten || !listed.conditions.isEmpty() || !std::ranges::equal(listed.fields.span().first(numberWritten), draft.fields()))
        return false;

    draft.setFields(listed.fields.span());
    draft.setValuesOperands(all.values.span().first(listed.fields.size()));
    walker.chooseConservativeFields(draft);
    Reference object = walker.hasFailed() ? Reference() : walker.build(WTF::move(draft));
    if (!object)
        return true;
    walker.bind(allocation.id(), object);
    noteFreshObject(walker, object, listed.fields.size());
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
