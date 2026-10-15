#include "config.h"
#include "AOTModuleEvaluation.h"

#if ENABLE(AOT)

#include "AOTTypedObject.h"
#include "InternalFieldTuple.h"
#include "JSArray.h"
#include "JSAsyncFromSyncIterator.h"
#include "JSAsyncFunctionGenerator.h"
#include "JSCInlines.h"
#include "JSPromise.h"
#include "JSPromiseCombinatorsGlobalContext.h"
#include "JSPromiseReaction.h"
#include "Microtask.h"
#include <wtf/HashSet.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

namespace {

enum Field : unsigned {
    StatusField,
    HasEvaluationErrorField,
    EvaluationErrorField,
    DFSIndexField,
    DFSAncestorIndexField,
    PendingAsyncDependenciesField,
    AsyncEvaluationOrderField,
    CycleRootField,
    TopLevelCapabilityField,
    AsyncParentModulesField,
    HasTLAField,
    ExecuteField,
    BodyPromiseField,
    numberOfFields,
};

enum class Status : int32_t { Linked, Evaluating, EvaluatingAsync, Evaluated };

constexpr int32_t orderIsUnset = -1;
constexpr int32_t orderIsDone = -2;

class Record {
public:
    Record(VM& vm, JSArray* cell)
        : m_vm(&vm)
        , m_cell(cell)
    {
    }

    JSArray* cell() const { return m_cell; }
    bool operator==(const Record& other) const { return m_cell == other.m_cell; }

    Status status() const { return static_cast<Status>(number(StatusField)); }
    void setStatus(Status status) { set(StatusField, jsNumber(static_cast<int32_t>(status))); }
    bool hasEvaluationError() const { return get(HasEvaluationErrorField).isTrue(); }
    JSValue evaluationError() const { return get(EvaluationErrorField); }
    void setEvaluationError(JSValue error)
    {
        set(HasEvaluationErrorField, jsBoolean(true));
        set(EvaluationErrorField, error);
    }
    int32_t dfsIndex() const { return number(DFSIndexField); }
    void setDFSIndex(int32_t index) { set(DFSIndexField, jsNumber(index)); }
    int32_t dfsAncestorIndex() const { return number(DFSAncestorIndexField); }
    void setDFSAncestorIndex(int32_t index) { set(DFSAncestorIndexField, jsNumber(index)); }
    int32_t pendingAsyncDependencies() const { return number(PendingAsyncDependenciesField); }
    void setPendingAsyncDependencies(int32_t count) { set(PendingAsyncDependenciesField, jsNumber(count)); }
    int32_t asyncEvaluationOrder() const { return number(AsyncEvaluationOrderField); }
    void setAsyncEvaluationOrder(int32_t order) { set(AsyncEvaluationOrderField, jsNumber(order)); }
    JSArray* cycleRoot() const { return arrayOrNull(CycleRootField); }
    void setCycleRoot(JSArray* root) { set(CycleRootField, root); }
    JSPromise* topLevelCapability() const
    {
        JSValue value = get(TopLevelCapabilityField);
        return value.isCell() ? uncheckedDowncast<JSPromise>(value.asCell()) : nullptr;
    }
    void setTopLevelCapability(JSPromise* promise) { set(TopLevelCapabilityField, promise); }
    JSArray* asyncParentModules() const { return arrayOrNull(AsyncParentModulesField); }
    void setAsyncParentModules(JSArray* parents) { set(AsyncParentModulesField, parents); }
    bool hasTLA() const { return get(HasTLAField).isTrue(); }
    JSValue execute() const { return get(ExecuteField); }
    JSValue bodyPromise() const { return get(BodyPromiseField); }
    void setBodyPromise(JSValue promise) { set(BodyPromiseField, promise); }

private:
    JSValue get(Field field) const { return m_cell->getIndexQuickly(field); }
    int32_t number(Field field) const { return get(field).asInt32(); }
    JSArray* arrayOrNull(Field field) const
    {
        JSValue value = get(field);
        return value.isCell() ? uncheckedDowncast<JSArray>(value.asCell()) : nullptr;
    }
    void set(Field field, JSValue value) { m_cell->setIndexQuickly(*m_vm, field, value); }

    VM* m_vm;
    JSArray* m_cell;
};

void executeModule(JSGlobalObject* globalObject, const Record& module)
{
    JSValue function = module.execute();
    call(globalObject, function, JSC::getCallData(function), jsUndefined(), ArgList());
}

void gatherAvailableAncestors(VM& vm, JSArray* module, Vector<JSArray*, 8>& execList)
{
    Vector<JSArray*, 8> worklist;
    worklist.append(module);
    while (!worklist.isEmpty()) {
        JSArray* parents = Record(vm, worklist.takeLast()).asyncParentModules();
        if (!parents)
            continue;
        for (unsigned i = 0; i < parents->length(); ++i) {
            Record m(vm, uncheckedDowncast<JSArray>(parents->getIndexQuickly(i).asCell()));
            JSArray* root = m.cycleRoot();
            if (!root || Record(vm, root).hasEvaluationError())
                continue;
            int32_t pending = m.pendingAsyncDependencies();
            if (!pending)
                continue;
            m.setPendingAsyncDependencies(pending - 1);
            if (pending == 1) {
                execList.append(m.cell());
                if (!m.hasTLA())
                    worklist.append(m.cell());
            }
        }
    }
}

bool resumesDependency(VM& vm, JSArray* module, JSArray* dependency)
{
    UncheckedKeyHashSet<JSArray*> seen;
    Vector<JSArray*, 8> work;
    work.append(module);
    while (!work.isEmpty()) {
        JSArray* current = work.takeLast();
        if (current == dependency)
            return true;
        if (!seen.add(current).isNewEntry)
            continue;
        if (JSArray* parents = Record(vm, current).asyncParentModules()) {
            for (unsigned i = 0; i < parents->length(); ++i)
                work.append(uncheckedDowncast<JSArray>(parents->getIndexQuickly(i).asCell()));
        }
    }
    return false;
}

bool importPromiseGatesAsyncDependency(Instance& instance, JSPromise* importPromise, JSArray* dependency)
{
    VM& vm = *instance.vm;
    auto cellOf = [](JSValue value) -> JSCell* {
        if (value.isEmpty() || !value.isCell())
            return nullptr;
        return value.asCell();
    };
    auto unwrapContext = [&](JSValue value) -> JSCell* {
        JSCell* cell = cellOf(value);
        if (auto* tuple = cell ? dynamicDowncast<InternalFieldTuple>(cell) : nullptr)
            return cellOf(tuple->getInternalField(0));
        return cell;
    };

    UncheckedKeyHashSet<JSPromise*> seen;
    Vector<JSPromise*, 16> work;
    work.append(importPromise);
    bool found = false;

    auto follow = [&](JSValue value) {
        if (JSCell* cell = cellOf(value)) {
            if (auto* promise = dynamicDowncast<JSPromise>(cell))
                work.append(promise);
        }
    };
    auto followPromiseOrDriver = [&](JSCell* cell) {
        if (!cell)
            return;
        if (auto* promise = dynamicDowncast<JSPromise>(cell))
            work.append(promise);
        else if (auto* generator = dynamicDowncast<JSAsyncFunctionGenerator>(cell))
            follow(generator->context());
        else {
            for (JSPromise* promise : Typed::promisesSettledBy(cell))
                work.append(promise);
        }
    };
    auto visitReaction = [&](InternalMicrotask task, JSValue cell, JSValue context) -> bool {
        switch (task) {
        case InternalMicrotask::AOTAsyncFunctionResume:
        case InternalMicrotask::AOTAsyncGeneratorResume:
            for (JSPromise* promise : Typed::promisesSettledBy(unwrapContext(context)))
                work.append(promise);
            break;
        case InternalMicrotask::AsyncFunctionResume:
        case InternalMicrotask::AsyncGeneratorDriverResume:
            followPromiseOrDriver(unwrapContext(context));
            break;
        case InternalMicrotask::AsyncFromSyncIteratorContinue:
        case InternalMicrotask::AsyncFromSyncIteratorDone: {
            JSCell* iteratorCell = unwrapContext(context);
            if (auto* iterator = iteratorCell ? dynamicDowncast<JSAsyncFromSyncIterator>(iteratorCell) : nullptr)
                followPromiseOrDriver(iterator->target());
            break;
        }
        case InternalMicrotask::PromiseFinallyReactionJob:
        case InternalMicrotask::PromiseFinallyAwaitJob: {
            JSCell* contextCell = cellOf(context);
            if (auto* record = contextCell ? dynamicDowncast<JSSlimPromiseReaction>(contextCell) : nullptr)
                follow(record->promise());
            break;
        }
        case InternalMicrotask::PromiseAllResolveJob:
        case InternalMicrotask::PromiseAllSettledResolveJob: {
            JSCell* contextCell = cellOf(cell);
            if (auto* globalContext = contextCell ? dynamicDowncast<JSPromiseCombinatorsGlobalContext>(contextCell) : nullptr)
                follow(globalContext->promise());
            break;
        }
        case InternalMicrotask::None:
        case InternalMicrotask::PromiseResolveThenableJobFast:
        case InternalMicrotask::PromiseResolveThenableJobWithInternalMicrotaskFast:
        case InternalMicrotask::PromiseResolveThenableJob:
        case InternalMicrotask::PromiseResolveThenableJobWithInternalMicrotask:
        case InternalMicrotask::PromiseResolveWithoutHandlerJob:
        case InternalMicrotask::PromiseFulfillWithoutHandlerJob:
        case InternalMicrotask::PromiseReactionJob:
            follow(cell);
            break;
        default:
            break;
        }
        return !found;
    };

    constexpr size_t maxPromises = 4096;
    while (!work.isEmpty() && !found) {
        JSPromise* promise = work.takeLast();
        for (JSArray* module : instance.typed->modulesExecutingAsync) {
            if (Record(vm, module).bodyPromise() == JSValue(promise)) {
                found = resumesDependency(vm, module, dependency);
                break;
            }
        }
        if (found)
            break;
        if (promise->status() != JSPromise::Status::Pending)
            continue;
        if (!seen.add(promise).isNewEntry)
            continue;
        if (seen.size() > maxPromises)
            return false;
        promise->forEachPendingReaction(visitReaction);
    }
    return found;
}

} // anonymous namespace

JSArray* newModuleRecord(Instance& instance, bool hasTopLevelAwait, JSValue execute)
{
    JSGlobalObject* globalObject = instance.globalObject;
    MarkedArgumentBuffer fields;
    fields.ensureCapacity(numberOfFields);
    fields.append(jsNumber(static_cast<int32_t>(Status::Linked)));
    fields.append(jsBoolean(false));
    fields.append(jsUndefined());
    fields.append(jsNumber(0));
    fields.append(jsNumber(0));
    fields.append(jsNumber(0));
    fields.append(jsNumber(orderIsUnset));
    fields.append(jsUndefined());
    fields.append(jsUndefined());
    fields.append(jsUndefined());
    fields.append(jsBoolean(hasTopLevelAwait));
    fields.append(execute);
    fields.append(jsUndefined());
    ASSERT(fields.size() == numberOfFields);
    return constructArray(globalObject, static_cast<ArrayAllocationProfile*>(nullptr), fields);
}

bool enterModule(Instance& instance, JSArray* cell)
{
    VM& vm = *instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    Record module(vm, cell);
    if (module.status() == Status::EvaluatingAsync || module.status() == Status::Evaluated) {
        if (module.hasEvaluationError())
            throwException(instance.globalObject, scope, module.evaluationError());
        return false;
    }
    if (module.status() == Status::Evaluating)
        return false;
    ModuleEvaluation& evaluation = *instance.typed->moduleEvaluation;
    module.setStatus(Status::Evaluating);
    module.setDFSIndex(evaluation.index);
    module.setDFSAncestorIndex(evaluation.index);
    module.setPendingAsyncDependencies(0);
    ++evaluation.index;
    evaluation.stack.append(cell);
    return true;
}

void didEvaluateRequiredModule(Instance& instance, JSArray* cell, JSArray* requiredCell)
{
    VM& vm = *instance.vm;
    JSGlobalObject* globalObject = instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    Record module(vm, cell);
    Record requiredModule(vm, requiredCell);
    ModuleEvaluation& evaluation = *instance.typed->moduleEvaluation;
    if (requiredModule.status() == Status::Evaluating) {
        if (!evaluation.stack.contains(requiredCell))
            return;
        module.setDFSAncestorIndex(std::min(module.dfsAncestorIndex(), requiredModule.dfsAncestorIndex()));
    } else {
        requiredModule = Record(vm, requiredModule.cycleRoot());
        if (requiredModule.hasEvaluationError()) {
            throwException(globalObject, scope, requiredModule.evaluationError());
            return;
        }
    }
    int32_t order = requiredModule.asyncEvaluationOrder();
    if (order < 0 || order == evaluation.referrerAsyncEvaluationOrder)
        return;
    if (evaluation.importPromise && importPromiseGatesAsyncDependency(instance, evaluation.importPromise, requiredModule.cell()))
        return;
    module.setPendingAsyncDependencies(module.pendingAsyncDependencies() + 1);
    JSArray* parents = requiredModule.asyncParentModules();
    if (!parents) {
        parents = constructEmptyArray(globalObject, nullptr);
        RETURN_IF_EXCEPTION(scope, void());
        requiredModule.setAsyncParentModules(parents);
    }
    scope.release();
    parents->putDirectIndex(globalObject, parents->length(), cell);
}

void leaveModule(Instance& instance, JSArray* cell)
{
    VM& vm = *instance.vm;
    JSGlobalObject* globalObject = instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    Record module(vm, cell);
    if (module.pendingAsyncDependencies() > 0 || module.hasTLA()) {
        module.setAsyncEvaluationOrder(instance.typed->moduleAsyncEvaluationCount++);
        if (!module.pendingAsyncDependencies())
            executeModule(globalObject, module);
    } else
        executeModule(globalObject, module);
    RETURN_IF_EXCEPTION(scope, void());
    if (module.dfsAncestorIndex() != module.dfsIndex())
        return;
    ModuleEvaluation& evaluation = *instance.typed->moduleEvaluation;
    for (;;) {
        Record requiredModule(vm, evaluation.stack.takeLast());
        requiredModule.setStatus(requiredModule.asyncEvaluationOrder() == orderIsUnset ? Status::Evaluated : Status::EvaluatingAsync);
        requiredModule.setCycleRoot(cell);
        if (requiredModule == module)
            return;
    }
}

JSArray* newImportTicket(Instance& instance, JSValue referrer)
{
    VM& vm = *instance.vm;
    MarkedArgumentBuffer fields;
    fields.append(jsUndefined());
    fields.append(jsNumber(referrer.isCell() ? std::max(Record(vm, uncheckedDowncast<JSArray>(referrer.asCell())).asyncEvaluationOrder(), -1) : -1));
    return constructArray(instance.globalObject, static_cast<ArrayAllocationProfile*>(nullptr), fields);
}

void setImportPromise(Instance& instance, JSArray* ticket, JSValue promise)
{
    ticket->setIndexQuickly(*instance.vm, 0, promise);
}

void didStartModuleBody(Instance& instance, JSArray* cell, JSValue promise)
{
    Record(*instance.vm, cell).setBodyPromise(promise);
    instance.typed->modulesExecutingAsync.append(cell);
}

JSPromise* evaluateModule(Instance& instance, JSArray* cell, JSValue innerModuleEvaluation, JSValue ticket)
{
    VM& vm = *instance.vm;
    JSGlobalObject* globalObject = instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    Record module(vm, cell);
    if (module.status() == Status::EvaluatingAsync || module.status() == Status::Evaluated) {
        if (JSArray* root = module.cycleRoot())
            module = Record(vm, root);
    }
    if (JSPromise* promise = module.topLevelCapability())
        return promise;
    JSPromise* capability = JSPromise::create(vm, globalObject->promiseStructure());
    capability->markAsHandled();
    module.setTopLevelCapability(capability);

    ModuleEvaluation evaluation;
    if (ticket.isCell()) {
        auto* fields = uncheckedDowncast<JSArray>(ticket.asCell());
        evaluation.importPromise = dynamicDowncast<JSPromise>(fields->getIndexQuickly(0));
        evaluation.referrerAsyncEvaluationOrder = fields->getIndexQuickly(1).asInt32();
    }
    evaluation.outer = std::exchange(instance.typed->moduleEvaluation, &evaluation);
    if (module.status() == Status::Linked)
        call(globalObject, innerModuleEvaluation, JSC::getCallData(innerModuleEvaluation), jsUndefined(), ArgList());
    else if (module.hasEvaluationError())
        throwException(globalObject, scope, module.evaluationError());
    instance.typed->moduleEvaluation = evaluation.outer;

    if (Exception* exception = scope.exception()) {
        JSValue error = exception->value();
        for (JSArray* onStack : evaluation.stack) {
            Record m(vm, onStack);
            m.setStatus(Status::Evaluated);
            m.setEvaluationError(error);
        }
        capability->rejectWithCaughtException(vm, scope);
    } else if (module.status() == Status::Evaluated)
        capability->fulfill(vm, jsUndefined());
    return capability;
}

void asyncModuleExecutionRejected(Instance& instance, JSArray* cell, JSValue error)
{
    VM& vm = *instance.vm;
    Vector<JSArray*, 8> stack;
    stack.append(cell);
    while (!stack.isEmpty()) {
        Record module(vm, stack.takeLast());
        instance.typed->modulesExecutingAsync.removeFirst(module.cell());
        if (module.status() == Status::Evaluated)
            continue;
        module.setEvaluationError(error);
        module.setStatus(Status::Evaluated);
        module.setAsyncEvaluationOrder(orderIsDone);
        if (JSPromise* capability = module.topLevelCapability())
            capability->reject(vm, error);
        if (JSArray* parents = module.asyncParentModules()) {
            for (unsigned i = parents->length(); i--;)
                stack.append(uncheckedDowncast<JSArray>(parents->getIndexQuickly(i).asCell()));
        }
    }
}

void asyncModuleExecutionFulfilled(Instance& instance, JSArray* cell)
{
    VM& vm = *instance.vm;
    JSGlobalObject* globalObject = instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    Record module(vm, cell);
    instance.typed->modulesExecutingAsync.removeFirst(cell);
    if (module.status() == Status::Evaluated)
        return;
    module.setAsyncEvaluationOrder(orderIsDone);
    module.setStatus(Status::Evaluated);
    if (JSPromise* capability = module.topLevelCapability())
        capability->fulfill(vm, jsUndefined());
    Vector<JSArray*, 8> execList;
    gatherAvailableAncestors(vm, cell, execList);
    std::ranges::sort(execList, [&](JSArray* left, JSArray* right) {
        return Record(vm, left).asyncEvaluationOrder() < Record(vm, right).asyncEvaluationOrder();
    });
    for (JSArray* next : execList) {
        Record m(vm, next);
        if (m.status() == Status::Evaluated)
            continue;
        executeModule(globalObject, m);
        if (Exception* exception = scope.exception()) {
            JSValue error = exception->value();
            TRY_CLEAR_EXCEPTION(scope, void());
            asyncModuleExecutionRejected(instance, next, error);
            continue;
        }
        if (m.hasTLA())
            continue;
        m.setAsyncEvaluationOrder(orderIsDone);
        m.setStatus(Status::Evaluated);
        if (JSPromise* capability = m.topLevelCapability())
            capability->fulfill(vm, jsUndefined());
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
