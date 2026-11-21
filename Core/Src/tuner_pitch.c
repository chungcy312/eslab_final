#include "tuner_pitch.h"

#include <math.h>
#include <stdint.h>

enum {
    MIN_FREQUENCY_HZ = 80,
    MAX_FREQUENCY_HZ = 1200,
    MAX_LAG = TUNER_SAMPLE_RATE / MIN_FREQUENCY_HZ,
    COMPARISON_SAMPLES = TUNER_FRAME_SAMPLES - MAX_LAG - 1
};

static const float min_rms_pcm = 120.0f;
static const float yin_threshold = 0.12f;
static const char *const note_names[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

/* Keep the time-domain inner loop fast in CubeIDE Debug builds as well. */
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("O2")))
#endif
float TunerPitch_Estimate(const float samples[TUNER_FRAME_SAMPLES])
{
    float sum = 0.0f;
    float sum_squares = 0.0f;
    for (unsigned i = 0; i < TUNER_FRAME_SAMPLES; ++i) {
        sum += samples[i];
        sum_squares += samples[i] * samples[i];
    }

    const float mean = sum / TUNER_FRAME_SAMPLES;
    const float variance = sum_squares / TUNER_FRAME_SAMPLES - mean * mean;
    if (variance < min_rms_pcm * min_rms_pcm) {
        return 0.0f;
    }

    float running_sum = 0.0f;
    float earlier_difference = 0.0f;
    float previous_difference = 0.0f;
    float previous_normalized = 1.0f;
    bool below_threshold = false;
    for (unsigned lag = 1; lag <= MAX_LAG + 1; ++lag) {
        float difference = 0.0f;
        for (unsigned i = 0; i < COMPARISON_SAMPLES; ++i) {
            const float delta = samples[i] - samples[i + lag];
            difference += delta * delta;
        }
        running_sum += difference;
        const float normalized = running_sum > 0.0f
            ? difference * lag / running_sum : 1.0f;

        /* Use the first trough under the YIN threshold, then refine its lag. */
        if (below_threshold && normalized >= previous_normalized) {
            const float curvature = earlier_difference - 2.0f * previous_difference + difference;
            float offset = 0.0f;
            if (curvature > 0.0f) {
                offset = 0.5f * (earlier_difference - difference) / curvature;
                offset = fminf(0.5f, fmaxf(-0.5f, offset));
            }
            const float frequency = TUNER_SAMPLE_RATE / ((float)(lag - 1) + offset);
            /* Allow for interpolation error near the nominal range limits. */
            const float edge_tolerance = 0.0012f;
            return frequency >= MIN_FREQUENCY_HZ * (1.0f - edge_tolerance) &&
                   frequency <= MAX_FREQUENCY_HZ * (1.0f + edge_tolerance) ? frequency : 0.0f;
        }

        below_threshold = normalized < yin_threshold;
        earlier_difference = previous_difference;
        previous_difference = difference;
        previous_normalized = normalized;
    }
    return 0.0f;
}

bool TunerPitch_NearestNote(float frequency_hz, TunerNote *note)
{
    if (!isfinite(frequency_hz) || frequency_hz <= 0.0f) {
        return false;
    }

    const float midi_position = 69.0f + 12.0f * log2f(frequency_hz / TUNER_A4_HZ);
    if (midi_position < -0.49f || midi_position > 127.49f) {
        return false;
    }

    const int midi_note = (int)lroundf(midi_position);
    note->name = note_names[midi_note % 12];
    note->octave = midi_note / 12 - 1;
    note->target_hz = TUNER_A4_HZ * exp2f((midi_note - 69) / 12.0f);
    note->cents = 100.0f * (midi_position - midi_note);
    return true;
}
