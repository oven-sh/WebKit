/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramPasses.h"

#if ENABLE(AOT)

#include "AOTAddGenericVersion.h"
#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinCalls.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTClaims.h"
#include "AOTDeferDefinitions.h"
#include "AOTDefinitions.h"
#include "AOTDescriptorLiteral.h"
#include "AOTEdgeResults.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowApply.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTIRBuilder.h"
#include "AOTKnownValue.h"
#include "AOTNarrowConstantVariables.h"
#include "AOTPassRunner.h"
#include "AOTPhaseTimes.h"
#include "AOTPredictedValues.h"
#include "AOTProgramCaches.h"
#include "AOTSlotPlan.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTThreshold.h"
#include "AOTValueIdentity.h"
#include "AOTWholeLiterals.h"
#include <memory>
#include <ranges>
#include <wtf/HashMap.h>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

template<typename T> using MapByIndex = HashMap<uint32_t, T, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

static Value makeVariableAccess(Opcode opcode, VariableID variable, std::initializer_list<ValueID> operands = { })
{
    Value access = IRBuilder::make(opcode, operands);
    access.setVariable(variable);
    return access;
}

enum class ClaimHolds : uint8_t { WhereverValueIs, FromHereOn };

static ValueID assumeType(IRBuilder& builder, ValueID value, TypeID wanted, ClaimHolds holds = ClaimHolds::WhereverValueIs)
{
    TypeID type = builder.function().at(value).type;
    if (builder.types().at(type).isSubsetOf(builder.types().at(wanted), builder.context().program))
        return value;
    TypeID castType = builder.types().intersectionOf(type, wanted);
    if (builder.context().build.validates())
        builder.append(makeValidateType(value, castType));
    if (holds == ClaimHolds::FromHereOn)
        return builder.append(makeAssumeType(value, builder.append(Opcode::Specified, { }), Proven::behindFact(castType)));
    return builder.append(makeAssumeType(value, Proven::byRulesWithoutChecks(castType)));
}

static bool listsAccessOf(const Program& program, FunctionID id, Opcode opcode, const Invocable<bool(VariableID)> auto& isWanted)
{
    bool listsOne = false;
    forEachListedName(program, id, { opcode }, [&](BlockID, ValueID, Opcode, NameKind kind, uint32_t name) {
        listsOne |= kind == NameKind::Variable && isWanted(VariableID(name));
    });
    return listsOne;
}

template<typename Change>
static std::span<const Change> thoseIn(const Vector<Change>& inOrderOfBodies, FunctionID body)
{
    auto range = std::ranges::equal_range(inOrderOfBodies, body, { }, &Change::body);
    return inOrderOfBodies.subspan(range.begin() - inOrderOfBodies.begin(), range.size());
}

template<typename Change>
static Vector<FunctionID> findFunctionsToEdit(const Program& program, const ParallelLoops& loops, const Vector<Change>& changes, bool changesReads, const Invocable<bool(VariableID)> auto& isReadOtherwise)
{
    Vector<FunctionID> functions;
    if (changesReads) {
        functions = collectInParallel<FunctionID>(loops, program.functions.size(), [&](unsigned index, Vector<FunctionID>& found) {
            if (listsAccessOf(program, FunctionID(index), Opcode::GetModuleVariable, isReadOtherwise))
                found.append(FunctionID(index));
        });
    }
    for (const Change& change : changes)
        functions.append(change.body);
    AOT::sort(functions);
    removeRepeatedElements(functions);
    return functions;
}

// A store for which an opcode is named stays as a mark and stores a new value of that opcode. A value that is stored where it is made gets a store behind it.
using MembersFirstRead = Vector<BuiltinID, 0>;

static void removeValues(IRBuilder& builder, const BitVector& isRemoved, const MapByIndex<VariableID>& storedWhereMade, const Invocable<std::optional<Opcode>(ValueID)> auto& markStoredBy, const MapByIndex<const MembersFirstRead*>& firstReadsLeftBehind = { })
{
    Function& function = builder.function();
    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        BlockValues values = std::exchange(function.edit(BlockID(index), BlockEdit::Values).values, { });
        BlockValues kept;
        kept.reserveInitialCapacity(values.size());
        for (ValueID id : values) {
            builder.setOrigin(function.at(id).origin());
            if (isRemoved.quickGet(id.index())) {
                if (auto found = firstReadsLeftBehind.find(id.index()); found != firstReadsLeftBehind.end()) {
                    for (BuiltinID member : *found->value) {
                        Value initialization = IRBuilder::make(Opcode::InitializeBuiltin);
                        initialization.setBuiltin(member);
                        kept.append(builder.addDetached(WTF::move(initialization)));
                    }
                }
                continue;
            }
            if (std::optional<Opcode> mark = markStoredBy(id)) {
                kept.append(builder.addDetached(IRBuilder::make(*mark)));
                function.edit(id, ValueEdit::Definition).setValueOperand(kept.last());
            }
            kept.append(id);
            if (auto found = storedWhereMade.find(id.index()); found != storedWhereMade.end())
                kept.append(builder.addDetached(makeVariableAccess(Opcode::PutModuleVariable, found->value, { id })));
        }
        function.edit(BlockID(index), BlockEdit::Values).values = WTF::move(kept);
    }
    for (size_t index : isRemoved)
        becomeUndefined(function, ValueID(index));
}

// For an expansion at each read. What follows a read moves to another block. What precedes it stays where it is.
static void forEachReadBackwards(const Function& function, const Invocable<bool(ValueID, VariableID)> auto& isWanted, const Invocable<void(ProgramPoint, VariableID)> auto& functor)
{
    uint32_t numberOfBlocks = function.blocks.size();
    Vector<uint32_t, 8> reads;
    for (uint32_t blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
        reads.shrink(0);
        const BlockValues& values = function.blocks[blockIndex].values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (function.at(values[index]).opcode == Opcode::GetModuleVariable && isWanted(values[index], function.at(values[index]).variable()))
                reads.append(index);
        }
        for (uint32_t index : reads.span() | std::views::reverse)
            functor({ BlockID(blockIndex), index }, function.at(function.blocks[blockIndex].values[index]).variable());
    }
}

// NarrowConstantVariables.

namespace {

// The value is unknown where the store does not store a primitive constant.
struct ConstantVariableStore {
    VariableID variable;
    KnownValue value;
    std::optional<Vector<KnownValue, 8>> list;
};

} // anonymous namespace

static Vector<ConstantVariableStore> collectConstantStores(const Program& program, const FlowResults& flow, BuildContext& build)
{
    return collectInParallel<ConstantVariableStore>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<ConstantVariableStore>& stores) {
        forEachListedValue(program, FunctionID(index), { Opcode::PutModuleVariable, Opcode::ValidateFirstRun }, [&](BlockID, uint32_t, ValueID, const Value& value) {
            const Variable& variable = program.at(value.variable());
            bool readsSeeOnlyStoredValue = variable.initialization == VariableInitialization::Uninitialized
                || (variable.initialization == VariableInitialization::Undefined && variable.flow.initialization == InitializationState::StoredBeforeEveryRead);
            // A static field is a property of its class too, which has more ways to change than a store.
            if (variable.owner || variable.flow.writers != WriterState::AllKnown || variable.flow.stores != StoreCount::AtMostOnce || variable.declared.isExpected() || !readsSeeOnlyStoredValue)
                return;
            // The type of what a store stores has to lie inside the type of the variable, so the type has to say the constant.
            KnownValue stored;
            std::optional<Vector<KnownValue, 8>> list;
            if (value.opcode == Opcode::PutModuleVariable) {
                stored = knownValueOfType(program.typeOf(program.functions[index], value.valueOperand()));
                list = findConstantsOfWholeLiteral(program, flow, program.functions[index], value.valueOperand());
            }
            stores.append(ConstantVariableStore { value.variable(), stored.isObject() ? KnownValue() : stored, WTF::move(list) });
        });
    });
}

// A second store in the text may be the one that runs.
static ConstantLists collectListsStoredAlone(const Program& program, const Vector<ConstantVariableStore>& stores)
{
    Vector<uint8_t> numberOfStores(FillWith { }, program.variables.size(), uint8_t { 0 });
    for (const ConstantVariableStore& store : stores)
        numberOfStores[store.variable.index()] = std::min(numberOfStores[store.variable.index()] + 1, 2);

    ConstantLists lists;
    for (const ConstantVariableStore& store : stores) {
        if (store.list && numberOfStores[store.variable.index()] == 1)
            lists.entries.append({ store.variable, Vector<KnownValue>(store.list->span()) });
    }
    AOT::sort(lists.entries, { }, &ConstantLists::Entry::variable);
    return lists;
}

ConstantLists collectConstantLists(const Program& program, const FlowResults& flow, BuildContext& build)
{
    return collectListsStoredAlone(program, collectConstantStores(program, flow, build));
}

ConstantLists narrowConstantVariables(ProgramEditor& editor, const FlowResults& flow, BuildContext& build)
{
    struct Contents {
        bool isStored { false };
        bool isOneConstant { true };
        KnownValue constant;
    };
    const Program& program = editor.program();
    Vector<ConstantVariableStore> stores = collectConstantStores(program, flow, build);
    Vector<Contents> contentsOfVariable(program.variables.size());
    for (const ConstantVariableStore& store : stores) {
        Contents& contents = contentsOfVariable[store.variable.index()];
        contents.isOneConstant &= store.value.isKnown() && (!contents.isStored || contents.constant == store.value);
        contents.isStored = true;
        contents.constant = store.value;
    }
    for (unsigned index = 0; index < contentsOfVariable.size(); ++index) {
        if (!contentsOfVariable[index].isStored || !contentsOfVariable[index].isOneConstant)
            continue;
        const Variable& variable = program.variables[index];
        const Type& declared = program.at(variable.declared.written());
        Type constant = typeOfKnownPrimitive(contentsOfVariable[index].constant);
        if (!constant.isSubsetOf(declared, program))
            continue;
        // A location with a dead zone holds `absent` until its store, and one that is born undefined holds that.
        uint16_t primitivesBeforeStore = declared.primitives & (variable.initialization == VariableInitialization::Undefined ? Primitive::Absent | Primitive::Undefined : Primitive::Absent);
        TypeID narrowed = editor.types().intern(constant.withPrimitives(primitivesBeforeStore));
        if (narrowed != variable.declared.written())
            editor.at(VariableID(index)).declared.changeType(Proven::byPosition(narrowed));
    }
    return collectListsStoredAlone(program, stores);
}

// DeferDefinitions.

namespace {

struct Deferral {
    enum class Kind : uint8_t { Closure, Class, RegExp };

    Kind kind { Kind::Closure };
    FunctionID body;
    Vector<ValueID, 4> removed;
    // The variables that no read finds empty. They stand for the object from now on.
    Vector<VariableID, 1> variables;
    // The variables that a read may find in their dead zone, and their stores. They go on saying whether the declaration has run, and a new variable beside each stands for the object.
    Vector<VariableID, 1> variablesWithDeadZone;
    Vector<ValueID, 1> storesThatEndDeadZone;
    // What the variables are born as.
    FunctionID function;
    LayoutID layout;
    // NewClass, where something in the body still uses the class object. It becomes a read of the variable, or of the one beside it.
    ValueID classObject;
    VariableID variableReadInBody;
    // NewRegExp.
    ValueID literal;
};

// What a read finds that comes before the only store.
enum class BeforeStore : uint8_t { NothingIsRead, DeadZone, SomethingElse };

struct DefinitionCounts {
    // How many operations of the program name each thing, counted up to two.
    Vector<uint8_t> storesOfVariable;
    Vector<uint8_t> closuresOfFunction;
    Vector<uint8_t> definitionsOfClass;
    // What the run time finds through a record of the program. A pass may later turn an access by name into a read of such a variable.
    BitVector isNamedByRecord;
};

struct DefinitionPlan {
    // In the order of the bodies.
    Vector<Deferral> deferrals;
    // By the variable that goes on saying whether its declaration has run.
    MapByIndex<VariableID> objectBeside;
    MapByIndex<Value> regExps;
    // What a variable holds that only says whether its declaration has run.
    TypeID typeOfDeadZone;
};

} // anonymous namespace

static DefinitionCounts countDefinitions(const Program& program, const ParallelLoops& loops)
{
    using Mention = std::pair<NameKind, uint32_t>;
    Vector<Mention> mentions = collectInParallel<Mention>(loops, program.functions.size(), [&](unsigned index, Vector<Mention>& found) {
        forEachListedName(program, FunctionID(index), { Opcode::PutModuleVariable, Opcode::NewClosure, Opcode::NewClass }, [&](BlockID, ValueID, Opcode opcode, NameKind kind, uint32_t name) {
            if (kind == (opcode == Opcode::PutModuleVariable ? NameKind::Variable : opcode == Opcode::NewClosure ? NameKind::Function : NameKind::Layout))
                found.append({ kind, name });
        });
    });
    DefinitionCounts counts;
    counts.storesOfVariable.fill(0, program.variables.size());
    counts.closuresOfFunction.fill(0, program.functions.size());
    counts.definitionsOfClass.fill(0, program.layouts.size());
    for (auto [kind, name] : mentions) {
        uint8_t& count = (kind == NameKind::Variable ? counts.storesOfVariable : kind == NameKind::Function ? counts.closuresOfFunction : counts.definitionsOfClass)[name];
        count = std::min<unsigned>(count + 1, 2);
    }
    // The closure table has one cell per function, and the closure of a declaration is in it.
    for (const Variable& variable : program.variables) {
        if (variable.initialization == VariableInitialization::Function && variable.function)
            counts.closuresOfFunction[variable.function.index()] = 2;
    }

    counts.isNamedByRecord.ensureSize(program.variables.size());
    auto addAll = [&](const auto& records) {
        for (const auto& record : records) {
            if (record.variable)
                counts.isNamedByRecord.quickSet(record.variable.index());
        }
    };
    for (const Module& module : program.modules) {
        addAll(module.exports);
        addAll(module.syntheticExports);
    }
    for (const Layout& layout : program.layouts)
        addAll(layout.staticFields);
    for (const EvalScope& scope : program.evalScopes)
        addAll(scope.bindings);
    return counts;
}

static void findDeferrals(const Program& program, const FlowResults& flow, BuildContext& build, const DefinitionCounts& counts, FunctionID id, Vector<Deferral>& found)
{
    const Function& function = program.at(id);
    std::optional<FunctionAnalyses> analyses;
    auto useLists = [&]() -> const UseLists& {
        if (!analyses)
            analyses.emplace(AnalysisContext { build, program, &flow, nullptr, id }, function);
        return analyses->useLists();
    };
    auto hasOnlyClosure = [&](FunctionID closed) { return counts.closuresOfFunction[closed.index()] == 1; };

    EffectSummaries noSummaries;
    ObservableOperations observable({ program, noSummaries }, function);
    auto neverThrows = [&](ValueID heritageLook) {
        return observable.neitherThrowsNorRunsCode(heritageLook);
    };

    // SomethingElse where the variable needs the object itself.
    auto whatIsReadBefore = [&](const Value& store, Deferral::Kind kind) {
        const Variable& variable = program.at(store.variable());
        bool isNamedByRecord = counts.isNamedByRecord.quickGet(store.variable().index());
        bool isExpected = variable.declared.isExpected();
        if (counts.storesOfVariable[store.variable().index()] != 1 || variable.initialization == VariableInitialization::Function || variable.owner
            || variable.flow.writers != WriterState::AllKnown || variable.flow.stores != StoreCount::AtMostOnce)
            return BeforeStore::SomethingElse;
        switch (kind) {
        case Deferral::Kind::Closure:
        case Deferral::Kind::Class: {
            // The front end hoists nothing into the variables of a CommonJS module, whose body may run again. A variable that is stored at most once belongs to a body that does not.
            ModuleKind moduleKind = variable.module ? program.at(variable.module).kind : ModuleKind::Data;
            if (moduleKind != ModuleKind::SourceText && moduleKind != ModuleKind::RuntimeSource && moduleKind != ModuleKind::CommonJs)
                return BeforeStore::SomethingElse;
            break;
        }
        case Deferral::Kind::RegExp:
            // An empty variable says that the object has not been made. Whoever reads it without the test for that takes it for a binding in its dead zone.
            if (isNamedByRecord || isExpected)
                return BeforeStore::SomethingElse;
            break;
        }
        if (variable.flow.initialization != InitializationState::Unknown)
            return BeforeStore::NothingIsRead;
        if (variable.initialization != VariableInitialization::Uninitialized || isExpected)
            return BeforeStore::SomethingElse;
        // A read has the type of the variable, so a variable whose type leaves out `absent` is never read while it is empty.
        if (!(program.at(program.provenTypeOf(variable.declared)).primitives & Primitive::Absent))
            return BeforeStore::NothingIsRead;
        // The run time takes what the variable holds for the value of the binding.
        return isNamedByRecord ? BeforeStore::SomethingElse : BeforeStore::DeadZone;
    };
    auto tryTakeStore = [&](Deferral& deferral, ValueID store) {
        switch (whatIsReadBefore(function.at(store), deferral.kind)) {
        case BeforeStore::NothingIsRead:
            deferral.removed.append(store);
            deferral.variables.append(function.at(store).variable());
            return true;
        case BeforeStore::DeadZone:
            deferral.storesThatEndDeadZone.append(store);
            deferral.variablesWithDeadZone.append(function.at(store).variable());
            return true;
        case BeforeStore::SomethingElse:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };

    auto scanVariableStore = [&](ValueID storeID) {
        const Value& store = function.at(storeID);
        const Value& allocation = function.at(sameObjectThroughCasts(function, store.valueOperand()));
        Deferral deferral { .body = id };
        if (allocation.opcode == Opcode::NewClosure && allocation.capturesOperands().empty() && !allocation.isMadeOnDemand() && hasOnlyClosure(allocation.function()))
            deferral.function = allocation.function();
        else if (allocation.opcode == Opcode::NewRegExp)
            deferral.kind = Deferral::Kind::RegExp;
        else
            return;
        if (whatIsReadBefore(store, deferral.kind) == BeforeStore::SomethingElse)
            return;
        // The allocation comes last. Nothing but the store gets hold of the object.
        for (ValueID link = store.valueOperand();; link = function.at(link).operand(0)) {
            if (useLists().useCount(link) != 1)
                return;
            deferral.removed.append(link);
            if (!isCast(function.at(link).opcode))
                break;
            if (!isCastWithoutTest(function.at(link).opcode))
                return;
        }
        deferral.literal = deferral.removed.last();
        tryTakeStore(deferral, storeID);
        found.append(WTF::move(deferral));
    };

    // A read gives what its variable is declared to hold, and every use of the class object was typed with the type of the class object.
    auto findVariableOfType = [&](const Deferral& deferral, TypeID type) {
        for (VariableID variable : deferral.variables) {
            if (program.at(variable).declared.established() == type)
                return variable;
        }
        for (VariableID variable : deferral.variablesWithDeadZone) {
            if (program.at(program.provenTypeOf(program.at(variable).declared)).withoutPrimitives(Primitive::Absent) == program.at(type))
                return variable;
        }
        return VariableID();
    };
    auto scanClassDefinition = [&](ValueID classObject) {
        LayoutID layoutID = function.at(classObject).layout();
        const Layout& layout = program.at(layoutID);
        if (counts.definitionsOfClass[layoutID.index()] != 1 || !layout.constructor || !hasOnlyClosure(layout.constructor))
            return;
        std::optional<ClassDefinitionSite> site = findClassDefinitionSite(program, function, useLists(), classObject);
        if (!site || !std::ranges::all_of(layout.classFlow.members, [&](const ClassMember& member) { return hasOnlyClosure(member.function); }))
            return;

        Deferral deferral { .kind = Deferral::Kind::Class, .body = id, .function = layout.constructor, .layout = layoutID };
        deferral.removed.appendVector(site->describedByRecord);
        if (site->heritageLook && neverThrows(site->heritageLook))
            deferral.removed.append(site->heritageLook);
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
            if ((function.at(copy).opcode == Opcode::FinishClass || isCastWithoutTest(function.at(copy).opcode)) && isLeftWithoutUse(copy))
                unused.append(copy);
        }
        if (isLeftWithoutUse(classObject))
            unused.append(classObject);
        else {
            deferral.variableReadInBody = findVariableOfType(deferral, function.at(classObject).type);
            if (!deferral.variableReadInBody)
                return;
            deferral.classObject = classObject;
        }
        deferral.removed.appendVector(unused);
        found.append(WTF::move(deferral));
    };

    for (const Block& block : function.blocks) {
        for (ValueID value : block.values) {
            if (function.at(value).opcode == Opcode::PutModuleVariable)
                scanVariableStore(value);
            else if (function.at(value).opcode == Opcode::NewClass)
                scanClassDefinition(value);
        }
    }
}

// The front end follows each read of a binding with a dead zone by the test for it, and flow puts a cast in front of a test that is sure to pass. Whatever else takes such a read may get `absent` or the object.
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
        const Function& function = program.functions[index];
        if (!listsAccessOf(program, FunctionID(index), Opcode::GetModuleVariable, [&](VariableID variable) { return hasDeadZone.quickGet(variable.index()); }))
            return;
        auto noteUse = [&](ValueID operand) {
            if (function.at(operand).opcode == Opcode::GetModuleVariable && hasDeadZone.quickGet(function.at(operand).variable().index()))
                found.append(function.at(operand).variable());
        };
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values) {
                const Value& value = function.at(id);
                bool validatesCast = value.opcode == Opcode::ValidateType && !(program.at(value.validatedType()).primitives & Primitive::Absent);
                if (!leavesDeadZoneBehind(program, function, value) && !validatesCast)
                    value.forEachChild(noteUse);
            }
            block.forEachChildOutsideValues(noteUse);
        }
    });
    if (readOtherwise.isEmpty())
        return;
    for (VariableID variable : readOtherwise)
        hasDeadZone.quickClear(variable.index());
    deferrals.removeAllMatching([&](const Deferral& deferral) {
        return !std::ranges::all_of(deferral.variablesWithDeadZone, [&](VariableID variable) { return hasDeadZone.quickGet(variable.index()); });
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
            const LayoutFlow& flow = program.at(deferral.layout).flow;
            if (flow.baseState != BaseState::Layout || isDeferred.quickGet(flow.base.index()))
                continue;
            isDeferred.quickClear(deferral.layout.index());
            hasChanged = true;
        }
    }
    deferrals.removeAllMatching([&](const Deferral& deferral) { return deferral.kind == Deferral::Kind::Class && !isDeferred.quickGet(deferral.layout.index()); });
}

static void changeVariables(ProgramEditor& editor, DefinitionPlan& plan)
{
    const Program& program = editor.program();
    plan.typeOfDeadZone = editor.types().intern(Type::fromPrimitives(Primitive::Absent | Primitive::Undefined));
    for (Deferral& deferral : plan.deferrals) {
        for (VariableID deadZone : deferral.variablesWithDeadZone) {
            // Every read of this one stands behind the test of the other, which passes only after the declaration has run.
            Variable beside = program.at(deadZone);
            beside.declared.changeType(Proven::byPosition(editor.types().intern(program.at(beside.declared.written()).withoutPrimitives(Primitive::Absent))));
            beside.flow.initialization = InitializationState::StoredBeforeEveryRead;
            VariableID object = editor.addVariable(WTF::move(beside));
            editor.at(deadZone).declared = DeclaredType(Proven::byPosition(plan.typeOfDeadZone));
            plan.objectBeside.add(deadZone.index(), object);
            deferral.variables.append(object);
            if (deferral.variableReadInBody == deadZone)
                deferral.variableReadInBody = object;
        }
        for (VariableID id : deferral.variables) {
            Variable& variable = editor.at(id);
            switch (deferral.kind) {
            case Deferral::Kind::Closure:
            case Deferral::Kind::Class:
                variable.initialization = VariableInitialization::Function;
                variable.function = deferral.function;
                variable.flow.stores = StoreCount::Never;
                if (id == deferral.variableReadInBody)
                    variable.flow.reads = ReadState::Unknown;
                break;
            case Deferral::Kind::RegExp:
                variable.declared.changeType(Proven::byPosition(editor.types().intern(program.at(variable.declared.written()).withPrimitives(Primitive::Absent))));
                variable.initialization = VariableInitialization::Uninitialized;
                variable.flow.initialization = InitializationState::Unknown;
                variable.flow.isUnchangedDuringActivation = false;
                plan.regExps.add(id.index(), program.at(deferral.body).at(deferral.literal));
                break;
            }
        }
    }
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
    removeValues(builder, isRemoved, { }, [&](ValueID id) { return endsDeadZone.quickGet(id.index()) ? std::optional(Opcode::Undefined) : std::nullopt; });
}

// The test stays where it is, on the variable that says whether the declaration has run. What took the result of the test takes the object. So does what took the cast that stood for a test.
static void readObjectsBehindDeadZoneTests(const DefinitionPlan& plan, IRBuilder& builder)
{
    if (plan.objectBeside.isEmpty())
        return;
    Function& function = builder.function();
    auto objectBesideReadBy = [&](ValueID id) {
        return function.at(id).opcode == Opcode::GetModuleVariable ? plan.objectBeside.get(function.at(id).variable().index()) : VariableID();
    };
    auto objectReadThrough = [&](ValueID id) {
        return function.at(id).opcode == Opcode::CheckInitialized || function.at(id).opcode == Opcode::AssumeType ? objectBesideReadBy(function.at(id).valueOperand()) : VariableID();
    };
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            Value& value = function.edit(id);
            if (value.opcode == Opcode::ValidateType && objectBesideReadBy(value.operand(0)))
                value.setValidatedType(TypeTable::undefined);
            if (!objectBesideReadBy(id))
                continue;
            value.type = plan.typeOfDeadZone;
            value.setExpected({ });
        }
    }
    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        if (std::ranges::none_of(function.blocks[index].values, [&](ValueID id) { return !!objectReadThrough(id); }))
            continue;
        BlockValues values = std::exchange(function.edit(BlockID(index), BlockEdit::Values).values, { });
        BlockValues rewritten;
        rewritten.reserveInitialCapacity(values.size() + 1);
        for (ValueID id : values) {
            if (VariableID object = objectReadThrough(id)) {
                if (function.at(id).opcode == Opcode::CheckInitialized) {
                    Value test = function.at(id);
                    test.setExpected({ });
                    builder.setOrigin(test.origin());
                    rewritten.append(builder.addDetached(WTF::move(test)));
                }
                Value& read = function.edit(id);
                read.become(Opcode::GetModuleVariable);
                read.setVariable(object);
                ASSERT(plan.regExps.contains(object.index()) || read.type == builder.context().program.at(object).declared.written());
            }
            rewritten.append(id);
        }
        function.edit(BlockID(index), BlockEdit::Values).values = WTF::move(rewritten);
    }
}

static void fillRegExpVariablesAtReads(const DefinitionPlan& plan, IRBuilder& builder)
{
    if (plan.regExps.isEmpty())
        return;
    auto holdsRegExp = [&](ValueID, VariableID variable) { return plan.regExps.contains(variable.index()); };
    forEachReadBackwards(builder.function(), holdsRegExp, [&](ProgramPoint point, VariableID variable) {
        IRBuilder::Expansion expansion = builder.beginExpansion(point);
        ValueID held = builder.append(makeVariableAccess(Opcode::GetModuleVariable, variable));
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.appendTypeTest(held, TypeTable::absent), 0, BlockFrequency::Rare, BlockFrequency::Normal);

        // The casts between the literal and its store went with the definition. What flow proved of the variable's object it proved of this one.
        const DeclaredType& declared = builder.context().program.at(variable).declared;
        ValueID made = builder.append(Value(plan.regExps.find(variable.index())->value));
        if (!declared.isExpected())
            made = assumeType(builder, made, *declared.established());
        builder.append(makeVariableAccess(Opcode::PutModuleVariable, variable, { made }));
        builder.endExpansion(expansion, made);

        builder.moveToEndOf(branch.notTaken);
        builder.endExpansion(expansion, branch.provenOnNotTaken);
    });
}

void deferDefinitions(ProgramEditor& editor, const FlowResults& flow, BuildContext& build)
{
    const Program& program = editor.program();
    const ParallelLoops& loops = build.parallelLoops();
    DefinitionCounts counts = countDefinitions(program, loops);
    DefinitionPlan plan;
    plan.deferrals = collectInParallel<Deferral>(loops, program.functions.size(), [&](unsigned index, Vector<Deferral>& found) {
        if (mayListValueWith(program, FunctionID(index), { Opcode::PutModuleVariable, Opcode::NewClass }) && program.functions[index].hasBody() && build.isEnabled(PassID::DeferDefinitions, FunctionID(index)))
            findDeferrals(program, flow, build, counts, FunctionID(index), found);
    });
    dropDeferralsWithOtherReads(program, loops, plan.deferrals);
    dropClassesBelowClassesThatCodeDefines(program, plan.deferrals);
    if (plan.deferrals.isEmpty())
        return;

    changeVariables(editor, plan);
    auto isReadOtherwise = [&](VariableID variable) { return plan.objectBeside.contains(variable.index()) || plan.regExps.contains(variable.index()); };
    Vector<FunctionID> functions = findFunctionsToEdit(program, loops, plan.deferrals, !plan.objectBeside.isEmpty() || !plan.regExps.isEmpty(), isReadOtherwise);
    editFunctionsInParallel(editor, build, functions.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        IRBuilder builder(AnalysisContext { build, program, &flow, nullptr, id }, function, arenas.types);
        removeDefinitions(thoseIn(plan.deferrals, id), builder);
        readObjectsBehindDeadZoneTests(plan, builder);
        fillRegExpVariablesAtReads(plan, builder);
        settleProofsBehindOperandsThatWereOnlyKept(function);
    });

    build.statistics().count(StatisticID::ClosuresDeferredInVariables, std::ranges::count(plan.deferrals, Deferral::Kind::Closure, &Deferral::kind));
    build.statistics().count(StatisticID::ClassesDeferredInVariables, std::ranges::count(plan.deferrals, Deferral::Kind::Class, &Deferral::kind));
    build.statistics().count(StatisticID::RegExpsDeferredInVariables, std::ranges::count(plan.deferrals, Deferral::Kind::RegExp, &Deferral::kind));
    build.statistics().count(StatisticID::DeadZonesKeptBesideDefinitions, plan.objectBeside.size());
}

// DeferInitializers.

namespace {

constexpr AbstractHeapSet heapsNobodyObserves = bookkeepingHeaps | AbstractHeapSet { AbstractHeap::StringStorage, AbstractHeap::ValidationState };
// An operand that the initializer has not made is looked at, kept and handed on. Nothing is read out of it, since it may have changed by the time of the first read.
constexpr OperandUses usesOfObjectsMadeElsewhere { OperandUse::Inspects, OperandUse::Retains, OperandUse::Returns };
constexpr OptionSet<ObjectMutation> mutationsOfContents {
    ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript, ObjectMutation::DeleteField, ObjectMutation::RedefineProperty, ObjectMutation::AddProperty,
    ObjectMutation::ChangePrototype, ObjectMutation::StoreAfterCreation,
};

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
    BitVector holdsUnchangingObject;
    // The body of its module stores it once, on every way to its end, and nothing else stores it: once the body has run, it holds its value for good.
    BitVector isSetWhenItsModuleHasRun;
    // Nothing but compiled reads finds it.
    Vector<Emptiness> emptiness;
};

struct InitializerScanContext {
    AnalysisContext analysisContextOf(FunctionID id) const { return { runner.build(), program, &inputs.flow, &inputs.effects, id }; }

    // Whether the body of the one module has run to its end when the body of the other begins: the other imports it, directly or not, and no circle of imports holds both.
    bool hasRunBefore(ModuleID finished, ModuleID beginning, BitVector& importedByBeginning) const
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

    const Program& program;
    const OptimizationInputs& inputs;
    const PassRunner& runner;
    const VariableFacts& variables;
    const StronglyConnectedComponents& circlesOfImports;
    // By function: the module that has it as its body. One file may hold the bodies of several modules, and all that is written in a file names the record of the file.
    const Vector<ModuleID>& moduleWithBody;
};

// A stretch of a body that runs once: one operation of a block that every run passes, all that lies between two such blocks, or the return of the last one with all that it enters by throwing.
struct Item {
    bool isRegion() const { return !value; }
    bool endsBody() const { return isRegion() && !join; }
    Origin originIn(const Function& body) const { return isRegion() ? body.at(block).terminator.origin() : body.at(value).origin(); }

    BlockID block;
    ValueID value;
    BlockID join;
    Vector<BlockID, 0> blocks;
    // The members of built-in objects that the functions it calls read for the first time, in that order. Where the item is put off, they are read where it stood.
    MembersFirstRead membersFirstRead;
};

struct ValueOfVariable {
    ValueID value;
    VariableID variable;
};

// What is put off together: the operations that make, fill and store what some variables hold. The first read of any of the variables runs all of it.
struct InitializerUnit {
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
    TypeID type;
};

struct InitializerCalleeAnswer {
    bool mayMove { false };
    bool returnsWhatItMade { false };
    // The answer holds where the bodies of the modules of these variables have run.
    Vector<VariableID, 0> variablesTakenToBeSet;
    MembersFirstRead membersFirstRead;
};

struct Knowledge {
    // An object that was made since the initializer began. Nothing outside has it yet.
    BitVector made;
    // An object that never changes once it is made: what is read out of it is the same whenever it is read.
    BitVector unchanging;
};

struct CallContext {
    uint64_t made { 0 };
    uint64_t unchanging { 0 };
    Vector<TypeID, 8> types;

    friend bool operator==(const CallContext&, const CallContext&) = default;
};

} // anonymous namespace

static bool isClassConstructorFunction(const Function& function)
{
    return function.kind == FunctionKind::BaseConstructor || function.kind == FunctionKind::DerivedConstructor;
}

static bool namesItsFirstOperand(Opcode opcode)
{
    return isCast(opcode) || opcode == Opcode::FinishClass || opcode == Opcode::NewClass;
}

static bool allocatesObjectThatNeverChanges(const FlowResults& flow, const Value& value)
{
    return interpretedOf(value.opcode) == Interpretation::Allocation && Value::opcodesWithAllocationSite.contains(value.opcode) && value.allocationSite()
        && (flow.objectsNeverChange(value.allocationSite()) || flow.classOf(value.allocationSite()).impossibleMutations.containsAll(mutationsOfContents));
}

// They lie on the way up the dominator tree from the one block that returns, and come in that order. None where no block returns or several do.
static Vector<BlockID, 16> findBlocksThatEveryRunPasses(FunctionAnalyses& analyses)
{
    Vector<BlockID, 16> blocks;
    BlockID last;
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        if (!returnsToCaller(analyses.function().at(block).terminator.kind))
            continue;
        if (last)
            return blocks;
        last = block;
    }
    for (BlockID block = last; block; block = block == entryBlock ? BlockID() : analyses.dominators().idom(block))
        blocks.append(block);
    return blocks;
}

static VariableFacts collectVariableFacts(const Program& program, const FlowResults& flow, const Vector<ModuleID>& moduleWithBody, BuildContext& build)
{
    struct Store {
        VariableID variable;
        bool isOnEveryWayThroughItsModule;
        bool mayStoreUndefined;
        bool storesUnchangingObject;
    };
    Vector<Store> stores = collectInParallel<Store>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<Store>& found) {
        if (!mayListValueWith(program, FunctionID(index), { Opcode::PutModuleVariable }))
            return;
        const Function& function = program.functions[index];
        ModuleID evaluated = moduleWithBody[index];
        BitVector isPassedByEveryRun;
        // The body of either kind runs at most once.
        if (evaluated && (program.at(evaluated).kind == ModuleKind::SourceText || program.at(evaluated).kind == ModuleKind::CommonJs) && function.hasBody()) {
            FunctionAnalyses analyses(AnalysisContext { build, program, &flow, nullptr, FunctionID(index) }, function);
            for (BlockID block : findBlocksThatEveryRunPasses(analyses))
                isPassedByEveryRun.set(block.index());
        }
        forEachListedValue(program, FunctionID(index), { Opcode::PutModuleVariable }, [&](BlockID block, uint32_t, ValueID, const Value& store) {
            found.append({ store.variable(), isPassedByEveryRun.get(block.index()) && program.at(store.variable()).module == evaluated,
                !!(program.typeOf(function, store.valueOperand()).primitives & Primitive::Undefined),
                allocatesObjectThatNeverChanges(flow, function.at(sameObjectThroughCasts(function, store.valueOperand()))) });
        });
    });
    size_t numberOfVariables = program.variables.size();
    Vector<uint8_t> numberOfStores(FillWith { }, numberOfVariables, uint8_t { 0 });
    Vector<const Store*> storeTo(FillWith { }, numberOfVariables, nullptr);
    for (const Store& store : stores) {
        numberOfStores[store.variable.index()] = std::min<unsigned>(numberOfStores[store.variable.index()] + 1, 2);
        storeTo[store.variable.index()] = &store;
    }
    // The run time finds these by a record and takes an empty one to be in its dead zone.
    BitVector isNamedByRecord = findVariablesDescribedToRunTime(program, flow);

    VariableFacts facts;
    for (BitVector* fact : { &facts.isStoredOnce, &facts.holdsOneValue, &facts.holdsUnchangingObject, &facts.isSetWhenItsModuleHasRun })
        fact->ensureSize(numberOfVariables);
    facts.emptiness.fill(Emptiness::None, numberOfVariables);
    for (size_t index = 0; index < numberOfVariables; ++index) {
        const Variable& variable = program.variables[index];
        if (variable.flow.writers != WriterState::AllKnown || variable.owner)
            continue;
        const Type& type = program.at(variable.declared.written());
        bool isExpected = variable.declared.isExpected();
        bool isNeverReadEmpty = variable.flow.initialization != InitializationState::Unknown || (!isExpected && variable.initialization == VariableInitialization::Uninitialized && !(type.primitives & Primitive::Absent));
        if (variable.initialization == VariableInitialization::Function) {
            if ((isNeverReadEmpty || !(type.primitives & Primitive::Absent)) && variable.flow.stores == StoreCount::Never && !numberOfStores[index])
                facts.holdsOneValue.quickSet(index);
            continue;
        }
        if (variable.flow.stores != StoreCount::AtMostOnce || numberOfStores[index] != 1)
            continue;
        bool isStoredBeforeEveryRead = isNeverReadEmpty && variable.flow.initialization != InitializationState::SetAtCreation;
        facts.isStoredOnce.quickSet(index);
        facts.isSetWhenItsModuleHasRun.quickSet(index, storeTo[index]->isOnEveryWayThroughItsModule);
        facts.holdsOneValue.quickSet(index, isStoredBeforeEveryRead);
        facts.holdsUnchangingObject.quickSet(index, isStoredBeforeEveryRead && storeTo[index]->storesUnchangingObject);
        if (isNamedByRecord.quickGet(index) || !variable.module || !type.isJavaScriptValue())
            continue;
        if (isStoredBeforeEveryRead)
            facts.emptiness[index] = Emptiness::AbsentFromBirth;
        else if (variable.initialization == VariableInitialization::Undefined)
            facts.emptiness[index] = Emptiness::AbsentFromDeclaration;
        else if (!storeTo[index]->mayStoreUndefined)
            facts.emptiness[index] = Emptiness::UndefinedFromDeclaration;
    }
    return facts;
}

static bool haveSameOperations(const Function& a, const Function& b)
{
    auto isSameOperation = [](const Value& a, const Value& b) {
        return a.opcode == b.opcode && a.type == b.type && a.expected().predicted() == b.expected().predicted() && a.hasSamePayloadAs(b) && std::ranges::equal(a.operands(), b.operands());
    };
    return std::ranges::equal(a.values, b.values, isSameOperation) && std::ranges::equal(a.blocks, b.blocks, [](const Block& a, const Block& b) {
        return a.terminator.kind == b.terminator.kind && std::ranges::equal(a.values, b.values);
    });
}

namespace {

// Says of an operation whether anybody can tell when it ran. An operation may do as it likes with an object that the initializer has made.
struct MovableCode {
    AOT_CLASS_THRESHOLD(unsigned, maxDepth, 12);
    AOT_CLASS_THRESHOLD(unsigned, maxValuesOfCallee, 4000);
    AOT_CLASS_THRESHOLD(unsigned, maxAnswers, 4000);
    static constexpr size_t maxValuesInKeptVersions = 1 << 16;

    const InitializerScanContext& context;
    TaskArenas& arenas;
    const Program& program { context.program };
    MapByIndex<Vector<std::pair<CallContext, InitializerCalleeAnswer>, 0>> answers { };
    Vector<std::pair<uint32_t, CallContext>, 8> beingAnswered { };
    unsigned numberOfAnswers { 0 };
    // What a call has made and what never changes decide the answer. The version comes from the types alone.
    MapByIndex<Vector<std::pair<Vector<TypeID, 8>, std::unique_ptr<const Function>>, 1>> versions { };
    size_t numberOfValuesInKeptVersions { 0 };
    Vector<VariableID, 4> variablesTakenToBeSet { };
    MembersFirstRead membersFirstRead { };
    // Of what is being looked at: whether every run of the item comes by it.
    bool isPassedByEveryRun { true };

    const Type& typeOf(const Function& function, ValueID id) const { return arenas.types.at(function.at(id).type); }
    bool isText(StringID string, ASCIILiteral text) const { return equalSpans(program.string(string), text.span8()); }
    bool isText(PropertyKeyID key, ASCIILiteral text) const { return program.at(key).text && isText(program.at(key).text, text); }
    bool isMadeOrNoReference(const Function& function, const Knowledge& known, ValueID id) const { return known.made.get(id.index()) || !mayBeReference(typeOf(function, id)); }
    // To make a key of it runs no code.
    bool isPrimitiveKey(const Function& function, ValueID id) const { return !typeOf(function, id).hasObjects() && typeOf(function, id).isJavaScriptValue(); }
    bool isCoroutine(FunctionID id) const { return program.at(id).hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator }); }
    bool isObjectDefineProperty(const BuiltinCall& call) const { return program.builtins().at(call.overload).builtin == Builtins::ObjectDefineProperty; }
    bool isOtherNameOfFirstOperand(const Function& function, const Value& value) const { return namesItsFirstOperand(value.opcode) || (value.opcode == Opcode::ToObject && typeOf(function, value.operand(0)).isObject()); }
    static bool hasOnlyFunctions(const Type& type) { return type.isObject() && !type.anyObject && !type.anyFunction && !type.layouts && type.builtins.isEmpty() && type.classes.isEmpty() && !type.functions.isEmpty(); }
    static bool hasOnlyPristineArrays(const Type& type) { return hasOnlyArrays(type, { PristineAspects::Storage, PristineAspects::Names }); }

    static bool hasOnlyArrays(const Type& type, OptionSet<PristineAspects> proven)
    {
        if (!type.isObject() || type.anyObject || type.anyFunction || type.layouts || !type.functions.isEmpty() || !type.builtins.isEmpty() || type.classes.isEmpty())
            return false;
        return std::ranges::all_of(type.classes, [&](const ClassPart& part) { return part.builtinClass == BuiltinClasses::Array && part.provenPristine.containsAll(proven); });
    }

    static ValueID allocationOf(const Function& function, ValueID id)
    {
        while (namesItsFirstOperand(function.at(id).opcode))
            id = function.at(id).operand(0);
        return id;
    }

    // The function allocates the object without a prototype of its own, and does nothing with it but store to it, read from it and keep it.
    bool isOnlyFilledHere(FunctionAnalyses& analyses, ValueID object) const
    {
        auto fillsReadsOrKeeps = [](Opcode opcode) {
            switch (opcode) {
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
                return true;
            default:
                return false;
            }
        };
        const Function& function = analyses.function();
        const UseLists& useLists = analyses.useLists();
        Vector<ValueID, 4> names { allocationOf(function, object) };
        const Value& allocated = function.at(names[0]);
        if (allocated.opcode != Opcode::NewDictionary && (allocated.opcode != Opcode::NewObject || allocated.prototypeOperand()))
            return false;
        for (size_t index = 0; index < names.size(); ++index) {
            for (const Use& use : useLists.usesOf(names[index])) {
                bool isReturned = use.kind == Use::Kind::TerminatorOperand && function.at(use.block).terminator.kind == TerminatorKind::Return;
                if (isReturned || (use.kind == Use::Kind::Operand && fillsReadsOrKeeps(function.at(use.user).opcode)))
                    continue;
                if (use.kind != Use::Kind::Operand || !isCast(function.at(use.user).opcode))
                    return false;
                names.append(use.user);
            }
        }
        return true;
    }

    // One answer serves every text and every number that a helper is handed.
    TypeID withConstantsWidened(TypeID id) const
    {
        const Type& type = arenas.types.at(id);
        bool hasText = type.primitives & Primitive::LiteralString;
        bool hasRange = (type.primitives & Primitive::Integer) && type.integers != Range::int32() && type.integers != Range::full();
        if (!hasText && !hasRange)
            return id;
        Type widened = type.withLiteralStringsWidened();
        if (hasRange)
            widened.integers = type.integers.isSubsetOf(Range::int32()) ? Range::int32() : Range::full();
        return arenas.types.intern(widened);
    }

    // `new` lays the object below what the function that it is made for holds under `prototype` at that moment. A class holds one object there for good.
    bool findsSamePrototypeWheneverDone(const Function& caller, const Knowledge& known, const Value& construct, std::span<const FunctionID> callees) const
    {
        ValueID newTarget = construct.newTargetOperand() ? construct.newTargetOperand() : construct.calleeOperand();
        if (known.made.get(newTarget.index()) || known.unchanging.get(newTarget.index()))
            return true;
        if (construct.newTargetOperand()) {
            if (!hasOnlyFunctions(typeOf(caller, newTarget)))
                return false;
            callees = typeOf(caller, newTarget).functions.span();
        }
        const PropertyKeyID prototype = program.keyOf(BuiltinNames::prototype);
        return std::ranges::all_of(callees, [&](FunctionID id) {
            return isClassConstructorFunction(program.at(id)) || context.inputs.flow.areClosuresAsCreated(id, singleElementSpan(prototype));
        });
    }

    InitializerCalleeAnswer answerForCall(const Function& caller, const Knowledge& known, const Value& call)
    {
        if (call.opcode != Opcode::Call && call.opcode != Opcode::Construct && call.opcode != Opcode::CallDirect)
            return { };
        CalleeList callees = calleesOfCall(program, call);
        // What the call of this run hands on says more than what all calls hand on.
        if (!callees.listsEveryCallee() && call.opcode != Opcode::CallDirect && hasOnlyFunctions(typeOf(caller, call.calleeOperand()))) {
            callees = CalleeList::none();
            for (FunctionID function : typeOf(caller, call.calleeOperand()).functions)
                callees.add(function);
        }
        std::optional<ProvenValue<std::span<const FunctionID>>> functions = callees.functionsAlone();
        if (!functions || functions->value().empty())
            return { };
        if (call.opcode == Opcode::Construct && !findsSamePrototypeWheneverDone(caller, known, call, functions->value()))
            return { };
        InitializerCalleeAnswer result { true, true };
        for (FunctionID id : functions->value()) {
            const Function& callee = program.at(id);
            unsigned numberOfParameters = callee.numberOfEntryParameters();
            // A rest parameter and `arguments` are made at the entrance, which this does not follow.
            if (isCoroutine(id) || callee.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest })
                || (call.opcode == Opcode::Call && isClassConstructorFunction(callee)) || numberOfParameters > 64)
                return { };
            // A parameter that gets no argument holds undefined.
            CallContext handed;
            handed.types.fill(TypeID(), numberOfParameters);
            for (unsigned index = 0; index < numberOfParameters; ++index) {
                if (callee.entryParameterKind(index) != EntryParameter::Parameter)
                    continue;
                handed.made |= uint64_t { 1 } << index;
                handed.types[index] = TypeTable::undefined;
            }
            call.forEachOperand([&](ValueID operand, uint32_t index) {
                std::optional<uint32_t> parameter = Value::opcodesWithCallees.contains(call.opcode) ? entryParameterOf(callee, call, index) : call.variadicOperandIndex(index);
                if (!parameter || *parameter >= numberOfParameters)
                    return;
                uint64_t bit = uint64_t { 1 } << *parameter;
                handed.made = isMadeOrNoReference(caller, known, operand) ? handed.made | bit : handed.made & ~bit;
                if (known.unchanging.get(operand.index()))
                    handed.unchanging |= bit;
                handed.types[*parameter] = withConstantsWidened(caller.at(operand).type);
            });
            InitializerCalleeAnswer answer = answerFor(id, handed);
            // One callee that may not move settles it: a call of one of a thousand functions asks about one.
            if (!answer.mayMove)
                return { };
            // What is read where the item stood is what the one callee reads in every run.
            if (!answer.membersFirstRead.isEmpty() && (!isPassedByEveryRun || functions->value().size() > 1))
                return { };
            result.returnsWhatItMade &= answer.returnsWhatItMade;
        }
        return result;
    }

    bool terminatorMayMove(FunctionAnalyses& analyses, BlockID block)
    {
        Effects effects = analyses.effectsOfTerminator(block).onAnyEdge();
        return !effects.mayRunOtherCode() && !effects.mayThrowByRule() && !effects.mayBeTerminated && effects.writes.containsOnly(heapsNobodyObserves | heapsWhoseWritesNobodySees);
    }

    bool isMadeGiven(FunctionAnalyses& analyses, const Knowledge& known, const Value& value)
    {
        const Function& function = analyses.function();
        if (interpretedOf(value.opcode) == Interpretation::Allocation || value.opcode == Opcode::NewDictionary || value.opcode == Opcode::NewArrayLike || value.opcode == Opcode::NewContiguousArray)
            return true;
        if (isOtherNameOfFirstOperand(function, value))
            return known.made.get(value.operand(0).index());
        if (value.opcode == Opcode::ArraySpeciesCreate)
            return hasOnlyPristineArrays(typeOf(function, value.operand(1)));
        if (value.opcode == Opcode::CallBuiltin) {
            BuiltinCall call = builtinCallOf(program, value);
            if (call.form == BuiltinCallForm::Whole && !call.arguments.empty() && isObjectDefineProperty(call))
                return known.made.get(call.arguments[0].index());
        }
        if (builtinCallOpcodes.contains(value.opcode))
            return freshnessOf(analyses.effectContext(), function, value) != ResultFreshness::Unknown;
        InitializerCalleeAnswer answer = answerForCall(function, known, value);
        return answer.mayMove && answer.returnsWhatItMade;
    }

    Knowledge findKnowledge(FunctionAnalyses& analyses, const CallContext& handed, const Vector<VariableID>* holdersOfWhatCountsAsMadeElsewhere = nullptr)
    {
        const Function& function = analyses.function();
        Knowledge known { BitVector(function.values.size()), BitVector(function.values.size()) };
        for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
            const Block& block = function.blocks[blockIndex];
            for (uint32_t index = 0; index < block.parameters.size(); ++index) {
                if (blockIndex || (index < 64 && handed.made >> index & 1))
                    known.made.set(block.parameters[index].index());
                if (blockIndex || (index < 64 && handed.unchanging >> index & 1))
                    known.unchanging.set(block.parameters[index].index());
            }
        }
        auto isUnchanging = [&](const Value& value) {
            const Type& type = arenas.types.at(value.type);
            if (type.hasOnlyLayouts() && std::ranges::all_of(membersOf(program, type), [&](LayoutID id) { return program.at(id).flow.impossibleMutations.containsAll(mutationsOfContents); }))
                return true;
            if (isOtherNameOfFirstOperand(function, value))
                return known.unchanging.get(value.operand(0).index());
            if (value.opcode == Opcode::GetModuleVariable)
                return context.variables.holdsUnchangingObject.get(value.variable().index());
            return allocatesObjectThatNeverChanges(context.inputs.flow, value);
        };

        // What a block is handed is made if every edge hands it such an object. Each round takes some away.
        for (bool hasChanged = true; hasChanged;) {
            hasChanged = false;
            auto update = [&](BitVector& bits, ValueID id, bool value) {
                hasChanged |= bits.get(id.index()) != value;
                bits.set(id.index(), value);
            };
            for (BlockID id : analyses.cfg().reversePostOrder()) {
                const Block& block = function.at(id);
                for (ValueID valueID : block.values) {
                    bool countsAsMadeElsewhere = holdersOfWhatCountsAsMadeElsewhere && (*holdersOfWhatCountsAsMadeElsewhere)[valueID.index()];
                    update(known.made, valueID, !countsAsMadeElsewhere && isMadeGiven(analyses, known, function.at(valueID)));
                    update(known.unchanging, valueID, isUnchanging(function.at(valueID)));
                }
                auto pass = [&](const Target& target, std::optional<uint32_t> targetIndex) {
                    const Block& successor = function.at(target.block);
                    for (uint32_t index = 0; index < successor.parameters.size(); ++index) {
                        ValueID passed = index < target.arguments.size() ? target.arguments[index] : ValueID();
                        // What a test has narrowed is the value that was tested.
                        if (!passed && targetIndex && index - target.arguments.size() < target.proofs.size())
                            passed = operandNarrowedOnEdge(function, { id, *targetIndex, static_cast<uint32_t>(index - target.arguments.size()) });
                        bool isReference = passed && mayBeReference(typeOf(function, passed));
                        if (!passed || (isReference && !known.made.get(passed.index())))
                            update(known.made, successor.parameters[index], false);
                        if (!passed || (isReference && !known.unchanging.get(passed.index())))
                            update(known.unchanging, successor.parameters[index], false);
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
    void narrowElementsThatAreThere(FunctionID id, Function& function)
    {
        Vector<std::tuple<BlockID, uint32_t, TypeID>, 2> narrowings;
        Vector<uint32_t> numberOfPredecessors(FillWith { }, function.blocks.size(), 0u);
        for (const Block& block : function.blocks)
            block.forEachTarget([&](const Target& target) { ++numberOfPredecessors[target.block.index()]; });
        for (const Block& block : function.blocks) {
            if (block.terminator.kind != TerminatorKind::Branch)
                continue;
            const Value& test = function.at(definitionThroughUpcasts(function, block.terminator.conditionOperand()));
            if (test.opcode != Opcode::ElemHas)
                continue;
            const Type& arrayType = typeOf(function, test.objectOperand());
            const ClassPart* part = hasOnlyPristineArrays(arrayType) ? arrayType.singleBuiltinClass() : nullptr;
            BlockID taken = block.terminator.targets[0].block;
            if (!part || !part->contentsAreProven || part->writtenContents.isEmpty() || numberOfPredecessors[taken.index()] != 1)
                continue;
            // Nothing in front of the read has run that could take the element away.
            const BlockValues& values = function.at(taken).values;
            auto found = std::ranges::find_if(values, [&](ValueID value) { return !isCast(function.at(value).opcode) && !isRematerializable(function.at(value)); });
            if (found == values.end() || function.at(*found).opcode != Opcode::ElemGet)
                continue;
            const Value& read = function.at(*found);
            if (sameObjectThroughCasts(function, read.objectOperand()) == sameObjectThroughCasts(function, test.objectOperand())
                && definitionThroughUpcasts(function, read.propertyOperand()) == definitionThroughUpcasts(function, test.propertyOperand()))
                narrowings.append({ taken, static_cast<uint32_t>(found - values.begin()), *part->provenContentsAt(0) });
        }
        if (narrowings.isEmpty())
            return;

        IRBuilder builder(context.analysisContextOf(id), function, arenas.types);
        for (auto [blockOfRead, index, type] : narrowings) {
            ValueID read = function.at(blockOfRead).values[index];
            builder.setOrigin(function.at(read).origin());
            ValueID cast = builder.addDetached(makeAssumeType(read, Proven::byRulesWithoutChecks(arenas.types.intersectionOf(function.at(read).type, type))));
            auto replace = [&](ValueID& operand) {
                if (operand == read)
                    operand = cast;
            };
            for (Block& block : function.editAllBlocks()) {
                for (ValueID user : block.values)
                    function.edit(user).forEachChild(replace);
                block.forEachChildOutsideValues(replace);
            }
            function.edit(blockOfRead, BlockEdit::Values).values.insert(index + 1, cast);
        }
    }

    // The body as it is for what this call hands over: tests of the parameters are decided, and what they guard is gone.
    std::optional<Function> specialize(FunctionID id, const CallContext& handed)
    {
        const Function& original = program.at(id);
        std::optional<Function> version;
        for (uint32_t index = 0; index < handed.types.size(); ++index) {
            if (!handed.types[index])
                continue;
            const Value& parameter = original.at(original.at(entryBlock).parameters[index]);
            TypeID narrowed = arenas.types.intersectionOf(parameter.type, handed.types[index]);
            if (narrowed == parameter.type && !parameter.expected().predicted())
                continue;
            if (!version)
                version = original;
            Value& narrowedParameter = version->edit(version->at(entryBlock).parameters[index]);
            narrowedParameter.type = narrowed;
            narrowedParameter.setExpected({ });
            if (DeclaredType* declared = version->declaredEntryParameter(index))
                *declared = DeclaredType(Proven::byPosition(narrowed));
        }
        for (unsigned round = 0; version && round < 2; ++round) {
            {
                FunctionAnalyses analyses(context.analysisContextOf(id), *version);
                FunctionPassContext pass { program, context.inputs, id, *version, analyses, arenas.types, arenas.strings, context.runner.build() };
                for (PassID step : { PassID::RetypeValues, PassID::SimplifyCasts, PassID::ResolveBuiltins, PassID::SimplifyBuiltinCalls, PassID::ResolveAccesses, PassID::RetypeValues })
                    context.runner.runFunctionPass(step, pass);
            }
            context.runner.runSchedule(FunctionSchedule::Simplify, program, context.inputs, id, *version, arenas);
            if (!round)
                narrowElementsThatAreThere(id, *version);
        }
        return version;
    }

    // Null where the types of the call narrow no parameter.
    const Function* versionFor(FunctionID id, const CallContext& handed, std::unique_ptr<const Function>& versionNotKept)
    {
        auto& keptVersions = versions.add(id.index(), Vector<std::pair<Vector<TypeID, 8>, std::unique_ptr<const Function>>, 1> { }).iterator->value;
        for (const auto& [types, kept] : keptVersions) {
            if (types != handed.types)
                continue;
            if (context.runner.build().options().verifiesAfterEveryPass) {
                std::optional<Function> madeAgain = specialize(id, handed);
                RELEASE_ASSERT(madeAgain && haveSameOperations(*kept, *madeAgain));
            }
            return kept.get();
        }
        std::optional<Function> version = specialize(id, handed);
        if (!version)
            return nullptr;
        std::unique_ptr<const Function> made = makeUniqueWithoutFastMallocCheck<Function>(WTF::move(*version));
        const Function* result = made.get();
        if (numberOfValuesInKeptVersions + result->values.size() > maxValuesInKeptVersions)
            versionNotKept = WTF::move(made);
        else {
            numberOfValuesInKeptVersions += result->values.size();
            keptVersions.append({ handed.types, WTF::move(made) });
        }
        return result;
    }

    InitializerCalleeAnswer answerForBody(FunctionID id, const Function& function, const CallContext& handed)
    {
        FunctionAnalyses analyses(context.analysisContextOf(id), function);
        Knowledge known = findKnowledge(analyses, handed);
        InitializerCalleeAnswer answer { true, true };
        BitVector blocksPassedByEveryRun(function.blocks.size());
        if (analyses.opcodesOfValues().contains(Opcode::InitializeBuiltin) || analyses.opcodesOfValues().intersects(Value::opcodesWithCallees)) {
            for (BlockID passed : findBlocksThatEveryRunPasses(analyses))
                blocksPassedByEveryRun.quickSet(passed.index());
        }
        SetForScope isPassedInThisBody(isPassedByEveryRun, true);
        for (BlockID blockID : analyses.cfg().reversePostOrder()) {
            const Block& block = function.at(blockID);
            // Nothing says of a loop that it ends.
            if (block.handler || analyses.isInCycle(blockID))
                return { };
            isPassedByEveryRun = blocksPassedByEveryRun.quickGet(blockID.index());
            for (ValueID valueID : block.values) {
                if (!isRematerializable(function.at(valueID)) && !mayMove(analyses, known, valueID))
                    return { };
            }
            bool returns = block.terminator.kind == TerminatorKind::Return || block.terminator.kind == TerminatorKind::ReturnCall;
            if (!terminatorMayMove(analyses, blockID) || (returnsToCaller(block.terminator.kind) && !returns))
                return { };
            if (returns && block.terminator.operand(0) && !isMadeOrNoReference(function, known, block.terminator.operand(0)))
                answer.returnsWhatItMade = false;
        }
        return answer;
    }

    InitializerCalleeAnswer answerFor(FunctionID id, const CallContext& handed)
    {
        if (auto found = answers.find(id.index()); found != answers.end()) {
            for (const auto& [knownContext, answer] : found->value) {
                if (knownContext != handed)
                    continue;
                variablesTakenToBeSet.appendVector(answer.variablesTakenToBeSet);
                membersFirstRead.appendVector(answer.membersFirstRead);
                return answer;
            }
        }
        // Many a helper is plain enough as it stands, and that answer serves whatever the types of a call are.
        bool hasTypes = std::ranges::any_of(handed.types, [](TypeID type) { return !!type; });
        size_t numberTakenBefore = variablesTakenToBeSet.size();
        size_t numberReadBefore = membersFirstRead.size();
        InitializerCalleeAnswer asItStands;
        if (hasTypes) {
            asItStands = answerFor(id, CallContext { handed.made, handed.unchanging, { } });
            if (asItStands.mayMove && asItStands.returnsWhatItMade)
                return asItStands;
            variablesTakenToBeSet.shrink(numberTakenBefore);
            membersFirstRead.shrink(numberReadBefore);
        }
        const Function& function = program.at(id);
        std::pair<uint32_t, CallContext> question { id.index(), handed };
        if (!function.hasBody() || function.values.size() > maxValuesOfCallee || beingAnswered.size() >= maxDepth || numberOfAnswers >= maxAnswers || beingAnswered.contains(question))
            return { };

        ++numberOfAnswers;
        beingAnswered.append(question);
        InitializerCalleeAnswer answer;
        std::unique_ptr<const Function> versionNotKept;
        if (!hasTypes)
            answer = answerForBody(id, function, handed);
        else if (const Function* version = versionFor(id, handed, versionNotKept))
            answer = answerForBody(id, *version, handed);
        else {
            answer = WTF::move(asItStands);
            variablesTakenToBeSet.appendVector(std::exchange(answer.variablesTakenToBeSet, { }));
            membersFirstRead.appendVector(std::exchange(answer.membersFirstRead, { }));
        }
        beingAnswered.removeLast();
        // Only the first read of a member adds it. A list that kept the others would double with each function that calls another one twice.
        for (BuiltinID member : membersFirstRead.subspan(numberReadBefore)) {
            if (!answer.membersFirstRead.contains(member))
                answer.membersFirstRead.append(member);
        }
        membersFirstRead.shrink(numberReadBefore);
        membersFirstRead.appendVector(answer.membersFirstRead);
        answer.variablesTakenToBeSet.append(variablesTakenToBeSet.subspan(numberTakenBefore));
        AOT::sort(answer.variablesTakenToBeSet);
        removeRepeatedElements(answer.variablesTakenToBeSet);
        answers.add(id.index(), Vector<std::pair<CallContext, InitializerCalleeAnswer>, 0> { }).iterator->value.append({ handed, answer });
        return answer;
    }

    // What Object.defineProperty makes of a property may refuse a store.
    bool isDefinedByDescriptor(FunctionAnalyses& analyses, ValueID object) const
    {
        const Function& function = analyses.function();
        Vector<ValueID, 4> names { allocationOf(function, object) };
        for (size_t index = 0; index < names.size(); ++index) {
            for (const Use& use : analyses.useLists().usesOf(names[index])) {
                if (use.kind != Use::Kind::Operand)
                    continue;
                const Value& user = function.at(use.user);
                if (isOtherNameOfFirstOperand(function, user) && user.operand(0) == names[index])
                    names.append(use.user);
                else if (user.opcode == Opcode::CallBuiltin && isObjectDefineProperty(builtinCallOf(program, user)))
                    return true;
            }
        }
        return false;
    }

    // A store to an object that the initializer has made and that no accessor, no lock and no other prototype stands in the way of.
    bool storeMayMove(FunctionAnalyses& analyses, const Knowledge& known, const Value& value)
    {
        const Function& function = analyses.function();
        ValueID object = value.objectOperand();
        bool defines = value.opcode == Opcode::PropDefine || value.opcode == Opcode::ElemDefine;
        bool hasName = value.opcode == Opcode::PropDefine || value.opcode == Opcode::PropSet;
        if (!known.made.get(object.index()) || (!hasName && !isPrimitiveKey(function, value.propertyOperand())))
            return false;

        const Type& type = typeOf(function, object);
        if (hasOnlyFunctions(type)) {
            static constexpr ASCIILiteral namesWithRules[] = { "prototype"_s, "name"_s, "length"_s, "caller"_s, "arguments"_s, "__proto__"_s };
            bool setsName = hasName && !defines;
            bool isRefusedAbove = setsName && (program.at(value.key()).kind == PropertyKeyKind::WellKnownSymbol
                || (isText(value.key(), "constructor"_s) && std::ranges::any_of(type.functions, [&](FunctionID id) { return isCoroutine(id); })));
            if (isRefusedAbove || isDefinedByDescriptor(analyses, object))
                return false;
            if (setsName && isText(value.key(), "prototype"_s) && std::ranges::all_of(type.functions, [&](FunctionID id) { return program.at(id).kind == FunctionKind::Plain && !isCoroutine(id); }))
                return true;
            return hasName && (defines || std::ranges::none_of(type.functions, [&](FunctionID id) { return isClassConstructorFunction(program.at(id)); }))
                && std::ranges::none_of(namesWithRules, [&](ASCIILiteral name) { return isText(value.key(), name); });
        }
        // No index of a pristine array is an accessor, and nobody can add one to the prototypes above it.
        // All that was done to an array since the initializer made it has passed these rules, and none of them lets an index become an accessor or the prototype change.
        if (hasOnlyArrays(type, { }) && !hasName && typeOf(function, value.propertyOperand()).isNumber())
            return true;
        if (function.at(allocationOf(function, object)).opcode != Opcode::NewDictionary || !isOnlyFilledHere(analyses, object)) {
            bool hasOnlyPlainObjects = type.isObject() && type.hasOnlyLayouts() && std::ranges::all_of(membersOf(program, type), [&](LayoutID id) {
                return (program.at(id).kind == LayoutKind::Record || program.at(id).kind == LayoutKind::Container) && !program.at(id).base;
            });
            // No such object ever gets an accessor, a lock or another prototype: above it stands Object.prototype, which nobody can change.
            auto isKnownToStayPlain = [&](LayoutID id) {
                const LayoutFlow& flow = program.at(id).flow;
                return flow.impossibleMutations.containsAll({ ObjectMutation::ChangePrototype, ObjectMutation::ChangeIntegrity, ObjectMutation::RedefineProperty }) && flow.ownAccessorsAreComplete && flow.ownAccessors.isEmpty()
                    && std::ranges::none_of(program.at(id).fields, [](const Field& field) { return field.form.containsAny({ FieldForm::IsAccessor, FieldForm::IsReadOnly, FieldForm::IsNotConfigurable }); });
            };
            if (!hasOnlyPlainObjects || (!std::ranges::all_of(membersOf(program, type), isKnownToStayPlain) && !isOnlyFilledHere(analyses, object)))
                return false;
        }
        // The setter of __proto__ does nothing with what is neither an object nor null.
        const Type& stored = typeOf(function, value.valueOperand());
        if (defines || (!stored.hasObjects() && !(stored.primitives & Primitive::Null)))
            return true;
        return hasName ? !isText(value.key(), "__proto__"_s) : typeOf(function, value.propertyOperand()).isNumber();
    }

    // Object.defineProperty(made, constant key, { value: .. }) where the object has no such property yet, or one that can be redefined.
    bool definitionByDescriptorMayMove(FunctionAnalyses& analyses, const Knowledge& known, const Value& value)
    {
        const Function& function = analyses.function();
        BuiltinCall call = builtinCallOf(program, value);
        if (!isObjectDefineProperty(call) || call.arguments.size() != 3 || !known.made.get(call.arguments[0].index()) || !known.made.get(call.arguments[2].index()))
            return false;
        const Value& key = function.at(call.arguments[1]);
        bool isWellKnownSymbol = key.opcode == Opcode::Builtin && typeOf(function, call.arguments[1]).hasOnlyPrimitives(Primitive::Symbol | Primitive::LiteralSymbol);
        bool hasNameWithRules = key.opcode == Opcode::String && (isText(key.string(), "prototype"_s) || isText(key.string(), "caller"_s) || isText(key.string(), "arguments"_s));
        if ((key.opcode != Opcode::String && !isWellKnownSymbol) || !hasOnlyFunctions(typeOf(function, call.arguments[0])) || hasNameWithRules)
            return false;

        // One definition under each key: what the first one makes cannot be redefined.
        ValueID allocation = allocationOf(function, call.arguments[0]);
        if (function.at(allocation).opcode != Opcode::NewClosure)
            return false;
        for (const Value& other : function.values) {
            if (&other == &value || other.opcode != Opcode::CallBuiltin)
                continue;
            BuiltinCall otherCall = builtinCallOf(program, other);
            if (!isObjectDefineProperty(otherCall) || otherCall.arguments.size() != 3 || allocationOf(function, otherCall.arguments[0]) != allocation)
                continue;
            const Value& otherKey = function.at(otherCall.arguments[1]);
            bool differs = otherKey.opcode != key.opcode ? (otherKey.opcode == Opcode::String || otherKey.opcode == Opcode::Builtin)
                : key.opcode == Opcode::String ? otherKey.string() != key.string() : otherKey.builtin() != key.builtin();
            if (!differs)
                return false;
        }

        // The descriptor is a literal that goes nowhere else and has `value` and flags.
        ValueID descriptor = call.arguments[2];
        for (;; descriptor = function.at(descriptor).operand(0)) {
            if (analyses.useLists().useCount(descriptor) != 1)
                return false;
            if (!isCast(function.at(descriptor).opcode))
                break;
        }
        std::optional<DescriptorLiteral> described = readDescriptorLiteral(program, function, function.at(descriptor));
        return described && !described->isAccessor() && !described->closureEnvironment;
    }

    bool mayMoveByEffects(FunctionAnalyses& analyses, const Knowledge& known, ValueID id)
    {
        const Function& function = analyses.function();
        const Value& value = function.at(id);
        const EffectsOnEdges& onEdges = analyses.effectsOf(id);
        Effects effects = onEdges.onAnyEdge();
        if (effects.mayCallUnknownCode || effects.maySuspend || effects.mayThrowByRule() || effects.mayBeTerminated)
            return false;

        const EffectContext& effectContext = analyses.effectContext();
        bool readsObjects = false;
        bool writesObjects = false;
        if (effectRuleOf(value.opcode) == EffectRule::Call) {
            Effects beyond = effectsBeyondOperands(effectContext, function, value, onEdges).onAnyEdge();
            if (!beyond.writes.containsOnly(heapsNobodyObserves | heapsWhoseWritesNobodySees) || !beyond.reads.containsOnly(heapsNobodyObserves))
                return false;
        } else {
            // What such an operation reads and writes of objects, it reads and writes of the objects that its operands name.
            constexpr AbstractHeapSet heapsOfOperands = heapsNobodyObserves | perObjectHeaps;
            if (!effects.writes.containsOnly(heapsOfOperands) || !effects.reads.containsOnly(heapsOfOperands))
                return false;
            readsObjects = effects.reads.containsAny(perObjectHeaps);
            writesObjects = effects.writes.containsAny(perObjectHeaps);
        }

        auto mayBeTaken = [&](ValueID operand, uint32_t index) {
            OperandSummary summary = summarizeOperand(effectContext, function, value, index);
            bool isRetained = summary.uses.containsAny({ OperandUse::Retains, OperandUse::RetainsContents });
            bool isRetainedOutside = isRetained && (summary.retainedIn == RetainedIn::UnknownMemory || (summary.retainedIn == RetainedIn::Operand && !known.made.get(value.operand(summary.retainingOperand).index())));
            if (isRetainedOutside || summary.uses.containsAny({ OperandUse::ChangesPrototype, OperandUse::ChangesIntegrity }))
                return false;
            if (known.made.get(operand.index()))
                return true;
            uint32_t declaredIndex = std::min(index, numberOfNamedOperandsOf(value.opcode));
            bool mayBeWritten = writesObjects && (confinementOf(value.opcode) != OperandConfinement::WritesOnlyNamedOperands || isWrittenOperand(value.opcode, declaredIndex));
            if (mayBeWritten || summary.uses.containsAny(operandWritingUses | unconfinedUses))
                return false;
            if (known.unchanging.get(operand.index()) && !summary.uses.containsAny(callingUses))
                return true;
            return !readsObjects && usesOfObjectsMadeElsewhere.containsAll(summary.uses);
        };
        bool isFine = true;
        value.forEachOperand([&](ValueID operand, uint32_t index) {
            isFine = isFine && (!mayBeReference(typeOf(function, operand)) || mayBeTaken(operand, index));
        });
        return isFine;
    }

    // The member is read where the item stood, so the operation goes with the item.
    std::optional<bool> firstReadMayMove(const Value& value)
    {
        if (value.opcode != Opcode::InitializeBuiltin)
            return std::nullopt;
        if (!isPassedByEveryRun || !onlyAddsMemberToHolder(program.builtins(), value.builtin()))
            return false;
        membersFirstRead.append(value.builtin());
        return true;
    }

    bool mayMove(FunctionAnalyses& analyses, const Knowledge& known, ValueID id)
    {
        const Function& function = analyses.function();
        const Value& value = function.at(id);
        if (std::optional<bool> answer = firstReadMayMove(value))
            return *answer;
        switch (value.opcode) {
        case Opcode::LoopHint:
            // The caller has looked at the loop.
            return true;
        case Opcode::GetModuleVariable:
            if (context.variables.holdsOneValue.quickGet(value.variable().index()) || !context.variables.isSetWhenItsModuleHasRun.quickGet(value.variable().index()))
                return context.variables.holdsOneValue.quickGet(value.variable().index());
            variablesTakenToBeSet.append(value.variable());
            return true;
        case Opcode::PropDefine:
        case Opcode::ElemDefine:
        case Opcode::PropSet:
        case Opcode::ElemSet:
            if (storeMayMove(analyses, known, value))
                return true;
            break;
        case Opcode::ResolveHeritage:
            if (hasOnlyClassesAndBuiltins(program, typeOf(function, value.heritageOperand())))
                return true;
            break;
        case Opcode::NewClass:
            return known.made.get(value.constructorOperand().index());
        case Opcode::DefineMethod:
            // The prototype property of a class cannot be redefined.
            if (!value.key() && value.side() == ClassSide::Static && (typeOf(function, value.computedKeyOperand()).primitives & Primitive::AnyString))
                return false;
            return known.made.get(value.classObjectOperand().index()) && (value.key() || isPrimitiveKey(function, value.computedKeyOperand()));
        case Opcode::CallBuiltin:
            return definitionByDescriptorMayMove(analyses, known, value);
        case Opcode::Call:
        case Opcode::Construct:
        case Opcode::CallDirect:
            return answerForCall(function, known, value).mayMove;
        default:
            // What a summary says of the memory that a callee touches does not tell the clock or the random numbers from bookkeeping.
            if (effectRuleOf(value.opcode) == EffectRule::Call || effectRuleOf(value.opcode) == EffectRule::Builtin)
                return false;
            break;
        }
        return mayMoveByEffects(analyses, known, id);
    }
};

struct InitializerPlan {
    const InitializerUnit* find(VariableID variable) const LIFETIME_BOUND { return unitOf[variable.index()] == UINT32_MAX ? nullptr : &units[unitOf[variable.index()]]; }
    Emptiness emptinessOf(VariableID variable) const { return variable.index() < emptiness.size() ? emptiness[variable.index()] : Emptiness::AbsentFromBirth; }

    const Vector<Emptiness>& emptiness;
    // In the order of the bodies.
    Vector<InitializerUnit> units { };
    Vector<uint32_t> unitOf { };
};

} // anonymous namespace

// Of a body that runs at most once and stores to a variable that may stay empty.
static void findInitializerUnits(const InitializerScanContext& context, FunctionID id, TaskArenas& arenas, Vector<InitializerUnit>& found)
{
    constexpr uint32_t noItem = UINT32_MAX;
    enum class State : uint8_t { Stays, IsRepeated, Moves };
    const Program& program = context.program;
    const VariableFacts& variables = context.variables;
    const Function& function = program.at(id);
    FunctionAnalyses analyses(context.analysisContextOf(id), function);
    MovableCode code { context, arenas };
    CFG& cfg = analyses.cfg();

    Vector<BlockID, 16> spine = findBlocksThatEveryRunPasses(analyses);
    if (spine.isEmpty())
        return;
    std::ranges::reverse(spine);
    BitVector isOnSpine(function.blocks.size());
    for (BlockID block : spine)
        isOnSpine.quickSet(block.index());

    Vector<Item> items;
    Vector<uint32_t> itemOf(FillWith { }, function.values.size(), noItem);
    auto addToNextItem = [&](const auto& values) {
        for (ValueID value : values)
            itemOf[value.index()] = items.size();
    };
    for (size_t position = 0; position < spine.size(); ++position) {
        for (ValueID value : function.at(spine[position]).values) {
            itemOf[value.index()] = items.size();
            items.append(Item { .block = spine[position], .value = value });
        }
        bool isLast = position + 1 == spine.size();
        Item region { .block = spine[position], .join = isLast ? BlockID() : spine[position + 1] };
        BitVector seen(function.blocks.size());
        Vector<BlockID, 8> worklist { region.block };
        while (!worklist.isEmpty()) {
            for (BlockID successor : cfg.successors(worklist.takeLast())) {
                if (successor == region.join || seen.quickSet(successor.index()))
                    continue;
                if (isOnSpine.quickGet(successor.index())) {
                    if (isLast)
                        continue;
                    return;
                }
                region.blocks.append(successor);
                worklist.append(successor);
            }
        }
        AOT::sort(region.blocks, { }, [&](BlockID interior) { return cfg.reversePostOrderIndex(interior); });
        for (BlockID interior : region.blocks) {
            addToNextItem(function.at(interior).parameters);
            addToNextItem(function.at(interior).values);
        }
        if (region.join)
            addToNextItem(function.at(region.join).parameters);
        items.append(WTF::move(region));
    }
    auto storeAt = [&](uint32_t index) -> const Value* {
        ValueID value = items[index].value;
        return value && function.at(value).opcode == Opcode::PutModuleVariable ? &function.at(value) : nullptr;
    };

    Knowledge known = code.findKnowledge(analyses, { });
    Vector<VariableID> variableHolding(function.values.size());
    Vector<uint32_t> storeOfHeldValue(FillWith { }, function.values.size(), noItem);
    MapByIndex<uint32_t> firstStoreTo;
    BitVector isStoredHere;
    for (uint32_t index = 0; index < items.size(); ++index) {
        const Value* store = storeAt(index);
        if (!store)
            continue;
        firstStoreTo.add(store->variable().index(), index);
        if (!variables.isStoredOnce.quickGet(store->variable().index()))
            continue;
        if (variables.mayStayEmpty(store->variable()))
            isStoredHere.set(store->variable().index());
        for (ValueID held = store->valueOperand();; held = function.at(held).operand(0)) {
            if (!variableHolding[held.index()]) {
                variableHolding[held.index()] = store->variable();
                storeOfHeldValue[held.index()] = index;
            }
            if (!isCastWithoutTest(function.at(held).opcode))
                break;
        }
    }

    auto givesTheSameWheneverDone = [&](ValueID start) {
        for (ValueID link = start;; link = function.at(link).operand(0)) {
            const Value& value = function.at(link);
            if (value.origin().inlineFrame)
                return false;
            if (isRematerializable(value))
                return true;
            // A read behind the store of this body goes where the store goes.
            if (value.opcode == Opcode::GetModuleVariable)
                return variables.holdsOneValue.quickGet(value.variable().index()) && !isStoredHere.get(value.variable().index());
            if (leavesDeadZoneBehind(program, function, value)) {
                const Value& tested = function.at(value.valueOperand());
                if (tested.opcode == Opcode::GetModuleVariable && variables.isStoredOnce.quickGet(tested.variable().index()) && !tested.origin().inlineFrame
                    && program.at(tested.variable()).initialization == VariableInitialization::Uninitialized && !isStoredHere.get(tested.variable().index()))
                    return true;
                if (program.typeOf(function, value.valueOperand()).primitives & Primitive::Absent)
                    return false;
            } else if (!isCastWithoutTest(value.opcode) || std::ranges::count_if(value.operands(), [](ValueID operand) { return !!operand; }) != 1)
                return false;
        }
    };

    // An object that several operations take ties them together, and one of them that stays keeps them all. It gets a variable of its own, named by its number until then.
    for (uint32_t index = 0; index < items.size(); ++index) {
        ValueID value = items[index].value;
        if (!value || variableHolding[value.index()] || !known.made.get(value.index()) || givesTheSameWheneverDone(value)
            || !mayBeReference(program.typeOf(function, value)) || function.at(value).origin().inlineFrame)
            continue;
        size_t numberOfUsers = std::ranges::count_if(analyses.useLists().usesOf(value), [&](const Use& use) {
            return use.kind != Use::Kind::Operand || !namesItsFirstOperand(function.at(use.user).opcode);
        });
        if (numberOfUsers < 2)
            continue;
        variableHolding[value.index()] = VariableID(program.variables.size() + value.index());
        storeOfHeldValue[value.index()] = index;
    }
    auto isHeldByAddedVariable = [&](ValueID value) { return variableHolding[value.index()] && variableHolding[value.index()].index() >= program.variables.size(); };
    Knowledge knownIfVariablesHoldWhatWasMadeElsewhere = code.findKnowledge(analyses, { }, &variableHolding);

    Vector<State> states(FillWith { }, items.size(), State::Stays);
    Vector<uint32_t> parents(items.size(), [](size_t index) { return static_cast<uint32_t>(index); });
    // By the representative of a unit, which is its first item.
    BitVector isKept(items.size());
    // Items that take what a variable holds without looking into it: they read the variable.
    Vector<std::pair<uint32_t, ValueID>> readings;
    Vector<uint32_t> itemsUsedByCodeThatStays;
    BitVector modulesImported;
    auto find = [&](uint32_t item) {
        while (parents[item] != item) {
            parents[item] = parents[parents[item]];
            item = parents[item];
        }
        return item;
    };
    auto unite = [&](uint32_t a, uint32_t b) {
        auto [earlier, later] = std::minmax({ find(a), find(b) });
        parents[later] = earlier;
    };
    auto isPutOff = [&](uint32_t item) { return states[item] == State::Moves && !isKept.quickGet(find(item)); };
    auto stateOf = [&](ValueID value) { return itemOf[value.index()] == noItem ? State::Stays : states[itemOf[value.index()]]; };

    auto forEachValueTakenBy = [&](const Item& item, const auto& functor) {
        if (!item.isRegion())
            return function.at(item.value).forEachChild(functor);
        BitVector isInside(function.blocks.size());
        for (BlockID block : item.blocks)
            isInside.quickSet(block.index());
        auto take = [&](ValueID taken) {
            if (!taken)
                return;
            ValueLocation location = analyses.useLists().locationOf(taken);
            if (location.placement == ValuePlacement::Detached || !isInside.quickGet(location.block.index()))
                functor(taken);
        };
        function.at(item.block).forEachChildOutsideValues(take);
        for (BlockID block : item.blocks) {
            for (ValueID value : function.at(block).values)
                function.at(value).forEachChild(take);
            function.at(block).forEachChildOutsideValues(take);
        }
    };
    auto mayMove = [&](const Item& item, const Knowledge& given) {
        auto valueMayMove = [&](ValueID value) {
            return !function.at(value).origin().inlineFrame && (isRematerializable(function.at(value)) || code.mayMove(analyses, given, value));
        };
        auto terminatorMayMove = [&](BlockID block) { return !function.at(block).terminator.origin().inlineFrame && code.terminatorMayMove(analyses, block); };
        if (!item.isRegion())
            return valueMayMove(item.value);
        SetForScope isPassedInsideRegion(code.isPassedByEveryRun, false);
        // Nothing says of a loop that it ends.
        return terminatorMayMove(item.block) && std::ranges::all_of(item.blocks, [&](BlockID interior) {
            const Block& block = function.at(interior);
            return !block.handler && !block.isHandlerBlock() && !analyses.isInCycle(interior) && std::ranges::all_of(block.values, valueMayMove) && terminatorMayMove(interior);
        });
    };

    auto classify = [&](uint32_t index) {
        const Item& item = items[index];
        const Block& block = function.at(item.block);
        const Value* value = item.isRegion() ? nullptr : &function.at(item.value);
        auto stay = [&] {
            forEachValueTakenBy(item, [&](ValueID taken) {
                // What is done again reads a variable, which code that stays reads at the start.
                for (ValueID link = taken;; link = function.at(link).operand(0)) {
                    if (stateOf(link) == State::Moves)
                        itemsUsedByCodeThatStays.append(itemOf[link.index()]);
                    if (stateOf(link) != State::IsRepeated || !function.at(link).numberOfOperands() || !function.at(link).operand(0))
                        break;
                }
            });
        };
        if (value && givesTheSameWheneverDone(item.value) && !variableHolding[item.value.index()]) {
            states[index] = State::IsRepeated;
            // It is also left where it stands, with what it casts.
            return stay();
        }
        if (block.handler || block.isHandlerBlock() || item.endsBody() || analyses.executionCountOf(item.block) != ExecutionCount::AtMostOnce)
            return stay();
        if (value && value->opcode == Opcode::InitializeBuiltin)
            return stay();
        if (value && value->opcode == Opcode::GetModuleVariable && isStoredHere.get(value->variable().index())) {
            uint32_t store = firstStoreTo.get(value->variable().index());
            if (store >= index || states[store] != State::Moves)
                return stay();
            states[index] = State::Moves;
            return unite(index, store);
        }
        bool isStore = storeAt(index);
        if (isStore && !variables.mayStayEmpty(value->variable()))
            return stay();

        // First as if what variables hold were made elsewhere: then the item only reads those variables.
        code.variablesTakenToBeSet.shrink(0);
        code.membersFirstRead.shrink(0);
        bool looksIntoWhatVariablesHold = !isStore && !mayMove(item, knownIfVariablesHoldWhatWasMadeElsewhere);
        if (looksIntoWhatVariablesHold) {
            code.membersFirstRead.shrink(0);
            if (!mayMove(item, known))
                return stay();
        }
        MembersFirstRead membersFirstRead;
        for (BuiltinID member : code.membersFirstRead) {
            if (!membersFirstRead.contains(member))
                membersFirstRead.append(member);
        }
        ModuleID evaluated = context.moduleWithBody[id.index()];
        bool areSetInFrontOfItem = std::ranges::all_of(code.variablesTakenToBeSet, [&](VariableID variable) {
            ModuleID module = program.at(variable).module;
            if (module != (evaluated ? evaluated : function.module))
                return evaluated && context.hasRunBefore(module, evaluated, modulesImported);
            // The store of this body stands in front of the item.
            auto store = firstStoreTo.find(variable.index());
            return store != firstStoreTo.end() && store->value < index;
        });
        if (!areSetInFrontOfItem)
            return stay();

        // What follows looks into the object under its other name, so the item goes where the object is made.
        bool handsOnWhatVariablesHold = value
            ? isCast(value->opcode) || value->opcode == Opcode::FinishClass || (known.made.get(item.value.index()) && !code.isMadeGiven(analyses, knownIfVariablesHoldWhatWasMadeElsewhere, *value))
            : std::ranges::any_of(function.at(item.join).parameters, [&](ValueID parameter) { return known.made.get(parameter.index()) && !knownIfVariablesHoldWhatWasMadeElsewhere.made.get(parameter.index()); });
        bool mayReadVariables = !handsOnWhatVariablesHold && !looksIntoWhatVariablesHold && !isStore;
        bool takesWhatStays = false;
        Vector<uint32_t, 8> united;
        Vector<ValueID, 4> read;
        forEachValueTakenBy(item, [&](ValueID taken) {
            uint32_t definingItem = itemOf[taken.index()];
            if (definingItem == index || stateOf(taken) == State::IsRepeated)
                return;
            if (definingItem != noItem && mayReadVariables && variableHolding[taken.index()] && storeOfHeldValue[taken.index()] < index)
                read.append(taken);
            else if (stateOf(taken) == State::Stays)
                takesWhatStays = true;
            else
                united.append(definingItem);
        });
        if (takesWhatStays)
            return stay();
        states[index] = State::Moves;
        items[index].membersFirstRead = WTF::move(membersFirstRead);
        for (uint32_t other : united)
            unite(index, other);
        for (ValueID taken : read)
            readings.append({ index, taken });
    };
    for (uint32_t index = 0; index < items.size(); ++index)
        classify(index);

    auto estimatedCostOf = [&](const Item& item) -> unsigned {
        constexpr unsigned costOfCode = 1000;
        if (item.isRegion())
            return costOfCode;
        const Value& value = function.at(item.value);
        if (effectRuleOf(value.opcode) == EffectRule::Call || effectRuleOf(value.opcode) == EffectRule::Builtin || value.opcode == Opcode::NewClass || value.opcode == Opcode::NativeBinding)
            return costOfCode;
        switch (value.opcode) {
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
    };
    BitVector fillsAddedVariable(items.size());
    Vector<unsigned> costs(FillWith { }, items.size(), 0u);
    Vector<uint32_t> firstStore(FillWith { }, items.size(), noItem);
    Vector<uint32_t> lastItem(FillWith { }, items.size(), 0u);
    for (uint32_t index = 0; index < items.size(); ++index) {
        if (states[index] != State::Moves)
            continue;
        uint32_t unit = find(index);
        costs[unit] += estimatedCostOf(items[index]);
        lastItem[unit] = index;
        if (firstStore[unit] == noItem && storeAt(index))
            firstStore[unit] = index;
    }
    for (auto [item, value] : readings) {
        if (isHeldByAddedVariable(value) && stateOf(value) == State::Moves)
            fillsAddedVariable.quickSet(find(itemOf[value.index()]));
    }

    // What code that stays takes is needed at the start, and so is all that was put off with it and all that those operations take.
    auto keepUnitsNeededAtStart = [&] {
        for (uint32_t item : itemsUsedByCodeThatStays)
            isKept.quickSet(find(item));
        for (bool hasChanged = true; hasChanged;) {
            hasChanged = false;
            // What reads comes behind what it reads: from the last one on, one round follows a whole chain.
            for (auto [item, value] : readings.span() | std::views::reverse) {
                if (!isPutOff(itemOf[value.index()]) || !isKept.quickGet(find(item)))
                    continue;
                isKept.quickSet(find(itemOf[value.index()]));
                hasChanged = true;
            }
        }
    };
    // A unit that fills no variable is there for code that stays. One that makes nothing costs less than the tests at its reads.
    // A function of its own, and a test at each read, cost more than a small literal.
    AOT_THRESHOLD(unsigned, leastCost, 100);
    for (uint32_t unit = 0; unit < items.size(); ++unit) {
        if (states[unit] == State::Moves && find(unit) == unit && ((firstStore[unit] == noItem && !fillsAddedVariable.quickGet(unit)) || costs[unit] < leastCost))
            isKept.quickSet(unit);
    }
    keepUnitsNeededAtStart();

    // Between the first store of a unit and its last operation the program sees the objects half done. Put off, they are never seen so.
    auto runsCompiledCode = [&](ValueID value) { return effectRuleOf(function.at(value).opcode) == EffectRule::Call || analyses.mayRunOtherCodeOnAnyEdge(value); };
    Vector<uint32_t> itemsThatRunCode;
    for (uint32_t index = 0; index < items.size(); ++index) {
        const Item& item = items[index];
        if (states[index] == State::IsRepeated)
            continue;
        if (item.isRegion() ? std::ranges::any_of(item.blocks, [&](BlockID block) { return std::ranges::any_of(function.at(block).values, runsCompiledCode); }) : runsCompiledCode(item.value))
            itemsThatRunCode.append(index);
    }
    for (uint32_t unit = 0; unit < items.size(); ++unit) {
        if (!isPutOff(unit) || find(unit) != unit || firstStore[unit] == noItem)
            continue;
        auto running = std::ranges::upper_bound(itemsThatRunCode, firstStore[unit]);
        if (std::any_of(running, std::lower_bound(running, itemsThatRunCode.end(), lastItem[unit]), [&](uint32_t item) { return states[item] != State::Moves || find(item) != unit; }))
            isKept.quickSet(unit);
    }
    // A unit that is kept takes what it reads at the start.
    keepUnitsNeededAtStart();

    Vector<uint32_t> unitAt(FillWith { }, items.size(), noItem);
    size_t first = found.size();
    for (uint32_t index = 0; index < items.size(); ++index) {
        if (!isPutOff(index))
            continue;
        uint32_t& position = unitAt[find(index)];
        if (position == noItem) {
            position = found.size();
            found.append(InitializerUnit { .body = id });
        }
        found[position].items.append(items[index]);
        if (const Value* store = storeAt(index))
            found[position].variables.append(store->variable());
    }
    size_t end = found.size();
    InitializerUnit whatStays { .body = id, .staysInBody = true };
    BitVector isTaken(function.values.size());
    BitVector hasVariable(function.values.size());
    for (size_t position = first; position < end; ++position) {
        isTaken.clearAll();
        for (const Item& item : found[position].items) {
            forEachValueTakenBy(item, [&](ValueID taken) {
                uint32_t definingItem = itemOf[taken.index()];
                if ((states[definingItem] == State::Moves && unitAt[find(definingItem)] == position) || isTaken.quickSet(taken.index()))
                    return;
                if (states[definingItem] == State::IsRepeated)
                    return found[position].repeated.append(taken);
                ValueOfVariable held { taken, variableHolding[taken.index()] };
                found[position].valuesOfVariables.append(held);
                if (!isHeldByAddedVariable(taken) || hasVariable.quickSet(taken.index()))
                    return;
                InitializerUnit& maker = isPutOff(definingItem) ? found[unitAt[find(definingItem)]] : whatStays;
                maker.addedVariables.append(held);
                if (!maker.staysInBody)
                    maker.variables.append(held.variable);
            });
        }
    }
    if (!whatStays.addedVariables.isEmpty())
        found.append(WTF::move(whatStays));
}

static ValueID assumeWithout(IRBuilder& builder, ValueID value, uint16_t primitives, ClaimHolds holds = ClaimHolds::WhereverValueIs)
{
    return assumeType(builder, value, builder.types().intern(builder.types().at(builder.function().at(value).type).withoutPrimitives(primitives)), holds);
}

static void buildInitializerUnit(const InitializerUnit& unit, const Function& body, IRBuilder& builder)
{
    MapByIndex<ValueID> values;
    MapByIndex<BlockID> blocks;
    auto copyIn = [](const auto& copies, auto original) {
        auto found = copies.find(original.index());
        RELEASE_ASSERT(found != copies.end());
        return found->value;
    };
    // The variable may stay empty from now on, and its type says so. This read finds it filled, as it did.
    auto readInPlaceOf = [&](ValueID id, VariableID variable) {
        builder.setOrigin(body.at(id).origin());
        values.add(id.index(), assumeType(builder, builder.append(makeVariableAccess(Opcode::GetModuleVariable, variable)), body.at(id).type));
    };
    auto copyValue = [&](ValueID id) {
        if (body.at(id).opcode == Opcode::GetModuleVariable)
            return readInPlaceOf(id, body.at(id).variable());
        Value value(body.at(id));
        builder.setOrigin(value.origin());
        value.forEachChild([&](ValueID& operand) { operand = copyIn(values, operand); });
        values.add(id.index(), builder.append(WTF::move(value)));
    };
    auto addBlockLike = [&](BlockID original) {
        const Block& block = body.at(original);
        BlockID added = builder.addBlock(block.frequency);
        builder.function().edit(added, BlockEdit::Rest).shape = block.shape;
        for (ValueID parameter : block.parameters) {
            ValueID copy = builder.addParameter(added, body.at(parameter).type);
            builder.function().edit(copy, ValueEdit::Type).setExpected(body.at(parameter).expected());
            values.add(parameter.index(), copy);
        }
        blocks.add(original.index(), added);
    };
    auto copyTerminator = [&](const Terminator& terminator) {
        Terminator copy(terminator);
        copy.forEachChild([&](ValueID& child) { child = copyIn(values, child); });
        for (Target& target : copy.targets)
            target.block = copyIn(blocks, target.block);
        builder.setOrigin(terminator.origin());
        builder.emitTerminator(WTF::move(copy));
    };

    auto repeat = [&](ValueID id) {
        // A cast comes behind what it casts. The test for a dead zone has passed where it stands.
        Vector<ValueID, 4> chain;
        for (ValueID link = id; link && !values.contains(link.index()); link = body.at(link).numberOfOperands() ? body.at(link).operand(0) : ValueID())
            chain.append(link);
        for (ValueID link : chain.span() | std::views::reverse) {
            if (body.at(link).opcode != Opcode::CheckInitialized) {
                copyValue(link);
                continue;
            }
            builder.setOrigin(body.at(link).origin());
            values.add(link.index(), assumeType(builder, copyIn(values, body.at(link).operand(0)), body.at(link).type));
        }
    };
    // What is done again is done where it is first taken, as in the body. All of it at the top would be held from there to its use, and a long initializer has more of it than there are registers.
    // The place is one that every run of the function passes, so what is done there is there for whatever takes it later.
    BitVector isRepeated(body.values.size());
    for (ValueID id : unit.repeated)
        isRepeated.quickSet(id.index());
    auto repeatWhatIsTakenBy = [&](const auto& user) {
        user.forEachChild([&](ValueID operand) {
            if (operand && isRepeated.quickGet(operand.index()))
                repeat(operand);
        });
    };

    builder.setOrigin(unit.items.first().originIn(body));
    builder.moveToEndOf(builder.addBlock());
    for (const ValueOfVariable& taken : unit.valuesOfVariables)
        readInPlaceOf(taken.value, taken.variable);

    ValueID stored;
    for (const Item& item : unit.items) {
        if (!item.isRegion()) {
            repeatWhatIsTakenBy(body.at(item.value));
            copyValue(item.value);
            if (body.at(item.value).opcode == Opcode::PutModuleVariable)
                stored = copyIn(values, body.at(item.value).valueOperand());
            continue;
        }
        repeatWhatIsTakenBy(body.at(item.block).terminator);
        for (BlockID block : item.blocks) {
            for (ValueID id : body.at(block).values)
                repeatWhatIsTakenBy(body.at(id));
            repeatWhatIsTakenBy(body.at(block).terminator);
        }
        for (BlockID block : item.blocks)
            addBlockLike(block);
        addBlockLike(item.join);
        copyTerminator(body.at(item.block).terminator);
        for (BlockID block : item.blocks) {
            builder.moveToEndOf(copyIn(blocks, block));
            for (ValueID id : body.at(block).values)
                copyValue(id);
            copyTerminator(body.at(block).terminator);
        }
        builder.moveToEndOf(copyIn(blocks, item.join));
    }
    for (ValueID id : unit.repeated)
        repeat(id);
    for (const ValueOfVariable& added : unit.addedVariables) {
        stored = copyIn(values, added.value);
        builder.append(makeVariableAccess(Opcode::PutModuleVariable, added.variable, { stored }));
    }
    builder.emitReturn(unit.variables.size() == 1 ? assumeType(builder, stored, unit.type) : builder.appendUndefined());
}

static void removeUnits(const InitializerPlan& plan, std::span<const InitializerUnit> units, IRBuilder& builder)
{
    if (units.empty())
        return;
    Function& function = builder.function();
    BitVector isRemoved(function.values.size());
    BitVector marksDeclaration(function.values.size());
    MapByIndex<VariableID> storedWhereMade;
    MapByIndex<const MembersFirstRead*> firstReadsLeftBehind;
    auto remove = [&](const auto& values) {
        for (ValueID value : values)
            isRemoved.quickSet(value.index());
    };
    for (const InitializerUnit& unit : units) {
        if (unit.staysInBody) {
            for (const ValueOfVariable& added : unit.addedVariables)
                storedWhereMade.add(added.value.index(), added.variable);
        }
        for (const Item& item : unit.items) {
            if (!item.isRegion()) {
                bool staysAsMark = function.at(item.value).opcode == Opcode::PutModuleVariable && plan.emptinessOf(function.at(item.value).variable()) != Emptiness::AbsentFromBirth;
                (staysAsMark ? marksDeclaration : isRemoved).quickSet(item.value.index());
                if (!item.membersFirstRead.isEmpty())
                    firstReadsLeftBehind.add(item.value.index(), &item.membersFirstRead);
                continue;
            }
            for (BlockID id : item.blocks) {
                Block& block = function.edit(id);
                remove(block.parameters);
                remove(block.values);
                block = Block { };
            }
            Block& join = function.edit(item.join);
            remove(join.parameters);
            join.parameters.clear();
            Terminator jump = IRBuilder::makeTerminator(TerminatorKind::Jump);
            jump.setOrigin(function.at(item.block).terminator.origin());
            jump.targets.append(Target { item.join, { }, { } });
            function.edit(item.block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator = WTF::move(jump);
        }
    }
    removeValues(builder, isRemoved, storedWhereMade, [&](ValueID id) -> std::optional<Opcode> {
        if (!marksDeclaration.quickGet(id.index()))
            return std::nullopt;
        return plan.emptinessOf(function.at(id).variable()) == Emptiness::AbsentFromDeclaration ? Opcode::Absent : Opcode::Undefined;
    }, firstReadsLeftBehind);
}

static void fillVariablesAtReads(const InitializerPlan& plan, FunctionID id, IRBuilder& builder)
{
    Function& function = builder.function();
    Vector<uint32_t> numberOfUses(FillWith { }, function.values.size(), 0u);
    auto count = [&](ValueID used) {
        if (used)
            ++numberOfUses[used.index()];
    };
    for (const Block& block : function.blocks) {
        for (ValueID value : block.values)
            function.at(value).forEachChild(count);
        block.forEachChildOutsideValues(count);
    }
    // A read that nothing takes needs no value, and neither does one that only the test for the dead zone takes, whose result nothing takes.
    BitVector needsNoValue(function.values.size());
    for (const Block& block : function.blocks) {
        for (ValueID valueID : block.values) {
            const Value& value = function.at(valueID);
            if (numberOfUses[valueID.index()])
                continue;
            if (value.opcode == Opcode::GetModuleVariable)
                needsNoValue.quickSet(valueID.index());
            if (value.opcode == Opcode::CheckInitialized && function.at(value.valueOperand()).opcode == Opcode::GetModuleVariable && numberOfUses[value.valueOperand().index()] == 1)
                needsNoValue.quickSet(value.valueOperand().index());
        }
    }
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
                    value.type = builder.context().program.provenTypeOf(builder.context().program.at(value.variable()).declared);
            }
        }
    }
    // What fills a variable reads it behind its own store.
    auto isFilledAtRead = [&](ValueID read, VariableID variable) { return plan.find(variable) && plan.find(variable)->function != id && !needsNoValue.quickGet(read.index()); };
    forEachReadBackwards(function, isFilledAtRead, [&](ProgramPoint point, VariableID variable) {
        const InitializerUnit& unit = *plan.find(variable);
        bool isMarkedByUndefined = plan.emptinessOf(variable) == Emptiness::UndefinedFromDeclaration;
        IRBuilder::Expansion expansion = builder.beginExpansion(point);
        ValueID held = builder.append(makeVariableAccess(Opcode::GetModuleVariable, variable));
        IRBuilder::BranchBlocks branch = builder.emitBranch(builder.appendTypeTest(held, isMarkedByUndefined ? TypeTable::undefined : TypeTable::absent), BlockFrequency::Rare, BlockFrequency::Normal);

        builder.moveToEndOf(branch.taken);
        Value call = IRBuilder::make(Opcode::CallDirect);
        call.setFunction(unit.function);
        call.setArgumentsOperands({ });
        ValueID made = builder.append(WTF::move(call));
        if (unit.variables.size() != 1)
            made = assumeWithout(builder, builder.append(makeVariableAccess(Opcode::GetModuleVariable, variable)), isMarkedByUndefined ? Primitive::Absent | Primitive::Undefined : Primitive::Absent);
        builder.endExpansion(expansion, made);

        // A cast that names a fact made behind the test: it stays there, and so does whoever looks at the value.
        builder.moveToEndOf(branch.notTaken);
        builder.endExpansion(expansion, assumeWithout(builder, held, isMarkedByUndefined ? Primitive::Undefined : Primitive::Absent, ClaimHolds::FromHereOn));
    });
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
    Vector<ModuleID> moduleWithBody(program.functions.size());
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        if (FunctionID body = program.modules[index].body)
            moduleWithBody[body.index()] = ModuleID(index);
    }
    VariableFacts variables = collectVariableFacts(program, flow, moduleWithBody, build);
    times.didFinish("collectVariableFacts"_s);
    if (std::ranges::all_of(variables.emptiness, [](Emptiness emptiness) { return emptiness == Emptiness::None; }))
        return;
    // A module that requires its importer while its own body runs closes a circle too.
    using ModulePair = std::pair<ModuleID, ModuleID>;
    Vector<ModulePair> requiredModules = collectInParallel<ModulePair>(loops, program.functions.size(), [&](unsigned index, Vector<ModulePair>& found) {
        forEachValueListedOrNot(program, FunctionID(index), { Opcode::RequireModule, Opcode::EvaluateModule }, [&](ValueID, const Value& value) {
            if (value.opcode == Opcode::RequireModule && program.functions[index].module)
                found.append({ program.functions[index].module, value.module() });
            if (value.opcode == Opcode::EvaluateModule)
                found.append({ value.referrer(), value.module() });
        });
    });
    Vector<Vector<ModuleID>> modulesReachedFrom(program.modules.size(), [&](size_t index) {
        return program.modules[index].requests.map([](const ModuleRequest& request) { return request.target; });
    });
    for (auto [from, to] : requiredModules)
        modulesReachedFrom[from.index()].append(to);
    StronglyConnectedComponents circlesOfImports = computeStronglyConnectedComponents(program.modules.size(), [&](unsigned index) -> const Vector<ModuleID>& { return modulesReachedFrom[index]; });
    InitializerScanContext context { program, inputs, runner, variables, circlesOfImports, moduleWithBody };

    InitializerPlan plan { variables.emptiness };
    Vector<uint32_t> bodies = findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        if (!mayListValueWith(program, FunctionID(index), { Opcode::PutModuleVariable }))
            return false;
        const Function& function = program.functions[index];
        if (!function.hasBody() || function.isUnsplitCoroutine() || !flow.runsAtMostOnce(program, FunctionID(index)) || !build.isEnabled(PassID::DeferInitializers, FunctionID(index)))
            return false;
        return listsAccessOf(program, FunctionID(index), Opcode::PutModuleVariable, [&](VariableID variable) { return variables.mayStayEmpty(variable); });
    });
    {
        // What is made to look at a callee is thrown away.
        auto phases = makeUnique<InterningPhases>(editor, bodies.size());
        {
            ProgramCaches caches(program, build);
            Vector<uint32_t> longestFirst(bodies.size(), [](size_t index) { return static_cast<uint32_t>(index); });
            AOT::sort(longestFirst, std::ranges::greater { }, [&](uint32_t index) { return std::pair { program.functions[bodies[index]].values.size(), index }; });
            plan.units = collectInParallel<InitializerUnit>(loops, bodies.size(), longestFirst.span(), [&](unsigned index, Vector<InitializerUnit>& found) {
                TaskArenas arenas(*phases, index);
                findInitializerUnits(context, FunctionID(bodies[index]), arenas, found);
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
    MapByIndex<VariableID> addedInBody;
    for (std::span<InitializerUnit> units = plan.units.mutableSpan(); !units.empty();) {
        const Function& body = program.at(units.front().body);
        size_t numberOfUnits = std::ranges::find_if(units, [&](const InitializerUnit& unit) { return unit.body != units.front().body; }) - units.begin();
        std::span<InitializerUnit> unitsOfBody = units.first(numberOfUnits);
        units = units.subspan(numberOfUnits);
        addedInBody.clear();
        std::optional<PredictedValues> predicted;
        for (InitializerUnit& unit : unitsOfBody) {
            for (ValueOfVariable& added : unit.addedVariables) {
                if (!predicted)
                    predicted.emplace(body, CFG(body));
                TypeID type = body.at(added.value).type;
                std::optional<ProvenInEveryVersion> proven = Proven::byRules(*predicted, added.value, type).inEveryVersion();
                Variable variable;
                variable.module = body.module;
                variable.declared = proven ? DeclaredType(*proven) : DeclaredType(ExpectedType(type));
                variable.initialization = VariableInitialization::Uninitialized;
                variable.flow.writers = writerStateOf(writersOfVariableThatPassFillsOnce()).state;
                variable.flow.stores = storeCountOf(writersOfVariableThatPassFillsOnce()).state;
                variable.flow.initialization = InitializationState::StoredBeforeEveryRead;
                addedInBody.add(added.variable.index(), editor.addVariable(WTF::move(variable)));
            }
        }
        if (addedInBody.isEmpty())
            continue;
        auto rename = [&](VariableID& variable) {
            if (variable.index() >= numberOfVariablesBefore)
                variable = addedInBody.get(variable.index());
        };
        for (InitializerUnit& unit : unitsOfBody) {
            std::ranges::for_each(unit.variables, rename);
            std::ranges::for_each(unit.addedVariables, rename, &ValueOfVariable::variable);
            std::ranges::for_each(unit.valuesOfVariables, rename, &ValueOfVariable::variable);
        }
    }

    Vector<FunctionID> created;
    Vector<uint32_t> unitOfCreated;
    size_t numberOfVariablesFilled = 0;
    plan.unitOf.fill(UINT32_MAX, program.variables.size());
    for (uint32_t index = 0; index < plan.units.size(); ++index) {
        InitializerUnit& unit = plan.units[index];
        if (unit.staysInBody)
            continue;
        unit.type = TypeTable::undefined;
        if (unit.variables.size() == 1)
            unit.type = editor.types().intern(program.at(program.provenTypeOf(program.at(unit.variables.first()).declared)).withoutPrimitives(Primitive::Absent));
        const Function& body = program.at(unit.body);
        Function head;
        head.name = program.strings.emptyString();
        head.module = body.module;
        head.position = unit.items.first().originIn(body).position;
        head.kind = FunctionKind::Plain;
        head.role = FunctionRole::OutlinedBody;
        head.flags = body.flags & OptionSet<FunctionFlags> { FunctionFlags::IsSloppy, FunctionFlags::IsChecked };
        head.thisType = DeclaredType(Proven::byPosition(TypeTable::anything));
        head.results.append(DeclaredType(Proven::byPosition(unit.type)));
        head.capabilities = initialCapabilitiesOf(head);
        unit.function = editor.addFunctionLike(unit.body, WTF::move(head));
        created.append(unit.function);
        unitOfCreated.append(index);
        // Flow saw the body take these as values. The function reads them.
        for (const ValueOfVariable& taken : unit.valuesOfVariables)
            editor.at(taken.variable).flow.reads = ReadState::Unknown;
        for (VariableID id : unit.variables) {
            plan.unitOf[id.index()] = index;
            ++numberOfVariablesFilled;
            Variable& variable = editor.at(id);
            Emptiness emptiness = plan.emptinessOf(id);
            variable.declared.changeType(Proven::byPosition(editor.types().intern(program.at(variable.declared.written()).withPrimitives(emptiness == Emptiness::UndefinedFromDeclaration ? Primitive::Absent | Primitive::Undefined : Primitive::Absent))));
            if (emptiness == Emptiness::AbsentFromBirth)
                variable.initialization = VariableInitialization::Uninitialized;
            else
                variable.flow.stores = StoreCount::Unknown;
            variable.flow.initialization = InitializationState::Unknown;
            variable.flow.isUnchangedDuringActivation = false;
        }
    }
    build.statistics().count(StatisticID::InitializersDeferred, created.size());
    build.statistics().count(StatisticID::VariablesFilledByInitializers, numberOfVariablesFilled);
    build.statistics().count(StatisticID::VariablesAddedForInitializers, program.variables.size() - numberOfVariablesBefore);

    editFunctionsInParallel(editor, build, created.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        IRBuilder builder(AnalysisContext { build, program, &flow, nullptr, id }, function, arenas.types);
        buildInitializerUnit(plan.units[unitOfCreated[index]], program.at(plan.units[unitOfCreated[index]].body), builder);
    });
    times.didFinish("createFunctions"_s);

    Vector<FunctionID> functions = findFunctionsToEdit(program, loops, plan.units, true, [&](VariableID variable) { return !!plan.find(variable); });
    editFunctionsInParallel(editor, build, functions.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        IRBuilder builder(AnalysisContext { build, program, &flow, nullptr, id }, function, arenas.types);
        removeUnits(plan, thoseIn(plan.units, id), builder);
        fillVariablesAtReads(plan, id, builder);
    });
    times.didFinish("fillVariablesAtReads"_s);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
