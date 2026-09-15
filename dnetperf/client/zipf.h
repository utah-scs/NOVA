#ifndef ZIPF_H
#define ZIPF_H

#include <stdint.h>

/*
 * Zipf sampler using the O(1) Luc Devroye rejection method.
 *
 * Call zipf_init() once at startup (it computes the O(n) normalization
 * constant zeta_n), then call zipf_sample() per packet.
 *
 * zipf_sample() takes a raw 64-bit random value and maps it to a key in
 * [0, n), where key 0 is the hottest.  theta controls skew: closer to 1
 * means more concentrated traffic (YCSB default is 0.99).
 */

struct zipf_params {
    uint64_t n;       /* key-space size (key_mask + 1) */
    double   theta;   /* skew exponent, in (0, 1) */
    double   zeta_n;  /* sum_{i=1}^{n} i^{-theta} */
    double   zeta_2;  /* sum_{i=1}^{2} i^{-theta} = 1 + 0.5^theta */
    double   alpha;   /* 1 / (1 - theta) */
    double   eta;     /* (1 - (2/n)^(1-theta)) / (1 - zeta_2/zeta_n) */
};

void     zipf_init(struct zipf_params *zp, uint64_t n, double theta);
uint64_t zipf_sample(const struct zipf_params *zp, uint64_t rng_val);

#endif /* ZIPF_H */
