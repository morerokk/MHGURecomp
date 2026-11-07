// The HDR analysis ([HDR] Analysis): what the window shows, measured on the GPU every frame (nvn/vulkan/hdr.c) and
// drawn over the game by the overlay (overlay.cpp draw_hdr_analysis): its brightness in nits, the colour gamut it uses,
// and a waveform (brightness by screen column), as Lilium's HDR analysis does with ReShade.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define HDR_WF_W 160  // the waveform: columns across the picture
#define HDR_WF_H 96   // rows: log10 nits from HDR_WF_LO to HDR_WF_HI
#define HDR_WF_LO -2.0f
#define HDR_WF_HI 4.0f

typedef struct {
    float max_nits, avg_nits, min_nits;  // luminance (BT.2020 Y) of the picture as the display gets it
    float max_cll, avg_cll;              // the brightest channel (MaxCLL, as in the HDR metadata)
    float center_nits, center_cll;       // the pixel in the middle of the picture
    float bt709, p3, bt2020, outside;    // % of pixels within each gamut (the smallest that holds them)
    float peak, game, ui;                // the settings: PeakBrightness, GameBrightness, UIBrightness
    int pq;                              // 1 HDR10 (PQ), 0 scRGB
    uint32_t wf_max;                     // the waveform's fullest cell
    uint32_t wf[HDR_WF_H][HDR_WF_W];     // [row 0 = HDR_WF_LO] [column]: pixels
} HdrStats;

bool vk_hdr_stats(HdrStats *out);  // the latest frame's (false: HDR or the analysis is off, or none yet)

#ifdef __cplusplus
}
#endif
