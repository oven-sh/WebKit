/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

// Nothing below needs AOTBody.h, AOTBodyIndex.h, AOTEffects.h, AOTMemoryLocation.h, AOTPrimitives.h and AOTThreads.h. Other files have always got them through this header.
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
#include "AOTLiteralStrings.h"
#include "AOTMemoryLocation.h"
#include "AOTModuleEvaluationPlan.h"
#include "AOTPrimitives.h"
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

// WHAT THE BODY OF A MODULE MAKES OUT OF THE TEXT OF THE PROGRAM ALONE AND ONLY STORES IN A VARIABLE OF THE MODULE IS MADE AT THE FIRST READ OF THE VARIABLE: ThinSummary::definitions.
// Nobody can tell: making it runs no code and reads nothing that changes, the one store is the only writer of the variable, so every read finds the one object, and whoever reads in front of the declaration finds what he found before.
// The run time takes what a variable holds for the value of the binding, so a variable that a record hands to it is left alone.
struct DeferredVariable {
    VariableID variable;
    // One primitive, which says that the object is not made yet. The object is no primitive.
    // `absent`: the variable is born so and no read comes in front of the store. Nothing stands where the store stood.
    // Otherwise a read in front of the store finds what the variable is born as, as ever, and where the store stood the mark is stored: undefined in a variable that is born `absent`, null in one that is born undefined.
    uint16_t mark { Primitive::Absent };
    bool isMarkedAtDeclaration { false };
    // The operation as it is written: a NewRegExp or a Data. A read that finds the mark does it, stores what it gives and goes on with that.
    FunctionID body;
    ValueID made;
    Opcode opcode { Opcode::Data };
};

struct DeferredDefinitions {
    // Null: the variable holds what the program stores in it.
    const DeferredVariable* findDeferred(VariableID id) const LIFETIME_BOUND
    {
        if (!isDeferred.get(id.index()))
            return nullptr;
        auto found = std::ranges::lower_bound(variables, id, { }, &DeferredVariable::variable);
        RELEASE_ASSERT(found != variables.end() && found->variable == id);
        return &*found;
    }
    // Of an operation of a body: the variable that it is made for. The body does not do it.
    const DeferredVariable* findMadeBy(FunctionID body, ValueID made) const LIFETIME_BOUND
    {
        if (!bodiesThatDefine.get(body.index()))
            return nullptr;
        auto found = std::ranges::lower_bound(variablesByMaker, std::pair { body, made }, { }, [&](uint32_t place) {
            return std::pair { variables[place].body, variables[place].made };
        });
        return found != variablesByMaker.end() && variables[*found].body == body && variables[*found].made == made ? &variables[*found] : nullptr;
    }

    // Ascending by variable.
    Vector<DeferredVariable> variables;
    // Places in `variables`, ascending by body and operation.
    Vector<uint32_t> variablesByMaker;
    BitVector isDeferred;
    BitVector bodiesThatDefine;
};

// A variable that stands for a field of an environment that is never made, or for a capture that no closure holds.
struct PromotedVariable {
    ModuleID module;
    StringID name;
    LocationFacts location;
};

struct PromotedEnvironments {
    uint32_t numberOfRemovedCapturesOf(FunctionID function) const { return function.index() < numbersOfRemovedCaptures.size() ? numbersOfRemovedCaptures[function.index()] : 0; }
    // Null for every other variable.
    const PromotedVariable* find(VariableID id) const LIFETIME_BOUND
    {
        if (!id || !firstVariable || id.index() < firstVariable.index() || id.index() - firstVariable.index() >= variables.size())
            return nullptr;
        return &variables[id.index() - firstVariable.index()];
    }

    VariableID firstVariable;
    Vector<PromotedVariable> variables;
    // By function.
    Vector<uint32_t> numbersOfRemovedCaptures;
};

struct GlobalNameUse {
    bool isStoredByName { false };
    // A read looks at the location alone.
    bool isStoredThroughBinding { false };
};

// A set that is empty claims nothing, but for the four that say what exists: modulesWithValue, modulesWithRequireCacheValue, layoutsWithClassRecord, functionsWithClosureCell.
struct PlanParts {
    DeferredDefinitions deferredDefinitions;
    // Milestone one: empty. Nothing is promoted, and no capture is removed.
    PromotedEnvironments promotedEnvironments;
    ModuleEvaluationPlan moduleEvaluation;
    // Milestone one: see SlotPlanParts.
    SlotPlan slots;
    // Milestone one: see ShapeTableParts.
    ShapeTable shapes;
    // Milestone one: see KeyOrderParts.
    KeyOrders keyOrders;
    // Milestone one: every parameter and result boxed, in the standard locations.
    CallingConventions conventions;
    // Milestone one: the empty string alone.
    LiteralStringTable literalStrings;

    // Which words of the statics a module has. Milestone one: every module but a CommonJs one.
    BitVector modulesWithValue;
    // The word follows the module's value. Milestone one: of those, the targets of a require() as written for which requireReadsModuleValue() and requireAddsToRequireCache() hold.
    BitVector modulesWithRequireCacheValue;
    // The modules of the run time. Of any other module ModuleNamespace reads the value word or asks the run time.
    BitVector modulesWithNamespaceWord;
    // By statements, through any number of modules. Of any other module EvaluateRequiredModule asks the run time although the module has run. None in a test run.
    BitVector modulesWhoseImportsAreNeverMocked;
    // bodyOfModuleOnlyReturns(), and the modules without a body.
    BitVector modulesWhoseBodyIsPassedOver;

    // Milestone one: the one layout of the kind Closure.
    LayoutID closureLayout;
    // Milestone one: every layout of the kind Instance.
    BitVector layoutsWithClassRecord;
    // The closure table may have a cell for the function. Image numbers only the cells that code or a table mentions. It says nothing of how many closures there are.
    // Milestone one: of the functions with a GenericCallEntry, those without captures, and every Layout::constructor whatever it captures. ClosureTable::registerClassObject() stops at a constructor without a cell.
    BitVector functionsWithClosureCell;
    // TestingSwitch::CheckProofsAtRunTime alone fills it. What a call of such a function gives may have come through no return of a body of the program: the function ends in a call of something that nobody lists,
    // or in a call of such a function. So what Link has proven of its result is held against the value where the call stands, and of every other function where it returns.
    BitVector functionsWhoseResultsPassNoReturn;
    // TestingSwitch::CheckProofsAtRunTime alone fills it. The run time stores into such a variable with no operation of compiled code: ProgramStatics::writeVariable(). So what Link has proven of it is held against what a read finds.
    BitVector variablesThatRunTimeMayFill;
    // The names that no body stores on the global object as it is written.
    BitVector globalNamesNeverStoredByName;
    BitVector globalNamesNeverStoredThroughBinding;

    // What Facts says, where emitted code takes a form by the answer whichever operation it is emitted for. 
    bool hasOneInstance { false };
    BitVector functionsThatEndTheirTailCalls;

    // RecordedOptions::useWideReferences().
    bool usesWideReferences { false };
};

// Everything that changes the shape of code and needs the whole program, and the answer to every question whether a thing exists.
// It is made once, in front of Compile. From then on it is read by any thread and never changes.
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
    bool isInUse(LayoutID id) const { return !slots().isNeverAllocated(id); }
    bool usesWideReferences() const { return m_parts.usesWideReferences; }

    const ShapeTable& shapes() const { return m_parts.shapes; }
    const KeyOrders& keyOrders() const { return m_parts.keyOrders; }
    const CallingConventions& conventions() const { return m_parts.conventions; }
    void setMethodTable(MethodTablePlan&& methodTable) { m_parts.conventions.setMethodTable(WTF::move(methodTable)); }
    void setParameterGuards(std::span<const ParameterGuardPlan> guards) { m_parts.conventions.setParameterGuards(guards); }
    void setFunctionsThatEndTheirTailCalls(BitVector&& functions) { m_parts.functionsThatEndTheirTailCalls = WTF::move(functions); }
    void setTailCalls(TailCallPlan&& tailCalls) { m_parts.conventions.setTailCalls(WTF::move(tailCalls)); }
    void setStackTests(StackTestPlan&& stackTests) { m_parts.conventions.setStackTests(WTF::move(stackTests)); }
    void fuseCallEntriesThatOnlyMove(const Program& program, const ParallelLoops& loops) { m_parts.conventions.fuseCallEntriesThatOnlyMove(program, loops); }
    const LiteralStringTable& literalStrings() const { return m_parts.literalStrings; }

    FieldAccess fieldAccess(const Program& program, PropertyKeyID key, std::span<const LayoutID> receivers, bool isStore) const { return slots().fieldAccess(program, key, receivers, isStore); }
    FieldAccess fieldAccess(const Program& program, PropertyKeyID key, LayoutSetID set, std::span<const LayoutID> members, bool isStore) const { return slots().fieldAccess(program, key, set, members, isStore); }

    // The fields in the order in which the object gets them, with or without storage.
    // None: the layout holds no compiled objects or has no shape, or some field with storage is listed and KeyOrders::findOrderWord() has no word for them.
    // With no such field the word is unorderedOrderWord, whether or not the layout has an order.
    JS_EXPORT_PRIVATE std::optional<PlacedAllocation> allocation(const Program&, LayoutID, AllocationClassID, std::span<const FieldIndex> initialFields) const;
    JS_EXPORT_PRIVATE PlacedAllocation environmentAllocation(LayoutID) const;
    // A closure has room for an odd number of captures.
    JS_EXPORT_PRIVATE PlacedAllocation closureAllocation(unsigned numberOfCaptures) const;

    PlannedVariable variable(VariableID id) const { return slots().variable(id); }

    bool hasModuleValue(ModuleID id) const { return m_parts.modulesWithValue.get(id.index()); }
    bool hasRequireCacheValue(ModuleID id) const { return m_parts.modulesWithRequireCacheValue.get(id.index()); }
    bool hasModuleNamespaceWord(ModuleID id) const { return m_parts.modulesWithNamespaceWord.get(id.index()); }
    bool importsWhatMayBeMocked(ModuleID id) const { return !m_parts.modulesWhoseImportsAreNeverMocked.get(id.index()); }
    bool moduleBodyIsEntered(ModuleID id) const { return !m_parts.modulesWhoseBodyIsPassedOver.get(id.index()); }

    bool hasClassRecord(LayoutID id) const { return m_parts.layoutsWithClassRecord.get(id.index()); }
    const BitVector& layoutsWithClassRecord() const { return m_parts.layoutsWithClassRecord; }
    // AOTClassGuards.h: the bit of either side is clear while the guard holds.
    bool classGuardsHoldWhileClear(LayoutID classLayout) const { return slots().classCreation(classLayout).guardsHoldWhileClear; }
    bool bitsOfClassGuardsAreClearWhileTheyHold(LayoutID classLayout, ClassSide side) const
    {
        const ClassCreationPlan& creation = slots().classCreation(classLayout);
        return side == ClassSide::Prototype ? creation.guardsOfPrototypeSideHoldWhileClear : creation.guardsHoldWhileClear;
    }

    FunctionKindData functionKind(const Program& program, CodeUnit closureEntry) const { return functionKindOf(program, slots(), conventions(), closureEntry); }
    bool hasClosureCell(FunctionID id) const { return m_parts.functionsWithClosureCell.get(id.index()); }
    bool resultsPassNoReturn(FunctionID id) const { return m_parts.functionsWhoseResultsPassNoReturn.get(id.index()); }
    bool runTimeMayFill(VariableID id) const { return m_parts.variablesThatRunTimeMayFill.get(id.index()); }

    GlobalNameUse globalName(PropertyKeyID key) const { return { !m_parts.globalNamesNeverStoredByName.get(key.index()), !m_parts.globalNamesNeverStoredThroughBinding.get(key.index()) }; }

    // The stubs and the tables of the image go by Facts::hasOneInstance() too.
    bool hasOneInstance() const { return m_parts.hasOneInstance; }
    bool endsItsTailCalls(FunctionID id) const { return m_parts.functionsThatEndTheirTailCalls.get(id.index()); }

private:
    PlanParts m_parts;
};

// Null where a step has reported an error. Of the program only the table of types may grow. The driver freezes that table behind this call.
JS_EXPORT_PRIVATE std::unique_ptr<Plan> planProgram(Program&, const Facts&, BuildContext&);

// In every build, behind planProgram(). Reports InternalErrorKind::PlacementCheckFailed for each rule that does not hold.
// Milestone one: the slots of a layout lie apart and inside its cell, a layout in use that holds compiled objects or is an environment has a shape and storage for every field,
// the shapes of objects lie below firstEnvironmentShape and all others from it on, an order lists fields with storage, and the method table has the units of its selectors and no other.
JS_EXPORT_PRIVATE void checkPlan(const Program&, const Facts&, const Plan&, BuildContext&);

// The units that something other than compiled code names: the host, the tables of the image, the run time. Ascending, each once. The queue of Compile starts with them.
JS_EXPORT_PRIVATE Vector<CodeUnit> unitsNamedOutsideCode(const Program&, const Facts&, const Plan&);

// The built-in constructor above a class, where a class record can name it: it is found at any time.
BuiltinID builtinParentNamedByClassRecord(const Program&, const Facts&, LayoutID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
