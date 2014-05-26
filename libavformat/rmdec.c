/*
 * RealAudio Demuxer, reimplemented for OPW, summer 2014.
 * Copyright (c) 2014 Katerina Barone-Adesi
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/* Format documentation:
 * http://wiki.multimedia.cx/index.php?title=RealMedia
 * https://common.helixcommunity.org/2003/HCS_SDK_r5/htmfiles/rmff.htm
 */

#include "libavutil/channel_layout.h"
#include "libavutil/intreadwrite.h"

#include "avformat.h"
#include "rm.h"

/* Header for RealAudio 1.0 (.ra version 3
 * and RealAudio 2.0 file (.ra version 4). */
#define RA_HEADER MKTAG('.', 'r', 'a', 0xfd)

struct RMStream {
};

typedef struct {
} RMDemuxContext;

static int rm_probe(AVProbeData *p)
{
    /* RealAudio header; TODO: handle RMF later. */
    /* TODO: check that this works */
    uint32_t buftag = MKTAG(p->buf[0], p->buf[1], p->buf[2], p->buf[3]);
    return (buftag == RA_HEADER) ? AVPROBE_SCORE_MAX : 0;
}

static int rm_read_header(AVFormatContext *s)
{
    AVIOContext *acpb = s->pb;
    RMDemuxContext *rmdc = s->priv_data;
    AVStream *st = NULL;

    uint32_t tag;
    uint16_t version, header_size;
    char a, b, c, d;

    tag = avio_rl32(acpb);

    a = tag >> 24;
    b = (tag >> 16) & 0xff;
    c = (tag >> 8) & 0xff;
    d = tag & 0xff;
    printf("Got tag %c%c%c%c\n", a, b, c, d);
    printf("tag %u, header %u\n", tag, RA_HEADER);
    if (tag != RA_HEADER)
        return AVERROR_INVALIDDATA;
    version = avio_rb16(acpb);
    printf("Version: %u\n", version);
    if (version != 3) /* TODO: add v4 uspport */
        return AVERROR_INVALIDDATA;
    header_size = avio_rb16(acpb);
    printf("Header size: %u\n", header_size);
    avio_skip(acpb, header_size); /* TODO: read rest of header properly */

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    st->codec->channel_layout = AV_CH_LAYOUT_MONO;
    st->codec->channels = 1;
    //st->codec->codec_tag = AV_RL32(st->codec->extradata); /* Why? */
    //st->codec->codec_id = ff_codec_get_id(ff_rm_codec_tags, st->codec->codec_tag);
    st->codec->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codec->sample_rate = 8000;


    return 0;
}

static int rm_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    return 0;
}

static int rm_read_close(AVFormatContext *s)
{
    return 0;
}

static int64_t rm_read_dts(AVFormatContext *s, int stream_index,
                               int64_t *ppos, int64_t pos_limit)
{
    return 0;
}

int
ff_rm_parse_packet (AVFormatContext *s, AVIOContext *pb,
                    AVStream *st, RMStream *ast, int len, AVPacket *pkt,
                    int *seq, int flags, int64_t timestamp)
{
    return 0;
}

int
ff_rm_retrieve_cache (AVFormatContext *s, AVIOContext *pb,
                      AVStream *st, RMStream *ast, AVPacket *pkt)
{
    return 0;
}

RMStream *ff_rm_alloc_rmstream (void)
{
    return av_mallocz(sizeof(RMStream));
}

int
ff_rm_read_mdpr_codecdata (AVFormatContext *s, AVIOContext *pb,
                           AVStream *st, RMStream *rst, int codec_data_size)
{
    return 0;
}

void ff_rm_free_rmstream (RMStream *rms)
{
}

AVInputFormat ff_rm_demuxer = {
    .name           = "rm",
    .long_name      = NULL_IF_CONFIG_SMALL("RealMedia"),
    .priv_data_size = sizeof(RMDemuxContext),
    .read_probe     = rm_probe,
    .read_header    = rm_read_header,
    .read_packet    = rm_read_packet,
    .read_close     = rm_read_close,
    .read_timestamp = rm_read_dts,
};

AVInputFormat ff_rdt_demuxer = {
    .name           = "rdt",
    .long_name      = NULL_IF_CONFIG_SMALL("RDT demuxer"),
    .priv_data_size = sizeof(RMDemuxContext),
    .read_close     = rm_read_close,
    .flags          = AVFMT_NOFILE,
};

