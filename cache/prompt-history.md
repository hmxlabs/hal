2026-01-29T09:21
Model: Clause Opus 4.5
Context: ../README.md
Prompt: 
User: In the cache directory create a readme explaining that this is the caching layer for the HPC scheduler as references in the README.md file.

Explain that this cache will:

- Allows users to use the Redis API to interact with it
- Users will always interact with their most local instance of the cache
- If the most local instance of the cache does not have the data it will attempt to retrieve it from the most proximate (by netword latance, speed, cost) location that has that data, store it locally and then pass it to the client
- The cache instances therefore form tree topology with the root of the tree containing all possible data and the subsequent branches containing data that has been requested
- Each instance of the cache will also further expose an additional API that lists all of the data it holds and the size of the data against each key
- Every time a cache instance changes its contents, it will send a message to a cache control plane with details of any keys that are evicted from the cache, any keys that were added to the cache, their size, which cache instance the data was sourced from and how long it took to obtain that data
- The cache control plane will maintain all of this data and will therefore be aware of exactly which data is available in each cache instance. It will also know which instance of the cache is most proximate to any other instance.
- The cache code itself will essentially be a copy (forked) from ValKey with the additional features to capture key data and notify the control plane added to it.

Write a README doc that explains all of this in the cache directory

Draw a diagram of the cache topology.
