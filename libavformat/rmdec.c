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

#include "libavutil/channel_layout.h"
#include "libavutil/intreadwrite.h"

#include "avformat.h"
#include "rm.h"

/* Header for RealAudio 1.0 (.ra version 3
 * and RealAudio 2.0 file (.ra version 4). */
//#define RA_HEADER MKTAG('.', 'r', 'a', 0xfd)
//#define REAL_CONTENT_DESCRIPTION MKTAG('C', 'O', 'N', 'T')
#define RA_HEADER ".ra\xfd"
#define REAL_CONTENT_DESCRIPTION "CONT"

/* The relevant VSELP format has 159-bit frames, stored in 20 bytes */
#define RA144_PKT_SIZE 20

struct RMStream {
};

/* Demux context for RealAudio */
typedef struct {
} RADemuxContext;

/* Demux context for RealMedia (audio+video) */
typedef struct {
} RMDemuxContext;

/* Return value >= 0: bytes read.
 * Return value < 0: error.
 */
static int read_content_description_field(AVFormatContext *s, const char *desc)
{
    AVIOContext *acpb = s->pb;
    uint16_t len;
    uint8_t *val;
    len = avio_r8(acpb);
    val = av_mallocz(len + 1);
    if (!val)
        return AVERROR(ENOMEM);
    avio_read(acpb, val, len);
    printf("Hm: %i, %s\n", len+2, val);
    return len + 2;
}

/* The content description header is documented, and the same in RA and RM:
 * https://common.helixcommunity.org/2003/HCS_SDK_r5/htmfiles/rmff.htm
 * It is similar for RA, but CONT is not set.
 */

static int real_read_content_description(AVFormatContext *s)
{
    AVIOContext *acpb = s->pb;
    //uint32_t cdh_size, sought;
    //uint16_t object_version; //, title_len, author_len, copyright_len, comment_len;
    //uint8_t *title, *author, *copyright, *comment;
    //char object_id[4];
    uint16_t data_size;
    int sought;
    int tmp;
    /* The RM spec defines these 10 bytes; RA files don't have the same opinion.
    //object_id = avio_rb32(acpb);
    avio_read(acpb, object_id, 4);
    //if (object_id != REAL_CONTENT_DESCRIPTION) { 
    if ((!isRA) && memcmp(object_id, REAL_CONTENT_DESCRIPTION, 4)) {
        printf("%hhx%hhx%hhx%hhx vs %c%c%c%c\n", object_id[0], object_id[1], object_id[2], object_id[3], REAL_CONTENT_DESCRIPTION[0], REAL_CONTENT_DESCRIPTION[1], REAL_CONTENT_DESCRIPTION[2], REAL_CONTENT_DESCRIPTION[3]);
        av_dlog(s,
                "Invalid Content Description Header: expected ..., got ...\n");
        return AVERROR_INVALIDDATA;
    }
    cdh_size = avio_rb32(acpb);
    printf("Got size %i\n", cdh_size);
    object_version = avio_rb16(acpb);
    printf("Got object_version %i\n", object_version);
    if (object_version != 0) {
        av_dlog(s,
                "Bad version %s in a Content Description Header, should be 0.\n",
                object_version);
        return AVERROR_INVALIDDATA;
    } */
    /* The wiki claims 10 unknown, then dword data size.
     * A real file suggests it's 13 unknown, then byte data size.
     */
    avio_skip(acpb, 13);
    data_size = avio_r8(acpb);
    sought = 12; /* Header bytes read so far */
    /*
    title_len = avio_rb16(acpb);
    author_len = avio_rb16(acpb);
    _len = avio_rb16(acpb);
    title_len = avio_rb16(acpb);
    */

    tmp = read_content_description_field(s, "title");
    if (tmp < 0)
        return tmp;
    else
        sought += tmp;
    tmp = read_content_description_field(s, "author");
    if (tmp < 0)
        return tmp;
    else
        sought += tmp;
    tmp = read_content_description_field(s, "copyright");
    if (tmp < 0)
        return tmp;
    else
        sought += tmp;
    tmp = read_content_description_field(s, "comment");
    if (tmp < 0)
        return tmp;
    else
        sought += tmp;

    printf("sought: %i, data_size: %i\n", sought, data_size);
    if (sought != data_size + 12) {
        printf("d'oh\n");
        av_dlog(s,
                "Content Description Header size declared %s, was %s.\n",
                cdh_size, sought);
        return AVERROR_INVALIDDATA;
    }
    return sought;
}


static int ra_probe(AVProbeData *p)
{
    /* RealAudio header; for RMF, use rm_probe */
    /* TODO: also check the version */
    //uint32_t buftag = MKTAG(p->buf[0], p->buf[1], p->buf[2], p->buf[3]);
    return (!memcmp(p->buf, RA_HEADER, 4)) ? AVPROBE_SCORE_MAX : 0;
}

static int ra_read_header(AVFormatContext *s)
{
    AVIOContext *acpb = s->pb;
    AVStream *st = NULL;

    char tag[4];
    //uint32_t tag;
    uint16_t version, header_size;
    int tmp;

    //tag = avio_rb32(acpb);
    avio_read(acpb, tag, 4);

    //if (tag != RA_HEADER)
    if (memcmp(tag, RA_HEADER, 4))
        return AVERROR_INVALIDDATA;
    version = avio_rb16(acpb);
    if (version != 3) /* TODO: add v4 support */
        return AVERROR_INVALIDDATA;
    header_size = avio_rb16(acpb);
    //avio_skip(acpb, header_size); /* TODO: read rest of header properly */
    /* TODO: make sure metadata round-trips, for example to AAC/mp4 */
    tmp = real_read_content_description(s);
    printf("Got tmp %i\n", tmp);

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    st->codec->channel_layout = AV_CH_LAYOUT_MONO;
    st->codec->channels = 1;
    st->codec->codec_id = AV_CODEC_ID_RA_144;
    st->codec->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codec->sample_rate = 8000;

    return 0;
}

static int ra_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    return av_get_packet(s->pb, pkt, RA144_PKT_SIZE);
}

static int ra_read_close(AVFormatContext *s)
{
    return 0;
}

static int64_t ra_read_dts(AVFormatContext *s, int stream_index,
                               int64_t *ppos, int64_t pos_limit)
{
    return 0;
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
    if(rms)
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
    .read_timestamp = ra_read_dts,
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

