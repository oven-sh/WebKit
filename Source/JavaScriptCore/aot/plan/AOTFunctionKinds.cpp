/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFunctionKinds.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTCallingConventions.h"
#include "AOTConvention.h"
#include "AOTFunctionDescription.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTRepresentation.h"
#include "AOTSlotPlan.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

Representation closureCaptureRepresentationOf(const Program& program, const CallingConventions& conventions, FunctionID function, unsigned capture)
{
    RELEASE_ASSERT(capture < program.at(function).captures.size());
    return conventions.parameterRepresentation(program, { function, CodeRole::Body }, capture);
}

static ALWAYS_INLINE Vector<Representation, 8> formsOfClosureCaptureWords(const Program& program, const CallingConventions& conventions, FunctionID function)
{
    return Vector<Representation, 8>(program.at(function).captures.size(), [&](size_t capture) {
        return closureCaptureRepresentationOf(program, conventions, function, static_cast<unsigned>(capture));
    });
}

static bool closureKeepsAsReference(Representation held) { return holdsCells(held); }

static bool closureHasWordFor(Representation held) { return held != Representation::Void; }

static unsigned numberOfReferencesAmongCaptures(std::span<const Representation> captures)
{
    unsigned count = 0;
    for (Representation capture : captures)
        count += closureKeepsAsReference(capture);
    return count;
}

static unsigned numberOfWordsAmongCaptures(std::span<const Representation> captures)
{
    unsigned count = 0;
    for (Representation capture : captures)
        count += closureHasWordFor(capture);
    return count;
}

static unsigned slotAmongClosureCaptures(std::span<const Representation> captures, unsigned capture)
{
    RELEASE_ASSERT(capture < captures.size() && closureHasWordFor(captures[capture]));
    unsigned referencesInFront = numberOfReferencesAmongCaptures(captures.first(capture));
    if (closureKeepsAsReference(captures[capture]))
        return referencesInFront;
    return numberOfReferencesAmongCaptures(captures) + numberOfWordsAmongCaptures(captures.first(capture)) - referencesInFront;
}

ClosureCaptureCounts closureCaptureCountsOf(const Program& program, const CallingConventions& conventions, FunctionID function)
{
    const Vector<Representation, 8> held = formsOfClosureCaptureWords(program, conventions, function);
    return { numberOfReferencesAmongCaptures(held.span()), numberOfWordsAmongCaptures(held.span()) };
}

unsigned closureCaptureSlotOf(const Program& program, const CallingConventions& conventions, FunctionID function, unsigned capture)
{
    const Vector<Representation, 8> held = formsOfClosureCaptureWords(program, conventions, function);
    return slotAmongClosureCaptures(held.span(), capture);
}

static std::optional<CoroutineKind> coroutineKindWrittenFor(const Function& function)
{
    bool isGenerator = function.has(FunctionFlags::IsGenerator);
    if (function.has(FunctionFlags::IsAsync))
        return isGenerator ? CoroutineKind::AsyncGenerator : CoroutineKind::AsyncFunction;
    if (isGenerator)
        return CoroutineKind::Generator;
    return std::nullopt;
}

static_assert(!static_cast<unsigned>(MethodKind::Method) && static_cast<unsigned>(MethodKind::Getter) == 1 && static_cast<unsigned>(MethodKind::Setter) == 2 && numberOfEnumerators<MethodKind> <= 1 << 2);
static_assert(static_cast<unsigned>(CallableKind::Method) < 1 << 2);

static FunctionKindData functionKindDataOfWrittenKind(FunctionKind functionKind)
{
    FunctionKindData kind { };
    switch (functionKind) {
    case FunctionKind::Plain:
    case FunctionKind::Module:
        break;
    case FunctionKind::Arrow:
        kind.callableKind = static_cast<unsigned>(CallableKind::Arrow);
        break;
    case FunctionKind::Method:
    case FunctionKind::Initializer:
        kind.callableKind = static_cast<unsigned>(CallableKind::Method);
        break;
    case FunctionKind::Getter:
        kind.callableKind = static_cast<unsigned>(CallableKind::Method);
        kind.accessorKind = static_cast<unsigned>(MethodKind::Getter);
        break;
    case FunctionKind::Setter:
        kind.callableKind = static_cast<unsigned>(CallableKind::Method);
        kind.accessorKind = static_cast<unsigned>(MethodKind::Setter);
        break;
    case FunctionKind::BaseConstructor:
        kind.isConstructor = true;
        break;
    case FunctionKind::DerivedConstructor:
        kind.isConstructor = true;
        kind.isDerivedConstructor = true;
        break;
    case FunctionKind::CommonJsModule:
        kind.isCommonJSModule = true;
        break;
    }
    return kind;
}

static bool functionObjectsLieInRuntimeModule(const Program& program, const Function& function)
{
    if (function.module && isBundlerHelperModule(program, program.at(function.module)))
        return false;
    if (function.role == FunctionRole::Library || function.enclosingRuntimeFunction)
        return true;
    return function.module && program.at(function.module).kind == ModuleKind::RuntimeSource;
}

static bool closuresOfKindMayBeSlim(FunctionKind kind)
{
    switch (kind) {
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
        return true;
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool closuresOfFunctionMayBeSlim(const Function& function)
{
    if (!closuresOfKindMayBeSlim(function.kind))
        return false;
    if (function.hasAny({ FunctionFlags::IsGenerator, FunctionFlags::HasNewTarget, FunctionFlags::TakesCallee }))
        return false;
    return function.captures.size() <= ClosureRequest::maxSlimClosureCaptures;
}

static bool closuresAreExpectedToStayAsMade(const Program& program, const LinkTables& linked, FunctionID id)
{
    const Function& function = program.at(id);
    if (functionObjectsLieInRuntimeModule(program, function))
        return false;
    if (id.index() < linked.functions.size() && linked.function(id).closuresAreChanged)
        return false;
    return function.kind != FunctionKind::Plain || function.has(FunctionFlags::IsAsync) || !function.constructedClass;
}

Vector<ClosureForm> chooseClosureForms(const Program&, const LinkTables&);
Vector<ClosureForm> chooseClosureForms(const Program& program, const LinkTables& linked)
{
    return Vector<ClosureForm>(program.functions.size(), [&](size_t index) {
        return closuresOfFunctionMayBeSlim(program.functions[index]) && closuresAreExpectedToStayAsMade(program, linked, FunctionID(index)) ? ClosureForm::Slim : ClosureForm::Full;
    });
}

BitVector findFunctionsWhoseClosuresGetOnlyListedNames(const Program&, const LinkTables&);
BitVector findFunctionsWhoseClosuresGetOnlyListedNames(const Program& program, const LinkTables& linked)
{
    BitVector functions(program.functions.size());
    for (size_t index = 0; index < std::min<size_t>(program.functions.size(), linked.functions.size()); ++index) {
        const Function& function = program.functions[index];
        if (!isClassConstructorOf(function.kind) && !functionObjectsLieInRuntimeModule(program, function) && linked.functions[index].closuresGetOnlyNamesThatAreListed)
            functions.quickSet(index);
    }
    return functions;
}

BitVector findFunctionsWhosePrototypePropertyStaysAsMade(const Program&, const LinkTables&);
BitVector findFunctionsWhosePrototypePropertyStaysAsMade(const Program& program, const LinkTables& linked)
{
    BitVector functions(program.functions.size());
    for (size_t index = 0; index < std::min<size_t>(program.functions.size(), linked.functions.size()); ++index) {
        const Function& function = program.functions[index];
        if (!isClassConstructorOf(function.kind) && !functionObjectsLieInRuntimeModule(program, function) && linked.functions[index].prototypePropertyStaysAsMade)
            functions.quickSet(index);
    }
    return functions;
}

bool isAsyncFunctionWithoutAwait(const Program&, const Function& source)
{
    return coroutineKindWrittenFor(source) == CoroutineKind::AsyncFunction && !source.syntax.contains(FunctionSyntax::HasLiveAwait);
}

static CodeUnit entryNamedByClosureWordsFor(CodeUnit unit)
{
    switch (unit.role) {
    case CodeRole::GenericCallEntry:
    case CodeRole::GenericConstructEntry:
    case CodeRole::AllocatingConstructEntry:
        return unit;
    case CodeRole::Body:
    case CodeRole::CoroutineBody:
    case CodeRole::ModuleStart:
    case CodeRole::SlowBody:
    case CodeRole::ParameterGuard:
    case CodeRole::TableEntry:
    case CodeRole::TableLookup:
        return { unit.function, CodeRole::GenericCallEntry };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool closureEntryTakesNewTarget(const Function& function, const CallingConventions& conventions, CodeUnit entry)
{
    if (!conventions.exists(entry))
        return function.has(FunctionFlags::HasNewTarget);
    GenericEntryPlan plan = conventions.genericEntry(entry);
    return plan.takesNewTarget || plan.constructEntry;
}

FunctionKindData functionKindOf(const Program& program, const SlotPlan& slots, const CallingConventions& conventions, CodeUnit closureEntry)
{
    CodeUnit entry = entryNamedByClosureWordsFor(closureEntry);
    const Function& function = program.at(entry.function);

    FunctionKindData kind = functionKindDataOfWrittenKind(function.kind);
    kind.isAsync = function.has(FunctionFlags::IsAsync);
    kind.isGenerator = function.has(FunctionFlags::IsGenerator);
    kind.takesNewTarget = closureEntryTakesNewTarget(function, conventions, entry);
    kind.hasInferredName = !function.has(FunctionFlags::HasExplicitName);
    kind.isInRuntimeModule = functionObjectsLieInRuntimeModule(program, function);
    kind.isSlim = slots.closureFormOf(entry.function) == ClosureForm::Slim;
    kind.closuresGetOnlyListedNames = slots.closuresGetOnlyListedNames(entry.function);
    kind.prototypePropertyStaysAsMade = slots.prototypePropertyStaysAsMade(entry.function);
    return kind;
}

static TraceVisibility traceVisibilityOfUnit(const Function& function, CodeRole role, bool isBuiltinThatProgramsCall, bool isEntryThatWalksBody)
{
    if (function.has(FunctionFlags::IsHiddenInStackTraces) && !isBuiltinThatProgramsCall)
        return TraceVisibility::Hidden;

    switch (isEntryThatWalksBody ? CodeRole::Body : role) {
    case CodeRole::Body:
        if (function.isCoroutine())
            return TraceVisibility::VisibleOnlyOnTop;
        break;
    case CodeRole::CoroutineBody:
    case CodeRole::SlowBody:
        break;
    case CodeRole::GenericCallEntry:
    case CodeRole::GenericConstructEntry:
    case CodeRole::AllocatingConstructEntry:
    case CodeRole::ModuleStart:
    case CodeRole::ParameterGuard:
    case CodeRole::TableEntry:
        return TraceVisibility::VisibleOnlyOnTop;
    case CodeRole::TableLookup:
        return TraceVisibility::Hidden;
    }
    return function.isHiddenInStackTraces() ? TraceVisibility::VisibleOnlyOnTop : TraceVisibility::Visible;
}

static bool unitRejectsOnThrow(const Function& function, CodeRole role, bool bodyNeverSuspends)
{
    if (coroutineKindWrittenFor(function) != CoroutineKind::AsyncFunction)
        return false;
    switch (role) {
    case CodeRole::Body:
        return true;
    case CodeRole::CoroutineBody:
        return !bodyNeverSuspends;
    case CodeRole::GenericCallEntry:
    case CodeRole::GenericConstructEntry:
    case CodeRole::AllocatingConstructEntry:
    case CodeRole::ModuleStart:
    case CodeRole::SlowBody:
    case CodeRole::ParameterGuard:
    case CodeRole::TableEntry:
    case CodeRole::TableLookup:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool rejectsOnThrow(const Program& program, const CallingConventions& conventions, CodeUnit unit)
{
    return unitRejectsOnThrow(program.at(unit.function), unit.role, conventions.of(unit.function).coroutineNeverSuspends);
}

static bool frameOfUnitIsNamedByOwner(const Function& function, CodeRole role)
{
    if (!isBodyOfCoroutine(role, function.isCoroutine()))
        return false;
    std::optional<CoroutineKind> kind = coroutineKindWrittenFor(function);
    if (!kind)
        return false;

    bool hasExplicitName = function.has(FunctionFlags::HasExplicitName);
    switch (*kind) {
    case CoroutineKind::AsyncFunction:
        return !hasExplicitName && function.syntax.contains(FunctionSyntax::HasLiveAwait);
    case CoroutineKind::Generator:
    case CoroutineKind::AsyncGenerator:
        return hasExplicitName;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<BuiltinID> builtinImplementedByTracedFunction(const Program& program, FunctionID id)
{
    const Function& function = program.at(id);
    if (!function.enclosingRuntimeFunction)
        return std::nullopt;
    const RuntimeFunctionEntry* entry = findRuntimeFunction(program, function.enclosingRuntimeFunction);
    if (!entry || entry->function != id)
        return std::nullopt;
    std::span<const BuiltinID> implemented = program.builtins().at(function.enclosingRuntimeFunction).implements;
    if (implemented.empty())
        return std::nullopt;
    return implemented.front();
}

static bool overloadOfTracedBuiltinCallsHostFunction(const BuiltinOverload& overload)
{
    switch (overload.code.implementation.callee) {
    case BuiltinCallee::None:
    case BuiltinCallee::Function:
        return false;
    case BuiltinCallee::HostFunction:
    case BuiltinCallee::InternalFunction:
    case BuiltinCallee::CustomGetter:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isTracedInsideBuiltinFunction(const BuiltinTable& builtins, const Function& function)
{
    if (function.syntax.contains(FunctionSyntax::IsShownAsNativeCode))
        return true;
    if (!function.enclosingRuntimeFunction)
        return false;
    switch (builtins.at(function.enclosingRuntimeFunction).role) {
    case RuntimeFunctionRole::EngineBuiltin:
    case RuntimeFunctionRole::HostBuiltin:
        return true;
    case RuntimeFunctionRole::CompilerFunction:
    case RuntimeFunctionRole::Model:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isTracedFunctionOfCompiler(const BuiltinTable& builtins, const Function& function)
{
    return function.enclosingRuntimeFunction && builtins.at(function.enclosingRuntimeFunction).role == RuntimeFunctionRole::CompilerFunction;
}

static bool tracedBuiltinIsCalledByPrograms(const BuiltinTable& builtins, const Function& function, std::optional<BuiltinID> builtin)
{
    return builtin && !isTracedFunctionOfCompiler(builtins, function) && builtins.at(*builtin).holder != Builtins::CompilerHelpers;
}

static StackTraceBuiltinKind stackTraceBuiltinKindOf(const BuiltinTable& builtins, const Function& function, std::optional<BuiltinID> builtin)
{
    static_assert(FunctionTraceData::kindBitsInsideHelperOfCompiler == static_cast<uint16_t>(StackTraceBuiltinKind::JSBuiltinConstructor) && !static_cast<uint16_t>(StackTraceBuiltinKind::None));
    if (!builtin && function.syntax.contains(FunctionSyntax::IsShownInHelperModule))
        return StackTraceBuiltinKind::JSBuiltinConstructor;
    if (!builtin)
        return isTracedInsideBuiltinFunction(builtins, function) ? StackTraceBuiltinKind::JSBuiltin : StackTraceBuiltinKind::None;
    if (function.syntax.contains(FunctionSyntax::IsShownInHelperModule))
        return StackTraceBuiltinKind::None;
    if (std::ranges::any_of(builtins.overloadsOf(*builtin), overloadOfTracedBuiltinCallsHostFunction))
        return StackTraceBuiltinKind::HostFunction;
    return builtins.at(*builtin).kind == BuiltinKind::Constructor ? StackTraceBuiltinKind::JSBuiltinConstructor : StackTraceBuiltinKind::JSBuiltin;
}

static CodeType codeTypeOfTracedFunction(const Function& function)
{
    if (function.kind == FunctionKind::Module)
        return ModuleCode;
    return function.syntax.contains(FunctionSyntax::IsEvaluatedScript) ? EvalCode : FunctionCode;
}

static_assert(static_cast<unsigned>(TraceVisibility::Hidden) < 1 << 2 && static_cast<unsigned>(StackTraceBuiltinKind::HostFunction) < 1 << 2);
static_assert(static_cast<unsigned>(EvalCode) < 1 << 2 && static_cast<unsigned>(FunctionCode) < 1 << 2 && static_cast<unsigned>(ModuleCode) < 1 << 2);

FunctionTraceRecord functionTraceOf(const Program& program, const CallingConventions& conventions, CodeUnit unit, bool isEntryThatWalksBody)
{
    const Function& function = program.at(unit.function);
    const BuiltinTable& builtins = program.builtins();
    std::optional<BuiltinID> builtin = builtinImplementedByTracedFunction(program, unit.function);

    FunctionTraceRecord record { };
    record.sourceFunctionIndex = unit.function.index();
    record.implementedBuiltinPlusOne = builtin ? builtin->index() + 1 : 0;
    record.implementsBuiltin = builtin.has_value();
    record.visibilityBits = static_cast<uint16_t>(traceVisibilityOfUnit(function, unit.role, tracedBuiltinIsCalledByPrograms(builtins, function, builtin), isEntryThatWalksBody));
    record.builtinKindBits = static_cast<uint16_t>(stackTraceBuiltinKindOf(builtins, function, builtin));
    record.codeTypeBits = static_cast<uint16_t>(codeTypeOfTracedFunction(function));
    record.rejectsOnThrow = rejectsOnThrow(program, conventions, unit);
    record.makesPromiseOfCall = record.rejectsOnThrow && unit.role == CodeRole::Body;
    record.isStrict = !function.has(FunctionFlags::IsSloppy);
    record.isConstructor = function.kind == FunctionKind::BaseConstructor || function.kind == FunctionKind::DerivedConstructor;
    record.isAsync = function.has(FunctionFlags::IsAsync);
    record.isNamedByOwner = frameOfUnitIsNamedByOwner(function, unit.role);
    record.isNakedConstructor = function.has(FunctionFlags::IsNakedConstructor);
    record.isNamedByComputedKey = function.has(FunctionFlags::IsNamedByComputedKey);
    return record;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
