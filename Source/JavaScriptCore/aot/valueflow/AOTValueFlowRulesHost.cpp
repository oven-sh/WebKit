/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRulesModules.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTKnownClosures.h"
#include "AOTValueFlowBuiltins.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include "AOTValueFlowRules.h"
#include "AOTValueFlowRulesHeap.h"
#include <algorithm>
#include <utility>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr ValueFlowName everyProperty { ValueFlowNameKind::EveryProperty, 0 };

static ClimbingSet only(ValueFlowRuleContext& context, ValueFlowObjectID object)
{
    return ClimbingSet(context.maker().of(object));
}

// The host makes the module object of a CommonJS module for whoever asks for it. An operation that names one gives it a number. `children`, `parent` and `require.main` lead to others,
// and only to modules that hold what they export in one variable, which the `exports` of the module object shows.
static void nameModuleObjects(const Program& program, ValueFlowBody& body)
{
    ValueFlowBody::Constant moduleObjects;
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        if (module.kind == ModuleKind::CommonJs && module.body && module.exportsVariable)
            moduleObjects.objects.append({ ValueFlowObjectKind::ModuleBuiltin, program.builtins().canonicalID(Builtins::ModuleObject).index(), ModuleID(index) });
    }
    if (!moduleObjects.isEmpty())
        body.constants.append(WTF::move(moduleObjects));
}

void describeHost(const Program& program, ValueFlowBody& body)
{
    RELEASE_ASSERT(body.nodes.size() == body.thrown() + 1);
    for (unsigned index = 0; index < numberOfValueFlowHostNodes; ++index)
        body.nodes.append({ ValueFlowBodyNodeKind::Merge, { }, 0 });

    nameModuleObjects(program, body);

    for (ValueFlowStatementKind kind : { ValueFlowStatementKind::HostRoots, ValueFlowStatementKind::UsedBuiltins, ValueFlowStatementKind::UnseenCode, ValueFlowStatementKind::ModulesLoadedByName })
        body.statements.append({ { }, { }, 0, 0, { }, { }, kind });
}

namespace {

enum class NativeCaller : uint8_t {
    // Whoever holds the closure.
    Anybody,
    // Native code of the host, on behalf of a built-in whose row names the function.
    NamedByTable,
};

} // anonymous namespace

// The host's native code passes what the function's own annotations say: there is no second list of types. A validating build tests each parameter at the head of the function, so a wrong annotation aborts there.
// Only an annotation counts that the checker has held the body against. A type that was inferred from what the body does with the parameter says nothing about the caller.
static bool isKeptByHost(const Function& callee, unsigned index, const DeclaredType& declared)
{
    return callee.entryParameterKind(index) == EntryParameter::Parameter && declared.site && !declared.flags.contains(PositionFlags::IsFromCompiler) && callee.has(FunctionFlags::IsChecked);
}

// An annotation gives a type with a site, and a type that is only expected has a Check behind it. A type with neither was made by the compiler: every entrypoint of the function assumes it, whoever calls.
static bool isAssumedByEveryEntrypoint(const DeclaredType& declared)
{
    return !declared.site && !declared.flags.contains(PositionFlags::IsExpected);
}

static void passFromNativeCode(ValueFlowRuleContext& context, FunctionID function, const Function& callee, unsigned index, NativeCaller caller, ValueSetID unknown)
{
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    ValueFlowNodeID parameter = context.nodes().entryParameter(function, index);
    // Of a function that is gone through once for each key, `args[1]` answers what the calls of the instance have passed at that position and never looks at the array: HeapRule::readsRestArgumentOfInstance().
    // Native code enters the instance of the other calls, and what it passes may lie at any index.
    bool isRestParameter = callee.has(FunctionFlags::HasRestParameter) && index == callee.firstParameterIndex() + callee.numberOfPositionalParameters();
    if (ValueFlowNodeID passedAtAnyIndex = isRestParameter ? context.restArgumentEntered(function, numberOfGroupedArguments) : ValueFlowNodeID())
        context.add(passedAtAnyIndex, unknown);
    // Whoever calls, the function itself makes the array of its rest parameter and the list behind `arguments`, and counts what it was passed. The caller chooses what lies in the array, as in passArguments():
    // under the last name for the one call into which a function spreads an array that it does nothing else with.
    if (isRestParameter || callee.hiddenRestParameterIndex() == index) {
        ClimbingSet rest = only(context, universe.restArrayOf(function));
        context.add(parameter, rest);
        context.add(context.written(rest, { ValueFlowNameKind::Elements, 0 }), unknown);
        context.add(context.written(rest, { ValueFlowNameKind::RestArgument, numberOfGroupedArguments + 1 }), unknown);
        return;
    }
    if (callee.argumentCountParameterIndex() == index) {
        context.add(parameter, universe.anyPrimitive());
        return;
    }
    const DeclaredType* declared = callee.declaredEntryParameter(index);
    bool isTypeKept = declared && declared->type && (isAssumedByEveryEntrypoint(*declared) || (caller == NativeCaller::NamedByTable && isKeptByHost(callee, index, *declared)));
    ValueSetID mask = isTypeKept ? universe.maskOf(context.maker(), program, program.at(declared->type)) : ValueSets::anything;
    if (mask == ValueSets::anything) {
        context.add(parameter, unknown);
        return;
    }

    const Type& type = program.at(declared->type);
    context.add(parameter, context.maker().intersect(universe.anyPrimitive(), mask));
    if (!type.hasObjects())
        return;
    // Where only the program makes an object of the type, native code passes one that it was handed. Any other object may be one that native code has made.
    if (universe.namesProgramObjectsAlone(program, type))
        context.connect(context.masked(ValueFlowProgramNodes::escaped(), context.maker().intersect(universe.familyObjectsOf(context.maker(), program, type), mask)), parameter);
    else
        context.add(parameter, context.maker().intersect(unknown, universe.objects()));
}

static void enterFromNativeCode(ValueFlowRuleContext& context, FunctionID function, NativeCaller caller, ValueFlowObjectID closure)
{
    const ValueFlowUniverse& universe = context.universe();
    const ValueFlowProgramNodes& nodes = context.nodes();
    const Function& callee = context.program().at(function);
    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
    ValueSetID unknown = unknownValue(context, ValueFlowUnknownSource::ParameterOfEscapedFunction);

    context.reach(function);
    for (unsigned index = 0; index < callee.numberOfEntryParameters(); ++index) {
        // Whoever calls a function hands it its own closure as the callee. Beside the unknown object nobody would see the closure, nor what lies above it: the class that `super` goes to.
        if (callee.calleeParameterIndex() == index)
            context.add(nodes.entryParameter(function, index), only(context, closure));
        else if (callee.entryParameterKind(index) != EntryParameter::Capture)
            passFromNativeCode(context, function, callee, index, caller, unknown);
    }
    // One node holds what a function returns to all its callers. Were it to escape, whatever any caller of the program passes through `identity` would have escaped because native code holds `identity`.
    // A function that tests its arguments has a generic version, which does what the function does with any operands. Entered with the unknown value as every operand it stands for every call
    // that native code makes, so what it returns and throws is what native code gets. The typed body takes what fits of the same operands, and keeps what it returns to the callers of the program.
    if (callee.role == FunctionRole::EntryChecks) {
        enterFromNativeCode(context, universe.genericVersionOf(function), caller, closure);
        return;
    }
    // A parameter that the function returns as it came gives native code back what native code has passed. What the callers of the program pass there stays with them.
    if (const auto* summary = nodes.resultSummaryOf(function))
        context.connect(summary->ownResult, escaped);
    else {
        for (unsigned index = 0; index < callee.results.size(); ++index)
            context.connect(nodes.result(function, index), escaped);
    }
    context.connect(nodes.thrown(function), escaped);
    if (!callee.isUnsplitCoroutine())
        return;

    // Each call of an async function makes a promise of its own. The one that native code gets from its call is an object that the program has never seen: the unknown object stands for it,
    // and what it holds has escaped above. The object of the function stands for the promises that the callers of the program get, to which native code does nothing unless one of them escapes.
    if (!callee.has(FunctionFlags::IsGenerator))
        return;
    context.add(escaped, only(context, universe.coroutineResultOf(function)));
    context.connect(nodes.yielded(function), escaped);
    context.add(nodes.sent(function), unknown);
}

void enterFromNativeCode(ValueFlowRuleContext& context, FunctionID function)
{
    enterFromNativeCode(context, function, NativeCaller::Anybody, context.universe().closureOf(function));
}

void passWhatAnnotationsSay(ValueFlowRuleContext& context, ClimbingSet callees)
{
    ValueSetID unknown = unknownValue(context, ValueFlowUnknownSource::ParameterOfEscapedFunction);
    context.forEachMember(context.restrict(callees, context.universe().closures()), [&](ValueFlowObjectID closure) {
        FunctionID function = context.universe().closureFunction(closure);
        const Function& callee = context.program().at(function);
        for (unsigned index = 0; index < callee.numberOfEntryParameters(); ++index) {
            if (callee.entryParameterKind(index) == EntryParameter::Parameter)
                passFromNativeCode(context, function, callee, index, NativeCaller::NamedByTable, unknown);
        }
    });
}

static bool isRaised(ValueFlowRuleContext& context, ValueFlowHostNode flag)
{
    return context.contains(context.valueOf(context.nodes().ofHost(flag)), ValueFlowUniverse::unknownObject());
}

static void raise(ValueFlowRuleContext& context, ValueFlowHostNode flag)
{
    context.add(context.nodes().ofHost(flag), context.leavesOutSomeUnknownSource() ? context.universe().undefinedValue() : context.universe().unknownObjectAlone());
}

void letUnseenCodeRun(ValueFlowRuleContext& context, UnseenCodeReach reach)
{
    raise(context, ValueFlowHostNode::UnseenCodeRuns);
    if (reach != UnseenCodeReach::GlobalScope)
        raise(context, ValueFlowHostNode::UnseenCodeHasLoader);
    if (reach == UnseenCodeReach::Realm)
        raise(context, ValueFlowHostNode::UnseenCodeChangesRealm);
}

void letUnknownCodeWrite(ValueFlowRuleContext& context)
{
    raise(context, ValueFlowHostNode::UnknownCodeWrites);
}

bool mayUnknownCodeWrite(ValueFlowRuleContext& context)
{
    return isRaised(context, ValueFlowHostNode::UnknownCodeWrites) || isRaised(context, ValueFlowHostNode::UnseenCodeRuns);
}

void letWhateverHasEscapedBeCalled(ValueFlowRuleContext& context)
{
    raise(context, ValueFlowHostNode::ValueNobodyKnowsIsCalled);
}

bool mayCallWhateverHasEscaped(ValueFlowRuleContext& context)
{
    return isRaised(context, ValueFlowHostNode::ValueNobodyKnowsIsCalled) || isRaised(context, ValueFlowHostNode::UnseenCodeRuns);
}

// The run time refuses whoever tries to change a locked object of the table, native code too. Of the other objects of the realm it locks the members that compiled code relies on,
// so only unseen code that reaches the realm changes one of those behind the back of the analysis. A new name on one of their prototype objects is for the lookup that passes it:
// HeapRule::holdersOfUndeclaredPropertiesUnder().
void letAnythingHappenTo(ValueFlowRuleContext& context, ClimbingSet escaped)
{
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet objects = context.restrict(escaped, ValueSets::complement(universe.lockedObjects()));
    if (!isRaised(context, ValueFlowHostNode::UnseenCodeChangesRealm))
        objects = context.restrict(objects, ValueSets::complement(universe.objectsOfRealm()));
    ValueSetID unknown = unknownValue(context, ValueFlowUnknownSource::ContentsOfEscapedObject);
    ValueSetID unknownObject = context.maker().intersect(unknown, universe.objects());
    context.mark(objects, OptionSet<ObjectMutation>::all());
    context.add(context.written(objects, { ValueFlowNameKind::UnknownKey, 0 }), unknown);
    context.add(context.written(objects, { ValueFlowNameKind::InternalFields, 0 }), unknown);
    context.add(context.written(objects, { ValueFlowNameKind::GetterUnderUnknownKey, 0 }), unknownObject);
    context.add(context.written(objects, { ValueFlowNameKind::SetterUnderUnknownKey, 0 }), unknownObject);
    context.add(context.written(context.restrict(objects, ValueSets::complement(universe.objectsWithLockedPrototypeLink())), { ValueFlowNameKind::PrototypeLink, 0 }), unknown);
}

// What a function of the library calls that only native code leads to has come from native code, and the table lists built-in by built-in what native code calls,
// or from seen code, which has a statement of its own that hands it over.
void letUndeclaredNativeCodeRun(ValueFlowRuleContext& context)
{
    FunctionID caller = context.functionOfStatement();
    if (!caller || !isInSeenCode(context, caller))
        return;
    context.use(Builtins::UndeclaredNativeCode, BuiltinUse::Read);
    // The method by which a class of the host prints itself is such code, and it calls a function of a start-up module.
    letHostChooseOperands(context);
}

void letHostChooseOperands(ValueFlowRuleContext& context)
{
    raise(context, ValueFlowHostNode::HostChoosesOperands);
}

void letModulesBeLoadedByName(ValueFlowRuleContext& context, ValueFlowHostNode answers)
{
    RELEASE_ASSERT(answers == ValueFlowHostNode::RequiredValuesLoadedByName || answers == ValueFlowHostNode::NamespacesLoadedByName);
    raise(context, answers == ValueFlowHostNode::RequiredValuesLoadedByName ? ValueFlowHostNode::ModulesAreRequiredByName : ValueFlowHostNode::ModulesAreImportedByName);
}

static void enterRuntimeFunction(ValueFlowRuleContext& context, RuntimeFunctionID id, NativeCaller caller)
{
    if (const RuntimeFunctionEntry* entry = findRuntimeFunction(context.program(), id); entry && entry->function)
        enterFromNativeCode(context, entry->function, caller, context.universe().closureOf(entry->function));
}

static void forEachFunctionOf(const BuiltinOverload& overload, NOESCAPE const Invocable<void(RuntimeFunctionID)> auto& functor)
{
    if (RuntimeFunctionID function = overload.code.implementation.function)
        functor(function);
}

static ValueFlowSources memberOfRequiredValue(ValueFlowRuleContext& context, ModuleID module, StringID name)
{
    const Program& program = context.program();
    std::optional<PropertyKeyID> key = program.types.find({ .kind = PropertyKeyKind::Text, .text = name });
    return { { context.lookup(valueOf(context, requiredValueOf(context, module)), key ? ValueFlowName::property(*key) : everyProperty) }, { } };
}

// What the host finds under the name of an export of an entry point. Nothing where the module cannot have the name.
static std::optional<ValueFlowSources> namedExportOf(ValueFlowRuleContext& context, ModuleID id, StringID name)
{
    const Program& program = context.program();
    const Module& module = program.at(id);
    ValueFlowSources unknown { { }, ClimbingSet(unknownValue(context, ValueFlowUnknownSource::DataFromOutside)) };

    auto found = std::ranges::find(module.exports, name, &ModuleExport::name);
    if (found != module.exports.end()) {
        switch (found->kind) {
        case ModuleExportKind::Variable:
            if (!found->variable)
                return unknown;
            useModule(context, program.at(found->variable).module);
            return ValueFlowSources { { context.nodes().variable(found->variable) }, { } };
        case ModuleExportKind::Namespace:
            return namespaceOf(context, found->module, found->phase);
        case ModuleExportKind::Indirect:
            return unknown;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    if (module.exportRule != SyntheticExportRule::None || module.exportsVariable) {
        bool isDefault = name == program.stringOf(BuiltinNames::defaultKeyword);
        ValueFlowSources value = isDefault ? requiredValueOf(context, id) : ValueFlowSources();
        if (!isDefault || module.exportRule != SyntheticExportRule::CommonJsIgnoringAnnotation)
            value.nodes.appendVector(memberOfRequiredValue(context, id, name).nodes);
        return value;
    }

    bool listsEveryExport = module.kind == ModuleKind::SourceText && !module.requests.containsIf([](const ModuleRequest& request) {
        return request.isDynamicStarExport;
    });
    if (listsEveryExport)
        return std::nullopt;
    connect(context, namespaceOf(context, id), ValueFlowProgramNodes::escaped());
    return unknown;
}

// The host hands the value to a built-in, as a call of the program would.
static void handToBuiltin(ValueFlowRuleContext& context, const HostRead& read, const ValueFlowSources& value)
{
    const ValueFlowUniverse& universe = context.universe();
    const BuiltinTable& builtins = context.program().builtins();
    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
    BuiltinID builtin = builtins.canonicalID(read.builtin);
    ValueFlowCallGroup group = context.group(only(context, universe.objectOf(builtin)), ValueFlowCallKind::CallByNativeCode);
    BuiltinID holder = builtins.at(builtin).holder;
    if (holder && isObjectKind(builtins.at(holder).kind))
        context.add(group.thisValue, only(context, universe.objectOf(holder)));
    connect(context, value, group.argumentAt(read.parameter));
    context.connect(group.result, escaped);
    context.connect(group.thrown, escaped);
}

// The host starts a server for a value under whose `fetch` it finds a function, or anything at all under `app` (isServerConfig() in Bun's BunCompiledEntryPoint.cpp). `routes` is here for a host that learns to look there.
// It waits for a promise, and for whatever else has a `then`, and asks the same of what that is fulfilled with, which nobody follows.
static bool mayBeServerConfiguration(ValueFlowRuleContext& context, const ValueFlowSources& value)
{
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet configuration = valueOf(context, value);
    if (context.meet(configuration, universe.placeholdersAlone()))
        return true;
    auto found = [&](BuiltinName name) {
        return context.valueOf(context.lookup(configuration, ValueFlowName::property(program.keyOf(name))));
    };
    for (BuiltinName name : { BuiltinNames::fetch, BuiltinNames::then }) {
        ClimbingSet function = found(name);
        if (context.meet(function, universe.callables()) || context.meet(function, universe.placeholdersAlone()))
            return true;
    }
    ValueFlowObjectID missing[] { ValueFlowUniverse::primitive(PrimitiveSet::Undefined), ValueFlowUniverse::primitive(PrimitiveSet::Null) };
    ValueSetID present = ValueSets::complement(context.maker().of(std::span<const ValueFlowObjectID>(missing)));
    return context.meet(found(BuiltinNames::app), present) || context.meet(found(BuiltinNames::routes), present);
}

static void applyHostRead(ValueFlowRuleContext& context, ModuleID module, const HostRead& read)
{
    std::optional<ValueFlowSources> value = namedExportOf(context, module, read.exportName);
    if (!value)
        return;
    if (!read.builtin) {
        connect(context, *value, ValueFlowProgramNodes::escaped());
        return;
    }
    // What the host looks at and leaves alone it does not keep.
    if (context.program().builtins().canonicalID(read.builtin) == Builtins::BunServe && !mayBeServerConfiguration(context, *value))
        return;
    handToBuiltin(context, read, *value);
}

// The load of an entry point answers a promise for its namespace object. With a function under `then` that object is a thenable: the promise calls the function,
// and what the function hands to its first argument stands in the place of the namespace. Nobody follows that value to the host, which reads the same names off it.
static void applyHostReadsOfThenable(ValueFlowRuleContext& context, const Entry& entry)
{
    std::optional<ValueFlowSources> then = namedExportOf(context, entry.module, context.program().stringOf(BuiltinNames::then));
    if (!then)
        return;
    ClimbingSet found = valueOf(context, *then);
    if (!context.meet(found, context.universe().callables()) && !context.contains(found, ValueFlowUniverse::unknownObject()))
        return;
    ValueFlowSources unknown { { }, ClimbingSet(unknownValue(context, ValueFlowUnknownSource::DataFromOutside)) };
    for (const HostRead& read : entry.hostReads) {
        if (read.builtin)
            handToBuiltin(context, read, unknown);
    }
}

// Nobody has looked at the operands here, so a condition on one of them may hold. A build that admits code at run time is not solved.
static bool mayHold(const BuiltinCondition* condition)
{
    return !condition || !condition->buildAdmitsRuntimeCode;
}

// A row with a condition speaks of a call, so it takes one: `typeof setTimeout` and `const later = setTimeout` run nothing. A read or a store of what is no object is the call.
// Whoever applies a built-in says what the row asks for: whether an overload that runs other code may take the call, whether an operand may have a property or be a text. Something then lies in a cell of native code
// on the built-in's own object. Nobody says what a caller passes that nobody follows.
static bool mayHold(ValueFlowRuleContext& context, BuiltinID builtin, const BuiltinCondition* condition)
{
    if (!condition)
        return true;
    if (!mayHold(condition))
        return false;
    // What the built-in answers is the unknown object wherever it goes, or an object of the host whose stores are noted, so no store into it goes unnoticed.
    if (condition->objectNobodyKnowsIsWritten)
        return isRaised(context, ValueFlowHostNode::ObjectNobodyKnowsIsWritten) || mayUnknownCodeWrite(context);

    const BuiltinTable& builtins = context.program().builtins();
    ValueFlowObjectID objectOfBuiltin = context.universe().objectOf(builtin);
    ClimbingSet object = only(context, objectOfBuiltin);
    // A row about a call on one entry takes a call whose receivers list that entry, and a caller nobody follows lists nothing. What has escaped is held until a call may reach it.
    bool isRun = context.holdsAnyOf(object, context.nodes().ofHost(ValueFlowHostNode::RunBuiltins));
    if (!condition->calledOn && (isRun || mayCallWhateverHasEscaped(context)) && context.holdsAnyOf(object, ValueFlowProgramNodes::escaped()))
        return true;

    auto holdsSomething = [&](ValueFlowName cell) {
        return context.meet(context.valueOf(context.read(object, cell)), ValueSets::anything);
    };
    if (isObjectKind(builtins.at(builtin).kind)) {
        if (!isRun)
            return false;
        if (condition->callRunsOtherCode && !holdsSomething(cellOfCallsThatRunOtherCode(builtins)))
            return false;
        if (condition->firstText() && !holdsSomething({ ValueFlowNameKind::Slot, builtins.slotCount() + condition->firstText().index() }))
            return false;
    }
    return !condition->hasProperty || holdsSomething({ ValueFlowNameKind::Slot, builtins.slotCount() + condition->hasProperty.index() });
}

// The row says what native code does with what it reads from the module. Where it says nothing, native code does not keep the value to itself.
static void loadRuntimeModule(ValueFlowRuleContext& context, const BuiltinModuleLoad& load)
{
    const Program& program = context.program();
    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
    auto use = [&](const ValueFlowSources& value, size_t indexOfExport) {
        BuiltinHostUses uses = indexOfExport < load.usesOfExports.size() ? load.usesOfExports[indexOfExport] : BuiltinHostUses();
        if (!uses.areListed) {
            connect(context, value, escaped);
            return;
        }
        BuiltinID onBehalfOf = load.builtin ? load.builtin : Builtins::UndeclaredNativeCode;
        for (ValueFlowNodeID node : value.nodes)
            applyHostUses(onBehalfOf, { node, ValueSets::nothing, ValueSets::anything }, uses, context);
        if (context.meet(value.constant, ValueSets::anything))
            applyHostUses(onBehalfOf, context.operandHolding(value.constant), uses, context);
    };
    ModuleID id = load.module ? context.universe().moduleOf(load.module) : moduleOf(program, load.module);
    if (!id) {
        if (BuiltinID namespaceObject = program.builtins().namespaceOf(load.module))
            context.use(namespaceObject, BuiltinUse::Read);
        return;
    }
    evaluateModule(context, id);

    const Module& module = program.at(id);
    if (module.kind == ModuleKind::Runtime || module.kind == ModuleKind::External)
        return;
    if (load.readsEveryExport) {
        // Native code calls what it reads here. The object on which the exports hang is only held wherever it arrives, so the exports are handed over by themselves.
        ValueFlowSources answer = requiredValueOf(context, id);
        connect(context, answer, escaped);
        context.connect(context.read(withSourcesOfCopies(context, context.restrict(valueOf(context, answer), context.universe().knownObjects())), everyProperty), escaped);
        return;
    }
    for (size_t index = 0; index < load.exports.size(); ++index) {
        BuiltinName name = load.exports[index];
        auto found = std::ranges::find(module.exports, program.stringOf(name), &ModuleExport::name);
        bool isListed = found != module.exports.end() && (found->kind != ModuleExportKind::Variable || found->variable);
        if (!isListed) {
            // Native code requires the module and reads the name as a property of what that answers. The object that the require answers stays where it is.
            use(memberOfRequiredValue(context, id, program.stringOf(name)), index);
            continue;
        }
        switch (found->kind) {
        case ModuleExportKind::Variable:
            useModule(context, program.at(found->variable).module);
            use({ { context.nodes().variable(found->variable) }, { } }, index);
            break;
        case ModuleExportKind::Namespace:
            use(namespaceOf(context, found->module), index);
            break;
        case ModuleExportKind::Indirect:
            break;
        }
    }
}

namespace {

// What hangs on the built-ins that one run of a statement looks at, each function and each row once.
class Attachments {
public:
    explicit Attachments(ValueFlowRuleContext& context)
        : m_context(context)
        , m_builtins(context.program().builtins())
    {
    }

    // For a built-in that is read, written, called or has escaped.
    void addRowsOf(BuiltinID builtin)
    {
        builtin = m_builtins.canonicalID(builtin);
        if (m_visitedBuiltins.set(builtin.index()))
            return;
        for (const BuiltinCalledFunction& called : m_builtins.calledFunctionsOf(builtin)) {
            if (mayHold(m_context, builtin, called.condition))
                add(called);
        }
        for (const BuiltinModuleLoad& load : m_builtins.moduleLoadsOf(builtin)) {
            if (mayHold(m_context, builtin, load.condition))
                m_loads.append(&load);
        }
    }

    // For a built-in whose code runs: the functions of the library that stand behind it.
    void addCodeOf(BuiltinID builtin)
    {
        for (const BuiltinOverload& overload : m_builtins.overloadsOf(m_builtins.canonicalID(builtin))) {
            forEachFunctionOf(overload, [&](RuntimeFunctionID function) {
                addCode(function);
            });
        }
    }

    // For a member that is stored to: what the table lists for the store. The host puts the new value in place of a property that it has not made yet, so nothing that makes the value runs.
    void addCodeOfStoresTo(BuiltinID builtin)
    {
        for (const BuiltinOverload& overload : m_builtins.overloadsOf(m_builtins.canonicalID(builtin))) {
            if (overload.mode != BuiltinCallMode::Set)
                continue;
            forEachFunctionOf(overload, [&](RuntimeFunctionID function) {
                addCode(function);
            });
        }
    }

    // For a built-in in hands that nobody follows, which apply any of its overloads: what hangs on the classes of what it answers. Of every other built-in the application says which overloads answer.
    void addClassesAnsweredBy(BuiltinID builtin)
    {
        builtin = m_builtins.canonicalID(builtin);
        for (const BuiltinOverload& overload : m_builtins.overloadsOf(builtin)) {
            addClassesIn(overload.result.type);

            // What the run time does with a promise takes no further call, so whoever may make one counts.
            bool isCall = overload.mode == BuiltinCallMode::Call || overload.mode == BuiltinCallMode::Construct;
            bool runsOtherCode = overload.effects.capability == BuiltinCapability::JS && !overload.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode);
            bool answersAnything = m_builtins.at(overload.result.type).flags.containsAny({ BuiltinTypeFlag::IsAnything, BuiltinTypeFlag::HasAnyObject });
            if (m_builtins.at(builtin).origin == BuiltinOrigin::Host && (isCall || overload.isUndeclared()) && runsOtherCode && answersAnything)
                addRowsOf(BuiltinClasses::Promise);
        }
    }

    void addClassesIn(BuiltinTypeID type)
    {
        if (m_visitedTypes.set(type.index()))
            return;
        for (const BuiltinTypePart& part : m_builtins.at(type).parts) {
            if (part.builtinClass)
                addRowsOf(part.builtinClass);
            for (const BuiltinSlotContents& contents : part.contents)
                addClassesIn(contents.type);
        }
    }

    // Where an instance of a class may exist, native code does with it what hangs on the constructor and on the prototype object.
    void addRowsOf(BuiltinClassID builtinClass)
    {
        for (BuiltinClassID current = builtinClass; current && !m_visitedClasses.set(current.index()); current = m_builtins.at(current).parent) {
            for (BuiltinID object : { m_builtins.at(current).constructor, m_builtins.at(current).prototype }) {
                if (object)
                    addRowsOf(object);
            }
        }
    }

    void addStartupModules()
    {
        for (const BuiltinModuleLoad& load : m_builtins.startupLoads())
            m_loads.append(&load);
        std::span<const BuiltinCalledFunction> rows = m_builtins.startupCalledFunctions();
        for (RuntimeFunctionID function : m_builtins.functionsCalledWithStartupLoads()) {
            auto row = std::ranges::find(rows, function, &BuiltinCalledFunction::function);
            if (row != rows.end())
                add(*row);
            else
                add(function);
        }
        m_hostMethodsMayRun = true;
    }

    void add(RuntimeFunctionID function) { m_functions.set(function.index()); }

    // Without a list native code calls the function with what the annotations of its parameters say, and anybody may get what that answers.
    void add(const BuiltinCalledFunction& row)
    {
        if (row.uses.areListed)
            m_rowsListingUses.append(&row);
        else
            add(row.function);
    }

    // A function that stands behind a built-in runs for whoever calls the built-in, and that may be seen code.
    void addCode(RuntimeFunctionID function)
    {
        add(function);
        m_functionsBehindBuiltins.set(function.index());
    }

    void enter()
    {
        for (size_t function : m_functions)
            enterRuntimeFunction(m_context, RuntimeFunctionID(static_cast<uint32_t>(function)), m_functionsBehindBuiltins.get(function) ? NativeCaller::Anybody : NativeCaller::NamedByTable);
        for (size_t function : m_functionsBehindBuiltins) {
            if (const RuntimeFunctionEntry* entry = findRuntimeFunction(m_context.program(), RuntimeFunctionID(static_cast<uint32_t>(function))); entry && entry->function)
                m_context.add(m_context.nodes().ofHost(ValueFlowHostNode::FunctionsOfSeenCode), only(m_context, m_context.universe().closureOf(entry->function)));
        }
        // Evaluated text has of the host what the program names. It finds the function where native code has put it, and calls it with what it likes.
        bool textFindsFunctions = isRaised(m_context, ValueFlowHostNode::UnseenCodeRuns);
        for (const BuiltinCalledFunction* row : m_rowsListingUses) {
            const RuntimeFunctionEntry* entry = findRuntimeFunction(m_context.program(), row->function);
            if (!entry || !entry->function)
                continue;
            ClimbingSet closure = only(m_context, m_context.universe().closureOf(entry->function));
            applyHostUses(row->builtin ? row->builtin : Builtins::UndeclaredNativeCode, { { }, m_context.maker().of(m_context.universe().closureOf(entry->function)), ValueSets::anything }, row->uses, m_context);
            if (textFindsFunctions)
                m_context.add(ValueFlowProgramNodes::escaped(), closure);
        }
        for (const BuiltinModuleLoad* load : m_loads)
            loadRuntimeModule(m_context, *load);
        if (!m_hostMethodsMayRun)
            return;
        for (const BuiltinHostMethodCall& methodCall : m_builtins.hostMethodCalls())
            callFromHost(Builtins::UndeclaredNativeCode, { nodeReadFromNativeCell(m_context, methodCall.nativeCell), ValueSets::nothing, ValueSets::anything }, *methodCall.call, m_context);
    }

private:
    ValueFlowRuleContext& m_context;
    const BuiltinTable& m_builtins;
    BitVector m_visitedBuiltins;
    BitVector m_visitedClasses;
    BitVector m_visitedTypes;
    BitVector m_functions;
    BitVector m_functionsBehindBuiltins;
    Vector<const BuiltinModuleLoad*, 16> m_loads;
    Vector<const BuiltinCalledFunction*, 16> m_rowsListingUses;
    bool m_hostMethodsMayRun { false };
};

} // anonymous namespace

static bool isLibraryRole(FunctionRole role)
{
    return role == FunctionRole::Library || role == FunctionRole::Model;
}

// What native code holds and does not call. A caller that nobody follows has loaded a module of the library by name: what the module exports is in its hands with the object that holds the exports.
// To whoever gets such an object back it is the unknown object, so it is read and written like an object that has escaped. But no function among these is entered with operands nobody knows, and
// no code of the library comes into the image for them: a call by value of a function that nothing has entered throws an error that names the function.
static void followWhatNativeCodeHolds(ValueFlowRuleContext& context)
{
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    ValueFlowNodeID heldNode = context.nodes().ofHost(ValueFlowHostNode::HeldByNativeCode);
    ValueFlowNodeID escapedNode = ValueFlowProgramNodes::escaped();
    ValueSetID objectsOfProgram = context.maker().intersect(universe.knownObjects(), ValueSets::complement(universe.builtinObjects()));
    ClimbingSet all = context.valueOf(heldNode);
    // Native code reaches the built-in objects without being handed them.
    ClimbingSet held = context.restrict(all, objectsOfProgram);
    held = context.unite(held, context.valueWithin(ClimbingSet(universe.holdersOfLibraryExports()), heldNode));
    // The set of what an object with many members holds is given up: whatever the analysis has stopped following may be held.
    if (context.contains(all, ValueFlowUniverse::untrackedObject()))
        held = context.unite(held, context.valueWithin(ClimbingSet(objectsOfProgram), ValueFlowProgramNodes::lost()));
    if (!context.meet(held, ValueSets::anything))
        return;

    // Whoever holds an object holds what it leads to, in the same way.
    for (ValueFlowNameKind kind : { ValueFlowNameKind::EveryProperty, ValueFlowNameKind::EveryGetter, ValueFlowNameKind::EverySetter, ValueFlowNameKind::PrototypeLink, ValueFlowNameKind::CopiedFrom })
        context.connect(context.read(held, { kind, 0 }), heldNode);
    context.add(heldNode, objectsHeldFromCreation(context, held));
    context.mark(held, ObjectMutation::ReachNativeCode);

    // What the program stores through the unknown object, and what it does to it, may hit any of these.
    ClimbingSet unknownObject(universe.unknownObjectAlone());
    constexpr std::pair<ValueFlowNameKind, ValueFlowNameKind> foundAndWrittenNames[] {
        { ValueFlowNameKind::EveryProperty, ValueFlowNameKind::UnknownKey },
        { ValueFlowNameKind::EveryGetter, ValueFlowNameKind::GetterUnderUnknownKey },
        { ValueFlowNameKind::EverySetter, ValueFlowNameKind::SetterUnderUnknownKey },
        { ValueFlowNameKind::PrototypeLink, ValueFlowNameKind::PrototypeLink },
    };
    for (auto [found, written] : foundAndWrittenNames)
        context.connect(context.read(unknownObject, { found, 0 }), context.written(held, { written, 0 }));
    for (ObjectMutation mutation : OptionSet<ObjectMutation>::all()) {
        if (context.meet(context.markedAmong(unknownObject, mutation), ValueSets::anything))
            context.mark(held, mutation);
    }
    if (mayUnknownCodeWrite(context))
        letAnythingHappenTo(context, held);

    // A function has one body, and its parameters are narrowed to what the solve passes. So a function of the library that anything enters has escaped like any other. So has a function of the program.
    ClimbingSet closures = context.restrict(held, universe.closures());
    context.forEachMember(closures, [&](ValueFlowObjectID closure) {
        FunctionID function = universe.closureFunction(closure);
        if (!isLibraryRole(program.at(function).role) || context.isReached(function))
            context.add(escapedNode, only(context, closure));
    });
}

void runHostRoots(const ValueFlowStatement&, ValueFlowRuleContext& context)
{
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    followWhatNativeCodeHolds(context);
    // A call that names its callee has no statement.
    ValueFlowNodeID functionsOfSeenCode = context.nodes().ofHost(ValueFlowHostNode::FunctionsOfSeenCode);
    // What is added for a function follows from the function alone: once a thread is enough.
    context.forEachMember(context.membersNewToThread(context.restrict(context.valueOf(functionsOfSeenCode), universe.closures()), 2), [&](ValueFlowObjectID closure) {
        FunctionID id = universe.closureFunction(closure);
        const Function& function = program.at(id);
        if (!isLibraryRole(function.role))
            return;
        // It follows from the function, and this statement runs whenever a function arrives.
        std::atomic<uint32_t>& remembered = universe.rememberedClosuresCalledDirectlyBy(id);
        uint32_t setPlusOne = remembered.load(std::memory_order_acquire);
        if (!setPlusOne) {
            Vector<ValueFlowObjectID, 8> called;
            for (const Value& value : function.values) {
                if (value.opcode == Opcode::CallDirect)
                    called.append(universe.closureOf(value.function()));
            }
            setPlusOne = context.maker().of(called.span()).index() + 1;
            remembered.store(setPlusOne, std::memory_order_release);
        }
        context.forEachMember(ClimbingSet(ValueSetID(setPlusOne - 1)), [&](ValueFlowObjectID called) {
            context.add(functionsOfSeenCode, only(context, called));
        });
    });

    for (const Entry& entry : program.entries) {
        // The host asks for the entry point in the name of the program.
        context.add(context.nodes().ofHost(ValueFlowHostNode::ModulesOfSeenCode), only(context, context.universe().namespaceOf(entry.module)));
        evaluateModule(context, entry.module);
        if (program.at(entry.module).kind == ModuleKind::CommonJs)
            namespaceOf(context, entry.module);
        for (const HostRead& read : entry.hostReads)
            applyHostRead(context, entry.module, read);
        applyHostReadsOfThenable(context, entry);
    }
}

std::optional<UnseenCodeCause> eventOfCalling(const BuiltinTable& builtins, BuiltinID builtin, bool refusesEvaluatedText)
{
    const Builtin& entry = builtins.at(builtin);
    if (entry.flags.contains(BuiltinFlag::LoadsModuleByName))
        return UnseenCodeCause::LoadsModuleByName;
    if (entry.flags.contains(BuiltinFlag::EvaluatesText))
        return refusesEvaluatedText ? std::nullopt : std::optional(UnseenCodeCause::EvaluatesText);
    const BuiltinUnseenCode* unseenCode = builtins.unseenCodeOf(builtin);
    if (unseenCode ? mayHold(unseenCode->condition) : entry.flags.contains(BuiltinFlag::RunsUnseenCode))
        return UnseenCodeCause::RunsUnseenCode;
    return std::nullopt;
}

// Once something has escaped and a value nobody knows is called, what the library keeps in its own objects is a value nobody knows, and nearly every store in the library goes through the unknown object.
// The library puts a new name on a built-in object of the realm only where seen code hands it the object: util._extend(target, source), EventEmitter.call(target).
// The object is then an operand of the function that stores. A built-in that writes into an operand writes into its receiver or into its first argument.
static bool storesIntoOperandOfItsFunction(ValueFlowRuleContext& context, FunctionID writer)
{
    ValueID site = context.siteOfStatement();
    if (!site)
        return true;
    const Function& function = context.program().at(writer);
    auto isParameter = [&](ValueID operand) {
        return operand && function.at(definitionThroughSameValues(function, operand)).opcode == Opcode::Parameter;
    };
    const Value& operation = function.at(site);
    if (isHeapOperation(operation.opcode))
        return operation.numberOfOperands() && isParameter(operation.operand(0));
    std::span<const ValueID> operands = operation.operands();
    return std::ranges::any_of(operands.subspan(std::min<size_t>(1, operands.size()), std::min<size_t>(2, operands.size() - std::min<size_t>(1, operands.size()))), isParameter);
}

// A statement that a rule has made stands for callers that nobody tells apart, so it counts.
void noteStoreThroughUnknownObject(ValueFlowRuleContext& context, ValueFlowName name)
{
    switch (name.kind) {
    case ValueFlowNameKind::Property:
    case ValueFlowNameKind::Getter:
    case ValueFlowNameKind::Setter:
    case ValueFlowNameKind::UnknownKey:
    case ValueFlowNameKind::GetterUnderUnknownKey:
    case ValueFlowNameKind::SetterUnderUnknownKey:
        break;
    default:
        return;
    }
    FunctionID writer = context.functionOfStatement();
    if (writer && isLibraryRole(context.program().at(writer).role)) {
        if (isCodeThatRecordsDescribe(context.universe(), writer) || !storesIntoOperandOfItsFunction(context, writer) || !isInSeenCode(context, writer))
            return;
    }
    const ValueFlowUniverse& universe = context.universe();
    switch (name.kind) {
    case ValueFlowNameKind::Property:
        context.add(context.nodes().ofHost(ValueFlowHostNode::NamesStoredThroughUnknownObject), only(context, universe.keyConstant(PropertyKeyID(name.payload))));
        return;
    case ValueFlowNameKind::UnknownKey:
        context.add(context.nodes().ofHost(ValueFlowHostNode::NamesStoredThroughUnknownObject), universe.unknownObjectAlone());
        return;
    case ValueFlowNameKind::Getter:
    case ValueFlowNameKind::Setter:
        context.add(context.nodes().ofHost(ValueFlowHostNode::NamesOfAccessorsStoredThroughUnknownObject), only(context, universe.keyConstant(PropertyKeyID(name.payload))));
        return;
    case ValueFlowNameKind::GetterUnderUnknownKey:
    case ValueFlowNameKind::SetterUnderUnknownKey:
        context.add(context.nodes().ofHost(ValueFlowHostNode::NamesOfAccessorsStoredThroughUnknownObject), universe.unknownObjectAlone());
        return;
    default:
        return;
    }
}

bool isInSeenCode(ValueFlowRuleContext& context, FunctionID id)
{
    const Function& function = context.program().at(id);
    if (!isLibraryRole(function.role))
        return true;
    // Whoever asks is due again when this function arrives there, not when any function does.
    if (context.holdsAnyOf(only(context, context.universe().closureOf(id)), context.nodes().ofHost(ValueFlowHostNode::FunctionsOfSeenCode)))
        return true;
    // Asking for a module runs its body. A function of the module that has merely escaped is entered by native code, and no statement of the program leads to what it calls.
    return function.module && context.program().at(function.module).body == id
        && context.holdsAnyOf(only(context, context.universe().namespaceOf(function.module)), context.nodes().ofHost(ValueFlowHostNode::ModulesOfSeenCode));
}

void noteRequest(ValueFlowRuleContext& context, FunctionID requester, ModuleID module)
{
    if (isInSeenCode(context, requester))
        context.add(context.nodes().ofHost(ValueFlowHostNode::ModulesOfSeenCode), only(context, context.universe().namespaceOf(module)));
}

static bool isWrittenByProgram(const Program& program, const Module& module)
{
    switch (module.kind) {
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
        return !module.flags.contains(ModuleFlags::IsLibrary) && !isBundlerHelperModule(program, module);
    case ModuleKind::External:
        return true;
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The modules that a load by a name nobody knows may answer. It evaluates only what the program wrote.
// A file of the program with code that is in the image because another file names it by a constant. Data runs nothing and answers to its name.
static bool isInnerFile(const Program& program, ModuleID id)
{
    ModuleKind kind = program.at(id).kind;
    return kind != ModuleKind::External && kind != ModuleKind::Data && !std::ranges::contains(program.entries, id, &Entry::module);
}

bool mayAnswerToName(const Program& program, ModuleID id)
{
    const Module& module = program.at(id);
    // The helpers of the bundler answer to the name under which a file of the program imports them. What they import in turn has no name that a loader is asked for, and the table of the run time does not list it.
    if (isBundlerHelperModule(program, module))
        return isBundlerHelperNamedByProgram(program, id);
    if (module.runtimeModule && !program.builtins().at(module.runtimeModule).flags.contains(RuntimeModuleFlag::IsPublic))
        return false;
    return !isWrittenByProgram(program, module) || !isInnerFile(program, id);
}

static void forEachModuleLoadableByName(ValueFlowRuleContext& context, NOESCAPE const Invocable<void(ModuleID, bool isOfProgram)> auto& functor)
{
    const Program& program = context.program();
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        if (!mayAnswerToName(program, ModuleID(index)))
            continue;
        // A load under a name nobody knows brings nothing into the image and runs no module that nothing else runs. Of the program's files the name finds the entry points, as in an executable
        // that `bun build --compile` makes without --aot: Modules::refusesRequestFromHost() turns every other file of the image away. Of the library it finds a module that is in the image
        // because reached code names it by a constant. A native module has no code to run: its object is there whether or not anybody has asked for it.
        bool isOfProgram = isWrittenByProgram(program, module);
        if (module.body ? !context.isReached(module.body) : !isOfProgram && module.kind != ModuleKind::Runtime)
            continue;
        functor(ModuleID(index), isOfProgram);
    }
}

// process.getBuiltinModule() answers a module of the library or undefined, whatever name it is given. It runs no file of the program and nothing from outside the image.
static bool loadsOnlyLibraryModules(BuiltinID loader)
{
    return loader == Builtins::ProcessGetBuiltinModule;
}

static void requireLibraryModulesByName(ValueFlowRuleContext& context, ValueFlowNodeID result)
{
    raise(context, ValueFlowHostNode::LibraryModulesAreRequiredByName);
    context.connect(context.nodes().ofHost(ValueFlowHostNode::LibraryValuesLoadedByName), result);
}

static void handLibraryModulesToNativeCode(ValueFlowRuleContext& context)
{
    raise(context, ValueFlowHostNode::LibraryModulesAreRequiredByName);
    context.connect(context.nodes().ofHost(ValueFlowHostNode::LibraryValuesLoadedByName), context.nodes().ofHost(ValueFlowHostNode::HeldByNativeCode));
}

// A caller that nobody follows loads by name. What the program's own modules answer has escaped. What a module of the library answers is held.
static void handLoadedModulesToNativeCode(ValueFlowRuleContext& context)
{
    for (ValueFlowHostNode answers : { ValueFlowHostNode::RequiredValuesLoadedByName, ValueFlowHostNode::NamespacesLoadedByName })
        letModulesBeLoadedByName(context, answers);
    forEachModuleLoadableByName(context, [&](ModuleID module, bool isOfProgram) {
        ValueFlowNodeID hands = isOfProgram ? ValueFlowProgramNodes::escaped() : context.nodes().ofHost(ValueFlowHostNode::HeldByNativeCode);
        connect(context, requiredValueOf(context, module), hands);
        connect(context, namespaceOf(context, module), hands);
    });
}

// node:vm and ShadowRealm compile text too: a script or a function in the global scope of this realm, or code in a realm of its own, which has of this one what it is handed.
// Bun.Transpiler makes text of text. It runs code only for a macro, through a few lines of its own that import the macro's file, and the loader refuses a file that is not in the image.
static constexpr BuiltinID textEvaluatorsOfHost[] {
    Builtins::NodeVMBindingScript, Builtins::NodeVMBindingModule, Builtins::NodeVMBindingCompileFunction, Builtins::ShadowRealmPrototypeEvaluate, Builtins::BunTranspiler,
};

// The evaluators take text and nothing else, and the text finds the realm closed and locked like everybody else. Whatever else lets in code starts it in a way of its own, with a loader in its hands.
static UnseenCodeReach reachOfCodeBehind(const BuiltinTable& builtins, BuiltinID builtin)
{
    const Builtin& entry = builtins.at(builtin);
    bool runsOnlyText = entry.origin == BuiltinOrigin::Engine && entry.flags.contains(BuiltinFlag::EvaluatesText) && !entry.flags.contains(BuiltinFlag::RunsUnseenCode) && !builtins.unseenCodeOf(builtin);
    return runsOnlyText || std::ranges::contains(textEvaluatorsOfHost, builtins.canonicalID(builtin)) ? UnseenCodeReach::GlobalScope : UnseenCodeReach::Realm;
}

// `result` takes what the call answers: none where nobody reads it.
static void raiseEvent(ValueFlowRuleContext& context, BuiltinID builtin, UnseenCodeCause cause, ValueFlowNodeID result)
{
    if (cause != UnseenCodeCause::LoadsModuleByName) {
        letUnseenCodeRun(context, reachOfCodeBehind(context.program().builtins(), builtin));
        return;
    }
    for (ValueFlowHostNode answers : { ValueFlowHostNode::RequiredValuesLoadedByName, ValueFlowHostNode::NamespacesLoadedByName }) {
        letModulesBeLoadedByName(context, answers);
        context.connect(context.nodes().ofHost(answers), result);
    }
}

// The analysis follows a built-in that lets in unseen code as a value, from the read that names it, and never stops: a set that is given up still lists it. The event is raised where the value is applied.
// A read under a key nobody knows hands out no such built-in, no placeholder and no family of a type stands for one, and one that has come into unknown hands is a value like any other: a mention, no call.
// Should a callee that nobody knows be `eval` at run time, the run time refuses it in an image whose build saw no application of an evaluator.
void applyEventsOfCall(const ValueFlowStatement& statement, ValueFlowRuleContext& context, ClimbingSet callees)
{
    const ValueFlowUniverse& universe = context.universe();
    const BuiltinTable& builtins = context.program().builtins();
    auto isCalledBySeenCode = [&] {
        return !statement.function || isInSeenCode(context, statement.function);
    };
    if (context.meet(callees, universe.closures()) && isCalledBySeenCode())
        context.add(context.nodes().ofHost(ValueFlowHostNode::FunctionsOfSeenCode), context.restrict(callees, universe.closures()));

    context.forEachMember(context.restrict(callees, universe.builtinFunctions()), [&](ValueFlowObjectID callee) {
        BuiltinID builtin = builtins.canonicalID(universe.builtinOf(callee));
        std::optional<UnseenCodeCause> cause = eventOfCalling(builtins, builtin, context.refusesEvaluatedText());
        bool loadsMachineCode = std::ranges::contains(loadersOfMachineCode, builtin, &LoaderOfMachineCode::callee);
        if ((!cause && !loadsMachineCode) || !isCalledBySeenCode())
            return;
        // For the records: unseenCodeRecordsOf() names every built-in that has raised an event, and the loader of machine code, whose application is none.
        context.add(context.nodes().ofHost(ValueFlowHostNode::FunctionsOfSeenCode), only(context, callee));
        if (!cause)
            return;
        if (loadsOnlyLibraryModules(builtin))
            requireLibraryModulesByName(context, statement.result);
        else
            raiseEvent(context, builtin, *cause, statement.result);
    });
}

bool isWayBetweenModuleObjects(BuiltinID member)
{
    return member == Builtins::ModuleRequireMain || member == Builtins::ModuleObjectChildren || member == Builtins::ModuleObjectParent;
}

// None for a module that has not run, and for one of which the host makes no module object.
static ValueFlowObjectID moduleObjectOfEvaluatedModule(ValueFlowRuleContext& context, ModuleID id)
{
    const Module& module = context.program().at(id);
    if (module.kind != ModuleKind::CommonJs || !module.body || !module.exportsVariable || !context.isReached(module.body))
        return { };
    return context.universe().objectOf(Builtins::ModuleObject, id);
}

static void handOut(ValueFlowRuleContext& context, ValueFlowObjectID moduleObject)
{
    context.add(context.nodes().ofHost(ValueFlowHostNode::ModuleObjectsHandedOut), only(context, moduleObject));
}

void appendModuleObjectsAnsweredBy(ValueFlowRuleContext& context, BuiltinID entry, Vector<ValueFlowObjectID, 32>& objects)
{
    const ValueFlowProgramNodes& nodes = context.nodes();
    // The module object of a file that the host has loaded from outside the image, and one that the program has made with `new Module()`, is no module's.
    // `require.main` and process.mainModule are never one of those in an executable.
    bool answersMainModule = entry == Builtins::ModuleRequireMain || entry == Builtins::ProcessMainModule;
    if (!answersMainModule)
        objects.append(context.universe().objectOf(Builtins::ModuleObject));
    auto answer = [&](ValueFlowHostNode answers) {
        context.connect(nodes.ofHost(answers), nodes.ofHost(ValueFlowHostNode::ModuleObjectsHandedOut));
        context.forEachMember(context.valueOf(nodes.ofHost(answers)), [&](ValueFlowObjectID object) {
            objects.append(object);
        });
    };
    if (entry == Builtins::ModuleObjectChildren)
        return answer(ValueFlowHostNode::ChildrenOfModuleObjects);
    if (entry == Builtins::ModuleObjectParent)
        return answer(ValueFlowHostNode::ParentsOfModuleObjects);
    if (!answersMainModule)
        return;
    for (const Entry& entryPoint : context.program().entries) {
        if (ValueFlowObjectID moduleObject = moduleObjectOfEvaluatedModule(context, entryPoint.module)) {
            handOut(context, moduleObject);
            objects.append(moduleObject);
        }
    }
}

// Native code looks up no member of a built-in object of the realm that no entry declares, and a lookup by name through the unknown object finds the member in the table.
// Code that the build has not seen takes whatever the object holds, and so does whoever holds an object that belongs to one module.
// Such code has all of the language. Of the host it has what the program's own code names: the code of the library behind a member of a realm object that nobody names is not in the image,
// and whoever finds the member at run time gets the error that names it. So that it is found is no use of it, and nobody has handed it over: runUnseenCode() opens the host's objects where they are.
// Were they among what has escaped, each `unknown.send` and `unknown.channel` of the program would be a read of process.send and process.channel.
static void exposeMembersOf(ValueFlowRuleContext& context, ValueFlowObjectID holderObject, BuiltinID holder, Vector<BuiltinID, 4>& waysBetweenModuleObjectsAnswered)
{
    const ValueFlowUniverse& universe = context.universe();
    const BuiltinTable& builtins = context.program().builtins();
    ModuleID module = universe.moduleOf(holderObject);
    bool isHolderOfRealm = builtins.at(holder).scope == BuiltinScope::Realm;
    // What lies above the object is handed out with it.
    if (builtins.at(holder).prototypeLink == BuiltinPrototypeLink::Builtin)
        context.addUnlessHeld(ValueFlowProgramNodes::escaped(), universe.objectOf(builtins.at(holder).inheritsFrom));
    for (BuiltinID member : builtins.membersOf(holder)) {
        // A member whose application is an event never follows its holder. It is a value that the analysis follows from a read that names it, and it comes into unknown hands only where the analysis loses track of it.
        if (builtins.letsInUnseenCode(builtins.canonicalID(member)))
            continue;
        // What `module.exports` holds lies in a variable of the module, which the property shows: whoever holds the module object finds it there like any other property.
        if (builtins.canonicalID(member) == Builtins::ModuleObjectExports)
            continue;
        // The host answers undefined for a way to the other module objects in a program that names none. In any other, whoever holds the object finds there what the program finds.
        if (isWayBetweenModuleObjects(builtins.canonicalID(member))) {
            if (namesWaysToModuleObjects(context.program()) && !waysBetweenModuleObjectsAnswered.contains(builtins.canonicalID(member))) {
                waysBetweenModuleObjectsAnswered.append(builtins.canonicalID(member));
                Vector<ValueFlowObjectID, 32> answers;
                appendModuleObjectsAnsweredBy(context, builtins.canonicalID(member), answers);
                context.add(ValueFlowProgramNodes::escaped(), ClimbingSet(context.maker().of(answers.span())));
            }
            continue;
        }
        if (isHolderOfRealm && builtins.at(member).origin == BuiltinOrigin::Host)
            continue;
        if (!isObjectKind(builtins.at(member).kind)) {
            context.use(member, { BuiltinUse::Read, BuiltinUse::Written });
            continue;
        }
        context.use(member, BuiltinUse::Read);
        ValueFlowObjectID object = module ? universe.objectOf(member, module) : ValueFlowObjectID();
        context.addUnlessHeld(ValueFlowProgramNodes::escaped(), object ? object : universe.objectOf(member));
    }
}

static constexpr BuiltinID locationsOfLoaders[] {
    Builtins::ModuleRequireCache,
    Builtins::NodeModule_cache,
    Builtins::ModuleObjectRequire,
    Builtins::NodeModulePrototypeRequire,
    Builtins::NodeModuleWrap,
    Builtins::NodeModuleWrapper,
    Builtins::NodeModule_load,
    Builtins::NodeModule_resolveFilename,
};

bool isLocationOfLoader(BuiltinID member)
{
    return std::ranges::contains(locationsOfLoaders, member);
}

bool isFoundByNameAlone(BuiltinID member)
{
    return isLocationOfLoader(member) || member == Builtins::ProcessMainModule;
}

// No module of the image is in the module cache, so nothing that is done to the cache changes what a constant name loads or how often a body runs.
// A name nobody knows may find what the program has put there. The run time refuses a store to a function of the loader and to the table of extensions, whoever makes it.
void applyEventsOfStore(const ValueFlowStatement& statement, ValueFlowRuleContext& context, ClimbingSet receivers, PropertyKeyID)
{
    if (!statement.function || !context.contains(receivers, context.universe().objectOf(Builtins::ModuleCache)))
        return;
    // The loader's own code fills its tables.
    if (isInSeenCode(context, statement.function))
        context.add(context.nodes().ofHost(ValueFlowHostNode::RequiredValuesLoadedByName), unknownValue(context, ValueFlowUnknownSource::ContentsOfEscapedObject));
}

bool isGetterFoundWithoutItsName(const BuiltinTable& builtins, BuiltinID builtin)
{
    if (builtins.at(builtin).kind != BuiltinKind::Accessor || !builtins.moduleLoadsOf(builtin).empty())
        return false;
    // The built-in that a function implements also lists it among the functions that it calls.
    return std::ranges::all_of(builtins.calledFunctionsOf(builtin), [&](const BuiltinCalledFunction& called) {
        return std::ranges::contains(builtins.at(called.function).implements, builtin);
    });
}

// Code that is started with the loader itself in its hands is taken to get at the module object of every CommonJS module that has run.
static void exposeModuleObjects(ValueFlowRuleContext& context)
{
    for (uint32_t index = 0; index < context.program().modules.size(); ++index) {
        if (ValueFlowObjectID moduleObject = moduleObjectOfEvaluatedModule(context, ModuleID(index))) {
            handOut(context, moduleObject);
            context.add(ValueFlowProgramNodes::escaped(), only(context, moduleObject));
        }
    }
}

// process.mainModule starts in the global scope, so whoever reads globals by a name that the build does not see finds there what the program finds, the module object of an entry point:
// machine code that the program has loaded, a read under a key nobody knows, text that nobody has seen.
static void exposeModuleObjectsOfEntryPoints(ValueFlowRuleContext& context)
{
    for (const Entry& entryPoint : context.program().entries) {
        if (ValueFlowObjectID moduleObject = moduleObjectOfEvaluatedModule(context, entryPoint.module)) {
            handOut(context, moduleObject);
            context.add(ValueFlowProgramNodes::escaped(), only(context, moduleObject));
        }
    }
}

void runUsedBuiltins(const ValueFlowStatement&, ValueFlowRuleContext& context)
{
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    const BuiltinTable& builtins = program.builtins();

    ClimbingSet escaped = context.valueWithin(ClimbingSet(universe.builtinObjects()), ValueFlowProgramNodes::escaped());
    bool membersFollowHolders = isRaised(context, ValueFlowHostNode::UnseenCodeRuns);
    // What has escaped is held, and a call whose callee nobody knows calls it: see mayCallWhateverHasEscaped().
    bool callsWhateverHasEscaped = mayCallWhateverHasEscaped(context);
    ClimbingSet run = context.valueOf(context.nodes().ofHost(ValueFlowHostNode::RunBuiltins));
    // The code behind a built-in of the host is in the image for a use that names the built-in. That the object has come into unknown hands is no such use.
    // A function that the program has taken from its holder, by name (`const escape = Bun.escapeHTML`) or with all the others (`Object.assign(copied, Blob.prototype)`), is in those hands as a value
    // that the program handed over or keeps beside a value nobody knows: beside the unknown object no call lists it any more, so a call of a value nobody knows is a call of it.
    ClimbingSet takenFromHolders = context.valueOf(context.nodes().ofHost(ValueFlowHostNode::FunctionsOfHostTakenFromKnownHolders));
    Vector<ValueFlowObjectID, 64> escapedObjectsOfLanguage;
    Vector<ValueFlowObjectID, 16> calledFunctionsOfHost;
    Attachments attachments(context);
    context.forEachMember(escaped, [&](ValueFlowObjectID object) {
        BuiltinID builtin = universe.builtinOf(object);
        if (!builtin)
            return;
        bool isOfLanguage = builtins.at(builtin).origin != BuiltinOrigin::Host;
        if (!isOfLanguage && callsWhateverHasEscaped && context.contains(takenFromHolders, object))
            calledFunctionsOfHost.append(object);
        if (!isOfLanguage)
            return;
        escapedObjectsOfLanguage.append(object);
        if (callsWhateverHasEscaped || context.contains(run, object))
            attachments.addClassesAnsweredBy(builtin);
    });
    ClimbingSet escapedOfLanguage(context.maker().of(escapedObjectsOfLanguage.span()));
    ClimbingSet used = context.unite(context.valueOf(context.nodes().ofHost(ValueFlowHostNode::UsedBuiltins)), escapedOfLanguage);
    // A built-in that a statement has had as itself has escaped as a value that the program handed over: the handler of a proxy holds it, or an object in the hands of native code. Whoever has it calls it.
    run = context.unite(run, callsWhateverHasEscaped ? escapedOfLanguage : context.valueWithin(escapedOfLanguage, context.nodes().ofHost(ValueFlowHostNode::BuiltinsThatStatementsName)));
    run = context.unite(run, ClimbingSet(context.maker().of(calledFunctionsOfHost.span())));
    for (ValueFlowObjectID function : calledFunctionsOfHost)
        context.use(universe.builtinOf(function), BuiltinUse::Called);

    // A function that may be called is a callee that some call lists, or it has escaped or is no longer followed and a call of the placeholder is reached. That it was read runs nothing.
    context.forEachMember(used, [&](ValueFlowObjectID object) {
        BuiltinID builtin = universe.builtinOf(object);
        if (!builtin)
            return;
        attachments.addRowsOf(builtin);
        // A read of what is no object runs the code behind it. Evaluated text has of the host what the program names, so it may call a function that the program only reads.
        if (!isObjectKind(builtins.at(builtin).kind) || membersFollowHolders || context.contains(run, object))
            attachments.addCodeOf(builtin);
    });
    context.forEachMember(context.valueOf(context.nodes().ofHost(ValueFlowHostNode::WrittenBuiltins)), [&](ValueFlowObjectID object) {
        if (BuiltinID builtin = universe.builtinOf(object))
            attachments.addCodeOfStoresTo(builtin);
    });
    for (const RuntimeFunctionEntry& entry : program.runtimeFunctions) {
        if (!entry.function || !builtins.contains(entry.id))
            continue;
        for (BuiltinID builtin : builtins.at(entry.id).implements) {
            if ((membersFollowHolders && isGetterFoundWithoutItsName(builtins, builtin)) || context.contains(isObjectKind(builtins.at(builtin).kind) && !membersFollowHolders ? run : used, universe.objectOf(builtin)))
                attachments.addCode(entry.id);
        }
    }
    if (isRaised(context, ValueFlowHostNode::HostChoosesOperands))
        attachments.addStartupModules();
    attachments.enter();

    // Whoever reads a way to the module objects is answered what the requests of the build lead to, and that is followed like any other value: see appendModuleObjectsAnsweredBy().
    // process.mainModule is the one way that starts in the global scope. The host answers undefined there in a program that names no way.
    if (namesWaysToModuleObjects(program))
        exposeModuleObjectsOfEntryPoints(context);
    Vector<BuiltinID, 4> waysBetweenModuleObjectsAnswered;
    context.forEachMember(escaped, [&](ValueFlowObjectID object) {
        BuiltinID builtin = universe.builtinOf(object);
        if (!builtin)
            return;
        if (builtins.at(builtin).origin != BuiltinOrigin::Host)
            context.use(builtin, context.contains(run, object) ? OptionSet<BuiltinUse> { BuiltinUse::Escaped, BuiltinUse::Called } : OptionSet<BuiltinUse> { BuiltinUse::Escaped });
        if (membersFollowHolders || builtins.at(builtin).scope != BuiltinScope::Realm)
            exposeMembersOf(context, object, builtin, waysBetweenModuleObjectsAnswered);
    });
}

// One more caller without types, not a second world. It reaches the global scope: the global object escapes, and the rule of what has escaped lets everything happen to it.
// It names the modules of the library, and holds what those in the image export. It reaches no variable of a module, no closure and no object that nothing global leads to:
// the run time refuses it every name that leads to a file of the image, unless the program itself loads by a name nobody knows.
void runUnseenCode(const ValueFlowStatement&, ValueFlowRuleContext& context)
{
    if (!isRaised(context, ValueFlowHostNode::UnseenCodeRuns))
        return;

    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    const BuiltinTable& builtins = program.builtins();

    context.add(ValueFlowProgramNodes::escaped(), only(context, universe.objectOf(Builtins::GlobalThis)));
    // It reaches every object of the host through the global object and does to it what it likes.
    Vector<ValueFlowObjectID, 64> objectsOfHost;
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        const Builtin& entry = builtins.at(BuiltinID(index));
        if (entry.origin == BuiltinOrigin::Host && entry.scope == BuiltinScope::Realm && isObjectKind(entry.kind) && builtins.canonicalID(BuiltinID(index)) == BuiltinID(index))
            objectsOfHost.append(universe.objectOf(BuiltinID(index)));
    }
    if (isRaised(context, ValueFlowHostNode::UnseenCodeChangesRealm))
        letAnythingHappenTo(context, ClimbingSet(context.maker().of(objectsOfHost.span())));
    if (isRaised(context, ValueFlowHostNode::ModulesAreRequiredByName) || isRaised(context, ValueFlowHostNode::ModulesAreImportedByName))
        handLoadedModulesToNativeCode(context);
    else
        handLibraryModulesToNativeCode(context);

    // Such code evaluates text without the program naming an evaluator, and the interpreter calls functions of the library for that text.
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        if (builtins.at(BuiltinID(index)).flags.contains(BuiltinFlag::EvaluatesText))
            context.use(BuiltinID(index), { BuiltinUse::Escaped, BuiltinUse::Called });
    }
    letHostChooseOperands(context);
    // It makes calls that nothing describes.
    context.use(Builtins::UndeclaredNativeCode, BuiltinUse::Read);

    // Text in the global scope finds no module object unless the program names a way to one: runUsedBuiltins(). Nor does a module that stays outside the image: the host gives it a stand-in
    // for the file that has required it, and the module cache lists no module of the image.
    if (isRaised(context, ValueFlowHostNode::UnseenCodeChangesRealm))
        exposeModuleObjects(context);
}

void runModulesLoadedByName(const ValueFlowStatement&, ValueFlowRuleContext& context)
{
    bool areRequired = isRaised(context, ValueFlowHostNode::ModulesAreRequiredByName);
    bool areImported = isRaised(context, ValueFlowHostNode::ModulesAreImportedByName);
    bool libraryIsRequired = isRaised(context, ValueFlowHostNode::LibraryModulesAreRequiredByName);
    if (!areRequired && !areImported && !libraryIsRequired)
        return;

    const ValueFlowProgramNodes& nodes = context.nodes();
    forEachModuleLoadableByName(context, [&](ModuleID id, bool isOfProgram) {
        if (isOfProgram && !areRequired && !areImported)
            return;
        // Whoever names the module at run time is seen code.
        context.add(nodes.ofHost(ValueFlowHostNode::ModulesOfSeenCode), only(context, context.universe().namespaceOf(id)));
        // Only for a module without code: data, or a module that stays outside the image.
        if (isOfProgram)
            evaluateModule(context, id);
        if (areRequired) {
            connect(context, requiredValueOf(context, id), nodes.ofHost(ValueFlowHostNode::RequiredValuesLoadedByName));
            // The file that has required it lists it among its children.
            if (ValueFlowObjectID moduleObject = moduleObjectOfEvaluatedModule(context, id))
                context.add(nodes.ofHost(ValueFlowHostNode::ChildrenOfModuleObjects), only(context, moduleObject));
        }
        if (areImported)
            connect(context, namespaceOf(context, id), nodes.ofHost(ValueFlowHostNode::NamespacesLoadedByName));
        if (libraryIsRequired && !isOfProgram)
            connect(context, requiredValueOf(context, id), nodes.ofHost(ValueFlowHostNode::LibraryValuesLoadedByName));
    });

    // A native module has no code to run, so the host answers its object whether or not anything in the program imports the module. That it may be the answer is no use of it.
    const BuiltinTable& builtins = context.program().builtins();
    Vector<ValueFlowObjectID, 32> objectsOfNativeModules;
    for (uint32_t index = 0; index < builtins.moduleCount(); ++index) {
        const RuntimeModuleData& module = builtins.at(RuntimeModuleID(index));
        if (module.namespaceObject && module.flags.contains(RuntimeModuleFlag::IsPublic) && !module.flags.contains(RuntimeModuleFlag::HasSource))
            objectsOfNativeModules.append(context.universe().objectOf(module.namespaceObject));
    }
    ClimbingSet nativeModules(context.maker().of(objectsOfNativeModules.span()));
    if (areRequired)
        context.add(nodes.ofHost(ValueFlowHostNode::RequiredValuesLoadedByName), nativeModules);
    if (areImported)
        context.add(nodes.ofHost(ValueFlowHostNode::NamespacesLoadedByName), nativeModules);
    if (libraryIsRequired)
        context.add(nodes.ofHost(ValueFlowHostNode::LibraryValuesLoadedByName), nativeModules);

    enterLoaderOfHost(context);
}

void enterLoaderOfHost(ValueFlowRuleContext& context)
{
    for (RuntimeFunctionID loader : context.program().builtins().moduleLoaderFunctions())
        enterRuntimeFunction(context, loader, NativeCaller::Anybody);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
