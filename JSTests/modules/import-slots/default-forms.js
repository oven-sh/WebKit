import anonymousFunction from "./default-anonymous-function.js";
import anonymousClass from "./default-anonymous-class.js";
import expression from "./default-expression.js";
import named, { named as alsoNamed, rename } from "./default-named-function.js";
import { default as viaName } from "./default-expression.js";
export function read() { return [anonymousFunction.name, anonymousFunction(), anonymousClass.name, expression, named(), named === alsoNamed, viaName].join(); }
export { rename, named };
