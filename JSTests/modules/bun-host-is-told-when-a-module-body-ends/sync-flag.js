import * as target from "./sync-target.js";
import { log } from "./log.js";

// The body of sync-target.js has not started: it imports this module.
log.push("sync flagged: " + tellWhenModuleBodyEnds(target, () => log.push("told of " + target.value)));
