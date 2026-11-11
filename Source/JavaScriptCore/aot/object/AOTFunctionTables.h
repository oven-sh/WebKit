/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTClosureWord.h"
#include "AOTImage.h"
#include "AOTProgramTables.h"
#include "AOTVector.h"
#include "Strong.h"
#include "Weak.h"
#include <array>
#include <wtf/HashMap.h>

namespace JSC {

class JSCell;
class JSString;
class NativeExecutable;
class Structure;
class VM;

namespace AOT {

ALWAYS_INLINE const ProgramTables& programTables()
{
    return Image::current()->programTables();
}

JS_EXPORT_PRIVATE uint32_t closureNumberOf(FunctionID);
JS_EXPORT_PRIVATE FunctionID closureFunctionWithNumber(uint32_t);
inline FunctionID functionOfClosureWord(uint64_t word) { return closureFunctionWithNumber(ClosureWord::closureNumberOf(word)); }
ALWAYS_INLINE FunctionKindData kindOfClosureNumber(uint32_t number) { return programTables().kindOfClosureNumber(number); }
ALWAYS_INLINE CaptureCounts captureCountsOfClosureNumber(uint32_t number) { return programTables().captureCountsOfClosureNumber(number); }

ALWAYS_INLINE FunctionKindData functionKindData(FunctionID function)
{
    return programTables().kindOfFunction(function.index());
}

ALWAYS_INLINE CaptureCounts captureCountsOf(FunctionID function)
{
    const ProgramTables& tables = programTables();
    ASSERT(function.index() >= tables.closureFunctions.first && function.index() < tables.closureFunctions.end);
    return tables.captureCountsOf(function.index());
}

unsigned functionLengthOf(FunctionID);
FunctionID sourceFunctionOf(FunctionID);

constexpr ClosureWord::Flags closureWordFlags(FunctionKindData kind)
{
    return { .isNotCallable = !!kind.isConstructor, .takesNewTarget = !!kind.takesNewTarget };
}

struct FunctionTables {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FunctionTables);

    static FunctionTables& of(VM&);
    static FunctionTables& createFor(VM&);
    explicit FunctionTables(VM&);

    enum class ExecutableKind : uint8_t { NotConstructible, Constructible, Class };
    static constexpr unsigned numberOfExecutables = 3;
    static_assert(static_cast<unsigned>(ExecutableKind::Class) + 1 == numberOfExecutables);

    static constexpr ExecutableKind executableKind(FunctionKindData kind)
    {
        if (kind.isConstructor)
            return ExecutableKind::Class;
        switch (kind.callable()) {
        case CallableKind::Plain:
            return kind.isAsync || kind.isGenerator ? ExecutableKind::NotConstructible : ExecutableKind::Constructible;
        case CallableKind::Arrow:
        case CallableKind::Method:
            return ExecutableKind::NotConstructible;
        }
        RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
    }
    static constexpr bool canConstruct(FunctionKindData kind) { return executableKind(kind) == ExecutableKind::Constructible || executableKind(kind) == ExecutableKind::Class; }
    static constexpr bool hasPrototypePropertyAtCreation(FunctionKindData kind)
    {
        if (kind.isConstructor)
            return true;
        if (kind.isInRuntimeModule)
            return false;
        return kind.isGenerator || executableKind(kind) == ExecutableKind::Constructible;
    }
    NativeExecutable* executableOf(FunctionID) const;

    using SourceTextMap = UncheckedKeyHashMap<FunctionID, Weak<JSString>>;

    const uint64_t* words;
    std::array<Strong<JSCell>, numberOfExecutables> executables;
    Vector<Strong<Structure>> closureStructuresWithoutTable;
    SourceTextMap sourceTexts;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
