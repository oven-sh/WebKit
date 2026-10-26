/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFunctionTables.h"

#if ENABLE(AOT)

#include "AOTBuiltinTableRealm.h"
#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTCompiledCallee.h"
#include "AOTConvention.h"
#include "AOTFunctionDescription.h"
#include "AOTFunctionNames.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTObjectModel.h"
#include "AOTObjectWords.h"
#include "AOTProgramStrings.h"
#include "AOTSlimClosure.h"
#include "AOTState.h"
#include "AOTStaticFields.h"
#include "AOTThreshold.h"
#include "AbstractSlotVisitorInlines.h"
#include "JSCInlines.h"
#include "NativeExecutable.h"
#include "Options.h"
#include "SlotVisitorInlines.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <wtf/MathExtras.h>
#include <wtf/Threading.h>
#include <wtf/text/ASCIIFastPath.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/SymbolImpl.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static_assert(FunctionTables::numberOfExecutables == ClosureWords::numberOfExecutables);
static_assert(FunctionTables::numberOfExecutables - 1 <= ResumeClosureRequest::maxExecutable);

namespace {

struct FunctionKindFields {
    CallableKind callableKind { CallableKind::Plain };
    bool isAsync { false };
    bool isGenerator { false };
    bool takesNewTarget { false };
    bool isConstructor { false };
    bool isDerivedConstructor { false };
};

} // anonymous namespace

static consteval FunctionKindData functionKindWith(FunctionKindFields fields)
{
    FunctionKindData kind { };
    kind.callableKind = static_cast<unsigned>(fields.callableKind);
    kind.isAsync = fields.isAsync;
    kind.isGenerator = fields.isGenerator;
    kind.takesNewTarget = fields.takesNewTarget;
    kind.isConstructor = fields.isConstructor;
    kind.isDerivedConstructor = fields.isDerivedConstructor;
    return kind;
}

static consteval bool usesNotConstructibleExecutable(FunctionKindFields fields)
{
    FunctionKindData kind = functionKindWith(fields);
    return FunctionTables::executableKind(kind) == FunctionTables::ExecutableKind::NotConstructible && !FunctionTables::canConstruct(kind);
}

static_assert(FunctionTables::executableKind(functionKindWith({ })) == FunctionTables::ExecutableKind::Constructible);
static_assert(FunctionTables::canConstruct(functionKindWith({ })));
static_assert(closureWordFlags(functionKindWith({ })) == ClosureWord::Flags { });

static_assert(FunctionTables::executableKind(functionKindWith({ .isConstructor = true })) == FunctionTables::ExecutableKind::Class);
static_assert(FunctionTables::canConstruct(functionKindWith({ .isConstructor = true })));
static_assert(closureWordFlags(functionKindWith({ .isConstructor = true })) == ClosureWord::Flags { .isNotCallable = true });

static_assert(FunctionTables::executableKind(functionKindWith({ .takesNewTarget = true, .isConstructor = true, .isDerivedConstructor = true })) == FunctionTables::ExecutableKind::Class);
static_assert(closureWordFlags(functionKindWith({ .takesNewTarget = true, .isConstructor = true, .isDerivedConstructor = true }))
    == ClosureWord::Flags { .isNotCallable = true, .takesNewTarget = true });

static_assert(usesNotConstructibleExecutable({ .isAsync = true }));
static_assert(usesNotConstructibleExecutable({ .isGenerator = true }));
static_assert(usesNotConstructibleExecutable({ .isAsync = true, .isGenerator = true }));
static_assert(usesNotConstructibleExecutable({ .callableKind = CallableKind::Arrow }));
static_assert(usesNotConstructibleExecutable({ .callableKind = CallableKind::Method }));

static_assert(FunctionTables::executableKind(functionKindWith({ .callableKind = CallableKind::Bound })) == FunctionTables::ExecutableKind::BoundClosure);
static_assert(!FunctionTables::canConstruct(functionKindWith({ .callableKind = CallableKind::Bound })));

static_assert(FunctionTables::executableKind(functionKindWith({ .takesNewTarget = true })) == FunctionTables::ExecutableKind::Constructible);
static_assert(closureWordFlags(functionKindWith({ .takesNewTarget = true })) == ClosureWord::Flags { .takesNewTarget = true });

unsigned functionLengthOf(FunctionID function)
{
    const ProgramTables& tables = programTables();
    ASSERT(function.index() >= tables.closureFunctions.first && function.index() < tables.closureFunctions.end);
    return tables.functionLengthOf(function.index());
}

FunctionID sourceFunctionOf(FunctionID function)
{
    return FunctionID(programTables().sourceFunctionOf(function.index()));
}

const BoundFunctionData& boundFunctionData(FunctionID function)
{
    auto boundFunctions = programTables().boundFunctions();
    auto found = std::ranges::lower_bound(boundFunctions, function.index(), { }, &BoundFunctionData::function);
    RELEASE_ASSERT(found != boundFunctions.end() && found->function == function.index());
    return *found;
}

static const uint64_t* closureWordsByFunction(const Image& image)
{
    auto section = image.section<uint64_t>(ImageSection::ClosureWords);
    return reinterpret_cast<const uint64_t*>(reinterpret_cast<uintptr_t>(section.data()) - image.programTables().closureFunctions.omittedWordCount * sizeof(uint64_t));
}

FunctionTables::FunctionTables(VM& vm)
    : words(closureWordsByFunction(*Image::current()))
{
    RELEASE_ASSERT(!Options::useJIT());
    for (unsigned index = 0; index < numberOfExecutables; ++index) {
        auto kind = static_cast<ExecutableKind>(index);
        bool isClass = kind == ExecutableKind::Class;
        bool constructs = isClass || kind == ExecutableKind::Constructible;
        NativeExecutable* executable = vm.getHostFunction(isClass ? callClassConstructor : callCompiledClosure, ImplementationVisibility::Private, constructs ? constructCompiledClosure : callHostFunctionAsConstructor, 0, String());
        executables[index] = Strong<JSCell>(vm, executable);
    }
}

NativeExecutable* FunctionTables::executableOf(FunctionID function) const
{
    return uncheckedDowncast<NativeExecutable>(executables[static_cast<unsigned>(executableKind(functionKindData(function)))].get());
}

FunctionTables& FunctionTables::of(VM& vm)
{
    RELEASE_ASSERT(vm.hasCompiledCode());
    auto& tables = vm.aotState().functionTables;
    if (!tables)
        tables = makeUnique<FunctionTables>(vm);
    return *tables;
}

static ClosureTable& closureTableOf(JSObject* callee)
{
    return State::of(callee->structure()->realm()).objects->closures;
}

static ASCIILiteral initialNamePrefix(FunctionKindData kind)
{
    if (kind.isBound())
        return "bound "_s;
    bool isGetter = kind.accessor() == MethodKind::Getter;
    RELEASE_ASSERT(isGetter || kind.accessor() == MethodKind::Setter);
    return isGetter ? "get "_s : "set "_s;
}

String writtenFunctionName(FunctionID function)
{
    const Image& image = *Image::current();
    ASSERT(function.index() < image.programTables().numberOfFunctions());
    StringID name(image.programTables().nameOfFunction(function.index()));
    return !image.isObfuscated() && image.text().hasText(name) ? image.text().createString(name) : emptyString();
}

String initialNameProperty(FunctionID function)
{
    FunctionKindData kind = functionKindData(function);
    String written = writtenFunctionName(function);
    return kind.hasInitialNamePrefix() ? makeString(initialNamePrefix(kind), written) : written;
}

JSString* initialNamePropertyString(VM& vm, State& state, FunctionID function)
{
    if (functionKindData(function).hasInitialNamePrefix())
        return jsString(vm, initialNameProperty(function));
    return state.strings->writtenNameOfFunction(vm, function);
}

// The frames of a generator, and of an async function that may wait, run under a function object of their own.
static bool isNamedInFramesByFunction(FunctionID closureFunction)
{
    FunctionDescription description(closureFunction);
    return !functionKindData(closureFunction).isBound() && !functionKindData(sourceFunctionOf(closureFunction)).isGenerator && !description.isAsync() && !description.recordsCallee();
}

static String nameSetAtRunTime(VM& vm, FunctionID function)
{
    FunctionTables& tables = FunctionTables::of(vm);
    if (!tables.hasNamesSetAtRunTime.load(std::memory_order_acquire)) [[likely]]
        return String();
    FunctionID sourceFunction = sourceFunctionOf(function);
    Locker locker { tables.namesLock };
    return tables.namesSetAtRunTime.get(sourceFunction);
}

static String nameLookedFor(VM& vm, FunctionID function)
{
    FunctionTables& tables = FunctionTables::of(vm);
    if (!tables.hasNamesLookedFor.load(std::memory_order_acquire)) [[likely]]
        return String();
    FunctionID sourceFunction = sourceFunctionOf(function);
    {
        Locker locker { tables.namesLock };
        if (!tables.namesLookedFor.contains(sourceFunction.index()))
            return String();
    }
    String name = initialNameProperty(sourceFunction);
    return name.isEmpty() ? String() : name;
}

// Every call is made from further in than its one entry says.
static constexpr RecordedCallee emptyTable[2] { { 0, nullptr }, { std::numeric_limits<uintptr_t>::max(), nullptr } };
AOT_THRESHOLD(size_t, leastNumberOfRecordedCallees, 256);

const RecordedCallee* FunctionTables::recordedCalleesOf(const FunctionTables* tables)
{
    return (tables && tables->hasRecordedCallees() ? tables->recordedCalleeStorage.span() : std::span { emptyTable }).subspan(1).data();
}

static RecordedCallee& entryOfCallFrom(std::span<RecordedCallee> storage, uintptr_t stackPointerOfCaller)
{
    return storage[1 + (stackPointerOfCaller & storage[0].stackPointerOfCaller) / sizeof(RecordedCallee)];
}

static void setRecordedCalleeStorage(VM& vm, FunctionTables& tables, Vector<RecordedCallee>&& storage)
{
    tables.recordedCalleeStorage = WTF::move(storage);
    VMState& state = vm.aotState();
    Locker locker { state.instancesLock };
    for (Instance* instance : state.instances)
        instance->heap.recordedCallees = FunctionTables::recordedCalleesOf(&tables);
}

void FunctionTables::recordCallee(VM& vm, const void* stackPointer, JSObject* closure)
{
    Locker locker { recordedCalleesLock };
    RecordedCallee added { std::bit_cast<uintptr_t>(stackPointer), closure };
    if (hasRecordedCallees()) {
        RecordedCallee& entry = entryOfCallFrom(recordedCalleeStorage.mutableSpan(), added.stackPointerOfCaller);
        if (entry.stackPointerOfCaller <= added.stackPointerOfCaller) {
            entry = added;
            return;
        }
    }

    const StackBounds& stack = Thread::currentSingleton().stack();
    Vector<RecordedCallee, 16> recorded { added };
    for (const RecordedCallee& entry : recordedCallees()) {
        if (entry.stackPointerOfCaller > added.stackPointerOfCaller && stack.contains(std::bit_cast<void*>(entry.stackPointerOfCaller)))
            recorded.append(entry);
    }
    for (size_t numberOfEntries = std::max(leastNumberOfRecordedCallees, recordedCallees().size());; numberOfEntries *= 2) {
        // Two places on one stack are less than the stack apart.
        RELEASE_ASSERT(numberOfEntries * sizeof(RecordedCallee) <= 2 * roundUpToPowerOfTwo(stack.size()));
        Vector<RecordedCallee> storage;
        storage.fill({ 0, nullptr }, 1 + numberOfEntries);
        storage[0].stackPointerOfCaller = (numberOfEntries - 1) * sizeof(RecordedCallee);
        bool eachHasItsPlace = std::ranges::all_of(recorded, [&](const RecordedCallee& entry) {
            return !std::exchange(entryOfCallFrom(storage.mutableSpan(), entry.stackPointerOfCaller), entry).closure;
        });
        if (eachHasItsPlace) {
            setRecordedCalleeStorage(vm, *this, WTF::move(storage));
            return;
        }
    }
}

JSObject* FunctionTables::recordedCalleeOfCallFrom(const void* stackPointer)
{
    if (!hasRecordedCallees())
        return nullptr;
    uintptr_t stackPointerOfCaller = std::bit_cast<uintptr_t>(stackPointer);
    const RecordedCallee& entry = entryOfCallFrom(recordedCalleeStorage.mutableSpan(), stackPointerOfCaller);
    return entry.stackPointerOfCaller == stackPointerOfCaller ? entry.closure : nullptr;
}

template<typename Visitor>
void FunctionTables::visitRecordedCallees(Visitor& visitor, const void* stackPointer)
{
    Locker locker { recordedCalleesLock };
    for (const RecordedCallee& entry : recordedCallees()) {
        if (JSObject* closure = entry.closure; closure && entry.stackPointerOfCaller > std::bit_cast<uintptr_t>(stackPointer))
            visitor.appendUnbarriered(closure);
    }
}
template void FunctionTables::visitRecordedCallees(AbstractSlotVisitor&, const void*);
template void FunctionTables::visitRecordedCallees(SlotVisitor&, const void*);

// No entry is older than one collection unless its function object was marked.
void FunctionTables::finalizeRecordedCallees(VM& vm, const void* stackPointer)
{
    if (!hasRecordedCallees())
        return;
    Locker locker { recordedCalleesLock };
    bool isInUse = false;
    for (RecordedCallee& entry : recordedCallees()) {
        if (!entry.closure)
            continue;
        if (entry.stackPointerOfCaller > std::bit_cast<uintptr_t>(stackPointer) && vm.heap.isMarked(entry.closure))
            isInUse = true;
        else
            entry = { 0, nullptr };
    }
    if (!isInUse && recordedCallees().size() > leastNumberOfRecordedCallees)
        setRecordedCalleeStorage(vm, *this, { });
}

static JSValue ownDataProperty(JSObject* closure, UniquedStringImpl* uid)
{
    if (auto* classObject = dynamicDowncast<Closure>(closure)) {
        const FieldData* field = StaticFields::findOwnField(*classObject, uid);
        if (field && StaticFields::isInVariable(*classObject, *field))
            return classObject->state().objects->classes.readStaticField(*field);
    }
    unsigned attributes;
    PropertyOffset offset = closure->structure()->getConcurrently(uid, attributes);
    if (offset == invalidOffset || attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessorOrValue))
        return JSValue();
    return closure->getDirect(offset);
}

static String ownTextProperty(JSObject* closure, const Identifier& name)
{
    JSValue value = ownDataProperty(closure, name.impl());
    if (!value || !value.isString())
        return String();
    String text = asString(value)->tryGetValueWithoutGC();
    return text.isEmpty() ? String() : text;
}

void didChangeDisplayName(VM& vm, JSObject* closure)
{
    FunctionID closureFunction = *functionOf(closure);
    if (!isNamedInFramesByFunction(closureFunction))
        return;
    FunctionTables& tables = FunctionTables::of(vm);
    FunctionID sourceFunction = sourceFunctionOf(closureFunction);
    auto* classObject = dynamicDowncast<Closure>(closure);
    bool isClassObject = classObject && classObject->classData();
    String text = isClassObject ? String() : ownTextProperty(closure, vm.propertyNames->displayName);
    bool mayHaveDisplayName = isClassObject || !text.isNull();
    Locker locker { tables.namesLock };
    if (!mayHaveDisplayName && !tables.displayNames.contains(sourceFunction))
        return;
    auto& entry = tables.displayNames.add(sourceFunction, FunctionTables::DisplayName { }).iterator->value;
    if (entry.isOneOfSeveral)
        return;
    if (entry.functionObject && entry.functionObject != closure) {
        if (mayHaveDisplayName) {
            entry = { };
            entry.isOneOfSeveral = true;
        }
        return;
    }
    entry.functionObject = closure;
    entry.text = WTF::move(text);
    if (isClassObject) {
        entry.stateOfClassObject = &classObject->state();
        entry.constructorOfClass = closureFunction;
    }
    tables.hasDisplayNames.store(true, std::memory_order_relaxed);
}

static String displayNameInFrames(VM& vm, FunctionID function)
{
    FunctionTables& tables = FunctionTables::of(vm);
    if (!tables.hasDisplayNames.load(std::memory_order_relaxed)) [[likely]]
        return String();
    FunctionID sourceFunction = sourceFunctionOf(function);
    Locker locker { tables.namesLock };
    auto entry = tables.displayNames.find(sourceFunction);
    if (entry == tables.displayNames.end() || entry->value.isOneOfSeveral)
        return String();
    if (!entry->value.stateOfClassObject)
        return entry->value.text;
    JSCell* classObject = entry->value.stateOfClassObject->objects->closures.closureIfCreated(entry->value.constructorOfClass);
    return classObject == entry->value.functionObject ? ownTextProperty(asObject(classObject), vm.propertyNames->displayName) : String();
}

static String functionNameAsWritten(FunctionID function)
{
    FunctionKindData kind = functionKindData(function);
    if (FunctionDescription(function).codeType() == ModuleCode || kind.isCommonJSModule)
        return emptyString();
    String written = writtenFunctionName(function);
    return kind.isBound() ? makeString("bound "_s, written) : written;
}

// The engine names a frame by the property `name` that the function object got when the literal was evaluated, "get " and "set " included. The frame of a generator has no function object to ask.
static String nameInFramesOfFunctionNamedByComputedKey(FunctionID function)
{
    FunctionID source = sourceFunctionOf(function);
    return functionKindData(source).isGenerator ? emptyString() : initialNameProperty(source);
}

// The engine shows the property `name` once the function object has it as a property, which is when it is defined.
static String nameGivenTo(VM& vm, JSObject* callee)
{
    FunctionID function = *functionOf(callee);
    // Such a closure has the property from the first look for it on. A slim closure of any other kind has it from its first property on, whichever that is.
    bool hasNameOnceLookedFor = functionKindData(function).notesLooksForName();
    String name = ownTextProperty(callee, vm.propertyNames->name);
    if (!name.isNull() && (hasNameOnceLookedFor || name != initialNameProperty(function)))
        return name;
    if (!hasNameOnceLookedFor) {
        if (String name = nameLookedFor(vm, function); !name.isNull())
            return name;
    }
    return ownTextProperty(callee, vm.propertyNames->displayName);
}

// The engine runs the body of a generator, and of an async function that awaits, as a function of its own. It has the name that is written in the function and none that the function takes from a variable or a property.
static bool runsInBodyWithoutName(FunctionID function)
{
    FunctionKindData kind = functionKindData(sourceFunctionOf(function));
    return kind.hasInferredName && (kind.isGenerator || (kind.isAsync && FunctionDescription(function).isNamedByOwner()));
}

String functionName(VM& vm, FunctionID function, NameForm form, JSObject* callee)
{
    switch (form) {
    case NameForm::AsWritten:
        return functionNameAsWritten(function);
    case NameForm::InFrames:
        if (runsInBodyWithoutName(function))
            return emptyString();
        if (callee) [[unlikely]] {
            if (String name = nameGivenTo(vm, callee); !name.isNull())
                return name;
        } else if (String name = nameSetAtRunTime(vm, function); !name.isEmpty()) [[unlikely]]
            return name;
        else if (String name = nameLookedFor(vm, function); !name.isNull()) [[unlikely]]
            return name;
        else if (String displayName = displayNameInFrames(vm, function); !displayName.isNull()) [[unlikely]]
            return displayName;
        if (FunctionDescription(function).isNamedByComputedKey()) [[unlikely]]
            return nameInFramesOfFunctionNamedByComputedKey(function);
        return functionNameAsWritten(function);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static String computedFunctionName(JSGlobalObject* globalObject, const Identifier& key, MethodKind kind)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String name = key.string();
    if (UniquedStringImpl* uid = key.impl(); uid->isSymbol()) {
        auto& symbol = static_cast<SymbolImpl&>(*uid);
        if (symbol.isPrivate())
            name = StringImpl::createSubstringSharingImpl(symbol, 0, symbol.length());
        else
            name = symbol.isNullSymbol() ? emptyString() : tryMakeString('[', StringView(uid), ']');
        if (!name) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope, "Function name is too long"_s);
            return { };
        }
    }
    if (kind == MethodKind::Method)
        return name;
    bool isGetter = kind == MethodKind::Getter;
    name = tryMakeString(isGetter ? "get "_s : "set "_s, name);
    if (!name) [[unlikely]]
        throwOutOfMemoryError(globalObject, scope, isGetter ? "Getter name is too long"_s : "Setter name is too long"_s);
    return name;
}

void didDefineNameProperty(VM& vm, JSObject* closure, const String& name)
{
    FunctionID function = *functionOf(closure);
    if (!isNamedInFramesByFunction(function))
        return;
    FunctionTables& tables = FunctionTables::of(vm);
    Locker locker { tables.namesLock };
    tables.namesSetAtRunTime.set(sourceFunctionOf(function), name);
    tables.hasNamesSetAtRunTime.store(true, std::memory_order_release);
}

void didLookForNameProperty(VM& vm, JSObject* closure)
{
    FunctionID function = *functionOf(closure);
    FunctionID sourceFunction = sourceFunctionOf(function);
    if (functionKindData(function).isBound() || functionKindData(sourceFunction).isGenerator || FunctionDescription(function).isAsync())
        return;
    FunctionTables& tables = FunctionTables::of(vm);
    Locker locker { tables.namesLock };
    if (tables.namesLookedFor.add(sourceFunction.index()))
        tables.hasNamesLookedFor.store(true, std::memory_order_release);
}

void setComputedFunctionName(VM& vm, JSObject* closure, const Identifier& key, MethodKind kind)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    String name = computedFunctionName(closure->realm(), key, kind);
    RETURN_IF_EXCEPTION(scope, void());
    CompiledCallee::reifyProperties(vm, closure);
    closure->putDirect(vm, vm.propertyNames->name, jsString(vm, name), PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum);
    if (isCompiledClosure(closure)) {
        FunctionRareData* rareData = uncheckedDowncast<Closure>(closure)->ensureRareData(vm);
        rareData->setHasReifiedName();
        rareData->setHasModifiedNameForBoundOrNonHostFunction();
    }
    didDefineNameProperty(vm, closure, name);
}

JSString* functionSourceText(JSGlobalObject* globalObject, FunctionID function)
{
    const ProgramTables& tables = programTables();
    auto texts = tables.functionTexts();
    if (texts.empty())
        return nullptr;
    VM& vm = globalObject->vm();
    FunctionTables::TextsByFunction& answered = FunctionTables::of(vm).sourceTexts;
    if (auto found = answered.find(function); found != answered.end()) {
        if (JSString* text = found->value.get())
            return text;
    }
    std::span entries { tables.at<FunctionTextEntry>(texts[0].entriesOffset), texts[0].numberOfEntries };
    auto entry = std::ranges::lower_bound(entries, function.index(), { }, &FunctionTextEntry::function);
    if (entry == entries.end() || entry->function != function.index())
        return nullptr;
    std::span bytes { tables.at<Latin1Character>(texts[0].textOffset) + entry->start, entry->end - entry->start };
    JSString* text = charactersAreAllASCII(bytes) ? jsOwnedString(vm, StringImpl::createWithoutCopying(bytes)) : jsString(vm, String::fromUTF8ReplacingInvalidSequences(bytes));
    answered.set(function, Weak<JSString>(text));
    return text;
}

std::optional<FunctionID> functionOf(const JSCell* cell)
{
    switch (cell->type()) {
    case SlimClosureType:
        return uncheckedDowncast<SlimClosure>(cell)->function();
    case SlimClosureWithPropertiesType:
        return uncheckedDowncast<ReifiedSlimClosure>(cell)->function();
    case CompiledClosureType:
        return uncheckedDowncast<Closure>(cell)->function();
    default:
        return std::nullopt;
    }
}

static FunctionID checkedFunctionOf(const JSCell* callee)
{
    std::optional<FunctionID> function = functionOf(callee);
    RELEASE_ASSERT(function);
    return *function;
}

namespace CompiledCallee {

bool isCompiledCallee(const JSCell* cell)
{
    return isCompiledCalleeType(cell->type());
}

String name(JSObject* callee)
{
    return initialNameProperty(checkedFunctionOf(callee));
}

unsigned length(JSObject* callee)
{
    return functionLengthOf(checkedFunctionOf(callee));
}

JSValue sourceText(JSGlobalObject* globalObject, JSObject* callee)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    FunctionID function = checkedFunctionOf(callee);
    FunctionKindData kindData = functionKindData(function);
    CallableKind callableKind = kindData.callable();
    if (callableKind == CallableKind::Bound)
        RELEASE_AND_RETURN(scope, jsMakeNontrivialString(globalObject, "function "_s, writtenFunctionName(function), "() { [native code] }"_s));
    ASCIILiteral async = kindData.isAsync ? "async "_s : ""_s;
    bool isProgramFunction = !kindData.isInRuntimeModule;
    JSString* text = isProgramFunction ? functionSourceText(globalObject, function) : nullptr;
    RETURN_IF_EXCEPTION(scope, { });
    if (text)
        return text;
    if (isProgramFunction && callableKind == CallableKind::Arrow)
        RELEASE_AND_RETURN(scope, jsMakeNontrivialString(globalObject, async, "() => { [native code] }"_s));
    bool isWrittenWithoutName = callableKind == CallableKind::Arrow || (!isProgramFunction && kindData.hasInferredName);
    String name = isWrittenWithoutName ? emptyString() : initialNameProperty(function);
    if (kindData.isConstructor)
        RELEASE_AND_RETURN(scope, jsMakeNontrivialString(globalObject, "class "_s, name, name.isEmpty() ? ""_s : " "_s, "{ [native code] }"_s));
    if (isProgramFunction && callableKind == CallableKind::Method)
        RELEASE_AND_RETURN(scope, jsMakeNontrivialString(globalObject, async, kindData.isGenerator ? "*"_s : ""_s, name, "() { [native code] }"_s));
    RELEASE_AND_RETURN(scope, jsMakeNontrivialString(globalObject, async, kindData.isGenerator ? "function* "_s : "function "_s, name, "() { [native code] }"_s));
}

bool isBoundFunction(JSObject* callee)
{
    return functionKindData(checkedFunctionOf(callee)).isBound();
}

FunctionKind kind(JSObject* callee)
{
    FunctionID function = checkedFunctionOf(callee);
    FunctionKindData kindData = functionKindData(function);
    if (kindData.isInRuntimeModule)
        return FunctionKind::Builtin;
    if (kindData.isConstructor)
        return FunctionKind::Class;
    CallableKind callableKind = kindData.callable();
    if (callableKind == CallableKind::Bound)
        return FunctionKind::Strict;
    if (kindData.isGenerator)
        return kindData.isAsync ? FunctionKind::AsyncGenerator : FunctionKind::Generator;
    if (kindData.isAsync)
        return FunctionKind::Async;
    switch (callableKind) {
    case CallableKind::Arrow:
        return FunctionKind::Arrow;
    case CallableKind::Method:
        return FunctionKind::Method;
    case CallableKind::Plain:
        return FunctionDescription(function).isStrict() ? FunctionKind::Strict : FunctionKind::Sloppy;
    case CallableKind::Bound:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

String displayName(JSObject* object)
{
    std::optional<FunctionID> function = functionOf(object);
    if (!function)
        return String();
    if (FunctionDescription(*function).isNamedByComputedKey())
        return emptyString();
    // The engine shows a built-in function by the name in its own text alone.
    FunctionKindData kindData = functionKindData(*function);
    bool takesNameFromOwner = kindData.hasInferredName || kindData.accessor() != MethodKind::Method;
    if (kindData.isInRuntimeModule && takesNameFromOwner && !kindData.isBound())
        return emptyString();
    return writtenFunctionName(*function);
}

bool reifyProperties(VM& vm, JSObject* object)
{
    if (object->type() != SlimClosureType)
        return false;
    uncheckedDowncast<SlimClosure>(object)->reify(vm);
    return true;
}

void setPrototypeProperty(VM& vm, JSObject* callee, JSObject* prototype, unsigned attributes)
{
    RELEASE_ASSERT(isCompiledCallee(callee));
    if (callee->type() == CompiledClosureType) {
        uncheckedDowncast<Closure>(callee)->setPrototypeProperty(vm, prototype, attributes);
        return;
    }
    reifyProperties(vm, callee);
    callee->putDirect(vm, vm.propertyNames->prototype, prototype, attributes);
}

void setNameProperty(JSGlobalObject* globalObject, JSObject* callee, JSString* name, unsigned attributes)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    RELEASE_ASSERT(isCompiledCallee(callee));
    String text = name->value(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    if (callee->type() == CompiledClosureType)
        uncheckedDowncast<Closure>(callee)->setNameProperty(vm, name, attributes);
    else {
        reifyProperties(vm, callee);
        callee->putDirect(vm, vm.propertyNames->name, name, attributes);
    }
    didDefineNameProperty(vm, callee, text);
}

Structure* cachedBoundFunctionStructure(JSObject* callee)
{
    if (!isSlimClosureType(callee->type()))
        return nullptr;
    return closureTableOf(callee).asyncBoundFunctionStructure();
}

void setCachedBoundFunctionStructure(VM&, JSObject* callee, Structure* structure)
{
    if (isSlimClosureType(callee->type()))
        closureTableOf(callee).setAsyncBoundFunctionStructure(structure);
}

} // namespace CompiledCallee

DeclaredAs declaredAs(JSCell* cell)
{
    std::optional<FunctionID> function = functionOf(cell);
    if (!function)
        return DeclaredAs::NotCompiled;
    FunctionKindData kindData = functionKindData(*function);
    if (kindData.isConstructor)
        return DeclaredAs::Class;
    if (kindData.isBound())
        return DeclaredAs::Function;
    if (kindData.isGenerator)
        return kindData.isAsync ? DeclaredAs::AsyncGeneratorFunction : DeclaredAs::GeneratorFunction;
    return kindData.isAsync ? DeclaredAs::AsyncFunction : DeclaredAs::Function;
}

String closureDeclaredName(JSObject* object)
{
    std::optional<FunctionID> function = functionOf(object);
    if (!function)
        return String();
    FunctionKindData kindData = functionKindData(*function);
    bool takesNameFromOwner = kindData.hasInferredName || kindData.accessor() != MethodKind::Method;
    if (takesNameFromOwner && !kindData.isBound())
        return emptyString();
    return writtenFunctionName(*function);
}

bool belongsToBuiltinModule(JSCell* cell)
{
    std::optional<FunctionID> function = functionOf(cell);
    return function && functionKindData(*function).isInRuntimeModule;
}

bool isCompiledClassObject(JSFunction* candidate)
{
    std::optional<FunctionID> function = functionOf(candidate);
    return function && functionKindData(*function).isConstructor;
}

bool isClosureOf(JSCell* cell, FunctionID function)
{
    BuiltinID calledBuiltin = builtinOfFunctionInFrames(function);
    if (std::optional<FunctionID> closureFunction = functionOf(cell))
        return !calledBuiltin && sourceFunctionOf(*closureFunction) == sourceFunctionOf(function);
    std::optional<uint32_t> implemented = calledBuiltin ? std::optional<uint32_t>(calledBuiltin.index()) : programTables().builtinImplementedBy(function.index());
    JSObject* object = cell->getObject();
    if (!implemented || !object)
        return false;
    JSGlobalObject* realm = object->realmMayBeNull();
    if (!realm)
        return false;
    auto found = findInRealm(realm, BuiltinCellKey::object(BuiltinID(*implemented)));
    return found && *found == cell;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
