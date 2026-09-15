//-----------------------------------------------------------------------------
// MurmurHash3 was written by Austin Appleby, and is placed in the public
// domain. The author hereby disclaims copyright to this source code.

// Note - The x86 and x64 versions do _not_ produce the same results, as the
// algorithms are optimized for their respective platforms. You can still
// compile and run any of them on any platform, but your performance with the
// non-native version will be less than optimal.

// #include "Minimal.h"
// #include "MurmurHash3.h"

//-----------------------------------------------------------------------------
// Platform-specific functions and macros

// Microsoft Visual Studio

#ifndef FALLS_THROUGH_TO
#if __GNUC__ >= 7
#define FALLS_THROUGH_TO __attribute__((fallthrough));
#elif __GNUC__ < 7
#define FALLS_THROUGH_TO
#endif
#endif

#define	FORCE_INLINE inline __attribute__((always_inline))

static inline unsigned int rotl32 ( unsigned int x, char r )
{
  return (x << r) | (x >> (32 - r));
}

static inline unsigned long rotl64 ( unsigned long x, char r )
{
  return (x << r) | (x >> (64 - r));
}

#define	ROTL32(x,y)	rotl32(x,y)
#define ROTL64(x,y)	rotl64(x,y)

#define BIG_CONSTANT(x) (x##LLU)

//-----------------------------------------------------------------------------
// Block read - if your platform needs to do endian-swapping or can only
// handle aligned reads, do the conversion here

FORCE_INLINE static unsigned int getblock ( const unsigned int * p, int i )
{
  return p[i];
}

//-----------------------------------------------------------------------------
// Finalization mix - force all bits of a hash block to avalanche

FORCE_INLINE static unsigned int fmix ( unsigned int h )
{
  h ^= h >> 16;
  h *= 0x85ebca6b;
  h ^= h >> 13;
  h *= 0xc2b2ae35;
  h ^= h >> 16;

  return h;
}

//-----------------------------------------------------------------------------
FORCE_INLINE
static void MurmurHash3_x86_128 ( const void * key, const int len,
                           unsigned int seed, void * out )
{
  const unsigned char * data = (const unsigned char*)key;
  const int nblocks = len / 16;

  unsigned int h1 = seed;
  unsigned int h2 = seed;
  unsigned int h3 = seed;
  unsigned int h4 = seed;

  unsigned int c1 = 0x239b961b; 
  unsigned int c2 = 0xab0e9789;
  unsigned int c3 = 0x38b34ae5; 
  unsigned int c4 = 0xa1e38b93;

  //----------
  // body

  const unsigned int * blocks = (const unsigned int *)(data + nblocks*16);

  for(int i = -nblocks; i; i++)
  {
    unsigned int k1 = getblock(blocks,i*4+0);
    unsigned int k2 = getblock(blocks,i*4+1);
    unsigned int k3 = getblock(blocks,i*4+2);
    unsigned int k4 = getblock(blocks,i*4+3);

    k1 *= c1; k1  = ROTL32(k1,15); k1 *= c2; h1 ^= k1;

    h1 = ROTL32(h1,19); h1 += h2; h1 = h1*5+0x561ccd1b;

    k2 *= c2; k2  = ROTL32(k2,16); k2 *= c3; h2 ^= k2;

    h2 = ROTL32(h2,17); h2 += h3; h2 = h2*5+0x0bcaa747;

    k3 *= c3; k3  = ROTL32(k3,17); k3 *= c4; h3 ^= k3;

    h3 = ROTL32(h3,15); h3 += h4; h3 = h3*5+0x96cd1c35;

    k4 *= c4; k4  = ROTL32(k4,18); k4 *= c1; h4 ^= k4;

    h4 = ROTL32(h4,13); h4 += h1; h4 = h4*5+0x32ac3b17;
  }

  //----------
  // tail

  const unsigned char * tail = (const unsigned char*)(data + nblocks*16);

  unsigned int k1 = 0;
  unsigned int k2 = 0;
  unsigned int k3 = 0;
  unsigned int k4 = 0;

  switch(len & 15)
  {
  case 15: k4 ^= tail[14] << 16; FALLS_THROUGH_TO
  case 14: k4 ^= tail[13] << 8; FALLS_THROUGH_TO
  case 13: k4 ^= tail[12] << 0;
           k4 *= c4; k4  = ROTL32(k4,18); k4 *= c1; h4 ^= k4;
           FALLS_THROUGH_TO
  case 12: k3 ^= tail[11] << 24; FALLS_THROUGH_TO
  case 11: k3 ^= tail[10] << 16; FALLS_THROUGH_TO
  case 10: k3 ^= tail[ 9] << 8; FALLS_THROUGH_TO
  case  9: k3 ^= tail[ 8] << 0;
           k3 *= c3; k3  = ROTL32(k3,17); k3 *= c4; h3 ^= k3;
           FALLS_THROUGH_TO
  case  8: k2 ^= tail[ 7] << 24; FALLS_THROUGH_TO
  case  7: k2 ^= tail[ 6] << 16; FALLS_THROUGH_TO
  case  6: k2 ^= tail[ 5] << 8; FALLS_THROUGH_TO
  case  5: k2 ^= tail[ 4] << 0;
           k2 *= c2; k2  = ROTL32(k2,16); k2 *= c3; h2 ^= k2;
           FALLS_THROUGH_TO
  case  4: k1 ^= tail[ 3] << 24; FALLS_THROUGH_TO
  case  3: k1 ^= tail[ 2] << 16; FALLS_THROUGH_TO
  case  2: k1 ^= tail[ 1] << 8; FALLS_THROUGH_TO
  case  1: k1 ^= tail[ 0] << 0;
           k1 *= c1; k1  = ROTL32(k1,15); k1 *= c2; h1 ^= k1;
  };

  //----------
  // finalization

  h1 ^= len; h2 ^= len; h3 ^= len; h4 ^= len;

  h1 += h2; h1 += h3; h1 += h4;
  h2 += h1; h3 += h1; h4 += h1;

  h1 = fmix(h1);
  h2 = fmix(h2);
  h3 = fmix(h3);
  h4 = fmix(h4);

  h1 += h2; h1 += h3; h1 += h4;
  h2 += h1; h3 += h1; h4 += h1;

  ((unsigned int*)out)[0] = h1;
  ((unsigned int*)out)[1] = h2;
  //((unsigned int*)out)[2] = h3;
  //((unsigned int*)out)[3] = h4;
}

//-----------------------------------------------------------------------------