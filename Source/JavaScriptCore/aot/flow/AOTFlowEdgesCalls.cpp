/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowEdges.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static bool definesResult(Opcode producer, uint32_t index)
{
    switch (producer) {
    case Opcode::CallDirect:
        return true;
    case Opcode::IteratorResume:
        return index == static_cast<uint32_t>(IteratorResumeResult::Value);
    case Opcode::Yield:
        return index == static_cast<uint32_t>(YieldResult::Sent);
    case Opcode::StepGenerator:
        return index == static_cast<uint32_t>(StepGeneratorResult::Value);
    case Opcode::StepArrayIterator:
        return index == static_cast<uint32_t>(StepArrayIteratorResult::Value);
    case Opcode::MapNext:
        return index == static_cast<uint32_t>(MapNextResult::Key) || index == static_cast<uint32_t>(MapNextResult::Value);
    case Opcode::SetNext:
        return index == static_cast<uint32_t>(SetNextResult::Element);
    default:
        return false;
    }
}

// The host's own modules reach some of its functions under private names of the global object.
static bool isPrivateGlobal(const Program& program, const Function& function, ValueID id)
{
    const Value& read = function.at(id);
    if (read.opcode != Opcode::PropGet || classOf(program, read.key()) != FlowKeyClass::PrivateName)
        return false;
    const Value& holder = function.at(read.objectOperand());
    return holder.opcode == Opcode::Builtin && holder.builtin() == Builtins::GlobalThis;
}

// The entry of such a function speaks for it, and alone: no program names the property, so the read answers the function that the host put there and nothing else.
// Of one without an entry nothing says what it does to its operands, so everything can happen to those objects, and to those alone.
FlowLocalID FlowSummaryBuilder::calleeOfPrivateGlobalCall(const Value& call)
{
    ValueID read = call.calleeOperand();
    BuiltinID entry = m_program.builtins().findOwnMember(Builtins::PrivateGlobals, builtinMemberNameOf(m_program, m_function.at(read).key()).name);
    if (entry) {
        FlowLocalID callee = m_assembler.addTemporary();
        m_assembler.seed(FlowSeed::builtin(callee, entry));
        return callee;
    }

    FlowLocalID unknownJavaScript = m_assembler.external(FlowExternalNode::unknownJavaScript());
    call.forEachOperand([&](ValueID operand, uint32_t) {
        if (operand != read && mayHoldReference(operand))
            m_assembler.copy(localOf(operand), unknownJavaScript);
    });
    return localOf(read);
}

void FlowSummaryBuilder::addCall(ValueID id, const Value& value)
{
    FlowCall call {
        .mode = FlowCallMode::Call,
        .origin = id,
        .callee = isPrivateGlobal(m_program, m_function, value.calleeOperand()) ? calleeOfPrivateGlobalCall(value) : localOf(value.calleeOperand()),
        .thisValue = localAtUse(value.thisValueOperand()),
        .result = localOf(id),
        .thrown = thrownTarget(),
    };
    if (call.thisValue)
        call.calleeKey = value.calleeKey();
    if (value.opcode == Opcode::CallList) {
        addCallStatement(id, WTF::move(call), { }, value.listOperand());
        return;
    }
    call.flags.set(FlowCallFlag::IsAnnotatedPure, value.isAnnotatedPure());
    call.expectedResult = value.expectedResult();
    addCallStatement(id, WTF::move(call), value.variadicOperands(), ValueID());
}

void FlowSummaryBuilder::addConstruct(ValueID id, const Value& value)
{
    FlowCall call {
        .mode = FlowCallMode::Construct,
        .origin = id,
        .callee = localOf(value.calleeOperand()),
        .newTarget = localOf(value.newTargetOperand()),
        .result = localOf(id),
        .thrown = thrownTarget(),
    };
    if (value.opcode == Opcode::ConstructList) {
        addCallStatement(id, WTF::move(call), { }, value.listOperand());
        return;
    }
    call.flags.set(FlowCallFlag::IsAnnotatedPure, value.isAnnotatedPure());
    addCallStatement(id, WTF::move(call), value.variadicOperands(), ValueID());
}

void FlowSummaryBuilder::addSuperConstruct(ValueID id, const Value& value)
{
    FlowCall call {
        .mode = FlowCallMode::SuperConstruct,
        .origin = id,
        .callee = localOf(value.parentOperand()),
        .newTarget = localOf(value.newTargetOperand()),
        .result = localOf(id),
        .thrown = thrownTarget(),
        .derivedLayout = value.layout(),
    };
    if (value.opcode == Opcode::SuperConstructList) {
        addCallStatement(id, WTF::move(call), { }, value.listOperand());
        return;
    }
    addCallStatement(id, WTF::move(call), value.variadicOperands(), ValueID());
}

void FlowSummaryBuilder::addCallStatement(ValueID id, FlowCall&& call, std::span<const ValueID> arguments, ValueID list)
{
    call.flags.set(FlowCallFlag::IsInCycle, isInCycle());
    call.flags.set(FlowCallFlag::IsTailCall, isTailCall(id));
    Vector<FlowLocalID, 8> argumentLocals;
    for (ValueID argument : arguments)
        argumentLocals.append(localAtUse(argument));
    call.arguments = m_assembler.addList(argumentLocals.span());
    call.argumentList = localOf(list);
    m_assembler.add(call);
}

void FlowSummaryBuilder::addCallDirect(ValueID id, const Value& value)
{
    const Function& callee = m_program.at(value.function());
    std::span<const ValueID> arguments = value.variadicOperands();
    RELEASE_ASSERT(arguments.size() == callee.numberOfEntryParameters());

    OptionSet<FlowCallFlag> flags;
    flags.set(FlowCallFlag::IsInCycle, isInCycle());
    flags.set(FlowCallFlag::IsTailCall, isTailCall(id));

    FlowLocalList argumentLocals = m_assembler.addList(localsOf(arguments).span());
    FlowLocalList resultLocals;
    uint32_t numberOfResults = callee.results.size();
    if (numberOfResults == 1) {
        FlowLocalID result = localOf(id);
        resultLocals = m_assembler.addList(singleElementSpan(result));
    } else
        resultLocals = m_assembler.addList(resultLocalsOf(id, numberOfResults).span());
    m_assembler.add(FlowDirectCall { flags, id, value.function(), argumentLocals, resultLocals, thrownTarget() });
}

void FlowSummaryBuilder::addResults(ValueID, const Value&)
{
}

void FlowSummaryBuilder::addResultAt(ValueID id, const Value& value)
{
    ValueID producerID = value.resultsOperand();
    const Value& producer = m_function.at(producerID);
    uint32_t index = value.index();

    if (producer.opcode == Opcode::Results) {
        m_assembler.copy(localOf(producer.operand(index)), localOf(id));
        return;
    }
    if (!definesResult(producer.opcode, index)) {
        seedTypeAsRead(id);
        return;
    }
    auto resultLocals = m_resultLocals.find(producerID);
    RELEASE_ASSERT(resultLocals != m_resultLocals.end());
    m_assembler.copy(resultLocals->value[index], localOf(id));
}

Vector<FlowLocalID, 4>& FlowSummaryBuilder::resultLocalsOf(ValueID producer, uint32_t numberOfResults)
{
    return m_resultLocals.ensure(producer, [&] {
        return Vector<FlowLocalID, 4>(numberOfResults, [&](size_t) {
            return m_assembler.addTemporary();
        });
    }).iterator->value;
}

void FlowSummaryBuilder::addResolveHeritage(ValueID id, const Value& value)
{
    ValueID heritage = value.heritageOperand();
    FlowLocalID result = localOf(id);

    addPrototypePropertyRead(id, heritage, result);
    m_assembler.add(FlowConditionalSeed { localOf(heritage), TypeTable::null, FlowFilterKind::Inside, id, FlowSeed::primitives(result, Primitive::Null) });
}

void FlowSummaryBuilder::addNewClass(ValueID id, const Value& value)
{
    FlowLocalID constructor = localOf(value.constructorOperand());

    m_assembler.copy(constructor, localOf(id));
    m_assembler.add(FlowClassDefinition { id, value.layout(), constructor, localOf(value.heritageOperand()), localOf(value.prototypeParentOperand()), isInCycle() });
}

void FlowSummaryBuilder::addGetClassPrototype(ValueID id, const Value& value)
{
    addPrototypePropertyRead(id, value.classObjectOperand(), localOf(id));
}

void FlowSummaryBuilder::addDefineMethod(ValueID id, const Value& value)
{
    m_assembler.add(FlowMethodDefinition {
        id,
        value.layout(),
        value.side(),
        value.methodKind(),
        value.index(),
        localOf(value.classObjectOperand()),
        localOf(value.functionOperand()),
        value.key(),
        localOf(value.computedKeyOperand()),
    });
}

void FlowSummaryBuilder::addGetConstructPrototype(ValueID id, const Value& value)
{
    FlowLocalID read = m_assembler.addTemporary();
    FlowLocalID result = localOf(id);

    addPrototypePropertyRead(id, value.newTargetOperand(), read);
    filter(read, result, TypeTable::object);
    m_assembler.add(FlowConditionalSeed { read, TypeTable::object, FlowFilterKind::Outside, id, FlowSeed::builtin(result, Builtins::ObjectPrototype) });
}

void FlowSummaryBuilder::addPrototypePropertyRead(ValueID origin, ValueID holder, FlowLocalID result)
{
    OptionSet<FlowAccessFlag> flags;
    flags.set(FlowAccessFlag::IsInCycle, isInCycle());
    m_assembler.add(FlowPropertyAccess {
        .kind = FlowAccessKind::Load,
        .flags = flags,
        .origin = origin,
        .receiver = localOf(holder),
        .key = m_program.keyOf(BuiltinNames::prototype),
        .value = result,
        .thrown = thrownTarget(),
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
