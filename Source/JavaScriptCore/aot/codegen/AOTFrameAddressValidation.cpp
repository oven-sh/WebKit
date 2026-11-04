/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFrameAddressValidation.h"

#if ENABLE(AOT)

#include "AOTCodeGenerator.h"
#include "AOTVector.h"
#include "B3BasicBlock.h"
#include "B3PatchpointValue.h"
#include "B3Procedure.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include <wtf/IndexMap.h>
#include <wtf/IndexSet.h>

namespace JSC { namespace AOT {

namespace {

// An Upsilon writes the shadow variable of its Phi and the Phi copies the shadow into its own value: two live ranges.
struct LiveValueNumbers {
    void merge(const LiveValueNumbers& other)
    {
        values.merge(other.values);
        phiShadows.merge(other.phiShadows);
        phiShadowsReadBehindSuspension.merge(other.phiShadowsReadBehindSuspension);
    }

    bool operator==(const LiveValueNumbers&) const = default;

    void killValue(size_t number, const B3::Value& definition)
    {
        if (values.remove(number) && definition.opcode() == B3::Phi)
            phiShadows.add(number);
    }

    bool killPhiShadow(size_t phiNumber)
    {
        phiShadows.remove(phiNumber);
        return phiShadowsReadBehindSuspension.remove(phiNumber);
    }

    void didReachSuspension() { phiShadowsReadBehindSuspension.merge(phiShadows); }

    BitVector values;
    BitVector phiShadows;
    BitVector phiShadowsReadBehindSuspension;
};

bool isLateUse(B3::ValueRep::Kind constraint)
{
    switch (constraint) {
    case B3::ValueRep::LateColdAny:
    case B3::ValueRep::SomeLateRegister:
    case B3::ValueRep::LateRegister:
        return true;
    case B3::ValueRep::WarmAny:
    case B3::ValueRep::ColdAny:
    case B3::ValueRep::SomeRegister:
    case B3::ValueRep::SomeRegisterWithClobber:
    case B3::ValueRep::SomeEarlyRegister:
    case B3::ValueRep::Register:
    case B3::ValueRep::Stack:
    case B3::ValueRep::StackArgument:
    case B3::ValueRep::Constant:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

class FrameAddressValidation {
public:
    FrameAddressValidation(B3::Procedure& proc, Vector<FrameAddressViolation>& violations, const BitVector& originsOfSuspensions, const BitVector& originsOfParametersWithoutObjects)
        : m_proc(proc)
        , m_violations(violations)
        , m_originsOfSuspensions(originsOfSuspensions)
        , m_originsOfParametersWithoutObjects(originsOfParametersWithoutObjects)
    {
    }

    void run()
    {
        if (!hasSuspension())
            return;
        findDerivedValues();
        if (m_derivedValues.isEmpty())
            return;
        computeLiveness();
        m_lastSuspensionOfShadow.fill(B3::Origin(), m_derivedValues.size());
        BitVector shadowsWrittenWithAddress;
        for (B3::BasicBlock* block : m_proc) {
            LiveValueNumbers live;
            visitBlockBackwards(block, live, [&](const B3::PatchpointValue& suspension, const LiveValueNumbers& liveAfterSuspension) {
                appendViolations(suspension, liveAfterSuspension);
            }, [&](size_t phiNumber) {
                shadowsWrittenWithAddress.add(phiNumber);
            });
        }
        for (size_t number : shadowsWrittenWithAddress) {
            if (!m_valuesReported.get(number))
                m_violations.append(FrameAddressViolation { m_derivedValues[number]->origin(), m_lastSuspensionOfShadow[number] });
        }
    }

private:
    bool isSuspension(const B3::Value* value) const
    {
        const auto* patchpoint = value->as<B3::PatchpointValue>();
        if (!patchpoint || patchpoint->pureFunction || patchpoint->runsWithoutFrame())
            return false;
        Typed::ValueID origin = Typed::valueOfOrigin(value->origin());
        return origin && m_originsOfSuspensions.get(origin.index());
    }

    bool hasSuspension() const
    {
        for (B3::BasicBlock* block : m_proc) {
            for (B3::Value* value : *block) {
                if (isSuspension(value))
                    return true;
            }
        }
        return false;
    }

    bool holdsNoObject(const B3::Value* phi) const
    {
        Typed::ValueID origin = Typed::valueOfOrigin(phi->origin());
        return origin && m_originsOfParametersWithoutObjects.get(origin.index());
    }

    bool isDerived(B3::Value* value) const
    {
        return m_derivedValueSet.contains(value);
    }

    bool producesFrameAddress(B3::Value* value) const
    {
        B3::Opcode opcode = value->opcode();
        if (opcode == B3::SlotBase || opcode == B3::FramePointer)
            return true;
        if (opcode == B3::Add || opcode == B3::Sub)
            return isDerived(value->child(0)) || isDerived(value->child(1));
        if (opcode == B3::Identity)
            return isDerived(value->child(0));
        if (opcode == B3::Select)
            return isDerived(value->child(1)) || isDerived(value->child(2));
        return false;
    }

    void findDerivedValues()
    {
        bool changed = true;
        while (changed) {
            changed = false;
            for (B3::BasicBlock* block : m_proc) {
                for (B3::Value* value : *block) {
                    if (auto* upsilon = value->as<B3::UpsilonValue>()) {
                        if (isDerived(upsilon->child(0)) && !holdsNoObject(upsilon->phi()))
                            changed |= m_derivedValueSet.add(upsilon->phi());
                        continue;
                    }
                    if (producesFrameAddress(value))
                        changed |= m_derivedValueSet.add(value);
                }
            }
        }
        if (m_derivedValueSet.isEmpty())
            return;

        m_numberOf.resize(m_proc.values().size());
        for (B3::BasicBlock* block : m_proc) {
            for (B3::Value* value : *block) {
                if (!isDerived(value))
                    continue;
                m_numberOf[value] = m_derivedValues.size();
                m_derivedValues.append(value);
            }
        }
    }

    void computeLiveness()
    {
        m_liveAtHead.grow(m_proc.size());
        const Vector<B3::BasicBlock*>& blocksInPostOrder = m_proc.cachedBlocksInPostOrder();
        bool changed = true;
        while (changed) {
            changed = false;
            for (B3::BasicBlock* block : blocksInPostOrder) {
                LiveValueNumbers live;
                visitBlockBackwards(block, live, [](const B3::PatchpointValue&, const LiveValueNumbers&) { }, [](size_t) { });
                if (live == m_liveAtHead[block->index()])
                    continue;
                m_liveAtHead[block->index()] = WTF::move(live);
                changed = true;
            }
        }
    }

    template<typename SuspensionFunctor, typename ShadowFunctor>
    void visitBlockBackwards(B3::BasicBlock* block, LiveValueNumbers& live, const SuspensionFunctor& functor, const ShadowFunctor& didWriteAddressReadBehindSuspension)
    {
        for (B3::BasicBlock* successor : block->successorBlocks())
            live.merge(m_liveAtHead[successor->index()]);

        for (unsigned index = block->size(); index--;) {
            B3::Value* value = block->at(index);
            if (auto* upsilon = value->as<B3::UpsilonValue>()) {
                if (isDerived(upsilon->phi()) && live.killPhiShadow(m_numberOf[upsilon->phi()]) && isDerived(upsilon->child(0)))
                    didWriteAddressReadBehindSuspension(m_numberOf[upsilon->phi()]);
            } else if (isDerived(value))
                live.killValue(m_numberOf[value], *value);

            if (isSuspension(value)) {
                functor(*value->as<B3::PatchpointValue>(), live);
                live.didReachSuspension();
            }

            for (B3::Value* child : value->children()) {
                if (isDerived(child))
                    live.values.add(m_numberOf[child]);
            }
        }
    }

    void appendViolations(const B3::PatchpointValue& suspension, const LiveValueNumbers& live)
    {
        BitVector reported = live.values;
        for (size_t number : reported)
            appendViolation(suspension, number);
        for (size_t number : live.phiShadows)
            m_lastSuspensionOfShadow[number] = suspension.origin();

        for (unsigned index = 0; index < suspension.numChildren(); ++index) {
            B3::Value* child = suspension.child(index);
            if (!isDerived(child) || !isLateUse(suspension.constrainedChild(index).rep().kind()))
                continue;
            if (reported.add(m_numberOf[child]))
                appendViolation(suspension, m_numberOf[child]);
        }
    }

    void appendViolation(const B3::PatchpointValue& suspension, size_t number)
    {
        m_violations.append(FrameAddressViolation { m_derivedValues[number]->origin(), suspension.origin() });
        m_valuesReported.add(number);
    }

    B3::Procedure& m_proc;
    Vector<FrameAddressViolation>& m_violations;
    const BitVector& m_originsOfSuspensions;
    const BitVector& m_originsOfParametersWithoutObjects;
    IndexSet<B3::Value*> m_derivedValueSet;
    Vector<B3::Value*> m_derivedValues;
    IndexMap<B3::Value*, unsigned> m_numberOf;
    Vector<LiveValueNumbers> m_liveAtHead;
    Vector<B3::Origin> m_lastSuspensionOfShadow;
    BitVector m_valuesReported;
};

} // anonymous namespace

B3::ClientPhase validateFrameAddresses(Vector<FrameAddressViolation>& violations, BitVector&& originsOfSuspensions, BitVector&& originsOfParametersWithoutObjects)
{
    return {
        B3::ClientPhase::Point::BeforeLowerToAir,
        "validateFrameAddresses"_s,
        createSharedTask<bool(B3::Procedure&)>([&violations, originsOfSuspensions = WTF::move(originsOfSuspensions), originsOfParametersWithoutObjects = WTF::move(originsOfParametersWithoutObjects)](B3::Procedure& proc) {
            FrameAddressValidation(proc, violations, originsOfSuspensions, originsOfParametersWithoutObjects).run();
            return false;
        })
    };
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
