/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTIDSets.h"
#include "AOTObjects.h"
#include "AOTPrimitives.h"
#include "AOTSchemaEnums.h"
#include "AOTTypeIDs.h"
#include "AOTTypeKinds.h"
#include "AOTWire.h"
#include <array>
#include <optional>
#include <span>
#include <wtf/Assertions.h>
#include <wtf/OptionSet.h>
#include <wtf/ScopedLambda.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT {

class BuiltinTable;

namespace Typed {

class Program;
class TypeArena;
class TypeTable;
struct PropertyKey;

// Where an `unknown` came from. A union keeps the seeds of both sides, so a fact can ask which kinds it must fear and --explain can name the cause.
enum class UnknownSeed : uint16_t {
    ResultOfUnknownCallee = 1 << 0,
    CaughtException = 1 << 1,
    // The row of a built-in says nothing of what it answers.
    UndescribedNativeResult = 1 << 2,
    ParameterOfEscapedFunction = 1 << 3,
    ContentsOfEscapedObject = 1 << 4,
    // What code from disk, eval or node:vm makes, hands over or stores.
    UnseenCode = 1 << 5,
    // JSON.parse, a message of a Worker, a structured clone, whatever the host reads in.
    DataFromOutside = 1 << 6,
    // The checker's type rests on nothing that the build saw: a declaration without a body, an assertion, a suppressed error.
    UncheckedConversion = 1 << 7,
    // An operation makes an object that nobody numbered and that has neither a layout nor a class: the schema's natural(object) without a site.
    ObjectWithoutLayout = 1 << 8,
    // An established position is written as `any`, `object` or a function type without members.
    WrittenAsLoose = 1 << 9,
    // The walk gave up on a loop's parameter, or entered a block by a resume switch.
    LostByWalk = 1 << 10,
};

using UnknownSeeds = OptionSet<UnknownSeed>;

IllTypedCause causeOf(UnknownSeed);

// What a parser has made of data from outside has no layout of the program: a test for one would turn it away every time. So no layout is guessed from a name for such an `unknown`.
inline bool standsOnlyForObjectsWithoutLayout(UnknownSeeds seeds)
{
    return !seeds.isEmpty() && seeds.containsOnly({ UnknownSeed::DataFromOutside, UnknownSeed::ObjectWithoutLayout });
}

// The enumerator as it is spelled above, which is how the text of a type writes a seed.
inline ASCIILiteral nameOf(UnknownSeed seed)
{
    switch (seed) {
    case UnknownSeed::ResultOfUnknownCallee:
        return "ResultOfUnknownCallee"_s;
    case UnknownSeed::CaughtException:
        return "CaughtException"_s;
    case UnknownSeed::UndescribedNativeResult:
        return "UndescribedNativeResult"_s;
    case UnknownSeed::ParameterOfEscapedFunction:
        return "ParameterOfEscapedFunction"_s;
    case UnknownSeed::ContentsOfEscapedObject:
        return "ContentsOfEscapedObject"_s;
    case UnknownSeed::UnseenCode:
        return "UnseenCode"_s;
    case UnknownSeed::DataFromOutside:
        return "DataFromOutside"_s;
    case UnknownSeed::UncheckedConversion:
        return "UncheckedConversion"_s;
    case UnknownSeed::ObjectWithoutLayout:
        return "ObjectWithoutLayout"_s;
    case UnknownSeed::WrittenAsLoose:
        return "WrittenAsLoose"_s;
    case UnknownSeed::LostByWalk:
        return "LostByWalk"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

inline constexpr unsigned mostContentsOfClass = 3;

// Objects of one of the engine's classes, described by the class. What is written of their contents is an expectation: proofs about contents are Facts' cells of named objects.
struct ClassPart {
    BuiltinClassID builtinClass;
    // Null: nothing is written, so anything.
    std::array<TypeID, mostContentsOfClass> writtenContents;

    TypeID writtenContentsAt(unsigned positionInType) const;
    TypeID writtenContentsAt(const BuiltinTable&, ContentsSlot) const;
    ExpectedType expectedContentsAt(unsigned positionInType) const { return ExpectedType(writtenContentsAt(positionInType)); }
    ExpectedType expectedContentsAt(const BuiltinTable& builtins, ContentsSlot slot) const { return ExpectedType(writtenContentsAt(builtins, slot)); }
    bool hasWrittenContents() const { return writtenContents[0] || writtenContents[1] || writtenContents[2]; }

    friend bool operator==(const ClassPart&, const ClassPart&) = default;
};
static_assert(sizeof(ClassPart) == 16);

// What makes a type. Fill one and hand it to TypeArena::intern(), which makes it canonical.
//
// The values of a type are its primitives, the objects it names, and its unnamed objects, which are `unknown`.
// `unknown` stands for objects that have escaped or that unseen code made. It is bounded or not:
//   anyObject                         any object at all
//   anyFunction                       any callable
//   unnamedLayouts, unnamedClasses    `unknown` within these layouts and classes: what a passed test, a guarded position or a written type says
// A named object beside `unknown` is kept. Unless it IsMerged it stands by right, and its cells are proof.
struct TypeParts {
    uint16_t primitives { 0 };
    UnknownSeeds seeds;
    bool anyObject { false };
    bool anyFunction { false };
    std::optional<OpaqueKind> opaque;
    Range integers;
    LayoutID environment;
    StringSetID stringLiterals;
    KeySetID symbolLiterals;
    ObjectSetID objects;
    LayoutSetID unnamedLayouts;
    ClassPartsID unnamedClasses;
    // Every number of the type that has a fraction lies between these two integers: `a / 2` of an int32, and what sums, Math.min() and joins make of it.
    // Empty: nothing is known of where they lie. It says nothing of `integers`, and whoever cuts that range leaves this one as it is.
    // Whoever adds FractionalNumber to the parts of another type empties it. No test asks for it: TypeArena::withoutBoundsOfFractions().
    Range boundsOfFractions;

    friend bool operator==(const TypeParts&, const TypeParts&) = default;
};

// A type as read out of a table: canonical parts, and what follows from them. Only TypeTable and TypeArena make one.
struct Type : TypeParts {
    // The layouts of every object of the type that has one, named or not. They are the shapes of all its objects exactly where hasOnlyLayouts().
    LayoutSetID layouts;
    // The classes of every object of the type that has one. A part that comes from a named object alone has no written contents.
    ClassPartsID classes;
    // The traits of the named objects, united.
    OptionSet<ObjectTrait> traitsOfNamed;

    bool hasUnknown() const { return anyObject || anyFunction || unnamedLayouts || unnamedClasses; }
    bool hasUnboundedUnknown() const { return anyObject || anyFunction; }
    bool hasNamedObjects() const { return !!objects; }
    bool hasOnlyNamedObjects() const { return objects && !hasUnknown(); }
    bool namesMergedObjects() const { return traitsOfNamed.contains(ObjectTrait::IsMerged); }

    bool hasObjects() const { return hasUnknown() || objects; }
    bool isNever() const { return !primitives && !hasObjects() && isJavaScriptValue(); }
    bool isAnything() const { return (primitives & Primitive::All) == Primitive::All && integers == Range::full() && boundsOfFractions.isEmpty() && anyObject; }
    bool isJavaScriptValue() const { return !environment && !opaque; }
    bool isOpaque(OpaqueKind kind) const { return opaque == kind && !primitives; }
    bool hasCells() const;
    bool hasOnlyPrimitives(uint16_t allowed) const { return !(primitives & ~allowed) && !hasObjects() && isJavaScriptValue(); }
    bool isNumber() const { return !isNever() && hasOnlyPrimitives(Primitive::Number); }
    bool isInteger() const { return !isNever() && hasOnlyPrimitives(Primitive::Integer); }
    bool isNumberWithoutFraction() const { return !isNever() && hasOnlyPrimitives(Primitive::NumberWithoutFraction); }
    bool holdsFractionsWithoutBounds() const { return primitives & Primitive::FractionalNumber && boundsOfFractions.isEmpty(); }
    // An integer, or a number with a fraction that lies between two known integers. So it is neither NaN nor an infinity nor -0.
    bool isNumberWithinBounds() const { return !isNever() && hasOnlyPrimitives(Primitive::Integer | Primitive::FractionalNumber) && !holdsFractionsWithoutBounds(); }
    // Of a type that isNumberWithinBounds(): every value lies in it, the ends included.
    Range boundsOfNumbers() const { return integers.unionWith(boundsOfFractions); }
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

    // A class may be one of callables as well: mayContainAnyCallable() in AOTTypeRelations.h.
    bool mayBeFunction() const { return anyFunction || traitsOfNamed.contains(ObjectTrait::IsCallable); }
    // Every callable of the type is a named closure or built-in.
    bool hasOnlyKnownCallees() const { return !anyFunction && !classes; }
    // Each speaks of every object of the type and admits undefined and null beside them. None holds of a type without objects.
    bool hasOnlyLayouts() const { return hasOnlyListedObjects() && !unnamedClasses && (traitsOfNamed & traitsOfShape).containsOnly({ ObjectTrait::HasLayout }); }
    bool hasOnlyClassObjects() const { return hasOnlyListedObjects() && !unnamedLayouts && (traitsOfNamed & traitsOfShape).containsOnly({ ObjectTrait::HasClass }); }
    bool hasOnlyClosures() const { return hasOnlyListedObjects() && !hasUnknown() && (traitsOfNamed & traitsOfIdentity).containsOnly({ ObjectTrait::IsClosure }); }
    bool hasOnlyBuiltins() const { return hasOnlyListedObjects() && !hasUnknown() && (traitsOfNamed & traitsOfIdentity).containsOnly({ ObjectTrait::IsBuiltin }); }
    // Where a member is asked for, undefined and null throw: what goes on is an object of one of the engine's classes.
    bool isClassObjectOrNullish() const { return hasOnlyClassObjects(); }

private:
    bool hasOnlyListedObjects() const { return hasObjects() && !hasUnboundedUnknown() && !(primitives & ~Primitive::UndefinedOrNull); }
};

static_assert(sizeof(Type) <= 88);

uint16_t fromWire(Wire::Primitives);
inline uint16_t fromWire(OptionSet<Wire::Primitives> primitives) { return fromWire(static_cast<Wire::Primitives>(primitives.toRaw())); }
Wire::Primitives toWire(uint16_t primitives);

// Whoever only reads takes one of these by value. It is made of a Program, a TypeTable or a TypeArena without a word.
// Made of an arena it also reads what that arena's thread made during Compile. Made of the others it stops at such an id.
class TypeReader {
public:
    TypeReader(const Program&);
    TypeReader(const TypeTable& table)
        : m_table(&table)
    {
    }
    TypeReader(const TypeArena&);

    Type at(TypeID) const;
    // TypeArena::recordOf().
    const Type& recordOf(TypeID) const;
    SetView<LayoutID, LayoutSetID> at(LayoutSetID) const;
    std::span<const LayoutID> listOf(LayoutSetID) const;
    SetView<ObjectID, ObjectSetID> at(ObjectSetID) const;
    SetView<StringID, StringSetID> at(StringSetID) const;
    SetView<PropertyKeyID, KeySetID> at(KeySetID) const;
    std::span<const ClassPart> at(ClassPartsID) const;
    const PropertyKey& at(PropertyKeyID) const;
    const ObjectRecord& at(ObjectID) const;
    // TypeArena::bytesOf().
    std::span<const uint8_t> bytesOf(StringID) const;

    const TypeTable& table() const { return *m_table; }
    const ObjectTable& objects() const;
    const BuiltinTable& builtins() const;
    // Null in a test that has types and no program.
    const Program* program() const;

private:
    const TypeTable* m_table;
    const TypeArena* m_arena { nullptr };
};

// Each list is sorted by number and lives as long as the type does.
std::span<const LayoutID> membersOf(TypeReader, const Type&);
std::span<const LayoutID> membersOf(TypeReader, LayoutSetID);
std::span<const ClassPart> classPartsOf(TypeReader, const Type&);

// The layout of every object of the type, or its environment. Null unless there is exactly one. Undefined and null may stand beside it.
LayoutID singleLayoutOf(TypeReader, const Type&);
// The pointers live as long as the type does.
const ClassPart* singleBuiltinClassOf(TypeReader, const Type&);
const ClassPart* findClassPart(TypeReader, const Type&, BuiltinClassID);
// The one function whose closures are all the objects of the type. A copy of the closure for an instance counts as that function.
FunctionID singleFunctionOf(TypeReader, const Type&);
BuiltinID singleBuiltinOf(TypeReader, const Type&);
// The one named object that is all the objects of the type.
ObjectID singleObjectOf(const Type&);
StringID singleStringLiteralOf(const Type&);
// One value in every run: undefined, null, a boolean, one integer, one literal, an object that is found by identity.
bool isSingleValue(TypeReader, const Type&);

// The closures and the built-ins among the named objects, in the order of their numbers. A function is named once, however many instances copied its closure.
void forEachFunctionOf(TypeReader, const Type&, NOESCAPE const ScopedLambda<void(FunctionID)>&);
void forEachBuiltinOf(TypeReader, const Type&, NOESCAPE const ScopedLambda<void(BuiltinID)>&);
bool namesFunction(TypeReader, const Type&, FunctionID);
bool namesBuiltin(TypeReader, const Type&, BuiltinID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
