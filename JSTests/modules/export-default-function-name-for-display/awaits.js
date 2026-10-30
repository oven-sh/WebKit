async function thrower() {
    await null;
    throw new Error();
}

export default async function () {
    await thrower();
}
