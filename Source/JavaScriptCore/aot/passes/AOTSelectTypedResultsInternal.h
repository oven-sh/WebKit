/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTJSONEmission.h"
#include "AOTResultRules.h"
#include "AOTSelectTypedResultsNodes.h"
#include "AOTSelectTypedResultsTargets.h"
#include <optional>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

// SelectTypedResults has nothing to do in a function without any of these.
inline constexpr OpcodeSet typedResultCallOpcodes { Opcode::Call, Opcode::CallBuiltin };

struct TypedResultSite {
    BlockID block;
    uint32_t index { 0 };
    ValueID call;
    ValueID source;
    ValueID await;
    // Of a body. A call behind a test of its receiver ends its block with a jump that hands the promise on, beside what the call by name on the other arm answers:
    // then the await stands in the block that the arms meet in, and takes the parameter of that block.
    BlockID awaitBlock;
    ValueID mergedPromise;
    TypedResultForm form { TypedResultForm::None };
    ResultNodeRef root;
    LayoutID rowLayout;
};

struct TypedResultChoice {
    std::optional<TypedResultSite> site;
    TypedResultDeclineReason declineReason { TypedResultDeclineReason::None };
};

bool mayHaveTypedResult(const Program&, const Value&);
TypedResultChoice chooseTypedResult(FunctionPassContext&, BlockID, uint32_t index);
bool canCallBodyBytesForm(const TypingContext&, BuiltinID bodyBytesForm, ValueID body);

class TypedResultExpander final : public Emission {
public:
    TypedResultExpander(IRBuilder& builder, const EmissionInputs& inputs, FunctionPassContext& pass)
        : Emission(builder, inputs)
        , m_pass(pass)
    {
    }

    void expand(const TypedResultSite&);

private:
    void expandParsedText(const TypedResultSite&);
    void expandParsedBody(const TypedResultSite&);
    void expandRows(const TypedResultSite&);
    void expandValidatedValue(const TypedResultSite&);
    void expandValidatedCall(const TypedResultSite&);

    ValueID addParameterForOriginOfPromise(const TypedResultSite&);
    ValueID emitParseOf(const TypedResultSite&, ValueID textOrBytes, BlockID generic);
    ValueID emitValidationOf(const TypedResultSite&, ValueID value);
    ValueID emitGeneralValidation(const TypedResultSite&, ValueID value);
    ValueID appendOriginalCall(const TypedResultSite&);

    FunctionPassContext& m_pass;
    Value m_originalCall;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
