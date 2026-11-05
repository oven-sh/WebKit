/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyPosix.h"

#if !OS(WINDOWS)

#include "ArrayBuffer.h"
#include "JSArrayBuffer.h"
#include "JSCInlines.h"
#include "PyBuffer.h"
#include "PyRange.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <wtf/SharedTask.h>
#include <wtf/ThreadSafeRefCounted.h>
#if OS(LINUX)
#include <sys/prctl.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module mmap. (Modules/mmapmodule.c in CPython is the reference.)
//
// The mapped memory is the memory of an ArrayBuffer of JS, which is what the buffer protocol here knows as an owner of memory
// that is not the collector's. The ArrayBuffer unmaps it when it is detached, which is what close() does, or destroyed, which
// happens when neither the mmap object nor any view of it can be reached. So a mapping cannot exceed what an ArrayBuffer can
// hold: 4 GiB.

// What the ArrayBuffer is to let go of.
struct MmapResources final : ThreadSafeRefCounted<MmapResources> {
    size_t size { 0 };
    int fd { -1 }; // A duplicate of the file's descriptor, for size() and resize().
    bool ownsMemory { true }; // False once mremap() has taken it.

    void release(void* data)
    {
        if (fd >= 0)
            close(std::exchange(fd, -1));
        if (std::exchange(ownsMemory, false) && data)
            munmap(data, size);
    }
};

// The number of the record is the MmapResources*, which the ArrayBuffer keeps alive. 0: closed.
enum MmapField : unsigned {
    MmapMemory, // The JSArrayBuffer. None: closed.
    MmapPosition,
    MmapOffset,
    MmapAccess,
    MmapFlags,
    MmapTracksFd,
    MmapExports,
    MmapFieldCount
};

enum MmapAccessMode : int { MmapAccessDefault, MmapAccessRead, MmapAccessWrite, MmapAccessCopy };

static MmapResources* mmapResources(PyRecord* map) { return reinterpret_cast<MmapResources*>(static_cast<intptr_t>(map->number)); }
static bool mmapIsClosed(PyRecord* map) { return !map->number; }
static ArrayBuffer* mmapArrayBuffer(PyRecord* map) { return uncheckedDowncast<JSArrayBuffer>(map->get(MmapMemory).asCell())->impl(); }
static uint8_t* mmapData(PyRecord* map) { return static_cast<uint8_t*>(mmapArrayBuffer(map)->data()); }
static int64_t mmapSize(PyRecord* map) { return static_cast<int64_t>(mmapResources(map)->size); }
static int64_t mmapPosition(PyRecord* map) { return tryInt64(map->get(MmapPosition)).value_or(0); }
static void mmapSetPosition(JSGlobalObject* globalObject, PyRecord* map, int64_t position) { map->set(getVM(globalObject), MmapPosition, makeInt(globalObject, position)); }
static int mmapAccess(PyRecord* map) { return map->get(MmapAccess).asInt32(); }

// False: a ValueError.
static bool mmapCheckOpen(JSGlobalObject* globalObject, PyRecord* map)
{
    if (!mmapIsClosed(map)) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseValueError(globalObject, scope, "mmap closed or invalid"_s);
    return false;
}

static bool mmapCheckWritable(JSGlobalObject* globalObject, PyRecord* map)
{
    if (mmapAccess(map) != MmapAccessRead) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseTypeError(globalObject, scope, "mmap can't modify a readonly memory map."_s);
    return false;
}

static bool mmapIsExported(JSGlobalObject* globalObject, PyRecord* map)
{
    return isStillExported(globalObject, map, [&] { return map->get(MmapExports).asInt32() > 0; });
}

static void mmapAdopt(JSGlobalObject* globalObject, PyRecord* map, void* data, size_t size, int fd)
{
    VM& vm = getVM(globalObject);
    auto resources = adoptRef(*new MmapResources);
    resources->size = size;
    resources->fd = fd;
    map->number = static_cast<int64_t>(reinterpret_cast<intptr_t>(resources.ptr()));
    auto buffer = ArrayBuffer::createFromBytes(std::span<const uint8_t> { static_cast<const uint8_t*>(data), size }, createSharedTask<void(void*)>([resources = WTF::move(resources)](void* memory) {
        resources->release(memory);
    }));
    map->set(vm, MmapMemory, JSArrayBuffer::create(vm, globalObject->arrayBufferStructure(ArrayBufferSharingMode::Default), WTF::move(buffer)));
}

static void mmapAnnotate(void* data, size_t size, const char* name)
{
#if OS(LINUX) && defined(PR_SET_VMA) && defined(PR_SET_VMA_ANON_NAME)
    prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, reinterpret_cast<unsigned long>(data), size, reinterpret_cast<unsigned long>(name));
#else
    UNUSED_PARAM(data);
    UNUSED_PARAM(size);
    UNUSED_PARAM(name);
#endif
}

// The format "i".
static bool mmapToInt(JSGlobalObject* globalObject, JSValue value, int& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t number;
    if (failed(scope, posixToLong(globalObject, value, number)))
        return false;
    if (number >= INT_MIN && number <= INT_MAX) {
        result = static_cast<int>(number);
        return true;
    }
    raiseOverflowError(globalObject, scope, number > 0 ? "signed integer is greater than maximum"_s : "signed integer is less than minimum"_s);
    return false;
}

static JSValue mmapNew(JSGlobalObject* globalObject, JSValue type, JSValue fileno, JSValue lengthValue, JSValue flagsValue, JSValue protValue, JSValue accessValue, JSValue offsetValue, JSValue trackFdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    int64_t size;
    int flags = MAP_SHARED;
    int protection = PROT_WRITE | PROT_READ;
    int access = MmapAccessDefault;
    int64_t offset = 0;
    bool tracksFd = true;
    if (failed(scope, mmapToInt(globalObject, fileno, fd)) || failed(scope, posixToSsize(globalObject, lengthValue, size)) || (flagsValue && failed(scope, mmapToInt(globalObject, flagsValue, flags))) || (protValue && failed(scope, mmapToInt(globalObject, protValue, protection)))
        || (accessValue && failed(scope, mmapToInt(globalObject, accessValue, access))) || (offsetValue && failed(scope, posixToLong(globalObject, offsetValue, offset))) || failed(scope, posixToBool(globalObject, trackFdValue, true, tracksFd)))
        return { };
    if (size < 0)
        return raiseOverflowError(globalObject, scope, "memory mapped length must be positive"_s);
    if (offset < 0)
        return raiseOverflowError(globalObject, scope, "memory mapped offset must be positive"_s);
    if (access != MmapAccessDefault && (flags != MAP_SHARED || protection != (PROT_WRITE | PROT_READ)))
        return raiseValueError(globalObject, scope, "mmap can't specify both access and flags, prot."_s);
    switch (access) {
    case MmapAccessRead:
        flags = MAP_SHARED;
        protection = PROT_READ;
        break;
    case MmapAccessWrite:
        flags = MAP_SHARED;
        protection = PROT_READ | PROT_WRITE;
        break;
    case MmapAccessCopy:
        flags = MAP_PRIVATE;
        protection = PROT_READ | PROT_WRITE;
        break;
    case MmapAccessDefault:
        if (!(protection & PROT_READ) || !(protection & PROT_WRITE))
            access = protection & PROT_WRITE ? MmapAccessWrite : MmapAccessRead;
        break;
    default:
        return raiseValueError(globalObject, scope, "mmap invalid access parameter."_s);
    }
    if (failed(scope, audit(globalObject, "mmap.__new__"_s, jsNumber(fd), posixMakeInt(globalObject, size), jsNumber(access), posixMakeInt(globalObject, offset))))
        return { };

#if OS(DARWIN)
    // There, what was written to the file may otherwise not be seen in the mapping.
    if (fd != -1)
        fcntl(fd, F_FULLFSYNC);
#endif
    struct stat status;
    if (fd != -1 && !fstat(fd, &status) && S_ISREG(status.st_mode)) {
        if (!size) {
            if (!status.st_size)
                return raiseValueError(globalObject, scope, "cannot mmap an empty file"_s);
            if (offset >= status.st_size)
                return raiseValueError(globalObject, scope, "mmap offset is greater than file size"_s);
            size = status.st_size - offset;
        } else if (offset > status.st_size || status.st_size - offset < size)
            return raiseValueError(globalObject, scope, "mmap length is greater than file size"_s);
    }
    if (static_cast<uint64_t>(size) > MAX_ARRAY_BUFFER_SIZE)
        return raiseValueError(globalObject, scope, "mmap length is too large"_s);

    int duplicate = -1;
    if (fd == -1)
        flags |= MAP_ANONYMOUS;
    else if (tracksFd) {
        duplicate = posixDupNotInheritable(fd);
        if (duplicate < 0)
            RELEASE_AND_RETURN(scope, posixError(globalObject));
    }
    void* data = mmap(nullptr, size, protection, flags, fd, offset);
    if (data == MAP_FAILED) {
        int error = errno;
        if (duplicate >= 0)
            close(duplicate);
        RELEASE_AND_RETURN(scope, posixError(globalObject, error));
    }
    if (flags & MAP_ANONYMOUS)
        mmapAnnotate(data, size, "cpython:mmap");

    PyRecord* map = PyRecord::create(globalObject, PyType::from(type), MmapFieldCount);
    map->set(vm, MmapPosition, jsNumber(0));
    map->set(vm, MmapOffset, makeInt(globalObject, offset));
    map->set(vm, MmapAccess, jsNumber(access));
    map->set(vm, MmapFlags, jsNumber(flags));
    map->set(vm, MmapTracksFd, jsBoolean(tracksFd));
    map->set(vm, MmapExports, jsNumber(0));
    mmapAdopt(globalObject, map, data, size, duplicate);
    return map;
}

static JSValue mmapClose(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* map = PyRecord::from(self);
    if (mmapIsExported(globalObject, map))
        return raiseBufferError(globalObject, scope, "cannot close exported pointers exist"_s);
    if (mmapIsClosed(map))
        return jsUndefined();
    Ref<ArrayBuffer> buffer = *mmapArrayBuffer(map);
    map->number = 0;
    map->set(vm, MmapMemory, jsUndefined());
    buffer->detach(vm);
    return jsUndefined();
}

static JSValue mmapGetClosed(JSGlobalObject*, JSValue self) { return jsBoolean(mmapIsClosed(PyRecord::from(self))); }

static JSValue mmapBytes(JSGlobalObject* globalObject, PyRecord* map, int64_t start, int64_t count)
{
    return PyBytes::create(globalObject, std::span<const uint8_t> { mmapData(map) + start, static_cast<size_t>(count) });
}

static JSValue mmapReadByte(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* map = PyRecord::from(self);
    if (failed(scope, mmapCheckOpen(globalObject, map)))
        return { };
    int64_t position = mmapPosition(map);
    if (position >= mmapSize(map))
        return raiseValueError(globalObject, scope, "read byte out of range"_s);
    mmapSetPosition(globalObject, map, position + 1);
    return jsNumber(mmapData(map)[position]);
}

static JSValue mmapReadLine(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* map = PyRecord::from(self);
    if (failed(scope, mmapCheckOpen(globalObject, map)))
        return { };
    int64_t position = mmapPosition(map);
    int64_t remaining = std::max<int64_t>(mmapSize(map) - position, 0);
    int64_t count = remaining;
    if (remaining) {
        if (auto* newline = static_cast<uint8_t*>(memchr(mmapData(map) + position, '\n', remaining)))
            count = newline + 1 - (mmapData(map) + position);
    }
    JSValue result = mmapBytes(globalObject, map, remaining ? position : 0, count);
    mmapSetPosition(globalObject, map, position + count);
    return result;
}

static JSValue mmapRead(JSGlobalObject* globalObject, JSValue self, JSValue countValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* map = PyRecord::from(self);
    int64_t count = std::numeric_limits<int64_t>::max();
    if (countValue && !isNone(countValue)) {
        if (!hasIndex(globalObject, countValue))
            return raiseTypeError(globalObject, scope, "argument should be integer or None, not '"_s, typeName(globalObject, countValue), '\'');
        count = toSsize(globalObject, countValue, rt.OverflowError());
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (failed(scope, mmapCheckOpen(globalObject, map)))
        return { };
    int64_t position = mmapPosition(map);
    int64_t remaining = std::max<int64_t>(mmapSize(map) - position, 0);
    if (count < 0 || count > remaining)
        count = remaining;
    JSValue result = mmapBytes(globalObject, map, remaining ? position : 0, count);
    mmapSetPosition(globalObject, map, position + count);
    return result;
}

template<bool reverse>
static JSValue mmapFind(JSGlobalObject* globalObject, JSValue self, JSValue needleValue, JSValue startValue, JSValue endValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* map = PyRecord::from(self);
    BufferHolder holder(globalObject);
    if (failed(scope, holder.get(needleValue)) || failed(scope, mmapCheckOpen(globalObject, map)))
        return { };
    int64_t start = mmapPosition(map);
    int64_t end = mmapSize(map);
    if (startValue && !isNone(startValue)) {
        if (failed(scope, posixToSsize(globalObject, startValue, start)) || (endValue && !isNone(endValue) && failed(scope, posixToSsize(globalObject, endValue, end))))
            return { };
    }
    // An __index__() may have closed it.
    if (failed(scope, mmapCheckOpen(globalObject, map)) || failed(scope, refreshBuffer(globalObject, holder.buffer)))
        return { };
    int64_t size = mmapSize(map);
    if (start < 0)
        start = std::max<int64_t>(start + size, 0);
    if (end < 0)
        end = std::max<int64_t>(end + size, 0);
    end = std::min(end, size);
    int64_t needleLength = holder.buffer.length;
    if (end < start || end - start < needleLength)
        return jsNumber(-1);
    const uint8_t* data = mmapData(map);
    const uint8_t* needle = holder.buffer.data;
    if (!needleLength)
        RELEASE_AND_RETURN(scope, posixMakeInt(globalObject, reverse ? end : start));
    if constexpr (reverse) {
        for (int64_t i = end - needleLength; i >= start; --i) {
            if (data[i] == needle[0] && !memcmp(data + i, needle, needleLength))
                RELEASE_AND_RETURN(scope, posixMakeInt(globalObject, i));
        }
        return jsNumber(-1);
    }
    auto* found = static_cast<const uint8_t*>(memmem(data + start, end - start, needle, needleLength));
    RELEASE_AND_RETURN(scope, found ? posixMakeInt(globalObject, static_cast<int64_t>(found - data)) : jsNumber(-1));
}

static JSValue mmapWrite(JSGlobalObject* globalObject, JSValue self, JSValue dataValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* map = PyRecord::from(self);
    BufferHolder holder(globalObject);
    if (failed(scope, holder.get(dataValue)) || failed(scope, mmapCheckOpen(globalObject, map)) || failed(scope, mmapCheckWritable(globalObject, map)))
        return { };
    int64_t position = mmapPosition(map);
    int64_t count = holder.buffer.length;
    if (position > mmapSize(map) || mmapSize(map) - position < count)
        return raiseValueError(globalObject, scope, "data out of range"_s);
    if (count)
        memmove(mmapData(map) + position, holder.buffer.data, count);
    mmapSetPosition(globalObject, map, position + count);
    RELEASE_AND_RETURN(scope, posixMakeInt(globalObject, count));
}

static JSValue mmapWriteByte(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* map = PyRecord::from(self);
    int64_t byte;
    if (failed(scope, posixToLong(globalObject, value, byte)))
        return { };
    if (byte < 0)
        return raiseOverflowError(globalObject, scope, "unsigned byte integer is less than minimum"_s);
    if (byte > 255)
        return raiseOverflowError(globalObject, scope, "unsigned byte integer is greater than maximum"_s);
    if (failed(scope, mmapCheckOpen(globalObject, map)) || failed(scope, mmapCheckWritable(globalObject, map)))
        return { };
    int64_t position = mmapPosition(map);
    if (position >= mmapSize(map))
        return raiseValueError(globalObject, scope, "write byte out of range"_s);
    mmapData(map)[position] = static_cast<uint8_t>(byte);
    mmapSetPosition(globalObject, map, position + 1);
    return jsUndefined();
}

static JSValue mmapSizeMethod(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* map = PyRecord::from(self);
    if (failed(scope, mmapCheckOpen(globalObject, map)))
        return { };
    if (mmapResources(map)->fd != -1) {
        struct stat status;
        if (fstat(mmapResources(map)->fd, &status) < 0)
            RELEASE_AND_RETURN(scope, posixError(globalObject));
        RELEASE_AND_RETURN(scope, posixMakeInt(globalObject, static_cast<int64_t>(status.st_size)));
    }
    if (map->get(MmapTracksFd).asBoolean())
        RELEASE_AND_RETURN(scope, posixMakeInt(globalObject, mmapSize(map)));
    return raiseValueError(globalObject, scope, "can't get size with trackfd=False"_s);
}

#if OS(LINUX)
static JSValue mmapResize(JSGlobalObject* globalObject, JSValue self, JSValue sizeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* map = PyRecord::from(self);
    int64_t newSize;
    if (failed(scope, posixToSsize(globalObject, sizeValue, newSize)) || failed(scope, mmapCheckOpen(globalObject, map)))
        return { };
    if (mmapIsExported(globalObject, map))
        return raiseBufferError(globalObject, scope, "mmap can't resize with extant buffers exported."_s);
    if (!map->get(MmapTracksFd).asBoolean())
        return raiseValueError(globalObject, scope, "mmap can't resize with trackfd=False."_s);
    if (mmapAccess(map) != MmapAccessWrite && mmapAccess(map) != MmapAccessDefault)
        return raiseTypeError(globalObject, scope, "mmap can't resize a readonly or copy-on-write memory map."_s);
    int64_t offset = tryInt64(map->get(MmapOffset)).value_or(0);
    if (newSize < 0 || std::numeric_limits<int64_t>::max() - newSize < offset || static_cast<uint64_t>(newSize) > MAX_ARRAY_BUFFER_SIZE)
        return raiseValueError(globalObject, scope, "new size out of range"_s);
    MmapResources* resources = mmapResources(map);
    // The system would refuse.
    if (resources->fd == -1 && !(map->get(MmapFlags).asInt32() & MAP_PRIVATE) && newSize > mmapSize(map))
        return raiseValueError(globalObject, scope, "mmap: can't expand a shared anonymous mapping"_s);
    if (resources->fd != -1 && ftruncate(resources->fd, offset + newSize) < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    void* data = mremap(mmapData(map), resources->size, newSize, MREMAP_MAYMOVE);
    if (data == MAP_FAILED)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    // The memory has a new ArrayBuffer, and the old one nothing to let go of.
    Ref<ArrayBuffer> previous = *mmapArrayBuffer(map);
    resources->ownsMemory = false;
    int fd = std::exchange(resources->fd, -1);
    mmapAdopt(globalObject, map, data, newSize, fd);
    previous->detach(vm);
    return jsUndefined();
}
#endif

static JSValue mmapTell(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* map = PyRecord::from(self);
    if (failed(scope, mmapCheckOpen(globalObject, map)))
        return { };
    return map->get(MmapPosition);
}

static JSValue mmapFlush(JSGlobalObject* globalObject, JSValue self, JSValue offsetValue, JSValue sizeValue, JSValue flagsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* map = PyRecord::from(self);
    int64_t offset = 0;
    int64_t size = -1;
    int flags = 0;
    if ((offsetValue && failed(scope, posixToSsize(globalObject, offsetValue, offset))) || (sizeValue && failed(scope, posixToSsize(globalObject, sizeValue, size))) || (flagsValue && failed(scope, posixToInt(globalObject, flagsValue, flags)))
        || failed(scope, mmapCheckOpen(globalObject, map)))
        return { };
    if (size == -1)
        size = mmapSize(map) - offset;
    if (size < 0 || offset < 0 || mmapSize(map) - offset < size)
        return raiseValueError(globalObject, scope, "flush values out of range"_s);
    if (mmapAccess(map) == MmapAccessRead || mmapAccess(map) == MmapAccessCopy)
        return jsUndefined();
    if (msync(mmapData(map) + offset, size, flags ? flags : MS_SYNC) < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsUndefined();
}

static JSValue mmapSeek(JSGlobalObject* globalObject, JSValue self, JSValue distanceValue, JSValue whenceValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* map = PyRecord::from(self);
    int64_t distance;
    int whence = 0;
    if (failed(scope, posixToSsize(globalObject, distanceValue, distance)) || (whenceValue && failed(scope, posixToInt(globalObject, whenceValue, whence))) || failed(scope, mmapCheckOpen(globalObject, map)))
        return { };
    if (whence < 0 || whence > 2)
        return raiseValueError(globalObject, scope, "unknown seek type"_s);
    int64_t base = !whence ? 0 : whence == 1 ? mmapPosition(map) : mmapSize(map);
    int64_t position;
    if (__builtin_add_overflow(base, distance, &position) || position < 0 || position > mmapSize(map))
        return raiseValueError(globalObject, scope, "seek out of range"_s);
    mmapSetPosition(globalObject, map, position);
    return map->get(MmapPosition);
}

static JSValue mmapSeekable(JSGlobalObject*, JSValue) { return jsBoolean(true); }

static JSValue mmapSetName(JSGlobalObject* globalObject, JSValue self, JSValue nameValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(nameValue))
        return raiseTypeError(globalObject, scope, "set_name() argument must be str, not "_s, posixTypeNameOfArgument(globalObject, nameValue));
    PosixBytes name;
    if (failed(scope, posixEncodeStr(globalObject, strValue(nameValue), name)))
        return { };
    if (name.contains('\0'))
        return raiseValueError(globalObject, scope, "embedded null character"_s);
#if OS(LINUX) && defined(PR_SET_VMA) && defined(PR_SET_VMA_ANON_NAME)
    auto* map = PyRecord::from(self);
    static constexpr char prefix[] = "cpython:mmap:";
    if (name.length() + strlen(prefix) > 79)
        return raiseValueError(globalObject, scope, "name is too long"_s);
    if (!(map->get(MmapFlags).asInt32() & MAP_ANONYMOUS))
        return raiseValueError(globalObject, scope, "Cannot set annotation on non-anonymous mappings"_s);
    if (failed(scope, mmapCheckOpen(globalObject, map)))
        return { };
    char buffer[80];
    snprintf(buffer, sizeof(buffer), "%s%s", prefix, name.characters());
    // A kernel that cannot do this is no reason to fail.
    if (prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, reinterpret_cast<unsigned long>(mmapData(map)), mmapSize(map), reinterpret_cast<unsigned long>(buffer)) < 0 && errno != EINVAL)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsUndefined();
#else
    UNUSED_PARAM(self);
    return raiseNotImplementedError(globalObject, scope, "Annotation of mmap is not supported on this platform"_s);
#endif
}

static JSValue mmapMove(JSGlobalObject* globalObject, JSValue self, JSValue destinationValue, JSValue sourceValue, JSValue countValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* map = PyRecord::from(self);
    int64_t destination;
    int64_t source;
    int64_t count;
    if (failed(scope, posixToSsize(globalObject, destinationValue, destination)) || failed(scope, posixToSsize(globalObject, sourceValue, source)) || failed(scope, posixToSsize(globalObject, countValue, count))
        || failed(scope, mmapCheckOpen(globalObject, map)) || failed(scope, mmapCheckWritable(globalObject, map)))
        return { };
    int64_t size = mmapSize(map);
    if (destination < 0 || source < 0 || count < 0 || size - destination < count || size - source < count)
        return raiseValueError(globalObject, scope, "source, destination, or count out of range"_s);
    memmove(mmapData(map) + destination, mmapData(map) + source, count);
    return jsUndefined();
}

static JSValue mmapAdvise(JSGlobalObject* globalObject, JSValue self, JSValue optionValue, JSValue startValue, JSValue lengthValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* map = PyRecord::from(self);
    int option;
    int64_t start = 0;
    if (failed(scope, posixToInt(globalObject, optionValue, option)) || (startValue && failed(scope, posixToSsize(globalObject, startValue, start))) || failed(scope, mmapCheckOpen(globalObject, map)))
        return { };
    int64_t length = mmapSize(map);
    if (lengthValue && !isNone(lengthValue) && failed(scope, posixToSsize(globalObject, lengthValue, length)))
        return { };
    if (failed(scope, mmapCheckOpen(globalObject, map)))
        return { };
    if (start < 0 || start >= mmapSize(map))
        return raiseValueError(globalObject, scope, "madvise start out of bounds"_s);
    if (length < 0)
        return raiseValueError(globalObject, scope, "madvise length invalid"_s);
    if (std::numeric_limits<int64_t>::max() - start < length)
        return raiseOverflowError(globalObject, scope, "madvise length too large"_s);
    length = std::min(length, mmapSize(map) - start);
    if (madvise(mmapData(map) + start, length, option))
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsUndefined();
}

static JSValue mmapEnter(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, mmapCheckOpen(globalObject, PyRecord::from(self))))
        return { };
    return self;
}

static JSValue mmapExit(JSGlobalObject* globalObject, JSValue self, JSValue, JSValue, JSValue) { return mmapClose(globalObject, self); }

static JSValue mmapRepr(JSGlobalObject* globalObject, JSValue self)
{
    auto* map = PyRecord::from(self);
    String name = map->type()->nameInMessages();
    if (mmapIsClosed(map))
        return makeStr(globalObject, makeString('<', name, " closed=True>"_s));
    static constexpr ASCIILiteral accessNames[] = { "ACCESS_DEFAULT"_s, "ACCESS_READ"_s, "ACCESS_WRITE"_s, "ACCESS_COPY"_s };
    return makeStr(globalObject, makeString('<', name, " closed=False, access="_s, accessNames[mmapAccess(map)], ", length="_s, mmapSize(map), ", pos="_s, mmapPosition(map), ", offset="_s,
        tryInt64(map->get(MmapOffset)).value_or(0), '>'));
}

// ---- As a buffer and as a sequence ----

static void mmapDidRelease(JSGlobalObject* globalObject, JSValue self)
{
    auto* map = PyRecord::from(self);
    map->set(getVM(globalObject), MmapExports, jsNumber(map->get(MmapExports).asInt32() - 1));
}

static JSValue mmapBuffer(JSGlobalObject* globalObject, JSValue self, JSValue flags)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* map = PyRecord::from(self);
    if (failed(scope, mmapCheckOpen(globalObject, map)))
        return { };
    JSValue view = exportBuffer(globalObject, self, flags, { map->get(MmapMemory).asCell(), 0, static_cast<size_t>(mmapSize(map)), mmapAccess(map) == MmapAccessRead, 1, { }, mmapDidRelease });
    RETURN_IF_EXCEPTION(scope, { });
    map->set(vm, MmapExports, jsNumber(map->get(MmapExports).asInt32() + 1));
    return view;
}

static JSValue mmapLength(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* map = PyRecord::from(self);
    if (failed(scope, mmapCheckOpen(globalObject, map)))
        return { };
    RELEASE_AND_RETURN(scope, posixMakeInt(globalObject, mmapSize(map)));
}

static JSValue mmapGetItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* map = PyRecord::from(self);
    if (failed(scope, mmapCheckOpen(globalObject, map)))
        return { };
    if (hasIndex(globalObject, key)) {
        int64_t index = toSsize(globalObject, key, rt.IndexError());
        RETURN_IF_EXCEPTION(scope, { });
        if (failed(scope, mmapCheckOpen(globalObject, map)))
            return { };
        if (index < 0)
            index += mmapSize(map);
        if (index < 0 || index >= mmapSize(map))
            return raiseIndexError(globalObject, scope, "mmap index out of range"_s);
        return jsNumber(mmapData(map)[index]);
    }
    auto* slice = PySlice::tryFrom(key);
    if (!slice)
        return raiseTypeError(globalObject, scope, "mmap indices must be integers"_s);
    PySlice::Indices indices;
    if (failed(scope, slice->unpack(globalObject, indices)) || failed(scope, mmapCheckOpen(globalObject, map)))
        return { };
    PySlice::adjust(mmapSize(map), indices);
    if (indices.length <= 0)
        RELEASE_AND_RETURN(scope, PyBytes::create(globalObject, std::span<const uint8_t> { }));
    if (indices.step == 1)
        RELEASE_AND_RETURN(scope, mmapBytes(globalObject, map, indices.start, indices.length));
    PyBytes* result = PyBytes::createUninitialized(globalObject, indices.length);
    const uint8_t* data = mmapData(map);
    for (int64_t i = 0, from = indices.start; i < indices.length; ++i, from += indices.step)
        result->mutableData()[i] = data[from];
    return result;
}

static JSValue mmapSetItem(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* map = PyRecord::from(self);
    if (failed(scope, mmapCheckWritable(globalObject, map)))
        return { };
    if (hasIndex(globalObject, key)) {
        int64_t index = toSsize(globalObject, key, rt.IndexError());
        RETURN_IF_EXCEPTION(scope, { });
        if (!value)
            return raiseTypeError(globalObject, scope, "mmap doesn't support item deletion"_s);
        if (!hasIndex(globalObject, value))
            return raiseTypeError(globalObject, scope, "mmap item value must be an int"_s);
        int64_t byte = toSsize(globalObject, value, rt.TypeError());
        RETURN_IF_EXCEPTION(scope, { });
        if (byte < 0 || byte > 255)
            return raiseValueError(globalObject, scope, "mmap item value must be in range(0, 256)"_s);
        if (failed(scope, mmapCheckOpen(globalObject, map)))
            return { };
        if (index < 0)
            index += mmapSize(map);
        if (index < 0 || index >= mmapSize(map))
            return raiseIndexError(globalObject, scope, "mmap index out of range"_s);
        mmapData(map)[index] = static_cast<uint8_t>(byte);
        return jsUndefined();
    }
    auto* slice = PySlice::tryFrom(key);
    if (!slice)
        return raiseTypeError(globalObject, scope, "mmap indices must be integer"_s);
    // For the errors of the bounds, which come first. What they select is worked out when no more code can run.
    PySlice::Indices indices;
    if (failed(scope, slice->unpack(globalObject, indices)))
        return { };
    if (!value)
        return raiseTypeError(globalObject, scope, "mmap object doesn't support slice deletion"_s);
    BufferHolder holder(globalObject);
    if (failed(scope, holder.get(value)) || failed(scope, mmapCheckOpen(globalObject, map)) || failed(scope, refreshBuffer(globalObject, holder.buffer)))
        return { };
    PySlice::adjust(mmapSize(map), indices);
    if (holder.buffer.length != indices.length)
        return raiseIndexError(globalObject, scope, "mmap slice assignment is wrong size"_s);
    uint8_t* data = mmapData(map);
    if (indices.step == 1) {
        if (indices.length)
            memmove(data + indices.start, holder.buffer.data, indices.length);
        return jsUndefined();
    }
    for (int64_t i = 0, to = indices.start; i < indices.length; ++i, to += indices.step)
        data[to] = holder.buffer.data[i];
    return jsUndefined();
}

static JSValue mmapDelItem(JSGlobalObject* globalObject, JSValue self, JSValue key) { return mmapSetItem(globalObject, self, key, { }); }

// Iterating over it gives bytes of length 1, though map[i] is an int: in CPython, that goes by the item of the sequence, and
// this by that of the mapping.
enum MmapIteratorField : unsigned { MmapIteratorMap, MmapIteratorFieldCount };

static JSValue mmapIteratorNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* iterator = PyRecord::from(self);
    JSValue mapValue = iterator->get(MmapIteratorMap);
    if (!isNone(mapValue)) {
        auto* map = PyRecord::from(mapValue);
        if (failed(scope, mmapCheckOpen(globalObject, map)))
            return { };
        if (iterator->number < mmapSize(map))
            RELEASE_AND_RETURN(scope, mmapBytes(globalObject, map, iterator->number++, 1));
        iterator->set(vm, MmapIteratorMap, jsUndefined());
    }
    return raiseStopIteration(globalObject, scope);
}

static JSValue mmapIteratorIter(JSGlobalObject*, JSValue self) { return self; }

static constexpr NativeDef mmapIteratorMethods[] = {
    PY_SLOT("__iter__", mmapIteratorIter, "($self, /)"),
    PY_SLOT("__next__", mmapIteratorNext, "($self, /)"),
};

static constexpr TypeDef mmapIteratorTypeDef { "iterator"_s, { TypeFlag::DisallowInstantiation }, mmapIteratorMethods, { }, { } };

static JSValue mmapIter(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* iterator = PyRecord::create(globalObject, runtime(globalObject).typeFor(mmapIteratorTypeDef), MmapIteratorFieldCount);
    iterator->set(getVM(globalObject), MmapIteratorMap, self);
    return iterator;
}

static constexpr NativeDef mmapMethods[] = {
    PY_NEW(mmapNew, "($type, /, fileno, length, flags=MAP_SHARED, prot=PROT_WRITE | PROT_READ, access=ACCESS_DEFAULT, offset=0, *, trackfd=True)"),
    PY_SLOT("__repr__", mmapRepr, "($self, /)"),
    PY_SLOT("__len__", mmapLength, "($self, /)"),
    PY_SLOT("__getitem__", mmapGetItem, "($self, key, /)"),
    PY_SLOT("__setitem__", mmapSetItem, "($self, key, value, /)"),
    PY_SLOT("__delitem__", mmapDelItem, "($self, key, /)"),
    PY_SLOT("__iter__", mmapIter, "($self, /)"),
    PY_SLOT("__buffer__", mmapBuffer, "($self, flags, /)"),
    PY_SLOT("__release_buffer__", releaseBufferMethod, "($self, buffer, /)"),
    PY_METHOD("close", mmapClose, "($self, /)"),
    PY_METHOD("find", mmapFind<false>, "($self, view, start=None, end=None, /)"),
    PY_METHOD("rfind", mmapFind<true>, "($self, view, start=None, end=None, /)"),
    PY_METHOD("flush", mmapFlush, "($self, offset=0, size=-1, /, *, flags=0)"),
    PY_METHOD("madvise", mmapAdvise, "($self, option, start=0, length=None, /)"),
    PY_METHOD("move", mmapMove, "($self, dest, src, count, /)"),
    PY_METHOD("read", mmapRead, "($self, n=None, /)"),
    PY_METHOD("read_byte", mmapReadByte, "($self, /)"),
    PY_METHOD("readline", mmapReadLine, "($self, /)"),
#if OS(LINUX)
    PY_METHOD("resize", mmapResize, "($self, newsize, /)"),
#endif
    PY_METHOD("seek", mmapSeek, "($self, pos, whence=0, /)"),
    PY_METHOD("seekable", mmapSeekable, "($self, /)"),
    PY_METHOD("set_name", mmapSetName, "($self, name, /)"),
    PY_METHOD("size", mmapSizeMethod, "($self, /)"),
    PY_METHOD("tell", mmapTell, "($self, /)"),
    PY_METHOD("write", mmapWrite, "($self, bytes, /)"),
    PY_METHOD("write_byte", mmapWriteByte, "($self, byte, /)"),
    PY_METHOD("__enter__", mmapEnter, "($self, /)"),
    PY_METHOD("__exit__", mmapExit, "($self, exc_type, exc_value, traceback, /)"),
};

static constexpr NativeGetSet mmapGetSets[] = {
    { "closed"_s, mmapGetClosed },
};

static constexpr TypeDef mmapTypeDef { "mmap.mmap"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::HasWeakref, TypeFlag::Immutable }, mmapMethods, mmapGetSets, { } };

// ---- The module ----

#define MMAP_CONSTANT(name) { #name ""_s, static_cast<int64_t>(name) }

static constexpr PosixConstant mmapConstants[] = {
    { "ACCESS_DEFAULT"_s, MmapAccessDefault },
    { "ACCESS_READ"_s, MmapAccessRead },
    { "ACCESS_WRITE"_s, MmapAccessWrite },
    { "ACCESS_COPY"_s, MmapAccessCopy },
    MMAP_CONSTANT(PROT_EXEC),
    MMAP_CONSTANT(PROT_READ),
    MMAP_CONSTANT(PROT_WRITE),
    MMAP_CONSTANT(MAP_SHARED),
    MMAP_CONSTANT(MAP_PRIVATE),
    MMAP_CONSTANT(MAP_ANONYMOUS),
    { "MAP_ANON"_s, MAP_ANONYMOUS },
    MMAP_CONSTANT(MS_ASYNC),
    MMAP_CONSTANT(MS_INVALIDATE),
    MMAP_CONSTANT(MS_SYNC),
#ifdef MAP_DENYWRITE
    MMAP_CONSTANT(MAP_DENYWRITE),
#endif
#ifdef MAP_EXECUTABLE
    MMAP_CONSTANT(MAP_EXECUTABLE),
#endif
#ifdef MAP_POPULATE
    MMAP_CONSTANT(MAP_POPULATE),
#endif
#ifdef MAP_STACK
    MMAP_CONSTANT(MAP_STACK),
#endif
#ifdef MAP_ALIGNED_SUPER
    MMAP_CONSTANT(MAP_ALIGNED_SUPER),
#endif
#ifdef MAP_CONCEAL
    MMAP_CONSTANT(MAP_CONCEAL),
#endif
#ifdef MAP_NORESERVE
    MMAP_CONSTANT(MAP_NORESERVE),
#endif
#ifdef MAP_NOEXTEND
    MMAP_CONSTANT(MAP_NOEXTEND),
#endif
#ifdef MAP_HASSEMAPHORE
    MMAP_CONSTANT(MAP_HASSEMAPHORE),
#endif
#ifdef MAP_NOCACHE
    MMAP_CONSTANT(MAP_NOCACHE),
#endif
#ifdef MAP_JIT
    MMAP_CONSTANT(MAP_JIT),
#endif
#ifdef MAP_RESILIENT_CODESIGN
    MMAP_CONSTANT(MAP_RESILIENT_CODESIGN),
#endif
#ifdef MAP_RESILIENT_MEDIA
    MMAP_CONSTANT(MAP_RESILIENT_MEDIA),
#endif
#ifdef MAP_32BIT
    MMAP_CONSTANT(MAP_32BIT),
#endif
#ifdef MAP_TRANSLATED_ALLOW_EXECUTE
    MMAP_CONSTANT(MAP_TRANSLATED_ALLOW_EXECUTE),
#endif
#ifdef MAP_UNIX03
    MMAP_CONSTANT(MAP_UNIX03),
#endif
#ifdef MAP_TPRO
    MMAP_CONSTANT(MAP_TPRO),
#endif
    MMAP_CONSTANT(MADV_NORMAL),
    MMAP_CONSTANT(MADV_RANDOM),
    MMAP_CONSTANT(MADV_SEQUENTIAL),
    MMAP_CONSTANT(MADV_WILLNEED),
    MMAP_CONSTANT(MADV_DONTNEED),
#ifdef MADV_REMOVE
    MMAP_CONSTANT(MADV_REMOVE),
#endif
#ifdef MADV_DONTFORK
    MMAP_CONSTANT(MADV_DONTFORK),
#endif
#ifdef MADV_DOFORK
    MMAP_CONSTANT(MADV_DOFORK),
#endif
#ifdef MADV_HWPOISON
    MMAP_CONSTANT(MADV_HWPOISON),
#endif
#ifdef MADV_MERGEABLE
    MMAP_CONSTANT(MADV_MERGEABLE),
#endif
#ifdef MADV_UNMERGEABLE
    MMAP_CONSTANT(MADV_UNMERGEABLE),
#endif
#ifdef MADV_SOFT_OFFLINE
    MMAP_CONSTANT(MADV_SOFT_OFFLINE),
#endif
#ifdef MADV_HUGEPAGE
    MMAP_CONSTANT(MADV_HUGEPAGE),
#endif
#ifdef MADV_NOHUGEPAGE
    MMAP_CONSTANT(MADV_NOHUGEPAGE),
#endif
#ifdef MADV_DONTDUMP
    MMAP_CONSTANT(MADV_DONTDUMP),
#endif
#ifdef MADV_DODUMP
    MMAP_CONSTANT(MADV_DODUMP),
#endif
#ifdef MADV_FREE
    MMAP_CONSTANT(MADV_FREE),
#endif
#ifdef MADV_NOSYNC
    MMAP_CONSTANT(MADV_NOSYNC),
#endif
#ifdef MADV_AUTOSYNC
    MMAP_CONSTANT(MADV_AUTOSYNC),
#endif
#ifdef MADV_NOCORE
    MMAP_CONSTANT(MADV_NOCORE),
#endif
#ifdef MADV_CORE
    MMAP_CONSTANT(MADV_CORE),
#endif
#ifdef MADV_PROTECT
    MMAP_CONSTANT(MADV_PROTECT),
#endif
#ifdef MADV_FREE_REUSABLE
    MMAP_CONSTANT(MADV_FREE_REUSABLE),
#endif
#ifdef MADV_FREE_REUSE
    MMAP_CONSTANT(MADV_FREE_REUSE),
#endif
};

void initializeMmapModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    module->add(globalObject, "error"_s, rt.OSError());
    module->add(globalObject, "mmap"_s, rt.typeFor(mmapTypeDef));
    addPosixConstants(globalObject, module, mmapConstants);
    long pageSize = sysconf(_SC_PAGESIZE);
    module->add(globalObject, "PAGESIZE"_s, jsNumber(static_cast<int32_t>(pageSize)));
    module->add(globalObject, "ALLOCATIONGRANULARITY"_s, jsNumber(static_cast<int32_t>(pageSize)));
}

// ---- The module _posixshmem: memory that has a name, for multiprocessing.shared_memory to map ----

// `position` 0: it is the only argument.
static bool shmToName(JSGlobalObject* globalObject, JSValue value, ASCIILiteral function, unsigned position, PosixBytes& name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(value)) {
        raiseTypeError(globalObject, scope, function, position ? "() argument 1 must be str, not "_s : "() argument must be str, not "_s, posixTypeNameOfArgument(globalObject, value));
        return false;
    }
    if (failed(scope, name.assign(globalObject, strValue(value))))
        return false;
    if (name.contains('\0')) {
        raiseValueError(globalObject, scope, "embedded null character"_s);
        return false;
    }
    return true;
}

static JSValue shmOpen(JSGlobalObject* globalObject, JSValue path, JSValue flagsValue, JSValue modeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PosixBytes name;
    int flags;
    int mode = 0777;
    if (failed(scope, shmToName(globalObject, path, "shm_open"_s, 1, name)) || failed(scope, posixToInt(globalObject, flagsValue, flags)) || (modeValue && failed(scope, posixToInt(globalObject, modeValue, mode))))
        return { };
    int fd;
    do {
        fd = shm_open(name.characters(), flags, mode);
    } while (fd < 0 && errno == EINTR && !failed(scope, checkSignals(globalObject)));
    RETURN_IF_EXCEPTION(scope, { });
    if (fd < 0)
        return raiseOSError(globalObject, scope, errno, path);
    return jsNumber(fd);
}

static JSValue shmUnlink(JSGlobalObject* globalObject, JSValue path)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PosixBytes name;
    if (failed(scope, shmToName(globalObject, path, "shm_unlink"_s, 0, name)))
        return { };
    int result;
    do {
        result = shm_unlink(name.characters());
    } while (result < 0 && errno == EINTR && !failed(scope, checkSignals(globalObject)));
    RETURN_IF_EXCEPTION(scope, { });
    if (result < 0)
        return raiseOSError(globalObject, scope, errno, path);
    return jsUndefined();
}

static constexpr NativeDef shmFunctions[] = {
    PY_FUNCTION("shm_open", shmOpen, "(path, flags, mode=511)"),
    PY_FUNCTION("shm_unlink", shmUnlink, "(path, /)"),
};

void initializePosixShmemModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->addFunctions(globalObject, shmFunctions);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // !OS(WINDOWS)
