/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinTypes.h"

#if ENABLE(AOT)

#include "AOTBuiltinCallPlan.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTResultRules.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class BuiltinTypeRole : uint8_t {
    // What native code hands out. Its `unknown` says where it came from.
    Value,
    // What is written of a position: a parameter, a field, the contents of a class. It has no seeds, like every target of a test.
    Bound,
};

struct BuiltinTypeReading {
    const BuiltinTypeContext& context;
    const BuiltinOperandTypes* operands { nullptr };
    BuiltinTypeRole role { BuiltinTypeRole::Value };
    bool writesContents { true };
};

} // anonymous namespace

// A test at run time answers the first three, and all four speak of names and of prototype links, as ObjectFact::IsPristine does.
// Dense and AsCreated speak of elements and of what was stored since the object was made. No fact about members proves them: each has a fact of its own, which counts only beside IsPristine.
static constexpr PristineAspectSet aspectsProvenByPristineMembers { PristineAspects::Storage, PristineAspects::Names, PristineAspects::NamesKeptBelow, PristineAspects::Prototype };

static TypeID typeOfBuiltinTypeRead(const BuiltinTypeReading&, BuiltinTypeID);

static uint16_t primitivesOfBuiltinPrimitive(BuiltinPrimitive member, bool hasOnlyIntegers)
{
    switch (member) {
    case BuiltinPrimitive::Undefined:
    case BuiltinPrimitive::NotPassed:
        return Primitive::Undefined;
    case BuiltinPrimitive::Null:
        return Primitive::Null;
    case BuiltinPrimitive::Boolean:
        return Primitive::Boolean;
    case BuiltinPrimitive::Number:
        return hasOnlyIntegers ? Primitive::Integer : Primitive::Number;
    case BuiltinPrimitive::String:
        return Primitive::String;
    case BuiltinPrimitive::Symbol:
        return Primitive::Symbol;
    case BuiltinPrimitive::BigInt:
        return Primitive::BigInt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uint16_t primitivesOfBuiltinType(const BuiltinType& type)
{
    if (type.isAnything())
        return Primitive::All;

    bool hasOnlyIntegers = type.flags.contains(BuiltinTypeFlag::HasOnlyIntegers);
    uint16_t primitives = 0;
    for (BuiltinPrimitive member : type.primitives)
        primitives |= primitivesOfBuiltinPrimitive(member, hasOnlyIntegers);
    return primitives;
}

Range integerRangeWrittenBy(const BuiltinType& type)
{
    if (!type.flags.contains(BuiltinTypeFlag::HasOnlyIntegers) || type.integers.isEmpty())
        return Range::full();
    return { type.integers.minimum, type.integers.maximum };
}

static TypeID primitivePartOfBuiltinType(const BuiltinTypeContext& context, const BuiltinType& type)
{
    TypeArena& types = context.types;
    TypeParts parts;
    parts.primitives = primitivesOfBuiltinType(type);
    if (parts.primitives & Primitive::Integer)
        parts.integers = integerRangeWrittenBy(type);

    TypeID result = types.intern(parts);
    if (parts.primitives & Primitive::String)
        return result;
    // A text that the program does not hold has no number, so it can be no member of a set.
    for (BuiltinName name : type.stringLiterals) {
        StringID text = context.program.stringOf(name);
        result = types.unionOf(result, text ? types.stringLiteral(text) : TypeID(TypeTable::string));
    }
    return result;
}

TypeID primitivesWrittenByBuiltinType(const BuiltinTypeContext& context, BuiltinTypeID id)
{
    return primitivePartOfBuiltinType(context, context.program.builtins().at(id));
}

static TypeID anyValueWrittenByTable(const BuiltinTypeReading& reading, UnknownSeed seed = UnknownSeed::UndescribedNativeResult)
{
    return reading.role == BuiltinTypeRole::Value ? reading.context.types.anything(seed) : TypeTable::anything;
}

static TypeID anyObjectWrittenByTable(const BuiltinTypeReading& reading)
{
    return reading.role == BuiltinTypeRole::Value ? reading.context.types.object(UnknownSeed::UndescribedNativeResult) : TypeTable::object;
}

static TypeID anyFunctionWrittenByTable(const BuiltinTypeReading& reading)
{
    return reading.role == BuiltinTypeRole::Value ? reading.context.types.function(UnknownSeed::UndescribedNativeResult) : TypeTable::function;
}

// What is written of the contents of a class proves nothing. Only the rules of reads know at which level it counts.
static TypingContext typingContextOfBuiltinTypes(const BuiltinTypeContext& context)
{
    return { context.program, context.types, context.positions ? *context.positions : PositionTypes::knowingNothing(), context.level };
}

static bool holdsResolvedValueOfPromise(const BuiltinSlot& slot)
{
    return slot.owner == BuiltinClasses::Promise && slot.contentsSlot == ContentsSlot::Value;
}

static TypeID typeOfBuiltinInstance(const BuiltinTypeReading& reading, const BuiltinTypePart& part)
{
    const BuiltinTypeContext& context = reading.context;
    const BuiltinTable& table = context.program.builtins();
    TypeArena& types = context.types;

    if (table.at(part.builtinClass).hasClosureInstances)
        return anyFunctionWrittenByTable(reading);
    if (!reading.writesContents || part.contents.empty())
        return types.builtinClass(part.builtinClass);

    BuiltinTypeReading readingOfContents { context, reading.operands, BuiltinTypeRole::Bound, true };
    std::array<TypeID, mostContentsOfClass> contents;
    contents.fill(TypeTable::anything);
    unsigned numberOfContents = 0;
    for (const BuiltinSlotContents& declared : part.contents) {
        const BuiltinSlot& slot = table.at(declared.slot);
        if (!slot.contentsSlot)
            continue;
        std::optional<uint8_t> position = positionInTypeOf(table, part.builtinClass, *slot.contentsSlot);
        if (!position || *position >= mostContentsOfClass)
            continue;

        TypeID held = typeOfBuiltinTypeRead(readingOfContents, declared.type);
        if (reading.operands && holdsResolvedValueOfPromise(slot))
            held = typeOfAwaited(typingContextOfBuiltinTypes(context), held);
        // `new Set<number>()`: what a collection is born with bounds nothing that it is given later, so one that is born empty says nothing. A promise is settled once.
        bool saysNothing = types.at(held).isAnything() || (isNeverType(held) && !holdsResolvedValueOfPromise(slot));
        contents[*position] = saysNothing ? TypeTable::anything : held;
        numberOfContents = std::max<unsigned>(numberOfContents, *position + 1);
    }
    return types.builtinClass(part.builtinClass, std::span<const TypeID>(contents).first(numberOfContents));
}

static TypeID typeOfBuiltinTuple(const BuiltinTypeReading& reading, const BuiltinLayout& tuple)
{
    TypeArena& types = reading.context.types;
    if (!reading.writesContents)
        return types.builtinClass(BuiltinClasses::Array);

    BuiltinTypeReading readingOfFields { reading.context, nullptr, BuiltinTypeRole::Bound, true };
    TypeID element = TypeTable::never;
    for (const BuiltinLayoutField& field : tuple.fields)
        element = types.unionOf(element, typeOfBuiltinTypeRead(readingOfFields, field.type));
    if (types.at(element).isAnything())
        return types.builtinClass(BuiltinClasses::Array);
    return types.builtinClass(BuiltinClasses::Array, singleElementSpan(element));
}

// Native code makes an ordinary object with the names of the layout, never a cell of the compiled layout.
static TypeID typeOfBuiltinLayoutRead(const BuiltinTypeReading& reading, BuiltinLayoutID id)
{
    const BuiltinLayout& layout = reading.context.program.builtins().at(id);
    return layout.isTuple ? typeOfBuiltinTuple(reading, layout) : anyObjectWrittenByTable(reading);
}

static TypeID typeFromBuiltinOperand(const BuiltinTypeReading& reading, BuiltinOperand operand, TypeID typeWithoutOperands, NOESCAPE const Invocable<TypeID(TypeID)> auto& typeFrom)
{
    if (!reading.operands)
        return typeWithoutOperands;

    auto typeFromPassed = [&](TypeID passed) -> TypeID {
        if (!passed)
            return typeFrom(TypeTable::undefined);
        return isNeverType(passed) ? TypeTable::never : typeFrom(passed);
    };

    switch (operand.kind()) {
    case BuiltinOperand::Kind::Receiver:
    case BuiltinOperand::Kind::Argument:
        return typeFromPassed(reading.operands->at(operand));
    case BuiltinOperand::Kind::ArgumentsFrom: {
        const Vector<TypeID, 4>& arguments = reading.operands->arguments;
        TypeID result = TypeTable::never;
        for (size_t index = operand.argumentIndex(); index < arguments.size(); ++index)
            result = reading.context.types.unionOf(result, typeFromPassed(arguments[index]));
        return result;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static TypeID typeFromClassesOfReceiver(const BuiltinTypeReading& reading, NOESCAPE const Invocable<BuiltinClassID(BuiltinClassID)> auto& resultClassOf)
{
    TypeArena& types = reading.context.types;
    TypeID anyObject = anyObjectWrittenByTable(reading);
    return typeFromBuiltinOperand(reading, BuiltinOperand::receiver(), anyObject, [&](TypeID receiverID) -> TypeID {
        Type receiver = types.at(receiverID);
        if (!receiver.hasOnlyClassObjects())
            return anyObject;

        // The list of parts may move as soon as the arena makes a type.
        Vector<BuiltinClassID, 4> resultClasses;
        for (const ClassPart& part : classPartsOf(types, receiver)) {
            BuiltinClassID resultClass = resultClassOf(part.builtinClass);
            if (!resultClass)
                return anyObject;
            resultClasses.append(resultClass);
        }

        TypeID result = TypeTable::never;
        for (BuiltinClassID resultClass : resultClasses)
            result = types.unionOf(result, types.builtinClass(resultClass));
        return result;
    });
}

static TypeID typeOfValuesIteratedByBuiltin(const BuiltinTypeReading& reading, TypeID iterableID)
{
    const BuiltinTypeContext& context = reading.context;
    Type iterable = context.types.at(iterableID);
    if (iterable.isAnyString())
        return TypeTable::string;
    // Native code steps whatever the object finds under `next` or Symbol.iterator, and no type says that an object has kept the members of its class.
    if (reading.role == BuiltinTypeRole::Value && context.level == TypeLevel::Proven)
        return anyValueWrittenByTable(reading);

    const ClassPart* part = singleBuiltinClassOf(context.types, iterable);
    if (!part || !context.program.builtins().findSlot(part->builtinClass, ContentsSlot::Element))
        return anyValueWrittenByTable(reading);
    return typeOfContentsRead(typingContextOfBuiltinTypes(context), iterableID, ContentsSlot::Element);
}

static TypeID typeOfBuiltinTypePartRead(const BuiltinTypeReading& reading, const BuiltinTypePart& part)
{
    const BuiltinTypeContext& context = reading.context;
    const BuiltinTable& table = context.program.builtins();
    TypeArena& types = context.types;

    switch (part.kind) {
    case BuiltinTypePartKind::Instance:
        return typeOfBuiltinInstance(reading, part);
    case BuiltinTypePartKind::InstanceOfReceiverClass:
        return typeFromClassesOfReceiver(reading, [](BuiltinClassID receiverClass) {
            return receiverClass;
        });
    case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
        return typeFromClassesOfReceiver(reading, [&](BuiltinClassID receiverClass) {
            return table.typedArrayClassOf(receiverClass);
        });
    case BuiltinTypePartKind::Layout:
        return typeOfBuiltinLayoutRead(reading, part.layout);
    case BuiltinTypePartKind::Builtin:
        return types.forBuiltin(table.canonicalID(part.builtin));
    case BuiltinTypePartKind::Operand:
        return typeFromBuiltinOperand(reading, *part.operand, anyValueWrittenByTable(reading), [](TypeID operand) {
            return operand;
        });
    case BuiltinTypePartKind::Slot: {
        const BuiltinSlot& slot = table.at(part.slot);
        if (!slot.contentsSlot)
            return typeOfBuiltinTypeRead({ context, nullptr, reading.role, reading.writesContents }, slot.holds);
        return typeFromBuiltinOperand(reading, *part.operand, anyValueWrittenByTable(reading), [&](TypeID container) {
            return typeOfContentsRead(typingContextOfBuiltinTypes(context), container, *slot.contentsSlot);
        });
    }
    case BuiltinTypePartKind::IteratedValues:
        return typeFromBuiltinOperand(reading, *part.operand, anyValueWrittenByTable(reading), [&](TypeID iterable) {
            TypeID iterated = typeOfValuesIteratedByBuiltin(reading, iterable);
            return part.isSettled ? typeOfAwaited(typingContextOfBuiltinTypes(context), iterated) : iterated;
        });
    case BuiltinTypePartKind::CallbackResult:
        return anyValueWrittenByTable(reading, UnknownSeed::ResultOfUnknownCallee);
    case BuiltinTypePartKind::Prototype:
        return types.withPrimitives(anyObjectWrittenByTable(reading), Primitive::Null);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static TypeID typeOfBuiltinTypeRead(const BuiltinTypeReading& reading, BuiltinTypeID id)
{
    const BuiltinTypeContext& context = reading.context;
    const BuiltinTable& table = context.program.builtins();
    if (!id || table.at(id).isAnything())
        return anyValueWrittenByTable(reading);

    const BuiltinType& type = table.at(id);
    TypeArena& types = context.types;
    TypeID result = primitivePartOfBuiltinType(context, type);
    if (type.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasPlainData }))
        result = types.unionOf(result, anyObjectWrittenByTable(reading));
    if (type.flags.contains(BuiltinTypeFlag::HasAnyFunction))
        result = types.unionOf(result, anyFunctionWrittenByTable(reading));
    for (const BuiltinTypePart& part : type.parts)
        result = types.unionOf(result, typeOfBuiltinTypePartRead(reading, part));
    return result;
}

TypeID typeOfBuiltinType(const BuiltinTypeContext& context, BuiltinTypeID id, const BuiltinOperandTypes* operands)
{
    return typeOfBuiltinTypeRead({ context, operands }, id);
}

TypeID typeOfBuiltinTypePart(const BuiltinTypeContext& context, const BuiltinTypePart& part, BuiltinFreshness, const BuiltinOperandTypes* operands)
{
    return typeOfBuiltinTypePartRead({ context, operands }, part);
}

TypeID typeOfBuiltinLayout(const BuiltinTypeContext& context, BuiltinLayoutID id)
{
    return typeOfBuiltinLayoutRead({ context }, id);
}

TypeID typeOfBuiltinLayoutField(const BuiltinTypeContext& context, const BuiltinLayoutField& field)
{
    return typeOfBuiltinTypeRead({ context, nullptr, BuiltinTypeRole::Bound, true }, field.type);
}

TypeID typeOfBuiltinConstant(const BuiltinTypeContext& context, BuiltinID id)
{
    const BuiltinTable& table = context.program.builtins();
    TypeArena& types = context.types;
    BuiltinKind kind = table.at(id).kind;
    if (isObjectKind(kind))
        return types.forBuiltin(table.canonicalID(id));
    if (kind != BuiltinKind::Constant)
        return types.anything(UnknownSeed::UndescribedNativeResult);
    if (std::optional<double> number = table.numberOf(id))
        return types.number(*number);
    if (BuiltinName symbol = table.nameOfSymbol(id)) {
        PropertyKeyID key = context.program.keyOf(symbol);
        return key ? types.symbolLiteral(key) : TypeTable::symbol;
    }
    return types.anything(UnknownSeed::UndescribedNativeResult);
}

TypeID parameterTypeOfOverload(const BuiltinTypeContext& context, BuiltinOverloadID id, BuiltinOperand operand)
{
    const BuiltinTable& table = context.program.builtins();
    const BuiltinParameter* parameter = table.parameterFor(table.at(id), operand);
    if (!parameter)
        return TypeTable::anything;
    bool isConvertedInFrontOfCall = parameter->converted && !conversionOpcodesOf(parameter->conversion).empty();
    return typeOfBuiltinTypeRead({ context, nullptr, BuiltinTypeRole::Bound, false }, isConvertedInFrontOfCall ? parameter->converted : parameter->accepted);
}

// `list.reduce(add)` and `list.reduce(add, 0)` are two rows and one text: a guess by the first alone turns away every call that passes an initial value.
static TypeID parameterTypeAcrossOverloadsOf(const BuiltinTypeContext& context, RuntimeFunctionID function, BuiltinOperand operand)
{
    const BuiltinTable& table = context.program.builtins();
    BuiltinOverloadID first;
    TypeID receiverOfFirst;
    TypeID taken;
    for (BuiltinID builtin : table.at(function).implements) {
        for (BuiltinOverloadID id : table.overloadIDsOf(builtin)) {
            if (table.at(id).libraryFunction() != function)
                continue;
            if (!first) {
                first = id;
                taken = parameterTypeOfOverload(context, id, operand);
                if (operand.isReceiver())
                    return taken;
                receiverOfFirst = parameterTypeOfOverload(context, id, BuiltinOperand::receiver());
                continue;
            }
            if (table.at(id).mode == table.at(first).mode && parameterTypeOfOverload(context, id, BuiltinOperand::receiver()) == receiverOfFirst)
                taken = context.types.unionOf(taken, parameterTypeOfOverload(context, id, operand));
        }
    }
    return taken;
}

TypeID typeExpectedByLibraryFunction(const Program& program, TypeArena& types, FunctionID id, uint32_t entryParameter)
{
    const Function& function = program.at(id);
    if (!function.enclosingRuntimeFunction || entryParameter >= function.numberOfEntryParameters())
        return { };

    std::optional<BuiltinOperand> operand;
    switch (function.entryParameterKind(entryParameter)) {
    case EntryParameter::This:
        operand = BuiltinOperand::receiver();
        break;
    case EntryParameter::Parameter:
        // The rest parameter takes a list of what the call passes from its position on.
        if (entryParameter - function.firstParameterIndex() < function.numberOfPositionalParameters())
            operand = BuiltinOperand::argument(entryParameter - function.firstParameterIndex());
        break;
    case EntryParameter::Capture:
    case EntryParameter::Callee:
    case EntryParameter::NewTarget:
    case EntryParameter::ArgumentCount:
    case EntryParameter::HiddenRest:
        break;
    }
    const DeclaredType* declared = function.declaredEntryParameter(entryParameter);
    if (!operand || !declared || declared->written() != TypeTable::anything || declared->isEstablished() || declared->has(PositionFlags::IsInt32Promise))
        return { };

    // A function that is written inside the text of the built-in has operands of its own.
    const RuntimeFunctionEntry* entry = findRuntimeFunction(program, function.enclosingRuntimeFunction);
    if (!entry || entry->function != id)
        return { };
    TypeID taken = parameterTypeAcrossOverloadsOf({ program, types }, entry->id, *operand);
    if (!taken || isNeverType(taken) || types.isSubset(TypeTable::anything, taken))
        return { };
    return taken;
}

TypedArrayType typedArrayTypeOf(const BuiltinTypeContext& context, TypeID id)
{
    if (!id)
        return NotTypedArray;
    Type type = context.types.at(id);
    if (!type.hasOnlyClassObjects())
        return NotTypedArray;

    const BuiltinTable& table = context.program.builtins();
    std::span<const ClassPart> parts = classPartsOf(context.types, type);
    if (parts.empty())
        return NotTypedArray;
    TypedArrayType common = table.typedArrayTypeOf(parts.front().builtinClass);
    for (const ClassPart& part : parts) {
        if (table.typedArrayTypeOf(part.builtinClass) != common)
            return NotTypedArray;
    }
    return common;
}

bool hasOnlyMapsAndSets(const BuiltinTypeContext& context, TypeID id)
{
    if (!id)
        return false;
    Type type = context.types.at(id);
    if (!type.isObject() || type.hasUnboundedUnknown() || type.traitsOfNamed.contains(ObjectTrait::HasNoShape))
        return false;

    auto isMapOrSet = [](BuiltinClassID builtinClass) {
        return builtinClass == BuiltinClasses::Map || builtinClass == BuiltinClasses::Set;
    };
    for (const ClassPart& part : classPartsOf(context.types, type)) {
        if (!isMapOrSet(part.builtinClass))
            return false;
    }
    return std::ranges::all_of(membersOf(context.types, type), [&](LayoutID layout) {
        return !context.program.at(layout).mayBeOrdinaryObject && isMapOrSet(builtinClassWrittenAbove(context.types, layout));
    });
}

PristineAspectSet pristineAspectsProvenBy(const BuiltinTypeContext& context, TypeID id, BuiltinClassID builtinClass)
{
    if (!context.facts || !id)
        return { };
    Type type = context.types.at(id);
    if (!type.hasOnlyClassObjects() || classPartsOf(context.types, type).size() != 1 || !findClassPart(context.types, type, builtinClass))
        return { };
    if (!context.facts->everyObjectHas(context.types, id, ObjectFact::IsPristine))
        return { };

    // The one fact is stated of arrays alone and the other of RegExps alone, so no other class pays for a second look at its objects.
    PristineAspectSet aspects = aspectsProvenByPristineMembers;
    if (builtinClass == BuiltinClasses::Array && context.facts->everyObjectHas(context.types, id, ObjectFact::HasNoHoles))
        aspects.add(PristineAspects::Dense);
    if (builtinClass == BuiltinClasses::RegExp && context.facts->everyObjectHas(context.types, id, ObjectFact::IsAsCreated))
        aspects.add(PristineAspects::AsCreated);
    return aspects;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
