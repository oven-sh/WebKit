/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTClosureWord.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTProgramTables.h"
#include "AOTVector.h"
#include "Strong.h"
#include "Weak.h"
#include <array>
#include <atomic>
#include <optional>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/MallocSpan.h>
#include <wtf/SparseBitVector.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class JSCell;
class JSObject;
class JSString;
class NativeExecutable;
class Structure;
class VM;

namespace AOT {

struct State;

ALWAYS_INLINE const ProgramTables& programTables()
{
    return Image::current()->programTables();
}

// A closure says its closure number. What the collector and the calls ask of it goes by that number. Whoever wants the function asks the tables once more, out of line.
JS_EXPORT_PRIVATE uint32_t closureNumberOf(FunctionID);
JS_EXPORT_PRIVATE FunctionID closureFunctionWithNumber(uint32_t);
inline FunctionID functionOfClosureWord(uint64_t word) { return closureFunctionWithNumber(ClosureWord::closureNumberOf(word)); }
JS_EXPORT_PRIVATE const Image& imageWithClosureNumber(uint32_t number);
// Of the image that gave the number out. Where a realm runs several images, a closure says by its number which of them it is of, and any thread may ask.
// A process with one image pays nothing for that: it takes the road of Image::current().
ALWAYS_INLINE const Image& imageOfClosureNumber(uint32_t number)
{
    if (const Image* image = Image::sharedByAllThreads()) [[likely]]
        return *image;
    return imageWithClosureNumber(number);
}
ALWAYS_INLINE const ProgramTables& programTablesWithClosureNumber(uint32_t number) { return imageOfClosureNumber(number).programTables(); }
ALWAYS_INLINE FunctionKindData kindOfClosureNumber(uint32_t number) { return programTablesWithClosureNumber(number).kindOfClosureNumber(number); }
ALWAYS_INLINE CaptureCounts captureCountsOfClosureNumber(uint32_t number) { return programTablesWithClosureNumber(number).captureCountsOfClosureNumber(number); }

ALWAYS_INLINE FunctionKindData functionKindData(FunctionID function)
{
    return programTables().kindOfFunction(function.index());
}

ALWAYS_INLINE CaptureCounts captureCountsOf(FunctionID function)
{
    const ProgramTables& tables = programTables();
    ASSERT(function.index() >= tables.closureFunctions.first && function.index() < tables.closureFunctions.end);
    return tables.captureCountsOf(function.index());
}

// While it lives, the thread's image is the one that gave the closure number out. For whoever asks the tables and the stubs of the image alone: the VM goes on with the image that runs.
class ImageOfClosureNumberScope {
    WTF_MAKE_NONCOPYABLE(ImageOfClosureNumberScope);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    ALWAYS_INLINE explicit ImageOfClosureNumberScope(uint32_t number)
    {
        if (!Image::sharedByAllThreads()) [[unlikely]] {
            m_previous = Image::current();
            Image::setCurrentOfThread(&imageWithClosureNumber(number));
        }
    }
    ALWAYS_INLINE ~ImageOfClosureNumberScope()
    {
        if (m_previous) [[unlikely]]
            Image::setCurrentOfThread(m_previous);
    }

private:
    const Image* m_previous { nullptr };
};

unsigned functionLengthOf(FunctionID);
FunctionID sourceFunctionOf(FunctionID);
const BoundFunctionData& boundFunctionData(FunctionID);

constexpr ClosureWord::Flags closureWordFlags(FunctionKindData kind)
{
    return { .isNotCallable = !!kind.isConstructor, .takesNewTarget = !!kind.takesNewTarget };
}

struct FunctionTables {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FunctionTables);

    static FunctionTables& of(VM&);
    static FunctionTables& createFor(VM&);
    explicit FunctionTables(VM&);

    enum class ExecutableKind : uint8_t { NotConstructible, Constructible, Class, BoundClosure };
    static constexpr unsigned numberOfExecutables = 4;
    static_assert(static_cast<unsigned>(ExecutableKind::BoundClosure) + 1 == numberOfExecutables);

    static constexpr ExecutableKind executableKind(FunctionKindData kind)
    {
        if (kind.isConstructor)
            return ExecutableKind::Class;
        switch (kind.callable()) {
        case CallableKind::Plain:
            return kind.isAsync || kind.isGenerator ? ExecutableKind::NotConstructible : ExecutableKind::Constructible;
        case CallableKind::Arrow:
        case CallableKind::Method:
            return ExecutableKind::NotConstructible;
        case CallableKind::Bound:
            return ExecutableKind::BoundClosure;
        }
        RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
    }
    static constexpr bool canConstruct(FunctionKindData kind) { return executableKind(kind) == ExecutableKind::Constructible || executableKind(kind) == ExecutableKind::Class; }
    // JavaScriptCore creates a built-in function written in JavaScript without a prototype property (JSFunction::mayHaveNonReifiedPrototype), and the definition of a class adds one.
    static constexpr bool hasPrototypePropertyAtCreation(FunctionKindData kind)
    {
        if (kind.isConstructor)
            return true;
        if (kind.isInRuntimeModule)
            return false;
        return kind.isGenerator || executableKind(kind) == ExecutableKind::Constructible;
    }
    NativeExecutable* executableOf(FunctionID) const;

    using NamesByFunction = UncheckedKeyHashMap<FunctionID, String>;
    // Compiled code calls a function without its function object, so the function may run after the object is gone. What frames show is written down with each change of the property.
    struct DisplayName {
        // Null: the function object has no own data property `displayName` that is a string with text.
        String text;
        // Tells the next change by the same function object from one by another. Never followed.
        const JSObject* functionObject { nullptr };
        // Of the realm of the function object. The tables belong to the VM and outlive a realm, so the entry goes in the collection that finds the realm dead.
        State* state { nullptr };
        // A class may keep the property in a variable, which compiled code writes. Its class object is the one in the closure table of the State, and is asked when a frame is named.
        FunctionID constructorOfClass;
        bool isOneOfSeveral { false };
    };
    using DisplayNamesByFunction = UncheckedKeyHashMap<FunctionID, DisplayName>;
    using TextsByFunction = UncheckedKeyHashMap<FunctionID, Weak<JSString>>;

    // By closure number.
    const uint64_t* words;
    std::array<Strong<JSCell>, numberOfExecutables> executables;
    // The built-in functions of a realm are created before its ClosureTable. Until the table visits the Structures of those closures, nothing else keeps them alive.
    Vector<Strong<Structure>> closureStructuresWithoutTable;
    Lock namesLock;
    NamesByFunction namesSetAtRunTime WTF_GUARDED_BY_LOCK(namesLock);
    NamesByFunction namesShownUntilLookedFor WTF_GUARDED_BY_LOCK(namesLock);
    DisplayNamesByFunction displayNames WTF_GUARDED_BY_LOCK(namesLock);
    // By the function as it is written.
    SparseBitVector<> namesLookedFor WTF_GUARDED_BY_LOCK(namesLock);
    std::atomic<bool> hasNamesSetAtRunTime { false };
    std::atomic<bool> hasNamesLookedFor { false };
    std::atomic<bool> hasDisplayNames { false };
    void forgetDisplayNamesOf(const State&);

    // HeapWords::recordedCallees. Compiled code writes nothing into the table that stands for no table.
    static const RecordedCallee* recordedCalleesOf(const FunctionTables*);
    // Where compiled code finds an entry from further out. The table grows until every call that may not have returned has a place of its own.
    void recordCallee(VM&, const void* stackPointerOfCaller, JSObject* closure);
    bool hasRecordedCallees() const { return !recordedCalleeStorage.isEmpty(); }
    // A function whose function objects nobody is seen to rename pays nothing for its frames. The run time marks the function object that it sees get a name, and a marked one is entered through the adapter, whose frame holds it.
    // From the first such name on a tail call by value comes by the run time: it gives up the frame above whatever holds the callee, so the holder is told who runs there now.
    // A frame that was entered before the name was given has no holder.
    void recordCalleesOf(VM&, FunctionID closureFunction);
    bool recordsCalleesOfCallsByValue() const { return !!isRenamedAtRunTime.load(std::memory_order_acquire); }
    bool recordsCalleesOf(FunctionID function) const
    {
        const uint8_t* isRenamed = isRenamedAtRunTime.load(std::memory_order_acquire);
        return isRenamed && isRenamed[sourceFunctionOf(function).index()];
    }
    // Otherwise every frame is named by its function.
    bool framesMayBeNamedByCallee() const { return hasRecordedCallees() || recordsCalleesOfCallsByValue() || (programTables().flags & ProgramTables::callersKeepCallees); }
    // Its frames are named by the function object that runs in them.
    bool framesAreNamedByCallee(FunctionID);
    std::span<RecordedCallee> recordedCallees() { return recordedCalleeStorage.mutableSpan().subspan(hasRecordedCallees() ? 1 : 0); }
    // Nothing unless the last call made from there recorded a function object of the function. Null: one that lies in a frame.
    std::optional<JSObject*> recordedCalleeOfCallFrom(const void* stackPointerOfCaller, FunctionID sourceFunction);
    // For a call that enters the function without its function object: the caller keeps the function object, or the environment that holds it, in a word of its own frame. Returns nullopt unless a function object of the function is found there, and null for one that lives in a frame or was never created.
    std::optional<JSObject*> calleeKeptInFrame(VM&, const void* frameRecordOfCaller, CalleeOfSite, FunctionID sourceFunction);
    // Those of calls that may not have returned. The others leave the table.
    template<typename Visitor> void visitRecordedCallees(VM&, Visitor&, uint64_t phaseOfCollector);
    void finalizeRecordedCallees(VM&, uint64_t phaseOfCollector);
    // While nobody has entered the program every call has returned. Otherwise an entry stays until the second collection behind it, and with it the realm of its function.
    void forgetRecordedCalleesIfNotEntered(VM&);

    // Only the thread of the VM changes them: compiled code writes an entry, the run time moves the table.
    Lock recordedCalleesLock;
    Vector<RecordedCallee> recordedCalleeStorage;
    // VM::entryCountFromOutside() when the entries that a collection has left in the table were last looked at.
    unsigned entryCountAtLastCollection { 0 };
    // The program stands still from the last phase in which a collection scans the stack to the phase that ends it. So the end goes by the places that the walk beside that scan found filled.
    std::optional<uint64_t> phaseOfLastWalkOfRecordedCallees;
    Vector<uint32_t> placesFilledAtLastWalk;
    // One byte per function of the image, allocated once such a function exists. It is set for the source and for the functions that implement its closures, which is what the stubs check.
    MallocSpan<uint8_t> functionsRenamedAtRunTime WTF_GUARDED_BY_LOCK(namesLock);
    std::atomic<const uint8_t*> isRenamedAtRunTime { nullptr };
    TextsByFunction sourceTexts;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
