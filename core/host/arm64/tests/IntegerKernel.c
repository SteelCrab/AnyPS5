typedef unsigned char U8;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef long long I64;

static const U32 weights[8] = {17, 93, 5, 131, 47, 19, 251, 61};
static volatile U64 initial = 0x123456789abcdef0ULL;
static volatile I64 signedInitial = -0x123456789abcdefLL;
static const U32 packedLeft[4] = {17, 93, 5, 131};
static const U32 packedRight[4] = {0x12345678, 0x9abcdef0, 0x11223344, 0x55667788};

void PackedMix(U32* restrict result, const U32* restrict left, const U32* restrict right) {
    const U32 mask[4] = {13, 17, 29, 101};
    for (U32 i = 0; i < 4; ++i) result[i] = ((left[i] + right[i]) ^ left[i]) & (right[i] | mask[i]);
}

static U64 mix(U64 value, U32 salt) {
    value = (value ^ salt) * 0x9e3779b97f4a7c15ULL;
    value ^= value >> 31;
    return (value << 13) | (value >> 51);
}

U64 GuestMain(void) {
    volatile U8 bytes[29];
    volatile unsigned short words[29];
    U64 hash = initial;
    I64 signedValue = signedInitial;
    for (U32 i = 0; i < 29; ++i) {
        bytes[i] = (U8)(i * 37 + 0x81);
        words[i] = (unsigned short)(i * 741 + 0x8001);
    }
    for (U32 i = 0; i < 29; ++i) {
        U32 divisor = weights[i & 7] + bytes[i];
        I64 narrow = (short)words[i];
        hash = mix(hash + bytes[i], weights[i & 7]);
        hash ^= hash / divisor;
        hash += hash % divisor;
        signedValue = signedValue / (I64)divisor + narrow;
        hash += (U64)(signedValue % (I64)divisor);
        hash ^= (U64)(narrow < -100 ? narrow : -100);
    }
    U32 packed[4];
    PackedMix(packed, packedLeft, packedRight);
    hash ^= packed[0];
    hash += packed[1];
    hash ^= packed[2];
    hash += packed[3];
    return hash ^ (U64)signedValue;
}
