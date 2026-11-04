/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTypes.h"
#include "AOTIRProgram.h"
#include "AOTResultRulesInternal.h"
#include "AOTTypeRelations.h"
#include "AOTVector.h"
#include <array>
#include <initializer_list>
#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OpcodeSet readsOutOfArrayStorageInResultRules { Opcode::GetArrayElement, Opcode::GetContiguousIndex, Opcode::GetOwnContiguousIndex };
inline constexpr OpcodeSet stepsOfCollectionsInResultRules { Opcode::MapNext, Opcode::SetNext };
inline constexpr OpcodeSet callsOfOverloadsInResultRules { Opcode::CallBuiltin, Opcode::CallInline };
inline constexpr OpcodeSet callsByValueInResultRules { Opcode::Call, Opcode::CallResolved, Opcode::CallList };
inline constexpr OpcodeSet callsOfBuiltinsByValueInResultRules { Opcode::Call, Opcode::Construct, Opcode::CallList, Opcode::ConstructList };
// What they give is what the callee hands back. AwaitCallBySavingFrame, a direct call, gives what the promise of its callee settles with.
inline constexpr OpcodeSet callsTypedBySummariesInResultRules = []() consteval {
    OpcodeSet opcodes;
    for (unsigned index = 0; index < numberOfEnumerators<Opcode>; ++index) {
        Opcode opcode = static_cast<Opcode>(index);
        if (accessClassOf(opcode) == AccessClass::IndirectCall && resultRuleOf(opcode) == ResultRule::Unknown)
            opcodes.add(opcode);
    }
    return opcodes;
}();
static_assert((callsTypedBySummariesInResultRules | callsByValueInResultRules | constructionsByValue) == callsTypedBySummariesInResultRules);
static_assert(!callsTypedBySummariesInResultRules.contains(Opcode::AwaitCallBySavingFrame));

static_assert(OperandIndex::Call::callee == OperandIndex::CallResolved::callee && OperandIndex::Call::callee == OperandIndex::CallList::callee);
static_assert(OperandIndex::CallBuiltin::receiver == OperandIndex::CallInline::receiver && OperandIndex::CallBuiltin::receiver == OperandIndex::TryCallInline::receiver);
static_assert(OperandIndex::CallBuiltin::lengthOfReceiver == OperandIndex::CallInline::lengthOfReceiver && OperandIndex::CallBuiltin::lengthOfReceiver == OperandIndex::TryCallInline::lengthOfReceiver);
static_assert(OperandIndex::CallBuiltin::arguments == OperandIndex::CallInline::arguments && OperandIndex::CallBuiltin::arguments == OperandIndex::TryCallInline::arguments);

// `known`: what PositionTypes answered. It is taken as it is, so it counts what the position holds before its first store. Behind it comes what holds of the position by construction.
inline TypeID typeHeldAtPositionInResultRules(ResultRules& rules, TypeID known, const DeclaredType& declared, UnknownSeed seed)
{
    if (known)
        return known;

    TypeID byConstruction = rules.context.typeIDOf(declared);
    bool maybeAbsent = byConstruction == TypeTable::anythingOrAbsent;
    if (byConstruction && byConstruction != TypeTable::anything && !maybeAbsent)
        return byConstruction;

    TypeID unknown = rules.provenOrUnknown({ }, declared.isEstablished() ? UnknownSeed::WrittenAsLoose : seed);
    return maybeAbsent ? rules.context.types.withPrimitives(unknown, Primitive::Absent) : unknown;
}

// A variable that Plan added has a number behind the program's own and no record. Its word may be empty.
inline TypeID typeHeldInVariableInResultRules(ResultRules& rules, TypeID known, VariableID variable, UnknownSeed seed)
{
    const Program& program = rules.context.program;
    if (variable.index() < program.variables.size())
        return typeHeldAtPositionInResultRules(rules, known, program.at(variable).declared, seed);
    if (known)
        return known;
    return rules.context.types.withPrimitives(rules.provenOrUnknown({ }, seed), Primitive::Absent);
}

inline bool describesObjectsAloneInResultRules(const NaturalType& written)
{
    return (written.isObject || written.isFunction || written.builtinClass) && written.primitives.isEmpty() && !written.integers && !written.opaque;
}

// The `prototype` of a class is read-only, so the class of the allocation itself always selects the layout that the allocation names.
// That another class lies below it is Facts' to prove, and for a class that does not the run time makes an ordinary object.
inline bool newTargetIsClassOfAllocationInResultRules(const TypingContext& context, LayoutID layout, TypeID newTarget)
{
    if (holdsOnlyPrimitives(newTarget))
        return false;
    Type type = context.at(newTarget);
    if (!type.isObject() || !type.hasOnlyClosures())
        return false;
    FunctionID only = singleFunctionOf(context.types, type);
    if (!only)
        return false;
    const Function& constructor = context.program.at(only);
    return isClassConstructorOf(constructor.kind) && constructor.constructedClass == layout;
}

// `newTarget`: null where the allocation names no prototype.
inline TypeID typeOfObjectAllocatedInResultRules(const TypingContext& context, LayoutID layout, ObjectID created, TypeID newTarget)
{
    if (!newTarget || !context.program.prototypeMaySelectOtherClass(layout))
        return typeOfAllocatedLayout(context, layout, created);

    switch (context.level) {
    case TypeLevel::Proven:
        if (newTargetIsClassOfAllocationInResultRules(context, layout, newTarget))
            return typeOfAllocatedLayout(context, layout, created);
        return context.types.object(UnknownSeed::ObjectWithoutLayout);
    case TypeLevel::Expected:
        break;
    }

    if (LayoutSetID selected = layoutsSelectedByNewTarget(context, layout, newTarget))
        return context.types.forLayoutSet(selected);
    Vector<LayoutID, 8> family { layout };
    family.append(context.program.layoutsExpectedBelow(layout));
    return context.types.forLayouts(family.span());
}

// Null: some entry has no such part, or there is no entry.
// What is added later is not what is written here: a string counts as any string, and each bound of the integers goes to the next landmark.
// So neither gets the type of the one value that typeOfData() would make for it. The iterator of the list reads a hole as undefined.
inline TypeID tryTypeOfDataEntryPartsInResultRules(const TypingContext& context, const DataNode::Array& entries, ContentsSlot slot)
{
    TypeArena& types = context.types;
    TypeParts widened;
    TypeID others = TypeTable::never;
    for (const DataNode& entry : entries.elements) {
        const DataNode* part = &entry;
        if (slot != ContentsSlot::Element) {
            const auto* pair = entry.tryAs<DataNode::Array>();
            if (!pair || pair->elements.size() != 2)
                return { };
            part = &pair->elements[slot == ContentsSlot::Key ? 0 : 1];
        }

        if (const auto* number = part->tryAs<DataNode::Number>()) {
            widened.primitives |= static_cast<uint16_t>(primitiveOfNumber(number->value));
            if (isTrackedInteger(number->value))
                widened.integers = widened.integers.unionWith(Range::one(static_cast<int64_t>(number->value)));
        } else if (part->is<DataNode::String>())
            widened.primitives |= Primitive::String;
        else if (part->is<DataNode::Hole>())
            widened.primitives |= Primitive::Undefined;
        else
            others = types.unionOf(others, typeOfData(context, *part));
    }

    if (!widened.integers.isEmpty())
        widened.integers = { widenedMinimum(widened.integers.minimum), widenedMaximum(widened.integers.maximum) };
    TypeID united = types.unionOf(types.intern(widened), others);
    return isNeverType(united) ? TypeID() : united;
}

inline TypeID typeOfCollectionFromDataInResultRules(const TypingContext& context, BuiltinClassID builtinClass, const DataNode& root)
{
    const auto* entries = root.tryAs<DataNode::Array>();
    if (!entries)
        return context.types.builtinClass(builtinClass);

    std::array<TypeID, mostContentsOfClass> contents;
    bool isAnythingWritten = false;
    for (ContentsSlot slot : { ContentsSlot::Element, ContentsSlot::Key, ContentsSlot::Value }) {
        std::optional<uint8_t> position = positionInTypeOf(context.types.builtins(), builtinClass, slot);
        if (!position || *position >= mostContentsOfClass)
            continue;
        contents[*position] = tryTypeOfDataEntryPartsInResultRules(context, *entries, slot);
        isAnythingWritten |= !!contents[*position];
    }
    if (!isAnythingWritten)
        return context.types.builtinClass(builtinClass);
    return context.types.builtinClass(builtinClass, std::span<const TypeID>(contents));
}

inline TypeID typeOfSlotReadInResultRules(ResultRules& rules, Opcode read, TypeID receiver, LayoutSetID receivers, PropertyKeyID key, bool holdsDeclaredType)
{
    const TypingContext& context = rules.context;
    bool defersClosure = read == Opcode::GetDeferredClosureSlot || read == Opcode::GetDeferredClosureSlotOrUndefined;
    if (read == Opcode::GetSlotGetter || read == Opcode::GetSlotSetter) {
        if (TypeID half = tryTypeOfProvenHalfInSlot(context, receivers, key, read == Opcode::GetSlotSetter))
            return half;
    }
    TypeID contents;
    if (defersClosure)
        contents = tryTypeOfDeferringSlotContents(context, receivers, key);
    else if (read == Opcode::GetSlotSetter)
        contents = tryTypeOfSetterInSlot(context, receivers, key);
    else
        contents = tryTypeOfSlotContents(context, receivers, key);

    if (!contents)
        return rules.provenOrUnknown({ }, seedOfUnknownResultOf(read));
    if (read != Opcode::GetSlotGetter && read != Opcode::GetSlotSetter)
        contents = typeOfSlotContentsInReceiver(context, contents, receiver, key);
    if (read == Opcode::GetSlotOrUndefined || read == Opcode::GetDeferredClosureSlotOrUndefined)
        return context.types.withPrimitives(contents, Primitive::Undefined);
    return holdsDeclaredType ? context.types.withoutNullish(contents) : contents;
}

// What `return` hands over in a coroutine is not what the caller of its ramp gets, and the other units make their own answer.
// A bound function hands back what its target hands back.
inline bool unitHandsBackResultsInResultRules(const Function& callee, CodeRole entered)
{
    if (callee.isCoroutine())
        return false;
    switch (entered) {
    case CodeRole::Body:
    case CodeRole::SlowBody:
    case CodeRole::ParameterGuard:
    case CodeRole::GenericCallEntry:
    case CodeRole::BoundFunction:
    case CodeRole::GenericEntryOfBoundFunction:
    case CodeRole::TableEntry:
        return true;
    case CodeRole::GenericConstructEntry:
    case CodeRole::AllocatingConstructEntry:
    case CodeRole::CoroutineBody:
    case CodeRole::ModuleStart:
    case CodeRole::TableLookup:
    case CodeRole::Serializer:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The ramp hands back the promise or the generator, and a bound function what the ramp of its target hands back. The start of a module returns undefined, whatever its body is.
inline bool unitHandsBackObjectOfCoroutineInResultRules(CodeRole entered)
{
    return entered == CodeRole::Body || entered == CodeRole::BoundFunction;
}

// How many results a direct call hands back is Plan's to say, so PositionTypes may know of one that the record of the function does not list.
inline TypeID typeOfResultHandedBackInResultRules(ResultRules& rules, FunctionID function, uint32_t resultIndex, UnknownSeed seed)
{
    const TypingContext& context = rules.context;
    if (TypeID known = context.positions.result(context.level, function, resultIndex))
        return known;
    const Function& callee = context.program.at(function);
    if (resultIndex >= callee.results.size())
        return rules.provenOrUnknown({ }, seed);
    return typeHeldAtPositionInResultRules(rules, { }, callee.results[resultIndex], seed);
}

// `new C(..)` and `super(..)`, where all that is constructed with are the closures of the constructor of one class that takes no `this`: its body makes the object, and returns what was constructed, whatever new.target is.
// A proof and never less. Null: nobody knows.
inline TypeID tryTypeConstructedByClassInResultRules(const TypingContext& context, TypeID callee)
{
    if (context.level != TypeLevel::Proven || !callee || holdsOnlyPrimitives(callee))
        return { };
    Type type = context.at(callee);
    FunctionID only = type.isObject() && type.isJavaScriptValue() ? singleFunctionOf(context.types, type) : FunctionID();
    if (!only)
        return { };
    const Function& constructor = context.program.at(only);
    if (!isClassConstructorOf(constructor.kind) || constructor.has(FunctionFlags::HasThis) || constructor.results.size() != 1)
        return { };
    return context.positions.result(context.level, only, 0);
}

inline bool holdsNoCalleeInResultRules(const TypingContext& context, TypeID callee)
{
    if (holdsOnlyPrimitives(callee))
        return true;
    Type type = context.at(callee);
    return !type.hasObjects() && type.isJavaScriptValue();
}

inline BuiltinTypeContext builtinTypeContextOfResultRules(const TypingContext& context)
{
    return { context.program, context.types, nullptr, &context.positions, context.level };
}

inline TypeID typeOfOverloadCallInResultRules(const TypingContext& context, BuiltinOverloadID overload, BuiltinCallForm form, OperandTypes operands)
{
    BuiltinOperandTypes passed;
    passed.receiver = operands[OperandIndex::CallBuiltin::receiver];
    for (uint32_t operandIndex = OperandIndex::CallBuiltin::arguments; operandIndex < operands.size(); ++operandIndex)
        passed.arguments.append(operands[operandIndex]);
    return resultTypeOfBuiltinCall(builtinTypeContextOfResultRules(context), { overload, form, passed, operands[OperandIndex::CallBuiltin::lengthOfReceiver] });
}

// A call or a construction by value of the one function object that the type of the callee names. It is that function whatever it is handed, so it gives what the rows give that these operands can reach.
// A call that passes no `this` passes undefined. The rows of a construction have the callee as new.target. Of arguments that come in a list nobody knows how many there are: every row of the mode may be reached.
// Null: the callee is no one built-in, or its rows say nothing.
inline TypeID tryTypeOfBuiltinCalledByValueInResultRules(const TypingContext& context, Opcode opcode, OperandTypes operands)
{
    static_assert(OperandIndex::Call::callee == OperandIndex::Construct::callee && OperandIndex::Call::arguments == OperandIndex::Construct::arguments);
    static_assert(OperandIndex::Call::thisValue == OperandIndex::Construct::newTarget);
    static_assert(OperandIndex::Call::callee == OperandIndex::ConstructList::callee && OperandIndex::Call::thisValue == OperandIndex::CallList::thisValue && OperandIndex::Call::thisValue == OperandIndex::ConstructList::newTarget);
    TypeID callee = operands[OperandIndex::Call::callee];
    if (!callee || holdsOnlyPrimitives(callee))
        return { };
    Type type = context.at(callee);
    BuiltinID builtin = type.isObject() ? singleBuiltinOf(context.types, type) : BuiltinID();
    if (!builtin || !isCallableBuiltin(context.program.builtins(), builtin))
        return { };

    BuiltinOperandTypes passed;
    TypeID besideCallee = operands[OperandIndex::Call::thisValue];
    bool constructs = constructionsByValue.contains(opcode);
    BuiltinCallMode mode = constructs ? BuiltinCallMode::Construct : BuiltinCallMode::Call;
    if (constructs && besideCallee && besideCallee != callee)
        return { };
    if (!constructs)
        passed.receiver = besideCallee ? besideCallee : TypeID(TypeTable::undefined);

    BuiltinTypeContext builtinContext = builtinTypeContextOfResultRules(context);
    TypeID written = TypeTable::never;
    if (opcode == Opcode::CallList || opcode == Opcode::ConstructList) {
        if (!rowsSpeakOfCallOnReceiver(builtinContext, builtin, passed.receiver))
            return { };
        const BuiltinTable& builtins = context.program.builtins();
        for (BuiltinOverloadID row : builtins.overloadIDsOf(builtins.canonicalID(builtin))) {
            if (builtins.at(row).mode == mode)
                written = context.types.unionOf(written, resultTypeOfBuiltinCall(builtinContext, { row, BuiltinCallForm::List, passed }));
        }
        if (isNeverType(written))
            return { };
    } else {
        for (uint32_t operandIndex = OperandIndex::Call::arguments; operandIndex < operands.size(); ++operandIndex) {
            if (!operands[operandIndex])
                return { };
            passed.arguments.append(operands[operandIndex]);
        }
        written = resultTypeOfReachableOverloads(builtinContext, builtin, mode, passed);
    }
    return context.at(written).isAnything() ? TypeID() : written;
}

// Of an operation whose operands do not stand at the positions of the parameters. Nothing holds native code to a result that is only declared: it is a guess.
inline TypeID tryTypeWrittenForOverloadInResultRules(const TypingContext& context, BuiltinOverloadID id)
{
    const BuiltinOverload& overload = context.program.builtins().at(id);
    if (overload.isUndeclared() || (overload.resultIsDeclaredOnly() && context.level == TypeLevel::Proven))
        return { };
    return typeOfBuiltinType(builtinTypeContextOfResultRules(context), overload.result.type);
}

inline ContentsSlot defaultContentsSlotInResultRules(const TypingContext& context, TypeID container)
{
    Type type = context.at(container);
    for (const ClassPart& part : classPartsOf(context.types, type)) {
        if (defaultContentsSlotOf(context.types.builtins(), part.builtinClass) != ContentsSlot::Element)
            return ContentsSlot::Value;
    }
    return ContentsSlot::Element;
}

// Null: the closure may be one that nobody has named, or nothing is known of the cell of one that it may be.
// The operation reads closures of the function alone, so the other objects of the type never come to it.
// The objects are listed first: a union may move the sets of the arena, and the view of the objects may point into them.
inline TypeID tryTypeOfCaptureOfNamedClosuresInResultRules(const TypingContext& context, TypeID closure, FunctionID function, uint32_t index)
{
    if (!closure || holdsOnlyPrimitives(closure))
        return { };
    Type type = context.at(closure);
    if (!type.hasOnlyNamedObjects() || type.namesMergedObjects())
        return { };

    Vector<ObjectID, 8> closuresOfFunction;
    context.types.at(type.objects).forEach([&](ObjectID object) {
        if (context.types.objects().closureFunction(object) == function)
            closuresOfFunction.append(object);
    });
    TypeID united;
    for (ObjectID object : closuresOfFunction) {
        TypeID held = context.positions.captureOfObject(context.level, object, index);
        if (!held)
            return { };
        united = united ? context.types.unionOf(united, held) : held;
    }
    return united;
}

static_assert(OperandIndex::CopyObject::source == OperandIndex::CopyObjectAndDefine::source && OperandIndex::CopyObject::source == OperandIndex::CopyTwoObjects::source);

// A copy that leaves out keys that are computed is made name by name, and is an ordinary object.
template<typename Operation>
bool mayCopyRecordByWordsInResultRules(const Operation& operation)
{
    UNUSED_PARAM(operation);
    if constexpr (HoldsOpcode<Operation>) {
        Opcode opcode = opcodeOf(operation);
        if (opcode == Opcode::CopyObjectAndDefine || opcode == Opcode::CopyTwoObjects)
            return true;
        // A body hands out its keys as a BodyList, a draft and a record as a span.
        return opcode == Opcode::CopyObject && operation.excludedOperands().empty();
    } else
        return false;
}

// `created` stands for the ordinary object that the copy makes. The copy of a record is a record, which its source stands for: KeepLayoutOfCopiedRecordRule of Facts says the same of the value.
// The members are listed first: what is made may move the sets of the arena, and the views may point into them.
inline TypeID typeOfObjectCopyInResultRules(const TypingContext& context, ObjectID created, TypeID source)
{
    TypeArena& types = context.types;
    TypeID made = types.forObject(created);
    if (source && holdsOnlyPrimitives(source))
        return made;

    Type type = types.at(source ? source : TypeTable::anything);
    Vector<ObjectID, 8> namedRecords;
    if (type.objects) {
        types.at(type.objects).forEach([&](ObjectID object) {
            if (mayHaveLayoutOfRecord(context.program, types.at(object)))
                namedRecords.append(object);
        });
    }
    Vector<LayoutID, 8> layoutsOfRecords;
    if (type.unnamedLayouts) {
        types.at(type.unnamedLayouts).forEach([&](LayoutID layout) {
            if (context.program.at(layout).kind == LayoutKind::Record)
                layoutsOfRecords.append(layout);
        });
    }

    TypeParts records;
    records.seeds = type.seeds;
    records.anyObject = type.anyObject || !type.isJavaScriptValue();
    if (!namedRecords.isEmpty())
        records.objects = types.objectSets().ofSorted(namedRecords.span());
    if (!layoutsOfRecords.isEmpty())
        records.unnamedLayouts = types.layoutSets().ofSorted(layoutsOfRecords.span());
    return types.unionOf(made, types.intern(records));
}

template<typename Operation>
TypeID ResultRules::natural(const Operation& operation, const NaturalType& written)
{
    // An operation that is refined by its operands may hand one of them back, so it need not make the object that `created` stands for.
    if (opcodesWithNaturalTypeRefinedByOperation().contains(opcodeOf(operation)))
        return refineFixedNatural(context, operation, operands, naturalTypeFrom(context.types, written));
    if (created && describesObjectsAloneInResultRules(written)) {
        if (mayCopyRecordByWordsInResultRules(operation))
            return typeOfObjectCopyInResultRules(context, created, operands[OperandIndex::CopyObject::source]);
        return context.types.forObject(created);
    }
    return naturalTypeFrom(context.types, written);
}

template<typename Operation>
TypeID ResultRules::natural(const Operation& operation, NaturalTable table)
{
    return naturalTypeOfOperands(context, opcodeOf(operation), table, operands);
}

template<typename Operation>
TypeID ResultRules::sameAs(const Operation& operation, uint32_t operandIndex)
{
    Opcode opcode = opcodeOf(operation);
    TypeID source = operands[operandIndex];
    if (!source)
        return provenOrUnknown({ }, seedOfUnknownResultOf(opcode));
    // The definition throws for what is no object.
    if (opcode == Opcode::DefineOwnProperty)
        return context.types.withOnlyObjects(source);
    return source;
}

template<typename Operation>
TypeID ResultRules::allocation(const Operation& operation, LayoutID layout)
{
    // The caller has put the type of new.target in the place of a prototype.
    std::optional<unsigned> prototype = OperandPosition::prototype(opcodeOf(operation));
    return typeOfObjectAllocatedInResultRules(context, layout, created, prototype ? operands[*prototype] : TypeID());
}

template<typename Operation>
TypeID ResultRules::allocation(const Operation& operation, BuiltinClassID builtinClass)
{
    if (created)
        return context.types.forObject(created);
    if (PayloadWord::data(opcodeOf(operation)))
        return typeOfCollectionFromDataInResultRules(context, builtinClass, context.program.at(operation.data()).root);
    return context.types.builtinClass(builtinClass);
}

template<typename Operation>
TypeID ResultRules::allocation(const Operation&, BuiltinClassID builtinClass, TypeID contents)
{
    if (created)
        return context.types.forObject(created);
    if (!contents)
        return context.types.builtinClass(builtinClass);
    return context.types.builtinClass(builtinClass, singleElementSpan(contents));
}

template<typename Operation>
TypeID ResultRules::allocation(const Operation&, BuiltinLayoutID layout)
{
    return typeOfAllocatedLayout(context, context.program.layoutOf(layout), created);
}

template<typename Operation>
TypeID ResultRules::declared(const Operation& operation, VariableID variable)
{
    Opcode opcode = opcodeOf(operation);
    TypeID known = context.positions.variable(context.level, variable);
    TypeID held = typeHeldInVariableInResultRules(*this, known, variable, seedOfUnknownResultOf(opcode));
    if (opcode == Opcode::GetModuleVariable && operation.followsItsStore())
        held = context.types.withoutPrimitives(held, Primitive::Absent);
    if (opcode == Opcode::GetModuleVariable && operation.mayPrecedeItsStore())
        return context.types.withPrimitives(held, Primitive::Unspecified);
    if (opcode != Opcode::GetEncodedModuleVariable || !(context.at(held).primitives & Primitive::Undefined))
        return held;
    return context.types.withPrimitives(context.types.withoutPrimitives(held, Primitive::Undefined), Primitive::Absent);
}

template<typename Operation>
TypeID ResultRules::declared(const Operation& operation, LayoutID layout, uint32_t field)
{
    Opcode opcode = opcodeOf(operation);
    UnknownSeed seed = seedOfUnknownResultOf(opcode);
    const Vector<Field>& fields = context.program.at(layout).fields;
    if (field >= fields.size())
        return provenOrUnknown({ }, seed);

    if (opcode == Opcode::GetEnvironmentField) {
        TypeID held = typeHeldAtPositionInResultRules(*this, context.positions.environmentField(context.level, layout, field), fields[field].declared, seed);
        if (operation.followsItsStore())
            held = context.types.withoutPrimitives(held, Primitive::Absent);
        return operation.mayPrecedeItsStore() ? context.types.withPrimitives(held, Primitive::Unspecified) : held;
    }

    std::optional<uint8_t> receiver = receiverOf(opcode);
    if (!receiver || fields[field].flags.contains(FieldFlags::IsBrand))
        return typeHeldAtPositionInResultRules(*this, { }, fields[field].declared, seed);
    // Whatever the receiver is, what lies under a private name was laid there under the name, and has arrived at the field that the class declares. The mark of an empty slot is no value: the read throws there.
    if (familyOf(opcode) == OperationFamily::PrivateAccess) {
        if (TypeID arriving = context.positions.readOfSlot(context.level, layout, field))
            return context.types.withoutPrimitives(arriving, Primitive::Absent);
    }
    return provenOrUnknown(tryTypeOfFieldRead(context, operands[*receiver], fields[field].key), seed);
}

template<typename Operation>
TypeID ResultRules::declared(const Operation& operation, PropertyKeyID key)
{
    Opcode opcode = opcodeOf(operation);
    if (familyOf(opcode) == OperationFamily::SlotAccess) {
        bool holdsDeclaredType = FlagMask::holdsDeclaredType(opcode) && operation.holdsDeclaredType();
        return typeOfSlotReadInResultRules(*this, opcode, operands[*receiverOf(opcode)], operation.receivers(), key, holdsDeclaredType);
    }
    if (opcode == Opcode::GetGlobalBinding) {
        bool mayBeShadowed = operation.seesScriptBindings() && !operation.cannotBeShadowed();
        return provenOrUnknown(tryTypeOfGlobalBindingRead(context, key, mayBeShadowed, operation.findsOwnProperty()), seedOfUnknownResultOf(opcode));
    }

    // A free name looks at no receiver: it finds the binding of a script, or else a property of the global object. A script that nobody has seen may declare the name, and what its binding holds lies in no cell.
    // But for a member of the global object that compiled code takes for a constant: ProgramExecutable::initializeGlobalProperties() refuses the declaration.
    if (FlagMask::isFreeName(opcode) && operation.isFreeName() && context.level == TypeLevel::Proven) {
        ObjectID globalObject = singleObjectOf(context.at(typeOfBuiltinObject(context, Builtins::GlobalThis)));
        return provenOrUnknown(globalObject ? context.positions.property(context.level, globalObject, key) : TypeID(), seedOfUnknownResultOf(opcode));
    }

    std::optional<uint8_t> receiver = receiverOf(opcode);
    RELEASE_ASSERT(receiver);
    return provenOrUnknown(tryTypeOfFieldRead(context, operands[*receiver], key), seedOfUnknownResultOf(opcode));
}

template<typename Operation>
TypeID ResultRules::declared(const Operation& operation, FunctionID function, uint32_t resultIndex)
{
    Opcode opcode = opcodeOf(operation);
    UnknownSeed seed = seedOfUnknownResultOf(opcode);
    const Function& callee = context.program.at(function);

    // What is trusted of a capture holds behind the entry of the function alone, and this reads the closure from anywhere: what is asked for holds of every closure.
    if (opcode == Opcode::GetCapture) {
        uint32_t index = operation.index();
        if (index >= callee.captures.size())
            return provenOrUnknown({ }, seed);
        // An environment is typed by its layout, which the cell of a closure does not speak of.
        TypeID written = callee.captures[index].written();
        TypeID known = context.positions.capture(context.level, function, index);
        if (written && !context.at(written).isJavaScriptValue())
            return typeHeldAtPositionInResultRules(*this, known, callee.captures[index], seed);
        // The word of the closure is laid out for what holds of every closure, so only an answer inside that is taken.
        TypeID ofNamedClosures = tryTypeOfCaptureOfNamedClosuresInResultRules(context, operands[OperandIndex::GetCapture::closure], function, index);
        if (ofNamedClosures && (!known || context.types.isSubset(ofNamedClosures, known)))
            known = ofNamedClosures;
        return typeHeldAtPositionInResultRules(*this, known, callee.captures[index], seed);
    }

    bool isConstruction = FlagMask::isConstruction(opcode) && operation.isConstruction();

    // One object stands for what every call of a coroutine answers with. That holds by construction, so no summary is asked: the answer is the same where Facts knows nothing.
    if (PayloadWord::role(opcode) && callee.isCoroutine()) {
        if (!resultIndex && !isConstruction && unitHandsBackObjectOfCoroutineInResultRules(operation.role()))
            return context.types.forObject(context.types.objects().coroutineResultOf(function));
        if (!resultIndex && !isConstruction && operation.role() == CodeRole::CoroutineBody)
            return provenOrUnknown(context.positions.resultOfBodyThatNeverSuspends(context.level, function), seed);
        return provenOrUnknown({ }, seed);
    }

    if (TypeID applied = context.positions.resultOfCallBeingTyped(context.level, resultIndex))
        return applied;

    // CallTable names one of the functions that it may enter, so what is known of the results of that one says nothing of the call.
    if (!PayloadWord::role(opcode)) {
        if (resultIndex >= callee.results.size())
            return provenOrUnknown({ }, seed);
        return typeHeldAtPositionInResultRules(*this, { }, callee.results[resultIndex], seed);
    }

    // Whoever constructs gets the object where the constructor hands back none. The constructor of a class that takes no `this` makes the object itself, and what its body returns is what was constructed.
    bool bodyReturnsWhatIsConstructed = isClassConstructorOf(callee.kind) && !callee.has(FunctionFlags::HasThis) && returnsResultOfFunction(operation.role());
    // The body of a plain function, entered by whoever has made `this`, hands back what it returns, and he looks at it: walkConstructOfFunctionInWalkCalls().
    bool bodyHandsBackWhatItReturns = callee.kind == FunctionKind::Plain && returnsResultOfFunction(operation.role());
    if ((isConstruction && !bodyReturnsWhatIsConstructed && !bodyHandsBackWhatItReturns) || !unitHandsBackResultsInResultRules(callee, operation.role()))
        return provenOrUnknown({ }, seed);
    return typeOfResultHandedBackInResultRules(*this, function, resultIndex, seed);
}

template<typename Operation>
TypeID ResultRules::declared(const Operation& operation, LayoutID layout)
{
    // A parent may hand back another object, and a construction gives nothing but an object. So this side always has a proof, and the unproven side of the rule is never asked.
    if (TypeID applied = context.positions.resultOfCallBeingTyped(context.level, 0))
        return applied;
    if (std::optional<unsigned> parent = OperandPosition::parent(opcodeOf(operation))) {
        if (TypeID constructed = tryTypeConstructedByClassInResultRules(context, operands[*parent]))
            return constructed;
    }
    return typeOfDeclaredLayout(context, layout);
}

template<typename Operation>
TypeID ResultRules::declared(const Operation& operation, ModuleID module)
{
    LayoutID layout = context.program.layoutOfWholeNamespace(module);
    if (!layout || operation.phase() == ImportPhase::Defer)
        return context.types.object(seedOfUnknownResultOf(opcodeOf(operation)));
    return context.types.forLayout(layout);
}

template<typename Operation>
TypeID ResultRules::declared(const Operation& operation, BuiltinOverloadID overload, uint32_t resultIndex)
{
    UNUSED_PARAM(operation);
    if (resultIndex)
        return provenOrUnknown({ }, UnknownSeed::UndescribedNativeResult);

    BuiltinCallForm form = BuiltinCallForm::Whole;
    if constexpr (HoldsOpcode<Operation>) {
        Opcode opcode = opcodeOf(operation);
        if (!callsOfOverloadsInResultRules.contains(opcode))
            return provenOrUnknown(tryTypeWrittenForOverloadInResultRules(context, overload), UnknownSeed::UndescribedNativeResult);
        if (opcode == Opcode::CallBuiltin)
            form = operation.builtinCallForm();
    }
    return typeOfOverloadCallInResultRules(context, overload, form, operands);
}

template<typename Operation>
TypeID ResultRules::declaredContents(const Operation& operation, uint32_t operandIndex, std::optional<ContentsSlot> slot)
{
    Opcode opcode = opcodeOf(operation);
    UnknownSeed seed = seedOfUnknownResultOf(opcode);
    TypeID container = operands[operandIndex];
    if (!container)
        return provenOrUnknown({ }, seed);
    if (isNeverType(container))
        return TypeTable::never;

    if (opcode == Opcode::ElemGet)
        return provenOrUnknown(tryTypeOfElementRead(context, container, operands[OperandIndex::ElemGet::property]), seed);
    if (protocolsOf(opcode, operandIndex).contains(Protocol::Thenable))
        return provenOrUnknown(tryTypeOfAwaited(context, container), seed);
    if (opcode == Opcode::GetArrayElement) {
        bool findsElement = operation.isPresent() || (operation.isInBounds() && operation.readsHoleAsNaN());
        return provenOrUnknown(tryTypeOfArrayElementRead(context, container, operation.arrayStorage(), findsElement, operation.readsHoleAsNaN()), seed);
    }
    if (readsOutOfArrayStorageInResultRules.contains(opcode))
        return provenOrUnknown(tryTypeOfArrayElementRead(context, container, ArrayStorage::Other, false, false), seed);

    TypeID contents = tryTypeOfContentsRead(context, container, slot ? *slot : defaultContentsSlotInResultRules(context, container));
    // What a step gives where it finds no entry nobody uses, so the guess is at what an entry holds.
    bool givesUndefinedWhereNothingIsFound = stepsOfCollectionsInResultRules.contains(opcode) && context.level == TypeLevel::Proven;
    if (contents && givesUndefinedWhereNothingIsFound)
        contents = context.types.withPrimitives(contents, Primitive::Undefined);
    // The record stands where the step has found an entry.
    if (!contents)
        contents = context.positions.contentsBeingTyped(context.level);
    return provenOrUnknown(contents, seed);
}

template<typename Operation>
TypeID ResultRules::declaredParameter(const Operation&)
{
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Operation>
TypeID ResultRules::unknown(const Operation& operation)
{
    Opcode opcode = opcodeOf(operation);
    UnknownSeed seed = seedOfUnknownResultOf(opcode);

    // The caller goes on with what the promise of the call would have been fulfilled with. One object stands for that promise, and the summary applied to the arguments at hand may name the copy of it for this call.
    if constexpr (HoldsOpcode<Operation>) {
        if (opcode == Opcode::AwaitCallBySavingFrame) {
            TypeID promise = context.positions.resultOfCallBeingTyped(context.level, 0);
            if (!promise)
                promise = context.types.forObject(context.types.objects().coroutineResultOf(operation.function()));
            TypeID given = tryTypeOfAwaited(context, promise);
            return given ? given : context.types.anything(seed);
        }
        // The lookup starts at the base. A proof and never less: what is expected of the receiver says nothing of what lies above the home object.
        if (opcode == Opcode::SuperGet && context.level == TypeLevel::Proven && operation.key()) {
            if (TypeID found = tryTypeOfFieldRead(context, operands[OperandIndex::SuperGet::base], operation.key()))
                return found;
        }
    }

    if constexpr (HoldsOpcode<Operation>) {
        if (opcode == Opcode::CallTable) {
            TypeID returned = context.positions.resultOfSelector(context.level, operation.selector());
            return returned ? returned : context.types.anything(seed);
        }
    }

    if (!callsTypedBySummariesInResultRules.contains(opcode))
        return context.types.anything(seed);

    if (TypeID applied = context.positions.resultOfCallBeingTyped(context.level, 0))
        return applied;
    if (callsOfBuiltinsByValueInResultRules.contains(opcode)) {
        if (TypeID written = tryTypeOfBuiltinCalledByValueInResultRules(context, opcode, operands))
            return written;
    }
    if (constructionsByValue.contains(opcode)) {
        static_assert(OperandIndex::Construct::callee == OperandIndex::Call::callee && OperandIndex::ConstructList::callee == OperandIndex::Call::callee);
        if (TypeID constructed = tryTypeConstructedByClassInResultRules(context, operands[OperandIndex::Call::callee]))
            return constructed;
        return context.level == TypeLevel::Expected ? context.types.object(seed) : context.types.anything(seed);
    }
    if (!callsByValueInResultRules.contains(opcode))
        return context.types.anything(seed);

    TypeID callee = operands[OperandIndex::Call::callee];
    if (!callee)
        return context.types.anything(seed);
    if (holdsNoCalleeInResultRules(context, callee))
        return TypeTable::never;
    if (FunctionID only = onlyCalleeOf(context, callee))
        return typeOfResultHandedBackInResultRules(*this, only, 0, seed);
    return context.types.anything(seed);
}

template<typename Operation>
TypeID ResultRules::cast(const Operation&, uint32_t, TypeID)
{
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Operation>
TypeID ResultRules::cast(const Operation&, uint32_t, ExpectedType)
{
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Operation>
TypeID ResultRules::cast(const Operation&, uint32_t, ProvenType)
{
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Operation>
TypeID ResultRules::cast(const Operation&, uint32_t)
{
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Operation>
TypeID ResultRules::resultOperand(const Operation& operation, uint32_t resultIndex)
{
    Opcode opcode = opcodeOf(operation);
    return provenOrUnknown(operands[numberOfNamedOperandsOf(opcode) + resultIndex], seedOfUnknownResultOf(opcode));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
