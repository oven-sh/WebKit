import * as target from "./throws-target.js";
import { log } from "./log.js";

tellWhenModuleBodyEnds(target, () => log.push("told of a body that threw"));
