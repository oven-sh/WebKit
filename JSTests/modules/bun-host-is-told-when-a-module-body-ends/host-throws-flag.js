import * as target from "./host-throws-target.js";

tellWhenModuleBodyEnds(target, () => { throw new Error("thrown by the host"); });
