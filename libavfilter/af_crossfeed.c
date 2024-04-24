/*
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
#include "libavutil/ffmath.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "audio.h"
#include "filters.h"
#include "formats.h"

typedef struct CrossfeedContext {
    const AVClass *class;

    double range;
    double strength;
    double slope;
    double level_in;
    double level_out;
    int block_samples;
    int block_size;

    double a0, a1, a2;
    double b0, b1, b2;

    double w1, w2;
    double rw1, rw2;

    int64_t pts;
    int nb_samples;

    double *mid;
    double *side[3];
} CrossfeedContext;

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats = NULL;
    AVFilterChannelLayouts *layout = NULL;
    int ret;

    if ((ret = ff_add_format                 (&formats, AV_SAMPLE_FMT_DBL  )) < 0 ||
        (ret = ff_set_common_formats         (ctx     , formats            )) < 0 ||
        (ret = ff_add_channel_layout         (&layout , &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO)) < 0 ||
        (ret = ff_set_common_channel_layouts (ctx     , layout             )) < 0 ||
        (ret = ff_set_common_all_samplerates (ctx                          )) < 0)
        return ret;

    return 0;
}

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    CrossfeedContext *s = ctx->priv;
    double A = ff_exp10(s->strength * -30 / 40);
    double g, k, Q;

    Q = 1. / sqrt((A + 1. / A) * (1. / s->slope - 1.) + 2.);
    g = tan(M_PI * (1. - s->range) * 2100. / inlink->sample_rate) / sqrt(A);
    k = 1. / Q;

    s->a0 = 1. / (1. + g * (g + k));
    s->a1 = g * s->a0;
    s->a2 = g * s->a1;
    s->b0 = 1.;
    s->b1 = k * (A - 1.);
    s->b2 = A * A - 1.;

    if (s->block_samples == 0 && s->block_size > 0) {
        s->pts = AV_NOPTS_VALUE;
        s->block_samples = s->block_size;
        s->mid = av_calloc(s->block_samples * 2, sizeof(*s->mid));
        for (int i = 0; i < 3; i++) {
            s->side[i] = av_calloc(s->block_samples * 2, sizeof(*s->side[0]));
            if (!s->side[i])
                return AVERROR(ENOMEM);
        }
    }

    return 0;
}

static void reverse_samples(double *dst, const double *src,
                            int nb_samples)
{
    for (int i = 0, j = nb_samples - 1; i < nb_samples; i++, j--)
        dst[i] = src[j];
}

static void filter_samples(double *dst, const double *src,
                           int nb_samples,
                           double m0, double m1, double m2,
                           double a0, double a1, double a2,
                           double *sw1, double *sw2)
{
    double w1 = *sw1;
    double w2 = *sw2;

    for (int n = 0; n < nb_samples; n++) {
        const double in = src[n];
        const double v0 = in;
        const double v3 = v0 - w2;
        const double v1 = a0 * w1 + a1 * v3;
        const double v2 = w2 + a1 * w1 + a2 * v3;

        w1 = 2.0 * v1 - w1;
        w2 = 2.0 * v2 - w2;

        dst[n] = m0 * v0 + m1 * v1 + m2 * v2;
    }

    *sw1 = w1;
    *sw2 = w2;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    CrossfeedContext *s = ctx->priv;
    const double *src = (const double *)in->data[0];
    const int is_disabled = ctx->is_disabled;
    const double level_in = s->level_in;
    const double level_out = s->level_out;
    const double b0 = s->b0;
    const double b1 = s->b1;
    const double b2 = s->b2;
    const double a0 = s->a0;
    const double a1 = s->a1;
    const double a2 = s->a2;
    AVFrame *out;
    int drop = 0;
    double *dst;

    if (av_frame_is_writable(in) && s->block_samples == 0) {
        out = in;
    } else {
        out = ff_get_audio_buffer(outlink, s->block_samples > 0 ? s->block_samples : in->nb_samples);
        if (!out) {
            av_frame_free(&in);
            return AVERROR(ENOMEM);
        }
        av_frame_copy_props(out, in);
    }
    dst = (double *)out->data[0];

    if (s->block_samples > 0 && s->pts == AV_NOPTS_VALUE)
        drop = 1;

    if (s->block_samples == 0) {
        const int nb_samples = out->nb_samples;
        double w1 = s->w1;
        double w2 = s->w2;

        for (int n = 0; n < nb_samples; n++, src += 2, dst += 2) {
            const double mid = (src[0] + src[1]) * level_in * .5;
            const double side = (src[0] - src[1]) * level_in * .5;
            const double in = side;
            const double v0 = in;
            const double v3 = v0 - w2;
            const double v1 = a0 * w1 + a1 * v3;
            const double v2 = w2 + a1 * w1 + a2 * v3;
            double oside;

            w1 = 2.0 * v1 - w1;
            w2 = 2.0 * v2 - w2;

            oside = b0 * v0 + b1 * v1 + b2 * v2;

            if (is_disabled) {
                dst[0] = src[0];
                dst[1] = src[1];
            } else {
                dst[0] = (mid + oside) * level_out;
                dst[1] = (mid - oside) * level_out;
            }
        }

        s->w1 = isnormal(w1) ? w1 : 0.0;
        s->w2 = isnormal(w2) ? w2 : 0.0;
    } else {
        const int block_samples = s->block_samples;
        const int nb_samples = in->nb_samples;
        double *mdst = s->mid + s->block_samples;
        double *sdst = s->side[0] + s->block_samples;
        double w1 = s->w1;
        double w2 = s->w2;

        for (int n = 0; n < nb_samples; n++, src += 2) {
            mdst[n] = (src[0] + src[1]) * level_in * .5;
            sdst[n] = (src[0] - src[1]) * level_in * .5;
        }

        for (int n = nb_samples; n < block_samples; n++) {
            mdst[n] = 0.0;
            sdst[n] = 0.0;
        }

        filter_samples(sdst, sdst, nb_samples,
                       b0, b1, b2, a0, a1, a2, &w1, &w2);

        s->w1 = isnormal(w1) ? w1 : 0.0;
        s->w2 = isnormal(w2) ? w2 : 0.0;

        reverse_samples(s->side[1], s->side[0], block_samples * 2);
        filter_samples(s->side[1], s->side[1], block_samples * 2,
                       b0, b1, b2, a0, a1, a2, &s->rw1, &s->rw2);
        s->rw1 = isnormal(s->rw1) ? s->rw1 : 0.0;
        s->rw2 = isnormal(s->rw2) ? s->rw2 : 0.0;
        reverse_samples(s->side[2], s->side[1], block_samples * 2);

        src = (const double *)in->data[0];
        mdst = s->mid;
        sdst = s->side[2];
        for (int n = 0; n < block_samples; n++, src += 2, dst += 2) {
            if (is_disabled) {
                dst[0] = src[0];
                dst[1] = src[1];
            } else {
                dst[0] = (mdst[n] + sdst[n]) * level_out;
                dst[1] = (mdst[n] - sdst[n]) * level_out;
            }
        }

        memcpy(s->mid, s->mid + block_samples,
               block_samples * sizeof(*s->mid));
        memcpy(s->side[0], s->side[0] + block_samples,
               block_samples * sizeof(*s->side[0]));
    }

    if (s->block_samples > 0) {
        int nb_samples = in->nb_samples;
        int64_t pts = in->pts;

        out->pts = s->pts;
        out->nb_samples = s->nb_samples;
        s->pts = pts;
        s->nb_samples = nb_samples;
    }

    if (out != in)
        av_frame_free(&in);
    if (!drop) {
        return ff_filter_frame(outlink, out);
    } else {
        av_frame_free(&out);
        ff_filter_set_ready(ctx, 10);
        return 0;
    }
}

static int activate(AVFilterContext *ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    CrossfeedContext *s = ctx->priv;
    AVFrame *in = NULL;
    int64_t pts;
    int status;
    int ret;

    FF_FILTER_FORWARD_STATUS_BACK(outlink, inlink);

    if (s->block_samples > 0) {
        ret = ff_inlink_consume_samples(inlink, s->block_samples, s->block_samples, &in);
    } else {
        ret = ff_inlink_consume_frame(inlink, &in);
    }
    if (ret < 0)
        return ret;
    if (ret > 0)
        return filter_frame(inlink, in);

    if (s->block_samples > 0 && ff_inlink_queued_samples(inlink) >= s->block_samples) {
        ff_filter_set_ready(ctx, 10);
        return 0;
    }

    if (ff_inlink_acknowledge_status(inlink, &status, &pts)) {
        if (s->block_samples > 0) {
            AVFrame *in = ff_get_audio_buffer(outlink, s->block_samples);
            if (!in)
                return AVERROR(ENOMEM);

            ret = filter_frame(inlink, in);
        }

        ff_outlink_set_status(outlink, status, pts);

        return ret;
    }

    FF_FILTER_FORWARD_WANTED(outlink, inlink);

    return FFERROR_NOT_READY;
}

static int process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                           char *res, int res_len, int flags)
{
    int ret;

    ret = ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    if (ret < 0)
        return ret;

    return config_input(ctx->inputs[0]);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    CrossfeedContext *s = ctx->priv;

    av_freep(&s->mid);
    for (int i = 0; i < 3; i++)
        av_freep(&s->side[i]);
}

#define OFFSET(x) offsetof(CrossfeedContext, x)
#define FLAGS AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_RUNTIME_PARAM
#define AF AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption crossfeed_options[] = {
    { "strength",  "set crossfeed strength",  OFFSET(strength),  AV_OPT_TYPE_DOUBLE, {.dbl=.2}, 0, 1, FLAGS },
    { "range",     "set soundstage wideness", OFFSET(range),     AV_OPT_TYPE_DOUBLE, {.dbl=.5}, 0, 1, FLAGS },
    { "slope",     "set curve slope",         OFFSET(slope),     AV_OPT_TYPE_DOUBLE, {.dbl=.5}, .01, 1, FLAGS },
    { "level_in",  "set level in",            OFFSET(level_in),  AV_OPT_TYPE_DOUBLE, {.dbl=.9}, 0, 1, FLAGS },
    { "level_out", "set level out",           OFFSET(level_out), AV_OPT_TYPE_DOUBLE, {.dbl=1.}, 0, 1, FLAGS },
    { "block_size", "set the block size",     OFFSET(block_size),AV_OPT_TYPE_INT,    {.i64=0}, 0, 32768, AF },
    { NULL }
};

AVFILTER_DEFINE_CLASS(crossfeed);

static const AVFilterPad inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .config_props = config_input,
    },
};

const AVFilter ff_af_crossfeed = {
    .name           = "crossfeed",
    .description    = NULL_IF_CONFIG_SMALL("Apply headphone crossfeed filter."),
    .priv_size      = sizeof(CrossfeedContext),
    .priv_class     = &crossfeed_class,
    .activate       = activate,
    .uninit         = uninit,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(ff_audio_default_filterpad),
    FILTER_QUERY_FUNC(query_formats),
    .flags          = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL,
    .process_command = process_command,
};
