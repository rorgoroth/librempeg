/*
 * Copyright (c) 2010 S.N. Hemanth Meenakshisundaram <smeenaks@ucsd.edu>
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

#include "libavutil/internal.h"
#include "avfilter.h"
#include "filters.h"
#include "audio.h"

typedef struct ANullSinkContext {
    const AVClass *class;
    unsigned eof;
} ANullSinkContext;

static int activate(AVFilterContext *ctx)
{
    ANullSinkContext *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    AVFrame *in;
    int64_t pts;
    int status;

    if (ff_inlink_queued_frames(inlink) > 0) {
        int ret = ff_inlink_consume_frame(inlink, &in);
        if (ret > 0)
            av_frame_free(&in);
    }
    if (ff_inlink_acknowledge_status(inlink, &status, &pts))
        s->eof = 1;

    if (s->eof)
        return AVERROR_EOF;
    return 0;
}

const AVFilter ff_asink_anullsink = {
    .name        = "anullsink",
    .description = NULL_IF_CONFIG_SMALL("Do absolutely nothing with the input audio."),
    .priv_size   = sizeof(ANullSinkContext),
    .activate    = activate,
    FILTER_INPUTS(ff_audio_default_filterpad),
    .outputs     = NULL,
};
