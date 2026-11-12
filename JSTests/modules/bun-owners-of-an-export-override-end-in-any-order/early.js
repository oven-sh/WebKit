import * as late from "./late.js";
import { log } from "./log.js";

// The body of late.js has yet to run: its binding has no value.
const owner = {};
log.push(layExportOverride(late, "value", "laid", owner));
log.push(JSON.stringify(exportFoundByOverride(late, "value", owner)));
log.push(late.value);
log.push(endExportOverride(late, "value", owner));
log.push(late.value);
