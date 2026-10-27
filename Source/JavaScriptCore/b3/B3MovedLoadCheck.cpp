/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3MovedLoadCheck.h"

#if ENABLE(B3_JIT)

#include "AllowMacroScratchRegisterUsage.h"
#include "B3BasicBlockInlines.h"
#include "B3Dominators.h"
#include "B3InsertionSetInlines.h"
#include "B3MemoryValueInlines.h"
#include "B3PatchpointValue.h"
#include "B3ProcedureInlines.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueInlines.h"
#include "CCallHelpers.h"
#include <algorithm>
#include <wtf/HashSet.h>

namespace JSC { namespace B3 {

std::optional<MovedLoad> MovedLoad::of(const Procedure& proc, const Value* value)
{
    const Policy& policy = proc.policy();
    if (!policy.validation.logsMovedLoads)
        return std::nullopt;
    std::optional<MovedLoad> moved = describe(value);
    if (moved && policy.saysWhatCountsOfMovedLoad && !policy.saysWhatCountsOfMovedLoad->run(*value->as<MemoryValue>(), *moved))
        return std::nullopt;
    return moved;
}

std::optional<MovedLoad> MovedLoad::describe(const Value* value)
{
    const MemoryValue* load = value->as<MemoryValue>();
    if (!load || !load->isLoad() || load->isExotic() || load->type().isVector())
        return std::nullopt;
    return MovedLoad { load->opcode(), load->type(), load->offset(), load->origin(), load->range() };
}

static void emitLoad(CCallHelpers& jit, const MovedLoad& load, CCallHelpers::Address location, GPRReg result)
{
    switch (load.opcode) {
    case Load8Z:
        jit.load8(location, result);
        return;
    case Load8S:
        jit.load8SignedExtendTo32(location, result);
        return;
    case Load16Z:
        jit.load16(location, result);
        return;
    case Load16S:
        jit.load16SignedExtendTo32(location, result);
        return;
    case Load:
        if (sizeofType(load.type) == 8)
            jit.load64(location, result);
        else
            jit.load32(location, result);
        return;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

PatchpointValue* createMovedLoadCheck(Procedure& proc, const MovedLoad& load, Value* address, Value* givenToUsers, RefPtr<DifferenceReport> report)
{
    RELEASE_ASSERT(givenToUsers->type() == load.type);
    PatchpointValue* check = proc.add<PatchpointValue>(Void, load.origin);
    check->append(ConstrainedValue(address, ValueRep::SomeRegister));
    check->append(ConstrainedValue(givenToUsers, ValueRep::SomeRegister));
    check->numGPScratchRegisters = load.comparesBitByBit() ? 4 : 2;
    check->clobber(RegisterSet::macroClobberedGPRs());
    check->effects = Effects::none();
    check->effects.writesLocalState = true;
    check->setRunsWithoutFrame(true);
    check->setGenerator([load, report = WTF::move(report)](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        GPRReg found = params.gpScratch(0);
        GPRReg given = params.gpScratch(1);
        bool is64Bit = sizeofType(load.type) == 8;
        emitLoad(jit, load, CCallHelpers::Address(params[0].gpr(), load.offset), found);
        if (params[1].isGPR())
            given = params[1].gpr();
        else if (is64Bit)
            jit.moveDoubleTo64(params[1].fpr(), given);
        else
            jit.moveFloatTo32(params[1].fpr(), given);
        CCallHelpers::Jump differs = is64Bit ? jit.branch64(CCallHelpers::NotEqual, found, given) : jit.branch32(CCallHelpers::NotEqual, found, given);
        CCallHelpers::Label done = jit.label();
        GPRReg differing = load.comparesBitByBit() ? params.gpScratch(2) : InvalidGPRReg;
        GPRReg counted = load.comparesBitByBit() ? params.gpScratch(3) : InvalidGPRReg;
        params.addLatePath([=](CCallHelpers& jit) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            differs.link(&jit);
            if (uint64_t bits = load.bitsOfWordsThatCount) {
                CCallHelpers::Jump counts = jit.branchTest64(CCallHelpers::NonZero, found, CCallHelpers::TrustedImm64(static_cast<int64_t>(bits)));
                jit.branchTest64(CCallHelpers::Zero, given, CCallHelpers::TrustedImm64(static_cast<int64_t>(bits))).linkTo(done, &jit);
                counts.link(&jit);
            }
            if (load.mayGetFilled)
                (is64Bit ? jit.branchTest64(CCallHelpers::Zero, given) : jit.branchTest32(CCallHelpers::Zero, given)).linkTo(done, &jit);
            for (const WordChange& change : load.changesThatDoNotCount) {
                RELEASE_ASSERT(!is64Bit);
                CCallHelpers::Jump isOther = jit.branch32(CCallHelpers::NotEqual, given, CCallHelpers::TrustedImm32(static_cast<int32_t>(change.before)));
                jit.branch32(CCallHelpers::Equal, found, CCallHelpers::TrustedImm32(static_cast<int32_t>(change.after))).linkTo(done, &jit);
                isOther.link(&jit);
            }
            if (load.comparesBitByBit()) {
                uint64_t bitsOfLoad = is64Bit ? std::numeric_limits<uint64_t>::max() : std::numeric_limits<uint32_t>::max();
                jit.move(found, differing);
                jit.xor64(given, differing);
                jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(load.bitsThatCount & bitsOfLoad)), counted);
                jit.and64(counted, differing);
                jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(~load.bitsThatMayGetSet)), counted);
                jit.and64(found, counted);
                CCallHelpers::Jump wasSet = jit.branchTest64(CCallHelpers::NonZero, counted, differing);
                jit.move(found, counted);
                jit.not64(counted);
                jit.and64(differing, counted);
                jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(~load.bitsThatMayGetCleared)), differing);
                jit.branchTest64(CCallHelpers::Zero, counted, differing).linkTo(done, &jit);
                wasSet.link(&jit);
            }
            if (report)
                report->run(jit, load, found, given);
            jit.breakpoint();
        });
    });
    return check;
}

static bool staysInPlace(const Value* value)
{
    Effects effects = value->effects();
    return effects.terminal || effects.exitsSideways || effects.writes || effects.writesPinned || effects.fence;
}

void MovedLoadLog::name(Value* value, unsigned entry)
{
    if (value)
        m_entriesNaming.add(value, Vector<unsigned, 2>()).iterator->value.append(entry);
}

void MovedLoadLog::add(const MovedLoad& load, BasicBlock* block, unsigned index, Value* address, Value* givenToUsers, Value* store)
{
    Value* before = nullptr;
    while (index-- && !before) {
        if (staysInPlace(block->at(index)))
            before = block->at(index);
    }
    // What an address is computed from outlives the address.
    int64_t offset = load.offset;
    for (;; address = address->child(0)) {
        if (address->opcode() == Identity)
            continue;
        if (address->opcode() != Add || !address->child(1)->hasInt64() || !isRepresentableAs<int32_t>(offset + address->child(1)->asInt64()))
            break;
        offset += address->child(1)->asInt64();
    }
    unsigned entry = m_entries.size();
    m_entries.append(Entry { load, address, givenToUsers, store, before, block });
    m_entries.last().load.offset = static_cast<int32_t>(offset);
    for (Value* value : { address, givenToUsers, store, before })
        name(value, entry);
}

void MovedLoadLog::addBehindEachValueThatStaysIn(BasicBlock* block, const MovedLoad& load, Value* address, Value* givenToUsers)
{
    for (unsigned index = 0; index + 1 < block->size(); ++index) {
        if (staysInPlace(block->at(index)))
            add(load, block, index + 1, address, givenToUsers);
    }
}

void MovedLoadLog::didClone(Value* original, Value* clone)
{
    if (isEmpty())
        return;
    auto found = m_entriesNaming.find(original);
    if (found == m_entriesNaming.end())
        return;
    if (m_depthOfRegionCopies) {
        m_copiesInRegion.set(original, clone);
        return;
    }
    // Where there are two of a value, its users may be handed a Phi of the two, and the log does not hear of that.
    for (unsigned index : found->value) {
        Entry& entry = m_entries[index];
        if (entry.address == original || entry.givenToUsers == original)
            drop(entry);
    }
}

void MovedLoadLog::willCopyRegion()
{
    if (!m_depthOfRegionCopies++)
        m_numberOfEntriesBeforeRegionCopy = m_entries.size();
}

void MovedLoadLog::didCopy(BasicBlock* original, BasicBlock* copy)
{
    RELEASE_ASSERT(m_depthOfRegionCopies);
    if (!isEmpty())
        m_blockCopiesInRegion.set(original, copy);
}

void MovedLoadLog::didCopyRegion()
{
    RELEASE_ASSERT(m_depthOfRegionCopies);
    if (--m_depthOfRegionCopies)
        return;
    auto copies = std::exchange(m_copiesInRegion, { });
    auto blockCopies = std::exchange(m_blockCopiesInRegion, { });
    auto copyOf = [&](Value* value) {
        Value* copy = value ? copies.get(value) : nullptr;
        return copy ? copy : value;
    };
    for (unsigned index = 0; index < m_numberOfEntriesBeforeRegionCopy; ++index) {
        Entry copy = m_entries[index];
        if (!copy.address)
            continue;
        if (!(copy.before ? copies.contains(copy.before) : blockCopies.contains(copy.block))) {
            if (copies.contains(copy.address) || copies.contains(copy.givenToUsers))
                drop(m_entries[index]);
            continue;
        }
        copy.address = copyOf(copy.address);
        copy.givenToUsers = copyOf(copy.givenToUsers);
        copy.store = copyOf(copy.store);
        copy.before = copyOf(copy.before);
        if (BasicBlock* block = blockCopies.get(copy.block))
            copy.block = block;
        unsigned entry = m_entries.size();
        m_entries.append(copy);
        for (Value* value : { copy.address, copy.givenToUsers, copy.store, copy.before })
            name(value, entry);
    }
}

void MovedLoadLog::dropEntriesWrittenBy(const Value* value)
{
    HeapSet writes = value->effects().writes;
    if (!writes)
        return;
    for (Entry& entry : m_entries) {
        if (writes.overlaps(entry.load.range))
            drop(entry);
    }
}

void MovedLoadLog::willDelete(Value* value)
{
    if (isEmpty())
        return;
    dropEntriesWrittenBy(value);
    if (m_depthOfRegionCopies) {
        m_copiesInRegion.removeIf([&](auto& pair) {
            return pair.value == value;
        });
    }
    Vector<unsigned, 2> entries = m_entriesNaming.take(value);
    Value* standsFor = value->opcode() == Identity ? value->child(0) : nullptr;
    for (unsigned index : entries) {
        Entry& entry = m_entries[index];
        if (!standsFor || entry.store == value || entry.before == value) {
            drop(entry);
            continue;
        }
        if (entry.address == value)
            entry.address = standsFor;
        if (entry.givenToUsers == value)
            entry.givenToUsers = standsFor;
        name(standsFor, index);
    }
}

void MovedLoadLog::didSplitForward(BasicBlock* block, BasicBlock* head)
{
    for (Entry& entry : m_entries) {
        if (entry.block == block)
            entry.block = head;
    }
}

void MovedLoadLog::willDelete(BasicBlock* block)
{
    for (Entry& entry : m_entries) {
        if (entry.block == block && !entry.before)
            drop(entry);
    }
}

void MovedLoadLog::willMove(Value* write)
{
    if (isEmpty())
        return;
    dropEntriesWrittenBy(write);
    auto entries = m_entriesNaming.find(write);
    if (entries == m_entriesNaming.end())
        return;
    for (unsigned index : entries->value)
        drop(m_entries[index]);
}

bool MovedLoadLog::insertChecks(Procedure& proc, RefPtr<DifferenceReport> report)
{
    Vector<Entry> entries = std::exchange(m_entries, { });
    m_entriesNaming.clear();
    if (entries.isEmpty())
        return false;

    struct Place {
        BasicBlock* block { nullptr };
        unsigned index { 0 };
    };
    // An entry may name a value that is gone: what stood for a deleted value may have been deleted before it. So nothing that an entry names is looked into before it is found here.
    UncheckedKeyHashMap<Value*, Place> places;
    UncheckedKeyHashSet<BasicBlock*> blocks;
    for (BasicBlock* block : proc) {
        blocks.add(block);
        for (unsigned index = 0; index < block->size(); ++index)
            places.add(block->at(index), Place { block, index });
    }
    Dominators& dominators = proc.dominators();

    struct Check {
        Place place;
        PatchpointValue* value;
    };
    Vector<Check> checks;
    for (Entry& entry : entries) {
        if (!entry.address)
            continue;
        Place place { entry.block, 0 };
        if (entry.before) {
            place = places.get(entry.before);
            if (!place.block)
                continue;
            ++place.index;
        } else if (!blocks.contains(entry.block))
            continue;
        if (place.index >= place.block->size())
            continue;
        while (!staysInPlace(place.block->at(place.index)))
            ++place.index;

        auto isThere = [&](Value* value) {
            Place made = places.get(value);
            if (!made.block)
                return false;
            return made.block == place.block ? made.index < place.index : dominators.strictlyDominates(made.block, place.block);
        };
        if (!isThere(entry.address) || !isThere(entry.givenToUsers) || (entry.store && !isThere(entry.store)) || entry.givenToUsers->type() != entry.load.type)
            continue;
        checks.append({ place, createMovedLoadCheck(proc, entry.load, entry.address, entry.givenToUsers, report) });
    }

    std::ranges::stable_sort(checks, [](const Check& a, const Check& b) {
        return a.place.block == b.place.block ? a.place.index < b.place.index : a.place.block->index() < b.place.block->index();
    });
    InsertionSet insertionSet(proc);
    for (unsigned index = 0; index < checks.size(); ++index) {
        insertionSet.insertValue(checks[index].place.index, checks[index].value);
        if (index + 1 == checks.size() || checks[index + 1].place.block != checks[index].place.block)
            insertionSet.execute(checks[index].place.block);
    }
    return !checks.isEmpty();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
