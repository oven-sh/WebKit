/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
#include "PythonPosixModule.h"

#if OS(UNIX)

#include "JSCInlines.h"
#include "JSGenericTypedArrayViewInlines.h"
#include "PyObjects.h"
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonOperations.h"
#include "PythonPosix.h"
#include "PythonSignatures.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wtf/Box.h>
#include <wtf/SharedTask.h>

// The module mmap: Modules/mmapmodule.c of CPython. Like posix, it is for whoever embeds the engine to say whether a program is to have it.
//
// What is mapped is an ArrayBuffer's, which is how anything here has bytes to show: see NativeState::exportedBytes(). So a memoryview of it is of the pages themselves. To close it is to detach the ArrayBuffer, and then
// whatever was looking at it has nothing to look at. CPython counts who is looking, and will not close or resize while anybody is. Nobody is counted here, of this or of a bytearray, since when a memoryview is done with is for the collector to find.

#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

namespace JSC { namespace Python {

namespace {

using Kind = PyNativeFunction::Kind;

constexpr int64_t most = std::numeric_limits<int64_t>::max(); // PY_SSIZE_T_MAX

// access_mode
enum class Access : int {
    Default,
    Read,
    Write,
    Copy,
};

// An mmap_object
struct MmapState final : NativeState {
    PYTHON_NATIVE_STATE(MmapState);
    ~MmapState() final
    {
        // What is mapped goes with the ArrayBuffer.
        if (descriptor >= 0)
            ::close(descriptor);
    }

    std::optional<ExportedBytes> exportedBytes() const final
    {
        if (!storage)
            return std::nullopt;
        return ExportedBytes { storage.get(), 'B', 1, access == Access::Read };
    }
    void willExportBytes(JSGlobalObject* globalObject) const final
    {
        auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
        if (!data)
            raiseValueError(globalObject, scope, "mmap closed or invalid"_s);
    }

    // mmap_item(): a bytes of the one, where m[i] is an int.
    JSValue sequenceItem(JSGlobalObject* globalObject, int64_t index) const final
    {
        auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
        if (!data)
            return raiseValueError(globalObject, scope, "mmap closed or invalid"_s);
        if (index < 0 || index >= size)
            return raise(globalObject, scope, BuiltinType::IndexError, "mmap index out of range"_s);
        return newBytes(globalObject, bytes().subspan(static_cast<size_t>(index), 1));
    }

    std::span<uint8_t> bytes() const { return std::span(data, static_cast<size_t>(size)); }

    WriteBarrier<JSUint8Array> storage;
    Box<bool> pagesHaveMoved; // mremap() has them somewhere else, so that they are not the ArrayBuffer's to unmap.
    uint8_t* data { nullptr }; // Null once it is closed
    int64_t size { 0 };
    int64_t position { 0 }; // From where the mapping begins
    off_t offset { 0 };
    int descriptor { -1 };
    int flags { 0 };
    Access access { Access::Default };
    bool tracksDescriptor { true };
};

template<typename Visitor> void MmapState::visit(Visitor& visitor) { visitor.append(storage); }

// The pages, as something that can be shown. They are unmapped when the ArrayBuffer is detached or freed.
void adopt(JSGlobalObject* globalObject, JSCell* owner, MmapState& self, void* address, int64_t size)
{
    VM& vm = globalObject->vm();
    self.data = static_cast<uint8_t*>(address);
    self.size = size;
    size_t length = static_cast<size_t>(size);
    self.pagesHaveMoved = Box<bool>::create(false);
    auto buffer = ArrayBuffer::createFromBytes(std::span<const uint8_t>(self.data, length), createSharedTask<void(void*)>([length, haveMoved = self.pagesHaveMoved](void* pages) {
        if (!*haveMoved)
            munmap(pages, length);
    }));
    self.storage.set(vm, owner, JSUint8Array::create(globalObject, globalObject->pyRealm()->structureFor(BuiltinType::ByteArray), WTF::move(buffer), 0, length));
}

// Nothing is mapped any longer, and whatever was looking at it has nothing to look at.
void unmap(VM& vm, MmapState& self)
{
    // It still has bytes to show, as far as anybody can tell who does not ask for them: see willExportBytes().
    self.data = nullptr;
    if (JSUint8Array* storage = self.storage.get(); storage && !storage->isDetached())
        storage->possiblySharedBuffer()->detach(vm);
}

// CHECK_VALID(). False if it raised.
bool checkValid(JSGlobalObject* globalObject, ThrowScope& scope, const MmapState& self)
{
    if (self.data)
        return true;
    raiseValueError(globalObject, scope, "mmap closed or invalid"_s);
    return false;
}

// is_writable(). False if it raised.
bool checkWritable(JSGlobalObject* globalObject, ThrowScope& scope, const MmapState& self)
{
    if (self.access != Access::Read)
        return true;
    raiseTypeError(globalObject, scope, "mmap can't modify a readonly memory map."_s);
    return false;
}

#define MMAP_PROLOGUE() \
    NATIVE_PROLOGUE(); \
    MmapState& self = stateOf<MmapState>(args[0]); \
    if (!checkValid(globalObject, scope, self)) \
        return { }

// _PyIndex_Check()
bool hasIndex(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    return value.isBoolean() || isInt(value) || typeOf(globalObject, value)->lookup(vm, vm.pythonNames().dunder_index);
}

} // anonymous namespace

PYTHON_NATIVE(mmapClose)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    MmapState& self = stateOf<MmapState>(args[0]);
    if (int descriptor = std::exchange(self.descriptor, -1); descriptor >= 0)
        ::close(descriptor);
    unmap(vm, self);
    RETURN_NONE();
}

PYTHON_NATIVE(mmapReadByte)
{
    MMAP_PROLOGUE();
    if (self.position >= self.size)
        return JSValue::encode(raiseValueError(globalObject, scope, "read byte out of range"_s));
    return JSValue::encode(jsNumber(self.data[self.position++]));
}

PYTHON_NATIVE(mmapReadLine)
{
    MMAP_PROLOGUE();
    auto rest = self.bytes().subspan(static_cast<size_t>(std::min(self.position, self.size)));
    size_t newline = find(std::span<const uint8_t>(rest), static_cast<uint8_t>('\n'));
    auto line = newline == notFound ? rest : rest.first(newline + 1);
    JSValue result = newBytes(globalObject, line);
    self.position += line.size();
    return JSValue::encode(result);
}

PYTHON_NATIVE(mmapRead)
{
    NATIVE_PROLOGUE();
    MmapState& self = stateOf<MmapState>(args[0]);
    // _Py_convert_optional_to_ssize_t()
    int64_t count = most;
    if (args.size() > 1 && !isNone(args[1])) {
        if (!hasIndex(globalObject, args[1]))
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("argument should be integer or None, not '"_s, typeName(globalObject, args[1]), '\'')));
        auto converted = toIndexOrOverflow(globalObject, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        count = *converted;
    }
    if (!checkValid(globalObject, scope, self))
        return { };
    // What is out of range is put right and nothing said.
    int64_t remaining = self.position < self.size ? self.size - self.position : 0;
    if (count < 0 || count > remaining)
        count = remaining;
    JSValue result = newBytes(globalObject, self.bytes().subspan(static_cast<size_t>(self.size - remaining), static_cast<size_t>(count)));
    self.position += count;
    return JSValue::encode(result);
}

// mmap_gfind_lock_held()
PYTHON_NATIVE(mmapFind)
{
    bool reverse = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    MmapState& self = stateOf<MmapState>(args[0]);
    Buffer needle = bufferOf(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!checkValid(globalObject, scope, self))
        return { };
    int64_t start = self.position;
    int64_t end = self.size;
    if (args.size() > 2 && !isNone(args[2])) {
        auto converted = toSsize(globalObject, args[2]);
        RETURN_IF_EXCEPTION(scope, { });
        start = *converted;
        if (args.size() > 3 && !isNone(args[3])) {
            converted = toSsize(globalObject, args[3]);
            RETURN_IF_EXCEPTION(scope, { });
            end = *converted;
        }
    }
    // What was asked where to begin may have closed it.
    if (!checkValid(globalObject, scope, self))
        return { };
    auto clamp = [&] (int64_t where) {
        if (where < 0)
            where += self.size;
        return std::clamp<int64_t>(where, 0, self.size);
    };
    start = clamp(start);
    end = clamp(end);
    if (end < start)
        return JSValue::encode(jsNumber(-1));
    std::span<const uint8_t> haystack = self.bytes().subspan(static_cast<size_t>(start), static_cast<size_t>(end - start));
    std::span<const uint8_t> wanted = needle.span();
    size_t found = notFound;
    if (wanted.size() <= haystack.size()) {
        if (wanted.empty())
            found = reverse ? haystack.size() : 0;
        else if (reverse) {
            for (size_t i = haystack.size() - wanted.size() + 1; i--;) {
                if (equalSpans(haystack.subspan(i, wanted.size()), wanted)) {
                    found = i;
                    break;
                }
            }
        } else if (void* where = memmem(haystack.data(), haystack.size(), wanted.data(), wanted.size()))
            found = static_cast<const uint8_t*>(where) - haystack.data();
    }
    return JSValue::encode(found == notFound ? jsNumber(-1) : intFromInt64(globalObject, start + static_cast<int64_t>(found)));
}

PYTHON_NATIVE(mmapWrite)
{
    NATIVE_PROLOGUE();
    MmapState& self = stateOf<MmapState>(args[0]);
    Buffer data = bufferOf(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!checkValid(globalObject, scope, self) || !checkWritable(globalObject, scope, self))
        return { };
    int64_t length = static_cast<int64_t>(data.size());
    if (self.position > self.size || self.size - self.position < length)
        return JSValue::encode(raiseValueError(globalObject, scope, "data out of range"_s));
    // It may be some of itself that it is given.
    memmove(self.data + self.position, data.data(), data.size());
    self.position += length;
    return JSValue::encode(intFromInt64(globalObject, length));
}

PYTHON_NATIVE(mmapWriteByte)
{
    NATIVE_PROLOGUE();
    MmapState& self = stateOf<MmapState>(args[0]);
    // The `unsigned_char` of Argument Clinic
    auto value = toCLong(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (*value < 0)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "unsigned byte integer is less than minimum"_s));
    if (*value > 255)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "unsigned byte integer is greater than maximum"_s));
    if (!checkValid(globalObject, scope, self) || !checkWritable(globalObject, scope, self))
        return { };
    if (self.position >= self.size)
        return JSValue::encode(raiseValueError(globalObject, scope, "write byte out of range"_s));
    self.data[self.position++] = static_cast<uint8_t>(*value);
    RETURN_NONE();
}

PYTHON_NATIVE(mmapSize)
{
    MMAP_PROLOGUE();
    struct stat status;
    if (fstat(self.descriptor, &status) == -1)
        return JSValue::encode(raiseOSError(globalObject, scope, errno));
    return JSValue::encode(intFromInt64(globalObject, status.st_size));
}

PYTHON_NATIVE(mmapResize)
{
    NATIVE_PROLOGUE();
    MmapState& self = stateOf<MmapState>(args[0]);
    auto converted = toSsize(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t newSize = *converted;
    if (!checkValid(globalObject, scope, self))
        return { };
    // is_resizeable()
    if (!self.tracksDescriptor)
        return JSValue::encode(raiseValueError(globalObject, scope, "mmap can't resize with trackfd=False."_s));
    if (self.access != Access::Write && self.access != Access::Default)
        return JSValue::encode(raiseTypeError(globalObject, scope, "mmap can't resize a readonly or copy-on-write memory map."_s));
    if (newSize < 0 || most - newSize < self.offset)
        return JSValue::encode(raiseValueError(globalObject, scope, "new size out of range"_s));
#if OS(LINUX)
    // mremap() will not make longer what is shared and is of no file.
    if (self.descriptor == -1 && !(self.flags & MAP_PRIVATE) && newSize > self.size)
        return JSValue::encode(raiseValueError(globalObject, scope, "mmap: can't expand a shared anonymous mapping"_s));
    if (self.descriptor != -1 && ftruncate(self.descriptor, self.offset + newSize) == -1)
        return JSValue::encode(raiseOSError(globalObject, scope, errno));
    // The pages may be somewhere else afterwards, and an ArrayBuffer's are where they are. So the old one gives them up without unmapping them, and there is a new one.
    uint8_t* old = self.data;
    int64_t oldSize = self.size;
    void* moved = mremap(old, static_cast<size_t>(oldSize), static_cast<size_t>(newSize), MREMAP_MAYMOVE);
    if (moved == MAP_FAILED)
        return JSValue::encode(raiseOSError(globalObject, scope, errno));
    *self.pagesHaveMoved = true;
    unmap(vm, self);
    adopt(globalObject, args[0].asCell(), self, moved, newSize);
    RETURN_NONE();
#else
    return JSValue::encode(raise(globalObject, scope, BuiltinType::SystemError, "mmap: resizing not available--no mremap()"_s));
#endif
}

PYTHON_NATIVE(mmapTell)
{
    MMAP_PROLOGUE();
    return JSValue::encode(intFromInt64(globalObject, self.position));
}

PYTHON_NATIVE(mmapFlush)
{
    NATIVE_PROLOGUE();
    MmapState& self = stateOf<MmapState>(args[0]);
    int64_t offset = 0;
    if (args.size() > 1) {
        auto converted = toSsize(globalObject, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        offset = *converted;
    }
    if (!checkValid(globalObject, scope, self))
        return { };
    int64_t size = self.size;
    if (args.size() > 2 && !isNone(args[2])) {
        auto converted = toSsize(globalObject, args[2]);
        RETURN_IF_EXCEPTION(scope, { });
        size = *converted;
        if (!checkValid(globalObject, scope, self))
            return { };
    }
    if (size < 0 || offset < 0 || self.size - offset < size)
        return JSValue::encode(raiseValueError(globalObject, scope, "flush values out of range"_s));
    if (self.access == Access::Read || self.access == Access::Copy)
        RETURN_NONE();
    if (msync(self.data + offset, static_cast<size_t>(size), MS_SYNC) == -1)
        return JSValue::encode(raiseOSError(globalObject, scope, errno));
    RETURN_NONE();
}

PYTHON_NATIVE(mmapSeek)
{
    NATIVE_PROLOGUE();
    MmapState& self = stateOf<MmapState>(args[0]);
    auto distance = toSsize(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    int how = 0;
    if (args.size() > 2) {
        auto converted = toCInt(globalObject, args[2]);
        RETURN_IF_EXCEPTION(scope, { });
        how = *converted;
    }
    if (!checkValid(globalObject, scope, self))
        return { };
    int64_t base;
    switch (how) {
    case 0:
        base = 0;
        break;
    case 1:
        base = self.position;
        break;
    case 2:
        base = self.size;
        break;
    default:
        return JSValue::encode(raiseValueError(globalObject, scope, "unknown seek type"_s));
    }
    if (most - base < *distance || base + *distance > self.size || base + *distance < 0)
        return JSValue::encode(raiseValueError(globalObject, scope, "seek out of range"_s));
    self.position = base + *distance;
    return JSValue::encode(intFromInt64(globalObject, self.position));
}

PYTHON_NATIVE(mmapSeekable)
{
    UNUSED_PARAM(globalObject);
    UNUSED_PARAM(callFrame);
    return JSValue::encode(jsBoolean(true));
}

PYTHON_NATIVE(mmapMove)
{
    NATIVE_PROLOGUE();
    MmapState& self = stateOf<MmapState>(args[0]);
    int64_t values[3];
    for (unsigned i = 0; i < 3; ++i) {
        auto converted = toSsize(globalObject, args[i + 1]);
        RETURN_IF_EXCEPTION(scope, { });
        values[i] = *converted;
    }
    auto [destination, source, count] = values;
    if (!checkValid(globalObject, scope, self) || !checkWritable(globalObject, scope, self))
        return { };
    if (destination < 0 || source < 0 || count < 0 || self.size - destination < count || self.size - source < count)
        return JSValue::encode(raiseValueError(globalObject, scope, "source, destination, or count out of range"_s));
    memmove(self.data + destination, self.data + source, static_cast<size_t>(count));
    RETURN_NONE();
}

PYTHON_NATIVE(mmapEnter)
{
    MMAP_PROLOGUE();
    return JSValue::encode(args[0]);
}

PYTHON_NATIVE(mmapRepr)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    MmapState& self = stateOf<MmapState>(args[0]);
    String name = typeOf(globalObject, args[0])->nameString(globalObject);
    if (!self.data)
        return JSValue::encode(jsString(vm, concatenate('<', name, " closed=True>"_s)));
    static constexpr ASCIILiteral accessNames[] = { "ACCESS_DEFAULT"_s, "ACCESS_READ"_s, "ACCESS_WRITE"_s, "ACCESS_COPY"_s };
    return JSValue::encode(jsString(vm, concatenate('<', name, " closed=False, access="_s, accessNames[static_cast<int>(self.access)], ", length="_s, self.size, ", pos="_s, self.position, ", offset="_s, static_cast<int64_t>(self.offset), '>')));
}

PYTHON_NATIVE(mmapAdvise)
{
    NATIVE_PROLOGUE();
    MmapState& self = stateOf<MmapState>(args[0]);
    auto option = toCInt(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t start = 0;
    if (args.size() > 2) {
        auto converted = toSsize(globalObject, args[2]);
        RETURN_IF_EXCEPTION(scope, { });
        start = *converted;
    }
    if (!checkValid(globalObject, scope, self))
        return { };
    int64_t length = self.size;
    if (args.size() > 3 && !isNone(args[3])) {
        auto converted = toSsize(globalObject, args[3]);
        RETURN_IF_EXCEPTION(scope, { });
        length = *converted;
        if (!checkValid(globalObject, scope, self))
            return { };
    }
    if (start < 0 || start >= self.size)
        return JSValue::encode(raiseValueError(globalObject, scope, "madvise start out of bounds"_s));
    if (length < 0)
        return JSValue::encode(raiseValueError(globalObject, scope, "madvise length invalid"_s));
    if (most - start < length)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "madvise length too large"_s));
    if (start + length > self.size)
        length = self.size - start;
    if (madvise(self.data + start, static_cast<size_t>(length), *option))
        return JSValue::encode(raiseOSError(globalObject, scope, errno));
    RETURN_NONE();
}

PYTHON_NATIVE(mmapLength)
{
    MMAP_PROLOGUE();
    return JSValue::encode(intFromInt64(globalObject, self.size));
}

PYTHON_NATIVE(mmapGetItem)
{
    MMAP_PROLOGUE();
    JSValue item = args[1];
    if (PySlice* slice = trySlice(item)) {
        auto bounds = slice->unpack(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (!checkValid(globalObject, scope, self))
            return { };
        auto indices = PySlice::adjust(*bounds, self.size);
        if (indices.length <= 0)
            return JSValue::encode(newBytes(globalObject, std::span<const uint8_t>()));
        if (indices.step == 1)
            return JSValue::encode(newBytes(globalObject, self.bytes().subspan(static_cast<size_t>(indices.start), static_cast<size_t>(indices.length))));
        Vector<uint8_t> result;
        if (!result.tryGrow(static_cast<size_t>(indices.length)))
            return JSValue::encode(raiseMemoryError(globalObject, scope));
        for (int64_t i = 0, from = indices.start; i < indices.length; ++i, from += indices.step)
            result[i] = self.data[from];
        return JSValue::encode(newBytes(globalObject, result.span()));
    }
    if (!hasIndex(globalObject, item))
        return JSValue::encode(raiseTypeError(globalObject, scope, "mmap indices must be integers"_s));
    auto index = toIndex(globalObject, item);
    RETURN_IF_EXCEPTION(scope, { });
    if (!checkValid(globalObject, scope, self))
        return { };
    int64_t i = *index < 0 ? *index + self.size : *index;
    if (i < 0 || i >= self.size)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "mmap index out of range"_s));
    return JSValue::encode(jsNumber(self.data[i]));
}

namespace {

// mmap_ass_subscript_lock_held(). `value` is empty to delete.
EncodedJSValue setOrDeleteItem(JSGlobalObject* globalObject, ThrowScope& scope, MmapState& self, JSValue item, JSValue value)
{
    if (!checkWritable(globalObject, scope, self))
        return { };
    if (PySlice* slice = trySlice(item)) {
        auto bounds = slice->unpack(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (!value)
            return JSValue::encode(raiseTypeError(globalObject, scope, "mmap object doesn't support slice deletion"_s));
        Buffer data = bufferOf(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        // Either may have closed it, and then there is no saying how long it is.
        if (!checkValid(globalObject, scope, self))
            return { };
        auto indices = PySlice::adjust(*bounds, self.size);
        if (static_cast<int64_t>(data.size()) != indices.length)
            return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "mmap slice assignment is wrong size"_s));
        if (indices.step == 1)
            memmove(self.data + indices.start, data.data(), data.size());
        else {
            auto source = data.span();
            for (int64_t i = 0, to = indices.start; i < indices.length; ++i, to += indices.step)
                self.data[to] = source[i];
        }
        RETURN_NONE();
    }
    if (!hasIndex(globalObject, item))
        return JSValue::encode(raiseTypeError(globalObject, scope, "mmap indices must be integer"_s));
    auto index = toIndex(globalObject, item);
    RETURN_IF_EXCEPTION(scope, { });
    if (!checkValid(globalObject, scope, self))
        return { };
    int64_t i = *index < 0 ? *index + self.size : *index;
    if (i < 0 || i >= self.size)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "mmap index out of range"_s));
    if (!value)
        return JSValue::encode(raiseTypeError(globalObject, scope, "mmap doesn't support item deletion"_s));
    if (!hasIndex(globalObject, value))
        return JSValue::encode(raiseTypeError(globalObject, scope, "mmap item value must be an int"_s));
    // PyNumber_AsSsize_t(value, PyExc_TypeError)
    auto byte = toIndex(globalObject, value);
    if (Exception* raised = scope.exception()) [[unlikely]] {
        if (typeOf(globalObject, raised->value()) == globalObject->pyRealm()->type(BuiltinType::IndexError)) {
            (void)scope.tryClearException();
            raiseTypeError(globalObject, scope, concatenate("cannot fit '"_s, typeName(globalObject, value), "' into an index-sized integer"_s));
        }
        return { };
    }
    if (*byte < 0 || *byte > 255)
        return JSValue::encode(raiseValueError(globalObject, scope, "mmap item value must be in range(0, 256)"_s));
    if (!checkValid(globalObject, scope, self))
        return { };
    if (i >= self.size)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "mmap index out of range"_s));
    self.data[i] = static_cast<uint8_t>(*byte);
    RETURN_NONE();
}

} // anonymous namespace

PYTHON_NATIVE(mmapSetItem)
{
    MMAP_PROLOGUE();
    return setOrDeleteItem(globalObject, scope, self, args[1], args[2]);
}

PYTHON_NATIVE(mmapDeleteItem)
{
    MMAP_PROLOGUE();
    return setOrDeleteItem(globalObject, scope, self, args[1], JSValue());
}

// new_mmap_object()
PYTHON_NATIVE(mmapNew)
{
    NATIVE_PROLOGUE();
    // PyArg_ParseTupleAndKeywords() sees first whether there are too many. Then it makes what it can of each as it comes to it, so that what is wrong with the first is said before that there is no second. Names that it does not know come last.
    constexpr unsigned mostByPosition = 6;
    if (args.size() - 1 > mostByPosition || !args.at(1)) {
        checkArgumentsSlow(globalObject, callFrame);
        return { };
    }
    auto descriptor = toCIntOfFormat(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    if (!args.at(2)) {
        checkArgumentsSlow(globalObject, callFrame);
        return { };
    }
    auto length = toSsize(globalObject, args.at(2));
    RETURN_IF_EXCEPTION(scope, { });
    int64_t size = *length;
    int flags = MAP_SHARED;
    int protection = PROT_WRITE | PROT_READ;
    int access = static_cast<int>(Access::Default);
    for (auto [index, target] : { std::pair<unsigned, int*> { 3, &flags }, { 4, &protection }, { 5, &access } }) {
        if (JSValue given = args.at(index)) {
            auto converted = toCIntOfFormat(globalObject, given);
            RETURN_IF_EXCEPTION(scope, { });
            *target = *converted;
        }
    }
    off_t offset = 0;
    if (JSValue given = args.at(6)) {
        auto converted = toFileOffset(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
        offset = *converted;
    }
    bool tracksDescriptor = true;
    if (JSValue given = args.at(7)) {
        tracksDescriptor = isTrue(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!checkArgumentsSlow(globalObject, callFrame))
        return { };

    if (size < 0)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "memory mapped length must be positive"_s));
    if (offset < 0)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "memory mapped offset must be positive"_s));
    if (access != static_cast<int>(Access::Default) && (flags != MAP_SHARED || protection != (PROT_WRITE | PROT_READ)))
        return JSValue::encode(raiseValueError(globalObject, scope, "mmap can't specify both access and flags, prot."_s));
    switch (access) {
    case static_cast<int>(Access::Read):
        flags = MAP_SHARED;
        protection = PROT_READ;
        break;
    case static_cast<int>(Access::Write):
        flags = MAP_SHARED;
        protection = PROT_READ | PROT_WRITE;
        break;
    case static_cast<int>(Access::Copy):
        flags = MAP_PRIVATE;
        protection = PROT_READ | PROT_WRITE;
        break;
    case static_cast<int>(Access::Default):
        if (!(protection & PROT_READ) || !(protection & PROT_WRITE))
            access = static_cast<int>(protection & PROT_WRITE ? Access::Write : Access::Read);
        break;
    default:
        return JSValue::encode(raiseValueError(globalObject, scope, "mmap invalid access parameter."_s));
    }

    audit(globalObject, "mmap.__new__"_s, jsNumber(*descriptor), intFromInt64(globalObject, size), jsNumber(access), intFromInt64(globalObject, offset));
    RETURN_IF_EXCEPTION(scope, { });

    int file = *descriptor;
#if OS(DARWIN)
    // bpo-11277: fsync() is not enough there.
    if (file != -1)
        (void)fcntl(file, F_FULLFSYNC);
#endif
    struct stat status;
    if (file != -1 && !fstat(file, &status) && S_ISREG(status.st_mode)) {
        if (!size) {
            if (!status.st_size)
                return JSValue::encode(raiseValueError(globalObject, scope, "cannot mmap an empty file"_s));
            if (offset >= status.st_size)
                return JSValue::encode(raiseValueError(globalObject, scope, "mmap offset is greater than file size"_s));
            size = status.st_size - offset;
        } else if (offset > status.st_size || status.st_size - offset < size)
            return JSValue::encode(raiseValueError(globalObject, scope, "mmap length is greater than file size"_s));
    }

    auto* object = PyStateObject::create(vm, asType(args[0])->instanceStructure(), makeUnique<MmapState>());
    auto& self = object->state<MmapState>();
    self.offset = offset;
    self.tracksDescriptor = tracksDescriptor;
    if (file == -1)
        flags |= MAP_ANONYMOUS; // Memory that is of no file
    else if (tracksDescriptor) {
        // _Py_dup()
        self.descriptor = fcntl(file, F_DUPFD_CLOEXEC, 0);
        if (self.descriptor < 0)
            return JSValue::encode(raiseOSError(globalObject, scope, errno));
    }
    self.flags = flags;
    void* address = mmap(nullptr, static_cast<size_t>(size), protection, flags, file, offset);
    if (address == MAP_FAILED)
        return JSValue::encode(raiseOSError(globalObject, scope, errno));
    self.access = static_cast<Access>(access);
    adopt(globalObject, object, self, address, size);
    return JSValue::encode(object);
}

JSObject* createMmapModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    JSObject* module = newBuiltinModule(globalObject, "mmap"_s);
    auto add = [&] (ASCIILiteral name, int64_t value) { module->putDirect(vm, Identifier::fromString(vm, name), intFromInt64(globalObject, value)); };
    module->putDirect(vm, Identifier::fromString(vm, "error"_s), realm->type(BuiltinType::OSError)->object());

    PyType* type = createBuiltinType(globalObject, "mmap.mmap"_s, realm->typeObject(), PyType::Layout::Native, PyType::IsBaseType | PyType::HasSequenceItemOfItsOwn);
    type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
    addMethods(globalObject, type, {
        { "__new__"_s, mmapNew, Kind::New, 0, "?($type, /, fileno, length, flags=1, prot=3, access=0, offset=0, *, trackfd=True)"_s, PyNativeFunction::Arguments::AreThoseOfTheClassButNotChecked },
        { "__repr__"_s, mmapRepr, Kind::Wrapper },
        { "__len__"_s, mmapLength, Kind::Wrapper },
        { "__getitem__"_s, mmapGetItem, Kind::Wrapper },
        { "__setitem__"_s, mmapSetItem, Kind::Wrapper },
        { "__delitem__"_s, mmapDeleteItem, Kind::Wrapper },
        { "close"_s, mmapClose },
        { "find"_s, mmapFind, Kind::Method, pack(false) },
        { "rfind"_s, mmapFind, Kind::Method, pack(true) },
        { "flush"_s, mmapFlush },
        { "madvise"_s, mmapAdvise },
        { "move"_s, mmapMove },
        { "read"_s, mmapRead },
        { "read_byte"_s, mmapReadByte },
        { "readline"_s, mmapReadLine },
        { "resize"_s, mmapResize },
        { "seek"_s, mmapSeek },
        { "seekable"_s, mmapSeekable },
        { "size"_s, mmapSize },
        { "tell"_s, mmapTell },
        { "write"_s, mmapWrite },
        { "write_byte"_s, mmapWriteByte },
        { "__enter__"_s, mmapEnter },
        { "__exit__"_s, mmapClose },
    });
    addBufferMethods(globalObject, type);
    addGenericGetAttribute(globalObject, type);
    addGetSet(globalObject, type, "closed"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsBoolean(!stateOf<MmapState>(self).data); });
    module->putDirect(vm, Identifier::fromString(vm, "mmap"_s), type->object());

#ifdef PROT_EXEC
    add("PROT_EXEC"_s, PROT_EXEC);
#endif
#ifdef PROT_READ
    add("PROT_READ"_s, PROT_READ);
#endif
#ifdef PROT_WRITE
    add("PROT_WRITE"_s, PROT_WRITE);
#endif
#ifdef MAP_SHARED
    add("MAP_SHARED"_s, MAP_SHARED);
#endif
#ifdef MAP_PRIVATE
    add("MAP_PRIVATE"_s, MAP_PRIVATE);
#endif
#ifdef MAP_DENYWRITE
    add("MAP_DENYWRITE"_s, MAP_DENYWRITE);
#endif
#ifdef MAP_EXECUTABLE
    add("MAP_EXECUTABLE"_s, MAP_EXECUTABLE);
#endif
#ifdef MAP_ANONYMOUS
    add("MAP_ANON"_s, MAP_ANONYMOUS);
    add("MAP_ANONYMOUS"_s, MAP_ANONYMOUS);
#endif
#ifdef MAP_POPULATE
    add("MAP_POPULATE"_s, MAP_POPULATE);
#endif
#ifdef MAP_STACK
    add("MAP_STACK"_s, MAP_STACK);
#endif
#ifdef MAP_ALIGNED_SUPER
    add("MAP_ALIGNED_SUPER"_s, MAP_ALIGNED_SUPER);
#endif
#ifdef MAP_CONCEAL
    add("MAP_CONCEAL"_s, MAP_CONCEAL);
#endif
#ifdef MAP_NORESERVE
    add("MAP_NORESERVE"_s, MAP_NORESERVE);
#endif
#ifdef MAP_NOEXTEND
    add("MAP_NOEXTEND"_s, MAP_NOEXTEND);
#endif
#ifdef MAP_HASSEMAPHORE
    add("MAP_HASSEMAPHORE"_s, MAP_HASSEMAPHORE);
#endif
#ifdef MAP_NOCACHE
    add("MAP_NOCACHE"_s, MAP_NOCACHE);
#endif
#ifdef MAP_JIT
    add("MAP_JIT"_s, MAP_JIT);
#endif
#ifdef MAP_RESILIENT_CODESIGN
    add("MAP_RESILIENT_CODESIGN"_s, MAP_RESILIENT_CODESIGN);
#endif
#ifdef MAP_RESILIENT_MEDIA
    add("MAP_RESILIENT_MEDIA"_s, MAP_RESILIENT_MEDIA);
#endif
#ifdef MAP_32BIT
    add("MAP_32BIT"_s, MAP_32BIT);
#endif
#ifdef MAP_TRANSLATED_ALLOW_EXECUTE
    add("MAP_TRANSLATED_ALLOW_EXECUTE"_s, MAP_TRANSLATED_ALLOW_EXECUTE);
#endif
#ifdef MAP_UNIX03
    add("MAP_UNIX03"_s, MAP_UNIX03);
#endif
#ifdef MAP_TPRO
    add("MAP_TPRO"_s, MAP_TPRO);
#endif
    add("PAGESIZE"_s, sysconf(_SC_PAGESIZE));
    add("ALLOCATIONGRANULARITY"_s, sysconf(_SC_PAGESIZE));
    add("ACCESS_DEFAULT"_s, static_cast<int>(Access::Default));
    add("ACCESS_READ"_s, static_cast<int>(Access::Read));
    add("ACCESS_WRITE"_s, static_cast<int>(Access::Write));
    add("ACCESS_COPY"_s, static_cast<int>(Access::Copy));
#ifdef MADV_NORMAL
    add("MADV_NORMAL"_s, MADV_NORMAL);
#endif
#ifdef MADV_RANDOM
    add("MADV_RANDOM"_s, MADV_RANDOM);
#endif
#ifdef MADV_SEQUENTIAL
    add("MADV_SEQUENTIAL"_s, MADV_SEQUENTIAL);
#endif
#ifdef MADV_WILLNEED
    add("MADV_WILLNEED"_s, MADV_WILLNEED);
#endif
#ifdef MADV_DONTNEED
    add("MADV_DONTNEED"_s, MADV_DONTNEED);
#endif
#ifdef MADV_REMOVE
    add("MADV_REMOVE"_s, MADV_REMOVE);
#endif
#ifdef MADV_DONTFORK
    add("MADV_DONTFORK"_s, MADV_DONTFORK);
#endif
#ifdef MADV_DOFORK
    add("MADV_DOFORK"_s, MADV_DOFORK);
#endif
#ifdef MADV_HWPOISON
    add("MADV_HWPOISON"_s, MADV_HWPOISON);
#endif
#ifdef MADV_MERGEABLE
    add("MADV_MERGEABLE"_s, MADV_MERGEABLE);
#endif
#ifdef MADV_UNMERGEABLE
    add("MADV_UNMERGEABLE"_s, MADV_UNMERGEABLE);
#endif
#ifdef MADV_SOFT_OFFLINE
    add("MADV_SOFT_OFFLINE"_s, MADV_SOFT_OFFLINE);
#endif
#ifdef MADV_HUGEPAGE
    add("MADV_HUGEPAGE"_s, MADV_HUGEPAGE);
#endif
#ifdef MADV_NOHUGEPAGE
    add("MADV_NOHUGEPAGE"_s, MADV_NOHUGEPAGE);
#endif
#ifdef MADV_DONTDUMP
    add("MADV_DONTDUMP"_s, MADV_DONTDUMP);
#endif
#ifdef MADV_DODUMP
    add("MADV_DODUMP"_s, MADV_DODUMP);
#endif
#ifdef MADV_FREE // (Also present on FreeBSD and macOS.)
    add("MADV_FREE"_s, MADV_FREE);
#endif
#ifdef MADV_NOSYNC
    add("MADV_NOSYNC"_s, MADV_NOSYNC);
#endif
#ifdef MADV_AUTOSYNC
    add("MADV_AUTOSYNC"_s, MADV_AUTOSYNC);
#endif
#ifdef MADV_NOCORE
    add("MADV_NOCORE"_s, MADV_NOCORE);
#endif
#ifdef MADV_CORE
    add("MADV_CORE"_s, MADV_CORE);
#endif
#ifdef MADV_PROTECT
    add("MADV_PROTECT"_s, MADV_PROTECT);
#endif
#ifdef MADV_FREE_REUSABLE // (As MADV_FREE but reclaims more faithful for task_info/Activity Monitor...)
    add("MADV_FREE_REUSABLE"_s, MADV_FREE_REUSABLE);
#endif
#ifdef MADV_FREE_REUSE // (Reuse pages previously tagged as reusable)
    add("MADV_FREE_REUSE"_s, MADV_FREE_REUSE);
#endif
    return module;
}

} } // namespace JSC::Python

#endif // OS(UNIX)
