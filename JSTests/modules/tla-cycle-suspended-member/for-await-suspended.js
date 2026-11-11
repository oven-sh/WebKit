import "./for-await-a.js";
async function* values() {
    yield 1;
    yield 2;
}
log.push("suspended: start");
try {
    for await (const value of values())
        log.push(`suspended: ${value}`);
} finally {
    log.push("suspended: finally");
}
