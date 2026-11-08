/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyCtypes.h"

#include "JSArrayBuffer.h"
#include "JSCInlines.h"
#include "JSWeakMapInlines.h"
#include "PyList.h"
#include "PyRange.h"
#include <wtf/HexNumber.h>
#include <wtf/Int128.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {
namespace Ctypes {

// ---- Where the memory is ----

#if ASSERT_ENABLED
} // namespace Ctypes
unsigned& countOfGrantedMemory()
{
    static thread_local unsigned count;
    return count;
}
namespace Ctypes {
#endif

uint8_t* PyCData::memoryOfView(JSGlobalObject* globalObject) const
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* view = uncheckedDowncast<PyMemoryView>(m_owner.get());
    if (view->isReleased()) {
        if (uint8_t* memory = memoryBehindReleasedView(globalObject))
            return memory;
        raiseValueError(globalObject, scope, "operation forbidden on released memoryview object"_s);
        return nullptr;
    }
    uint8_t* data = view->data(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    // An empty bytearray need not have memory at all.
    static uint64_t nothing;
    if (!data)
        return reinterpret_cast<uint8_t*>(&nothing);
    return data + m_address;
}

uint8_t* PyCData::memoryBehindReleasedView(JSGlobalObject* globalObject) const
{
    ASSERT(m_isView);
    std::span<uint8_t> pinned = uncheckedDowncast<PyMemoryView>(m_owner.get())->memoryOfJSThatIsPinned(globalObject);
    uintptr_t begin = std::bit_cast<uintptr_t>(pinned.data());
    // (The address is kept as a double. From 2^53 on that is not exact.)
    uintptr_t address = whereViewWas();
    if (address >= (1ull << 53) || address < begin || address - begin > pinned.size() || static_cast<uint64_t>(size) > pinned.size() - (address - begin))
        return nullptr;
    return std::bit_cast<uint8_t*>(address);
}

void PyCData::setStorage(VM& vm, PyByteStorage* storage, size_t offset)
{
    setMemoryIn(vm, storage, storage->data() + offset);
}

void PyCData::setMemoryIn(VM& vm, JSCell* owner, const uint8_t* address)
{
    m_owner.set(vm, this, owner);
    m_address = std::bit_cast<uintptr_t>(address);
    if (m_isView)
        m_past.clear(); // It was a number.
    m_isView = false;
}

void PyCData::keepFormerStorage(VM& vm, PyByteStorage* storage)
{
    // (Not a tuple: gc.get_referents() would hand it out.)
    JSCellButterfly* earlier = formerStorages();
    unsigned count = earlier ? earlier->length() : 0;
    auto* all = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, count + 1);
    for (unsigned i = 0; i < count; ++i)
        all->setIndex(vm, i, earlier->get(i));
    all->setIndex(vm, count, storage);
    m_past.set(vm, this, all);
}

void PyCData::setView(VM& vm, PyMemoryView* view, int64_t offset, const uint8_t* memory)
{
    m_owner.set(vm, this, view);
    m_address = static_cast<uintptr_t>(offset);
    m_isView = true;
    m_past.setWithoutWriteBarrier(jsNumber(static_cast<double>(std::bit_cast<uintptr_t>(memory))));
}

void PyCData::setAddress(const void* address)
{
    ASSERT(!m_past);
    m_owner.clear();
    m_address = std::bit_cast<uintptr_t>(address);
    m_isView = false;
}

void PyCData::setMemoryOf(VM& vm, const PyCData* other, int64_t offset)
{
    ASSERT(!m_past);
    m_owner.setMayBeNull(vm, this, other->m_owner.get());
    m_address = other->m_address + static_cast<uintptr_t>(offset);
    m_isView = other->m_isView;
    if (m_isView)
        m_past.setWithoutWriteBarrier(jsNumber(static_cast<double>(other->whereViewWas() + static_cast<uintptr_t>(offset))));
}

// ---- Making instances ----

// Whether the instances have a __del__. To ask may run code of the program: a key in the dict of a class that is no str is asked whether it is '__del__'. So it is asked first. From then
// on nothing runs until the instance has its memory: where that is and how far it goes is looked at, then the instance is made, and registered (didMakeData()) last of all. So
// nobody finds an instance that has no memory, and none is finalized that was refused.
static bool hasFinalizer(PyType* type)
{
    return type->isHeapType() && type->lookup(Special::Del);
}

static ALWAYS_INLINE PyCData* didMakeData(JSGlobalObject* globalObject, PyCData* data, bool isFinalized)
{
    if (isFinalized) [[unlikely]]
        runtime(globalObject).registerFinalizable(data);
    return data;
}

static PyCData* allocateData(JSGlobalObject* globalObject, PyType* type, PyCTypeInfo* info)
{
    VM& vm = getVM(globalObject);
    State& state = *info->state.get();
    // hasDataForInstances(), and which of the two.
    PyType* layout = type->layoutBase();
    if (layout != state.dataType.get() && layout != state.functionType.get()) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(vm);
        raiseTypeError(globalObject, scope, "the instances of '"_s, type->name(), "' are no ctypes data"_s);
        return nullptr;
    }
    info->isFinal = true;
    PyCData* data = layout == state.functionType.get() ? PyCFuncPtr::create(vm, state.functionStructure.get(), type) : PyCData::create(vm, state.dataStructure.get(), type);
    data->length = info->length;
    data->size = info->size;
    return data;
}

static PyByteStorage* newMemory(JSGlobalObject* globalObject, const PyCTypeInfo* info, int64_t size, int64_t room = 0)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // In CPython a small instance has its memory in itself: 16 bytes, zeroed, whatever it needs of them. Programs have come to rely on what
    // follows being zero: a c_wchar or a full (c_char * 3) passed where C expects a string that ends. The cell is a multiple of 16
    // bytes anyway, and what is left over would otherwise hold what some dead object left there.
    static constexpr int64_t granule = 16;
    auto tryWith = [&](int64_t bytes) -> PyByteStorage* {
        if (bytes > std::numeric_limits<int64_t>::max() - granule)
            return nullptr;
        size_t capacity = std::max<size_t>(roundUpToMultipleOf<granule>(static_cast<size_t>(bytes)), granule);
        if (info->flags & (IsPointer | HasPointer))
            return PyCStorage::tryCreate(vm, info->state->storageStructure.get(), capacity);
        return PyByteStorage::tryCreate(vm, rt.structure<PyByteStorage>(), capacity);
    };
    PyByteStorage* storage = room > size ? tryWith(room) : nullptr;
    if (!storage)
        storage = tryWith(size);
    if (!storage) {
        raiseMemoryError(globalObject, scope, String());
        return nullptr;
    }
    memset(storage->data(), 0, storage->capacity());
    return storage;
}

static ALWAYS_INLINE void giveMemory(VM& vm, PyCData* data, PyByteStorage* storage, int64_t size)
{
    data->setStorage(vm, storage, 0);
    data->size = size;
    data->ownsMemory = true;
}

bool allocateMemory(JSGlobalObject* globalObject, PyCData* data, const PyCTypeInfo* info, int64_t size, int64_t room)
{
    PyByteStorage* storage = newMemory(globalObject, info, size, room);
    if (!storage)
        return false;
    giveMemory(getVM(globalObject), data, storage, size);
    return true;
}

static PyCTypeInfo* concreteInfoOf(JSGlobalObject* globalObject, PyType* type)
{
    PyCTypeInfo* info = infoOf(globalObject, type);
    if (!info) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        raiseTypeError(globalObject, scope, "abstract class"_s);
    }
    return info;
}

PyCData* newData(JSGlobalObject* globalObject, PyType* type)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCTypeInfo* info = concreteInfoOf(globalObject, type);
    RETURN_IF_EXCEPTION(scope, nullptr);
    bool isFinalized = hasFinalizer(type);
    PyByteStorage* storage = newMemory(globalObject, info, info->size);
    RETURN_IF_EXCEPTION(scope, nullptr);
    PyCData* data = allocateData(globalObject, type, info);
    RETURN_IF_EXCEPTION(scope, nullptr);
    giveMemory(vm, data, storage, info->size);
    return didMakeData(globalObject, data, isFinalized);
}

// ---- Who keeps what a pointer points to ----

const ClassInfo PyCStorage::s_info = { "PyCStorage"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PyCStorage) };
static_assert(sizeof(PyCStorage) == sizeof(PyByteStorage)); // data()

PyCStorage::PyCStorage(VM& vm, Structure* structure, size_t capacity)
    : Base(vm, structure, capacity)
{
    slot().clear();
}

PyCStorage* PyCStorage::tryCreate(VM& vm, Structure* structure, size_t capacity)
{
    ASSERT(!(capacity % sizeof(WriteBarrier<Unknown>)));
    void* cell = tryAllocate(vm, capacity, sizeof(WriteBarrier<Unknown>));
    if (!cell)
        return nullptr;
    auto* storage = new (NotNull, cell) PyCStorage(vm, structure, capacity);
    storage->finishCreation(vm);
    return storage;
}

template<typename Visitor>
void PyCStorage::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyCStorage>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->slot());
}

DEFINE_VISIT_CHILDREN(PyCStorage);

const ClassInfo PyCTable::s_info = { "PyCTable"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PyCTable) };

PyCTable::PyCTable(VM& vm, Structure* structure)
    : Base(vm, structure)
{
}

PyCTable* PyCTable::tryCreate(VM& vm, Structure* structure)
{
    void* cell = tryAllocateCell<PyCTable>(vm);
    if (!cell)
        return nullptr;
    auto* table = new (NotNull, cell) PyCTable(vm, structure);
    table->finishCreation(vm);
    return table;
}

// The record of a cell is the entry for its first place, if that is the only one: so it is with a pointer. Else it is a table, with the entry for the place at 8 * i as its property i. Indexed properties are
// dense or sparse by the rule that JSC has for them (ArrayConventions.h), so what a table takes goes by the entries and not by the memory.
static ALWAYS_INLINE PyCTable* tableIn(JSValue record)
{
    return record && record.isCell() ? dynamicDowncast<PyCTable>(record.asCell()) : nullptr;
}

static JSValue recordOf(JSGlobalObject* globalObject, JSCell* cell)
{
    if (auto* storage = dynamicDowncast<PyCStorage>(cell)) [[likely]]
        return storage->record();
    JSWeakMap* records = stateOf(globalObject).records.get();
    JSValue record = records ? records->get(cell) : jsUndefined();
    return record.isUndefined() ? JSValue() : record;
}

static void setRecordOf(JSGlobalObject* globalObject, JSCell* cell, JSValue record)
{
    VM& vm = getVM(globalObject);
    if (auto* storage = dynamicDowncast<PyCStorage>(cell)) [[likely]] {
        storage->setRecord(vm, record);
        return;
    }
    State& state = stateOf(globalObject);
    if (!record) {
        if (state.records)
            state.records->remove(cell);
        return;
    }
    if (!state.records)
        state.records.set(vm, &state, JSWeakMap::create(vm, globalObject->weakMapStructure()));
    state.records->set(vm, cell, record);
}

std::span<uint8_t> bytesOfOwner(JSCell* cell)
{
    if (cell->type() == PyBytesType) {
        auto* bytes = uncheckedDowncast<PyBytes>(cell);
        return { bytes->mutableData(), bytes->length() + 1 };
    }
    if (auto* storage = dynamicDowncast<PyByteStorage>(cell))
        return { storage->data(), storage->capacity() };
    return { };
}

static constexpr int64_t sizeOfPlace = sizeof(void*);

// False: there is none there, or it is too far out for the table.
static ALWAYS_INLINE bool indexOfPlace(int64_t offset, unsigned& index)
{
    if (offset < 0 || offset % sizeOfPlace || offset / sizeOfPlace > MAX_ARRAY_INDEX)
        return false;
    index = static_cast<unsigned>(offset / sizeOfPlace);
    return true;
}

static ALWAYS_INLINE int64_t placeOfIndex(unsigned index)
{
    return static_cast<int64_t>(index) * sizeOfPlace;
}

JSValue entryAt(JSGlobalObject* globalObject, const Home& home)
{
    unsigned index;
    if (!home || !indexOfPlace(home.offset, index))
        return { };
    JSValue record = recordOf(globalObject, home.cell);
    if (PyCTable* table = tableIn(record)) {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(getVM(globalObject));
        JSValue entry = table->getDirectIndex(globalObject, index);
        scope.assertNoException();
        return entry;
    }
    return index ? JSValue() : record;
}

void setEntry(JSGlobalObject* globalObject, const Home& home, JSValue entry)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned index;
    if (!home || !indexOfPlace(home.offset, index))
        return;
    ASSERT(!entry || entry.isCell());
    JSValue record = recordOf(globalObject, home.cell);
    PyCTable* table = tableIn(record);
    if (!table) {
        if (!index) {
            setRecordOf(globalObject, home.cell, entry);
            return;
        }
        if (!entry)
            return;
        table = PyCTable::tryCreate(vm, stateOf(globalObject).tableStructure.get());
        if (!table) {
            raiseMemoryError(globalObject, scope, String());
            return;
        }
        if (record) {
            table->putByIndexInline(globalObject, 0u, record, true);
            RETURN_IF_EXCEPTION(scope, void());
        }
        setRecordOf(globalObject, home.cell, table);
    }
    scope.release();
    // (An ordinary put, which nothing intercepts: there is no prototype. putDirectIndex() takes the short way for arrays and plain objects only, and would make every other object a dictionary.)
    if (entry)
        table->putByIndexInline(globalObject, index, entry, true);
    else
        JSObject::deletePropertyByIndex(table, globalObject, index);
}

// What the record has for the places that lie wholly in the `size` bytes from `offset` on: where each is, counted from `offset`, and its entry. It goes by the entries that there are, not by the places.
static void collectEntries(JSValue record, int64_t offset, int64_t size, Vector<int64_t, 8>& places, MarkedArgumentBuffer* entries = nullptr)
{
    int64_t last = offset + size - sizeOfPlace;
    auto add = [&](unsigned index, JSValue entry) {
        int64_t place = placeOfIndex(index);
        if (!entry || place < offset || place > last)
            return;
        places.append(place - offset);
        if (entries)
            entries->append(entry);
    };
    PyCTable* table = tableIn(record);
    if (!table) {
        add(0, record);
        return;
    }
    // Where indexed properties are dense.
    auto addDense = [&](unsigned length, auto&& at) {
        int64_t end = std::min<int64_t>(length, last / sizeOfPlace + 1);
        for (int64_t i = std::max<int64_t>(offset, 0) / sizeOfPlace; i < end; ++i)
            add(static_cast<unsigned>(i), at(static_cast<unsigned>(i)));
    };
    switch (table->indexingType()) {
    case ALL_CONTIGUOUS_INDEXING_TYPES: {
        Butterfly* butterfly = table->butterfly();
        addDense(butterfly->publicLength(), [&](unsigned i) { return butterfly->contiguous().at(table, i).get(); });
        break;
    }
    case ALL_ARRAY_STORAGE_INDEXING_TYPES: {
        ArrayStorage* storage = table->butterfly()->arrayStorage();
        addDense(std::min(storage->length(), storage->vectorLength()), [&](unsigned i) { return storage->m_vector[i].get(); });
        SparseArrayValueMap* map = storage->m_sparseMap.get();
        if (!map)
            break;
        if (static_cast<uint64_t>(size) >= map->size()) {
            for (auto& entry : *map)
                add(entry.index(), entry.asValue().get());
            break;
        }
        // A few bytes of a memory that has many entries.
        for (int64_t place = offset; place <= last; ++place) {
            unsigned index;
            if (!indexOfPlace(place, index))
                continue;
            if (auto entry = map->find(index); entry != map->notFound())
                add(index, entry->asValue().get());
        }
        break;
    }
    default:
        // Cells are all that is ever put there.
        break;
    }
}

void copyEntries(JSGlobalObject* globalObject, const Home& from, const Home& to, int64_t size)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!to || size < sizeOfPlace)
        return;
    JSValue source = from ? recordOf(globalObject, from.cell) : JSValue();
    JSValue destination = recordOf(globalObject, to.cell);
    if (!source && !destination)
        return;
    // All is looked at before anything is written: the two may be one memory, and overlap.
    Vector<int64_t, 8> places;
    Vector<int64_t, 8> overwritten;
    MarkedArgumentBuffer entries;
    collectEntries(source, from.offset, size, places, &entries);
    collectEntries(destination, to.offset, size, overwritten);
    if (entries.hasOverflowed()) [[unlikely]] {
        raiseMemoryError(globalObject, scope, String());
        return;
    }
    for (int64_t place : overwritten) {
        setEntry(globalObject, { to.cell, to.offset + place }, { });
        RETURN_IF_EXCEPTION(scope, void());
    }
    for (unsigned i = 0; i < places.size(); ++i) {
        setEntry(globalObject, { to.cell, to.offset + places[i] }, entries.at(i));
        RETURN_IF_EXCEPTION(scope, void());
    }
}

// What to write down beside an address in the memory of the instance. Empty: nobody is known to own that.
static ALWAYS_INLINE JSValue entryFor(PyCData* data)
{
    if (data->isView())
        return data;
    return data->owner() ? JSValue(data->owner()) : JSValue();
}

// What a pointer points to is, as a rule, the memory of an instance that the pointer keeps alive. Then it is known how far that
// memory goes, and who owns it.
struct Provenance {
    JSCell* owner { nullptr }; // A PyMemoryView, or what has bytesOfOwner(). Null: nothing is known.
    const uint8_t* begin { nullptr };
    const uint8_t* end { nullptr };
    int64_t offsetOfBegin { 0 }; // In the view, if that is the owner.
    JSCell* entry { nullptr }; // What to write down beside the address: see entryAt().
    bool isInWhatIsGone { false }; // It points to where something that is kept alive was, and is no more: a view has been released, JS has detached or shrunk the memory, or it has moved.
    bool isNotAllSeen { false }; // More is kept alive than is looked through.
};

static bool findProvenance(JSGlobalObject* globalObject, JSValue kept, const uint8_t* address, Provenance& result, unsigned depth = 0)
{
    if (!kept || !kept.isCell())
        return false;
    if (depth > 4) {
        result.isNotAllSeen = true;
        return false;
    }
    if (auto* other = PyCData::tryFrom(kept)) {
        if (!other->owner())
            return false;
        const uint8_t* begin;
        size_t size;
        if (other->isView()) {
            auto isGone = [&] {
                // (Wraps around below it.)
                result.isInWhatIsGone |= std::bit_cast<uintptr_t>(address) - other->whereViewWas() <= static_cast<uintptr_t>(other->size);
                return false;
            };
            auto* view = uncheckedDowncast<PyMemoryView>(other->owner());
            if (view->isReleased()) {
                begin = other->memoryBehindReleasedView(globalObject);
                if (!begin)
                    return isGone();
                // Whoever is made to lie in there has to hold the cell of the view as well.
                if (address < begin || address > begin + other->size)
                    return false;
                result = { view, begin, begin + other->size, other->offsetInView(), other };
                return true;
            }
            auto scope = DECLARE_TOP_EXCEPTION_SCOPE(getVM(globalObject));
            begin = view->data(globalObject);
            if (scope.exception()) {
                dropPendingException(globalObject, scope);
                return isGone();
            }
            size = static_cast<size_t>(view->length);
            // As a rule what is exported does not move. The bytes of a small typed array of JS do, when its buffer is first asked for; so does a bytearray under a view of a view of it whose
            // first has been released.
            if (!begin || address < begin || address > begin + size)
                return isGone();
        } else {
            std::span<uint8_t> memory = bytesOfOwner(other->owner());
            begin = memory.data();
            size = memory.size();
            if (address < begin || address > begin + size) {
                // It may point to where the instance was before a resize().
                JSCellButterfly* former = other->formerStorages();
                for (unsigned i = former ? former->length() : 0; i--;) {
                    auto* earlier = uncheckedDowncast<PyByteStorage>(former->get(i).asCell());
                    if (address >= earlier->data() && address <= earlier->data() + earlier->capacity()) {
                        result = { earlier, earlier->data(), earlier->data() + earlier->capacity(), 0, earlier };
                        return true;
                    }
                }
                return false;
            }
        }
        result = { other->owner(), begin, begin + size, 0, entryFor(other).asCell() };
        return true;
    }
    if (auto* tuple = PyTuple::tryFrom(kept)) {
        for (unsigned i = 0; i < tuple->length(); ++i) {
            if (findProvenance(globalObject, tuple->at(i), address, result, depth + 1))
                return true;
        }
        return false;
    }
    if (auto* dict = PyDict::tryFrom(kept)) {
        JSValue key;
        JSValue value;
        unsigned seen = 0;
        for (unsigned position = 0; dict->next(position, key, value); ++seen) {
            if (seen == 32) {
                result.isNotAllSeen = true;
                break;
            }
            if (findProvenance(globalObject, value, address, result, depth + 1))
                return true;
        }
    }
    return false;
}

static bool findProvenanceByEntry(JSGlobalObject* globalObject, JSValue entry, const uint8_t* address, Provenance& result)
{
    if (!entry || !entry.isCell())
        return false;
    if (PyCData::is(entry))
        return findProvenance(globalObject, entry, address, result);
    std::span<uint8_t> memory = bytesOfOwner(entry.asCell());
    if (!memory.data() || address < memory.data() || address > memory.data() + memory.size())
        return false;
    result = { entry.asCell(), memory.data(), memory.data() + memory.size(), 0, entry.asCell() };
    return true;
}

// What is kept for a part lies in the _objects of the whole under the way to the part (keyOfPart()). If something that the part is in was stored whole, what lies under the way
// to that are the _objects of what was stored, and the rest of the way is a key in them. `way` is from the outside in.
static bool findProvenanceUnder(JSGlobalObject* globalObject, PyDict* kept, std::span<const int64_t> way, const uint8_t* address, Provenance& result, unsigned& looksLeft)
{
    Runtime& rt = runtime(globalObject);
    for (size_t taken = 1; taken <= way.size(); ++taken) {
        if (!looksLeft)
            return false;
        --looksLeft;
        StringBuilder key;
        for (size_t i = taken; i--;)
            key.append(hex(static_cast<uint32_t>(way[i]), Lowercase), i ? ":"_s : ""_s);
        JSValue value = kept->get(makeStr(rt.vm(), key.toString()));
        if (taken == way.size())
            return findProvenance(globalObject, value, address, result);
        if (auto* inner = PyDict::tryFrom(value); inner && inner->type() == rt.dictType() && findProvenanceUnder(globalObject, inner, way.subspan(taken), address, result, looksLeft))
            return true;
    }
    return false;
}

bool liesInMemoryOf(JSGlobalObject* globalObject, PyCData* data, const void* address)
{
    Provenance provenance;
    return findProvenance(globalObject, data, static_cast<const uint8_t*>(address), provenance) || provenance.isInWhatIsGone;
}

static bool findProvenanceOfPart(JSGlobalObject* globalObject, PyDict* kept, PyCData* pointer, const uint8_t* address, Provenance& result)
{
    Vector<int64_t, 16> way;
    for (PyCData* part = pointer; part->base; part = part->base.get())
        way.append(part->index);
    way.reverse();
    unsigned looksLeft = 64;
    // Under its own place, if a pointer was stored there; under those of its own two parts, if its contents were set.
    if (findProvenanceUnder(globalObject, kept, way.span(), address, result, looksLeft))
        return true;
    for (int64_t index : { 1, 0 }) {
        way.append(index);
        if (findProvenanceUnder(globalObject, kept, way.span(), address, result, looksLeft))
            return true;
        way.removeLast();
    }
    return false;
}

// What an address points into that lies at `home`, in the memory of the instance.
static bool findProvenanceOfTarget(JSGlobalObject* globalObject, PyCData* holder, const Home& home, const uint8_t* target, Provenance& provenance)
{
    if (findProvenanceByEntry(globalObject, entryAt(globalObject, home), target, provenance)) [[likely]]
        return true;
    // Nothing was written down for the place, or the bytes have been written since by something that writes nothing down. What the instance keeps alive is in _objects as well, which the
    // program can get at: this helps against accidents, it is no wall.
    PyCData* root = holder;
    while (root->base)
        root = root->base.get();
    JSValue kept = root->objects.get();
    if (findProvenance(globalObject, kept, target, provenance))
        return true;
    if (!provenance.isNotAllSeen || root == holder) [[likely]]
        return false;
    // It is a part of something larger, which keeps more alive for its parts than is looked through.
    auto* dict = PyDict::tryFrom(kept);
    return dict && dict->type() == runtime(globalObject).dictType() && findProvenanceOfPart(globalObject, dict, holder, target, provenance);
}

// What to write down beside a copy of that address.
static JSValue entryForCopy(JSGlobalObject* globalObject, PyCData* holder, const Home& home, const uint8_t* target)
{
    if (!target)
        return { };
    Provenance provenance;
    if (findProvenanceOfTarget(globalObject, holder, home, target, provenance))
        return provenance.entry;
    // (By it a copy knows as well that what it points into is gone.)
    return entryAt(globalObject, home);
}

static ALWAYS_INLINE bool isAddress(Code code)
{
    return code == Code::CharPointer || code == Code::WCharPointer || code == Code::VoidPointer;
}

// An address has been stored there as a value (c_char_p, c_wchar_p, c_void_p).
static void didStoreAddress(JSGlobalObject* globalObject, const Home& home, const Packed& packed)
{
    if (packed.owner) {
        setEntry(globalObject, home, packed.owner);
        return;
    }
    // An int, or None: nothing says what it points into. If that is what the place pointed into before, it goes on being known: the value has been read and stored again, or moved on a little.
    JSValue entry = entryAt(globalObject, home);
    if (!entry)
        return;
    const uint8_t* target;
    memcpy(&target, packed.bytes, sizeof(target));
    Provenance provenance;
    if (!findProvenanceByEntry(globalObject, entry, target, provenance) && !provenance.isInWhatIsGone)
        setEntry(globalObject, home, { });
}

// What the pointer holds. False: an exception; a null pointer is one.
static bool pointerTarget(JSGlobalObject* globalObject, PyCData* pointer, uint8_t*& target, Home* home = nullptr)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    {
        auto memory = Place { pointer }.resolve(globalObject, sizeof(void*), home);
        RETURN_IF_EXCEPTION(scope, false);
        memcpy(&target, memory.data(), sizeof(void*));
    }
    if (!target) {
        raiseValueError(globalObject, scope, "NULL pointer access"_s);
        return false;
    }
    return true;
}

// `size` bytes, `offset` bytes from where the pointer points now. If it is known what it points into, they have to be within that: the type of a pointer need
// not be that of what it was made to point to. Null: an exception.
static uint8_t* lookThroughPointer(JSGlobalObject* globalObject, PyCData* pointer, int64_t offset, int64_t size, Provenance& provenance)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint8_t* target;
    Home home;
    if (failed(scope, pointerTarget(globalObject, pointer, target, &home)))
        return nullptr;
    uint8_t* address = std::bit_cast<uint8_t*>(std::bit_cast<uintptr_t>(target) + static_cast<uintptr_t>(offset));
    if (!findProvenanceOfTarget(globalObject, pointer, home, target, provenance)) {
        if (!provenance.isInWhatIsGone) [[likely]]
            return address;
        raiseValueError(globalObject, scope, "the memory that the pointer was made to point into is no longer there"_s);
        return nullptr;
    }
    if (address >= provenance.begin && address <= provenance.end && size <= provenance.end - address)
        return address;
    raiseValueError(globalObject, scope, "access through a pointer beyond the memory of the object it points into"_s);
    return nullptr;
}

static Home homeInView(PyMemoryView* view, int64_t offset)
{
    if (view->isReleased())
        return { };
    return { view->managedBuffer()->owner(), view->offset + offset };
}

static Home homeOf(PyCData* data, int64_t offset)
{
    JSCell* owner = data->owner();
    if (!owner)
        return { };
    if (data->isView())
        return homeInView(uncheckedDowncast<PyMemoryView>(owner), data->offsetInView() + offset);
    return { owner, data->addressInStorage() - bytesOfOwner(owner).data() + offset };
}

static Home homeOf(const Provenance& provenance, const uint8_t* address)
{
    if (!provenance.owner)
        return { };
    if (auto* view = dynamicDowncast<PyMemoryView>(provenance.owner))
        return homeInView(view, provenance.offsetOfBegin + (address - provenance.begin));
    return { provenance.owner, address - provenance.begin };
}

static ALWAYS_INLINE Granted grant(VM& vm, uint8_t* data, size_t size)
{
#if ASSERT_ENABLED
    return { vm, data, size };
#else
    UNUSED_PARAM(vm);
    return { data, size };
#endif
}

Granted Place::resolve(JSGlobalObject* globalObject, int64_t size, Home* home) const
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // A CField can be asked about an instance of any ctypes type, and an instance can be given a class that takes it for more than it is.
    if (size < 0 || (!isThroughPointer && (offset < 0 || offset > object->size || size > object->size - offset))) {
        raiseValueError(globalObject, scope, "the memory of the instance is too small"_s);
        return { };
    }
    if (isThroughPointer) {
        Provenance provenance;
        uint8_t* address = lookThroughPointer(globalObject, object, offset, size, provenance);
        RETURN_IF_EXCEPTION(scope, { });
        if (home)
            *home = homeOf(provenance, address);
        return grant(vm, address, static_cast<size_t>(size));
    }
    uint8_t* memory = object->memory(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (!memory) {
        raiseValueError(globalObject, scope, "NULL pointer access"_s);
        return { };
    }
    if (home)
        *home = homeOf(object, offset);
    return grant(vm, memory + offset, static_cast<size_t>(size));
}

PyCData* dataAt(JSGlobalObject* globalObject, PyType* type, const Place& place, int64_t index)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCTypeInfo* info = concreteInfoOf(globalObject, type);
    RETURN_IF_EXCEPTION(scope, nullptr);
    bool isFinalized = hasFinalizer(type);
    if (!place.isThroughPointer) {
        if (place.offset < 0 || place.offset > place.object->size || info->size > place.object->size - place.offset) {
            raiseValueError(globalObject, scope, "the memory of the instance is too small"_s);
            return nullptr;
        }
        PyCData* data = allocateData(globalObject, type, info);
        RETURN_IF_EXCEPTION(scope, nullptr);
        data->base.set(vm, data, place.object);
        data->index = index;
        data->setMemoryOf(vm, place.object, place.offset);
        return didMakeData(globalObject, data, isFinalized);
    }
    // The pointer may be made to point elsewhere while this instance is in use: it holds on to the memory itself.
    Provenance provenance;
    uint8_t* address = lookThroughPointer(globalObject, place.object, place.offset, info->size, provenance);
    RETURN_IF_EXCEPTION(scope, nullptr);
    PyCData* data = allocateData(globalObject, type, info);
    RETURN_IF_EXCEPTION(scope, nullptr);
    data->base.set(vm, data, place.object);
    data->index = index;
    if (!provenance.owner)
        data->setAddress(address);
    else if (auto* view = dynamicDowncast<PyMemoryView>(provenance.owner))
        data->setView(vm, view, provenance.offsetOfBegin + (address - provenance.begin), address);
    else
        data->setMemoryIn(vm, provenance.owner, address);
    return didMakeData(globalObject, data, isFinalized);
}

PyCData* dataAtAddress(JSGlobalObject* globalObject, PyType* type, const void* address)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool ok = audit(globalObject, "ctypes.cdata"_s, intFromPointer(globalObject, address));
    RETURN_IF_EXCEPTION(scope, nullptr);
    ASSERT_UNUSED(ok, ok);
    PyCTypeInfo* info = concreteInfoOf(globalObject, type);
    RETURN_IF_EXCEPTION(scope, nullptr);
    bool isFinalized = hasFinalizer(type);
    PyCData* data = allocateData(globalObject, type, info);
    RETURN_IF_EXCEPTION(scope, nullptr);
    data->setAddress(address);
    return didMakeData(globalObject, data, isFinalized);
}

bool checkRoomFor(JSGlobalObject* globalObject, PyCTypeInfo* info, int64_t length, int64_t offset)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (info->size <= length - offset)
        return true;
    raiseValueError(globalObject, scope, "Buffer size too small ("_s, length, " instead of at least "_s, static_cast<int64_t>(static_cast<uint64_t>(info->size) + static_cast<uint64_t>(offset)), " bytes)"_s);
    return false;
}

PyCData* dataInView(JSGlobalObject* globalObject, PyType* type, PyMemoryView* view, int64_t offset)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // A hook has run since the view was made, and more run here: each may have found the view and released it.
    auto located = [&]() -> uint8_t* {
        if (view->isReleased()) {
            raiseValueError(globalObject, scope, "operation forbidden on released memoryview object"_s);
            return nullptr;
        }
        return view->data(globalObject);
    };
    uint8_t* memory = located();
    RETURN_IF_EXCEPTION(scope, nullptr);
    bool ok = audit(globalObject, "ctypes.cdata"_s, intFromPointer(globalObject, memory + offset));
    RETURN_IF_EXCEPTION(scope, nullptr);
    ASSERT_UNUSED(ok, ok);
    PyCTypeInfo* info = concreteInfoOf(globalObject, type);
    RETURN_IF_EXCEPTION(scope, nullptr);
    bool isFinalized = hasFinalizer(type);
    // The caller has looked, but the hooks have run since, and the class may have been given its fields by one.
    if (failed(scope, checkRoomFor(globalObject, info, view->length, offset)))
        return nullptr;
    memory = located();
    RETURN_IF_EXCEPTION(scope, nullptr);
    PyCData* data = allocateData(globalObject, type, info);
    RETURN_IF_EXCEPTION(scope, nullptr);
    data->setView(vm, view, offset, memory + offset);
    return didMakeData(globalObject, data, isFinalized);
}

PyCData* dataFromCopy(JSGlobalObject* globalObject, PyType* type, std::span<const uint8_t> source)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* data = newData(globalObject, type);
    RETURN_IF_EXCEPTION(scope, nullptr);
    auto memory = Place { data }.resolve(globalObject, std::min<int64_t>(source.size(), data->size));
    RETURN_IF_EXCEPTION(scope, nullptr);
    memcpy(memory.data(), source.data(), memory.size());
    return data;
}

bool isSimpleInstanceType(State& state, PyType* type)
{
    return type->base() == state.simpleType.get();
}

// ---- What keeps what alive ----

// The way from the container to the part, from the inside out, in hexadecimal. Null: it is too long.
static String keyOfPart(PyCData* target, int64_t index)
{
    StringBuilder key;
    key.append(hex(static_cast<uint32_t>(index), Lowercase));
    for (PyCData* part = target; part->base; part = part->base.get()) {
        if (key.length() > 239)
            return String();
        key.append(':', hex(static_cast<uint32_t>(part->index), Lowercase));
    }
    return key.toString();
}

// Nothing is kept yet, or what is kept is kept for the whole and there is a part to keep something for.
static NEVER_INLINE void makeRoomForWhatIsKept(JSGlobalObject* globalObject, PyCData* data, Kept kept)
{
    VM& vm = getVM(globalObject);
    JSValue objects = data->objects.get();
    if (!objects) {
        data->objects.set(vm, data, data->length || kept == Kept::ForPart ? JSValue(PyDict::create(globalObject)) : none());
        return;
    }
    // It was made without parts.
    auto* dict = PyDict::create(globalObject);
    if (!isNone(objects))
        dict->set(vm, makeStr(vm, keyOfPart(data, -1)), objects);
    data->objects.set(vm, data, dict);
}

static ALWAYS_INLINE PyCData* containerOf(JSGlobalObject* globalObject, PyCData* data, Kept kept = Kept::ForWhole)
{
    // The whole of an instance that lies in another is a part of that one.
    if (data->base) {
        kept = Kept::ForPart;
        do
            data = data->base.get();
        while (data->base);
    }
    auto isDict = [&](JSValue objects) {
        auto* dict = PyDict::tryFrom(objects);
        return dict && dict->type() == runtime(globalObject).dictType();
    };
    if (JSValue objects = data->objects.get(); !objects || (kept == Kept::ForPart && !isDict(objects))) [[unlikely]]
        makeRoomForWhatIsKept(globalObject, data, kept);
    return data;
}

JSValue keptObjects(JSGlobalObject* globalObject, PyCData* data, Kept kept)
{
    return containerOf(globalObject, data, kept)->objects.get();
}

bool keepAlive(JSGlobalObject* globalObject, PyCData* target, int64_t index, JSValue keep, Kept kept)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isNone(keep))
        return true;
    PyCData* container = containerOf(globalObject, target, kept);
    auto* dict = PyDict::tryFrom(container->objects.get());
    if (!dict || dict->type() != rt.dictType()) {
        container->objects.set(vm, container, keep);
        return true;
    }
    String key = keyOfPart(target, index);
    if (key.isNull()) {
        raiseValueError(globalObject, scope, "ctypes object structure too deep"_s);
        return false;
    }
    dict->set(globalObject, makeStr(vm, key), keep);
    return !scope.exception();
}

JSValue keptFor(JSGlobalObject* globalObject, PyCData* target, int64_t index)
{
    Runtime& rt = runtime(globalObject);
    PyCData* container = target;
    while (container->base)
        container = container->base.get();
    JSValue objects = container->objects.get();
    auto* dict = PyDict::tryFrom(objects);
    if (!dict || dict->type() != rt.dictType())
        return objects;
    String key = keyOfPart(target, index);
    if (key.isNull())
        return { };
    return dict->get(makeStr(rt.vm(), key));
}

// ---- Reading and writing parts ----

void extendError(JSGlobalObject* globalObject, PyType* exceptionType, const String& prefix)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue exception = takePendingException(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    String name = typeOf(globalObject, exception)->name();
    JSString* text = str(globalObject, exception);
    String message;
    if (scope.exception()) {
        takePendingException(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        message = "???"_s;
    } else
        message = stringOf(text);
    raise(globalObject, scope, exceptionType, makeString(prefix, name, ": "_s, message));
}

JSValue getData(JSGlobalObject* globalObject, PyType* type, Accessor accessor, const Place& place, int64_t index, int64_t size, Bits bits)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!accessor) {
        PyCTypeInfo* info = infoOf(globalObject, type);
        if (info && info->accessor && isSimpleInstanceType(*info->state.get(), type))
            accessor = info->accessor;
    }
    if (!accessor)
        RELEASE_AND_RETURN(scope, dataAt(globalObject, type, place, index));
    auto memory = place.resolve(globalObject, size);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue hint = accessor.code == Code::Object ? keptFor(globalObject, place.object, index) : JSValue();
    RELEASE_AND_RETURN(scope, load(globalObject, accessor, memory, bits, hint));
}

// What has to be kept alive for the part from now on. Empty: an exception.
static JSValue storeData(JSGlobalObject* globalObject, PyType* type, Accessor accessor, const Place& place, JSValue value, int64_t size, Bits bits)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto storeWith = [&](Accessor accessor) -> JSValue {
        Packed packed;
        bool ok = pack(globalObject, accessor, value, size, packed);
        RETURN_IF_EXCEPTION(scope, { });
        ASSERT_UNUSED(ok, ok);
        Home home;
        {
            auto memory = place.resolve(globalObject, size, isAddress(accessor.code) ? &home : nullptr);
            RETURN_IF_EXCEPTION(scope, { });
            store(memory, accessor, packed, bits);
        }
        ensureStillAliveHere(value);
        if (isAddress(accessor.code)) {
            didStoreAddress(globalObject, home, packed);
            RETURN_IF_EXCEPTION(scope, { });
        }
        return packed.keep;
    };

    if (accessor)
        RELEASE_AND_RETURN(scope, storeWith(accessor));

    PyCData* source = PyCData::tryFrom(value);
    if (!source) {
        PyCTypeInfo* info = infoOf(globalObject, type);
        if (info && info->accessor)
            RELEASE_AND_RETURN(scope, storeWith(info->accessor));
        // A tuple stands for the arguments to make an instance with.
        if (auto* tuple = PyTuple::tryFrom(value)) {
            JSValue object = callWithTupleAndDict(globalObject, type, tuple, nullptr);
            if (scope.exception()) [[unlikely]] {
                scope.release();
                extendError(globalObject, runtime(globalObject).RuntimeError(), makeString('(', type->nameInMessages(), ") "_s));
                return { };
            }
            if (!PyCData::is(object))
                return raiseTypeError(globalObject, scope, "expected "_s, type->nameInMessages(), " instance, got "_s, typeName(globalObject, object));
            RELEASE_AND_RETURN(scope, storeData(globalObject, type, accessor, place, object, size, bits));
        }
        if (isNone(value) && info && info->kind == TypeKind::Pointer) {
            Home home;
            {
                auto memory = place.resolve(globalObject, sizeof(void*), &home);
                RETURN_IF_EXCEPTION(scope, { });
                memset(memory.data(), 0, memory.size());
            }
            setEntry(globalObject, home, { });
            RETURN_IF_EXCEPTION(scope, { });
            return none();
        }
        return raiseTypeError(globalObject, scope, "expected "_s, type->nameInMessages(), " instance, got "_s, typeName(globalObject, value));
    }

    bool isOfType = isInstance(globalObject, value, type);
    RETURN_IF_EXCEPTION(scope, { });
    if (isOfType) {
        // __instancecheck__ may say anything.
        if (source->size < size)
            return raiseValueError(globalObject, scope, "the memory of the instance is too small"_s);
        Home homeOfSource;
        Home home;
        const uint8_t* target = nullptr;
        {
            auto from = Place { source }.resolve(globalObject, size, &homeOfSource);
            RETURN_IF_EXCEPTION(scope, { });
            auto to = place.resolve(globalObject, size, &home);
            RETURN_IF_EXCEPTION(scope, { });
            memmove(to.data(), from.data(), to.size());
            if (size == sizeof(void*))
                memcpy(&target, to.data(), sizeof(void*));
        }
        if (PyCTypeInfo* info = infoOf(globalObject, type); info && info->flags & IsPointer && size == sizeof(void*)) {
            // Also where only the search knows what it points into: what the search goes by is not handed on in a way that lasts.
            setEntry(globalObject, home, entryForCopy(globalObject, source, homeOfSource, target));
            RETURN_IF_EXCEPTION(scope, { });
        } else if (info && info->flags & HasPointer) {
            copyEntries(globalObject, homeOfSource, home, size);
            RETURN_IF_EXCEPTION(scope, { });
        }
        return keptObjects(globalObject, source);
    }

    if (PyCTypeInfo* pointerInfo = infoOf(globalObject, type); pointerInfo && pointerInfo->kind == TypeKind::Pointer) {
        PyCTypeInfo* arrayInfo = infoOfInstance(globalObject, source);
        if (arrayInfo && arrayInfo->kind == TypeKind::Array && arrayInfo->proto.get() == pointerInfo->proto.get()) {
            uint8_t* from = source->memory(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            Home home;
            {
                auto to = place.resolve(globalObject, sizeof(void*), &home);
                RETURN_IF_EXCEPTION(scope, { });
                memcpy(to.data(), &from, to.size());
            }
            setEntry(globalObject, home, entryFor(source));
            RETURN_IF_EXCEPTION(scope, { });
            // The array itself has to live, not only what it keeps alive.
            return PyTuple::create(globalObject, { keptObjects(globalObject, source), source });
        }
    }
    return raiseTypeError(globalObject, scope, "incompatible types, "_s, typeName(globalObject, value), " instance instead of "_s, type->nameInMessages(), " instance"_s);
}

bool setData(JSGlobalObject* globalObject, PyType* type, Accessor accessor, const Place& place, JSValue value, int64_t index, int64_t size, Bits bits)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue keep = storeData(globalObject, type, accessor, place, value, size, bits);
    RETURN_IF_EXCEPTION(scope, false);
    RELEASE_AND_RETURN(scope, keepAlive(globalObject, place.object, index, keep));
}

// ---- _CData ----

static JSValue dataNew(JSGlobalObject* globalObject, JSValue type, const CallArgs&)
{
    return newData(globalObject, PyType::from(type));
}

static JSValue dataHash(JSGlobalObject* globalObject, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseTypeError(globalObject, scope, "unhashable type"_s);
}

static void releaseExportOfView(JSGlobalObject*, JSValue object)
{
    auto* view = uncheckedDowncast<PyMemoryView>(PyCData::from(object)->owner());
    ASSERT(view->exports && view->managedBuffer());
    --view->exports;
    --view->managedBuffer()->bufferCount;
}

JSValue dataBuffer(JSGlobalObject* globalObject, JSValue self, JSValue flags)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* data = PyCData::from(self);
    PyCTypeInfo* info = infoOfInstance(globalObject, self);
    if (!info)
        return raiseTypeError(globalObject, scope, "abstract class"_s);
    PyCTypeInfo* itemInfo = info;
    while (itemInfo->kind == TypeKind::Array) {
        PyCTypeInfo* next = infoOf(globalObject, itemInfo->proto.get());
        if (!next)
            break;
        itemInfo = next;
    }

    Buffer buffer;
    buffer.object = self;
    buffer.length = data->size;
    buffer.itemSize = itemInfo->size;
    buffer.isReadOnly = false;
    buffer.format = info->format ? stringOf(info->format.get()) : "B"_str;
    PyTuple* shape = info->shape.get();
    buffer.dimensions = shape ? shape->length() : 0;
    CheckedInt64 sizeOfShape = buffer.itemSize;
    bool isEmpty = false;
    for (unsigned i = 0; i < buffer.dimensions; ++i) {
        buffer.shape.append(tryInt64(shape->at(i)).value_or(0));
        sizeOfShape *= buffer.shape.last();
        isEmpty |= !buffer.shape.last();
    }
    // The shape and the size of an item are the type's, as it is now; the memory is the instance's. If the type has outgrown the instance (S * 4, and S is given its fields
    // afterwards; __class__ is assigned to), the buffer is the bytes of the instance and no more is said of them.
    if (!isEmpty && (sizeOfShape.hasOverflowed() || sizeOfShape.value() > data->size)) {
        buffer.itemSize = 1;
        buffer.format = "B"_str;
        buffer.dimensions = 1;
        buffer.shape = { data->size };
    }

    if (data->isView()) {
        auto* view = uncheckedDowncast<PyMemoryView>(data->owner());
        if (view->isReleased())
            return raiseValueError(globalObject, scope, "operation forbidden on released memoryview object"_s);
        buffer.owner = view->managedBuffer()->owner();
        buffer.offset = view->offset + data->offsetInView();
        buffer.requiredSize = view->managedBuffer()->requiredSize;
        // It is a buffer taken from the view: as long as it is in use the view cannot be released, so whoever exported the memory goes on counting it as exported.
        ++view->exports;
        ++view->managedBuffer()->bufferCount;
        buffer.release = releaseExportOfView;
    } else if (JSCell* owner = data->owner(); owner && (owner->type() != PyBytesType || data->addressInStorage() + data->size < bytesOfOwner(owner).data() + bytesOfOwner(owner).size())) {
        buffer.owner = owner;
        buffer.offset = data->addressInStorage() - bytesOfOwner(owner).data();
        buffer.requiredSize = buffer.offset + data->size;
    } else {
        // Nothing is known about the memory, or it takes in the NUL behind a bytes object, in front of which the memory ends that a buffer can have of one. (The instance, which the buffer holds, keeps that alive.)
        // A buffer wants an owner: an ArrayBuffer that frees nothing.
        uint8_t* memory = data->memory(globalObject);
        if (!memory && data->size)
            return raiseValueError(globalObject, scope, "NULL pointer access"_s);
        if (static_cast<uint64_t>(data->size) > MAX_ARRAY_BUFFER_SIZE)
            return raiseBufferError(globalObject, scope, "the memory is too large to be exported"_s);
        auto arrayBuffer = ArrayBuffer::createFromBytes(std::span<const uint8_t> { memory, static_cast<size_t>(data->size) }, createSharedTask<void(void*)>([](void*) { }));
        buffer.owner = JSArrayBuffer::create(vm, globalObject->arrayBufferStructure(ArrayBufferSharingMode::Default), WTF::move(arrayBuffer));
        buffer.offset = 0;
        buffer.requiredSize = data->size;
    }
    scope.release();
    JSValue result = exportBuffer(globalObject, flags, buffer);
    // It raised before it had taken the buffer over.
    if (auto release = std::exchange(buffer.release, nullptr)) [[unlikely]]
        release(globalObject, self);
    return result;
}

static JSValue dataFromOutParam(JSGlobalObject*, JSValue self) { return self; }

static JSValue dataReduce(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* data = PyCData::from(self);
    PyCTypeInfo* info = infoOfInstance(globalObject, self);
    if (!info)
        return raiseTypeError(globalObject, scope, "abstract class"_s);
    if (info->flags & (IsPointer | HasPointer))
        return raiseValueError(globalObject, scope, "ctypes objects containing pointers cannot be pickled"_s);
    JSValue dict = getAttr(globalObject, self, rt.names.dDict());
    RETURN_IF_EXCEPTION(scope, { });
    auto memory = Place { data }.resolve(globalObject, data->size);
    RETURN_IF_EXCEPTION(scope, { });
    PyBytes* bytes = newBytes(globalObject, memory.size());
    RETURN_IF_EXCEPTION(scope, { });
    memcpy(bytes->mutableData(), memory.data(), memory.size());
    JSValue state = PyTuple::create(globalObject, { dict, bytes });
    JSValue arguments = PyTuple::create(globalObject, { data->type(), state });
    return PyTuple::create(globalObject, { info->state->unpickle.get(), arguments });
}

static JSValue dataSetState(JSGlobalObject* globalObject, JSValue self, JSValue dict, JSValue bytes)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* data = PyCData::from(self);
    if (!isDict(dict))
        return raiseTypeError(globalObject, scope, "__setstate__() argument 1 must be dict, not "_s, typeNameOfArgument(globalObject, dict));
    {
        ByteBuilder encoded;
        BufferHolder holder(globalObject);
        std::span<const uint8_t> source;
        if (isStr(bytes)) {
            StrView view(globalObject, strValue(bytes));
            RETURN_IF_EXCEPTION(scope, { });
            if (!appendUTF8(encoded, view.view()))
                return raiseValueError(globalObject, scope, "surrogates not allowed"_s);
            if (encoded.hasOverflowed()) [[unlikely]]
                return raiseMemoryError(globalObject, scope, String());
            source = encoded.span();
        } else {
            if (!hasBuffer(bytes))
                return raiseTypeError(globalObject, scope, "a bytes-like object is required, not '"_s, typeName(globalObject, bytes), '\'');
            bool ok = holder.get(bytes);
            RETURN_IF_EXCEPTION(scope, { });
            ASSERT_UNUSED(ok, ok);
            source = holder.buffer.span();
        }
        size_t count = std::min(source.size(), static_cast<size_t>(data->size));
        auto memory = Place { data }.resolve(globalObject, count);
        RETURN_IF_EXCEPTION(scope, { });
        memmove(memory.data(), source.data(), memory.size());
    }
    JSValue own = getAttr(globalObject, self, rt.names.dDict());
    RETURN_IF_EXCEPTION(scope, { });
    if (!isDict(own))
        return raiseTypeError(globalObject, scope, data->type()->nameInMessages(), ".__dict__ must be a dictionary, not "_s, typeName(globalObject, own));
    JSValue key;
    JSValue value;
    for (unsigned position = 0; PyDict::from(dict)->next(position, key, value);) {
        PyDict::from(own)->set(globalObject, key, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return none();
}

static JSValue dataBase(JSGlobalObject*, JSValue self)
{
    PyCData* base = PyCData::from(self)->base.get();
    return base ? JSValue(base) : none();
}
static JSValue dataNeedsFree(JSGlobalObject*, JSValue self) { return jsNumber(PyCData::from(self)->ownsMemory ? 1 : 0); }
static JSValue dataObjects(JSGlobalObject*, JSValue self)
{
    JSValue objects = PyCData::from(self)->objects.get();
    return objects ? objects : none();
}

static constexpr NativeDef dataMethods[] = {
    PY_SLOT("__hash__", dataHash, "($self, /)"),
    PY_SLOT("__buffer__", dataBuffer, "($self, flags, /)"),
    PY_METHOD("__ctypes_from_outparam__", dataFromOutParam, "($self, /)"),
    PY_METHOD("__setstate__", dataSetState, "($self, dict, data, /)"),
    PY_METHOD("__reduce__", dataReduce, "($self, /)"),
};

static constexpr NativeGetSet dataGetSets[] = {
    PY_MEMBER("_b_base_", dataBase),
    PY_MEMBER("_b_needsfree_", dataNeedsFree),
    PY_MEMBER("_objects", dataObjects),
};

const TypeDef dataTypeDef { "_ctypes._CData"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::Immutable, TypeFlag::DisallowInstantiation }, dataMethods, dataGetSets, "XXX to be provided"_s };

// ---- _SimpleCData ----

static JSValue simpleGetValue(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* data = PyCData::from(self);
    PyCTypeInfo* info = infoOfInstance(globalObject, self);
    if (!info || !info->accessor)
        return raiseTypeError(globalObject, scope, "abstract class"_s);
    auto memory = Place { data }.resolve(globalObject, info->size);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue hint = info->accessor.code == Code::Object ? keptFor(globalObject, data, 0) : JSValue();
    RELEASE_AND_RETURN(scope, load(globalObject, info->accessor, memory, { }, hint));
}

static void simpleSetValue(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* data = PyCData::from(self);
    if (!value) {
        raiseTypeError(globalObject, scope, "can't delete attribute"_s);
        return;
    }
    PyCTypeInfo* info = infoOfInstance(globalObject, self);
    if (!info || !info->accessor) {
        raiseTypeError(globalObject, scope, "abstract class"_s);
        return;
    }
    Packed packed;
    bool ok = pack(globalObject, info->accessor, value, info->size, packed);
    RETURN_IF_EXCEPTION(scope, void());
    ASSERT_UNUSED(ok, ok);
    Home home;
    {
        auto memory = Place { data }.resolve(globalObject, info->size, isAddress(info->accessor.code) ? &home : nullptr);
        RETURN_IF_EXCEPTION(scope, void());
        store(memory, info->accessor, packed);
    }
    if (isAddress(info->accessor.code)) {
        didStoreAddress(globalObject, home, packed);
        RETURN_IF_EXCEPTION(scope, void());
    }
    RELEASE_AND_RETURN(scope, void(keepAlive(globalObject, data, 0, packed.keep, Kept::ForWhole)));
}

static JSValue simpleInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, checkPositional(globalObject, "__init__"_s, args, 0, 1)))
        return { };
    if (args.size()) {
        simpleSetValue(globalObject, self, args[0]);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return none();
}

static JSValue simpleFromOutParam(JSGlobalObject* globalObject, JSValue self)
{
    if (!isSimpleInstanceType(stateOf(globalObject), PyCData::from(self)->type()))
        return self;
    return simpleGetValue(globalObject, self);
}

static JSValue dataIsNonZero(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* data = PyCData::from(self);
    auto memory = Place { data }.resolve(globalObject, data->size);
    RETURN_IF_EXCEPTION(scope, { });
    for (uint8_t byte : memory) {
        if (byte)
            return jsBoolean(true);
    }
    return jsBoolean(false);
}

static JSValue simpleRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* data = PyCData::from(self);
    if (!isSimpleInstanceType(stateOf(globalObject), data->type())) {
        String address = addressForRepr(globalObject, self);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, makeStr(globalObject, makeString('<', data->type()->nameInMessages(), " object at "_s, address, '>')));
    }
    JSValue value = simpleGetValue(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    JSString* text = repr(globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(globalObject, makeString(data->type()->nameInMessages(), '(', stringOf(text), ')'));
}

static constexpr NativeDef simpleMethods[] = {
    PY_NEW(dataNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", simpleRepr, "($self, /)"),
    PY_SLOT("__init__", simpleInit, "($self, /, *args, **kwargs)"),
    PY_SLOT("__buffer__", dataBuffer, "($self, flags, /)"),
    PY_SLOT("__bool__", dataIsNonZero, "($self, /)"),
    PY_METHOD("__ctypes_from_outparam__", simpleFromOutParam, "($self, /)"),
};

static constexpr NativeGetSet simpleGetSets[] = {
    { "value"_s, simpleGetValue, simpleSetValue },
};

const TypeDef simpleTypeDef { "_ctypes._SimpleCData"_s, { TypeFlag::Base, TypeFlag::Immutable }, simpleMethods, simpleGetSets, { } };

// ---- Structure, Union ----

// Sets the fields of the class, after those of its bases, from the positional arguments. Returns how many fields there are, or -1 for an exception.
static int64_t structInitFromPositional(JSGlobalObject* globalObject, JSValue self, PyType* type, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = stateOf(globalObject);
    // The bases as they are now: what sets a field may run anything.
    MarkedArgumentBuffer classes;
    classes.append(type);
    while (type->base() && infoOf(globalObject, type->base())) {
        type = type->base();
        classes.append(type);
    }
    int64_t index = 0;
    while (!classes.isEmpty()) {
        type = PyType::from(classes.takeLast());
        PyCTypeInfo* info = infoOf(globalObject, type);
        if (!info)
            continue;
        JSValue fields = type->dict()->get(state.fieldsName.get());
        if (!fields)
            continue;
        for (int64_t i = index; i < info->length && i < static_cast<int64_t>(args.size()); ++i) {
            JSValue pair = getItem(globalObject, fields, makeInt(globalObject, i - index));
            RETURN_IF_EXCEPTION(scope, -1);
            JSValue name = getItem(globalObject, pair, jsNumber(0));
            RETURN_IF_EXCEPTION(scope, -1);
            JSString* attribute = attributeName(globalObject, name);
            RETURN_IF_EXCEPTION(scope, -1);
            if (args.hasKeywords() && args.keyword(attribute)) {
                String text = reprForMessage(globalObject, name);
                RETURN_IF_EXCEPTION(scope, -1);
                raiseTypeError(globalObject, scope, "duplicate values for field "_s, text);
                return -1;
            }
            setAttr(globalObject, self, attribute, args[static_cast<unsigned>(i)]);
            RETURN_IF_EXCEPTION(scope, -1);
        }
        index = info->length;
    }
    return index;
}

static JSValue structInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.size()) {
        int64_t count = structInitFromPositional(globalObject, self, PyCData::from(self)->type(), args);
        RETURN_IF_EXCEPTION(scope, { });
        if (count < static_cast<int64_t>(args.size()))
            return raiseTypeError(globalObject, scope, "too many initializers"_s);
    }
    PyTuple* keys = args.keywordCount() ? keywordKeys(globalObject, args.keywordNames()) : nullptr;
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        setAttrAsGiven(globalObject, self, args.keywordName(i), keys ? keys->at(i) : JSValue(args.keywordName(i)), args.keywordValue(i));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return none();
}

static constexpr NativeDef structMethods[] = {
    PY_NEW(dataNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", structInit, "($self, /, *args, **kwargs)"),
    PY_SLOT("__buffer__", dataBuffer, "($self, flags, /)"),
};

const TypeDef structTypeDef { "_ctypes.Structure"_s, { TypeFlag::Base, TypeFlag::Immutable }, structMethods, { }, "Structure base class"_s };
const TypeDef unionTypeDef { "_ctypes.Union"_s, { TypeFlag::Base, TypeFlag::Immutable }, structMethods, { }, "Union base class"_s };

// ---- Array ----

static JSValue arrayInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    for (unsigned i = 0; i < args.size(); ++i) {
        setItem(globalObject, self, jsNumber(i), args[i]);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return none();
}

static JSValue arrayItem(JSGlobalObject* globalObject, PyCData* array, int64_t index)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (index < 0 || index >= array->length)
        return raiseIndexError(globalObject, scope, "invalid index"_s);
    PyCTypeInfo* info = infoOfInstance(globalObject, array);
    if (!info || info->kind != TypeKind::Array || !info->length)
        return raiseIndexError(globalObject, scope, "invalid index"_s);
    int64_t size = info->size / info->length;
    RELEASE_AND_RETURN(scope, getData(globalObject, PyType::from(info->proto.get()), { }, { array, index * size }, index, size));
}

// The characters a slice selects, as bytes or str. `base`, with offset 0, is where item 0 is.
static JSValue sliceOfCharacters(JSGlobalObject* globalObject, Code code, Place base, int64_t start, int64_t step, int64_t count)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (count <= 0)
        return code == Code::Char ? JSValue(runtime(globalObject).emptyBytes()) : JSValue(jsEmptyString(vm));
    // From the first of them in memory to the last. (The arithmetic is that of C: what it comes to is looked at.)
    uint64_t itemSize = code == Code::Char ? 1 : sizeof(wchar_t);
    int64_t end = static_cast<int64_t>(static_cast<uint64_t>(start) + static_cast<uint64_t>(count - 1) * static_cast<uint64_t>(step));
    int64_t first = std::min(start, end);
    int64_t last = std::max(start, end);
    base.offset = static_cast<int64_t>(static_cast<uint64_t>(first) * itemSize);
    uint64_t distance = static_cast<uint64_t>(last) - static_cast<uint64_t>(first);
    if (distance >= std::numeric_limits<int64_t>::max() / itemSize)
        return raiseMemoryError(globalObject, scope, String());
    auto granted = base.resolve(globalObject, static_cast<int64_t>((distance + 1) * itemSize));
    RETURN_IF_EXCEPTION(scope, { });
    const uint8_t* memory = granted.data();
    start = start == first ? 0 : static_cast<int64_t>(distance);
    if (code == Code::Char) {
        PyBytes* result = newBytes(globalObject, static_cast<size_t>(count));
        RETURN_IF_EXCEPTION(scope, { });
        uint8_t* out = result->mutableData();
        for (int64_t i = 0, current = start; i < count; ++i, current += step)
            out[i] = memory[current];
        return result;
    }
    auto* text = reinterpret_cast<const wchar_t*>(memory);
    if (step == 1)
        RELEASE_AND_RETURN(scope, strFromWideCharacters(globalObject, text + start, static_cast<size_t>(count)));
    Vector<wchar_t> selected;
    if (!selected.tryReserveInitialCapacity(static_cast<size_t>(count)))
        return raiseMemoryError(globalObject, scope, String());
    for (int64_t i = 0, current = start; i < count; ++i, current += step)
        selected.append(text[current]);
    RELEASE_AND_RETURN(scope, strFromWideCharacters(globalObject, selected.span().data(), selected.size()));
}

static JSValue arrayGetItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* array = PyCData::from(self);
    if (hasIndex(globalObject, key)) {
        int64_t index = toSsize(globalObject, key, rt.IndexError());
        RETURN_IF_EXCEPTION(scope, { });
        if (index < 0)
            index += array->length;
        RELEASE_AND_RETURN(scope, arrayItem(globalObject, array, index));
    }
    auto* slice = PySlice::tryFrom(key);
    if (!slice)
        return raiseTypeError(globalObject, scope, "indices must be integers"_s);
    PySlice::Indices indices;
    bool ok = slice->indices(globalObject, array->length, indices);
    RETURN_IF_EXCEPTION(scope, { });
    ASSERT_UNUSED(ok, ok);
    PyCTypeInfo* info = infoOfInstance(globalObject, self);
    PyCTypeInfo* itemInfo = info ? infoOf(globalObject, info->proto.get()) : nullptr;
    if (!itemInfo)
        return raiseTypeError(globalObject, scope, "abstract class"_s);
    if (info->kind == TypeKind::Array && (itemInfo->accessor == Accessor { Code::Char } || itemInfo->accessor == Accessor { Code::WChar }))
        RELEASE_AND_RETURN(scope, sliceOfCharacters(globalObject, itemInfo->accessor.code, { array }, indices.start, indices.step, indices.length));
    JSArray* result = newList(globalObject, static_cast<unsigned>(indices.length));
    RETURN_IF_EXCEPTION(scope, { });
    for (int64_t i = 0, current = indices.start; i < indices.length; ++i, current += indices.step) {
        JSValue item = arrayItem(globalObject, array, current);
        RETURN_IF_EXCEPTION(scope, { });
        listAppend(globalObject, result, item);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

static bool arraySetItemAt(JSGlobalObject* globalObject, PyCData* array, int64_t index, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCTypeInfo* info = infoOfInstance(globalObject, array);
    if (!info || info->kind != TypeKind::Array || index < 0 || index >= info->length) {
        raiseIndexError(globalObject, scope, "invalid index"_s);
        return false;
    }
    int64_t size = info->size / info->length;
    RELEASE_AND_RETURN(scope, setData(globalObject, PyType::from(info->proto.get()), { }, { array, index * size }, value, index, size));
}

static JSValue arraySetItem(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* array = PyCData::from(self);
    if (hasIndex(globalObject, key)) {
        int64_t index = toSsize(globalObject, key, rt.IndexError());
        RETURN_IF_EXCEPTION(scope, { });
        if (index < 0)
            index += array->length;
        arraySetItemAt(globalObject, array, index, value);
        RETURN_IF_EXCEPTION(scope, { });
        return none();
    }
    auto* slice = PySlice::tryFrom(key);
    if (!slice)
        return raiseTypeError(globalObject, scope, "indices must be integer"_s);
    PySlice::Indices indices;
    bool ok = slice->indices(globalObject, array->length, indices);
    RETURN_IF_EXCEPTION(scope, { });
    ASSERT_UNUSED(ok, ok);
    int64_t otherLength = length(globalObject, value);
    // What has no length has not the right one.
    if (scope.exception())
        takePendingException(globalObject);
    if (otherLength != indices.length)
        return raiseValueError(globalObject, scope, "Can only assign sequence of same size"_s);
    for (int64_t i = 0, current = indices.start; i < otherLength; ++i, current += indices.step) {
        JSValue item = getItem(globalObject, value, makeInt(globalObject, i));
        RETURN_IF_EXCEPTION(scope, { });
        arraySetItemAt(globalObject, array, current, item);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return none();
}

static JSValue arrayDelItem(JSGlobalObject* globalObject, JSValue, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseTypeError(globalObject, scope, "Array does not support item deletion"_s);
}

static JSValue arrayLength(JSGlobalObject* globalObject, JSValue self) { return makeInt(globalObject, PyCData::from(self)->length); }

static constexpr NativeDef arrayMethods[] = {
    PY_NEW(dataNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", arrayInit, "($self, /, *args, **kwargs)"),
    PY_SLOT("__buffer__", dataBuffer, "($self, flags, /)"),
    PY_SLOT("__len__", arrayLength, "($self, /)"),
    NativeDef(NativeKind::SlotWrapper, "__getitem__"_s, arrayGetItem, "($self, key, /)"_s, ItemOfSequence),
    PY_SLOT("__setitem__", arraySetItem, "($self, key, value, /)"),
    PY_SLOT("__delitem__", arrayDelItem, "($self, key, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};

const TypeDef arrayTypeDef { "_ctypes.Array"_s, { TypeFlag::Base, TypeFlag::Immutable }, arrayMethods, { }, { } };

// What an array of c_char has besides.

static JSValue charArrayGetRaw(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* array = PyCData::from(self);
    auto memory = Place { array }.resolve(globalObject, array->size);
    RETURN_IF_EXCEPTION(scope, { });
    PyBytes* result = newBytes(globalObject, memory.size());
    RETURN_IF_EXCEPTION(scope, { });
    memcpy(result->mutableData(), memory.data(), memory.size());
    return result;
}

static void charArraySetRaw(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* array = PyCData::from(self);
    if (!value) {
        raise(globalObject, scope, runtime(globalObject).AttributeError(), makeString("attribute 'raw' of '"_s, array->type()->nameInMessages(), "' objects cannot be deleted"_s));
        return;
    }
    BufferHolder holder(globalObject);
    bool ok = holder.get(value);
    RETURN_IF_EXCEPTION(scope, void());
    ASSERT_UNUSED(ok, ok);
    if (holder.buffer.length > array->size) {
        raiseValueError(globalObject, scope, "byte string too long"_s);
        return;
    }
    auto memory = Place { array }.resolve(globalObject, holder.buffer.length);
    RETURN_IF_EXCEPTION(scope, void());
    memmove(memory.data(), holder.buffer.data, memory.size());
}

static JSValue charArrayGetValue(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* array = PyCData::from(self);
    auto memory = Place { array }.resolve(globalObject, array->size);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, load(globalObject, { Code::CharArray }, memory));
}

static void charArraySetValue(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* array = PyCData::from(self);
    if (!value) {
        raiseTypeError(globalObject, scope, "can't delete attribute"_s);
        return;
    }
    auto* bytes = PyBytes::tryFrom(value);
    if (!bytes) {
        raiseTypeError(globalObject, scope, "bytes expected instead of "_s, typeName(globalObject, value), " instance"_s);
        return;
    }
    size_t size = bytes->length();
    if (size > static_cast<uint64_t>(array->size)) {
        raiseValueError(globalObject, scope, "byte string too long"_s);
        return;
    }
    auto memory = Place { array }.resolve(globalObject, array->size);
    RETURN_IF_EXCEPTION(scope, void());
    memcpy(memory.data(), bytes->data(), size);
    if (size < memory.size())
        memory[size] = 0;
}

static JSValue wcharArrayGetValue(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* array = PyCData::from(self);
    auto memory = Place { array }.resolve(globalObject, array->size);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, load(globalObject, { Code::WCharArray }, memory));
}

static void wcharArraySetValue(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* array = PyCData::from(self);
    if (!value) {
        raiseTypeError(globalObject, scope, "can't delete attribute"_s);
        return;
    }
    if (!isStr(value)) {
        raiseTypeError(globalObject, scope, "unicode string expected instead of "_s, typeName(globalObject, value), " instance"_s);
        return;
    }
    size_t length;
    PyByteStorage* storage = wideCharactersOf(globalObject, strValue(value), length);
    RETURN_IF_EXCEPTION(scope, void());
    size_t room = static_cast<size_t>(array->size) / sizeof(wchar_t);
    if (length > room) {
        raiseValueError(globalObject, scope, "string too long"_s);
        return;
    }
    auto memory = Place { array }.resolve(globalObject, array->size);
    RETURN_IF_EXCEPTION(scope, void());
    memcpy(memory.data(), storage->data(), (length < room ? length + 1 : length) * sizeof(wchar_t));
    ensureStillAliveHere(storage);
}

static constexpr NativeGetSet charArrayGetSetTable[] = {
    { "raw"_s, charArrayGetRaw, charArraySetRaw, false, "value"_s },
    { "value"_s, charArrayGetValue, charArraySetValue, false, "string value"_s },
};

static constexpr NativeGetSet wcharArrayGetSetTable[] = {
    { "value"_s, wcharArrayGetValue, wcharArraySetValue, false, "string value"_s },
};

std::span<const NativeGetSet> charArrayGetSets() { return charArrayGetSetTable; }
std::span<const NativeGetSet> wcharArrayGetSets() { return wcharArrayGetSetTable; }

// ---- _Pointer ----

// The type of what it points to, with what that knows. False: an exception.
static bool pointerItemType(JSGlobalObject* globalObject, PyCData* pointer, PyType*& type, PyCTypeInfo*& itemInfo)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCTypeInfo* info = infoOfInstance(globalObject, pointer);
    type = info ? PyType::tryFrom(info->proto.get()) : nullptr;
    itemInfo = type ? infoOf(globalObject, type) : nullptr;
    if (!itemInfo) {
        // (CPython looks at the pointer first.)
        uint8_t* target;
        if (!failed(scope, pointerTarget(globalObject, pointer, target)))
            raiseTypeError(globalObject, scope, "abstract class"_s);
        return false;
    }
    return true;
}

// (The arithmetic is that of C: what it comes to is looked at when the place is asked for.)
static ALWAYS_INLINE Place placeOfItem(PyCData* pointer, int64_t index, PyCTypeInfo* itemInfo)
{
    return { pointer, static_cast<int64_t>(static_cast<uint64_t>(index) * static_cast<uint64_t>(itemInfo->size)), true };
}

static JSValue pointerItem(JSGlobalObject* globalObject, PyCData* pointer, int64_t index)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type;
    PyCTypeInfo* itemInfo;
    if (failed(scope, pointerItemType(globalObject, pointer, type, itemInfo)))
        return { };
    RELEASE_AND_RETURN(scope, getData(globalObject, type, { }, placeOfItem(pointer, index, itemInfo), index, itemInfo->size));
}

static JSValue pointerGetItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* pointer = PyCData::from(self);
    if (hasIndex(globalObject, key)) {
        int64_t index = toSsize(globalObject, key, rt.IndexError());
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, pointerItem(globalObject, pointer, index));
    }
    auto* slice = PySlice::tryFrom(key);
    if (!slice)
        return raiseTypeError(globalObject, scope, "Pointer indices must be integer"_s);
    // A pointer has no length, so nothing counts from the end.
    int64_t step = 1;
    if (!isNone(slice->step())) {
        step = toSsize(globalObject, slice->step(), rt.ValueError());
        RETURN_IF_EXCEPTION(scope, { });
        if (!step)
            return raiseValueError(globalObject, scope, "slice step cannot be zero"_s);
    }
    int64_t start = 0;
    if (isNone(slice->start())) {
        if (step < 0)
            return raiseValueError(globalObject, scope, "slice start is required for step < 0"_s);
    } else {
        start = toSsize(globalObject, slice->start(), rt.ValueError());
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (isNone(slice->stop()))
        return raiseValueError(globalObject, scope, "slice stop is required"_s);
    int64_t stop = toSsize(globalObject, slice->stop(), rt.ValueError());
    RETURN_IF_EXCEPTION(scope, { });
    // CPython's arithmetic, in a type that holds what it comes to.
    auto countIn = [&]<typename Integer>(Integer first) { return (stop - first + (step > 0 ? -1 : 1)) / step + 1; };
    constexpr int64_t farOut = 1LL << 61;
    int64_t count = 0;
    if ((step > 0 && start > stop) || (step < 0 && start < stop))
        count = 0;
    else if (start > -farOut && start < farOut && stop > -farOut && stop < farOut) [[likely]]
        count = countIn(start);
    else {
        Int128 wide = countIn(static_cast<Int128>(start));
        if (wide > std::numeric_limits<int64_t>::max())
            return raiseMemoryError(globalObject, scope, String());
        count = static_cast<int64_t>(wide);
    }

    PyType* type;
    PyCTypeInfo* itemInfo;
    if (failed(scope, pointerItemType(globalObject, pointer, type, itemInfo)))
        return { };
    if (itemInfo->accessor == Accessor { Code::Char } || itemInfo->accessor == Accessor { Code::WChar })
        RELEASE_AND_RETURN(scope, sliceOfCharacters(globalObject, itemInfo->accessor.code, { pointer, 0, true }, start, step, count));
    if (count > std::numeric_limits<int32_t>::max())
        return raiseMemoryError(globalObject, scope, String());
    JSArray* result = newList(globalObject, static_cast<unsigned>(count));
    RETURN_IF_EXCEPTION(scope, { });
    for (int64_t i = 0, current = start; i < count; ++i, current += step) {
        JSValue item = pointerItem(globalObject, pointer, current);
        RETURN_IF_EXCEPTION(scope, { });
        listAppend(globalObject, result, item);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

static JSValue pointerSetItem(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* pointer = PyCData::from(self);
    int64_t index = toSsize(globalObject, key, rt.IndexError());
    RETURN_IF_EXCEPTION(scope, { });
    uint8_t* target;
    PyType* type;
    PyCTypeInfo* itemInfo;
    if (failed(scope, pointerTarget(globalObject, pointer, target)) || failed(scope, pointerItemType(globalObject, pointer, type, itemInfo)))
        return { };
    // Converting the value may run code that makes the pointer point elsewhere, or frees what it points to: where the value goes is looked at behind that.
    setData(globalObject, type, { }, placeOfItem(pointer, index, itemInfo), value, index, itemInfo->size);
    RETURN_IF_EXCEPTION(scope, { });
    return none();
}

static JSValue pointerDelItem(JSGlobalObject* globalObject, JSValue, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseTypeError(globalObject, scope, "Pointer does not support item deletion"_s);
}

static JSValue pointerGetContents(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* pointer = PyCData::from(self);
    PyType* type;
    PyCTypeInfo* itemInfo;
    if (failed(scope, pointerItemType(globalObject, pointer, type, itemInfo)))
        return { };
    RELEASE_AND_RETURN(scope, dataAt(globalObject, type, { pointer, 0, true }, 0));
}

static void pointerSetContents(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCData* pointer = PyCData::from(self);
    if (!value) {
        raiseTypeError(globalObject, scope, "Pointer does not support item deletion"_s);
        return;
    }
    PyCTypeInfo* info = infoOfInstance(globalObject, self);
    PyCData* target = PyCData::tryFrom(value);
    if (!info || !target) {
        PyType* proto = info ? PyType::tryFrom(info->proto.get()) : nullptr;
        raiseTypeError(globalObject, scope, "expected "_s, proto ? proto->nameInMessages() : "ctypes"_str, " instead of "_s, typeName(globalObject, value));
        return;
    }
    uint8_t* address = target->memory(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    Home home;
    {
        auto memory = Place { pointer }.resolve(globalObject, sizeof(void*), &home);
        RETURN_IF_EXCEPTION(scope, void());
        memcpy(memory.data(), &address, memory.size());
    }
    setEntry(globalObject, home, entryFor(target));
    RETURN_IF_EXCEPTION(scope, void());
    // What it points to has to live, and so has what that keeps alive.
    if (failed(scope, keepAlive(globalObject, pointer, 1, value)))
        return;
    RELEASE_AND_RETURN(scope, void(keepAlive(globalObject, pointer, 0, keptObjects(globalObject, target))));
}

static JSValue pointerNew(JSGlobalObject* globalObject, JSValue type, const CallArgs&)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyCTypeInfo* info = infoOf(globalObject, type);
    if (!info || !info->proto)
        return raiseTypeError(globalObject, scope, "Cannot create instance: has no _type_"_s);
    RELEASE_AND_RETURN(scope, newData(globalObject, PyType::from(type)));
}

static JSValue pointerInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, checkPositional(globalObject, "POINTER"_s, args, 0, 1)))
        return { };
    if (args.size()) {
        pointerSetContents(globalObject, self, args[0]);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return none();
}

static JSValue pointerIsNotNull(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto memory = Place { PyCData::from(self) }.resolve(globalObject, sizeof(void*));
    RETURN_IF_EXCEPTION(scope, { });
    void* target;
    memcpy(&target, memory.data(), sizeof(void*));
    return jsBoolean(!!target);
}

static constexpr NativeDef pointerMethods[] = {
    PY_NEW(pointerNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", pointerInit, "($self, /, *args, **kwargs)"),
    PY_SLOT("__buffer__", dataBuffer, "($self, flags, /)"),
    PY_SLOT("__bool__", pointerIsNotNull, "($self, /)"),
    NativeDef(NativeKind::SlotWrapper, "__getitem__"_s, pointerGetItem, "($self, key, /)"_s, ItemOfSequence),
    PY_SLOT("__setitem__", pointerSetItem, "($self, key, value, /)"),
    PY_SLOT("__delitem__", pointerDelItem, "($self, key, /)"),
};

static constexpr NativeGetSet pointerGetSets[] = {
    { "contents"_s, pointerGetContents, pointerSetContents, false, "the object this pointer points to (read-write)"_s },
};

const TypeDef pointerTypeDef { "_ctypes._Pointer"_s, { TypeFlag::Base, TypeFlag::Immutable }, pointerMethods, pointerGetSets, { } };

} // namespace Ctypes
} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
