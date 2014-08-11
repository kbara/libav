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
#include "libavutil/mathematics.h"

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

/* Subpacket type indicators: first 2 data bits */
#define RM_PARTIAL_FRAME      0 /* 00 */
#define RM_WHOLE_FRAME        1 /* 01 */
#define RM_LAST_PARTIAL_FRAME 2 /* 10 */
#define RM_MULTIPLE_FRAMES    3 /* 11 */

#define RM_MULTIFRAME_PENDING 0x1000 /* Arbitrary large positive number */

/* The relevant VSELP format has 159-bit frames, stored in 20 bytes */
#define RA144_PKT_SIZE 20

/* RealAudio 1.0 (.ra version 3) only has one FourCC value */
#define RA3_FOURCC MKTAG('l', 'p', 'c', 'J')

/* An internal signature in RealAudio 2.0 (.ra version 4) headers */
#define RA4_SIGNATURE MKTAG('.', 'r', 'a', '4')
#define RA5_SIGNATURE MKTAG('.', 'r', 'a', '5')

#define RM_VIDEO_TAG MKTAG('V', 'I', 'D', 'O')

/* Deinterleaving constants from the old rmdec implementation */
#define DEINT_ID_GENR MKTAG('g', 'e', 'n', 'r') ///< interleaving for Cooker/ATRAC
#define DEINT_ID_INT0 MKTAG('I', 'n', 't', '0') ///< no interleaving needed
#define DEINT_ID_INT4 MKTAG('I', 'n', 't', '4') ///< interleaving for 28.8
#define DEINT_ID_SIPR MKTAG('s', 'i', 'p', 'r') ///< interleaving for Sipro
#define DEINT_ID_VBRF MKTAG('v', 'b', 'r', 'f') ///< VBR case for AAC
#define DEINT_ID_VBRS MKTAG('v', 'b', 'r', 's') ///< VBR case for AAC


struct Interleaver;

typedef struct RAStream {
    uint32_t coded_frame_size, interleaver_id, fourcc_tag;
    uint16_t codec_flavor, subpacket_h, frame_size, subpacket_size, sample_size;
    int full_pkt_size, subpkt_size, subpacket_pp;
} RAStream;

typedef struct RealPacketCache {
    int pending_packets;
    int packets_read;
    uint8_t *pkt_buf;
    size_t buf_size;
    uint8_t *next_pkt_start;
    uint32_t next_offset;
} RealPacketCache;

/* Demux context for RealAudio */
typedef struct RADemuxContext {
    int version;
    struct RAStream rast;
    //int pending_audio_packets;
    //int audio_packets_read;
    RealPacketCache *rpc;
    AVStream *avst;
    struct Interleaver *interleaver;
    void *interleaver_state;
} RADemuxContext;

/* pkt_size and len are signed ints because that's what functions
   such as avio_read take - it's questionable. */
typedef struct Interleaver {
    uint32_t interleaver_tag;
    int (*get_packet)(AVFormatContext *s, AVPacket *pkt,
                      RADemuxContext *radc, int pkt_size);
    int (*postread_packet)(RADemuxContext *radc, int bytes_read);
    void *priv_data;
} Interleaver;

typedef struct Int4State {
    int subpkt_fs, subpkt_cfs;
} Int4State;

/* RealMedia files have one Media Property header per stream */
typedef struct RMMediaProperties {
    uint16_t stream_number;
    uint32_t chunk_size, max_bitrate, avg_bitrate, largest_pkt, avg_pkt;
    uint32_t stream_start_offset, preroll, duration, type_specific_size;
    uint8_t desc_size, mime_size;
    uint8_t stream_desc[256];
    uint8_t mime_type[256];
    uint8_t *type_specific_data;
    int is_realaudio; /* Not explicitly in the header, but useful */
} RMMediaProperties;

/* RealMedia files have one or more DATA headers, which contain
 * the actual data. Empirically, it seems to be one in most files,
 * and > 1 in the multirate files which are available.
 */
typedef struct RMDataHeader {
    uint32_t data_chunk_size, num_data_packets, next_data_chunk_offset;
} RMDataHeader;

typedef struct RMVidStream {
    int curpic_num, cur_slice, pkt_pos, videobuf_pos;
    int slices, videobuf_size;
    AVPacket pkt;
} RMVidStream;

/* Information about a particular RM stream, beyond what is described in
 * the relevant AVStream.
 */
typedef struct RMStream {
    int full_pkt_size, subpkt_size, fps;
    int subpacket_pp; /* Subpackets per full packet. */
    RMMediaProperties rmmp;
    RealPacketCache *rpc;
    RADemuxContext radc; /* Unused if not RA */
    //RMVidStream vst;
} RMStream;

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
    int64_t cur_pkt_start;
    uint32_t cur_timestamp_ms;
    RMDataHeader cur_data_header;
} RMDemuxContext;


/* Utility code */
static int rm_initialize_pkt_buf(RealPacketCache *rpc, int size)
{
    rpc->pkt_buf = av_mallocz(size);
    if (!rpc->pkt_buf)
        return AVERROR(ENOMEM);
    rpc->buf_size       = size;
    return 0;
}

static void rm_clear_rpc(RealPacketCache *rpc)
{
    av_free(rpc->pkt_buf);
    memset(rpc, '\0', sizeof(RealPacketCache));
}


/* Lightly modified from the old code. */
static int real_read_extradata(AVIOContext *pb, AVCodecContext *avctx,
                             uint32_t size)
{
    avctx->extradata = av_mallocz(size/* + FF_INPUT_BUFFER_PADDING_SIZE*/);
    if (!avctx->extradata)
        return AVERROR(ENOMEM);
    avctx->extradata_size = avio_read(pb, avctx->extradata, size);
    if (avctx->extradata_size != size) {
        av_freep(&avctx->extradata);
        if (avctx->extradata_size < 0)
            return avctx->extradata_size;
        else
            return AVERROR(EIO);
    }
    return 0;
}

/* Audio interleavers */

static int rm_postread_generic_packet(RADemuxContext *radc, int bytes_read)
{
    RealPacketCache *rpc = radc->rpc;
    RAStream *rast       = &(radc->rast);

    rpc->packets_read    = bytes_read / rast->full_pkt_size;
    rpc->pending_packets = rpc->packets_read;
    rpc->next_pkt_start  = rpc->pkt_buf;
    return 0;
}

/* TODO: if partial packets need to be implemented, the read needs to change.*/
static int rm_get_generic_packet(AVFormatContext *s, AVPacket *pkt,
                                 RADemuxContext *radc, int pkt_size)
{
    RealPacketCache *rpc = radc->rpc;
    AVStream *st         = radc->avst;

    if (!rpc->pending_packets) {
        av_log(s, AV_LOG_WARNING,
               "RealMedia: tried to retrieve non-pending packet.\n");
        return -1; /* No packet pending on this stream. */
    }
    if (rpc->pkt_buf + rpc->buf_size < rpc->next_pkt_start + pkt_size) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: tried to read too much in get_generic_packet.\n");
        rpc->pending_packets = 0;
        return -2; /* Attempt to read too much. */
    }

    if (av_new_packet(pkt, pkt_size) < 0)
        return AVERROR(ENOMEM);
    memcpy(pkt->data, rpc->next_pkt_start, pkt_size);
    pkt->stream_index = st->index;
    rpc->pending_packets--;
    if (rpc->pending_packets)
        rpc->next_pkt_start += pkt_size;
    return 0;
}

static int rm_postread_sipr_packet(RADemuxContext *radc, int bytes_read)
{
    RealPacketCache *rpc = radc->rpc;
    RAStream *rast = &(radc->rast);

    ff_rm_reorder_sipr_data(rpc->pkt_buf, rast->subpacket_h, rast->frame_size);
    rpc->packets_read    = bytes_read / rast->full_pkt_size;
    rpc->pending_packets = rpc->packets_read;
    rpc->next_pkt_start  = rpc->pkt_buf;

    return 0;
}

static int rm_postread_int4_packet(RADemuxContext *radc, int bytes_read)
{
    RealPacketCache *rpc = radc->rpc;
    RAStream *rast       = &(radc->rast);
    Int4State *int4state = radc->interleaver_state;

    rpc->packets_read    = bytes_read / rast->coded_frame_size;
    rpc->pending_packets = rpc->packets_read;

    /* Reset state */
    rpc->next_pkt_start  = rpc->pkt_buf;
    memset(int4state, '\0', sizeof(Int4State));
    return 0;
}

static int rm_get_int4_packet(AVFormatContext *s, AVPacket *pkt,
                              RADemuxContext *radc, int pkt_size)
{
    RAStream *rast       = &(radc->rast);
    RealPacketCache *rpc = radc->rpc;
    AVStream *st         = radc->avst;
    Int4State *int4state = radc->interleaver_state;
    uint8_t *pkt_start;

    assert(rast->coded_frame_size == pkt_size);
    pkt_start = rpc->next_pkt_start + rast->coded_frame_size *
                (int4state->subpkt_cfs * rast->subpacket_h / 2 +
                int4state->subpkt_fs);

    if (av_new_packet(pkt, rast->subpkt_size) < 0)
        return AVERROR(ENOMEM);
    memcpy(pkt->data, pkt_start, rast->subpkt_size);
    pkt->stream_index = st->index;
    rpc->pending_packets--;

    int4state->subpkt_cfs++;
    if (int4state->subpkt_cfs >= rast->subpacket_h) {
        int4state->subpkt_cfs = 0;
        int4state->subpkt_fs++;
        if (int4state->subpkt_fs >= rast->subpacket_h / 2)
            int4state->subpkt_fs = 0;
    }

    if ((int4state->subpkt_fs == 0) && (int4state->subpkt_cfs == 0)) {
        rpc->next_pkt_start += rast->full_pkt_size;
    }

    return 0;
}

Interleaver ra_interleavers[] = {
    {
        .interleaver_tag = 0,
        .get_packet      = rm_get_generic_packet,
        .postread_packet = rm_postread_generic_packet
    },
    {
        .interleaver_tag = DEINT_ID_INT4,
        .get_packet      = rm_get_int4_packet,
        .postread_packet = rm_postread_int4_packet
    },
    {
        .interleaver_tag = DEINT_ID_SIPR,
        .get_packet      = rm_get_generic_packet,
        .postread_packet = rm_postread_sipr_packet
    }
};

static int ra_interleaver_count = sizeof(ra_interleavers);

static Interleaver *ra_find_interleaver(uint32_t tag)
{
    for (int i = 0; i < ra_interleaver_count; i++)
        if (ra_interleavers[i].interleaver_tag == tag)
            return &ra_interleavers[i];
    return NULL;
}


/* RealAudio Demuxer */
static int ra_probe(AVProbeData *p)
{
    /* RealAudio header; for RMF, use rm_probe. */
    uint8_t version;
    if (MKTAG(p->buf[0], p->buf[1], p->buf[2], p->buf[3]) != RA_HEADER)
       return 0;
    version = p->buf[5];
    /* Only probe version 3 or 4. All known v5 samples are embedded in RM */
    if ((version < 3) || (version > 4))
        return 0;
    return AVPROBE_SCORE_MAX;
}

static int ra_interleaver_specific_setup(AVFormatContext *s, AVStream *st,
                                         RADemuxContext *radc)
{
    RAStream *rast = &(radc->rast);

    printf("rast->interleaver_id: %x\n", rast->interleaver_id);
    switch(rast->interleaver_id) {

    case DEINT_ID_INT4:
        /* Int4 is composed of several interleaved subpackets.
         * Calculate the size they all take together. */
        rast->subpacket_pp = rast->subpacket_h * rast->frame_size /
            st->codec->block_align;
        rast->full_pkt_size     = rast->subpacket_pp * rast->coded_frame_size;
        rast->subpkt_size       = rast->full_pkt_size / rast->subpacket_pp;
        radc->interleaver       = ra_find_interleaver(DEINT_ID_INT4);
        radc->interleaver_state = av_mallocz(sizeof(Int4State));
        if (!radc->interleaver_state)
            return AVERROR(ENOMEM);
        break;
    case DEINT_ID_INT0:
        rast->full_pkt_size = (rast->coded_frame_size * rast->subpacket_h) / 2;
        rast->subpkt_size   = rast->full_pkt_size;
        radc->interleaver   = ra_find_interleaver(0); /* Generic's enough */
        break;
    case DEINT_ID_SIPR:
        rast->subpkt_size   = st->codec->block_align;
        rast->full_pkt_size = rast->frame_size * rast->subpacket_h;
        rast->subpacket_pp  = rast->full_pkt_size / rast->subpkt_size;
        radc->interleaver   = ra_find_interleaver(DEINT_ID_SIPR);
        break;
    default:
            printf("Implement full_pkt_size for another interleaver.\n");
            return -1; /* FIXME TODO */
    }
    return 0;
}

/* TODO: fully implement this... */
static int ra4_codec_specific_setup(enum AVCodecID codec_id, AVFormatContext *s,
                                    AVStream *st, RADemuxContext *radc)
{
    RAStream *rast = &(radc->rast);
    int ret;

    rast->subpacket_pp = 1;

    switch(st->codec->codec_id) {
    uint32_t codecdata_length;

    case AV_CODEC_ID_RA_288:
        st->codec->block_align = rast->coded_frame_size;
        break;
    /*case AV_CODEC_ID_ATRAC3:
        avio_skip(s->pb, 3);
        if (radc->version == 5)
            avio_skip(s->pb, 1);
        codecdata_length = avio_rb32(s->pb);
        if ((ret = real_read_extradata(s->pb, st->codec, codecdata_length)) < 0)
            return ret;
        if (rast->subpacket_size == 0) { // it's unsigned
            av_log(s, AV_LOG_ERROR,
                   "RealMedia: ATRAC3 subpacket size must not be 0.\n");
            return AVERROR_INVALIDDATA;
        }
        st->codec->block_align = rast->subpacket_size;
        break;*/ // untested, etc
    case AV_CODEC_ID_SIPR:
        /*avio_skip(s->pb, 3);
        if (radc->version == 5)
            avio_skip(s->pb, 1);
        codecdata_length = avio_rb32(s->pb);*/
        if (rast->codec_flavor > 3) {
            av_log(s, AV_LOG_ERROR,
                   "RealMedia: SIPR file flavor %"PRIu16" too large\n",
                   rast->codec_flavor);
            return AVERROR_INVALIDDATA;
        }
        st->codec->block_align = ff_sipr_subpk_size[rast->codec_flavor];
        //if ((ret = real_read_extradata(s->pb, st->codec, codecdata_length)) < 0)
        //    return ret;
        break;
    case AV_CODEC_ID_AAC:
        printf("Put in AAC support\n");
        return AVERROR_INVALIDDATA; /* TODO */
        break;
    default:
        printf("Add support for another codec.\n"); /* TODO */
        return AVERROR_INVALIDDATA;
    }

    ret = ra_interleaver_specific_setup(s, st, radc);
    if (ret < 0)
        return ret;

    if (st->codec->codec_id == AV_CODEC_ID_AC3) {
        rast->full_pkt_size *= 2;
        rast->subpkt_size   *= 2;
    }

    if (rast->full_pkt_size == 0) {
        av_log(s, AV_LOG_ERROR, "RealMedia: full packet size must not be zero.\n");
        return AVERROR_INVALIDDATA;
    }
    return 0;
}

/* This is taken almost verbatim from the old code */
/* TODO: get it reviewed */
static int ra4_sanity_check_headers(uint32_t interleaver_id, RAStream *rast, AVStream *st)
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
    uint16_t len;
    uint8_t *val;

    if (length_bytes == 1)
        len = avio_r8(s->pb);
    else // 2
        len = avio_rb16(s->pb);
    val = av_mallocz(len + 1);
    if (!val)
        return AVERROR(ENOMEM);
    avio_read(s->pb, val, len);
    av_dict_set(&s->metadata, desc, val, 0);
    av_free(val);
    return 0;
}


static int real_read_content_description(AVFormatContext *s, int length_bytes)
{
    int read_ret;
    int lb = length_bytes;

    read_ret = real_read_content_description_field(s, "title", lb);
    if (read_ret < 0)
        return read_ret;
    read_ret = real_read_content_description_field(s, "author", lb);
    if (read_ret < 0)
        return read_ret;
    read_ret = real_read_content_description_field(s, "copyright", lb);
    if (read_ret < 0)
        return read_ret;
    read_ret = real_read_content_description_field(s, "comment", lb);
    if (read_ret < 0)
        return read_ret;

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
    RAStream   *rast = &(ra->rast);

    int content_description_size, is_fourcc_ok, start_pos;
    uint32_t fourcc_tag, header_bytes_read;

    start_pos = avio_tell(s->pb);
    avio_skip(s->pb, 10); /* unknown */
    avio_skip(s->pb, 4); /* Data size: currently unused by this code */

    content_description_size = ra_read_content_description(s);
    if (content_description_size < 0) {
        av_log(s, AV_LOG_ERROR, "RealAudio: error reading header metadata.\n");
        return content_description_size; /* Preserve the error code */
    }

    header_bytes_read = avio_tell(s->pb) - start_pos;
    /* An unknown byte, followed by FourCC data, is optionally present */
    if (header_bytes_read != header_size) { /* Looks like there is FourCC data */
        avio_skip(s->pb, 1); /* Unknown byte */
        is_fourcc_ok = get_fourcc(s, &fourcc_tag);
        if (is_fourcc_ok < 0)
            return is_fourcc_ok; /* Preserve the error code */
        if (fourcc_tag != RA3_FOURCC) {
             av_log(s, AV_LOG_ERROR,
                    "RealAudio: Unexpected FourCC data %"PRIu32", expected %"PRIu32".\n",
                    fourcc_tag, RA3_FOURCC);
            return AVERROR_INVALIDDATA;
        }
        header_bytes_read = avio_tell(s->pb) - start_pos;
        if (header_bytes_read != header_size) {
            av_log(s, AV_LOG_ERROR,
                "RealAudio: read %"PRIu32" header bytes, expected %"PRIu16".\n",
                header_bytes_read, header_size);
            return AVERROR_INVALIDDATA;
        }
    }


    /* Reading all the header data has gone ok; initialiaze codec info. */
    st->codec->channel_layout = AV_CH_LAYOUT_MONO;
    st->codec->channels       = 1;
    st->codec->codec_id       = AV_CODEC_ID_RA_144;
    st->codec->codec_type     = AVMEDIA_TYPE_AUDIO;
    st->codec->sample_rate    = 8000;

    ra->avst        = st;
    ra->interleaver = ra_find_interleaver(0);

    rast->subpacket_pp  = 1;
    rast->subpkt_size   = RA144_PKT_SIZE;
    rast->full_pkt_size = RA144_PKT_SIZE;
    return 0;
}

static int ra_read_header_v4_5(AVFormatContext *s, uint16_t header_size,
                             RADemuxContext *ra, AVStream *st, int32_t version)
{
    RAStream *rast = &(ra->rast);

    int content_description_size, is_fourcc_ok, expected_signature;
    uint32_t ra_signature, variable_data_size, variable_header_size;
    uint32_t interleaver_id;
    uint16_t version2;
    uint8_t interleaver_id_len;
    uint64_t start_pos, header_bytes_read;

    ra->avst = st;
    start_pos = avio_tell(s->pb);

    expected_signature = version == 4 ? RA4_SIGNATURE : RA5_SIGNATURE;
    ra_signature = avio_rl32(s->pb);
    if (ra_signature != expected_signature) {
        av_log(s, AV_LOG_ERROR,
               "RealAudio: bad ra signature %"PRIx32", expected %"PRIx32".\n",
               ra_signature, expected_signature);
        return AVERROR_INVALIDDATA;
    }

    /* Data size - 0x27 (the fixed-length part) */
    variable_data_size = avio_rb32(s->pb);
    printf("Data size (non-fixed): %x\n", variable_data_size);


    version2 = avio_rb16(s->pb);
    if (version2 != version) {
        av_log(s, AV_LOG_ERROR,
               "RealAudio: Second version %"PRIx16", expected %"PRIx32".\n",
               version2, version);
        return AVERROR_INVALIDDATA;
    }

    /* Header size - 16 */
    variable_header_size = avio_rb32(s->pb);
    printf("Header size (non-fixed): %x\n", variable_header_size);


    rast->codec_flavor = avio_rb16(s->pb); /* TODO: use this? */
    printf("Got codec flavor %"PRIx16"\n", rast->codec_flavor);

    rast->coded_frame_size = avio_rb32(s->pb);
    avio_skip(s->pb, 12); /* Unknown */
    rast->subpacket_h = avio_rb16(s->pb);
    st->codec->block_align = rast->frame_size = avio_rb16(s->pb);
    rast->subpacket_size = avio_rb16(s->pb);
    avio_skip(s->pb, 2); /* Unknown */
    st->codec->sample_rate = avio_rb16(s->pb);
    avio_skip(s->pb, 2); /* Unknown */
    if (version == 5)
        avio_skip(s->pb, 6); /* Unknown */
    rast->sample_size = avio_rb16(s->pb);
    st->codec->channels = avio_rb16(s->pb);

    printf("Coded frame size: %x\n", rast->coded_frame_size);
    printf("Subpacket_h: %x\n", rast->subpacket_h);
    printf("Frame size: %x\n", rast->frame_size);
    printf("Subpacket size: %x\n", rast->subpacket_size);
    printf("Sample rate: %x\n", st->codec->sample_rate);
    printf("Sample size: %x\n", rast->sample_size);

    if (version == 4) {
        interleaver_id_len = avio_r8(s->pb);
        if (interleaver_id_len != 4) {
            av_log(s, AV_LOG_ERROR,
                   "RealAudio: Unexpected interleaver ID length %"PRIu8", "
                   "expected 4.\n",
                interleaver_id_len);
            return AVERROR_INVALIDDATA;
        }
        rast->interleaver_id = interleaver_id = avio_rl32(s->pb);
        printf("Interleaver: %c%c%c%c\n", interleaver_id >> 24,
               (interleaver_id >> 16) & 0xff, (interleaver_id >> 8) & 0xff,
             interleaver_id & 0xff);

        is_fourcc_ok = get_fourcc(s, &(rast->fourcc_tag));
        if (is_fourcc_ok < 0)
            return is_fourcc_ok; /* Preserve the error code */
    } else if (version == 5) {
        rast->interleaver_id = interleaver_id = avio_rl32(s->pb);
        rast->fourcc_tag     = avio_rl32(s->pb);
    }
    st->codec->codec_tag = rast->fourcc_tag;
    /*printf("Fourcc: %c%c%c%c\n", fourcc_tag >> 24,
           (fourcc_tag >> 16) & 0xff, (fourcc_tag >> 8) & 0xff,
           fourcc_tag & 0xff); TODO */
    avio_skip(s->pb, 3); /* Unknown */
    if (version == 5)
        avio_skip(s->pb, 1); /* Unknown */

    content_description_size = ra_read_content_description(s);
    if (content_description_size < 0) {
        av_log(s, AV_LOG_ERROR, "RealAudio: error reading header metadata.\n");
        return content_description_size; /* Preserve the error code */
    }

    st->codec->codec_id       = ff_codec_get_id(ff_rm_codec_tags,
                                                st->codec->codec_tag);
    st->codec->codec_type     = AVMEDIA_TYPE_AUDIO;
    printf("Codec id %x\n", st->codec->codec_id);

    ra4_codec_specific_setup(st->codec->codec_id, s, st, ra);

    header_bytes_read = avio_tell(s->pb) - start_pos;
    if (header_size && (header_bytes_read != header_size)) {
        av_log(s, AV_LOG_WARNING,
               "RealAudio: read %"PRIx64" header bytes, expected %"PRIx32".\n",
               header_bytes_read, header_size);
        avio_seek(s->pb, header_size - header_bytes_read, SEEK_CUR);
    }

    return ra4_sanity_check_headers(rast->interleaver_id, rast, st);
}

/* This is called by ra_read_header, as well as for embedded
 * RealAudio streams within RealMedia files */
static int ra_read_header_with(AVFormatContext *s, RADemuxContext *ra,
                               AVStream *st)
{
    uint16_t version, header_size;
    int ret;

    version = avio_rb16(s->pb);
    ra->version = version;
    header_size = avio_rb16(s->pb); /* Excluding bytes until now */
    ra->rpc = av_mallocz(sizeof(RealPacketCache));
    if (!ra->rpc)
        return AVERROR(ENOMEM);

    /* Version 3 is quite different; v4 and v5 are very similar. */
    if (version == 3)
        ret = ra_read_header_v3(s, header_size, ra, st);
    else if ((version == 4) || (version == 5))
        ret = ra_read_header_v4_5(s, header_size, ra, st, version);
    else {
        av_log(s, AV_LOG_ERROR, "RealAudio: Unsupported version %"PRIx16"\n", version);
        ret = AVERROR_PATCHWELCOME;
    }
    if (ret < 0)
        av_freep(&ra->rpc);
    return ret;
}

/* This handles pure RealAudio files */
static int ra_read_header(AVFormatContext *s)
{
    RADemuxContext *ra   = s->priv_data;
    RAStream *rast       = &(ra->rast);
    uint32_t tag;
    AVStream *st;
    int header_ret;

    tag = avio_rl32(s->pb);
    if (tag != RA_HEADER) {
        av_log(s, AV_LOG_ERROR,
               "RealAudio: bad magic %"PRIx32", expected %"PRIx32".\n",
               tag, RA_HEADER);
        return AVERROR_INVALIDDATA;
    }

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    header_ret = ra_read_header_with(s, ra, st);
    if (header_ret < 0) {
        av_freep(&(ra->rpc));
        return header_ret;
    }

    return rm_initialize_pkt_buf(ra->rpc, rast->full_pkt_size);
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

static int ra_store_cache(AVFormatContext *s, Interleaver *inter,
                    RealPacketCache *rpc, RADemuxContext *radc, int size)
{
    int bytes_read;
    bytes_read = avio_read(s->pb, rpc->pkt_buf, size);
    rpc->next_pkt_start = rpc->pkt_buf;
    inter->postread_packet(radc, bytes_read);
    return 0;
}

static int ra_read_packet_with(AVFormatContext *s, AVPacket *pkt,
                               RADemuxContext *radc)
{
    RAStream *rast       = &(radc->rast);
    RealPacketCache *rpc = radc->rpc;
    Interleaver *inter   = radc->interleaver;
    AVStream *st         = radc->avst;
    int pkt_get_ret;

    if (s->pb->eof_reached)
        return AVERROR(EIO);
    /* Cache a packet if possible; preserve the error if not. */
    if (!rpc->pending_packets) {
        int ret = ra_store_cache(s, inter, rpc, radc, rast->full_pkt_size);
        if (ret <0)
            return ret;
    }
    if (pkt_get_ret = inter->get_packet(s, pkt, radc, rast->full_pkt_size) < 0)
        return pkt_get_ret;
    ra_ac3_swap_bytes(st, pkt); /* TODO: put this in get_packet? */
    return 0;
//    if (ra->version == 3)
//        return av_get_packet(s->pb, pkt, rast->full_pkt_size);

    /* Nope, it's a bit more complicated */

//    if (rast->interleaver_id == DEINT_ID_INT4) {
//        return ra_read_interleaved_packets(s, pkt, ra);
//    /* Simple case: no interleaving */
//    } else if (rast->interleaver_id == DEINT_ID_INT0) {
//        get_pkt = av_get_packet(s->pb, pkt, rast->full_pkt_size);
//        /* Swap the bytes iff it's ac3 - check done in rm_ac3_swap_bytes */
//        ra_ac3_swap_bytes(st, pkt);
//        return get_pkt;
//    } else if ((rast->interleaver_id == DEINT_ID_VBRF) ||
//        (rast->interleaver_id == DEINT_ID_VBRS)) {
//        /* TODO FIXME implement this */
//        av_log(s, AV_LOG_ERROR, "RealAudio: VBR* unimplemented.\n");
//        return AVERROR_INVALIDDATA;
//    } else {
//        av_log(s, AV_LOG_ERROR,
//            "RealAudio: internal error, unknown interleaver\n");
//        return AVERROR_INVALIDDATA;
//    }
}

static int ra_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    return ra_read_packet_with(s, pkt, s->priv_data);
}

/* TODO: revisit this */
static int ra_read_close(AVFormatContext *s)
{
//    RADemuxContext *ra = s->priv_data;
//    RAStream *rast = &(ra->rast);

    return 0;
}


/* RealMedia demuxer */
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

static int rm_read_index_header(AVFormatContext *s, uint32_t *next_header)
{
    RMDemuxContext *rmdc = s->priv_data;
    AVStream *st;
    uint32_t index_tag, num_indices;
    int16_t object_version, stream_number;

    index_tag = avio_rl32(s->pb);
    if (index_tag != RM_INDX_HEADER) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: got %"PRIx32", not INDX section (0x%"PRIx64").\n",
               index_tag, avio_tell(s->pb));
        return AVERROR_INVALIDDATA;
    }

    avio_rb32(s->pb); /* the size of the index chunk */
    object_version    = avio_rb16(s->pb);
    if (object_version == 0) {
        num_indices   = avio_rb32(s->pb);
        stream_number = avio_rb16(s->pb);
        *next_header  = avio_rb32(s->pb);
    } else {
        av_log(s, AV_LOG_ERROR, "RealMedia: unknown index version %"PRIu16"\n",
               object_version);
        return AVERROR_INVALIDDATA;
    }

    if (stream_number >= rmdc->num_streams)
    {
        av_log(s, AV_LOG_ERROR, "Stream number %"PRIu16" too large.\n",
               stream_number);
        return AVERROR_INVALIDDATA;
    }
    st = s->streams[stream_number];

    /* Read the Index Records */
    for (int i = 0; i < num_indices; i++)
    {
        /* offset       = Offset from the beginning of the file
         * packet count = # of packets from beginning until now,
         *                assuming the file is played from the beginning.
         */
        uint32_t timestamp, offset;
        uint16_t ir_object_version;

        ir_object_version = avio_rb16(s->pb);
        if (ir_object_version == 0)
        {
            timestamp    = avio_rb32(s->pb);
            offset       = avio_rb32(s->pb);
            avio_rb32(s->pb); /* packet count */
            av_add_index_entry(st, offset, timestamp, 0, 0, AVINDEX_KEYFRAME);
        } else {
            av_log(s, AV_LOG_ERROR,
                   "RealMedia: unknown index version %"PRIu16"\n",
                   ir_object_version);
            return AVERROR_INVALIDDATA;
        }
    }

    return 0;
}

static int rm_read_indices(AVFormatContext *s)
{
    int err_ret = 0;
    uint32_t next_header_start;

    /* eof_reached is only set after reading too far. */
    while (s->pb->buffer_size > avio_tell(s->pb)) {
        int index_ret = rm_read_index_header(s, &next_header_start);
        if (index_ret < 0)
            err_ret = index_ret;

        /* Recover if reading an index fails; read the next  */
        if ((avio_tell(s->pb) != next_header_start) && (next_header_start != 0))
        {
            av_log(s, AV_LOG_WARNING,
                   "RealMedia: Index expected at %"PRIx32"; at %"PRIx64"\n",
                   next_header_start, avio_tell(s->pb));
            avio_seek(s->pb, next_header_start, SEEK_SET);
        }
    }
    return err_ret; /* 0 iff everything was ok */
}

static int rm_get_num(AVIOContext *pb)
{
    int n, n1;

    n = avio_rb16(pb);
    n &= 0x7FFF;
    if (n >= 0x4000) {
        return n - 0x4000;
    } else {
        n1 = avio_rb16(pb);
        return (n << 16) | n1;
    }
}

/* TODO: check if this is at the expected index position. */
static int rm_read_data_chunk_header(AVFormatContext *s)
{
    RMDemuxContext *rm = s->priv_data;

    rm->cur_pkt_start      = avio_tell(s->pb);
    rm->cur_pkt_version    = avio_rb16(s->pb);
    rm->cur_pkt_size       = avio_rb16(s->pb);
    rm->cur_stream_number  = avio_rb16(s->pb);
    rm->cur_timestamp_ms   = avio_rb32(s->pb);

    if (rm->cur_pkt_version == 0) {
        avio_r8(s->pb); /* packet_group */
        avio_r8(s->pb); /* flags */
    } else if (rm->cur_pkt_version == 1) {
        avio_rb16(s->pb); /* ASM rule */
        avio_r8(s->pb); /* ASM flags */
    } else {
        avio_seek(s->pb, rm->cur_pkt_start, SEEK_SET);
        if (avio_rl32(s->pb) == RM_INDX_HEADER) {
            avio_seek(s->pb, -4, SEEK_CUR);
            return rm_read_indices(s);
        }
        /* It wasn't an index; something has gone badly wrong. */
        av_log(s, AV_LOG_ERROR,
               "RealMedia: Send sample. Unknown packet_version %"PRIx16" "
               "at hex position %"PRIx64".\n",
               rm->cur_pkt_version, rm->cur_pkt_start);
        return AVERROR_INVALIDDATA;
    }

    if (rm->cur_stream_number >= rm->num_streams) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: Invalid stream %"PRIu32": max is %"PRIu32".\n",
               rm->cur_stream_number, rm->num_streams - 1);
        /* TODO: zero the cur_* variables? */
        return AVERROR_PATCHWELCOME;
    }

    return 0;
}


/* Undocumented; logic adapted from the old code. */
/* This assumes slices are contiguous; revisit if false. */
static int rm_handle_slices(AVFormatContext *s, AVPacket *pkt,
                            int subpacket_type, int hdr, int dch_len)
{
    RMDemuxContext *rm = s->priv_data;
    int full_frame_len, pos, cur_len, compat_slices;
    int slices, videobuf_size, videobuf_pos, pkt_pos;
    int slice_header_bytes = 0;
    uint8_t cur_slice, seq;
    int64_t pre_slice_header_pos;
    int ends_with_multiframe = 0;

    pre_slice_header_pos = avio_tell(s->pb);
    seq                  = avio_r8(s->pb);
    full_frame_len       = rm_get_num(s->pb);
    pos                  = rm_get_num(s->pb);
    avio_r8(s->pb);      /* pic_num */
    /* The +1 is because 'hdr' was read by the caller. */
    slice_header_bytes   = avio_tell(s->pb) - pre_slice_header_pos + 1;

    //if ((seq & 0x7F) == 1 || curpic_num != pic_num) {
    slices        = ((hdr & 0x3F) << 1);
    /* The old code calculated the number of slices wrong. */
    compat_slices = slices + 1;
    if (seq & 0x80) {/* The first bit of seq is the last slice count field. */
        slices += 1;
    }
    videobuf_size = full_frame_len + 8 * compat_slices + 1;
    if(av_new_packet(pkt, videobuf_size) < 0)
        return AVERROR(ENOMEM);
    videobuf_pos = 8 * slices + 1;
    //cur_slice    = 0;
    //curpic_num   = pic_num;
    pkt_pos      = avio_tell(s->pb);
    cur_len      = dch_len;
    /* Reread the slice header rather than special-casing the first run. */
    avio_seek(s->pb, -1 * slice_header_bytes, SEEK_CUR);
    pkt->data[0] = slices - 1;

    /* Slice numbers start at 1. */
    for (cur_slice = 1; cur_slice <= slices; cur_slice++) {
        int dch_ret;
        int64_t pre_header_pos;
        int garbage_bytes = 0;

        pre_slice_header_pos = avio_tell(s->pb);
        avio_r8(s->pb);      /* Slice header */
        seq                  = avio_r8(s->pb) & 0x7F;
        full_frame_len       = rm_get_num(s->pb);
        pos                  = rm_get_num(s->pb);
        avio_r8(s->pb);      /* pic_num */
        slice_header_bytes   = avio_tell(s->pb) - pre_slice_header_pos;

        /* Sanity check: the current slice should match the last 7 bits of seq. */
        if (cur_slice != seq)
        {
            av_log(s, AV_LOG_ERROR,
                   "RealMedia: bad slice #, wanted %"PRIu8", got %"PRIu8"\n",
                   cur_slice, seq);
            av_free_packet(pkt);
            return AVERROR_INVALIDDATA;
        }
        //printf("cur_slice: %i at 0x%"PRIx64"\n", cur_slice, avio_tell(s->pb));
        cur_len -= slice_header_bytes;

        /* RM_LAST_PARTIAL_FRAME slices can be followed by
           multiple frames in the same data chunk. */
        if ((subpacket_type == RM_LAST_PARTIAL_FRAME)) {
            if (cur_len > pos) {
                ends_with_multiframe = cur_len - pos;
                cur_len = pos;
            }
        } else if ((cur_slice == slices) && (cur_len > pos)) {
            /* Some last slices are RM_PARTIAL_FRAME, not RM_LAST_PARTIAL_FRAME
             * and are part of a too-large data chunk. Why?
             * This is an ugly hack to at least keep data chunk headers
             * at the beginning of the next read. */
            garbage_bytes = cur_len - pos;
            cur_len = pos;
        }

        AV_WL32(pkt->data - 7 + 8 * cur_slice, 1);
        AV_WL32(pkt->data - 3 + 8 * cur_slice,
                videobuf_pos - 8 * slices - 1);
        if (videobuf_pos + cur_len > videobuf_size) {
            av_log(s, AV_LOG_ERROR, "videobuf_pos + cur_len > videobuf_size\n");
            av_free_packet(pkt);
            return AVERROR_INVALIDDATA;
        }
        //printf("cur_len: %i\n", cur_len);
        if (avio_read(s->pb, pkt->data + videobuf_pos, cur_len) != cur_len)
            return AVERROR(EIO);
        videobuf_pos += cur_len;

        if (garbage_bytes)
            avio_seek(s->pb, garbage_bytes, SEEK_CUR);

        /* Don't read the data header after the last slice. */
        if (cur_slice != slices) {
            pre_header_pos = avio_tell(s->pb);
            //printf("Pre-dch pos: 0x%"PRIx64"\n", pre_header_pos);
            dch_ret = rm_read_data_chunk_header(s);
            if (dch_ret)
            {   av_log(s, AV_LOG_ERROR,
                       "RealMedia: error reading data chunk header in slices.\n");
                return dch_ret;
            }
            cur_len = rm->cur_pkt_size - (avio_tell(s->pb) - pre_header_pos);
        }
    }

    cur_slice -= 1;
    //printf("videobuf_pos: %i, other: %i\n", videobuf_pos, cur_slice - compat_slices);
    pkt->size   = videobuf_pos; //+ 8 * (cur_slice - compat_slices);
    pkt->pts    = AV_NOPTS_VALUE;
    pkt->pos    = pkt_pos;
    //}
    /* 0 if false, bytes left if true. */
    return ends_with_multiframe;
}


/* Figure out which bitstream layout is being used, frame information, etc.
 * This partially replaces rm_assemble_video_frame, and shamelessly borrows
 * logic from it: frame composition seems undocumented correctly elsewhere.
 * For now, cheerfully assume that there aren't extra DATA blocks in
 * inconvenient places.
 */
static int rm_assemble_video(AVFormatContext *s, RMStream *rmst,
                             RealPacketCache *rpc, AVPacket *pkt, int dch_len,
                             uint32_t timestamp)
{
    uint8_t first_bits, subpacket_type;
    uint32_t len;
    int ret;

    //printf("In AV, position: %"PRIx64"\n", avio_tell(s->pb));
    len            = dch_len; /* Length of the current data chunk. */
    first_bits     = avio_r8(s->pb);
    subpacket_type = first_bits >> 6;
    switch(subpacket_type) {
    case RM_MULTIPLE_FRAMES: /* 11 */
        //len     = get_num(s->pb);
        //pos     = get_num(s->pb); /* TODO: this is a timestamp. */
        //pic_num = avio_r8(s->pb);
        avio_seek(s->pb, -1, SEEK_CUR);
        if (rpc->pkt_buf)
            rm_clear_rpc(rpc);
        if (rm_initialize_pkt_buf(rpc, dch_len))
            return AVERROR(ENOMEM);
        avio_read(s->pb, rpc->pkt_buf, dch_len);
        rpc->pending_packets += RM_MULTIFRAME_PENDING;
        rpc->next_offset      = 0;
        return 0;
    case RM_WHOLE_FRAME: /* 01 */
        /* Why is it +9 with the prelude below? */
        if (av_new_packet(pkt, len + 9) < 0)
            return AVERROR(ENOMEM);
        pkt->data[0] = 0;
        AV_WL32(pkt->data + 1, 1);
        AV_WL32(pkt->data + 5, 0);
        avio_read(s->pb, pkt->data + 9, len);
        pkt->pts = timestamp;
        rpc->pending_packets = 1;
        return 0;
    /* Partial frames, not whole ones. */
    case RM_LAST_PARTIAL_FRAME: /* 10 */ /* Intentional fallthrough */
    case RM_PARTIAL_FRAME:      /* 00 */
        ret = rm_handle_slices(s, pkt, subpacket_type, first_bits, dch_len);
        if (ret >= 0)
            rpc->pending_packets = 1;
        if (ret > 0)
            return rm_assemble_video(s, rmst, rpc, pkt, ret, timestamp);
        return ret;
    }
    return 0; /* Unreachable, but GCC insists. */
}


/* This should always start at a RM data chunk header, and consume
 * one or more of them. The stream is determined by the data.
 * The buffer information is found in the stream's private data.
 * The pkt argument is only used for video.
 */
static int rm_cache_packet(AVFormatContext *s, AVPacket *pkt)
{
    AVStream *st;
    RMStream *rmst;
    RealPacketCache *rpc;
    Interleaver *inter;
    RADemuxContext *radc;
    RMDemuxContext *rm = s->priv_data;
    int read_so_far        = 0;
    uint8_t *read_to       = NULL;
    int first_stream       = -1;
    int bytes_read         = 0;
    int data_bytes_to_read = -1;
    int chunk_size, pre_header_pos, header_bytes;

    do {
        int data_header_ret, read_ret; //, start_pos;

        pre_header_pos = avio_tell(s->pb);
        data_header_ret = rm_read_data_chunk_header(s);
        if (data_header_ret)
        {
             av_log(s, AV_LOG_ERROR,
                    "RealMedia: error reading data chunk header.\n");
            return data_header_ret;
        }
        header_bytes = avio_tell(s->pb) - pre_header_pos;

        if (first_stream == -1) {
            first_stream = rm->cur_stream_number;
        } else if (first_stream != rm->cur_stream_number) {
            printf("Looks like this does need partial packet support...\n");
            /* TODO: implement that, and seek back to start_pos */
            return -1;
        }

        st         = s->streams[rm->cur_stream_number];
        rmst       = st->priv_data;
        radc       = &(rmst->radc);
        rpc        = rmst->rpc;

        chunk_size = rm->cur_pkt_size - header_bytes;
        /* Bail out of all this and handle this elsewhere if it's video */
        if (st->codec->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            int vid_ok;
            uint32_t ts;
            ts = rm->cur_timestamp_ms;
            vid_ok = rm_assemble_video(s, rmst, rpc, pkt, chunk_size, ts);
            if (vid_ok < 0)
                /* It went horribly wrong; see if something else can be retrieved. */
                return rm_cache_packet(s, pkt);
            return vid_ok;
        }

        inter = radc->interleaver;

        //inter->preread_packet(s, radc);

        /* FIXME: if chunk sizes aren't constant for a stream, this
           needs to be rewritten. */
        if (data_bytes_to_read == -1) /* Not set yet */
            /* Least common multiple */
            data_bytes_to_read = (rmst->full_pkt_size * chunk_size) /
                                 av_gcd(rmst->full_pkt_size, chunk_size);

        /* Initialize the packet buffer if necessary */
        if (!rpc->pkt_buf)
            if (rm_initialize_pkt_buf(rpc, data_bytes_to_read))
                return AVERROR(ENOMEM);
        /* Revisit this if adding partial packet support. */
        if (data_bytes_to_read > rpc->buf_size) {
            printf("Add realloc support.\n");
            return -1;
        }

        if (!read_to) {
            read_to = rpc->pkt_buf;
            /* Make sure no stale data is present. */
            memset(read_to, '\0', rpc->buf_size);
        }

        //return av_get_packet(s->pb, pkt, pkt_size)
        read_ret = avio_read(s->pb, read_to, chunk_size);
        if (read_ret < 0) {
            av_log(s, AV_LOG_ERROR, "RealMedia: failed read.\n");
            return read_ret;
        }
        bytes_read += read_ret;
        read_so_far += chunk_size;
        read_to += chunk_size;
    } while (read_so_far < data_bytes_to_read);

    if (bytes_read != data_bytes_to_read) {
        av_log(s, AV_LOG_ERROR, "RealMedia: read the wrong amount.\n");
        return AVERROR(EIO);
    }

    inter->postread_packet(radc, bytes_read);
    rpc->next_pkt_start = rpc->pkt_buf;
    return 0;
}

/*
static int rm_postread_video_packet(RMStream *rmst, int bytes_read)
{
    printf("Postread called\n");
    return 0;
}*/

/* Get one frame from a multi-frame packet.
   The multiframe packet format:
   [11......] multiframe indicator, 6 bits reserved
   [AB......] A is reserved, B indicates frame size:
   if B = 0, frame bits = 30, else frame bits = 14
   [CD......] C is reserved, D indicates timestamp size.
   If D = 0, timestamp bits = 30, else timestamp bits = 14
   [EEEEEEEE] Sequence number.
   [The specified number of data bytes]
*/
#define RM_FRAME_SIZE_MASK   0x40
#define RM_FRAME_OFFSET_BITS 0x3F
static int rm_get_one_frame(AVFormatContext *s, AVStream *st, AVPacket *pkt,
                            int pkt_size)
{
    RMStream *rm         = st->priv_data;
    RealPacketCache *rpc = rm->rpc;
    int32_t cur_offset, first_bits, frame_size, timestamp;
    int ret = 0;

    cur_offset = rpc->packets_read;
    if ((rpc->pkt_buf[cur_offset] >> 6) != RM_MULTIPLE_FRAMES)
    {
        av_log(s->pb, AV_LOG_ERROR, "RealMedia: broken multiple frame.\n");
        ret = AVERROR_INVALIDDATA; /* TODO: better error code? */
        goto cleanup;
    }
    cur_offset++;

    first_bits = rpc->pkt_buf[cur_offset] & RM_FRAME_OFFSET_BITS;
    if (rpc->pkt_buf[cur_offset] & RM_FRAME_SIZE_MASK) {
        frame_size = (first_bits << 8) + rpc->pkt_buf[cur_offset + 1];
        cur_offset += 2;
    } else {
        frame_size = (first_bits                    << 24) +
                     (rpc->pkt_buf[cur_offset + 1] << 16) +
                     (rpc->pkt_buf[cur_offset + 2] <<  8) +
                      rpc->pkt_buf[cur_offset + 3];
        cur_offset += 4;
    }

    first_bits = rpc->pkt_buf[cur_offset] & RM_FRAME_OFFSET_BITS;
    if (rpc->pkt_buf[cur_offset] & RM_FRAME_SIZE_MASK) {
        timestamp = (first_bits << 8) + rpc->pkt_buf[cur_offset + 1];
        cur_offset += 2;
    } else {
        timestamp = (first_bits                    << 24) +
                    (rpc->pkt_buf[cur_offset + 1] << 16) +
                    (rpc->pkt_buf[cur_offset + 2] <<  8) +
                     rpc->pkt_buf[cur_offset + 3];
        cur_offset += 4;
    }

    cur_offset++; /* ignore the sequence number */
    if (cur_offset + frame_size > rpc->buf_size)
    {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: frame size too large in multiple frame.\n");
        ret = AVERROR_INVALIDDATA;
        goto cleanup;
    }

    if (av_new_packet(pkt, frame_size + 9) < 0)
        return AVERROR(ENOMEM);

    pkt->data[0] = 0;
    AV_WL32(pkt->data + 1, 1);
    AV_WL32(pkt->data + 5, 0);
    rpc->pending_packets = 1;
    memcpy(pkt->data + 9, rpc->pkt_buf + cur_offset, frame_size);
    pkt->stream_index    = st->index;
    if (timestamp)
        pkt->pts         = timestamp;

    rpc->next_offset = cur_offset + frame_size;

    if (rpc->next_offset == rpc->buf_size) {
        ret = 0;
        goto cleanup;
    } else {
        return 0;
    }

cleanup:
    av_free_packet(pkt);
    rm_clear_rpc(rpc);
    return ret;
}

static int rm_get_video_packet(AVFormatContext *s, AVStream *st,
                               AVPacket *pkt, int pkt_size)
{
    RMStream *rm        = st->priv_data;
    RealPacketCache *rpc = rm->rpc;

    /* Is there a packet already set up? */
    if (rpc->pending_packets ^ RM_MULTIFRAME_PENDING) {
        rpc->pending_packets--;
        pkt->stream_index = st->index;
        if (rpc->pending_packets == 0)
            rm_clear_rpc(rpc);
        return 0;
    }
    /* Handle multiframe packets */
    return rm_get_one_frame(s, st, pkt, pkt_size);
}

static int rm_read_media_properties_header(AVFormatContext *s,
                                           RMMediaProperties *rmmp)
{
    RMDemuxContext *rm = s->priv_data;
    AVStream *st;
    uint32_t mdpr_tag, content_tag, content_tag2, before_embed, after_embed;
    uint16_t chunk_version;
    int bytes_read, header_ret, fix_offset;

    mdpr_tag = avio_rl32(s->pb);
    if (mdpr_tag != RM_MDPR_HEADER) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: got %"PRIx32", but expected an MDPR section.\n",
               mdpr_tag);
        return AVERROR_INVALIDDATA;
    }

    rmmp->chunk_size = avio_rb32(s->pb);

    chunk_version = avio_rb16(s->pb);
    if (chunk_version != 0) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: expected MDPR chunk version 0, got %"PRIx16".\n",
               chunk_version);
        return AVERROR_INVALIDDATA;
    }

    rmmp->stream_number       = avio_rb16(s->pb);
    rmmp->max_bitrate         = avio_rb32(s->pb);
    rmmp->avg_bitrate         = avio_rb32(s->pb);
    rmmp->largest_pkt         = avio_rb32(s->pb);
    rmmp->avg_pkt             = avio_rb32(s->pb);
    rmmp->stream_start_offset = avio_rb32(s->pb);
    rmmp->preroll             = avio_rb32(s->pb);
    rmmp->duration            = avio_rb32(s->pb);
    rmmp->desc_size           = avio_r8(s->pb);

    /* RealMedia Stream numbers are zero-indexed */
    if (rmmp->stream_number >= rm->num_streams) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: Invalid stream %"PRIu32": max is %"PRIu32".\n",
               rmmp->stream_number, rm->num_streams - 1);
        return AVERROR_INVALIDDATA;
    }

    st = s->streams[rmmp->stream_number];
    st->priv_data = ff_rm_alloc_rmstream();
    if (!st->priv_data)
        return AVERROR(ENOMEM);

    st->id                = rmmp->stream_number;
    st->start_time        = rmmp->stream_start_offset;
    st->duration          = rmmp->duration;
    st->codec->codec_type = AVMEDIA_TYPE_DATA;


    bytes_read = avio_read(s->pb, rmmp->stream_desc, rmmp->desc_size);
    if (bytes_read < rmmp->desc_size) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: failed to read stream description.\n");
        return bytes_read;
    }

    rmmp->mime_size = avio_r8(s->pb);
    bytes_read = avio_read(s->pb, rmmp->mime_type, rmmp->mime_size);
    if (bytes_read < rmmp->mime_size) {
        av_log(s, AV_LOG_ERROR, "RealMedia: failed to read mime type.\n");
        return bytes_read;
    }

    rmmp->type_specific_size = avio_rb32(s->pb);

    /* No type-specific data? Done! */
    if (!rmmp->type_specific_size)
        return 0;

    before_embed = avio_tell(s->pb);

    content_tag = avio_rl32(s->pb);
    if (content_tag == RA_HEADER) {
        RMStream *rmst;
        RADemuxContext *radc;
        RAStream *rast;
        //Interleaver *inter;

        st->codec->codec_type = AVMEDIA_TYPE_AUDIO;

        rmst     = st->priv_data;
        radc     = &(rmst->radc);
        rast     = &(radc->rast);
        //inter    = &(rmst->interleaver);

        header_ret = ra_read_header_with(s, radc, st);
        if (header_ret) {
            av_log(s, AV_LOG_ERROR,
                   "RealMedia: failed to read embedded RealAudio header.\n");
            return header_ret;
        }
        rmmp->is_realaudio = 1;
        rmst->rpc           = radc->rpc;
        rmst->full_pkt_size = rast->full_pkt_size;
        rmst->subpkt_size   = rast->subpkt_size;
        rmst->subpacket_pp  = rast->subpacket_pp;

        /* TODO: this should become obsolete
        switch(rast->interleaver_id) {
        case DEINT_ID_INT4:
            inter->priv_data = av_mallocz(sizeof(Int4State));
            if (!inter->priv_data)
                return AVERROR(ENOMEM);
            inter->interleaver_tag = rast->interleaver_id;
            inter->get_packet      = rm_get_int4_packet;
            inter->preread_packet  = rm_preread_generic_packet;
            inter->postread_packet = rm_postread_int4_packet;
            break;
        default:
            inter->interleaver_tag = 0;
            inter->get_packet      = rm_get_generic_packet;
            inter->preread_packet  = rm_preread_generic_packet;
            inter->postread_packet = rm_postread_generic_packet;
        } */
    } else if (RM_VIDEO_TAG == (content_tag2 = avio_rl32(s->pb))) {
        RMStream *rmst     = st->priv_data;
        //Interleaver *inter = &(rmst->interleaver);
        int extradata_ret;

        st->codec->codec_tag = avio_rl32(s->pb);
        st->codec->codec_id = ff_codec_get_id(ff_rm_codec_tags,
                                              st->codec->codec_tag);
        if (st->codec->codec_id == AV_CODEC_ID_NONE) {
            av_log(s, AV_LOG_ERROR, "RealMedia: failed to get codec id.\n");
            return AVERROR_INVALIDDATA;
        }
        st->codec->width  = avio_rb16(s->pb);
        st->codec->height = avio_rb16(s->pb);
        avio_skip(s->pb, 2); /* Old code said: maybe bits per sample? */
        if (avio_rb32(s->pb) != 0)
            av_log(s, AV_LOG_WARNING, "RealMedia: expected zeros.\n");
        rmst->fps = avio_rb32(s->pb);
        if (rmst->fps > 0)
            av_reduce(&st->avg_frame_rate.den, &st->avg_frame_rate.num,
                      0x10000, rmst->fps, (1 << 30) - 1);

        st->codec->codec_type = AVMEDIA_TYPE_VIDEO;
        //inter->interleaver_tag = st->codec->codec_id;
        //inter->get_packet      = rm_get_video_packet;
        //inter->preread_packet  = rm_preread_video_packet;
        //inter->postread_packet = rm_postread_video_packet;

        rmst->rpc = av_mallocz(sizeof(RealPacketCache));
        if (!rmst->rpc)
            return AVERROR(ENOMEM);

        extradata_ret = real_read_extradata(s->pb, st->codec,
                                          rmmp->type_specific_size -
                                          (avio_tell(s->pb) - before_embed));
        if (extradata_ret < 0) {
            av_freep(&rmst->rpc);
            return extradata_ret;
        }
    } else {
        printf("Deal with tag %x\n", content_tag);
        /* FIXME TODO: make these initializations more reasonable. */
        //rmst->full_pkt_size = 1;
        //rmst->subpkt_size   = 1;
        //rmst->subpacket_pp  = 1;
    }

    after_embed = avio_tell(s->pb);
    if (after_embed != (rmmp->type_specific_size + before_embed)) {
        fix_offset = (rmmp->type_specific_size + before_embed) - after_embed;
        av_log(s, AV_LOG_WARNING,
               "RealMedia: ended up in the wrong place reading MDPR "
               "type-specific data, attempting to recover.\n");
        avio_seek(s->pb, fix_offset, SEEK_CUR);
    }

    return 0;
}

/* TODO: share code with rm_read_data_header? */
static int rm_read_cont_header(AVFormatContext *s)
{
    uint32_t cont_tag;
    uint16_t chunk_version;

    cont_tag = avio_rl32(s->pb);
    if (cont_tag != RM_CONT_HEADER) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: got %"PRIx32", but expected a CONT section.\n",
               cont_tag);
        return AVERROR_INVALIDDATA;
    }

    avio_rb32(s->pb); /* Chunk size */

    chunk_version = avio_rb16(s->pb);
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
    uint32_t data_tag;
    uint16_t chunk_version;

    data_tag = avio_rl32(s->pb);
    if (data_tag != RM_DATA_HEADER) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: got %"PRIx32", but expected a DATA section.\n",
               data_tag);
        return AVERROR_INVALIDDATA;
    }

    rmdh->data_chunk_size = avio_rb32(s->pb);

    chunk_version = avio_rb16(s->pb);
    if (chunk_version != 0) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: expected DATA chunk version 0, got %"PRIx16".\n",
               chunk_version);
        return AVERROR_INVALIDDATA;
    }

    rmdh->num_data_packets       = avio_rb32(s->pb);
    rmdh->next_data_chunk_offset = avio_rb32(s->pb);

    return 0;
}

static int rm_initialize_streams(AVFormatContext *s, int num_streams)
{
    int i, j, err = 0;
    AVStream *st;

    for (i = 0; i < num_streams; i++) {
        st = avformat_new_stream(s, NULL);
        if (!st)
            err = 1;
        else {
            st->priv_data = ff_rm_alloc_rmstream();
            if (!st->priv_data)
                err = 1;
        }
        if (err) {
            for (j = 0; j < i; j++) {
                av_free(s->streams[j]->priv_data);
                av_free(s->streams[j]);
                return AVERROR(ENOMEM);
            }
        }
    }
    return 0;
}


static int rm_read_prop_header(AVFormatContext *s)
{
    RMDemuxContext *rm = s->priv_data;
    uint32_t prop_tag;
    uint16_t prop_chunk_version;
    int init_stream_ret;

    /* Read the PROP header */
    prop_tag = avio_rl32(s->pb);
    if (prop_tag != RM_PROP_HEADER) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: got %"PRIx32", but expected a PROP section.\n",
               prop_tag);
        return AVERROR_INVALIDDATA;
    }

    /* 'Typically' 0x32 */
    rm->prop_chunk_size = avio_rb32(s->pb);
    prop_chunk_version = avio_rb16(s->pb);
    if (prop_chunk_version != 0) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: expected PROP chunk version 0, got %"PRIx16".\n",
               prop_chunk_version);
        return AVERROR_INVALIDDATA;
    }

    rm->prop_max_bitrate    = avio_rb32(s->pb);
    rm->prop_avg_bitrate    = avio_rb32(s->pb);
    rm->prop_largest_pkt    = avio_rb32(s->pb);
    rm->prop_avg_pkt        = avio_rb32(s->pb);
    rm->prop_num_pkts       = avio_rb32(s->pb);
    rm->prop_file_duration  = avio_rb32(s->pb);
    rm->suggested_ms_buffer = avio_rb32(s->pb);
    rm->first_indx_offset   = avio_rb32(s->pb);
    rm->first_data_offset   = avio_rb32(s->pb);
    rm->num_streams         = avio_rb16(s->pb);
    rm->flags               = avio_rb16(s->pb);

    /* Initialize these before reading packets */
    rm->cur_pkt_start = 0;
    rm->cur_pkt_size  = 0;

    init_stream_ret = rm_initialize_streams(s, rm->num_streams);
    if (init_stream_ret) /* Setting up failed */
        return init_stream_ret; /* Preserve why */

    return 0;
}

/* TODO: find the sample with rmf chunk size = 10 and a *word* file version */
static int rm_read_header(AVFormatContext *s)
{
    RMDemuxContext *rm = s->priv_data;
    uint32_t rm_tag, file_version;
    uint16_t rm_chunk_version;
    int prop_count = 0;

    /* Read the RMF header */
    rm_tag = avio_rl32(s->pb);
    if (rm_tag != RM_HEADER) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: bad magic %"PRIx32", expected %"PRIx32".\n",
               rm_tag, RM_HEADER);
        return AVERROR_INVALIDDATA;
    }

    rm->header_chunk_size = avio_rb32(s->pb);

    rm_chunk_version = avio_rb16(s->pb);
    if (rm_chunk_version != 0) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: expected RMF chunk version 0, got %"PRIx16".\n",
               rm_chunk_version);
        return AVERROR_INVALIDDATA;
    }

    file_version = avio_rb32(s->pb);
    if (file_version != 0) {
        av_log(s, AV_LOG_ERROR,
               "RealMedia: expected file version 0, got %"PRIx32".\n",
               file_version);
        return AVERROR_INVALIDDATA;
    }

    rm->num_headers = avio_rb32(s->pb);

    /* Read the tags; return 0 on success, !0 early on failure */
    for (;;) {
        int header_ret;
        uint32_t next_tag;
        RMMediaProperties rmmp;

        next_tag = avio_rl32(s->pb);
        printf("next tag: %x\n", next_tag);
        /* FIXME: the tag is being preserved as an extra check */
        avio_seek(s->pb, -4, SEEK_CUR); /* REMOVE THIS */
        switch(next_tag) {
        case RM_PROP_HEADER:
            printf("prop\n");
            if (prop_count) {
                av_log(s, AV_LOG_ERROR, "RealMedia: too many PROP headers.\n");
                return AVERROR_INVALIDDATA;
            }
            header_ret = rm_read_prop_header(s);
            if (header_ret)
                return header_ret;
            prop_count = 1;
            break;
        case RM_CONT_HEADER:
            printf("cont\n");
            header_ret = rm_read_cont_header(s);
            if (header_ret)
                return header_ret;
            break;
        case RM_MDPR_HEADER:
            printf("mdpr\n");
            if (!prop_count) {
                av_log(s, AV_LOG_ERROR, "RealMedia: invalid header order,"
                             " need PROP before MDPR.\n");
                return AVERROR_INVALIDDATA;
            }
            /* The properties have to be read to find the stream number. */
            memset(&rmmp, '\0', sizeof(RMMediaProperties));
            header_ret = rm_read_media_properties_header(s, &rmmp);
            if (header_ret) /* Reading media properties failed */
                return header_ret;
            else {
                AVStream *st = s->streams[rmmp.stream_number];
                RMStream *rmst = st->priv_data;
                RMMediaProperties *target_rmmp = &(rmst->rmmp);

                memcpy(target_rmmp, &rmmp, sizeof(RMMediaProperties));
            }
            break;
        case RM_DATA_HEADER:
            printf("data\n");
            header_ret = rm_read_data_header(s, &(rm->cur_data_header));
            if (header_ret)
                return header_ret;
            break;
        default:
            /* There's not another tag right away */
            //avio_seek(s->pb, -4, SEEK_CUR);
            printf("Done with headers\n");
            return 0;
        }
    }
}



static int rm_read_cached_packet(AVFormatContext *s, AVPacket *pkt)
{
    RMDemuxContext *rm = s->priv_data;
    int i;

    for (i = 0; i < rm->num_streams; i++) {
        AVStream *st         = st = s->streams[i];
        RMStream *rmst       = st->priv_data;
        RealPacketCache *rpc = rmst->rpc;
        RADemuxContext *radc = &(rmst->radc);
        //int ret;

        if (rpc->pending_packets) {
            Interleaver *inter;
            if (st->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
                return rm_get_video_packet(s, st, pkt, rmst->subpkt_size);
            }
            inter = radc->interleaver;
            return inter->get_packet(s, pkt, radc, rmst->subpkt_size);
            //printf("Packet size: 0x%x, pos: 0x%"PRIx64"\n", pkt->size, avio_tell(s->pb));
            //return ret;
        }
    }
    return -1; /* No queued packets */
}

static int rm_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int cache_read;

    if ((cache_read = rm_read_cached_packet(s, pkt)) == -1) {
        RMDemuxContext *rm = s->priv_data;
        /* There were no cached packets; cache at least one,
           if there are any left to cache. */
        if (avio_tell(s->pb) < rm->first_indx_offset) {
            rm_cache_packet(s, pkt);
            return rm_read_cached_packet(s, pkt);
        } else {
            /* Read the indexes, and then the file ends. */
            rm_read_indices(s);
            return AVERROR(EIO);
        }
    }
    return cache_read;
}

/* TODO: am I actually using RMVidStream by now? */
static void rm_cleanup_stream(AVStream *st)
{
    RMStream *rmst = st->priv_data;
    rm_clear_rpc(rmst->rpc);
    av_free(rmst->rmmp.type_specific_data);
}

static int rm_read_close(AVFormatContext *s)
{
    RMDemuxContext *rmdc = s->priv_data;
    for (int i = 0; i < rmdc->num_streams; i++)
        rm_cleanup_stream(s->streams[i]);
    return 0;
}

/* Elenril said something about pts rather than dts. Thoughts? */
/* Cargo-culted from the old code, asfdec, and mpeg.c */
static int64_t rm_read_pts(AVFormatContext *s, int stream_index,
                           int64_t *ppos, int64_t pos_limit)
{
    RMDemuxContext *rmdc = s->priv_data;
    int64_t start_pos, header_bytes;
    int64_t dts = AV_NOPTS_VALUE;

    /* TODO: actually find the next data chunk header after *ppos */
    if (avio_seek(s->pb, *ppos, SEEK_SET) < 0)
        return AV_NOPTS_VALUE;

    for (;;) {
        start_pos = avio_tell(s->pb);
        if (rm_read_data_chunk_header(s)) {
            av_log(s, AV_LOG_WARNING,
                   "RealMedia: data chunk header failure in rm_read_pts.\n");
            return AV_NOPTS_VALUE;
        }

        /* Wrong stream; seek to the next likely header and try again. */
        /* TODO: handle multiple data sections. */
        if (rmdc->cur_stream_number != stream_index) {
            header_bytes = avio_tell(s->pb) - start_pos;
            avio_seek(s->pb, rmdc->cur_pkt_size - header_bytes, SEEK_CUR);
            continue;
        }

        dts = rmdc->cur_timestamp_ms;
        break;
    }

    return dts;
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
    RMStream *rmst = av_mallocz(sizeof(RMStream));
    return rmst;
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
    .read_timestamp = rm_read_pts,
};

AVInputFormat ff_rdt_demuxer = {
    .name           = "rdt",
    .long_name      = NULL_IF_CONFIG_SMALL("RDT demuxer"),
    .priv_data_size = sizeof(RMDemuxContext),
    .read_close     = rm_read_close,
    .flags          = AVFMT_NOFILE,
};

