/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <span>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FlowResults;
class IRBuilder;
struct FunctionPassContext;

// Flow gives a position back where it sees no value outside the type arrive.
enum class TaintedPositionsRun : uint8_t { BeforeFlow, AfterFlow };
void guardTaintedPositions(Program&, BuildContext&, TaintedPositionsRun);
void guardTaintedPositionsAfterFlow(Program&, const FlowResults&, BuildContext&);
// The types of the fields are only expected from here on. guardTaintedPositionsAfterFlow() puts the checks at their reads.
void expectFields(Program&, std::span<const FieldReference>, BuildContext&);
// The body is typed without its checks where it stands, and is left as it was.
bool mayReturnTaintedValue(const Program&, FunctionID, Function& body, BuildContext&);
bool mayTurnValueAway(const Program&, const Function&, const Value& check);
// A type that holds any object names no layout: code that has it proven reads the object by name, as code that knows nothing does. A second version for the values that it turns away stands beside the same code.
bool changesCodeWhereProven(const Type& checked);
// What TypeScript calls an array or an instance of a class is one. An interface names members, and objects of other layouts than the literals' have them too.
bool isMetByOtherObjects(const Program&, const Type& checked);
// `let current = first; while (current) current = current.next()`: a test of such a parameter at the entrance proves the first turn alone, and a caller that it turns away loses the typed body for every turn.
class ParametersLeftToBody {
public:
    ParametersLeftToBody(const Program& program, const Function& function)
        : m_program(program)
        , m_function(function)
    {
    }

    bool contains(size_t entryParameter, const Type& checked);

private:
    void findFirstValuesOfLoopVariables();

    const Program& m_program;
    const Function& m_function;
    std::optional<BitVector> m_isFirstValueOfLoopVariable;
};

// Where a test that fails may begin the function again in its generic version: no block in front of the test has done anything that a second run would show.
FunctionID findGenericVersionToRestartIn(FunctionPassContext&);
BitVector findBlocksThatEndBeforeAnyEffect(FunctionPassContext&);
BlockID ensureCallOfGenericVersion(FunctionPassContext&, IRBuilder&, FunctionID genericVersion);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
