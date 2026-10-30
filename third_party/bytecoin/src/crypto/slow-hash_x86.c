// Copyright (c) 2012-2018, The CryptoNote developers, The Bytecoin developers.
// Licensed under the GNU Lesser General Public License. See LICENSE for details.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __APPLE__
#include "TargetConditionals.h"
#endif

// We need if x86, but no portable way to express that
#if !defined(__EMSCRIPTEN__) && !defined(__PPC__) && !TARGET_OS_IPHONE && !defined(__ANDROID__)

#include <emmintrin.h>
#include <wmmintrin.h>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

#include "hash.h"
#include "initializer.h"
#include "int-util.h"
#include "oaes/aesb.h"
#include "oaes/oaes_lib.h"

#if defined(__GNUC__)
#define likely(x) (__builtin_expect(!!(x), 1))
#define unlikely(x) (__builtin_expect(!!(x), 0))
#else
#define likely(x) (x)
#define unlikely(x) (x)
#define __attribute__(x)
#endif

#if defined(_MSC_VER)
#define restrict
#endif

#define MEMORY (1 << 21) /* 2 MiB */
#define ITER (1 << 20)
#define AES_BLOCK_SIZE 16
#define AES_KEY_SIZE 32 /*16*/
#define INIT_SIZE_BLK 8
#define INIT_SIZE_BYTE (INIT_SIZE_BLK * AES_BLOCK_SIZE)  // 128

#pragma pack(push, 1)
union cn_slow_hash_state {
	struct cryptoKeccakState hs;
	struct {
		uint8_t k[64];
		uint8_t init[INIT_SIZE_BYTE];
	};
};
#pragma pack(pop)

#if defined(_MSC_VER)
#define ALIGNED_DATA(x) __declspec(align(x))
#define ALIGNED_DECL(t, x) ALIGNED_DATA(x) t
#elif defined(__GNUC__)
#define ALIGNED_DATA(x) __attribute__((aligned(x)))
#define ALIGNED_DECL(t, x) t ALIGNED_DATA(x)
#endif

struct cn_ctx {
	ALIGNED_DECL(uint8_t long_state[MEMORY], 16);
	ALIGNED_DECL(union cn_slow_hash_state state, 16);
	ALIGNED_DECL(uint8_t text[INIT_SIZE_BYTE], 16);
	ALIGNED_DECL(uint64_t a[AES_BLOCK_SIZE >> 3], 16);
	ALIGNED_DECL(uint64_t b[AES_BLOCK_SIZE >> 3], 16);
	ALIGNED_DECL(uint8_t c[AES_BLOCK_SIZE], 16);
	oaes_ctx *aes_ctx;
};

static_assert(sizeof(struct cn_ctx) == SLOW_HASH_CONTEXT_SIZE, "Invalid structure size");

static void ExpandAESKey256_sub1(__m128i *tmp1, __m128i *tmp2) {
	__m128i tmp4;
	*tmp2 = _mm_shuffle_epi32(*tmp2, 0xFF);
	tmp4  = _mm_slli_si128(*tmp1, 0x04);
	*tmp1 = _mm_xor_si128(*tmp1, tmp4);
	tmp4  = _mm_slli_si128(tmp4, 0x04);
	*tmp1 = _mm_xor_si128(*tmp1, tmp4);
	tmp4  = _mm_slli_si128(tmp4, 0x04);
	*tmp1 = _mm_xor_si128(*tmp1, tmp4);
	*tmp1 = _mm_xor_si128(*tmp1, *tmp2);
}

static void ExpandAESKey256_sub2(__m128i *tmp1, __m128i *tmp3) {
	__m128i tmp2, tmp4;

	tmp4  = _mm_aeskeygenassist_si128(*tmp1, 0x00);
	tmp2  = _mm_shuffle_epi32(tmp4, 0xAA);
	tmp4  = _mm_slli_si128(*tmp3, 0x04);
	*tmp3 = _mm_xor_si128(*tmp3, tmp4);
	tmp4  = _mm_slli_si128(tmp4, 0x04);
	*tmp3 = _mm_xor_si128(*tmp3, tmp4);
	tmp4  = _mm_slli_si128(tmp4, 0x04);
	*tmp3 = _mm_xor_si128(*tmp3, tmp4);
	*tmp3 = _mm_xor_si128(*tmp3, tmp2);
}

// Special thanks to Intel for helping me
// with ExpandAESKey256() and its subroutines
static void ExpandAESKey256(uint8_t *keybuf) {
	__m128i tmp1, tmp2, tmp3, *keys;

	keys = (__m128i *)keybuf;

	tmp1 = _mm_load_si128((__m128i *)keybuf);
	tmp3 = _mm_load_si128((__m128i *)(keybuf + 0x10));

	tmp2 = _mm_aeskeygenassist_si128(tmp3, 0x01);
	ExpandAESKey256_sub1(&tmp1, &tmp2);
	keys[2] = tmp1;
	ExpandAESKey256_sub2(&tmp1, &tmp3);
	keys[3] = tmp3;

	tmp2 = _mm_aeskeygenassist_si128(tmp3, 0x02);
	ExpandAESKey256_sub1(&tmp1, &tmp2);
	keys[4] = tmp1;
	ExpandAESKey256_sub2(&tmp1, &tmp3);
	keys[5] = tmp3;

	tmp2 = _mm_aeskeygenassist_si128(tmp3, 0x04);
	ExpandAESKey256_sub1(&tmp1, &tmp2);
	keys[6] = tmp1;
	ExpandAESKey256_sub2(&tmp1, &tmp3);
	keys[7] = tmp3;

	tmp2 = _mm_aeskeygenassist_si128(tmp3, 0x08);
	ExpandAESKey256_sub1(&tmp1, &tmp2);
	keys[8] = tmp1;
	ExpandAESKey256_sub2(&tmp1, &tmp3);
	keys[9] = tmp3;

	tmp2 = _mm_aeskeygenassist_si128(tmp3, 0x10);
	ExpandAESKey256_sub1(&tmp1, &tmp2);
	keys[10] = tmp1;
	ExpandAESKey256_sub2(&tmp1, &tmp3);
	keys[11] = tmp3;

	tmp2 = _mm_aeskeygenassist_si128(tmp3, 0x20);
	ExpandAESKey256_sub1(&tmp1, &tmp2);
	keys[12] = tmp1;
	ExpandAESKey256_sub2(&tmp1, &tmp3);
	keys[13] = tmp3;

	tmp2 = _mm_aeskeygenassist_si128(tmp3, 0x40);
	ExpandAESKey256_sub1(&tmp1, &tmp2);
	keys[14] = tmp1;
}

static void (*const extra_hashes[4])(const void *, size_t, struct cryptoHash *) = {
    crypto_hash_extra_blake, crypto_hash_extra_groestl, crypto_hash_extra_jh, crypto_hash_extra_skein};

#include "slow-hash_x86.inl"
#define AESNI
#include "slow-hash_x86.inl"

static int cpu_has_aesni(void) {
	int ecx;
#if defined(_MSC_VER)
	int cpuinfo[4];
	__cpuid(cpuinfo, 1);
	ecx = cpuinfo[2];
#else
	int a, b, d;
	__cpuid(1, a, b, ecx, d);
#endif
	return (ecx & (1 << 25)) ? 1 : 0;
}

static void cn_slow_hash_aesni_double(
	void *context0,
	const void *data0,
	size_t length0,
	struct cryptoHash *hash0,
	void *context1,
	const void *data1,
	size_t length1,
	struct cryptoHash *hash1) {
	struct cn_ctx *ctx0 = (struct cn_ctx *)context0;
	struct cn_ctx *ctx1 = (struct cn_ctx *)context1;
	ALIGNED_DECL(uint8_t expanded_key0[256], 16);
	ALIGNED_DECL(uint8_t expanded_key1[256], 16);
	ALIGNED_DECL(uint64_t a0[2], 16);
	ALIGNED_DECL(uint64_t a1[2], 16);
	ALIGNED_DECL(uint64_t c0[2], 16);
	ALIGNED_DECL(uint64_t c1[2], 16);
	ALIGNED_DECL(uint64_t b0[2], 16);
	ALIGNED_DECL(uint64_t b1[2], 16);
	__m128i *longoutput0 = (__m128i *)ctx0->long_state;
	__m128i *longoutput1 = (__m128i *)ctx1->long_state;
	__m128i *expkey0;
	__m128i *expkey1;
	__m128i *xmminput0 = (__m128i *)ctx0->text;
	__m128i *xmminput1 = (__m128i *)ctx1->text;
	__m128i bx0;
	__m128i bx1;
	size_t i;
	size_t j;
	size_t block;

	crypto_keccak_into_state((const uint8_t *)data0, length0, &ctx0->state.hs);
	crypto_keccak_into_state((const uint8_t *)data1, length1, &ctx1->state.hs);
	memcpy(ctx0->text, ctx0->state.init, INIT_SIZE_BYTE);
	memcpy(ctx1->text, ctx1->state.init, INIT_SIZE_BYTE);
	memcpy(expanded_key0, ctx0->state.hs.b, AES_KEY_SIZE);
	memcpy(expanded_key1, ctx1->state.hs.b, AES_KEY_SIZE);
	ExpandAESKey256(expanded_key0);
	ExpandAESKey256(expanded_key1);
	expkey0 = (__m128i *)expanded_key0;
	expkey1 = (__m128i *)expanded_key1;

	for (i = 0; likely(i < MEMORY); i += INIT_SIZE_BYTE) {
		for (j = 0; j < 10; j++) {
			for (block = 0; block < 8; block++) {
				xmminput0[block] = _mm_aesenc_si128(
					xmminput0[block], expkey0[j]);
				xmminput1[block] = _mm_aesenc_si128(
					xmminput1[block], expkey1[j]);
			}
		}
		for (block = 0; block < 8; block++) {
			_mm_store_si128(&longoutput0[(i >> 4) + block], xmminput0[block]);
			_mm_store_si128(&longoutput1[(i >> 4) + block], xmminput1[block]);
		}
	}

	for (i = 0; i < 2; i++) {
		a0[i] = ((uint64_t *)ctx0->state.k)[i] ^
			((uint64_t *)ctx0->state.k)[i + 4];
		a1[i] = ((uint64_t *)ctx1->state.k)[i] ^
			((uint64_t *)ctx1->state.k)[i + 4];
		b0[i] = ((uint64_t *)ctx0->state.k)[i + 2] ^
			((uint64_t *)ctx0->state.k)[i + 6];
		b1[i] = ((uint64_t *)ctx1->state.k)[i + 2] ^
			((uint64_t *)ctx1->state.k)[i + 6];
	}
	bx0 = _mm_load_si128((__m128i *)b0);
	bx1 = _mm_load_si128((__m128i *)b1);

	for (i = 0; likely(i < 0x80000); i++) {
		__m128i cx0 = _mm_load_si128(
			(__m128i *)&ctx0->long_state[a0[0] & 0x1ffff0]);
		__m128i cx1 = _mm_load_si128(
			(__m128i *)&ctx1->long_state[a1[0] & 0x1ffff0]);
		__m128i ax0 = _mm_load_si128((__m128i *)a0);
		__m128i ax1 = _mm_load_si128((__m128i *)a1);
		uint64_t *nextblock0;
		uint64_t *nextblock1;
		uint64_t *dst0;
		uint64_t *dst1;
		uint64_t hi0;
		uint64_t lo0;
		uint64_t hi1;
		uint64_t lo1;

		cx0 = _mm_aesenc_si128(cx0, ax0);
		cx1 = _mm_aesenc_si128(cx1, ax1);
		_mm_store_si128((__m128i *)c0, cx0);
		_mm_store_si128((__m128i *)c1, cx1);

		bx0 = _mm_xor_si128(bx0, cx0);
		bx1 = _mm_xor_si128(bx1, cx1);
		_mm_store_si128(
			(__m128i *)&ctx0->long_state[a0[0] & 0x1ffff0], bx0);
		_mm_store_si128(
			(__m128i *)&ctx1->long_state[a1[0] & 0x1ffff0], bx1);

		nextblock0 = (uint64_t *)&ctx0->long_state[c0[0] & 0x1ffff0];
		nextblock1 = (uint64_t *)&ctx1->long_state[c1[0] & 0x1ffff0];
		b0[0] = nextblock0[0];
		b0[1] = nextblock0[1];
		b1[0] = nextblock1[0];
		b1[1] = nextblock1[1];
		lo0 = mul128(c0[0], b0[0], &hi0);
		lo1 = mul128(c1[0], b1[0], &hi1);
		a0[0] += hi0;
		a0[1] += lo0;
		a1[0] += hi1;
		a1[1] += lo1;
		dst0 = (uint64_t *)&ctx0->long_state[c0[0] & 0x1ffff0];
		dst1 = (uint64_t *)&ctx1->long_state[c1[0] & 0x1ffff0];
		dst0[0] = a0[0];
		dst0[1] = a0[1];
		dst1[0] = a1[0];
		dst1[1] = a1[1];
		a0[0] ^= b0[0];
		a0[1] ^= b0[1];
		a1[0] ^= b1[0];
		a1[1] ^= b1[1];
		bx0 = cx0;
		bx1 = cx1;
	}

	memcpy(ctx0->text, ctx0->state.init, INIT_SIZE_BYTE);
	memcpy(ctx1->text, ctx1->state.init, INIT_SIZE_BYTE);
	memcpy(expanded_key0, &ctx0->state.hs.b[32], AES_KEY_SIZE);
	memcpy(expanded_key1, &ctx1->state.hs.b[32], AES_KEY_SIZE);
	ExpandAESKey256(expanded_key0);
	ExpandAESKey256(expanded_key1);

	for (i = 0; likely(i < MEMORY); i += INIT_SIZE_BYTE) {
		for (block = 0; block < 8; block++) {
			xmminput0[block] = _mm_xor_si128(
				longoutput0[(i >> 4) + block], xmminput0[block]);
			xmminput1[block] = _mm_xor_si128(
				longoutput1[(i >> 4) + block], xmminput1[block]);
		}
		for (j = 0; j < 10; j++) {
			for (block = 0; block < 8; block++) {
				xmminput0[block] = _mm_aesenc_si128(
					xmminput0[block], expkey0[j]);
				xmminput1[block] = _mm_aesenc_si128(
					xmminput1[block], expkey1[j]);
			}
		}
	}

	memcpy(ctx0->state.init, ctx0->text, INIT_SIZE_BYTE);
	memcpy(ctx1->state.init, ctx1->text, INIT_SIZE_BYTE);
	crypto_keccak_permutation(&ctx0->state.hs);
	crypto_keccak_permutation(&ctx1->state.hs);
	extra_hashes[ctx0->state.hs.b[0] & 3](&ctx0->state, 200, hash0);
	extra_hashes[ctx1->state.hs.b[0] & 3](&ctx1->state, 200, hash1);
}

static void cn_slow_hash_runtime_aes_check(void *a, const void *b, size_t c, void *d) {
	if (cpu_has_aesni())
		cn_slow_hash_aesni(a, b, c, d);
	else
		cn_slow_hash_noaesni(a, b, c, d);
}

static void (*cn_slow_hash_fp)(void *, const void *, size_t, void *) = cn_slow_hash_runtime_aes_check;

int crypto_cn_slow_hash_double_supported(void) {
	return cn_slow_hash_fp == &cn_slow_hash_aesni;
}

void crypto_cn_slow_hash_double(
	void *context0,
	const void *data0,
	size_t length0,
	struct cryptoHash *hash0,
	void *context1,
	const void *data1,
	size_t length1,
	struct cryptoHash *hash1) {
	if (cn_slow_hash_fp == &cn_slow_hash_aesni) {
		cn_slow_hash_aesni_double(
			context0, data0, length0, hash0,
			context1, data1, length1, hash1);
		return;
	}

	cn_slow_hash_noaesni(context0, data0, length0, hash0);
	cn_slow_hash_noaesni(context1, data1, length1, hash1);
}

void crypto_cn_slow_hash(void *a, const void *b, size_t c, struct cryptoHash *d) { (*cn_slow_hash_fp)(a, b, c, d); }

// If INITIALIZER fails to compile on your platform, just comment out INITIALIZER below
INITIALIZER(detect_aes) { cn_slow_hash_fp = cpu_has_aesni() ? &cn_slow_hash_aesni : &cn_slow_hash_noaesni; }

#endif  // !TARGET_OS_IPHONE
