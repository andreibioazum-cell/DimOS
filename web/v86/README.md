# v86 browser runtime

Vendored files used by the root `index.html` launcher:

- `libv86.js` and `v86.wasm`: npm package `v86@0.5.424`;
- `seabios.bin` and `vgabios.bin`: `copy/v86` commit `f3d4472a9c934b9ad78a311f5849ba711a296d23`;
- `LICENSE`: v86 BSD 2-Clause license.

The runtime is stored in the repository so a hosted launcher does not depend on a third-party CDN. When `index.html` is opened directly through the `file:` protocol, the page uses CDN URLs for WASM and BIOS because browsers normally block local XHR/fetch requests. Serving the repository over HTTP is recommended.
