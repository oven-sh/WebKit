/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// BuiltinCalls and EscapeAnalysis, a list of own keys that shares the storage of its order: `registry.names = Object.keys(config)`. The run time keeps one list of keys for the objects of a shape that got their keys in one order,
// and the code in line of the built-in hands out an array cell over it: a list that has to exist costs one cell, however many keys it holds. The state is told what the list holds, as of a literal,
// so whoever only reads it needs no cell, and the sweep drops the operation. Nothing is kept past what the state keeps. It reads words and allocates: it neither throws nor runs code. Withheld: the call stays as written.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static BuiltinOverloadID overloadThatListsOwnKeysInCopiesOwnKeyLists(const BuiltinTable& builtins)
{
    for (BuiltinOverloadID overload : builtins.overloadIDsOf(builtins.canonicalID(Builtins::ObjectKeys))) {
        if (builtins.at(overload).mode == BuiltinCallMode::Call && builtins.at(overload).code.implementation.inlineBuiltin == InlineBuiltin::OwnPropertyKeys)
            return overload;
    }
    return { };
}

// As AOTWalkEscapeAnalysis.cpp has them: Walker::constant() has one record for one integer, so a read under a written index asks under the same Reference, and the length lies under the key by which a program reads it.
static Reference positionInListInCopiesOwnKeyLists(Walker& walker, uint32_t position)
{
    Draft index(Opcode::Integer);
    index.setInteger(static_cast<int64_t>(position));
    return walker.constant(WTF::move(index));
}

static Reference keyOfLengthInCopiesOwnKeyLists(Walker& walker)
{
    Draft key(Opcode::String);
    key.setString(walker.program().stringOf(BuiltinNames::length));
    return walker.constant(WTF::move(key));
}

bool buildListOfOwnKeysOverSharedStorageOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    if (value.opcode() != Opcode::Call || !walker.has(Mechanism::BuiltinCalls) || !walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::EscapeAnalysis))
        return false;
    auto call = value.as<Opcode::Call>();
    Reference callee = call.isDoneInPlace() || call.argumentsOperands().size() != 1 ? Reference() : walker.referenceOf(call.calleeOperand());
    TypeID typeOfCallee = callee ? walker.typeOf(callee) : TypeID();
    if (!typeOfCallee)
        return false;
    const Program& program = walker.program();
    const BuiltinTable& builtins = program.builtins();
    Type named = walker.types().at(typeOfCallee);
    BuiltinID builtin = named.isObject() ? singleBuiltinOf(walker.types(), named) : BuiltinID();
    if (!builtin || builtins.canonicalID(builtin) != builtins.canonicalID(Builtins::ObjectKeys))
        return false;

    // An object that nothing else has is made only for whoever needs its cell, and a literal of its names needs none.
    Reference source = walker.referenceOf(call.argumentsOperands()[0]);
    if (!source || !source.isValue())
        return false;
    ValueID cell = rootOfCellInSweptCode(walker.code(), source.id());
    if (walker.code().isConstant(cell) || walker.state().isFresh(Reference::value(cell)))
        return false;
    // An empty literal is one cell too.
    std::optional<OwnKeyOrder> order = ownKeysInSight(walker, source);
    if (!order || order->fields.isEmpty())
        return false;
    BuiltinOverloadID overload = overloadThatListsOwnKeysInCopiesOwnKeyLists(builtins);
    if (!overload)
        return false;
    Draft written = walker.draftOf(value);
    if (walker.hasFailed())
        return true;

    uint32_t parameters[] { builtins.at(overload).code.implementation.inlineParameters[0] };
    Draft list(Opcode::CallInline, written.origin());
    list.expected = written.expected;
    list.resultIsReturnedAtOnce = written.resultIsReturnedAtOnce;
    list.inputValueGiven = value.id();
    list.setInlineBuiltin(InlineBuiltin::OwnPropertyKeys);
    list.setInlineParameters(parameters);
    list.setOverload(overload);
    list.setArgumentsOperands(singleElementSpan(source));
    // The object is proven to be one whose keys the run time lists without running code: the function reads the words of the object and the lists that the run time keeps, allocates, and does nothing else.
    Effects effects = Effects::none();
    effects.reads.add(AbstractHeapSet { AbstractHeap::AllocatorState, AbstractHeap::RuntimeState, AbstractHeap::ShapeWords, AbstractHeap::OrderWords });
    effects.writes.add(AbstractHeapSet { AbstractHeap::AllocatorState, AbstractHeap::RuntimeState });
    effects.mayCollect = true;
    Reference built = walker.build(WTF::move(list), &effects);
    if (!built)
        return true;

    WalkState& state = walker.state();
    uint32_t position = 0;
    for (FieldIndex field : order->fields) {
        Draft name(Opcode::String);
        name.setString(program.at(walker.plan().slots().keyOf(program, { order->layout, field })).text);
        state.noteElement(built, positionInListInCopiesOwnKeyLists(walker, position++), walker.constant(WTF::move(name)));
    }
    state.noteElement(built, keyOfLengthInCopiesOwnKeyLists(walker), positionInListInCopiesOwnKeyLists(walker, position));
    state.noteShape(built, walker.code().at(built.id()).type);
    state.noteFresh(built);
    walker.bind(value.id(), built);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
