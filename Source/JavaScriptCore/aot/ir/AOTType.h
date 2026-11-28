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

enum class UnknownSeed : uint16_t {
    ResultOfUnknownCallee = 1 << 0,
    CaughtException = 1 << 1,
    UndescribedNativeResult = 1 << 2,
    ParameterOfEscapedFunction = 1 << 3,
    ContentsOfEscapedObject = 1 << 4,
    UnseenCode = 1 << 5,
    DataFromOutside = 1 << 6,
    UncheckedConversion = 1 << 7,
    ObjectWithoutLayout = 1 << 8,
    WrittenAsLoose = 1 << 9,
    LostByWalk = 1 << 10,
};

using UnknownSeeds = OptionSet<UnknownSeed>;

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

struct ClassPart {
    BuiltinClassID builtinClass;
    std::array<TypeID, mostContentsOfClass> writtenContents;

    TypeID writtenContentsAt(unsigned positionInType) const;
    TypeID writtenContentsAt(const BuiltinTable&, ContentsSlot) const;
    ExpectedType expectedContentsAt(unsigned positionInType) const { return ExpectedType(writtenContentsAt(positionInType)); }
    ExpectedType expectedContentsAt(const BuiltinTable& builtins, ContentsSlot slot) const { return ExpectedType(writtenContentsAt(builtins, slot)); }
    bool hasWrittenContents() const { return writtenContents[0] || writtenContents[1] || writtenContents[2]; }

    friend bool operator==(const ClassPart&, const ClassPart&) = default;
};
static_assert(sizeof(ClassPart) == 16);

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
    Range boundsOfFractions;

    friend bool operator==(const TypeParts&, const TypeParts&) = default;
};

struct Type : TypeParts {
    LayoutSetID layouts;
    ClassPartsID classes;
    OptionSet<ObjectTrait> traitsOfNamed;
    uint8_t answersOfParts { 0 };

    void noteAnswersOfParts() { answersOfParts = workOutAnswersOfParts(); }

    bool hasUnknown() const { return answers() & HoldsUnknown; }
    bool hasUnboundedUnknown() const { return answers() & HoldsUnboundedUnknown; }
    bool hasNamedObjects() const { return !!objects; }
    bool hasOnlyNamedObjects() const { return (answers() & (HoldsObjects | HoldsUnknown)) == HoldsObjects; }
    bool namesMergedObjects() const { return traitsOfNamed.contains(ObjectTrait::IsMerged); }

    bool hasObjects() const { return answers() & HoldsObjects; }
    bool isNever() const { return !primitives && !(answers() & (HoldsObjects | IsNoJavaScriptValue)); }
    bool isAnything() const { return (primitives & Primitive::All) == Primitive::All && integers == Range::full() && boundsOfFractions.isEmpty() && anyObject; }
    bool isJavaScriptValue() const { return !(answers() & IsNoJavaScriptValue); }
    bool isOpaque(OpaqueKind kind) const { return opaque == kind && !primitives; }
    bool hasCells() const;
    bool hasOnlyPrimitives(uint16_t allowed) const { return !(primitives & ~allowed) && !(answers() & (HoldsObjects | IsNoJavaScriptValue)); }
    bool isNumber() const { return !isNever() && hasOnlyPrimitives(Primitive::Number); }
    bool isInteger() const { return !isNever() && hasOnlyPrimitives(Primitive::Integer); }
    bool isNumberWithoutFraction() const { return !isNever() && hasOnlyPrimitives(Primitive::NumberWithoutFraction); }
    bool holdsFractionsWithoutBounds() const { return primitives & Primitive::FractionalNumber && boundsOfFractions.isEmpty(); }
    bool isNumberWithinBounds() const { return !isNever() && hasOnlyPrimitives(Primitive::Integer | Primitive::FractionalNumber) && !holdsFractionsWithoutBounds(); }
    Range boundsOfNumbers() const { return integers.unionWith(boundsOfFractions); }
    bool isBoolean() const { return !isNever() && hasOnlyPrimitives(Primitive::Boolean); }
    bool isString() const { return !isNever() && hasOnlyPrimitives(Primitive::String); }
    bool isAnyString() const { return !isNever() && hasOnlyPrimitives(Primitive::AnyString); }
    bool isUndefinedOrNull() const { return !isNever() && hasOnlyPrimitives(Primitive::UndefinedOrNull); }
    bool isNumberOrNullish() const { return primitives & Primitive::Number && hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull); }
    bool isAnyStringOrNullish() const { return primitives & Primitive::AnyString && hasOnlyPrimitives(Primitive::AnyString | Primitive::UndefinedOrNull); }
    bool isObject() const { return hasObjects() && !primitives; }
    bool mayBeUndefinedOrNull() const { return primitives & Primitive::UndefinedOrNull; }
    bool mayBeUnspecified() const { return primitives & Primitive::Unspecified; }
    bool mayBeNaN() const { return primitives & Primitive::WholeNumber; }
    bool hasLiteralStrings() const { return primitives & Primitive::LiteralString; }
    bool isInt32() const { return isInteger() && integers.isSubsetOf(Range::int32()); }
    bool isInt32OrNullish() const { return hasOnlyPrimitives(Primitive::Integer | Primitive::UndefinedOrNull | Primitive::Absent) && integers.isSubsetOf(Range::int32()); }
    bool isNumberLike() const { return hasOnlyPrimitives(Primitive::NumberLike); }

    bool mayBeFunction() const { return anyFunction || traitsOfNamed.contains(ObjectTrait::IsCallable); }
    bool hasOnlyKnownCallees() const { return !anyFunction && !classes; }
    bool hasOnlyLayouts() const { return answers() & ObjectsHaveLayouts && !(primitives & ~Primitive::UndefinedOrNull); }
    bool hasOnlyClassObjects() const { return answers() & ObjectsHaveClasses && !(primitives & ~Primitive::UndefinedOrNull); }
    bool hasOnlyClosures() const { return answers() & ObjectsAreClosures && !(primitives & ~Primitive::UndefinedOrNull); }
    bool hasOnlyBuiltins() const { return answers() & ObjectsAreBuiltins && !(primitives & ~Primitive::UndefinedOrNull); }

private:
    enum : uint8_t {
        HoldsUnknown = 1 << 0,
        HoldsUnboundedUnknown = 1 << 1,
        HoldsObjects = 1 << 2,
        IsNoJavaScriptValue = 1 << 3,
        ObjectsHaveLayouts = 1 << 4,
        ObjectsHaveClasses = 1 << 5,
        ObjectsAreClosures = 1 << 6,
        ObjectsAreBuiltins = 1 << 7,
    };

    uint8_t workOutAnswersOfParts() const
    {
        bool holdsUnboundedUnknown = anyObject || anyFunction;
        bool holdsUnknown = holdsUnboundedUnknown || unnamedLayouts || unnamedClasses;
        bool holdsObjects = holdsUnknown || objects;
        bool holdsOnlyListedObjects = holdsObjects && !holdsUnboundedUnknown;
        OptionSet<ObjectTrait> shapesOfNamed = traitsOfNamed & traitsOfShape;
        OptionSet<ObjectTrait> identitiesOfNamed = traitsOfNamed & traitsOfIdentity;
        unsigned bits = 0;
        if (holdsUnknown)
            bits |= HoldsUnknown;
        if (holdsUnboundedUnknown)
            bits |= HoldsUnboundedUnknown;
        if (holdsObjects)
            bits |= HoldsObjects;
        if (environment || opaque)
            bits |= IsNoJavaScriptValue;
        if (holdsOnlyListedObjects && !unnamedClasses && shapesOfNamed.containsOnly({ ObjectTrait::HasLayout }))
            bits |= ObjectsHaveLayouts;
        if (holdsOnlyListedObjects && !unnamedLayouts && shapesOfNamed.containsOnly({ ObjectTrait::HasClass }))
            bits |= ObjectsHaveClasses;
        if (holdsOnlyListedObjects && !holdsUnknown && identitiesOfNamed.containsOnly({ ObjectTrait::IsClosure }))
            bits |= ObjectsAreClosures;
        if (holdsOnlyListedObjects && !holdsUnknown && identitiesOfNamed.containsOnly({ ObjectTrait::IsBuiltin }))
            bits |= ObjectsAreBuiltins;
        return static_cast<uint8_t>(bits);
    }

    uint8_t answers() const
    {
        ASSERT(answersOfParts == workOutAnswersOfParts());
        return answersOfParts;
    }
};

static_assert(sizeof(Type) <= 88);

uint16_t fromWire(Wire::Primitives);
inline uint16_t fromWire(OptionSet<Wire::Primitives> primitives) { return fromWire(static_cast<Wire::Primitives>(primitives.toRaw())); }
Wire::Primitives toWire(uint16_t primitives);

class TypeReader {
public:
    TypeReader(const Program&);
    TypeReader(const TypeTable& table)
        : m_table(&table)
    {
    }
    TypeReader(const TypeArena&);

    Type at(TypeID) const;
    const Type& recordOf(TypeID) const;
    SetView<LayoutID, LayoutSetID> at(LayoutSetID) const;
    std::span<const LayoutID> listOf(LayoutSetID) const;
    SetView<ObjectID, ObjectSetID> at(ObjectSetID) const;
    SetView<StringID, StringSetID> at(StringSetID) const;
    SetView<PropertyKeyID, KeySetID> at(KeySetID) const;
    std::span<const ClassPart> at(ClassPartsID) const;
    const PropertyKey& at(PropertyKeyID) const;
    const ObjectRecord& at(ObjectID) const;
    std::span<const uint8_t> bytesOf(StringID) const;

    const TypeTable& table() const { return *m_table; }
    const ObjectTable& objects() const;
    const BuiltinTable& builtins() const;
    const Program* program() const;

private:
    const TypeTable* m_table;
    const TypeArena* m_arena { nullptr };
};

std::span<const LayoutID> membersOf(TypeReader, const Type&);
std::span<const LayoutID> membersOf(TypeReader, LayoutSetID);
std::span<const ClassPart> classPartsOf(TypeReader, const Type&);

inline LayoutID singleLayoutOf(TypeReader, const Type& type)
{
    if (type.hasOnlyLayouts())
        return holdsOneMember(type.layouts) ? LayoutID(positionOf(type.layouts)) : LayoutID();
    if (type.primitives & ~Primitive::UndefinedOrNull)
        return { };
    return type.environment;
}
const ClassPart* singleBuiltinClassOf(TypeReader, const Type&);
const ClassPart* findClassPart(TypeReader, const Type&, BuiltinClassID);
FunctionID functionSharedByClosuresOf(TypeReader, const Type&);
BuiltinID builtinSharedByObjectsOf(TypeReader, const Type&);
inline FunctionID singleFunctionOf(TypeReader reader, const Type& type) { return type.hasOnlyClosures() ? functionSharedByClosuresOf(reader, type) : FunctionID(); }
inline BuiltinID singleBuiltinOf(TypeReader reader, const Type& type) { return type.hasOnlyBuiltins() ? builtinSharedByObjectsOf(reader, type) : BuiltinID(); }
ObjectID singleObjectOf(const Type&);
StringID singleStringLiteralOf(const Type&);
bool isSingleValue(TypeReader, const Type&);

void forEachFunctionAmongClosuresOf(TypeReader, const Type&, NOESCAPE const ScopedLambda<void(FunctionID)>&);
inline void forEachFunctionOf(TypeReader reader, const Type& type, NOESCAPE const ScopedLambda<void(FunctionID)>& functor)
{
    if (type.traitsOfNamed.contains(ObjectTrait::IsClosure))
        forEachFunctionAmongClosuresOf(reader, type, functor);
}
void forEachBuiltinOf(TypeReader, const Type&, NOESCAPE const ScopedLambda<void(BuiltinID)>&);
bool namesFunction(TypeReader, const Type&, FunctionID);
bool namesBuiltin(TypeReader, const Type&, BuiltinID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
