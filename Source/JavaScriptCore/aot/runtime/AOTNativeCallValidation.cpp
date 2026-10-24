/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNativeCallValidation.h"

#if ENABLE(AOT)

#include "AOTBuiltinTableBindings.h"
#include "AOTCompiledCallee.h"
#include "AOTInstance.h"
#include "AOTNameClass.h"
#include "AOTNativeCallDeclaration.h"
#include "AOTObject.h"
#include "AOTOperandSnapshot.h"
#include "AOTRuntimeReporter.h"
#include "AOTState.h"
#include "AOTVector.h"
#include "JSFunctionWithFields.h"
#include "DeferGCInlines.h"
#include "ErrorInstance.h"
#include "GlobalObjectMethodTable.h"
#include "InternalFunction.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"
#include "MarkedSpaceInlines.h"
#include "NullGetterFunction.h"
#include "NullSetterFunction.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <mutex>
#include <wtf/DataLog.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

static constexpr unsigned maxCheckedArguments = 16;

static bool mayRunCodeThroughOperand(const BuiltinParameter& parameter, JSValue operand)
{
    if (operand.isUndefinedOrNull())
        return false;
    if (!parameter.protocols.isEmpty())
        return true;
    return operand.isObject() && parameter.conversion != BuiltinConversion::None;
}

#if !defined(BUN_HARDENED)
namespace {

class CheckedNativeCalls {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(CheckedNativeCalls);
    WTF_MAKE_NONCOPYABLE(CheckedNativeCalls);
public:
    static CheckedNativeCalls* ifAskedFor()
    {
        static CheckedNativeCalls* calls;
        static std::once_flag once;
        std::call_once(once, [] {
            const char* path = getenv("BUN_AOT_CHECKED_NATIVE_CALLS");
            if (!path || !*path)
                return;
            calls = new CheckedNativeCalls(path);
            RuntimeReporter::callAtExit(writeCheckedNativeCalls);
        });
        return calls;
    }

    void add(const String& entry, bool didThrow, bool didChangeOperand, bool hadExpectation)
    {
        Locker locker { m_lock };
        auto found = m_counts.find(entry);
        if (found == m_counts.end())
            found = m_counts.add(entry.isolatedCopy(), Counts { }).iterator;
        Counts& counts = found->value;
        ++(didThrow ? counts.threw : counts.returned);
        counts.changedOperand += didChangeOperand;
        counts.hadExpectation += hadExpectation;
    }

    void write()
    {
        Locker locker { m_lock };
        Vector<String> entries = copyToVector(m_counts.keys());
        std::ranges::sort(entries, codePointCompareLessThan);
        StringPrintStream out;
        for (const String& entry : entries) {
            const Counts& counts = m_counts.find(entry)->value;
            out.print(entry, "\t", counts.returned, "\t", counts.threw, "\t", counts.changedOperand, "\t", counts.hadExpectation, "\n");
        }
        m_counts.clear();
        auto text = out.toString().utf8();
        FILE* file = text.length() ? fopen(m_path.span().data(), "ab") : nullptr;
        if (!file)
            return;
        // One write, so that the lines of two processes do not mix.
        setvbuf(file, nullptr, _IOFBF, text.length() + 1);
        fwrite(text.data(), 1, text.length(), file);
        fclose(file);
    }

private:
    struct Counts {
        uint64_t returned { 0 };
        uint64_t threw { 0 };
        uint64_t changedOperand { 0 };
        uint64_t hadExpectation { 0 };
    };

    explicit CheckedNativeCalls(const char* path)
        : m_path(unsafeMakeSpan(path, strlen(path) + 1))
    {
    }

    Vector<char> m_path;
    Lock m_lock;
    HashMap<String, Counts> m_counts WTF_GUARDED_BY_LOCK(m_lock);
};

} // anonymous namespace
#endif

void writeCheckedNativeCalls()
{
#if !defined(BUN_HARDENED)
    if (CheckedNativeCalls* calls = CheckedNativeCalls::ifAskedFor())
        calls->write();
#endif
}

class NativeCallValidationScope::Checker {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(Checker);
    WTF_MAKE_NONCOPYABLE(Checker);
public:
    Checker(VM&, const NativeCall&, const NativeCallExpectation&);
    bool finish(JSValue result);

private:
    struct Operand {
        BuiltinOperand position;
        JSObject* object;
        OperandSnapshot before;
    };

    struct CollectionVersions {
        friend bool operator==(const CollectionVersions&, const CollectionVersions&) = default;

        HeapVersion marking { 0 };
        HeapVersion eden { 0 };
        HeapVersion newlyAllocated { 0 };
    };

    CollectionVersions collectionVersions() const;
    JSValue operandAt(BuiltinOperand) const;
    void addOperand(BuiltinOperand, JSValue);

    OperandUses usesOf(BuiltinOperand) const;
    OperandUses usesOfBytesOf(const Operand&) const;
    bool holdsUnderNamedMember(const BuiltinParameter&, JSObject* holder, JSObject* held) const;
    bool isAt(BuiltinOperand, JSObject*) const;
    bool isTakenForUnknownFunction() const;
    bool isHelperOfLibraryFunctions() const;
    bool mayReturnHeldValue() const;
    bool mayReturnWhateverNativeCodeHolds() const;
    bool covers(const BuiltinKey&, UniquedStringImpl*) const;
    bool hasActionOn(JSObject*, OperandChange, UniquedStringImpl* key) const;
    AbstractHeaps declaredWrites() const;
    bool mayRunCode() const;
    bool mayThrow() const;
    bool usesProxyByProtocol() const;
    bool mayCollect() const;

    void checkOperandsOfUnknownFunction();
    void checkChanges(bool didRunCode);
    void checkArrivedValue(const Operand& holder, const OperandDifference&);
    bool isAmongRetainedContents(JSObject* arrived, UniquedStringImpl* key) const;
    void checkResult();
    void checkException(bool didRunCode);
    void fail(ValidationFailureKind, JSValue found, const String& message);
    void failForUnknownFunction(JSValue found, OperandUses declaring, const String& message);
    void noteChangeWithoutDeclaration(const String& message);
    String describeCallee() const;

    VM& m_vm;
    NativeCall m_call;
    NativeCallExpectation m_expectation;
    NativeCallDeclaration m_declaration;
    Vector<Operand, 4> m_operands;
    CollectionVersions m_collectionVersions;
    JSValue m_result;
    bool m_didEnterVMBefore { false };
    bool m_didEnterCompiledCodeBefore { false };
    // The call in the course of which this one is made.
    Checker* m_outer { nullptr };
    bool m_didCallFunctionThatWrites { false };
    bool m_didChangeOperand { false };
    static thread_local Checker* s_innermost;
};

thread_local NativeCallValidationScope::Checker* NativeCallValidationScope::Checker::s_innermost;

NativeCallValidationScope::Checker::Checker(VM& vm, const NativeCall& call, const NativeCallExpectation& expectation)
    : m_vm(vm)
    , m_call(call)
    , m_expectation(expectation)
{
    DeferGCForAWhile deferGC(vm);
    m_declaration = findDeclaration(call, m_expectation);

    if (call.kind == CodeSpecializationKind::CodeForCall)
        addOperand(BuiltinOperand::receiver(), call.thisValue);
    for (unsigned index = 0; index < std::min<unsigned>(call.arguments.size(), maxCheckedArguments); ++index)
        addOperand(BuiltinOperand::argument(index), call.arguments.at(index));
    checkOperandsOfUnknownFunction();

    m_collectionVersions = collectionVersions();
    m_didEnterVMBefore = std::exchange(vm.didEnterVM, false);
    if (Instance* instance = Instance::of(call.realm))
        m_didEnterCompiledCodeBefore = std::exchange(instance->scratch.didEnterCompiledCode, 0);
    m_outer = std::exchange(s_innermost, this);
}

NativeCallExpectation NativeCallExpectation::take(JSGlobalObject* realm, JSObject* callee)
{
    Instance* instance = Instance::of(realm);
    if (!instance)
        return unknown();
    ExpectedCallWords& words = instance->scratch.expectedCall;
    if (words.callee != callee)
        return unknown();
    words.callee = nullptr;
    if (words.site == overwrittenSite)
        return unknown();
    return decode(words);
}

NativeCallValidationScope::Checker::CollectionVersions NativeCallValidationScope::Checker::collectionVersions() const
{
    MarkedSpace& space = m_vm.heap.objectSpace();
    return { space.markingVersion(), space.edenVersion(), space.newlyAllocatedVersion() };
}

JSValue NativeCallValidationScope::Checker::operandAt(BuiltinOperand position) const
{
    return position.isReceiver() ? m_call.thisValue : m_call.arguments.at(position.argumentIndex());
}

void NativeCallValidationScope::Checker::addOperand(BuiltinOperand position, JSValue value)
{
    if (!canCapture(value))
        return;
    JSObject* object = asObject(value);
    bool isRepeated = m_operands.containsIf([&](const Operand& operand) {
        return operand.object == object;
    });
    // What is declared for either position may happen to the object.
    if (isRepeated)
        return;
    m_operands.append({ position, object, capture(object) });
}

// The analysis hands the operands of a callee it does not know to native code, whatever the callee turns out to be.
bool NativeCallValidationScope::Checker::isTakenForUnknownFunction() const
{
    switch (m_declaration.source) {
    case NativeCallDeclarationSource::Compiler:
        return false;
    case NativeCallDeclarationSource::NotInTable:
        return true;
    case NativeCallDeclarationSource::CalleeInRealm:
    case NativeCallDeclarationSource::NativeCodeOfCallee:
    case NativeCallDeclarationSource::NameOfModuleMember:
        return m_expectation.isKnown && !m_expectation.names(BuiltinTable::singleton().canonicalID(m_declaration.builtin));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isBuiltinIterator(JSObject* object)
{
    switch (object->type()) {
    case JSArrayIteratorType:
    case JSMapIteratorType:
    case JSSetIteratorType:
    case JSStringIteratorType:
    case JSRegExpStringIteratorType:
    case JSIteratorHelperType:
    case JSWrapForValidIteratorType:
    case JSGeneratorType:
        return true;
    default:
        return false;
    }
}

// { transfer: [buffer] }: the member itself, or an element of the array that it holds.
bool NativeCallValidationScope::Checker::holdsUnderNamedMember(const BuiltinParameter& parameter, JSObject* holder, JSObject* held) const
{
    const BuiltinTable& table = BuiltinTable::singleton();
    DeferGCForAWhile deferGC(m_vm);
    for (BuiltinName name : parameter.namedMembers) {
        Identifier key = table.identifierOf(m_vm, name);
        PropertySlot slot(holder, PropertySlot::InternalMethodType::VMInquiry, &m_vm);
        if (!holder->methodTable()->getOwnPropertySlot(holder, m_call.realm, key, slot) || !slot.isValue())
            continue;
        JSValue member = slot.getValue(m_call.realm, key);
        if (member == JSValue(held))
            return true;
        if (!parameter.protocols.contains(Protocol::Iterate) || !isJSArray(member))
            continue;
        JSArray* array = asArray(member);
        for (unsigned index = 0; index < array->length(); ++index) {
            if (array->canGetIndexQuickly(index) && array->getIndexQuickly(index) == JSValue(held))
                return true;
        }
    }
    return false;
}

OperandUses NativeCallValidationScope::Checker::usesOf(BuiltinOperand position) const
{
    const BuiltinTable& table = BuiltinTable::singleton();
    JSObject* object = asObject(operandAt(position));
    OperandUses uses;
    auto addUsesAt = [&](const BuiltinOverload& overload, BuiltinOperand other) {
        if (overload.isUndeclared())
            uses = OperandUses::all();
        else if (const BuiltinParameter* parameter = table.parameterFor(overload, other)) {
            uses.add(parameter->uses);
            // A function that iterates steps a built-in iterator without a call of `next`, whose entry has the write.
            if (parameter->protocols.contains(Protocol::Iterate) && isBuiltinIterator(object))
                uses.add(OperandUse::WritesContents);
        }
    };
    for (const BuiltinOverload* overload : m_declaration.overloads) {
        if (m_call.kind == CodeSpecializationKind::CodeForCall && m_call.thisValue == JSValue(object))
            addUsesAt(*overload, BuiltinOperand::receiver());
        for (unsigned index = 0; index < std::min<unsigned>(m_call.arguments.size(), BuiltinOperand::maxArgumentIndex); ++index) {
            JSValue argument = m_call.arguments.at(index);
            if (argument == JSValue(object)) {
                addUsesAt(*overload, BuiltinOperand::argument(index));
                continue;
            }
            // What is declared for an operand and for what it reaches may happen to another operand that it holds.
            const BuiltinParameter* parameter = overload->isUndeclared() || !argument.isObject() ? nullptr : table.parameterFor(*overload, BuiltinOperand::argument(index));
            if (parameter && parameter->uses.contains(OperandUse::AppliesTransitively) && holdsUnderNamedMember(*parameter, asObject(argument), object))
                uses.add(parameter->uses);
        }
    }
    return uses;
}

bool NativeCallValidationScope::Checker::isAt(BuiltinOperand position, JSObject* object) const
{
    if (position.isReceiver())
        return m_call.kind == CodeSpecializationKind::CodeForCall && m_call.thisValue == JSValue(object);
    for (unsigned index = position.argumentIndex(); index < m_call.arguments.size(); ++index) {
        if (position.coversArgument(index) && m_call.arguments.at(index) == JSValue(object))
            return true;
    }
    return false;
}

bool NativeCallValidationScope::Checker::covers(const BuiltinKey& key, UniquedStringImpl* name) const
{
    const BuiltinTable& table = BuiltinTable::singleton();
    switch (key.kind) {
    case BuiltinKeyKind::None:
    case BuiltinKeyKind::Any:
    case BuiltinKeyKind::ElementsOf:
        return true;
    case BuiltinKeyKind::Indexes: {
        // As flow has it (Program::isIndexLike()): an index, or another canonical numeric string: an array-like whose length is past the limit of arrays takes its elements under such names.
        if (name->isSymbol())
            return false;
        CString text = String(name).utf8();
        return Typed::mayBeElementKey(Typed::classifyName(byteCast<uint8_t>(text.span())));
    }
    case BuiltinKeyKind::Names:
        return std::ranges::any_of(key.names, [&](BuiltinName listed) {
            return table.identifierOf(m_vm, listed).impl() == name;
        });
    case BuiltinKeyKind::Operand: {
        JSValue operand = key.operand ? operandAt(*key.operand) : JSValue();
        if (!operand || operand.isObject())
            return true;
        if (operand.isSymbol())
            return &asSymbol(operand)->uid() == name;
        return !name->isSymbol() && operand.toWTFString(m_call.realm) == StringView(name);
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool declares(BuiltinPropertyActionKind action, OperandChange change)
{
    switch (action) {
    case BuiltinPropertyActionKind::StoresByKey:
    case BuiltinPropertyActionKind::AssignsFrom:
        return change == OperandChange::PropertyAdded || change == OperandChange::PropertyStored;
    case BuiltinPropertyActionKind::DefinesName:
    case BuiltinPropertyActionKind::TakesDescriptor:
    case BuiltinPropertyActionKind::TakesDescriptorMap:
    case BuiltinPropertyActionKind::DefinesFrom:
    case BuiltinPropertyActionKind::DefinesGetter:
    case BuiltinPropertyActionKind::DefinesSetter:
        return change == OperandChange::PropertyAdded || change == OperandChange::PropertyStored || change == OperandChange::AttributesChanged;
    case BuiltinPropertyActionKind::DeletesByKey:
        return change == OperandChange::PropertyRemoved;
    case BuiltinPropertyActionKind::ChangesIntegrity:
        return change == OperandChange::AttributesChanged;
    case BuiltinPropertyActionKind::ReadsByKey:
    case BuiltinPropertyActionKind::ChangesPrototype:
    case BuiltinPropertyActionKind::BecomesPrototype:
    case BuiltinPropertyActionKind::ReadsGetter:
    case BuiltinPropertyActionKind::ReadsSetter:
    case BuiltinPropertyActionKind::TestsByKey:
    case BuiltinPropertyActionKind::DescribesOwnProperties:
    case BuiltinPropertyActionKind::DescribesResult:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool NativeCallValidationScope::Checker::hasActionOn(JSObject* object, OperandChange change, UniquedStringImpl* key) const
{
    return std::ranges::any_of(m_declaration.overloads, [&](const BuiltinOverload* overload) {
        return overload->isUndeclared() || std::ranges::any_of(overload->flow.propertyActions, [&](const BuiltinPropertyAction& action) {
            return declares(action.kind, change) && isAt(action.object, object) && covers(action.key, key);
        });
    });
}

AbstractHeaps NativeCallValidationScope::Checker::declaredWrites() const
{
    AbstractHeaps writes;
    for (const BuiltinOverload* overload : m_declaration.overloads) {
        bool mayRunAnyCode = overload->effects.capability == BuiltinCapability::JS && !overload->flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode);
        if (overload->isUndeclared() || mayRunAnyCode)
            return AbstractHeaps::fromRaw((1ull << numberOfEnumerators<AbstractHeap>) - 1);
        writes.add(overload->effects.writes);
    }
    return writes;
}

bool NativeCallValidationScope::Checker::mayRunCode() const
{
    const BuiltinTable& table = BuiltinTable::singleton();
    if (m_declaration.overloads.isEmpty())
        return true;
    // A constructor reads `prototype` from another new.target, which may be a proxy or have an accessor there. The frame has it in the place of `this`.
    if (m_call.kind == CodeSpecializationKind::CodeForConstruct && m_call.thisValue.isObject() && m_call.thisValue != JSValue(m_call.callee))
        return true;

    const Builtin& builtin = table.at(m_declaration.builtin);
    if (builtin.flags.containsAny({ BuiltinFlag::LoadsModuleByName, BuiltinFlag::EvaluatesText, BuiltinFlag::RunsUnseenCode }))
        return true;
    if (!table.calledFunctionsOf(m_declaration.builtin).empty() || !table.moduleLoadsOf(m_declaration.builtin).empty() || table.unseenCodeOf(m_declaration.builtin))
        return true;

    for (const BuiltinOverload* overload : m_declaration.overloads) {
        if (overload->isUndeclared())
            return true;
        // The capability is that of the function behind the conversions of its operands.
        bool runsOnlyConversions = overload->effects.capability != BuiltinCapability::JS;
        auto mayRunCodeThrough = [&](const BuiltinParameter& parameter, JSValue operand) {
            return (operand.isObject() || !runsOnlyConversions) && mayRunCodeThroughOperand(parameter, operand);
        };
        if (overload->receiver && mayRunCodeThrough(*overload->receiver, m_call.thisValue))
            return true;
        for (unsigned index = 0; index < std::min<unsigned>(m_call.arguments.size(), BuiltinOperand::maxArgumentIndex); ++index) {
            const BuiltinParameter* parameter = table.parameterFor(*overload, BuiltinOperand::argument(index));
            if (parameter && mayRunCodeThrough(*parameter, m_call.arguments.at(index)))
                return true;
        }
        if (runsOnlyConversions)
            continue;
        if (!overload->flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode))
            return true;
        bool callsBeforeReturn = std::ranges::any_of(overload->flow.callbacks, [](const BuiltinCallback& callback) {
            return !isCalledAfterReturn(callback.times);
        }) || std::ranges::any_of(overload->flow.handlerCalls, [](const BuiltinHandlerCall& handler) {
            return !isCalledAfterReturn(handler.times);
        });
        if (callsBeforeReturn)
            return true;
    }
    return false;
}

// A conversion throws for an operand outside of what the parameter accepts. Only the overload that the compiler matched is known to fit the operands.
bool NativeCallValidationScope::Checker::mayThrow() const
{
    if (m_declaration.source != NativeCallDeclarationSource::Compiler || m_expectation.matchesAnyOverload)
        return true;
    return std::ranges::any_of(m_declaration.overloads, [](const BuiltinOverload* overload) {
        bool mayRunAnyCode = overload->effects.capability == BuiltinCapability::JS && !overload->flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode);
        return overload->throws() || mayRunAnyCode;
    });
}

bool NativeCallValidationScope::Checker::usesProxyByProtocol() const
{
    const BuiltinTable& table = BuiltinTable::singleton();
    auto isProxyUsedBy = [](const BuiltinParameter& parameter, JSValue operand) {
        return !parameter.protocols.isEmpty() && operand.isObject() && asObject(operand)->type() == ProxyObjectType;
    };
    return std::ranges::any_of(m_declaration.overloads, [&](const BuiltinOverload* overload) {
        if (overload->receiver && isProxyUsedBy(*overload->receiver, m_call.thisValue))
            return true;
        for (unsigned index = 0; index < std::min<unsigned>(m_call.arguments.size(), BuiltinOperand::maxArgumentIndex); ++index) {
            const BuiltinParameter* parameter = table.parameterFor(*overload, BuiltinOperand::argument(index));
            if (parameter && isProxyUsedBy(*parameter, m_call.arguments.at(index)))
                return true;
        }
        return false;
    });
}

bool NativeCallValidationScope::Checker::mayCollect() const
{
    const BuiltinTable& table = BuiltinTable::singleton();
    // The capability is that of the function behind the conversions of its operands.
    auto mayMakeFlat = [](const BuiltinParameter& parameter, JSValue operand) {
        return operand.isString() && readsCharactersOfString(parameter.conversion);
    };
    return m_declaration.overloads.isEmpty() || std::ranges::any_of(m_declaration.overloads, [&](const BuiltinOverload* overload) {
        if (overload->isUndeclared() || overload->effects.capability != BuiltinCapability::None)
            return true;
        if (overload->receiver && mayMakeFlat(*overload->receiver, m_call.thisValue))
            return true;
        for (unsigned index = 0; index < std::min<unsigned>(m_call.arguments.size(), BuiltinOperand::maxArgumentIndex); ++index) {
            const BuiltinParameter* parameter = table.parameterFor(*overload, BuiltinOperand::argument(index));
            if (parameter && mayMakeFlat(*parameter, m_call.arguments.at(index)))
                return true;
        }
        return false;
    });
}

String NativeCallValidationScope::Checker::describeCallee() const
{
    const BuiltinTable& table = BuiltinTable::singleton();
    switch (m_declaration.source) {
    case NativeCallDeclarationSource::Compiler:
        if (m_expectation.matchesAnyOverload)
            return makeString(table.tokenOf(m_declaration.builtin), "/any"_s);
        return table.tokenOf(m_expectation.overload);
    case NativeCallDeclarationSource::CalleeInRealm:
    case NativeCallDeclarationSource::NativeCodeOfCallee:
        return makeString(table.tokenOf(m_declaration.builtin), isTakenForUnknownFunction() ? "/not-named-by-the-call"_s : "/any"_s);
    case NativeCallDeclarationSource::NameOfModuleMember:
        return makeString(table.tokenOf(m_declaration.builtin), "/by-name"_s);
    case NativeCallDeclarationSource::NotInTable:
        return makeString("function-not-in-table '"_s, nameOfNativeFunction(m_call.callee), "' ("_s, m_call.callee->classInfo()->className, ')');
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static String describe(BuiltinOperand position)
{
    return position.isReceiver() ? "this"_str : makeString("argument "_s, position.argumentIndex());
}

static String describe(OperandUses uses)
{
    StringPrintStream out;
    CommaPrinter comma("|"_s);
    for (OperandUse use : uses)
        out.print(comma, nameOf(use));
    return out.toString();
}

void NativeCallValidationScope::Checker::fail(ValidationFailureKind kind, JSValue found, const String& message)
{
    // A log that was asked for gets every mismatch of a run. One entry of the table may be wrong for a function that every program calls.
    RuntimeReporter* reporter = m_vm.aotState().reporter.get();
    if (reporter && reporter->isExplicitlyEnabled()) {
        reporter->reportNativeCall(kind, describeCallee(), message);
        return;
    }
    dataLogLn("AOT native call: ", describeCallee(), " ", message);
    crashOnValidationFailure(m_vm, { kind, m_expectation.function, m_expectation.value, found });
}

// The analysis opens the operands of a function that the host's modules reach under a private name of the global object.
static bool isPrivateGlobal(JSGlobalObject* realm, JSObject* callee)
{
    auto isPrivateName = [](UniquedStringImpl* key) {
        return key->isSymbol() && static_cast<SymbolImpl*>(key)->isPrivate();
    };
    SymbolTable* variables = realm->symbolTable();
    ConcurrentJSLocker locker(variables->m_lock);
    for (auto entry = variables->begin(locker); entry != variables->end(locker); ++entry) {
        if (isPrivateName(entry->key.get()) && realm->variableAt(entry->value.scopeOffset()).get() == JSValue(callee))
            return true;
    }
    bool isFound = false;
    realm->structure()->forEachPropertyConcurrently([&](const PropertyTableEntry& entry) {
        isFound = isPrivateName(entry.key()) && realm->getDirect(entry.offset()) == JSValue(callee);
        return !isFound;
    });
    return isFound;
}

// Where unseen code may run, an object that native code holds may change in any way, and no proof rests on the rule.
static bool isAnsweredBy(JSGlobalObject* realm, BuiltinID binding, JSObject* callee)
{
    auto* hook = realm->globalObjectMethodTable()->nativeBinding;
    auto* function = dynamicDowncast<JSFunction>(callee);
    std::optional<uint16_t> hostNumber = BuiltinBindings::hostNumberOf(binding);
    if (!hook || !function || !function->isHostFunction() || !hostNumber)
        return false;

    VM& vm = realm->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue answer = hook(realm, *hostNumber);
    if (scope.exception()) [[unlikely]] {
        scope.clearException();
        return false;
    }
    auto hasSameCode = [&](JSValue value) {
        auto* other = dynamicDowncast<JSFunction>(value);
        return other && other->isHostFunction() && other->nativeFunction() == function->nativeFunction();
    };
    if (hasSameCode(answer))
        return true;
    if (!answer.isObject())
        return false;
    JSObject* holder = asObject(answer);
    bool isFound = false;
    // A binding may answer its functions in an array.
    if (auto* array = dynamicDowncast<JSArray>(holder)) {
        for (unsigned index = 0; index < array->length() && !isFound; ++index)
            isFound = array->canGetIndexQuickly(index) && hasSameCode(array->getIndexQuickly(index));
        if (isFound)
            return true;
    }
    holder->structure()->forEachPropertyConcurrently([&](const PropertyTableEntry& entry) {
        isFound = hasSameCode(holder->getDirect(entry.offset()));
        return !isFound;
    });
    return isFound;
}

// Nothing leads from the realm to what a native binding answers, and the compiler does not name the binding at the call.
static bool isDeclaredByNativeBinding(JSGlobalObject* realm, JSObject* callee, OperandUses declaring)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    for (unsigned index = 0; index < table.builtinCount(); ++index) {
        const Builtin& builtin = table.at(BuiltinID(index));
        if (builtin.kind != BuiltinKind::NativeBinding)
            continue;
        bool declares = std::ranges::any_of(table.overloadsOf(BuiltinID(index)), [&](const BuiltinOverload& overload) {
            return !overload.isUndeclared() && std::ranges::any_of(overload.parameters, [&](const BuiltinParameter& parameter) {
                return parameter.uses.containsAny(declaring);
            });
        });
        if (declares && isAnsweredBy(realm, BuiltinID(index), callee))
            return true;
    }
    return false;
}

void NativeCallValidationScope::Checker::failForUnknownFunction(JSValue found, OperandUses declaring, const String& message)
{
    bool restsOnNativeCode = m_expectation.isKnown && m_expectation.trustsNativeCode && !isPrivateGlobal(m_call.realm, m_call.callee) && !isHelperOfLibraryFunctions();
    if (restsOnNativeCode && isDeclaredByNativeBinding(m_call.realm, m_call.callee, declaring))
        return;
    if (restsOnNativeCode) {
        fail(ValidationFailureKind::NativeCallChangedOperand, found, message);
        return;
    }
    RuntimeReporter* reporter = m_vm.aotState().reporter.get();
    if (reporter && reporter->isExplicitlyEnabled())
        reporter->reportNativeCall(ValidationFailureKind::NativeCallChangedOperand, describeCallee(), makeString(message, " (no proof of this program rests on it)"_s));
}

// Flow opens the operands it knows of an overload without a declaration, so no proof about this operand is wrong. What the unknown object stands for there keeps its proofs.
void NativeCallValidationScope::Checker::noteChangeWithoutDeclaration(const String& message)
{
    RuntimeReporter* reporter = m_vm.aotState().reporter.get();
    if (reporter && reporter->isExplicitlyEnabled())
        reporter->reportNativeCall(ValidationFailureKind::NativeCallChangedOperand, describeCallee(), makeString(message, ", and the overload has no declaration"_s));
}

// A library function calls these, and where flow goes by the entry or the model of that function it does not see the call.
bool NativeCallValidationScope::Checker::isHelperOfLibraryFunctions() const
{
    Instance* instance = Instance::of(m_call.realm);
    return instance && std::ranges::contains(instance->realm.linkTimeConstants, JSValue::encode(m_call.callee));
}

// Flow hands the operands of a function it does not know to native code.
void NativeCallValidationScope::Checker::checkOperandsOfUnknownFunction()
{
    if (m_declaration.source != NativeCallDeclarationSource::NotInTable || isHelperOfLibraryFunctions())
        return;
    // A bound function hands its operands on to its target, which flow calls in its place.
    if (m_call.callee->inherits<JSBoundFunction>())
        return;
    for (const Operand& operand : m_operands) {
        Object* compiled = compiledObjectOrNull(operand.object);
        if (compiled && compiled->layout().impossibleMutations().contains(ObjectMutation::ReachNativeCode))
            fail(ValidationFailureKind::ImpossibleMutation, compiled, makeString(describe(operand.position), ": layout "_s, compiled->layout().index, " never reaches native code"_s));
    }
}

// Two views of one buffer, and the buffer itself, hold the same bytes: what the call writes through one of them shows in the others.
OperandUses NativeCallValidationScope::Checker::usesOfBytesOf(const Operand& operand) const
{
    OperandUses uses = usesOf(operand.position);
    auto bytesOf = [](const Operand& each) {
        return std::span(static_cast<const uint8_t*>(each.before.bytes), each.before.byteLength);
    };
    std::span bytes = bytesOf(operand);
    for (const Operand& other : m_operands) {
        std::span otherBytes = bytesOf(other);
        if (other.object != operand.object && bytes.data() < otherBytes.data() + otherBytes.size() && otherBytes.data() < bytes.data() + bytes.size())
            uses.add(usesOf(other.position));
    }
    return uses;
}

static OperandChange changeAsCompiledCodeTakesIt(JSObject* object, const OperandDifference& difference)
{
    bool isOfProperty = difference.change == OperandChange::PropertyAdded || difference.change == OperandChange::PropertyStored || difference.change == OperandChange::PropertyRemoved;
    if (isOfProperty && difference.key && isJSArray(object) && isCanonicalNumericIndexString(difference.key.get()))
        return OperandChange::ElementsChanged;
    return difference.change;
}

void NativeCallValidationScope::Checker::checkChanges(bool didRunCode)
{
    AbstractHeaps declared = declaredWrites();
    bool hasNoDeclaration = !m_declaration.overloads.isEmpty() && std::ranges::all_of(m_declaration.overloads, &BuiltinOverload::isUndeclared);
    for (const Operand& operand : m_operands) {
        Vector<OperandDifference> differences = differencesBetween(m_vm, operand.object, operand.before, capture(operand.object));
        if (differences.isEmpty())
            continue;
        m_didChangeOperand = true;
        for (const OperandDifference& difference : differences) {
            OperandChange change = changeAsCompiledCodeTakesIt(operand.object, difference);
            OperandUses uses = difference.change == OperandChange::BytesChanged ? usesOfBytesOf(operand) : usesOf(operand.position);
            // A resolving function settles the promise that it was made for. Handed that very promise, it rejects it: no change of what it is handed as such.
            if (difference.change == OperandChange::InternalStateChanged && operand.object->type() == JSPromiseType && m_call.callee->inherits<JSFunctionWithFields>())
                continue;
            String what = makeString(describe(operand.position), ": "_s, nameOf(difference.change), difference.detail.isEmpty() ? ""_s : " "_s, difference.detail);
            AbstractHeaps heaps = heapsHolding(change);
            if (difference.change == OperandChange::InternalStateChanged)
                heaps = heapsOfClassOf(operand.object);

            if (m_expectation.isKnown && !heaps.isEmpty() && !m_expectation.writes.containsAny(heaps))
                fail(ValidationFailureKind::NativeCallWroteHeap, operand.object, makeString(what, ", the call writes "_s, describe(m_expectation.writes), ", not "_s, describe(heaps)));
            if (didRunCode)
                continue;
            OperandUses declaring = usesDeclaring(change);
            if (!declaring.isEmpty() && isTakenForUnknownFunction()) {
                failForUnknownFunction(operand.object, declaring, makeString(what, ", and native code changes nothing it is handed"_s));
                continue;
            }
            if (!declaring.isEmpty() && hasNoDeclaration) {
                noteChangeWithoutDeclaration(what);
                continue;
            }
            checkArrivedValue(operand, difference);
            if (!declaring.isEmpty() && !uses.containsAny(declaring))
                fail(ValidationFailureKind::NativeCallChangedOperand, operand.object, makeString(what, ", uses "_s, describe(uses), ", needs "_s, describe(declaring)));
            else if (change == difference.change && difference.key && uses != OperandUses::all() && !hasActionOn(operand.object, difference.change, difference.key.get()))
                fail(ValidationFailureKind::NativeCallChangedOperand, operand.object, makeString(what, ", no property action covers the key"_s));
            else if (!m_declaration.overloads.isEmpty() && !heaps.isEmpty() && !declared.containsAny(heaps))
                fail(ValidationFailureKind::NativeCallWroteHeap, operand.object, makeString(what, ", the entry writes "_s, describe(declared), ", not "_s, describe(heaps)));
        }
    }
}

// What an operand holds may be another operand, or the operand itself, and a copy of its properties hands that on under its key.
bool NativeCallValidationScope::Checker::isAmongRetainedContents(JSObject* arrived, UniquedStringImpl* key) const
{
    if (!key)
        return false;
    DeferGCForAWhile deferGC(m_vm);
    return std::ranges::any_of(m_operands, [&](const Operand& source) {
        if (!usesOf(source.position).contains(OperandUse::RetainsContents))
            return false;
        PropertySlot slot(source.object, PropertySlot::InternalMethodType::VMInquiry, &m_vm);
        if (!source.object->methodTable()->getOwnPropertySlot(source.object, m_call.realm, PropertyName(key), slot) || !slot.isValue())
            return false;
        return slot.getValue(m_call.realm, PropertyName(key)) == JSValue(arrived);
    });
}

// An operand that another operand holds after the call is retained.
void NativeCallValidationScope::Checker::checkArrivedValue(const Operand& holder, const OperandDifference& difference)
{
    // The bits may be those of an unboxed number, so they are only compared.
    for (const Operand& operand : m_operands) {
        if (JSValue::encode(operand.object) != difference.arrived || operand.object == holder.object)
            continue;
        OperandUses uses = usesOf(operand.position);
        if (!uses.contains(OperandUse::Retains) && !isAmongRetainedContents(operand.object, difference.key.get()))
            fail(ValidationFailureKind::NativeCallKeptOperand, operand.object, makeString(describe(operand.position), ": "_s, describe(holder.position), " holds it, uses "_s, describe(uses)));
    }
}

// What an operand holds may be another operand, or the operand itself.
bool NativeCallValidationScope::Checker::mayReturnHeldValue() const
{
    return std::ranges::any_of(m_declaration.overloads, [](const BuiltinOverload* overload) {
        auto reachesResult = [](const BuiltinPlace& place) {
            return place.kind == BuiltinPlaceKind::Result;
        };
        return overload->isUndeclared()
            || std::ranges::any_of(overload->flow.transfers, [&](const BuiltinTransfer& transfer) {
                return reachesResult(transfer.to) && transfer.from.kind != BuiltinPlaceKind::Operand;
            })
            || std::ranges::any_of(overload->flow.propertyActions, [&](const BuiltinPropertyAction& action) {
                return reachesResult(action.value);
            })
            || std::ranges::any_of(overload->flow.callbacks, [](const BuiltinCallback& callback) {
                return callback.resultUse == BuiltinCallbackResultUse::Returned;
            });
    });
}

// A result declared as anything stands for every object that native code holds, and it holds an operand that it retains.
bool NativeCallValidationScope::Checker::mayReturnWhateverNativeCodeHolds() const
{
    const BuiltinTable& table = BuiltinTable::singleton();
    return std::ranges::all_of(m_declaration.overloads, [&](const BuiltinOverload* overload) {
        return table.at(overload->result.type).isAnything();
    });
}

void NativeCallValidationScope::Checker::checkResult()
{
    if (!m_result || !m_result.isObject() || isTakenForUnknownFunction() || mayReturnHeldValue())
        return;
    bool mayReturnRetainedOperand = mayReturnWhateverNativeCodeHolds();
    for (const Operand& operand : m_operands) {
        if (operand.object != asObject(m_result))
            continue;
        OperandUses uses = usesOf(operand.position);
        if (mayReturnRetainedOperand && uses.contains(OperandUse::Retains))
            continue;
        if (!uses.contains(OperandUse::Returns))
            fail(ValidationFailureKind::NativeCallKeptOperand, operand.object, makeString(describe(operand.position), ": it is the result, uses "_s, describe(uses)));
    }
}

void NativeCallValidationScope::Checker::checkException(bool didRunCode)
{
    Exception* exception = m_vm.exceptionForInspection();
    if (!exception || m_vm.isTerminationException(exception))
        return;
    if (auto* error = dynamicDowncast<ErrorInstance>(exception->value()); error && (error->isStackOverflowError() || error->isOutOfMemoryError()))
        return;
    if (m_expectation.isKnown && !m_expectation.mayThrow)
        fail(ValidationFailureKind::NativeCallThrew, exception->value(), "the call does not throw"_s);
    else if (!didRunCode && !mayThrow() && !usesProxyByProtocol())
        fail(ValidationFailureKind::NativeCallThrew, exception->value(), "the entry does not throw"_s);
}

bool NativeCallValidationScope::Checker::finish(JSValue result)
{
    m_result = result;
    s_innermost = m_outer;
    bool didRunCode = m_vm.didEnterVM;
    // What throws allocates the error that it throws: compiled code expects a collection on the way to a handler, whatever the call is said to do.
    bool didCollect = collectionVersions() != m_collectionVersions && !m_vm.exceptionForInspection();
    m_vm.didEnterVM |= m_didEnterVMBefore;
    bool didEnterCompiledCode = false;
    if (Instance* instance = Instance::of(m_call.realm)) {
        didEnterCompiledCode = instance->scratch.didEnterCompiledCode;
        instance->scratch.didEnterCompiledCode |= m_didEnterCompiledCodeBefore;
    }

    DeferGCForAWhile deferGC(m_vm);
    if (didEnterCompiledCode && m_expectation.isKnown && !m_expectation.mayEnterCompiledCode)
        fail(ValidationFailureKind::NativeCallRanCode, m_call.callee, "the call enters no compiled code"_s);
    if (didRunCode && !mayRunCode())
        fail(ValidationFailureKind::NativeCallRanCode, m_call.callee, "the entry runs no code with these operands"_s);
    if (didCollect && m_expectation.isKnown && !m_expectation.mayCollect)
        fail(ValidationFailureKind::NativeCallCollected, m_call.callee, "the call does not collect"_s);
    // The conversion of an object operand allocates whether or not it enters JavaScript, and the capability of the entry says nothing of it.
    else if (didCollect && !didRunCode && !mayCollect() && !mayRunCode())
        fail(ValidationFailureKind::NativeCallCollected, m_call.callee, "the entry does not allocate"_s);
    // An entry that may run code through an operand may as well get to a function of the table that way: Reflect.set(o, "__proto__", p) calls the native setter. What that writes is its own doing.
    bool othersMayHaveWritten = didRunCode || (m_didCallFunctionThatWrites && mayRunCode());
    checkException(othersMayHaveWritten);
    checkChanges(othersMayHaveWritten);
    if (!othersMayHaveWritten)
        checkResult();
    bool countsAsCode = didRunCode || m_declaration.overloads.isEmpty();
    if (m_outer && !countsAsCode && (m_didCallFunctionThatWrites || !declaredWrites().isEmpty()))
        m_outer->m_didCallFunctionThatWrites = true;
#if !defined(BUN_HARDENED)
    if (CheckedNativeCalls* calls = CheckedNativeCalls::ifAskedFor(); calls && m_declaration.source != NativeCallDeclarationSource::NotInTable)
        calls->add(describeCallee(), m_vm.exceptionForInspection(), m_didChangeOperand, m_expectation.isKnown);
#endif
    return countsAsCode;
}

// The engine fills the missing half of an accessor pair with a function of its own. No program can name it, and a call of it is the read or the store that finds no accessor.
static bool standsForMissingAccessor(JSObject* callee)
{
    return is<NullGetterFunction>(callee) || is<NullSetterFunction>(callee);
}

// The function object of a compiled closure has native code that enters the compiled code, or that throws where a class is called without `new`. What it is handed goes to compiled code or nowhere.
static bool entersCompiledCode(JSObject* callee)
{
    return CompiledCallee::isCompiledCallee(callee);
}

NEVER_INLINE void NativeCallValidationScope::open(VM& vm, const NativeCall& call, const NativeCallExpectation& expectation)
{
    JSC_REMOVABLE_DEFINITION(ImageCompiler, &NativeCallValidationScope::open);
    if (standsForMissingAccessor(call.callee) || entersCompiledCode(call.callee))
        return;
    m_checker = new Checker(vm, call, expectation);
}

NEVER_INLINE bool NativeCallValidationScope::close()
{
    JSC_REMOVABLE_DEFINITION(ImageCompiler, &NativeCallValidationScope::close);
    bool didRunCode = m_checker->finish(m_result);
    delete std::exchange(m_checker, nullptr);
    return didRunCode;
}

static TaggedNativeFunction nativeFunctionOf(JSObject* callee, CodeSpecializationKind kind)
{
    if (auto* function = dynamicDowncast<JSFunction>(callee))
        return kind == CodeSpecializationKind::CodeForCall ? function->nativeFunction() : function->nativeConstructor();
    return uncheckedDowncast<InternalFunction>(callee)->nativeFunctionFor(kind);
}

static EncodedJSValue callUnderValidation(JSGlobalObject* realm, CallFrame* callFrame, CodeSpecializationKind kind)
{
    JSObject* callee = callFrame->jsCallee();
    NativeCallValidationScope validation(realm->vm(), { realm, callee, kind, callFrame->thisValue(), ArgList(callFrame) });
    EncodedJSValue result = nativeFunctionOf(callee, kind)(realm, callFrame);
    validation.didReturn(JSValue::decode(result));
    return result;
}

JSC_DEFINE_HOST_FUNCTION(callNativeFunctionUnderValidation, (JSGlobalObject* realm, CallFrame* callFrame))
{
    return callUnderValidation(realm, callFrame, CodeSpecializationKind::CodeForCall);
}

JSC_DEFINE_HOST_FUNCTION(constructNativeFunctionUnderValidation, (JSGlobalObject* realm, CallFrame* callFrame))
{
    return callUnderValidation(realm, callFrame, CodeSpecializationKind::CodeForConstruct);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
