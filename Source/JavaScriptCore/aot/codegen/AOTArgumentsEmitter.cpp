/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArgumentsEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTCallEmitter.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTConvention.h"
#include "AOTConversionEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTEntryEmitter.h"
#include "AOTMachineFrame.h"
#include "AOTStubCatalog.h"
#include "AOTStubsCallLists.h"
#include "AOTTypeArena.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "Interpreter.h"
#include <iterator>

namespace JSC { namespace AOT { namespace Typed {

Word32 ArgumentsEmitter::restLength(uint32_t firstArgument)
{
    Word32 count = emitter<EntryEmitter>().argumentCount();
    if (!firstArgument)
        return count;
    Word32 skipped = word32(static_cast<int32_t>(firstArgument));
    Condition hasRest = below(skipped, count);
    return select(hasRest, sub(count, skipped), 0);
}

Pointer ArgumentsEmitter::restList(uint32_t firstArgument)
{
    EntryEmitter& entry = emitter<EntryEmitter>();
    RELEASE_ASSERT(!entry.indexOfFirstArgumentInList());
    return offsetBy(entry.argumentList(), static_cast<ptrdiff_t>(firstArgument) * sizeOfStackWord);
}

void ArgumentsEmitter::generateGetArgument(const ValueContext& value)
{
    uint32_t index = value->argumentIndex();
    Pointer list = restList(0);
    auto load = [&] {
        return Boxed(m_out.load64(m_out.address(m_heaps.argumentBuffer, list.value(), static_cast<ptrdiff_t>(index) * sizeOfStackWord)));
    };
    if (index < emitter<EntryEmitter>().numberOfArgumentsAlwaysInList()) {
        define(value, load());
        return;
    }
    Word32 position = word32(static_cast<int32_t>(index));
    define(value, either<Boxed>(below(position, emitter<EntryEmitter>().argumentCount()), load, [&] {
        return undefined();
    }));
}

void ArgumentsEmitter::generateGetRestLength(const ValueContext& value)
{
    define(value, restLength(value->firstArgument()));
}

std::optional<uint32_t> ArgumentsEmitter::findForwardedRest(ValueID rest) const
{
    static constexpr uint32_t maxFirstArgument = 64;
    if (!rest || !m_code.has(Mechanism::EscapeAnalysis, m_facts))
        return std::nullopt;
    const Operation& created = m_code.at(rest);
    if (created.opcode != Opcode::CreateRest || created.storage() != AllocationStorage::Heap || created.firstArgument() > maxFirstArgument)
        return std::nullopt;
    ValueID call = m_code.useCountOf(rest) == 1 ? m_code.onlyUserOf(rest) : ValueID();
    if (!call || m_code.at(call).opcode != Opcode::CallList || m_code.at(call).listOperand() != rest || m_code.at(call).origin().isTailCall)
        return std::nullopt;
    const Terminator& terminator = m_code.at(m_code.blockOf(call)).terminator;
    if (terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == call)
        return std::nullopt;
    for (const LiveAtSuspension& record : m_code.suspensions()) {
        if (std::ranges::contains(m_code.liveAt(record.suspension), rest))
            return std::nullopt;
    }
    return created.firstArgument();
}

void ArgumentsEmitter::generateCreateRest(const ValueContext& value)
{
    if (findForwardedRest(value.id())) {
        define(value, CellPointer(loadImmutablePointer(AOT_INSTANCE_OFFSET(realm.emptyArgumentList)).value()));
        return;
    }
    if (value->storage() == AllocationStorage::Frame)
        generator().fail(value, InternalErrorKind::OpcodeWithoutCode, "the frame has no form for a list of arguments"_s);
    uint32_t firstArgument = value->firstArgument();
    define(value, emitter<CellAllocationEmitter>().newArrayFromBuffer(value, restLength(firstArgument), restList(firstArgument)));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
