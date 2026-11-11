/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <JavaScriptCore/JSExportMacros.h>
#include <wtf/Forward.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC {

class JSCell;
class JSGlobalObject;
class JSObject;
class JSValue;
class VM;

namespace AOT {

void noteOmittedCodeError(JSGlobalObject*, JSObject* error);
JS_EXPORT_PRIVATE JSValue takeOmittedMemberPlaceholder(JSGlobalObject*, const String& nameOfMember);
struct OmittedFunction {
    ASCIILiteral name;
    unsigned length { 0 };
    bool canBeConstructed { false };
    bool hasPrototype { true };
};
JS_EXPORT_PRIVATE JSValue takeOmittedFunctionPlaceholder(JSGlobalObject*, const OmittedFunction&, JSObject* owner);
bool isOmittedFunctionPlaceholder(JSCell*);
JS_EXPORT_PRIVATE bool isPlaceholderForOmittedBuiltin(JSCell*);
class MemberReificationScope {
public:
    explicit MemberReificationScope(bool isPresenceCheck);
    ~MemberReificationScope();

private:
    bool m_previous;
    bool m_previousForRead;
};
bool isReifyingForPresenceCheck();
void explainOmittedMemberError(JSGlobalObject*, JSObject* error, JSValue base);
void clearOmittedCodeError(VM&);
bool hasOmittedMemberNotes(const VM*);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
