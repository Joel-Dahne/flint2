/*
    Copyright (C) 2019 Daniel Schultz

    This file is part of FLINT.

    FLINT is free software: you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License (LGPL) as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.  See <http://www.gnu.org/licenses/>.
*/

#include "fmpz_mpoly.h"
#include "nmod_mpoly.h"
#include "thread_pool.h"
#include "fmpq.h"

/* instructions do A = B + I*(C - B) mod M */
typedef struct
{
    slong a_idx; /* index of A */
    slong b_idx; /* index of B */
    slong c_idx; /* index of C */
    fmpz_t idem;     /* I */
    fmpz_t modulus;  /* M */
} _fmpz_crt_prog_instr;

typedef struct
{
    _fmpz_crt_prog_instr * prog; /* straight line program */
    slong length; /* length of prog */
    slong alloc;  /* alloc of prog */
    slong localsize; /* length of outputs required in nmod_poly_crt_run */
    slong temp1loc; /* index of temporary used in run */
    slong temp2loc; /* index of another tempory used in run */
    int good;   /* the moduli are good for CRT, essentially relatively prime */
} fmpz_crt_struct;

typedef fmpz_crt_struct fmpz_crt_t[1];

/* general crt for fmpz_t - compile once, run many times ****************/

void fmpz_crt_init(fmpz_crt_t CRT);

void fmpz_crt_clear(fmpz_crt_t P);

int fmpz_crt_precompute_p(
    fmpz_crt_t CRT,
    const fmpz * const * moduli,
    slong len);

void _fmpz_crt_run_p(
    fmpz * outputs,
    const fmpz_crt_t CRT,
    const fmpz * const * inputs);

static slong _fmpz_crt_local_size(const fmpz_crt_t CRT)
{
    return CRT->localsize;
}

void fmpz_crt_init(fmpz_crt_t P)
{
    P->prog = NULL;
    P->alloc = 0;
    P->length = 0;
    P->localsize = 1;
    P->temp1loc = 0;
    P->temp2loc = 0;
    P->good = 0;
}

static void _fmpz_crt_fit_length(fmpz_crt_t P, slong k)
{
    k = FLINT_MAX(WORD(1), k);

    if (P->alloc == 0)
    {
        FLINT_ASSERT(P->prog == NULL);
        P->prog = (_fmpz_crt_prog_instr *) flint_malloc(k
                                           *sizeof(_fmpz_crt_prog_instr));
        P->alloc = k;
    }
    else if (k > P->alloc)
    {
        FLINT_ASSERT(P->prog != NULL);
        P->prog = (_fmpz_crt_prog_instr *) flint_realloc(P->prog, k
                                           *sizeof(_fmpz_crt_prog_instr));
        P->alloc = k;
    }
}

static void _fmpz_crt_set_length(fmpz_crt_t P, slong k)
{
    slong i;

    FLINT_ASSERT(k <= P->length);

    for (i = k; i < P->length; i++)
    {
        fmpz_clear(P->prog[i].modulus);
        fmpz_clear(P->prog[i].idem);
    }
    P->length = k;
}

void fmpz_crt_clear(fmpz_crt_t P)
{
    _fmpz_crt_set_length(P, 0);

    if (P->alloc > 0)
    {
        flint_free(P->prog);
    }
}

typedef struct {
    slong idx;
    mp_bitcnt_t degree;
} index_deg_pair;

/*
    combine all moduli in [start, stop)
    return index of instruction that computes the result
*/
static slong _push_prog(
    fmpz_crt_t P,
    const fmpz * const * moduli,
    const index_deg_pair * perm,
    slong ret_idx,
    slong start,
    slong stop)
{
    slong i, mid;
    slong b_idx, c_idx;
    mp_bitcnt_t lefttot, righttot;
    slong leftret, rightret;
    fmpz * leftmodulus, * rightmodulus;

    /* we should have at least 2 moduli */
    FLINT_ASSERT(start + 1 < stop);

    mid = start + (stop - start)/2;

    FLINT_ASSERT(start < mid);
    FLINT_ASSERT(mid < stop);

    lefttot = 0;
    for (i = start; i < mid; i++)
    {
        lefttot += perm[i].degree;
    }

    righttot = 0;
    for (i = mid; i < stop; i++)
    {
        righttot += perm[i].degree;
    }

    /* try to balance the total degree on left and right */
    while (lefttot < righttot
            && mid + 1 < stop
            && perm[mid].degree < righttot - lefttot)
    {
        lefttot += perm[mid].degree;
        righttot -= perm[mid].degree;
        mid++;
    }

    P->localsize = FLINT_MAX(P->localsize, 1 + ret_idx);

    /* compile left [start, mid) */
    if (start + 1 < mid)
    {
        b_idx = ret_idx + 1;
        leftret = _push_prog(P, moduli, perm, b_idx, start, mid);
        if (!P->good)
        {
            return -1;
        }
        leftmodulus = P->prog[leftret].modulus;
    }
    else
    {
        b_idx = -1 - perm[start].idx;
        leftmodulus = (fmpz *) moduli[perm[start].idx];
    }

    /* compile right [mid, end) */
    if (mid + 1 < stop)
    {
        c_idx = ret_idx + 2;
        rightret = _push_prog(P, moduli, perm, c_idx, mid, stop);
        if (!P->good)
        {
            return -1;
        }
        rightmodulus = P->prog[rightret].modulus;
    }
    else
    {
        c_idx = -1 - perm[mid].idx;
        rightmodulus = (fmpz *) moduli[perm[mid].idx];
    }

    /* check if fmpz_invmod is going to throw */
    if (fmpz_is_zero(leftmodulus) || fmpz_is_zero(rightmodulus))
    {
        P->good = 0;
        return -1;
    }

    /* compile [start, end) */
    i = P->length;
    _fmpz_crt_fit_length(P, i + 1);
    fmpz_init(P->prog[i].modulus);
    fmpz_init(P->prog[i].idem);
    P->good = P->good && fmpz_invmod(P->prog[i].modulus, leftmodulus, rightmodulus);
    fmpz_mul(P->prog[i].idem, leftmodulus, P->prog[i].modulus);
    fmpz_mul(P->prog[i].modulus, leftmodulus, rightmodulus);
    P->prog[i].a_idx = ret_idx;
    P->prog[i].b_idx = b_idx;
    P->prog[i].c_idx = c_idx;
    P->length = i + 1;

    return i;
}

static int index_deg_pair_cmp(
    const index_deg_pair * lhs,
    const index_deg_pair * rhs)
{
    return (lhs->degree < rhs->degree) ? -1 : (lhs->degree > rhs->degree);
}

/*
    Return 1 if moduli can be CRT'ed, 0 otherwise.
    A return of 0 means that future calls to run will leave output undefined.
*/
int fmpz_crt_precompute_p(
    fmpz_crt_t P,
    const fmpz * const * moduli,
    slong len)
{
    slong i;
    index_deg_pair * perm;
    TMP_INIT;

    FLINT_ASSERT(len > 0);

    TMP_START;
    perm = (index_deg_pair *) TMP_ALLOC(len * sizeof(index_deg_pair));

    for (i = 0; i < len; i++)
    {
        perm[i].idx = i;
        perm[i].degree = fmpz_bits(moduli[i]);
    }

    /* make perm sort the degs so that degs[perm[i-1]] <= degs[perm[i-0]] */
    qsort(perm, len, sizeof(index_deg_pair),
                        (int(*)(const void*, const void*)) index_deg_pair_cmp);
    for (i = 0; i < len; i++)
    {
        FLINT_ASSERT(perm[i].degree == fmpz_bits(moduli[perm[i].idx]));
        FLINT_ASSERT(i == 0 || perm[i - 1].degree <= perm[i].degree);
    }

    _fmpz_crt_fit_length(P, FLINT_MAX(WORD(1), len - 1));
    _fmpz_crt_set_length(P, 0);
    P->localsize = 1;
    P->good = 1;

    if (1 < len)
    {
        _push_prog(P, moduli, perm, 0, 0, len);
    }
    else
    {
        /*
            There is only one modulus. Lets compute as
                output[0] = input[0] + 0*(input[0] - input[0]) mod moduli[0]
        */
        i = 0;
        fmpz_init(P->prog[i].modulus);
        fmpz_init(P->prog[i].idem);
        fmpz_set(P->prog[i].modulus, moduli[0]);
        P->prog[i].a_idx = 0;
        P->prog[i].b_idx = -WORD(1);
        P->prog[i].c_idx = -WORD(1);
        P->length = i + 1;

        P->good = !fmpz_is_zero(moduli[0]);
    }

    if (!P->good)
    {
        _fmpz_crt_set_length(P, 0);
    }

    /* two more spots for temporaries */
    P->temp1loc = P->localsize++;
    P->temp2loc = P->localsize++;

    TMP_END;

    return P->good;
}

/*
    If P was set with a call to fmpz_poly_crt_compile(P, m, len), return
    in outputs[0] signed integer r of smallest abs value such that
        r = inputs[0] mod m[0]
        r = inputs[1] mod m[1]
            ...
        r = inputs[len-1] mod m[len-1]
    For thread safety "outputs" is expected to have enough space for all
    temporaries, thus should be at least as long as P->localsize.
*/

void _fmpz_crt_run_p(
    fmpz * outputs,
    const fmpz_crt_t P,
    const fmpz * const * inputs)
{
    slong i;
    slong a, b, c;
    const fmpz * B, * C;
    fmpz * A, * t1, * t2;

    t1 = outputs + P->temp1loc;
    t2 = outputs + P->temp2loc;

    for (i = 0; i < P->length; i++)
    {
        a = P->prog[i].a_idx;
        b = P->prog[i].b_idx;
        c = P->prog[i].c_idx;
        FLINT_ASSERT(a >= 0);
        A = outputs + a;
        B = b < 0 ? inputs[-b-1] : outputs + b;
        C = c < 0 ? inputs[-c-1] : outputs + c;

        /* A = B + I*(C - B) mod M */
        fmpz_sub(t1, B, C);
        fmpz_mul(t2, P->prog[i].idem, t1);
        fmpz_sub(t1, B, t2);
        fmpz_mods(A, t1, P->prog[i].modulus);

        /* last calculation should write answer to outputs[0] */
        if (i + 1 >= P->length)
        {
            FLINT_ASSERT(A == outputs + 0);
        }
    }
}


typedef struct
{
    volatile int gcd_is_one;
    volatile mp_limb_t p;
    pthread_mutex_t mutex;
    fmpz_t gamma;
    const fmpz_mpoly_ctx_struct * ctx;
    fmpz_mpolyu_struct * A, * B;
    ulong num_threads;
    slong var;
}
_splitbase_struct;

typedef _splitbase_struct _splitbase_t[1];

typedef struct
{
    slong idx;
    _splitbase_struct * base;
    fmpz_mpolyu_t G, Abar, Bbar;
    fmpz_t modulus;
    slong image_count;
    slong required_images;
    thread_pool_handle master_handle;
    slong num_workers;
    thread_pool_handle * worker_handles;
}
_splitworker_arg_struct;

static void _splitworker(void * varg)
{
    _splitworker_arg_struct * arg = (_splitworker_arg_struct *) varg;
    _splitbase_struct * base = arg->base;
    const fmpz_mpoly_ctx_struct * ctx = base->ctx;
    mp_bitcnt_t bits = base->A->bits;
    slong N = mpoly_words_per_exp_sp(bits, ctx->minfo);
    slong offset, shift;
    nmod_mpolyun_t Ap, Bp, Gp, Abarp, Bbarp;
    fmpz_mpolyu_t T;
    int success;
    mp_limb_t p, gammared;
    nmod_mpoly_ctx_t pctx;

    mpoly_gen_offset_shift_sp(&offset, &shift, ctx->minfo->nvars - 1, bits, ctx->minfo);

    fmpz_mpolyu_init(T, bits, ctx);

    fmpz_one(arg->modulus);
    arg->image_count = 0;

    nmod_mpoly_ctx_init(pctx, ctx->minfo->nvars, ORD_LEX, 2);
    nmod_mpolyun_init(Ap, bits, pctx);
    nmod_mpolyun_init(Bp, bits, pctx);
    nmod_mpolyun_init(Gp, bits, pctx);
    nmod_mpolyun_init(Abarp, bits, pctx);
    nmod_mpolyun_init(Bbarp, bits, pctx);

    while (arg->image_count < arg->required_images)
    {
        /* get prime */
        pthread_mutex_lock(&base->mutex);
        p = base->p;
        if (p >= UWORD_MAX_PRIME)
        {
            pthread_mutex_unlock(&base->mutex);
            break;
        }
        p = n_nextprime(base->p, 1);
        base->p = p;
        pthread_mutex_unlock(&base->mutex);

        /* make sure reduction does not kill both lc(A) and lc(B) */
        gammared = fmpz_fdiv_ui(base->gamma, p);
        if (gammared == 0)
        {
            continue;
        }

        nmod_mpoly_ctx_set_modulus(pctx, p);

        /* the unfortunate nmod poly's store their own context :( */
        nmod_mpolyun_set_mod(Ap, pctx->ffinfo->mod);
        nmod_mpolyun_set_mod(Bp, pctx->ffinfo->mod);
        nmod_mpolyun_set_mod(Gp, pctx->ffinfo->mod);
        nmod_mpolyun_set_mod(Abarp, pctx->ffinfo->mod);
        nmod_mpolyun_set_mod(Bbarp, pctx->ffinfo->mod);

        /* reduction should kill neither A nor B */
        fmpz_mpolyu_intp_reduce_p_mpolyun(Ap, pctx, base->A, ctx);
        fmpz_mpolyu_intp_reduce_p_mpolyun(Bp, pctx, base->B, ctx);
        FLINT_ASSERT(Ap->length > 0);
        FLINT_ASSERT(Bp->length > 0);

        if (arg->num_workers == 0)
        {
            success = nmod_mpolyun_gcd_brown_smprime(Gp, Abarp, Bbarp,
                                           Ap, Bp, ctx->minfo->nvars - 1, pctx);
        }
        else
        {
            success = nmod_mpolyun_gcd_brown_smprime_threaded(Gp, Abarp, Bbarp,
                                           Ap, Bp, ctx->minfo->nvars - 1, pctx,
                                        arg->worker_handles, arg->num_workers);
        }
        if (!success)
        {
            continue;
        }

        FLINT_ASSERT(Gp->length > 0);
        FLINT_ASSERT(Abarp->length > 0);
        FLINT_ASSERT(Bbarp->length > 0);

        /* check up */
        if (base->gcd_is_one)
        {
            break;
        }

        if (nmod_mpolyun_is_nonzero_nmod(Gp, pctx))
        {
            base->gcd_is_one = 1;
            break;
        }

        if (!fmpz_is_one(arg->modulus))
        {
            int cmp = 0;
            FLINT_ASSERT(arg->G->length > 0);
            if (arg->G->exps[0] != Gp->exps[0])
            {
                cmp = arg->G->exps[0] > Gp->exps[0] ? 1 : -1;
            }
            if (cmp == 0)
            {
                slong k = nmod_poly_degree((Gp->coeffs + 0)->coeffs + 0);
                cmp = mpoly_monomial_cmp_nomask_extra(
                       (arg->G->coeffs + 0)->exps + N*0,
                           (Gp->coeffs + 0)->exps + N*0, N, offset, k << shift);
            }

            if (cmp < 0)
            {
                continue;
            }
            else if (cmp > 0)
            {
                fmpz_one(arg->modulus);
                arg->image_count = 0;
            }
        }

        FLINT_ASSERT(1 == nmod_mpolyun_leadcoeff(Gp, pctx));
        nmod_mpolyun_scalar_mul_nmod(Gp, gammared, pctx);

        if (!fmpz_is_one(arg->modulus))
        {
            fmpz_mpolyu_intp_crt_p_mpolyun(arg->G, T, ctx, arg->modulus, Gp, pctx);
            fmpz_mpolyu_intp_crt_p_mpolyun(arg->Abar, T, ctx, arg->modulus, Abarp, pctx);
            fmpz_mpolyu_intp_crt_p_mpolyun(arg->Bbar, T, ctx, arg->modulus, Bbarp, pctx);
        }
        else
        {
            fmpz_mpolyu_intp_lift_p_mpolyun(arg->G, ctx, Gp, pctx);
            fmpz_mpolyu_intp_lift_p_mpolyun(arg->Abar, ctx, Abarp, pctx);
            fmpz_mpolyu_intp_lift_p_mpolyun(arg->Bbar, ctx, Bbarp, pctx);
        }

        fmpz_mul_ui(arg->modulus, arg->modulus, p);
        arg->image_count++;
    }

    fmpz_mpolyu_clear(T, ctx);

    nmod_mpolyun_clear(Ap, pctx);
    nmod_mpolyun_clear(Bp, pctx);
    nmod_mpolyun_clear(Gp, pctx);
    nmod_mpolyun_clear(Abarp, pctx);
    nmod_mpolyun_clear(Bbarp, pctx);

    nmod_mpoly_ctx_clear(pctx);
}


/* A = crt(B[0], ...., B[count-1]) wrt to P */
void fmpz_mpoly_crt(
    const fmpz_crt_t P,
    fmpz_t Amax,
    fmpz_t Asum,
    fmpz_mpoly_t A,
    fmpz_mpoly_struct * const * B,
    slong count,
    const fmpz_mpoly_ctx_t ctx)
{
    int cmp;
    slong N = mpoly_words_per_exp_sp(A->bits, ctx->minfo);
    fmpz ** input, * output;
    slong * start;
    slong Ai;
    slong j, k;
    fmpz_t zero;
    TMP_INIT;

    TMP_START;

    fmpz_init(zero);

    input = (fmpz **) TMP_ALLOC(count * sizeof(fmpz *));
    start = (slong *) TMP_ALLOC(count * sizeof(slong));
    output = (fmpz *) TMP_ALLOC(_fmpz_crt_local_size(P) * sizeof(fmpz));
    for (k = 0; k < _fmpz_crt_local_size(P); k++)
    {
        fmpz_init(output + k);
    }

    /* start[k] is the next available term in B[k] */
    for (k = 0; k < count; k++)
    {
        start[k] = 0;
    }

    Ai = 0;
    while (1)
    {
        fmpz_mpoly_fit_length(A, Ai + 1, ctx);

        k = 0;
        do
        {
            input[k] = zero;
            if (start[k] < B[k]->length)
            {
                goto found_max;
            }
        } while (++k < count);

        break; /* all B[k] have been scanned completely */

    found_max:

        input[k] = B[k]->coeffs + start[k];
        mpoly_monomial_set(A->exps + N*Ai, B[k]->exps + N*start[k], N);
        start[k]++;

        for (k++; k < count; k++)
        {
            input[k] = zero;
            if (start[k] >= B[k]->length)
            {
                continue;
            }

            cmp = mpoly_monomial_cmp_nomask(B[k]->exps + N*start[k], A->exps + N*Ai, N);
            if (cmp == 0)
            {
                input[k] = B[k]->coeffs + start[k];
                start[k]++;
            }
            else if (cmp > 0)
            {
                /* undo previous max's */
                for (j = 0; j < k; j++)
                {
                    start[j] -= (input[j] != zero);
                    input[j] = zero;
                }
                goto found_max;
            }
        }

        _fmpz_crt_run_p(output, P, (const fmpz * const *) input);
        fmpz_swap(output + 0, A->coeffs + Ai);

        if (fmpz_sgn(A->coeffs + Ai) > 0)
        {
            fmpz_add(Asum, Asum, A->coeffs + Ai);
        }
        else
        {
            fmpz_sub(Asum, Asum, A->coeffs + Ai);
        }

        if (fmpz_cmpabs(Amax, A->coeffs + Ai) < 0)
        {
            fmpz_set(Amax, A->coeffs + Ai);
            fmpz_abs(Amax, Amax);
        }

        Ai += !fmpz_is_zero(A->coeffs + Ai);

    }
    A->length = Ai;

    for (k = 0; k < _fmpz_crt_local_size(P); k++)
    {
        fmpz_clear(output + k);
    }

    TMP_END;

    fmpz_clear(zero);
    return;
}

/*
    Append to A the result of crt'ing the coeff of X^exp
    Amax = max(Amax, abs(coeff0), abs(coeff1), ...)
    Asum = Asum + abs(coeffs0) + abs(coeffs1) + ...
*/
void fmpz_mpolyu_crt_exp(
    const fmpz_crt_t P,
    fmpz_t Amax, fmpz_t Asum,
    fmpz_mpolyu_t A,
    ulong exp,
    fmpz_mpolyu_struct * const * B,
    slong count,
    const fmpz_mpoly_ctx_t ctx)
{
    slong j, k;
    slong Ai;
    fmpz_mpoly_struct ** C;
    fmpz_mpoly_t zero;
    TMP_INIT;

    fmpz_mpoly_init(zero, ctx);
    zero->length = 0;

    TMP_START;
    C = (fmpz_mpoly_struct **) TMP_ALLOC(count * sizeof(fmpz_mpoly_struct *));
    for (k = 0; k < count; k++)
    {
        C[k] = zero;
        for (j = 0; j < B[k]->length; j++)
        {
            if (B[k]->exps[j] == exp)
            {
                C[k] = B[k]->coeffs + j;
                break;
            }
        }
    }

    Ai = A->length;
    fmpz_mpolyu_fit_length(A, Ai + 1, ctx);
    A->exps[Ai] = exp;
    fmpz_mpoly_crt(P, Amax, Asum, A->coeffs + Ai, C, count, ctx);
    A->length += (A->coeffs + Ai)->length != 0;

    TMP_END;
    fmpz_mpoly_clear(zero, ctx);
    return;
}



typedef struct
{
    volatile int idx;
    volatile slong G_exp, Abar_exp, Bbar_exp;
    pthread_mutex_t mutex;
    const fmpz_mpoly_ctx_struct * ctx;
    fmpz_crt_struct * CRT;
    fmpz_mpolyu_struct ** gptrs, ** abarptrs, ** bbarptrs;
    slong num_images;
}
_joinbase_struct;

typedef _joinbase_struct _joinbase_t[1];

typedef struct
{
    _joinbase_struct * base;
    fmpz_mpolyu_t G, Abar, Bbar;
    fmpz_t Gmax, Gsum, Abarmax, Abarsum, Bbarmax, Bbarsum;
}
_joinworker_arg_struct;

static void _joinworker(void * varg)
{
    _joinworker_arg_struct * arg = (_joinworker_arg_struct *) varg;
    _joinbase_struct * base = arg->base;
    slong our_G_exp, our_Abar_exp, our_Bbar_exp;

    while (1)
    {
        /* get exponent of either G, Abar, or Bbar to start working on */
        pthread_mutex_lock(&base->mutex);
        our_G_exp = base->G_exp;
        our_Abar_exp = base->Abar_exp;
        our_Bbar_exp = base->Bbar_exp;
        if (our_G_exp >= 0)
        {
            base->G_exp = our_G_exp - 1;
        }
        else if (our_Abar_exp >= 0)
        {
            base->Abar_exp = our_Abar_exp - 1;
        }
        else if (our_Bbar_exp >= 0)
        {
            base->Bbar_exp = our_Bbar_exp - 1;
        }
        pthread_mutex_unlock(&base->mutex);

        if (our_G_exp >= 0)
        {
            fmpz_mpolyu_crt_exp(base->CRT, arg->Gmax, arg->Gsum,
                                          arg->G, our_G_exp, base->gptrs,
                                                  base->num_images, base->ctx);
        }
        else if (our_Abar_exp >= 0)
        {
            fmpz_mpolyu_crt_exp(base->CRT, arg->Abarmax, arg->Abarsum,
                                     arg->Abar, our_Abar_exp, base->abarptrs,
                                                  base->num_images, base->ctx);
        }
        else if (our_Bbar_exp >= 0)
        {
            fmpz_mpolyu_crt_exp(base->CRT, arg->Bbarmax, arg->Bbarsum,
                                     arg->Bbar, our_Bbar_exp, base->bbarptrs,
                                                  base->num_images, base->ctx);
        }
        else
        {
            return;
        }
    }
}


/*
    A = B[0] + ... + B[num_threads - 1]
    A and the B[i] are in ZZ[X][x_0, ..., x_(var-1)][var]
    The B[i] have distinct exponents on X, so this is just a top level merge.
    The inputs B[i] are clobbered.
*/
static void _final_join(
    fmpz_mpolyu_t A,
    fmpz_mpolyu_struct ** B,
    slong num_threads,
    const fmpz_mpoly_ctx_t ctx)
{
    slong i, Ai, total_length;
    slong * starts;
    TMP_INIT;

    TMP_START;
    starts = (slong *) TMP_ALLOC(num_threads*sizeof(slong));
    total_length = 0;
    for (i = 0; i < num_threads; i++)
    {
        starts[i] = 0;
        total_length += B[i]->length;
    }

    fmpz_mpolyu_fit_length(A, total_length, ctx);
    Ai = 0;
    while (1)
    {
        slong max_pos = -WORD(1);
        slong max_exp = -WORD(1);
        for (i = 0; i < num_threads; i++)
        {
            if (starts[i] < B[i]->length
                          && (slong)(B[i]->exps[starts[i]]) > max_exp)
            {
                max_pos = i;
                max_exp = B[i]->exps[starts[i]];
            }
        }
        if (max_pos < 0)
        {
            break;
        }
        A->exps[Ai] = max_exp;
        fmpz_mpoly_swap(A->coeffs + Ai, B[max_pos]->coeffs + starts[max_pos], ctx);
        starts[max_pos]++;
        Ai++;
    }
    A->length = Ai;
    FLINT_ASSERT(Ai == total_length);
    FLINT_ASSERT(fmpz_mpolyu_is_canonical(A, ctx));

    TMP_END;
}

/*
    Set 1 <= l <= min(n, m) and fractions v + 0, ..., v + l - 1
    More comments in useage

    example operation on input n = 10, m = 16

    gcd is 2:
        5/8, 5/8
    split first 5/8:
        2/3, 5/8, 3/5,
    split first 5/8:
        2/3, 2/3, 3/5, 3/5,
    split first 3/5:
        2/3, 2/3, 2/3, 3/5, 1/2
    split first 3/5:
        2/3, 2/3, 2/3, 2/3, 1/2, 1/2

    The maximum fraction is now 0.666 which is not much bigger than n/m
*/
static void _divide_master_threads(slong * l_, fmpq * v, slong n, slong m)
{
    slong l, i;
    double score_threashold;
    fmpq_t left, right;

    FLINT_ASSERT(n > 0);
    FLINT_ASSERT(m > 0);

    fmpq_init(left);
    fmpq_init(right);

    score_threashold = (double)(n)/(double)(m);
    score_threashold *= 1.1;

    /* initial choice for v */
    l = n_gcd(n, m);
    for (i = 0; i < l; i++)
    {
        fmpq_set_si(v + i, n, m);
    }

    i = 0;
    while (i < l)
    {
        if (   fmpz_cmp_ui(fmpq_denref(v + i), 2) >= 0
            && fmpq_farey_neighbors(left, right, v + i)
            && fmpq_get_d(right) < score_threashold)
        {
            /* delete v + i, add left and right */
            FLINT_ASSERT(l < m);
            fmpq_set(v + i, right);
            fmpq_set(v + l, left);
            l++;
            continue;
        }
        i++;
    }

    fmpq_clear(left);
    fmpq_clear(right);

    *l_ = l;
}

/* inputs A and B are modified */
int fmpz_mpolyu_gcd_brown_threaded(
    fmpz_mpolyu_t G,
    fmpz_mpolyu_t Abar,
    fmpz_mpolyu_t Bbar,
    fmpz_mpolyu_t A,
    fmpz_mpolyu_t B,
    const fmpz_mpoly_ctx_t ctx,
    const thread_pool_handle * handles,
    slong num_workers)
{
    slong i, j, k;
    mp_bitcnt_t bits = A->bits;
    slong N = mpoly_words_per_exp_sp(bits, ctx->minfo);
    slong num_threads = num_workers + 1;
    slong num_master_threads;
    slong num_images;
    int success;
    fmpz_t bound, modulus, temp;
    fmpz_t gnm, gns, anm, ans, bnm, bns;
    fmpz_t cA, cB, cG, cAbar, cBbar;
    fmpz ** mptrs;
    fmpz_mpolyu_struct ** gptrs, ** abarptrs, ** bbarptrs;
    fmpz_crt_t P;
    fmpq * qvec;
    _splitworker_arg_struct * splitargs;
    _splitbase_t splitbase;
    _joinworker_arg_struct * joinargs;
    _joinbase_t joinbase;

    fmpz_init(gnm);
    fmpz_init(gns);
    fmpz_init(anm);
    fmpz_init(ans);
    fmpz_init(bnm);
    fmpz_init(bns);
    fmpz_init(bound);
    fmpz_init(temp);
    fmpz_init(modulus);

    /* compute contents of G, Abar, Bbar, A, B */
    fmpz_init(cA);
    fmpz_init(cB);
    fmpz_init(cG);
    fmpz_init(cAbar);
    fmpz_init(cBbar);
    fmpz_mpolyu_content_fmpz(cA, A, ctx);
    fmpz_mpolyu_content_fmpz(cB, B, ctx);
    fmpz_gcd(cG, cA, cB);
    fmpz_divexact(cAbar, cA, cG);
    fmpz_divexact(cBbar, cB, cG);

    /* remove content from inputs */
    fmpz_mpolyu_divexact_fmpz(A, A, cA, ctx);
    fmpz_mpolyu_divexact_fmpz(B, B, cB, ctx);

    /* init split info */
    qvec = _fmpq_vec_init(num_threads);

    fmpz_init(splitbase->gamma);
    fmpz_gcd(splitbase->gamma, fmpz_mpolyu_leadcoeff(A),
                               fmpz_mpolyu_leadcoeff(B));

    /*
        if compute_split is jumped back to, there could be as many as
        num_threads + 1 images that need to be joined.
    */
    gptrs = (fmpz_mpolyu_struct **) flint_malloc(
                               (num_threads + 1)*sizeof(fmpz_mpolyu_struct *));
    abarptrs = (fmpz_mpolyu_struct **) flint_malloc(
                               (num_threads + 1)*sizeof(fmpz_mpolyu_struct *));
    bbarptrs = (fmpz_mpolyu_struct **) flint_malloc(
                               (num_threads + 1)*sizeof(fmpz_mpolyu_struct *));
    mptrs = (fmpz **) flint_malloc((num_threads + 1)*sizeof(fmpz *));

    splitargs = (_splitworker_arg_struct *) flint_malloc(
                                  num_threads*sizeof(_splitworker_arg_struct));
    for (i = 0; i < num_threads; i++)
    {
        fmpz_mpolyu_init(splitargs[i].G, bits, ctx);
        fmpz_mpolyu_init(splitargs[i].Abar, bits, ctx);
        fmpz_mpolyu_init(splitargs[i].Bbar, bits, ctx);
        fmpz_init(splitargs[i].modulus);
        splitargs[i].worker_handles = (thread_pool_handle *) flint_malloc(
                                       num_threads*sizeof(thread_pool_handle));
    }

    splitbase->num_threads = num_threads;
    splitbase->A = A;
    splitbase->B = B;
    splitbase->ctx = ctx;
    splitbase->p = UWORD(1) << (FLINT_BITS - 2);

    pthread_mutex_init(&splitbase->mutex, NULL);

    /* initial bound on target modulus */
    fmpz_mpolyu_height(bound, A, ctx);
    fmpz_mpolyu_height(temp, B, ctx);

    if (fmpz_cmp(bound, temp) < 0)
        fmpz_swap(bound, temp);
    fmpz_mul(bound, bound, splitbase->gamma);
    fmpz_add(bound, bound, bound);

    /* no images yet */
    fmpz_one(modulus);

compute_split:

    splitbase->gcd_is_one = 0;
    fmpz_cdiv_q(temp, bound, modulus);
    fmpz_add_ui(temp, temp, 2);

    /*
        n := fmpz_clog_ui(temp, splitbase->p) is the number of images we need.
        We have m := num_threads threads available for work.
        We can calculate images in parallel (parallel on p), but the
        calculation of each image can itself also be done in parallel.

        An integer l := num_master_threads with 1 <= l <= min(n, m) is selected
        and fractions a_i/b_i, 0 <= i < k are also selected with sum_i(a_i) = n
        and sum_i(b_i) = m. Then l jobs are spawned, each doing a_i images
        where each image uses b_i threads.
    */
    _divide_master_threads(&num_master_threads, qvec,
                                fmpz_clog_ui(temp, splitbase->p), num_threads);
    FLINT_ASSERT(num_master_threads > 0);

    k = 0;
    for (i = 0; i < num_master_threads; i++)
    {
        splitargs[i].idx = i;
        splitargs[i].base = splitbase;
        splitargs[i].required_images = fmpz_get_si(fmpq_numref(qvec + i));
        splitargs[i].num_workers = fmpz_get_si(fmpq_denref(qvec + i)) - 1;
        FLINT_ASSERT(splitargs[i].num_workers >= 0);

        if (i == 0)
        {
            splitargs[i].master_handle = -1;
        }
        else
        {
            splitargs[i].master_handle = handles[k++];
        }
        FLINT_ASSERT(splitargs[i].num_workers <= num_workers);
        for (j = 0; j < splitargs[i].num_workers; j++)
        {
            splitargs[i].worker_handles[j] = handles[k++];
        }
    }
    /* all handles should have been distributed */
    FLINT_ASSERT(k == num_workers);

    for (i = 1; i < num_master_threads; i++)
    {
        thread_pool_wake(global_thread_pool, splitargs[i].master_handle,
                                                 _splitworker, &splitargs[i]);
    }
    _splitworker(&splitargs[0]);
    for (i = 1; i < num_master_threads; i++)
    {
        thread_pool_wait(global_thread_pool, splitargs[i].master_handle);
    }

    if (splitbase->gcd_is_one)
    {
        fmpz_mpolyu_one(G, ctx);
        fmpz_mpolyu_swap(Abar, A, ctx);
        fmpz_mpolyu_swap(Bbar, B, ctx);
        goto successful_put_content;
    }

    /* check each thread reached its goal */
    for (i = 0; i < num_master_threads; i++)
    {
        if (splitargs[i].image_count < splitargs[i].required_images)
        {
            /* ran out of rational primes - must fail */
            success = 0;
            goto cleanup_split;
        }
    }

    /* find images to join */
    num_images = 0;
    if (!fmpz_is_one(modulus))
    {
        gptrs[num_images] = G;
        abarptrs[num_images] = Abar;
        bbarptrs[num_images] = Bbar;
        mptrs[num_images] = modulus;
        num_images++;
    }

    i = 0;
    if (num_images == 0)
    {
        gptrs[num_images] = splitargs[i].G;
        abarptrs[num_images] = splitargs[i].Abar;
        bbarptrs[num_images] = splitargs[i].Bbar;
        mptrs[num_images] = splitargs[i].modulus;
        num_images++;
        i++;
    }

    FLINT_ASSERT(num_images <= num_master_threads + 1);

    for (; i < num_master_threads; i++)
    {
        int cmp = 0;
        if (gptrs[0]->exps[0] != splitargs[i].G->exps[0])
        {
            cmp = gptrs[0]->exps[0] > splitargs[i].G->exps[0] ? 1 : -1;
        }
        if (cmp == 0)
        {
            cmp = mpoly_monomial_cmp_nomask(
                             (gptrs[0]->coeffs + 0)->exps + N*0,
                       (splitargs[i].G->coeffs + 0)->exps + N*0, N);
        }

        if (cmp < 0)
        {
            /* splitarg[i] was unlucky - ignore it */
        }
        else
        {
            if (cmp > 0)
            {
                /* splitarg[0], ..., splitarg[i - 1] were unlucky */
                num_images = 0;
            }
            gptrs[num_images] = splitargs[i].G;
            abarptrs[num_images] = splitargs[i].Abar;
            bbarptrs[num_images] = splitargs[i].Bbar;
            mptrs[num_images] = splitargs[i].modulus;
            num_images++;
        }
        FLINT_ASSERT(num_images <= num_master_threads + 1);
    }

    /* now must join ptrs[0], ..., ptrs[num_images-1] where num_images > 0 */
    fmpz_crt_init(P);
    success = fmpz_crt_precompute_p(P, (const fmpz * const *) mptrs, num_images);
    FLINT_ASSERT(success);

    joinbase->num_images = num_images;
    joinbase->gptrs = gptrs;
    joinbase->abarptrs = abarptrs;
    joinbase->bbarptrs = bbarptrs;
    joinbase->G_exp = gptrs[0]->exps[0];
    joinbase->Abar_exp = abarptrs[0]->exps[0];
    joinbase->Bbar_exp = bbarptrs[0]->exps[0];
    joinbase->ctx = ctx;
    joinbase->CRT = P;
    pthread_mutex_init(&joinbase->mutex, NULL);

    joinargs = (_joinworker_arg_struct *) flint_malloc(
                                   num_threads*sizeof(_joinworker_arg_struct));
    for (i = 0; i < num_threads; i++)
    {
        joinargs[i].base = joinbase;
        fmpz_mpolyu_init(joinargs[i].G, bits, ctx);
        fmpz_mpolyu_init(joinargs[i].Abar, bits, ctx);
        fmpz_mpolyu_init(joinargs[i].Bbar, bits, ctx);
        fmpz_init(joinargs[i].Gmax);
        fmpz_init(joinargs[i].Gsum);
        fmpz_init(joinargs[i].Abarmax);
        fmpz_init(joinargs[i].Abarsum);
        fmpz_init(joinargs[i].Bbarmax);
        fmpz_init(joinargs[i].Bbarsum);
    }

    for (i = 0; i + 1 < num_threads; i++)
    {
        thread_pool_wake(global_thread_pool, handles[i],
                                                    _joinworker, joinargs + i);
    }
    _joinworker(joinargs + num_threads - 1);
    for (i = 0; i + 1 < num_threads; i++)
    {
        thread_pool_wait(global_thread_pool, handles[i]);
    }
    pthread_mutex_destroy(&joinbase->mutex);

    /* reuse gptrs, abarpts, bbarpts for final trivial join */
    for (i = 0; i < num_threads; i++)
    {
        gptrs[i] = joinargs[i].G;
        abarptrs[i] = joinargs[i].Abar;
        bbarptrs[i] = joinargs[i].Bbar;
    }
    _final_join(G, gptrs, num_threads, ctx);
    _final_join(Abar, abarptrs, num_threads, ctx);
    _final_join(Bbar, bbarptrs, num_threads, ctx);

    /* update modulus - modulus could be one of the mptrs */
    fmpz_one(temp);
    for (i = 0; i < num_images; i++)
    {
        fmpz_mul(temp, temp, mptrs[i]);
    }
    fmpz_swap(modulus, temp);

    /* calculate heights */
    fmpz_zero(gnm);
    fmpz_zero(gns);
    fmpz_zero(anm);
    fmpz_zero(ans);
    fmpz_zero(bnm);
    fmpz_zero(bns);
    for (i = 0; i < num_threads; i++)
    {
        fmpz_add(gns, gns, joinargs[i].Gsum);
        fmpz_add(ans, ans, joinargs[i].Abarsum);
        fmpz_add(bns, bns, joinargs[i].Bbarsum);
        if (fmpz_cmp(gnm, joinargs[i].Gmax) < 0)
            fmpz_set(gnm, joinargs[i].Gmax);
        if (fmpz_cmp(anm, joinargs[i].Abarmax) < 0)
            fmpz_set(anm, joinargs[i].Abarmax);
        if (fmpz_cmp(bnm, joinargs[i].Bbarmax) < 0)
            fmpz_set(bnm, joinargs[i].Bbarmax);
    }

    /* free join data */
    fmpz_crt_clear(P);
    for (i = 0; i < num_threads; i++)
    {
        fmpz_clear(joinargs[i].Gmax);
        fmpz_clear(joinargs[i].Gsum);
        fmpz_clear(joinargs[i].Abarmax);
        fmpz_clear(joinargs[i].Abarsum);
        fmpz_clear(joinargs[i].Bbarmax);
        fmpz_clear(joinargs[i].Bbarsum);
        fmpz_mpolyu_clear(joinargs[i].G, ctx);
        fmpz_mpolyu_clear(joinargs[i].Abar, ctx);
        fmpz_mpolyu_clear(joinargs[i].Bbar, ctx);
    }
    flint_free(joinargs);

    /* only try divisibility check once modulus exceeds heuristic bound */
    if (fmpz_cmp(modulus, bound) <= 0)
    {
        goto compute_split;
    }

    /* divisibility check */
    fmpz_mul(ans, ans, gnm);
    fmpz_mul(anm, anm, gns);
    fmpz_mul(bns, bns, gnm);
    fmpz_mul(bnm, bnm, gns);
    if (fmpz_cmp(ans, anm) > 0)
        fmpz_swap(ans, anm);
    if (fmpz_cmp(bns, bnm) > 0)
        fmpz_swap(bns, bnm);
    fmpz_add(ans, ans, ans);
    fmpz_add(bns, bns, bns);
    if (fmpz_cmp(ans, modulus) < 0 && fmpz_cmp(bns, modulus) < 0)
    {
        goto successful;
    }

    /* divisiblity check failed - increase bound and try more */
    fmpz_mul_2exp(bound, modulus, 2*FLINT_BITS);
    goto compute_split;

successful:

    FLINT_ASSERT(fmpz_equal(splitbase->gamma, fmpz_mpolyu_leadcoeff(G)));

    fmpz_mpolyu_content_fmpz(temp, G, ctx);
    fmpz_mpolyu_divexact_fmpz(G, G, temp, ctx);
    fmpz_mpolyu_divexact_fmpz(Abar, Abar, fmpz_mpolyu_leadcoeff(G), ctx);
    fmpz_mpolyu_divexact_fmpz(Bbar, Bbar, fmpz_mpolyu_leadcoeff(G), ctx);

successful_put_content:

    fmpz_mpolyu_mul_fmpz(G, G, cG, ctx);
    fmpz_mpolyu_mul_fmpz(Abar, Abar, cAbar, ctx);
    fmpz_mpolyu_mul_fmpz(Bbar, Bbar, cBbar, ctx);

    success = 1;

cleanup_split:

    pthread_mutex_destroy(&splitbase->mutex);
    fmpz_clear(splitbase->gamma);

    for (i = 0; i < num_threads; i++)
    {
        fmpz_mpolyu_clear(splitargs[i].G, ctx);
        fmpz_mpolyu_clear(splitargs[i].Abar, ctx);
        fmpz_mpolyu_clear(splitargs[i].Bbar, ctx);
        fmpz_clear(splitargs[i].modulus);
        flint_free(splitargs[i].worker_handles);
    }

    flint_free(gptrs);
    flint_free(abarptrs);
    flint_free(bbarptrs);
    flint_free(mptrs);
    flint_free(splitargs);

    _fmpq_vec_clear(qvec, num_threads);

    fmpz_clear(cA);
    fmpz_clear(cB);
    fmpz_clear(cG);
    fmpz_clear(cAbar);
    fmpz_clear(cBbar);

    fmpz_clear(gnm);
    fmpz_clear(gns);
    fmpz_clear(anm);
    fmpz_clear(ans);
    fmpz_clear(bnm);
    fmpz_clear(bns);
    fmpz_clear(bound);
    fmpz_clear(temp);
    fmpz_clear(modulus);

    return success;
}


int fmpz_mpoly_gcd_brown_threaded(
    fmpz_mpoly_t G,
    const fmpz_mpoly_t A,
    const fmpz_mpoly_t B,
    const fmpz_mpoly_ctx_t ctx,
    slong thread_limit)
{
    int success;
    slong * perm;
    ulong * shift, * stride;
    slong i;
    mp_bitcnt_t new_bits;
    fmpz_mpoly_ctx_t uctx;
    fmpz_mpolyu_t Au, Bu, Gu, Abaru, Bbaru;

    if (fmpz_mpoly_is_zero(A, ctx))
    {
        if (fmpz_mpoly_is_zero(B, ctx))
        {
            fmpz_mpoly_zero(G, ctx);
            return 1;
        }
        if (fmpz_sgn(B->coeffs + 0) < 0)
        {
            fmpz_mpoly_neg(G, B, ctx);
            return 1;
        }
        else
        {
            fmpz_mpoly_set(G, B, ctx);
            return 1;
        }
    }

    if (fmpz_mpoly_is_zero(B, ctx))
    {
        if (fmpz_sgn(A->coeffs + 0) < 0)
        {
            fmpz_mpoly_neg(G, A, ctx);
            return 1;
        }
        else
        {
            fmpz_mpoly_set(G, A, ctx);
            return 1;
        }
    }

    if (A->bits > FLINT_BITS || B->bits > FLINT_BITS)
    {
        return 0;
    }

    perm = (slong *) flint_malloc(ctx->minfo->nvars*sizeof(slong));
    shift = (ulong *) flint_malloc(ctx->minfo->nvars*sizeof(ulong));
    stride = (ulong *) flint_malloc(ctx->minfo->nvars*sizeof(ulong));
    for (i = 0; i < ctx->minfo->nvars; i++)
    {
        perm[i] = i + 1 < ctx->minfo->nvars ? i + 1 : 0;
        shift[i] = 0;
        stride[i] = 1;
    }

    if (ctx->minfo->nvars == 1)
    {
        fmpz_poly_t a, b, g;
        fmpz_poly_init(a);
        fmpz_poly_init(b);
        fmpz_poly_init(g);
        _fmpz_mpoly_to_fmpz_poly_deflate(a, A, 0, shift, stride, ctx);
        _fmpz_mpoly_to_fmpz_poly_deflate(b, B, 0, shift, stride, ctx);
        fmpz_poly_gcd(g, a, b);
        _fmpz_mpoly_from_fmpz_poly_inflate(G, A->bits, g, 0, shift, stride, ctx);
        fmpz_poly_clear(a);
        fmpz_poly_clear(b);
        fmpz_poly_clear(g);
        success = 1;
        goto cleanup1;
    }

    new_bits = FLINT_MAX(A->bits, B->bits);

    fmpz_mpoly_ctx_init(uctx, ctx->minfo->nvars - 1, ORD_LEX);
    fmpz_mpolyu_init(Au, new_bits, uctx);
    fmpz_mpolyu_init(Bu, new_bits, uctx);
    fmpz_mpolyu_init(Gu, new_bits, uctx);
    fmpz_mpolyu_init(Abaru, new_bits, uctx);
    fmpz_mpolyu_init(Bbaru, new_bits, uctx);

    fmpz_mpoly_to_mpolyu_perm_deflate(Au, A, perm, shift, stride, uctx, ctx);
    fmpz_mpoly_to_mpolyu_perm_deflate(Bu, B, perm, shift, stride, uctx, ctx);

    /* calculate gcd */
    {
        thread_pool_handle * handles;
        slong i, max_num_workers, num_workers;

        handles = NULL;
        num_workers = 0;
        if (global_thread_pool_initialized)
        {
            max_num_workers = thread_pool_get_size(global_thread_pool);
            max_num_workers = FLINT_MIN(thread_limit - 1, max_num_workers);
            if (max_num_workers > 0)
            {
                handles = (thread_pool_handle *) flint_malloc(
                                   max_num_workers*sizeof(thread_pool_handle));
                num_workers = thread_pool_request(global_thread_pool,
                                                     handles, max_num_workers);
            }
        }

        success = fmpz_mpolyu_gcd_brown_threaded(Gu, Abaru, Bbaru, Au, Bu,
                                                   uctx, handles, num_workers);

        for (i = 0; i < num_workers; i++)
        {
            thread_pool_give_back(global_thread_pool, handles[i]);
        }

        if (handles)
            flint_free(handles);
    }

    if (success)
    {
        fmpz_mpoly_from_mpolyu_perm_inflate(G, new_bits,
                                           Gu, perm, shift, stride, uctx, ctx);
        if (fmpz_sgn(G->coeffs + 0) < 0)
            fmpz_mpoly_neg(G, G, ctx);
    }

    fmpz_mpolyu_clear(Au, uctx);
    fmpz_mpolyu_clear(Bu, uctx);
    fmpz_mpolyu_clear(Gu, uctx);
    fmpz_mpolyu_clear(Abaru, uctx);
    fmpz_mpolyu_clear(Bbaru, uctx);
    fmpz_mpoly_ctx_clear(uctx);

cleanup1:

    flint_free(perm);
    flint_free(shift);
    flint_free(stride);

    return success;
}
