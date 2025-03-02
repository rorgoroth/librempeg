/*
 * MPEG-1/2 encoder header
 * Copyright (c) 2007 Aurelien Jacobs <aurel@gnuage.org>
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

#ifndef AVCODEC_MPEG12ENC_H
#define AVCODEC_MPEG12ENC_H

#include <stdint.h>

#include "mpegvideoenc.h"
#include "mpegvideodata.h"

void ff_mpeg1_encode_picture_header(MPVMainEncContext *m);
void ff_mpeg1_encode_mb(MpegEncContext *s, int16_t block[8][64],
                        int motion_x, int motion_y);
void ff_mpeg1_encode_slice_header(MpegEncContext *s);

// Must not be called before intra_dc_precision has been sanitized in ff_mpv_encode_init()
static inline void ff_mpeg1_encode_init(MpegEncContext *s)
{
    s->y_dc_scale_table =
    s->c_dc_scale_table = ff_mpeg12_dc_scale_table[s->intra_dc_precision];
}

#endif /* AVCODEC_MPEG12ENC_H */
