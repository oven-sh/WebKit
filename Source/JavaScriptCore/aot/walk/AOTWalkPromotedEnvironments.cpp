/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */


#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTIRProgram.h"
#include "AOTPlan.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

Reference Walker::unspecifiedConstant(TypeID type)
{
    for (const auto& [known, word] : m_unspecifiedConstants) {
        if (known == type)
            return word;
    }
    Origin origin = m_origin;
    origin.isTailCall = false;
    Draft word(Opcode::Unspecified, origin);
    word.setValueType(type);
    Reference built = constant(WTF::move(word));
    if (built)
        m_unspecifiedConstants.append({ type, built });
    return built;
}

Reference wordInPlaceOfRemovedCapture(Walker& walker, FunctionID function, uint32_t capture)
{
    return walker.unspecifiedConstant(walker.program().provenTypeOf(walker.program().at(function).captures[capture]));
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
    bool restsOnLoaderAlone = walker.program().at(function).isModuleBody() && !isBodyWrittenInAnotherFile(walker.program(), function);
    if (!walker.plan().promotedEnvironments().isMaker(function) || restsOnLoaderAlone || walker.facts().isTakenToRunOnce(function) || walker.isUnreachable())
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

    LayoutID layout = value.as<Opcode::NewObject>().layout();
    if (!promoted.isPromoted(layout))
        return false;
    Draft written = walker.draftOf(value);
    Reference environment = walker.unspecifiedConstant(walker.types().forEnvironment(layout));
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

void Walker::storeValueNoLongerCaptured(const StoreOfCapturedValue& store)
{
    Reference stored = referenceBoundTo(store.value);
    if (!stored || isUnreachable() || m_state.isPastDeadZone(store.variable))
        return;
    const PromotedVariable& variable = *plan().promotedEnvironments().find(store.variable);
    Origin origin = m_origin;
    origin.isTailCall = false;
    TypeID type = typeOf(stored);
    if (!type || !m_types.isSubset(type, program().provenTypeOf(program().at(variable.function).captures[variable.capture]))) {
        fail("a value that functions no longer capture is not proven to have the type of the capture"_s);
        return;
    }
    Draft put(Opcode::PutModuleVariable, origin);
    put.setVariable(store.variable);
    put.setValueOperand(stored);
    chooseConservativeFields(put);
    build(WTF::move(put));
    m_state.noteEndOfDeadZone(store.variable);
}

void Walker::storeParametersNoLongerCaptured()
{
    m_storesOfCapturedValues = plan().promotedEnvironments().storesIn(unit().function);
    for (const StoreOfCapturedValue& store : m_storesOfCapturedValues) {
        if (currentBodyIndex().isParameter(store.value))
            storeValueNoLongerCaptured(store);
    }
}

void Walker::storeValueBehindItsDefinitionInMaker(ValueID definition)
{
    auto found = std::ranges::lower_bound(m_storesOfCapturedValues, definition, { }, &StoreOfCapturedValue::value);
    if (found != m_storesOfCapturedValues.end() && found->value == definition)
        storeValueNoLongerCaptured(*found);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
