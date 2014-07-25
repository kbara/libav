/*
 * RealAudio Demuxer, reimplemented for OPW, summer 2014.
 * Copyright (c) 2014 Katerina Barone-Adesi
 * Copyright (c) 2000, 2001 Fabrice Bellard
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
 * This is largely a from-scratch rewrite, but borrows RAv4 interleaving
 * from the old implementation; it is not well-documented.
 *
 * Naming convention:
 * ra_ = RealAudio
 * rm_ = RealMedia (potentially with video)
 * real_ = both.
 */

/* Numbers are big-endian, while strings are most naturally seen as
 * little-endian in these formats.
 */

#include <inttypes.h>

#include "libavutil/channel_layout.h"
#include "libavutil/intreadwrite.h"

#include "avformat.h"
#include "rm.h"
#include "rmsipr.h"

/* Header for RealAudio 1.0 (.ra version 3
 * and RealAudio 2.0 file (.ra version 4). */
#define RA_HEADER MKTAG('.', 'r', 'a', '\xfd')
/* Header for RealMedia files */
#define RM_HEADER MKTAG('.', 'R', 'M', 'F')
/* Headers for sections of RealMedia files */
#define RM_PROP_HEADER MKTAG('P', 'R', 'O', 'P')
#define RM_MDPR_HEADER MKTAG('M', 'D', 'P', 'R')
#define RM_CONT_HEADER MKTAG('C', 'O', 'N', 'T')
#define RM_DATA_HEADER MKTAG('D', 'A', 'T', 'A')
#define RM_INDX_HEADER MKTAG('I', 'N', 'D', 'X')

/* The relevant VSELP format has 159-bit frames, stored in 20 bytes */
#define RA144_PKT_SIZE 20

/* RealAudio 1.0 (.ra version 3) only has one FourCC value */
#define RA3_FOURCC MKTAG('l', 'p', 'c', 'J')

/* An internal signature in RealAudio 2.0 (.ra version 4) headers */
#define RA4_SIGNATURE MKTAG('.', 'r', 'a', '4')

/* Deinterleaving constants from the old rmdec implementation */
#define DEINT_ID_GENR MKTAG('g', 'e', 'n', 'r') ///< interleaving for Cooker/ATRAC
#define DEINT_ID_INT0 MKTAG('I', 'n', 't', '0') ///< no interleaving needed
#define DEINT_ID_INT4 MKTAG('I', 'n', 't', '4') ///< interleaving for 28.8
#define DEINT_ID_SIPR MKTAG('s', 'i', 'p', 'r') ///< interleaving for Sipro
#define DEINT_ID_VBRF MKTAG('v', 'b', 'r', 'f') ///< VBR case for AAC
#define DEINT_ID_VBRS MKTAG('v', 'b', 'r', 's') ///< VBR case for AAC

/* pkt_size and len are signed ints because that's what functions
   such as avio_read take - it's questionable. */
typedef struct Interleaver {
    uint32_t tag;
    int (*get_packet)(AVFormatContext *s, int stream_num, AVPacket *pkt,
                      int pkt_size);
    int (*read_packet)(AVFormatContext *s, uint8_t *buf, int len);
} Interleaver;

typedef struct RA4Stream {
    uint8_t *pkt_contents;
    uint32_t coded_frame_size, interleaver_id, fourcc_tag;
    uint16_t codec_flavor, subpacket_h, frame_size, subpacket_size, sample_size;
} RA4Stream;

struct RMStream {
};

/* Demux context for RealAudio */
typedef struct RADemuxContext {
    int version;
    AVStream *avst;
    struct RA4Stream rast;
    int pending_audio_packets;
    int audio_packets_read;
} RADemuxContext;

typedef struct RAInRealMediaDemuxContext {
    RADemuxContext radc;
    Interleaver interleaver;
} RAInRealMediaDemuxContext;

/* RealMedia files have one Media Property header per stream */
typedef struct RMMediaProperties {
    uint16_t stream_number;
    uint32_t chunk_size, max_bitrate, avg_bitrate, largest_pkt, avg_pkt;
    uint32_t stream_start_offset, preroll, duration, type_specific_size;
    uint8_t desc_size, mime_size;
    uint8_t stream_desc[256];
    uint8_t mime_type[256];
    uint8_t *type_specific_data;
} RMMediaProperties;

/* RealMedia files have one or more DATA headers, which contain
 * the actual data. Empirically, it seems to be one in most files,
 * and > 1 in the multirate files which are available.
 */
typedef struct RMDataHeader {
    uint32_t data_chunk_size, num_data_packets, next_data_chunk_offset;
} RMDataHeader;

/* Demux context for RealMedia (audio+video).
   This includes information from the RMF and PROP headers,
   and pointers to the per-stream Media Properties headers. */
typedef struct RMDemuxContext {
    /* RMF header information */
    uint32_t header_chunk_size, num_headers;
    /* Information from the PROP header */
    uint32_t prop_chunk_size, prop_max_bitrate, prop_avg_bitrate;
    uint32_t prop_largest_pkt, prop_avg_pkt, prop_num_pkts;
    uint32_t prop_file_duration, suggested_ms_buffer;
    uint32_t first_indx_offset, first_data_offset;
    uint16_t num_streams, flags;
    /* Information related to the current packet */
    uint16_t cur_pkt_size, cur_stream_number, cur_pkt_version;
    int cur_pkt_start;
    uint32_t cur_timestamp_ms;
    RMDataHeader cur_data_header;
    RMMediaProperties **rmp; /* Indexed by stream number */
    uint8_t **pkt_caches; /* Indexed by stream number */
} RMDemuxContext;



static int ra_probe(AVProbeData *p)
{
    /* RealAudio header; for RMF, use rm_probe. */
    uint8_t version;
    if (MKTAG(p->buf[0], p->buf[1], p->buf[2], p->buf[3]) != RA_HEADER)
       return 0;
    version = p->buf[5];
    /* Only v3 is currently supported, but v3-v4 should be. Does RA v5 exist? */
    if ((version < 3) || (version > 4))
        return 0;
    return AVPROBE_SCORE_MAX;
}


/* TODO: fully implement this... logic from the old code*/
static int ra4_codec_specific_setup(enum AVCodecID codec_id, AVFormatContext *s, AVStream *st)
{
    RADemuxContext *ra = s->priv_data;
    RA4Stream *rast = &(ra->rast);

    if (codec_id == AV_CODEC_ID_RA_288) {
        st->codec->block_align = rast->coded_frame_size;
    }
    /* TODO FIXME: handle other formats here */

    return 0;
}

/* This is taken almost verbatim from the old code */
/* TODO: get it reviewed */
static int ra4_sanity_check_headers(uint32_t interleaver_id, RA4Stream *rast, AVStream *st)
{
    if (rast->interleaver_id == DEINT_ID_INT4) {
        if (st->codec->block_align <= 0)
            return AVERROR_INVALIDDATA;
        /* The following test is clearly bogus
        if (rast->frame_size * rast->subpacket_h > (unsigned)INT_MAX)
            return AVERROR_INVALIDDATA; */
        if (rast->frame_size * rast->subpacket_h < st->codec->block_align)
            return AVERROR_INVALIDDATA;
    }

    switch(interleaver_id) {
    case DEINT_ID_INT4:
        if (rast->coded_frame_size > rast->frame_size)
            return AVERROR_INVALIDDATA;
        if (rast->subpacket_h <= 1)
            return AVERROR_INVALIDDATA;
        if (rast->coded_frame_size * rast->subpacket_h >
                (2 + (rast->subpacket_h & 1)) * rast->frame_size)
            return AVERROR_INVALIDDATA;
        break;
    case DEINT_ID_GENR: /* FIXME: untested */
        if (rast->subpacket_size <= 0)
            return AVERROR_INVALIDDATA;
        if (rast->subpacket_size > rast->frame_size)
            return AVERROR_INVALIDDATA;
        break;
    case DEINT_ID_SIPR:
    case DEINT_ID_INT0:
    case DEINT_ID_VBRS:
    case DEINT_ID_VBRF:
        break;
    default:
        av_log(NULL, 0 ,"Unknown interleaver %"PRIX32"\n",
               rast->interleaver_id);
        return AVERROR_INVALIDDATA;
    }
    return 0;
}

static int real_read_content_description_field(AVFormatContext *s,
                                             const char *desc,
                                             int length_bytes)
{
    AVIOContext *acpb = s->pb;
    uint16_t len;
    uint8_t *val;

    if (length_bytes == 1)
        len = avio_r8(acpb);
    else // 2
        len = avio_rb16(acpb);
    val = av_mallocz(len + 1);
    if (!val)
        return AVERROR(ENOMEM);
    avio_read(acpb, val, len);
    av_dict_set(&s->metadata, desc, val, 0);
    av_free(val);
    return 0;
}


static int real_read_content_description(AVFormatContext *s, int length_bytes)
{
    int bytes_read_or_error;
    int lb = length_bytes;

    bytes_read_or_error = real_read_content_description_field(s, "title", lb);
    if (bytes_read_or_error < 0)
        return bytes_read_or_error;
    bytes_read_or_error = real_read_content_description_field(s, "author", lb);
    if (bytes_read_or_error < 0)
        return bytes_read_or_error;
    bytes_read_or_error = real_read_content_description_field(s,
                                                              "copyright",
                                                              lb);
    if (bytes_read_or_error < 0)
        return bytes_read_or_error;
    bytes_read_or_error = real_read_content_description_field(s, "comment", lb);
    if (bytes_read_or_error < 0)
        return bytes_read_or_error;

    return 0;
}

static int ra_read_content_description(AVFormatContext *s)
{
    return real_read_content_description(s, 1);
}

static int get_fourcc(AVFormatContext *s, uint32_t *fourcc)
{
    uint8_t fourcc_len;

    fourcc_len = avio_r8(s->pb);
    if (fourcc_len != 4) {
        av_log(s, AV_LOG_ERROR,
               "RealAudio: Unexpected FourCC length %"PRIu8", expected 4.\n",
               fourcc_len);
        return AVERROR_INVALIDDATA;
    }

    *fourcc = avio_rl32(s->pb);
    return 0;
}

static int ra_read_header_v3(AVFormatContext *s, uint16_t header_size,
                             RADemuxContext *ra, AVStream *st)
{
    AVIOContext *acpb = s->pb;

    int content_description_size, is_fourcc_ok, start_pos;
    uint32_t fourcc_tag, header_bytes_read;

    start_pos = avio_tell(acpb);
    avio_skip(acpb, 10); /* unknown */
    avio_skip(acpb, 4); /* Data size: currently unused by this code */

    content_description_size = ra_read_content_description(s);
    if (content_description_size < 0) {
        av_log(s, AV_LOG_ERROR, "RealAudio: error reading header metadata.\n");
        return content_description_size; /* Preserve the error code */
    }

    header_bytes_read = avio_tell(acpb) - start_pos;
    /* An unknown byte, followed by FourCC data, is optionally present */
    if (header_bytes_read != header_size) { /* Looks like there is FourCC data */
        avio_skip(acpb, 1); /* Unknown byte */
        is_fourcc_ok = get_fourcc(s, &fourcc_tag);
        if (is_fourcc_ok < 0)
            return is_fourcc_ok; /* Preserve the error code */
        if (fourcc_tag != RA3_FOURCC) {
             av_log(s, AV_LOG_ERROR,
                    "RealAudio: Unexpected FourCC data %"PRIu32", expected %"PRIu32".\n",
                    fourcc_tag, RA3_FOURCC);
            return AVERROR_INVALIDDATA;
        }
        header_bytes_read = avio_tell(acpb) - start_pos;
        if (header_bytes_read != header_size) {
            av_log(s, AV_LOG_ERROR,
                "RealAudio: read %"PRIu32" header bytes, expected %"PRIu16".\n",
                header_bytes_read, header_size);
            return AVERROR_INVALIDDATA;
        }
    }

    /* Reading all the header data has gone ok; initialiaze codec info. */
    ra->avst = st;
    st->codec->channel_layout = AV_CH_LAYOUT_MONO;
    st->codec->channels       = 1;
    st->codec->codec_id       = AV_CODEC_ID_RA_144;
    st->codec->codec_type     = AVMEDIA_TYPE_AUDIO;
    st->codec->sample_rate    = 8000;

    return 0;
}

static int ra_read_header_v4(AVFormatContext *s, uint16_t header_size,
                             RADemuxContext *ra, AVStream *st)
{
    AVIOContext *acpb = s->pb;
    RA4Stream *rast = &(ra->rast);

    int content_description_size, is_fourcc_ok;
    uint32_t ra4_signature, variable_data_size, variable_header_size;
    uint32_t interleaver_id;
    uint16_t version2;
    uint8_t interleaver_id_len;

    ra->avst = st;

    ra4_signature = avio_rl32(acpb);
    if (ra4_signature != RA4_SIGNATURE) {
        av_log(s, AV_LOG_ERROR,
               "RealAudio: bad ra4 signature %"PRIx32", expected %"PRIx32".\n",
               ra4_signature, RA4_SIGNATURE);
        return AVERROR_INVALIDDATA;
    }

    /* Data size - 0x27 (the fixed-length part) */
    variable_data_size = avio_rb32(acpb);
    printf("Data size (non-fixed): %x\n", variable_data_size);


    version2 = avio_rb16(acpb);
    if (version2 != 4) {
        av_log(s, AV_LOG_ERROR,
               "RealAudio: Second version %"PRIx16", expected 4\n",
               version2);
        return AVERROR_INVALIDDATA;
    }

    /* Header size - 16 */
    variable_header_size = avio_rb32(acpb);
    printf("Header size (non-fixed): %x\n", variable_header_size);


    rast->codec_flavor = avio_rb16(acpb); /* TODO: use this? */
    printf("Got codec flavor %"PRIx16"\n", rast->codec_flavor);

    rast->coded_frame_size = avio_rb32(acpb);
    avio_skip(acpb, 12); /* Unknown */
    rast->subpacket_h = avio_rb16(acpb);
    st->codec->block_align = rast->frame_size = avio_rb16(acpb);
    rast->subpacket_size = avio_rb16(acpb);
    avio_skip(acpb, 2); /* Unknown */
    st->codec->sample_rate = avio_rb16(acpb);
    avio_skip(acpb, 2); /* Unknown */
    rast->sample_size = avio_rb16(acpb);
    st->codec->channels = avio_rb16(acpb);

    printf("Coded frame size: %x\n", rast->coded_frame_size);
    printf("Subpacket_h: %x\n", rast->subpacket_h);
    printf("Frame size: %x\n", rast->frame_size);
    printf("Subpacket size: %x\n", rast->subpacket_size);
    printf("Sample rate: %x\n", st->codec->sample_rate);
    printf("Sample size: %x\n", rast->sample_size);

    interleaver_id_len = avio_r8(acpb);
    if (interleaver_id_len != 4) {
        av_log(s, AV_LOG_ERROR,
               "RealAudio: Unexpected interleaver ID length %"PRIu8", expected 4.\n",
               interleaver_id_len);
        return AVERROR_INVALIDDATA;
    }
    rast->interleaver_id = interleaver_id = avio_rl32(acpb);
    printf("Interleaver: %c%c%c%c\n", interleaver_id >> 24,
           (interleaver_id >> 16) & 0xff, (interleaver_id >> 8) & 0xff,
           interleaver_id & 0xff);

    is_fourcc_ok = get_fourcc(s, &(rast->fourcc_tag));
    if (is_fourcc_ok < 0)
        return is_fourcc_ok; /* Preserve the error code */
    st->codec->codec_tag = rast->fourcc_tag;
    /*printf("Fourcc: %c%c%c%c\n", fourcc_tag >> 24,
           (fourcc_tag >> 16) & 0xff, (fourcc_tag >> 8) & 0xff,
           fourcc_tag & 0xff); TODO */
    avio_skip(acpb, 3); /* Unknown */

    content_description_size = ra_read_content_description(s);
    if (content_description_size < 0) {
        av_log(s, AV_LOG_ERROR, "RealAudio: error reading header metadata.\n");
        return content_description_size; /* Preserve the error code */
    }

    /* TODO: only RAv3 handling sets a channel layout - is that correct? */
    st->codec->codec_id       = ff_codec_get_id(ff_rm_codec_tags,
                                                st->codec->codec_tag);
    st->codec->codec_type     = AVMEDIA_TYPE_AUDIO;
    printf("Codec id %x\n", st->codec->codec_id);

    ra4_codec_specific_setup(st->codec->codec_id, s, st);

    return ra4_sanity_check_headers(rast->interleaver_id, rast, st);
}

/* This is called by ra_read_header, as well as for embedded
 * RealAudio streams within RealMedia files */
static int ra_read_header_with(AVFormatContext *s, RADemuxContext *ra,
                               AVStream *st)
{
    AVIOContext *acpb = s->pb;
    uint16_t version, header_size;

    version = avio_rb16(acpb);
    ra->version = version;
    header_size = avio_rb16(acpb); /* Excluding bytes until now */

    if (version == 3)
        return ra_read_header_v3(s, header_size, ra, st);
    else if (version == 4)
        return ra_read_header_v4(s, header_size, ra, st);
    else {
        av_log(s, AV_LOG_ERROR, "RealAudio: Unsupported version %"PRIx16"\n", version);
        return AVERROR_INVALIDDATA;
    }
}

/* This handles pure RealAudio files */
static int ra_read_header(AVFormatContext *s)
{
    AVIOContext *acpb = s->pb;
    RADemuxContext *ra = s->priv_data;
    uint32_t tag;
    AVStream *st;

    tag = avio_rl32(acpb);
    if (tag != RA_HEADER) {
        av_log(s, AV_LOG_ERROR,
               "RealAudio: bad magic %"PRIx32", expected %"PRIx32".\n",
               tag, RA_HEADER);
        return AVERROR_INVALIDDATA;
    }

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    return ra_read_header_with(s, ra, st);
}

/* Exactly the same as in the old code */
static void ra_ac3_swap_bytes(AVStream *st, AVPacket *pkt)
{
    uint8_t *ptr;
    int j;

    if (st->codec->codec_id == AV_CODEC_ID_AC3) {
        ptr = pkt->data;
        for (j = 0; j < pkt->size; j+= 2) {
            FFSWAP(int, ptr[0], ptr[1]);
            ptr += 2;
        }
    }
}

static int ra_retrieve_cache(RADemuxContext *ra, AVStream *st, RA4Stream *rast,
                             AVPacket *pkt)
{
    /* This replaces the old ff_rm_retrieve_cache */
    // TODO: handle VBRF/VBRS
    /* The cache is a fraction of a megabyte; using memcpy
       rather than reference-counted buffers is reasonable. */
    if (ra->pending_audio_packets) {
        av_new_packet(pkt, st->codec->block_align);
        /*memcpy(pkt->data,
               rast->pkt_contents + st->codec->block_align *
                    (rast->subpacket_h * rast->frame_size /
                    st->codec->block_align - ra->pending_audio_packets),
                st->codec->block_align); */
        memcpy(pkt->data,
               rast->pkt_contents + st->codec->block_align *
                (ra->audio_packets_read - ra->pending_audio_packets),
               st->codec->block_align);
        /* TODO: are the next two lines necessary? */
        pkt->flags = 0; /* TODO: revisit this when using timestamps */
        pkt->stream_index = st->index;
        ra->pending_audio_packets--;
    }

    return 0;
}

/* This was part of ff_rm_parse_packet */
/* Warning: dealing with subpackets has been made local,
   and there is explicit looping; the control flow is different */
static int ra_read_interleaved_packets(AVFormatContext *s, AVPacket *pkt,
                                       RADemuxContext *ra)
{
    AVStream *st = ra->avst;
    RA4Stream *rast = &(ra->rast);
    int expected_packets, read_packets, read;
    size_t interleaved_buffer_size;

    /* There's data waiting around already; return that */
    if (ra->pending_audio_packets) {
        return ra_retrieve_cache(ra, st, rast, pkt);
    }

    /* Clear the stored packet counts, so there's no chance of having stale ones
       if an error occurs during this function. */
    ra->pending_audio_packets = 0;
    ra->audio_packets_read = 0;

    expected_packets = rast->subpacket_h * rast->frame_size /
        st->codec->block_align;
    read_packets = 0;
    interleaved_buffer_size = expected_packets * rast->coded_frame_size;

    if ((rast->interleaver_id == DEINT_ID_INT4) && (!(rast->pkt_contents))) {
        rast->pkt_contents = av_mallocz(interleaved_buffer_size);
        if (!rast->pkt_contents)
            return AVERROR(ENOMEM);
    }

    for (int subpkt_cnt = 0; subpkt_cnt < rast->subpacket_h; subpkt_cnt++) {
        if (rast->interleaver_id == DEINT_ID_INT4) {
            for (int cur_subpkt = 0;
                 cur_subpkt < rast->subpacket_h / 2;
                 cur_subpkt++) {
                read = avio_read(s->pb,
                                 rast->pkt_contents +
                                    cur_subpkt * 2 * rast->frame_size +
                                    subpkt_cnt * rast->coded_frame_size,
                                 rast->coded_frame_size);
                if (read > 0)
                    read_packets++;
                else {
                    ra->pending_audio_packets = read_packets;
                    return AVERROR_EOF;
                }
            }
        } else if (rast->interleaver_id == DEINT_ID_GENR) {
            printf("IMPLEMENT GENR!\n");
            return -1; /* TODO FIXME */
        } else {
            av_log(s, AV_LOG_ERROR,
                   "RealAudio: internal error, unexpected interleaver\n");
            return AVERROR_INVALIDDATA;
        }
    }
    ra->pending_audio_packets = FFMIN(expected_packets, read_packets);
    ra->audio_packets_read = read_packets;
    return ra_retrieve_cache(ra, st, rast, pkt);
}


static int ra_read_packet_with(AVFormatContext *s, AVPacket *pkt, RADemuxContext *ra)
{
    RA4Stream *rast = &(ra->rast);
    AVStream *st = ra->avst;
    int len, get_pkt;

    if (ra->version == 3)
        return av_get_packet(s->pb, pkt, RA144_PKT_SIZE);

    /* Nope, it's version 4, and a bit more complicated */
    len = (rast->coded_frame_size * rast->subpacket_h) / 2;

    if (rast->interleaver_id == DEINT_ID_INT4) {
        return ra_read_interleaved_packets(s, pkt, ra);
    /* Simple case: no interleaving */
    } else if (rast->interleaver_id == DEINT_ID_INT0) {
        if (st->codec->codec_id == AV_CODEC_ID_AC3)
            len *= 2;
        get_pkt = av_get_packet(s->pb, pkt, len);
        /* Swap the bytes iff it's ac3 - check done in rm_ac3_swap_bytes */
        ra_ac3_swap_bytes(st, pkt);
        return get_pkt;
    } else if ((rast->interleaver_id == DEINT_ID_VBRF) ||
        (rast->interleaver_id == DEINT_ID_VBRS)) {
        /* TODO FIXME implement this */
        av_log(s, AV_LOG_ERROR, "RealAudio: VBR* unimplemented.\n");
        return AVERROR_INVALIDDATA;
    } else {
        av_log(s, AV_LOG_ERROR,
            "RealAudio: internal error, unknown interleaver\n");
        return AVERROR_INVALIDDATA;
    }
}

static int ra_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    return ra_read_packet_with(s, pkt, s->priv_data);
}

static int ra_read_close(AVFormatContext *s)
{
    RADemuxContext *ra = s->priv_data;
    RA4Stream *rast = &(ra->rast);

    av_freep(&(rast->pkt_contents));
    return 0;
}

/* The header should start with .RMF, and file and chunk version 0 */
static int rm_probe(AVProbeData *p)
{
    if (MKTAG(p->buf[0], p->buf[1], p->buf[2], p->buf[3]) != RM_HEADER)
        return 0;
    /* The dword chunk size is only non-zero in byte 8 in all known samples */
    if ((p->buf[4] != 0) || (p->buf[5] != 0) || (p->buf[6] != 0))
        return 0;
    /* The word chunk version is always zero */
    if ((p->buf[8] != 0) || (p->buf[9]) != 0)
        return 0;
    /* The dword file version is always zero */
    if ((p->buf[10] != 0) || (p->buf[11] != 0) ||
        (p->buf[12] != 0) || (p->buf[13]) != 0)
        return 0;
    /* It seems to be a RealMedia file */
    return AVPROBE_SCORE_MAX;
}

static int rm_read_media_properties_header(AVFormatContext *s,
                                           RMMediaProperties *rmmp)
{
    AVIOContext *acpb = s->pb;
    RMDemuxContext *rm = s->priv_data;
    AVStream *st;
    uint32_t mdpr_tag, content_tag, before_embed, after_embed;
    uint16_t chunk_version;
    int bytes_read, header_ret, fix_offset;

    mdpr_tag = avio_rl32(acpb);
    if (mdpr_tag != RM_MDPR_HEADER) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: got %"PRIx32", but expected an MDPR section.\n",
               mdpr_tag);
        return AVERROR_INVALIDDATA;
    }

    rmmp->chunk_size = avio_rb32(acpb);

    chunk_version = avio_rb16(acpb);
    if (chunk_version != 0) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: expected MDPR chunk version 0, got %"PRIx16".\n",
               chunk_version);
        return AVERROR_INVALIDDATA;
    }

    rmmp->stream_number       = avio_rb16(acpb);
    rmmp->max_bitrate         = avio_rb32(acpb);
    rmmp->avg_bitrate         = avio_rb32(acpb);
    rmmp->largest_pkt         = avio_rb32(acpb);
    rmmp->avg_pkt             = avio_rb32(acpb);
    rmmp->stream_start_offset = avio_rb32(acpb);
    rmmp->preroll             = avio_rb32(acpb);
    rmmp->duration            = avio_rb32(acpb);
    rmmp->desc_size           = avio_r8(acpb);

    /* RealMedia Stream numbers are zero-indexed */
    if (rmmp->stream_number >= rm->num_streams) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: Invalid stream %"PRIu32": max is %"PRIu32".\n",
               rmmp->stream_number, rm->num_streams - 1);
        return AVERROR_INVALIDDATA;
    }

    st = s->streams[rmmp->stream_number];
    st->id = rmmp->stream_number;
    st->start_time = rmmp->stream_start_offset;
    st->duration = rmmp->duration;
    st->codec->codec_type = AVMEDIA_TYPE_DATA;

    bytes_read = avio_read(acpb, rmmp->stream_desc, rmmp->desc_size);
    if (bytes_read < rmmp->desc_size) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: failed to read stream description.\n");
        return bytes_read;
    }

    rmmp->mime_size = avio_r8(acpb);
    bytes_read = avio_read(acpb, rmmp->mime_type, rmmp->mime_size);
    if (bytes_read < rmmp->mime_size) {
        av_log(s, AV_LOG_ERROR, "RealMedia: failed to read mime type.\n");
        return bytes_read;
    }

    rmmp->type_specific_size = avio_rb32(acpb);
    /*
    if (rmmp->type_specific_size > 0) {
        rmmp->type_specific_data = av_mallocz(rmmp->type_specific_size);
        if (!rmmp->type_specific_data) {
            av_log(s, AV_LOG_ERROR,
                   "RealMedia: failed to allocate type-specific memory.\n");
            return AVERROR(ENOMEM);
        }
        bytes_read = avio_read(acpb,
                               rmmp->type_specific_data,
                               rmmp->type_specific_size);
        if (bytes_read < rmmp->type_specific_size) {
            av_log(s, AV_LOG_ERROR,
                   "RealMedia: failed to read type-specific data.\n");
            return bytes_read;
        }
    } else
        rmmp->type_specific_data = NULL;
    */
    before_embed = avio_tell(acpb);

    content_tag = avio_rl32(acpb);
    if (content_tag == RA_HEADER) {
        RAInRealMediaDemuxContext *rarmdc;
        st->codec->codec_type = AVMEDIA_TYPE_AUDIO;
        st->priv_data = av_mallocz(sizeof(RAInRealMediaDemuxContext));
        if (!st->priv_data)
            return AVERROR(ENOMEM);
        rarmdc = st->priv_data;
        header_ret = ra_read_header_with(s, &(rarmdc->radc), st);
    } else {
        printf("Implement me\n");
        header_ret = -1; /* FIXME */
    }

    after_embed = avio_tell(acpb);
    if (after_embed != (rmmp->type_specific_size + before_embed)) {
        fix_offset = (rmmp->type_specific_size + before_embed) - after_embed;
        av_log(s, AV_LOG_WARNING,
               "RealMedia: ended up in the wrong place reading MDPR "
               "type-specific data, attempting to recover.\n");
        avio_seek(acpb, fix_offset, SEEK_CUR);
    }

    return header_ret;
}

/* TODO: share code with rm_read_data_header? */
static int rm_read_cont_header(AVFormatContext *s)
{
    AVIOContext *acpb = s->pb;
    uint32_t cont_tag;
    uint16_t chunk_version;

    cont_tag = avio_rl32(acpb);
    if (cont_tag != RM_CONT_HEADER) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: got %"PRIx32", but expected a CONT section.\n",
               cont_tag);
        return AVERROR_INVALIDDATA;
    }

    avio_rb32(acpb); /* Chunk size */

    chunk_version = avio_rb16(acpb);
    if (chunk_version != 0) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: expected CONT chunk version 0, got %"PRIx16".\n",
               chunk_version);
        return AVERROR_INVALIDDATA;
    }

    return real_read_content_description(s, 2);
}

static int rm_read_data_header(AVFormatContext *s, RMDataHeader *rmdh)
{
    AVIOContext *acpb = s->pb;
    uint32_t data_tag;
    uint16_t chunk_version;

    data_tag = avio_rl32(acpb);
    if (data_tag != RM_DATA_HEADER) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: got %"PRIx32", but expected a DATA section.\n",
               data_tag);
        return AVERROR_INVALIDDATA;
    }

    rmdh->data_chunk_size = avio_rb32(acpb);

    chunk_version = avio_rb16(acpb);
    if (chunk_version != 0) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: expected DATA chunk version 0, got %"PRIx16".\n",
               chunk_version);
        return AVERROR_INVALIDDATA;
    }

    rmdh->num_data_packets       = avio_rb32(acpb);
    rmdh->next_data_chunk_offset = avio_rb32(acpb);

    return 0;
}

static int initialize_streams(AVFormatContext *s, int num_streams)
{
    int i;
    AVStream *st;

    for (i = 0; i < num_streams; i++) {
        st = avformat_new_stream(s, NULL);
        if (!st)
            return AVERROR(ENOMEM);
    }
    return 0;
}

static void free_media_properties(RMDemuxContext *rm, int num_streams)
{
    int i;

    for (i = 0; i < num_streams; i++)
        av_free(rm->rmp[i]);

    av_free(rm->rmp);
}

static int initialize_media_properties(RMDemuxContext *rm)
{
    int i;

    rm->rmp = av_malloc(rm->num_streams * sizeof(RMMediaProperties *));
    if (!rm->rmp)
        return AVERROR(ENOMEM);

    for (i = 0; i < rm->num_streams; i++) {
        rm->rmp[i] = av_mallocz(sizeof(RMMediaProperties));
        if (!rm->rmp[i]) {
            free_media_properties(rm, i);
            return AVERROR(ENOMEM);
        }
    }
    return 0;
}

/* TODO: find the sample with rmf chunk size = 10 and a *word* file version */
static int rm_read_header(AVFormatContext *s)
{
    AVIOContext *acpb = s->pb;
    RMDemuxContext *rm = s->priv_data;
    uint32_t rm_tag, file_version, prop_tag, next_tag;
    uint16_t rm_chunk_version, prop_chunk_version;
    int header_ret, stream_init_ret, media_prop_init_ret;

    /* Read the RMF header */
    rm_tag = avio_rl32(acpb);
    if (rm_tag != RM_HEADER) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: bad magic %"PRIx32", expected %"PRIx32".\n",
               rm_tag, RM_HEADER);
        return AVERROR_INVALIDDATA;
    }

    rm->header_chunk_size = avio_rb32(acpb);

    rm_chunk_version = avio_rb16(acpb);
    if (rm_chunk_version != 0) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: expected RMF chunk version 0, got %"PRIx16".\n",
               rm_chunk_version);
        return AVERROR_INVALIDDATA;
    }

    file_version = avio_rb32(acpb);
    if (file_version != 0) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: expected file version 0, got %"PRIx32".\n",
               file_version);
        return AVERROR_INVALIDDATA;
    }

    rm->num_headers = avio_rb32(acpb);

    /* Read the PROP header */
    prop_tag = avio_rl32(acpb);
    if (prop_tag != RM_PROP_HEADER) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: got %"PRIx32", but expected a PROP section.\n",
               prop_tag);
        return AVERROR_INVALIDDATA;
    }

    /* 'Typically' 0x32 */
    rm->prop_chunk_size = avio_rb32(acpb);
    prop_chunk_version = avio_rb16(acpb);
    if (prop_chunk_version != 0) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: expected PROP chunk version 0, got %"PRIx16".\n",
               prop_chunk_version);
        return AVERROR_INVALIDDATA;
    }

    rm->prop_max_bitrate    = avio_rb32(acpb);
    rm->prop_avg_bitrate    = avio_rb32(acpb);
    rm->prop_largest_pkt    = avio_rb32(acpb);
    rm->prop_avg_pkt        = avio_rb32(acpb);
    rm->prop_num_pkts       = avio_rb32(acpb);
    rm->prop_file_duration  = avio_rb32(acpb);
    rm->suggested_ms_buffer = avio_rb32(acpb);
    rm->first_indx_offset   = avio_rb32(acpb);
    rm->first_data_offset   = avio_rb32(acpb);
    rm->num_streams         = avio_rb16(acpb);
    rm->flags               = avio_rb16(acpb);

    /* Initialize these before reading packets */
    rm->cur_pkt_start = 0;
    rm->cur_pkt_size  = 0;

    stream_init_ret = initialize_streams(s, rm->num_streams);
    if (stream_init_ret) /* Initializing streams failed */
        return stream_init_ret; /* Preserve why */

    media_prop_init_ret = initialize_media_properties(rm);
    if (media_prop_init_ret)
        return media_prop_init_ret;

    /* Read the tags; return 0 on success, !0 early on failure */
    for (;;) {
        next_tag = avio_rl32(acpb);
        printf("next tag: %x\n", next_tag);
        /* FIXME: the tag is being preserved as an extra check */
        avio_seek(acpb, -4, SEEK_CUR); /* REMOVE THIS */
        switch(next_tag) {
        case RM_CONT_HEADER:
            printf("cont\n");
            header_ret = rm_read_cont_header(s);
            if (header_ret)
                return header_ret;
            break;
        case RM_MDPR_HEADER:
            printf("mdpr\n");
            header_ret = rm_read_media_properties_header(s, rm->rmp[0]);
            if (header_ret)
                return header_ret;
            break;
        case RM_DATA_HEADER:
            printf("data\n");
            header_ret = rm_read_data_header(s, &(rm->cur_data_header));
            if (header_ret)
                return header_ret;
            break;
        default:
            /* There's not another tag right away */
            //avio_seek(acpb, -4, SEEK_CUR);
            printf("Done with headers\n");
            return 0;
        }
    }
}

static int rm_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    RMDemuxContext *rm = s->priv_data;
    AVIOContext *acpb  = s->pb;
    AVStream *st;
    int cur_pos = avio_tell(acpb);
    //uint8_t packet_group, flags;

    /* Is this in the middle of an RM-level data packet? */
    if (rm->cur_pkt_start + rm->cur_pkt_size <= cur_pos) {
        rm->cur_pkt_start      = cur_pos;
        rm->cur_pkt_version    = avio_rb16(acpb);
        rm->cur_pkt_size       = avio_rb16(acpb);
        rm->cur_stream_number  = avio_rb16(acpb);
        rm->cur_timestamp_ms   = avio_rb32(acpb);

        printf("Stream number: %x\n", rm->cur_stream_number);
        if (rm->cur_stream_number >= rm->num_streams) {
            av_log(s, AV_LOG_ERROR,
                    "RealMedia: Invalid stream %"PRIu32": max is %"PRIu32".\n",
                    rm->cur_stream_number, rm->num_streams - 1);
            return AVERROR_INVALIDDATA;
        }
        if (rm->cur_pkt_version == 0) {
            avio_r8(acpb); /* packet_group */
            avio_r8(acpb); /* flags */
        }  else if (rm->cur_pkt_version == 1) {
            printf("Implement me.\n"); /* TODO FIXME */
            return AVERROR_INVALIDDATA;
        } else {
            av_log(s, AV_LOG_ERROR,
                   "RealMedia: Send sample. Unknown packet_version %"PRIx16".\n",
                   rm->cur_pkt_version);
            return AVERROR_INVALIDDATA;
        }
    }

    st = s->streams[rm->cur_stream_number];

    if (st->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
        /* TODO: make this conditional on it being RA... */
        return ra_read_packet_with(s, pkt, st->priv_data);
    } else { /* FIXME TODO */
        printf("Implement non-audio stream support\n");
        return -1;
    }
}

static int rm_read_close(AVFormatContext *s)
{
    RMDemuxContext *rm = s->priv_data;
    free_media_properties(rm, rm->num_streams);
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
    .read_close     = ra_read_close,
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

