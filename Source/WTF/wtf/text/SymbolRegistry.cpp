/*
 * Copyright (C) 2015 Yusuke Suzuki <utatane.tea@gmail.com>.
 * Copyright (C) 2022 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include <wtf/text/SymbolRegistry.h>

#include <wtf/Lock.h>
#include <wtf/text/SymbolImpl.h>

namespace WTF {

// SPEC-ungil §H (closes vmstate Dev 8): every access to m_table — symbolForKey,
// remove, and the destructor walk — runs under one lock. The lock belongs to
// the §LK.8 destructor-leaf class: it may be acquired UNDER MSPL/BVL/heap-9b
// holds (an in-lock sweep destroying the last reference to a
// RegisteredSymbolImpl reaches remove() through ~StringImpl's registered-symbol
// arm, on any thread), which is sound because every holder only hash-walks,
// list-splices, and fastMallocs — it acquires no other lock and never waits
// (vmstate I7 extended). Symbol.for traffic is orders of magnitude below
// atomization traffic, so a plain leaf lock — not a sharded/concurrent
// registry — is the chartered design; that non-goal carries an explicit
// bench-evidence reopen condition.
//
// GRANULARITY NOTE: §H specs the lock as a per-registry member (`Lock m_lock`
// on SymbolRegistry), but SymbolRegistry.h is not in this slice's owned files,
// so until the header gains the member the lock is a single file-static —
// serializing across ALL registries (public + private, all VMs) instead of
// per-table. That is strictly MORE serialization (every §H-required ordering
// still holds), preserves the destructor-leaf rank argument unchanged (the
// shared lock is still a leaf: same holder budget, no nesting under itself —
// symbolForKey/remove/the walk never call back into registry code under the
// lock, see the per-section comments), and at Symbol.for traffic levels is
// within the same bench-evidence reopen condition as the chartered
// non-sharding. NOTE (GIL-removal round 5): this deviation is now RECORDED as
// INTEGRATE-ungil.md supersession-ledger row 10 (R5-4), and the flag-off
// Symbol.for/sweep-path lock cost is on the §B.5 flag-off bench adjudication
// list. Moving to the spec'd per-registry member is NOT the mechanical
// follow-up an earlier draft of this note claimed: a member m_lock is
// destroyed right after ~SymbolRegistry's body, re-opening a destroyed-lock
// window for a straggling ~StringImpl-driven remove() that loaded its
// back-pointer before the destructor walk's clear — the file-static's
// outliving-the-registry property (lifecycle paragraph below) is
// load-bearing, so any move to a member lock must also re-solve that
// ordering.
//
// Lifecycle: the per-VM registries are destroyed in ~VM strictly after every
// spawned thread has exited (SPEC-ungil U16), so the destructor cannot race a
// live symbolForKey; the walk still takes the lock so that any straggling
// ~StringImpl-driven remove() (a terminal deref on another thread of a symbol
// already off the strong path) is ordered against it. A file-static lock
// trivially outlives every registry, so the destructor-vs-remove ordering
// never uses a destroyed lock.
static Lock s_symbolRegistryLock;

SymbolRegistry::SymbolRegistry(Type type)
    : m_symbolType(type)
{
}

SymbolRegistry::~SymbolRegistry()
{
    // §H destructor walk: clear each registered symbol's back-pointer under
    // the lock. The table's strong references are released only when m_table
    // itself is destroyed, after this body returns — i.e. OUTSIDE the lock —
    // so the ~StringImpl runs it triggers see a null symbolRegistry() and
    // never re-enter remove() (no self-deadlock, no use of a destroyed lock).
    Locker locker { s_symbolRegistryLock };
    for (auto& key : m_table)
        SUPPRESS_UNCOUNTED_ARG downcast<SymbolImpl>(key.get())->asRegisteredSymbolImpl()->clearSymbolRegistry();
}

Ref<RegisteredSymbolImpl> SymbolRegistry::symbolForKey(const String& rep)
{
    // The whole lookup-or-create runs under the registry lock: two racing Symbol.for(key)
    // calls must funnel to ONE RegisteredSymbolImpl. Creating the symbol under
    // the lock stays within the §LK.8 holder budget (fastMalloc allocation
    // only; RegisteredSymbolImpl::create takes no lock). The table holds a
    // strong reference (see StringImpl::derefSharedZero's symbol arm), so an
    // entry found here can never be mid-destruction.
    Locker locker { s_symbolRegistryLock };

    auto addResult = m_table.add(rep.impl());
    if (!addResult.isNewEntry)
        return *downcast<SymbolImpl>(addResult.iterator->get())->asRegisteredSymbolImpl();

    RefPtr<RegisteredSymbolImpl> symbol;
    if (m_symbolType == Type::PrivateSymbol)
        symbol = RegisteredSymbolImpl::createPrivate(*rep.impl(), *this);
    else
        symbol = RegisteredSymbolImpl::create(*rep.impl(), *this);

    // This overwrite releases the table's reference on the plain key string;
    // the caller's `rep` still holds one, so no destructor can run under the
    // lock here.
    *addResult.iterator = symbol;
    return symbol.releaseNonNull();
}

// When removing a registered symbol from the table, we know it's already the one in the table, so no need for a string equality check.
struct SymbolRegistryTableRemovalHashTranslator {
    static unsigned hash(const RegisteredSymbolImpl* key) { return key->hash(); }
    static bool NODELETE equal(const RefPtr<StringImpl>& a, const RegisteredSymbolImpl* b) { return a == b; }
};

void SymbolRegistry::remove(RegisteredSymbolImpl& uid)
{
    // §H: callable from ~StringImpl's registered-symbol arm on ANY thread,
    // including from an in-lock sweep (§LK.8 makes that nesting legal). The
    // dying symbol is already off the strong path (its refcount transition is
    // what brought us here), so the matching table RefPtr removed below points
    // at an object whose destructor is the caller; pointer-identity removal
    // (no string-equality probe of the destructing key) keeps that shape
    // exactly as landed.
    Locker locker { s_symbolRegistryLock };
    ASSERT(uid.symbolRegistry() == this);
    auto iterator = m_table.find<SymbolRegistryTableRemovalHashTranslator>(&uid);
    ASSERT_WITH_MESSAGE(iterator != m_table.end(), "The string being removed is registered in the string table of another thread!");
    m_table.remove(iterator);
}

}
