/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3BasicBlock.h"
#include "B3MovedLoadCheck.h"
#include "B3Value.h"
#include <optional>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/IndexSet.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace B3 {

class Procedure;

class RegionCloner {
    WTF_MAKE_NONCOPYABLE(RegionCloner);
public:
    using BlockMap = UncheckedKeyHashMap<BasicBlock*, BasicBlock*>;
    using ValueMap = UncheckedKeyHashMap<Value*, Value*>;
    enum class PhiTreatment : uint8_t { Demote, KeepIfJumpFed };
    enum class ValueOwners : uint8_t { MayBeStale, AreCurrent };

    JS_EXPORT_PRIVATE explicit RegionCloner(Procedure&);

    JS_EXPORT_PRIVATE void addBlock(BasicBlock*);
    void withdrawBlock(BasicBlock* block)
    {
        RELEASE_ASSERT(m_step == Step::Declaring);
        m_sources.remove(block);
        m_declaredBlocks.remove(block);
    }
    bool contains(BasicBlock* block) const { return m_sources.contains(block); }
    bool isEmpty() const { return m_sources.isEmpty(); }

    JS_EXPORT_PRIVATE void demoteEscapingValues(PhiTreatment = PhiTreatment::Demote, ValueOwners = ValueOwners::MayBeStale);

    void removeBlock(BasicBlock* block)
    {
        m_sources.remove(block);
        m_didChange = true;
    }
    JS_EXPORT_PRIVATE Value* appendClone(Value* original, BasicBlock* destination, ValueMap&);
    JS_EXPORT_PRIVATE void substitute(ValueMap&, Value* original, Value* replacement);
    JS_EXPORT_PRIVATE void substituteOnOnePath(ValueMap&, Value* original, Value* replacement);
    JS_EXPORT_PRIVATE void substitutePhisFedBy(BasicBlock* predecessor, BasicBlock* block, ValueMap&);
    JS_EXPORT_PRIVATE BlockMap cloneBlocks(std::span<BasicBlock* const>);

    // No other block names a value of a block that was declared: what leaves such a block goes through a variable.
    JS_EXPORT_PRIVATE bool valuesStayInDeclaredBlocks() const;
    JS_EXPORT_PRIVATE bool finish();

private:
    enum class Step : uint8_t { Declaring, Copying, Finished };

    Procedure& m_proc;
    Step m_step { Step::Declaring };
    PhiTreatment m_phiTreatment { PhiTreatment::Demote };
    bool m_didChange { false };
    IndexSet<BasicBlock*> m_sources;
    IndexSet<BasicBlock*> m_declaredBlocks;
    IndexSet<Value*> m_valuesOfOnePath;
    std::optional<MovedLoadLog::OriginalsKeepTheirMeaning> m_originalsKeepTheirMeaning;
};

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
