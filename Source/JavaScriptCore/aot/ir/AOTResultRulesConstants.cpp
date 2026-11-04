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
#include "AOTObjects.h"
#include "AOTVector.h"
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
    // The module's own compiled code makes the object of such a module, so nothing that the table says of the built-in holds of it.
    if (compiledModuleWithObject(context.program, builtins.canonicalID(builtin)))
        return types.object(UnknownSeed::UndescribedNativeResult);
    if (BuiltinName symbol = builtins.nameOfSymbol(builtin))
        return types.symbolLiteral(context.program.keyOf(symbol));
    if (builtins.at(builtin).kind == BuiltinKind::DataProperty)
        return typeReadFromBuiltinDataProperty(context, builtin);
    return typeOfBuiltinConstant({ context.program, types }, builtin);
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
    // Each module has an object of its own. What stands for the built-in without a module is none of them.
    return types.object(UnknownSeed::UndescribedNativeResult);
}

namespace {

struct WrittenContentsOfData {
    uint16_t primitives { 0 };
    Range integers;
    TypeID objects { TypeTable::never };
};

} // anonymous namespace

static TypeID typeOfArrayInData(const TypingContext&, const DataNode::Array&);

static void addToWrittenContentsOfData(const TypingContext& context, const DataNode& node, WrittenContentsOfData& contents)
{
    TypeArena& types = context.types;
    switch (node.kind()) {
    // A hole reads as undefined.
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
    // What is stored later need not lie between the least and the greatest number written, so each bound goes to the next landmark.
    if (!contents.integers.isEmpty())
        parts.integers = { widenedMinimum(contents.integers.minimum), widenedMaximum(contents.integers.maximum) };
    return types.unionOf(types.intern(parts), contents.objects);
}

static TypeID typeOfArrayInData(const TypingContext& context, const DataNode::Array& array)
{
    TypeArena& types = context.types;
    WrittenContentsOfData contents;
    for (const DataNode& element : array.elements)
        addToWrittenContentsOfData(context, element, contents);

    TypeID written = array.elements.isEmpty() ? array.contents : typeOfWrittenContentsOfData(types, contents);
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
    // A NewDictionary stands for the allocation, and that names no layout.
    if (context.program.at(layout).holdsOrdinaryObjects)
        return context.types.object(UnknownSeed::ObjectWithoutLayout);
    if (created)
        return context.types.forObject(created);
    return context.types.forLayout(layout);
}

TypeID typeOfDeclaredLayout(const TypingContext& context, LayoutID layout)
{
    TypeArena& types = context.types;
    // The constructor of a parent may hand back any object.
    if (context.level == TypeLevel::Proven)
        return types.object(UnknownSeed::ResultOfUnknownCallee);
    if (context.program.hasDeclaredMembers(layout))
        return types.forLayoutSet(context.program.declaredMembersOf(layout));

    Vector<LayoutID, 8> layouts { layout };
    layouts.append(context.program.layoutsExpectedBelow(layout));
    return types.forLayouts(layouts.span());
}

template<typename Operation>
TypeID typeOfConstant(const TypingContext& context, const Operation& operation, ObjectID created)
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
    case Opcode::KeyConstant:
        return typeOfKeyConstant(context, operation.key());
    case Opcode::Data:
        return typeOfData(context, context.program.at(operation.data()).root, created);
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
