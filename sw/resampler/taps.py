#!/usr/bin/env python3

# SPDX-FileCopyrightText: Korneliusz Osmenda <korneliuszo@gmail.com>
# SPDX-License-Identifier: MIT

import sympy
import numpy as np
import math
import more_itertools

x = sympy.symbols('x')
flts = sympy.cos(sympy.pi*x/12)**2*sympy.sin(sympy.pi*x/2)/(sympy.pi*x)

fltw0 =  sympy.Piecewise((flts,((x>=-6)&(x<=6))),(0,True))
fltw1 = fltw0.diff()/sympy.factorial(1)
fltw2 = fltw0.diff().diff()/sympy.factorial(2)
fltw3 = fltw0.diff().diff().diff()/sympy.factorial(3)

if True:
    f0 = sympy.lambdify(x, fltw0, "numpy")
    flt0 = [q for q in np.nan_to_num(f0(np.array(range(-6,7))),nan=float(fltw0.limit(x,0)))]
    f1 = sympy.lambdify(x, fltw1, "numpy")
    flt1 = [q for q in np.nan_to_num(f1(np.array(range(-6,7))),nan=float(fltw1.limit(x,0)))]
    f2 = sympy.lambdify(x, fltw2, "numpy")
    flt2 = [q for q in np.nan_to_num(f2(np.array(range(-6,7))),nan=float(fltw2.limit(x,0)))]
    f3 = sympy.lambdify(x, fltw3, "numpy")
    flt3 = [q for q in np.nan_to_num(f3(np.array(range(-6,7))),nan=float(fltw3.limit(x,0)))]

import sys

print(sum((abs(_) for _ in flt0)))
print(sum((abs(_) for _ in flt1)))
print(sum((abs(_) for _ in flt2)))
print(sum((abs(_) for _ in flt3)))

maxsignal = \
(sum((abs(sum(_)) for _ in zip(flt0, \
    list(_/2 for _ in flt1), \
    list(_/4 for _ in flt2), \
    list(_/8 for _ in flt3) \
    ))))

print(maxsignal)

assert(sum(flt0)<2)
assert(sum(flt1)<2)
assert(sum(flt2)<1)
assert(sum(flt3)<1)
assert(maxsignal<2)

f=open(sys.argv[1],'w')

f.write(f"""
#pragma once
#include <array>

static constexpr std::array<int32_t,13*4> fir_coeff = 
{{
    {", ".join((str(int(q*(1<<15))) for q in more_itertools.interleave(flt0[::-1],flt1[::-1],flt2[::-1],flt3[::-1])))}
}};

""")
