export async function load(specifier) {
    return await import(specifier);
}

export async function loadAfterAwait(specifier) {
    await Promise.resolve();
    await null;
    return await import(specifier);
}

export async function loadAll(specifier) {
    return await Promise.all([import(specifier), Promise.resolve(1)]);
}
