/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectTypedResultsTypeNodes.h"

#if ENABLE(AOT)

#include "AOTExpandOwnKeysOrders.h"
#include <algorithm>
#include <span>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

using TypeAlternatives = Vector<TypeAlternative, 4>;

TypeID expectationOf(const DeclaredType& declared)
{
    if (declared.predicted)
        return declared.predicted;
    return declared.written ? declared.written : declared.type;
}

static void appendBooleanAlternative(const Type& type, TypeAlternatives& alternatives)
{
    uint16_t booleans = type.primitives & Primitive::Boolean;
    if (!booleans)
        return;
    if (booleans == Primitive::Boolean) {
        alternatives.append(TypeAlternative { .shape = ResultShape::Boolean });
        return;
    }
    alternatives.append(TypeAlternative {
        .shape = ResultShape::BooleanLiteral,
        .literal = { .kind = ResultLiteral::Kind::Boolean, .boolean = booleans == Primitive::True },
    });
}

static void appendStringAlternatives(const Type& type, TypeAlternatives& alternatives)
{
    if (type.primitives & Primitive::String) {
        alternatives.append(TypeAlternative { .shape = ResultShape::String });
        return;
    }
    for (StringID string : type.stringLiterals) {
        alternatives.append(TypeAlternative {
            .shape = ResultShape::StringLiteral,
            .literal = { .kind = ResultLiteral::Kind::String, .string = string },
        });
    }
}

static void appendHeadLayouts(const Program& program, const Type& type, TypeAlternatives& alternatives)
{
    std::span<const LayoutID> members = membersOf(program, type);
    BitVector isSubtypeOfMember;
    for (LayoutID member : members) {
        for (LayoutID subtype : program.at(program.declaredMembersOf(member)).members) {
            if (subtype == member)
                continue;
            auto position = std::ranges::lower_bound(members, subtype);
            if (position != members.end() && *position == subtype)
                isSubtypeOfMember.set(static_cast<size_t>(position - members.begin()));
        }
    }

    for (size_t index = 0; index < members.size(); ++index) {
        if (program.at(members[index]).kind == LayoutKind::Record && !isSubtypeOfMember.get(index))
            alternatives.append(TypeAlternative { .shape = ResultShape::Object, .layout = members[index] });
    }
}

TypeAlternatives alternativesOf(const Program& program, TypeID id)
{
    const Type& type = program.at(id);
    TypeAlternatives alternatives;
    if (type.primitives & Primitive::Null)
        alternatives.append(TypeAlternative { .shape = ResultShape::Null });
    appendBooleanAlternative(type, alternatives);
    if (type.primitives & Primitive::Number)
        alternatives.append(TypeAlternative { .shape = ResultShape::Number });
    appendStringAlternatives(type, alternatives);
    if (const ClassPart* array = type.findClassPart(BuiltinClasses::Array))
        alternatives.append(TypeAlternative { .shape = ResultShape::Array, .contents = array->contentsAt(program.builtins(), ContentsSlot::Element) });
    appendHeadLayouts(program, type, alternatives);
    return alternatives;
}

static bool mayBeUndefined(const Program& program, TypeID type)
{
    return !type || program.at(type).primitives & Primitive::Undefined;
}

static void appendMembers(const Program& program, LayoutID layout, ResultNode& node)
{
    const Vector<Field>& fields = program.at(layout).fields;
    for (uint32_t index = 0; index < fields.size(); ++index) {
        const Field& field = fields[index];
        if (!isEnumerableStringField(program, field))
            continue;
        TypeID expectation = expectationOf(field.declared);
        node.members.append(ResultMember {
            .key = field.key,
            .node = ResultNodeRef::forType(expectation ? expectation : TypeTable::anything),
            .field = index,
            .isOptional = mayBeUndefined(program, field.declared.type),
            .mustBeOwn = false,
        });
    }
}

static ResultNode describeAlternative(const Program& program, TypeID type, const TypeAlternative& alternative)
{
    ResultNode node;
    node.shape = alternative.shape;
    node.type = type;
    node.literal = alternative.literal;
    switch (alternative.shape) {
    case ResultShape::Array:
        node.children.append(ResultNodeRef::forType(alternative.contents));
        return node;
    case ResultShape::Object:
        node.layout = alternative.layout;
        appendMembers(program, alternative.layout, node);
        return node;
    case ResultShape::Null:
    case ResultShape::String:
    case ResultShape::Number:
    case ResultShape::Boolean:
    case ResultShape::BooleanLiteral:
    case ResultShape::StringLiteral:
        return node;
    case ResultShape::Unknown:
    case ResultShape::Never:
    case ResultShape::Undefined:
    case ResultShape::AnyObject:
    case ResultShape::BigInt:
    case ResultShape::NumberLiteral:
    case ResultShape::Tuple:
    case ResultShape::Dictionary:
    case ResultShape::Union:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ResultNode describeTypeNode(const Program& program, TypeID type, uint32_t part)
{
    TypeAlternatives alternatives = alternativesOf(program, type);
    if (part) {
        RELEASE_ASSERT(part <= alternatives.size());
        return describeAlternative(program, type, alternatives[part - 1]);
    }

    ResultNode node;
    node.type = type;
    if (program.at(type).anyObject || alternatives.isEmpty())
        return node;
    if (alternatives.size() == 1)
        return describeAlternative(program, type, alternatives[0]);

    node.shape = ResultShape::Union;
    for (uint32_t index = 0; index < alternatives.size(); ++index)
        node.children.append(ResultNodeRef::forType(type, index + 1));
    return node;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
