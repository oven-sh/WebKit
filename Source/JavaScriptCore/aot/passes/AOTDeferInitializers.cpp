/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTDeferInitializers.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinCalls.h"
#include "AOTBuiltinTable.h"
#include "AOTEdgeResults.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowApply.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTIR.h"
#include "AOTIRBuilder.h"
#include "AOTInductionVariables.h"
#include "AOTInterningPhases.h"
#include "AOTPass.h"
#include "AOTPassRunner.h"
#include "AOTPhaseTimes.h"
#include "AOTProgramCaches.h"
#include "AOTSlotPlan.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <memory>
#include <optional>
#include <ranges>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/DataLog.h>
#include <wtf/HashMap.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// What changes the result of no operation.
constexpr AbstractHeapSet heapsNobodyObserves = bookkeepingHeaps | AbstractHeapSet { AbstractHeap::StringStorage, AbstractHeap::ValidationState };
// An operand that the initializer has not made is looked at, kept and handed on. Nothing is read out of it, since it may have changed by the time of the first read.
constexpr OperandUses usesOfObjectsMadeElsewhere { OperandUse::Inspects, OperandUse::Retains, OperandUse::Returns };

// What a variable that is filled at its first read holds until then, and from when on.
enum class Emptiness : uint8_t {
    None,
    // No read comes before the store: it is born empty.
    AbsentFromBirth,
    // A read before the store finds undefined. Where the store stood, `absent` is stored.
    AbsentFromDeclaration,
    // A read before the store finds the dead zone, which is `absent`. Where the store stood, undefined is stored, which the one store of the variable never stores.
    UndefinedFromDeclaration,
};

struct VariableFacts {
    bool mayStayEmpty(VariableID id) const { return emptiness[id.index()] != Emptiness::None; }

    // Every writer is known, and one operation of the program stores to it, at most once.
    BitVector isStoredOnce;
    // Beyond that, no read comes before the store.
    BitVector holdsOneValue;
    // It holds one value, an object that never changes once it is made.
    BitVector holdsUnchangingObject;
    // The body of its module stores it once, on every way to its end, and nothing else stores it: once the body has run, it holds its value for good.
    BitVector isSetWhenItsModuleHasRun;
    // What is stored to it is neither undefined nor null.
    BitVector isStoredSomething;
    // Nothing but compiled reads finds it.
    Vector<Emptiness> emptiness;
    // Layout and field, ascending: the fields that an operation stores to by name.
    Vector<uint64_t> fieldsStoredByName;

    bool isStoredByName(LayoutID layout, uint32_t field) const { return std::ranges::binary_search(fieldsStoredByName, static_cast<uint64_t>(layout.index()) << 32 | field); }
};

struct ScanContext {
    // Whether the body of the one module has run to its end when the body of the other begins: the other imports it, directly or not, and no circle of imports holds both.
    // What a module requires runs where the `require` stands, if it runs at all.
    bool hasRunBefore(ModuleID finished, ModuleID beginning, BitVector& importedByBeginning) const;

    const Program& program;
    const OptimizationInputs& inputs;
    const FlowResults& flow;
    const EffectSummaries& effects;
    const PassRunner& runner;
    BuildContext& build;
    const VariableFacts& variables;
    const StronglyConnectedComponents& circlesOfImports;
    // By function: the module that has it as its body. One file may hold the bodies of several modules, and all that is written in a file names the record of the file.
    const Vector<ModuleID>& moduleWithBody;
};

// A stretch of a body that runs once: one operation of a block that every run passes, or all that lies between two such blocks.
struct Item {
    BlockID block;
    // The position of the operation. The number of values of the block for what follows its last operation.
    uint32_t index { 0 };
    ValueID value;
    BlockID join;
    Vector<BlockID, 0> blocks;

    bool isRegion() const { return !value; }
};

struct ValueOfVariable {
    ValueID value;
    VariableID variable;
};

// What is put off together: the operations that make, fill and store what some variables hold. The first read of any of the variables runs all of it.
struct Unit {
    FunctionID body;
    Vector<Item> items;
    Vector<VariableID, 1> variables;
    // What the items take from outside: what gives the same whenever it is done, which is done again, and what other variables hold, which is read.
    Vector<ValueID, 4> repeated;
    Vector<ValueOfVariable, 2> valuesOfVariables;
    // What other units take of this one and no variable of the program holds. Each such value gets a variable of its own.
    Vector<ValueOfVariable, 0> addedVariables;
    // The operations that make those values stay in the body, which stores them where it makes them.
    bool staysInBody { false };
    FunctionID function;
    // What the function returns: what the only variable holds, or undefined.
    TypeID type;
};

struct CalleeAnswer {
    bool mayMove { false };
    bool returnsWhatItMade { false };
    // The answer holds where the bodies of the modules of these variables have run.
    Vector<VariableID, 0> variablesTakenToBeSet;
};

// What is known of the values of a function while an initializer runs.
struct Knowledge {
    bool mayReadFrom(ValueID id) const { return made.get(id.index()) || unchanging.get(id.index()); }

    // An object that was made since the initializer began. Nothing outside has it yet.
    BitVector made;
    // An object that never changes once it is made: what is read out of it is the same whenever it is read.
    BitVector unchanging;
};

// What a call hands to the entry parameters of its callee.
struct CallContext {
    uint64_t made { 0 };
    uint64_t unchanging { 0 };
    Vector<TypeID, 8> types;

    friend bool operator==(const CallContext&, const CallContext&) = default;
};

// Says of an operation whether anybody can tell when it ran. An operation may do as it likes with an object that the initializer has made.
class MovableCode {
public:
    MovableCode(const ScanContext& context, TaskArenas& arenas)
        : m_context(context)
        , m_program(context.program)
        , m_arenas(arenas)
    {
    }

    Knowledge findKnowledge(FunctionAnalyses&, const CallContext&, const BitVector* countAsMadeElsewhere = nullptr);
    // The blocks of loops of which nothing says that they end.
    BitVector findBlocksThatMayRunForever(FunctionAnalyses&);
    bool terminatorMayMove(FunctionAnalyses&, BlockID);
    bool mayMove(FunctionAnalyses&, const Knowledge&, ValueID);
    CalleeAnswer answerFor(FunctionID, const CallContext&);
    // The variables that what was asked about since the last call of this has taken to be set.
    Vector<VariableID, 4> takeVariablesTakenToBeSet() { return std::exchange(m_variablesTakenToBeSet, { }); }

private:
    static constexpr unsigned maxDepth = 12;
    static constexpr unsigned maxValuesOfCallee = 4000;
    static constexpr unsigned maxAnswers = 4000;
    static constexpr size_t maxValuesInKeptVersions = 1 << 16;

    // What a call has made and what never changes decide the answer. The version comes from the types alone.
    struct KeptVersion {
        Vector<TypeID, 8> types;
        std::unique_ptr<const Function> version;
    };

    const Type& typeOf(const Function& function, ValueID id) const { return m_arenas.types.at(function.at(id).type); }
    bool isText(PropertyKeyID, ASCIILiteral) const;
    bool isMadeOrNoReference(const Function&, const Knowledge&, ValueID) const;
    bool isPrimitiveKey(const Function&, ValueID) const;
    bool becomesStringWithoutCode(const Function&, ValueID) const;
    void narrowElementsThatAreThere(FunctionID, Function&);
    bool hasOnlyPlainObjects(const Type&) const;
    bool isDictionaryFilledHere(FunctionAnalyses&, const Knowledge&, ValueID) const;
    bool keepsOperandsInResultOrWhatWasMade(const Function&, const Knowledge&, const BuiltinOverload&, const BuiltinCall&) const;
    bool hasOnlyFunctions(const Type&) const;
    bool hasOnlyFunctionsWithoutClass(const Type&) const;
    bool hasOnlyClassesAndBuiltins(const Type&) const;
    bool hasOnlyPristineArrays(const Type&) const;
    bool hasOnlyInstancesOf(const Type&, BuiltinClassID) const;
    bool isPristineArray(const Function&, const Knowledge&, ValueID) const;
    std::optional<bool> knownCondition(const Function&, ValueID) const;
    bool dependsOnOperandsAlone(const BuiltinOverload&) const;
    bool isCollectionAsMade(const Function&, const Knowledge&, ValueID) const;
    bool matchMayMove(const Function&, const Knowledge&, const BuiltinOverload&, const BuiltinCall&) const;
    bool freezeMayMove(FunctionAnalyses&, const Knowledge&, ValueID call, ValueID frozen) const;
    bool throwsOnlyForOperandsOfOtherTypes(const Function&, const Knowledge&, const BuiltinOverload&, const BuiltinCall&) const;
    bool hasOnlyObjectsThatNeverChange(const Type&) const;
    bool isKnownToStayPlain(const Type&) const;
    bool findsFieldThatHoldsOneValue(const Type&, PropertyKeyID) const;
    bool isOnlyFilledHere(FunctionAnalyses&, ValueID object) const;
    ValueID allocationOf(const Function&, ValueID) const;
    TypeID withConstantsWidened(TypeID) const;
    std::optional<CallContext> contextOf(const Function& caller, const Knowledge&, const Value& call, const Function& callee) const;
    std::optional<CalleeAnswer> answerForCall(const Function&, const Knowledge&, const Value& call);
    std::optional<ValueID> operandReturnedBy(const Function&, const Value&) const;
    std::optional<Function> specialize(FunctionID, const CallContext&);
    const Function* versionFor(FunctionID, const CallContext&, std::unique_ptr<const Function>& versionNotKept);
    CalleeAnswer answerForBody(FunctionID, const Function&, const CallContext&);

    bool storeMayMove(FunctionAnalyses&, const Knowledge&, const Value&);
    bool readMayMove(FunctionAnalyses&, const Knowledge&, const Value&);
    bool definitionByDescriptorMayMove(FunctionAnalyses&, const Knowledge&, const Value&);
    bool builtinCallMayMove(FunctionAnalyses&, const Knowledge&, ValueID);
    bool mayMoveByEffects(FunctionAnalyses&, const Knowledge&, ValueID);

    const ScanContext& m_context;
    const Program& m_program;
    TaskArenas& m_arenas;
    HashMap<uint32_t, Vector<std::pair<CallContext, CalleeAnswer>, 1>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_answers;
    Vector<std::pair<uint32_t, CallContext>, 8> m_beingAnswered;
    unsigned m_numberOfAnswers { 0 };
    HashMap<uint32_t, Vector<KeptVersion, 1>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_versions;
    size_t m_numberOfValuesInKeptVersions { 0 };
    Vector<VariableID, 4> m_variablesTakenToBeSet;

};

class BodyScanner {
public:
    BodyScanner(const ScanContext& context, FunctionID id, TaskArenas& arenas, Vector<Unit>& found)
        : m_context(context)
        , m_program(context.program)
        , m_id(id)
        , m_function(context.program.at(id))
        , m_found(found)
        , m_analyses(AnalysisContext { context.build, context.program, &context.flow, &context.effects, id }, m_function)
        , m_code(context, arenas)
    {
    }

    void run();

private:
    static constexpr uint32_t noItem = UINT32_MAX;
    enum class State : uint8_t { Stays, IsRepeated, Moves };

    const UseLists& useLists() { return m_analyses.useLists(); }

    bool findItems();
    void noteWhatVariablesHold();
    void noteWhatAddedVariablesMayHold();
    bool isHeldByAddedVariable(ValueID id) const { return m_variableHolding[id.index()] && m_variableHolding[id.index()].index() >= m_program.variables.size(); }
    bool givesTheSameWheneverDone(ValueID) const;
    void forEachValueTakenBy(const Item&, NOESCAPE const Invocable<void(ValueID)> auto&);
    bool mayMove(const Item&, const Knowledge&);
    bool areSetInFrontOf(uint32_t item, std::span<const VariableID>);
    bool runsCompiledCode(const Item&);
    unsigned estimatedCostOf(const Item&) const;
    void classify(uint32_t);
    void keepUnitsNotWorthPuttingOff();
    void keepUnitsNeededAtStart();
    void keepUnitsThatOtherCodeMaySeeHalfDone();
    void collectUnits();

    uint32_t find(uint32_t);
    void unite(uint32_t, uint32_t);
    void keep(uint32_t item);

    const ScanContext& m_context;
    const Program& m_program;
    FunctionID m_id;
    const Function& m_function;
    Vector<Unit>& m_found;
    FunctionAnalyses m_analyses;
    MovableCode m_code;

    Vector<Item> m_items;
    Vector<uint32_t> m_itemOf;
    Vector<State> m_states;
    Vector<uint32_t> m_parents;
    // By the representative of a unit.
    BitVector m_isKept;
    Vector<VariableID> m_variableHolding;
    Vector<uint32_t> m_storeOfHeldValue;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_firstStoreTo;
    // The variables that may stay empty and that this body stores to.
    BitVector m_isStoredHere;
    Knowledge m_known;
    Knowledge m_knownIfVariablesHoldWhatWasMadeElsewhere;
    BitVector m_mayRunForever;
    BitVector m_modulesImported;
    // Items that take what a variable holds without looking into it: they read the variable.
    struct Reading {
        uint32_t item;
        ValueID value;
    };
    Vector<Reading> m_readings;
    Vector<uint32_t> m_itemsUsedByCodeThatStays;
};

struct Plan {
    const Unit* find(VariableID variable) const LIFETIME_BOUND
    {
        auto found = std::ranges::lower_bound(byVariable, variable, { }, &std::pair<VariableID, uint32_t>::first);
        return found != byVariable.end() && found->first == variable ? &units[found->second] : nullptr;
    }

    std::span<const Unit> unitsIn(FunctionID body) const LIFETIME_BOUND
    {
        auto range = std::ranges::equal_range(units, body, { }, &Unit::body);
        return units.subspan(range.begin() - units.begin(), range.size());
    }

    Emptiness emptinessOf(VariableID variable) const { return variable.index() < emptiness->size() ? (*emptiness)[variable.index()] : Emptiness::AbsentFromBirth; }

    // In the order of the bodies.
    Vector<Unit> units;
    Vector<std::pair<VariableID, uint32_t>> byVariable;
    const Vector<Emptiness>* emptiness { nullptr };
};

} // anonymous namespace

bool ScanContext::hasRunBefore(ModuleID finished, ModuleID beginning, BitVector& importedByBeginning) const
{
    if (circlesOfImports.componentOf[finished.index()] == circlesOfImports.componentOf[beginning.index()])
        return false;
    if (importedByBeginning.isEmpty()) {
        importedByBeginning.ensureSize(program.modules.size());
        Vector<ModuleID, 32> worklist { beginning };
        importedByBeginning.quickSet(beginning.index());
        while (!worklist.isEmpty()) {
            for (const ModuleRequest& request : program.at(worklist.takeLast()).requests) {
                if (request.kind == ModuleRequestKind::Import && request.phase == ImportPhase::Evaluation && !importedByBeginning.quickSet(request.target.index()))
                    worklist.append(request.target);
            }
        }
    }
    return importedByBeginning.quickGet(finished.index());
}

static constexpr OptionSet<ObjectMutation> mutationsOfContents {
    ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript, ObjectMutation::DeleteField, ObjectMutation::RedefineProperty, ObjectMutation::AddProperty,
    ObjectMutation::ChangePrototype, ObjectMutation::StoreAfterCreation,
};

// The body of either kind runs at most once.
static bool isProgramFile(ModuleKind kind)
{
    return kind == ModuleKind::SourceText || kind == ModuleKind::CommonJs;
}

static Vector<ModuleID> findModulesWithBodies(const Program& program)
{
    Vector<ModuleID> moduleWithBody(program.functions.size());
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        if (FunctionID body = program.modules[index].body)
            moduleWithBody[body.index()] = ModuleID(index);
    }
    return moduleWithBody;
}

static VariableFacts collectVariableFacts(const Program& program, const FlowResults& flow, const Vector<ModuleID>& moduleWithBody, BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    BitVector isStoredUnchangingObject(program.variables.size());
    BitVector mayBeStoredUndefined(program.variables.size());
    BitVector mayBeStoredNothing(program.variables.size());
    BitVector isStoredOnEveryWayThroughItsModule(program.variables.size());
    Lock lock;
    Vector<VariableID> stores = collectInParallel<VariableID>(loops, program.functions.size(), [&](unsigned index, Vector<VariableID>& found) {
        if (!mayListValueWith(program, FunctionID(index), { Opcode::PutModuleVariable }))
            return;
        const Function& function = program.functions[index];
        ModuleID evaluated = moduleWithBody[index];
        bool isBodyOfModule = evaluated && isProgramFile(program.at(evaluated).kind) && function.hasBody();
        // The blocks that every run passes lie on the way up the dominator tree from the one block that returns.
        BitVector isPassedByEveryRun;
        if (isBodyOfModule) {
            FunctionAnalyses analyses(AnalysisContext { build, program, &flow, nullptr, FunctionID(index) }, function);
            BlockID last;
            bool hasOneEnd = true;
            for (BlockID block : analyses.cfg().reversePostOrder()) {
                if (!returnsToCaller(function.at(block).terminator.kind))
                    continue;
                hasOneEnd &= !last;
                last = block;
            }
            if (last && hasOneEnd) {
                for (BlockID block = last;; block = analyses.dominators().idom(block)) {
                    isPassedByEveryRun.set(block.index());
                    if (block == entryBlock)
                        break;
                }
            }
        }
        forEachListedValue(program, FunctionID(index), { Opcode::PutModuleVariable }, [&](BlockID block, uint32_t, ValueID, const Value& store) {
            found.append(store.variable());
            if (isPassedByEveryRun.get(block.index()) && program.at(store.variable()).module == evaluated) {
                Locker locker { lock };
                isStoredOnEveryWayThroughItsModule.set(store.variable().index());
            }
            if (program.typeOf(function, store.valueOperand()).primitives & Primitive::Undefined) {
                Locker locker { lock };
                mayBeStoredUndefined.set(store.variable().index());
            }
            if (program.typeOf(function, store.valueOperand()).primitives & (Primitive::Undefined | Primitive::Null | Primitive::Absent)) {
                Locker locker { lock };
                mayBeStoredNothing.set(store.variable().index());
            }
            const Value& stored = function.at(sameObjectThroughCasts(function, store.valueOperand()));
            bool isAllocation = interpretedOf(stored.opcode) == Interpretation::Allocation && Value::opcodesWithAllocationSite.contains(stored.opcode) && stored.allocationSite();
            if (isAllocation && (flow.objectsNeverChange(stored.allocationSite()) || flow.classOf(stored.allocationSite()).impossibleMutations.containsAll(mutationsOfContents))) {
                Locker locker { lock };
                isStoredUnchangingObject.set(store.variable().index());
            }
        });
    });
    Vector<uint64_t> fieldsStoredByName = collectInParallel<uint64_t>(loops, program.functions.size(), [&](unsigned index, Vector<uint64_t>& found) {
        const Function& function = program.functions[index];
        forEachListedValue(program, FunctionID(index), { Opcode::PropSet, Opcode::PropDefine, Opcode::PutSlot, Opcode::PutSlotOrAdd }, [&](BlockID, uint32_t, ValueID, const Value& value) {
            auto note = [&](LayoutID layout) {
                if (std::optional<uint32_t> field = program.at(layout).findField(value.key()))
                    found.append(static_cast<uint64_t>(layout.index()) << 32 | *field);
            };
            if (value.receivers()) {
                for (LayoutID layout : program.at(value.receivers()).members)
                    note(layout);
            }
            for (LayoutID layout : membersOf(program, program.typeOf(function, value.objectOperand())))
                note(layout);
        });
    });
    std::ranges::sort(fieldsStoredByName);
    removeRepeatedElements(fieldsStoredByName);

    Vector<uint8_t> numberOfStores;
    numberOfStores.fill(0, program.variables.size());
    for (VariableID variable : stores)
        numberOfStores[variable.index()] = std::min<unsigned>(numberOfStores[variable.index()] + 1, 2);

    // The run time finds these by a record and takes an empty one to be in its dead zone.
    BitVector isNamedByRecord = findVariablesDescribedToRunTime(program, flow);

    VariableFacts facts;
    facts.fieldsStoredByName = WTF::move(fieldsStoredByName);
    facts.isStoredOnce.ensureSize(program.variables.size());
    facts.holdsOneValue.ensureSize(program.variables.size());
    facts.holdsUnchangingObject.ensureSize(program.variables.size());
    facts.isSetWhenItsModuleHasRun.ensureSize(program.variables.size());
    facts.isStoredSomething.ensureSize(program.variables.size());
    facts.emptiness.fill(Emptiness::None, program.variables.size());
    for (size_t index = 0; index < program.variables.size(); ++index) {
        const Variable& variable = program.variables[index];
        if (variable.flow.writers != WriterState::AllKnown)
            continue;
        if (variable.owner)
            continue;
        const Type& type = program.at(variable.declared.type);
        bool isExpected = variable.declared.flags.contains(PositionFlags::IsExpected);
        bool isNeverReadEmpty = variable.flow.initialization != InitializationState::Unknown || (!isExpected && variable.initialization == VariableInitialization::Uninitialized && !(type.primitives & Primitive::Absent));
        if (variable.initialization == VariableInitialization::Function) {
            if ((isNeverReadEmpty || !(type.primitives & Primitive::Absent)) && variable.flow.stores == StoreCount::Never && !numberOfStores[index])
                facts.holdsOneValue.quickSet(index);
            continue;
        }
        if (variable.flow.stores != StoreCount::AtMostOnce || numberOfStores[index] != 1)
            continue;
        facts.isStoredOnce.quickSet(index);
        if (!mayBeStoredNothing.get(index))
            facts.isStoredSomething.quickSet(index);
        if (isStoredOnEveryWayThroughItsModule.get(index))
            facts.isSetWhenItsModuleHasRun.quickSet(index);
        bool isStoredBeforeEveryRead = isNeverReadEmpty && variable.flow.initialization != InitializationState::SetAtCreation;
        if (isStoredBeforeEveryRead) {
            facts.holdsOneValue.quickSet(index);
            if (isStoredUnchangingObject.get(index))
                facts.holdsUnchangingObject.quickSet(index);
        }
        if (isNamedByRecord.quickGet(index) || !variable.module)
            continue;
        if (!type.isJavaScriptValue())
            continue;
        if (isStoredBeforeEveryRead)
            facts.emptiness[index] = Emptiness::AbsentFromBirth;
        else if (variable.initialization == VariableInitialization::Undefined)
            facts.emptiness[index] = Emptiness::AbsentFromDeclaration;
        else if (!mayBeStoredUndefined.get(index))
            facts.emptiness[index] = Emptiness::UndefinedFromDeclaration;
    }
    return facts;
}

bool MovableCode::isText(PropertyKeyID key, ASCIILiteral text) const
{
    const PropertyKey& record = m_program.at(key);
    return record.text && spanHasPrefix(m_program.string(record.text), text.span8()) && m_program.string(record.text).size() == text.length();
}

bool MovableCode::isMadeOrNoReference(const Function& function, const Knowledge& known, ValueID id) const
{
    return known.made.get(id.index()) || !mayBeReference(typeOf(function, id));
}

// To make a key of it runs no code.
bool MovableCode::isPrimitiveKey(const Function& function, ValueID id) const
{
    const Type& type = typeOf(function, id);
    return !type.hasObjects() && type.isJavaScriptValue();
}

// Neither an object, whose conversion runs code, nor a symbol, whose conversion throws. What is left to throw is a string too long for memory.
bool MovableCode::becomesStringWithoutCode(const Function& function, ValueID id) const
{
    const Type& type = typeOf(function, id);
    return !type.hasObjects() && type.isJavaScriptValue() && !(type.primitives & (Primitive::AnySymbol | Primitive::Absent));
}

static bool isClassConstructor(const Function& function)
{
    return function.kind == FunctionKind::BaseConstructor || function.kind == FunctionKind::DerivedConstructor;
}

// Objects that a literal or an enum makes.
bool MovableCode::hasOnlyPlainObjects(const Type& type) const
{
    if (!type.isObject() || type.anyObject || type.anyFunction || !type.functions.isEmpty() || !type.builtins.isEmpty() || !type.classes.isEmpty() || !type.hasOnlyLayouts(m_program))
        return false;
    return std::ranges::all_of(membersOf(m_program, type), [&](LayoutID id) {
        const Layout& layout = m_program.at(id);
        return (layout.kind == LayoutKind::Record || layout.kind == LayoutKind::Container) && !layout.base;
    });
}

bool MovableCode::hasOnlyFunctions(const Type& type) const
{
    return type.isObject() && !type.anyObject && !type.anyFunction && !type.layouts && type.builtins.isEmpty() && type.classes.isEmpty() && !type.functions.isEmpty();
}

bool MovableCode::hasOnlyFunctionsWithoutClass(const Type& type) const
{
    return hasOnlyFunctions(type) && std::ranges::none_of(type.functions, [&](FunctionID id) {
        return isClassConstructor(m_program.at(id));
    });
}

// The prototype property of a class and of a built-in constructor can neither be stored to nor redefined.
bool MovableCode::hasOnlyClassesAndBuiltins(const Type& type) const
{
    if (type.anyObject || type.anyFunction || type.layouts || !type.classes.isEmpty() || (type.primitives & ~Primitive::Null) || !type.isJavaScriptValue())
        return false;
    bool hasOnlyConstructors = std::ranges::all_of(type.builtins, [&](BuiltinID id) {
        return m_program.builtins().at(id).kind == BuiltinKind::Constructor;
    });
    return hasOnlyConstructors && std::ranges::all_of(type.functions, [&](FunctionID id) {
        return isClassConstructor(m_program.at(id));
    });
}

bool MovableCode::hasOnlyPristineArrays(const Type& type) const
{
    if (!type.isObject() || type.anyObject || type.anyFunction || type.layouts || !type.functions.isEmpty() || !type.builtins.isEmpty() || type.classes.isEmpty())
        return false;
    return std::ranges::all_of(type.classes, [](const ClassPart& part) {
        return part.builtinClass == BuiltinClasses::Array && part.provenPristine.containsAll({ PristineAspects::Storage, PristineAspects::Names });
    });
}

bool MovableCode::hasOnlyInstancesOf(const Type& type, BuiltinClassID builtinClass) const
{
    if (!type.isObject() || type.anyObject || type.anyFunction || type.layouts || !type.functions.isEmpty() || !type.builtins.isEmpty() || type.classes.isEmpty())
        return false;
    return std::ranges::all_of(type.classes, [&](const ClassPart& part) {
        return part.builtinClass == builtinClass;
    });
}

// All that was done to an array since the initializer made it has passed these rules, and none of them lets an index become an accessor or the prototype change.
bool MovableCode::isPristineArray(const Function& function, const Knowledge& known, ValueID id) const
{
    const Type& type = typeOf(function, id);
    return hasOnlyPristineArrays(type) || (known.made.get(id.index()) && hasOnlyInstancesOf(type, BuiltinClasses::Array));
}

// A Set or a Map whose iteration runs the built-in iterators, which cannot be replaced.
bool MovableCode::isCollectionAsMade(const Function& function, const Knowledge& known, ValueID id) const
{
    const Type& type = typeOf(function, id);
    if (!hasOnlyInstancesOf(type, BuiltinClasses::Set) && !hasOnlyInstancesOf(type, BuiltinClasses::Map))
        return false;
    return known.made.get(id.index()) || std::ranges::all_of(type.classes, [](const ClassPart& part) {
        return part.provenPristine.containsAll({ PristineAspects::Storage, PristineAspects::Names });
    });
}

std::optional<bool> MovableCode::knownCondition(const Function& function, ValueID id) const
{
    const Value& value = function.at(definitionThroughUpcasts(function, id));
    // The engine's own code counts elements that it has stored, and memory ends long before 2 ** 53 - 1 of them.
    if (value.opcode == Opcode::GreaterEq && function.role == FunctionRole::Library) {
        const Value& limit = function.at(definitionThroughUpcasts(function, value.operand(1)));
        if (limit.opcode == Opcode::Integer && limit.integer() == (int64_t { 1 } << 53) - 1 && typeOf(function, value.operand(0)).isNumberWithoutFraction())
            return false;
    }
    if (value.opcode != Opcode::CallBuiltin)
        return std::nullopt;
    BuiltinCall call = builtinCallOf(m_program, value);
    if (m_program.builtins().at(call.overload).builtin != Builtins::ArrayIsArray || call.arguments.size() != 1)
        return std::nullopt;
    const Type& type = typeOf(function, call.arguments[0]);
    if (hasOnlyInstancesOf(type, BuiltinClasses::Array))
        return true;
    if (!type.hasObjects())
        return false;
    return std::nullopt;
}

bool MovableCode::hasOnlyObjectsThatNeverChange(const Type& type) const
{
    if (!type.hasObjects() || type.anyObject || type.anyFunction || !type.functions.isEmpty() || !type.builtins.isEmpty() || !type.classes.isEmpty() || !type.hasOnlyLayouts(m_program))
        return false;
    return std::ranges::all_of(membersOf(m_program, type), [&](LayoutID id) {
        return m_program.at(id).flow.impossibleMutations.containsAll(mutationsOfContents);
    });
}

// No such object ever gets an accessor, a lock or another prototype: above it stands Object.prototype, which nobody can change.
bool MovableCode::isKnownToStayPlain(const Type& type) const
{
    constexpr OptionSet<ObjectMutation> mutations { ObjectMutation::ChangePrototype, ObjectMutation::ChangeIntegrity, ObjectMutation::RedefineProperty };
    return std::ranges::all_of(membersOf(m_program, type), [&](LayoutID id) {
        const Layout& layout = m_program.at(id);
        return layout.flow.impossibleMutations.containsAll(mutations) && layout.flow.ownAccessorsAreComplete && layout.flow.ownAccessors.isEmpty() && std::ranges::none_of(layout.fields, [](const Field& field) {
            return field.form.containsAny({ FieldForm::IsAccessor, FieldForm::IsReadOnly, FieldForm::IsNotConfigurable });
        });
    });
}

// Every object of the type has the field, which is stored once, in front of every read, and is neither deleted nor redefined.
bool MovableCode::findsFieldThatHoldsOneValue(const Type& type, PropertyKeyID key) const
{
    if (!type.isObject() || type.anyObject || type.anyFunction || !type.functions.isEmpty() || !type.builtins.isEmpty() || !type.classes.isEmpty() || !type.hasOnlyLayouts(m_program))
        return false;
    return std::ranges::all_of(membersOf(m_program, type), [&](LayoutID id) {
        const Layout& layout = m_program.at(id);
        auto field = std::ranges::find(layout.fields, key, &Field::key);
        if (field == layout.fields.end() || field->form.contains(FieldForm::IsAccessor))
            return false;
        const LocationFlow& location = field->flow.location;
        if (location.writers != WriterState::AllKnown || !field->flow.stability.containsAll({ FieldStability::IsNeverDeleted, FieldStability::IsNeverRedefined }))
            return false;
        if (location.stores != StoreCount::Unknown && location.initialization != InitializationState::Unknown)
            return true;
        // Every store names the field, and no operation does: it holds what the object was made with.
        return location.storeNaming == StoreNaming::EveryStoreNamesLocation && !m_context.variables.isStoredByName(id, field - layout.fields.begin());
    });
}

// The function allocates the object without a prototype of its own, and does nothing with it but store to it, read from it and keep it.
bool MovableCode::isOnlyFilledHere(FunctionAnalyses& analyses, ValueID object) const
{
    const Function& function = analyses.function();
    const UseLists& useLists = analyses.useLists();
    ValueID allocation = allocationOf(function, object);
    const Value& allocated = function.at(allocation);
    if (allocated.opcode != Opcode::NewDictionary && (allocated.opcode != Opcode::NewObject || allocated.prototypeOperand()))
        return false;

    Vector<ValueID, 4> names { allocation };
    for (size_t index = 0; index < names.size(); ++index) {
        for (const Use& use : useLists.usesOf(names[index])) {
            if (use.kind == Use::Kind::TerminatorOperand && function.at(use.block).terminator.kind == TerminatorKind::Return)
                continue;
            if (use.kind != Use::Kind::Operand)
                return false;
            const Value& user = function.at(use.user);
            switch (user.opcode) {
            case Opcode::PropDefine:
            case Opcode::ElemDefine:
            case Opcode::PropSet:
            case Opcode::ElemSet:
            case Opcode::PropGet:
            case Opcode::ElemGet:
            case Opcode::PropHas:
            case Opcode::ElemHas:
            case Opcode::CopyDataProperties:
            case Opcode::NewObject:
            case Opcode::NewArray:
            case Opcode::NewClosure:
            case Opcode::PutModuleVariable:
            case Opcode::PutEnvironmentField:
                break;
            default:
                if (!isCast(user.opcode))
                    return false;
                names.append(use.user);
                break;
            }
        }
    }
    return true;
}

bool MovableCode::isDictionaryFilledHere(FunctionAnalyses& analyses, const Knowledge& known, ValueID object) const
{
    const Function& function = analyses.function();
    return known.made.get(object.index()) && function.at(allocationOf(function, object)).opcode == Opcode::NewDictionary && isOnlyFilledHere(analyses, object);
}

ValueID MovableCode::allocationOf(const Function& function, ValueID id) const
{
    for (;;) {
        const Value& value = function.at(id);
        if (isCast(value.opcode) || value.opcode == Opcode::FinishClass || value.opcode == Opcode::NewClass)
            id = value.operand(0);
        else
            return id;
    }
}

std::optional<ValueID> MovableCode::operandReturnedBy(const Function&, const Value& value) const
{
    if (value.opcode != Opcode::CallBuiltin)
        return std::nullopt;
    BuiltinCall call = builtinCallOf(m_program, value);
    if (call.form != BuiltinCallForm::Whole || call.arguments.empty())
        return std::nullopt;
    BuiltinID builtin = m_program.builtins().at(call.overload).builtin;
    if (builtin == Builtins::ObjectDefineProperty || builtin == Builtins::ObjectFreeze)
        return call.arguments[0];
    return std::nullopt;
}

// One answer serves every text and every number that a helper is handed.
TypeID MovableCode::withConstantsWidened(TypeID id) const
{
    const Type& type = m_arenas.types.at(id);
    bool hasText = type.primitives & Primitive::LiteralString;
    bool hasRange = (type.primitives & Primitive::Integer) && type.integers != Range::int32() && type.integers != Range::full();
    if (!hasText && !hasRange)
        return id;
    Type widened = type.withLiteralStringsWidened();
    if (hasRange)
        widened.integers = type.integers.isSubsetOf(Range::int32()) ? Range::int32() : Range::full();
    return m_arenas.types.intern(widened);
}

std::optional<CallContext> MovableCode::contextOf(const Function& caller, const Knowledge& known, const Value& call, const Function& callee) const
{
    unsigned numberOfParameters = callee.numberOfEntryParameters();
    if (numberOfParameters > 64)
        return std::nullopt;
    // A parameter that gets no argument holds undefined.
    CallContext context;
    context.types.fill(TypeID(), numberOfParameters);
    for (unsigned index = 0; index < numberOfParameters; ++index) {
        if (callee.entryParameterKind(index) != EntryParameter::Parameter)
            continue;
        context.made |= uint64_t { 1 } << index;
        context.types[index] = TypeTable::undefined;
    }
    call.forEachOperand([&](ValueID operand, uint32_t index) {
        if (!operand)
            return;
        std::optional<uint32_t> parameter = Value::opcodesWithCallees.contains(call.opcode) ? entryParameterOf(callee, call, index) : call.variadicOperandIndex(index);
        if (!parameter || *parameter >= numberOfParameters)
            return;
        uint64_t bit = uint64_t { 1 } << *parameter;
        context.made = isMadeOrNoReference(caller, known, operand) ? context.made | bit : context.made & ~bit;
        if (known.unchanging.get(operand.index()))
            context.unchanging |= bit;
        context.types[*parameter] = withConstantsWidened(caller.at(operand).type);
    });
    return context;
}

std::optional<CalleeAnswer> MovableCode::answerForCall(const Function& caller, const Knowledge& known, const Value& call)
{
    switch (call.opcode) {
    case Opcode::Call:
    case Opcode::Construct:
    case Opcode::CallDirect:
        break;
    default:
        return std::nullopt;
    }
    CalleeList callees = calleesOfCall(m_program, call);
    if (!callees.isComplete && call.opcode != Opcode::CallDirect) {
        // What the call of this run hands on says more than what all calls hand on.
        const Type& type = typeOf(caller, call.calleeOperand());
        if (hasOnlyFunctions(type)) {
            callees = CalleeList::none();
            for (FunctionID function : type.functions)
                callees.add(function);
        }
    }
    if (!callees.isComplete || !callees.builtins.isEmpty() || callees.functions.isEmpty())
        return std::nullopt;
    CalleeAnswer result { true, true };
    for (FunctionID id : callees.functions) {
        const Function& callee = m_program.at(id);
        // A rest parameter and `arguments` are made at the entrance, which this does not follow.
        if (callee.hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator, FunctionFlags::HasRestParameter, FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest })
            || (call.opcode == Opcode::Call && isClassConstructor(callee))) {
            return CalleeAnswer { };
        }
        std::optional<CallContext> context = contextOf(caller, known, call, callee);
        if (!context)
            return CalleeAnswer { };
        CalleeAnswer answer = answerFor(id, *context);
        // One callee that may not move settles it: a call of one of a thousand functions asks about one.
        if (!answer.mayMove)
            return CalleeAnswer { };
        result.returnsWhatItMade &= answer.returnsWhatItMade;
    }
    return result;
}

// Every round passes a test of a number that each round moves by the same step toward a bound. The bound may change from round to round: its type says how far it can go.
static bool isCountedAndEnds(FunctionAnalyses& analyses, const NaturalLoop& loop)
{
    const Program& program = analyses.program();
    const Function& function = analyses.function();
    CFG& cfg = analyses.cfg();
    Dominators& dominators = analyses.dominators();
    NaturalLoops& loops = analyses.naturalLoops();
    for (const InductionVariable& variable : analyses.inductionVariables().of(loop).variables) {
        if (variable.step.kind != InductionStepKind::Constant || !variable.step.constant || !program.typeOf(function, variable.parameter).isNumberWithoutFraction())
            continue;
        for (const InductionTest& test : variable.tests) {
            if (test.exits == LoopExits::Unknown || !isBoundingRelation(test.relation, variable.direction))
                continue;
            // One more than a whole number below 2 ** 53 is another number.
            const Type& bound = program.typeOf(function, test.bound);
            if (!bound.isInteger() || !bound.integers.isSubsetOf(Range { -(int64_t { 1 } << 53), int64_t { 1 } << 53 }))
                continue;
            bool isPassedByEveryRound = std::ranges::all_of(cfg.predecessors(loop.header()), [&](BlockID predecessor) {
                return !loops.belongsTo(predecessor, loop) || dominators.dominates(test.block, predecessor);
            });
            if (isPassedByEveryRound)
                return true;
        }
    }
    return false;
}

BitVector MovableCode::findBlocksThatMayRunForever(FunctionAnalyses& analyses)
{
    const Function& function = analyses.function();
    BitVector result(function.blocks.size());
    CFG& cfg = analyses.cfg();
    if (!cfg.hasCycle())
        return result;

    Dominators& dominators = analyses.dominators();
    bool hasOnlyNaturalCycles = true;
    for (BlockID from : cfg.reversePostOrder()) {
        for (BlockID to : cfg.successors(from)) {
            if (cfg.reversePostOrderIndex(to) <= cfg.reversePostOrderIndex(from) && !dominators.dominates(to, from))
                hasOnlyNaturalCycles = false;
        }
    }
    if (!hasOnlyNaturalCycles) {
        for (BlockID block : cfg.reversePostOrder()) {
            if (analyses.isInCycle(block))
                result.quickSet(block.index());
        }
        return result;
    }
    NaturalLoops& loops = analyses.naturalLoops();
    for (unsigned index = 0; index < loops.numLoops(); ++index) {
        const NaturalLoop& loop = loops.loop(index);
        if (isCountedAndEnds(analyses, loop))
            continue;
        for (unsigned member = 0; member < loop.size(); ++member)
            result.quickSet(loop.at(member).index());
    }
    return result;
}

bool MovableCode::terminatorMayMove(FunctionAnalyses& analyses, BlockID block)
{
    const Terminator& terminator = analyses.function().at(block).terminator;
    if (terminator.kind == TerminatorKind::SwitchString && isPrimitiveKey(analyses.function(), terminator.operand(0)))
        return true;
    Effects effects = analyses.effectsOfTerminator(block).onAnyEdge();
    return !effects.mayRunOtherCode() && !effects.mayThrowByRule() && !effects.mayBeTerminated && effects.writes.containsOnly(heapsNobodyObserves | heapsWhoseWritesNobodySees);
}

Knowledge MovableCode::findKnowledge(FunctionAnalyses& analyses, const CallContext& context, const BitVector* countAsMadeElsewhere)
{
    const Function& function = analyses.function();
    Knowledge known { BitVector(function.values.size()), BitVector(function.values.size()) };
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const Block& block = function.blocks[blockIndex];
        for (uint32_t index = 0; index < block.parameters.size(); ++index) {

            if (blockIndex || (index < 64 && context.made >> index & 1))
                known.made.set(block.parameters[index].index());
            if (blockIndex || (index < 64 && context.unchanging >> index & 1))
                known.unchanging.set(block.parameters[index].index());
        }
    }

    auto isAlias = [&](const Value& value) {
        return isCast(value.opcode) || value.opcode == Opcode::FinishClass || value.opcode == Opcode::NewClass || (value.opcode == Opcode::ToObject && typeOf(function, value.operand(0)).isObject());
    };
    auto isMadeBy = [&](ValueID id, const Value& value) {
        if (countAsMadeElsewhere && countAsMadeElsewhere->get(id.index()))
            return false;
        if (interpretedOf(value.opcode) == Interpretation::Allocation || value.opcode == Opcode::NewDictionary || value.opcode == Opcode::NewArrayLike || value.opcode == Opcode::NewContiguousArray)
            return true;
        if (isAlias(value))
            return known.made.get(value.operand(0).index());
        if (value.opcode == Opcode::ArraySpeciesCreate)
            return hasOnlyPristineArrays(typeOf(function, value.operand(1)));
        if (std::optional<ValueID> returned = operandReturnedBy(function, value))
            return known.made.get(returned->index());
        if (builtinCallOpcodes.contains(value.opcode))
            return freshnessOf(analyses.effectContext(), function, value) != ResultFreshness::Unknown;
        if (std::optional<CalleeAnswer> answer = answerForCall(function, known, value))
            return answer->mayMove && answer->returnsWhatItMade;
        return false;
    };
    auto isUnchanging = [&](const Value& value) {
        if (hasOnlyObjectsThatNeverChange(m_arenas.types.at(value.type)))
            return true;
        if (isAlias(value))
            return known.unchanging.get(value.operand(0).index());
        if (value.opcode == Opcode::GetModuleVariable)
            return m_context.variables.holdsUnchangingObject.get(value.variable().index());
        if (Value::opcodesWithAllocationSite.contains(value.opcode) && value.allocationSite() && interpretedOf(value.opcode) == Interpretation::Allocation)
            return m_context.flow.objectsNeverChange(value.allocationSite()) || m_context.flow.classOf(value.allocationSite()).impossibleMutations.containsAll(mutationsOfContents);
        return false;
    };

    // What a block is handed is made if every edge hands it such an object. Each round takes some away.
    for (bool hasChanged = true; hasChanged;) {
        hasChanged = false;
        auto update = [&](BitVector& bits, ValueID id, bool value) {
            if (bits.get(id.index()) == value)
                return;
            bits.set(id.index(), value);
            hasChanged = true;
        };
        for (BlockID id : analyses.cfg().reversePostOrder()) {
            const Block& block = function.at(id);
            for (ValueID valueID : block.values) {
                update(known.made, valueID, isMadeBy(valueID, function.at(valueID)));
                update(known.unchanging, valueID, isUnchanging(function.at(valueID)));

            }
            auto pass = [&](const Target& target, std::optional<uint32_t> targetIndex) {
                const Block& successor = function.at(target.block);
                for (uint32_t index = 0; index < successor.parameters.size(); ++index) {
                    ValueID parameter = successor.parameters[index];
                    ValueID handed = index < target.arguments.size() ? target.arguments[index] : ValueID();
                    // What a test has narrowed is the value that was tested.
                    if (!handed && targetIndex && index - target.arguments.size() < target.proofs.size())
                        handed = operandNarrowedOnEdge(function, { id, *targetIndex, static_cast<uint32_t>(index - target.arguments.size()) });
                    bool isReference = handed && mayBeReference(typeOf(function, handed));
                    if (!handed || (isReference && !known.made.get(handed.index())))
                        update(known.made, parameter, false);
                    if (!handed || (isReference && !known.unchanging.get(handed.index())))
                        update(known.unchanging, parameter, false);

                }
            };
            for (uint32_t targetIndex = 0; targetIndex < block.terminator.targets.size(); ++targetIndex)
                pass(block.terminator.targets[targetIndex], targetIndex);
            if (block.handler)
                pass(*block.handler, std::nullopt);
        }
    }
    return known;
}

// `if (k in array) use(array[k])`: no prototype above a pristine array has an index, so the element is the array's own, and it is what the array is proven to hold.
void MovableCode::narrowElementsThatAreThere(FunctionID id, Function& function)
{
    struct Narrowing {
        BlockID block;
        uint32_t index;
        TypeID type;
    };
    Vector<Narrowing, 2> narrowings;
    Vector<uint32_t> numberOfPredecessors(FillWith { }, function.blocks.size(), 0u);
    for (const Block& block : function.blocks) {
        block.forEachTarget([&](const Target& target) {
            ++numberOfPredecessors[target.block.index()];
        });
    }
    for (const Block& block : function.blocks) {
        if (block.terminator.kind != TerminatorKind::Branch)
            continue;
        const Value& test = function.at(definitionThroughUpcasts(function, block.terminator.conditionOperand()));
        if (test.opcode != Opcode::ElemHas)
            continue;
        const Type& arrayType = typeOf(function, test.objectOperand());
        const ClassPart* part = hasOnlyPristineArrays(arrayType) ? arrayType.singleBuiltinClass() : nullptr;
        BlockID taken = block.terminator.targets[0].block;
        if (!part || !part->contentsAreProven || part->contents.isEmpty() || numberOfPredecessors[taken.index()] != 1)
            continue;
        const BlockValues& values = function.at(taken).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& read = function.at(values[index]);
            // Nothing in front of the read has run that could take the element away.
            if (read.opcode != Opcode::ElemGet) {
                if (!isCast(read.opcode) && !isRematerializable(read))
                    break;
                continue;
            }
            bool isSameElement = sameObjectThroughCasts(function, read.objectOperand()) == sameObjectThroughCasts(function, test.objectOperand())
                && definitionThroughUpcasts(function, read.propertyOperand()) == definitionThroughUpcasts(function, test.propertyOperand());
            if (isSameElement)
                narrowings.append({ taken, index, part->contents[0] });
            break;
        }
    }
    if (narrowings.isEmpty())
        return;

    IRBuilder builder(AnalysisContext { m_context.build, m_program, &m_context.flow, &m_context.effects, id }, function, m_arenas.types);
    for (const Narrowing& narrowing : narrowings) {
        ValueID read = function.at(narrowing.block).values[narrowing.index];
        TypeID castType = m_arenas.types.intersectionOf(function.at(read).type, narrowing.type);
        builder.setOrigin(function.at(read).origin());
        ValueID cast = builder.addDetached(makeAssumeType(read, castType));
        auto replace = [&](ValueID& operand) {
            if (operand == read)
                operand = cast;
        };
        for (Block& block : function.editAllBlocks()) {
            for (ValueID user : block.values)
                function.edit(user).forEachChild(replace);
            block.forEachChildOutsideValues(replace);
            for (Target& target : block.terminator.targets) {
                for (ValueID& argument : target.arguments)
                    replace(argument);
            }
        }
        function.edit(narrowing.block).values.insert(narrowing.index + 1, cast);
    }
}

// The body as it is for what this call hands over: tests of the parameters are decided, and what they guard is gone.
std::optional<Function> MovableCode::specialize(FunctionID id, const CallContext& context)
{
    const Function& original = m_program.at(id);
    TypeArena& types = m_arenas.types;
    Function version;
    bool isNarrowed = false;
    for (uint32_t index = 0; index < context.types.size(); ++index) {
        if (!context.types[index])
            continue;
        const Value& parameter = original.at(original.at(entryBlock).parameters[index]);
        TypeID narrowed = types.intersectionOf(parameter.type, context.types[index]);
        if (narrowed == parameter.type && !parameter.expected())
            continue;
        if (!isNarrowed)
            version = original;
        isNarrowed = true;
        Value& narrowedParameter = version.edit(version.at(entryBlock).parameters[index]);
        narrowedParameter.type = narrowed;
        narrowedParameter.setExpected({ });
        if (DeclaredType* declared = version.declaredEntryParameter(index))
            *declared = DeclaredType { .type = narrowed };
    }
    if (!isNarrowed)
        return std::nullopt;
    for (unsigned round = 0; round < 2; ++round) {
        {
            FunctionAnalyses analyses(AnalysisContext { m_context.build, m_program, &m_context.flow, &m_context.effects, id }, version);
            FunctionPassContext pass { m_program, m_context.inputs, id, version, analyses, m_arenas.types, m_arenas.strings, m_context.build };
            for (PassID step : { PassID::RetypeValues, PassID::SimplifyCasts, PassID::ResolveBuiltins, PassID::SimplifyBuiltinCalls, PassID::ResolveAccesses, PassID::RetypeValues })
                m_context.runner.runFunctionPass(step, pass);
        }
        m_context.runner.runSchedule(FunctionSchedule::Simplify, m_program, m_context.inputs, id, version, m_arenas);
        if (!round)
            narrowElementsThatAreThere(id, version);
    }
    return version;
}

static bool isSameOperation(const Value& a, const Value& b)
{
    if (a.opcode != b.opcode || a.type != b.type || a.expected() != b.expected() || a.numberOfOperands() != b.numberOfOperands() || !a.hasSamePayloadAs(b))
        return false;
    for (uint32_t index = 0; index < a.numberOfOperands(); ++index) {
        if (a.operand(index) != b.operand(index))
            return false;
    }
    return true;
}

static bool haveSameOperations(const Function& a, const Function& b)
{
    if (a.blocks.size() != b.blocks.size() || !std::ranges::equal(a.values, b.values, isSameOperation))
        return false;
    for (size_t index = 0; index < a.blocks.size(); ++index) {
        if (a.blocks[index].terminator.kind != b.blocks[index].terminator.kind || !std::ranges::equal(a.blocks[index].values, b.blocks[index].values))
            return false;
    }
    return true;
}

// Null where the types of the call narrow no parameter.
const Function* MovableCode::versionFor(FunctionID id, const CallContext& context, std::unique_ptr<const Function>& versionNotKept)
{
    if (auto found = m_versions.find(id.index()); found != m_versions.end()) {
        for (const KeptVersion& kept : found->value) {
            if (kept.types != context.types)
                continue;
            if (m_context.build.options().verifiesAfterEveryPass) {
                std::optional<Function> madeAgain = specialize(id, context);
                RELEASE_ASSERT(madeAgain && haveSameOperations(*kept.version, *madeAgain));
            }
            return kept.version.get();
        }
    }
    std::optional<Function> version = specialize(id, context);
    if (!version)
        return nullptr;
    std::unique_ptr<const Function> made = makeUniqueWithoutFastMallocCheck<Function>(WTF::move(*version));
    const Function* result = made.get();
    if (m_numberOfValuesInKeptVersions + result->values.size() > maxValuesInKeptVersions) {
        versionNotKept = WTF::move(made);
        return result;
    }
    m_numberOfValuesInKeptVersions += result->values.size();
    m_versions.ensure(id.index(), [] {
        return Vector<KeptVersion, 1> { };
    }).iterator->value.append(KeptVersion { context.types, WTF::move(made) });
    return result;
}

CalleeAnswer MovableCode::answerForBody(FunctionID id, const Function& function, const CallContext& context)
{
    FunctionAnalyses analyses(AnalysisContext { m_context.build, m_program, &m_context.flow, &m_context.effects, id }, function);
    Knowledge known = findKnowledge(analyses, context);
    BitVector mayRunForever = findBlocksThatMayRunForever(analyses);
    BitVector isReached(function.blocks.size());
    Vector<BlockID, 16> worklist { entryBlock };
    isReached.quickSet(entryBlock.index());
    while (!worklist.isEmpty()) {
        const Block& block = function.at(worklist.takeLast());
        std::optional<bool> condition = block.terminator.kind == TerminatorKind::Branch ? knownCondition(function, block.terminator.conditionOperand()) : std::nullopt;
        for (uint32_t index = 0; index < block.terminator.targets.size(); ++index) {
            if (condition && index == (*condition ? 1u : 0u))
                continue;
            BlockID target = block.terminator.targets[index].block;
            if (!isReached.quickSet(target.index()))
                worklist.append(target);
        }
    }

    CalleeAnswer answer { true, true };
    for (BlockID blockID : analyses.cfg().reversePostOrder()) {
        const Block& block = function.at(blockID);
        if (!isReached.quickGet(blockID.index()))
            continue;
        if (block.handler || mayRunForever.quickGet(blockID.index())) {
            return { };
        }
        for (ValueID valueID : block.values) {
            if (isRematerializable(function.at(valueID)) || mayMove(analyses, known, valueID))
                continue;
            return { };
        }
        bool returns = block.terminator.kind == TerminatorKind::Return || block.terminator.kind == TerminatorKind::ReturnCall;
        if (!terminatorMayMove(analyses, blockID) || (returnsToCaller(block.terminator.kind) && !returns)) {
            return { };
        }
        if (returns) {
            ValueID returned = block.terminator.operand(0);
            if (returned && !isMadeOrNoReference(function, known, returned))
                answer.returnsWhatItMade = false;
        }
    }
    return answer;
}

CalleeAnswer MovableCode::answerFor(FunctionID id, const CallContext& context)
{
    if (auto found = m_answers.find(id.index()); found != m_answers.end()) {
        for (const auto& [knownContext, answer] : found->value) {
            if (knownContext != context)
                continue;
            m_variablesTakenToBeSet.appendVector(answer.variablesTakenToBeSet);
            return answer;
        }
    }
    // Many a helper is plain enough as it stands, and that answer serves whatever the types of a call are.
    bool hasTypes = std::ranges::any_of(context.types, [](TypeID type) {
        return !!type;
    });
    CalleeAnswer asItStands;
    if (hasTypes) {
        size_t numberTakenBefore = m_variablesTakenToBeSet.size();
        asItStands = answerFor(id, CallContext { context.made, context.unchanging, { } });
        if (asItStands.mayMove && asItStands.returnsWhatItMade)
            return asItStands;
        m_variablesTakenToBeSet.shrink(numberTakenBefore);
    }
    const Function& function = m_program.at(id);
    std::pair<uint32_t, CallContext> question { id.index(), context };
    if (!function.hasBody() || function.values.size() > maxValuesOfCallee || m_beingAnswered.size() >= maxDepth || m_numberOfAnswers >= maxAnswers || m_beingAnswered.contains(question))
        return { };

    ++m_numberOfAnswers;
    m_beingAnswered.append(question);
    size_t numberTakenBefore = m_variablesTakenToBeSet.size();
    CalleeAnswer answer;
    std::unique_ptr<const Function> versionNotKept;
    if (!hasTypes)
        answer = answerForBody(id, function, context);
    else if (const Function* version = versionFor(id, context, versionNotKept))
        answer = answerForBody(id, *version, context);
    else {
        answer = WTF::move(asItStands);
        m_variablesTakenToBeSet.appendVector(std::exchange(answer.variablesTakenToBeSet, { }));
    }
    m_beingAnswered.removeLast();
    answer.variablesTakenToBeSet.append(m_variablesTakenToBeSet.subspan(numberTakenBefore));
    std::ranges::sort(answer.variablesTakenToBeSet);
    removeRepeatedElements(answer.variablesTakenToBeSet);
    m_answers.ensure(id.index(), [] {
        return Vector<std::pair<CallContext, CalleeAnswer>, 1> { };
    }).iterator->value.append({ context, answer });
    return answer;
}

// A store to an object that the initializer has made and that no accessor, no lock and no other prototype stands in the way of.
bool MovableCode::storeMayMove(FunctionAnalyses& analyses, const Knowledge& known, const Value& value)
{
    const Function& function = analyses.function();
    ValueID object = value.objectOperand();
    if (!known.made.get(object.index()))
        return false;
    const Type& type = typeOf(function, object);
    bool defines = value.opcode == Opcode::PropDefine || value.opcode == Opcode::ElemDefine;
    bool hasName = value.opcode == Opcode::PropDefine || value.opcode == Opcode::PropSet;
    if (!hasName && !isPrimitiveKey(function, value.propertyOperand()))
        return false;

    if (hasOnlyFunctions(type)) {
        static constexpr ASCIILiteral namesWithRules[] = { "prototype"_s, "name"_s, "length"_s, "caller"_s, "arguments"_s, "__proto__"_s };
        bool isRefusedAbove = hasName && !defines && (m_program.at(value.key()).kind == PropertyKeyKind::WellKnownSymbol || (isText(value.key(), "constructor"_s) && std::ranges::any_of(type.functions, [&](FunctionID id) {
            return m_program.at(id).hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator });
        })));
        if (isRefusedAbove)
            return false;
        bool isPrototypeOfPlainFunction = hasName && !defines && isText(value.key(), "prototype"_s) && std::ranges::all_of(type.functions, [&](FunctionID id) {
            return m_program.at(id).kind == FunctionKind::Plain && !m_program.at(id).hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator });
        });
        if (isPrototypeOfPlainFunction)
            return true;
        if (!hasName || (!defines && !hasOnlyFunctionsWithoutClass(type)) || std::ranges::any_of(namesWithRules, [&](ASCIILiteral name) { return isText(value.key(), name); }))
            return false;
        return true;
    }
    // No index of a pristine array is an accessor, and nobody can add one to the prototypes above it.
    if (isPristineArray(function, known, object) && !hasName && typeOf(function, value.propertyOperand()).isNumber())
        return true;
    if (!isDictionaryFilledHere(analyses, known, object)) {
        if (!hasOnlyPlainObjects(type))
            return false;
        if (!isKnownToStayPlain(type) && !isOnlyFilledHere(analyses, object))
            return false;
    }
    if (defines)
        return true;
    // The setter of __proto__ does nothing with what is neither an object nor null.
    const Type& stored = typeOf(function, value.valueOperand());
    if (!stored.hasObjects() && !(stored.primitives & Primitive::Null))
        return true;
    if (hasName ? isText(value.key(), "__proto__"_s) : !typeOf(function, value.propertyOperand()).isNumber())
        return false;
    return true;
}

bool MovableCode::readMayMove(FunctionAnalyses& analyses, const Knowledge& known, const Value& value)
{
    const Function& function = analyses.function();
    ValueID object = value.objectOperand();
    if (value.opcode == Opcode::PropGet && findsFieldThatHoldsOneValue(typeOf(function, object), value.key()))
        return true;
    if (!known.mayReadFrom(object))
        return false;
    const Type& type = typeOf(function, object);
    bool hasName = value.opcode == Opcode::PropGet || value.opcode == Opcode::PropHas;
    if (!hasName && !isPrimitiveKey(function, value.propertyOperand()))
        return false;
    if (isPristineArray(function, known, object) && (hasName ? isText(value.key(), "length"_s) : typeOf(function, value.propertyOperand()).isNumber()))
        return true;
    ProvenAccess proven = provenAccessOf(m_program, function, value);
    if (!proven.isComplete || !proven.accessors.isEmpty() || !proven.methods.isEmpty() || type.mayBeUndefinedOrNull())
        return false;
    return true;
}

// Object.defineProperty(made, constant key, { value: .. }) where the object has no such property yet, or one that can be redefined.
bool MovableCode::definitionByDescriptorMayMove(FunctionAnalyses& analyses, const Knowledge& known, const Value& value)
{
    const Function& function = analyses.function();
    BuiltinCall call = builtinCallOf(m_program, value);
    if (call.arguments.size() != 3 || !known.made.get(call.arguments[0].index()) || !known.made.get(call.arguments[2].index()))
        return false;

    const Value& key = function.at(call.arguments[1]);
    bool isWellKnownSymbol = key.opcode == Opcode::Builtin && typeOf(function, call.arguments[1]).hasOnlyPrimitives(Primitive::Symbol | Primitive::LiteralSymbol);
    if (key.opcode != Opcode::String && !isWellKnownSymbol)
        return false;
    const Type& target = typeOf(function, call.arguments[0]);
    bool isFunction = target.isObject() && !target.anyObject && !target.anyFunction && !target.layouts && target.builtins.isEmpty() && target.classes.isEmpty() && !target.functions.isEmpty();
    if (!isFunction)
        return false;
    if (key.opcode == Opcode::String) {
        std::span<const uint8_t> text = m_program.string(key.string());
        for (ASCIILiteral name : { "prototype"_s, "caller"_s, "arguments"_s }) {
            if (text.size() == name.length() && spanHasPrefix(text, name.span8()))
                return false;
        }
    }

    // One definition under each key: what the first one makes cannot be redefined.
    ValueID allocation = allocationOf(function, call.arguments[0]);
    if (function.at(allocation).opcode != Opcode::NewClosure)
        return false;
    for (const Value& other : function.values) {
        if (&other == &value || other.opcode != Opcode::CallBuiltin)
            continue;
        BuiltinCall otherCall = builtinCallOf(m_program, other);
        if (m_program.builtins().at(otherCall.overload).builtin != Builtins::ObjectDefineProperty)
            continue;
        if (otherCall.arguments.size() != 3 || allocationOf(function, otherCall.arguments[0]) != allocation)
            continue;
        const Value& otherKey = function.at(otherCall.arguments[1]);
        bool differs = otherKey.opcode != key.opcode ? (otherKey.opcode == Opcode::String || otherKey.opcode == Opcode::Builtin)
            : key.opcode == Opcode::String ? otherKey.string() != key.string() : otherKey.builtin() != key.builtin();
        if (!differs)
            return false;
    }

    // The descriptor is a literal that goes nowhere else and has `value` and flags.
    const UseLists& useLists = analyses.useLists();
    ValueID descriptor = call.arguments[2];
    for (;; descriptor = function.at(descriptor).operand(0)) {
        if (useLists.useCount(descriptor) != 1)
            return false;
        if (!isCast(function.at(descriptor).opcode))
            break;
    }
    const Value& literal = function.at(descriptor);
    if (literal.opcode != Opcode::NewObject || literal.prototypeOperand() || m_program.at(literal.layout()).kind != LayoutKind::Record)
        return false;
    const Layout& layout = m_program.at(literal.layout());
    for (uint32_t field : literal.fields()) {
        PropertyKeyID name = layout.fields[field].key;
        bool isKnown = name && (isText(name, "value"_s) || isText(name, "writable"_s) || isText(name, "enumerable"_s) || isText(name, "configurable"_s));
        if (!isKnown || layout.fields[field].form.contains(FieldForm::IsAccessor))
            return false;
    }
    return true;
}

// What the records say of the memory that a built-in touches does not tell the clock, the random numbers or the stack of calls from bookkeeping.
// These answer by their operands and by what the operands hold, and leave a trace nowhere else.
bool MovableCode::dependsOnOperandsAlone(const BuiltinOverload& overload) const
{
    static constexpr BuiltinID builtins[] = {
        Builtins::ObjectKeys, Builtins::ObjectValues, Builtins::ObjectEntries, Builtins::ObjectAssign, Builtins::ObjectHasOwn, Builtins::ObjectIs, Builtins::ObjectGetOwnPropertyNames,
        Builtins::ObjectPrototypeHasOwnProperty,
        Builtins::ArrayIsArray, Builtins::ArrayOf, Builtins::ArrayPrototypeLength, Builtins::ArrayPrototypeJoin, Builtins::ArrayPrototypeSlice, Builtins::ArrayPrototypeConcat,
        Builtins::ArrayPrototypeIndexOf, Builtins::ArrayPrototypeLastIndexOf, Builtins::ArrayPrototypeIncludes, Builtins::ArrayPrototypeAt, Builtins::ArrayPrototypePush,
        Builtins::ArrayPrototypePop, Builtins::ArrayPrototypeShift, Builtins::ArrayPrototypeUnshift, Builtins::ArrayPrototypeReverse, Builtins::ArrayPrototypeSort, Builtins::ArrayPrototypeFill,
        Builtins::ArrayPrototypeToReversed, Builtins::ArrayPrototypeToSorted, Builtins::ArrayPrototypeWith,
        Builtins::Map, Builtins::MapPrototypeSet, Builtins::MapPrototypeGet, Builtins::MapPrototypeHas, Builtins::MapPrototypeDelete, Builtins::MapPrototypeClear, Builtins::MapPrototypeSize,
        Builtins::Set, Builtins::SetPrototypeAdd, Builtins::SetPrototypeHas, Builtins::SetPrototypeDelete, Builtins::SetPrototypeClear, Builtins::SetPrototypeSize,
        Builtins::StringPrototypeSplit, Builtins::StringPrototypeLength, Builtins::String, Builtins::Symbol, Builtins::SymbolFor, Builtins::WeakMap, Builtins::WeakSet,
    };
    return m_program.builtins().at(overload.builtin).flags.contains(BuiltinFlag::IsDeterministic) || std::ranges::contains(builtins, overload.builtin);
}

// The steps of these that throw are the tests of the receiver and the conversions of the arguments.
bool MovableCode::throwsOnlyForOperandsOfOtherTypes(const Function& function, const Knowledge& known, const BuiltinOverload& overload, const BuiltinCall& call) const
{
    if (call.form != BuiltinCallForm::Whole)
        return false;
    BuiltinID builtin = overload.builtin;
    auto hasOnlyArgumentsThat = [&](NOESCAPE const Invocable<bool(const Type&)> auto& predicate) {
        return std::ranges::all_of(call.arguments, [&](ValueID argument) {
            return !argument || predicate(typeOf(function, argument));
        });
    };
    // A description and a key of the registry are converted to strings.
    if ((builtin == Builtins::Symbol || builtin == Builtins::SymbolFor) && overload.mode == BuiltinCallMode::Call)
        return hasOnlyArgumentsThat([](const Type& type) { return type.hasOnlyPrimitives(Primitive::AnyString | Primitive::Undefined | Primitive::Number); });
    if (builtin == Builtins::String && overload.mode == BuiltinCallMode::Call)
        return hasOnlyArgumentsThat([](const Type& type) { return !type.hasObjects() && type.isJavaScriptValue(); });
    if ((builtin == Builtins::WeakMap || builtin == Builtins::WeakSet || builtin == Builtins::Map || builtin == Builtins::Set) && overload.mode == BuiltinCallMode::Construct)
        return hasOnlyArgumentsThat([](const Type& type) { return type.hasOnlyPrimitives(Primitive::UndefinedOrNull); });
    if (overload.mode != BuiltinCallMode::Call || !call.receiver)
        return false;
    const Type& receiver = typeOf(function, call.receiver);
    auto isOneOf = [&](std::initializer_list<BuiltinID> list) {
        return std::ranges::contains(list, builtin);
    };
    auto argumentsAre = [&](NOESCAPE const Invocable<bool(const Type&)> auto& predicate) {
        return std::ranges::all_of(call.arguments, [&](ValueID argument) {
            return !argument || predicate(typeOf(function, argument));
        });
    };
    if (isOneOf({ Builtins::MapPrototypeSet, Builtins::MapPrototypeGet, Builtins::MapPrototypeHas, Builtins::MapPrototypeDelete, Builtins::MapPrototypeClear, Builtins::MapPrototypeSize }))
        return hasOnlyInstancesOf(receiver, BuiltinClasses::Map);
    if (isOneOf({ Builtins::SetPrototypeAdd, Builtins::SetPrototypeHas, Builtins::SetPrototypeDelete, Builtins::SetPrototypeClear, Builtins::SetPrototypeSize }))
        return hasOnlyInstancesOf(receiver, BuiltinClasses::Set);

    auto isStringOrUndefined = [](const Type& type) {
        return type.hasOnlyPrimitives(Primitive::AnyString | Primitive::Undefined);
    };
    auto isNumberOrUndefined = [](const Type& type) {
        return type.hasOnlyPrimitives(Primitive::Number | Primitive::Undefined);
    };
    if (receiver.isAnyString()) {
        if (isOneOf({ Builtins::StringPrototypeSplit, Builtins::StringPrototypeIndexOf, Builtins::StringPrototypeLastIndexOf, Builtins::StringPrototypeStartsWith, Builtins::StringPrototypeEndsWith,
            Builtins::StringPrototypeIncludes, Builtins::StringPrototypeConcat, Builtins::StringPrototypeReplace, Builtins::StringPrototypeReplaceAll }))
            return argumentsAre([&](const Type& type) { return isStringOrUndefined(type) || isNumberOrUndefined(type); });
        if (isOneOf({ Builtins::StringPrototypeSlice, Builtins::StringPrototypeSubstring, Builtins::StringPrototypeCharAt, Builtins::StringPrototypeCharCodeAt, Builtins::StringPrototypeCodePointAt,
            Builtins::StringPrototypeAt, Builtins::StringPrototypeToLowerCase, Builtins::StringPrototypeToUpperCase, Builtins::StringPrototypePadStart, Builtins::StringPrototypePadEnd }))
            return argumentsAre([&](const Type& type) { return isStringOrUndefined(type) || isNumberOrUndefined(type); });
        return false;
    }

    if (!isPristineArray(function, known, call.receiver))
        return false;
    bool holdsWhatBecomesString = std::ranges::all_of(receiver.classes, [&](const ClassPart& part) {
        return part.contentsAreProven && std::ranges::all_of(part.contents, [&](TypeID contents) {
            const Type& type = m_arenas.types.at(contents);
            return !type.hasObjects() && !(type.primitives & Primitive::AnySymbol);
        });
    });
    if (builtin == Builtins::ArrayPrototypeJoin)
        return holdsWhatBecomesString && argumentsAre(isStringOrUndefined);
    if (isOneOf({ Builtins::ArrayPrototypeSlice, Builtins::ArrayPrototypeAt }))
        return argumentsAre(isNumberOrUndefined);
    if (isOneOf({ Builtins::ArrayPrototypeIndexOf, Builtins::ArrayPrototypeLastIndexOf, Builtins::ArrayPrototypeIncludes, Builtins::ArrayPrototypePush, Builtins::ArrayPrototypePop, Builtins::ArrayPrototypeReverse }))
        return true;
    if (builtin == Builtins::ArrayPrototypeSort)
        return holdsWhatBecomesString && argumentsAre([](const Type& type) { return type.hasOnlyPrimitives(Primitive::Undefined); });
    if (builtin == Builtins::ArrayPrototypeConcat) {
        return std::ranges::all_of(call.arguments, [&](ValueID argument) {
            return !typeOf(function, argument).hasObjects() || isPristineArray(function, known, argument);
        });
    }
    return false;
}

bool MovableCode::keepsOperandsInResultOrWhatWasMade(const Function&, const Knowledge& known, const BuiltinOverload& overload, const BuiltinCall& call) const
{
    auto isMade = [&](BuiltinOperand operand) {
        if (operand.isReceiver())
            return call.receiver && known.made.get(call.receiver.index());
        for (unsigned index = 0; index < call.arguments.size(); ++index) {
            if (operand.coversArgument(index) && call.arguments[index] && !known.made.get(call.arguments[index].index()))
                return false;
        }
        return true;
    };
    return std::ranges::all_of(overload.flow.transfers, [&](const BuiltinTransfer& transfer) {
        switch (transfer.to.kind) {
        case BuiltinPlaceKind::Result:
        case BuiltinPlaceKind::ResultSlot:
        case BuiltinPlaceKind::ResultField:
            return true;
        case BuiltinPlaceKind::Operand:
        case BuiltinPlaceKind::Slot:
        case BuiltinPlaceKind::IteratedValues:
        case BuiltinPlaceKind::OwnValues:
            return transfer.to.operand && isMade(*transfer.to.operand);
        case BuiltinPlaceKind::None:
        case BuiltinPlaceKind::NewValue:
        case BuiltinPlaceKind::NativeMemory:
        case BuiltinPlaceKind::Prototype:
        case BuiltinPlaceKind::Member:
        case BuiltinPlaceKind::NativeCell:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

// A match reads the pattern and the flags, reads and writes lastIndex, and notes the match for RegExp.$1 and its kin. RegExp.prototype cannot be changed.
bool MovableCode::matchMayMove(const Function& function, const Knowledge& known, const BuiltinOverload& overload, const BuiltinCall& call) const
{
    static constexpr BuiltinID ofStrings[] = { Builtins::StringPrototypeReplace, Builtins::StringPrototypeReplaceAll, Builtins::StringPrototypeSplit, Builtins::StringPrototypeMatch, Builtins::StringPrototypeSearch };
    static constexpr BuiltinID ofRegExps[] = { Builtins::RegExpPrototypeTest, Builtins::RegExpPrototypeExec };
    if (call.form != BuiltinCallForm::Whole || overload.mode != BuiltinCallMode::Call || !call.receiver || call.arguments.empty() || !call.arguments[0] || m_context.flow.mayReadLegacyRegExpStatics())
        return false;
    bool isOfString = std::ranges::contains(ofStrings, overload.builtin);
    if (!isOfString && !std::ranges::contains(ofRegExps, overload.builtin))
        return false;
    ValueID regExp = isOfString ? call.arguments[0] : call.receiver;
    ValueID text = isOfString ? call.receiver : call.arguments[0];
    const Value& literal = function.at(allocationOf(function, regExp));
    if (!known.made.get(regExp.index()) || literal.opcode != Opcode::NewRegExp || !hasOnlyInstancesOf(typeOf(function, regExp), BuiltinClasses::RegExp) || !typeOf(function, text).isAnyString())
        return false;
    if (overload.builtin == Builtins::StringPrototypeReplaceAll && !literal.regExpFlags().contains(RegExpFlags::Global))
        return false;
    return std::ranges::all_of(call.arguments.subspan(1), [&](ValueID argument) {
        return !argument || typeOf(function, argument).hasOnlyPrimitives(Primitive::AnyString | Primitive::Undefined | Primitive::Number);
    });
}

// Nothing stores to the object behind the call, so nothing finds out by a store that it is frozen.
bool MovableCode::freezeMayMove(FunctionAnalyses& analyses, const Knowledge& known, ValueID call, ValueID frozen) const
{
    const Function& function = analyses.function();
    const UseLists& useLists = analyses.useLists();
    if (!known.made.get(frozen.index()))
        return false;
    const Type& type = typeOf(function, frozen);
    if (!hasOnlyPlainObjects(type) && !hasOnlyInstancesOf(type, BuiltinClasses::Array))
        return false;
    ValueLocation callLocation = useLists.locationOf(call);
    Vector<ValueID, 4> names { allocationOf(function, frozen) };
    for (size_t index = 0; index < names.size(); ++index) {
        for (const Use& use : useLists.usesOf(names[index])) {
            if (use.kind != Use::Kind::Operand)
                continue;
            const Value& user = function.at(use.user);
            if (isCast(user.opcode) || operandReturnedBy(function, user) == names[index]) {
                names.append(use.user);
                continue;
            }
            OperandSummary summary = summarizeOperand(analyses.effectContext(), function, user, use.operandIndex);
            bool mayStore = summary.uses.containsAny(operandWritingUses | unconfinedUses | callingUses) || isWrittenOperand(user.opcode, std::min(use.operandIndex, numberOfNamedOperandsOf(user.opcode))) || interpretedOf(user.opcode) == Interpretation::Store;
            if (!mayStore || use.user == call)
                continue;
            ValueLocation location = useLists.locationOf(use.user);
            if (location.block != callLocation.block || location.index > callLocation.index)
                return false;
        }
    }
    return true;
}

// A built-in touches what it is handed and what that leads to. It may write to what the initializer has made and read from what does not change.
bool MovableCode::builtinCallMayMove(FunctionAnalyses& analyses, const Knowledge& known, ValueID id)
{
    const Function& function = analyses.function();
    const Value& value = function.at(id);
    BuiltinCall call = builtinCallOf(m_program, value);
    const BuiltinOverload& overload = m_program.builtins().at(call.overload);
    if (overload.builtin == Builtins::ObjectDefineProperty && definitionByDescriptorMayMove(analyses, known, value))
        return true;
    if (matchMayMove(function, known, overload, call))
        return true;
    if (overload.builtin == Builtins::ObjectFreeze && call.form == BuiltinCallForm::Whole && call.arguments.size() == 1 && freezeMayMove(analyses, known, id, call.arguments[0]))
        return true;

    auto holdsNoObjects = [&](ValueID array) {
        return std::ranges::all_of(typeOf(function, array).classes, [&](const ClassPart& part) {
            return std::ranges::none_of(part.contents, [&](TypeID contents) {
                return m_arenas.types.at(contents).hasObjects();
            });
        });
    };
    // The iterators of arrays and the methods that add an entry cannot be replaced.
    bool makesCollection = overload.mode == BuiltinCallMode::Construct && (overload.builtin == Builtins::Set || overload.builtin == Builtins::Map) && call.form == BuiltinCallForm::Whole && call.arguments.size() == 1;
    if (makesCollection) {
        ValueID entries = call.arguments[0];
        if (!mayBeReference(typeOf(function, entries)))
            return true;
        if (known.mayReadFrom(entries) && isCollectionAsMade(function, known, entries) && (overload.builtin == Builtins::Set || hasOnlyInstancesOf(typeOf(function, entries), BuiltinClasses::Map)))
            return true;
        if (known.mayReadFrom(entries) && isPristineArray(function, known, entries)) {
            if (overload.builtin == Builtins::Set)
                return true;
            const Value& literal = function.at(allocationOf(function, entries));
            bool holdsPairs = literal.opcode == Opcode::NewArray && std::ranges::all_of(literal.elementsOperands(), [&](ValueID pair) {
                return pair && known.mayReadFrom(pair) && isPristineArray(function, known, pair);
            });
            if (holdsPairs)
                return true;
        }
    }

    if (!dependsOnOperandsAlone(overload))
        return false;
    Effects effects = analyses.effectsOf(id).onAnyEdge();
    if (effects.mayRunOtherCode() || effects.mayBeTerminated)
        return false;
    bool throwsForOtherTypesOnly = throwsOnlyForOperandsOfOtherTypes(function, known, overload, call);
    if (effects.mayThrowByRule() && !overload.onlyPropagatesErrors() && !throwsForOtherTypesOnly)
        return false;
    constexpr AbstractHeapSet heapsOfOperands = heapsNobodyObserves | perObjectHeaps;
    if (!effects.writes.containsOnly(heapsOfOperands) || !effects.reads.containsOnly(heapsOfOperands))
        return false;

    if (call.form != BuiltinCallForm::Whole)
        return false;
    bool keepsInResultOrWhatWasMade = keepsOperandsInResultOrWhatWasMade(function, known, overload, call);
    auto operandMayMove = [&](ValueID operand, BuiltinOperand position) {
        if (!operand || !mayBeReference(typeOf(function, operand)))
            return true;
        OperandBehavior behavior = declaredBehaviorOf(overload, position);
        constexpr OperandUses usesThatAreNotFollowed = callingUses | OperandUses { OperandUse::PassesToUserCode, OperandUse::ChangesPrototype, OperandUse::BecomesPrototype, OperandUse::ChangesIntegrity, OperandUse::DefinesName, OperandUse::TakesDescriptor };
        if (behavior.uses.containsAny(usesThatAreNotFollowed))
            return false;
        if (behavior.uses.containsAny({ OperandUse::Retains, OperandUse::RetainsContents }) && !keepsInResultOrWhatWasMade)
            return false;
        if (behavior.uses.containsAny(operandWritingUses) && !known.made.get(operand.index()))
            return false;
        // It keeps the object, or looks at which object it is. What a built-in reads of objects it may read of any operand that it looks at: the length of an array is no use of its own.
        bool namesNoMember = behavior.protocols.isEmpty() && behavior.namedMembers.empty() && behavior.calledMembers.empty();
        if (namesNoMember && usesOfObjectsMadeElsewhere.containsAll(behavior.uses) && (!behavior.uses.contains(OperandUse::Inspects) || !effects.reads.containsAny(perObjectHeaps)))
            return true;
        if (!known.mayReadFrom(operand))
            return false;
        // What it does to the operand it does to what the operand holds.
        if (behavior.uses.containsAny({ OperandUse::AppliesTransitively, OperandUse::AppliesToContents }) && !(isPristineArray(function, known, operand) && holdsNoObjects(operand)))
            return false;
        // No step on a pristine array, a plain object without accessors, a Map or a Set runs code of the program or is refused.
        const Type& type = typeOf(function, operand);
        bool isPlain = hasOnlyPlainObjects(type) && (isKnownToStayPlain(type) || isOnlyFilledHere(analyses, operand));
        if (!isPristineArray(function, known, operand) && !isPlain && !hasOnlyInstancesOf(type, BuiltinClasses::Map) && !hasOnlyInstancesOf(type, BuiltinClasses::Set))
            return false;
        return true;
    };
    if (!operandMayMove(call.receiver, BuiltinOperand::receiver()))
        return false;
    for (unsigned index = 0; index < call.arguments.size(); ++index) {
        if (!operandMayMove(call.arguments[index], BuiltinOperand::argument(index)))
            return false;
    }
    return true;
}

bool MovableCode::mayMoveByEffects(FunctionAnalyses& analyses, const Knowledge& known, ValueID id)
{
    const Function& function = analyses.function();
    const Value& value = function.at(id);
    const EffectsOnEdges& onEdges = analyses.effectsOf(id);
    Effects effects = onEdges.onAnyEdge();
    if (effects.mayCallUnknownCode)
        return false;
    if (effects.maySuspend)
        return false;
    if (effects.mayThrowByRule())
        return false;
    if (effects.mayBeTerminated)
        return false;

    const EffectContext& context = analyses.effectContext();
    bool isCall = effectRuleOf(value.opcode) == EffectRule::Call;
    bool readsObjects = false;
    bool writesObjects = false;
    if (isCall) {
        Effects beyond = effectsBeyondOperands(context, function, value, onEdges).onAnyEdge();
        if (!beyond.writes.containsOnly(heapsNobodyObserves | heapsWhoseWritesNobodySees))
            return false;
        if (!beyond.reads.containsOnly(heapsNobodyObserves))
            return false;
    } else {
        // What such an operation reads and writes of objects, it reads and writes of the objects that its operands name.
        constexpr AbstractHeapSet heapsOfOperands = heapsNobodyObserves | perObjectHeaps;
        if (!effects.writes.containsOnly(heapsOfOperands) || !effects.reads.containsOnly(heapsOfOperands))
            return false;
        readsObjects = effects.reads.containsAny(perObjectHeaps);
        writesObjects = effects.writes.containsAny(perObjectHeaps);
    }

    bool isFine = true;
    value.forEachOperand([&](ValueID operand, uint32_t index) {
        if (!isFine || !operand || !mayBeReference(typeOf(function, operand)))
            return;
        auto refuse = [&] {
            isFine = false;
        };
        OperandSummary summary = summarizeOperand(context, function, value, index);
        if (summary.uses.containsAny({ OperandUse::Retains, OperandUse::RetainsContents })) {
            if (summary.retainedIn == RetainedIn::UnknownMemory)
                return refuse();
            if (summary.retainedIn == RetainedIn::Operand && !known.made.get(value.operand(summary.retainingOperand).index()))
                return refuse();
        }
        if (summary.uses.containsAny({ OperandUse::ChangesPrototype, OperandUse::ChangesIntegrity }))
            return refuse();
        if (known.made.get(operand.index()))
            return;
        uint32_t declaredIndex = std::min(index, numberOfNamedOperandsOf(value.opcode));
        if (writesObjects && (confinementOf(value.opcode) != OperandConfinement::WritesOnlyNamedOperands || isWrittenOperand(value.opcode, declaredIndex)))
            return refuse();
        if (summary.uses.containsAny(operandWritingUses | unconfinedUses))
            return refuse();
        if (known.unchanging.get(operand.index()) && !summary.uses.containsAny(callingUses))
            return;
        if (readsObjects || !usesOfObjectsMadeElsewhere.containsAll(summary.uses))
            return refuse();
    });
    return isFine;
}

bool MovableCode::mayMove(FunctionAnalyses& analyses, const Knowledge& known, ValueID id)
{
    const Function& function = analyses.function();
    const Value& value = function.at(id);
    switch (value.opcode) {
    case Opcode::LoopHint:
        // The caller has looked at the loop.
        return true;
    case Opcode::NativeBinding:
        // It hands out a function or an object of native code, the same one each time.
        return true;
    case Opcode::GetModuleVariable:
        if (!m_context.variables.holdsOneValue.quickGet(value.variable().index()) && m_context.variables.isSetWhenItsModuleHasRun.quickGet(value.variable().index())) {
            m_variablesTakenToBeSet.append(value.variable());
            return true;
        }
        return m_context.variables.holdsOneValue.quickGet(value.variable().index());
    case Opcode::CheckInitialized: {
        if (!(typeOf(function, value.valueOperand()).primitives & Primitive::Absent))
            return true;
        // The read has noted what it takes for granted.
        const Value& tested = function.at(definitionThroughUpcasts(function, value.valueOperand()));
        if (tested.opcode == Opcode::GetModuleVariable && m_context.variables.isSetWhenItsModuleHasRun.quickGet(tested.variable().index()))
            return true;
        break;
    }
    case Opcode::Check:
        return typeOf(function, value.valueOperand()).isSubsetOf(m_program.at(value.castType()), m_program);
    case Opcode::PropDefine:
    case Opcode::ElemDefine:
    case Opcode::PropSet:
    case Opcode::ElemSet:
        if (storeMayMove(analyses, known, value))
            return true;
        break;
    case Opcode::PropGet:
    case Opcode::PropHas:
    case Opcode::ElemGet:
    case Opcode::ElemHas:
        if (readMayMove(analyses, known, value))
            return true;
        break;
    case Opcode::Add: {
        // A BigInt and a number do not add up.
        auto isFine = [&](ValueID operand) {
            return becomesStringWithoutCode(function, operand) && !(typeOf(function, operand).primitives & Primitive::BigInt);
        };
        if (isFine(value.operand(0)) && isFine(value.operand(1)))
            return true;
        break;
    }
    case Opcode::Strcat:
    case Opcode::ToString:
    case Opcode::ToPropertyKey: {
        bool isFine = true;
        value.forEachOperand([&](ValueID operand, uint32_t) {
            isFine &= becomesStringWithoutCode(function, operand) || (value.opcode == Opcode::ToPropertyKey && isPrimitiveKey(function, operand));
        });
        if (isFine)
            return true;
        break;
    }
    case Opcode::GetEnvironmentField: {
        const LocationFlow& location = m_program.at(value.layout()).fields[value.field()].flow.location;
        if (location.writers == WriterState::AllKnown && location.stores != StoreCount::Unknown && location.initialization != InitializationState::Unknown)
            return true;
        break;
    }
    case Opcode::ToObject:
        if (typeOf(function, value.operand(0)).isObject())
            return true;
        break;
    case Opcode::ArraySpeciesCreate:
        // The constructor of a pristine array is Array, whose species cannot be replaced.
        if (hasOnlyPristineArrays(typeOf(function, value.operand(1))) && typeOf(function, value.operand(0)).isInteger() && typeOf(function, value.operand(0)).integers.isSubsetOf(Range { 0, UINT32_MAX }))
            return true;
        break;
    case Opcode::NewArrayLike:
    case Opcode::NewContiguousArray:
        // No length of an array is refused as the length of another.
        if (typeOf(function, value.operand(0)).isInteger() && typeOf(function, value.operand(0)).integers.isSubsetOf(Range { 0, UINT32_MAX }))
            return true;
        break;
    case Opcode::AppendElement:
    case Opcode::AppendHole:
        return known.made.get(value.operand(0).index());
    case Opcode::AppendSpread:
    case Opcode::AppendElements:
        if (known.made.get(value.operand(0).index()) && known.mayReadFrom(value.operand(1)) && (isPristineArray(function, known, value.operand(1)) || isCollectionAsMade(function, known, value.operand(1))))
            return true;
        break;
    case Opcode::CopyDataProperties: {
        ValueID target = value.operand(0);
        ValueID source = value.operand(1);
        const Type& sourceType = typeOf(function, source);
        auto isPlain = [&](ValueID object) {
            return hasOnlyPlainObjects(typeOf(function, object)) && (isKnownToStayPlain(typeOf(function, object)) || isOnlyFilledHere(analyses, object));
        };
        if (known.made.get(target.index()) && isPlain(target) && (!sourceType.hasObjects() || (known.mayReadFrom(source) && isPlain(source))) && value.numberOfOperands() == 2)
            return true;
        break;
    }
    case Opcode::ResolveHeritage:
        if (hasOnlyClassesAndBuiltins(typeOf(function, value.heritageOperand())))
            return true;
        break;
    case Opcode::GetConstructPrototype:
        if (hasOnlyClassesAndBuiltins(typeOf(function, value.operand(0))) && !typeOf(function, value.operand(0)).primitives)
            return true;
        break;
    case Opcode::NewClass:
        return known.made.get(value.constructorOperand().index());
    case Opcode::DefineMethod:
        return known.made.get(value.classObjectOperand().index()) && (value.key() || isPrimitiveKey(function, value.computedKeyOperand()));
    case Opcode::CallBuiltin:
        if (knownCondition(function, id) || builtinCallMayMove(analyses, known, id))
            return true;
        return false;
    case Opcode::StrictEq:
        // It runs no code. What is left to throw is a rope too long for memory.
        return true;
    case Opcode::Call:
    case Opcode::Construct:
    case Opcode::CallDirect:
        if (std::optional<CalleeAnswer> answer = answerForCall(function, known, value); answer && answer->mayMove)
            return true;
        return false;
    default:
        // What a summary says of the memory that a callee touches does not tell the clock or the random numbers from bookkeeping.
        if (effectRuleOf(value.opcode) == EffectRule::Call || effectRuleOf(value.opcode) == EffectRule::Builtin)
            return false;
        break;
    }
    return mayMoveByEffects(analyses, known, id);
}

uint32_t BodyScanner::find(uint32_t item)
{
    while (m_parents[item] != item) {
        m_parents[item] = m_parents[m_parents[item]];
        item = m_parents[item];
    }
    return item;
}

void BodyScanner::unite(uint32_t a, uint32_t b)
{
    a = find(a);
    b = find(b);
    if (a == b)
        return;
    // The earlier item stands for the unit.
    if (b < a)
        std::swap(a, b);
    m_parents[b] = a;
}

// The blocks that every run passes lie on the way down the dominator tree to the block that returns.
bool BodyScanner::findItems()
{
    CFG& cfg = m_analyses.cfg();
    BlockID last;
    for (BlockID block : cfg.reversePostOrder()) {
        if (!returnsToCaller(m_function.at(block).terminator.kind))
            continue;
        if (last)
            return false;
        last = block;
    }
    if (!last)
        return false;

    Dominators& dominators = m_analyses.dominators();
    Vector<BlockID, 16> spine;
    for (BlockID block = last;; block = dominators.idom(block)) {
        spine.append(block);
        if (block == entryBlock)
            break;
    }
    std::ranges::reverse(spine);

    m_itemOf.fill(noItem, m_function.values.size());
    BitVector isOnSpine(m_function.blocks.size());
    for (BlockID block : spine)
        isOnSpine.quickSet(block.index());
    for (size_t position = 0; position < spine.size(); ++position) {
        BlockID id = spine[position];
        const Block& block = m_function.at(id);
        for (uint32_t index = 0; index < block.values.size(); ++index) {
            m_itemOf[block.values[index].index()] = m_items.size();
            m_items.append(Item { .block = id, .index = index, .value = block.values[index] });
        }
        if (position + 1 == spine.size())
            break;

        Item region { .block = id, .index = static_cast<uint32_t>(block.values.size()), .join = spine[position + 1] };
        BitVector seen(m_function.blocks.size());
        Vector<BlockID, 8> worklist;
        auto visit = [&](BlockID successor) {
            if (successor != region.join && !seen.quickSet(successor.index()))
                worklist.append(successor);
        };
        for (BlockID successor : cfg.successors(id))
            visit(successor);
        while (!worklist.isEmpty()) {
            BlockID interior = worklist.takeLast();
            if (isOnSpine.quickGet(interior.index()))
                return false;
            region.blocks.append(interior);
            for (BlockID successor : cfg.successors(interior))
                visit(successor);
        }
        std::ranges::sort(region.blocks, { }, [&](BlockID interior) {
            return cfg.reversePostOrderIndex(interior);
        });
        uint32_t item = m_items.size();
        for (BlockID interior : region.blocks) {
            for (ValueID parameter : m_function.at(interior).parameters)
                m_itemOf[parameter.index()] = item;
            for (ValueID value : m_function.at(interior).values)
                m_itemOf[value.index()] = item;
        }
        for (ValueID parameter : m_function.at(region.join).parameters)
            m_itemOf[parameter.index()] = item;
        m_items.append(WTF::move(region));
    }
    return true;
}

void BodyScanner::noteWhatVariablesHold()
{
    m_variableHolding.fill(VariableID(), m_function.values.size());
    m_storeOfHeldValue.fill(noItem, m_function.values.size());
    for (uint32_t index = 0; index < m_items.size(); ++index) {
        const Item& item = m_items[index];
        if (item.isRegion() || m_function.at(item.value).opcode != Opcode::PutModuleVariable)
            continue;
        const Value& store = m_function.at(item.value);
        m_firstStoreTo.add(store.variable().index(), index);
        if (!m_context.variables.isStoredOnce.quickGet(store.variable().index()))
            continue;
        if (m_context.variables.mayStayEmpty(store.variable()))
            m_isStoredHere.set(store.variable().index());
        for (ValueID held = store.valueOperand();; held = m_function.at(held).operand(0)) {
            if (!m_variableHolding[held.index()]) {
                m_variableHolding[held.index()] = store.variable();
                m_storeOfHeldValue[held.index()] = index;
            }
            if (!isCastWithoutTest(m_function.at(held).opcode))
                break;
        }
    }
}

// An object that several operations take ties them together, and one of them that stays keeps them all. With a variable of its own, those that do not look into it,
// or look into what never changes, only read that variable.
void BodyScanner::noteWhatAddedVariablesMayHold()
{
    for (uint32_t index = 0; index < m_items.size(); ++index) {
        const Item& item = m_items[index];
        if (item.isRegion() || m_variableHolding[item.value.index()] || !m_known.made.get(item.value.index()) || givesTheSameWheneverDone(item.value))
            continue;
        if (!mayBeReference(m_program.typeOf(m_function, item.value)) || m_function.at(item.value).origin().inlineFrame)
            continue;
        unsigned numberOfUsers = 0;
        for (const Use& use : useLists().usesOf(item.value)) {
            bool isAlias = use.kind == Use::Kind::Operand && (isCast(m_function.at(use.user).opcode) || m_function.at(use.user).opcode == Opcode::FinishClass || m_function.at(use.user).opcode == Opcode::NewClass);
            numberOfUsers += !isAlias;
        }
        if (numberOfUsers < 2)
            continue;
        m_variableHolding[item.value.index()] = VariableID(m_program.variables.size() + item.value.index());
        m_storeOfHeldValue[item.value.index()] = index;
    }
}

// A constant, a read of a variable that holds one value, and a cast of either.
// The test for the dead zone of a variable that is stored once stays where it is. Behind it the variable holds its value for good.
bool BodyScanner::givesTheSameWheneverDone(ValueID id) const
{
    for (;; id = m_function.at(id).operand(0)) {
        const Value& value = m_function.at(id);
        if (value.origin().inlineFrame)
            return false;
        if (isRematerializable(value))
            return true;
        // A read behind the store of this body goes where the store goes.
        if (value.opcode == Opcode::GetModuleVariable)
            return m_context.variables.holdsOneValue.quickGet(value.variable().index()) && !m_isStoredHere.get(value.variable().index());
        if (leavesDeadZoneBehind(m_program, m_function, value)) {
            const Value& tested = m_function.at(value.valueOperand());
            if (tested.opcode == Opcode::GetModuleVariable && m_context.variables.isStoredOnce.quickGet(tested.variable().index()) && !tested.origin().inlineFrame
                && m_program.at(tested.variable()).initialization == VariableInitialization::Uninitialized && !m_isStoredHere.get(tested.variable().index()))
                return true;
            if (m_program.typeOf(m_function, value.valueOperand()).primitives & Primitive::Absent)
                return false;
            continue;
        }
        if (!isCastWithoutTest(value.opcode) || value.numberOfOperands() != 1)
            return false;
    }
}

void BodyScanner::forEachValueTakenBy(const Item& item, NOESCAPE const Invocable<void(ValueID)> auto& functor)
{
    if (!item.isRegion()) {
        m_function.at(item.value).forEachOperand([&](ValueID operand, uint32_t) {
            if (operand)
                functor(operand);
        });
        return;
    }
    BitVector isInside(m_function.blocks.size());
    for (BlockID block : item.blocks)
        isInside.quickSet(block.index());
    auto take = [&](ValueID id) {
        if (!id)
            return;
        ValueLocation location = useLists().locationOf(id);
        if (location.placement == ValuePlacement::Detached || !isInside.quickGet(location.block.index()))
            functor(id);
    };
    auto takeOfTerminator = [&](const Block& block) {
        block.terminator.forEachOperand([&](ValueID operand, uint32_t) {
            take(operand);
        });
        for (const Target& target : block.terminator.targets) {
            for (ValueID argument : target.arguments)
                take(argument);
        }
    };
    takeOfTerminator(m_function.at(item.block));
    for (BlockID id : item.blocks) {
        const Block& block = m_function.at(id);
        for (ValueID value : block.values) {
            m_function.at(value).forEachOperand([&](ValueID operand, uint32_t) {
                take(operand);
            });
        }
        takeOfTerminator(block);
    }
}

bool BodyScanner::mayMove(const Item& item, const Knowledge& known)
{
    auto valueMayMove = [&](ValueID id) {
        const Value& value = m_function.at(id);
        if (value.origin().inlineFrame)
            return false;
        if (isRematerializable(value) || m_code.mayMove(m_analyses, known, id))
            return true;
        return false;
    };
    if (!item.isRegion())
        return valueMayMove(item.value);

    auto terminatorMayMove = [&](BlockID block) {
        if (m_function.at(block).terminator.origin().inlineFrame || !m_code.terminatorMayMove(m_analyses, block))
            return false;
        return true;
    };
    if (!terminatorMayMove(item.block))
        return false;
    for (BlockID id : item.blocks) {
        const Block& block = m_function.at(id);
        if (block.handler || block.isHandlerBlock() || m_mayRunForever.quickGet(id.index()))
            return false;
        if (!std::ranges::all_of(block.values, valueMayMove) || !terminatorMayMove(id))
            return false;
    }
    return true;
}

bool BodyScanner::areSetInFrontOf(uint32_t item, std::span<const VariableID> variables)
{
    ModuleID evaluated = m_context.moduleWithBody[m_id.index()];
    return std::ranges::all_of(variables, [&](VariableID id) {
        ModuleID module = m_program.at(id).module;
        if (module != (evaluated ? evaluated : m_function.module))
            return evaluated && m_context.hasRunBefore(module, evaluated, m_modulesImported);
        // The store of this body stands in front of the item.
        auto store = m_firstStoreTo.find(id.index());
        return store != m_firstStoreTo.end() && store->value < item;
    });
}

bool BodyScanner::runsCompiledCode(const Item& item)
{
    auto runs = [&](ValueID id) {
        const Value& value = m_function.at(id);
        return effectRuleOf(value.opcode) == EffectRule::Call || m_analyses.mayRunOtherCodeOnAnyEdge(id);
    };
    if (!item.isRegion())
        return runs(item.value);
    return std::ranges::any_of(item.blocks, [&](BlockID block) {
        return std::ranges::any_of(m_function.at(block).values, runs);
    });
}

// About how many instructions the start saves.
unsigned BodyScanner::estimatedCostOf(const Item& item) const
{
    constexpr unsigned costOfCode = 1000;
    if (item.isRegion())
        return costOfCode;
    const Value& value = m_function.at(item.value);
    if (effectRuleOf(value.opcode) == EffectRule::Call || effectRuleOf(value.opcode) == EffectRule::Builtin)
        return costOfCode;
    switch (value.opcode) {
    case Opcode::NewClass:
    case Opcode::NativeBinding:
        return costOfCode;
    case Opcode::DefineMethod:
    case Opcode::PropDefine:
    case Opcode::PropSet:
    case Opcode::ElemDefine:
    case Opcode::ElemSet:
    case Opcode::AppendSpread:
    case Opcode::AppendElements:
    case Opcode::CopyDataProperties:
        return 50;
    default:
        break;
    }
    if (interpretedOf(value.opcode) == Interpretation::Allocation || value.opcode == Opcode::NewDictionary)
        return 20 + 2 * value.numberOfOperands();
    return 1;
}

void BodyScanner::keep(uint32_t item)
{
    m_isKept.quickSet(find(item));
}

void BodyScanner::classify(uint32_t index)
{
    const Item& item = m_items[index];
    const Block& block = m_function.at(item.block);
    bool isInPlaceThatMoves = !block.handler && !block.isHandlerBlock() && m_analyses.executionCountOf(item.block) == ExecutionCount::AtMostOnce;

    auto stay = [&] {
        m_states[index] = State::Stays;
        forEachValueTakenBy(item, [&](ValueID taken) {
            // What is done again reads a variable, which code that stays reads at the start.
            for (ValueID link = taken;; link = m_function.at(link).operand(0)) {
                uint32_t definingItem = m_itemOf[link.index()];
                if (definingItem != noItem && m_states[definingItem] == State::Moves) {
                    m_itemsUsedByCodeThatStays.append(definingItem);
                    break;
                }
                if (definingItem == noItem || m_states[definingItem] != State::IsRepeated || !m_function.at(link).numberOfOperands())
                    break;
            }
        });
    };

    if (!item.isRegion() && givesTheSameWheneverDone(item.value) && !m_variableHolding[item.value.index()]) {
        m_states[index] = State::IsRepeated;
        return;
    }
    if (!isInPlaceThatMoves)
        return stay();

    if (!item.isRegion() && m_function.at(item.value).opcode == Opcode::GetModuleVariable && m_isStoredHere.get(m_function.at(item.value).variable().index())) {
        VariableID variable = m_function.at(item.value).variable();
        for (uint32_t earlier = 0; earlier < index; ++earlier) {
            const Item& store = m_items[earlier];
            if (store.isRegion() || m_function.at(store.value).opcode != Opcode::PutModuleVariable || m_function.at(store.value).variable() != variable)
                continue;
            if (m_states[earlier] != State::Moves)
                break;
            m_states[index] = State::Moves;
            unite(index, earlier);
            return;
        }
        return stay();
    }

    bool isStore = !item.isRegion() && m_function.at(item.value).opcode == Opcode::PutModuleVariable;
    if (isStore && !m_context.variables.mayStayEmpty(m_function.at(item.value).variable()))
        return stay();

    // First as if what variables hold were made elsewhere: then the item only reads those variables.
    bool looksIntoWhatVariablesHold = false;
    m_code.takeVariablesTakenToBeSet();
    if (!isStore && !mayMove(item, m_knownIfVariablesHoldWhatWasMadeElsewhere)) {
        looksIntoWhatVariablesHold = true;
        if (!mayMove(item, m_known))
            return stay();
    }
    if (!areSetInFrontOf(index, m_code.takeVariablesTakenToBeSet().span()))
        return stay();

    bool takesWhatStays = false;
    Vector<uint32_t, 8> united;
    Vector<ValueID, 4> read;
    forEachValueTakenBy(item, [&](ValueID taken) {
        uint32_t definingItem = m_itemOf[taken.index()];
        if (definingItem == noItem) {
            takesWhatStays = true;
            return;
        }
        if (definingItem == index || m_states[definingItem] == State::IsRepeated)
            return;
        bool isAlias = !item.isRegion() && (isCast(m_function.at(item.value).opcode) || m_function.at(item.value).opcode == Opcode::FinishClass);
        bool mayReadVariable = m_variableHolding[taken.index()] && m_storeOfHeldValue[taken.index()] < index && !looksIntoWhatVariablesHold && !isStore && !isAlias;
        if (mayReadVariable) {
            read.append(taken);
            return;
        }
        if (m_states[definingItem] == State::Stays) {
            takesWhatStays = true;
            return;
        }
        united.append(definingItem);
    });
    if (takesWhatStays)
        return stay();

    m_states[index] = State::Moves;
    for (uint32_t other : united)
        unite(index, other);
    for (ValueID value : read)
        m_readings.append({ index, value });
}

// A unit that fills no variable is there for code that stays. One that makes nothing costs less than the tests at its reads.
void BodyScanner::keepUnitsNotWorthPuttingOff()
{
    // A function of its own, and a test at each read, cost more than a small literal.
    constexpr unsigned leastCost = 100;
    BitVector fillsVariable(m_items.size());
    Vector<unsigned> costs(FillWith { }, m_items.size(), 0u);
    for (uint32_t index = 0; index < m_items.size(); ++index) {
        if (m_states[index] != State::Moves)
            continue;
        const Item& item = m_items[index];
        if (!item.isRegion() && m_function.at(item.value).opcode == Opcode::PutModuleVariable)
            fillsVariable.quickSet(find(index));
        costs[find(index)] += estimatedCostOf(item);
    }
    for (const Reading& reading : m_readings) {
        uint32_t definingItem = m_itemOf[reading.value.index()];
        if (isHeldByAddedVariable(reading.value) && m_states[definingItem] == State::Moves)
            fillsVariable.quickSet(find(definingItem));
    }
    for (uint32_t index = 0; index < m_items.size(); ++index) {
        if (m_states[index] != State::Moves || find(index) != index)
            continue;
        if (!fillsVariable.quickGet(index))
            keep(index);
        else if (costs[index] < leastCost)
            keep(index);
    }
}

// What code that stays takes is needed at the start, and so is all that was put off with it and all that those operations take.
void BodyScanner::keepUnitsNeededAtStart()
{
    for (uint32_t item : m_itemsUsedByCodeThatStays)
        keep(item);
    for (bool hasChanged = true; hasChanged;) {
        hasChanged = false;
        // What reads comes behind what it reads: from the last one on, one round follows a whole chain.
        for (const Reading& reading : m_readings.span() | std::views::reverse) {
            uint32_t definingItem = m_itemOf[reading.value.index()];
            if (m_states[definingItem] != State::Moves || !m_isKept.quickGet(find(reading.item)) || m_isKept.quickGet(find(definingItem)))
                continue;
            keep(definingItem);
            hasChanged = true;
        }
    }
}

// Between the first store of a unit and its last operation the program sees the objects half done. Put off, they are never seen so.
void BodyScanner::keepUnitsThatOtherCodeMaySeeHalfDone()
{
    Vector<uint32_t> firstStore(FillWith { }, m_items.size(), noItem);
    Vector<uint32_t> lastItem(FillWith { }, m_items.size(), 0u);
    for (uint32_t index = 0; index < m_items.size(); ++index) {
        if (m_states[index] != State::Moves)
            continue;
        uint32_t unit = find(index);
        lastItem[unit] = index;
        const Item& item = m_items[index];
        if (firstStore[unit] == noItem && !item.isRegion() && m_function.at(item.value).opcode == Opcode::PutModuleVariable)
            firstStore[unit] = index;
    }
    Vector<uint32_t> itemsThatRunCode;
    for (uint32_t index = 0; index < m_items.size(); ++index) {
        if (m_states[index] != State::IsRepeated && runsCompiledCode(m_items[index]))
            itemsThatRunCode.append(index);
    }
    for (uint32_t unit = 0; unit < m_items.size(); ++unit) {
        if (m_states[unit] != State::Moves || find(unit) != unit || firstStore[unit] == noItem || m_isKept.quickGet(unit))
            continue;
        auto first = std::ranges::upper_bound(itemsThatRunCode, firstStore[unit]);
        for (; first != itemsThatRunCode.end() && *first < lastItem[unit]; ++first) {
            if (m_states[*first] == State::Moves && find(*first) == unit)
                continue;
            keep(unit);
            break;
        }
    }
}

void BodyScanner::collectUnits()
{
    Vector<uint32_t> unitAt(FillWith { }, m_items.size(), noItem);
    Vector<uint32_t, 16> representatives;
    size_t first = m_found.size();
    for (uint32_t index = 0; index < m_items.size(); ++index) {
        if (m_states[index] != State::Moves || m_isKept.quickGet(find(index)))
            continue;
        uint32_t& position = unitAt[find(index)];
        if (position == noItem) {
            position = m_found.size();
            m_found.append(Unit { .body = m_id });
            representatives.append(find(index));
        }
        Unit& unit = m_found[position];
        const Item& item = m_items[index];
        unit.items.append(item);
        if (!item.isRegion() && m_function.at(item.value).opcode == Opcode::PutModuleVariable)
            unit.variables.append(m_function.at(item.value).variable());
    }

    BitVector isTaken(m_function.values.size());
    for (size_t position = first; position < m_found.size(); ++position) {
        Unit& unit = m_found[position];
        isTaken.clearAll();
        uint32_t representative = representatives[position - first];
        for (const Item& item : unit.items) {
            forEachValueTakenBy(item, [&](ValueID taken) {
                uint32_t definingItem = m_itemOf[taken.index()];
                if (m_states[definingItem] == State::Moves && find(definingItem) == representative)
                    return;
                if (isTaken.quickSet(taken.index()))
                    return;
                if (m_states[definingItem] == State::IsRepeated)
                    unit.repeated.append(taken);
                else
                    unit.valuesOfVariables.append({ taken, m_variableHolding[taken.index()] });
            });
        }
    }

    isTaken.clearAll();
    Unit whatStays { .body = m_id, .staysInBody = true };
    size_t end = m_found.size();
    for (size_t position = first; position < end; ++position) {
        for (const ValueOfVariable& taken : m_found[position].valuesOfVariables) {
            if (!isHeldByAddedVariable(taken.value) || isTaken.quickSet(taken.value.index()))
                continue;
            uint32_t definingItem = m_itemOf[taken.value.index()];
            bool moves = m_states[definingItem] == State::Moves && !m_isKept.quickGet(find(definingItem));
            Unit& maker = moves ? m_found[unitAt[find(definingItem)]] : whatStays;
            maker.addedVariables.append(taken);
            if (moves)
                maker.variables.append(taken.variable);
        }
    }
    if (!whatStays.addedVariables.isEmpty())
        m_found.append(WTF::move(whatStays));
}

void BodyScanner::run()
{
    if (m_function.isUnsplitCoroutine() || !m_context.flow.runsAtMostOnce(m_program, m_id))
        return;
    bool storesSuchVariable = std::ranges::any_of(m_function.blocks, [&](const Block& block) {
        return std::ranges::any_of(block.values, [&](ValueID id) {
            const Value& value = m_function.at(id);
            return value.opcode == Opcode::PutModuleVariable && m_context.variables.mayStayEmpty(value.variable());
        });
    });
    if (!storesSuchVariable || !findItems())
        return;

    m_known = m_code.findKnowledge(m_analyses, { });
    noteWhatVariablesHold();
    noteWhatAddedVariablesMayHold();
    BitVector isHeld(m_function.values.size());
    for (size_t index = 0; index < m_variableHolding.size(); ++index) {
        if (m_variableHolding[index])
            isHeld.quickSet(index);
    }
    m_knownIfVariablesHoldWhatWasMadeElsewhere = m_code.findKnowledge(m_analyses, { }, &isHeld);
    m_mayRunForever = m_code.findBlocksThatMayRunForever(m_analyses);

    m_states.fill(State::Stays, m_items.size());
    m_parents = Vector<uint32_t>(m_items.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    m_isKept.ensureSize(m_items.size());
    for (uint32_t index = 0; index < m_items.size(); ++index)
        classify(index);
    m_function.at(m_items.last().block).terminator.forEachOperand([&](ValueID returned, uint32_t) {
        if (returned && m_itemOf[returned.index()] != noItem && m_states[m_itemOf[returned.index()]] == State::Moves)
            m_itemsUsedByCodeThatStays.append(m_itemOf[returned.index()]);
    });
    keepUnitsNotWorthPuttingOff();
    keepUnitsNeededAtStart();
    keepUnitsThatOtherCodeMaySeeHalfDone();
    // A unit that is kept takes what it reads at the start.
    keepUnitsNeededAtStart();
    collectUnits();
}

static Function createHead(const Program& program, const Unit& unit)
{
    const Function& body = program.at(unit.body);
    const Item& first = unit.items.first();
    Function created;
    created.name = program.strings.emptyString();
    created.module = body.module;
    created.position = (first.isRegion() ? body.at(first.block).terminator.origin() : body.at(first.value).origin()).position;
    created.kind = FunctionKind::Plain;
    created.role = FunctionRole::OutlinedBody;
    created.flags = body.flags & OptionSet<FunctionFlags> { FunctionFlags::IsSloppy, FunctionFlags::IsChecked };
    created.thisType = DeclaredType { .type = TypeTable::anything };
    created.results.append(DeclaredType { .type = unit.type });
    created.capabilities = initialCapabilitiesOf(created);
    return created;
}

static ValueID assumeType(IRBuilder& builder, ValueID value, TypeID wanted)
{
    const Program& program = builder.context().program;
    TypeID type = builder.function().at(value).type;
    if (builder.types().at(type).isSubsetOf(builder.types().at(wanted), program))
        return value;
    TypeID castType = builder.types().intersectionOf(type, wanted);
    if (builder.context().build.validates())
        builder.append(makeValidateType(value, castType));
    return builder.append(makeAssumeType(value, castType));
}

namespace {

class UnitBuilder {
public:
    UnitBuilder(const AnalysisContext& context, const Unit& unit, Function& created, TypeArena& types)
        : m_body(context.program.at(unit.body))
        , m_unit(unit)
        , m_builder(context, created, types)
    {
    }

    void build();

private:
    ValueID copyOf(ValueID id) const
    {
        auto found = m_values.find(id.index());
        RELEASE_ASSERT(found != m_values.end());
        return found->value;
    }

    BlockID copyOf(BlockID id) const
    {
        auto found = m_blocks.find(id.index());
        RELEASE_ASSERT(found != m_blocks.end());
        return found->value;
    }

    void repeat(ValueID);
    void copyValue(ValueID);
    BlockID addBlockLike(BlockID);
    void copyTerminator(const Terminator&);
    void copyRegion(const Item&);

    using Copies = HashMap<uint32_t, ValueID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    using BlockCopies = HashMap<uint32_t, BlockID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

    const Function& m_body;
    const Unit& m_unit;
    IRBuilder m_builder;
    Copies m_values;
    BlockCopies m_blocks;
};

} // anonymous namespace

// A cast comes behind what it casts. The test for a dead zone has passed where it stands.
void UnitBuilder::repeat(ValueID id)
{
    if (m_values.contains(id.index()))
        return;
    const Value& original = m_body.at(id);
    if (original.numberOfOperands())
        repeat(original.operand(0));
    m_builder.setOrigin(original.origin());
    if (original.opcode == Opcode::CheckInitialized) {
        m_values.add(id.index(), assumeType(m_builder, copyOf(original.operand(0)), original.type));
        return;
    }
    copyValue(id);
}

void UnitBuilder::copyValue(ValueID id)
{
    Value value(m_body.at(id));
    m_builder.setOrigin(value.origin());
    // The variable may stay empty from now on, and its type says so. This read finds it filled, as it did.
    if (value.opcode == Opcode::GetModuleVariable) {
        Value read = IRBuilder::make(Opcode::GetModuleVariable);
        read.setVariable(value.variable());
        m_values.add(id.index(), assumeType(m_builder, m_builder.append(WTF::move(read)), value.type));
        return;
    }
    value.forEachOperand([&](ValueID& operand, uint32_t) {
        if (operand)
            operand = copyOf(operand);
    });
    m_values.add(id.index(), m_builder.append(WTF::move(value)));
}

BlockID UnitBuilder::addBlockLike(BlockID original)
{
    const Block& block = m_body.at(original);
    BlockID added = m_builder.addBlock(block.frequency);
    m_builder.function().edit(added).shape = block.shape;
    for (ValueID parameter : block.parameters) {
        ValueID copy = m_builder.addParameter(added, m_body.at(parameter).type);
        m_builder.function().edit(copy).setExpected(m_body.at(parameter).expected());
        m_values.add(parameter.index(), copy);
    }
    m_blocks.add(original.index(), added);
    return added;
}

void UnitBuilder::copyTerminator(const Terminator& terminator)
{
    Terminator copy(terminator);
    copy.forEachOperand([&](ValueID& operand, uint32_t) {
        if (operand)
            operand = copyOf(operand);
    });
    for (Target& target : copy.targets) {
        target.block = copyOf(target.block);
        for (ValueID& argument : target.arguments)
            argument = copyOf(argument);
    }
    m_builder.setOrigin(terminator.origin());
    m_builder.emitTerminator(WTF::move(copy));
}

void UnitBuilder::copyRegion(const Item& region)
{
    for (BlockID block : region.blocks)
        addBlockLike(block);
    BlockID join = addBlockLike(region.join);
    copyTerminator(m_body.at(region.block).terminator);
    for (BlockID block : region.blocks) {
        m_builder.moveToEndOf(copyOf(block));
        for (ValueID id : m_body.at(block).values)
            copyValue(id);
        copyTerminator(m_body.at(block).terminator);
    }
    m_builder.moveToEndOf(join);
}

void UnitBuilder::build()
{
    const Item& first = m_unit.items.first();
    m_builder.setOrigin(first.isRegion() ? m_body.at(first.block).terminator.origin() : m_body.at(first.value).origin());
    m_builder.moveToEndOf(m_builder.addBlock());

    for (ValueID id : m_unit.repeated)
        repeat(id);
    for (const ValueOfVariable& taken : m_unit.valuesOfVariables) {
        Value read = IRBuilder::make(Opcode::GetModuleVariable);
        read.setVariable(taken.variable);
        m_builder.setOrigin(m_body.at(taken.value).origin());
        m_values.add(taken.value.index(), assumeType(m_builder, m_builder.append(WTF::move(read)), m_body.at(taken.value).type));
    }

    ValueID stored;
    for (const Item& item : m_unit.items) {
        if (item.isRegion()) {
            copyRegion(item);
            continue;
        }
        copyValue(item.value);
        if (m_body.at(item.value).opcode == Opcode::PutModuleVariable)
            stored = copyOf(m_body.at(item.value).valueOperand());
    }
    for (const ValueOfVariable& added : m_unit.addedVariables) {
        stored = copyOf(added.value);
        Value store = IRBuilder::make(Opcode::PutModuleVariable, { stored });
        store.setVariable(added.variable);
        m_builder.append(WTF::move(store));
    }
    if (m_unit.variables.size() == 1)
        m_builder.emitReturn(assumeType(m_builder, stored, m_unit.type));
    else
        m_builder.emitReturn(m_builder.appendUndefined());
}

static void removeUnits(const Plan& plan, std::span<const Unit> units, IRBuilder& builder)
{
    if (units.empty())
        return;
    Function& function = builder.function();
    BitVector isRemoved(function.values.size());
    BitVector marksDeclaration(function.values.size());
    HashMap<uint32_t, VariableID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> storedWhereMade;
    for (const Unit& unit : units) {
        if (unit.staysInBody) {
            for (const ValueOfVariable& added : unit.addedVariables)
                storedWhereMade.add(added.value.index(), added.variable);
        }
        for (const Item& item : unit.items) {
            if (!item.isRegion()) {
                bool staysAsMark = function.at(item.value).opcode == Opcode::PutModuleVariable && plan.emptinessOf(function.at(item.value).variable()) != Emptiness::AbsentFromBirth;
                (staysAsMark ? marksDeclaration : isRemoved).quickSet(item.value.index());
                continue;
            }
            for (BlockID id : item.blocks) {
                Block& block = function.edit(id);
                for (ValueID parameter : block.parameters)
                    isRemoved.quickSet(parameter.index());
                for (ValueID value : block.values)
                    isRemoved.quickSet(value.index());
                block = Block { };
            }
            Block& join = function.edit(item.join);
            for (ValueID parameter : join.parameters)
                isRemoved.quickSet(parameter.index());
            join.parameters.clear();
            Origin origin = function.at(item.block).terminator.origin();
            Terminator jump = IRBuilder::makeTerminator(TerminatorKind::Jump);
            jump.setOrigin(origin);
            jump.targets.append(Target { item.join, { }, { } });
            function.edit(item.block).terminator = WTF::move(jump);
        }
    }
    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        BlockValues values = std::exchange(function.edit(BlockID(index), BlockEdit::Values).values, { });
        BlockValues kept;
        kept.reserveInitialCapacity(values.size());
        for (ValueID id : values) {
            if (isRemoved.quickGet(id.index()))
                continue;
            if (marksDeclaration.quickGet(id.index())) {
                builder.setOrigin(function.at(id).origin());
                bool isAbsent = plan.emptinessOf(function.at(id).variable()) == Emptiness::AbsentFromDeclaration;
                ValueID mark = builder.addDetached(IRBuilder::make(isAbsent ? Opcode::Absent : Opcode::Undefined));
                function.edit(id).setValueOperand(mark);
                kept.append(mark);
            }
            kept.append(id);
            if (auto found = storedWhereMade.find(id.index()); found != storedWhereMade.end()) {
                builder.setOrigin(function.at(id).origin());
                Value store = IRBuilder::make(Opcode::PutModuleVariable, { id });
                store.setVariable(found->value);
                kept.append(builder.addDetached(WTF::move(store)));
            }
        }
        function.edit(BlockID(index)).values = WTF::move(kept);
    }
    for (size_t index : isRemoved)
        becomeUndefined(function, ValueID(index));
}

// A read that nothing takes needs no value, and neither does one that only the test for the dead zone takes, whose result nothing takes.
static BitVector findReadsThatNeedNoValue(const Function& function)
{
    Vector<uint32_t> numberOfUses(FillWith { }, function.values.size(), 0u);
    auto count = [&](ValueID id) {
        if (id)
            ++numberOfUses[id.index()];
    };
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values)
            function.at(id).forEachChild(count);
        block.terminator.forEachChild(count);
        for (const Target& target : block.terminator.targets) {
            for (ValueID argument : target.arguments)
                count(argument);
        }
        if (block.handler) {
            for (ValueID argument : block.handler->arguments)
                count(argument);
        }
        count(block.subject);
    }
    BitVector result(function.values.size());
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            if (value.opcode == Opcode::GetModuleVariable && !numberOfUses[id.index()])
                result.quickSet(id.index());
            if (value.opcode != Opcode::CheckInitialized || numberOfUses[id.index()])
                continue;
            ValueID tested = value.valueOperand();
            if (function.at(tested).opcode == Opcode::GetModuleVariable && numberOfUses[tested.index()] == 1)
                result.quickSet(tested.index());
        }
    }
    return result;
}

static void fillAtRead(const Plan& plan, const Unit& unit, VariableID variable, IRBuilder& builder, ProgramPoint point)
{
    bool isMarkedByUndefined = plan.emptinessOf(variable) == Emptiness::UndefinedFromDeclaration;
    auto appendRead = [&] {
        Value read = IRBuilder::make(Opcode::GetModuleVariable);
        read.setVariable(variable);
        return builder.append(WTF::move(read));
    };
    IRBuilder::Expansion expansion = builder.beginExpansion(point);
    ValueID held = appendRead();
    IRBuilder::BranchBlocks branch = builder.emitBranch(builder.appendTypeTest(held, isMarkedByUndefined ? TypeTable::undefined : TypeTable::absent), BlockFrequency::Rare, BlockFrequency::Normal);

    builder.moveToEndOf(branch.taken);
    Value call = IRBuilder::make(Opcode::CallDirect);
    call.setFunction(unit.function);
    call.setArgumentsOperands({ });
    ValueID made = builder.append(WTF::move(call));
    if (unit.variables.size() != 1) {
        ValueID filled = appendRead();
        TypeID type = builder.types().intern(builder.types().at(builder.function().at(filled).type).withoutPrimitives(isMarkedByUndefined ? Primitive::Absent | Primitive::Undefined : Primitive::Absent));
        made = assumeType(builder, filled, type);
    }
    builder.endExpansion(expansion, made);

    // A cast, which stays behind the test. What the edge of a test hands on is the read itself to the back end, which would take what hangs on it out of a loop and in front of the test.
    builder.moveToEndOf(branch.notTaken);
    TypeID filledType = builder.types().intern(builder.types().at(builder.function().at(held).type).withoutPrimitives(isMarkedByUndefined ? Primitive::Undefined : Primitive::Absent));
    builder.endExpansion(expansion, assumeType(builder, held, filledType));
}

static void fillVariablesAtReads(const Plan& plan, FunctionID id, IRBuilder& builder)
{
    Function& function = builder.function();
    uint32_t numberOfBlocks = function.blocks.size();
    Vector<std::pair<uint32_t, const Unit*>, 8> reads;
    BitVector needsNoValue = findReadsThatNeedNoValue(function);
    // Such a read is left as it is. It has the type of the variable, which may be empty from now on, and the test for the dead zone behind it what that type leaves.
    for (bool retypesTests : { false, true }) {
        for (const Block& block : function.blocks) {
            for (ValueID valueID : block.values) {
                Value& value = function.edit(valueID);
                ValueID read = retypesTests && value.opcode == Opcode::CheckInitialized ? value.valueOperand() : valueID;
                if ((retypesTests && read == valueID) || !needsNoValue.quickGet(read.index()) || !plan.find(function.at(read).variable()))
                    continue;
                if (retypesTests)
                    value.type = builder.types().intern(builder.types().at(function.at(read).type).withoutPrimitives(Primitive::Absent));
                else
                    value.type = builder.context().program.at(value.variable()).declared.type;
            }
        }
    }
    for (uint32_t blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
        BlockID block(blockIndex);
        reads.shrink(0);
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& value = function.at(values[index]);
            const Unit* unit = value.opcode == Opcode::GetModuleVariable ? plan.find(value.variable()) : nullptr;
            // What fills a variable reads it behind its own store.
            if (unit && unit->function != id && !needsNoValue.quickGet(values[index].index()))
                reads.append({ index, unit });
        }
        for (auto [index, unit] : reads.span() | std::views::reverse) {
            const Value& read = function.at(function.at(block).values[index]);
            VariableID variable = read.variable();
            builder.setOrigin(read.origin());
            fillAtRead(plan, *unit, variable, builder, { block, index });
        }
    }
}

void deferInitializers(ProgramEditor& editor, const OptimizationInputs& inputs, const PassRunner& runner)
{
    const Program& program = editor.program();
    const FlowResults& flow = inputs.flow;
    BuildContext& build = runner.build();
    const ParallelLoops& loops = build.parallelLoops();
    if (!flow.hasResults())
        return;
    PhaseTimes times(build, PassID::DeferInitializers);
    Vector<ModuleID> moduleWithBody = findModulesWithBodies(program);
    VariableFacts variables = collectVariableFacts(program, flow, moduleWithBody, build);
    times.didFinish("collectVariableFacts"_s);
    if (std::ranges::all_of(variables.emptiness, [](Emptiness emptiness) { return emptiness == Emptiness::None; }))
        return;
    // A module that requires its importer while its own body runs closes a circle too.
    Vector<std::pair<ModuleID, ModuleID>> requiredModules = collectInParallel<std::pair<ModuleID, ModuleID>>(loops, program.functions.size(), [&](unsigned index, Vector<std::pair<ModuleID, ModuleID>>& found) {
        forEachValueListedOrNot(program, FunctionID(index), { Opcode::RequireModule, Opcode::EvaluateModule }, [&](ValueID, const Value& value) {
            const Function& function = program.functions[index];
            if (value.opcode == Opcode::RequireModule && function.module)
                found.append({ function.module, value.module() });
            if (value.opcode == Opcode::EvaluateModule)
                found.append({ value.referrer(), value.module() });
        });
    });
    Vector<Vector<ModuleID>> modulesReachedFrom(program.modules.size(), [&](size_t index) {
        return program.modules[index].requests.map([](const ModuleRequest& request) {
            return request.target;
        });
    });
    for (auto [from, to] : requiredModules)
        modulesReachedFrom[from.index()].append(to);
    StronglyConnectedComponents circlesOfImports = computeStronglyConnectedComponents(program.modules.size(), [&](unsigned index) -> const Vector<ModuleID>& {
        return modulesReachedFrom[index];
    });
    ScanContext context { program, inputs, flow, inputs.effects, runner, build, variables, circlesOfImports, moduleWithBody };

    Plan plan;
    plan.emptiness = &variables.emptiness;
    Vector<uint32_t> bodies = findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        if (!mayListValueWith(program, FunctionID(index), { Opcode::PutModuleVariable }))
            return false;
        const Function& function = program.functions[index];
        if (!function.hasBody() || function.isUnsplitCoroutine() || !flow.runsAtMostOnce(program, FunctionID(index)) || !build.isEnabled(PassID::DeferInitializers, FunctionID(index)))
            return false;
        bool storesSuchVariable = false;
        forEachListedName(program, FunctionID(index), { Opcode::PutModuleVariable }, [&](BlockID, ValueID, Opcode, NameKind kind, uint32_t name) {
            storesSuchVariable |= kind == NameKind::Variable && variables.mayStayEmpty(VariableID(name));
        });
        return storesSuchVariable;
    });
    {
        // What is made to look at a callee is thrown away.
        auto phases = makeUnique<InterningPhases>(editor, bodies.size());
        {
            ProgramCaches caches(program, build);
            // Each body is a piece of work of its own.
            Vector<uint32_t> longestFirst(bodies.size(), [](size_t index) {
                return static_cast<uint32_t>(index);
            });
            std::ranges::sort(longestFirst, std::ranges::greater { }, [&](uint32_t index) {
                return std::pair { program.functions[bodies[index]].values.size(), index };
            });
            plan.units = collectInParallel<Unit>(loops, bodies.size(), longestFirst.span(), [&](unsigned index, Vector<Unit>& found) {
                TaskArenas arenas(*phases, index);
                BodyScanner(context, FunctionID(bodies[index]), arenas, found).run();
            });
        }
        phases->discard();
        build.destroyLater(WTF::move(phases));
    }
    times.didFinish("scanBodies"_s);
    if (plan.units.isEmpty())
        return;

    // The scan has named the variables to add by the values that they hold.
    size_t numberOfVariablesBefore = program.variables.size();
    HashMap<uint32_t, VariableID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> addedInBody;
    for (size_t first = 0; first < plan.units.size();) {
        size_t end = first;
        while (end < plan.units.size() && plan.units[end].body == plan.units[first].body)
            ++end;
        std::span<Unit> unitsOfBody = plan.units.mutableSpan().subspan(first, end - first);
        first = end;
        addedInBody.clear();
        const Function& body = program.at(unitsOfBody.front().body);
        for (Unit& unit : unitsOfBody) {
            for (ValueOfVariable& added : unit.addedVariables) {
                Variable variable;
                variable.module = body.module;
                variable.declared = DeclaredType { .type = body.at(added.value).type };
                variable.initialization = VariableInitialization::Uninitialized;
                variable.flow.writers = WriterState::AllKnown;
                variable.flow.stores = StoreCount::AtMostOnce;
                variable.flow.initialization = InitializationState::StoredBeforeEveryRead;
                addedInBody.add(added.variable.index(), editor.addVariable(WTF::move(variable)));
            }
        }
        if (addedInBody.isEmpty())
            continue;
        auto rename = [&](VariableID& id) {
            if (id.index() >= numberOfVariablesBefore)
                id = addedInBody.get(id.index());
        };
        for (Unit& unit : unitsOfBody) {
            for (VariableID& id : unit.variables)
                rename(id);
            for (ValueOfVariable& added : unit.addedVariables)
                rename(added.variable);
            for (ValueOfVariable& taken : unit.valuesOfVariables)
                rename(taken.variable);
        }
    }

    Vector<FunctionID> created;
    Vector<uint32_t> unitOfCreated;
    for (uint32_t index = 0; index < plan.units.size(); ++index) {
        Unit& unit = plan.units[index];
        if (unit.staysInBody)
            continue;
        unit.type = TypeTable::undefined;
        if (unit.variables.size() == 1) {
            const DeclaredType& declared = program.at(unit.variables.first()).declared;
            unit.type = declared.flags.contains(PositionFlags::IsExpected) ? TypeTable::anything : editor.types().intern(program.at(declared.type).withoutPrimitives(Primitive::Absent));
        }
        unit.function = editor.addFunctionLike(unit.body, createHead(program, unit));
        created.append(unit.function);
        unitOfCreated.append(index);
        for (VariableID id : unit.variables) {
            plan.byVariable.append({ id, index });
            Variable& variable = editor.at(id);
            Emptiness emptiness = plan.emptinessOf(id);
            variable.declared.type = editor.types().intern(program.at(variable.declared.type).withPrimitives(emptiness == Emptiness::UndefinedFromDeclaration ? Primitive::Absent | Primitive::Undefined : Primitive::Absent));
            if (emptiness == Emptiness::AbsentFromBirth)
                variable.initialization = VariableInitialization::Uninitialized;
            else
                variable.flow.stores = StoreCount::Unknown;
            variable.flow.initialization = InitializationState::Unknown;
            variable.flow.isUnchangedDuringActivation = false;
        }
    }
    std::ranges::sort(plan.byVariable);
    build.statistics().count(StatisticID::InitializersDeferred, created.size());
    build.statistics().count(StatisticID::VariablesFilledByInitializers, plan.byVariable.size());
    build.statistics().count(StatisticID::VariablesAddedForInitializers, program.variables.size() - numberOfVariablesBefore);

    editFunctionsInParallel(editor, build, created.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        UnitBuilder(AnalysisContext { build, program, &flow, nullptr, id }, plan.units[unitOfCreated[index]], function, arenas.types).build();
    });
    times.didFinish("createFunctions"_s);

    Vector<FunctionID> functions = collectInParallel<FunctionID>(loops, program.functions.size(), [&](unsigned index, Vector<FunctionID>& found) {
        bool readsSuchVariable = false;
        forEachListedName(program, FunctionID(index), { Opcode::GetModuleVariable }, [&](BlockID, ValueID, Opcode, NameKind kind, uint32_t name) {
            readsSuchVariable |= kind == NameKind::Variable && !!plan.find(VariableID(name));
        });
        if (readsSuchVariable)
            found.append(FunctionID(index));
    });
    for (const Unit& unit : plan.units)
        functions.append(unit.body);
    std::ranges::sort(functions);
    removeRepeatedElements(functions);
    editFunctionsInParallel(editor, build, functions.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        IRBuilder builder(AnalysisContext { build, program, &flow, nullptr, id }, function, arenas.types);
        removeUnits(plan, plan.unitsIn(id), builder);
        fillVariablesAtReads(plan, id, builder);
    });
    times.didFinish("fillVariablesAtReads"_s);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
