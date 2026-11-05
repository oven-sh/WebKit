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

void noteErrorOfCodeLeftOut(JSGlobalObject*, JSObject* error);
JS_EXPORT_PRIVATE JSValue takeStandInForMemberLeftOut(JSGlobalObject*, const String& nameOfMember, JSObject* answeredBy = nullptr);
struct FunctionLeftOut {
    ASCIILiteral name;
    unsigned length { 0 };
    bool canBeConstructed { false };
    bool hasPrototype { true };
};
JS_EXPORT_PRIVATE JSValue takeStandInForFunctionLeftOut(JSGlobalObject*, const FunctionLeftOut&, JSObject* answeredBy);
bool standsInForFunction(JSCell*);
JS_EXPORT_PRIVATE bool isPlaceholderForOmittedBuiltin(JSCell*);
class MakingOfMemberForLook {
public:
    explicit MakingOfMemberForLook(bool lookOnlyTests);
    ~MakingOfMemberForLook();

private:
    bool m_previous;
};
bool isMakingMemberForLookThatOnlyTests();
JSValue takeStandInForGetterLeftOut(JSGlobalObject*, JSObject* getter);
void explainErrorBehindMemberLeftOut(JSGlobalObject*, JSObject* error, JSValue base);
void forgetErrorOfCodeLeftOut(VM&);
bool isAnythingNotedOfMembersLeftOut(const VM*);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
