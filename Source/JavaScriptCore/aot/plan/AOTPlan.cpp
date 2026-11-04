/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlan.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTCellFormat.h"
#include "AOTClosure.h"
#include "AOTFieldsStorage.h"
#include "AOTClosuresOfLiterals.h"
#include "AOTIRProgram.h"
#include "AOTKeyOrdersOfLayouts.h"
#include "AOTMechanisms.h"
#include "AOTNameClass.h"
#include "AOTNumberMethodSelectors.h"
#include "AOTPhaseTimes.h"
#include "AOTRowsOfLayouts.h"
#include "AOTSlotRepresentation.h"
#include "AOTSort.h"
#include "AOTTailCallPlan.h"
#include "AOTUnionPlan.h"
#include "AOTThinSummary.h"
#include "AOTThreads.h"
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(!(AOT::Closure::allocationSize(1) % 16));
static_assert(AOT::Closure::maxNumberOfCaptures % 2);
static_assert(unorderedOrderWord == orderWordOf(0, 0));
// FunctionAsZero tells a word that was never stored from every value that a store leaves there.
static_assert(representsAbsentAsZero(SlotRepresentation::JSValue) && traitsOf(SlotRepresentation::JSValue).undefinedBits);

// What the other files of plan/ make for planProgram(). No header declares them.
// AOTFunctionKinds.cpp.
Vector<ClosureForm> chooseClosureForms(const Program&, const LinkTables&);
// AOTSlotPlan.cpp: every member but `variables`, `classCreation` and `closureForms`. It reports what the records of the image cannot say.
SlotPlanParts computeSlotPlanParts(Program&, const Facts&, BuildContext&);
// AOTShapeTest.cpp.
ShapeTable computeShapeTable(const Program&, const LinkTables&, const SlotPlan&, LayoutID closureLayout, std::span<const uint32_t> orderOfLayouts);
// AOTKeyOrders.cpp.
KeyOrders computeKeyOrders(const Program&, const LinkTables&, const ThinSummaries&, const BodyIndexTable&, Mechanisms, const SlotPlan&, BuildContext&);
// AOTLiteralStrings.cpp.
LiteralStringTable computeLiteralStrings(const Program&);
// walk/AOTWalkGuesses.cpp: typesBehindParameterGuard() of each function. Ascending by function.
Vector<ParameterGuardPlan> planParameterGuards(Program&, const Facts&, const Plan&, BuildContext&);
// AOTUnionMethodSelectors.cpp: MethodFamily::receivers and typesOfArguments, and the typed selectors.
void finishMethodTableForUnions(Program&, const Facts&, Plan&);

// A family for each name that some call by name spells and under which objects of the program's classes find methods, as the classes are written.
// A row stands on the guard of the name at the class of its layout (object/AOTClassGuards.h) unless Link has proven that nothing under the name can change. An object that was laid below another prototype says so itself: the entry asks it.
static MethodTablePlan planMethodTable(const Program& program, const Facts& facts, const Plan& plan)
{
    MethodTablePlan table;
    if (!facts.mechanisms().contains(Mechanism::DirectCalls))
        return table;
    const LinkTables& linked = facts.linked();
    const CallingConventions& conventions = plan.conventions();

    // By key: a bit for each number of arguments that a call passes.
    Vector<uint8_t> numbersOfArgumentsPassedUnderKey;
    numbersOfArgumentsPassedUnderKey.fill(0, program.types.numberOfPropertyKeys());
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (!facts.mayBeReached(FunctionID(index)))
            continue;
        for (const SummaryCall& call : facts.summaries().at(FunctionID(index)).calls) {
            if (call.callee == SummaryCall::Callee::Name && call.key && !call.isMadeByProtocol && !call.isConstruction && !call.passesList && call.numberOfArguments <= maxArgumentsOfTableCall)
                numbersOfArgumentsPassedUnderKey[call.key.index()] |= 1 << call.numberOfArguments;
        }
    }

    auto isEnteredThroughTable = [&](FunctionID method) {
        const Function& function = program.at(method);
        FunctionConvention convention = conventions.of(method);
        // Only the calls that Link lists enter such a method, and each of them goes straight to it. A call through the table mentions every method of the family, and nothing leads from it to this one: LinkedFunction::isInQueueFromStart.
        if (linked.function(method).everyCallerIsKnown && linked.function(method).everyCallHasOneCallee)
            return false;
        return function.kind == FunctionKind::Method && function.captures.isEmpty() && !function.has(FunctionFlags::TakesCallee) && !function.has(FunctionFlags::HasNewTarget)
            && convention.has(CodeRole::Body) && !convention.takesArgumentsInBuffer && conventions.numberOfResults(conventions.directEntryOf(method)) == 1;
    };
    auto sideOf = [](const LinkedMember& member) {
        return std::pair { member.classLayout, member.side };
    };

    struct RowUnderKey {
        PropertyKeyID key;
        MethodTableRow row;
    };
    Vector<RowUnderKey> rows;
    // By key: the last layout on whose way up the key was met.
    Vector<uint32_t> lastLayoutThatPassedKey;
    lastLayoutThatPassedKey.fill(UINT32_MAX, program.types.numberOfPropertyKeys());
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        LayoutID layout(index);
        const Layout& record = program.layouts[index];
        if (record.kind != LayoutKind::Instance || record.has(LayoutFlags::IsConstructorFunction) || !isFoundByShapeTest(record) || !plan.isInUse(layout) || !plan.hasClassRecord(layout))
            continue;
        unsigned numberOfClassesPassed = 0;
        for (LayoutID current = layout; current; current = program.at(current).base) {
            // A row of classes that comes round, a constructor function, a member under a key that is computed: nobody says what is found from here on.
            if (++numberOfClassesPassed > program.layouts.size() || program.at(current).has(LayoutFlags::IsConstructorFunction))
                break;
            auto members = std::ranges::equal_range(linked.members, std::pair { current, ClassSide::Prototype }, { }, sideOf);
            if (std::ranges::any_of(members, [](const LinkedMember& member) { return !member.key; }))
                break;
            for (const LinkedMember& member : members) {
                if (std::exchange(lastLayoutThatPassedKey[member.key.index()], index) == index)
                    continue;
                if (!numbersOfArgumentsPassedUnderKey[member.key.index()] || member.kind != MethodKind::Method || !member.written || !isEnteredThroughTable(member.written))
                    continue;
                bool isOnlyMemberUnderKey = linked.membersUnder(current, ClassSide::Prototype, member.key).size() == 1;
                if (!isOnlyMemberUnderKey)
                    continue;
                if (program.at(member.key).kind != PropertyKeyKind::Text || findField(program, layout, member.key))
                    continue;
                bool isProven = !linked.layout(layout).possibleChanges.containsAny({ LayoutChange::ChangesPrototype, LayoutChange::GetsAccessor })
                    && linked.findMember(program, layout, ClassSide::Prototype, member.key).method == member.written;
                rows.append({ member.key, MethodTableRow { layout, member.written, isProven ? LayoutID() : layout } });
            }
        }
    }
    std::ranges::stable_sort(rows, { }, &RowUnderKey::key);

    for (size_t first = 0; first < rows.size();) {
        MethodFamily family;
        family.key = rows[first].key;
        size_t end = first;
        for (; end < rows.size() && rows[end].key == family.key; ++end) {
            family.rows.append(rows[end].row);
            family.methods.append(rows[end].row.method);
            // An entry goes on to the lookup for an object that stands below another prototype than its class made.
            family.hasGuardedRow = true;
        }
        family.hasGuardedRow = !formsOfSelector(program, linked, conventions, family).everyRowIsProven;
        first = end;
        AOT::sort(family.methods);
        removeRepeatedElements(family.methods);
        uint32_t placeOfFamily = static_cast<uint32_t>(table.families.size());
        for (unsigned numberOfArguments = 0; numberOfArguments <= maxArgumentsOfTableCall; ++numberOfArguments) {
            if (!(numbersOfArgumentsPassedUnderKey[family.key.index()] & 1 << numberOfArguments))
                continue;
            table.selectors.append(MethodSelector { placeOfFamily, numberOfArguments, table.numberOfUnits });
            table.numberOfUnits += 1 + static_cast<uint32_t>(family.methods.size());
        }
        table.families.append(WTF::move(family));
    }
    return table;
}

// ---- Variables

// What a table of the image hands to the run time: the exports of modules, the bindings that evaluated text finds, mocks, and static fields, which lie in their variables until the class object is made.
// Any other variable has a word where code mentions it.
static BitVector variablesDescribedToRunTimeByPlan(const Program& program)
{
    BitVector described(program.variables.size());
    auto add = [&](VariableID variable) {
        if (variable)
            described.quickSet(variable.index());
    };
    for (const Module& module : program.modules) {
        for (const ModuleExport& entry : module.exports)
            add(entry.variable);
        for (const SyntheticExport& entry : module.syntheticExports)
            add(entry.variable);
        add(module.exportsVariable);
    }
    for (const EvalScope& scope : program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings)
            add(binding.variable);
    }
    for (uint32_t index = 0; index < program.variables.size(); ++index) {
        if (program.variables[index].flags.contains(VariableFlags::HoldsMock) || program.variables[index].owner)
            described.quickSet(index);
    }
    return described;
}

// The body is one that the loader enters once: the scan has asked all of that but whether several modules name it.
// `checksProofs`, TestingSwitch::CheckProofsAtRunTime: that no read comes in front of the declaration is Link's word, and a variable that is marked from birth has no state left that could show a read which does.
// So each variable is marked where it is declared, and what a read in front of that finds is held against the proof like what any read finds: Walker::checkProof().
static DeferredDefinitions deferredDefinitionsOfPlan(const Program& program, const Facts& facts, const BitVector& variablesDescribedToRunTime, bool checksProofs)
{
    DeferredDefinitions result;
    if (!withoutMechanismsMissingDependencies(facts.mechanisms()).contains(Mechanism::Deferral))
        return result;

    const LinkTables& linked = facts.linked();
    BitVector bodiesSeen(program.functions.size());
    BitVector bodiesOfSeveralModules(program.functions.size());
    for (const Module& module : program.modules) {
        if (module.body && bodiesSeen.quickSet(module.body.index()))
            bodiesOfSeveralModules.quickSet(module.body.index());
    }

    for (size_t number : bodiesSeen) {
        FunctionID body(static_cast<uint32_t>(number));
        if (bodiesOfSeveralModules.quickGet(number))
            continue;
        const BodyIndex* index = nullptr;
        for (const SummaryDefinition& definition : facts.summaries().at(body).definitions) {
            const Variable& variable = program.at(definition.variable);
            if (definition.variable.index() >= linked.variables.size() || variablesDescribedToRunTime.quickGet(definition.variable.index()))
                continue;
            // A binding that cannot be assigned to is stored into once, where its module declares it.
            const LinkedCell& cell = linked.variable(definition.variable);
            if (!cell.everyWriterIsKnown || cell.isWrittenAfterConstruction || !cell.proven || variable.owner || variable.initialization == VariableInitialization::Function
                || variable.flags.containsAny({ VariableFlags::HoldsMock, VariableFlags::IsReplaceable }))
                continue;
            if (!index)
                index = facts.bodies().find(body);
            if (!index)
                break;
            BodyValue made = index->value(definition.made);
            // Most reads of a closure only call it and take nothing of the object. A test and a rare block at each of them would cost more than the closure.
            if (made.opcode() == Opcode::NewClosure)
                continue;

            // The image fills the first word of a variable only where somebody may read it, so only the empty word, which is what the statics are made of, is a mark from birth.
            bool isBornAbsent = variable.initialization == VariableInitialization::Uninitialized;
            bool isMarkedFromBirth = isBornAbsent && !checksProofs && (cell.isFilledAtEveryRead || !(program.at(cell.proven).primitives & Primitive::Absent));
            uint16_t mark = isMarkedFromBirth ? Primitive::Absent : isBornAbsent ? Primitive::Undefined : Primitive::Null;
            // A read is typed within what is established of the variable.
            if (std::optional<ProvenInEveryVersion> established = variable.declared.established(); established && !(program.at(*established).primitives & mark))
                continue;
            result.variables.append(DeferredVariable { definition.variable, mark, !isMarkedFromBirth, body, definition.made, made.opcode() });
        }
    }

    std::ranges::sort(result.variables, { }, &DeferredVariable::variable);
    result.isDeferred.ensureSize(program.variables.size());
    result.bodiesThatDefine.ensureSize(program.functions.size());
    for (uint32_t place = 0; place < result.variables.size(); ++place) {
        RELEASE_ASSERT(!result.isDeferred.quickSet(result.variables[place].variable.index()));
        result.bodiesThatDefine.quickSet(result.variables[place].body.index());
        result.variablesByMaker.append(place);
    }
    std::ranges::sort(result.variablesByMaker, { }, [&](uint32_t place) {
        return std::pair { result.variables[place].body, result.variables[place].made };
    });
    return result;
}

static Vector<PlannedVariable> variablesOfPlan(const Program& program, const BitVector& described)
{
    constexpr SlotTraits traits = traitsOf(SlotRepresentation::JSValue);

    Vector<PlannedVariable> variables(program.variables.size());
    for (uint32_t index = 0; index < program.variables.size(); ++index) {
        const Variable& declaration = program.variables[index];
        PlannedVariable& planned = variables[index];
        planned.isDescribedToRunTime = described.quickGet(index);
        // Only the run time stores into the variable of a static field. It starts empty.
        if (declaration.owner)
            continue;
        switch (declaration.initialization) {
        case VariableInitialization::Uninitialized:
            planned.initialWord = traits.absentBits;
            break;
        case VariableInitialization::Undefined:
            planned.initialWord = traits.undefinedBits;
            break;
        case VariableInitialization::Function:
            RELEASE_ASSERT(declaration.function);
            planned.encoding = VariableEncoding::FunctionAsZero;
            planned.function = declaration.function;
            break;
        }
    }
    return variables;
}

// ---- Modules

// A mock that has ended may have kept a module below the importer from running, and only the run time knows. What may be mocked is itself left out only where it imports something that may be.
// A spy replaces what an export holds, which no record says: a test run claims nothing.
static BitVector modulesWhoseImportsAreNeverMockedInPlan(const Program& program)
{
    uint32_t numberOfModules = static_cast<uint32_t>(program.modules.size());
    BitVector neverMocked(numberOfModules);
    if (program.admitsReplacedBuiltins)
        return neverMocked;

    Vector<uint32_t> firstImporter;
    firstImporter.fill(0, numberOfModules + 1);
    auto forEachImport = [&](const Invocable<void(uint32_t imported, uint32_t importer)> auto& functor) {
        for (uint32_t index = 0; index < numberOfModules; ++index) {
            for (const ModuleRequest& request : program.modules[index].requests) {
                if (request.kind == ModuleRequestKind::Import && request.target)
                    functor(request.target.index(), index);
            }
        }
    };
    forEachImport([&](uint32_t imported, uint32_t) {
        ++firstImporter[imported + 1];
    });
    for (uint32_t index = 0; index < numberOfModules; ++index)
        firstImporter[index + 1] += firstImporter[index];
    Vector<uint32_t> importers;
    importers.fill(0, firstImporter.last());
    Vector<uint32_t> nextImporter = firstImporter;
    forEachImport([&](uint32_t imported, uint32_t importer) {
        importers[nextImporter[imported]++] = importer;
    });

    BitVector importsWhatMayBeMocked(numberOfModules);
    Vector<uint32_t, 16> pending;
    for (uint32_t index = 0; index < numberOfModules; ++index) {
        if (program.modules[index].flags.contains(ModuleFlags::MayBeMocked))
            pending.append(index);
    }
    while (!pending.isEmpty()) {
        uint32_t imported = pending.takeLast();
        for (uint32_t importer : importers.subspan(firstImporter[imported], firstImporter[imported + 1] - firstImporter[imported])) {
            if (!importsWhatMayBeMocked.quickSet(importer))
                pending.append(importer);
        }
    }
    for (uint32_t index = 0; index < numberOfModules; ++index) {
        if (!importsWhatMayBeMocked.quickGet(index))
            neverMocked.quickSet(index);
    }
    return neverMocked;
}

static void chooseModuleWordsOfPlan(const Program& program, PlanParts& parts)
{
    uint32_t numberOfModules = static_cast<uint32_t>(program.modules.size());
    parts.modulesWithValue.ensureSize(numberOfModules);
    parts.modulesWithRequireCacheValue.ensureSize(numberOfModules);
    parts.modulesWithNamespaceWord.ensureSize(numberOfModules);
    parts.modulesWhoseBodyIsPassedOver.ensureSize(numberOfModules);
    for (uint32_t index = 0; index < numberOfModules; ++index) {
        const Module& module = program.modules[index];
        if (module.kind != ModuleKind::CommonJs)
            parts.modulesWithValue.quickSet(index);
        // The value word of such a module holds what require() answers. Image places the word where code mentions it.
        if (module.kind == ModuleKind::Runtime || module.kind == ModuleKind::RuntimeSource)
            parts.modulesWithNamespaceWord.quickSet(index);
        if (!module.body || !program.at(module.body).hasBody() || bodyOfModuleOnlyReturns(program.at(module.body).body))
            parts.modulesWhoseBodyIsPassedOver.quickSet(index);
        // A RequireModule keeps the referrer that wrote it, so the requests as written name every pair that code can ask about.
        for (const ModuleRequest& request : module.requests) {
            if (request.kind != ModuleRequestKind::Require || !request.target)
                continue;
            if (requireReadsModuleValue(program, program.at(request.target)) && requireAddsToRequireCache(program, ModuleID(index), request.target))
                parts.modulesWithRequireCacheValue.quickSet(request.target.index());
        }
    }
    parts.modulesWithRequireCacheValue.filter(parts.modulesWithValue);
    parts.modulesWhoseImportsAreNeverMocked = modulesWhoseImportsAreNeverMockedInPlan(program);
}

static BitVector bodiesWithModuleStartInPlan(const Program& program, const ModuleEvaluationPlan& evaluation)
{
    BitVector bodies(program.functions.size());
    for (const ModuleStart& start : evaluation.starts())
        bodies.quickSet(start.body.index());
    return bodies;
}

// ---- Layouts, classes and closures

static LayoutID closureLayoutOfPlan(const Program& program)
{
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (program.layouts[index].kind == LayoutKind::Closure)
            return LayoutID(index);
    }
    return { };
}

static BitVector layoutsMadeAsCompiledObjectsInPlan(const Program& program, const SlotPlan& slots)
{
    BitVector layouts(program.layouts.size());
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (slots.holdsCompiledObjects(LayoutID(index)) && !slots.isNeverAllocated(LayoutID(index)))
            layouts.quickSet(index);
    }
    return layouts;
}

// A record names the class above by its number, and the run time takes a number without a record for no class at all. So a class that reached code evaluates has one, whether or not an object of it is made,
// so has a layout of which reached code makes an object, and so has every class above those. `if (false) { class Dead { } }`, an exported class of a file that nobody imports: nothing evaluates it, and its record would spell its name in the executable.
static BitVector layoutsWithClassRecordInPlan(const Program& program, const Facts& facts)
{
    BitVector layouts(program.layouts.size());
    Vector<LayoutID, 64> toFollow;
    auto add = [&](LayoutID id) {
        if (id && program.at(id).kind == LayoutKind::Instance && !layouts.quickSet(id.index()))
            toFollow.append(id);
    };
    const LinkTables& link = facts.linked();
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (!link.function(FunctionID(index)).isReached)
            continue;
        for (LayoutID evaluated : facts.summaries().at(FunctionID(index)).classesEvaluated)
            add(evaluated);
    }
    // Whoever makes an object asks the record of its layout what stands above the object. `Object.create(methods)`: no NewClass evaluates the layout of what it makes.
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (!program.layouts[index].has(LayoutFlags::IsNeverAllocated) && (index >= link.layouts.size() || link.layouts[index].isAllocated))
            add(LayoutID(index));
    }
    // The loader evaluates the class of a constructor function for a variable that is born as the function.
    for (const Variable& variable : program.variables) {
        if (variable.initialization == VariableInitialization::Function && link.function(variable.function).isReached)
            add(classOfConstructorFunction(program, variable.function));
    }
    while (!toFollow.isEmpty()) {
        LayoutID id = toFollow.takeLast();
        ClassFacts classFacts = facts.classOf(id);
        add(program.at(id).base);
        add(classFacts.base);
        add(classFacts.prototypeBase);
    }
    return layouts;
}

// PlanParts::variablesThatRunTimeMayFill: of what variablesDescribedToRunTimeByPlan() lists, all but the exports that the run time only reads.
static BitVector variablesThatRunTimeMayFillInPlan(const Program& program)
{
    BitVector found(program.variables.size());
    auto add = [&](VariableID variable) {
        if (variable)
            found.quickSet(variable.index());
    };
    for (const Module& module : program.modules) {
        for (const SyntheticExport& entry : module.syntheticExports)
            add(entry.variable);
        add(module.exportsVariable);
    }
    for (const EvalScope& scope : program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings)
            add(binding.variable);
    }
    for (uint32_t index = 0; index < program.variables.size(); ++index) {
        const Variable& variable = program.variables[index];
        if (variable.flags.containsAny({ VariableFlags::HoldsMock, VariableFlags::IsReplaceable }) || variable.owner)
            found.quickSet(index);
    }
    return found;
}

// PlanParts::functionsWhoseResultsPassNoReturn.
static BitVector functionsWhoseResultsPassNoReturnInPlan(const Program& program, const Facts& facts)
{
    const LinkTables& linked = facts.linked();
    BitVector found(program.functions.size());
    if (linked.functions.size() != program.functions.size())
        return found;

    Vector<FunctionID> toTell;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        FunctionID function(index);
        bool endsInUnlistedCall = std::ranges::any_of(facts.summaries().at(function).calls, [&](const SummaryCall& call) {
            return call.isInTailPosition && !linked.calleeAt(function, call.site);
        });
        if (endsInUnlistedCall && !found.quickSet(index))
            toTell.append(function);
    }
    // Upward, from a callee to whoever ends in a call of it.
    Vector<std::pair<FunctionID, FunctionID>> callersByCallee;
    for (const LinkedCall& call : linked.calls) {
        if (call.isInTailPosition)
            callersByCallee.append({ call.callee, call.caller });
    }
    std::ranges::sort(callersByCallee);
    while (!toTell.isEmpty()) {
        FunctionID callee = toTell.takeLast();
        for (auto [told, caller] : std::ranges::equal_range(callersByCallee, callee, { }, &std::pair<FunctionID, FunctionID>::first)) {
            if (!found.quickSet(caller.index()))
                toTell.append(caller);
        }
    }
    return found;
}

static BitVector functionsWithClosureCellInPlan(const Program& program, const CallingConventions& conventions)
{
    uint32_t numberOfFunctions = static_cast<uint32_t>(program.functions.size());
    // The run time registers the first class object of a layout in the cell of its constructor, whatever the constructor captures.
    BitVector constructors(numberOfFunctions);
    for (const Layout& layout : program.layouts) {
        if (layout.constructor)
            constructors.quickSet(layout.constructor.index());
    }

    BitVector functions(numberOfFunctions);
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        // The table makes a closure from the word that names this entry.
        if (!conventions.of(FunctionID(index)).has(CodeRole::GenericCallEntry))
            continue;
        if (program.functions[index].captures.isEmpty() || constructors.quickGet(index))
            functions.quickSet(index);
    }
    return functions;
}

BuiltinID builtinParentNamedByClassRecord(const Program& program, const Facts& facts, LayoutID classLayout)
{
    if (program.at(classLayout).kind != LayoutKind::Instance)
        return { };
    ClassFacts classFacts = facts.classOf(classLayout);
    if (classFacts.baseState != BaseState::BuiltinClass || !classFacts.baseClass)
        return { };
    return program.builtins().at(classFacts.baseClass).constructor;
}

// ---- How classes are made

static bool isDefinedByClassSyntaxInPlan(const Layout& layout)
{
    return layout.kind == LayoutKind::Instance && layout.constructor && !layout.isDerived() && !layout.has(LayoutFlags::IsConstructorFunction);
}

// The run time has one environment at hand when it makes the closure of a member: the one that the class object captures.
static bool capturesAtMostEnvironmentOfClassObjectInPlan(const Program& program, const Function& constructor, const Function& member)
{
    if (member.captures.isEmpty())
        return true;
    if (member.captures.size() != 1 || constructor.captures.size() != 1)
        return false;
    TypeID captured = member.captures[0].written();
    return captured && captured == constructor.captures[0].written() && program.at(captured).environment;
}

// A function without a name of its own takes one from its key when it is defined, and the run time gives none.
// A member under a name that its holder is made with redefines a property, and a record only adds.
static bool classRecordCanCreateMemberInPlan(const Program& program, const Function& constructor, const ClassMember& member)
{
    if (!member.key || !member.function || member.isAssignedByProgram || program.isIndexLike(member.key) || classHolderIsMadeWithName(program, member.side, member.key))
        return false;
    const Function& function = program.at(member.function);
    return function.name && capturesAtMostEnvironmentOfClassObjectInPlan(program, constructor, function);
}

// The run time makes the one closure of a constructor function by itself only where the function captures nothing.
static bool classAboveIsEvaluatedOnceInPlan(const Program& program, const Facts& facts, LayoutID id)
{
    if (!facts.isEvaluatedAtMostOnce(id))
        return false;
    const Layout& layout = program.at(id);
    return !layout.has(LayoutFlags::IsConstructorFunction) || (layout.constructor && program.at(layout.constructor).captures.isEmpty());
}

static LayoutID provenBaseOfClassInPlan(const Facts& facts, LayoutID id)
{
    ClassFacts classFacts = facts.classOf(id);
    return classFacts.baseState == BaseState::Layout ? classFacts.base : LayoutID();
}

// `class C { m() { } get g() { } static s() { } }`: the record of the class lists the members, and the run time makes them of it: with the class, or, of a class that is evaluated once, when a member is first looked for.
// The walk builds no definition of such a member: leaveMemberToClassRecord() of walk/AOTWalkReplacements.cpp. A definition that stays all the same defines the member once more, in its place among the keys.
// Withheld with TypedOperations: every member is created at its definition.
static Vector<ClassCreationPlan> classCreationOfPlan(const Program& program, const Facts& facts)
{
    if (!facts.mechanisms().contains(Mechanism::TypedOperations))
        return { };

    size_t numberOfLayouts = program.layouts.size();
    // Of the classes above a class, as far up as they are proven.
    BitVector classesBelowClassesEvaluatedOnce = answerAlongRows(numberOfLayouts, false, [&](LayoutID id) -> StepAlongRow {
        LayoutID base = provenBaseOfClassInPlan(facts, id);
        if (!base)
            return { true, { } };
        if (!classAboveIsEvaluatedOnceInPlan(program, facts, base))
            return { false, { } };
        return { std::nullopt, base };
    });
    BitVector classesWhoseRowEndsWithoutHeritage = answerAlongRows(numberOfLayouts, false, [&](LayoutID id) -> StepAlongRow {
        if (LayoutID base = provenBaseOfClassInPlan(facts, id))
            return { std::nullopt, base };
        return { !program.at(id).has(LayoutFlags::HasHeritage) && !facts.classOf(id).prototypeBase, { } };
    });

    // ClassData::guardsHoldWhileClear of object/AOTClassGuards.h: the record names every member of the class, and so does that of each class above, as far up as the records name a parent: ClassData::base, or expectedBase where none is proven.
    // A definition that is handed another parent ends the guards itself (ClassDefinition::armGuards()), and until a class is defined no object is an instance of it. Past a parent that nobody has written a class for, code resolves nothing under a guard.
    BitVector classesWhoseGuardsHoldWhileClear = answerAlongRows(numberOfLayouts, false, [&](LayoutID id) -> StepAlongRow {
        const Layout& layout = program.at(id);
        bool recordNamesEveryMember = std::ranges::all_of(facts.membersOf(id), [](const ClassMember& member) {
            return member.key && !member.isAssignedByProgram;
        });
        if (!isDefinedByClassSyntaxInPlan(layout) || !recordNamesEveryMember)
            return { false, { } };
        ClassFacts classFacts = facts.classOf(id);
        LayoutID parent;
        switch (classFacts.baseState) {
        case BaseState::BuiltinClass:
            return { true, { } };
        // Not `extends null`, and not a class that the program lays below another behind its definition: until then the record names a parent that the class does not have.
        case BaseState::None:
            return { !layout.has(LayoutFlags::HasHeritage) && !classFacts.prototypeBase, { } };
        case BaseState::Layout:
            parent = classFacts.base;
            break;
        case BaseState::Unknown:
            parent = writtenBaseOf(layout);
            if (!parent)
                return { true, { } };
            break;
        }
        if (!parent)
            return { false, { } };
        return { std::nullopt, parent };
    });

    Vector<ClassCreationPlan> plans(numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        LayoutID id(index);
        const Layout& layout = program.layouts[index];
        if (layout.kind != LayoutKind::Instance || !layout.constructor)
            continue;
        std::span<const ClassMember> members = facts.membersOf(id);
        ClassCreationPlan& plan = plans[index];
        plan.members.fill(MemberCreation::AtDefinition, members.size());
        if (!isDefinedByClassSyntaxInPlan(layout))
            continue;

        // The order of keys is observable: what the record creates comes first, so on each side it creates a beginning of the members and nothing behind that.
        const Function& constructor = program.at(layout.constructor);
        std::array<bool, numberOfEnumerators<ClassSide>> recordCreatesSide;
        recordCreatesSide.fill(true);
        for (size_t member = 0; member < members.size(); ++member) {
            bool& recordCreatesNextMember = recordCreatesSide[static_cast<unsigned>(members[member].side)];
            recordCreatesNextMember = recordCreatesNextMember && classRecordCanCreateMemberInPlan(program, constructor, members[member]);
            if (recordCreatesNextMember)
                plan.members[member] = MemberCreation::ByClassRecord;
        }

        bool basesAreEvaluatedOnce = classesBelowClassesEvaluatedOnce.quickGet(index);
        plan.createsMembersLazily = facts.isEvaluatedAtMostOnce(id) && recordCreatesSide[static_cast<unsigned>(ClassSide::Prototype)];
        plan.createsPrototypeBeforeClassObject = plan.createsMembersLazily && basesAreEvaluatedOnce && classesWhoseRowEndsWithoutHeritage.quickGet(index);
        // Static fields do not stand in the way: until the class object is made they lie in their variables (object/AOTStaticFields.h).
        plan.createsClassObjectOnMutation = plan.createsMembersLazily && basesAreEvaluatedOnce && recordCreatesSide[static_cast<unsigned>(ClassSide::Static)];
        plan.guardsHoldWhileClear = classesWhoseGuardsHoldWhileClear.quickGet(index);
        plan.guardsOfPrototypeSideHoldWhileClear = plan.guardsHoldWhileClear;
    }
    return plans;
}

// ---- Names of the global object

// Only a store through a binding replaces a value without a report. What unseen code, native code, a store under a computed key or a store that stays by name puts on the global object fires the watch
// that GlobalBindings::fileConstant() has set, which empties the constant in the record. So all that counts are the stores that the walk may bind: those on the global object as a body writes it.
// A key without a stable number is stored, for all that anybody knows.
static BitVector globalNamesNeverStoredInPlan(const Program& program, const Facts& facts)
{
    unsigned numberOfKeys = program.types.numberOfPropertyKeys();
    BitVector neverStored(numberOfKeys);
    for (unsigned index = 0; index < numberOfKeys; ++index) {
        if (program.types.hasStableNumber(PropertyKeyID(index)))
            neverStored.quickSet(index);
    }
    uint32_t globalObject = program.builtins().canonicalID(Builtins::GlobalThis).index();
    const ThinSummaries& summaries = facts.summaries();
    for (uint32_t function = 0; function < summaries.size(); ++function) {
        const ThinSummary& summary = summaries.at(FunctionID(function));
        for (const SummaryAccess& access : summary.accesses) {
            if (access.kind != SummaryAccess::Kind::Store || !access.key || access.key.index() >= numberOfKeys)
                continue;
            const SummaryNode& subject = summary.nodes[access.subject];
            if (subject.kind == SummaryNode::Kind::Builtin && subject.first == globalObject)
                neverStored.quickClear(access.key.index());
        }
    }
    return neverStored;
}

// ---- The driver

static void planModulesAndGlobalNames(const Program& program, const Facts& facts, BuildContext& build, PlanParts& parts)
{
    parts.layoutsWithClassRecord = layoutsWithClassRecordInPlan(program, facts);
    parts.literalStrings = computeLiteralStrings(program);
    parts.moduleEvaluation = planModuleEvaluation(program, facts, build.parallelLoops(), build.statistics());
    chooseModuleWordsOfPlan(program, parts);
    // Plan sees a store as it is written, not whether the walk will bind it, so both sets take one answer.
    parts.globalNamesNeverStoredByName = globalNamesNeverStoredInPlan(program, facts);
    parts.globalNamesNeverStoredThroughBinding = parts.globalNamesNeverStoredByName;
}

static void planKeyOrders(const Program& program, const Facts& facts, BuildContext& build, PlanParts& parts)
{
    parts.keyOrders = computeKeyOrders(program, facts.linked(), facts.summaries(), facts.bodies(), facts.mechanisms(), parts.slots, build);
}

static void planShapes(Program& program, const Facts& facts, PlanParts& parts)
{
    Vector<uint32_t> orderOfLayoutsForShapes = orderOfLayoutsForShapeNumbers(program, facts, parts.slots);
    parts.shapes = computeShapeTable(program, facts.linked(), parts.slots, parts.closureLayout, orderOfLayoutsForShapes.span());
}

static void planConventions(const Program& program, const Facts& facts, BuildContext& build, PlanParts& parts)
{
    // A convention counts its parameters in sixteen bits, as the table of functions counts them: FunctionTablesByRange::collectClosureFunction() names the same limit.
    bool everyFunctionCanBeCounted = true;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        size_t numberOfParameters = program.functions[index].numberOfEntryParameters();
        if (numberOfParameters <= std::numeric_limits<decltype(FunctionConvention::numberOfEntryParameters)>::max())
            continue;
        build.reportLimitExceeded(LimitID::MaxReservedStackArguments, numberOfParameters, Subject { .function = FunctionID(index) });
        everyFunctionCanBeCounted = false;
    }
    if (!everyFunctionCanBeCounted)
        return;

    parts.conventions = computeCallingConventions(program, facts, bodiesWithModuleStartInPlan(program, parts.moduleEvaluation), layoutsMadeAsCompiledObjectsInPlan(program, parts.slots));
    parts.functionsWithClosureCell = functionsWithClosureCellInPlan(program, parts.conventions);
    uint64_t numberOfAsyncFunctionsThatNeverSuspend = 0;
    uint64_t numberOfFunctionsThatReturnNumberOrUndefinedRaw = 0;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        const FunctionConvention& convention = parts.conventions.of(FunctionID(index));
        numberOfAsyncFunctionsThatNeverSuspend += convention.coroutineNeverSuspends;
        if (!convention.has(CodeRole::Body) || convention.numberOfResults != 1)
            continue;
        Representation result = parts.conventions.resultRepresentation({ FunctionID(index), CodeRole::Body });
        numberOfFunctionsThatReturnNumberOrUndefinedRaw += result == Representation::OptionalInt64 || result == Representation::OptionalDouble;
    }
    build.statistics().count(StatisticID::AsyncWorkersMade, numberOfAsyncFunctionsThatNeverSuspend);
    build.statistics().count(StatisticID::NumbersOrUndefinedReturnedRaw, numberOfFunctionsThatReturnNumberOrUndefinedRaw);
    // Results are checked under each of the three words.
    if (AOT::hasFlag(build.options().testing, Wire::TestingSwitch::CheckProofsAtRunTime) || AOT::hasFlag(build.options().testing, Wire::TestingSwitch::CheckProofsAtEntries) || AOT::hasFlag(build.options().testing, Wire::TestingSwitch::CheckProofsAtEntriesAndFields)) {
        parts.functionsWhoseResultsPassNoReturn = functionsWhoseResultsPassNoReturnInPlan(program, facts);
        parts.variablesThatRunTimeMayFill = variablesThatRunTimeMayFillInPlan(program);
    }
}

std::unique_ptr<Plan> planProgram(Program& program, const Facts& facts, BuildContext& build)
{
    StepTimes times(build, nameOf(Stage::PlanSlots));
    auto finish = [&](ASCIILiteral step) {
        times.didFinish(step);
        return !build.diagnostics().hasErrors();
    };
    auto runLane = [&](ASCIILiteral step, const auto& lane) {
        StepTimes timesOfLane(build, nameOf(Stage::PlanSlots));
        lane();
        timesOfLane.didFinish(step);
    };

    PlanParts parts;
    parts.usesWideReferences = build.recordedOptions().useWideReferences();
    parts.hasOneInstance = facts.hasOneInstance();
    parts.closureLayout = closureLayoutOfPlan(program);
    // A lane writes members of the parts that no other lane of its group reads. Only the slots add types to the program.
    SlotPlanParts slots;
    // It goes by Facts alone, and the plan gets it where its turn is: in front of planTailCallsAndStackTests().
    BitVector functionsThatEndTheirTailCalls;
    runLanes(build.parallelLoops(), {
        [&] { runLane("plan: lane of slots"_s, [&] { slots = computeSlotPlanParts(program, facts, build); }); },
        [&] { runLane("plan: lane of modules, classes, literal strings"_s, [&] { planModulesAndGlobalNames(program, facts, build, parts); }); },
        [&] { runLane("plan: lane of tail calls that end"_s, [&] {
            if (facts.mechanisms().contains(Mechanism::DirectCalls))
                functionsThatEndTheirTailCalls = findFunctionsThatEndTheirTailCalls(program, facts);
        }); },
    });
    if (!finish("plan: slots beside modules, classes, literal strings"_s))
        return nullptr;

    slots.closureForms = chooseClosureForms(program, facts.linked());
    leaveClosuresOfLiteralsUnmade(program, facts, build.recordedOptions().useWideReferences(), slots);
    const BitVector variablesDescribedToRunTime = variablesDescribedToRunTimeByPlan(program);
    parts.deferredDefinitions = deferredDefinitionsOfPlan(program, facts, variablesDescribedToRunTime, AOT::hasFlag(build.options().testing, Wire::TestingSwitch::CheckProofsAtRunTime));
    slots.variables = variablesOfPlan(program, variablesDescribedToRunTime);
    slots.classCreation = classCreationOfPlan(program, facts);
    parts.slots = SlotPlan(WTF::move(slots));
    if (!finish("plan: closure forms, variables, class creation"_s))
        return nullptr;

    runLanes(build.parallelLoops(), {
        [&] { runLane("plan: lane of key orders"_s, [&] { planKeyOrders(program, facts, build, parts); }); },
        [&] { runLane("plan: lane of shapes"_s, [&] { planShapes(program, facts, parts); }); },
        [&] { runLane("plan: lane of conventions and closure cells"_s, [&] { planConventions(program, facts, build, parts); }); },
    });
    if (!finish("plan: conventions and closure cells"_s))
        return nullptr;

    // Where a guard pays is asked of the code that builds typed operations, which asks the plan.
    std::unique_ptr<Plan> plan = makeUnique<Plan>(WTF::move(parts));
    plan->setMethodTable(planMethodTable(program, facts, *plan));
    if (!finish("plan: method table"_s))
        return nullptr;
    plan->setParameterGuards(planParameterGuards(program, facts, *plan, build).span());
    plan->setFunctionsThatEndTheirTailCalls(WTF::move(functionsThatEndTheirTailCalls));
    planTailCallsAndStackTests(program, facts, *plan, build);
    finishMethodTableForUnions(program, facts, *plan);
    plan->fuseCallEntriesThatOnlyMove(program, build.parallelLoops());
    if (!finish("plan: parameter guards"_s))
        return nullptr;
    return plan;
}

// ---- Allocations

static bool allocationIsBornUnfinishedInPlan(const PlannedLayout& planned, std::span<const FieldIndex> initialFields)
{
    unsigned numberOfFields = planned.numberOfFields();
    BitVector isInitialField(numberOfFields);
    for (FieldIndex field : initialFields) {
        RELEASE_ASSERT(field < numberOfFields);
        isInitialField.quickSet(field);
    }
    for (FieldIndex field = 0; field < numberOfFields; ++field) {
        if (planned.slots[field].isFilledAtBirth && !isInitialField.quickGet(field))
            return true;
    }
    return false;
}

std::optional<PlacedAllocation> Plan::allocation(const Program& program, LayoutID id, AllocationClassID, std::span<const FieldIndex> initialFields) const
{
    const PlannedLayout& planned = layout(id);
    uint32_t shape = shapes().generalShapeOf(id);
    // The image describes no cell of a layout that is never allocated or whose objects are not compiled objects.
    if (!planned.holdsCompiledObjects() || !shapes().isAssigned(shape))
        return std::nullopt;

    PlacedAllocation placed;
    placed.layout = id;
    placed.shape = shape;
    placed.size = planned.size;
    const Layout& declared = program.at(declaredLayoutOf(program, id));
    if (declared.kind == LayoutKind::Instance)
        placed.constructedBy = declared.constructor;
    placed.isBornUnfinished = allocationIsBornUnfinishedInPlan(planned, initialFields);

    if (shapes().shape(shape).keysAreNeverEnumerated) {
        placed.orderWord = unorderedOrderWord;
        return placed;
    }

    Vector<FieldIndex, 16> fieldsWithStorage;
    for (FieldIndex field : initialFields) {
        if (planned.slots[field].hasStorage() && planned.environmentOfDeferredClosures != field)
            fieldsWithStorage.append(field);
    }
    std::optional<uint32_t> orderWord = keyOrders().findOrderWord(id, fieldsWithStorage.span());
    if (!orderWord)
        return std::nullopt;
    placed.orderWord = *orderWord;
    return placed;
}

PlacedAllocation Plan::environmentAllocation(LayoutID id) const
{
    PlacedAllocation placed;
    placed.layout = id;
    placed.shape = shapes().generalShapeOf(id);
    placed.size = layout(id).size;
    return placed;
}

PlacedAllocation Plan::closureAllocation(unsigned numberOfCaptures) const
{
    RELEASE_ASSERT(numberOfCaptures <= AOT::Closure::maxNumberOfCaptures);
    unsigned roomForCaptures = numberOfCaptures | 1;
    PlacedAllocation placed;
    placed.layout = m_parts.closureLayout;
    placed.shape = shapes().generalShapeOf(m_parts.closureLayout);
    placed.size = static_cast<uint32_t>(AOT::Closure::allocationSize(roomForCaptures));
    placed.numberOfCaptures = static_cast<uint16_t>(roomForCaptures);
    return placed;
}

// ---- The units that something other than compiled code names

namespace {

class CollectorOfUnitsNamedOutsideCode {
    WTF_MAKE_NONCOPYABLE(CollectorOfUnitsNamedOutsideCode);
public:
    CollectorOfUnitsNamedOutsideCode(const Program& program, const Facts& facts, const Plan& plan)
        : m_program(program)
        , m_facts(facts)
        , m_plan(plan)
        , m_conventions(plan.conventions())
        , m_functionsThatHostEnters(functionsCalledByHost(program))
        , m_visitedClasses(program.layouts.size())
    {
    }

    Vector<CodeUnit> collect();

private:
    void add(CodeUnit);
    void addClosureOf(FunctionID);
    void addFunctionsNamedByClassRecords(LayoutID classLayout);

    CodeUnit unitThatLoaderEnters(ModuleID) const;
    void addModulesThatLoaderEnters();
    void addBodiesThatStartsOfModulesCall();
    void addFunctionsThatHostEnters();
    void addInitialFunctionsOfVariables();
    void addFunctionsWithoutReferrers();

    const Program& m_program;
    const Facts& m_facts;
    const Plan& m_plan;
    const CallingConventions& m_conventions;
    // By function. The bodies of modules are among them until addModulesThatLoaderEnters() has taken out those that the loader does not enter.
    BitVector m_functionsThatHostEnters;
    BitVector m_visitedClasses;
    Vector<CodeUnit> m_units;
};

void CollectorOfUnitsNamedOutsideCode::add(CodeUnit unit)
{
    if (m_conventions.exists(unit))
        m_units.append(unit);
}

// Whoever makes a closure writes the number of this entry into its word, whether or not anything ever enters the function. So this does not ask whether the function may be reached.
// Once the entry of a constructor has code, the record of its class names the entries of the members that it creates.
void CollectorOfUnitsNamedOutsideCode::addClosureOf(FunctionID function)
{
    if (!function)
        return;
    CodeUnit entry = m_conventions.closureEntryOf(function);
    if (!m_conventions.exists(entry))
        return;
    m_units.append(entry);
    if (CodeUnit constructEntry = m_conventions.genericEntry(entry).constructEntry)
        add(constructEntry);
    addFunctionsNamedByClassRecords(m_program.at(function).constructedClass);
}

// The run time makes a class of its record, and first the classes above it of theirs. The members are read as UnitsBehindMention::addClass() of image/AOTMentions.cpp reads them.
void CollectorOfUnitsNamedOutsideCode::addFunctionsNamedByClassRecords(LayoutID classLayout)
{
    LayoutID current = classLayout;
    while (current && m_plan.hasClassRecord(current) && !m_visitedClasses.set(current.index())) {
        addClosureOf(m_program.at(current).constructor);
        std::span<const ClassMember> members = m_facts.membersOf(current);
        const Vector<MemberCreation>& creations = m_plan.slots().classCreation(current).members;
        for (size_t index = 0; index < members.size() && index < creations.size(); ++index) {
            switch (creations[index]) {
            case MemberCreation::AtDefinition:
                break;
            case MemberCreation::ByClassRecord:
                addClosureOf(members[index].function);
                break;
            }
        }
        ClassFacts classFacts = m_facts.classOf(current);
        current = classFacts.baseState == BaseState::Layout ? classFacts.base : LayoutID();
    }
}

// The table of modules and the units behind a mention of a module ask the same: askAboutBodyOfModule() of image/AOTModuleLists.cpp, UnitsBehindMention::addModule() of image/AOTMentions.cpp.
CodeUnit CollectorOfUnitsNamedOutsideCode::unitThatLoaderEnters(ModuleID id) const
{
    const ModuleEvaluationPlan& evaluation = m_plan.moduleEvaluation();
    if (evaluation.isEvaluatedByCodeAlone(id))
        return { };
    FunctionID body = m_program.at(id).body;
    if (evaluation.findStart(id))
        return { body, CodeRole::ModuleStart };
    if (m_plan.moduleBodyIsEntered(id))
        return { body, CodeRole::Body };
    return { };
}

// The table of modules names a body only where the unit has code, so a module that is left out here does not run, and nothing says so.
void CollectorOfUnitsNamedOutsideCode::addModulesThatLoaderEnters()
{
    for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
        FunctionID body = m_program.modules[index].body;
        if (!body)
            continue;
        CodeUnit entered = unitThatLoaderEnters(ModuleID(index));
        if (!entered || !m_facts.mayBeReached(body)) {
            m_functionsThatHostEnters.clear(body.index());
            continue;
        }
        // A start has no code. The loader comes to it through the entry of the body, which addFunctionsThatHostEnters() adds.
        if (entered.role != CodeRole::ModuleStart)
            add(entered);
    }
}

// No code names what the row of a start calls: listStartsOfModules() of image/AOTModuleTables.cpp writes the rows of the same starts, and stops the build at a body without code.
void CollectorOfUnitsNamedOutsideCode::addBodiesThatStartsOfModulesCall()
{
    const ModuleEvaluationPlan& evaluation = m_plan.moduleEvaluation();
    for (const ModuleStart& start : evaluation.starts()) {
        if (!m_conventions.exists({ start.body, CodeRole::ModuleStart }))
            continue;
        for (const ModuleStartOperation& operation : evaluation.operationsOf(start)) {
            if (operation.kind == ModuleStartOperation::Kind::CallBody)
                add(m_conventions.directEntryOf(operation.callee));
        }
    }
}

// LinkedFunction::isInQueueFromStart: every unit whose code may name another function, but for a slow version that only a fast version that is left enters. The others may come at any time: UnitQueue::standsOutsideCycles().
void CollectorOfUnitsNamedOutsideCode::addFunctionsWithoutReferrers()
{
    const Vector<LinkedFunction>& functions = m_facts.linked().functions;
    for (uint32_t index = 0; index < functions.size(); ++index) {
        if (!functions[index].isInQueueFromStart)
            continue;
        for (unsigned role = 0; role < numberOfCodeRolesOfOneFunction; ++role) {
            if (CodeUnit unit { FunctionID(index), static_cast<CodeRole>(role) }; m_conventions.isSeedOfFunctionInQueueFromStart(unit))
                add(unit);
        }
    }
}

void CollectorOfUnitsNamedOutsideCode::addFunctionsThatHostEnters()
{
    for (size_t index : m_functionsThatHostEnters) {
        FunctionID function(index);
        if (m_facts.mayBeReached(function))
            addClosureOf(function);
    }
}

// Any other variable has a word only where code mentions it, and the mention brings the entry: forEachUnitBehind() of image/AOTMentions.cpp.
void CollectorOfUnitsNamedOutsideCode::addInitialFunctionsOfVariables()
{
    unsigned numberOfVariables = m_plan.slots().numberOfVariables();
    for (unsigned index = 0; index < numberOfVariables; ++index) {
        PlannedVariable variable = m_plan.variable(VariableID(index));
        if (variable.isDescribedToRunTime)
            addClosureOf(variable.function);
    }
}

Vector<CodeUnit> CollectorOfUnitsNamedOutsideCode::collect()
{
    addModulesThatLoaderEnters();
    addBodiesThatStartsOfModulesCall();
    addFunctionsThatHostEnters();
    addInitialFunctionsOfVariables();
    addFunctionsWithoutReferrers();

    AOT::sort(m_units);
    removeRepeatedElements(m_units);
    return WTF::move(m_units);
}

} // anonymous namespace

Vector<CodeUnit> unitsNamedOutsideCode(const Program& program, const Facts& facts, const Plan& plan)
{
    return CollectorOfUnitsNamedOutsideCode(program, facts, plan).collect();
}

// ---- The check

#define FOR_EACH_AOT_PLAN_RULE(v) \
    v(WideReferencesOfBuild, "the plan and the options of the build disagree on wide references") \
    v(NumberOfLayouts, "the plan does not have one layout for each layout of the program") \
    v(NumberOfVariables, "the plan does not have one variable for each variable of the program and each added one") \
    v(NumberOfSlots, "the layout does not have one slot for each declared field, each appended field and each hidden word") \
    v(CellSize, "the size of the cell is no multiple of 8") \
    v(SlotAlignment, "a slot is not aligned to its width") \
    v(SlotOutsideCell, "a slot lies in front of the first field or past the end of the cell") \
    v(SlotOutsideExtension, "a slot lies in front of the first granule or past the end of the Extension") \
    v(SlotsOverlap, "a slot shares bytes with another slot of the layout") \
    v(StorageMissing, "cells of the layout may be made, and a field has no storage") \
    v(ShapeMissing, "cells of the layout may be made, and the layout has no shape") \
    v(ShapeOfOtherLayout, "a shape of the layout names another layout, or a shape lies outside the shapes of its layout") \
    v(FirstEnvironmentShape, "the first shape of environments is no shape") \
    v(SideOfShape, "the shape of an object does not lie below the first shape of environments, or another shape does") \
    v(OrdersWithoutObjects, "the layout holds no compiled objects and has key orders") \
    v(NumberOfKeyOrders, "the layout has more key orders than an order word can name") \
    v(FieldOfKeyOrder, "a key order is empty, or lists a field twice or one without storage") \
    v(SortingOfKeyOrders, "the key orders of the layout are not sorted, or one begins another") \
    v(UnitsOfMethodTable, "the method table does not have the units of its selectors and no other") \
    v(BytesKeptZero, "bytes that the layout keeps zero lie outside the cell or share bytes with a slot") \
    v(ExtensionWord, "a slot lies out of line and the layout has no word for the Extension, or it has the word and no such slot")

namespace {

enum class RuleOfPlan : uint32_t {
    None,
#define AOT_PLAN_RULE(name, words) name,
    FOR_EACH_AOT_PLAN_RULE(AOT_PLAN_RULE)
#undef AOT_PLAN_RULE
};

struct ExtentOfPlannedSlot {
    uint64_t first { 0 };
    uint64_t end { 0 };
};

class CheckerOfPlan {
    WTF_MAKE_NONCOPYABLE(CheckerOfPlan);
public:
    CheckerOfPlan(const Program& program, const Plan& plan, BuildContext& build)
        : m_program(program)
        , m_plan(plan)
        , m_build(build)
    {
    }

    void check();

private:
    static ASCIILiteral wordsOf(RuleOfPlan);
    bool expect(bool holds, RuleOfPlan, LayoutID = { });

    void checkArea(LayoutID, const PlannedLayout&, SlotStorage, uint64_t start, uint64_t limit, RuleOfPlan ruleOfBounds);
    void checkLayout(LayoutID);
    void checkShapes();
    void checkKeyOrdersOf(LayoutID);
    void checkMethodTable();

    const Program& m_program;
    const Plan& m_plan;
    BuildContext& m_build;
    Vector<ExtentOfPlannedSlot, 32> m_extents;
    BitVector m_isListed;
};

ASCIILiteral CheckerOfPlan::wordsOf(RuleOfPlan rule)
{
    switch (rule) {
    case RuleOfPlan::None:
        break;
#define AOT_PLAN_RULE(name, words) \
    case RuleOfPlan::name: \
        return words ""_s;
    FOR_EACH_AOT_PLAN_RULE(AOT_PLAN_RULE)
#undef AOT_PLAN_RULE
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool CheckerOfPlan::expect(bool holds, RuleOfPlan rule, LayoutID layout)
{
    if (!holds) {
        m_build.diagnostics().add(InternalError {
            .kind = InternalErrorKind::PlacementCheckFailed,
            .layout = layout,
            .count = static_cast<uint32_t>(rule),
            .detail = String(wordsOf(rule)),
        });
    }
    return holds;
}

void CheckerOfPlan::checkArea(LayoutID id, const PlannedLayout& planned, SlotStorage storage, uint64_t start, uint64_t limit, RuleOfPlan ruleOfBounds)
{
    m_extents.shrink(0);
    for (const PlacedSlot& slot : planned.slots) {
        if (slot.storage != storage)
            continue;
        uint64_t end = static_cast<uint64_t>(slot.offset) + slot.width();
        expect(!(slot.offset % slot.width()), RuleOfPlan::SlotAlignment, id);
        expect(slot.offset >= start && end <= limit && slot.offset <= SlotData::maxOffset, ruleOfBounds, id);
        m_extents.append({ slot.offset, end });
    }
    AOT::sort(m_extents, { }, &ExtentOfPlannedSlot::first);
    uint64_t endOfSlotsInFront = 0;
    for (const ExtentOfPlannedSlot& extent : m_extents) {
        expect(extent.first >= endOfSlotsInFront, RuleOfPlan::SlotsOverlap, id);
        endOfSlotsInFront = std::max(endOfSlotsInFront, extent.end);
    }
}

void CheckerOfPlan::checkLayout(LayoutID id)
{
    const PlannedLayout& planned = m_plan.layout(id);
    if (!planned.hasSlots())
        return;
    if (!expect(planned.slots.size() == m_program.at(id).fields.size() + planned.appendedKeys.size() + numberOfHiddenWords, RuleOfPlan::NumberOfSlots, id))
        return;
    bool hasSlotOutOfLine = planned.slots.containsIf([](const PlacedSlot& slot) {
        return slot.isOutOfLine();
    });
    expect(hasSlotOutOfLine == planned.hasExtension(), RuleOfPlan::ExtensionWord, id);

    bool isEnvironment = planned.storage == LayoutStorage::Environment;
    expect(!(planned.size % 8), RuleOfPlan::CellSize, id);
    checkArea(id, planned, SlotStorage::Inline, isEnvironment ? offsetOfFirstEnvironmentVariable : offsetOfFirstField, planned.size, RuleOfPlan::SlotOutsideCell);
    checkArea(id, planned, SlotStorage::OutOfLine, offsetOfFirstExtensionSlot, std::min(planned.extensionSize, maxExtensionSize), RuleOfPlan::SlotOutsideExtension);
    for (const ReservedBytes& reserved : planned.bytesKeptZero) {
        uint64_t endOfReserved = static_cast<uint64_t>(reserved.offset) + reserved.width;
        expect(reserved.width && reserved.offset >= offsetOfFirstField && endOfReserved <= planned.size, RuleOfPlan::BytesKeptZero, id);
        for (const PlacedSlot& slot : planned.slots)
            expect(slot.storage != SlotStorage::Inline || slot.offset >= endOfReserved || static_cast<uint64_t>(slot.offset) + slot.width() <= reserved.offset, RuleOfPlan::BytesKeptZero, id);
    }

    if (!m_plan.isInUse(id) || !(planned.holdsCompiledObjects() || isEnvironment))
        return;
    expect(m_plan.shapes().isAssigned(m_plan.shapes().generalShapeOf(id)), RuleOfPlan::ShapeMissing, id);
    for (FieldIndex field = 0; field < planned.numberOfFields(); ++field)
        expect(planned.slots[field].hasStorage() || !planned.slots[field].indexInLayoutRecord, RuleOfPlan::StorageMissing, id);
}

void CheckerOfPlan::checkShapes()
{
    const ShapeTable& shapes = m_plan.shapes();
    uint32_t firstEnvironmentShape = shapes.firstEnvironmentShape();
    expect(firstEnvironmentShape, RuleOfPlan::FirstEnvironmentShape);
    for (uint32_t number = 1; number <= shapes.numberOfShapes(); ++number) {
        if (!shapes.isAssigned(number))
            continue;
        LayoutID layout = shapes.shape(number).layout;
        ShapeRange range = shapes.shapesOf(layout);
        expect(number >= range.first && number < range.end(), RuleOfPlan::ShapeOfOtherLayout, layout);
        expect(m_plan.layout(layout).holdsCompiledObjects() == (number < firstEnvironmentShape), RuleOfPlan::SideOfShape, layout);
    }
    for (uint32_t index = 0; index < m_program.layouts.size(); ++index) {
        ShapeRange range = shapes.shapesOf(LayoutID(index));
        for (uint32_t number = range.first; number < range.end(); ++number)
            expect(shapes.isAssigned(number) && shapes.shape(number).layout == LayoutID(index), RuleOfPlan::ShapeOfOtherLayout, LayoutID(index));
    }
}

void CheckerOfPlan::checkKeyOrdersOf(LayoutID id)
{
    const KeyOrders& orders = m_plan.keyOrders();
    uint32_t numberOfOrders = orders.numberOfOrdersOf(id);
    if (!numberOfOrders)
        return;
    const PlannedLayout& planned = m_plan.layout(id);
    if (!expect(planned.holdsCompiledObjects(), RuleOfPlan::OrdersWithoutObjects, id))
        return;
    expect(numberOfOrders <= maxKeyOrdersPerLayout, RuleOfPlan::NumberOfKeyOrders, id);

    for (uint32_t number = 0; number < numberOfOrders; ++number) {
        std::span<const FieldIndex> order = orders.order(id, number);
        m_isListed.clearAll();
        bool listsFieldsOnce = !order.empty() && order.size() <= maxFieldsInOrder && std::ranges::all_of(order, [&](FieldIndex field) {
            return field < planned.numberOfFields() && planned.slots[field].hasStorage() && !m_isListed.set(field);
        });
        expect(listsFieldsOnce, RuleOfPlan::FieldOfKeyOrder, id);
        if (!number)
            continue;
        std::span<const FieldIndex> orderInFront = orders.order(id, number - 1);
        bool beginsThisOrder = orderInFront.size() <= order.size() && std::ranges::equal(orderInFront, order.first(orderInFront.size()));
        expect(std::ranges::lexicographical_compare(orderInFront, order) && !beginsThisOrder, RuleOfPlan::SortingOfKeyOrders, id);
    }
}

// Of a selector: the lookup, and an entry for each method of the family.
void CheckerOfPlan::checkMethodTable()
{
    const CallingConventions& conventions = m_plan.conventions();
    size_t numberOfFamilies = conventions.methodFamilies().size();
    uint64_t numberOfTableUnits = 0;
    for (const MethodSelector& selector : conventions.methodSelectors()) {
        if (!expect(selector.family < numberOfFamilies && selector.firstUnit == numberOfTableUnits, RuleOfPlan::UnitsOfMethodTable))
            return;
        numberOfTableUnits += 1 + conventions.familyOf(selector).methods.size();
    }
    uint64_t numberOfUnits = static_cast<uint64_t>(conventions.numberOfFunctions()) * numberOfCodeRolesOfOneFunction + conventions.boundFunctions().size() * numberOfBoundRoles + numberOfTableUnits;
    // A place for each layout that a unit can name, or none at all.
    expect(!conventions.numberOfPlacesOfSerializers() || conventions.numberOfPlacesOfSerializers() == std::min<size_t>(m_program.layouts.size(), CodeUnit::numberOfLayoutsThatSerializersCanName), RuleOfPlan::UnitsOfMethodTable);
    numberOfUnits += conventions.numberOfPlacesOfSerializers();
    expect(conventions.numberOfUnits() == numberOfUnits, RuleOfPlan::UnitsOfMethodTable);
}

void CheckerOfPlan::check()
{
    expect(m_plan.usesWideReferences() == m_build.recordedOptions().useWideReferences(), RuleOfPlan::WideReferencesOfBuild);
    size_t numberOfVariables = m_program.variables.size() + m_plan.deferredDefinitions().numberOfAddedVariables() + m_plan.promotedEnvironments().variables.size();
    expect(m_plan.slots().numberOfVariables() == numberOfVariables, RuleOfPlan::NumberOfVariables);
    checkMethodTable();
    if (!expect(m_plan.slots().numberOfLayouts() == m_program.layouts.size(), RuleOfPlan::NumberOfLayouts))
        return;

    for (uint32_t index = 0; index < m_program.layouts.size(); ++index) {
        checkLayout(LayoutID(index));
        checkKeyOrdersOf(LayoutID(index));
    }
    checkShapes();
}

} // anonymous namespace

#undef FOR_EACH_AOT_PLAN_RULE

void checkPlan(const Program& program, const Facts& facts, const Plan& plan, BuildContext& build)
{
    CheckerOfPlan(program, plan, build).check();
    checkStorageOfFields(program, facts, plan, build);
    if (build.validates())
        checkFormsRoundRingsOfTailCalls(program, facts, plan, build);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
