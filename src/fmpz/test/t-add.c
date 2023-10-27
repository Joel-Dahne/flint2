/*
    Copyright (C) 2009 William Hart

    This file is part of FLINT.

    FLINT is free software: you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License (LGPL) as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.  See <https://www.gnu.org/licenses/>.
*/

#include "flint.h"
#include "ulong_extras.h"
#include "fmpz.h"

int
main(void)
{
    int i, result;
    FLINT_TEST_INIT(state);

    flint_printf("add....");
    fflush(stdout);

    for (i = 0; i < 10000 * flint_test_multiplier(); i++)
    {
        fmpz_t a, b, c;
        mpz_t d, e, f, g;
        int aliasing;

        fmpz_init(a);
        fmpz_init(b);
        fmpz_init(c);

        mpz_init(d);
        mpz_init(e);
        mpz_init(f);
        mpz_init(g);

        fmpz_randtest(a, state, 200);
        fmpz_randtest(b, state, 200);

        fmpz_get_mpz(d, a);
        fmpz_get_mpz(e, b);

        aliasing = n_randint(state, 4);

        if (aliasing == 0)
        {
            fmpz_add(c, a, b);
        }
        else if (aliasing == 1)
        {
            fmpz_set(a, b);
            mpz_set(d, e);
            fmpz_add(c, a, a);
        }
        else if (aliasing == 2)
        {
            fmpz_set(c, a);
            fmpz_add(c, c, b);
        }
        else
        {
            fmpz_set(c, b);
            fmpz_add(c, a, c);
        }

        mpz_add(f, d, e);

        fmpz_get_mpz(g, c);

        result = (mpz_cmp(f, g) == 0) && _fmpz_is_canonical(c);
        if (!result)
        {
            flint_printf("FAIL:\n");
            gmp_printf("d = %Zd, e = %Zd, f = %Zd, g = %Zd\n", d, e, f, g);
            fflush(stdout);
            flint_abort();
        }

        fmpz_clear(a);
        fmpz_clear(b);
        fmpz_clear(c);

        mpz_clear(d);
        mpz_clear(e);
        mpz_clear(f);
        mpz_clear(g);
    }

    FLINT_TEST_CLEANUP(state);

    flint_printf("PASS\n");
    return 0;
}
