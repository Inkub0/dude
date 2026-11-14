#ifndef __SHA256_H__
#define __SHA256_H__

#include <stddef.h>
#include <stdint.h>

/*
===============================================================================

	Calculates a SHA-256 (FIPS 180-4) digest for a block or a stream of data.

	Used to verify a downloaded mod library against a pinned hash before it is
	placed on disk and loaded (see framework/ModDownload).

===============================================================================
*/

struct sha256Context_t {
	uint32_t		state[8];
	uint64_t		count;			// total number of bytes fed in
	unsigned char	buffer[64];		// partial 64-byte block
};

void SHA256_Init( sha256Context_t *ctx );
void SHA256_Update( sha256Context_t *ctx, const void *data, size_t length );
void SHA256_Final( sha256Context_t *ctx, unsigned char digest[32] );

// one-shot helper for a single contiguous buffer
void SHA256_BlockChecksum( const void *data, size_t length, unsigned char digest[32] );

// format a 32-byte digest as 64 lowercase hex chars plus a terminating '\0'
// (hex must have room for 65 bytes)
void SHA256_DigestToHex( const unsigned char digest[32], char hex[65] );

#endif /* !__SHA256_H__ */
