for (let i = 0; i < 2_000; i++) {
    let a = new Array(i);
    a.fill(i);
    Object.seal(a);
}
for (let i = 0; i < 2_000; i++) {
    let a = new Array(i);
    a.fill(i);
    Object.freeze(a);
}
