/*
 * Copyright (C) 2026 Jordan Hall. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "SharedStructBlock.h"

#include <wtf/Condition.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <new>
#include <wtf/NeverDestroyed.h>
#include <wtf/Seconds.h>
#include <wtf/Threading.h>

namespace JSC {

namespace {

std::atomic<size_t> liveBlocks { 0 };
std::atomic<size_t> liveStrings { 0 };

// The blocks that the cycle collector has yet to look at. A block is in the table exactly when bufferedBit is set in
// its m_state, and both change only under the lock. The table does not own a reference: a block leaves it before it
// is destroyed (see SharedStructBlock::release()).
struct CandidateTable {
    Lock lock;
    UncheckedKeyHashSet<SharedStructBlock*> blocks;
};

CandidateTable& candidateTable()
{
    static NeverDestroyed<CandidateTable> table;
    return table.get();
}

// One pass at a time, process-wide.
Lock collectorLock;

void candidateAdded(size_t candidateCount);

} // namespace

size_t liveSharedStructBlockCount() { return liveBlocks.load(std::memory_order_relaxed); }
size_t liveSharedStringCount() { return liveStrings.load(std::memory_order_relaxed); }

WTF_MAKE_TZONE_ALLOCATED_IMPL(SharedString);
WTF_MAKE_TZONE_ALLOCATED_IMPL(SharedSyncHandle);
WTF_MAKE_TZONE_ALLOCATED_IMPL(SharedSymbol);
WTF_MAKE_TZONE_ALLOCATED_IMPL(SharedStructShape);
WTF_MAKE_TZONE_ALLOCATED_IMPL(SharedStructBlock);

Ref<SharedString> SharedString::create(StringView view)
{
    Vector<char16_t> characters;
    characters.reserveInitialCapacity(view.length());
    for (unsigned i = 0; i < view.length(); ++i)
        characters.append(view[i]);
    liveStrings.fetch_add(1, std::memory_order_relaxed);
    return adoptRef(*new SharedString(WTF::move(characters)));
}

SharedString::~SharedString()
{
    liveStrings.fetch_sub(1, std::memory_order_relaxed);
}

// ------------------------------ SharedSymbol ------------------------------

static Vector<char16_t> copyCharacters(StringView view)
{
    Vector<char16_t> characters;
    characters.reserveInitialCapacity(view.length());
    for (unsigned i = 0; i < view.length(); ++i)
        characters.append(view[i]);
    return characters;
}

SharedSymbol::~SharedSymbol() = default;

static Lock uniqueSymbolLock;

static UncheckedKeyHashMap<const void*, SharedSymbol*>& uniqueSymbols()
{
    static NeverDestroyed<UncheckedKeyHashMap<const void*, SharedSymbol*>> table;
    return table.get();
}

RefPtr<SharedSymbol> SharedSymbol::findUnique(const void* symbolImpl)
{
    Locker locker { uniqueSymbolLock };
    auto iterator = uniqueSymbols().find(symbolImpl);
    return iterator == uniqueSymbols().end() ? nullptr : RefPtr<SharedSymbol>(iterator->value);
}

void SharedSymbol::bind(SharedSymbol& shared, const void* vm, const void* symbolImpl)
{
    Locker locker { uniqueSymbolLock };
    // The table points at the symbol for good.
    shared.ref();
    uniqueSymbols().add(symbolImpl, &shared);
    shared.m_bindings.append({ vm, symbolImpl });
}

const void* SharedSymbol::implIn(const void* vm) const
{
    Locker locker { uniqueSymbolLock };
    for (const auto& binding : m_bindings) {
        if (binding.first == vm)
            return binding.second;
    }
    return nullptr;
}

Ref<SharedSymbol> SharedSymbol::createUnique(std::optional<StringView> description)
{
    return adoptRef(*new SharedSymbol(Kind::Unique, !!description, description ? copyCharacters(*description) : Vector<char16_t>()));
}

static Ref<SharedSymbol> internSharedSymbol(SharedSymbol::Kind kind, StringView name, Lock& lock, UncheckedKeyHashMap<String, Ref<SharedSymbol>>& table, Ref<SharedSymbol> (*create)(StringView))
{
    String key = name.toString();
    Locker locker { lock };
    auto iterator = table.find(key);
    if (iterator != table.end())
        return iterator->value.copyRef();
    Ref symbol = create(name);
    ASSERT_UNUSED(kind, symbol->kind() == kind);
    table.add(key.isolatedCopy(), symbol.copyRef());
    return symbol;
}

Ref<SharedSymbol> SharedSymbol::registered(StringView key)
{
    static Lock lock;
    static NeverDestroyed<UncheckedKeyHashMap<String, Ref<SharedSymbol>>> table;
    return internSharedSymbol(Kind::Registered, key, lock, table.get(), [](StringView name) {
        return adoptRef(*new SharedSymbol(Kind::Registered, true, copyCharacters(name)));
    });
}

Ref<SharedSymbol> SharedSymbol::wellKnown(StringView name)
{
    static Lock lock;
    static NeverDestroyed<UncheckedKeyHashMap<String, Ref<SharedSymbol>>> table;
    return internSharedSymbol(Kind::WellKnown, name, lock, table.get(), [](StringView symbolName) {
        return adoptRef(*new SharedSymbol(Kind::WellKnown, true, copyCharacters(symbolName)));
    });
}

Ref<ArrayBuffer> SharedSyncHandle::createBuffer() const
{
    ArrayBufferContents copy;
    m_contents.shareWith(copy);
    return ArrayBuffer::create(WTF::move(copy));
}

// ------------------------------ SharedStructShape ------------------------------

static Lock sharedStructShapeTableLock;

// The canonical numeric string of an array index (0 to 2**32 - 2), which names an indexed property.
static std::optional<uint32_t> arrayIndexOfName(std::span<const char16_t> name)
{
    if (name.empty() || name.size() > 10 || (name[0] == '0' && name.size() > 1))
        return std::nullopt;
    uint64_t value = 0;
    for (char16_t character : name) {
        if (character < '0' || character > '9')
            return std::nullopt;
        value = value * 10 + (character - '0');
    }
    if (value >= 0xffffffffull)
        return std::nullopt;
    return static_cast<uint32_t>(value);
}

SharedStructShape::SharedStructShape(const String& encoding)
{
    StringView view(encoding);
    unsigned position = 0;
    while (position < view.length()) {
        char16_t kind = view[position++];
        unsigned length = 0;
        while (position < view.length() && view[position] != ':')
            length = length * 10 + (view[position++] - '0');
        ++position;
        StringView payload = view.substring(position, length);
        position += length;

        Field field;
        if (kind == 'y') {
            uintptr_t address = 0;
            for (unsigned i = 0; i < payload.length(); ++i)
                address = address * 10 + (payload[i] - '0');
            field.symbol = RefPtr<SharedSymbol>(reinterpret_cast<SharedSymbol*>(address));
        } else
            field.name = copyCharacters(payload);

        unsigned slot = m_fields.size();
        if (!field.symbol) {
            if (auto index = arrayIndexOfName(field.name.span()))
                m_arrayIndexFields.append({ *index, slot });
            else
                m_namedSlots.append(slot);
        } else
            m_namedSlots.append(slot);
        m_fields.append(WTF::move(field));
    }
    std::sort(m_arrayIndexFields.begin(), m_arrayIndexFields.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
}

Ref<SharedStructShape> SharedStructShape::intern(const String& names)
{ // `names` is the encoding of the fields.
    static NeverDestroyed<UncheckedKeyHashMap<String, Ref<SharedStructShape>>> table;
    Locker locker { sharedStructShapeTableLock };
    auto iterator = table->find(names);
    if (iterator != table->end())
        return iterator->value.copyRef();
    Ref shape = adoptRef(*new SharedStructShape(names));
    // The key is a copy that no other thread shares, because the table outlives every VM.
    table->add(names.isolatedCopy(), shape.copyRef());
    return shape;
}

std::optional<unsigned> SharedStructShape::indexOf(StringView name) const
{
    for (unsigned slot : m_namedSlots) {
        if (!m_fields[slot].symbol && name == StringView(m_fields[slot].name.span()))
            return slot;
    }
    return std::nullopt;
}

std::optional<unsigned> SharedStructShape::slotOfArrayIndex(uint32_t index) const
{
    for (const auto& field : m_arrayIndexFields) {
        if (field.first == index)
            return field.second;
    }
    return std::nullopt;
}

// ------------------------------ SharedSlotValue ------------------------------

SharedSlotValue SharedSlotValue::structBlock(Ref<SharedStructBlock>&& block)
{
    return SharedSlotValue(box(TagStruct, &block.leakRef()));
}

SharedStructBlock& SharedSlotValue::blockValue() const
{
    return *static_cast<SharedStructBlock*>(pointer());
}

uint64_t SharedSlotValue::box(uint64_t tag, void* pointer)
{
    uint64_t address = reinterpret_cast<uintptr_t>(pointer);
    RELEASE_ASSERT(address <= datumMask);
    return qnan | (tag << tagShift) | address;
}

SharedSlotValue::Kind SharedSlotValue::kind() const
{
    switch (tagOf(m_bits)) {
    case 0:
        return Kind::Number;
    case TagSpecial:
        switch (m_bits & datumMask) {
        case 0:
            return Kind::Undefined;
        case 1:
            return Kind::Null;
        default:
            return Kind::Boolean;
        }
    case TagString:
        return Kind::String;
    case TagBigInt:
        return Kind::BigInt;
    case TagStruct:
        return Kind::Struct;
    case TagSymbol:
        return Kind::Symbol;
    default:
        return Kind::Sync;
    }
}

// Dropping the last reference to a block drops the references its slots hold, so a chain of blocks (a linked list
// of shared structs) of any length would release itself recursively and overflow the stack. The release of a block
// that happens while another one is being released is queued, and the outermost release drains the queue in a loop.
static thread_local Vector<SharedStructBlock*>* pendingBlockReleases;

static void releaseBlockReference(SharedStructBlock* block)
{
    if (pendingBlockReleases) {
        pendingBlockReleases->append(block);
        return;
    }
    Vector<SharedStructBlock*> pending;
    pendingBlockReleases = &pending;
    block->deref();
    while (!pending.isEmpty()) {
        SharedStructBlock* next = pending.takeLast();
        next->deref();
    }
    pendingBlockReleases = nullptr;
}

void SharedSlotValue::releaseReference(uint64_t bits)
{
    void* address = reinterpret_cast<void*>(bits & datumMask);
    switch (tagOf(bits)) {
    case TagString:
    case TagBigInt:
        static_cast<SharedString*>(address)->deref();
        break;
    case TagStruct:
        releaseBlockReference(static_cast<SharedStructBlock*>(address));
        break;
    case TagSync:
        static_cast<SharedSyncHandle*>(address)->deref();
        break;
    case TagSymbol:
        static_cast<SharedSymbol*>(address)->deref();
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

SharedSlotValue SharedSlotValue::retain(uint64_t bits)
{
    void* address = reinterpret_cast<void*>(bits & datumMask);
    switch (tagOf(bits)) {
    case TagString:
    case TagBigInt:
        static_cast<SharedString*>(address)->ref();
        break;
    case TagStruct:
        static_cast<SharedStructBlock*>(address)->ref();
        break;
    case TagSync:
        static_cast<SharedSyncHandle*>(address)->ref();
        break;
    case TagSymbol:
        static_cast<SharedSymbol*>(address)->ref();
        break;
    default:
        break;
    }
    return SharedSlotValue(bits);
}

bool SharedSlotValue::holds(uint64_t current, const SharedSlotValue& expected)
{
    uint64_t tag = tagOf(expected.m_bits);
    if (tag == TagString || tag == TagBigInt) {
        return tagOf(current) == tag
            && static_cast<SharedString*>(reinterpret_cast<void*>(current & datumMask))->equals(expected.stringValue());
    }
    if (tag == TagSync) {
        if (tagOf(current) != TagSync)
            return false;
        auto& currentHandle = *static_cast<SharedSyncHandle*>(reinterpret_cast<void*>(current & datumMask));
        return currentHandle.kind() == expected.syncValue().kind() && currentHandle.identity() == expected.syncValue().identity();
    }
    return current == expected.m_bits;
}

// ------------------------------ SharedStructBlock ------------------------------

// Counts an operation on the block while it runs: `unit` says whether it is a reader or a writer, and the epoch
// advances once. The collector reads the word before and after it looks at the block (see below).
class SharedStructBlock::ActivityGuard {
public:
    ActivityGuard(SharedStructBlock& block, uint64_t unit)
        : m_activity(block.m_activity)
        , m_unit(unit)
    {
        m_activity.fetch_add(unit + epochUnit, std::memory_order_seq_cst);
    }
    ~ActivityGuard() { m_activity.fetch_sub(m_unit, std::memory_order_seq_cst); }

private:
    std::atomic<uint64_t>& m_activity;
    uint64_t m_unit;
};

Ref<SharedStructBlock> SharedStructBlock::create(Ref<SharedStructShape>&& shape)
{
    unsigned count = shape->fieldCount();
    return adoptRef(*new SharedStructBlock(RefPtr<SharedStructShape>(WTF::move(shape)), count, std::make_unique<std::atomic<uint64_t>[]>(count)));
}

RefPtr<SharedStructBlock> SharedStructBlock::createArray(unsigned length)
{
    std::unique_ptr<std::atomic<uint64_t>[]> slots(new (std::nothrow) std::atomic<uint64_t>[length]);
    if (!slots)
        return nullptr;
    return adoptRef(*new SharedStructBlock(nullptr, length, WTF::move(slots)));
}

SharedStructBlock::SharedStructBlock(RefPtr<SharedStructShape>&& shape, unsigned count, std::unique_ptr<std::atomic<uint64_t>[]>&& slots)
    : m_shape(WTF::move(shape))
    , m_count(count)
    , m_slots(WTF::move(slots))
{
    uint64_t undefined = SharedSlotValue::undefined().bits();
    for (unsigned i = 0; i < m_count; ++i)
        m_slots[i].store(undefined, std::memory_order_relaxed);
    liveBlocks.fetch_add(1, std::memory_order_relaxed);
}

SharedStructBlock::~SharedStructBlock()
{
    liveBlocks.fetch_sub(1, std::memory_order_relaxed);
    // No reader exists any more, and each slot owns its reference.
    for (unsigned i = 0; i < m_count; ++i)
        SharedSlotValue::releaseBits(m_slots[i].load(std::memory_order_relaxed));
    Vector<uint64_t> garbage;
    {
        Locker locker { m_garbageLock };
        garbage = std::exchange(m_garbage, Vector<uint64_t>());
    }
    for (uint64_t bits : garbage)
        SharedSlotValue::releaseBits(bits);
}

// ---- reference counting

void SharedStructBlock::release(bool mayBecomeCandidate) const
{
    // A reference that is not the last one leaves the block alive, and if it holds blocks it may now be reachable
    // only from itself. The block is recorded before the count drops, while this thread's reference still keeps it
    // alive; recording it afterwards could name a block that another thread has already destroyed. If the count is
    // 1 this reference is the last (nothing else can take one from a block that no slot holds), so there is
    // nothing to record.
    if (mayBecomeCandidate && m_hasBlockSlots.load(std::memory_order_relaxed) && (m_state.load(std::memory_order_relaxed) & countMask) > 1)
        becomeCandidate();
    uint32_t previous = m_state.fetch_sub(1, std::memory_order_acq_rel);
    if ((previous & countMask) != 1)
        return;
    // The last reference. The collector takes references only to blocks it finds in the table, with
    // tryRefForCollector() and under the table's lock, which fails once the count is 0; so leaving the table under
    // the lock is enough to keep the collector away from the block while it is destroyed.
    if (previous & bufferedBit)
        leaveCandidates();
    delete this;
}

void SharedStructBlock::becomeCandidate() const
{
    if (m_state.load(std::memory_order_relaxed) & bufferedBit)
        return;
    CandidateTable& table = candidateTable();
    size_t candidateCount;
    {
        Locker locker { table.lock };
        if (m_state.fetch_or(bufferedBit, std::memory_order_acq_rel) & bufferedBit)
            return;
        table.blocks.add(const_cast<SharedStructBlock*>(this));
        candidateCount = table.blocks.size();
    }
    candidateAdded(candidateCount);
}

void SharedStructBlock::leaveCandidates() const
{
    CandidateTable& table = candidateTable();
    Locker locker { table.lock };
    table.blocks.remove(const_cast<SharedStructBlock*>(this));
}

bool SharedStructBlock::tryRefForCollector() const
{
    uint32_t state = m_state.load(std::memory_order_relaxed);
    do {
        if (!(state & countMask))
            return false;
    } while (!m_state.compare_exchange_weak(state, state + 1, std::memory_order_acq_rel, std::memory_order_relaxed));
    return true;
}

// ---- slots

void SharedStructBlock::noteStored(const SharedSlotValue& value)
{
    if (value.kind() == SharedSlotValue::Kind::Struct)
        m_hasBlockSlots.store(true, std::memory_order_seq_cst);
}

void SharedStructBlock::retire(uint64_t old)
{
    if (!SharedSlotValue::ownsReference(old))
        return;
    Vector<uint64_t> batch;
    {
        Locker locker { m_garbageLock };
        m_garbage.append(old);
        if (m_activity.load(std::memory_order_seq_cst) & readersMask)
            return;
        batch = std::exchange(m_garbage, Vector<uint64_t>());
    }
    for (uint64_t bits : batch)
        SharedSlotValue::releaseBits(bits);
}

SharedSlotValue SharedStructBlock::load(unsigned index)
{
    RELEASE_ASSERT(index < fieldCount());
    ActivityGuard guard(*this, readerUnit);
    return SharedSlotValue::retain(m_slots[index].load(std::memory_order_seq_cst));
}

void SharedStructBlock::store(unsigned index, SharedSlotValue&& value)
{
    RELEASE_ASSERT(index < fieldCount());
    noteStored(value);
    uint64_t old;
    {
        ActivityGuard guard(*this, writerUnit);
        old = m_slots[index].exchange(value.leakBits(), std::memory_order_seq_cst);
    }
    retire(old);
}

SharedSlotValue SharedStructBlock::exchange(unsigned index, SharedSlotValue&& value)
{
    RELEASE_ASSERT(index < fieldCount());
    noteStored(value);
    uint64_t old;
    SharedSlotValue previous;
    {
        ActivityGuard guard(*this, writerUnit);
        old = m_slots[index].exchange(value.leakBits(), std::memory_order_seq_cst);
        // The swap handed this thread the slot's reference, which retire() has not released yet.
        previous = SharedSlotValue::retain(old);
    }
    retire(old);
    return previous;
}

SharedSlotValue SharedStructBlock::compareExchange(unsigned index, const SharedSlotValue* expected, SharedSlotValue&& desired)
{
    RELEASE_ASSERT(index < fieldCount());
    noteStored(desired);
    auto& slot = m_slots[index];
    SharedSlotValue seen;
    uint64_t replaced = 0;
    bool didReplace = false;
    {
        ActivityGuard guard(*this, readerUnit);
        while (true) {
            uint64_t current = slot.load(std::memory_order_seq_cst);
            if (!expected || !SharedSlotValue::holds(current, *expected)) {
                seen = SharedSlotValue::retain(current);
                break;
            }
            // Equal content can sit behind different words, so swap on the exact word that was observed and
            // look again if the slot changed in between.
            uint64_t observed = current;
            if (slot.compare_exchange_strong(observed, desired.bits(), std::memory_order_seq_cst, std::memory_order_seq_cst)) {
                desired.leakBits();
                // The swap made `current` ours, and retire() has not run yet.
                seen = SharedSlotValue::retain(current);
                replaced = current;
                didReplace = true;
                break;
            }
        }
    }
    if (didReplace)
        retire(replaced);
    return seen;
}

// ------------------------------ The cycle collector ------------------------------
//
// What it is for. A block holds a counted reference to every block in its slots, so blocks that hold each other
// would never reach a count of 0. Nothing here involves a VM's garbage collector: a JS wrapper holds one reference
// to its block until the wrapper itself is finalized, and that is the only way JS code keeps a block alive.
//
// Candidates. Every deref() that leaves a block with a non-zero count, on a block that has ever had a block stored
// in it, records the block in the candidate table (before decrementing, see release()). That is the Bacon-Rajan
// rule: a cycle can only become garbage when the last reference from outside it goes away, and that reference
// belongs to some block of the cycle or of a chain that leads to it, or is a wrapper of one; each such drop is a
// decrement to a non-zero count (the cycle holds the rest) on a block that holds blocks. When the table passes a
// threshold, or a little while after the first entry, a background thread runs a pass; collectSharedStructCycles()
// runs one on demand. Passes are serialized by collectorLock.
//
// A pass.
//  1. Take every candidate for which the collector can add a reference (blocks that are being destroyed are
//     skipped; their last deref() removes them), and clear their candidate bit. Each of these is a root.
//  2. Discover the blocks reachable from the roots through block-typed slots, loading each slot like any reader
//     does (so the target is kept alive by the reference that the load takes). The pass keeps exactly one reference,
//     the "pin", for each block it found and drops the extra ones. Retire lists are flushed on the way (a retired
//     reference to a block is a reference that no slot accounts for).
//  3. Read every block's activity word (m_activity); a block with an operation in flight is "busy".
//  4. Read the slots of every block without taking a reference (raw loads) and build the edges, whose targets are
//     looked up by address among the pinned blocks. A target that is not among them makes the block "unstable".
//  5. Count for every block the edges that arrive from inside the discovered set (its internal count), and compare
//     with its reference count: expected = 1 (the pin) + internal count. A block whose count is higher has a
//     reference that this pass cannot account for (a wrapper, a handle, a slot of a block that was not discovered,
//     a retired reference not yet released, a thread that is reading it): it is "external". A block whose count is
//     lower, a busy block and an unstable block are "transient": the snapshot is not trustworthy.
//  6. Everything reachable from a transient or external block is live. What is left, G, is garbage: its blocks'
//     references all come from other blocks of G.
//  7. Read the activity words of the blocks of G again. If any differs from step 3, give up on this pass.
//  8. Empty the slots of the blocks of G, and release what they held; then drop the pins, which destroys the blocks
//     of G (their counts are their pins alone). References from G to a block outside of G are released as an
//     ordinary deref(), so that block may become a candidate in turn.
//  Roots that were found live because of a transient block are put back in the table, so that a later pass retries.
//
// Why G can be cleared. Let t1 be the moment step 3 ends for all blocks and t2 the moment step 7 starts. Every
// operation on a block (load, store, exchange, compareExchange) advances the block's epoch when it begins and counts
// itself in m_activity until it ends, and every one of them is a seq_cst operation on that word. A block of G has,
// at step 3, no operation in flight, and at step 7 the same word, so no operation on it began, or was still
// running, anywhere in [t1, t2]. Therefore (a) no slot of a block of G changed in that window, so the edges read in
// step 4 are the true edges throughout it, and (b) nothing took a new reference to a block through a slot of a
// block of G in it (taking a reference from a slot is only done by those operations, and a thread gets a reference
// to a block in no other way than by copying one that it or a peer already holds, or by loading it).
// Let nI(N, t) be the number of references to a block N of G at time t that do not come from a slot of a block
// of G and are not the pin. Step 5 read nI(N, b) == 0 at some b in [t1, t2], or N would be external. Suppose
// nI(N, t) >= 1 at some t in [t1, t2]. That reference was created by copying another reference to N (so
// nI(N) >= 1 before), by loading N from a slot of a block outside G (that slot is itself a reference counted in
// nI(N), and it is still there when the load takes its own reference, because a retired reference is released only
// when no reader is in flight), or by a load from a block of G (excluded by (b)); a reference to a block cannot be
// created in any other way. So nI(N) >= 1 held continuously from t1 to t, and in particular at b: a contradiction.
// Hence at t2 no thread or slot outside G holds a block of G, and none can gain one afterwards: the only way to
// reach a block of G is to hold a reference to a block of G first. So no thread can read or write a block of G,
// nobody can store a block of G anywhere, and G stays unreferenced; clearing its slots is invisible to every agent.
// References lost during the pass (a wrapper finalized, a slot overwritten) only lower counts: a count that has
// dropped below what the edges explain is transient (step 5), and an external reference that is gone is gone for
// good. The pass holds its own pin on each block it looks at, so none of them is destroyed during the pass, and it
// reads other threads' slots only through the load protocol (step 2) or as bare words that it compares by address
// only (step 4). Every doubt is conservative: a pass that sees anything it cannot explain reclaims less, never
// more. Passes are serialized, and a block that is being cleared is in no other pass.
//
// Cost. A load or a store pays two read-modify-writes on m_activity, as the load paid two on m_readers before, and
// never touches a lock that the collector holds. deref() of a block that holds blocks takes the candidate lock once
// per block between two passes.

void SharedStructBlock::flushGarbage()
{
    Vector<uint64_t> batch;
    {
        Locker locker { m_garbageLock };
        if (m_garbage.isEmpty() || (m_activity.load(std::memory_order_seq_cst) & readersMask))
            return;
        batch = std::exchange(m_garbage, Vector<uint64_t>());
    }
    for (uint64_t bits : batch)
        SharedSlotValue::releaseBits(bits);
}

SharedStructBlock* SharedStructBlock::blockOfWord(uint64_t word)
{
    if (SharedSlotValue::tagOf(word) != SharedSlotValue::TagStruct)
        return nullptr;
    return static_cast<SharedStructBlock*>(reinterpret_cast<void*>(word & SharedSlotValue::datumMask));
}

SharedStructBlock* SharedStructBlock::retainBlockAt(unsigned index)
{
    ActivityGuard guard(*this, readerUnit);
    SharedStructBlock* block = blockOfWord(m_slots[index].load(std::memory_order_seq_cst));
    if (block)
        block->ref();
    return block;
}

void SharedStructBlock::takeAllWords(Vector<uint64_t>& words)
{
    uint64_t undefined = SharedSlotValue::undefined().bits();
    for (unsigned i = 0; i < m_count; ++i) {
        uint64_t old = m_slots[i].exchange(undefined, std::memory_order_seq_cst);
        if (SharedSlotValue::ownsReference(old))
            words.append(old);
    }
    Locker locker { m_garbageLock };
    for (uint64_t bits : m_garbage)
        words.append(bits);
    m_garbage.clear();
}

void SharedStructBlock::releaseCollectedWord(uint64_t word, bool targetIsCollected)
{
    if (SharedStructBlock* block = blockOfWord(word)) {
        if (targetIsCollected)
            block->derefForCollector();
        else
            releaseBlockReference(block);
        return;
    }
    SharedSlotValue::releaseBits(word);
}

namespace {

struct CollectorNode {
    SharedStructBlock* block { nullptr };
    bool isRoot { false };
    bool busy { false };
    bool unstable { false };
    bool live { false };
    bool liveFromTransient { false };
    uint64_t activity { 0 };
    uint32_t internalCount { 0 };
    unsigned edgesBegin { 0 };
    unsigned edgesEnd { 0 };
};

// A pass never looks at more blocks than this; a block whose target does not fit is treated as unstable.
constexpr size_t maxCollectorNodes = 1 << 22;

} // namespace

SharedStructCollectorAccess::Result SharedStructCollectorAccess::collectOnce()
{
    Locker collectorLocker { collectorLock };
    Result result;

    Vector<CollectorNode> nodes;
    UncheckedKeyHashMap<SharedStructBlock*, unsigned> index;
    Vector<unsigned> edges;

    // 1. The roots.
    {
        CandidateTable& table = candidateTable();
        Locker locker { table.lock };
        Vector<SharedStructBlock*> taken;
        for (SharedStructBlock* block : table.blocks) {
            if (!block->tryRefForCollector())
                continue;
            block->clearBufferedBit();
            taken.append(block);
        }
        for (SharedStructBlock* block : taken) {
            table.blocks.remove(block);
            index.add(block, static_cast<unsigned>(nodes.size()));
            CollectorNode node;
            node.block = block;
            node.isRoot = true;
            nodes.append(node);
        }
    }
    if (nodes.isEmpty())
        return result;

    // 2. Discovery. nodes grows while it is walked.
    for (unsigned i = 0; i < nodes.size(); ++i) {
        SharedStructBlock* block = nodes[i].block;
        block->flushGarbage();
        if (!block->hasBlockSlots())
            continue;
        for (unsigned slot = 0; slot < block->fieldCount(); ++slot) {
            SharedStructBlock* child = block->retainBlockAt(slot);
            if (!child)
                continue;
            if (nodes.size() >= maxCollectorNodes || !index.add(child, static_cast<unsigned>(nodes.size())).isNewEntry) {
                child->derefForCollector();
                continue;
            }
            CollectorNode node;
            node.block = child;
            nodes.append(node);
        }
    }

    // 3. Activity.
    for (CollectorNode& node : nodes) {
        node.activity = node.block->activityWord();
        node.busy = SharedStructBlock::isInFlight(node.activity);
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // 4. Edges, from bare words.
    for (CollectorNode& node : nodes) {
        node.edgesBegin = static_cast<unsigned>(edges.size());
        SharedStructBlock* block = node.block;
        if (block->hasBlockSlots()) {
            for (unsigned slot = 0; slot < block->fieldCount(); ++slot) {
                SharedStructBlock* target = SharedStructBlock::blockOfWord(block->rawWord(slot));
                if (!target)
                    continue;
                auto found = index.find(target);
                if (found == index.end())
                    node.unstable = true;
                else
                    edges.append(found->value);
            }
        }
        node.edgesEnd = static_cast<unsigned>(edges.size());
    }
    for (unsigned target : edges)
        ++nodes[target].internalCount;
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // 5 and 6. Counts, then liveness.
    Vector<unsigned> transientSeeds;
    Vector<unsigned> externalSeeds;
    for (unsigned i = 0; i < nodes.size(); ++i) {
        CollectorNode& node = nodes[i];
        uint32_t count = node.block->refCount();
        uint32_t expected = 1 + node.internalCount;
        if (node.busy || node.unstable || count < expected)
            transientSeeds.append(i);
        else if (count > expected)
            externalSeeds.append(i);
    }
    bool sawTransient = !transientSeeds.isEmpty();
    auto spread = [&](Vector<unsigned>& stack, bool fromTransient) {
        while (!stack.isEmpty()) {
            unsigned i = stack.takeLast();
            for (unsigned k = nodes[i].edgesBegin; k < nodes[i].edgesEnd; ++k) {
                CollectorNode& child = nodes[edges[k]];
                if (fromTransient ? child.liveFromTransient : child.live)
                    continue;
                child.live = true;
                if (fromTransient)
                    child.liveFromTransient = true;
                stack.append(edges[k]);
            }
        }
    };
    for (unsigned i : transientSeeds) {
        nodes[i].live = true;
        nodes[i].liveFromTransient = true;
    }
    spread(transientSeeds, true);
    for (unsigned i : externalSeeds)
        nodes[i].live = true;
    spread(externalSeeds, false);
    result.retry = sawTransient || nodes.size() >= maxCollectorNodes;

    // 7. The blocks of G must not have been touched since step 3.
    size_t collected = 0;
    bool untouched = true;
    for (CollectorNode& node : nodes) {
        if (node.live)
            continue;
        ++collected;
        if (node.block->activityWord() != node.activity)
            untouched = false;
    }
    if (!untouched) {
        collected = 0;
        result.retry = true;
        // Nothing is reclaimed, so every root that was not found alive is retried.
        for (CollectorNode& node : nodes) {
            if (!node.live)
                node.liveFromTransient = true;
        }
    }

    // Roots that were found live only because a transient block reaches them, and the roots of an abandoned pass,
    // go back in the table. Their pins are still held, so they cannot be gone.
    {
        CandidateTable& table = candidateTable();
        Locker locker { table.lock };
        for (CollectorNode& node : nodes) {
            if (!node.isRoot || !node.liveFromTransient)
                continue;
            if (node.block->m_state.fetch_or(SharedStructBlock::bufferedBit, std::memory_order_acq_rel) & SharedStructBlock::bufferedBit)
                continue;
            table.blocks.add(node.block);
        }
    }

    // 8. Reclaim G.
    if (collected) {
        Vector<uint64_t> words;
        for (CollectorNode& node : nodes) {
            if (!node.live)
                node.block->takeAllWords(words);
        }
        for (uint64_t word : words) {
            SharedStructBlock* target = SharedStructBlock::blockOfWord(word);
            bool targetIsCollected = false;
            if (target) {
                auto found = index.find(target);
                targetIsCollected = found != index.end() && !nodes[found->value].live;
            }
            SharedStructBlock::releaseCollectedWord(word, targetIsCollected);
        }
        result.reclaimed = collected;
    }

    // Drop the pins. The blocks of G go with theirs.
    for (CollectorNode& node : nodes)
        node.block->derefForCollector();
    return result;
}

// ------------------------------ Running the collector ------------------------------

namespace {

constexpr size_t baseThreshold = 1024;
constexpr size_t maxThreshold = 1 << 20;

Lock wakeLock;
Condition wakeCondition;
bool threadStarted; // Guarded by wakeLock.
bool armed; // Guarded by wakeLock.
std::atomic<bool> urgent { false }; // Written under wakeLock.
std::atomic<size_t> threshold { baseThreshold };

size_t candidateCount()
{
    CandidateTable& table = candidateTable();
    Locker locker { table.lock };
    return table.blocks.size();
}

// The background collector. It waits until a candidate is recorded, then gives the program a moment to gather more
// (or until the table reaches the threshold), and runs a pass. Passes that find nothing make it wait longer and
// ask for more candidates before the next urgent pass, so that a program whose blocks are all alive pays little.
void collectorThreadMain()
{
    Seconds delay = Seconds::fromMilliseconds(20);
    for (;;) {
        {
            Locker locker { wakeLock };
            wakeCondition.wait(wakeLock, [&] { return armed; });
            if (!urgent.load(std::memory_order_relaxed))
                wakeCondition.waitFor(wakeLock, delay, [&] { return urgent.load(std::memory_order_relaxed); });
            armed = false;
            urgent.store(false, std::memory_order_relaxed);
        }
        SharedStructCollectorAccess::Result result = SharedStructCollectorAccess::collectOnce();
        if (result.reclaimed) {
            delay = Seconds::fromMilliseconds(20);
            threshold.store(baseThreshold, std::memory_order_relaxed);
        } else {
            delay = std::min(delay * 2, Seconds::fromMilliseconds(1000));
            threshold.store(std::min(threshold.load(std::memory_order_relaxed) * 2, maxThreshold), std::memory_order_relaxed);
        }
        if (result.retry || candidateCount()) {
            Locker locker { wakeLock };
            armed = true;
        }
    }
}

void candidateAdded(size_t count)
{
    bool wantsNow = count >= threshold.load(std::memory_order_relaxed);
    if (count != 1 && !wantsNow)
        return;
    if (wantsNow && urgent.load(std::memory_order_relaxed))
        return;
    Locker locker { wakeLock };
    if (!threadStarted) {
        threadStarted = true;
        Thread::create("SharedStruct cycle collector"_s, [] {
            collectorThreadMain();
        });
    }
    armed = true;
    if (wantsNow)
        urgent.store(true, std::memory_order_relaxed);
    wakeCondition.notifyOne();
}

} // namespace

size_t collectSharedStructCycles()
{
    size_t total = 0;
    for (unsigned round = 0; round < 64; ++round) {
        SharedStructCollectorAccess::Result result = SharedStructCollectorAccess::collectOnce();
        total += result.reclaimed;
        if (!result.reclaimed) {
            if (!result.retry && !candidateCount())
                break;
            Thread::yield();
        }
    }
    return total;
}

} // namespace JSC
