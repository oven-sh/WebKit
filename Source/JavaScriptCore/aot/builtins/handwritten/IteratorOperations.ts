$linkTimeConstant;
$alwaysInline;
export function iteratorGenericNext(nextMethod, iterator) {
    "use strict";

    var result = nextMethod.$call(iterator);
    if (!$isObject(result))
        $throwTypeError("Iterator result interface is not an object.");
    return result;
}

$linkTimeConstant;
$alwaysInline;
export function iteratorGenericClose(iterator) {
    "use strict";

    var returnMethod = iterator.return;
    if ($isUndefinedOrNull(returnMethod))
        return;
    if (!$isObject(returnMethod.$call(iterator)))
        $throwTypeError("Iterator result interface is not an object.");
}
