/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTType.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTRepresentation.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include <algorithm>
#include <bit>
#include <iterator>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

TypeID ClassPart::writtenContentsAt(unsigned positionInType) const
{
    if (positionInType >= writtenContents.size() || !writtenContents[positionInType])
        return TypeTable::anything;
    return writtenContents[positionInType];
}

TypeID ClassPart::writtenContentsAt(const BuiltinTable& builtins, ContentsSlot slot) const
{
    std::optional<uint8_t> positionInType = positionInTypeOf(builtins, builtinClass, slot);
    return positionInType ? writtenContentsAt(*positionInType) : TypeTable::anything;
}

bool Type::hasCells() const
{
    if (hasObjects() || environment || primitives & cellPrimitives)
        return true;
    return opaque && holdsCells(representationOf(*opaque));
}

static constexpr std::pair<Wire::Primitives, uint16_t> wirePrimitives[] = {
    { Wire::Primitives::Undefined, Primitive::Undefined },
    { Wire::Primitives::Null, Primitive::Null },
    { Wire::Primitives::False, Primitive::False },
    { Wire::Primitives::True, Primitive::True },
    { Wire::Primitives::Number, Primitive::Number },
    { Wire::Primitives::String, Primitive::String },
    { Wire::Primitives::Symbol, Primitive::Symbol },
    { Wire::Primitives::BigInt, Primitive::BigInt },
    { Wire::Primitives::Absent, Primitive::Absent },
};
static_assert(std::size(wirePrimitives) == numberOfEnumerators<Wire::Primitives>);

uint16_t fromWire(Wire::Primitives wire)
{
    uint16_t primitives = 0;
    for (auto [wireBit, bits] : wirePrimitives) {
        if (static_cast<uint32_t>(wire) & static_cast<uint32_t>(wireBit))
            primitives |= bits;
    }
    return primitives;
}

Wire::Primitives toWire(uint16_t primitives)
{
    uint32_t wire = 0;
    for (auto [wireBit, bits] : wirePrimitives) {
        if ((primitives & bits) == bits)
            wire |= static_cast<uint32_t>(wireBit);
    }
    return static_cast<Wire::Primitives>(wire);
}

TypeReader::TypeReader(const Program& program)
    : m_table(&program.types)
{
}

template<typename MemberID, typename SetID>
static MemberID onlyMemberOfSetInType(SetID set)
{
    return holdsOneMember(set) ? MemberID(positionOf(set)) : MemberID();
}

const ClassPart* findClassPart(TypeReader reader, const Type& type, BuiltinClassID builtinClass)
{
    std::span<const ClassPart> parts = classPartsOf(reader, type);
    auto part = std::ranges::find(parts, builtinClass, &ClassPart::builtinClass);
    return part == parts.end() ? nullptr : &*part;
}

template<typename Name, typename NameOf>
static Name nameSharedByAllObjectsOfType(const SetView<ObjectID, ObjectSetID>& objects, const NameOf& nameOf)
{
    Name shared;
    bool allShareIt = objects.allOf([&](ObjectID object) {
        Name name = nameOf(object);
        if (!shared)
            shared = name;
        return name && name == shared;
    });
    return allShareIt ? shared : Name();
}

static void forEachObjectOfTypeNumberedBelow(const SetView<ObjectID, ObjectSetID>& objects, uint32_t end, NOESCAPE const Invocable<void(ObjectID)> auto& functor)
{
    objects.anyOf([&](ObjectID object) {
        if (object.index() >= end)
            return true;
        functor(object);
        return false;
    });
}

template<typename Name, typename NameOf>
static void forEachDistinctNameAmongObjectsOfType(const SetView<ObjectID, ObjectSetID>& objects, uint32_t end, const NameOf& nameOf, const ScopedLambda<void(Name)>& functor)
{
    Vector<Name, 64> names;
    forEachObjectOfTypeNumberedBelow(objects, end, [&](ObjectID object) {
        if (Name name = nameOf(object))
            names.append(name);
    });
    if (!std::ranges::is_sorted(names))
        std::ranges::sort(names);

    Name previous;
    for (Name name : names) {
        if (name != previous)
            functor(name);
        previous = name;
    }
}

static NEVER_INLINE FunctionID functionSharedBySeveralClosures(TypeReader reader, const Type& type)
{
    const ObjectTable& objects = reader.objects();
    return nameSharedByAllObjectsOfType<FunctionID>(reader.at(type.objects), [&](ObjectID object) {
        return objects.closureFunction(object);
    });
}

FunctionID functionSharedByClosuresOf(TypeReader reader, const Type& type)
{
    if (ObjectID onlyObject = onlyMemberOfSetInType<ObjectID>(type.objects))
        return reader.objects().closureFunction(onlyObject);
    return functionSharedBySeveralClosures(reader, type);
}

static NEVER_INLINE BuiltinID builtinSharedBySeveralObjects(TypeReader reader, const Type& type)
{
    const ObjectTable& objects = reader.objects();
    return nameSharedByAllObjectsOfType<BuiltinID>(reader.at(type.objects), [&](ObjectID object) {
        return objects.builtinOf(object);
    });
}

BuiltinID builtinSharedByObjectsOf(TypeReader reader, const Type& type)
{
    if (ObjectID onlyObject = onlyMemberOfSetInType<ObjectID>(type.objects))
        return reader.objects().builtinOf(onlyObject);
    return builtinSharedBySeveralObjects(reader, type);
}

ObjectID singleObjectOf(const Type& type)
{
    if (!type.hasOnlyNamedObjects() || type.primitives & ~Primitive::UndefinedOrNull)
        return { };
    return onlyMemberOfSetInType<ObjectID>(type.objects);
}

StringID singleStringLiteralOf(const Type& type)
{
    if (!type.hasOnlyPrimitives(Primitive::LiteralString))
        return { };
    return onlyMemberOfSetInType<StringID>(type.stringLiterals);
}

static bool standsForOneSymbolInTypes(PropertyKeyKind kind)
{
    switch (kind) {
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return true;
    case PropertyKeyKind::Text:
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::ModulePrivateName:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool isSingleValue(TypeReader reader, const Type& type)
{
    if (!type.isJavaScriptValue())
        return false;
    if (type.hasObjects()) {
        ObjectID object = singleObjectOf(type);
        if (!object || type.primitives)
            return false;
        const ObjectRecord& record = reader.at(object);
        return record.has(ObjectTrait::IsFoundByIdentity) && !record.has(ObjectTrait::IsMerged);
    }
    switch (type.primitives) {
    case Primitive::Undefined:
    case Primitive::Null:
    case Primitive::False:
    case Primitive::True:
    case Primitive::Absent:
        return true;
    case Primitive::Integer:
        return type.integers.isSingleValue();
    case Primitive::LiteralString:
        return holdsOneMember(type.stringLiterals);
    case Primitive::LiteralSymbol: {
        PropertyKeyID key = onlyMemberOfSetInType<PropertyKeyID>(type.symbolLiterals);
        return key && standsForOneSymbolInTypes(reader.at(key).kind);
    }
    default:
        return false;
    }
}

void forEachFunctionAmongClosuresOf(TypeReader reader, const Type& type, NOESCAPE const ScopedLambda<void(FunctionID)>& functor)
{
    const ObjectTable& objects = reader.objects();
    SetView<ObjectID, ObjectSetID> named = reader.at(type.objects);
    auto functionOf = [&](ObjectID object) {
        return objects.closureFunction(object);
    };
    uint32_t end = objects.endOf(ObjectKind::Closure);
    forEachDistinctNameAmongObjectsOfType(named, end, functionOf, functor);
}

void forEachBuiltinOf(TypeReader reader, const Type& type, NOESCAPE const ScopedLambda<void(BuiltinID)>& functor)
{
    if (!type.traitsOfNamed.contains(ObjectTrait::IsBuiltin))
        return;
    const ObjectTable& objects = reader.objects();
    uint32_t end = objects.endOf(ObjectKind::ModuleBuiltin);
    forEachDistinctNameAmongObjectsOfType(reader.at(type.objects), end, [&](ObjectID object) {
        return objects.builtinOf(object);
    }, functor);
}

bool namesFunction(TypeReader reader, const Type& type, FunctionID function)
{
    if (!function || !type.traitsOfNamed.contains(ObjectTrait::IsClosure))
        return false;
    return reader.at(type.objects).contains(reader.objects().closureOf(function));
}

bool namesBuiltin(TypeReader reader, const Type& type, BuiltinID builtin)
{
    if (!builtin || !type.traitsOfNamed.contains(ObjectTrait::IsBuiltin))
        return false;
    const ObjectTable& objects = reader.objects();
    const BuiltinTable& builtins = reader.builtins();
    BuiltinID canonical = builtins.canonicalID(builtin);
    SetView<ObjectID, ObjectSetID> named = reader.at(type.objects);
    if (named.contains(objects.objectOf(canonical)))
        return true;
    if (builtins.at(canonical).scope != BuiltinScope::Module)
        return false;
    return named.anyOf([&](ObjectID object) {
        return objects.builtinOf(object) == canonical;
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
