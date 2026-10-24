/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectTypedResultsTypeNodes.h"

#if ENABLE(AOT)

#include "AOTExpandOwnKeysOrders.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <functional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

using TypeAlternatives = Vector<TypeAlternative, 4>;

TypeID expectationOf(const DeclaredType& declared)
{
    return declared.type;
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

namespace {

struct FamilyOfMembers {
    LayoutSetID family;
    LayoutID owner;
    bool hasSeveralOwners { false };
};

class HeadCandidates {
public:
    HeadCandidates(const Program& program, std::span<const LayoutID> members)
    {
        for (LayoutID member : members) {
            if (program.at(member).kind == LayoutKind::Record)
                m_layouts.append(member);
        }
        m_numberLeft = m_layouts.size();
    }

    size_t numberLeft() const { return m_numberLeft; }

    void removeIf(NOESCAPE const Invocable<bool(LayoutID)> auto& functor)
    {
        compact();
        m_layouts.removeAllMatching(functor);
        m_numberLeft = m_layouts.size();
    }

    void remove(LayoutID layout)
    {
        auto position = std::ranges::lower_bound(m_layouts, layout);
        if (position == m_layouts.end() || *position != layout || m_isRemoved.set(static_cast<size_t>(position - m_layouts.begin())))
            return;
        --m_numberLeft;
    }

    Vector<LayoutID, 4> take()
    {
        compact();
        return std::exchange(m_layouts, { });
    }

private:
    void compact()
    {
        if (m_numberLeft == m_layouts.size())
            return;
        size_t index = 0;
        m_layouts.removeAllMatching([&](LayoutID) {
            return m_isRemoved.get(index++);
        });
        m_isRemoved.clearAll();
    }

    Vector<LayoutID, 4> m_layouts;
    BitVector m_isRemoved;
    size_t m_numberLeft { 0 };
};

} // anonymous namespace

static Vector<FamilyOfMembers> familiesOf(const Program& program, std::span<const LayoutID> members)
{
    Vector<FamilyOfMembers> families(members.size(), [&](size_t index) {
        return FamilyOfMembers { program.declaredMembersOf(members[index]), members[index] };
    });
    AOT::sort(families, { }, [](const FamilyOfMembers& family) {
        return std::pair { family.family, family.owner };
    });
    size_t numberOfFamilies = 0;
    for (const FamilyOfMembers& family : families) {
        if (numberOfFamilies && families[numberOfFamilies - 1].family == family.family)
            families[numberOfFamilies - 1].hasSeveralOwners = true;
        else
            families[numberOfFamilies++] = family;
    }
    families.shrink(numberOfFamilies);
    AOT::sort(families, std::ranges::greater { }, [&](const FamilyOfMembers& family) {
        return std::pair { program.at(family.family).members.size(), family.family };
    });
    return families;
}

Vector<LayoutID, 4> headLayoutsOf(const Program& program, const Type& type)
{
    std::span<const LayoutID> members = membersOf(program, type);
    HeadCandidates candidates(program, members);
    if (!candidates.numberLeft())
        return { };

    for (const FamilyOfMembers& family : familiesOf(program, members)) {
        if (!candidates.numberLeft())
            break;
        std::span<const LayoutID> subtypes = program.at(family.family).members.span();
        auto isOwnerAlone = [&](LayoutID layout) {
            return !family.hasSeveralOwners && layout == family.owner;
        };
        if (candidates.numberLeft() <= subtypes.size()) {
            candidates.removeIf([&](LayoutID candidate) {
                return !isOwnerAlone(candidate) && std::ranges::binary_search(subtypes, candidate);
            });
            continue;
        }
        for (LayoutID subtype : subtypes) {
            if (!isOwnerAlone(subtype))
                candidates.remove(subtype);
        }
    }
    return candidates.take();
}

static void appendHeadLayouts(const Program& program, const Type& type, TypeAlternatives& alternatives)
{
    for (LayoutID layout : headLayoutsOf(program, type))
        alternatives.append(TypeAlternative { .shape = ResultShape::Object, .layout = layout });
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

// What flow has proven about the arrays in a field says nothing about what a parser has to make. Fields that differ in that alone share a parser: the one of the type as it was read.
static TypeID withoutProofsAboutClasses(const Program& program, TypeID id)
{
    if (!id || TypeTable::isProvisional(id))
        return id;
    auto hasProof = [](const ClassPart& part) {
        return part.contentsAreProven || !part.provenPristine.isEmpty() || part.arrayStorage != ArrayStorage::Other;
    };
    if (std::ranges::none_of(program.at(id).classes, hasProof))
        return id;

    Type type = program.at(id);
    for (ClassPart& part : type.classes) {
        part.contentsAreProven = false;
        part.provenPristine = { };
        part.arrayStorage = ArrayStorage::Other;
    }
    type.canonicalize();
    return program.types.find(type).value_or(id);
}

static void appendMembers(const Program& program, LayoutID layout, ResultNode& node)
{
    const Vector<Field>& fields = program.at(layout).fields;
    for (uint32_t index = 0; index < fields.size(); ++index) {
        const Field& field = fields[index];
        if (!isEnumerableStringField(program, field))
            continue;
        TypeID expectation = withoutProofsAboutClasses(program, expectationOf(field.declared));
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
