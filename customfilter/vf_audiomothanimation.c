/*
 * Copyright (c) 2008 Affine Systems, Inc (Michael Sullivan, Bobby Impollonia)
 * Copyright (c) 2013 Andrey Utkin <andrey.krieger.utkin gmail com>
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
 * Box and grid drawing filters. Also a nice template for a filter
 * that needs to write in the input frame.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavutil/colorspace.h"
#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "libavutil/eval.h"
#include "libavutil/pixdesc.h"
#include "libavutil/parseutils.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

#define MIN(a, b)               ((a) < (b) ? (a) : (b))

#define MAX(a, b)               ((a) > (b) ? (a) : (b))

#define ROUNDED_DIV(a, b)       (((a) + ((b)/2)) / (b))

static const char *const var_names[] = {
    "dar",
    "hsub", "vsub",
    "in_h", "ih",      ///< height of the input video
    "in_w", "iw",      ///< width  of the input video
    "sar",
    "x",
    "y",
    "h",              ///< height of the rendered box
    "w",              ///< width  of the rendered box
    "t",
    "fill",
    NULL
};

enum { Y, U, V, A };

enum var_name {
    VAR_DAR,
    VAR_HSUB, VAR_VSUB,
    VAR_IN_H, VAR_IH,
    VAR_IN_W, VAR_IW,
    VAR_SAR,
    VAR_X,
    VAR_Y,
    VAR_H,
    VAR_W,
    VAR_T,
    VAR_MAX,
    VARS_NB
};

typedef struct AudioMothAnimationContext {
    const AVClass *class;
    int x, y, w, h;
    int framecount;
    char *skip_str;
    char *color_str;
    unsigned char yuv_color[4];
    int invert_color; ///< invert luma color
    int vsub, hsub;   ///< chroma subsampling
    char *x_expr, *y_expr; ///< expression for x and y
    char *w_expr, *h_expr; ///< expression for width and height
    char *t_expr;          ///< expression for framecount
    int have_alpha;
    int replace;
} AudioMothAnimationContext;

static const int NUM_EXPR_EVALS = 5;

static int numberOfBoxes = 0;

static int frameCounter = 0;

static float *skippingPercentages;
static int skipCount;

static float* splitAndConvert(const char *inputString, int *skipCount) {
    // Find the number of elements by counting the lines
    *skipCount = 1; // Initialize to 1 to account for the first element
    for (int i = 0; inputString[i]; i++) {
        if (inputString[i] == '|') {
            (*skipCount)++;
        }
    }

    // Allocate memory for the float array
    float *floatArray = (float *)malloc(*skipCount * sizeof(float));
    if (floatArray == NULL) {
        printf("Memory allocation failed.\n");
        return NULL;
    }

    // Split the input string into substrings and convert to floats
    char *token = strtok((char *)inputString, "|");
    int index = 0;
    while (token != NULL) {
        float floatValue = atof(token); // Convert the substring to a float
        floatArray[index] = floatValue;
        index++;
        token = strtok(NULL, "|");
    }

    return floatArray;
}

static av_cold int init(AVFilterContext *ctx)
{
    AudioMothAnimationContext *s = ctx->priv;
    uint8_t rgba_color[4];

    if (!strcmp(s->color_str, "invert"))
        s->invert_color = 1;
    else if (av_parse_color(rgba_color, s->color_str, -1, ctx) < 0)
        return AVERROR(EINVAL);

    if (!s->invert_color) {
        s->yuv_color[Y] = RGB_TO_Y_CCIR(rgba_color[0], rgba_color[1], rgba_color[2]);
        s->yuv_color[U] = RGB_TO_U_CCIR(rgba_color[0], rgba_color[1], rgba_color[2], 0);
        s->yuv_color[V] = RGB_TO_V_CCIR(rgba_color[0], rgba_color[1], rgba_color[2], 0);
        s->yuv_color[A] = rgba_color[3];
    }

    numberOfBoxes += 1;

    // If not in skip mode, the skip string will just be '-'

    int compare = strcmp(s->skip_str, "-");

    if (skipCount == 0 && compare != 0) {

        skippingPercentages = splitAndConvert(s->skip_str, &skipCount);

    }

    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_YUV444P,  AV_PIX_FMT_YUV422P,  AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_YUV411P,  AV_PIX_FMT_YUV410P,
        AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUVJ420P,
        AV_PIX_FMT_YUV440P,  AV_PIX_FMT_YUVJ440P,
        AV_PIX_FMT_YUVA420P, AV_PIX_FMT_YUVA422P, AV_PIX_FMT_YUVA444P,
        AV_PIX_FMT_NONE
    };
    AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);
    if (!fmts_list)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);
}

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    AudioMothAnimationContext *s = ctx->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);
    double var_values[VARS_NB], res;
    char *expr;
    int ret;
    int i;

    s->hsub = desc->log2_chroma_w;
    s->vsub = desc->log2_chroma_h;
    s->have_alpha = desc->flags & AV_PIX_FMT_FLAG_ALPHA;

    var_values[VAR_IN_H] = var_values[VAR_IH] = inlink->h;
    var_values[VAR_IN_W] = var_values[VAR_IW] = inlink->w;
    var_values[VAR_SAR]  = inlink->sample_aspect_ratio.num ? av_q2d(inlink->sample_aspect_ratio) : 1;
    var_values[VAR_DAR]  = (double)inlink->w / inlink->h * var_values[VAR_SAR];
    var_values[VAR_HSUB] = s->hsub;
    var_values[VAR_VSUB] = s->vsub;
    var_values[VAR_X] = NAN;
    var_values[VAR_Y] = NAN;
    var_values[VAR_H] = NAN;
    var_values[VAR_W] = NAN;
    var_values[VAR_T] = NAN;

    for (i = 0; i <= NUM_EXPR_EVALS; i++) {
        /* evaluate expressions, fail on last iteration */
        var_values[VAR_MAX] = inlink->w;
        if ((ret = av_expr_parse_and_eval(&res, (expr = s->x_expr),
                                          var_names, var_values,
                                          NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0 && i == NUM_EXPR_EVALS)
            goto fail;
        s->x = var_values[VAR_X] = res;

        var_values[VAR_MAX] = inlink->h;
        if ((ret = av_expr_parse_and_eval(&res, (expr = s->y_expr),
                                          var_names, var_values,
                                          NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0 && i == NUM_EXPR_EVALS)
            goto fail;
        s->y = var_values[VAR_Y] = res;

        var_values[VAR_MAX] = inlink->w - s->x;
        if ((ret = av_expr_parse_and_eval(&res, (expr = s->w_expr),
                                          var_names, var_values,
                                          NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0 && i == NUM_EXPR_EVALS)
            goto fail;
        s->w = var_values[VAR_W] = res;

        var_values[VAR_MAX] = inlink->h - s->y;
        if ((ret = av_expr_parse_and_eval(&res, (expr = s->h_expr),
                                          var_names, var_values,
                                          NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0 && i == NUM_EXPR_EVALS)
            goto fail;
        s->h = var_values[VAR_H] = res;

        var_values[VAR_MAX] = INT_MAX;
        if ((ret = av_expr_parse_and_eval(&res, (expr = s->t_expr),
                                          var_names, var_values,
                                          NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0 && i == NUM_EXPR_EVALS)
            goto fail;
        s->framecount = var_values[VAR_T] = res;
    }

    /* if w or h are zero, use the input w/h */
    s->w = (s->w > 0) ? s->w : inlink->w;
    s->h = (s->h > 0) ? s->h : inlink->h;

    /* sanity check width and height */
    if (s->w <  0 || s->h <  0) {
        av_log(ctx, AV_LOG_ERROR, "Size values less than 0 are not acceptable.\n");
        return AVERROR(EINVAL);
    }

    av_log(ctx, AV_LOG_VERBOSE, "x:%d y:%d w:%d h:%d color:0x%02X%02X%02X%02X\n",
           s->x, s->y, s->w, s->h,
           s->yuv_color[Y], s->yuv_color[U], s->yuv_color[V], s->yuv_color[A]);

    return 0;

fail:
    av_log(ctx, AV_LOG_ERROR,
           "Error when evaluating the expression '%s'.\n",
           expr);
    return ret;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AudioMothAnimationContext *s = inlink->dst->priv;
    int plane, x, y, xb = s->x, yb = s->y;
    unsigned char *row[4];

    int counter = frameCounter / numberOfBoxes;

    int xPosition = 0;

    if (skipCount == 0) {

        xPosition = xb + ROUNDED_DIV(counter * (s->w - 1), s->framecount - 1);

    } else {

        // Calculate how far through the file the current frame is as a percentage

        int currentPercentage = ROUNDED_DIV((skipCount - 1) * counter, s->framecount - 1);

        // Use that percentage to work out what percentage through the unskipped audio the frame is

        // xPosition = xb + ROUNDED_DIV((s->w - 1) * skippingPercentages[currentPercentage], 100);
        xPosition = xb + skippingPercentages[currentPercentage];

    }

    xPosition = MAX(xPosition, xb);

    xPosition = MIN(xPosition, xb + s->w - 1);

    if (s->have_alpha && s->replace) {
        for (y = FFMAX(yb, 0); y < frame->height && y < (yb + s->h); y++) {
            row[0] = frame->data[0] + y * frame->linesize[0];
            row[3] = frame->data[3] + y * frame->linesize[3];

            for (plane = 1; plane < 3; plane++)
                row[plane] = frame->data[plane] +
                     frame->linesize[plane] * (y >> s->vsub);

            if (s->invert_color) {
                for (x = FFMAX(xb, 0); x < xb + s->w && x < frame->width; x++)
                    if (x == xPosition) {
                        row[0][x] = 0xff - row[0][x];
                    }
            } else {
                for (x = FFMAX(xb, 0); x < xb + s->w && x < frame->width; x++) {
                    if (x == xPosition) {
                        row[0][x] = s->yuv_color[Y];
                        row[1][x >> s->hsub] = s->yuv_color[U];
                        row[2][x >> s->hsub] = s->yuv_color[V];
                        row[3][x] = s->yuv_color[A];
                    }
                }
            }
        }
    } else {
        for (y = FFMAX(yb, 0); y < frame->height && y < (yb + s->h); y++) {
            row[0] = frame->data[0] + y * frame->linesize[0];

            for (plane = 1; plane < 3; plane++)
                row[plane] = frame->data[plane] +
                     frame->linesize[plane] * (y >> s->vsub);

            if (s->invert_color) {
                for (x = FFMAX(xb, 0); x < xb + s->w && x < frame->width; x++)
                    if (x == xPosition) {
                        row[0][x] = 0xff - row[0][x];
                    }
            } else {
                for (x = FFMAX(xb, 0); x < xb + s->w && x < frame->width; x++) {
                    double alpha = (double)s->yuv_color[A] / 255;
                    if (x == xPosition) {
                        row[0][x] = (1 - alpha) * row[0][x] + alpha * s->yuv_color[Y];
                        row[1][x >> s->hsub] = (1 - alpha) * row[1][x >> s->hsub] + alpha * s->yuv_color[U];
                        row[2][x >> s->hsub] = (1 - alpha) * row[2][x >> s->hsub] + alpha * s->yuv_color[V];
                    }
                }
            }
        }
    }

    frameCounter += 1;

    return ff_filter_frame(inlink->dst->outputs[0], frame);
}

#define OFFSET(x) offsetof(AudioMothAnimationContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

#if CONFIG_AUDIOMOTHANIMATION_FILTER

static const AVOption audiomothanimation_options[] = {
    { "x",          "set horizontal position of the left box edge", OFFSET(x_expr),    AV_OPT_TYPE_STRING, { .str="0" },       CHAR_MIN, CHAR_MAX, FLAGS },
    { "y",          "set vertical position of the top box edge",    OFFSET(y_expr),    AV_OPT_TYPE_STRING, { .str="0" },       CHAR_MIN, CHAR_MAX, FLAGS },
    { "width",      "set width of the box",                         OFFSET(w_expr),    AV_OPT_TYPE_STRING, { .str="0" },       CHAR_MIN, CHAR_MAX, FLAGS },
    { "w",          "set width of the box",                         OFFSET(w_expr),    AV_OPT_TYPE_STRING, { .str="0" },       CHAR_MIN, CHAR_MAX, FLAGS },
    { "height",     "set height of the box",                        OFFSET(h_expr),    AV_OPT_TYPE_STRING, { .str="0" },       CHAR_MIN, CHAR_MAX, FLAGS },
    { "h",          "set height of the box",                        OFFSET(h_expr),    AV_OPT_TYPE_STRING, { .str="0" },       CHAR_MIN, CHAR_MAX, FLAGS },
    { "color",      "set color of the box",                         OFFSET(color_str), AV_OPT_TYPE_STRING, { .str = "black" }, CHAR_MIN, CHAR_MAX, FLAGS },
    { "c",          "set color of the box",                         OFFSET(color_str), AV_OPT_TYPE_STRING, { .str = "black" }, CHAR_MIN, CHAR_MAX, FLAGS },
    { "skip",       "set percentage skip coordinates",              OFFSET(skip_str), AV_OPT_TYPE_STRING,  { .str = "-" },     CHAR_MIN, CHAR_MAX, FLAGS },
    { "framecount", "set the framecount",                           OFFSET(t_expr),    AV_OPT_TYPE_STRING, { .str="3" },       CHAR_MIN, CHAR_MAX, FLAGS },
    { "f",          "set the framecount",                           OFFSET(t_expr),    AV_OPT_TYPE_STRING, { .str="3" },       CHAR_MIN, CHAR_MAX, FLAGS },
    { "replace",    "replace color & alpha",                        OFFSET(replace),   AV_OPT_TYPE_BOOL,   { .i64=0 },         0,        1,        FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(audiomothanimation);

static const AVFilterPad audiomothanimation_inputs[] = {
    {
        .name           = "default",
        .type           = AVMEDIA_TYPE_VIDEO,
        .config_props   = config_input,
        .filter_frame   = filter_frame,
        .needs_writable = 1,
    },
    { NULL }
};

static const AVFilterPad audiomothanimation_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_audiomothanimation = {
    .name          = "audiomothanimation",
    .description   = NULL_IF_CONFIG_SMALL("Draw a colored box on the input video."),
    .priv_size     = sizeof(AudioMothAnimationContext),
    .priv_class    = &audiomothanimation_class,
    .init          = init,
    .query_formats = query_formats,
    .inputs        = audiomothanimation_inputs,
    .outputs       = audiomothanimation_outputs,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
};
#endif /* CONFIG_AUDIOMOTHANIMATION_FILTER */
