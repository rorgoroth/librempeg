/*
 * Copyright (c) 2015 The FFmpeg Project
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

#include "libavutil/channel_layout.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "audio.h"
#include "filters.h"
#include "formats.h"

typedef struct ExtraStereoContext {
    const AVClass *class;
    float mult;
    int clip;
    void (*do_extrastereo)(AVFilterContext *ctx, AVFrame *in, AVFrame *out, const int clip);
} ExtraStereoContext;

#define OFFSET(x) offsetof(ExtraStereoContext, x)
#define A AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption extrastereo_options[] = {
    { "m", "set the difference coefficient", OFFSET(mult), AV_OPT_TYPE_FLOAT, {.dbl=2.5}, -10, 10, A },
    { "c", "enable clipping",                OFFSET(clip), AV_OPT_TYPE_BOOL,  {.i64=1},     0,  1, A },
    { NULL }
};

AVFILTER_DEFINE_CLASS(extrastereo);

static int query_formats(const AVFilterContext *ctx,
                         AVFilterFormatsConfig **cfg_in,
                         AVFilterFormatsConfig **cfg_out)
{
    static const enum AVSampleFormat formats[] = {
        AV_SAMPLE_FMT_FLT,
        AV_SAMPLE_FMT_DBL,
        AV_SAMPLE_FMT_NONE,
    };
    static const AVChannelLayout layouts[] = {
        AV_CHANNEL_LAYOUT_STEREO,
        { .nb_channels = 0 },
    };

     int ret;

    ret = ff_set_common_formats_from_list2(ctx, cfg_in, cfg_out, formats);
    if (ret < 0)
        return ret;

    ret = ff_set_common_channel_layouts_from_list2(ctx, cfg_in, cfg_out, layouts);
    if (ret < 0)
        return ret;

    return 0;
}

#define DEPTH 32
#include "extrastereo_template.c"

#undef DEPTH
#define DEPTH 64
#include "extrastereo_template.c"

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    ExtraStereoContext *s = ctx->priv;

    switch (inlink->format) {
    case AV_SAMPLE_FMT_FLT:
        s->do_extrastereo = extrastereo_flt;
        break;
    case AV_SAMPLE_FMT_DBL:
        s->do_extrastereo = extrastereo_dbl;
        break;
    }

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    ExtraStereoContext *s = ctx->priv;
    AVFrame *out;

    if (av_frame_is_writable(in)) {
        out = in;
    } else {
        out = ff_get_audio_buffer(outlink, in->nb_samples);
        if (!out) {
            av_frame_free(&in);
            return AVERROR(ENOMEM);
        }
        av_frame_copy_props(out, in);
    }

    s->do_extrastereo(ctx, in, out, s->clip);

    if (out != in)
        av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

static const AVFilterPad inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .filter_frame = filter_frame,
        .config_props = config_input,
    },
};

const AVFilter ff_af_extrastereo = {
    .name           = "extrastereo",
    .description    = NULL_IF_CONFIG_SMALL("Increase difference between stereo audio channels."),
    .priv_size      = sizeof(ExtraStereoContext),
    .priv_class     = &extrastereo_class,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(ff_audio_default_filterpad),
    FILTER_QUERY_FUNC2(query_formats),
    .flags          = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
    .process_command = ff_filter_process_command,
};
