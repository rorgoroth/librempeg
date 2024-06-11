/*
 * Copyright (c) 2020 Paul B Mahol
 *
 * Speech Normalizer
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
 * Speech Normalizer
 */

#include <float.h>

#include "libavutil/avassert.h"
#include "libavutil/channel_layout.h"
#include "libavutil/fifo.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "audio.h"
#include "avfilter.h"
#include "filters.h"
#include "internal.h"

#define MAX_ITEMS  882000
#define MIN_PEAK (1. / 32768.)

typedef struct PeriodItem {
    int size;
    int type;
    double max_peak;
    double rms_sum;
} PeriodItem;

typedef struct ChannelContext {
    int state;
    int bypass;
    PeriodItem pi[MAX_ITEMS];
    double gain_state;
    double pi_max_peak;
    double pi_rms_sum;
    int pi_start;
    int pi_end;
    int pi_size;
    int acc;
} ChannelContext;

typedef struct SpeechNormalizerContext {
    const AVClass *class;

    double rms_value;
    double peak_value;
    double max_expansion;
    double max_compression;
    double threshold_value;
    double raise_amount;
    double fall_amount;
    char *ch_layout_str;
    AVChannelLayout ch_layout;
    int invert;
    int link;

    ChannelContext *cc;
    double prev_gain;

    int eof;
    int64_t pts;

    AVFifo *fifo;

    void (*analyze_channel)(AVFilterContext *ctx, ChannelContext *cc,
                            const uint8_t *srcp, int nb_samples);
    void (*filter_channels[2])(AVFilterContext *ctx,
                               AVFrame *in, AVFrame *out, int nb_samples);
} SpeechNormalizerContext;

#define OFFSET(x) offsetof(SpeechNormalizerContext, x)
#define FLAGS AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption speechnorm_options[] = {
    { "peak", "set the peak value", OFFSET(peak_value), AV_OPT_TYPE_DOUBLE, {.dbl=0.95}, 0.0, 1.0, FLAGS },
    { "p",    "set the peak value", OFFSET(peak_value), AV_OPT_TYPE_DOUBLE, {.dbl=0.95}, 0.0, 1.0, FLAGS },
    { "expansion", "set the max expansion factor", OFFSET(max_expansion), AV_OPT_TYPE_DOUBLE, {.dbl=2.0}, 1.0, 50.0, FLAGS },
    { "e",         "set the max expansion factor", OFFSET(max_expansion), AV_OPT_TYPE_DOUBLE, {.dbl=2.0}, 1.0, 50.0, FLAGS },
    { "compression", "set the max compression factor", OFFSET(max_compression), AV_OPT_TYPE_DOUBLE, {.dbl=2.0}, 1.0, 50.0, FLAGS },
    { "c",           "set the max compression factor", OFFSET(max_compression), AV_OPT_TYPE_DOUBLE, {.dbl=2.0}, 1.0, 50.0, FLAGS },
    { "threshold", "set the threshold value", OFFSET(threshold_value), AV_OPT_TYPE_DOUBLE, {.dbl=0}, 0.0, 1.0, FLAGS },
    { "t",         "set the threshold value", OFFSET(threshold_value), AV_OPT_TYPE_DOUBLE, {.dbl=0}, 0.0, 1.0, FLAGS },
    { "raise", "set the expansion raising amount", OFFSET(raise_amount), AV_OPT_TYPE_DOUBLE, {.dbl=0.001}, 0.0, 1.0, FLAGS },
    { "r",     "set the expansion raising amount", OFFSET(raise_amount), AV_OPT_TYPE_DOUBLE, {.dbl=0.001}, 0.0, 1.0, FLAGS },
    { "fall", "set the compression raising amount", OFFSET(fall_amount), AV_OPT_TYPE_DOUBLE, {.dbl=0.001}, 0.0, 1.0, FLAGS },
    { "f",    "set the compression raising amount", OFFSET(fall_amount), AV_OPT_TYPE_DOUBLE, {.dbl=0.001}, 0.0, 1.0, FLAGS },
    { "channels", "set channels to filter", OFFSET(ch_layout_str), AV_OPT_TYPE_STRING, {.str="all"}, 0, 0, FLAGS },
    { "h",        "set channels to filter", OFFSET(ch_layout_str), AV_OPT_TYPE_STRING, {.str="all"}, 0, 0, FLAGS },
    { "invert", "set inverted filtering", OFFSET(invert), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS },
    { "i",      "set inverted filtering", OFFSET(invert), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS },
    { "link", "set linked channels filtering", OFFSET(link), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS },
    { "l",    "set linked channels filtering", OFFSET(link), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS },
    { "rms", "set the RMS value", OFFSET(rms_value), AV_OPT_TYPE_DOUBLE, {.dbl=0.0}, 0.0, 1.0, FLAGS },
    { "m",   "set the RMS value", OFFSET(rms_value), AV_OPT_TYPE_DOUBLE, {.dbl=0.0}, 0.0, 1.0, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(speechnorm);

static int get_pi_samples(ChannelContext *cc, int eof)
{
    if (eof) {
        PeriodItem *pi = cc->pi;

        return cc->acc + pi[cc->pi_end].size;
    } else {
        PeriodItem *pi = cc->pi;

        if (pi[cc->pi_start].type == 0)
            return cc->pi_size;
    }

    return cc->acc;
}

static int available_samples(AVFilterContext *ctx)
{
    SpeechNormalizerContext *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    int min_pi_nb_samples;

    min_pi_nb_samples = get_pi_samples(&s->cc[0], s->eof);
    for (int ch = 1; ch < inlink->ch_layout.nb_channels && min_pi_nb_samples > 0; ch++) {
        ChannelContext *cc = &s->cc[ch];

        min_pi_nb_samples = FFMIN(min_pi_nb_samples, get_pi_samples(cc, s->eof));
    }

    return min_pi_nb_samples;
}

static void consume_pi(ChannelContext *cc, int nb_samples)
{
    if (cc->pi_size >= nb_samples) {
        cc->pi_size -= nb_samples;
        cc->acc -= FFMIN(cc->acc, nb_samples);
    } else {
        av_assert1(0);
    }
}

static double next_gain(AVFilterContext *ctx, double pi_max_peak, int bypass, double state,
                        double pi_rms_sum, int pi_size, double scale)
{
    SpeechNormalizerContext *s = ctx->priv;
    const double compression = 1. / s->max_compression;
    const int type = s->invert ? pi_max_peak <= s->threshold_value : pi_max_peak >= s->threshold_value;
    const double ratio = s->peak_value / pi_max_peak;
    double expansion = FFMIN(s->max_expansion, ratio);

    if (s->rms_value > DBL_EPSILON)
        expansion = FFMIN(expansion, s->rms_value / sqrt(pi_rms_sum / pi_size));

    if (bypass) {
        return 1.;
    } else if (type) {
        const double raise_amount = s->raise_amount * scale;

        if (ratio > 1.0 && state < 1.0 && raise_amount == 0.0)
            state = 1.0;
        return FFMIN(expansion, state + raise_amount);
    } else {
        return FFMIN(expansion, FFMAX(compression, state - s->fall_amount * scale));
    }
}

static void next_pi(AVFilterContext *ctx, ChannelContext *cc, int bypass)
{
    av_assert1(cc->pi_size >= 0);
    if (cc->pi_size == 0) {
        SpeechNormalizerContext *av_unused s = ctx->priv;
        int start = cc->pi_start;
        double scale;

        av_assert1(cc->pi[start].size > 0);
        av_assert1(cc->pi[start].type > 0 || s->eof);
        cc->pi_size = cc->pi[start].size;
        cc->pi_rms_sum = cc->pi[start].rms_sum;
        cc->pi_max_peak = cc->pi[start].max_peak;
        av_assert1(cc->pi_start != cc->pi_end || s->eof);
        cc->pi[start].size = 0;
        cc->pi[start].type = 0;
        start++;
        if (start >= MAX_ITEMS)
            start = 0;
        cc->pi_start = start;
        scale = fmin(1.0, cc->pi_size / (double)ctx->inputs[0]->sample_rate);
        cc->gain_state = next_gain(ctx, cc->pi_max_peak, bypass, cc->gain_state,
                                   cc->pi_rms_sum, cc->pi_size, scale);
    }
}

static double min_gain(AVFilterContext *ctx, ChannelContext *cc, int max_size)
{
    SpeechNormalizerContext *s = ctx->priv;
    double min_gain = s->max_expansion;
    double gain_state = cc->gain_state;
    int size = cc->pi_size;
    int idx = cc->pi_start;
    double scale;

    min_gain = FFMIN(min_gain, gain_state);
    while (size <= max_size) {
        if (idx == cc->pi_end)
            break;
        scale = fmin(1.0, cc->pi[idx].size / (double)ctx->inputs[0]->sample_rate);
        gain_state = next_gain(ctx, cc->pi[idx].max_peak, 0, gain_state,
                               cc->pi[idx].rms_sum, cc->pi[idx].size, scale);
        min_gain = FFMIN(min_gain, gain_state);
        size += cc->pi[idx].size;
        idx++;
        if (idx >= MAX_ITEMS)
            idx = 0;
    }

    return min_gain;
}

#define DIFFSIGN(x,y) (((x)>(y)) - ((x)<-(y)))

#define ANALYZE_CHANNEL(name, ptype, zero, min_peak)                            \
static void analyze_channel_## name (AVFilterContext *ctx, ChannelContext *cc,  \
                                     const uint8_t *srcp, int nb_samples)       \
{                                                                               \
    const ptype *src = (const ptype *)srcp;                                     \
    PeriodItem *pi = (PeriodItem *)&cc->pi;                                     \
    int pi_end = cc->pi_end;                                                    \
    int state = cc->state;                                                      \
    int n = 0;                                                                  \
                                                                                \
    if (state == -2)                                                            \
        state = DIFFSIGN(src[0], min_peak);                                     \
                                                                                \
    while (n < nb_samples) {                                                    \
        int new_size, split = 0;                                                \
        ptype new_max_peak;                                                     \
        ptype new_rms_sum;                                                      \
                                                                                \
        split = (!state) && pi[pi_end].size >= nb_samples;                      \
        if (state != DIFFSIGN(src[n], min_peak) || split) {                     \
            ptype max_peak = pi[pi_end].max_peak;                               \
            ptype rms_sum = pi[pi_end].rms_sum;                                 \
            int old_state = state;                                              \
                                                                                \
            state = DIFFSIGN(src[n], min_peak);                                 \
            av_assert1(pi[pi_end].size > 0);                                    \
            if (max_peak >= min_peak || split) {                                \
                pi[pi_end].type = 1;                                            \
                cc->acc += pi[pi_end].size;                                     \
                pi_end++;                                                       \
                if (pi_end >= MAX_ITEMS)                                        \
                    pi_end = 0;                                                 \
                if (state != old_state) {                                       \
                    pi[pi_end].max_peak = DBL_MIN;                              \
                    pi[pi_end].rms_sum = 0.0;                                   \
                } else {                                                        \
                    pi[pi_end].max_peak = max_peak;                             \
                    pi[pi_end].rms_sum = rms_sum;                               \
                }                                                               \
                pi[pi_end].type = 0;                                            \
                pi[pi_end].size = 0;                                            \
                av_assert1(pi_end != cc->pi_start);                             \
            }                                                                   \
        }                                                                       \
                                                                                \
        new_max_peak = pi[pi_end].max_peak;                                     \
        new_rms_sum = pi[pi_end].rms_sum;                                       \
        new_size = pi[pi_end].size;                                             \
        if (state > zero) {                                                     \
            while (src[n] > min_peak) {                                         \
                new_max_peak = FFMAX(new_max_peak,  src[n]);                    \
                new_rms_sum += src[n] * src[n];                                 \
                new_size++;                                                     \
                n++;                                                            \
                if (n >= nb_samples)                                            \
                    break;                                                      \
            }                                                                   \
        } else if (state < zero) {                                              \
            while (src[n] < min_peak) {                                         \
                new_max_peak = FFMAX(new_max_peak, -src[n]);                    \
                new_rms_sum += src[n] * src[n];                                 \
                new_size++;                                                     \
                n++;                                                            \
                if (n >= nb_samples)                                            \
                    break;                                                      \
            }                                                                   \
        } else {                                                                \
            while (src[n] >= -min_peak && src[n] <= min_peak) {                 \
                new_max_peak = min_peak;                                        \
                new_size++;                                                     \
                n++;                                                            \
                if (n >= nb_samples)                                            \
                    break;                                                      \
            }                                                                   \
        }                                                                       \
                                                                                \
        pi[pi_end].max_peak = new_max_peak;                                     \
        pi[pi_end].rms_sum = new_rms_sum;                                       \
        pi[pi_end].size = new_size;                                             \
    }                                                                           \
    cc->pi_end = pi_end;                                                        \
    cc->state = state;                                                          \
}

ANALYZE_CHANNEL(dbl, double, 0.0, MIN_PEAK)
ANALYZE_CHANNEL(flt, float,  0.f, (float)MIN_PEAK)

#define FILTER_CHANNELS(name, ptype)                                            \
static void filter_channels_## name (AVFilterContext *ctx,                      \
                                     AVFrame *in, AVFrame *out, int nb_samples) \
{                                                                               \
    SpeechNormalizerContext *s = ctx->priv;                                     \
    AVFilterLink *inlink = ctx->inputs[0];                                      \
                                                                                \
    for (int ch = 0; ch < inlink->ch_layout.nb_channels; ch++) {                \
        ChannelContext *cc = &s->cc[ch];                                        \
        const ptype *src = (const ptype *)in->extended_data[ch];                \
        ptype *dst = (ptype *)out->extended_data[ch];                           \
        enum AVChannel channel = av_channel_layout_channel_from_index(&inlink->ch_layout, ch); \
        const int bypass = av_channel_layout_index_from_channel(&s->ch_layout, channel) < 0; \
        int n = 0;                                                              \
                                                                                \
        while (n < nb_samples) {                                                \
            ptype gain;                                                         \
            int size;                                                           \
                                                                                \
            next_pi(ctx, cc, bypass);                                           \
            size = FFMIN(nb_samples - n, cc->pi_size);                          \
            av_assert1(size > 0);                                               \
            gain = cc->gain_state;                                              \
            consume_pi(cc, size);                                               \
            if (ctx->is_disabled) {                                             \
                memcpy(dst + n, src + n, size * sizeof(*dst));                  \
            } else {                                                            \
                for (int i = n; i < n + size; i++)                              \
                    dst[i] = src[i] * gain;                                     \
            }                                                                   \
            n += size;                                                          \
        }                                                                       \
    }                                                                           \
}

FILTER_CHANNELS(dbl, double)
FILTER_CHANNELS(flt, float)

static double dlerp(double min, double max, double mix)
{
    return min + (max - min) * mix;
}

static float flerp(float min, float max, float mix)
{
    return min + (max - min) * mix;
}

#define FILTER_LINK_CHANNELS(name, ptype, tlerp)                                \
static void filter_link_channels_## name (AVFilterContext *ctx,                 \
                                          AVFrame *in, AVFrame *out,            \
                                          int nb_samples)                       \
{                                                                               \
    SpeechNormalizerContext *s = ctx->priv;                                     \
    AVFilterLink *inlink = ctx->inputs[0];                                      \
    int n = 0;                                                                  \
                                                                                \
    while (n < nb_samples) {                                                    \
        int min_size = nb_samples - n;                                          \
        ptype gain = s->max_expansion;                                          \
                                                                                \
        for (int ch = 0; ch < inlink->ch_layout.nb_channels; ch++) {            \
            ChannelContext *cc = &s->cc[ch];                                    \
                                                                                \
            enum AVChannel channel = av_channel_layout_channel_from_index(&inlink->ch_layout, ch); \
            cc->bypass = av_channel_layout_index_from_channel(&s->ch_layout, channel) < 0; \
                                                                                \
            next_pi(ctx, cc, cc->bypass);                                       \
            min_size = FFMIN(min_size, cc->pi_size);                            \
        }                                                                       \
                                                                                \
        av_assert1(min_size > 0);                                               \
        for (int ch = 0; ch < inlink->ch_layout.nb_channels; ch++) {            \
            ChannelContext *cc = &s->cc[ch];                                    \
                                                                                \
            if (cc->bypass)                                                     \
                continue;                                                       \
            gain = FFMIN(gain, min_gain(ctx, cc, min_size));                    \
        }                                                                       \
                                                                                \
        for (int ch = 0; ch < inlink->ch_layout.nb_channels; ch++) {            \
            ChannelContext *cc = &s->cc[ch];                                    \
            const ptype *src = (const ptype *)in->extended_data[ch];            \
            ptype *dst = (ptype *)out->extended_data[ch];                       \
                                                                                \
            consume_pi(cc, min_size);                                           \
            if (cc->bypass || ctx->is_disabled) {                               \
                memcpy(dst + n, src + n, min_size * sizeof(*dst));              \
            } else {                                                            \
                for (int i = n; i < n + min_size; i++) {                        \
                    ptype g = tlerp(s->prev_gain, gain, (i-n)/(ptype)min_size); \
                    dst[i] = src[i] * g;                                        \
                }                                                               \
            }                                                                   \
        }                                                                       \
                                                                                \
        s->prev_gain = gain;                                                    \
        n += min_size;                                                          \
    }                                                                           \
}

FILTER_LINK_CHANNELS(dbl, double, dlerp)
FILTER_LINK_CHANNELS(flt, float, flerp)

static int filter_frame(AVFilterContext *ctx)
{
    SpeechNormalizerContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFilterLink *inlink = ctx->inputs[0];
    int ret;

    while (av_fifo_can_read(s->fifo) > 0) {
        int min_pi_nb_samples;
        AVFrame *in = NULL, *out;

        av_fifo_peek(s->fifo, &in, 1, 0);
        if (!in)
            break;

        min_pi_nb_samples = available_samples(ctx);
        if (min_pi_nb_samples < in->nb_samples && !s->eof)
            break;

        av_fifo_read(s->fifo, &in, 1);
        if (!in)
            break;

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

        s->filter_channels[s->link](ctx, in, out, in->nb_samples);

        s->pts = in->pts + av_rescale_q(in->nb_samples, av_make_q(1, outlink->sample_rate),
                                        outlink->time_base);

        if (out != in)
            av_frame_free(&in);
        return ff_filter_frame(outlink, out);
    }

    for (int f = 0; f < ff_inlink_queued_frames(inlink); f++) {
        AVFrame *in;

        ret = ff_inlink_consume_frame(inlink, &in);
        if (ret < 0)
            return ret;
        if (ret == 0)
            break;

        av_fifo_write(s->fifo, &in, 1);

        for (int ch = 0; ch < inlink->ch_layout.nb_channels; ch++) {
            ChannelContext *cc = &s->cc[ch];

            s->analyze_channel(ctx, cc, in->extended_data[ch], in->nb_samples);
        }
    }

    return 1;
}

static int activate(AVFilterContext *ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    SpeechNormalizerContext *s = ctx->priv;
    int ret, status;
    int64_t pts;

    ret = av_channel_layout_copy(&s->ch_layout, &inlink->ch_layout);
    if (ret < 0)
        return ret;
    if (strcmp(s->ch_layout_str, "all"))
        av_channel_layout_from_string(&s->ch_layout,
                                      s->ch_layout_str);

    FF_FILTER_FORWARD_STATUS_BACK(outlink, inlink);

    ret = filter_frame(ctx);
    if (ret <= 0)
        return ret;

    if (!s->eof && ff_inlink_acknowledge_status(inlink, &status, &pts)) {
        if (status == AVERROR_EOF)
            s->eof = 1;
    }

    if (s->eof && ff_inlink_queued_samples(inlink) == 0 &&
        av_fifo_can_read(s->fifo) == 0) {
        ff_outlink_set_status(outlink, AVERROR_EOF, s->pts);
        return 0;
    }

    if (av_fifo_can_read(s->fifo) > 0) {
        const int nb_samples = available_samples(ctx);
        AVFrame *in;

        av_fifo_peek(s->fifo, &in, 1, 0);
        if (nb_samples >= in->nb_samples || s->eof) {
            ff_filter_set_ready(ctx, 10);
            return 0;
        }
    }

    FF_FILTER_FORWARD_WANTED(outlink, inlink);

    return FFERROR_NOT_READY;
}

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    SpeechNormalizerContext *s = ctx->priv;

    s->prev_gain = 1.;
    s->cc = av_calloc(inlink->ch_layout.nb_channels, sizeof(*s->cc));
    if (!s->cc)
        return AVERROR(ENOMEM);

    for (int ch = 0; ch < inlink->ch_layout.nb_channels; ch++) {
        ChannelContext *cc = &s->cc[ch];

        cc->state = -2;
        cc->gain_state = s->max_expansion;
    }

    switch (inlink->format) {
    case AV_SAMPLE_FMT_FLTP:
        s->analyze_channel = analyze_channel_flt;
        s->filter_channels[0] = filter_channels_flt;
        s->filter_channels[1] = filter_link_channels_flt;
        break;
    case AV_SAMPLE_FMT_DBLP:
        s->analyze_channel = analyze_channel_dbl;
        s->filter_channels[0] = filter_channels_dbl;
        s->filter_channels[1] = filter_link_channels_dbl;
        break;
    default:
        av_assert1(0);
    }

    s->fifo = av_fifo_alloc2(1024, sizeof(AVFrame *), AV_FIFO_FLAG_AUTO_GROW);
    if (!s->fifo)
        return AVERROR(ENOMEM);

    return 0;
}

static int process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                           char *res, int res_len, int flags)
{
    SpeechNormalizerContext *s = ctx->priv;
    int link = s->link;
    int ret;

    ret = ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    if (ret < 0)
        return ret;
    if (link != s->link)
        s->prev_gain = 1.;

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    SpeechNormalizerContext *s = ctx->priv;

    av_fifo_freep2(&s->fifo);
    av_channel_layout_uninit(&s->ch_layout);
    av_freep(&s->cc);
}

static const AVFilterPad inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .config_props = config_input,
    },
};

const AVFilter ff_af_speechnorm = {
    .name            = "speechnorm",
    .description     = NULL_IF_CONFIG_SMALL("Speech Normalizer."),
    .priv_size       = sizeof(SpeechNormalizerContext),
    .priv_class      = &speechnorm_class,
    .activate        = activate,
    .uninit          = uninit,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(ff_audio_default_filterpad),
    FILTER_SAMPLEFMTS(AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_DBLP),
    .flags           = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL,
    .process_command = process_command,
};
