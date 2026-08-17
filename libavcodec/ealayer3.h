/*
 * Copyright (c) 2026 The FFmpeg project
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVCODEC_EALAYER3_H
#define AVCODEC_EALAYER3_H

#include <stdint.h>
#include "get_bits.h"
#include "libavcodec/mpegaudio.h"

enum EALayer3MpegVersion {
    MPEG_VER_2_5 = 0,
    MPEG_VER_FORBIDDEN = 1,
    MPEG_VER_2 = 2,
    MPEG_VER_1 = 3,
};

/*
 * EALayer3 granule headers are compacted MP3 headers.
 * MPEG1: the two granules are separated, each have their own header
 *
 * https://wiki.multimedia.cx/index.php/EALayer3
 */
typedef struct EALayer3GranuleHeader {
    uint8_t version; // Corresponds to mpeg bits 21:20
    uint8_t sample_rate_index; // Corresponds to mpeg bits 11:10
    uint8_t mode; // Corresponds to mpeg bits 7:6
    uint8_t mode_ext; // Corresponds to mpeg bits 5:4
} EALayer3Header;

typedef struct EALayer3Granule {
    // The first half contains the header and mpeg-compressed data
    uint8_t has_uncompressed;
    EALayer3Header header;
    uint8_t granule_index; // 0 or 1
    uint32_t len_compressed_bits; // Len in bits
    uint32_t len_compressed; // Len in bytes

    // If has_uncompressed==1, the second half contains the uncompressed samples
    uint32_t uncompressed_len; // Number of uncompressed samples
    uint32_t uncompressed_pos; // Where the uncompressed samples belong in the final frame
    uint32_t uncompressed_offset; // Bytes from the start of the granule
    uint32_t len_total; // Len in bytes
} EALayer3Granule;

/*
 * Number of bytes to parse, to determine the length of the first half of a granule.
 * 151 bits rounded to 19 bytes
 */
#define PARSE_FIRST_HALF_REQUIRED_BYTES 19

/*
 * Parse the first half of a granule (header and mpeg-compressed data)
 */
static inline int parse_granule_first_half(uint8_t *bytes, EALayer3Granule *gr) {
    GetBitContext bits;
    init_get_bits8(&bits, bytes, PARSE_FIRST_HALF_REQUIRED_BYTES);

    // Read the header (17 bits)
    gr->has_uncompressed = get_bits(&bits, 8);
    gr->header.version = get_bits(&bits, 2);
    if (gr->header.version == MPEG_VER_FORBIDDEN)
        return -1;
    gr->header.sample_rate_index = get_bits(&bits, 2);
    if (gr->header.sample_rate_index == 3)
        return -1;
    gr->header.mode = get_bits(&bits, 2);
    gr->header.mode_ext = get_bits(&bits, 2);
    gr->granule_index = get_bits1(&bits);

    // Skip the mpeg-compressed data
    unsigned n_channels = gr->header.mode == MPA_MONO ? 1 : 2;
    if (gr->header.version == MPEG_VER_1 && gr->granule_index == 1) {
        for (unsigned i = 0; i < n_channels; i++) {
            skip_bits(&bits, 4); // scfsi
        }
    }
    unsigned data_bits = 0;
    for (unsigned i = 0; i < n_channels; i++) {
        data_bits += get_bits(&bits, 12); // part2_3_len
        if (gr->header.version == MPEG_VER_1) {
            skip_bits(&bits, 47);
        } else {
            skip_bits(&bits, 51);
        }
    }
    gr->len_compressed_bits = get_bits_count(&bits) + data_bits;
    gr->len_compressed = (gr->len_compressed_bits + 7) / 8; // Pad do bytes
    return 0;
}

/*
 * Number of bytes to parse, to determine the lenght of the second half of a granule
 */
#define PARSE_SECOND_HALF_REQUIRED_BYTES 8

/*
 * Parse the second half of a granule (uncompressed data)
 */
static inline int parse_granule_second_half(uint8_t *bytes, EALayer3Granule *gr) {
    gr->uncompressed_len = AV_RB32(bytes);
    gr->uncompressed_pos = AV_RB32(bytes + 4);

    unsigned byte_offset = gr->len_compressed;
    byte_offset += 8; // Add len of the 2 fields above
    gr->uncompressed_offset = byte_offset;
    if (gr->header.mode == MPA_MONO) {
        byte_offset += 2 * gr->uncompressed_len;
    } else {
        byte_offset += 4 * gr->uncompressed_len;
    }
    gr->len_total = byte_offset;
    return 0;
}

#endif /* AVCODEC_EALAYER3_H */
