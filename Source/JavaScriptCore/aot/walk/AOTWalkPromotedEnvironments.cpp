/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// What the plan has promoted (AOTPromotedEnvironments.cpp) is decided there alone, so every unit of a function goes by it, the slow body too.
// A field of such a scope stays a GetEnvironmentField | PutEnvironmentField: Link's effects, the looks at loops and the state go on speaking of fields, and the emitter finds the word among the variables.
// In the place of the scope, and of a capture that is left out of a call, stands a word that says nothing. Whoever inspects it stops the build (UnspecifiedValueInspected).

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTIRProgram.h"
#include "AOTPlan.h"

namespace JSC { namespace AOT { namespace Typed {

Reference Walker::wordThatSaysNothing(TypeID type)
{
    for (const auto& [known, word] : m_wordsThatSayNothing) {
        if (known == type)
            return word;
    }
    Origin origin = m_origin;
    origin.isTailCall = false;
    Draft word(Opcode::Unspecified, origin);
    word.setValueType(type);
    Reference built = constant(WTF::move(word));
    if (built)
        m_wordsThatSayNothing.append({ type, built });
    return built;
}

Reference wordInPlaceOfRemovedCapture(Walker& walker, FunctionID function, uint32_t capture)
{
    return walker.wordThatSaysNothing(walker.program().provenTypeOf(walker.program().at(function).captures[capture]));
}

Reference valueNoLongerCaptured(Walker& walker, FunctionID function, uint32_t capture, Origin origin)
{
    const RemovedCapture* removed = walker.plan().promotedEnvironments().findRemoved(function, capture);
    RELEASE_ASSERT(removed);
    if (!removed->variable)
        return wordInPlaceOfRemovedCapture(walker, function, capture);
    origin.isTailCall = false;
    Draft read(Opcode::GetModuleVariable, origin);
    read.setVariable(removed->variable);
    walker.chooseConservativeFields(read);
    return walker.build(WTF::move(read));
}

void takeWhatIsNoLongerCaptured(Walker& walker)
{
    FunctionID function = walker.unit().function;
    const PromotedEnvironments& promoted = walker.plan().promotedEnvironments();
    if (!promoted.numberOfRemovedCapturesOf(function) || walker.isUnreachable())
        return;
    BodyBlock entry = walker.currentBodyIndex().block(entryBlock);
    uint32_t numberOfCaptures = static_cast<uint32_t>(walker.program().at(function).captures.size());
    for (uint32_t capture = 0; capture < numberOfCaptures; ++capture) {
        if (promoted.isRemoved(function, capture))
            walker.bind(entry.parameter(capture), valueNoLongerCaptured(walker, function, capture, walker.originOfOperation()));
    }
}

void stopAtSecondEntryOfMaker(Walker& walker)
{
    FunctionID function = walker.unit().function;
    if (!walker.plan().promotedEnvironments().isMaker(function) || walker.program().at(function).isModuleBody() || walker.facts().isTakenToRunOnce(function) || walker.isUnreachable())
        return;
    Draft enter(Opcode::EnterBodyThatRunsOnce, walker.originOfOperation());
    enter.setFunctionNumber(function.index());
    walker.build(WTF::move(enter));
}

bool leaveEnvironmentAmongVariables(Walker& walker, const BodyValue& value)
{
    const PromotedEnvironments& promoted = walker.plan().promotedEnvironments();
    if (promoted.layouts.isEmpty())
        return false;
    if (value.opcode() == Opcode::ReleaseHint) {
        Reference released = walker.referenceBoundTo(value.operand(0));
        return released && released.isConstant() && walker.code().at(released.id()).opcode == Opcode::Unspecified;
    }

    LayoutID layout = value.as<Opcode::NewObject>().layout();
    if (!promoted.isPromoted(layout))
        return false;
    Draft written = walker.draftOf(value);
    Reference environment = walker.wordThatSaysNothing(walker.types().forEnvironment(layout));
    if (!environment || walker.isUnreachable())
        return true;
    std::span<const FieldIndex> fields = written.fields();
    std::span<const Reference> values = written.valuesOperands();
    if (fields.size() != values.size()) {
        walker.fail("an allocation lists other values than fields"_s);
        return true;
    }
    for (size_t index = 0; index < fields.size() && !walker.isUnreachable(); ++index) {
        Draft store(Opcode::PutEnvironmentField, written.origin());
        store.setLayout(layout);
        store.setField(fields[index]);
        store.setEnvironmentOperand(environment);
        store.setValueOperand(values[index]);
        walker.chooseConservativeFields(store);
        walker.build(WTF::move(store));
        walker.noteEnvironmentField(environment, FieldReference { layout, fields[index] }, values[index]);
    }
    walker.bind(value.id(), environment);
    return true;
}

void storeValuesNoLongerCaptured(Walker& walker, const BodyValue& value)
{
    const PromotedEnvironments& promoted = walker.plan().promotedEnvironments();
    std::span<const StoreOfCapturedValue> stores = promoted.storesAt(walker.currentFunction(), value.id());
    if (stores.empty())
        return;
    Draft written = walker.draftOf(value);
    Origin origin = written.origin();
    origin.isTailCall = false;
    for (const StoreOfCapturedValue& store : stores) {
        if (walker.isUnreachable())
            return;
        const PromotedVariable& variable = *promoted.find(store.variable);
        Reference stored = store.capture < written.numberOfVariadicOperands() ? written.variadicOperands()[store.capture] : Reference();
        TypeID type = stored ? walker.typeOf(stored) : TypeID();
        if (!type || !walker.types().isSubset(type, walker.program().provenTypeOf(walker.program().at(variable.function).captures[variable.capture]))) {
            walker.fail("a value that functions no longer capture is not proven to have the type of the capture"_s);
            return;
        }
        // A capture is an entry parameter of its reader, and the words that check only entries check it there. Its readers take no operand any more: it is held to its type here.
        walker.checkProofOfCopy(stored, walker.positions().variable(TypeLevel::Proven, store.variable), walker.checksProofsOf(ProofSource::Capture) ? ProofSource::Capture : ProofSource::EntryParameter, origin);
        if (walker.isUnreachable())
            return;
        Draft put(Opcode::PutModuleVariable, origin);
        put.setVariable(store.variable);
        put.setValueOperand(stored);
        walker.chooseConservativeFields(put);
        walker.build(WTF::move(put));
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
