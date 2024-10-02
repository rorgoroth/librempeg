/*
 * Copyright (c) 2023 Paul B Mahol
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with FFmpeg; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <float.h>

#include "libavutil/channel_layout.h"
#include "libavutil/opt.h"
#include "libavutil/mem.h"
#include "libavutil/tx.h"
#include "audio.h"
#include "avfilter.h"
#include "filters.h"
#include "formats.h"

enum operation {
    OP_LEFT,
    OP_RIGHT,
    OP_STEREO,
    NB_OPERATION
};

typedef struct StereoFieldContext {
    const AVClass *class;

    double D;
    int mode;

    int fft_size;
    int overlap;

    void *window;

    AVFrame *in;
    AVFrame *in_frame;
    AVFrame *out_dist_frame;
    AVFrame *windowed_frame;
    AVFrame *windowed_out;

    int (*sf_stereo)(AVFilterContext *ctx, AVFrame *out);

    AVTXContext *tx_ctx, *itx_ctx;
    av_tx_fn tx_fn, itx_fn;
} StereoFieldContext;

#define OFFSET(x) offsetof(StereoFieldContext, x)
#define FLAGS AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption stereofield_options[] = {
    { "d", "set the depth", OFFSET(D), AV_OPT_TYPE_DOUBLE, {.dbl=0.}, -1, 1, FLAGS },
    { "o", "set the operating mode", OFFSET(mode), AV_OPT_TYPE_INT, {.i64=OP_STEREO}, 0, NB_OPERATION-1, FLAGS, "mode" },
    {  "l", "left",   0, AV_OPT_TYPE_CONST, {.i64=OP_LEFT},   0, 0, FLAGS, .unit = "mode"},
    {  "r", "right",  0, AV_OPT_TYPE_CONST, {.i64=OP_RIGHT},  0, 0, FLAGS, .unit = "mode"},
    {  "s", "stereo", 0, AV_OPT_TYPE_CONST, {.i64=OP_STEREO}, 0, 0, FLAGS, .unit = "mode"},
    {NULL}
};

AVFILTER_DEFINE_CLASS(stereofield);

static int query_formats(const AVFilterContext *ctx,
                         AVFilterFormatsConfig **cfg_in,
                         AVFilterFormatsConfig **cfg_out)
{
    static const enum AVSampleFormat formats[] = {
        AV_SAMPLE_FMT_FLTP,
        AV_SAMPLE_FMT_DBLP,
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

    return ff_set_common_channel_layouts_from_list2(ctx, cfg_in, cfg_out, layouts);
}

#define DEPTH 32
#include "stereofield_template.c"

#undef DEPTH
#define DEPTH 64
#include "stereofield_template.c"

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    StereoFieldContext *s = ctx->priv;
    int ret;

    s->fft_size = 1 << av_ceil_log2((inlink->sample_rate + 19) / 20);
    s->overlap = (s->fft_size + 3) / 4;

    s->in_frame       = ff_get_audio_buffer(inlink, (s->fft_size + 2) * 2);
    s->out_dist_frame = ff_get_audio_buffer(inlink, (s->fft_size + 2) * 2);
    s->windowed_frame = ff_get_audio_buffer(inlink, (s->fft_size + 2) * 2);
    s->windowed_out   = ff_get_audio_buffer(inlink, (s->fft_size + 2) * 2);
    if (!s->in_frame || !s->windowed_out || !s->out_dist_frame || !s->windowed_frame)
        return AVERROR(ENOMEM);

    switch (inlink->format) {
    case AV_SAMPLE_FMT_FLTP:
        s->sf_stereo = sf_stereo_float;
        ret = sf_tx_init_float(ctx);
        break;
    case AV_SAMPLE_FMT_DBLP:
        s->sf_stereo = sf_stereo_double;
        ret = sf_tx_init_double(ctx);
        break;
    }

    return ret;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    StereoFieldContext *s = ctx->priv;
    AVFrame *out;
    int ret;

    out = ff_get_audio_buffer(outlink, s->overlap);
    if (!out) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    s->in = in;
    s->sf_stereo(ctx, out);

    av_frame_copy_props(out, in);
    out->pts -= av_rescale_q(s->fft_size - s->overlap, av_make_q(1, outlink->sample_rate), outlink->time_base);
    out->nb_samples = in->nb_samples;
    ret = ff_filter_frame(outlink, out);
fail:
    av_frame_free(&in);
    s->in = NULL;
    return ret < 0 ? ret : 0;
}

static int activate(AVFilterContext *ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    StereoFieldContext *s = ctx->priv;
    AVFrame *in = NULL;
    int ret = 0, status;
    int64_t pts;

    FF_FILTER_FORWARD_STATUS_BACK(outlink, inlink);

    ret = ff_inlink_consume_samples(inlink, s->overlap, s->overlap, &in);
    if (ret < 0)
        return ret;

    if (ret > 0) {
        return filter_frame(inlink, in);
    } else if (ff_inlink_acknowledge_status(inlink, &status, &pts)) {
        ff_outlink_set_status(outlink, status, pts);
        return 0;
    } else {
        if (ff_inlink_queued_samples(inlink) >= s->overlap) {
            ff_filter_set_ready(ctx, 10);
        } else if (ff_outlink_frame_wanted(outlink)) {
            ff_inlink_request_frame(inlink);
        }
        return 0;
    }
}

static av_cold void uninit(AVFilterContext *ctx)
{
    StereoFieldContext *s = ctx->priv;

    av_freep(&s->window);

    av_frame_free(&s->in_frame);
    av_frame_free(&s->out_dist_frame);
    av_frame_free(&s->windowed_frame);
    av_frame_free(&s->windowed_out);

    av_tx_uninit(&s->tx_ctx);
    av_tx_uninit(&s->itx_ctx);
}

static const AVFilterPad inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .config_props = config_input,
    },
};

const AVFilter ff_af_stereofield = {
    .name            = "stereofield",
    .description     = NULL_IF_CONFIG_SMALL("Apply Stereo Field effect."),
    .priv_size       = sizeof(StereoFieldContext),
    .priv_class      = &stereofield_class,
    .uninit          = uninit,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(ff_audio_default_filterpad),
    FILTER_QUERY_FUNC2(query_formats),
    .flags           = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL,
    .activate        = activate,
    .process_command = ff_filter_process_command,
};
