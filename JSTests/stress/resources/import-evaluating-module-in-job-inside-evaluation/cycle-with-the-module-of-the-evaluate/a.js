import "./root.js";
log.push("a1");
track("a", import("./a.js"));
track("inner", import("./root.js"));
drainMicrotasks();
log.push(`a2(a=${states.a},inner=${states.inner})`);
