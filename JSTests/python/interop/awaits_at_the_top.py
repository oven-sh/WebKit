# For a-module-can-await.py and a-module-can-await.mjs to import.
import js

print("    the module begins")
first = await js.Promise.resolve("first")
print("    it has awaited once")


async def twice(x):
    return await js.Promise.resolve(x * 2)


second = await twice(21)
print("    it is done")
