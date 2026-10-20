/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRepresentationRules.h"

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTConversions.h"
#include "AOTValueIdentity.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

namespace {

bool isCastOfOperand(Opcode opcode)
{
    return isCast(opcode) || opcode == Opcode::FinishClass;
}

bool isConversion(Opcode opcode)
{
    return familyOf(opcode) == OperationFamily::Representation;
}

bool needsConversion(Representation stored, Representation wanted)
{
    return hasRegister(wanted) && stored != wanted;
}

class RepresentationChecker {
public:
    RepresentationChecker(const RepresentationInputs& inputs, Vector<InternalError>& errors)
        : m_inputs(inputs)
        , m_function(inputs.function)
        , m_errors(errors)
    {
    }

    void run()
    {
        CFG cfg(m_function);
        for (BlockID block : cfg.reversePostOrder())
            checkBlock(block);
    }

private:
    void checkBlock(BlockID id)
    {
        const Block& block = m_function.at(id);
        for (ValueID parameter : block.parameters) {
            if (!hasRepresentationOfResult(parameter))
                report(InternalErrorKind::WrongRepresentation, id, parameter);
        }
        for (ValueID value : block.values) {
            if (std::optional<InternalErrorKind> error = findError(value))
                report(*error, id, value);
        }
        if (ValueID operand = findOperandInWrongRepresentation(block.terminator))
            report(InternalErrorKind::WrongRepresentation, id, operand);
        forEachOutgoingEdge(m_function, id, [&](Edge edge, const Target& target) {
            checkEdge(edge, target);
        });
    }

    std::optional<InternalErrorKind> findError(ValueID id) const
    {
        const Value& value = m_function.at(id);
        if (!hasRepresentationOfResult(id))
            return InternalErrorKind::WrongRepresentation;
        if (isCastOfOperand(value.opcode) && storedRepresentationOf(value.operand(0)) != value.representation)
            return InternalErrorKind::WrongRepresentation;
        if (findOperandInWrongRepresentation(value))
            return InternalErrorKind::WrongRepresentation;
        if (isConversion(value.opcode) && !isSingleConversionStep(id))
            return InternalErrorKind::UnsupportedConversion;
        return std::nullopt;
    }

    bool hasRepresentationOfResult(ValueID id) const
    {
        const Value& value = m_function.at(id);
        if (!isLegalRepresentation(m_inputs.typeOf(id), value.representation))
            return false;
        Representation expected = resultRepresentationOf(m_inputs, value);
        return expected == Representation::Unchosen || expected == value.representation;
    }

    template<typename User>
    ValueID findOperandInWrongRepresentation(const User& user) const
    {
        ValueID found;
        user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (!found && needsConversion(storedRepresentationOf(operand), wantedRepresentationOf(m_inputs, user, operandIndex)))
                found = operand;
        });
        return found;
    }

    bool isSingleConversionStep(ValueID id) const
    {
        const Value& conversion = m_function.at(id);
        Representation from = storedRepresentationOf(conversion.operand(0));
        Representation to = conversion.representation;
        if (from == Representation::Unchosen || !hasRegister(to))
            return false;
        std::optional<ConversionPath> path = findConversion(m_inputs.typeOf(id), from, to);
        return path && path->size() == 1 && path->first() == ConversionStep { conversion.opcode, to };
    }

    void checkEdge(Edge edge, const Target& target)
    {
        const CompactList<ValueID>& parameters = m_function.at(target.block).parameters;
        for (uint32_t index = 0; index < parameters.size(); ++index) {
            Representation wanted = storedRepresentationOf(parameters[index]);
            IncomingValue incoming = incomingValueOf(m_function, edge, index);
            switch (incoming.kind) {
            case IncomingValue::Kind::Argument:
                if (needsConversion(storedRepresentationOf(incoming.argument), wanted))
                    report(InternalErrorKind::WrongRepresentation, edge.from, incoming.argument);
                break;
            case IncomingValue::Kind::EdgeResult:
                if (needsConversion(edgeResultRepresentationOf(m_inputs, incoming.result), wanted))
                    report(InternalErrorKind::WrongRepresentation, edge.from, parameters[index]);
                break;
            }
        }
    }

    Representation storedRepresentationOf(ValueID id) const { return m_function.at(id).representation; }

    void report(InternalErrorKind kind, BlockID block, ValueID value)
    {
        m_errors.append(InternalError { .kind = kind, .function = m_inputs.id, .block = block, .value = value });
    }

    const RepresentationInputs& m_inputs;
    const Function& m_function;
    Vector<InternalError>& m_errors;
};

} // anonymous namespace

void checkRepresentations(const RepresentationInputs& inputs, Vector<InternalError>& errors)
{
    RepresentationChecker(inputs, errors).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
