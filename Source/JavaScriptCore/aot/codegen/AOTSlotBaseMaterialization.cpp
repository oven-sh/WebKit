/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFrameAddressValidation.h"

#if ENABLE(AOT)

#include "B3BasicBlock.h"
#include "B3InsertionSetInlines.h"
#include "B3PatchpointValue.h"
#include "B3Procedure.h"
#include "B3SlotBaseValue.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include <wtf/IndexMap.h>
#include <wtf/HashSet.h>
#include <wtf/IndexSet.h>
#include <wtf/SharedTask.h>

namespace JSC { namespace AOT {

namespace {

// A distance from the start of a stack slot, or from the frame pointer where there is no slot.
struct FrameLocation {
    bool operator==(const FrameLocation&) const = default;

    B3::Air::StackSlot* slot { nullptr };
    int64_t offset { 0 };
};

struct IncomingLocations {
    enum class Kind : uint8_t { None, One, Other };

    static IncomingLocations one(FrameLocation location) { return { Kind::One, location }; }
    static IncomingLocations other() { return { Kind::Other, { } }; }

    IncomingLocations movedBy(int64_t distance) const
    {
        if (kind != Kind::One)
            return *this;
        return one({ location.slot, location.offset + distance });
    }

    bool merge(const IncomingLocations& incoming)
    {
        if (incoming.kind == Kind::None || kind == Kind::Other)
            return false;
        if (kind == Kind::One && incoming.kind == Kind::One && location == incoming.location)
            return false;
        *this = kind == Kind::None ? incoming : other();
        return true;
    }

    Kind kind { Kind::None };
    FrameLocation location;
};

class SlotBaseMaterialization {
public:
    explicit SlotBaseMaterialization(B3::Procedure& proc)
        : m_proc(proc)
        , m_insertionSet(proc)
    {
    }

    bool run()
    {
        findAddresses();
        if (m_addresses.isEmpty())
            return false;
        if (findPhisOfOneLocation())
            findAddresses();

        bool changed = false;
        for (B3::BasicBlock* block : m_proc) {
            for (unsigned index = 0; index < block->size(); ++index) {
                B3::Value* user = block->at(index);
                if (m_addresses.contains(user))
                    continue;
                for (B3::Value*& child : user->children()) {
                    if (!m_addresses.contains(child))
                        continue;
                    child = insertCopyBefore(index, child);
                    changed = true;
                }
            }
            m_insertionSet.execute(block);
        }
        return changed;
    }

private:
    bool computesAddress(B3::Value* value) const
    {
        B3::Opcode opcode = value->opcode();
        if (opcode == B3::SlotBase || opcode == B3::FramePointer)
            return true;
        if (opcode == B3::Add || opcode == B3::Sub)
            return m_addresses.contains(value->child(0)) || m_addresses.contains(value->child(1));
        if (opcode == B3::Identity)
            return m_addresses.contains(value->child(0));
        if (opcode == B3::Select)
            return m_addresses.contains(value->child(1)) || m_addresses.contains(value->child(2));
        return false;
    }

    void findAddresses()
    {
        for (B3::BasicBlock* block : m_proc.cachedBlocksInPreOrder()) {
            for (B3::Value* value : *block) {
                if (computesAddress(value))
                    m_addresses.add(value);
            }
        }
    }

    IncomingLocations locationsOf(B3::Value* value) const
    {
        switch (value->opcode()) {
        case B3::SlotBase:
            return IncomingLocations::one({ value->as<B3::SlotBaseValue>()->slot(), 0 });
        case B3::FramePointer:
            return IncomingLocations::one({ });
        case B3::Identity:
            return locationsOf(value->child(0));
        case B3::Add:
            if (value->child(1)->hasInt64())
                return locationsOf(value->child(0)).movedBy(value->child(1)->asInt64());
            if (value->child(0)->hasInt64())
                return locationsOf(value->child(1)).movedBy(value->child(0)->asInt64());
            return IncomingLocations::other();
        case B3::Sub:
            if (value->child(1)->hasInt64())
                return locationsOf(value->child(0)).movedBy(-value->child(1)->asInt64());
            return IncomingLocations::other();
        case B3::Phi:
            return m_locationsOfPhi[value];
        default:
            return IncomingLocations::other();
        }
    }

    // A phase that copies blocks keeps a value that lives across them in a variable, and the variable comes back as Phis. All of them stand for the one address.
    bool findPhisOfOneLocation()
    {
        m_locationsOfPhi.resize(m_proc.values().size());
        bool changed = true;
        while (changed) {
            changed = false;
            for (B3::BasicBlock* block : m_proc) {
                for (B3::Value* value : *block) {
                    if (auto* upsilon = value->as<B3::UpsilonValue>())
                        changed |= m_locationsOfPhi[upsilon->phi()].merge(locationsOf(upsilon->child(0)));
                }
            }
        }

        bool found = false;
        for (B3::BasicBlock* block : m_proc) {
            for (B3::Value* value : *block) {
                if (value->opcode() == B3::Phi && m_locationsOfPhi[value].kind == IncomingLocations::Kind::One)
                    found |= m_addresses.add(value);
            }
        }
        return found;
    }

    B3::Value* insertLocationBefore(unsigned index, const FrameLocation& location, B3::Origin origin)
    {
        B3::Value* base = location.slot
            ? m_insertionSet.insert<B3::SlotBaseValue>(index, origin, location.slot)
            : m_insertionSet.insert<B3::Value>(index, B3::FramePointer, origin);
        if (!location.offset)
            return base;
        return m_insertionSet.insert<B3::Value>(index, B3::Add, origin, base, m_insertionSet.insertIntConstant(index, origin, B3::pointerType(), location.offset));
    }

    B3::Value* insertCopyBefore(unsigned index, B3::Value* address)
    {
        if (address->opcode() == B3::Phi)
            return insertLocationBefore(index, m_locationsOfPhi[address].location, address->origin());

        B3::Value* copy = m_proc.clone(address);
        for (B3::Value*& child : copy->children()) {
            if (m_addresses.contains(child))
                child = insertCopyBefore(index, child);
        }
        return m_insertionSet.insertValue(index, copy);
    }

    B3::Procedure& m_proc;
    B3::InsertionSet m_insertionSet;
    IndexSet<B3::Value*> m_addresses;
    IndexMap<B3::Value*, IncomingLocations> m_locationsOfPhi;
};

} // anonymous namespace

B3::ClientPhase materializeSlotBasesLate()
{
    return {
        B3::ClientPhase::Point::AfterOptimization,
        "materializeSlotBasesLate"_s,
        createSharedTask<bool(B3::Procedure&)>([](B3::Procedure& proc) {
            return SlotBaseMaterialization(proc).run();
        })
    };
}

// CellAllocationEmitter::allocateInFrame() makes the one patchpoint that has no code.
static bool isBirth(const B3::Value* value)
{
    const auto* patchpoint = value->as<B3::PatchpointValue>();
    return patchpoint && !patchpoint->generator();
}

// CSE gives two births of one slot the same SlotBase, and tail duplication of its block then puts phis between the copies and the second birth.
static void checkThatOnlyBirthsTakeSlotsOfCells(B3::Procedure& proc)
{
    IndexMap<B3::Value*, Vector<B3::Value*, 2>> incomingValues(proc.values().size());
    for (B3::Value* value : proc.values()) {
        if (auto* upsilon = value->as<B3::UpsilonValue>())
            incomingValues[upsilon->phi()].append(upsilon->child(0));
    }

    UncheckedKeyHashSet<B3::Air::StackSlot*> slotsOfCells;
    IndexSet<B3::Value*> seen;
    Vector<B3::Value*, 8> worklist;
    for (B3::Value* value : proc.values()) {
        if (isBirth(value))
            worklist.append(value->child(0));
    }
    while (!worklist.isEmpty()) {
        B3::Value* address = worklist.takeLast();
        if (!seen.add(address))
            continue;
        if (auto* slotBase = address->as<B3::SlotBaseValue>()) {
            slotsOfCells.add(slotBase->slot());
            continue;
        }
        RELEASE_ASSERT(address->opcode() == B3::Phi);
        worklist.appendVector(incomingValues[address]);
    }

    IndexSet<B3::Value*> addresses;
    for (B3::Value* value : proc.values()) {
        auto* slotBase = value->as<B3::SlotBaseValue>();
        if (slotBase && slotsOfCells.contains(slotBase->slot()))
            addresses.add(value);
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (B3::Value* value : proc.values()) {
            auto* upsilon = value->as<B3::UpsilonValue>();
            if (upsilon && addresses.contains(upsilon->child(0)))
                changed |= addresses.add(upsilon->phi());
        }
    }
    for (B3::Value* value : proc.values()) {
        if (isBirth(value) || value->opcode() == B3::Upsilon)
            continue;
        for (B3::Value* child : value->children())
            RELEASE_ASSERT(!addresses.contains(child));
    }
}

B3::ClientPhase dissolveBirthsOfFrameCells(const bool& hasBirths, bool verifies)
{
    return {
        B3::ClientPhase::Point::AfterOptimization,
        "dissolveBirthsOfFrameCells"_s,
        createSharedTask<bool(B3::Procedure&)>([&hasBirths, verifies](B3::Procedure& proc) {
            if (!hasBirths)
                return false;
            if (verifies)
                checkThatOnlyBirthsTakeSlotsOfCells(proc);
            for (B3::Value* value : proc.values()) {
                if (isBirth(value))
                    value->replaceWithIdentity(value->child(0));
            }
            for (B3::Value* value : proc.values())
                value->performSubstitution();
            return true;
        })
    };
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
