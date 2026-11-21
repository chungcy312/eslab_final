#include "tuner.h"
#include "tuner_pitch.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

enum {
    DMA_HALF_SAMPLES = 256,
    CAPTURE_SAMPLES = 4096,
    UPDATE_SAMPLES = 6 * DMA_HALF_SAMPLES,
    DMA_TIMEOUT_MS = 250
};

_Static_assert((CAPTURE_SAMPLES & (CAPTURE_SAMPLES - 1)) == 0,
               "The capture ring must contain a power-of-two number of samples");
_Static_assert(CAPTURE_SAMPLES > TUNER_FRAME_SAMPLES + DMA_HALF_SAMPLES,
               "The capture ring must leave room for ongoing DMA callbacks");

static int32_t dma_samples[DMA_HALF_SAMPLES * 2];
static volatile int32_t capture[CAPTURE_SAMPLES];
static float frame[TUNER_FRAME_SAMPLES];
static volatile uint32_t captured;
static volatile uint32_t last_dma_ms;
static volatile bool dma_error;

static UART_HandleTypeDef *output_uart;
static DFSDM_Filter_HandleTypeDef *audio_filter;
static uint32_t last_frame_end;
static uint32_t last_error_ms;
static bool frame_reported;
static bool error_reported;

static void send_empty_reading(const char *status)
{
    char line[168];
    const int length = snprintf(line, sizeof(line),
        "{\"a4_hz\":%d,\"note\":null,\"frequency_hz\":null,\"target_hz\":null,"
        "\"cents\":null,\"in_tune\":false,\"status\":\"%s\"}\r\n", TUNER_A4_HZ, status);
    if (length > 0 && length < (int)sizeof(line)) {
        HAL_UART_Transmit(output_uart, (uint8_t *)line, (uint16_t)length, 25);
    }
}

static void send_reading(float frequency_hz, const TunerNote *note)
{
    const long measured = lroundf(frequency_hz * 100.0f);
    const long target = lroundf(note->target_hz * 100.0f);
    const long cents = lroundf(note->cents * 100.0f);
    const bool in_tune = labs(cents) <= 100L * TUNER_IN_TUNE_CENTS;
    const char *status = in_tune ? "IN_TUNE" : (cents < 0 ? "FLAT" : "SHARP");

    /* Integer formatting keeps UART output working with newlib-nano defaults. */
    char line[192];
    const int length = snprintf(line, sizeof(line),
        "{\"a4_hz\":%d,\"note\":\"%s%d\",\"frequency_hz\":%ld.%02ld,"
        "\"target_hz\":%ld.%02ld,\"cents\":%s%ld.%02ld,\"in_tune\":%s,\"status\":\"%s\"}\r\n",
        TUNER_A4_HZ, note->name, note->octave, measured / 100, measured % 100,
        target / 100, target % 100, cents < 0 ? "-" : "", labs(cents) / 100,
        labs(cents) % 100, in_tune ? "true" : "false", status);
    if (length > 0 && length < (int)sizeof(line)) {
        HAL_UART_Transmit(output_uart, (uint8_t *)line, (uint16_t)length, 25);
    }
}

void Tuner_Start(DFSDM_Filter_HandleTypeDef *filter, UART_HandleTypeDef *uart)
{
    audio_filter = filter;
    output_uart = uart;
    last_dma_ms = HAL_GetTick();
    if (HAL_DFSDM_FilterRegularStart_DMA(filter, dma_samples, DMA_HALF_SAMPLES * 2) != HAL_OK) {
        dma_error = true;
    }
}

void Tuner_Poll(void)
{
    const uint32_t last_audio = last_dma_ms;
    const uint32_t now = HAL_GetTick();
    if (dma_error || (uint32_t)(now - last_audio) >= DMA_TIMEOUT_MS) {
        if (!error_reported || (uint32_t)(now - last_error_ms) >= 500U) {
            send_empty_reading("AUDIO_ERROR");
            last_error_ms = now;
            error_reported = true;
        }
        return;
    }
    error_reported = false;

    const uint32_t frame_end = captured;
    if ((!frame_reported && frame_end < TUNER_FRAME_SAMPLES) ||
        (frame_reported && (uint32_t)(frame_end - last_frame_end) < UPDATE_SAMPLES)) {
        return;
    }

    const uint32_t frame_start = frame_end - TUNER_FRAME_SAMPLES;
    for (unsigned i = 0; i < TUNER_FRAME_SAMPLES; ++i) {
        frame[i] = (float)capture[(frame_start + i) & (CAPTURE_SAMPLES - 1)];
    }
    /* The extra DMA block accounts for a callback that has started to write
       new samples but has not yet advanced the published capture position. */
    if ((uint32_t)(captured - frame_start) > CAPTURE_SAMPLES - DMA_HALF_SAMPLES) {
        return;
    }

    last_frame_end = frame_end;
    frame_reported = true;
    const float frequency_hz = TunerPitch_Estimate(frame);
    TunerNote note;
    if (TunerPitch_NearestNote(frequency_hz, &note)) {
        send_reading(frequency_hz, &note);
    } else {
        send_empty_reading("NO_SIGNAL");
    }
}

static void capture_dma_block(unsigned offset)
{
    const uint32_t start = captured;
    const volatile int32_t *source = dma_samples + offset;
    for (unsigned i = 0; i < DMA_HALF_SAMPLES; ++i) {
        /* RDATAR stores 24-bit signed PCM in the upper bits. */
        capture[(start + i) & (CAPTURE_SAMPLES - 1)] = source[i] >> 8;
    }
    captured = start + DMA_HALF_SAMPLES;
    last_dma_ms = HAL_GetTick();
}

void HAL_DFSDM_FilterRegConvHalfCpltCallback(DFSDM_Filter_HandleTypeDef *filter)
{
    if (filter == audio_filter) {
        capture_dma_block(0);
    }
}

void HAL_DFSDM_FilterRegConvCpltCallback(DFSDM_Filter_HandleTypeDef *filter)
{
    if (filter == audio_filter) {
        capture_dma_block(DMA_HALF_SAMPLES);
    }
}

void HAL_DFSDM_FilterErrorCallback(DFSDM_Filter_HandleTypeDef *filter)
{
    if (filter == audio_filter) {
        dma_error = true;
    }
}
