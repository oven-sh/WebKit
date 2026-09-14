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
#include "CModule.h"

#if USE(BUN_JSC_ADDITIONS)

#include "B3Compile.h"
#include "B3EstimateStaticExecutionCounts.h"
#include "B3FixSSA.h"
#include "B3Procedure.h"
#include "BIRToB3.h"
#include "Disassembler.h"
#include "ExecutableAllocator.h"
#include "FFICallingConvention.h"
#include "JITCompilation.h"
#include "JSCInlines.h"
#include "JSFFIFunction.h"
#include "ObjectConstructor.h"
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/ProcessID.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/ThreadingPrimitives.h>
#include <wtf/text/MakeString.h>

#if OS(WINDOWS)
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/mman.h>
#endif

#if ASAN_ENABLED
#include <sanitizer/lsan_interface.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace FFI {

WTF_MAKE_TZONE_ALLOCATED_IMPL(CModule);

namespace {

// A thread's copies of the `_Thread_local` objects of the modules whose code it has run.
struct ThreadLocalBlocks {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(ThreadLocalBlocks);
    ~ThreadLocalBlocks()
    {
        for (auto& entry : blocks)
            fastFree(entry.value);
    }
    UncheckedKeyHashMap<uint64_t, void*> blocks;
    unsigned destructorRounds { 0 };
};

WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(ThreadLocalBlocks);

// The blocks are freed when the thread exits, by the system's thread-specific-data destructor and not by
// a C++ thread_local one: those run before the program's own pthread_key_create and tss_create
// destructors, which may still use the thread's `_Thread_local` objects.
#if OS(WINDOWS)
// Fiber-local storage callbacks are each called once, in the order the indices were allocated in: one of the
// program's whose index was allocated after this one finds a new block with the initial values, as above.
void WINAPI destroyThreadLocalBlocks(void* blocks)
{
    delete static_cast<ThreadLocalBlocks*>(blocks);
}

ThreadLocalBlocks& threadLocalBlocks()
{
    static DWORD key = [] {
        DWORD key = FlsAlloc(destroyThreadLocalBlocks);
        RELEASE_ASSERT(key != FLS_OUT_OF_INDEXES);
        return key;
    }();
    auto* blocks = static_cast<ThreadLocalBlocks*>(FlsGetValue(key));
    if (!blocks) {
        blocks = new ThreadLocalBlocks;
        FlsSetValue(key, blocks);
    }
    return *blocks;
}
#else
WTF::ThreadSpecificKey s_threadLocalBlocksKey;

// The system calls every key's destructor, then does so again for the keys that were given a value
// meanwhile, PTHREAD_DESTRUCTOR_ITERATIONS times over. Asking for another round until the last one
// keeps the blocks there for the program's own destructors, whichever order the keys were made in.
// The last round is the last thing the system tells anyone about the thread, so the blocks go then. A
// destructor of the program's that is still being called in that round (it asked for every round itself) and
// whose key was made after this one runs after that: what it reads then is a new block with the initial values,
// which nothing frees. The C library's own thread-local storage goes after the last round, with no hook there.
void destroyThreadLocalBlocks(void* value)
{
    auto* blocks = static_cast<ThreadLocalBlocks*>(value);
    if (++blocks->destructorRounds < PTHREAD_DESTRUCTOR_ITERATIONS) {
        WTF::threadSpecificSet(s_threadLocalBlocksKey, blocks);
        return;
    }
    delete blocks;
}

ThreadLocalBlocks& threadLocalBlocks()
{
    static std::once_flag once;
    std::call_once(once, [] {
        WTF::threadSpecificKeyCreate(&s_threadLocalBlocksKey, destroyThreadLocalBlocks);
    });
    auto* blocks = static_cast<ThreadLocalBlocks*>(WTF::threadSpecificGet(s_threadLocalBlocksKey));
    if (!blocks) {
        blocks = new ThreadLocalBlocks;
        WTF::threadSpecificSet(s_threadLocalBlocksKey, blocks);
    }
    return *blocks;
}
#endif

std::atomic<uint64_t> s_nextThreadLocalKey { 1 };

// Every module that finished loading, for as long as the process lives (see CModule.h).
Lock s_loadedModulesLock;
Vector<Ref<CModule>>& loadedModules() WTF_REQUIRES_LOCK(s_loadedModulesLock)
{
    static NeverDestroyed<Vector<Ref<CModule>>> modules;
    return modules;
}

#if !OS(WINDOWS)
// `#pragma comment(lib, "sqlite3")` means what `-lsqlite3` means: libsqlite3.so (libsqlite3.dylib), the name a
// linker looks for, which many systems only have once the library's development package is installed. A name
// with a '/' or a '.' in it is handed to dlopen as it is, which is how to name a run-time library ("libz.so.1").
void* openLibrary(const CString& name)
{
    bool isBareName = !memchr(name.data(), '/', name.length()) && !memchr(name.data(), '.', name.length());
    if (isBareName) {
#if OS(DARWIN)
        CString fileName = makeString("lib"_s, name.span(), ".dylib"_s).utf8();
#else
        CString fileName = makeString("lib"_s, name.span(), ".so"_s).utf8();
#endif
        if (void* handle = dlopen(fileName.data(), RTLD_LAZY | RTLD_LOCAL))
            return handle;
    }
    return dlopen(name.data(), RTLD_LAZY | RTLD_LOCAL);
}

void* symbolIn(void* library, const char* name) { return dlsym(library, name); }
String lastLibraryError() { return String::fromUTF8(dlerror()); }
#else
bool isImportLibraryName(const CString& name)
{
    auto span = name.span();
    if (span.size() <= 4)
        return false;
    auto suffix = span.last(4);
    return suffix[0] == '.' && (suffix[1] | 0x20) == 'l' && (suffix[2] | 0x20) == 'i' && (suffix[3] | 0x20) == 'b';
}

// `#pragma comment(lib, "user32.lib")` names an import library; what it imports from is the DLL of that name.
void* openLibrary(const CString& name)
{
    auto span = name.span();
    CString fileName = name;
    if (isImportLibraryName(name))
        fileName = makeString(span.first(span.size() - 4), ".dll"_s).utf8();
    else if (!memchr(name.data(), '.', name.length()) && !memchr(name.data(), '\\', name.length()) && !memchr(name.data(), '/', name.length()))
        fileName = makeString(span, ".dll"_s).utf8();
    return LoadLibraryA(fileName.data());
}

void* symbolIn(void* library, const char* name) { return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name)); }
String lastLibraryError() { return makeString("error "_s, static_cast<unsigned>(GetLastError())); }
#endif

} // anonymous namespace

CModule::CModule(std::unique_ptr<BIR::Module>&& bir)
    : m_bir(WTF::move(bir))
    , m_threadLocalKey(s_nextThreadLocalKey++)
{
}

std::optional<std::pair<uint8_t, uint8_t>> CModule::hostTarget()
{
#if (CPU(X86_64) && (OS(WINDOWS) || OS(LINUX))) || (CPU(ARM64) && OS(DARWIN))
#if CPU(X86_64)
    constexpr BIR::Arch arch = BIR::Arch::X86_64;
#else
    constexpr BIR::Arch arch = BIR::Arch::ARM64;
#endif
#if OS(DARWIN)
    constexpr BIR::OS os = BIR::OS::Darwin;
#elif OS(WINDOWS)
    constexpr BIR::OS os = BIR::OS::Windows;
#else
    constexpr BIR::OS os = BIR::OS::Linux;
#endif
    return { { static_cast<uint8_t>(arch), static_cast<uint8_t>(os) } };
#else
    return std::nullopt;
#endif
}

void* SYSV_ABI CModule::threadLocalBase(void* context)
{
    CModule& module = *static_cast<CModule*>(context);
    auto result = threadLocalBlocks().blocks.ensure(module.m_threadLocalKey, [&] {
        const BIR::ThreadLocalData& tls = module.m_bir->tls;
        size_t size = std::max<size_t>(tls.size, 1);
        void* block = fastAlignedMalloc(std::max<size_t>(tls.alignment, 16), roundUpToMultipleOf<16>(size));
        memset(block, 0, size);
        memcpy(block, tls.initialized.span().data(), tls.initialized.size());
        for (const BIR::Reloc& reloc : tls.relocs) {
            uint64_t address = module.relocatedAddress(reloc, static_cast<uint8_t*>(block));
            memcpy(static_cast<uint8_t*>(block) + reloc.offset, &address, sizeof(address));
        }
        return block;
    });
    return result.iterator->value;
}

uint64_t CModule::relocatedAddress(const BIR::Reloc& reloc, uint8_t* threadLocalBlock) const
{
    uint64_t address = 0;
    switch (reloc.kind) {
    case BIR::RelocKind::Data:
        address = reinterpret_cast<uint64_t>(m_data + reloc.index);
        break;
    case BIR::RelocKind::Func:
        address = reinterpret_cast<uint64_t>(m_functionTable[reloc.index]);
        break;
    case BIR::RelocKind::Extern:
        address = reinterpret_cast<uint64_t>(m_externAddresses[reloc.index]);
        break;
    case BIR::RelocKind::Tls:
        address = reinterpret_cast<uint64_t>(threadLocalBlock + reloc.index);
        break;
    }
    return address + static_cast<uint64_t>(reloc.addend);
}

BIRLinkEnvironment CModule::linkEnvironment() const
{
    return { m_data, m_functionTable.span().data(), m_externAddresses.span(), const_cast<CModule*>(this), threadLocalBase };
}

// Of a module that failed to load, none of whose code has run: one that loaded is never destroyed.
CModule::~CModule()
{
    if (m_data) {
#if ASAN_ENABLED
        __lsan_unregister_root_region(m_data, m_dataAllocationSize);
#endif
        OSAllocator::decommitAndRelease(m_data, m_dataAllocationSize);
    }
#if OS(WINDOWS)
    for (void* library : m_libraries)
        FreeLibrary(static_cast<HMODULE>(library));
#else
    for (void* library : m_libraries)
        dlclose(library);
#endif
}

std::expected<Ref<CModule>, String> CModule::tryCreate(std::span<const uint8_t> bytes, const ExternResolver& resolver)
{
#if ENABLE(B3_JIT)
    if (!Options::useJIT())
        return std::unexpected<String>("compiling C requires the JIT"_s);

    auto decoded = BIR::Module::decode(bytes);
    if (!decoded)
        return std::unexpected<String>(decoded.error());
    Ref<CModule> module = adoptRef(*new CModule(WTF::move(decoded.value())));
    const BIR::Module& bir = module->bir();

    auto host = hostTarget();
    if (!host)
        return std::unexpected<String>("compiled C does not run on this platform"_s);
    if (static_cast<uint8_t>(bir.arch) != host->first || static_cast<uint8_t>(bir.os) != host->second)
        return std::unexpected<String>("BIR module was compiled for a different target"_s);

    if (bir.usesVectors && !Options::useWasmSIMD()) {
#if CPU(X86_64)
        return std::unexpected<String>("this C code uses 128-bit vectors, which need AVX on x86-64"_s);
#else
        return std::unexpected<String>("this C code uses 128-bit vectors, which are turned off (useWasmSIMD)"_s);
#endif
    }

    for (const CString& name : bir.libraries) {
        void* handle = openLibrary(name);
        if (!handle) {
#if OS(WINDOWS)
            // An import library need not have a DLL: uuid.lib, which <windows.h> names, is only constants.
            // What a missing one leaves undefined is reported by name below.
            if (isImportLibraryName(name))
                continue;
#endif
            return std::unexpected<String>(makeString("cannot load library '"_s, name.span(), "': "_s, lastLibraryError()));
        }
        module->m_libraries.append(handle);
    }

    for (const BIR::Extern& entry : bir.externs) {
        void* address = nullptr;
        for (void* library : module->m_libraries) {
            if ((address = symbolIn(library, entry.name.data())))
                break;
        }
        if (!address)
            address = resolver(entry.name);
        if (!address && !entry.isWeak)
            return std::unexpected<String>(makeString("undefined symbol '"_s, entry.name.span(), '\''));
        module->m_externAddresses.append(address);
    }

    // Whole pages, which the system hands over zeroed: the constant part is protected below.
    module->m_dataAllocationSize = roundUpToMultipleOf(pageSize(), std::max<size_t>(bir.data.size, 1));
    module->m_data = static_cast<uint8_t*>(OSAllocator::tryReserveAndCommit(module->m_dataAllocationSize));
    if (!module->m_data)
        return std::unexpected<String>("out of memory for the module's data"_s);
#if ASAN_ENABLED
    // What a file-scope object of the program's points at is reachable, as it is for a program of its own, whose
    // data the leak checker finds in its image. These pages are in no image. (A thread's `_Thread_local` objects
    // are in blocks from malloc that the thread's own storage points at, which the checker follows by itself.)
    __lsan_register_root_region(module->m_data, module->m_dataAllocationSize);
#endif
    memcpy(module->m_data, bir.data.constants.data(), bir.data.constants.size());
    memcpy(module->m_data + bir.data.readOnlySize, bir.data.writable.data(), bir.data.writable.size());

#if defined(MADV_DOFORK)
    // A C program that forks runs on in the child, up to an exec or an _exit at the least: its code has to be
    // there. OSAllocator asks the system to leave what it reserves out of a forked child, and the pool compiled
    // code goes into is one of those reservations.
    static std::once_flag keepCodeInForkedChildren;
    std::call_once(keepCodeInForkedChildren, [] {
        auto* start = startOfFixedExecutableMemoryPool<uint8_t*>();
        size_t size = endOfFixedExecutableMemoryPool<uint8_t*>() - start;
        while (madvise(start, size, MADV_DOFORK) == -1 && errno == EAGAIN) { }
    });
#endif

    module->m_functionTable.fill(nullptr, bir.functions.size());
    BIRLinkEnvironment environment = module->linkEnvironment();
    // A static function that was inlined everywhere it is called needs no code of its own, so
    // compile outward from what the outside world can reach.
    Vector<unsigned> worklist;
    Vector<bool> isQueued;
    isQueued.fill(false, bir.functions.size());
    auto enqueue = [&](unsigned functionIndex) {
        if (!isQueued[functionIndex]) {
            isQueued[functionIndex] = true;
            worklist.append(functionIndex);
        }
    };
    for (unsigned i = 0; i < bir.functions.size(); ++i) {
        if (bir.functions[i].isExported)
            enqueue(i);
    }
    for (const Vector<BIR::Reloc>* relocs : { &bir.data.relocs, &bir.tls.relocs }) {
        for (const BIR::Reloc& reloc : *relocs) {
            if (reloc.kind == BIR::RelocKind::Func)
                enqueue(static_cast<unsigned>(reloc.index));
        }
    }
    for (uint32_t constructor : bir.constructors)
        enqueue(constructor);
    for (uint32_t destructor : bir.destructors)
        enqueue(destructor);
    auto compile = [&](unsigned functionIndex) -> bool {
        B3::Procedure proc(bir.usesVectors);
#if OS(WINDOWS) && CPU(X86_64)
        // C functions and their callers on Windows expect rsi, rdi and xmm6-xmm15 to survive a call. The JIT's
        // own convention (and so B3's idea of a callee-saved register) is the System V one, where they do not.
        // The two integer registers are saved like any other; a vector register can only be saved as a
        // double, and all 128 bits have to survive, so those ten are left alone instead.
        {
            RegisterSet additional;
            additional.add(X86Registers::esi, IgnoreVectors);
            additional.add(X86Registers::edi, IgnoreVectors);
            proc.code().setAdditionalCalleeSaveRegisters(additional);
            for (auto reg : { X86Registers::xmm6, X86Registers::xmm7, X86Registers::xmm8, X86Registers::xmm9, X86Registers::xmm10,
                     X86Registers::xmm11, X86Registers::xmm12, X86Registers::xmm13, X86Registers::xmm14, X86Registers::xmm15 })
                proc.pinRegister(reg);
        }
#endif
        BIRToB3 lowering(bir, environment, proc);
        lowering.lowerFunction(functionIndex);
        for (unsigned referenced : lowering.referencedFunctions())
            enqueue(referenced);
        // C locals are B3 Variables. B3 turns those into SSA values only after its strength reduction,
        // loop-invariant hoisting and load/store elimination have run, so to those passes every use of a
        // pointer held in a local is a different, unknown address. JS and wasm arrive in SSA form already.
        proc.resetReachability();
        B3::fixSSA(proc);
        // Nothing has profiled this code. Without an estimate every block has the same weight, and the
        // register allocator spills inside loops as readily as outside them.
        B3::estimateStaticExecutionCounts(proc);

        bool dumpAllMachineCode = Options::dumpCModuleFunction() && !strcmp(Options::dumpCModuleFunction(), "*");
        bool shouldDump = Options::dumpCModuleFunction() && bir.functions[functionIndex].name == String::fromUTF8(Options::dumpCModuleFunction());
        if (shouldDump) [[unlikely]]
            dataLogLn("B3 for C function ", bir.functions[functionIndex].name, " before optimization:\n", proc);
        auto compiled = B3::tryCompile(proc, bir.functions[functionIndex].name.utf8());
        if (!compiled)
            return false;
        auto compilation = makeUnique<Compilation>(WTF::move(*compiled));
        module->m_functionTable[functionIndex] = compilation->code().untaggedPtr();
        if (dumpAllMachineCode) [[unlikely]] {
            dataLogLn("Machine code for C function ", bir.functions[functionIndex].name, " (", compilation->codeRef().size(), " bytes):");
            disassemble(compilation->codeRef().retaggedCode<DisassemblyPtrTag>(), compilation->codeRef().size(), nullptr, nullptr, "    ", WTF::dataFile());
        }
        if (shouldDump) [[unlikely]] {
            dataLogLn("Air for C function ", bir.functions[functionIndex].name, ":\n", proc.code());
            dataLogLn("Machine code for C function ", bir.functions[functionIndex].name, " (", compilation->codeRef().size(), " bytes):");
            disassemble(compilation->codeRef().retaggedCode<DisassemblyPtrTag>(), compilation->codeRef().size(), nullptr, nullptr, "    ", WTF::dataFile());
        }
#if OS(LINUX)
        if (Options::writeCModulePerfMap()) [[unlikely]] {
            // perf's JIT map format: one "<start> <size> <name>" line, in hex, per symbol.
            CString path = makeString("/tmp/perf-"_s, getCurrentProcessID(), ".map"_s).utf8();
            if (FILE* file = fopen(path.data(), "a")) {
                fprintf(file, "%llx %zx C:%s\n", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(compilation->code().untaggedPtr())), compilation->codeRef().size(), bir.functions[functionIndex].name.utf8().data());
                fclose(file);
            }
        }
#endif
        module->m_compilations.append(WTF::move(compilation));
        return true;
    };
    // The code stays for as long as the process does, in the one pool every JIT shares, so there being no room
    // left in it is an error of this load: the functions compiled so far go with the module.
    auto compileEverythingQueued = [&]() -> std::optional<String> {
        while (!worklist.isEmpty()) {
            unsigned functionIndex = worklist.takeLast();
            if (!compile(functionIndex))
                return makeString("out of executable memory for '"_s, bir.functions[functionIndex].name, '\'');
        }
        return std::nullopt;
    };
    if (auto error = compileEverythingQueued())
        return std::unexpected<String>(WTF::move(*error));

    // The optimizing JIT lowers a small exported function a second time, into its JavaScript caller, and
    // decides afresh what to inline into it. Whatever it decides, what that lowering calls has to have
    // an entry: every function named by a body it may lower, which is every body that is kept that an
    // exported one reaches.
    auto bodyIsKept = [&](const BIR::Function& function) {
        return function.insts.size() <= Options::maximumFFIInlineCInstructionCount();
    };
    {
        Vector<unsigned> mayBeLoweredAgain;
        Vector<bool> isVisited;
        isVisited.fill(false, bir.functions.size());
        auto visit = [&](unsigned functionIndex) {
            if (!isVisited[functionIndex] && bodyIsKept(bir.functions[functionIndex])) {
                isVisited[functionIndex] = true;
                mayBeLoweredAgain.append(functionIndex);
            }
        };
        for (unsigned i = 0; i < bir.functions.size(); ++i) {
            if (bir.functions[i].isExported)
                visit(i);
        }
        while (!mayBeLoweredAgain.isEmpty()) {
            for (const BIR::Inst& inst : bir.functions[mayBeLoweredAgain.takeLast()].insts) {
                if (inst.op != BIR::Op::Call && inst.op != BIR::Op::FuncAddr)
                    continue;
                enqueue(inst.a);
                visit(inst.a);
            }
        }
        if (auto error = compileEverythingQueued())
            return std::unexpected<String>(WTF::move(*error));
    }

    for (const BIR::Reloc& reloc : bir.data.relocs) {
        uint64_t address = module->relocatedAddress(reloc, nullptr);
        memcpy(module->m_data + reloc.offset, &address, sizeof(address));
    }
    if (size_t constantBytes = roundDownToMultipleOf(pageSize(), static_cast<size_t>(bir.data.readOnlySize)))
        OSAllocator::protect(module->m_data, constantBytes, true, false);

    // The bodies that stay are the ones the optimizing JIT may still inline into a JavaScript caller.
    // One of those calls, rather than inlines, a function whose body is gone.
    for (BIR::Function& function : module->m_bir->functions) {
        if (!bodyIsKept(function))
            function.releaseBody();
    }
    module->m_bir->data.constants = { };
    module->m_bir->data.writable = { };

    // From here on the program's code may run, and what it does may point into the module.
    {
        Locker locker { s_loadedModulesLock };
        loadedModules().append(module.copyRef());
    }
    return module;
#else
    UNUSED_PARAM(bytes);
    UNUSED_PARAM(resolver);
    return std::unexpected<String>("compiling C requires the FTL JIT backend"_s);
#endif
}

void CModule::runConstructors()
{
    Locker locker { m_constructorsLock };
    if (m_didRunConstructors)
        return;
    m_didRunConstructors = true;
    for (uint32_t constructor : m_bir->constructors)
        reinterpret_cast<void (*)()>(m_functionTable[constructor])();
}

JSObject* CModule::createExportsObject(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSObject* result = constructEmptyObject(globalObject);
    for (const BIR::Export& entry : m_bir->exports) {
        // This VM's own copy: another VM may be doing the same on its thread.
        String name = entry.name.isolatedCopy();
        JSFFIFunction* function = JSFFIFunction::create(vm, globalObject, globalObject->ffiFunctionStructure(), Ref { *entry.signature }, entrypoint(entry.function), name);
        RETURN_IF_EXCEPTION(scope, nullptr);
        function->setCModule(*this, entry.function);
        // A C function can be given any name (`int f(void) __asm__("7");`), one that reads as an index too.
        result->putDirectMayBeIndex(globalObject, Identifier::fromString(vm, name), function);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    return result;
}

} } // namespace JSC::FFI

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // USE(BUN_JSC_ADDITIONS)
