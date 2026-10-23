/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFunctionKinds.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTChooseCallingConventions.h"
#include "AOTConvention.h"
#include "AOTCreateEntrypoints.h"
#include "AOTFlowResults.h"
#include "AOTFunctionDescription.h"
#include "AOTRepresentation.h"
#include "AOTSlotPlanSteps.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <compare>
#include <functional>
#include <optional>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ClosureCellSortKey {
    friend auto operator<=>(const ClosureCellSortKey&, const ClosureCellSortKey&) = default;

    bool isRarelyFilled;
    uint32_t moduleIndex;
    uint32_t sourceFunctionIndex;
    uint32_t functionIndex;
};

} // anonymous namespace

static CoroutineKind coroutineKindOf(const Function& function)
{
    return function.creator.as<CreatorRecord::Coroutine>().kind;
}

static ClosureUse closureUseOf(const Function& function, const FlowResults& flow, FunctionID id)
{
    if (function.role != FunctionRole::BoundFunction)
        return flow.function(id).closureUse;
    auto& record = function.creator.as<CreatorRecord::BoundFunction>();
    return flow.closureUseOfBoundFunction(record.target, record.numberOfBoundArguments);
}

// A closure of either form is a complete function object, whoever gets hold of it.
static bool isExpectedToHaveNoProperties(const Program& program, const FlowResults& flow, FunctionID id)
{
    auto& function = program.at(id);
    if (closureUseOf(function, flow, id) == ClosureUse::CalledOnly)
        return true;
    if (isInRuntimeModule(program, function))
        return false;
    return function.kind != FunctionKind::Plain || function.has(FunctionFlags::IsAsync) || !function.constructedClass;
}

ClosureForm closureFormOf(const Program& program, const FlowResults& flow, FunctionID id)
{
    auto& function = program.at(id);
    switch (function.kind) {
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
        break;
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return ClosureForm::Full;
    }
    switch (function.role) {
    case FunctionRole::Source:
    case FunctionRole::Library:
    case FunctionRole::BoundFunction:
    case FunctionRole::ResultWrapper:
    case FunctionRole::ParameterWrapper:
    case FunctionRole::EntryChecks:
        break;
    case FunctionRole::CoroutineRamp:
        if (coroutineKindOf(function) != CoroutineKind::AsyncFunction)
            return ClosureForm::Full;
        break;
    case FunctionRole::Model:
    case FunctionRole::Signature:
    case FunctionRole::GenericEntrypoint:
    case FunctionRole::TableEntrypoint:
    case FunctionRole::TableLookup:
    case FunctionRole::GenericVersion:
    case FunctionRole::AllocatedThisVersion:
    case FunctionRole::ConstructBody:
    case FunctionRole::CallSiteVersion:
    case FunctionRole::CalleeVersion:
    case FunctionRole::CoroutineBody:
    case FunctionRole::AsyncWorker:
    case FunctionRole::ResultWorker:
    case FunctionRole::OutlinedBody:
    case FunctionRole::Serializer:
    case FunctionRole::Parser:
    case FunctionRole::ValidatingParser:
    case FunctionRole::Validator:
    case FunctionRole::ChainStep:
    case FunctionRole::StubBody:
        return ClosureForm::Full;
    }
    if (function.hasAny({ FunctionFlags::IsGenerator, FunctionFlags::HasNewTarget, FunctionFlags::TakesCallee }))
        return ClosureForm::Full;
    if (function.captures.size() > ClosureRequest::maxSlimClosureCaptures)
        return ClosureForm::Full;
    return isExpectedToHaveNoProperties(program, flow, id) ? ClosureForm::Slim : ClosureForm::Full;
}

Vector<ClosureForm> chooseClosureForms(const SlotPlanContext& context, const SlotUses& uses)
{
    const Program& program = context.program;
    Vector<ClosureForm> forms(FillWith { }, program.functions.size(), ClosureForm::Full);
    forEachIndexInParallel(context.build.parallelLoops(), program.functions.size(), [&](unsigned index) {
        bool isExpectedToGetProperties = uses.functionsWhoseNewClosuresAreStoredInto.get(index) && program.functions[index].role != FunctionRole::BoundFunction;
        if (!isExpectedToGetProperties)
            forms[index] = closureFormOf(program, context.inputs.flow, FunctionID(index));
    });

    uint64_t numberOfSlimFunctions = 0;
    uint64_t numberOfFullFunctions = 0;
    for (size_t index = 0; index < forms.size(); ++index) {
        const Function& function = program.functions[index];
        if (!canBeCalledByValueOf(function.role) || function.hasOnlyDirectCallers())
            continue;
        if (forms[index] == ClosureForm::Slim)
            ++numberOfSlimFunctions;
        else
            ++numberOfFullFunctions;
    }
    context.build.statistics().count(StatisticID::SlimClosureFunctions, numberOfSlimFunctions);
    context.build.statistics().count(StatisticID::FullClosureFunctions, numberOfFullFunctions);
    return forms;
}

static DeclaredType declaredCapture(TypeID type) { return { .type = type }; }
static const DeclaredType& declaredCapture(const DeclaredType& declared) { return declared; }

template<typename Capture>
static bool isReference(const Program& program, const Capture& capture)
{
    return holdsCells(declaredRepresentationOf(program, declaredCapture(capture)));
}

template<typename Capture>
static unsigned referenceCountAmong(const Program& program, std::span<const Capture> captures)
{
    unsigned count = 0;
    for (const Capture& capture : captures)
        count += isReference(program, capture);
    return count;
}

template<typename Capture>
static unsigned captureSlotAmong(const Program& program, std::span<const Capture> captures, unsigned capture)
{
    if (isReference(program, captures[capture]))
        return referenceCountAmong(program, captures.first(capture));
    return referenceCountAmong(program, captures) + capture - referenceCountAmong(program, captures.first(capture));
}

ClosureCaptureCounts closureCaptureCountsOf(const Program& program, const Function& function)
{
    RELEASE_ASSERT(function.role != FunctionRole::GenericEntrypoint);
    return { referenceCountAmong(program, function.captures.span()), static_cast<unsigned>(function.captures.size()) };
}

unsigned closureCaptureSlotOf(const Program& program, std::span<const TypeID> captureTypes, unsigned capture)
{
    return captureSlotAmong(program, captureTypes, capture);
}

unsigned closureCaptureSlotOf(const Program& program, const Function& function, unsigned capture)
{
    return captureSlotAmong(program, function.captures.span(), capture);
}

FunctionID closureTargetOf(const Program& program, FunctionID id)
{
    auto& function = program.at(id);
    return function.role == FunctionRole::GenericEntrypoint ? function.creator.as<CreatorRecord::Entrypoint>().target : id;
}

FunctionID firstSourceOf(const Program& program, FunctionID id)
{
    while (FunctionID next = program.at(id).derivation.source)
        id = next;
    return id;
}

// A bound function is derived from its target, but it is a function object of its own.
static const Function& describedFunctionOf(const Program& program, FunctionID id)
{
    while (true) {
        auto& function = program.at(id);
        FunctionID next = function.role == FunctionRole::BoundFunction ? FunctionID() : function.derivation.source;
        if (!next)
            return function;
        id = next;
    }
}

static_assert(static_cast<unsigned>(MethodKind::Getter) == 1 && static_cast<unsigned>(MethodKind::Setter) == 2);

static FunctionKindData kindDataOf(FunctionKind functionKind)
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

static unsigned closureStructureOf(FunctionKindData kind)
{
    if (kind.isConstructor)
        return classObjectClosureStructure;
    return kind.isAsync | kind.isGenerator << 1;
}

static bool takesNewTarget(const Function& function)
{
    if (function.role != FunctionRole::GenericEntrypoint)
        return function.has(FunctionFlags::HasNewTarget);
    auto& record = function.creator.as<CreatorRecord::Entrypoint>();
    return record.takesNewTarget || record.constructEntrypoint;
}

// A file of library functions is no runtime module, and a pass that replaces a function in place gives it another role.
bool isInRuntimeModule(const Program& program, const Function& function)
{
    if (function.role == FunctionRole::Library || function.enclosingRuntimeFunction)
        return true;
    return function.module && program.at(function.module).kind == ModuleKind::RuntimeSource;
}

FunctionKindData functionKindOf(const Program& program, const SlotPlan& slots, FunctionID id)
{
    FunctionID targetID = closureTargetOf(program, id);
    auto& described = describedFunctionOf(program, targetID);
    FunctionKindData kind = kindDataOf(described.kind);
    if (described.role == FunctionRole::BoundFunction)
        kind.callableKind = static_cast<unsigned>(CallableKind::Bound);
    kind.isAsync = described.has(FunctionFlags::IsAsync);
    kind.isGenerator = described.has(FunctionFlags::IsGenerator);
    kind.takesNewTarget = takesNewTarget(program.at(id));
    kind.hasInferredName = !described.has(FunctionFlags::HasExplicitName);
    kind.isInRuntimeModule = isInRuntimeModule(program, described);
    kind.isSlim = slots.closureFormOf(targetID) == ClosureForm::Slim;
    kind.closureStructure = closureStructureOf(kind);
    return kind;
}

static bool hasClosureWord(FunctionRole role)
{
    switch (role) {
    case FunctionRole::GenericEntrypoint:
        return true;
    case FunctionRole::Source:
    case FunctionRole::Library:
    case FunctionRole::Model:
    case FunctionRole::Signature:
    case FunctionRole::TableEntrypoint:
    case FunctionRole::TableLookup:
    case FunctionRole::GenericVersion:
    case FunctionRole::AllocatedThisVersion:
    case FunctionRole::ConstructBody:
    case FunctionRole::CallSiteVersion:
    case FunctionRole::CalleeVersion:
    case FunctionRole::CoroutineRamp:
    case FunctionRole::CoroutineBody:
    case FunctionRole::AsyncWorker:
    case FunctionRole::BoundFunction:
    case FunctionRole::ResultWrapper:
    case FunctionRole::ResultWorker:
    case FunctionRole::ParameterWrapper:
    case FunctionRole::EntryChecks:
    case FunctionRole::OutlinedBody:
    case FunctionRole::Serializer:
    case FunctionRole::Parser:
    case FunctionRole::ValidatingParser:
    case FunctionRole::Validator:
    case FunctionRole::ChainStep:
    case FunctionRole::StubBody:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ClosureFunctionRange closureFunctionRangeOf(const Program& program)
{
    uint32_t firstClosureFunction = program.functions.size();
    uint32_t endOfClosureFunctions = program.functions.size();
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (!hasClosureWord(program.functions[index].role))
            continue;
        firstClosureFunction = std::min(firstClosureFunction, index);
        endOfClosureFunctions = index + 1;
    }
    return { FunctionID(firstClosureFunction), FunctionID(endOfClosureFunctions), firstClosureFunction };
}

static BitVector collectLayoutConstructors(const Program& program)
{
    BitVector constructors(program.functions.size());
    for (const Layout& layout : program.layouts) {
        if (layout.constructor)
            constructors.quickSet(layout.constructor.index());
    }
    return constructors;
}

static BitVector collectFunctionsWithRuntimeCreatedClosures(const Program& program, const BitVector& layoutConstructors)
{
    BitVector functions = layoutConstructors;
    auto add = [&](FunctionID function) {
        if (function)
            functions.quickSet(function.index());
    };
    for (const Variable& variable : program.variables) {
        if (variable.initialization == VariableInitialization::Function)
            add(variable.function);
    }
    for (const Layout& layout : program.layouts) {
        for (const ClassMember& member : layout.classFlow.members)
            add(member.function);
    }
    for (const RuntimeFunctionEntry& entry : program.runtimeFunctions)
        add(entry.function);
    return functions;
}

static bool hasClosureCell(const Program& program, const BitVector& layoutConstructors, const Function& function)
{
    if (function.role != FunctionRole::GenericEntrypoint || function.derivation.parameter != callEntrypointParameter || !function.hasBody())
        return false;
    FunctionID target = function.creator.as<CreatorRecord::Entrypoint>().target;
    return program.at(target).captures.isEmpty() || layoutConstructors.quickGet(target.index());
}

// Source order keeps the cells that a module fills at startup on the same pages. Rarely filled cells go last.
Vector<uint32_t> numberClosureCells(const Program& program, const ParallelLoops& loops)
{
    BitVector layoutConstructors = collectLayoutConstructors(program);
    BitVector runtimeCreated = collectFunctionsWithRuntimeCreatedClosures(program, layoutConstructors);
    auto keys = collectInParallel<ClosureCellSortKey>(loops, program.functions.size(), [&](unsigned index, Vector<ClosureCellSortKey>& found) {
        if (!hasClosureCell(program, layoutConstructors, program.functions[index]))
            return;
        FunctionID target = closureTargetOf(program, FunctionID(index));
        FunctionID source = firstSourceOf(program, target);
        ModuleID module = program.at(source).module;
        found.append({ !runtimeCreated.quickGet(target.index()), module ? module.index() : UINT32_MAX, source.index(), index });
    });
    parallelSort(loops, keys.mutableSpan(), std::less<ClosureCellSortKey> { });

    Vector<uint32_t> numbers(FillWith { }, program.functions.size(), 0u);
    for (uint32_t index = 0; index < keys.size(); ++index)
        numbers[keys[index].functionIndex] = index + 1;
    return numbers;
}

static TraceVisibility visibilityOf(const Function& function)
{
    switch (function.role) {
    case FunctionRole::OutlinedBody:
    case FunctionRole::Serializer:
    case FunctionRole::Parser:
    case FunctionRole::ValidatingParser:
    case FunctionRole::Validator:
    case FunctionRole::ChainStep:
    case FunctionRole::StubBody:
        return TraceVisibility::Hidden;
    case FunctionRole::Source:
    case FunctionRole::Library:
    case FunctionRole::Model:
    case FunctionRole::Signature:
    case FunctionRole::GenericEntrypoint:
    case FunctionRole::TableEntrypoint:
    case FunctionRole::TableLookup:
    case FunctionRole::GenericVersion:
    case FunctionRole::AllocatedThisVersion:
    case FunctionRole::ConstructBody:
    case FunctionRole::CallSiteVersion:
    case FunctionRole::CalleeVersion:
    case FunctionRole::CoroutineRamp:
    case FunctionRole::CoroutineBody:
    case FunctionRole::AsyncWorker:
    case FunctionRole::BoundFunction:
    case FunctionRole::ResultWrapper:
    case FunctionRole::ResultWorker:
    case FunctionRole::ParameterWrapper:
    case FunctionRole::EntryChecks:
        return function.isHiddenInStackTraces() ? TraceVisibility::VisibleOnlyOnTop : TraceVisibility::Visible;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool rejectsOnThrow(const Program& program, const Function& function)
{
    if (function.role == FunctionRole::CoroutineBody)
        return coroutineKindOf(function) == CoroutineKind::AsyncFunction;
    if (function.role != FunctionRole::GenericEntrypoint)
        return false;
    const Function& target = program.at(function.creator.as<CreatorRecord::Entrypoint>().target);
    return target.role == FunctionRole::CoroutineRamp && coroutineKindOf(target) == CoroutineKind::AsyncFunction;
}

// StackFrame gives an async frame of such a function no name. The frame of an async function keeps the name the function itself has, not one it takes from a variable or a property.
static bool isNamedByOwner(const Function& function)
{
    if (function.role != FunctionRole::CoroutineBody && function.role != FunctionRole::AsyncWorker)
        return false;
    bool hasExplicitName = function.has(FunctionFlags::HasExplicitName);
    switch (coroutineKindOf(function)) {
    case CoroutineKind::AsyncFunction:
        return !hasExplicitName;
    case CoroutineKind::Generator:
    case CoroutineKind::AsyncGenerator:
        return hasExplicitName;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<BuiltinID> builtinImplementedBy(const Program& program, FunctionID sourceID)
{
    const Function& source = program.at(sourceID);
    if (auto* stubBody = source.creator.tryAs<CreatorRecord::StubBody>())
        return stubBody->builtin;
    if (!source.enclosingRuntimeFunction)
        return std::nullopt;
    const RuntimeFunctionEntry* entry = findRuntimeFunction(program, source.enclosingRuntimeFunction);
    if (!entry || entry->function != sourceID)
        return std::nullopt;
    std::span<const BuiltinID> implemented = program.builtins().at(source.enclosingRuntimeFunction).implements;
    if (implemented.empty())
        return std::nullopt;
    return implemented.front();
}

static bool callsHostFunction(const BuiltinOverload& overload)
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

// In the engine a function written inside a built-in function is built-in code too.
static bool isInsideBuiltinFunction(const BuiltinTable& builtins, const Function& source)
{
    if (!source.enclosingRuntimeFunction)
        return false;
    switch (builtins.at(source.enclosingRuntimeFunction).role) {
    case RuntimeFunctionRole::EngineBuiltin:
    case RuntimeFunctionRole::HostBuiltin:
        return true;
    case RuntimeFunctionRole::CompilerFunction:
    case RuntimeFunctionRole::Model:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static StackTraceBuiltinKind builtinKindOf(const BuiltinTable& builtins, const Function& source, std::optional<BuiltinID> builtin)
{
    if (!builtin)
        return isInsideBuiltinFunction(builtins, source) ? StackTraceBuiltinKind::JSBuiltin : StackTraceBuiltinKind::None;
    if (std::ranges::any_of(builtins.overloadsOf(*builtin), callsHostFunction))
        return StackTraceBuiltinKind::HostFunction;
    return builtins.at(*builtin).kind == BuiltinKind::Constructor ? StackTraceBuiltinKind::JSBuiltinConstructor : StackTraceBuiltinKind::JSBuiltin;
}

FunctionTraceRecord functionTraceOf(const Program& program, FunctionID id)
{
    const Function& function = program.at(id);
    FunctionID sourceID = firstSourceOf(program, id);
    const Function& source = program.at(sourceID);
    std::optional<BuiltinID> builtin = builtinImplementedBy(program, sourceID);
    FunctionTraceRecord record { };
    record.sourceFunctionIndex = sourceID.index();
    record.implementedBuiltinPlusOne = builtin ? builtin->index() + 1 : 0;
    record.implementsBuiltin = builtin.has_value();
    record.visibilityBits = static_cast<uint32_t>(visibilityOf(function));
    record.builtinKindBits = static_cast<uint32_t>(builtinKindOf(program.builtins(), source, builtin));
    record.codeTypeBits = static_cast<uint32_t>(source.kind == FunctionKind::Module ? ModuleCode : FunctionCode);
    record.rejectsOnThrow = rejectsOnThrow(program, function);
    record.isStrict = !source.has(FunctionFlags::IsSloppy);
    record.isConstructor = source.kind == FunctionKind::BaseConstructor || source.kind == FunctionKind::DerivedConstructor;
    record.isAsync = source.has(FunctionFlags::IsAsync);
    record.isNamedByOwner = isNamedByOwner(function);
    record.isNakedConstructor = source.has(FunctionFlags::IsNakedConstructor);
    record.isNamedBySymbol = source.has(FunctionFlags::IsNamedBySymbol);
    return record;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
