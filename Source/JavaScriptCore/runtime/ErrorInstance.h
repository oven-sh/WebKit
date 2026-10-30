/*
 *  Copyright (C) 1999-2000 Harri Porten (porten@kde.org)
 *  Copyright (C) 2008-2024 Apple Inc. All rights reserved.
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

#pragma once

#include "ErrorType.h"
#include "JSObject.h"
#include "RuntimeType.h"
#include "StackFrame.h"

namespace JSC {

class CallLinkInfo;
namespace AOT { struct ReturnAddressTrace; }

class ErrorInstance : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;

    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | OverridesGetOwnSpecialPropertyNames | OverridesPut | GetOwnPropertySlotIsImpureForPropertyAbsence;
    static constexpr DestructionMode needsDestruction = NeedsDestruction;

    static void destroy(JSCell* cell)
    {
        static_cast<ErrorInstance*>(cell)->ErrorInstance::~ErrorInstance();
    }

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.errorInstanceSpace<mode>();
    }

    enum class SourceTextWhereErrorOccurred { FoundExactSource, FoundApproximateSource };
    typedef String (*SourceAppender) (const String& originalMessage, StringView sourceText, RuntimeType, SourceTextWhereErrorOccurred);

    DECLARE_EXPORT_INFO;
    DECLARE_VISIT_CHILDREN;

    static size_t estimatedSize(JSCell*, VM&);

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

    static ErrorInstance* create(VM&, Structure*, const String& message, JSValue cause, SourceAppender = nullptr, RuntimeType = TypeNothing, ErrorType = ErrorType::Error, bool useCurrentFrame = true, JSCell* subclassCaller = nullptr);
    static ErrorInstance* create(VM&, Structure*, const String& message, JSValue cause, ErrorType, JSCell* owner, CallLinkInfo*);

    JS_EXPORT_PRIVATE static ErrorInstance* create(JSGlobalObject*, String&& message, ErrorType, LineColumn, String&& sourceURL, String&& stackString, String&& cause = { });
    static ErrorInstance* create(JSGlobalObject*, Structure*, JSValue message, JSValue options, SourceAppender = nullptr, RuntimeType = TypeNothing, ErrorType = ErrorType::Error, bool useCurrentFrame = true, JSCell* subclassCaller = nullptr);

    JS_EXPORT_PRIVATE void captureStackTrace(VM &vm, JSC::JSGlobalObject* globalObject, size_t framesToSkip = 0, bool append = false);

    bool hasSourceAppender() const { return !!m_sourceAppender; }
    SourceAppender sourceAppender() const { return m_sourceAppender; }
    void setSourceAppender(SourceAppender appender) { m_sourceAppender = appender; }
    void clearSourceAppender() { m_sourceAppender = nullptr; }
    void setRuntimeTypeForCause(RuntimeType type) { m_runtimeTypeForCause = type; }
    RuntimeType runtimeTypeForCause() const { return m_runtimeTypeForCause; }
    void clearRuntimeTypeForCause() { m_runtimeTypeForCause = TypeNothing; }
    ErrorType errorType() const { return m_errorType; }
    void setStackOverflowError()
    {
#if ENABLE(WEBASSEMBLY)
        m_catchableFromWasm = false;
#endif
        m_stackOverflowError = true;
    }
    bool isStackOverflowError() const { return m_stackOverflowError; }
    void setOutOfMemoryError()
    {
#if ENABLE(WEBASSEMBLY)
        m_catchableFromWasm = false;
#endif
        m_outOfMemoryError = true;
    }
    bool isOutOfMemoryError() const { return m_outOfMemoryError; }

    void setNativeGetterTypeError() { m_nativeGetterTypeError = true; }
    bool isNativeGetterTypeError() const { return m_nativeGetterTypeError; }

    void setParseError() { m_parseError = true; }
    bool isParseError() const { return m_parseError; }

#if ENABLE(WEBASSEMBLY)
    void setCatchableFromWasm(bool flag) { m_catchableFromWasm = flag; }
    bool isCatchableFromWasm() const { return m_catchableFromWasm; }
#endif

#if USE(BUN_JSC_ADDITIONS)
    const String& sourceURL() const { return m_sourceURL; }
    void setSourceURL(String sourceURL) { m_sourceURL = sourceURL; } 

    unsigned line() const { return m_lineColumn.line; }
    void setLine(unsigned line) { m_lineColumn.line = line; }

    unsigned column() const { return m_lineColumn.column; }
    void setColumn(unsigned column) { m_lineColumn.column = column; }
    void setStackFrames(VM& vm, WTF::Vector<JSC::StackFrame>&& stackFrames);
    // The line, the column and the sourceURL are own properties, or there are none to make. It says nothing about "stack".
    bool hasMaterializedErrorInfo() const { return m_errorInfoMaterialized; }

    // "stack" is composed (VM::onComputeErrorInfoJSValue() is asked for ErrorInfoRequest::Stack, which can run JavaScript)
    // only in a [[Get]] of it. Anything else that needs the error's own properties puts the host's accessor in its place,
    // and that accessor's getter calls composeStackForAccessor(): unlike a read of an untouched error it composes a stack
    // with no frames too, which is the header alone. The result is the stack, or the empty value if none was composed:
    // the hook threw, or it is running for this error now.
    JS_EXPORT_PRIVATE JSValue composeStackForAccessor(VM&);
    // The hook is running for ErrorInfoRequest::Stack: the frames are in use, and what it returns becomes "stack".
    bool isComposingStack() const { return m_isComposingStack; }
    // There are frames, or what a collection made of them, that "stack" has not been composed from yet.
    bool hasStackToCompose() const { return (m_stackTrace && !m_stackTrace->isEmpty()) || m_returnAddresses || m_stackStringIsFramesOnly; }
    // "stack" is given this value, as an assignment gives it, so none is composed: the host's accessor calls this from
    // its setter. While the hook runs, what the hook returns still takes the place of the value.
    JS_EXPORT_PRIVATE void setStack(VM&, JSValue);

#endif

    JS_EXPORT_PRIVATE String sanitizedToString(JSGlobalObject*);
    // A message or a name that is a Symbol has no string, and the conversion throws a TypeError. A caller that compares
    // errors gives `symbol`: it gets the Symbol there and the null string, and nothing is thrown.
    JS_EXPORT_PRIVATE String sanitizedMessageString(JSGlobalObject*, Symbol** symbol = nullptr);
    JS_EXPORT_PRIVATE String sanitizedNameString(JSGlobalObject*, Symbol** symbol = nullptr);
    
    JS_EXPORT_PRIVATE String tryGetMessageForDebugging();

    Vector<StackFrame>* stackTrace() LIFETIME_BOUND
    {
        if (m_returnAddresses) [[unlikely]]
            materializeStackTrace();
        return m_stackTrace.get();
    }
    AOT::ReturnAddressTrace* returnAddresses() const { return m_returnAddresses; }
    const Vector<StackFrame>* framesBesideReturnAddresses() const { return m_stackTrace.get(); }

    bool materializeErrorInfoIfNeeded(VM&);
    bool materializeErrorInfoIfNeeded(VM&, PropertyName);
    static bool isLazilyMaterialized(VM&, PropertyName);

    // Replaces the stack trace captured by finishCreationForEmbedderError() with information that
    // came from somewhere else, such as a structured clone of an embedder error. The properties are
    // still materialized lazily, exactly as they are for a captured stack trace.
    JS_EXPORT_PRIVATE void setErrorInfoForEmbedderError(LineColumn, String&& sourceURL, String&& stackString);

    void setStackPropertyProvidedByCapturedStackTrace()
    {
        if (!m_errorInfoMaterialized)
            m_stackPropertyProvidedByCapturedStackTrace = true;
    }

    bool trySaveCapturedStackTraceForLazyMaterialization(VM&, Vector<StackFrame>&);

    JS_EXPORT_PRIVATE void reconcileWeakReferencesAtGCEnd(VM&, CollectionScope);

protected:
    JS_EXPORT_PRIVATE explicit ErrorInstance(VM&, Structure*, ErrorType);

    void finishCreation(VM&, const String& message, JSValue cause, SourceAppender = nullptr, RuntimeType = TypeNothing, bool useCurrentFrame = true, JSCell* subclassCaller = nullptr);
    void finishCreation(VM&, const String& message, JSValue cause, JSCell* owner, CallLinkInfo*);
    void finishCreation(VM&, String&& message, LineColumn, String&& sourceURL, String&& stackString, String&& cause);

    // For subclasses (e.g. embedder error wrappers like WebCore's DOMException) that should be
    // ErrorInstances but must not gain own "message" / "cause" properties; they expose those by
    // other means. A stack trace is still captured, so "stack" (and "line" / "column" /
    // "sourceURL") materialize lazily just like they do for a plain Error.
    JS_EXPORT_PRIVATE void finishCreationForEmbedderError(VM&);

    JS_EXPORT_PRIVATE static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    JS_EXPORT_PRIVATE static void getOwnSpecialPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    JS_EXPORT_PRIVATE static bool defineOwnProperty(JSObject*, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow);
    JS_EXPORT_PRIVATE static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    JS_EXPORT_PRIVATE static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);

    void computeErrorInfo(VM&, bool allocationAllowed);
    JS_EXPORT_PRIVATE void materializeStackTrace();
    bool tryRetainReturnAddresses(VM&, bool useCurrentFrame, JSCell* subclassCaller, std::unique_ptr<Vector<StackFrame>>& otherFrames);
    bool materializeCapturedStackPropertyIfSaved(VM&);

    SourceAppender m_sourceAppender { nullptr };
    // Frames captured by the constructor
    std::unique_ptr<Vector<StackFrame>> m_stackTrace;
    AOT::ReturnAddressTrace* m_returnAddresses { nullptr };
    // Frames captured by Error.captureStackTrace()
    std::unique_ptr<Vector<StackFrame>> m_capturedStackTrace;

    LineColumn m_lineColumn;
    String m_sourceURL;
    String m_stackString;
    RuntimeType m_runtimeTypeForCause { TypeNothing };
    ErrorType m_errorType { ErrorType::Error };
    bool m_stackOverflowError : 1;
    bool m_outOfMemoryError : 1;
    bool m_errorInfoMaterialized : 1;
    bool m_hasErrorInfo : 1;
    bool m_stackPropertyProvidedByCapturedStackTrace : 1;
    bool m_nativeGetterTypeError : 1;
    bool m_parseError : 1;
#if ENABLE(WEBASSEMBLY)
    bool m_catchableFromWasm : 1;
#endif
#if USE(BUN_JSC_ADDITIONS)
    bool m_stackStringIsFramesOnly : 1;
    bool m_isComposingStack : 1;
    bool m_stackReplacedWhileComposing : 1;
    JSValue composeStack(VM&);
    void discardStackToCompose();
    void discardStackToComposeIfReplaced(VM&);
    void putStack(VM&, JSValue);
    void putStackAccessorIfAbsent(VM&, JSValue stackAccessor);
    bool stackIsHostAccessor(VM&);
#endif
};

String appendSourceToErrorMessage(CodeBlock*, BytecodeIndex, const String&, RuntimeType, ErrorInstance::SourceAppender);

} // namespace JSC
