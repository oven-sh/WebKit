/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include "AOTTypedThreads.h"
#include "Options.h"
#include <wtf/Lock.h>
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT { namespace Typed {
namespace {
bool isTransparentCast(const Program& program, const Value& value)
{
    if (value.opcode != Opcode::Cast)
        return false;
    auto& type = program.at(value.type);
    if (!type.hasObjects())
        return false;
    if (value.castReason() != CastReason::As && value.castReason() != CastReason::Coerce)
        return true;
    if (value.castReason() == CastReason::Coerce && type.anyObject && program.at(value.coercedType()).objectLayouts.isEmpty())
        return true;
    return !type.anyObject && type.objectLayouts.isEmpty() && type.engineClasses.isEmpty();
}

ValueID originOf(const Program& program, const Function& function, ValueID id)
{
    while (isTransparentCast(program, function.at(id)))
        id = function.at(id).child(0);
    return id;
}

template<typename Is>
bool isUsedOnlyAsCallee(const Program& program, const Function& function, const Value& user, const Is& is)
{
    auto isAmong = [&](std::span<const ValueID> operands) {
        return std::ranges::any_of(operands, is);
    };
    auto hasNoObjects = [&](ValueID id) {
        return !program.typeOf(function, id).hasObjects();
    };
    switch (user.opcode) {
    case Opcode::AssertNotAllocated:
    case Opcode::IsClosureOf:
    case Opcode::GetCapture:
    case Opcode::IsUndefinedOrNull:
    case Opcode::ToBoolean:
    case Opcode::TypeOf:
    case Opcode::TypeOfIs:
    case Opcode::HasShapeOf:
    case Opcode::IsInstanceOfClass:
        return true;
    case Opcode::Cast:
        return user.castReason() != CastReason::As && user.castReason() != CastReason::Coerce;
    case Opcode::StrictEq:
        return hasNoObjects(is(user.child(0)) ? user.child(1) : user.child(0));
    case Opcode::Eq:
        return program.typeOf(function, is(user.child(0)) ? user.child(1) : user.child(0)).isUndefinedOrNull();
    case Opcode::Call:
        return !(user.child(1) && is(user.child(1))) && !isAmong(user.arguments.span());
    case Opcode::CallMethod:
        return function.copiesFirstArgumentToHeap && !program.typeOf(function, user.child(0)).singleLayout() && user.arguments.size() <= numberOfArgumentGPRs && !is(user.child(0)) && !isAmong(user.arguments.span().subspan(1));
    case Opcode::CallBuiltin: {
        auto& implementation = Builtins::singleton().at(user.builtin()).implementation;
        if (implementation.kind != BuiltinImplementation::Inline || (user.child(0) && is(user.child(0))))
            return false;
        if (implementation.inlined == InlineBuiltin::IsCallable)
            return true;
        return implementation.inlined == InlineBuiltin::Call && !isAmong(user.arguments.span().subspan(1));
    }
    case Opcode::CallDirect: {
        auto& callee = program.at(user.function());
        unsigned first = callee.captureTypes.size() + callee.has(Format::FunctionHasThis);
        for (unsigned i = 0; i < user.arguments.size(); ++i) {
            if (!is(user.arguments[i]))
                continue;
            if (i < callee.captureTypes.size() ? !callee.calledOnlyCaptures.get(i) : i < first || !callee.calledOnlyParameters.get(i - first))
                return false;
        }
        return true;
    }
    default:
        return false;
    }
}

template<typename Is>
bool isCapturedOnlyToBeCalled(const Program& program, const Value& closure, const Is& is)
{
    auto& callee = program.at(closure.function());
    if (std::ranges::any_of(closure.children, [&](ValueID child) { return child && is(child); }))
        return false;
    for (unsigned k = 0; k < closure.arguments.size(); ++k) {
        if (is(closure.arguments[k]) && (k >= callee.captureTypes.size() || !callee.calledOnlyCaptures.get(k)))
            return false;
    }
    return true;
}

bool canStackAllocateClosuresOf(const Program& program, const Function& callee)
{
    if ((callee.kind != FunctionKind::Plain && callee.kind != FunctionKind::Arrow) || callee.declaredAs || callee.takesCalleeClosure || callee.resumeFunction || program.isResumeFunction(callee))
        return false;
    return std::ranges::none_of(callee.values, [&](const Value& value) {
        if (value.opcode == Opcode::GetMyArgumentByVal)
            return value.readsArgumentsObject();
        return (value.opcode == Opcode::Operation || value.opcode == Opcode::CallBuiltin) && languageOperationOf(program, value) == LanguageOperation::CreateArguments;
    });
}

BitVector blocksInCycles(const Function& function)
{
    unsigned numberOfBlocks = function.blocks.size();
    BitVector result(numberOfBlocks);
    Vector<unsigned> order;
    Vector<unsigned> lowest;
    order.fill(0, numberOfBlocks);
    lowest.fill(0, numberOfBlocks);
    BitVector isOnStack(numberOfBlocks);
    Vector<unsigned> stack;
    Vector<std::pair<unsigned, unsigned>> visiting;
    unsigned next = 0;
    for (unsigned root = 0; root < numberOfBlocks; ++root) {
        if (order[root])
            continue;
        visiting.append({ root, 0 });
        while (!visiting.isEmpty()) {
            auto [b, k] = visiting.last();
            if (!k) {
                order[b] = lowest[b] = ++next;
                stack.append(b);
                isOnStack.quickSet(b);
            }
            auto& targets = function.blocks[b].terminator.targets;
            if (k < targets.size()) {
                ++visiting.last().second;
                unsigned to = targets[k].block.index();
                if (to == b)
                    result.quickSet(b);
                if (!order[to])
                    visiting.append({ to, 0 });
                else if (isOnStack.quickGet(to))
                    lowest[b] = std::min(lowest[b], order[to]);
                continue;
            }
            visiting.removeLast();
            if (!visiting.isEmpty())
                lowest[visiting.last().first] = std::min(lowest[visiting.last().first], lowest[b]);
            if (lowest[b] != order[b])
                continue;
            bool isSingleton = stack.last() == b;
            for (;;) {
                unsigned member = stack.takeLast();
                isOnStack.quickClear(member);
                if (!isSingleton)
                    result.quickSet(member);
                if (member == b)
                    break;
            }
        }
    }
    return result;
}

template<typename OriginOf, typename IsClosureInFrame>
bool mayOutliveLoopIteration(const Function& function, ValueID allocationSite, unsigned allocationBlock, const OriginOf& originOf, const IsClosureInFrame& isStackAllocatedClosure)
{
    BitVector isAlias(function.values.size());
    isAlias.quickSet(allocationSite.index());
    Vector<std::pair<ValueID, unsigned>, 4> aliases;
    for (bool changed = true; changed;) {
        changed = false;
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                if (!isStackAllocatedClosure(id) || isAlias.quickGet(id.index()))
                    continue;
                if (std::ranges::any_of(value.arguments, [&](ValueID captured) { return isAlias.quickGet(originOf(captured).index()); })) {
                    isAlias.quickSet(id.index());
                    changed = true;
                }
            }
            for (auto& target : block.terminator.targets) {
                auto& parameters = function.at(target.block).parameters;
                for (unsigned k = 0; k < target.arguments.size(); ++k) {
                    if (isAlias.quickGet(originOf(target.arguments[k]).index()) && !isAlias.quickSet(parameters[k].index())) {
                        aliases.append({ parameters[k], target.block.index() });
                        changed = true;
                    }
                }
            }
        }
    }
    for (auto [alias, definedIn] : aliases) {
        if (definedIn == allocationBlock)
            return true;
        auto isUsedIn = [&, alias = alias](const Block& block) {
            bool result = false;
            auto use = [&](ValueID id) {
                result |= originOf(id) == alias;
            };
            for (ValueID id : block.values)
                function.at(id).forEachChild(use);
            if (block.terminator.value)
                use(block.terminator.value);
            for (auto& target : block.terminator.targets) {
                for (ValueID argument : target.arguments)
                    use(argument);
            }
            return result;
        };
        BitVector seen(function.blocks.size());
        Vector<unsigned> worklist { allocationBlock };
        seen.quickSet(allocationBlock);
        while (!worklist.isEmpty()) {
            auto& block = function.blocks[worklist.takeLast()];
            if (isUsedIn(block))
                return true;
            for (auto& target : block.terminator.targets) {
                unsigned to = target.block.index();
                if (to != definedIn && !seen.quickSet(to))
                    worklist.append(to);
            }
        }
    }
    return false;
}

class EnvironmentMaterialization {
public:
    EnvironmentMaterialization(const Program& program, Function& function, ValueID environment)
        : m_program(program)
        , m_function(function)
        , m_environment(environment)
    {
    }

    bool canRun()
    {
        if (!isProfitable())
            return false;
        Vector<Location, 4> materializations;
        Vector<std::pair<ValueID, Location>, 4> stackAllocatedClosures;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& values = m_function.blocks[b].values;
            for (size_t i = 0; i < values.size(); ++i) {
                auto& value = m_function.at(values[i]);
                if (values[i] == m_environment)
                    m_allocationBlock = b;
                if (requiresHeapEnvironment(value))
                    materializations.append({ b, i });
                else if (value.isStackAllocatedClosure() && value.arguments.contains(m_environment))
                    stackAllocatedClosures.append({ values[i], { b, i } });
            }
        }
        for (auto& [closure, allocationSite] : stackAllocatedClosures) {
            BitVector isAlias = aliasesOf(closure);
            BitVector afterAllocation = blocksReachableFrom(allocationSite.block);
            for (auto& materialization : materializations) {
                if ((materialization.block == allocationSite.block ? materialization.index > allocationSite.index : false) || afterAllocation.quickGet(materialization.block)) {
                    if (isUsedAfter(materialization, isAlias))
                        return false;
                }
            }
        }
        return true;
    }

    void run()
    {
        unsigned numberOfBlocks = m_function.blocks.size();
        unsigned allocationBlock = numberOfBlocks;
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            if (m_function.blocks[b].values.contains(m_environment))
                allocationBlock = b;
        }
        RELEASE_ASSERT(allocationBlock < numberOfBlocks);

        BitVector isNotDominated(numberOfBlocks);
        Vector<unsigned> worklist;
        if (allocationBlock) {
            isNotDominated.quickSet(0);
            worklist.append(0);
        }
        while (!worklist.isEmpty()) {
            for (auto& target : m_function.blocks[worklist.takeLast()].terminator.targets) {
                unsigned to = target.block.index();
                if (to != allocationBlock && !isNotDominated.quickSet(to))
                    worklist.append(to);
            }
        }

        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            auto& values = m_function.blocks[b].values;
            for (size_t i = 0; i < values.size(); ++i) {
                if (!requiresHeapEnvironment(m_function.at(values[i])))
                    continue;
                Value materialize;
                materialize.opcode = Opcode::MaterializeEnvironment;
                materialize.position = m_function.at(values[i]).position;
                materialize.inlineFrame = m_function.at(values[i]).inlineFrame;
                materialize.type = m_function.at(m_environment).type;
                materialize.rep = m_function.at(m_environment).rep;
                materialize.children = { m_environment, m_environment, ValueID() };
                m_function.values.append(WTF::move(materialize));
                values.insert(i++, ValueID(m_function.values.size() - 1));
                worklist.append(b);
            }
        }

        BitVector mayBeMaterialized(numberOfBlocks);
        while (!worklist.isEmpty()) {
            for (auto& target : m_function.blocks[worklist.takeLast()].terminator.targets) {
                unsigned to = target.block.index();
                if (to != allocationBlock && !isNotDominated.quickGet(to) && !mayBeMaterialized.quickSet(to))
                    worklist.append(to);
            }
        }
        BitVector isLiveAtHead(numberOfBlocks);
        Vector<Vector<unsigned, 2>> predecessors(numberOfBlocks);
        Vector<ValueID> lastMaterialization;
        lastMaterialization.fill(ValueID(), numberOfBlocks);
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            if (isNotDominated.quickGet(b))
                continue;
            auto& block = m_function.blocks[b];
            for (auto& target : block.terminator.targets)
                predecessors[target.block.index()].append(b);
            bool uses = false;
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                value.forEachChild([&](ValueID child) {
                    uses |= child == m_environment;
                });
                if (value.opcode == Opcode::MaterializeEnvironment && value.child(1) == m_environment)
                    lastMaterialization[b] = id;
            }
            if (uses && b != allocationBlock && !isLiveAtHead.quickSet(b))
                worklist.append(b);
        }
        while (!worklist.isEmpty()) {
            for (unsigned from : predecessors[worklist.takeLast()]) {
                if (from != allocationBlock && !isLiveAtHead.quickSet(from))
                    worklist.append(from);
            }
        }

        Vector<ValueID> incoming;
        incoming.fill(ValueID(), numberOfBlocks);
        BitVector hasParameter(numberOfBlocks);
        auto outgoing = [&](unsigned b) {
            if (lastMaterialization[b])
                return lastMaterialization[b];
            return b != allocationBlock && mayBeMaterialized.quickGet(b) ? incoming[b] : m_environment;
        };
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t b : mayBeMaterialized) {
                if (!isLiveAtHead.quickGet(b) || hasParameter.quickGet(b))
                    continue;
                ValueID reaching;
                bool isMerge = false;
                for (unsigned from : predecessors[b]) {
                    ValueID value = outgoing(from);
                    if (!value)
                        continue;
                    isMerge |= reaching && reaching != value;
                    reaching = value;
                }
                if (isMerge) {
                    Value parameter;
                    parameter.opcode = Opcode::Parameter;
                    parameter.position = m_function.at(m_environment).position;
                    parameter.inlineFrame = m_function.at(m_environment).inlineFrame;
                    parameter.type = m_function.at(m_environment).type;
                    parameter.rep = m_function.at(m_environment).rep;
                    parameter.immediates = { static_cast<uint32_t>(b), static_cast<uint32_t>(m_function.blocks[b].parameters.size()) };
                    m_function.values.append(WTF::move(parameter));
                    reaching = ValueID(m_function.values.size() - 1);
                    m_function.blocks[b].parameters.append(reaching);
                    hasParameter.quickSet(b);
                }
                if (incoming[b] != reaching) {
                    incoming[b] = reaching;
                    changed = true;
                }
            }
        }
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            if (isNotDominated.quickGet(b))
                continue;
            auto& block = m_function.blocks[b];
            ValueID current = b != allocationBlock && mayBeMaterialized.quickGet(b) ? incoming[b] : m_environment;
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                if (value.opcode == Opcode::MaterializeEnvironment && value.child(1) == m_environment) {
                    RELEASE_ASSERT(current);
                    value.children[0] = current;
                    current = id;
                    continue;
                }
                value.forEachChild([&](ValueID& child) {
                    if (child == m_environment) {
                        RELEASE_ASSERT(current);
                        child = current;
                    }
                });
            }
            for (auto& target : block.terminator.targets) {
                if (hasParameter.quickGet(target.block.index())) {
                    RELEASE_ASSERT(current);
                    target.arguments.append(current);
                }
            }
        }
    }

private:
    bool isProfitable() const
    {
        auto materializations = [&](const Block& block) {
            return block.values.containsIf([&](ValueID id) {
                return requiresHeapEnvironment(m_function.at(id));
            });
        };
        unsigned numberOfBlocks = m_function.blocks.size();
        BitVector seen(numberOfBlocks);
        Vector<unsigned> worklist;
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            if (m_function.blocks[b].values.contains(m_environment)) {
                seen.quickSet(b);
                worklist.append(b);
            }
        }
        unsigned allocationBlock = worklist[0];
        while (!worklist.isEmpty()) {
            auto& block = m_function.blocks[worklist.takeLast()];
            if (materializations(block))
                continue;
            if (block.terminator.kind == TerminatorKind::Return)
                return true;
            for (auto& target : block.terminator.targets) {
                if (target.block.index() == allocationBlock)
                    return true;
                if (!seen.quickSet(target.block.index()))
                    worklist.append(target.block.index());
            }
        }
        return false;
    }

    bool requiresHeapEnvironment(const Value& value) const
    {
        return value.opcode == Opcode::NewClosure && !value.isStackAllocatedClosure() && value.arguments.contains(m_environment);
    }

    struct Location {
        unsigned block;
        size_t index;
    };

    BitVector aliasesOf(ValueID closure) const
    {
        BitVector result(m_function.values.size());
        result.quickSet(closure.index());
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& block : m_function.blocks) {
                for (ValueID id : block.values) {
                    auto& value = m_function.at(id);
                    if (!value.isStackAllocatedClosure() || result.quickGet(id.index()))
                        continue;
                    if (std::ranges::any_of(value.arguments, [&](ValueID captured) { return result.quickGet(originOf(m_program, m_function, captured).index()); })) {
                        result.quickSet(id.index());
                        changed = true;
                    }
                }
                for (auto& target : block.terminator.targets) {
                    auto& parameters = m_function.at(target.block).parameters;
                    for (unsigned k = 0; k < target.arguments.size(); ++k) {
                        if (result.quickGet(originOf(m_program, m_function, target.arguments[k]).index()) && !result.quickSet(parameters[k].index()))
                            changed = true;
                    }
                }
            }
        }
        return result;
    }

    BitVector blocksReachableFrom(unsigned from) const
    {
        BitVector result(m_function.blocks.size());
        Vector<unsigned> worklist { from };
        while (!worklist.isEmpty()) {
            for (auto& target : m_function.blocks[worklist.takeLast()].terminator.targets) {
                if (target.block.index() != m_allocationBlock && !result.quickSet(target.block.index()))
                    worklist.append(target.block.index());
            }
        }
        return result;
    }

    bool isUsedAfter(Location location, const BitVector& isAlias) const
    {
        auto isUsedIn = [&](const Block& block, size_t first) {
            bool result = false;
            auto use = [&](ValueID id) {
                result |= isAlias.quickGet(originOf(m_program, m_function, id).index());
            };
            for (size_t i = first; i < block.values.size(); ++i)
                m_function.at(block.values[i]).forEachChild(use);
            if (block.terminator.value)
                use(block.terminator.value);
            for (auto& target : block.terminator.targets) {
                for (ValueID argument : target.arguments)
                    use(argument);
            }
            return result;
        };
        if (isUsedIn(m_function.blocks[location.block], location.index + 1))
            return true;
        for (size_t b : blocksReachableFrom(location.block)) {
            if (isUsedIn(m_function.blocks[b], 0))
                return true;
        }
        return false;
    }

    const Program& m_program;
    Function& m_function;
    ValueID m_environment;
    unsigned m_allocationBlock { UINT_MAX };
};

bool readsExtraArguments(const Program& program, size_t index)
{
    static constexpr OpcodeSet mayReadExtraArguments { Opcode::GetArgument, Opcode::CreateRest, Opcode::GetRestLength, Opcode::GetMyArgumentByVal, Opcode::CallForwardVarargs, Opcode::ConstructSuperForwardVarargs, Opcode::Operation, Opcode::CallBuiltin };
    bool result = false;
    program.functions.forEachValue(index, mayReadExtraArguments, [&](ValueID id) {
        auto& value = program.functions[index].at(id);
        result |= (value.opcode != Opcode::Operation && value.opcode != Opcode::CallBuiltin) || languageOperationOf(program, value) == LanguageOperation::CreateArguments;
    });
    return result;
}

uint64_t operandsWithOtherUses(const Program& program, const Function& function, std::span<const ValueID> operands)
{
    RELEASE_ASSERT(operands.size() <= 64);
    Vector<uint64_t> mayBe;
    mayBe.fill(0, function.values.size());
    for (size_t i = 0; i < operands.size(); ++i)
        mayBe[operands[i].index()] |= 1ull << i;
    auto possibleObjectsOf = [&](ValueID id) -> uint64_t {
        uint64_t result = mayBe[originOf(program, function, id).index()];
        return result && program.typeOf(function, id).hasObjects() ? result : 0;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (auto& block : function.blocks) {
            for (auto& target : block.terminator.targets) {
                auto& parameters = function.at(target.block).parameters;
                for (unsigned k = 0; k < target.arguments.size(); ++k) {
                    uint64_t more = possibleObjectsOf(target.arguments[k]) & ~mayBe[parameters[k].index()];
                    mayBe[parameters[k].index()] |= more;
                    changed |= !!more;
                }
            }
        }
    }
    uint64_t hasOtherUses = 0;
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            auto& user = function.at(id);
            if (isTransparentCast(program, user))
                continue;
            uint64_t used = 0;
            user.forEachChild([&](ValueID child) {
                used |= possibleObjectsOf(child);
            });
            for (used &= ~hasOtherUses; used; used &= used - 1) {
                uint64_t one = used & -used;
                auto is = [&](ValueID child) {
                    return !!(possibleObjectsOf(child) & one);
                };
                if (!isUsedOnlyAsCallee(program, function, user, is))
                    hasOtherUses |= one;
            }
        }
        auto& terminator = block.terminator;
        if (terminator.value && (terminator.kind == TerminatorKind::Return || terminator.kind == TerminatorKind::Throw))
            hasOtherUses |= possibleObjectsOf(terminator.value);
    }
    return hasOtherUses;
}

} // anonymous namespace

void findCalledOnlyOperands(Program& program)
{
    if (Options::validateAOTTypedFacts() || Options::auditAOTTypedAssertions())
        return;
    struct Operand {
        unsigned index;
        bool isCapture;
    };
    auto forEachCalledOnlyOperand = [](const Function& function, const auto& functor) {
        auto& entryParameters = function.blocks[0].parameters;
        for (size_t k : function.calledOnlyCaptures)
            functor(entryParameters[k], Operand { static_cast<unsigned>(k), true });
        unsigned first = function.captureTypes.size() + function.has(Format::FunctionHasThis);
        for (size_t k : function.calledOnlyParameters)
            functor(entryParameters[first + k], Operand { static_cast<unsigned>(k), false });
    };
    unsigned count = program.functions.size();
    unsigned numberOfOperands = 0;
    Vector<Vector<unsigned>> callees(count);
    Vector<uint8_t> calledOnlyCounts(FillWith { }, count, static_cast<uint8_t>(0));
    forEachIndexInParallel(count, [&](unsigned i) {
        auto& function = program.functions.atKeepingValueIndex(i);
        function.calledOnlyCaptures.clearAll();
        function.calledOnlyParameters.clearAll();
        if (!program.hasCode(FunctionID(i)) || function.blocks.isEmpty())
            return;
        auto& entryParameters = function.blocks[0].parameters;
        unsigned first = function.captureTypes.size() + function.has(Format::FunctionHasThis);
        unsigned calledOnlyCount = 0;
        auto canBeCalledOnly = [&](size_t location) {
            if (location >= entryParameters.size() || calledOnlyCount == 64)
                return false;
            auto& type = program.typeOf(function, entryParameters[location]);
            if (!type.anyFunction && !type.anyObject)
                return false;
            ++calledOnlyCount;
            return true;
        };
        for (size_t k = 0; k < function.captureTypes.size(); ++k) {
            if (canBeCalledOnly(k))
                function.calledOnlyCaptures.set(k);
        }
        for (size_t k = 0; k < function.parameterTypes.size(); ++k) {
            if (canBeCalledOnly(first + k))
                function.calledOnlyParameters.set(k);
        }
        if (!calledOnlyCount)
            return;
        if (readsExtraArguments(program, i)) {
            function.calledOnlyCaptures.clearAll();
            function.calledOnlyParameters.clearAll();
            return;
        }
        calledOnlyCounts[i] = calledOnlyCount;
        static constexpr OpcodeSet directCalls { Opcode::CallDirect };
        std::as_const(program).functions.forEachValue(i, directCalls, [&](ValueID id) {
            callees[i].append(function.at(id).function().index());
        });
        std::ranges::sort(callees[i]);
        callees[i].shrink(std::ranges::unique(callees[i]).begin() - callees[i].begin());
    });
    Vector<unsigned> candidates;
    for (unsigned i = 0; i < count; ++i) {
        numberOfOperands += calledOnlyCounts[i];
        if (calledOnlyCounts[i])
            candidates.append(i);
    }
    Vector<uint8_t> hasChanged;
    Vector<uint8_t> isScanned;
    Vector<uint64_t> hasOtherUses;
    hasChanged.fill(0, count);
    isScanned.fill(0, count);
    hasOtherUses.fill(0, count);
    unsigned unchangedCount = numberOfOperands;
    unsigned numberOfRounds = 0;
    unsigned numberOfScans = 0;
    for (bool changed = true; changed;) {
        bool isFirstRound = !numberOfRounds++;
        forEachIndexInParallel(candidates.size(), [&](unsigned k) {
            unsigned i = candidates[k];
            auto& function = std::as_const(program).functions[i];
            hasOtherUses[i] = 0;
            isScanned[i] = 0;
            if (function.calledOnlyParameters.isEmpty() && function.calledOnlyCaptures.isEmpty())
                return;
            if (!isFirstRound && std::ranges::none_of(callees[i], [&](unsigned callee) { return hasChanged[callee]; }))
                return;
            Vector<ValueID, 16> operands;
            forEachCalledOnlyOperand(function, [&](ValueID operand, Operand) {
                operands.append(operand);
            });
            isScanned[i] = 1;
            hasOtherUses[i] = operandsWithOtherUses(program, function, operands.span());
        });
        changed = false;
        for (unsigned i : candidates) {
            numberOfScans += isScanned[i];
            hasChanged[i] = !!hasOtherUses[i];
            changed |= !!hasOtherUses[i];
            unchangedCount -= std::popcount(hasOtherUses[i]);
        }
        forEachIndexInParallel(candidates.size(), [&](unsigned k) {
            unsigned i = candidates[k];
            if (!hasOtherUses[i])
                return;
            auto& function = program.functions.atKeepingValueIndex(i);
            Vector<Operand, 16> said;
            forEachCalledOnlyOperand(function, [&](ValueID, Operand operand) {
                said.append(operand);
            });
            for (uint64_t rest = hasOtherUses[i]; rest; rest &= rest - 1) {
                auto& operand = said[WTF::ctz(rest)];
                (operand.isCapture ? function.calledOnlyCaptures : function.calledOnlyParameters).clear(operand.index);
            }
        });
    }
    destroyOnHelperThread(WTF::move(callees));
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: of ", numberOfOperands, " operands that may hold objects or functions, ", unchangedCount, " are proven to be only called, in ", numberOfRounds, " rounds and ", numberOfScans, " function scans");
}

void verifyCalledOnlyParameters(const Program& program, Vector<String>& findings)
{
    if (Options::validateAOTTypedFacts() || Options::auditAOTTypedAssertions())
        return;
    Lock lock;
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = program.functions[i];
        if (!program.hasCode(FunctionID(i)) || (function.calledOnlyParameters.isEmpty() && function.calledOnlyCaptures.isEmpty()) || function.blocks.isEmpty())
            return;
        auto& entryParameters = function.blocks[0].parameters;
        BitVector derivesFromParameter(function.values.size());
        for (size_t k : function.calledOnlyCaptures) {
            if (k < function.captureTypes.size() && k < entryParameters.size())
                derivesFromParameter.quickSet(entryParameters[k].index());
        }
        unsigned first = function.captureTypes.size() + function.has(Format::FunctionHasThis);
        for (size_t k : function.calledOnlyParameters) {
            if (first + k < entryParameters.size())
                derivesFromParameter.quickSet(entryParameters[first + k].index());
        }
        auto is = [&](ValueID id) {
            return derivesFromParameter.quickGet(originOf(program, function, id).index()) && program.typeOf(function, id).hasObjects();
        };
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& block : function.blocks) {
                for (auto& target : block.terminator.targets) {
                    auto& parameters = function.at(target.block).parameters;
                    for (unsigned k = 0; k < target.arguments.size(); ++k) {
                        if (is(target.arguments[k]) && !derivesFromParameter.quickSet(parameters[k].index()))
                            changed = true;
                    }
                }
            }
        }
        auto report = [&](ASCIILiteral operation, const Value& at) {
            StringPrintStream out;
            ModuleID module = at.inlineFrame ? program.at(function.inlineFrames[at.inlineFrame].callee).module : function.module;
            if (module) {
                auto location = program.lineColumnFor(module, at.position);
                auto& described = program.modules[module.index()];
                out.print(String::fromUTF8(program.string(described.displayPath ? described.displayPath : described.path)), ":", location.line, ":", location.column, ": ");
            }
            out.print("f", i, " ", String::fromUTF8(program.string(function.name)), ": a parameter that is said to be only called comes to ", operation);
            Locker locker { lock };
            findings.append(out.toString());
        };
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& user = function.at(id);
                if (isTransparentCast(program, user))
                    continue;
                bool uses = false;
                user.forEachChild([&](ValueID child) {
                    uses |= is(child);
                });
                if (uses && !isUsedOnlyAsCallee(program, function, user, is))
                    report(nameOf(user.opcode), user);
            }
            auto& terminator = block.terminator;
            if (terminator.value && (terminator.kind == TerminatorKind::Return || terminator.kind == TerminatorKind::Throw) && is(terminator.value))
                report(terminator.kind == TerminatorKind::Return ? "Return"_s : "Throw"_s, function.at(terminator.value));
        }
    });
    std::ranges::sort(findings, [](const String& a, const String& b) {
        return codePointCompareLessThan(a, b);
    });
}

void findStackAllocatedClosures(Program& program)
{
    if (Options::validateAOTTypedFacts() || Options::auditAOTTypedAssertions())
        return;
    Vector<uint8_t> mayBeStackAllocatedLayouts(FillWith { }, program.functions.size(), static_cast<uint8_t>(0));
    static constexpr OpcodeSet closures { Opcode::NewClosure, Opcode::NewClosureOnce };
    Vector<uint8_t> hasClosureAllocations(FillWith { }, program.functions.size(), static_cast<uint8_t>(0));
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        if (!program.hasCode(FunctionID(i)))
            return;
        auto& function = std::as_const(program).functions[i];
        std::as_const(program).functions.forEachIndexedValue(i, closures, [&](ValueID id) {
            WTF::atomicStore(&hasClosureAllocations[function.at(id).function().index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
        });
    });
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        if (hasClosureAllocations[i])
            mayBeStackAllocatedLayouts[i] = canStackAllocateClosuresOf(program, std::as_const(program).functions[i]);
    });
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        if (!program.hasCode(FunctionID(i)))
            return;
        if (!program.functions.mayContain(i, closures)) {
            program.functions.atKeepingValueIndex(i).mayPointIntoFrame.clearAll();
            return;
        }
        auto& function = program.functions.atKeepingValueIndex(i);
        function.mayPointIntoFrame.clearAll();
        BitVector isCandidate(function.values.size());
        Vector<std::pair<ValueID, unsigned>> candidates;
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            for (ValueID id : function.blocks[b].values) {
                auto& value = function.at(id);
                if (value.opcode == Opcode::NewClosure)
                    value.immediates[1] = Format::none;
                else if (value.opcode != Opcode::NewClosureOnce)
                    continue;
                if (mayBeStackAllocatedLayouts[value.function().index()]) {
                    isCandidate.quickSet(id.index());
                    candidates.append({ id, b });
                }
            }
        }
        if (candidates.isEmpty())
            return;

        BitVector mayAlias = isCandidate;
        BitVector flowsThroughPhi(function.values.size());
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& block : function.blocks) {
                for (auto& target : block.terminator.targets) {
                    auto& parameters = function.at(target.block).parameters;
                    for (unsigned k = 0; k < target.arguments.size(); ++k) {
                        ValueID origin = originOf(program, function, target.arguments[k]);
                        if (!mayAlias.quickGet(origin.index()))
                            continue;
                        flowsThroughPhi.quickSet(origin.index());
                        if (!mayAlias.quickSet(parameters[k].index()))
                            changed = true;
                    }
                }
            }
        }
        struct Capture {
            ValueID captured;
            ValueID closure;
        };
        Vector<Capture, 4> captures;
        BitVector isEscaping(function.values.size());
        BitVector exposesCaptures(function.values.size());
        for (auto& block : function.blocks) {
            for (ValueID id : block.values) {
                auto& user = std::as_const(function).at(id);
                if (isTransparentCast(program, user))
                    continue;
                user.forEachChild([&](ValueID child) {
                    ValueID alias = originOf(program, function, child);
                    if (!mayAlias.quickGet(alias.index()) || isEscaping.quickGet(alias.index()))
                        return;
                    auto is = [&](ValueID operand) {
                        return originOf(program, function, operand) == alias;
                    };
                    if (isCandidate.quickGet(id.index()) && isCapturedOnlyToBeCalled(program, user, is)) {
                        captures.append({ alias, id });
                        return;
                    }
                    if (user.opcode == Opcode::GetCapture || (user.opcode == Opcode::CallDirect && program.at(user.function()).copiesFirstArgumentToHeap))
                        exposesCaptures.quickSet(alias.index());
                    if (user.mustBeProperTailCall || !isUsedOnlyAsCallee(program, function, user, is))
                        isEscaping.quickSet(alias.index());
                });
            }
            if (block.terminator.value)
                isEscaping.quickSet(originOf(program, function, block.terminator.value).index());
        }
        std::optional<BitVector> inCycles;
        auto originInFunction = [&](ValueID operand) {
            return originOf(program, function, operand);
        };
        auto canBeStackAllocated = [&](ValueID id) {
            return isCandidate.quickGet(id.index()) && !isEscaping.quickGet(id.index());
        };
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& block : function.blocks) {
                for (auto& target : block.terminator.targets) {
                    auto& parameters = function.at(target.block).parameters;
                    for (unsigned k = 0; k < target.arguments.size(); ++k) {
                        ValueID origin = originInFunction(target.arguments[k]);
                        if (isEscaping.quickGet(parameters[k].index()) && !isEscaping.quickSet(origin.index()))
                            changed = true;
                        if (exposesCaptures.quickGet(parameters[k].index()) && !exposesCaptures.quickSet(origin.index()))
                            changed = true;
                    }
                }
            }
            for (auto [captured, closure] : captures) {
                if ((isEscaping.quickGet(closure.index()) || exposesCaptures.quickGet(closure.index())) && !isEscaping.quickSet(captured.index()))
                    changed = true;
                if (flowsThroughPhi.quickGet(closure.index()) && !flowsThroughPhi.quickSet(captured.index()))
                    changed = true;
            }
            if (changed)
                continue;
            for (auto [id, b] : candidates) {
                if (isEscaping.quickGet(id.index()) || !flowsThroughPhi.quickGet(id.index()))
                    continue;
                if (!inCycles)
                    inCycles = blocksInCycles(function);
                if (inCycles->quickGet(b) && mayOutliveLoopIteration(function, id, b, originInFunction, canBeStackAllocated)) {
                    isEscaping.quickSet(id.index());
                    changed = true;
                }
            }
        }
        BitVector isStackAllocated(function.values.size());
        for (auto [id, b] : candidates) {
            UNUSED_VARIABLE(b);
            if (isEscaping.quickGet(id.index()))
                continue;
            auto& value = function.at(id);
            if (value.opcode != Opcode::NewClosure)
                program.functions.invalidateValueIndex(i);
            value.opcode = Opcode::NewClosure;
            value.children[0] = ValueID();
            value.immediates[1] = closureIsStackAllocated;
            isStackAllocated.quickSet(id.index());
        }
        if (isStackAllocated.isEmpty())
            return;
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& block : function.blocks) {
                for (auto& target : block.terminator.targets) {
                    auto& parameters = function.at(target.block).parameters;
                    for (unsigned k = 0; k < target.arguments.size(); ++k) {
                        if (isStackAllocated.quickGet(originOf(program, function, target.arguments[k]).index()) && !isStackAllocated.quickSet(parameters[k].index()))
                            changed = true;
                    }
                }
            }
        }
        function.mayPointIntoFrame = WTF::move(isStackAllocated);
    });
}

void findStackAllocatedEnvironments(Program& program, Vector<String>& findings)
{
    if (Options::validateAOTTypedFacts() || Options::auditAOTTypedAssertions())
        return;
    static constexpr unsigned maxFields = 32;
    enum : uint8_t { Unknown, No, Yes };
    Vector<uint8_t> acceptsEnvironment;
    acceptsEnvironment.fill(Unknown, program.types.size());
    Vector<uint8_t> doesEscape(FillWith { }, program.layouts.size(), static_cast<uint8_t>(0));
    struct AllocationSite {
        unsigned function;
        ValueID environment;
        unsigned block;
        bool isCapturedByHeapClosure;
        bool flowsThroughPhi;
    };
    Vector<Vector<AllocationSite>> allocationsByFunction(program.functions.size());

    auto scan = [&](unsigned i) {
        if (!program.hasCode(FunctionID(i)))
            return;
        auto& function = program.functions.atKeepingValueIndex(i);
        auto& allocationSites = allocationsByFunction[i];
        function.stackAllocatedEnvironments.clearAll();
        BitVector isCapturedByHeapClosure;
        BitVector flowsThroughPhi;
        auto markEscaped = [&](ValueID id, ASCIILiteral operation, const Value& at) {
            TypeID type = function.at(id).type;
            if (!type)
                return;
            uint8_t known = WTF::atomicLoad(&acceptsEnvironment[type.index()], std::memory_order_relaxed);
            if (known == Unknown) {
                known = std::ranges::any_of(program.at(type).objectLayouts, [&](LayoutID supertype) {
                    return program.at(supertype).kind == LayoutKind::Environment;
                }) ? Yes : No;
                WTF::atomicStore(&acceptsEnvironment[type.index()], known, std::memory_order_relaxed);
            }
            if (known == No)
                return;
            for (LayoutID supertype : program.at(type).objectLayouts) {
                if (!WTF::atomicLoad(&doesEscape[supertype.index()], std::memory_order_relaxed) && !WTF::atomicExchange(&doesEscape[supertype.index()], static_cast<uint8_t>(1), std::memory_order_relaxed) && program.recordsAllocations)
                    program.environmentHeapReasons.add(supertype.index(), Program::EnvironmentUse { operation, FunctionID(i), at.inlineFrame, at.position });
            }
        };
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            auto& block = function.blocks[b];
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                auto markOperandEscaped = [&](ValueID operand) {
                    markEscaped(operand, nameOf(value.opcode), value);
                };
                switch (value.opcode) {
                case Opcode::NewObjectWithShape:
                    if (program.at(value.layout()).kind == LayoutKind::Environment)
                        allocationSites.append({ i, id, b, false, false });
                    value.forEachChild(markOperandEscaped);
                    break;
                case Opcode::GetField:
                    markOperandEscaped(id);
                    break;
                case Opcode::PutField:
                    markOperandEscaped(value.child(1));
                    break;
                case Opcode::CallDirect:
                    markOperandEscaped(id);
                    if (value.mustBeProperTailCall)
                        value.forEachChild(markOperandEscaped);
                    break;
                case Opcode::GetCapture:
                case Opcode::AssertNotAllocated:
                    break;
                case Opcode::NewClosure:
                    if (value.isStackAllocatedClosure())
                        break;
                    for (ValueID captured : value.arguments) {
                        auto& created = function.at(captured);
                        if (created.opcode != Opcode::NewObjectWithShape || program.at(created.layout()).kind != LayoutKind::Environment) {
                            markOperandEscaped(captured);
                            continue;
                        }
                        isCapturedByHeapClosure.set(captured.index());
                    }
                    break;
                default:
                    markOperandEscaped(id);
                    value.forEachChild(markOperandEscaped);
                    break;
                }
            }
            if (block.terminator.value)
                markEscaped(block.terminator.value, "Return"_s, function.at(block.terminator.value));
            for (auto& target : block.terminator.targets) {
                for (ValueID argument : target.arguments)
                    flowsThroughPhi.set(argument.index());
            }
        }
        for (auto& allocationSite : allocationSites) {
            allocationSite.isCapturedByHeapClosure = isCapturedByHeapClosure.get(allocationSite.environment.index());
            allocationSite.flowsThroughPhi = flowsThroughPhi.get(allocationSite.environment.index());
        }
    };
    if (program.recordsAllocations) {
        for (unsigned i = 0; i < program.functions.size(); ++i)
            scan(i);
    } else
        forEachIndexInParallel(program.functions.size(), scan);
    BitVector escapes(program.layouts.size());
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        if (doesEscape[i])
            escapes.quickSet(i);
    }

    Vector<uint8_t> mayBeStackAllocated(FillWith { }, program.layouts.size(), static_cast<uint8_t>(0));
    Vector<uint8_t> hasHeapAllocation(FillWith { }, program.layouts.size(), static_cast<uint8_t>(0));
    auto say = [](Vector<uint8_t>& said, unsigned index) {
        WTF::atomicStore(&said[index], static_cast<uint8_t>(1), std::memory_order_relaxed);
    };
    Vector<Vector<ValueID>> materializedIn(program.functions.size());
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        if (allocationsByFunction[i].isEmpty())
            return;
        auto& function = program.functions.atKeepingValueIndex(i);
        std::optional<BitVector> inCycles;
        for (auto& allocationSite : allocationsByFunction[i]) {
            unsigned index = function.at(allocationSite.environment).layout().index();
            auto& layout = program.layouts[index];
            if (escapes.quickGet(index) || layout.fields.size() > maxFields)
                continue;
            if (!inCycles)
                inCycles = blocksInCycles(function);
            bool allocatesOnHeap = inCycles->quickGet(allocationSite.block) && mayOutliveLoopIteration(function, allocationSite.environment, allocationSite.block, [&](ValueID operand) { return originOf(program, function, operand); }, [&](ValueID id) { return function.at(id).isStackAllocatedClosure(); });
            if (allocationSite.isCapturedByHeapClosure && !allocatesOnHeap) {
                if (!allocationSite.flowsThroughPhi && EnvironmentMaterialization(program, function, allocationSite.environment).canRun())
                    materializedIn[i].append(allocationSite.environment);
                else
                    allocatesOnHeap = true;
                say(hasHeapAllocation, index);
            }
            if (allocatesOnHeap) {
                say(hasHeapAllocation, index);
                continue;
            }
            say(mayBeStackAllocated, index);
            function.stackAllocatedEnvironments.set(allocationSite.environment.index());
        }
    });
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        auto& layout = program.layouts[i];
        layout.mayBeStackAllocated = mayBeStackAllocated[i];
        layout.isStackAllocated = layout.mayBeStackAllocated && !hasHeapAllocation[i];
    }
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        for (ValueID environment : materializedIn[i])
            EnvironmentMaterialization(program, program.functions[i], environment).run();
    });
    Vector<Vector<String>> findingsIn(program.functions.size());
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = program.functions.atKeepingValueIndex(i);
        if (!materializedIn[i].isEmpty())
            verify(program, FunctionID(i), function, findingsIn[i]);
        if (function.stackAllocatedEnvironments.isEmpty())
            return;
        auto& mayPointIntoFrame = function.mayPointIntoFrame;
        mayPointIntoFrame.merge(function.stackAllocatedEnvironments);
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& block : function.blocks) {
                for (ValueID id : block.values) {
                    if (function.at(id).opcode == Opcode::MaterializeEnvironment && mayPointIntoFrame.get(function.at(id).child(0).index()) && !mayPointIntoFrame.set(id.index()))
                        changed = true;
                }
                for (auto& target : block.terminator.targets) {
                    auto& parameters = function.at(target.block).parameters;
                    for (unsigned k = 0; k < target.arguments.size(); ++k) {
                        if (mayPointIntoFrame.get(target.arguments[k].index()) && !mayPointIntoFrame.set(parameters[k].index()))
                            changed = true;
                    }
                }
            }
        }
    });
    for (auto& perFunction : findingsIn)
        findings.appendVector(WTF::move(perFunction));
}

void reuseEnvironments(Program& program)
{
    Vector<uint8_t> isReused(FillWith { }, program.layouts.size(), static_cast<uint8_t>(0));
    bool hasCandidates = false;
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        auto& layout = program.layouts[i];
        if (!layout.reuseVariable)
            continue;
        hasCandidates = true;
        if (!layout.isStackAllocated && !layout.mayBeStackAllocated && !Options::auditAOTTypedAssertions())
            isReused[i] = 1;
    }
    if (!hasCandidates)
        return;

    BitVector isCandidate(program.layouts.size());
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        if (program.layouts[i].reuseVariable)
            isCandidate.quickSet(i);
    }
    BitVector hasCandidateForLayout(program.types.size());
    for (unsigned i = 0; i < program.types.size(); ++i) {
        for (LayoutID supertype : program.at(TypeID(i)).objectLayouts) {
            if (isCandidate.quickGet(supertype.index()))
                hasCandidateForLayout.quickSet(i);
        }
    }
    auto forEachCandidate = [&](const Function& function, ValueID id, const auto& functor) {
        TypeID type = function.at(id).type;
        RELEASE_ASSERT(type);
        if (!hasCandidateForLayout.quickGet(type.index()))
            return;
        for (LayoutID supertype : program.at(type).objectLayouts) {
            if (program.at(supertype).reuseVariable)
                functor(supertype);
        }
    };
    Vector<uint8_t> containsCandidates(FillWith { }, program.functions.size(), static_cast<uint8_t>(0));
    struct Escape {
        unsigned block;
        ValueID before;
        ValueID environment;
        LayoutID layout;
    };
    Vector<Vector<Escape>> escapesByFunction(program.functions.size());
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        if (!program.hasCode(FunctionID(i)))
            return;
        auto& function = std::as_const(program).functions[i];
        containsCandidates[i] = function.values.containsIf([&](const Value& value) {
            if (!value.type || hasCandidateForLayout.quickGet(value.type.index()))
                return true;
            return (value.opcode == Opcode::PutField || value.opcode == Opcode::NewObjectWithShape) && isCandidate.get(value.layout().index());
        });
        if (!containsCandidates[i])
            return;
        auto giveUp = [&](ValueID id) {
            forEachCandidate(function, id, [&](LayoutID layout) {
                WTF::atomicStore(&isReused[layout.index()], static_cast<uint8_t>(0), std::memory_order_relaxed);
            });
        };
        auto disqualifyUnlessTypedAs = [&](ValueID id, TypeID holder) {
            forEachCandidate(function, id, [&](LayoutID layout) {
                if (!holder || !program.at(holder).objectLayouts.contains(layout))
                    WTF::atomicStore(&isReused[layout.index()], static_cast<uint8_t>(0), std::memory_order_relaxed);
            });
        };
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            auto& block = function.blocks[b];
            for (ValueID id : block.values) {
                auto& value = function.at(id);
                auto escapes = [&](ValueID operand) {
                    LayoutID only = program.at(function.at(operand).type).singleLayout();
                    forEachCandidate(function, operand, [&](LayoutID layout) {
                        if (layout == only)
                            escapesByFunction[i].append({ b, id, operand, layout });
                        else
                            WTF::atomicStore(&isReused[layout.index()], static_cast<uint8_t>(0), std::memory_order_relaxed);
                    });
                };
                switch (value.opcode) {
                case Opcode::Parameter:
                case Opcode::GetCapture:
                case Opcode::GetField:
                    break;
                case Opcode::NewObjectWithShape:
                    value.forEachChild(giveUp);
                    break;
                case Opcode::Cast:
                    if (!isTransparentCast(program, value)) {
                        giveUp(value.child(0));
                        giveUp(id);
                    } else
                        disqualifyUnlessTypedAs(value.child(0), value.type);
                    break;
                case Opcode::PutField:
                    escapes(value.child(1));
                    break;
                case Opcode::CallDirect: {
                    giveUp(id);
                    auto& callee = std::as_const(program).at(value.function());
                    if (callee.has(FunctionUsesArgumentBuffer) || callee.isGenericEntrypoint()) {
                        value.forEachChild(giveUp);
                        break;
                    }
                    unsigned at = 0;
                    auto pass = [&](TypeID holder) {
                        if (at < value.arguments.size())
                            disqualifyUnlessTypedAs(value.arguments[at], holder);
                        ++at;
                    };
                    for (TypeID capture : callee.captureTypes)
                        pass(capture);
                    if (callee.has(Format::FunctionHasThis))
                        pass(callee.thisType);
                    for (TypeID parameter : callee.parameterTypes)
                        pass(parameter);
                    for (; at < value.arguments.size(); ++at)
                        giveUp(value.arguments[at]);
                    break;
                }
                case Opcode::NewClosure: {
                    if (!value.isStackAllocatedClosure()) {
                        value.forEachChild(escapes);
                        break;
                    }
                    auto& captureTypes = std::as_const(program).at(value.function()).captureTypes;
                    for (unsigned k = 0; k < value.arguments.size(); ++k)
                        disqualifyUnlessTypedAs(value.arguments[k], k < captureTypes.size() ? captureTypes[k] : TypeID());
                    break;
                }
                case Opcode::NewClosureOnce:
                    value.forEachChild(escapes);
                    break;
                default:
                    value.forEachChild(giveUp);
                    giveUp(id);
                    break;
                }
            }
            if (block.terminator.kind != TerminatorKind::Jump && block.terminator.kind != TerminatorKind::Branch && block.terminator.value)
                giveUp(block.terminator.value);
            for (auto& target : block.terminator.targets) {
                auto& parameters = function.at(target.block).parameters;
                for (unsigned k = 0; k < target.arguments.size(); ++k)
                    disqualifyUnlessTypedAs(target.arguments[k], k < parameters.size() ? function.at(parameters[k]).type : TypeID());
            }
        }
    });

    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        if (!containsCandidates[i])
            return;
        auto& function = program.functions.atKeepingValueIndex(i);
        auto isRelease = [&](ValueID id) {
            auto& value = function.at(id);
            return value.opcode == Opcode::PutField && program.at(value.layout()).reuseVariable && value.field() + 1 == program.at(value.layout()).fields.size();
        };
        struct AliasInputs {
            Vector<std::pair<uint32_t, uint32_t>> valuesByLayout;
            Vector<ValueID> castsAndStackClosures;
            Vector<std::pair<ValueID, ValueID>> argumentsAndParameters;
            Vector<unsigned> valueBlock;
        };
        std::optional<AliasInputs> gathered;
        auto mayBeAliasByType = [&](const Value& value) {
            bool isBlockParameter = value.opcode == Opcode::Parameter && value.immediates[0];
            return value.opcode != Opcode::NewObjectWithShape && value.opcode != Opcode::Cast && !isBlockParameter;
        };
        auto aliasInputs = [&]() -> const AliasInputs& {
            if (gathered)
                return *gathered;
            gathered.emplace();
            for (unsigned v = 0; v < function.values.size(); ++v) {
                auto& value = function.values[v];
                if (!mayBeAliasByType(value))
                    continue;
                RELEASE_ASSERT(value.type);
                if (!hasCandidateForLayout.quickGet(value.type.index()))
                    continue;
                for (LayoutID supertype : program.at(value.type).objectLayouts) {
                    if (isCandidate.quickGet(supertype.index()))
                        gathered->valuesByLayout.append({ supertype.index(), v });
                }
            }
            std::ranges::sort(gathered->valuesByLayout);
            gathered->valueBlock.fill(UINT_MAX, function.values.size());
            for (unsigned b = 0; b < function.blocks.size(); ++b) {
                for (ValueID id : function.blocks[b].values) {
                    gathered->valueBlock[id.index()] = b;
                    if (auto& value = function.at(id); value.opcode == Opcode::Cast || value.isStackAllocatedClosure())
                        gathered->castsAndStackClosures.append(id);
                }
                for (auto& target : function.blocks[b].terminator.targets) {
                    auto& parameters = function.at(target.block).parameters;
                    for (unsigned k = 0; k < target.arguments.size() && k < parameters.size(); ++k)
                        gathered->argumentsAndParameters.append({ target.arguments[k], parameters[k] });
                }
            }
            return *gathered;
        };
        auto isUsedAfterIndex = [&](unsigned releaseBlock, size_t releaseIndex, ValueID environment) {
            if (function.at(environment).opcode != Opcode::NewObjectWithShape)
                return true;
            LayoutID layout = function.at(environment).layout();
            auto& inputs = aliasInputs();
            BitVector isAlias(function.values.size());
            isAlias.quickSet(environment.index());
            if (isCandidate.quickGet(layout.index())) {
                for (auto found = std::ranges::lower_bound(inputs.valuesByLayout, std::pair { layout.index(), 0u }); found != inputs.valuesByLayout.end() && found->first == layout.index(); ++found)
                    isAlias.quickSet(found->second);
            } else {
                for (unsigned v = 0; v < function.values.size(); ++v) {
                    if (mayBeAliasByType(function.values[v]) && program.at(function.values[v].type).objectLayouts.contains(layout))
                        isAlias.quickSet(v);
                }
            }
            unsigned allocationBlock = inputs.valueBlock[environment.index()];
            for (bool changed = true; changed;) {
                changed = false;
                for (ValueID id : inputs.castsAndStackClosures) {
                    if (isAlias.quickGet(id.index()))
                        continue;
                    bool holds = false;
                    function.at(id).forEachChild([&](ValueID child) {
                        holds |= isAlias.quickGet(child.index());
                    });
                    if (holds) {
                        isAlias.quickSet(id.index());
                        changed = true;
                    }
                }
                for (auto [argument, parameter] : inputs.argumentsAndParameters) {
                    if (isAlias.quickGet(argument.index()) && !isAlias.quickSet(parameter.index()))
                        changed = true;
                }
            }
            auto usesIn = [&](unsigned b, size_t from) {
                auto& block = function.blocks[b];
                bool uses = false;
                auto look = [&](ValueID child) {
                    uses |= isAlias.quickGet(child.index());
                };
                for (size_t k = from; k < block.values.size(); ++k)
                    function.at(block.values[k]).forEachChild(look);
                if (block.terminator.value)
                    look(block.terminator.value);
                for (auto& target : block.terminator.targets) {
                    for (ValueID argument : target.arguments)
                        look(argument);
                }
                return uses;
            };
            if (usesIn(releaseBlock, releaseIndex + 1))
                return true;
            BitVector seen(function.blocks.size());
            Vector<unsigned, 16> worklist;
            auto visit = [&](unsigned b) {
                for (auto& target : function.blocks[b].terminator.targets) {
                    if (target.block.index() != allocationBlock && !seen.quickSet(target.block.index()))
                        worklist.append(target.block.index());
                }
            };
            visit(releaseBlock);
            while (!worklist.isEmpty()) {
                unsigned b = worklist.takeLast();
                if (usesIn(b, 0))
                    return true;
                visit(b);
            }
            return false;
        };
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            auto& values = function.blocks[b].values;
            for (size_t k = 0; k < values.size(); ++k) {
                if (isRelease(values[k]) && isReused[function.at(values[k]).layout().index()] && isUsedAfterIndex(b, k, function.at(values[k]).child(0))) {
                    program.functions.invalidateValueIndex(i);
                    values.removeAt(k--);
                }
            }
        }
    });

    Vector<uint8_t> isProfitableLayout(FillWith { }, program.layouts.size(), static_cast<uint8_t>(0));
    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        if (!containsCandidates[i])
            return;
        auto& function = std::as_const(program).functions[i];
        auto isKnownProfitable = [&](LayoutID layout) {
            return WTF::atomicLoad(&isProfitableLayout[layout.index()], std::memory_order_relaxed);
        };
        for (unsigned first = 0; first < function.blocks.size(); ++first) {
            for (ValueID allocationSite : function.blocks[first].values) {
                if (function.at(allocationSite).opcode != Opcode::NewObjectWithShape)
                    continue;
                LayoutID layout = function.at(allocationSite).layout();
                if (!isReused[layout.index()] || isKnownProfitable(layout))
                    continue;
                BitVector escapesIn(function.blocks.size());
                for (auto& escape : escapesByFunction[i]) {
                    if (escape.layout == layout)
                        escapesIn.quickSet(escape.block);
                }
                BitVector seen(function.blocks.size());
                Vector<unsigned, 16> worklist { first };
                seen.quickSet(first);
                while (!worklist.isEmpty() && !isKnownProfitable(layout)) {
                    unsigned b = worklist.takeLast();
                    if (escapesIn.quickGet(b))
                        continue;
                    for (ValueID id : function.blocks[b].values) {
                        auto& value = function.at(id);
                        if (value.opcode == Opcode::PutField && value.layout() == layout && value.field() + 1 == program.at(layout).fields.size())
                            WTF::atomicStore(&isProfitableLayout[layout.index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
                    }
                    for (auto& target : function.blocks[b].terminator.targets) {
                        if (!seen.quickSet(target.block.index()))
                            worklist.append(target.block.index());
                    }
                }
            }
        }
    });
    unsigned unreleasedCount = 0;
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        unreleasedCount += isReused[i] && !isProfitableLayout[i];
        isReused[i] &= isProfitableLayout[i];
    }

    forEachIndexInParallel(program.functions.size(), [&](unsigned i) {
        auto& function = program.functions.atKeepingValueIndex(i);
        for (auto& allocation : function.allocations) {
            auto& layout = program.at(allocation.layout);
            if (!layout.reuseVariable || isReused[allocation.layout.index()])
                continue;
            allocation.order.removeAllMatching([&](uint16_t field) {
                return field + 2u >= layout.fields.size();
            });
            allocation.initialFieldCount = static_cast<uint16_t>(allocation.order.size());
        }
        if (!containsCandidates[i])
            return;
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            bool didRemove = function.blocks[b].values.removeAllMatching([&](ValueID id) {
                auto& value = function.at(id);
                if (value.opcode != Opcode::PutField)
                    return false;
                auto& layout = program.at(value.layout());
                return layout.reuseVariable && !isReused[value.layout().index()] && value.field() + 2 >= layout.fields.size();
            });
            if (didRemove)
                program.functions.invalidateValueIndex(i);
        }
        for (auto& escape : escapesByFunction[i]) {
            if (!isReused[escape.layout.index()])
                continue;
            program.functions.invalidateValueIndex(i);
            Value constant;
            constant.opcode = Opcode::Boolean;
            constant.immediates[0] = 1;
            constant.type = program.types.boolean;
            constant.rep = repOf(program.at(constant.type));
            constant.position = function.at(escape.before).position;
            constant.inlineFrame = function.at(escape.before).inlineFrame;
            Value store;
            store.opcode = Opcode::PutField;
            store.type = program.types.undefined;
            store.rep = repOf(program.at(store.type));
            store.position = constant.position;
            store.inlineFrame = constant.inlineFrame;
            store.immediates = { escape.layout.index(), static_cast<uint32_t>(program.at(escape.layout).fields.size() - 2) };
            function.values.append(WTF::move(constant));
            store.children = { escape.environment, ValueID(function.values.size() - 1), ValueID() };
            function.values.append(WTF::move(store));
            auto& values = function.blocks[escape.block].values;
            size_t at = values.find(escape.before);
            RELEASE_ASSERT(at != notFound);
            values.insert(at, ValueID(function.values.size() - 1));
            values.insert(at, ValueID(function.values.size() - 2));
        }
    });
    unsigned flaggedCount = 0;
    unsigned reusedCount = 0;
    unsigned stackAllocatedCount = 0;
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        auto& layout = program.layouts[i];
        if (!layout.reuseVariable)
            continue;
        ++flaggedCount;
        reusedCount += isReused[i];
        stackAllocatedCount += layout.isStackAllocated || layout.mayBeStackAllocated;
        if (isReused[i])
            continue;
        layout.fields.shrink(layout.fields.size() - 2);
        layout.reuseVariable = VariableID();
    }
    dataLogLnIf(Options::verboseAOTCompilation(), "AOT: of ", flaggedCount, " environments that may be used again ", reusedCount, " are; ", stackAllocatedCount, " lie in frames, ", unreleasedCount, " have no release that is reached without an escape, ", flaggedCount - reusedCount - stackAllocatedCount - unreleasedCount, " are given up");
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
