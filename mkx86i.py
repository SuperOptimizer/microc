#!/usr/bin/env python3
# mkx86i.py: writes src/x86i.c (the x86-64 intrinsic table: names, types, VEX encodings) and <immintrin.h>
# (with the other *intrin.h that include it) under libc/glibc/include and libc/linux/include. Run from the top.
import sys
out = []
E = []  # (name, ret, args, kind, form, enc, extra)

def enc(opc, mp=1, pp=1, W=0, L=1, digit=0):
    return opc | mp << 8 | pp << 10 | W << 12 | L << 13 | digit << 14

def add(name, ret, args, kind='XK_VEX', form='XF_RVM', e=0, extra=0):
    E.append((name, ret, args, kind, form, e, extra))

# ---- memory ----
for w, p, L in (('mm256', 'M256', 1), ('mm', 'M128', 0)):
    for suf, t in (('ps', ''), ('pd', 'D'), ('si%d' % (256 if L else 128), 'I')):
        T = p + t
        for al in ('loadu', 'load'):
            add('%s_%s_%s' % (w, al, suf), T, ['PTR'], 'XK_LOAD', 'XF_NONE')
        for al in ('storeu', 'store'):
            add('%s_%s_%s' % (w, al, suf), 'VOID', ['PTR', T], 'XK_STORE', 'XF_NONE')
add('mm_loadl_epi64', 'M128I', ['PTR'], 'XK_VLDM', 'XF_RM', enc(0x7e, 1, 2, 0, 0))
add('mm_loadu_si64', 'M128I', ['PTR'], 'XK_VLDM', 'XF_RM', enc(0x7e, 1, 2, 0, 0))
add('mm_loadu_si32', 'M128I', ['PTR'], 'XK_VLDM', 'XF_RM', enc(0x6e, 1, 1, 0, 0))
add('mm_storel_epi64', 'VOID', ['PTR', 'M128I'], 'XK_VSTM', 'XF_MR', enc(0xd6, 1, 1, 0, 0))
add('mm_storeu_si64', 'VOID', ['PTR', 'M128I'], 'XK_VSTM', 'XF_MR', enc(0xd6, 1, 1, 0, 0))
add('mm_storeu_si32', 'VOID', ['PTR', 'M128I'], 'XK_VSTM', 'XF_MR', enc(0x7e, 1, 1, 0, 0))
add('mm256_broadcast_ss', 'M256', ['PTR'], 'XK_VLDM', 'XF_RM', enc(0x18, 2, 1, 0, 1))
add('mm_broadcast_ss', 'M128', ['PTR'], 'XK_VLDM', 'XF_RM', enc(0x18, 2, 1, 0, 0))
add('mm256_broadcast_sd', 'M256D', ['PTR'], 'XK_VLDM', 'XF_RM', enc(0x19, 2, 1, 0, 1))
add('mm256_broadcastsi128_si256', 'M256I', ['M128I'], 'XK_BCAST128', 'XF_NONE')

# ---- constants, splats, element lists ----
for w, p in (('mm256', 'M256'), ('mm', 'M128')):
    add(w + '_setzero_ps', p, [], 'XK_ZERO', 'XF_NONE')
    add(w + '_setzero_pd', p + 'D', [], 'XK_ZERO', 'XF_NONE')
    add(w + '_setzero_si%d' % (256 if w == 'mm256' else 128), p + 'I', [], 'XK_ZERO', 'XF_NONE')
    add(w + '_undefined_ps', p, [], 'XK_ZERO', 'XF_NONE')
    add(w + '_undefined_si%d' % (256 if w == 'mm256' else 128), p + 'I', [], 'XK_ZERO', 'XF_NONE')
    for suf, t, lane in (('ps', 'F32', 'M_F32'), ('pd', 'F64', 'M_F64'), ('epi32', 'I32', 'M_32'), ('epi16', 'I16', 'M_16U'),
                         ('epi8', 'I8', 'M_8U'), ('epi64x', 'I64', 'M_64')):
        T = p + ('' if suf == 'ps' else 'D' if suf == 'pd' else 'I')
        add('%s_set1_%s' % (w, suf), T, [t], 'XK_SPLAT', 'XF_NONE', 0, lane)
    n = 8 if w == 'mm256' else 4
    for suf, t, cnt in (('ps', 'F32', n), ('pd', 'F64', n // 2), ('epi32', 'I32', n), ('epi16', 'I16', 2 * n), ('epi8', 'I8', 4 * n),
                        ('epi64x', 'I64', n // 2)):
        T = p + ('' if suf == 'ps' else 'D' if suf == 'pd' else 'I')
        add('%s_set_%s' % (w, suf), T, [t] * cnt, 'XK_SET', 'XF_NONE')
        add('%s_setr_%s' % (w, suf), T, [t] * cnt, 'XK_SETR', 'XF_NONE')
add('mm256_set_m128i', 'M256I', ['M128I', 'M128I'], 'XK_SET', 'XF_NONE')
add('mm256_setr_m128i', 'M256I', ['M128I', 'M128I'], 'XK_SETR', 'XF_NONE')
add('mm256_set_m128', 'M256', ['M128', 'M128'], 'XK_SET', 'XF_NONE')
add('mm256_setr_m128', 'M256', ['M128', 'M128'], 'XK_SETR', 'XF_NONE')

# ---- reinterpretations: the same register ----
for a, b in (('ps', 'M256'), ('pd', 'M256D'), ('si256', 'M256I')):
    for c, d in (('ps', 'M256'), ('pd', 'M256D'), ('si256', 'M256I')):
        if a != c:
            add('mm256_cast%s_%s' % (a, c), d, [b], 'XK_CAST', 'XF_NONE')
for a, b in (('ps', 'M128'), ('pd', 'M128D'), ('si128', 'M128I')):
    for c, d in (('ps', 'M128'), ('pd', 'M128D'), ('si128', 'M128I')):
        if a != c:
            add('mm_cast%s_%s' % (a, c), d, [b], 'XK_CAST', 'XF_NONE')
# the low part / a wider register whose upper part is zero (an undefined one is zero too)
for n, r, a in (('mm256_castsi256_si128', 'M128I', 'M256I'), ('mm256_castps256_ps128', 'M128', 'M256'),
                ('mm256_castpd256_pd128', 'M128D', 'M256D'), ('mm256_castsi128_si256', 'M256I', 'M128I'),
                ('mm256_castps128_ps256', 'M256', 'M128'), ('mm256_castpd128_pd256', 'M256D', 'M128D'),
                ('mm256_zextsi128_si256', 'M256I', 'M128I'), ('mm256_zextps128_ps256', 'M256', 'M128'),
                ('mm256_zextpd128_pd256', 'M256D', 'M128D'),
                ('mm_cvtss_f32', 'F32', 'M128'), ('mm256_cvtss_f32', 'F32', 'M256'), ('mm_cvtsd_f64', 'F64', 'M128D'),
                ('mm256_cvtsd_f64', 'F64', 'M256D')):
    add(n, r, [a], 'XK_LO', 'XF_NONE')

# ---- floating point ----
for w, L, p in (('mm256', 1, 'M256'), ('mm', 0, 'M128')):
    for suf, pp, T in (('ps', 0, p), ('pd', 1, p + 'D')):
        for nm, opc in (('add', 0x58), ('sub', 0x5c), ('mul', 0x59), ('div', 0x5e), ('min', 0x5d), ('max', 0x5f), ('and', 0x54),
                        ('andnot', 0x55), ('or', 0x56), ('xor', 0x57), ('unpacklo', 0x14), ('unpackhi', 0x15)):
            add('%s_%s_%s' % (w, nm, suf), T, [T, T], 'XK_VEX', 'XF_RVM', enc(opc, 1, pp, 0, L))
        add('%s_sqrt_%s' % (w, suf), T, [T], 'XK_VEX', 'XF_RM', enc(0x51, 1, pp, 0, L))
        add('%s_cmp_%s' % (w, suf), T, [T, T, 'IMM'], 'XK_VEX', 'XF_RVM', enc(0xc2, 1, pp, 0, L))
        add('%s_shuffle_%s' % (w, suf), T, [T, T, 'IMM'], 'XK_VEX', 'XF_RVM', enc(0xc6, 1, pp, 0, L))
        add('%s_blendv_%s' % (w, suf), T, [T, T, T], 'XK_VEX', 'XF_RVMR', enc(0x4a + pp, 3, 1, 0, L))
        add('%s_blend_%s' % (w, suf), T, [T, T, 'IMM'], 'XK_VEX', 'XF_RVM', enc(0x0c + pp, 3, 1, 0, L))
        add('%s_movemask_%s' % (w, suf), 'I32', [T], 'XK_VEX', 'XF_RM', enc(0x50, 1, pp, 0, L))
        add('%s_round_%s' % (w, suf), T, [T, 'IMM'], 'XK_VEX', 'XF_RM', enc(0x08 + pp, 3, 1, 0, L))
        add('%s_floor_%s' % (w, suf), T, [T], 'XK_VEX', 'XF_RM', enc(0x08 + pp, 3, 1, 0, L), 0x100 | 9)
        add('%s_ceil_%s' % (w, suf), T, [T], 'XK_VEX', 'XF_RM', enc(0x08 + pp, 3, 1, 0, L), 0x100 | 10)
        for nm, opc in (('fmadd', 0xa8), ('fmsub', 0xaa), ('fnmadd', 0xac), ('fnmsub', 0xae)):
            add('%s_%s_%s' % (w, nm, suf), T, [T, T, T], 'XK_VEX', 'XF_DVM', enc(opc, 2, 1, pp, L))
        add('%s_testz_%s' % (w, suf), 'I32', [T, T], 'XK_TESTZ', 'XF_RM', enc(0x0e + pp, 2, 1, 0, L))
        add('%s_testc_%s' % (w, suf), 'I32', [T, T], 'XK_TESTC', 'XF_RM', enc(0x0e + pp, 2, 1, 0, L))
    add(w + '_rcp_ps', p, [p], 'XK_VEX', 'XF_RM', enc(0x53, 1, 0, 0, L))
    add(w + '_rsqrt_ps', p, [p], 'XK_VEX', 'XF_RM', enc(0x52, 1, 0, 0, L))
    add(w + '_cvtepi32_ps', p, [p + 'I'], 'XK_VEX', 'XF_RM', enc(0x5b, 1, 0, 0, L))
    add(w + '_cvttps_epi32', p + 'I', [p], 'XK_VEX', 'XF_RM', enc(0x5b, 1, 2, 0, L))
    add(w + '_cvtps_epi32', p + 'I', [p], 'XK_VEX', 'XF_RM', enc(0x5b, 1, 1, 0, L))
    add(w + '_permute_ps', p, [p, 'IMM'], 'XK_VEX', 'XF_RM', enc(0x04, 3, 1, 0, L))
    add(w + '_permutevar_ps', p, [p, p + 'I'], 'XK_VEX', 'XF_RVM', enc(0x0c, 2, 1, 0, L))
    add(w + '_movehdup_ps', p, [p], 'XK_VEX', 'XF_RM', enc(0x16, 1, 2, 0, L))
    add(w + '_moveldup_ps', p, [p], 'XK_VEX', 'XF_RM', enc(0x12, 1, 2, 0, L))
    add(w + '_addsub_ps', p, [p, p], 'XK_VEX', 'XF_RVM', enc(0xd0, 1, 3, 0, L))
    add(w + '_hadd_ps', p, [p, p], 'XK_VEX', 'XF_RVM', enc(0x7c, 1, 3, 0, L))
    add(w + '_dp_ps', p, [p, p, 'IMM'], 'XK_VEX', 'XF_RVM', enc(0x40, 3, 1, 0, L))
add('mm256_cvtepi32_pd', 'M256D', ['M128I'], 'XK_VEX', 'XF_RM', enc(0xe6, 1, 2, 0, 1))
add('mm256_cvtps_pd', 'M256D', ['M128'], 'XK_VEX', 'XF_RM', enc(0x5a, 1, 0, 0, 1))
add('mm256_cvtpd_ps', 'M128', ['M256D'], 'XK_VEX', 'XF_RM', enc(0x5a, 1, 1, 0, 1))
add('mm256_cvttpd_epi32', 'M128I', ['M256D'], 'XK_VEX', 'XF_RM', enc(0xe6, 1, 1, 0, 1))
add('mm256_cvtpd_epi32', 'M128I', ['M256D'], 'XK_VEX', 'XF_RM', enc(0xe6, 1, 3, 0, 1))
add('mm256_permute2f128_ps', 'M256', ['M256', 'M256', 'IMM'], 'XK_VEX', 'XF_RVM', enc(0x06, 3, 1, 0, 1))
add('mm256_permute2f128_pd', 'M256D', ['M256D', 'M256D', 'IMM'], 'XK_VEX', 'XF_RVM', enc(0x06, 3, 1, 0, 1))
add('mm256_permute2f128_si256', 'M256I', ['M256I', 'M256I', 'IMM'], 'XK_VEX', 'XF_RVM', enc(0x06, 3, 1, 0, 1))
add('mm256_extractf128_ps', 'M128', ['M256', 'IMM'], 'XK_VEX', 'XF_MR', enc(0x19, 3, 1, 0, 1))
add('mm256_extractf128_pd', 'M128D', ['M256D', 'IMM'], 'XK_VEX', 'XF_MR', enc(0x19, 3, 1, 0, 1))
add('mm256_extractf128_si256', 'M128I', ['M256I', 'IMM'], 'XK_VEX', 'XF_MR', enc(0x19, 3, 1, 0, 1))
add('mm256_insertf128_ps', 'M256', ['M256', 'M128', 'IMM'], 'XK_VEX', 'XF_RVM', enc(0x18, 3, 1, 0, 1))
add('mm256_insertf128_pd', 'M256D', ['M256D', 'M128D', 'IMM'], 'XK_VEX', 'XF_RVM', enc(0x18, 3, 1, 0, 1))
add('mm256_insertf128_si256', 'M256I', ['M256I', 'M128I', 'IMM'], 'XK_VEX', 'XF_RVM', enc(0x18, 3, 1, 0, 1))
add('mm256_broadcastss_ps', 'M256', ['M128'], 'XK_VEX', 'XF_RM', enc(0x18, 2, 1, 0, 1))
add('mm_broadcastss_ps', 'M128', ['M128'], 'XK_VEX', 'XF_RM', enc(0x18, 2, 1, 0, 0))
add('mm256_broadcastsd_pd', 'M256D', ['M128D'], 'XK_VEX', 'XF_RM', enc(0x19, 2, 1, 0, 1))
add('mm256_permutevar8x32_ps', 'M256', ['M256', 'M256I'], 'XK_VEX', 'XF_RVMS', enc(0x16, 2, 1, 0, 1))
add('mm256_permute4x64_pd', 'M256D', ['M256D', 'IMM'], 'XK_VEX', 'XF_RM', enc(0x01, 3, 1, 1, 1))
# F16C
add('mm256_cvtps_ph', 'M128I', ['M256', 'IMM'], 'XK_VEX', 'XF_MR', enc(0x1d, 3, 1, 0, 1))
add('mm_cvtps_ph', 'M128I', ['M128', 'IMM'], 'XK_VEX', 'XF_MR', enc(0x1d, 3, 1, 0, 0))
add('mm256_cvtph_ps', 'M256', ['M128I'], 'XK_VEX', 'XF_RM', enc(0x13, 2, 1, 0, 1))
add('mm_cvtph_ps', 'M128', ['M128I'], 'XK_VEX', 'XF_RM', enc(0x13, 2, 1, 0, 0))
add('cvtsh_ss', 'F32', ['U16'], 'XK_CVTSH', 'XF_NONE', enc(0x13, 2, 1, 0, 0))
add('cvtss_sh', 'U16', ['F32', 'IMM'], 'XK_CVTSS_SH', 'XF_NONE', enc(0x1d, 3, 1, 0, 0))

# ---- integers ----
for w, L, T, si in (('mm256', 1, 'M256I', 'si256'), ('mm', 0, 'M128I', 'si128')):
    X = 'M128I'
    rvm = (('add_epi8', 0xfc), ('add_epi16', 0xfd), ('add_epi32', 0xfe), ('add_epi64', 0xd4), ('sub_epi8', 0xf8),
           ('sub_epi16', 0xf9), ('sub_epi32', 0xfa), ('sub_epi64', 0xfb), ('adds_epu8', 0xdc), ('adds_epu16', 0xdd),
           ('subs_epu8', 0xd8), ('subs_epu16', 0xd9), ('adds_epi8', 0xec), ('adds_epi16', 0xed), ('subs_epi8', 0xe8),
           ('subs_epi16', 0xe9), ('mullo_epi16', 0xd5), ('mulhi_epi16', 0xe5), ('mulhi_epu16', 0xe4), ('mul_epu32', 0xf4),
           ('madd_epi16', 0xf5), ('and_' + si, 0xdb), ('andnot_' + si, 0xdf), ('or_' + si, 0xeb), ('xor_' + si, 0xef),
           ('cmpeq_epi8', 0x74), ('cmpeq_epi16', 0x75), ('cmpeq_epi32', 0x76), ('cmpgt_epi8', 0x64), ('cmpgt_epi16', 0x65),
           ('cmpgt_epi32', 0x66), ('min_epu8', 0xda), ('max_epu8', 0xde), ('min_epi16', 0xea), ('max_epi16', 0xee),
           ('avg_epu8', 0xe0), ('avg_epu16', 0xe3), ('sad_epu8', 0xf6), ('packus_epi16', 0x67), ('packs_epi16', 0x63),
           ('packs_epi32', 0x6b), ('unpacklo_epi8', 0x60), ('unpacklo_epi16', 0x61), ('unpacklo_epi32', 0x62),
           ('unpacklo_epi64', 0x6c), ('unpackhi_epi8', 0x68), ('unpackhi_epi16', 0x69), ('unpackhi_epi32', 0x6a),
           ('unpackhi_epi64', 0x6d))
    for nm, opc in rvm:
        add('%s_%s' % (w, nm), T, [T, T], 'XK_VEX', 'XF_RVM', enc(opc, 1, 1, 0, L))
    rvm38 = (('mullo_epi32', 0x40), ('mul_epi32', 0x28), ('maddubs_epi16', 0x04), ('cmpeq_epi64', 0x29), ('cmpgt_epi64', 0x37),
             ('min_epi8', 0x38), ('max_epi8', 0x3c), ('min_epu16', 0x3a), ('max_epu16', 0x3e), ('min_epi32', 0x39),
             ('max_epi32', 0x3d), ('min_epu32', 0x3b), ('max_epu32', 0x3f), ('sign_epi8', 0x08), ('sign_epi16', 0x09),
             ('sign_epi32', 0x0a), ('packus_epi32', 0x2b), ('shuffle_epi8', 0x00), ('hadd_epi16', 0x01), ('hadd_epi32', 0x02),
             ('mulhrs_epi16', 0x0b))
    for nm, opc in rvm38:
        add('%s_%s' % (w, nm), T, [T, T], 'XK_VEX', 'XF_RVM', enc(opc, 2, 1, 0, L))
    for nm, opc in (('abs_epi8', 0x1c), ('abs_epi16', 0x1d), ('abs_epi32', 0x1e)):
        add('%s_%s' % (w, nm), T, [T], 'XK_VEX', 'XF_RM', enc(opc, 2, 1, 0, L))
    for nm, opc, d in (('slli_epi16', 0x71, 6), ('slli_epi32', 0x72, 6), ('slli_epi64', 0x73, 6), ('srli_epi16', 0x71, 2),
                       ('srli_epi32', 0x72, 2), ('srli_epi64', 0x73, 2), ('srai_epi16', 0x71, 4), ('srai_epi32', 0x72, 4),
                       ('srli_' + si, 0x73, 3), ('slli_' + si, 0x73, 7), ('bsrli_' + ('epi128' if L else si), 0x73, 3),
                       ('bslli_' + ('epi128' if L else si), 0x73, 7)):
        add('%s_%s' % (w, nm), T, [T, 'IMM'], 'XK_VEX', 'XF_VMI', enc(opc, 1, 1, 0, L, d))
    for nm, opc in (('sll_epi16', 0xf1), ('sll_epi32', 0xf2), ('sll_epi64', 0xf3), ('srl_epi16', 0xd1), ('srl_epi32', 0xd2),
                    ('srl_epi64', 0xd3), ('sra_epi16', 0xe1), ('sra_epi32', 0xe2)):
        add('%s_%s' % (w, nm), T, [T, X], 'XK_VEX', 'XF_RVM', enc(opc, 1, 1, 0, L))
    for nm, opc, W in (('sllv_epi32', 0x47, 0), ('srlv_epi32', 0x45, 0), ('srav_epi32', 0x46, 0), ('sllv_epi64', 0x47, 1),
                       ('srlv_epi64', 0x45, 1)):
        add('%s_%s' % (w, nm), T, [T, T], 'XK_VEX', 'XF_RVM', enc(opc, 2, 1, W, L))
    for nm, opc in (('cvtepu8_epi16', 0x30), ('cvtepu8_epi32', 0x31), ('cvtepu8_epi64', 0x32), ('cvtepu16_epi32', 0x33),
                    ('cvtepu16_epi64', 0x34), ('cvtepu32_epi64', 0x35), ('cvtepi8_epi16', 0x20), ('cvtepi8_epi32', 0x21),
                    ('cvtepi8_epi64', 0x22), ('cvtepi16_epi32', 0x23), ('cvtepi16_epi64', 0x24), ('cvtepi32_epi64', 0x25)):
        add('%s_%s' % (w, nm), T, [X], 'XK_VEX', 'XF_RM', enc(opc, 2, 1, 0, L))
    for nm, opc, pp in (('shuffle_epi32', 0x70, 1), ('shufflelo_epi16', 0x70, 3), ('shufflehi_epi16', 0x70, 2)):
        add('%s_%s' % (w, nm), T, [T, 'IMM'], 'XK_VEX', 'XF_RM', enc(opc, 1, pp, 0, L))
    add(w + '_alignr_epi8', T, [T, T, 'IMM'], 'XK_VEX', 'XF_RVM', enc(0x0f, 3, 1, 0, L))
    add(w + '_blend_epi16', T, [T, T, 'IMM'], 'XK_VEX', 'XF_RVM', enc(0x0e, 3, 1, 0, L))
    add(w + '_blend_epi32', T, [T, T, 'IMM'], 'XK_VEX', 'XF_RVM', enc(0x02, 3, 1, 0, L))
    add(w + '_blendv_epi8', T, [T, T, T], 'XK_VEX', 'XF_RVMR', enc(0x4c, 3, 1, 0, L))
    add(w + '_movemask_epi8', 'I32', [T], 'XK_VEX', 'XF_RM', enc(0xd7, 1, 1, 0, L))
    for nm, opc in (('broadcastb_epi8', 0x78), ('broadcastw_epi16', 0x79), ('broadcastd_epi32', 0x58), ('broadcastq_epi64', 0x59)):
        add('%s_%s' % (w, nm), T, [X], 'XK_VEX', 'XF_RM', enc(opc, 2, 1, 0, L))
    add('%s_testz_%s' % (w, si), 'I32', [T, T], 'XK_TESTZ', 'XF_RM', enc(0x17, 2, 1, 0, L))
    add('%s_testc_%s' % (w, si), 'I32', [T, T], 'XK_TESTC', 'XF_RM', enc(0x17, 2, 1, 0, L))
    # gathers: base, index, scale; with a mask: source, base, index, mask, scale
    for nm, opc, W, r, ix in (('i32gather_epi32', 0x90, 0, T, T), ('i32gather_ps', 0x92, 0, 'M256' if L else 'M128', T),
                              ('i32gather_epi64', 0x90, 1, T, X), ('i32gather_pd', 0x92, 1, 'M256D' if L else 'M128D', X)):
        add('%s_%s' % (w, nm), r, ['PTR', ix, 'IMM'], 'XK_GATHER', 'XF_NONE', enc(opc, 2, 1, W, L))
        add('%s_mask_%s' % (w, nm), r, [r, 'PTR', ix, r, 'IMM'], 'XK_MGATHER', 'XF_NONE', enc(opc, 2, 1, W, L))
add('mm256_extracti128_si256', 'M128I', ['M256I', 'IMM'], 'XK_VEX', 'XF_MR', enc(0x39, 3, 1, 0, 1))
add('mm256_inserti128_si256', 'M256I', ['M256I', 'M128I', 'IMM'], 'XK_VEX', 'XF_RVM', enc(0x38, 3, 1, 0, 1))
add('mm256_permutevar8x32_epi32', 'M256I', ['M256I', 'M256I'], 'XK_VEX', 'XF_RVMS', enc(0x36, 2, 1, 0, 1))
add('mm256_permute4x64_epi64', 'M256I', ['M256I', 'IMM'], 'XK_VEX', 'XF_RM', enc(0x00, 3, 1, 1, 1))
add('mm256_permute2x128_si256', 'M256I', ['M256I', 'M256I', 'IMM'], 'XK_VEX', 'XF_RVM', enc(0x46, 3, 1, 0, 1))
# between general and vector registers
add('mm_cvtsi128_si32', 'I32', ['M128I'], 'XK_VEX', 'XF_MR', enc(0x7e, 1, 1, 0, 0))
add('mm_cvtsi128_si64', 'I64', ['M128I'], 'XK_VEX', 'XF_MR', enc(0x7e, 1, 1, 1, 0))
add('mm_cvtsi128_si64x', 'I64', ['M128I'], 'XK_VEX', 'XF_MR', enc(0x7e, 1, 1, 1, 0))
add('mm_cvtsi32_si128', 'M128I', ['I32'], 'XK_VEX', 'XF_RM', enc(0x6e, 1, 1, 0, 0))
add('mm_cvtsi64_si128', 'M128I', ['I64'], 'XK_VEX', 'XF_RM', enc(0x6e, 1, 1, 1, 0))
add('mm_cvtsi64x_si128', 'M128I', ['I64'], 'XK_VEX', 'XF_RM', enc(0x6e, 1, 1, 1, 0))
add('mm256_cvtsi256_si32', 'I32', ['M256I'], 'XK_VEX', 'XF_MR', enc(0x7e, 1, 1, 0, 0))
add('mm_extract_epi8', 'I32', ['M128I', 'IMM'], 'XK_VEX', 'XF_MR', enc(0x14, 3, 1, 0, 0))
add('mm_extract_epi16', 'I32', ['M128I', 'IMM'], 'XK_VEX', 'XF_RM', enc(0xc5, 1, 1, 0, 0))
add('mm_extract_epi32', 'I32', ['M128I', 'IMM'], 'XK_VEX', 'XF_MR', enc(0x16, 3, 1, 0, 0))
add('mm_extract_epi64', 'I64', ['M128I', 'IMM'], 'XK_VEX', 'XF_MR', enc(0x16, 3, 1, 1, 0))
add('mm_extract_ps', 'I32', ['M128', 'IMM'], 'XK_VEX', 'XF_MR', enc(0x17, 3, 1, 0, 0))
add('mm_insert_epi8', 'M128I', ['M128I', 'I32', 'IMM'], 'XK_VEX', 'XF_RVM', enc(0x20, 3, 1, 0, 0))
add('mm_insert_epi16', 'M128I', ['M128I', 'I32', 'IMM'], 'XK_VEX', 'XF_RVM', enc(0xc4, 1, 1, 0, 0))
add('mm_insert_epi32', 'M128I', ['M128I', 'I32', 'IMM'], 'XK_VEX', 'XF_RVM', enc(0x22, 3, 1, 0, 0))
add('mm_insert_epi64', 'M128I', ['M128I', 'I64', 'IMM'], 'XK_VEX', 'XF_RVM', enc(0x22, 3, 1, 1, 0))
add('mm256_extract_epi32', 'I32', ['M256I', 'IMM'], 'XK_EXTRACT', 'XF_NONE', 0, 'M_32')
add('mm256_extract_epi64', 'I64', ['M256I', 'IMM'], 'XK_EXTRACT', 'XF_NONE', 0, 'M_64')
add('mm256_extract_epi16', 'I32', ['M256I', 'IMM'], 'XK_EXTRACT', 'XF_NONE', 0, 'M_16U')
add('mm256_extract_epi8', 'I32', ['M256I', 'IMM'], 'XK_EXTRACT', 'XF_NONE', 0, 'M_8U')
# BMI1 / BMI2 / LZCNT (general registers)
add('pdep_u64', 'U64', ['U64', 'U64'], 'XK_VEX', 'XF_RVM', enc(0xf5, 2, 3, 1, 0))
add('pdep_u32', 'U32', ['U32', 'U32'], 'XK_VEX', 'XF_RVM', enc(0xf5, 2, 3, 0, 0))
add('pext_u64', 'U64', ['U64', 'U64'], 'XK_VEX', 'XF_RVM', enc(0xf5, 2, 2, 1, 0))
add('pext_u32', 'U32', ['U32', 'U32'], 'XK_VEX', 'XF_RVM', enc(0xf5, 2, 2, 0, 0))
add('bzhi_u64', 'U64', ['U64', 'U32'], 'XK_VEX', 'XF_RVMS', enc(0xf5, 2, 0, 1, 0))
add('bzhi_u32', 'U32', ['U32', 'U32'], 'XK_VEX', 'XF_RVMS', enc(0xf5, 2, 0, 0, 0))
add('andn_u64', 'U64', ['U64', 'U64'], 'XK_VEX', 'XF_RVM', enc(0xf2, 2, 0, 1, 0))
add('andn_u32', 'U32', ['U32', 'U32'], 'XK_VEX', 'XF_RVM', enc(0xf2, 2, 0, 0, 0))
# processor identification for __builtin_cpu_supports: leaf, subleaf, register (0 eax 1 ebx 2 ecx 3 edx)
add('mcc_cpuid', 'U32', ['IMM', 'IMM', 'IMM'], 'XK_CPUID', 'XF_NONE')
add('mm_pause', 'VOID', [], 'XK_RAW', 'XF_NONE', 0xf390)
add('mm_sfence', 'VOID', [], 'XK_RAW', 'XF_NONE', 0x0faef8)
add('mm_lfence', 'VOID', [], 'XK_RAW', 'XF_NONE', 0x0faee8)
add('mm_mfence', 'VOID', [], 'XK_RAW', 'XF_NONE', 0x0faef0)
add('mm256_zeroupper', 'VOID', [], 'XK_RAW', 'XF_NONE', 0xc5f877)
add('mm_prefetch', 'VOID', ['PTR', 'IMM'], 'XK_PREFETCH', 'XF_NONE', enc(0x18, 1, 0, 0, 0))

# ---- AVX-512 (EVEX, zmm): what ufsm uses (volcomp_fast.h, fitjoin.c) and their neighbours ----
def enc512(opc, mp=1, pp=1, W=0):
    return opc | mp << 8 | pp << 10 | W << 12 | 1 << 17
for suf, t in (('ps', 'M512'), ('pd', 'M512D'), ('si512', 'M512I'), ('epi32', 'M512I'), ('epi64', 'M512I')):
    for al in ('loadu', 'load'):
        add('mm512_%s_%s' % (al, suf), t, ['PTR'], 'XK_LOAD', 'XF_NONE')
    for al in ('storeu', 'store'):
        add('mm512_%s_%s' % (al, suf), 'VOID', ['PTR', t], 'XK_STORE', 'XF_NONE')
for suf, t in (('ps', 'M512'), ('pd', 'M512D'), ('si512', 'M512I'), ('epi32', 'M512I')):
    add('mm512_setzero_' + suf, t, [], 'XK_ZERO', 'XF_NONE')
for suf, t, a, lane in (('ps', 'M512', 'F32', 'M_F32'), ('pd', 'M512D', 'F64', 'M_F64'), ('epi32', 'M512I', 'I32', 'M_32'),
                        ('epi64', 'M512I', 'I64', 'M_64'), ('epi16', 'M512I', 'I16', 'M_16U'), ('epi8', 'M512I', 'I8', 'M_8U')):
    add('mm512_set1_' + suf, t, [a], 'XK_SPLAT', 'XF_NONE', 0, lane)
for a, b in (('ps', 'M512'), ('pd', 'M512D'), ('si512', 'M512I')):
    for c, d in (('ps', 'M512'), ('pd', 'M512D'), ('si512', 'M512I')):
        if a != c:
            add('mm512_cast%s_%s' % (a, c), d, [b], 'XK_CAST', 'XF_NONE')
for nm, opc in (('add', 0x58), ('sub', 0x5c), ('mul', 0x59), ('div', 0x5e), ('max', 0x5f), ('min', 0x5d)):
    add('mm512_%s_ps' % nm, 'M512', ['M512', 'M512'], 'XK_VEX', 'XF_RVM', enc512(opc, 1, 0, 0))
    add('mm512_%s_pd' % nm, 'M512D', ['M512D', 'M512D'], 'XK_VEX', 'XF_RVM', enc512(opc, 1, 1, 1))
add('mm512_sqrt_ps', 'M512', ['M512'], 'XK_VEX', 'XF_RM', enc512(0x51, 1, 0, 0))
add('mm512_sqrt_pd', 'M512D', ['M512D'], 'XK_VEX', 'XF_RM', enc512(0x51, 1, 1, 1))
for nm, opc in (('fmadd', 0xa8), ('fmsub', 0xaa), ('fnmadd', 0xac), ('fnmsub', 0xae)):
    add('mm512_%s_ps' % nm, 'M512', ['M512', 'M512', 'M512'], 'XK_VEX', 'XF_DVM', enc512(opc, 2, 1, 0))
    add('mm512_%s_pd' % nm, 'M512D', ['M512D', 'M512D', 'M512D'], 'XK_VEX', 'XF_DVM', enc512(opc, 2, 1, 1))
for nm, opc, W in (('and_si512', 0xdb, 0), ('or_si512', 0xeb, 0), ('xor_si512', 0xef, 0), ('andnot_si512', 0xdf, 0),
                   ('and_epi32', 0xdb, 0), ('or_epi32', 0xeb, 0), ('xor_epi32', 0xef, 0), ('and_epi64', 0xdb, 1),
                   ('or_epi64', 0xeb, 1), ('xor_epi64', 0xef, 1), ('add_epi32', 0xfe, 0), ('sub_epi32', 0xfa, 0),
                   ('add_epi64', 0xd4, 1), ('sub_epi64', 0xfb, 1), ('add_epi16', 0xfd, 0), ('sub_epi16', 0xf9, 0),
                   ('add_epi8', 0xfc, 0), ('sub_epi8', 0xf8, 0), ('min_epu8', 0xda, 0), ('max_epu8', 0xde, 0)):
    add('mm512_' + nm, 'M512I', ['M512I', 'M512I'], 'XK_VEX', 'XF_RVM', enc512(opc, 1, 1, W))
add('mm512_mullo_epi32', 'M512I', ['M512I', 'M512I'], 'XK_VEX', 'XF_RVM', enc512(0x40, 2, 1, 0))
add('mm512_cvtepi32_ps', 'M512', ['M512I'], 'XK_VEX', 'XF_RM', enc512(0x5b, 1, 0, 0))
add('mm512_cvttps_epi32', 'M512I', ['M512'], 'XK_VEX', 'XF_RM', enc512(0x5b, 1, 2, 0))
# a mask: the k register's bits to a general register (vptestm[bwdq] k, a, b; kmov)
add('mm512_test_epi8_mask', 'U64', ['M512I', 'M512I'], 'XK_KTEST', 'XF_NONE', enc512(0x26, 2, 1, 0))
add('mm512_test_epi16_mask', 'U32', ['M512I', 'M512I'], 'XK_KTEST', 'XF_NONE', enc512(0x26, 2, 1, 1))
add('mm512_test_epi32_mask', 'U16', ['M512I', 'M512I'], 'XK_KTEST', 'XF_NONE', enc512(0x27, 2, 1, 0))
add('mm512_test_epi64_mask', 'U16', ['M512I', 'M512I'], 'XK_KTEST', 'XF_NONE', enc512(0x27, 2, 1, 1))

E.sort(key=lambda e: e[0])
names = [e[0] for e in E]
assert len(names) == len(set(names)), [n for n in names if names.count(n) > 1]
o = []
o.append('/* x86-64 intrinsics: the table behind <immintrin.h>. Generated by mkx86i.py (edit that); see x86i.h. */')
o.append('#include "ir.h"')
o.append('#include "x86i.h"')
o.append('#include <string.h>')
o.append('')
o.append('const X86I x86i_tab[] = {')
for (n, r, a, k, fm, e, x) in E:
    al = ', '.join('XT_' + t for t in a) if len(a) <= 5 else ', '.join('XT_' + t for t in a[:1])
    if not al:
        al = '0'
    o.append('    {"%s", XT_%s, %d, {%s}, %s, %s, 0x%xu, %s},' % (n, r, len(a), al, k, fm, e, x))
o.append('};')
o.append('const u32 x86i_count = sizeof x86i_tab / sizeof x86i_tab[0];')
o.append('')
o.append('''/* The entry named n (without its leading underscore), or -1. The table is sorted by name. */
int x86i_find(const char *n)
{
    int lo = 0, hi = (int)x86i_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(n, x86i_tab[mid].name);
        if (c == 0)
            return mid;
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return -1;
}''')
open('src/x86i.c', 'w').write('\n'.join(o) + '\n')
PRE = "/* mcc's <immintrin.h>: x86-64 SIMD intrinsics (SSE through AVX2, FMA, F16C, BMI2).\n   The vector types are GNU vectors; each intrinsic is a compiler builtin (src/x86i.c lists them) that becomes\n   the instruction itself in x86-64 host code. They are there whatever -m options say; the __AVX2__ and\n   other macros say what -mavx2, -march=... asked for. AVX-512: the few that ufsm uses\n   (512-bit vectors are registers only with -mavx512f or -march=x86-64-v4 / znver4 / native on such a machine). */\n#ifndef __MCC_IMMINTRIN_H\n#define __MCC_IMMINTRIN_H\n\ntypedef float __m512 __attribute__((__vector_size__(64)));\ntypedef double __m512d __attribute__((__vector_size__(64)));\ntypedef long long __m512i __attribute__((__vector_size__(64)));\ntypedef float __m512_u __attribute__((__vector_size__(64), __aligned__(1)));\ntypedef double __m512d_u __attribute__((__vector_size__(64), __aligned__(1)));\ntypedef long long __m512i_u __attribute__((__vector_size__(64), __aligned__(1)));\ntypedef unsigned char __mmask8;\ntypedef unsigned short __mmask16;\ntypedef unsigned int __mmask32;\ntypedef unsigned long long __mmask64;\ntypedef float __m256 __attribute__((__vector_size__(32)));\ntypedef double __m256d __attribute__((__vector_size__(32)));\ntypedef long long __m256i __attribute__((__vector_size__(32)));\ntypedef float __m128 __attribute__((__vector_size__(16)));\ntypedef double __m128d __attribute__((__vector_size__(16)));\ntypedef long long __m128i __attribute__((__vector_size__(16)));\ntypedef float __m256_u __attribute__((__vector_size__(32), __aligned__(1)));\ntypedef double __m256d_u __attribute__((__vector_size__(32), __aligned__(1)));\ntypedef long long __m256i_u __attribute__((__vector_size__(32), __aligned__(1)));\ntypedef float __m128_u __attribute__((__vector_size__(16), __aligned__(1)));\ntypedef double __m128d_u __attribute__((__vector_size__(16), __aligned__(1)));\ntypedef long long __m128i_u __attribute__((__vector_size__(16), __aligned__(1)));\n\n/* GCC's names for the lane types, which some code uses */\ntypedef float __v8sf __attribute__((__vector_size__(32)));\ntypedef double __v4df __attribute__((__vector_size__(32)));\ntypedef long long __v4di __attribute__((__vector_size__(32)));\ntypedef unsigned long long __v4du __attribute__((__vector_size__(32)));\ntypedef int __v8si __attribute__((__vector_size__(32)));\ntypedef unsigned int __v8su __attribute__((__vector_size__(32)));\ntypedef short __v16hi __attribute__((__vector_size__(32)));\ntypedef unsigned short __v16hu __attribute__((__vector_size__(32)));\ntypedef char __v32qi __attribute__((__vector_size__(32)));\ntypedef signed char __v32qs __attribute__((__vector_size__(32)));\ntypedef unsigned char __v32qu __attribute__((__vector_size__(32)));\ntypedef float __v4sf __attribute__((__vector_size__(16)));\ntypedef double __v2df __attribute__((__vector_size__(16)));\ntypedef long long __v2di __attribute__((__vector_size__(16)));\ntypedef unsigned long long __v2du __attribute__((__vector_size__(16)));\ntypedef int __v4si __attribute__((__vector_size__(16)));\ntypedef unsigned int __v4su __attribute__((__vector_size__(16)));\ntypedef short __v8hi __attribute__((__vector_size__(16)));\ntypedef unsigned short __v8hu __attribute__((__vector_size__(16)));\ntypedef char __v16qi __attribute__((__vector_size__(16)));\ntypedef signed char __v16qs __attribute__((__vector_size__(16)));\ntypedef unsigned char __v16qu __attribute__((__vector_size__(16)));\n\n#define _MM_SHUFFLE(z, y, x, w) (((z) << 6) | ((y) << 4) | ((x) << 2) | (w))\n#define _MM_SHUFFLE2(x, y) (((x) << 1) | (y))\n\n#define _MM_FROUND_TO_NEAREST_INT 0x00\n#define _MM_FROUND_TO_NEG_INF 0x01\n#define _MM_FROUND_TO_POS_INF 0x02\n#define _MM_FROUND_TO_ZERO 0x03\n#define _MM_FROUND_CUR_DIRECTION 0x04\n#define _MM_FROUND_RAISE_EXC 0x00\n#define _MM_FROUND_NO_EXC 0x08\n#define _MM_FROUND_NINT (_MM_FROUND_TO_NEAREST_INT | _MM_FROUND_RAISE_EXC)\n#define _MM_FROUND_FLOOR (_MM_FROUND_TO_NEG_INF | _MM_FROUND_RAISE_EXC)\n#define _MM_FROUND_CEIL (_MM_FROUND_TO_POS_INF | _MM_FROUND_RAISE_EXC)\n#define _MM_FROUND_TRUNC (_MM_FROUND_TO_ZERO | _MM_FROUND_RAISE_EXC)\n#define _MM_FROUND_RINT (_MM_FROUND_CUR_DIRECTION | _MM_FROUND_RAISE_EXC)\n#define _MM_FROUND_NEARBYINT (_MM_FROUND_CUR_DIRECTION | _MM_FROUND_NO_EXC)\n\n#define _CMP_EQ_OQ 0x00\n#define _CMP_LT_OS 0x01\n#define _CMP_LE_OS 0x02\n#define _CMP_UNORD_Q 0x03\n#define _CMP_NEQ_UQ 0x04\n#define _CMP_NLT_US 0x05\n#define _CMP_NLE_US 0x06\n#define _CMP_ORD_Q 0x07\n#define _CMP_EQ_UQ 0x08\n#define _CMP_NGE_US 0x09\n#define _CMP_NGT_US 0x0a\n#define _CMP_FALSE_OQ 0x0b\n#define _CMP_NEQ_OQ 0x0c\n#define _CMP_GE_OS 0x0d\n#define _CMP_GT_OS 0x0e\n#define _CMP_TRUE_UQ 0x0f\n#define _CMP_EQ_OS 0x10\n#define _CMP_LT_OQ 0x11\n#define _CMP_LE_OQ 0x12\n#define _CMP_UNORD_S 0x13\n#define _CMP_NEQ_US 0x14\n#define _CMP_NLT_UQ 0x15\n#define _CMP_NLE_UQ 0x16\n#define _CMP_ORD_S 0x17\n#define _CMP_EQ_US 0x18\n#define _CMP_NGE_UQ 0x19\n#define _CMP_NGT_UQ 0x1a\n#define _CMP_FALSE_OS 0x1b\n#define _CMP_NEQ_OS 0x1c\n#define _CMP_GE_OQ 0x1d\n#define _CMP_GT_OQ 0x1e\n#define _CMP_TRUE_US 0x1f\n\n#define _MM_HINT_T0 3\n#define _MM_HINT_T1 2\n#define _MM_HINT_T2 1\n#define _MM_HINT_NTA 0\n"
POST = '\n/* the SSE comparisons by name */\n#define _mm_cmpeq_ps(a, b) _mm_cmp_ps(a, b, _CMP_EQ_OQ)\n#define _mm_cmplt_ps(a, b) _mm_cmp_ps(a, b, _CMP_LT_OS)\n#define _mm_cmple_ps(a, b) _mm_cmp_ps(a, b, _CMP_LE_OS)\n#define _mm_cmpgt_ps(a, b) _mm_cmp_ps(a, b, _CMP_GT_OS)\n#define _mm_cmpge_ps(a, b) _mm_cmp_ps(a, b, _CMP_GE_OS)\n#define _mm_cmpneq_ps(a, b) _mm_cmp_ps(a, b, _CMP_NEQ_UQ)\n#define _mm_cmpeq_pd(a, b) _mm_cmp_pd(a, b, _CMP_EQ_OQ)\n#define _mm_cmplt_pd(a, b) _mm_cmp_pd(a, b, _CMP_LT_OS)\n#define _mm_cmple_pd(a, b) _mm_cmp_pd(a, b, _CMP_LE_OS)\n#define _mm_cmpgt_pd(a, b) _mm_cmp_pd(a, b, _CMP_GT_OS)\n#define _mm_cmpge_pd(a, b) _mm_cmp_pd(a, b, _CMP_GE_OS)\n#define _mm_cmpneq_pd(a, b) _mm_cmp_pd(a, b, _CMP_NEQ_UQ)\n#define _mm_cmplt_epi8(a, b) _mm_cmpgt_epi8(b, a)\n#define _mm_cmplt_epi16(a, b) _mm_cmpgt_epi16(b, a)\n#define _mm_cmplt_epi32(a, b) _mm_cmpgt_epi32(b, a)\n\n#endif\n'
h = PRE + '\n' + '\n'.join('#define _%s __builtin_x86_%s' % (n, n) for n in names if n != 'mcc_cpuid') + '\n' + POST
for d in ('libc/glibc/include', 'libc/linux/include'):
    open(d + '/immintrin.h', 'w').write(h)
    for f in ('x86intrin.h', 'emmintrin.h', 'xmmintrin.h', 'pmmintrin.h', 'tmmintrin.h', 'smmintrin.h', 'nmmintrin.h',
              'avxintrin.h', 'avx2intrin.h', 'f16cintrin.h', 'fmaintrin.h', 'bmi2intrin.h', 'bmiintrin.h', 'mmintrin.h'):
        open(d + '/' + f, 'w').write('/* mcc: everything is in <immintrin.h> */\n#include <immintrin.h>\n')
print('%d intrinsics' % len(E))
