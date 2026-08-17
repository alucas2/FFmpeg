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

/*
 * @file
 * Electronic Arts Layer 3, a mofication of MP3
 *
 * Adapted from the work of Ben Moench
 * https://bitbucket.org/Zenchreal/ealayer3/src/master/
 */

#include "avcodec.h"
#include "codec_internal.h"
#include "ealayer3.h"
#include "get_bits.h"
#include "mpegaudiodecheader.h"
#include "put_bits.h"
#include "packet.h"
#include <stdbool.h>

#define USE_FLOATS 0

#include "codec_internal.h"
#include "mpegaudio.h"

#define SHR(a,b)       (((int)(a))>>(b))
/* WARNING: only correct for positive numbers */
#define FIXR_OLD(a)    ((int)((a) * FRAC_ONE + 0.5))
#define FIXR(a)        ((int)((a) * FRAC_ONE + 0.5))
#define FIXHR(a)       ((int)((a) * (1LL<<32) + 0.5))
#define MULH3(x, y, s) MULH((s)*(x), y)
#define MULLx(x, y, s) MULL((int)(x),(y),s)
#define RENAME(a)      a ## _fixed
#define OUT_FMT   AV_SAMPLE_FMT_S16
#define OUT_FMT_P AV_SAMPLE_FMT_S16P

/* Intensity stereo table. See commit b91d46614df189e7905538e7f5c4ed9c7ed0d274
 * (float based mp1/mp2/mp3 decoders.) for how they were created. */
static const int32_t is_table[2][16] = {
    { 0x000000, 0x1B0CB1, 0x2ED9EC, 0x400000, 0x512614, 0x64F34F, 0x800000 },
    { 0x800000, 0x64F34F, 0x512614, 0x400000, 0x2ED9EC, 0x1B0CB1, 0x000000 }
};

/* Antialiasing table. See commit ce4a29c066cddfc180979ed86396812f24337985
 * (optimize antialias) for how they were created. */
static const int32_t csa_table[8][4] = {
    { 0x36E129F8, 0xDF128056, 0x15F3AA4E, 0xA831565E },
    { 0x386E75F2, 0xE1CF24A5, 0x1A3D9A97, 0xA960AEB3 },
    { 0x3CC6B73A, 0xEBF19FA6, 0x28B856E0, 0xAF2AE86C },
    { 0x3EEEA054, 0xF45B88BC, 0x334A2910, 0xB56CE868 },
    { 0x3FB6905C, 0xF9F27F18, 0x39A90F74, 0xBA3BEEBC },
    { 0x3FF23F20, 0xFD60D1E4, 0x3D531104, 0xBD6E92C4 },
    { 0x3FFE5932, 0xFF175EE4, 0x3F15B816, 0xBF1905B2 },
    { 0x3FFFE34A, 0xFFC3612F, 0x3FC34479, 0xBFC37DE5 }
};

#include "mpegaudiodec_template.c"

typedef struct EALayer3DecodeContext {
    // MPEG1: store granule0 while we wait for granule 1
    bool gr0_valid;
    EALayer3Granule gr0;
    AVPacket *gr0_pkt;

    MPADecodeContext inner; // Function from mpegaudiodec_template.c
} EALayer3DecodeContext;

static av_cold int ealayer3_decode_init(AVCodecContext *avctx) {
    EALayer3DecodeContext *ctx = avctx->priv_data;
    ctx->gr0_valid = false;
    ctx->gr0_pkt = av_packet_alloc();
    decode_ctx_init(avctx, &ctx->inner); // Function from mpegaudiodec_template.c
    avctx->sample_fmt = OUT_FMT; // Interleave to make the copying of uncompressed samples easier
    return 0;
}

static void copy_bits(GetBitContext *src, PutBitContext *dst, unsigned n) {
    while (n >= 32) {
        put_bits32(dst, get_bits_long(src, 32));
        n -= 32;
    }
    if (n != 0)
        put_bits(dst, n, get_bits_long(src, n));
}

static int copy_uncompressed_samples(
    AVCodecContext *avctx,
    AVFrame *frame,
    EALayer3Granule *gr,
    AVPacket *gr_pkt
) {
    unsigned nb_channels = gr->header.mode == MPA_MONO ? 1 : 2;
    uint8_t *dst = frame->extended_data[0] + gr->uncompressed_pos;
    uint8_t *src = gr_pkt->data + gr->uncompressed_offset;
    unsigned size = nb_channels * gr->uncompressed_len * sizeof(OUT_INT);
    if (
        size > (frame->extended_data[0] + frame->linesize[0] - dst)
        || size > gr_pkt->data + gr_pkt->size - src
    ) {
        return AVERROR_INVALIDDATA;
    }
    av_log(avctx, AV_LOG_DEBUG, "copied uncompressed samples: pos=%d, len=%d\n", gr->uncompressed_pos, gr->uncompressed_len);
    memcpy(dst, src, size);
    return 0;
}

#define MAX_MPEG_FRAME_BUFFER 2880

static int ealayer3_reconstruct_decode(
    AVCodecContext *avctx,
    AVFrame *frame,
    EALayer3Granule *gr0,
    AVPacket *gr0_pkt,
    EALayer3Granule *gr1,
    AVPacket *gr1_pkt
) {
    EALayer3DecodeContext *ctx = avctx->priv_data;
    uint8_t buf[MAX_MPEG_FRAME_BUFFER];
    PutBitContext buf_write;
    GetBitContext gr0_read;
    GetBitContext gr1_read;

    unsigned nb_channels = gr0->header.mode == MPA_MONO ? 1 : 2;

    init_put_bits(&buf_write, buf, MAX_MPEG_FRAME_BUFFER);
    init_get_bits8(&gr0_read, gr0_pkt->data, gr0_pkt->size);
    if (gr0->header.version == MPEG_VER_1) {
        init_get_bits8(&gr1_read, gr1_pkt->data, gr1_pkt->size);
    }

    // Reconstruct the header
    put_bits(&buf_write, 11, 0x7FF); // Frame sync
    put_bits(&buf_write, 2, gr0->header.version); // MPEG version
    put_bits(&buf_write, 2, 0x1); // Layer 3
    put_bits(&buf_write, 1, 0x1); // No crc protection
    put_bits(&buf_write, 4, 0x0); // Bitrate index
    put_bits(&buf_write, 2, gr0->header.sample_rate_index); // Sampling rate
    put_bits(&buf_write, 1, 0x0); // Padding bit
    put_bits(&buf_write, 1, 0x0); // Private bit
    put_bits(&buf_write, 2, gr0->header.mode); // Channel mode
    put_bits(&buf_write, 2, gr0->header.mode_ext); // Mode extension
    put_bits(&buf_write, 1, 0x1); // Copyright
    put_bits(&buf_write, 1, 0x1); // Original
    put_bits(&buf_write, 2, 0x0); // Emphasis

    // Reconstruct the rest of the frame
    if (gr0->header.version == MPEG_VER_1) {
        skip_bits(&gr0_read, 17);
        skip_bits(&gr1_read, 17);
        put_bits(&buf_write, 9, 0x0); // Main data start
        if (gr0->header.mode == MPA_MONO) {
            put_bits(&buf_write, 5, 0x0);
        } else {
            put_bits(&buf_write, 3, 0x0);
        }
        for (unsigned i = 0; i < nb_channels; i++) {
            copy_bits(&gr1_read, &buf_write, 4); // Scfsi
        }
        for (unsigned i = 0; i < nb_channels; i++) {
            copy_bits(&gr0_read, &buf_write, 12 + 47); // Granule 0 side info
        }
        for (unsigned i = 0; i < nb_channels; i++) {
            copy_bits(&gr1_read, &buf_write, 12 + 47); // Granule 1 side info
        }
        copy_bits(&gr0_read, &buf_write, gr0->len_compressed_bits - get_bits_count(&gr0_read));
        copy_bits(&gr1_read, &buf_write, gr1->len_compressed_bits - get_bits_count(&gr1_read));
    } else {
        skip_bits(&gr0_read, 17);
        put_bits(&buf_write, 8, 0x0); // Main data start
        if (gr0->header.mode == MPA_MONO) {
            put_bits(&buf_write, 1, 0x0);
        } else {
            put_bits(&buf_write, 2, 0x0);
        }
        for (unsigned i = 0; i < nb_channels; i++) {
            copy_bits(&gr0_read, &buf_write, 12 + 51); // Granule 0 side info
        }
        copy_bits(&gr0_read, &buf_write, gr0->len_compressed_bits - get_bits_count(&gr0_read));
    }
    align_put_bits(&buf_write);

    // Decode the reconstructed MPEG frame with functions from mpegaudiodec_template.c
    int ret;
    ret = avpriv_mpegaudio_decode_header((MPADecodeHeader*)&ctx->inner, AV_RB32(buf));
    if (ret < 0)
        return AVERROR_INVALIDDATA;
    ctx->inner.frame = frame;
    mp_decode_frame(&ctx->inner, NULL, buf, put_bits_count(&buf_write) / 8);
    if (ret < 0)
        return ret;
    avctx->sample_rate = ctx->inner.sample_rate;

    // Put the interleaved uncompressed samples
    if (gr0->has_uncompressed) {
        copy_uncompressed_samples(avctx, frame, gr0, gr0_pkt);
    }
    if (gr0->header.version == MPEG_VER_1 && gr1->has_uncompressed) {
        copy_uncompressed_samples(avctx, frame, gr1, gr1_pkt);
    }

    return 0;
}

static int ealayer3_decode_frame(
    AVCodecContext *avctx,
    AVFrame *frame,
    int *got_frame_ptr,
    AVPacket *avpkt
) {
    int ret;
    EALayer3DecodeContext *ctx = avctx->priv_data;
    EALayer3Granule gr = {0};
    *got_frame_ptr = 0;

    if (
        parse_granule_first_half(avpkt->data, &gr)
        || parse_granule_second_half(avpkt->data + gr.len_compressed, &gr)
    )
        return AVERROR_INVALIDDATA; // Unreachable because packet already parsed by muxer

    if (gr.header.version == MPEG_VER_1) {
        if (gr.granule_index == 0) {
            // MPEG1: store granule 0
            if (ctx->gr0_valid)
                return AVERROR_INVALIDDATA; // Granule 0 already occupied

            ctx->gr0_valid = true;
            ctx->gr0 = gr;
            av_packet_ref(ctx->gr0_pkt, avpkt);
        } else {
            // MPEG1: decode granules 0 and 1 together
            if(!ctx->gr0_valid)
                return AVERROR_INVALIDDATA; // Granule 0 is missing
            if (memcmp(&ctx->gr0.header, &gr.header, sizeof(EALayer3Header)) != 0)
                return AVERROR_INVALIDDATA; // Granule 0 and 1 disagree
            ret = ealayer3_reconstruct_decode(avctx, frame, &ctx->gr0, ctx->gr0_pkt, &gr, avpkt);
            if (ret < 0)
                return ret;
            ctx->gr0_valid = false;
            av_packet_unref(ctx->gr0_pkt);
            *got_frame_ptr = 1;
        }
    } else {
        // MPEG2: decode granule alone
        ret = ealayer3_reconstruct_decode(avctx, frame, &gr, avpkt, NULL, NULL);
        if (ret < 0)
            return ret;
        *got_frame_ptr = 1;
    }
    return avpkt->size;
}

static av_cold void ealayer3_decode_flush(AVCodecContext *avctx) {
    EALayer3DecodeContext *ctx = avctx->priv_data;
    ctx->gr0_valid = false;
    flush(avctx); // From mpegaudiodec_template.c
}

static av_cold int ealayer3_decode_close(AVCodecContext *avctx) {
    EALayer3DecodeContext *ctx = avctx->priv_data;
    av_packet_free(&ctx->gr0_pkt);
    return 0;
}

const FFCodec ff_ealayer3_decoder = {
    .p.name         = "ealayer3",
    CODEC_LONG_NAME("Electronic Arts Layer 3 (Modified MPEG audio layer 3)"),
    .p.type         = AVMEDIA_TYPE_AUDIO,
    .p.id           = AV_CODEC_ID_EALAYER3,
    .priv_data_size = sizeof(EALayer3DecodeContext),
    .init           = ealayer3_decode_init,
    FF_CODEC_DECODE_CB(ealayer3_decode_frame),
    .p.capabilities = AV_CODEC_CAP_DR1,
    .flush          = ealayer3_decode_flush,
    .close          = ealayer3_decode_close,
};
