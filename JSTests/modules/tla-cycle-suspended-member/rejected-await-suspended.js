import "./rejected-await-a.js";
log.push("suspended: start");
try {
    await Promise.reject(new Error("rejected"));
} catch (error) {
    log.push(`suspended: caught ${error.message}`);
}
log.push("suspended: end");
