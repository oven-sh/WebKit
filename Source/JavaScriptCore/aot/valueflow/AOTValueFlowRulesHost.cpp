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
#include "AOTPrimitiveKeyTexts.h"
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

// The walk over all modules is cut into parts, each a statement of its own: a part runs again only when one of its own modules changes, and the parts run side by side.
AOT_THRESHOLD(uint32_t, modulesOfPartLoadedByName, 1024);

static uint32_t numberOfPartsLoadedByName(const Program& program)
{
    return std::max<uint32_t>(1, (program.modules.size() + modulesOfPartLoadedByName - 1) / modulesOfPartLoadedByName);
}

// Where the walk has parts, what is said of the copies of files stands in two statements in front of them: which files the copies reach, once, and where that goes on to.
static constexpr uint32_t numberOfStatementsOnCopiesOfFiles = 2;

// A layer that test files share has thousands of entry points. None where one statement takes them all.
AOT_THRESHOLD(uint32_t, entriesOfPartOfHostRoots, 256);

static uint32_t numberOfPartsOfEntries(const Program& program)
{
    if (program.entries.size() <= entriesOfPartOfHostRoots)
        return 0;
    return (program.entries.size() + entriesOfPartOfHostRoots - 1) / entriesOfPartOfHostRoots;
}

// The host has no operands: the word for their guards says which part a statement is. A site would be taken for a value of a function.
static void appendPart(ValueFlowBody& body, ValueFlowStatementKind kind, uint32_t part)
{
    ValueFlowBody::Statement statement { { }, { }, 0, 0, { }, { }, kind };
    statement.guardsOfOperands = part;
    body.statements.append(statement);
}

static uint32_t partOf(const ValueFlowStatement& statement)
{
    return statement.guardsOfOperands;
}

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

    for (uint32_t part = 0; part <= numberOfPartsOfEntries(program); ++part)
        appendPart(body, ValueFlowStatementKind::HostRoots, part);
    for (ValueFlowStatementKind kind : { ValueFlowStatementKind::UsedBuiltins, ValueFlowStatementKind::UnseenCode })
        body.statements.append({ { }, { }, 0, 0, { }, { }, kind });
    uint32_t parts = numberOfPartsLoadedByName(program);
    for (uint32_t part = 0; part < (parts == 1 ? 1 : numberOfStatementsOnCopiesOfFiles + parts); ++part)
        appendPart(body, ValueFlowStatementKind::ModulesLoadedByName, part);
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
    context.add(ValueFlowProgramNodes::escaped(), context.universe().holdersOfPublicLibraryExports());
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

static bool mayProgramHaveStoredUnder(ValueFlowRuleContext&, BuiltinID member);

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
    if (condition->testsReplaceBuiltins)
        return context.program().testsReplaceBuiltins();
    if (condition->programStores)
        return mayProgramHaveStoredUnder(context, condition->programStores);

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
        noteLoadOf(context, load.module);
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

static void followFunctionsOfSeenCode(ValueFlowRuleContext& context)
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
            for (const Value& value : context.functionWithBody(id).values) {
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
}

// The first statement runs again each time seen code calls one more function of the library. The entry points are left to the others.
// The code of the images below makes its records out of sight and keeps them: whatever holds a value that nobody knows may hold one.
static void letImagesBelowMakeRecords(ValueFlowRuleContext& context)
{
    const Program& program = context.program();
    if (program.recordsOfImagesBelow.isEmpty())
        return;
    Vector<ValueFlowObjectID, 16> records(program.recordsOfImagesBelow.size(), [&](size_t index) {
        return context.universe().instanceOf(program.recordsOfImagesBelow[index].layout);
    });
    ValueSetID made = context.maker().of(records.span());
    context.add(context.written(ClimbingSet(made), prototypeLinkName), only(context, context.universe().objectOf(Builtins::ObjectPrototype)));
    ValueSetID unknown = unknownValue(context, ValueFlowUnknownSource::ContentsOfEscapedObject);
    context.add(context.written(ClimbingSet(made), unknownKeyName), unknown);
    // The prototype object of a class of another image is an object that nobody knows.
    if (std::ranges::any_of(program.recordsOfImagesBelow, &Program::RecordOfImageBelow::isInstanceOfClass))
        context.add(context.written(ClimbingSet(made), prototypeLinkName), unknown);
    context.add(ValueFlowProgramNodes::escaped(), made);
}

void runHostRoots(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    if (!partOf(statement)) {
        letImagesBelowMakeRecords(context);
        // What is thrown or what a promise is rejected with ends at the host where nobody catches it. The host prints it the way console.error does, whatever lies inside it too.
        if (!isHostFlagRaised(context, ValueFlowHostNode::HostChoosesOperands) && mayProgramHaveStoredMethodThatHostLooksFor(context))
            letHostChooseOperands(context);
        // `Object.values(require.cache)[0].exports`: whoever gets at an entry of the table has all that its file exports, whether or not a `require` of the program keeps its answer.
        for (uint32_t index = 0; index < program.modules.size(); ++index) {
            if (program.modules[index].flags.contains(ModuleFlags::IsShownByEntryOfCache))
                connect(context, requiredValueOf(context, ModuleID(index)), ValueFlowProgramNodes::escaped());
            if (program.modules[index].flags.contains(ModuleFlags::IsHandedOutByTestRunner)) {
                ModuleID module(index);
                if (isAskedOfHostByName(program.modules[index])) {
                    context.add(context.nodes().ofHost(ValueFlowHostNode::ModulesOfSeenCode), only(context, universe.namespaceOf(module)));
                    evaluateModule(context, module);
                }
                for (const ValueFlowSources& answer : { requiredValueOf(context, module), namespaceOf(context, module) })
                    connect(context, answer, context.nodes().ofHost(ValueFlowHostNode::HeldByNativeCode));
            }
        }
    }
    std::span<const Entry> entries = program.entries.span();
    if (!numberOfPartsOfEntries(program))
        followFunctionsOfSeenCode(context);
    else if (uint32_t index = partOf(statement))
        entries = entries.subspan((index - 1) * entriesOfPartOfHostRoots).first(std::min<size_t>(entriesOfPartOfHostRoots, entries.size() - (index - 1) * entriesOfPartOfHostRoots));
    else {
        followFunctionsOfSeenCode(context);
        return;
    }

    for (const Entry& entry : entries) {
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

bool refusesAllSourceText(const BuiltinTable& builtins, BuiltinID builtin, RefusedCode refused)
{
    const Builtin& entry = builtins.at(builtin);
    if (entry.origin == BuiltinOrigin::Engine)
        return refused.evaluatedText && entry.flags.contains(BuiltinFlag::EvaluatesText);
    return refused.hostEvaluatedText && (entry.flags.contains(BuiltinFlag::EvaluatesText) || builtins.has(BuiltinMark::EvaluatesTextInGlobalScope, builtin));
}

std::optional<UnseenCodeCause> eventOfCalling(const BuiltinTable& builtins, BuiltinID builtin, RefusedCode refused)
{
    const Builtin& entry = builtins.at(builtin);
    // `module._compile(text)` hands the text a require, so it loads what the text names: nothing where the text is refused.
    if (refusesAllSourceText(builtins, builtin, refused))
        return std::nullopt;
    bool loadsFromImageAlone = refused.codeOutsideImage || loadsOnlyLibraryModules(builtin);
    // A load of a module of the image runs no code that nobody has read. The text does.
    if (entry.flags.contains(BuiltinFlag::EvaluatesText) && (loadsFromImageAlone || !entry.flags.contains(BuiltinFlag::LoadsModuleByName)))
        return UnseenCodeCause::EvaluatesText;
    if (entry.flags.contains(BuiltinFlag::LoadsModuleByName))
        return loadsFromImageAlone ? UnseenCodeCause::LoadsModuleByName : UnseenCodeCause::LoadsCodeByName;
    const BuiltinUnseenCode* unseenCode = builtins.unseenCodeOf(builtin);
    if (unseenCode && unseenCode->condition && unseenCode->condition->hasProperty)
        return std::nullopt;
    if (unseenCode ? mayHold(unseenCode->condition) : entry.flags.contains(BuiltinFlag::RunsUnseenCode))
        return UnseenCodeCause::RunsUnseenCode;
    return std::nullopt;
}

// Where code outside the image is refused the run time refuses every file that such a loader is asked for, those of the image too: it runs a file from its text.
bool loadsCodeOnceRead(const BuiltinTable& builtins, BuiltinID builtin, RefusedCode refused)
{
    return !refused.codeOutsideImage && builtins.has(BuiltinMark::ReadLoadsCodeByName, builtins.canonicalID(builtin));
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
    return !isWrittenByProgram(program, module) || module.kind == ModuleKind::External || module.kind == ModuleKind::Data || module.flags.contains(ModuleFlags::IsFoundByName) || std::ranges::contains(program.entries, id, &Entry::module);
}

bool answersOnlyToLoadsOfProgram(const Program& program, ModuleID id)
{
    const Module& module = program.at(id);
    return module.kind == ModuleKind::Data || (module.flags.contains(ModuleFlags::IsFoundByName) && !std::ranges::contains(program.entries, id, &Entry::module));
}

bool isAskedOfHostByName(const Module& module)
{
    return module.kind == ModuleKind::RuntimeSource && !module.flags.contains(ModuleFlags::IsLoadedByImportOnly);
}

static void forEachModuleLoadableByName(ValueFlowRuleContext& context, uint32_t first, uint32_t end, NOESCAPE const Invocable<void(ModuleID, bool isOfProgram)> auto& functor)
{
    const Program& program = context.program();
    const ValueFlowHeapTables& tables = context.universe().builtinTables().heap();
    for (uint32_t index = first; index < end; ++index) {
        const Module& module = program.modules[index];
        if (!tables.mayAnswerToName(ModuleID(index)))
            continue;
        bool isOfProgram = isWrittenByProgram(program, module);
        if (module.body ? !isOfProgram && !context.isReached(module.body) && !isAskedOfHostByName(module) : !isOfProgram && module.kind != ModuleKind::Runtime)
            continue;
        functor(ModuleID(index), isOfProgram);
    }
}

static void handLibraryModulesTo(ValueFlowRuleContext& context, ValueFlowNodeID hands)
{
    raiseHostFlag(context, ValueFlowHostNode::LibraryModulesAreRequiredByName);
    context.connect(context.nodes().ofHost(ValueFlowHostNode::LibraryValuesLoadedByName), hands);
}

// Whoever asks for this runs again each time something escapes. The walk over all modules is left to runModulesLoadedByName(), which runs again only when a module changes.
static void handLoadedModulesToNativeCode(ValueFlowRuleContext& context)
{
    for (ValueFlowHostNode answers : { ValueFlowHostNode::RequiredValuesLoadedByName, ValueFlowHostNode::NamespacesLoadedByName })
        letModulesBeLoadedByName(context, answers);
    raiseHostFlag(context, ValueFlowHostNode::LoadedModulesAreHandedToNativeCode);
}

// Text has import() and process.getBuiltinModule() and no require of a file. The loader answers it with an entry point under any name,
// and with another file of the image only where the program itself loads files by name.
static void handLoadableModulesToText(ValueFlowRuleContext& context, bool filesOfProgramAreLoadedByName)
{
    handLibraryModulesTo(context, context.nodes().ofHost(ValueFlowHostNode::HeldByNativeCode));
    forEachModuleLoadableByName(context, 0, context.program().modules.size(), [&](ModuleID module, bool isOfProgram) {
        if (!isOfProgram || (answersOnlyToLoadsOfProgram(context.program(), module) && !filesOfProgramAreLoadedByName))
            return;
        // The files that the host runs name what they use, and a file of the build hands them that.
        if (context.refusedCode().hostRunsFilesOutsideImage && showsOutsideOnlyWhatIsOpen(context.program().at(module)))
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
static bool isNameOfPackageOutsideImage(const ValueFlowUniverse& universe, RefusedCode refused, std::span<const uint8_t> name)
{
    std::span<const uint8_t> package = packageNamedBy(name);
    return !package.empty() && refused.codeOutsideImage && !universe.builtinTables().heap().holdsModuleOfPackage(package);
}

// "node:" and "bun:" stand in front of names of the library alone.
static bool hasSchemeOfLibrary(std::span<const uint8_t> name)
{
    return spanHasPrefix(name, byteCast<uint8_t>("node:"_span)) || spanHasPrefix(name, byteCast<uint8_t>("bun:"_span));
}

ModulesThatNamesFind modulesThatNamesFind(const Program& program, const ValueFlowUniverse& universe, RefusedCode refused, const LoadUnderName& load, NOESCAPE const NamesOfLoad& forEachName)
{
    const ValueFlowHeapTables& tables = universe.builtinTables().heap();
    ModulesThatNamesFind found;
    auto addPath = [&](std::span<const uint8_t> path) {
        std::optional<Vector<ModuleID, 2>> files = load.referrer ? tables.filesThatPathFinds(load.referrer, path) : std::nullopt;
        if (!files || (files->isEmpty() && !refused.codeOutsideImage))
            found.anything = true;
        else if (files->isEmpty())
            found.fileOutsideImage = true;
        else
            found.filesOfProgram.appendVector(*files);
    };
    auto add = [&](std::span<const uint8_t> text) {
        if (ModuleID module = tables.libraryModuleNamed(text))
            found.libraryModules.append(module);
        else if (hasSchemeOfLibrary(text) || isNameOfPackageOutsideImage(universe, refused, text))
            found.nativeModule = true;
        else if (!text.empty() && text[0] == '.')
            addPath(text);
        else
            found.anything = true;
    };
    // A text that is no key of the program is any string to the values.
    const Function& function = program.at(load.function);
    if (const Value& definition = function.at(definitionThroughSameValues(function, load.written)); definition.opcode == Opcode::String) {
        add(program.string(definition.string()));
        return found;
    }
    forEachName([&](ValueFlowObjectID member) {
        if (universe.kindOf(member) == ValueFlowObjectKind::Key) {
            const PropertyKey& key = program.at(universe.keyOf(member));
            if (key.kind == PropertyKeyKind::Text)
                add(program.string(key.text));
            else
                found.anything = true;
            return found.anything;
        }
        constexpr uint16_t neverConvertedToString = Primitive::Symbol | Primitive::Absent;
        constexpr uint16_t noString = Primitive::NumberLike | Primitive::BigInt | neverConvertedToString;
        if (member.index() < ValueFlowUniverse::numberOfPrimitiveKinds && ((1u << member.index()) & (load.takesStringsAlone ? noString : neverConvertedToString)))
            return false;
        // No module of the library is spelled "undefined".
        auto text = std::ranges::find_if(primitiveKeyTexts, [&](const PrimitiveKeyText& entry) {
            return member == ValueFlowUniverse::primitive(static_cast<PrimitiveSet>(entry.primitive));
        });
        found.anything = text == std::end(primitiveKeyTexts) || !isNameOfPackageOutsideImage(universe, refused, byteCast<uint8_t>(text->text.span()));
        return found.anything;
    });
    return found;
}

ModulesThatNamesFind modulesThatNamesFind(ValueFlowRuleContext& context, const LoadUnderName& load, const ValueFlowOperand& name)
{
    context.functionWithBody(load.function);
    return modulesThatNamesFind(context.program(), context.universe(), context.refusedCode(), load, [&](const ScopedLambda<bool(ValueFlowObjectID)>& functor) {
        context.anyMember(context.valueOf(name), functor);
    });
}

static ClimbingSet publicNativeModules(ValueFlowRuleContext& context)
{
    const BuiltinTable& builtins = context.program().builtins();
    Vector<ValueFlowObjectID, 32> objects;
    for (uint32_t index = 0; index < builtins.moduleCount(); ++index) {
        const RuntimeModuleData& module = builtins.at(RuntimeModuleID(index));
        if (module.namespaceObject && module.flags.contains(RuntimeModuleFlag::IsPublic) && !module.flags.contains(RuntimeModuleFlag::HasSource))
            objects.append(context.universe().objectOf(module.namespaceObject));
    }
    return ClimbingSet(context.maker().of(objects.span()));
}

void noteLoadUnderSpelledNames(ValueFlowRuleContext& context, FunctionID load, const ModulesThatNamesFind& found, ValueFlowHostNode answers, ValueFlowNodeID answer)
{
    RELEASE_ASSERT(answers == ValueFlowHostNode::RequiredValuesLoadedByName || answers == ValueFlowHostNode::NamespacesLoadedByName);
    if (isWrittenInProgram(context.program().at(load))) {
        raiseHostFlag(context, ValueFlowHostNode::LibraryModulesAreLoadedUnderSpelledNames);
        // The image keeps every file that a name may find findable. Only this load is known to find fewer.
        if (!found.filesOfProgram.isEmpty())
            raiseHostFlag(context, answers == ValueFlowHostNode::RequiredValuesLoadedByName ? ValueFlowHostNode::ModulesAreRequiredByName : ValueFlowHostNode::ModulesAreImportedByName);
    }
    if (found.nativeModule && answer)
        context.add(answer, publicNativeModules(context));
    // `require.cache["fs"] = { exports: fake }`: require() looks there for a Node.js module that is named without `node:`, overridableRequire() in CommonJS.ts.
    if (answers == ValueFlowHostNode::RequiredValuesLoadedByName && (found.nativeModule || !found.libraryModules.isEmpty()) && answer) {
        context.connect(context.nodes().ofHost(ValueFlowHostNode::ExportsOfEntriesStoredInModuleCache), answer);
        if (isHostFlagRaised(context, ValueFlowHostNode::UnseenCodeHasLoader))
            context.add(answer, unknownValue(context, ValueFlowUnknownSource::UnseenCode));
    }
}

static ModulesThatNamesFind modulesThatNamesPassedFind(const ValueFlowStatement& call, ValueFlowRuleContext& context, ValueFlowObjectID loader, BuiltinID builtin)
{
    constexpr size_t indexOfName = 2;
    if (call.kind != ValueFlowStatementKind::Operation || call.opcode != Opcode::Call || call.operandCount <= indexOfName)
        return { .anything = true };
    ModulesThatNamesFind found = modulesThatNamesFind(context, { call.function, context.functionWithBody(call.function).at(call.site).operand(indexOfName), context.universe().moduleOf(loader), builtin == Builtins::ModuleRequire }, context.operandsOf(call)[indexOfName]);
    // What the loader makes of a file on disk has a site at the two operations alone.
    found.anything |= found.fileOutsideImage;
    return found;
}

static std::optional<RequestAtRunTime> requestMadeByCalling(const BuiltinTable& builtins, BuiltinID canonicalCallee)
{
    if (canonicalCallee == Builtins::ModuleRequire)
        return RequestAtRunTime::Require;
    if (canonicalCallee == Builtins::ModuleRequireResolve)
        return RequestAtRunTime::Resolve;
    const Builtin& callee = builtins.at(canonicalCallee);
    if (callee.holder && builtins.canonicalID(callee.holder) == Builtins::ModuleRequirePrototype && callee.name == builtins.at(Builtins::ModuleRequireResolve).name)
        return RequestAtRunTime::Resolve;
    return std::nullopt;
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

    context.forEachMemberInLine(context.restrict(callees, universe.builtinFunctions()), [&](ValueFlowObjectID callee) {
        BuiltinID builtin = builtins.canonicalID(universe.builtinOf(callee));
        std::optional<RequestAtRunTime> request = requestMadeByCalling(builtins, builtin);
        bool asksProgram = request && askLoaderFunctionsOfProgram(context, universe.moduleOf(callee), *request);
        std::optional<UnseenCodeCause> cause = eventOfCalling(builtins, builtin, context.refusedCode());
        if (!cause && someCallPassesPropertyThatRunsUnseenCode(context, builtin))
            cause = UnseenCodeCause::RunsUnseenCode;
        bool loadsMachineCode = std::ranges::contains(builtins.loadersOfMachineCode(), builtin, &BuiltinLoaderOfMachineCode::callee);
        if ((!cause && !loadsMachineCode) || !isCalledBySeenCode())
            return;
        context.add(context.nodes().ofHost(ValueFlowHostNode::FunctionsOfSeenCode), only(context, callee));
        if (!cause)
            return;
        if (!loadsModulesByName(*cause))
            return raiseEvent(context, builtin, *cause, statement.result);
        ModulesThatNamesFind found = modulesThatNamesPassedFind(statement, context, callee, builtin);
        found.anything |= asksProgram;
        if (found.anything || (loadsOnlyLibraryModules(builtin) && !found.filesOfProgram.isEmpty()))
            return loadsOnlyLibraryModules(builtin) ? handLibraryModulesTo(context, statement.result) : raiseEvent(context, builtin, *cause, statement.result);
        noteLoadUnderSpelledNames(context, statement.function, found, ValueFlowHostNode::RequiredValuesLoadedByName, statement.result);
        for (ModuleID module : found.libraryModules)
            requireModule(statement, context, module);
        for (ModuleID module : found.filesOfProgram)
            requireModule(statement, context, module);
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

static bool storesPathOfMainModule(ValueFlowRuleContext& context)
{
    return isHostFlagRaised(context, ValueFlowHostNode::PathOfMainModuleIsStored);
}

// The run time answers for the same files: Modules::mainModuleObject().
static BitVector modulesThatMayBeMain(ValueFlowRuleContext& context)
{
    const Program& program = context.program();
    bool findsFilesByName = context.refusedCode().hostRunsFilesOutsideImage || isHostFlagRaised(context, ValueFlowHostNode::ModulesAreRequiredByName) || isHostFlagRaised(context, ValueFlowHostNode::ModulesAreImportedByName);
    BitVector modules = modulesThatHostAsksFor(program, findsFilesByName);
    if (!storesPathOfMainModule(context))
        return modules;
    for (size_t index = 0; index < program.modules.size(); ++index) {
        if (!program.modules[index].flags.contains(ModuleFlags::IsLibrary))
            modules.quickSet(index);
    }
    return modules;
}

void appendModuleObjectsAnsweredBy(ValueFlowRuleContext& context, BuiltinID entry, Vector<ValueFlowObjectID, 32>& objects)
{
    const ValueFlowProgramNodes& nodes = context.nodes();
    bool answersMainModule = entry == Builtins::ModuleRequireMain || entry == Builtins::ProcessMainModule;
    if (!answersMainModule || isHostFlagRaised(context, ValueFlowHostNode::CodeOutsideImageIsLoaded) || storesPathOfMainModule(context))
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
    bool mayBeNoCommonJSFile = storesPathOfMainModule(context);
    for (size_t index : modulesThatMayBeMain(context)) {
        if (ValueFlowObjectID moduleObject = moduleObjectOfEvaluatedModule(context, ModuleID(static_cast<uint32_t>(index)))) {
            handOut(context, moduleObject);
            objects.append(moduleObject);
        } else if (mayBeNoCommonJSFile)
            showAnswerOfRequireInModuleObject(context, ModuleID(static_cast<uint32_t>(index)));
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

bool isCalledByLoaderOnceStored(BuiltinID member)
{
    const BuiltinTable& builtins = BuiltinTable::singleton();
    return builtins.has(BuiltinMark::IsResolverThatLoaderCalls, member) || builtins.has(BuiltinMark::IsRequireThatLoaderCalls, member);
}

void applyEventsOfStore(const ValueFlowStatement& statement, ValueFlowRuleContext& context, ClimbingSet receivers, PropertyKeyID)
{
    if (!statement.function || !context.contains(receivers, context.universe().objectOf(Builtins::ModuleCache)))
        return;
    if (isInSeenCode(context, statement.function)) {
        context.add(context.nodes().ofHost(ValueFlowHostNode::RequiredValuesLoadedByName), unknownValue(context, ValueFlowUnknownSource::ContentsOfEscapedObject));
        context.add(context.nodes().ofHost(ValueFlowHostNode::ExportsOfEntriesStoredInModuleCache), unknownValue(context, ValueFlowUnknownSource::ContentsOfEscapedObject));
    }
}

void applyEventsOfRead(ValueFlowRuleContext& context, ClimbingSet receivers)
{
    // Where the host runs files of its own the run time refuses the entry of a module that the build has closed.
    if (context.refusedCode().hostRunsFilesOutsideImage || !context.contains(receivers, context.universe().objectOf(Builtins::ModuleCache)))
        return;
    raiseHostFlag(context, ValueFlowHostNode::UnseenCopiesOfFilesMayRun);
    raiseHostFlag(context, ValueFlowHostNode::EntriesOfModuleCacheAreRead);
}

// In a test run a file taken out of require.cache loads again, through the loader of the host.
void applyEventsOfDelete(ValueFlowRuleContext& context, ClimbingSet receivers)
{
    if (!context.contains(receivers, context.universe().objectOf(Builtins::ModuleCache)))
        return;
    raiseHostFlag(context, ValueFlowHostNode::UnseenCopiesOfFilesMayRun);
    bool mayEvict = std::ranges::any_of(context.program().modules, [](const Module& module) {
        return module.flags.contains(ModuleFlags::MayBeEvicted);
    });
    if (mayEvict)
        enterLoaderOfHost(context);
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

static bool mayProgramHaveStoredUnder(ValueFlowRuleContext& context, BuiltinID member)
{
    ClimbingSet object = only(context, context.universe().objectOf(member));
    return context.holdsAnyOf(object, context.nodes().ofHost(ValueFlowHostNode::WrittenBuiltins)) || context.holdsAnyOf(object, context.nodes().ofHost(ValueFlowHostNode::MembersStoredByNameThroughPlaceholders));
}

static bool isSeenToStoreLoaderFunction(ValueFlowRuleContext& context, BuiltinID member)
{
    return context.holdsAnyOf(only(context, context.universe().objectOf(member)), context.nodes().ofHost(ValueFlowHostNode::LoaderFunctionsSeenToBeStored));
}

// Whoever is handed the object by the host takes the loaders off it under every name that the program spells.
static void handOverLoadersOn(ValueFlowRuleContext& context, ValueFlowObjectID holder)
{
    const ValueFlowUniverse& universe = context.universe();
    const BuiltinTable& builtins = context.program().builtins();
    ModuleID module = universe.moduleOf(holder);
    Vector<ValueFlowObjectID, 8> loaders;
    Vector<ValueFlowObjectID, 8> named;
    for (BuiltinID member : builtins.membersOf(universe.builtinOf(holder))) {
        BuiltinID canonical = builtins.canonicalID(member);
        if (!builtins.letsInUnseenCode(canonical) || !context.program().keyOf(builtins.at(member).name))
            continue;
        ValueFlowObjectID ofModule = module ? universe.objectOf(canonical, module) : ValueFlowObjectID();
        loaders.append(ofModule ? ofModule : universe.objectOf(canonical));
        named.append(universe.objectOf(canonical));
    }
    context.add(ValueFlowProgramNodes::escaped(), ClimbingSet(context.maker().of(loaders.span())));
    context.add(context.nodes().ofHost(ValueFlowHostNode::LoadersAndEvaluatorsThatProgramNames), ClimbingSet(context.maker().of(named.span())));
}

bool askLoaderFunctionsOfProgram(ValueFlowRuleContext& context, ModuleID asking, RequestAtRunTime request)
{
    const BuiltinTable& builtins = context.program().builtins();
    bool asksProgram = false;
    for (const BuiltinMarkedEntry& resolver : builtins.marked(BuiltinMark::IsResolverThatLoaderCalls)) {
        if (!isSeenToStoreLoaderFunction(context, resolver.builtin))
            continue;
        asksProgram = true;
        ValueFlowObjectID holder = context.universe().objectOf(builtins.at(resolver.builtin).holder);
        context.add(ValueFlowProgramNodes::escaped(), only(context, holder));
        handOverLoadersOn(context, holder);
    }
    if (request == RequestAtRunTime::Require) {
        for (const BuiltinMarkedEntry& require : builtins.marked(BuiltinMark::IsRequireThatLoaderCalls))
            asksProgram |= isSeenToStoreLoaderFunction(context, require.builtin);
    }
    if (!asksProgram || !asking)
        return asksProgram;
    exposeModuleObjectOf(context, asking);
    if (ValueFlowObjectID moduleObject = moduleObjectOfEvaluatedModule(context, asking))
        handOverLoadersOn(context, moduleObject);
    return true;
}

static bool mayMachineCodeRun(ValueFlowRuleContext& context)
{
    Vector<ValueFlowObjectID, 32> loaders;
    for (const BuiltinLoaderOfMachineCode& loader : context.program().builtins().loadersOfMachineCode()) {
        if (!context.program().builtins().has(BuiltinMark::LoadsCodeWithoutJSAccess, loader.callee))
            context.universe().appendObjectsOf(loader.callee, loaders);
    }
    return context.holdsAnyOf(ClimbingSet(context.maker().of(loaders.span())), context.nodes().ofHost(ValueFlowHostNode::RunBuiltins));
}

static void exposeModuleObjectsOfEntryPoints(ValueFlowRuleContext& context)
{
    ClimbingSet process = only(context, context.universe().objectOf(context.program().builtins().at(Builtins::ProcessMainModule).holder));
    if (!isHostFlagRaised(context, ValueFlowHostNode::UnseenCodeRuns) && !mayMachineCodeRun(context) && !context.holdsAnyOf(process, ValueFlowProgramNodes::escaped()) && !context.holdsAnyOf(process, ValueFlowProgramNodes::lost()))
        return;
    bool mayBeNoCommonJSFile = storesPathOfMainModule(context);
    for (size_t index : modulesThatMayBeMain(context)) {
        exposeModuleObjectOf(context, ModuleID(static_cast<uint32_t>(index)));
        if (mayBeNoCommonJSFile)
            showAnswerOfRequireInModuleObject(context, ModuleID(static_cast<uint32_t>(index)));
    }
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

void appendLoadersAndEvaluatorsReturnedBy(const ValueFlowUniverse& universe, const BuiltinTable& builtins, BuiltinID builtin, Vector<ValueFlowObjectID, 32>& returned)
{
    for (const BuiltinOverload& overload : builtins.overloadsOf(builtin)) {
        for (const BuiltinTypePart& part : builtins.at(overload.result.type).parts) {
            if (part.kind == BuiltinTypePartKind::Builtin && builtins.letsInUnseenCodeWhenCalled(builtins.canonicalID(part.builtin)))
                universe.appendObjectsOf(builtins.canonicalID(part.builtin), returned);
        }
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
        appendLoadersAndEvaluatorsReturnedBy(context.universe(), builtins, builtin, returned);
        context.add(ValueFlowProgramNodes::escaped(), ClimbingSet(context.maker().of(returned.span())));
        context.add(context.nodes().ofHost(ValueFlowHostNode::LoadersAndEvaluatorsThatProgramNames), ClimbingSet(context.maker().of(returned.span())));
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
    context.forEachMemberInLine(escaped, [&](ValueFlowObjectID object) {
        BuiltinID builtin = universe.builtinOf(object);
        if (!builtin)
            return;
        callsLoaderThatHasEscaped |= callsWhateverHasEscaped && builtins.at(builtins.canonicalID(builtin)).flags.contains(BuiltinFlag::LoadsModuleByName);
        if (std::optional<RequestAtRunTime> request = callsWhateverHasEscaped ? requestMadeByCalling(builtins, builtins.canonicalID(builtin)) : std::nullopt)
            askLoaderFunctionsOfProgram(context, universe.moduleOf(object), *request);
        // A member of a holder of the realm that has escaped has not escaped with it: the program has to have taken it off its holder.
        bool isInHandsOfProgram = builtins.at(builtin).origin != BuiltinOrigin::Host || context.contains(context.valueOf(context.nodes().ofHost(ValueFlowHostNode::LoadersAndEvaluatorsThatProgramNames)), universe.objectOf(builtins.canonicalID(builtin)));
        if (callsWhateverHasEscaped && builtins.letsInUnseenCode(builtins.canonicalID(builtin)) && context.contains(handedOut, object) && isInHandsOfProgram)
            callWhatLetsInUnseenCode(context, object);
        bool isOfLanguage = builtins.at(builtin).origin != BuiltinOrigin::Host;
        if (!isOfLanguage && callsWhateverHasEscaped && context.contains(takenFromHolders, object)) {
            calledFunctionsOfHost.append(object);
            exposeObjectsOfModulesReturnedBy(context, builtin);
        }
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
    if (callsWhateverHasEscaped || isHostFlagRaised(context, ValueFlowHostNode::MemberNobodyKnowsIsCalled))
        callBuiltinsThatReadSpecies(context, context.restrict(escaped, universe.builtinFunctionsThatReadSpecies()));
    ClimbingSet escapedOfLanguage(context.maker().of(escapedObjectsOfLanguage.span()));
    ClimbingSet used = context.unite(context.valueOf(context.nodes().ofHost(ValueFlowHostNode::UsedBuiltins)), escapedOfLanguage);
    if (ValueFlowObjectID moduleCache = universe.objectOf(Builtins::ModuleCache); !context.refusedCode().hostRunsFilesOutsideImage && (context.contains(used, moduleCache) || context.contains(handedOut, moduleCache)))
        raiseHostFlag(context, ValueFlowHostNode::UnseenCopiesOfFilesMayRun);
    if (context.contains(handedOut, universe.objectOf(Builtins::ModuleCache)))
        context.add(context.nodes().ofHost(ValueFlowHostNode::ExportsOfEntriesStoredInModuleCache), unknownValue(context, ValueFlowUnknownSource::ContentsOfEscapedObject));
    run = context.unite(run, callsWhateverHasEscaped ? escapedOfLanguage : context.valueWithin(escapedOfLanguage, context.nodes().ofHost(ValueFlowHostNode::BuiltinsThatStatementsName)));
    run = context.unite(run, ClimbingSet(context.maker().of(calledFunctionsOfHost.span())));
    for (ValueFlowObjectID function : calledFunctionsOfHost) {
        context.use(universe.builtinOf(function), BuiltinUse::Called);
        passUnknownOperandsTo(context, universe.builtinOf(function));
    }

    context.forEachMemberInLine(used, [&](ValueFlowObjectID object) {
        BuiltinID builtin = universe.builtinOf(object);
        if (!builtin)
            return;
        if (context.contains(run, object) && loadsCodeOnceRead(builtins, builtin, context.refusedCode()))
            handLoadedModulesToNativeCode(context);
        attachments.addRowsOf(builtin);
        if (!isObjectKind(builtins.at(builtin).kind) || membersFollowHolders || context.contains(run, object))
            attachments.addCodeOf(builtin);
    });
    context.forEachMemberInLine(context.valueOf(context.nodes().ofHost(ValueFlowHostNode::WrittenBuiltins)), [&](ValueFlowObjectID object) {
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
    context.forEachMemberInLine(escaped, [&](ValueFlowObjectID object) {
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
    if (context.refusedCode().hostRunsFilesOutsideImage) {
        raiseHostFlag(context, ValueFlowHostNode::CodeOutsideImageIsLoaded);
        letUnseenCodeRun(context, UnseenCodeReach::Loader);
        context.add(ValueFlowProgramNodes::escaped(), context.universe().holdersOfPublicLibraryExports());
    }
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
        // Unseen code can read process.versions too, and gets the same object.
        if (entry.scope == BuiltinScope::Realm && entry.holder && builtins.at(entry.holder).kind != BuiltinKind::Prototype && builtins.isUnder(BuiltinID(index), Builtins::GlobalThis)) {
            exposeNativeCellsReturnedBy(context, BuiltinID(index));
            exposeObjectsOfModulesReturnedBy(context, BuiltinID(index));
        }
    }
    // No object of the host escapes with the global object, which has them all. What the program lays on one, unseen code reads there, and it runs the accessors that the program defines there.
    for (ValueFlowNameKind kind : { ValueFlowNameKind::EveryProperty, ValueFlowNameKind::EveryGetter, ValueFlowNameKind::EverySetter })
        context.connect(context.readWhatWasWritten(ClimbingSet(context.maker().of(objectsOfHost.span())), { kind, 0 }), ValueFlowProgramNodes::escaped());
    if (isHostFlagRaised(context, ValueFlowHostNode::UnseenCodeChangesRealm))
        letAnythingHappenTo(context, ClimbingSet(context.maker().of(objectsOfHost.span())));
    bool filesOfProgramAreLoadedByName = isHostFlagRaised(context, ValueFlowHostNode::ModulesAreRequiredByName) || isHostFlagRaised(context, ValueFlowHostNode::ModulesAreImportedByName);
    if (filesOfProgramAreLoadedByName && isHostFlagRaised(context, ValueFlowHostNode::UnseenCodeHasLoader))
        handLoadedModulesToNativeCode(context);
    else
        handLoadableModulesToText(context, filesOfProgramAreLoadedByName || context.refusedCode().hostRunsFilesOutsideImage);

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

// In a test run require.cache lists the files that have run, each with what require() answers for it. A file that is taken out of it, or loaded under a query,
// runs again as code that nobody has seen, and imports what its text names.
static bool copiesOfFilesMayRun(ValueFlowRuleContext& context)
{
    return context.holdsAnything(context.nodes().ofHost(ValueFlowHostNode::UnseenCopiesOfFilesMayRun));
}

// Goes by the flags and requests of the modules, which stay as they are.
static void connectWhatCopiesOfFilesReach(ValueFlowRuleContext& context)
{
    const Program& program = context.program();
    const ValueFlowProgramNodes& nodes = context.nodes();
    // A file that has not run yet and imports one that was taken out, itself or through others, is left to the loader of the host as well.
    BitVector mayRunAsCopy(program.modules.size());
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        if (program.modules[index].flags.contains(ModuleFlags::MayBeEvicted))
            mayRunAsCopy.quickSet(index);
    }
    for (bool hasFoundMore = true; hasFoundMore;) {
        hasFoundMore = false;
        for (uint32_t index = 0; index < program.modules.size(); ++index) {
            const Module& module = program.modules[index];
            if (mayRunAsCopy.quickGet(index) || !isWrittenByProgram(program, module))
                continue;
            if (std::ranges::any_of(module.requests, [&](const ModuleRequest& request) { return mayRunAsCopy.quickGet(request.target.index()); })) {
                mayRunAsCopy.quickSet(index);
                hasFoundMore = true;
            }
        }
    }
    // The original beside a mock runs as a copy while the compiled file and its importers stay what they are. `jest.requireActual()` goes through the loader of the host.
    BitVector isReached = mayRunAsCopy;
    bool hasOriginals = false;
    for (const Variable& variable : program.variables) {
        if (variable.flags.contains(VariableFlags::HoldsMock) && !program.at(variable.module).runtimeModule) {
            mayRunAsCopy.quickSet(variable.module.index());
            hasOriginals = true;
        }
    }
    if (hasOriginals)
        enterLoaderOfHost(context);
    // A copy reads the names of the global scope as the compiled file does, and calls what the program has laid there. It comes in by the entry that everybody may use.
    if (!mayRunAsCopy.isEmpty())
        context.add(ValueFlowProgramNodes::escaped(), only(context, context.universe().objectOf(Builtins::GlobalThis)));
    for (size_t index : mayRunAsCopy) {
        for (const ModuleRequest& request : program.modules[index].requests) {
            if (isWrittenByProgram(program, program.at(request.target)))
                isReached.quickSet(request.target.index());
        }
    }
    // What the text of a copy names in a call, import() or require(), is no request of its module. The front end marks it.
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        if (program.modules[index].flags.contains(ModuleFlags::IsFoundByName) && isWrittenByProgram(program, program.modules[index]))
            isReached.quickSet(index);
    }
    ValueFlowNodeID reached = nodes.ofHost(ValueFlowHostNode::ReachedByUnseenCopiesOfFiles);
    for (size_t index : isReached) {
        ModuleID id(static_cast<uint32_t>(index));
        // The host begins a file that nothing has run yet, and perhaps no compiled code ever runs it.
        if (program.at(id).kind != ModuleKind::External)
            evaluateModule(context, id);
        connect(context, requiredValueOf(context, id), reached);
    }
}

static void handOnWhatCopiesOfFilesReach(ValueFlowRuleContext& context)
{
    const Program& program = context.program();
    const ValueFlowProgramNodes& nodes = context.nodes();
    ValueFlowNodeID reached = nodes.ofHost(ValueFlowHostNode::ReachedByUnseenCopiesOfFiles);
    // A copy links to the names of a namespace object and takes what they hold. It shows the object itself to nobody, so no spy that is out of sight gets hold of it.
    ValueSetID namespaceObjects = context.universe().namespaces();
    if (context.holdsAnything(nodes.ofHost(ValueFlowHostNode::EntriesOfModuleCacheAreRead))) {
        context.connect(reached, ValueFlowProgramNodes::escaped());
        // The table has an entry for every file of the program that has run, whether or not anything takes the file out.
        for (uint32_t index = 0; index < program.modules.size(); ++index) {
            if (program.modules[index].flags.contains(ModuleFlags::IsListedInCache))
                connect(context, requiredValueOf(context, ModuleID(index)), ValueFlowProgramNodes::escaped());
        }
        for (ModuleID listed : libraryModulesListedInRequireCacheForStatements(program))
            connect(context, namespaceOf(context, listed), ValueFlowProgramNodes::escaped());
    } else {
        ClimbingSet linkedTo = context.restrict(context.valueOf(reached), namespaceObjects);
        context.connect(context.masked(reached, ValueSets::complement(namespaceObjects)), ValueFlowProgramNodes::escaped());
        context.connect(context.readWhatWasWritten(linkedTo, { ValueFlowNameKind::EveryProperty, 0 }), ValueFlowProgramNodes::escaped());
        context.mark(linkedTo, ObjectMutation::ReachNativeCode);
    }
}

void runModulesLoadedByName(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    uint32_t numberOfParts = numberOfPartsLoadedByName(context.program());
    bool isWhole = numberOfParts == 1;
    if (!isWhole && partOf(statement) < numberOfStatementsOnCopiesOfFiles) {
        if (!copiesOfFilesMayRun(context))
            return;
        if (!partOf(statement))
            connectWhatCopiesOfFilesReach(context);
        else
            handOnWhatCopiesOfFilesReach(context);
        return;
    }
    // What else is not said of one module stands behind the walk, in the last part.
    uint32_t part = isWhole ? 0 : partOf(statement) - numberOfStatementsOnCopiesOfFiles;
    bool isLastPart = part + 1 == numberOfParts;
    bool areRequired = isHostFlagRaised(context, ValueFlowHostNode::ModulesAreRequiredByName);
    bool areImported = isHostFlagRaised(context, ValueFlowHostNode::ModulesAreImportedByName);
    bool libraryIsRequired = isHostFlagRaised(context, ValueFlowHostNode::LibraryModulesAreRequiredByName);
    bool areLoaded = areRequired || areImported || libraryIsRequired;
    const ValueFlowProgramNodes& nodes = context.nodes();
    // Native code gets the modules also where a source of unknown values is left out and no flag counts as raised.
    bool areHandedToNativeCode = context.holdsAnything(nodes.ofHost(ValueFlowHostNode::LoadedModulesAreHandedToNativeCode));
    if (isWhole && copiesOfFilesMayRun(context)) {
        connectWhatCopiesOfFilesReach(context);
        handOnWhatCopiesOfFilesReach(context);
    }
    auto enterLoaderForSpelledNames = [&] {
        if (isLastPart && isHostFlagRaised(context, ValueFlowHostNode::LibraryModulesAreLoadedUnderSpelledNames))
            enterLoaderOfHost(context);
    };
    if (!areLoaded && !areHandedToNativeCode) {
        enterLoaderForSpelledNames();
        return;
    }

    uint32_t first = part * modulesOfPartLoadedByName;
    uint32_t end = std::min<uint32_t>(first + modulesOfPartLoadedByName, context.program().modules.size());
    forEachModuleLoadableByName(context, first, end, [&](ModuleID id, bool isOfProgram) {
        bool isLoaded = areLoaded && (!isOfProgram || areRequired || areImported);
        if (isLoaded) {
            context.add(nodes.ofHost(ValueFlowHostNode::ModulesOfSeenCode), only(context, context.universe().namespaceOf(id)));
            if (isOfProgram || isAskedOfHostByName(context.program().at(id)))
                evaluateModule(context, id);
        }
        ValueFlowNodeID hands = isOfProgram ? ValueFlowProgramNodes::escaped() : nodes.ofHost(ValueFlowHostNode::HeldByNativeCode);
        bool isRequired = isLoaded && areRequired;
        bool isRequiredFromLibrary = isLoaded && libraryIsRequired && !isOfProgram;
        if (areHandedToNativeCode || isRequired || isRequiredFromLibrary) {
            ValueFlowSources required = requiredValueOf(context, id);
            if (areHandedToNativeCode)
                connect(context, required, hands);
            if (isRequired) {
                connect(context, required, nodes.ofHost(ValueFlowHostNode::RequiredValuesLoadedByName));
                if (ValueFlowObjectID moduleObject = moduleObjectOfEvaluatedModule(context, id))
                    context.add(nodes.ofHost(ValueFlowHostNode::ChildrenOfModuleObjects), only(context, moduleObject));
            }
            if (isRequiredFromLibrary)
                connect(context, required, nodes.ofHost(ValueFlowHostNode::LibraryValuesLoadedByName));
        }
        bool isImported = isLoaded && areImported;
        if (areHandedToNativeCode || isImported) {
            ValueFlowSources namespaceObject = namespaceOf(context, id);
            if (areHandedToNativeCode)
                connect(context, namespaceObject, hands);
            if (isImported)
                connect(context, namespaceObject, nodes.ofHost(ValueFlowHostNode::NamespacesLoadedByName));
        }
    });
    if (!areLoaded) {
        enterLoaderForSpelledNames();
        return;
    }
    if (!isLastPart)
        return;

    ClimbingSet nativeModules = publicNativeModules(context);
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
