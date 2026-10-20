/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowCallState.h"

#if ENABLE(AOT)

#include "AOTFlowPropertyState.h"
#include <algorithm>
#include <span>
#include <wtf/ScopedLambda.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class ExportLookup : uint8_t {
    Unknown,
    Exposed,
};

} // anonymous namespace

// A lookup that took the member for a constant waits on the name under the holder, and a call of the member's setter writes it without creating a location there.
static void revisitLookupsOfWrittenMember(FlowSolver& solver, BuiltinID member)
{
    const Builtin& entry = solver.builtins().at(member);
    if (!entry.holder || !entry.name || !solver.propertyState().membersTakenForConstants.get(member.index()))
        return;
    if (FlowObjectID holder = solver.objects().findBuiltin(entry.holder))
        solver.revisitPropertyWaiters(holder, solver.program().keyOf(entry.name));
}

// Loading a module without source runs the native code that creates its namespace object, and the load is recorded as a read of that object.
static bool isNamespaceCreatedByNativeCode(const BuiltinTable& builtins, BuiltinID builtin)
{
    for (uint32_t index = 0; index < builtins.moduleCount(); ++index) {
        const RuntimeModuleData& module = builtins.at(RuntimeModuleID(index));
        if (module.namespaceObject == builtin && !module.flags.contains(RuntimeModuleFlag::HasSource))
            return true;
    }
    return false;
}

OptionSet<BuiltinUse> usesEnteringCode(const BuiltinTable& builtins, BuiltinID builtin)
{
    constexpr OptionSet<BuiltinUse> callingUses { BuiltinUse::Called, BuiltinUse::Constructed, BuiltinUse::Escaped };
    constexpr OptionSet<BuiltinUse> accessingUses { BuiltinUse::Read, BuiltinUse::Written };

    switch (builtins.at(builtin).kind) {
    // Native code of the feature creates the value of a data property on the first read, and what the property holds is called as an unknown function.
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
        return callingUses | accessingUses;
    case BuiltinKind::Namespace:
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Prototype:
    case BuiltinKind::Method:
    case BuiltinKind::Constant:
    case BuiltinKind::NativeBinding:
        return isNamespaceCreatedByNativeCode(builtins, builtin) ? callingUses | accessingUses : callingUses;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Native code of the host makes instances of classes on its own, and hands them out wherever the table says no more than "anything": as an answer, to a callback, in a property.
// An entry names every feature that its native code enters, those of the classes it instantiates among them. So a class that trimming can remove may have such an instance once each feature of its prototype is named
// by an entry in use: native code holds the prototype from then on, and the unknown object may be an instance.
static void enterFeature(FlowSolver& solver, RemovableFeature feature, const Witness& witness)
{
    BitVector& enteredFeatures = solver.callState().builtins.featuresEnteredByHost;
    if (enteredFeatures.set(static_cast<size_t>(feature)))
        return;

    const BuiltinTable& builtins = solver.builtins();
    for (uint32_t index = 0; index < builtins.classCount(); ++index) {
        BuiltinID prototype = builtins.at(BuiltinClassID(index)).prototype;
        if (!prototype)
            continue;
        prototype = builtins.canonicalID(prototype);
        std::span<const BuiltinFeatureUse> lines = builtins.featureUsesOf(prototype);
        bool isCompleted = std::ranges::contains(lines, feature, &BuiltinFeatureUse::feature) && std::ranges::all_of(lines, [&](const BuiltinFeatureUse& line) {
            return enteredFeatures.get(static_cast<size_t>(line.feature));
        });
        if (isCompleted)
            solver.joinObject(solver.graph().nativeCode(), ensureBuiltinObject(solver, prototype), witness);
    }
}

static void enterFeaturesOf(FlowSolver& solver, BuiltinID builtin, const Witness& witness, NOESCAPE const ScopedLambda<bool(const BuiltinFeatureUse&)>& isEntered)
{
    const BuiltinTable& builtins = solver.builtins();
    // The engine's own native code answers what its entries say.
    if (builtins.at(builtin).origin != BuiltinOrigin::Host)
        return;
    for (const BuiltinFeatureUse& line : builtins.featureUsesOf(builtin)) {
        if (isEntered(line))
            enterFeature(solver, line.feature, witness);
    }
}

void noteBuiltinUse(FlowSolver& solver, BuiltinID builtin, OptionSet<BuiltinUse> uses, const Witness& witness)
{
    // A test, a read and a write are about one location. A call and an escape are about the object, whichever location it was read from.
    constexpr OptionSet<BuiltinUse> usesOfLocation { BuiltinUse::Tested, BuiltinUse::Read, BuiltinUse::Written };

    // Start-up fixes each member that compiled code takes as a constant, so only a store of seen code writes one: what native code's own statements stand for is refused.
    if (uses.contains(BuiltinUse::Written) && !hasFunction(witness) && !solver.records().mayRunUnknownJavaScript()) {
        uses.remove(BuiltinUse::Written);
        solver.callState().unseenCode.membersWithRefusedWrites.set(builtin.index());
    }

    // A lookup through the unknown object leaves a member that has escaped to native code, which reads what it holds. Whether a site found the member before it escaped says nothing.
    if (uses.contains(BuiltinUse::Escaped))
        uses.add(BuiltinUse::Read);

    const BuiltinTable& builtins = solver.builtins();
    BuiltinID object = builtins.canonicalID(builtin);
    OptionSet<BuiltinUse> addedToLocation = solver.records().addBuiltinUse(builtin, uses & usesOfLocation, witness);
    OptionSet<BuiltinUse> addedToObject = solver.records().addBuiltinUse(object, uses - usesOfLocation, witness);
    OptionSet<BuiltinUse> added = addedToLocation | addedToObject;
    if (added.isEmpty())
        return;

    if (addedToLocation.containsAny(usesEnteringCode(builtins, builtin)) || addedToObject.containsAny(usesEnteringCode(builtins, object))) {
        enterFeaturesOf(solver, object, witness, [](const BuiltinFeatureUse& line) {
            return !line.overload && !line.condition;
        });
    }
    if (added.contains(BuiltinUse::Written))
        revisitLookupsOfWrittenMember(solver, builtin);
    if (!added.containsOnly(BuiltinUse::Tested) && !solver.callState().builtins.builtinsWithEnteredAttachments.set(object.index()))
        enterAttachmentsOf(solver, object, witness);
    if (added.contains(BuiltinUse::Escaped))
        enterAttachmentsOfEscapedBuiltin(solver, object, witness);
    didUseLoaderLocation(solver, builtin, added, witness);
}

bool letsInUnseenCode(const BuiltinTable& builtins, BuiltinID builtin)
{
    return builtins.letsInUnseenCode(builtin);
}

bool standsForWhatItHolds(const BuiltinTable& builtins, BuiltinID builtin)
{
    return builtins.at(builtin).kind == BuiltinKind::NativeBinding || builtins.letsInUnseenCode(builtin);
}

static void enterCalledFunction(FlowSolver& solver, const BuiltinCalledFunction& called, const Witness& witness)
{
    const RuntimeFunctionData& data = solver.builtins().at(called.function);
    // A pass that expands the built-in calls a compiler function, and expands nothing in a program without it.
    bool isCalledByPass = data.role == RuntimeFunctionRole::CompilerFunction;
    // The front end sends such a function only where it compiles for one of its platforms.
    bool isOnSomePlatformsOnly = data.platforms != everyBuiltinPlatform;
    if ((isCalledByPass || isOnSomePlatformsOnly) && !findRuntimeFunction(solver.program(), called.function))
        return;
    if (FunctionID function = functionOf(solver, called.function, called.builtin, witness))
        enterFromNativeCode(solver, function, FlowObjectID(), witness);
}

void enterAttachmentsOf(FlowSolver& solver, BuiltinID builtin, const Witness& witness)
{
    const BuiltinTable& builtins = solver.builtins();
    for (const BuiltinCalledFunction& called : builtins.calledFunctionsOf(builtin)) {
        if (!called.condition)
            enterCalledFunction(solver, called, witness);
    }
    for (const BuiltinModuleLoad& load : builtins.moduleLoadsOf(builtin)) {
        if (!load.condition)
            loadRuntimeModule(solver, load, witness);
    }
}

static void enterAttachmentsOnce(FlowSolver& solver, BuiltinID builtin, const Witness& witness)
{
    if (!builtin)
        return;
    BuiltinID object = solver.builtins().canonicalID(builtin);
    if (!solver.callState().builtins.builtinsWithEnteredAttachments.set(object.index()))
        enterAttachmentsOf(solver, object, witness);
}

void noteInstanceOf(FlowSolver& solver, BuiltinClassID builtinClass, const Witness& witness)
{
    const BuiltinTable& builtins = solver.builtins();
    for (BuiltinClassID current = builtinClass; current; current = builtins.at(current).parent) {
        enterAttachmentsOnce(solver, builtins.at(current).constructor, witness);
        enterAttachmentsOnce(solver, builtins.at(current).prototype, witness);
    }
}

void letUndeclaredNativeCodeRun(FlowSolver& solver, const Witness& witness)
{
    enterAttachmentsOnce(solver, Builtins::UndeclaredNativeCode, witness);
}

// A function of the library that only native code leads to runs for a built-in in use, and the table lists what native code calls for that one.
void didCallUndeclaredNativeCode(FlowSolver& solver, FlowStatementSite site, const Witness& witness)
{
    if (isInSeenCode(solver, site))
        letUndeclaredNativeCodeRun(solver, witness);
}

static bool satisfies(const Program& program, StringID string, const BuiltinCondition& condition)
{
    bool isListed = std::ranges::any_of(condition.isOneOf, [&](BuiltinName name) {
        return program.stringOf(name) == string;
    });
    if (isListed)
        return true;

    std::span<const uint8_t> text = program.string(string);
    return std::ranges::any_of(condition.startsWith, [&](BuiltinName prefix) {
        return spanHasPrefix(text, program.builtins().textOf(prefix));
    });
}

bool conditionMayHold(const Program& program, const FlowValueTable& values, const FlowValue& operand, const BuiltinCondition& condition)
{
    constexpr uint16_t primitivesWithKnownText = Primitive::LiteralString | Primitive::UndefinedOrNull | Primitive::Absent;

    if (operand.mayBeObject() || (operand.primitives & ~primitivesWithKnownText))
        return true;
    return std::ranges::any_of(values.stringsOf(operand.strings), [&](StringID string) {
        return satisfies(program, string, condition);
    });
}

bool conditionMayHold(FlowSolver& solver, const FlowBuiltinCall& call, const BuiltinCondition* condition)
{
    if (!condition)
        return true;
    if (condition->buildAdmitsRuntimeCode)
        return solver.build().options().admitsRuntimeCode();
    // Each overload that takes the call answers for itself.
    if (condition->callRunsOtherCode)
        return false;
    if (!mayBePassed(call, condition->operand))
        return false;
    FlowNodeID node = operandNodeOf(call, condition->operand);
    if (!node)
        return false;

    solver.waitOn(node, call.request.site);
    return conditionMayHold(solver.program(), solver.values(), solver.valueOf(node), *condition);
}

void applyCallAttachments(FlowSolver& solver, const FlowBuiltinCall& call)
{
    const BuiltinTable& builtins = solver.builtins();
    const Builtin& entry = builtins.at(call.builtin);
    Witness witness = witnessOf(solver, call);

    bool isCall = call.mode == BuiltinCallMode::Call || call.mode == BuiltinCallMode::Construct;
    if (isCall && !entry.flags.contains(BuiltinFlag::LoadsModuleByName) && isAppliedBySeenCode(solver, call)) {
        const BuiltinUnseenCode* unseenCode = builtins.unseenCodeOf(call.builtin);
        bool mayRunUnseenCode = unseenCode ? conditionMayHold(solver, call, unseenCode->condition) : entry.flags.contains(BuiltinFlag::RunsUnseenCode);
        if (mayRunUnseenCode) {
            raiseUnseenCode(solver, UnseenCodeCause::RunsUnseenCode, call.builtin, witness);
            FlowNodeID unknownJavaScript = solver.graph().unknownJavaScript();
            solver.connect(call.receiver, unknownJavaScript, { }, witness);
            for (FlowNodeID position : call.arguments.positions)
                solver.connect(position, unknownJavaScript, { }, witness);
            solver.connect(call.arguments.rest, unknownJavaScript, { }, witness);
        }
    }

    for (const BuiltinModuleLoad& load : builtins.moduleLoadsOf(call.builtin)) {
        if (load.condition && conditionMayHold(solver, call, load.condition))
            loadRuntimeModule(solver, load, witness);
    }
    for (const BuiltinCalledFunction& called : builtins.calledFunctionsOf(call.builtin)) {
        if (called.condition && conditionMayHold(solver, call, called.condition))
            enterCalledFunction(solver, called, witness);
    }
    enterFeaturesOf(solver, builtins.canonicalID(call.builtin), witness, [&](const BuiltinFeatureUse& line) {
        return !line.overload && line.condition && conditionMayHold(solver, call, line.condition);
    });
}

static bool runsOtherCode(const BuiltinOverload& overload)
{
    return overload.effects.capability == BuiltinCapability::JS && !overload.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode);
}

void applyOverloadAttachments(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    const BuiltinTable& builtins = solver.builtins();
    BuiltinID builtin = overloadCall.call.builtin;
    Witness witness = witnessOf(solver, overloadCall.call);
    enterFeaturesOf(solver, builtins.canonicalID(builtin), witness, [&](const BuiltinFeatureUse& line) {
        return line.overload == overloadCall.id && conditionMayHold(solver, overloadCall.call, line.condition);
    });
    if (!runsOtherCode(overloadCall.overload))
        return;

    for (const BuiltinModuleLoad& load : builtins.moduleLoadsOf(builtin)) {
        if (load.condition && load.condition->callRunsOtherCode)
            loadRuntimeModule(solver, load, witness);
    }
    for (const BuiltinCalledFunction& called : builtins.calledFunctionsOf(builtin)) {
        if (called.condition && called.condition->callRunsOtherCode)
            enterCalledFunction(solver, called, witness);
    }
}

void noteUndescribedNativeCode(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    const BuiltinTable& builtins = solver.builtins();
    const BuiltinOverload& overload = overloadCall.overload;
    const FlowBuiltinCall& call = overloadCall.call;
    const Builtin& entry = builtins.at(call.builtin);
    // The engine's own native code and the helpers of the compiler call no function of the host's library and make no promise of the host.
    if (entry.origin != BuiltinOrigin::Host || entry.flags.contains(BuiltinFlag::IsCompilerHelper))
        return;

    bool isCall = overload.mode == BuiltinCallMode::Call || overload.mode == BuiltinCallMode::Construct;
    bool answersAnything = builtins.at(overload.result.type).flags.containsAny({ BuiltinTypeFlag::IsAnything, BuiltinTypeFlag::HasAnyObject });
    bool mayAnswerPromise = isCall && runsOtherCode(overload) && answersAnything;
    if ((!overload.isUndeclared() && !mayAnswerPromise) || !isInSeenCode(solver, call.request.site))
        return;

    Witness witness = witnessOf(solver, call);
    if (overload.isUndeclared())
        letUndeclaredNativeCodeRun(solver, witness);
    else
        noteInstanceOf(solver, BuiltinClasses::Promise, witness);
}

void enterAttachmentsOfEscapedBuiltin(FlowSolver& solver, BuiltinID builtin, const Witness& witness)
{
    const BuiltinTable& builtins = solver.builtins();
    auto mayHoldForUnknownOperands = [&](const BuiltinCondition* condition) {
        return !condition || !condition->buildAdmitsRuntimeCode || solver.build().options().admitsRuntimeCode();
    };
    for (const BuiltinModuleLoad& load : builtins.moduleLoadsOf(builtin)) {
        if (mayHoldForUnknownOperands(load.condition))
            loadRuntimeModule(solver, load, witness);
    }
    for (const BuiltinCalledFunction& called : builtins.calledFunctionsOf(builtin)) {
        if (called.condition && mayHoldForUnknownOperands(called.condition))
            enterCalledFunction(solver, called, witness);
    }
    enterFeaturesOf(solver, builtin, witness, [](const BuiltinFeatureUse&) {
        return true;
    });
}

// Native code does not keep what it reads from a module to itself: it hangs it on objects of the realm, where code that nobody has seen finds it.
static FlowNodeID valuesReadByNativeCode(FlowSolver& solver, ModuleID module, const Witness& witness)
{
    FlowNodeID values = solver.temporaryFor(callTemporaryKey(nativeCodeSite(solver), FlowCallTemporaryRole::ExportsReadByNativeCode, module.index()));
    handToUnknownRecipient(solver, values, witness);
    return values;
}

static ExportLookup readExport(FlowSolver& solver, const ModuleExport& entry, FlowNodeID values, const Witness& witness)
{
    switch (entry.kind) {
    case ModuleExportKind::Variable: {
        if (!entry.variable)
            return ExportLookup::Unknown;
        FlowNodeID variable = solver.graph().variable(entry.variable);
        solver.connect(variable, values);
        solver.noteLocationEvent(variable, FlowLocationEvent::ReadByUnknownCode);
        return ExportLookup::Exposed;
    }
    case ModuleExportKind::Namespace:
        connectNamespaceOf(solver, entry.module, values, witness);
        return ExportLookup::Exposed;
    case ModuleExportKind::Indirect:
        return ExportLookup::Exposed;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ExportLookup readExportNamed(FlowSolver& solver, const Module& module, StringID name, FlowNodeID values, const Witness& witness)
{
    auto entry = std::ranges::find(module.exports, name, &ModuleExport::name);
    if (entry != module.exports.end())
        return readExport(solver, *entry, values, witness);
    return ExportLookup::Unknown;
}

// Native code requires the module and reads the name as a property of what that answers, not the variable that an import of the name reads. The object that the require answers stays where it is.
static void readMemberOfRequiredValue(FlowSolver& solver, ModuleID module, const BuiltinName& name, FlowNodeID values, const Witness& witness)
{
    FlowStatementSite site = nativeCodeSite(solver);
    FlowNodeID requiredValue = solver.temporaryFor(callTemporaryKey(site, FlowCallTemporaryRole::RequiredValueReadByNativeCode, module.index()));
    connectRequiredValueOf(solver, module, requiredValue, witness);
    runProtocolsInOwnStatement(solver, { site, ValueID(), requiredValue, Protocol::Named, std::span(&name, 1), values, solver.graph().nativeCode() });
}

void loadRuntimeModule(FlowSolver& solver, const BuiltinModuleLoad& load, const Witness& witness)
{
    ModuleID id = moduleOf(solver.callState(), load.module);
    if (!id) {
        if (BuiltinID namespaceObject = solver.builtins().namespaceOf(load.module))
            noteBuiltinUse(solver, namespaceObject, BuiltinUse::Read, witness);
        return;
    }
    evaluateModule(solver, id, witness);

    const Program& program = solver.program();
    const Module& module = program.at(id);
    switch (module.kind) {
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return;
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
        break;
    }

    if (!load.readsEveryExport && load.exports.empty())
        return;

    FlowNodeID values = valuesReadByNativeCode(solver, id, witness);
    if (load.readsEveryExport) {
        connectRequiredValueOf(solver, id, values, witness);
        return;
    }
    for (const BuiltinName& name : load.exports) {
        if (readExportNamed(solver, module, program.stringOf(name), values, witness) == ExportLookup::Unknown)
            readMemberOfRequiredValue(solver, id, name, values, witness);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
