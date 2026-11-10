/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResultRules.h"

#if ENABLE(AOT)

#include "AOTBody.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTypes.h"
#include "AOTCode.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTObjects.h"
#include "AOTVector.h"
#include <algorithm>
#include <span>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static TypeID typeOfBooleanConstant(bool value)
{
    return TypeArena::fromPrimitives(value ? Primitive::True : Primitive::False);
}

static TypeID typeOfIntegerConstant(TypeArena& types, int64_t integer)
{
    if (!Range::full().contains(integer))
        return TypeArena::fromPrimitives(Primitive::WholeNumber);
    return types.integer(Range::one(integer));
}

TypeID typeOfKeyConstant(const TypingContext& context, PropertyKeyID id)
{
    const PropertyKey& key = context.types.at(id);
    switch (key.kind) {
    case PropertyKeyKind::Text:
        return context.types.stringLiteral(key.text);
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return context.types.symbolLiteral(id);
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::ModulePrivateName:
    case PropertyKeyKind::PrivateName:
        return context.types.anything(UnknownSeed::UndescribedNativeResult);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static TypeID typeReadFromBuiltinDataProperty(const TypingContext& context, BuiltinID builtin)
{
    TypeArena& types = context.types;
    BuiltinTypeContext builtinTypes { context.program, types };
    TypeID result = TypeTable::never;
    for (const BuiltinOverload& overload : context.program.builtins().overloadsOf(builtin)) {
        if (overload.mode != BuiltinCallMode::Get)
            continue;
        if (context.level == TypeLevel::Proven && overload.resultIsDeclaredOnly())
            return types.anything(UnknownSeed::UndescribedNativeResult);
        result = types.unionOf(result, typeOfBuiltinType(builtinTypes, overload.result.type));
    }
    return isNeverType(result) ? types.anything(UnknownSeed::UndescribedNativeResult) : result;
}

TypeID typeOfBuiltinObject(const TypingContext& context, BuiltinID builtin)
{
    const BuiltinTable& builtins = context.program.builtins();
    TypeArena& types = context.types;
    if (compiledModuleWithObject(context.program, builtins.canonicalID(builtin)))
        return types.object(UnknownSeed::UndescribedNativeResult);
    if (BuiltinName symbol = builtins.nameOfSymbol(builtin))
        return types.symbolLiteral(context.program.keyOf(symbol));
    if (builtins.at(builtin).kind == BuiltinKind::DataProperty)
        return typeReadFromBuiltinDataProperty(context, builtin);
    return typeOfBuiltinConstant({ context.program, types }, builtin);
}

static bool hasCompleteSignature(const BuiltinTable& builtins, BuiltinID function)
{
    if (builtins.at(function).marks || builtins.letsInUnseenCode(function) || !builtins.calledFunctionsOf(function).empty() || !builtins.moduleLoadsOf(function).empty())
        return false;
    return std::ranges::all_of(builtins.overloadsOf(function), [](const BuiltinOverload& overload) {
        if (overload.mode == BuiltinCallMode::Get)
            return true;
        bool runsCode = overload.effects.capability == BuiltinCapability::JS && !overload.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode);
        return !overload.isUndeclared() && !runsCode && overload.flow.callbacks.empty() && overload.flow.handlerCalls.empty() && overload.flow.propertyActions.empty();
    });
}

static bool hasDescribedResult(const BuiltinTable& builtins, BuiltinID function)
{
    return std::ranges::any_of(builtins.overloadsOf(function), [&](const BuiltinOverload& overload) {
        if (overload.mode != BuiltinCallMode::Call || !overload.result.type)
            return false;
        const BuiltinType& answer = builtins.at(overload.result.type);
        return !answer.isAnything() && (!(answer.primitives - BuiltinPrimitive::Undefined).isEmpty() || !answer.stringLiterals.empty() || !answer.parts.empty());
    });
}

BuiltinID findNativeFunction(const BuiltinTable& builtins, BuiltinID binding)
{
    BuiltinID answered;
    for (const BuiltinOverload& overload : builtins.overloadsOf(binding)) {
        if (overload.mode != BuiltinCallMode::Get)
            continue;
        if (answered || overload.isUndeclared() || overload.resultIsDeclaredOnly() || !overload.result.type)
            return { };
        const BuiltinType& written = builtins.at(overload.result.type);
        if (!written.flags.isEmpty() || !written.primitives.isEmpty() || !written.stringLiterals.empty() || written.parts.size() != 1 || written.parts[0].kind != BuiltinTypePartKind::Builtin)
            return { };
        answered = builtins.canonicalID(written.parts[0].builtin);
        if (builtins.at(answered).kind != BuiltinKind::Function)
            return { };
    }
    bool isDescribed = answered && hasDescribedResult(builtins, answered) && hasCompleteSignature(builtins, binding) && hasCompleteSignature(builtins, answered);
    return isDescribed ? answered : BuiltinID();
}

static TypeID typeOfNativeBinding(const TypingContext& context, BuiltinID binding)
{
    BuiltinID function = findNativeFunction(context.program.builtins(), binding);
    return function ? context.types.forBuiltin(function) : context.types.anything(UnknownSeed::UndescribedNativeResult);
}

static TypeID typeOfBuiltinObjectOfModule(const TypingContext& context, BuiltinID builtin, ModuleID module)
{
    const BuiltinTable& builtins = context.program.builtins();
    TypeArena& types = context.types;
    BuiltinID canonical = builtins.canonicalID(builtin);
    if (!isObjectKind(builtins.at(canonical).kind))
        return typeOfBuiltinObject(context, builtin);
    if (ObjectID object = types.objects().objectOf(canonical, module))
        return types.forObject(object);
    return types.object(UnknownSeed::UndescribedNativeResult);
}

namespace {

struct WrittenContentsOfData {
    uint16_t primitives { 0 };
    Range integers;
    TypeID objects { TypeTable::never };
};

} // anonymous namespace

static TypeID typeOfArrayInData(const TypingContext&, const DataNode::Array&, DataID whoseRootItIs = { });

static void addToWrittenContentsOfData(const TypingContext& context, const DataNode& node, WrittenContentsOfData& contents)
{
    TypeArena& types = context.types;
    switch (node.kind()) {
    case DataNode::Kind::Undefined:
    case DataNode::Kind::Hole:
        contents.primitives |= Primitive::Undefined;
        return;
    case DataNode::Kind::Null:
        contents.primitives |= Primitive::Null;
        return;
    case DataNode::Kind::Boolean:
        contents.primitives |= node.as<DataNode::Boolean>().value ? Primitive::True : Primitive::False;
        return;
    case DataNode::Kind::Number: {
        double number = node.as<DataNode::Number>().value;
        contents.primitives |= static_cast<uint16_t>(primitiveOfNumber(number));
        if (isTrackedInteger(number))
            contents.integers = contents.integers.unionWith(Range::one(static_cast<int64_t>(number)));
        return;
    }
    case DataNode::Kind::String:
        contents.primitives |= Primitive::String;
        return;
    case DataNode::Kind::BigInt:
        contents.primitives |= Primitive::BigInt;
        return;
    case DataNode::Kind::Array:
        contents.objects = types.unionOf(contents.objects, typeOfArrayInData(context, node.as<DataNode::Array>()));
        return;
    case DataNode::Kind::Object: {
        LayoutID layout = node.as<DataNode::Object>().layout;
        contents.objects = types.unionOf(contents.objects, layout ? typeOfAllocatedLayout(context, layout) : TypeTable::object);
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static TypeID typeOfWrittenContentsOfData(TypeArena& types, const WrittenContentsOfData& contents)
{
    TypeParts parts;
    parts.primitives = contents.primitives;
    if (!contents.integers.isEmpty())
        parts.integers = { widenedMinimum(contents.integers.minimum), widenedMaximum(contents.integers.maximum) };
    return types.unionOf(types.intern(parts), contents.objects);
}

static TypeID typeOfArrayInData(const TypingContext& context, const DataNode::Array& array, DataID whoseRootItIs)
{
    TypeArena& types = context.types;
    auto askEachElement = [&] {
        WrittenContentsOfData contents;
        for (const DataNode& element : array.elements)
            addToWrittenContentsOfData(context, element, contents);
        return typeOfWrittenContentsOfData(types, contents);
    };

    TypeID written = array.contents;
    if (whoseRootItIs && array.elements.size() >= TypeArena::fewestElementsOfArrayWhoseContentsAreKeptForBuild) {
        if (std::optional<TypeID> remembered = types.rememberedContentsOfArrayInData(whoseRootItIs)) {
            written = *remembered;
            if constexpr (SharedAnswers::worksFoundAnswersOutAgain)
                RELEASE_ASSERT(askEachElement() == written);
        } else {
            written = askEachElement();
            types.rememberContentsOfArrayInData(whoseRootItIs, written);
        }
    } else if (!array.elements.isEmpty())
        written = askEachElement();
    if (!written || isNeverType(written))
        return types.builtinClass(BuiltinClasses::Array);
    return types.builtinClass(BuiltinClasses::Array, singleElementSpan(written));
}

TypeID typeOfData(const TypingContext& context, const DataNode& node, ObjectID created)
{
    TypeArena& types = context.types;
    switch (node.kind()) {
    case DataNode::Kind::Undefined:
        return TypeTable::undefined;
    case DataNode::Kind::Null:
        return TypeTable::null;
    case DataNode::Kind::Hole:
        return TypeTable::absent;
    case DataNode::Kind::Boolean:
        return typeOfBooleanConstant(node.as<DataNode::Boolean>().value);
    case DataNode::Kind::Number:
        return types.number(node.as<DataNode::Number>().value);
    case DataNode::Kind::String:
        return types.stringLiteral(node.as<DataNode::String>().value);
    case DataNode::Kind::BigInt:
        return TypeTable::bigInt;
    case DataNode::Kind::Array:
        return created ? types.forObject(created) : typeOfArrayInData(context, node.as<DataNode::Array>());
    case DataNode::Kind::Object: {
        if (created)
            return types.forObject(created);
        LayoutID layout = node.as<DataNode::Object>().layout;
        return layout ? typeOfAllocatedLayout(context, layout) : types.object(UnknownSeed::ObjectWithoutLayout);
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

TypeID typeOfAllocatedLayout(const TypingContext& context, LayoutID layout, ObjectID created)
{
    if (context.program.at(layout).isEnvironment())
        return context.types.forEnvironment(layout);
    if (context.program.at(layout).holdsOrdinaryObjects)
        return context.types.object(UnknownSeed::ObjectWithoutLayout);
    if (created)
        return context.types.forObject(created);
    return context.types.forLayout(layout);
}

TypeID typeOfDeclaredLayout(const TypingContext& context, LayoutID layout)
{
    TypeArena& types = context.types;
    if (context.level == TypeLevel::Proven)
        return types.object(UnknownSeed::ResultOfUnknownCallee);
    if (context.program.hasDeclaredMembers(layout))
        return types.forLayoutSet(context.program.declaredMembersOf(layout));

    Vector<LayoutID, 8> layouts { layout };
    layouts.append(context.program.layoutsExpectedBelow(layout));
    return types.forLayouts(layouts.span());
}

template<typename Operation>
ALWAYS_INLINE_INTO_OTHER_FILES TypeID typeOfConstant(const TypingContext& context, const Operation& operation, ObjectID created)
{
    TypeArena& types = context.types;
    switch (opcodeOf(operation)) {
    case Opcode::Undefined:
        return TypeTable::undefined;
    case Opcode::Null:
        return TypeTable::null;
    case Opcode::Absent:
        return TypeTable::absent;
    case Opcode::Boolean:
        return typeOfBooleanConstant(operation.boolean());
    case Opcode::Integer:
        return typeOfIntegerConstant(types, operation.integer());
    case Opcode::Double:
        return types.number(operation.number());
    case Opcode::String:
        return types.stringLiteral(operation.string());
    case Opcode::BigInt:
        return TypeTable::bigInt;
    case Opcode::Builtin:
        return typeOfBuiltinObject(context, operation.builtin());
    case Opcode::ModuleBuiltin:
        return typeOfBuiltinObjectOfModule(context, operation.builtin(), operation.module());
    case Opcode::NativeBinding:
        return typeOfNativeBinding(context, operation.builtin());
    case Opcode::KeyConstant:
        return typeOfKeyConstant(context, operation.key());
    case Opcode::Data: {
        const DataNode& root = context.program.at(operation.data()).root;
        if (const auto* array = created ? nullptr : root.tryAs<DataNode::Array>())
            return typeOfArrayInData(context, *array, operation.data());
        return typeOfData(context, root, created);
    }
    case Opcode::NewClosure:
        return created ? types.forObject(created) : types.forFunction(operation.function());
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template TypeID typeOfConstant<Draft>(const TypingContext&, const Draft&, ObjectID);
template TypeID typeOfConstant<Operation>(const TypingContext&, const Operation&, ObjectID);
template TypeID typeOfConstant<BodyValue>(const TypingContext&, const BodyValue&, ObjectID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
