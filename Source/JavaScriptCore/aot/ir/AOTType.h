/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTCompactList.h"
#include "AOTDiagnostics.h"
#include "AOTID.h"
#include "AOTLimits.h"
#include "AOTPrimitives.h"
#include "AOTSchemaEnums.h"
#include "AOTSortedSet.h"
#include "AOTTypeKinds.h"
#include "AOTWire.h"
#include <compare>
#include <optional>
#include <span>
#include <wtf/Hasher.h>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;
class BuiltinTable;

namespace Typed {

class Program;
class TypeArena;
class TypeTable;

struct ClassPart {
    BuiltinClassID builtinClass;
    bool contentsAreProven { false };
    ArrayStorage arrayStorage { ArrayStorage::Other };
    OptionSet<PristineAspects> provenPristine;
    // As they stand in the type: for the algebra of types, walkers, printers and tables. Whoever wants to know what a collection holds asks one of the two questions below.
    InlineList<TypeID, 3> writtenContents;

    TypeID writtenContentsAt(unsigned indexInType) const;
    TypeID writtenContentsAt(const BuiltinTable&, ContentsSlot) const;
    // All that is ever stored, where flow has followed it. Nothing where the contents are declared or guessed.
    std::optional<ProvenValue<TypeID>> provenContentsAt(unsigned indexInType) const;
    std::optional<ProvenValue<TypeID>> provenContentsAt(const BuiltinTable&, ContentsSlot) const;
    ExpectedType expectedContentsAt(unsigned indexInType) const { return ExpectedType(writtenContentsAt(indexInType)); }
    ExpectedType expectedContentsAt(const BuiltinTable&, ContentsSlot) const;

    friend bool operator==(const ClassPart&, const ClassPart&) = default;
};

template<> inline constexpr bool canMoveWithMemcpy<ClassPart> = true;

using ClassParts = InlineList<ClassPart, 1>;

struct RegExpLiteralInType {
    StringID pattern;
    uint32_t rawFlags { 0 };

    OptionSet<RegExpFlags> flags() const { return OptionSet<RegExpFlags>::fromRaw(static_cast<OptionSet<RegExpFlags>::StorageType>(rawFlags)); }

    friend constexpr bool operator==(const RegExpLiteralInType&, const RegExpLiteralInType&) = default;
    friend constexpr auto operator<=>(const RegExpLiteralInType&, const RegExpLiteralInType&) = default;
};

struct Type;

struct WideningContext {
    const Program& program;
    TypeArena& types;
    BuildContext& build;
    Subject subject;
    bool* didWiden { nullptr };
    // The type that the value had until now. A union inside it keeps each list that is no longer than that type's: the program holds one as long already.
    const Type* typeBefore { nullptr };
    bool* didKeepListsOfTypeBefore { nullptr };
};

struct Type {
    uint16_t primitives { 0 };
    bool anyObject { false };
    bool anyFunction { false };
    std::optional<OpaqueKind> opaque;
    Range integers;
    LayoutID environment;
    LayoutSetID layouts;
    SortedSet<StringID> stringLiterals;
    SortedSet<PropertyKeyID> symbolLiterals;
    ClassParts classes;
    SortedSet<FunctionID> functions;
    SortedSet<BuiltinID> builtins;
    SortedSet<RegExpLiteralInType> regExpLiteralSet;

    Type() = default;
    NEVER_INLINE ~Type() = default;
    Type(const Type&) = default;
    Type(Type&&) = default;
    Type& operator=(const Type&) = default;
    Type& operator=(Type&&) = default;

    static Type never() { return { }; }
    static Type fromPrimitives(uint16_t);
    static Type integer(Range);
    static Type number(double);
    static Type function();
    static Type object();
    static Type anything();
    static Type forLayoutSet(LayoutSetID);
    static Type forLayout(LayoutID, TypeArena&);
    static Type forLayouts(std::span<const LayoutID> unsortedLayouts, TypeArena&);
    static Type forEnvironment(LayoutID);
    static Type builtinClass(BuiltinClassID);
    static Type builtinClass(BuiltinClassID, std::span<const TypeID> contents);
    static Type forFunction(FunctionID);
    static Type forBuiltin(BuiltinID);
    static Type stringLiteral(StringID);
    static Type symbolLiteral(PropertyKeyID);
    static Type regExpLiteral(StringID pattern, OptionSet<RegExpFlags>);
    static Type forOpaque(OpaqueKind);

    bool hasObjects() const { return anyObject || anyFunction || layouts || !classes.isEmpty() || !functions.isEmpty() || !builtins.isEmpty(); }
    bool isNever() const { return !primitives && !hasObjects() && isJavaScriptValue(); }
    bool isAnything() const { return (primitives & Primitive::All) == Primitive::All && integers == Range::full() && anyObject && anyFunction; }
    bool isJavaScriptValue() const { return !environment && !opaque; }
    bool isOpaque(OpaqueKind kind) const { return opaque == kind && !primitives; }
    bool hasCells() const;
    bool hasOnlyPrimitives(uint16_t allowed) const { return !(primitives & ~allowed) && !hasObjects() && isJavaScriptValue(); }
    bool isNumber() const { return !isNever() && hasOnlyPrimitives(Primitive::Number); }
    bool isInteger() const { return !isNever() && hasOnlyPrimitives(Primitive::Integer); }
    bool isNumberWithoutFraction() const { return !isNever() && hasOnlyPrimitives(Primitive::NumberWithoutFraction); }
    bool isBoolean() const { return !isNever() && hasOnlyPrimitives(Primitive::Boolean); }
    bool isString() const { return !isNever() && hasOnlyPrimitives(Primitive::String); }
    bool isAnyString() const { return !isNever() && hasOnlyPrimitives(Primitive::AnyString); }
    bool isUndefinedOrNull() const { return !isNever() && hasOnlyPrimitives(Primitive::UndefinedOrNull); }
    bool isNumberOrNullish() const { return primitives & Primitive::Number && hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull); }
    bool isStringOrNullish() const { return primitives & Primitive::String && hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull); }
    bool isAnyStringOrNullish() const { return primitives & Primitive::AnyString && hasOnlyPrimitives(Primitive::AnyString | Primitive::UndefinedOrNull); }
    bool isObject() const { return hasObjects() && !primitives; }
    bool mayBeUndefinedOrNull() const { return primitives & Primitive::UndefinedOrNull; }
    bool mayBeUnspecified() const { return primitives & Primitive::Unspecified; }
    bool mayBeNaN() const { return primitives & Primitive::WholeNumber; }
    bool hasLiteralStrings() const { return primitives & Primitive::LiteralString; }
    bool isInt32() const { return isInteger() && integers.isSubsetOf(Range::int32()); }
    bool isInt32OrNullish() const { return hasOnlyPrimitives(Primitive::Integer | Primitive::UndefinedOrNull | Primitive::Absent) && integers.isSubsetOf(Range::int32()); }
    bool isNumberLike() const { return hasOnlyPrimitives(Primitive::NumberLike); }
    bool mayBeFunction() const { return anyFunction || anyObject || !functions.isEmpty() || !builtins.isEmpty(); }
    bool hasOnlyKnownCallees() const { return !anyFunction && !anyObject && classes.isEmpty(); }
    bool hasOnlyClassObjects() const { return !anyObject && !anyFunction && !layouts && functions.isEmpty() && builtins.isEmpty(); }
    bool hasOnlyLayouts() const;
    bool isSingleValue() const;

    LayoutID singleLayout(const Program&) const;
    const ClassPart* singleBuiltinClass() const LIFETIME_BOUND;
    const ClassPart* findClassPart(BuiltinClassID) const LIFETIME_BOUND;
    FunctionID singleFunction() const;
    BuiltinID singleBuiltin() const;
    StringID singleStringLiteral() const;
    std::span<const RegExpLiteralInType> regExpLiterals() const LIFETIME_BOUND { return regExpLiteralSet.span(); }

    Type withoutPrimitives(uint16_t) const;
    Type withPrimitives(uint16_t) const;
    Type withoutNullish() const;
    Type withoutObjects() const;
    Type withOnlyObjects() const;
    Type withLiteralStringsWidened() const;
    Type withContentsProven(bool) const;
    Type withUntestableBuiltinsWidened() const;
    Type withoutClassProofs() const;

    void canonicalize();

    bool isSubsetOf(const Type&, const Program&) const;
    bool overlaps(const Type&, const Program&) const;
    Type unionWith(const Type&, TypeArena&) const;
    Type unionWideningWith(const Type&, const WideningContext&) const;
    Type intersectWith(const Type&, TypeArena&) const;
    Type castTo(const Type& castType, TypeArena&) const;
    Type without(const Type&, TypeArena&) const;

    friend bool operator==(const Type&, const Type&) = default;
};

void add(Hasher&, const Type&);
void add(Hasher&, RegExpLiteralInType);

uint16_t fromWire(Wire::Primitives);
inline uint16_t fromWire(OptionSet<Wire::Primitives> primitives) { return fromWire(static_cast<Wire::Primitives>(primitives.toRaw())); }
Wire::Primitives toWire(uint16_t primitives);

std::span<const LayoutID> membersOf(const Program&, const Type&);
std::span<const LayoutID> membersOf(const TypeTable&, const Type&);


} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
