/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include "JSCInlines.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

BlockID splitEdge(Function& function, BlockID from, unsigned targetIndex)
{
    BlockID added(function.blocks.size());
    Block block;
    block.terminator.kind = TerminatorKind::Jump;
    block.terminator.targets.append(std::exchange(function.at(from).terminator.targets[targetIndex], Target { added, { } }));
    function.blocks.append(WTF::move(block));
    return added;
}

namespace {

class ObjectAllocationSinking {
public:
    ObjectAllocationSinking(const Program& program, Function& function, AllocationSinkingPolicy policy)
        : m_program(program)
        , m_function(function)
        , m_policy(policy)
    {
    }

    bool run()
    {
        if (!collectCandidates())
            return false;
        computeBlockOrder();
        if (!pruneBlockLocalCandidates())
            return false;
        computeLiveness();
        for (;;) {
            analyze();
            countMaterializations();
            if (!abandonUnprofitableCandidates())
                break;
            if (hasNothingLeftToTry())
                return false;
        }
        if (m_policy == AllocationSinkingPolicy::AllowMaterialization && duplicateTailForMerge())
            return true;
        if (!std::ranges::any_of(m_candidates, [](const Candidate& object) { return !object.isAbandoned; }))
            return false;
        rewrite();
        return true;
    }

private:
    static constexpr uint32_t none = UINT32_MAX;
    static constexpr uint32_t unvisited = UINT32_MAX - 1;
    static constexpr unsigned maxMaterializations = 2;
    static constexpr unsigned maxFields = 64;
    static constexpr unsigned maxTailSize = 200;

    enum class Mode : uint8_t { Analyze, Count, Rewrite };
    enum class Status : uint8_t { Unknown, Virtual, MaybeMaterialized, Materialized };
    enum class KnownBoolean : uint8_t { Unknown, False, True };
    static constexpr unsigned noTarget = UINT_MAX;

    struct ObjectState {
        Status status { Status::Unknown };
        uint64_t stored { 0 };
        uint64_t created { 0 };

        friend bool operator==(const ObjectState&, const ObjectState&) = default;
    };

    struct Entry {
        uint32_t object;
        ObjectState state;

        friend bool operator==(const Entry&, const Entry&) = default;
    };
    using State = Vector<Entry, 2>;

    struct Candidate {
        ValueID allocation;
        unsigned block;
        bool isClosure { false };
        uint32_t group { none };
        uint64_t initialFields { 0 };
        uint64_t creatableFields { 0 };
        uint64_t speculativelyLoadableFields { 0 };
        uint64_t fieldsLoadedWhileMaybeMaterialized { 0 };
        bool allowsMaybeMaterialized { true };
        ASCIILiteral maybeMaterializedRefusal;
        BitVector liveAtHead;
        unsigned materializationCount { 0 };
        bool diesVirtual { false };
        bool isMaterializedOutsideBirthBlock { false };
        bool isMaterializedForMerge { false };
        bool isMaterializedForParameter { false };
        unsigned mergeBlock { UINT_MAX };
        Vector<ValueID> singleStoredValue;
        uint64_t multiplyStoredFields { 0 };
        bool isAbandoned { false };
        ValueID escapeSite;
        ASCIILiteral escapeReason;
        ASCIILiteral abandonReason;
    };

    struct Edge {
        unsigned from;
        unsigned target;
    };

    struct Meet {
        ObjectState first;
        uint64_t current { 0 };
        unsigned count { 0 };
        bool differs { false };
        bool isVirtualOnSomeEdge { false };
    };

    const Type& typeOf(ValueID id) const { return m_program.typeOf(m_function, id); }
    const Layout& layoutOf(const Candidate& object) const { return m_program.at(m_function.at(object.allocation).layout()); }
    const Allocation& allocationOf(const Candidate& object) const { return m_function.allocations[m_function.at(object.allocation).allocation()]; }
    TypeID fieldType(const Candidate& object, unsigned field) const
    {
        auto& declared = layoutOf(object).fields[field];
        if (declared.flags & fieldHoldsClosureMadeOnce)
            return m_program.types.undefined;
        return declared.type ? declared.type : m_program.types.anything;
    }
    bool isVirtual(uint32_t object) const { return object != none && m_state[object].status == Status::Virtual; }
    bool isMaybeMaterialized(uint32_t object) const { return object != none && m_state[object].status == Status::MaybeMaterialized; }
    bool isTracked(uint32_t object) const { return isVirtual(object) || isMaybeMaterialized(object); }
    uint32_t aliasOf(ValueID id) const { return id && id.index() < m_alias.size() ? m_alias[id.index()] : none; }
    bool hasInvokedValue(unsigned b) const { return m_function.blocks[b].terminator.kind == TerminatorKind::Invoke && !m_function.blocks[b].values.isEmpty(); }
    bool isIgnoredEdge(unsigned from, unsigned target) const
    {
        if (m_ignoredEdges.size() > 8)
            return m_ignoredEdgeSet.contains(static_cast<uint64_t>(from) << 32 | target);
        return m_ignoredEdges.containsIf([&](const Edge& edge) { return edge.from == from && edge.target == target; });
    }
    bool isDeadEdge(const Edge& edge) const { return isIgnoredEdge(edge.from, edge.target) || isDeadHandlerEdge(edge) || m_deadTarget[edge.from] == edge.target; }
    bool isDeadHandlerEdge(const Edge& edge) const { return edge.target == 1 && m_function.blocks[edge.from].terminator.kind == TerminatorKind::Invoke && m_isHandlerEdgeDead.get(edge.from); }

    static const Entry* find(const State& state, uint32_t object)
    {
        if (state.size() > 16) {
            auto found = std::ranges::lower_bound(state, object, { }, &Entry::object);
            return found != state.end() && found->object == object ? &*found : nullptr;
        }
        for (auto& entry : state) {
            if (entry.object == object)
                return &entry;
        }
        return nullptr;
    }

    bool isHandledUse(const Value& user, ValueID object) const
    {
        switch (user.opcode) {
        case Opcode::PutField:
        case Opcode::PutById:
            return user.child(1) != object || m_policy == AllocationSinkingPolicy::AllowMaterialization;
        case Opcode::NewClosure:
        case Opcode::IsClosureOf:
        case Opcode::GetCapture:
            return m_policy == AllocationSinkingPolicy::AllowMaterialization;
        case Opcode::GetField:
        case Opcode::GetById:
        case Opcode::Cast:
        case Opcode::AssertNotAllocated:
        case Opcode::StrictEq:
        case Opcode::IsUndefinedOrNull:
        case Opcode::ToBoolean:
        case Opcode::TypeOfIs:
            return true;
        default:
            return false;
        }
    }

    bool pruneEscapingCandidates(Vector<std::pair<ValueID, unsigned>, 8>& found) const
    {
        Vector<uint32_t> origin;
        origin.fill(none, m_function.values.size());
        Vector<uint32_t, 8> parent;
        for (unsigned i = 0; i < found.size(); ++i) {
            origin[found[i].first.index()] = i;
            parent.append(i);
        }
        auto classOf = [&](uint32_t object) {
            while (parent[object] != object)
                object = parent[object] = parent[parent[object]];
            return object;
        };
        auto flow = [&](ValueID from, ValueID to) {
            uint32_t given = origin[from.index()];
            if (given == none)
                return false;
            uint32_t& held = origin[to.index()];
            if (held == none) {
                held = given;
                return true;
            }
            if (classOf(held) == classOf(given))
                return false;
            parent[classOf(held)] = classOf(given);
            return true;
        };
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& block : m_function.blocks) {
                for (ValueID id : block.values) {
                    if (m_function.at(id).opcode == Opcode::Cast)
                        changed |= flow(m_function.at(id).child(0), id);
                }
                for (auto& target : block.terminator.targets) {
                    auto& parameters = m_function.at(target.block).parameters;
                    for (unsigned i = 0; i < target.arguments.size() && i < parameters.size(); ++i)
                        changed |= flow(target.arguments[i], parameters[i]);
                }
            }
        }
        BitVector escapes(found.size());
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                value.forEachChild([&](ValueID child) {
                    if (origin[child.index()] != none && !isHandledUse(value, child))
                        escapes.set(classOf(origin[child.index()]));
                });
            }
            if (ValueID value = block.terminator.value; value && origin[value.index()] != none)
                escapes.set(classOf(origin[value.index()]));
            for (auto& target : block.terminator.targets) {
                auto& parameters = m_function.at(target.block).parameters;
                for (unsigned i = 0; i < target.arguments.size() && i < parameters.size(); ++i) {
                    if (origin[parameters[i].index()] != none && origin[target.arguments[i].index()] == none)
                        escapes.set(classOf(origin[parameters[i].index()]));
                }
            }
        }
        unsigned kept = 0;
        for (unsigned i = 0; i < found.size(); ++i) {
            if (!escapes.get(classOf(i)))
                found[kept++] = found[i];
        }
        found.shrink(kept);
        return kept;
    }

    bool collectCandidates()
    {
        Vector<std::pair<ValueID, unsigned>, 8> found;
        Vector<ValueID, 8> local;
        BitVector isLocal;
        BitVector isFound;
        auto addLocal = [&](ValueID id) {
            isLocal.ensureSize(m_function.values.size());
            isFound.ensureSize(m_function.values.size());
            isLocal.quickSet(id.index());
            local.append(id);
        };
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& block = m_function.blocks[b];
            local.shrink(0);
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                if (!local.isEmpty()) {
                    std::as_const(value).forEachChild([&](ValueID child) {
                        if (!isHandledUse(value, child))
                            isLocal.quickClear(child.index());
                    });
                }
                if (value.opcode == Opcode::NewClosure && m_policy == AllocationSinkingPolicy::AllowMaterialization && value.arguments.containsIf([&](ValueID capture) {
                    return isLocal.get(capture.index()) || isFound.get(capture.index());
                }))
                    addLocal(id);
                if (value.opcode != Opcode::NewObjectWithShape || value.child(0))
                    continue;
                auto& layout = m_program.at(value.layout());
                if (layout.kind != LayoutKind::Closure && !layout.isCoroutineFrame && layout.fields.size() <= maxFields)
                    addLocal(id);
            }
            if (block.terminator.value && !local.isEmpty())
                isLocal.quickClear(block.terminator.value.index());
            for (ValueID id : local) {
                if (!isLocal.quickClear(id.index()))
                    continue;
                isFound.quickSet(id.index());
                found.append({ id, b });
            }
        }
        if (found.isEmpty() || (m_policy == AllocationSinkingPolicy::EliminateOnly && !pruneEscapingCandidates(found)))
            return false;
        m_candidateForValue.fill(none, m_function.values.size());
        bool hasHandlers = m_function.blocks.containsIf([](auto& block) { return block.terminator.kind == TerminatorKind::Invoke; });
        for (auto [id, b] : found) {
            auto& value = m_function.at(id);
            Candidate object;
            object.allocation = id;
            object.block = b;
            if (value.opcode == Opcode::NewClosure) {
                object.isClosure = true;
                m_candidateForValue[id.index()] = m_candidates.size();
                m_candidates.append(WTF::move(object));
                continue;
            }
            auto& layout = m_program.at(value.layout());
            object.singleStoredValue.fill(ValueID(), layout.fields.size());
            auto& allocation = m_function.allocations[value.allocation()];
            uint64_t initialFieldSet = 0;
            for (uint16_t field : allocation.order.span().first(allocation.born)) {
                initialFieldSet |= 1ull << field;
                if (layout.fields[field].flags & (fieldIsNeverStored | fieldIsAbsentAtBirth) || (layout.fields[field].flags & fieldIsMethod && layout.kind != LayoutKind::Record))
                    continue;
                object.initialFields |= 1ull << field;
                if (isObjectLayout(layout.kind) ? !layout.mayGiveUp && layout.fields[field].flags & fieldIsNeverSeenAbsent && !mayInherit(m_program, value.layout(), layout.fields[field].name) : layout.fields[field].flags & (fieldIsImmutable | fieldIsNeverSeenUninitialized) && !(layout.fields[field].flags & fieldHoldsClosureMadeOnce))
                    object.speculativelyLoadableFields |= 1ull << field;
            }
            if (layout.kind == LayoutKind::Record && !hasHandlers && m_policy == AllocationSinkingPolicy::AllowMaterialization) {
                for (unsigned field = 0; field < layout.fields.size(); ++field) {
                    if (!(initialFieldSet & 1ull << field) && !(layout.fields[field].flags & fieldIsNeverStored))
                        object.creatableFields |= 1ull << field;
                }
            }
            m_candidateForValue[id.index()] = m_candidates.size();
            m_candidates.append(WTF::move(object));
        }
        return !m_candidates.isEmpty();
    }

    void computeBlockOrder()
    {
        unsigned numberOfBlocks = m_function.blocks.size();
        m_predecessors.grow(numberOfBlocks);
        BitVector seen(numberOfBlocks);
        seen.set(0);
        Vector<std::pair<unsigned, unsigned>> stack { { 0u, 0u } };
        while (!stack.isEmpty()) {
            auto [block, next] = stack.last();
            auto& targets = m_function.blocks[block].terminator.targets;
            if (next == targets.size()) {
                m_blockOrder.append(block);
                stack.removeLast();
                continue;
            }
            ++stack.last().second;
            unsigned target = targets[next].block.index();
            m_predecessors[target].append(Edge { block, next });
            if (!seen.set(target))
                stack.append({ target, 0u });
        }
        m_blockOrder.reverse();
        Vector<unsigned> place(FillWith { }, numberOfBlocks, UINT_MAX);
        for (unsigned k = 0; k < m_blockOrder.size(); ++k)
            place[m_blockOrder[k]] = k;
        m_isLedBackTo.ensureSize(numberOfBlocks);
        for (unsigned b : m_blockOrder) {
            for (auto& edge : m_predecessors[b]) {
                if (place[edge.from] >= place[b])
                    m_isLedBackTo.quickSet(b);
            }
        }
        m_firstThatActsWithoutObjects.fill(0, numberOfBlocks + 1);
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            m_firstThatActsWithoutObjects[b] = m_valuesThatActWithoutObjects.size();
            for (ValueID id : m_function.blocks[b].values) {
                auto& value = m_function.at(id);
                switch (value.opcode) {
                case Opcode::IsClosureOf:
                case Opcode::GetCapture:
                case Opcode::LogicalNot:
                case Opcode::AssertNotAllocated:
                    m_valuesThatActWithoutObjects.append(id);
                    break;
                case Opcode::PutField:
                    if (m_program.at(value.layout()).reuseVariable)
                        m_valuesThatActWithoutObjects.append(id);
                    break;
                default:
                    break;
                }
            }
        }
        m_firstThatActsWithoutObjects[numberOfBlocks] = m_valuesThatActWithoutObjects.size();
    }

    void findBlocksThatHoldObjects()
    {
        m_holdsObjects.clearAll();
        m_holdsObjects.ensureSize(m_function.blocks.size());
        for (auto& object : m_candidates) {
            if (object.isAbandoned)
                continue;
            m_holdsObjects.quickSet(object.block);
            m_holdsObjects.merge(object.liveAtHead);
        }
    }

    void reset()
    {
        unsigned numberOfBlocks = m_function.blocks.size();
        m_head.clear();
        m_head.grow(numberOfBlocks);
        m_tail.clear();
        m_tail.grow(numberOfBlocks);
        m_isReached.clearAll();
        m_isReached.ensureSize(numberOfBlocks);
        m_isHandlerEdgeDead.clearAll();
        m_isHandlerEdgeDead.ensureSize(numberOfBlocks);
        m_deadTarget.fill(noTarget, numberOfBlocks);
        m_knownBoolean.fill(KnownBoolean::Unknown, m_function.values.size());
        m_loadedValue.fill(ValueID(), m_function.values.size());
        m_state.fill(ObjectState { }, m_candidates.size());
        m_tracked.clear();
        m_isTracked.clearAll();
        m_isTracked.ensureSize(m_candidates.size());
        m_alias.fill(none, m_function.values.size());
        for (unsigned i = 0; i < m_candidates.size(); ++i) {
            if (!m_candidates[i].isAbandoned)
                m_alias[m_candidates[i].allocation.index()] = i;
        }
    }

    bool pruneBlockLocalCandidates()
    {
        reset();
        findBlocksThatHoldObjects();
        m_mode = Mode::Analyze;
        for (unsigned b : m_blockOrder)
            processBlock(b);
        Vector<Candidate> kept;
        for (unsigned i = 0; i < m_candidates.size(); ++i) {
            auto& object = m_candidates[i];
            auto* entry = find(m_tail[object.block], i);
            m_candidateForValue[object.allocation.index()] = none;
            if (!entry || entry->state.status != Status::Virtual)
                continue;
            m_candidateForValue[object.allocation.index()] = kept.size();
            kept.append(WTF::move(object));
        }
        m_candidates = WTF::move(kept);
        return !m_candidates.isEmpty();
    }

    void computeLiveness()
    {
        Vector<Vector<uint32_t, 1>> mayAlias(m_function.values.size());
        for (unsigned i = 0; i < m_candidates.size(); ++i)
            mayAlias[m_candidates[i].allocation.index()].append(i);
        auto flow = [&](ValueID from, ValueID to) {
            bool changed = false;
            for (uint32_t object : mayAlias[from.index()]) {
                if (!mayAlias[to.index()].contains(object)) {
                    mayAlias[to.index()].append(object);
                    changed = true;
                }
            }
            return changed;
        };
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& object : m_candidates) {
                object.multiplyStoredFields = 0;
                if (!object.isClosure)
                    object.singleStoredValue.fill(ValueID(), layoutOf(object).fields.size());
            }
            for (unsigned b : m_blockOrder) {
                for (ValueID id : m_function.blocks[b].values) {
                    auto& store = m_function.at(id);
                    if (store.opcode != Opcode::PutField && store.opcode != Opcode::PutById)
                        continue;
                    for (uint32_t i : mayAlias[store.child(0).index()]) {
                        auto field = accessedField(i, store);
                        if (!field)
                            continue;
                        if (m_candidates[i].singleStoredValue[*field])
                            m_candidates[i].multiplyStoredFields |= 1ull << *field;
                        m_candidates[i].singleStoredValue[*field] = store.child(1);
                    }
                }
            }
            for (unsigned b : m_blockOrder) {
                auto& block = m_function.blocks[b];
                for (ValueID id : block.values) {
                    auto& value = m_function.at(id);
                    switch (value.opcode) {
                    case Opcode::Cast:
                        changed |= flow(value.child(0), id);
                        break;
                    case Opcode::GetField:
                    case Opcode::GetById:
                        for (unsigned n = 0; n < mayAlias[value.child(0).index()].size(); ++n) {
                            uint32_t i = mayAlias[value.child(0).index()][n];
                            if (auto field = accessedField(i, value); field && m_candidates[i].singleStoredValue[*field])
                                changed |= flow(m_candidates[i].singleStoredValue[*field], id);
                        }
                        break;
                    case Opcode::GetCapture:
                        for (unsigned n = 0; n < mayAlias[value.child(0).index()].size(); ++n) {
                            auto& closure = m_function.at(m_candidates[mayAlias[value.child(0).index()][n]].allocation);
                            if (closure.opcode == Opcode::NewClosure && value.immediates[1] < closure.arguments.size())
                                changed |= flow(closure.arguments[value.immediates[1]], id);
                        }
                        break;
                    default:
                        break;
                    }
                }
                for (auto& target : block.terminator.targets) {
                    auto& parameters = m_function.at(target.block).parameters;
                    for (unsigned i = 0; i < target.arguments.size() && i < parameters.size(); ++i)
                        changed |= flow(target.arguments[i], parameters[i]);
                }
            }
        }
        for (auto& object : m_candidates)
            object.liveAtHead.ensureSize(m_function.blocks.size());
        Vector<uint32_t> parent(m_candidates.size(), [](size_t i) {
            return static_cast<uint32_t>(i);
        });
        BitVector isJoined(m_candidates.size());
        auto rootOf = [&](uint32_t object) {
            while (parent[object] != object)
                object = parent[object] = parent[parent[object]];
            return object;
        };
        auto join = [&](uint32_t one, uint32_t other) {
            if (one == other)
                return;
            isJoined.quickSet(one);
            isJoined.quickSet(other);
            parent[rootOf(one)] = rootOf(other);
        };
        for (unsigned b : m_blockOrder) {
            for (ValueID id : m_function.blocks[b].values) {
                auto& value = m_function.at(id);
                if (value.opcode == Opcode::PutField || value.opcode == Opcode::PutById) {
                    for (uint32_t holder : mayAlias[value.child(0).index()]) {
                        for (uint32_t held : mayAlias[value.child(1).index()])
                            join(holder, held);
                    }
                }
                if (uint32_t closure = m_candidateForValue[id.index()]; closure != none && m_candidates[closure].isClosure) {
                    for (ValueID capture : value.arguments) {
                        for (uint32_t captured : mayAlias[capture.index()])
                            join(closure, captured);
                    }
                }
            }
        }
        for (unsigned i = 0; i < m_candidates.size(); ++i) {
            if (isJoined.quickGet(i))
                m_candidates[i].group = rootOf(i);
        }
        for (auto& object : m_candidates) {
            if (object.group == none)
                continue;
            object.allowsMaybeMaterialized = false;
            object.maybeMaterializedRefusal = "every way, with what it holds or what holds it"_s;
        }
        Vector<unsigned> worklist;
        auto use = [&](unsigned b, ValueID operand) {
            for (uint32_t i : mayAlias[operand.index()]) {
                auto& object = m_candidates[i];
                worklist.append(b);
                while (!worklist.isEmpty()) {
                    unsigned block = worklist.takeLast();
                    if (block == object.block || object.liveAtHead.set(block))
                        continue;
                    for (auto& edge : m_predecessors[block])
                        worklist.append(edge.from);
                }
            }
        };
        m_mayWriteFields.ensureSize(m_function.values.size());
        for (unsigned b : m_blockOrder) {
            auto& block = m_function.blocks[b];
            for (size_t index = 0; index < block.values.size(); ++index) {
                ValueID id = block.values[index];
                if (mayWriteFields(block, index))
                    m_mayWriteFields.set(id.index());
                if (m_function.at(id).opcode == Opcode::AssertNotAllocated)
                    continue;
                std::as_const(m_function.at(id)).forEachChild([&](ValueID child) {
                    use(b, child);
                });
            }
            block.terminator.forEachChild([&](ValueID& child) {
                use(b, child);
            });
        }
        for (unsigned i = 0; i < m_candidates.size(); ++i) {
            if (uint32_t group = m_candidates[i].group; group != none && group != i)
                m_candidates[group].liveAtHead.merge(m_candidates[i].liveAtHead);
        }
        for (unsigned i = 0; i < m_candidates.size(); ++i) {
            if (uint32_t group = m_candidates[i].group; group != none && group != i)
                m_candidates[i].liveAtHead.merge(m_candidates[group].liveAtHead);
        }
    }

    bool mayWriteFields(const Block& block, size_t index) const
    {
        switch (m_function.at(block.values[index]).opcode) {
        case Opcode::CallDirect:
        case Opcode::Await:
        case Opcode::Yield:
        case Opcode::GetResumeMode:
            return true;
        default:
            return writesOf(m_program, m_function, block, index) >= Writes::Fields;
        }
    }

    void restoreState(const State& state)
    {
        for (uint32_t object : m_tracked) {
            m_state[object] = { };
            m_isTracked.quickClear(object);
        }
        m_tracked.clear();
        for (auto& entry : state)
            setState(entry.object, entry.state);
    }

    State saveState() const
    {
        State state;
        for (uint32_t object : m_tracked) {
            if (m_state[object].status != Status::Unknown)
                state.append(Entry { object, m_state[object] });
        }
        std::ranges::sort(state, { }, &Entry::object);
        return state;
    }

    void setState(uint32_t object, ObjectState state)
    {
        if (m_state[object].status == Status::Unknown && !m_isTracked.quickSet(object))
            m_tracked.append(object);
        m_state[object] = state;
    }

    void analyze()
    {
        reset();
        findBlocksThatHoldObjects();
        m_mode = Mode::Analyze;
        m_meet.fill(Meet { }, m_candidates.size());
        bool isFirstSweep = true;
        for (bool changed = true; changed;) {
            changed = false;
            bool everyBlockIsReached = true;
            for (unsigned b : m_blockOrder) {
                if (b && !computeHead(b, changed)) {
                    everyBlockIsReached = false;
                    continue;
                }
                changed |= !m_isReached.set(b);
                State tail = std::exchange(m_tail[b], State { });
                bool wasHandlerEdgeDead = m_isHandlerEdgeDead.get(b);
                unsigned previousDeadTarget = m_deadTarget[b];
                processBlock(b);
                changed |= tail != m_tail[b] || wasHandlerEdgeDead != m_isHandlerEdgeDead.get(b) || previousDeadTarget != m_deadTarget[b];
            }
            if (std::exchange(isFirstSweep, false) && everyBlockIsReached && nothingComesBack())
                break;
        }
    }

    bool nothingComesBack() const
    {
        for (size_t b : m_isLedBackTo) {
            if (!m_head[b].isEmpty())
                return false;
            for (ValueID parameter : m_function.blocks[b].parameters) {
                if (m_alias[parameter.index()] != none)
                    return false;
            }
        }
        return true;
    }

    bool computeHead(unsigned b, bool& changed)
    {
        auto& parameters = m_function.blocks[b].parameters;
        if (parameters.isEmpty() && !m_holdsObjects.quickGet(b)) {
            return m_predecessors[b].containsIf([&](const Edge& edge) {
                return m_isReached.get(edge.from) && !isDeadEdge(edge);
            });
        }
        Vector<uint32_t, 4> common;
        common.fill(unvisited, parameters.size());
        unsigned numberOfEdges = 0;
        for (auto& edge : m_predecessors[b]) {
            if (!m_isReached.get(edge.from) || isDeadEdge(edge))
                continue;
            ++numberOfEdges;
            auto& arguments = m_function.blocks[edge.from].terminator.targets[edge.target].arguments;
            for (unsigned i = 0; i < parameters.size(); ++i) {
                uint32_t alias = i < arguments.size() ? aliasOf(arguments[i]) : none;
                if (alias != none && !find(m_tail[edge.from], alias))
                    alias = none;
                common[i] = common[i] == unvisited || common[i] == alias ? alias : none;
            }
        }
        if (!numberOfEdges)
            return false;
        Vector<uint32_t, 4> met;
        for (auto& edge : m_predecessors[b]) {
            if (!m_isReached.get(edge.from) || isDeadEdge(edge))
                continue;
            auto& arguments = m_function.blocks[edge.from].terminator.targets[edge.target].arguments;
            for (auto& entry : m_tail[edge.from]) {
                ObjectState state = entry.state;
                for (unsigned i = 0; i < parameters.size() && i < arguments.size() && state.status == Status::Virtual; ++i) {
                    if (aliasOf(arguments[i]) == entry.object && common[i] != entry.object)
                        state = { Status::Materialized, 0 };
                }
                auto& meet = m_meet[entry.object];
                uint64_t current = state.status == Status::Materialized ? m_candidates[entry.object].speculativelyLoadableFields : state.stored;
                if (!meet.count++) {
                    met.append(entry.object);
                    meet.first = state;
                    meet.current = current;
                    meet.differs = false;
                    meet.isVirtualOnSomeEdge = false;
                }
                meet.isVirtualOnSomeEdge |= entry.state.status == Status::Virtual;
                if (meet.count > 1) {
                    meet.differs |= meet.first != state;
                    meet.current &= current;
                }
            }
        }
        std::ranges::sort(met);
        Vector<uint32_t, 2> groupsToMaterialize;
        for (uint32_t object : met) {
            auto& meet = m_meet[object];
            if (m_candidates[object].group != none && (meet.count != numberOfEdges || meet.differs || (meet.first.status == Status::Materialized && meet.isVirtualOnSomeEdge)) && !groupsToMaterialize.contains(m_candidates[object].group))
                groupsToMaterialize.append(m_candidates[object].group);
        }
        State head;
        for (uint32_t object : met) {
            auto& meet = m_meet[object];
            if (meet.count == numberOfEdges && m_candidates[object].liveAtHead.get(b)) {
                if (groupsToMaterialize.contains(m_candidates[object].group))
                    head.append(Entry { object, ObjectState { Status::Materialized, 0 } });
                else
                    head.append(Entry { object, !meet.differs ? meet.first : m_candidates[object].allowsMaybeMaterialized ? ObjectState { Status::MaybeMaterialized, meet.current } : ObjectState { Status::Materialized, 0 } });
            }
            meet.count = 0;
        }
        for (unsigned i = 0; i < parameters.size(); ++i) {
            changed |= m_alias[parameters[i].index()] != common[i];
            m_alias[parameters[i].index()] = common[i];
        }
        changed |= head != m_head[b];
        m_head[b] = WTF::move(head);
        return true;
    }

    void countMaterializations()
    {
        m_mode = Mode::Count;
        for (auto& object : m_candidates) {
            if (object.isAbandoned)
                continue;
            object.mergeBlock = UINT_MAX;
            object.materializationCount = 0;
            object.fieldsLoadedWhileMaybeMaterialized = 0;
            object.diesVirtual = false;
            object.isMaterializedOutsideBirthBlock = false;
            object.isMaterializedForMerge = false;
            object.isMaterializedForParameter = false;
            object.escapeSite = { };
            object.escapeReason = { };
        }
        for (unsigned b : m_blockOrder) {
            if (!m_isReached.get(b))
                continue;
            processBlock(b);
            processEdges(b);
        }
    }

    bool hasNothingLeftToTry() const
    {
        return std::ranges::all_of(m_candidates, [&](const Candidate& object) {
            return object.isAbandoned && (m_policy == AllocationSinkingPolicy::EliminateOnly || !object.isMaterializedForMerge || object.mergeBlock == UINT_MAX);
        });
    }

    bool abandonUnprofitableCandidates()
    {
        if (std::exchange(m_didDisallowMaybeMaterialized, false))
            return true;
        bool any = false;
        for (unsigned i = 0; i < m_candidates.size(); ++i) {
            auto& object = m_candidates[i];
            if (object.isAbandoned)
                continue;
            if (object.abandonReason.isNull()) {
                if (object.materializationCount > maxMaterializations)
                    object.abandonReason = "more than 2 places"_s;
                else if (object.materializationCount && !object.diesVirtual)
                    object.abandonReason = object.isMaterializedForMerge ? object.maybeMaterializedRefusal : object.isMaterializedForParameter ? "every way, one for a block parameter"_s : "every way"_s;
                else if (object.materializationCount && m_policy == AllocationSinkingPolicy::EliminateOnly)
                    object.abandonReason = "not yet"_s;
                else
                    continue;
            }
            object.isAbandoned = true;
            any = true;
            if (m_policy == AllocationSinkingPolicy::EliminateOnly || !object.isMaterializedOutsideBirthBlock || m_isSpeculating)
                continue;
            noteAllocation(m_program, m_function, m_function.at(object.allocation), AllocationOutcome::Unchanged, 0, object.escapeSite ? &m_function.at(object.escapeSite) : nullptr, object.abandonReason);
        }
        return any;
    }

    void didMaterialize(uint32_t object, ValueID escapeSite, ASCIILiteral escapeReason, bool isInBlock = true)
    {
        auto& counted = m_candidates[object];
        counted.isMaterializedOutsideBirthBlock |= !isInBlock || m_currentBlock != counted.block;
        if (!counted.materializationCount++) {
            counted.escapeSite = escapeSite;
            counted.escapeReason = escapeReason;
        }
    }

    ValueID append(Value&& value)
    {
        m_function.values.append(WTF::move(value));
        return ValueID(m_function.values.size() - 1);
    }

    uint32_t candidateInField(uint32_t holder, unsigned field) const
    {
        auto& object = m_candidates[holder];
        if (object.multiplyStoredFields & 1ull << field || field >= object.singleStoredValue.size())
            return none;
        return aliasOf(object.singleStoredValue[field]);
    }

    uint64_t fieldsHoldingCandidates(uint32_t holder) const
    {
        uint64_t result = 0;
        for (unsigned field = 0; field < m_candidates[holder].singleStoredValue.size(); ++field) {
            if (candidateInField(holder, field) != none)
                result |= 1ull << field;
        }
        return result;
    }

    ValueID fieldValue(uint32_t holder, unsigned field) const
    {
        uint32_t held = candidateInField(holder, field);
        if (held == none)
            return m_fieldValues[holder][field];
        RELEASE_ASSERT(m_state[held].status == Status::Materialized);
        return m_materialized[held];
    }

    static Vector<unsigned, 4> createdFieldsInOrder(uint64_t created)
    {
        Vector<unsigned, 4> fields;
        for (; created; created >>= 8)
            fields.append((created & 0xff) - 1);
        fields.reverse();
        return fields;
    }

    ValueID emitUndefined(const Candidate& object, Vector<ValueID>& out)
    {
        Value undefined;
        undefined.opcode = Opcode::Undefined;
        undefined.type = m_program.types.undefined;
        undefined.rep = repOf(m_program.at(undefined.type));
        undefined.position = m_function.at(object.allocation).position;
        undefined.inlineFrame = m_function.at(object.allocation).inlineFrame;
        out.append(append(WTF::move(undefined)));
        return out.last();
    }

    void emitStore(uint32_t holder, unsigned field, bool initialFieldSet, ValueID value, Vector<ValueID>& out)
    {
        auto& object = m_candidates[holder];
        Value store;
        store.opcode = initialFieldSet ? Opcode::PutField : Opcode::PutById;
        store.type = m_program.types.undefined;
        store.rep = repOf(m_program.at(store.type));
        store.position = m_function.at(object.allocation).position;
        store.inlineFrame = m_function.at(object.allocation).inlineFrame;
        store.children = { m_materialized[holder], value, ValueID() };
        if (initialFieldSet)
            store.immediates = { m_function.at(object.allocation).layout().index(), field };
        else
            store.immediates = { layoutOf(object).fields[field].name.index(), Format::none };
        out.append(append(WTF::move(store)));
    }

    Vector<unsigned, 4> materializeCell(uint32_t index, ObjectState state, Vector<ValueID>& out)
    {
        auto& object = m_candidates[index];
        auto isThere = [&](unsigned field) {
            return !isVirtual(candidateInField(index, field));
        };
        Vector<unsigned, 4> now;
        Vector<unsigned, 4> late;
        Allocation allocation = allocationOf(object);
        for (uint16_t field : allocation.order.span().first(allocation.born)) {
            if (!(state.stored & 1ull << field))
                continue;
            now.append(field);
            if (!isThere(field))
                late.append(field);
        }
        bool allAreThere = true;
        allocation.order.shrink(allocation.born);
        for (unsigned field : createdFieldsInOrder(state.created)) {
            allAreThere &= isThere(field);
            if (!allAreThere) {
                late.append(field);
                continue;
            }
            now.append(field);
            allocation.order.append(field);
        }
        if (m_mode != Mode::Rewrite)
            return late;
        ValueID undefined = now.containsIf([&](unsigned field) { return !isThere(field); }) ? emitUndefined(object, out) : ValueID();
        m_materialized[index] = append(Value(m_function.at(object.allocation)));
        if (allocation.order.size() != allocation.born) {
            allocation.born = allocation.order.size();
            m_function.at(m_materialized[index]).immediates[1] = m_function.allocationOf(WTF::move(allocation));
        }
        out.append(m_materialized[index]);
        for (unsigned field : now)
            emitStore(index, field, true, isThere(field) ? fieldValue(index, field) : undefined, out);
        return late;
    }

    void collectVirtualReferences(uint32_t index, Vector<uint32_t, 4>& members) const
    {
        if (members.contains(index))
            return;
        members.append(index);
        auto& candidate = m_candidates[index];
        if (candidate.isClosure) {
            for (ValueID capture : m_function.at(candidate.allocation).arguments) {
                if (uint32_t captured = aliasOf(capture); isVirtual(captured))
                    collectVirtualReferences(captured, members);
            }
            return;
        }
        for (unsigned field = 0; field < candidate.singleStoredValue.size(); ++field) {
            if (uint32_t held = candidateInField(index, field); m_state[index].stored & 1ull << field && isVirtual(held))
                collectVirtualReferences(held, members);
        }
    }

    void materializeClosure(uint32_t index, Vector<ValueID>& out)
    {
        Arguments captures = m_function.at(m_candidates[index].allocation).arguments;
        for (ValueID& capture : captures) {
            if (uint32_t captured = aliasOf(capture); isVirtual(captured))
                materializeClosure(captured, out);
            if (m_mode == Mode::Rewrite)
                capture = resolve(capture);
        }
        if (m_mode == Mode::Rewrite) {
            m_materialized[index] = append(Value(m_function.at(m_candidates[index].allocation)));
            m_function.at(m_materialized[index]).arguments = WTF::move(captures);
            out.append(m_materialized[index]);
        }
        m_state[index] = { Status::Materialized, 0 };
    }

    void materialize(uint32_t root, ValueID escapeSite, ASCIILiteral escapeReason, Vector<ValueID>& out, bool isInBlock)
    {
        Vector<uint32_t, 4> members;
        collectVirtualReferences(root, members);
        Vector<std::pair<uint32_t, Vector<unsigned, 4>>, 2> deferredStores;
        for (uint32_t index : members) {
            if (m_mode == Mode::Count) {
                didMaterialize(index, escapeSite, escapeReason, isInBlock);
                if (m_isOnHandlerEdge)
                    m_candidates[index].abandonReason = "built on the way to a handler"_s;
            }
            if (m_candidates[index].isClosure)
                continue;
            deferredStores.append({ index, materializeCell(index, m_state[index], out) });
            m_state[index] = { Status::Materialized, 0 };
        }
        for (uint32_t index : members) {
            if (isVirtual(index))
                materializeClosure(index, out);
        }
        if (m_mode != Mode::Rewrite)
            return;
        for (auto& [holder, fields] : deferredStores) {
            for (unsigned field : fields)
                emitStore(holder, field, !isObjectLayout(layoutOf(m_candidates[holder]).kind), fieldValue(holder, field), out);
        }
    }

    void escape(uint32_t object, ValueID escapeSite, ASCIILiteral escapeReason = { })
    {
        materialize(object, escapeSite, escapeReason, m_newValues, true);
    }

    void disallowMaybeMaterialized(uint32_t object, ASCIILiteral escapeReason)
    {
        if (m_mode != Mode::Count || !m_candidates[object].allowsMaybeMaterialized)
            return;
        m_candidates[object].allowsMaybeMaterialized = false;
        m_candidates[object].maybeMaterializedRefusal = escapeReason;
        m_didDisallowMaybeMaterialized = true;
    }

    ValueID emitLoad(uint32_t index, unsigned field, Vector<ValueID>& out)
    {
        auto& object = m_candidates[index];
        auto& layout = layoutOf(object);
        Value read;
        read.opcode = isObjectLayout(layout.kind) ? Opcode::GetById : Opcode::GetField;
        read.type = fieldType(object, field);
        read.rep = repOf(m_program.at(read.type));
        read.position = m_function.at(object.allocation).position;
        read.inlineFrame = m_function.at(object.allocation).inlineFrame;
        read.children = { m_materialized[index], ValueID(), ValueID() };
        if (isObjectLayout(layout.kind))
            read.immediates = { layout.fields[field].name.index(), Format::none };
        else
            read.immediates = { m_function.at(object.allocation).layout().index(), field };
        out.append(append(WTF::move(read)));
        return out.last();
    }

    ValueID resolve(ValueID id) const
    {
        uint32_t object = aliasOf(id);
        if (object == none)
            return id;
        RELEASE_ASSERT(m_state[object].status == Status::Materialized);
        return m_materialized[object];
    }

    void escapeOperands(ValueID id)
    {
        if (m_tracked.isEmpty() && m_mode != Mode::Rewrite)
            return;
        Vector<ValueID, 8> operands;
        std::as_const(m_function.at(id)).forEachChild([&](ValueID child) {
            operands.append(child);
        });
        for (ValueID operand : operands) {
            if (isVirtual(aliasOf(operand)))
                escape(aliasOf(operand), id);
            else if (isMaybeMaterialized(aliasOf(operand)))
                disallowMaybeMaterialized(aliasOf(operand), "every way, needed behind a merge"_s);
        }
        if (m_mode != Mode::Rewrite)
            return;
        m_function.at(id).forEachChild([&](ValueID& child) {
            child = resolve(child);
        });
    }

    bool isTransparentCast(const Value& cast) const
    {
        return cast.castReason() != CastReason::Coerce && typeOf(m_candidates[aliasOf(cast.child(0))].allocation).isWithin(m_program.at(cast.type), m_program);
    }

    std::optional<unsigned> accessedField(uint32_t index, const Value& access) const
    {
        auto& object = m_candidates[index];
        if (object.isClosure)
            return std::nullopt;
        auto& layout = layoutOf(object);
        std::optional<uint32_t> field;
        bool isByName = false;
        switch (access.opcode) {
        case Opcode::GetField:
        case Opcode::PutField:
            if (access.layout() == m_function.at(object.allocation).layout())
                field = access.field();
            else if (isObjectLayout(layout.kind))
                field = layout.fieldNamed(m_program.at(access.layout()).fields[access.field()].name);
            break;
        case Opcode::GetById:
        case Opcode::PutById:
            if (access.immediates[1] == Format::none && isObjectLayout(layout.kind) && !mayInheritAtBirth(m_program, m_function, object.allocation, access.string()))
                field = layout.fieldNamed(access.string());
            isByName = true;
            break;
        default:
            break;
        }
        if (!field || !((isByName ? object.initialFields | object.creatableFields : object.initialFields) & 1ull << *field))
            return std::nullopt;
        return *field;
    }

    bool canHoldVirtually(uint32_t holder, const Value& store) const
    {
        if (!isVirtual(holder) || m_policy != AllocationSinkingPolicy::AllowMaterialization)
            return false;
        auto field = accessedField(holder, store);
        if (!field || m_candidates[holder].multiplyStoredFields & 1ull << *field)
            return false;
        return m_program.at(m_program.types.undefined).isWithin(m_program.at(fieldType(m_candidates[holder], *field)), m_program);
    }

    ValueID closureLoadedBy(ValueID read) const
    {
        while (read.index() < m_loadedValue.size() && !m_loadedValue[read.index()] && m_function.at(read).opcode == Opcode::Cast && m_function.at(read).castReason() == CastReason::Upcast)
            read = m_function.at(read).child(0);
        ValueID closure = read.index() < m_loadedValue.size() ? m_loadedValue[read.index()] : ValueID();
        while (closure && m_function.at(closure).opcode == Opcode::Cast && m_function.at(closure).castReason() == CastReason::Upcast)
            closure = m_function.at(closure).child(0);
        return closure && m_function.at(closure).opcode == Opcode::NewClosure ? closure : ValueID();
    }

    void replaceWithBoolean(ValueID id, bool result)
    {
        m_knownBoolean[id.index()] = result ? KnownBoolean::True : KnownBoolean::False;
        if (m_mode != Mode::Rewrite)
            return;
        auto& value = m_function.at(id);
        value.opcode = Opcode::Boolean;
        value.children = { };
        value.immediates = { result, Format::none };
        m_newValues.append(id);
    }

    bool execute(ValueID id)
    {
        Opcode opcode = m_function.at(id).opcode;
        ValueID first = m_function.at(id).child(0);
        ValueID second = m_function.at(id).child(1);
        m_knownBoolean[id.index()] = KnownBoolean::Unknown;
        m_loadedValue[id.index()] = ValueID();
        switch (opcode) {
        case Opcode::NewObjectWithShape:
            if (uint32_t object = m_candidateForValue[id.index()]; object != none && !m_candidates[object].isAbandoned) {
                setState(object, { Status::Virtual, 0 });
                if (m_mode == Mode::Rewrite)
                    m_fieldValues[object].fill(ValueID(), layoutOf(m_candidates[object]).fields.size());
                return false;
            }
            break;
        case Opcode::NewClosure:
            if (uint32_t object = m_candidateForValue[id.index()]; object != none && !m_candidates[object].isAbandoned) {
                setState(object, { Status::Virtual, 0 });
                return false;
            }
            break;
        case Opcode::Cast:
            m_alias[id.index()] = none;
            if (isTracked(aliasOf(first)) && isTransparentCast(m_function.at(id))) {
                m_alias[id.index()] = aliasOf(first);
                return false;
            }
            break;
        case Opcode::AssertNotAllocated:
            if (uint32_t object = aliasOf(first); object != none && m_state[object].status != Status::Materialized)
                return false;
            break;
        case Opcode::PutField:
        case Opcode::PutById:
            if (auto& store = m_function.at(id); opcode == Opcode::PutField && m_program.at(store.layout()).reuseVariable && store.field() + 1 == m_program.at(store.layout()).fields.size()) {
                if (uint32_t object = aliasOf(first); object != none && m_state[object].status != Status::Materialized)
                    return false;
            }
            if (isVirtual(aliasOf(second)) && !canHoldVirtually(aliasOf(first), m_function.at(id)))
                escape(aliasOf(second), id);
            if (uint32_t object = aliasOf(first); isVirtual(object)) {
                auto field = accessedField(object, m_function.at(id));
                bool creates = field && !((m_candidates[object].initialFields | m_state[object].stored) & 1ull << *field);
                if (field && typeOf(second).isWithin(m_program.at(fieldType(m_candidates[object], *field)), m_program) && !(creates && m_state[object].created >> 56)) {
                    m_state[object].stored |= 1ull << *field;
                    if (creates)
                        m_state[object].created = m_state[object].created << 8 | (*field + 1);
                    if (m_mode == Mode::Rewrite)
                        m_fieldValues[object][*field] = isVirtual(aliasOf(second)) ? ValueID() : resolve(second);
                    return false;
                }
                if (isVirtual(aliasOf(second)))
                    escape(aliasOf(second), id);
                if (m_mode == Mode::Count)
                    m_candidates[object].abandonReason = "a store that has no rule"_s;
            } else if (isMaybeMaterialized(object))
                disallowMaybeMaterialized(object, "every way, stored to behind a merge"_s);
            break;
        case Opcode::GetField:
        case Opcode::GetById:
            m_alias[id.index()] = none;
            if (uint32_t object = aliasOf(first); isTracked(object)) {
                auto field = accessedField(object, m_function.at(id));
                if (isVirtual(object) && field && !((m_candidates[object].initialFields | m_state[object].stored) & 1ull << *field) && m_program.at(m_program.types.undefined).isWithin(m_program.at(m_function.at(id).type), m_program)) {
                    if (m_mode == Mode::Rewrite) {
                        ValueID undefined = emitUndefined(m_candidates[object], m_newValues);
                        auto& value = m_function.at(id);
                        value.opcode = Opcode::Cast;
                        value.children = { undefined, ValueID(), ValueID() };
                        value.immediates = { Format::none, static_cast<uint32_t>(CastReason::Upcast) };
                        m_newValues.append(id);
                    }
                    return false;
                }
                if (isVirtual(object) && field && m_state[object].stored & 1ull << *field && isVirtual(candidateInField(object, *field)) && typeOf(m_candidates[candidateInField(object, *field)].allocation).isWithin(m_program.at(m_function.at(id).type), m_program)) {
                    m_alias[id.index()] = candidateInField(object, *field);
                    return false;
                }
                if (isMaybeMaterialized(object) && field && !(m_state[object].stored & 1ull << *field))
                    disallowMaybeMaterialized(object, m_candidates[object].speculativelyLoadableFields & 1ull << *field ? "every way, read behind a merge and a write"_s : "every way, read behind a merge of a field that may not be read early"_s);
                if (field && m_state[object].stored & 1ull << *field && !(isVirtual(object) && isVirtual(candidateInField(object, *field))) && m_program.at(fieldType(m_candidates[object], *field)).isWithin(m_program.at(m_function.at(id).type), m_program)) {
                    if (isMaybeMaterialized(object))
                        m_candidates[object].fieldsLoadedWhileMaybeMaterialized |= 1ull << *field;
                    else if (!(m_candidates[object].multiplyStoredFields & 1ull << *field))
                        m_loadedValue[id.index()] = m_candidates[object].singleStoredValue[*field];
                    if (m_mode == Mode::Rewrite) {
                        auto& value = m_function.at(id);
                        value.opcode = Opcode::Cast;
                        value.children = { isMaybeMaterialized(object) ? m_fieldValues[object][*field] : fieldValue(object, *field), ValueID(), ValueID() };
                        value.immediates = { Format::none, static_cast<uint32_t>(CastReason::Upcast) };
                        m_newValues.append(id);
                    }
                    return false;
                }
            }
            break;
        case Opcode::StrictEq:
            if (isVirtual(aliasOf(first)) || isVirtual(aliasOf(second)) || (isMaybeMaterialized(aliasOf(first)) && aliasOf(first) == aliasOf(second))) {
                replaceWithBoolean(id, aliasOf(first) == aliasOf(second));
                return false;
            }
            break;
        case Opcode::IsClosureOf:
            if (uint32_t object = aliasOf(first); isVirtual(object)) {
                replaceWithBoolean(id, m_function.at(id).immediates[1] != closureIsTheOneThatConstructs && m_candidates[object].isClosure && m_function.at(m_candidates[object].allocation).function() == m_function.at(id).function());
                return false;
            }
            if (ValueID closure = closureLoadedBy(first)) {
                bool isOfFunction = m_function.at(closure).function() == m_function.at(id).function();
                if (!isOfFunction || (m_function.at(id).immediates[1] != closureIsAsMade && m_function.at(id).immediates[1] != closureIsTheOneThatConstructs)) {
                    replaceWithBoolean(id, isOfFunction);
                    return false;
                }
            }
            break;
        case Opcode::GetCapture:
            m_alias[id.index()] = none;
            if (uint32_t object = aliasOf(first); isVirtual(object) && m_candidates[object].isClosure) {
                auto& closure = m_function.at(m_candidates[object].allocation);
                if (closure.function() == m_function.at(id).function() && m_function.at(id).immediates[1] < closure.arguments.size()) {
                    ValueID capture = closure.arguments[m_function.at(id).immediates[1]];
                    uint32_t captured = aliasOf(capture);
                    if (typeOf(captured == none ? capture : m_candidates[captured].allocation).isWithin(m_program.at(m_function.at(id).type), m_program)) {
                        if (isVirtual(captured)) {
                            m_alias[id.index()] = captured;
                            return false;
                        }
                        if (m_mode == Mode::Rewrite) {
                            ValueID resolved = resolve(capture);
                            auto& value = m_function.at(id);
                            value.opcode = Opcode::Cast;
                            value.children = { resolved, ValueID(), ValueID() };
                            value.immediates = { Format::none, static_cast<uint32_t>(CastReason::Upcast) };
                            m_newValues.append(id);
                        }
                        return false;
                    }
                }
            }
            if (ValueID closure = closureLoadedBy(first); closure && m_function.at(closure).function() == m_function.at(id).function() && m_function.at(id).immediates[1] < m_function.at(closure).arguments.size()) {
                ValueID capture = m_function.at(closure).arguments[m_function.at(id).immediates[1]];
                uint32_t captured = aliasOf(capture);
                if ((captured == none || m_state[captured].status == Status::Materialized) && typeOf(captured == none ? capture : m_candidates[captured].allocation).isWithin(m_program.at(m_function.at(id).type), m_program)) {
                    if (m_mode == Mode::Rewrite) {
                        ValueID resolved = resolve(capture);
                        auto& value = m_function.at(id);
                        value.opcode = Opcode::Cast;
                        value.children = { resolved, ValueID(), ValueID() };
                        value.immediates = { Format::none, static_cast<uint32_t>(CastReason::Upcast) };
                        m_newValues.append(id);
                    }
                    return false;
                }
            }
            break;
        case Opcode::LogicalNot:
            if (first.index() < m_knownBoolean.size() && m_knownBoolean[first.index()] != KnownBoolean::Unknown) {
                replaceWithBoolean(id, m_knownBoolean[first.index()] == KnownBoolean::False);
                return false;
            }
            break;
        case Opcode::IsUndefinedOrNull:
            if (isTracked(aliasOf(first))) {
                replaceWithBoolean(id, false);
                return false;
            }
            break;
        case Opcode::ToBoolean:
            if (isTracked(aliasOf(first))) {
                replaceWithBoolean(id, true);
                return false;
            }
            break;
        case Opcode::TypeOfIs:
            if (isTracked(aliasOf(first))) {
                replaceWithBoolean(id, m_function.at(id).typeofType() == (m_candidates[aliasOf(first)].isClosure ? TypeofType::Function : TypeofType::Object));
                return false;
            }
            break;
        default:
            break;
        }
        escapeOperands(id);
        if (m_mayWriteFields.get(id.index())) {
            for (uint32_t object : m_tracked) {
                if (isMaybeMaterialized(object))
                    m_state[object].stored = 0;
            }
        }
        if (m_mode == Mode::Rewrite)
            m_newValues.append(id);
        return true;
    }

    void processBlock(unsigned b)
    {
        m_currentBlock = b;
        restoreState(m_head[b]);
        ValuesOfBlock valuesBeforeRewriting;
        if (m_mode == Mode::Rewrite) {
            valuesBeforeRewriting = m_function.blocks[b].values;
            m_newValues.clear();
            loadHeadParameters(b);
        }
        auto& values = m_mode == Mode::Rewrite ? valuesBeforeRewriting : m_function.blocks[b].values;
        bool lastValueIsUnchanged = true;
        if (m_mode != Mode::Rewrite && !m_holdsObjects.quickGet(b)) {
            for (ValueID id : m_valuesThatActWithoutObjects.span().subspan(m_firstThatActsWithoutObjects[b], m_firstThatActsWithoutObjects[b + 1] - m_firstThatActsWithoutObjects[b])) {
                bool isUnchanged = execute(id);
                if (id == values.last())
                    lastValueIsUnchanged = isUnchanged;
            }
        } else {
            for (ValueID id : values)
                lastValueIsUnchanged = execute(id);
        }
        bool isHandlerEdgeDead = hasInvokedValue(b) && !lastValueIsUnchanged;
        m_isHandlerEdgeDead.set(b, isHandlerEdgeDead);
        TerminatorKind kind = m_function.blocks[b].terminator.kind;
        if (ValueID value = m_function.blocks[b].terminator.value; isVirtual(aliasOf(value)))
            escape(aliasOf(value), ValueID(), kind == TerminatorKind::Return ? "Return"_s : kind == TerminatorKind::Throw ? "Throw"_s : "a condition"_s);
        else if (isMaybeMaterialized(aliasOf(value)))
            disallowMaybeMaterialized(aliasOf(value), "every way, needed behind a merge"_s);
        m_deadTarget[b] = noTarget;
        if (ValueID condition = m_function.blocks[b].terminator.value; kind == TerminatorKind::Branch && condition.index() < m_knownBoolean.size() && m_knownBoolean[condition.index()] != KnownBoolean::Unknown)
            m_deadTarget[b] = m_knownBoolean[condition.index()] == KnownBoolean::True;
        if (m_mode == Mode::Analyze) {
            m_tail[b] = saveState();
            return;
        }
        if (m_mode != Mode::Rewrite)
            return;
        auto& terminator = m_function.blocks[b].terminator;
        if (terminator.value)
            terminator.value = resolve(terminator.value);
        if (isHandlerEdgeDead) {
            terminator.kind = TerminatorKind::Jump;
            terminator.targets.shrink(1);
        }
        if (m_deadTarget[b] != noTarget) {
            terminator.kind = TerminatorKind::Jump;
            terminator.value = { };
            terminator.targets.removeAt(m_deadTarget[b]);
            m_deadTarget[b] = noTarget;
        }
    }

    struct EdgeMaterialization {
        uint32_t object;
        bool isForParameter;
    };

    Vector<EdgeMaterialization, 2> materializationsForEdge(unsigned b, unsigned t) const
    {
        Vector<EdgeMaterialization, 2> result;
        auto& target = m_function.blocks[b].terminator.targets[t];
        auto add = [&](uint32_t object, bool isForParameter) {
            if (isVirtual(object) && !result.containsIf([&](const EdgeMaterialization& other) { return other.object == object; }))
                result.append(EdgeMaterialization { object, isForParameter });
        };
        auto& parameters = parametersOf(target.block.index());
        for (unsigned i = 0; i < target.arguments.size() && i < parameters.size(); ++i) {
            if (uint32_t object = aliasOf(target.arguments[i]); object != none && m_alias[parameters[i].index()] != object)
                add(object, true);
        }
        for (auto& entry : m_head[target.block.index()]) {
            if (entry.state.status == Status::Materialized)
                add(entry.object, false);
        }
        return result;
    }

    const CompactList<ValueID>& parametersOf(unsigned b) const { return m_function.blocks[b].parameters; }
    static ASCIILiteral edgeMaterializationReason(const EdgeMaterialization& one) { return one.isForParameter ? "a block parameter"_s : "a merge"_s; }
    void didMaterializeOnEdge(const EdgeMaterialization& one, unsigned to)
    {
        (one.isForParameter ? m_candidates[one.object].isMaterializedForParameter : m_candidates[one.object].isMaterializedForMerge) = true;
        if (!one.isForParameter && m_candidates[one.object].mergeBlock == UINT_MAX)
            m_candidates[one.object].mergeBlock = to;
    }

    std::optional<Vector<unsigned>> collectTail(unsigned first) const
    {
        Vector<unsigned> rest { first };
        BitVector seen(m_function.blocks.size());
        seen.set(first);
        unsigned size = 0;
        for (unsigned next = 0; next < rest.size(); ++next) {
            auto& block = m_function.blocks[rest[next]];
            size += block.values.size() + 1;
            if (size > maxTailSize)
                return std::nullopt;
            for (auto& target : block.terminator.targets) {
                if (target.block.index() == first)
                    return std::nullopt;
                if (!seen.set(target.block.index()))
                    rest.append(target.block.index());
            }
        }
        return rest;
    }

    void duplicateTail(const Vector<unsigned>& rest, const Vector<Edge>& edges)
    {
        Vector<uint32_t> copyOfBlock;
        copyOfBlock.fill(none, m_function.blocks.size());
        Vector<uint32_t> copyOfValue;
        copyOfValue.fill(none, m_function.values.size());
        for (unsigned i = 0; i < rest.size(); ++i) {
            copyOfBlock[rest[i]] = m_function.blocks.size() + i;
            auto copy = [&](ValueID id) {
                copyOfValue[id.index()] = append(Value(m_function.at(id))).index();
            };
            for (ValueID id : m_function.blocks[rest[i]].parameters)
                copy(id);
            for (ValueID id : m_function.blocks[rest[i]].values) {
                if (m_function.at(id).opcode != Opcode::AssertNotAllocated)
                    copy(id);
            }
        }
        auto inCopy = [&](ValueID id) {
            return id && copyOfValue[id.index()] != none ? ValueID(copyOfValue[id.index()]) : id;
        };
        for (unsigned b : rest) {
            Block block;
            for (ValueID id : m_function.blocks[b].parameters) {
                m_function.at(inCopy(id)).immediates = { copyOfBlock[b], static_cast<uint32_t>(block.parameters.size()) };
                block.parameters.append(inCopy(id));
            }
            for (ValueID id : m_function.blocks[b].values) {
                if (copyOfValue[id.index()] == none)
                    continue;
                m_function.at(inCopy(id)).forEachChild([&](ValueID& child) {
                    child = inCopy(child);
                });
                block.values.append(inCopy(id));
            }
            block.terminator = m_function.blocks[b].terminator;
            block.terminator.forEachChild([&](ValueID& child) {
                child = inCopy(child);
            });
            for (auto& target : block.terminator.targets)
                target.block = BlockID(copyOfBlock[target.block.index()]);
            m_function.blocks.append(WTF::move(block));
        }
        for (auto& edge : edges)
            m_function.blocks[edge.from].terminator.targets[edge.target].block = BlockID(copyOfBlock[rest[0]]);
    }

    bool duplicateTailForMerge()
    {
        bool didAttempt = false;
        Vector<std::pair<uint32_t, unsigned>, 2> attemptedGroups;
        for (unsigned i = 0; i < m_candidates.size(); ++i) {
            if (!m_candidates[i].isAbandoned || !m_candidates[i].isMaterializedForMerge || m_candidates[i].mergeBlock == UINT_MAX)
                continue;
            unsigned merge = m_candidates[i].mergeBlock;
            uint32_t group = m_candidates[i].group;
            if (group != none && attemptedGroups.contains(std::pair { group, merge }))
                continue;
            auto rest = collectTail(merge);
            if (!rest)
                continue;
            didAttempt = true;
            attemptedGroups.append({ group, merge });
            Vector<Candidate> saved = m_candidates;
            for (unsigned j = 0; j < m_candidates.size(); ++j) {
                m_candidates[j].isAbandoned = j != i && (group == none || m_candidates[j].group != group);
                if (!m_candidates[j].isAbandoned)
                    m_candidates[j].abandonReason = { };
            }
            auto& object = m_candidates[i];
            object.allowsMaybeMaterialized = false;
            m_isSpeculating = true;
            analyze();
            Vector<Edge> built;
            bool hasVirtualPredecessor = false;
            for (auto& edge : m_predecessors[merge]) {
                if (!m_isReached.get(edge.from) || isDeadEdge(edge))
                    continue;
                auto* entry = find(m_tail[edge.from], i);
                if (entry && entry->state.status == Status::Materialized)
                    built.append(edge);
                else
                    hasVirtualPredecessor = true;
            }
            bool pays = false;
            if (!built.isEmpty() && hasVirtualPredecessor) {
                m_ignoredEdges = built;
                for (auto& edge : built)
                    m_ignoredEdgeSet.add(static_cast<uint64_t>(edge.from) << 32 | edge.target);
                object.allowsMaybeMaterialized = group == none;
                do {
                    analyze();
                    countMaterializations();
                } while (abandonUnprofitableCandidates());
                pays = !object.isAbandoned;
                m_ignoredEdges.clear();
                m_ignoredEdgeSet.clear();
            }
            m_isSpeculating = false;
            m_candidates = WTF::move(saved);
            if (pays) {
                duplicateTail(*rest, built);
                return true;
            }
        }
        if (didAttempt) {
            analyze();
            countMaterializations();
        }
        return false;
    }

    void processEdges(unsigned b)
    {
        if (m_mode == Mode::Count && m_tracked.isEmpty())
            return;
        TerminatorKind kind = m_function.blocks[b].terminator.kind;
        unsigned numberOfTargets = m_function.blocks[b].terminator.targets.size();
        if (kind == TerminatorKind::Invoke && m_isHandlerEdgeDead.get(b))
            numberOfTargets = 1;
        if (!numberOfTargets) {
            for (uint32_t object : m_tracked) {
                if (isTracked(object))
                    m_candidates[object].diesVirtual = true;
            }
        }
        if (numberOfTargets > 1 && kind != TerminatorKind::Invoke && m_ignoredEdges.isEmpty() && m_deadTarget[b] == noTarget) {
            auto everywhere = materializationsForEdge(b, 0);
            for (unsigned t = 1; t < numberOfTargets; ++t) {
                auto here = materializationsForEdge(b, t);
                everywhere.removeAllMatching([&](const EdgeMaterialization& one) {
                    return !here.containsIf([&](const EdgeMaterialization& other) { return other.object == one.object; });
                });
            }
            for (auto& one : everywhere) {
                didMaterializeOnEdge(one, m_function.blocks[b].terminator.targets[0].block.index());
                escape(one.object, ValueID(), edgeMaterializationReason(one));
            }
        }
        for (unsigned t = 0; t < numberOfTargets; ++t) {
            if (isIgnoredEdge(b, t) || m_deadTarget[b] == t)
                continue;
            unsigned to = m_function.blocks[b].terminator.targets[t].block.index();
            auto materializations = materializationsForEdge(b, t);
            State before = saveState();
            if (m_mode == Mode::Count) {
                m_isOnHandlerEdge = kind == TerminatorKind::Invoke && t;
                for (auto& one : materializations) {
                    didMaterializeOnEdge(one, to);
                    if (isVirtual(one.object))
                        materialize(one.object, ValueID(), edgeMaterializationReason(one), m_newValues, numberOfTargets == 1);
                }
                m_isOnHandlerEdge = false;
                for (uint32_t object : m_tracked) {
                    if (isTracked(object) && !find(m_head[to], object))
                        m_candidates[object].diesVirtual = true;
                }
                auto& target = m_function.blocks[b].terminator.targets[t];
                for (unsigned i = 0; i < target.arguments.size() && i < parametersOf(to).size(); ++i) {
                    if (uint32_t object = aliasOf(target.arguments[i]); isMaybeMaterialized(object) && m_alias[parametersOf(to)[i].index()] != object)
                        disallowMaybeMaterialized(object, "every way, needed behind a merge"_s);
                }
                for (auto& entry : m_head[to]) {
                    if (entry.state.status == Status::MaybeMaterialized && m_state[entry.object].status == Status::Materialized && kind == TerminatorKind::Invoke && t)
                        disallowMaybeMaterialized(entry.object, "every way, read on the way to a handler"_s);
                }
                restoreState(before);
                continue;
            }
            Vector<ValueID> onEdge;
            for (auto& one : materializations) {
                if (isVirtual(one.object))
                    materialize(one.object, ValueID(), { }, numberOfTargets == 1 ? m_newValues : onEdge, true);
            }
            Arguments arguments;
            {
                auto& target = m_function.blocks[b].terminator.targets[t];
                auto& parameters = parametersOf(to);
                for (unsigned i = 0; i < target.arguments.size(); ++i) {
                    if (i < parameters.size() && m_alias[parameters[i].index()] != none)
                        continue;
                    arguments.append(resolve(target.arguments[i]));
                }
            }
            for (auto& entry : m_head[to]) {
                if (entry.state.status == Status::Materialized) {
                    RELEASE_ASSERT(m_state[entry.object].status == Status::Materialized);
                    arguments.append(m_materialized[entry.object]);
                    continue;
                }
                bool wasMaterialized = find(before, entry.object)->state.status == Status::Materialized;
                RELEASE_ASSERT(entry.state.status == Status::MaybeMaterialized || m_state[entry.object] == entry.state);
                uint64_t passed = fieldsPassedToHead(entry);
                for (unsigned field = 0; field < maxFields; ++field) {
                    if (passed & 1ull << field)
                        arguments.append(wasMaterialized ? emitLoad(entry.object, field, numberOfTargets == 1 ? m_newValues : onEdge) : m_fieldValues[entry.object][field]);
                }
            }
            m_function.blocks[b].terminator.targets[t].arguments = WTF::move(arguments);
            if (!onEdge.isEmpty())
                m_function.at(splitEdge(m_function, BlockID(b), t)).values = WTF::move(onEdge);
            restoreState(before);
        }
        if (m_mode == Mode::Rewrite)
            m_function.blocks[b].values = m_newValues;
    }

    uint64_t fieldsPassedToHead(const Entry& entry) const
    {
        return entry.state.status == Status::MaybeMaterialized ? entry.state.stored & m_candidates[entry.object].fieldsLoadedWhileMaybeMaterialized : entry.state.stored & ~fieldsHoldingCandidates(entry.object);
    }

    void loadHeadParameters(unsigned b)
    {
        unsigned next = 0;
        for (auto& entry : m_head[b]) {
            if (entry.state.status == Status::Materialized) {
                m_materialized[entry.object] = m_headParameters[b][next++];
                continue;
            }
            if (m_candidates[entry.object].isClosure)
                continue;
            m_fieldValues[entry.object].fill(ValueID(), layoutOf(m_candidates[entry.object]).fields.size());
            uint64_t passed = fieldsPassedToHead(entry);
            for (unsigned field = 0; field < maxFields; ++field) {
                if (passed & 1ull << field)
                    m_fieldValues[entry.object][field] = m_headParameters[b][next++];
            }
        }
    }

    void rewrite()
    {
        for (auto& object : m_candidates) {
            if (object.isAbandoned)
                continue;
            noteAllocation(m_program, m_function, m_function.at(object.allocation), object.materializationCount ? AllocationOutcome::BuiltLate : AllocationOutcome::Removed, object.materializationCount, object.escapeSite ? &m_function.at(object.escapeSite) : nullptr, object.escapeReason);
        }
        m_mode = Mode::Rewrite;
        m_fieldValues.grow(m_candidates.size());
        m_materialized.fill(ValueID(), m_candidates.size());
        m_headParameters.grow(m_function.blocks.size());
        for (unsigned b : m_blockOrder) {
            if (!m_isReached.get(b))
                continue;
            auto add = [&](const Candidate& object, TypeID type) {
                Value parameter;
                parameter.opcode = Opcode::Parameter;
                parameter.position = m_function.at(object.allocation).position;
                parameter.inlineFrame = m_function.at(object.allocation).inlineFrame;
                parameter.type = type;
                parameter.rep = repOf(m_program.at(type));
                m_headParameters[b].append(append(WTF::move(parameter)));
            };
            for (auto& entry : m_head[b]) {
                auto& object = m_candidates[entry.object];
                if (entry.state.status == Status::Materialized) {
                    add(object, m_function.at(object.allocation).type);
                    continue;
                }
                uint64_t passed = fieldsPassedToHead(entry);
                for (unsigned field = 0; field < maxFields; ++field) {
                    if (passed & 1ull << field)
                        add(object, fieldType(object, field));
                }
            }
        }
        unsigned numberOfValues = m_alias.size();
        m_alias.grow(m_function.values.size());
        for (unsigned v = numberOfValues; v < m_alias.size(); ++v)
            m_alias[v] = none;
        for (unsigned b : m_blockOrder) {
            if (!m_isReached.get(b))
                continue;
            processBlock(b);
            processEdges(b);
        }
        for (unsigned b : m_blockOrder) {
            if (!m_isReached.get(b)) {
                m_function.blocks[b] = Block { };
                continue;
            }
            auto& parameters = m_function.blocks[b].parameters;
            parameters.removeAllMatching([&](ValueID parameter) {
                return m_alias[parameter.index()] != none;
            });
            parameters.appendVector(m_headParameters[b]);
            for (unsigned i = 0; i < parameters.size(); ++i)
                m_function.at(parameters[i]).immediates = { b, i };
        }
    }

    const Program& m_program;
    Function& m_function;
    AllocationSinkingPolicy m_policy;
    Mode m_mode { Mode::Analyze };
    unsigned m_currentBlock { 0 };
    Vector<Candidate> m_candidates;
    Vector<uint32_t> m_candidateForValue;
    Vector<uint32_t> m_alias;
    Vector<unsigned> m_blockOrder;
    Vector<Vector<Edge, 2>> m_predecessors;
    Vector<State> m_head;
    Vector<State> m_tail;
    BitVector m_isReached;
    BitVector m_isHandlerEdgeDead;
    Vector<ObjectState> m_state;
    Vector<uint32_t, 8> m_tracked;
    BitVector m_isTracked;
    Vector<Meet> m_meet;
    BitVector m_mayWriteFields;
    BitVector m_holdsObjects;
    BitVector m_isLedBackTo;
    Vector<ValueID> m_valuesThatActWithoutObjects;
    Vector<uint32_t> m_firstThatActsWithoutObjects;
    Vector<Edge> m_ignoredEdges;
    UncheckedKeyHashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_ignoredEdgeSet;
    Vector<unsigned> m_deadTarget;
    Vector<KnownBoolean> m_knownBoolean;
    Vector<ValueID> m_loadedValue;
    bool m_didDisallowMaybeMaterialized { false };
    bool m_isOnHandlerEdge { false };
    bool m_isSpeculating { false };
    Vector<Vector<ValueID>> m_fieldValues;
    Vector<ValueID> m_materialized;
    Vector<Vector<ValueID>> m_headParameters;
    Vector<ValueID> m_newValues;
};

} // anonymous namespace

bool sinkObjectAllocations(const Program& program, Function& function, AllocationSinkingPolicy policy)
{
    return ObjectAllocationSinking(program, function, policy).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
