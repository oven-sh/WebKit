/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowOrderInternal.h"

#if ENABLE(AOT)

#include "AOTSchemaWalkers.h"
#include "AOTThreads.h"
#include <algorithm>
#include <functional>
#include <tuple>
#include <wtf/BitVector.h>
#include <wtf/HashSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

FlowScopeLocationNumbering createScopeLocationNumbering(const Program& program)
{
    FlowScopeLocationNumbering numbering;
    numbering.numberOfVariables = program.variables.size();
    numbering.firstField.reserveInitialCapacity(program.layouts.size() + 1);
    uint32_t next = numbering.numberOfVariables;
    for (const Layout& layout : program.layouts) {
        numbering.firstField.append(next);
        if (layout.kind == LayoutKind::Environment)
            next += layout.fields.size();
    }
    numbering.firstField.append(next);
    return numbering;
}

void numberMemberLocations(const FlowOrderInputs& inputs, FlowScopeLocationNumbering& numbering)
{
    for (const FlowMemberAccess& access : inputs.memberAccesses()) {
        if (followsMembersOf(inputs, access.holder))
            numbering.members.append({ access.holder, access.key });
    }
    sortAndRemoveDuplicates(inputs.build().parallelLoops(), numbering.members);
}

static void collectVariableAccess(VariableID variable, FunctionID function, ValueID id, const Value& value, FlowScopeAccesses& accesses)
{
    FlowScopeLocation location = variable.index();
    if (value.opcode == Opcode::GetModuleVariable)
        accesses.reads.append({ location, function, id });
    else if (value.opcode == Opcode::PutModuleVariable)
        accesses.stores.append({ location, function, id });
    else
        accesses.opaqueVariables.append(location);
}

static void collectEnvironmentAccess(const FlowScopeLocationNumbering& numbering, LayoutID layout, FunctionID function, ValueID id, const Value& value, FlowScopeAccesses& accesses)
{
    if (value.opcode == Opcode::GetEnvironmentField)
        accesses.reads.append({ *numbering.locationOf(layout, value.field()), function, id });
    else if (value.opcode == Opcode::PutEnvironmentField)
        accesses.stores.append({ *numbering.locationOf(layout, value.field()), function, id });
    else if (value.opcode == Opcode::NewObject)
        accesses.allocations.append({ layout, function, id });
    else
        accesses.opaqueLayouts.append(layout);
}

static bool readsModuleValue(ModuleReferenceKind kind)
{
    switch (kind) {
    case ModuleReferenceKind::Target:
        return true;
    case ModuleReferenceKind::Referrer:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void collectScopeAccesses(const Program& program, const FlowScopeLocationNumbering& numbering, FunctionID functionID, FlowScopeAccesses& accesses)
{
    const Function& function = program.at(functionID);
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            forEachVariable(value, [&](VariableID variable) {
                if (variable)
                    collectVariableAccess(variable, functionID, id, value, accesses);
            });
            forEachLayout(value, [&](LayoutID layout) {
                if (layout && program.at(layout).kind == LayoutKind::Environment)
                    collectEnvironmentAccess(numbering, layout, functionID, id, value, accesses);
            });
            forEachFunctionReference(value, [&](FunctionID referenced, FunctionReferenceKind kind) {
                if (referenced)
                    accesses.references.append({ referenced, kind, id });
            });
            forEachModuleReference(value, [&](ModuleID module, ModuleReferenceKind kind) {
                if (module && readsModuleValue(kind))
                    accesses.moduleValueReads.append({ module, functionID, id });
            });
            forEachEvalScope(value, [&](EvalScopeID scope) {
                if (scope)
                    accesses.evalScopes.append(scope);
            });
        }
    }
}

static void shrinkToFit(FlowScopeAccesses& accesses)
{
    accesses.reads.shrinkToFit();
    accesses.stores.shrinkToFit();
    accesses.allocations.shrinkToFit();
    accesses.moduleValueReads.shrinkToFit();
    accesses.references.shrinkToFit();
    accesses.opaqueVariables.shrinkToFit();
    accesses.opaqueLayouts.shrinkToFit();
    accesses.evalScopes.shrinkToFit();
}

void ensureScopeAccesses(const FlowOrderInputs& inputs, const FlowScopeLocationNumbering& numbering, FlowScopeAccessesByFunction& byFunction)
{
    const Program& program = inputs.program();
    RELEASE_ASSERT(byFunction.size() == program.functions.size());
    forEachIndexInParallel(inputs.build().parallelLoops(), program.functions.size(), [&](unsigned index) {
        if (byFunction[index] || !program.functions[index].hasBody() || !inputs.isReached(FunctionID(index)))
            return;
        byFunction[index].reset(new FlowScopeAccesses);
        collectScopeAccesses(program, numbering, FunctionID(index), *byFunction[index]);
        shrinkToFit(*byFunction[index]);
    });
}

static void markLayoutOpaque(FlowScopeIndex& index, LayoutID layout)
{
    uint32_t end = index.numbering.firstField[layout.index() + 1];
    for (uint32_t location = index.numbering.firstField[layout.index()]; location < end; ++location)
        index.opaqueLocations.set(location);
}

static void markEvalBindingsOpaque(FlowScopeIndex& index, const EvalScope& scope)
{
    for (const EvalBinding& binding : scope.bindings) {
        if (binding.variable)
            index.opaqueLocations.set(binding.variable.index());
        if (!binding.layout)
            continue;
        if (!binding.field) {
            markLayoutOpaque(index, binding.layout);
            continue;
        }
        if (auto location = index.numbering.locationOf(binding.layout, *binding.field))
            index.opaqueLocations.set(*location);
    }
}

static void markEvalBindingsOpaque(FlowScopeIndex& index)
{
    for (const auto& body : index.bodies) {
        if (!body)
            continue;
        for (EvalScopeID scope : body->evalScopes)
            markEvalBindingsOpaque(index, index.program().at(scope));
    }
}

namespace {

// Puts the accesses of members into the lists of the index. Only a store that has certainly written the member when it completes is listed.
// An access in an instance that stands for the calls of one site happens while that call runs, so it is listed as the call.
class MemberAccessMerger {
public:
    explicit MemberAccessMerger(FlowScopeIndex& index)
        : m_index(index)
        , m_inputs(index.inputs)
    {
    }

    void run();

private:
    using Operation = std::tuple<FunctionID, FlowInstanceID, ValueID>;
    using CallSite = std::pair<FunctionID, ValueID>;

    static Operation keyOf(const FlowMemberOperation& operation) { return { operation.function, operation.callSiteInstance, operation.origin }; }

    void collectStoredHolders();
    void collectReplacedPrototypes();
    void collectStoresOfPrototypeLinks();
    void collectCallees();
    void noteCallee(FlowInstanceID caller, ValueID site, FlowInstanceID callee);
    FunctionAnalyses& analysesOf(FunctionID);
    bool completesWhenRefused(const FlowMemberOperation&) const;
    bool storesToHolderAlone(const FlowMemberAccess&, Vector<FlowScopeLocation, 1>* assumedStores = nullptr) const;
    bool completesBeforeEveryReturn(FunctionID, ValueID);
    bool isCoveredInOwnInstance(const FlowMemberAccess& read);
    bool isHandedOnlyToStoreOfLink(const FlowMemberAccess& read, FlowObjectID prototypeObject);
    void listReadOfPrototypeLink(const FlowMemberAccess& readOfPrototypeProperty);
    std::optional<CallSite> findCompletedOperation(const FlowMemberOperation& store);
    CallSite findStartedOperation(const FlowMemberOperation& read) const;
    void listRead(FlowScopeLocation, const FlowMemberOperation&);
    void listStore(FlowScopeLocation, const FlowMemberAccess&);
    void listReadsThroughUnknownObject();

    FlowScopeIndex& m_index;
    const FlowOrderInputs& m_inputs;
    HashMap<Operation, Vector<FlowObjectID, 2>> m_holdersStoredBy;
    HashSet<Operation> m_uncertainStores;
    HashMap<FlowObjectID, FlowScopeLocation> m_prototypePropertyOfReplacedObject;
    HashMap<FlowObjectID, Vector<Operation, 1>> m_storesOfPrototypeLink;
    HashMap<CallSite, FlowInstanceID> m_soleCalleeOf;
    HashMap<FunctionID, std::unique_ptr<FunctionAnalyses>> m_analyses;
    HashMap<FlowInstanceID, Vector<const FlowMemberAccess*, 2>> m_certainStoresOfCallSiteInstance;
    HashMap<PropertyKeyID, Vector<FlowMemberOperation, 2>> m_readsThroughUnknownObject;
    BitVector m_locationsWithListedStore;
};

void MemberAccessMerger::collectStoredHolders()
{
    for (const FlowMemberAccess& access : m_inputs.memberAccesses()) {
        if (!access.isStore || !access.operation.function)
            continue;
        Vector<FlowObjectID, 2>& holders = m_holdersStoredBy.add(keyOf(access.operation), Vector<FlowObjectID, 2>()).iterator->value;
        if (!holders.contains(access.holder))
            holders.append(access.holder);
    }
    for (const FlowMemberOperation& operation : m_inputs.uncertainMemberStores())
        m_uncertainStores.add(keyOf(operation));
}

// Nobody ever holds the prototype object a function was created with when another object is stored into the prototype property before the first read of it.
// The order of that property decides, and it is computed with the order of the members, so a store that counts on it names it.
void MemberAccessMerger::collectReplacedPrototypes()
{
    PropertyKeyID prototypeKey = m_index.program().keyOf(BuiltinNames::prototype);
    const FlowObjectTable& objects = m_inputs.objects();
    for (const FlowMemberAccess& access : m_inputs.memberAccesses()) {
        if (!access.isStore || access.key != prototypeKey || objects.kindOf(access.holder) != FlowObjectKind::Closure)
            continue;
        if (FlowObjectID original = objects.findPrototype(access.holder))
            m_prototypePropertyOfReplacedObject.add(original, *m_index.numbering.locationOf(access.holder, access.key));
    }
}

void MemberAccessMerger::collectStoresOfPrototypeLinks()
{
    PropertyKeyID link = m_index.program().keyOf(BuiltinNames::__proto__);
    for (const FlowMemberAccess& access : m_inputs.memberAccesses()) {
        if (access.isStore && access.key == link && m_inputs.objects().kindOf(access.holder) == FlowObjectKind::Prototype)
            m_storesOfPrototypeLink.add(access.holder, Vector<Operation, 1>()).iterator->value.append(keyOf(access.operation));
    }
}

void MemberAccessMerger::noteCallee(FlowInstanceID caller, ValueID site, FlowInstanceID callee)
{
    if (!site || !caller || m_inputs.isHost(caller))
        return;
    auto result = m_soleCalleeOf.add({ m_inputs.functionOf(caller), site }, callee);
    if (!result.isNewEntry && result.iterator->value != callee)
        result.iterator->value = FlowInstanceID();
}

void MemberAccessMerger::collectCallees()
{
    for (const FlowCallLink& link : m_inputs.callLinks())
        noteCallee(link.callerInstance, link.site, link.kind == FlowCallLinkKind::Call ? link.calleeInstance : FlowInstanceID());
    for (const FlowBuiltinCallLink& link : m_inputs.builtinCallLinks())
        noteCallee(link.callerInstance, link.site, { });
    for (const FlowUnknownCallee& callee : m_inputs.unknownCallees())
        noteCallee(callee.callerInstance, callee.site, { });
}

FunctionAnalyses& MemberAccessMerger::analysesOf(FunctionID function)
{
    return *m_analyses.ensure(function, [&] {
        return makeUnique<FunctionAnalyses>(AnalysisContext { m_inputs.build(), m_index.program(), nullptr, nullptr, function }, m_index.program().at(function));
    }).iterator->value;
}

// A holder that is frozen or not extensible refuses the store. Strict code throws then, and a built-in may answer false.
bool MemberAccessMerger::completesWhenRefused(const FlowMemberOperation& operation) const
{
    const Value& value = m_index.program().at(operation.function).at(operation.origin);
    switch (effectRuleOf(value.opcode)) {
    case EffectRule::Call:
    case EffectRule::Builtin:
    case EffectRule::Inline:
        return true;
    case EffectRule::Access:
    case EffectRule::Declared:
    case EffectRule::Operator:
        break;
    }
    return Value::opcodesWithMode.contains(value.opcode) && value.mode() == ECMAMode::Sloppy;
}

// The statement of the store lists each holder among its receivers, and says so when it has any other receiver. A function past the work limit says nothing more.
bool MemberAccessMerger::storesToHolderAlone(const FlowMemberAccess& store, Vector<FlowScopeLocation, 1>* assumedStores) const
{
    const FlowMemberOperation& operation = store.operation;
    if (m_inputs.isPastWorkLimit(operation.function) || m_uncertainStores.contains(keyOf(operation)) || m_prototypePropertyOfReplacedObject.contains(store.holder))
        return false;
    if (completesWhenRefused(operation) && m_inputs.mayChangeIntegrity(store.holder))
        return false;

    // An operation with several instances may store to another holder in each.
    for (FlowObjectID receiver : m_holdersStoredBy.find(keyOf(operation))->value) {
        if (receiver == store.holder)
            continue;
        auto replaced = m_prototypePropertyOfReplacedObject.find(receiver);
        if (replaced == m_prototypePropertyOfReplacedObject.end())
            return false;
        if (assumedStores && !assumedStores->contains(replaced->value))
            assumedStores->append(replaced->value);
    }
    return true;
}

bool MemberAccessMerger::completesBeforeEveryReturn(FunctionID functionID, ValueID value)
{
    const Function& function = m_index.program().at(functionID);
    if (function.isUnsplitCoroutine())
        return false;
    FunctionAnalyses& analyses = analysesOf(functionID);
    return std::ranges::all_of(analyses.cfg().reversePostOrder(), [&](BlockID block) {
        return !returnsToCaller(function.at(block).terminator.kind) || analyses.dominates(value, pointBeforeTerminatorOf(function, block));
    });
}

bool MemberAccessMerger::isCoveredInOwnInstance(const FlowMemberAccess& read)
{
    FlowInstanceID instance = read.operation.callSiteInstance;
    auto stores = instance ? m_certainStoresOfCallSiteInstance.find(instance) : m_certainStoresOfCallSiteInstance.end();
    if (stores == m_certainStoresOfCallSiteInstance.end())
        return false;
    return std::ranges::any_of(stores->value, [&](const FlowMemberAccess* store) {
        if (store->holder != read.holder || store->key != read.key)
            return false;
        FunctionAnalyses& analyses = analysesOf(read.operation.function);
        ValueLocation location = analyses.useLists().locationOf(read.operation.origin);
        return location.placement == ValuePlacement::InBlock && analyses.dominates(store->operation.origin, ProgramPoint { location.block, location.index });
    });
}

// The operation that changes the link takes the object from a read of the prototype property. Nothing else sees what that read gives.
bool MemberAccessMerger::isHandedOnlyToStoreOfLink(const FlowMemberAccess& read, FlowObjectID prototypeObject)
{
    const FlowMemberOperation& operation = read.operation;
    if (!operation.function)
        return false;
    std::optional<Use> use = analysesOf(operation.function).useLists().singleUse(operation.origin);
    if (!use || use->kind != Use::Kind::Operand)
        return false;
    return m_storesOfPrototypeLink.find(prototypeObject)->value.contains(Operation { operation.function, operation.callSiteInstance, use->user });
}

// Whoever follows the link of the prototype object that a function is created with holds that object or one that inherits from it, and only a read of the prototype property leads there.
void MemberAccessMerger::listReadOfPrototypeLink(const FlowMemberAccess& readOfPrototypeProperty)
{
    FlowObjectID prototypeObject = m_inputs.objects().findPrototype(readOfPrototypeProperty.holder);
    if (!prototypeObject || !m_storesOfPrototypeLink.contains(prototypeObject))
        return;
    std::optional<FlowScopeLocation> link = m_index.numbering.locationOf(prototypeObject, m_index.program().keyOf(BuiltinNames::__proto__));
    if (link && !isHandedOnlyToStoreOfLink(readOfPrototypeProperty, prototypeObject))
        listRead(*link, readOfPrototypeProperty.operation);
}

// The outermost operation that has written the member whenever it has completed.
auto MemberAccessMerger::findCompletedOperation(const FlowMemberOperation& store) -> std::optional<CallSite>
{
    CallSite operation { store.function, store.origin };
    for (FlowInstanceID instance = store.callSiteInstance; instance;) {
        const FlowCallSiteKey& callSite = m_inputs.callSiteOf(instance);
        if (m_inputs.isPastWorkLimit(callSite.caller) || m_soleCalleeOf.get({ callSite.caller, callSite.call }) != instance)
            return std::nullopt;
        if (!completesBeforeEveryReturn(operation.first, operation.second))
            return std::nullopt;
        operation = { callSite.caller, callSite.call };
        instance = callSite.outer;
    }
    return operation;
}

auto MemberAccessMerger::findStartedOperation(const FlowMemberOperation& read) const -> CallSite
{
    CallSite operation { read.function, read.origin };
    for (FlowInstanceID instance = read.callSiteInstance; instance;) {
        const FlowCallSiteKey& callSite = m_inputs.callSiteOf(instance);
        operation = { callSite.caller, callSite.call };
        instance = callSite.outer;
    }
    return operation;
}

void MemberAccessMerger::listRead(FlowScopeLocation location, const FlowMemberOperation& read)
{
    // The host calls a function of the library at a site of its own too.
    CallSite operation = read.function ? findStartedOperation(read) : CallSite { };
    if (!operation.first || m_inputs.isHost(operation.first)) {
        m_index.opaqueLocations.set(location);
        return;
    }
    m_index.reads.append({ location, operation.first, operation.second });
}

void MemberAccessMerger::listStore(FlowScopeLocation location, const FlowMemberAccess& store)
{
    if (!store.operation.function) {
        m_index.opaqueLocations.set(location);
        return;
    }
    Vector<FlowScopeLocation, 1> assumedStores;
    if (!storesToHolderAlone(store, &assumedStores))
        return;
    std::optional<CallSite> operation = findCompletedOperation(store.operation);
    if (!operation)
        return;
    m_index.stores.append({ location, operation->first, operation->second });
    m_locationsWithListedStore.set(location);
    for (FlowScopeLocation assumed : assumedStores)
        m_index.assumedStores.append({ location, assumed });
}

// An object that has escaped comes back as the unknown object, and so do the objects that inherit from it. A member without a listed store has no order to lose.
void MemberAccessMerger::listReadsThroughUnknownObject()
{
    const FlowScopeLocationNumbering& numbering = m_index.numbering;
    for (size_t location : m_locationsWithListedStore) {
        const FlowMemberLocation& member = numbering.members[location - numbering.firstMember()];
        if (!m_inputs.hasEscaped(member.holder))
            continue;
        auto reads = m_readsThroughUnknownObject.find(member.key);
        if (reads == m_readsThroughUnknownObject.end())
            continue;
        for (const FlowMemberOperation& read : reads->value)
            listRead(static_cast<FlowScopeLocation>(location), read);
    }
}

void MemberAccessMerger::run()
{
    collectStoredHolders();
    collectReplacedPrototypes();
    collectStoresOfPrototypeLinks();
    collectCallees();
    for (const FlowMemberAccess& access : m_inputs.memberAccesses()) {
        if (access.operation.function && access.isStore && access.operation.callSiteInstance && storesToHolderAlone(access))
            m_certainStoresOfCallSiteInstance.add(access.operation.callSiteInstance, Vector<const FlowMemberAccess*, 2>()).iterator->value.append(&access);
    }

    PropertyKeyID prototypeKey = m_index.program().keyOf(BuiltinNames::prototype);
    for (const FlowMemberAccess& access : m_inputs.memberAccesses()) {
        if (access.holder == unknownFlowObject && !access.isStore) {
            m_readsThroughUnknownObject.add(access.key, Vector<FlowMemberOperation, 2>()).iterator->value.append(access.operation);
            continue;
        }
        if (!access.isStore && access.key == prototypeKey && m_inputs.objects().kindOf(access.holder) == FlowObjectKind::Closure)
            listReadOfPrototypeLink(access);
        std::optional<FlowScopeLocation> location = m_index.numbering.locationOf(access.holder, access.key);
        if (!location)
            continue;
        if (access.isStore)
            listStore(*location, access);
        else if (!isCoveredInOwnInstance(access))
            listRead(*location, access.operation);
    }
    listReadsThroughUnknownObject();
}

} // anonymous namespace

static void mergeBodyAccesses(FlowScopeIndex& index)
{
    for (const auto& body : index.bodies) {
        if (!body)
            continue;
        index.reads.appendVector(body->reads);
        index.stores.appendVector(body->stores);
        index.allocations.appendVector(body->allocations);
        index.moduleValueReads.appendVector(body->moduleValueReads);
        for (FlowScopeLocation location : body->opaqueVariables)
            index.opaqueLocations.set(location);
        for (LayoutID layout : body->opaqueLayouts)
            markLayoutOpaque(index, layout);
    }

    const FlowOrderInputs& inputs = index.inputs;
    for (const FlowModuleLoadRecord& load : inputs.moduleLoads()) {
        if (inputs.isHost(load.caller) || !load.origin)
            index.anyModuleValueMayBeRead = true;
        else
            index.moduleValueReads.append({ load.module, inputs.functionOf(load.caller), load.origin });
    }

    for (const FlowStaticFieldAccess& access : inputs.staticFieldAccesses()) {
        FlowScopeLocation location = access.variable.index();
        if (!access.function) {
            index.opaqueLocations.set(location);
            continue;
        }
        index.accessesThroughClassObjects.append({ location, access.function, access.origin });
        if (access.isStore)
            index.storesThroughClassObjects.append({ location, access.function, access.origin });
    }

    MemberAccessMerger(index).run();

    const ParallelLoops& loops = inputs.build().parallelLoops();
    parallelSort(loops, index.reads.mutableSpan(), std::less<FlowScopeLocationAccess>());
    parallelSort(loops, index.stores.mutableSpan(), std::less<FlowScopeLocationAccess>());
    parallelSort(loops, index.accessesThroughClassObjects.mutableSpan(), std::less<FlowScopeLocationAccess>());
    parallelSort(loops, index.storesThroughClassObjects.mutableSpan(), std::less<FlowScopeLocationAccess>());
    parallelSort(loops, index.allocations.mutableSpan(), std::less<FlowEnvironmentAllocation>());
    parallelSort(loops, index.moduleValueReads.mutableSpan(), std::less<FlowModuleValueRead>());
}

static bool canOwnVariables(ModuleKind kind)
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

static FunctionID moduleBodyOwnerOf(const FlowScopeIndex& index, ModuleID moduleID)
{
    const Program& program = index.program();
    if (!moduleID || moduleID.index() >= program.modules.size() || !index.inputs.evaluatedModules().get(moduleID.index()))
        return { };
    const Module& module = program.at(moduleID);
    if (!canOwnVariables(module.kind) || !module.body || !index.bodies[module.body.index()])
        return { };
    return module.body;
}

static void findVariableOwners(FlowScopeIndex& index)
{
    const Program& program = index.program();
    for (const Module& module : program.modules) {
        for (const SyntheticExport& syntheticExport : module.syntheticExports) {
            if (syntheticExport.variable)
                index.syntheticVariables.set(syntheticExport.variable.index());
        }
    }
    for (unsigned variable = 0; variable < program.variables.size(); ++variable) {
        if (index.syntheticVariables.get(variable))
            continue;
        index.locationOwners[variable] = moduleBodyOwnerOf(index, program.variables[variable].module);
        if (!index.locationOwners[variable])
            index.opaqueLocations.set(variable);
    }
}

// Code that the build cannot see hands a class object back as an unknown object, and an access to that one is in no list.
static void findStaticFieldsOfEscapedClasses(FlowScopeIndex& index)
{
    const FlowOrderInputs& inputs = index.inputs;
    for (const Layout& layout : index.program().layouts) {
        if (layout.staticFields.isEmpty() || !layout.constructor)
            continue;
        if (!inputs.hasClassObjectEscaped(layout.constructor))
            continue;
        for (const StaticField& field : layout.staticFields)
            index.staticFieldsOfEscapedClasses.set(field.variable.index());
    }
}

static bool canRunOnlyDuringCall(const Function& function)
{
    return !function.mayBeCalledByHost() && !function.isModuleBody() && !function.isUnsplitCoroutine();
}

static void findSoleCallSites(FlowScopeIndex& index)
{
    const Program& program = index.program();
    for (unsigned caller = 0; caller < index.bodies.size(); ++caller) {
        if (!index.bodies[caller])
            continue;
        for (const FlowFunctionMention& mention : index.bodies[caller]->references) {
            ++index.calls.numberOfMentions[mention.function.index()];
            if (program.functions[caller].at(mention.value).opcode == Opcode::CallDirect)
                index.calls.soleCallSites[mention.function.index()] = { FunctionID(caller), mention.value };
        }
    }
    for (const Variable& variable : program.variables) {
        if (variable.function)
            ++index.calls.numberOfMentions[variable.function.index()];
    }
    for (unsigned function = 0; function < program.functions.size(); ++function) {
        if (index.calls.numberOfMentions[function] != 1 || !index.bodies[function] || !canRunOnlyDuringCall(program.functions[function]))
            index.calls.soleCallSites[function] = { };
    }
    for (const FlowCallLink& link : index.inputs.callLinks()) {
        FlowCallSite& site = index.calls.soleCallSites[link.callee.index()];
        if (site != FlowCallSite { link.caller, link.site })
            site = { };
    }
}

static void collectCalls(FlowScopeIndex& index)
{
    const FlowOrderInputs& inputs = index.inputs;
    FlowScopeCalls& calls = index.calls;
    BitVector isCalled(index.program().functions.size());
    for (const FlowCallLink& link : inputs.callLinks()) {
        isCalled.set(link.callee.index());
        if (inputs.isHost(link.caller) || !link.site)
            calls.functionsEnteredByHost.set(link.callee.index());
        else
            calls.byCaller.append({ link.caller, link.callee, link.site, { } });
    }
    for (unsigned function = 0; function < index.program().functions.size(); ++function) {
        if (!isCalled.get(function))
            calls.functionsEnteredByHost.set(function);
    }

    const ParallelLoops& loops = inputs.build().parallelLoops();
    sortAndRemoveDuplicates(loops, calls.byCaller);
    // Every owner asks for every call of the functions it looks at, and the answer lies in the caller's body alone.
    forEachIndexInParallel(loops, calls.byCaller.size(), [&](unsigned call) {
        FlowScopeCall& record = calls.byCaller[call];
        record.subject = variableBehindSubjectOf(index.program().at(record.caller), record.site);
    });
    calls.byCallee = calls.byCaller;
    parallelSort(loops, calls.byCallee.mutableSpan(), [](const FlowScopeCall& a, const FlowScopeCall& b) {
        return std::tuple { a.callee, a.caller, a.site } < std::tuple { b.callee, b.caller, b.site };
    });
}

static void findEnvironmentOwners(FlowScopeIndex& index)
{
    for (unsigned layoutIndex = 0; layoutIndex < index.program().layouts.size(); ++layoutIndex) {
        LayoutID layout(layoutIndex);
        auto sites = allocationsOf(index, layout);
        if (sites.empty() || sites.front().function != sites.back().function)
            continue;
        index.environmentOwners[layoutIndex] = sites.front().function;
        uint32_t end = index.numbering.firstField[layoutIndex + 1];
        for (uint32_t location = index.numbering.firstField[layoutIndex]; location < end; ++location)
            index.locationOwners[location] = sites.front().function;
    }
}

static FunctionID outermostCallerOf(const FlowScopeIndex& index, FunctionID function)
{
    for (size_t remaining = index.program().functions.size(); remaining; --remaining) {
        FunctionID caller = index.calls.soleCallSites[function.index()].caller;
        if (!caller)
            return function;
        function = caller;
    }
    return { };
}

// The owner is the function in whose activation the first listed store runs.
static void findMemberOwners(FlowScopeIndex& index)
{
    for (FlowScopeLocation location = index.numbering.firstMember(); location < index.numbering.numberOfLocations(); ++location) {
        auto stores = accessesAt(index.stores.span(), location);
        if (!stores.empty())
            index.locationOwners[location] = outermostCallerOf(index, stores.front().function);
        if (!index.locationOwners[location])
            index.opaqueLocations.set(location);
    }
}

static FlowScopeOwner& ownerFor(FlowScopeIndex& index, FunctionID function)
{
    auto owner = std::ranges::lower_bound(index.owners, function, { }, &FlowScopeOwner::function);
    RELEASE_ASSERT(owner != index.owners.end() && owner->function == function);
    return *owner;
}

static void createOwners(FlowScopeIndex& index)
{
    Vector<FunctionID> functions;
    for (FunctionID owner : index.locationOwners) {
        if (owner)
            functions.append(owner);
    }
    for (FunctionID owner : index.environmentOwners) {
        if (owner)
            functions.append(owner);
    }
    sortAndRemoveDuplicates(index.inputs.build().parallelLoops(), functions);
    index.owners.reserveInitialCapacity(functions.size());
    for (FunctionID function : functions)
        index.owners.append(FlowScopeOwner { function, { }, { }, { }, { }, { }, { } });
}

static void assignExport(FlowScopeIndex& index, VariableID variable, ModuleID exporter)
{
    FunctionID function = variable ? index.locationOwners[variable.index()] : FunctionID();
    if (!function)
        return;
    FlowScopeOwner& owner = ownerFor(index, function);
    owner.exports.append({ variable, exporter });
    if (owner.exporters.isEmpty() || owner.exporters.last() != exporter)
        owner.exporters.append(exporter);
}

// Whoever holds the exporter's namespace object reaches the exported namespace object through it, and every variable behind that one.
static void assignExportsOfNamespace(FlowScopeIndex& index, ModuleID exportedNamespace, ModuleID exporter)
{
    const Program& program = index.program();
    BitVector visitedModules(program.modules.size());
    Vector<ModuleID, 8> pending { exportedNamespace };
    while (!pending.isEmpty()) {
        ModuleID module = pending.takeLast();
        if (!module || visitedModules.set(module.index()))
            continue;
        for (const ModuleExport& moduleExport : program.at(module).exports) {
            switch (moduleExport.kind) {
            case ModuleExportKind::Variable:
                assignExport(index, moduleExport.variable, exporter);
                break;
            case ModuleExportKind::Namespace:
                pending.append(moduleExport.module);
                break;
            case ModuleExportKind::Indirect:
                break;
            }
        }
    }
}

static void assignOwnedLocations(FlowScopeIndex& index)
{
    const Program& program = index.program();
    for (unsigned variable = 0; variable < program.variables.size(); ++variable) {
        FunctionID function = index.locationOwners[variable];
        if (!function)
            continue;
        FlowScopeOwner& owner = ownerFor(index, function);
        owner.module = program.variables[variable].module;
        owner.variables.append(VariableID(variable));
    }
    for (unsigned layout = 0; layout < program.layouts.size(); ++layout) {
        if (FunctionID function = index.environmentOwners[layout])
            ownerFor(index, function).environments.append(LayoutID(layout));
    }
    for (FlowScopeLocation location = index.numbering.firstMember(); location < index.numbering.numberOfLocations(); ++location) {
        FunctionID function = index.locationOwners[location];
        if (!function)
            continue;
        FlowScopeOwner& owner = ownerFor(index, function);
        owner.members.append(location);
        if (moduleBodyOwnerOf(index, program.at(function).module) == function)
            owner.module = program.at(function).module;
    }
    for (unsigned moduleIndex = 0; moduleIndex < program.modules.size(); ++moduleIndex) {
        for (const ModuleExport& moduleExport : program.modules[moduleIndex].exports) {
            switch (moduleExport.kind) {
            case ModuleExportKind::Variable:
                assignExport(index, moduleExport.variable, ModuleID(moduleIndex));
                break;
            case ModuleExportKind::Namespace:
                assignExportsOfNamespace(index, moduleExport.module, ModuleID(moduleIndex));
                break;
            case ModuleExportKind::Indirect:
                break;
            }
        }
    }
    for (FlowScopeOwner& owner : index.owners)
        std::ranges::sort(owner.exports);
}

FlowScopeIndex createScopeIndex(const FlowOrderInputs& inputs, const FlowModuleOrder& moduleOrder, const FlowScopeAccessesByFunction& bodies, FlowScopeLocationNumbering&& numbering)
{
    const Program& program = inputs.program();
    FlowScopeIndex index { inputs, moduleOrder, bodies, WTF::move(numbering), { }, { }, { }, { }, { }, { }, { }, { }, { }, { }, { }, { }, { }, { }, false };
    uint32_t numberOfLocations = index.numbering.numberOfLocations();
    index.opaqueLocations = BitVector(numberOfLocations);
    index.syntheticVariables = BitVector(program.variables.size());
    index.staticFieldsOfEscapedClasses = BitVector(program.variables.size());
    index.locationOwners.grow(numberOfLocations);
    index.environmentOwners.grow(program.layouts.size());
    index.calls.numberOfMentions.fill(0, program.functions.size());
    index.calls.soleCallSites.grow(program.functions.size());
    index.calls.functionsEnteredByHost = BitVector(program.functions.size());
    index.anyModuleValueMayBeRead = inputs.mayLoadAnyModule();

    mergeBodyAccesses(index);
    markEvalBindingsOpaque(index);
    findVariableOwners(index);
    findStaticFieldsOfEscapedClasses(index);
    findSoleCallSites(index);
    collectCalls(index);
    findEnvironmentOwners(index);
    findMemberOwners(index);
    createOwners(index);
    assignOwnedLocations(index);
    return index;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
