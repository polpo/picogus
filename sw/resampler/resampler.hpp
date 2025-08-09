#pragma once

/*
 * SPDX-FileCopyrightText: Korneliusz Osmenda <korneliuszo@gmail.com>
 *
 * SPDX-License-Identifier: MIT
 */


#include <taps.hpp>
#include <stdint.h>
#include <cmath>


template<int16_t (*IN_FN)()>
class Resampler {
	int64_t phase; //in 31.32
	uint64_t ratio;
	std::size_t fir_pos;
	int16_t fir[13];
	int32_t c0,c1,c2,c3; //in 0.15
public:
	void set_ratio(uint32_t in, uint32_t out)
	{
		uint64_t val = ((uint64_t)in)<<32;
		ratio = val/out;
	}
	int16_t get_sample()
	{
		phase+=ratio;
		while(phase>=1UL<<31) //0.5
		{
			fir[fir_pos]=IN_FN(); //0.15
			fir_pos++;
			phase-=1ULL<<32;
			if(fir_pos>=13)
				fir_pos=0;

			const int32_t* lfir = fir_coeff.data();
			int16_t* inp = &fir[fir_pos];
			int16_t* inp2 = &fir[0];
			int16_t* fir_split = &fir[13];
			int16_t* fir_split2 = &fir[fir_pos];

			uint32_t TMP,TMP2;
			int32_t lc0,lc1,lc2,lc3; //in 0.15
			lc0=0;
			lc1=0;
			lc2=0;
			lc3=0;
			asm (
				"1:\n"
				"ldrh %[TMP2], [%[INP],#0]\n"
				"SXTH %[TMP2],%[TMP2]\n"
				"ldr %[TMP], [%[FIR],#0]\n"
				"mul %[TMP], %[TMP2], %[TMP]\n"
				"add %[C0], %[C0], %[TMP]\n"
				"ldr %[TMP], [%[FIR],#4]\n"
				"mul %[TMP], %[TMP2], %[TMP]\n"
				"add %[C1], %[C1], %[TMP]\n"
				"ldr %[TMP], [%[FIR],#8]\n"
				"mul %[TMP], %[TMP2], %[TMP]\n"
				"add %[C2], %[C2], %[TMP]\n"
				"ldr %[TMP], [%[FIR],#12]\n"
				"mul %[TMP], %[TMP2], %[TMP]\n"
				"add %[C3], %[C3], %[TMP]\n"
				"add %[FIR],%[FIR],#16\n"
				"add %[INP],%[INP],#2\n"
				"cmp %[INP],%[FIR_SPLIT]\n"
				"bne 1b\n"
				"mov %[INP],%[INP2]\n"
				"2:\n"
				"cmp %[INP],%[FIR_SPLIT2]\n"
				"beq 3f\n"
				"ldrh %[TMP2], [%[INP],#0]\n"
				"SXTH %[TMP2],%[TMP2]\n"
				"ldr %[TMP], [%[FIR],#0]\n"
				"mul %[TMP], %[TMP2], %[TMP]\n"
				"add %[C0], %[C0], %[TMP]\n"
				"ldr %[TMP], [%[FIR],#4]\n"
				"mul %[TMP], %[TMP2], %[TMP]\n"
				"add %[C1], %[C1], %[TMP]\n"
				"ldr %[TMP], [%[FIR],#8]\n"
				"mul %[TMP], %[TMP2], %[TMP]\n"
				"add %[C2], %[C2], %[TMP]\n"
				"ldr %[TMP], [%[FIR],#12]\n"
				"mul %[TMP], %[TMP2], %[TMP]\n"
				"add %[C3], %[C3], %[TMP]\n"
				"add %[FIR],%[FIR],#16\n"
				"add %[INP],%[INP],#2\n"
				"b 2b\n"
				"3:\n"

			: [INP]"+l"(inp)
			, [FIR]"+l"(lfir)
			, [TMP]"+l"(TMP)
			, [TMP2]"+l"(TMP2)
			, [C0]"+r"(lc0)
			, [C1]"+r"(lc1)
			, [C2]"+r"(lc2)
			, [C3]"+r"(lc3)
			, [FIR_SPLIT]"+r"(fir_split)
			, [FIR_SPLIT2]"+r"(fir_split2)
			, [INP2]"+r"(inp2)
			);
			c0=lc0; // maxsignal 2.30 tap 2.
			c1=lc1>>(30-14); // tap 2.30 -> 2.14
			c2=lc2>>15; // tap 1.30 -> 1.15
			c3=lc3>>15; // tap 1.30 -> 1.15
		};
		int32_t lphase = phase>>16; //-1.16
		int32_t val = (c0 +
				((lphase*(c1 +
						((lphase*(c2 +
								((lphase*(c3))>>(16+15-15)) //1.15
								))>>(16+15-14)) //2.14
								))>>(16+14-30)) //2.30
								);
		return val>>16;
	}
};
