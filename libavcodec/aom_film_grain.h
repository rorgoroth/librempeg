/*
 * AOM film grain synthesis
 * Copyright (c) 2021 Niklas Haas <ffmpeg@haasn.xyz>
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
 * AOM film grain synthesis.
 * @author Niklas Haas <ffmpeg@haasn.xyz>
 */

#ifndef AVCODEC_AOM_FILM_GRAIN_H
#define AVCODEC_AOM_FILM_GRAIN_H

#include "libavutil/film_grain_params.h"

// Synthesizes film grain on top of `in` and stores the result to `out`. `out`
// must already have been allocated and set to the same size and format as `in`.
int ff_aom_apply_film_grain(AVFrame *out, const AVFrame *in,
                            const AVFilmGrainParams *params);

#endif /* AVCODEC_AOM_FILM_GRAIN_H */
