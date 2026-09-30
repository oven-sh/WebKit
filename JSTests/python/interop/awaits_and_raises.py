# For a-module-can-await.py to import.
import js

await js.Promise.resolve(1)
raise KeyError("from a module that awaits")
