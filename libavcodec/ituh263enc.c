/*
 * ITU H.263 bitstream encoder
 * Copyright (c) 2000,2001 Fabrice Bellard
 * H.263+ support.
 * Copyright (c) 2001 Juan J. Sierralta P
 * Copyright (c) 2002-2004 Michael Niedermayer <michaelni@gmx.at>
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
 * H.263 bitstream encoder.
 */

#include "config_components.h"

#include <limits.h>
#include <string.h>

#include "libavutil/attributes.h"
#include "libavutil/thread.h"
#include "avcodec.h"
#include "codec_internal.h"
#include "mpegvideo.h"
#include "flvenc.h"
#include "mpegvideoenc.h"
#include "h263.h"
#include "h263enc.h"
#include "h263data.h"
#include "h263dsp.h"
#include "mathops.h"
#include "mpegutils.h"
#include "internal.h"

/**
 * Table of number of bits a motion vector component needs.
 */
static uint8_t mv_penalty[MAX_FCODE+1][MAX_DMV*2+1];

/**
 * Minimal fcode that a motion vector component would need in umv.
 * All entries in this table are 1.
 */
static uint8_t umv_fcode_tab[MAX_MV*2+1];

//unified encoding tables for run length encoding of coefficients
//unified in the sense that the specification specifies the encoding in several steps.
static uint8_t  uni_h263_intra_aic_rl_len [64*64*2*2];
static uint8_t  uni_h263_inter_rl_len [64*64*2*2];
//#define UNI_MPEG4_ENC_INDEX(last,run,level) ((last)*128 + (run)*256 + (level))
//#define UNI_MPEG4_ENC_INDEX(last,run,level) ((last)*128*64 + (run) + (level)*64)
#define UNI_MPEG4_ENC_INDEX(last,run,level) ((last)*128*64 + (run)*128 + (level))

static av_cold void init_mv_penalty_and_fcode(void)
{
    for (int f_code = 1; f_code <= MAX_FCODE; f_code++) {
        for (int mv = -MAX_DMV; mv <= MAX_DMV; mv++) {
            int len;

            if (mv == 0) len = 1; // ff_mvtab[0][1]
            else {
                int val, bit_size, code;

                bit_size = f_code - 1;

                val = mv;
                if (val < 0)
                    val = -val;
                val--;
                code = (val >> bit_size) + 1;
                if (code < 33) {
                    len = ff_mvtab[code][1] + 1 + bit_size;
                } else {
                    len = 12 /* ff_mvtab[32][1] */ + av_log2(code>>5) + 2 + bit_size;
                }
            }

            mv_penalty[f_code][mv + MAX_DMV] = len;
        }
    }

    memset(umv_fcode_tab, 1, sizeof(umv_fcode_tab));
}

static av_cold void init_uni_h263_rl_tab(const RLTable *rl, uint8_t *len_tab)
{
    av_assert0(MAX_LEVEL >= 64);
    av_assert0(MAX_RUN   >= 63);

    for (int slevel = -64; slevel < 64; slevel++) {
        if (slevel == 0) continue;
        for (int run = 0; run < 64; run++) {
            for (int last = 0; last <= 1; last++) {
                const int index = UNI_MPEG4_ENC_INDEX(last, run, slevel + 64);
                int level = slevel < 0 ? -slevel : slevel;
                int sign  = slevel < 0 ? 1 : 0;
                int bits, len, code;

                len_tab[index] = 100;

                /* ESC0 */
                code = get_rl_index(rl, last, run, level);
                bits = rl->table_vlc[code][0];
                len  = rl->table_vlc[code][1];
                bits = bits * 2 + sign;
                len++;

                if (code != rl->n && len < len_tab[index])
                    len_tab[index] = len;

                /* ESC */
                bits = rl->table_vlc[rl->n][0];
                len  = rl->table_vlc[rl->n][1];
                bits = bits *   2 + last; len++;
                bits = bits *  64 + run;  len += 6;
                bits = bits * 256 + (level & 0xff); len += 8;

                if (len < len_tab[index])
                    len_tab[index] = len;
            }
        }
    }
}

static av_cold void h263_encode_init_static(void)
{
    static uint8_t rl_intra_table[2][2 * MAX_RUN + MAX_LEVEL + 3];

    ff_rl_init(&ff_rl_intra_aic, rl_intra_table);
    ff_h263_init_rl_inter();

    init_uni_h263_rl_tab(&ff_rl_intra_aic,  uni_h263_intra_aic_rl_len);
    init_uni_h263_rl_tab(&ff_h263_rl_inter, uni_h263_inter_rl_len);

    init_mv_penalty_and_fcode();
}

av_cold const uint8_t (*ff_h263_get_mv_penalty(void))[MAX_DMV*2+1]
{
    static AVOnce init_static_once = AV_ONCE_INIT;

    ff_thread_once(&init_static_once, h263_encode_init_static);

    return mv_penalty;
}

void ff_h263_encode_motion(PutBitContext *pb, int val, int f_code)
{
    if (val == 0) {
        /* zero vector -- corresponds to ff_mvtab[0] */
        put_bits(pb, 1, 1);
    } else {
        int sign, code, bits;
        int bit_size = f_code - 1;
        int range = 1 << bit_size;
        /* modulo encoding */
        val = sign_extend(val, 6 + bit_size);
        sign = val>>31;
        val= (val^sign)-sign;
        sign&=1;

        val--;
        code = (val >> bit_size) + 1;
        bits = val & (range - 1);

        put_bits(pb, ff_mvtab[code][1] + 1, (ff_mvtab[code][0] << 1) | sign);
        if (bit_size > 0) {
            put_bits(pb, bit_size, bits);
        }
    }
}

#if CONFIG_H263_ENCODER // Snow and SVQ1 need the above
static const uint8_t wrong_run[102] = {
 1,  2,  3,  5,  4, 10,  9,  8,
11, 15, 17, 16, 23, 22, 21, 20,
19, 18, 25, 24, 27, 26, 11,  7,
 6,  1,  2, 13,  2,  2,  2,  2,
 6, 12,  3,  9,  1,  3,  4,  3,
 7,  4,  1,  1,  5,  5, 14,  6,
 1,  7,  1,  8,  1,  1,  1,  1,
10,  1,  1,  5,  9, 17, 25, 24,
29, 33, 32, 41,  2, 23, 28, 31,
 3, 22, 30,  4, 27, 40,  8, 26,
 6, 39,  7, 38, 16, 37, 15, 10,
11, 12, 13, 14,  1, 21, 20, 18,
19,  2,  1, 34, 35, 36
};

/**
 * Return the 4 bit value that specifies the given aspect ratio.
 * This may be one of the standard aspect ratios or it specifies
 * that the aspect will be stored explicitly later.
 */
av_const int ff_h263_aspect_to_info(AVRational aspect){
    int i;

    if(aspect.num==0 || aspect.den==0) aspect= (AVRational){1,1};

    for(i=1; i<6; i++){
        if(av_cmp_q(ff_h263_pixel_aspect[i], aspect) == 0){
            return i;
        }
    }

    return FF_ASPECT_EXTENDED;
}

void ff_h263_encode_picture_header(MpegEncContext * s)
{
    int format, coded_frame_rate, coded_frame_rate_base, i, temp_ref;
    int best_clock_code=1;
    int best_divisor=60;
    int best_error= INT_MAX;
    int custom_pcf;

    if(s->h263_plus){
        for(i=0; i<2; i++){
            int div, error;
            div= (s->avctx->time_base.num*1800000LL + 500LL*s->avctx->time_base.den) / ((1000LL+i)*s->avctx->time_base.den);
            div= av_clip(div, 1, 127);
            error= FFABS(s->avctx->time_base.num*1800000LL - (1000LL+i)*s->avctx->time_base.den*div);
            if(error < best_error){
                best_error= error;
                best_divisor= div;
                best_clock_code= i;
            }
        }
    }
    custom_pcf = best_clock_code != 1 || best_divisor != 60;
    coded_frame_rate= 1800000;
    coded_frame_rate_base= (1000+best_clock_code)*best_divisor;

    align_put_bits(&s->pb);

    /* Update the pointer to last GOB */
    s->ptr_lastgob = put_bits_ptr(&s->pb);
    put_bits(&s->pb, 22, 0x20); /* PSC */
    temp_ref= s->picture_number * (int64_t)coded_frame_rate * s->avctx->time_base.num / //FIXME use timestamp
                         (coded_frame_rate_base * (int64_t)s->avctx->time_base.den);
    put_sbits(&s->pb, 8, temp_ref); /* TemporalReference */

    put_bits(&s->pb, 1, 1);     /* marker */
    put_bits(&s->pb, 1, 0);     /* H.263 id */
    put_bits(&s->pb, 1, 0);     /* split screen off */
    put_bits(&s->pb, 1, 0);     /* camera  off */
    put_bits(&s->pb, 1, 0);     /* freeze picture release off */

    format = ff_match_2uint16(ff_h263_format, FF_ARRAY_ELEMS(ff_h263_format), s->width, s->height);
    if (!s->h263_plus) {
        /* H.263v1 */
        put_bits(&s->pb, 3, format);
        put_bits(&s->pb, 1, (s->pict_type == AV_PICTURE_TYPE_P));
        /* By now UMV IS DISABLED ON H.263v1, since the restrictions
        of H.263v1 UMV implies to check the predicted MV after
        calculation of the current MB to see if we're on the limits */
        put_bits(&s->pb, 1, 0);         /* Unrestricted Motion Vector: off */
        put_bits(&s->pb, 1, 0);         /* SAC: off */
        put_bits(&s->pb, 1, s->obmc);   /* Advanced Prediction */
        put_bits(&s->pb, 1, 0);         /* only I/P-frames, no PB-frame */
        put_bits(&s->pb, 5, s->qscale);
        put_bits(&s->pb, 1, 0);         /* Continuous Presence Multipoint mode: off */
    } else {
        int ufep=1;
        /* H.263v2 */
        /* H.263 Plus PTYPE */

        put_bits(&s->pb, 3, 7);
        put_bits(&s->pb,3,ufep); /* Update Full Extended PTYPE */
        if (format == 8)
            put_bits(&s->pb,3,6); /* Custom Source Format */
        else
            put_bits(&s->pb, 3, format);

        put_bits(&s->pb,1, custom_pcf);
        put_bits(&s->pb,1, s->umvplus); /* Unrestricted Motion Vector */
        put_bits(&s->pb,1,0); /* SAC: off */
        put_bits(&s->pb,1,s->obmc); /* Advanced Prediction Mode */
        put_bits(&s->pb,1,s->h263_aic); /* Advanced Intra Coding */
        put_bits(&s->pb,1,s->loop_filter); /* Deblocking Filter */
        put_bits(&s->pb,1,s->h263_slice_structured); /* Slice Structured */
        put_bits(&s->pb,1,0); /* Reference Picture Selection: off */
        put_bits(&s->pb,1,0); /* Independent Segment Decoding: off */
        put_bits(&s->pb,1,s->alt_inter_vlc); /* Alternative Inter VLC */
        put_bits(&s->pb,1,s->modified_quant); /* Modified Quantization: */
        put_bits(&s->pb,1,1); /* "1" to prevent start code emulation */
        put_bits(&s->pb,3,0); /* Reserved */

        put_bits(&s->pb, 3, s->pict_type == AV_PICTURE_TYPE_P);

        put_bits(&s->pb,1,0); /* Reference Picture Resampling: off */
        put_bits(&s->pb,1,0); /* Reduced-Resolution Update: off */
        put_bits(&s->pb,1,s->no_rounding); /* Rounding Type */
        put_bits(&s->pb,2,0); /* Reserved */
        put_bits(&s->pb,1,1); /* "1" to prevent start code emulation */

        /* This should be here if PLUSPTYPE */
        put_bits(&s->pb, 1, 0); /* Continuous Presence Multipoint mode: off */

        if (format == 8) {
            /* Custom Picture Format (CPFMT) */
            unsigned aspect_ratio_info = ff_h263_aspect_to_info(s->avctx->sample_aspect_ratio);

            put_bits(&s->pb,4, aspect_ratio_info);
            put_bits(&s->pb,9,(s->width >> 2) - 1);
            put_bits(&s->pb,1,1); /* "1" to prevent start code emulation */
            put_bits(&s->pb,9,(s->height >> 2));
            if (aspect_ratio_info == FF_ASPECT_EXTENDED){
                put_bits(&s->pb, 8, s->avctx->sample_aspect_ratio.num);
                put_bits(&s->pb, 8, s->avctx->sample_aspect_ratio.den);
            }
        }
        if (custom_pcf) {
            if(ufep){
                put_bits(&s->pb, 1, best_clock_code);
                put_bits(&s->pb, 7, best_divisor);
            }
            put_sbits(&s->pb, 2, temp_ref>>8);
        }

        /* Unlimited Unrestricted Motion Vectors Indicator (UUI) */
        if (s->umvplus)
//            put_bits(&s->pb,1,1); /* Limited according tables of Annex D */
//FIXME check actual requested range
            put_bits(&s->pb,2,1); /* unlimited */
        if(s->h263_slice_structured)
            put_bits(&s->pb,2,0); /* no weird submodes */

        put_bits(&s->pb, 5, s->qscale);
    }

    put_bits(&s->pb, 1, 0);     /* no PEI */

    if(s->h263_slice_structured){
        put_bits(&s->pb, 1, 1);

        av_assert1(s->mb_x == 0 && s->mb_y == 0);
        ff_h263_encode_mba(s);

        put_bits(&s->pb, 1, 1);
    }
}

/**
 * Encode a group of blocks header.
 */
void ff_h263_encode_gob_header(MpegEncContext * s, int mb_line)
{
    put_bits(&s->pb, 17, 1); /* GBSC */

    if(s->h263_slice_structured){
        put_bits(&s->pb, 1, 1);

        ff_h263_encode_mba(s);

        if(s->mb_num > 1583)
            put_bits(&s->pb, 1, 1);
        put_bits(&s->pb, 5, s->qscale); /* GQUANT */
        put_bits(&s->pb, 1, 1);
        put_bits(&s->pb, 2, s->pict_type == AV_PICTURE_TYPE_I); /* GFID */
    }else{
        int gob_number= mb_line / s->gob_index;

        put_bits(&s->pb, 5, gob_number); /* GN */
        put_bits(&s->pb, 2, s->pict_type == AV_PICTURE_TYPE_I); /* GFID */
        put_bits(&s->pb, 5, s->qscale); /* GQUANT */
    }
}

/**
 * modify qscale so that encoding is actually possible in H.263 (limit difference to -2..2)
 */
void ff_clean_h263_qscales(MpegEncContext *s){
    int i;
    int8_t * const qscale_table = s->cur_pic.qscale_table;

    for(i=1; i<s->mb_num; i++){
        if(qscale_table[ s->mb_index2xy[i] ] - qscale_table[ s->mb_index2xy[i-1] ] >2)
            qscale_table[ s->mb_index2xy[i] ]= qscale_table[ s->mb_index2xy[i-1] ]+2;
    }
    for(i=s->mb_num-2; i>=0; i--){
        if(qscale_table[ s->mb_index2xy[i] ] - qscale_table[ s->mb_index2xy[i+1] ] >2)
            qscale_table[ s->mb_index2xy[i] ]= qscale_table[ s->mb_index2xy[i+1] ]+2;
    }

    if(s->codec_id != AV_CODEC_ID_H263P){
        for(i=1; i<s->mb_num; i++){
            int mb_xy= s->mb_index2xy[i];

            if(qscale_table[mb_xy] != qscale_table[s->mb_index2xy[i-1]] && (s->mb_type[mb_xy]&CANDIDATE_MB_TYPE_INTER4V)){
                s->mb_type[mb_xy]|= CANDIDATE_MB_TYPE_INTER;
            }
        }
    }
}

static const int dquant_code[5]= {1,0,9,2,3};

/**
 * Encode an 8x8 block.
 * @param block the 8x8 block
 * @param n block index (0-3 are luma, 4-5 are chroma)
 */
static void h263_encode_block(MpegEncContext * s, int16_t * block, int n)
{
    int level, run, last, i, j, last_index, last_non_zero, sign, slevel, code;
    const RLTable *rl;

    rl = &ff_h263_rl_inter;
    if (s->mb_intra && !s->h263_aic) {
        /* DC coef */
        level = block[0];
        /* 255 cannot be represented, so we clamp */
        if (level > 254) {
            level = 254;
            block[0] = 254;
        }
        /* 0 cannot be represented also */
        else if (level < 1) {
            level = 1;
            block[0] = 1;
        }
        if (level == 128) //FIXME check rv10
            put_bits(&s->pb, 8, 0xff);
        else
            put_bits(&s->pb, 8, level);
        i = 1;
    } else {
        i = 0;
        if (s->h263_aic && s->mb_intra)
            rl = &ff_rl_intra_aic;

        if(s->alt_inter_vlc && !s->mb_intra){
            int aic_vlc_bits=0;
            int inter_vlc_bits=0;
            int wrong_pos=-1;
            int aic_code;

            last_index = s->block_last_index[n];
            last_non_zero = i - 1;
            for (; i <= last_index; i++) {
                j = s->intra_scantable.permutated[i];
                level = block[j];
                if (level) {
                    run = i - last_non_zero - 1;
                    last = (i == last_index);

                    if(level<0) level= -level;

                    code = get_rl_index(rl, last, run, level);
                    aic_code = get_rl_index(&ff_rl_intra_aic, last, run, level);
                    inter_vlc_bits += rl->table_vlc[code][1]+1;
                    aic_vlc_bits   += ff_rl_intra_aic.table_vlc[aic_code][1]+1;

                    if (code == rl->n) {
                        inter_vlc_bits += 1+6+8-1;
                    }
                    if (aic_code == ff_rl_intra_aic.n) {
                        aic_vlc_bits += 1+6+8-1;
                        wrong_pos += run + 1;
                    }else
                        wrong_pos += wrong_run[aic_code];
                    last_non_zero = i;
                }
            }
            i = 0;
            if(aic_vlc_bits < inter_vlc_bits && wrong_pos > 63)
                rl = &ff_rl_intra_aic;
        }
    }

    /* AC coefs */
    last_index = s->block_last_index[n];
    last_non_zero = i - 1;
    for (; i <= last_index; i++) {
        j = s->intra_scantable.permutated[i];
        level = block[j];
        if (level) {
            run = i - last_non_zero - 1;
            last = (i == last_index);
            sign = 0;
            slevel = level;
            if (level < 0) {
                sign = 1;
                level = -level;
            }
            code = get_rl_index(rl, last, run, level);
            put_bits(&s->pb, rl->table_vlc[code][1], rl->table_vlc[code][0]);
            if (code == rl->n) {
              if(!CONFIG_FLV_ENCODER || s->h263_flv <= 1){
                put_bits(&s->pb, 1, last);
                put_bits(&s->pb, 6, run);

                av_assert2(slevel != 0);

                if(level < 128)
                    put_sbits(&s->pb, 8, slevel);
                else{
                    put_bits(&s->pb, 8, 128);
                    put_sbits(&s->pb, 5, slevel);
                    put_sbits(&s->pb, 6, slevel>>5);
                }
              }else{
                    ff_flv2_encode_ac_esc(&s->pb, slevel, level, run, last);
              }
            } else {
                put_bits(&s->pb, 1, sign);
            }
            last_non_zero = i;
        }
    }
}

/* Encode MV differences on H.263+ with Unrestricted MV mode */
static void h263p_encode_umotion(PutBitContext *pb, int val)
{
    short sval = 0;
    short i = 0;
    short n_bits = 0;
    short temp_val;
    int code = 0;
    int tcode;

    if ( val == 0)
        put_bits(pb, 1, 1);
    else if (val == 1)
        put_bits(pb, 3, 0);
    else if (val == -1)
        put_bits(pb, 3, 2);
    else {

        sval = ((val < 0) ? (short)(-val):(short)val);
        temp_val = sval;

        while (temp_val != 0) {
            temp_val = temp_val >> 1;
            n_bits++;
        }

        i = n_bits - 1;
        while (i > 0) {
            tcode = (sval & (1 << (i-1))) >> (i-1);
            tcode = (tcode << 1) | 1;
            code = (code << 2) | tcode;
            i--;
        }
        code = ((code << 1) | (val < 0)) << 1;
        put_bits(pb, (2*n_bits)+1, code);
    }
}

static int h263_pred_dc(MpegEncContext * s, int n, int16_t **dc_val_ptr)
{
    int x, y, wrap, a, c, pred_dc;
    int16_t *dc_val;

    /* find prediction */
    if (n < 4) {
        x = 2 * s->mb_x + (n & 1);
        y = 2 * s->mb_y + ((n & 2) >> 1);
        wrap = s->b8_stride;
        dc_val = s->dc_val[0];
    } else {
        x = s->mb_x;
        y = s->mb_y;
        wrap = s->mb_stride;
        dc_val = s->dc_val[n - 4 + 1];
    }
    /* B C
     * A X
     */
    a = dc_val[(x - 1) + (y) * wrap];
    c = dc_val[(x) + (y - 1) * wrap];

    /* No prediction outside GOB boundary */
    if (s->first_slice_line && n != 3) {
        if (n != 2) c = 1024;
        if (n != 1 && s->mb_x == s->resync_mb_x) a = 1024;
    }
    /* just DC prediction */
    if (a != 1024 && c != 1024)
        pred_dc = (a + c) >> 1;
    else if (a != 1024)
        pred_dc = a;
    else
        pred_dc = c;

    /* we assume pred is positive */
    *dc_val_ptr = &dc_val[x + y * wrap];
    return pred_dc;
}

void ff_h263_encode_mb(MpegEncContext * s,
                       int16_t block[6][64],
                       int motion_x, int motion_y)
{
    int cbpc, cbpy, i, cbp, pred_x, pred_y;
    int16_t pred_dc;
    int16_t rec_intradc[6];
    int16_t *dc_ptr[6];
    const int interleaved_stats = s->avctx->flags & AV_CODEC_FLAG_PASS1;

    if (!s->mb_intra) {
        /* compute cbp */
        cbp= get_p_cbp(s, block, motion_x, motion_y);

        if ((cbp | motion_x | motion_y | s->dquant | (s->mv_type - MV_TYPE_16X16)) == 0) {
            /* skip macroblock */
            put_bits(&s->pb, 1, 1);
            if(interleaved_stats){
                s->misc_bits++;
                s->last_bits++;
            }

            return;
        }
        put_bits(&s->pb, 1, 0);         /* mb coded */

        cbpc = cbp & 3;
        cbpy = cbp >> 2;
        if(s->alt_inter_vlc==0 || cbpc!=3)
            cbpy ^= 0xF;
        if(s->dquant) cbpc+= 8;
        if(s->mv_type==MV_TYPE_16X16){
            put_bits(&s->pb,
                    ff_h263_inter_MCBPC_bits[cbpc],
                    ff_h263_inter_MCBPC_code[cbpc]);

            put_bits(&s->pb, ff_h263_cbpy_tab[cbpy][1], ff_h263_cbpy_tab[cbpy][0]);
            if(s->dquant)
                put_bits(&s->pb, 2, dquant_code[s->dquant+2]);

            if(interleaved_stats){
                s->misc_bits+= get_bits_diff(s);
            }

            /* motion vectors: 16x16 mode */
            ff_h263_pred_motion(s, 0, 0, &pred_x, &pred_y);

            if (!s->umvplus) {
                ff_h263_encode_motion_vector(s, motion_x - pred_x,
                                                motion_y - pred_y, 1);
            }
            else {
                h263p_encode_umotion(&s->pb, motion_x - pred_x);
                h263p_encode_umotion(&s->pb, motion_y - pred_y);
                if (((motion_x - pred_x) == 1) && ((motion_y - pred_y) == 1))
                    /* To prevent Start Code emulation */
                    put_bits(&s->pb,1,1);
            }
        }else{
            put_bits(&s->pb,
                    ff_h263_inter_MCBPC_bits[cbpc+16],
                    ff_h263_inter_MCBPC_code[cbpc+16]);
            put_bits(&s->pb, ff_h263_cbpy_tab[cbpy][1], ff_h263_cbpy_tab[cbpy][0]);
            if(s->dquant)
                put_bits(&s->pb, 2, dquant_code[s->dquant+2]);

            if(interleaved_stats){
                s->misc_bits+= get_bits_diff(s);
            }

            for(i=0; i<4; i++){
                /* motion vectors: 8x8 mode*/
                ff_h263_pred_motion(s, i, 0, &pred_x, &pred_y);

                motion_x = s->cur_pic.motion_val[0][s->block_index[i]][0];
                motion_y = s->cur_pic.motion_val[0][s->block_index[i]][1];
                if (!s->umvplus) {
                    ff_h263_encode_motion_vector(s, motion_x - pred_x,
                                                    motion_y - pred_y, 1);
                }
                else {
                    h263p_encode_umotion(&s->pb, motion_x - pred_x);
                    h263p_encode_umotion(&s->pb, motion_y - pred_y);
                    if (((motion_x - pred_x) == 1) && ((motion_y - pred_y) == 1))
                        /* To prevent Start Code emulation */
                        put_bits(&s->pb,1,1);
                }
            }
        }

        if(interleaved_stats){
            s->mv_bits+= get_bits_diff(s);
        }
    } else {
        av_assert2(s->mb_intra);

        cbp = 0;
        if (s->h263_aic) {
            /* Predict DC */
            for(i=0; i<6; i++) {
                int16_t level = block[i][0];
                int scale;

                if(i<4) scale= s->y_dc_scale;
                else    scale= s->c_dc_scale;

                pred_dc = h263_pred_dc(s, i, &dc_ptr[i]);
                level -= pred_dc;
                /* Quant */
                if (level >= 0)
                    level = (level + (scale>>1))/scale;
                else
                    level = (level - (scale>>1))/scale;

                if(!s->modified_quant){
                    if (level < -127)
                        level = -127;
                    else if (level > 127)
                        level = 127;
                }

                block[i][0] = level;
                /* Reconstruction */
                rec_intradc[i] = scale*level + pred_dc;
                /* Oddify */
                rec_intradc[i] |= 1;
                //if ((rec_intradc[i] % 2) == 0)
                //    rec_intradc[i]++;
                /* Clipping */
                if (rec_intradc[i] < 0)
                    rec_intradc[i] = 0;
                else if (rec_intradc[i] > 2047)
                    rec_intradc[i] = 2047;

                /* Update AC/DC tables */
                *dc_ptr[i] = rec_intradc[i];
                /* AIC can change CBP */
                if (s->block_last_index[i] > 0 ||
                    (s->block_last_index[i] == 0 && level !=0))
                    cbp |= 1 << (5 - i);
            }
        }else{
            for(i=0; i<6; i++) {
                /* compute cbp */
                if (s->block_last_index[i] >= 1)
                    cbp |= 1 << (5 - i);
            }
        }

        cbpc = cbp & 3;
        if (s->pict_type == AV_PICTURE_TYPE_I) {
            if(s->dquant) cbpc+=4;
            put_bits(&s->pb,
                ff_h263_intra_MCBPC_bits[cbpc],
                ff_h263_intra_MCBPC_code[cbpc]);
        } else {
            if(s->dquant) cbpc+=8;
            put_bits(&s->pb, 1, 0);     /* mb coded */
            put_bits(&s->pb,
                ff_h263_inter_MCBPC_bits[cbpc + 4],
                ff_h263_inter_MCBPC_code[cbpc + 4]);
        }
        if (s->h263_aic) {
            /* XXX: currently, we do not try to use ac prediction */
            put_bits(&s->pb, 1, 0);     /* no AC prediction */
        }
        cbpy = cbp >> 2;
        put_bits(&s->pb, ff_h263_cbpy_tab[cbpy][1], ff_h263_cbpy_tab[cbpy][0]);
        if(s->dquant)
            put_bits(&s->pb, 2, dquant_code[s->dquant+2]);

        if(interleaved_stats){
            s->misc_bits+= get_bits_diff(s);
        }
    }

    for(i=0; i<6; i++) {
        /* encode each block */
        h263_encode_block(s, block[i], i);

        /* Update INTRADC for decoding */
        if (s->h263_aic && s->mb_intra) {
            block[i][0] = rec_intradc[i];

        }
    }

    if(interleaved_stats){
        if (!s->mb_intra) {
            s->p_tex_bits+= get_bits_diff(s);
        }else{
            s->i_tex_bits+= get_bits_diff(s);
            s->i_count++;
        }
    }
}

void ff_h263_update_mb(MpegEncContext *s)
{
    const int mb_xy = s->mb_y * s->mb_stride + s->mb_x;

    if (s->cur_pic.mbskip_table)
        s->cur_pic.mbskip_table[mb_xy] = s->mb_skipped;

    if (s->mv_type == MV_TYPE_8X8)
        s->cur_pic.mb_type[mb_xy] = MB_TYPE_FORWARD_MV | MB_TYPE_8x8;
    else if(s->mb_intra)
        s->cur_pic.mb_type[mb_xy] = MB_TYPE_INTRA;
    else
        s->cur_pic.mb_type[mb_xy] = MB_TYPE_FORWARD_MV | MB_TYPE_16x16;

    ff_h263_update_motion_val(s);
}

av_cold void ff_h263_encode_init(MpegEncContext *s)
{
    s->me.mv_penalty = ff_h263_get_mv_penalty(); // FIXME exact table for MSMPEG4 & H.263+

    s->intra_ac_vlc_length     =s->inter_ac_vlc_length     = uni_h263_inter_rl_len;
    s->intra_ac_vlc_last_length=s->inter_ac_vlc_last_length= uni_h263_inter_rl_len + 128*64;
    if(s->h263_aic){
        s->intra_ac_vlc_length     = uni_h263_intra_aic_rl_len;
        s->intra_ac_vlc_last_length= uni_h263_intra_aic_rl_len + 128*64;

        s->y_dc_scale_table =
        s->c_dc_scale_table = ff_aic_dc_scale_table;
    }
    s->ac_esc_length= 7+1+6+8;

    if (s->modified_quant)
        s->chroma_qscale_table = ff_h263_chroma_qscale_table;

    // use fcodes >1 only for MPEG-4 & H.263 & H.263+ FIXME
    switch(s->codec_id){
    case AV_CODEC_ID_MPEG4:
        break;
    case AV_CODEC_ID_H263P:
        if(s->umvplus)
            s->fcode_tab = umv_fcode_tab + MAX_MV;
        if(s->modified_quant){
            s->min_qcoeff= -2047;
            s->max_qcoeff=  2047;
        }else{
            s->min_qcoeff= -127;
            s->max_qcoeff=  127;
        }
        break;
        // Note for MPEG-4 & H.263 the dc-scale table will be set per frame as needed later
    case AV_CODEC_ID_FLV1:
        if (s->h263_flv > 1) {
            s->min_qcoeff= -1023;
            s->max_qcoeff=  1023;
        } else {
            s->min_qcoeff= -127;
            s->max_qcoeff=  127;
        }
        break;
    default: //nothing needed - default table already set in mpegvideo.c
        s->min_qcoeff= -127;
        s->max_qcoeff=  127;
    }

    ff_h263dsp_init(&s->h263dsp);
}

void ff_h263_encode_mba(MpegEncContext *s)
{
    int i, mb_pos;

    for(i=0; i<6; i++){
        if(s->mb_num-1 <= ff_mba_max[i]) break;
    }
    mb_pos= s->mb_x + s->mb_width*s->mb_y;
    put_bits(&s->pb, ff_mba_length[i], mb_pos);
}

#define OFFSET(x) offsetof(MpegEncContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption h263_options[] = {
    { "obmc",         "use overlapped block motion compensation.", OFFSET(obmc), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, VE },
    { "mb_info",      "emit macroblock info for RFC 2190 packetization, the parameter value is the maximum payload size", OFFSET(mb_info), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, VE },
    FF_MPV_COMMON_OPTS
    FF_MPV_COMMON_MOTION_EST_OPTS
    { NULL },
};

static const AVClass h263_class = {
    .class_name = "H.263 encoder",
    .option     = h263_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_h263_encoder = {
    .p.name         = "h263",
    CODEC_LONG_NAME("H.263 / H.263-1996"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_H263,
    .p.pix_fmts = (const enum AVPixelFormat[]){AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE},
    .color_ranges   = AVCOL_RANGE_MPEG,
    .p.priv_class   = &h263_class,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
    .priv_data_size = sizeof(MpegEncContext),
    .init           = ff_mpv_encode_init,
    FF_CODEC_ENCODE_CB(ff_mpv_encode_picture),
    .close          = ff_mpv_encode_end,
};

static const AVOption h263p_options[] = {
    { "umv",        "Use unlimited motion vectors.",    OFFSET(umvplus),       AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, VE },
    { "aiv",        "Use alternative inter VLC.",       OFFSET(alt_inter_vlc), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, VE },
    { "obmc",       "use overlapped block motion compensation.", OFFSET(obmc), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, VE },
    { "structured_slices", "Write slice start position at every GOB header instead of just GOB number.", OFFSET(h263_slice_structured), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, VE},
    FF_MPV_COMMON_OPTS
    FF_MPV_COMMON_MOTION_EST_OPTS
    { NULL },
};
static const AVClass h263p_class = {
    .class_name = "H.263p encoder",
    .option     = h263p_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_h263p_encoder = {
    .p.name         = "h263p",
    CODEC_LONG_NAME("H.263+ / H.263-1998 / H.263 version 2"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_H263P,
    .p.pix_fmts     = (const enum AVPixelFormat[]){ AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE },
    .color_ranges   = AVCOL_RANGE_MPEG,
    .p.priv_class   = &h263p_class,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_SLICE_THREADS |
                      AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
    .priv_data_size = sizeof(MpegEncContext),
    .init           = ff_mpv_encode_init,
    FF_CODEC_ENCODE_CB(ff_mpv_encode_picture),
    .close          = ff_mpv_encode_end,
};
#endif
