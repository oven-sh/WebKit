/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRulesModules.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTValueFlowRules.h"
#include <algorithm>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr ValueFlowName unknownKey { ValueFlowNameKind::UnknownKey, 0 };
static constexpr ValueFlowName everyProperty { ValueFlowNameKind::EveryProperty, 0 };
static constexpr ValueFlowName awaitedValue { ValueFlowNameKind::AwaitedValue, 0 };

static ClimbingSet only(ValueFlowRuleContext& context, ValueFlowObjectID object)
{
    return ClimbingSet(context.maker().of(object));
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

static ValueFlowSources unknownSources(ValueFlowRuleContext& context)
{
    return sourcesOf(ClimbingSet(unknownValue(context)));
}

ModuleID moduleOf(const Program& program, RuntimeModuleID runtimeModule)
{
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        if (program.modules[index].runtimeModule == runtimeModule)
            return ModuleID(index);
    }
    return { };
}

ModuleID compiledModuleWithObject(const Program& program, BuiltinID builtin)
{
    const BuiltinTable& builtins = program.builtins();
    const Builtin& entry = builtins.at(builtin);
    if (entry.holder || !isObjectKind(entry.kind) || entry.origin != BuiltinOrigin::Host)
        return { };
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        if (module.kind != ModuleKind::RuntimeSource || !module.runtimeModule)
            continue;
        BuiltinID object = builtins.namespaceOf(module.runtimeModule);
        if (object && builtins.canonicalID(object) == builtin)
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

static bool isEvaluatedWithImporter(const Program& program, const ModuleRequest& request)
{
    // The names of a dynamic star export are those that the evaluated module has.
    return request.isDynamicStarExport || !awaitsUse(program, request.target);
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
    if (module.kind != ModuleKind::SourceText)
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
            return unknownSources(context);
        useModule(context, context.program().at(entry.variable).module);
        return sourcesOf(context.nodes().variable(entry.variable));
    case ModuleExportKind::Namespace:
        return namespaceOf(context, entry.module, entry.phase);
    case ModuleExportKind::Indirect:
        useModule(context, entry.module);
        return unknownSources(context);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ClimbingSet moduleObjectOf(ValueFlowRuleContext& context, ModuleID id)
{
    ValueFlowObjectID object = context.universe().objectOf(Builtins::ModuleObject, id);
    return object ? only(context, object) : ClimbingSet(context.universe().unknownObjectAlone());
}

// What the loader reads from the objects themselves: it goes no further up. An accessor runs. Without a key: every own property.
static ValueFlowSources ownPropertyOf(ValueFlowRuleContext& context, ClimbingSet values, std::optional<PropertyKeyID> key)
{
    const ValueFlowUniverse& universe = context.universe();
    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
    ValueFlowName valueName = key ? ValueFlowName::property(*key) : everyProperty;
    ValueFlowName getterName = key ? ValueFlowName::getter(*key) : ValueFlowName { ValueFlowNameKind::EveryGetter, 0 };
    ClimbingSet unknownObject(universe.unknownObjectAlone());
    ClimbingSet holders = context.restrict(context.restrict(values, universe.objects()), ValueSets::complement(universe.unknownObjectAlone()));

    ValueFlowSources sources = sourcesOf(context.read(holders, valueName));
    ClimbingSet getters = context.valueOf(context.read(holders, getterName));
    // A store through the unknown object may hit whatever has escaped.
    if (context.meet(holders, context.valueOf(escaped))) {
        sources.nodes.append(context.read(unknownObject, valueName));
        getters = context.unite(getters, context.valueOf(context.read(unknownObject, getterName)));
    }
    if (context.contains(values, ValueFlowUniverse::unknownObject()))
        sources.constant = ClimbingSet(unknownValue(context));

    // What a built-in object of the realm has of its own is in the table, which a lookup knows.
    bool readsTable = false;
    context.forEachMember(context.restrict(holders, universe.builtinObjects()), [&](ValueFlowObjectID object) {
        readsTable |= universe.kindOf(object) == ValueFlowObjectKind::Builtin;
    });
    if (readsTable)
        sources.nodes.append(context.lookup(context.restrict(holders, universe.builtinObjects()), valueName));

    if (!context.meet(getters, universe.callables()))
        return sources;
    ValueFlowCallGroup group = context.group(context.restrict(getters, universe.callables()), ValueFlowCallKind::Get);
    context.add(group.thisValue, holders);
    context.connect(group.thrown, escaped);
    sources.nodes.append(group.result);
    return sources;
}

// What `module.exports` holds, or what the body of a module of the library returns.
static ValueFlowSources moduleExportsValueOf(ValueFlowRuleContext& context, ModuleID id)
{
    const Program& program = context.program();
    const Module& module = program.at(id);
    switch (module.kind) {
    case ModuleKind::CommonJs:
        return ownPropertyOf(context, moduleObjectOf(context, id), program.keyOf(BuiltinNames::exports));
    case ModuleKind::RuntimeSource:
        if (hasCode(program, module) && !program.at(module.body).results.isEmpty())
            return sourcesOf(context.nodes().result(module.body, 0));
        return unknownSources(context);
    case ModuleKind::SourceText:
    case ModuleKind::Data:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return unknownSources(context);
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
        return unknownSources(context);
    return sourcesOf(ClimbingSet(context.universe().undefinedValue()));
}

static ValueFlowSources runtimeNamespaceOf(ValueFlowRuleContext& context, RuntimeModuleID runtimeModule)
{
    const ValueFlowUniverse& universe = context.universe();
    if (BuiltinID namespaceObject = context.program().builtins().namespaceOf(runtimeModule))
        return sourcesOf(only(context, universe.objectOf(namespaceObject)));
    return sourcesOf(ClimbingSet(universe.unknownObjectAlone()));
}

ValueFlowSources requiredValueOf(ValueFlowRuleContext& context, ModuleID id)
{
    const Program& program = context.program();
    const Module& module = program.at(id);
    switch (module.kind) {
    case ModuleKind::Runtime:
        return runtimeNamespaceOf(context, module.runtimeModule);
    case ModuleKind::External:
        return unknownSources(context);
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
        return unknownSources(context);
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
        break;
    }

    // What the body of a requested module throws has escaped when that body was entered.
    ValueFlowSources sources = module.requests.isEmpty() ? ValueFlowSources() : unknownSources(context);
    if (module.body)
        sources.nodes.append(context.nodes().thrown(module.body));
    return sources;
}

// The unknown value stands only for what has escaped, and the names that the run time captures are read from these values.
static void exposeSourcesOfCapturedNames(ValueFlowRuleContext& context, ModuleID root)
{
    const Program& program = context.program();
    BitVector visited;
    Vector<ModuleID, 8> pending { root };
    while (!pending.isEmpty()) {
        ModuleID id = pending.takeLast();
        if (visited.set(id.index()))
            continue;
        const Module& module = program.at(id);
        if (module.exportRule != SyntheticExportRule::None)
            connect(context, moduleExportsValueOf(context, id), ValueFlowProgramNodes::escaped());
        for (const ModuleRequest& request : module.requests) {
            if (request.isDynamicStarExport)
                pending.append(request.target);
        }
    }
}

// The properties of a namespace object are the variables that its module exports. Answers the modules whose namespace objects are among them.
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
                context.add(location, universe.unknownObjectAlone());
                break;
            }
            ClimbingSet exportedNamespace = only(context, universe.namespaceOf(entry.module));
            namespacesInside.append(entry.module);
            if (entry.phase == ImportPhase::Evaluation) {
                context.add(location, exportedNamespace);
                break;
            }
            context.add(ValueFlowProgramNodes::escaped(), exportedNamespace);
            context.add(location, universe.unknownObjectAlone());
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
    if (hasCapturedNames) {
        context.add(context.written(namespaceObject, unknownKey), unknownValue(context));
        exposeSourcesOfCapturedNames(context, id);
        return;
    }
    // `require` and a store mark a namespace object that has no export of this name.
    addPrimitivesUnder(BuiltinNames::__esModule);
}

ValueFlowSources namespaceOf(ValueFlowRuleContext& context, ModuleID first, ImportPhase phase)
{
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    if (program.at(first).kind == ModuleKind::External)
        return sourcesOf(ClimbingSet(universe.unknownObjectAlone()));

    BitVector visited;
    Vector<ModuleID, 4> pending { first };
    while (!pending.isEmpty()) {
        ModuleID id = pending.takeLast();
        if (visited.set(id.index()))
            continue;
        context.add(context.nodes().ofHost(ValueFlowHostNode::CreatedNamespaces), only(context, universe.namespaceOf(id)));
        useModule(context, id);
        tieNamespaceToExports(context, id, pending);
    }

    ClimbingSet namespaceObject = only(context, universe.namespaceOf(first));
    switch (phase) {
    case ImportPhase::Evaluation:
        return sourcesOf(namespaceObject);
    case ImportPhase::Defer:
        // The deferred namespace object is another object, which nobody follows: what it serves escapes.
        context.add(ValueFlowProgramNodes::escaped(), namespaceObject);
        return sourcesOf(ClimbingSet(universe.unknownObjectAlone()));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The loader fills the variables of a module that has no code of its own.
static void fillExportsOfLoader(ValueFlowRuleContext& context, const Module& module)
{
    for (const SyntheticExport& entry : module.syntheticExports) {
        if (entry.variable)
            context.add(context.nodes().variable(entry.variable), unknownValue(context));
    }
    for (const ModuleExport& entry : module.exports) {
        if (entry.kind == ModuleExportKind::Variable && entry.variable)
            context.add(context.nodes().variable(entry.variable), unknownValue(context));
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
    if (module.kind == ModuleKind::External && module.externalKind == ExternalKind::UncompiledCode)
        letUnseenCodeRun(context);
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
            if (isEvaluatedWithImporter(program, request))
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
    // The host's body passes the operands of a CommonJS module.
    bool isCommonJS = module.kind == ModuleKind::CommonJs;

    for (unsigned index = 0; index < function.numberOfEntryParameters(); ++index) {
        ValueFlowNodeID parameter = nodes.entryParameter(body, index);
        switch (function.entryParameterKind(index)) {
        case EntryParameter::Capture:
            break;
        case EntryParameter::This:
        case EntryParameter::Parameter:
            if (!isCommonJS)
                context.add(parameter, universe.undefinedValue());
            break;
        case EntryParameter::Callee:
            // The body can hand its callee to any code, which calls it with arguments of its own.
            context.add(parameter, only(context, universe.closureOf(body)));
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

// Once the body has run the loader reads every own enumerable property of the module's value, so every getter runs, and keeps what it finds under the names that importers use in variables.
static void captureSyntheticExports(ValueFlowRuleContext& context, ModuleID id)
{
    const Program& program = context.program();
    const Module& module = program.at(id);
    if (module.exportRule == SyntheticExportRule::None)
        return;

    ValueFlowSources source = moduleExportsValueOf(context, id);
    ClimbingSet values = valueOf(context, source);
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

void runModuleEvaluation(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const Program& program = context.program();
    ModuleID id = program.at(statement.function).module;
    const Module& module = program.at(id);
    RELEASE_ASSERT(module.body == statement.function);

    noteEvaluation(context, id);
    for (const ModuleRequest& request : module.requests) {
        noteRequest(context, statement.function, request.target);
        if (isEvaluatedWithImporter(program, request))
            evaluateModule(context, request.target);
    }
    passOperandsOfBody(context, module);

    if (module.kind == ModuleKind::CommonJs) {
        if (ValueFlowObjectID exports = context.universe().objectOf(Builtins::ModuleObjectExports, id))
            context.add(context.written(moduleObjectOf(context, id), ValueFlowName::property(program.keyOf(BuiltinNames::exports))), only(context, exports));
    }
    captureSyntheticExports(context, id);
}

bool isModuleOperation(Opcode opcode)
{
    switch (opcode) {
    case Opcode::Builtin:
    case Opcode::ModuleNamespace:
    case Opcode::ImportModule:
    case Opcode::RequireModule:
    case Opcode::ImportDynamic:
    case Opcode::RequireDynamic:
    case Opcode::RequireBuiltinModule:
    case Opcode::ResolveDynamic:
    case Opcode::DirectEval:
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
    if (statement.result)
        context.connect(context.nodes().ofHost(ValueFlowHostNode::AnswersOfReplacedLoader), statement.result);
    connect(context, evaluationErrorsOf(context, module), statement.thrown ? statement.thrown : ValueFlowProgramNodes::escaped());
}

// Only a load in the program's own code names what it likes. A function of the library names modules of the library, and the run time refuses it any that the image does not hold.
static void loadByUnknownName(const ValueFlowStatement& statement, ValueFlowRuleContext& context, ValueFlowHostNode answers, ValueFlowNodeID target)
{
    if (context.program().at(statement.function).role == FunctionRole::Source)
        letModulesBeLoadedByName(context, answers);
    // A file outside the image is parsed as data.
    context.add(target, unknownValue(context));
    context.connect(context.nodes().ofHost(answers), target);
}

// Where nobody follows the promise, what it is settled with escapes.
static ValueFlowNodeID settledValueOfPromiseAt(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    ValueFlowObjectID promise = context.universe().siteObject(statement);
    if (!promise)
        return ValueFlowProgramNodes::escaped();
    if (statement.result)
        context.add(statement.result, only(context, promise));
    return context.written(only(context, promise), slot(BuiltinSlots::PromiseValue));
}

static void importModule(const ValueFlowStatement& statement, ValueFlowRuleContext& context, ModuleID module)
{
    noteRequest(context, statement.function, module);
    evaluateModule(context, module);
    // The promise is resolved with the namespace object, so an export named `then` is called.
    context.connect(context.lookup(valueOf(context, namespaceOf(context, module)), awaitedValue), settledValueOfPromiseAt(statement, context));
    // The host reports a rejection that no handler takes.
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
    ValueSetID unknown = unknownValue(context);
    auto open = [&](ValueFlowNodeID location) {
        context.connect(location, ValueFlowProgramNodes::escaped());
        context.add(location, unknown);
    };

    if (isInSeenCode(context, statement.function))
        letUnseenCodeRun(context);
    escapeOperands(statement, context);
    for (const EvalBinding& binding : program.at(program.at(statement.function).at(statement.site).evalScope()).bindings) {
        if (binding.variable)
            open(nodes.variable(binding.variable));
        if (binding.layout && binding.field)
            open(nodes.field(binding.layout, *binding.field));
    }
    if (statement.result)
        context.add(statement.result, unknown);
}

void runModuleOperation(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const Program& program = context.program();
    const Value& operation = program.at(statement.function).at(statement.site);
    ValueSetID unknown = unknownValue(context);

    switch (statement.opcode) {
    case Opcode::Builtin: {
        // The operation names the object by the built-in and so does its type, so the value holds both.
        BuiltinID builtin = program.builtins().canonicalID(operation.builtin());
        if (statement.result)
            context.add(statement.result, only(context, context.universe().objectOf(builtin)));
        requireModule(statement, context, compiledModuleWithObject(program, builtin));
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
    case Opcode::RequireBuiltinModule:
        if (ModuleID module = moduleOf(program, operation.runtimeModule())) {
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
        break;
    case Opcode::RequireDynamic:
        escapeOperands(statement, context);
        if (statement.result)
            loadByUnknownName(statement, context, ValueFlowHostNode::RequiredValuesLoadedByName, statement.result);
        else if (program.at(statement.function).role == FunctionRole::Source)
            letModulesBeLoadedByName(context, ValueFlowHostNode::RequiredValuesLoadedByName);
        break;
    case Opcode::ResolveDynamic:
        // import.meta.resolve is native code all the way.
        if (!operation.isURL())
            context.use(Builtins::ModuleRequireResolve, BuiltinUse::Called);
        escapeOperands(statement, context);
        if (statement.result)
            context.add(statement.result, unknown);
        break;
    case Opcode::DirectEval:
        runDirectEval(statement, context);
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    if (statement.thrown)
        context.add(statement.thrown, unknown);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
