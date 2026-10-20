/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTClosureWord.h"
#include "AOTImage.h"
#include "AOTProgramTables.h"
#include "Strong.h"
#include "Weak.h"
#include <array>
#include <atomic>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class JSCell;
class JSObject;
class NativeExecutable;
class VM;

namespace AOT {

ALWAYS_INLINE const ProgramTables& programTables()
{
    return Image::current()->programTables();
}

ALWAYS_INLINE FunctionKindData functionKindData(FunctionID function)
{
    return programTables().functionKinds()[function.index()];
}

ALWAYS_INLINE CaptureCounts captureCountsOf(FunctionID function)
{
    const ProgramTables& tables = programTables();
    ASSERT(function.index() >= tables.closureFunctions.first && function.index() < tables.closureFunctions.end);
    return CaptureCounts::decode(tables.closureCaptureCounts()[function.index() - tables.closureFunctions.first]);
}

unsigned functionLengthOf(FunctionID);
FunctionID sourceFunctionOf(FunctionID);
const BoundFunctionData& boundFunctionData(FunctionID);

constexpr ClosureWord::Flags closureWordFlags(FunctionKindData kind)
{
    return { .isNotCallable = !!kind.isConstructor, .takesNewTarget = !!kind.takesNewTarget };
}

struct FunctionTables {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FunctionTables);

    static FunctionTables& of(VM&);
    explicit FunctionTables(VM&);

    enum class ExecutableKind : uint8_t { NotConstructible, Constructible, Class, BoundClosure };
    static constexpr unsigned numberOfExecutables = 4;
    static_assert(static_cast<unsigned>(ExecutableKind::BoundClosure) + 1 == numberOfExecutables);

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
        case CallableKind::Bound:
            return ExecutableKind::BoundClosure;
        }
        RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
    }
    static constexpr bool canConstruct(FunctionKindData kind) { return executableKind(kind) == ExecutableKind::Constructible || executableKind(kind) == ExecutableKind::Class; }
    // JavaScriptCore creates a built-in function written in JavaScript without a prototype property (JSFunction::mayHaveNonReifiedPrototype), and the definition of a class adds one.
    // A property that exists from creation is a data property that nothing can delete or turn into an accessor.
    static constexpr bool hasPrototypePropertyAtCreation(FunctionKindData kind)
    {
        if (kind.isConstructor)
            return true;
        if (kind.isInRuntimeModule)
            return false;
        return kind.isGenerator || executableKind(kind) == ExecutableKind::Constructible;
    }
    NativeExecutable* executableOf(FunctionID) const;

    using NamesByFunction = UncheckedKeyHashMap<FunctionID, String>;
    struct ClosureWithDisplayName {
        Weak<JSObject> closure;
        bool isOneOfSeveral { false };
    };
    using ClosuresByFunction = UncheckedKeyHashMap<FunctionID, ClosureWithDisplayName>;

    const uint64_t* words;
    std::array<Strong<JSCell>, numberOfExecutables> executables;
    Lock namesLock;
    NamesByFunction namesSetAtRunTime WTF_GUARDED_BY_LOCK(namesLock);
    ClosuresByFunction closuresWithDisplayName WTF_GUARDED_BY_LOCK(namesLock);
    std::atomic<bool> hasNamesSetAtRunTime { false };
    std::atomic<bool> hasDisplayNames { false };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
