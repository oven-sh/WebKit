#pragma once

#if ENABLE(AOT)

#include "AOTRuntime.h"
#include <wtf/Vector.h>

namespace JSC {

class JSArray;
class JSPromise;

namespace AOT {

struct ModuleEvaluation {
    Vector<JSArray*, 8> stack;
    unsigned index { 0 };
    int32_t referrerAsyncEvaluationOrder { -1 };
    JSPromise* importPromise { nullptr };
    ModuleEvaluation* outer { nullptr };
};

JSArray* newModuleRecord(Instance&, bool hasTopLevelAwait, JSValue execute);
bool enterModule(Instance&, JSArray* module);
void didEvaluateRequiredModule(Instance&, JSArray* module, JSArray* requiredModule);
void leaveModule(Instance&, JSArray* module);
JSArray* newImportTicket(Instance&, JSValue referrer);
void setImportPromise(Instance&, JSArray* ticket, JSValue promise);
JSPromise* evaluateModule(Instance&, JSArray* module, JSValue innerModuleEvaluation, JSValue ticket);
void didStartModuleBody(Instance&, JSArray* module, JSValue promise);
void asyncModuleExecutionFulfilled(Instance&, JSArray* module);
void asyncModuleExecutionRejected(Instance&, JSArray* module, JSValue error);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
