typedef char int8_t;
typedef uchar uint8_t;
typedef short int16_t;
typedef ushort uint16_t;
typedef int int32_t;
typedef uint uint32_t;

struct __attribute__ ((packed)) block_q4_0
{
    half d;
    uint8_t qs[QK4_0 / 2];
};

struct __attribute__ ((packed)) block_q4_1
{
    half d;
    half m;
    uint8_t qs[QK4_1 / 2];
};

struct __attribute__ ((packed)) block_q5_0
{
    half d;
    uint32_t qh;
    uint8_t qs[QK5_0 / 2];
};

struct __attribute__ ((packed)) block_q5_1
{
    half d;
    half m;
    uint32_t qh;
    uint8_t qs[QK5_1 / 2];
};

struct __attribute__ ((packed)) block_q8_0
{
    half d;
    int8_t qs[QK8_0];
};

struct __attribute__((packed)) block_q2_K
{
    uint8_t scales[16];
    uint8_t qs[64];
    half d;
    half dmin;
};

struct __attribute__((packed)) block_q3_K
{
    uint8_t hmask[32];
    uint8_t qs[64];
    uint8_t scales[12];
    half d;
};

struct __attribute__((packed)) block_q4_K
{
    half d;
    half dmin;
    uint8_t scales[12];
    uint8_t qs[128];
};

struct __attribute__((packed)) block_q5_K
{
    half d;
    half dmin;
    uint8_t scales[12];
    uint8_t qh[32];
    uint8_t qs[128];
};

struct __attribute__((packed)) block_q6_K
{
    uint8_t ql[128];
    uint8_t qh[64];
    int8_t scales[16];
    half d;
};

__kernel void convert_fp16_to_fp32(__global half* x, __global float* y) {
    const uint i = get_global_id(0);

    y[i] = vload_half(0, &x[i]);
}

void dequantize_q4_0(__global const struct block_q4_0* x, const int ib, const int iqs, float* v0, float* v1) {
    const float d = vload_half(0, &x[ib].d);

    const uint8_t vui = x[ib].qs[iqs];

    const int8_t vi0 = vui & 0xF;
    const int8_t vi1 = vui >> 4;

    *v0 = (vi0 - 8)*d;
    *v1 = (vi1 - 8)*d;
}
void dequantize_q4_1(__global const struct block_q4_1* x, const int ib, const int iqs, float* v0, float* v1) {
    const float d = vload_half(0, &x[ib].d);
    const float m = vload_half(0, &x[ib].m);

    const uint8_t vui = x[ib].qs[iqs];

    const int8_t vi0 = vui & 0xF;
    const int8_t vi1 = vui >> 4;

    *v0 = vi0*d + m;
    *v1 = vi1*d + m;
}
void dequantize_q5_0(__global const struct block_q5_0* x, const int ib, const int iqs, float* v0, float* v1) {
    const float d = vload_half(0, &x[ib].d);

    uint32_t qh = x[ib].qh;

    const uint8_t xh_0 = ((qh >> (iqs +  0)) << 4) & 0x10;
    const uint8_t xh_1 = ((qh >> (iqs + 12))     ) & 0x10;

    const int32_t x0 = ((x[ib].qs[iqs] & 0xf) | xh_0) - 16;
    const int32_t x1 = ((x[ib].qs[iqs] >>  4) | xh_1) - 16;

    *v0 = x0*d;
    *v1 = x1*d;
}
void dequantize_q5_1(__global const struct block_q5_1* x, const int ib, const int iqs, float* v0, float* v1) {
    const float d = vload_half(0, &x[ib].d);
    const float m = vload_half(0, &x[ib].m);

    uint32_t qh = x[ib].qh;

    const uint8_t xh_0 = ((qh >> (iqs +  0)) << 4) & 0x10;
    const uint8_t xh_1 = ((qh >> (iqs + 12))     ) & 0x10;

    const int32_t x0 = ((x[ib].qs[iqs] & 0xf) | xh_0);
    const int32_t x1 = ((x[ib].qs[iqs] >>  4) | xh_1);

    *v0 = x0*d + m;
    *v1 = x1*d + m;
}
void dequantize_q8_0(__global const struct block_q8_0* x, const int ib, const int iqs, float* v0, float* v1) {
    const float d = vload_half(0, &x[ib].d);

    const int8_t vi0 = x[ib].qs[iqs + 0];
    const int8_t vi1 = x[ib].qs[iqs + 1];

    *v0 = vi0*d;
    *v1 = vi1*d;
}
void convert_f16(__global half* x, const int ib, const int iqs, float* v0, float* v1){
    *v0 = vload_half(0, &x[ib + 0]);
    *v1 = vload_half(0, &x[ib + 1]);
}

inline void get_scale_min_k4(int j, const __global uint8_t *q, uint8_t *d, uint8_t *m)
{
    if (j < 4)
    {
        *d = q[j] & 63;
        *m = q[j + 4] & 63;
    }
    else
    {
        *d = (q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4);
        *m = (q[j + 4] >> 4) | ((q[j - 0] >> 6) << 4);
    }
}

__kernel void dequantize_block_q2_K(__global const struct block_q2_K *x, __global float *yy)
{
    const int i = get_group_id(0) + get_global_offset(0);
    const int tid = get_local_id(0);
    const int n = tid / 32;
    const int l = tid - 32 * n;
    const int is = 8 * n + l / 16;

    const uint8_t q = x[i].qs[32 * n + l];
    __global float *y = yy + get_group_id(0) * QK_K + 128 * n;

    const float dall = vload_half(0, &x[i].d);
    const float dmin = vload_half(0, &x[i].dmin);

    y[l + 0] = dall * (x[i].scales[is + 0] & 0xF) * ((q >> 0) & 3) - dmin * (x[i].scales[is + 0] >> 4);
    y[l + 32] = dall * (x[i].scales[is + 2] & 0xF) * ((q >> 2) & 3) - dmin * (x[i].scales[is + 2] >> 4);
    y[l + 64] = dall * (x[i].scales[is + 4] & 0xF) * ((q >> 4) & 3) - dmin * (x[i].scales[is + 4] >> 4);
    y[l + 96] = dall * (x[i].scales[is + 6] & 0xF) * ((q >> 6) & 3) - dmin * (x[i].scales[is + 6] >> 4);
}

__kernel void dequantize_block_q3_K(__global const struct block_q3_K *x, __global float *yy)
{
    int r = get_local_id(0) / 4;
    int i = get_group_id(0) + get_global_offset(0);
    int tid = r / 2;
    int is0 = r % 2;
    int l0 = 16 * is0 + 4 * (get_local_id(0) % 4);
    int n = tid / 4;
    int j = tid - 4 * n;

    uint8_t m = 1 << (4 * n + j);
    int is = 8 * n + 2 * j + is0;
    int shift = 2 * j;

    int8_t us = is < 4 ? (x[i].scales[is - 0] & 0xF) | (((x[i].scales[is + 8] >> 0) & 3) << 4)
              : is < 8 ? (x[i].scales[is - 0] & 0xF) | (((x[i].scales[is + 4] >> 2) & 3) << 4)
              : is < 12  ? (x[i].scales[is - 8] >> 4) | (((x[i].scales[is + 0] >> 4) & 3) << 4)
              : (x[i].scales[is - 8] >> 4) | (((x[i].scales[is - 4] >> 6) & 3) << 4);
    float d_all = vload_half(0, &x[i].d);
    float dl = d_all * (us - 32);

    __global float *y = yy + get_group_id(0) * QK_K + 128 * n + 32 * j;
    const __global uint8_t *q = x[i].qs + 32 * n;
    const __global uint8_t *hm = x[i].hmask;

    for (int l = l0; l < l0 + 4; ++l)
        y[l] = dl * ((int8_t)((q[l] >> shift) & 3) - ((hm[l] & m) ? 0 : 4));
}

__kernel void dequantize_block_q4_K(__global const struct block_q4_K *x, __global float *yy)
{
    const int i = get_group_id(0) + get_global_offset(0);
    const int tid = get_local_id(0);
    const int il = tid / 8;
    const int ir = tid % 8;
    const int is = 2 * il;
    const int n = 4;

    __global float *y = yy + get_group_id(0) * QK_K + 64 * il + n * ir;

    const float dall = vload_half(0, &x[i].d);
    const float dmin = vload_half(0, &x[i].dmin);

    __global const uint8_t *q = x[i].qs + 32 * il + n * ir;

    uint8_t sc, m;
    get_scale_min_k4(is + 0, x[i].scales, &sc, &m);
    float d1 = dall * sc;
    float m1 = dmin * m;
    get_scale_min_k4(is + 1, x[i].scales, &sc, &m);
    float d2 = dall * sc;
    float m2 = dmin * m;
    for (int l = 0; l < n; ++l)
    {
        y[l + 0] = d1 * (q[l] & 0xF) - m1;
        y[l + 32] = d2 * (q[l] >> 4) - m2;
    }
}

__kernel void dequantize_block_q5_K(__global const struct block_q5_K *x, __global float *yy)
{
    const int i = get_group_id(0) + get_global_offset(0);
    const int tid = get_local_id(0);
    const int il = tid / 16;
    const int ir = tid % 16;
    const int is = 2 * il;

    __global float *y = yy + get_group_id(0) * QK_K + 64 * il + 2 * ir;

    const float dall = vload_half(0, &x[i].d);
    const float dmin = vload_half(0, &x[i].dmin);

    __global const uint8_t *ql = x[i].qs + 32 * il + 2 * ir;
    __global const uint8_t *qh = x[i].qh + 2 * ir;

    uint8_t sc, m;
    get_scale_min_k4(is + 0, x[i].scales, &sc, &m);
    const float d1 = dall * sc;
    const float m1 = dmin * m;
    get_scale_min_k4(is + 1, x[i].scales, &sc, &m);
    const float d2 = dall * sc;
    const float m2 = dmin * m;

    uint8_t hm = 1 << (2 * il);
    y[0] = d1 * ((ql[0] & 0xF) + (qh[0] & hm ? 16 : 0)) - m1;
    y[1] = d1 * ((ql[1] & 0xF) + (qh[1] & hm ? 16 : 0)) - m1;
    hm <<= 1;
    y[32] = d2 * ((ql[0] >> 4) + (qh[0] & hm ? 16 : 0)) - m2;
    y[33] = d2 * ((ql[1] >> 4) + (qh[1] & hm ? 16 : 0)) - m2;
}

__kernel void dequantize_block_q6_K(__global const struct block_q6_K *x, __global float *yy)
{
    const int i = get_group_id(0) + get_global_offset(0);
    const int tid = get_local_id(0);
    const int ip = tid / 32;
    const int il = tid - 32 * ip;
    const int is = 8 * ip + il / 16;

    __global float *y = yy + get_group_id(0) * QK_K + 128 * ip + il;

    const float d = vload_half(0, &x[i].d);

    __global const uint8_t *ql = x[i].ql + 64 * ip + il;
    const uint8_t qh = x[i].qh[32 * ip + il];
    __global const int8_t *sc = x[i].scales + is;

    y[0] = d * sc[0] * ((int8_t)((ql[0] & 0xF) | (((qh >> 0) & 3) << 4)) - 32);
    y[32] = d * sc[2] * ((int8_t)((ql[32] & 0xF) | (((qh >> 2) & 3) << 4)) - 32);
    y[64] = d * sc[4] * ((int8_t)((ql[0] >> 4) | (((qh >> 4) & 3) << 4)) - 32);
    y[96] = d * sc[6] * ((int8_t)((ql[32] >> 4) | (((qh >> 6) & 3) << 4)) - 32);
}

__kernel void dequantize_mul_mat_vec_q2_K(__global const struct block_q2_K * xx, __local float* tmp, __global float* yy, __global float* dst, const int ncols) {

    const int row = get_group_id(0);

    const int num_blocks_per_row = ncols / QK_K;
    const int ib0 = row*num_blocks_per_row + get_global_offset(0);

    __global const struct block_q2_K * x = xx + ib0;

    const int tid = get_local_id(0)/K_QUANTS_PER_ITERATION;  // 0...31 or 0...15
    const int ix  = get_local_id(0)%K_QUANTS_PER_ITERATION;  // 0 or 0,1

    const int step = 16/K_QUANTS_PER_ITERATION;

    const int im = tid/step;                             // 0 or 1. 0 computes 0..., 1 computes 128...
    const int in = tid - step*im;                        // 0...15 or 0...7

    const int l0 = K_QUANTS_PER_ITERATION*in;            // 0...15 or 0...14 in steps of 2
    const int q_offset = 32*im + l0;
    const int s_offset = 8*im;
    const int y_offset = 128*im + l0;

    tmp[16 * ix + tid] = 0;

    uint32_t aux[4];
    const uint8_t * d = (const uint8_t *)aux;
    const uint8_t * m = (const uint8_t *)(aux + 2);

    for (int i = ix; i < num_blocks_per_row; i += K_QUANTS_PER_ITERATION) {

        __global const float   * y = yy + i * QK_K + y_offset;
        __global const uint8_t * q = x[i].qs + q_offset;

        const float dall = vload_half(0, &x[i].d);
        const float dmin = vload_half(0, &x[i].dmin);

        __global const uint32_t * a = (__global const uint32_t *)(x[i].scales + s_offset);
        aux[0] = a[0] & 0x0f0f0f0f;
        aux[1] = a[1] & 0x0f0f0f0f;
        aux[2] = (a[0] >> 4) & 0x0f0f0f0f;
        aux[3] = (a[1] >> 4) & 0x0f0f0f0f;

        float sum1 = 0, sum2 = 0;
        for (int l = 0; l < K_QUANTS_PER_ITERATION; ++l) {
            sum1 += y[l+ 0] * d[0] * ((q[l+ 0] >> 0) & 3)
                  + y[l+32] * d[2] * ((q[l+ 0] >> 2) & 3)
                  + y[l+64] * d[4] * ((q[l+ 0] >> 4) & 3)
                  + y[l+96] * d[6] * ((q[l+ 0] >> 6) & 3)
                  + y[l+16] * d[1] * ((q[l+16] >> 0) & 3)
                  + y[l+48] * d[3] * ((q[l+16] >> 2) & 3)
                  + y[l+80] * d[5] * ((q[l+16] >> 4) & 3)
                  +y[l+112] * d[7] * ((q[l+16] >> 6) & 3);
            sum2 += y[l+ 0] * m[0] + y[l+32] * m[2] + y[l+64] * m[4] + y[ l+96] * m[6]
                  + y[l+16] * m[1] + y[l+48] * m[3] + y[l+80] * m[5] + y[l+112] * m[7];

        }
        tmp[16 * ix + tid] += dall * sum1 - dmin * sum2;

    }

    // sum up partial sums and write back result
    barrier(CLK_LOCAL_MEM_FENCE);
    for (int s=16; s>0; s>>=1) {
        if (tid < s) {
            tmp[tid] += tmp[tid + s];
        }
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if (tid == 0) {
        dst[row] = tmp[0];
    }
}

__kernel void dequantize_mul_mat_vec_q3_K(__global const struct block_q3_K * xx, __local float* tmp, __global float* yy, __global float* dst, const int ncols) {
    const uint16_t kmask1 = 0x0303;
    const uint16_t kmask2 = 0x0f0f;

    const int row = get_group_id(0);

    const int num_blocks_per_row = ncols / QK_K;
    const int ib0 = row*num_blocks_per_row + get_global_offset(0);

    __global const struct block_q3_K * x = xx + ib0;

    const int tid = get_local_id(0)/K_QUANTS_PER_ITERATION;  // 0...31 or 0...16
    const int ix  = get_local_id(0)%K_QUANTS_PER_ITERATION;  // 0 or 0,1

    const int n  = K_QUANTS_PER_ITERATION;               // iterations in the inner loop
    const int step = 16/K_QUANTS_PER_ITERATION;
    const int im = tid/step;                             // 0 or 1. 0 computes 0..., 1 computes 128...
    const int in = tid - step*im;                        // 0....15 or 0...7

    const uint8_t m = 1 << (4*im);

    const int l0 = n*in;                                 // 0...15 or 0...14 in steps of 2
    const int q_offset =  32*im + l0;
    const int y_offset = 128*im + l0;

    uint16_t utmp[4];
    const int8_t * s = (const int8_t *)utmp;

    const uint16_t s_shift = 4*im;

    tmp[16 * ix + tid] = 0;

    for (int i = ix; i < num_blocks_per_row; i += K_QUANTS_PER_ITERATION) {

        __global const float   * y  = yy + i * QK_K + y_offset;
        __global const uint8_t * q = x[i].qs + q_offset;
        __global const uint8_t * h = x[i].hmask + l0;

        __global const uint16_t * a = (__global const uint16_t *)x[i].scales;
        utmp[0] = ((a[0] >> s_shift) & kmask2) | (((a[4] >> (s_shift + 0)) & kmask1) << 4);
        utmp[1] = ((a[1] >> s_shift) & kmask2) | (((a[5] >> (s_shift + 0)) & kmask1) << 4);
        utmp[2] = ((a[2] >> s_shift) & kmask2) | (((a[4] >> (s_shift + 2)) & kmask1) << 4);
        utmp[3] = ((a[3] >> s_shift) & kmask2) | (((a[5] >> (s_shift + 2)) & kmask1) << 4);

        const float d = vload_half(0, &x[i].d);

        float sum = 0;
        for (int l = 0; l < n; ++l) {
            sum += y[l+ 0] * (s[0] - 32) * (((q[l] >> 0) & 3) - (h[l] & (m << 0) ? 0 : 4))
                 + y[l+32] * (s[2] - 32) * (((q[l] >> 2) & 3) - (h[l] & (m << 1) ? 0 : 4))
                 + y[l+64] * (s[4] - 32) * (((q[l] >> 4) & 3) - (h[l] & (m << 2) ? 0 : 4))
                 + y[l+96] * (s[6] - 32) * (((q[l] >> 6) & 3) - (h[l] & (m << 3) ? 0 : 4));
            sum += y[l+16] * (s[1] - 32) * (((q[l+16] >> 0) & 3) - (h[l+16] & (m << 0) ? 0 : 4))
                 + y[l+48] * (s[3] - 32) * (((q[l+16] >> 2) & 3) - (h[l+16] & (m << 1) ? 0 : 4))
                 + y[l+80] * (s[5] - 32) * (((q[l+16] >> 4) & 3) - (h[l+16] & (m << 2) ? 0 : 4))
                + y[l+112] * (s[7] - 32) * (((q[l+16] >> 6) & 3) - (h[l+16] & (m << 3) ? 0 : 4));
        }
        tmp[16 * ix + tid] += d * sum;

    }

    // sum up partial sums and write back result
    barrier(CLK_LOCAL_MEM_FENCE);
    for (int s=16; s>0; s>>=1) {
        if (tid < s) {
            tmp[tid] += tmp[tid + s];
        }
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if (tid == 0) {
        dst[row] = tmp[0];
    }
}

__kernel void dequantize_mul_mat_vec_q4_K(__global const struct block_q4_K * xx, __local float* tmp, __global float* yy, __global float* dst, const int ncols) {

    //to rename it later, just to test now
    const uint16_t kmask1 = 0x3f3f;
    const uint16_t kmask2 = 0x0f0f;
    const uint16_t kmask3 = 0xc0c0;

    const int row = get_group_id(0);
    const int num_blocks_per_row = ncols / QK_K;
    const int ib0 = row*num_blocks_per_row + get_global_offset(0);

    const int tid = get_local_id(0)/K_QUANTS_PER_ITERATION;  // 0...15
    const int ix  = get_local_id(0)%K_QUANTS_PER_ITERATION;

    const int step = 8/K_QUANTS_PER_ITERATION;

    const int il  = tid/step;     // 0...3
    const int ir  = tid - step*il;// 0...3
    const int n   = 2*K_QUANTS_PER_ITERATION;

    const int im = il/2;  // 0 or 1. 0 computes 0,32 + 128,160, 1 computes 64,96 + 192,224
    const int in = il%2;

    const int l0 = n*(2*ir + in);
    const int q_offset = 32*im + l0;
    const int y_offset = 64*im + l0;

    uint16_t aux[4];
    const uint8_t * sc = (const uint8_t *)aux;

    __global const struct block_q4_K * x = xx + ib0;

    tmp[16 * ix + tid] = 0;

    for (int i = ix; i < num_blocks_per_row; i += K_QUANTS_PER_ITERATION) {

        __global const uint8_t * q1 = x[i].qs + q_offset;
        __global const uint8_t * q2 = q1 + 64;
        __global const float   * y1 = yy + i*QK_K + y_offset;
        __global const float   * y2 = y1 + 128;

        const float dall = vload_half(0, &x[i].d);
        const float dmin = vload_half(0, &x[i].dmin);

        __global const uint16_t * a = (__global const uint16_t *)x[i].scales;
        aux[0] = a[im+0] & kmask1;
        aux[1] = a[im+2] & kmask1;
        aux[2] = ((a[im+4] >> 0) & kmask2) | ((a[im+0] & kmask3) >> 2);
        aux[3] = ((a[im+4] >> 4) & kmask2) | ((a[im+2] & kmask3) >> 2);

        float4 s = (float4)(0.f);
        float smin = 0;
        for (int l = 0; l < n; ++l) {
            s.x += y1[l] * (q1[l] & 0xF); s.y += y1[l+32] * (q1[l] >> 4);
            s.z += y2[l] * (q2[l] & 0xF); s.w += y2[l+32] * (q2[l] >> 4);
            smin += y1[l] * sc[2] + y1[l+32] * sc[3] + y2[l] * sc[6] + y2[l+32] * sc[7];
        }
        tmp[16 * ix + tid] += dall * (s.x * sc[0] + s.y * sc[1] + s.z * sc[4] + s.w * sc[5]) - dmin * smin;

    }

    // sum up partial sums and write back result
    barrier(CLK_LOCAL_MEM_FENCE);
    for (int s=16; s>0; s>>=1) {
        if (tid < s) {
            tmp[tid] += tmp[tid + s];
        }
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if (tid == 0) {
        dst[row] = tmp[0];
    }
}

__kernel void dequantize_mul_mat_vec_q5_K(__global const struct block_q5_K * xx, __local float* tmp, __global float* yy, __global float* dst, const int ncols) {

    const uint16_t kmask1 = 0x3f3f;
    const uint16_t kmask2 = 0x0f0f;
    const uint16_t kmask3 = 0xc0c0;

    const int row = get_group_id(0);
    const int num_blocks_per_row = ncols / QK_K;
    const int ib0 = row*num_blocks_per_row + get_global_offset(0);

    const int tid = get_local_id(0)/2;  // 0...15
    const int ix  = get_local_id(0)%2;

    const int il  = tid/4;     // 0...3
    const int ir  = tid - 4*il;// 0...3
    const int n   = 2;

    const int im = il/2;  // 0 or 1. 0 computes 0,32 + 128,160, 1 computes 64,96 + 192,224
    const int in = il%2;

    const int l0 = n*(2*ir + in);
    const int q_offset = 32*im + l0;
    const int y_offset = 64*im + l0;

    const uint8_t hm1  = 1 << (2*im);
    const uint8_t hm2  = hm1 << 4;

    uint16_t aux[4];
    const uint8_t * sc = (const uint8_t *)aux;

    __global const struct block_q5_K * x = xx + ib0;

    tmp[16 * ix + tid] = 0;

    for (int i = ix; i < num_blocks_per_row; i += 2) {

        __global const uint8_t * ql1 = x[i].qs + q_offset;
        __global const uint8_t * ql2 = ql1 + 64;
        __global const uint8_t * qh  = x[i].qh + l0;
        __global const float   * y1  = yy + i*QK_K + y_offset;
        __global const float   * y2  = y1 + 128;

        const float dall = vload_half(0, &x[i].d);
        const float dmin = vload_half(0, &x[i].dmin);

        __global const uint16_t * a = (__global const uint16_t *)x[i].scales;
        aux[0] = a[im+0] & kmask1;
        aux[1] = a[im+2] & kmask1;
        aux[2] = ((a[im+4] >> 0) & kmask2) | ((a[im+0] & kmask3) >> 2);
        aux[3] = ((a[im+4] >> 4) & kmask2) | ((a[im+2] & kmask3) >> 2);

        float4 sum = (float4)(0.f);
        float smin = 0;
        for (int l = 0; l < n; ++l) {
            sum.x += y1[l+ 0] * ((ql1[l+ 0] & 0xF) + (qh[l+ 0] & (hm1 << 0) ? 16 : 0))
                   + y1[l+16] * ((ql1[l+16] & 0xF) + (qh[l+16] & (hm1 << 0) ? 16 : 0));
            sum.y += y1[l+32] * ((ql1[l+ 0] >>  4) + (qh[l+ 0] & (hm1 << 1) ? 16 : 0))
                   + y1[l+48] * ((ql1[l+16] >>  4) + (qh[l+16] & (hm1 << 1) ? 16 : 0));
            sum.z += y2[l+ 0] * ((ql2[l+ 0] & 0xF) + (qh[l+ 0] & (hm2 << 0) ? 16 : 0))
                   + y2[l+16] * ((ql2[l+16] & 0xF) + (qh[l+16] & (hm2 << 0) ? 16 : 0));
            sum.w += y2[l+32] * ((ql2[l+ 0] >>  4) + (qh[l+ 0] & (hm2 << 1) ? 16 : 0))
                   + y2[l+48] * ((ql2[l+16] >>  4) + (qh[l+16] & (hm2 << 1) ? 16 : 0));
            smin += (y1[l] + y1[l+16]) * sc[2] + (y1[l+32] + y1[l+48]) * sc[3]
                  + (y2[l] + y2[l+16]) * sc[6] + (y2[l+32] + y2[l+48]) * sc[7];
        }
        tmp[16 * ix + tid] += dall * (sum.x * sc[0] + sum.y * sc[1] + sum.z * sc[4] + sum.w * sc[5]) - dmin * smin;

    }

    // sum up partial sums and write back result
    barrier(CLK_LOCAL_MEM_FENCE);
    for (int s=16; s>0; s>>=1) {
        if (tid < s) {
            tmp[tid] += tmp[tid + s];
        }
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if (tid == 0) {
        dst[row] = tmp[0];
    }
}

__kernel void dequantize_mul_mat_vec_q6_K(__global const struct block_q6_K * xx, __local float* tmp, __global const float * yy, __global float * dst, const int ncols) {

    const int row = get_group_id(0);

    const int num_blocks_per_row = ncols / QK_K;
    const int ib0 = row*num_blocks_per_row + get_global_offset(0);

    __global const struct block_q6_K * x = xx + ib0;

    const int tid = get_local_id(0)/K_QUANTS_PER_ITERATION;  // 0...31 or 0...16
    const int ix  = get_local_id(0)%K_QUANTS_PER_ITERATION;  // 0 or 0, 1

    const int step = 16/K_QUANTS_PER_ITERATION;          // 16 or 8

    const int im = tid/step;                             // 0 or 1. 0 computes 0..., 1 computes 128...
    const int in = tid - step*im;                        // 0...15 or 0...7

#if K_QUANTS_PER_ITERATION == 1
    const int l0 = K_QUANTS_PER_ITERATION*in;            // 0...15
    const int is = 0;

#else

    const int l0 = 4 * in;                               // 0, 4, 8, ..., 28
    const int is = in / 4;

#endif

    const int ql_offset = 64*im + l0;
    const int qh_offset = 32*im + l0;
    const int s_offset  =  8*im + is;
    const int y_offset = 128*im + l0;

    tmp[16 * ix + tid] = 0; // partial sum for thread in warp

    for (int i = ix; i < num_blocks_per_row; i += K_QUANTS_PER_ITERATION) {

        __global const float   * y  = yy + i * QK_K + y_offset;
        __global const uint8_t * ql = x[i].ql + ql_offset;
        __global const uint8_t * qh = x[i].qh + qh_offset;
        __global const int8_t  * s  = x[i].scales + s_offset;

        const float d = vload_half(0, &x[i].d);

#if K_QUANTS_PER_ITERATION == 1
        float sum = y[ 0] * s[0] * d * ((int8_t)((ql[ 0] & 0xF) | ((qh[ 0] & 0x03) << 4)) - 32)
                  + y[16] * s[1] * d * ((int8_t)((ql[16] & 0xF) | ((qh[16] & 0x03) << 4)) - 32)
                  + y[32] * s[2] * d * ((int8_t)((ql[32] & 0xF) | ((qh[ 0] & 0x0c) << 2)) - 32)
                  + y[48] * s[3] * d * ((int8_t)((ql[48] & 0xF) | ((qh[16] & 0x0c) << 2)) - 32)
                  + y[64] * s[4] * d * ((int8_t)((ql[ 0]  >> 4) | ((qh[ 0] & 0x30) >> 0)) - 32)
                  + y[80] * s[5] * d * ((int8_t)((ql[16]  >> 4) | ((qh[16] & 0x30) >> 0)) - 32)
                  + y[96] * s[6] * d * ((int8_t)((ql[32]  >> 4) | ((qh[ 0] & 0xc0) >> 2)) - 32)
                  +y[112] * s[7] * d * ((int8_t)((ql[48]  >> 4) | ((qh[16] & 0xc0) >> 2)) - 32);
        tmp[16 * ix + tid] += sum;
#else
        float sum = 0;
        for (int l = 0; l < 4; ++l) {
            sum += y[l+ 0] * s[0] * d * ((int8_t)((ql[l+ 0] & 0xF) | (((qh[l] >> 0) & 3) << 4)) - 32)
                 + y[l+32] * s[2] * d * ((int8_t)((ql[l+32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32)
                 + y[l+64] * s[4] * d * ((int8_t)((ql[l+ 0]  >> 4) | (((qh[l] >> 4) & 3) << 4)) - 32)
                 + y[l+96] * s[6] * d * ((int8_t)((ql[l+32]  >> 4) | (((qh[l] >> 6) & 3) << 4)) - 32);
        }
        tmp[16 * ix + tid] += sum;
#endif

    }

    // sum up partial sums and write back result
    barrier(CLK_LOCAL_MEM_FENCE);
    for (int s=16; s>0; s>>=1) {
        if (tid < s) {
            tmp[tid] += tmp[tid + s];
        }
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if (tid == 0) {
        dst[row] = tmp[0];
    }
}

#define DEQUANT(KERNEL_NAME, X_TYPE, QUANT_K, QUANT_R, DEQUANT_FUNC) \
    __kernel void KERNEL_NAME(__global X_TYPE* x, __global float* y) { \
        const size_t i = get_group_id(0)*get_local_size(0) + get_local_id(0)*2; \
        \
        if (i >= get_global_size(0)) { \
            return; \
        } \
        \
        const uint qk = QUANT_K; \
        const uint qr = QUANT_R; \
        \
        const size_t ib = i/qk + get_global_offset(0); /* block index */ \
        const size_t iqs = (i%qk)/qr; /* quant index */ \
        const int iybs = i - i%qk; /* y block start index */ \
        const int y_offset = qr == 1 ? 1 : qk/2; \
        \
        /* dequantize */ \
        float v0, v1; \
        DEQUANT_FUNC(x, ib, iqs, &v0, &v1); \
        y[iybs + iqs + 0] = v0; \
        y[iybs + iqs + y_offset] = v1; \
    }

#define DEQUANT_MUL_MAT_VEC(KERNEL_NAME, X_TYPE, QUANT_K, QUANT_R, DEQUANT_FUNC) \
    __kernel void KERNEL_NAME(__global X_TYPE* x, __local float* tmp, __global float* y, __global float* dst, const int ncols) { \
        const int local_size = get_local_size(0); \
        const int row = get_group_id(0); \
        const int tid = get_local_id(0); \
        \
        const uint qk = QUANT_K; \
        const uint qr = QUANT_R; \
        \
        const int col_step = local_size * 2; \
        const int y_offset = qr == 1 ? 1 : qk/2; \
        \
        x += get_global_offset(0); \
        \
        tmp[tid] = 0; \
        \
        for (int col = tid*2; col < ncols; col += col_step) { \
            const int ib = (row*ncols + col)/qk; /* block index */ \
            const int iqs = (col%qk)/qr; /* quant index */ \
            const int iybs = col - col%qk; /* y block start index */ \
            \
            /* dequantize */ \
            float v0, v1; \
            DEQUANT_FUNC(x, ib, iqs, &v0, &v1); \
            \
            /* matrix multiplication */ \
            tmp[tid] += v0 * y[iybs + iqs + 0]; \
            tmp[tid] += v1 * y[iybs + iqs + y_offset]; \
        } \
        \
        /* sum up partial sums and write back result */  \
        barrier(CLK_LOCAL_MEM_FENCE); \
        for (int s=local_size/2; s>0; s>>=1) { \
            if (tid < s) { \
                tmp[tid] += tmp[tid + s]; \
            } \
            barrier(CLK_LOCAL_MEM_FENCE); \
        } \
        if (tid == 0) { \
            dst[row] = tmp[0]; \
        } \
    }

#define MUL(KERNEL_NAME, TYPE) \
    __kernel void KERNEL_NAME(__global TYPE* x, const int x_offset, __global TYPE* y, const int y_offset, __global TYPE* dst, const int dst_offset, const int ky) { \
        const int i = get_group_id(0)*get_local_size(0) + get_local_id(0); \
        \
        if (i >= get_global_size(0)) { \
            return; \
        } \
        \
        dst[dst_offset + i] = x[x_offset + i] * y[y_offset + i%ky]; \
    }

DEQUANT(dequantize_row_q4_0, struct block_q4_0, QK4_0, QR4_0, dequantize_q4_0)
DEQUANT(dequantize_row_q4_1, struct block_q4_1, QK4_1, QR4_1, dequantize_q4_1)
DEQUANT(dequantize_row_q5_0, struct block_q5_0, QK5_0, QR5_0, dequantize_q5_0)
DEQUANT(dequantize_row_q5_1, struct block_q5_1, QK5_1, QR5_1, dequantize_q5_1)
DEQUANT(dequantize_row_q8_0, struct block_q8_0, QK8_0, QR8_0, dequantize_q8_0)
DEQUANT(convert_row_f16, half_t, 1, 1, convert_f16)

DEQUANT_MUL_MAT_VEC(dequantize_mul_mat_vec_q4_0, struct block_q4_0, QK4_0, QR4_0, dequantize_q4_0)
DEQUANT_MUL_MAT_VEC(dequantize_mul_mat_vec_q4_1, struct block_q4_1, QK4_1, QR4_1, dequantize_q4_1)
DEQUANT_MUL_MAT_VEC(dequantize_mul_mat_vec_q5_0, struct block_q5_0, QK5_0, QR5_0, dequantize_q5_0)
DEQUANT_MUL_MAT_VEC(dequantize_mul_mat_vec_q5_1, struct block_q5_1, QK5_1, QR5_1, dequantize_q5_1)
DEQUANT_MUL_MAT_VEC(dequantize_mul_mat_vec_q8_0, struct block_q8_0, QK8_0, QR8_0, dequantize_q8_0)
DEQUANT_MUL_MAT_VEC(convert_mul_mat_vec_f16, half_t, 1, 1, convert_f16);

MUL(mul_f32, float)
