import * as target from "./tla-target.js";
import { log } from "./log.js";

log.push("tla flagged: " + tellWhenModuleBodyEnds(target, () => log.push("told of " + target.value)));
