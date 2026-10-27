$linkTimeConstant;
export function createDisposableResource(value, isAsync /* , method */) {
    "use strict";

    var method;
    if ($argumentCount() < 3) {
        if ($isUndefinedOrNull(value))
            value = undefined;
        else {
            if (!$isObject(value))
                $throwTypeError("Disposable value must be an object");
            if (isAsync)
                method = $getAsyncDisposeMethod(value);
            else
                method = $getDisposeMethod(value);
        }
    } else {
        method = $argument(2);
        if (!$isCallable(method))
            $throwTypeError("Callback that is called on dispose must be callable");
    }

    return { value, method };
}

$linkTimeConstant;
export function getDisposeMethod(value) {
    "use strict";

    if ($isUndefinedOrNull(value))
        return undefined;
    if (!$isObject(value))
        $throwTypeError("Disposable value must be an object, null, or undefined");
    var method = value.$$dispose;
    if ($isUndefinedOrNull(method))
        $throwTypeError("@@dispose must not be undefined or null");
    if (!$isCallable(method))
        $throwTypeError("@@dispose must be callable");
    return method;
}

$linkTimeConstant;
export function getAsyncDisposeMethod(value) {
    "use strict";

    if ($isUndefinedOrNull(value))
        return undefined;
    if (!$isObject(value))
        $throwTypeError("Disposable value must be an object, null, or undefined");

    var method = value.$$asyncDispose;
    if ($isUndefinedOrNull(method)) {
        method = value.$$dispose;
        if ($isUndefinedOrNull(method))
            $throwTypeError("@@asyncDispose and @@dispose must not be undefined or null");
        if (!$isCallable(method))
            $throwTypeError("@@dispose must be callable");
        var syncMethod = method;
        var receiver = value;
        return function () {
            try {
                syncMethod.$call(receiver);
            } catch (e) {
                return $promiseReject($Promise, e);
            }
            return $promiseResolve($Promise, undefined);
        };
    }
    if (!$isCallable(method))
        $throwTypeError("@@asyncDispose must be callable");
    return method;
}

$linkTimeConstant;
export function addDisposableResource(disposeCapability, value, isAsync /* , method */) {
    "use strict";

    $assert($isArray(disposeCapability));

    var resource;
    if ($argumentCount() < 4) {
        if ($isUndefinedOrNull(value) && !isAsync)
            return;
        resource = $createDisposableResource(value, isAsync);
    } else {
        $assert(value === undefined);
        resource = $createDisposableResource(undefined, isAsync, $argument(3));
    }

    $putByValDirect(disposeCapability, $toLength(disposeCapability.length), resource);
}

export function adopt(value, onDispose) {
    "use strict";

    if (!$isDisposableStack(this))
        $throwTypeError("DisposableStack.prototype.adopt requires that |this| be a DisposableStack object");

    if ($getDisposableStackInternalField(this, $disposableStackFieldState) === $DisposableStackStateDisposed)
        throw new $ReferenceError("DisposableStack.prototype.adopt requires that |this| be a pending DisposableStack object");

    if (!$isCallable(onDispose))
        $throwTypeError("DisposableStack.prototype.adopt requires that onDispose argument be a callable");

    var closure = () => { onDispose.$call(undefined, value); };
    $addDisposableResource($getDisposableStackInternalField(this, $disposableStackFieldCapability), undefined, /* isAsync */ false, closure);

    return value;
}

$overriddenName = "defer";
export function deferMethod(onDispose) {
    "use strict";

    if (!$isDisposableStack(this))
        $throwTypeError("DisposableStack.prototype.defer requires that |this| be a DisposableStack object");

    if ($getDisposableStackInternalField(this, $disposableStackFieldState) === $DisposableStackStateDisposed)
        throw new $ReferenceError("DisposableStack.prototype.defer requires that |this| be a pending DisposableStack object");

    if (!$isCallable(onDispose))
        $throwTypeError("DisposableStack.prototype.defer requires that onDispose argument be a callable");

    $addDisposableResource($getDisposableStackInternalField(this, $disposableStackFieldCapability), undefined, /* isAsync */ false, onDispose);

    return undefined;
}

export function dispose() {
    "use strict";

    if (!$isDisposableStack(this))
        $throwTypeError("DisposableStack.prototype.dispose requires that |this| be a DisposableStack object");

    if ($getDisposableStackInternalField(this, $disposableStackFieldState) === $DisposableStackStateDisposed)
        return undefined;

    $putDisposableStackInternalField(this, $disposableStackFieldState, $DisposableStackStateDisposed);

    var stack = $getDisposableStackInternalField(this, $disposableStackFieldCapability);
    $assert($isArray(stack));

    var i = $toLength(stack.length);
    var thrown = false;
    var suppressed;

    while (i) {
        var resource = stack[--i];
        try {
            resource.method.$call(resource.value);
        } catch (e) {
            if (thrown)
                suppressed = new $SuppressedError(e, suppressed);
            else {
                thrown = true;
                suppressed = e;
            }
        }
    }

    $putDisposableStackInternalField(this, $disposableStackFieldCapability, []);
    if (thrown)
        throw suppressed;
}

export function move() {
    "use strict";

    if (!$isDisposableStack(this))
        $throwTypeError("DisposableStack.prototype.move requires that |this| be a DisposableStack object");

    if ($getDisposableStackInternalField(this, $disposableStackFieldState) === $DisposableStackStateDisposed)
        throw new $ReferenceError("DisposableStack.prototype.move requires that |this| be a pending DisposableStack object");

    var newDisposableStack = new $DisposableStack();
    $putDisposableStackInternalField(newDisposableStack, $disposableStackFieldCapability, $getDisposableStackInternalField(this, $disposableStackFieldCapability));

    $putDisposableStackInternalField(this, $disposableStackFieldCapability, []);
    $putDisposableStackInternalField(this, $disposableStackFieldState, $DisposableStackStateDisposed);

    return newDisposableStack;
}

export function use(value) {
    "use strict";

    if (!$isDisposableStack(this))
        $throwTypeError("DisposableStack.prototype.use requires that |this| be a DisposableStack object");

    if ($getDisposableStackInternalField(this, $disposableStackFieldState) === $DisposableStackStateDisposed)
        throw new $ReferenceError("DisposableStack.prototype.use requires that |this| be a pending DisposableStack object");

    $addDisposableResource($getDisposableStackInternalField(this, $disposableStackFieldCapability), value, /* isAsync */ false);

    return value;
}
