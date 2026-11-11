export function call(thisArgument) {
    "use strict";

    // What the place of the call names is this function. Its frame stays, so that the one it calls finds no name there.
    if (this === $functionPrototypeCall || this === $functionPrototypeApply) {
        const forwardedValues = [];
        for (let i = 1; i < $argumentCount(); i++)
            $putByValDirect(forwardedValues, i - 1, arguments[i]);
        const result = this.$apply(thisArgument, forwardedValues);
        return result;
    }

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

    if (this === $functionPrototypeCall || this === $functionPrototypeApply) {
        const result = this.$apply(thisValue, argumentValues);
        return result;
    }

    return this.$apply(thisValue, argumentValues);
}
