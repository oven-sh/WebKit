import "./awaits-a.js";
log.push("suspended: start");
for (let i = 1; i <= 3; ++i) {
    await 0;
    log.push(`suspended: ${i}`);
}
