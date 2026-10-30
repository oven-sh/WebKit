/*
 *  Copyright (C) 1999-2000 Harri Porten (porten@kde.org)
 *  Copyright (C) 2003-2024 Apple Inc. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "config.h"
#include "ErrorInstance.h"

#include "AOTErrorMessages.h"
#include "AOTStackTrace.h"
#include "CodeBlock.h"
#include "CustomGetterSetter.h"
#include "DisallowVMEntry.h"
#include "ErrorInstanceInlines.h"
#include "GetterSetter.h"
#include "InlineCallFrame.h"
#include "IntegrityInlines.h"
#include "Interpreter.h"
#include "JSCInlines.h"
#include "JSCustomGetterFunction.h"
#include "JSCustomSetterFunction.h"
#include "ParseInt.h"
#include "PropertyNameArray.h"
#include "StackFrame.h"
#include "VM.h"
#include <wtf/text/MakeString.h>

namespace JSC {

const ClassInfo ErrorInstance::s_info = { "Error"_s, &JSNonFinalObject::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(ErrorInstance) };

static JSC_DECLARE_CUSTOM_GETTER(errorInstanceMaterializingStackGetter);

JSC_DEFINE_CUSTOM_GETTER(errorInstanceMaterializingStackGetter, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto* errorInstance = uncheckedDowncast<ErrorInstance>(JSValue::decode(thisValue));
    errorInstance->materializeErrorInfoIfNeeded(vm);
    return JSValue::encode(errorInstance->getDirect(vm, vm.propertyNames->stack));
}

ErrorInstance::ErrorInstance(VM& vm, Structure* structure, ErrorType errorType)
    : Base(vm, structure)
    , m_errorType(errorType)
    , m_stackOverflowError(false)
    , m_outOfMemoryError(false)
    , m_errorInfoMaterialized(false)
    , m_hasErrorInfo(false)
    , m_stackPropertyProvidedByCapturedStackTrace(false)
    , m_nativeGetterTypeError(false)
    , m_parseError(false)
#if ENABLE(WEBASSEMBLY)
    , m_catchableFromWasm(true)
#endif // ENABLE(WEBASSEMBLY)
#if USE(BUN_JSC_ADDITIONS)
    , m_stackStringIsFramesOnly(false)
    , m_isComposingStack(false)
    , m_stackReplacedWhileComposing(false)
    , m_stackToComposeIsDetached(false)
#endif
{
}

ErrorInstance* ErrorInstance::create(JSGlobalObject* globalObject, String&& message, ErrorType errorType, LineColumn lineColumn, String&& sourceURL, String&& stackString, String&& cause)
{
    VM& vm = globalObject->vm();
    Structure* structure = globalObject->errorStructure(errorType);
    ErrorInstance* instance = new (NotNull, allocateCell<ErrorInstance>(vm)) ErrorInstance(vm, structure, errorType);
    instance->finishCreation(vm, WTF::move(message), lineColumn, WTF::move(sourceURL), WTF::move(stackString), WTF::move(cause));
    return instance;
}

ErrorInstance* ErrorInstance::create(JSGlobalObject* globalObject, Structure* structure, JSValue message, JSValue options, SourceAppender appender, RuntimeType type, ErrorType errorType, bool useCurrentFrame, JSCell* subclassCaller)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    String messageString = message.isUndefined() ? String() : message.toWTFString(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);

    JSValue cause;
    if (options.isObject()) {
        // Since `throw undefined;` is valid, we need to distinguish the case where `cause` is an explicit undefined.
        cause = asObject(options)->getIfPropertyExists(globalObject, vm.propertyNames->cause);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }

    return create(vm, structure, messageString, cause, appender, type, errorType, useCurrentFrame, subclassCaller);
}

String appendSourceToErrorMessage(CodeBlock* codeBlock, BytecodeIndex bytecodeIndex, const String& message, RuntimeType type, ErrorInstance::SourceAppender appender)
{
    if (!codeBlock->hasExpressionInfo() || message.isNull())
        return message;

#if USE(BUN_JSC_ADDITIONS)
    // A private builtin (one of JSC's own, whose source has no URL) has expression info only when assertions are on
    // (BytecodeGenerator::emitExpressionInfo), for the positions. Its source text stays out of the message in every
    // build, so that a message does not depend on the build and does not name the builtin's internals.
    if (auto* executable = dynamicDowncast<FunctionExecutable>(codeBlock->ownerExecutable()); executable && executable->isPrivateBuiltinFunction())
        return message;
#endif

    auto info = codeBlock->expressionInfoForBytecodeIndex(bytecodeIndex);
    int expressionStart = info.divot - info.startOffset;
    int expressionStop = info.divot + info.endOffset;

    StringView sourceString = codeBlock->source().provider()->source();
    if (!expressionStop || expressionStart > static_cast<int>(sourceString.length()))
        return message;

    if (expressionStart < expressionStop)
        return appender(message, codeBlock->source().provider()->getRange(expressionStart, expressionStop), type, ErrorInstance::SourceTextWhereErrorOccurred::FoundExactSource);

    // No range information, so give a few characters of context.
    int dataLength = sourceString.length();
    int start = expressionStart;
    int stop = expressionStart;
    // Get up to 20 characters of context to the left and right of the divot, clamping to the line.
    // Then strip whitespace.
    while (start > 0 && (expressionStart - start < 20) && sourceString[start - 1] != '\n')
        start--;
    while (start < (expressionStart - 1) && isStrWhiteSpace(sourceString[start]))
        start++;
    while (stop < dataLength && (stop - expressionStart < 20) && sourceString[stop] != '\n')
        stop++;
    while (stop > expressionStart && isStrWhiteSpace(sourceString[stop - 1]))
        stop--;
    return appender(message, codeBlock->source().provider()->getRange(start, stop), type, ErrorInstance::SourceTextWhereErrorOccurred::FoundApproximateSource);
}

void ErrorInstance::setStackFrames(VM& vm, WTF::Vector<StackFrame>&& stackFrames)
{
    // The hook is reading the frames these would replace, and its result becomes "stack".
    if (m_isComposingStack) [[unlikely]]
        return;

    std::unique_ptr<Vector<StackFrame>> stackTrace = makeUnique<Vector<StackFrame>>(WTF::move(stackFrames));

    Locker locker { cellLock() };
    m_stackTrace = WTF::move(stackTrace);
    m_returnAddresses = nullptr;
    // A collection may already have formatted the frames these replace.
    m_stackString = String();
    m_hasErrorInfo = false;
#if USE(BUN_JSC_ADDITIONS)
    m_stackStringIsFramesOnly = false;
    // The host puts its accessor for these frames.
    m_stackToComposeIsDetached = false;
#endif
    vm.writeBarrier(this);
}

template<typename Visitor>
void ErrorInstance::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    ErrorInstance* thisObject = uncheckedDowncast<ErrorInstance>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    if (!thisObject->m_returnAddresses)
        return;
    Locker locker { thisObject->cellLock() };
    if (auto* returnAddresses = thisObject->m_returnAddresses)
        visitor.markAuxiliary(returnAddresses);
}

DEFINE_VISIT_CHILDREN(ErrorInstance);

size_t ErrorInstance::estimatedSize(JSCell* cell, VM& vm)
{
    size_t size = sizeof(ErrorInstance);
    ErrorInstance* errorInstance = uncheckedDowncast<ErrorInstance>(cell);

    {
        Locker locker { errorInstance->cellLock() };
        if (errorInstance->m_stackTrace)
            size += errorInstance->m_stackTrace->sizeInBytes();
#if ENABLE(AOT)
        if (errorInstance->m_returnAddresses)
            size += errorInstance->m_returnAddresses->sizeInBytes();
#endif
        if (!errorInstance->m_stackString.isEmpty())
            size += errorInstance->m_stackString.impl()->costDuringGC();
    }

    return Base::estimatedSize(cell, vm) + size;
}

void ErrorInstance::materializeStackTrace()
{
#if ENABLE(AOT)
    auto stackTrace = makeUnique<Vector<StackFrame>>();
    unsigned onTop = m_returnAddresses->numberOfFramesOnTop;
    if (onTop)
        stackTrace->append(m_stackTrace->span().first(onTop));
    StackFrame::appendCompiledFrames(*m_returnAddresses, *stackTrace);
    if (m_stackTrace)
        stackTrace->append(m_stackTrace->span().subspan(onTop));
    Locker locker { cellLock() };
    m_stackTrace = WTF::move(stackTrace);
    m_returnAddresses = nullptr;
#endif
}

#if ENABLE(AOT)
NEVER_INLINE bool ErrorInstance::tryRetainReturnAddresses(VM& vm, bool useCurrentFrame, JSCell* subclassCaller, std::unique_ptr<Vector<StackFrame>>& otherFrames)
{
    if (!useCurrentFrame)
        return false;
    std::optional<unsigned> stackTraceLimit = realm()->stackTraceLimit();
    if (!stackTraceLimit)
        return false;
    auto* returnAddresses = AOT::tryCaptureStackTrace(vm, this, *stackTraceLimit, otherFrames, subclassCaller);
    if (!returnAddresses)
        return false;
    Locker locker { cellLock() };
    m_returnAddresses = returnAddresses;
    return true;
}
#endif

void ErrorInstance::captureStackTrace(VM& vm, JSGlobalObject* globalObject, size_t framesToSkip, bool append)
{
    if (m_returnAddresses) [[unlikely]] {
        if (append)
            materializeStackTrace();
        else {
            Locker locker { cellLock() };
            m_returnAddresses = nullptr;
        }
    }
    {
        Locker locker { cellLock() };

        size_t limit = globalObject->stackTraceLimit().value_or(0);
        std::unique_ptr<Vector<StackFrame>> stackTrace = makeUnique<Vector<StackFrame>>();
        vm.interpreter.getStackTrace(this, *stackTrace, framesToSkip, limit);

        if (!m_stackTrace || !append) {
            m_stackTrace = WTF::move(stackTrace);
            vm.writeBarrier(this);
            return;
        }

        if (m_stackTrace) {
            size_t remaining = limit - std::min(stackTrace->size(), limit);
            remaining = std::min(remaining, m_stackTrace->size());
            if (remaining > 0) {
                ASSERT(m_stackTrace->size() >= remaining);
                stackTrace->append(m_stackTrace->span().first(remaining));
            }
        }

        m_stackTrace = WTF::move(stackTrace);
    }
    vm.writeBarrier(this);
}

void ErrorInstance::finishCreation(VM& vm, const String& message, JSValue cause, SourceAppender appender, RuntimeType type, bool useCurrentFrame, JSCell* subclassCaller)
{
    Base::finishCreation(vm);
    ASSERT(inherits(info()));

    m_sourceAppender = appender;
    m_runtimeTypeForCause = type;

    std::unique_ptr<Vector<StackFrame>> stackTrace;
#if ENABLE(AOT)
    if (!vm.hasCompiledCode() || !tryRetainReturnAddresses(vm, useCurrentFrame, subclassCaller, stackTrace)) [[likely]]
#endif
        stackTrace = getStackTrace(vm, this, useCurrentFrame, nullptr, nullptr, subclassCaller);
    {
        Locker locker { cellLock() };
        m_stackTrace = WTF::move(stackTrace);
    }
    vm.writeBarrier(this);

    String messageWithSource = message;

    if (((m_stackTrace && !m_stackTrace->isEmpty()) || m_returnAddresses) && hasSourceAppender()) {
        auto [codeBlock, bytecodeIndex] = getBytecodeIndex(vm, vm.topCallFrame);
        if (codeBlock) {
            ErrorInstance::SourceAppender appender = sourceAppender();
            clearSourceAppender();
            RuntimeType type = runtimeTypeForCause();
            clearRuntimeTypeForCause();
            messageWithSource = appendSourceToErrorMessage(codeBlock, bytecodeIndex, message, type, appender);
        }
#if ENABLE(AOT)
        else if (String quoted = AOT::quotedSourceOfFailedExpression(vm); !quoted.isNull() && !message.isNull()) {
            ErrorInstance::SourceAppender appender = sourceAppender();
            clearSourceAppender();
            RuntimeType type = runtimeTypeForCause();
            clearRuntimeTypeForCause();
            messageWithSource = appender(message, quoted, type, SourceTextWhereErrorOccurred::FoundExactSource);
        }
#endif
    }

    if (!messageWithSource.isNull())
        putDirect(vm, vm.propertyNames->message, jsString(vm, WTF::move(messageWithSource)), static_cast<unsigned>(PropertyAttribute::DontEnum));

    if (!cause.isEmpty())
        putDirect(vm, vm.propertyNames->cause, cause, static_cast<unsigned>(PropertyAttribute::DontEnum));
}

void ErrorInstance::finishCreation(VM& vm, const String& message, JSValue cause, JSCell* owner, CallLinkInfo* callLinkInfo)
{
    Base::finishCreation(vm);
    ASSERT(inherits(info()));

    std::unique_ptr<Vector<StackFrame>> stackTrace = getStackTrace(vm, this, /* useCurrentFrame */ true, owner, callLinkInfo);
    {
        Locker locker { cellLock() };
        m_stackTrace = WTF::move(stackTrace);
    }
    vm.writeBarrier(this);
    if (!message.isNull())
        putDirect(vm, vm.propertyNames->message, jsString(vm, message), static_cast<unsigned>(PropertyAttribute::DontEnum));

    if (!cause.isEmpty())
        putDirect(vm, vm.propertyNames->cause, cause, static_cast<unsigned>(PropertyAttribute::DontEnum));
}

void ErrorInstance::finishCreation(VM& vm, String&& message, LineColumn lineColumn, String&& sourceURL, String&& stackString, String&& cause)
{
    Base::finishCreation(vm);
    ASSERT(inherits(info()));

    m_lineColumn = lineColumn;
    m_sourceURL = WTF::move(sourceURL);

    {
        Locker locker { cellLock() };
        m_stackString = WTF::move(stackString);
    }
    m_hasErrorInfo = !m_stackString.isNull();

    if (!message.isNull())
        putDirect(vm, vm.propertyNames->message, jsString(vm, WTF::move(message)), static_cast<unsigned>(PropertyAttribute::DontEnum));
    if (!cause.isNull())
        putDirect(vm, vm.propertyNames->cause, jsString(vm, WTF::move(cause)), static_cast<unsigned>(PropertyAttribute::DontEnum));
}

void ErrorInstance::finishCreationForEmbedderError(VM& vm)
{
    Base::finishCreation(vm);
    ASSERT(inherits(info()));

    std::unique_ptr<Vector<StackFrame>> stackTrace = getStackTrace(vm, this, /* useCurrentFrame */ true);
    {
        Locker locker { cellLock() };
        m_stackTrace = WTF::move(stackTrace);
    }
    vm.writeBarrier(this);

    // Deliberately add no own "message" / "cause" properties; the embedder exposes those itself.
}

void ErrorInstance::setErrorInfoForEmbedderError(LineColumn lineColumn, String&& sourceURL, String&& stackString)
{
    ASSERT(!m_errorInfoMaterialized);
    ASSERT(!m_capturedStackTrace);

    {
        Locker locker { cellLock() };
        m_stackTrace = nullptr;
        m_returnAddresses = nullptr;
    }
    m_lineColumn = lineColumn;
    m_sourceURL = WTF::move(sourceURL);
    m_stackString = WTF::move(stackString);
    m_hasErrorInfo = !m_stackString.isNull();
#if USE(BUN_JSC_ADDITIONS)
    m_stackStringIsFramesOnly = false;
#endif
}

// Based on ErrorPrototype's errorProtoFuncToString(), but is modified to
// have no observable side effects to the user (i.e. does not call proxies,
// and getters).
String ErrorInstance::sanitizedMessageString(JSGlobalObject* globalObject, Symbol** symbol)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Integrity::auditStructureID(structureID());

    JSValue messageValue;
    auto messagePropertName = vm.propertyNames->message;
    if (structure()->hasAnyKindOfGetterSetterProperties()) [[unlikely]] {
        // A custom value is a message that the engine makes when it is first read. An inquiry may not make it.
        PropertySlot deferredSlot(this, PropertySlot::InternalMethodType::GetOwnProperty);
        if (JSObject::getOwnPropertySlot(this, globalObject, messagePropertName, deferredSlot) && deferredSlot.isCustom())
            deferredSlot.getValue(globalObject, messagePropertName);
        RETURN_IF_EXCEPTION(scope, {});
    }
    PropertySlot messageSlot(this, PropertySlot::InternalMethodType::VMInquiry, &vm);
    if (JSObject::getOwnPropertySlot(this, globalObject, messagePropertName, messageSlot) && messageSlot.isValue())
        messageValue = messageSlot.getValue(globalObject, messagePropertName);
    RETURN_IF_EXCEPTION(scope, {});

    if (!messageValue || !messageValue.isPrimitive())
        return {};

    if (symbol && messageValue.isSymbol()) {
        *symbol = asSymbol(messageValue);
        return { };
    }
    RELEASE_AND_RETURN(scope, messageValue.toWTFString(globalObject));
}

String ErrorInstance::sanitizedNameString(JSGlobalObject* globalObject, Symbol** symbol)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Integrity::auditStructureID(structureID());

    JSValue nameValue;
    auto namePropertName = vm.propertyNames->name;
    PropertySlot nameSlot(this, PropertySlot::InternalMethodType::VMInquiry, &vm);

    JSValue currentObj = this;
    unsigned prototypeDepth = 0;

    // We only check the current object and its prototype (2 levels) because normal
    // Error objects may have a name property, and if not, its prototype should have
    // a name property for the type of error e.g. "SyntaxError".
    while (currentObj.isCell() && prototypeDepth++ < 2) {
        JSObject* obj = uncheckedDowncast<JSObject>(currentObj);
        bool isFound = obj == this ? JSObject::getOwnPropertySlot(obj, globalObject, namePropertName, nameSlot) : obj->ordinaryGetOwnPropertySlot(globalObject, namePropertName, nameSlot);
        if (isFound && nameSlot.isValue()) {
            nameValue = nameSlot.getValue(globalObject, namePropertName);
            break;
        }
        currentObj = obj->getPrototypeDirect();
    }
    RETURN_IF_EXCEPTION(scope, {});

    if (!nameValue || !nameValue.isPrimitive() || nameValue.isUndefined())
        return "Error"_s;
    if (symbol && nameValue.isSymbol()) {
        *symbol = asSymbol(nameValue);
        return { };
    }
    RELEASE_AND_RETURN(scope, nameValue.toWTFString(globalObject));
}

String ErrorInstance::sanitizedToString(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Integrity::auditStructureID(structureID());

    String nameString = sanitizedNameString(globalObject);
    RETURN_IF_EXCEPTION(scope, String());

    String messageString = sanitizedMessageString(globalObject);
    RETURN_IF_EXCEPTION(scope, String());

    return makeString(nameString, nameString.isEmpty() || messageString.isEmpty() ? ""_s : ": "_s, messageString);
}

String ErrorInstance::tryGetMessageForDebugging()
{
    VM& vm = this->vm();

    JSValue messageValue;
    auto messagePropertName = vm.propertyNames->message;
    PropertySlot messageSlot(this, PropertySlot::InternalMethodType::VMInquiry, &vm);
    if (JSObject::getOwnNonIndexPropertySlot(vm, structure(), messagePropertName, messageSlot))
        messageValue = messageSlot.getPureResult();

    if (JSString* string = dynamicDowncast<JSString>(messageValue))
        return string->tryGetValue();
    return emptyString();
}

static bool hasUnmarkedFrame(VM& vm, const Vector<StackFrame>& frames)
{
    return std::ranges::any_of(frames, [&](const StackFrame& frame) {
        return !frame.isMarked(vm);
    });
}

void ErrorInstance::reconcileWeakReferencesAtGCEnd(VM& vm, CollectionScope)
{
    // We don't want to keep our stack traces alive forever if the user doesn't access the stack trace.
    // If we did, we might end up keeping functions (and their global objects) alive that happened to
    // get caught in a trace.
    // Since the frames are weak, a dead one means the trace can no longer be reconstructed, so
    // materialize it into strings while it is still readable.
    if (m_capturedStackTrace && hasUnmarkedFrame(vm, *m_capturedStackTrace)) {
        DeferGCForAWhile deferGC(vm);
        String stackString = Interpreter::stackTraceAsString(vm, *m_capturedStackTrace);
        Locker locker { cellLock() };
        m_stackString = WTF::move(stackString);
        m_capturedStackTrace->clear();
    }

#if USE(BUN_JSC_ADDITIONS)
    // The hook that composes "stack" is reading the frames, and keeps alive the ones it reads.
    if (m_isComposingStack)
        return;
#endif

#if ENABLE(AOT)
    if (m_returnAddresses && !vm.heap.isMarked(AOT::livenessToken(m_returnAddresses->instance))) [[unlikely]] {
        computeErrorInfo(vm, false);
        return;
    }
#endif
    if (m_stackTrace && hasUnmarkedFrame(vm, *m_stackTrace))
        computeErrorInfo(vm, false);
}

void ErrorInstance::computeErrorInfo(VM& vm, bool allocationAllowed)
{
#if USE(BUN_JSC_ADDITIONS)
    // The frames outlive the materialization of the other properties: "stack" is composed from them when it is read.
    // While the hook composes it, the hook reads them, and only an error that has none comes here.
    ASSERT(!m_isComposingStack || !m_stackTrace || m_stackTrace->isEmpty());
#else
    ASSERT(!m_errorInfoMaterialized);
#endif
    // Here we use DeferGCForAWhile instead of DeferGC since GC's Heap::runEndPhase can trigger this function. In
    // that case, DeferGC's destructor might trigger another GC cycle which is unexpected.
    DeferGCForAWhile deferGC(vm);
    UNUSED_PARAM(allocationAllowed);
    if (m_returnAddresses) [[unlikely]]
        materializeStackTrace();

    if (m_stackTrace && !m_stackTrace->isEmpty()) {
        auto& fn = vm.onComputeErrorInfo();
        WTF::String stackString;
        if (fn) {
            if (!m_stackPropertyProvidedByCapturedStackTrace) {
                // Possibly the end of a collection: the hook formats the frames, and composeStack() has the header put in front of them.
                stackString = fn(vm, *m_stackTrace.get(), m_lineColumn.line, m_lineColumn.column, m_sourceURL);
                m_stackStringIsFramesOnly = true;
            }
        } else {
            getLineColumnAndSource(vm, m_stackTrace.get(), m_lineColumn, m_sourceURL);
            if (!m_stackPropertyProvidedByCapturedStackTrace)
                stackString = Interpreter::stackTraceAsString(vm, *m_stackTrace.get());
        }
        m_hasErrorInfo = true;

        {
            Locker locker { cellLock() };
            m_stackTrace = nullptr;
            // Otherwise m_stackString is the captured stack trace's, if a collection had to format it.
            if (!m_stackPropertyProvidedByCapturedStackTrace)
                m_stackString = WTF::move(stackString);
        }

    }
}

#if USE(BUN_JSC_ADDITIONS)
// The own "stack" is the accessor that the host's hook gave for it: as it was put, or as defineOwnProperty() rebuilds an
// accessor whose attributes change (Object.freeze(), Object.seal()), which is a GetterSetter around the same getter.
bool ErrorInstance::stackIsHostAccessor(VM& vm, bool setterToo)
{
    unsigned attributes;
    PropertyOffset offset = structure()->get(vm, vm.propertyNames->stack, attributes);
    if (!isValidOffset(offset))
        return false;
    if (attributes & PropertyAttribute::CustomAccessor)
        return true;
    if (!(attributes & PropertyAttribute::Accessor))
        return false;
    // A program can define the getter of another object's accessor here. That one has another name.
    auto* getterSetter = dynamicDowncast<GetterSetter>(getDirect(offset));
    if (!getterSetter)
        return false;
    auto* getter = dynamicDowncast<JSCustomGetterFunction>(getterSetter->getter());
    if (!getter || getter->propertyName() != vm.propertyNames->stack)
        return false;
    if (!setterToo)
        return true;
    auto* setter = dynamicDowncast<JSCustomSetterFunction>(getterSetter->setter());
    return setter && setter->propertyName() == vm.propertyNames->stack;
}

void ErrorInstance::discardStackToCompose()
{
    // The hook is reading the frames. composeStack() drops them when it returns.
    if (m_isComposingStack) {
        m_stackReplacedWhileComposing = true;
        return;
    }

    Locker locker { cellLock() };
    m_stackTrace = nullptr;
    m_returnAddresses = nullptr;
    m_stackToComposeIsDetached = false;
    if (m_stackStringIsFramesOnly) {
        m_stackString = String();
        m_stackStringIsFramesOnly = false;
    }
}

// A value takes the place of what "stack" is. What was made of the property before stays: Object.defineProperty(),
// Object.seal(), Object.freeze(), whose accessor the value of a frozen error replaces as a read-only one.
void ErrorInstance::putStack(VM& vm, JSValue value)
{
    auto attributes = static_cast<unsigned>(PropertyAttribute::DontEnum);
    unsigned currentAttributes;
    if (isValidOffset(structure()->get(vm, vm.propertyNames->stack, currentAttributes))) {
        attributes = currentAttributes & (PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
        if (structure()->isFrozen(vm))
            attributes |= PropertyAttribute::ReadOnly;
    }
    // An error has "stack" from the start, so one with immutable properties still gets it. No JavaScript runs inside.
    AllowLazyMaterializationOfImmutableProperties allowMaterialization(vm);
    putDirect(vm, vm.propertyNames->stack, value, attributes);
}

bool ErrorInstance::setStack(VM& vm, JSValue value)
{
    // This is an assignment. An error whose properties are immutable refuses it.
    if (structure()->isImmutableProperty(vm.propertyNames->stack.impl())) [[unlikely]]
        return false;
    // The setter of the host's accessor outlives the accessor, and a program can call it on any error. What the program
    // made of the property stays: that it deleted it, the value or the getter that it defined in the place of the host's.
    if (m_stackToComposeIsDetached) [[unlikely]]
        return false;
    unsigned attributes;
    if (isValidOffset(structure()->get(vm, vm.propertyNames->stack, attributes))) {
        if (attributes & PropertyAttribute::Accessor) {
            if (!stackIsHostAccessor(vm)) [[unlikely]]
                return false;
        } else if (!(attributes & PropertyAttribute::CustomAccessor) && (attributes & PropertyAttribute::ReadOnly) && !m_isComposingStack) [[unlikely]] {
            // A value that an assignment cannot change. (While the hook runs, the value is one the host put for the hook to read.)
            return false;
        }
    } else if (m_errorInfoMaterialized && !isStructureExtensible()) [[unlikely]] {
        // The program deleted "stack", and the error takes no new property.
        return false;
    }
    discardStackToCompose();
    putStack(vm, value);
    return true;
}

// The one place "stack" is composed from the frames, or from what a collection made of them. Only a [[Get]] of "stack"
// and the getter of the host's accessor come here, as in V8, because the hook can run JavaScript for it: the error's name
// and message, Error.prepareStackTrace. If the hook throws, nothing has changed and the next read composes again. Returns
// what the hook composed, or the empty value if it threw. `stored` says that the value is the own "stack" now. It is not
// if the property is the program's, which can have become so inside the hook: the frames stay for the next call then.
JSValue ErrorInstance::composeStack(VM& vm, bool& stored)
{
    ASSERT(!m_isComposingStack);
    auto scope = DECLARE_THROW_SCOPE(vm);
    stored = false;

    if (m_returnAddresses) [[unlikely]]
        materializeStackTrace();

    bool hasFrames = m_stackTrace && !m_stackTrace->isEmpty();
    bool hasPosition = hasFrames || m_stackStringIsFramesOnly;
    // The hook maps the position it is given, which is a parse error's, as well as the top frame's: the fields stay as they
    // are for its next call.
    LineColumn lineColumn = m_lineColumn;
    String sourceURL = m_sourceURL;
    JSValue stack;
    {
        // The frames are weak. The hook keeps the ones it reads alive, and reconcileWeakReferencesAtGCEnd() leaves them to it.
        DeferGCForAWhile deferGC(vm);
        Vector<StackFrame> noFrames;
        // Frames captured after a collection formatted the ones before them are the ones to compose from.
        String formattedFrames = !hasFrames && m_stackStringIsFramesOnly ? m_stackString : String();
        m_isComposingStack = true;
        stack = vm.onComputeErrorInfoJSValue()(vm, hasFrames ? *m_stackTrace : noFrames, formattedFrames, lineColumn.line, lineColumn.column, sourceURL, this, ErrorInfoRequest::Stack);
        m_isComposingStack = false;
    }
    bool replaced = m_stackReplacedWhileComposing;
    m_stackReplacedWhileComposing = false;
    if (scope.exception()) [[unlikely]] {
        // What the hook's JavaScript made of "stack" before it threw stays, and nothing is composed over it.
        if (replaced)
            discardStackToCompose();
        return { };
    }
    if (!stack)
        stack = jsUndefined();

    if (m_stackToComposeIsDetached) [[unlikely]]
        return stack;

    discardStackToCompose();
    if (hasPosition && !m_errorInfoMaterialized) {
        m_errorInfoMaterialized = true;
        // An error has these from the start, so one with immutable properties still gets them. No JavaScript runs inside.
        AllowLazyMaterializationOfImmutableProperties allowMaterialization(vm);
        auto attributes = static_cast<unsigned>(PropertyAttribute::DontEnum);
        putDirect(vm, vm.propertyNames->line, jsNumber(lineColumn.line), attributes);
        putDirect(vm, vm.propertyNames->column, jsNumber(lineColumn.column), attributes);
        if (!sourceURL.isEmpty())
            putDirect(vm, vm.propertyNames->sourceURL, jsString(vm, WTF::move(sourceURL)), attributes);
    }
    if (hasPosition)
        m_sourceURL = String();

    // (A value that the hook's JavaScript assigned gives way to what the hook composed, as in V8.)
    putStack(vm, stack);
    stored = true;
    return stack;
}

JSValue ErrorInstance::composeStackForAccessor(VM& vm)
{
    if (m_isComposingStack || !vm.onComputeErrorInfoJSValue())
        return { };
    // With no frames the accessor is all that says a stack is wanted: Error.captureStackTrace() that found none, an error made by native code.
    if (hasStackToCompose() || stackIsHostAccessor(vm)) {
        bool stored;
        return composeStack(vm, stored);
    }
    return { };
}
#endif

bool ErrorInstance::materializeErrorInfoIfNeeded(VM& vm)
{
    bool materializedCapturedStackProperty = materializeCapturedStackPropertyIfSaved(vm);
    if (m_errorInfoMaterialized)
        return materializedCapturedStackProperty;
    if (m_returnAddresses) [[unlikely]]
        materializeStackTrace();

#if USE(BUN_JSC_ADDITIONS)

    auto& fn = vm.onComputeErrorInfoJSValue();
    if (fn && m_stackTrace && !m_stackTrace->isEmpty()) {
        // This is not a read of "stack": the position is wanted, and "stack" has to be an own property. No JavaScript runs
        // for that. The frames stay, and the host's accessor stands for "stack" until a read composes it from them.
        m_errorInfoMaterialized = true;
        DeferGCForAWhile deferGC(vm);

        LineColumn lineColumn = m_lineColumn;
        String sourceURL = m_sourceURL;
        JSValue stackAccessor;
        if (!m_stackPropertyProvidedByCapturedStackTrace) {
            DisallowVMEntry disallowVMEntry(vm);
            stackAccessor = fn(vm, *m_stackTrace.get(), String(), lineColumn.line, lineColumn.column, sourceURL, this, ErrorInfoRequest::Position);
        } else {
            // "stack" is the one Error.captureStackTrace() made. Nothing is composed from these frames.
            Locker locker { cellLock() };
            m_stackTrace = nullptr;
        }

        auto attributes = static_cast<unsigned>(PropertyAttribute::DontEnum);

        // An error has these from the start, so one with immutable properties still gets them.
        AllowLazyMaterializationOfImmutableProperties allowMaterialization(vm);
        putDirect(vm, vm.propertyNames->line, jsNumber(lineColumn.line), attributes);
        putDirect(vm, vm.propertyNames->column, jsNumber(lineColumn.column), attributes);
        if (!sourceURL.isEmpty())
            putDirect(vm, vm.propertyNames->sourceURL, jsString(vm, WTF::move(sourceURL)), attributes);

        putStackAccessorIfAbsent(vm, stackAccessor);
        return true;
    }

#endif

    computeErrorInfo(vm, true);

    if (m_hasErrorInfo) {
        auto attributes = static_cast<unsigned>(PropertyAttribute::DontEnum);

        {
            // An error has these from the start, so one with immutable properties still gets them. No JavaScript runs inside.
            AllowLazyMaterializationOfImmutableProperties allowMaterialization(vm);
            putDirect(vm, vm.propertyNames->line, jsNumber(m_lineColumn.line), attributes);
            putDirect(vm, vm.propertyNames->column, jsNumber(m_lineColumn.column), attributes);
            if (!m_sourceURL.isEmpty())
                putDirect(vm, vm.propertyNames->sourceURL, jsString(vm, WTF::move(m_sourceURL)), attributes);
        }

        if (!m_stackPropertyProvidedByCapturedStackTrace) {
#if USE(BUN_JSC_ADDITIONS)
            if (m_stackStringIsFramesOnly) {
                // A collection formatted the frames. The header is the error's name and message when "stack" is read,
                // so the host's accessor stands for "stack" here too.
                JSValue stackAccessor;
                if (fn) {
                    DeferGCForAWhile deferGC(vm);
                    DisallowVMEntry disallowVMEntry(vm);
                    Vector<StackFrame> noFrames;
                    LineColumn lineColumn = m_lineColumn;
                    String sourceURL;
                    stackAccessor = fn(vm, noFrames, m_stackString, lineColumn.line, lineColumn.column, sourceURL, this, ErrorInfoRequest::Position);
                }
                putStackAccessorIfAbsent(vm, stackAccessor);
            } else
#endif
            if (!m_stackString.isNull()) {
                WTF::String stackString;
                {
                    Locker locker { cellLock() };
                    stackString = WTF::move(m_stackString);
                }
                AllowLazyMaterializationOfImmutableProperties allowMaterialization(vm);
                putDirect(vm, vm.propertyNames->stack, jsString(vm, WTF::move(stackString)), attributes);
            }
        }
        m_errorInfoMaterialized = true;
    }

    return true;
}

#if USE(BUN_JSC_ADDITIONS)
void ErrorInstance::putStackAccessorIfAbsent(VM& vm, JSValue stackAccessor)
{
    if (!stackAccessor || isValidOffset(getDirectOffset(vm, vm.propertyNames->stack)))
        return;
    ASSERT(stackAccessor.isCustomGetterSetter());
    AllowLazyMaterializationOfImmutableProperties allowMaterialization(vm);
    putDirectCustomAccessor(vm, vm.propertyNames->stack, stackAccessor, PropertyAttribute::DontEnum | PropertyAttribute::CustomAccessor);
}
#endif

static bool isErrorInfoProperty(VM& vm, PropertyName propertyName)
{
    return propertyName == vm.propertyNames->line
        || propertyName == vm.propertyNames->column
        || propertyName == vm.propertyNames->sourceURL
        || propertyName == vm.propertyNames->stack;
}

bool ErrorInstance::isLazilyMaterialized(VM& vm, PropertyName propertyName)
{
    return isErrorInfoProperty(vm, propertyName);
}

bool ErrorInstance::materializeErrorInfoIfNeeded(VM& vm, PropertyName propertyName)
{
    if (isErrorInfoProperty(vm, propertyName))
        return materializeErrorInfoIfNeeded(vm);
    return false;
}

bool ErrorInstance::trySaveCapturedStackTraceForLazyMaterialization(VM& vm, Vector<StackFrame>& stackTrace)
{
    bool needsPlaceholder = !m_capturedStackTrace;
    if (needsPlaceholder && (isValidOffset(getDirectOffset(vm, vm.propertyNames->stack)) || !isStructureExtensible()))
        return false;
    auto capturedStackTrace = makeUnique<Vector<StackFrame>>(WTF::move(stackTrace));
    {
        Locker locker { cellLock() };
        m_capturedStackTrace = WTF::move(capturedStackTrace);
        m_stackString = String();
    }
    if (needsPlaceholder)
        putDirectCustomAccessor(vm, vm.propertyNames->stack, CustomGetterSetter::create(vm, errorInstanceMaterializingStackGetter, nullptr), PropertyAttribute::DontEnum | PropertyAttribute::CustomValue);
    vm.writeBarrier(this);
    return true;
}

bool ErrorInstance::materializeCapturedStackPropertyIfSaved(VM& vm)
{
    if (!m_capturedStackTrace)
        return false;

    std::unique_ptr<Vector<StackFrame>> capturedStackTrace;
    String stackString;
    {
        Locker locker { cellLock() };
        capturedStackTrace = WTF::move(m_capturedStackTrace);
        stackString = WTF::move(m_stackString);
    }
    if (stackString.isNull()) {
        // The frames are weak, so a collection while formatting could free what they point to.
        DeferGCForAWhile deferGC(vm);
        stackString = Interpreter::stackTraceAsString(vm, *capturedStackTrace);
    }
    putDirect(vm, vm.propertyNames->stack, jsString(vm, WTF::move(stackString)), static_cast<unsigned>(PropertyAttribute::DontEnum));
    return true;
}

bool ErrorInstance::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    ErrorInstance* thisObject = uncheckedDowncast<ErrorInstance>(object);
    // Only materializing can throw; a ThrowScope on the common path would oblige every caller to check.
    if (isErrorInfoProperty(vm, propertyName)) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(vm);
#if USE(BUN_JSC_ADDITIONS)
        // A [[Get]] of "stack" composes it, and nothing else does: every other lookup finds the host's accessor in its place.
        // Where that accessor is there already it composes the stack for a read on the error itself, so that an inline
        // cache of the read holds. It is given the receiver, so a read through another one (a Proxy) is composed here.
        if (propertyName == vm.propertyNames->stack && slot.internalMethodType() == PropertySlot::InternalMethodType::Get
            && thisObject->hasStackToCompose() && !thisObject->m_stackToComposeIsDetached && !thisObject->m_isComposingStack && !thisObject->m_stackPropertyProvidedByCapturedStackTrace && vm.onComputeErrorInfoJSValue()
            && (!thisObject->m_errorInfoMaterialized || slot.thisValue() != JSValue(thisObject) || !thisObject->stackIsHostAccessor(vm))) {
            bool stored;
            JSValue stack = thisObject->composeStack(vm, stored);
            RETURN_IF_EXCEPTION(scope, false);
            if (!stored) [[unlikely]] {
                // The hook's JavaScript defined or deleted "stack" while it composed it. The read gets what was composed.
                slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::DontEnum), stack);
                return true;
            }
        } else
#endif
            thisObject->materializeErrorInfoIfNeeded(vm);
        RETURN_IF_EXCEPTION(scope, false);
    }
    return Base::getOwnPropertySlot(thisObject, globalObject, propertyName, slot);
}

void ErrorInstance::getOwnSpecialPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& propertyNames, DontEnumPropertiesMode mode)
{
    VM& vm = globalObject->vm();
    ErrorInstance* thisObject = uncheckedDowncast<ErrorInstance>(object);
    // The lazy properties are not enumerable and their names are strings: no other listing has them.
    if (mode == DontEnumPropertiesMode::Include && propertyNames.includeStringProperties())
        thisObject->materializeErrorInfoIfNeeded(vm);
}

bool ErrorInstance::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ErrorInstance* thisObject = uncheckedDowncast<ErrorInstance>(object);
    if (isErrorInfoProperty(vm, propertyName)) [[unlikely]] {
        thisObject->materializeErrorInfoIfNeeded(vm);
        RETURN_IF_EXCEPTION(scope, {});
#if USE(BUN_JSC_ADDITIONS)
        if (propertyName == vm.propertyNames->stack) {
            bool result = Base::defineOwnProperty(thisObject, globalObject, propertyName, descriptor, shouldThrow);
            RETURN_IF_EXCEPTION(scope, {});
            // The property is the program's now, unless the descriptor left it what it was: one of attributes alone
            // (Object.freeze(), Object.seal()), the host's own getter and setter (a copy of another error's descriptor).
            if (result && thisObject->hasStackToCompose() && (descriptor.value() || descriptor.getterPresent() || descriptor.setterPresent()) && !thisObject->stackIsHostAccessor(vm, true))
                thisObject->m_stackToComposeIsDetached = true;
            return result;
        }
#endif
    }
    RELEASE_AND_RETURN(scope, Base::defineOwnProperty(thisObject, globalObject, propertyName, descriptor, shouldThrow));
}

bool ErrorInstance::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ErrorInstance* thisObject = uncheckedDowncast<ErrorInstance>(cell);
    if (isErrorInfoProperty(vm, propertyName)) [[unlikely]] {
        bool materializedProperties = thisObject->materializeErrorInfoIfNeeded(vm);
        RETURN_IF_EXCEPTION(scope, {});
        if (materializedProperties)
            slot.disableCaching();
#if USE(BUN_JSC_ADDITIONS)
        if (propertyName == vm.propertyNames->stack) {
            bool result = Base::put(thisObject, globalObject, propertyName, value, slot);
            RETURN_IF_EXCEPTION(scope, {});
            thisObject->discardStackToComposeIfReplaced(vm);
            return result;
        }
#endif
    }
    RELEASE_AND_RETURN(scope, Base::put(thisObject, globalObject, propertyName, value, slot));
}

bool ErrorInstance::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, DeletePropertySlot& slot)
{
    VM& vm = globalObject->vm();
    ErrorInstance* thisObject = uncheckedDowncast<ErrorInstance>(cell);
    if (isErrorInfoProperty(vm, propertyName)) [[unlikely]] {
        bool materializedProperties = thisObject->materializeErrorInfoIfNeeded(vm);
        if (materializedProperties)
            slot.disableCaching();
#if USE(BUN_JSC_ADDITIONS)
        if (propertyName == vm.propertyNames->stack) {
            bool result = Base::deleteProperty(thisObject, globalObject, propertyName, slot);
            if (result && thisObject->hasStackToCompose())
                thisObject->m_stackToComposeIsDetached = true;
            return result;
        }
#endif
    }
    return Base::deleteProperty(thisObject, globalObject, propertyName, slot);
}

#if USE(BUN_JSC_ADDITIONS)
// After "stack" was assigned: if the host's accessor is no longer what "stack" is, the property has its value and nothing
// composes it. (An assignment to a property that is the program's is the program's business.)
void ErrorInstance::discardStackToComposeIfReplaced(VM& vm)
{
    if (hasStackToCompose() && !m_stackToComposeIsDetached && !stackIsHostAccessor(vm))
        discardStackToCompose();
}
#endif

#if USE(BUN_JSC_ADDITIONS) && CPU(ADDRESS64)
static_assert(sizeof(ErrorInstance) == 80, "The state of a lazy stack is bits beside the others: an error stays the size it was");
#endif

} // namespace JSC
