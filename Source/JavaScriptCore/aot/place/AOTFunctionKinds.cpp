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
#include "AOTIRProgram.h"
#include "AOTImagesBelow.h"
#include "AOTPlacementSteps.h"
#include "AOTRepresentation.h"
#include "AOTSort.h"
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

static CoroutineKind kindOfCoroutine(const Function& function)
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
    case FunctionRole::ChecksThatGiveUp:
        break;
    case FunctionRole::CoroutineRamp:
        if (kindOfCoroutine(function) != CoroutineKind::AsyncFunction)
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
    case FunctionRole::ChainStep:
    case FunctionRole::StubBody:
        return ClosureForm::Full;
    }
    if (function.hasAny({ FunctionFlags::IsGenerator, FunctionFlags::HasNewTarget, FunctionFlags::TakesCallee }))
        return ClosureForm::Full;
    if (function.captures.size() > ClosureRequest::maxSlimClosureCaptures)
        return ClosureForm::Full;
    // A full closure has the property `name` from the first look for it on, which is what a stack frame asks a function object that has a displayName.
    if (function.has(FunctionCapabilities::NotesLooksForName) && flow.closuresAreGivenDisplayName(id))
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

static DeclaredType declaredCapture(TypeID type) { return DeclaredType(Proven::byPosition(type)); }
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

static_assert(!static_cast<unsigned>(MethodKind::Method) && static_cast<unsigned>(MethodKind::Getter) == 1 && static_cast<unsigned>(MethodKind::Setter) == 2 && numberOfEnumerators<MethodKind> == methodThatNotesLooksForName);

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

static bool takesNewTarget(const Function& function)
{
    if (function.role != FunctionRole::GenericEntrypoint)
        return function.has(FunctionFlags::HasNewTarget);
    auto& record = function.creator.as<CreatorRecord::Entrypoint>();
    return record.takesNewTarget || record.constructEntrypoint;
}

// A file of library functions is no runtime module, and a pass that replaces a function in place gives it another role.
// What the bundler imports is evaluated as a file of the program is, so its functions are ordinary function objects.
bool isInRuntimeModule(const Program& program, const Function& function)
{
    if (function.module && isBundlerHelperModule(program, program.at(function.module)))
        return false;
    if (function.role == FunctionRole::Library || function.enclosingRuntimeFunction)
        return true;
    return function.module && program.at(function.module).kind == ModuleKind::RuntimeSource;
}

static FunctionKindData functionKindOf(const Program& program, const SlotPlan& slots, FunctionID id)
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
    if (described.has(FunctionCapabilities::NotesLooksForName) && kind.accessor() == MethodKind::Method)
        kind.accessorKind = methodThatNotesLooksForName;
    kind.isInRuntimeModule = isInRuntimeModule(program, described);
    kind.isSlim = slots.closureFormOf(targetID) == ClosureForm::Slim;
    kind.closureStructure = kind.closureStructureOfKind();
    return kind;
}

static ClosureFunctionRange closureFunctionRangeOf(const Program& program)
{
    uint32_t firstClosureFunction = program.functions.size();
    uint32_t endOfClosureFunctions = program.functions.size();
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (!hasClosureWordOf(program.functions[index].role))
            continue;
        firstClosureFunction = std::min(firstClosureFunction, index);
        endOfClosureFunctions = index + 1;
    }
    return { FunctionID(firstClosureFunction), FunctionID(endOfClosureFunctions), firstClosureFunction };
}

static Vector<FunctionID> closureFormsBySourceOf(const Program& program)
{
    // The word of a closure names the entrypoint for calls.
    BitVector isConstructEntrypoint(program.functions.size());
    for (const Function& function : program.functions) {
        const auto* entrypoint = function.creator.tryAs<CreatorRecord::Entrypoint>();
        if (entrypoint && entrypoint->constructEntrypoint)
            isConstructEntrypoint.quickSet(entrypoint->constructEntrypoint.index());
    }
    struct Form {
        FunctionID source;
        FunctionID function;
    };
    Vector<Form> forms;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (!hasClosureWordOf(program.functions[index].role) || isConstructEntrypoint.quickGet(index))
            continue;
        FunctionID source = firstSourceOf(program, FunctionID(index));
        if (program.at(source).has(FunctionCapabilities::RecordsCalleeOnceRenamed))
            forms.append({ source, FunctionID(index) });
    }
    AOT::stableSort(forms, { }, [](const Form& form) { return form.source.index(); });
    Vector<FunctionID> result;
    for (size_t index = 0; index < forms.size(); ++index) {
        bool isOneOfSeveral = (index && forms[index - 1].source == forms[index].source) || (index + 1 < forms.size() && forms[index + 1].source == forms[index].source);
        if (isOneOfSeveral)
            result.append(forms[index].function);
    }
    return result;
}

static bool hasClosureCell(const Program& program, const BitVector& layoutConstructors, const Function& function)
{
    if (function.role != FunctionRole::GenericEntrypoint || function.derivation.parameter != callEntrypointParameter || !function.hasBody())
        return false;
    FunctionID target = function.creator.as<CreatorRecord::Entrypoint>().target;
    return program.at(target).captures.isEmpty() || layoutConstructors.quickGet(target.index());
}

// Source order keeps the cells that a module fills at startup on the same pages. Rarely filled cells go last.
static Vector<uint32_t> numberClosureCells(const Program& program, const ParallelLoops& loops, const BitVector& declaredByVariables)
{
    BitVector layoutConstructors(program.functions.size());
    BitVector runtimeCreated = declaredByVariables;
    auto addRuntimeCreated = [&](FunctionID function) {
        if (function)
            runtimeCreated.quickSet(function.index());
    };
    for (const Layout& layout : program.layouts) {
        if (layout.constructor)
            layoutConstructors.quickSet(layout.constructor.index());
        addRuntimeCreated(layout.constructor);
        for (const ClassMember& member : layout.classFlow.members)
            addRuntimeCreated(member.function);
    }
    for (const RuntimeFunctionEntry& entry : program.runtimeFunctions)
        addRuntimeCreated(entry.function);
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

FunctionNumbering numberFunctions(const PlacementContext& context, const PlacedSites& sites)
{
    const Program& program = context.program;
    const ParallelLoops& loops = context.build.parallelLoops();
    unsigned numberOfFunctions = program.functions.size();
    BitVector declaredByVariables(numberOfFunctions);
    for (const Variable& variable : program.variables) {
        if (variable.initialization == VariableInitialization::Function && variable.function)
            declaredByVariables.quickSet(variable.function.index());
    }
    FunctionNumbering numbering;
    numbering.kinds.grow(numberOfFunctions);
    numbering.constructedClasses.grow(numberOfFunctions);
    numbering.closureIdentities.grow(numberOfFunctions);
    forEachIndexInParallel(loops, numberOfFunctions, [&](unsigned index) {
        FunctionID id(index);
        numbering.kinds[index] = functionKindOf(program, context.slots(), id);
        numbering.constructedClasses[index] = program.at(id).constructedClass;
        bool isCreatedByTable = declaredByVariables.quickGet(index) || hasSingletonClosure(program, id);
        bool isOnlyTableCell = isCreatedByTable && program.at(id).captures.isEmpty() && !sites.functionsWithAllocatedClosures.get(index);
        numbering.closureIdentities[index] = isOnlyTableCell ? ClosureIdentity::TableCell : ClosureIdentity::Unknown;
    });
    numbering.closureCellNumbers = numberClosureCells(program, loops, declaredByVariables);
    numbering.closureFunctions = closureFunctionRangeOf(program);
    if (const ImagesBelow* below = context.build.imagesBelow())
        numbering.closureFunctions.numbersBelow = below->numbersOfLast().endOfClosureNumbers;
    numbering.closureFormsBySource = closureFormsBySourceOf(program);
    return numbering;
}

static TraceVisibility visibilityOf(const Function& function, bool implementsBuiltin)
{
    switch (function.role) {
    case FunctionRole::OutlinedBody:
    case FunctionRole::Serializer:
    case FunctionRole::Parser:
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
    case FunctionRole::ChecksThatGiveUp:
        // What the front end hides is a function of its own that no trace lists, unless it is a built-in that programs can call. What its role hides is a part of a function that traces list.
        if (function.has(FunctionFlags::IsHiddenInStackTraces) && !implementsBuiltin)
            return TraceVisibility::Hidden;
        // The function that holds another function's entry tests is shown as the function whose id it took.
        if (capabilitiesOf(function.role).contains(FunctionCapabilities::IsHiddenInStackTraces))
            return TraceVisibility::VisibleOnlyOnTop;
        return function.isHiddenInStackTraces() ? TraceVisibility::VisibleOnlyOnTop : TraceVisibility::Visible;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool rejectsOnThrow(const Program& program, const Function& function)
{
    if (function.role == FunctionRole::CoroutineBody)
        return kindOfCoroutine(function) == CoroutineKind::AsyncFunction;
    if (function.role != FunctionRole::GenericEntrypoint)
        return false;
    const Function& target = program.at(function.creator.as<CreatorRecord::Entrypoint>().target);
    return target.role == FunctionRole::CoroutineRamp && kindOfCoroutine(target) == CoroutineKind::AsyncFunction;
}

// StackFrame gives an async frame of such a function no name. The frame of an async function keeps the name the function itself has, not one it takes from a variable or a property.
static bool isNamedByOwner(const Function& function)
{
    bool hasExplicitName = function.has(FunctionFlags::HasExplicitName);
    // The engine goes by the text. An `await` is written in the function, and the build has found that the function never reaches it.
    if (function.role == FunctionRole::AsyncWorker)
        return !hasExplicitName && function.syntax.contains(FunctionSyntax::HasWrittenAwait);
    if (function.role != FunctionRole::CoroutineBody)
        return false;
    switch (kindOfCoroutine(function)) {
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

// In the engine a function written inside a built-in function is built-in code too. A module of the library marks the function that it holds in place of such a one.
static bool isInsideBuiltinFunction(const BuiltinTable& builtins, const Function& source)
{
    if (source.syntax.contains(FunctionSyntax::IsShownAsNativeCode))
        return true;
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

static bool isCompilerFunction(const BuiltinTable& builtins, const Function& source)
{
    return source.enclosingRuntimeFunction && builtins.at(source.enclosingRuntimeFunction).role == RuntimeFunctionRole::CompilerFunction;
}

// Only compiled code calls a compiler helper, even one that is implemented in the engine.
static bool isCalledByPrograms(const BuiltinTable& builtins, const Function& source, std::optional<BuiltinID> builtin)
{
    return builtin && !isCompilerFunction(builtins, source) && builtins.at(*builtin).holder != Builtins::CompilerHelpers;
}

static StackTraceBuiltinKind builtinKindOf(const BuiltinTable& builtins, const Function& source, std::optional<BuiltinID> builtin)
{
    static_assert(FunctionTraceData::kindBitsInsideHelperOfCompiler == static_cast<uint16_t>(StackTraceBuiltinKind::JSBuiltinConstructor) && !static_cast<uint16_t>(StackTraceBuiltinKind::None));
    if (!builtin && source.syntax.contains(FunctionSyntax::IsShownInModuleOfHelpers))
        return StackTraceBuiltinKind::JSBuiltinConstructor;
    if (!builtin)
        return isInsideBuiltinFunction(builtins, source) ? StackTraceBuiltinKind::JSBuiltin : StackTraceBuiltinKind::None;
    // The host has what it does as a function of a module of its own, and a stack trace lists that like any other function: FunctionDescription::isHelperOfCompiler().
    if (source.syntax.contains(FunctionSyntax::IsShownInModuleOfHelpers))
        return StackTraceBuiltinKind::None;
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
    record.visibilityBits = static_cast<uint32_t>(visibilityOf(function, isCalledByPrograms(program.builtins(), source, builtin)));
    record.builtinKindBits = static_cast<uint32_t>(builtinKindOf(program.builtins(), source, builtin));
    record.codeTypeBits = static_cast<uint32_t>(source.kind == FunctionKind::Module ? ModuleCode : source.syntax.contains(FunctionSyntax::IsEvaluatedScript) ? EvalCode : FunctionCode);
    record.rejectsOnThrow = rejectsOnThrow(program, function);
    record.isStrict = !source.has(FunctionFlags::IsSloppy);
    record.isConstructor = source.kind == FunctionKind::BaseConstructor || source.kind == FunctionKind::DerivedConstructor;
    record.isAsync = source.has(FunctionFlags::IsAsync);
    record.isNamedByOwner = isNamedByOwner(function);
    record.isNakedConstructor = source.has(FunctionFlags::IsNakedConstructor);
    record.isNamedByComputedKey = source.has(FunctionFlags::IsNamedByComputedKey);
    record.recordsCalleeOnceRenamed = source.has(FunctionCapabilities::RecordsCalleeOnceRenamed);
    record.recordsCallee = source.has(FunctionCapabilities::RecordsCallee) && !record.recordsCalleeOnceRenamed;
    return record;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
