/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeRelations.h"

#if ENABLE(AOT)

#include "AOTBuiltinTableRealm.h"
#include "AOTIRProgram.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

template<typename T>
struct LiteralPart {
    uint16_t widenedBit;
    uint16_t literalBit;
    SortedSet<T> Type::* literals;
};

constexpr LiteralPart<StringID> stringPart { Primitive::String, Primitive::LiteralString, &Type::stringLiterals };
constexpr LiteralPart<PropertyKeyID> symbolPart { Primitive::Symbol, Primitive::LiteralSymbol, &Type::symbolLiterals };
constexpr uint16_t plainPrimitives = Primitive::Defined & ~(Primitive::Integer | Primitive::AnyString | Primitive::AnySymbol);

} // anonymous namespace

ClassRelation relate(const BuiltinTable& builtins, BuiltinClassID first, BuiltinClassID second)
{
    const BuiltinClass& a = builtins.at(first);
    const BuiltinClass& b = builtins.at(second);
    if (a.instanceTest == BuiltinInstanceTest::None || b.instanceTest == BuiltinInstanceTest::None)
        return ClassRelation::Unknown;

    bool haveSameCellType = a.type == b.type;
    if (a.instanceTest == BuiltinInstanceTest::JSTypeEquals && b.instanceTest == BuiltinInstanceTest::JSTypeEquals)
        return haveSameCellType ? ClassRelation::Unknown : ClassRelation::Disjoint;
    if (a.instanceTest == BuiltinInstanceTest::SharingModeEquals && b.instanceTest == BuiltinInstanceTest::SharingModeEquals)
        return a.sharingMode == b.sharingMode ? ClassRelation::Unknown : ClassRelation::Disjoint;
    if (builtins.isSameOrSubclassOf(first, second) || builtins.isSameOrSubclassOf(second, first))
        return ClassRelation::Unknown;
    // Every cell of its type is an instance of a class that is tested by cell type.
    if (a.instanceTest != b.instanceTest && haveSameCellType)
        return ClassRelation::Unknown;
    return ClassRelation::Disjoint;
}

bool isFunctionClass(const BuiltinTable& builtins, BuiltinClassID id)
{
    if (builtins.isSameOrSubclassOf(id, BuiltinClasses::Function))
        return true;
    JSType type = builtins.at(id).type;
    return isFunctionType(type) || type == ProxyObjectType;
}

bool isCallableBuiltin(const BuiltinTable& builtins, BuiltinID id)
{
    switch (builtins.at(id).kind) {
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Method:
        return true;
    case BuiltinKind::Prototype:
        return !!builtins.dispatchOf(id, BuiltinCallMode::Call);
    case BuiltinKind::Namespace:
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
    case BuiltinKind::Constant:
    case BuiltinKind::NativeBinding:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool isTestableBuiltin(const BuiltinTable& builtins, BuiltinID id)
{
    return BuiltinCellKey::object(id).hasCell(builtins);
}

bool hasCallableParts(const Type& type)
{
    return type.anyFunction || !type.functions.isEmpty();
}

bool includesFunction(const Type& type, FunctionID function)
{
    return type.anyFunction || type.functions.contains(function);
}

bool includesBuiltin(const Type& type, BuiltinID builtin, const BuiltinTable& builtins)
{
    return type.builtins.contains(builtin) || (type.anyFunction && isCallableBuiltin(builtins, builtin));
}

bool mayContainAnyCallable(const BuiltinTable& builtins, const Type& type)
{
    return type.anyFunction || std::ranges::any_of(type.classes, [&](const ClassPart& part) {
        return isFunctionClass(builtins, part.builtinClass);
    });
}

bool mayContainAnyBuiltin(const Type& type)
{
    return type.anyFunction || !type.classes.isEmpty();
}

static bool isNativeCallee(BuiltinCallee callee)
{
    switch (callee) {
    case BuiltinCallee::None:
    case BuiltinCallee::Function:
        return false;
    case BuiltinCallee::HostFunction:
    case BuiltinCallee::InternalFunction:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The engine, the host and the library make the built-in objects, so none is a closure of a function of the application.
// Where the table names the code of an entry, the entry's function object is native or a closure of that function.
// An entry that calls whatever its property holds, or a function that the table does not name, may be a closure of any function of the library.
bool mayBeClosureOf(const TypeTable& types, BuiltinID builtin, FunctionID function)
{
    const Program* program = types.program();
    if (!program)
        return true;
    if (!isLibraryCode(*program, program->at(function)))
        return false;

    const BuiltinTable& builtins = types.builtins();
    bool isKnown = false;
    for (const BuiltinOverload& overload : builtins.overloadsOf(builtins.canonicalID(builtin))) {
        const BuiltinImplementation& implementation = overload.code.implementation;
        const RuntimeFunctionEntry* entry = findRuntimeFunction(*program, implementation.function);
        if (entry && entry->function == function)
            return true;
        if (implementation.function || isNativeCallee(implementation.callee))
            isKnown = true;
        else if (implementation.kind == BuiltinImplementationKind::Generic || implementation.callee == BuiltinCallee::Function)
            return true;
    }
    return !isKnown;
}

bool mayContainBuiltin(const TypeTable& types, const Type& type, BuiltinID builtin)
{
    if (mayContainAnyBuiltin(type) || type.builtins.contains(builtin))
        return true;
    return std::ranges::any_of(type.functions, [&](FunctionID function) {
        return mayBeClosureOf(types, builtin, function);
    });
}

bool overlapsAnotherClass(const BuiltinTable& builtins, BuiltinClassID builtinClass, const Type& type)
{
    return std::ranges::any_of(type.classes, [&](const ClassPart& part) {
        return part.builtinClass != builtinClass && relate(builtins, builtinClass, part.builtinClass) == ClassRelation::Unknown;
    });
}

bool isPartSubset(const ClassPart& a, const ClassPart& b, const TypeTable& types)
{
    if (!a.provenPristine.containsAll(b.provenPristine))
        return false;
    if (!b.contentsAreProven)
        return true;
    if (!a.contentsAreProven)
        return false;
    if (b.arrayStorage != ArrayStorage::Other && a.arrayStorage != b.arrayStorage)
        return false;
    for (unsigned index = 0; index < b.contents.size(); ++index) {
        if (!isSubset(types.at(a.contentsAt(index)), types.at(b.contentsAt(index)), types))
            return false;
    }
    return true;
}

static bool areCallablesSubset(const Type& a, const Type& b, const BuiltinTable& builtins)
{
    if (a.anyFunction && !b.anyFunction)
        return false;
    bool includesFunctions = std::ranges::all_of(a.functions, [&](FunctionID function) {
        return includesFunction(b, function);
    });
    return includesFunctions && std::ranges::all_of(a.builtins, [&](BuiltinID builtin) {
        return includesBuiltin(b, builtin, builtins);
    });
}

static bool areClassesSubset(const Type& a, const Type& b, const TypeTable& types)
{
    return std::ranges::all_of(a.classes, [&](const ClassPart& part) {
        const ClassPart* partner = b.findClassPart(part.builtinClass);
        return partner && isPartSubset(part, *partner, types);
    });
}

static bool areRegExpLiteralsSubset(const Type& a, const Type& b)
{
    if (b.regExpLiteralSet.isEmpty() || !a.findClassPart(BuiltinClasses::RegExp))
        return true;
    return !a.regExpLiteralSet.isEmpty() && a.regExpLiteralSet.isSubsetOf(b.regExpLiteralSet);
}

static bool areLayoutsSubset(const Type& a, const Type& b, const TypeTable& types)
{
    if (!a.layouts || a.layouts == b.layouts)
        return true;
    if (!b.layouts)
        return false;
    return isSubsetOf(types.at(a.layouts), types.at(b.layouts));
}

static bool areObjectsSubset(const Type& a, const Type& b, const TypeTable& types)
{
    if (!a.hasObjects() || b.anyObject)
        return true;
    if (a.anyObject)
        return false;
    return areCallablesSubset(a, b, types.builtins())
        && areClassesSubset(a, b, types)
        && areRegExpLiteralsSubset(a, b)
        && areLayoutsSubset(a, b, types);
}

bool isSubset(const Type& a, const Type& b, const TypeTable& types)
{
    if (&a == &b)
        return true;
    uint16_t missingPrimitives = a.primitives & ~b.primitives;
    if (b.primitives & Primitive::String)
        missingPrimitives &= ~Primitive::LiteralString;
    if (b.primitives & Primitive::Symbol)
        missingPrimitives &= ~Primitive::LiteralSymbol;
    if (missingPrimitives)
        return false;
    if (a.primitives & Primitive::Integer && !a.integers.isSubsetOf(b.integers))
        return false;
    if (!(b.primitives & Primitive::String) && !a.stringLiterals.isSubsetOf(b.stringLiterals))
        return false;
    if (!(b.primitives & Primitive::Symbol) && !a.symbolLiterals.isSubsetOf(b.symbolLiterals))
        return false;
    if (a.opaque && a.opaque != b.opaque)
        return false;
    if (a.environment && a.environment != b.environment)
        return false;
    return areObjectsSubset(a, b, types);
}

bool Type::isSubsetOf(const Type& other, const Program& program) const
{
    return isSubset(*this, other, program.types);
}

template<typename T>
static bool literalsOverlap(const Type& a, const Type& b, LiteralPart<T> part)
{
    uint16_t bits = part.widenedBit | part.literalBit;
    if (!(a.primitives & bits) || !(b.primitives & bits))
        return false;
    return (a.primitives | b.primitives) & part.widenedBit || (a.*part.literals).overlaps(b.*part.literals);
}

static bool callablesOverlap(const BuiltinTable& builtins, const Type& a, const Type& b)
{
    return (hasCallableParts(a) && mayContainAnyCallable(builtins, b))
        || (hasCallableParts(b) && mayContainAnyCallable(builtins, a))
        || a.functions.overlaps(b.functions);
}

static bool mayContainBuiltinOf(const TypeTable& types, const Type& container, const Type& type)
{
    return std::ranges::any_of(type.builtins, [&](BuiltinID builtin) {
        return mayContainBuiltin(types, container, builtin);
    });
}

static bool builtinsOverlap(const Type& a, const Type& b, const TypeTable& types)
{
    return mayContainBuiltinOf(types, b, a) || mayContainBuiltinOf(types, a, b);
}

static bool regExpLiteralsOverlap(const Type& a, const Type& b)
{
    return a.regExpLiteralSet.isEmpty() || b.regExpLiteralSet.isEmpty() || a.regExpLiteralSet.overlaps(b.regExpLiteralSet);
}

static bool classesOverlap(const BuiltinTable& builtins, const Type& a, const Type& b)
{
    return std::ranges::any_of(a.classes, [&](const ClassPart& part) {
        if (overlapsAnotherClass(builtins, part.builtinClass, b))
            return true;
        if (!b.findClassPart(part.builtinClass))
            return false;
        return part.builtinClass != BuiltinClasses::RegExp || regExpLiteralsOverlap(a, b);
    });
}

static bool layoutsOverlap(const Type& a, const Type& b, const TypeTable& types)
{
    if (!a.layouts || !b.layouts)
        return false;
    return a.layouts == b.layouts || overlaps(types.at(a.layouts), types.at(b.layouts));
}

static bool objectsOverlap(const Type& a, const Type& b, const TypeTable& types)
{
    if (!a.hasObjects() || !b.hasObjects())
        return false;
    if (a.anyObject || b.anyObject)
        return true;
    const BuiltinTable& builtins = types.builtins();
    return callablesOverlap(builtins, a, b) || builtinsOverlap(a, b, types) || classesOverlap(builtins, a, b) || layoutsOverlap(a, b, types);
}

bool overlap(const Type& a, const Type& b, const TypeTable& types)
{
    if (a.primitives & b.primitives & plainPrimitives)
        return true;
    if (!a.integers.intersectWith(b.integers).isEmpty())
        return true;
    if (literalsOverlap(a, b, stringPart) || literalsOverlap(a, b, symbolPart))
        return true;
    if (a.opaque && a.opaque == b.opaque)
        return true;
    if (a.environment && a.environment == b.environment)
        return true;
    return objectsOverlap(a, b, types);
}

bool Type::overlaps(const Type& other, const Program& program) const
{
    return overlap(*this, other, program.types);
}

std::span<const LayoutID> membersOf(const TypeTable& types, const Type& type)
{
    if (!type.layouts)
        return { };
    return types.at(type.layouts).members.span();
}

std::span<const LayoutID> membersOf(const Program& program, const Type& type)
{
    return membersOf(program.types, type);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
