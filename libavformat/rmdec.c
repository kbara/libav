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
 *
 * Naming convention:
 * ra_ = RealAudio
 * rm_ = RealMedia (potentially with video)
 * real_ = both.
 */

#include <inttypes.h>

#include "libavutil/channel_layout.h"
#include "libavutil/intreadwrite.h"

#include "avformat.h"
#include "rm.h"

/* Header for RealAudio 1.0 (.ra version 3
 * and RealAudio 2.0 file (.ra version 4). */
#define RA_HEADER MKTAG('.', 'r', 'a', '\xfd')

/* The relevant VSELP format has 159-bit frames, stored in 20 bytes */
#define RA144_PKT_SIZE 20

/* RealAudio 1.0 (.ra version 3) only has one FourCC value */
#define RA3_FOURCC MKTAG('l', 'p', 'c', 'J')

struct RMStream {
};

/* Demux context for RealAudio */
typedef struct RADemuxContext {
} RADemuxContext;

/* Demux context for RealMedia (audio+video) */
typedef struct RMDemuxContext {
} RMDemuxContext;

/* Return value > 0: bytes read.
 * Return value < 0: error.
 * Return value == 0: can't happen.
 */
static int ra_read_content_description_field(AVFormatContext *s, const char *desc)
{
    AVIOContext *acpb = s->pb;
    uint16_t len;
    uint8_t *val;
    len = avio_r8(acpb);
    val = av_mallocz(len + 1);
    if (!val)
        return AVERROR(ENOMEM);
    avio_read(acpb, val, len);
    av_dict_set(&s->metadata, desc, val, 0);
    av_free(val);
    return len + 1; /* +1 due to reading one byte representing length */
}

/* A RealAudio 1.0 (.ra version 3) content description has 4 fields,
 * and differs in several ways from an RMF CONT header.
 */
static int ra_read_content_description(AVFormatContext *s)
{
    int sought = 0;
    int bytes_read_or_error;

    bytes_read_or_error = ra_read_content_description_field(s, "title");
    if (bytes_read_or_error < 0)
        return bytes_read_or_error;
    else
        sought += bytes_read_or_error;
    bytes_read_or_error = ra_read_content_description_field(s, "author");
    if (bytes_read_or_error < 0)
        return bytes_read_or_error;
    else
        sought += bytes_read_or_error;
    bytes_read_or_error = ra_read_content_description_field(s, "copyright");
    if (bytes_read_or_error < 0)
        return bytes_read_or_error;
    else
        sought += bytes_read_or_error;
    bytes_read_or_error = ra_read_content_description_field(s, "comment");
    if (bytes_read_or_error < 0)
        return bytes_read_or_error;
    else
        sought += bytes_read_or_error;

    return sought;
}


static int ra_probe(AVProbeData *p)
{
    /* RealAudio header; for RMF, use rm_probe. */
    uint8_t version;
    if (MKTAG(p->buf[0], p->buf[1], p->buf[2], p->buf[3]) != RA_HEADER)
       return 0;
    version = p->buf[5];
    /* Only v3 is currently supported, but v3-v5 should be.*/
    if ((version < 3) || (version > 5))
        return 0;
    return AVPROBE_SCORE_MAX;
}

static int ra_read_header(AVFormatContext *s)
{
    AVIOContext *acpb = s->pb;
    AVStream *st = NULL;

    int content_description_size, header_bytes_read;
    const int fourcc_bytes = 6;
    uint32_t tag, fourcc_tag;
    uint16_t version, header_size;
    uint8_t fourcc_len;

    /* Do a Little-Endian read here, unlike everywhere else. */
    tag = avio_rl32(acpb);
    if (tag != RA_HEADER) {
        av_log(s, AV_LOG_ERROR,
               "RealAudio: bad magic %"PRIx32", expected %"PRIx32".\n",
               tag, RA_HEADER);
        return AVERROR_INVALIDDATA;
    }
    version = avio_rb16(acpb);
    if (version != 3) { /* TODO: add v4 support */
        av_log(s, AV_LOG_ERROR, "RealAudio: Unsupported version %"PRIx32"\n", version);
        return AVERROR_INVALIDDATA;
    }
    header_size = avio_rb16(acpb); /* Excluding bytes until now */

    avio_skip(acpb, 10); /* unknown */
    avio_skip(acpb, 4); /* Data size: currently unused by this code */
    header_bytes_read = 14; /* Header bytes read since the header_size field */

    content_description_size = ra_read_content_description(s);
    if (content_description_size < 0) {
        av_log(s, AV_LOG_ERROR, "RealAudio: error reading header metadata.\n");
        av_dict_free(&s->metadata);
        return content_description_size; /* Preserve the error code */
    }
    header_bytes_read += content_description_size;

    /* An unknown byte, followed by FourCC data, is optionally present */
    if (header_bytes_read != header_size) { /* Looks like there is FourCC data */
        avio_skip(acpb, 1); /* Unknown byte */
        fourcc_len = avio_r8(acpb);
        if (fourcc_len != 4) {
            av_log(s, AV_LOG_ERROR,
                   "RealAudio: Unexpected FourCC length %"PRIu8", expected 4.\n",
                   fourcc_len);
            return AVERROR_INVALIDDATA;
        }
        fourcc_tag = avio_rb32(acpb);
        if (fourcc_tag != RA3_FOURCC) {
             av_log(s, AV_LOG_ERROR,
                    "RealAudio: Unexpected FourCC data %"PRIu32", expected %"PRIu32".\n",
                    fourcc_tag, RA3_FOURCC);
            return AVERROR_INVALIDDATA;
        }
        header_bytes_read += fourcc_bytes;
        if (header_bytes_read != header_size) {
            av_log(s, AV_LOG_ERROR,
                "RealAudio: read %"PRIu32" header bytes, expected %"PRIu16".\n",
                header_bytes_read, header_size);
            return AVERROR_INVALIDDATA;
        }
    }

    /* Reading all the header data has gone ok; initialiaze codec info. */
    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    st->codec->channel_layout = AV_CH_LAYOUT_MONO;
    st->codec->channels       = 1;
    st->codec->codec_id       = AV_CODEC_ID_RA_144;
    st->codec->codec_type     = AVMEDIA_TYPE_AUDIO;
    st->codec->sample_rate    = 8000;

    return 0;
}

static int ra_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    return av_get_packet(s->pb, pkt, RA144_PKT_SIZE);
}

static int rm_probe(AVProbeData *p)
{
    return 0;
}

static int rm_read_header(AVFormatContext *s)
{
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


int ff_rm_parse_packet(AVFormatContext *s, AVIOContext *pb,
                       AVStream *st, RMStream *ast, int len, AVPacket *pkt,
                       int *seq, int flags, int64_t timestamp)
{
    return 0;
}

int ff_rm_retrieve_cache(AVFormatContext *s, AVIOContext *pb,
                         AVStream *st, RMStream *ast, AVPacket *pkt)
{
    return 0;
}

RMStream *ff_rm_alloc_rmstream (void)
{
    return av_mallocz(sizeof(RMStream));
}

int ff_rm_read_mdpr_codecdata(AVFormatContext *s, AVIOContext *pb,
                              AVStream *st, RMStream *rst, int codec_data_size)
{
    return 0;
}

void ff_rm_free_rmstream (RMStream *rms)
{
    if (rms)
        av_freep(&rms);
}


AVInputFormat ff_ra_demuxer = {
    .name           = "ra",
    .long_name      = NULL_IF_CONFIG_SMALL("RealAudio"),
    .priv_data_size = sizeof(RADemuxContext),
    .read_probe     = ra_probe,
    .read_header    = ra_read_header,
    .read_packet    = ra_read_packet,
};

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

