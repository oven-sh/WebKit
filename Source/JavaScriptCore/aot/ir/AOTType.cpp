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
#include "AOTSort.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include <algorithm>
#include <iterator>
#include <utility>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

TypeID ClassPart::contentsAt(unsigned indexInType) const
{
    if (indexInType >= contents.size() || !contents[indexInType])
        return TypeTable::anything;
    return contents[indexInType];
}

static std::optional<uint8_t> indexInTypeOf(const BuiltinTable& builtins, BuiltinClassID builtinClass, ContentsSlot slot)
{
    BuiltinSlotID id = builtins.findSlot(builtinClass, slot);
    if (!id)
        return std::nullopt;
    return builtins.at(id).indexInType;
}

TypeID ClassPart::contentsAt(const BuiltinTable& builtins, ContentsSlot slot) const
{
    std::optional<uint8_t> indexInType = indexInTypeOf(builtins, builtinClass, slot);
    return indexInType ? contentsAt(*indexInType) : TypeTable::anything;
}

Type Type::fromPrimitives(uint16_t primitives)
{
    Type type;
    type.primitives = primitives;
    if (primitives & Primitive::Integer)
        type.integers = Range::full();
    return type;
}

Type Type::integer(Range range)
{
    Type type;
    if (range.isEmpty())
        return type;
    type.primitives = Primitive::Integer;
    type.integers = range;
    return type;
}

Type Type::number(double value)
{
    PrimitiveSet primitive = primitiveOfNumber(value);
    if (primitive == PrimitiveSet::Integer)
        return integer(Range::one(static_cast<int64_t>(value)));
    return fromPrimitives(static_cast<uint16_t>(primitive));
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
    Type type = fromPrimitives(Primitive::All);
    type.anyObject = true;
    type.anyFunction = true;
    return type;
}

Type Type::forLayoutSet(LayoutSetID set)
{
    Type type;
    type.layouts = set;
    return type;
}

Type Type::forLayout(LayoutID layout, TypeArena& arena)
{
    return forLayoutSet(arena.completeLayoutSet(singleElementSpan(layout)));
}

Type Type::forLayouts(std::span<const LayoutID> unsortedLayouts, TypeArena& arena)
{
    if (unsortedLayouts.empty())
        return never();
    return forLayoutSet(arena.completeLayoutSet(unsortedLayouts));
}

Type Type::forEnvironment(LayoutID layout)
{
    Type type;
    type.environment = layout;
    return type;
}

Type Type::builtinClass(BuiltinClassID classID)
{
    return builtinClass(classID, { });
}

Type Type::builtinClass(BuiltinClassID classID, std::span<const TypeID> contents)
{
    ClassPart part;
    part.builtinClass = classID;
    part.contents.append(contents);
    Type type;
    type.classes.append(WTF::move(part));
    return type;
}

Type Type::forFunction(FunctionID function)
{
    Type type;
    type.functions.add(function);
    return type;
}

Type Type::forBuiltin(BuiltinID builtin)
{
    Type type;
    type.builtins.add(builtin);
    return type;
}

Type Type::stringLiteral(StringID string)
{
    Type type;
    type.primitives = Primitive::LiteralString;
    type.stringLiterals.add(string);
    return type;
}

Type Type::symbolLiteral(PropertyKeyID symbol)
{
    Type type;
    type.primitives = Primitive::LiteralSymbol;
    type.symbolLiterals.add(symbol);
    return type;
}

Type Type::regExpLiteral(StringID pattern, OptionSet<RegExpFlags> flags)
{
    Type type = builtinClass(BuiltinClasses::RegExp);
    type.regExpLiteralSet.add({ pattern, flags.toRaw() });
    return type;
}

Type Type::forOpaque(OpaqueKind kind)
{
    Type type;
    type.opaque = kind;
    return type;
}

bool Type::hasCells() const
{
    if (hasObjects() || environment || primitives & cellPrimitives)
        return true;
    return opaque && holdsCells(representationOf(*opaque));
}

enum class ObjectPart : uint8_t { Layouts, Classes, Functions, Builtins };

static bool hasOnlyObjectPart(const Type& type, ObjectPart part)
{
    if (type.primitives & ~Primitive::UndefinedOrNull || type.anyObject || type.anyFunction)
        return false;
    return (part == ObjectPart::Layouts || !type.layouts)
        && (part == ObjectPart::Classes || type.classes.isEmpty())
        && (part == ObjectPart::Functions || type.functions.isEmpty())
        && (part == ObjectPart::Builtins || type.builtins.isEmpty());
}

bool Type::hasOnlyLayouts(const TypeTable& types) const
{
    return hasOnlyObjectPart(*this, ObjectPart::Layouts) && layouts && !types.at(layouts).hasMembersWithoutLayout;
}

bool Type::hasOnlyLayouts(const Program& program) const
{
    return hasOnlyLayouts(program.types);
}

bool hasMembersWithoutLayout(const TypeTable& types, const Type& type)
{
    return type.layouts && types.at(type.layouts).hasMembersWithoutLayout;
}

bool hasMembersWithoutLayout(const Program& program, const Type& type)
{
    return hasMembersWithoutLayout(program.types, type);
}

bool Type::isSingleValue() const
{
    if (!isJavaScriptValue())
        return false;
    if (hasObjects()) {
        BuiltinID builtin = singleBuiltin();
        return !primitives && builtin && isTestableBuiltin(BuiltinTable::singleton(), builtin);
    }
    switch (primitives) {
    case Primitive::Undefined:
    case Primitive::Null:
    case Primitive::False:
    case Primitive::True:
    case Primitive::Absent:
        return true;
    case Primitive::Integer:
        return integers.isSingleValue();
    case Primitive::LiteralString:
        return stringLiterals.size() == 1;
    case Primitive::LiteralSymbol:
        return symbolLiterals.size() == 1;
    default:
        return false;
    }
}

LayoutID Type::singleLayout(const Program& program) const
{
    if (hasOnlyLayouts(program)) {
        std::span<const LayoutID> members = membersOf(program, *this);
        if (members.size() == 1)
            return members[0];
    }
    if (primitives & ~Primitive::UndefinedOrNull)
        return { };
    return environment;
}

const ClassPart* Type::singleBuiltinClass() const
{
    if (!hasOnlyObjectPart(*this, ObjectPart::Classes) || classes.size() != 1)
        return nullptr;
    return &classes[0];
}

const ClassPart* Type::findClassPart(BuiltinClassID builtinClass) const
{
    for (const ClassPart& part : classes) {
        if (part.builtinClass == builtinClass)
            return &part;
    }
    return nullptr;
}

FunctionID Type::singleFunction() const
{
    if (!hasOnlyObjectPart(*this, ObjectPart::Functions) || functions.size() != 1)
        return { };
    return functions[0];
}

BuiltinID Type::singleBuiltin() const
{
    if (!hasOnlyObjectPart(*this, ObjectPart::Builtins) || builtins.size() != 1)
        return { };
    return builtins[0];
}

StringID Type::singleStringLiteral() const
{
    if (!hasOnlyPrimitives(Primitive::LiteralString) || stringLiterals.size() != 1)
        return { };
    return stringLiterals[0];
}

Type Type::withoutPrimitives(uint16_t removed) const
{
    Type type = *this;
    type.primitives &= ~removed;
    if (!(type.primitives & Primitive::Integer))
        type.integers = Range::empty();
    if (!(type.primitives & Primitive::LiteralString))
        type.stringLiterals.clear();
    if (!(type.primitives & Primitive::LiteralSymbol))
        type.symbolLiterals.clear();
    return type;
}

Type Type::withPrimitives(uint16_t added) const
{
    RELEASE_ASSERT(!(added & (Primitive::LiteralString | Primitive::LiteralSymbol)));
    Type type = *this;
    type.primitives |= added;
    if (added & Primitive::Integer)
        type.integers = Range::full();
    type.canonicalize();
    return type;
}

Type Type::withoutNullish() const
{
    return withoutPrimitives(Primitive::UndefinedOrNull);
}

Type Type::withoutObjects() const
{
    Type type = *this;
    copyObjectParts(type, never());
    return type;
}

Type Type::withOnlyObjects() const
{
    if (!isJavaScriptValue())
        return never();
    return withoutPrimitives(Primitive::Defined);
}

Type Type::withLiteralStringsWidened() const
{
    Type type = *this;
    if (type.primitives & Primitive::LiteralString) {
        type.stringLiterals.clear();
        type.primitives = (type.primitives & ~Primitive::LiteralString) | Primitive::String;
    }
    return type;
}

Type Type::withContentsProven(bool areProven) const
{
    Type type = *this;
    for (ClassPart& part : type.classes)
        part.contentsAreProven = areProven;
    return type;
}

static void widenClassesWithoutInstanceTest(Type& type)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    bool hasSuchClass = std::ranges::any_of(type.classes, [&](const ClassPart& part) {
        return table.at(part.builtinClass).instanceTest == BuiltinInstanceTest::None;
    });
    if (hasSuchClass)
        type.anyObject = true;
}

Type Type::withoutClassProofs() const
{
    Type type = *this;
    widenClassesWithoutInstanceTest(type);
    for (ClassPart& part : type.classes) {
        part.contentsAreProven = false;
        part.provenPristine = { };
        part.arrayStorage = ArrayStorage::Other;
    }
    type.regExpLiteralSet.clear();
    type.canonicalize();
    return type;
}

static void widenUntestableBuiltins(Type& type)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    auto isUntestable = [&](BuiltinID builtin) {
        return !isTestableBuiltin(table, builtin);
    };
    for (BuiltinID builtin : type.builtins) {
        if (isUntestable(builtin))
            (isCallableBuiltin(table, builtin) ? type.anyFunction : type.anyObject) = true;
    }
    removeAllMatching(type.builtins, isUntestable);
}

Type Type::withUntestableBuiltinsWidened() const
{
    Type type = *this;
    widenUntestableBuiltins(type);
    type.canonicalize();
    return type;
}

static void canonicalizeInternalValue(Type& type)
{
    if ((type.opaque && type.environment) || type.hasObjects() || type.primitives & ~Primitive::Absent) {
        uint16_t absent = type.primitives & Primitive::Absent;
        type = Type::anything();
        type.primitives |= absent;
        return;
    }
    Type canonical;
    canonical.primitives = type.primitives;
    canonical.opaque = type.opaque;
    canonical.environment = type.environment;
    type = WTF::move(canonical);
}

static void canonicalizeIntegers(Type& type)
{
    if (type.primitives & Primitive::Integer) {
        type.integers = type.integers.intersectWith(Range::full());
        if (!type.integers.isEmpty())
            return;
        type.primitives &= ~Primitive::Integer;
    }
    type.integers = Range::empty();
}

template<typename Literal>
static void canonicalizeLiterals(uint16_t& primitives, SortedSet<Literal>& literals, uint16_t anyBit, uint16_t literalBit)
{
    if (primitives & anyBit || (literals.isEmpty() && primitives & literalBit)) {
        literals.clear();
        primitives = (primitives & ~literalBit) | anyBit;
        return;
    }
    if (!literals.isEmpty())
        primitives |= literalBit;
}

static void widenToObject(Type& type)
{
    type.anyObject = true;
    type.anyFunction = true;
    type.layouts = { };
    type.classes.clear();
    type.functions.clear();
    type.builtins.clear();
    type.regExpLiteralSet.clear();
}

static void mergeClassParts(ClassPart& part, const ClassPart& other)
{
    part.contentsAreProven &= other.contentsAreProven;
    part.provenPristine = part.provenPristine & other.provenPristine;
    if (part.arrayStorage != other.arrayStorage)
        part.arrayStorage = ArrayStorage::Other;
    if (part.contents.size() < other.contents.size())
        part.contents.grow(other.contents.size());
    for (unsigned i = 0; i < part.contents.size(); ++i) {
        if (part.contentsAt(i) != other.contentsAt(i))
            part.contents[i] = TypeTable::anything;
    }
}

static void canonicalizeClasses(ClassParts& classes)
{
    auto isOutOfOrder = [](const ClassPart& a, const ClassPart& b) {
        return a.builtinClass >= b.builtinClass;
    };
    if (std::ranges::adjacent_find(classes, isOutOfOrder) == classes.end())
        return;
    AOT::stableSort(classes, [](const ClassPart& a, const ClassPart& b) {
        return a.builtinClass < b.builtinClass;
    });
    size_t size = 1;
    for (size_t i = 1; i < classes.size(); ++i) {
        if (classes[i].builtinClass == classes[size - 1].builtinClass)
            mergeClassParts(classes[size - 1], classes[i]);
        else
            classes[size++] = classes[i];
    }
    classes.shrink(size);
}

static void canonicalizeContents(ClassPart& part)
{
    for (TypeID& contents : part.contents) {
        if (!contents)
            contents = TypeTable::anything;
    }
    while (!part.contents.isEmpty() && part.contents.last() == TypeTable::anything)
        part.contents.removeLast();
}

static void canonicalizeObjects(Type& type)
{
    if (type.anyObject) {
        widenToObject(type);
        return;
    }
    canonicalizeClasses(type.classes);
    for (ClassPart& part : type.classes)
        canonicalizeContents(part);
    if (!type.regExpLiteralSet.isEmpty() && !type.findClassPart(BuiltinClasses::RegExp))
        type.regExpLiteralSet.clear();
}

void Type::canonicalize()
{
    primitives &= Primitive::Defined;
    if (!isJavaScriptValue()) {
        canonicalizeInternalValue(*this);
        return;
    }
    canonicalizeIntegers(*this);
    canonicalizeLiterals(primitives, symbolLiterals, Primitive::Symbol, Primitive::LiteralSymbol);
    canonicalizeObjects(*this);
    canonicalizeLiterals(primitives, stringLiterals, Primitive::String, Primitive::LiteralString);
}

void add(Hasher& hasher, const Type& type)
{
    add(hasher, type.primitives, type.anyObject, type.anyFunction);
    add(hasher, type.opaque ? static_cast<unsigned>(*type.opaque) + 1 : 0u);
    add(hasher, type.integers.minimum, type.integers.maximum, type.stringLiterals, type.symbolLiterals, type.layouts, type.environment);
    add(hasher, static_cast<uint32_t>(type.classes.size()));
    for (const ClassPart& part : type.classes) {
        add(hasher, part.builtinClass, part.contentsAreProven, part.provenPristine.toRaw(), part.arrayStorage, static_cast<uint32_t>(part.contents.size()));
        for (TypeID contents : part.contents)
            add(hasher, contents);
    }
    add(hasher, type.functions, type.builtins, type.regExpLiteralSet);
}

void add(Hasher& hasher, RegExpLiteralInType literal)
{
    add(hasher, literal.pattern, literal.rawFlags);
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
