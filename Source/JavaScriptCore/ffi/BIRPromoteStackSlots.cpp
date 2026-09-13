/*
 * Copyright (C) 2026 Anthropic PBC. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "BIRPromoteStackSlots.h"

#if USE(BUN_JSC_ADDITIONS) && ENABLE(B3_JIT)

#include "AirStackSlot.h"
#include "B3BasicBlockInlines.h"
#include "B3Const32Value.h"
#include "B3FixSSA.h"
#include "B3InsertionSetInlines.h"
#include "B3MemoryValue.h"
#include "B3Procedure.h"
#include "B3SlotBaseValue.h"
#include "B3ValueInlines.h"
#include "B3Variable.h"
#include "B3VariableValue.h"
#include <wtf/HashMap.h>

namespace JSC { namespace FFI {

using namespace B3;

namespace {

// How every access to a promotable slot reads or writes it. A slot is promoted only when all of
// its accesses agree, so it can become one Variable of `type`.
struct SlotAccess {
    enum class Kind : uint8_t { Unknown, Full, Byte, Half, Escaped } kind { Kind::Unknown };
    B3::Type type { Void };
    Variable* variable { nullptr };
};

SlotAccess::Kind accessKind(B3::Opcode opcode)
{
    switch (opcode) {
    case Load:
    case Store:
        return SlotAccess::Kind::Full;
    case Load8Z:
    case Load8S:
    case Store8:
        return SlotAccess::Kind::Byte;
    case Load16Z:
    case Load16S:
    case Store16:
        return SlotAccess::Kind::Half;
    default:
        return SlotAccess::Kind::Escaped;
    }
}

} // anonymous namespace

bool promoteStackSlots(Procedure& proc)
{
    // The address of a C local that was passed to an inlined callee reaches its loads and stores
    // through the callee's parameter Variable. SSA conversion removes that hop.
    proc.resetReachability();
    fixSSA(proc);

    // Every value that is exactly the address of a slot: its SlotBase, and Identities of those.
    UncheckedKeyHashMap<Value*, Air::StackSlot*> addressOf;
    bool changed = true;
    while (changed) {
        changed = false;
        for (BasicBlock* block : proc) {
            for (Value* value : *block) {
                if (addressOf.contains(value))
                    continue;
                Air::StackSlot* slot = nullptr;
                if (auto* slotBase = value->as<SlotBaseValue>())
                    slot = slotBase->slot();
                else if (value->opcode() == Identity) {
                    auto iterator = addressOf.find(value->child(0));
                    if (iterator != addressOf.end())
                        slot = iterator->value;
                }
                if (slot) {
                    addressOf.add(value, slot);
                    changed = true;
                }
            }
        }
    }
    if (addressOf.isEmpty())
        return false;

    UncheckedKeyHashMap<Air::StackSlot*, SlotAccess> accesses;
    auto escape = [&](Air::StackSlot* slot) {
        accesses.ensure(slot, [] { return SlotAccess(); }).iterator->value.kind = SlotAccess::Kind::Escaped;
    };

    for (BasicBlock* block : proc) {
        for (Value* value : *block) {
            if (value->opcode() == Identity && addressOf.contains(value))
                continue;
            auto* memory = value->as<MemoryValue>();
            for (unsigned i = 0; i < value->numChildren(); ++i) {
                auto iterator = addressOf.find(value->child(i));
                if (iterator == addressOf.end())
                    continue;
                Air::StackSlot* slot = iterator->value;
                SlotAccess::Kind kind = memory ? accessKind(memory->opcode()) : SlotAccess::Kind::Escaped;
                bool isPointerOperand = memory && value->child(i) == memory->lastChild() && (memory->isLoad() || i);
                if (kind == SlotAccess::Kind::Escaped || !isPointerOperand || memory->offset() || memory->hasFence()) {
                    escape(slot);
                    continue;
                }
                B3::Type type = memory->isLoad() ? memory->type() : memory->child(0)->type();
                if (kind == SlotAccess::Kind::Full && sizeofType(type) > slot->byteSize()) {
                    escape(slot);
                    continue;
                }
                SlotAccess& access = accesses.ensure(slot, [] { return SlotAccess(); }).iterator->value;
                if (access.kind == SlotAccess::Kind::Unknown) {
                    access.kind = kind;
                    access.type = type;
                } else if (access.kind != kind || access.type != type)
                    access.kind = SlotAccess::Kind::Escaped;
            }
        }
    }

    bool promotedAny = false;
    for (auto& entry : accesses) {
        SlotAccess& access = entry.value;
        if (access.kind == SlotAccess::Kind::Escaped || access.kind == SlotAccess::Kind::Unknown)
            continue;
        access.variable = proc.addVariable(access.type);
        promotedAny = true;
    }
    if (!promotedAny)
        return false;

    InsertionSet insertionSet(proc);
    for (BasicBlock* block : proc) {
        for (unsigned index = 0; index < block->size(); ++index) {
            auto* memory = block->at(index)->as<MemoryValue>();
            if (!memory)
                continue;
            auto address = addressOf.find(memory->lastChild());
            if (address == addressOf.end())
                continue;
            const SlotAccess& access = accesses.find(address->value)->value;
            if (!access.variable)
                continue;
            Origin origin = memory->origin();
            if (memory->isStore()) {
                insertionSet.insert<VariableValue>(index, B3::Set, origin, access.variable, memory->child(0));
                memory->replaceWithNop();
                continue;
            }
            Value* loaded = insertionSet.insert<VariableValue>(index, B3::Get, origin, access.variable);
            switch (memory->opcode()) {
            case Load8Z:
                loaded = insertionSet.insert<Value>(index, BitAnd, origin, loaded, insertionSet.insert<Const32Value>(index, origin, 0xff));
                break;
            case Load8S:
                loaded = insertionSet.insert<Value>(index, SExt8, origin, loaded);
                break;
            case Load16Z:
                loaded = insertionSet.insert<Value>(index, BitAnd, origin, loaded, insertionSet.insert<Const32Value>(index, origin, 0xffff));
                break;
            case Load16S:
                loaded = insertionSet.insert<Value>(index, SExt16, origin, loaded);
                break;
            default:
                break;
            }
            memory->replaceWithIdentity(loaded);
        }
        insertionSet.execute(block);
    }
    return true;
}

} } // namespace JSC::FFI

#endif // USE(BUN_JSC_ADDITIONS) && ENABLE(B3_JIT)
