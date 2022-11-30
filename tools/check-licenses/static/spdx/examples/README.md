check-licenses.spdx.json is generated with the following commands:

```
# Build check-licenses in a clean workspace
fx set core.x64 --with //tools/check-licenses:host
fx clean && fx build

# Run check-licenses against itself
fx check-licenses //tools/check-licenses

# Copy the resulting SPDX.json file to this directory.
# By default, check-licenses outputs to /tmp/ directory.
cp /tmp/check-licenses/version/core.x64/results.spdx.json check-licenses.spdx.json
```
