/*
===========================================================================
	SHA-256 (FIPS 180-4) implementation.

	Standalone, public-domain style implementation. No external dependencies
	beyond <string.h> / fixed-width integer types, so it can also be compiled
	on its own for a known-answer self-test.
===========================================================================
*/

#include <string.h>

#include "idlib/hashing/SHA256.h"

static const uint32_t SHA256_K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define SHA256_ROTR(x, n)	( ( (x) >> (n) ) | ( (x) << ( 32 - (n) ) ) )
#define SHA256_SHR(x, n)	( (x) >> (n) )
#define SHA256_CH(x, y, z)	( ( (x) & (y) ) ^ ( ~(x) & (z) ) )
#define SHA256_MAJ(x, y, z)	( ( (x) & (y) ) ^ ( (x) & (z) ) ^ ( (y) & (z) ) )
#define SHA256_BSIG0(x)		( SHA256_ROTR(x, 2) ^ SHA256_ROTR(x, 13) ^ SHA256_ROTR(x, 22) )
#define SHA256_BSIG1(x)		( SHA256_ROTR(x, 6) ^ SHA256_ROTR(x, 11) ^ SHA256_ROTR(x, 25) )
#define SHA256_SSIG0(x)		( SHA256_ROTR(x, 7) ^ SHA256_ROTR(x, 18) ^ SHA256_SHR(x, 3) )
#define SHA256_SSIG1(x)		( SHA256_ROTR(x, 17) ^ SHA256_ROTR(x, 19) ^ SHA256_SHR(x, 10) )

static void SHA256_Transform( uint32_t state[8], const unsigned char block[64] ) {
	uint32_t w[64];
	uint32_t a, b, c, d, e, f, g, h;
	int i;

	for ( i = 0; i < 16; i++ ) {
		w[i] = ( (uint32_t)block[i * 4 + 0] << 24 )
			 | ( (uint32_t)block[i * 4 + 1] << 16 )
			 | ( (uint32_t)block[i * 4 + 2] <<  8 )
			 | ( (uint32_t)block[i * 4 + 3]       );
	}
	for ( i = 16; i < 64; i++ ) {
		w[i] = SHA256_SSIG1( w[i - 2] ) + w[i - 7] + SHA256_SSIG0( w[i - 15] ) + w[i - 16];
	}

	a = state[0]; b = state[1]; c = state[2]; d = state[3];
	e = state[4]; f = state[5]; g = state[6]; h = state[7];

	for ( i = 0; i < 64; i++ ) {
		uint32_t t1 = h + SHA256_BSIG1( e ) + SHA256_CH( e, f, g ) + SHA256_K[i] + w[i];
		uint32_t t2 = SHA256_BSIG0( a ) + SHA256_MAJ( a, b, c );
		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}

	state[0] += a; state[1] += b; state[2] += c; state[3] += d;
	state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void SHA256_Init( sha256Context_t *ctx ) {
	ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
	ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
	ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
	ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
	ctx->count = 0;
}

void SHA256_Update( sha256Context_t *ctx, const void *data, size_t length ) {
	const unsigned char *p = (const unsigned char *)data;
	size_t have = (size_t)( ctx->count % 64 );
	ctx->count += length;

	if ( have ) {
		size_t need = 64 - have;
		if ( length < need ) {
			memcpy( ctx->buffer + have, p, length );
			return;
		}
		memcpy( ctx->buffer + have, p, need );
		SHA256_Transform( ctx->state, ctx->buffer );
		p += need;
		length -= need;
	}
	while ( length >= 64 ) {
		SHA256_Transform( ctx->state, p );
		p += 64;
		length -= 64;
	}
	if ( length ) {
		memcpy( ctx->buffer, p, length );
	}
}

void SHA256_Final( sha256Context_t *ctx, unsigned char digest[32] ) {
	uint64_t bits = ctx->count * 8;
	size_t have = (size_t)( ctx->count % 64 );
	unsigned char pad[72];
	size_t padLen;
	int i;

	// 0x80 then zeros so that the message ends 8 bytes short of a block boundary
	pad[0] = 0x80;
	memset( pad + 1, 0, sizeof( pad ) - 1 );
	padLen = ( have < 56 ) ? ( 56 - have ) : ( 120 - have );

	// append the 64-bit big-endian bit length
	pad[padLen + 0] = (unsigned char)( bits >> 56 );
	pad[padLen + 1] = (unsigned char)( bits >> 48 );
	pad[padLen + 2] = (unsigned char)( bits >> 40 );
	pad[padLen + 3] = (unsigned char)( bits >> 32 );
	pad[padLen + 4] = (unsigned char)( bits >> 24 );
	pad[padLen + 5] = (unsigned char)( bits >> 16 );
	pad[padLen + 6] = (unsigned char)( bits >>  8 );
	pad[padLen + 7] = (unsigned char)( bits       );

	SHA256_Update( ctx, pad, padLen + 8 );

	for ( i = 0; i < 8; i++ ) {
		digest[i * 4 + 0] = (unsigned char)( ctx->state[i] >> 24 );
		digest[i * 4 + 1] = (unsigned char)( ctx->state[i] >> 16 );
		digest[i * 4 + 2] = (unsigned char)( ctx->state[i] >>  8 );
		digest[i * 4 + 3] = (unsigned char)( ctx->state[i]       );
	}
}

void SHA256_BlockChecksum( const void *data, size_t length, unsigned char digest[32] ) {
	sha256Context_t ctx;
	SHA256_Init( &ctx );
	SHA256_Update( &ctx, data, length );
	SHA256_Final( &ctx, digest );
}

void SHA256_DigestToHex( const unsigned char digest[32], char hex[65] ) {
	static const char d[] = "0123456789abcdef";
	int i;
	for ( i = 0; i < 32; i++ ) {
		hex[i * 2 + 0] = d[( digest[i] >> 4 ) & 0xf];
		hex[i * 2 + 1] = d[digest[i] & 0xf];
	}
	hex[64] = '\0';
}
