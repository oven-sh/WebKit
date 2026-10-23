/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMarkResultsUsedAsKeys.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTIRProgram.h"
#include "AOTSort.h"
#include "AOTStringBuilderAnalysis.h"
#include "AOTThreads.h"
#include "AOTUseLists.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// What a function returns: strings that it makes, and results of functions that it calls.
struct ReturnedStrings {
    FunctionID function;
    Vector<ValueID, 2> made;
    Vector<FunctionID, 2> forwarded;
};

struct ResultUse {
    FunctionID callee;
    FunctionID caller;
    bool isKey { false };
    bool isPartOfKey { false };
    bool isReturned { false };
};

struct UseCounts {
    unsigned keys { 0 };
    unsigned partsOfKeys { 0 };
    unsigned others { 0 };
};

class ReturnedStringFinder {
public:
    ReturnedStringFinder(const Program& program, FunctionID id)
        : m_program(program)
        , m_function(program.at(id))
    {
        m_result.function = id;
    }

    ReturnedStrings run();

private:
    void add(ValueID);
    void addValuesPassedTo(ValueID parameter);

    const Program& m_program;
    const Function& m_function;
    ReturnedStrings m_result;
    BitVector m_isVisited;
    Vector<ValueID, 8> m_worklist;
};

void ReturnedStringFinder::add(ValueID id)
{
    if (id && !m_isVisited.set(id.index()))
        m_worklist.append(id);
}

void ReturnedStringFinder::addValuesPassedTo(ValueID parameter)
{
    auto addFrom = [&](const Target& target) {
        const auto& parameters = m_function.at(target.block).parameters;
        for (uint32_t index = 0; index < target.arguments.size(); ++index) {
            if (parameters[index] == parameter)
                add(target.arguments[index]);
        }
    };
    for (const Block& block : m_function.blocks) {
        for (const Target& target : block.terminator.targets)
            addFrom(target);
        if (block.handler)
            addFrom(*block.handler);
    }
}

ReturnedStrings ReturnedStringFinder::run()
{
    for (const Block& block : m_function.blocks) {
        if (block.terminator.kind == TerminatorKind::Return)
            add(block.terminator.valueOperand());
        else if (block.terminator.kind == TerminatorKind::ReturnCall)
            add(block.terminator.callOperand());
    }
    while (!m_worklist.isEmpty()) {
        ValueID id = m_worklist.takeLast();
        const Value& value = m_function.at(id);
        if (ValueID definition = sameObjectThroughCasts(m_function, id); definition != id)
            add(definition);
        else if (value.opcode == Opcode::Parameter)
            addValuesPassedTo(id);
        else if (value.opcode == Opcode::CallDirect)
            m_result.forwarded.append(value.function());
        else if (canBeMarkedAsLikelyKey(m_program, m_function, value))
            m_result.made.append(id);
    }
    return WTF::move(m_result);
}

} // anonymous namespace

static bool mayReturnOneString(const Program& program, const Function& function)
{
    return function.hasBody() && function.results.size() == 1 && program.at(function.results[0].type).primitives & Primitive::AnyString;
}

static ResultUse findUseOfResult(const Program& program, const Function& caller, const UseLists& useLists, ValueID call)
{
    struct Text {
        ValueID value;
        bool isPart;
    };
    ResultUse result;
    BitVector isVisited;
    Vector<Text, 8> worklist { Text { call, false } };
    isVisited.set(call.index());
    while (!worklist.isEmpty()) {
        Text text = worklist.takeLast();
        for (const Use& use : useLists.usesOf(text.value)) {
            Text next { { }, text.isPart };
            if (isKeyOperand(program, caller, use))
                (text.isPart ? result.isPartOfKey : result.isKey) = true;
            else if (use.kind == Use::Kind::TerminatorOperand) {
                TerminatorKind kind = caller.at(use.block).terminator.kind;
                result.isReturned |= !text.isPart && (kind == TerminatorKind::Return || kind == TerminatorKind::ReturnCall);
            } else if (use.kind == Use::Kind::EdgeArgument)
                next.value = caller.at(targetOf(caller, use.edge()).block).parameters[use.operandIndex];
            else if (use.kind == Use::Kind::Operand && isCast(caller.at(use.user).opcode))
                next.value = use.user;
            else if (use.kind == Use::Kind::Operand && isStringConcatenation(program, caller, caller.at(use.user)))
                next = { use.user, true };
            if (next.value && !isVisited.set(next.value.index()))
                worklist.append(next);
        }
    }
    return result;
}

static void collectUsesOfResults(const Program& program, FunctionID id, const BitVector& isCandidate, Vector<ResultUse>& uses)
{
    Vector<ValueID, 4> calls;
    forEachDirectCall(program, id, [&](BlockID, ValueID value, FunctionID callee) {
        if (isCandidate.get(callee.index()))
            calls.append(value);
    });
    if (calls.isEmpty())
        return;
    const Function& caller = program.at(id);

    CFG cfg(caller);
    UseLists useLists(caller, cfg);
    for (ValueID call : calls) {
        if (!cfg.isReachable(useLists.blockOf(call)) || useLists.usesOf(call).empty())
            continue;
        ResultUse use = findUseOfResult(program, caller, useLists, call);
        use.callee = caller.at(call).function();
        use.caller = id;
        uses.append(use);
    }
}

void markResultsUsedAsKeys(ProgramEditor& editor, BuildContext& build)
{
    const Program& program = editor.program();
    unsigned numberOfFunctions = program.functions.size();
    Vector<ReturnedStrings> returned = collectInParallel<ReturnedStrings>(build.parallelLoops(), numberOfFunctions, [&](unsigned index, Vector<ReturnedStrings>& found) {
        if (!mayReturnOneString(program, program.functions[index]))
            return;
        ReturnedStrings strings = ReturnedStringFinder(program, FunctionID(index)).run();
        if (!strings.made.isEmpty() || !strings.forwarded.isEmpty())
            found.append(WTF::move(strings));
    });

    // A function is of interest where it makes a string, or hands on the result of one that is.
    BitVector isCandidate(numberOfFunctions);
    for (const ReturnedStrings& strings : returned) {
        if (!strings.made.isEmpty())
            isCandidate.quickSet(strings.function.index());
    }
    if (isCandidate.isEmpty())
        return;
    for (bool didChange = true; std::exchange(didChange, false);) {
        for (const ReturnedStrings& strings : returned) {
            if (isCandidate.quickGet(strings.function.index()))
                continue;
            if (std::ranges::any_of(strings.forwarded, [&](FunctionID callee) { return isCandidate.quickGet(callee.index()); })) {
                isCandidate.quickSet(strings.function.index());
                didChange = true;
            }
        }
    }

    Vector<ResultUse> uses = collectInParallel<ResultUse>(build.parallelLoops(), numberOfFunctions, [&](unsigned index, Vector<ResultUse>& found) {
        collectUsesOfResults(program, FunctionID(index), isCandidate, found);
    });
    AOT::stableSort(uses, { }, [](const ResultUse& use) {
        return use.callee.index();
    });

    Vector<uint32_t> firstUseOf;
    firstUseOf.fill(0, numberOfFunctions + 1);
    for (const ResultUse& use : uses)
        ++firstUseOf[use.callee.index() + 1];
    for (unsigned index = 0; index < numberOfFunctions; ++index)
        firstUseOf[index + 1] += firstUseOf[index];
    auto usesOf = [&](FunctionID callee) {
        return uses.span().subspan(firstUseOf[callee.index()], firstUseOf[callee.index() + 1] - firstUseOf[callee.index()]);
    };
    // A caller that only returns the result stands for its own callers, and for nobody where no direct call of it is known: an entrypoint for calls by value, an export.
    auto countUses = [&](FunctionID maker) {
        UseCounts counts;
        BitVector isVisited;
        Vector<FunctionID, 8> worklist { maker };
        isVisited.set(maker.index());
        while (!worklist.isEmpty()) {
            for (const ResultUse& use : usesOf(worklist.takeLast())) {
                if (use.isKey)
                    ++counts.keys;
                else if (use.isPartOfKey)
                    ++counts.partsOfKeys;
                else if (!use.isReturned)
                    ++counts.others;
                else if (!isVisited.set(use.caller.index()))
                    worklist.append(use.caller);
            }
        }
        return counts;
    };

    uint64_t numberOfMarkedStrings = 0;
    for (const ReturnedStrings& strings : returned) {
        if (strings.made.isEmpty() || !build.isEnabled(PassID::MarkResultsUsedAsKeys, strings.function))
            continue;
        UseCounts counts = countUses(strings.function);
        // A rope serves as well as a part of a key: its characters are copied and it is never flattened. What a builder makes is flat anyway, and as a key it is shared with equal texts.
        bool marksConcatenations = counts.keys > counts.partsOfKeys + counts.others;
        bool marksBuilders = counts.keys + counts.partsOfKeys > counts.others;
        for (ValueID string : strings.made) {
            bool isConcatenation = isStringConcatenation(program, program.at(strings.function), program.at(strings.function).at(string));
            if (!(isConcatenation ? marksConcatenations : marksBuilders))
                continue;
            markAsLikelyKey(editor.atForLowering(strings.function), string);
            ++numberOfMarkedStrings;
        }
    }
    build.statistics().count(StatisticID::ResultsMarkedAsKeys, numberOfMarkedStrings);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
