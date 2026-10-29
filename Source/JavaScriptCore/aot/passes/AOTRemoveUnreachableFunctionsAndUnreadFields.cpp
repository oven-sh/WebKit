/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRemoveUnreachableFunctions.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTCallGraph.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTHeldOperands.h"
#include "AOTIRBuilder.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTObservableOperations.h"
#include "AOTPassRunner.h"
#include "AOTPhaseTimes.h"
#include "AOTProgramCaches.h"
#include "AOTRemoveUnreadExports.h"
#include "AOTRemoveUnreadFields.h"
#include "AOTSchemaWalkers.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTUseLists.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <wtf/Atomics.h>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/UnionFind.h>

namespace JSC { namespace AOT { namespace Typed {

BitVector findReachableFunctions(const Program& program, const CallGraph& callGraph, const BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    unsigned count = program.functions.size();

    // The entrypoints that have a body. They come in their own order, which the counting sort by target keeps.
    using EntrypointLink = std::pair<FunctionID, FunctionID>;
    Vector<EntrypointLink> links = collectInParallel<EntrypointLink>(loops, count, [&](unsigned index, Vector<EntrypointLink>& found) {
        const Function& function = program.functions[index];
        const auto* record = function.creator.tryAs<CreatorRecord::Entrypoint>();
        if (record && function.hasBody() && record->target.index() < count)
            found.append({ record->target, FunctionID(index) });
    });
    Vector<uint32_t> offsets(FillWith { }, count + 1, 0u);
    for (const EntrypointLink& link : links)
        ++offsets[link.first.index() + 1];
    std::partial_sum(offsets.begin(), offsets.end(), offsets.begin());
    Vector<uint32_t> cursors(offsets.subspan(0, count));
    Vector<FunctionID> entrypoints(links.size());
    for (const EntrypointLink& link : links)
        entrypoints[cursors[link.first.index()]++] = link.second;
    // Every reference to a closure asks for those of its function.
    auto entrypointsOf = [&](FunctionID target) -> std::span<const FunctionID> {
        size_t index = target.index();
        if (index + 1 >= offsets.size())
            return { };
        return entrypoints.subspan(offsets[index], offsets[index + 1] - offsets[index]);
    };

    // A byte a function, written by all threads. Of all threads that reach a function in one round, one lists it.
    std::unique_ptr<std::atomic<uint8_t>[]> isReached(new std::atomic<uint8_t>[count]());
    auto reach = [&](FunctionID function, Vector<FunctionID>& reachedBodies) {
        if (function.index() >= count)
            return;
        std::atomic<uint8_t>& flag = isReached[function.index()];
        if (!flag.load(std::memory_order_relaxed) && !flag.exchange(1, std::memory_order_relaxed) && hasBody(program, function))
            reachedBodies.append(function);
    };

    Vector<FunctionID> frontier;
    // The tables take the body of a module or of a function that is registered for the host as the sign that it can run.
    BitVector isCalledByHost = functionsCalledByHost(program, loops);
    for (FunctionID root : callGraph.roots()) {
        if (isCalledByHost.get(root.index()) || entrypointsOf(root).empty())
            reach(root, frontier);
    }
    for (unsigned index = 0; index < count; ++index) {
        if (!build.isEnabled(PassID::RemoveUnreachableFunctions, FunctionID(index)))
            reach(FunctionID(index), frontier);
    }

    while (!frontier.isEmpty()) {
        frontier = collectInParallel<FunctionID>(loops, frontier.size(), [&](unsigned position, Vector<FunctionID>& reachedBodies) {
            // An operation that may enter a function has the effects of that body, also where it enters through an entrypoint: a function without a body counts as unknown code.
            for (FunctionID callee : callGraph.calleesOf(frontier[position]))
                reach(callee, reachedBodies);
            forEachFunctionReference(program, frontier[position], [&](FunctionID target, FunctionReferenceKind kind) {
                switch (kind) {
                case FunctionReferenceKind::Call:
                case FunctionReferenceKind::Dispatch:
                    reach(target, reachedBodies);
                    return;
                case FunctionReferenceKind::Closure:
                case FunctionReferenceKind::Constructor:
                    break;
                }
                // Whoever has the closure of a function enters one of its entrypoints, so a closure alone does not keep a body that the entrypoints hold a copy of.
                if (entrypointsOf(target).empty())
                    reach(target, reachedBodies);
                for (FunctionID entrypoint : entrypointsOf(target))
                    reach(entrypoint, reachedBodies);
                // Asked about its instances, a bound function that captures no closure of its target has the run time make one.
                const auto* bound = program.at(target).creator.tryAs<CreatorRecord::BoundFunction>();
                if (!bound || bound->capturesTargetClosure)
                    return;
                for (FunctionID entrypoint : entrypointsOf(bound->target))
                    reach(entrypoint, reachedBodies);
            });
        });
    }

    BitVector result(count);
    for (unsigned index = 0; index < count; ++index) {
        if (isReached[index].load(std::memory_order_relaxed))
            result.quickSet(index);
    }
    return result;
}

void removeUnreachableFunctions(ProgramEditor& editor, const CallGraph& callGraph, BuildContext& build)
{
    if (callGraph.roots().empty())
        return;

    PhaseTimes times(build, PassID::RemoveUnreachableFunctions);
    const Program& program = editor.program();
    BitVector isReachable = findReachableFunctions(program, callGraph, build);
    times.didFinish("findReachableFunctions"_s);
    const ParallelLoops& loops = build.parallelLoops();
    Vector<uint32_t> unreachableFunctions = findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        return !isReachable.get(index) && hasBody(program, FunctionID(index));
    });
    forEachIndexInParallel(loops, unreachableFunctions.size(), [&](unsigned position) {
        editor.removeBody(FunctionID(unreachableFunctions[position]));
    });
    build.statistics().count(StatisticID::UnreachableFunctionsRemoved, unreachableFunctions.size());
    times.didFinish("remove bodies"_s);
}

BitVector findBodiesWithoutCode(const Program& program, const BuildContext& build, std::span<const Vector<FunctionID>> calledFunctions)
{
    const ParallelLoops& loops = build.parallelLoops();
    unsigned count = program.functions.size();
    BitVector isCalledByHost = functionsCalledByHost(program, loops);
    BitVector hasNoCode(count);
    for (uint32_t index : findIndicesInParallel(loops, count, [&](unsigned index) {
        RecordFacts facts = recordFactsOf(program, FunctionID(index));
        // Every table names entrypoints, for closures and for the host alike.
        return facts.hasBody && isEnteredOnlyByCallsOf(facts.role) && !isCalledByHost.get(index) && build.isEnabled(PassID::RemoveUnreachableFunctions, FunctionID(index));
    }))
        hasNoCode.quickSet(index);
    if (hasNoCode.isEmpty())
        return hasNoCode;

    Vector<FunctionID> frontier = collectInParallel<FunctionID>(loops, count, [&](unsigned index, Vector<FunctionID>& found) {
        if (!hasNoCode.quickGet(index) && hasBody(program, FunctionID(index)))
            found.append(FunctionID(index));
    });
    while (!frontier.isEmpty()) {
        Vector<FunctionID> called = collectInParallel<FunctionID>(loops, frontier.size(), [&](unsigned position, Vector<FunctionID>& found) {
            auto note = [&](FunctionID target) {
                if (hasNoCode.get(target.index()))
                    found.append(target);
            };
            if (calledFunctions.empty())
                forEachCalledFunction(program, frontier[position], note);
            else
                std::ranges::for_each(calledFunctions[frontier[position].index()], note);
        });
        frontier.shrink(0);
        for (FunctionID function : called) {
            if (hasNoCode.quickClear(function.index()))
                frontier.append(function);
        }
    }
    return hasNoCode;
}

static constexpr OperandUses keyedStoreUses { OperandUse::StoresByKey, OperandUse::AssignsFrom, OperandUse::DefinesName, OperandUse::TakesDescriptor };

static bool isUnread(const BuildContext& build, const Layout& layout, const Field& field)
{
    constexpr OptionSet<FieldFlags> excludedFlags { FieldFlags::IsInherited, FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsHidden };
    if (field.flags.containsAny(excludedFlags) || field.form.contains(FieldForm::IsAccessor))
        return false;
    if (!layout.isEnvironment() && !field.key)
        return false;
    if (field.flow.location.writers != WriterState::AllKnown)
        return false;
    // It has no storage as it is, whatever flow says of the names of its stores. Only a candidate gets its storage back.
    if (hasNoAccesses(field.flow.location))
        return true;
    // A store that the scan cannot find would define the key at run time on an object without the field.
    if (field.flow.location.storeNaming != StoreNaming::EveryStoreNamesLocation)
        return false;
    switch (field.flow.location.reads) {
    case ReadState::OnlyCalled:
    case ReadState::NeverRead:
        return true;
    case ReadState::Unknown:
        return build.isSabotaged(Sabotage::RemovedFieldsHaveUnknownReaders);
    case ReadState::OnlyAccessed:
    case ReadState::OnlyWhereDefined:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// An object lives as long as a place that holds it, whether or not the place is read.
// A built-in function and the symbol of a key live as long as the program. A closure that is only called, that nothing tells from another and that captures nothing holds nothing, and nobody sees it go.
// Nobody sees a record or an instance go either where only code of the program has such an object in hand, nothing holds one weakly, and all that one holds is of the same kind.
class UnnoticedObjects {
public:
    UnnoticedObjects(const Program& program, const FlowResults& flow)
        : m_program(program)
        , m_flow(flow)
        , m_layouts(program.layouts.size())
    {
        Vector<Vector<LayoutID, 1>> holders(program.layouts.size());
        Vector<LayoutID> noticed;
        for (uint32_t index = 0; index < program.layouts.size(); ++index) {
            auto addHeld = [&](LayoutID held) {
                holders[held.index()].append(LayoutID(index));
            };
            if (isSeenByProgramAlone(LayoutID(index)) && listLayoutsHeld(LayoutID(index), addHeld))
                m_layouts.quickSet(index);
            else
                noticed.append(LayoutID(index));
        }
        while (!noticed.isEmpty()) {
            for (LayoutID holder : holders[noticed.takeLast().index()]) {
                if (m_layouts.quickClear(holder.index()))
                    noticed.append(holder);
            }
        }
    }

    bool mayHoldObject(const Type& type) const
    {
        return mayHoldObjectWithoutLayout(type) || std::ranges::any_of(membersOf(m_program, type), [&](LayoutID id) {
            return !m_layouts.quickGet(id.index());
        });
    }

private:
    bool mayHoldObjectWithoutLayout(const Type& type) const
    {
        if (type.isOpaque(OpaqueKind::Guard))
            return false;
        if (type.anyObject || type.anyFunction || !type.classes.isEmpty() || !type.isJavaScriptValue() || type.primitives & Primitive::Symbol)
            return true;
        return std::ranges::any_of(type.functions, [&](FunctionID function) {
            return !m_program.at(function).captures.isEmpty() || !m_flow.closuresGoUnnoticed(function);
        });
    }

    // The prototype of a class that is evaluated once lives as long as the program, with all that it holds.
    bool isSeenByProgramAlone(LayoutID id) const
    {
        constexpr OptionSet<ObjectMutation> mutationsThatShowOrFill { ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript, ObjectMutation::RedefineProperty, ObjectMutation::AddProperty, ObjectMutation::ChangePrototype };
        constexpr OptionSet<LayoutFlags> flagsOfOtherObjects { LayoutFlags::HasMembersWithoutLayout, LayoutFlags::IsTypeParameter, LayoutFlags::IsOfCreatedObject };
        const Layout& layout = m_program.at(id);
        bool hasLastingPrototype = layout.kind == LayoutKind::Record || (layout.kind == LayoutKind::Instance && isEvaluatedOnce(m_program, layout));
        return hasLastingPrototype && !layout.flags.containsAny(flagsOfOtherObjects) && m_flow.objectsAreNeverHeldWeakly(id) && layout.flow.impossibleMutations.containsAll(mutationsThatShowOrFill);
    }

    // Names the layouts of what an object of the layout may hold, and the classes above and below its own. Says no where it may hold anything else.
    bool listLayoutsHeld(LayoutID id, const Invocable<void(LayoutID)> auto& functor) const
    {
        const Layout& layout = m_program.at(id);
        if (layout.flow.baseState == BaseState::Layout)
            functor(layout.flow.base);
        else if (layout.flow.baseState != BaseState::None)
            return false;
        std::ranges::for_each(m_program.layoutsBelow(id), functor);
        return std::ranges::none_of(layout.fields, [&](const Field& field) {
            const Type& type = m_program.at(slotTypeOf(m_program, field));
            std::ranges::for_each(membersOf(m_program, type), functor);
            return field.form.contains(FieldForm::IsAccessor) || mayHoldObjectWithoutLayout(type);
        });
    }

    const Program& m_program;
    const FlowResults& m_flow;
    BitVector m_layouts;
};

// Remembers which functions return only new objects that hold no object. An answer depends on the function and on how many calls deep the search may go, so every thread finds the same one.
class FreshDataFunctions {
public:
    AOT_CLASS_THRESHOLD(unsigned, maxCallDepth, 2);

    explicit FreshDataFunctions(size_t numberOfFunctions)
    {
        for (auto& answers : m_answers)
            answers = Vector<uint8_t>(FillWith { }, numberOfFunctions, static_cast<uint8_t>(0));
    }

    std::optional<bool> find(FunctionID function, unsigned callDepth) const
    {
        uint8_t answer = WTF::atomicLoad(&m_answers[callDepth][function.index()], std::memory_order_relaxed);
        return answer ? std::optional(answer == isFresh) : std::nullopt;
    }

    void set(FunctionID function, unsigned callDepth, bool answer)
    {
        WTF::atomicStore(&m_answers[callDepth][function.index()], answer ? isFresh : isNotFresh, std::memory_order_relaxed);
    }

private:
    static constexpr uint8_t isFresh = 1;
    static constexpr uint8_t isNotFresh = 2;

    mutable std::array<Vector<uint8_t>, maxCallDepth> m_answers;
};

// Whether a store in one function keeps an object alive that would go without it.
class StoresOfObjects {
public:
    StoresOfObjects(const BuildContext& build, const Program& program, const FlowResults& flow, const UnnoticedObjects& unnoticedObjects, const EffectSummaries* effects, FunctionID id, FreshDataFunctions* freshDataFunctions = nullptr, unsigned callDepth = FreshDataFunctions::maxCallDepth)
        : m_build(build)
        , m_program(program)
        , m_flow(flow)
        , m_unnoticedObjects(unnoticedObjects)
        , m_effects(effects)
        , m_id(id)
        , m_function(program.at(id))
        , m_freshDataFunctions(freshDataFunctions)
        , m_callDepth(callDepth)
    {
    }

    bool keepsObjectAlive(ValueID stored, ValueID holder, unsigned depth = 3)
    {
        if (stored == holder || !m_unnoticedObjects.mayHoldObject(m_program.typeOf(m_function, stored)))
            return false;
        if (!depth)
            return true;
        const Value& definition = m_function.at(stored);
        // Flow knows more of a closure than that it is new.
        if (definition.opcode == Opcode::NewClosure)
            return closureKeepsObjectAlive(stored, holder, depth);
        if (isCastWithoutTest(definition.opcode))
            return useLists().useCount(stored) != 1 || keepsObjectAlive(definition.operand(0), holder, depth);
        if (effectRuleOf(definition.opcode) == EffectRule::Call)
            return useLists().useCount(stored) != 1 || !answersWithWhatNobodyHolds(definition);
        return !isSeenOnlyByItsMaker(stored, depth);
    }

private:
    const UseLists& useLists()
    {
        if (!m_useLists) {
            m_cfg.emplace(m_function);
            m_useLists.emplace(m_function, *m_cfg);
        }
        return *m_useLists;
    }

    bool closureKeepsObjectAlive(ValueID stored, ValueID holder, unsigned depth)
    {
        const Value& closure = m_function.at(stored);
        if (closure.isMadeOnDemand() || (!m_flow.closuresGoUnnoticed(closure.function()) && !isSeenOnlyByStore(stored)))
            return true;
        return std::ranges::any_of(closure.capturesOperands(), [&](ValueID capture) {
            return keepsObjectAlive(capture, holder, depth - 1) && !isHeldBy(holder, capture, stored);
        });
    }

    // A suspension that lists the closure holds it and does not look at it.
    bool isSeenOnlyByStore(ValueID closure)
    {
        return std::ranges::count_if(useLists().usesOf(closure), [&](const Use& use) {
            return use.kind != Use::Kind::Operand || !isHeldOperand(m_function.at(use.user), use.operandIndex);
        }) == 1;
    }

    // The annotation says that the call may go when nobody uses its answer.
    bool answersWithWhatNobodyHolds(const Value& call)
    {
        if (Value::opcodesWithIsAnnotatedPure.contains(call.opcode))
            return call.isAnnotatedPure();
        return Value::opcodesWithIsConstruction.contains(call.opcode) && !call.isConstruction() && returnsOnlyFreshData(call.function());
    }

    // The operation made the object, nothing but the function that holds the operation has had it in hand, and all it did was to fill it with what holds no object. One use is the store that is asked about, or what hands the object on to that store.
    bool isSeenOnlyByItsMaker(ValueID object, unsigned depth)
    {
        const Value& allocation = m_function.at(object);
        if (!m_effects)
            return false;
        bool isDeclaredAllocation = interpretedOf(allocation.opcode) == Interpretation::Allocation && effectRuleOf(allocation.opcode) == EffectRule::Declared;
        if (!isDeclaredAllocation && freshnessOf(EffectContext { m_program, *m_effects }, m_function, allocation) != ResultFreshness::Fresh)
            return false;
        bool hasLayout = Value::opcodesWithLayout.contains(allocation.opcode);
        if (hasLayout) {
            LayoutKind kind = m_program.at(allocation.layout()).kind;
            if (kind != LayoutKind::Record && kind != LayoutKind::Container)
                return false;
        }
        auto holdsNoObject = [&](ValueID value) {
            return !keepsObjectAlive(value, object, depth - 1);
        };
        bool isBornWithoutObjects = true;
        allocation.forEachOperand([&](ValueID operand, uint32_t) {
            isBornWithoutObjects &= holdsNoObject(operand);
        });
        if (!isBornWithoutObjects)
            return false;
        constexpr OperandUses fillingUses { OperandUse::Inspects, OperandUse::StoresByKey };
        unsigned numberOfOwners = 0;
        for (const Use& use : useLists().usesOf(object)) {
            if (use.kind != Use::Kind::Operand)
                return false;
            const Value& user = m_function.at(use.user);
            OperandUses uses = summarizeOperand(EffectContext { m_program, *m_effects }, m_function, user, use.operandIndex).uses;
            // Defining a property of a new ordinary object runs no code of the program, and neither does a store to a data property that the object has had since its creation. An assignment has a mode, a definition has none.
            if (hasLayout && receiverOf(user.opcode) == use.operandIndex && interpretedOf(user.opcode) == Interpretation::Store) {
                if (!Value::opcodesWithMode.contains(user.opcode))
                    uses.remove(OperandUse::PassesToUserCode);
                else if (Value::opcodesWithKey.contains(user.opcode) && Value::opcodesWithFields.contains(allocation.opcode)) {
                    auto field = findField(m_program, allocation.layout(), user.key(), BaseChain::Proven);
                    if (field && field->layout == allocation.layout() && std::ranges::contains(allocation.fields(), field->field) && !fieldAt(m_program, *field).form.contains(FieldForm::IsAccessor))
                        uses.remove(OperandUse::PassesToUserCode);
                }
            }
            if (!fillingUses.containsAll(uses)) {
                if (++numberOfOwners > 1)
                    return false;
                continue;
            }
            bool fillsWithoutObjects = true;
            user.forEachOperand([&](ValueID operand, uint32_t) {
                fillsWithoutObjects &= holdsNoObject(operand);
            });
            if (!fillsWithoutObjects || m_program.typeOf(m_function, use.user).hasCells())
                return false;
        }
        return true;
    }

    // Each object that the function returns is new, only the function has seen it, and what fills it holds no object.
    bool returnsOnlyFreshData(FunctionID callee)
    {
        if (!m_freshDataFunctions || !m_callDepth)
            return false;
        unsigned callDepth = m_callDepth - 1;
        if (std::optional<bool> known = m_freshDataFunctions->find(callee, callDepth))
            return *known;
        const Function& function = m_program.at(callee);
        bool answer = !function.blocks.isEmpty() && function.results.size() == 1 && function.hasOrdinaryFrame() && !function.isUnsplitCoroutine();
        if (answer) {
            StoresOfObjects stores(m_build, m_program, m_flow, m_unnoticedObjects, m_effects, callee, m_freshDataFunctions, callDepth);
            answer = std::ranges::none_of(function.blocks, [&](const Block& block) {
                if (block.terminator.kind == TerminatorKind::Return)
                    return !block.terminator.valueOperand() || stores.keepsObjectAlive(block.terminator.valueOperand(), ValueID());
                return block.terminator.kind == TerminatorKind::ReturnCall && stores.keepsObjectAlive(block.terminator.callOperand(), ValueID());
            });
        }
        m_freshDataFunctions->set(callee, callDepth, answer);
        return answer;
    }

    // A value stored once into its holder's own field lives as long as the holder: an environment wherever the store is, anything else if the holder is an environment whose field is kept and is filled before the holder is passed the closure,
    // or after that in the same block if every operation in between completes: until the store runs, the function holds what it is about to store.
    bool isHeldBy(ValueID holder, ValueID held, ValueID closure)
    {
        if (!holder)
            return false;
        auto keyOf = [](ValueID holder, ValueID held) {
            return static_cast<uint64_t>(holder.index()) << 32 | held.index();
        };
        if (!m_valuesHeld) {
            m_valuesHeld.emplace();
            auto add = [&](ValueID holder, FieldReference reference, ValueID held, ValueID store) {
                const Field& field = fieldAt(m_program, reference);
                if (field.flow.location.stores != StoreCount::AtMostOnce)
                    return;
                const Layout& layout = m_program.at(reference.layout);
                if (m_program.typeOf(m_function, held).environment || (layout.isEnvironment() && !isUnread(m_build, layout, field)))
                    m_valuesHeld->add(keyOf(holder, held), store);
            };
            forEachListedValue(m_program, m_id, { Opcode::NewObject, Opcode::PutEnvironmentField }, [&](BlockID, uint32_t, ValueID valueID, const Value& value) {
                if (value.opcode == Opcode::PutEnvironmentField) {
                    add(value.operand(OperandIndex::PutEnvironmentField::environment), { value.layout(), value.field() }, value.operand(OperandIndex::PutEnvironmentField::value), valueID);
                    return;
                }
                for (size_t listed = 0; listed < value.fields().size(); ++listed)
                    add(valueID, { value.layout(), value.fields()[listed] }, value.variadicOperands()[listed], valueID);
            });
        }
        auto found = m_valuesHeld->find(keyOf(holder, held));
        if (found == m_valuesHeld->end())
            return false;
        if (found->value == holder || m_program.typeOf(m_function, held).environment)
            return true;
        ValueID given = findProvidingOperation(holder, closure);
        ValueLocation store = useLists().locationOf(found->value);
        ValueLocation made = useLists().locationOf(given);
        if (store.placement != ValuePlacement::InBlock || made.placement != ValuePlacement::InBlock || store.block != made.block)
            return false;
        return store.index < made.index || numberOfExitsInFrontOf(found->value) == numberOfExitsInFrontOf(given);
    }

    // Until it is passed to its holder, a closure with no other use is only reachable from the function, which also holds what it is about to store.
    ValueID findProvidingOperation(ValueID holder, ValueID closure)
    {
        std::span<const Use> uses = useLists().usesOf(closure);
        if (uses.size() != 1 || uses[0].kind != Use::Kind::Operand)
            return closure;
        const Value& user = m_function.at(uses[0].user);
        if (user.opcode == Opcode::NewObject)
            return uses[0].user == holder ? holder : closure;
        bool isStoreToHolder = user.opcode == Opcode::PutEnvironmentField && uses[0].operandIndex == OperandIndex::PutEnvironmentField::value && user.operand(OperandIndex::PutEnvironmentField::environment) == holder;
        return isStoreToHolder ? uses[0].user : closure;
    }

    // How many of the operations that stand in front of a value in its block may leave the block.
    uint32_t numberOfExitsInFrontOf(ValueID value)
    {
        if (m_numberOfExitsInFront.isEmpty()) {
            m_numberOfExitsInFront.fill(0, m_function.values.size());
            for (const Block& block : m_function.blocks) {
                uint32_t numberOfExits = 0;
                for (ValueID member : block.values) {
                    m_numberOfExitsInFront[member.index()] = numberOfExits;
                    Opcode opcode = m_function.at(member).opcode;
                    Effects bound = declaredBoundOf(opcode);
                    numberOfExits += !effectFactsOf(opcode).staysWithinDeclaredBound || bound.mayThrowByRule() || !bound.alwaysCompletes();
                }
            }
        }
        return m_numberOfExitsInFront[value.index()];
    }

    const BuildContext& m_build;
    const Program& m_program;
    const FlowResults& m_flow;
    const UnnoticedObjects& m_unnoticedObjects;
    const EffectSummaries* m_effects;
    FunctionID m_id;
    const Function& m_function;
    FreshDataFunctions* m_freshDataFunctions;
    unsigned m_callDepth;
    std::optional<CFG> m_cfg;
    std::optional<UseLists> m_useLists;
    std::optional<HashMap<uint64_t, ValueID, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>> m_valuesHeld;
    Vector<uint32_t> m_numberOfExitsInFront;
};

static bool mayBeReadByUnknownKey(NamespaceUse use)
{
    switch (use) {
    case NamespaceUse::Unknown:
        return true;
    case NamespaceUse::NamedReadsOnly:
    case NamespaceUse::NeverCreated:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr bool isRemovableSlotStore(Opcode opcode)
{
    return accessClassOf(opcode) == AccessClass::Slot && accessOf(opcode) == Access::Store && resultRuleOf(opcode) == ResultRule::None;
}

static_assert(isRemovableSlotStore(Opcode::PutSlot) && isRemovableSlotStore(Opcode::PutSlotOrAdd));

static constexpr bool isSlotRead(Opcode opcode)
{
    return accessClassOf(opcode) == AccessClass::Slot && accessOf(opcode) == Access::Read;
}

static_assert(isSlotRead(Opcode::GetSlot) && isSlotRead(Opcode::GetDeferredClosureSlot));
static_assert(resultRuleOf(Opcode::PutEnvironmentField) == ResultRule::None);

static constexpr bool findsFieldByKey(Opcode opcode)
{
    return Value::opcodesWithReceivers.contains(opcode) && !Value::opcodesWithField.contains(opcode) && !Value::opcodesWithPlacedKeys.contains(opcode);
}

static constexpr bool createsPropertyByName(Opcode opcode)
{
    return accessClassOf(opcode) == AccessClass::ByName && (accessOf(opcode) == Access::Store || accessOf(opcode) == Access::Define);
}

static_assert(createsPropertyByName(Opcode::PropSet) && createsPropertyByName(Opcode::PropDefine));

static BitVector findUsedValues(const Function& function)
{
    BitVector isUsed(function.values.size());
    auto markUsed = [&](ValueID value) {
        isUsed.quickSet(value.index());
    };
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values)
            function.at(id).forEachChild(markUsed);
        block.forEachChildOutsideValues(markUsed);
    }
    return isUsed;
}

// Only GetModuleVariable reads a variable that no module, eval scope or class object shows to anybody else, so a scan of the bodies sees every read of it.
static BitVector findVariablesThatOnlyBodiesRead(const Program& program, const FlowResults& flow)
{
    BitVector variables;
    if (!flow.hasResults())
        return variables;

    BitVector isReadOutsideBodies(program.variables.size());
    auto add = [&](VariableID variable) {
        if (variable)
            isReadOutsideBodies.quickSet(variable.index());
    };
    BitVector isReadByHost(program.modules.size());
    for (const Entry& entry : program.entries) {
        if (!entry.hostReads.isEmpty())
            isReadByHost.quickSet(entry.module.index());
    }
    for (unsigned index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        const ModuleFlow& moduleFlow = flow.module(ModuleID(index));
        bool isEveryExportRead = isReadByHost.quickGet(index) || mayBeReadByUnknownKey(moduleFlow.namespaceUse);
        for (const ModuleExport& entry : module.exports) {
            if (entry.variable && (isEveryExportRead || std::ranges::binary_search(moduleFlow.exportsReadByName, entry.variable)))
                add(entry.variable);
        }
        for (const SyntheticExport& entry : module.syntheticExports)
            add(entry.variable);
        // The run time reads the variable that holds `module.exports` for whoever asks for the value of the module, takes its names from that value, or holds its module object.
        if (module.exportsVariable && program.at(module.exportsVariable).flow.reads == ReadState::Unknown)
            add(module.exportsVariable);
    }
    for (const EvalScope& scope : program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings)
            add(binding.variable);
    }

    variables.ensureSize(program.variables.size());
    for (unsigned index = 0; index < program.variables.size(); ++index) {
        const Variable& variable = program.variables[index];
        if (!variable.owner && variable.flow.writers == WriterState::AllKnown && !isReadOutsideBodies.quickGet(index))
            variables.quickSet(index);
    }
    return variables;
}

void removeUnreadFields(ProgramEditor& editor, const OptimizationInputs& inputs, const PassRunner& runner)
{
    const Program& program = editor.program();
    BuildContext& build = runner.build();
    const ParallelLoops& loops = build.parallelLoops();

    // A candidate is a field, by its place in the ascending list, or a variable, by its number behind the fields.
    Vector<FieldReference> fields;
    BitVector variables;
    bool areReceiverSetsEnabled = true;
    for (uint32_t function = 0; function < program.numberOfFunctionsAsRead() && areReceiverSetsEnabled; ++function)
        areReceiverSetsEnabled = build.isEnabled(FlowResultKind::ReceiverSets, function);
    for (uint32_t layoutIndex = 0; areReceiverSetsEnabled && layoutIndex < program.layouts.size(); ++layoutIndex) {
        const Layout& layout = program.layouts[layoutIndex];
        if (!supportsRemovalOf(layout.kind) || layout.flow.allocation == AllocationState::NeverAllocated)
            continue;
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            if (isUnread(build, layout, layout.fields[field]))
                fields.append(FieldReference { LayoutID(layoutIndex), field });
        }
    }
    variables = findVariablesThatOnlyBodiesRead(program, inputs.flow);
    if (fields.isEmpty() && variables.isEmpty())
        return;

    auto candidateOf = [&](LayoutID layout, uint32_t field) -> std::optional<uint32_t> {
        FieldReference declaration = declarationOf(program, { layout, field });
        auto found = std::ranges::lower_bound(fields, declaration);
        if (found == fields.end() || *found != declaration)
            return std::nullopt;
        return static_cast<uint32_t>(found - fields.begin());
    };
    auto candidateOfVariable = [&](size_t index) {
        return static_cast<uint32_t>(fields.size() + index);
    };
    Vector<uint8_t> liveCandidates(FillWith { }, fields.size() + variables.size(), static_cast<uint8_t>(0));
    auto isLive = [&](uint32_t candidate) -> bool {
        return WTF::atomicLoad(&liveCandidates[candidate], std::memory_order_relaxed);
    };
    auto markLive = [&](uint32_t candidate) {
        if (!isLive(candidate))
            WTF::atomicStore(&liveCandidates[candidate], static_cast<uint8_t>(1), std::memory_order_relaxed);
    };
    auto markFieldsLive = [&](LayoutID id) {
        size_t candidate = std::ranges::lower_bound(fields, id, { }, &FieldReference::layout) - fields.begin();
        for (; candidate < fields.size() && fields[candidate].layout == id; ++candidate)
            markLive(static_cast<uint32_t>(candidate));

        const Layout& layout = program.at(id);
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            if (!layout.fields[field].flags.contains(FieldFlags::IsInherited))
                continue;
            if (auto inherited = candidateOf(id, field))
                markLive(*inherited);
        }
    };

    OpcodeSet opcodesStoringThroughOperands;
    OpcodeSet scannedOpcodes = Value::opcodesWithLayout | Value::opcodesWithVariable;
    for (Opcode opcode : allMembersOf<Opcode>) {
        bool mayStore = callsBuiltinOf(effectRuleOf(opcode));
        for (uint32_t operandIndex = 0, end = numberOfNamedOperandsOf(opcode) + hasVariadicOperand(opcode); operandIndex < end; ++operandIndex)
            mayStore |= operandUsesOf(opcode, operandIndex).containsAny(keyedStoreUses);
        if (mayStore)
            opcodesStoringThroughOperands.add(opcode);
        if (mayStore || findsFieldByKey(opcode))
            scannedOpcodes.add(opcode);
    }

    // The keys under which a lookup finds a candidate, or a field that was inherited from one.
    BitVector keysOfCandidates;
    Vector<PropertyKeyID> keysFound = collectInParallel<PropertyKeyID>(loops, program.layouts.size(), [&](unsigned index, Vector<PropertyKeyID>& keys) {
        const auto& fieldsOfLayout = program.layouts[index].fields;
        for (uint32_t field = 0; field < fieldsOfLayout.size(); ++field) {
            if (fieldsOfLayout[field].key && candidateOf(LayoutID(index), field))
                keys.append(fieldsOfLayout[field].key);
        }
    });
    for (PropertyKeyID key : keysFound)
        keysOfCandidates.set(key.index());
    Vector<std::pair<PropertyKeyID, uint32_t>> candidatesByKey;
    for (uint32_t candidate = 0; candidate < fields.size(); ++candidate) {
        if (PropertyKeyID key = fieldAt(program, fields[candidate]).key)
            candidatesByKey.append({ key, candidate });
    }
    AOT::sort(candidatesByKey);

    for (const EvalScope& scope : program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings) {
            if (!binding.layout || !binding.field)
                continue;
            if (auto candidate = candidateOf(binding.layout, *binding.field))
                markLive(*candidate);
        }
    }
    BitVector isLayoutOfData(program.layouts.size());
    for (const Data& data : program.data) {
        forEachLayout(data, [&](LayoutID layout) {
            if (!isLayoutOfData.quickSet(layout.index()))
                markFieldsLive(layout);
        });
    }
    for (uint32_t layoutIndex = 0; layoutIndex < program.layouts.size(); ++layoutIndex) {
        if (program.layouts[layoutIndex].flow.observedUses.contains(ObservedUse::ReachesTypedParse))
            markFieldsLive(LayoutID(layoutIndex));
    }

    struct RemovableAccess {
        BlockID block;
        ValueID access;
        Vector<uint32_t, 1> candidates;
    };
    using FunctionAccesses = std::pair<FunctionID, Vector<RemovableAccess>>;
    FreshDataFunctions freshDataFunctions(program.functions.size());
    UnnoticedObjects unnoticedObjects(program, inputs.flow);
    std::optional<ProgramCaches> caches(std::in_place, program, build);
    Vector<FunctionAccesses> functions = collectInParallel<FunctionAccesses>(loops, program.functions.size(), [&](unsigned index, Vector<FunctionAccesses>& collected) {
        FunctionID id(index);
        const Function& function = program.at(id);
        if (!mayListValueWith(program, id, scannedOpcodes) || !function.hasBody())
            return;
        bool canChangeFunction = build.isEnabled(PassID::RemoveUnreadFields, id);
        std::optional<BitVector> usedValues;
        StoresOfObjects stores(build, program, inputs.flow, unnoticedObjects, &inputs.effects, id, &freshDataFunctions);
        Vector<RemovableAccess> accesses;
        forEachListedValue(program, id, scannedOpcodes, [&](BlockID block, uint32_t, ValueID valueID, const Value& value) {
            Opcode opcode = value.opcode;
            auto addAccess = [&](Vector<uint32_t, 1>&& candidates, bool isRemovable) {
                if (isRemovable && canChangeFunction)
                    accesses.append(RemovableAccess { block, valueID, WTF::move(candidates) });
                else
                    std::ranges::for_each(candidates, markLive);
            };

            // The read of a deferred closure fills the slot, so dead code elimination leaves it where nothing uses its value.
            auto isUnusedSlotRead = [&] {
                if (!isSlotRead(opcode))
                    return false;
                if (!usedValues)
                    usedValues = findUsedValues(function);
                return !usedValues->quickGet(valueID.index());
            };
            auto scanAccessByKey = [&] {
                std::span<const LayoutID> members;
                bool reachesOnlyCandidates = false;
                bool mayReachAnyLayout = false;
                if (LayoutSetID stamp = value.receivers()) {
                    auto all = program.at(stamp).all();
                    members = all ? all->value() : program.at(stamp).seen().predicted();
                    reachesOnlyCandidates = !!all;
                } else if (std::optional<uint8_t> receiver = receiverOf(opcode)) {
                    const Type& type = program.typeOf(function, value.operand(*receiver));
                    members = membersOf(program, type);
                    mayReachAnyLayout = type.anyObject;
                }

                PropertyKeyID key = Value::opcodesWithKey.contains(opcode) ? value.key() : PropertyKeyID();
                if (!key) {
                    std::ranges::for_each(members, markFieldsLive);
                    return;
                }
                // Most keys are the key of no candidate, and then no member has a candidate under the key.
                if (!keysOfCandidates.get(key.index()))
                    return;
                // Nothing says which objects arrive here. On one without the field the store would create a property.
                if (mayReachAnyLayout && createsPropertyByName(opcode)) {
                    for (auto [keyOfCandidate, candidate] : std::ranges::equal_range(candidatesByKey, key, { }, &std::pair<PropertyKeyID, uint32_t>::first))
                        markLive(candidate);
                    return;
                }

                Vector<uint32_t, 1> found;
                for (LayoutID member : members) {
                    std::optional<uint32_t> candidate;
                    if (auto field = findField(program, member, key, BaseChain::Proven))
                        candidate = candidateOf(field->layout, field->field);
                    if (candidate)
                        found.append(*candidate);
                    else
                        reachesOnlyCandidates = false;
                }
                if (found.isEmpty())
                    return;
                AOT::sort(found);
                removeRepeatedElements(found);
                bool isRemovableStore = isRemovableSlotStore(opcode) && !stores.keepsObjectAlive(value.operand(*storedOf(opcode)), value.operand(*receiverOf(opcode)));
                addAccess(WTF::move(found), reachesOnlyCandidates && (isRemovableStore || isUnusedSlotRead()));
            };

            if (LayoutID layout = Value::opcodesWithLayout.contains(opcode) ? value.layout() : LayoutID()) {
                if (resultRuleOf(opcode) == ResultRule::Allocation && opcode != Opcode::NewObject)
                    markFieldsLive(layout);
                auto addNamedField = [&](uint32_t field, ValueID stored, ValueID holder) {
                    if (auto candidate = candidateOf(layout, field))
                        addAccess({ *candidate }, stored && !stores.keepsObjectAlive(stored, holder));
                };
                for (const PayloadFieldInfo& field : payloadFieldsOf(opcode)) {
                    if (field.kind == PayloadKind::Field) {
                        auto namedField = decodePayloadWord<std::optional<uint32_t>>(value.payloadWordAt(field.firstWord));
                        if (namedField && opcode == Opcode::PutEnvironmentField)
                            addNamedField(*namedField, value.operand(OperandIndex::PutEnvironmentField::value), value.operand(OperandIndex::PutEnvironmentField::environment));
                        else if (namedField)
                            addNamedField(*namedField, ValueID(), ValueID());
                        continue;
                    }
                    if (field.kind != PayloadKind::List || field.elementKind != PayloadKind::Field)
                        continue;
                    auto namedFields = value.payloadListFrom(field.firstWord);
                    for (size_t listed = 0; listed < namedFields.size(); ++listed)
                        addNamedField(namedFields[listed], opcode == Opcode::NewObject ? value.variadicOperands()[listed] : ValueID(), valueID);
                }
            }
            if (Value::opcodesWithVariable.contains(opcode) && variables.get(value.variable().index()))
                addAccess({ candidateOfVariable(value.variable().index()) }, opcode == Opcode::PutModuleVariable && !stores.keepsObjectAlive(value.operand(OperandIndex::PutModuleVariable::value), ValueID()));
            if (findsFieldByKey(opcode))
                scanAccessByKey();
            if (opcodesStoringThroughOperands.contains(opcode)) {
                value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                    std::span<const LayoutID> members = membersOf(program, program.typeOf(function, operand));
                    if (!members.empty() && summarizeOperand(EffectContext { program, inputs.effects }, function, value, operandIndex).uses.containsAny(keyedStoreUses))
                        std::ranges::for_each(members, markFieldsLive);
                });
            }
        });
        if (!accesses.isEmpty())
            collected.append({ id, WTF::move(accesses) });
    });
    caches.reset();

    // An access goes only if each candidate that it may reach goes.
    struct CandidateSet : WTF::UnionFind<CandidateSet> { };
    Vector<CandidateSet> sets(liveCandidates.size());
    bool hasSharedAccess = false;
    for (const auto& [id, accesses] : functions) {
        for (const RemovableAccess& site : accesses) {
            for (uint32_t candidate : site.candidates.span().subspan(1)) {
                sets[candidate].unify(&sets[site.candidates[0]]);
                hasSharedAccess = true;
            }
        }
    }
    if (hasSharedAccess) {
        auto rootOf = [&](uint32_t candidate) {
            return static_cast<size_t>(sets[candidate].find() - sets.span().data());
        };
        BitVector hasLiveMember(liveCandidates.size());
        for (uint32_t candidate = 0; candidate < liveCandidates.size(); ++candidate) {
            if (isLive(candidate))
                hasLiveMember.quickSet(rootOf(candidate));
        }
        for (uint32_t candidate = 0; candidate < liveCandidates.size(); ++candidate) {
            if (hasLiveMember.quickGet(rootOf(candidate)))
                markLive(candidate);
        }
    }

    uint64_t numberOfRemovedFields = 0;
    for (uint32_t candidate = 0; candidate < fields.size(); ++candidate) {
        bool hadNoAccesses = hasNoAccesses(fieldAt(program, fields[candidate]).flow.location);
        if (isLive(candidate) != hadNoAccesses)
            continue;
        Field& field = editor.at(fields[candidate]);
        if (hadNoAccesses) {
            field.flow.location.stores = StoreCount::Unknown;
            continue;
        }
        field.flow.location.stores = StoreCount::Never;
        field.flow.location.reads = ReadState::NeverRead;
        // No allocation lists it any more: an object is finished without it.
        if (field.flow.occupancy == Occupancy::FilledAtBirth || field.flow.occupancy == Occupancy::FilledWithValueAtBirth)
            field.flow.occupancy = Occupancy::Unknown;
        // A validating build tells the run time which names it took away, so that a lookup of one from outside compiled code ends the process.
        if (build.validates())
            field.flags.add(FieldFlags::IsRemovedAsUnread);
        ++numberOfRemovedFields;
    }
    build.statistics().count(StatisticID::UnreadFieldsRemoved, numberOfRemovedFields);
    for (size_t index : variables) {
        if (isLive(candidateOfVariable(index)))
            continue;
        LocationFlow& flow = editor.at(VariableID(index)).flow;
        flow.stores = StoreCount::Never;
        flow.reads = ReadState::NeverRead;
    }

    functions.removeAllMatching([&](FunctionAccesses& entry) {
        entry.second.removeAllMatching([&](const RemovableAccess& site) {
            return std::ranges::any_of(site.candidates, isLive);
        });
        return entry.second.isEmpty();
    });
    uint64_t numberOfVariableStores = 0;
    Vector<FunctionID> ids;
    ids.reserveInitialCapacity(functions.size());
    for (const auto& [id, accesses] : functions) {
        ids.append(id);
        for (const RemovableAccess& site : accesses)
            numberOfVariableStores += program.at(id).at(site.access).opcode == Opcode::PutModuleVariable;
    }
    build.statistics().count(StatisticID::UnreadVariableStoresRemoved, numberOfVariableStores);

    editFunctionsInParallel(editor, build, ids.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        IRBuilder builder(AnalysisContext { build, program, &inputs.flow, &inputs.effects, id }, function, arenas.types);
        std::span<const RemovableAccess> accesses = functions[index].second.span();
        auto isNext = [&](BlockID block) {
            return !accesses.empty() && accesses.front().block == block;
        };
        Vector<ValueID, 4> valuesOfErasedStores;
        while (!accesses.empty()) {
            BlockID block = accesses.front().block;
            const BlockValues& values = function.at(block).values;
            BitVector erasedIndexes;
            for (uint32_t position = 0; position < values.size() && isNext(block); ++position) {
                if (accesses.front().access != values[position])
                    continue;
                while (isNext(block) && accesses.front().access == values[position])
                    accesses = accesses.subspan(1);

                Value& access = function.edit(values[position]);
                if (access.opcode != Opcode::NewObject) {
                    if (access.opcode == Opcode::PutEnvironmentField)
                        valuesOfErasedStores.append(access.valueOperand());
                    erasedIndexes.set(position);
                    continue;
                }
                RELEASE_ASSERT(access.fields().size() == access.variadicOperands().size());
                Vector<uint32_t, 16> remainingFields;
                Vector<ValueID, 16> remainingValues;
                for (size_t listed = 0; listed < access.fields().size(); ++listed) {
                    auto candidate = candidateOf(access.layout(), access.fields()[listed]);
                    if (candidate && !isLive(*candidate))
                        continue;
                    remainingFields.append(access.fields()[listed]);
                    remainingValues.append(access.variadicOperands()[listed]);
                }
                access.setFields(remainingFields.span());
                access.setVariadicOperands(remainingValues.span());
            }
            RELEASE_ASSERT(!isNext(block));
            if (!erasedIndexes.isEmpty())
                builder.eraseValues(block, erasedIndexes);
        }
        // What only a suspension lists now is not built: the suspension lists what it would hold.
        bool isListedBySuspension = !valuesOfErasedStores.isEmpty() && std::ranges::any_of(function.blocks, [&](const Block& block) {
            return std::ranges::any_of(block.values, [&](ValueID value) {
                return canHoldOperands(function.at(value).opcode) && std::ranges::find_first_of(heldOperandsOf(function.at(value)), valuesOfErasedStores) != heldOperandsOf(function.at(value)).end();
            });
        });
        if (isListedBySuspension) {
            FunctionAnalyses analyses(AnalysisContext { build, program, &inputs.flow, &inputs.effects, id }, function);
            FunctionPassContext context { program, inputs, id, function, analyses, arenas.types, arenas.strings, build };
            runner.runFunctionPass(PassID::SinkAllocations, context);
        }
        simplifyEditedFunction(runner, program, inputs, id, function, arenas);
    });
}

void removeUnreadExports(ProgramEditor& editor, const FlowResults& flow, BuildContext& build)
{
    if (!flow.hasResults())
        return;

    const Program& program = editor.program();
    BitVector exportedVariables(program.variables.size());
    auto addExport = [&](VariableID variable) {
        if (variable)
            exportedVariables.quickSet(variable.index());
    };
    for (const Module& module : program.modules) {
        for (const ModuleExport& entry : module.exports)
            addExport(entry.variable);
        for (const SyntheticExport& entry : module.syntheticExports)
            addExport(entry.variable);
    }
    BitVector unreadVariables;
    for (unsigned index = 0; index < program.variables.size(); ++index) {
        const Variable& variable = program.variables[index];
        bool mayBeReadThroughNamespace = exportedVariables.quickGet(index) && mayBeReadByUnknownKey(flow.module(variable.module).namespaceUse);
        // Whether a static field exists decides where a store by name to its class or to a subclass goes, so its definition is no dead store.
        if (variable.flow.reads == ReadState::NeverRead && variable.flow.writers == WriterState::AllKnown && !variable.owner && !mayBeReadThroughNamespace)
            unreadVariables.set(index);
    }

    // Flow saw the reads that have become constants since.
    const ParallelLoops& loops = build.parallelLoops();
    BitVector variablesToLookFor = findVariablesThatOnlyBodiesRead(program, flow);
    variablesToLookFor.exclude(unreadVariables);
    if (!variablesToLookFor.isEmpty()) {
        Vector<uint8_t> isRead(FillWith { }, program.variables.size(), static_cast<uint8_t>(0));
        forEachIndexInParallel(loops, program.functions.size(), [&](unsigned index) {
            forEachListedValue(program, FunctionID(index), { Opcode::GetModuleVariable, Opcode::GetEncodedModuleVariable }, [&](BlockID, uint32_t, ValueID, const Value& value) {
                WTF::atomicStore(&isRead[value.variable().index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
            });
        });
        for (size_t index : variablesToLookFor) {
            if (!isRead[index])
                unreadVariables.set(index);
        }
    }
    if (unreadVariables.isEmpty())
        return;

    using FunctionStores = std::pair<FunctionID, Vector<VariableID, 4>>;
    Vector<uint8_t> mayHoldObjects(FillWith { }, program.variables.size(), static_cast<uint8_t>(0));
    EffectSummaries unknownSummaries;
    UnnoticedObjects unnoticedObjects(program, flow);
    Vector<FunctionStores> stores = collectInParallel<FunctionStores>(loops, program.functions.size(), [&](unsigned index, Vector<FunctionStores>& found) {
        Vector<VariableID, 4> storedVariables;
        StoresOfObjects storesOfObjects(build, program, flow, unnoticedObjects, &unknownSummaries, FunctionID(index));
        forEachListedValue(program, FunctionID(index), { Opcode::PutModuleVariable }, [&](BlockID, uint32_t, ValueID, const Value& value) {
            if (!unreadVariables.get(value.variable().index()))
                return;
            storedVariables.append(value.variable());
            if (storesOfObjects.keepsObjectAlive(value.operand(OperandIndex::PutModuleVariable::value), ValueID()))
                WTF::atomicStore(&mayHoldObjects[value.variable().index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
        });
        if (!storedVariables.isEmpty())
            found.append({ FunctionID(index), WTF::move(storedVariables) });
    });
    for (size_t index = 0; index < mayHoldObjects.size(); ++index) {
        if (mayHoldObjects[index])
            unreadVariables.quickClear(index);
    }
    stores.removeAllMatching([&](FunctionStores& entry) {
        entry.second.removeAllMatching([&](VariableID variable) {
            return !unreadVariables.get(variable.index());
        });
        return entry.second.isEmpty();
    });

    Vector<Function*> editedFunctions;
    BitVector variablesWithRemovedStores;
    BitVector variablesWithRemainingStores;
    uint64_t numberOfRemovedStores = 0;
    for (const auto& [function, variables] : stores) {
        bool isEdited = build.isEnabled(PassID::RemoveUnreadExports, function);
        for (VariableID variable : variables)
            (isEdited ? variablesWithRemovedStores : variablesWithRemainingStores).set(variable.index());
        if (!isEdited)
            continue;
        editedFunctions.append(&editor.at(function));
        numberOfRemovedStores += variables.size();
    }

    forEachIndexInParallel(loops, editedFunctions.size(), [&](unsigned index) {
        Function& function = *editedFunctions[index];
        for (Block& block : function.editAllBlocks()) {
            block.values.removeAllMatching([&](ValueID id) {
                return function.at(id).opcode == Opcode::PutModuleVariable && unreadVariables.get(function.at(id).variable().index());
            });
        }
        // The annotation says that the call may go when nobody uses its answer. An argument can be such a call too, and a cast that tests nothing is no use by itself.
        ObservableOperations observable({ program, unknownSummaries }, function);
        for (bool didRemove = true; std::exchange(didRemove, false);) {
            BitVector usedValues = findUsedValues(function);
            for (Block& block : function.editAllBlocks()) {
                didRemove |= !!block.values.removeAllMatching([&](ValueID id) {
                    const Value& value = function.at(id);
                    if (usedValues.quickGet(id.index()))
                        return false;
                    bool isAnnotatedPure = (value.opcode == Opcode::Call || value.opcode == Opcode::Construct) && value.isAnnotatedPure();
                    return isAnnotatedPure || isCastWithoutTest(value.opcode) || (resultRuleOf(value.opcode) == ResultRule::Constant && observable.neitherThrowsNorRunsCode(id));
                });
            }
        }
    });

    variablesWithRemovedStores.exclude(variablesWithRemainingStores);
    for (size_t index : variablesWithRemovedStores)
        editor.at(VariableID(index)).flow.stores = StoreCount::Never;
    for (size_t index : unreadVariables)
        editor.at(VariableID(index)).flow.reads = ReadState::NeverRead;
    build.statistics().count(StatisticID::UnreadVariableStoresRemoved, numberOfRemovedStores);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
