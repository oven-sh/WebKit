/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include "AOTTypedStringAppend.h"
#include "AOTTypedThreads.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr unsigned noBlock = UINT_MAX;
constexpr unsigned noRegion = 0;
constexpr unsigned firstFreeState = 2;
constexpr unsigned maxPlanAttempts = 4;
constexpr unsigned minStoredParts = 8;
constexpr size_t maxStoredConstantLength = 256;
constexpr unsigned maxPlacementDepth = 64;
constexpr unsigned minPartsWithoutCalls = 8;

struct Point {
    unsigned block { noBlock };
    unsigned index { 0 };
};

struct Part {
    StringAppendPart kind { StringAppendPart::Value };
    ValueID value;

    friend bool operator==(const Part&, const Part&) = default;
};

using Parts = Vector<Part, 16>;

enum class Step : uint8_t { Begin, Append, Rename, Finish, Other };

struct Insertion {
    Point at;
    Step step { Step::Other };
    ValueID value;
    unsigned region { 0 };
    unsigned stateBefore { 0 };
    unsigned stateAfter { 0 };
    unsigned guardedSite { UINT_MAX };
};

struct GuardedSite {
    ValueID guard;
    ValueID result;
    ValueID fastResult;
    Vector<ValueID, 4> slowPath;
};

enum class KnownStart : uint8_t { None, TopOfStack, RegionStart };

struct Context {
    unsigned region { 0 };
    unsigned state { 0 };
    bool needsBegin { false };
    KnownStart knownStart { KnownStart::None };
    Parts pending;
};

struct BufferedValue {
    unsigned state { 0 };
    unsigned region { 0 };
    unsigned contextState { 0 };
    bool contextNeedsBegin { false };
    Parts contextPending;
    KnownStart knownStart { KnownStart::None };
};

struct Checkpoint {
    size_t insertions;
    size_t values;
    size_t dead;
    size_t bufferedValues;
    size_t accounted;
    size_t conversions;
    size_t guardedSites;
    unsigned nextState;
    unsigned loopCount;
};

bool isStringPreservingCast(const Program& program, const Value& value)
{
    if (value.opcode != Opcode::Cast || !(program.at(value.type).primitives & Primitive::String))
        return false;
    switch (value.castReason()) {
    case CastReason::Store:
    case CastReason::As:
    case CastReason::NonNull:
    case CastReason::Flow:
    case CastReason::Upcast:
        return true;
    default:
        return false;
    }
}

BitVector reachableBlocks(const Function& function)
{
    BitVector isReachable(function.blocks.size());
    Vector<unsigned, 32> worklist { 0u };
    isReachable.set(0);
    while (!worklist.isEmpty()) {
        for (auto& target : function.blocks[worklist.takeLast()].terminator.targets) {
            if (!isReachable.set(target.block.index()))
                worklist.append(target.block.index());
        }
    }
    return isReachable;
}

BitVector computeKnownStrings(const Program& program, const Function& function, const BitVector& isReachable, const BitVector& returnsStrings)
{
    BitVector isString(function.values.size());
    Vector<ValueID> doubtful;
    auto consider = [&](ValueID id) {
        auto& value = function.at(id);
        bool isCertain = program.at(value.type).isString() || value.opcode == Opcode::String || value.opcode == Opcode::Strcat || value.opcode == Opcode::ToString
            || (value.opcode == Opcode::CallDirect && !value.isJumpToContinuation() && !value.constructs && returnsStrings.get(value.function().index()));
        bool mayBeFoundOut = value.opcode == Opcode::Add || isStringPreservingCast(program, value) || (value.opcode == Opcode::Parameter && value.parameterBlock().index());
        if (!isCertain && !mayBeFoundOut)
            return;
        isString.set(id.index());
        if (!isCertain)
            doubtful.append(id);
    };
    Vector<Vector<unsigned, 2>> predecessors(function.blocks.size());
    for (unsigned b = 0; b < function.blocks.size(); ++b) {
        if (!isReachable.get(b))
            continue;
        for (ValueID id : function.blocks[b].parameters)
            consider(id);
        for (ValueID id : function.blocks[b].values)
            consider(id);
        for (auto& target : function.blocks[b].terminator.targets)
            predecessors[target.block.index()].append(b);
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (ValueID id : doubtful) {
            if (!isString.get(id.index()))
                continue;
            auto& value = function.at(id);
            bool holds = true;
            if (value.opcode == Opcode::Add)
                holds = isString.get(value.child(0).index()) || isString.get(value.child(1).index());
            else if (value.opcode == Opcode::Cast)
                holds = isString.get(value.child(0).index());
            else {
                unsigned block = value.parameterBlock().index();
                unsigned position = value.parameterIndex();
                for (unsigned predecessor : predecessors[block]) {
                    for (auto& target : function.blocks[predecessor].terminator.targets) {
                        if (target.block.index() == block)
                            holds &= position < target.arguments.size() && isString.get(target.arguments[position].index());
                    }
                }
            }
            if (!holds) {
                isString.clear(id.index());
                changed = true;
            }
        }
    }
    return isString;
}

BitVector findEscapeHTMLAliases(const Program& program)
{
    auto& builtins = Builtins::singleton();
    BuiltinName holder = builtins.nameOf(asBytes("Bun"_s.span8()));
    BuiltinName member = builtins.nameOf(asBytes("escapeHTML"_s.span8()));
    Vector<Vector<std::pair<unsigned, bool>>> storesByFunction(program.functions.size());
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = program.functions[i];
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& store = function.at(id);
                if (store.opcode != Opcode::PutModuleVariable)
                    continue;
                auto& read = function.at(program.underlyingValue(function, store.child(0)));
                bool isAlias = read.opcode == Opcode::GetById && program.builtinName(read.string()) == member;
                if (isAlias) {
                    auto& global = function.at(program.underlyingValue(function, read.child(0)));
                    isAlias = global.opcode == Opcode::GetGlobal && program.builtinName(global.string()) == holder;
                }
                storesByFunction[i].append({ store.variable().index(), isAlias });
            }
        }
    });
    BitVector aliases(program.variables.size());
    BitVector others(program.variables.size());
    for (auto& stores : storesByFunction) {
        for (auto [variable, isAlias] : stores)
            (isAlias ? aliases : others).set(variable);
    }
    aliases.exclude(others);
    return aliases;
}

class StringAppendPlanner {
public:
    StringAppendPlanner(const Program& program, FunctionID id, const Function& function, const BitVector& returnsStrings, const BitVector& escapeHTMLAliases)
        : m_program(program)
        , m_id(id)
        , m_function(function)
        , m_returnsStrings(returnsStrings)
        , m_escapeHTMLAliases(escapeHTMLAliases)
    {
    }

    bool run()
    {
        splitCriticalEdges();
        index();
        m_isString = computeKnownStrings(m_program, m_function, m_isReachable, m_returnsStrings);
        m_originalValueCount = m_function.values.size();
        m_accounted.fill(0, m_originalValueCount);
        countContinuationUses();
        findAccumulators();
        planRegions();
        if (!usesOnlyLiveValues()) {
            dataLogLnIf(Options::verboseAOTCompilation(), "AOT: dropped the string append plan of function ", m_id.index(), ": it uses a value it removes");
            return false;
        }
        return !m_insertions.isEmpty();
    }

    Function commit()
    {
        for (auto& value : m_newValues)
            m_function.values.append(WTF::move(value));
        m_newValues.clear();
        m_isCommitting = true;
        for (auto& [root, finish] : m_finishedRoots) {
            Value replacement(m_function.at(finish));
            auto& value = m_function.at(root);
            replacement.type = value.type;
            replacement.rep = value.rep;
            replacement.copyPredictionFrom(value);
            value = WTF::move(replacement);
        }
        if (!m_replacements.isEmpty()) {
            auto replace = [&](ValueID& operand) {
                for (auto& [from, to] : m_replacements) {
                    if (operand == from)
                        operand = to;
                }
            };
            for (auto& block : m_function.blocks) {
                for (ValueID id : block.values) {
                    if (!m_isDead.get(id.index()))
                        m_function.at(id).forEachChild(replace);
                }
                block.terminator.forEachChild(replace);
            }
        }
        std::ranges::stable_sort(m_insertions, [](const Insertion& a, const Insertion& b) {
            return a.at.block != b.at.block ? a.at.block < b.at.block : a.at.index < b.at.index;
        });
        mergeAdjacentAppends();
        size_t next = 0;
        unsigned originalBlockCount = m_function.blocks.size();
        for (unsigned b = 0; b < originalBlockCount; ++b) {
            auto& block = m_function.blocks[b];
            bool changes = next < m_insertions.size() && m_insertions[next].at.block == b;
            for (ValueID id : block.values)
                changes |= m_isDead.get(id.index());
            if (!changes)
                continue;
            BlockValues values;
            Vector<std::pair<BlockValues, unsigned>, 2> cutOff;
            auto insertUpTo = [&](unsigned index) {
                for (; next < m_insertions.size() && m_insertions[next].at.block == b && m_insertions[next].at.index <= index; ++next) {
                    auto& insertion = m_insertions[next];
                    if (insertion.guardedSite != UINT_MAX) {
                        values.append(m_guardedSites[insertion.guardedSite].guard);
                        cutOff.append({ std::exchange(values, { }), insertion.guardedSite });
                        continue;
                    }
                    if (insertion.step == Step::Rename || !insertion.value)
                        continue;
                    if (m_finishedRoots.containsIf([&](auto& entry) { return entry.second == insertion.value; }))
                        continue;
                    values.append(insertion.value);
                }
            };
            for (unsigned i = 0; i < block.values.size(); ++i) {
                insertUpTo(i);
                ValueID id = block.values[i];
                if (m_isDead.get(id.index()))
                    continue;
                values.append(id);
            }
            insertUpTo(UINT_MAX);
            if (cutOff.isEmpty()) {
                block.values = WTF::move(values);
                continue;
            }
            Terminator terminator = std::exchange(block.terminator, { });
            unsigned current = b;
            for (unsigned k = 0; k < cutOff.size(); ++k) {
                auto& site = m_guardedSites[cutOff[k].second];
                unsigned fastPath = m_function.blocks.size();
                m_function.blocks.grow(fastPath + 3);
                auto& head = m_function.blocks[current];
                head.values = WTF::move(cutOff[k].first);
                head.terminator.kind = TerminatorKind::Branch;
                head.terminator.value = site.guard;
                head.terminator.targets.append(Target { BlockID(fastPath), { } });
                head.terminator.targets.append(Target { BlockID(fastPath + 1), { } });
                m_function.blocks[fastPath + 1].values.append(site.slowPath.span());
                for (unsigned path : { fastPath, fastPath + 1 }) {
                    m_function.blocks[path].terminator.kind = TerminatorKind::Jump;
                    m_function.blocks[path].terminator.targets.append(Target { BlockID(fastPath + 2), Arguments { path == fastPath ? site.fastResult : site.slowPath.last() } });
                }
                current = fastPath + 2;
                m_function.blocks[current].parameters.append(site.result);
                m_function.at(site.result).immediates = { current, 0 };
            }
            m_function.blocks[current].values = WTF::move(values);
            m_function.blocks[current].terminator = WTF::move(terminator);
        }
        removeBufferedPhis();
        return WTF::move(m_function);
    }

private:
    static bool isConstant(Opcode opcode)
    {
        switch (opcode) {
        case Opcode::String:
        case Opcode::Integer:
        case Opcode::Double:
        case Opcode::Boolean:
        case Opcode::Undefined:
        case Opcode::Null:
            return true;
        default:
            return false;
        }
    }

    void mergeAdjacentAppends()
    {
        auto partCount = [](uint64_t kinds) {
            unsigned count = 0;
            for (; kinds & ((1u << stringAppendPartBits) - 1); kinds >>= stringAppendPartBits)
                ++count;
            return count;
        };
        size_t previous = notFound;
        for (size_t k = 0; k < m_insertions.size(); ++k) {
            auto& insertion = m_insertions[k];
            if (previous != notFound) {
                auto& earlier = m_insertions[previous];
                bool isAdjacent = earlier.at.block == insertion.at.block;
                auto& values = m_function.blocks[earlier.at.block].values;
                for (unsigned i = earlier.at.index; isAdjacent && i < insertion.at.index; ++i)
                    isAdjacent = m_isDead.get(values[i].index()) || isConstant(m_function.at(values[i]).opcode);
                if (!isAdjacent)
                    previous = notFound;
            }
            if (insertion.guardedSite != UINT_MAX || insertion.step == Step::Begin || insertion.step == Step::Finish || (insertion.step == Step::Other && insertion.value && !isConstant(m_function.at(insertion.value).opcode))) {
                previous = notFound;
                continue;
            }
            if (insertion.step != Step::Append)
                continue;
            if (previous != notFound && m_insertions[previous].region == insertion.region) {
                auto& earlier = m_insertions[previous];
                const Value first(m_function.at(earlier.value));
                auto& second = m_function.at(insertion.value);
                constexpr uint64_t partsMask = (1ull << stringAppendRegionShift) - 1;
                uint64_t firstKinds = static_cast<uint64_t>(m_function.at(first.arguments[0]).integer()) & partsMask;
                uint64_t secondKinds = static_cast<uint64_t>(m_function.at(second.arguments[0]).integer()) & partsMask;
                if (partCount(firstKinds) + partCount(secondKinds) <= maxStringAppendParts) {
                    m_function.at(second.arguments[0]).setBits(firstKinds | secondKinds << (stringAppendPartBits * partCount(firstKinds)) | static_cast<uint64_t>(insertion.region) << stringAppendRegionShift);
                    Arguments arguments;
                    arguments.append(second.arguments[0]);
                    arguments.append(first.arguments.span().subspan(1));
                    arguments.append(second.arguments.span().subspan(1));
                    second.arguments = WTF::move(arguments);
                    for (auto& other : m_insertions) {
                        if (other.value == first.arguments[0])
                            other.value = { };
                    }
                    earlier.value = { };
                }
            }
            previous = k;
        }
    }

    const Value& at(ValueID id) const
    {
        if (id.index() < m_function.values.size())
            return m_function.at(id);
        return m_newValues[id.index() - m_function.values.size()];
    }

    const Type& typeOf(ValueID id) const { return m_program.at(at(id).type); }

    void splitCriticalEdges()
    {
        unsigned count = m_function.blocks.size();
        for (unsigned b = 0; b < count; ++b) {
            auto kind = m_function.blocks[b].terminator.kind;
            if (m_function.blocks[b].terminator.targets.size() < 2 || kind == TerminatorKind::Invoke || kind == TerminatorKind::Throw)
                continue;
            for (unsigned k = 0; k < m_function.blocks[b].terminator.targets.size(); ++k) {
                auto& target = m_function.blocks[b].terminator.targets[k];
                bool hasStringArgument = false;
                for (ValueID argument : target.arguments)
                    hasStringArgument |= !!(m_program.typeOf(m_function, argument).primitives & Primitive::String);
                if (hasStringArgument)
                    splitEdge(m_function, BlockID(b), k);
            }
        }
    }

    void index()
    {
        unsigned numberOfBlocks = m_function.blocks.size();
        unsigned numberOfValues = m_function.values.size();
        m_useCounts.fill(0, numberOfValues);
        m_blockOf.fill(noBlock, numberOfValues);
        m_indexOf.fill(0, numberOfValues);
        m_predecessors.resize(numberOfBlocks);
        m_isReachable = reachableBlocks(m_function);
        {
            BitVector isSeen(numberOfBlocks);
            Vector<std::pair<unsigned, unsigned>, 32> stack { { 0u, 0u } };
            isSeen.set(0);
            while (!stack.isEmpty()) {
                auto& [block, next] = stack.last();
                auto& targets = m_function.blocks[block].terminator.targets;
                if (next < targets.size()) {
                    unsigned target = targets[next++].block.index();
                    if (!isSeen.set(target))
                        stack.append({ target, 0u });
                    continue;
                }
                m_postOrder.append(block);
                stack.removeLast();
            }
        }
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            auto& block = m_function.blocks[b];
            for (ValueID parameter : block.parameters)
                m_blockOf[parameter.index()] = b;
            for (unsigned i = 0; i < block.values.size(); ++i) {
                ValueID id = block.values[i];
                m_blockOf[id.index()] = b;
                m_indexOf[id.index()] = i + 1;
                std::as_const(m_function.at(id)).forEachChild([&](ValueID child) {
                    ++m_useCounts[child.index()];
                });
            }
            if (block.terminator.value)
                ++m_useCounts[block.terminator.value.index()];
            for (auto& target : block.terminator.targets) {
                for (ValueID argument : target.arguments)
                    ++m_useCounts[argument.index()];
                if (m_isReachable.get(b))
                    m_predecessors[target.block.index()].append(b);
            }
        }
        Vector<unsigned> dominator = immediateDominatorsOf(m_function);
        Vector<Vector<unsigned, 2>> children(numberOfBlocks);
        for (unsigned b = 1; b < numberOfBlocks; ++b) {
            if (m_isReachable.get(b))
                children[dominator[b]].append(b);
        }
        m_dominatorPreorder.fill(0, numberOfBlocks);
        m_dominatorPostorder.fill(0, numberOfBlocks);
        unsigned clock = 0;
        Vector<std::pair<unsigned, unsigned>, 32> stack { { 0u, 0u } };
        m_dominatorPreorder[0] = ++clock;
        while (!stack.isEmpty()) {
            auto& [block, next] = stack.last();
            if (next < children[block].size()) {
                unsigned child = children[block][next++];
                m_dominatorPreorder[child] = ++clock;
                stack.append({ child, 0u });
                continue;
            }
            m_dominatorPostorder[block] = ++clock;
            stack.removeLast();
        }
    }

    bool dominates(unsigned a, unsigned b) const { return m_dominatorPreorder[a] <= m_dominatorPreorder[b] && m_dominatorPostorder[b] <= m_dominatorPostorder[a]; }

    bool isAvailableAt(ValueID id, Point at) const
    {
        if (id.index() >= m_originalValueCount) {
            for (auto& [conversion, point] : m_conversions) {
                if (conversion == id)
                    return point.block == at.block ? point.index <= at.index : dominates(point.block, at.block);
            }
            return true;
        }
        unsigned block = m_blockOf[id.index()];
        if (block == noBlock)
            return false;
        if (block == at.block)
            return m_indexOf[id.index()] <= at.index;
        return dominates(block, at.block);
    }

    static bool isPlainPrimitive(const Type& type) { return !type.hasObjects() && !(type.primitives & Primitive::Symbol); }

    bool isString(ValueID id) const { return id.index() >= m_originalValueCount || m_isString.get(id.index()); }

    bool isHandledElsewhere(ValueID id)
    {
        if (!m_builders) {
            m_builders = analyzeStringBuilders(m_program, m_id, m_function);
            m_keys = computeMapKeyValues(m_program, m_id, m_function);
        }
        return m_builders->hasBuilder(id) || m_builders->isFoldedIntoAppend.get(id.index()) || m_keys.get(id.index());
    }

    bool isConcatenation(ValueID id)
    {
        if (id.index() >= m_originalValueCount || !m_isString.get(id.index()) || m_finishedRoots.containsIf([&](auto& entry) { return entry.first == id; }))
            return false;
        auto& value = m_function.at(id);
        if (value.opcode == Opcode::Add)
            return !isHandledElsewhere(id);
        if (value.opcode != Opcode::Strcat || value.arguments.isEmpty())
            return false;
        for (ValueID part : value.arguments) {
            if (!isString(part) && !isPlainPrimitive(typeOf(part)))
                return false;
        }
        return !isHandledElsewhere(id);
    }

    bool isStringPhi(ValueID id) const
    {
        if (id.index() >= m_originalValueCount || !m_isString.get(id.index()))
            return false;
        auto& value = m_function.at(id);
        return value.opcode == Opcode::Parameter && value.parameterBlock().index() && m_isReachable.get(value.parameterBlock().index());
    }

    Checkpoint checkpoint() const
    {
        return { m_insertions.size(), m_newValues.size(), m_deadLog.size(), m_bufferedLog.size(), m_accountedLog.size(), m_conversions.size(), m_guardedSites.size(), m_nextState, m_loopCount };
    }

    void rollBack(const Checkpoint& to)
    {
        m_insertions.shrink(to.insertions);
        m_newValues.shrink(to.values);
        while (m_deadLog.size() > to.dead)
            m_isDead.clear(m_deadLog.takeLast().index());
        while (m_bufferedLog.size() > to.bufferedValues)
            m_bufferedValues.remove(m_bufferedLog.takeLast().index());
        while (m_accountedLog.size() > to.accounted)
            --m_accounted[m_accountedLog.takeLast().index()];
        m_conversions.shrink(to.conversions);
        m_guardedSites.shrink(to.guardedSites);
        m_nextState = to.nextState;
        m_loopCount = to.loopCount;
    }

    void markDead(ValueID id)
    {
        if (!m_isDead.set(id.index()))
            m_deadLog.append(id);
    }

    void account(ValueID id)
    {
        ++m_accounted[id.index()];
        m_accountedLog.append(id);
    }

    ValueID addValue(Value&& value)
    {
        m_newValues.append(WTF::move(value));
        return ValueID(m_function.values.size() + m_newValues.size() - 1);
    }

    ValueID addOperation(BuiltinID operation, const Value& origin, TypeID type, Arguments&& arguments)
    {
        Value value;
        value.opcode = Opcode::CallBuiltin;
        value.position = origin.position;
        value.inlineFrame = origin.inlineFrame;
        value.type = type;
        value.rep = repOf(m_program.at(type));
        value.immediates = { operation.index(), Format::none };
        value.arguments = WTF::move(arguments);
        if (m_isCommitting) {
            m_function.values.append(WTF::move(value));
            return ValueID(m_function.values.size() - 1);
        }
        return addValue(WTF::move(value));
    }

    ValueID addInteger(const Value& origin, int64_t integer)
    {
        Value value;
        value.opcode = Opcode::Integer;
        value.position = origin.position;
        value.inlineFrame = origin.inlineFrame;
        value.type = m_program.types.integer;
        value.rep = repOf(m_program.at(value.type));
        value.setBits(static_cast<uint64_t>(integer));
        return addValue(WTF::move(value));
    }

    void insert(Point at, Step step, ValueID value, unsigned region, unsigned stateBefore, unsigned stateAfter)
    {
        m_insertions.append(Insertion { at, step, value, region, stateBefore, stateAfter });
    }

    Point pointBefore(ValueID id) const { return { m_blockOf[id.index()], m_indexOf[id.index()] - 1 }; }
    Point endOf(unsigned block) const { return { block, static_cast<unsigned>(m_function.blocks[block].values.size()) }; }

    bool flush(Context& context, Point at, const Value& origin)
    {
        if (!context.needsBegin && context.pending.isEmpty())
            return true;
        for (auto& part : context.pending) {
            if (part.value && part.value.index() < m_originalValueCount && m_isAccumulator.get(part.value.index()) && m_isInLoop.get(at.block))
                return false;
            if (part.kind != StringAppendPart::Value || isAvailableAt(part.value, at))
                continue;
            auto& value = this->at(part.value);
            if (value.opcode != Opcode::String)
                return false;
            Value copy(value);
            part.value = addValue(WTF::move(copy));
            insert(at, Step::Other, part.value, context.region, 0, 0);
        }
        if (context.needsBegin) {
            ValueID flag = addInteger(origin, context.region << stringAppendBeginRegionShift);
            insert(at, Step::Other, flag, context.region, 0, 0);
            insert(at, Step::Begin, addOperation(Operations::StringAppendBegin, origin, m_program.types.undefined, { flag }), context.region, 0, context.state);
            context.needsBegin = false;
        }
        for (size_t first = 0, size = 0; first < context.pending.size(); first += size) {
            size = 0;
            while (first + size < context.pending.size() && size + groupSize(context.pending[first + size].kind) <= maxStringAppendParts)
                size += groupSize(context.pending[first + size].kind);
            auto [kinds, append] = makeAppend(context.pending.span().subspan(first, size), origin, context.region);
            insert(at, Step::Other, kinds, context.region, 0, 0);
            unsigned stateAfter = m_nextState++;
            insert(at, Step::Append, append, context.region, context.state, stateAfter);
            context.state = stateAfter;
        }
        context.pending.shrink(0);
        return true;
    }

    std::pair<ValueID, ValueID> makeAppend(std::span<const Part> parts, const Value& origin, unsigned region)
    {
        uint64_t kinds = static_cast<uint64_t>(region) << stringAppendRegionShift;
        Arguments arguments;
        arguments.append(ValueID());
        for (size_t k = 0; k < parts.size(); ++k) {
            kinds |= static_cast<uint64_t>(parts[k].kind) << (stringAppendPartBits * k);
            if (parts[k].value)
                arguments.append(parts[k].value);
        }
        kinds |= static_cast<uint64_t>(StringAppendPart::End) << (stringAppendPartBits * parts.size());
        arguments[0] = addInteger(origin, static_cast<int64_t>(kinds));
        ValueID kindsID = arguments[0];
        return { kindsID, addOperation(Operations::StringAppend, origin, m_program.types.undefined, WTF::move(arguments)) };
    }

    bool isEscapeHTMLCall(ValueID id) const
    {
        if (id.index() >= m_originalValueCount || m_useCounts[id.index()] != 1 || m_isDead.get(id.index()))
            return false;
        auto& value = m_function.at(id);
        if (value.opcode != Opcode::Call || value.arguments.size() != 1 || value.isInTailPosition || !isPlainPrimitive(typeOf(value.arguments[0])) || typeOf(value.arguments[0]).primitives & Primitive::BigInt)
            return false;
        if (value.child(1) && !typeOf(value.child(1)).hasOnlyPrimitives(Primitive::Undefined))
            return false;
        return isEscapeHTMLAlias(value.child(0));
    }

    bool isEscapeHTMLAlias(ValueID id, unsigned depth = 0) const
    {
        while (m_function.at(id).opcode == Opcode::Cast)
            id = m_function.at(id).child(0);
        auto& value = m_function.at(id);
        if (value.opcode == Opcode::GetModuleVariable)
            return m_escapeHTMLAliases.get(value.variable().index());
        if (value.opcode != Opcode::Parameter || !value.parameterBlock().index() || depth > 2)
            return false;
        unsigned block = value.parameterBlock().index();
        for (unsigned predecessor : m_predecessors[block]) {
            for (auto& target : m_function.blocks[predecessor].terminator.targets) {
                if (target.block.index() == block && (value.parameterIndex() >= target.arguments.size() || !isEscapeHTMLAlias(target.arguments[value.parameterIndex()], depth + 1)))
                    return false;
            }
        }
        return !m_predecessors[block].isEmpty();
    }

    bool hasOnlyConstantsBetween(ValueID first, ValueID last) const
    {
        if (m_blockOf[first.index()] != m_blockOf[last.index()] || m_indexOf[first.index()] >= m_indexOf[last.index()])
            return false;
        auto& values = m_function.blocks[m_blockOf[first.index()]].values;
        for (unsigned i = m_indexOf[first.index()]; i + 1 < m_indexOf[last.index()]; ++i) {
            if (m_isDead.get(values[i].index()))
                continue;
            if (!isConstant(m_function.at(values[i]).opcode))
                return false;
        }
        return true;
    }

    bool isSliceEscapedBy(ValueID id, ValueID call) const
    {
        static const BuiltinName stringName = Builtins::singleton().nameOf(asBytes("String"_s.span8()));
        static const BuiltinName sliceName = Builtins::singleton().nameOf(asBytes("slice"_s.span8()));
        if (id.index() >= m_originalValueCount || m_useCounts[id.index()] != 1 || m_isDead.get(id.index()))
            return false;
        auto& value = m_function.at(id);
        if (value.opcode != Opcode::CallBuiltin || value.arguments.isEmpty() || value.arguments.size() > 2 || !value.child(0))
            return false;
        auto& builtin = Builtins::singleton().at(value.builtin());
        if (builtin.kind != BuiltinKind::Method || builtin.holderName != stringName || builtin.nameID != sliceName)
            return false;
        auto& receiver = typeOf(value.child(0));
        if (!(receiver.primitives & Primitive::String) || !receiver.hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull))
            return false;
        for (ValueID argument : value.arguments) {
            if (!typeOf(argument).isNumber())
                return false;
        }
        return hasOnlyConstantsBetween(id, call);
    }

    bool placeEscapeHTMLCall(Context& context, ValueID call, Vector<ValueID, 2>&& conversion)
    {
        const Value origin(m_function.at(call));
        Point point = pointBefore(call);
        ValueID text = origin.arguments[0];
        if (!(typeOf(text).primitives & Primitive::String))
            return false;
        bool isSlice = isSliceEscapedBy(text, call);
        GuardedSite site;
        if (isSlice) {
            point = pointBefore(text);
            site.slowPath.append(text);
            markDead(text);
        }
        site.fastResult = isSlice ? m_function.at(text).child(0) : text;
        ValueID mustBeString = addInteger(origin, isSlice);
        insert(point, Step::Other, mustBeString, context.region, 0, 0);
        Value guard;
        guard.opcode = Opcode::CallBuiltin;
        guard.position = origin.position;
        guard.inlineFrame = origin.inlineFrame;
        guard.type = m_program.types.boolean;
        guard.rep = repOf(m_program.at(guard.type));
        guard.immediates = { Operations::StringAppendMayEscape.index(), Format::none };
        guard.arguments = { origin.child(0), site.fastResult, mustBeString };
        guard.guardsFastPath = true;
        Value result;
        result.opcode = Opcode::Parameter;
        result.position = origin.position;
        result.inlineFrame = origin.inlineFrame;
        result.type = at(site.fastResult).type;
        result.rep = repOf(m_program.at(result.type));
        site.guard = addValue(WTF::move(guard));
        site.result = addValue(WTF::move(result));
        site.slowPath.append(call);
        site.slowPath.appendVector(conversion);
        markDead(call);
        if (!isAvailableAt(origin.child(0), point))
            return false;
        context.pending.append(Part { isSlice ? StringAppendPart::HTMLEscapedSlice : StringAppendPart::HTMLEscapedValue, site.guard });
        context.pending.append(Part { StringAppendPart::Value, site.result });
        if (isSlice) {
            auto& slice = m_function.at(text);
            context.pending.append(Part { StringAppendPart::Value, slice.arguments[0] });
            ValueID end = slice.arguments.size() > 1 ? slice.arguments[1] : addInteger(origin, std::numeric_limits<int32_t>::max());
            if (slice.arguments.size() == 1) {
                insert(point, Step::Other, end, context.region, 0, 0);
                m_conversions.append({ end, point });
            }
            context.pending.append(Part { StringAppendPart::Value, end });
        }
        insert(point, Step::Other, ValueID(), context.region, 0, 0);
        m_insertions.last().guardedSite = m_guardedSites.size();
        m_conversions.append({ site.guard, point });
        m_conversions.append({ site.result, point });
        m_guardedSites.append(WTF::move(site));
        context.knownStart = KnownStart::None;
        return true;
    }

    static unsigned groupSize(StringAppendPart kind)
    {
        return kind == StringAppendPart::HTMLEscapedSlice ? 4 : kind == StringAppendPart::HTMLEscapedValue || kind == StringAppendPart::NumberInRadix ? 2 : 1;
    }

    void appendLeaf(Context& context, ValueID id)
    {
        if (at(id).opcode == Opcode::String && m_program.string(at(id).string()).empty())
            return;
        context.pending.append(Part { StringAppendPart::Value, id });
        context.knownStart = KnownStart::None;
    }

    bool placeNumberToString(Context& context, ValueID id)
    {
        if (id.index() >= m_originalValueCount || m_useCounts[id.index()] != 1 || m_isDead.get(id.index()) || m_mustMaterialize.get(id.index()))
            return false;
        auto& value = m_function.at(id);
        if (value.opcode != Opcode::CallBuiltin || !value.child(0) || value.arguments.size() > 1 || !typeOf(value.child(0)).isNumber())
            return false;
        auto& implementation = Builtins::singleton().at(value.builtin()).implementation;
        if (implementation.kind != BuiltinImplementation::Inline || implementation.inlined != InlineBuiltin::NumberToString)
            return false;
        int64_t radix = 10;
        if (!value.arguments.isEmpty()) {
            auto& argument = m_function.at(value.arguments[0]);
            if (argument.opcode != Opcode::Integer || argument.integer() < 2 || argument.integer() > 36)
                return false;
            radix = argument.integer();
        }
        markDead(id);
        if (radix != 10)
            context.pending.append(Part { StringAppendPart::NumberInRadix, value.arguments[0] });
        appendLeaf(context, value.child(0));
        return true;
    }

    ValueID skipConversions(ValueID id)
    {
        while (id.index() < m_originalValueCount && m_useCounts[id.index()] == 1 && !m_mustMaterialize.get(id.index())) {
            auto& value = m_function.at(id);
            ValueID child = value.child(0);
            bool isSkippable = (value.opcode == Opcode::ToString && (isString(child) || isPlainPrimitive(typeOf(child)))) || (isStringPreservingCast(m_program, value) && isString(child));
            if (!isSkippable)
                break;
            markDead(id);
            id = child;
        }
        return id;
    }

    bool isBounded(ValueID id, bool countsShortEscapes = false, unsigned depth = 0)
    {
        if (depth > 8)
            return false;
        auto& value = at(id);
        if (value.opcode == Opcode::String)
            return true;
        if (id.index() < m_originalValueCount && m_useCounts[id.index()] == 1) {
            if (countsShortEscapes && isEscapeHTMLCall(id))
                return true;
            if (value.opcode == Opcode::ToString)
                return isBounded(value.child(0), countsShortEscapes, depth + 1);
            if (isConcatenation(id)) {
                bool result = value.arguments.size() <= maxStringAppendParts;
                value.forEachChild([&](ValueID child) {
                    result = result && isBounded(child, countsShortEscapes, depth + 1);
                });
                return result;
            }
        }
        return !(typeOf(id).primitives & (Primitive::String | Primitive::LiteralString | Primitive::BigInt)) && isPlainPrimitive(typeOf(id));
    }

    bool hasSameContext(const BufferedValue& buffered, const Context& context) const
    {
        return buffered.region == context.region && buffered.contextState == context.state && buffered.contextNeedsBegin == context.needsBegin && buffered.contextPending == context.pending;
    }

    bool place(Context& context, ValueID operand, ValueID checkedUser = { })
    {
        ValueID id = skipConversions(operand);
        if (id.index() < m_originalValueCount && m_function.at(id).opcode == Opcode::ToString && m_useCounts[id.index()] == 1 && isEscapeHTMLCall(m_function.at(id).child(0)) && hasOnlyConstantsBetween(m_function.at(id).child(0), id)) {
            Checkpoint before = checkpoint();
            Context saved = context;
            markDead(id);
            if (placeEscapeHTMLCall(context, m_function.at(id).child(0), { id }))
                return true;
            rollBack(before);
            context = WTF::move(saved);
        }
        if (placeNumberToString(context, id))
            return true;
        if (id.index() < m_originalValueCount) {
            if (auto found = m_bufferedValues.find(id.index()); found != m_bufferedValues.end()) {
                if (!hasSameContext(*found->value, context)) {
                    m_conflict = id;
                    return false;
                }
                account(id);
                context.state = found->value->state;
                context.needsBegin = false;
                context.pending.shrink(0);
                context.knownStart = KnownStart::None;
                return true;
            }
            if (m_isDead.get(id.index()))
                return false;
            if (!m_mustMaterialize.get(id.index()) && m_placementDepth < maxPlacementDepth) {
                Checkpoint before = checkpoint();
                Context saved = context;
                bool isUsedOnce = m_useCounts[id.index()] == 1;
                bool canBeBuffered = isUsedOnce || m_continuationUses[id.index()] == m_useCounts[id.index()];
                bool succeeded = false;
                bool attempted = true;
                ++m_placementDepth;
                if (canBeBuffered && isConcatenation(id)) {
                    succeeded = placeConcatenation(context, id, false, checkedUser);
                    if (!succeeded && !m_isNeverMeasured.set(id.index())) {
                        rollBack(before);
                        context = saved;
                        succeeded = placeConcatenation(context, id, false, checkedUser);
                    }
                } else if (canBeBuffered && isStringPhi(id) && (!isUsedOnce || hasConcatenationOperand(id)))
                    succeeded = placePhi(context, id, false);
                else
                    attempted = false;
                --m_placementDepth;
                if (succeeded)
                    return true;
                if (attempted) {
                    rollBack(before);
                    context = WTF::move(saved);
                }
            }
        }
        if (!isString(id) && !isPlainPrimitive(typeOf(id)))
            return false;
        appendLeaf(context, id);
        return true;
    }

    bool placeConcatenation(Context& context, ValueID id, bool isRegionRoot, ValueID checkedUser = { })
    {
        auto& value = m_function.at(id);
        bool isUsedOnce = m_useCounts[id.index()] == 1 || isRegionRoot;
        BufferedValue buffered;
        if (!isUsedOnce)
            buffered = BufferedValue { 0, context.region, context.state, context.needsBegin, context.pending, KnownStart::None };
        bool isChecked = !isRegionRoot && !isBounded(id) && !(checkedUser && m_useCounts[id.index()] == 1 && hasOnlyConstantsBetween(id, checkedUser));
        bool isMeasured = !m_isNeverMeasured.get(id.index()) && isChecked && isUsedOnce && isBounded(id, true);
        if (isMeasured)
            isChecked = false;
        size_t firstMeasuredPart = context.pending.size();
        unsigned stateBeforeMeasuredParts = context.state;
        Vector<ValueID, 8> operands;
        if (value.opcode == Opcode::Add)
            operands = { value.child(0), value.child(1) };
        else
            operands.append(value.arguments.span());
        StringAppendPart check = StringAppendPart::CheckLengthAndPop;
        bool continuesBufferedValue = false;
        if (isChecked) {
            ValueID first = operands[0];
            while (first.index() < m_originalValueCount && !m_bufferedValues.contains(first.index()) && m_useCounts[first.index()] == 1 && (m_function.at(first).opcode == Opcode::ToString || isStringPreservingCast(m_program, m_function.at(first))))
                first = m_function.at(first).child(0);
            if (auto found = m_bufferedValues.find(first.index()); found != m_bufferedValues.end()) {
                if (found->value->knownStart == KnownStart::None)
                    return false;
                check = found->value->knownStart == KnownStart::RegionStart ? StringAppendPart::CheckRegionLength : StringAppendPart::CheckLength;
                continuesBufferedValue = true;
                buffered.knownStart = found->value->knownStart;
            }
        }
        if (isChecked && !continuesBufferedValue) {
            if (context.knownStart == KnownStart::RegionStart)
                check = StringAppendPart::CheckRegionLength;
            else if (context.knownStart == KnownStart::TopOfStack)
                check = StringAppendPart::CheckLength;
            else if (isUsedOnce) {
                context.pending.append(Part { StringAppendPart::PushStart, { } });
                context.knownStart = KnownStart::TopOfStack;
            } else
                return false;
            buffered.knownStart = context.knownStart;
        }
        markDead(id);
        for (unsigned k = 0; k < operands.size(); ++k) {
            ValueID operand = operands[k];
            if (value.opcode == Opcode::Add && !isString(operand) && !isPlainPrimitive(typeOf(operand))) {
                Value empty;
                empty.opcode = Opcode::String;
                empty.position = value.position;
                empty.inlineFrame = value.inlineFrame;
                empty.type = m_program.types.string;
                empty.rep = repOf(m_program.at(empty.type));
                empty.immediates = { m_program.emptyString.index(), Format::none };
                ValueID emptyID = addValue(WTF::move(empty));
                Value conversion(value);
                conversion.type = m_program.types.string;
                conversion.rep = repOf(m_program.at(conversion.type));
                conversion.copyPredictionFrom(Value { });
                conversion.children = { operand, emptyID, ValueID() };
                ValueID conversionID = addValue(WTF::move(conversion));
                if (isEscapeHTMLCall(operand) && hasOnlyConstantsBetween(operand, id)) {
                    if (!placeEscapeHTMLCall(context, operand, { emptyID, conversionID }))
                        return false;
                    continue;
                }
                insert(pointBefore(id), Step::Other, emptyID, context.region, 0, 0);
                insert(pointBefore(id), Step::Other, conversionID, context.region, 0, 0);
                m_conversions.append({ conversionID, pointBefore(id) });
                appendLeaf(context, conversionID);
                continue;
            }
            if (!place(context, operand, isChecked || isRegionRoot ? id : ValueID()))
                return false;
        }
        if (isMeasured) {
            if (context.state != stateBeforeMeasuredParts || context.pending.size() < firstMeasuredPart || context.pending.size() - firstMeasuredPart > maxStringAppendParts)
                return false;
            for (size_t k = firstMeasuredPart; k < context.pending.size(); ++k) {
                if (groupSize(context.pending[k].kind) > 1)
                    k += groupSize(context.pending[k].kind) - 1;
                else if (context.pending[k].kind != StringAppendPart::Value || !isBounded(context.pending[k].value))
                    return false;
            }
            auto [kinds, append] = makeAppend(context.pending.span().subspan(firstMeasuredPart), value, noRegion);
            m_newValues[append.index() - m_function.values.size()].immediates[0] = Operations::StringAppendMeasure.index();
            insert(pointBefore(id), Step::Other, kinds, context.region, 0, 0);
            insert(pointBefore(id), Step::Other, append, context.region, 0, 0);
        }
        if (isChecked) {
            context.pending.append(Part { check, { } });
            context.knownStart = KnownStart::None;
        }
        if (isChecked || !isUsedOnce) {
            if (!flush(context, pointBefore(id), value))
                return false;
        }
        if (!isUsedOnce) {
            buffered.state = context.state;
            m_bufferedValues.add(id.index(), makeUniqueWithoutFastMallocCheck<BufferedValue>(WTF::move(buffered)));
            m_bufferedLog.append(id);
            account(id);
        }
        return true;
    }

    bool hasConcatenationOperand(ValueID id, unsigned depth = 0)
    {
        if (depth > 4)
            return false;
        auto& value = m_function.at(id);
        unsigned block = value.parameterBlock().index();
        unsigned position = value.parameterIndex();
        for (unsigned predecessor : m_predecessors[block]) {
            auto& target = m_function.blocks[predecessor].terminator.targets[0];
            if (position >= target.arguments.size())
                return false;
            ValueID argument = target.arguments[position];
            while (m_useCounts[argument.index()] == 1 && (m_function.at(argument).opcode == Opcode::ToString || isStringPreservingCast(m_program, m_function.at(argument))) && isString(m_function.at(argument).child(0)))
                argument = m_function.at(argument).child(0);
            if (m_useCounts[argument.index()] == 1 && (isConcatenation(argument) || (isStringPhi(argument) && hasConcatenationOperand(argument, depth + 1))))
                return true;
        }
        return false;
    }

    bool placePhi(Context& context, ValueID id, bool isRegionRoot)
    {
        auto& value = m_function.at(id);
        unsigned block = value.parameterBlock().index();
        unsigned position = value.parameterIndex();
        bool isUsedOnce = m_useCounts[id.index()] == 1 && !isRegionRoot;
        bool pushedStart = false;
        if (!isUsedOnce && context.knownStart == KnownStart::None) {
            context.pending.append(Part { StringAppendPart::PushStart, { } });
            context.knownStart = KnownStart::TopOfStack;
            pushedStart = true;
        }
        unsigned merged = m_nextState++;
        m_bufferedValues.add(id.index(), makeUniqueWithoutFastMallocCheck<BufferedValue>(BufferedValue { merged, context.region, context.state, context.needsBegin, context.pending, context.knownStart }));
        m_bufferedLog.append(id);
        if (!isRegionRoot)
            account(id);
        bool isLoop = false;
        for (unsigned predecessor : m_predecessors[block]) {
            auto& terminator = m_function.blocks[predecessor].terminator;
            if (terminator.kind != TerminatorKind::Jump || position >= terminator.targets[0].arguments.size())
                return false;
            isLoop |= dominates(block, predecessor);
            Context predecessorContext = context;
            if (!place(predecessorContext, terminator.targets[0].arguments[position]))
                return false;
            if (!flush(predecessorContext, endOf(predecessor), value))
                return false;
            insert(endOf(predecessor), Step::Rename, ValueID(), context.region, predecessorContext.state, merged);
        }
        m_loopCount += isLoop;
        context.state = merged;
        context.needsBegin = false;
        context.pending.shrink(0);
        context.knownStart = KnownStart::None;
        if (pushedStart)
            context.pending.append(Part { StringAppendPart::PopStart, { } });
        return true;
    }

    bool allBufferedUsesAccounted(size_t firstBuffered, ValueID exempt) const
    {
        for (size_t k = firstBuffered; k < m_bufferedLog.size(); ++k) {
            ValueID id = m_bufferedLog[k];
            if (id != exempt && m_accounted[id.index()] != m_useCounts[id.index()])
                return false;
        }
        return true;
    }

    ValueID findBufferedValueWithOtherUse(size_t firstBuffered, ValueID exempt) const
    {
        for (size_t k = firstBuffered; k < m_bufferedLog.size(); ++k) {
            ValueID id = m_bufferedLog[k];
            if (id != exempt && m_accounted[id.index()] != m_useCounts[id.index()])
                return id;
        }
        return { };
    }

    void countContinuationUses()
    {
        m_continuationUses.fill(0, m_originalValueCount);
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            if (!m_isReachable.get(b))
                continue;
            auto& block = m_function.blocks[b];
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                if (!m_isString.get(id.index()))
                    continue;
                if (value.opcode == Opcode::Add || value.opcode == Opcode::ToString || isStringPreservingCast(m_program, value))
                    ++m_continuationUses[value.child(0).index()];
                else if (value.opcode == Opcode::Strcat && !value.arguments.isEmpty())
                    ++m_continuationUses[value.arguments[0].index()];
            }
            for (auto& target : block.terminator.targets) {
                auto& parameters = m_function.at(target.block).parameters;
                for (unsigned i = 0; i < std::min(parameters.size(), target.arguments.size()); ++i) {
                    if (m_isString.get(parameters[i].index()))
                        ++m_continuationUses[target.arguments[i].index()];
                }
            }
        }
    }

    void findAccumulators()
    {
        unsigned numberOfBlocks = m_function.blocks.size();
        m_isLoopHeader = BitVector(numberOfBlocks);
        m_isInLoop = BitVector(numberOfBlocks);
        for (unsigned header = 0; header < numberOfBlocks; ++header) {
            for (unsigned predecessor : m_predecessors[header]) {
                if (!dominates(header, predecessor))
                    continue;
                m_isLoopHeader.set(header);
                BitVector isSeen(numberOfBlocks);
                isSeen.set(header);
                Vector<unsigned, 16> worklist;
                if (!isSeen.set(predecessor))
                    worklist.append(predecessor);
                while (!worklist.isEmpty()) {
                    for (unsigned before : m_predecessors[worklist.takeLast()]) {
                        if (!isSeen.set(before))
                            worklist.append(before);
                    }
                }
                m_isInLoop.merge(isSeen);
            }
        }
        m_isAccumulator = BitVector(m_originalValueCount);
        for (size_t header : m_isLoopHeader) {
            for (ValueID parameter : m_function.blocks[header].parameters) {
                if (m_isString.get(parameter.index()))
                    m_isAccumulator.set(parameter.index());
            }
        }
        for (bool changed = !m_isAccumulator.isEmpty(); changed;) {
            changed = false;
            for (unsigned k = m_postOrder.size(); k--;) {
                auto& block = m_function.blocks[m_postOrder[k]];
                for (ValueID id : block.values) {
                    auto& value = m_function.at(id);
                    if (m_isAccumulator.get(id.index()) || !m_isString.get(id.index()))
                        continue;
                    if (value.opcode != Opcode::Add && value.opcode != Opcode::Strcat && value.opcode != Opcode::ToString && value.opcode != Opcode::Cast)
                        continue;
                    bool holds = false;
                    value.forEachChild([&](ValueID child) {
                        holds |= m_isAccumulator.get(child.index());
                    });
                    if (holds) {
                        m_isAccumulator.set(id.index());
                        changed = true;
                    }
                }
                for (auto& target : block.terminator.targets) {
                    auto& parameters = m_function.at(target.block).parameters;
                    for (unsigned i = 0; i < std::min(parameters.size(), target.arguments.size()); ++i) {
                        if (m_isAccumulator.get(target.arguments[i].index()) && m_isString.get(parameters[i].index()) && !m_isAccumulator.set(parameters[i].index()))
                            changed = true;
                    }
                }
            }
        }
    }

    bool isProfitable() const
    {
        return m_loopCount;
    }

    void measureParts(ValueID id, unsigned depth, unsigned& parts, size_t& constantLength)
    {
        auto& value = at(id);
        if (value.opcode == Opcode::String) {
            constantLength += m_program.string(value.string()).size();
            ++parts;
            return;
        }
        if (depth < 8 && id.index() < m_originalValueCount && (!depth || m_useCounts[id.index()] == 1) && isConcatenation(id)) {
            value.forEachChild([&](ValueID child) {
                measureParts(child, depth + 1, parts, constantLength);
            });
            return;
        }
        ++parts;
    }

    bool isStoredInArray(ValueID root)
    {
        if (m_useCounts[root.index()] != 1 || !m_isInLoop.get(m_blockOf[root.index()]))
            return false;
        unsigned parts = 0;
        size_t constantLength = 0;
        measureParts(root, 0, parts, constantLength);
        if (parts < minStoredParts || constantLength > maxStoredConstantLength)
            return false;
        auto use = findSingleExternalUse(root);
        if (!use || use->index >= m_function.blocks[use->block].values.size())
            return false;
        auto& user = m_function.at(m_function.blocks[use->block].values[use->index]);
        switch (user.opcode) {
        case Opcode::PutByVal:
            return user.child(0) != root && user.child(1) != root;
        case Opcode::NewArray:
            return true;
        case Opcode::CallBuiltin: {
            static const BuiltinName arrayName = Builtins::singleton().nameOf(asBytes("Array"_s.span8()));
            static const BuiltinName pushName = Builtins::singleton().nameOf(asBytes("push"_s.span8()));
            auto& builtin = Builtins::singleton().at(user.builtin());
            return builtin.kind == BuiltinKind::Method && builtin.holderName == arrayName && builtin.nameID == pushName;
        }
        default:
            return false;
        }
    }

    void planRegions()
    {
        for (unsigned k = m_postOrder.size(); k--;) {
            unsigned b = m_postOrder[k];
            if (!m_isLoopHeader.get(b))
                continue;
            for (ValueID id : m_function.blocks[b].parameters) {
                if (!m_bufferedValues.contains(id.index()) && isStringPhi(id))
                    planPhiRegion(id);
            }
        }
        for (unsigned b : m_postOrder) {
            auto& block = m_function.blocks[b];
            for (unsigned i = block.values.size(); i--;) {
                ValueID id = block.values[i];
                if (m_isDead.get(id.index()) || m_bufferedValues.contains(id.index()) || !isConcatenation(id))
                    continue;
                planRegion(id);
            }
            for (ValueID id : block.parameters) {
                if (b && !m_bufferedValues.contains(id.index()) && isStringPhi(id))
                    planPhiRegion(id);
            }
        }
    }

    std::optional<Point> findSingleExternalUse(ValueID id) const
    {
        std::optional<Point> result;
        unsigned found = 0;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            if (!m_isReachable.get(b))
                continue;
            auto& block = m_function.blocks[b];
            for (unsigned i = 0; i < block.values.size(); ++i) {
                if (m_isDead.get(block.values[i].index()))
                    continue;
                m_function.at(block.values[i]).forEachChild([&](ValueID child) {
                    if (child != id)
                        return;
                    ++found;
                    result = Point { b, i };
                });
            }
            if (block.terminator.value == id) {
                ++found;
                result = endOf(b);
            }
            for (auto& target : block.terminator.targets) {
                auto& parameters = m_function.at(target.block).parameters;
                for (unsigned i = 0; i < target.arguments.size(); ++i) {
                    if (target.arguments[i] != id || (i < parameters.size() && m_bufferedValues.contains(parameters[i].index())))
                        continue;
                    ++found;
                    result = endOf(b);
                }
            }
        }
        return found == 1 ? result : std::nullopt;
    }

    void planPhiRegion(ValueID root)
    {
        if (m_continuationUses[root.index()] + 1 < m_useCounts[root.index()] || m_nextRegion > maxStringAppendRegion)
            return;
        const Value origin(m_function.at(root));
        for (unsigned attempt = 0; attempt < maxPlanAttempts; ++attempt) {
            Checkpoint before = checkpoint();
            size_t firstBuffered = m_bufferedLog.size();
            unsigned region = m_nextRegion;
            Context context { region, m_nextState++, true, KnownStart::RegionStart, { } };
            m_loopCount = 0;
            bool planned = placePhi(context, root, true);
            bool isProfitable = this->isProfitable();
            auto use = planned && isProfitable && m_accounted[root.index()] + 1 == m_useCounts[root.index()] ? findSingleExternalUse(root) : std::nullopt;
            if (use && allBufferedUsesAccounted(firstBuffered, root) && flush(context, *use, origin)) {
                ValueID regionID = addInteger(origin, region);
                insert(*use, Step::Other, regionID, region, 0, 0);
                ValueID finish = addOperation(Operations::StringAppendFinish, origin, origin.type, { regionID });
                insert(*use, Step::Finish, finish, region, context.state, 0);
                m_replacements.append({ root, finish });
                ++m_nextRegion;
                if (verifyPlan())
                    return;
                --m_nextRegion;
                m_replacements.removeLast();
                rollBack(before);
                return;
            }
            ValueID culprit = use ? findBufferedValueWithOtherUse(firstBuffered, root) : planned ? ValueID() : std::exchange(m_conflict, { });
            if (culprit == root)
                culprit = { };
            rollBack(before);
            if (!culprit)
                return;
            m_mustMaterialize.set(culprit.index());
        }
    }

    void planRegion(ValueID root)
    {
        if (m_nextRegion > maxStringAppendRegion)
            return;
        const Value origin(m_function.at(root));
        for (unsigned attempt = 0; attempt < maxPlanAttempts; ++attempt) {
            Checkpoint before = checkpoint();
            size_t firstBuffered = m_bufferedLog.size();
            unsigned region = m_nextRegion;
            Context context { region, m_nextState++, true, KnownStart::RegionStart, { } };
            m_loopCount = 0;
            bool planned = placeConcatenation(context, root, true) && flush(context, pointBefore(root), origin);
            bool isProfitable = this->isProfitable() || isStoredInArray(root);
            if (planned && isProfitable && allBufferedUsesAccounted(firstBuffered, { })) {
                ValueID regionID = addInteger(origin, region);
                insert(pointBefore(root), Step::Other, regionID, region, 0, 0);
                ValueID finish = addOperation(Operations::StringAppendFinish, origin, m_program.types.string, { regionID });
                insert(pointBefore(root), Step::Finish, finish, region, context.state, 0);
                m_isDead.clear(root.index());
                m_finishedRoots.append({ root, finish });
                ++m_nextRegion;
                if (verifyPlan())
                    return;
                --m_nextRegion;
                m_finishedRoots.removeLast();
                rollBack(before);
                return;
            }
            ValueID culprit = planned ? (isProfitable ? findBufferedValueWithOtherUse(firstBuffered, { }) : ValueID()) : std::exchange(m_conflict, { });
            rollBack(before);
            if (!culprit)
                return;
            m_mustMaterialize.set(culprit.index());
        }
    }

    bool verifyPlan()
    {
        using Stack = Vector<std::pair<unsigned, unsigned>, 4>;
        unsigned numberOfBlocks = m_function.blocks.size();
        Vector<Vector<unsigned, 4>> insertionsByBlock(numberOfBlocks);
        for (unsigned k = 0; k < m_insertions.size(); ++k) {
            if (m_insertions[k].step != Step::Other)
                insertionsByBlock[m_insertions[k].at.block].append(k);
        }
        for (auto& list : insertionsByBlock) {
            std::ranges::stable_sort(list, [&](unsigned a, unsigned b) {
                return m_insertions[a].at.index < m_insertions[b].at.index;
            });
        }
        BitVector suspends;
        for (unsigned i = 0; i < m_originalValueCount; ++i) {
            auto& value = m_function.values[i];
            if (value.opcode == Opcode::Await || value.opcode == Opcode::Yield)
                suspends.set(i);
            else if (value.opcode == Opcode::CallBuiltin) {
                auto& implementation = Builtins::singleton().at(value.builtin()).implementation;
                if (implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::AwaitBySavingFrame)
                    suspends.set(i);
            }
        }
        Vector<std::optional<Stack>> stateAtHead(numberOfBlocks);
        stateAtHead[0] = Stack { };
        Vector<unsigned, 32> worklist { 0u };
        while (!worklist.isEmpty()) {
            unsigned b = worklist.takeLast();
            Stack stack = *stateAtHead[b];
            auto& values = m_function.blocks[b].values;
            unsigned nextValue = 0;
            auto suspendsBefore = [&](unsigned index) {
                if (suspends.isEmpty())
                    return false;
                for (; nextValue < values.size() && nextValue < index; ++nextValue) {
                    if (!stack.isEmpty() && suspends.get(values[nextValue].index()))
                        return true;
                }
                return false;
            };
            for (unsigned k : insertionsByBlock[b]) {
                auto& insertion = m_insertions[k];
                if (suspendsBefore(insertion.at.index))
                    return false;
                if (insertion.step == Step::Begin) {
                    if (!stack.isEmpty())
                        return false;
                    ValueID flag = at(insertion.value).arguments[0];
                    m_newValues[flag.index() - m_function.values.size()].setBits(insertion.region << stringAppendBeginRegionShift);
                    stack.append({ insertion.region, insertion.stateAfter });
                    continue;
                }
                if (stack.isEmpty() || stack.last() != std::pair { insertion.region, insertion.stateBefore })
                    return false;
                if (insertion.step == Step::Finish)
                    stack.removeLast();
                else
                    stack.last().second = insertion.stateAfter;
            }
            if (suspendsBefore(UINT_MAX))
                return false;
            auto& terminator = m_function.blocks[b].terminator;
            bool hasHandler = terminator.kind == TerminatorKind::Invoke || (terminator.kind == TerminatorKind::Throw && !terminator.targets.isEmpty());
            if (hasHandler && (!stack.isEmpty() || !insertionsByBlock[b].isEmpty() || m_function.blocks[b].values.containsIf([&](ValueID id) { return m_isDead.get(id.index()); })))
                return false;
            if (terminator.kind == TerminatorKind::Return && !stack.isEmpty())
                return false;
            for (auto& target : terminator.targets) {
                auto& head = stateAtHead[target.block.index()];
                if (!head) {
                    head = stack;
                    worklist.append(target.block.index());
                } else if (*head != stack)
                    return false;
            }
        }
        return true;
    }

    bool usesOnlyLiveValues() const
    {
        BitVector isRemoved;
        for (ValueID id : m_bufferedLog) {
            if (m_function.at(id).opcode == Opcode::Parameter)
                isRemoved.set(id.index());
        }
        bool result = true;
        auto check = [&](ValueID id) {
            if (id.index() >= m_originalValueCount || (!m_isDead.get(id.index()) && !isRemoved.get(id.index())))
                return;
            result &= m_replacements.containsIf([&](auto& entry) { return entry.first == id; });
        };
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            if (!m_isReachable.get(b))
                continue;
            auto& block = m_function.blocks[b];
            for (ValueID id : block.values) {
                if (!m_isDead.get(id.index()) && !m_finishedRoots.containsIf([&](auto& entry) { return entry.first == id; }))
                    m_function.at(id).forEachChild(check);
            }
            if (block.terminator.value)
                check(block.terminator.value);
            for (auto& target : block.terminator.targets) {
                auto& parameters = m_function.at(target.block).parameters;
                for (unsigned i = 0; i < target.arguments.size(); ++i) {
                    if (i >= parameters.size() || !isRemoved.get(parameters[i].index()))
                        check(target.arguments[i]);
                }
            }
        }
        for (auto& insertion : m_insertions) {
            if (insertion.value && insertion.value.index() >= m_originalValueCount)
                at(insertion.value).forEachChild(check);
        }
        return result;
    }

    void removeBufferedPhis()
    {
        BitVector isRemoved;
        for (ValueID id : m_bufferedLog) {
            if (m_function.at(id).opcode == Opcode::Parameter)
                isRemoved.set(id.index());
        }
        if (isRemoved.isEmpty())
            return;
        for (auto& block : m_function.blocks) {
            for (auto& target : block.terminator.targets) {
                auto& parameters = m_function.at(target.block).parameters;
                Arguments remainingArguments;
                for (unsigned i = 0; i < target.arguments.size(); ++i) {
                    if (!isRemoved.get(parameters[i].index()))
                        remainingArguments.append(target.arguments[i]);
                }
                if (remainingArguments.size() != target.arguments.size())
                    target.arguments = WTF::move(remainingArguments);
            }
        }
        for (unsigned b = 1; b < m_function.blocks.size(); ++b) {
            auto& block = m_function.blocks[b];
            unsigned before = block.parameters.size();
            block.parameters.removeAllMatching([&](ValueID parameter) {
                return isRemoved.get(parameter.index());
            });
            if (block.parameters.size() == before)
                continue;
            for (unsigned i = 0; i < block.parameters.size(); ++i)
                m_function.at(block.parameters[i]).immediates = { b, i };
        }
        for (size_t parameter : isRemoved) {
            auto& value = m_function.values[parameter];
            value.opcode = Opcode::Undefined;
            value.type = m_program.types.undefined;
            value.rep = repOf(m_program.at(value.type));
            value.setPrediction({ });
            value.setPredictedClosure({ });
            value.immediates = { Format::none, Format::none };
        }
    }

    const Program& m_program;
    FunctionID m_id;
    Function m_function;
    const BitVector& m_returnsStrings;
    const BitVector& m_escapeHTMLAliases;
    unsigned m_originalValueCount { 0 };
    bool m_isCommitting { false };
    Vector<uint32_t> m_useCounts;
    Vector<uint32_t> m_blockOf;
    Vector<uint32_t> m_indexOf;
    Vector<Vector<unsigned, 2>> m_predecessors;
    BitVector m_isReachable;
    Vector<unsigned> m_dominatorPreorder;
    Vector<unsigned> m_dominatorPostorder;
    BitVector m_isString;
    BitVector m_isLoopHeader;
    BitVector m_isInLoop;
    BitVector m_isAccumulator;
    std::optional<StringBuilderAnalysis> m_builders;
    BitVector m_keys;
    BitVector m_mustMaterialize;
    BitVector m_isNeverMeasured;
    Vector<Insertion> m_insertions;
    Vector<Value> m_newValues;
    BitVector m_isDead;
    Vector<ValueID> m_deadLog;
    UncheckedKeyHashMap<uint32_t, std::unique_ptr<BufferedValue>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_bufferedValues;
    Vector<ValueID> m_bufferedLog;
    Vector<uint32_t> m_accounted;
    Vector<ValueID> m_accountedLog;
    Vector<std::pair<ValueID, Point>> m_conversions;
    Vector<GuardedSite> m_guardedSites;
    Vector<std::pair<ValueID, ValueID>> m_finishedRoots;
    Vector<std::pair<ValueID, ValueID>> m_replacements;
    Vector<unsigned> m_postOrder;
    Vector<uint32_t> m_continuationUses;
    ValueID m_conflict;
    unsigned m_nextState { firstFreeState };
    unsigned m_nextRegion { noRegion + 1 };
    unsigned m_loopCount { 0 };
    unsigned m_placementDepth { 0 };
};

bool maySuspend(const Program& program, const Function& function)
{
    if (function.has(FunctionIsPartOfHiddenCoroutine | FunctionIsPartOfAsyncFunction | Format::FunctionIsAsync | Format::FunctionIsGenerator) || function.resumeFunction || function.startFunction || function.savesFrameOnSuspend || program.isResumeFunction(function))
        return true;
    for (auto& value : function.values) {
        if (value.opcode == Opcode::Await || value.opcode == Opcode::Yield || value.opcode == Opcode::GetResumeMode)
            return true;
    }
    return false;
}

bool mayReturnString(const Program& program, FunctionID id, const Function& function)
{
    if (function.blocks.isEmpty() || !program.hasCode(id))
        return false;
    bool returns = false;
    for (auto& block : function.blocks) {
        if (block.terminator.kind != TerminatorKind::Return)
            continue;
        if (!block.terminator.value)
            return false;
        auto& type = program.typeOf(function, block.terminator.value);
        if (!(type.primitives & Primitive::String))
            return false;
        if (type.hasObjects()) {
            ValueID returned = block.terminator.value;
            while (function.at(returned).opcode == Opcode::Cast)
                returned = function.at(returned).child(0);
            switch (function.at(returned).opcode) {
            case Opcode::Add:
            case Opcode::Parameter:
            case Opcode::CallDirect:
                break;
            default:
                return false;
            }
        }
        returns = true;
    }
    return returns;
}

bool returnsOnlyStrings(const Program& program, const Function& function, const BitVector& returnsStrings)
{
    BitVector isReachable = reachableBlocks(function);
    BitVector isString = computeKnownStrings(program, function, isReachable, returnsStrings);
    bool returns = false;
    for (unsigned b = 0; b < function.blocks.size(); ++b) {
        auto& terminator = function.blocks[b].terminator;
        if (!isReachable.get(b) || terminator.kind != TerminatorKind::Return)
            continue;
        if (!isString.get(terminator.value.index()))
            return false;
        returns = true;
    }
    return returns;
}

bool mayBenefit(const Program& program, const Function& function, const BitVector& escapeHTMLAliases)
{
    unsigned operandCount = 0;
    bool hasConcatenation = false;
    bool hasOtherReason = false;
    for (auto& value : function.values) {
        switch (value.opcode) {
        case Opcode::Strcat:
            hasConcatenation = true;
            operandCount += value.arguments.size();
            break;
        case Opcode::Add:
            if (program.at(value.type).primitives & Primitive::String) {
                hasConcatenation = true;
                ++operandCount;
            }
            break;
        case Opcode::GetModuleVariable:
            hasOtherReason |= escapeHTMLAliases.get(value.variable().index());
            break;
        case Opcode::Parameter:
            hasOtherReason |= value.parameterBlock().index() && program.at(value.type).primitives & Primitive::String && !program.at(value.type).hasObjects();
            break;
        default:
            break;
        }
    }
    return hasConcatenation && (hasOtherReason || operandCount >= minPartsWithoutCalls);
}

} // anonymous namespace

void planStringAppends(Program& program, Vector<String>&)
{
    constexpr unsigned maxRounds = 6;
    unsigned count = program.functions.size();
    auto& functions = std::as_const(program).functions;
    Vector<uint8_t> results(FillWith { }, count, static_cast<uint8_t>(0));
    auto collectInParallel = [&](const auto& ask, const auto& take) {
        forEachIndexInParallel(count, [&](unsigned i) {
            results[i] = ask(i);
        });
        bool any = false;
        for (unsigned i = 0; i < count; ++i) {
            if (results[i]) {
                take(i);
                any = true;
            }
        }
        return any;
    };

    Vector<unsigned> undecidedCandidates;
    collectInParallel([&](unsigned i) {
        return mayReturnString(program, FunctionID(i), functions[i]);
    }, [&](unsigned i) {
        undecidedCandidates.append(i);
    });
    BitVector returnsStrings(count);
    for (unsigned round = 0; round < maxRounds && !undecidedCandidates.isEmpty(); ++round) {
        forEachIndexInParallel(undecidedCandidates.size(), [&](unsigned k) {
            unsigned i = undecidedCandidates[k];
            results[i] = returnsOnlyStrings(program, functions[i], returnsStrings);
        });
        size_t numberBefore = undecidedCandidates.size();
        undecidedCandidates.removeAllMatching([&](unsigned i) {
            if (!results[i])
                return false;
            returnsStrings.set(i);
            return true;
        });
        if (undecidedCandidates.size() == numberBefore)
            break;
    }

    BitVector escapeHTMLAliases = findEscapeHTMLAliases(program);
    Vector<std::unique_ptr<StringAppendPlanner>> plans(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        static constexpr OpcodeSet concatenations { Opcode::Strcat, Opcode::Add };
        if (!functions.mayContain(i, concatenations))
            return;
        auto& function = functions[i];
        if (function.blocks.isEmpty() || !program.hasCode(FunctionID(i)) || !mayBenefit(program, function, escapeHTMLAliases) || maySuspend(program, function))
            return;
        auto planner = makeUniqueWithoutFastMallocCheck<StringAppendPlanner>(program, FunctionID(i), function, returnsStrings, escapeHTMLAliases);
        if (planner->run())
            plans[i] = WTF::move(planner);
    });
    Vector<std::unique_ptr<Function>> planned(count);
    forEachIndexInParallel(count, [&](unsigned i) {
        if (!plans[i])
            return;
        auto function = makeUniqueWithoutFastMallocCheck<Function>(std::exchange(plans[i], nullptr)->commit());
        Vector<String> findings;
        verify(program, FunctionID(i), *function, findings);
        if (findings.isEmpty())
            planned[i] = WTF::move(function);
        else
            dataLogLnIf(Options::verboseAOTCompilation(), "AOT: dropped the string append plan of function ", i, ": ", findings[0]);
    });
    forEachIndexInParallel(count, [&](unsigned i) {
        if (planned[i])
            program.functions[i] = WTF::move(*planned[i]);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
