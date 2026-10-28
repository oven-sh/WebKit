/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRulesModules.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTKnownClosures.h"
#include "AOTValueFlowBuiltinTables.h"
#include "AOTValueFlowBuiltins.h"
#include "AOTValueFlowHeapTables.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include "AOTValueFlowRules.h"
#include "AOTValueFlowRulesHeap.h"
#include <algorithm>
#include <utility>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

void describeHost(const Program& program, ValueFlowBody& body)
{
    RELEASE_ASSERT(body.nodes.size() == body.thrown() + 1);
    for (unsigned index = 0; index < numberOfValueFlowHostNodes; ++index)
        body.nodes.append({ ValueFlowBodyNodeKind::Merge, { }, 0 });

    ValueFlowBody::Constant moduleObjects;
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        if (module.kind == ModuleKind::CommonJs && module.body && module.exportsVariable)
            moduleObjects.objects.append({ ValueFlowObjectKind::ModuleBuiltin, program.builtins().canonicalID(Builtins::ModuleObject).index(), ModuleID(index) });
    }
    if (!moduleObjects.isEmpty())
        body.constants.append(WTF::move(moduleObjects));

    for (ValueFlowStatementKind kind : { ValueFlowStatementKind::HostRoots, ValueFlowStatementKind::UsedBuiltins, ValueFlowStatementKind::UnseenCode, ValueFlowStatementKind::ModulesLoadedByName })
        body.statements.append({ { }, { }, 0, 0, { }, { }, kind });
}

static void passFromNativeCode(ValueFlowRuleContext& context, FunctionID function, const Function& callee, unsigned index, NativeCaller caller, ValueSetID unknown)
{
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    ValueFlowNodeID parameter = context.nodes().entryParameter(function, index);
    bool isRestParameter = callee.has(FunctionFlags::HasRestParameter) && index == callee.firstParameterIndex() + callee.numberOfPositionalParameters();
    if (ValueFlowNodeID passedAtAnyIndex = isRestParameter ? context.restArgumentEntered(function, numberOfGroupedArguments) : ValueFlowNodeID())
        context.add(passedAtAnyIndex, unknown);
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
    bool hasType = declared && declared->written();
    bool isAssumedByEveryEntrypoint = hasType && !declared->site && !declared->isExpected();
    bool isKeptByHost = hasType && caller == NativeCaller::NamedByTable && callee.entryParameterKind(index) == EntryParameter::Parameter && declared->site && !declared->has(PositionFlags::IsFromCompiler) && callee.has(FunctionFlags::IsChecked);
    ValueSetID mask = isAssumedByEveryEntrypoint || isKeptByHost ? universe.maskOf(context.maker(), program, program.at(declared->expected().predicted())) : ValueSets::anything;
    if (mask == ValueSets::anything) {
        context.add(parameter, unknown);
        return;
    }

    const Type& type = program.at(declared->expected().predicted());
    context.add(parameter, context.maker().intersect(universe.anyPrimitive(), mask));
    if (!type.hasObjects())
        return;
    if (universe.namesProgramObjectsAlone(program, type))
        context.connect(context.masked(ValueFlowProgramNodes::escaped(), context.maker().intersect(universe.familyObjectsOf(context.maker(), program, type), mask)), parameter);
    else
        context.add(parameter, context.maker().intersect(unknown, universe.objects()));
}

static bool isLibraryRole(FunctionRole role)
{
    return role == FunctionRole::Library || role == FunctionRole::Model;
}

// A module that a function of the library answers with stays found by name alone.
static void handOutOfFunction(ValueFlowRuleContext& context, const Function& callee, ValueFlowNodeID values)
{
    if (!isLibraryRole(callee.role)) {
        carryOut(context, values);
        return;
    }
    ValueSetID modules = context.universe().holdersOfLibraryExports();
    carryOut(context, context.masked(values, ValueSets::complement(modules)));
    context.connect(context.masked(values, modules), ValueFlowProgramNodes::escaped());
}

void enterFromNativeCode(ValueFlowRuleContext& context, FunctionID function, NativeCaller caller)
{
    const ValueFlowUniverse& universe = context.universe();
    const ValueFlowProgramNodes& nodes = context.nodes();
    const Function& callee = context.program().at(function);
    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
    ValueSetID unknown = unknownValue(context, ValueFlowUnknownSource::ParameterOfEscapedFunction);

    context.reach(function);
    for (unsigned index = 0; index < callee.numberOfEntryParameters(); ++index) {
        if (callee.calleeParameterIndex() == index)
            context.add(nodes.entryParameter(function, index), only(context, universe.closureOf(function)));
        else if (callee.entryParameterKind(index) != EntryParameter::Capture)
            passFromNativeCode(context, function, callee, index, caller, unknown);
    }
    if (callee.kind == FunctionKind::Plain && callee.constructedClass && callee.constructorForm() != ConstructorForm::NotConstructor && callee.thisParameterIndex()) {
        ClimbingSet instance = only(context, universe.instanceOf(callee.constructedClass));
        context.connect(context.lookup(only(context, universe.closureOf(function)), ValueFlowName::property(context.program().keyOf(BuiltinNames::prototype))), context.written(instance, prototypeLinkName));
        context.add(nodes.entryParameter(function, *callee.thisParameterIndex()), instance);
        context.add(escaped, instance);
    }
    if (const auto* summary = nodes.resultSummaryOf(function))
        handOutOfFunction(context, callee, summary->ownResult);
    else {
        for (unsigned index = 0; index < callee.results.size(); ++index)
            handOutOfFunction(context, callee, nodes.result(function, index));
    }
    if (isLibraryRole(callee.role))
        context.connect(nodes.thrown(function), escaped);
    else
        carryOut(context, nodes.thrown(function));
    if (!callee.isUnsplitCoroutine() || !callee.has(FunctionFlags::IsGenerator))
        return;
    context.add(escaped, only(context, universe.coroutineResultOf(function)));
    if (isLibraryRole(callee.role))
        context.connect(nodes.yielded(function), escaped);
    else
        carryOut(context, nodes.yielded(function));
    context.add(nodes.sent(function), unknown);
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

bool isHostFlagRaised(ValueFlowRuleContext& context, ValueFlowHostNode flag)
{
    return context.contains(context.valueOf(context.nodes().ofHost(flag)), ValueFlowUniverse::unknownObject());
}

void raiseHostFlag(ValueFlowRuleContext& context, ValueFlowHostNode flag)
{
    context.add(context.nodes().ofHost(flag), context.leavesOutSomeUnknownSource() ? context.universe().undefinedValue() : context.universe().unknownObjectAlone());
}

void letUnseenCodeRun(ValueFlowRuleContext& context, UnseenCodeReach reach)
{
    raiseHostFlag(context, ValueFlowHostNode::UnseenCodeRuns);
    if (reach != UnseenCodeReach::GlobalScope)
        raiseHostFlag(context, ValueFlowHostNode::UnseenCodeHasLoader);
    if (reach == UnseenCodeReach::Realm)
        raiseHostFlag(context, ValueFlowHostNode::UnseenCodeChangesRealm);
}

void letAnythingHappenTo(ValueFlowRuleContext& context, ClimbingSet escaped)
{
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet objects = context.restrict(escaped, ValueSets::complement(universe.lockedObjects()));
    if (!isHostFlagRaised(context, ValueFlowHostNode::UnseenCodeChangesRealm))
        objects = context.restrict(objects, ValueSets::complement(universe.objectsOfRealm()));
    ValueSetID unknown = unknownValue(context, ValueFlowUnknownSource::ContentsOfEscapedObject);
    ValueSetID unknownObject = context.maker().intersect(unknown, universe.objects());
    context.mark(objects, OptionSet<ObjectMutation>::all());
    // No statement of the program writes through a set that holds a mark, and no read asks for one: what the program lays on an object and what nobody knows never share a node, whichever objects escape together.
    auto throughReceiversNobodyKnows = [&](ClimbingSet receivers) {
        return context.unite(receivers, ClimbingSet(only(context, ValueFlowUniverse::markOfMemberLeftOut())));
    };
    ClimbingSet receivers = throughReceiversNobodyKnows(objects);
    context.add(context.written(receivers, { ValueFlowNameKind::UnknownKey, 0 }), unknown);
    context.add(context.written(receivers, { ValueFlowNameKind::InternalFields, 0 }), unknown);
    context.add(context.written(receivers, { ValueFlowNameKind::GetterUnderUnknownKey, 0 }), unknownObject);
    context.add(context.written(receivers, { ValueFlowNameKind::SetterUnderUnknownKey, 0 }), unknownObject);
    context.add(context.written(throughReceiversNobodyKnows(context.restrict(objects, ValueSets::complement(universe.objectsWithLockedPrototypeLink()))), { ValueFlowNameKind::PrototypeLink, 0 }), unknown);
}

void letUndeclaredNativeCodeRun(ValueFlowRuleContext& context)
{
    FunctionID caller = context.functionOfStatement();
    if (!caller || !isInSeenCode(context, caller))
        return;
    context.use(Builtins::UndeclaredNativeCode, BuiltinUse::Read);
    letHostChooseOperands(context);
}

void letModulesBeLoadedByName(ValueFlowRuleContext& context, ValueFlowHostNode answers)
{
    RELEASE_ASSERT(answers == ValueFlowHostNode::RequiredValuesLoadedByName || answers == ValueFlowHostNode::NamespacesLoadedByName);
    raiseHostFlag(context, answers == ValueFlowHostNode::RequiredValuesLoadedByName ? ValueFlowHostNode::ModulesAreRequiredByName : ValueFlowHostNode::ModulesAreImportedByName);
    if (context.refusedCode().codeOutsideImage)
        return;
    raiseHostFlag(context, ValueFlowHostNode::CodeOutsideImageIsLoaded);
    letUnseenCodeRun(context, UnseenCodeReach::Loader);
    context.add(ValueFlowProgramNodes::escaped(), context.universe().holdersOfLibraryExports());
    context.add(context.nodes().ofHost(answers), unknownValue(context, ValueFlowUnknownSource::UnseenCode));
}

static void enterRuntimeFunction(ValueFlowRuleContext& context, RuntimeFunctionID id, NativeCaller caller)
{
    if (FunctionID function = context.libraryFunctionToEnter(id))
        enterFromNativeCode(context, function, caller);
}

static ValueFlowSources memberOf(ValueFlowRuleContext& context, const ValueFlowSources& object, StringID name)
{
    std::optional<PropertyKeyID> key = context.program().types.find({ .kind = PropertyKeyKind::Text, .text = name });
    return { { context.lookup(valueOf(context, object), key ? ValueFlowName::property(context.program(), *key) : everyPropertyName) }, { } };
}

static std::optional<ValueFlowSources> namedExportOf(ValueFlowRuleContext& context, ModuleID id, StringID name)
{
    const Program& program = context.program();
    const Module& module = program.at(id);
    ValueFlowSources unknown { { }, ClimbingSet(unknownValue(context, ValueFlowUnknownSource::DataFromOutside)) };

    auto found = std::ranges::find(module.exports, name, &ModuleExport::name);
    if (found != module.exports.end())
        return found->kind == ModuleExportKind::Indirect && program.at(found->module).kind == ModuleKind::External ? unknown : exportedValueOf(context, *found);

    if (module.exportRule != SyntheticExportRule::None || module.exportsVariable) {
        bool isDefault = name == program.stringOf(BuiltinNames::defaultKeyword);
        ValueFlowSources value = isDefault ? requiredValueOf(context, id) : ValueFlowSources();
        if (!isDefault || module.exportRule != SyntheticExportRule::CommonJsIgnoringAnnotation)
            value.nodes.appendVector(memberOf(context, requiredValueOf(context, id), name).nodes);
        return value;
    }

    if (module.kind != ModuleKind::SourceText) {
        connect(context, namespaceOf(context, id), ValueFlowProgramNodes::escaped());
        return unknown;
    }
    bool listsEveryExport = !module.requests.containsIf([](const ModuleRequest& request) {
        return request.isDynamicStarExport;
    });
    // A star passes on every name but `default`.
    if (listsEveryExport || name == program.stringOf(BuiltinNames::defaultKeyword))
        return std::nullopt;
    return memberOf(context, namespaceOf(context, id), name);
}

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

static bool mayBeServerConfiguration(ValueFlowRuleContext& context, const ValueFlowSources& value)
{
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet configuration = valueOf(context, value);
    auto found = [&](BuiltinName name) {
        return context.valueOf(context.lookup(configuration, ValueFlowName::property(program, program.keyOf(name))));
    };
    auto hasHandler = [&] {
        if (context.meet(configuration, universe.placeholdersAlone()))
            return true;
        for (BuiltinName name : { BuiltinNames::fetch, BuiltinNames::then }) {
            ClimbingSet function = found(name);
            if (context.meet(function, universe.callables()) || context.meet(function, universe.placeholdersAlone()))
                return true;
        }
        ValueFlowObjectID missing[] { ValueFlowUniverse::primitive(PrimitiveSet::Undefined), ValueFlowUniverse::primitive(PrimitiveSet::Null) };
        ValueSetID present = ValueSets::complement(context.maker().of(std::span<const ValueFlowObjectID>(missing)));
        return context.meet(found(BuiltinNames::app), present) || context.meet(found(BuiltinNames::routes), present);
    };
    if (!hasHandler())
        return false;
    // A server has `fetch` too: the host reads `stop` to tell one from a configuration.
    found(BuiltinNames::stop);
    return true;
}

static bool mayHold(const BuiltinCondition* condition)
{
    return !condition || !condition->buildAdmitsRuntimeCode;
}

static bool mayHold(ValueFlowRuleContext& context, BuiltinID builtin, const BuiltinCondition* condition)
{
    if (!condition)
        return true;
    if (!mayHold(condition))
        return false;
    if (condition->objectNobodyKnowsIsWritten)
        return isHostFlagRaised(context, ValueFlowHostNode::ObjectNobodyKnowsIsWritten) || mayUnknownCodeWrite(context);

    const BuiltinTable& builtins = context.program().builtins();
    ValueFlowObjectID objectOfBuiltin = context.universe().objectOf(builtin);
    ClimbingSet object = only(context, objectOfBuiltin);
    bool isRun = context.holdsAnyOf(object, context.nodes().ofHost(ValueFlowHostNode::RunBuiltins));
    if (!condition->calledOn && (isRun || mayCallWhateverHasEscaped(context)) && context.holdsAnyOf(object, ValueFlowProgramNodes::escaped()))
        return true;

    auto holdsSomething = [&](ValueFlowCellName cell) {
        return context.meet(context.valueOf(context.read(object, cell)), ValueSets::anything);
    };
    if (isObjectKind(builtins.at(builtin).kind)) {
        if (!isRun)
            return false;
        if (condition->callRunsOtherCode && !holdsSomething(cellOfCallsThatRunOtherCode(builtins)))
            return false;
        if (condition->firstText() && !holdsSomething(ValueFlowCellName { ValueFlowNameKind::Slot, builtins.slotCount() + condition->firstText().index() }))
            return false;
    }
    return !condition->hasProperty || holdsSomething(ValueFlowCellName { ValueFlowNameKind::Slot, builtins.slotCount() + condition->hasProperty.index() });
}

bool usesFeatureAmong(ValueFlowRuleContext& context, ClimbingSet entries, const BitVector& features)
{
    const BuiltinTable& builtins = context.program().builtins();
    bool uses = false;
    context.forEachMember(context.valueWithin(entries, context.nodes().ofHost(ValueFlowHostNode::UsedBuiltins)), [&](ValueFlowObjectID entry) {
        BuiltinID builtin = builtins.canonicalID(context.universe().builtinOf(entry));
        uses = uses || std::ranges::any_of(builtins.featureUsesOf(builtin), [&](const BuiltinFeatureUse& line) {
            return features.get(static_cast<size_t>(line.feature)) && mayHold(context, builtin, line.condition);
        });
    });
    return uses;
}

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
        ValueFlowSources answer = requiredValueOf(context, id);
        connect(context, answer, escaped);
        context.connect(context.readWhatWasWritten(withSourcesOfCopies(context, context.restrict(valueOf(context, answer), context.universe().knownObjects())), everyPropertyName), escaped);
        return;
    }
    for (size_t index = 0; index < load.exports.size(); ++index) {
        BuiltinName name = load.exports[index];
        auto found = std::ranges::find(module.exports, program.stringOf(name), &ModuleExport::name);
        bool isListed = found != module.exports.end() && (found->kind != ModuleExportKind::Variable || found->variable);
        if (!isListed) {
            use(memberOf(context, requiredValueOf(context, id), program.stringOf(name)), index);
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
            if (program.at(found->module).kind != ModuleKind::External)
                use(exportedValueOf(context, *found), index);
            break;
        }
    }
}

struct Attachments {
    void addRowsOf(BuiltinID builtin)
    {
        builtin = builtins.canonicalID(builtin);
        if (visitedBuiltins.set(builtin.index()))
            return;
        for (const BuiltinCalledFunction& called : builtins.calledFunctionsOf(builtin)) {
            if (mayHold(context, builtin, called.condition))
                add(called);
        }
        for (const BuiltinModuleLoad& load : builtins.moduleLoadsOf(builtin)) {
            if (mayHold(context, builtin, load.condition))
                loads.append(&load);
        }
    }

    void addCodeOf(BuiltinID builtin, bool isStoredTo = false)
    {
        for (const BuiltinOverload& overload : builtins.overloadsOf(builtins.canonicalID(builtin))) {
            RuntimeFunctionID function = overload.code.implementation.function;
            if (function && (!isStoredTo || overload.mode == BuiltinCallMode::Set))
                addCode(function);
        }
    }

    void addClassesAnsweredBy(BuiltinID builtin)
    {
        builtin = builtins.canonicalID(builtin);
        for (const BuiltinOverload& overload : builtins.overloadsOf(builtin)) {
            addClassesIn(overload.result.type);

            bool isCall = overload.mode == BuiltinCallMode::Call || overload.mode == BuiltinCallMode::Construct;
            bool runsOtherCode = overload.effects.capability == BuiltinCapability::JS && !overload.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode);
            bool answersAnything = builtins.at(overload.result.type).flags.containsAny({ BuiltinTypeFlag::IsAnything, BuiltinTypeFlag::HasAnyObject });
            if (builtins.at(builtin).origin == BuiltinOrigin::Host && (isCall || overload.isUndeclared()) && runsOtherCode && answersAnything)
                addRowsOf(BuiltinClasses::Promise);
        }
    }

    void addClassesIn(BuiltinTypeID type)
    {
        if (visitedTypes.set(type.index()))
            return;
        for (const BuiltinTypePart& part : builtins.at(type).parts) {
            if (part.builtinClass)
                addRowsOf(part.builtinClass);
            for (const BuiltinSlotContents& contents : part.contents)
                addClassesIn(contents.type);
        }
    }

    void addRowsOf(BuiltinClassID builtinClass)
    {
        for (BuiltinClassID current = builtinClass; current && !visitedClasses.set(current.index()); current = builtins.at(current).parent) {
            for (BuiltinID object : { builtins.at(current).constructor, builtins.at(current).prototype }) {
                if (object)
                    addRowsOf(object);
            }
        }
    }

    void addStartupModules()
    {
        for (const BuiltinModuleLoad& load : builtins.startupLoads())
            loads.append(&load);
        std::span<const BuiltinCalledFunction> rows = builtins.startupCalledFunctions();
        for (RuntimeFunctionID function : builtins.functionsCalledWithStartupLoads()) {
            auto row = std::ranges::find(rows, function, &BuiltinCalledFunction::function);
            if (row != rows.end())
                add(*row);
            else
                add(function);
        }
        hostMethodsMayRun = true;
    }

    void add(RuntimeFunctionID function) { functions.set(function.index()); }

    void add(const BuiltinCalledFunction& row)
    {
        if (row.uses.areListed)
            rowsListingUses.append(&row);
        else
            add(row.function);
    }

    void addCode(RuntimeFunctionID function)
    {
        if (isEnteredByCallsAlone(context.program(), function))
            return;
        add(function);
        functionsBehindBuiltins.set(function.index());
    }

    void enter()
    {
        for (size_t function : functions)
            enterRuntimeFunction(context, RuntimeFunctionID(static_cast<uint32_t>(function)), functionsBehindBuiltins.get(function) ? NativeCaller::Anybody : NativeCaller::NamedByTable);
        for (size_t function : functionsBehindBuiltins) {
            if (const RuntimeFunctionEntry* entry = findRuntimeFunction(context.program(), RuntimeFunctionID(static_cast<uint32_t>(function))); entry && entry->function)
                context.add(context.nodes().ofHost(ValueFlowHostNode::FunctionsOfSeenCode), only(context, context.universe().closureOf(entry->function)));
        }
        bool textFindsFunctions = isHostFlagRaised(context, ValueFlowHostNode::UnseenCodeRuns);
        for (const BuiltinCalledFunction* row : rowsListingUses) {
            FunctionID function = context.libraryFunctionToEnter(row->function);
            if (!function)
                continue;
            ClimbingSet closure = only(context, context.universe().closureOf(function));
            applyHostUses(row->builtin ? row->builtin : Builtins::UndeclaredNativeCode, { { }, context.maker().of(context.universe().closureOf(function)), ValueSets::anything }, row->uses, context);
            if (textFindsFunctions)
                context.add(ValueFlowProgramNodes::escaped(), closure);
        }
        for (const BuiltinModuleLoad* load : loads)
            loadRuntimeModule(context, *load);
        if (!hostMethodsMayRun)
            return;
        for (const BuiltinHostMethodCall& methodCall : builtins.hostMethodCalls())
            callFromHost(Builtins::UndeclaredNativeCode, { nodeReadFromNativeCell(context, methodCall.nativeCell), ValueSets::nothing, ValueSets::anything }, *methodCall.call, context);
    }

    ValueFlowRuleContext& context;
    const BuiltinTable& builtins;
    BitVector visitedBuiltins { };
    BitVector visitedClasses { };
    BitVector visitedTypes { };
    BitVector functions { };
    BitVector functionsBehindBuiltins { };
    Vector<const BuiltinModuleLoad*, 16> loads { };
    Vector<const BuiltinCalledFunction*, 16> rowsListingUses { };
    bool hostMethodsMayRun { false };
};

static void followWhatNativeCodeHolds(ValueFlowRuleContext& context)
{
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    ValueFlowNodeID heldNode = context.nodes().ofHost(ValueFlowHostNode::HeldByNativeCode);
    ValueFlowNodeID escapedNode = ValueFlowProgramNodes::escaped();
    ValueSetID objectsOfProgram = context.maker().intersect(universe.knownObjects(), ValueSets::complement(universe.builtinObjects()));
    ClimbingSet all = context.valueOf(heldNode);
    ClimbingSet held = context.restrict(all, objectsOfProgram);
    held = context.unite(held, context.valueWithin(ClimbingSet(universe.objectsOfLibraryFoundByNameAlone()), heldNode));
    if (context.contains(all, ValueFlowUniverse::untrackedObject()))
        held = context.unite(held, context.valueWithin(ClimbingSet(objectsOfProgram), ValueFlowProgramNodes::lost()));
    if (!context.meet(held, ValueSets::anything))
        return;

    for (ValueFlowNameKind kind : { ValueFlowNameKind::EveryProperty, ValueFlowNameKind::EveryGetter, ValueFlowNameKind::EverySetter, ValueFlowNameKind::PrototypeLink, ValueFlowNameKind::CopiedFrom })
        context.connect(context.readWhatWasWritten(held, { kind, 0 }), heldNode);
    context.add(heldNode, objectsHeldFromCreation(context, held));
    context.mark(held, ObjectMutation::ReachNativeCode);

    ClimbingSet unknownObject(universe.unknownObjectAlone());
    constexpr std::pair<ValueFlowNameKind, ValueFlowNameKind> foundAndWrittenNames[] {
        { ValueFlowNameKind::EveryProperty, ValueFlowNameKind::UnknownKey },
        { ValueFlowNameKind::EveryGetter, ValueFlowNameKind::GetterUnderUnknownKey },
        { ValueFlowNameKind::EverySetter, ValueFlowNameKind::SetterUnderUnknownKey },
        { ValueFlowNameKind::PrototypeLink, ValueFlowNameKind::PrototypeLink },
    };
    for (auto [found, written] : foundAndWrittenNames)
        context.connect(context.readWhatWasWritten(unknownObject, { found, 0 }), context.written(held, { written, 0 }));
    for (ObjectMutation mutation : OptionSet<ObjectMutation>::all()) {
        if (context.meet(context.markedAmong(unknownObject, mutation), ValueSets::anything))
            context.mark(held, mutation);
    }
    if (mayUnknownCodeWrite(context))
        letAnythingHappenTo(context, held);

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
    ValueFlowNodeID functionsOfSeenCode = context.nodes().ofHost(ValueFlowHostNode::FunctionsOfSeenCode);
    context.forEachMember(context.membersNewToThread(context.restrict(context.valueOf(functionsOfSeenCode), universe.closures()), 2), [&](ValueFlowObjectID closure) {
        FunctionID id = universe.closureFunction(closure);
        const Function& function = program.at(id);
        if (!isLibraryRole(function.role))
            return;
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
        context.add(context.nodes().ofHost(ValueFlowHostNode::ModulesOfSeenCode), only(context, context.universe().namespaceOf(entry.module)));
        evaluateModule(context, entry.module);
        if (program.at(entry.module).kind == ModuleKind::CommonJs)
            namespaceOf(context, entry.module);
        for (const HostRead& read : entry.hostReads) {
            std::optional<ValueFlowSources> value = namedExportOf(context, entry.module, read.exportName);
            if (value && !read.builtin)
                connect(context, *value, ValueFlowProgramNodes::escaped());
            else if (value && (program.builtins().canonicalID(read.builtin) != Builtins::BunServe || mayBeServerConfiguration(context, *value)))
                handToBuiltin(context, read, *value);
        }
        std::optional<ValueFlowSources> then = namedExportOf(context, entry.module, program.stringOf(BuiltinNames::then));
        if (!then)
            continue;
        ClimbingSet found = valueOf(context, *then);
        if (!context.meet(found, universe.callables()) && !context.contains(found, ValueFlowUniverse::unknownObject()))
            continue;
        ValueFlowSources unknown { { }, ClimbingSet(unknownValue(context, ValueFlowUnknownSource::DataFromOutside)) };
        for (const HostRead& read : entry.hostReads) {
            if (read.builtin)
                handToBuiltin(context, read, unknown);
        }
    }
}

static bool loadsOnlyLibraryModules(BuiltinID loader)
{
    return loader == Builtins::ProcessGetBuiltinModule;
}

bool refusesEveryText(const BuiltinTable& builtins, BuiltinID builtin, RefusedCode refused)
{
    const Builtin& entry = builtins.at(builtin);
    if (entry.origin == BuiltinOrigin::Engine)
        return refused.evaluatedText && entry.flags.contains(BuiltinFlag::EvaluatesText);
    return refused.hostEvaluatedText && (entry.flags.contains(BuiltinFlag::EvaluatesText) || builtins.has(BuiltinMark::EvaluatesTextInGlobalScope, builtin));
}

std::optional<UnseenCodeCause> eventOfCalling(const BuiltinTable& builtins, BuiltinID builtin, RefusedCode refused)
{
    const Builtin& entry = builtins.at(builtin);
    if (entry.flags.contains(BuiltinFlag::LoadsModuleByName))
        return refused.codeOutsideImage || loadsOnlyLibraryModules(builtin) ? UnseenCodeCause::LoadsModuleByName : UnseenCodeCause::LoadsCodeByName;
    if (refusesEveryText(builtins, builtin, refused))
        return std::nullopt;
    if (entry.flags.contains(BuiltinFlag::EvaluatesText))
        return UnseenCodeCause::EvaluatesText;
    const BuiltinUnseenCode* unseenCode = builtins.unseenCodeOf(builtin);
    if (unseenCode && unseenCode->condition && unseenCode->condition->hasProperty)
        return std::nullopt;
    if (unseenCode ? mayHold(unseenCode->condition) : entry.flags.contains(BuiltinFlag::RunsUnseenCode))
        return UnseenCodeCause::RunsUnseenCode;
    return std::nullopt;
}

static bool someCallPassesPropertyThatRunsUnseenCode(ValueFlowRuleContext& context, BuiltinID builtin)
{
    const BuiltinTable& builtins = context.program().builtins();
    const BuiltinUnseenCode* row = builtins.unseenCodeOf(builtin);
    if (!row || !row->condition || !row->condition->hasProperty)
        return false;
    ValueFlowCellName cell { ValueFlowNameKind::Slot, builtins.slotCount() + row->condition->hasProperty.index() };
    return context.meet(context.valueOf(context.read(only(context, context.universe().objectOf(builtin)), cell)), ValueSets::anything);
}

bool isInSeenCode(ValueFlowRuleContext& context, FunctionID id)
{
    const Function& function = context.program().at(id);
    if (!isLibraryRole(function.role))
        return true;
    if (context.holdsAnyOf(only(context, context.universe().closureOf(id)), context.nodes().ofHost(ValueFlowHostNode::FunctionsOfSeenCode)))
        return true;
    return function.module && context.program().at(function.module).body == id
        && context.holdsAnyOf(only(context, context.universe().namespaceOf(function.module)), context.nodes().ofHost(ValueFlowHostNode::ModulesOfSeenCode));
}

// No node lists what has escaped beside a value nobody knows. Where such a value is called, it may be a function that the program has taken off a module of the library.
void letSeenCodeCallEscapedFunctionsAmong(ValueFlowRuleContext& context, ValueFlowNodeID exportsThatProgramHasTaken)
{
    if (!mayCallWhateverHasEscaped(context))
        return;
    ClimbingSet functions = context.valueWithin(ClimbingSet(context.universe().closures()), exportsThatProgramHasTaken);
    context.add(context.nodes().ofHost(ValueFlowHostNode::FunctionsOfSeenCode), context.valueWithin(functions, ValueFlowProgramNodes::escaped()));
}

void noteRequest(ValueFlowRuleContext& context, FunctionID requester, ModuleID module)
{
    if (isInSeenCode(context, requester))
        context.add(context.nodes().ofHost(ValueFlowHostNode::ModulesOfSeenCode), only(context, context.universe().namespaceOf(module)));
    if (!isLibraryRole(context.program().at(requester).role))
        context.add(context.nodes().ofHost(ValueFlowHostNode::LibraryModulesRequestedByProgram), context.universe().holdersOfExportsOf(module));
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

bool mayAnswerToName(const Program& program, ModuleID id)
{
    const Module& module = program.at(id);
    if (isBundlerHelperModule(program, module))
        return isBundlerHelperNamedByProgram(program, id);
    if (module.runtimeModule && !program.builtins().at(module.runtimeModule).flags.contains(RuntimeModuleFlag::IsPublic))
        return false;
    return !isWrittenByProgram(program, module) || module.kind == ModuleKind::External || module.kind == ModuleKind::Data || std::ranges::contains(program.entries, id, &Entry::module);
}

bool isAskedOfHostByName(const Module& module)
{
    return module.kind == ModuleKind::RuntimeSource && !module.flags.contains(ModuleFlags::IsLoadedByImportOnly);
}

static void forEachModuleLoadableByName(ValueFlowRuleContext& context, NOESCAPE const Invocable<void(ModuleID, bool isOfProgram)> auto& functor)
{
    const Program& program = context.program();
    const ValueFlowHeapTables& tables = context.universe().builtinTables().heap();
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        if (!tables.mayAnswerToName(ModuleID(index)))
            continue;
        bool isOfProgram = isWrittenByProgram(program, module);
        if (module.body ? !context.isReached(module.body) && !isAskedOfHostByName(module) : !isOfProgram && module.kind != ModuleKind::Runtime)
            continue;
        functor(ModuleID(index), isOfProgram);
    }
}

static void handLibraryModulesTo(ValueFlowRuleContext& context, ValueFlowNodeID hands)
{
    raiseHostFlag(context, ValueFlowHostNode::LibraryModulesAreRequiredByName);
    context.connect(context.nodes().ofHost(ValueFlowHostNode::LibraryValuesLoadedByName), hands);
}

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

// Text has import() and process.getBuiltinModule() and no require of a file. The loader answers it with an entry point under any name,
// and with a data file of the image only where the program itself loads files by name.
static void handLoadableModulesToText(ValueFlowRuleContext& context, bool filesOfProgramAreLoadedByName)
{
    handLibraryModulesTo(context, context.nodes().ofHost(ValueFlowHostNode::HeldByNativeCode));
    forEachModuleLoadableByName(context, [&](ModuleID module, bool isOfProgram) {
        if (!isOfProgram || (context.program().at(module).kind == ModuleKind::Data && !filesOfProgramAreLoadedByName))
            return;
        connect(context, requiredValueOf(context, module), ValueFlowProgramNodes::escaped());
        connect(context, namespaceOf(context, module), ValueFlowProgramNodes::escaped());
    });
}

static void raiseEvent(ValueFlowRuleContext& context, BuiltinID builtin, UnseenCodeCause cause, ValueFlowNodeID result)
{
    if (!loadsModulesByName(cause)) {
        const BuiltinTable& builtins = context.program().builtins();
        const Builtin& entry = builtins.at(builtin);
        bool runsOnlyText = entry.origin == BuiltinOrigin::Engine && entry.flags.contains(BuiltinFlag::EvaluatesText) && !entry.flags.contains(BuiltinFlag::RunsUnseenCode) && !builtins.unseenCodeOf(builtin);
        letUnseenCodeRun(context, runsOnlyText || builtins.has(BuiltinMark::EvaluatesTextInGlobalScope, builtins.canonicalID(builtin)) ? UnseenCodeReach::GlobalScope : UnseenCodeReach::Realm);
        return;
    }
    for (ValueFlowHostNode answers : { ValueFlowHostNode::RequiredValuesLoadedByName, ValueFlowHostNode::NamespacesLoadedByName }) {
        letModulesBeLoadedByName(context, answers);
        context.connect(context.nodes().ofHost(answers), result);
    }
}

// "left-pad/lib" and "@scope/left-pad/lib" name the packages "left-pad" and "@scope/left-pad". A path, a URL and a name that a package maps for itself name none.
static std::span<const uint8_t> packageNamedBy(std::span<const uint8_t> name)
{
    if (name.empty() || name[0] == '.' || name[0] == '/' || name[0] == '#' || std::ranges::any_of(name, [](uint8_t character) { return character == ':' || character == '\\'; }))
        return { };
    auto lengthOfPart = [](std::span<const uint8_t> text) -> size_t {
        return std::ranges::find(text, '/') - text.begin();
    };
    size_t length = lengthOfPart(name);
    if (name[0] == '@' && length < name.size())
        length += 1 + lengthOfPart(name.subspan(length + 1));
    return name.first(length);
}

// Where code outside the image is refused, the name of a package loads a file of that package in the image or nothing, whatever file asks.
static bool isNameOfPackageOutsideImage(ValueFlowRuleContext& context, std::span<const uint8_t> name)
{
    std::span<const uint8_t> package = packageNamedBy(name);
    return !package.empty() && context.refusedCode().codeOutsideImage && !context.universe().builtinTables().heap().holdsModuleOfPackage(package);
}

static bool passesOnlyNamesThatNoFileOfProgramAnswersTo(const ValueFlowStatement& call, ValueFlowRuleContext& context)
{
    constexpr size_t indexOfName = 2;
    if (call.kind != ValueFlowStatementKind::Operation || call.opcode != Opcode::Call || call.operandCount <= indexOfName)
        return false;
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    auto isSuchName = [&](StringID text) {
        return universe.builtinTables().heap().isNameOfLibraryModule(program.string(text)) || isNameOfPackageOutsideImage(context, program.string(text));
    };
    // A text that is no key of the program is any string to the values.
    const Function& function = program.at(call.function);
    if (const Value& name = function.at(definitionThroughSameValues(function, function.at(call.site).operand(indexOfName))); name.opcode == Opcode::String)
        return isSuchName(name.string());
    bool passesOnlyThose = true;
    context.forEachMember(context.valueOf(context.operandsOf(call)[indexOfName]), [&](ValueFlowObjectID name) {
        const PropertyKey* key = universe.kindOf(name) == ValueFlowObjectKind::Key ? &program.at(universe.keyOf(name)) : nullptr;
        passesOnlyThose = passesOnlyThose && key && key->kind == PropertyKeyKind::Text && isSuchName(key->text);
    });
    return passesOnlyThose;
}

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
        std::optional<UnseenCodeCause> cause = eventOfCalling(builtins, builtin, context.refusedCode());
        if (!cause && someCallPassesPropertyThatRunsUnseenCode(context, builtin))
            cause = UnseenCodeCause::RunsUnseenCode;
        bool loadsMachineCode = std::ranges::contains(builtins.loadersOfMachineCode(), builtin, &BuiltinLoaderOfMachineCode::callee);
        if ((!cause && !loadsMachineCode) || !isCalledBySeenCode())
            return;
        context.add(context.nodes().ofHost(ValueFlowHostNode::FunctionsOfSeenCode), only(context, callee));
        if (!cause)
            return;
        if (loadsOnlyLibraryModules(builtin) || (loadsModulesByName(*cause) && passesOnlyNamesThatNoFileOfProgramAnswersTo(statement, context)))
            handLibraryModulesTo(context, statement.result);
        else
            raiseEvent(context, builtin, *cause, statement.result);
    });
}

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
    bool answersMainModule = entry == Builtins::ModuleRequireMain || entry == Builtins::ProcessMainModule;
    if (!answersMainModule)
        objects.append(context.universe().objectOf(Builtins::ModuleObject));
    if (!namesWaysToModuleObjects(context.program()))
        return;
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

void handChildrenToWhatReplacesTheirList(ValueFlowRuleContext& context, ValueSetID children)
{
    if (context.holdsAnyOf(only(context, context.universe().objectOf(Builtins::ModuleObjectChildren)), context.nodes().ofHost(ValueFlowHostNode::WrittenBuiltins)))
        context.add(ValueFlowProgramNodes::escaped(), children);
}

static void exposeMembersOf(ValueFlowRuleContext& context, ValueFlowObjectID holderObject, BuiltinID holder, bool programNamesWaysToModuleObjects, Vector<BuiltinID, 4>& waysBetweenModuleObjectsAnswered)
{
    const ValueFlowUniverse& universe = context.universe();
    const BuiltinTable& builtins = context.program().builtins();
    ModuleID module = universe.moduleOf(holderObject);
    bool isHolderOfRealm = builtins.at(holder).scope == BuiltinScope::Realm;
    if (builtins.at(holder).prototypeLink == BuiltinPrototypeLink::Builtin)
        context.addUnlessHeld(ValueFlowProgramNodes::escaped(), universe.objectOf(builtins.at(holder).inheritsFrom));
    for (BuiltinID member : builtins.membersOf(holder)) {
        if (builtins.letsInUnseenCode(builtins.canonicalID(member)))
            continue;
        if (builtins.canonicalID(member) == Builtins::ModuleObjectExports)
            continue;
        if (BuiltinID way = builtins.canonicalID(member); way == Builtins::ModuleRequireMain || way == Builtins::ModuleObjectChildren || way == Builtins::ModuleObjectParent) {
            if (programNamesWaysToModuleObjects && !waysBetweenModuleObjectsAnswered.contains(builtins.canonicalID(member))) {
                waysBetweenModuleObjectsAnswered.append(builtins.canonicalID(member));
                Vector<ValueFlowObjectID, 32> answers;
                appendModuleObjectsAnsweredBy(context, builtins.canonicalID(member), answers);
                if (builtins.canonicalID(member) == Builtins::ModuleObjectChildren)
                    answers.append(universe.resultOf(member, 0));
                context.add(ValueFlowProgramNodes::escaped(), ClimbingSet(context.maker().of(answers.span())));
            }
            continue;
        }
        if (isHolderOfRealm && builtins.at(member).origin == BuiltinOrigin::Host)
            continue;
        if (!isObjectKind(builtins.at(member).kind)) {
            context.use(member, { BuiltinUse::Read, BuiltinUse::Written });
            openProxiesThatMemberAnswers(context, member);
            continue;
        }
        context.use(member, BuiltinUse::Read);
        ValueFlowObjectID object = module ? universe.objectOf(member, module) : ValueFlowObjectID();
        context.addUnlessHeld(ValueFlowProgramNodes::escaped(), object ? object : universe.objectOf(member));
    }
}

static constexpr NamedBuiltin entriesThatInstantiateModules[] { Builtins::WebAssemblyInstantiate, Builtins::WebAssemblyInstantiateStreaming, Builtins::WebAssemblyInstance };

static void letModulesImportMembersOf(ValueFlowRuleContext& context, BuiltinID holder)
{
    const BuiltinTable& builtins = context.program().builtins();
    for (BuiltinID member : builtins.membersOf(holder)) {
        if (builtins.letsInUnseenCode(builtins.canonicalID(member)) || builtins.at(member).origin == BuiltinOrigin::Host)
            continue;
        BuiltinKind kind = builtins.at(member).kind;
        bool isCallable = kind == BuiltinKind::Constructor || kind == BuiltinKind::Function || kind == BuiltinKind::Method;
        context.useAsAnswerOfUnknownObject(member, isCallable ? OptionSet<BuiltinUse> { BuiltinUse::Read, BuiltinUse::Called } : OptionSet<BuiltinUse> { BuiltinUse::Read });
    }
}

bool isCalledWithoutStartupLoads(const BuiltinTable& builtins, BuiltinID holder, BuiltinName name)
{
    BuiltinID method = holder ? builtins.findOwnMember(builtins.canonicalID(holder), name) : BuiltinID();
    return method && builtins.has(BuiltinMark::IsCalledWithoutStartupLoads, builtins.canonicalID(method));
}

bool isFoundByNameAlone(BuiltinID member)
{
    return BuiltinTable::singleton().has(BuiltinMark::HoldsLoader, member) || member == Builtins::ProcessMainModule;
}

void applyEventsOfStore(const ValueFlowStatement& statement, ValueFlowRuleContext& context, ClimbingSet receivers, PropertyKeyID)
{
    if (!statement.function || !context.contains(receivers, context.universe().objectOf(Builtins::ModuleCache)))
        return;
    if (isInSeenCode(context, statement.function))
        context.add(context.nodes().ofHost(ValueFlowHostNode::RequiredValuesLoadedByName), unknownValue(context, ValueFlowUnknownSource::ContentsOfEscapedObject));
}

static bool isGetterFoundWithoutItsName(const BuiltinTable& builtins, BuiltinID builtin)
{
    if (builtins.at(builtin).kind != BuiltinKind::Accessor || !builtins.moduleLoadsOf(builtin).empty())
        return false;
    return std::ranges::all_of(builtins.calledFunctionsOf(builtin), [&](const BuiltinCalledFunction& called) {
        return std::ranges::contains(builtins.at(called.function).implements, builtin);
    });
}

static void exposeModuleObjectOf(ValueFlowRuleContext& context, ModuleID id)
{
    if (ValueFlowObjectID moduleObject = moduleObjectOfEvaluatedModule(context, id)) {
        handOut(context, moduleObject);
        context.add(ValueFlowProgramNodes::escaped(), only(context, moduleObject));
    }
}

static bool mayMachineCodeRun(ValueFlowRuleContext& context)
{
    Vector<ValueFlowObjectID, 32> loaders;
    for (const BuiltinLoaderOfMachineCode& loader : context.program().builtins().loadersOfMachineCode())
        context.universe().appendObjectsOf(loader.callee, loaders);
    return context.holdsAnyOf(ClimbingSet(context.maker().of(loaders.span())), context.nodes().ofHost(ValueFlowHostNode::RunBuiltins));
}

static void exposeModuleObjectsOfEntryPoints(ValueFlowRuleContext& context)
{
    ClimbingSet process = only(context, context.universe().objectOf(context.program().builtins().at(Builtins::ProcessMainModule).holder));
    if (!isHostFlagRaised(context, ValueFlowHostNode::UnseenCodeRuns) && !mayMachineCodeRun(context) && !context.holdsAnyOf(process, ValueFlowProgramNodes::escaped()) && !context.holdsAnyOf(process, ValueFlowProgramNodes::lost()))
        return;
    for (const Entry& entryPoint : context.program().entries)
        exposeModuleObjectOf(context, entryPoint.module);
}

// Array.prototype.map and its like read `constructor` from their receiver and `Symbol.species` from that. Nothing else that happens to an escaped object goes that way.
static void callBuiltinsThatReadSpecies(ValueFlowRuleContext& context, ClimbingSet functions)
{
    context.forEachMember(functions, [&](ValueFlowObjectID function) {
        readSpeciesOfUnknownObject(context.universe().builtinOf(function), context);
    });
}

// A host function that has escaped may be handed anything else that has escaped, and treats it as its row says.
static void passUnknownOperandsTo(ValueFlowRuleContext& context, BuiltinID function)
{
    const BuiltinTable& builtins = context.program().builtins();
    ValueSetID unknown = unknownValue(context, ValueFlowUnknownSource::ParameterOfEscapedFunction);
    auto pass = [&](const BuiltinParameter& parameter) {
        if (ValueFlowBehaviorID behavior = context.universe().builtinTables().behaviorOf(parameter))
            context.add(context.hub(behavior), unknown);
    };
    for (const BuiltinOverload& overload : builtins.overloadsOf(builtins.canonicalID(function))) {
        if (overload.receiver)
            pass(*overload.receiver);
        for (const BuiltinParameter& parameter : overload.parameters)
            pass(parameter);
    }
}

// No node lists what has escaped beside a value nobody knows. Where such a value is called, a loader or an evaluator that has escaped is called, with operands nobody knows.
static void callWhatLetsInUnseenCode(ValueFlowRuleContext& context, ValueFlowObjectID callee)
{
    const BuiltinTable& builtins = context.program().builtins();
    BuiltinID builtin = builtins.canonicalID(context.universe().builtinOf(callee));
    std::optional<UnseenCodeCause> cause = eventOfCalling(builtins, builtin, context.refusedCode());
    if (!cause) {
        // createRequire() returns a require function.
        Vector<ValueFlowObjectID, 32> returned;
        for (const BuiltinOverload& overload : builtins.overloadsOf(builtin)) {
            for (const BuiltinTypePart& part : builtins.at(overload.result.type).parts) {
                if (part.kind == BuiltinTypePartKind::Builtin && builtins.letsInUnseenCodeWhenCalled(builtins.canonicalID(part.builtin)))
                    context.universe().appendObjectsOf(builtins.canonicalID(part.builtin), returned);
            }
        }
        context.add(ValueFlowProgramNodes::escaped(), ClimbingSet(context.maker().of(returned.span())));
        return;
    }
    context.add(context.nodes().ofHost(ValueFlowHostNode::FunctionsOfSeenCode), only(context, callee));
    if (loadsOnlyLibraryModules(builtin))
        handLibraryModulesTo(context, context.nodes().ofHost(ValueFlowHostNode::HeldByNativeCode));
    else if (loadsModulesByName(*cause))
        handLoadedModulesToNativeCode(context);
    else
        raiseEvent(context, builtin, *cause, { });
}

void runUsedBuiltins(const ValueFlowStatement&, ValueFlowRuleContext& context)
{
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    const BuiltinTable& builtins = program.builtins();

    ClimbingSet handedOut = context.valueWithin(ClimbingSet(universe.builtinObjects()), ValueFlowProgramNodes::escaped());
    ClimbingSet escaped = context.unite(handedOut, context.valueOf(context.nodes().ofHost(ValueFlowHostNode::LoadersAndEvaluatorsThatEvaluatedTextNames)));
    bool membersFollowHolders = isHostFlagRaised(context, ValueFlowHostNode::UnseenCodeRuns);
    bool callsWhateverHasEscaped = mayCallWhateverHasEscaped(context);
    ClimbingSet run = context.valueOf(context.nodes().ofHost(ValueFlowHostNode::RunBuiltins));
    ClimbingSet takenFromHolders = context.valueOf(context.nodes().ofHost(ValueFlowHostNode::FunctionsOfHostTakenFromKnownHolders));
    Vector<ValueFlowObjectID, 64> escapedObjectsOfLanguage;
    Vector<ValueFlowObjectID, 16> calledFunctionsOfHost;
    bool callsLoaderThatHasEscaped = false;
    Attachments attachments { context, builtins };
    context.forEachMember(escaped, [&](ValueFlowObjectID object) {
        BuiltinID builtin = universe.builtinOf(object);
        if (!builtin)
            return;
        callsLoaderThatHasEscaped |= callsWhateverHasEscaped && builtins.at(builtins.canonicalID(builtin)).flags.contains(BuiltinFlag::LoadsModuleByName);
        // A member of a holder of the realm that has escaped has not escaped with it: the program has to have taken it off its holder.
        bool isInHandsOfProgram = builtins.at(builtin).origin != BuiltinOrigin::Host || context.contains(context.valueOf(context.nodes().ofHost(ValueFlowHostNode::LoadersAndEvaluatorsThatProgramNames)), universe.objectOf(builtins.canonicalID(builtin)));
        if (callsWhateverHasEscaped && builtins.letsInUnseenCode(builtins.canonicalID(builtin)) && context.contains(handedOut, object) && isInHandsOfProgram)
            callWhatLetsInUnseenCode(context, object);
        bool isOfLanguage = builtins.at(builtin).origin != BuiltinOrigin::Host;
        if (!isOfLanguage && callsWhateverHasEscaped && context.contains(takenFromHolders, object))
            calledFunctionsOfHost.append(object);
        if (!isOfLanguage)
            return;
        escapedObjectsOfLanguage.append(object);
        if (callsWhateverHasEscaped || context.contains(run, object))
            attachments.addClassesAnsweredBy(builtin);
    });
    if (callsLoaderThatHasEscaped) {
        if (isHostFlagRaised(context, ValueFlowHostNode::ModulesAreRequiredByName) || isHostFlagRaised(context, ValueFlowHostNode::ModulesAreImportedByName))
            handLoadedModulesToNativeCode(context);
        else if (isHostFlagRaised(context, ValueFlowHostNode::LibraryModulesAreRequiredByName))
            handLibraryModulesTo(context, context.nodes().ofHost(ValueFlowHostNode::HeldByNativeCode));
    }
    if (callsWhateverHasEscaped)
        callBuiltinsThatReadSpecies(context, context.restrict(escaped, universe.builtinFunctionsThatReadSpecies()));
    ClimbingSet escapedOfLanguage(context.maker().of(escapedObjectsOfLanguage.span()));
    ClimbingSet used = context.unite(context.valueOf(context.nodes().ofHost(ValueFlowHostNode::UsedBuiltins)), escapedOfLanguage);
    run = context.unite(run, callsWhateverHasEscaped ? escapedOfLanguage : context.valueWithin(escapedOfLanguage, context.nodes().ofHost(ValueFlowHostNode::BuiltinsThatStatementsName)));
    run = context.unite(run, ClimbingSet(context.maker().of(calledFunctionsOfHost.span())));
    for (ValueFlowObjectID function : calledFunctionsOfHost) {
        context.use(universe.builtinOf(function), BuiltinUse::Called);
        passUnknownOperandsTo(context, universe.builtinOf(function));
    }

    context.forEachMember(used, [&](ValueFlowObjectID object) {
        BuiltinID builtin = universe.builtinOf(object);
        if (!builtin)
            return;
        attachments.addRowsOf(builtin);
        if (!isObjectKind(builtins.at(builtin).kind) || membersFollowHolders || context.contains(run, object))
            attachments.addCodeOf(builtin);
    });
    context.forEachMember(context.valueOf(context.nodes().ofHost(ValueFlowHostNode::WrittenBuiltins)), [&](ValueFlowObjectID object) {
        if (BuiltinID builtin = universe.builtinOf(object))
            attachments.addCodeOf(builtin, true);
    });
    for (const RuntimeFunctionEntry& entry : program.runtimeFunctions) {
        if (!entry.function || !builtins.contains(entry.id))
            continue;
        for (BuiltinID builtin : builtins.at(entry.id).implements) {
            if ((membersFollowHolders && isGetterFoundWithoutItsName(builtins, builtin)) || context.contains(isObjectKind(builtins.at(builtin).kind) && !membersFollowHolders ? run : used, universe.objectOf(builtin)))
                attachments.addCode(entry.id);
        }
    }
    if (isHostFlagRaised(context, ValueFlowHostNode::HostChoosesOperands))
        attachments.addStartupModules();
    attachments.enter();

    bool programNamesWaysToModuleObjects = namesWaysToModuleObjects(program);
    if (programNamesWaysToModuleObjects)
        exposeModuleObjectsOfEntryPoints(context);
    bool modulesFindImportsByName = std::ranges::any_of(entriesThatInstantiateModules, [&](BuiltinID entry) {
        return context.contains(run, universe.objectOf(entry));
    });
    Vector<BuiltinID, 4> waysBetweenModuleObjectsAnswered;
    context.forEachMember(escaped, [&](ValueFlowObjectID object) {
        BuiltinID builtin = universe.builtinOf(object);
        if (!builtin)
            return;
        if (builtins.at(builtin).origin != BuiltinOrigin::Host)
            context.use(builtin, context.contains(run, object) ? OptionSet<BuiltinUse> { BuiltinUse::Escaped, BuiltinUse::Called } : OptionSet<BuiltinUse> { BuiltinUse::Escaped });
        if (membersFollowHolders || builtins.at(builtin).scope != BuiltinScope::Realm)
            exposeMembersOf(context, object, builtin, programNamesWaysToModuleObjects, waysBetweenModuleObjectsAnswered);
        else if (modulesFindImportsByName && builtins.at(builtin).origin != BuiltinOrigin::Host)
            letModulesImportMembersOf(context, builtin);
    });
}

void runUnseenCode(const ValueFlowStatement&, ValueFlowRuleContext& context)
{
    if (!isHostFlagRaised(context, ValueFlowHostNode::UnseenCodeRuns))
        return;

    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    const BuiltinTable& builtins = program.builtins();

    context.add(ValueFlowProgramNodes::escaped(), only(context, universe.objectOf(Builtins::GlobalThis)));
    Vector<ValueFlowObjectID, 64> objectsOfHost;
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        const Builtin& entry = builtins.at(BuiltinID(index));
        if (entry.origin == BuiltinOrigin::Host && entry.scope == BuiltinScope::Realm && isObjectKind(entry.kind) && builtins.canonicalID(BuiltinID(index)) == BuiltinID(index))
            objectsOfHost.append(universe.objectOf(BuiltinID(index)));
    }
    if (isHostFlagRaised(context, ValueFlowHostNode::UnseenCodeChangesRealm))
        letAnythingHappenTo(context, ClimbingSet(context.maker().of(objectsOfHost.span())));
    bool filesOfProgramAreLoadedByName = isHostFlagRaised(context, ValueFlowHostNode::ModulesAreRequiredByName) || isHostFlagRaised(context, ValueFlowHostNode::ModulesAreImportedByName);
    if (filesOfProgramAreLoadedByName && isHostFlagRaised(context, ValueFlowHostNode::UnseenCodeHasLoader))
        handLoadedModulesToNativeCode(context);
    else
        handLoadableModulesToText(context, filesOfProgramAreLoadedByName);

    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        if (builtins.at(BuiltinID(index)).flags.contains(BuiltinFlag::EvaluatesText))
            context.use(BuiltinID(index), { BuiltinUse::Escaped, BuiltinUse::Called });
    }
    letHostChooseOperands(context);
    context.use(Builtins::UndeclaredNativeCode, BuiltinUse::Read);

    if (!isHostFlagRaised(context, ValueFlowHostNode::UnseenCodeChangesRealm))
        return;
    for (uint32_t index = 0; index < program.modules.size(); ++index)
        exposeModuleObjectOf(context, ModuleID(index));
}

void runModulesLoadedByName(const ValueFlowStatement&, ValueFlowRuleContext& context)
{
    bool areRequired = isHostFlagRaised(context, ValueFlowHostNode::ModulesAreRequiredByName);
    bool areImported = isHostFlagRaised(context, ValueFlowHostNode::ModulesAreImportedByName);
    bool libraryIsRequired = isHostFlagRaised(context, ValueFlowHostNode::LibraryModulesAreRequiredByName);
    if (!areRequired && !areImported && !libraryIsRequired)
        return;

    const ValueFlowProgramNodes& nodes = context.nodes();
    forEachModuleLoadableByName(context, [&](ModuleID id, bool isOfProgram) {
        if (isOfProgram && !areRequired && !areImported)
            return;
        context.add(nodes.ofHost(ValueFlowHostNode::ModulesOfSeenCode), only(context, context.universe().namespaceOf(id)));
        if (isOfProgram || isAskedOfHostByName(context.program().at(id)))
            evaluateModule(context, id);
        if (areRequired) {
            connect(context, requiredValueOf(context, id), nodes.ofHost(ValueFlowHostNode::RequiredValuesLoadedByName));
            if (ValueFlowObjectID moduleObject = moduleObjectOfEvaluatedModule(context, id))
                context.add(nodes.ofHost(ValueFlowHostNode::ChildrenOfModuleObjects), only(context, moduleObject));
        }
        if (areImported)
            connect(context, namespaceOf(context, id), nodes.ofHost(ValueFlowHostNode::NamespacesLoadedByName));
        if (libraryIsRequired && !isOfProgram)
            connect(context, requiredValueOf(context, id), nodes.ofHost(ValueFlowHostNode::LibraryValuesLoadedByName));
    });

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
