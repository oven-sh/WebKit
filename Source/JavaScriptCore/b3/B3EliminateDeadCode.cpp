/*
 * Copyright (C) 2015-2019 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "B3EliminateDeadCode.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlock.h"
#include "B3PhaseScope.h"
#include "B3ValueInlines.h"
#include "B3Variable.h"
#include "B3VariableValue.h"
#include <wtf/GraphNodeWorklist.h>
#include <wtf/IndexSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace B3 {

// FIXME: this pass currently only eliminates values and variables, it does not seem to eliminate dead blocks.

struct LiveValues {
    enum class State : uint8_t { NotReached, Needed, PresumedDead, Live };

    bool contains(Value* value) const { return states[value->index()] == State::Live; }

    Vector<State, 1024> states;
    Vector<BasicBlock*, 64> blocksWithPossiblyDeadValues;
};

static LiveValues markLiveValues(Procedure& proc)
{
    using State = LiveValues::State;

    LiveValues result;
    result.states.fill(State::NotReached, proc.values().size());
    State* states = result.states.mutableSpan().data();
    Vector<Value*, 64> stack;
    Vector<UpsilonValue*, 64> upsilons;
    auto markNeeded = [&](Value* value) ALWAYS_INLINE_LAMBDA {
        State& state = states[value->index()];
        if (state == State::NotReached)
            state = State::Needed;
        else if (state == State::PresumedDead) {
            state = State::Live;
            stack.append(value);
        }
    };
    for (unsigned blockIndex = proc.size(); blockIndex--;) {
        BasicBlock* block = proc[blockIndex];
        if (!block)
            continue;
        bool mayContainDeadValues = false;
        Value* const* values = block->values().span().data();
        for (unsigned index = block->size(); index--;) {
            Value* value = values[index];
            State& state = states[value->index()];
            if (state != State::Needed) {
                Opcode opcode = value->opcode();
                bool isLive = false;
                if (opcode == Upsilon) {
                    State phiState = states[static_cast<UpsilonValue*>(value)->phi()->index()];
                    isLive = phiState == State::Needed || phiState == State::Live;
                    if (!isLive)
                        upsilons.append(static_cast<UpsilonValue*>(value));
                } else if (opcode != Phi)
                    isLive = value->mustExecute();
                if (!isLive) {
                    state = State::PresumedDead;
                    mayContainDeadValues = true;
                    continue;
                }
            }
            state = State::Live;
            for (Value* child : value->children())
                markNeeded(child);
        }
        if (mayContainDeadValues)
            result.blocksWithPossiblyDeadValues.append(block);
    }
    for (;;) {
        while (!stack.isEmpty()) {
            for (Value* child : stack.takeLast()->children())
                markNeeded(child);
        }
        
        bool didPush = false;
        for (size_t upsilonIndex = 0; upsilonIndex < upsilons.size(); ++upsilonIndex) {
            UpsilonValue* upsilon = upsilons[upsilonIndex];
            State phiState = states[upsilon->phi()->index()];
            if (phiState == State::Needed || phiState == State::Live) {
                markNeeded(upsilon);
                upsilons[upsilonIndex--] = upsilons.last();
                upsilons.takeLast();
                didPush = true;
            }
        }
        if (!didPush)
            break;
    }
    return result;
}

static bool deleteDeadValues(Procedure& proc, const LiveValues& liveValues)
{
    bool changed = false;
    for (BasicBlock* block : liveValues.blocksWithPossiblyDeadValues | std::views::reverse) {
        size_t sourceIndex = 0;
        size_t targetIndex = 0;
        size_t size = block->size();
        Value** values = block->values().mutableSpan().data();
        while (sourceIndex < size) {
            Value* value = values[sourceIndex++];
            if (liveValues.contains(value))
                values[targetIndex++] = value;
            else {
                proc.deleteValue(value);
                changed = true;
            }
        }
        block->values().shrink(targetIndex);
    }
    return changed;
}

bool eliminateDeadCodeImpl(Procedure& proc)
{
    bool changed = false;
    LiveValues liveValues = markLiveValues(proc);
    if (proc.variables().isEmpty())
        return deleteDeadValues(proc, liveValues);

    IndexSet<Variable*> liveVariables;
    
    for (BasicBlock* block : proc) {
        size_t sourceIndex = 0;
        size_t targetIndex = 0;
        while (sourceIndex < block->size()) {
            Value* value = block->at(sourceIndex++);
            if (liveValues.contains(value)) {
                if (VariableValue* variableValue = value->as<VariableValue>())
                    liveVariables.add(variableValue->variable());
                block->at(targetIndex++) = value;
            } else {
                proc.deleteValue(value);
                changed = true;
            }
        }
        block->values().shrink(targetIndex);
    }

    if (liveVariables.isEmpty())
        proc.deleteAllVariables();
    else {
        for (Variable* variable : proc.variables()) {
            if (!liveVariables.contains(variable))
                proc.deleteVariable(variable);
        }
    }

    return changed;
}

bool eliminateDeadCode(Procedure& proc)
{
    PhaseScope phaseScope(proc, "eliminateDeadCode"_s);
    return eliminateDeadCodeImpl(proc);
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
