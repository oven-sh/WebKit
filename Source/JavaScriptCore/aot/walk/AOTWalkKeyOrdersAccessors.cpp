/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Guesses and DirectCalls, an accessor in a slot: `{ get total() { .. } }.total`. The slot holds the pair of functions, and whichever lies there is the one that the access would call: behind a look whether the slot is filled
// the half is loaded and called, directly where it is the function that Link has seen laid there. An empty slot, or another shape: the access as written. Nothing is kept past the access. Withheld: the access as written.
// DirectCalls, where Link has proven the half (LinkedAccessorsOfField::getterIsProven): the access is the call, with no look, no load and no other arm; a function that needs its closure loads it behind the look and tests nothing.
// Folding, a lookup past own names: `f.cache`, `"x" in f`, `list.nope`, where the object is proven or seen to have only the names it was made with, and the table of built-ins shows that nothing above it answers.
// TypedSlots, a store that creates a property which nobody lists: a plain store.

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

// Closed, such an access is a lookup and a call out of a stub, which saves every register round it.
static constexpr uint32_t closedOperationsOfAccessThatCallsAccessorInKeyOrdersAccessors = 2;

// ---- A store that creates a property which nobody lists

bool creatingStoreListsNoKey(const Walker& walker, std::span<const LayoutID> receivers, PropertyKeyID key)
{
    const Plan& plan = walker.plan();
    return !receivers.empty() && std::ranges::all_of(receivers, [&](LayoutID receiver) {
        // Storage out of line is made by the store that adds.
        std::optional<FieldIndex> field = plan.layout(receiver).keysAreNeverEnumerated ? plan.slots().findField(walker.program(), receiver, key) : std::nullopt;
        return field && plan.slot(receiver, *field).hasStorage() && !plan.slot(receiver, *field).isOutOfLine();
    });
}

// ---- An accessor in a slot

static LayoutID onlyLayoutOfInKeyOrdersAccessors(const Walker& walker, TypeID listed)
{
    if (!listed)
        return { };
    Type type = walker.types().at(listed);
    if (!type.isJavaScriptValue() || type.primitives || !type.hasOnlyLayouts() || type.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow) || !holdsOneMember(type.layouts))
        return { };
    return singleLayoutOf(walker.types(), type);
}

// The field of the layout whose slot holds the functions of an accessor under the key.
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

// What Link has seen laid there, where this unit may name it. A guess: a test stands in front of whatever is built on it.
static FunctionID findAccessorExpectedInKeyOrdersAccessors(const Walker& walker, FieldReference field, bool isStore)
{
    const LinkTables& linked = walker.facts().linked();
    const LinkedAccessorsOfField* expected = linked.findAccessorsExpectedIn(field);
    FunctionID accessor = expected ? (isStore ? expected->setter : expected->getter) : FunctionID();
    return accessor && linked.mayBeMentionedBy(walker.unit().function, accessor) ? accessor : FunctionID();
}

// LinkedAccessorsOfField::getterIsProven, setterIsProven.
static bool isProvenHalfInKeyOrdersAccessors(const Walker& walker, FieldReference field, bool isStore)
{
    const LinkedAccessorsOfField* laid = walker.facts().linked().findAccessorsExpectedIn(field);
    return laid && (isStore ? laid->setterIsProven : laid->getterIsProven);
}

// Off an object that is proven to be of the layout the access IS a call of the one function, with the object as `this`. Nothing is asked of the object, and there is no other arm.
// A function that needs its closure finds it in the slot, which the run time fills object by object: callAccessorInSlot().
bool callProvenAccessorOfItsOpcodes(Walker& walker, const BodyValue& access)
{
    bool isStore = access.opcode() == Opcode::PropSet;
    if (access.isFreeName())
        return false;
    // Nearly every name says no here.
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
    if (!field || !isProvenHalfInKeyOrdersAccessors(walker, *field, isStore))
        return false;
    FunctionID half = findAccessorExpectedInKeyOrdersAccessors(walker, *field, isStore);
    return half && callAccessor(walker, access, half, object, stored);
}

FunctionID findGetterExpectedInSlot(Walker& walker, TypeID lookedAt, PropertyKeyID key)
{
    if (!key || liesInThread(key) || !walker.facts().linked().keysOfAccessorsExpectedInFields.get(key.index()))
        return { };
    if (!walker.has(Mechanism::Guesses) || !walker.has(Mechanism::DirectCalls) || !walker.has(Mechanism::TypedOperations))
        return { };
    std::optional<FieldReference> field = findAccessorSlotInKeyOrdersAccessors(walker, onlyLayoutOfInKeyOrdersAccessors(walker, lookedAt), key);
    return field ? findAccessorExpectedInKeyOrdersAccessors(walker, *field, false) : FunctionID();
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
    // Nearly every name says no here.
    if (PropertyKeyID name = access.key(); !name || liesInThread(name) || !walker.facts().linked().keysOfAccessorsExpectedInFields.get(name.index()))
        return false;
    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !walker.has(Mechanism::DirectCalls) || !walker.has(Mechanism::TypedOperations))
        return false;
    Reference object = walker.referenceOf(access.objectOperand());
    Reference stored = isStore ? walker.referenceOf(access.valueOperand()) : Reference();
    if (!object || !object.isValue() || (isStore && !stored))
        return false;

    // The receiver is proven to be of the one layout, or declared to be.
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
    // Off a proven receiver callProvenAccessor() has had its turn: the function needs its closure.
    bool halfIsProven = field && isProvenHalfInKeyOrdersAccessors(walker, *field, isStore);
    FunctionID provenHalf = halfIsProven ? findAccessorExpectedInKeyOrdersAccessors(walker, *field, isStore) : FunctionID();
    if (!field || !shouldOpenGuess(walker, object, types.forLayout(layout), true, closedOperationsOfAccessThatCallsAccessorInKeyOrdersAccessors))
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(access.id()));
    uint32_t place = access.id().index() - block.firstValue().index();
    Origin origin = walker.originOf(access);
    origin.isTailCall = false;

    BlockID asWritten = walker.addBlock(BlockFrequency::Rare);
    BlockID meeting = walker.addBlock();
    walker.noteGuessOpened();

    struct Arm {
        BlockID last;
        Reference value;
    };
    Vector<Arm, 3> arms;
    auto endArm = [&](Reference value) {
        if (!walker.isUnreachable() && !walker.hasFailed())
            arms.append(Arm { walker.currentBlock(), value });
    };
    auto goOnWhereTestPasses = [&](Reference test, BlockID whereTestFails) {
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
    // Whatever function lies there is the one that the access would call, with the object as `this`.
    auto callByValue = [&](Reference half) {
        Draft call(Opcode::Call, origin);
        if (!isStore)
            call.inputValueGiven = access.id();
        call.setCalleeOperand(half);
        call.setThisValueOperand(object);
        if (isStore)
            call.setArgumentsOperands(singleElementSpan(stored));
        endArm(walker.build(WTF::move(call)));
    };

    bool goesOn = true;
    if (!layoutIsProven) {
        Draft test(Opcode::ShapeTest, origin);
        test.setObjectOperand(object);
        test.setLayouts(types.at(types.forLayout(layout)).layouts);
        Reference hasShape = walker.build(WTF::move(test));
        goesOn = goOnWhereTestPasses(hasShape, asWritten);
        if (goesOn) {
            object = buildProof(object, hasShape, OperandIndex::ShapeTest::object);
            goesOn = !!object;
            // Behind the test of the shape the half is proven as well.
            if (goesOn && provenHalf && callAccessor(walker, access, provenHalf, object, stored)) {
                endArm(isStore || walker.isUnreachable() ? Reference() : walker.referenceOf(access.id()));
                goesOn = false;
            }
        }
    }
    if (goesOn) {
        Draft look(Opcode::IsSlotFilled, origin);
        fillSlotOperationInKeyOrdersAccessors(walker, look, object, *field, key);
        goesOn = goOnWhereTestPasses(walker.build(WTF::move(look)), asWritten);
    }
    Reference half;
    if (goesOn) {
        Draft load(isStore ? Opcode::GetSlotSetter : Opcode::GetSlotGetter, origin);
        fillSlotOperationInKeyOrdersAccessors(walker, load, object, *field, key);
        if (isStore)
            load.setIsInStrictCode(access.mode() == ECMAMode::Strict);
        else {
            // The row types the getter by what the property gives, which is any value, and a cell is no form of that.
            if (!halfIsProven)
                load.representation = Representation::JSValue;
        }
        half = walker.build(WTF::move(load));
    }
    if (walker.hasFailed())
        return true;

    // The function needs its closure: what the filled slot holds is one.
    if (half && provenHalf) {
        if (callAccessor(walker, access, provenHalf, object, stored, half))
            endArm(isStore || walker.isUnreachable() ? Reference() : walker.referenceOf(access.id()));
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
            Reference closure = goOnWhereTestPasses(isExpected, otherFunction) ? buildProof(half, isExpected, OperandIndex::TypeTest::value) : Reference();
            if (closure && callAccessor(walker, access, expected, object, stored, closure))
                endArm(isStore || walker.isUnreachable() ? Reference() : walker.referenceOf(access.id()));
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

    walker.continueIn(asWritten);
    if (!walker.isUnreachable()) {
        Walker::ClosedArm closedArm(walker);
        walker.walkStretch(block.id(), place, place + 1);
        endArm(isStore || walker.isUnreachable() ? Reference() : walker.referenceOf(access.id()));
    }
    if (walker.hasFailed())
        return true;

    Reference met;
    bool everyArmGivesValue = !isStore && std::ranges::all_of(arms, [](const Arm& arm) {
        return !!arm.value;
    });
    if (everyArmGivesValue && arms.size() > 1) {
        const Code& code = walker.code();
        TypeID type = code.at(arms[0].value.id()).type;
        Vector<Representation, 3> forms;
        for (const Arm& arm : arms) {
            type = joinOf(types, walker.facts().families(), type, code.at(arm.value.id()).type);
            forms.append(walker.representationOf(arm.value));
        }
        met = walker.addParameter(meeting, type, walker.formOfParameter(type, forms.span()));
    }
    for (const Arm& arm : arms) {
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
        walker.bind(access.id(), met);
    else if (everyArmGivesValue && arms.size() == 1)
        walker.bind(access.id(), arms[0].value);
    return true;
}

// ---- A static field in its variable

// `class Meter { static raw = 0 }`. While the class object is not made, a static field that has been defined lies in the variable that the record of the class names for it (object/AOTStaticFields.h).
// No proof: the read asks the run time's own two questions at run time, and where either says no the access goes as it is written. What has to hold of the text: the receiver is THE class object of the class
// that declares the field (the variables belong to the class object of the one evaluation), and the name is none that a function has of itself.
static VariableID findVariableOfStaticFieldInKeyOrdersAccessors(Walker& walker, TypeID receiver, PropertyKeyID key)
{
    const Program& program = walker.program();
    if (!receiver || !key || liesInThread(key) || program.at(key).kind != PropertyKeyKind::Text)
        return { };
    TypeArena& types = walker.types();
    Type type = types.at(receiver);
    // Nearly every receiver says no here.
    if (!type.isObject() || type.primitives || !type.hasOnlyClosures())
        return { };
    FunctionID constructor = singleFunctionOf(types, type);
    LayoutID classLayout = constructor ? classOfClassObject(program, constructor) : LayoutID();
    if (!classLayout || !walker.facts().isEvaluatedAtMostOnce(classLayout) || !walker.plan().slots().classCreation(classLayout).createsClassObjectOnMutation)
        return { };
    const Layout& record = program.at(classLayout);
    std::optional<uint32_t> field = record.findStaticField(key);
    // One under a computed key answers to a name that nobody knows.
    if (!field || std::ranges::any_of(record.staticFields, [](const StaticField& other) { return !other.key; }))
        return { };
    if (classHolderIsMadeWithName(program, ClassSide::Static, key))
        return { };
    VariableID variable = record.staticFields[*field].variable;
    PlannedVariable planned = walker.plan().variable(variable);
    if (!planned.hasWordInStatics() || planned.encoding != VariableEncoding::Plain || planned.representation != SlotRepresentation::JSValue)
        return { };
    return variable;
}

bool accessStaticFieldInVariable(Walker& walker, const BodyValue& access)
{
    Opcode opcode = access.opcode();
    bool isStore = opcode == Opcode::PropSet;
    if ((opcode != Opcode::PropGet && !isStore) || access.isFreeName())
        return false;
    // Nearly every name says no here.
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
    if (!variable || !shouldOpenGuess(walker, object, walker.typeOf(object), true, 1))
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(access.id()));
    uint32_t place = access.id().index() - block.firstValue().index();
    Origin origin = walker.originOf(access);
    origin.isTailCall = false;

    Draft read(Opcode::GetStaticFieldOrAbsent, origin);
    read.setVariable(variable);
    read.setValueType(types.withPrimitives(TypeTable::anything, Primitive::Absent));
    read.setClassObjectOperand(object);
    Reference held = walker.build(WTF::move(read));
    if (!held || walker.hasFailed())
        return true;
    Draft test(Opcode::TypeTest, origin);
    test.setValueOperand(held);
    test.setTestedType(TypeArena::fromPrimitives(Primitive::Absent));
    Reference isAbsent = walker.build(WTF::move(test));
    if (!isAbsent || walker.hasFailed())
        return true;

    BlockID asWritten = walker.addBlock(BlockFrequency::Rare);
    BlockID liesInVariable = walker.addBlock();
    BlockID meeting = walker.addBlock();
    walker.noteGuessOpened();
    walker.branch(isAbsent, asWritten, liesInVariable);

    struct Arm {
        BlockID last;
        Reference value;
    };
    Vector<Arm, 2> arms;
    auto endArm = [&](Reference value) {
        if (!walker.isUnreachable() && !walker.hasFailed())
            arms.append(Arm { walker.currentBlock(), value });
    };

    walker.continueIn(liesInVariable);
    if (!walker.isUnreachable()) {
        if (isStore) {
            Draft store(Opcode::PutStaticField, origin);
            store.setVariable(variable);
            store.setClassObjectOperand(object);
            store.setValueOperand(stored);
            walker.build(WTF::move(store));
            endArm({ });
        } else {
            Draft proof(Opcode::Narrowed, origin);
            proof.setValueOperand(held);
            proof.setTestOperand(isAbsent);
            proof.setOperandIndex(OperandIndex::TypeTest::value);
            proof.setIsOnTakenEdge(false);
            endArm(walker.build(WTF::move(proof)));
        }
    }
    if (walker.hasFailed())
        return true;

    walker.continueIn(asWritten);
    if (!walker.isUnreachable()) {
        Walker::ClosedArm closedArm(walker);
        walker.walkStretch(block.id(), place, place + 1);
        endArm(isStore || walker.isUnreachable() ? Reference() : walker.referenceOf(access.id()));
    }
    if (walker.hasFailed())
        return true;

    Reference met;
    bool everyArmGivesValue = !isStore && std::ranges::all_of(arms, [](const Arm& arm) {
        return !!arm.value;
    });
    if (everyArmGivesValue && arms.size() > 1) {
        const Code& code = walker.code();
        TypeID type = code.at(arms[0].value.id()).type;
        Vector<Representation, 2> forms;
        for (const Arm& arm : arms) {
            type = joinOf(types, walker.facts().families(), type, code.at(arm.value.id()).type);
            forms.append(walker.representationOf(arm.value));
        }
        met = walker.addParameter(meeting, type, walker.formOfParameter(type, forms.span()));
    }
    for (const Arm& arm : arms) {
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
        walker.bind(access.id(), met);
    else if (everyArmGivesValue && arms.size() == 1)
        walker.bind(access.id(), arms[0].value);
    return true;
}

// ---- A lookup past own names

// From `start` upward the table lists no member under the key. Each object that the lookup passes by can never get the key, and stays below what it stands below, up to the one that stands below nothing.
static bool nothingAnswersFromInKeyOrdersAccessors(const Program& program, BuiltinID start, PropertyKeyID key)
{
    const BuiltinTable& table = program.builtins();
    MembersUnderKey members(program, key);
    BuiltinID holder = table.canonicalID(start);
    while (true) {
        // Compiled code makes the object of such a module, and each module has its own of an object of that scope: the table speaks of neither.
        if (compiledModuleWithObject(program, holder) || table.at(table.rootOf(holder)).scope != BuiltinScope::Realm)
            return false;
        if (members.findOwnMember(holder) || !listsEveryMemberUnder(program, holder, key) || !members.isClosedKey(holder) || !table.hasLockedPrototypeLink(holder))
            return false;
        const Builtin& object = table.at(holder);
        switch (object.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return false;
        case BuiltinPrototypeLink::Null:
            return true;
        case BuiltinPrototypeLink::Builtin:
            holder = table.canonicalID(object.inheritsFrom);
            break;
        }
    }
}

// The closures of the one function stay as they are made: nobody stores into one, defines on one, deletes off one or lays one below something else, and every caller is known, so none has left the program.
// What a function object has of its own by its kind is not lacking.
static bool closuresLackNameInKeyOrdersAccessors(const Walker& walker, const Type& type, PropertyKeyID key)
{
    const Program& program = walker.program();
    const LinkTables& linked = walker.facts().linked();
    FunctionID function = singleFunctionOf(walker.types(), type);
    if (!function || function.index() >= linked.functions.size() || !linked.function(function).closuresStayAsMade)
        return false;
    // A coroutine's function object stands below a prototype of its own kind.
    const Function& record = program.at(function);
    if (record.isCoroutine() || record.constructedClass)
        return false;
    for (BuiltinName own : { BuiltinName(BuiltinNames::name), BuiltinName(BuiltinNames::length), BuiltinName(BuiltinNames::prototype), BuiltinName(BuiltinNames::arguments), BuiltinName(BuiltinNames::caller) }) {
        if (program.keyOf(own) == key)
            return false;
    }
    return nothingAnswersFromInKeyOrdersAccessors(program, Builtins::FunctionPrototype, key);
}

// The state has seen the object as its class makes it: it has no name of its own but those that the class gives it, and stands below the prototypes that it was made below.
static bool objectAsMadeLacksNameInKeyOrdersAccessors(Walker& walker, Reference object, const Type& type, PropertyKeyID key)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    const ClassPart* part = singleBuiltinClassOf(walker.types(), type);
    if (!part || !table.mayHavePristineInstances(part->builtinClass) || !isListAsMade(walker, object))
        return false;
    BuiltinName name = program.builtinNameOf(key);
    if (name && table.isRedefinedBelow(part->builtinClass, name))
        return false;
    // `error.name + error.stack`: what a constructor lays on each object that it makes is listed with the class, and not with the prototype.
    if (table.mayDefineOnNewInstances(part->builtinClass, name))
        return false;
    BuiltinID prototype;
    for (BuiltinClassID current = part->builtinClass; current && !prototype; current = table.at(current).parent)
        prototype = table.at(current).prototype;
    // What the class gives each instance is listed with the prototype, so a name of that kind counts as found.
    return prototype && nothingAnswersFromInKeyOrdersAccessors(program, prototype, key);
}

bool foldLookupPastOwnNames(Walker& walker, const BodyValue& access)
{
    Opcode opcode = access.opcode();
    if ((opcode != Opcode::PropGet && opcode != Opcode::PropHas) || !walker.has(Mechanism::Folding) || !walker.has(Mechanism::TypedOperations))
        return false;
    if (opcode == Opcode::PropGet && access.isFreeName())
        return false;
    const Program& program = walker.program();
    PropertyKeyID key = access.key();
    if (!key || liesInThread(key) || program.at(key).kind != PropertyKeyKind::Text || program.isIndexLike(key) || program.nameClassOf(key) == NameClass::Proto)
        return false;
    Reference object = walker.referenceOf(access.objectOperand());
    TypeID proven = object ? walker.typeOf(object) : TypeID();
    if (!proven)
        return false;
    Type type = walker.types().at(proven);
    if (!type.isObject() || !type.isJavaScriptValue())
        return false;
    if (!closuresLackNameInKeyOrdersAccessors(walker, type, key) && !objectAsMadeLacksNameInKeyOrdersAccessors(walker, object, type, key))
        return false;

    Draft answer = walker.draftOf(access);
    if (walker.hasFailed())
        return true;
    answer.become(opcode == Opcode::PropGet ? Opcode::Undefined : Opcode::Boolean);
    if (opcode == Opcode::PropHas)
        answer.setBoolean(false);
    walker.bind(access.id(), walker.constant(WTF::move(answer)));
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
