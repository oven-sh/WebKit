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
#include "AOTValueFlowRulesHeap.h"
#include <algorithm>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT { namespace Typed {

ClimbingSet unknownObject(ValueFlowRuleContext& context, ValueFlowUnknownSource source)
{
    return context.restrict(ClimbingSet(unknownValue(context, source)), context.universe().objects());
}

// It stands over a namespace object of the program. No class of the host has it as an instance.
static ClimbingSet objectThatLoaderMakes(ValueFlowRuleContext& context)
{
    return context.restrict(ClimbingSet(undescribedValueThatIsNoBuffer(context)), context.universe().objects());
}

static ValueFlowName writtenNameOf(const Program& program, StringID text)
{
    std::optional<PropertyKeyID> key = program.types.find({ .kind = PropertyKeyKind::Text, .text = text });
    return key ? ValueFlowName::property(program, *key) : unknownKeyName;
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

static bool moduleHasCode(const Program& program, const Module& module)
{
    return module.body && program.at(module.body).hasBody();
}

bool awaitsUse(const Program& program, ModuleID module)
{
    return program.at(module).flags.contains(ModuleFlags::HasNoSideEffects) && moduleHasCode(program, program.at(module));
}

void useModule(ValueFlowRuleContext& context, ModuleID module)
{
    if (awaitsUse(context.program(), module))
        evaluateModule(context, module);
}

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

static bool isEvaluatedWithImporter(ValueFlowRuleContext& context, const ModuleRequest& request)
{
    const Program& program = context.program();
    if (request.kind == ModuleRequestKind::Require)
        return false;
    if (request.isDynamicStarExport || !awaitsUse(program, request.target))
        return true;
    return context.isReached(program.at(request.target).body) || exportsUsedNameAgain(context, request.target);
}

static bool mayHaveExportsNobodyLists(const Module& module)
{
    if (module.kind != ModuleKind::SourceText || module.exportRule != SyntheticExportRule::None)
        return true;
    return module.requests.containsIf([](const ModuleRequest& request) {
        return request.isDynamicStarExport;
    });
}

static ValueFlowSources ownPropertyOf(ValueFlowRuleContext&, ClimbingSet values, std::optional<PropertyKeyID>);
static ValueFlowSources runtimeNamespaceOf(ValueFlowRuleContext&, RuntimeModuleID);

ValueFlowSources exportedValueOf(ValueFlowRuleContext& context, const ModuleExport& entry)
{
    switch (entry.kind) {
    case ModuleExportKind::Variable:
        if (!entry.variable)
            return unknownSources(context, ValueFlowUnknownSource::DataFromOutside);
        useModule(context, context.program().at(entry.variable).module);
        return sourcesOf(context.nodes().variable(entry.variable));
    case ModuleExportKind::Namespace:
        return namespaceOf(context, entry.module, entry.phase);
    case ModuleExportKind::Indirect: {
        const Program& program = context.program();
        const Module& target = program.at(entry.module);
        useModule(context, entry.module);
        // The host reads the name from the object that the module mirrors.
        if (target.kind == ModuleKind::Runtime)
            return ownPropertyOf(context, valueOf(context, runtimeNamespaceOf(context, target.runtimeModule)), program.types.find({ .kind = PropertyKeyKind::Text, .text = entry.importName }));
        // The run time reads the name on the namespace object of a module of the program.
        if (target.kind != ModuleKind::External)
            connect(context, namespaceOf(context, entry.module), ValueFlowProgramNodes::escaped());
        return unknownSources(context, sourceOfWhatLoaderMakes(target));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

struct OwnProperties {
    ValueFlowSources held;
    ValueFlowSources others;
};

ClimbingSet knownObjectsThatAreFollowed(ValueFlowRuleContext& context, ClimbingSet set)
{
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet objects = context.restrict(set, universe.knownObjects());
    if (!context.contains(set, ValueFlowUniverse::unknownObject()))
        return objects;
    ClimbingSet listedForTheirCalls = context.restrict(objects, universe.builtinTables().heap().objectsThatLetInUnseenCode());
    if (!context.meet(listedForTheirCalls, ValueSets::anything))
        return objects;
    return context.restrict(objects, ValueSets::complement(context.withMembersOf(ValueSets::nothing, context.valueWithin(listedForTheirCalls, ValueFlowProgramNodes::escaped()))));
}

ClimbingSet withSourcesOfCopies(ValueFlowRuleContext& context, ClimbingSet objects)
{
    ClimbingSet all = objects;
    ClimbingSet last = objects;
    for (;;) {
        last = context.restrict(context.valueOf(context.readWhatWasWritten(last, copiedFromName)), context.universe().knownObjects());
        bool findsNewSource = false;
        context.forEachMember(last, [&](ValueFlowObjectID source) {
            findsNewSource |= !context.contains(all, source);
        });
        if (!findsNewSource)
            return all;
        all = context.unite(all, last);
    }
}

static OwnProperties readOwnProperties(ValueFlowRuleContext& context, ClimbingSet values, std::optional<PropertyKeyID> key)
{
    const ValueFlowUniverse& universe = context.universe();
    ValueFlowName valueName = key ? ValueFlowName::property(context.program(), *key) : everyPropertyName;
    ValueFlowName getterName = key ? ValueFlowName::getter(context.program(), *key) : everyGetterName;
    ClimbingSet receivers = context.restrict(values, universe.knownObjects());
    ClimbingSet holders = withSourcesOfCopies(context, receivers);

    OwnProperties properties;
    properties.held = sourcesOf(context.readWhatWasWritten(holders, valueName));
    ClimbingSet getters = context.valueOf(context.readWhatWasWritten(holders, getterName));
    auto readWhatIsStoredThrough = [&](ValueSetID placeholder, ValueFlowNodeID covered) {
        if (!context.holdsAnyOf(holders, covered))
            return;
        properties.held.nodes.append(context.readWhatWasWritten(ClimbingSet(placeholder), valueName));
        getters = context.unite(getters, context.valueOf(context.readWhatWasWritten(ClimbingSet(placeholder), getterName)));
    };
    readWhatIsStoredThrough(universe.unknownObjectAlone(), ValueFlowProgramNodes::escaped());
    readWhatIsStoredThrough(universe.untrackedObjectAlone(), ValueFlowProgramNodes::lost());

    bool readsTable = false;
    context.forEachMember(context.restrict(holders, universe.builtinObjects()), [&](ValueFlowObjectID object) {
        readsTable |= universe.kindOf(object) == ValueFlowObjectKind::Builtin;
    });
    if (readsTable)
        properties.held.nodes.append(context.lookup(context.restrict(holders, universe.builtinObjects()), valueName));

    if (context.contains(values, ValueFlowUniverse::unknownObject()))
        properties.others.constant = ClimbingSet(unknownValue(context, ValueFlowUnknownSource::ContentsOfEscapedObject));
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

// The loader lists every own name of what a file exports, enumerable or not.
static void listOwnPropertiesNobodyWrote(ValueFlowRuleContext& context, ClimbingSet values, std::optional<PropertyKeyID> key, ValueFlowNodeID target)
{
    readOwnPropertiesNobodyWrote(context, withSourcesOfCopies(context, context.restrict(values, context.universe().knownObjects())), key, target);
}

static ValueFlowSources moduleExportsValueOf(ValueFlowRuleContext& context, ModuleID id)
{
    const Program& program = context.program();
    const Module& module = program.at(id);
    switch (module.kind) {
    case ModuleKind::CommonJs:
        if (!module.exportsVariable)
            return moduleHasCode(program, module) ? sourcesOf(ClimbingSet(context.universe().undefinedValue())) : unknownSources(context, sourceOfWhatLoaderMakes(module));
        useModule(context, id);
        return sourcesOf(context.nodes().variable(module.exportsVariable));
    case ModuleKind::RuntimeSource:
        if (moduleHasCode(program, module) && !program.at(module.body).results.isEmpty())
            return sourcesOf(context.nodes().result(module.body, 0));
        return unknownSources(context, sourceOfWhatLoaderMakes(module));
    case ModuleKind::SourceText:
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
    if (!hasExportTableOf(module.kind))
        return moduleExportsValueOf(context, id);
    if (const ModuleExport* entry = findExport(module, program.stringOf(BuiltinNames::defaultKeyword)))
        return exportedValueOf(context, *entry);
    if (module.kind == ModuleKind::Data)
        return unknownSources(context, ValueFlowUnknownSource::DataFromOutside);
    return sourcesOf(ClimbingSet(context.universe().undefinedValue()));
}

static ValueFlowSources runtimeNamespaceOf(ValueFlowRuleContext& context, RuntimeModuleID runtimeModule)
{
    if (BuiltinID namespaceObject = context.program().builtins().namespaceOf(runtimeModule))
        return sourcesOf(only(context, context.universe().objectOf(namespaceObject)));
    return sourcesOf(unknownObject(context, ValueFlowUnknownSource::UndescribedNativeResult));
}

// The run time has one namespace object for a file: that of the module that stands for the file where ES modules name what they import from it, or the file's own.
static ValueFlowSources namespaceOfFile(ValueFlowRuleContext& context, ModuleID file)
{
    const Program& program = context.program();
    ValueFlowSources sources = namespaceOf(context, file);
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        if (commonJsModuleCapturedBy(program, program.modules[index]) == file)
            append(context, sources, namespaceOf(context, ModuleID(index)));
    }
    return sources;
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
    case RequireResult::ModuleExports: {
        ValueFlowSources sources = moduleExportsValueOf(context, id);
        // Where the file threw and nobody got the error, the loader answers with what it has of the file.
        if (module.flags.contains(ModuleFlags::MayBeImportedWhileItRuns))
            append(context, sources, namespaceOfFile(context, id));
        return sources;
    }
    case RequireResult::DefaultExport:
        return defaultExportOf(context, id);
    case RequireResult::Namespace: {
        ValueFlowSources sources = namespaceOf(context, id);
        BuiltinName name = program.builtins().nameOf(byteCast<uint8_t>("module.exports"_s.span()));
        if (const ModuleExport* entry = name ? findExport(module, program.stringOf(name)) : nullptr)
            append(context, sources, exportedValueOf(context, *entry));
        if (module.kind == ModuleKind::SourceText)
            append(context, sources, sourcesOf(objectThatLoaderMakes(context)));
        return sources;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ValueFlowSources evaluationErrorsOf(ValueFlowRuleContext& context, ModuleID id)
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

    bool importsSomething = std::ranges::contains(module.requests, ModuleRequestKind::Import, &ModuleRequest::kind);
    ValueFlowSources sources = importsSomething ? unknownSources(context, ValueFlowUnknownSource::ThrownByRunTime) : ValueFlowSources();
    if (module.body)
        sources.nodes.append(context.nodes().thrown(module.body));
    return sources;
}

static void tieNamespaceToExports(ValueFlowRuleContext& context, ModuleID id, Vector<ModuleID, 4>& namespacesInside)
{
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    const Module& module = program.at(id);
    ClimbingSet namespaceObject = only(context, universe.namespaceOf(id));

    for (uint32_t index = 0; index < module.exports.size(); ++index) {
        const ModuleExport& entry = module.exports[index];
        PropertyKeyID key = module.namespaceLayout ? program.at(module.namespaceLayout).fields[index].key : PropertyKeyID();
        ValueFlowNodeID location = context.written(namespaceObject, key ? ValueFlowName::property(program, key) : writtenNameOf(program, entry.name));
        switch (entry.kind) {
        case ModuleExportKind::Variable:
            connect(context, exportedValueOf(context, entry), location);
            break;
        case ModuleExportKind::Indirect:
            if (program.at(entry.module).kind == ModuleKind::External || program.at(entry.module).kind == ModuleKind::Runtime) {
                connect(context, exportedValueOf(context, entry), location);
                break;
            }
            useModule(context, entry.module);
            context.add(ValueFlowProgramNodes::escaped(), only(context, universe.namespaceOf(entry.module)));
            namespacesInside.append(entry.module);
            context.add(location, unknownValue(context, sourceOfWhatLoaderMakes(program.at(entry.module))));
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
            context.add(location, objectThatLoaderMakes(context));
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
    addPrimitivesUnder(BuiltinNames::__esModule);
    if (!hasCapturedNames)
        return;

    ValueFlowNodeID underAnyName = context.written(namespaceObject, unknownKeyName);
    ValueFlowNodeID holdersOfNames = context.written(namespaceObject, copiedFromName);

    if (module.exportRule != SyntheticExportRule::None || module.exportsVariable) {
        ValueFlowSources value = moduleExportsValueOf(context, id);
        connect(context, value, context.written(namespaceObject, writtenNameOf(program, program.stringOf(BuiltinNames::defaultKeyword))));
        ClimbingSet values = valueOf(context, value);
        context.add(holdersOfNames, context.restrict(values, universe.knownObjects()));
        // Where every own key of what the module exports is known, what the getter under a key answers lies under that key.
        std::optional<ValueSetID> keysAtCreation = keysThatObjectsWereMadeWith(context, values, OwnKeys::All);
        ClimbingSet keys = keysAtCreation ? context.unite(ClimbingSet(*keysAtCreation), context.valueOf(context.read(values, ValueFlowCellName { ValueFlowNameKind::AddedNames }))) : ClimbingSet();
        if (keysAtCreation && !context.meet(keys, ValueSets::complement(universe.keys()))) {
            context.forEachMember(keys, [&](ValueFlowObjectID key) {
                connect(context, readOwnProperties(context, values, universe.keyOf(key)).others, context.written(namespaceObject, ValueFlowName::property(program, universe.keyOf(key))));
            });
        } else
            connect(context, readOwnProperties(context, values, std::nullopt).others, underAnyName);
        listOwnPropertiesNobodyWrote(context, values, std::nullopt, underAnyName);
    } else if (module.kind != ModuleKind::SourceText)
        context.add(underAnyName, unknownValue(context, sourceOfWhatLoaderMakes(module)));
    if (module.kind == ModuleKind::Runtime) {
        ClimbingSet answered = valueOf(context, runtimeNamespaceOf(context, module.runtimeModule));
        context.add(holdersOfNames, context.restrict(answered, universe.knownObjects()));
        connect(context, ownPropertyOf(context, answered, std::nullopt), underAnyName);
    }

    for (const ModuleRequest& request : module.requests) {
        if (!request.isDynamicStarExport)
            continue;
        const Module& target = program.at(request.target);
        if (target.kind == ModuleKind::External) {
            context.add(underAnyName, unknownValue(context, sourceOfWhatLoaderMakes(target)));
            continue;
        }
        context.add(holdersOfNames, only(context, universe.namespaceOf(request.target)));
        namespacesInside.append(request.target);
    }
}

ValueFlowSources namespaceOf(ValueFlowRuleContext& context, ModuleID first, ImportPhase phase)
{
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    if (program.at(first).kind == ModuleKind::External)
        return sourcesOf(unknownObject(context, sourceOfWhatLoaderMakes(program.at(first))));

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
        context.add(ValueFlowProgramNodes::escaped(), namespaceObject);
        return sourcesOf(objectThatLoaderMakes(context));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void runGettersThatProgramLaidOn(ValueFlowRuleContext& context, ClimbingSet values)
{
    ClimbingSet receivers = context.restrict(values, context.universe().knownObjects());
    ClimbingSet getters = context.valueOf(context.readWhatWasWritten(receivers, everyGetterName));
    forEachPartOfCallees(context, context.restrict(getters, context.universe().callables()), [&](ClimbingSet callees) {
        ValueFlowCallGroup group = context.group(callees, ValueFlowCallKind::Get);
        context.add(group.thisValue, receivers);
        context.connect(group.thrown, ValueFlowProgramNodes::escaped());
    });
}

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
        context.add(ValueFlowProgramNodes::escaped(), context.universe().holdersOfLibraryExports());
    }
    if (module.kind == ModuleKind::External)
        enterLoaderOfHost(context);
    if (moduleHasCode(program, module))
        return;

    const BuiltinTable& builtins = program.builtins();
    auto filled = [&] {
        return unknownValue(context, sourceOfWhatLoaderMakes(module));
    };
    BuiltinID object = module.kind == ModuleKind::Runtime && module.runtimeModule ? builtins.namespaceOf(module.runtimeModule) : BuiltinID();
    if (object)
        runGettersThatProgramLaidOn(context, valueOf(context, runtimeNamespaceOf(context, module.runtimeModule)));
    for (const SyntheticExport& entry : module.syntheticExports) {
        if (!entry.variable)
            continue;
        ValueFlowNodeID variable = context.nodes().variable(entry.variable);
        std::optional<PropertyKeyID> key = object ? program.types.find({ .kind = PropertyKeyKind::Text, .text = entry.name }) : std::nullopt;
        BuiltinName name = key ? program.builtinNameOf(*key) : BuiltinName();
        BuiltinID member = name ? builtins.findOwnMember(object, name) : BuiltinID();
        if (!member) {
            context.add(variable, filled());
            if (key)
                connect(context, ownPropertyOf(context, valueOf(context, runtimeNamespaceOf(context, module.runtimeModule)), key), variable);
            continue;
        }
        ClimbingSet objectOfModule = valueOf(context, runtimeNamespaceOf(context, module.runtimeModule));
        OwnProperties properties = readOwnProperties(context, objectOfModule, key);
        bool mayBeMadeAnew = context.meet(objectOfModule, context.markedAmong(objectOfModule, { ObjectMutation::DeleteField, ObjectMutation::RedefineProperty }))
            || context.holdsAnyOf(objectOfModule, ValueFlowProgramNodes::escaped())
            || context.holdsAnyOf(objectOfModule, ValueFlowProgramNodes::lost());
        if (mayBeMadeAnew)
            context.add(variable, filled());
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
            context.add(context.nodes().variable(entry.variable), filled());
    }
}

void evaluateModule(ValueFlowRuleContext& context, ModuleID first)
{
    const Program& program = context.program();
    BitVector visited;
    Vector<ModuleID, 4> pending { first };
    while (!pending.isEmpty()) {
        ModuleID id = pending.takeLast();
        const Module& module = program.at(id);
        if (moduleHasCode(program, module)) {
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
        case EntryParameter::NewTarget:
            context.add(parameter, universe.undefinedValue());
            break;
        case EntryParameter::Callee:
            context.add(parameter, only(context, universe.closureOf(body)));
            if (isWrittenInProgram(function))
                enterFromNativeCode(context, body);
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

    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
    context.connect(nodes.thrown(body), escaped);
    // What is thrown or what a promise is rejected with ends at the host where nobody catches it. The host prints it the way console.error does, whatever lies inside it too.
    if (!isHostFlagRaised(context, ValueFlowHostNode::HostChoosesOperands) && mayProgramHaveStoredMethodThatHostLooksFor(context))
        letHostChooseOperands(context);
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

// The children of a module object that is handed out hold, under `exports`, what its require() has answered for a file that is no CommonJS file.
static void showAnswerOfRequireAmongChildren(ValueFlowRuleContext& context, ModuleID required)
{
    const Module& module = context.program().at(required);
    bool hasObjectMadeByRequire = !module.flags.contains(ModuleFlags::IsLibrary) && (module.kind == ModuleKind::SourceText || module.kind == ModuleKind::Data);
    if (hasObjectMadeByRequire && namesWaysToModuleObjects(context.program()))
        connect(context, requiredValueOf(context, required), ValueFlowProgramNodes::escaped());
}

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
        PropertyKeyID key = context.program().keyOf(BuiltinNames::exports);
        ValueFlowNodeID variable = context.nodes().variable(module.exportsVariable);
        context.connect(variable, context.written(moduleObject, ValueFlowName::property(key)));
        connect(context, ownPropertyOf(context, moduleObject, key), variable);
        context.add(context.nodes().ofHost(ValueFlowHostNode::ChildrenOfModuleObjects), children);
        for (const ModuleRequest& request : module.requests) {
            if (request.kind == ModuleRequestKind::Require)
                showAnswerOfRequireAmongChildren(context, request.target);
        }
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

    if (module.exportRule == SyntheticExportRule::None || module.kind == ModuleKind::CommonJs)
        return;

    ValueFlowSources source = moduleExportsValueOf(context, id);
    ClimbingSet values = valueOf(context, source);
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
        context.add(variable, context.universe().undefinedValue());
        std::optional<PropertyKeyID> key = program.types.find({ .kind = PropertyKeyKind::Text, .text = entry.name });
        connect(context, ownPropertyOf(context, values, key), variable);
        listOwnPropertiesNobodyWrote(context, values, key, variable);
        if (context.sets().meet(context.universe().holdersOfExportsOf(id), context.universe().eventRaisingLibraryExportHolders()))
            letSeenCodeCallEscapedFunctionsAmong(context, variable);
    }
}

// A require() that the run time answers: the file has no request for the module.
static void followModuleObjectsOfRequire(ValueFlowRuleContext& context, ModuleID referrer, ModuleID required)
{
    const ValueFlowUniverse& universe = context.universe();
    if (!referrer || !context.program().at(referrer).exportsVariable)
        return;
    ValueFlowObjectID parent = universe.objectOf(Builtins::ModuleObject, referrer);
    ValueFlowObjectID child = universe.objectOf(Builtins::ModuleObject, required);
    if (!parent)
        return;
    ValueFlowNodeID handedOut = context.nodes().ofHost(ValueFlowHostNode::ModuleObjectsHandedOut);
    if (context.holdsAnyOf(only(context, parent), handedOut)) {
        if (child)
            context.add(context.nodes().ofHost(ValueFlowHostNode::ChildrenOfModuleObjects), only(context, child));
        showAnswerOfRequireAmongChildren(context, required);
    }
    if (child && context.holdsAnyOf(only(context, child), handedOut))
        context.add(context.nodes().ofHost(ValueFlowHostNode::ParentsOfModuleObjects), only(context, parent));
}

static void requireModule(const ValueFlowStatement& statement, ValueFlowRuleContext& context, ModuleID module)
{
    noteRequest(context, statement.function, module);
    evaluateModule(context, module);
    connect(context, requiredValueOf(context, module), statement.result);
    const BuiltinTable& builtins = context.program().builtins();
    RuntimeModuleID runtimeModule = context.program().at(module).runtimeModule;
    if (BuiltinID object = runtimeModule ? builtins.namespaceOf(runtimeModule) : BuiltinID(); object && statement.result && builtins.canonicalID(object) == Builtins::Bun)
        context.add(statement.result, only(context, ValueFlowUniverse::markOfLoadedBun()));
    connect(context, evaluationErrorsOf(context, module), statement.thrown ? statement.thrown : ValueFlowProgramNodes::escaped());
}

static void loadByUnknownName(const ValueFlowStatement& statement, ValueFlowRuleContext& context, ValueFlowHostNode answers, ValueFlowNodeID target)
{
    if (isWrittenInProgram(context.program().at(statement.function)))
        letModulesBeLoadedByName(context, answers);
    const ValueFlowUniverse& universe = context.universe();
    ValueFlowObjectID object = universe.siteObject(statement, 1);
    ValueFlowObjectID array = universe.siteObject(statement, 2);
    RELEASE_ASSERT(object && array);
    ValueFlowObjectID objects[] { object, array };
    ValueSetID data = context.maker().unite(universe.anyPrimitive(), context.maker().of(std::span<const ValueFlowObjectID>(objects)));
    context.add(context.written(only(context, object), unknownKeyName), data);
    context.add(context.written(only(context, array), { ValueFlowNameKind::Elements, 0 }), data);
    context.add(target, data);
    context.connect(context.nodes().ofHost(answers), target);
}

static ValueFlowNodeID settledValueOfPromiseAt(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    ValueFlowObjectID promise = universe.siteObject(statement);
    if (!promise)
        return ValueFlowProgramNodes::escaped();
    RELEASE_ASSERT(universe.siteOf(promise)->origin == ValueFlowSiteOrigin::Operation);
    if (statement.result)
        context.add(statement.result, only(context, promise));
    return context.written(only(context, promise), { ValueFlowNameKind::Slot, BuiltinSlots::PromiseValue.index() });
}

static ValueFlowNodeID reasonOfPromiseAt(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    ValueFlowObjectID promise = context.universe().siteObject(statement);
    return promise ? context.written(only(context, promise), { ValueFlowNameKind::Slot, BuiltinSlots::PromiseReason.index() }) : ValueFlowProgramNodes::escaped();
}

static void escapeOperands(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    for (const ValueFlowOperand& operand : context.operandsOf(statement))
        context.connect(operand, ValueFlowProgramNodes::escaped());
}

static void runDirectEval(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const Program& program = context.program();
    const ValueFlowProgramNodes& nodes = context.nodes();
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
    // The text alone can name what stands in its scope: nobody has handed it to a value nobody knows.
    ValueSetID lettingInUnseenCode = context.universe().builtinTables().heap().objectsThatLetInUnseenCode();
    auto open = [&](ValueFlowNodeID location) {
        context.connect(context.masked(location, ValueSets::complement(lettingInUnseenCode)), ValueFlowProgramNodes::escaped());
        context.connect(context.masked(location, lettingInUnseenCode), nodes.ofHost(ValueFlowHostNode::LoadersAndEvaluatorsThatEvaluatedTextNames));
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
    case Opcode::Builtin:
        if (ModuleID module = compiledModuleWithObject(program, program.builtins().canonicalID(operation.builtin())))
            requireModule(statement, context, module);
        else if (statement.result) {
            context.add(statement.result, only(context, context.universe().objectOf(operation.builtin())));
            context.add(statement.result, only(context, ValueFlowUniverse::markOfLoadedBun()));
        }
        return;
    case Opcode::ModuleNamespace:
        noteRequest(context, statement.function, operation.module());
        evaluateModule(context, operation.module());
        connect(context, namespaceOf(context, operation.module(), operation.phase()), statement.result);
        connect(context, evaluationErrorsOf(context, operation.module()), statement.thrown ? statement.thrown : ValueFlowProgramNodes::escaped());
        return;
    case Opcode::ImportModule: {
        escapeOperands(statement, context);
        auto import = [&](ModuleID module) {
            noteRequest(context, statement.function, module);
            evaluateModule(context, module);
            context.connect(context.lookup(valueOf(context, namespaceOf(context, module)), awaitedValueName), settledValueOfPromiseAt(statement, context));
            connect(context, evaluationErrorsOf(context, module), reasonOfPromiseAt(statement, context));
            connect(context, evaluationErrorsOf(context, module), ValueFlowProgramNodes::escaped());
        };
        import(operation.module());
        if (!context.operandsOf(statement).empty()) {
            for (ModuleID form : formsInImage(program, operation.module()))
                import(form);
        }
        context.add(reasonOfPromiseAt(statement, context), unknownValue(context, ValueFlowUnknownSource::ThrownByRunTime));
        break;
    }
    case Opcode::RequireModule:
        requireModule(statement, context, operation.module());
        followModuleObjectsOfRequire(context, operation.referrer(), operation.module());
        return;
    case Opcode::EvaluateModule:
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
        context.add(reasonOfPromiseAt(statement, context), unknownValue(context, ValueFlowUnknownSource::ThrownByRunTime));
        break;
    case Opcode::RequireDynamic:
        escapeOperands(statement, context);
        if (program.at(operation.referrer()).exportsVariable)
            context.add(context.nodes().ofHost(ValueFlowHostNode::ParentsOfModuleObjects), only(context, context.universe().objectOf(Builtins::ModuleObject, operation.referrer())));
        if (statement.result)
            loadByUnknownName(statement, context, ValueFlowHostNode::RequiredValuesLoadedByName, statement.result);
        else if (isWrittenInProgram(program.at(statement.function)))
            letModulesBeLoadedByName(context, ValueFlowHostNode::RequiredValuesLoadedByName);
        break;
    case Opcode::ResolveDynamic:
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
        if (context.isGuardedByRemovedFeature(binding))
            context.add(statement.result, unknownValue(context, ValueFlowUnknownSource::UndescribedNativeResult));
        else if (isObjectKind(builtins.at(binding).kind))
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
