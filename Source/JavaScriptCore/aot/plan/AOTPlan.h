/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArgumentLocations.h"
#include "AOTBody.h"
#include "AOTBodyIndex.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTCallingConventions.h"
#include "AOTCodeRole.h"
#include "AOTEffects.h"
#include "AOTFacts.h"
#include "AOTFunctionKinds.h"
#include "AOTID.h"
#include "AOTKeyOrders.h"
#include "AOTMemoryLocation.h"
#include "AOTModuleEvaluationPlan.h"
#include "AOTPrimitives.h"
#include "AOTPromotedEnvironments.h"
#include "AOTShapeTest.h"
#include "AOTSlotPlan.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include <memory>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class Program;

struct DeferredVariable {
    VariableID variable;
    uint16_t mark { Primitive::Absent };
    bool isMarkedAtDeclaration { false };
    FunctionID body;
    ValueID made;
    Opcode opcode { Opcode::Data };
};

struct DeferredDefinitions {
    const DeferredVariable* findDeferred(VariableID id) const LIFETIME_BOUND
    {
        if (!isDeferred.get(id.index()))
            return nullptr;
        auto found = std::ranges::lower_bound(variables, id, { }, &DeferredVariable::variable);
        RELEASE_ASSERT(found != variables.end() && found->variable == id);
        return &*found;
    }
    const DeferredVariable* findMadeBy(FunctionID body, ValueID made) const LIFETIME_BOUND
    {
        if (!bodiesThatDefine.get(body.index()))
            return nullptr;
        auto found = std::ranges::lower_bound(variablesByMaker, std::pair { body, made }, { }, [&](uint32_t place) {
            return std::pair { variables[place].body, variables[place].made };
        });
        return found != variablesByMaker.end() && variables[*found].body == body && variables[*found].made == made ? &variables[*found] : nullptr;
    }

    Vector<DeferredVariable> variables;
    Vector<uint32_t> variablesByMaker;
    BitVector isDeferred;
    BitVector bodiesThatDefine;
};

struct GlobalNameUse {
    bool isStoredByName { false };
    bool isStoredThroughBinding { false };
};

struct PlanParts {
    DeferredDefinitions deferredDefinitions;
    PromotedEnvironments promotedEnvironments;
    ModuleEvaluationPlan moduleEvaluation;
    SlotPlan slots;
    ShapeTable shapes;
    KeyOrders keyOrders;
    CallingConventions conventions;

    BitVector modulesWithValue;
    BitVector modulesWithRequireCacheValue;
    BitVector modulesWithNamespaceWord;
    BitVector modulesWhoseImportsAreNeverMocked;
    BitVector modulesWhoseBodyIsPassedOver;

    LayoutID closureLayout;
    BitVector layoutsWithClassRecord;
    BitVector functionsWithClosureCell;
    BitVector functionsWhoseResultsPassNoReturn;
    BitVector variablesThatRunTimeMayFill;
    BitVector functionsWhoseCallEntryWalksBody;
    BitVector globalNamesNeverStoredByName;
    BitVector namesOfFieldsWrittenAsOneFunction;
    BitVector globalNamesNeverStoredThroughBinding;

    bool hasOneInstance { false };
    FunctionsThatEndTheirTailCalls functionsThatEndTheirTailCalls;

    bool usesWideReferences { false };
};

class Plan {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(Plan);
    WTF_MAKE_NONCOPYABLE(Plan);
public:
    explicit Plan(PlanParts&& parts)
        : m_parts(WTF::move(parts))
    {
    }

    const DeferredDefinitions& deferredDefinitions() const { return m_parts.deferredDefinitions; }
    const PromotedEnvironments& promotedEnvironments() const { return m_parts.promotedEnvironments; }
    const ModuleEvaluationPlan& moduleEvaluation() const { return m_parts.moduleEvaluation; }

    const SlotPlan& slots() const { return m_parts.slots; }
    const PlannedLayout& layout(LayoutID id) const { return slots().layout(id); }
    const PlacedSlot& slot(LayoutID id, FieldIndex field) const { return slots().slot(id, field); }
    bool isInUse(LayoutID id) const { return slots().hasDirectInstances(id); }
    bool isAllocatable(LayoutID id) const { return !slots().isNeverAllocated(id); }
    bool usesWideReferences() const { return m_parts.usesWideReferences; }

    const ShapeTable& shapes() const { return m_parts.shapes; }
    const KeyOrders& keyOrders() const { return m_parts.keyOrders; }
    const CallingConventions& conventions() const { return m_parts.conventions; }
    void setMethodTable(MethodTablePlan&& methodTable) { m_parts.conventions.setMethodTable(WTF::move(methodTable)); }
    void setParameterGuards(std::span<const ParameterGuardPlan> guards) { m_parts.conventions.setParameterGuards(guards); }
    void setFunctionsThatEndTheirTailCalls(FunctionsThatEndTheirTailCalls&& functions) { m_parts.functionsThatEndTheirTailCalls = WTF::move(functions); }
    void setTailCalls(TailCallPlan&& tailCalls) { m_parts.conventions.setTailCalls(WTF::move(tailCalls)); }
    void setStackTests(StackTestPlan&& stackTests) { m_parts.conventions.setStackTests(WTF::move(stackTests)); }
    void fuseCallEntriesThatOnlyMove(const Program& program, const Facts& facts, const ParallelLoops& loops) { m_parts.conventions.fuseCallEntriesThatOnlyMove(program, facts, loops); }

    FieldAccess fieldAccess(const Program& program, PropertyKeyID key, LayoutSetID set, std::span<const LayoutID> members, bool isStore) const { return slots().fieldAccess(program, key, set, members, isStore); }

    JS_EXPORT_PRIVATE std::optional<PlacedAllocation> allocation(const Program&, LayoutID, std::span<const FieldIndex> initialFields) const;
    JS_EXPORT_PRIVATE PlacedAllocation environmentAllocation(LayoutID) const;
    JS_EXPORT_PRIVATE PlacedAllocation closureAllocation(unsigned numberOfCaptures) const;

    PlannedVariable variable(VariableID id) const { return slots().variable(id); }

    bool hasModuleValue(ModuleID id) const { return m_parts.modulesWithValue.get(id.index()); }
    bool hasRequireCacheValue(ModuleID id) const { return m_parts.modulesWithRequireCacheValue.get(id.index()); }
    bool hasModuleNamespaceWord(ModuleID id) const { return m_parts.modulesWithNamespaceWord.get(id.index()); }
    bool importsWhatMayBeMocked(ModuleID id) const { return !m_parts.modulesWhoseImportsAreNeverMocked.get(id.index()); }
    bool moduleBodyIsEntered(ModuleID id) const { return !m_parts.modulesWhoseBodyIsPassedOver.get(id.index()); }

    bool hasClassRecord(LayoutID id) const { return m_parts.layoutsWithClassRecord.get(id.index()); }
    const BitVector& layoutsWithClassRecord() const { return m_parts.layoutsWithClassRecord; }
    bool classGuardsHoldWhileClear(LayoutID classLayout) const { return slots().classCreation(classLayout).guardsHoldWhileClear; }
    bool bitsOfClassGuardsAreClearWhileTheyHold(LayoutID classLayout, ClassSide side) const
    {
        const ClassCreationPlan& creation = slots().classCreation(classLayout);
        return side == ClassSide::Prototype ? creation.guardsOfPrototypeSideHoldWhileClear : creation.guardsHoldWhileClear;
    }

    FunctionKindData functionKind(const Program& program, CodeUnit closureEntry) const { return functionKindOf(program, slots(), conventions(), closureEntry); }
    bool hasClosureCell(FunctionID id) const { return m_parts.functionsWithClosureCell.get(id.index()); }
    bool resultsPassNoReturn(FunctionID id) const { return m_parts.functionsWhoseResultsPassNoReturn.get(id.index()); }
    TypeID typeHeldOfResult(const Program& program, const LinkTables& link, FunctionID id) const
    {
        if (id.index() >= link.functions.size() || program.at(id).isCoroutine())
            return { };
        if (resultsPassNoReturn(id) && conventions().resultRepresentation({ id, CodeRole::Body }) == Representation::JSValue)
            return { };
        return link.functions[id.index()].result;
    }
    TypeID typeHeldOfResult(const Program& program, const LinkTables& link, CodeUnit unit) const
    {
        if (returnsResultOfFunction(unit.role))
            return typeHeldOfResult(program, link, unit.function);
        if (unit.role != CodeRole::CoroutineBody || !conventions().of(unit.function).coroutineNeverSuspends)
            return { };
        const LinkedCoroutine* coroutine = link.findCoroutine(unit.function);
        return coroutine && coroutine->returned && !isNeverType(coroutine->returned) ? coroutine->returned : TypeID();
    }
    bool runTimeMayFill(VariableID id) const { return m_parts.variablesThatRunTimeMayFill.get(id.index()); }
    bool genericCallEntryWalksBody(FunctionID id) const { return m_parts.functionsWhoseCallEntryWalksBody.get(id.index()); }
    void findCallEntriesThatWalkBodies(const Program&, const Facts&);

    bool someFieldUnderNameIsWrittenAsOneFunction(PropertyKeyID key) const { return m_parts.namesOfFieldsWrittenAsOneFunction.get(key.index()); }
    GlobalNameUse globalName(PropertyKeyID key) const { return { !m_parts.globalNamesNeverStoredByName.get(key.index()), !m_parts.globalNamesNeverStoredThroughBinding.get(key.index()) }; }

    bool hasOneInstance() const { return m_parts.hasOneInstance; }
    bool endsItsTailCalls(FunctionID id) const { return m_parts.functionsThatEndTheirTailCalls.whateverArrives.get(id.index()); }
    uint64_t entryParametersThatMustEndTheirTailCalls(FunctionID id) const { return m_parts.functionsThatEndTheirTailCalls.entryParametersThatMustEndTheirs(id); }

private:
    PlanParts m_parts;
};

JS_EXPORT_PRIVATE std::unique_ptr<Plan> planProgram(Program&, const Facts&, BuildContext&);

JS_EXPORT_PRIVATE void checkPlan(const Program&, const Facts&, const Plan&, BuildContext&);

JS_EXPORT_PRIVATE Vector<CodeUnit> unitsNamedOutsideCode(const Program&, const Facts&, const Plan&);


struct ParentInClassRecord {
    LayoutID layout;
    bool isProven { false };
};
ParentInClassRecord parentNamedByClassRecord(const Program&, const Facts&, LayoutID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
