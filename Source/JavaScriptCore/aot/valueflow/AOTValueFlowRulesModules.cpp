/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRulesModules.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTValueFlowBuiltins.h"
#include "AOTValueFlowRules.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr ValueFlowName unknownKey { ValueFlowNameKind::UnknownKey, 0 };
static constexpr ValueFlowName everyProperty { ValueFlowNameKind::EveryProperty, 0 };
static constexpr ValueFlowName everyGetter { ValueFlowNameKind::EveryGetter, 0 };
static constexpr ValueFlowName copiedFrom { ValueFlowNameKind::CopiedFrom, 0 };
static constexpr ValueFlowName awaitedValue { ValueFlowNameKind::AwaitedValue, 0 };

static ClimbingSet only(ValueFlowRuleContext& context, ValueFlowObjectID object)
{
    return ClimbingSet(context.maker().of(object));
}

static ClimbingSet unknownObject(ValueFlowRuleContext& context, ValueFlowUnknownSource source)
{
    return context.restrict(ClimbingSet(unknownValue(context, source)), context.universe().objects());
}

static bool isInApplication(const Program& program, FunctionID function)
{
    return isWrittenInProgram(program.at(function));
}

static ValueFlowName slot(BuiltinSlotID slot)
{
    return { ValueFlowNameKind::Slot, slot.index() };
}

// No operation of the program spells a name that has no key, so only an access under a key nobody knows finds what lies under it.
static ValueFlowName writtenNameOf(const Program& program, StringID text)
{
    std::optional<PropertyKeyID> key = program.types.find({ .kind = PropertyKeyKind::Text, .text = text });
    return key ? ValueFlowName::property(*key) : unknownKey;
}

void connect(ValueFlowRuleContext& context, const ValueFlowSources& sources, ValueFlowNodeID target)
{
    if (!target)
        return;
    for (ValueFlowNodeID node : sources.nodes)
        context.connect(node, target);
    context.add(target, sources.constant);
}

ClimbingSet valueOf(ValueFlowRuleContext& context, const ValueFlowSources& sources)
{
    ClimbingSet value = sources.constant;
    for (ValueFlowNodeID node : sources.nodes)
        value = context.unite(value, context.valueOf(node));
    return value;
}

static void append(ValueFlowRuleContext& context, ValueFlowSources& sources, const ValueFlowSources& others)
{
    sources.nodes.appendVector(others.nodes);
    sources.constant = context.unite(sources.constant, others.constant);
}

static ValueFlowSources sourcesOf(ClimbingSet constant)
{
    return { { }, constant };
}

static ValueFlowSources sourcesOf(ValueFlowNodeID node)
{
    return { { node }, { } };
}

static ValueFlowSources unknownSources(ValueFlowRuleContext& context, ValueFlowUnknownSource source)
{
    return sourcesOf(ClimbingSet(unknownValue(context, source)));
}

// Where a value comes from that the loader has for the module and that no code of the image makes.
static ValueFlowUnknownSource sourceOfWhatLoaderMakes(const Module& module)
{
    switch (module.kind) {
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
        return ValueFlowUnknownSource::UndescribedNativeResult;
    case ModuleKind::External:
        switch (module.externalKind) {
        case ExternalKind::UncompiledCode:
            return ValueFlowUnknownSource::UnseenCode;
        case ExternalKind::HostValue:
            return ValueFlowUnknownSource::UndescribedNativeResult;
        }
        RELEASE_ASSERT_NOT_REACHED();
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
        return ValueFlowUnknownSource::DataFromOutside;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ModuleID moduleOf(const Program& program, RuntimeModuleID runtimeModule)
{
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        if (program.modules[index].runtimeModule == runtimeModule)
            return ModuleID(index);
    }
    return { };
}

static const ModuleExport* findExport(const Module& module, StringID name)
{
    auto found = std::ranges::find(module.exports, name, &ModuleExport::name);
    return found == module.exports.end() ? nullptr : &*found;
}

static bool hasCode(const Program& program, const Module& module)
{
    return module.body && program.at(module.body).hasBody();
}

bool awaitsUse(const Program& program, ModuleID module)
{
    return program.at(module).flags.contains(ModuleFlags::HasNoSideEffects) && hasCode(program, program.at(module));
}

void useModule(ValueFlowRuleContext& context, ModuleID module)
{
    if (awaitsUse(context.program(), module))
        evaluateModule(context, module);
}

// The importer may have a name from a module that exports it again.
static bool exportsUsedNameAgain(ValueFlowRuleContext& context, ModuleID id)
{
    const Program& program = context.program();
    for (const ModuleExport& entry : program.at(id).exports) {
        if (entry.kind != ModuleExportKind::Variable || !entry.variable)
            continue;
        ModuleID holder = program.at(entry.variable).module;
        if (holder != id && (!awaitsUse(program, holder) || context.isReached(program.at(holder).body)))
            return true;
    }
    return false;
}

// A `require` is an operation in the code of the requirer, and evaluates its target where it stands: the request for it reaches nothing.
static bool isEvaluatedWithImporter(ValueFlowRuleContext& context, const ModuleRequest& request)
{
    const Program& program = context.program();
    if (request.kind == ModuleRequestKind::Require)
        return false;
    // The names of a dynamic star export are those that the evaluated module has.
    if (request.isDynamicStarExport || !awaitsUse(program, request.target))
        return true;
    return context.isReached(program.at(request.target).body) || exportsUsedNameAgain(context, request.target);
}

static bool hasExportTable(ModuleKind kind)
{
    switch (kind) {
    case ModuleKind::SourceText:
    case ModuleKind::Data:
        return true;
    case ModuleKind::CommonJs:
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The run time adds the names that it finds on the value of such a module, or of the target of a dynamic star export, when it links the namespace object.
static bool mayHaveExportsNobodyLists(const Module& module)
{
    if (module.kind != ModuleKind::SourceText || module.exportRule != SyntheticExportRule::None)
        return true;
    return module.requests.containsIf([](const ModuleRequest& request) {
        return request.isDynamicStarExport;
    });
}

static ValueFlowSources exportedValueOf(ValueFlowRuleContext& context, const ModuleExport& entry)
{
    switch (entry.kind) {
    case ModuleExportKind::Variable:
        if (!entry.variable)
            return unknownSources(context, ValueFlowUnknownSource::DataFromOutside);
        useModule(context, context.program().at(entry.variable).module);
        return sourcesOf(context.nodes().variable(entry.variable));
    case ModuleExportKind::Namespace:
        return namespaceOf(context, entry.module, entry.phase);
    case ModuleExportKind::Indirect:
        // The name is that of a module that stays outside the image.
        useModule(context, entry.module);
        return unknownSources(context, sourceOfWhatLoaderMakes(context.program().at(entry.module)));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

// What the loader reads from the values themselves: it goes no further up.
struct OwnProperties {
    // What lies in the objects that the analysis follows. A lookup through one of them finds this much too.
    ValueFlowSources held;
    // What their getters answer, which run with the object as `this`, and what an object holds that nobody follows.
    ValueFlowSources others;
};

} // anonymous namespace

ClimbingSet withSourcesOfCopies(ValueFlowRuleContext& context, ClimbingSet objects)
{
    ClimbingSet all = objects;
    ClimbingSet last = objects;
    for (;;) {
        last = context.restrict(context.valueOf(context.read(last, copiedFrom)), context.universe().knownObjects());
        bool findsNewSource = false;
        context.forEachMember(last, [&](ValueFlowObjectID source) {
            findsNewSource |= !context.contains(all, source);
        });
        if (!findsNewSource)
            return all;
        all = context.unite(all, last);
    }
}

// Without a key: every own property.
static OwnProperties readOwnProperties(ValueFlowRuleContext& context, ClimbingSet values, std::optional<PropertyKeyID> key)
{
    const ValueFlowUniverse& universe = context.universe();
    ValueFlowName valueName = key ? ValueFlowName::property(*key) : everyProperty;
    ValueFlowName getterName = key ? ValueFlowName::getter(*key) : everyGetter;
    ClimbingSet receivers = context.restrict(values, universe.knownObjects());
    // A spread has values alone: the getters of a source have run when it was made, and what they have answered lies on the target. A definition by the descriptors of a source has its getters too.
    ClimbingSet holders = withSourcesOfCopies(context, receivers);

    OwnProperties properties;
    properties.held = sourcesOf(context.read(holders, valueName));
    ClimbingSet getters = context.valueOf(context.read(holders, getterName));
    // A store through a placeholder may hit whatever the placeholder stands for.
    auto readWhatIsStoredThrough = [&](ValueSetID placeholder, ValueFlowNodeID covered) {
        if (!context.holdsAnyOf(holders, covered))
            return;
        properties.held.nodes.append(context.read(ClimbingSet(placeholder), valueName));
        getters = context.unite(getters, context.valueOf(context.read(ClimbingSet(placeholder), getterName)));
    };
    readWhatIsStoredThrough(universe.unknownObjectAlone(), ValueFlowProgramNodes::escaped());
    readWhatIsStoredThrough(universe.untrackedObjectAlone(), ValueFlowProgramNodes::lost());

    // What a built-in object of the realm has of its own is in the table, which a lookup knows.
    bool readsTable = false;
    context.forEachMember(context.restrict(holders, universe.builtinObjects()), [&](ValueFlowObjectID object) {
        readsTable |= universe.kindOf(object) == ValueFlowObjectKind::Builtin;
    });
    if (readsTable)
        properties.held.nodes.append(context.lookup(context.restrict(holders, universe.builtinObjects()), valueName));

    if (context.contains(values, ValueFlowUniverse::unknownObject()))
        properties.others.constant = ClimbingSet(unknownValue(context, ValueFlowUnknownSource::ContentsOfEscapedObject));
    // The untracked object may be any object of the program: a lookup through it answers by the name alone, and calls the getters that it finds.
    if (context.contains(values, ValueFlowUniverse::untrackedObject()))
        properties.others.nodes.append(context.lookup(ClimbingSet(universe.untrackedObjectAlone()), valueName));

    forEachPartOfCallees(context, context.restrict(getters, universe.callables()), [&](ClimbingSet callees) {
        ValueFlowCallGroup group = context.group(callees, ValueFlowCallKind::Get);
        context.add(group.thisValue, receivers);
        context.connect(group.thrown, ValueFlowProgramNodes::escaped());
        properties.others.nodes.append(group.result);
    });
    return properties;
}

static ValueFlowSources ownPropertyOf(ValueFlowRuleContext& context, ClimbingSet values, std::optional<PropertyKeyID> key)
{
    OwnProperties properties = readOwnProperties(context, values, key);
    append(context, properties.held, properties.others);
    return properties.held;
}

// What `module.exports` holds, or what the body of a module of the library returns.
static ValueFlowSources moduleExportsValueOf(ValueFlowRuleContext& context, ModuleID id)
{
    const Program& program = context.program();
    const Module& module = program.at(id);
    switch (module.kind) {
    case ModuleKind::CommonJs:
        // A file whose exports are variables, one for each name, has no object that anybody could be answered.
        if (!module.exportsVariable)
            return hasCode(program, module) ? sourcesOf(ClimbingSet(context.universe().undefinedValue())) : unknownSources(context, sourceOfWhatLoaderMakes(module));
        useModule(context, id);
        return sourcesOf(context.nodes().variable(module.exportsVariable));
    case ModuleKind::RuntimeSource:
        if (hasCode(program, module) && !program.at(module.body).results.isEmpty())
            return sourcesOf(context.nodes().result(module.body, 0));
        return unknownSources(context, sourceOfWhatLoaderMakes(module));
    case ModuleKind::SourceText:
        // The loader fills the names of such a module from what the CommonJS file behind it exports.
        if (ModuleID captured = commonJsModuleCapturedBy(program, module))
            return moduleExportsValueOf(context, captured);
        return unknownSources(context, sourceOfWhatLoaderMakes(module));
    case ModuleKind::Data:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return unknownSources(context, sourceOfWhatLoaderMakes(module));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ValueFlowSources defaultExportOf(ValueFlowRuleContext& context, ModuleID id)
{
    const Program& program = context.program();
    const Module& module = program.at(id);
    if (!hasExportTable(module.kind))
        return moduleExportsValueOf(context, id);
    if (const ModuleExport* entry = findExport(module, program.stringOf(BuiltinNames::defaultKeyword)))
        return exportedValueOf(context, *entry);
    // The loader parses the file of a module of data, whether or not the program holds a variable for what comes out.
    if (module.kind == ModuleKind::Data)
        return unknownSources(context, ValueFlowUnknownSource::DataFromOutside);
    return sourcesOf(ClimbingSet(context.universe().undefinedValue()));
}

static ValueFlowSources runtimeNamespaceOf(ValueFlowRuleContext& context, RuntimeModuleID runtimeModule)
{
    const ValueFlowUniverse& universe = context.universe();
    if (BuiltinID namespaceObject = context.program().builtins().namespaceOf(runtimeModule))
        return sourcesOf(only(context, universe.objectOf(namespaceObject)));
    return sourcesOf(unknownObject(context, ValueFlowUnknownSource::UndescribedNativeResult));
}

ValueFlowSources requiredValueOf(ValueFlowRuleContext& context, ModuleID id)
{
    const Program& program = context.program();
    const Module& module = program.at(id);
    switch (module.kind) {
    case ModuleKind::Runtime:
        return runtimeNamespaceOf(context, module.runtimeModule);
    case ModuleKind::External:
        return unknownSources(context, sourceOfWhatLoaderMakes(module));
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
        break;
    }

    switch (module.requireResult) {
    case RequireResult::ModuleExports:
        return moduleExportsValueOf(context, id);
    case RequireResult::DefaultExport:
        return defaultExportOf(context, id);
    case RequireResult::Namespace: {
        // The run time answers the namespace object when this export holds undefined or null.
        ValueFlowSources sources = namespaceOf(context, id);
        BuiltinName name = program.builtins().nameOf(byteCast<uint8_t>("module.exports"_s.span()));
        if (const ModuleExport* entry = name ? findExport(module, program.stringOf(name)) : nullptr)
            append(context, sources, exportedValueOf(context, *entry));
        // A require() that comes to an ES module while another require() evaluates it is answered an object without keys, which the loader makes: Modules::require().
        if (module.kind == ModuleKind::SourceText)
            append(context, sources, sourcesOf(unknownObject(context, ValueFlowUnknownSource::UndescribedNativeResult)));
        return sources;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ValueFlowSources evaluationErrorsOf(ValueFlowRuleContext& context, ModuleID id)
{
    const Module& module = context.program().at(id);
    switch (module.kind) {
    case ModuleKind::Runtime:
        return { };
    case ModuleKind::External:
        return unknownSources(context, module.externalKind == ExternalKind::UncompiledCode ? ValueFlowUnknownSource::UnseenCode : ValueFlowUnknownSource::ThrownByRunTime);
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
        break;
    }

    // What the body of an imported module throws has escaped when that body was entered, and the loader throws where an import cannot be linked.
    bool importsSomething = std::ranges::contains(module.requests, ModuleRequestKind::Import, &ModuleRequest::kind);
    ValueFlowSources sources = importsSomething ? unknownSources(context, ValueFlowUnknownSource::ThrownByRunTime) : ValueFlowSources();
    if (module.body)
        sources.nodes.append(context.nodes().thrown(module.body));
    return sources;
}

// The loader reads every own property of the value of the module when it links the namespace object, so every getter runs, and it hands the value to nobody.
static void tieNamespaceToCapturedNames(ValueFlowRuleContext& context, ModuleID id, Vector<ModuleID, 4>& namespacesBehind)
{
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    const Module& module = program.at(id);
    ClimbingSet namespaceObject = only(context, universe.namespaceOf(id));
    ValueFlowNodeID underAnyName = context.written(namespaceObject, unknownKey);
    ValueFlowNodeID holdersOfNames = context.written(namespaceObject, copiedFrom);

    if (module.exportRule != SyntheticExportRule::None || module.exportsVariable) {
        ValueFlowSources value = moduleExportsValueOf(context, id);
        connect(context, value, context.written(namespaceObject, writtenNameOf(program, program.stringOf(BuiltinNames::defaultKeyword))));
        ClimbingSet values = valueOf(context, value);
        context.add(holdersOfNames, context.restrict(values, universe.knownObjects()));
        connect(context, readOwnProperties(context, values, std::nullopt).others, underAnyName);
    } else if (module.kind != ModuleKind::SourceText)
        context.add(underAnyName, unknownValue(context, sourceOfWhatLoaderMakes(module)));
    // The exports of a module of the host are read off the object that the host answers for it, each of them: whatever the host does to create one of them is done.
    if (module.kind == ModuleKind::Runtime) {
        ClimbingSet answered = valueOf(context, runtimeNamespaceOf(context, module.runtimeModule));
        context.add(holdersOfNames, context.restrict(answered, universe.knownObjects()));
        connect(context, ownPropertyOf(context, answered, std::nullopt), underAnyName);
    }

    // The names of a dynamic star export are those that the namespace object of its target has.
    for (const ModuleRequest& request : module.requests) {
        if (!request.isDynamicStarExport)
            continue;
        const Module& target = program.at(request.target);
        if (target.kind == ModuleKind::External) {
            context.add(underAnyName, unknownValue(context, sourceOfWhatLoaderMakes(target)));
            continue;
        }
        context.add(holdersOfNames, only(context, universe.namespaceOf(request.target)));
        namespacesBehind.append(request.target);
    }
}

// The properties of a namespace object are the variables that its module exports. Answers the modules whose namespace objects are among them or stand behind them.
static void tieNamespaceToExports(ValueFlowRuleContext& context, ModuleID id, Vector<ModuleID, 4>& namespacesInside)
{
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    const Module& module = program.at(id);
    ClimbingSet namespaceObject = only(context, universe.namespaceOf(id));

    for (uint32_t index = 0; index < module.exports.size(); ++index) {
        const ModuleExport& entry = module.exports[index];
        PropertyKeyID key = module.namespaceLayout ? program.at(module.namespaceLayout).fields[index].key : PropertyKeyID();
        ValueFlowNodeID location = context.written(namespaceObject, key ? ValueFlowName::property(key) : writtenNameOf(program, entry.name));
        switch (entry.kind) {
        case ModuleExportKind::Variable:
        case ModuleExportKind::Indirect:
            connect(context, exportedValueOf(context, entry), location);
            break;
        case ModuleExportKind::Namespace: {
            if (program.at(entry.module).kind == ModuleKind::External) {
                context.add(location, unknownObject(context, sourceOfWhatLoaderMakes(program.at(entry.module))));
                break;
            }
            ClimbingSet exportedNamespace = only(context, universe.namespaceOf(entry.module));
            namespacesInside.append(entry.module);
            if (entry.phase == ImportPhase::Evaluation) {
                context.add(location, exportedNamespace);
                break;
            }
            context.add(ValueFlowProgramNodes::escaped(), exportedNamespace);
            context.add(location, unknownObject(context, ValueFlowUnknownSource::UndescribedNativeResult));
            break;
        }
        }
    }

    bool hasCapturedNames = mayHaveExportsNobodyLists(module);
    for (const SyntheticExport& entry : module.syntheticExports) {
        if (entry.variable)
            context.connect(context.nodes().variable(entry.variable), context.written(namespaceObject, writtenNameOf(program, entry.name)));
        else
            hasCapturedNames = true;
    }

    auto addPrimitivesUnder = [&](BuiltinName name) {
        if (PropertyKeyID key = name ? program.keyOf(name) : PropertyKeyID())
            context.add(context.written(namespaceObject, ValueFlowName::property(key)), universe.anyPrimitive());
    };
    addPrimitivesUnder(program.builtins().nameOf(WellKnownSymbol::ToStringTag));
    // `require` and a store mark a namespace object that has no export of this name.
    addPrimitivesUnder(BuiltinNames::__esModule);
    if (hasCapturedNames)
        tieNamespaceToCapturedNames(context, id, namespacesInside);
}

ValueFlowSources namespaceOf(ValueFlowRuleContext& context, ModuleID first, ImportPhase phase)
{
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    if (program.at(first).kind == ModuleKind::External)
        return sourcesOf(unknownObject(context, sourceOfWhatLoaderMakes(program.at(first))));

    // Whoever ties a module ties the modules inside and behind it in the same run.
    bool staysForGood = context.isStatementOfBody();
    ValueFlowNodeID tiedForGood = context.nodes().ofHost(ValueFlowHostNode::NamespacesTiedByStatementsOfBodies);
    ValueSetID tiedByThisStatement = staysForGood && context.rememberedByStatement() ? context.rememberedByStatement() : ValueSets::nothing;
    Vector<ValueFlowObjectID, 4> tiedNow;
    BitVector visited;
    Vector<ModuleID, 4> pending { first };
    while (!pending.isEmpty()) {
        ModuleID id = pending.takeLast();
        if (visited.set(id.index()))
            continue;
        ClimbingSet created = only(context, universe.namespaceOf(id));
        if (!context.sets().contains(tiedByThisStatement, universe.namespaceOf(id))) {
            if (context.wasAddedInEarlierRound(tiedForGood, created))
                continue;
            tiedNow.append(universe.namespaceOf(id));
        }
        if (staysForGood)
            context.add(tiedForGood, created);
        context.add(context.nodes().ofHost(ValueFlowHostNode::CreatedNamespaces), only(context, universe.namespaceOf(id)));
        useModule(context, id);
        tieNamespaceToExports(context, id, pending);
    }
    if (staysForGood && !tiedNow.isEmpty())
        context.rememberedByStatement() = context.maker().unite(tiedByThisStatement, context.maker().of(tiedNow.span()));

    ClimbingSet namespaceObject = only(context, universe.namespaceOf(first));
    switch (phase) {
    case ImportPhase::Evaluation:
        return sourcesOf(namespaceObject);
    case ImportPhase::Defer:
        // The deferred namespace object is another object, which nobody follows: what it serves escapes.
        context.add(ValueFlowProgramNodes::escaped(), namespaceObject);
        return sourcesOf(unknownObject(context, ValueFlowUnknownSource::UndescribedNativeResult));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The loader fills the variables of a module that has no code of its own.
static void fillExportsOfLoader(ValueFlowRuleContext& context, const Module& module)
{
    const Program& program = context.program();
    const BuiltinTable& builtins = program.builtins();
    ValueSetID filled = unknownValue(context, sourceOfWhatLoaderMakes(module));
    BuiltinID object = module.kind == ModuleKind::Runtime && module.runtimeModule ? builtins.namespaceOf(module.runtimeModule) : BuiltinID();
    for (const SyntheticExport& entry : module.syntheticExports) {
        if (!entry.variable)
            continue;
        ValueFlowNodeID variable = context.nodes().variable(entry.variable);
        std::optional<PropertyKeyID> key = object ? program.types.find({ .kind = PropertyKeyKind::Text, .text = entry.name }) : std::nullopt;
        BuiltinName name = key ? program.builtinNameOf(*key) : BuiltinName();
        BuiltinID member = name ? builtins.findOwnMember(object, name) : BuiltinID();
        if (!member) {
            context.add(variable, filled);
            continue;
        }
        ClimbingSet objectOfModule = valueOf(context, runtimeNamespaceOf(context, module.runtimeModule));
        OwnProperties properties = readOwnProperties(context, objectOfModule, key);
        bool mayBeMadeAnew = context.meet(objectOfModule, context.markedAmong(objectOfModule, { ObjectMutation::DeleteField, ObjectMutation::RedefineProperty }))
            || context.holdsAnyOf(objectOfModule, ValueFlowProgramNodes::escaped())
            || context.holdsAnyOf(objectOfModule, ValueFlowProgramNodes::lost());
        if (mayBeMadeAnew)
            context.add(variable, filled);
        if (!isObjectKind(builtins.at(member).kind))
            context.add(variable, context.universe().undefinedValue());
        else if (context.universe().builtinTables().heap().letsInUnseenCode(member)) {
            context.use(member, BuiltinUse::Read);
            context.add(variable, only(context, context.universe().objectOf(member)));
        }
        connect(context, properties.held, variable);
        connect(context, properties.others, variable);
    }
    for (const ModuleExport& entry : module.exports) {
        if (entry.kind == ModuleExportKind::Variable && entry.variable)
            context.add(context.nodes().variable(entry.variable), filled);
    }
}

// What holds for a module with and without a body.
static void noteEvaluation(ValueFlowRuleContext& context, ModuleID id)
{
    const Program& program = context.program();
    const Module& module = program.at(id);
    if (module.runtimeModule) {
        if (BuiltinID namespaceObject = program.builtins().namespaceOf(module.runtimeModule))
            context.use(namespaceObject, BuiltinUse::Read);
    }
    if (module.kind == ModuleKind::External && module.externalKind == ExternalKind::UncompiledCode) {
        letUnseenCodeRun(context, UnseenCodeReach::Loader);
        // It has the loader, so it gets at every module of the library that the image holds.
        context.add(ValueFlowProgramNodes::escaped(), context.universe().holdersOfLibraryExports());
    }
    // The run time asks the host for it, and the host's loader is code of the library.
    if (module.kind == ModuleKind::External)
        enterLoaderOfHost(context);
    if (!hasCode(program, module))
        fillExportsOfLoader(context, module);
}

void evaluateModule(ValueFlowRuleContext& context, ModuleID first)
{
    const Program& program = context.program();
    BitVector visited;
    Vector<ModuleID, 4> pending { first };
    while (!pending.isEmpty()) {
        ModuleID id = pending.takeLast();
        const Module& module = program.at(id);
        // The statement in the body does the rest, in the same round.
        if (hasCode(program, module)) {
            context.reach(module.body);
            continue;
        }
        if (visited.set(id.index()))
            continue;
        noteEvaluation(context, id);
        for (const ModuleRequest& request : module.requests) {
            if (isEvaluatedWithImporter(context, request))
                pending.append(request.target);
        }
    }
}

static void passOperandsOfBody(ValueFlowRuleContext& context, const Module& module)
{
    const ValueFlowUniverse& universe = context.universe();
    const ValueFlowProgramNodes& nodes = context.nodes();
    FunctionID body = module.body;
    const Function& function = context.program().at(body);

    for (unsigned index = 0; index < function.numberOfEntryParameters(); ++index) {
        ValueFlowNodeID parameter = nodes.entryParameter(body, index);
        switch (function.entryParameterKind(index)) {
        case EntryParameter::Capture:
            break;
        case EntryParameter::This:
        case EntryParameter::Parameter:
            context.add(parameter, universe.undefinedValue());
            break;
        case EntryParameter::Callee:
            // A body of the program can hand its callee to any code, which calls it with arguments of its own. The library does not do that with the bodies of its modules.
            context.add(parameter, only(context, universe.closureOf(body)));
            if (isInApplication(context.program(), body))
                enterFromNativeCode(context, body);
            break;
        case EntryParameter::NewTarget:
            context.add(parameter, universe.undefinedValue());
            break;
        case EntryParameter::ArgumentCount:
            context.add(parameter, universe.anyPrimitive());
            break;
        case EntryParameter::HiddenRest:
            context.add(parameter, only(context, universe.restArrayOf(body)));
            break;
        }
    }
    if (function.has(FunctionFlags::HasRestParameter))
        context.add(nodes.entryParameter(body, function.firstParameterIndex() + function.numberOfPositionalParameters()), only(context, universe.restArrayOf(body)));

    // The host reports what the body throws, and holds the promise of a body that awaits.
    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
    context.connect(nodes.thrown(body), escaped);
    if (function.isUnsplitCoroutine()) {
        context.add(escaped, only(context, universe.coroutineResultOf(body)));
        for (unsigned index = 0; index < function.results.size(); ++index)
            context.connect(nodes.result(body, index), escaped);
    }
}

static bool mayCaptureDefaultMember(SyntheticExportRule rule)
{
    switch (rule) {
    case SyntheticExportRule::CommonJs:
    case SyntheticExportRule::RuntimeModule:
        return true;
    case SyntheticExportRule::None:
    case SyntheticExportRule::CommonJsIgnoringAnnotation:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Once the body has run the loader reads the own enumerable properties of the module's value and keeps what it finds under the names that importers use in variables.
static void captureSyntheticExports(ValueFlowRuleContext& context, ModuleID id)
{
    const Program& program = context.program();
    const Module& module = program.at(id);
    if (module.exportRule == SyntheticExportRule::None || module.kind == ModuleKind::CommonJs)
        return;

    ValueFlowSources source = moduleExportsValueOf(context, id);
    ClimbingSet values = valueOf(context, source);
    // The loader reads every own property of what a CommonJS module exports. Of a module of the run time it runs an accessor only for a name that something binds.
    if (module.exportRule != SyntheticExportRule::RuntimeModule)
        ownPropertyOf(context, values, std::nullopt);

    StringID defaultName = program.stringOf(BuiltinNames::defaultKeyword);
    for (const SyntheticExport& entry : module.syntheticExports) {
        if (!entry.variable)
            continue;
        ValueFlowNodeID variable = context.nodes().variable(entry.variable);
        bool isDefaultExport = entry.name == defaultName;
        if (isDefaultExport)
            connect(context, source, variable);
        if (isDefaultExport && !mayCaptureDefaultMember(module.exportRule))
            continue;
        // A getter that throws and a member that is no own enumerable property are captured as undefined.
        context.add(variable, context.universe().undefinedValue());
        connect(context, ownPropertyOf(context, values, program.types.find({ .kind = PropertyKeyKind::Text, .text = entry.name })), variable);
    }
}

// `module` is an object that the host makes for whoever asks for it. Its `exports` shows the variable that holds what the module exports: a read of the property reads the variable, and a store stores to it.
static void tieModuleObjectToExports(ValueFlowRuleContext& context, const Module& module, ClimbingSet moduleObject)
{
    PropertyKeyID key = context.program().keyOf(BuiltinNames::exports);
    ValueFlowNodeID variable = context.nodes().variable(module.exportsVariable);
    context.connect(variable, context.written(moduleObject, ValueFlowName::property(key)));
    connect(context, ownPropertyOf(context, moduleObject, key), variable);
}

// The module objects hang together as the requests of the build say: `children` lists what the module has required, and `parent` is a module that has required it.
static void followModuleObject(ValueFlowRuleContext& context, ModuleID id)
{
    const Module& module = context.program().at(id);
    const ValueFlowUniverse& universe = context.universe();
    ValueFlowObjectID object = module.exportsVariable ? universe.objectOf(Builtins::ModuleObject, id) : ValueFlowObjectID();
    if (!object)
        return;

    Vector<ValueFlowObjectID, 8> objectsOfRequiredModules;
    for (const ModuleRequest& request : module.requests) {
        ValueFlowObjectID required = request.kind == ModuleRequestKind::Require ? universe.objectOf(Builtins::ModuleObject, request.target) : ValueFlowObjectID();
        if (required)
            objectsOfRequiredModules.append(required);
    }
    ClimbingSet moduleObject = only(context, object);
    ClimbingSet children(context.maker().of(objectsOfRequiredModules.span()));
    ValueFlowNodeID handedOut = context.nodes().ofHost(ValueFlowHostNode::ModuleObjectsHandedOut);
    if (context.holdsAnyOf(moduleObject, handedOut)) {
        tieModuleObjectToExports(context, module, moduleObject);
        context.add(context.nodes().ofHost(ValueFlowHostNode::ChildrenOfModuleObjects), children);
    }
    if (!objectsOfRequiredModules.isEmpty() && context.holdsAnyOf(children, handedOut))
        context.add(context.nodes().ofHost(ValueFlowHostNode::ParentsOfModuleObjects), moduleObject);
}

void runModuleEvaluation(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const Program& program = context.program();
    ModuleID id = context.universe().moduleWithBody(statement.function);
    RELEASE_ASSERT(id);
    const Module& module = program.at(id);

    noteEvaluation(context, id);
    for (const ModuleRequest& request : module.requests) {
        if (request.kind == ModuleRequestKind::Require)
            continue;
        noteRequest(context, statement.function, request.target);
        if (isEvaluatedWithImporter(context, request))
            evaluateModule(context, request.target);
    }
    passOperandsOfBody(context, module);
    followModuleObject(context, id);
    captureSyntheticExports(context, id);
}

bool isModuleOperation(Opcode opcode)
{
    switch (opcode) {
    case Opcode::Builtin:
    case Opcode::ModuleNamespace:
    case Opcode::ImportModule:
    case Opcode::RequireModule:
    case Opcode::EvaluateModule:
    case Opcode::ImportDynamic:
    case Opcode::RequireDynamic:
    case Opcode::RequireBuiltinModule:
    case Opcode::ResolveDynamic:
    case Opcode::DirectEval:
    case Opcode::NativeBinding:
        return true;
    default:
        return false;
    }
}

static void requireModule(const ValueFlowStatement& statement, ValueFlowRuleContext& context, ModuleID module)
{
    noteRequest(context, statement.function, module);
    evaluateModule(context, module);
    connect(context, requiredValueOf(context, module), statement.result);
    connect(context, evaluationErrorsOf(context, module), statement.thrown ? statement.thrown : ValueFlowProgramNodes::escaped());
}

// What a load finds outside the image: the run time refuses code there, and a file that loads as an object of the host.
static ValueSetID dataFromOutsideImage(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    ValueFlowObjectID object = universe.siteObject(statement, 1);
    ValueFlowObjectID array = universe.siteObject(statement, 2);
    RELEASE_ASSERT(object && array);
    ValueFlowObjectID objects[] { object, array };
    ValueSetID data = context.maker().unite(universe.anyPrimitive(), context.maker().of(std::span<const ValueFlowObjectID>(objects)));
    context.add(context.written(only(context, object), unknownKey), data);
    context.add(context.written(only(context, array), { ValueFlowNameKind::Elements, 0 }), data);
    return data;
}

// Only a load in the program's own code names what it likes. A function of the library names modules of the library, and the run time refuses it any that the image does not hold.
static void loadByUnknownName(const ValueFlowStatement& statement, ValueFlowRuleContext& context, ValueFlowHostNode answers, ValueFlowNodeID target)
{
    if (isInApplication(context.program(), statement.function))
        letModulesBeLoadedByName(context, answers);
    context.add(target, dataFromOutsideImage(statement, context));
    context.connect(context.nodes().ofHost(answers), target);
}

// Where nobody follows the promise, what it is settled with escapes.
static ValueFlowNodeID settledValueOfPromiseAt(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    ValueFlowObjectID promise = universe.siteObject(statement);
    if (!promise)
        return ValueFlowProgramNodes::escaped();
    RELEASE_ASSERT(universe.siteOf(promise)->origin == ValueFlowSiteOrigin::Operation);
    if (statement.result)
        context.add(statement.result, only(context, promise));
    return context.written(only(context, promise), slot(BuiltinSlots::PromiseValue));
}

// What the handlers of a rejection are handed.
static ValueFlowNodeID reasonOfPromiseAt(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    ValueFlowObjectID promise = context.universe().siteObject(statement);
    return promise ? context.written(only(context, promise), slot(BuiltinSlots::PromiseReason)) : ValueFlowProgramNodes::escaped();
}

static void importModule(const ValueFlowStatement& statement, ValueFlowRuleContext& context, ModuleID module)
{
    noteRequest(context, statement.function, module);
    evaluateModule(context, module);
    // The promise is resolved with the namespace object, so an export named `then` is called.
    context.connect(context.lookup(valueOf(context, namespaceOf(context, module)), awaitedValue), settledValueOfPromiseAt(statement, context));
    // It is rejected with what the evaluation throws. The host reports a rejection that no handler takes.
    connect(context, evaluationErrorsOf(context, module), reasonOfPromiseAt(statement, context));
    connect(context, evaluationErrorsOf(context, module), ValueFlowProgramNodes::escaped());
}

static void escapeOperands(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    for (const ValueFlowOperand& operand : context.operandsOf(statement))
        context.connect(operand, ValueFlowProgramNodes::escaped());
}

// The text reads and assigns whatever its scope binds.
static void runDirectEval(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const Program& program = context.program();
    const ValueFlowProgramNodes& nodes = context.nodes();
    // A text is refused with an error of the engine. What is no text is answered as it is.
    if (context.refusesEvaluatedText()) {
        for (const ValueFlowOperand& operand : context.operandsOf(statement)) {
            if (statement.result)
                context.connect(operand, statement.result);
        }
        if (statement.thrown)
            context.add(statement.thrown, unknownValue(context, ValueFlowUnknownSource::ThrownByRunTime));
        return;
    }

    ValueSetID unknown = unknownValue(context, ValueFlowUnknownSource::UnseenCode);
    auto open = [&](ValueFlowNodeID location) {
        context.connect(location, ValueFlowProgramNodes::escaped());
        context.add(location, unknown);
    };

    if (isInSeenCode(context, statement.function))
        letUnseenCodeRun(context, UnseenCodeReach::GlobalScope);
    escapeOperands(statement, context);
    for (const EvalBinding& binding : program.at(program.at(statement.function).at(statement.site).evalScope()).bindings) {
        if (binding.variable)
            open(nodes.variable(binding.variable));
        if (binding.layout && binding.field)
            open(nodes.field(binding.layout, *binding.field));
    }
    if (statement.result)
        context.add(statement.result, unknown);
    if (statement.thrown)
        context.add(statement.thrown, unknown);
}

void runModuleOperation(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const Program& program = context.program();
    const Value& operation = program.at(statement.function).at(statement.site);

    switch (statement.opcode) {
    case Opcode::Builtin: {
        requireModule(statement, context, compiledModuleWithObject(program, program.builtins().canonicalID(operation.builtin())));
        return;
    }
    case Opcode::ModuleNamespace:
        noteRequest(context, statement.function, operation.module());
        evaluateModule(context, operation.module());
        connect(context, namespaceOf(context, operation.module(), operation.phase()), statement.result);
        connect(context, evaluationErrorsOf(context, operation.module()), statement.thrown ? statement.thrown : ValueFlowProgramNodes::escaped());
        return;
    case Opcode::ImportModule:
        importModule(statement, context, operation.module());
        break;
    case Opcode::RequireModule:
        requireModule(statement, context, operation.module());
        return;
    case Opcode::EvaluateModule:
        // The module has run, is running, or throws its error. Whoever wants something of it names a variable of it.
        noteRequest(context, statement.function, operation.module());
        evaluateModule(context, operation.module());
        connect(context, evaluationErrorsOf(context, operation.module()), statement.thrown ? statement.thrown : ValueFlowProgramNodes::escaped());
        return;
    case Opcode::RequireBuiltinModule:
        if (ModuleID module = operation.runtimeModule() ? context.universe().moduleOf(operation.runtimeModule()) : moduleOf(program, operation.runtimeModule())) {
            requireModule(statement, context, module);
            return;
        }
        if (BuiltinID namespaceObject = program.builtins().namespaceOf(operation.runtimeModule()))
            context.use(namespaceObject, BuiltinUse::Read);
        connect(context, runtimeNamespaceOf(context, operation.runtimeModule()), statement.result);
        return;
    case Opcode::ImportDynamic:
        escapeOperands(statement, context);
        loadByUnknownName(statement, context, ValueFlowHostNode::NamespacesLoadedByName, settledValueOfPromiseAt(statement, context));
        // The loader makes an error for a name that it does not find, and whatever module it finds may throw.
        context.add(reasonOfPromiseAt(statement, context), unknownValue(context, ValueFlowUnknownSource::ThrownByRunTime));
        break;
    case Opcode::RequireDynamic:
        escapeOperands(statement, context);
        // The file that is found has this one for its parent. runModulesLoadedByName() lists what may be found among the children.
        if (program.at(operation.referrer()).exportsVariable)
            context.add(context.nodes().ofHost(ValueFlowHostNode::ParentsOfModuleObjects), only(context, context.universe().objectOf(Builtins::ModuleObject, operation.referrer())));
        if (statement.result)
            loadByUnknownName(statement, context, ValueFlowHostNode::RequiredValuesLoadedByName, statement.result);
        else if (isInApplication(program, statement.function))
            letModulesBeLoadedByName(context, ValueFlowHostNode::RequiredValuesLoadedByName);
        break;
    case Opcode::ResolveDynamic:
        // import.meta.resolve is native code all the way.
        if (!operation.isURL())
            context.use(Builtins::ModuleRequireResolve, BuiltinUse::Called);
        escapeOperands(statement, context);
        if (statement.result)
            context.add(statement.result, unknownValue(context, ValueFlowUnknownSource::UndescribedNativeResult));
        break;
    case Opcode::DirectEval:
        runDirectEval(statement, context);
        break;
    case Opcode::NativeBinding: {
        const BuiltinTable& builtins = program.builtins();
        BuiltinID binding = builtins.canonicalID(operation.builtin());
        context.use(binding, BuiltinUse::Read);
        if (!statement.result)
            break;
        // The host makes a function for whoever asks: the entry is that function. Of any other binding the entry says under Get what the host answers.
        if (isObjectKind(builtins.at(binding).kind))
            context.add(statement.result, only(context, context.universe().objectOf(binding)));
        else if (builtins.dispatchOf(binding, BuiltinCallMode::Get))
            applyBuiltin(binding, { .kind = ValueFlowCallKind::Get, .result = statement.result, .thrown = statement.thrown }, context);
        else
            context.add(statement.result, unknownValue(context, ValueFlowUnknownSource::UndescribedNativeResult));
        break;
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    if (statement.thrown)
        context.add(statement.thrown, unknownValue(context, ValueFlowUnknownSource::ThrownByRunTime));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
