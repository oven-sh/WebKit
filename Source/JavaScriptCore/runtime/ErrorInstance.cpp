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
#include "ErrorInstanceInlines.h"
#include "InlineCallFrame.h"
#include "IntegrityInlines.h"
#include "Interpreter.h"
#include "JSCInlines.h"
#include "ParseInt.h"
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
    , m_positionPropertiesOwnedByUser(0)
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
    std::unique_ptr<Vector<StackFrame>> stackTrace = makeUnique<Vector<StackFrame>>(WTF::move(stackFrames));

    Locker locker { cellLock() };
    m_stackTrace = WTF::move(stackTrace);
    m_returnAddresses = nullptr;
    // A collection may already have formatted the frames these replace.
    m_stackString = String();
    m_hasErrorInfo = false;
#if USE(BUN_JSC_ADDITIONS)
    m_stackStringIsFramesOnly = false;
#endif
    vm.writeBarrier(this);
    locker.unlockEarly();
    reportStackTraceMemory(vm);
}

size_t ErrorInstance::stackTraceMemorySize() const
{
    size_t size = 0;
    if (m_stackTrace)
        size += m_stackTrace->sizeInBytes();
    if (m_capturedStackTrace)
        size += m_capturedStackTrace->sizeInBytes();
    if (auto* string = m_stackString.impl())
        size += string->sizeInBytes();
    return size;
}

// For where the frames go away, or become a string at the end of a collection.
void ErrorInstance::updateReportedStackTraceMemory()
{
    size_t sizeInKB = stackTraceMemorySize() / KB;
    m_reportedStackTraceMemoryInKB.store(static_cast<uint16_t>(std::min<size_t>(sizeInKB, UINT16_MAX)), std::memory_order_relaxed);
}

// Can collect.
NEVER_INLINE void ErrorInstance::reportStackTraceMemory(VM& vm)
{
    uint16_t before = m_reportedStackTraceMemoryInKB.load(std::memory_order_relaxed);
    updateReportedStackTraceMemory();
    uint16_t after = m_reportedStackTraceMemoryInKB.load(std::memory_order_relaxed);
    if (after > before)
        vm.heap.reportExtraMemoryAllocated(this, (after - before) * KB);
}

// The frames of an error with the default Error.stackTraceLimit take less than a KB, which is not reported.
static ALWAYS_INLINE bool isLongStackTrace(const std::unique_ptr<Vector<StackFrame>>& stackTrace)
{
    return stackTrace && stackTrace->size() >= KB / sizeof(StackFrame);
}

template<typename Visitor>
void ErrorInstance::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    ErrorInstance* thisObject = uncheckedDowncast<ErrorInstance>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    if (size_t reportedInKB = thisObject->m_reportedStackTraceMemoryInKB.load(std::memory_order_relaxed)) [[unlikely]]
        visitor.reportExtraMemoryVisited(reportedInKB * KB);
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
    std::optional<unsigned> stackTraceLimit = realm()->stackTraceLimit();
    if (!stackTraceLimit)
        return false;
    if (!*stackTraceLimit) {
        otherFrames = makeUnique<Vector<StackFrame>>();
        return true;
    }
    auto* returnAddresses = AOT::tryCaptureStackTrace(vm, this, *stackTraceLimit, otherFrames, subclassCaller, useCurrentFrame ? AOT::TopFramePolicy::Include : AOT::TopFramePolicy::Skip);
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

        if (m_stackTrace && append) {
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
    reportStackTraceMemory(vm);
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
        else if (String quoted = AOT::failedExpressionSource(vm); !quoted.isNull() && !message.isNull()) {
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

    if (isLongStackTrace(m_stackTrace)) [[unlikely]]
        reportStackTraceMemory(vm);
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

    if (isLongStackTrace(m_stackTrace)) [[unlikely]]
        reportStackTraceMemory(vm);
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

    reportStackTraceMemory(vm);
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

    if (isLongStackTrace(m_stackTrace)) [[unlikely]]
        reportStackTraceMemory(vm);
}

void ErrorInstance::setErrorInfoForEmbedderError(LineColumn lineColumn, String&& sourceURL, String&& stackString)
{
    VM& vm = this->vm();
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
    reportStackTraceMemory(vm);
}

// Based on ErrorPrototype's errorProtoFuncToString(), but is modified to
// have no observable side effects to the user (i.e. does not call proxies,
// and getters).
String ErrorInstance::sanitizedMessageString(JSGlobalObject* globalObject)
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

    RELEASE_AND_RETURN(scope, messageValue.toWTFString(globalObject));
}

String ErrorInstance::sanitizedNameString(JSGlobalObject* globalObject)
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
        updateReportedStackTraceMemory();
    }

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
    ASSERT(!m_errorInfoMaterialized);
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
                // Possibly the end of a collection: the hook formats the frames, and stackWithHeader() prepends the header.
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
            updateReportedStackTraceMemory();
        }

    }
}

#if USE(BUN_JSC_ADDITIONS)
// "name: message" in front of the frames a collection formatted, as a stack materialized on access
// begins. undefined if reading the name or the message throws, which is what the hook that
// materializes a stack on access leaves.
JSValue ErrorInstance::stackWithHeader(VM& vm, String&& frames)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSGlobalObject* globalObject = this->globalObject();
    String name = sanitizedNameString(globalObject);
    RETURN_IF_EXCEPTION(scope, jsUndefined());
    String message = sanitizedMessageString(globalObject);
    RETURN_IF_EXCEPTION(scope, jsUndefined());
    // The name and the message come from JS: past String::MaxLength makeString() calls CRASH().
    ASCIILiteral separator = name.isEmpty() || message.isEmpty() ? ""_s : ": "_s;
    String stack = tryMakeString(name, separator, message, frames);
    if (stack.isNull())
        stack = tryMakeString(name, separator, message);
    if (stack.isNull())
        stack = WTF::move(message);
    return jsString(vm, WTF::move(stack));
}
#endif

#if USE(BUN_JSC_ADDITIONS)
// The bits of m_positionPropertiesOwnedByUser.
static constexpr uint8_t lineProperty = 1 << 0;
static constexpr uint8_t columnProperty = 1 << 1;
static constexpr uint8_t sourceURLProperty = 1 << 2;

// What the engine gives the line, column and sourceURL that it defines.
static constexpr unsigned positionPropertyAttributes = static_cast<unsigned>(PropertyAttribute::DontEnum);

static uint8_t positionPropertyFor(VM& vm, PropertyName propertyName)
{
    if (propertyName == vm.propertyNames->line)
        return lineProperty;
    if (propertyName == vm.propertyNames->column)
        return columnProperty;
    if (propertyName == vm.propertyNames->sourceURL)
        return sourceURLProperty;
    return 0;
}

// Whether materialization writes the name. It does not write one that user code owns. A store that an inline cache
// answered does not call put(), so it marks nothing: the property it made has other attributes than the engine's, and
// that one stays too. The engine's own is written again, which is how frames installed later give the error their position.
bool ErrorInstance::materializesPositionProperty(VM& vm, PropertyName propertyName, uint8_t property)
{
    if (m_positionPropertiesOwnedByUser & property)
        return false;
    Structure* structure = this->structure();
    // A transition that adds the name says that the error does not have it, without a lookup in the property table.
    PropertyOffset offset;
    if (!structure->isDictionary() && Structure::addPropertyTransitionToExistingStructure(structure, propertyName, positionPropertyAttributes, offset))
        return true;
    unsigned attributes;
    return !isValidOffset(structure->get(vm, propertyName, attributes)) || attributes == positionPropertyAttributes;
}

// The caller holds an AllowLazyMaterializationOfImmutableProperties. No JavaScript runs.
void ErrorInstance::putPositionProperties(VM& vm)
{
    if (materializesPositionProperty(vm, vm.propertyNames->line, lineProperty))
        putDirect(vm, vm.propertyNames->line, jsNumber(m_lineColumn.line), positionPropertyAttributes);
    if (materializesPositionProperty(vm, vm.propertyNames->column, columnProperty))
        putDirect(vm, vm.propertyNames->column, jsNumber(m_lineColumn.column), positionPropertyAttributes);
    if (!m_sourceURL.isEmpty()) {
        // The field is emptied whether or not the property is written, as before: the host reads it when it formats frames.
        String sourceURL = WTF::move(m_sourceURL);
        if (materializesPositionProperty(vm, vm.propertyNames->sourceURL, sourceURLProperty))
            putDirect(vm, vm.propertyNames->sourceURL, jsString(vm, WTF::move(sourceURL)), positionPropertyAttributes);
    }
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
        m_errorInfoMaterialized = true;
        DeferGCForAWhile deferGC(vm);

        JSValue stack;
        if (!m_stackPropertyProvidedByCapturedStackTrace)
            stack = fn(vm, *m_stackTrace.get(), m_lineColumn.line, m_lineColumn.column, m_sourceURL, this);

        {
            Locker locker { cellLock() };
            m_stackTrace->clear();
            m_stackTrace = nullptr;
            m_stackString = String();
            updateReportedStackTraceMemory();
        }

        auto attributes = static_cast<unsigned>(PropertyAttribute::DontEnum);

        // An error has these from the start, so one with immutable properties still gets them. No JavaScript runs from here on.
        AllowLazyMaterializationOfImmutableProperties allowMaterialization(vm);
        putPositionProperties(vm);

        if (!m_stackPropertyProvidedByCapturedStackTrace)
            putDirect(vm, vm.propertyNames->stack, stack, attributes);
        return true;
    }

#endif

    computeErrorInfo(vm, true);

    if (m_hasErrorInfo) {
        auto attributes = static_cast<unsigned>(PropertyAttribute::DontEnum);

        {
            // An error has these from the start, so one with immutable properties still gets them. No JavaScript runs inside.
            AllowLazyMaterializationOfImmutableProperties allowMaterialization(vm);
#if USE(BUN_JSC_ADDITIONS)
            putPositionProperties(vm);
#else
            putDirect(vm, vm.propertyNames->line, jsNumber(m_lineColumn.line), attributes);
            putDirect(vm, vm.propertyNames->column, jsNumber(m_lineColumn.column), attributes);
            if (!m_sourceURL.isEmpty())
                putDirect(vm, vm.propertyNames->sourceURL, jsString(vm, WTF::move(m_sourceURL)), attributes);
#endif
        }

        if (!m_stackPropertyProvidedByCapturedStackTrace) {
            WTF::String stackString;
            {
                Locker locker { cellLock() };
                stackString = WTF::move(m_stackString);
                updateReportedStackTraceMemory();
            }
            // The value is made before the scope opens: stackWithHeader() reads this error's name and message.
            JSValue stackValue;
#if USE(BUN_JSC_ADDITIONS)
            if (m_stackStringIsFramesOnly)
                stackValue = stackWithHeader(vm, WTF::move(stackString));
            else
#endif
                stackValue = jsString(vm, WTF::move(stackString));
            AllowLazyMaterializationOfImmutableProperties allowMaterialization(vm);
            putDirect(vm, vm.propertyNames->stack, stackValue, attributes);
        }
        m_errorInfoMaterialized = true;
    }

    return true;
}

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
    reportStackTraceMemory(vm);
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
        updateReportedStackTraceMemory();
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
        thisObject->materializeErrorInfoIfNeeded(vm);
        RETURN_IF_EXCEPTION(scope, false);
    }
    return Base::getOwnPropertySlot(thisObject, globalObject, propertyName, slot);
}

void ErrorInstance::getOwnSpecialPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder&, DontEnumPropertiesMode mode)
{
    VM& vm = globalObject->vm();
    ErrorInstance* thisObject = uncheckedDowncast<ErrorInstance>(object);
    if (mode == DontEnumPropertiesMode::Include)
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
        if (uint8_t property = positionPropertyFor(vm, propertyName)) {
            // The descriptor is applied to what the engine defined, as before. The result is user code's.
            bool isDefined = Base::defineOwnProperty(thisObject, globalObject, propertyName, descriptor, shouldThrow);
            RETURN_IF_EXCEPTION(scope, {});
            if (isDefined)
                thisObject->m_positionPropertiesOwnedByUser |= property;
            return isDefined;
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
#if USE(BUN_JSC_ADDITIONS)
        if (uint8_t property = positionPropertyFor(vm, propertyName))
            RELEASE_AND_RETURN(scope, thisObject->putPositionProperty(globalObject, propertyName, property, value, slot));
#endif
        bool materializedProperties = thisObject->materializeErrorInfoIfNeeded(vm);
        RETURN_IF_EXCEPTION(scope, {});
        if (materializedProperties)
            slot.disableCaching();
    }
    RELEASE_AND_RETURN(scope, Base::put(thisObject, globalObject, propertyName, value, slot));
}

#if USE(BUN_JSC_ADDITIONS)
bool ErrorInstance::putPositionProperty(JSGlobalObject* globalObject, PropertyName propertyName, uint8_t property, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // A store that only passes through, from an object that inherits from this error, leaves the name the engine's. So does a
    // store to an error whose properties are immutable, which is refused.
    bool isStoreOfUserCode = !isThisValueAltered(slot, this) && !structure()->isImmutableProperty(propertyName.uid());
    // The first store of user code makes the name an ordinary property, as a store to an Error makes one in V8, which defines
    // no line, column or sourceURL. The name is marked before materialization, which then leaves it to this store.
    bool takesOver = isStoreOfUserCode && !(m_positionPropertiesOwnedByUser & property);
    bool wasExtensible = isStructureExtensible();
    if (isStoreOfUserCode)
        m_positionPropertiesOwnedByUser |= property;
    bool materializedProperties = materializeErrorInfoIfNeeded(vm);
    RETURN_IF_EXCEPTION(scope, {});
    unsigned attributes;
    PropertyOffset offset = structure()->get(vm, propertyName, attributes);
    if (takesOver) {
        if (isValidOffset(offset) && attributes == positionPropertyAttributes && isStructureExtensible()) {
            // The engine defined the name: an earlier lookup, a store of another name, or the parser. It is removed, and the
            // store below adds it again behind the properties that the error has now. A removal shares its Structure
            // transition only for a property without attributes (Structure.cpp looks the transition up under the attributes
            // of the property and files it under none), so the attributes are cleared first. An error that takes no more
            // properties keeps the engine's, and the store replaces its value.
            putDirect(vm, propertyName, getDirect(offset), 0);
            DeletePropertySlot deleteSlot;
            JSObject::deleteProperty(this, globalObject, propertyName, deleteSlot);
            RETURN_IF_EXCEPTION(scope, {});
            offset = invalidOffset;
        } else if (!isValidOffset(offset) && wasExtensible && !isStructureExtensible() && !structure()->isImmutableProperty(propertyName.uid())) {
            // The host's stack hook made the error refuse new properties during this store. The value goes where the engine
            // would have defined the name, as before.
            putDirect(vm, propertyName, value, positionPropertyAttributes);
            slot.disableCaching();
            return true;
        }
    }
    // An inline cache answers a later store by the Structure and does not call this function. That is right only for a store
    // that replaces a property of user code: a store to the engine's own has to come here to take the name over.
    if (materializedProperties || !isValidOffset(offset) || (attributes & PropertyAttribute::DontEnum))
        slot.disableCaching();
    RELEASE_AND_RETURN(scope, Base::put(this, globalObject, propertyName, value, slot));
}
#endif

bool ErrorInstance::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, DeletePropertySlot& slot)
{
    VM& vm = globalObject->vm();
    ErrorInstance* thisObject = uncheckedDowncast<ErrorInstance>(cell);
    if (isErrorInfoProperty(vm, propertyName)) [[unlikely]] {
        bool isCacheable = true;
#if USE(BUN_JSC_ADDITIONS)
        // Marked before materialization: a name that the engine has not defined yet is then not defined at all. An error
        // whose properties are immutable has its properties from the start, so it keeps the engine's. A cached delete
        // does not come here.
        if (uint8_t property = positionPropertyFor(vm, propertyName); property && !thisObject->structure()->isImmutableProperty(propertyName.uid())) {
            thisObject->m_positionPropertiesOwnedByUser |= property;
            isCacheable = false;
        }
#endif
        bool materializedProperties = thisObject->materializeErrorInfoIfNeeded(vm);
        if (materializedProperties || !isCacheable)
            slot.disableCaching();
    }
    return Base::deleteProperty(thisObject, globalObject, propertyName, slot);
}

} // namespace JSC
