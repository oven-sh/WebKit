import "./late-throw-a.js";
log.push("suspended: start");
await 0;
log.push("suspended: after the await");
throw new Error("late-throw-suspended.js");
