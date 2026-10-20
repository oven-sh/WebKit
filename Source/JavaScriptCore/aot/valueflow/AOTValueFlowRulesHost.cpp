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
#include "AOTValueFlowRules.h"
#include <algorithm>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr ValueFlowName everyProperty { ValueFlowNameKind::EveryProperty, 0 };

static ClimbingSet only(ValueFlowRuleContext& context, ValueFlowObjectID object)
{
    return ClimbingSet(context.maker().of(object));
}

static void passOperandsOfCommonJSModules(const Program& program, ValueFlowBody& body)
{
    uint32_t anyString = body.constants.size();
    body.constants.append({ Primitive::String, { } });

    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        if (module.kind != ModuleKind::CommonJs || !module.body)
            continue;

        const Function& function = program.at(module.body);
        auto objectOfModule = [&](BuiltinID builtin) {
            body.constants.append({ 0, { ValueFlowObjectName { ValueFlowObjectKind::ModuleBuiltin, program.builtins().canonicalID(builtin).index(), ModuleID(index) } } });
            return static_cast<uint32_t>(body.constants.size() - 1);
        };
        auto pass = [&](CommonJsParameter parameter, uint32_t constant) {
            unsigned position = static_cast<unsigned>(parameter);
            if (position < function.numberOfPositionalParameters())
                body.seedsOnceReached.append({ ValueFlowNodeName::entryParameter(module.body, function.firstParameterIndex() + position), constant, 0 });
        };

        uint32_t exports = objectOfModule(Builtins::ModuleObjectExports);
        pass(CommonJsParameter::Exports, exports);
        pass(CommonJsParameter::Require, objectOfModule(Builtins::ModuleRequire));
        pass(CommonJsParameter::Module, objectOfModule(Builtins::ModuleObject));
        pass(CommonJsParameter::Filename, anyString);
        pass(CommonJsParameter::Dirname, anyString);
        if (std::optional<unsigned> thisParameter = function.thisParameterIndex())
            body.seedsOnceReached.append({ ValueFlowNodeName::entryParameter(module.body, *thisParameter), exports, 0 });
    }
}

void describeHost(const Program& program, ValueFlowBody& body)
{
    RELEASE_ASSERT(body.nodes.size() == body.thrown() + 1);
    for (unsigned index = 0; index < numberOfValueFlowHostNodes; ++index)
        body.nodes.append({ ValueFlowBodyNodeKind::Merge, { }, 0 });

    passOperandsOfCommonJSModules(program, body);

    for (ValueFlowStatementKind kind : { ValueFlowStatementKind::HostRoots, ValueFlowStatementKind::UsedBuiltins, ValueFlowStatementKind::UnseenCode, ValueFlowStatementKind::ModulesLoadedByName })
        body.statements.append({ { }, { }, 0, 0, { }, { }, kind });
}

void enterFromNativeCode(ValueFlowRuleContext& context, FunctionID function)
{
    const ValueFlowUniverse& universe = context.universe();
    const ValueFlowProgramNodes& nodes = context.nodes();
    const Function& callee = context.program().at(function);
    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
    ValueSetID unknown = unknownValue(context);

    context.reach(function);
    for (unsigned index = 0; index < callee.numberOfEntryParameters(); ++index) {
        if (callee.entryParameterKind(index) != EntryParameter::Capture)
            context.add(nodes.entryParameter(function, index), unknown);
    }
    for (unsigned index = 0; index < callee.results.size(); ++index)
        context.connect(nodes.result(function, index), escaped);
    context.connect(nodes.thrown(function), escaped);
    if (!callee.isUnsplitCoroutine())
        return;

    context.add(escaped, only(context, universe.coroutineResultOf(function)));
    if (callee.has(FunctionFlags::IsGenerator)) {
        context.connect(nodes.yielded(function), escaped);
        context.add(nodes.sent(function), unknown);
    }
}

static bool isRaised(ValueFlowRuleContext& context, ValueFlowHostNode flag)
{
    return context.contains(context.valueOf(context.nodes().ofHost(flag)), ValueFlowUniverse::unknownObject());
}

static void raise(ValueFlowRuleContext& context, ValueFlowHostNode flag)
{
    context.add(context.nodes().ofHost(flag), context.universe().unknownObjectAlone());
}

void letUnseenCodeRun(ValueFlowRuleContext& context)
{
    raise(context, ValueFlowHostNode::UnseenCodeRuns);
}

void openEscapedObjects(ValueFlowRuleContext& context)
{
    raise(context, ValueFlowHostNode::EscapedObjectsAreOpen);
}

bool areEscapedObjectsOpen(ValueFlowRuleContext& context)
{
    return isRaised(context, ValueFlowHostNode::EscapedObjectsAreOpen) || isRaised(context, ValueFlowHostNode::UnseenCodeRuns);
}

// The run time refuses whoever tries to change a locked object of the table, native code too.
void letAnythingHappenTo(ValueFlowRuleContext& context, ClimbingSet escaped)
{
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet objects = context.restrict(escaped, ValueSets::complement(universe.lockedObjects()));
    ValueSetID unknown = unknownValue(context);
    ValueSetID unknownObject = universe.unknownObjectAlone();
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

static void enterRuntimeFunction(ValueFlowRuleContext& context, RuntimeFunctionID id)
{
    if (const RuntimeFunctionEntry* entry = findRuntimeFunction(context.program(), id); entry && entry->function)
        enterFromNativeCode(context, entry->function);
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
    ValueFlowSources unknown { { }, ClimbingSet(unknownValue(context)) };

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

    if (module.exportRule != SyntheticExportRule::None) {
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

static void applyHostRead(ValueFlowRuleContext& context, ModuleID module, const HostRead& read)
{
    const ValueFlowUniverse& universe = context.universe();
    const BuiltinTable& builtins = context.program().builtins();
    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
    std::optional<ValueFlowSources> value = namedExportOf(context, module, read.exportName);
    if (!value)
        return;
    if (!read.builtin) {
        connect(context, *value, escaped);
        return;
    }

    // The host hands the value to a built-in, as a call of the program would.
    BuiltinID builtin = builtins.canonicalID(read.builtin);
    ValueFlowCallGroup group = context.group(only(context, universe.objectOf(builtin)), ValueFlowCallKind::CallByNativeCode);
    BuiltinID holder = builtins.at(builtin).holder;
    if (holder && isObjectKind(builtins.at(holder).kind))
        context.add(group.thisValue, only(context, universe.objectOf(holder)));
    connect(context, *value, read.parameter < numberOfGroupedArguments ? group.argument(read.parameter) : group.otherArguments);
    context.connect(group.result, escaped);
    context.connect(group.thrown, escaped);
}

// The text of an operand is not followed, so a condition on it may hold. A build that admits code at run time is not solved.
static bool mayHold(const BuiltinCondition* condition)
{
    return !condition || !condition->buildAdmitsRuntimeCode;
}

// Native code does not keep what it reads from a module to itself.
static void loadRuntimeModule(ValueFlowRuleContext& context, const BuiltinModuleLoad& load)
{
    const Program& program = context.program();
    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
    ModuleID id = moduleOf(program, load.module);
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
        connect(context, requiredValueOf(context, id), escaped);
        return;
    }
    for (BuiltinName name : load.exports) {
        auto found = std::ranges::find(module.exports, program.stringOf(name), &ModuleExport::name);
        bool isListed = found != module.exports.end() && (found->kind != ModuleExportKind::Variable || found->variable);
        if (!isListed) {
            // Native code requires the module and reads the name as a property of what that answers. The object that the require answers stays where it is.
            connect(context, memberOfRequiredValue(context, id, program.stringOf(name)), escaped);
            continue;
        }
        switch (found->kind) {
        case ModuleExportKind::Variable:
            useModule(context, program.at(found->variable).module);
            context.connect(context.nodes().variable(found->variable), escaped);
            break;
        case ModuleExportKind::Namespace:
            connect(context, namespaceOf(context, found->module), escaped);
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
            if (mayHold(called.condition))
                add(called.function);
        }
        for (const BuiltinModuleLoad& load : m_builtins.moduleLoadsOf(builtin)) {
            if (mayHold(load.condition))
                m_loads.append(&load);
        }
    }

    // For a built-in whose code runs: the functions of the library that stand behind it, and what hangs on the classes of what it answers.
    void addCodeOf(BuiltinID builtin)
    {
        builtin = m_builtins.canonicalID(builtin);
        for (const BuiltinOverload& overload : m_builtins.overloadsOf(builtin)) {
            forEachFunctionOf(overload, [&](RuntimeFunctionID function) {
                addCode(function);
            });
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
        for (RuntimeFunctionID function : m_builtins.functionsCalledWithStartupLoads())
            add(function);
    }

    void add(RuntimeFunctionID function) { m_functions.set(function.index()); }

    // A function that stands behind a built-in runs for whoever calls the built-in, and that may be seen code.
    void addCode(RuntimeFunctionID function)
    {
        add(function);
        m_functionsBehindBuiltins.set(function.index());
    }

    void enter()
    {
        for (size_t function : m_functions)
            enterRuntimeFunction(m_context, RuntimeFunctionID(static_cast<uint32_t>(function)));
        for (size_t function : m_functionsBehindBuiltins) {
            if (const RuntimeFunctionEntry* entry = findRuntimeFunction(m_context.program(), RuntimeFunctionID(static_cast<uint32_t>(function))); entry && entry->function)
                m_context.add(m_context.nodes().ofHost(ValueFlowHostNode::FunctionsOfSeenCode), only(m_context, m_context.universe().closureOf(entry->function)));
        }
        for (const BuiltinModuleLoad* load : m_loads)
            loadRuntimeModule(m_context, *load);
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
};

} // anonymous namespace

static bool isLibraryRole(FunctionRole role)
{
    return role == FunctionRole::Library || role == FunctionRole::Model;
}

void runHostRoots(const ValueFlowStatement&, ValueFlowRuleContext& context)
{
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    // A call that names its callee has no statement.
    ValueFlowNodeID functionsOfSeenCode = context.nodes().ofHost(ValueFlowHostNode::FunctionsOfSeenCode);
    context.forEachMember(context.valueOf(functionsOfSeenCode), [&](ValueFlowObjectID closure) {
        const Function& function = program.at(universe.closureFunction(closure));
        if (!isLibraryRole(function.role))
            return;
        for (const Value& value : function.values) {
            if (value.opcode == Opcode::CallDirect)
                context.add(functionsOfSeenCode, only(context, universe.closureOf(value.function())));
        }
    });

    for (const Entry& entry : program.entries) {
        // The host asks for the entry point in the name of the program.
        context.add(context.nodes().ofHost(ValueFlowHostNode::ModulesOfSeenCode), only(context, context.universe().namespaceOf(entry.module)));
        evaluateModule(context, entry.module);
        for (const HostRead& read : entry.hostReads)
            applyHostRead(context, entry.module, read);
    }
}

std::optional<UnseenCodeCause> eventOfCalling(const BuiltinTable& builtins, BuiltinID builtin)
{
    const Builtin& entry = builtins.at(builtin);
    if (entry.flags.contains(BuiltinFlag::LoadsModuleByName))
        return UnseenCodeCause::LoadsModuleByName;
    if (entry.flags.contains(BuiltinFlag::EvaluatesText))
        return UnseenCodeCause::EvaluatesText;
    const BuiltinUnseenCode* unseenCode = builtins.unseenCodeOf(builtin);
    if (unseenCode ? mayHold(unseenCode->condition) : entry.flags.contains(BuiltinFlag::RunsUnseenCode))
        return UnseenCodeCause::RunsUnseenCode;
    return std::nullopt;
}

bool isInSeenCode(ValueFlowRuleContext& context, FunctionID id)
{
    const Function& function = context.program().at(id);
    if (!isLibraryRole(function.role))
        return true;
    if (context.contains(context.valueOf(context.nodes().ofHost(ValueFlowHostNode::FunctionsOfSeenCode)), context.universe().closureOf(id)))
        return true;
    return function.module && context.contains(context.valueOf(context.nodes().ofHost(ValueFlowHostNode::ModulesOfSeenCode)), context.universe().namespaceOf(function.module));
}

void noteRequest(ValueFlowRuleContext& context, FunctionID requester, ModuleID module)
{
    if (isInSeenCode(context, requester))
        context.add(context.nodes().ofHost(ValueFlowHostNode::ModulesOfSeenCode), only(context, context.universe().namespaceOf(module)));
}

static void raiseEvent(ValueFlowRuleContext& context, UnseenCodeCause cause)
{
    if (cause != UnseenCodeCause::LoadsModuleByName) {
        letUnseenCodeRun(context);
        return;
    }
    // Nobody follows the result of the call, so what it may answer escapes.
    for (ValueFlowHostNode answers : { ValueFlowHostNode::RequiredValuesLoadedByName, ValueFlowHostNode::NamespacesLoadedByName }) {
        letModulesBeLoadedByName(context, answers);
        context.connect(context.nodes().ofHost(answers), ValueFlowProgramNodes::escaped());
    }
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
        std::optional<UnseenCodeCause> cause = eventOfCalling(builtins, builtins.canonicalID(universe.builtinOf(callee)));
        if (cause && isCalledBySeenCode())
            raiseEvent(context, *cause);
    });
}

// Native code looks up no member of a built-in object of the realm that no entry declares, and a lookup by name through the unknown object finds the member in the table.
// Code that the build has not seen takes whatever the object holds, and so does whoever holds an object that belongs to one module.
static void exposeMembersOf(ValueFlowRuleContext& context, ValueFlowObjectID holderObject, BuiltinID holder)
{
    const ValueFlowUniverse& universe = context.universe();
    const BuiltinTable& builtins = context.program().builtins();
    ModuleID module = universe.moduleOf(holderObject);
    // What lies above the object is handed out with it.
    if (builtins.at(holder).prototypeLink == BuiltinPrototypeLink::Builtin)
        context.add(ValueFlowProgramNodes::escaped(), only(context, universe.objectOf(builtins.at(holder).inheritsFrom)));
    // The run time refuses a built-in whose application is an event unless the build has seen such an event, so such a member stays behind until unseen code can run.
    bool refusesEvents = !isRaised(context, ValueFlowHostNode::UnseenCodeRuns);
    for (BuiltinID member : builtins.membersOf(holder)) {
        if (refusesEvents && builtins.letsInUnseenCode(builtins.canonicalID(member)))
            continue;
        if (!isObjectKind(builtins.at(member).kind)) {
            context.use(member, { BuiltinUse::Read, BuiltinUse::Written });
            continue;
        }
        context.use(member, BuiltinUse::Read);
        ValueFlowObjectID object = module ? universe.objectOf(member, module) : ValueFlowObjectID();
        context.add(ValueFlowProgramNodes::escaped(), only(context, object ? object : universe.objectOf(member)));
    }
}

static constexpr BuiltinID holdersOfLoaderAnswers[] { Builtins::ModuleCache, Builtins::ModuleRequireExtensions, Builtins::NodeModule_extensions };
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

static void replaceLoaderAnswers(ValueFlowRuleContext& context)
{
    ValueFlowNodeID answers = context.nodes().ofHost(ValueFlowHostNode::AnswersOfReplacedLoader);
    context.add(answers, unknownValue(context));
    context.connect(context.nodes().ofHost(ValueFlowHostNode::RequiredValuesLoadedByName), answers);
    letModulesBeLoadedByName(context, ValueFlowHostNode::RequiredValuesLoadedByName);
}

static bool mayNameLocationOfLoader(const Program& program, PropertyKeyID name)
{
    if (!name)
        return true;
    BuiltinName builtinName = program.builtinNameOf(name);
    return builtinName && std::ranges::any_of(locationsOfLoaders, [&](BuiltinID location) {
        return program.builtins().at(location).name == builtinName;
    });
}

static bool hasLocationOfLoaderUnder(const Program& program, BuiltinID holder, PropertyKeyID name)
{
    auto isLocationOfLoader = [](BuiltinID member) {
        return std::ranges::contains(locationsOfLoaders, member);
    };
    if (name)
        return isLocationOfLoader(findOwnMember(program, holder, name));
    return std::ranges::any_of(program.builtins().membersOf(holder), isLocationOfLoader);
}

// An entry that the program stores into the module cache, and a function that it puts in the place of the loader's, answer every later require, also one with a constant name.
// The run time refuses such a store to whatever is no seen code.
void applyEventsOfStore(const ValueFlowStatement& statement, ValueFlowRuleContext& context, ClimbingSet receivers, PropertyKeyID name)
{
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    if (!statement.function)
        return;

    // The loader's own code fills its tables. A table that has escaped has replaced the answers already.
    ClimbingSet holders = context.restrict(receivers, universe.builtinObjects());
    bool storesIntoTable = std::ranges::any_of(holdersOfLoaderAnswers, [&](BuiltinID table) {
        return context.contains(holders, universe.objectOf(table));
    });
    bool replacesAnswers = storesIntoTable && isInSeenCode(context, statement.function);
    if (!replacesAnswers && mayNameLocationOfLoader(program, name)) {
        if (name && context.contains(receivers, ValueFlowUniverse::unknownObject()))
            holders = context.unite(holders, context.restrict(context.valueOf(ValueFlowProgramNodes::escaped()), universe.builtinObjects()));
        context.forEachMember(holders, [&](ValueFlowObjectID object) {
            BuiltinID holder = universe.builtinOf(object);
            replacesAnswers |= holder && hasLocationOfLoaderUnder(program, program.builtins().canonicalID(holder), name);
        });
    }
    if (replacesAnswers)
        replaceLoaderAnswers(context);
}

// `require.cache` and `module.parent` lead to the module object of every CommonJS module that has run.
static void exposeModuleObjects(ValueFlowRuleContext& context)
{
    const Program& program = context.program();
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        if (module.kind != ModuleKind::CommonJs || !module.body || !context.isReached(module.body))
            continue;
        if (ValueFlowObjectID moduleObject = context.universe().objectOf(Builtins::ModuleObject, ModuleID(index)))
            context.add(ValueFlowProgramNodes::escaped(), only(context, moduleObject));
    }
}

void runUsedBuiltins(const ValueFlowStatement&, ValueFlowRuleContext& context)
{
    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    const BuiltinTable& builtins = program.builtins();

    ClimbingSet escaped = context.restrict(context.valueOf(ValueFlowProgramNodes::escaped()), universe.builtinObjects());
    // The code behind a built-in of the host is in the image for a use that names the built-in. That the object has come into unknown hands is no such use.
    Vector<ValueFlowObjectID, 64> escapedObjectsOfLanguage;
    context.forEachMember(escaped, [&](ValueFlowObjectID object) {
        BuiltinID builtin = universe.builtinOf(object);
        if (builtin && builtins.at(builtin).origin != BuiltinOrigin::Host)
            escapedObjectsOfLanguage.append(object);
    });
    ClimbingSet escapedOfLanguage(context.maker().of(escapedObjectsOfLanguage.span()));
    ClimbingSet used = context.unite(context.valueOf(context.nodes().ofHost(ValueFlowHostNode::UsedBuiltins)), escapedOfLanguage);
    ClimbingSet run = context.unite(context.valueOf(context.nodes().ofHost(ValueFlowHostNode::RunBuiltins)), escapedOfLanguage);
    bool membersFollowHolders = isRaised(context, ValueFlowHostNode::UnseenCodeRuns);
    // A lookup through the unknown object records the members that it may find and answers the unknown value, so a call of a callee nobody knows may be a call of any function that was read.
    bool functionsThatAreReadMayRun = areEscapedObjectsOpen(context);

    Attachments attachments(context);
    context.forEachMember(used, [&](ValueFlowObjectID object) {
        BuiltinID builtin = universe.builtinOf(object);
        if (!builtin)
            return;
        attachments.addRowsOf(builtin);
        // A read or a store of what is no object runs the code behind it.
        bool mayBeCalleeNobodyKnows = functionsThatAreReadMayRun && context.sets().contains(universe.builtinFunctions(), object);
        if (!isObjectKind(builtins.at(builtin).kind) || context.contains(run, object) || mayBeCalleeNobodyKnows)
            attachments.addCodeOf(builtin);
    });
    for (const RuntimeFunctionEntry& entry : program.runtimeFunctions) {
        if (!entry.function || !builtins.contains(entry.id))
            continue;
        for (BuiltinID builtin : builtins.at(entry.id).implements) {
            bool runsWhereUsed = !isObjectKind(builtins.at(builtin).kind) || functionsThatAreReadMayRun;
            if (context.contains(runsWhereUsed ? used : run, universe.objectOf(builtin)))
                attachments.addCode(entry.id);
        }
    }
    if (isRaised(context, ValueFlowHostNode::HostChoosesOperands))
        attachments.addStartupModules();
    attachments.enter();

    for (BuiltinID location : { Builtins::ModuleRequireCache, Builtins::NodeModule_cache }) {
        if (context.contains(used, universe.objectOf(location)))
            exposeModuleObjects(context);
    }
    // Whoever holds a table of the loader stores into it, and nobody sees the store.
    for (BuiltinID table : holdersOfLoaderAnswers) {
        if (context.contains(escaped, universe.objectOf(table)))
            replaceLoaderAnswers(context);
    }

    context.forEachMember(escaped, [&](ValueFlowObjectID object) {
        BuiltinID builtin = universe.builtinOf(object);
        if (!builtin)
            return;
        if (builtins.at(builtin).origin != BuiltinOrigin::Host || membersFollowHolders)
            context.use(builtin, BuiltinUse::Escaped);
        if (membersFollowHolders || builtins.at(builtin).scope != BuiltinScope::Realm)
            exposeMembersOf(context, object, builtin);
    });
}

// One more caller without types, not a second world: the global object and what every loadable module exports escape. The rule of what has escaped lets everything happen to it.
void runUnseenCode(const ValueFlowStatement&, ValueFlowRuleContext& context)
{
    if (!isRaised(context, ValueFlowHostNode::UnseenCodeRuns))
        return;

    const Program& program = context.program();
    const ValueFlowUniverse& universe = context.universe();
    const ValueFlowProgramNodes& nodes = context.nodes();
    const BuiltinTable& builtins = program.builtins();
    ValueFlowNodeID escapedNode = ValueFlowProgramNodes::escaped();

    context.add(escapedNode, only(context, universe.objectOf(Builtins::GlobalThis)));
    for (ValueFlowHostNode answers : { ValueFlowHostNode::RequiredValuesLoadedByName, ValueFlowHostNode::NamespacesLoadedByName }) {
        letModulesBeLoadedByName(context, answers);
        context.connect(nodes.ofHost(answers), escapedNode);
    }

    // Such code evaluates text without the program naming an evaluator, and the interpreter calls functions of the library for that text.
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        if (builtins.at(BuiltinID(index)).flags.contains(BuiltinFlag::EvaluatesText))
            context.use(BuiltinID(index), BuiltinUse::Escaped);
    }
    letHostChooseOperands(context);
    // It makes calls that nothing describes.
    context.use(Builtins::UndeclaredNativeCode, BuiltinUse::Read);

    exposeModuleObjects(context);
}

void runModulesLoadedByName(const ValueFlowStatement&, ValueFlowRuleContext& context)
{
    bool areRequired = isRaised(context, ValueFlowHostNode::ModulesAreRequiredByName);
    bool areImported = isRaised(context, ValueFlowHostNode::ModulesAreImportedByName);
    if (!areRequired && !areImported)
        return;

    const Program& program = context.program();
    const ValueFlowProgramNodes& nodes = context.nodes();
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        ModuleID id(index);
        const Module& module = program.modules[index];
        // A module of the library that is written in JavaScript is in the image only if seen code names it somewhere. The run time refuses any other, as it refuses a file that was not compiled.
        if (module.kind == ModuleKind::RuntimeSource && (!module.body || !context.isReached(module.body)))
            continue;
        // Whoever names the module at run time is seen code.
        context.add(nodes.ofHost(ValueFlowHostNode::ModulesOfSeenCode), only(context, context.universe().namespaceOf(id)));
        evaluateModule(context, id);
        if (areRequired)
            connect(context, requiredValueOf(context, id), nodes.ofHost(ValueFlowHostNode::RequiredValuesLoadedByName));
        if (areImported)
            connect(context, namespaceOf(context, id), nodes.ofHost(ValueFlowHostNode::NamespacesLoadedByName));
    }
    for (RuntimeFunctionID loader : program.builtins().moduleLoaderFunctions())
        enterRuntimeFunction(context, loader);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
