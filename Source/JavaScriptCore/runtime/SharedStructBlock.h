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

#pragma once

#include "ArrayBuffer.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <memory>
#include <optional>
#include <span>
#include <wtf/Lock.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Vector.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class SharedStructBlock;

// The VM-independent half of a shared struct (https://github.com/tc39/proposal-structs).
//
// A SharedStructBlock is a thread-safe refcounted array of 8-byte atomic slots. Every VM that sees the struct
// holds a JSSharedStruct view over the same block (see JSSharedStruct.h), so a write made by one agent is seen by
// all of them. Nothing in this file refers to a VM, a JSValue or the garbage collector.
//
// A slot is one 64-bit word. A number is stored as its bits (every NaN becomes the canonical quiet NaN). Every
// other kind is a quiet NaN with a non-zero 3-bit tag in bits 48..50 and a 48-bit datum: a small constant for
// undefined, null and the booleans, or the address of a thread-safe refcounted object that the slot owns one
// reference of (a string, a BigInt as decimal digits, another block, or the state of a mutex or condition).
//
// Ownership and reclamation: overwriting a slot hands its old reference to retire(). A reader loads the slot
// and takes its own reference while it is counted as a reader in m_activity (all sequentially consistent), so a retired
// reference is released only when no reader can still hold the old address: the writer swaps the slot first
// (the old word is unreachable from it from then on), then, under the garbage lock, appends the old word and
// releases the whole list only if the reader count reads zero. A reader that loaded the old word incremented the count
// before that load, the writer's zero read comes after the swap, and a counter that reads zero has seen every
// earlier increment matched by its decrement, so that reader already holds its own reference. If readers are in
// flight the list waits for the next overwrite or for the block to die. This is the scheme of Bun's polyfill
// (src/shared_struct/value.rs) and is provably safe; a block under constant read pressure defers reclamation.
// Cycles between blocks (a holds b holds a, a block that holds itself, a ring of any length, a cycle that runs
// through SharedArray elements) are reclaimed by a trial-deletion collector that works on the blocks alone and is
// independent of every VM's garbage collector (see "The cycle collector" in the .cpp, which has the soundness
// argument). A block whose reference count is decremented to a non-zero value while it holds a block becomes a
// candidate; a process-wide pass finds the sub-graph of candidates in which every reference comes from inside the
// sub-graph, and clears the slots of its blocks. Loads and stores never take a collector lock: they only count
// themselves in an atomic word (see m_activity), which is also how a pass learns that it raced with one.
// A chain of blocks of any length is released iteratively (see releaseBlockReference() in the .cpp).
//
// The same block backs a SharedArray (see JSSharedArray.h): there the slots are the indexed elements and the
// block has no shape.

// An immutable string owned by slots. The characters are UTF-16 so lone surrogates survive. WTF::String is
// not safe to share between threads (its reference count is not atomic), so a slot never holds one.
class SharedString final : public ThreadSafeRefCounted<SharedString> {
    WTF_MAKE_TZONE_ALLOCATED(SharedString);
public:
    JS_EXPORT_PRIVATE static Ref<SharedString> create(StringView);
    JS_EXPORT_PRIVATE ~SharedString();
    std::span<const char16_t> characters() const LIFETIME_BOUND { return m_characters.span(); }
    bool equals(const SharedString& other) const { return m_characters.size() == other.m_characters.size() && std::ranges::equal(m_characters, other.m_characters); }

private:
    explicit SharedString(Vector<char16_t>&& characters)
        : m_characters(WTF::move(characters))
    {
    }

    const Vector<char16_t> m_characters;
};

// The state of an Atomics.Mutex or Atomics.Condition: the SharedArrayBuffer contents that are its whole state.
class SharedSyncHandle final : public ThreadSafeRefCounted<SharedSyncHandle> {
    WTF_MAKE_TZONE_ALLOCATED(SharedSyncHandle);
public:
    enum class Kind : uint8_t { Mutex, Condition };

    static Ref<SharedSyncHandle> create(Kind kind, ArrayBufferContents&& contents) { return adoptRef(*new SharedSyncHandle(kind, WTF::move(contents))); }
    Kind kind() const { return m_kind; }
    // The address of the state word, which identifies the mutex or condition in every VM.
    const void* identity() const { return m_contents.data(); }
    // A new ArrayBuffer over the same shared memory, to wrap in the receiving VM.
    JS_EXPORT_PRIVATE Ref<ArrayBuffer> createBuffer() const;

private:
    SharedSyncHandle(Kind kind, ArrayBufferContents&& contents)
        : m_kind(kind)
        , m_contents(WTF::move(contents))
    {
    }

    Kind m_kind;
    ArrayBufferContents m_contents;
};

// A symbol that can be held by a slot. Every VM that reads one makes a symbol of its own for it and remembers the
// pair, so that one SharedSymbol is one symbol in each VM (see JSSharedStruct.cpp). A registered symbol (Symbol.for)
// and a well-known symbol (Symbol.iterator) are interned by their key and by their name, so that each is the same
// symbol in every VM without any table of pairs. A unique symbol is a SharedSymbol of its own, made when a VM first
// stores it; it lives for the rest of the process, because the VMs that know it point at it.
class SharedSymbol final : public ThreadSafeRefCounted<SharedSymbol> {
    WTF_MAKE_TZONE_ALLOCATED(SharedSymbol);
public:
    enum class Kind : uint8_t { Unique, Registered, WellKnown };

    JS_EXPORT_PRIVATE static Ref<SharedSymbol> createUnique(std::optional<StringView> description);
    JS_EXPORT_PRIVATE static Ref<SharedSymbol> registered(StringView key);
    JS_EXPORT_PRIVATE static Ref<SharedSymbol> wellKnown(StringView name);
    JS_EXPORT_PRIVATE ~SharedSymbol();

    // The unique symbols of the VMs of the process. A symbol of a VM is told by the address of its SymbolImpl, which
    // the caller keeps alive for the life of the process; a SharedSymbol has at most one symbol in each VM.
    // The SharedSymbol of a symbol, or null.
    JS_EXPORT_PRIVATE static RefPtr<SharedSymbol> findUnique(const void* symbolImpl);
    // Records that `symbolImpl` is the symbol of `shared` in `vm`. The process keeps a reference to `shared`.
    JS_EXPORT_PRIVATE static void bind(SharedSymbol& shared, const void* vm, const void* symbolImpl);
    // The SymbolImpl of this symbol in `vm`, or null if the VM has none yet.
    JS_EXPORT_PRIVATE const void* implIn(const void* vm) const;

    Kind kind() const { return m_kind; }
    // Symbol() has no description; Symbol("") has an empty one. The key of a registered symbol and the name of a
    // well-known one are in description().
    bool hasDescription() const { return m_hasDescription; }
    std::span<const char16_t> description() const LIFETIME_BOUND { return m_description.span(); }

private:
    SharedSymbol(Kind kind, bool hasDescription, Vector<char16_t>&& description)
        : m_kind(kind)
        , m_hasDescription(hasDescription)
        , m_description(WTF::move(description))
    {
    }

    Kind m_kind;
    bool m_hasDescription;
    const Vector<char16_t> m_description;
    Vector<std::pair<const void*, const void*>> m_bindings;
};

// The layout of a shared struct: its field names, root of the class chain first. Shapes are interned by their
// field list, so every struct of one layout shares one immutable shape in every VM.
class SharedStructShape final : public ThreadSafeRefCounted<SharedStructShape> {
    WTF_MAKE_TZONE_ALLOCATED(SharedStructShape);
public:
    // `encoding` lists the fields, each as a letter, its length, ':' and its characters: 's' and a name (any string,
    // an array index too), or 'y' and the address of a SharedSymbol in decimal. Safe from any thread.
    JS_EXPORT_PRIVATE static Ref<SharedStructShape> intern(const String& encoding);

    unsigned fieldCount() const { return m_fields.size(); }
    bool isSymbolField(unsigned index) const { return !!m_fields[index].symbol; }
    // The name of a field that is named by a string.
    std::span<const char16_t> fieldName(unsigned index) const LIFETIME_BOUND { return m_fields[index].name.span(); }
    SharedSymbol* fieldSymbol(unsigned index) const { return m_fields[index].symbol.get(); }
    // The slot of the field that a string that is not an array index names.
    JS_EXPORT_PRIVATE std::optional<unsigned> indexOf(StringView) const;
    JS_EXPORT_PRIVATE std::optional<unsigned> slotOfArrayIndex(uint32_t) const;
    // The fields that are properties with a name (not array indices), in order: the slot of the k-th.
    unsigned namedFieldCount() const { return m_namedSlots.size(); }
    unsigned namedSlot(unsigned k) const { return m_namedSlots[k]; }
    // The fields named by array indices, with the smallest index first.
    std::span<const std::pair<uint32_t, unsigned>> arrayIndexFields() const LIFETIME_BOUND { return m_arrayIndexFields.span(); }

private:
    explicit SharedStructShape(const String&);

    struct Field {
        Vector<char16_t> name;
        RefPtr<SharedSymbol> symbol;
    };
    Vector<Field> m_fields;
    Vector<unsigned> m_namedSlots;
    Vector<std::pair<uint32_t, unsigned>> m_arrayIndexFields;
};

// One value that a slot can hold, with the reference to its object that a slot would own.
class SharedSlotValue final {
    WTF_MAKE_NONCOPYABLE(SharedSlotValue);
public:
    enum class Kind : uint8_t { Undefined, Null, Boolean, Number, String, BigInt, Struct, Sync, Symbol };

    SharedSlotValue()
        : m_bits(special(0))
    {
    }
    SharedSlotValue(SharedSlotValue&& other)
        : m_bits(std::exchange(other.m_bits, special(0)))
    {
    }
    SharedSlotValue& operator=(SharedSlotValue&& other)
    {
        if (this != &other) {
            releaseBits(m_bits);
            m_bits = std::exchange(other.m_bits, special(0));
        }
        return *this;
    }
    ~SharedSlotValue() { releaseBits(m_bits); }

    static SharedSlotValue undefined() { return SharedSlotValue(special(0)); }
    static SharedSlotValue null() { return SharedSlotValue(special(1)); }
    static SharedSlotValue boolean(bool value) { return SharedSlotValue(special(value ? 3 : 2)); }
    static SharedSlotValue number(double value) { return SharedSlotValue(value != value ? qnan : std::bit_cast<uint64_t>(value)); }
    static SharedSlotValue string(Ref<SharedString>&& string) { return SharedSlotValue(box(TagString, &string.leakRef())); }
    static SharedSlotValue bigInt(Ref<SharedString>&& digits) { return SharedSlotValue(box(TagBigInt, &digits.leakRef())); }
    static SharedSlotValue structBlock(Ref<SharedStructBlock>&&);
    static SharedSlotValue sync(Ref<SharedSyncHandle>&& handle) { return SharedSlotValue(box(TagSync, &handle.leakRef())); }
    static SharedSlotValue symbol(Ref<SharedSymbol>&& symbol) { return SharedSlotValue(box(TagSymbol, &symbol.leakRef())); }

    Kind kind() const;
    bool boolValue() const { return m_bits == special(3); }
    double numberValue() const { return std::bit_cast<double>(m_bits); }
    // Valid while this value is alive.
    SharedString& stringValue() const LIFETIME_BOUND { return *static_cast<SharedString*>(pointer()); }
    SharedSyncHandle& syncValue() const LIFETIME_BOUND { return *static_cast<SharedSyncHandle*>(pointer()); }
    SharedSymbol& symbolValue() const LIFETIME_BOUND { return *static_cast<SharedSymbol*>(pointer()); }
    SharedStructBlock& blockValue() const LIFETIME_BOUND;

    uint64_t bits() const { return m_bits; }

private:
    friend class SharedStructBlock;

    static constexpr uint64_t qnan = 0x7ff8000000000000ull;
    static constexpr unsigned tagShift = 48;
    static constexpr uint64_t datumMask = (1ull << tagShift) - 1;
    static constexpr uint64_t TagSpecial = 1;
    static constexpr uint64_t TagString = 2;
    static constexpr uint64_t TagBigInt = 3;
    static constexpr uint64_t TagStruct = 4;
    static constexpr uint64_t TagSync = 5;
    static constexpr uint64_t TagSymbol = 6;

    explicit SharedSlotValue(uint64_t bits)
        : m_bits(bits)
    {
    }

    static constexpr uint64_t special(uint64_t datum) { return qnan | (TagSpecial << tagShift) | datum; }
    static uint64_t box(uint64_t tag, void* pointer);
    static bool isBoxed(uint64_t bits) { return (bits & 0xfff8000000000000ull) == qnan && ((bits >> tagShift) & 7); }
    static uint64_t tagOf(uint64_t bits) { return isBoxed(bits) ? (bits >> tagShift) & 7 : 0; }
    static bool ownsReference(uint64_t bits) { return tagOf(bits) >= TagString; }
    void* pointer() const { return reinterpret_cast<void*>(m_bits & datumMask); }

    // Drops the reference that `bits` owns, if it owns one.
    static void releaseBits(uint64_t bits)
    {
        if (ownsReference(bits))
            releaseReference(bits);
    }
    JS_EXPORT_PRIVATE static void releaseReference(uint64_t bits);
    // A value for the slot word `bits` with a new reference of its own. The caller guarantees the object is alive.
    static SharedSlotValue retain(uint64_t bits);
    // Whether the slot word `current` holds `expected`: numbers and constants by bits, strings and BigInts by
    // content, blocks by identity, mutexes and conditions by the state they wrap.
    static bool holds(uint64_t current, const SharedSlotValue& expected);
    // Gives up ownership of the reference, which the caller stores in a slot.
    uint64_t leakBits() { return std::exchange(m_bits, special(0)); }

    uint64_t m_bits;
};

// Test and diagnostics hooks. The number of SharedStructBlocks and of SharedStrings (which also hold BigInt digits)
// that exist right now.
JS_EXPORT_PRIVATE size_t liveSharedStructBlockCount();
JS_EXPORT_PRIVATE size_t liveSharedStringCount();
// Runs the cycle collector on the calling thread until it finds nothing more to reclaim, and returns the number of
// blocks it destroyed. It is also run on a background thread when enough candidates have gathered, so this is only
// needed to reclaim now.
JS_EXPORT_PRIVATE size_t collectSharedStructCycles();

// The cycle collector's pass, a friend of SharedStructBlock (defined in the .cpp).
struct SharedStructCollectorAccess {
    struct Result {
        size_t reclaimed { 0 };
        // Some block was in use while the pass ran, so a repeat may find more.
        bool retry { false };
    };
    static Result collectOnce();
};

// The reference count is the one of a ThreadSafeRefCounted, plus a flag for the cycle collector: a block that has
// been recorded as a candidate must not be destroyed under the collector's hands, so the last reference to such a
// block is released under the candidate lock (see deref()).
class SharedStructBlock final {
    WTF_MAKE_TZONE_ALLOCATED(SharedStructBlock);
public:
    void ref() const { m_state.fetch_add(1, std::memory_order_relaxed); }
    // Dropping a reference that leaves others behind, on a block that holds blocks, records it as a candidate for
    // the cycle collector (it may be the last reference from outside a cycle).
    void deref() const { release(true); }
    uint32_t refCount() const { return m_state.load(std::memory_order_relaxed) & countMask; }

    // Every slot starts as undefined.
    JS_EXPORT_PRIVATE static Ref<SharedStructBlock> create(Ref<SharedStructShape>&&);
    // The block of a SharedArray: `length` indexed slots and no shape. Null if the memory is not available.
    JS_EXPORT_PRIVATE static RefPtr<SharedStructBlock> createArray(unsigned length);
    JS_EXPORT_PRIVATE ~SharedStructBlock();

    bool isArray() const { return !m_shape; }
    // Only for the block of a shared struct.
    SharedStructShape& shape() const LIFETIME_BOUND
    {
        ASSERT(m_shape);
        return *m_shape;
    }
    // The number of slots: the fields of a shared struct or the length of a SharedArray.
    unsigned fieldCount() const { return m_count; }

    // All of these are sequentially consistent, whatever the caller asks for, because reclaiming an
    // overwritten reference relies on it.
    JS_EXPORT_PRIVATE SharedSlotValue load(unsigned index);
    JS_EXPORT_PRIVATE void store(unsigned index, SharedSlotValue&&);
    JS_EXPORT_PRIVATE SharedSlotValue exchange(unsigned index, SharedSlotValue&&);
    // Writes `desired` if the slot holds `expected` (null never matches) and returns the value that was read.
    JS_EXPORT_PRIVATE SharedSlotValue compareExchange(unsigned index, const SharedSlotValue* expected, SharedSlotValue&& desired);

private:
    friend struct SharedStructCollectorAccess;
    class ActivityGuard;

    SharedStructBlock(RefPtr<SharedStructShape>&&, unsigned count, std::unique_ptr<std::atomic<uint64_t>[]>&&);
    void retire(uint64_t old);

    // m_state: the reference count in the low 31 bits, and whether the block is in the candidate table.
    static constexpr uint32_t countMask = 0x7fffffffu;
    static constexpr uint32_t bufferedBit = 0x80000000u;
    // m_activity: bits 0..15 count the operations that read a slot and may take a reference (loads, and the
    // compare-exchange loop), bits 16..31 count the writers inside their exchange, and bits 32..63 are an epoch
    // that every operation advances when it starts. retire() looks at the readers only; the collector looks at all
    // of it (see the .cpp).
    static constexpr uint64_t readerUnit = 1;
    static constexpr uint64_t writerUnit = 1ull << 16;
    static constexpr uint64_t epochUnit = 1ull << 32;
    static constexpr uint64_t readersMask = 0xffffull;
    static constexpr uint64_t inFlightMask = 0xffffffffull;

    void release(bool mayBecomeCandidate) const;
    void becomeCandidate() const;
    void leaveCandidates() const;
    void noteStored(const SharedSlotValue&);

    // Used by the cycle collector only, which holds its own reference to every block it looks at.
    bool tryRefForCollector() const;
    void derefForCollector() const { release(false); }
    void clearBufferedBit() const { m_state.fetch_and(~bufferedBit, std::memory_order_acq_rel); }
    bool hasBlockSlots() const { return m_hasBlockSlots.load(std::memory_order_seq_cst); }
    uint64_t activityWord() const { return m_activity.load(std::memory_order_seq_cst); }
    static bool isInFlight(uint64_t activity) { return activity & inFlightMask; }
    uint64_t rawWord(unsigned index) const { return m_slots[index].load(std::memory_order_seq_cst); }
    static SharedStructBlock* blockOfWord(uint64_t word);
    // The block in slot `index` with a reference of the caller, or null if the slot holds no block.
    SharedStructBlock* retainBlockAt(unsigned index);
    // Releases what the retire list holds, if no reader can still see it.
    void flushGarbage();
    // Empties every slot and the retire list into `words`, whose references the caller releases.
    void takeAllWords(Vector<uint64_t>& words);
    static void releaseCollectedWord(uint64_t word, bool targetIsCollected);

    mutable std::atomic<uint32_t> m_state { 1 };
    RefPtr<SharedStructShape> m_shape;
    unsigned m_count;
    std::unique_ptr<std::atomic<uint64_t>[]> m_slots;
    std::atomic<uint64_t> m_activity { 0 };
    // Set before a block is first stored in a slot and never cleared: a block that never held one cannot be part
    // of a cycle.
    std::atomic<bool> m_hasBlockSlots { false };
    Lock m_garbageLock;
    Vector<uint64_t> m_garbage WTF_GUARDED_BY_LOCK(m_garbageLock);
};

} // namespace JSC
