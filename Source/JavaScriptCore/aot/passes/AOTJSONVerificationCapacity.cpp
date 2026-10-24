/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONVerificationCapacity.h"

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTStringBuilderCursor.h"
#include "AOTThreshold.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

AOT_THRESHOLD(unsigned, maxCapacityRounds, 4);

static ValueID forwardedCursorOf(const Value& value)
{
    if (value.opcode == Opcode::ResultAt)
        return value.resultsOperand();
    if (isCast(value.opcode))
        return value.operand(0);
    return { };
}

namespace {

class BuilderCapacityVerifier {
    WTF_MAKE_NONCOPYABLE(BuilderCapacityVerifier);
public:
    BuilderCapacityVerifier(const Program&, const Function&, const CFG&);

    std::optional<BuilderCapacityViolation> run();

private:
    bool isCursor(ValueID id) const { return m_program.typeOf(m_function, id).opaque == OpaqueKind::BuilderCursor; }
    uint32_t reservedBytesOf(ValueID id) const { return m_reservedBytes[id.index()].value_or(0); }
    std::optional<uint32_t> reservedBytesOnEdge(Edge, uint32_t parameterIndex) const;
    bool mergeParameters(BlockID, bool shouldWiden);
    uint32_t computeReservedBytes(BlockID, ValueID);
    void visitBlock(BlockID);

    const Program& m_program;
    const Function& m_function;
    const CFG& m_cfg;
    Vector<std::optional<uint32_t>> m_reservedBytes;
    std::optional<BuilderCapacityViolation> m_violation;
};

BuilderCapacityVerifier::BuilderCapacityVerifier(const Program& program, const Function& function, const CFG& cfg)
    : m_program(program)
    , m_function(function)
    , m_cfg(cfg)
    , m_reservedBytes(function.values.size())
{
}

std::optional<BuilderCapacityViolation> BuilderCapacityVerifier::run()
{
    bool didChange = true;
    for (unsigned round = 1; didChange; ++round) {
        didChange = false;
        m_violation = std::nullopt;
        for (BlockID block : m_cfg.reversePostOrder()) {
            didChange |= mergeParameters(block, round >= maxCapacityRounds);
            visitBlock(block);
        }
    }
    return m_violation;
}

std::optional<uint32_t> BuilderCapacityVerifier::reservedBytesOnEdge(Edge edge, uint32_t parameterIndex) const
{
    IncomingValue incoming = incomingValueOf(m_function, edge, parameterIndex);
    switch (incoming.kind) {
    case IncomingValue::Kind::Argument:
        return m_reservedBytes[incoming.argument.index()];
    case IncomingValue::Kind::EdgeResult:
        return 0;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool BuilderCapacityVerifier::mergeParameters(BlockID block, bool shouldWiden)
{
    bool didChange = false;
    auto& parameters = m_function.at(block).parameters;
    for (uint32_t index = 0; index < parameters.size(); ++index) {
        if (!isCursor(parameters[index]))
            continue;
        std::optional<uint32_t>& reservedBytes = m_reservedBytes[parameters[index].index()];
        std::optional<uint32_t> merged = reservedBytes;
        for (Edge edge : m_cfg.incomingEdges(block)) {
            if (std::optional<uint32_t> incoming = reservedBytesOnEdge(edge, index))
                merged = std::min(merged.value_or(*incoming), *incoming);
        }
        if (merged == reservedBytes)
            continue;
        if (shouldWiden)
            merged = 0;
        reservedBytes = merged;
        didChange = true;
    }
    return didChange;
}

uint32_t BuilderCapacityVerifier::computeReservedBytes(BlockID block, ValueID id)
{
    const Value& row = m_function.at(id);
    if (row.opcode == Opcode::StringBuilderReserve)
        return row.size();

    uint32_t taken = bytesTakenFromReservation(m_program, row);
    uint32_t reserved = reservedBytesOf(row.cursorOperand());
    bool exceedsReservation = taken > reserved;
    if (exceedsReservation && !m_violation)
        m_violation = BuilderCapacityViolation { block, id, taken };
    if (renewsReservation(row.opcode))
        return maxStringBuilderReservation;
    return exceedsReservation ? 0 : reserved - taken;
}

void BuilderCapacityVerifier::visitBlock(BlockID block)
{
    for (ValueID id : m_function.at(block).values) {
        const Value& value = m_function.at(id);
        if (familyOf(value.opcode) == OperationFamily::JSONWrite) {
            m_reservedBytes[id.index()] = computeReservedBytes(block, id);
            continue;
        }
        if (!isCursor(id))
            continue;
        ValueID forwarded = forwardedCursorOf(value);
        m_reservedBytes[id.index()] = forwarded ? reservedBytesOf(forwarded) : 0;
    }
}

} // anonymous namespace

std::optional<BuilderCapacityViolation> findBuilderCapacityViolation(const Program& program, const Function& function, const CFG& cfg)
{
    return BuilderCapacityVerifier(program, function, cfg).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
