/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedType.h"

#if ENABLE(AOT)

#include "AOTTypedIR.h"

namespace JSC { namespace AOT { namespace Typed {

Type Type::ofPrimitives(uint16_t primitives)
{
    Type type;
    type.primitives = primitives;
    if (primitives & Primitive::Int)
        type.integers = Range::full();
    return type;
}

Type Type::integer(Range range)
{
    Type type;
    if (range.isEmpty())
        return type;
    type.primitives = Primitive::Int;
    type.integers = range;
    return type;
}

Type Type::view(LayoutID layout)
{
    Type type;
    type.views.append(layout);
    return type;
}

Type Type::engineClass(const EngineClass& engineClass)
{
    Type type;
    type.engineClasses.append(engineClass.index);
    return type;
}

Type Type::function()
{
    Type type;
    type.anyFunction = true;
    return type;
}

Type Type::object()
{
    Type type;
    type.anyObject = true;
    type.anyFunction = true;
    return type;
}

Type Type::anything()
{
    Type type = ofPrimitives(Primitive::All);
    type.anyObject = true;
    type.anyFunction = true;
    return type;
}

LayoutID Type::onlyView() const
{
    if (primitives & ~Primitive::UndefinedOrNull || anyObject || anyFunction || !engineClasses.isEmpty() || views.size() != 1)
        return { };
    return views[0];
}

const EngineClass* Type::onlyEngineClass() const
{
    if (primitives & ~Primitive::UndefinedOrNull || anyObject || anyFunction || !views.isEmpty() || engineClasses.size() != 1)
        return nullptr;
    return &Builtins::singleton().classes()[engineClasses[0]];
}

Type Type::withoutPrimitives(uint16_t removed) const
{
    Type type = *this;
    type.primitives &= ~removed;
    if (!(type.primitives & Primitive::Int))
        type.integers = Range::empty();
    return type;
}

Type Type::withLiteralStringWidened() const
{
    Type type = *this;
    if (type.primitives & Primitive::LiteralString)
        type.primitives = (type.primitives & ~Primitive::LiteralString) | Primitive::String;
    return type;
}

void Type::normalizeLiteralString()
{
    if (primitives & Primitive::LiteralString && (hasObjects() || primitives & ~(Primitive::LiteralString | Primitive::UndefinedOrNull | Primitive::Absent)))
        primitives = (primitives & ~Primitive::LiteralString) | Primitive::String;
}

bool Type::admitsForeignObjects(const Program& program) const
{
    for (LayoutID view : views) {
        if (program.at(view).admitsForeignObjects)
            return true;
    }
    return false;
}

bool Type::admitsUncheckedObjects(const Program& program) const
{
    for (LayoutID view : views) {
        if (program.at(view).admitsUncheckedObjects())
            return true;
    }
    return false;
}

bool Type::admitsOnlyOtherLayouts(const Program& program) const
{
    bool admitsThem = false;
    for (LayoutID view : views) {
        if (program.at(view).admitsForeignObjects && !program.at(view).admitsOnlyOtherLayouts)
            return false;
        admitsThem |= program.at(view).admitsOnlyOtherLayouts;
    }
    return admitsThem;
}

bool Type::isWithin(const Type& other, const Program& program, bool foreignObjectsPass) const
{
    uint16_t lacked = primitives & ~other.primitives;
    if (other.primitives & Primitive::String)
        lacked &= ~Primitive::LiteralString;
    if (lacked)
        return false;
    if (primitives & Primitive::Int && !integers.isWithin(other.integers))
        return false;
    if (other.anyObject || (foreignObjectsPass && other.admitsUncheckedObjects(program)))
        return true;
    if (anyObject || (anyFunction && !other.anyFunction))
        return false;
    for (uint16_t engineClass : engineClasses) {
        if (!other.engineClasses.contains(engineClass))
            return false;
    }
    for (LayoutID view : views) {
        // It stands for any object.
        if (program.at(view).admitsUncheckedObjects())
            return false;
        bool holdsOtherLayouts = program.at(view).admitsOnlyOtherLayouts;
        // The members say which objects there are of a view of objects. An environment is of its own layout and no other.
        bool goesByMembers = isObjectLayout(program.at(view).kind);
        bool isWithinOne = false;
        for (LayoutID otherView : other.views)
            isWithinOne |= view == otherView || (goesByMembers && std::ranges::includes(program.at(otherView).members, program.at(view).members) && (!holdsOtherLayouts || program.at(otherView).admitsOnlyOtherLayouts));
        if (!isWithinOne)
            return false;
    }
    return true;
}

void add(Hasher& hasher, const Type& type)
{
    add(hasher, type.primitives, type.integers.least, type.integers.most, type.anyObject, type.anyFunction, type.elements);
    for (LayoutID view : type.views)
        add(hasher, view);
    for (uint16_t engineClass : type.engineClasses)
        add(hasher, engineClass);
}

TypeTable::TypeTable()
{
}

TypeID TypeTable::intern(const Type& type)
{
    unsigned hash = computeHash(type);
    auto& candidates = m_byHash.add(hash, Vector<TypeID, 1>()).iterator->value;
    for (TypeID candidate : candidates) {
        if (m_types[candidate.index()] == type)
            return candidate;
    }
    RELEASE_ASSERT(!m_isFrozen);
    TypeID id(m_types.size());
    m_types.append(type);
    candidates.append(id);
    return id;
}

Rep repOf(const Type& type)
{
    if (type.isNever())
        return Rep::JSValue;
    if (type.hasLiteralString()) {
        RELEASE_ASSERT(type.hasOnlyPrimitives(Primitive::LiteralString | Primitive::UndefinedOrNull | Primitive::Absent));
        return Rep::LiteralString;
    }
    if (type.isBoolean())
        return Rep::Int32;
    if (type.isInteger())
        return Rep::Int64;
    if (type.isNumber())
        return Rep::Double;
    if (type.primitives & Primitive::Number && type.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull | Primitive::Absent))
        return type.primitives & Primitive::Double ? Rep::OptionalDouble : Rep::OptionalInt64;
    if (!(type.primitives & ~(Primitive::String | Primitive::Symbol | Primitive::BigInt)))
        return Rep::Cell;
    return Rep::JSValue;
}

Store storeOf(const Type& type)
{
    switch (repOf(type)) {
    case Rep::Int32:
        return Store::Boolean;
    case Rep::Int64: {
        auto fits = [&](int64_t least, int64_t most) {
            return type.integers.isWithin({ least, most });
        };
        if (fits(0, UINT8_MAX))
            return Store::Uint8;
        if (fits(INT8_MIN, INT8_MAX))
            return Store::Int8;
        if (fits(0, UINT16_MAX))
            return Store::Uint16;
        if (fits(INT16_MIN, INT16_MAX))
            return Store::Int16;
        if (fits(INT32_MIN, INT32_MAX))
            return Store::Int32;
        if (fits(0, UINT32_MAX))
            return Store::Uint32;
        return Store::Int64;
    }
    case Rep::Double:
        return Store::Double;
    case Rep::OptionalInt64:
        return Store::OptionalInt64;
    case Rep::OptionalDouble:
        return Store::OptionalDouble;
    case Rep::Cell:
    case Rep::Void:
        return Store::Cell;
    case Rep::JSValue:
        return Store::JSValue;
    case Rep::LiteralString:
        return Store::LiteralString;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(Rep rep)
{
    switch (rep) {
    case Rep::Void:
        return "void"_s;
    case Rep::Int32:
        return "i32"_s;
    case Rep::Int64:
        return "i64"_s;
    case Rep::Double:
        return "f64"_s;
    case Rep::OptionalInt64:
        return "i64?"_s;
    case Rep::OptionalDouble:
        return "f64?"_s;
    case Rep::Cell:
        return "ref"_s;
    case Rep::JSValue:
        return "val"_s;
    case Rep::LiteralString:
        return "lit"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(Store store)
{
    switch (store) {
    case Store::Uint8:
        return "u8"_s;
    case Store::Int8:
        return "i8"_s;
    case Store::Uint16:
        return "u16"_s;
    case Store::Int16:
        return "i16"_s;
    case Store::Int32:
        return "i32"_s;
    case Store::Uint32:
        return "u32"_s;
    case Store::Int64:
        return "i64"_s;
    case Store::Float:
        return "f32"_s;
    case Store::Double:
        return "f64"_s;
    case Store::Cell:
        return "ref"_s;
    case Store::JSValue:
        return "val"_s;
    case Store::OptionalInt64:
        return "i64?"_s;
    case Store::OptionalDouble:
        return "f64?"_s;
    case Store::Boolean:
        return "bool"_s;
    case Store::Int32OrElsewhere:
        return "i32|"_s;
    case Store::Cell32:
        return "ref32"_s;
    case Store::OptionalCell32:
        return "ref32?"_s;
    case Store::OptionalBoolean:
        return "bool?"_s;
    case Store::LiteralString:
        return "lit"_s;
    case Store::LiteralString32:
        return "lit32"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

unsigned sizeOf(Store store)
{
    switch (store) {
    case Store::Uint8:
    case Store::Int8:
    case Store::Boolean:
    case Store::OptionalBoolean:
        return 1;
    case Store::Uint16:
    case Store::Int16:
        return 2;
    case Store::Int32:
    case Store::Uint32:
    case Store::Float:
    case Store::Int32OrElsewhere:
    case Store::Cell32:
    case Store::OptionalCell32:
    case Store::LiteralString32:
        return 4;
    case Store::Int64:
    case Store::Double:
    case Store::Cell:
    case Store::JSValue:
    case Store::OptionalInt64:
    case Store::OptionalDouble:
    case Store::LiteralString:
        return 8;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Rep repOf(Store store)
{
    switch (store) {
    case Store::Uint8:
    case Store::Int8:
    case Store::Uint16:
    case Store::Int16:
    case Store::Int32:
    case Store::Boolean:
    // The word as it is. What it stands for takes the object: Lowering::readInt32OrElsewhere().
    case Store::Int32OrElsewhere:
        return Rep::Int32;
    case Store::Uint32:
    case Store::Int64:
        return Rep::Int64;
    case Store::Float:
    case Store::Double:
        return Rep::Double;
    case Store::Cell:
    case Store::Cell32:
        return Rep::Cell;
    case Store::JSValue:
    case Store::OptionalCell32:
    case Store::OptionalBoolean:
        return Rep::JSValue;
    case Store::OptionalInt64:
        return Rep::OptionalInt64;
    case Store::OptionalDouble:
        return Rep::OptionalDouble;
    case Store::LiteralString:
    case Store::LiteralString32:
        return Rep::LiteralString;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
