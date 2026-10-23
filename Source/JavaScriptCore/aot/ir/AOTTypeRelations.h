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
// A test finds the object of a built-in by the cell the realm keeps for it. There is none where each module has an object of its own, and none for what a native binding answers.
bool isTestableBuiltin(const BuiltinTable&, BuiltinID);

bool hasCallableParts(const Type&);
bool includesFunction(const Type&, FunctionID);
bool includesBuiltin(const Type&, BuiltinID, const BuiltinTable&);
bool mayContainAnyCallable(const BuiltinTable&, const Type&);
bool mayContainAnyBuiltin(const Type&);
bool mayBeClosureOf(const TypeTable&, BuiltinID, FunctionID);
struct LibraryClosuresOfBuiltin {
    bool mayBeAnyFunctionOfLibrary { false };
    Vector<FunctionID, 2> functions;
};
LibraryClosuresOfBuiltin libraryClosuresOf(const TypeTable&, BuiltinID);
// For a caller that asks about many built-ins and one function: the record is findTableRecordOf(program, function).
const RuntimeFunctionData* findTableRecordOf(const Program&, FunctionID);
bool mayBeClosureOf(const TypeTable&, BuiltinID, FunctionID, const RuntimeFunctionData* recordOfFunction);
bool mayContainBuiltin(const TypeTable&, const Type&, BuiltinID);
bool overlapsAnotherClass(const BuiltinTable&, BuiltinClassID, const Type&);

bool isSubset(const Type&, const Type&, const TypeTable&);
bool isPartSubset(const ClassPart&, const ClassPart&, const TypeTable&);
bool overlap(const Type&, const Type&, const TypeTable&);
// Whether a value of the one type and a value of the other can be one piece of memory that somebody writes. Two values that are both undefined, or one number, are none.
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
