/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTJSONEmission.h"
#include "AOTSelectTypedResultsNodes.h"
#include "AOTValidationRules.h"
#include <span>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct ValidationExits {
    BlockID rejected;
    BlockID unknown;
};

enum class RuntimeClass : uint8_t {
    Undefined = 1 << 0,
    Null = 1 << 1,
    Boolean = 1 << 2,
    Number = 1 << 3,
    String = 1 << 4,
    BigInt = 1 << 5,
    Array = 1 << 6,
    Object = 1 << 7,
};

OptionSet<RuntimeClass> runtimeClassesOf(const Program&, ResultNodeRef);
bool appliesDefault(const Program&, ResultNodeRef);

class ValidatorBuilder final : public Emission {
public:
    ValidatorBuilder(IRBuilder&, const EmissionInputs&);

    ValueID emitNode(ResultNodeRef, ValueID value, const ValidationExits&);
    ValueID emitNodeBody(ResultNodeRef, ValueID value, const ValidationExits&);

    static DerivedSignature signatureOf(Emission&, const ResultNode&);

private:
    struct PlainObject {
        ValueID object;
        ValueID guard;
    };

    struct CopiedMember {
        const ResultMember* member { nullptr };
        ValueID value;
    };

    ValueID emitCallOfValidator(ResultNodeRef, const ResultNode&, ValueID value, const ValidationExits&);
    ValueID emitInterpretedNode(ResultNodeRef, ValueID value, const ValidationExits&);
    ValueID emitDefault(const ResultNode&, ValueID value, BlockID whenDefined);

    ValueID emitPrimitive(const ResultNode&, ValueID value, const ValidationExits&);
    ValueID emitLiteral(const ResultNode&, ValueID value, const ValidationExits&);
    ValueID emitAnyObject(ValueID value, const ValidationExits&);

    ValueID emitPlainArray(ValueID value, const ValidationExits&);
    ValueID emitArray(const ResultNode&, ValueID value, const ValidationExits&);
    ValueID emitTuple(const ResultNode&, ValueID value, const ValidationExits&);

    PlainObject emitPlainObject(ValueID value, const ValidationExits&);
    ValueID emitObject(ResultNodeRef, const ResultNode&, ValueID value, const ValidationExits&);
    ValueID emitMember(const PlainObject&, const ResultMember&, const ValidationExits&);
    void emitRejectUndeclaredMembers(const PlainObject&, const ResultNode&, const ValidationExits&);
    void emitValidateUndeclaredMembers(const PlainObject&, const ResultNode&, ValueID copy, const ValidationExits&);
    void emitBranchOnDeclaredKey(const ResultNode&, ValueID key, BlockID whenDeclared, BlockID whenUndeclared);
    ValueID emitCopy(const PlainObject&, const ResultNode&, std::span<const CopiedMember>, const ValidationExits&);
    ValueID emitDictionary(const ResultNode&, ValueID value, const ValidationExits&);

    struct OwnKeyLoop {
        ValueID keys;
        ValueID key;
        BlockID body;
        BlockID next;
        BlockID done;
    };
    OwnKeyLoop beginOwnKeyLoop(ValueID object);

    ValueID emitUnion(ResultNodeRef, const ResultNode&, ValueID value, const ValidationExits&);
    ValueID emitArmsOfClass(RuntimeClass, std::span<const ResultNodeRef> arms, ValueID value, const ValidationExits&);
    ValueID emitArmsInOrder(std::span<const ResultNodeRef> arms, ValueID value, const ValidationExits&);
    ValueID emitArmsByDiscriminant(const ResultDiscriminant&, std::span<const ResultNodeRef> arms, ValueID value, const ValidationExits&);
    ValueID emitRequireClass(RuntimeClass, ValueID value, BlockID otherwise);

    NodeDecisionInputs decisionInputs() const { return { inputs().program, inputs().flow, inputs().build }; }
    bool validationEndsStretch(ResultNodeRef);

    ValidationRuleEmitter m_rules;
};

inline constexpr uint32_t valueResultIndex = 1;

inline const Field& fieldOf(const Program& program, const ResultNode& node, const ResultMember& member)
{
    return program.at(node.layout).fields[*member.field];
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
