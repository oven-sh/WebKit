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
        // A class may keep the property in a variable, which compiled code writes. Its class object is the one in the closure table of this State, and is asked when a frame is named.
        State* stateOfClassObject { nullptr };
        FunctionID constructorOfClass;
        bool isOneOfSeveral { false };
    };
    using DisplayNamesByFunction = UncheckedKeyHashMap<FunctionID, DisplayName>;
    using TextsByFunction = UncheckedKeyHashMap<FunctionID, Weak<JSString>>;

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

    // HeapWords::recordedCallees. Compiled code writes nothing into the table that stands for no table.
    static const RecordedCallee* recordedCalleesOf(const FunctionTables*);
    // Where compiled code finds an entry from further out. The table grows until every call that may not have returned has a place of its own.
    void recordCallee(VM&, const void* stackPointerOfCaller, JSObject* closure);
    bool hasRecordedCallees() const { return !recordedCalleeStorage.isEmpty(); }
    // A function whose function objects nobody is seen to rename pays nothing for its frames until the run time sees one of them get a displayName. From then on every call by value comes by the run time, which records the callees that are function objects of such a function.
    // A frame that was entered before has no record.
    void recordCalleesOf(VM&, FunctionID sourceFunction);
    bool recordsCalleesOfCallsByValue() const { return !!isRenamedAtRunTime.load(std::memory_order_acquire); }
    bool recordsCalleesOf(FunctionID function) const
    {
        const uint8_t* isRenamed = isRenamedAtRunTime.load(std::memory_order_acquire);
        return isRenamed && isRenamed[function.index()];
    }
    void recordCalleeOfCallByValue(VM&, const void* stackPointerOfCaller, JSObject* closure);
    // Its frames are named by the function object that runs in them.
    bool framesAreNamedByCallee(FunctionID);
    std::span<RecordedCallee> recordedCallees() { return recordedCalleeStorage.mutableSpan().subspan(hasRecordedCallees() ? 1 : 0); }
    // Nothing unless the last call made from there recorded a function object of the function. Null: one that lies in a frame.
    std::optional<JSObject*> recordedCalleeOfCallFrom(const void* stackPointerOfCaller, FunctionID sourceFunction);
    // Those of calls that may not have returned. The others leave the table.
    template<typename Visitor> void visitRecordedCallees(VM&, Visitor&);
    void finalizeRecordedCallees(VM&);

    // Only the thread of the VM changes them: compiled code writes an entry, the run time moves the table.
    Lock recordedCalleesLock;
    Vector<RecordedCallee> recordedCalleeStorage;
    // VM::entryCountFromOutside() when the entries that a collection has left in the table were last looked at.
    unsigned entryCountAtLastCollection { 0 };
    // A byte for each function of the image, once there is such a function. It is set for every function that was made from the renamed one too.
    Vector<uint8_t> functionsRenamedAtRunTime WTF_GUARDED_BY_LOCK(namesLock);
    std::atomic<const uint8_t*> isRenamedAtRunTime { nullptr };
    TextsByFunction sourceTexts;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
