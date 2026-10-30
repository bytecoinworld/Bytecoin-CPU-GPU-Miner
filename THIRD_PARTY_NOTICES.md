# Third-party notices

BCN Miner includes third-party source code. The upstream notices and complete
license texts are kept with the corresponding source directories; do not
remove them when redistributing source or binaries.

| Component | Source | License |
|---|---|---|
| Bytecoin CryptoNight implementation | [`third_party/bytecoin`](third_party/bytecoin/README.md), upstream commit `c3d9b94e0d34ee486ff35e7c781f673c1097b955` | LGPL-3.0; see `third_party/bytecoin/LICENSE` and individual source notices |
| XMRig AMD-derived OpenCL kernels | [`third_party/xmrig-amd`](third_party/xmrig-amd/README.md), upstream commit `529a29110881da598d66e9e0bb5f1002bc796e5c` | GPL-3.0-or-later; see `third_party/xmrig-amd/LICENSE` and kernel notices |

The overall BCN Miner project is licensed under GPL-3.0-only. This does not
replace or remove the separate terms applying to included third-party
components. Other embedded files may contain additional license terms; retain
their notices.

The Bytecoin cryptographic library is statically linked into the executable.
Before distributing a binary, comply with LGPL-3.0 requirements applicable to
that combined work, including the requirements concerning relinking with a
modified version of the library, or use another compliant distribution
arrangement. Consult the license texts and obtain qualified legal advice if
needed.
