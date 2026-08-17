/*
 * Electronic Arts .cdata file Demuxer
 * Copyright (c) 2007 Peter Ross
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

/**
 * @file
 * Electronic Arts cdata Format Demuxer
 * by Peter Ross (pross@xvid.org)
 *
 * Technical details here:
 *  http://wiki.multimedia.cx/index.php?title=EA_Command_And_Conquer_3_Audio_Codec
 *  https://wiki.multimedia.cx/index.php/EA_SAGE_Audio_Files
 */

#include "avio.h"
#include "libavutil/channel_layout.h"
#include "avformat.h"
#include "demux.h"
#include "internal.h"

#include "libavcodec/ealayer3.h"

typedef struct CdataDemuxContext {
    enum AVCodecID codec;

    // ADPCM EA XA context
    unsigned int channels;
    unsigned int audio_pts;

    // EALayer3 context
    AVChannelLayout channel_layout; // Channel layout specified in the header
    unsigned int sample_rate; // Sample rate specified in the header
    unsigned int current_stream;
    uint8_t previous_granule_index; // 1 bit
} CdataDemuxContext;

static int cdata_probe(const AVProbeData *p)
{
    const uint8_t *b = p->buf;

    if (b[0] == 0x04 && (b[1] == 0x00 || b[1] == 0x04 || b[1] == 0x0C || b[1] == 0x14))
        return AVPROBE_SCORE_MAX/8;
    return 0;
}

static int cdata_read_header(AVFormatContext *s)
{
    CdataDemuxContext *cdata = s->priv_data;
    AVIOContext *pb = s->pb;
    unsigned int sample_rate, header, total_samples;
    AVStream *st;
    AVChannelLayout channel_layout;

    header = avio_rb16(pb);
    switch (header) {
        case 0x0400:
            channel_layout = (AVChannelLayout){ .nb_channels = 1, .order = AV_CHANNEL_ORDER_UNSPEC };
            break;
        case 0x0404:
            channel_layout  = (AVChannelLayout){ .nb_channels = 2, .order = AV_CHANNEL_ORDER_UNSPEC };
            break;
        case 0x040C:
            channel_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_QUAD;         break;
        case 0x0414:
            channel_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT1_BACK; break;
        case 0x0500:
            channel_layout = (AVChannelLayout){ .nb_channels = 1, .order = AV_CHANNEL_ORDER_UNSPEC };
            break;
        case 0x0504:
            channel_layout = (AVChannelLayout){ .nb_channels = 2, .order = AV_CHANNEL_ORDER_UNSPEC };
            break;
        default:
            av_log(s, AV_LOG_INFO, "unknown header 0x%04x\n", header);
            return -1;
    };
    cdata->channels = channel_layout.nb_channels;

    sample_rate = avio_rb16(pb);
    total_samples = avio_rb32(pb);
    avio_skip(pb, (total_samples & 0x20000000) ? 12 : 8);

    switch (header >> 8) {
        case 0x04:
            st = avformat_new_stream(s, NULL);
            if (!st)
                return AVERROR(ENOMEM);
            st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
            st->codecpar->codec_tag = 0; /* no fourcc */
            st->codecpar->codec_id = AV_CODEC_ID_ADPCM_EA_XAS;
            st->codecpar->ch_layout = channel_layout;
            st->codecpar->sample_rate = sample_rate;
            avpriv_set_pts_info(st, 64, 1, sample_rate);
            cdata->codec = AV_CODEC_ID_ADPCM_EA_XAS;
            cdata->audio_pts = 0;
            break;
        case 0x05:
            cdata->codec = AV_CODEC_ID_EALAYER3;
            cdata->channel_layout = channel_layout;
            cdata->sample_rate = sample_rate;
            cdata->current_stream = 0;
            cdata->previous_granule_index = -1;
            s->ctx_flags |= AVFMTCTX_NOHEADER; // Number of streams unknown yet
            break;
        default:
            av_assert0(0); // Unreachable
    }
    return 0;
}

static int read_packet_adpcm_ea_xa(AVFormatContext *s, AVPacket *pkt) {
    CdataDemuxContext *cdata = s->priv_data;
    int packet_size = 76*cdata->channels;

    int ret = av_get_packet(s->pb, pkt, packet_size);
    if (ret < 0)
        return ret;
    pkt->pts = cdata->audio_pts++;
    return 0;
}

static int read_packet_ealayer3(AVFormatContext *s, AVPacket *pkt) {
    CdataDemuxContext *cdata = s->priv_data;
    EALayer3Granule gr = {0};
    int ret;

    // Read the granule first half
    ret = av_get_packet(s->pb, pkt, PARSE_FIRST_HALF_REQUIRED_BYTES);
    if (ret < 0)
        return ret;
    if (parse_granule_first_half(pkt->data, &gr))
        return AVERROR_INVALIDDATA;
    ret = av_append_packet(s->pb, pkt, gr.len_compressed - pkt->size);
    if (ret < 0)
        return ret;

    // A null granule means no more packets
    if (
        gr.header.version == 0
        && gr.header.sample_rate_index == 0
        && gr.header.mode == 0
        && gr.header.mode_ext == 0
    )
        return AVERROR_EOF;

    // Read the granule second half
    if (gr.has_uncompressed) {
        ret = av_append_packet(s->pb, pkt, PARSE_SECOND_HALF_REQUIRED_BYTES);
        if (ret < 0)
            return ret;
        if (parse_granule_second_half(pkt->data + gr.len_compressed, &gr))
            return AVERROR_INVALIDDATA;
        ret = av_append_packet(s->pb, pkt, gr.len_total - pkt->size);
        if (ret < 0)
            return ret;
    }

    // Determine which stream the granule belongs to
    if (gr.header.version == MPEG_VER_1) {
        // MPEG1: granules for each stream are interleaved as follows:
        // s0_g0, s1_g0, sN_g0, s0_g1, s1_g1, sN_g1, s0_g0, etc...
        if (gr.granule_index != cdata->previous_granule_index) {
            cdata->current_stream = 0;
            cdata->previous_granule_index = gr.granule_index;
        } else {
            cdata->current_stream++;
        }
    } else {
        // MPEG2: there is only one stream
        cdata->current_stream = 0;
    }
    pkt->stream_index = cdata->current_stream;

    // Allocate new stream if needed
    if (cdata->current_stream == s->nb_streams) {
        AVStream* st = avformat_new_stream(s, NULL);
        if (!st)
            return AVERROR(ENOMEM);
        st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        st->codecpar->codec_id = AV_CODEC_ID_EALAYER3;
        st->codecpar->sample_rate = cdata->sample_rate;
        st->codecpar->ch_layout = cdata->channel_layout;
    }
    return 0;
}

static int cdata_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    CdataDemuxContext *cdata = s->priv_data;
    switch (cdata->codec) {
        case AV_CODEC_ID_ADPCM_EA_XAS:
            return read_packet_adpcm_ea_xa(s, pkt);
        case AV_CODEC_ID_EALAYER3:
            return read_packet_ealayer3(s, pkt);
        default:
            av_assert0(0); // Unreachable
    }
}

const FFInputFormat ff_ea_cdata_demuxer = {
    .p.name         = "ea_cdata",
    .p.long_name    = NULL_IF_CONFIG_SMALL("Electronic Arts cdata"),
    .p.extensions   = "cdata",
    .priv_data_size = sizeof(CdataDemuxContext),
    .read_probe     = cdata_probe,
    .read_header    = cdata_read_header,
    .read_packet    = cdata_read_packet,
};
