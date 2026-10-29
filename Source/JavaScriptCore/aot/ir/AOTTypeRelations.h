/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTSortedSet.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

enum class ClassRelation : uint8_t { Unknown, Disjoint };

ClassRelation relate(const BuiltinTable&, BuiltinClassID, BuiltinClassID);
bool isFunctionClass(const BuiltinTable&, BuiltinClassID);
bool isCallableBuiltin(const BuiltinTable&, BuiltinID);
bool isTestableBuiltin(const BuiltinTable&, BuiltinID);

bool hasCallableParts(const Type&);
bool includesFunction(const Type&, FunctionID);
bool includesBuiltin(const Type&, BuiltinID, const BuiltinTable&);
bool mayContainAnyCallable(const BuiltinTable&, const Type&);
bool mayContainAnyClosure(const BuiltinTable&, const Type&);
bool mayBeInstanceOf(const BuiltinTable&, BuiltinID, BuiltinClassID);
bool mayBeClosureOf(const TypeTable&, BuiltinID, FunctionID);
struct LibraryClosuresOfBuiltin {
    bool mayBeAnyFunctionOfLibrary { false };
    Vector<FunctionID, 2> functions;
};
LibraryClosuresOfBuiltin libraryClosuresOf(const TypeTable&, BuiltinID);
const RuntimeFunctionData* findTableRecordOf(const Program&, FunctionID);
bool mayBeClosureOf(const TypeTable&, BuiltinID, FunctionID, const RuntimeFunctionData* recordOfFunction);
bool mayContainBuiltin(const TypeTable&, const Type&, BuiltinID);
bool overlapsAnotherClass(const BuiltinTable&, BuiltinClassID, const Type&);

bool isSubset(const Type&, const Type&, const TypeTable&);
bool isPartSubset(const ClassPart&, const ClassPart&, const TypeTable&);
bool overlap(const Type&, const Type&, const TypeTable&);
bool mayBeSameObject(const Type&, const Type&, const TypeTable&);

enum class DeclaredParts : uint8_t { FromEither, FromSecond };

Type intersect(const Type&, const Type&, TypeArena&, DeclaredParts);

void copyObjectParts(Type& target, const Type& source);

template<typename T>
void removeAllMatching(SortedSet<T>& set, NOESCAPE const Invocable<bool(T)> auto& matches)
{
    if (std::ranges::none_of(set, matches))
        return;
    SortedSet<T> remaining;
    for (T value : set) {
        if (!matches(value))
            remaining.add(value);
    }
    set = WTF::move(remaining);
}

template<typename T>
struct LiteralPart {
    uint16_t widenedBit;
    uint16_t literalBit;
    SortedSet<T> Type::* literals;
};

inline constexpr LiteralPart<StringID> stringPart { Primitive::String, Primitive::LiteralString, &Type::stringLiterals };
inline constexpr LiteralPart<PropertyKeyID> symbolPart { Primitive::Symbol, Primitive::LiteralSymbol, &Type::symbolLiterals };
inline constexpr uint16_t plainPrimitives = Primitive::Defined & ~(Primitive::Integer | Primitive::AnyString | Primitive::AnySymbol);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
