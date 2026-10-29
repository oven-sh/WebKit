/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTThreads.h"
#include "AOTValueFlowRules.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

// The values of such a function lie with its instances, and one of them may come back where another does not.
bool isFollowedOnceForEachKey(const ValueFlowResultInputs& inputs, FunctionID id)
{
    const ValueFlowInstancedFunctions* instanced = inputs.solver().input().instancedFunctions;
    return instanced && instanced->find(id);
}

// An operation whose operands all hold values holds a value too, or there is a reason why it never completes: it always throws, or what it calls never comes back.
// The code behind an operation without a value is taken to be unreachable.
class OperationsWithoutValue {
public:
    OperationsWithoutValue(const ValueFlowResultInputs& inputs, FunctionID id, ValueSetID nullish)
        : m_inputs(inputs)
        , m_id(id)
        , m_function(inputs.program.at(id))
        , m_nullish(nullish)
    {
    }

    void appendTo(Vector<InternalError>& found)
    {
        Vector<const ValueFlowStatement*, 4> suspects;
        for (const ValueFlowStatement& statement : statements()) {
            if (isOperationWithoutValue(statement) && mayHaveToHoldValue(statement) && !hasOperandWithoutValue(statement) && !standsBehindTestThatNothingPasses(statement) && mustHaveValue(statement))
                suspects.append(&statement);
        }
        if (suspects.isEmpty())
            return;
        BitVector isReached = findOperationsReached();
        for (const ValueFlowStatement* statement : suspects) {
            if (isReached.get(statement->site.index()))
                found.append(InternalError { .kind = InternalErrorKind::OperationWithoutValue, .function = m_id, .value = statement->site, .location = m_function.locationOf(statement->site) });
        }
    }

private:
    std::span<const ValueFlowStatement> statements() const { return m_inputs.bodies().statementsOf(m_id.index()); }
    std::span<const ValueFlowOperand> operandsOf(const ValueFlowStatement& statement) const { return m_inputs.bodies().operandsOf(statement); }
    // What the rule of the operation was shown. Behind a test, that is less than the results say: they let a value that nobody knows through every test.
    ValueSetID valueOf(const ValueFlowOperand& operand)
    {
        if (!m_maker)
            m_maker.emplace(m_inputs.makerPool());
        return ValueFlowRuleContext::valueShownToRules(operand, m_inputs.solver(), m_maker->get());
    }
    bool holdsNothing(ValueFlowNodeID node) const { return m_inputs.solver().possibleValuesOf(node) == ValueSets::nothing; }
    static bool isGiven(const ValueFlowOperand& operand) { return operand.node || (operand.constant && operand.constant != ValueSets::nothing); }

    bool isOperationWithoutValue(const ValueFlowStatement& statement) const
    {
        return statement.kind == ValueFlowStatementKind::Operation && statement.result && holdsNothing(statement.result);
    }

    bool holdsNothing(const ValueFlowOperand& operand) { return valueOf(operand) == ValueSets::nothing; }

    bool hasOperandWithoutValue(const ValueFlowStatement& statement)
    {
        return std::ranges::any_of(operandsOf(statement), [&](const ValueFlowOperand& operand) {
            return isGiven(operand) && holdsNothing(operand);
        });
    }

    bool standsBehindTestThatNothingPasses(const ValueFlowStatement& statement)
    {
        return std::ranges::any_of(m_inputs.bodies().guardsOf(statement), [&](const ValueFlowOperand& guard) {
            return holdsNothing(guard);
        });
    }

    bool comesBack(FunctionID id, bool isConstruction) const
    {
        const Function& callee = m_inputs.program.at(id);
        if (throwsInsteadOfRunning(callee, isConstruction) || isFollowedOnceForEachKey(m_inputs, id))
            return false;
        if (callee.isUnsplitCoroutine())
            return true;
        if (callee.results.isEmpty())
            return false;
        const ValueFlowProgramNodes& nodes = m_inputs.bodies().nodes();
        const ValueFlowProgramNodes::ResultSummary* summary = isConstruction ? nullptr : nodes.resultSummaryOf(id);
        // A constructor below another class has no object of its own: its construction answers the object that super() made, or never completes.
        if (isConstruction && callee.kind == FunctionKind::DerivedConstructor)
            return m_inputs.sets().meet(m_inputs.solver().possibleValuesOf(nodes.result(id, 0)), m_inputs.universe().objects());
        return !holdsNothing(summary ? summary->ownResult : nodes.result(id, 0));
    }

    bool callsWhatComesBack(const ValueFlowStatement& statement)
    {
        const ValueFlowUniverse& universe = m_inputs.universe();
        ValueSetID callees = valueOf(operandsOf(statement)[0]);
        if (m_inputs.sets().meet(callees, universe.placeholdersAlone()))
            return true;
        bool isConstruction = !!OperandPosition::newTarget(statement.opcode);
        bool answer = false;
        m_inputs.sets().forEachMember(callees, [&](ValueFlowObjectID callee) {
            if (FunctionID function = universe.closureFunction(callee))
                answer |= comesBack(function, isConstruction);
        });
        return answer;
    }

    bool asksForCodeToRun(ValueFlowStatementID id, unsigned levels) const
    {
        bool answer = false;
        m_inputs.solver().forEachMadeNodeAskedForBy(id, [&](const ValueFlowNodeKey& key, ValueFlowStatementID made) {
            if (answer)
                return;
            answer = key.kind == ValueFlowMadeNodeKind::CallGroup || key.kind == ValueFlowMadeNodeKind::Hub || (made && levels && asksForCodeToRun(made, levels - 1));
        });
        return answer;
    }

    // A read by name finds what was stored under the name on its receiver, unless code of the program runs first: an accessor, a trap.
    // Where nothing was stored under a name that every such object is born with, no such object was ever finished.
    bool readsWhatWasStored(const ValueFlowStatement& statement)
    {
        AOT_THRESHOLD(unsigned, levelsOfLookups, 3);
        if (statement.opcode != Opcode::PropGet)
            return false;
        ValueSetID receivers = valueOf(operandsOf(statement)[0]);
        if (m_inputs.sets().isSubset(receivers, m_nullish))
            return false;
        bool isStored = false;
        ValueFlowHeapView(m_inputs).forEachNodeWrittenUnder(ValueFlowName::property(m_function.at(statement.site).key()), [&](ValueSetID objects, ValueSetID value) {
            isStored |= value != ValueSets::nothing && m_inputs.sets().meet(objects, receivers);
        });
        if (!isStored)
            return false;
        return !asksForCodeToRun(ValueFlowStatementID(static_cast<uint32_t>(&statement - m_inputs.bodies().statements().data())), levelsOfLookups);
    }

    bool mayHaveToHoldValue(const ValueFlowStatement& statement) const
    {
        // Nothing may fit the type that the result is held to.
        if (m_inputs.solver().graph().maskOf(statement.result) != ValueSets::anything)
            return false;
        switch (flowRuleOf(statement.opcode)) {
        case FlowRule::Call:
        case FlowRule::Heap:
        case FlowRule::ConversionToObject:
        case FlowRule::General:
        case FlowRule::Convert:
        case FlowRule::NewDictionary:
            return true;
        // These refuse some values, run a protocol of the language on their operands or wait for something.
        case FlowRule::Arithmetic:
        case FlowRule::Await:
        case FlowRule::DefineMethod:
        case FlowRule::Generator:
        case FlowRule::InstanceOf:
        case FlowRule::Iteration:
        case FlowRule::Module:
        case FlowRule::NewClass:
        case FlowRule::Promise:
        case FlowRule::ReadOfPrototypeProperty:
        case FlowRule::StepOfLibrary:
        case FlowRule::ToPropertyKey:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool mustHaveValue(const ValueFlowStatement& statement)
    {
        FlowRule rule = flowRuleOf(statement.opcode);
        if (rule == FlowRule::Call)
            return callsWhatComesBack(statement);
        if (rule == FlowRule::Heap)
            return readsWhatWasStored(statement);
        return rule != FlowRule::ConversionToObject || !m_inputs.sets().isSubset(valueOf(operandsOf(statement)[0]), m_nullish);
    }

    // Control does not get past an operation without a value, or past the definition of an operand without one.
    BitVector findOperationsReached()
    {
        BitVector endsItsBlock(m_function.values.size());
        for (const ValueFlowStatement& statement : statements()) {
            if (statement.kind != ValueFlowStatementKind::Operation)
                continue;
            if (isOperationWithoutValue(statement))
                endsItsBlock.quickSet(statement.site.index());
            const Value& operation = m_function.at(statement.site);
            std::span<const ValueFlowOperand> operands = operandsOf(statement);
            for (uint32_t index = 0; index < operands.size(); ++index) {
                if (!isGiven(operands[index]) || !holdsNothing(operands[index]))
                    continue;
                endsItsBlock.quickSet(statement.site.index());
                if (operands.size() == operation.numberOfOperands() && operation.operand(index))
                    endsItsBlock.quickSet(operation.operand(index).index());
            }
        }

        BitVector isReached(m_function.values.size());
        BitVector isListed(m_function.blocks.size());
        Vector<BlockID, 16> blocks { entryBlock };
        isListed.quickSet(entryBlock.index());
        auto list = [&](const Target& target) {
            if (!isListed.quickSet(target.block.index()))
                blocks.append(target.block);
        };
        while (!blocks.isEmpty()) {
            const Block& block = m_function.at(blocks.takeLast());
            bool completes = std::ranges::none_of(block.parameters, [&](ValueID parameter) {
                return endsItsBlock.quickGet(parameter.index());
            });
            if (!completes)
                continue;
            for (ValueID value : block.values) {
                isReached.quickSet(value.index());
                if (endsItsBlock.quickGet(value.index())) {
                    completes = false;
                    break;
                }
            }
            if (block.handler)
                list(*block.handler);
            if (!completes)
                continue;
            for (const Target& target : block.terminator.targets)
                list(target);
        }
        return isReached;
    }

    const ValueFlowResultInputs& m_inputs;
    FunctionID m_id;
    const Function& m_function;
    ValueSetID m_nullish;
    std::optional<BorrowedValueSetMaker> m_maker;
};

} // anonymous namespace

void reportOperationsWithoutValue(const ValueFlowResultInputs& inputs)
{
    ValueSetID nullish;
    {
        BorrowedValueSetMaker maker(inputs.makerPool());
        nullish = maker.get().unite(maker.get().of(ValueFlowUniverse::primitive(PrimitiveSet::Undefined)), maker.get().of(ValueFlowUniverse::primitive(PrimitiveSet::Null)));
    }
    Vector<InternalError> found = collectInParallel<InternalError>(inputs.loops(), inputs.program.functions.size(), [&](unsigned index, Vector<InternalError>& foundInFunction) {
        FunctionID id(index);
        if (!inputs.program.at(id).hasBody() || !inputs.isReached(id) || isFollowedOnceForEachKey(inputs, id))
            return;
        OperationsWithoutValue(inputs, id, nullish).appendTo(foundInFunction);
    });
    for (InternalError& error : found)
        inputs.build.diagnostics().add(WTF::move(error));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
