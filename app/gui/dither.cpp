#include "dither.h"

#include <cstdint>

// A hash rather than a table of random numbers. A table has to repeat, and on
// a gradient this shallow the repeat is visible: two rows sixty pixels apart
// hold nearly the same color, so identical noise on both makes the tile itself
// show up as a faint grid.
static uint32_t mix(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

float Dither::noise(int x, int y)
{
    const uint32_t hashed = mix(uint32_t(x) ^ mix(uint32_t(y) + 0x9e3779b9u));

    // The two halves of a well mixed hash are independent enough to use as two
    // samples, and summing them gives the triangular distribution we want
    const float first = (hashed & 0xFFFF) / 65535.0f;
    const float second = ((hashed >> 16) & 0xFFFF) / 65535.0f;
    return first + second - 1.0f;
}
