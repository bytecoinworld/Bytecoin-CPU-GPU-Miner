# Bytecoin cryptographic source

The files in `src/crypto` and `tests/hash/tests-slow.txt` are from
[`bcndev/bytecoin`](https://github.com/bcndev/bytecoin), commit
`c3d9b94e0d34ee486ff35e7c781f673c1097b955`.

The Bytecoin source is licensed under LGPL-3.0; see [LICENSE](LICENSE) and
the notices in the individual source files. The OpenAES and AES helper files
also contain their own license terms and notices, which are preserved.

The vendored source files have not been modified. BCN Miner links the selected
Bytecoin CryptoNight files into `bytecoin_cryptonight`; distributors must meet
the LGPL requirements, including allowing users to replace or relink the
library as required by that license.
