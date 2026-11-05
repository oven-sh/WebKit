/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The scope of a function that runs at most once is never made: its variables lie among the variables of the module, and a closure over it captures nothing.
// Suspension: a coroutine neither makes such a scope nor loses a capture. Throw: a store that was reached stays, as it would have stayed in the cell.
// Enforced: the maker stops the program by name at its second entry (EnterBodyThatRunsOnce), is copied into no caller and is never written straight.
// Withheld (Mechanism::PromotedEnvironments): the tables are empty, and every reader goes by the captures as they are written.

#include "config.h"
#include "AOTPromotedEnvironments.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTMechanisms.h"
#include "AOTSlotPlan.h"
#include "AOTSlotRepresentation.h"
#include "AOTSort.h"
#include "AOTThinSummary.h"
#include <optional>
#include <tuple>
#include <type_traits>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct FunctionsWhoseClosuresOperationMakes {
    template<typename IDType, typename... Role>
    void operator()(IDType id, Role... role)
    {
        if constexpr (std::is_same_v<IDType, FunctionID>) {
            if (id && ((role == FunctionReferenceKind::Closure) || ...))
                functions.set(id.index());
        }
    }

    BitVector& functions;
};

class PlannerOfPromotedEnvironments {
public:
    PlannerOfPromotedEnvironments(const Program& program, const Facts& facts, const SlotPlanParts& slots)
        : m_program(program)
        , m_facts(facts)
        , m_slots(slots)
        , m_isMade(program.layouts.size())
        , m_isRefused(program.layouts.size())
    {
    }

    PromotedEnvironments run();

private:
    static constexpr uint32_t noSource = 0;
    static constexpr uint32_t noSourceServes = 1;
    static constexpr uint32_t firstSource = 2;

    struct ScopeMadeOnce {
        LayoutID layout;
        FunctionID maker;
    };

    struct Source {
        FunctionID function;
        TypeID type;
        VariableID variable;
        uint32_t firstSupplier { 0 };
        uint32_t numberOfStores { 0 };
    };

    struct Supplier {
        uint32_t source { 0 };
        ValueID site;
        FunctionID callee;
        uint32_t capture { 0 };
    };

    bool servesAsMaker(FunctionID, const ThinSummary&, const BodyIndex&) const;
    bool mayLieAmongVariables(LayoutID) const;
    bool capturesMayBeLeftOut(FunctionID) const;
    TypeID typeOfCapture(FunctionID function, uint32_t capture) const { return m_program.provenTypeOf(m_program.at(function).captures[capture]); }

    void findScopesMadeOnce();
    void refuseScopesThatPositionsHold();

    uint32_t nodeOf(FunctionID function, uint32_t capture) const { return m_facts.linked().firstCaptureOfFunction[function.index()] + capture; }
    uint32_t rootOf(uint32_t node);
    void unite(uint32_t, uint32_t);
    void addSource(uint32_t node, uint32_t source);
    bool followCapturedValues();
    void settleSources();

    VariableID addVariable(PromotedVariable&&);
    void leaveOutCaptures();
    void listStores();
    void findFunctionsThatLeadToMakers();

    const Program& m_program;
    const Facts& m_facts;
    const SlotPlanParts& m_slots;
    BitVector m_isMade;
    BitVector m_isRefused;
    BitVector m_functionsWhoseClosuresRunTimeMakes;
    Vector<ScopeMadeOnce> m_scopes;
    Vector<uint32_t> m_parents;
    Vector<uint32_t> m_sourcesOfClasses;
    Vector<uint32_t> m_sourcesOfValues;
    Vector<Source> m_sources;
    Vector<Supplier> m_suppliers;
    PromotedEnvironments m_result;
};

bool PlannerOfPromotedEnvironments::servesAsMaker(FunctionID id, const ThinSummary& summary, const BodyIndex& index) const
{
    const Function& function = m_program.at(id);
    if (!m_facts.runsAtMostOnce(id) || function.isCoroutine() || summary.has(SummaryTrait::HasDirectEval) || index.hasIrreducibleFlow())
        return false;
    // The run time puts the state of a module of the library back and runs its body again when the body has thrown.
    return m_program.at(function.module).kind != ModuleKind::RuntimeSource;
}

bool PlannerOfPromotedEnvironments::mayLieAmongVariables(LayoutID id) const
{
    const Layout& layout = m_program.at(id);
    const PlannedLayout& planned = m_slots.layouts[id.index()];
    if (layout.kind != LayoutKind::Environment || planned.storage != LayoutStorage::Environment || planned.liesOnlyInFrames)
        return false;
    if (planned.numberOfFields() < layout.fields.size())
        return false;
    for (FieldIndex field = 0; field < layout.fields.size(); ++field) {
        const PlacedSlot& slot = planned.slots[field];
        if (slot.isOutOfLine() || slot.deferredFunction || slot.representation == SlotRepresentation::DeferredClosure)
            return false;
        if (!m_program.at(m_program.provenTypeOf(layout.fields[field].declared)).isJavaScriptValue())
            return false;
    }
    return true;
}

bool PlannerOfPromotedEnvironments::capturesMayBeLeftOut(FunctionID id) const
{
    const Function& function = m_program.at(id);
    if (!function.hasBody() || function.isCoroutine() || !function.has(FunctionCapabilities::MayChangeSignature) || m_functionsWhoseClosuresRunTimeMakes.get(id.index()))
        return false;
    switch (function.kind) {
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
        return true;
    // The record of a class makes the closures of members over the word that the class object captures.
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void PlannerOfPromotedEnvironments::findScopesMadeOnce()
{
    const ThinSummaries& summaries = m_facts.summaries();
    for (uint32_t number = 0; number < summaries.size(); ++number) {
        FunctionID id(number);
        const ThinSummary& summary = summaries.at(id);
        const BodyIndex* index = summary.environmentsMade.isEmpty() ? nullptr : m_facts.bodies().find(id);
        if (!index)
            continue;
        bool serves = servesAsMaker(id, summary, *index);
        for (ValueID site : summary.environmentsMade) {
            LayoutID layout = index->value(site).as<Opcode::NewObject>().layout();
            if (m_isMade.quickSet(layout.index()) || !serves || index->loopOf(index->blockOf(site)) || !mayLieAmongVariables(layout))
                m_isRefused.quickSet(layout.index());
            else
                m_scopes.append({ layout, id });
        }
    }
}

void PlannerOfPromotedEnvironments::refuseScopesThatPositionsHold()
{
    auto scopeIn = [&](const DeclaredType& declared) -> std::pair<LayoutID, bool> {
        TypeID written = declared.written();
        if (!written || holdsOnlyPrimitives(written))
            return { };
        Type type = m_program.at(written);
        return { type.environment, !!type.primitives };
    };
    auto refuse = [&](const DeclaredType& declared) {
        if (LayoutID scope = scopeIn(declared).first)
            m_isRefused.quickSet(scope.index());
    };

    for (const Layout& layout : m_program.layouts) {
        if (!layout.isEnvironment())
            continue;
        for (const Field& field : layout.fields)
            refuse(field.declared);
    }
    for (const Variable& variable : m_program.variables)
        refuse(variable.declared);
    for (uint32_t number = 0; number < m_program.functions.size(); ++number) {
        const Function& function = m_program.functions[number];
        refuse(function.thisType);
        for (const DeclaredType& parameter : function.parameters)
            refuse(parameter);
        for (const DeclaredType& result : function.results)
            refuse(result);
        bool keepsCaptures = !function.captures.isEmpty() && !capturesMayBeLeftOut(FunctionID(number));
        for (const DeclaredCapture& capture : function.captures) {
            auto [scope, mayBeNoScope] = scopeIn(capture);
            if (scope && (keepsCaptures || mayBeNoScope))
                m_isRefused.quickSet(scope.index());
        }
    }
    for (const EvalScope& scope : m_program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings) {
            if (binding.layout)
                m_isRefused.quickSet(binding.layout.index());
        }
    }
}

uint32_t PlannerOfPromotedEnvironments::rootOf(uint32_t node)
{
    while (m_parents[node] != node) {
        m_parents[node] = m_parents[m_parents[node]];
        node = m_parents[node];
    }
    return node;
}

void PlannerOfPromotedEnvironments::addSource(uint32_t node, uint32_t source)
{
    uint32_t& known = m_sourcesOfClasses[rootOf(node)];
    if (known == noSource)
        known = source;
    else if (source != noSource && known != source)
        known = noSourceServes;
}

void PlannerOfPromotedEnvironments::unite(uint32_t first, uint32_t second)
{
    first = rootOf(first);
    second = rootOf(second);
    if (first == second)
        return;
    m_parents[second] = first;
    addSource(first, m_sourcesOfClasses[second]);
}

// Captures that are handed on from one function to the next hold one value. The classes are united in one go over the sites that the scan has listed.
bool PlannerOfPromotedEnvironments::followCapturedValues()
{
    const LinkTables& linked = m_facts.linked();
    if (linked.firstCaptureOfFunction.size() != m_program.functions.size() + 1)
        return false;
    uint32_t numberOfCaptures = linked.firstCaptureOfFunction.last();
    m_parents = Vector<uint32_t>(numberOfCaptures, [](size_t node) {
        return static_cast<uint32_t>(node);
    });
    m_sourcesOfClasses.fill(noSource, numberOfCaptures);

    const ThinSummaries& summaries = m_facts.summaries();
    for (uint32_t number = 0; number < summaries.size(); ++number) {
        FunctionID id(number);
        const ThinSummary& summary = summaries.at(id);
        if (summary.sitesThatHandOverCaptures.isEmpty())
            continue;
        const BodyIndex* index = m_facts.bodies().find(id);
        if (!index)
            return false;
        bool serves = servesAsMaker(id, summary, *index);
        if (serves)
            m_sourcesOfValues.fill(noSource, index->numberOfValues());
        BodyBlock entry = index->block(entryBlock);
        uint32_t numberOfOwnCaptures = static_cast<uint32_t>(m_program.at(id).captures.size());
        for (ValueID site : summary.sitesThatHandOverCaptures) {
            BodyValue value = index->value(site);
            bool makesClosure = value.opcode() == Opcode::NewClosure;
            if (!makesClosure && value.opcode() != Opcode::CallDirect) {
                FunctionsWhoseClosuresOperationMakes made { m_functionsWhoseClosuresRunTimeMakes };
                value.forEachReference(made);
                continue;
            }
            FunctionID callee = makesClosure ? value.as<Opcode::NewClosure>().function() : value.as<Opcode::CallDirect>().function();
            std::span<const ValueID> operands = makesClosure ? value.as<Opcode::NewClosure>().capturesOperands() : value.as<Opcode::CallDirect>().argumentsOperands();
            uint32_t numberOfCapturesOfCallee = static_cast<uint32_t>(m_program.at(callee).captures.size());
            for (uint32_t capture = 0; capture < numberOfCapturesOfCallee; ++capture) {
                if (m_program.at(typeOfCapture(callee, capture)).environment)
                    continue;
                uint32_t node = nodeOf(callee, capture);
                ValueID handed = capture < operands.size() && operands[capture] ? index->valueBehindCasts(operands[capture]) : ValueID();
                std::optional<uint32_t> own = handed && index->isParameter(handed) ? entry.indexOfParameter(handed) : std::nullopt;
                if (own && *own < numberOfOwnCaptures) {
                    unite(node, nodeOf(id, *own));
                    continue;
                }
                if (!handed || !serves || index->loopOf(index->blockOf(handed))) {
                    addSource(node, noSourceServes);
                    continue;
                }
                uint32_t& source = m_sourcesOfValues[handed.index()];
                if (source == noSource) {
                    m_sources.append(Source { id, { }, { }, 0, 0 });
                    source = firstSource + static_cast<uint32_t>(m_sources.size()) - 1;
                }
                m_suppliers.append(Supplier { source - firstSource, site, callee, capture });
                addSource(node, source);
            }
        }
    }
    return true;
}

// One store serves where the first site stands in front of every other one. A store stands in no loop: no look at a loop counts on it.
void PlannerOfPromotedEnvironments::settleSources()
{
    AOT::sort(m_suppliers, { }, [](const Supplier& supplier) {
        return std::pair { supplier.source, supplier.site };
    });
    for (uint32_t first = 0; first < m_suppliers.size();) {
        uint32_t end = first;
        while (end < m_suppliers.size() && m_suppliers[end].source == m_suppliers[first].source)
            ++end;
        std::span<const Supplier> suppliers = m_suppliers.span().subspan(first, end - first);
        Source& source = m_sources[suppliers[0].source];
        const BodyIndex& index = *m_facts.bodies().find(source.function);
        BlockID blockOfFirst = index.blockOf(suppliers[0].site);
        TypeID type = typeOfCapture(suppliers[0].callee, suppliers[0].capture);
        Type held = m_program.at(type);
        bool serves = held.isJavaScriptValue() && !(held.primitives & Primitive::Absent);
        bool firstStandsInFront = true;
        for (const Supplier& supplier : suppliers) {
            serves &= typeOfCapture(supplier.callee, supplier.capture) == type;
            firstStandsInFront &= index.dominates(blockOfFirst, index.blockOf(supplier.site));
        }
        source.firstSupplier = first;
        source.numberOfStores = firstStandsInFront ? 1 : static_cast<uint32_t>(suppliers.size());
        for (const Supplier& supplier : suppliers.first(source.numberOfStores))
            serves &= !index.loopOf(index.blockOf(supplier.site));
        if (serves)
            source.type = type;
        first = end;
    }
}

VariableID PlannerOfPromotedEnvironments::addVariable(PromotedVariable&& variable)
{
    m_result.makers.set(variable.maker.index());
    m_result.variables.append(WTF::move(variable));
    return VariableID(m_result.firstVariable.index() + static_cast<uint32_t>(m_result.variables.size()) - 1);
}

void PlannerOfPromotedEnvironments::leaveOutCaptures()
{
    for (uint32_t number = 0; number < m_program.functions.size(); ++number) {
        FunctionID id(number);
        const Function& function = m_program.functions[number];
        if (function.captures.isEmpty() || !capturesMayBeLeftOut(id))
            continue;
        uint32_t numberRemoved = 0;
        for (uint32_t capture = 0; capture < function.captures.size(); ++capture) {
            TypeID type = typeOfCapture(id, capture);
            VariableID variable;
            if (LayoutID scope = m_program.at(type).environment) {
                if (!m_result.isPromoted(scope))
                    continue;
            } else {
                uint32_t found = m_sourcesOfClasses[rootOf(nodeOf(id, capture))];
                if (found < firstSource)
                    continue;
                Source& source = m_sources[found - firstSource];
                if (source.type != type)
                    continue;
                if (!source.variable) {
                    const Supplier& first = m_suppliers[source.firstSupplier];
                    source.variable = addVariable(PromotedVariable { m_program.at(source.function).module, m_program.at(first.callee).captures[first.capture].variableName, { }, source.function, { }, 0, first.callee, first.capture });
                }
                variable = source.variable;
            }
            m_result.removedCaptures.append(RemovedCapture { id, capture, variable });
            ++numberRemoved;
        }
        if (!numberRemoved)
            continue;
        if (m_result.numbersOfRemovedCaptures.isEmpty())
            m_result.numbersOfRemovedCaptures.fill(0, m_program.functions.size());
        m_result.numbersOfRemovedCaptures[number] = numberRemoved;
    }
}

void PlannerOfPromotedEnvironments::listStores()
{
    for (const Source& source : m_sources) {
        if (!source.variable)
            continue;
        for (const Supplier& supplier : m_suppliers.span().subspan(source.firstSupplier, source.numberOfStores))
            m_result.stores.append(StoreOfCapturedValue { source.function, supplier.site, supplier.capture, source.variable });
    }
    AOT::sort(m_result.stores, { }, [](const StoreOfCapturedValue& store) {
        return std::tuple { store.function, store.site, store.capture };
    });
}

// A maker fills words, and Link's effects do not list that: a call that leads to one is taken to do anything. Each function on the way runs at most once, so one call enters it.
void PlannerOfPromotedEnvironments::findFunctionsThatLeadToMakers()
{
    m_result.functionsThatLeadToMakers = m_result.makers;
    if (m_result.makers.isEmpty())
        return;
    Vector<FunctionID> callers(m_program.functions.size());
    for (const LinkedCall& call : m_facts.linked().calls) {
        if (m_facts.runsAtMostOnce(call.callee))
            callers[call.callee.index()] = call.caller;
    }
    for (size_t maker : m_result.makers) {
        for (FunctionID caller = callers[maker]; caller && !m_result.functionsThatLeadToMakers.set(caller.index()); caller = callers[caller.index()]) { }
    }
}

PromotedEnvironments PlannerOfPromotedEnvironments::run()
{
    if (!followCapturedValues())
        return { };
    settleSources();

    m_result.firstVariable = VariableID(static_cast<uint32_t>(m_program.variables.size()));
    findScopesMadeOnce();
    if (!m_scopes.isEmpty())
        refuseScopesThatPositionsHold();
    AOT::sort(m_scopes, { }, &ScopeMadeOnce::layout);
    for (const ScopeMadeOnce& scope : m_scopes) {
        if (m_isRefused.quickGet(scope.layout.index()))
            continue;
        const Layout& layout = m_program.at(scope.layout);
        m_result.layouts.append(PromotedLayout { scope.layout, VariableID(m_result.firstVariable.index() + static_cast<uint32_t>(m_result.variables.size())) });
        m_result.makers.set(scope.maker.index());
        for (uint32_t field = 0; field < layout.fields.size(); ++field)
            addVariable(PromotedVariable { m_program.at(scope.maker).module, layout.fields[field].variableName, { }, scope.maker, scope.layout, field, { }, 0 });
    }
    leaveOutCaptures();
    listStores();
    findFunctionsThatLeadToMakers();
    return WTF::move(m_result);
}

} // anonymous namespace

PromotedEnvironments planPromotedEnvironments(const Program& program, const Facts& facts, const SlotPlanParts& slots)
{
    UNUSED_PARAM(program);
    UNUSED_PARAM(facts);
    UNUSED_PARAM(slots);
    return { };
}

void appendPromotedVariables(const PromotedEnvironments& promoted, SlotPlanParts& slots)
{
    for (const PromotedVariable& variable : promoted.variables) {
        PlannedVariable planned;
        if (variable.layout)
            planned.representation = slots.layouts[variable.layout.index()].slots[variable.field].representation;
        SlotTraits traits = traitsOf(planned.representation);
        if (traits.hasAbsent)
            planned.initialWord = traits.absentBits;
        slots.variables.append(planned);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
