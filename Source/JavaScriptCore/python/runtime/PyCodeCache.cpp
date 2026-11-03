/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyCodeCache.h"

#include "CachedTypes.h"
#include "JSCBytecodeCacheVersion.h"
#include "JSCInlines.h"
#include "PyBytes.h"
#include "PyCommandLine.h"
#include "PyExec.h"
#include "PyFunction.h"
#include "PyHooks.h"
#include "PyInternals.h"
#include "PyMonitoring.h"
#include "PyNumbers.h"
#include "PyRange.h"
#include "PyRuntime.h"
#include "PySet.h"
#include "PyStr.h"
#include "PyTuple.h"
#include "PyVirtualFiles.h"
#include "UnlinkedFunctionCodeBlock.h"
#include <wtf/Int128.h>

#if !OS(WINDOWS)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// ---- The constants of code, for CachedTypes.cpp ----

bool constantParts(JSCell* cell, ConstantParts& parts)
{
    using Kind = ConstantParts::Kind;
    JSType jsType = cell->type();
    if (jsType == InternalFunctionType) {
        Runtime& rt = Runtime::from(cell->structure()->realm());
        if (cell == rt.AssertionError())
            parts.kind = Kind::AssertionError;
        else if (cell == rt.NotImplementedError())
            parts.kind = Kind::NotImplementedError;
        else
            return false;
        return true;
    }
    if (!isPyObjectType(jsType))
        return false;
    Runtime& rt = Runtime::from(cell->structure()->realm());
    PyType* type = uncheckedDowncast<PyObject>(cell)->type();
    switch (jsType) {
    case PyFloatType:
        if (type != rt.floatType())
            return false;
        parts.kind = Kind::Float;
        parts.numbers[0] = uncheckedDowncast<PyFloat>(cell)->value();
        return true;
    case PyComplexType:
        if (type != rt.complexType())
            return false;
        parts.kind = Kind::Complex;
        parts.numbers[0] = uncheckedDowncast<PyComplex>(cell)->real();
        parts.numbers[1] = uncheckedDowncast<PyComplex>(cell)->imaginary();
        return true;
    case PyBytesType:
        if (type != rt.bytesType())
            return false;
        parts.kind = Kind::Bytes;
        parts.bytes = uncheckedDowncast<PyBytes>(cell)->span();
        return true;
    case PyTupleType:
    case PyKwNamesType: {
        if (type != rt.tupleType())
            return false;
        parts.kind = jsType == PyTupleType ? Kind::Tuple : Kind::KeywordNames;
        auto* tuple = uncheckedDowncast<PyTuple>(cell);
        for (unsigned i = 0; i < tuple->length(); ++i)
            parts.items.append(tuple->at(i));
        return true;
    }
    case PySetType: {
        if (type != rt.frozensetType())
            return false;
        parts.kind = Kind::FrozenSet;
        JSValue item;
        for (size_t position = 0; uncheckedDowncast<PySet>(cell)->next(position, item);)
            parts.items.append(item);
        return true;
    }
    case PySliceType: {
        parts.kind = Kind::Slice;
        auto* slice = uncheckedDowncast<PySlice>(cell);
        parts.items.appendList({ slice->start(), slice->stop(), slice->step() });
        return true;
    }
    default:
        if (JSValue(cell) != rt.ellipsis())
            return false;
        parts.kind = Kind::Ellipsis;
        return true;
    }
}

JSValue constantFromParts(JSGlobalObject* globalObject, const ConstantParts& parts)
{
    using Kind = ConstantParts::Kind;
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    switch (parts.kind) {
    case Kind::Float:
        return PyFloat::create(vm, rt.structure<PyFloat>(), rt.floatType(), parts.numbers[0]);
    case Kind::Complex:
        return PyComplex::create(globalObject, parts.numbers[0], parts.numbers[1]);
    case Kind::Bytes:
        return PyBytes::create(globalObject, parts.bytes);
    case Kind::Tuple:
        return PyTuple::create(globalObject, parts.items.span());
    case Kind::KeywordNames: {
        auto* names = PyTuple::createUninitialized(vm, rt.keywordNamesStructure(), rt.tupleType(), parts.items.size());
        for (unsigned i = 0; i < parts.items.size(); ++i)
            names->initializeAt(vm, i, parts.items[i]);
        return names;
    }
    case Kind::FrozenSet: {
        PySet* set = PySet::createFrozen(globalObject);
        for (JSValue item : parts.items)
            set->add(globalObject, item);
        return set;
    }
    case Kind::Slice:
        RELEASE_ASSERT(parts.items.size() == 3);
        return PySlice::create(globalObject, parts.items[0], parts.items[1], parts.items[2]);
    case Kind::Ellipsis:
        return rt.ellipsis();
    case Kind::AssertionError:
        return rt.AssertionError();
    case Kind::NotImplementedError:
        return rt.NotImplementedError();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// ---- Telling builds apart ----

static ALWAYS_INLINE uint64_t mix(uint64_t a, uint64_t b)
{
    UInt128 product = static_cast<UInt128>(a) * b;
    return static_cast<uint64_t>(product) ^ static_cast<uint64_t>(product >> 64);
}

// Against accidents, not against intent. (The scheme is wyhash's: it goes through memory about as fast as memory can be read.)
static uint64_t checksumOf(std::span<const uint8_t> bytes)
{
    static constexpr uint64_t secret[] = { 0x2d358dccaa6c78a5ull, 0x8bb84b93962eacc9ull, 0x4b33a62ed433d4a3ull, 0x4d5a2da51de1aa47ull };
    auto load = [](const uint8_t* p) {
        uint64_t word;
        memcpy(&word, p, sizeof(word));
        return word;
    };
    const uint8_t* p = bytes.data();
    size_t left = bytes.size();
    uint64_t a = secret[0] ^ left;
    uint64_t b = a;
    uint64_t c = a;
    for (; left >= 48; left -= 48, p += 48) {
        a = mix(load(p) ^ secret[1], load(p + 8) ^ a);
        b = mix(load(p + 16) ^ secret[2], load(p + 24) ^ b);
        c = mix(load(p + 32) ^ secret[3], load(p + 40) ^ c);
    }
    a ^= b ^ c;
    for (; left >= 16; left -= 16, p += 16)
        a = mix(load(p) ^ secret[1], load(p + 8) ^ a);
    uint8_t rest[16] = { };
    memcpy(rest, p, left);
    a = mix(load(rest) ^ secret[2], load(rest + 8) ^ a);
    return mix(a ^ secret[1], bytes.size() ^ secret[3]);
}

static uint64_t checksumOf(std::span<const uint8_t> payload, uint32_t codeFlags)
{
    return mix(checksumOf(payload) ^ 0x8bb84b93962eacc9ull, codeFlags ^ 0x4d5a2da51de1aa47ull);
}

uint64_t hashOfSource(uint64_t key, std::span<const uint8_t> source)
{
    return mix(checksumOf(source) ^ 0x4b33a62ed433d4a3ull, key ^ 0x2d358dccaa6c78a5ull);
}

uint64_t codeCacheStamp()
{
    static uint64_t stamp = [] {
        // What the bytecode means depends on the compiler, on the numbers of the instructions and of the functions of the
        // runtime, on the layout of the records in the file: on the build. The linker has given that a name.
        uint64_t binary = computeJSCBinaryVersion();
#if OS(LINUX)
        if (struct stat status; !binary && !stat("/proc/self/exe", &status))
            binary = mix(static_cast<uint64_t>(status.st_mtime) ^ 0x9e3779b97f4a7c15ull, static_cast<uint64_t>(status.st_size) + 1);
#endif
        return mix(binary ^ 0x2d358dccaa6c78a5ull, (static_cast<uint64_t>(computeJSCBytecodeCacheVersion()) << 8 | sizeof(void*)) ^ 0x8bb84b93962eacc9ull);
    }();
    return stamp;
}

uint32_t pycMagicNumber()
{
    // Two bytes and "\r\n", as CPython's are, whose two bytes are a number below this. Builds that 15 bits do not tell apart
    // are told apart by the stamp that the code starts with.
    return 0x8000 | static_cast<uint32_t>(codeCacheStamp() >> 49) | '\r' << 16 | '\n' << 24;
}

// ---- marshal ----

static constexpr size_t pycHeaderSize = 16;
static constexpr size_t offsetOfPayloadInPycFile = pycHeaderSize + 1 + MarshalledCodeHeader::size;

static void appendLittleEndian(Vector<uint8_t>& out, uint64_t value, unsigned size)
{
    for (unsigned i = 0; i < size; ++i)
        out.append(static_cast<uint8_t>(value >> (8 * i)));
}

static uint64_t readLittleEndian(std::span<const uint8_t> bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes.size(); ++i)
        value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    return value;
}

void MarshalledCodeHeader::write(Vector<uint8_t>& out) const
{
    appendLittleEndian(out, stamp, 8);
    appendLittleEndian(out, checksum, 8);
    appendLittleEndian(out, payloadSize, 4);
    appendLittleEndian(out, codeFlags, 4);
    appendLittleEndian(out, reserved, 4);
}

MarshalledCodeHeader MarshalledCodeHeader::read(std::span<const uint8_t, size> bytes)
{
    MarshalledCodeHeader header;
    header.stamp = readLittleEndian(bytes.subspan(0, 8));
    header.checksum = readLittleEndian(bytes.subspan(8, 8));
    header.payloadSize = static_cast<uint32_t>(readLittleEndian(bytes.subspan(16, 4)));
    header.codeFlags = static_cast<uint32_t>(readLittleEndian(bytes.subspan(20, 4)));
    header.reserved = static_cast<uint32_t>(readLittleEndian(bytes.subspan(24, 4)));
    return header;
}

bool writeMarshalledCode(JSGlobalObject* globalObject, PyCode* code, Vector<uint8_t>& out)
{
    FunctionExecutable* executable = code->executable();
    Monitoring::OriginalInstructions originalInstructions(runtime(globalObject).monitoring());
    RefPtr<CachedBytecode> payload = encodePythonCode(getVM(globalObject), executable->unlinkedExecutable(), executable->source());
    if (!payload || payload->size() > std::numeric_limits<int32_t>::max())
        return false;
    uint32_t codeFlags = code->flags().toRaw();
    MarshalledCodeHeader header { codeCacheStamp(), checksumOf(payload->span(), codeFlags), static_cast<uint32_t>(payload->size()), codeFlags, 0 };
    out.reserveCapacity(out.size() + MarshalledCodeHeader::size + payload->size());
    header.write(out);
    out.append(payload->span());
    return true;
}

bool isOfThisBuild(const MarshalledCodeHeader& header)
{
    return header.stamp == codeCacheStamp() && !header.reserved;
}

PyCode* readMarshalledCode(JSGlobalObject* globalObject, const MarshalledCodeHeader& header, std::span<const uint8_t> bytes)
{
    ASSERT(bytes.size() == header.payloadSize);
    if (!isOfThisBuild(header))
        return nullptr;
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    // What readPycFile() has just looked through need not be looked through again.
    auto* intactFile = PyBytes::tryFrom(rt.slot(Runtime::Slot::IntactPycFile));
    rt.setSlot(Runtime::Slot::IntactPycFile, JSValue());
    bool isKnownToBeIntact = intactFile && bytes.data() == intactFile->data() + offsetOfPayloadInPycFile && bytes.size() == intactFile->length() - offsetOfPayloadInPycFile;
    if (!isKnownToBeIntact && checksumOf(bytes, header.codeFlags) != header.checksum)
        return nullptr;
    // The records are read where they are, for as long as any of the code is there, and are aligned.
    Ref payload = ThreadSafeRefCountedFixedVector<uint8_t>::create(bytes.begin(), bytes.end());
    SourceCode parentSource;
    UnlinkedFunctionExecutable* unlinked = decodePythonCode(globalObject, WTF::move(payload), parentSource);
    if (!unlinked)
        return nullptr;
    CodeInfo* info = unlinked->pythonCodeInfo();
    if (!info)
        return nullptr;
    FunctionExecutable* executable = unlinked->link(vm, nullptr, parentSource);
    auto flags = OptionSet<CodeFlag>::fromRaw(header.codeFlags);
    return flags == info->flags ? PyCode::of(globalObject, executable) : PyCode::createWithFlags(globalObject, executable, flags);
}

static void setFilenameOfAllCode(VM& vm, UnlinkedFunctionExecutable& executable, const String& path)
{
    executable.pythonCodeInfo()->filename = path;
    UnlinkedFunctionCodeBlock* codeBlock = executable.codeBlocksDecodingCached(vm).first;
    for (unsigned i = 0; i < codeBlock->numberOfFunctionExprs(); ++i)
        setFilenameOfAllCode(vm, *codeBlock->functionExpr(i), path);
}

void setCodeFilename(JSGlobalObject* globalObject, PyCode* code, const String& path)
{
    if (code->codeInfo().filename == path)
        return;
    setFilenameOfAllCode(getVM(globalObject), *code->executable()->unlinkedExecutable(), path);
}

// ---- .pyc files ----

bool hasPycMagicNumber(std::span<const uint8_t> contents)
{
    return contents.size() >= pycHeaderSize && readLittleEndian(contents.first(4)) == pycMagicNumber();
}

// The code in the contents of a .pyc file, if that is all there is in it.
static bool findMarshalledCode(std::span<const uint8_t> contents, MarshalledCodeHeader& header, std::span<const uint8_t>& payload)
{
    if (contents.size() < offsetOfPayloadInPycFile || (contents[pycHeaderSize] & 0x7f) != marshalTypeOfCode)
        return false;
    header = MarshalledCodeHeader::read(contents.subspan(pycHeaderSize + 1).first<MarshalledCodeHeader::size>());
    payload = contents.subspan(offsetOfPayloadInPycFile);
    return payload.size() == header.payloadSize;
}

PyCode* codeInPycFile(JSGlobalObject* globalObject, std::span<const uint8_t> contents)
{
    MarshalledCodeHeader header;
    std::span<const uint8_t> payload;
    if (!hasPycMagicNumber(contents) || !findMarshalledCode(contents, header, payload))
        return nullptr;
    return readMarshalledCode(globalObject, header, payload);
}

PyBytes* readPycFile(JSGlobalObject* globalObject, const String& path)
{
    Runtime& rt = runtime(globalObject);
    PyBytes* contents = nullptr;
#if OS(WINDOWS)
    auto read = readFile(path);
    if (!read)
        return nullptr;
    contents = PyBytes::create(globalObject, read->span());
#else
    auto pathBytes = encodeFilesystem(path);
    pathBytes.append(0);
    int fd = openFile(byteCast<char>(pathBytes.span()).data(), O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return nullptr;
    struct stat status;
    if (int result = fstat(fd, &status); result < 0 || !S_ISREG(status.st_mode)) {
        int errorNumber = result < 0 ? errno : S_ISDIR(status.st_mode) ? EISDIR : EIO;
        close(fd);
        errno = errorNumber;
        return nullptr;
    }
    contents = PyBytes::tryCreateUninitialized(rt.vm(), rt.structure<PyBytes>(), rt.bytesType(), status.st_size);
    size_t filled = 0;
    while (contents && filled < contents->length()) {
        ssize_t count = read(fd, contents->mutableData() + filled, contents->length() - filled);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        filled += count;
    }
    close(fd);
    // It is replaced as a whole, never written to: one that is shorter now than it was is not what anybody wrote.
    if (!contents || filled < contents->length()) {
        errno = contents ? EIO : ENOMEM;
        return nullptr;
    }
#endif
    // What another build or another Python wrote, and what is no code, is for importlib to turn down, which knows what to say.
    if (!hasPycMagicNumber(contents->span()) || contents->length() == pycHeaderSize || (contents->data()[pycHeaderSize] & 0x7f) != marshalTypeOfCode)
        return contents;
    MarshalledCodeHeader header;
    std::span<const uint8_t> payload;
    if (!findMarshalledCode(contents->span(), header, payload) || !isOfThisBuild(header) || checksumOf(payload, header.codeFlags) != header.checksum) {
        errno = EIO;
        return nullptr;
    }
    rt.setSlot(Runtime::Slot::IntactPycFile, contents);
    return contents;
}

#if !OS(WINDOWS)

// importlib.util.cache_from_source()
static String cacheFromSource(const String& path)
{
    size_t slash = path.reverseFind('/');
    String head = slash == notFound ? emptyString() : path.left(slash);
    String tail = slash == notFound ? path : path.substring(slash + 1);
    size_t dot = tail.reverseFind('.');
    StringView base = StringView(tail).left(dot == notFound || !dot ? tail.length() : dot);
    int optimization = configuration().optimizationLevel;
    String filename = optimization ? makeString(base, '.', codeCacheTag(), ".opt-"_s, optimization, ".pyc"_s) : makeString(base, '.', codeCacheTag(), ".pyc"_s);
    if (const String& prefix = configuration().pycachePrefix; !prefix.isNull()) {
        if (!head.startsWith('/'))
            return { };
        return makeString(prefix, head, '/', filename);
    }
    return makeString(head, "/__pycache__/"_s, filename);
}

static bool statSourceFile(const String& path, uint32_t& modificationTime, uint32_t& size)
{
    auto bytes = encodeFilesystem(path);
    struct stat status;
    VirtualPath fromRoot;
    bool isVirtual = toVirtualPath(byteCast<char>(bytes.span()), fromRoot);
    bytes.append(0);
    int result = isVirtual ? virtualStat(fromRoot.span(), status) : stat(byteCast<char>(bytes.span()).data(), &status);
    if (result < 0 || !S_ISREG(status.st_mode))
        return false;
    modificationTime = static_cast<uint32_t>(status.st_mtime);
    size = static_cast<uint32_t>(status.st_size);
    return true;
}

// As importlib's _write_atomic(): whoever reads the file finds all of it or none. Nothing is said if it cannot be done.
static void writePycFile(const String& path, std::span<const uint8_t> contents)
{
    auto bytes = encodeFilesystem(path);
    Vector<char, 256> real;
    VirtualPath fromRoot;
    if (toVirtualPath(byteCast<char>(bytes.span()), fromRoot)) {
        if (!toCachePath(fromRoot.span(), real))
            return;
    } else
        real.append(byteCast<char>(bytes.span()));
    if (!makeParentDirectories(real.span()))
        return;
    Vector<char, 256> temporary(real);
    auto suffix = makeString('.', getpid(), ".tmp"_s).utf8();
    temporary.append(byteCast<char>(suffix.span()));
    temporary.append('\0');
    real.append('\0');
    int fd = open(temporary.span().data(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd < 0)
        return;
    bool isWritten = true;
    while (!contents.empty()) {
        ssize_t written = write(fd, contents.data(), contents.size());
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            isWritten = false;
            break;
        }
        contents = contents.subspan(written);
    }
    close(fd);
    if (!isWritten || rename(temporary.span().data(), real.span().data()) < 0)
        unlink(temporary.span().data());
}

#endif // !OS(WINDOWS)

JSValue codeOfSourceFile(JSGlobalObject* globalObject, const String& path, const String& shownFileName)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    String cachePath;
    uint32_t modificationTime = 0;
    uint32_t size = 0;
#if !OS(WINDOWS)
    if (!statSourceFile(path, modificationTime, size))
        return { };
    cachePath = cacheFromSource(path);
    if (!cachePath.isNull()) {
        if (auto contents = readFile(cachePath)) {
            auto bytes = contents->span();
            // Flags of 0: it goes by the time and the size of the source.
            if (hasPycMagicNumber(bytes) && !readLittleEndian(bytes.subspan(4, 4)) && readLittleEndian(bytes.subspan(8, 4)) == modificationTime && readLittleEndian(bytes.subspan(12, 4)) == size) {
                if (PyCode* code = codeInPycFile(globalObject, bytes)) {
                    setCodeFilename(globalObject, code, shownFileName);
                    return code;
                }
            }
        }
    }
#else
    UNUSED_VARIABLE(modificationTime);
    UNUSED_VARIABLE(size);
#endif

    auto source = readFile(path);
    if (!source)
        return { };
    JSValue code = compileSource(globalObject, source->span(), shownFileName, "exec"_s, 0, -1);
    RETURN_IF_EXCEPTION(scope, { });

#if !OS(WINDOWS)
    if (!cachePath.isNull() && configuration().writeBytecode && source->size() == size) {
        Vector<uint8_t> contents;
        appendLittleEndian(contents, pycMagicNumber(), 4);
        appendLittleEndian(contents, 0, 4);
        appendLittleEndian(contents, modificationTime, 4);
        appendLittleEndian(contents, size, 4);
        contents.append(marshalTypeOfCode);
        if (writeMarshalledCode(globalObject, PyCode::from(code), contents))
            writePycFile(cachePath, contents.span());
    }
#endif
    return code;
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
