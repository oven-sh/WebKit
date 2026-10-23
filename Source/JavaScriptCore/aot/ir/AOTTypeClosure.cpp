/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeClosure.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTFlowSiteData.h"
#include "AOTKnownClosures.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTSchemaResultRules.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

ValueKinds ValueKinds::of(const Type& type)
{
    uint8_t bits = 0;
    if (type.primitives & Primitive::Boolean)
        bits |= Boolean;
    if (type.primitives & Primitive::Number)
        bits |= Number;
    if (type.primitives & Primitive::AnyString)
        bits |= String;
    if (type.primitives & Primitive::AnySymbol)
        bits |= Symbol;
    if (type.primitives & Primitive::BigInt)
        bits |= BigInt;
    if (type.hasObjects())
        bits |= Object;
    return ValueKinds(bits);
}

Type ValueKinds::type() const
{
    uint16_t primitives = 0;
    if (m_bits & Boolean)
        primitives |= Primitive::Boolean;
    if (m_bits & Number)
        primitives |= Primitive::Number;
    if (m_bits & String)
        primitives |= Primitive::String;
    if (m_bits & Symbol)
        primitives |= Primitive::Symbol;
    if (m_bits & BigInt)
        primitives |= Primitive::BigInt;
    Type type = Type::fromPrimitives(primitives);
    if (m_bits & Object) {
        type.anyObject = true;
        type.anyFunction = true;
    }
    return type;
}

bool alwaysFailsConversion(const Type& entering, const Type& tested, TypeArena& types)
{
    return entering.isJavaScriptValue() && !ValueKinds::of(entering).isEmpty() && entering.castTo(tested, types).isNever();
}

ValueKinds kindsOutside(const Program& program, const Type& entering, const Type& tested, ValueKinds followed, TypeArena& types)
{
    ValueKinds outside;
    followed.forEachKind([&](ValueKinds kind) {
        Type part = entering.castTo(kind.type(), types);
        if (!part.isNever() && !part.isSubsetOf(tested, program))
            outside.add(kind);
    });
    return outside;
}

void admitKinds(DeclaredType& declared, ValueKinds kinds, TypeArena& types)
{
    declared.type = types.unionOf(declared.type, types.intern(kinds.type()));
}

static std::optional<Type> tryTypeWithoutOperands(const Value& value)
{
    switch (value.opcode) {
    case Opcode::Boolean:
        return Type::fromPrimitives(value.boolean() ? Primitive::True : Primitive::False);
    case Opcode::Integer:
        if (!Range::full().contains(value.integer()))
            return Type::fromPrimitives(Primitive::WholeNumber);
        return Type::integer(Range::one(value.integer()));
    case Opcode::Double:
        return Type::number(value.number());
    case Opcode::String:
        return Type::stringLiteral(value.string());
    case Opcode::BigInt:
        return Type::fromPrimitives(Primitive::BigInt);
    default:
        if (resultRuleOf(value.opcode) == ResultRule::Allocation)
            return Type::object();
        return std::nullopt;
    }
}

bool mayHaveConversionThatAlwaysFails(const Program& program, const Function& function)
{
    for (const Value& value : function.values) {
        if (value.opcode != Opcode::Convert)
            continue;
        DeclaredType declared = value.declared();
        if (declared.flags.contains(PositionFlags::IsExpected))
            continue;
        std::optional<Type> entering = tryTypeWithoutOperands(function.at(definitionThroughSameValues(function, value.valueOperand())));
        if (!entering)
            continue;
        const Type& tested = program.at(declared.type);
        if (!entering->isSubsetOf(tested, program) && !(entering->hasObjects() && tested.hasObjects()))
            return true;
    }
    return false;
}

static Type typeOfMember(const DataNode& node, TypeArena& types)
{
    switch (node.kind()) {
    case DataNode::Kind::Undefined:
    case DataNode::Kind::Null:
    case DataNode::Kind::Hole:
        return Type::never();
    case DataNode::Kind::Boolean:
        return Type::fromPrimitives(node.as<DataNode::Boolean>().value ? Primitive::True : Primitive::False);
    case DataNode::Kind::Number:
        return Type::number(node.as<DataNode::Number>().value);
    case DataNode::Kind::String:
        return Type::stringLiteral(node.as<DataNode::String>().value);
    case DataNode::Kind::BigInt:
        return Type::fromPrimitives(Primitive::BigInt);
    case DataNode::Kind::Array:
        return Type::builtinClass(BuiltinClasses::Array);
    case DataNode::Kind::Object:
        return node.as<DataNode::Object>().layout ? Type::forLayout(node.as<DataNode::Object>().layout, types) : Type::object();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void forEachDataMemberThatAlwaysFailsConversion(const Program& program, TypeArena& types, NOESCAPE const ScopedLambda<void(FieldReference, ValueKinds)>& functor)
{
    for (const Data& data : program.data) {
        forEachDataNodeInPreorder(data.root, [&](uint32_t, const DataNode& node) {
            const auto* object = node.tryAs<DataNode::Object>();
            if (!object || !object->layout)
                return;
            for (const DataMember& member : object->members) {
                std::optional<FieldReference> field = findField(program, object->layout, member.key, BaseChain::Predicted);
                if (!field)
                    continue;
                const DeclaredType& declared = fieldAt(program, *field).declared;
                if (declared.flags.contains(PositionFlags::IsExpected))
                    continue;
                Type entering = typeOfMember(member.value, types);
                if (alwaysFailsConversion(entering, program.at(declared.type), types))
                    functor(*field, ValueKinds::of(entering));
            }
        });
    }
}

static bool includesLayout(const Program& program, const Type& type, LayoutID layout)
{
    return type.anyObject || (type.layouts && std::ranges::binary_search(program.at(type.layouts).members, layout));
}

// An array is an object without a layout. What is proven about its contents and its storage is flow's to answer for: the slot holds any array.
static bool includesArrays(const Program& program, const Type& type)
{
    return type.anyObject || type.findClassPart(BuiltinClasses::Array) || hasMembersWithoutLayout(program, type);
}

bool includesData(const Program& program, const Type& type, const DataNode& node)
{
    switch (node.kind()) {
    case DataNode::Kind::Undefined:
        return type.primitives & Primitive::Undefined;
    case DataNode::Kind::Null:
        return type.primitives & Primitive::Null;
    case DataNode::Kind::Hole:
        return type.primitives & Primitive::Absent;
    case DataNode::Kind::Boolean:
        return type.primitives & (node.as<DataNode::Boolean>().value ? Primitive::True : Primitive::False);
    case DataNode::Kind::Number:
        return Type::number(node.as<DataNode::Number>().value).isSubsetOf(type, program);
    case DataNode::Kind::String:
        return Type::stringLiteral(node.as<DataNode::String>().value).isSubsetOf(type, program);
    case DataNode::Kind::BigInt:
        return type.primitives & Primitive::BigInt;
    case DataNode::Kind::Array:
        return includesArrays(program, type);
    case DataNode::Kind::Object:
        return node.as<DataNode::Object>().layout ? includesLayout(program, type, node.as<DataNode::Object>().layout) : type.anyObject;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void expectFieldsFilledWithOtherData(Program& program, BuildContext& build)
{
    Vector<FieldReference> fields = collectInParallel<FieldReference>(build.parallelLoops(), program.data.size(), [&](unsigned index, Vector<FieldReference>& found) {
        forEachDataNodeInPreorder(program.data[index].root, [&](uint32_t, const DataNode& node) {
            const auto* object = node.tryAs<DataNode::Object>();
            if (!object || !object->layout)
                return;
            const Layout& layout = program.at(object->layout);
            for (const DataMember& member : object->members) {
                std::optional<uint32_t> field = layout.findField(member.key);
                if (!field)
                    continue;
                const DeclaredType& declared = layout.fields[*field].declared;
                if (!declared.flags.contains(PositionFlags::IsExpected) && !includesData(program, program.at(slotTypeOf(program, layout.fields[*field])), member.value))
                    found.append({ object->layout, *field });
            }
        });
    });
    for (FieldReference filled : fields) {
        for (FieldReference field : { filled, declarationOf(program, filled) }) {
            DeclaredType& declared = program.at(field.layout).fields[field.field].declared;
            declared.flags.add(PositionFlags::IsExpected);
            declared.flags.remove(PositionFlags::IsInt32Promise);
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
