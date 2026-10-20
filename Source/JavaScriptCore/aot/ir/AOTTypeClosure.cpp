/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeClosure.h"

#if ENABLE(AOT)

#include "AOTFlowSiteData.h"
#include "AOTKnownClosures.h"
#include "AOTSchemaResultRules.h"
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
    TypeID admitted = types.intern(kinds.type());
    declared.type = types.unionOf(declared.type, admitted);
    if (declared.written)
        declared.written = types.unionOf(declared.written, admitted);
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
        if (value.opcode != Opcode::ClaimDeclared)
            continue;
        std::optional<Type> entering = tryTypeWithoutOperands(function.at(definitionThroughSameValues(function, value.valueOperand())));
        if (!entering)
            continue;
        const Type& tested = program.at(value.declared().type);
        if (!entering->isSubsetOf(tested, program) && !(entering->hasObjects() && tested.hasObjects()))
            return true;
    }
    return false;
}

static Type typeOfMember(const DataNode& node)
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
    case DataNode::Kind::Object:
        return Type::object();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void forEachDataMemberThatAlwaysFailsConversion(const Program& program, TypeArena& types, NOESCAPE const ScopedLambda<void(FieldReference, ValueKinds)>& functor)
{
    for (const Data& data : program.data) {
        forEachDataNodeInPreorder(data.root, [&](uint32_t, const DataNode& node) {
            const auto* object = node.tryAs<DataNode::Object>();
            if (!object)
                return;
            for (const DataMember& member : object->members) {
                std::optional<FieldReference> field = findField(program, object->layout, member.key, BaseChain::Predicted);
                if (!field)
                    continue;
                Type entering = typeOfMember(member.value);
                if (alwaysFailsConversion(entering, program.at(fieldAt(program, *field).declared.type), types))
                    functor(*field, ValueKinds::of(entering));
            }
        });
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
