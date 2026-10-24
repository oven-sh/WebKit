/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTJSONEmission.h"
#include "AOTSelectTypedResultsNodes.h"
#include "AOTValidationRules.h"

namespace JSC { namespace AOT { namespace Typed {

struct ValidationExits {
    BlockID rejected;
    BlockID unknown;
};

class ValidatorBuilder final : public Emission {
public:
    ValidatorBuilder(IRBuilder& builder, const EmissionInputs& inputs)
        : Emission(builder, inputs)
        , m_rules(*this)
    {
    }

    ValueID emitNode(ResultNodeRef, ValueID value, const ValidationExits&);
    ValueID emitNodeBody(ResultNodeRef, ValueID value, const ValidationExits&);

    static DerivedSignature signatureOf(Emission&, const ResultNode&);

    NodeDecisionInputs decisionInputs() const { return { inputs().program, inputs().flow, inputs().build }; }
    ValidationRuleEmitter& rules() { return m_rules; }

private:
    ValidationRuleEmitter m_rules;
};

inline const Field& fieldOf(const Program& program, const ResultNode& node, const ResultMember& member)
{
    return program.at(node.layout).fields[*member.field];
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
