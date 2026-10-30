# XMRig OpenCL CryptoNight kernels

The OpenCL kernels in `opencl` are derived from
[`xmrig/xmrig-amd`](https://github.com/xmrig/xmrig-amd), upstream commit
`529a29110881da598d66e9e0bb5f1002bc796e5c`.

The kernels are licensed under GPL-3.0-or-later. The complete license is in
[LICENSE](LICENSE), and source copyright/license notices are preserved in the
kernel files. The `.cl` files use the upstream C++ raw-string concatenation
format; BCN Miner removes only those string-literal delimiters when assembling
the OpenCL program at runtime.

BCN Miner currently uses the original CryptoNight (variant 0) OpenCL path.
GPU devices must compile the kernels and pass a startup target/hash check
against BCN Miner's Bytecoin CPU implementation before they are enabled.
Every candidate nonce is then re-hashed by the CPU before submission.
The output-candidate limit is adapted from the upstream fixed 255-candidate
buffer to a bounded 1024-candidate buffer so larger GPU batches remain safe.

Distributors must comply with GPL-3.0 and the licenses for all bundled
components. BCN Miner also includes the LGPL-3.0 Bytecoin CPU hash library;
see [the Bytecoin notices](../bytecoin/README.md).
