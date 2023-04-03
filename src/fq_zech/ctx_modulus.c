/*
    Copyright (C) 2013 Mike Hansen

    This file is part of FLINT.

    FLINT is free software: you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License (LGPL) as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.  See <https://www.gnu.org/licenses/>.
*/

#include "fq_nmod.h"
#include "fq_zech.h"

const nmod_poly_struct * fq_zech_ctx_modulus(const fq_zech_ctx_t ctx)
{
    return fq_nmod_ctx_modulus(ctx->fq_nmod_ctx);
}
