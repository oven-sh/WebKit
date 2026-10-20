/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowApplication.h"
#include "AOTValueFlowAnalysis.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

// Tables of the sizes and in the orders that FlowResults and ApplyFlowResults expect, which state nothing about the program: applied, they leave it as it was read.
// Most records say "unknown" when they are left alone. In these tables the record that is missing is the statement, so they are filled:
//   unseenCode             none: only the code of the image can run, and the run time refuses any other
//   builtins.uses          none for a built-in: nothing reads, calls, writes or hands it on
//   builtins.featureUses   none for a feature: a trimmed executable drops it
//   builtins.localeUses    none: no locale data is kept
//   callsOfBoundFunctions  false for a call: every function in its stamp is entered through a closure of that function
// indirectCalls is one more, but only for a function whose callers are all known, and here no function is.
FlowResultTables createUnknownResultTables(const Program&, BuildContext&);

struct ValueFlowResultInputs {
    const Program& program;
    ValueFlowSolution& solution;
    BuildContext& build;

    const ValueFlowBodies& bodies() const { return solution.bodies(); }
    const ValueFlowUniverse& universe() const { return solution.bodies().universe(); }
    const ValueFlowSolver& solver() const { return solution.solver(); }
    const ValueSets& sets() const { return solution.sets(); }
    const ParallelLoops& loops() const { return build.parallelLoops(); }
    bool isReached(FunctionID function) const { return solution.solver().isReached(function); }
    bool hasEscaped(ValueFlowObjectID object) const { return sets().contains(solver().valueOf(ValueFlowProgramNodes::escaped()), object); }
};

// The solve has not reached the function, and its operations are compiled all the same: the host may enter it, or it is a function of the library, which a pass may link to a call of the built-in that it implements.
// What such code reads, stores, allocates and calls nobody has followed.
inline bool isUnsolvedCode(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, FunctionID function)
{
    if (inputs.isReached(function) || !inputs.program.at(function).hasBody())
        return false;
    return tables.functions.functions[function.index()].reachability != FunctionReachability::Unreached || inputs.program.at(function).role != FunctionRole::Source;
}

// An environment is no object of the language, and a closure or a namespace object has its layout without an allocation.
inline bool hasObjectsThatCodeAlters(LayoutKind kind)
{
    switch (kind) {
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
        return true;
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// What the rules have marked on the object. What is done to the unknown object is done to every object that has escaped, and code that the build has not seen does anything to it.
OptionSet<ObjectMutation> markedMutationsOf(const ValueFlowResultInputs&, ValueFlowObjectID);

enum class ValueFlowMembers : uint8_t {
    // The functor has seen every member. One that the node and the constant both hold came twice.
    Listed,
    // The operand holds all of the universe but a few members, and the functor has seen none.
    AlmostEverything,
};

ValueFlowMembers forEachMemberOf(const ValueFlowResultInputs& inputs, const ValueFlowOperand& operand, NOESCAPE const Invocable<void(ValueFlowObjectID)> auto& functor)
{
    const ValueSets& sets = inputs.sets();
    ValueSetID mask = operand.mask ? operand.mask : ValueSets::anything;
    ValueSetID held[] { operand.constant ? operand.constant : ValueSets::nothing, operand.node ? inputs.solver().valueOf(operand.node) : ValueSets::nothing };
    for (ValueSetID set : held) {
        if (ValueSets::isComplement(set) && ValueSets::isComplement(mask))
            return ValueFlowMembers::AlmostEverything;
    }
    for (ValueSetID set : held) {
        bool listsMask = ValueSets::isComplement(set);
        sets.forEachMember(listsMask ? mask : set, [&](ValueFlowObjectID member) {
            if (sets.contains(listsMask ? set : mask, member))
                functor(member);
        });
    }
    return ValueFlowMembers::Listed;
}

// What the stamps say about the functions, for the results about functions.
struct ValueFlowStampedCalls {
    // The functions that the stamp of some call lists.
    BitVector functionsInStamps;
    // Some call may enter any closure, and its stamp lists none.
    bool someCallEntersAnything { false };
    // The functions that something enters that lists them in no stamp: an accessor, a protocol, a built-in that calls an operand, a bound function. Filled with the results about functions.
    BitVector functionsEnteredWithoutStamp;
    // The functions that unsolved code calls or makes closures of. Filled with the results about functions.
    BitVector functionsNamedByUnsolvedCode;
};

// All that a store may put into a field of an object, by the layout that declares the field. Nothing for a field that the layout inherits.
struct ValueFlowFieldContents {
    Vector<Vector<ValueSetID>> stored;
};

// Callee sets.
ValueFlowStampedCalls computeCallResults(const ValueFlowResultInputs&, TypeTable&, FlowResultTables&);
// Functions that are never reached, and functions whose callers are all known.
void computeFunctionResults(const ValueFlowResultInputs&, ValueFlowStampedCalls&, FlowResultTables&);
// Functions that run at most once. Rests on the stamps and on the callers.
void computeExecutionResults(const ValueFlowResultInputs&, const ValueFlowStampedCalls&, FlowResultTables&);
// The namespace objects that are never created, and who writes, stores to and reads the variables and the fields of environments. Rests on the executions.
void computeLocationResults(const ValueFlowResultInputs&, FlowResultTables&);
// The types of variables, fields, parameters and results of functions and of what operations take and give, with the layouts that keep their shape. Rests on the writers of the locations and the callers of the functions.
void computeTypeResults(const ValueFlowResultInputs&, const BitVector& layoutsThatKeepTheirShape, const ValueFlowFieldContents&, TypeTable&, FlowResultTables&, Vector<String>* setsOutsideTypes);
// Which variables and fields of environments are stored before every read. Rests on the stamps and on the writers of the locations.
void computeOrderResults(const ValueFlowResultInputs&, const ValueFlowStampedCalls&, FlowResultTables&);
// The layouts that are never allocated, and what can never happen to an object of a layout. Answers the layouts whose objects keep their shape.
BitVector computeObjectResults(const ValueFlowResultInputs&, const ValueFlowStampedCalls&, FlowResultTables&);
// How often a class is evaluated, and which of its members and links stay as it defines them. Rests on the executions.
void computeClassResults(const ValueFlowResultInputs&, FlowResultTables&);
// Who writes the fields of objects, which are never deleted or redefined, and which every allocation fills. Rests on what never happens to the objects of a layout.
ValueFlowFieldContents computeFieldResults(const ValueFlowResultInputs&, FlowResultTables&);
// The layouts none of whose fields is ever stored a value outside its type. Rests on the types.
void computeLayoutsNeverStoredOtherTypes(const ValueFlowResultInputs&, FlowResultTables&);
// Uses of built-ins, features and locales, and the code that the build has not seen.
void computeBuiltinResults(const ValueFlowResultInputs&, FlowResultTables&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
