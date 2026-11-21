#ifndef TUNER_PITCH_H
#define TUNER_PITCH_H

#include <stdbool.h>

enum { TUNER_SAMPLE_RATE = 16000, TUNER_FRAME_SAMPLES = 2048 };
#define TUNER_A4_HZ 440
#define TUNER_IN_TUNE_CENTS 5

typedef struct {
    const char *name;
    int octave;
    float target_hz;
    float cents;
} TunerNote;

float TunerPitch_Estimate(const float samples[TUNER_FRAME_SAMPLES]);
bool TunerPitch_NearestNote(float frequency_hz, TunerNote *note);

#endif
