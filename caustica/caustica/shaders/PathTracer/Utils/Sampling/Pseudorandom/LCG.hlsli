/** Simple linear congruential generator (LCG).

    The code uses the parameters from the book series "Numerical Recipes".
    The period is 2^32 and its state size is 32 bits.

    Note: Only for basic applications. The generator has poor statistical
    properties and is sensitive to good seeding. If many parallel generators
    are used (e.g. one per pixel) there will be significant correlation
    between the generated pseudorandom sequences. In those cases, it is
    recommended to use one of the generators with larger state.
*/

struct LCG
{
    uint state;
};

/** Generates the next pseudorandom number in the sequence (32 bits).
*/
uint nextRandom(inout LCG rng)
{
    const uint A = 1664525u;
    const uint C = 1013904223u;
    rng.state = (A * rng.state + C);
    return rng.state;
}

/** Initialize LCG pseudorandom number generator.
    \param[in] s0 Initial state (seed).
*/
LCG createLCG(uint s0)
{
    LCG rng;
    rng.state = s0;
    return rng;
}
