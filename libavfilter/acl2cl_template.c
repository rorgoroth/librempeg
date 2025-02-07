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

#undef ftype
#undef MIX2
#undef SAMPLE_FORMAT
#if DEPTH == 8
#define ftype uint8_t
#define SAMPLE_FORMAT u8p
#elif DEPTH == 16
#define ftype int16_t
#define SAMPLE_FORMAT s16p
#elif DEPTH == 31
#define ftype int32_t
#define SAMPLE_FORMAT s32p
#elif DEPTH == 63
#define ftype int64_t
#define SAMPLE_FORMAT s64p
#elif DEPTH == 32
#define ftype float
#define SAMPLE_FORMAT fltp
#else
#define ftype double
#define SAMPLE_FORMAT dblp
#endif

#define F(x) ((ftype)(x))

#if DEPTH == 32 || DEPTH == 64
#define MIX2(a, b) (((a)+(b))*F(0.5))
#else
#define MIX2(a, b) (((a)+(b))/2)
#endif

#define fn3(a,b)   a##_##b
#define fn2(a,b)   fn3(a,b)
#define fn(a)      fn2(a, SAMPLE_FORMAT)

static int fn(do_cl2cl)(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    ThreadData *td = arg;
    AVFrame *out = td->out;
    AVFrame *in = td->in;
    const int nb_channels = out->ch_layout.nb_channels;
    const int nb_samples = in->nb_samples;
    const int start = (nb_channels * jobnr) / nb_jobs;
    const int end = (nb_channels * (jobnr+1)) / nb_jobs;
    AVChannelLayout *out_ch_layout = &out->ch_layout;
    AVChannelLayout *in_ch_layout = &in->ch_layout;
    const size_t bytes_per_sample = av_get_bytes_per_sample(in->format);

    for (int ch = start; ch < end; ch++) {
        const int chan = av_channel_layout_channel_from_index(out_ch_layout, ch);
        const int idx = av_channel_layout_index_from_channel(in_ch_layout, chan);

        if (idx < 0) {
            switch (chan) {
            case AV_CHAN_FRONT_CENTER:
                {
                    const int fl_idx = av_channel_layout_index_from_channel(in_ch_layout, AV_CHAN_FRONT_LEFT);
                    const int fr_idx = av_channel_layout_index_from_channel(in_ch_layout, AV_CHAN_FRONT_RIGHT);

                    if (fl_idx >= 0 && fr_idx >= 0) {
                        const ftype *fl_src = (const ftype *)in->extended_data[fl_idx];
                        const ftype *fr_src = (const ftype *)in->extended_data[fr_idx];
                        ftype *dst = (ftype *)out->extended_data[ch];

                        for (int n = 0; n < nb_samples; n++)
                            dst[n] = MIX2(fl_src[n], fr_src[n]);
                    }
                }
                break;
            case AV_CHAN_FRONT_LEFT:
            case AV_CHAN_FRONT_RIGHT:
                {
                    const int fc_idx = av_channel_layout_index_from_channel(in_ch_layout, AV_CHAN_FRONT_CENTER);

                    if (fc_idx >= 0) {
                        const ftype *fc_src = (const ftype *)in->extended_data[fc_idx];
                        ftype *dst = (ftype *)out->extended_data[ch];

                        for (int n = 0; n < nb_samples; n++)
                            dst[n] = MIX2(fc_src[n], F(0.0));
                    }
                }
                break;
            default:
                break;
            }

            continue;
        }

        memcpy(out->extended_data[ch], in->extended_data[idx], nb_samples * bytes_per_sample);
    }

    return 0;
}
