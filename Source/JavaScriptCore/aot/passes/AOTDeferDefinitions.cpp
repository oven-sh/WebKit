/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTDeferDefinitions.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTDefinitions.h"
#include "AOTFlowApply.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTIR.h"
#include "AOTIRBuilder.h"
#include "AOTKnownClosures.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <ranges>
#include <utility>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct DefinitionMention {
    enum class Kind : uint8_t { VariableStore, Closure, ClassDefinition, EnvironmentAllocation, EnvironmentStore };

    Kind kind { Kind::VariableStore };
    uint32_t subject { 0 };
    uint32_t field { 0 };
};

// How many operations of the program name each thing, counted up to two.
struct Counts {
    bool isStoredByOneOperation(FieldReference field) const
    {
        auto stores = std::ranges::equal_range(storedEnvironmentFields, field);
        return stores.size() == 1;
    }

    Vector<uint8_t> storesOfVariable;
    Vector<uint8_t> closuresOfFunction;
    Vector<uint8_t> definitionsOfClass;
    Vector<uint8_t> allocationsOfEnvironment;
    // Ascending, once for each store.
    Vector<FieldReference> storedEnvironmentFields;
};

// What the run time finds through a record of the program. A pass may later turn an access by name into a read of such a variable.
struct NamedByRecords {
    bool contains(FieldReference field) const { return std::ranges::binary_search(environmentFields, field); }

    BitVector variables;
    Vector<FieldReference> environmentFields;
};

struct DefinitionScanContext {
    const Program& program;
    const FlowResults& flow;
    BuildContext& build;
    const Counts& counts;
    const NamedByRecords& named;
};

struct Deferral {
    enum class Kind : uint8_t { Closure, ClosureInEnvironment, Class, RegExp };

    Kind kind { Kind::Closure };
    FunctionID body;
    // What goes from the body.
    Vector<ValueID, 4> removed;
    // The variables that no read finds empty. They stand for the object from now on. None yet for a field of an environment.
    Vector<VariableID, 1> variables;
    // The variables that a read may find in their dead zone, and their stores. They go on saying whether the declaration has run, and a new variable beside each stands for the object.
    Vector<VariableID, 1> variablesWithDeadZone;
    Vector<ValueID, 1> storesThatEndDeadZone;
    FieldReference field;
    // What the variables are born as.
    FunctionID function;
    // The class.
    LayoutID layout;
    // NewClass, where something in the body still uses the class object. It becomes a read of the variable, or of the one beside it.
    ValueID classObject;
    VariableID variableReadInBody;
    // NewRegExp.
    ValueID literal;
};

// What a read finds that comes before the only store.
enum class BeforeStore : uint8_t { NothingIsRead, DeadZone, SomethingElse };

class DeferredDefinitionScanner {
public:
    DeferredDefinitionScanner(const DefinitionScanContext& context, FunctionID id, Vector<Deferral>& found)
        : m_context(context)
        , m_program(context.program)
        , m_id(id)
        , m_function(context.program.at(id))
        , m_found(found)
    {
    }

    void run();

private:
    FunctionAnalyses& analyses();
    const UseLists& useLists() { return analyses().useLists(); }

    bool hasOnlyClosure(FunctionID function) const { return m_context.counts.closuresOfFunction[function.index()] == 1; }
    BeforeStore whatIsReadBefore(const Value& store, Deferral::Kind) const;
    bool tryTakeStore(Deferral&, ValueID store) const;
    bool collectStoredAllocation(ValueID stored, Vector<ValueID, 4>& values);
    bool isOnlyClosureOverNothing(const Value& allocation) const;
    bool isOnlyEnvironment(ValueID environment, LayoutID);
    VariableID findVariableOfType(const Deferral&, TypeID) const;

    void scanVariableStore(ValueID);
    void scanEnvironmentStore(ValueID);
    void scanClassDefinition(ValueID);

    const DefinitionScanContext& m_context;
    const Program& m_program;
    FunctionID m_id;
    const Function& m_function;
    Vector<Deferral>& m_found;
    std::optional<FunctionAnalyses> m_analyses;
};

struct MovedField {
    FieldReference field;
    VariableID variable;
};

struct SplitVariable {
    VariableID deadZone;
    VariableID object;
};

struct DeferredRegExp {
    VariableID variable;
    Value literal;
};

struct DefinitionPlan {
    std::span<const Deferral> deferralsIn(FunctionID body) const LIFETIME_BOUND
    {
        auto range = std::ranges::equal_range(deferrals, body, { }, &Deferral::body);
        return deferrals.subspan(range.begin() - deferrals.begin(), range.size());
    }

    VariableID variableOf(FieldReference field) const
    {
        auto found = std::ranges::lower_bound(movedFields, field, { }, &MovedField::field);
        return found != movedFields.end() && found->field == field ? found->variable : VariableID();
    }

    VariableID objectBeside(VariableID deadZone) const
    {
        auto found = std::ranges::lower_bound(splitVariables, deadZone, { }, &SplitVariable::deadZone);
        return found != splitVariables.end() && found->deadZone == deadZone ? found->object : VariableID();
    }

    const DeferredRegExp* findRegExp(VariableID variable) const LIFETIME_BOUND
    {
        auto found = std::ranges::lower_bound(regExps, variable, { }, &DeferredRegExp::variable);
        return found != regExps.end() && found->variable == variable ? &*found : nullptr;
    }

    bool isChangedRead(const Value& value) const
    {
        if (value.opcode == Opcode::GetEnvironmentField)
            return !!variableOf({ value.layout(), value.field() });
        return value.opcode == Opcode::GetModuleVariable && (objectBeside(value.variable()) || findRegExp(value.variable()));
    }

    bool changesReads() const { return !movedFields.isEmpty() || !splitVariables.isEmpty() || !regExps.isEmpty(); }

    // In the order of the bodies.
    Vector<Deferral> deferrals;
    // Each ascending.
    Vector<MovedField> movedFields;
    Vector<SplitVariable> splitVariables;
    Vector<DeferredRegExp> regExps;
    // What a variable holds that only says whether its declaration has run.
    TypeID typeOfDeadZone;
};

} // anonymous namespace

static void collectMentions(const Program& program, FunctionID function, Vector<DefinitionMention>& found)
{
    constexpr OpcodeSet mentioningOpcodes { Opcode::PutModuleVariable, Opcode::NewClosure, Opcode::NewClass, Opcode::NewObject, Opcode::PutEnvironmentField };
    forEachListedName(program, function, mentioningOpcodes, [&](BlockID, ValueID value, Opcode opcode, NameKind kind, uint32_t name) {
        switch (opcode) {
        case Opcode::PutModuleVariable:
            if (kind == NameKind::Variable)
                found.append({ DefinitionMention::Kind::VariableStore, name });
            break;
        case Opcode::NewClosure:
            if (kind == NameKind::Function)
                found.append({ DefinitionMention::Kind::Closure, name });
            break;
        case Opcode::NewClass:
            if (kind == NameKind::Layout)
                found.append({ DefinitionMention::Kind::ClassDefinition, name });
            break;
        case Opcode::NewObject:
            if (kind == NameKind::Layout && program.at(LayoutID(name)).kind == LayoutKind::Environment)
                found.append({ DefinitionMention::Kind::EnvironmentAllocation, name });
            break;
        case Opcode::PutEnvironmentField:
            if (kind == NameKind::Layout)
                found.append({ DefinitionMention::Kind::EnvironmentStore, name, program.functions[function.index()].at(value).field() });
            break;
        default:
            break;
        }
    });
}

static Counts countMentions(const Program& program, const ParallelLoops& loops)
{
    Vector<DefinitionMention> mentions = collectInParallel<DefinitionMention>(loops, program.functions.size(), [&](unsigned index, Vector<DefinitionMention>& found) {
        collectMentions(program, FunctionID(index), found);
    });

    Counts counts;
    counts.storesOfVariable.fill(0, program.variables.size());
    counts.closuresOfFunction.fill(0, program.functions.size());
    counts.definitionsOfClass.fill(0, program.layouts.size());
    counts.allocationsOfEnvironment.fill(0, program.layouts.size());
    auto count = [](uint8_t& counter) {
        counter = std::min<unsigned>(counter + 1, 2);
    };
    for (const DefinitionMention& mention : mentions) {
        switch (mention.kind) {
        case DefinitionMention::Kind::VariableStore:
            count(counts.storesOfVariable[mention.subject]);
            break;
        case DefinitionMention::Kind::Closure:
            count(counts.closuresOfFunction[mention.subject]);
            break;
        case DefinitionMention::Kind::ClassDefinition:
            count(counts.definitionsOfClass[mention.subject]);
            break;
        case DefinitionMention::Kind::EnvironmentAllocation:
            count(counts.allocationsOfEnvironment[mention.subject]);
            break;
        case DefinitionMention::Kind::EnvironmentStore:
            counts.storedEnvironmentFields.append({ LayoutID(mention.subject), mention.field });
            break;
        }
    }
    parallelSort(loops, counts.storedEnvironmentFields.mutableSpan(), std::less<FieldReference> { });

    // The closure table has one cell per function, and the closure of a declaration is in it.
    for (const Variable& variable : program.variables) {
        if (variable.initialization == VariableInitialization::Function && variable.function)
            counts.closuresOfFunction[variable.function.index()] = 2;
    }
    return counts;
}

static NamedByRecords findWhatRecordsName(const Program& program)
{
    NamedByRecords named;
    named.variables.ensureSize(program.variables.size());
    auto add = [&](VariableID variable) {
        if (variable)
            named.variables.quickSet(variable.index());
    };
    for (const Module& module : program.modules) {
        for (const ModuleExport& entry : module.exports)
            add(entry.variable);
        for (const SyntheticExport& entry : module.syntheticExports)
            add(entry.variable);
    }
    for (const Layout& layout : program.layouts) {
        for (const StaticField& field : layout.staticFields)
            add(field.variable);
    }
    for (const EvalScope& scope : program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings) {
            add(binding.variable);
            if (binding.layout && binding.field)
                named.environmentFields.append({ binding.layout, *binding.field });
        }
    }
    AOT::sort(named.environmentFields);
    return named;
}

// The front end hoists nothing into the variables of a CommonJS module, whose body may run again. A variable that is stored at most once belongs to a body that does not.
static bool mayHoldVariablesBornAsFunctions(ModuleKind kind)
{
    switch (kind) {
    case ModuleKind::SourceText:
    case ModuleKind::RuntimeSource:
    case ModuleKind::CommonJs:
        return true;
    case ModuleKind::Data:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasOneStoreAtMost(const LocationFlow& flow)
{
    return flow.writers == WriterState::AllKnown && flow.stores == StoreCount::AtMostOnce;
}

static BeforeStore whatIsReadBeforeStore(const Program& program, const Variable& variable)
{
    if (variable.flow.initialization != InitializationState::Unknown)
        return BeforeStore::NothingIsRead;
    if (variable.initialization != VariableInitialization::Uninitialized || variable.declared.flags.contains(PositionFlags::IsExpected))
        return BeforeStore::SomethingElse;
    // A read has the type of the variable, so a variable whose type leaves out `absent` is never read while it is empty.
    return program.at(variable.declared.type).primitives & Primitive::Absent ? BeforeStore::DeadZone : BeforeStore::NothingIsRead;
}

FunctionAnalyses& DeferredDefinitionScanner::analyses()
{
    if (!m_analyses)
        m_analyses.emplace(AnalysisContext { m_context.build, m_program, &m_context.flow, nullptr, m_id }, m_function);
    return *m_analyses;
}

// SomethingElse where the variable needs the object itself.
BeforeStore DeferredDefinitionScanner::whatIsReadBefore(const Value& store, Deferral::Kind kind) const
{
    VariableID id = store.variable();
    const Variable& variable = m_program.at(id);
    if (m_context.counts.storesOfVariable[id.index()] != 1 || variable.initialization == VariableInitialization::Function || variable.owner || !hasOneStoreAtMost(variable.flow))
        return BeforeStore::SomethingElse;

    bool isNamedByRecord = m_context.named.variables.quickGet(id.index());
    switch (kind) {
    case Deferral::Kind::Closure:
    case Deferral::Kind::Class:
        if (!variable.module || !mayHoldVariablesBornAsFunctions(m_program.at(variable.module).kind))
            return BeforeStore::SomethingElse;
        break;
    case Deferral::Kind::RegExp:
        // An empty variable says that the object has not been made. Whoever reads it without the test for that takes it for a binding in its dead zone.
        if (isNamedByRecord || variable.declared.flags.contains(PositionFlags::IsExpected))
            return BeforeStore::SomethingElse;
        break;
    case Deferral::Kind::ClosureInEnvironment:
        RELEASE_ASSERT_NOT_REACHED();
    }

    BeforeStore found = whatIsReadBeforeStore(m_program, variable);
    // The run time takes what the variable holds for the value of the binding.
    return found == BeforeStore::DeadZone && isNamedByRecord ? BeforeStore::SomethingElse : found;
}

bool DeferredDefinitionScanner::tryTakeStore(Deferral& deferral, ValueID id) const
{
    const Value& store = m_function.at(id);
    switch (whatIsReadBefore(store, deferral.kind)) {
    case BeforeStore::NothingIsRead:
        deferral.removed.append(id);
        deferral.variables.append(store.variable());
        return true;
    case BeforeStore::DeadZone:
        deferral.storesThatEndDeadZone.append(id);
        deferral.variablesWithDeadZone.append(store.variable());
        return true;
    case BeforeStore::SomethingElse:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The allocation comes last. Nothing but the store gets hold of the object.
bool DeferredDefinitionScanner::collectStoredAllocation(ValueID stored, Vector<ValueID, 4>& values)
{
    for (ValueID id = stored;; id = m_function.at(id).operand(0)) {
        if (useLists().useCount(id) != 1)
            return false;
        values.append(id);
        if (!isCast(m_function.at(id).opcode))
            return true;
        if (!isCastWithoutTest(m_function.at(id).opcode))
            return false;
    }
}

bool DeferredDefinitionScanner::isOnlyClosureOverNothing(const Value& allocation) const
{
    return allocation.opcode == Opcode::NewClosure && allocation.capturesOperands().empty() && !allocation.isMadeOnDemand() && hasOnlyClosure(allocation.function());
}

void DeferredDefinitionScanner::scanVariableStore(ValueID id)
{
    const Value& store = m_function.at(id);
    const Value& allocation = m_function.at(sameObjectThroughCasts(m_function, store.valueOperand()));
    Deferral deferral { .body = m_id };
    if (isOnlyClosureOverNothing(allocation)) {
        deferral.kind = Deferral::Kind::Closure;
        deferral.function = allocation.function();
    } else if (allocation.opcode == Opcode::NewRegExp)
        deferral.kind = Deferral::Kind::RegExp;
    else
        return;

    if (whatIsReadBefore(store, deferral.kind) == BeforeStore::SomethingElse || !collectStoredAllocation(store.valueOperand(), deferral.removed))
        return;
    deferral.literal = deferral.removed.last();
    tryTakeStore(deferral, id);
    m_found.append(WTF::move(deferral));
}

// One environment of the layout exists at most, so a field of the layout is one place.
bool DeferredDefinitionScanner::isOnlyEnvironment(ValueID environment, LayoutID layout)
{
    if (m_context.counts.allocationsOfEnvironment[layout.index()] != 1)
        return false;
    ValueID allocation = sameObjectThroughCasts(m_function, environment);
    if (m_function.at(allocation).opcode != Opcode::NewObject || m_function.at(allocation).layout() != layout)
        return false;
    return analyses().executionCountOf(useLists().blockOf(allocation)) == ExecutionCount::AtMostOnce;
}

void DeferredDefinitionScanner::scanEnvironmentStore(ValueID id)
{
    const Value& store = m_function.at(id);
    FieldReference field { store.layout(), store.field() };
    const Layout& layout = m_program.at(field.layout);
    if (layout.kind != LayoutKind::Environment || !m_function.module || !mayHoldVariablesBornAsFunctions(m_program.at(m_function.module).kind))
        return;
    if (!isOnlyClosureOverNothing(m_function.at(sameObjectThroughCasts(m_function, store.valueOperand()))))
        return;

    const LocationFlow& location = layout.fields[field.field].flow.location;
    if (!hasOneStoreAtMost(location) || location.initialization != InitializationState::StoredBeforeEveryRead || location.deferral != ClosureDeferral::Unknown)
        return;
    if (!m_context.counts.isStoredByOneOperation(field) || m_context.named.contains(field) || !isOnlyEnvironment(store.environmentOperand(), field.layout))
        return;

    Deferral deferral { .kind = Deferral::Kind::ClosureInEnvironment, .body = m_id, .field = field };
    if (!collectStoredAllocation(store.valueOperand(), deferral.removed))
        return;
    deferral.function = m_function.at(deferral.removed.last()).function();
    deferral.removed.append(id);
    m_found.append(WTF::move(deferral));
}

// A read gives what its variable is declared to hold, and every use of the class object was typed with the type of the class object.
VariableID DeferredDefinitionScanner::findVariableOfType(const Deferral& deferral, TypeID type) const
{
    for (VariableID variable : deferral.variables) {
        const DeclaredType& declared = m_program.at(variable).declared;
        if (declared.type == type && !declared.flags.contains(PositionFlags::IsExpected))
            return variable;
    }
    for (VariableID variable : deferral.variablesWithDeadZone) {
        if (m_program.at(m_program.at(variable).declared.type).withoutPrimitives(Primitive::Absent) == m_program.at(type))
            return variable;
    }
    return { };
}

void DeferredDefinitionScanner::scanClassDefinition(ValueID id)
{
    LayoutID layoutID = m_function.at(id).layout();
    const Layout& layout = m_program.at(layoutID);
    if (m_context.counts.definitionsOfClass[layoutID.index()] != 1 || !layout.constructor || !hasOnlyClosure(layout.constructor))
        return;
    std::optional<ClassDefinitionSite> site = findClassDefinitionSite(m_program, m_function, useLists(), id);
    if (!site)
        return;
    bool everyMemberHasOnlyClosure = std::ranges::all_of(layout.classFlow.members, [&](const ClassMember& member) {
        return hasOnlyClosure(member.function);
    });
    if (!everyMemberHasOnlyClosure)
        return;

    Deferral deferral { .kind = Deferral::Kind::Class, .body = m_id, .function = layout.constructor, .layout = layoutID };
    deferral.removed.appendVector(site->describedByRecord);
    bool holdsClassObjectSomewhere = false;
    for (ValueID store : site->stores)
        holdsClassObjectSomewhere |= tryTakeStore(deferral, store);
    if (!holdsClassObjectSomewhere)
        return;

    // A value that hands the class object on goes when nothing is left that takes it.
    Vector<ValueID, 16> formerUsers;
    formerUsers.appendVector(deferral.removed);
    formerUsers.appendVector(deferral.storesThatEndDeadZone);
    AOT::sort(formerUsers);
    Vector<ValueID, 2> unused;
    auto isLeftWithoutUse = [&](ValueID holder) {
        return std::ranges::all_of(useLists().usesOf(holder), [&](const Use& use) {
            return use.kind == Use::Kind::Operand && (std::ranges::binary_search(formerUsers, use.user) || unused.contains(use.user));
        });
    };
    for (ValueID copy : site->copies.span() | std::views::reverse) {
        Opcode opcode = m_function.at(copy).opcode;
        if ((opcode == Opcode::FinishClass || isCastWithoutTest(opcode)) && isLeftWithoutUse(copy))
            unused.append(copy);
    }
    if (isLeftWithoutUse(id))
        unused.append(id);
    else {
        deferral.variableReadInBody = findVariableOfType(deferral, m_function.at(id).type);
        if (!deferral.variableReadInBody)
            return;
        deferral.classObject = id;
    }
    deferral.removed.appendVector(unused);
    m_found.append(WTF::move(deferral));
}

void DeferredDefinitionScanner::run()
{
    for (const Block& block : m_function.blocks) {
        for (ValueID id : block.values) {
            switch (m_function.at(id).opcode) {
            case Opcode::PutModuleVariable:
                scanVariableStore(id);
                break;
            case Opcode::PutEnvironmentField:
                if (m_context.flow.hasResults())
                    scanEnvironmentStore(id);
                break;
            case Opcode::NewClass:
                scanClassDefinition(id);
                break;
            default:
                break;
            }
        }
    }
}

// The front end follows each read of a binding with a dead zone by the test for it, and flow puts a cast in front of a test that is sure to pass. Whatever else takes such a read may get `absent` or the object.
static void findReadsWithoutDeadZoneTest(const Program& program, FunctionID functionID, const BitVector& hasDeadZone, Vector<VariableID>& found)
{
    const Function& function = program.functions[functionID.index()];
    auto variableReadBy = [&](ValueID id) {
        const Value& value = function.at(id);
        return value.opcode == Opcode::GetModuleVariable && hasDeadZone.quickGet(value.variable().index()) ? value.variable() : VariableID();
    };
    bool readsSuchVariable = false;
    forEachListedName(program, functionID, { Opcode::GetModuleVariable }, [&](BlockID, ValueID, Opcode, NameKind kind, uint32_t name) {
        readsSuchVariable |= kind == NameKind::Variable && hasDeadZone.quickGet(name);
    });
    if (!readsSuchVariable)
        return;

    auto noteUse = [&](ValueID operand) {
        if (VariableID variable = variableReadBy(operand))
            found.append(variable);
    };
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            bool validatesCast = value.opcode == Opcode::ValidateType && !(program.at(value.validatedType()).primitives & Primitive::Absent);
            if (!leavesDeadZoneBehind(program, function, value) && !validatesCast)
                value.forEachChild(noteUse);
        }
        block.terminator.forEachChild(noteUse);
        if (block.handler) {
            for (ValueID argument : block.handler->arguments)
                noteUse(argument);
        }
        if (block.subject)
            noteUse(block.subject);
    }
}

static void dropDeferralsWithOtherReads(const Program& program, const ParallelLoops& loops, Vector<Deferral>& deferrals)
{
    BitVector hasDeadZone(program.variables.size());
    for (const Deferral& deferral : deferrals) {
        for (VariableID variable : deferral.variablesWithDeadZone)
            hasDeadZone.quickSet(variable.index());
    }
    if (hasDeadZone.isEmpty())
        return;

    Vector<VariableID> readOtherwise = collectInParallel<VariableID>(loops, program.functions.size(), [&](unsigned index, Vector<VariableID>& found) {
        findReadsWithoutDeadZoneTest(program, FunctionID(index), hasDeadZone, found);
    });
    if (readOtherwise.isEmpty())
        return;
    for (VariableID variable : readOtherwise)
        hasDeadZone.quickClear(variable.index());
    deferrals.removeAllMatching([&](const Deferral& deferral) {
        return !std::ranges::all_of(deferral.variablesWithDeadZone, [&](VariableID variable) {
            return hasDeadZone.quickGet(variable.index());
        });
    });
}

// The run time makes the class above before the class, whenever the class is first needed. Code that defines the class above would then make a second one.
static void dropClassesBelowClassesThatCodeDefines(const Program& program, Vector<Deferral>& deferrals)
{
    BitVector isDeferred(program.layouts.size());
    for (const Deferral& deferral : deferrals) {
        if (deferral.kind == Deferral::Kind::Class)
            isDeferred.quickSet(deferral.layout.index());
    }
    if (isDeferred.isEmpty())
        return;

    for (bool hasChanged = true; hasChanged;) {
        hasChanged = false;
        for (const Deferral& deferral : deferrals) {
            if (deferral.kind != Deferral::Kind::Class || !isDeferred.quickGet(deferral.layout.index()))
                continue;
            const Layout& layout = program.at(deferral.layout);
            if (layout.flow.baseState != BaseState::Layout || isDeferred.quickGet(layout.flow.base.index()))
                continue;
            isDeferred.quickClear(deferral.layout.index());
            hasChanged = true;
        }
    }
    deferrals.removeAllMatching([&](const Deferral& deferral) {
        return deferral.kind == Deferral::Kind::Class && !isDeferred.quickGet(deferral.layout.index());
    });
}

static Variable createVariableFor(const Program& program, const Deferral& deferral)
{
    const Field& field = fieldAt(program, deferral.field);
    const LocationFlow& location = field.flow.location;
    Variable variable;
    if (field.key)
        variable.name = program.at(field.key).text;
    variable.module = program.at(deferral.body).module;
    variable.declared = field.declared;
    if (field.flags.contains(FieldFlags::IsImmutable))
        variable.flags.add(VariableFlags::IsImmutable);
    variable.flow.writers = WriterState::AllKnown;
    variable.flow.stores = StoreCount::AtMostOnce;
    variable.flow.initialization = InitializationState::StoredBeforeEveryRead;
    variable.flow.reads = location.reads;
    variable.flow.cause = location.cause;
    variable.flow.witness = location.witness;
    return variable;
}

// Every read of this one stands behind the test of the other, which passes only after the declaration has run.
static Variable createVariableBeside(ProgramEditor& editor, VariableID deadZone)
{
    const Program& program = editor.program();
    Variable variable = program.at(deadZone);
    variable.declared.type = editor.types().intern(program.at(variable.declared.type).withoutPrimitives(Primitive::Absent));
    variable.flow.initialization = InitializationState::StoredBeforeEveryRead;
    return variable;
}

static void splitVariablesWithDeadZone(ProgramEditor& editor, DefinitionPlan& plan, Deferral& deferral)
{
    for (VariableID deadZone : deferral.variablesWithDeadZone) {
        VariableID object = editor.addVariable(createVariableBeside(editor, deadZone));
        editor.at(deadZone).declared = DeclaredType { .type = plan.typeOfDeadZone };
        plan.splitVariables.append({ deadZone, object });
        deferral.variables.append(object);
        if (deferral.variableReadInBody == deadZone)
            deferral.variableReadInBody = object;
    }
}

static void changeVariables(ProgramEditor& editor, DefinitionPlan& plan)
{
    const Program& program = editor.program();
    plan.typeOfDeadZone = editor.types().intern(Type::fromPrimitives(Primitive::Absent | Primitive::Undefined));
    for (Deferral& deferral : plan.deferrals) {
        if (deferral.kind == Deferral::Kind::ClosureInEnvironment) {
            deferral.variables.append(editor.addVariable(createVariableFor(program, deferral)));
            plan.movedFields.append({ deferral.field, deferral.variables.first() });
        }
        splitVariablesWithDeadZone(editor, plan, deferral);

        for (VariableID id : deferral.variables) {
            Variable& variable = editor.at(id);
            switch (deferral.kind) {
            case Deferral::Kind::Closure:
            case Deferral::Kind::ClosureInEnvironment:
            case Deferral::Kind::Class:
                variable.initialization = VariableInitialization::Function;
                variable.function = deferral.function;
                variable.flow.stores = StoreCount::Never;
                break;
            case Deferral::Kind::RegExp:
                variable.declared.type = editor.types().intern(program.at(variable.declared.type).withPrimitives(Primitive::Absent));
                variable.initialization = VariableInitialization::Uninitialized;
                variable.flow.initialization = InitializationState::Unknown;
                variable.flow.isUnchangedDuringActivation = false;
                plan.regExps.append({ id, program.at(deferral.body).at(deferral.literal) });
                break;
            }
        }
    }
    AOT::sort(plan.movedFields, { }, &MovedField::field);
    AOT::sort(plan.splitVariables, { }, &SplitVariable::deadZone);
    AOT::sort(plan.regExps, { }, &DeferredRegExp::variable);
}

static Vector<FunctionID> findFunctionsToEdit(const Program& program, const ParallelLoops& loops, const DefinitionPlan& plan)
{
    Vector<FunctionID> functions;
    if (plan.changesReads()) {
        BitVector layoutsWithMovedFields(program.layouts.size());
        for (const MovedField& moved : plan.movedFields)
            layoutsWithMovedFields.quickSet(moved.field.layout.index());
        // Plan::isChangedRead() holds for no read whose names are refused here.
        auto mayBeChangedRead = [&](NameKind kind, uint32_t name) {
            if (kind == NameKind::Layout)
                return layoutsWithMovedFields.quickGet(name);
            return kind == NameKind::Variable && (plan.objectBeside(VariableID(name)) || plan.findRegExp(VariableID(name)));
        };
        functions = collectInParallel<FunctionID>(loops, program.functions.size(), [&](unsigned index, Vector<FunctionID>& found) {
            bool readsChangedPlace = false;
            forEachListedValueNaming(program, FunctionID(index), { Opcode::GetEnvironmentField, Opcode::GetModuleVariable }, mayBeChangedRead, [&](BlockID, ValueID, const Value& value) {
                readsChangedPlace |= plan.isChangedRead(value);
            });
            if (readsChangedPlace)
                found.append(FunctionID(index));
        });
    }
    for (const Deferral& deferral : plan.deferrals)
        functions.append(deferral.body);
    AOT::sort(functions);
    removeRepeatedElements(functions);
    return functions;
}

static void removeDefinitions(std::span<const Deferral> deferrals, IRBuilder& builder)
{
    if (deferrals.empty())
        return;
    Function& function = builder.function();
    BitVector isRemoved(function.values.size());
    BitVector endsDeadZone(function.values.size());
    for (const Deferral& deferral : deferrals) {
        for (ValueID id : deferral.removed)
            isRemoved.quickSet(id.index());
        for (ValueID id : deferral.storesThatEndDeadZone)
            endsDeadZone.quickSet(id.index());
        if (!deferral.classObject)
            continue;
        Value& classObject = function.edit(deferral.classObject);
        classObject.become(Opcode::GetModuleVariable);
        classObject.setVariable(deferral.variableReadInBody);
    }

    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        BlockValues values = std::exchange(function.edit(BlockID(index), BlockEdit::Values).values, { });
        BlockValues kept;
        kept.reserveInitialCapacity(values.size());
        for (ValueID id : values) {
            if (isRemoved.quickGet(id.index()))
                continue;
            if (endsDeadZone.quickGet(id.index())) {
                builder.setOrigin(function.at(id).origin());
                ValueID marker = builder.addDetached(IRBuilder::make(Opcode::Undefined));
                function.edit(id, ValueEdit::Definition).setValueOperand(marker);
                kept.append(marker);
            }
            kept.append(id);
        }
        function.edit(BlockID(index), BlockEdit::Values).values = WTF::move(kept);
    }
    for (size_t index : isRemoved)
        becomeUndefined(function, ValueID(index));
}

// The test stays where it is, on the variable that says whether the declaration has run. What took the result of the test takes the object. So does what took the cast that stood for a test.
static void readObjectsBehindDeadZoneTests(const DefinitionPlan& plan, IRBuilder& builder)
{
    if (plan.splitVariables.isEmpty())
        return;
    Function& function = builder.function();
    auto objectBesideReadBy = [&](ValueID id) {
        const Value& read = function.at(id);
        return read.opcode == Opcode::GetModuleVariable ? plan.objectBeside(read.variable()) : VariableID();
    };
    auto objectReadThrough = [&](ValueID id) {
        const Value& test = function.at(id);
        return test.opcode == Opcode::CheckInitialized || test.opcode == Opcode::AssumeType ? objectBesideReadBy(test.valueOperand()) : VariableID();
    };

    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        for (ValueID id : function.blocks[index].values) {
            Value& value = function.edit(id);
            if (value.opcode == Opcode::ValidateType && objectBesideReadBy(value.operand(0)))
                value.setValidatedType(TypeTable::undefined);
            if (!objectBesideReadBy(id))
                continue;
            value.type = plan.typeOfDeadZone;
            value.setExpected(TypeID());
        }
    }
    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        bool hasTest = std::ranges::any_of(function.blocks[index].values, [&](ValueID id) {
            return !!objectReadThrough(id);
        });
        if (!hasTest)
            continue;
        BlockValues values = std::exchange(function.edit(BlockID(index), BlockEdit::Values).values, { });
        BlockValues rewritten;
        rewritten.reserveInitialCapacity(values.size() + 1);
        for (ValueID id : values) {
            if (VariableID object = objectReadThrough(id)) {
                if (function.at(id).opcode == Opcode::CheckInitialized) {
                    Value test = function.at(id);
                    test.setExpected(TypeID());
                    builder.setOrigin(test.origin());
                    rewritten.append(builder.addDetached(WTF::move(test)));
                }

                Value& read = function.edit(id);
                read.become(Opcode::GetModuleVariable);
                read.setVariable(object);
                ASSERT(plan.findRegExp(object) || read.type == builder.context().program.at(object).declared.type);
            }
            rewritten.append(id);
        }
        function.edit(BlockID(index), BlockEdit::Values).values = WTF::move(rewritten);
    }
}

static void readMovedFieldsFromVariables(const DefinitionPlan& plan, Function& function)
{
    if (plan.movedFields.isEmpty())
        return;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            Value& read = function.edit(id);
            if (read.opcode != Opcode::GetEnvironmentField)
                continue;
            VariableID variable = plan.variableOf({ read.layout(), read.field() });
            if (!variable)
                continue;
            read.become(Opcode::GetModuleVariable);
            read.setVariable(variable);
        }
    }
}

// The casts between the literal and its store went with the definition. What flow proved of the variable's object it proved of this one.
static ValueID assumeTypeOfVariable(IRBuilder& builder, ValueID made, VariableID variable)
{
    const Program& program = builder.context().program;
    const DeclaredType& declared = program.at(variable).declared;
    TypeID type = builder.function().at(made).type;
    if (declared.flags.contains(PositionFlags::IsExpected) || builder.types().at(type).isSubsetOf(builder.types().at(declared.type), program))
        return made;

    TypeID castType = builder.types().intersectionOf(type, declared.type);
    if (builder.context().build.validates())
        builder.append(makeValidateType(made, castType));
    return builder.append(makeAssumeType(made, castType));
}

static void fillAtRead(const DeferredRegExp& deferred, IRBuilder& builder, ProgramPoint point)
{
    IRBuilder::Expansion expansion = builder.beginExpansion(point);
    Value read = IRBuilder::make(Opcode::GetModuleVariable);
    read.setVariable(deferred.variable);
    ValueID held = builder.append(WTF::move(read));
    IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.appendTypeTest(held, TypeTable::absent), 0, BlockFrequency::Rare, BlockFrequency::Normal);

    ValueID made = assumeTypeOfVariable(builder, builder.append(Value(deferred.literal)), deferred.variable);
    Value store = IRBuilder::make(Opcode::PutModuleVariable, { made });
    store.setVariable(deferred.variable);
    builder.append(WTF::move(store));
    builder.endExpansion(expansion, made);

    builder.moveToEndOf(branch.notTaken);
    builder.endExpansion(expansion, branch.provenOnNotTaken);
}

static void fillRegExpVariablesAtReads(const DefinitionPlan& plan, IRBuilder& builder)
{
    if (plan.regExps.isEmpty())
        return;
    Function& function = builder.function();
    uint32_t numberOfBlocks = function.blocks.size();
    Vector<std::pair<uint32_t, const DeferredRegExp*>, 8> reads;
    for (uint32_t blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
        BlockID block(blockIndex);
        reads.shrink(0);
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& value = function.at(values[index]);
            const DeferredRegExp* deferred = value.opcode == Opcode::GetModuleVariable ? plan.findRegExp(value.variable()) : nullptr;
            if (deferred)
                reads.append({ index, deferred });
        }
        // What follows a read moves to another block. What precedes it stays where it is.
        for (auto [index, deferred] : reads.span() | std::views::reverse)
            fillAtRead(*deferred, builder, { block, index });
    }
}

static uint64_t numberOf(const DefinitionPlan& plan, Deferral::Kind kind)
{
    return std::ranges::count(plan.deferrals, kind, &Deferral::kind);
}

void deferDefinitions(ProgramEditor& editor, const FlowResults& flow, BuildContext& build)
{
    const Program& program = editor.program();
    const ParallelLoops& loops = build.parallelLoops();
    Counts counts = countMentions(program, loops);
    NamedByRecords named = findWhatRecordsName(program);
    DefinitionScanContext context { program, flow, build, counts, named };

    DefinitionPlan plan;
    plan.deferrals = collectInParallel<Deferral>(loops, program.functions.size(), [&](unsigned index, Vector<Deferral>& found) {
        constexpr OpcodeSet definingOpcodes { Opcode::PutModuleVariable, Opcode::PutEnvironmentField, Opcode::NewClass };
        if (mayListValueWith(program, FunctionID(index), definingOpcodes) && program.functions[index].hasBody() && build.isEnabled(PassID::DeferDefinitions, FunctionID(index)))
            DeferredDefinitionScanner(context, FunctionID(index), found).run();
    });
    dropDeferralsWithOtherReads(program, loops, plan.deferrals);
    dropClassesBelowClassesThatCodeDefines(program, plan.deferrals);
    if (plan.deferrals.isEmpty())
        return;

    changeVariables(editor, plan);
    Vector<FunctionID> functions = findFunctionsToEdit(program, loops, plan);
    editFunctionsInParallel(editor, build, functions.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        IRBuilder builder(AnalysisContext { build, program, &flow, nullptr, id }, function, arenas.types);
        removeDefinitions(plan.deferralsIn(id), builder);
        readObjectsBehindDeadZoneTests(plan, builder);
        readMovedFieldsFromVariables(plan, function);
        fillRegExpVariablesAtReads(plan, builder);
    });

    Statistics& statistics = build.statistics();
    statistics.count(StatisticID::ClosuresDeferredInVariables, numberOf(plan, Deferral::Kind::Closure));
    statistics.count(StatisticID::ClosuresDeferredInEnvironmentFields, numberOf(plan, Deferral::Kind::ClosureInEnvironment));
    statistics.count(StatisticID::ClassesDeferredInVariables, numberOf(plan, Deferral::Kind::Class));
    statistics.count(StatisticID::RegExpsDeferredInVariables, numberOf(plan, Deferral::Kind::RegExp));
    statistics.count(StatisticID::DeadZonesKeptBesideDefinitions, plan.splitVariables.size());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
