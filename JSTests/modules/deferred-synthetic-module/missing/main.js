import "./setup.js";
import { nope } from "./lib.deferred.js";
globalThis.missingLog.push("main:" + nope);
