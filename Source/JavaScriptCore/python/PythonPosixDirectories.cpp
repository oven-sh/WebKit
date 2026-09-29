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
#include "PythonPosix.h"

#if OS(UNIX)

#include <dirent.h>
#include <errno.h>
#include <unistd.h>

// os.scandir() and os.DirEntry, and the classes of what the functions about files return: Modules/posixmodule.c of CPython.

namespace JSC { namespace Python {

int duplicateDescriptor(JSGlobalObject*, int);

namespace {

struct DirEntryState final : NativeState {
    PYTHON_NATIVE_STATE(DirEntryState);

    WriteBarrier<Unknown> name;
    WriteBarrier<Unknown> path;
    WriteBarrier<Unknown> stat; // Each of these two once it has been asked for.
    WriteBarrier<Unknown> lstat;
    int directory { defaultDirectoryDescriptor };
    unsigned char type { DT_UNKNOWN };
    ino_t inode { 0 };
};

template<typename Visitor>
void DirEntryState::visit(Visitor& visitor)
{
    visitor.append(name);
    visitor.append(path);
    visitor.append(stat);
    visitor.append(lstat);
}

struct ScandirState final : NativeState {
    PYTHON_NATIVE_STATE(ScandirState);

    ~ScandirState() { close(); }

    // ScandirIterator_closedir()
    void close()
    {
        DIR* open = std::exchange(directory, nullptr);
        if (!open)
            return;
        if (descriptor != -1)
            ::rewinddir(open);
        ::closedir(open);
    }

    DIR* directory { nullptr };
    CString narrow;
    bool hasNarrow { false };
    bool givesBytes { false };
    int descriptor { -1 };
    WriteBarrier<Unknown> object; // What it was asked for with, for what is raised to say.
};

template<typename Visitor>
void ScandirState::visit(Visitor& visitor)
{
    visitor.append(object);
}

// DirEntry_fetch_stat()
JSValue fetchStat(JSGlobalObject* globalObject, DirEntryState& entry, bool followsSymlinks)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto path = toFileSystemEncoded(globalObject, entry.path.get());
    RETURN_IF_EXCEPTION(scope, { });
    struct stat status;
    int result;
    if (entry.directory != defaultDirectoryDescriptor)
        result = ::fstatat(entry.directory, path->data(), &status, followsSymlinks ? 0 : AT_SYMLINK_NOFOLLOW);
    else
        result = followsSymlinks ? ::stat(path->data(), &status) : ::lstat(path->data(), &status);
    if (result)
        return raisePathObjectError(globalObject, scope, entry.path.get());
    RELEASE_AND_RETURN(scope, statResultFrom(globalObject, status));
}

JSValue getLstat(JSGlobalObject* globalObject, JSCell* self, DirEntryState& entry)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!entry.lstat) {
        JSValue status = fetchStat(globalObject, entry, false);
        RETURN_IF_EXCEPTION(scope, { });
        entry.lstat.set(vm, self, status);
    }
    return entry.lstat.get();
}

std::optional<bool> testMode(JSGlobalObject*, JSCell* self, DirEntryState&, bool followsSymlinks, mode_t);

std::optional<bool> isSymlink(JSGlobalObject* globalObject, JSCell* self, DirEntryState& entry)
{
    if (entry.type != DT_UNKNOWN)
        return entry.type == DT_LNK;
    return testMode(globalObject, self, entry, false, S_IFLNK);
}

// os_DirEntry_stat_impl()
JSValue getStat(JSGlobalObject* globalObject, JSCell* self, DirEntryState& entry, bool followsSymlinks)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!followsSymlinks)
        RELEASE_AND_RETURN(scope, getLstat(globalObject, self, entry));
    if (!entry.stat) {
        auto isLink = isSymlink(globalObject, self, entry);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue status = *isLink ? fetchStat(globalObject, entry, true) : getLstat(globalObject, self, entry);
        RETURN_IF_EXCEPTION(scope, { });
        entry.stat.set(vm, self, status);
    }
    return entry.stat.get();
}

// DirEntry_test_mode()
std::optional<bool> testMode(JSGlobalObject* globalObject, JSCell* self, DirEntryState& entry, bool followsSymlinks, mode_t bits)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isLink = entry.type == DT_LNK;
    if (entry.type == DT_UNKNOWN || (followsSymlinks && isLink)) {
        JSValue status = getStat(globalObject, self, entry, followsSymlinks);
        if (scope.exception()) {
            // If it is not there any more, it is not a file, and it is not a directory.
            if (catchException(globalObject, BuiltinType::FileNotFoundError))
                return false;
            return std::nullopt;
        }
        JSValue mode = getAttribute(globalObject, status, Identifier::fromString(vm, "st_mode"_s));
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        auto number = toCLong(globalObject, mode);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        return (static_cast<mode_t>(*number) & S_IFMT) == bits;
    }
    if (isLink)
        return false;
    return bits == S_IFDIR ? entry.type == DT_DIR : entry.type == DT_REG;
}

} // anonymous namespace

enum class EntryTest : uint8_t { IsSymlink, IsJunction, IsDir, IsFile };

// is_symlink(), is_junction(), is_dir(*, follow_symlinks=True) and is_file(*, follow_symlinks=True)
PYTHON_NATIVE(dirEntryTest)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& entry = stateOf<DirEntryState>(self);
    auto which = unpack<EntryTest>(callFrame, 0);
    if (which == EntryTest::IsJunction)
        return JSValue::encode(jsBoolean(false));
    std::optional<bool> result;
    if (which == EntryTest::IsSymlink)
        result = isSymlink(globalObject, self, entry);
    else {
        bool followsSymlinks = true;
        if (JSValue value = args.at(1)) {
            followsSymlinks = isTrue(globalObject, value);
            RETURN_IF_EXCEPTION(scope, { });
        }
        result = testMode(globalObject, self, entry, followsSymlinks, which == EntryTest::IsDir ? S_IFDIR : S_IFREG);
    }
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(*result));
}

PYTHON_NATIVE(dirEntryStat)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    bool followsSymlinks = true;
    if (JSValue value = args.at(1)) {
        followsSymlinks = isTrue(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(getStat(globalObject, self, stateOf<DirEntryState>(self), followsSymlinks)));
}

PYTHON_NATIVE(dirEntryInode)
{
    NativeArguments args(callFrame);
    return JSValue::encode(intFromUInt64(globalObject, stateOf<DirEntryState>(args[0]).inode));
}

PYTHON_NATIVE(dirEntryFspath)
{
    NativeArguments args(callFrame);
    UNUSED_PARAM(globalObject);
    return JSValue::encode(stateOf<DirEntryState>(args[0]).path.get());
}

PYTHON_NATIVE(dirEntryRepr)
{
    NATIVE_PROLOGUE();
    String name = repr(globalObject, stateOf<DirEntryState>(args[0]).name.get());
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate("<DirEntry "_s, name, '>'))));
}

// DirEntry_from_posix_info()
static JSValue newDirEntry(JSGlobalObject* globalObject, ScandirState& iterator, const struct dirent& found)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto make = [&] (std::span<const char> characters) -> JSValue {
        if (iterator.givesBytes)
            return newBytes(globalObject, byteCast<uint8_t>(characters));
        return decodeFileSystemBytes(globalObject, characters);
    };
    auto nameCharacters = unsafeSpan(found.d_name);
    JSValue name = make(nameCharacters);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue path = name;
    if (iterator.descriptor == -1) {
        // join_path_filename()
        Vector<char> joined;
        if (iterator.hasNarrow)
            joined.append(iterator.narrow.span());
        else
            joined.append('.');
        if (!joined.isEmpty() && joined.last() != '/')
            joined.append('/');
        joined.append(nameCharacters);
        path = make(joined.span());
        RETURN_IF_EXCEPTION(scope, { });
    }
    auto state = makeUnique<DirEntryState>();
    auto& entry = *state;
    entry.directory = iterator.descriptor != -1 ? iterator.descriptor : defaultDirectoryDescriptor;
    entry.type = found.d_type;
    entry.inode = found.d_ino;
    JSObject* result = PyStateObject::create(vm, posixState(globalObject).dirEntry->instanceStructure(), WTF::move(state));
    entry.name.set(vm, result, name);
    entry.path.set(vm, result, path);
    return result;
}

PYTHON_NATIVE(scandirNext)
{
    NATIVE_PROLOGUE();
    auto& iterator = stateOf<ScandirState>(args[0]);
    // It has been gone through, or closed.
    while (iterator.directory) {
        errno = 0;
        struct dirent* found = ::readdir(iterator.directory);
        if (!found) {
            int error = errno;
            iterator.close();
            if (error) {
                errno = error;
                return JSValue::encode(raisePathObjectError(globalObject, scope, iterator.object.get()));
            }
            break;
        }
        const char* name = found->d_name;
        if (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2])))
            continue;
        JSValue entry = newDirEntry(globalObject, iterator, *found);
        if (scope.exception())
            iterator.close();
        return JSValue::encode(entry);
    }
    return JSValue::encode(raise(globalObject, scope, BuiltinType::StopIteration, JSValue()));
}

// close() and __exit__()
PYTHON_NATIVE(scandirClose)
{
    NativeArguments args(callFrame);
    UNUSED_PARAM(globalObject);
    stateOf<ScandirState>(args[0]).close();
    RETURN_NONE();
}

// ScandirIterator_finalize()
PYTHON_NATIVE(scandirDel)
{
    NATIVE_PROLOGUE();
    auto& iterator = stateOf<ScandirState>(args[0]);
    if (!iterator.directory)
        RETURN_NONE();
    iterator.close();
    Exception* raised = takeRaisedException(vm);
    RETURN_IF_EXCEPTION(scope, { });
    String shown = repr(globalObject, args[0]);
    if (!scope.exception())
        warn(globalObject, BuiltinType::ResourceWarning, concatenate("unclosed scandir iterator "_s, shown), 1, args[0]);
    // What has nothing to do with the warning can come up when everything is being taken down.
    if (scope.exception() && isInstance(globalObject, scope.exception()->value(), realm->type(BuiltinType::Warning)))
        reportUnraisable(globalObject, concatenate("Exception ignored while finalizing scandir iterator "_s, shown));
    if (scope.exception() && !scope.tryClearException())
        return { };
    restoreRaisedException(globalObject, raised);
    RETURN_NONE();
}

// __enter__() and __iter__()
PYTHON_NATIVE(scandirSelf)
{
    NativeArguments args(callFrame);
    UNUSED_PARAM(globalObject);
    return JSValue::encode(args[0]);
}

PYTHON_NATIVE(posixScandir)
{
    NATIVE_PROLOGUE();
    PathArgument path("scandir"_s, "path"_s, PathArgument::Nullable | PathArgument::AllowsDescriptor);
    if (!path.convert(globalObject, args.at(0)))
        return { };
    if (!audit(globalObject, "os.scandir"_s, path.object ? path.object : jsUndefined()))
        return { };
    auto state = makeUnique<ScandirState>();
    auto& iterator = *state;
    iterator.narrow = path.bytes;
    iterator.hasNarrow = path.hasNarrow;
    iterator.givesBytes = path.hasNarrow && path.isBytes(globalObject);
    iterator.descriptor = path.descriptor;
    errno = 0;
    int descriptor = -1;
    if (path.descriptor != -1) {
        // closedir() closes it, so it is another that is given up.
        descriptor = duplicateDescriptor(globalObject, path.descriptor);
        if (descriptor == -1)
            return { };
        iterator.directory = ::fdopendir(descriptor);
    } else
        iterator.directory = ::opendir(path.hasNarrow ? path.narrow() : ".");
    if (!iterator.directory) {
        raisePathError(globalObject, scope, path);
        if (descriptor != -1)
            ::close(descriptor);
        return { };
    }
    JSObject* result = PyStateObject::create(vm, posixState(globalObject).scandirIterator->instanceStructure(), WTF::move(state));
    if (path.object)
        iterator.object.set(vm, result, path.object);
    return JSValue::encode(result);
}

// statresult_new(): made from a tuple, it may have None for the times as floats, and then they are what the times are as ints.
PYTHON_NATIVE(statResultNew)
{
    NATIVE_PROLOGUE();
    JSValue result = callWithKeywords(globalObject, posixState(globalObject).newOfStatResult.get(), args.allFrom(0), args.keywordNames());
    RETURN_IF_EXCEPTION(scope, { });
    PyTuple* tuple = asTuple(result);
    PyTuple* hidden = asTuple(tuple->getDirect(vm, vm.pythonNames().private_hiddenFields));
    for (unsigned i = 0; i < 3; ++i) {
        if (isNone(hidden->at(i)))
            hidden->initializeAt(vm, i, tuple->at(7 + i));
    }
    return JSValue::encode(result);
}

void initializePosixFileTypes(JSGlobalObject* globalObject, PosixModuleState& state)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    using Arguments = PyNativeFunction::Arguments;

    auto makeSequence = [&] (WriteBarrier<PyType>& slot, ASCIILiteral name, std::initializer_list<ASCIILiteral> fields, unsigned countInSequence) {
        PyType* type = createBuiltinType(globalObject, name, realm->typeTuple(), PyType::Layout::Tuple, PyType::IsSequence | PyType::IsDerivedFromBuiltin);
        slot.set(vm, realm, type);
        makeStructSequenceType(globalObject, type, std::span(fields.begin(), fields.size()), countInSequence);
        return type;
    };
    PyType* statResult = makeSequence(state.statResult, "os.stat_result"_s, {
        "st_mode"_s, "st_ino"_s, "st_dev"_s, "st_nlink"_s, "st_uid"_s, "st_gid"_s, "st_size"_s, { }, { }, { },
        "st_atime"_s, "st_mtime"_s, "st_ctime"_s, "st_atime_ns"_s, "st_mtime_ns"_s, "st_ctime_ns"_s, "st_blksize"_s, "st_blocks"_s, "st_rdev"_s,
#if OS(DARWIN)
        "st_flags"_s, "st_gen"_s, "st_birthtime"_s,
#endif
    }, 10);
    state.newOfStatResult.set(vm, realm, statResult->getDirect(vm, vm.pythonNames().dunder_new));
    addMethods(globalObject, statResult, { { "__new__"_s, statResultNew, Kind::New, 0, "structseq(sequence, dict={})"_s, Arguments::AreNotChecked } });
    makeSequence(state.statVFSResult, "os.statvfs_result"_s, { "f_bsize"_s, "f_frsize"_s, "f_blocks"_s, "f_bfree"_s, "f_bavail"_s, "f_files"_s, "f_ffree"_s, "f_favail"_s, "f_flag"_s, "f_namemax"_s, "f_fsid"_s }, 10);
    makeSequence(state.terminalSize, "os.terminal_size"_s, { "columns"_s, "lines"_s }, 2);

    PyType* dirEntry = createBuiltinType(globalObject, "posix.DirEntry"_s, realm->typeObject(), PyType::Layout::Native, 0);
    state.dirEntry.set(vm, realm, dirEntry);
    dirEntry->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, dirEntry));
    addMethods(globalObject, dirEntry, {
        { "__repr__"_s, dirEntryRepr },
        { "is_dir"_s, dirEntryTest, Kind::Method, pack(EntryTest::IsDir), { }, Arguments::AreCheckedAsWithDefiningClass },
        { "is_file"_s, dirEntryTest, Kind::Method, pack(EntryTest::IsFile), { }, Arguments::AreCheckedAsWithDefiningClass },
        { "is_symlink"_s, dirEntryTest, Kind::Method, pack(EntryTest::IsSymlink), { }, Arguments::AreCheckedAsWithDefiningClass },
        { "is_junction"_s, dirEntryTest, Kind::Method, pack(EntryTest::IsJunction) },
        { "stat"_s, dirEntryStat, Kind::Method, 0, { }, Arguments::AreCheckedAsWithDefiningClass },
        { "inode"_s, dirEntryInode },
        { "__fspath__"_s, dirEntryFspath },
    });
    addMember(globalObject, dirEntry, "name"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return stateOf<DirEntryState>(self).name.get(); });
    addMember(globalObject, dirEntry, "path"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return stateOf<DirEntryState>(self).path.get(); });

    PyType* iterator = createBuiltinType(globalObject, "posix.ScandirIterator"_s, realm->typeObject(), PyType::Layout::Native, 0);
    state.scandirIterator.set(vm, realm, iterator);
    iterator->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, iterator));
    addMethods(globalObject, iterator, {
        { "__iter__"_s, scandirSelf },
        { "__next__"_s, scandirNext },
        { "__del__"_s, scandirDel },
        { "__enter__"_s, scandirSelf, Kind::Method, 0, "($self, /)"_s },
        { "__exit__"_s, scandirClose, Kind::Method, 0, "($self, /, *args)"_s, Arguments::AreNotChecked },
        { "close"_s, scandirClose, Kind::Method, 0, "($self, /)"_s },
    });
}

void addPosixScandir(JSGlobalObject* globalObject, JSObject* module)
{
    addFunction(globalObject, module, "scandir"_s, posixScandir);
}

} } // namespace JSC::Python

#endif // OS(UNIX)
