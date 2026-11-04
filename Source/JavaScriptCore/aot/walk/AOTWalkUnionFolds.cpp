/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Three small answers: reads of a template object in sight, the look at a heritage that can only be a class, and the filler that keeps an allocation listed round one value that does not fit its slot.
// At a suspension and at a throw: nothing. What is built is the operation as written under narrower effects, or a constant.
// Withheld: the reads with Folding, the look with TypedOperations. The filler is asked for wherever a slot has a form that not every value fits, which is with TypedSlots.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTSlotPlan.h"
#include "AOTSlotRepresentation.h"
#include "AOTTypeArena.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet readsOfTemplateObjectsInUnionFolds { Opcode::PropGet, Opcode::ElemGet };

// The operation runs no code of the program and throws nothing. The run time may make what it is asked for when it is first asked, and it files what it finds.
static Effects effectsOfLookThatRunsNoCodeInUnionFolds()
{
    Effects effects = Effects::unknownCode();
    effects.writes = effects.writes.restrictedTo(heapsWhoseWritesNobodySees);
    effects.mayThrow = ThrowEffect::onlyForLackOfMemory();
    effects.mayCallUnknownCode = false;
    effects.mayBeTerminated = false;
    return effects;
}

static bool buildAsWrittenWithoutCodeInUnionFolds(Walker& walker, const BodyValue& value)
{
    Draft written = walker.draftOf(value);
    walker.chooseConservativeFields(written);
    Effects effects = effectsOfLookThatRunsNoCodeInUnionFolds();
    if (Reference built = walker.build(WTF::move(written), &effects))
        walker.bind(value.id(), built);
    return true;
}

// ---- ``tag`a${x}b` `` with tag copied in line

// The object and its list of raw texts are frozen when they are made. So whoever else has seen the object, its texts are those of its source site.
static bool collectStringsOfTemplateObjectInSight(const Walker& walker, Reference object, Vector<TemplateString, 8>& strings)
{
    if (!object || !object.isRecord())
        return false;
    const Code& code = walker.code();
    ValueID root = object.id();
    while (opcodesThatNameTheirOperand.contains(code.at(root).opcode))
        root = code.at(root).operand(0);
    const Operation& allocation = code.at(root);
    if (allocation.opcode != Opcode::NewTemplateObject)
        return false;
    for (TemplateString string : allocation.templateStrings())
        strings.append(string);
    return true;
}

static Reference textInUnionFolds(Walker& walker, StringID text)
{
    Draft constant(Opcode::String);
    constant.setString(text);
    return walker.constant(WTF::move(constant));
}

// Each read is answered by itself, whoever else uses the object. An object that nothing uses any more is the sweep's.
bool foldReadOfTemplateObject(Walker& walker, const BodyValue& read)
{
    if (!readsOfTemplateObjectsInUnionFolds.contains(read.opcode()) || !walker.has(Mechanism::Folding))
        return false;
    const Program& program = walker.program();
    Vector<TemplateString, 8> strings;

    if (read.opcode() == Opcode::PropGet) {
        auto byName = read.as<Opcode::PropGet>();
        if (byName.isFreeName() || !collectStringsOfTemplateObjectInSight(walker, walker.referenceOf(byName.objectOperand()), strings))
            return false;
        if (byName.key() == program.keyOf(BuiltinNames::length)) {
            Draft count(Opcode::Integer);
            count.setInteger(static_cast<int64_t>(strings.size()));
            walker.bind(read.id(), walker.constant(WTF::move(count)));
            return true;
        }
        // No constant stands for the list of raw texts. The read finds a value of the object's own, so one that nobody needs is dropped.
        return byName.key() == program.keyOf(BuiltinNames::raw) && buildAsWrittenWithoutCodeInUnionFolds(walker, read);
    }

    auto byIndex = read.as<Opcode::ElemGet>();
    bool readsRawText = false;
    if (!collectStringsOfTemplateObjectInSight(walker, walker.referenceOf(byIndex.objectOperand()), strings)) {
        const BodyIndex& index = walker.currentBodyIndex();
        ValueID list = index.valueBehindCasts(byIndex.objectOperand());
        if (!list || index.isParameter(list) || index.opcodeOf(list) != Opcode::PropGet)
            return false;
        auto rawTexts = index.value(list).as<Opcode::PropGet>();
        if (rawTexts.isFreeName() || rawTexts.key() != program.keyOf(BuiltinNames::raw) || !collectStringsOfTemplateObjectInSight(walker, walker.referenceOf(rawTexts.objectOperand()), strings))
            return false;
        readsRawText = true;
    }

    // Behind the last text the read asks the prototypes.
    Reference position = walker.referenceOf(byIndex.propertyOperand());
    if (!position)
        return false;
    Type typeOfPosition = walker.types().at(walker.typeOf(position));
    if (!typeOfPosition.isInteger() || !typeOfPosition.integers.isSingleValue() || typeOfPosition.integers.minimum < 0 || static_cast<uint64_t>(typeOfPosition.integers.minimum) >= strings.size())
        return false;
    const TemplateString& string = strings[static_cast<size_t>(typeOfPosition.integers.minimum)];
    // A text with an escape that names no character has no cooked form.
    if (readsRawText || string.cooked)
        walker.bind(read.id(), textInUnionFolds(walker, readsRawText ? string.raw : string.cooked));
    else
        walker.bind(read.id(), walker.constant(Draft(Opcode::Undefined)));
    return true;
}

// ---- `class B extends A {}`

// Every value of the type is null, a class object or a constructor of the table whose `prototype` is a constant, so whoever drops the look drops nothing that the program sees.
// The first read of a member of the table may run code of the library to make it, or add it behind the keys that its holder has by then, which a program sees only where it lists those keys.
static bool lookAtHeritageNeitherThrowsNorRunsCode(const Walker& walker, const Type& heritage)
{
    if (!heritage.isJavaScriptValue() || (heritage.primitives & ~Primitive::Null) || !heritage.hasOnlyNamedObjects() || heritage.namesMergedObjects())
        return false;
    if (!(heritage.traitsOfNamed & traitsOfIdentity).containsOnly({ ObjectTrait::IsClosure, ObjectTrait::IsBuiltin }))
        return false;

    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    bool holdsOfEveryObject = true;
    forEachFunctionOf(walker.types(), heritage, [&](FunctionID constructor) {
        holdsOfEveryObject &= !!classOfClassObject(program, constructor);
    });
    forEachBuiltinOf(walker.types(), heritage, [&](BuiltinID constructor) {
        if (!holdsOfEveryObject)
            return;
        BuiltinID prototype = table.at(constructor).kind == BuiltinKind::Constructor ? findConstantMemberFrom(program, walker.facts(), constructor, program.keyOf(BuiltinNames::prototype)) : BuiltinID();
        holdsOfEveryObject = prototype && isObjectKind(table.at(table.canonicalID(prototype)).kind) && !isMadeWithCodeOfLibrary(table, prototype);
        if (holdsOfEveryObject && firstReadChangesListedKeys(table, prototype))
            holdsOfEveryObject = !walker.facts().listsKeysOf(table.canonicalID(table.at(prototype).holder));
    });
    return holdsOfEveryObject;
}

bool buildLookAtHeritage(Walker& walker, const BodyValue& value)
{
    if (value.opcode() != Opcode::ResolveHeritage || !walker.has(Mechanism::TypedOperations))
        return false;
    Reference heritage = walker.referenceOf(value.as<Opcode::ResolveHeritage>().heritageOperand());
    if (!heritage)
        return false;
    Type type = walker.types().at(walker.typeOf(heritage));
    // Of null, of one class object and of one constructor of the table buildTypedClassOperation() has the answer itself in the place of the look. It is asked behind this link.
    if (!type.hasObjects() || (type.isObject() && (singleFunctionOf(walker.types(), type) || singleBuiltinOf(walker.types(), type))))
        return false;
    if (!lookAtHeritageNeitherThrowsNorRunsCode(walker, type))
        return false;
    // A class object that the run time makes when it is first asked for gets its prototype object here at the latest.
    return buildAsWrittenWithoutCodeInUnionFolds(walker, value);
}

// ---- `{ id: 1, tag: maybe, more: 2 }`

// What an allocation lists in the place of a value that is not proven to fit its slot: the key is listed where it is written, and the value is defined by name behind the allocation, by the definition that tests it.
// Null: the form of the slot has no undefined, or no store can fill the slot behind the allocation. Then the allocation ends in front of the field.
Reference fillerForSlotInAllocation(Walker& walker, FieldReference field)
{
    const PlacedSlot& slot = walker.plan().slot(field.layout, field.field);
    if (!slot.hasStorage() || slot.holdsAccessor || slot.representation == SlotRepresentation::DeferredClosure)
        return { };
    if (!canLeaveSlot(slot.representation) || !traitsOf(slot.representation).hasUndefined)
        return { };
    return walker.constant(Draft(Opcode::Undefined));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
