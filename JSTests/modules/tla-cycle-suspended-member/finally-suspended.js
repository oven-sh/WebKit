import "./finally-a.js";
log.push("suspended: start");
try {
    await 0;
    log.push("suspended: after the await");
} finally {
    log.push("suspended: finally");
}
log.push("suspended: end");
