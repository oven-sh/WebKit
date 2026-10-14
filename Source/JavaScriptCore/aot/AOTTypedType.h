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
// An integral number other than -0, of magnitude at most 2^62.
static constexpr uint16_t Int = 1 << 4;
// Any other number.
static constexpr uint16_t Double = 1 << 5;
static constexpr uint16_t String = 1 << 6;
static constexpr uint16_t Symbol = 1 << 7;
static constexpr uint16_t BigInt = 1 << 8;
// A field or an element that is not there. Never the value of an expression.
static constexpr uint16_t Absent = 1 << 9;

static constexpr uint16_t Boolean = False | True;
static constexpr uint16_t Number = Int | Double;
static constexpr uint16_t UndefinedOrNull = Undefined | Null;
static constexpr uint16_t All = (1 << 9) - 1;
}

static constexpr int64_t maxExactInteger = 1ll << 53;
static constexpr int64_t maxKeptInteger = 1ll << 62;
// Added to any kept integer, an integer this small leaves the sum within maxKeptInteger once it is rounded to a double.

struct Range {
    int64_t least { 1 };
    int64_t most { 0 };

    static constexpr Range full() { return { -maxKeptInteger, maxKeptInteger }; }
    static constexpr Range int32() { return { INT32_MIN, INT32_MAX }; }
    static constexpr Range uint32() { return { 0, UINT32_MAX }; }
    // Of a string, and of an array the engine keeps in one piece.
    static constexpr Range length() { return { 0, INT32_MAX }; }
    static constexpr Range empty() { return { 1, 0 }; }
    static constexpr Range one(int64_t value) { return { value, value }; }

    constexpr bool isEmpty() const { return least > most; }
    constexpr bool isWithin(Range other) const { return isEmpty() || (least >= other.least && most <= other.most); }
    constexpr bool contains(int64_t value) const { return least <= value && value <= most; }
    constexpr Range hull(Range other) const
    {
        if (isEmpty())
            return other;
        if (other.isEmpty())
            return *this;
        return { std::min(least, other.least), std::max(most, other.most) };
    }
    constexpr Range overlap(Range other) const { return { std::max(least, other.least), std::min(most, other.most) }; }
    friend constexpr bool operator==(Range, Range) = default;
};

class Program;

// What a value is. It comes from the type checker, through the front end, and is a fact: nothing here finds out a type, and compiled
// code tests nothing that a type says. Whatever makes a value sees to it that the value is of its type.
struct Type {
    uint16_t primitives { 0 };
    // With Primitive::Int: which.
    Range integers;
    bool anyObject { false };
    bool anyFunction { false };
    // An object of a type of the program: its own layout is among the members of one of these. Sorted.
    Vector<LayoutID, 1> views;
    // An instance of a class of the engine, by EngineClass::index. Sorted.
    Vector<uint16_t, 1> engineClasses;
    // Of an array: what its elements are expected to be. Nothing keeps anyone from storing something else, so what is read is tested.
    TypeID elements;

    static Type never() { return { }; }
    static Type ofPrimitives(uint16_t);
    static Type integer(Range);
    static Type view(LayoutID);
    static Type engineClass(const EngineClass&);
    static Type function();
    static Type object();
    static Type anything();

    bool isNever() const { return !primitives && !hasObjects(); }
    bool isAnything() const { return (primitives & Primitive::All) == Primitive::All && anyObject && anyFunction; }
    bool hasObjects() const { return anyObject || anyFunction || !views.isEmpty() || !engineClasses.isEmpty(); }
    bool hasCells() const { return hasObjects() || primitives & (Primitive::String | Primitive::Symbol | Primitive::BigInt); }
    bool hasOnlyPrimitives(uint16_t allowed) const { return !(primitives & ~allowed) && !hasObjects(); }
    bool isNumber() const { return !isNever() && hasOnlyPrimitives(Primitive::Number); }
    bool isInteger() const { return !isNever() && hasOnlyPrimitives(Primitive::Int); }
    bool isBoolean() const { return !isNever() && hasOnlyPrimitives(Primitive::Boolean); }
    bool isString() const { return !isNever() && hasOnlyPrimitives(Primitive::String); }
    bool isObject() const { return hasObjects() && !primitives; }
    bool isUndefinedOrNull() const { return !isNever() && hasOnlyPrimitives(Primitive::UndefinedOrNull); }
    bool mayBeUndefinedOrNull() const { return primitives & Primitive::UndefinedOrNull; }

    // The one type of the program the value is looked at as, if it is an object of that, `undefined` or `null`, and nothing else.
    LayoutID onlyView() const;
    // Likewise the one class of the engine.
    const EngineClass* onlyEngineClass() const;
    Type withoutPrimitives(uint16_t) const;
    // Whether one of the views does: then any object is of the type. See Layout::admitsForeignObjects.
    bool admitsForeignObjects(const Program&) const;
    // Whether every value of this type is one of the other. Without `foreignObjectsPass`: and every object of this type is one that the
    // other type has for what it is, not because a view admits whatever comes. For whoever makes something else of the rest.
    bool isWithin(const Type&, const Program&, bool foreignObjectsPass = true) const;

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

    // From here on there are no new types: functions are compiled side by side, and what a type is numbered must not depend on that.
    void freeze() { m_isFrozen = true; }
    // For what runs alone again: placement.
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
    // References handed out by at() stay valid while types are interned.
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
};

// How a value of the type sits in a register.
Rep repOf(const Type&);
// How it is kept in memory.
Store storeOf(const Type&);
ASCIILiteral nameOf(Rep);
ASCIILiteral nameOf(Store);
unsigned sizeOf(Store);
Rep repOf(Store);
inline bool holdsCells(Rep rep) { return rep == Rep::Cell || rep == Rep::JSValue; }
inline bool holdsCells(Store store) { return store == Store::Cell || store == Store::JSValue || store == Store::Cell32 || store == Store::OptionalCell32; }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
