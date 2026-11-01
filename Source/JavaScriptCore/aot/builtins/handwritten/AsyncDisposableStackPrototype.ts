export function adopt(value, onAsyncDispose)
{
    "use strict";

    if (!$isAsyncDisposableStack(this))
        $throwTypeError("AsyncDisposableStack.prototype.adopt requires that |this| be a AsyncDisposableStack object");

    if ($getAsyncDisposableStackInternalField(this, $asyncDisposableStackFieldState) === $AsyncDisposableStackStateDisposed)
        throw new $ReferenceError("AsyncDisposableStack.prototype.adopt requires that |this| be a pending AsyncDisposableStack object");

    if (!$isCallable(onAsyncDispose))
        $throwTypeError("AsyncDisposableStack.prototype.adopt requires that onAsyncDispose argument be a callable");

    var closure = () => onAsyncDispose.$call(undefined, value);
    $addDisposableResource($getAsyncDisposableStackInternalField(this, $asyncDisposableStackFieldCapability), undefined, /* isAsync */ true, closure);

    return value;
}

$overriddenName = "defer";
export function deferMethod(onAsyncDispose)
{
    "use strict";

    if (!$isAsyncDisposableStack(this))
        $throwTypeError("AsyncDisposableStack.prototype.defer requires that |this| be a AsyncDisposableStack object");

    if ($getAsyncDisposableStackInternalField(this, $asyncDisposableStackFieldState) === $AsyncDisposableStackStateDisposed)
        throw new $ReferenceError("AsyncDisposableStack.prototype.defer requires that |this| be a pending AsyncDisposableStack object");

    if (!$isCallable(onAsyncDispose))
        $throwTypeError("AsyncDisposableStack.prototype.defer requires that onAsyncDispose argument be a callable");

    $addDisposableResource($getAsyncDisposableStackInternalField(this, $asyncDisposableStackFieldCapability), undefined, /* isAsync */ true, onAsyncDispose);

    return undefined;
}

export function disposeAsync()
{
    "use strict";

    var promise = $newPromise();

    if (!$isAsyncDisposableStack(this)) {
        $rejectPromiseWithFirstResolvingFunctionCallCheck(promise, $makeTypeError("AsyncDisposableStack.prototype.disposeAsync requires that |this| be a AsyncDisposableStack object"));
        return promise;
    }

    if ($getAsyncDisposableStackInternalField(this, $asyncDisposableStackFieldState) === $AsyncDisposableStackStateDisposed) {
        $resolvePromiseWithFirstResolvingFunctionCallCheck(promise, undefined);
        return promise;
    }

    $putAsyncDisposableStackInternalField(this, $asyncDisposableStackFieldState, $AsyncDisposableStackStateDisposed);

    var stack = $getAsyncDisposableStackInternalField(this, $asyncDisposableStackFieldCapability);
    $assert($isArray(stack));

    var i = $toLength(stack.length);
    var thrown = false;
    var suppressed;
    var needsAwait = false;
    var hasAwaited = false;

    var handleError = (result) => {
        if (thrown)
            suppressed = new $SuppressedError(result, suppressed);
        else {
            thrown = true;
            suppressed = result;
        }
        loop();
    };

    var finish = () => {
        if (thrown)
            $rejectPromiseWithFirstResolvingFunctionCallCheck(promise, suppressed);
        else
            $resolvePromiseWithFirstResolvingFunctionCallCheck(promise, undefined);
    };

    var loop = () => {
        while (i) {
            var resource = stack[$toLength(--i)];
            if (resource.method === undefined) {
                needsAwait = true;
                continue;
            }
            var result;
            try {
                result = resource.method.$call(resource.value);
            } catch (error) {
                if (thrown)
                    suppressed = new $SuppressedError(error, suppressed);
                else {
                    thrown = true;
                    suppressed = error;
                }
                continue;
            }
            hasAwaited = true;
            $promiseResolve($Promise, result).$then(loop, handleError);
            return;
        }
        if (needsAwait && !hasAwaited)
            $promiseResolve($Promise, undefined).$then(finish, finish);
        else
            finish();
    };

    loop();

    $putAsyncDisposableStackInternalField(this, $asyncDisposableStackFieldCapability, []);

    return promise;
}

export function move()
{
    "use strict";

    if (!$isAsyncDisposableStack(this))
        $throwTypeError("AsyncDisposableStack.prototype.move requires that |this| be a AsyncDisposableStack object");

    if ($getAsyncDisposableStackInternalField(this, $asyncDisposableStackFieldState) === $AsyncDisposableStackStateDisposed)
        throw new $ReferenceError("AsyncDisposableStack.prototype.move requires that |this| be a pending AsyncDisposableStack object");

    var newAsyncDisposableStack = new $AsyncDisposableStack();
    $putAsyncDisposableStackInternalField(newAsyncDisposableStack, $asyncDisposableStackFieldCapability, $getAsyncDisposableStackInternalField(this, $asyncDisposableStackFieldCapability));

    $putAsyncDisposableStackInternalField(this, $asyncDisposableStackFieldCapability, []);
    $putAsyncDisposableStackInternalField(this, $asyncDisposableStackFieldState, $AsyncDisposableStackStateDisposed);

    return newAsyncDisposableStack;
}

export function use(value)
{
    "use strict";

    if (!$isAsyncDisposableStack(this))
        $throwTypeError("AsyncDisposableStack.prototype.use requires that |this| be a AsyncDisposableStack object");

    if ($getAsyncDisposableStackInternalField(this, $asyncDisposableStackFieldState) === $AsyncDisposableStackStateDisposed)
        throw new $ReferenceError("AsyncDisposableStack.prototype.use requires that |this| be a pending AsyncDisposableStack object");

    $addDisposableResource($getAsyncDisposableStackInternalField(this, $asyncDisposableStackFieldCapability), value, /* isAsync */ true);

    return value;
}
