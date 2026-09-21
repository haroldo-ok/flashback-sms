
/*
 * REminiscence - Flashback interpreter
 * Copyright (C) 2005-2019 Gregory Montoir (cyx@users.sourceforge.net)
 */

#include "unpack.h"
#include "util.h"

struct BytekillerCtx {
	int size;
	uint32_t crc;
	uint32_t bits;
	uint8_t *dst;
	const uint8_t *src;
};

static int nextBit(BytekillerCtx *uc) {
	int bit = uc->bits & 1;
	uc->bits >>= 1;
	if (uc->bits == 0) {
		uc->bits = READ_BE_UINT32(uc->src); uc->src -= 4;
		uc->crc ^= uc->bits;
		bit = uc->bits & 1;
		uc->bits = (1 << 31) | (uc->bits >> 1);
	}
	return bit;
}

static uint32_t getBits(BytekillerCtx *uc, int count) {
	uint32_t bits = 0;
	for (int i = 0; i < count; ++i) {
		bits = (bits << 1) | nextBit(uc);
	}
	return bits;
}

static void copyBytes(BytekillerCtx *uc, int len, int offset) {
	uc->size -= len;
	if (uc->size < 0) {
		len += uc->size;
		uc->size = 0;
	}
	if (offset != 0) {
		 for (int i = 0; i < len; ++i, --uc->dst) {
			*(uc->dst) = *(uc->dst + offset);
		}
	} else {
		for (int i = 0; i < len; ++i, --uc->dst) {
			*(uc->dst) = (uint8_t)getBits(uc, 8);
		}
	}
}

bool bytekiller_unpack(uint8_t *dst, int dstSize, const uint8_t *src, int srcSize) {
	BytekillerCtx uc;
	uc.src = src + srcSize - 4;
	uc.size = READ_BE_UINT32(uc.src); uc.src -= 4;
	if (uc.size > dstSize) {
		warning("Unexpected unpack size %d, buffer size %d", uc.size, dstSize);
		return false;
	}
	uc.dst = dst + uc.size - 1;
	uc.crc = READ_BE_UINT32(uc.src); uc.src -= 4;
	uc.bits = READ_BE_UINT32(uc.src); uc.src -= 4;
	uc.crc ^= uc.bits;
	while (uc.size > 0) {
		int code = getBits(&uc, 2);
		switch (code) {
		case 0:
			copyBytes(&uc, getBits(&uc, 3) + 1, 0);
			break;
		case 1:
			copyBytes(&uc, 2, getBits(&uc, 8));
			break;
		default:
			code = ((code & 1) << 1) | nextBit(&uc);
			switch (code) {
			case 3:
				copyBytes(&uc, getBits(&uc, 8) + 9, 0);
				break;
			case 2: {
					const int len = getBits(&uc, 8) + 1;
					copyBytes(&uc, len, getBits(&uc, 12));
				}
				break;
			default:
				copyBytes(&uc, code + 3, getBits(&uc, code + 9));
				break;
			}
			break;
		}
	}
	assert(uc.size == 0);
	return uc.crc == 0;
}

struct bitstream_t {
	uint16_t mask;
	int size;
	const uint8_t *src;
};

static uint8_t read_byte(struct bitstream_t *bs) {
	const uint8_t b = *(bs->src);
	++bs->src;
	return b;
}

static void fill(struct bitstream_t *bs) {
	if (bs->size < 9) {
		const uint8_t b = read_byte(bs);
		bs->mask |= b << (8 - bs->size);
		bs->size += 8;
	}
}

static int get_bit(struct bitstream_t *bs) {
	fill(bs);
	assert(bs->size > 0);
	const int val = (bs->mask & 0x8000) != 0;
	bs->mask <<= 1;
	--bs->size;
	return val;
}

static int get_bits(struct bitstream_t *bs, int count) {
	if (count > bs->size) {
		const int val = bs->mask >> (16 - bs->size);
		count -= bs->size;
		bs->size = 0;
		bs->mask = 0;
		fill(bs);
		return (val << count) | get_bits(bs, count);
	} else {
		assert(count <= bs->size);
		const int val = bs->mask >> (16 - count);
		bs->mask <<= count;
		bs->size -= count;
		return val;
	}
}

static uint8_t rol1(uint8_t r) {
	return (r << 1) | (r >> 7);
}

struct UnpackCtx {
	struct bitstream_t bs;
	uint8_t *dst;
	uint32_t dstSize, dstOffset;
};

static void outputb(UnpackCtx *ctx, uint8_t b) {
	assert(ctx->dstOffset < ctx->dstSize);
	ctx->dst[ctx->dstOffset++] = b;
}

static void outputs(UnpackCtx *ctx, int offset, int count) {
	assert(ctx->dstOffset + count <= ctx->dstSize);
	for (int i = 0; i < count; ++i) {
		ctx->dst[ctx->dstOffset + i] = ctx->dst[ctx->dstOffset + offset + i];
	}
	ctx->dstOffset += count;
}

static int decode_bs(UnpackCtx *ctx, const uint8_t *src, int size) {
	struct bitstream_t *bs = &ctx->bs;
	bs->mask = 0;
	bs->size = 0;
	bs->src = src;

	uint8_t key = read_byte(bs);
	while (size != 0) {
		if (get_bit(bs) == 0) {
			const uint8_t val = get_bits(bs, 8);
			outputb(ctx, val ^ key);
			--size;
			if (size == 0) {
				break;
			}
			key = rol1(val);
			continue;
		}
		int count = 1;
		while (count <= 8 && get_bit(bs)) {
			++count;
		}
		const int count2 = get_bits(bs, count) + 3;
		const int offset = get_bits(bs, 13);
		outputs(ctx, -offset - 1, count2);
		size -= count2;
	}
	const int read = bs->src - src;
	debug(DBG_UNPACK, "end %d bytes, out %d", read, ctx->dstOffset);
	return read;
}

bool pc98_unpack(uint8_t *dst, int dstSize, const uint8_t *src, int srcSize) {
	UnpackCtx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.dst = dst;
	ctx.dstSize = dstSize;
	ctx.dstOffset = 0;
	return decode_bs(&ctx, src, dstSize);
}
