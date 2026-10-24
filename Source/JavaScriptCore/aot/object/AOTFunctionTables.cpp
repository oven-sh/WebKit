/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFunctionTables.h"

#if ENABLE(AOT)

#include "AOTClosure.h"
#include "AOTConvention.h"
#include "AOTObjectWords.h"
#include "JSCInlines.h"
#include "NativeExecutable.h"
#include "Options.h"
#include <algorithm>

namespace JSC { namespace AOT {

static_assert(FunctionTables::numberOfExecutables == ClosureWords::numberOfExecutables);
static_assert(FunctionTables::numberOfExecutables - 1 <= ResumeClosureRequest::maxExecutable);

namespace {

struct FunctionKindFields {
    CallableKind callableKind { CallableKind::Plain };
    bool isAsync { false };
    bool isGenerator { false };
    bool takesNewTarget { false };
    bool isConstructor { false };
    bool isDerivedConstructor { false };
};

} // anonymous namespace

static consteval FunctionKindData functionKindWith(FunctionKindFields fields)
{
    FunctionKindData kind { };
    kind.callableKind = static_cast<unsigned>(fields.callableKind);
    kind.isAsync = fields.isAsync;
    kind.isGenerator = fields.isGenerator;
    kind.takesNewTarget = fields.takesNewTarget;
    kind.isConstructor = fields.isConstructor;
    kind.isDerivedConstructor = fields.isDerivedConstructor;
    return kind;
}

static consteval bool usesNotConstructibleExecutable(FunctionKindFields fields)
{
    FunctionKindData kind = functionKindWith(fields);
    return FunctionTables::executableKind(kind) == FunctionTables::ExecutableKind::NotConstructible && !FunctionTables::canConstruct(kind);
}

static_assert(FunctionTables::executableKind(functionKindWith({ })) == FunctionTables::ExecutableKind::Constructible);
static_assert(FunctionTables::canConstruct(functionKindWith({ })));
static_assert(closureWordFlags(functionKindWith({ })) == ClosureWord::Flags { });

static_assert(FunctionTables::executableKind(functionKindWith({ .isConstructor = true })) == FunctionTables::ExecutableKind::Class);
static_assert(FunctionTables::canConstruct(functionKindWith({ .isConstructor = true })));
static_assert(closureWordFlags(functionKindWith({ .isConstructor = true })) == ClosureWord::Flags { .isNotCallable = true });

static_assert(FunctionTables::executableKind(functionKindWith({ .takesNewTarget = true, .isConstructor = true, .isDerivedConstructor = true })) == FunctionTables::ExecutableKind::Class);
static_assert(closureWordFlags(functionKindWith({ .takesNewTarget = true, .isConstructor = true, .isDerivedConstructor = true }))
    == ClosureWord::Flags { .isNotCallable = true, .takesNewTarget = true });

static_assert(usesNotConstructibleExecutable({ .isAsync = true }));
static_assert(usesNotConstructibleExecutable({ .isGenerator = true }));
static_assert(usesNotConstructibleExecutable({ .isAsync = true, .isGenerator = true }));
static_assert(usesNotConstructibleExecutable({ .callableKind = CallableKind::Arrow }));
static_assert(usesNotConstructibleExecutable({ .callableKind = CallableKind::Method }));

static_assert(FunctionTables::executableKind(functionKindWith({ .callableKind = CallableKind::Bound })) == FunctionTables::ExecutableKind::BoundClosure);
static_assert(!FunctionTables::canConstruct(functionKindWith({ .callableKind = CallableKind::Bound })));

static_assert(FunctionTables::executableKind(functionKindWith({ .takesNewTarget = true })) == FunctionTables::ExecutableKind::Constructible);
static_assert(closureWordFlags(functionKindWith({ .takesNewTarget = true })) == ClosureWord::Flags { .takesNewTarget = true });

unsigned functionLengthOf(FunctionID function)
{
    const ProgramTables& tables = programTables();
    ASSERT(function.index() >= tables.closureFunctions.first && function.index() < tables.closureFunctions.end);
    return tables.functionLengthOf(function.index());
}

FunctionID sourceFunctionOf(FunctionID function)
{
    return FunctionID(programTables().sourceFunctionOf(function.index()));
}

const BoundFunctionData& boundFunctionData(FunctionID function)
{
    auto boundFunctions = programTables().boundFunctions();
    auto found = std::ranges::lower_bound(boundFunctions, function.index(), { }, &BoundFunctionData::function);
    RELEASE_ASSERT(found != boundFunctions.end() && found->function == function.index());
    return *found;
}

static const uint64_t* closureWordsByFunction(const Image& image)
{
    auto section = image.section<uint64_t>(ImageSection::ClosureWords);
    return reinterpret_cast<const uint64_t*>(reinterpret_cast<uintptr_t>(section.data()) - image.programTables().closureFunctions.omittedWordCount * sizeof(uint64_t));
}

FunctionTables::FunctionTables(VM& vm)
    : words(closureWordsByFunction(*Image::current()))
{
    RELEASE_ASSERT(!Options::useJIT());
    for (unsigned index = 0; index < numberOfExecutables; ++index) {
        auto kind = static_cast<ExecutableKind>(index);
        bool isClass = kind == ExecutableKind::Class;
        bool constructs = isClass || kind == ExecutableKind::Constructible;
        NativeExecutable* executable = vm.getHostFunction(isClass ? callClassConstructor : callCompiledClosure, ImplementationVisibility::Private, constructs ? constructCompiledClosure : callHostFunctionAsConstructor, 0, String());
        executables[index] = Strong<JSCell>(vm, executable);
    }
}

NativeExecutable* FunctionTables::executableOf(FunctionID function) const
{
    return uncheckedDowncast<NativeExecutable>(executables[static_cast<unsigned>(executableKind(functionKindData(function)))].get());
}

FunctionTables& FunctionTables::of(VM& vm)
{
    RELEASE_ASSERT(vm.hasCompiledCode());
    auto& tables = vm.aotState().functionTables;
    if (!tables)
        tables = makeUnique<FunctionTables>(vm);
    return *tables;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
