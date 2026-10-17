/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTypedBuiltins.h"
#include "AOTTypedData.h"
#include "AOTTypedID.h"
#include <wtf/SegmentedVector.h>
#include <wtf/HashMap.h>
#include <wtf/Hasher.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace Primitive {
static constexpr uint16_t Undefined = 1 << 0;
static constexpr uint16_t Null = 1 << 1;
static constexpr uint16_t False = 1 << 2;
static constexpr uint16_t True = 1 << 3;
static constexpr uint16_t Int = 1 << 4;
static constexpr uint16_t Double = 1 << 5;
static constexpr uint16_t String = 1 << 6;
static constexpr uint16_t Symbol = 1 << 7;
static constexpr uint16_t BigInt = 1 << 8;
static constexpr uint16_t Absent = 1 << 9;
static constexpr uint16_t LiteralString = 1 << 10;

static constexpr uint16_t Boolean = False | True;
static constexpr uint16_t Number = Int | Double;
static constexpr uint16_t UndefinedOrNull = Undefined | Null;
static constexpr uint16_t All = (1 << 9) - 1;
}

static constexpr int64_t maxExactInteger = 1ll << 53;
static constexpr int64_t maxTrackedInteger = 1ll << 62;

struct Range {
    int64_t minimum { 1 };
    int64_t maximum { 0 };

    static constexpr Range full() { return { -maxTrackedInteger, maxTrackedInteger }; }
    static constexpr Range int32() { return { INT32_MIN, INT32_MAX }; }
    static constexpr Range uint32() { return { 0, UINT32_MAX }; }
    static constexpr Range length() { return { 0, INT32_MAX }; }
    static constexpr Range empty() { return { 1, 0 }; }
    static constexpr Range one(int64_t value) { return { value, value }; }

    constexpr bool isEmpty() const { return minimum > maximum; }
    constexpr bool isSubsetOf(Range other) const { return isEmpty() || (minimum >= other.minimum && maximum <= other.maximum); }
    constexpr bool contains(int64_t value) const { return minimum <= value && value <= maximum; }
    constexpr Range unionWith(Range other) const
    {
        if (isEmpty())
            return other;
        if (other.isEmpty())
            return *this;
        return { std::min(minimum, other.minimum), std::max(maximum, other.maximum) };
    }
    constexpr Range intersectWith(Range other) const { return { std::max(minimum, other.minimum), std::min(maximum, other.maximum) }; }
    friend constexpr bool operator==(Range, Range) = default;
};

class Program;

struct Type {
    uint16_t primitives { 0 };
    Range integers;
    bool anyObject { false };
    bool anyFunction { false };
    Vector<LayoutID, 1> objectLayouts;
    Vector<uint16_t, 1> engineClasses;
    TypeID elements;

    static Type never() { return { }; }
    static Type fromPrimitives(uint16_t);
    static Type integer(Range);
    static Type forLayout(LayoutID);
    static Type engineClass(const EngineClass&);
    static Type function();
    static Type object();
    static Type anything();

    bool isNever() const { return !primitives && !hasObjects(); }
    bool isAnything() const { return (primitives & Primitive::All) == Primitive::All && anyObject && anyFunction; }
    bool hasObjects() const { return anyObject || anyFunction || !objectLayouts.isEmpty() || !engineClasses.isEmpty(); }
    bool hasCells() const { return hasObjects() || primitives & (Primitive::String | Primitive::Symbol | Primitive::BigInt | Primitive::LiteralString); }
    bool hasOnlyPrimitives(uint16_t allowed) const { return !(primitives & ~allowed) && !hasObjects(); }
    bool isNumber() const { return !isNever() && hasOnlyPrimitives(Primitive::Number); }
    bool isInteger() const { return !isNever() && hasOnlyPrimitives(Primitive::Int); }
    bool isBoolean() const { return !isNever() && hasOnlyPrimitives(Primitive::Boolean); }
    bool isString() const { return !isNever() && hasOnlyPrimitives(Primitive::String); }
    bool hasLiteralString() const { return primitives & Primitive::LiteralString; }
    bool isObject() const { return hasObjects() && !primitives; }
    bool isUndefinedOrNull() const { return !isNever() && hasOnlyPrimitives(Primitive::UndefinedOrNull); }
    bool mayBeUndefinedOrNull() const { return primitives & Primitive::UndefinedOrNull; }

    LayoutID singleLayout() const;
    const EngineClass* singleEngineClass() const;
    Type withoutPrimitives(uint16_t) const;
    Type withLiteralStringWidened() const;
    void normalizeLiteralString();
    bool acceptsUntypedObjects(const Program&) const;
    bool acceptsUncheckedObjects(const Program&) const;
    bool acceptsOnlyOtherLayouts(const Program&) const;
    bool isSubsetOf(const Type&, const Program&, bool untypedObjectsPass = true) const;

    friend bool operator==(const Type&, const Type&) = default;
};

void add(Hasher&, const Type&);

struct TypeHash {
    static unsigned hash(const Type& type) { return computeHash(type); }
    static bool equal(const Type& a, const Type& b) { return a == b; }
    static constexpr bool safeToCompareToEmptyOrDeleted = false;
};

class TypeTable {
    WTF_MAKE_NONCOPYABLE(TypeTable);
public:
    TypeTable();

    TypeID intern(const Type&);
    const Type& at(TypeID id) const { return m_types[id.index()]; }
    unsigned size() const { return m_types.size(); }

    void freeze() { m_isFrozen = true; }
    void thaw() { m_isFrozen = false; }

    TypeID never;
    TypeID undefined;
    TypeID boolean;
    TypeID int32;
    TypeID integer;
    TypeID number;
    TypeID string;
    TypeID function;
    TypeID object;
    TypeID anything;

private:
    SegmentedVector<Type, 64> m_types;
    UncheckedKeyHashMap<unsigned, Vector<TypeID, 1>, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_byHash;
    bool m_isFrozen { false };
};

enum class Rep : uint8_t {
    Void,
    Int32,
    Int64,
    Double,
    OptionalInt64,
    OptionalDouble,
    Cell,
    JSValue,
    LiteralString,
};

Rep repOf(const Type&);
Store storeOf(const Type&);
ASCIILiteral nameOf(Rep);
ASCIILiteral nameOf(Store);
unsigned sizeOf(Store);
Rep repOf(Store);
inline bool holdsCells(Rep rep) { return rep == Rep::Cell || rep == Rep::JSValue || rep == Rep::LiteralString; }
inline bool holdsCells(Store store) { return store == Store::Cell || store == Store::JSValue || store == Store::Cell32 || store == Store::OptionalCell32 || store == Store::LiteralString || store == Store::LiteralString32; }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
