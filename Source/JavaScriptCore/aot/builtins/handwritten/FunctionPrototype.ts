export function call(thisArgument) {
    "use strict";

    switch ($argumentCount()) {
    case 0:
    case 1:
        return this.$call(thisArgument);
    case 2:
        return this.$call(thisArgument, $argument(1));
    case 3:
        return this.$call(thisArgument, $argument(1), $argument(2));
    case 4:
        return this.$call(thisArgument, $argument(1), $argument(2), $argument(3));
    case 5:
        return this.$call(thisArgument, $argument(1), $argument(2), $argument(3), $argument(4));
    }

    const argumentValues = [];
    for (let i = 1; i < $argumentCount(); i++)
        $putByValDirect(argumentValues, i - 1, arguments[i]);

    return this.$apply(thisArgument, argumentValues);
}

export function apply(thisValue, argumentValues) {
    "use strict";

    return this.$apply(thisValue, argumentValues);
}
