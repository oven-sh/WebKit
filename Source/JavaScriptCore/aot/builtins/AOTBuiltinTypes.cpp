/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinTypes.h"

#if ENABLE(AOT)

#include "AOTBuiltinCallPlan.h"
#include "AOTTypeRelations.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static uint16_t primitivesOfMember(BuiltinPrimitive member, bool hasOnlyIntegers)
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
        primitives |= primitivesOfMember(member, hasOnlyIntegers);
    return primitives;
}

static Range integersOf(const BuiltinType& type)
{
    if (!type.flags.contains(BuiltinTypeFlag::HasOnlyIntegers) || type.integers.isEmpty())
        return Range::full();
    return { type.integers.minimum, type.integers.maximum };
}

static Type primitivePartOf(const Program& program, const BuiltinType& type)
{
    Type result = Type::fromPrimitives(primitivesOfBuiltinType(type));
    if (result.primitives & Primitive::Integer)
        result.integers = integersOf(type);
    if (!(result.primitives & Primitive::String)) {
        for (BuiltinName name : type.stringLiterals)
            result.stringLiterals.add(program.stringOf(name));
    }
    result.canonicalize();
    return result;
}

static Vector<TypeID, 2> contentsOfInstancePart(const BuiltinTable& table, const BuiltinTypePart& part, NOESCAPE const Invocable<TypeID(const BuiltinSlotContents&)> auto& internedTypeOf)
{
    Vector<TypeID, 2> contents(FillWith { }, table.numberOfSlotsInType(part.builtinClass), TypeTable::anything);
    for (const BuiltinSlotContents& declared : part.contents) {
        if (std::optional<uint8_t> index = table.at(declared.slot).indexInType)
            contents[*index] = internedTypeOf(declared);
    }
    return contents;
}

static bool holdsResolvedValue(const BuiltinSlot& slot)
{
    return slot.owner == BuiltinClasses::Promise && slot.contentsSlot == ContentsSlot::Value;
}

static Type closedTypeOf(const Program&, BuiltinTypeID, TypeArena* contentsArena);

static ClassPart closedInstancePartOf(const Program& program, const BuiltinTypePart& part, TypeArena* contentsArena)
{
    ClassPart result;
    result.builtinClass = part.builtinClass;
    if (!contentsArena || part.contents.empty())
        return result;

    result.writtenContents = contentsOfInstancePart(program.builtins(), part, [&](const BuiltinSlotContents& declared) {
        return contentsArena->intern(closedTypeOf(program, declared.type, contentsArena));
    });
    return result;
}

static ClassPart closedTuplePartOf(const Program& program, const BuiltinLayout& tuple, TypeArena* contentsArena)
{
    ClassPart result;
    result.builtinClass = BuiltinClasses::Array;
    if (!contentsArena)
        return result;

    Type element = Type::never();
    for (const BuiltinLayoutField& field : tuple.fields)
        element = element.unionWith(closedTypeOf(program, field.type, contentsArena), *contentsArena);
    result.writtenContents.append(contentsArena->intern(element));
    return result;
}

static Type closedTypeOf(const Program& program, BuiltinTypeID id, TypeArena* contentsArena)
{
    const BuiltinTable& table = program.builtins();
    if (!id || table.at(id).isAnything())
        return Type::anything();

    const BuiltinType& type = table.at(id);
    Type result = primitivePartOf(program, type);
    if (type.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasPlainData }))
        result.anyObject = true;
    if (type.flags.contains(BuiltinTypeFlag::HasAnyFunction))
        result.anyFunction = true;

    for (const BuiltinTypePart& part : type.parts) {
        switch (part.kind) {
        case BuiltinTypePartKind::Instance:
            if (table.at(part.builtinClass).hasClosureInstances)
                result.anyFunction = true;
            else
                result.classes.append(closedInstancePartOf(program, part, contentsArena));
            break;
        case BuiltinTypePartKind::Builtin:
            result.builtins.add(table.canonicalID(part.builtin));
            break;
        case BuiltinTypePartKind::Layout:
            if (table.at(part.layout).isTuple)
                result.classes.append(closedTuplePartOf(program, table.at(part.layout), contentsArena));
            else
                result.anyObject = true;
            break;
        case BuiltinTypePartKind::InstanceOfReceiverClass:
        case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
            result.anyObject = true;
            break;
        case BuiltinTypePartKind::Prototype:
            result.anyObject = true;
            result.primitives |= Primitive::Null;
            break;
        case BuiltinTypePartKind::Operand:
        case BuiltinTypePartKind::Slot:
        case BuiltinTypePartKind::IteratedValues:
        case BuiltinTypePartKind::CallbackResult:
            return Type::anything();
        }
    }
    result.canonicalize();
    return result;
}

Type typeOfBuiltinLayout(const TypingContext& context, BuiltinLayoutID id)
{
    const BuiltinLayout& layout = context.program.builtins().at(id);
    if (!layout.isTuple)
        return Type::object();

    Type result;
    result.classes.append(closedTuplePartOf(context.program, layout, &context.types));
    result.canonicalize();
    return result;
}

static Type typeFromOperand(const BuiltinTypeInputs& inputs, BuiltinOperand operand, const Type& typeWithoutOperands, NOESCAPE const Invocable<Type(const Type&)> auto& typeFrom)
{
    if (!inputs.operands)
        return typeWithoutOperands;

    TypeArena& types = inputs.context.types;
    auto typeFromID = [&](TypeID id) {
        if (!id)
            return typeFrom(Type::fromPrimitives(Primitive::Undefined));
        const Type& type = types.at(id);
        return type.isNever() ? Type::never() : typeFrom(type);
    };

    switch (operand.kind()) {
    case BuiltinOperand::Kind::Receiver:
    case BuiltinOperand::Kind::Argument:
        return typeFromID(inputs.operands->at(operand));
    case BuiltinOperand::Kind::ArgumentsFrom: {
        const Vector<TypeID, 4>& arguments = inputs.operands->arguments;
        Type result = Type::never();
        for (size_t index = operand.argumentIndex(); index < arguments.size(); ++index)
            result = result.unionWith(typeFromID(arguments[index]), types);
        return result;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Type typeFromReceiverClasses(const BuiltinTypeInputs& inputs, NOESCAPE const Invocable<BuiltinClassID(BuiltinClassID)> auto& resultClassOf)
{
    return typeFromOperand(inputs, BuiltinOperand::receiver(), Type::object(), [&](const Type& receiver) {
        if (receiver.primitives & ~Primitive::UndefinedOrNull || !receiver.hasOnlyClassObjects())
            return Type::object();
        Type result = Type::never();
        for (const ClassPart& part : receiver.classes) {
            BuiltinClassID resultClass = resultClassOf(part.builtinClass);
            if (!resultClass)
                return Type::object();
            result = result.unionWith(Type::builtinClass(resultClass), inputs.context.types);
        }
        return result;
    });
}

static Type typeOfIteratedValues(const TypingContext& context, const Type& iterable)
{
    if (iterable.isAnyString())
        return Type::fromPrimitives(Primitive::String);
    const ClassPart* part = iterable.singleBuiltinClass();
    if (!part || !context.program.builtins().findSlot(part->builtinClass, ContentsSlot::Element))
        return Type::anything();
    return typeOfContentsRead(context, iterable, ContentsSlot::Element);
}

Type typeOfBuiltinTypePart(const BuiltinTypeInputs& inputs, const BuiltinTypePart& part, BuiltinFreshness)
{
    const TypingContext& context = inputs.context;
    const BuiltinTable& table = context.program.builtins();

    switch (part.kind) {
    case BuiltinTypePartKind::Instance: {
        if (table.at(part.builtinClass).hasClosureInstances)
            return Type::function();
        Vector<TypeID, 2> contents = contentsOfInstancePart(table, part, [&](const BuiltinSlotContents& declared) {
            Type held = typeOfBuiltinType(inputs, declared.type);
            return context.types.intern(holdsResolvedValue(table.at(declared.slot)) ? typeOfAwaited(context, held) : held);
        });
        Type instance = Type::builtinClass(part.builtinClass, contents.span());
        instance.canonicalize();
        return instance;
    }
    case BuiltinTypePartKind::InstanceOfReceiverClass:
        return typeFromReceiverClasses(inputs, [](BuiltinClassID receiverClass) {
            return receiverClass;
        });
    case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
        return typeFromReceiverClasses(inputs, [&](BuiltinClassID receiverClass) {
            return table.typedArrayClassOf(receiverClass);
        });
    case BuiltinTypePartKind::Layout:
        return typeOfBuiltinLayout(context, part.layout);
    case BuiltinTypePartKind::Builtin:
        return Type::forBuiltin(table.canonicalID(part.builtin));
    case BuiltinTypePartKind::Operand:
        return typeFromOperand(inputs, *part.operand, Type::anything(), [](const Type& operand) {
            return operand;
        });
    case BuiltinTypePartKind::Slot: {
        const BuiltinSlot& slot = table.at(part.slot);
        if (!slot.contentsSlot)
            return typeOfBuiltinType({ context, nullptr }, slot.holds);
        return typeFromOperand(inputs, *part.operand, Type::anything(), [&](const Type& container) {
            return typeOfContentsRead(context, container, *slot.contentsSlot);
        });
    }
    case BuiltinTypePartKind::IteratedValues:
        return typeFromOperand(inputs, *part.operand, Type::anything(), [&](const Type& iterable) {
            Type iterated = typeOfIteratedValues(context, iterable);
            return part.isSettled ? typeOfAwaited(context, iterated) : iterated;
        });
    case BuiltinTypePartKind::CallbackResult:
        return Type::anything();
    case BuiltinTypePartKind::Prototype:
        return Type::object().withPrimitives(Primitive::Null);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Type typeOfBuiltinType(const BuiltinTypeInputs& inputs, BuiltinTypeID id)
{
    const Program& program = inputs.context.program;
    const BuiltinTable& table = program.builtins();
    if (!id || table.at(id).isAnything())
        return Type::anything();

    const BuiltinType& type = table.at(id);
    TypeArena& types = inputs.context.types;
    Type result = primitivePartOf(program, type);
    if (type.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasPlainData }))
        result = result.unionWith(Type::object(), types);
    if (type.flags.contains(BuiltinTypeFlag::HasAnyFunction))
        result = result.unionWith(Type::function(), types);
    for (const BuiltinTypePart& part : type.parts)
        result = result.unionWith(typeOfBuiltinTypePart(inputs, part, type.freshness), types);
    result.canonicalize();
    return result;
}

Type parameterTypeOfOverload(const Program& program, BuiltinOverloadID id, BuiltinOperand operand)
{
    const BuiltinTable& table = program.builtins();
    const BuiltinParameter* parameter = table.parameterFor(table.at(id), operand);
    if (!parameter)
        return Type::anything();
    bool isConvertedInFrontOfCall = parameter->converted && !conversionOpcodesOf(parameter->conversion).empty();
    return closedTypeOf(program, isConvertedInFrontOfCall ? parameter->converted : parameter->accepted, nullptr);
}

Type typeOfBuiltinLayoutField(const Program& program, const BuiltinLayoutField& field, TypeArena& arena)
{
    return closedTypeOf(program, field.type, &arena);
}

PristineAspectSet pristineAspectsProvenBy(const Type& type, BuiltinClassID builtinClass)
{
    if (!type.hasOnlyClassObjects() || type.classes.size() != 1)
        return { };
    const ClassPart* part = type.findClassPart(builtinClass);
    return part ? part->provenPristine : PristineAspectSet();
}

static const BuiltinTypePart* findPartAdmitting(const BuiltinTable& table, const BuiltinType& tested, BuiltinClassID builtinClass)
{
    auto found = std::ranges::find_if(tested.parts, [&](const BuiltinTypePart& part) {
        return part.kind == BuiltinTypePartKind::Instance && table.isSameOrSubclassOf(builtinClass, part.builtinClass);
    });
    return found == tested.parts.end() ? nullptr : &*found;
}

PristineRequirement pristineRequirementOf(const BuiltinTable& table, const Type& operand, BuiltinTypeID tested)
{
    const ClassPart* part = operand.singleBuiltinClass();
    if (!part || !tested)
        return { };
    const BuiltinTypePart* admitting = findPartAdmitting(table, table.at(tested), part->builtinClass);
    if (!admitting)
        return { };
    return { part->builtinClass, admitting->pristine - part->provenPristine };
}

namespace {

enum class BuiltinBaseState : uint8_t { Unknown, None, Found };

struct BuiltinBase {
    BuiltinBaseState state { BuiltinBaseState::Unknown };
    BuiltinClassID builtinClass;
};

class FitWalk {
public:
    FitWalk(const TypingContext& context, const Type& operand, const BuiltinType& tested)
        : m_context(context)
        , m_program(context.program)
        , m_table(context.program.builtins())
        , m_operand(operand)
        , m_tested(tested)
    {
    }

    TypeFit run();
    bool needsPristineProof() const { return m_needsPristineProof; }

private:
    bool has(BuiltinTypeFlag flag) const { return m_tested.flags.contains(flag); }
    bool has(BuiltinPrimitive primitive) const { return m_tested.primitives.contains(primitive); }
    bool hasPart(BuiltinTypePartKind kind) const { return std::ranges::contains(m_tested.parts, kind, &BuiltinTypePart::kind); }
    bool hasInstancePart() const { return hasPart(BuiltinTypePartKind::Instance); }

    bool hasInstancePartIf(NOESCAPE const Invocable<bool(const BuiltinTypePart&)> auto& predicate) const
    {
        return std::ranges::any_of(m_tested.parts, [&](const BuiltinTypePart& part) {
            return part.kind == BuiltinTypePartKind::Instance && predicate(part);
        });
    }

    void add(TypeFit fit)
    {
        m_isInside &= fit == TypeFit::Inside;
        m_isOutside &= fit == TypeFit::Outside;
    }

    TypeFit fitOfMember(BuiltinPrimitive member) const { return has(member) ? TypeFit::Inside : TypeFit::Outside; }
    TypeFit fitOfPrimitive(PrimitiveSet) const;
    TypeFit fitOfIntegers() const;
    TypeFit fitOfLiteralStrings() const;
    TypeFit fitOfFunctions() const;
    TypeFit fitOfBuiltin(BuiltinID) const;
    TypeFit fitOfLayout(LayoutID) const;
    TypeFit fitOfClassPart(const ClassPart&);
    TypeFit fitOfContents(const ClassPart&, const BuiltinTypePart& admitting) const;

    const TypingContext& m_context;
    const Program& m_program;
    const BuiltinTable& m_table;
    const Type& m_operand;
    const BuiltinType& m_tested;
    bool m_isInside { true };
    bool m_isOutside { true };
    bool m_needsPristineProof { false };
};

struct FitWithoutProofs {
    TypeFit fit { TypeFit::Unknown };
    bool needsPristineProof { false };
};

} // anonymous namespace

static constexpr TypeFit fitOfAtom(bool isCovered, bool isTouched)
{
    if (isCovered)
        return TypeFit::Inside;
    return isTouched ? TypeFit::Unknown : TypeFit::Outside;
}

static BuiltinBase builtinBaseOf(const Program& program, LayoutID layout)
{
    LayoutID current = layout;
    for (size_t steps = 0; steps < program.layouts.size(); ++steps) {
        const LayoutFlow& flow = program.at(current).flow;
        switch (flow.baseState) {
        case BaseState::Unknown:
            return { };
        case BaseState::None:
            return { BuiltinBaseState::None, { } };
        case BaseState::BuiltinClass:
            return { BuiltinBaseState::Found, flow.baseClass };
        case BaseState::Layout:
            current = flow.base;
            break;
        }
    }
    return { };
}

static bool isRelativeToOperands(const BuiltinTypePart& part)
{
    switch (part.kind) {
    case BuiltinTypePartKind::Instance:
    case BuiltinTypePartKind::Layout:
    case BuiltinTypePartKind::Builtin:
        return false;
    case BuiltinTypePartKind::InstanceOfReceiverClass:
    case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
    case BuiltinTypePartKind::Operand:
    case BuiltinTypePartKind::Slot:
    case BuiltinTypePartKind::IteratedValues:
    case BuiltinTypePartKind::CallbackResult:
    case BuiltinTypePartKind::Prototype:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

TypeFit FitWalk::run()
{
    for (PrimitiveSet primitive : OptionSet<PrimitiveSet>::fromRaw(m_operand.primitives))
        add(fitOfPrimitive(primitive));
    if (m_operand.anyObject)
        add(fitOfAtom(has(BuiltinTypeFlag::HasAnyObject), m_tested.mayBeObject()));
    if (hasCallableParts(m_operand))
        add(fitOfFunctions());
    for (BuiltinID builtin : m_operand.builtins)
        add(fitOfBuiltin(builtin));
    for (LayoutID layout : membersOf(m_program, m_operand))
        add(fitOfLayout(layout));
    for (const ClassPart& part : m_operand.classes)
        add(fitOfClassPart(part));

    if (m_isInside)
        return TypeFit::Inside;
    return m_isOutside ? TypeFit::Outside : TypeFit::Unknown;
}

TypeFit FitWalk::fitOfPrimitive(PrimitiveSet primitive) const
{
    switch (primitive) {
    case PrimitiveSet::Undefined:
        return fitOfMember(BuiltinPrimitive::Undefined);
    case PrimitiveSet::Null:
        return fitOfMember(BuiltinPrimitive::Null);
    case PrimitiveSet::False:
    case PrimitiveSet::True:
        return fitOfMember(BuiltinPrimitive::Boolean);
    case PrimitiveSet::Integer:
        return fitOfIntegers();
    case PrimitiveSet::WholeNumber:
    case PrimitiveSet::FractionalNumber:
        return has(BuiltinTypeFlag::HasOnlyIntegers) ? TypeFit::Outside : fitOfMember(BuiltinPrimitive::Number);
    case PrimitiveSet::String:
        return fitOfAtom(has(BuiltinPrimitive::String), !m_tested.stringLiterals.empty());
    case PrimitiveSet::LiteralString:
        return fitOfLiteralStrings();
    case PrimitiveSet::Symbol:
    case PrimitiveSet::LiteralSymbol:
        return fitOfMember(BuiltinPrimitive::Symbol);
    case PrimitiveSet::BigInt:
        return fitOfMember(BuiltinPrimitive::BigInt);
    case PrimitiveSet::Absent:
    case PrimitiveSet::Unspecified:
        return TypeFit::Outside;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

TypeFit FitWalk::fitOfIntegers() const
{
    if (!has(BuiltinPrimitive::Number))
        return TypeFit::Outside;
    Range accepted = integersOf(m_tested);
    return fitOfAtom(m_operand.integers.isSubsetOf(accepted), !m_operand.integers.intersectWith(accepted).isEmpty());
}

TypeFit FitWalk::fitOfLiteralStrings() const
{
    if (has(BuiltinPrimitive::String))
        return TypeFit::Inside;
    size_t numberOfListedLiterals = static_cast<size_t>(std::ranges::count_if(m_tested.stringLiterals, [&](BuiltinName name) {
        return m_operand.stringLiterals.contains(m_program.stringOf(name));
    }));
    return fitOfAtom(numberOfListedLiterals == m_operand.stringLiterals.size(), !!numberOfListedLiterals);
}

TypeFit FitWalk::fitOfFunctions() const
{
    bool isCovered = m_tested.flags.containsAny({ BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasAnyFunction });
    bool mayBeListedBuiltin = m_operand.anyFunction && hasPart(BuiltinTypePartKind::Builtin);
    return fitOfAtom(isCovered, mayBeListedBuiltin || hasInstancePartIf([&](const BuiltinTypePart& part) {
        return isFunctionClass(m_table, part.builtinClass);
    }));
}

TypeFit FitWalk::fitOfBuiltin(BuiltinID builtin) const
{
    bool isNamed = std::ranges::any_of(m_tested.parts, [&](const BuiltinTypePart& part) {
        return part.kind == BuiltinTypePartKind::Builtin && m_table.canonicalID(part.builtin) == m_table.canonicalID(builtin);
    });
    bool takesFunctions = has(BuiltinTypeFlag::HasAnyFunction);
    bool isCovered = has(BuiltinTypeFlag::HasAnyObject) || isNamed || (takesFunctions && isCallableBuiltin(m_table, builtin));
    return fitOfAtom(isCovered, takesFunctions || hasInstancePart());
}

TypeFit FitWalk::fitOfLayout(LayoutID layout) const
{
    bool mayBePlainData = has(BuiltinTypeFlag::HasPlainData);
    bool isNamed = std::ranges::any_of(m_tested.parts, [&](const BuiltinTypePart& part) {
        return part.kind == BuiltinTypePartKind::Layout && m_program.layoutOf(part.layout) == layout;
    });
    if (has(BuiltinTypeFlag::HasAnyObject) || isNamed)
        return TypeFit::Inside;

    BuiltinBase base = builtinBaseOf(m_program, layout);
    switch (base.state) {
    case BuiltinBaseState::Unknown:
        return fitOfAtom(false, mayBePlainData || hasInstancePart());
    case BuiltinBaseState::None:
        return fitOfAtom(false, mayBePlainData);
    case BuiltinBaseState::Found: {
        auto mayAdmit = [&](const BuiltinTypePart& part) {
            return part.pristine.isEmpty() || !m_table.isSameOrSubclassOf(base.builtinClass, part.builtinClass);
        };
        const BuiltinTypePart* admitting = findPartAdmitting(m_table, m_tested, base.builtinClass);
        bool isCovered = admitting && admitting->pristine.isEmpty() && admitting->contents.empty();
        return fitOfAtom(isCovered, mayBePlainData || hasInstancePartIf(mayAdmit));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

TypeFit FitWalk::fitOfContents(const ClassPart& part, const BuiltinTypePart& admitting) const
{
    bool areInside = std::ranges::all_of(admitting.contents, [&](const BuiltinSlotContents& tested) {
        std::optional<uint8_t> index = m_table.at(tested.slot).indexInType;
        if (!index || !part.contentsAreProven)
            return false;
        OperandFit fit = fitOf(m_context, *part.provenContentsAt(*index), tested.type);
        return fit.fit == TypeFit::Inside && !fit.unprovenPristine;
    });
    return areInside ? TypeFit::Inside : TypeFit::Unknown;
}

TypeFit FitWalk::fitOfClassPart(const ClassPart& part)
{
    BuiltinClassID builtinClass = part.builtinClass;
    bool takesFunctions = has(BuiltinTypeFlag::HasAnyFunction);
    if (has(BuiltinTypeFlag::HasAnyObject))
        return TypeFit::Inside;
    if (takesFunctions && m_table.isSameOrSubclassOf(builtinClass, BuiltinClasses::Function))
        return TypeFit::Inside;

    if (const BuiltinTypePart* admitting = findPartAdmitting(m_table, m_tested, builtinClass)) {
        if (fitOfContents(part, *admitting) != TypeFit::Inside)
            return TypeFit::Unknown;
        m_needsPristineProof |= !part.provenPristine.containsAll(admitting->pristine);
        return TypeFit::Inside;
    }

    bool mayBeFunction = takesFunctions && isFunctionClass(m_table, builtinClass);
    bool mayBePlainData = has(BuiltinTypeFlag::HasPlainData) && (m_table.isSameOrSubclassOf(BuiltinClasses::Array, builtinClass) || m_table.isSameOrSubclassOf(BuiltinClasses::Object, builtinClass));
    bool mayBeListedObject = hasPart(BuiltinTypePartKind::Builtin) || hasPart(BuiltinTypePartKind::Layout);
    return fitOfAtom(false, mayBeFunction || mayBePlainData || mayBeListedObject || hasInstancePartIf([&](const BuiltinTypePart& tested) {
        return m_table.isSameOrSubclassOf(tested.builtinClass, builtinClass);
    }));
}

static FitWithoutProofs fitWithoutProofs(const TypingContext& context, TypeID operandOrNotPassed, BuiltinTypeID testedID)
{
    const BuiltinTable& table = context.program.builtins();
    if (!testedID || table.at(testedID).isAnything())
        return { TypeFit::Inside, false };

    const BuiltinType& tested = table.at(testedID);
    if (std::ranges::any_of(tested.parts, isRelativeToOperands))
        return { TypeFit::Unknown, false };
    if (!operandOrNotPassed) {
        bool takesOmittedOperand = tested.primitives.containsAny({ BuiltinPrimitive::NotPassed, BuiltinPrimitive::Undefined });
        return { takesOmittedOperand ? TypeFit::Inside : TypeFit::Outside, false };
    }

    const Type& operand = context.types.at(operandOrNotPassed);
    if (operand.isNever())
        return { TypeFit::Inside, false };
    if (!operand.isJavaScriptValue())
        return { TypeFit::Unknown, false };

    FitWalk walk(context, operand, tested);
    TypeFit fit = walk.run();
    return { fit, fit == TypeFit::Inside && walk.needsPristineProof() };
}

OperandFit fitOf(const TypingContext& context, TypeID operandOrNotPassed, BuiltinTypeID tested)
{
    FitWithoutProofs unproven = fitWithoutProofs(context, operandOrNotPassed, tested);
    if (!unproven.needsPristineProof)
        return { unproven.fit, { } };
    const Type& operand = context.types.at(operandOrNotPassed);
    if (!operand.singleBuiltinClass())
        return { TypeFit::Unknown, { } };
    return { TypeFit::Inside, pristineRequirementOf(context.program.builtins(), operand, tested) };
}

bool fitsIfPristine(const TypingContext& context, TypeID operandOrNotPassed, BuiltinTypeID tested)
{
    return fitWithoutProofs(context, operandOrNotPassed, tested).fit == TypeFit::Inside;
}

Type typeOfBuiltinConstant(const BuiltinTable& table, BuiltinID id)
{
    BuiltinKind kind = table.at(id).kind;
    if (isObjectKind(kind))
        return Type::forBuiltin(table.canonicalID(id));
    if (kind != BuiltinKind::Constant)
        return Type::anything();
    if (std::optional<double> number = table.numberOf(id))
        return Type::number(*number);
    if (table.nameOfSymbol(id))
        return Type::fromPrimitives(Primitive::Symbol);
    return Type::anything();
}

TypedArrayType typedArrayTypeOf(const BuiltinTable& table, const Type& type)
{
    if (type.primitives & ~Primitive::UndefinedOrNull || !type.hasOnlyClassObjects() || type.classes.isEmpty())
        return NotTypedArray;
    TypedArrayType common = table.typedArrayTypeOf(type.classes[0].builtinClass);
    for (const ClassPart& part : type.classes) {
        if (table.typedArrayTypeOf(part.builtinClass) != common)
            return NotTypedArray;
    }
    return common;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
